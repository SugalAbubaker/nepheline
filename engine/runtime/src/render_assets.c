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

#include <string.h>

// The cooked streams are the renderer's streams, bit for bit.
NPH_STATIC_ASSERT(sizeof(NphMeshPosition) == sizeof(NphRenderVertexPosition),
                  "cooked positions are render positions");
NPH_STATIC_ASSERT(sizeof(NphMeshAttrib) == sizeof(NphRenderVertexAttrib),
                  "cooked attributes are render attributes");
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
    const NphResult r = nph_render_texture_create(c->renderer, &d, &a->texture);
    if (r == NPH_RESULT_UNSUPPORTED) {
        // A device without BC textures draws the materials untextured.
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "%s textures are not supported here; drawing untextured",
                     nph_texture_format_info(h->format)->name);
        a->texture = NPH_RENDER_TEXTURE_NULL;
        return NPH_RESULT_OK;
    }
    return r;
}

static NphResult create_mesh(RenderAssetCache *c, RenderAsset *a, const NphAsset *asset)
{
    NphMeshView mv;
    NPH_TRY(nph_mesh_view(asset, &mv));
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
    // The same layout (asserted above): the cooked streams are handed over
    // as they are, through untyped pointers.
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
    d.name = "cooked mesh";
    return nph_render_mesh_create(c->renderer, &d, &a->mesh);
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

static NphResult create_material(RenderAssetCache *c, RenderAsset *a)
{
    NphMaterialView mv;
    NPH_TRY(nph_material_view(nph_asset_get(c->loader, a->handle), &mv));
    const NphMaterialHeader *h = mv.header;
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
    d.normal_scale = h->textures[NPH_MATERIAL_TEXTURE_NORMAL].scale;
    d.occlusion_strength = h->textures[NPH_MATERIAL_TEXTURE_OCCLUSION].scale;
    for (u32 t = 0; t < (u32)NPH_RENDER_SLOT_COUNT; ++t) {
        const NphMaterialTextureRef *ref = &h->textures[t];
        const RenderAsset *tex = render_asset(c, a->textures[t]);
        d.textures[t].texture = tex != NULL && tex->state == RENDER_ASSET_READY
                                    ? tex->texture
                                    : NPH_RENDER_TEXTURE_NULL;
        d.textures[t].wrap_u = (NphRenderWrap)ref->wrap_u;
        d.textures[t].wrap_v = (NphRenderWrap)ref->wrap_v;
        d.textures[t].nearest = ref->nearest != 0U;
    }
    d.name = "cooked material";
    return nph_render_material_create(c->renderer, &d, &a->material);
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
