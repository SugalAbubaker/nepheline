// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal pieces of the runtime module shared between its source files.

#include <nepheline/asset/loader.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/gpu/types.h>
#include <nepheline/render/renderer.h>
#include <nepheline/runtime/texture_stream.h>

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

// A mesh asset's view as the renderer's description: its streams handed
// over as they are (the same records), its submeshes into `subs`
// (NPH_RENDER_MAX_SUBMESHES of them). NPH_RESULT_UNSUPPORTED for more
// submeshes than the renderer takes, or a head of levels apart (no mesh of
// its own). The description points into the view's asset.
NphResult render_assets_mesh_desc(const NphMeshView *mv, NphRenderSubmesh *subs,
                                  NphRenderMeshDesc *out);

// ---------------------------------------------------------------------------
// Proxy streaming's decisions (proxy_stream.c), pure, tested apart
// ---------------------------------------------------------------------------

// What a cell's errors, world units, are over a view's threshold `lod_error`
// (pixels) seen from the camera `near` from its box, as the requests see
// them: from a quarter nearer, and no nearer than `znear`; a level's error
// times this past 1 is too coarse there. 0 in a view without levels of
// detail.
f32 proxy_stream_scale(f32 near, f32 pixels_per_unit, f32 lod_error, f32 znear);

// The level a cell of `level_count` levels (their largest consumer errors
// `errors`, finest first) is wanted at: `asked` where the views asked
// (NPH_RENDER_CELL_NO_REQUEST: not), else past `distance` less a quarter
// (the camera `near` from its box) one level coarser than its cut would use
// there (the requests' test, at `scale`: proxy_stream_scale), else, or in a
// view without levels of detail (`scale` 0), its coarsest.
u32 proxy_stream_wanted(u32 asked, const f32 *errors, u32 level_count, f32 near, f32 distance,
                        f32 scale);

// What a cell's level is worth holding: needed (at or coarser than the level
// wanted) before spare, then the error its cell shows without it (the
// level's own, the error of the next coarser in its place) over the view's
// threshold. Within a cell a finer level is worth no more than a coarser one
// (the errors do not fall): a cell's finest held level is its least worth
// keeping, its next finer level its most worth loading.
typedef struct ProxyStreamWorth {
    b32 needed;
    f32 error;
} ProxyStreamWorth;

// Level `level` of a cell with `errors`, wanted at `wanted`, at `scale`.
ProxyStreamWorth proxy_stream_worth(const f32 *errors, u32 level, u32 wanted, f32 scale);

// `a` is worth more than `b`.
bool proxy_stream_worth_above(ProxyStreamWorth a, ProxyStreamWorth b);

// A level worth `w` pushes one worth `held` out past the budget: needed
// over spare, else worth a quarter more (hysteresis: two cells of nearly
// the same worth do not trade a level back and forth as the camera moves).
bool proxy_stream_displaces(ProxyStreamWorth w, ProxyStreamWorth held);

// What a level's mesh takes on the GPU, by its counts (render/renderer.h's
// streams: records, bounds, makers, levels, voxels).
u64 proxy_stream_level_bytes(const NphMeshView *mv);

// A level an update may load (a cell's next finer) or drop (its finest
// held), kept in the update's one pass over the cells (ADR-0063 stage 4):
// its cell's index, in the order the cells were added, the level, and its
// worth.
typedef struct ProxyStreamPick {
    u32 cell;
    u32 level;
    ProxyStreamWorth worth;
} ProxyStreamPick;

// `p` kept among the first `cap` of `picks` (`*count` of them so far, a heap
// whose root is the last of them, each slot after its children): with
// `drop`, those worth least go first, else those worth most; between equal
// worths, the cell added first, as a scan over the cells would pick.
// O(log cap).
void proxy_stream_pick_keep(ProxyStreamPick *picks, u32 *count, u32 cap, ProxyStreamPick p,
                            bool drop);

// The `count` picks kept, in their order (the first to load or drop first).
void proxy_stream_pick_sort(ProxyStreamPick *picks, u32 count, bool drop);

// ---------------------------------------------------------------------------
// Texture streaming's decisions (texture_stream.c), pure, tested apart
// ---------------------------------------------------------------------------

// The level a paged texture with small levels from `fixed` is wanted at, by
// the frame each level was last asked for (`asked`, NPH_U64_MAX: never):
// the finest asked within `hold` frames of `frame`, else `fixed` (its small
// levels alone).
u32 texture_stream_wanted(const u64 *asked, u32 fixed, u64 frame, u32 hold);

// The frame any of its levels was last asked for (NPH_U64_MAX: never).
u64 texture_stream_last_asked(const u64 *asked, u32 fixed);

// A texture's place among those to install: `short` levels coarser than
// wanted, last asked `asked` (NPH_U64_MAX: never).
typedef struct TextureStreamNeed {
    u32 short_levels;
    u64 asked;
} TextureStreamNeed;

// `a` installs before `b`: the further from its wanted level first, then
// the more recently asked (never asked: last).
bool texture_stream_installs_before(TextureStreamNeed a, TextureStreamNeed b);

// `a`'s spare level is evicted before `b`'s: the less recently asked first
// (never asked: first).
bool texture_stream_evicts_before(u64 a_asked, u64 b_asked);

// The texels of page (x, y) of level `mip` in a level's tightly packed rows
// (`level`), and the level's row pitch, for a texture of `width` texels in
// `fi`'s blocks, its pages `page_width` x `page_height` texels.
const u8 *texture_stream_page_texels(const u8 *level, const NphGpuFormatInfo *fi, u32 width,
                                     u32 mip, u32 page_width, u32 page_height, u32 x, u32 y,
                                     u64 *out_pitch);

// The renderer's paging, as the stream uses it: the renderer's own
// (nph_texture_stream_create), or a stand-in the tests drive.
typedef struct TextureStreamPaging {
    void *user;
    NphResult (*pages)(void *user, NphRenderTexture texture, NphRenderTexturePages *out);
    NphResult (*levels_requested)(void *user, NphRenderTexture texture, u64 *out);
    NphResult (*install)(void *user, NphRenderTexture texture, u32 mip, u32 x, u32 y,
                         const void *texels, u64 row_pitch);
    void (*evict)(void *user, NphRenderTexture texture, u32 mip, u32 x, u32 y);
    void (*room)(void *user, NphRenderPageRoom *out);
    u64 (*frame)(void *user); // the requests' clock
} TextureStreamPaging;

// nph_texture_stream_create over `paging` (copied); desc->renderer is not
// used.
NphResult texture_stream_create_with(const NphTextureStreamDesc *desc,
                                     const TextureStreamPaging *paging, NphTextureStream **out);
