// SPDX-License-Identifier: Apache-2.0
// The dynamic AABB tree (aabb_tree.h; docs/physics.md "Broadphase").

#include <nepheline/core/assert.h>
#include <nepheline/physics/aabb_tree.h>

#include <float.h>
#include <string.h>

#define NULL_NODE NPH_AABB_TREE_NULL

enum {
    NODE_ALLOCATED = 1U << 0U,
    NODE_ENLARGED = 1U << 1U,
    NODE_REBUILT = 1U << 2U, // an inner node a rebuild just made, until refitted
};

#define REBUILD_BINS 16U

static f32 box_area(NphAABB b)
{
    const NphVec3 d = nph_vec3_sub(b.max, b.min);
    return 2.0F * (d.x * d.y + d.y * d.z + d.z * d.x);
}

static NphAABB box_union(NphAABB a, NphAABB b)
{
    return nph_aabb(nph_vec3_min(a.min, b.min), nph_vec3_max(a.max, b.max));
}

static bool box_contains(NphAABB outer, NphAABB inner)
{
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           inner.max.x <= outer.max.x && inner.max.y <= outer.max.y && inner.max.z <= outer.max.z;
}

static bool box_overlaps(NphAABB a, NphAABB b)
{
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y &&
           a.min.z <= b.max.z && b.min.z <= a.max.z;
}

static bool is_leaf(const NphAabbTreeNode *n)
{
    return n->height == 0U;
}

static void thread_free_list(NphAabbTree *t, u32 from)
{
    for (u32 i = from; i < t->node_capacity; ++i) {
        NphAabbTreeNode *n = &t->nodes[i];
        memset(n, 0, sizeof *n);
        n->parent = i + 1U < t->node_capacity ? i + 1U : NULL_NODE;
    }
    t->free_list = from < t->node_capacity ? from : NULL_NODE;
}

NphResult nph_aabb_tree_init(NphAabbTree *tree, NphAllocator *alloc, u32 initial_capacity)
{
    if (tree == NULL || alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    memset(tree, 0, sizeof *tree);
    tree->alloc = alloc;
    tree->root = NULL_NODE;
    tree->node_capacity = initial_capacity == 0U ? 16U : initial_capacity;
    tree->nodes = NPH_ALLOC_ARRAY(alloc, NphAabbTreeNode, tree->node_capacity);
    if (tree->nodes == NULL) {
        tree->node_capacity = 0U;
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    thread_free_list(tree, 0U);
    return NPH_RESULT_OK;
}

static void free_rebuild_scratch(NphAabbTree *t)
{
    if (t->rebuild_capacity > 0U) {
        NPH_FREE_ARRAY(t->alloc, u32, t->leaf_indices, t->rebuild_capacity);
        NPH_FREE_ARRAY(t->alloc, NphAABB, t->leaf_boxes, t->rebuild_capacity);
        NPH_FREE_ARRAY(t->alloc, NphVec3, t->leaf_centers, t->rebuild_capacity);
        NPH_FREE_ARRAY(t->alloc, u32, t->bins_scratch, 3ULL * t->rebuild_capacity);
    }
    t->leaf_indices = NULL;
    t->leaf_boxes = NULL;
    t->leaf_centers = NULL;
    t->bins_scratch = NULL;
    t->rebuild_capacity = 0U;
}

void nph_aabb_tree_free(NphAabbTree *tree)
{
    if (tree == NULL || tree->alloc == NULL) {
        return;
    }
    if (tree->nodes != NULL) {
        NPH_FREE_ARRAY(tree->alloc, NphAabbTreeNode, tree->nodes, tree->node_capacity);
    }
    free_rebuild_scratch(tree);
    memset(tree, 0, sizeof *tree);
    tree->root = NULL_NODE;
}

static u32 alloc_node(NphAabbTree *t)
{
    if (t->free_list == NULL_NODE) {
        const u32 old = t->node_capacity;
        const u32 cap = old * 2U;
        NphAabbTreeNode *grown = (NphAabbTreeNode *)NPH_REALLOC(
            t->alloc, t->nodes, (u64)old * sizeof(NphAabbTreeNode),
            (u64)cap * sizeof(NphAabbTreeNode), NPH_ALIGNOF(NphAabbTreeNode));
        if (grown == NULL) {
            return NULL_NODE;
        }
        t->nodes = grown;
        t->node_capacity = cap;
        thread_free_list(t, old);
    }
    const u32 i = t->free_list;
    NphAabbTreeNode *n = &t->nodes[i];
    t->free_list = n->parent;
    memset(n, 0, sizeof *n);
    n->parent = NULL_NODE;
    n->child1 = NULL_NODE;
    n->child2 = NULL_NODE;
    n->flags = NODE_ALLOCATED;
    t->node_count += 1U;
    return i;
}

static void free_node(NphAabbTree *t, u32 i)
{
    NphAabbTreeNode *n = &t->nodes[i];
    memset(n, 0, sizeof *n);
    n->parent = t->free_list;
    t->free_list = i;
    t->node_count -= 1U;
}

// Recomputes an inner node from its children.
static void refit(NphAabbTree *t, u32 i)
{
    NphAabbTreeNode *n = &t->nodes[i];
    const NphAabbTreeNode *c1 = &t->nodes[n->child1];
    const NphAabbTreeNode *c2 = &t->nodes[n->child2];
    n->aabb = box_union(c1->aabb, c2->aabb);
    n->category_bits = c1->category_bits | c2->category_bits;
    n->height = (u16)(1U + nph_max_u32(c1->height, c2->height));
}

// The sibling whose pairing with a new leaf adds the least area to the
// tree. Each descent's lower bound is what the ancestors already grew by
// plus the child's own minimum growth; the walk stops when no subtree can
// beat the best found (Bittner et al. 2012; Catto, GDC 2019).
static u32 find_best_sibling(const NphAabbTree *t, NphAABB box)
{
    const NphVec3 center = nph_aabb_center(box);
    const f32 area_d = box_area(box);
    u32 index = t->root;
    NphAABB root_box = t->nodes[index].aabb;
    f32 area_base = box_area(root_box);
    f32 direct_cost = box_area(box_union(root_box, box));
    f32 inherited = 0.0F;
    u32 best = index;
    f32 best_cost = direct_cost;
    while (!is_leaf(&t->nodes[index])) {
        const u32 c1 = t->nodes[index].child1;
        const u32 c2 = t->nodes[index].child2;
        const f32 cost = direct_cost + inherited;
        if (cost < best_cost) {
            best_cost = cost;
            best = index;
        }
        inherited += direct_cost - area_base;
        const bool leaf1 = is_leaf(&t->nodes[c1]);
        const bool leaf2 = is_leaf(&t->nodes[c2]);
        const NphAABB b1 = t->nodes[c1].aabb;
        const NphAABB b2 = t->nodes[c2].aabb;
        const f32 direct1 = box_area(box_union(b1, box));
        const f32 direct2 = box_area(box_union(b2, box));
        f32 lower1 = FLT_MAX;
        f32 lower2 = FLT_MAX;
        f32 area1 = 0.0F;
        f32 area2 = 0.0F;
        if (leaf1) {
            const f32 c = direct1 + inherited;
            if (c < best_cost) {
                best_cost = c;
                best = c1;
            }
        } else {
            area1 = box_area(b1);
            lower1 = inherited + direct1 + nph_f32_min(area_d - area1, 0.0F);
        }
        if (leaf2) {
            const f32 c = direct2 + inherited;
            if (c < best_cost) {
                best_cost = c;
                best = c2;
            }
        } else {
            area2 = box_area(b2);
            lower2 = inherited + direct2 + nph_f32_min(area_d - area2, 0.0F);
        }
        if (leaf1 && leaf2) {
            break;
        }
        if (best_cost <= lower1 && best_cost <= lower2) {
            break;
        }
        if (lower1 == lower2 && !leaf1) {
            // Equal bounds: break the tie by closeness, which keeps the
            // descent deterministic and the tree spatially coherent.
            lower1 = nph_vec3_distance_sq(nph_aabb_center(b1), center);
            lower2 = nph_vec3_distance_sq(nph_aabb_center(b2), center);
        }
        if (lower1 < lower2 && !leaf1) {
            index = c1;
            area_base = area1;
            direct_cost = direct1;
        } else {
            index = c2;
            area_base = area2;
            direct_cost = direct2;
        }
    }
    return best;
}

// A rotation at node a swaps one of its children with a grandchild on the
// other side when that shrinks the summed area (Catto, GDC 2019). The
// subtree's leaves do not change, so neither does a's box.
static void rotate(NphAabbTree *t, u32 ia)
{
    NphAabbTreeNode *nodes = t->nodes;
    const u32 ib = nodes[ia].child1;
    const u32 ic = nodes[ia].child2;
    const bool b_leaf = is_leaf(&nodes[ib]);
    const bool c_leaf = is_leaf(&nodes[ic]);
    if (b_leaf && c_leaf) {
        return;
    }
    // The area change of each rotation (negative is better); a rotation
    // that does not apply keeps zero.
    enum {
        ROT_NONE,
        ROT_BF,
        ROT_BG,
        ROT_CD,
        ROT_CE
    } best = ROT_NONE;
    f32 cost[5] = {0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
    if (!c_leaf) {
        const u32 iff = nodes[ic].child1;
        const u32 ig = nodes[ic].child2;
        const f32 base = box_area(nodes[ic].aabb);
        // b <-> f leaves c = (b, g); b <-> g leaves c = (f, b).
        cost[ROT_BF] = box_area(box_union(nodes[ib].aabb, nodes[ig].aabb)) - base;
        cost[ROT_BG] = box_area(box_union(nodes[ib].aabb, nodes[iff].aabb)) - base;
    }
    if (!b_leaf) {
        const u32 id = nodes[ib].child1;
        const u32 ie = nodes[ib].child2;
        const f32 base = box_area(nodes[ib].aabb);
        cost[ROT_CD] = box_area(box_union(nodes[ic].aabb, nodes[ie].aabb)) - base;
        cost[ROT_CE] = box_area(box_union(nodes[ic].aabb, nodes[id].aabb)) - base;
    }
    for (u32 k = ROT_BF; k <= ROT_CE; ++k) {
        if (cost[k] < cost[best]) {
            best = k == ROT_BF ? ROT_BF : (k == ROT_BG ? ROT_BG : (k == ROT_CD ? ROT_CD : ROT_CE));
        }
    }
    switch (best) {
    case ROT_BF: {
        const u32 iff = nodes[ic].child1;
        nodes[ia].child1 = iff;
        nodes[ic].child1 = ib;
        nodes[ib].parent = ic;
        nodes[iff].parent = ia;
        refit(t, ic);
        break;
    }
    case ROT_BG: {
        const u32 ig = nodes[ic].child2;
        nodes[ia].child1 = ig;
        nodes[ic].child2 = ib;
        nodes[ib].parent = ic;
        nodes[ig].parent = ia;
        refit(t, ic);
        break;
    }
    case ROT_CD: {
        const u32 id = nodes[ib].child1;
        nodes[ia].child2 = id;
        nodes[ib].child1 = ic;
        nodes[ic].parent = ib;
        nodes[id].parent = ia;
        refit(t, ib);
        break;
    }
    case ROT_CE: {
        const u32 ie = nodes[ib].child2;
        nodes[ia].child2 = ie;
        nodes[ib].child2 = ic;
        nodes[ic].parent = ib;
        nodes[ie].parent = ia;
        refit(t, ib);
        break;
    }
    case ROT_NONE:
    default:
        return;
    }
    refit(t, ia);
}

static bool insert_leaf(NphAabbTree *t, u32 leaf)
{
    if (t->root == NULL_NODE) {
        t->root = leaf;
        t->nodes[leaf].parent = NULL_NODE;
        return true;
    }
    const u32 sibling = find_best_sibling(t, t->nodes[leaf].aabb);
    const u32 old_parent = t->nodes[sibling].parent;
    const u32 new_parent = alloc_node(t);
    if (new_parent == NULL_NODE) {
        return false;
    }
    NphAabbTreeNode *nodes = t->nodes; // alloc_node may have moved the array
    nodes[new_parent].parent = old_parent;
    nodes[new_parent].child1 = sibling;
    nodes[new_parent].child2 = leaf;
    nodes[sibling].parent = new_parent;
    nodes[leaf].parent = new_parent;
    if (old_parent == NULL_NODE) {
        t->root = new_parent;
    } else if (nodes[old_parent].child1 == sibling) {
        nodes[old_parent].child1 = new_parent;
    } else {
        nodes[old_parent].child2 = new_parent;
    }
    for (u32 i = new_parent; i != NULL_NODE; i = nodes[i].parent) {
        refit(t, i);
        rotate(t, i);
    }
    return true;
}

static void remove_leaf(NphAabbTree *t, u32 leaf)
{
    NphAabbTreeNode *nodes = t->nodes;
    if (leaf == t->root) {
        t->root = NULL_NODE;
        return;
    }
    const u32 parent = nodes[leaf].parent;
    const u32 grand = nodes[parent].parent;
    const u32 sibling = nodes[parent].child1 == leaf ? nodes[parent].child2 : nodes[parent].child1;
    if (grand == NULL_NODE) {
        t->root = sibling;
        nodes[sibling].parent = NULL_NODE;
        free_node(t, parent);
        return;
    }
    if (nodes[grand].child1 == parent) {
        nodes[grand].child1 = sibling;
    } else {
        nodes[grand].child2 = sibling;
    }
    nodes[sibling].parent = grand;
    free_node(t, parent);
    for (u32 i = grand; i != NULL_NODE; i = nodes[i].parent) {
        refit(t, i);
    }
}

static bool box_valid(NphAABB b)
{
    return nph_vec3_is_finite(b.min) && nph_vec3_is_finite(b.max) && b.min.x <= b.max.x &&
           b.min.y <= b.max.y && b.min.z <= b.max.z;
}

NphResult nph_aabb_tree_create_proxy(NphAabbTree *tree, NphAABB aabb, u64 category_bits,
                                     u64 user_data, u32 *out_proxy)
{
    NPH_ASSERT(tree != NULL && out_proxy != NULL);
    if (!box_valid(aabb)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 leaf = alloc_node(tree);
    if (leaf == NULL_NODE) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphAabbTreeNode *n = &tree->nodes[leaf];
    n->aabb = aabb;
    n->category_bits = category_bits;
    n->user_data = user_data;
    n->height = 0U;
    if (!insert_leaf(tree, leaf)) {
        free_node(tree, leaf);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    tree->proxy_count += 1U;
    *out_proxy = leaf;
    return NPH_RESULT_OK;
}

void nph_aabb_tree_destroy_proxy(NphAabbTree *tree, u32 proxy)
{
    NPH_ASSERT(tree != NULL && proxy < tree->node_capacity);
    NPH_ASSERT(is_leaf(&tree->nodes[proxy]) && (tree->nodes[proxy].flags & NODE_ALLOCATED) != 0U);
    remove_leaf(tree, proxy);
    free_node(tree, proxy);
    tree->proxy_count -= 1U;
}

void nph_aabb_tree_move_proxy(NphAabbTree *tree, u32 proxy, NphAABB aabb)
{
    NPH_ASSERT(tree != NULL && proxy < tree->node_capacity && is_leaf(&tree->nodes[proxy]));
    NPH_ASSERT(box_valid(aabb));
    remove_leaf(tree, proxy);
    tree->nodes[proxy].aabb = aabb;
    // Removal freed one inner node and insertion takes one, so this cannot
    // run out of memory.
    const bool ok = insert_leaf(tree, proxy);
    NPH_ASSERT(ok);
    NPH_UNUSED(ok);
}

void nph_aabb_tree_enlarge_proxy(NphAabbTree *tree, u32 proxy, NphAABB aabb)
{
    NPH_ASSERT(tree != NULL && proxy < tree->node_capacity && is_leaf(&tree->nodes[proxy]));
    NPH_ASSERT(box_valid(aabb));
    NphAabbTreeNode *nodes = tree->nodes;
    nodes[proxy].aabb = aabb;
    nodes[proxy].flags = (u16)(nodes[proxy].flags | NODE_ENLARGED);
    u32 i = nodes[proxy].parent;
    while (i != NULL_NODE) {
        const NphAABB grown = box_union(nodes[i].aabb, nodes[proxy].aabb);
        const bool changed = !box_contains(nodes[i].aabb, nodes[proxy].aabb);
        nodes[i].aabb = grown;
        nodes[i].flags = (u16)(nodes[i].flags | NODE_ENLARGED);
        i = nodes[i].parent;
        if (!changed) {
            break;
        }
    }
    // The rest of the way up only needs the mark, and stops at one that has
    // it already.
    while (i != NULL_NODE && (nodes[i].flags & NODE_ENLARGED) == 0U) {
        nodes[i].flags = (u16)(nodes[i].flags | NODE_ENLARGED);
        i = nodes[i].parent;
    }
}

void nph_aabb_tree_set_category_bits(NphAabbTree *tree, u32 proxy, u64 category_bits)
{
    NPH_ASSERT(tree != NULL && proxy < tree->node_capacity && is_leaf(&tree->nodes[proxy]));
    NphAabbTreeNode *nodes = tree->nodes;
    nodes[proxy].category_bits = category_bits;
    for (u32 i = nodes[proxy].parent; i != NULL_NODE; i = nodes[i].parent) {
        nodes[i].category_bits =
            nodes[nodes[i].child1].category_bits | nodes[nodes[i].child2].category_bits;
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

void nph_aabb_tree_query(const NphAabbTree *tree, NphAABB aabb, u64 mask_bits,
                         NphAabbTreeQueryFn fn, void *context)
{
    NPH_ASSERT(tree != NULL && fn != NULL);
    if (tree->root == NULL_NODE) {
        return;
    }
    u32 stack[NPH_AABB_TREE_STACK_SIZE];
    u32 top = 0U;
    stack[top++] = tree->root;
    while (top > 0U) {
        const u32 i = stack[--top];
        const NphAabbTreeNode *n = &tree->nodes[i];
        if ((n->category_bits & mask_bits) == 0U || !box_overlaps(n->aabb, aabb)) {
            continue;
        }
        if (is_leaf(n)) {
            if (!fn(i, n->user_data, context)) {
                return;
            }
        } else {
            NPH_ASSERT_MSG(top + 2U <= NPH_AABB_TREE_STACK_SIZE, "aabb tree query: stack overflow");
            if (top + 2U <= NPH_AABB_TREE_STACK_SIZE) {
                stack[top++] = n->child2;
                stack[top++] = n->child1;
            }
        }
    }
}

void nph_aabb_tree_raycast(const NphAabbTree *tree, const NphAabbTreeRay *ray, u64 mask_bits,
                           NphAabbTreeRayFn fn, void *context)
{
    NPH_ASSERT(tree != NULL && ray != NULL && fn != NULL);
    if (tree->root == NULL_NODE) {
        return;
    }
    NphAabbTreeRay r = *ray;
    const NphRay math_ray = nph_ray(r.origin, r.translation);
    u32 stack[NPH_AABB_TREE_STACK_SIZE];
    u32 top = 0U;
    stack[top++] = tree->root;
    while (top > 0U) {
        const u32 i = stack[--top];
        const NphAabbTreeNode *n = &tree->nodes[i];
        if ((n->category_bits & mask_bits) == 0U ||
            !nph_ray_intersects_aabb(math_ray, n->aabb, r.max_fraction, NULL, NULL)) {
            continue;
        }
        if (is_leaf(n)) {
            const f32 value = fn(&r, i, n->user_data, context);
            if (value < 0.0F) {
                return;
            }
            if (value < r.max_fraction) {
                r.max_fraction = value;
            }
            continue;
        }
        // The nearer child is visited first, so a hit there clips the ray
        // before the farther one is opened.
        f32 t1 = FLT_MAX;
        f32 t2 = FLT_MAX;
        const bool h1 = nph_ray_intersects_aabb(math_ray, tree->nodes[n->child1].aabb,
                                                r.max_fraction, &t1, NULL);
        const bool h2 = nph_ray_intersects_aabb(math_ray, tree->nodes[n->child2].aabb,
                                                r.max_fraction, &t2, NULL);
        NPH_ASSERT_MSG(top + 2U <= NPH_AABB_TREE_STACK_SIZE, "aabb tree raycast: stack overflow");
        if (top + 2U > NPH_AABB_TREE_STACK_SIZE) {
            continue;
        }
        if (h1 && h2) {
            if (t1 <= t2) {
                stack[top++] = n->child2;
                stack[top++] = n->child1;
            } else {
                stack[top++] = n->child1;
                stack[top++] = n->child2;
            }
        } else if (h1) {
            stack[top++] = n->child1;
        } else if (h2) {
            stack[top++] = n->child2;
        }
    }
}

// ---------------------------------------------------------------------------
// Rebuild: top-down binned SAH (Wald 2007)
// ---------------------------------------------------------------------------

static NphResult reserve_rebuild(NphAabbTree *t, u32 count)
{
    if (count <= t->rebuild_capacity) {
        return NPH_RESULT_OK;
    }
    u32 cap = nph_max_u32(count, t->rebuild_capacity * 2U);
    cap = nph_max_u32(cap, 64U);
    u32 *idx = NPH_ALLOC_ARRAY(t->alloc, u32, cap);
    NphAABB *boxes = NPH_ALLOC_ARRAY(t->alloc, NphAABB, cap);
    NphVec3 *centers = NPH_ALLOC_ARRAY(t->alloc, NphVec3, cap);
    u32 *scratch = NPH_ALLOC_ARRAY(t->alloc, u32, 3ULL * cap);
    if (idx == NULL || boxes == NULL || centers == NULL || scratch == NULL) {
        if (idx != NULL) {
            NPH_FREE_ARRAY(t->alloc, u32, idx, cap);
        }
        if (boxes != NULL) {
            NPH_FREE_ARRAY(t->alloc, NphAABB, boxes, cap);
        }
        if (centers != NULL) {
            NPH_FREE_ARRAY(t->alloc, NphVec3, centers, cap);
        }
        if (scratch != NULL) {
            NPH_FREE_ARRAY(t->alloc, u32, scratch, 3ULL * cap);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    free_rebuild_scratch(t);
    t->leaf_indices = idx;
    t->leaf_boxes = boxes;
    t->leaf_centers = centers;
    t->bins_scratch = scratch;
    t->rebuild_capacity = cap;
    return NPH_RESULT_OK;
}

// Partitions items [lo, hi) and returns the split index, both sides
// non-empty.
static u32 partition_sah(NphAabbTree *t, u32 lo, u32 hi)
{
    const u32 count = hi - lo;
    NphAABB cb = nph_aabb_from_point(t->leaf_centers[lo]);
    for (u32 i = lo + 1U; i < hi; ++i) {
        cb.min = nph_vec3_min(cb.min, t->leaf_centers[i]);
        cb.max = nph_vec3_max(cb.max, t->leaf_centers[i]);
    }
    const NphVec3 extent = nph_vec3_sub(cb.max, cb.min);
    const u32 axis = nph_vec3_max_index(extent);
    const f32 axis_min = nph_vec3_at(cb.min, axis);
    const f32 axis_extent = nph_vec3_at(extent, axis);
    u32 mid = lo + count / 2U;
    if (axis_extent > 0.0F && count > 2U) {
        u32 bin_count[REBUILD_BINS] = {0};
        NphAABB bin_box[REBUILD_BINS];
        for (u32 b = 0U; b < REBUILD_BINS; ++b) {
            bin_box[b] = nph_aabb_empty();
        }
        const f32 scale = (f32)REBUILD_BINS / axis_extent;
        for (u32 i = lo; i < hi; ++i) {
            const f32 x = (nph_vec3_at(t->leaf_centers[i], axis) - axis_min) * scale;
            const u32 b = nph_min_u32((u32)x, REBUILD_BINS - 1U);
            bin_count[b] += 1U;
            bin_box[b] = box_union(bin_box[b], t->leaf_boxes[i]);
        }
        // Sweep: the cost of splitting after bin k is left area * count +
        // right area * count.
        f32 right_area[REBUILD_BINS];
        u32 right_count[REBUILD_BINS];
        NphAABB acc = nph_aabb_empty();
        u32 n = 0U;
        for (u32 b = REBUILD_BINS - 1U; b > 0U; --b) {
            acc = box_union(acc, bin_box[b]);
            n += bin_count[b];
            right_area[b] = n > 0U ? box_area(acc) : 0.0F;
            right_count[b] = n;
        }
        acc = nph_aabb_empty();
        n = 0U;
        f32 best = FLT_MAX;
        u32 best_split = 0U;
        for (u32 b = 0U; b + 1U < REBUILD_BINS; ++b) {
            acc = box_union(acc, bin_box[b]);
            n += bin_count[b];
            if (n == 0U || right_count[b + 1U] == 0U) {
                continue;
            }
            const f32 cost = box_area(acc) * (f32)n + right_area[b + 1U] * (f32)right_count[b + 1U];
            if (cost < best) {
                best = cost;
                best_split = b + 1U;
            }
        }
        if (best < FLT_MAX) {
            // In-place partition by bin, stable enough to stay deterministic.
            u32 i = lo;
            u32 j = hi;
            while (i < j) {
                const f32 x = (nph_vec3_at(t->leaf_centers[i], axis) - axis_min) * scale;
                const u32 b = nph_min_u32((u32)x, REBUILD_BINS - 1U);
                if (b < best_split) {
                    i += 1U;
                } else {
                    j -= 1U;
                    const u32 ti = t->leaf_indices[i];
                    const NphAABB tb = t->leaf_boxes[i];
                    const NphVec3 tc = t->leaf_centers[i];
                    t->leaf_indices[i] = t->leaf_indices[j];
                    t->leaf_boxes[i] = t->leaf_boxes[j];
                    t->leaf_centers[i] = t->leaf_centers[j];
                    t->leaf_indices[j] = ti;
                    t->leaf_boxes[j] = tb;
                    t->leaf_centers[j] = tc;
                }
            }
            if (i > lo && i < hi) {
                mid = i;
            }
        }
    }
    return mid;
}

// Builds the subtree over items [lo, hi) and returns its root. Pending
// right halves wait on an explicit stack (the bins scratch, three words a
// task), so a degenerate split sequence cannot exhaust the thread's stack.
static u32 build_subtree(NphAabbTree *t, u32 lo, u32 hi)
{
    u32 *tasks = t->bins_scratch; // (lo, hi, parent << 1 | slot)
    u32 task_count = 0U;
    u32 root = NULL_NODE;
    tasks[0] = lo;
    tasks[1] = hi;
    tasks[2] = NULL_NODE;
    task_count = 1U;
    while (task_count > 0U) {
        task_count -= 1U;
        const u32 tlo = tasks[3ULL * task_count];
        const u32 thi = tasks[3ULL * task_count + 1U];
        const u32 link = tasks[3ULL * task_count + 2U];
        u32 node;
        if (thi - tlo == 1U) {
            node = t->leaf_indices[tlo];
        } else {
            node = alloc_node(t);
            NPH_ASSERT(node != NULL_NODE); // nodes were freed for every inner node rebuilt
            const u32 mid = partition_sah(t, tlo, thi);
            // Children are filled in by their tasks; mark them unset.
            t->nodes[node].child1 = NULL_NODE;
            t->nodes[node].child2 = NULL_NODE;
            t->nodes[node].height = 1U; // inner until refit
            t->nodes[node].flags = (u16)(t->nodes[node].flags | NODE_REBUILT);
            tasks[3ULL * task_count] = mid;
            tasks[3ULL * task_count + 1U] = thi;
            tasks[3ULL * task_count + 2U] = (node << 1U) | 1U;
            task_count += 1U;
            tasks[3ULL * task_count] = tlo;
            tasks[3ULL * task_count + 1U] = mid;
            tasks[3ULL * task_count + 2U] = (node << 1U);
            task_count += 1U;
        }
        if (link == NULL_NODE) {
            root = node;
            t->nodes[node].parent = NULL_NODE;
        } else {
            const u32 parent = link >> 1U;
            if ((link & 1U) != 0U) {
                t->nodes[parent].child2 = node;
            } else {
                t->nodes[parent].child1 = node;
            }
            t->nodes[node].parent = parent;
        }
    }
    return root;
}

// Refits the inner nodes the rebuild made, bottom-up (children before
// parents: a post-order walk on the explicit stack). Kept subtrees and
// leaves are already right and are not entered.
static void refit_rebuilt(NphAabbTree *t, u32 root)
{
    u32 *stack = t->bins_scratch;
    u32 top = 0U;
    u32 last = NULL_NODE;
    u32 cur = root;
    while (top > 0U || cur != NULL_NODE) {
        if (cur != NULL_NODE) {
            if ((t->nodes[cur].flags & NODE_REBUILT) == 0U) {
                last = cur;
                cur = NULL_NODE;
                continue;
            }
            stack[top++] = cur;
            cur = t->nodes[cur].child1;
            continue;
        }
        const u32 peek = stack[top - 1U];
        NphAabbTreeNode *n = &t->nodes[peek];
        if (n->child2 != last && (t->nodes[n->child2].flags & NODE_REBUILT) != 0U) {
            cur = n->child2;
        } else {
            refit(t, peek);
            n->flags = (u16)(n->flags & ~NODE_REBUILT);
            last = peek;
            top -= 1U;
        }
    }
}

NphResult nph_aabb_tree_rebuild(NphAabbTree *tree, bool full, u32 *out_leaves)
{
    NPH_ASSERT(tree != NULL);
    if (out_leaves != NULL) {
        *out_leaves = 0U;
    }
    if (tree->root == NULL_NODE || is_leaf(&tree->nodes[tree->root]) ||
        (!full && (tree->nodes[tree->root].flags & NODE_ENLARGED) == 0U)) {
        if (tree->root != NULL_NODE) {
            tree->nodes[tree->root].flags = (u16)(tree->nodes[tree->root].flags & ~NODE_ENLARGED);
        }
        return NPH_RESULT_OK;
    }
    NPH_TRY(reserve_rebuild(tree, tree->node_count));
    // Collect the leaves to place: every leaf for a full build; otherwise
    // the untouched subtrees below enlarged nodes count as single items.
    // Inner nodes on the way are freed; their slots are reused below.
    u32 count = 0U;
    u32 *stack = tree->bins_scratch;
    u32 top = 0U;
    stack[top++] = tree->root;
    while (top > 0U) {
        const u32 i = stack[--top];
        NphAabbTreeNode *n = &tree->nodes[i];
        if (is_leaf(n) || (!full && (n->flags & NODE_ENLARGED) == 0U)) {
            n->flags = (u16)(n->flags & ~NODE_ENLARGED);
            tree->leaf_indices[count] = i;
            tree->leaf_boxes[count] = n->aabb;
            tree->leaf_centers[count] = nph_aabb_center(n->aabb);
            count += 1U;
            continue;
        }
        const u32 c1 = n->child1;
        const u32 c2 = n->child2;
        free_node(tree, i);
        stack[top++] = c2;
        stack[top++] = c1;
    }
    if (count == 1U) {
        tree->root = tree->leaf_indices[0];
        tree->nodes[tree->root].parent = NULL_NODE;
    } else {
        tree->root = build_subtree(tree, 0U, count);
        refit_rebuilt(tree, tree->root);
    }
    if (out_leaves != NULL) {
        *out_leaves = count;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

u32 nph_aabb_tree_height(const NphAabbTree *tree)
{
    NPH_ASSERT(tree != NULL);
    return tree->root == NULL_NODE ? 0U : tree->nodes[tree->root].height;
}

f32 nph_aabb_tree_area_ratio(const NphAabbTree *tree)
{
    NPH_ASSERT(tree != NULL);
    if (tree->root == NULL_NODE) {
        return 0.0F;
    }
    const f32 root_area = box_area(tree->nodes[tree->root].aabb);
    if (!(root_area > 0.0F)) {
        return 0.0F;
    }
    f64 total = 0.0;
    for (u32 i = 0U; i < tree->node_capacity; ++i) {
        const NphAabbTreeNode *n = &tree->nodes[i];
        if ((n->flags & NODE_ALLOCATED) != 0U && !is_leaf(n)) {
            total += (f64)box_area(n->aabb);
        }
    }
    return (f32)(total / (f64)root_area);
}

bool nph_aabb_tree_validate(const NphAabbTree *tree)
{
    if (tree == NULL) {
        return false;
    }
    u32 free_count = 0U;
    for (u32 i = tree->free_list; i != NULL_NODE; i = tree->nodes[i].parent) {
        if (i >= tree->node_capacity || free_count > tree->node_capacity) {
            return false;
        }
        free_count += 1U;
    }
    if (free_count + tree->node_count != tree->node_capacity) {
        return false;
    }
    if (tree->root == NULL_NODE) {
        return tree->node_count == 0U && tree->proxy_count == 0U;
    }
    if (tree->nodes[tree->root].parent != NULL_NODE) {
        return false;
    }
    u32 stack[NPH_AABB_TREE_STACK_SIZE];
    u32 top = 0U;
    u32 seen = 0U;
    u32 leaves = 0U;
    stack[top++] = tree->root;
    while (top > 0U) {
        const u32 i = stack[--top];
        if (i >= tree->node_capacity) {
            return false;
        }
        const NphAabbTreeNode *n = &tree->nodes[i];
        if ((n->flags & NODE_ALLOCATED) == 0U) {
            return false;
        }
        seen += 1U;
        if (is_leaf(n)) {
            leaves += 1U;
            continue;
        }
        if (n->child1 >= tree->node_capacity || n->child2 >= tree->node_capacity) {
            return false;
        }
        const NphAabbTreeNode *c1 = &tree->nodes[n->child1];
        const NphAabbTreeNode *c2 = &tree->nodes[n->child2];
        if (c1->parent != i || c2->parent != i) {
            return false;
        }
        if (n->height != 1U + nph_max_u32(c1->height, c2->height)) {
            return false;
        }
        if (!box_contains(n->aabb, c1->aabb) || !box_contains(n->aabb, c2->aabb)) {
            return false;
        }
        if (n->category_bits != (c1->category_bits | c2->category_bits)) {
            return false;
        }
        if (top + 2U > NPH_AABB_TREE_STACK_SIZE) {
            return false;
        }
        stack[top++] = n->child1;
        stack[top++] = n->child2;
    }
    return seen == tree->node_count && leaves == tree->proxy_count;
}
