// SPDX-License-Identifier: Apache-2.0
// The world partition's cells and their proxies (cook/cells.h; ADR-0062).
#include <nepheline/asset/mesh.h>
#include <nepheline/core/assert.h>
#include <nepheline/math/scalar.h>

#include <cook/cells.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

CookHlodSettings cook_hlod_resolved(const CookHlodSettings *s)
{
    CookHlodSettings r;
    memset(&r, 0, sizeof r);
    if (s != NULL) {
        r = *s;
    }
    r.cell = r.cell > 0.0F ? r.cell : COOK_HLOD_CELL;
    r.distance = r.distance > 0.0F ? r.distance : COOK_HLOD_DISTANCE;
    r.pixel = r.pixel > 0.0F ? r.pixel : COOK_HLOD_PIXEL;
    return r;
}

f32 cook_hlod_finest_cell(const CookHlodSettings *s)
{
    const CookHlodSettings r = cook_hlod_resolved(s);
    const f64 error = (f64)r.distance * (f64)r.pixel;
    return (f32)ldexp(1.0, (i32)floor(log2(error)));
}

// ---------------------------------------------------------------------------
// The partition
// ---------------------------------------------------------------------------

// The squares are numbered to +-2^30 of the origin.
#define SQUARE_LIMIT 1073741824.0

typedef struct Placed {
    u64 key; // the square, (z, x) biased; the last for no cell
    u32 instance;
    u32 pad;
} Placed;

static int compare_placed(const void *x, const void *y)
{
    const Placed *p = (const Placed *)x;
    const Placed *q = (const Placed *)y;
    if (p->key != q->key) {
        return p->key < q->key ? -1 : 1;
    }
    return p->instance < q->instance ? -1 : (p->instance > q->instance ? 1 : 0);
}

static u64 square_key(i64 x, i64 z)
{
    const i64 bias = (i64)SQUARE_LIMIT;
    return ((u64)(z + bias) << 32U) | (u64)(x + bias);
}

NphResult cook_cells_partition(const CookCellInstance *instances, u32 count, f32 edge,
                               NphArrayU32 *order, CookCellArray *cells)
{
    NPH_ASSERT(order != NULL && cells != NULL && (instances != NULL || count == 0U));
    NPH_ARRAY_CLEAR(order);
    NPH_ARRAY_CLEAR(cells);
    if (!(edge > 0.0F) || !nph_f32_is_finite(edge)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_ARRAY(Placed) placed;
    NPH_ARRAY_INIT(&placed, order->alloc);
    NphResult r = NPH_ARRAY_RESIZE(&placed, count);
    for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
        const CookCellInstance *ci = &instances[i];
        Placed pl;
        memset(&pl, 0, sizeof pl);
        pl.key = NPH_U64_MAX;
        pl.instance = i;
        if (ci->cellable && ci->sphere.radius <= 0.5F * edge) {
            const f64 x = floor((f64)ci->sphere.center.x / (f64)edge);
            const f64 z = floor((f64)ci->sphere.center.z / (f64)edge);
            if (!(nph_f64_abs(x) < SQUARE_LIMIT) || !(nph_f64_abs(z) < SQUARE_LIMIT)) {
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
            pl.key = square_key((i64)x, (i64)z);
        }
        placed.data[i] = pl;
    }
    if (r == NPH_RESULT_OK && count > 1U) {
        qsort(placed.data, count, sizeof(Placed), compare_placed);
    }
    for (u32 i = 0; i < count && r == NPH_RESULT_OK;) {
        u32 j = i;
        while (j < count && placed.data[j].key == placed.data[i].key) {
            ++j;
        }
        if (placed.data[i].key != NPH_U64_MAX) {
            CookCell c;
            memset(&c, 0, sizeof c);
            const i64 bias = (i64)SQUARE_LIMIT;
            c.x = (i32)((i64)(placed.data[i].key & 0xFFFFFFFFU) - bias);
            c.z = (i32)((i64)(placed.data[i].key >> 32U) - bias);
            c.first = i;
            c.count = j - i;
            c.bounds = nph_aabb_empty();
            for (u32 k = i; k < j; ++k) {
                const NphSphere sp = instances[placed.data[k].instance].sphere;
                c.bounds =
                    nph_aabb_expand(c.bounds, nph_vec3_sub(sp.center, nph_vec3_splat(sp.radius)));
                c.bounds =
                    nph_aabb_expand(c.bounds, nph_vec3_add(sp.center, nph_vec3_splat(sp.radius)));
            }
            r = NPH_ARRAY_PUSH(cells, c);
        }
        for (u32 k = i; k < j && r == NPH_RESULT_OK; ++k) {
            r = NPH_ARRAY_PUSH(order, placed.data[k].instance);
        }
        i = j;
    }
    NPH_ARRAY_FREE(&placed);
    return r;
}

// ---------------------------------------------------------------------------
// A cell's proxy
// ---------------------------------------------------------------------------

// The cell at square (x, z), or NPH_U32_MAX (the cells are in (z, x) order).
static u32 find_cell(const CookCell *cells, u32 count, i32 x, i32 z)
{
    u32 lo = 0U;
    u32 hi = count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2U;
        const CookCell *c = &cells[mid];
        if (c->z == z && c->x == x) {
            return mid;
        }
        if (c->z < z || (c->z == z && c->x < x)) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return NPH_U32_MAX;
}

static bool sphere_meets_box(NphSphere s, const NphAABB *b)
{
    f32 d2 = 0.0F;
    const f32 c[3] = {s.center.x, s.center.y, s.center.z};
    const f32 lo[3] = {b->min.x, b->min.y, b->min.z};
    const f32 hi[3] = {b->max.x, b->max.y, b->max.z};
    for (u32 a = 0; a < 3U; ++a) {
        const f32 e = c[a] < lo[a] ? lo[a] - c[a] : (c[a] > hi[a] ? c[a] - hi[a] : 0.0F);
        d2 += e * e;
    }
    return d2 <= s.radius * s.radius;
}

// Instance `i`'s transform into the proxy's space: its own, shifted by the
// origin.
static NphRenderProxyInstance proxy_instance(const CookCellInstance *ci, NphVec3 origin)
{
    NphRenderProxyInstance pi;
    memset(&pi, 0, sizeof pi);
    memcpy(pi.transform, ci->transform, sizeof pi.transform);
    pi.transform[0][3] -= (f64)origin.x;
    pi.transform[1][3] -= (f64)origin.y;
    pi.transform[2][3] -= (f64)origin.z;
    pi.mesh = ci->mesh;
    return pi;
}

NphResult cook_cell_proxy(const CookCellProxyInput *in, NphRenderClusterLods *out,
                          NphVec3 *out_origin, NphRenderCellProxyStats *stats)
{
    NPH_ASSERT(in != NULL && out != NULL && out_origin != NULL && in->cell < in->cell_count);
    const CookHlodSettings s = cook_hlod_resolved(&in->settings);
    const CookCell *cell = &in->cells[in->cell];
    const f32 edge = s.cell;
    const NphVec3 origin = nph_vec3((f32)cell->x * edge, 0.0F, (f32)cell->z * edge);
    *out_origin = origin;
    NphAllocator *a = in->scratch;
    // The block of 3 x 3 squares round it, from an edge below its instances
    // to an edge above.
    NphAABB block;
    block.min =
        nph_vec3((f32)(cell->x - 1) * edge, cell->bounds.min.y - edge, (f32)(cell->z - 1) * edge);
    block.max =
        nph_vec3((f32)(cell->x + 2) * edge, cell->bounds.max.y + edge, (f32)(cell->z + 2) * edge);
    NPH_ARRAY(NphRenderProxyInstance) own;
    NPH_ARRAY(NphRenderProxyInstance) occluders;
    NPH_ARRAY_INIT(&own, a);
    NPH_ARRAY_INIT(&occluders, a);
    NphResult r = NPH_RESULT_OK;
    for (u32 k = cell->first; k < cell->first + cell->count && r == NPH_RESULT_OK; ++k) {
        r = NPH_ARRAY_PUSH(&own, proxy_instance(&in->instances[in->order[k]], origin));
    }
    // The eight squares' instances, then those of no cell that reach the
    // block (the terrain).
    for (i32 dz = -1; dz <= 1 && r == NPH_RESULT_OK; ++dz) {
        for (i32 dx = -1; dx <= 1 && r == NPH_RESULT_OK; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            const u32 n = find_cell(in->cells, in->cell_count, cell->x + dx, cell->z + dz);
            if (n == NPH_U32_MAX) {
                continue;
            }
            const CookCell *nc = &in->cells[n];
            for (u32 k = nc->first; k < nc->first + nc->count && r == NPH_RESULT_OK; ++k) {
                r = NPH_ARRAY_PUSH(&occluders,
                                   proxy_instance(&in->instances[in->order[k]], origin));
            }
        }
    }
    u32 celled = 0U;
    for (u32 c = 0; c < in->cell_count; ++c) {
        celled += in->cells[c].count;
    }
    for (u32 k = celled; k < in->instance_count && r == NPH_RESULT_OK; ++k) {
        const CookCellInstance *ci = &in->instances[in->order[k]];
        if (sphere_meets_box(ci->sphere, &block)) {
            r = NPH_ARRAY_PUSH(&occluders, proxy_instance(ci, origin));
        }
    }
    if (r == NPH_RESULT_OK) {
        NphRenderCellProxyInput pin;
        memset(&pin, 0, sizeof pin);
        pin.meshes = in->meshes;
        pin.mesh_count = in->mesh_count;
        pin.instances = own.data;
        pin.instance_count = own.len;
        pin.occluders = occluders.data;
        pin.occluder_count = occluders.len;
        pin.occluder_bounds.min = nph_vec3_sub(block.min, origin);
        pin.occluder_bounds.max = nph_vec3_sub(block.max, origin);
        pin.cell = cook_hlod_finest_cell(&s);
        pin.distance = s.distance;
        pin.scratch = a;
        r = nph_render_cell_proxy_build(&pin, out, stats);
    }
    NPH_ARRAY_FREE(&own);
    NPH_ARRAY_FREE(&occluders);
    return r;
}

// ---------------------------------------------------------------------------
// The proxy as a mesh asset
// ---------------------------------------------------------------------------

NPH_STATIC_ASSERT(sizeof(NphMeshlet) == sizeof(NphRenderMeshlet),
                  "a cluster's record is a meshlet's");
NPH_STATIC_ASSERT(sizeof(NphMeshletBounds) == sizeof(NphRenderMeshletBounds),
                  "a cluster's bounds are a meshlet's");
NPH_STATIC_ASSERT(sizeof(NphMeshClusterLod) == sizeof(NphRenderClusterLod),
                  "a cluster's level record is the renderer's");
NPH_STATIC_ASSERT(sizeof(NphMeshLodGroup) == sizeof(NphRenderLodGroup),
                  "a group is the renderer's");
NPH_STATIC_ASSERT(sizeof(NphMeshLodNode) == sizeof(NphRenderLodNode), "a node is the renderer's");
NPH_STATIC_ASSERT(sizeof(NphMeshVoxelCluster) == sizeof(NphRenderVoxelCluster),
                  "a voxel cluster's record is the renderer's");
NPH_STATIC_ASSERT(sizeof(NphMeshVoxelBrick) == sizeof(NphRenderPackedBrick),
                  "a brick is the renderer's");
NPH_STATIC_ASSERT(sizeof(NphMeshLodMaker) == sizeof(NphRenderLodMaker) &&
                      offsetof(NphMeshLodMaker, error) == offsetof(NphRenderLodMaker, error),
                  "a maker's record is the renderer's");

// The bounds of `l`'s clusters: their spheres', a box and a sphere.
static void clusters_bounds(const NphRenderClusterLods *l, NphAABB *out_box, NphSphere *out_sphere)
{
    NphAABB bounds = nph_aabb_empty();
    NphSphere sphere =
        nph_sphere(l->clusters.bounds.data[0].center, l->clusters.bounds.data[0].radius);
    for (u32 c = 0; c < l->clusters.bounds.len; ++c) {
        const NphRenderMeshletBounds *b = &l->clusters.bounds.data[c];
        bounds = nph_aabb_expand(bounds, nph_vec3_sub(b->center, nph_vec3_splat(b->radius)));
        bounds = nph_aabb_expand(bounds, nph_vec3_add(b->center, nph_vec3_splat(b->radius)));
        sphere = nph_sphere_union(sphere, nph_sphere(b->center, b->radius));
    }
    *out_box = bounds;
    *out_sphere = sphere;
}

// A level's blobs, its voxel clusters' records `meshlets` naming theirs in
// `packed` (render/voxel.h "The compact form"), its clusters' makers
// `makers`.
static NphResult write_level(const NphRenderClusterLods *proxy, const NphRenderMeshlet *meshlets,
                             const NphRenderPackedVoxels *packed, const NphRenderLodMaker *makers,
                             NphAssetWriter *writer)
{
    const u32 n = proxy->clusters.meshlets.len;
    NphAABB bounds;
    NphSphere sphere;
    clusters_bounds(proxy, &bounds, &sphere);
    NphMeshHeader h;
    memset(&h, 0, sizeof h);
    h.submesh_count = 1U;
    h.meshlet_count = n;
    h.flags = (u32)NPH_MESH_FLAG_LEVEL;
    h.position_scale = nph_vec3_one();
    h.bounds = bounds;
    h.sphere = sphere;
    NphSubmesh sub;
    memset(&sub, 0, sizeof sub);
    sub.meshlet_count = proxy->level0_clusters;
    sub.bounds = bounds;
    NphMeshLodHeader lh;
    memset(&lh, 0, sizeof lh);
    lh.levels = proxy->levels;
    lh.level0_meshlets = proxy->level0_clusters;
    lh.group_count = proxy->groups.len;
    lh.group_cluster_count = proxy->group_clusters.len;
    lh.node_count = proxy->nodes.len;
    lh.brick_count = packed->bricks.len;
    lh.voxel_word_count = packed->words.len;
    lh.voxel_cluster_count = packed->clusters.len;
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_HEADER, 0U, &h, sizeof h,
                                      NPH_ASSET_CODEC_NONE, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_SUBMESHES, 0U, &sub, sizeof sub,
                                      NPH_ASSET_CODEC_NONE, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_MESHLETS, 0U, meshlets,
                                      (u64)n * sizeof(NphRenderMeshlet), NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(nph_asset_writer_add_blob(
        writer, NPH_MESH_TAG_MESHLET_BOUNDS, 0U, proxy->clusters.bounds.data,
        NPH_ARRAY_BYTES(&proxy->clusters.bounds), NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_HEADER, 0U, &lh, sizeof lh,
                                      NPH_ASSET_CODEC_NONE, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_CLUSTERS, 0U, proxy->lods.data,
                                      NPH_ARRAY_BYTES(&proxy->lods), NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_GROUPS, 0U, proxy->groups.data,
                                      NPH_ARRAY_BYTES(&proxy->groups), NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(
        nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_MEMBERS, 0U, proxy->group_clusters.data,
                                  NPH_ARRAY_BYTES(&proxy->group_clusters), NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_NODES, 0U, proxy->nodes.data,
                                      NPH_ARRAY_BYTES(&proxy->nodes), NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_MAKERS, 0U, makers,
                                      (u64)n * sizeof(NphRenderLodMaker), NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_VOXEL_CLUSTERS, 0U,
                                      packed->clusters.data, NPH_ARRAY_BYTES(&packed->clusters),
                                      NPH_ASSET_CODEC_LZ, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_VOXEL_BRICKS, 0U, packed->bricks.data,
                                      NPH_ARRAY_BYTES(&packed->bricks), NPH_ASSET_CODEC_LZ, 0U));
    return nph_asset_writer_add_blob(writer, NPH_MESH_TAG_VOXELS, 0U, packed->words.data,
                                     NPH_ARRAY_BYTES(&packed->words), NPH_ASSET_CODEC_LZ, 0U);
}

NphResult cook_proxy_level_write(const NphRenderClusterLods *proxy, u32 level,
                                 NphAssetWriter *writer, NphMeshLevel *out)
{
    NPH_ASSERT(proxy != NULL && writer != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (proxy->clusters.meshlets.len == 0U || level >= proxy->levels) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // The level as a mesh of its own, then its voxels compact, packed from
    // its clusters' records, which then name their own (render/voxel.h "The
    // compact form").
    NphAllocator *a = proxy->clusters.meshlets.alloc;
    NphRenderClusterLods lv;
    nph_render_cluster_lods_init(&lv, a);
    NphRenderLodMakerArray makers;
    NPH_ARRAY_INIT(&makers, a);
    NphRenderPackedVoxels packed;
    nph_render_packed_voxels_init(&packed, a);
    NphResult r = nph_render_cluster_lods_level(proxy, level, &lv, &makers);
    if (r == NPH_RESULT_OK) {
        r = nph_render_voxels_pack(lv.clusters.meshlets.data, lv.clusters.meshlets.len,
                                   lv.bricks.data, lv.bricks.len, lv.voxels.data, lv.voxels.len,
                                   &packed);
    }
    if (r == NPH_RESULT_OK) {
        r = write_level(&lv, lv.clusters.meshlets.data, &packed, makers.data, writer);
    }
    if (r == NPH_RESULT_OK) {
        // From its makers' smallest error to its consumers' largest.
        f32 maker = NPH_F32_MAX;
        for (u32 i = 0; i < makers.len; ++i) {
            maker = nph_f32_min(maker, makers.data[i].error);
        }
        f32 error = 0.0F;
        for (u32 g = 0; g < lv.groups.len; ++g) {
            error = nph_f32_max(error, lv.groups.data[g].error);
        }
        out->maker_error = maker;
        out->error = error;
        out->cluster_count = lv.clusters.meshlets.len;
    }
    nph_render_packed_voxels_free(&packed);
    NPH_ARRAY_FREE(&makers);
    nph_render_cluster_lods_free(&lv);
    return r;
}

NphResult cook_proxy_head_write(const NphRenderClusterLods *proxy, const NphMeshLevel *levels,
                                u32 count, NphAssetWriter *writer)
{
    NPH_ASSERT(proxy != NULL && writer != NULL && (levels != NULL || count == 0U));
    if (proxy->clusters.meshlets.len == 0U || count == 0U || count > NPH_MESH_MAX_LEVELS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAABB bounds;
    NphSphere sphere;
    clusters_bounds(proxy, &bounds, &sphere);
    NphMeshHeader h;
    memset(&h, 0, sizeof h);
    h.submesh_count = 1U;
    h.flags = (u32)NPH_MESH_FLAG_LEVELS;
    h.position_scale = nph_vec3_one();
    h.bounds = bounds;
    h.sphere = sphere;
    NphSubmesh sub;
    memset(&sub, 0, sizeof sub);
    sub.bounds = bounds;
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_HEADER, 0U, &h, sizeof h,
                                      NPH_ASSET_CODEC_NONE, 0U));
    NPH_TRY(nph_asset_writer_add_blob(writer, NPH_MESH_TAG_SUBMESHES, 0U, &sub, sizeof sub,
                                      NPH_ASSET_CODEC_NONE, 0U));
    return nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LEVELS, 0U, levels,
                                     (u64)count * sizeof(NphMeshLevel), NPH_ASSET_CODEC_NONE, 0U);
}
