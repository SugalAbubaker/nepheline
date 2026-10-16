// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal pieces of the runtime module shared between its source files.

#include <nepheline/asset/loader.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/render/renderer.h>

NPH_LOG_CATEGORY_DECLARE(NPH_LOG_RUNTIME);

#define RENDER_ASSET_NONE NPH_U32_MAX

// ---------------------------------------------------------------------------
// The render asset cache (render_assets.c): cooked meshes, materials, and
// textures by GUID, each loaded once, turned into a render resource when
// ready, and destroyed when its last user releases it.
// ---------------------------------------------------------------------------

typedef enum RenderAssetKind {
    RENDER_ASSET_MESH = 0,
    RENDER_ASSET_MATERIAL,
    RENDER_ASSET_TEXTURE,
} RenderAssetKind;

typedef enum RenderAssetState {
    RENDER_ASSET_FREE = 0,
    RENDER_ASSET_LOADING, // the asset is loading
    RENDER_ASSET_WAITING, // a material whose textures are loading
    RENDER_ASSET_READY,   // the render resource exists
    RENDER_ASSET_FAILED,  // logged once; users draw without it
} RenderAssetState;

#define RENDER_MATERIAL_TEXTURES 5U

typedef struct RenderAsset {
    NphGuid guid;
    u32 kind;  // RenderAssetKind
    u32 state; // RenderAssetState
    u32 refs;
    u32 next_free;
    b32 forgotten; // out of by_guid: its users acquire the GUID anew (render_assets_forget)
    NphAssetHandle handle;
    NphRenderMesh mesh;
    NphRenderMaterial material;
    NphRenderTexture texture;
    u32 textures[RENDER_MATERIAL_TEXTURES]; // a material's textures (cache indices)
} RenderAsset;

typedef struct RenderAssetCache {
    NphAllocator *alloc;
    NphRenderer *renderer;
    NphAssetLoader *loader;
    f32 emissive_scale;
    RenderAsset *items;
    u32 cap;
    u32 high;           // slots ever used
    u32 free_head;      // RENDER_ASSET_NONE when empty
    NphHashMap by_guid; // NphGuid -> u32 index
} RenderAssetCache;

NphResult render_assets_init(RenderAssetCache *c, NphAllocator *alloc, NphRenderer *renderer,
                             NphAssetLoader *loader, u32 cap, f32 emissive_scale);
// Waits for the loads in flight, then destroys everything (live users
// included: the caller has destroyed its instances first).
void render_assets_free(RenderAssetCache *c);

// A reference to the asset with `guid` of `kind`: an existing entry, or a
// new one whose load starts now. RENDER_ASSET_NONE for a null GUID; an
// entry in the FAILED state for a load that could not even start (a full
// cache returns NONE and logs).
u32 render_assets_acquire(RenderAssetCache *c, NphGuid guid, RenderAssetKind kind);
void render_assets_release(RenderAssetCache *c, u32 index);

// The entry of `guid`, or RENDER_ASSET_NONE.
u32 render_assets_find(const RenderAssetCache *c, NphGuid guid);

// Forgets the entry as the one of its GUID, and its asset as the one at its
// path (asset/loader.h nph_asset_forget): the next acquire of the GUID
// loads it again into a new entry, while this one stays for its holders.
void render_assets_forget(RenderAssetCache *c, u32 index);

// Advances loads: LOADING to READY or FAILED, materials WAITING for their
// textures to READY. Returns the first resource-creation failure that is
// not a content error (FULL, OUT_OF_MEMORY).
NphResult render_assets_poll(RenderAssetCache *c);

// Blocks until no load is in flight (then polls).
NphResult render_assets_wait(RenderAssetCache *c);

NPH_INLINE const RenderAsset *render_asset(const RenderAssetCache *c, u32 index)
{
    return index < c->high ? &c->items[index] : NULL;
}

// Ready or failed: nothing more will happen to it.
NPH_INLINE bool render_asset_settled(const RenderAssetCache *c, u32 index)
{
    const RenderAsset *a = render_asset(c, index);
    return a == NULL || a->state == RENDER_ASSET_READY || a->state == RENDER_ASSET_FAILED;
}

typedef struct RenderAssetCounts {
    u32 loading;
    u32 ready;
    u32 failed;
} RenderAssetCounts;

RenderAssetCounts render_assets_counts(const RenderAssetCache *c);
