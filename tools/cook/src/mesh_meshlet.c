// SPDX-License-Identifier: Apache-2.0
// Meshlets (mesh.h): the greedy clusterizer of meshoptimizer (Kapoulkine,
// clusterizer.cpp, meshopt_buildMeshlets), our implementation.
//
// A meshlet grows one triangle at a time. Candidates are the unused
// triangles touching the meshlet's vertices; the topology decides first
// (a triangle adding no vertex, then one that would otherwise be left
// dangling, then by how many vertices it adds), and among equals the
// spatial score decides: distance from the meshlet's centre relative to
// the radius a meshlet of this mesh is expected to have, scaled by how
// far the triangle's normal leans from the meshlet's mean normal
// (`cone_weight`). When the best candidate does not fit, the choice is
// remade on topology alone, because that triangle will start the next
// meshlet. When no candidate touches the meshlet, the unused triangle
// nearest its centre is taken (a k-d tree over triangle centroids, with
// per-node counts of the triangles left so exhausted regions are
// skipped).
//
// Bounds (cook_meshlet_bounds): a Ritter sphere, and a normal cone with
// an apex for the culling test of §8.6.3 (cull when the view direction
// to the apex lies within the cone). docs/cook.md derives why the stored
// snorm8 axis and cutoff keep the test conservative.
#include <nepheline/core/assert.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>

#include <cook/mesh.h>
#include <string.h>

void cook_meshlets_init(CookMeshlets *m, NphAllocator *alloc)
{
    NPH_ASSERT(m != NULL && alloc != NULL);
    NPH_ARRAY_INIT(&m->meshlets, alloc);
    NPH_ARRAY_INIT(&m->bounds, alloc);
    NPH_ARRAY_INIT(&m->vertices, alloc);
    NPH_ARRAY_INIT(&m->triangles, alloc);
}

void cook_meshlets_free(CookMeshlets *m)
{
    if (m == NULL) {
        return;
    }
    NPH_ARRAY_FREE(&m->meshlets);
    NPH_ARRAY_FREE(&m->bounds);
    NPH_ARRAY_FREE(&m->vertices);
    NPH_ARRAY_FREE(&m->triangles);
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

void cook_meshlet_bounds(const NphVec3 *positions, const u32 *vertices, const u8 *triangles,
                         u32 triangle_count, NphMeshletBounds *out)
{
    NPH_ASSERT(positions != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->cone_cutoff = 127; // with a zero axis: never culled
    if (triangle_count == 0U) {
        return;
    }
    NPH_ASSERT(vertices != NULL && triangles != NULL &&
               triangle_count <= NPH_MESHLET_MAX_TRIANGLES);
    const u32 tris = nph_min_u32(triangle_count, NPH_MESHLET_MAX_TRIANGLES);

    // The sphere: Ritter's bounds over the corners (duplicates do not
    // change it).
    NphVec3 points[NPH_MESHLET_MAX_TRIANGLES * 3U];
    for (u32 i = 0; i < tris * 3U; ++i) {
        points[i] = positions[vertices[triangles[i]]];
    }
    const NphSphere sphere = nph_sphere_from_points(points, tris * 3U);
    out->center = sphere.center;
    out->radius = sphere.radius;
    out->cone_apex = sphere.center;

    // The cone axis: the mean of the unit triangle normals, quantised to
    // snorm8 first so every quantity below is computed against the axis
    // the GPU will read.
    NphVec3 normals[NPH_MESHLET_MAX_TRIANGLES];
    NphVec3 axis = nph_vec3_zero();
    for (u32 t = 0; t < tris; ++t) {
        const NphVec3 a = points[(u64)t * 3U];
        const NphVec3 b = points[(u64)t * 3U + 1U];
        const NphVec3 c = points[(u64)t * 3U + 2U];
        normals[t] =
            nph_vec3_normalize_or_zero(nph_vec3_cross(nph_vec3_sub(b, a), nph_vec3_sub(c, a)));
        axis = nph_vec3_add(axis, normals[t]);
    }
    axis = nph_vec3_normalize_or_zero(axis);
    const i8 qx = nph_f32_to_snorm8(axis.x);
    const i8 qy = nph_f32_to_snorm8(axis.y);
    const i8 qz = nph_f32_to_snorm8(axis.z);
    const NphVec3 axis_q =
        nph_vec3(nph_snorm8_to_f32(qx), nph_snorm8_to_f32(qy), nph_snorm8_to_f32(qz));
    const f32 len_q = nph_vec3_length(axis_q);
    // mindp = min over the normals of dot(axis_q, n) = |axis_q| cos(alpha),
    // alpha the half-angle of the normal cone around the axis's direction.
    f32 mindp = NPH_F32_MAX;
    for (u32 t = 0; t < tris; ++t) {
        mindp = nph_f32_min(mindp, nph_vec3_dot(axis_q, normals[t]));
    }
    if (len_q == 0.0F || mindp <= 0.1F * len_q) {
        // Normals spread over (about) a hemisphere or more, or a zero
        // normal: no cone that could ever cull. The zero axis says so.
        return;
    }
    // The apex: on the axis behind the centre, far enough that it lies
    // behind the plane of every triangle: center - axis_q * t with
    // t >= dot(center - p, n) / dot(axis_q, n) for each triangle.
    f32 tmax = 0.0F;
    for (u32 t = 0; t < tris; ++t) {
        const f32 dn = nph_vec3_dot(axis_q, normals[t]); // > 0.1 |axis_q| here
        const f32 dc = nph_vec3_dot(nph_vec3_sub(sphere.center, points[(u64)t * 3U]), normals[t]);
        tmax = nph_f32_max(tmax, dc / dn);
    }
    // The GPU culls when dot(normalize(apex - camera), axis_q) >= cutoff.
    // With a unit axis the bound is sin(alpha); the stored axis has length
    // |axis_q|, which scales the dot product, so the cutoff is
    // |axis_q| sin(alpha) = sqrt(|axis_q|^2 - mindp^2), rounded up.
    const f32 cutoff = nph_f32_sqrt(nph_f32_max(len_q * len_q - mindp * mindp, 0.0F));
    const f32 q = nph_f32_ceil(cutoff * 127.0F);
    if (q >= 127.0F) {
        return; // no stored cutoff below one is conservative: never cull
    }
    out->cone_axis[0] = qx;
    out->cone_axis[1] = qy;
    out->cone_axis[2] = qz;
    out->cone_cutoff = (i8)(i32)q;
    out->cone_apex = nph_vec3_sub(sphere.center, nph_vec3_scale(axis_q, tmax));
}

// ---------------------------------------------------------------------------
// A k-d tree over triangle centroids for the nearest-unused query
// ---------------------------------------------------------------------------

#define KD_LEAF_SIZE 8U
#define KD_LEAF      3U
#define KD_NONE      NPH_U32_MAX

typedef struct KdNode {
    f32 split;
    u32 axis;      // 0..2 for a branch, KD_LEAF for a leaf
    u32 first;     // leaf: first entry of `items`; branch: index of the right child
    u32 count;     // leaf: entries
    u32 parent;    // KD_NONE for the root
    u32 remaining; // unused triangles in the subtree
} KdNode;

typedef NPH_ARRAY(KdNode) KdNodeArray;

typedef struct KdTree {
    KdNodeArray nodes;
    u32 *items;   // triangle ids, grouped by leaf
    u32 *leaf_of; // triangle -> its leaf node
    const NphVec3 *points;
    u32 count;
} KdTree;

typedef struct KdRange {
    u32 first;
    u32 count;
    u32 parent;
    b32 right; // the parent's right child (the left one is always parent + 1)
} KdRange;

// Moves the median of items[first, first + count) along `axis` into the
// middle position (Hoare's selection; deterministic pivots).
static void kd_select(const NphVec3 *points, u32 *items, u32 first, u32 count, u32 axis, u32 nth)
{
    u32 lo = first;
    u32 hi = first + count - 1U;
    while (lo < hi) {
        const f32 pivot = nph_vec3_at(points[items[lo + (hi - lo) / 2U]], axis);
        u32 i = lo;
        u32 j = hi;
        while (i <= j) {
            while (nph_vec3_at(points[items[i]], axis) < pivot) {
                ++i;
            }
            while (nph_vec3_at(points[items[j]], axis) > pivot) {
                --j;
            }
            if (i <= j) {
                const u32 t = items[i];
                items[i] = items[j];
                items[j] = t;
                ++i;
                if (j == 0U) {
                    break;
                }
                --j;
            }
        }
        if (nth <= j) {
            hi = j;
        } else if (nth >= i) {
            lo = i;
        } else {
            break;
        }
    }
}

static NphResult kd_build(NphAllocator *alloc, const NphVec3 *points, u32 count, KdTree *out)
{
    memset(out, 0, sizeof *out);
    NPH_ARRAY_INIT(&out->nodes, alloc);
    out->points = points;
    out->count = count;
    out->items = NPH_ALLOC_ARRAY(alloc, u32, nph_max_u32(count, 1U));
    out->leaf_of = NPH_ALLOC_ARRAY(alloc, u32, nph_max_u32(count, 1U));
    NphResult r =
        out->items != NULL && out->leaf_of != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r != NPH_RESULT_OK || count == 0U) {
        return r;
    }
    for (u32 i = 0; i < count; ++i) {
        out->items[i] = i;
    }
    // Built depth first without recursion: a branch is followed by its
    // left subtree, then its right one, whose index the branch records.
    typedef NPH_ARRAY(KdRange) KdRangeArray;
    KdRangeArray stack;
    NPH_ARRAY_INIT(&stack, alloc);
    const KdRange root = {0U, count, KD_NONE, 0};
    r = NPH_ARRAY_PUSH(&stack, root);
    while (r == NPH_RESULT_OK && stack.len != 0U) {
        const KdRange range = NPH_ARRAY_POP(&stack);
        KdNode node;
        memset(&node, 0, sizeof node);
        node.parent = range.parent;
        node.remaining = range.count;
        const u32 index = out->nodes.len;
        if (range.right) {
            out->nodes.data[range.parent].first = index;
        }
        // The axis of the largest extent.
        NphVec3 lo = points[out->items[range.first]];
        NphVec3 hi = lo;
        for (u32 i = 1; i < range.count; ++i) {
            lo = nph_vec3_min(lo, points[out->items[range.first + i]]);
            hi = nph_vec3_max(hi, points[out->items[range.first + i]]);
        }
        const NphVec3 extent = nph_vec3_sub(hi, lo);
        u32 axis = extent.x >= extent.y && extent.x >= extent.z ? 0U
                   : extent.y >= extent.z                       ? 1U
                                                                : 2U;
        if (range.count <= KD_LEAF_SIZE || nph_vec3_at(extent, axis) == 0.0F) {
            node.axis = KD_LEAF;
            node.first = range.first;
            node.count = range.count;
            for (u32 i = 0; i < range.count; ++i) {
                out->leaf_of[out->items[range.first + i]] = index;
            }
            r = NPH_ARRAY_PUSH(&out->nodes, node);
            continue;
        }
        const u32 half = range.count / 2U;
        kd_select(points, out->items, range.first, range.count, axis, range.first + half);
        node.axis = axis;
        node.split = nph_vec3_at(points[out->items[range.first + half]], axis);
        node.first = KD_NONE; // set when the right child is created
        r = NPH_ARRAY_PUSH(&out->nodes, node);
        // The right range goes on the stack first so the left one is
        // built next, at index + 1.
        const KdRange right = {range.first + half, range.count - half, index, 1};
        const KdRange left = {range.first, half, index, 0};
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&stack, right);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&stack, left);
        }
    }
    NPH_ARRAY_FREE(&stack);
    return r;
}

static void kd_free(NphAllocator *alloc, KdTree *t)
{
    NPH_ARRAY_FREE(&t->nodes);
    if (t->items != NULL) {
        NPH_FREE_ARRAY(alloc, u32, t->items, nph_max_u32(t->count, 1U));
    }
    if (t->leaf_of != NULL) {
        NPH_FREE_ARRAY(alloc, u32, t->leaf_of, nph_max_u32(t->count, 1U));
    }
}

static void kd_remove(KdTree *t, u32 item)
{
    for (u32 n = t->leaf_of[item]; n != KD_NONE; n = t->nodes.data[n].parent) {
        NPH_ASSERT(t->nodes.data[n].remaining > 0U);
        t->nodes.data[n].remaining -= 1U;
    }
}

// The unused item nearest `p` (KD_NONE when every item is used).
static u32 kd_nearest(const KdTree *t, const u8 *used, NphVec3 p, NphArrayU32 *stack)
{
    u32 best = KD_NONE;
    f32 best_d2 = NPH_F32_MAX;
    NPH_ARRAY_CLEAR(stack);
    if (t->nodes.len == 0U || t->nodes.data[0].remaining == 0U ||
        NPH_ARRAY_PUSH(stack, 0U) != NPH_RESULT_OK) {
        return KD_NONE;
    }
    while (stack->len != 0U) {
        const u32 n = NPH_ARRAY_POP(stack);
        const KdNode *node = &t->nodes.data[n];
        if (node->remaining == 0U) {
            continue;
        }
        if (node->axis == KD_LEAF) {
            for (u32 i = 0; i < node->count; ++i) {
                const u32 item = t->items[node->first + i];
                if (used[item] != 0U) {
                    continue;
                }
                const f32 d2 = nph_vec3_distance_sq(t->points[item], p);
                if (d2 < best_d2 || (d2 == best_d2 && item < best)) {
                    best_d2 = d2;
                    best = item;
                }
            }
            continue;
        }
        const f32 delta = nph_vec3_at(p, node->axis) - node->split;
        const u32 left = n + 1U;
        const u32 right = node->first;
        const u32 near_child = delta <= 0.0F ? left : right;
        const u32 far_child = delta <= 0.0F ? right : left;
        // The far side can only hold a closer item within |delta| of the
        // plane; it is pushed first so the near side is searched first.
        if (delta * delta < best_d2 && NPH_ARRAY_PUSH(stack, far_child) != NPH_RESULT_OK) {
            return best;
        }
        if (NPH_ARRAY_PUSH(stack, near_child) != NPH_RESULT_OK) {
            return best;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// The builder
// ---------------------------------------------------------------------------

typedef struct Builder {
    NphAllocator *alloc;
    const CookMesh *mesh;
    CookMeshlets *out;
    u32 max_vertices;
    u32 max_triangles;
    f32 cone_weight;
    // Per mesh vertex: its slot in the current meshlet (0xFF when absent),
    // and its run of the submesh's adjacency (unused triangles only).
    u8 *local;
    u32 *adj_offset;
    u32 *adj_count; // the triangles still unused around the vertex
    // Per triangle of the submesh being built.
    u32 *adj; // triangle ids, by vertex
    NphVec3 *centroid;
    NphVec3 *normal;
    u8 *used;
    // The meshlet being built.
    NphMeshlet cur;
    u32 cur_vertices[NPH_MESHLET_MAX_VERTICES];
    NphVec3 center_sum;
    NphVec3 normal_sum;
    f32 expected_radius;
    NphArrayU32 stack; // scratch for the k-d tree
} Builder;

static const u32 *submesh_tri(const Builder *b, const CookSubmesh *sub, u32 t)
{
    return &b->mesh->indices.data[sub->index_offset + t * 3U];
}

static u32 new_vertices(const Builder *b, const u32 *tri)
{
    return (b->local[tri[0]] == 0xFFU ? 1U : 0U) + (b->local[tri[1]] == 0xFFU ? 1U : 0U) +
           (b->local[tri[2]] == 0xFFU ? 1U : 0U);
}

// The meshoptimizer priority: lower is better.
static u32 topology_priority(const Builder *b, const u32 *tri, u32 extra)
{
    if (extra == 0U) {
        return 0U; // adds no vertex
    }
    const u32 la = b->adj_count[tri[0]];
    const u32 lb = b->adj_count[tri[1]];
    const u32 lc = b->adj_count[tri[2]];
    if (la == 1U || lb == 1U || lc == 1U) {
        return 1U; // would be left dangling, expensive to place later
    }
    if ((la == 2U ? 1U : 0U) + (lb == 2U ? 1U : 0U) + (lc == 2U ? 1U : 0U) >= 2U) {
        return 1U + extra; // makes another triangle dangling, good for the flow
    }
    return 2U + extra;
}

// The best unused neighbour of the current meshlet, or NPH_U32_MAX. With
// `spatial` false the tie-break is topological (fewest triangles left
// around the corners) for a triangle that will start the next meshlet.
static u32 best_neighbor(const Builder *b, const CookSubmesh *sub, bool spatial)
{
    u32 best = NPH_U32_MAX;
    u32 best_priority = 5U;
    f32 best_score = NPH_F32_MAX;
    const f32 n = (f32)nph_max_u32(b->cur.triangle_count, 1U);
    const NphVec3 center = nph_vec3_scale(b->center_sum, 1.0F / n);
    const NphVec3 mean_normal = nph_vec3_normalize_or_zero(b->normal_sum);
    for (u32 i = 0; i < b->cur.vertex_count; ++i) {
        const u32 v = b->cur_vertices[i];
        for (u32 j = 0; j < b->adj_count[v]; ++j) {
            const u32 t = b->adj[b->adj_offset[v] + j];
            const u32 *tri = submesh_tri(b, sub, t);
            const u32 extra = new_vertices(b, tri);
            const u32 priority = topology_priority(b, tri, extra);
            if (priority > best_priority) {
                continue; // topology outranks any score
            }
            f32 score;
            if (spatial) {
                const f32 distance = nph_vec3_distance(b->centroid[t], center);
                const f32 spread = nph_vec3_dot(b->normal[t], mean_normal);
                const f32 cone = nph_f32_max(1.0F - spread * b->cone_weight, 1e-3F);
                score = (1.0F + distance / b->expected_radius * (1.0F - b->cone_weight)) * cone;
            } else {
                score = (f32)(b->adj_count[tri[0]] + b->adj_count[tri[1]] + b->adj_count[tri[2]]);
            }
            if (priority < best_priority || score < best_score) {
                best = t;
                best_priority = priority;
                best_score = score;
            }
        }
    }
    return best;
}

// Closes the current meshlet: its vertices go to MLVX, its bounds are
// computed over the mesh's positions, and the slots are released.
static NphResult finish_meshlet(Builder *b, const NphVec3 *positions, u32 submesh)
{
    if (b->cur.triangle_count == 0U) {
        return NPH_RESULT_OK;
    }
    NphMeshlet m = b->cur;
    m.vertex_offset = b->out->vertices.len;
    NPH_TRY(NPH_ARRAY_PUSH_N(&b->out->vertices, b->cur_vertices, b->cur.vertex_count));
    NphMeshletBounds bounds;
    cook_meshlet_bounds(positions, b->out->vertices.data + m.vertex_offset,
                        b->out->triangles.data + (u64)m.triangle_offset * 3U, m.triangle_count,
                        &bounds);
    NPH_TRY(NPH_ARRAY_PUSH(&b->out->meshlets, m));
    NPH_TRY(NPH_ARRAY_PUSH(&b->out->bounds, bounds));
    for (u32 i = 0; i < b->cur.vertex_count; ++i) {
        b->local[b->cur_vertices[i]] = 0xFFU;
    }
    memset(&b->cur, 0, sizeof b->cur);
    b->cur.submesh = submesh;
    b->cur.triangle_offset = b->out->triangles.len / 3U;
    b->center_sum = nph_vec3_zero();
    b->normal_sum = nph_vec3_zero();
    return NPH_RESULT_OK;
}

// Adds triangle `t` to the current meshlet, closing it first when the
// triangle does not fit; marks it used everywhere.
static NphResult add_triangle(Builder *b, const NphVec3 *positions, const CookSubmesh *sub,
                              u32 submesh, KdTree *kd, u32 t)
{
    const u32 *tri = submesh_tri(b, sub, t);
    if (b->cur.vertex_count + new_vertices(b, tri) > b->max_vertices ||
        b->cur.triangle_count >= b->max_triangles) {
        NPH_TRY(finish_meshlet(b, positions, submesh));
    }
    for (u32 k = 0; k < 3U; ++k) {
        const u32 v = tri[k];
        if (b->local[v] == 0xFFU) {
            b->local[v] = (u8)b->cur.vertex_count;
            b->cur_vertices[b->cur.vertex_count] = v;
            b->cur.vertex_count += 1U;
        }
        NPH_TRY(NPH_ARRAY_PUSH(&b->out->triangles, b->local[v]));
        // Out of the vertex's adjacency: the lists hold unused triangles.
        u32 *list = b->adj + b->adj_offset[v];
        for (u32 j = 0; j < b->adj_count[v]; ++j) {
            if (list[j] == t) {
                list[j] = list[b->adj_count[v] - 1U];
                b->adj_count[v] -= 1U;
                break;
            }
        }
    }
    b->cur.triangle_count += 1U;
    b->center_sum = nph_vec3_add(b->center_sum, b->centroid[t]);
    b->normal_sum = nph_vec3_add(b->normal_sum, b->normal[t]);
    b->used[t] = 1U;
    kd_remove(kd, t);
    return NPH_RESULT_OK;
}

static NphResult build_submesh(Builder *b, const NphVec3 *positions, u32 s)
{
    const CookSubmesh *sub = &b->mesh->submeshes.data[s];
    const u32 tri_count = sub->index_count / 3U;
    memset(&b->cur, 0, sizeof b->cur);
    b->cur.submesh = s;
    b->cur.triangle_offset = b->out->triangles.len / 3U;
    b->center_sum = nph_vec3_zero();
    b->normal_sum = nph_vec3_zero();
    if (tri_count == 0U) {
        return NPH_RESULT_OK;
    }
    NphAllocator *alloc = b->alloc;
    b->adj = NPH_ALLOC_ARRAY(alloc, u32, (u64)tri_count * 3U);
    b->centroid = NPH_ALLOC_ARRAY(alloc, NphVec3, tri_count);
    b->normal = NPH_ALLOC_ARRAY(alloc, NphVec3, tri_count);
    b->used = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, tri_count);
    KdTree kd;
    memset(&kd, 0, sizeof kd);
    NphResult r = b->adj != NULL && b->centroid != NULL && b->normal != NULL && b->used != NULL
                      ? NPH_RESULT_OK
                      : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        // Adjacency over the submesh's vertices (a counting sort that
        // touches only the vertices it references).
        for (u32 t = 0; t < tri_count; ++t) {
            const u32 *tri = submesh_tri(b, sub, t);
            for (u32 k = 0; k < 3U; ++k) {
                b->adj_count[tri[k]] = 0U;
                b->adj_offset[tri[k]] = NPH_U32_MAX;
            }
        }
        for (u32 t = 0; t < tri_count; ++t) {
            const u32 *tri = submesh_tri(b, sub, t);
            for (u32 k = 0; k < 3U; ++k) {
                b->adj_count[tri[k]] += 1U;
            }
        }
        u32 offset = 0U;
        for (u32 t = 0; t < tri_count; ++t) {
            const u32 *tri = submesh_tri(b, sub, t);
            for (u32 k = 0; k < 3U; ++k) {
                if (b->adj_offset[tri[k]] == NPH_U32_MAX) {
                    b->adj_offset[tri[k]] = offset;
                    offset += b->adj_count[tri[k]];
                    b->adj_count[tri[k]] = 0U;
                }
            }
        }
        f32 area_sum = 0.0F;
        for (u32 t = 0; t < tri_count; ++t) {
            const u32 *tri = submesh_tri(b, sub, t);
            for (u32 k = 0; k < 3U; ++k) {
                b->adj[b->adj_offset[tri[k]] + b->adj_count[tri[k]]] = t;
                b->adj_count[tri[k]] += 1U;
            }
            const NphVec3 p0 = positions[tri[0]];
            const NphVec3 p1 = positions[tri[1]];
            const NphVec3 p2 = positions[tri[2]];
            const NphVec3 cross = nph_vec3_cross(nph_vec3_sub(p1, p0), nph_vec3_sub(p2, p0));
            const f32 len = nph_vec3_length(cross);
            area_sum += len * 0.5F;
            b->normal[t] = len > 0.0F ? nph_vec3_scale(cross, 1.0F / len) : nph_vec3_zero();
            b->centroid[t] = nph_vec3_scale(nph_vec3_add(nph_vec3_add(p0, p1), p2), 1.0F / 3.0F);
        }
        // A meshlet of this mesh is expected to cover about max_triangles
        // average triangles, roughly a disc: the scale of the distance term.
        b->expected_radius = nph_f32_max(
            nph_f32_sqrt(area_sum / (f32)tri_count * (f32)b->max_triangles) * 0.5F, 1e-20F);
        r = kd_build(alloc, b->centroid, tri_count, &kd);
    }
    u32 cursor = 0U; // the seed of the first meshlet: the first unused triangle
    u32 emitted = 0U;
    while (r == NPH_RESULT_OK && emitted < tri_count) {
        u32 t = NPH_U32_MAX;
        if (b->cur.triangle_count != 0U) {
            t = best_neighbor(b, sub, true);
            if (t != NPH_U32_MAX) {
                const u32 *tri = submesh_tri(b, sub, t);
                if (b->cur.vertex_count + new_vertices(b, tri) > b->max_vertices ||
                    b->cur.triangle_count >= b->max_triangles) {
                    // It starts the next meshlet: choose that start by topology.
                    t = best_neighbor(b, sub, false);
                }
            } else {
                const NphVec3 center =
                    nph_vec3_scale(b->center_sum, 1.0F / (f32)b->cur.triangle_count);
                t = kd_nearest(&kd, b->used, center, &b->stack);
            }
        }
        if (t == NPH_U32_MAX) {
            while (b->used[cursor] != 0U) {
                ++cursor;
            }
            t = cursor;
        }
        r = add_triangle(b, positions, sub, s, &kd, t);
        ++emitted;
    }
    if (r == NPH_RESULT_OK) {
        r = finish_meshlet(b, positions, s);
    }
    kd_free(alloc, &kd);
    if (b->adj != NULL) {
        NPH_FREE_ARRAY(alloc, u32, b->adj, (u64)tri_count * 3U);
    }
    if (b->centroid != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec3, b->centroid, tri_count);
    }
    if (b->normal != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec3, b->normal, tri_count);
    }
    if (b->used != NULL) {
        NPH_FREE_ARRAY(alloc, u8, b->used, tri_count);
    }
    b->adj = NULL;
    b->centroid = NULL;
    b->normal = NULL;
    b->used = NULL;
    return r;
}

NphResult cook_mesh_build_meshlets(const CookMesh *mesh, u32 max_vertices, u32 max_triangles,
                                   f32 cone_weight, CookMeshlets *out)
{
    NPH_ASSERT(mesh != NULL && out != NULL);
    NPH_TRY(cook_mesh_validate(mesh));
    if (max_vertices == 0U) {
        max_vertices = NPH_MESHLET_MAX_VERTICES;
    }
    if (max_triangles == 0U) {
        max_triangles = NPH_MESHLET_MAX_TRIANGLES;
    }
    if (max_vertices < 3U || max_vertices > NPH_MESHLET_MAX_VERTICES || max_triangles < 1U ||
        max_triangles > NPH_MESHLET_MAX_TRIANGLES ||
        !(cone_weight >= 0.0F && cone_weight <= 1.0F)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 n = nph_max_u32(mesh->vertices.len, 1U);
    Builder b;
    memset(&b, 0, sizeof b);
    b.alloc = mesh->alloc;
    b.mesh = mesh;
    b.out = out;
    b.max_vertices = max_vertices;
    b.max_triangles = max_triangles;
    b.cone_weight = cone_weight;
    b.local = NPH_ALLOC_ARRAY(mesh->alloc, u8, n);
    b.adj_offset = NPH_ALLOC_ARRAY(mesh->alloc, u32, n);
    b.adj_count = NPH_ALLOC_ARRAY(mesh->alloc, u32, n);
    NPH_ARRAY_INIT(&b.stack, mesh->alloc);
    NphVec3 *positions = NPH_ALLOC_ARRAY(mesh->alloc, NphVec3, n);
    NphResult r =
        b.local != NULL && b.adj_offset != NULL && b.adj_count != NULL && positions != NULL
            ? NPH_RESULT_OK
            : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        memset(b.local, 0xFF, (size_t)n);
        for (u32 v = 0; v < mesh->vertices.len; ++v) {
            positions[v] = mesh->vertices.data[v].position;
        }
    }
    for (u32 s = 0; s < mesh->submeshes.len && r == NPH_RESULT_OK; ++s) {
        r = build_submesh(&b, positions, s);
    }
    NPH_ARRAY_FREE(&b.stack);
    if (positions != NULL) {
        NPH_FREE_ARRAY(mesh->alloc, NphVec3, positions, n);
    }
    if (b.local != NULL) {
        NPH_FREE_ARRAY(mesh->alloc, u8, b.local, n);
    }
    if (b.adj_offset != NULL) {
        NPH_FREE_ARRAY(mesh->alloc, u32, b.adj_offset, n);
    }
    if (b.adj_count != NULL) {
        NPH_FREE_ARRAY(mesh->alloc, u32, b.adj_count, n);
    }
    return r;
}
