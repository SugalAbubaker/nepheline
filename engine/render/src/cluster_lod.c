// SPDX-License-Identifier: Apache-2.0
// Cluster levels of detail (render/cluster_lod.h; docs/render.md "Levels of
// detail"; ADR-0060): the levels, their groups, and the hierarchy over them.
//
// Level by level (Cignoni et al., "Batched Multi Triangulation", 2005; Karis
// et al., "Nanite: A Deep Dive", SIGGRAPH 2021):
//
//   1. The level's clusters are partitioned into groups of `group_size`
//      neighbours: grown greedily from seeds in Morton order, each step
//      taking the cluster that shares the most vertices with the group, so
//      that groups are compact and their boundaries short (a short boundary
//      leaves more of the group free to simplify); a cluster sharing none
//      (foliage, debris) joins the nearest group that has room nearby.
//   2. The classes a group shares with another are locked: no collapse moves
//      them, so the group's simplified triangles meet its neighbours' (at
//      this level or the one below) exactly.
//   3. Each group is simplified to half its triangles (cluster_simplify.c)
//      and split into clusters again with the meshlet builder, in the
//      group's own compact numbering of the mesh's vertices.
//
// A group's error is the largest of its simplification's and of its
// clusters' own (those of the groups that made them), and its sphere bounds
// theirs: the projected error then never falls going up the levels, so one
// threshold cuts the levels in one consistent place. The levels stop when one
// cluster is left, or when a level takes off less than a tenth of the
// triangles (the rest cannot be simplified further without breaking a rule:
// disconnected pieces smaller than any collapse, say).
//
// The coarsest level's clusters are grouped as any level's, into root
// groups whose error is the largest float (always too coarse, so their
// clusters are drawn wherever their makers allow): a level that stops
// shrinking (foliage, scattered pieces) may leave hundreds of clusters, and
// groups of about eight keep them culled, skipped, and handed on a few at a
// time rather than all together.
//
// The hierarchy: each level's groups, and the root groups, sorted in Morton
// order of their spheres' centres, under a tree of
// NPH_RENDER_LOD_NODE_CHILDREN children a node; the trees under the root
// node.
#include <nepheline/core/assert.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>

#include "cluster_lod_internal.h"

#include <stdlib.h>
#include <string.h>

// A level stops the levels when its triangles are more than this share of
// the level before's.
#define STALL_RATIO 0.9F
// A cluster that shares no vertex with a group joins it only from within
// this many of the group's radii (plus its own).
#define NEAR_RADII 2.0F
// How far ahead in Morton order the nearest such cluster is looked for.
#define NEAR_WINDOW 64U

void nph_render_cluster_lods_init(NphRenderClusterLods *l, NphAllocator *alloc)
{
    NPH_ASSERT(l != NULL && alloc != NULL);
    memset(l, 0, sizeof *l);
    nph_render_meshlets_init(&l->clusters, alloc);
    NPH_ARRAY_INIT(&l->lods, alloc);
    NPH_ARRAY_INIT(&l->groups, alloc);
    NPH_ARRAY_INIT(&l->group_clusters, alloc);
    NPH_ARRAY_INIT(&l->nodes, alloc);
}

void nph_render_cluster_lods_free(NphRenderClusterLods *l)
{
    if (l == NULL) {
        return;
    }
    nph_render_meshlets_free(&l->clusters);
    NPH_ARRAY_FREE(&l->lods);
    NPH_ARRAY_FREE(&l->groups);
    NPH_ARRAY_FREE(&l->group_clusters);
    NPH_ARRAY_FREE(&l->nodes);
}

f32 nph_render_lod_projected_error(NphVec3 center, f32 radius, f32 error, f32 scale, NphVec3 camera,
                                   f32 pixels_per_unit, f32 znear)
{
    const f32 d = nph_vec3_distance(center, camera) - radius * scale;
    return error * scale * pixels_per_unit / nph_f32_max(d, znear);
}

// The smallest sphere nph_sphere_union grows over `spheres` (`count`, at
// least one), its radius then raised until it holds each of them in double
// precision: the renderer's tests bound a sphere's parts by it
// (vis_lod_node_too_coarse, vis_lod_surely_too_coarse), and a union taken
// in single precision may fall short of a part by a rounding.
static NphSphere sphere_hull(const NphSphere *spheres, u32 count)
{
    NPH_ASSERT(spheres != NULL && count != 0U);
    NphSphere hull = spheres[0];
    for (u32 i = 1; i < count; ++i) {
        hull = nph_sphere_union(hull, spheres[i]);
    }
    f64 radius = (f64)hull.radius;
    for (u32 i = 0; i < count; ++i) {
        const f64 dx = (f64)spheres[i].center.x - (f64)hull.center.x;
        const f64 dy = (f64)spheres[i].center.y - (f64)hull.center.y;
        const f64 dz = (f64)spheres[i].center.z - (f64)hull.center.z;
        const f64 reach = nph_f64_sqrt(dx * dx + dy * dy + dz * dz) + (f64)spheres[i].radius;
        radius = reach > radius ? reach : radius;
    }
    f32 r = (f32)radius;
    while ((f64)r < radius) {
        r = nph_f32_from_bits(nph_f32_to_bits(r) + 1U);
    }
    hull.radius = r;
    return hull;
}

// ---------------------------------------------------------------------------
// Morton order
// ---------------------------------------------------------------------------

static u32 spread10(u32 x)
{
    x &= 0x3FFU;
    x = (x | (x << 16U)) & 0x030000FFU;
    x = (x | (x << 8U)) & 0x0300F00FU;
    x = (x | (x << 4U)) & 0x030C30C3U;
    x = (x | (x << 2U)) & 0x09249249U;
    return x;
}

// A point's cell of 1024 along an axis, from its offset times the axis's
// reciprocal extent. A subnormal extent's reciprocal is infinite: the
// lowest point's 0 times it is NaN, which goes to cell 0 with everything
// not above 0, as the conversion of a NaN to an integer is undefined.
static u32 morton_cell(f32 offset, f32 inv_extent)
{
    const f32 t = offset * inv_extent * 1023.0F;
    if (!(t > 0.0F)) {
        return 0U;
    }
    return (u32)nph_f32_min(t, 1023.0F);
}

static u32 morton(NphVec3 p, NphVec3 lo, NphVec3 inv_extent)
{
    const u32 x = morton_cell(p.x - lo.x, inv_extent.x);
    const u32 y = morton_cell(p.y - lo.y, inv_extent.y);
    const u32 z = morton_cell(p.z - lo.z, inv_extent.z);
    return spread10(x) | (spread10(y) << 1U) | (spread10(z) << 2U);
}

typedef struct Ranked {
    u32 code;
    u32 item;
} Ranked;

static int compare_ranked(const void *a, const void *b)
{
    const Ranked *x = (const Ranked *)a;
    const Ranked *y = (const Ranked *)b;
    if (x->code != y->code) {
        return x->code < y->code ? -1 : 1;
    }
    return x->item < y->item ? -1 : (x->item > y->item ? 1 : 0);
}

// `count` items' points in Morton order: `out` (count entries) takes the
// items, the points' bounds normalised.
static void morton_order(const NphVec3 *points, u32 count, Ranked *out)
{
    if (count == 0U) {
        return;
    }
    NphVec3 lo = points[0];
    NphVec3 hi = points[0];
    for (u32 i = 1U; i < count; ++i) {
        lo = nph_vec3_min(lo, points[i]);
        hi = nph_vec3_max(hi, points[i]);
    }
    const NphVec3 extent = nph_vec3_sub(hi, lo);
    const NphVec3 inv =
        nph_vec3(extent.x > 0.0F ? 1.0F / extent.x : 0.0F, extent.y > 0.0F ? 1.0F / extent.y : 0.0F,
                 extent.z > 0.0F ? 1.0F / extent.z : 0.0F);
    for (u32 i = 0; i < count; ++i) {
        out[i].code = morton(points[i], lo, inv);
        out[i].item = i;
    }
    qsort(out, count, sizeof(Ranked), compare_ranked);
}

// ---------------------------------------------------------------------------
// The builder's state
// ---------------------------------------------------------------------------

// A grid of triangles for nearest-triangle queries ("The measured error" below).
typedef struct LevelGrid {
    NphVec3 lo;
    f32 cell;
    u32 n[3];
    u32 *start; // n0 n1 n2 + 1 runs into items
    u64 cells;
    NphArrayU32 items;
    const NphVec3 *positions;
    const u32 *tris; // three vertices a triangle: `owned`'s, or the mesh's indices
    u32 tri_count;
    NphArrayU32 owned;
} LevelGrid;

static NphVec3 grid_corner(const LevelGrid *g, u32 t, u32 k)
{
    NPH_ASSERT(g->tris != NULL && g->positions != NULL);
    return g->positions[g->tris[(u64)t * 3U + k]];
}

typedef struct Build {
    const NphRenderClusterLodInput *in;
    NphAllocator *alloc; // scratch
    NphRenderClusterLods *out;
    LodMesh mesh;
    u32 max_vertices;
    u32 max_triangles;
    u32 group_size;
    // Per cluster (all levels): the sphere its own error is projected from
    // and that error (its maker's; level 0: its bounds, 0).
    NPH_ARRAY(NphSphere) own_sphere;
    NphArrayF32 own_error;
    // Per class: the group of the level that last took it, and whether two
    // groups share it (scratch, NPH_U32_MAX / 0 between levels).
    u32 *class_group;
    u8 *locked;
    f32 *class_dist;      // per class: its points' measured distance this level (< 0: unknown)
    u32 *class_tri_count; // per class: scratch for its triangles in a level's result (0 between)
    u32 *class_tri_start;
    LevelGrid ground; // level 0's triangles: the mesh the levels are measured back to
} Build;

typedef NPH_ARRAY(Ranked) RankedArray;

// One cluster's classes, appended to `out` (duplicates kept).
static NphResult cluster_classes(const Build *b, u32 cluster, NphArrayU32 *out)
{
    const NphRenderMeshlet *m = &b->out->clusters.meshlets.data[cluster];
    for (u32 i = 0; i < m->vertex_count; ++i) {
        const u32 v = b->out->clusters.vertices.data[m->vertex_offset + i];
        NPH_TRY(NPH_ARRAY_PUSH(out, b->mesh.class_of[v]));
    }
    return NPH_RESULT_OK;
}

typedef struct AdjEdge {
    u32 a;
    u32 b;
} AdjEdge;

static int compare_adj(const void *x, const void *y)
{
    const AdjEdge *p = (const AdjEdge *)x;
    const AdjEdge *q = (const AdjEdge *)y;
    if (p->a != q->a) {
        return p->a < q->a ? -1 : 1;
    }
    return p->b < q->b ? -1 : (p->b > q->b ? 1 : 0);
}

typedef struct ClassEntry {
    u32 cls;
    u32 cluster; // local to the level
} ClassEntry;

static int compare_class_entries(const void *x, const void *y)
{
    const ClassEntry *p = (const ClassEntry *)x;
    const ClassEntry *q = (const ClassEntry *)y;
    if (p->cls != q->cls) {
        return p->cls < q->cls ? -1 : 1;
    }
    return p->cluster < q->cluster ? -1 : (p->cluster > q->cluster ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Partition
// ---------------------------------------------------------------------------

// The level's clusters [first, first + n) into groups: `group_of` (n
// entries, local) takes each cluster's group, and the groups' count is
// returned through `out_groups`.
static NphResult partition(Build *b, u32 first, u32 n, u32 *group_of, u32 *out_groups)
{
    NphAllocator *a = b->alloc;
    *out_groups = 0U;
    // Clusters sharing a class are adjacent; a pair's weight is how many
    // classes they share.
    NPH_ARRAY(ClassEntry) entries;
    NPH_ARRAY_INIT(&entries, a);
    NphArrayU32 classes;
    NPH_ARRAY_INIT(&classes, a);
    NPH_ARRAY(AdjEdge) pairs;
    NPH_ARRAY_INIT(&pairs, a);
    NphArrayU32 adj_start;
    NPH_ARRAY_INIT(&adj_start, a);
    NphArrayU32 adj;
    NPH_ARRAY_INIT(&adj, a);
    NphArrayU32 adj_weight;
    NPH_ARRAY_INIT(&adj_weight, a);
    NphVec3 *centers = NPH_ALLOC_ARRAY(a, NphVec3, nph_max_u32(n, 1U));
    f32 *radii = NPH_ALLOC_ARRAY(a, f32, nph_max_u32(n, 1U));
    Ranked *order = NPH_ALLOC_ARRAY(a, Ranked, nph_max_u32(n, 1U));
    u32 *rank_of = NPH_ALLOC_ARRAY(a, u32, nph_max_u32(n, 1U));
    u32 *gain = NPH_ALLOC_ARRAY_ZEROED(a, u32, nph_max_u32(n, 1U));
    NphArrayU32 members;
    NPH_ARRAY_INIT(&members, a);
    NphArrayU32 frontier;
    NPH_ARRAY_INIT(&frontier, a);
    NphResult r =
        centers != NULL && radii != NULL && order != NULL && rank_of != NULL && gain != NULL
            ? NPH_RESULT_OK
            : NPH_RESULT_OUT_OF_MEMORY;
    for (u32 i = 0; i < n && r == NPH_RESULT_OK; ++i) {
        const NphRenderMeshletBounds *bd = &b->out->clusters.bounds.data[first + i];
        centers[i] = bd->center;
        radii[i] = bd->radius;
        group_of[i] = NPH_U32_MAX;
        NPH_ARRAY_CLEAR(&classes);
        r = cluster_classes(b, first + i, &classes);
        for (u32 k = 0; k < classes.len && r == NPH_RESULT_OK; ++k) {
            const ClassEntry e = {classes.data[k], i};
            r = NPH_ARRAY_PUSH(&entries, e);
        }
    }
    if (r == NPH_RESULT_OK && entries.len > 1U) {
        qsort(entries.data, entries.len, sizeof(ClassEntry), compare_class_entries);
    }
    for (u32 i = 0; i < entries.len && r == NPH_RESULT_OK;) {
        u32 j = i;
        while (j < entries.len && entries.data[j].cls == entries.data[i].cls) {
            ++j;
        }
        for (u32 x = i; x < j && r == NPH_RESULT_OK; ++x) {
            for (u32 y = x + 1U; y < j && r == NPH_RESULT_OK; ++y) {
                const u32 p = entries.data[x].cluster;
                const u32 q = entries.data[y].cluster;
                if (p == q) {
                    continue; // a cluster lists a class once a vertex
                }
                const AdjEdge e0 = {p, q};
                const AdjEdge e1 = {q, p};
                r = NPH_ARRAY_PUSH(&pairs, e0);
                if (r == NPH_RESULT_OK) {
                    r = NPH_ARRAY_PUSH(&pairs, e1);
                }
            }
        }
        i = j;
    }
    if (r == NPH_RESULT_OK && pairs.len > 1U) {
        qsort(pairs.data, pairs.len, sizeof(AdjEdge), compare_adj);
    }
    // Adjacency lists with weights: runs of equal pairs.
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&adj_start, n + 1U);
    }
    if (r == NPH_RESULT_OK) {
        memset(adj_start.data, 0, ((size_t)n + 1U) * sizeof(u32));
        u32 cur = 0U;
        for (u32 i = 0; i < pairs.len && r == NPH_RESULT_OK;) {
            u32 j = i;
            while (j < pairs.len && pairs.data[j].a == pairs.data[i].a &&
                   pairs.data[j].b == pairs.data[i].b) {
                ++j;
            }
            while (cur < pairs.data[i].a) {
                adj_start.data[cur + 1U] = adj.len;
                ++cur;
            }
            r = NPH_ARRAY_PUSH(&adj, pairs.data[i].b);
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&adj_weight, j - i);
            }
            i = j;
        }
        while (cur < n) {
            adj_start.data[cur + 1U] = adj.len;
            ++cur;
        }
    }
    if (r == NPH_RESULT_OK) {
        morton_order(centers, n, order);
        for (u32 i = 0; i < n; ++i) {
            rank_of[order[i].item] = i;
        }
    }
    u32 groups = 0U;
    for (u32 seed_rank = 0; seed_rank < n && r == NPH_RESULT_OK; ++seed_rank) {
        const u32 seed = order[seed_rank].item;
        if (group_of[seed] != NPH_U32_MAX) {
            continue;
        }
        NPH_ARRAY_CLEAR(&members);
        NPH_ARRAY_CLEAR(&frontier);
        group_of[seed] = groups;
        r = NPH_ARRAY_PUSH(&members, seed);
        NphSphere sphere = nph_sphere(centers[seed], radii[seed]);
        while (r == NPH_RESULT_OK && members.len < b->group_size) {
            // The frontier's gains from the newest member.
            NPH_ASSERT(members.data != NULL && members.len != 0U);
            const u32 last = members.data[members.len - 1U];
            for (u32 k = adj_start.data[last]; k < adj_start.data[last + 1U]; ++k) {
                // The run is inside the lists (the analyzer cannot tell
                // across the frontier's pushes).
                NPH_ASSERT(adj.data != NULL && adj_weight.data != NULL && k < adj.len);
                const u32 c = adj.data[k];
                if (group_of[c] != NPH_U32_MAX) {
                    continue;
                }
                if (gain[c] == 0U) {
                    r = NPH_ARRAY_PUSH(&frontier, c);
                }
                gain[c] += adj_weight.data[k];
            }
            u32 best = NPH_U32_MAX;
            for (u32 k = 0; k < frontier.len; ++k) {
                const u32 c = frontier.data[k];
                if (group_of[c] != NPH_U32_MAX) {
                    continue;
                }
                if (best == NPH_U32_MAX || gain[c] > gain[best] ||
                    (gain[c] == gain[best] && rank_of[c] < rank_of[best])) {
                    best = c;
                }
            }
            if (best == NPH_U32_MAX) {
                // No neighbour left: the nearest ungrouped cluster close by.
                f32 best_d = NPH_F32_MAX;
                for (u32 k = seed_rank + 1U; k < n && k <= seed_rank + NEAR_WINDOW; ++k) {
                    const u32 c = order[k].item;
                    if (group_of[c] != NPH_U32_MAX) {
                        continue;
                    }
                    const f32 d = nph_vec3_distance(centers[c], sphere.center);
                    if (d <= (sphere.radius + radii[c]) * NEAR_RADII && d < best_d) {
                        best = c;
                        best_d = d;
                    }
                }
            }
            if (best == NPH_U32_MAX) {
                break;
            }
            group_of[best] = groups;
            r = NPH_ARRAY_PUSH(&members, best);
            sphere = nph_sphere_union(sphere, nph_sphere(centers[best], radii[best]));
        }
        for (u32 k = 0; k < frontier.len; ++k) {
            gain[frontier.data[k]] = 0U;
        }
        groups += 1U;
    }
    *out_groups = groups;
    NPH_ARRAY_FREE(&entries);
    NPH_ARRAY_FREE(&classes);
    NPH_ARRAY_FREE(&pairs);
    NPH_ARRAY_FREE(&adj_start);
    NPH_ARRAY_FREE(&adj);
    NPH_ARRAY_FREE(&adj_weight);
    NPH_ARRAY_FREE(&members);
    NPH_ARRAY_FREE(&frontier);
    const u32 cap = nph_max_u32(n, 1U);
    if (centers != NULL) {
        NPH_FREE_ARRAY(a, NphVec3, centers, cap);
    }
    if (radii != NULL) {
        NPH_FREE_ARRAY(a, f32, radii, cap);
    }
    if (order != NULL) {
        NPH_FREE_ARRAY(a, Ranked, order, cap);
    }
    if (rank_of != NULL) {
        NPH_FREE_ARRAY(a, u32, rank_of, cap);
    }
    if (gain != NULL) {
        NPH_FREE_ARRAY(a, u32, gain, cap);
    }
    return r;
}

// ---------------------------------------------------------------------------
// One group
// ---------------------------------------------------------------------------

typedef struct SubTri {
    u32 submesh;
    u32 order;
} SubTri;

static int compare_subtri(const void *x, const void *y)
{
    const SubTri *p = (const SubTri *)x;
    const SubTri *q = (const SubTri *)y;
    if (p->submesh != q->submesh) {
        return p->submesh < q->submesh ? -1 : 1;
    }
    return p->order < q->order ? -1 : (p->order > q->order ? 1 : 0);
}

// Splits `tris` triangles (mesh vertices, a submesh each) into clusters of
// level `level` made by group `group`, appended to the output.
static NphResult recluster(Build *b, const u32 *indices, const u32 *submeshes, u32 tris, u32 group,
                           u32 level)
{
    NphAllocator *a = b->alloc;
    LodMesh *m = &b->mesh;
    NphRenderClusterLods *out = b->out;
    // Sorted by submesh, then in the simplifier's order.
    SubTri *order = NPH_ALLOC_ARRAY(a, SubTri, nph_max_u32(tris, 1U));
    u32 *local_indices = NPH_ALLOC_ARRAY(a, u32, (u64)nph_max_u32(tris, 1U) * 3U);
    NphArrayU32 vertices; // local -> mesh vertex
    NPH_ARRAY_INIT(&vertices, a);
    NPH_ARRAY(NphVec3) positions;
    NPH_ARRAY_INIT(&positions, a);
    NPH_ARRAY(NphRenderIndexRange) ranges;
    NPH_ARRAY_INIT(&ranges, a);
    NphArrayU32 range_submesh;
    NPH_ARRAY_INIT(&range_submesh, a);
    NphRenderMeshlets built;
    nph_render_meshlets_init(&built, a);
    NphResult r = order != NULL && local_indices != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 t = 0; t < tris; ++t) {
            order[t].submesh = submeshes[t];
            order[t].order = t;
        }
        if (tris > 1U) {
            qsort(order, tris, sizeof(SubTri), compare_subtri);
        }
    }
    // The group's own numbering of the vertices (the mesh's local_wedge
    // scratch, free between simplifications).
    for (u32 i = 0; i < tris && r == NPH_RESULT_OK; ++i) {
        const u32 t = order[i].order;
        if (i == 0U || order[i].submesh != order[i - 1U].submesh) {
            const NphRenderIndexRange range = {i * 3U, 0U};
            r = NPH_ARRAY_PUSH(&ranges, range);
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&range_submesh, order[i].submesh);
            }
        }
        if (r == NPH_RESULT_OK) {
            NPH_ARRAY_LAST(&ranges)->index_count += 3U;
        }
        for (u32 k = 0; k < 3U && r == NPH_RESULT_OK; ++k) {
            const u32 v = indices[(u64)t * 3U + k];
            if (m->local_wedge[v] == NPH_U32_MAX) {
                m->local_wedge[v] = vertices.len;
                r = NPH_ARRAY_PUSH(&vertices, v);
                if (r == NPH_RESULT_OK) {
                    r = NPH_ARRAY_PUSH(&positions, b->in->positions[v]);
                }
            }
            local_indices[(u64)i * 3U + k] = m->local_wedge[v];
        }
    }
    for (u32 i = 0; i < vertices.len; ++i) {
        m->local_wedge[vertices.data[i]] = NPH_U32_MAX;
    }
    if (r == NPH_RESULT_OK && tris != 0U) {
        NphRenderMeshletInput mi;
        memset(&mi, 0, sizeof mi);
        mi.positions = positions.data;
        mi.vertex_count = vertices.len;
        mi.indices = local_indices;
        mi.index_count = tris * 3U;
        mi.ranges = ranges.data;
        mi.range_count = ranges.len;
        mi.max_vertices = b->max_vertices;
        mi.max_triangles = b->max_triangles;
        mi.cone_weight = b->in->cone_weight;
        mi.scratch = a;
        r = nph_render_meshlets_build(&mi, &built);
    }
    // Appended with the mesh's vertices, the submeshes restored.
    for (u32 i = 0; i < built.meshlets.len && r == NPH_RESULT_OK; ++i) {
        NphRenderMeshlet ml = built.meshlets.data[i];
        const u32 vfirst = ml.vertex_offset;
        ml.vertex_offset = out->clusters.vertices.len;
        const u32 tfirst = ml.triangle_offset;
        ml.triangle_offset = out->clusters.triangles.len / 3U;
        NPH_ASSERT(range_submesh.data != NULL && ml.submesh < range_submesh.len);
        ml.submesh = range_submesh.data[ml.submesh];
        for (u32 k = 0; k < ml.vertex_count && r == NPH_RESULT_OK; ++k) {
            r = NPH_ARRAY_PUSH(&out->clusters.vertices,
                               vertices.data[built.vertices.data[vfirst + k]]);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH_N(&out->clusters.triangles, built.triangles.data + (u64)tfirst * 3U,
                                 (u32)ml.triangle_count * 3U);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&out->clusters.meshlets, ml);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&out->clusters.bounds, built.bounds.data[i]);
        }
        if (r == NPH_RESULT_OK) {
            const NphRenderClusterLod lod = {group, NPH_RENDER_LOD_NONE, level, 0U};
            r = NPH_ARRAY_PUSH(&out->lods, lod);
        }
    }
    nph_render_meshlets_free(&built);
    NPH_ARRAY_FREE(&vertices);
    NPH_ARRAY_FREE(&positions);
    NPH_ARRAY_FREE(&ranges);
    NPH_ARRAY_FREE(&range_submesh);
    if (order != NULL) {
        NPH_FREE_ARRAY(a, SubTri, order, nph_max_u32(tris, 1U));
    }
    if (local_indices != NULL) {
        NPH_FREE_ARRAY(a, u32, local_indices, (u64)nph_max_u32(tris, 1U) * 3U);
    }
    return r;
}

// A group's triangles, from its clusters: mesh vertices and submeshes.
static NphResult gather(const Build *b, const u32 *clusters, u32 count, NphArrayU32 *indices,
                        NphArrayU32 *submeshes)
{
    NPH_ARRAY_CLEAR(indices);
    NPH_ARRAY_CLEAR(submeshes);
    const NphRenderMeshlets *c = &b->out->clusters;
    for (u32 i = 0; i < count; ++i) {
        const NphRenderMeshlet *m = &c->meshlets.data[clusters[i]];
        for (u32 t = 0; t < m->triangle_count; ++t) {
            for (u32 k = 0; k < 3U; ++k) {
                const u8 corner = c->triangles.data[((u64)m->triangle_offset + t) * 3U + k];
                NPH_TRY(NPH_ARRAY_PUSH(indices, c->vertices.data[m->vertex_offset + corner]));
            }
            NPH_TRY(NPH_ARRAY_PUSH(submeshes, m->submesh));
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// The measured error
// ---------------------------------------------------------------------------

// A level's result in a uniform grid of cells, for the distance from each
// level-0 point to its nearest triangle: each triangle listed in every cell
// its box meets; cells about twice the mean edge, at most a few per
// triangle in all.
#define GRID_CELLS_PER_TRIANGLE 4U
#define GRID_MAX_SIDE           1024U

static u32 grid_coord(const LevelGrid *g, f32 v, u32 axis)
{
    const f32 t = (v - nph_vec3_at(g->lo, axis)) / g->cell;
    if (!(t > 0.0F)) {
        return 0U;
    }
    return nph_min_u32((u32)nph_f32_min(t, (f32)GRID_MAX_SIDE), g->n[axis] - 1U);
}

static u64 grid_cell(const LevelGrid *g, u32 x, u32 y, u32 z)
{
    return ((u64)z * g->n[1] + y) * g->n[0] + x;
}

static void grid_free(NphAllocator *a, LevelGrid *g)
{
    if (g->start != NULL) {
        NPH_FREE_ARRAY(a, u32, g->start, g->cells + 1U);
    }
    NPH_ARRAY_FREE(&g->items);
    NPH_ARRAY_FREE(&g->owned);
}

// The cells over the grid's triangles (`positions`, `tris`, `tri_count`).
static NphResult grid_index(LevelGrid *g, NphAllocator *alloc)
{
    const u32 tris = g->tri_count;
    if (tris == 0U) {
        return NPH_RESULT_OK;
    }
    NphVec3 lo = grid_corner(g, 0U, 0U);
    NphVec3 hi = lo;
    f64 edges = 0.0;
    for (u32 t = 0; t < tris; ++t) {
        NphVec3 p[3];
        for (u32 k = 0; k < 3U; ++k) {
            p[k] = grid_corner(g, t, k);
        }
        for (u32 k = 0; k < 3U; ++k) {
            lo = nph_vec3_min(lo, p[k]);
            hi = nph_vec3_max(hi, p[k]);
            edges += (f64)nph_vec3_distance(p[k], p[(k + 1U) % 3U]);
        }
    }
    const NphVec3 e = nph_vec3_sub(hi, lo);
    g->lo = lo;
    g->cell = nph_f32_max((f32)(2.0 * edges / (3.0 * (f64)tris)), 1e-12F);
    // Not more cells than a few a triangle: larger cells where the result
    // spreads thin through its box.
    for (u32 attempt = 0; attempt < 64U; ++attempt) {
        u64 cells = 1U;
        for (u32 a = 0; a < 3U; ++a) {
            g->n[a] =
                nph_min_u32((u32)nph_f32_min(nph_vec3_at(e, a) / g->cell, (f32)GRID_MAX_SIDE) + 1U,
                            GRID_MAX_SIDE);
            cells *= g->n[a];
        }
        g->cells = cells;
        if (cells <= (u64)tris * GRID_CELLS_PER_TRIANGLE + 64U) {
            break;
        }
        g->cell *= 1.25F;
    }
    g->start = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, g->cells + 1U);
    if (g->start == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 pass = 0; pass < 2U; ++pass) {
        if (pass == 1U) {
            for (u64 i = 0; i < g->cells; ++i) {
                g->start[i + 1U] += g->start[i];
            }
            NPH_TRY(NPH_ARRAY_RESIZE(&g->items, g->start[g->cells]));
        }
        for (u32 t = 0; t < tris; ++t) {
            const NphVec3 p0 = grid_corner(g, t, 0U);
            const NphVec3 p1 = grid_corner(g, t, 1U);
            const NphVec3 p2 = grid_corner(g, t, 2U);
            const NphVec3 tlo = nph_vec3_min(nph_vec3_min(p0, p1), p2);
            const NphVec3 thi = nph_vec3_max(nph_vec3_max(p0, p1), p2);
            u32 c0[3];
            u32 c1[3];
            for (u32 a = 0; a < 3U; ++a) {
                c0[a] = grid_coord(g, nph_vec3_at(tlo, a), a);
                c1[a] = grid_coord(g, nph_vec3_at(thi, a), a);
            }
            for (u32 z = c0[2]; z <= c1[2]; ++z) {
                for (u32 y = c0[1]; y <= c1[1]; ++y) {
                    for (u32 x = c0[0]; x <= c1[0]; ++x) {
                        const u64 cell = grid_cell(g, x, y, z);
                        if (pass == 0U) {
                            g->start[cell + 1U] += 1U;
                        } else {
                            g->items.data[g->start[cell]] = t;
                            g->start[cell] += 1U;
                        }
                    }
                }
            }
        }
    }
    for (u64 i = g->cells; i > 0U; --i) {
        g->start[i] = g->start[i - 1U];
    }
    g->start[0] = 0U;
    return NPH_RESULT_OK;
}

// The grid of clusters [first, end)'s triangles.
static NphResult grid_build(Build *b, LevelGrid *g, u32 first, u32 end)
{
    const NphRenderMeshlets *c = &b->out->clusters;
    for (u32 i = first; i < end; ++i) {
        const NphRenderMeshlet *m = &c->meshlets.data[i];
        for (u32 k = 0; k < (u32)m->triangle_count * 3U; ++k) {
            const u8 corner = c->triangles.data[(u64)m->triangle_offset * 3U + k];
            NPH_TRY(NPH_ARRAY_PUSH(&g->owned, c->vertices.data[m->vertex_offset + corner]));
        }
    }
    g->positions = b->in->positions;
    g->tris = g->owned.data;
    g->tri_count = g->owned.len / 3U;
    return grid_index(g, b->alloc);
}

static f32 grid_triangle(const LevelGrid *g, u32 t, NphVec3 q)
{
    return nph_vec3_distance(q, nph_closest_point_on_triangle(grid_corner(g, t, 0U),
                                                              grid_corner(g, t, 1U),
                                                              grid_corner(g, t, 2U), q));
}

// The triangles of cell (x, y, z) against `q`, into `best`.
static void grid_visit(const LevelGrid *g, i32 x, i32 y, i32 z, NphVec3 q, f32 *best)
{
    if (x < 0 || y < 0 || z < 0 || x >= (i32)g->n[0] || y >= (i32)g->n[1] || z >= (i32)g->n[2]) {
        return;
    }
    const u64 cell = grid_cell(g, (u32)x, (u32)y, (u32)z);
    for (u32 i = g->start[cell]; i < g->start[cell + 1U]; ++i) {
        *best = nph_f32_min(*best, grid_triangle(g, g->items.data[i], q));
    }
}

// The distance from `q` to the nearest triangle, at most `best` (a distance
// already known: NPH_F32_MAX for none): the cells in rings of growing
// Chebyshev distance around q's, each ring's shell alone, until the inner
// rings' cube holds every triangle nearer than the best found (q's
// distance from that cube's faces bounds every triangle beyond it).
static f32 grid_nearest(const LevelGrid *g, NphVec3 q, f32 best)
{
    if (g->start == NULL) {
        return best;
    }
    i32 cq[3];
    f32 outside = 0.0F;
    for (u32 a = 0; a < 3U; ++a) {
        const f32 lo = nph_vec3_at(g->lo, a);
        const f32 hi = lo + g->cell * (f32)g->n[a];
        const f32 v = nph_vec3_at(q, a);
        outside = nph_f32_max(outside, nph_f32_max(lo - v, v - hi));
        cq[a] = (i32)grid_coord(g, v, a);
    }
    const i32 rings = (i32)nph_max_u32(nph_max_u32(g->n[0], g->n[1]), g->n[2]);
    for (i32 r = 0; r <= rings; ++r) {
        if (r > 0) {
            // The inner cube: cells cq - (r - 1) .. cq + (r - 1) a side.
            f32 inner = NPH_F32_MAX;
            for (u32 a = 0; a < 3U; ++a) {
                const f32 lo = nph_vec3_at(g->lo, a);
                const f32 v = nph_vec3_at(q, a);
                const f32 near = v - (lo + (f32)(cq[a] - r + 1) * g->cell);
                const f32 far = lo + (f32)(cq[a] + r) * g->cell - v;
                inner = nph_f32_min(inner, nph_f32_min(near, far));
            }
            if (best <= nph_f32_max(outside, inner)) {
                break;
            }
        }
        if (r == 0) {
            grid_visit(g, cq[0], cq[1], cq[2], q, &best);
            continue;
        }
        // The shell: the two z faces whole, the two y faces between them,
        // the two x faces between those.
        for (i32 y = cq[1] - r; y <= cq[1] + r; ++y) {
            for (i32 x = cq[0] - r; x <= cq[0] + r; ++x) {
                grid_visit(g, x, y, cq[2] - r, q, &best);
                grid_visit(g, x, y, cq[2] + r, q, &best);
            }
        }
        for (i32 z = cq[2] - r + 1; z <= cq[2] + r - 1; ++z) {
            for (i32 x = cq[0] - r; x <= cq[0] + r; ++x) {
                grid_visit(g, x, cq[1] - r, z, q, &best);
                grid_visit(g, x, cq[1] + r, z, q, &best);
            }
            for (i32 y = cq[1] - r + 1; y <= cq[1] + r - 1; ++y) {
                grid_visit(g, cq[0] - r, y, z, q, &best);
                grid_visit(g, cq[0] + r, y, z, q, &best);
            }
        }
    }
    return best;
}

// The farthest of the level-0 points class `c` stands for from the
// level's result, each seeded with its distance from the class's own
// triangles (`tris`, `count`; usually the nearest already); for a class
// left without a triangle, each point counts no farther than the class
// itself, which the piece it ended in collapsed into.
static f32 class_distance(const Build *b, const LevelGrid *g, u32 c, const u32 *tris, u32 count,
                          bool orphan)
{
    const LodMesh *m = &b->mesh;
    const NphVec3 at = b->in->positions[m->class_vertex[c]];
    f32 worst = 0.0F;
    for (u32 p = c; p != NPH_U32_MAX; p = m->absorbed_next[p]) {
        const NphVec3 q = b->in->positions[m->class_vertex[p]];
        f32 seed = orphan ? nph_vec3_distance(q, at) : NPH_F32_MAX;
        for (u32 i = 0; i < count; ++i) {
            seed = nph_f32_min(seed, grid_triangle(g, tris[i], q));
        }
        // A point already as near as the worst so far changes nothing.
        if (seed <= worst) {
            continue;
        }
        worst = nph_f32_max(worst, grid_nearest(g, q, seed));
    }
    return worst;
}

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

// The groups of the level [first, first + n), simplified into the next.
// `*out_made` takes the next level's cluster count.
static NphResult build_level(Build *b, u32 level, u32 first, u32 n, u32 *out_made,
                             u32 *out_tris_before, u32 *out_tris_after)
{
    NphAllocator *a = b->alloc;
    NphRenderClusterLods *out = b->out;
    *out_made = 0U;
    *out_tris_before = 0U;
    *out_tris_after = 0U;
    u32 *group_of = NPH_ALLOC_ARRAY(a, u32, nph_max_u32(n, 1U));
    if (group_of == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 groups = 0U;
    NphResult r = partition(b, first, n, group_of, &groups);
    // Each group's clusters in turn (a counting sort, in cluster order).
    NphArrayU32 start;
    NPH_ARRAY_INIT(&start, a);
    NphArrayU32 members;
    NPH_ARRAY_INIT(&members, a);
    NphArrayU32 classes;
    NPH_ARRAY_INIT(&classes, a);
    NphArrayU32 touched; // classes whose class_group was set
    NPH_ARRAY_INIT(&touched, a);
    NphArrayU32 indices;
    NPH_ARRAY_INIT(&indices, a);
    NphArrayU32 submeshes;
    NPH_ARRAY_INIT(&submeshes, a);
    // Per group: where its clusters and its orphans begin (one past the
    // last group: where they end).
    NphArrayU32 made_start;
    NPH_ARRAY_INIT(&made_start, a);
    NphArrayU32 orphan_start;
    NPH_ARRAY_INIT(&orphan_start, a);
    NPH_ARRAY(NphSphere) hull_parts; // a group's clusters' own spheres
    NPH_ARRAY_INIT(&hull_parts, a);
    NphArrayU32 orphans;
    NPH_ARRAY_INIT(&orphans, a);
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&start, groups + 1U);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&members, n);
    }
    if (r == NPH_RESULT_OK) {
        memset(start.data, 0, ((size_t)groups + 1U) * sizeof(u32));
        for (u32 i = 0; i < n; ++i) {
            start.data[group_of[i] + 1U] += 1U;
        }
        for (u32 g = 0; g < groups; ++g) {
            start.data[g + 1U] += start.data[g];
        }
        for (u32 i = 0; i < n; ++i) {
            const u32 g = group_of[i];
            members.data[start.data[g]] = first + i;
            start.data[g] += 1U;
        }
        for (u32 g = groups; g > 0U; --g) {
            start.data[g] = start.data[g - 1U];
        }
        start.data[0] = 0U;
    }
    // The classes two groups share are locked.
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        NPH_ARRAY_CLEAR(&classes);
        for (u32 i = start.data[g]; i < start.data[g + 1U] && r == NPH_RESULT_OK; ++i) {
            r = cluster_classes(b, members.data[i], &classes);
        }
        for (u32 k = 0; k < classes.len && r == NPH_RESULT_OK; ++k) {
            const u32 c = classes.data[k];
            if (b->class_group[c] == NPH_U32_MAX) {
                b->class_group[c] = g;
                r = NPH_ARRAY_PUSH(&touched, c);
            } else if (b->class_group[c] != g) {
                b->locked[c] = 1U;
            }
        }
    }
    const u32 made_first = out->clusters.meshlets.len;
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        const u32 *cl = members.data + start.data[g];
        const u32 count = start.data[g + 1U] - start.data[g];
        // The group's record: its clusters, their spheres and errors.
        NphRenderLodGroup rec;
        memset(&rec, 0, sizeof rec);
        f32 error = 0.0F;
        NPH_ARRAY_CLEAR(&hull_parts);
        for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
            r = NPH_ARRAY_PUSH(&hull_parts, b->own_sphere.data[cl[i]]);
            error = nph_f32_max(error, b->own_error.data[cl[i]]);
        }
        if (r != NPH_RESULT_OK) {
            break;
        }
        const NphSphere sphere = sphere_hull(hull_parts.data, hull_parts.len);
        r = gather(b, cl, count, &indices, &submeshes);
        if (r != NPH_RESULT_OK) {
            break;
        }
        const u32 tris = submeshes.len;
        *out_tris_before += tris;
        LodSimplifyInput si;
        memset(&si, 0, sizeof si);
        si.mesh = &b->mesh;
        si.indices = indices.data;
        si.submeshes = submeshes.data;
        si.triangle_count = tris;
        si.locked = b->locked;
        si.target_triangles = (tris + 1U) / 2U;
        si.orphans = &orphans;
        si.scratch = a;
        r = NPH_ARRAY_PUSH(&orphan_start, orphans.len);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&made_start, out->clusters.meshlets.len);
        }
        if (r != NPH_RESULT_OK) {
            break;
        }
        u32 kept = tris;
        f32 simplify_error = 0.0F;
        r = lod_simplify(&si, &kept, &simplify_error);
        if (r != NPH_RESULT_OK) {
            break;
        }
        *out_tris_after += kept;
        const u32 group_index = out->groups.len;
        rec.center = sphere.center;
        rec.radius = sphere.radius;
        rec.error = nph_f32_max(error, simplify_error);
        rec.level = level;
        rec.cluster_offset = out->group_clusters.len;
        rec.cluster_count = count;
        r = NPH_ARRAY_PUSH(&out->groups, rec);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH_N(&out->group_clusters, cl, count);
        }
        for (u32 i = 0; i < count; ++i) {
            out->lods.data[cl[i]].parent = group_index;
        }
        if (r == NPH_RESULT_OK) {
            r = recluster(b, indices.data, submeshes.data, kept, group_index, level + 1U);
        }
        // The new clusters' own sphere and error: the group's.
        while (r == NPH_RESULT_OK && b->own_sphere.len < out->clusters.meshlets.len) {
            r = NPH_ARRAY_PUSH(&b->own_sphere, sphere);
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&b->own_error, rec.error);
            }
        }
    }
    for (u32 k = 0; k < touched.len; ++k) {
        b->class_group[touched.data[k]] = NPH_U32_MAX;
        b->locked[touched.data[k]] = 0U;
    }
    // The measured error: every level-0 point, through the class it went to,
    // against the level's whole result (a point by a group's locked border
    // may lie under its neighbour's triangles), each group the farthest of
    // its classes' and orphans' points; its clusters take it as their own.
    LevelGrid grid;
    memset(&grid, 0, sizeof grid);
    NPH_ARRAY_INIT(&grid.items, a);
    NPH_ARRAY_INIT(&grid.owned, a);
    if (r == NPH_RESULT_OK) {
        r = grid_build(b, &grid, made_first, out->clusters.meshlets.len);
    }
    // Each class's triangles in the result: a counting sort through the
    // class-indexed scratch (`class_tri_count`, zero between levels).
    NphArrayU32 class_seen; // the classes the result has, in first-seen order
    NPH_ARRAY_INIT(&class_seen, a);
    NphArrayU32 by_class;
    NPH_ARRAY_INIT(&by_class, a);
    const u32 corners = grid.tri_count * 3U;
    for (u32 i = 0; i < corners && r == NPH_RESULT_OK; ++i) {
        const u32 c = b->mesh.class_of[grid.tris[i]];
        if (b->class_tri_count[c] == 0U) {
            r = NPH_ARRAY_PUSH(&class_seen, c);
        }
        b->class_tri_count[c] += 1U;
    }
    if (r == NPH_RESULT_OK) {
        // Counts to run starts (class_tri_start), then filled.
        u32 at = 0U;
        for (u32 i = 0; i < class_seen.len; ++i) {
            const u32 c = class_seen.data[i];
            b->class_tri_start[c] = at;
            at += b->class_tri_count[c];
            b->class_tri_count[c] = 0U;
        }
        r = NPH_ARRAY_RESIZE(&by_class, at);
    }
    for (u32 i = 0; i < corners && r == NPH_RESULT_OK; ++i) {
        const u32 c = b->mesh.class_of[grid.tris[i]];
        by_class.data[b->class_tri_start[c] + b->class_tri_count[c]] = i / 3U;
        b->class_tri_count[c] += 1U;
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH(&made_start, out->clusters.meshlets.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH(&orphan_start, orphans.len);
    }
    NPH_ARRAY_CLEAR(&touched);
    const u32 groups_first = out->groups.len - (made_start.len > 0U ? made_start.len - 1U : 0U);
    for (u32 g = 0; g + 1U < made_start.len && r == NPH_RESULT_OK; ++g) {
        f32 measured = 0.0F;
        for (u32 cl = made_start.data[g]; cl < made_start.data[g + 1U] && r == NPH_RESULT_OK;
             ++cl) {
            const NphRenderMeshlet *ml = &out->clusters.meshlets.data[cl];
            for (u32 k = 0; k < ml->vertex_count && r == NPH_RESULT_OK; ++k) {
                const u32 c = b->mesh.class_of[out->clusters.vertices.data[ml->vertex_offset + k]];
                if (b->class_dist[c] < 0.0F) {
                    b->class_dist[c] =
                        class_distance(b, &grid, c, by_class.data + b->class_tri_start[c],
                                       b->class_tri_count[c], false);
                    r = NPH_ARRAY_PUSH(&touched, c);
                }
                measured = nph_f32_max(measured, b->class_dist[c]);
            }
        }
        // The other way: the result's triangles back to the mesh (their
        // centroids and edge midpoints), where a chord across a curve may
        // lie farther from it than any of the mesh's own points does from
        // the chord.
        for (u32 cl = made_start.data[g]; cl < made_start.data[g + 1U]; ++cl) {
            const NphRenderMeshlet *ml = &out->clusters.meshlets.data[cl];
            for (u32 t = 0; t < ml->triangle_count; ++t) {
                NphVec3 p[3];
                for (u32 k = 0; k < 3U; ++k) {
                    const u8 corner =
                        out->clusters.triangles.data[((u64)ml->triangle_offset + t) * 3U + k];
                    p[k] =
                        b->in->positions[out->clusters.vertices.data[ml->vertex_offset + corner]];
                }
                const NphVec3 samples[4] = {
                    nph_vec3_scale(nph_vec3_add(p[0], p[1]), 0.5F),
                    nph_vec3_scale(nph_vec3_add(p[1], p[2]), 0.5F),
                    nph_vec3_scale(nph_vec3_add(p[2], p[0]), 0.5F),
                    nph_vec3_scale(nph_vec3_add(nph_vec3_add(p[0], p[1]), p[2]), 1.0F / 3.0F)};
                for (u32 k = 0; k < 4U; ++k) {
                    measured =
                        nph_f32_max(measured, grid_nearest(&b->ground, samples[k], NPH_F32_MAX));
                }
            }
        }
        for (u32 i = orphan_start.data[g]; i < orphan_start.data[g + 1U]; ++i) {
            NPH_ASSERT(orphans.data != NULL && i < orphans.len);
            measured =
                nph_f32_max(measured, class_distance(b, &grid, orphans.data[i], NULL, 0U, true));
        }
        NphRenderLodGroup *rg = &out->groups.data[groups_first + g];
        rg->error = nph_f32_max(rg->error, measured);
        for (u32 cl = made_start.data[g]; cl < made_start.data[g + 1U]; ++cl) {
            b->own_error.data[cl] = rg->error;
        }
    }
    for (u32 k = 0; k < touched.len; ++k) {
        b->class_dist[touched.data[k]] = -1.0F;
    }
    for (u32 i = 0; i < class_seen.len; ++i) {
        b->class_tri_count[class_seen.data[i]] = 0U;
    }
    NPH_ARRAY_FREE(&class_seen);
    NPH_ARRAY_FREE(&by_class);
    grid_free(a, &grid);
    NPH_ARRAY_FREE(&made_start);
    NPH_ARRAY_FREE(&orphan_start);
    NPH_ARRAY_FREE(&orphans);
    NPH_ARRAY_FREE(&hull_parts);
    *out_made = out->clusters.meshlets.len - made_first;
    NPH_ARRAY_FREE(&start);
    NPH_ARRAY_FREE(&members);
    NPH_ARRAY_FREE(&classes);
    NPH_ARRAY_FREE(&touched);
    NPH_ARRAY_FREE(&indices);
    NPH_ARRAY_FREE(&submeshes);
    NPH_FREE_ARRAY(a, u32, group_of, nph_max_u32(n, 1U));
    return r;
}

// ---------------------------------------------------------------------------
// The root groups
// ---------------------------------------------------------------------------

// The coarsest level's clusters [first, first + n) into root groups,
// partitioned as a level's are (their error the largest float).
static NphResult build_roots(Build *b, u32 first, u32 n)
{
    NphRenderClusterLods *out = b->out;
    NphAllocator *a = b->alloc;
    u32 *group_of = NPH_ALLOC_ARRAY(a, u32, n);
    u32 *start = NULL;
    u32 groups = 0U;
    NphResult r = group_of != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        r = partition(b, first, n, group_of, &groups);
    }
    if (r == NPH_RESULT_OK) {
        start = NPH_ALLOC_ARRAY_ZEROED(a, u32, (u64)groups + 1U);
        r = start != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    NPH_ARRAY(NphSphere) parts;
    NPH_ARRAY_INIT(&parts, a);
    NphArrayU32 members;
    NPH_ARRAY_INIT(&members, a);
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&members, n);
    }
    if (r == NPH_RESULT_OK) {
        // Each group's clusters in turn (a counting sort, in cluster order).
        for (u32 i = 0; i < n; ++i) {
            start[group_of[i] + 1U] += 1U;
        }
        for (u32 g = 0; g < groups; ++g) {
            start[g + 1U] += start[g];
        }
        for (u32 i = 0; i < n; ++i) {
            const u32 g = group_of[i];
            members.data[start[g]] = first + i;
            start[g] += 1U;
        }
        // The fill moved each group's start to its end: back by one group.
        for (u32 g = groups; g > 0U; --g) {
            start[g] = start[g - 1U];
        }
        start[0] = 0U;
    }
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        NPH_ARRAY_CLEAR(&parts);
        for (u32 i = start[g]; i < start[g + 1U] && r == NPH_RESULT_OK; ++i) {
            r = NPH_ARRAY_PUSH(&parts, b->own_sphere.data[members.data[i]]);
        }
        if (r != NPH_RESULT_OK) {
            break;
        }
        const NphSphere sphere = sphere_hull(parts.data, parts.len);
        NphRenderLodGroup rec;
        memset(&rec, 0, sizeof rec);
        rec.center = sphere.center;
        rec.radius = sphere.radius;
        rec.error = NPH_F32_MAX;
        rec.level = out->levels - 1U;
        rec.cluster_offset = out->group_clusters.len;
        rec.cluster_count = start[g + 1U] - start[g];
        for (u32 i = start[g]; i < start[g + 1U]; ++i) {
            out->lods.data[members.data[i]].parent = out->groups.len;
        }
        r = NPH_ARRAY_PUSH(&out->groups, rec);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH_N(&out->group_clusters, members.data + start[g], rec.cluster_count);
        }
    }
    NPH_ARRAY_FREE(&parts);
    NPH_ARRAY_FREE(&members);
    if (start != NULL) {
        NPH_FREE_ARRAY(a, u32, start, (u64)groups + 1U);
    }
    if (group_of != NULL) {
        NPH_FREE_ARRAY(a, u32, group_of, n);
    }
    return r;
}

// ---------------------------------------------------------------------------
// The hierarchy
// ---------------------------------------------------------------------------

typedef struct NodeChild {
    NphSphere sphere;
    f32 error;
    f32 maker_error;
    u32 ref; // node index, or group | NPH_RENDER_LOD_LEAF
} NodeChild;

// A group as a leaf of the hierarchy: its own sphere and error, and the
// smallest error of the groups that made its clusters (0 for level 0's).
static NodeChild group_leaf(const NphRenderClusterLods *l, u32 g)
{
    const NphRenderLodGroup *rg = &l->groups.data[g];
    NodeChild c;
    c.sphere = nph_sphere(rg->center, rg->radius);
    c.error = rg->error;
    c.maker_error = NPH_F32_MAX;
    for (u32 i = 0; i < rg->cluster_count; ++i) {
        const u32 maker = l->lods.data[l->group_clusters.data[rg->cluster_offset + i]].group;
        const f32 e = maker == NPH_RENDER_LOD_NONE ? 0.0F : l->groups.data[maker].error;
        c.maker_error = nph_f32_min(c.maker_error, e);
    }
    if (rg->cluster_count == 0U) {
        c.maker_error = 0.0F;
    }
    c.ref = g | NPH_RENDER_LOD_LEAF;
    return c;
}

typedef NPH_ARRAY(NodeChild) NodeChildArray;

// A node over `children` (at most NPH_RENDER_LOD_NODE_CHILDREN), appended;
// its summary (sphere, error, reference) is returned through `out`.
static NphResult push_node(NphRenderClusterLods *l, const NodeChild *children, u32 count,
                           NodeChild *out)
{
    NPH_ASSERT(children != NULL && count != 0U && count <= NPH_RENDER_LOD_NODE_CHILDREN);
    NphRenderLodNode node;
    memset(&node, 0, sizeof node);
    NphSphere parts[NPH_RENDER_LOD_NODE_CHILDREN];
    f32 error = 0.0F;
    f32 maker_error = NPH_F32_MAX;
    for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
        if (i < count) {
            node.sphere[i] = nph_vec4(children[i].sphere.center.x, children[i].sphere.center.y,
                                      children[i].sphere.center.z, children[i].sphere.radius);
            node.error[i] = children[i].error;
            node.maker_error[i] = children[i].maker_error;
            node.child[i] = children[i].ref;
            parts[i] = children[i].sphere;
            error = nph_f32_max(error, children[i].error);
            maker_error = nph_f32_min(maker_error, children[i].maker_error);
        } else {
            node.child[i] = NPH_RENDER_LOD_NONE;
        }
    }
    out->sphere = sphere_hull(parts, count);
    out->error = error;
    out->maker_error = maker_error;
    out->ref = l->nodes.len;
    return NPH_ARRAY_PUSH(&l->nodes, node);
}

// A tree over `items` (in order), its root's summary through `out`: chunks
// of NPH_RENDER_LOD_NODE_CHILDREN per node, level by level up.
static NphResult build_tree(NphRenderClusterLods *l, NphAllocator *a, const NodeChild *items,
                            u32 count, NodeChild *out)
{
    NodeChildArray cur;
    NodeChildArray next;
    NPH_ARRAY_INIT(&cur, a);
    NPH_ARRAY_INIT(&next, a);
    NphResult r = NPH_ARRAY_PUSH_N(&cur, items, count);
    while (r == NPH_RESULT_OK && cur.len > 1U) {
        NPH_ARRAY_CLEAR(&next);
        for (u32 i = 0; i < cur.len && r == NPH_RESULT_OK; i += NPH_RENDER_LOD_NODE_CHILDREN) {
            NodeChild summary;
            r = push_node(l, cur.data + i, nph_min_u32(NPH_RENDER_LOD_NODE_CHILDREN, cur.len - i),
                          &summary);
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&next, summary);
            }
        }
        const NodeChildArray t = cur;
        cur = next;
        next = t;
    }
    if (r == NPH_RESULT_OK) {
        *out = cur.data[0];
    }
    NPH_ARRAY_FREE(&cur);
    NPH_ARRAY_FREE(&next);
    return r;
}

static NphResult build_hierarchy(Build *b)
{
    NphRenderClusterLods *l = b->out;
    NphAllocator *a = b->alloc;
    // Node 0 is the root, filled in last.
    NphRenderLodNode root;
    memset(&root, 0, sizeof root);
    NPH_TRY(NPH_ARRAY_PUSH(&l->nodes, root));
    NodeChildArray tops; // each level's tree, then the root groups'
    NPH_ARRAY_INIT(&tops, a);
    NodeChildArray items;
    NPH_ARRAY_INIT(&items, a);
    NPH_ARRAY(NphVec3) centers;
    NPH_ARRAY_INIT(&centers, a);
    RankedArray order;
    NPH_ARRAY_INIT(&order, a);
    NphResult r = NPH_RESULT_OK;
    const u32 groups = l->groups.len;
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK;) {
        // The run of one level's groups, or of the root groups (they come
        // level by level, the root groups last).
        const bool roots = l->groups.data[g].error == NPH_F32_MAX;
        u32 end = g;
        while (end < groups && l->groups.data[end].level == l->groups.data[g].level &&
               (l->groups.data[end].error == NPH_F32_MAX) == roots) {
            ++end;
        }
        const u32 n = end - g;
        NPH_ARRAY_CLEAR(&centers);
        for (u32 i = g; i < end && r == NPH_RESULT_OK; ++i) {
            r = NPH_ARRAY_PUSH(&centers, l->groups.data[i].center);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_RESIZE(&order, n);
        }
        if (r != NPH_RESULT_OK) {
            break;
        }
        morton_order(centers.data, n, order.data);
        NPH_ARRAY_CLEAR(&items);
        for (u32 i = 0; i < n && r == NPH_RESULT_OK; ++i) {
            r = NPH_ARRAY_PUSH(&items, group_leaf(l, g + order.data[i].item));
        }
        NodeChild top;
        memset(&top, 0, sizeof top);
        if (r == NPH_RESULT_OK) {
            r = build_tree(l, a, items.data, items.len, &top);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&tops, top);
        }
        g = end;
    }
    // The root: the levels' trees and the root groups', nested under further
    // nodes while there are more than a node holds.
    if (r == NPH_RESULT_OK && tops.len > NPH_RENDER_LOD_NODE_CHILDREN) {
        NodeChild top;
        memset(&top, 0, sizeof top);
        r = build_tree(l, a, tops.data, tops.len, &top);
        if (r == NPH_RESULT_OK) {
            // The tree's own root becomes node 0's only child.
            NPH_ARRAY_CLEAR(&tops);
            r = NPH_ARRAY_PUSH(&tops, top);
        }
    }
    if (r == NPH_RESULT_OK) {
        NodeChild summary;
        r = push_node(l, tops.data, tops.len, &summary);
        if (r == NPH_RESULT_OK) {
            // push_node appended it; it belongs at 0.
            l->nodes.data[0] = l->nodes.data[l->nodes.len - 1U];
            l->nodes.len -= 1U;
        }
    }
    NPH_ARRAY_FREE(&tops);
    NPH_ARRAY_FREE(&items);
    NPH_ARRAY_FREE(&centers);
    NPH_ARRAY_FREE(&order);
    return r;
}

// ---------------------------------------------------------------------------
// The build
// ---------------------------------------------------------------------------

static NphResult validate(const NphRenderClusterLodInput *in, u32 *max_vertices, u32 *max_triangles)
{
    *max_vertices = in->max_vertices != 0U ? in->max_vertices : NPH_RENDER_MESHLET_MAX_VERTICES;
    *max_triangles = in->max_triangles != 0U ? in->max_triangles : NPH_RENDER_MESHLET_MAX_TRIANGLES;
    const NphRenderMeshlets *m = in->meshlets;
    if (*max_vertices < 3U || *max_vertices > NPH_RENDER_MESHLET_MAX_VERTICES ||
        *max_triangles < 1U || *max_triangles > NPH_RENDER_MESHLET_MAX_TRIANGLES ||
        !(in->cone_weight >= 0.0F && in->cone_weight <= 1.0F) || in->scratch == NULL || m == NULL ||
        in->index_count % 3U != 0U ||
        (in->index_count != 0U && (in->indices == NULL || in->positions == NULL)) ||
        (in->range_count != 0U && in->ranges == NULL) || m->bounds.len != m->meshlets.len ||
        (in->attribs != NULL && in->attrib_stride == 0U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < in->index_count; ++i) {
        if (in->indices[i] >= in->vertex_count) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    for (u32 v = 0; v < in->vertex_count; ++v) {
        const NphVec3 p = in->positions[v];
        if (!nph_f32_is_finite(p.x) || !nph_f32_is_finite(p.y) || !nph_f32_is_finite(p.z)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    // The meshlets must index the vertices and their own arrays, with each
    // submesh a range.
    for (u32 i = 0; i < m->meshlets.len; ++i) {
        const NphRenderMeshlet *ml = &m->meshlets.data[i];
        if (ml->submesh >= in->range_count || ml->vertex_count > *max_vertices ||
            (u64)ml->vertex_offset + ml->vertex_count > m->vertices.len ||
            ((u64)ml->triangle_offset + ml->triangle_count) * 3U > m->triangles.len) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        for (u32 k = 0; k < ml->vertex_count; ++k) {
            if (m->vertices.data[ml->vertex_offset + k] >= in->vertex_count) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
        for (u32 k = 0; k < (u32)ml->triangle_count * 3U; ++k) {
            if (m->triangles.data[(u64)ml->triangle_offset * 3U + k] >= ml->vertex_count) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
    }
    return NPH_RESULT_OK;
}

static void build_free(Build *b)
{
    NphAllocator *a = b->alloc;
    const u32 c = nph_max_u32(b->mesh.class_count, 1U);
    if (b->class_group != NULL) {
        NPH_FREE_ARRAY(a, u32, b->class_group, c);
    }
    if (b->locked != NULL) {
        NPH_FREE_ARRAY(a, u8, b->locked, c);
    }
    if (b->class_dist != NULL) {
        NPH_FREE_ARRAY(a, f32, b->class_dist, c);
    }
    if (b->class_tri_count != NULL) {
        NPH_FREE_ARRAY(a, u32, b->class_tri_count, c);
    }
    if (b->class_tri_start != NULL) {
        NPH_FREE_ARRAY(a, u32, b->class_tri_start, c);
    }
    NPH_ARRAY_FREE(&b->own_sphere);
    NPH_ARRAY_FREE(&b->own_error);
    grid_free(a, &b->ground);
    lod_mesh_free(&b->mesh, a);
}

NphResult nph_render_cluster_lods_build(const NphRenderClusterLodInput *in,
                                        NphRenderClusterLods *out)
{
    NPH_ASSERT(in != NULL && out != NULL);
    u32 max_vertices = 0U;
    u32 max_triangles = 0U;
    NPH_TRY(validate(in, &max_vertices, &max_triangles));
    Build b;
    memset(&b, 0, sizeof b);
    b.in = in;
    b.alloc = in->scratch;
    b.out = out;
    b.max_vertices = max_vertices;
    b.max_triangles = max_triangles;
    b.group_size = in->group_size != 0U ? in->group_size : 8U;
    NPH_ARRAY_INIT(&b.own_sphere, b.alloc);
    NPH_ARRAY_INIT(&b.own_error, b.alloc);
    NPH_ARRAY_INIT(&b.ground.items, b.alloc);
    NPH_ARRAY_INIT(&b.ground.owned, b.alloc);
    NphResult r = lod_mesh_init(&b.mesh, b.alloc, in->positions, in->attribs, in->attrib_stride,
                                in->vertex_count, in->indices, in->index_count);
    if (r == NPH_RESULT_OK) {
        const u32 c = nph_max_u32(b.mesh.class_count, 1U);
        b.class_group = NPH_ALLOC_ARRAY(b.alloc, u32, c);
        b.locked = NPH_ALLOC_ARRAY_ZEROED(b.alloc, u8, c);
        b.class_dist = NPH_ALLOC_ARRAY(b.alloc, f32, c);
        b.class_tri_count = NPH_ALLOC_ARRAY_ZEROED(b.alloc, u32, c);
        b.class_tri_start = NPH_ALLOC_ARRAY(b.alloc, u32, c);
        r = b.class_group != NULL && b.locked != NULL && b.class_dist != NULL &&
                    b.class_tri_count != NULL && b.class_tri_start != NULL
                ? NPH_RESULT_OK
                : NPH_RESULT_OUT_OF_MEMORY;
        if (r == NPH_RESULT_OK) {
            memset(b.class_group, 0xFF, (size_t)c * sizeof(u32));
            for (u32 i = 0; i < c; ++i) {
                b.class_dist[i] = -1.0F;
            }
        }
    }
    if (r == NPH_RESULT_OK) {
        b.ground.positions = in->positions;
        b.ground.tris = in->indices;
        b.ground.tri_count = in->index_count / 3U;
        r = grid_index(&b.ground, b.alloc);
    }
    // Level 0: the input's meshlets as they are.
    const NphRenderMeshlets *m0 = in->meshlets;
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&out->clusters.meshlets, m0->meshlets.data, m0->meshlets.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&out->clusters.bounds, m0->bounds.data, m0->bounds.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&out->clusters.vertices, m0->vertices.data, m0->vertices.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&out->clusters.triangles, m0->triangles.data, m0->triangles.len);
    }
    for (u32 i = 0; i < m0->meshlets.len && r == NPH_RESULT_OK; ++i) {
        const NphRenderClusterLod lod = {NPH_RENDER_LOD_NONE, NPH_RENDER_LOD_NONE, 0U, 0U};
        r = NPH_ARRAY_PUSH(&out->lods, lod);
        if (r == NPH_RESULT_OK) {
            const NphRenderMeshletBounds *bd = &m0->bounds.data[i];
            r = NPH_ARRAY_PUSH(&b.own_sphere, nph_sphere(bd->center, bd->radius));
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&b.own_error, 0.0F);
        }
    }
    out->level0_clusters = m0->meshlets.len;
    out->levels = 1U;
    u32 first = 0U;
    u32 count = m0->meshlets.len;
    while (r == NPH_RESULT_OK && count > 1U &&
           (in->max_levels == 0U || out->levels < in->max_levels)) {
        u32 made = 0U;
        u32 before = 0U;
        u32 after = 0U;
        r = build_level(&b, out->levels - 1U, first, count, &made, &before, &after);
        if (r != NPH_RESULT_OK) {
            break;
        }
        first += count;
        count = made;
        out->levels += 1U;
        if ((f32)after > STALL_RATIO * (f32)before) {
            break;
        }
    }
    // The root groups: the coarsest level's clusters, grouped.
    if (r == NPH_RESULT_OK && count != 0U) {
        r = build_roots(&b, first, count);
    }
    if (r == NPH_RESULT_OK && out->groups.len != 0U) {
        r = build_hierarchy(&b);
    }
    build_free(&b);
    return r;
}
