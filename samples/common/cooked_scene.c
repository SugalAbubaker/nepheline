// SPDX-License-Identifier: Apache-2.0
// The cooked-scene bridge (cooked_scene.h).
#include "cooked_scene.h"

#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/texture.h>
#include <nepheline/core/log.h>
#include <nepheline/math/mat.h>

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_COOKED_SCENE, "cooked_scene", NPH_LOG_LEVEL_INFO);

// The cooked streams are the renderer's streams, bit for bit.
NPH_STATIC_ASSERT(sizeof(NphMeshPosition) == sizeof(NphRenderVertexPosition),
                  "cooked positions are render positions");
NPH_STATIC_ASSERT(sizeof(NphMeshAttrib) == sizeof(NphRenderVertexAttrib),
                  "cooked attributes are render attributes");
NPH_STATIC_ASSERT(sizeof(NphMeshlet) == sizeof(NphRenderMeshlet) &&
                      sizeof(NphMeshletBounds) == sizeof(NphRenderMeshletBounds) &&
                      sizeof(NphMeshClusterLod) == sizeof(NphRenderClusterLod) &&
                      sizeof(NphMeshLodGroup) == sizeof(NphRenderLodGroup) &&
                      sizeof(NphMeshLodNode) == sizeof(NphRenderLodNode) &&
                      sizeof(NphMeshVoxelCluster) == sizeof(NphRenderVoxelCluster) &&
                      sizeof(NphMeshVoxelBrick) == sizeof(NphRenderPackedBrick) &&
                      sizeof(NphMeshLodMaker) == sizeof(NphRenderLodMaker),
                  "cooked meshlets and levels are the renderer's records (the cooker asserts each "
                  "field: tools/cook/src/mesh_meshlet.c, mesh_lod.c)");
NPH_STATIC_ASSERT((u32)NPH_MATERIAL_WRAP_MIRROR == (u32)NPH_RENDER_WRAP_MIRROR,
                  "the wrap modes share their numbering");
NPH_STATIC_ASSERT((u32)NPH_MATERIAL_ALPHA_BLEND == (u32)NPH_RENDER_ALPHA_BLEND,
                  "the alpha modes share their numbering");
NPH_STATIC_ASSERT((u32)NPH_MATERIAL_TEXTURE_EMISSIVE == (u32)NPH_RENDER_SLOT_EMISSIVE,
                  "the texture slots share their numbering");

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static u32 find_asset(const CookedScene *s, NphGuid guid)
{
    for (u32 i = 0; i < s->asset_count; ++i) {
        if (nph_guid_equals(s->guids[i], guid)) {
            return i;
        }
    }
    return NPH_U32_MAX;
}

static NphResult request(CookedScene *s, NphGuid guid, NphJobCounter *done)
{
    if (find_asset(s, guid) != NPH_U32_MAX) {
        return NPH_RESULT_OK;
    }
    if (s->asset_count == s->asset_cap) {
        return NPH_RESULT_FULL;
    }
    NphAssetHandle h = NPH_ASSET_HANDLE_NULL;
    NPH_TRY(nph_asset_load_guid_async(s->loader, guid, done, &h));
    s->guids[s->asset_count] = guid;
    s->assets[s->asset_count] = h;
    ++s->asset_count;
    return NPH_RESULT_OK;
}

static NphResult wait_all(CookedScene *s, NphJobSystem *jobs, NphJobCounter *done)
{
    nph_job_wait(jobs, done);
    for (u32 i = 0; i < s->asset_count; ++i) {
        const NphResult r = nph_asset_wait(s->loader, s->assets[i]);
        if (r != NPH_RESULT_OK) {
            char guid[NPH_GUID_STRING_LEN + 1U];
            nph_guid_to_string(s->guids[i], guid);
            NPH_LOG_ERROR(&LOG_COOKED_SCENE, "asset %s: %s", guid, nph_result_string(r));
            return r;
        }
    }
    return NPH_RESULT_OK;
}

static const NphAsset *asset_of(const CookedScene *s, NphGuid guid)
{
    const u32 i = find_asset(s, guid);
    return i == NPH_U32_MAX ? NULL : nph_asset_get(s->loader, s->assets[i]);
}

static NphResult alloc_tables(CookedScene *s, u32 cap)
{
    s->asset_cap = cap;
    s->assets = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphAssetHandle, cap);
    s->guids = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphGuid, cap);
    s->textures = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderTexture, cap);
    s->meshes = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderMesh, cap);
    s->asset_materials = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderMaterial, cap);
    return s->assets != NULL && s->guids != NULL && s->textures != NULL && s->meshes != NULL &&
                   s->asset_materials != NULL
               ? NPH_RESULT_OK
               : NPH_RESULT_OUT_OF_MEMORY;
}

// ---------------------------------------------------------------------------
// Render resources
// ---------------------------------------------------------------------------

static NphGpuFormat gpu_format(u32 format)
{
    switch (format) {
    case NPH_TEXTURE_FORMAT_R8_UNORM:
        return NPH_GPU_FORMAT_R8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8_UNORM:
        return NPH_GPU_FORMAT_R8G8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM:
        return NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB:
        return NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    case NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
        return NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    case NPH_TEXTURE_FORMAT_BC4_UNORM:
        return NPH_GPU_FORMAT_BC4_UNORM;
    case NPH_TEXTURE_FORMAT_BC5_UNORM:
        return NPH_GPU_FORMAT_BC5_UNORM;
    case NPH_TEXTURE_FORMAT_BC6H_UFLOAT:
        return NPH_GPU_FORMAT_BC6H_UFLOAT;
    case NPH_TEXTURE_FORMAT_BC7_UNORM:
        return NPH_GPU_FORMAT_BC7_UNORM;
    case NPH_TEXTURE_FORMAT_BC7_SRGB:
        return NPH_GPU_FORMAT_BC7_SRGB;
    default:
        return NPH_GPU_FORMAT_UNDEFINED;
    }
}

static NphResult create_texture(CookedScene *s, u32 index)
{
    NphTextureView tv;
    NPH_TRY(nph_texture_view(nph_asset_get(s->loader, s->assets[index]), &tv));
    const NphTextureHeader *h = tv.header;
    NphRenderTextureDesc d;
    memset(&d, 0, sizeof d);
    d.kind = h->kind == NPH_TEXTURE_CUBE ? NPH_RENDER_TEXTURE_CUBE : NPH_RENDER_TEXTURE_2D;
    d.format = gpu_format(h->format);
    d.width = h->width;
    d.height = h->height;
    d.mip_count = h->mip_count;
    d.layer_count = h->layer_count;
    d.mips = tv.mips;
    d.mip_sizes = tv.mip_sizes;
    d.flags = (h->flags & (u32)NPH_TEXTURE_FLAG_NORMAL_MAP) != 0U
                  ? (u32)NPH_RENDER_TEXTURE_FLAG_NORMAL_TWO_CHANNEL
                  : 0U;
    d.name = "cooked texture";
    const NphResult r = nph_render_texture_create(s->renderer, &d, &s->textures[index]);
    if (r == NPH_RESULT_UNSUPPORTED) {
        // A device without BC textures draws the material untextured.
        NPH_LOG_WARN(&LOG_COOKED_SCENE, "%s textures are not supported here; drawing untextured",
                     nph_texture_format_info(h->format)->name);
        return NPH_RESULT_OK;
    }
    return r;
}

// The cooked meshlets, and the levels of detail where the mesh has them
// (asset/mesh.h version 2) with their voxel clusters' bricks and voxels
// (version 3), into `d`: the same records as the renderer's, handed over as
// they are through untyped pointers.
static void cooked_clusters(const NphMeshView *mv, NphRenderMeshDesc *d)
{
    const NphMeshHeader *h = mv->header;
    if (h->meshlet_count == 0U) {
        return;
    }
    const void *meshlets = mv->meshlets;
    const void *bounds = mv->meshlet_bounds;
    d->meshlets = meshlets;
    d->meshlet_bounds = bounds;
    d->meshlet_count = h->meshlet_count;
    d->meshlet_vertices = mv->meshlet_vertices;
    d->meshlet_vertex_count = h->meshlet_vertex_count;
    d->meshlet_triangles = mv->meshlet_triangles;
    d->meshlet_triangle_count = h->meshlet_triangle_count;
    if (mv->lod == NULL) {
        return;
    }
    const void *clusters = mv->lod_clusters;
    const void *groups = mv->lod_groups;
    const void *nodes = mv->lod_nodes;
    d->cluster_lods = clusters;
    d->lod_groups = groups;
    d->lod_group_count = mv->lod->group_count;
    d->lod_group_clusters = mv->lod_members;
    d->lod_group_cluster_count = mv->lod->group_cluster_count;
    d->lod_nodes = nodes;
    d->lod_node_count = mv->lod->node_count;
    d->lod_level0_meshlets = mv->lod->level0_meshlets;
    const void *voxel_clusters = mv->voxel_clusters;
    const void *bricks = mv->voxel_bricks;
    d->voxel_clusters = voxel_clusters;
    d->voxel_cluster_count = mv->lod->voxel_cluster_count;
    d->voxel_bricks = bricks;
    d->voxel_brick_count = mv->lod->brick_count;
    d->voxel_words = mv->voxel_words;
    d->voxel_word_count = mv->lod->voxel_word_count;
    const void *makers = mv->lod_makers;
    d->lod_makers = makers;
}

// The render mesh of a mesh asset (a head of levels apart has none: its
// levels are meshes of their own, load_levels).
static NphResult create_mesh_of(CookedScene *s, const NphAsset *asset, NphRenderMesh *out)
{
    NphMeshView mv;
    NPH_TRY(nph_mesh_view(asset, &mv));
    if (mv.levels != NULL) {
        return NPH_RESULT_OK;
    }
    const NphMeshHeader *h = mv.header;
    if (h->submesh_count > NPH_RENDER_MAX_SUBMESHES) {
        return NPH_RESULT_UNSUPPORTED;
    }
    NphRenderSubmesh subs[NPH_RENDER_MAX_SUBMESHES];
    for (u32 i = 0; i < h->submesh_count; ++i) {
        subs[i].index_offset = mv.submeshes[i].index_offset;
        subs[i].index_count = mv.submeshes[i].index_count;
        subs[i].material_slot = mv.submeshes[i].material_slot;
    }
    NphRenderMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertex_count = h->vertex_count;
    d.index_count = h->index_count;
    // Same layout (asserted at the top): hand the cooked streams over as
    // they are, through untyped pointers.
    const void *positions = mv.positions;
    const void *attribs = mv.attribs;
    d.positions = positions;
    d.attribs = attribs;
    d.indices = mv.indices;
    d.submeshes = subs;
    d.submesh_count = h->submesh_count;
    d.position_offset = h->position_offset;
    d.position_scale = h->position_scale;
    d.bounds = h->bounds;
    cooked_clusters(&mv, &d);
    d.name = "cooked mesh";
    return nph_render_mesh_create(s->renderer, &d, out);
}

static NphResult create_mesh(CookedScene *s, u32 index)
{
    return create_mesh_of(s, nph_asset_get(s->loader, s->assets[index]), &s->meshes[index]);
}

// The levels' loads in flight at once: their assets, decompressed, held
// until their meshes are made.
#define LEVEL_BATCH 256U

typedef struct LevelBatch {
    NphAssetHandle handles[LEVEL_BATCH];
    u32 slots[LEVEL_BATCH]; // into level_meshes
    u32 count;
    NphJobCounter done;
} LevelBatch;

// The batch's levels: waited for, a mesh made of each, its asset released.
static NphResult flush_levels(CookedScene *s, const CookedSceneDesc *desc, LevelBatch *b)
{
    nph_job_wait(desc->jobs, &b->done);
    NphResult r = NPH_RESULT_OK;
    for (u32 i = 0; i < b->count; ++i) {
        NphResult ri = nph_asset_wait(s->loader, b->handles[i]);
        if (ri == NPH_RESULT_OK) {
            // As the stream makes them: a mesh that is not a level apart (a
            // head naming a head) is refused.
            ri = nph_proxy_level_mesh_create(s->renderer, nph_asset_get(s->loader, b->handles[i]),
                                             &s->level_meshes[b->slots[i]]);
        }
        r = r == NPH_RESULT_OK ? ri : r;
        nph_asset_release(s->loader, b->handles[i]);
    }
    b->count = 0U;
    return r;
}

// The cells' proxies' levels apart (asset/mesh.h version 6; ADR-0063): each
// head's levels (streamed, its coarsest alone) loaded LEVEL_BATCH at a time,
// a render mesh made of each and its asset released at once (the GPU holds
// it, and the levels are the bulk of a large world's proxies), each cell's
// run of them noted.
static NphResult load_levels(CookedScene *s, const CookedSceneDesc *desc, const NphSceneView *sv,
                             u32 cells)
{
    u32 total = 0U;
    for (u32 k = 0; k < cells; ++k) {
        const NphAsset *head =
            nph_guid_is_null(sv->cells[k].proxy) ? NULL : asset_of(s, sv->cells[k].proxy);
        NphMeshView mv;
        if (head != NULL && nph_mesh_view(head, &mv) == NPH_RESULT_OK) {
            total += mv.level_count;
        }
    }
    s->cell_level_first = NPH_ALLOC_ARRAY_ZEROED(s->alloc, u32, nph_max_u32(cells, 1U));
    s->cell_level_count = NPH_ALLOC_ARRAY_ZEROED(s->alloc, u32, nph_max_u32(cells, 1U));
    s->level_mesh_cap = nph_max_u32(total, 1U);
    s->level_meshes = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderMesh, s->level_mesh_cap);
    if (s->cell_level_first == NULL || s->cell_level_count == NULL || s->level_meshes == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    LevelBatch b;
    memset(&b, 0, sizeof b);
    NphResult r = NPH_RESULT_OK;
    for (u32 k = 0; k < cells && r == NPH_RESULT_OK; ++k) {
        const NphAsset *head =
            nph_guid_is_null(sv->cells[k].proxy) ? NULL : asset_of(s, sv->cells[k].proxy);
        NphMeshView mv;
        if (head == NULL || nph_mesh_view(head, &mv) != NPH_RESULT_OK || mv.levels == NULL) {
            continue;
        }
        s->cell_level_first[k] = s->level_mesh_count;
        const u32 first = desc->stream ? mv.level_count - 1U : 0U;
        s->cell_level_count[k] = mv.level_count - first;
        for (u32 i = first; i < mv.level_count && r == NPH_RESULT_OK; ++i) {
            if (b.count == LEVEL_BATCH) {
                r = flush_levels(s, desc, &b);
            }
            if (r == NPH_RESULT_OK) {
                r = nph_asset_load_guid_async(s->loader, mv.levels[i].mesh, &b.done,
                                              &b.handles[b.count]);
            }
            if (r == NPH_RESULT_OK) {
                b.slots[b.count++] = s->level_mesh_count++;
            }
        }
    }
    const NphResult rf = flush_levels(s, desc, &b);
    return r == NPH_RESULT_OK ? rf : r;
}

static NphRenderTexture texture_of(const CookedScene *s, NphGuid guid)
{
    if (nph_guid_is_null(guid)) {
        return NPH_RENDER_TEXTURE_NULL;
    }
    const u32 i = find_asset(s, guid);
    return i == NPH_U32_MAX ? NPH_RENDER_TEXTURE_NULL : s->textures[i];
}

static NphResult create_material(CookedScene *s, const NphAsset *asset, f32 emissive_scale,
                                 NphRenderMaterial *out)
{
    NphMaterialView mv;
    NPH_TRY(nph_material_view(asset, &mv));
    const NphMaterialHeader *h = mv.header;
    NphRenderMaterialDesc d;
    nph_render_material_desc_default(&d);
    d.base_color = h->base_color_factor;
    d.emissive = nph_vec3_scale(h->emissive_factor, h->emissive_strength * emissive_scale);
    d.metallic = h->metallic_factor;
    d.roughness = h->roughness_factor;
    d.alpha_cutoff = h->alpha_cutoff;
    d.alpha_mode = (NphRenderAlphaMode)h->alpha_mode;
    d.double_sided = (h->flags & (u32)NPH_MATERIAL_FLAG_DOUBLE_SIDED) != 0U;
    d.unlit = (h->flags & (u32)NPH_MATERIAL_FLAG_UNLIT) != 0U;
    d.normal_scale = h->textures[NPH_MATERIAL_TEXTURE_NORMAL].scale;
    d.occlusion_strength = h->textures[NPH_MATERIAL_TEXTURE_OCCLUSION].scale;
    for (u32 t = 0; t < (u32)NPH_RENDER_SLOT_COUNT; ++t) {
        const NphMaterialTextureRef *ref = &h->textures[t];
        d.textures[t].texture = texture_of(s, ref->texture);
        d.textures[t].wrap_u = (NphRenderWrap)ref->wrap_u;
        d.textures[t].wrap_v = (NphRenderWrap)ref->wrap_v;
        d.textures[t].nearest = ref->nearest != 0U;
    }
    return nph_render_material_create(s->renderer, &d, out);
}

// World transforms of every node: parents come first in a cooked scene.
static void world_matrices(const NphSceneView *sv, NphMat4 *out)
{
    for (u32 i = 0; i < sv->header->node_count; ++i) {
        const NphSceneNode *n = &sv->nodes[i];
        const NphMat4 local = nph_mat4_from_trs(n->translation, n->rotation, n->scale);
        out[i] = n->parent == NPH_SCENE_NO_NODE ? local : nph_mat4_mul(out[n->parent], local);
    }
}

static NphRenderTransform at_origin(const NphMat4 *m, NphDVec3 origin)
{
    NphRenderTransform t = nph_render_transform_from_mat4(m);
    t.translation = nph_dvec3_add(t.translation, origin);
    return t;
}

// The world partition's cells (asset/scene.h version 2; ADR-0062), each a
// render cell with its proxy at the proxy's origin, made before the instances
// that name them: its levels apart's meshes (load_levels), or the one mesh
// of a proxy cooked whole. A cell without a proxy (nothing of its instances
// voxelized) makes none: its instances are drawn as themselves.
static NphResult create_cells(CookedScene *s, const CookedSceneDesc *desc, const NphSceneView *sv)
{
    if (desc->stream) {
        NphProxyStreamDesc pd;
        memset(&pd, 0, sizeof pd);
        pd.alloc = s->alloc;
        pd.renderer = s->renderer;
        pd.loader = s->loader;
        pd.max_cells = nph_max_u32(s->cell_cap, 1U);
        pd.budget_bytes = desc->stream_budget;
        NPH_TRY(nph_proxy_stream_create(&pd, &s->stream));
    }
    for (u32 k = 0; k < s->cell_cap && k < sv->cell_count; ++k) {
        const NphSceneCell *c = &sv->cells[k];
        s->cells[k] = NPH_RENDER_CELL_NULL;
        s->cell_count = k + 1U;
        const u32 mi = nph_guid_is_null(c->proxy) ? NPH_U32_MAX : find_asset(s, c->proxy);
        if (mi == NPH_U32_MAX) {
            continue;
        }
        const u32 ma =
            nph_guid_is_null(c->proxy_material) ? NPH_U32_MAX : find_asset(s, c->proxy_material);
        const NphRenderMaterial material =
            ma == NPH_U32_MAX ? NPH_RENDER_MATERIAL_NULL : s->asset_materials[ma];
        NphRenderCellDesc d;
        memset(&d, 0, sizeof d);
        d.center = nph_dvec3_add(nph_dvec3(((f64)c->bounds.min.x + (f64)c->bounds.max.x) * 0.5,
                                           ((f64)c->bounds.min.y + (f64)c->bounds.max.y) * 0.5,
                                           ((f64)c->bounds.min.z + (f64)c->bounds.max.z) * 0.5),
                                 desc->origin);
        d.extent = nph_vec3_scale(nph_vec3_sub(c->bounds.max, c->bounds.min), 0.5F);
        const bool apart = s->cell_level_count != NULL && s->cell_level_count[k] != 0U;
        d.proxy_levels = apart ? &s->level_meshes[s->cell_level_first[k]] : &s->meshes[mi];
        d.proxy_level_count = apart ? s->cell_level_count[k] : 1U;
        if (!apart && s->meshes[mi].value == 0U) {
            return NPH_RESULT_CORRUPT; // a head without its levels
        }
        d.proxy_materials = &material;
        d.proxy_material_count = 1U;
        d.proxy_transform = nph_render_transform_identity();
        d.proxy_transform.translation =
            nph_dvec3_add(nph_dvec3_from_vec3(c->proxy_origin), desc->origin);
        d.proxy_error = c->proxy_cell;
        d.distance = c->proxy_distance;
        // A streamed proxy's levels: their errors, which the views' requests
        // answer to, and its head's table for the stream.
        NphMeshView head;
        memset(&head, 0, sizeof head);
        f32 errors[NPH_RENDER_CELL_MAX_LEVELS];
        const bool streamed =
            s->stream != NULL && apart &&
            nph_mesh_view(nph_asset_get(s->loader, s->assets[mi]), &head) == NPH_RESULT_OK &&
            head.levels != NULL;
        if (streamed) {
            for (u32 i = 0; i < head.level_count; ++i) {
                errors[i] = head.levels[i].error;
            }
            d.proxy_level_errors = errors;
            d.proxy_level_error_count = head.level_count;
        }
        NPH_TRY(nph_render_cell_create(s->renderer, &d, &s->cells[k]));
        ++s->proxy_count;
        if (streamed) {
            NphProxyStreamCell pc;
            memset(&pc, 0, sizeof pc);
            pc.cell = s->cells[k];
            pc.coarsest = s->level_meshes[s->cell_level_first[k]];
            pc.levels = head.levels;
            pc.level_count = head.level_count;
            pc.center = d.center;
            pc.extent = d.extent;
            pc.distance = d.distance;
            NPH_TRY(nph_proxy_stream_add(s->stream, &pc));
        }
    }
    return NPH_RESULT_OK;
}

// The cell each instance lies in (its index in the scene's cell table), or
// NPH_U32_MAX: the cells' instance ranges, which the scene view checked.
static void instance_cells(const NphSceneView *sv, u32 *out)
{
    for (u32 i = 0; i < sv->header->instance_count; ++i) {
        out[i] = NPH_U32_MAX;
    }
    for (u32 k = 0; k < sv->cell_count; ++k) {
        const NphSceneCell *c = &sv->cells[k];
        for (u32 i = 0; i < c->instance_count; ++i) {
            out[c->instance_offset + i] = k;
        }
    }
}

static NphResult create_instances(CookedScene *s, const CookedSceneDesc *desc,
                                  const NphSceneView *sv, const NphMat4 *world, const u32 *cells)
{
    const NphSceneHeader *sh = sv->header;
    NphRenderMaterial table[NPH_RENDER_MAX_SUBMESHES];
    s->bounds = nph_aabb_empty();
    for (u32 i = 0; i < sh->instance_count; ++i) {
        const NphSceneMeshInstance *inst = &sv->instances[i];
        const u32 mi = find_asset(s, inst->mesh);
        if (mi == NPH_U32_MAX || inst->material_count > NPH_RENDER_MAX_SUBMESHES) {
            return NPH_RESULT_CORRUPT;
        }
        for (u32 m = 0; m < inst->material_count; ++m) {
            table[m] = s->materials[inst->material_offset + m];
        }
        NphRenderInstanceDesc d;
        memset(&d, 0, sizeof d);
        d.mesh = s->meshes[mi];
        d.materials = table;
        d.material_count = inst->material_count;
        d.transform = at_origin(&world[inst->node], desc->origin);
        d.cell =
            cells != NULL && cells[i] < s->cell_count ? s->cells[cells[i]] : NPH_RENDER_CELL_NULL;
        NPH_TRY(nph_render_instance_create(s->renderer, &d, &s->instances[s->instance_count]));
        ++s->instance_count;
        NphMeshView mv;
        NPH_TRY(nph_mesh_view(nph_asset_get(s->loader, s->assets[mi]), &mv));
        s->bounds =
            nph_aabb_union(s->bounds, nph_aabb_transform(mv.header->bounds,
                                                         nph_affine_from_mat4(world[inst->node])));
    }
    return NPH_RESULT_OK;
}

static NphResult create_lights(CookedScene *s, const CookedSceneDesc *desc, const NphSceneView *sv,
                               const NphMat4 *world)
{
    for (u32 i = 0; i < sv->header->light_count; ++i) {
        const NphSceneLight *l = &sv->lights[i];
        NphRenderLightDesc d;
        memset(&d, 0, sizeof d);
        d.type = l->type == NPH_SCENE_LIGHT_POINT  ? NPH_RENDER_LIGHT_TYPE_POINT
                 : l->type == NPH_SCENE_LIGHT_SPOT ? NPH_RENDER_LIGHT_TYPE_SPOT
                                                   : NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
        d.color = l->color;
        d.intensity = l->intensity;
        d.range = l->range;
        d.inner_cone = l->inner_cone;
        d.outer_cone = l->outer_cone;
        const NphMat4 *m = &world[l->node];
        d.position = nph_dvec3_add(nph_dvec3_from_vec3(nph_vec4_xyz(m->c[3])), desc->origin);
        // glTF lights shine along their node's -Z.
        d.direction =
            nph_vec3_normalize_or(nph_mat4_transform_vector(*m, nph_vec3(0.0F, 0.0F, -1.0F)),
                                  nph_vec3(0.0F, -1.0F, 0.0F));
        NPH_TRY(nph_render_light_create(s->renderer, &d, &s->lights[s->light_count]));
        s->light_descs[s->light_count] = d;
        ++s->light_count;
    }
    return NPH_RESULT_OK;
}

static void first_camera(CookedScene *s, const CookedSceneDesc *desc, const NphSceneView *sv,
                         const NphMat4 *world)
{
    for (u32 i = 0; i < sv->header->camera_count; ++i) {
        const NphSceneCamera *c = &sv->cameras[i];
        if (c->type != NPH_SCENE_CAMERA_PERSPECTIVE) {
            continue;
        }
        const NphMat4 *m = &world[c->node];
        const NphVec3 eye = nph_vec4_xyz(m->c[3]);
        const NphVec3 forward = nph_mat4_transform_vector(*m, nph_vec3(0.0F, 0.0F, -1.0F));
        const NphVec3 up = nph_mat4_transform_vector(*m, nph_vec3(0.0F, 1.0F, 0.0F));
        const NphDVec3 deye = nph_dvec3_add(nph_dvec3_from_vec3(eye), desc->origin);
        s->camera =
            nph_render_camera_look_at(deye, nph_dvec3_add(deye, nph_dvec3_from_vec3(forward)), up,
                                      c->yfov, c->znear > 0.0F ? c->znear : 0.05F);
        s->has_camera = 1;
        return;
    }
}

static NphResult load(CookedScene *s, const CookedSceneDesc *desc)
{
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    // The scene alone first: its header sizes the tables.
    NphAssetHandle scene_handle = NPH_ASSET_HANDLE_NULL;
    NPH_TRY(nph_asset_load_guid(s->loader, desc->scene, &scene_handle));
    NphSceneView sv;
    NphResult r = nph_scene_view(nph_asset_get(s->loader, scene_handle), &sv);
    const u32 cells = desc->no_cells || r != NPH_RESULT_OK ? 0U : sv.cell_count;
    if (r == NPH_RESULT_OK) {
        const NphSceneHeader *sh = sv.header;
        const u32 cap =
            1U + sh->instance_count + sh->material_count * (1U + NPH_RENDER_SLOT_COUNT) + cells;
        r = alloc_tables(s, cap);
    }
    if (r != NPH_RESULT_OK) {
        nph_asset_release(s->loader, scene_handle);
        return r;
    }
    s->guids[0] = desc->scene;
    s->assets[0] = scene_handle;
    s->asset_count = 1U;
    const NphSceneHeader *sh = sv.header;
    for (u32 i = 0; i < sh->instance_count; ++i) {
        NPH_TRY(request(s, sv.instances[i].mesh, &done));
    }
    for (u32 i = 0; i < sh->material_count; ++i) {
        NPH_TRY(request(s, sv.materials[i], &done));
    }
    for (u32 k = 0; k < cells; ++k) {
        if (!nph_guid_is_null(sv.cells[k].proxy)) {
            NPH_TRY(request(s, sv.cells[k].proxy, &done));
        }
    }
    NPH_TRY(wait_all(s, desc->jobs, &done));
    for (u32 i = 0; i < sh->material_count; ++i) {
        NphMaterialView mv;
        NPH_TRY(nph_material_view(asset_of(s, sv.materials[i]), &mv));
        for (u32 t = 0; t < NPH_MATERIAL_TEXTURE_COUNT; ++t) {
            if (!nph_guid_is_null(mv.header->textures[t].texture)) {
                NPH_TRY(request(s, mv.header->textures[t].texture, &done));
            }
        }
    }
    NPH_TRY(wait_all(s, desc->jobs, &done));

    // Render resources: textures and meshes, then the materials that name
    // the textures, then the instances and lights.
    for (u32 i = 0; i < s->asset_count; ++i) {
        const NphAsset *a = nph_asset_get(s->loader, s->assets[i]);
        const bool texture = a->header.type == NPH_ASSET_TYPE_TEXTURE;
        if (!texture && a->header.type != NPH_ASSET_TYPE_MESH) {
            continue;
        }
        r = texture ? create_texture(s, i) : create_mesh(s, i);
        if (r != NPH_RESULT_OK) {
            // Which one, and how far the load got: a capacity runs out at a
            // count, not at an asset.
            char guid[NPH_GUID_STRING_LEN + 1U];
            nph_guid_to_string(s->guids[i], guid);
            NPH_LOG_ERROR(&LOG_COOKED_SCENE, "%s %s (asset %u of %u): %s",
                          texture ? "texture" : "mesh", guid, i, s->asset_count,
                          nph_result_string(r));
            return r;
        }
    }
    const f32 emissive_scale = desc->emissive_scale > 0.0F ? desc->emissive_scale : 1.0F;
    s->material_cap = sh->material_count != 0U ? sh->material_count : 1U;
    s->instance_cap = sh->instance_count != 0U ? sh->instance_count : 1U;
    s->light_cap = sh->light_count != 0U ? sh->light_count : 1U;
    s->cell_cap = cells != 0U ? cells : 1U;
    s->materials = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderMaterial, s->material_cap);
    s->instances = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderInstance, s->instance_cap);
    s->cells = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderCell, s->cell_cap);
    s->lights = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderLight, s->light_cap);
    s->light_descs = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphRenderLightDesc, s->light_cap);
    NphMat4 *world = NPH_ALLOC_ARRAY(s->alloc, NphMat4, sh->node_count != 0U ? sh->node_count : 1U);
    u32 *instance_cell = cells != 0U ? NPH_ALLOC_ARRAY(s->alloc, u32, s->instance_cap) : NULL;
    if (s->materials == NULL || s->instances == NULL || s->cells == NULL || s->lights == NULL ||
        s->light_descs == NULL || world == NULL || (cells != 0U && instance_cell == NULL)) {
        if (world != NULL) {
            NPH_FREE_ARRAY(s->alloc, NphMat4, world, sh->node_count != 0U ? sh->node_count : 1U);
        }
        if (instance_cell != NULL) {
            NPH_FREE_ARRAY(s->alloc, u32, instance_cell, s->instance_cap);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    r = NPH_RESULT_OK;
    for (u32 i = 0; i < sh->material_count && r == NPH_RESULT_OK; ++i) {
        // The table names a material once per instance slot: one render
        // material per asset, which every slot naming it shares.
        const u32 a = find_asset(s, sv.materials[i]);
        NPH_ASSERT(a != NPH_U32_MAX); // requested above
        if (s->asset_materials[a].value == 0U) {
            r = create_material(s, nph_asset_get(s->loader, s->assets[a]), emissive_scale,
                                &s->asset_materials[a]);
            if (r == NPH_RESULT_OK) {
                ++s->render_material_count;
            }
        }
        if (r == NPH_RESULT_OK) {
            s->materials[i] = s->asset_materials[a];
            ++s->material_count;
        }
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_COOKED_SCENE, "material %u of %u (%u created): %s", s->material_count,
                      sh->material_count, s->render_material_count, nph_result_string(r));
    }
    if (r == NPH_RESULT_OK && cells != 0U) {
        r = load_levels(s, desc, &sv, cells);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_COOKED_SCENE, "the proxies' levels (%u loaded): %s",
                          s->level_mesh_count, nph_result_string(r));
        }
    }
    if (r == NPH_RESULT_OK && cells != 0U) {
        r = create_cells(s, desc, &sv);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_COOKED_SCENE, "cell %u of %u: %s", s->cell_count, cells,
                          nph_result_string(r));
        }
        instance_cells(&sv, instance_cell);
    }
    if (r == NPH_RESULT_OK) {
        world_matrices(&sv, world);
        r = create_instances(s, desc, &sv, world, instance_cell);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_COOKED_SCENE, "instance %u of %u: %s", s->instance_count,
                          sh->instance_count, nph_result_string(r));
        }
    }
    if (r == NPH_RESULT_OK) {
        r = create_lights(s, desc, &sv, world);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_COOKED_SCENE, "light %u of %u: %s", s->light_count, sh->light_count,
                          nph_result_string(r));
        }
    }
    if (r == NPH_RESULT_OK) {
        first_camera(s, desc, &sv, world);
    }
    NPH_FREE_ARRAY(s->alloc, NphMat4, world, sh->node_count != 0U ? sh->node_count : 1U);
    if (instance_cell != NULL) {
        NPH_FREE_ARRAY(s->alloc, u32, instance_cell, s->instance_cap);
    }
    return r;
}

NphResult cooked_scene_update(CookedScene *s, const NphRenderView *view, u32 height)
{
    NPH_ASSERT(s != NULL && view != NULL);
    if (s->stream == NULL) {
        return NPH_RESULT_OK;
    }
    const NphProxyStreamView v = nph_proxy_stream_view(view, height);
    return nph_proxy_stream_update(s->stream, &v);
}

NphResult cooked_scene_load(const CookedSceneDesc *desc, CookedScene *out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (desc->alloc == NULL || desc->renderer == NULL || desc->loader == NULL ||
        desc->jobs == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    out->alloc = desc->alloc;
    out->renderer = desc->renderer;
    out->loader = desc->loader;
    const NphResult r = load(out, desc);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_COOKED_SCENE, "loading the scene failed: %s", nph_result_string(r));
        cooked_scene_unload(out);
    }
    return r;
}

void cooked_scene_unload(CookedScene *s)
{
    if (s == NULL || s->alloc == NULL) {
        return;
    }
    NphRenderer *r = s->renderer;
    // The stream first: it gives the cells back their coarsest levels and
    // destroys the finer ones it made.
    nph_proxy_stream_destroy(s->stream);
    s->stream = NULL;
    if (s->instances != NULL) {
        for (u32 i = 0; i < s->instance_count; ++i) {
            nph_render_instance_destroy(r, s->instances[i]);
        }
    }
    if (s->cells != NULL) {
        for (u32 i = 0; i < s->cell_count; ++i) {
            nph_render_cell_destroy(r, s->cells[i]);
        }
    }
    if (s->lights != NULL) {
        for (u32 i = 0; i < s->light_count; ++i) {
            nph_render_light_destroy(r, s->lights[i]);
        }
    }
    NphAllocator *a = s->alloc;
    if (s->level_meshes != NULL) {
        for (u32 i = 0; i < s->level_mesh_count; ++i) {
            nph_render_mesh_destroy(r, s->level_meshes[i]);
        }
        NPH_FREE_ARRAY(a, NphRenderMesh, s->level_meshes, s->level_mesh_cap);
    }
    const u32 cells = nph_max_u32(s->cell_cap, 1U);
    if (s->cell_level_first != NULL) {
        NPH_FREE_ARRAY(a, u32, s->cell_level_first, cells);
    }
    if (s->cell_level_count != NULL) {
        NPH_FREE_ARRAY(a, u32, s->cell_level_count, cells);
    }
    for (u32 i = 0; i < s->asset_count; ++i) {
        if (s->asset_materials != NULL) {
            nph_render_material_destroy(r, s->asset_materials[i]);
        }
        if (s->meshes != NULL) {
            nph_render_mesh_destroy(r, s->meshes[i]);
        }
        if (s->textures != NULL) {
            nph_render_texture_destroy(r, s->textures[i]);
        }
        nph_asset_release(s->loader, s->assets[i]);
    }
    if (s->assets != NULL) {
        NPH_FREE_ARRAY(a, NphAssetHandle, s->assets, s->asset_cap);
    }
    if (s->guids != NULL) {
        NPH_FREE_ARRAY(a, NphGuid, s->guids, s->asset_cap);
    }
    if (s->textures != NULL) {
        NPH_FREE_ARRAY(a, NphRenderTexture, s->textures, s->asset_cap);
    }
    if (s->meshes != NULL) {
        NPH_FREE_ARRAY(a, NphRenderMesh, s->meshes, s->asset_cap);
    }
    if (s->asset_materials != NULL) {
        NPH_FREE_ARRAY(a, NphRenderMaterial, s->asset_materials, s->asset_cap);
    }
    if (s->materials != NULL) {
        NPH_FREE_ARRAY(a, NphRenderMaterial, s->materials, s->material_cap);
    }
    if (s->instances != NULL) {
        NPH_FREE_ARRAY(a, NphRenderInstance, s->instances, s->instance_cap);
    }
    if (s->cells != NULL) {
        NPH_FREE_ARRAY(a, NphRenderCell, s->cells, s->cell_cap);
    }
    if (s->lights != NULL) {
        NPH_FREE_ARRAY(a, NphRenderLight, s->lights, s->light_cap);
    }
    if (s->light_descs != NULL) {
        NPH_FREE_ARRAY(a, NphRenderLightDesc, s->light_descs, s->light_cap);
    }
    memset(s, 0, sizeof *s);
}
