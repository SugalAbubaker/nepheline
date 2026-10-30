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
#include <nepheline/core/hashmap.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>

#include "cluster_lod_internal.h"

#include <math.h>
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
// A group of fewer triangles stays triangles (ADR-0061): it costs little
// drawn as it is.
#define VOXEL_MIN_TRIANGLES 256U
// A group is aggregate geometry, which alone becomes voxels, where its
// pieces (triangles connected through shared positions) average at most
// this many triangles: a surface that stalls on its borders or seams (an
// open sheet's coarsest groups, a flat-shaded mesh's) is one piece or a few,
// and stays triangles.
#define VOXEL_PIECE_TRIANGLES 32U

void nph_render_cluster_lods_init(NphRenderClusterLods *l, NphAllocator *alloc)
{
    NPH_ASSERT(l != NULL && alloc != NULL);
    memset(l, 0, sizeof *l);
    nph_render_meshlets_init(&l->clusters, alloc);
    NPH_ARRAY_INIT(&l->lods, alloc);
    NPH_ARRAY_INIT(&l->groups, alloc);
    NPH_ARRAY_INIT(&l->group_clusters, alloc);
    NPH_ARRAY_INIT(&l->nodes, alloc);
    NPH_ARRAY_INIT(&l->bricks, alloc);
    NPH_ARRAY_INIT(&l->voxels, alloc);
    NPH_ARRAY_INIT(&l->added, alloc);
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
    NPH_ARRAY_FREE(&l->bricks);
    NPH_ARRAY_FREE(&l->voxels);
    NPH_ARRAY_FREE(&l->added);
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
NphSphere lod_sphere_hull(const NphSphere *spheres, u32 count)
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
// Cuts at uniform thresholds
// ---------------------------------------------------------------------------

// floor(log2(x)) for a positive normal float, from its exponent alone.
static i32 floor_log2(f32 x)
{
    return (i32)((nph_f32_to_bits(x) >> 23U) & 0xFFU) - 127;
}

// 2^e as a float, e within the normal range.
static f32 pow2(i32 e)
{
    return nph_f32_from_bits((u32)(e + 127) << 23U);
}

// The exponent of the smallest power of two at least x > 0: a subnormal x
// gives -126, at or below every threshold.
static i32 ceil_log2(f32 x)
{
    const u32 bits = nph_f32_to_bits(x);
    const i32 e = (i32)((bits >> 23U) & 0xFFU) - 127;
    if (e < -126) {
        return -126;
    }
    return (bits & 0x7FFFFFU) != 0U ? e + 1 : e;
}

// The cuts [*i0, *i1) of `count` from 2^exp0 that hold cluster c: its
// maker's error is at most T = 2^j from j = ceil_log2(made) on, and its
// consumer's above T for j below ceil_log2(consumed) (T a power of two).
static void cut_run(const NphRenderClusterLod *lods, const NphRenderLodGroup *groups, u32 c,
                    i32 exp0, u32 count, u32 *i0, u32 *i1)
{
    const u32 maker = lods[c].group;
    const f32 made = maker == NPH_RENDER_LOD_NONE ? 0.0F : groups[maker].error;
    const u32 parent = lods[c].parent;
    i32 lo = 0;
    if (made > 0.0F) {
        lo = nph_max_i32(ceil_log2(made) - exp0, 0);
    }
    i32 hi = (i32)count;
    if (parent != NPH_RENDER_LOD_NONE) {
        const f32 consumed = groups[parent].error;
        hi = consumed > 0.0F ? nph_min_i32(ceil_log2(consumed) - exp0, (i32)count) : 0;
    }
    *i0 = (u32)nph_min_i32(lo, (i32)count);
    *i1 = (u32)nph_max_i32(hi, (i32)*i0);
}

NphResult nph_render_lod_cuts(const NphRenderClusterLod *lods, u32 cluster_count,
                              const NphRenderLodGroup *groups, u32 group_count, u32 max_entries,
                              i32 *out_exp0, u32 *out_count, NphArrayU32 *table)
{
    NPH_ASSERT(out_exp0 != NULL && out_count != NULL && table != NULL);
    NPH_ARRAY_CLEAR(table);
    // The errors' range, short of the root groups' and of zero.
    f32 lo = NPH_F32_MAX;
    f32 hi = 0.0F;
    for (u32 g = 0; g < group_count; ++g) {
        const f32 e = groups[g].error;
        if (e > 0.0F && e < NPH_F32_MAX) {
            lo = nph_f32_min(lo, e);
            hi = nph_f32_max(hi, e);
        }
    }
    i32 exp0 = -126;
    u32 count = 1U;
    if (hi > 0.0F) {
        // Clamped to the normal floats: an error below 2^-126 is level 0's
        // business, and none reaches 2^127.
        exp0 = nph_max_i32(floor_log2(nph_f32_max(lo, pow2(-126))), -126);
        const i32 top = nph_min_i32(floor_log2(hi) + 1, 126);
        count = (u32)(top - exp0 + 1);
        if (count > NPH_RENDER_LOD_MAX_CUTS) {
            exp0 = top - (i32)NPH_RENDER_LOD_MAX_CUTS + 1;
            count = NPH_RENDER_LOD_MAX_CUTS;
        }
    }
    // Each cut's size, then the finest dropped until the table fits.
    u32 sizes[NPH_RENDER_LOD_MAX_CUTS];
    memset(sizes, 0, sizeof sizes);
    for (u32 c = 0; c < cluster_count; ++c) {
        u32 i0 = 0U;
        u32 i1 = 0U;
        cut_run(lods, groups, c, exp0, count, &i0, &i1);
        for (u32 i = i0; i < i1; ++i) {
            sizes[i] += 1U;
        }
    }
    u64 total = (u64)count + 1U;
    for (u32 i = 0; i < count; ++i) {
        total += sizes[i];
    }
    u32 skip = 0U;
    while (total > max_entries && skip + 1U < count) {
        total -= (u64)sizes[skip] + 1U;
        ++skip;
    }
    if (total > max_entries) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    exp0 += (i32)skip;
    count -= skip;
    NPH_TRY(NPH_ARRAY_RESIZE(table, (u32)total));
    // The offsets, then each cut's clusters in index order through a
    // cursor each.
    u32 cursor[NPH_RENDER_LOD_MAX_CUTS];
    u32 at = count + 1U;
    for (u32 i = 0; i < count; ++i) {
        table->data[i] = at;
        cursor[i] = at;
        at += sizes[skip + i];
    }
    table->data[count] = at;
    for (u32 c = 0; c < cluster_count; ++c) {
        u32 i0 = 0U;
        u32 i1 = 0U;
        cut_run(lods, groups, c, exp0, count, &i0, &i1);
        for (u32 i = i0; i < i1; ++i) {
            table->data[cursor[i]++] = c;
        }
    }
    *out_exp0 = exp0;
    *out_count = count;
    return NPH_RESULT_OK;
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
    f64 level_cost_limit; // the level's error, squared: no collapse costs more
    f64 level_error;      // the error before it, which errors do not fall below
    // Per class: the group of the level that last took it, and whether two
    // groups share it (scratch, NPH_U32_MAX / 0 between levels).
    u32 *class_group;
    u8 *locked;
    f32 *class_dist;      // per class: its points' measured distance this level (< 0: unknown)
    u32 *class_tri_count; // per class: scratch for its triangles in a level's result (0 between)
    u32 *class_tri_start;
    LevelGrid ground; // level 0's triangles: the mesh the levels are measured back to
    // Voxels (ADR-0061), per cluster of every level: its cell (0 for
    // triangles), and the run of the triangles it was made from in
    // `sources` (the level being built's inputs') or `next_sources` (its
    // results'), which take each other's place between levels.
    NphArrayF32 cell_of;
    NphArrayU32 source_start;
    NphArrayU32 source_count;
    LodVoxelSourceArray sources;
    LodVoxelSourceArray next_sources;
    // With the input's texture coordinates: the vertices' positions and texture
    // coordinates, the added ones after the input's. `mesh.positions` and `uvs`
    // read them, and stay put while a level's groups are built.
    NPH_ARRAY(NphVec3) grown_positions;
    NPH_ARRAY(NphVec2) grown_uvs;
    NPH_ARRAY(NphVec3) grown_normals;
    const NphVec3 *normals; // per vertex, those added too, or NULL
    const NphVec2 *uvs;
} Build;

// What one group of a level makes, in arrays of its own (offsets into
// them): groups that share no class may be built at once, each on a thread
// of its own, and all are joined to the level's arrays in group order once
// the level's groups are done, so the result is the same however many
// threads build it.
typedef struct GroupOut {
    NphRenderLodGroup rec;     // its record (the place of its clusters' list set at the join)
    NphSphere sphere;          // round its clusters: the own sphere of the clusters it made
    NphRenderClusterLods part; // the clusters it made, their places, bricks and voxels
    NphArrayF32 cell_of;       // per voxel cluster made: its cell
    NphArrayU32 source_start;  // per voxel cluster made: its run of `sources`
    NphArrayU32 source_count;
    LodVoxelSourceArray sources;        // the triangles its voxel clusters were made from
    NphArrayU32 orphans;                // the classes its simplification left without a triangle
    NphRenderLodAddedVertexArray added; // its added vertices (LOD_ADDED_BASE up, in `part`)
    u64 before;                         // the primitives (triangles and voxels) it took and made
    u64 after;
    bool voxels;  // it made voxels: its error is its cell's, not measured
    bool changed; // its triangles became voxels
    NphResult result;
} GroupOut;

static void group_out_init(GroupOut *o, NphAllocator *a)
{
    memset(o, 0, sizeof *o);
    nph_render_cluster_lods_init(&o->part, a);
    NPH_ARRAY_INIT(&o->cell_of, a);
    NPH_ARRAY_INIT(&o->source_start, a);
    NPH_ARRAY_INIT(&o->source_count, a);
    NPH_ARRAY_INIT(&o->sources, a);
    NPH_ARRAY_INIT(&o->orphans, a);
    NPH_ARRAY_INIT(&o->added, a);
}

static void group_out_free(GroupOut *o)
{
    nph_render_cluster_lods_free(&o->part);
    NPH_ARRAY_FREE(&o->cell_of);
    NPH_ARRAY_FREE(&o->source_start);
    NPH_ARRAY_FREE(&o->source_count);
    NPH_ARRAY_FREE(&o->sources);
    NPH_ARRAY_FREE(&o->orphans);
    NPH_ARRAY_FREE(&o->added);
}

// fn over [0, count) in pieces of `batch`, as jobs on `jobs` where given
// and its pool takes them, else here; returns once every piece is done.
static void run_range(NphJobSystem *jobs, u32 count, u32 batch, NphJobRangeFn fn, void *user)
{
    if (jobs != NULL && count > batch) {
        NphJobCounter counter = NPH_JOB_COUNTER_INIT;
        if (nph_job_parallel_for(jobs, count, batch, fn, user, NPH_JOB_PRIORITY_NORMAL, &counter) ==
            NPH_RESULT_OK) {
            nph_job_wait(jobs, &counter);
            return;
        }
        // A full job pool is not a failure: the range runs here.
    }
    fn(user, 0U, count);
}

typedef NPH_ARRAY(Ranked) RankedArray;

// A voxel cluster's record has no vertices (ADR-0061).
static bool is_voxel_cluster(const NphRenderMeshlet *m)
{
    return m->vertex_count == 0U;
}

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
// Where a vertex of a group's results stands.
static NphVec3 group_vertex_position(const Build *b, const GroupOut *o, u32 v)
{
    return b->mesh.positions[v >= LOD_ADDED_BASE ? o->added.data[v - LOD_ADDED_BASE].position : v];
}

static NphResult recluster(const Build *b, GroupOut *o, const u32 *indices, const u32 *submeshes,
                           u32 tris, u32 group, u32 level)
{
    NphAllocator *a = b->alloc;
    NphRenderClusterLods *out = &o->part;
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
    NphHashMap numbers; // mesh vertex -> local
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));
    NphResult r = order != NULL && local_indices != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    bool numbering = false;
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&numbers, a, &desc);
        numbering = r == NPH_RESULT_OK;
    }
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_reserve(&numbers, tris + 16U);
    }
    if (r == NPH_RESULT_OK) {
        for (u32 t = 0; t < tris; ++t) {
            order[t].submesh = submeshes[t];
            order[t].order = t;
        }
        if (tris > 1U) {
            qsort(order, tris, sizeof(SubTri), compare_subtri);
        }
    }
    // The group's own numbering of the vertices, in the order its sorted
    // triangles reach them.
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
            const u32 *known = (const u32 *)nph_hashmap_find_u64(&numbers, v);
            u32 number = vertices.len;
            if (known != NULL) {
                number = *known;
            } else {
                r = nph_hashmap_insert_u64(&numbers, v, &number);
                if (r == NPH_RESULT_OK) {
                    r = NPH_ARRAY_PUSH(&vertices, v);
                }
                if (r == NPH_RESULT_OK) {
                    const NphVec3 at = group_vertex_position(b, o, v);
                    r = NPH_ARRAY_PUSH(&positions, at);
                }
            }
            local_indices[(u64)i * 3U + k] = number;
        }
    }
    if (numbering) {
        nph_hashmap_free(&numbers);
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

// A group's triangles, from its clusters: mesh vertices and submeshes (a
// voxel cluster has none).
static NphResult gather(const Build *b, const u32 *clusters, u32 count, NphArrayU32 *indices,
                        NphArrayU32 *submeshes)
{
    NPH_ARRAY_CLEAR(indices);
    NPH_ARRAY_CLEAR(submeshes);
    const NphRenderMeshlets *c = &b->out->clusters;
    for (u32 i = 0; i < count; ++i) {
        const NphRenderMeshlet *m = &c->meshlets.data[clusters[i]];
        if (is_voxel_cluster(m)) {
            continue;
        }
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

// The grid of clusters [first, end)'s triangles (voxel clusters have none).
static NphResult grid_build(Build *b, LevelGrid *g, u32 first, u32 end)
{
    const NphRenderMeshlets *c = &b->out->clusters;
    for (u32 i = first; i < end; ++i) {
        const NphRenderMeshlet *m = &c->meshlets.data[i];
        if (is_voxel_cluster(m)) {
            continue;
        }
        for (u32 k = 0; k < (u32)m->triangle_count * 3U; ++k) {
            const u8 corner = c->triangles.data[(u64)m->triangle_offset * 3U + k];
            NPH_TRY(NPH_ARRAY_PUSH(&g->owned, c->vertices.data[m->vertex_offset + corner]));
        }
    }
    g->positions = b->mesh.positions;
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
    const NphVec3 at = m->positions[m->class_vertex[c]];
    f32 worst = 0.0F;
    for (u32 p = c; p != NPH_U32_MAX; p = m->absorbed_next[p]) {
        const NphVec3 q = m->positions[m->class_vertex[p]];
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
// Voxel groups (ADR-0061)
// ---------------------------------------------------------------------------

// The voxels of voxel cluster `cl`.
static u32 cluster_voxels(const Build *b, u32 cl)
{
    const NphRenderMeshlet *m = &b->out->clusters.meshlets.data[cl];
    u32 n = 0U;
    for (u32 k = 0; k < m->triangle_count; ++k) {
        const NphRenderVoxelBrick *br = &b->out->bricks.data[m->vertex_offset + k];
        n += nph_popcount_u32(br->mask[0]) + nph_popcount_u32(br->mask[1]);
    }
    return n;
}

// Whether a group's triangles (their submeshes) may all become voxels.
static bool voxelizable(const Build *b, const u32 *submeshes, u32 tris)
{
    const NphRenderVoxelSampler *s = b->in->voxels;
    if (s == NULL) {
        return false;
    }
    for (u32 t = 0; t < tris; ++t) {
        if (s->submesh_surface[submeshes[t]] == NPH_RENDER_VOXEL_SURFACE_NONE) {
            return false;
        }
    }
    return true;
}

static u32 find_root(u32 *parent, u32 x)
{
    NPH_ASSERT(parent != NULL);
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

// The pieces of a group's triangles (`indices`, mesh vertices): connected
// through their vertices' classes (positions), by union-find over the
// classes the triangles touch, numbered locally (a hash map of the
// group's own: groups may count at once).
static NphResult count_pieces(const Build *b, const u32 *indices, u32 tris, u32 *out_pieces)
{
    const LodMesh *m = &b->mesh;
    *out_pieces = 0U;
    NphArrayU32 local; // per corner: its class's local number
    NPH_ARRAY_INIT(&local, b->alloc);
    NphArrayU32 parent;
    NPH_ARRAY_INIT(&parent, b->alloc);
    NphHashMap numbers; // class -> local number
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));
    NphResult r = nph_hashmap_init(&numbers, b->alloc, &desc);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    r = nph_hashmap_reserve(&numbers, tris + 16U);
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&local, tris * 3U);
    }
    for (u32 i = 0; i < tris * 3U && r == NPH_RESULT_OK; ++i) {
        const u32 c = m->class_of[indices[i]];
        const u32 *known = (const u32 *)nph_hashmap_find_u64(&numbers, c);
        if (known != NULL) {
            local.data[i] = *known;
            continue;
        }
        const u32 number = parent.len; // its own root
        r = nph_hashmap_insert_u64(&numbers, c, &number);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&parent, number);
        }
        local.data[i] = number;
    }
    nph_hashmap_free(&numbers);
    for (u32 t = 0; t < tris && r == NPH_RESULT_OK; ++t) {
        // Every triangle's classes were numbered above (the analyzer cannot
        // tell the arrays are not empty).
        NPH_ASSERT(parent.data != NULL && parent.len != 0U && local.data != NULL);
        const u32 a = find_root(parent.data, local.data[(u64)t * 3U]);
        for (u32 k = 1U; k < 3U; ++k) {
            const u32 o = find_root(parent.data, local.data[(u64)t * 3U + k]);
            if (o != a) {
                parent.data[o] = a;
            }
        }
    }
    u32 pieces = 0U;
    for (u32 i = 0; i < parent.len; ++i) {
        pieces += parent.data[i] == i ? 1U : 0U;
    }
    NPH_ARRAY_FREE(&local);
    NPH_ARRAY_FREE(&parent);
    *out_pieces = pieces;
    return r;
}

// The finest cell the coordinates allow for the sources (their largest
// magnitude, `*out_reach`, within LOD_VOXEL_COORD_LIMIT / 2 cells), and
// their area.
static f64 voxel_floor(const LodVoxelInput *vin, f64 *out_area, f64 *out_reach)
{
    f64 reach = 0.0;
    f64 area = 0.0;
    for (u32 s = 0; s < vin->source_count; ++s) {
        f64 p[3][3];
        for (u32 k = 0; k < 3U; ++k) {
            const NphVec3 q = vin->positions[vin->sources[s].v[k]];
            p[k][0] = (f64)q.x;
            p[k][1] = (f64)q.y;
            p[k][2] = (f64)q.z;
            for (u32 a = 0; a < 3U; ++a) {
                reach = nph_f64_max(reach, nph_f64_abs(p[k][a]));
            }
        }
        const f64 e0[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        const f64 e1[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        const f64 n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                          e0[0] * e1[1] - e0[1] * e1[0]};
        area += 0.5 * nph_f64_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
    *out_area = area;
    *out_reach = reach;
    return nph_f64_max(reach / (0.5 * (f64)LOD_VOXEL_COORD_LIMIT), 1e-30);
}

// The smallest power of two at least `least` (> 0) whose cells the sources
// overlap at most `budget` times. The count only falls as the cell grows
// (each cell of a size holds eight of the next finer, on one grid), so the
// search starts near where about `budget` cells cover the sources' area
// and walks from there. Past the sources' reach it falls no further: a
// cell then holds them along each axis, or two do where they lie across
// zero, at which the grid of every cell size divides the axis. A budget
// below that count (a group of a few voxels across the mesh's origin) takes
// the first cell past the reach.
static NphResult voxel_cell(const LodVoxelInput *vin, f64 least, u32 budget, f64 *out_cell)
{
    f64 area = 0.0;
    f64 reach = 0.0;
    least = nph_f64_max(least, voxel_floor(vin, &area, &reach));
    f64 cell = ldexp(1.0, (int)ceil(log2(least)));
    const f64 guess = nph_f64_sqrt(area / (f64)nph_max_u32(budget, 1U));
    while (cell < guess) {
        cell *= 2.0;
    }
    u32 n = 0U;
    NPH_TRY(lod_voxel_count(vin, cell, budget, &n));
    if (n <= budget) {
        while (cell * 0.5 >= least) {
            u32 m = 0U;
            NPH_TRY(lod_voxel_count(vin, cell * 0.5, budget, &m));
            if (m > budget) {
                break;
            }
            cell *= 0.5;
        }
    } else {
        while (n > budget && cell <= reach) {
            cell *= 2.0;
            NPH_TRY(lod_voxel_count(vin, cell, budget, &n));
        }
    }
    *out_cell = cell;
    return NPH_RESULT_OK;
}

typedef struct CellOrder {
    u64 brick; // the brick's key
    u32 bit;   // the cell's bit within it
    u32 cell;  // index into the cells
} CellOrder;

static int compare_cell_order(const void *x, const void *y)
{
    const CellOrder *p = (const CellOrder *)x;
    const CellOrder *q = (const CellOrder *)y;
    if (p->brick != q->brick) {
        return p->brick < q->brick ? -1 : 1;
    }
    return p->bit < q->bit ? -1 : (p->bit > q->bit ? 1 : 0);
}

typedef struct BrickRun {
    i64 bc[3]; // the brick's coordinates (its lowest cell's over four)
    u32 first; // its cells in the cell order
    u32 count;
    u64 sort; // scratch for the splits
} BrickRun;

static int compare_runs(const void *x, const void *y)
{
    const BrickRun *p = (const BrickRun *)x;
    const BrickRun *q = (const BrickRun *)y;
    return p->sort < q->sort ? -1 : (p->sort > q->sort ? 1 : 0);
}

static i64 floor_div4(i64 x)
{
    return x >= 0 ? x / 4 : -((-x + 3) / 4);
}

// The clusters of the bricks runs[first, first + count): split at their
// voxels' median along the longest axis of the bricks' span until each fits
// a voxel cluster, compact parts for the culling; each part's (first,
// count) appended to `ranges`. The order within a part is kept, so the
// split is the same for the same input.
static NphResult split_runs(BrickRun *runs, u32 first, u32 count, NphArrayU32 *ranges)
{
    u32 voxels = 0U;
    i64 lo[3] = {INT64_MAX, INT64_MAX, INT64_MAX};
    i64 hi[3] = {INT64_MIN, INT64_MIN, INT64_MIN};
    for (u32 k = first; k < first + count; ++k) {
        voxels += runs[k].count;
        for (u32 a = 0; a < 3U; ++a) {
            lo[a] = runs[k].bc[a] < lo[a] ? runs[k].bc[a] : lo[a];
            hi[a] = runs[k].bc[a] > hi[a] ? runs[k].bc[a] : hi[a];
        }
    }
    if (count <= NPH_RENDER_VOXEL_CLUSTER_BRICKS && voxels <= NPH_RENDER_VOXEL_CLUSTER_VOXELS) {
        NPH_TRY(NPH_ARRAY_PUSH(ranges, first));
        return NPH_ARRAY_PUSH(ranges, count);
    }
    u32 axis = 0U;
    for (u32 a = 1U; a < 3U; ++a) {
        if (hi[a] - lo[a] > hi[axis] - lo[axis]) {
            axis = a;
        }
    }
    for (u32 k = first; k < first + count; ++k) {
        runs[k].sort = ((u64)(runs[k].bc[axis] - lo[axis]) << 32U) | (u64)(k - first);
    }
    qsort(runs + first, count, sizeof(BrickRun), compare_runs);
    u32 split = 0U;
    u32 acc = 0U;
    while (split + 1U < count && acc + runs[first + split].count <= voxels / 2U) {
        acc += runs[first + split].count;
        ++split;
    }
    split = nph_max_u32(split, 1U);
    NPH_TRY(split_runs(runs, first, split, ranges));
    return split_runs(runs, first + split, count - split, ranges);
}

// The submeshes a cluster's voxels show, with their counts.
typedef struct SubmeshVotes {
    NphArrayU32 submesh;
    NphArrayU32 votes;
} SubmeshVotes;

static NphResult vote(SubmeshVotes *v, u32 submesh)
{
    for (u32 k = 0; k < v->submesh.len; ++k) {
        if (v->submesh.data[k] == submesh) {
            v->votes.data[k] += 1U;
            return NPH_RESULT_OK;
        }
    }
    NPH_TRY(NPH_ARRAY_PUSH(&v->submesh, submesh));
    return NPH_ARRAY_PUSH(&v->votes, 1U);
}

// The submesh most of them show (the lowest on a tie).
static u32 elected(const SubmeshVotes *v)
{
    u32 best = 0U;
    for (u32 k = 1U; k < v->submesh.len; ++k) {
        if (v->votes.data[k] > v->votes.data[best] ||
            (v->votes.data[k] == v->votes.data[best] &&
             v->submesh.data[k] < v->submesh.data[best])) {
            best = k;
        }
    }
    return v->submesh.len != 0U ? v->submesh.data[best] : 0U;
}

NphResult lod_voxel_emit(NphRenderClusterLods *out, NphAllocator *a, const LodVoxelCellArray *cells,
                         f64 cell, u32 group, u32 level, u32 *cell_cluster, u32 *out_made)
{
    NPH_ASSERT(out != NULL && cells != NULL && cell_cluster != NULL && out_made != NULL);
    const u32 n = cells->len;
    *out_made = 0U;
    CellOrder *order = NPH_ALLOC_ARRAY(a, CellOrder, nph_max_u32(n, 1U));
    NPH_ARRAY(BrickRun) runs;
    NPH_ARRAY_INIT(&runs, a);
    NphArrayU32 ranges;
    NPH_ARRAY_INIT(&ranges, a);
    SubmeshVotes votes;
    NPH_ARRAY_INIT(&votes.submesh, a);
    NPH_ARRAY_INIT(&votes.votes, a);
    NphResult r = order != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    const i64 bias = LOD_VOXEL_COORD_LIMIT;
    for (u32 i = 0; i < n && r == NPH_RESULT_OK; ++i) {
        const LodVoxelCell *c = &cells->data[i];
        i64 bc[3];
        u32 bit = 0U;
        for (u32 k = 0; k < 3U; ++k) {
            bc[k] = floor_div4((i64)c->c[k]);
            bit += (u32)((i64)c->c[k] - 4 * bc[k]) << (2U * k);
        }
        order[i].brick =
            ((u64)(bc[2] + bias) << 42U) | ((u64)(bc[1] + bias) << 21U) | (u64)(bc[0] + bias);
        order[i].bit = bit;
        order[i].cell = i;
    }
    if (r == NPH_RESULT_OK && n > 1U) {
        qsort(order, n, sizeof(CellOrder), compare_cell_order);
    }
    for (u32 i = 0; i < n && r == NPH_RESULT_OK;) {
        u32 j = i;
        while (j < n && order[j].brick == order[i].brick) {
            ++j;
        }
        BrickRun run;
        memset(&run, 0, sizeof run);
        const LodVoxelCell *c = &cells->data[order[i].cell];
        for (u32 k = 0; k < 3U; ++k) {
            run.bc[k] = floor_div4((i64)c->c[k]);
        }
        run.first = i;
        run.count = j - i;
        r = NPH_ARRAY_PUSH(&runs, run);
        i = j;
    }
    if (r == NPH_RESULT_OK && runs.len != 0U) {
        r = split_runs(runs.data, 0U, runs.len, &ranges);
    }
    const u32 first_cluster = out->clusters.meshlets.len;
    const f64 step = cell / (f64)NPH_RENDER_VOXEL_BOX_STEPS;
    for (u32 q = 0; q + 1U < ranges.len && r == NPH_RESULT_OK; q += 2U) {
        const u32 k = q / 2U;
        NphRenderMeshlet ml;
        memset(&ml, 0, sizeof ml);
        ml.vertex_offset = out->bricks.len;
        ml.triangle_count = (u16)ranges.data[q + 1U];
        f64 lo[3] = {NPH_F64_MAX, NPH_F64_MAX, NPH_F64_MAX};
        f64 hi[3] = {-NPH_F64_MAX, -NPH_F64_MAX, -NPH_F64_MAX};
        NPH_ARRAY_CLEAR(&votes.submesh);
        NPH_ARRAY_CLEAR(&votes.votes);
        const u32 run_end = ranges.data[q] + ranges.data[q + 1U];
        for (u32 ri = ranges.data[q]; ri < run_end && r == NPH_RESULT_OK; ++ri) {
            const BrickRun *run = &runs.data[ri];
            NphRenderVoxelBrick br;
            memset(&br, 0, sizeof br);
            br.origin =
                nph_vec3((f32)((f64)run->bc[0] * 4.0 * cell), (f32)((f64)run->bc[1] * 4.0 * cell),
                         (f32)((f64)run->bc[2] * 4.0 * cell));
            br.cell = (f32)cell;
            br.first_voxel = out->voxels.len;
            for (u32 o = run->first; o < run->first + run->count && r == NPH_RESULT_OK; ++o) {
                const u32 ci = order[o].cell;
                const LodVoxelCell *c = &cells->data[ci];
                br.mask[order[o].bit >> 5U] |= 1U << (order[o].bit & 31U);
                cell_cluster[ci] = k;
                r = NPH_ARRAY_PUSH(&out->voxels, c->voxel);
                if (r == NPH_RESULT_OK) {
                    r = vote(&votes, c->submesh);
                }
                // The footprint's box, mesh space; a capsule reaches past it by
                // its radius, within the cell.
                const u32 box = c->voxel.box;
                const f64 reach =
                    (box >> NPH_RENDER_VOXEL_SHAPE_SHIFT) == NPH_RENDER_VOXEL_SHAPE_CAPSULE
                        ? (f64)nph_render_voxel_radius_decode((c->voxel.material >> 26U) & 63U) *
                              cell
                        : 0.0;
                for (u32 ax = 0; ax < 3U; ++ax) {
                    const f64 base = (f64)c->c[ax] * cell;
                    const f64 bl = base + (f64)((box >> (5U * ax)) & 31U) * step - reach;
                    const f64 bh =
                        base + (f64)(((box >> (15U + 5U * ax)) & 31U) + 1U) * step + reach;
                    lo[ax] = nph_f64_min(lo[ax], nph_f64_max(bl, base));
                    hi[ax] = nph_f64_max(hi[ax], nph_f64_min(bh, base + cell));
                }
            }
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&out->bricks, br);
            }
        }
        ml.submesh = elected(&votes);
        // The sphere round the footprints' box, its radius rounded up past
        // the rounding of its centre to floats.
        NphRenderMeshletBounds bd;
        memset(&bd, 0, sizeof bd);
        bd.center = nph_vec3((f32)(0.5 * (lo[0] + hi[0])), (f32)(0.5 * (lo[1] + hi[1])),
                             (f32)(0.5 * (lo[2] + hi[2])));
        const f64 center[3] = {(f64)bd.center.x, (f64)bd.center.y, (f64)bd.center.z};
        f64 r2 = 0.0;
        for (u32 ax = 0; ax < 3U; ++ax) {
            const f64 d =
                nph_f64_max(nph_f64_abs(lo[ax] - center[ax]), nph_f64_abs(hi[ax] - center[ax]));
            r2 += d * d;
        }
        const f64 radius = nph_f64_sqrt(r2);
        f32 rf = (f32)radius;
        while ((f64)rf < radius) {
            rf = nph_f32_from_bits(nph_f32_to_bits(rf) + 1U);
        }
        bd.radius = rf;
        bd.cone_cutoff = 127; // with a zero axis: never culled as facing away
        bd.cone_apex = bd.center;
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&out->clusters.meshlets, ml);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&out->clusters.bounds, bd);
        }
        if (r == NPH_RESULT_OK) {
            const NphRenderClusterLod lod = {group, NPH_RENDER_LOD_NONE, level, 0U};
            r = NPH_ARRAY_PUSH(&out->lods, lod);
        }
    }
    *out_made = out->clusters.meshlets.len - first_cluster;
    NPH_ARRAY_FREE(&votes.submesh);
    NPH_ARRAY_FREE(&votes.votes);
    NPH_ARRAY_FREE(&ranges);
    NPH_ARRAY_FREE(&runs);
    if (order != NULL) {
        NPH_FREE_ARRAY(a, CellOrder, order, nph_max_u32(n, 1U));
    }
    return r;
}

// The voxel clusters of group `group` (level `level`) from its cells
// (lod_voxel_emit), with their cells; each source to the cluster of its home
// cell (the first cluster where it has none), in `next_sources` cluster by
// cluster. `*out_voxels` takes the voxels made.
static NphResult emit_voxels(const Build *b, GroupOut *dst, const LodVoxelCellArray *cells,
                             f64 cell, u32 group, u32 level, const LodVoxelSource *sources,
                             const u32 *source_cell, u32 source_count, u32 *out_voxels)
{
    NphAllocator *a = b->alloc;
    NphRenderClusterLods *out = &dst->part;
    const u32 n = cells->len;
    *out_voxels = n;
    u32 *cell_cluster = NPH_ALLOC_ARRAY(a, u32, nph_max_u32(n, 1U));
    NphArrayU32 counts; // sources per new cluster
    NPH_ARRAY_INIT(&counts, a);
    u32 made = 0U;
    NphResult r = cell_cluster != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        r = lod_voxel_emit(out, a, cells, cell, group, level, cell_cluster, &made);
    }
    for (u32 k = 0; k < made && r == NPH_RESULT_OK; ++k) {
        r = NPH_ARRAY_PUSH(&dst->cell_of, (f32)cell);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&counts, 0U);
        }
    }
    const u32 first_cluster = out->clusters.meshlets.len - made;
    // The sources, cluster by cluster (a counting sort, in source order).
    for (u32 s = 0; s < source_count && r == NPH_RESULT_OK && made != 0U; ++s) {
        const u32 k = source_cell[s] != NPH_U32_MAX ? cell_cluster[source_cell[s]] : 0U;
        counts.data[k] += 1U;
    }
    u32 at = dst->sources.len;
    for (u32 k = 0; k < made && r == NPH_RESULT_OK; ++k) {
        r = NPH_ARRAY_PUSH(&dst->source_start, at);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&dst->source_count, counts.data[k]);
        }
        at += counts.data[k];
        counts.data[k] = 0U;
    }
    if (r == NPH_RESULT_OK && made != 0U) {
        r = NPH_ARRAY_RESIZE(&dst->sources, at);
    }
    for (u32 s = 0; s < source_count && r == NPH_RESULT_OK && made != 0U; ++s) {
        const u32 k = source_cell[s] != NPH_U32_MAX ? cell_cluster[source_cell[s]] : 0U;
        dst->sources.data[dst->source_start.data[first_cluster + k] + counts.data[k]] = sources[s];
        counts.data[k] += 1U;
    }
    NPH_ARRAY_FREE(&counts);
    if (cell_cluster != NULL) {
        NPH_FREE_ARRAY(a, u32, cell_cluster, nph_max_u32(n, 1U));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

// One group of a level: its clusters `cl` (`count`), simplified or made
// voxels into the next level's clusters, in `o` (initialised by the
// caller), as the output's group `group`. It reads the level's clusters
// and the build's per-cluster arrays, which no group writes, and reads and
// writes the mesh's quadrics, normals and lists for its own classes alone
// (lod_simplify), so groups that share no class may be built at once.
static NphResult group_build(Build *b, u32 level, const u32 *cl, u32 count, u32 group, GroupOut *o)
{
    NphAllocator *a = b->alloc;
    const NphRenderClusterLods *out = b->out;
    NphArrayU32 indices;
    NPH_ARRAY_INIT(&indices, a);
    NphArrayU32 submeshes;
    NPH_ARRAY_INIT(&submeshes, a);
    NPH_ARRAY(NphSphere) hull_parts; // its clusters' own spheres
    NPH_ARRAY_INIT(&hull_parts, a);
    // Voxels: the triangles it voxelizes, and their cells.
    LodVoxelSourceArray group_sources;
    NPH_ARRAY_INIT(&group_sources, a);
    NphArrayU32 source_cell;
    NPH_ARRAY_INIT(&source_cell, a);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, a);
    // Its clusters' spheres and errors, and its voxel clusters' cells (the
    // coarsest and the finest) and voxels (ADR-0061).
    NphResult r = NPH_RESULT_OK;
    f32 error = 0.0F;
    bool voxel_in = false;
    f64 cell_in = 0.0;
    f64 cell_in_fine = NPH_F64_MAX;
    u32 voxels_in = 0U;
    for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
        r = NPH_ARRAY_PUSH(&hull_parts, b->own_sphere.data[cl[i]]);
        error = nph_f32_max(error, b->own_error.data[cl[i]]);
        if (is_voxel_cluster(&out->clusters.meshlets.data[cl[i]])) {
            voxel_in = true;
            cell_in = nph_f64_max(cell_in, (f64)b->cell_of.data[cl[i]]);
            cell_in_fine = nph_f64_min(cell_in_fine, (f64)b->cell_of.data[cl[i]]);
            voxels_in += cluster_voxels(b, cl[i]);
        }
    }
    if (r == NPH_RESULT_OK) {
        o->sphere = lod_sphere_hull(hull_parts.data, hull_parts.len);
        r = gather(b, cl, count, &indices, &submeshes);
    }
    const u32 tris = submeshes.len;
    o->before = (u64)tris + voxels_in;
    // What it would voxelize: its triangles as they are before the
    // simplifier rewrites them, then those its voxel clusters were made
    // from.
    bool may_voxelize =
        r == NPH_RESULT_OK &&
        (voxel_in || (tris >= VOXEL_MIN_TRIANGLES && voxelizable(b, submeshes.data, tris)));
    if (may_voxelize && !voxel_in) {
        u32 pieces = 0U;
        r = count_pieces(b, indices.data, tris, &pieces);
        may_voxelize = (u64)pieces * VOXEL_PIECE_TRIANGLES >= tris;
    }
    for (u32 t = 0; t < tris && may_voxelize && r == NPH_RESULT_OK; ++t) {
        LodVoxelSource src;
        src.v[0] = indices.data[(u64)t * 3U];
        src.v[1] = indices.data[(u64)t * 3U + 1U];
        src.v[2] = indices.data[(u64)t * 3U + 2U];
        src.submesh = submeshes.data[t];
        r = NPH_ARRAY_PUSH(&group_sources, src);
    }
    for (u32 i = 0; i < count && voxel_in && r == NPH_RESULT_OK; ++i) {
        if (is_voxel_cluster(&out->clusters.meshlets.data[cl[i]])) {
            r = NPH_ARRAY_PUSH_N(&group_sources, b->sources.data + b->source_start.data[cl[i]],
                                 b->source_count.data[cl[i]]);
        }
    }
    LodVoxelInput vin;
    memset(&vin, 0, sizeof vin);
    vin.positions = b->mesh.positions;
    vin.sources = group_sources.data;
    vin.source_count = group_sources.len;
    vin.sampler = b->in->voxels;
    vin.scratch = a;
    u32 kept = tris;
    f32 simplify_error = 0.0F;
    bool voxels = voxel_in;
    f64 cell = 0.0;
    if (r == NPH_RESULT_OK && !voxel_in) {
        LodSimplifyInput si;
        memset(&si, 0, sizeof si);
        si.mesh = &b->mesh;
        si.indices = indices.data;
        si.submeshes = submeshes.data;
        si.triangle_count = tris;
        si.locked = b->locked;
        si.target_triangles = (tris + 1U) / 2U;
        si.uvs = b->uvs;
        si.normals = b->normals;
        si.added = &o->added;
        si.orphans = &o->orphans;
        si.scratch = a;
        si.cost_limit = b->level_cost_limit;
        si.borders = b->in->voxels == NULL ? 1 : 0;
        r = lod_simplify(&si, &kept, &simplify_error);
        if (r == NPH_RESULT_OK && may_voxelize && tris != 0U) {
            // Voxels where the collapses stall, or where their error
            // passes the voxels' own: cells from twice the error so far,
            // as many as four times the triangles (ADR-0061).
            const bool stalled = (u64)kept * 4U > (u64)tris * 3U;
            const u32 budget = (u32)nph_min_u64((u64)tris * 4U, NPH_U32_MAX);
            const NphResult rc = voxel_cell(&vin, 2.0 * (f64)error, budget, &cell);
            if (rc == NPH_RESULT_OK) {
                voxels = stalled || (f64)simplify_error > cell;
            } else if (rc != NPH_RESULT_OVERFLOW) {
                r = rc; // past the coordinates' reach the group keeps its triangles
            }
            // Or voxels where they cost no more than the triangles kept, at
            // most twice as coarse as those: a level's step, which voxels
            // continue as aggregate triangles do not (the triangles of
            // needles halve as their error doubles, and so outnumber the
            // pixels farther off; voxels of a surface quarter).
            const f64 at = nph_f64_max((f64)error, (f64)simplify_error);
            if (r == NPH_RESULT_OK && !voxels && kept != 0U && at > 0.0) {
                f64 even = 0.0;
                const NphResult re = voxel_cell(&vin, at, kept, &even);
                if (re == NPH_RESULT_OK && even <= 2.0 * at) {
                    voxels = true;
                    cell = even;
                } else if (re != NPH_RESULT_OK && re != NPH_RESULT_OVERFLOW) {
                    r = re;
                }
            }
        }
    } else if (r == NPH_RESULT_OK) {
        // Coarser voxels: at least the coarsest input's cell and twice the
        // finest's, at most half the inputs' primitives. A group of voxels of
        // one cell doubles it; one mixing sizes (its parts became voxels at
        // different levels) brings the fine ones to the coarse ones' cell
        // where that halves them, rather than to twice it, which raised
        // every part's error to twice the coarsest's and so kept the fine
        // parts' finer clusters in cuts out to that error (ADR-0061).
        const u64 budget = nph_max_u64(((u64)tris + voxels_in) / 2U, 1U);
        r = voxel_cell(&vin, nph_f64_max(2.0 * cell_in_fine, cell_in),
                       (u32)nph_min_u64(budget, NPH_U32_MAX), &cell);
    }
    if (r == NPH_RESULT_OK && voxels) {
        r = NPH_ARRAY_RESIZE(&source_cell, nph_max_u32(group_sources.len, 1U));
        if (r == NPH_RESULT_OK) {
            r = lod_voxelize(&vin, cell, &cells, source_cell.data);
        }
        // A group of triangles whose rays hit nothing keeps its
        // triangles; one of voxels may end in none (all masked away).
        if (r == NPH_RESULT_OK && cells.len == 0U && !voxel_in) {
            voxels = false;
        }
    }
    if (r == NPH_RESULT_OK) {
        memset(&o->rec, 0, sizeof o->rec);
        o->rec.center = o->sphere.center;
        o->rec.radius = o->sphere.radius;
        o->rec.error = voxels ? nph_f32_max(error, (f32)cell) : nph_f32_max(error, simplify_error);
        o->rec.level = level;
        o->rec.cluster_count = count;
        o->voxels = voxels;
        if (voxels) {
            u32 made_voxels = 0U;
            NPH_ARRAY_CLEAR(&o->added); // for triangles it does not keep
            r = emit_voxels(b, o, &cells, cell, group, level + 1U, group_sources.data,
                            source_cell.data, group_sources.len, &made_voxels);
            o->after = made_voxels;
            o->changed = !voxel_in;
        } else {
            r = recluster(b, o, indices.data, submeshes.data, kept, group, level + 1U);
            o->after = kept;
        }
    }
    NPH_ARRAY_FREE(&indices);
    NPH_ARRAY_FREE(&submeshes);
    NPH_ARRAY_FREE(&hull_parts);
    NPH_ARRAY_FREE(&group_sources);
    NPH_ARRAY_FREE(&source_cell);
    NPH_ARRAY_FREE(&cells);
    return r;
}

// Group `o`'s results (its clusters `cl`, `count`) after those of the
// groups before it: its record, its clusters' places, the clusters it made
// with their offsets moved past what is already there, their own spheres,
// errors, cells and sources, and its orphans (onto `orphans`).
static NphResult group_join(Build *b, const u32 *cl, u32 count, const GroupOut *o,
                            NphArrayU32 *orphans)
{
    NphRenderClusterLods *out = b->out;
    const u32 group_index = out->groups.len;
    NphRenderLodGroup rec = o->rec;
    rec.cluster_offset = out->group_clusters.len;
    NPH_TRY(NPH_ARRAY_PUSH(&out->groups, rec));
    NPH_TRY(NPH_ARRAY_PUSH_N(&out->group_clusters, cl, count));
    for (u32 i = 0; i < count; ++i) {
        out->lods.data[cl[i]].parent = group_index;
    }
    const NphRenderMeshlets *made = &o->part.clusters;
    const u32 vertex_base = out->clusters.vertices.len;
    const u32 triangle_base = out->clusters.triangles.len / 3U;
    const u32 brick_base = out->bricks.len;
    const u32 voxel_base = out->voxels.len;
    const u32 source_base = b->next_sources.len;
    for (u32 k = 0; k < made->meshlets.len; ++k) {
        NphRenderMeshlet ml = made->meshlets.data[k];
        // A voxel cluster's vertex offset is its first brick's.
        const bool voxel = is_voxel_cluster(&ml);
        if (voxel) {
            ml.vertex_offset += brick_base;
        } else {
            ml.vertex_offset += vertex_base;
            ml.triangle_offset += triangle_base;
        }
        NPH_TRY(NPH_ARRAY_PUSH(&out->clusters.meshlets, ml));
        NPH_TRY(NPH_ARRAY_PUSH(&out->clusters.bounds, made->bounds.data[k]));
        NPH_TRY(NPH_ARRAY_PUSH(&out->lods, o->part.lods.data[k]));
        // The new clusters' own sphere and error: the group's; a triangle
        // cluster has no cell and no sources.
        NPH_TRY(NPH_ARRAY_PUSH(&b->own_sphere, o->sphere));
        NPH_TRY(NPH_ARRAY_PUSH(&b->own_error, o->rec.error));
        NPH_TRY(NPH_ARRAY_PUSH(&b->cell_of, voxel ? o->cell_of.data[k] : 0.0F));
        NPH_TRY(
            NPH_ARRAY_PUSH(&b->source_start, voxel ? source_base + o->source_start.data[k] : 0U));
        NPH_TRY(NPH_ARRAY_PUSH(&b->source_count, voxel ? o->source_count.data[k] : 0U));
    }
    NPH_TRY(NPH_ARRAY_PUSH_N(&out->clusters.vertices, made->vertices.data, made->vertices.len));
    NPH_TRY(NPH_ARRAY_PUSH_N(&out->clusters.triangles, made->triangles.data, made->triangles.len));
    // Its added vertices get their places after the mesh's, in group order.
    const u32 added_first = b->mesh.vertex_count;
    for (u32 k = vertex_base; k < out->clusters.vertices.len; ++k) {
        const u32 v = out->clusters.vertices.data[k];
        if (v >= LOD_ADDED_BASE) {
            out->clusters.vertices.data[k] = added_first + (v - LOD_ADDED_BASE);
        }
    }
    for (u32 k = 0; k < o->added.len; ++k) {
        const NphRenderLodAddedVertex a = o->added.data[k];
        const NphVec3 at = b->mesh.positions[a.position];
        NPH_TRY(lod_mesh_add_vertex(&b->mesh, b->alloc, b->mesh.class_of[a.position]));
        NPH_TRY(NPH_ARRAY_PUSH(&b->grown_positions, at));
        NPH_TRY(NPH_ARRAY_PUSH(&b->grown_uvs, a.uv));
        if (b->normals != NULL) {
            // Read first: the array may move as it grows.
            const NphVec3 normal = b->grown_normals.data[a.source];
            NPH_TRY(NPH_ARRAY_PUSH(&b->grown_normals, normal));
            b->normals = b->grown_normals.data;
        }
        b->mesh.positions = b->grown_positions.data;
        b->uvs = b->grown_uvs.data;
        NPH_TRY(NPH_ARRAY_PUSH(&out->added, a));
    }
    for (u32 k = 0; k < o->part.bricks.len; ++k) {
        NphRenderVoxelBrick br = o->part.bricks.data[k];
        br.first_voxel += voxel_base;
        NPH_TRY(NPH_ARRAY_PUSH(&out->bricks, br));
    }
    NPH_TRY(NPH_ARRAY_PUSH_N(&out->voxels, o->part.voxels.data, o->part.voxels.len));
    NPH_TRY(NPH_ARRAY_PUSH_N(&b->next_sources, o->sources.data, o->sources.len));
    NPH_TRY(NPH_ARRAY_PUSH_N(orphans, o->orphans.data, o->orphans.len));
    return NPH_RESULT_OK;
}

// One colour of a level's groups, built at once: each group's clusters,
// and what each made.
typedef struct LevelWork {
    Build *b;
    u32 level;
    const u32 *members; // each group's clusters in turn
    const u32 *start;   // per group (and one past the last): where its clusters begin
    const u32 *groups;  // the colour's groups
    u32 groups_first;   // the level's first group's index in the output
    GroupOut *outs;     // per group of the level
} LevelWork;

static void build_groups(void *user, u32 begin, u32 end)
{
    const LevelWork *w = (const LevelWork *)user;
    for (u32 i = begin; i < end; ++i) {
        const u32 g = w->groups[i];
        const u32 first = w->start[g];
        w->outs[g].result = group_build(w->b, w->level, w->members + first,
                                        w->start[g + 1U] - first, w->groups_first + g, &w->outs[g]);
    }
}

// A (locked class, group) incidence, for the groups' neighbours.
typedef struct ClassGroup {
    u32 cls;
    u32 group;
} ClassGroup;

static int compare_class_groups(const void *x, const void *y)
{
    const ClassGroup *a = (const ClassGroup *)x;
    const ClassGroup *b = (const ClassGroup *)y;
    if (a->cls != b->cls) {
        return a->cls < b->cls ? -1 : 1;
    }
    return a->group < b->group ? -1 : (a->group > b->group ? 1 : 0);
}

// The groups coloured so that no two that share a class have one colour:
// each takes, in group order, the least colour none of the neighbours
// coloured before it has (greedy colouring; Welsh & Powell, The Computer
// Journal 1967). `order` (groups entries) takes the groups colour by
// colour, each colour's in group order, and `colour_start` (*out_colours +
// 1 entries, at most groups + 1) where each colour begins in it. The
// colours then run in turn, a colour's groups at once: a group sees the
// collapses its neighbours of earlier colours made onto the classes they
// share, as in a build of one group after another, whatever the threads.
static NphResult colour_groups(Build *b, const u32 *members, const u32 *start, u32 groups,
                               u32 *order, NphArrayU32 *colour_start, u32 *out_colours)
{
    NphAllocator *a = b->alloc;
    *out_colours = 0U;
    NPH_ARRAY(ClassGroup) pairs;
    NPH_ARRAY_INIT(&pairs, a);
    NphArrayU32 classes;
    NPH_ARRAY_INIT(&classes, a);
    NPH_ARRAY(AdjEdge) edges;
    NPH_ARRAY_INIT(&edges, a);
    NphArrayU32 adj_start;
    NPH_ARRAY_INIT(&adj_start, a);
    NphArrayU32 colour;
    NPH_ARRAY_INIT(&colour, a);
    NphArrayU32 seen; // per colour: the last group that saw a neighbour of it
    NPH_ARRAY_INIT(&seen, a);
    NphResult r = NPH_RESULT_OK;
    // Each group's locked classes.
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        NPH_ARRAY_CLEAR(&classes);
        for (u32 i = start[g]; i < start[g + 1U] && r == NPH_RESULT_OK; ++i) {
            r = cluster_classes(b, members[i], &classes);
        }
        for (u32 k = 0; k < classes.len && r == NPH_RESULT_OK; ++k) {
            if (b->locked[classes.data[k]] != 0U) {
                const ClassGroup p = {classes.data[k], g};
                r = NPH_ARRAY_PUSH(&pairs, p);
            }
        }
    }
    if (r == NPH_RESULT_OK && pairs.len > 1U) {
        qsort(pairs.data, pairs.len, sizeof(ClassGroup), compare_class_groups);
    }
    // The groups of each class neighbour each other (a class lies in a few).
    for (u32 i = 0; i < pairs.len && r == NPH_RESULT_OK;) {
        u32 j = i;
        while (j < pairs.len && pairs.data[j].cls == pairs.data[i].cls) {
            ++j;
        }
        for (u32 x = i; x < j && r == NPH_RESULT_OK; ++x) {
            for (u32 y = x + 1U; y < j && r == NPH_RESULT_OK; ++y) {
                const u32 p = pairs.data[x].group;
                const u32 q = pairs.data[y].group;
                if (p == q) {
                    continue; // a group lists a class once a cluster
                }
                const AdjEdge e0 = {p, q};
                const AdjEdge e1 = {q, p};
                r = NPH_ARRAY_PUSH(&edges, e0);
                if (r == NPH_RESULT_OK) {
                    r = NPH_ARRAY_PUSH(&edges, e1);
                }
            }
        }
        i = j;
    }
    if (r == NPH_RESULT_OK && edges.len > 1U) {
        qsort(edges.data, edges.len, sizeof(AdjEdge), compare_adj);
    }
    // Each group's run of edges (duplicates and all: they colour alike).
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&adj_start, groups + 1U);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&colour, nph_max_u32(groups, 1U));
    }
    if (r == NPH_RESULT_OK) {
        u32 e = 0U;
        for (u32 g = 0; g <= groups; ++g) {
            while (e < edges.len && edges.data[e].a < g) {
                ++e;
            }
            adj_start.data[g] = e;
        }
    }
    u32 colours = 0U;
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        // The colours of its neighbours coloured before it, marked with g + 1.
        for (u32 e = adj_start.data[g]; e < adj_start.data[g + 1U]; ++e) {
            const u32 n = edges.data[e].b;
            if (n < g) {
                seen.data[colour.data[n]] = g + 1U;
            }
        }
        u32 c = 0U;
        while (c < colours && seen.data[c] == g + 1U) {
            ++c;
        }
        if (c == colours) {
            r = NPH_ARRAY_PUSH(&seen, 0U);
            colours += 1U;
        }
        colour.data[g] = c;
    }
    // The groups colour by colour (a counting sort, in group order).
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(colour_start, colours + 1U);
    }
    if (r == NPH_RESULT_OK) {
        memset(colour_start->data, 0, ((size_t)colours + 1U) * sizeof(u32));
        for (u32 g = 0; g < groups; ++g) {
            colour_start->data[colour.data[g] + 1U] += 1U;
        }
        for (u32 c = 0; c < colours; ++c) {
            colour_start->data[c + 1U] += colour_start->data[c];
        }
        NPH_ARRAY_CLEAR(&seen);
        r = NPH_ARRAY_PUSH_N(&seen, colour_start->data, colours);
    }
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        order[seen.data[colour.data[g]]] = g;
        seen.data[colour.data[g]] += 1U;
    }
    *out_colours = colours;
    NPH_ARRAY_FREE(&pairs);
    NPH_ARRAY_FREE(&classes);
    NPH_ARRAY_FREE(&edges);
    NPH_ARRAY_FREE(&adj_start);
    NPH_ARRAY_FREE(&colour);
    NPH_ARRAY_FREE(&seen);
    return r;
}

// A level's measured error (below), a class at a time and a group at a time.
typedef struct MeasureWork {
    Build *b;
    const LevelGrid *grid;
    const u32 *class_seen; // the classes the level's result has
    const u32 *by_class;   // each class's triangles in the result
    const u32 *made_start; // per group (and one past the last): its first cluster made
    const u32 *orphan_start;
    const u32 *orphans;
    const u8 *voxel_group; // per group: it made voxels (nothing to measure)
    f32 *measured;         // per group: its measured error
} MeasureWork;

// The distance of each class's level-0 points from the level's result.
static void measure_classes(void *user, u32 begin, u32 end)
{
    const MeasureWork *w = (const MeasureWork *)user;
    Build *b = w->b;
    for (u32 i = begin; i < end; ++i) {
        const u32 c = w->class_seen[i];
        b->class_dist[c] = class_distance(b, w->grid, c, w->by_class + b->class_tri_start[c],
                                          b->class_tri_count[c], false);
    }
}

// Each group's measured error: its classes' distances, its triangles' back
// to the mesh, and its orphans'.
static void measure_groups(void *user, u32 begin, u32 end)
{
    const MeasureWork *w = (const MeasureWork *)user;
    const Build *b = w->b;
    const NphRenderClusterLods *out = b->out;
    for (u32 g = begin; g < end; ++g) {
        if (w->voxel_group[g] != 0U) {
            w->measured[g] = 0.0F; // a voxel group's error is its cell's
            continue;
        }
        f32 measured = 0.0F;
        for (u32 cl = w->made_start[g]; cl < w->made_start[g + 1U]; ++cl) {
            const NphRenderMeshlet *ml = &out->clusters.meshlets.data[cl];
            for (u32 k = 0; k < ml->vertex_count; ++k) {
                const u32 c = b->mesh.class_of[out->clusters.vertices.data[ml->vertex_offset + k]];
                // Measured above: a cluster's vertices are its triangles'.
                const f32 d =
                    b->class_dist[c] >= 0.0F
                        ? b->class_dist[c]
                        : class_distance(b, w->grid, c, w->by_class + b->class_tri_start[c],
                                         b->class_tri_count[c], false);
                measured = nph_f32_max(measured, d);
            }
        }
        // The other way: the result's triangles back to the mesh (their
        // centroids and edge midpoints), where a chord across a curve may
        // lie farther from it than any of the mesh's own points does from
        // the chord.
        for (u32 cl = w->made_start[g]; cl < w->made_start[g + 1U]; ++cl) {
            const NphRenderMeshlet *ml = &out->clusters.meshlets.data[cl];
            for (u32 t = 0; t < ml->triangle_count; ++t) {
                NphVec3 p[3];
                for (u32 k = 0; k < 3U; ++k) {
                    const u8 corner =
                        out->clusters.triangles.data[((u64)ml->triangle_offset + t) * 3U + k];
                    p[k] =
                        b->mesh.positions[out->clusters.vertices.data[ml->vertex_offset + corner]];
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
        for (u32 i = w->orphan_start[g]; i < w->orphan_start[g + 1U]; ++i) {
            measured =
                nph_f32_max(measured, class_distance(b, w->grid, w->orphans[i], NULL, 0U, true));
        }
        w->measured[g] = measured;
    }
}

// A level's error: the groups' collapses go no further than this many times the error
// of the median collapse they could make as they stand. A group's error is its worst
// collapse's, and all its clusters draw until the screen allows it, so one collapse
// a group makes to reach its target, far dearer than the others, keeps its cheap ones
// from being drawn for a long way (a tree's bark, whose groups hold its cards: ADR-0069).
// A group stopped by it makes do with less, and the next level, whose error is larger,
// takes it on. Twice the median lets a surface halve: its collapses' errors double as
// its triangles halve, and those it has yet to make cost a little more than the median.
#define LEVEL_ERROR_FACTOR 2.0

typedef struct LevelCostWork {
    Build *b;
    const u32 *members;
    const u32 *start;
    LodCostArray *costs; // per group
    NphResult *results;  // per group
} LevelCostWork;

static void level_costs(void *user, u32 begin, u32 end)
{
    const LevelCostWork *w = (const LevelCostWork *)user;
    Build *b = w->b;
    NphArrayU32 indices;
    NPH_ARRAY_INIT(&indices, b->alloc);
    NphArrayU32 submeshes;
    NPH_ARRAY_INIT(&submeshes, b->alloc);
    for (u32 g = begin; g < end; ++g) {
        NphResult r = gather(b, w->members + w->start[g], w->start[g + 1U] - w->start[g], &indices,
                             &submeshes);
        if (r == NPH_RESULT_OK && submeshes.len != 0U) {
            LodSimplifyInput si;
            memset(&si, 0, sizeof si);
            si.mesh = &b->mesh;
            si.indices = indices.data;
            si.submeshes = submeshes.data;
            si.triangle_count = submeshes.len;
            si.locked = b->locked;
            si.uvs = b->uvs;
            si.normals = b->normals;
            si.scratch = b->alloc;
            si.borders = 1;
            r = lod_simplify_costs(&si, &w->costs[g]);
        }
        w->results[g] = r;
    }
    NPH_ARRAY_FREE(&indices);
    NPH_ARRAY_FREE(&submeshes);
}

static int compare_costs(const void *x, const void *y)
{
    const f64 a = *(const f64 *)x;
    const f64 b = *(const f64 *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}

// The error of the level whose groups are `start` and `members`. A mesh that may
// voxelize has none (the build's comment on `voxels`).
static NphResult level_error_limit(Build *b, const u32 *members, const u32 *start, u32 groups)
{
    b->level_cost_limit = NPH_F64_MAX;
    if (b->in->voxels != NULL || groups == 0U) {
        return NPH_RESULT_OK;
    }
    NphAllocator *a = b->alloc;
    LodCostArray *costs = NPH_ALLOC_ARRAY(a, LodCostArray, groups);
    NphResult *results = NPH_ALLOC_ARRAY(a, NphResult, groups);
    LodCostArray all;
    NPH_ARRAY_INIT(&all, a);
    NphResult r = costs != NULL && results != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 g = 0; g < groups; ++g) {
            NPH_ARRAY_INIT(&costs[g], a);
        }
        LevelCostWork work;
        work.b = b;
        work.members = members;
        work.start = start;
        work.costs = costs;
        work.results = results;
        run_range(b->in->jobs, groups, 1U, level_costs, &work);
        for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
            r = results[g];
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH_N(&all, costs[g].data, costs[g].len);
            }
        }
    }
    if (r == NPH_RESULT_OK && all.len != 0U) {
        qsort(all.data, all.len, sizeof(f64), compare_costs);
        const f64 median = nph_f64_sqrt(all.data[all.len / 2U]);
        b->level_error = nph_f64_max(b->level_error, LEVEL_ERROR_FACTOR * median);
        b->level_cost_limit = b->level_error * b->level_error;
    }
    if (costs != NULL) {
        for (u32 g = 0; g < groups; ++g) {
            NPH_ARRAY_FREE(&costs[g]);
        }
        NPH_FREE_ARRAY(a, LodCostArray, costs, groups);
    }
    if (results != NULL) {
        NPH_FREE_ARRAY(a, NphResult, results, groups);
    }
    NPH_ARRAY_FREE(&all);
    return r;
}

// The groups of the level [first, first + n), simplified into the next.
// `*out_made` takes the next level's cluster count, `*out_before` and
// `*out_after` the primitives (triangles and voxels) the groups took and
// made, and `*out_changed` the groups whose triangles became voxels.
static NphResult build_level(Build *b, u32 level, u32 first, u32 n, u32 *out_made, u64 *out_before,
                             u64 *out_after, u32 *out_changed)
{
    NphAllocator *a = b->alloc;
    NphRenderClusterLods *out = b->out;
    *out_made = 0U;
    *out_before = 0U;
    *out_after = 0U;
    *out_changed = 0U;
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
    // Per group: where its clusters and its orphans begin (one past the
    // last group: where they end), and whether it made voxels (its error is
    // its cell's, not measured).
    NphArrayU32 made_start;
    NPH_ARRAY_INIT(&made_start, a);
    NphArrayU32 orphan_start;
    NPH_ARRAY_INIT(&orphan_start, a);
    NphArrayU32 orphans;
    NPH_ARRAY_INIT(&orphans, a);
    NphArrayU8 voxel_group;
    NPH_ARRAY_INIT(&voxel_group, a);
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
    if (r == NPH_RESULT_OK) {
        r = level_error_limit(b, members.data, start.data, groups);
    }
    // The groups by colour (no two of a colour share a class), the colours
    // in turn, a colour's groups at once where there are jobs; then each
    // group's results after those of the groups before it.
    const u32 made_first = out->clusters.meshlets.len;
    const u32 groups_first = out->groups.len;
    GroupOut *outs = NULL;
    u32 *order = NULL;
    NphArrayU32 colour_start;
    NPH_ARRAY_INIT(&colour_start, a);
    u32 colours = 0U;
    if (r == NPH_RESULT_OK) {
        outs = NPH_ALLOC_ARRAY(a, GroupOut, nph_max_u32(groups, 1U));
        order = NPH_ALLOC_ARRAY(a, u32, nph_max_u32(groups, 1U));
        r = outs != NULL && order != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 g = 0; g < groups && outs != NULL; ++g) {
        group_out_init(&outs[g], a);
    }
    if (r == NPH_RESULT_OK) {
        r = colour_groups(b, members.data, start.data, groups, order, &colour_start, &colours);
    }
    for (u32 c = 0; c < colours && r == NPH_RESULT_OK; ++c) {
        LevelWork work;
        work.b = b;
        work.level = level;
        work.members = members.data;
        work.start = start.data;
        work.groups = order + colour_start.data[c];
        work.groups_first = groups_first;
        work.outs = outs;
        run_range(b->in->jobs, colour_start.data[c + 1U] - colour_start.data[c], 1U, build_groups,
                  &work);
    }
    NPH_ARRAY_FREE(&colour_start);
    if (order != NULL) {
        NPH_FREE_ARRAY(a, u32, order, nph_max_u32(groups, 1U));
    }
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        NPH_ASSERT(outs != NULL);
        r = outs[g].result;
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&made_start, out->clusters.meshlets.len);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&orphan_start, orphans.len);
        }
        if (r == NPH_RESULT_OK) {
            const u8 flag = outs[g].voxels ? 1U : 0U;
            r = NPH_ARRAY_PUSH(&voxel_group, flag);
        }
        if (r == NPH_RESULT_OK) {
            const u32 at = start.data[g];
            r = group_join(b, members.data + at, start.data[g + 1U] - at, &outs[g], &orphans);
        }
        *out_before += outs[g].before;
        *out_after += outs[g].after;
        *out_changed += outs[g].changed ? 1U : 0U;
    }
    for (u32 g = 0; g < groups && outs != NULL; ++g) {
        group_out_free(&outs[g]);
    }
    if (outs != NULL) {
        NPH_FREE_ARRAY(a, GroupOut, outs, nph_max_u32(groups, 1U));
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
    // Every class the result has, then every group, at once where there
    // are jobs: each a function of the level's result alone.
    f32 *measured = NULL;
    if (r == NPH_RESULT_OK) {
        measured = NPH_ALLOC_ARRAY(a, f32, nph_max_u32(groups, 1U));
        r = measured != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    if (r == NPH_RESULT_OK) {
        MeasureWork mw;
        mw.b = b;
        mw.grid = &grid;
        mw.class_seen = class_seen.data;
        mw.by_class = by_class.data;
        mw.made_start = made_start.data;
        mw.orphan_start = orphan_start.data;
        mw.orphans = orphans.data;
        mw.voxel_group = voxel_group.data;
        mw.measured = measured;
        run_range(b->in->jobs, class_seen.len, 64U, measure_classes, &mw);
        run_range(b->in->jobs, groups, 1U, measure_groups, &mw);
    }
    for (u32 g = 0; g < groups && r == NPH_RESULT_OK; ++g) {
        // A flag a group, pushed with its record (the analyzer cannot tell).
        NPH_ASSERT(voxel_group.data != NULL && g < voxel_group.len && measured != NULL);
        if (voxel_group.data[g] != 0U) {
            continue;
        }
        NphRenderLodGroup *rg = &out->groups.data[groups_first + g];
        rg->error = nph_f32_max(rg->error, measured[g]);
        for (u32 cl = made_start.data[g]; cl < made_start.data[g + 1U]; ++cl) {
            b->own_error.data[cl] = rg->error;
        }
    }
    if (measured != NULL) {
        NPH_FREE_ARRAY(a, f32, measured, nph_max_u32(groups, 1U));
    }
    for (u32 i = 0; i < class_seen.len; ++i) {
        b->class_dist[class_seen.data[i]] = -1.0F;
        b->class_tri_count[class_seen.data[i]] = 0U;
    }
    NPH_ARRAY_FREE(&class_seen);
    NPH_ARRAY_FREE(&by_class);
    grid_free(a, &grid);
    NPH_ARRAY_FREE(&made_start);
    NPH_ARRAY_FREE(&orphan_start);
    NPH_ARRAY_FREE(&orphans);
    NPH_ARRAY_FREE(&voxel_group);
    *out_made = out->clusters.meshlets.len - made_first;
    NPH_ARRAY_FREE(&start);
    NPH_ARRAY_FREE(&members);
    NPH_ARRAY_FREE(&classes);
    NPH_ARRAY_FREE(&touched);
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
        const NphSphere sphere = lod_sphere_hull(parts.data, parts.len);
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
    out->sphere = lod_sphere_hull(parts, count);
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

NphResult lod_build_hierarchy(NphRenderClusterLods *l, NphAllocator *a)
{
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
    // A sampler has its functions and a surface per submesh (a masked one
    // needs `covers`).
    const NphRenderVoxelSampler *vs = in->voxels;
    if (vs != NULL) {
        if (vs->sample == NULL || (in->range_count != 0U && vs->submesh_surface == NULL)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        for (u32 i = 0; i < in->range_count; ++i) {
            const u8 kind = vs->submesh_surface[i];
            if (kind > (u8)NPH_RENDER_VOXEL_SURFACE_MASKED ||
                (kind == (u8)NPH_RENDER_VOXEL_SURFACE_MASKED && vs->covers == NULL)) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
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
        // A record without vertices is a voxel cluster's, which only the
        // levels make.
        if (ml->submesh >= in->range_count || ml->vertex_count == 0U || ml->triangle_count == 0U ||
            ml->vertex_count > *max_vertices ||
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
    NPH_ARRAY_FREE(&b->cell_of);
    NPH_ARRAY_FREE(&b->source_start);
    NPH_ARRAY_FREE(&b->source_count);
    NPH_ARRAY_FREE(&b->sources);
    NPH_ARRAY_FREE(&b->next_sources);
    NPH_ARRAY_FREE(&b->grown_positions);
    NPH_ARRAY_FREE(&b->grown_uvs);
    NPH_ARRAY_FREE(&b->grown_normals);
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
    NPH_ARRAY_INIT(&b.cell_of, b.alloc);
    NPH_ARRAY_INIT(&b.source_start, b.alloc);
    NPH_ARRAY_INIT(&b.source_count, b.alloc);
    NPH_ARRAY_INIT(&b.sources, b.alloc);
    NPH_ARRAY_INIT(&b.next_sources, b.alloc);
    NPH_ARRAY_INIT(&b.grown_positions, b.alloc);
    NPH_ARRAY_INIT(&b.grown_uvs, b.alloc);
    NPH_ARRAY_INIT(&b.grown_normals, b.alloc);
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
    // A mesh that may voxelize keeps the simplifier its voxels were measured with (ADR-0061):
    // a group's collapses stalling, and their error against the voxels', is what makes it
    // voxels, so what is measured against that stays as it was (ADR-0069).
    if (r == NPH_RESULT_OK && in->uvs != NULL && in->voxels == NULL && in->vertex_count != 0U) {
        // Copies that can grow by the vertices the levels add.
        r = NPH_ARRAY_PUSH_N(&b.grown_positions, in->positions, in->vertex_count);
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH_N(&b.grown_uvs, in->uvs, in->vertex_count);
        }
        if (r == NPH_RESULT_OK) {
            b.mesh.positions = b.grown_positions.data;
            b.uvs = b.grown_uvs.data;
        }
    }
    if (r == NPH_RESULT_OK && in->normals != NULL && in->voxels == NULL && in->vertex_count != 0U) {
        r = NPH_ARRAY_PUSH_N(&b.grown_normals, in->normals, in->vertex_count);
        b.normals = b.grown_normals.data;
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
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&b.cell_of, 0.0F);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&b.source_start, 0U);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_PUSH(&b.source_count, 0U);
        }
    }
    out->level0_clusters = m0->meshlets.len;
    out->levels = 1U;
    u32 first = 0U;
    u32 count = m0->meshlets.len;
    while (r == NPH_RESULT_OK && count > 1U &&
           (in->max_levels == 0U || out->levels < in->max_levels)) {
        u32 made = 0U;
        u64 before = 0U;
        u64 after = 0U;
        u32 changed = 0U;
        r = build_level(&b, out->levels - 1U, first, count, &made, &before, &after, &changed);
        if (r != NPH_RESULT_OK) {
            break;
        }
        first += count;
        count = made;
        out->levels += 1U;
        // The voxels the level made are the next level's sources.
        NPH_ARRAY_CLEAR(&b.sources);
        const LodVoxelSourceArray swap = b.sources;
        b.sources = b.next_sources;
        b.next_sources = swap;
        // The levels stop when they stop shrinking, unless triangles just
        // became voxels (which may make more primitives, never more levels
        // than that once per group).
        if (changed == 0U && (f64)after > (f64)STALL_RATIO * (f64)before) {
            break;
        }
    }
    // The root groups: the coarsest level's clusters, grouped.
    if (r == NPH_RESULT_OK && count != 0U) {
        r = build_roots(&b, first, count);
    }
    if (r == NPH_RESULT_OK && out->groups.len != 0U) {
        r = lod_build_hierarchy(b.out, b.alloc);
    }
    build_free(&b);
    return r;
}
