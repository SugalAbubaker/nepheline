// SPDX-License-Identifier: Apache-2.0
#pragma once

// Streaming paged textures' levels (ADR-0065 stage 3; docs/runtime.md
// "Texture streaming").
//
// A paged texture (NPH_RENDER_TEXTURE_FLAG_PAGED) holds its levels resident
// whole: its small levels always, and above them the levels from its first
// resident one, which its samples clamp to (render/renderer.h). The stream
// holds, for each texture it is given, the levels its pixels ask for:
//   - its wanted level is the finest asked for within `hold_frames`
//     (nph_render_texture_levels_requested); a texture unasked so long wants
//     its small levels alone;
//   - each update it installs pages of the textures held coarser than
//     wanted, a level at a time from the coarsest missing (a finer level is
//     sampled only once whole), the textures furthest from their wanted
//     level first, then the most recently asked, at most `max_pages` pages
//     an update (the renderer binds max_page_moves a frame);
//   - past `budget_bytes`, before installing, it evicts levels its textures
//     do not want (finer than wanted, or of a texture unasked), the least
//     recently asked first; it evicts no wanted level, so that textures that
//     want the same do not trade levels back and forth: a budget that the
//     wanted levels fill leaves the rest coarser than wanted (counted:
//     NphTextureStreamStats.short_textures).
// A level being installed that its texture stops wanting is evicted with
// the rest. Pages' texels come from the caller's levels, copied as they
// are installed.
//
// The renderer's page memory (NphRendererDesc.texture_page_bytes) holds the
// budget beside the textures' small levels and the pages evicted within the
// frames in flight. Where it does not, pages wait for it; and where it is
// full of pages none of which are returning, the stream evicts what no
// texture wants as if past the budget (a level an update, logged once).
//
// Costs: create allocates the textures' table (`max_textures`); add,
// remove, and texture_state look the texture up among those held (O(n));
// update allocates nothing, asks the renderer for each texture's requests
// once, sorts the textures that want more (O(n log n)), and copies at most
// `max_pages` pages' texels (64 KiB each).
// Thread-safety: NPH_MAIN_THREAD_ONLY; update between nph_gpu_frame_begin and
// nph_render_frame, so that the pages' copies go with the frame. The
// renderer outlives the stream.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/core/types.h>
#include <nepheline/render/renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphTextureStream NphTextureStream;

typedef struct NphTextureStreamDesc {
    NphAllocator *alloc;   // required
    NphRenderer *renderer; // required
    u32 max_textures;      // textures it holds at once (at least 1)
    // The bytes the levels it holds above the textures' small levels may
    // take on the GPU, those being installed counted whole (required).
    u64 budget_bytes;
    u32 max_pages;   // pages installed an update at most (0: 32)
    u32 hold_frames; // frames a level asked for stays wanted (0: 240, a second at 240 Hz)
} NphTextureStreamDesc;

// A paged texture to stream, with its levels' texels, rows tightly packed
// (as NphRenderTextureDesc.mips), each level below its small ones at
// least. The pointers are copied; the texels are the caller's for as long
// as the stream holds the texture.
typedef struct NphTextureStreamTexture {
    NphRenderTexture texture; // paged (nph_render_texture_pages says so)
    const void *const *mips;
} NphTextureStreamTexture;

// NPH_RESULT_INVALID_ARGUMENT for a missing renderer or allocator, no
// textures, or no budget; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_texture_stream_create(const NphTextureStreamDesc *desc,
                                                 NphTextureStream **out);

// Evicts every level it installed and frees the stream; the textures are
// the caller's. Safe on NULL.
void nph_texture_stream_destroy(NphTextureStream *s);

// Adds a texture, its small levels alone held. NPH_RESULT_INVALID_ARGUMENT
// for a stale texture, one not paged, or texels missing;
// NPH_RESULT_ALREADY_EXISTS for one held; NPH_RESULT_FULL past
// max_textures.
NPH_MUST_USE NphResult nph_texture_stream_add(NphTextureStream *s,
                                              const NphTextureStreamTexture *texture);

// Stops streaming a texture: evicts the levels the stream installed, and
// no longer reads its texels. A texture it does not hold is ignored. A
// texture destroyed while held is let go at the next update (its pages go
// with it).
void nph_texture_stream_remove(NphTextureStream *s, NphRenderTexture texture);

// One frame's streaming (see the file comment), by the renderer's device's
// frame index (nph_gpu_frame_index), the requests' clock. Fails only where
// the renderer refuses a page for a reason other than its staging or page
// memory being full (those wait for the next update).
NPH_MUST_USE NphResult nph_texture_stream_update(NphTextureStream *s);

typedef struct NphTextureStreamStats {
    u32 textures;
    u32 held_levels;    // above the textures' small levels, whole
    u64 held_bytes;     // the pages of those and of the levels being installed
    u32 asked;          // textures asked for within hold_frames (the last update)
    u32 short_textures; // textures held coarser than wanted (the last update)
    u32 loading;        // levels being installed
    // Pages installed by the last update, and evicted by it and by the
    // removals since.
    u32 installed;
    u32 evicted;
} NphTextureStreamStats;

void nph_texture_stream_stats(const NphTextureStream *s, NphTextureStreamStats *out);

// What the stream holds and wants of a texture, as of the last update: for
// tools and tests.
typedef struct NphTextureStreamTextureState {
    u32 held;    // its finest level installed whole (its small levels' first: none)
    u32 wanted;  // the level it is wanted at
    b32 loading; // its next finer level is being installed
} NphTextureStreamTextureState;

// NPH_RESULT_NOT_FOUND for a texture it does not hold.
NPH_MUST_USE NphResult nph_texture_stream_texture_state(const NphTextureStream *s,
                                                        NphRenderTexture texture,
                                                        NphTextureStreamTextureState *out);

#ifdef __cplusplus
}
#endif
