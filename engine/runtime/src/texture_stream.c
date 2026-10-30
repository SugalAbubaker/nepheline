// SPDX-License-Identifier: Apache-2.0
// Streaming paged textures' levels (runtime/texture_stream.h; ADR-0065
// stage 3; docs/runtime.md "Texture streaming").

#include <nepheline/core/alloc.h>
#include <nepheline/core/log.h>
#include <nepheline/gpu/device.h>
#include <nepheline/gpu/sparse.h>
#include <nepheline/runtime/texture_stream.h>

#include "runtime_internal.h"

#include <string.h>

#define NONE         NPH_U32_MAX
#define NEVER        NPH_U64_MAX
#define PAGE_BYTES   ((u64)NPH_GPU_SPARSE_PAGE_BYTES)
#define DEFAULT_HOLD 240U
#define DEFAULT_MAX  32U

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------

u32 texture_stream_wanted(const u64 *asked, u32 fixed, u64 frame, u32 hold)
{
    for (u32 m = 0; m < fixed; ++m) {
        if (asked[m] != NEVER && asked[m] <= frame && frame - asked[m] <= hold) {
            return m;
        }
    }
    return fixed;
}

u64 texture_stream_last_asked(const u64 *asked, u32 fixed)
{
    u64 last = NEVER;
    for (u32 m = 0; m < fixed; ++m) {
        if (asked[m] != NEVER && (last == NEVER || asked[m] > last)) {
            last = asked[m];
        }
    }
    return last;
}

bool texture_stream_installs_before(TextureStreamNeed a, TextureStreamNeed b)
{
    if (a.short_levels != b.short_levels) {
        return a.short_levels > b.short_levels;
    }
    if (a.asked == NEVER || b.asked == NEVER) {
        return a.asked != NEVER && b.asked == NEVER;
    }
    return a.asked > b.asked;
}

bool texture_stream_evicts_before(u64 a_asked, u64 b_asked)
{
    if (a_asked == NEVER || b_asked == NEVER) {
        return a_asked == NEVER && b_asked != NEVER;
    }
    return a_asked < b_asked;
}

const u8 *texture_stream_page_texels(const u8 *level, const NphGpuFormatInfo *fi, u32 width,
                                     u32 mip, u32 page_width, u32 page_height, u32 x, u32 y,
                                     u64 *out_pitch)
{
    const u32 w = nph_gpu_mip_extent(width, mip);
    const u64 pitch = (u64)((w + fi->block_width - 1U) / fi->block_width) * fi->bytes_per_block;
    *out_pitch = pitch;
    const u64 row = (u64)y * (page_height / fi->block_height);
    const u64 col = (u64)x * (page_width / fi->block_width);
    return level + row * pitch + col * fi->bytes_per_block;
}

// ---------------------------------------------------------------------------
// The renderer's paging
// ---------------------------------------------------------------------------

static NphResult renderer_pages(void *user, NphRenderTexture texture, NphRenderTexturePages *out)
{
    return nph_render_texture_pages((const NphRenderer *)user, texture, out);
}

static NphResult renderer_levels_requested(void *user, NphRenderTexture texture, u64 *out)
{
    return nph_render_texture_levels_requested((NphRenderer *)user, texture, out);
}

static NphResult renderer_install(void *user, NphRenderTexture texture, u32 mip, u32 x, u32 y,
                                  const void *texels, u64 row_pitch)
{
    return nph_render_texture_page_install((NphRenderer *)user, texture, mip, x, y, texels,
                                           row_pitch);
}

static void renderer_evict(void *user, NphRenderTexture texture, u32 mip, u32 x, u32 y)
{
    nph_render_texture_page_evict((NphRenderer *)user, texture, mip, x, y);
}

static void renderer_room(void *user, NphRenderPageRoom *out)
{
    nph_render_texture_page_room((NphRenderer *)user, out);
}

static u64 renderer_frame(void *user)
{
    return nph_gpu_frame_index(nph_renderer_device((NphRenderer *)user));
}

// ---------------------------------------------------------------------------
// The stream
// ---------------------------------------------------------------------------

typedef struct StreamTexture {
    NphRenderTexture texture;
    const void *mips[NPH_GPU_MAX_MIPS]; // the caller's texels
    const NphGpuFormatInfo *format;
    NphRenderTexturePages pages;
    u32 held;      // levels from here are installed whole (pages.tail_first_mip: none)
    u32 loading;   // the level being installed (NONE)
    u32 next_page; // its next page to install, row by row
    u32 wanted;
    u64 asked; // the frame any of its levels was last asked for
} StreamTexture;

struct NphTextureStream {
    NphAllocator *alloc;
    TextureStreamPaging paging;
    StreamTexture *textures; // [cap], the first `count` held
    u32 count;
    u32 cap;
    u32 *order; // [cap]: the update's textures to install, in turn
    u64 budget;
    u32 max_pages;
    u32 hold;
    u64 held_bytes; // the levels held whole and those being installed, whole
    b32 warned;     // that the page memory holds less than the budget
    NphTextureStreamStats stats;
};

static u32 level_pages(const StreamTexture *t, u32 mip)
{
    return t->pages.pages_x[mip] * t->pages.pages_y[mip];
}

static u64 level_bytes(const StreamTexture *t, u32 mip)
{
    return (u64)level_pages(t, mip) * PAGE_BYTES;
}

static u32 find(const NphTextureStream *s, NphRenderTexture texture)
{
    for (u32 i = 0; i < s->count; ++i) {
        if (s->textures[i].texture.value == texture.value) {
            return i;
        }
    }
    return NONE;
}

// Evicts the first `pages` pages of level `mip`, row by row.
static void evict_pages(NphTextureStream *s, StreamTexture *t, u32 mip, u32 pages)
{
    for (u32 i = 0; i < pages; ++i) {
        s->paging.evict(s->paging.user, t->texture, mip, i % t->pages.pages_x[mip],
                        i / t->pages.pages_x[mip]);
    }
    s->stats.evicted += pages;
}

// Stops installing `t`'s level, evicting what of it is installed.
static void cancel_loading(NphTextureStream *s, StreamTexture *t)
{
    if (t->loading == NONE) {
        return;
    }
    evict_pages(s, t, t->loading, t->next_page);
    s->held_bytes -= level_bytes(t, t->loading);
    t->loading = NONE;
    t->next_page = 0U;
}

// Evicts `t`'s finest held level.
static void evict_level(NphTextureStream *s, StreamTexture *t)
{
    NPH_ASSERT(t->held < t->pages.tail_first_mip);
    evict_pages(s, t, t->held, level_pages(t, t->held));
    s->held_bytes -= level_bytes(t, t->held);
    s->stats.held_levels -= 1U;
    t->held += 1U;
}

// Forgets texture `i`, which holds nothing the stream installed (or whose
// pages went with it): the last texture takes its place.
static void forget(NphTextureStream *s, u32 i)
{
    StreamTexture *t = &s->textures[i];
    if (t->loading != NONE) {
        s->held_bytes -= level_bytes(t, t->loading);
    }
    for (u32 m = t->held; m < t->pages.tail_first_mip; ++m) {
        s->held_bytes -= level_bytes(t, m);
        s->stats.held_levels -= 1U;
    }
    s->count -= 1U;
    if (i != s->count) {
        s->textures[i] = s->textures[s->count];
    }
    s->stats.textures = s->count;
}

// Evicts one level no texture wants (finer than its texture's wanted
// level), that of the least recently asked texture; false if none is.
static bool evict_spare(NphTextureStream *s)
{
    StreamTexture *victim = NULL;
    for (u32 i = 0; i < s->count; ++i) {
        StreamTexture *t = &s->textures[i];
        if (t->held < t->wanted && t->loading == NONE &&
            (victim == NULL || texture_stream_evicts_before(t->asked, victim->asked))) {
            victim = t;
        }
    }
    if (victim == NULL) {
        return false;
    }
    evict_level(s, victim);
    return true;
}

static bool fits(const NphTextureStream *s, u64 bytes)
{
    return s->held_bytes + bytes <= s->budget;
}

// Evicts what no texture wants until `bytes` more fit the budget, the least
// recently asked first; whether they fit. Within an update, once nothing is
// spare nothing becomes so (an install leaves its texture at or coarser than
// its wanted level), so `*spare` saves the textures short of room a scan of
// every texture each (Bistro's 189 short at 512 MB: 0.09 ms an update).
static bool make_room(NphTextureStream *s, u64 bytes, bool *spare)
{
    while (!fits(s, bytes)) {
        if (!*spare || !evict_spare(s)) {
            *spare = false;
            return false;
        }
    }
    return true;
}

// Texture `a` installs before texture `b` (indices among those held).
static bool installs_first(const NphTextureStream *s, u32 a, u32 b)
{
    const StreamTexture *ta = &s->textures[a];
    const StreamTexture *tb = &s->textures[b];
    const TextureStreamNeed na = {ta->held - ta->wanted, ta->asked};
    const TextureStreamNeed nb = {tb->held - tb->wanted, tb->asked};
    if (texture_stream_installs_before(na, nb)) {
        return true;
    }
    if (texture_stream_installs_before(nb, na)) {
        return false;
    }
    return a < b; // ties: the texture held longer
}

// Sifts order[i] down the heap of the first `n`, whose root installs last.
static void sift_down(NphTextureStream *s, u32 n, u32 i)
{
    const u32 v = s->order[i];
    for (;;) {
        u32 c = 2U * i + 1U;
        if (c >= n) {
            break;
        }
        if (c + 1U < n && installs_first(s, s->order[c], s->order[c + 1U])) {
            c += 1U; // the child that installs later
        }
        if (!installs_first(s, v, s->order[c])) {
            break;
        }
        s->order[i] = s->order[c];
        i = c;
    }
    s->order[i] = v;
}

// The update's candidates in turn, by a heap sort: the last to install to
// the end each time.
static void sort_order(NphTextureStream *s, u32 n)
{
    for (u32 i = n / 2U; i > 0U; --i) {
        sift_down(s, n, i - 1U);
    }
    for (u32 end = n; end > 1U; --end) {
        const u32 last = s->order[0];
        s->order[0] = s->order[end - 1U];
        s->order[end - 1U] = last;
        sift_down(s, end - 1U, 0U);
    }
}

NphResult texture_stream_create_with(const NphTextureStreamDesc *desc,
                                     const TextureStreamPaging *paging, NphTextureStream **out)
{
    NPH_ASSERT(desc != NULL && paging != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL || desc->max_textures == 0U || desc->budget_bytes == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphTextureStream *s = NPH_ALLOC_TYPE(desc->alloc, NphTextureStream);
    if (s == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    s->alloc = desc->alloc;
    s->paging = *paging;
    s->cap = desc->max_textures;
    s->budget = desc->budget_bytes;
    s->max_pages = desc->max_pages != 0U ? desc->max_pages : DEFAULT_MAX;
    s->hold = desc->hold_frames != 0U ? desc->hold_frames : DEFAULT_HOLD;
    s->textures = NPH_ALLOC_ARRAY_ZEROED(desc->alloc, StreamTexture, s->cap);
    s->order = NPH_ALLOC_ARRAY(desc->alloc, u32, s->cap);
    if (s->textures == NULL || s->order == NULL) {
        nph_texture_stream_destroy(s);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    *out = s;
    return NPH_RESULT_OK;
}

NphResult nph_texture_stream_create(const NphTextureStreamDesc *desc, NphTextureStream **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->renderer == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const TextureStreamPaging paging = {
        desc->renderer, renderer_pages, renderer_levels_requested, renderer_install, renderer_evict,
        renderer_room,  renderer_frame,
    };
    return texture_stream_create_with(desc, &paging, out);
}

void nph_texture_stream_destroy(NphTextureStream *s)
{
    if (s == NULL) {
        return;
    }
    if (s->textures != NULL) {
        for (u32 i = 0; i < s->count; ++i) {
            StreamTexture *t = &s->textures[i];
            cancel_loading(s, t);
            while (t->held < t->pages.tail_first_mip) {
                evict_level(s, t);
            }
        }
    }
    NPH_FREE_ARRAY(s->alloc, StreamTexture, s->textures, s->cap);
    NPH_FREE_ARRAY(s->alloc, u32, s->order, s->cap);
    NPH_FREE_TYPE(s->alloc, NphTextureStream, s);
}

NphResult nph_texture_stream_add(NphTextureStream *s, const NphTextureStreamTexture *texture)
{
    NPH_ASSERT(s != NULL && texture != NULL);
    if (find(s, texture->texture) != NONE) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    if (s->count == s->cap) {
        return NPH_RESULT_FULL;
    }
    StreamTexture *t = &s->textures[s->count];
    memset(t, 0, sizeof *t);
    NPH_TRY(s->paging.pages(s->paging.user, texture->texture, &t->pages));
    if (!t->pages.paged || texture->mips == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 m = 0; m < t->pages.tail_first_mip; ++m) {
        if (texture->mips[m] == NULL) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        t->mips[m] = texture->mips[m];
    }
    t->texture = texture->texture;
    t->format = nph_gpu_format_info(t->pages.format);
    t->held = t->pages.tail_first_mip;
    t->loading = NONE;
    t->wanted = t->pages.tail_first_mip;
    t->asked = NEVER;
    s->count += 1U;
    s->stats.textures = s->count;
    return NPH_RESULT_OK;
}

void nph_texture_stream_remove(NphTextureStream *s, NphRenderTexture texture)
{
    NPH_ASSERT(s != NULL);
    const u32 i = find(s, texture);
    if (i == NONE) {
        return;
    }
    StreamTexture *t = &s->textures[i];
    cancel_loading(s, t);
    while (t->held < t->pages.tail_first_mip) {
        evict_level(s, t);
    }
    forget(s, i);
}

// Installs pages of `t`'s level being installed, at most `*budget` of them;
// `*full` where the renderer has no room left this frame (its staging or
// its page memory).
static NphResult install_some(NphTextureStream *s, StreamTexture *t, u32 *budget, bool *full)
{
    const u32 mip = t->loading;
    const u32 pages = level_pages(t, mip);
    while (t->next_page < pages && *budget > 0U) {
        const u32 x = t->next_page % t->pages.pages_x[mip];
        const u32 y = t->next_page / t->pages.pages_x[mip];
        u64 pitch = 0U;
        const u8 *texels =
            texture_stream_page_texels((const u8 *)t->mips[mip], t->format, t->pages.width, mip,
                                       t->pages.page_width, t->pages.page_height, x, y, &pitch);
        const NphResult r = s->paging.install(s->paging.user, t->texture, mip, x, y, texels, pitch);
        if (r == NPH_RESULT_FULL) {
            *full = true;
            return NPH_RESULT_OK;
        }
        NPH_TRY(r);
        t->next_page += 1U;
        *budget -= 1U;
        s->stats.installed += 1U;
    }
    if (t->next_page == pages) {
        t->held = mip;
        t->loading = NONE;
        t->next_page = 0U;
        s->stats.held_levels += 1U;
    }
    return NPH_RESULT_OK;
}

// The renderer refused a page for want of room. Where its page memory is
// full of pages none of which are returning (it holds less than the budget
// beside the textures' small levels), what no texture wants leaves as if
// past the budget, a level an update, its pages returning once the frames
// in flight are done with them. Pages returning, or the staging full, only
// mean waiting.
static void page_memory_full(NphTextureStream *s)
{
    NphRenderPageRoom room;
    s->paging.room(s->paging.user, &room);
    if (room.pages_free != 0U || room.pages_returning != 0U || room.staging_free == 0U) {
        return;
    }
    if (evict_spare(s) && !s->warned) {
        s->warned = 1;
        NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                     "texture stream: the renderer's page memory (%u pages) holds less than "
                     "the budget (%llu MB) beside the textures' small levels",
                     room.pages, (unsigned long long)(s->budget / 1000000U));
    }
}

NphResult nph_texture_stream_update(NphTextureStream *s)
{
    NPH_ASSERT(s != NULL);
    const u64 frame = s->paging.frame(s->paging.user);
    s->stats.installed = 0U;
    s->stats.evicted = 0U;
    s->stats.asked = 0U;
    // Each texture's wanted level; a level being installed it no longer
    // wants leaves, and a texture destroyed under the stream is let go.
    u32 candidates = 0U;
    for (u32 i = 0; i < s->count;) {
        StreamTexture *t = &s->textures[i];
        u64 asked[NPH_GPU_MAX_MIPS];
        if (s->paging.levels_requested(s->paging.user, t->texture, asked) != NPH_RESULT_OK) {
            forget(s, i); // the last texture, not yet seen, now at `i`
            continue;
        }
        const u32 fixed = t->pages.tail_first_mip;
        t->wanted = texture_stream_wanted(asked, fixed, frame, s->hold);
        t->asked = texture_stream_last_asked(asked, fixed);
        if (t->wanted < fixed) {
            s->stats.asked += 1U;
        }
        if (t->loading != NONE && t->loading < t->wanted) {
            cancel_loading(s, t);
        }
        if (t->held > t->wanted) {
            s->order[candidates++] = i;
        }
        ++i;
    }
    // The textures furthest from their wanted level first: each its next
    // finer level, room made of what no texture wants.
    sort_order(s, candidates);
    u32 pages_left = s->max_pages;
    bool full = false;
    bool spare = true;
    for (u32 k = 0; k < candidates && pages_left > 0U && !full; ++k) {
        StreamTexture *t = &s->textures[s->order[k]];
        if (t->loading == NONE) {
            const u32 next = t->held - 1U;
            const u64 bytes = level_bytes(t, next);
            if (!make_room(s, bytes, &spare)) {
                continue; // too large for what the wanted levels leave: a smaller may fit
            }
            t->loading = next;
            t->next_page = 0U;
            s->held_bytes += bytes;
        }
        NPH_TRY(install_some(s, t, &pages_left, &full));
    }
    if (full) {
        page_memory_full(s);
    }
    u32 short_textures = 0U;
    u32 loading = 0U;
    for (u32 i = 0; i < s->count; ++i) {
        short_textures += s->textures[i].held > s->textures[i].wanted ? 1U : 0U;
        loading += s->textures[i].loading != NONE ? 1U : 0U;
    }
    s->stats.short_textures = short_textures;
    s->stats.loading = loading;
    return NPH_RESULT_OK;
}

void nph_texture_stream_stats(const NphTextureStream *s, NphTextureStreamStats *out)
{
    NPH_ASSERT(s != NULL && out != NULL);
    *out = s->stats;
    out->held_bytes = s->held_bytes;
}

NphResult nph_texture_stream_texture_state(const NphTextureStream *s, NphRenderTexture texture,
                                           NphTextureStreamTextureState *out)
{
    NPH_ASSERT(s != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    const u32 i = find(s, texture);
    if (i == NONE) {
        return NPH_RESULT_NOT_FOUND;
    }
    const StreamTexture *t = &s->textures[i];
    out->held = t->held;
    out->wanted = t->wanted;
    out->loading = t->loading != NONE ? 1 : 0;
    return NPH_RESULT_OK;
}
