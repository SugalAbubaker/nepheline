// SPDX-License-Identifier: Apache-2.0
// Triangle meshes (mesh.h; docs/physics.md "Meshes"; ADR-0046): the builder
// (welding, degenerate triangles, active edges, a binned-SAH tree collapsed
// into four-wide nodes), the validation of blocks read from files, and the
// box and ray queries.

#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/log.h>
#include <nepheline/math/simd.h>
#include <nepheline/physics/mesh.h>

#include "physics_internal.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

#define NONE         0xFFFFFFFFU
#define BINS         16U
#define HEADER_BYTES ((u32)sizeof(NphPhysicsMesh))

// Below this binary depth splits are binned SAH; past it they are medians,
// so the tree's depth stays bounded (median splits halve a range) whatever
// the input: 64 + 28 binary levels, at most 92, which the four-wide tree
// folds to 46 (the query stack below holds three siblings a level).
#define SAH_DEPTH   64U
#define STACK_DEPTH 160U

// The relative size below which a triangle has no area: the sine of its
// angle at the first vertex, squared, under 1e-12.
#define DEGENERATE_SIN_SQ 1e-12

void nph_physics_mesh_triangle(const NphPhysicsMesh *m, u32 t, NphVec3 out[3])
{
    const NphVec3 *v = nph_physics_mesh_vertices(m);
    const u32 *tri = nph_physics_mesh_triangles(m) + 3ULL * t;
    out[0] = v[tri[0]];
    out[1] = v[tri[1]];
    out[2] = v[tri[2]];
}

// ---------------------------------------------------------------------------
// The build's scratch, released on every path out
// ---------------------------------------------------------------------------

typedef struct EdgeKey {
    u64 key; // the edge's welded vertices, lower first
    u32 triangle;
    u32 slot; // the edge's index in its triangle (from vertex slot to slot + 1)
} EdgeKey;

typedef struct Item {
    NphAABB box;
    NphVec3 center;
    u32 triangle; // into the kept triangles
} Item;

// A binary node: inner (count 0, children left and right) or a leaf of
// `count` items from `first`.
typedef struct BinNode {
    NphAABB box;
    u32 left;
    u32 right;
    u32 first;
    u32 count;
} BinNode;

typedef struct Scratch {
    NphAllocator *alloc;
    u32 n_in;                  // input vertices
    u32 t_in;                  // input triangles
    u32 *remap;                // [n_in] input vertex -> welded
    u32 *chain;                // [n_in] the next welded vertex in the same cell
    NphVec3 *welded;           // [n_in]
    u32 *tris;                 // [3 * t_in] kept triangles, welded indices
    u32 *originals;            // [t_in]
    u8 *edges;                 // [t_in]
    EdgeKey *keys;             // [3 * t_in]
    Item *items;               // [t_in]
    BinNode *bins;             // [2 * t_in]
    u32 *tasks;                // [4 * 2 * t_in]
    u32 *vertex_out;           // [n_in] welded -> output index
    NphPhysicsMeshNode *nodes; // [max(t_in, 2) - 1] the four-wide tree, before its block
} Scratch;

// A four-wide tree over t triangles has at most one node per inner node
// of the binary tree it collapses: t - 1, and one for a lone leaf.
static u32 node_capacity(u32 t)
{
    return nph_max_u32(t, 2U) - 1U;
}

static void scratch_free(Scratch *s)
{
    NphAllocator *a = s->alloc;
    const u64 n = s->n_in;
    const u64 t = s->t_in;
    if (s->remap != NULL) {
        NPH_FREE_ARRAY(a, u32, s->remap, n);
    }
    if (s->chain != NULL) {
        NPH_FREE_ARRAY(a, u32, s->chain, n);
    }
    if (s->welded != NULL) {
        NPH_FREE_ARRAY(a, NphVec3, s->welded, n);
    }
    if (s->tris != NULL) {
        NPH_FREE_ARRAY(a, u32, s->tris, 3U * t);
    }
    if (s->originals != NULL) {
        NPH_FREE_ARRAY(a, u32, s->originals, t);
    }
    if (s->edges != NULL) {
        NPH_FREE_ARRAY(a, u8, s->edges, t);
    }
    if (s->keys != NULL) {
        NPH_FREE_ARRAY(a, EdgeKey, s->keys, 3U * t);
    }
    if (s->items != NULL) {
        NPH_FREE_ARRAY(a, Item, s->items, t);
    }
    if (s->bins != NULL) {
        NPH_FREE_ARRAY(a, BinNode, s->bins, 2U * t);
    }
    if (s->tasks != NULL) {
        NPH_FREE_ARRAY(a, u32, s->tasks, 8U * t);
    }
    if (s->vertex_out != NULL) {
        NPH_FREE_ARRAY(a, u32, s->vertex_out, n);
    }
    if (s->nodes != NULL) {
        NPH_FREE_ARRAY(a, NphPhysicsMeshNode, s->nodes, node_capacity(s->t_in));
    }
}

static NphResult scratch_alloc(Scratch *s)
{
    NphAllocator *a = s->alloc;
    const u64 n = s->n_in;
    const u64 t = s->t_in;
    s->remap = NPH_ALLOC_ARRAY(a, u32, n);
    s->chain = NPH_ALLOC_ARRAY(a, u32, n);
    s->welded = NPH_ALLOC_ARRAY(a, NphVec3, n);
    s->tris = NPH_ALLOC_ARRAY(a, u32, 3U * t);
    s->originals = NPH_ALLOC_ARRAY(a, u32, t);
    s->edges = NPH_ALLOC_ARRAY_ZEROED(a, u8, t);
    s->keys = NPH_ALLOC_ARRAY(a, EdgeKey, 3U * t);
    s->items = NPH_ALLOC_ARRAY(a, Item, t);
    s->bins = NPH_ALLOC_ARRAY(a, BinNode, 2U * t);
    s->tasks = NPH_ALLOC_ARRAY(a, u32, 8U * t);
    s->vertex_out = NPH_ALLOC_ARRAY(a, u32, n);
    s->nodes = NPH_ALLOC_ARRAY(a, NphPhysicsMeshNode, node_capacity(s->t_in));
    if (s->remap == NULL || s->chain == NULL || s->welded == NULL || s->tris == NULL ||
        s->originals == NULL || s->edges == NULL || s->keys == NULL || s->items == NULL ||
        s->bins == NULL || s->tasks == NULL || s->vertex_out == NULL || s->nodes == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Welding
// ---------------------------------------------------------------------------

typedef struct CellKey {
    i32 x;
    i32 y;
    i32 z;
} CellKey;

static CellKey cell_of(NphVec3 p, f32 inv_cell)
{
    return (CellKey){(i32)nph_f32_floor(p.x * inv_cell), (i32)nph_f32_floor(p.y * inv_cell),
                     (i32)nph_f32_floor(p.z * inv_cell)};
}

// An exact copy's key: the coordinates' bits, with -0 as +0.
static CellKey bits_of(NphVec3 p)
{
    const f32 c[3] = {p.x + 0.0F, p.y + 0.0F, p.z + 0.0F};
    CellKey k;
    memcpy(&k, c, sizeof k);
    return k;
}

// Welds the input vertices: each joins the lowest-numbered earlier
// representative within `tol`, or becomes one. Cells are 2 tol wide, so the
// ball around a vertex meets at most eight of them. Returns the count.
static NphResult weld(Scratch *s, const NphVec3 *v, f32 tol, u32 *out_count)
{
    NphHashMap cells;
    const NphHashMapDesc desc = {(u32)sizeof(CellKey), (u32)sizeof(u32), NULL, NULL, 0U};
    NPH_TRY(nph_hashmap_init(&cells, s->alloc, &desc));
    NphResult r = nph_hashmap_reserve(&cells, nph_min_u32(s->n_in, 1U << 20U));
    const bool exact = !(tol > 0.0F);
    const f32 inv_cell = exact ? 0.0F : 0.5F / tol;
    const f32 tol_sq = tol * tol;
    u32 count = 0U;
    for (u32 i = 0U; i < s->n_in && r == NPH_RESULT_OK; ++i) {
        const NphVec3 p = v[i];
        u32 best = NONE;
        if (exact) {
            const CellKey key = bits_of(p);
            const u32 *head = nph_hashmap_find(&cells, &key);
            if (head != NULL) {
                best = *head;
            }
        } else {
            const CellKey lo = cell_of(nph_vec3_sub(p, nph_vec3_splat(tol)), inv_cell);
            const CellKey hi = cell_of(nph_vec3_add(p, nph_vec3_splat(tol)), inv_cell);
            for (i32 x = lo.x; x <= hi.x; ++x) {
                for (i32 y = lo.y; y <= hi.y; ++y) {
                    for (i32 z = lo.z; z <= hi.z; ++z) {
                        const CellKey key = {x, y, z};
                        const u32 *head = nph_hashmap_find(&cells, &key);
                        for (u32 k = head != NULL ? *head : NONE; k != NONE; k = s->chain[k]) {
                            if (k < best && nph_vec3_distance_sq(s->welded[k], p) <= tol_sq) {
                                best = k;
                            }
                        }
                    }
                }
            }
        }
        if (best != NONE) {
            s->remap[i] = best;
            continue;
        }
        const u32 w = count++;
        s->welded[w] = p;
        s->remap[i] = w;
        const CellKey key = exact ? bits_of(p) : cell_of(p, inv_cell);
        u32 *head = nph_hashmap_find(&cells, &key);
        if (head != NULL) {
            s->chain[w] = *head;
            *head = w;
        } else {
            s->chain[w] = NONE;
            r = nph_hashmap_insert(&cells, &key, &w);
        }
    }
    nph_hashmap_free(&cells);
    *out_count = count;
    return r;
}

// ---------------------------------------------------------------------------
// Triangles and their edges
// ---------------------------------------------------------------------------

static NphVec3 triangle_normal(const Scratch *s, u32 t)
{
    const u32 *tri = s->tris + 3ULL * t;
    const NphVec3 a = s->welded[tri[0]];
    return nph_vec3_normalize_or_zero(
        nph_vec3_cross(nph_vec3_sub(s->welded[tri[1]], a), nph_vec3_sub(s->welded[tri[2]], a)));
}

// Keeps the triangles with three distinct welded vertices and an area.
static u32 keep_triangles(Scratch *s, const u32 *indices)
{
    u32 kept = 0U;
    for (u32 t = 0U; t < s->t_in; ++t) {
        const u32 a = s->remap[indices[3ULL * t]];
        const u32 b = s->remap[indices[3ULL * t + 1U]];
        const u32 c = s->remap[indices[3ULL * t + 2U]];
        if (a == b || b == c || a == c) {
            continue;
        }
        // In 64 bits: the products of squared lengths overflow f32 long
        // before coordinates do.
        const NphVec3 pa = s->welded[a];
        const f64 e1[3] = {(f64)s->welded[b].x - (f64)pa.x, (f64)s->welded[b].y - (f64)pa.y,
                           (f64)s->welded[b].z - (f64)pa.z};
        const f64 e2[3] = {(f64)s->welded[c].x - (f64)pa.x, (f64)s->welded[c].y - (f64)pa.y,
                           (f64)s->welded[c].z - (f64)pa.z};
        const f64 cx = e1[1] * e2[2] - e1[2] * e2[1];
        const f64 cy = e1[2] * e2[0] - e1[0] * e2[2];
        const f64 cz = e1[0] * e2[1] - e1[1] * e2[0];
        const f64 cross_sq = cx * cx + cy * cy + cz * cz;
        const f64 l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
        const f64 l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        if (!(cross_sq > DEGENERATE_SIN_SQ * l1 * l2)) {
            continue;
        }
        s->tris[3ULL * kept] = a;
        s->tris[3ULL * kept + 1U] = b;
        s->tris[3ULL * kept + 2U] = c;
        s->originals[kept] = t;
        kept += 1U;
    }
    return kept;
}

static int edge_key_compare(const void *pa, const void *pb)
{
    const EdgeKey *a = pa;
    const EdgeKey *b = pb;
    if (a->key != b->key) {
        return a->key < b->key ? -1 : 1;
    }
    if (a->triangle != b->triangle) {
        return a->triangle < b->triangle ? -1 : 1;
    }
    return a->slot < b->slot ? -1 : (a->slot > b->slot ? 1 : 0);
}

// Whether the edge two triangles share is active: back to back, or convex
// by more than the threshold (for a two-sided mesh, bent either way by more
// than it: its back sees the other convexity). Flat and concave edges are
// not (Jolt's ActiveEdges::IsEdgeActive).
static bool edge_active(const Scratch *s, const EdgeKey *k1, const EdgeKey *k2, f32 cos_limit,
                        bool two_sided)
{
    const u32 *t1 = s->tris + 3ULL * k1->triangle;
    const u32 *t2 = s->tris + 3ULL * k2->triangle;
    // Consistent winding runs the edge one way in each triangle.
    if (t1[k1->slot] == t2[k2->slot]) {
        return true;
    }
    const NphVec3 n1 = triangle_normal(s, k1->triangle);
    const NphVec3 n2 = triangle_normal(s, k2->triangle);
    const f32 cos_angle = nph_vec3_dot(n1, n2);
    if (cos_angle < -0.99985F) {
        return true; // back to back: a fin
    }
    if (!(cos_angle < cos_limit)) {
        return false; // within the threshold of flat
    }
    if (two_sided) {
        return true;
    }
    // Convex when the far vertex of the second lies behind the first's plane.
    const NphVec3 start = s->welded[t1[k1->slot]];
    const NphVec3 far = s->welded[t2[(k2->slot + 2U) % 3U]];
    return nph_vec3_dot(n1, nph_vec3_sub(far, start)) < 0.0F;
}

static void mark_active_edges(Scratch *s, u32 kept, f32 cos_limit, bool two_sided)
{
    const u64 n = 3ULL * kept;
    for (u32 t = 0U; t < kept; ++t) {
        for (u32 e = 0U; e < 3U; ++e) {
            const u32 a = s->tris[3ULL * t + e];
            const u32 b = s->tris[3ULL * t + (e + 1U) % 3U];
            EdgeKey *k = &s->keys[3ULL * t + e];
            k->key = a < b ? ((u64)a << 32U) | b : ((u64)b << 32U) | a;
            k->triangle = t;
            k->slot = e;
        }
    }
    qsort(s->keys, (size_t)n, sizeof(EdgeKey), edge_key_compare);
    u64 i = 0U;
    while (i < n) {
        u64 j = i + 1U;
        while (j < n && s->keys[j].key == s->keys[i].key) {
            j += 1U;
        }
        // An open edge, or one shared by more than two, is always active.
        const bool active =
            j - i != 2U || edge_active(s, &s->keys[i], &s->keys[i + 1U], cos_limit, two_sided);
        if (active) {
            for (u64 k = i; k < j; ++k) {
                s->edges[s->keys[k].triangle] |= (u8)(1U << s->keys[k].slot);
            }
        }
        i = j;
    }
}

// ---------------------------------------------------------------------------
// The tree: binned SAH (Wald 2007), collapsed to four wide
// ---------------------------------------------------------------------------

static f32 area(NphAABB b)
{
    const NphVec3 d = nph_vec3_sub(b.max, b.min);
    return d.x * d.y + d.y * d.z + d.z * d.x;
}

static NphAABB box_union(NphAABB a, NphAABB b)
{
    return (NphAABB){nph_vec3_min(a.min, b.min), nph_vec3_max(a.max, b.max)};
}

static u32 bin_of(const Item *it, u32 axis, f32 axis_min, f32 scale)
{
    const f32 x = (nph_vec3_at(it->center, axis) - axis_min) * scale;
    return x > 0.0F ? nph_min_u32((u32)x, BINS - 1U) : 0U;
}

static int center_compare_x(const void *pa, const void *pb)
{
    const Item *a = pa;
    const Item *b = pb;
    if (a->center.x != b->center.x) {
        return a->center.x < b->center.x ? -1 : 1;
    }
    return a->triangle < b->triangle ? -1 : (a->triangle > b->triangle ? 1 : 0);
}

static int center_compare_y(const void *pa, const void *pb)
{
    const Item *a = pa;
    const Item *b = pb;
    if (a->center.y != b->center.y) {
        return a->center.y < b->center.y ? -1 : 1;
    }
    return a->triangle < b->triangle ? -1 : (a->triangle > b->triangle ? 1 : 0);
}

static int center_compare_z(const void *pa, const void *pb)
{
    const Item *a = pa;
    const Item *b = pb;
    if (a->center.z != b->center.z) {
        return a->center.z < b->center.z ? -1 : 1;
    }
    return a->triangle < b->triangle ? -1 : (a->triangle > b->triangle ? 1 : 0);
}

// Splits items [lo, hi) (more than a leaf's worth) and returns the split,
// both sides non-empty: the cheapest of the binned SAH's planes along the
// centres' longest axis, or below SAH_DEPTH... past it, the median.
static u32 split(Item *items, u32 lo, u32 hi, u32 depth)
{
    const u32 count = hi - lo;
    NphAABB cb = nph_aabb_from_point(items[lo].center);
    for (u32 i = lo + 1U; i < hi; ++i) {
        cb.min = nph_vec3_min(cb.min, items[i].center);
        cb.max = nph_vec3_max(cb.max, items[i].center);
    }
    const NphVec3 extent = nph_vec3_sub(cb.max, cb.min);
    const u32 axis = nph_vec3_max_index(extent);
    const f32 axis_min = nph_vec3_at(cb.min, axis);
    const f32 axis_extent = nph_vec3_at(extent, axis);
    const u32 mid = lo + count / 2U;
    if (!(axis_extent > 0.0F)) {
        return mid; // every centre in one point: any split
    }
    if (depth >= SAH_DEPTH) {
        int (*cmp)(const void *, const void *) =
            axis == 0U ? center_compare_x : (axis == 1U ? center_compare_y : center_compare_z);
        qsort(items + lo, count, sizeof(Item), cmp);
        return mid;
    }
    u32 bin_count[BINS] = {0};
    NphAABB bin_box[BINS];
    for (u32 b = 0U; b < BINS; ++b) {
        bin_box[b] = nph_aabb_empty();
    }
    const f32 scale = (f32)BINS / axis_extent;
    for (u32 i = lo; i < hi; ++i) {
        const u32 b = bin_of(&items[i], axis, axis_min, scale);
        bin_count[b] += 1U;
        bin_box[b] = box_union(bin_box[b], items[i].box);
    }
    f32 right_area[BINS];
    u32 right_count[BINS];
    NphAABB acc = nph_aabb_empty();
    u32 n = 0U;
    for (u32 b = BINS - 1U; b > 0U; --b) {
        acc = box_union(acc, bin_box[b]);
        n += bin_count[b];
        right_area[b] = n > 0U ? area(acc) : 0.0F;
        right_count[b] = n;
    }
    acc = nph_aabb_empty();
    n = 0U;
    f32 best = FLT_MAX;
    u32 best_split = 0U;
    for (u32 b = 0U; b + 1U < BINS; ++b) {
        acc = box_union(acc, bin_box[b]);
        n += bin_count[b];
        if (n == 0U || right_count[b + 1U] == 0U) {
            continue;
        }
        const f32 cost = area(acc) * (f32)n + right_area[b + 1U] * (f32)right_count[b + 1U];
        if (cost < best) {
            best = cost;
            best_split = b + 1U;
        }
    }
    if (!(best < FLT_MAX)) {
        return mid;
    }
    // In place, by bin: deterministic.
    u32 i = lo;
    u32 j = hi;
    while (i < j) {
        if (bin_of(&items[i], axis, axis_min, scale) < best_split) {
            i += 1U;
        } else {
            j -= 1U;
            const Item t = items[i];
            items[i] = items[j];
            items[j] = t;
        }
    }
    return i > lo && i < hi ? i : mid;
}

// The binary tree over items [0, count): returns the node count; node 0 is
// the root. Pending right halves wait on an explicit stack of (lo, hi,
// parent link, depth), so a long chain of splits cannot exhaust the
// thread's.
static u32 build_binary(Scratch *s, u32 count)
{
    u32 nodes = 0U;
    u32 *tasks = s->tasks;
    u32 pending = 1U;
    tasks[0] = 0U;
    tasks[1] = count;
    tasks[2] = NONE;
    tasks[3] = 0U;
    while (pending > 0U) {
        pending -= 1U;
        const u32 lo = tasks[4ULL * pending];
        const u32 hi = tasks[4ULL * pending + 1U];
        const u32 link = tasks[4ULL * pending + 2U];
        const u32 depth = tasks[4ULL * pending + 3U];
        const u32 index = nodes++;
        BinNode *node = &s->bins[index];
        node->box = s->items[lo].box;
        for (u32 i = lo + 1U; i < hi; ++i) {
            node->box = box_union(node->box, s->items[i].box);
        }
        if (link != NONE) {
            BinNode *parent = &s->bins[link >> 1U];
            if ((link & 1U) == 0U) {
                parent->left = index;
            } else {
                parent->right = index;
            }
        }
        if (hi - lo <= NPH_PHYSICS_MESH_LEAF_TRIANGLES) {
            node->first = lo;
            node->count = hi - lo;
            node->left = NONE;
            node->right = NONE;
            continue;
        }
        const u32 mid = split(s->items, lo, hi, depth);
        node->first = 0U;
        node->count = 0U;
        // The right half first, so the left is built next (smaller indices
        // for the left: the order the tree's leaves follow).
        const u32 r = 4U * pending;
        tasks[r] = mid;
        tasks[r + 1U] = hi;
        tasks[r + 2U] = (index << 1U) | 1U;
        tasks[r + 3U] = depth + 1U;
        tasks[r + 4U] = lo;
        tasks[r + 5U] = mid;
        tasks[r + 6U] = index << 1U;
        tasks[r + 7U] = depth + 1U;
        pending += 2U;
    }
    return nodes;
}

static void set_child(NphPhysicsMeshNode *n, u32 slot, NphAABB box, u32 child)
{
    n->min_x[slot] = box.min.x;
    n->min_y[slot] = box.min.y;
    n->min_z[slot] = box.min.z;
    n->max_x[slot] = box.max.x;
    n->max_y[slot] = box.max.y;
    n->max_z[slot] = box.max.z;
    n->child[slot] = child;
}

static void set_empty(NphPhysicsMeshNode *n, u32 slot)
{
    const NphAABB empty = {nph_vec3_splat(FLT_MAX), nph_vec3_splat(-FLT_MAX)};
    set_child(n, slot, empty, NPH_PHYSICS_MESH_CHILD_EMPTY);
}

// Collapses the binary tree into four-wide nodes: each node opens its
// largest inner children until it holds four (Dammertz et al. 2008), in
// order, so the leaves keep the binary tree's order. Returns the count.
static u32 collapse(const Scratch *s, NphPhysicsMeshNode *out)
{
    const BinNode *bins = s->bins;
    u32 count = 1U;
    if (bins[0].count > 0U) {
        // One leaf: the whole mesh.
        set_child(&out[0], 0U, bins[0].box, nph_physics_mesh_leaf(bins[0].first, bins[0].count));
        for (u32 k = 1U; k < 4U; ++k) {
            set_empty(&out[0], k);
        }
        return count;
    }
    u32 *tasks = s->tasks; // (binary node, output node)
    u32 pending = 1U;
    tasks[0] = 0U;
    tasks[1] = 0U;
    while (pending > 0U) {
        pending -= 1U;
        const u32 b = tasks[2ULL * pending];
        const u32 o = tasks[2ULL * pending + 1U];
        u32 kids[4] = {bins[b].left, bins[b].right, NONE, NONE};
        u32 n = 2U;
        while (n < 4U) {
            u32 open = NONE;
            f32 best = -1.0F;
            for (u32 k = 0U; k < n; ++k) {
                const BinNode *c = &bins[kids[k]];
                if (c->count == 0U && area(c->box) > best) {
                    best = area(c->box);
                    open = k;
                }
            }
            if (open == NONE) {
                break;
            }
            const BinNode *c = &bins[kids[open]];
            for (u32 k = n; k > open + 1U; --k) {
                kids[k] = kids[k - 1U];
            }
            kids[open] = c->left;
            kids[open + 1U] = c->right;
            n += 1U;
        }
        // Children in reverse onto the stack, so they are emitted in order.
        u32 child_index[4];
        for (u32 k = 0U; k < n; ++k) {
            const BinNode *c = &bins[kids[k]];
            child_index[k] = c->count > 0U ? nph_physics_mesh_leaf(c->first, c->count) : count++;
            set_child(&out[o], k, c->box, child_index[k]);
        }
        for (u32 k = n; k < 4U; ++k) {
            set_empty(&out[o], k);
        }
        for (u32 k = n; k > 0U; --k) {
            if (!nph_physics_mesh_child_is_leaf(child_index[k - 1U])) {
                tasks[2ULL * pending] = kids[k - 1U];
                tasks[2ULL * pending + 1U] = child_index[k - 1U];
                pending += 1U;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static bool desc_valid(const NphPhysicsMeshDesc *d)
{
    if (d == NULL || d->vertices == NULL || d->indices == NULL || d->vertex_count < 3U ||
        d->triangle_count == 0U || d->triangle_count > NPH_PHYSICS_MESH_MAX_TRIANGLES ||
        !nph_f32_is_finite(d->weld_tolerance) || !nph_f32_is_finite(d->active_edge_angle) ||
        d->active_edge_angle < 0.0F || d->active_edge_angle > 3.14159265F) {
        return false;
    }
    for (u32 i = 0U; i < d->vertex_count; ++i) {
        const NphVec3 v = d->vertices[i];
        if (!nph_vec3_is_finite(v) ||
            nph_vec3_max_component(nph_vec3_abs(v)) > NPH_PHYSICS_MAX_COORDINATE) {
            return false;
        }
    }
    const u64 n = 3ULL * d->triangle_count;
    for (u64 i = 0U; i < n; ++i) {
        if (d->indices[i] >= d->vertex_count) {
            return false;
        }
    }
    return true;
}

NphResult nph_physics_mesh_build(const NphPhysicsMeshDesc *desc, NphAllocator *alloc,
                                 NphPhysicsMesh **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    *out = NULL;
    if (!desc_valid(desc)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    f32 extent = 0.0F;
    for (u32 i = 0U; i < desc->vertex_count; ++i) {
        const NphVec3 a = nph_vec3_abs(desc->vertices[i]);
        extent = nph_f32_max(extent, nph_f32_max(a.x, nph_f32_max(a.y, a.z)));
    }
    // A tolerance of at least 2^-29 of the extent keeps the cells' indices
    // in 32 bits.
    f32 tol = desc->weld_tolerance;
    if (tol == 0.0F) {
        tol = 1e-6F * extent;
    }
    if (tol > 0.0F) {
        tol = nph_f32_max(tol, extent * 1.8626451e-9F);
    }
    const f32 angle = desc->active_edge_angle > 0.0F ? desc->active_edge_angle
                                                     : NPH_PHYSICS_MESH_DEFAULT_ACTIVE_ANGLE;
    const bool two_sided = desc->two_sided != 0;

    Scratch s;
    memset(&s, 0, sizeof s);
    s.alloc = alloc;
    s.n_in = desc->vertex_count;
    s.t_in = desc->triangle_count;
    NphResult r = scratch_alloc(&s);
    u32 welded = 0U;
    if (r == NPH_RESULT_OK) {
        r = weld(&s, desc->vertices, tol, &welded);
    }
    const u32 kept = r == NPH_RESULT_OK ? keep_triangles(&s, desc->indices) : 0U;
    if (r == NPH_RESULT_OK && kept == 0U) {
        r = NPH_RESULT_INVALID_ARGUMENT;
    }
    if (r != NPH_RESULT_OK) {
        scratch_free(&s);
        return r;
    }
    mark_active_edges(&s, kept, nph_f32_cos(angle), two_sided);
    for (u32 t = 0U; t < kept; ++t) {
        const u32 *tri = s.tris + 3ULL * t;
        Item *it = &s.items[t];
        it->box = nph_aabb_from_point(s.welded[tri[0]]);
        it->box.min = nph_vec3_min(nph_vec3_min(it->box.min, s.welded[tri[1]]), s.welded[tri[2]]);
        it->box.max = nph_vec3_max(nph_vec3_max(it->box.max, s.welded[tri[1]]), s.welded[tri[2]]);
        it->center = nph_aabb_center(it->box);
        it->triangle = t;
    }
    build_binary(&s, kept);
    // The output's vertices: those the kept triangles use, numbered in the
    // order the tree's leaves first use them.
    for (u32 i = 0U; i < welded; ++i) {
        s.vertex_out[i] = NONE;
    }
    u32 vertex_count = 0U;
    for (u32 i = 0U; i < kept; ++i) {
        const u32 *tri = s.tris + 3ULL * s.items[i].triangle;
        for (u32 k = 0U; k < 3U; ++k) {
            if (s.vertex_out[tri[k]] == NONE) {
                s.vertex_out[tri[k]] = vertex_count++;
            }
        }
    }
    const u32 node_count = collapse(&s, s.nodes);
    NPH_ASSERT(node_count <= node_capacity(kept));
    const u64 nodes_offset = HEADER_BYTES;
    const u64 vertices_offset = nodes_offset + (u64)node_count * sizeof(NphPhysicsMeshNode);
    const u64 triangles_offset = vertices_offset + (u64)vertex_count * sizeof(NphVec3);
    const u64 edges_offset = triangles_offset + 3ULL * kept * sizeof(u32);
    const u64 originals_offset = (edges_offset + kept + 3U) & ~3ULL;
    const u64 byte_size = originals_offset + (u64)kept * sizeof(u32);
    if (byte_size > NPH_U32_MAX) {
        scratch_free(&s);
        return NPH_RESULT_INVALID_ARGUMENT; // too large for one block
    }
    NphPhysicsMesh *m = NPH_ALLOC_ZEROED(alloc, byte_size, 16U);
    if (m == NULL) {
        scratch_free(&s);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memcpy((u8 *)m + nodes_offset, s.nodes, (size_t)node_count * sizeof(NphPhysicsMeshNode));
    NphVec3 *vertices = (NphVec3 *)((u8 *)m + vertices_offset);
    u32 *triangles = (u32 *)((u8 *)m + triangles_offset);
    u8 *edges = (u8 *)m + edges_offset;
    u32 *originals = (u32 *)((u8 *)m + originals_offset);
    for (u32 i = 0U; i < welded; ++i) {
        if (s.vertex_out[i] != NONE) {
            vertices[s.vertex_out[i]] = s.welded[i];
        }
    }
    NphAABB bounds = nph_aabb_from_point(vertices[0]);
    for (u32 i = 1U; i < vertex_count; ++i) {
        bounds.min = nph_vec3_min(bounds.min, vertices[i]);
        bounds.max = nph_vec3_max(bounds.max, vertices[i]);
    }
    for (u32 i = 0U; i < kept; ++i) {
        const u32 t = s.items[i].triangle;
        for (u32 k = 0U; k < 3U; ++k) {
            triangles[3ULL * i + k] = s.vertex_out[s.tris[3ULL * t + k]];
        }
        edges[i] = s.edges[t];
        originals[i] = s.originals[t];
    }
    m->byte_size = (u32)byte_size;
    m->vertex_count = vertex_count;
    m->triangle_count = kept;
    m->node_count = node_count;
    m->nodes_offset = (u32)nodes_offset;
    m->vertices_offset = (u32)vertices_offset;
    m->triangles_offset = (u32)triangles_offset;
    m->edges_offset = (u32)edges_offset;
    m->originals_offset = (u32)originals_offset;
    m->flags = two_sided ? NPH_PHYSICS_MESH_TWO_SIDED : 0U;
    m->bounds = bounds;
    scratch_free(&s);
    *out = m;
    return NPH_RESULT_OK;
}

void nph_physics_mesh_free(NphPhysicsMesh *mesh, NphAllocator *alloc)
{
    if (mesh != NULL) {
        NPH_FREE(alloc, mesh, mesh->byte_size, 16U);
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

#define MESH_FAIL(...)                                                                             \
    do {                                                                                           \
        NPH_LOG_WARN(&NPH_LOG_PHYSICS, "mesh validate: " __VA_ARGS__);                             \
        return false;                                                                              \
    } while (0)

// A child waiting in the validation's stack, with the node and slot whose
// box must hold it (no parent for the root).
typedef struct Pending {
    u32 child;
    u32 parent;
    u32 slot;
    u32 depth; // of the child, the root's children at 1
} Pending;

// A query at an inner node of depth d holds at most three siblings a level
// above it and the node's four children: whatever order it visits them in
// (the raycast's nearest first), its stack stays within this.
NPH_INLINE u32 stack_need(u32 depth)
{
    return 3U * depth + 4U;
}

static bool vertex_in_range(NphVec3 v)
{
    return nph_vec3_max_component(nph_vec3_abs(v)) <= NPH_PHYSICS_MAX_COORDINATE; // not NaN
}

// An empty slot's box: what the builder writes, which no box overlaps.
static bool empty_box(const NphPhysicsMeshNode *n, u32 k)
{
    return n->min_x[k] == FLT_MAX && n->min_y[k] == FLT_MAX && n->min_z[k] == FLT_MAX &&
           n->max_x[k] == -FLT_MAX && n->max_y[k] == -FLT_MAX && n->max_z[k] == -FLT_MAX;
}

static bool range_ok(u64 offset, u64 count, u64 elem, u64 align, u64 size)
{
    return offset >= HEADER_BYTES && offset % align == 0U && count <= size &&
           offset + count * elem <= size;
}

static bool box_contains(const NphPhysicsMeshNode *n, u32 k, NphVec3 p)
{
    return p.x >= n->min_x[k] && p.x <= n->max_x[k] && p.y >= n->min_y[k] && p.y <= n->max_y[k] &&
           p.z >= n->min_z[k] && p.z <= n->max_z[k];
}

static bool box_contains_node(const NphPhysicsMeshNode *n, u32 k, const NphPhysicsMeshNode *c)
{
    for (u32 j = 0U; j < 4U; ++j) {
        if (c->child[j] == NPH_PHYSICS_MESH_CHILD_EMPTY) {
            continue;
        }
        if (c->min_x[j] < n->min_x[k] || c->max_x[j] > n->max_x[k] || c->min_y[j] < n->min_y[k] ||
            c->max_y[j] > n->max_y[k] || c->min_z[j] < n->min_z[k] || c->max_z[j] > n->max_z[k]) {
            return false;
        }
    }
    return true;
}

bool nph_physics_mesh_validate(const void *block, u32 size)
{
    if (block == NULL || size < HEADER_BYTES || ((uptr)block & 15U) != 0U) {
        MESH_FAIL("no 16-byte aligned header in %u bytes", size);
    }
    const NphPhysicsMesh *m = block;
    const u64 bytes = m->byte_size;
    if (bytes < HEADER_BYTES || bytes > size) {
        MESH_FAIL("byte_size %u outside the %u-byte block", m->byte_size, size);
    }
    const u32 nt = m->triangle_count;
    const u32 nv = m->vertex_count;
    const u32 nn = m->node_count;
    if (nt == 0U || nt > NPH_PHYSICS_MESH_MAX_TRIANGLES || nv < 3U || nn == 0U ||
        nn > node_capacity(nt) || (m->flags & ~(u32)NPH_PHYSICS_MESH_TWO_SIDED) != 0U) {
        MESH_FAIL("counts triangles=%u vertices=%u nodes=%u flags=%u", nt, nv, nn, m->flags);
    }
    if (!range_ok(m->nodes_offset, nn, sizeof(NphPhysicsMeshNode), 16U, bytes) ||
        !range_ok(m->vertices_offset, nv, sizeof(NphVec3), 4U, bytes) ||
        !range_ok(m->triangles_offset, 3ULL * nt, sizeof(u32), 4U, bytes) ||
        !range_ok(m->edges_offset, nt, sizeof(u8), 1U, bytes) ||
        !range_ok(m->originals_offset, nt, sizeof(u32), 4U, bytes)) {
        MESH_FAIL("arrays outside the %u-byte block", m->byte_size);
    }
    if (!vertex_in_range(m->bounds.min) || !vertex_in_range(m->bounds.max)) {
        MESH_FAIL("bounds not finite or beyond the coordinate limit");
    }
    const NphVec3 *v = nph_physics_mesh_vertices(m);
    for (u32 i = 0U; i < nv; ++i) {
        if (!nph_vec3_is_finite(v[i]) || !nph_aabb_contains_point(m->bounds, v[i])) {
            MESH_FAIL("vertex %u not finite or outside the bounds", i);
        }
    }
    const u32 *tri = nph_physics_mesh_triangles(m);
    const u8 *edges = nph_physics_mesh_edges(m);
    for (u64 i = 0U; i < 3ULL * nt; ++i) {
        if (tri[i] >= nv) {
            MESH_FAIL("triangle %u's vertex %u out of range", (u32)(i / 3U), tri[i]);
        }
    }
    for (u32 t = 0U; t < nt; ++t) {
        if ((edges[t] & ~(u32)NPH_PHYSICS_MESH_EDGES) != 0U) {
            MESH_FAIL("triangle %u's edge flags %u", t, edges[t]);
        }
    }
    // The tree, depth first in child order: every node reached once and
    // after its parent, the leaves covering the triangles in order, every
    // box holding what it contains.
    const NphPhysicsMeshNode *nodes = nph_physics_mesh_nodes(m);
    Pending stack[STACK_DEPTH];
    u32 top = 0U;
    stack[top++] = (Pending){0U, NONE, 0U, 0U};
    u32 visited = 0U;
    u32 next_triangle = 0U;
    while (top > 0U) {
        const Pending e = stack[--top];
        if (nph_physics_mesh_child_is_leaf(e.child)) {
            const u32 first = nph_physics_mesh_leaf_first(e.child);
            const u32 count = nph_physics_mesh_leaf_count(e.child);
            if (first != next_triangle || (u64)first + count > nt) {
                MESH_FAIL("leaf %u+%u out of order", first, count);
            }
            for (u32 t = first; t < first + count; ++t) {
                for (u32 j = 0U; j < 3U; ++j) {
                    if (!box_contains(&nodes[e.parent], e.slot, v[tri[3ULL * t + j]])) {
                        MESH_FAIL("triangle %u outside its leaf's box", t);
                    }
                }
            }
            next_triangle = first + count;
            continue;
        }
        const u32 index = e.child;
        if (index >= nn ||
            (e.parent != NONE &&
             (index <= e.parent || !box_contains_node(&nodes[e.parent], e.slot, &nodes[index])))) {
            MESH_FAIL("node %u out of order or outside its parent's box", index);
        }
        visited += 1U;
        if (visited > nn) {
            MESH_FAIL("a node reached twice");
        }
        const NphPhysicsMeshNode *n = &nodes[index];
        u32 used = 0U;
        for (u32 k = 0U; k < 4U; ++k) {
            const u32 c = n->child[k];
            if (c == NPH_PHYSICS_MESH_CHILD_EMPTY) {
                if (!empty_box(n, k)) {
                    MESH_FAIL("node %u: empty slot %u's box is not the empty box", index, k);
                }
                continue;
            }
            if (used != k) {
                MESH_FAIL("node %u: a child after an empty slot", index);
            }
            const NphVec3 lo = nph_vec3(n->min_x[k], n->min_y[k], n->min_z[k]);
            const NphVec3 hi = nph_vec3(n->max_x[k], n->max_y[k], n->max_z[k]);
            if (!vertex_in_range(lo) || !vertex_in_range(hi) || !(lo.x <= hi.x) ||
                !(lo.y <= hi.y) || !(lo.z <= hi.z)) {
                MESH_FAIL("node %u: child %u's box inverted or not finite", index, k);
            }
            used += 1U;
        }
        if (used == 0U) {
            MESH_FAIL("node %u is empty", index);
        }
        if (stack_need(e.depth) > STACK_DEPTH || top + used > STACK_DEPTH) {
            MESH_FAIL("deeper than the query stack");
        }
        // In reverse onto the stack, so the children come off in order.
        for (u32 k = used; k > 0U; --k) {
            stack[top++] = (Pending){n->child[k - 1U], index, k - 1U, e.depth + 1U};
        }
    }
    if (visited != nn || next_triangle != nt) {
        MESH_FAIL("%u of %u nodes reached, %u of %u triangles in leaves", visited, nn,
                  next_triangle, nt);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

// The children of `n` whose boxes overlap `box`, as a mask of four bits.
static u32 overlap_mask(const NphPhysicsMeshNode *n, NphAABB box)
{
    const NphF32x4 a = nph_f32x4_and(
        nph_f32x4_and(
            nph_f32x4_cmp_le(nph_f32x4_load_aligned(n->min_x), nph_f32x4_splat(box.max.x)),
            nph_f32x4_cmp_ge(nph_f32x4_load_aligned(n->max_x), nph_f32x4_splat(box.min.x))),
        nph_f32x4_and(
            nph_f32x4_cmp_le(nph_f32x4_load_aligned(n->min_y), nph_f32x4_splat(box.max.y)),
            nph_f32x4_cmp_ge(nph_f32x4_load_aligned(n->max_y), nph_f32x4_splat(box.min.y))));
    const NphF32x4 b = nph_f32x4_and(
        nph_f32x4_cmp_le(nph_f32x4_load_aligned(n->min_z), nph_f32x4_splat(box.max.z)),
        nph_f32x4_cmp_ge(nph_f32x4_load_aligned(n->max_z), nph_f32x4_splat(box.min.z)));
    return nph_f32x4_movemask(nph_f32x4_and(a, b));
}

void nph_physics_mesh_overlap(const NphPhysicsMesh *m, NphAABB box, NphPhysicsMeshVisit visit,
                              void *user)
{
    const NphPhysicsMeshNode *nodes = nph_physics_mesh_nodes(m);
    const u32 *tri = nph_physics_mesh_triangles(m);
    const NphVec3 *v = nph_physics_mesh_vertices(m);
    u32 stack[STACK_DEPTH]; // child codes: inner nodes and leaves
    u32 top = 0U;
    stack[top++] = 0U;
    while (top > 0U) {
        const u32 code = stack[--top];
        if (!nph_physics_mesh_child_is_leaf(code)) {
            const NphPhysicsMeshNode *n = &nodes[code];
            const u32 mask = overlap_mask(n, box); // empty slots' boxes overlap nothing
            for (u32 k = 4U; k > 0U; --k) {
                if ((mask & (1U << (k - 1U))) != 0U &&
                    n->child[k - 1U] != NPH_PHYSICS_MESH_CHILD_EMPTY) {
                    NPH_ASSERT(top < STACK_DEPTH);
                    stack[top++] = n->child[k - 1U];
                }
            }
            continue;
        }
        // A leaf's box covers up to four triangles: test each.
        const u32 first = nph_physics_mesh_leaf_first(code);
        const u32 last = first + nph_physics_mesh_leaf_count(code);
        for (u32 t = first; t < last; ++t) {
            const NphVec3 a = v[tri[3ULL * t]];
            const NphVec3 b = v[tri[3ULL * t + 1U]];
            const NphVec3 d = v[tri[3ULL * t + 2U]];
            const NphAABB tb = {nph_vec3_min(nph_vec3_min(a, b), d),
                                nph_vec3_max(nph_vec3_max(a, b), d)};
            if (nph_aabb_overlaps(tb, box) && !visit(user, t)) {
                return;
            }
        }
    }
}

// The entry fraction of the ray into each of the four boxes, grown by
// `extent` (a swept box's half extents; zero for a ray), FLT_MAX for a
// miss: the slab test with the reciprocal direction. A grown box's slab
// starts `extent` before its own: the origin moved back instead.
static void slab4(const NphPhysicsMeshNode *n, NphVec3 o, NphVec3 inv, NphVec3 extent, f32 limit,
                  f32 out[4])
{
    const NphF32x4 ox = nph_f32x4_splat(o.x);
    const NphF32x4 oy = nph_f32x4_splat(o.y);
    const NphF32x4 oz = nph_f32x4_splat(o.z);
    const NphF32x4 ex = nph_f32x4_splat(extent.x);
    const NphF32x4 ey = nph_f32x4_splat(extent.y);
    const NphF32x4 ez = nph_f32x4_splat(extent.z);
    const NphF32x4 ix = nph_f32x4_splat(inv.x);
    const NphF32x4 iy = nph_f32x4_splat(inv.y);
    const NphF32x4 iz = nph_f32x4_splat(inv.z);
    const NphF32x4 lo_x = nph_f32x4_sub(nph_f32x4_load_aligned(n->min_x), ex);
    const NphF32x4 hi_x = nph_f32x4_add(nph_f32x4_load_aligned(n->max_x), ex);
    const NphF32x4 lo_y = nph_f32x4_sub(nph_f32x4_load_aligned(n->min_y), ey);
    const NphF32x4 hi_y = nph_f32x4_add(nph_f32x4_load_aligned(n->max_y), ey);
    const NphF32x4 lo_z = nph_f32x4_sub(nph_f32x4_load_aligned(n->min_z), ez);
    const NphF32x4 hi_z = nph_f32x4_add(nph_f32x4_load_aligned(n->max_z), ez);
    const NphF32x4 t1x = nph_f32x4_mul(nph_f32x4_sub(lo_x, ox), ix);
    const NphF32x4 t2x = nph_f32x4_mul(nph_f32x4_sub(hi_x, ox), ix);
    const NphF32x4 t1y = nph_f32x4_mul(nph_f32x4_sub(lo_y, oy), iy);
    const NphF32x4 t2y = nph_f32x4_mul(nph_f32x4_sub(hi_y, oy), iy);
    const NphF32x4 t1z = nph_f32x4_mul(nph_f32x4_sub(lo_z, oz), iz);
    const NphF32x4 t2z = nph_f32x4_mul(nph_f32x4_sub(hi_z, oz), iz);
    const NphF32x4 enter =
        nph_f32x4_max(nph_f32x4_max(nph_f32x4_min(t1x, t2x), nph_f32x4_min(t1y, t2y)),
                      nph_f32x4_max(nph_f32x4_min(t1z, t2z), nph_f32x4_zero()));
    const NphF32x4 leave =
        nph_f32x4_min(nph_f32x4_min(nph_f32x4_max(t1x, t2x), nph_f32x4_max(t1y, t2y)),
                      nph_f32x4_min(nph_f32x4_max(t1z, t2z), nph_f32x4_splat(limit)));
    const u32 hit = nph_f32x4_movemask(nph_f32x4_cmp_le(enter, leave));
    f32 e[4];
    nph_f32x4_store(e, enter);
    for (u32 k = 0U; k < 4U; ++k) {
        out[k] = (hit & (1U << k)) != 0U ? e[k] : FLT_MAX;
    }
}

// A finite reciprocal: a ray parallel to an axis gets a huge one, whose
// products stay ordered (0 * huge is 0, not NaN).
static f32 safe_inverse(f32 d)
{
    if (nph_f32_abs(d) < 1e-30F) {
        return d < 0.0F ? -1e30F : 1e30F;
    }
    return 1.0F / d;
}

bool nph_physics_mesh_raycast(const NphPhysicsMesh *m, NphVec3 origin, NphVec3 delta,
                              NphPhysicsMeshHit *hit)
{
    if (!(nph_vec3_length_sq(delta) > 0.0F)) {
        return false;
    }
    const NphPhysicsMeshNode *nodes = nph_physics_mesh_nodes(m);
    const u32 *tri = nph_physics_mesh_triangles(m);
    const NphVec3 *v = nph_physics_mesh_vertices(m);
    const bool two_sided = (m->flags & NPH_PHYSICS_MESH_TWO_SIDED) != 0U;
    const NphWatertightRay ray = nph_physics_watertight_ray(origin, delta);
    const NphVec3 inv =
        nph_vec3(safe_inverse(delta.x), safe_inverse(delta.y), safe_inverse(delta.z));
    f32 best = FLT_MAX;
    u32 best_triangle = NONE;
    bool best_back = false;
    u32 stack[STACK_DEPTH]; // child codes, with the fraction where the ray enters each
    f32 stack_enter[STACK_DEPTH];
    u32 top = 0U;
    stack[top] = 0U;
    stack_enter[top] = 0.0F;
    top += 1U;
    while (top > 0U) {
        top -= 1U;
        const u32 code = stack[top];
        if (stack_enter[top] > best) {
            continue; // entered past the closest hit so far
        }
        if (nph_physics_mesh_child_is_leaf(code)) {
            const u32 first = nph_physics_mesh_leaf_first(code);
            const u32 last = first + nph_physics_mesh_leaf_count(code);
            for (u32 t = first; t < last; ++t) {
                bool back = false;
                const f32 f = nph_physics_watertight_hit(
                    &ray, v[tri[3ULL * t]], v[tri[3ULL * t + 1U]], v[tri[3ULL * t + 2U]], &back);
                // Equal fractions keep the first found: deterministic.
                if (f >= 0.0F && f < best && (two_sided || !back)) {
                    best = f;
                    best_triangle = t;
                    best_back = back;
                }
            }
            continue;
        }
        const NphPhysicsMeshNode *n = &nodes[code];
        f32 enter[4];
        slab4(n, origin, inv, nph_vec3_zero(), nph_f32_min(best, 1.0F), enter);
        // A slab test passes an inverted box: empty slots are skipped by
        // their code.
        for (u32 k = 0U; k < 4U; ++k) {
            if (n->child[k] == NPH_PHYSICS_MESH_CHILD_EMPTY) {
                enter[k] = FLT_MAX;
            }
        }
        // The farthest first onto the stack, so the nearest comes off next.
        u32 order[4] = {0U, 1U, 2U, 3U};
        for (u32 i = 1U; i < 4U; ++i) {
            for (u32 j = i; j > 0U && enter[order[j]] > enter[order[j - 1U]]; --j) {
                const u32 t = order[j];
                order[j] = order[j - 1U];
                order[j - 1U] = t;
            }
        }
        for (u32 i = 0U; i < 4U; ++i) {
            const u32 k = order[i];
            if (enter[k] < FLT_MAX && enter[k] <= best) {
                NPH_ASSERT(top < STACK_DEPTH);
                stack[top] = n->child[k];
                stack_enter[top] = enter[k];
                top += 1U;
            }
        }
    }
    if (best_triangle == NONE) {
        return false;
    }
    NphVec3 p[3];
    nph_physics_mesh_triangle(m, best_triangle, p);
    const NphVec3 n = nph_vec3_normalize_or_zero(
        nph_vec3_cross(nph_vec3_sub(p[1], p[0]), nph_vec3_sub(p[2], p[0])));
    hit->fraction = best;
    hit->normal = best_back ? nph_vec3_neg(n) : n;
    hit->triangle = best_triangle;
    hit->original = nph_physics_mesh_originals(m)[best_triangle];
    hit->back = best_back ? 1 : 0;
    return true;
}

// The swept box's entry into a triangle's box (grown by `extent`): the
// slab test of the box's centre. FLT_MAX for a miss.
static f32 sweep_triangle_entry(NphVec3 a, NphVec3 b, NphVec3 c, NphVec3 center, NphVec3 extent,
                                NphVec3 delta, f32 limit)
{
    const NphAABB box = {nph_vec3_sub(nph_vec3_min(nph_vec3_min(a, b), c), extent),
                         nph_vec3_add(nph_vec3_max(nph_vec3_max(a, b), c), extent)};
    f32 t = 0.0F;
    return nph_ray_intersects_aabb(nph_ray(center, delta), box, limit, &t, NULL) ? t : FLT_MAX;
}

void nph_physics_mesh_visit_sweep(const NphPhysicsMesh *m, NphVec3 center, NphVec3 extent,
                                  NphVec3 delta, NphDVec3 origin, NphPhysicsSweepVisit visit,
                                  void *user)
{
    const NphPhysicsMeshNode *nodes = nph_physics_mesh_nodes(m);
    const u32 *tri = nph_physics_mesh_triangles(m);
    const u8 *edges = nph_physics_mesh_edges(m);
    const NphVec3 *v = nph_physics_mesh_vertices(m);
    const NphVec3 inv =
        nph_vec3(safe_inverse(delta.x), safe_inverse(delta.y), safe_inverse(delta.z));
    f32 best = 1.0F; // what the visits still want
    u32 stack[STACK_DEPTH];
    f32 stack_enter[STACK_DEPTH];
    u32 top = 0U;
    stack[top] = 0U;
    stack_enter[top] = 0.0F;
    top += 1U;
    while (top > 0U) {
        top -= 1U;
        const u32 code = stack[top];
        if (stack_enter[top] > best) {
            continue;
        }
        if (nph_physics_mesh_child_is_leaf(code)) {
            const u32 first = nph_physics_mesh_leaf_first(code);
            const u32 last = first + nph_physics_mesh_leaf_count(code);
            for (u32 t = first; t < last; ++t) {
                const NphVec3 pa = v[tri[3ULL * t]];
                const NphVec3 pb = v[tri[3ULL * t + 1U]];
                const NphVec3 pc = v[tri[3ULL * t + 2U]];
                if (sweep_triangle_entry(pa, pb, pc, center, extent, delta, best) > best) {
                    continue;
                }
                const NphVec3 rel[3] = {nph_physics_relative_vertex(pa, origin),
                                        nph_physics_relative_vertex(pb, origin),
                                        nph_physics_relative_vertex(pc, origin)};
                const f32 want = visit(user, t, rel, edges[t]);
                if (want < 0.0F) {
                    return;
                }
                best = nph_f32_min(best, want);
            }
            continue;
        }
        const NphPhysicsMeshNode *n = &nodes[code];
        f32 enter[4];
        slab4(n, center, inv, extent, best, enter);
        for (u32 k = 0U; k < 4U; ++k) {
            if (n->child[k] == NPH_PHYSICS_MESH_CHILD_EMPTY) {
                enter[k] = FLT_MAX;
            }
        }
        // The farthest first onto the stack, so the nearest comes off next.
        u32 order[4] = {0U, 1U, 2U, 3U};
        for (u32 i = 1U; i < 4U; ++i) {
            for (u32 j = i; j > 0U && enter[order[j]] > enter[order[j - 1U]]; --j) {
                const u32 s = order[j];
                order[j] = order[j - 1U];
                order[j - 1U] = s;
            }
        }
        for (u32 i = 0U; i < 4U; ++i) {
            const u32 k = order[i];
            if (enter[k] < FLT_MAX && enter[k] <= best) {
                NPH_ASSERT(top < STACK_DEPTH);
                stack[top] = n->child[k];
                stack_enter[top] = enter[k];
                top += 1U;
            }
        }
    }
}
