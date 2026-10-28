// SPDX-License-Identifier: Apache-2.0
// Cell proxies (render/cell_proxy.h; docs/render.md "Cell proxies";
// ADR-0062): the instances' cuts gathered as the voxelizer's sources, the
// visibility sweep, each level voxelized tile by tile, and the levels' octree
// of groups.
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cell_proxy.h>

#include "cluster_lod_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// A level is voxelized in tiles of this many of its cells along x and z (and
// all of y), so that the sources gathered at once stay a tile's.
#define TILE_CELLS 64

// A level samples its instances' cut at this share of its cell.
#define SOURCE_ERROR 0.5

// The most texels the sweep's grid may take (64 MiB of transmittance): the
// region of the cell and its occluders seen from one direction.
#define SWEEP_MAX_TEXELS (16U * 1024U * 1024U)

// A level's clusters each hold a block of this many cells along each axis
// (2 x 2 x 2 bricks: at most 8 bricks and 512 voxels, within a voxel
// cluster's limits); a group's block is twice that, the next level's
// cluster block.
#define CLUSTER_BLOCK 8

static i64 floor_div(i64 x, i64 n)
{
    return x >= 0 ? x / n : -((-x + n - 1) / n);
}

static u64 key_of(i64 x, i64 y, i64 z)
{
    const i64 bias = LOD_VOXEL_COORD_LIMIT;
    return ((u64)(z + bias) << 42U) | ((u64)(y + bias) << 21U) | (u64)(x + bias);
}

static u64 block_key(const i32 c[3], i64 block)
{
    return key_of(floor_div(c[0], block), floor_div(c[1], block), floor_div(c[2], block));
}

static bool aabb_overlap(const NphAABB *a, const NphAABB *b)
{
    return a->min.x <= b->max.x && b->min.x <= a->max.x && a->min.y <= b->max.y &&
           b->min.y <= a->max.y && a->min.z <= b->max.z && b->min.z <= a->max.z;
}

// ---------------------------------------------------------------------------
// The input
// ---------------------------------------------------------------------------

// A mesh's clusters by their consumer's error, largest first: a cut at error
// t is among those whose consumer's error passes t.
typedef struct MeshOrder {
    u32 *cluster;
    f32 *consumer; // the consumer's error, per entry
    u32 count;
} MeshOrder;

typedef struct Proxy {
    const NphRenderCellProxyInput *in;
    NphAllocator *a;
    u32 instances; // the cell's, then the occluders
    LodVoxelTransform *transforms;
    f64 *stretch; // per instance: the most its transform stretches a length
    MeshOrder *order;
    u32 *submesh_base; // per mesh: its first submesh in the sources' numbering
    u32 *submesh_mesh; // per source submesh: its mesh
    u8 *surface;       // per source submesh
    u32 submeshes;
    NphRenderCellProxyStats stats;
} Proxy;

static const NphRenderProxyInstance *proxy_instance(const Proxy *p, u32 i)
{
    return i < p->in->instance_count ? &p->in->instances[i]
                                     : &p->in->occluders[i - p->in->instance_count];
}

static bool finite_matrix(const f64 m[3][4])
{
    for (u32 r = 0; r < 3U; ++r) {
        for (u32 c = 0; c < 4U; ++c) {
            if (!nph_f64_is_finite(m[r][c])) {
                return false;
            }
        }
    }
    return true;
}

static NphResult validate(const NphRenderCellProxyInput *in)
{
    int e = 0;
    if (!(in->cell > 0.0F) || !nph_f32_is_finite(in->cell) || frexp((f64)in->cell, &e) != 0.5 ||
        in->instance_count == 0U || in->instances == NULL || in->meshes == NULL ||
        in->mesh_count == 0U || in->scratch == NULL ||
        (in->occluder_count != 0U && in->occluders == NULL)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 m = 0; m < in->mesh_count; ++m) {
        const NphRenderProxyMesh *pm = &in->meshes[m];
        if (pm->lods == NULL || pm->submesh_count == 0U ||
            pm->lods->lods.len != pm->lods->clusters.meshlets.len ||
            pm->lods->clusters.bounds.len != pm->lods->clusters.meshlets.len ||
            pm->lods->groups.len == 0U) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        // Positions where a cluster has vertices (a mesh kept for its voxels
        // alone, or for nothing, has none).
        for (u32 c = 0; c < pm->lods->clusters.meshlets.len && pm->positions == NULL; ++c) {
            if (pm->lods->clusters.meshlets.data[c].vertex_count != 0U) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
        if (pm->sampler != NULL &&
            (pm->sampler->sample == NULL || pm->sampler->submesh_surface == NULL)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    for (u32 i = 0; i < in->instance_count + in->occluder_count; ++i) {
        const NphRenderProxyInstance *pi =
            i < in->instance_count ? &in->instances[i] : &in->occluders[i - in->instance_count];
        if (pi->mesh >= in->mesh_count || !finite_matrix(pi->transform) ||
            (i < in->instance_count && in->meshes[pi->mesh].sampler == NULL)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    return NPH_RESULT_OK;
}

// The most `t` stretches a length: the square root of the largest eigenvalue
// of its linear part's A^T A, bounded above by its largest absolute row sum
// (Gershgorin), exact for a rotation and a uniform scale.
static f64 transform_stretch(const LodVoxelTransform *t)
{
    f64 bound = 0.0;
    for (u32 r = 0; r < 3U; ++r) {
        f64 row = 0.0;
        for (u32 c = 0; c < 3U; ++c) {
            f64 ata = 0.0;
            for (u32 k = 0; k < 3U; ++k) {
                ata += t->to[k][r] * t->to[k][c];
            }
            row += nph_f64_abs(ata);
        }
        bound = nph_f64_max(bound, row);
    }
    return sqrt(bound);
}

typedef struct ConsumerEntry {
    f32 error;
    u32 cluster;
} ConsumerEntry;

static int compare_consumers(const void *x, const void *y)
{
    const ConsumerEntry *p = (const ConsumerEntry *)x;
    const ConsumerEntry *q = (const ConsumerEntry *)y;
    if (p->error != q->error) {
        return p->error > q->error ? -1 : 1;
    }
    return p->cluster < q->cluster ? -1 : (p->cluster > q->cluster ? 1 : 0);
}

static f32 maker_error(const NphRenderClusterLods *l, u32 c)
{
    const u32 g = l->lods.data[c].group;
    return g == NPH_RENDER_LOD_NONE ? 0.0F : l->groups.data[g].error;
}

static f32 consumer_error(const NphRenderClusterLods *l, u32 c)
{
    const u32 g = l->lods.data[c].parent;
    return g == NPH_RENDER_LOD_NONE ? NPH_F32_MAX : l->groups.data[g].error;
}

static NphResult mesh_order(NphAllocator *a, const NphRenderClusterLods *l, MeshOrder *out)
{
    const u32 n = l->clusters.meshlets.len;
    out->count = n;
    out->cluster = NPH_ALLOC_ARRAY(a, u32, nph_max_u32(n, 1U));
    out->consumer = NPH_ALLOC_ARRAY(a, f32, nph_max_u32(n, 1U));
    ConsumerEntry *e = NPH_ALLOC_ARRAY(a, ConsumerEntry, nph_max_u32(n, 1U));
    NphResult r = out->cluster != NULL && out->consumer != NULL && e != NULL
                      ? NPH_RESULT_OK
                      : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 c = 0; c < n; ++c) {
            if (l->lods.data[c].group != NPH_RENDER_LOD_NONE &&
                l->lods.data[c].group >= l->groups.len) {
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
            if (l->lods.data[c].parent != NPH_RENDER_LOD_NONE &&
                l->lods.data[c].parent >= l->groups.len) {
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
            e[c].error = consumer_error(l, c);
            e[c].cluster = c;
        }
    }
    if (r == NPH_RESULT_OK) {
        qsort(e, n, sizeof(ConsumerEntry), compare_consumers);
        for (u32 k = 0; k < n; ++k) {
            out->cluster[k] = e[k].cluster;
            out->consumer[k] = e[k].error;
        }
    }
    if (e != NULL) {
        NPH_FREE_ARRAY(a, ConsumerEntry, e, nph_max_u32(n, 1U));
    }
    return r;
}

static void proxy_free(Proxy *p)
{
    NphAllocator *a = p->a;
    const u32 n = nph_max_u32(p->instances, 1U);
    if (p->transforms != NULL) {
        NPH_FREE_ARRAY(a, LodVoxelTransform, p->transforms, n);
    }
    if (p->stretch != NULL) {
        NPH_FREE_ARRAY(a, f64, p->stretch, n);
    }
    if (p->order != NULL) {
        for (u32 m = 0; m < p->in->mesh_count; ++m) {
            const u32 c = nph_max_u32(p->order[m].count, 1U);
            if (p->order[m].cluster != NULL) {
                NPH_FREE_ARRAY(a, u32, p->order[m].cluster, c);
            }
            if (p->order[m].consumer != NULL) {
                NPH_FREE_ARRAY(a, f32, p->order[m].consumer, c);
            }
        }
        NPH_FREE_ARRAY(a, MeshOrder, p->order, p->in->mesh_count);
    }
    if (p->submesh_base != NULL) {
        NPH_FREE_ARRAY(a, u32, p->submesh_base, p->in->mesh_count);
    }
    const u32 s = nph_max_u32(p->submeshes, 1U);
    if (p->submesh_mesh != NULL) {
        NPH_FREE_ARRAY(a, u32, p->submesh_mesh, s);
    }
    if (p->surface != NULL) {
        NPH_FREE_ARRAY(a, u8, p->surface, s);
    }
}

static NphResult proxy_init(Proxy *p, const NphRenderCellProxyInput *in)
{
    memset(p, 0, sizeof *p);
    p->in = in;
    p->a = in->scratch;
    NphAllocator *a = p->a;
    p->instances = in->instance_count + in->occluder_count;
    const u32 n = nph_max_u32(p->instances, 1U);
    p->transforms = NPH_ALLOC_ARRAY_ZEROED(a, LodVoxelTransform, n);
    p->stretch = NPH_ALLOC_ARRAY(a, f64, n);
    p->order = NPH_ALLOC_ARRAY_ZEROED(a, MeshOrder, in->mesh_count);
    p->submesh_base = NPH_ALLOC_ARRAY(a, u32, in->mesh_count);
    if (p->transforms == NULL || p->stretch == NULL || p->order == NULL ||
        p->submesh_base == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < p->instances; ++i) {
        LodVoxelTransform *t = &p->transforms[i];
        memcpy(t->to, proxy_instance(p, i)->transform, sizeof t->to);
        if (!lod_voxel_transform_init(t)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        p->stretch[i] = transform_stretch(t);
    }
    for (u32 m = 0; m < in->mesh_count; ++m) {
        p->submesh_base[m] = p->submeshes;
        p->submeshes += in->meshes[m].submesh_count;
        NPH_TRY(mesh_order(a, in->meshes[m].lods, &p->order[m]));
    }
    const u32 s = nph_max_u32(p->submeshes, 1U);
    p->submesh_mesh = NPH_ALLOC_ARRAY(a, u32, s);
    p->surface = NPH_ALLOC_ARRAY(a, u8, s);
    if (p->submesh_mesh == NULL || p->surface == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 m = 0; m < in->mesh_count; ++m) {
        const NphRenderProxyMesh *pm = &in->meshes[m];
        for (u32 k = 0; k < pm->submesh_count; ++k) {
            p->submesh_mesh[p->submesh_base[m] + k] = m;
            p->surface[p->submesh_base[m] + k] = pm->sampler != NULL
                                                     ? pm->sampler->submesh_surface[k]
                                                     : (u8)NPH_RENDER_VOXEL_SURFACE_OPAQUE;
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Cuts and sources
// ---------------------------------------------------------------------------

// A cluster of an instance's cut, with its bounds in the proxy's space.
typedef struct CutItem {
    NphAABB box;
    u32 instance;
    u32 cluster;
} CutItem;

typedef NPH_ARRAY(CutItem) CutItemArray;

static NphVec3 transform_point(const LodVoxelTransform *t, NphVec3 q)
{
    f64 out[3];
    for (u32 r = 0; r < 3U; ++r) {
        out[r] =
            t->to[r][0] * (f64)q.x + t->to[r][1] * (f64)q.y + t->to[r][2] * (f64)q.z + t->to[r][3];
    }
    return nph_vec3((f32)out[0], (f32)out[1], (f32)out[2]);
}

// The cut at error `error` (proxy units) of instances [first, end): each
// instance's clusters made at most that error and consumed above it, in
// its mesh's units; `clip` (NULL: none) keeps the clusters whose bounds meet
// it.
static NphResult level_cut(const Proxy *p, u32 first, u32 end, f64 error, const NphAABB *clip,
                           CutItemArray *out)
{
    NPH_ARRAY_CLEAR(out);
    for (u32 i = first; i < end; ++i) {
        const NphRenderProxyInstance *pi = proxy_instance(p, i);
        const NphRenderClusterLods *l = p->in->meshes[pi->mesh].lods;
        const MeshOrder *o = &p->order[pi->mesh];
        const f64 t = error / p->stretch[i];
        for (u32 k = 0; k < o->count && (f64)o->consumer[k] > t; ++k) {
            const u32 c = o->cluster[k];
            if ((f64)maker_error(l, c) > t) {
                continue;
            }
            const NphRenderMeshletBounds *b = &l->clusters.bounds.data[c];
            const NphVec3 centre = transform_point(&p->transforms[i], b->center);
            const f32 reach = (f32)((f64)b->radius * p->stretch[i]);
            CutItem item;
            item.box.min = nph_vec3_sub(centre, nph_vec3_splat(reach));
            item.box.max = nph_vec3_add(centre, nph_vec3_splat(reach));
            item.instance = i;
            item.cluster = c;
            if (clip != NULL && !aabb_overlap(&item.box, clip)) {
                continue;
            }
            NPH_TRY(NPH_ARRAY_PUSH(out, item));
        }
    }
    return NPH_RESULT_OK;
}

// The voxelizer's sources: the triangles' corners in the proxy's space, each
// with its instance and its mesh's own vertex (for the materials), and the
// voxels as footprints.
typedef struct Gather {
    const Proxy *p;
    NPH_ARRAY(NphVec3) positions;
    NphArrayU32 vertex_instance;
    NphArrayU32 vertex_own;
    LodVoxelSourceArray triangles;
    NPH_ARRAY(LodVoxelFootprint) footprints;
} Gather;

static void gather_init(Gather *g, const Proxy *p)
{
    g->p = p;
    NPH_ARRAY_INIT(&g->positions, p->a);
    NPH_ARRAY_INIT(&g->vertex_instance, p->a);
    NPH_ARRAY_INIT(&g->vertex_own, p->a);
    NPH_ARRAY_INIT(&g->triangles, p->a);
    NPH_ARRAY_INIT(&g->footprints, p->a);
}

static void gather_clear(Gather *g)
{
    NPH_ARRAY_CLEAR(&g->positions);
    NPH_ARRAY_CLEAR(&g->vertex_instance);
    NPH_ARRAY_CLEAR(&g->vertex_own);
    NPH_ARRAY_CLEAR(&g->triangles);
    NPH_ARRAY_CLEAR(&g->footprints);
}

static void gather_free(Gather *g)
{
    NPH_ARRAY_FREE(&g->positions);
    NPH_ARRAY_FREE(&g->vertex_instance);
    NPH_ARRAY_FREE(&g->vertex_own);
    NPH_ARRAY_FREE(&g->triangles);
    NPH_ARRAY_FREE(&g->footprints);
}

static NphResult gather_cluster(Gather *g, u32 instance, u32 c)
{
    const Proxy *p = g->p;
    const u32 mesh = proxy_instance(p, instance)->mesh;
    const NphRenderProxyMesh *pm = &p->in->meshes[mesh];
    const NphRenderClusterLods *l = pm->lods;
    const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
    if (m->submesh >= pm->submesh_count) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 submesh = p->submesh_base[mesh] + m->submesh;
    if (m->vertex_count == 0U) {
        // A voxel cluster: each voxel of its bricks, in its cell.
        if ((u64)m->vertex_offset + m->triangle_count > l->bricks.len) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        for (u32 k = 0; k < m->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
            u32 rank = 0U;
            for (u32 bit = 0; bit < NPH_RENDER_VOXEL_BRICK_CELLS; ++bit) {
                if (((br->mask[bit >> 5U] >> (bit & 31U)) & 1U) == 0U) {
                    continue;
                }
                if ((u64)br->first_voxel + rank >= l->voxels.len) {
                    return NPH_RESULT_INVALID_ARGUMENT;
                }
                LodVoxelFootprint f;
                memset(&f, 0, sizeof f);
                f.voxel = l->voxels.data[br->first_voxel + rank];
                f.base = nph_vec3(br->origin.x + (f32)(bit & 3U) * br->cell,
                                  br->origin.y + (f32)((bit >> 2U) & 3U) * br->cell,
                                  br->origin.z + (f32)(bit >> 4U) * br->cell);
                f.cell = br->cell;
                f.transform = instance;
                f.submesh = submesh;
                NPH_TRY(NPH_ARRAY_PUSH(&g->footprints, f));
                rank += 1U;
            }
        }
        return NPH_RESULT_OK;
    }
    if ((u64)m->vertex_offset + m->vertex_count > l->clusters.vertices.len ||
        ((u64)m->triangle_offset + m->triangle_count) * 3U > l->clusters.triangles.len) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 base = g->positions.len;
    const LodVoxelTransform *t = &p->transforms[instance];
    for (u32 k = 0; k < m->vertex_count; ++k) {
        const u32 v = l->clusters.vertices.data[m->vertex_offset + k];
        NPH_TRY(NPH_ARRAY_PUSH(&g->positions, transform_point(t, pm->positions[v])));
        NPH_TRY(NPH_ARRAY_PUSH(&g->vertex_instance, instance));
        NPH_TRY(NPH_ARRAY_PUSH(&g->vertex_own, v));
    }
    for (u32 k = 0; k < m->triangle_count; ++k) {
        const u8 *corner = &l->clusters.triangles.data[((u64)m->triangle_offset + k) * 3U];
        if (corner[0] >= m->vertex_count || corner[1] >= m->vertex_count ||
            corner[2] >= m->vertex_count) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        const LodVoxelSource s = {{base + corner[0], base + corner[1], base + corner[2]}, submesh};
        NPH_TRY(NPH_ARRAY_PUSH(&g->triangles, s));
    }
    return NPH_RESULT_OK;
}

// The materials through the instances: a triangle's point sampled by its
// mesh's sampler at its own vertices, its normal taken into the proxy's
// space; an occluder's mesh without a sampler is opaque grey.
static void proxy_sample(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint,
                         NphRenderVoxelSample *out)
{
    const Gather *g = (const Gather *)user;
    const Proxy *p = g->p;
    const u32 instance = g->vertex_instance.data[v[0]];
    const u32 mesh = p->submesh_mesh[submesh];
    const NphRenderVoxelSampler *s = p->in->meshes[mesh].sampler;
    if (s == NULL) {
        const NphVec3 *q = g->positions.data;
        out->base_color = nph_vec3_splat(0.5F);
        out->normal = nph_vec3_normalize(
            nph_vec3_cross(nph_vec3_sub(q[v[1]], q[v[0]]), nph_vec3_sub(q[v[2]], q[v[0]])));
        out->roughness = 1.0F;
        out->metallic = 0.0F;
        out->occlusion = 1.0F;
        return;
    }
    const u32 own[3] = {g->vertex_own.data[v[0]], g->vertex_own.data[v[1]],
                        g->vertex_own.data[v[2]]};
    s->sample(s->user, submesh - p->submesh_base[mesh], own, b,
              (f32)((f64)footprint / p->stretch[instance]), out);
    const LodVoxelTransform *t = &p->transforms[instance];
    const f64 n[3] = {(f64)out->normal.x, (f64)out->normal.y, (f64)out->normal.z};
    f64 m[3];
    for (u32 r = 0; r < 3U; ++r) {
        m[r] = t->normal[r][0] * n[0] + t->normal[r][1] * n[1] + t->normal[r][2] * n[2];
    }
    const f64 len2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
    if (len2 > 0.0 && nph_f64_is_finite(len2)) {
        const f64 inv = 1.0 / sqrt(len2);
        out->normal = nph_vec3((f32)(m[0] * inv), (f32)(m[1] * inv), (f32)(m[2] * inv));
    }
}

static bool proxy_covers(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint)
{
    const Gather *g = (const Gather *)user;
    const Proxy *p = g->p;
    const u32 instance = g->vertex_instance.data[v[0]];
    const u32 mesh = p->submesh_mesh[submesh];
    const NphRenderVoxelSampler *s = p->in->meshes[mesh].sampler;
    if (s == NULL || s->covers == NULL) {
        return true;
    }
    const u32 own[3] = {g->vertex_own.data[v[0]], g->vertex_own.data[v[1]],
                        g->vertex_own.data[v[2]]};
    return s->covers(s->user, submesh - p->submesh_base[mesh], own, b,
                     (f32)((f64)footprint / p->stretch[instance]));
}

// ---------------------------------------------------------------------------
// A level, tile by tile
// ---------------------------------------------------------------------------

// The cells a tile keeps: its own (along x and z), and of those the ones
// whose ancestor `shift` levels up is in `set` (NULL: all).
typedef struct TileKeep {
    i64 x0, x1, z0, z1;
    const NphHashMap *set;
    u32 shift;
} TileKeep;

static bool tile_keep(void *user, const i32 c[3])
{
    const TileKeep *k = (const TileKeep *)user;
    if (c[0] < k->x0 || c[0] >= k->x1 || c[2] < k->z0 || c[2] >= k->z1) {
        return false;
    }
    if (k->set == NULL) {
        return true;
    }
    const i64 n = (i64)1 << k->shift;
    return nph_hashmap_contains_u64(
        k->set, key_of(floor_div(c[0], n), floor_div(c[1], n), floor_div(c[2], n)));
}

// Voxelizes the sources of `items` at cells of edge `cell` into `out`
// (appended), tile by tile, keeping the cells `set` and `shift` name as
// TileKeep has them.
static NphResult voxelize_level(Proxy *p, const CutItemArray *items, f64 cell,
                                const NphHashMap *set, u32 shift, LodVoxelCellArray *out)
{
    if (items->len == 0U) {
        return NPH_RESULT_OK;
    }
    NphAllocator *a = p->a;
    f64 lo[2] = {NPH_F64_MAX, NPH_F64_MAX};
    f64 hi[2] = {-NPH_F64_MAX, -NPH_F64_MAX};
    for (u32 i = 0; i < items->len; ++i) {
        const NphAABB *b = &items->data[i].box;
        lo[0] = nph_f64_min(lo[0], (f64)b->min.x);
        lo[1] = nph_f64_min(lo[1], (f64)b->min.z);
        hi[0] = nph_f64_max(hi[0], (f64)b->max.x);
        hi[1] = nph_f64_max(hi[1], (f64)b->max.z);
    }
    for (u32 k = 0; k < 2U; ++k) {
        lo[k] = floor(lo[k] / cell);
        hi[k] = floor(hi[k] / cell);
        if (!(lo[k] > -(f64)LOD_VOXEL_COORD_LIMIT) || !(hi[k] < (f64)LOD_VOXEL_COORD_LIMIT)) {
            return NPH_RESULT_OVERFLOW;
        }
    }
    const i64 tx0 = floor_div((i64)lo[0], TILE_CELLS);
    const i64 tx1 = floor_div((i64)hi[0], TILE_CELLS);
    const i64 tz0 = floor_div((i64)lo[1], TILE_CELLS);
    const i64 tz1 = floor_div((i64)hi[1], TILE_CELLS);
    Gather g;
    gather_init(&g, p);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, a);
    NphArrayU32 homes;
    NPH_ARRAY_INIT(&homes, a);
    NphRenderVoxelSampler sampler;
    memset(&sampler, 0, sizeof sampler);
    sampler.user = &g;
    sampler.submesh_surface = p->surface;
    sampler.sample = proxy_sample;
    sampler.covers = proxy_covers;
    NphResult r = NPH_RESULT_OK;
    for (i64 tz = tz0; tz <= tz1 && r == NPH_RESULT_OK; ++tz) {
        for (i64 tx = tx0; tx <= tx1 && r == NPH_RESULT_OK; ++tx) {
            TileKeep keep;
            keep.x0 = tx * TILE_CELLS;
            keep.x1 = keep.x0 + TILE_CELLS;
            keep.z0 = tz * TILE_CELLS;
            keep.z1 = keep.z0 + TILE_CELLS;
            keep.set = set;
            keep.shift = shift;
            NphAABB tile;
            tile.min =
                nph_vec3((f32)((f64)keep.x0 * cell), -NPH_F32_MAX, (f32)((f64)keep.z0 * cell));
            tile.max =
                nph_vec3((f32)((f64)keep.x1 * cell), NPH_F32_MAX, (f32)((f64)keep.z1 * cell));
            gather_clear(&g);
            for (u32 i = 0; i < items->len && r == NPH_RESULT_OK; ++i) {
                if (aabb_overlap(&items->data[i].box, &tile)) {
                    r = gather_cluster(&g, items->data[i].instance, items->data[i].cluster);
                }
            }
            if (r != NPH_RESULT_OK || (g.triangles.len == 0U && g.footprints.len == 0U)) {
                continue;
            }
            p->stats.triangles += g.triangles.len;
            p->stats.footprints += g.footprints.len;
            LodVoxelInput vin;
            memset(&vin, 0, sizeof vin);
            vin.positions = g.positions.data;
            vin.sources = g.triangles.data;
            vin.source_count = g.triangles.len;
            vin.sampler = &sampler;
            vin.scratch = a;
            vin.footprints = g.footprints.data;
            vin.footprint_count = g.footprints.len;
            vin.transforms = p->transforms;
            vin.keep = tile_keep;
            vin.keep_user = &keep;
            r = NPH_ARRAY_RESIZE(&homes, nph_max_u32(g.triangles.len, 1U));
            if (r == NPH_RESULT_OK) {
                r = lod_voxelize(&vin, cell, &cells, homes.data);
            }
            if (r == NPH_RESULT_OK && cells.len != 0U) {
                r = NPH_ARRAY_PUSH_N(out, cells.data, cells.len);
            }
        }
    }
    NPH_ARRAY_FREE(&cells);
    NPH_ARRAY_FREE(&homes);
    gather_free(&g);
    return r;
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

typedef struct Splat {
    NphRenderVoxel voxel;
    f32 base[3]; // its cell's lowest corner
    f32 cell;
    f32 depth; // along the direction swept
    u32 own;   // the visibility level's cell, NPH_U32_MAX for an occluder's
} Splat;

typedef struct SortItem {
    u32 key;
    u32 index;
} SortItem;

// Sorts `items` by key (least first, stable) through `tmp`: an LSD radix sort
// of four passes of eight bits.
static void radix_sort(SortItem *items, SortItem *tmp, u32 n)
{
    for (u32 pass = 0; pass < 4U; ++pass) {
        const u32 shift = pass * 8U;
        u32 count[257];
        memset(count, 0, sizeof count);
        for (u32 i = 0; i < n; ++i) {
            count[((items[i].key >> shift) & 255U) + 1U] += 1U;
        }
        for (u32 k = 0; k < 256U; ++k) {
            count[k + 1U] += count[k];
        }
        for (u32 i = 0; i < n; ++i) {
            tmp[count[(items[i].key >> shift) & 255U]++] = items[i];
        }
        memcpy(items, tmp, (size_t)n * sizeof(SortItem));
    }
}

// Direction k of n spread over the sphere: a Fibonacci lattice (Gonzalez,
// "Measurement of Areas on a Sphere Using Fibonacci and Latitude-Longitude
// Lattices", Mathematical Geosciences 2010).
static void sweep_direction(u32 k, u32 n, f64 out[3])
{
    const f64 y = 1.0 - (2.0 * (f64)k + 1.0) / (f64)n;
    const f64 r = sqrt(nph_f64_max(1.0 - y * y, 0.0));
    const f64 phi = (f64)k * 2.39996322972865332; // the golden angle, pi (3 - sqrt 5)
    out[0] = r * cos(phi);
    out[1] = y;
    out[2] = r * sin(phi);
}

// Whether the line through `q` along `d` meets the cube of lowest corner
// `lo` and edge `e`.
static bool line_meets_cube(const f64 q[3], const f64 d[3], const f32 lo[3], f64 e)
{
    f64 s0 = -NPH_F64_MAX;
    f64 s1 = NPH_F64_MAX;
    for (u32 a = 0; a < 3U; ++a) {
        const f64 l = (f64)lo[a] - q[a];
        const f64 h = l + e;
        if (nph_f64_abs(d[a]) < 1e-12) {
            if (l > 0.0 || h < 0.0) {
                return false;
            }
            continue;
        }
        const f64 t0 = l / d[a];
        const f64 t1 = h / d[a];
        s0 = nph_f64_max(s0, nph_f64_min(t0, t1));
        s1 = nph_f64_min(s1, nph_f64_max(t0, t1));
    }
    return s0 <= s1;
}

// The sweep: for each direction, a grid of parallel rays half the
// visibility level's cell apart across the plane facing it, and the splats
// front to back (by their cells' centres: an order every ray along the
// direction keeps through a grid of cells); each ray stops at the first
// footprint it meets, which is seen. A footprint no ray meets (smaller than
// the rays' spacing) is seen where some ray through its cell is still open.
static NphResult sweep(Proxy *p, Splat *splats, u32 n, f64 texel, u32 directions, u8 *seen)
{
    NphAllocator *a = p->a;
    SortItem *items = NPH_ALLOC_ARRAY(a, SortItem, nph_max_u32(n, 1U));
    SortItem *tmp = NPH_ALLOC_ARRAY(a, SortItem, nph_max_u32(n, 1U));
    NphArrayU8 open; // per ray: nothing met yet
    NPH_ARRAY_INIT(&open, a);
    NphResult r = items != NULL && tmp != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    for (u32 k = 0; k < directions && r == NPH_RESULT_OK && n != 0U; ++k) {
        f64 d[3];
        sweep_direction(k, directions, d);
        // A basis of the plane facing d.
        const f64 helper[3] = {nph_f64_abs(d[1]) < 0.9 ? 0.0 : 1.0,
                               nph_f64_abs(d[1]) < 0.9 ? 1.0 : 0.0, 0.0};
        f64 e1[3] = {helper[1] * d[2] - helper[2] * d[1], helper[2] * d[0] - helper[0] * d[2],
                     helper[0] * d[1] - helper[1] * d[0]};
        const f64 inv1 = 1.0 / sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
        for (u32 c = 0; c < 3U; ++c) {
            e1[c] *= inv1;
        }
        const f64 e2[3] = {d[1] * e1[2] - d[2] * e1[1], d[2] * e1[0] - d[0] * e1[2],
                           d[0] * e1[1] - d[1] * e1[0]};
        const f64 reach1 = nph_f64_abs(e1[0]) + nph_f64_abs(e1[1]) + nph_f64_abs(e1[2]);
        const f64 reach2 = nph_f64_abs(e2[0]) + nph_f64_abs(e2[1]) + nph_f64_abs(e2[2]);
        const NphVec3 dir = nph_vec3((f32)d[0], (f32)d[1], (f32)d[2]);
        f64 u0 = NPH_F64_MAX;
        f64 v0 = NPH_F64_MAX;
        f64 u1 = -NPH_F64_MAX;
        f64 v1 = -NPH_F64_MAX;
        f64 t0 = NPH_F64_MAX;
        f64 t1 = -NPH_F64_MAX;
        for (u32 i = 0; i < n; ++i) {
            Splat *s = &splats[i];
            const f64 h = 0.5 * (f64)s->cell;
            const f64 c[3] = {(f64)s->base[0] + h, (f64)s->base[1] + h, (f64)s->base[2] + h};
            const f64 u = c[0] * e1[0] + c[1] * e1[1] + c[2] * e1[2];
            const f64 v = c[0] * e2[0] + c[1] * e2[1] + c[2] * e2[2];
            const f64 t = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
            u0 = nph_f64_min(u0, u - h * reach1);
            u1 = nph_f64_max(u1, u + h * reach1);
            v0 = nph_f64_min(v0, v - h * reach2);
            v1 = nph_f64_max(v1, v + h * reach2);
            t0 = nph_f64_min(t0, t);
            t1 = nph_f64_max(t1, t);
            s->depth = (f32)t;
        }
        const f64 fw = ceil((u1 - u0) / texel) + 1.0;
        const f64 fh = ceil((v1 - v0) / texel) + 1.0;
        if (!(fw * fh <= (f64)SWEEP_MAX_TEXELS)) {
            r = NPH_RESULT_INVALID_ARGUMENT;
            break;
        }
        const u32 w = (u32)fw;
        const u32 hgt = (u32)fh;
        r = NPH_ARRAY_RESIZE(&open, w * hgt);
        if (r != NPH_RESULT_OK) {
            break;
        }
        memset(open.data, 1, (size_t)w * hgt);
        // Front (largest t) first.
        const f64 scale = t1 > t0 ? 4294967295.0 / (t1 - t0) : 0.0;
        for (u32 i = 0; i < n; ++i) {
            items[i].key =
                (u32)nph_f64_clamp((t1 - (f64)splats[i].depth) * scale, 0.0, 4294967295.0);
            items[i].index = i;
        }
        radix_sort(items, tmp, n);
        for (u32 o = 0; o < n; ++o) {
            const Splat *s = &splats[items[o].index];
            const f64 h = 0.5 * (f64)s->cell;
            const f64 c[3] = {(f64)s->base[0] + h, (f64)s->base[1] + h, (f64)s->base[2] + h};
            const f64 u = c[0] * e1[0] + c[1] * e1[1] + c[2] * e1[2];
            const f64 v = c[0] * e2[0] + c[1] * e2[1] + c[2] * e2[2];
            const f64 t = (f64)s->depth;
            const NphVec3 base = nph_vec3(s->base[0], s->base[1], s->base[2]);
            // The rays whose places u0 + (i + 1/2) texel lie in its cell's
            // shadow's bounds, each from in front of the cell through it.
            const i64 i0 = (i64)ceil((u - h * reach1 - u0) / texel - 0.5);
            const i64 i1 = (i64)floor((u + h * reach1 - u0) / texel - 0.5);
            const i64 j0 = (i64)ceil((v - h * reach2 - v0) / texel - 0.5);
            const i64 j1 = (i64)floor((v + h * reach2 - v0) / texel - 0.5);
            bool met = false;
            bool lit = false;
            bool crossed = false; // some open ray crosses its cell
            for (i64 j = nph_max_i64(j0, 0); j <= j1 && j < (i64)hgt; ++j) {
                for (i64 i = nph_max_i64(i0, 0); i <= i1 && i < (i64)w; ++i) {
                    const f64 tu = u0 + ((f64)i + 0.5) * texel;
                    const f64 tv = v0 + ((f64)j + 0.5) * texel;
                    // From 2h in front of the cell's centre, back along d.
                    const f64 q[3] = {tu * e1[0] + tv * e2[0] + (t + 2.0 * h) * d[0],
                                      tu * e1[1] + tv * e2[1] + (t + 2.0 * h) * d[1],
                                      tu * e1[2] + tv * e2[2] + (t + 2.0 * h) * d[2]};
                    if (!line_meets_cube(q, d, s->base, 2.0 * h)) {
                        continue;
                    }
                    u8 *ray = &open.data[(u64)j * w + (u64)i];
                    crossed = crossed || *ray != 0U;
                    const NphVec3 origin = nph_vec3((f32)q[0], (f32)q[1], (f32)q[2]);
                    if (!voxel_footprint_hit(&s->voxel, base, s->cell, origin, nph_vec3_neg(dir),
                                             0.0F, (f32)(4.0 * h))) {
                        continue;
                    }
                    met = true;
                    lit = lit || *ray != 0U;
                    *ray = 0U;
                }
            }
            if (s->own != NPH_U32_MAX && (met ? lit : crossed)) {
                seen[s->own] = 1U;
            }
        }
    }
    NPH_ARRAY_FREE(&open);
    if (tmp != NULL) {
        NPH_FREE_ARRAY(a, SortItem, tmp, nph_max_u32(n, 1U));
    }
    if (items != NULL) {
        NPH_FREE_ARRAY(a, SortItem, items, nph_max_u32(n, 1U));
    }
    return r;
}

// Which of `own` (at cells of edge `cell`) some direction sees through them
// and `occ` (at cells of edge `occ_cell`), in `seen` (own->len entries,
// zeroed by the caller); then, unless the flags say otherwise, their
// neighbours too.
static NphResult visibility(Proxy *p, const LodVoxelCellArray *own, f64 cell,
                            const LodVoxelCellArray *occ, f64 occ_cell, u8 *seen)
{
    NphAllocator *a = p->a;
    const NphRenderCellProxyInput *in = p->in;
    const u32 n = own->len + occ->len;
    Splat *splats = NPH_ALLOC_ARRAY(a, Splat, nph_max_u32(n, 1U));
    if (splats == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < n; ++i) {
        const bool mine = i < own->len;
        const LodVoxelCell *c = mine ? &own->data[i] : &occ->data[i - own->len];
        const f64 e = mine ? cell : occ_cell;
        Splat *s = &splats[i];
        s->voxel = c->voxel;
        for (u32 k = 0; k < 3U; ++k) {
            s->base[k] = (f32)((f64)c->c[k] * e);
        }
        s->cell = (f32)e;
        s->depth = 0.0F;
        s->own = mine ? i : NPH_U32_MAX;
    }
    const u32 directions =
        in->visibility_directions != 0U ? in->visibility_directions : NPH_RENDER_PROXY_DIRECTIONS;
    NphResult r = sweep(p, splats, n, 0.5 * cell, directions, seen);
    NPH_FREE_ARRAY(a, Splat, splats, nph_max_u32(n, 1U));
    for (u32 i = 0; i < own->len && r == NPH_RESULT_OK; ++i) {
        p->stats.visible += seen[i];
    }
    if (r != NPH_RESULT_OK || (in->flags & NPH_RENDER_PROXY_NO_DILATION) != 0U) {
        return r;
    }
    // The neighbours of the seen: one more cell round them.
    NphHashMap set;
    const NphHashMapDesc desc = nph_hashmap_desc_u64(0U);
    NPH_TRY(nph_hashmap_init(&set, a, &desc));
    for (u32 i = 0; i < own->len && r == NPH_RESULT_OK; ++i) {
        if (seen[i] != 0U) {
            const i32 *c = own->data[i].c;
            r = nph_hashmap_insert_u64(&set, key_of(c[0], c[1], c[2]), NULL);
        }
    }
    for (u32 i = 0; i < own->len && r == NPH_RESULT_OK; ++i) {
        if (seen[i] != 0U) {
            continue;
        }
        const i32 *c = own->data[i].c;
        for (i32 k = 0; k < 27 && seen[i] == 0U; ++k) {
            const i64 x = (i64)c[0] + (k % 3) - 1;
            const i64 y = (i64)c[1] + ((k / 3) % 3) - 1;
            const i64 z = (i64)c[2] + (k / 9) - 1;
            if (k != 13 && nph_hashmap_contains_u64(&set, key_of(x, y, z))) {
                seen[i] = 2U;
            }
        }
    }
    nph_hashmap_free(&set);
    return r;
}

// ---------------------------------------------------------------------------
// The levels' records
// ---------------------------------------------------------------------------

typedef struct Placed {
    u64 block;
    u32 cell;
} Placed;

static int compare_placed(const void *x, const void *y)
{
    const Placed *p = (const Placed *)x;
    const Placed *q = (const Placed *)y;
    if (p->block != q->block) {
        return p->block < q->block ? -1 : 1;
    }
    return p->cell < q->cell ? -1 : (p->cell > q->cell ? 1 : 0);
}

typedef NPH_ARRAY(Placed) PlacedArray;

typedef struct Level {
    LodVoxelCellArray cells;
    f64 cell;
    // Its clusters (once made): the first, how many, and each one's block
    // (its cells', CLUSTER_BLOCK of them along each axis).
    u32 first_cluster;
    u32 clusters;
    NPH_ARRAY(u64) cluster_block;
} Level;

// The cells of `level` in blocks of `block` cells, in block order, into
// `order` (one entry a cell).
static NphResult place_cells(const Level *level, i64 block, PlacedArray *order)
{
    NPH_ARRAY_CLEAR(order);
    for (u32 i = 0; i < level->cells.len; ++i) {
        const Placed pl = {block_key(level->cells.data[i].c, block), i};
        NPH_TRY(NPH_ARRAY_PUSH(order, pl));
    }
    if (order->len > 1U) {
        qsort(order->data, order->len, sizeof(Placed), compare_placed);
    }
    return NPH_RESULT_OK;
}

// The clusters of one block's cells (order[from, to)), made by `group` at
// `level`, appended to `out` with their blocks.
static NphResult emit_block(Proxy *p, NphRenderClusterLods *out, Level *lv, u32 level,
                            const Placed *order, u32 from, u32 to, u32 group,
                            LodVoxelCellArray *scratch, u32 *cell_cluster)
{
    NPH_ARRAY_CLEAR(scratch);
    for (u32 k = from; k < to; ++k) {
        LodVoxelCell c = lv->cells.data[order[k].cell];
        c.submesh = 0U;
        NPH_TRY(NPH_ARRAY_PUSH(scratch, c));
    }
    u32 made = 0U;
    NPH_TRY(lod_voxel_emit(out, p->a, scratch, lv->cell, group, level, cell_cluster, &made));
    for (u32 k = 0; k < made; ++k) {
        NPH_TRY(NPH_ARRAY_PUSH(&lv->cluster_block, order[from].block));
    }
    lv->clusters += made;
    return NPH_RESULT_OK;
}

static NphSphere cluster_sphere(const NphRenderClusterLods *out, u32 c)
{
    const NphRenderMeshletBounds *b = &out->clusters.bounds.data[c];
    return nph_sphere(b->center, b->radius);
}

// The records of levels [0, count): level 0's clusters block by block, then
// for each level the groups of its clusters' parent blocks, each making the
// next level's cluster of its block; the root group over the last level's;
// the hierarchy.
static NphResult assemble(Proxy *p, Level *levels, u32 count, NphRenderClusterLods *out)
{
    NphAllocator *a = p->a;
    PlacedArray order;
    NPH_ARRAY_INIT(&order, a);
    LodVoxelCellArray scratch;
    NPH_ARRAY_INIT(&scratch, a);
    NphArrayU32 cell_cluster;
    NPH_ARRAY_INIT(&cell_cluster, a);
    NPH_ARRAY(NphSphere) own; // per cluster: its maker's sphere (level 0: its bounds)
    NPH_ARRAY_INIT(&own, a);
    NPH_ARRAY(NphSphere) parts;
    NPH_ARRAY_INIT(&parts, a);
    PlacedArray consumed; // the level's clusters by their group's block
    NPH_ARRAY_INIT(&consumed, a);
    NphResult r = NPH_RESULT_OK;
    // Level 0, made by no group.
    Level *l0 = &levels[0];
    l0->first_cluster = out->clusters.meshlets.len;
    r = place_cells(l0, CLUSTER_BLOCK, &order);
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&cell_cluster, nph_max_u32(l0->cells.len, 1U));
    }
    for (u32 i = 0; i < order.len && r == NPH_RESULT_OK;) {
        u32 j = i;
        while (j < order.len && order.data[j].block == order.data[i].block) {
            ++j;
        }
        r = emit_block(p, out, l0, 0U, order.data, i, j, NPH_RENDER_LOD_NONE, &scratch,
                       cell_cluster.data);
        i = j;
    }
    for (u32 c = l0->first_cluster; c < out->clusters.meshlets.len && r == NPH_RESULT_OK; ++c) {
        r = NPH_ARRAY_PUSH(&own, cluster_sphere(out, c));
    }
    p->stats.clusters[0] = l0->clusters;
    for (u32 lvl = 0; lvl + 1U < count && r == NPH_RESULT_OK; ++lvl) {
        Level *lv = &levels[lvl];
        Level *next = &levels[lvl + 1U];
        // This level's clusters by their group's block (twice theirs), and
        // the next level's cells by the same blocks (their cluster blocks).
        NPH_ARRAY_CLEAR(&consumed);
        for (u32 k = 0; k < lv->clusters && r == NPH_RESULT_OK; ++k) {
            const u64 b = lv->cluster_block.data[k];
            const i64 bias = LOD_VOXEL_COORD_LIMIT;
            const i32 bc[3] = {(i32)((i64)(b & 0x1FFFFFU) - bias),
                               (i32)((i64)((b >> 21U) & 0x1FFFFFU) - bias),
                               (i32)((i64)((b >> 42U) & 0x1FFFFFU) - bias)};
            const Placed pl = {block_key(bc, 2), lv->first_cluster + k};
            r = NPH_ARRAY_PUSH(&consumed, pl);
        }
        if (r == NPH_RESULT_OK && consumed.len > 1U) {
            qsort(consumed.data, consumed.len, sizeof(Placed), compare_placed);
        }
        if (r == NPH_RESULT_OK) {
            r = place_cells(next, CLUSTER_BLOCK, &order);
        }
        if (r == NPH_RESULT_OK) {
            r = NPH_ARRAY_RESIZE(&cell_cluster, nph_max_u32(next->cells.len, 1U));
        }
        next->first_cluster = out->clusters.meshlets.len;
        // The groups: the blocks of either, in block order.
        u32 i = 0U;
        u32 j = 0U;
        while ((i < consumed.len || j < order.len) && r == NPH_RESULT_OK) {
            u64 block = NPH_U64_MAX;
            if (i < consumed.len) {
                block = consumed.data[i].block;
            }
            if (j < order.len && order.data[j].block < block) {
                block = order.data[j].block;
            }
            u32 i_end = i;
            while (i_end < consumed.len && consumed.data[i_end].block == block) {
                ++i_end;
            }
            u32 j_end = j;
            while (j_end < order.len && order.data[j_end].block == block) {
                ++j_end;
            }
            const u32 group = out->groups.len;
            const u32 made_first = out->clusters.meshlets.len;
            if (j_end > j) {
                r = emit_block(p, out, next, lvl + 1U, order.data, j, j_end, group, &scratch,
                               cell_cluster.data);
            }
            // Its sphere: round what it consumes (their makers' spheres) and
            // what it makes.
            NPH_ARRAY_CLEAR(&parts);
            for (u32 k = i; k < i_end && r == NPH_RESULT_OK; ++k) {
                r = NPH_ARRAY_PUSH(&parts, own.data[consumed.data[k].cell]);
            }
            for (u32 c = made_first; c < out->clusters.meshlets.len && r == NPH_RESULT_OK; ++c) {
                r = NPH_ARRAY_PUSH(&parts, cluster_sphere(out, c));
            }
            if (r != NPH_RESULT_OK) {
                break;
            }
            const NphSphere sphere = lod_sphere_hull(parts.data, parts.len);
            NphRenderLodGroup rec;
            memset(&rec, 0, sizeof rec);
            rec.center = sphere.center;
            rec.radius = sphere.radius;
            rec.error = (f32)next->cell;
            rec.level = lvl;
            rec.cluster_offset = out->group_clusters.len;
            rec.cluster_count = i_end - i;
            for (u32 k = i; k < i_end && r == NPH_RESULT_OK; ++k) {
                out->lods.data[consumed.data[k].cell].parent = group;
                r = NPH_ARRAY_PUSH(&out->group_clusters, consumed.data[k].cell);
            }
            for (u32 c = made_first; c < out->clusters.meshlets.len && r == NPH_RESULT_OK; ++c) {
                r = NPH_ARRAY_PUSH(&own, sphere);
            }
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&out->groups, rec);
            }
            i = i_end;
            j = j_end;
        }
        p->stats.clusters[lvl + 1U] = next->clusters;
    }
    // The root group over the last level's clusters.
    const Level *top = &levels[count - 1U];
    if (r == NPH_RESULT_OK && top->clusters != 0U) {
        NPH_ARRAY_CLEAR(&parts);
        for (u32 k = 0; k < top->clusters && top->first_cluster + k < own.len && r == NPH_RESULT_OK;
             ++k) {
            const u32 c = top->first_cluster + k;
            r = NPH_ARRAY_PUSH(&parts, own.data[c]);
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&parts, cluster_sphere(out, c));
            }
        }
        if (r == NPH_RESULT_OK) {
            const NphSphere sphere = lod_sphere_hull(parts.data, parts.len);
            NphRenderLodGroup rec;
            memset(&rec, 0, sizeof rec);
            rec.center = sphere.center;
            rec.radius = sphere.radius;
            rec.error = NPH_F32_MAX;
            rec.level = count - 1U;
            rec.cluster_offset = out->group_clusters.len;
            rec.cluster_count = top->clusters;
            for (u32 k = 0; k < top->clusters && r == NPH_RESULT_OK; ++k) {
                out->lods.data[top->first_cluster + k].parent = out->groups.len;
                r = NPH_ARRAY_PUSH(&out->group_clusters, top->first_cluster + k);
            }
            if (r == NPH_RESULT_OK) {
                r = NPH_ARRAY_PUSH(&out->groups, rec);
            }
        }
    }
    if (r == NPH_RESULT_OK) {
        out->levels = count;
        out->level0_clusters = l0->clusters;
    }
    if (r == NPH_RESULT_OK && out->groups.len != 0U) {
        r = lod_build_hierarchy(out, a);
    }
    NPH_ARRAY_FREE(&order);
    NPH_ARRAY_FREE(&scratch);
    NPH_ARRAY_FREE(&cell_cluster);
    NPH_ARRAY_FREE(&own);
    NPH_ARRAY_FREE(&parts);
    NPH_ARRAY_FREE(&consumed);
    return r;
}

// ---------------------------------------------------------------------------
// The build
// ---------------------------------------------------------------------------

// Whether a level's cells span at most two cluster blocks along each axis
// (at most eight clusters): the proxy's coarsest level.
static bool level_is_top(const Level *lv)
{
    if (lv->cells.len == 0U) {
        return true;
    }
    i64 lo[3] = {INT64_MAX, INT64_MAX, INT64_MAX};
    i64 hi[3] = {INT64_MIN, INT64_MIN, INT64_MIN};
    for (u32 i = 0; i < lv->cells.len; ++i) {
        for (u32 k = 0; k < 3U; ++k) {
            const i64 b = floor_div(lv->cells.data[i].c[k], CLUSTER_BLOCK);
            lo[k] = b < lo[k] ? b : lo[k];
            hi[k] = b > hi[k] ? b : hi[k];
        }
    }
    return hi[0] - lo[0] <= 1 && hi[1] - lo[1] <= 1 && hi[2] - lo[2] <= 1;
}

// The keys of `seen`'s cells (cells of `own`) `up` levels up, into `out`
// (initialised here; freed again on failure).
static NphResult ancestor_set(NphAllocator *a, const LodVoxelCellArray *own, const u8 *seen, u32 up,
                              NphHashMap *out)
{
    const NphHashMapDesc desc = nph_hashmap_desc_u64(0U);
    NPH_TRY(nph_hashmap_init(out, a, &desc));
    const i64 n = (i64)1 << up;
    NphResult r = NPH_RESULT_OK;
    for (u32 i = 0; i < own->len && r == NPH_RESULT_OK; ++i) {
        if (seen[i] == 0U) {
            continue;
        }
        const i32 *c = own->data[i].c;
        const u64 key = key_of(floor_div(c[0], n), floor_div(c[1], n), floor_div(c[2], n));
        if (!nph_hashmap_contains_u64(out, key)) {
            r = nph_hashmap_insert_u64(out, key, NULL);
        }
    }
    if (r != NPH_RESULT_OK) {
        nph_hashmap_free(out);
    }
    return r;
}

NphResult nph_render_cell_proxy_build(const NphRenderCellProxyInput *in, NphRenderClusterLods *out,
                                      NphRenderCellProxyStats *stats)
{
    NPH_ASSERT(in != NULL && out != NULL);
    if (stats != NULL) {
        memset(stats, 0, sizeof *stats);
    }
    NPH_TRY(validate(in));
    Proxy p;
    NphResult r = proxy_init(&p, in);
    NphAllocator *a = in->scratch;
    const u32 v = nph_min_u32(in->visibility_level != 0U ? in->visibility_level : 1U,
                              NPH_RENDER_PROXY_MAX_LEVELS - 1U);
    p.stats.visibility_level = v;
    Level levels[NPH_RENDER_PROXY_MAX_LEVELS];
    memset(levels, 0, sizeof levels);
    for (u32 lvl = 0; lvl < NPH_RENDER_PROXY_MAX_LEVELS; ++lvl) {
        NPH_ARRAY_INIT(&levels[lvl].cells, a);
        NPH_ARRAY_INIT(&levels[lvl].cluster_block, a);
        levels[lvl].cell = (f64)in->cell * (f64)((u64)1 << lvl);
    }
    CutItemArray items;
    NPH_ARRAY_INIT(&items, a);
    LodVoxelCellArray occ;
    NPH_ARRAY_INIT(&occ, a);
    u8 *seen = NULL;
    u32 seen_count = 0U;
    u32 top = v;
    // The visibility level, whole, and what of it is seen.
    Level *lv = &levels[v];
    if (r == NPH_RESULT_OK) {
        r = level_cut(&p, 0U, in->instance_count, SOURCE_ERROR * lv->cell, NULL, &items);
    }
    if (r == NPH_RESULT_OK) {
        r = voxelize_level(&p, &items, lv->cell, NULL, 0U, &lv->cells);
    }
    if (r == NPH_RESULT_OK) {
        seen_count = nph_max_u32(lv->cells.len, 1U);
        seen = NPH_ALLOC_ARRAY_ZEROED(a, u8, seen_count);
        r = seen != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
        p.stats.swept = lv->cells.len;
    }
    if (r == NPH_RESULT_OK && (in->flags & NPH_RENDER_PROXY_NO_VISIBILITY) != 0U) {
        memset(seen, 1, seen_count);
        p.stats.visible = lv->cells.len;
    } else if (r == NPH_RESULT_OK) {
        // The occluders at twice the cell, within their bounds.
        const f64 occ_cell = 2.0 * lv->cell;
        if (in->occluder_count != 0U) {
            r = level_cut(&p, in->instance_count, p.instances, SOURCE_ERROR * occ_cell,
                          &in->occluder_bounds, &items);
            if (r == NPH_RESULT_OK) {
                r = voxelize_level(&p, &items, occ_cell, NULL, 0U, &occ);
            }
            // The occluders' cells within their bounds alone.
            u32 kept = 0U;
            for (u32 i = 0; i < occ.len && r == NPH_RESULT_OK; ++i) {
                const i32 *c = occ.data[i].c;
                const NphAABB cb = {
                    nph_vec3((f32)((f64)c[0] * occ_cell), (f32)((f64)c[1] * occ_cell),
                             (f32)((f64)c[2] * occ_cell)),
                    nph_vec3((f32)((f64)(c[0] + 1) * occ_cell), (f32)((f64)(c[1] + 1) * occ_cell),
                             (f32)((f64)(c[2] + 1) * occ_cell))};
                if (aabb_overlap(&cb, &in->occluder_bounds)) {
                    occ.data[kept++] = occ.data[i];
                }
            }
            occ.len = kept;
            p.stats.occluders = occ.len;
        }
        if (r == NPH_RESULT_OK) {
            r = visibility(&p, &lv->cells, lv->cell, &occ, occ_cell, seen);
        }
    }
    // The coarser levels: the cells holding a seen one.
    if (r == NPH_RESULT_OK && !level_is_top(lv)) {
        for (u32 lvl = v + 1U; lvl < NPH_RENDER_PROXY_MAX_LEVELS && r == NPH_RESULT_OK; ++lvl) {
            NphHashMap set;
            r = ancestor_set(a, &lv->cells, seen, lvl - v, &set);
            if (r != NPH_RESULT_OK) {
                break;
            }
            r = level_cut(&p, 0U, in->instance_count, SOURCE_ERROR * levels[lvl].cell, NULL,
                          &items);
            if (r == NPH_RESULT_OK) {
                r = voxelize_level(&p, &items, levels[lvl].cell, &set, 0U, &levels[lvl].cells);
            }
            nph_hashmap_free(&set);
            top = lvl;
            if (level_is_top(&levels[lvl])) {
                break;
            }
        }
    }
    // The finer levels: the cells inside a seen one.
    if (r == NPH_RESULT_OK && v > 0U) {
        NphHashMap set;
        r = ancestor_set(a, &lv->cells, seen, 0U, &set);
        if (r == NPH_RESULT_OK) {
            for (u32 lvl = v; lvl-- > 0U && r == NPH_RESULT_OK;) {
                r = level_cut(&p, 0U, in->instance_count, SOURCE_ERROR * levels[lvl].cell, NULL,
                              &items);
                if (r == NPH_RESULT_OK) {
                    r = voxelize_level(&p, &items, levels[lvl].cell, &set, v - lvl,
                                       &levels[lvl].cells);
                }
            }
            nph_hashmap_free(&set);
        }
    }
    // The visibility level's own: its seen cells.
    if (r == NPH_RESULT_OK) {
        u32 kept = 0U;
        for (u32 i = 0; i < lv->cells.len; ++i) {
            if (seen[i] != 0U) {
                lv->cells.data[kept++] = lv->cells.data[i];
            }
        }
        lv->cells.len = kept;
    }
    // A level may be empty (geometry its rays miss); the proxy is empty where
    // every level is.
    bool any = false;
    for (u32 lvl = 0; lvl <= top; ++lvl) {
        any = any || levels[lvl].cells.len != 0U;
    }
    if (r == NPH_RESULT_OK && any) {
        r = assemble(&p, levels, top + 1U, out);
    }
    if (r == NPH_RESULT_OK) {
        p.stats.levels = any ? top + 1U : 0U;
        for (u32 lvl = 0; lvl <= top; ++lvl) {
            p.stats.voxels[lvl] = levels[lvl].cells.len;
        }
        if (stats != NULL) {
            *stats = p.stats;
        }
    }
    if (seen != NULL) {
        NPH_FREE_ARRAY(a, u8, seen, seen_count);
    }
    NPH_ARRAY_FREE(&occ);
    NPH_ARRAY_FREE(&items);
    for (u32 lvl = 0; lvl < NPH_RENDER_PROXY_MAX_LEVELS; ++lvl) {
        NPH_ARRAY_FREE(&levels[lvl].cells);
        NPH_ARRAY_FREE(&levels[lvl].cluster_block);
    }
    proxy_free(&p);
    return r;
}
