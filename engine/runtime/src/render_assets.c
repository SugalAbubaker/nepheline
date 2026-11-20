// SPDX-License-Identifier: Apache-2.0
// The render asset cache: cooked meshes, materials, and textures by GUID,
// loaded asynchronously and turned into render resources when ready. See
// runtime_internal.h. The conversions from the cooked formats are the ones
// samples/common/cooked_scene.c introduced in Phase 7.
#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/texture.h>
#include <nepheline/core/assert.h>

#include "runtime_internal.h"

#include <stddef.h>
#include <string.h>

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
NPH_STATIC_ASSERT(sizeof(NphMeshRayRange) == sizeof(NphRenderRaySubmesh) &&
                      offsetof(NphMeshRayRange, alpha_key) ==
                          offsetof(NphRenderRaySubmesh, alpha_key) &&
                      sizeof(NphMeshRayTriangle) == sizeof(NphRenderRayTriangle) &&
                      offsetof(NphMeshRayTriangle, corner) ==
                          offsetof(NphRenderRayTriangle, corner),
                  "cooked triangles for the rays are the renderer's records");
NPH_STATIC_ASSERT((u32)NPH_MATERIAL_WRAP_MIRROR == (u32)NPH_RENDER_WRAP_MIRROR,
                  "the wrap modes share their numbering");
NPH_STATIC_ASSERT((u32)NPH_MATERIAL_ALPHA_BLEND == (u32)NPH_RENDER_ALPHA_BLEND,
                  "the alpha modes share their numbering");
NPH_STATIC_ASSERT((u32)NPH_MATERIAL_TEXTURE_EMISSIVE == (u32)NPH_RENDER_SLOT_EMISSIVE,
                  "the texture slots share their numbering");
NPH_STATIC_ASSERT(RENDER_MATERIAL_TEXTURES == (u32)NPH_MATERIAL_TEXTURE_COUNT,
                  "a material names five textures");

static const char *kind_name(u32 kind)
{
    switch (kind) {
    case RENDER_ASSET_MESH:
        return "mesh";
    case RENDER_ASSET_MATERIAL:
        return "material";
    default:
        return "texture";
    }
}

static void log_failure(const RenderAsset *a, const char *what, NphResult r)
{
    char guid[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(a->guid, guid);
    NPH_LOG_WARN(&NPH_LOG_RUNTIME, "%s %s: %s (%s)", kind_name(a->kind), guid, what,
                 nph_result_string(r));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult render_assets_init(RenderAssetCache *c, NphAllocator *alloc, NphRenderer *renderer,
                             NphAssetLoader *loader, u32 cap, f32 emissive_scale)
{
    memset(c, 0, sizeof *c);
    c->alloc = alloc;
    c->renderer = renderer;
    c->loader = loader;
    c->emissive_scale = emissive_scale > 0.0F ? emissive_scale : 1.0F;
    c->cap = cap;
    c->free_head = RENDER_ASSET_NONE;
    c->items = NPH_ALLOC_ARRAY_ZEROED(alloc, RenderAsset, cap);
    if (c->items == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphHashMapDesc d;
    memset(&d, 0, sizeof d);
    d.key_size = (u32)sizeof(NphGuid);
    d.val_size = (u32)sizeof(u32);
    const NphResult r = nph_hashmap_init(&c->by_guid, alloc, &d);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_ARRAY(alloc, RenderAsset, c->items, cap);
        c->items = NULL;
    }
    return r;
}

static void destroy_resource(RenderAssetCache *c, RenderAsset *a)
{
    nph_render_mesh_destroy(c->renderer, a->mesh);
    nph_render_material_destroy(c->renderer, a->material);
    if (c->textures != NULL && a->texture.value != 0U) {
        nph_texture_stream_remove(c->textures, a->texture);
    }
    nph_render_texture_destroy(c->renderer, a->texture);
    a->mesh = NPH_RENDER_MESH_NULL;
    a->material = NPH_RENDER_MATERIAL_NULL;
    a->texture = NPH_RENDER_TEXTURE_NULL;
}

void render_assets_free(RenderAssetCache *c)
{
    if (c->items == NULL) {
        return;
    }
    (void)render_assets_wait(c);
    // Materials before the textures they name.
    for (u32 pass = 0; pass < 2U; ++pass) {
        for (u32 i = 0; i < c->high; ++i) {
            RenderAsset *a = &c->items[i];
            const bool material = a->kind == RENDER_ASSET_MATERIAL;
            if (a->state == RENDER_ASSET_FREE || (pass == 0U) != material) {
                continue;
            }
            destroy_resource(c, a);
            nph_asset_release(c->loader, a->handle);
            if (a->refresh != (u32)RENDER_REFRESH_NONE) {
                nph_asset_release(c->loader, a->next); // its new textures go in the next pass
                a->refresh = (u32)RENDER_REFRESH_NONE;
            }
            a->state = RENDER_ASSET_FREE;
        }
    }
    nph_hashmap_free(&c->by_guid);
    NPH_FREE_ARRAY(c->alloc, RenderAsset, c->items, c->cap);
    c->items = NULL;
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

u32 render_assets_acquire(RenderAssetCache *c, NphGuid guid, RenderAssetKind kind)
{
    if (nph_guid_is_null(guid)) {
        return RENDER_ASSET_NONE;
    }
    const u32 *found = NPH_HASHMAP_FIND(&c->by_guid, u32, &guid);
    if (found != NULL) {
        RenderAsset *a = &c->items[*found];
        if (a->kind != (u32)kind) {
            char text[NPH_GUID_STRING_LEN + 1U];
            nph_guid_to_string(guid, text);
            NPH_LOG_WARN(&NPH_LOG_RUNTIME, "asset %s is used as a %s and as a %s", text,
                         kind_name(a->kind), kind_name((u32)kind));
            return RENDER_ASSET_NONE;
        }
        a->refs += 1U;
        return *found;
    }
    u32 index = c->free_head;
    if (index != RENDER_ASSET_NONE) {
        c->free_head = c->items[index].next_free;
    } else if (c->high < c->cap) {
        index = c->high++;
    } else {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the render asset cache is full (%u assets)", c->cap);
        return RENDER_ASSET_NONE;
    }
    if (nph_hashmap_insert(&c->by_guid, &guid, &index) != NPH_RESULT_OK) {
        c->items[index].next_free = c->free_head;
        c->free_head = index;
        return RENDER_ASSET_NONE;
    }
    RenderAsset *a = &c->items[index];
    memset(a, 0, sizeof *a);
    a->guid = guid;
    a->kind = (u32)kind;
    a->refs = 1U;
    a->state = RENDER_ASSET_LOADING;
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        a->textures[t] = RENDER_ASSET_NONE;
    }
    const NphResult r = nph_asset_load_guid_async(c->loader, guid, NULL, &a->handle);
    if (r != NPH_RESULT_OK) {
        log_failure(a, "cannot load", r);
        a->state = RENDER_ASSET_FAILED;
    }
    return index;
}

static void cancel_refresh(RenderAssetCache *c, RenderAsset *a);

void render_assets_release(RenderAssetCache *c, u32 index)
{
    if (index == RENDER_ASSET_NONE || index >= c->high) {
        return;
    }
    RenderAsset *a = &c->items[index];
    NPH_ASSERT(a->state != RENDER_ASSET_FREE && a->refs > 0U);
    if (--a->refs != 0U) {
        return;
    }
    cancel_refresh(c, a);
    destroy_resource(c, a);
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        render_assets_release(c, a->textures[t]);
    }
    nph_asset_release(c->loader, a->handle); // during a load: unloads when it lands
    if (!a->forgotten) {
        NPH_VERIFY(nph_hashmap_remove(&c->by_guid, &a->guid));
    }
    a->forgotten = 0;
    a->state = RENDER_ASSET_FREE;
    a->next_free = c->free_head;
    c->free_head = index;
}

u32 render_assets_find(const RenderAssetCache *c, NphGuid guid)
{
    const u32 *found = NPH_HASHMAP_FIND(&c->by_guid, u32, &guid);
    return found != NULL ? *found : RENDER_ASSET_NONE;
}

void render_assets_forget(RenderAssetCache *c, u32 index)
{
    if (index == RENDER_ASSET_NONE || index >= c->high) {
        return;
    }
    RenderAsset *a = &c->items[index];
    if (a->state == RENDER_ASSET_FREE || a->forgotten) {
        return;
    }
    NPH_VERIFY(nph_hashmap_remove(&c->by_guid, &a->guid));
    a->forgotten = 1;
    nph_asset_forget(c->loader, a->handle);
}

// ---------------------------------------------------------------------------
// Render resources from cooked assets
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

static NphResult create_texture(RenderAssetCache *c, RenderAsset *a, const NphAsset *asset)
{
    NphTextureView tv;
    NPH_TRY(nph_texture_view(asset, &tv));
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
    if (c->textures != NULL) {
        d.flags |= (u32)NPH_RENDER_TEXTURE_FLAG_PAGED;
    }
    const NphResult r = nph_render_texture_create(c->renderer, &d, &a->texture);
    if (r == NPH_RESULT_UNSUPPORTED) {
        // A device without BC textures draws the materials untextured.
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "%s textures are not supported here; drawing untextured",
                     nph_texture_format_info(h->format)->name);
        a->texture = NPH_RENDER_TEXTURE_NULL;
        return NPH_RESULT_OK;
    }
    NPH_TRY(r);
    if (c->textures == NULL) {
        return NPH_RESULT_OK;
    }
    // Paged: its levels come from the stream, the texels from the asset, which
    // the entry holds for as long as the texture lives.
    NphRenderTexturePages pages;
    NPH_TRY(nph_render_texture_pages(c->renderer, a->texture, &pages));
    if (!pages.paged) {
        return NPH_RESULT_OK; // too small to have pages, say: resident whole
    }
    NphTextureStreamTexture st;
    memset(&st, 0, sizeof st);
    st.texture = a->texture;
    st.mips = tv.mips;
    return nph_texture_stream_add(c->textures, &st);
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

NphResult render_assets_mesh_desc(const NphMeshView *mv, NphRenderSubmesh *subs,
                                  NphRenderMeshDesc *out)
{
    const NphMeshHeader *h = mv->header;
    // A head of levels apart (a cell's proxy, asset/mesh.h version 6) is no
    // mesh to draw: its levels are (proxy_stream.c).
    if (h->submesh_count > NPH_RENDER_MAX_SUBMESHES || mv->levels != NULL) {
        return NPH_RESULT_UNSUPPORTED;
    }
    for (u32 i = 0; i < h->submesh_count; ++i) {
        subs[i].index_offset = mv->submeshes[i].index_offset;
        subs[i].index_count = mv->submeshes[i].index_count;
        subs[i].material_slot = mv->submeshes[i].material_slot;
    }
    NphRenderMeshDesc *d = out;
    memset(d, 0, sizeof *d);
    d->vertex_count = h->vertex_count;
    d->index_count = h->index_count;
    // The same layout (asserted above): the cooked streams are handed over
    // as they are, through untyped pointers.
    const void *positions = mv->positions;
    const void *attribs = mv->attribs;
    d->positions = positions;
    d->attribs = attribs;
    d->uv1 = mv->uv1; // the second set where a slot reads one (ADR-0107), else NULL
    d->indices = mv->indices;
    d->submeshes = subs;
    d->submesh_count = h->submesh_count;
    d->position_offset = h->position_offset;
    d->position_scale = h->position_scale;
    d->bounds = h->bounds;
    d->no_rays = (h->flags & (u32)NPH_MESH_FLAG_NO_RAYS) != 0U ? 1 : 0;
    cooked_clusters(mv, d);
    // Its triangles for the rays alone, where it has them (asset/mesh.h version 7; ADR-0096),
    // handed over as they are (the same records, asserted above): whatever materials its
    // instances draw it with, the renderer's rays take them only on frames those hold them,
    // and the mesh's own triangles otherwise (ADR-0098). A mesh of voxels alone has no
    // triangles for a ray's to be part of.
    if (mv->ray_ranges != NULL && h->index_count != 0U && !d->no_rays) {
        const void *runs = mv->ray_ranges;
        const void *triangles = mv->ray_triangles;
        d->ray_submeshes = runs;
        d->ray_triangles = triangles;
        d->ray_triangle_count = mv->ray_triangle_count;
    }
    d->name = "cooked mesh";
    return NPH_RESULT_OK;
}

static NphResult create_mesh(RenderAssetCache *c, RenderAsset *a, const NphAsset *asset)
{
    NphMeshView mv;
    NPH_TRY(nph_mesh_view(asset, &mv));
    NphRenderSubmesh subs[NPH_RENDER_MAX_SUBMESHES];
    NphRenderMeshDesc d;
    NPH_TRY(render_assets_mesh_desc(&mv, subs, &d));
    NPH_TRY(nph_render_mesh_create(c->renderer, &d, &a->mesh));
    // The materials its slots name, for the entities drawing it (the view checked the count).
    if (mv.slot_material_count != 0U) {
        memcpy(a->slot_materials, mv.slot_materials, sizeof(NphGuid) * mv.slot_material_count);
    }
    a->slot_material_count = mv.slot_material_count;
    return NPH_RESULT_OK;
}

// A material's textures load first; it is created once they settle.
static NphResult start_material(RenderAssetCache *c, u32 index, const NphAsset *asset)
{
    NphMaterialView mv;
    NPH_TRY(nph_material_view(asset, &mv));
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        const u32 tex =
            render_assets_acquire(c, mv.header->textures[t].texture, RENDER_ASSET_TEXTURE);
        c->items[index].textures[t] = tex; // `items` is fixed: acquiring never moves it
    }
    c->items[index].state = RENDER_ASSET_WAITING;
    return NPH_RESULT_OK;
}

// A cooked material's coordinates' transform as the renderer takes it (ADR-0105).
static NphRenderUvTransform render_uv(const NphMaterialUvTransform *t)
{
    const NphRenderUvTransform out = {t->m00, t->m01, t->m02, t->m10, t->m11, t->m12};
    return out;
}

// The render description of header `h`, transform `uv` (NULL: the identity) and optics
// `optics` (NULL: the default) with the textures `textures` (cache indices) hold.
static void material_desc(const RenderAssetCache *c, const NphMaterialHeader *h,
                          const NphMaterialUvTransform *uv, const NphMaterialOptics *optics,
                          const u32 textures[RENDER_MATERIAL_TEXTURES], NphRenderMaterialDesc *out)
{
    NphRenderMaterialDesc d;
    nph_render_material_desc_default(&d);
    d.base_color = h->base_color_factor;
    d.emissive = nph_vec3_scale(h->emissive_factor, h->emissive_strength * c->emissive_scale);
    d.metallic = h->metallic_factor;
    d.roughness = h->roughness_factor;
    d.alpha_cutoff = h->alpha_cutoff;
    d.alpha_mode = (NphRenderAlphaMode)h->alpha_mode;
    d.double_sided = (h->flags & (u32)NPH_MATERIAL_FLAG_DOUBLE_SIDED) != 0U;
    d.unlit = (h->flags & (u32)NPH_MATERIAL_FLAG_UNLIT) != 0U;
    d.transmissive = (h->flags & (u32)NPH_MATERIAL_FLAG_TRANSMISSIVE) != 0U;
    d.transmission_texels =
        d.transmissive && (h->flags & (u32)NPH_MATERIAL_FLAG_TRANSMISSION_TEXELS) != 0U;
    d.normal_scale = h->textures[NPH_MATERIAL_TEXTURE_NORMAL].scale;
    d.occlusion_strength = h->textures[NPH_MATERIAL_TEXTURE_OCCLUSION].scale;
    // F0 and glass's tint (ADR-0107).
    if (optics != NULL) {
        d.f0 = optics->f0;
        d.transmission = d.transmissive ? optics->transmission : nph_vec3_zero();
    }
    for (u32 t = 0; t < (u32)NPH_RENDER_SLOT_COUNT; ++t) {
        const NphMaterialTextureRef *ref = &h->textures[t];
        const RenderAsset *tex = render_asset(c, textures[t]);
        d.textures[t].texture = tex != NULL && tex->state == RENDER_ASSET_READY
                                    ? tex->texture
                                    : NPH_RENDER_TEXTURE_NULL;
        d.textures[t].wrap_u = (NphRenderWrap)ref->wrap_u;
        d.textures[t].wrap_v = (NphRenderWrap)ref->wrap_v;
        d.textures[t].nearest = ref->nearest != 0U;
        d.textures[t].texcoord = ref->texcoord;
    }
    // The cooker's key of its alpha test (ADR-0096), where it reads the texture the key was
    // made of: an alpha-tested material whose base colour texture this device took. The
    // renderer holds its instances' rays to it on the frames they read that texture's finest
    // level, paged or whole (ADR-0098).
    // The cooker makes keys of the first set alone (cook/ray_trim.h).
    if (h->alpha_mode == (u32)NPH_MATERIAL_ALPHA_MASK &&
        d.textures[NPH_RENDER_SLOT_BASE_COLOR].texture.value != 0U &&
        d.textures[NPH_RENDER_SLOT_BASE_COLOR].texcoord == 0U) {
        d.alpha_key = h->alpha_key;
    }
    // The key is of the test the mesh's own coordinates read: a transformed material has none.
    if (uv != NULL) {
        d.uv = render_uv(uv);
        if (!nph_render_uv_is_identity(&d.uv)) {
            d.alpha_key = 0U;
        }
    }
    d.name = "cooked material";
    *out = d;
}

static NphResult create_material(RenderAssetCache *c, RenderAsset *a)
{
    NphMaterialView mv;
    NPH_TRY(nph_material_view(nph_asset_get(c->loader, a->handle), &mv));
    NphRenderMaterialDesc d;
    material_desc(c, mv.header, mv.uv, mv.optics, a->textures, &d);
    return nph_render_material_create(c->renderer, &d, &a->material);
}

// ---------------------------------------------------------------------------
// Materials loaded again in place
// ---------------------------------------------------------------------------

static void cancel_refresh(RenderAssetCache *c, RenderAsset *a)
{
    if (a->refresh == (u32)RENDER_REFRESH_NONE) {
        return;
    }
    a->refresh = (u32)RENDER_REFRESH_NONE;
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        const u32 tex = a->next_textures[t];
        a->next_textures[t] = RENDER_ASSET_NONE;
        render_assets_release(c, tex);
    }
    nph_asset_release(c->loader, a->next);
    a->next = NPH_ASSET_HANDLE_NULL;
}

bool render_assets_refresh(RenderAssetCache *c, u32 index)
{
    if (index == RENDER_ASSET_NONE || index >= c->high) {
        return false;
    }
    RenderAsset *a = &c->items[index];
    if (a->kind != (u32)RENDER_ASSET_MATERIAL || a->state != (u32)RENDER_ASSET_READY ||
        a->forgotten != 0) {
        return false;
    }
    // One in flight read the file as it was: this one reads it as it is.
    cancel_refresh(c, a);
    nph_asset_forget(c->loader, a->handle);
    NphAssetHandle next = NPH_ASSET_HANDLE_NULL;
    const NphResult r = nph_asset_load_guid_async(c->loader, a->guid, NULL, &next);
    if (r != NPH_RESULT_OK) {
        log_failure(a, "could not be loaded again", r);
        return false;
    }
    a->next = next;
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        a->next_textures[t] = RENDER_ASSET_NONE;
    }
    a->refresh = (u32)RENDER_REFRESH_LOADING;
    return true;
}

NphResult render_assets_material_set(RenderAssetCache *c, u32 index, const NphMaterialHeader *h,
                                     const NphMaterialUvTransform *uv)
{
    NPH_ASSERT(h != NULL);
    const RenderAsset *a = render_asset(c, index);
    if (a == NULL || a->kind != (u32)RENDER_ASSET_MATERIAL || a->state != (u32)RENDER_ASSET_READY) {
        return NPH_RESULT_INVALID_STATE;
    }
    // The editor's edits carry no optics (its materials have the default).
    NphRenderMaterialDesc d;
    material_desc(c, h, uv, NULL, a->textures, &d);
    return nph_render_material_update(c->renderer, a->material, &d);
}

static bool content_error(NphResult r);

// Advances the refresh of entry `index`: its new asset to its textures, its textures to the
// update. The first failure that is not a content error, in `*first`.
static void advance_refresh(RenderAssetCache *c, u32 index, NphResult *first)
{
    RenderAsset *a = &c->items[index];
    NphMaterialView mv;
    memset(&mv, 0, sizeof mv);
    if (a->refresh == (u32)RENDER_REFRESH_LOADING) {
        const NphAssetState st = nph_asset_state(c->loader, a->next);
        if (st == NPH_ASSET_STATE_LOADING) {
            return;
        }
        const NphResult r = st == NPH_ASSET_STATE_READY
                                ? nph_material_view(nph_asset_get(c->loader, a->next), &mv)
                                : nph_asset_error(c->loader, a->next);
        if (r != NPH_RESULT_OK) {
            log_failure(a, "failed to load again; the version it had stays", r);
            cancel_refresh(c, a);
            return;
        }
        for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
            const u32 tex =
                render_assets_acquire(c, mv.header->textures[t].texture, RENDER_ASSET_TEXTURE);
            c->items[index].next_textures[t] = tex; // `items` is fixed: acquiring never moves it
        }
        a = &c->items[index];
        a->refresh = (u32)RENDER_REFRESH_WAITING;
    }
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        if (!render_asset_settled(c, a->next_textures[t])) {
            return;
        }
    }
    NPH_VERIFY(nph_material_view(nph_asset_get(c->loader, a->next), &mv) == NPH_RESULT_OK);
    NphRenderMaterialDesc d;
    material_desc(c, mv.header, mv.uv, mv.optics, a->next_textures, &d);
    const NphResult r = nph_render_material_update(c->renderer, a->material, &d);
    if (r != NPH_RESULT_OK) {
        log_failure(a, "is not usable again; the version it had stays", r);
        cancel_refresh(c, a);
        if (!content_error(r) && *first == NPH_RESULT_OK) {
            *first = r;
        }
        return;
    }
    // The new version is the entry's: the old asset and textures go.
    const NphAssetHandle old = a->handle;
    u32 old_textures[RENDER_MATERIAL_TEXTURES];
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        old_textures[t] = a->textures[t];
        a->textures[t] = a->next_textures[t];
        a->next_textures[t] = RENDER_ASSET_NONE;
    }
    a->handle = a->next;
    a->next = NPH_ASSET_HANDLE_NULL;
    a->refresh = (u32)RENDER_REFRESH_NONE;
    for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
        render_assets_release(c, old_textures[t]);
    }
    nph_asset_release(c->loader, old);
}

// Errors that say the content is bad (the asset fails, users go on) as
// opposed to the engine running out of something.
static bool content_error(NphResult r)
{
    return r != NPH_RESULT_FULL && r != NPH_RESULT_OUT_OF_MEMORY;
}

NphResult render_assets_poll(RenderAssetCache *c)
{
    NphResult first = NPH_RESULT_OK;
    for (u32 i = 0; i < c->high; ++i) {
        RenderAsset *a = &c->items[i];
        if (a->state != RENDER_ASSET_LOADING) {
            continue;
        }
        const NphAssetState st = nph_asset_state(c->loader, a->handle);
        if (st == NPH_ASSET_STATE_LOADING) {
            continue;
        }
        if (st != NPH_ASSET_STATE_READY) {
            log_failure(a, "failed to load", nph_asset_error(c->loader, a->handle));
            a->state = RENDER_ASSET_FAILED;
            continue;
        }
        const NphAsset *asset = nph_asset_get(c->loader, a->handle);
        NphResult r;
        if (a->kind == RENDER_ASSET_MATERIAL) {
            r = start_material(c, i, asset);
            a = &c->items[i];
        } else {
            r = a->kind == RENDER_ASSET_MESH ? create_mesh(c, a, asset)
                                             : create_texture(c, a, asset);
            if (r == NPH_RESULT_OK) {
                a->state = RENDER_ASSET_READY;
            }
        }
        if (r != NPH_RESULT_OK) {
            log_failure(a, "is not usable", r);
            a->state = RENDER_ASSET_FAILED;
            if (!content_error(r) && first == NPH_RESULT_OK) {
                first = r;
            }
        }
    }
    // Materials whose textures have all settled.
    for (u32 i = 0; i < c->high; ++i) {
        RenderAsset *a = &c->items[i];
        if (a->state != RENDER_ASSET_WAITING) {
            continue;
        }
        bool settled = true;
        for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES && settled; ++t) {
            settled = render_asset_settled(c, a->textures[t]);
        }
        if (!settled) {
            continue;
        }
        const NphResult r = create_material(c, a);
        if (r != NPH_RESULT_OK) {
            log_failure(a, "is not usable", r);
            a->state = RENDER_ASSET_FAILED;
            if (!content_error(r) && first == NPH_RESULT_OK) {
                first = r;
            }
        } else {
            a->state = RENDER_ASSET_READY;
        }
    }
    // Materials loading again in place.
    for (u32 i = 0; i < c->high; ++i) {
        if (c->items[i].state == RENDER_ASSET_READY &&
            c->items[i].refresh != (u32)RENDER_REFRESH_NONE) {
            advance_refresh(c, i, &first);
        }
    }
    return first;
}

NphResult render_assets_wait(RenderAssetCache *c)
{
    NphResult first = NPH_RESULT_OK;
    for (;;) {
        bool waited = false;
        for (u32 i = 0; i < c->high; ++i) {
            if (c->items[i].state == RENDER_ASSET_LOADING) {
                (void)nph_asset_wait(c->loader, c->items[i].handle);
                waited = true;
            }
            if (c->items[i].refresh != (u32)RENDER_REFRESH_NONE) {
                if (c->items[i].refresh == (u32)RENDER_REFRESH_LOADING) {
                    (void)nph_asset_wait(c->loader, c->items[i].next);
                }
                waited = true; // its new textures load from the poll below
            }
        }
        const NphResult r = render_assets_poll(c);
        if (first == NPH_RESULT_OK) {
            first = r;
        }
        if (!waited) {
            return first;
        }
    }
}

RenderAssetCounts render_assets_counts(const RenderAssetCache *c)
{
    RenderAssetCounts n = {0U, 0U, 0U};
    for (u32 i = 0; i < c->high; ++i) {
        switch ((RenderAssetState)c->items[i].state) {
        case RENDER_ASSET_LOADING:
        case RENDER_ASSET_WAITING:
            n.loading += 1U;
            break;
        case RENDER_ASSET_READY:
            n.ready += 1U;
            break;
        case RENDER_ASSET_FAILED:
            n.failed += 1U;
            break;
        case RENDER_ASSET_FREE:
            break;
        }
    }
    return n;
}
