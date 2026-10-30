// SPDX-License-Identifier: Apache-2.0
// Paged textures: their pages' residency, installs and evictions, each
// texture's first resident level, and the frame's requests
// (docs/adr/0065-virtual-texturing.md; docs/render.md "Virtual textures").
//
// A page's life:
// - install (any thread): a page of memory and a staging slot, its texels
//   copied into the slot: INSTALLING, queued;
// - frame N, after its upload flush: bound, in the frame's one binding;
// - frame N + 1, before its flush: its copy recorded from the staging slot:
//   RESIDENT. The copy reaches the GPU in the frame's upload batch, which
//   the frame's graphics waits for, and the binding was made a frame
//   before, so the batch never waits on it;
// - evict: EVICTING, from the frame E being recorded next;
// - frame E + NPH_GPU_FRAMES_IN_FLIGHT, after its flush: unbound and its
//   memory freed (gpu/sparse.h returns it once the frame completes): ABSENT.
// A texture's samples clamp to its first resident level, the first from
// which every level's pages are RESIDENT (texture_paging.glsl): a level
// counts from the frame its last page's copy is recorded and stops counting
// from the frame one of its pages leaves. Every frame's materials take the
// levels as the frame begins, so a frame in flight keeps the levels it was
// recorded with, and no frame reads a page while its binding changes
// (Vulkan's rule; ADR-0065 point 3).

#include <nepheline/core/alloc.h>
#include <nepheline/core/log.h>
#include <nepheline/gpu/sparse.h>
#include <nepheline/gpu/upload.h>

#include "render_internal.h"

#include <string.h>

#define PAGE_BYTES NPH_GPU_SPARSE_PAGE_BYTES
#define NO_STAGING NPH_U32_MAX
#define NOT_BOUND  NPH_U64_MAX

// ---------------------------------------------------------------------------
// Levels and records
// ---------------------------------------------------------------------------

// The level of a page, and its column and row there.
static u32 page_level(const RenderPagedTexture *pt, u32 page, u32 *out_x, u32 *out_y)
{
    u32 level = 0U;
    while (level + 1U < pt->fixed_mip && pt->level_first[level + 1U] <= page) {
        ++level;
    }
    const u32 in_level = page - pt->level_first[level];
    *out_x = in_level % pt->pages_x[level];
    *out_y = in_level / pt->pages_x[level];
    return level;
}

// The texture's first resident level: the first from which every level's
// pages are resident (fixed_mip when none is). Its materials are
// written again where it changes.
static void update_full_level(RenderPaging *p, RenderPagedTexture *pt)
{
    u32 full = pt->fixed_mip;
    while (full > 0U &&
           pt->resident[full - 1U] == pt->pages_x[full - 1U] * pt->pages_y[full - 1U]) {
        --full;
    }
    if (full != pt->full_level) {
        p->level_changed = 1;
    }
    pt->full_level = full;
}

static NphResult write_record(NphRenderer *r, u32 sampled_index, const NphRenderTexturePaging *rec)
{
    RenderPaging *p = &r->paging;
    if (sampled_index >= p->record_capacity) {
        return NPH_RESULT_FULL;
    }
    return nph_gpu_upload_buffer(
        r->dev, p->records, (u64)sampled_index * sizeof(NphRenderTexturePaging), rec, sizeof *rec);
}

static u32 log2_u32(u32 v)
{
    u32 n = 0U;
    while ((1U << (n + 1U)) <= v) {
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool render_paging_device_supported(const NphRenderer *r)
{
    // The clamp is the samplers' minimum level of detail, which every device
    // has: sparse residency is all paging needs, where a sparse image samples
    // as the same ordinary image does (a paged texture must draw as the
    // texture whole: NPH_GPU_QUIRK_SPARSE_GRADIENT_SAMPLES).
    const NphGpuDeviceInfo *info = nph_gpu_device_info(r->dev);
    return !r->desc.disable_texture_paging && info->features.sparse_residency &&
           (info->quirks & (u32)NPH_GPU_QUIRK_SPARSE_GRADIENT_SAMPLES) == 0U;
}

NphResult render_paging_init(NphRenderer *r)
{
    RenderPaging *p = &r->paging;
    nph_mutex_init(&p->lock);
    p->initialized = 1;
    const NphRendererDesc *d = &r->desc;
    const NphGpuDeviceInfo *info = nph_gpu_device_info(r->dev);
    p->supported = render_paging_device_supported(r) ? 1 : 0;
    if (!p->supported) {
        return NPH_RESULT_OK;
    }
    NphAllocator *a = r->alloc;
    NphGpuDevice *dev = r->dev;
    // Each texture's request words: one of its own, then its bits rounded
    // up to whole words.
    p->word_capacity = d->max_texture_pages / 32U + 2U * d->max_textures;
    p->word_owner = NPH_ALLOC_ARRAY_ZEROED(a, u32, p->word_capacity);
    p->textures = NPH_ALLOC_ARRAY_ZEROED(a, RenderPagedTexture, d->max_textures);
    p->frame_level = NPH_ALLOC_ARRAY_ZEROED(a, u8, d->max_textures);
    p->staging_slots = d->page_staging_slots != 0U ? d->page_staging_slots
                                                   : nph_max_u32(4U * d->max_page_moves, 64U);
    p->staging_free = NPH_ALLOC_ARRAY(a, u32, p->staging_slots);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        p->staging_done[i] = NPH_ALLOC_ARRAY(a, u32, p->staging_slots);
    }
    p->binds = NPH_ALLOC_ARRAY(a, NphGpuSparseBind, 2ULL * d->max_page_moves);
    if (p->word_owner == NULL || p->textures == NULL || p->frame_level == NULL ||
        p->staging_free == NULL || p->binds == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        if (p->staging_done[i] == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    for (u32 i = 0; i < p->staging_slots; ++i) {
        p->staging_free[i] = p->staging_slots - 1U - i;
    }
    p->staging_free_count = p->staging_slots;
    NPH_TRY(nph_range_alloc_init(&p->word_alloc, a, p->word_capacity, d->max_textures));

    NphGpuPagePoolDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_pages = (u32)nph_min_u64(d->texture_page_bytes / PAGE_BYTES, NPH_U32_MAX);
    pd.name = "texture pages";
    NPH_TRY(nph_gpu_page_pool_create(dev, &pd, &p->pool));

    p->record_capacity = info->limits.bindless_sampled_images;
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = (u64)p->record_capacity * sizeof(NphRenderTexturePaging);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "texture paging records";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &p->records));
    bd.size = (u64)p->word_capacity * sizeof(u32);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC |
               (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.name = "texture page requests";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &p->requests));
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        bd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
        bd.memory = NPH_GPU_MEMORY_READBACK;
        bd.name = "texture page requests readback";
        NPH_TRY(nph_gpu_buffer_create(dev, &bd, &p->request_readback[i]));
    }
    bd.size = (u64)p->staging_slots * PAGE_BYTES;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    bd.memory = NPH_GPU_MEMORY_UPLOAD;
    bd.name = "texture page staging";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &p->staging));
    p->staging_map = (u8 *)nph_gpu_buffer_map(dev, p->staging);
    NPH_LOG_INFO(&NPH_LOG_RENDER,
                 "%s: textures paged: %llu MiB of pages, %u pages in all, %u staging slots",
                 r->name, (unsigned long long)(d->texture_page_bytes >> 20U), d->max_texture_pages,
                 p->staging_slots);
    return NPH_RESULT_OK;
}

void render_paging_shutdown(NphRenderer *r)
{
    RenderPaging *p = &r->paging;
    if (!p->initialized) {
        return;
    }
    NphAllocator *a = r->alloc;
    NphGpuDevice *dev = r->dev;
    if (p->textures != NULL) {
        for (u32 i = 0; i < r->desc.max_textures; ++i) {
            if (p->textures[i].pages != NULL) {
                render_paging_texture_destroy(r, i);
            }
        }
    }
    nph_gpu_buffer_destroy(dev, p->records);
    nph_gpu_buffer_destroy(dev, p->requests);
    nph_gpu_buffer_destroy(dev, p->staging);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        nph_gpu_buffer_destroy(dev, p->request_readback[i]);
        NPH_FREE_ARRAY(a, u32, p->staging_done[i], p->staging_slots);
    }
    nph_gpu_page_pool_destroy(dev, p->pool);
    nph_range_alloc_destroy(&p->word_alloc);
    NPH_FREE_ARRAY(a, u32, p->word_owner, p->word_capacity);
    NPH_FREE_ARRAY(a, RenderPagedTexture, p->textures, r->desc.max_textures);
    NPH_FREE_ARRAY(a, u8, p->frame_level, r->desc.max_textures);
    NPH_FREE_ARRAY(a, u32, p->staging_free, p->staging_slots);
    NPH_FREE_ARRAY(a, NphGpuSparseBind, p->binds, 2ULL * r->desc.max_page_moves);
    NPH_FREE_ARRAY(a, RenderPageRef, p->installs, p->install_cap);
    NPH_FREE_ARRAY(a, RenderPageRef, p->evictions, p->eviction_cap);
    nph_mutex_destroy(&p->lock);
    memset(p, 0, sizeof *p);
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

NphResult render_paging_texture_create(NphRenderer *r, u32 slot, const NphRenderTextureDesc *desc,
                                       u32 layers)
{
    RenderPaging *p = &r->paging;
    RenderTexture *t = &r->textures[slot];
    if (!p->supported || (desc->flags & (u32)NPH_RENDER_TEXTURE_FLAG_PAGED) == 0U ||
        desc->kind != NPH_RENDER_TEXTURE_2D || layers != 1U) {
        return NPH_RESULT_UNSUPPORTED;
    }
    NphGpuDevice *dev = r->dev;
    // The resolve's variants that test pages, before the first texture is
    // paged (a frame with one must not sample it without them).
    NPH_TRY(render_vis_paged_pipelines(r));
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.type = NPH_GPU_IMAGE_2D;
    id.format = desc->format;
    id.width = desc->width;
    id.height = desc->height;
    id.mip_count = desc->mip_count;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    id.flags = (u32)NPH_GPU_IMAGE_FLAG_SPARSE;
    id.name = desc->name != NULL ? desc->name : "render texture (paged)";
    NphGpuImage image = NPH_GPU_IMAGE_NULL;
    NPH_TRY(nph_gpu_image_create(dev, &id, &image));
    NphGpuSparseImageInfo info;
    NphResult res = nph_gpu_image_sparse_info(dev, image, &info);
    // The levels resident from creation, so that every texture always has a
    // level to sample: from the first under a page in either dimension. They
    // are the device's mip tail, and on a device that keeps none (lavapipe:
    // its tail starts past the last level), those levels' own pages.
    u32 fixed = res == NPH_RESULT_OK ? info.tail_first_mip : 0U;
    for (u32 m = 0; m < fixed; ++m) {
        if (nph_gpu_mip_extent(desc->width, m) < info.page_width ||
            nph_gpu_mip_extent(desc->height, m) < info.page_height) {
            fixed = m;
            break;
        }
    }
    u32 fixed_pages = res == NPH_RESULT_OK ? info.tail_pages : 0U;
    for (u32 m = fixed; res == NPH_RESULT_OK && m < info.tail_first_mip; ++m) {
        u32 px = 0U;
        u32 py = 0U;
        nph_gpu_image_sparse_pages(dev, image, m, &px, &py);
        fixed_pages += px * py;
    }
    // A texture whose every level is small has nothing to page; more pages
    // resident from creation than a texture keeps is not expected of the
    // standard shapes.
    if (res != NPH_RESULT_OK || fixed == 0U || fixed >= RENDER_PAGED_LEVELS ||
        fixed_pages > RENDER_FIXED_PAGES) {
        nph_gpu_image_destroy(dev, image);
        return NPH_RESULT_UNSUPPORTED;
    }
    RenderPagedTexture *pt = &p->textures[slot];
    memset(pt, 0, sizeof *pt);
    pt->fixed_mip = fixed;
    pt->page_width = info.page_width;
    pt->page_height = info.page_height;
    u32 pages = 0U;
    for (u32 m = 0; m < fixed; ++m) {
        nph_gpu_image_sparse_pages(dev, image, m, &pt->pages_x[m], &pt->pages_y[m]);
        pt->level_first[m] = pages;
        pages += pt->pages_x[m] * pt->pages_y[m];
    }
    pt->page_count = pages;
    nph_mutex_lock(&p->lock);
    res = pages <= 0xFFFFU ? nph_range_alloc(&p->word_alloc, 1U + (pages + 31U) / 32U, &pt->words)
                           : NPH_RESULT_FULL;
    nph_mutex_unlock(&p->lock);
    if (res == NPH_RESULT_OK) {
        pt->pages = NPH_ALLOC_ARRAY_ZEROED(r->alloc, RenderPage, pages);
        res = pt->pages != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    // The levels resident from creation, bound now and uploaded with the
    // frame's other uploads (which wait for the binding).
    pt->fixed_pages = fixed_pages;
    for (u32 i = 0; i < fixed_pages && res == NPH_RESULT_OK; ++i) {
        res = nph_gpu_page_alloc(dev, p->pool, &pt->fixed[i]);
    }
    if (res == NPH_RESULT_OK) {
        NphGpuSparseBind binds[RENDER_FIXED_PAGES];
        u32 n = 0U;
        for (u32 i = 0; i < info.tail_pages; ++i, ++n) {
            binds[n].image = image;
            binds[n].mip = NPH_GPU_SPARSE_TAIL;
            binds[n].x = i;
            binds[n].y = 0U;
            binds[n].pool = p->pool;
            binds[n].page = pt->fixed[n];
        }
        for (u32 m = fixed; m < info.tail_first_mip; ++m) {
            u32 px = 0U;
            u32 py = 0U;
            nph_gpu_image_sparse_pages(dev, image, m, &px, &py);
            for (u32 i = 0; i < px * py; ++i, ++n) {
                binds[n].image = image;
                binds[n].mip = m;
                binds[n].x = i % px;
                binds[n].y = i / px;
                binds[n].pool = p->pool;
                binds[n].page = pt->fixed[n];
            }
        }
        res = n != 0U ? nph_gpu_sparse_bind(dev, binds, n, NULL) : NPH_RESULT_OK;
    }
    for (u32 m = fixed; m < desc->mip_count && res == NPH_RESULT_OK; ++m) {
        res = nph_gpu_upload_image(dev, image, m, 0U, 1U, desc->mips[m], desc->mip_sizes[m]);
    }
    if (res != NPH_RESULT_OK) {
        for (u32 i = 0; i < RENDER_FIXED_PAGES; ++i) {
            nph_gpu_page_free(dev, p->pool, pt->fixed[i]);
        }
        nph_mutex_lock(&p->lock);
        nph_range_free(&p->word_alloc, pt->words);
        nph_mutex_unlock(&p->lock);
        NPH_FREE_ARRAY(r->alloc, RenderPage, pt->pages, pages);
        memset(pt, 0, sizeof *pt);
        nph_gpu_image_destroy(dev, image);
        return res;
    }
    nph_mutex_lock(&p->lock);
    for (u32 i = 0; i < pages; ++i) {
        pt->pages[i].staging = NO_STAGING;
        pt->pages[i].frame = NOT_BOUND;
        pt->pages[i].requested = NPH_U64_MAX;
    }
    const u32 word_count = 1U + (pages + 31U) / 32U;
    for (u32 w = 0; w < word_count; ++w) {
        p->word_owner[pt->words.offset + w] = slot + 1U;
    }
    p->words_used = nph_max_u32(p->words_used, pt->words.offset + word_count);
    update_full_level(p, pt);
    ++p->paged_count;
    nph_mutex_unlock(&p->lock);

    NphRenderTexturePaging rec;
    memset(&rec, 0, sizeof rec);
    rec.first_word = pt->words.offset;
    rec.extent = desc->width | (desc->height << 16U);
    rec.shape =
        log2_u32(pt->page_width) | (log2_u32(pt->page_height) << 4U) | (pt->fixed_mip << 8U);
    u32 levels[4] = {0U, 0U, 0U, 0U};
    for (u32 m = 0; m < pt->fixed_mip; ++m) {
        levels[m >> 1U] |= pt->level_first[m] << ((m & 1U) * 16U);
    }
    rec.levels01 = levels[0];
    rec.levels23 = levels[1];
    rec.levels45 = levels[2];
    rec.levels67 = levels[3];
    t->image = image;
    t->paged = 1;
    return write_record(r, nph_gpu_image_sampled_index(dev, image), &rec);
}

void render_paging_texture_destroy(NphRenderer *r, u32 slot)
{
    RenderPaging *p = &r->paging;
    RenderPagedTexture *pt = &p->textures[slot];
    if (pt->pages == NULL) {
        return;
    }
    NphGpuDevice *dev = r->dev;
    nph_mutex_lock(&p->lock);
    for (u32 i = 0; i < pt->page_count; ++i) {
        RenderPage *pg = &pt->pages[i];
        nph_gpu_page_free(dev, p->pool, pg->memory);
        if (pg->staging != NO_STAGING) {
            // A copy from it may be in this frame's batch: free with the frame.
            p->staging_done[r->frame_slot][p->staging_done_count[r->frame_slot]++] = pg->staging;
        }
    }
    for (u32 i = 0; i < pt->fixed_pages; ++i) {
        nph_gpu_page_free(dev, p->pool, pt->fixed[i]);
    }
    const u32 word_count = 1U + (pt->page_count + 31U) / 32U;
    for (u32 w = 0; w < word_count; ++w) {
        p->word_owner[pt->words.offset + w] = 0U;
    }
    nph_range_free(&p->word_alloc, pt->words);
    --p->paged_count;
    NPH_FREE_ARRAY(r->alloc, RenderPage, pt->pages, pt->page_count);
    memset(pt, 0, sizeof *pt);
    nph_mutex_unlock(&p->lock);
    // The queues' entries for it are skipped: their handles no longer match.
}

// A texture resident whole: a zero record at its bindless index, which a
// paged texture may have held before.
NphResult render_paging_texture_whole(NphRenderer *r, NphGpuImage image)
{
    if (!r->paging.supported) {
        return NPH_RESULT_OK;
    }
    NphRenderTexturePaging rec;
    memset(&rec, 0, sizeof rec);
    return write_record(r, nph_gpu_image_sampled_index(r->dev, image), &rec);
}

// ---------------------------------------------------------------------------
// Installs and evictions
// ---------------------------------------------------------------------------

// The texture's paging and the page's index, for a live paged texture's
// page; NULL otherwise.
static RenderPagedTexture *page_of(NphRenderer *r, NphRenderTexture texture, u32 mip, u32 x, u32 y,
                                   u32 *out_slot, u32 *out_page)
{
    const u32 slot = render_slot_of(&r->texture_pool, texture.value);
    if (slot == NPH_U32_MAX || !r->textures[slot].paged) {
        return NULL;
    }
    RenderPagedTexture *pt = &r->paging.textures[slot];
    if (mip >= pt->fixed_mip || x >= pt->pages_x[mip] || y >= pt->pages_y[mip]) {
        return NULL;
    }
    *out_slot = slot;
    *out_page = pt->level_first[mip] + y * pt->pages_x[mip] + x;
    return pt;
}

static bool push_ref(NphAllocator *a, RenderPageRef **array, u32 *count, u32 *cap,
                     const RenderPageRef *ref)
{
    if (*count == *cap) {
        const u32 grown = nph_max_u32(*cap * 2U, 64U);
        RenderPageRef *next = (RenderPageRef *)NPH_REALLOC(
            a, *array, (u64)*cap * sizeof(RenderPageRef), (u64)grown * sizeof(RenderPageRef),
            NPH_ALIGNOF(RenderPageRef));
        if (next == NULL) {
            return false;
        }
        *array = next;
        *cap = grown;
    }
    (*array)[(*count)++] = *ref;
    return true;
}

NphResult nph_render_texture_page_install(NphRenderer *r, NphRenderTexture texture, u32 mip, u32 x,
                                          u32 y, const void *texels, u64 row_pitch)
{
    NPH_ASSERT(r != NULL && texels != NULL);
    u32 slot = 0U;
    u32 page = 0U;
    RenderPagedTexture *pt = page_of(r, texture, mip, x, y, &slot, &page);
    if (pt == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    RenderPaging *p = &r->paging;
    const NphGpuImageDesc *id = nph_gpu_image_desc(r->dev, r->textures[slot].image);
    const NphGpuFormatInfo *fi = nph_gpu_format_info(id->format);
    const u32 level_w = nph_gpu_mip_extent(id->width, mip);
    const u32 level_h = nph_gpu_mip_extent(id->height, mip);
    const u32 pw = nph_min_u32(pt->page_width, level_w - x * pt->page_width);
    const u32 ph = nph_min_u32(pt->page_height, level_h - y * pt->page_height);
    const u64 row_bytes =
        (u64)((pw + fi->block_width - 1U) / fi->block_width) * fi->bytes_per_block;
    const u32 rows = (ph + fi->block_height - 1U) / fi->block_height;
    const u64 pitch = row_pitch != 0U ? row_pitch : row_bytes;
    if (pitch < row_bytes) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    nph_mutex_lock(&p->lock);
    RenderPage *pg = &pt->pages[page];
    NphResult res = NPH_RESULT_OK;
    if (pg->state == (u8)NPH_RENDER_PAGE_EVICTING) {
        // Its memory still holds it: sampled again from the next frame.
        pg->state = (u8)NPH_RENDER_PAGE_RESIDENT;
        ++pt->resident[mip];
        update_full_level(p, pt);
    } else if (pg->state == (u8)NPH_RENDER_PAGE_ABSENT) {
        NphGpuPage memory = NPH_GPU_PAGE_NULL;
        res = p->staging_free_count != 0U ? nph_gpu_page_alloc(r->dev, p->pool, &memory)
                                          : NPH_RESULT_FULL;
        const RenderPageRef ref = {slot, texture.value, page, 0U};
        if (res == NPH_RESULT_OK &&
            !push_ref(r->alloc, &p->installs, &p->install_count, &p->install_cap, &ref)) {
            nph_gpu_page_free(r->dev, p->pool, memory);
            res = NPH_RESULT_OUT_OF_MEMORY;
        }
        if (res == NPH_RESULT_OK) {
            const u32 s = p->staging_free[--p->staging_free_count];
            u8 *dst = p->staging_map + (u64)s * PAGE_BYTES;
            const u8 *src = (const u8 *)texels;
            for (u32 row = 0; row < rows; ++row) {
                memcpy(dst + (u64)row * row_bytes, src + (u64)row * pitch, row_bytes);
            }
            pg->state = (u8)NPH_RENDER_PAGE_INSTALLING;
            pg->memory = memory;
            pg->staging = s;
            pg->frame = NOT_BOUND;
        }
    }
    nph_mutex_unlock(&p->lock);
    return res;
}

void nph_render_texture_page_evict(NphRenderer *r, NphRenderTexture texture, u32 mip, u32 x, u32 y)
{
    NPH_ASSERT(r != NULL);
    u32 slot = 0U;
    u32 page = 0U;
    RenderPagedTexture *pt = page_of(r, texture, mip, x, y, &slot, &page);
    if (pt == NULL) {
        return;
    }
    RenderPaging *p = &r->paging;
    nph_mutex_lock(&p->lock);
    RenderPage *pg = &pt->pages[page];
    // Frames up to the one recorded last may read it; it is unbound once
    // they are done.
    const u64 due = r->last_frame_index == NPH_U64_MAX ? 0U : r->last_frame_index + 1U;
    if (pg->state == (u8)NPH_RENDER_PAGE_RESIDENT) {
        --pt->resident[mip];
        update_full_level(p, pt);
        pg->state = (u8)NPH_RENDER_PAGE_EVICTING;
        pg->frame = due;
        const RenderPageRef ref = {slot, texture.value, page, 0U};
        if (!push_ref(r->alloc, &p->evictions, &p->eviction_count, &p->eviction_cap, &ref)) {
            // No room to wait: resident it stays.
            ++pt->resident[mip];
            update_full_level(p, pt);
            pg->state = (u8)NPH_RENDER_PAGE_RESIDENT;
        }
    } else if (pg->state == (u8)NPH_RENDER_PAGE_INSTALLING) {
        if (pg->frame == NOT_BOUND) {
            // Not bound yet: nothing on the GPU has it.
            nph_gpu_page_free(r->dev, p->pool, pg->memory);
            p->staging_free[p->staging_free_count++] = pg->staging;
            pg->memory = NPH_GPU_PAGE_NULL;
            pg->staging = NO_STAGING;
            pg->state = (u8)NPH_RENDER_PAGE_ABSENT;
        } else {
            // Bound, its copy not recorded: never sampled, but its binding
            // waits as an eviction's does. Its staging slot is free now.
            p->staging_free[p->staging_free_count++] = pg->staging;
            pg->staging = NO_STAGING;
            pg->state = (u8)NPH_RENDER_PAGE_EVICTING;
            pg->frame = due;
            const RenderPageRef ref = {slot, texture.value, page, 0U};
            if (!push_ref(r->alloc, &p->evictions, &p->eviction_count, &p->eviction_cap, &ref)) {
                NPH_LOG_ERROR(&NPH_LOG_RENDER, "%s: no room to evict a page; it stays bound",
                              r->name);
            }
        }
    }
    nph_mutex_unlock(&p->lock);
}

NphRenderPageState nph_render_texture_page_state(NphRenderer *r, NphRenderTexture texture, u32 mip,
                                                 u32 x, u32 y)
{
    NPH_ASSERT(r != NULL);
    u32 slot = 0U;
    u32 page = 0U;
    RenderPagedTexture *pt = page_of(r, texture, mip, x, y, &slot, &page);
    if (pt == NULL) {
        return NPH_RENDER_PAGE_ABSENT;
    }
    nph_mutex_lock(&r->paging.lock);
    const NphRenderPageState s = (NphRenderPageState)pt->pages[page].state;
    nph_mutex_unlock(&r->paging.lock);
    return s;
}

u64 nph_render_texture_page_requested(NphRenderer *r, NphRenderTexture texture, u32 mip, u32 x,
                                      u32 y)
{
    NPH_ASSERT(r != NULL);
    u32 slot = 0U;
    u32 page = 0U;
    RenderPagedTexture *pt = page_of(r, texture, mip, x, y, &slot, &page);
    if (pt == NULL) {
        return NPH_U64_MAX;
    }
    nph_mutex_lock(&r->paging.lock);
    const u64 f = pt->pages[page].requested;
    nph_mutex_unlock(&r->paging.lock);
    return f;
}

bool nph_renderer_texture_paging_supported(const NphRenderer *r)
{
    NPH_ASSERT(r != NULL);
    return r->paging.supported != 0;
}

NphResult nph_render_texture_pages(const NphRenderer *r, NphRenderTexture texture,
                                   NphRenderTexturePages *out)
{
    NPH_ASSERT(r != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    const u32 slot = render_slot_of(&r->texture_pool, texture.value);
    if (slot == NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!r->textures[slot].paged) {
        return NPH_RESULT_OK;
    }
    const RenderPagedTexture *pt = &r->paging.textures[slot];
    out->paged = 1;
    out->page_width = pt->page_width;
    out->page_height = pt->page_height;
    out->tail_first_mip = pt->fixed_mip;
    for (u32 m = 0; m < pt->fixed_mip; ++m) {
        out->pages_x[m] = pt->pages_x[m];
        out->pages_y[m] = pt->pages_y[m];
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// The pixel of each 8 x 8 tile whose footprints ask for pages in `frame`
// (x | y << 3): the tile's pixels in the order of an 8 x 8 Bayer matrix, so
// that each frame's pixel lies far from the last few and every pixel asks
// once in 64 frames. The entry at (x, y) interleaves the bits of x ^ y and
// y, the highest pair from bit 0 (Bayer, "An optimum method for two-level
// rendition of continuous-tone pictures", 1973); here the inverse.
static u32 feedback_pixel(u64 frame)
{
    const u32 k = (u32)(frame & 63U);
    u32 x = 0U;
    u32 y = 0U;
    for (u32 i = 0; i < 3U; ++i) {
        const u32 pair = (k >> (2U * (2U - i))) & 3U;
        x |= (pair >> 1U) << i;
        y |= (pair & 1U) << i;
    }
    return (x ^ y) | (y << 3U);
}

// A queue entry's page, while its texture lives.
static RenderPage *ref_page(NphRenderer *r, const RenderPageRef *ref, RenderPagedTexture **out_pt)
{
    if (r->textures[ref->texture].handle != ref->handle || !r->textures[ref->texture].paged) {
        return NULL;
    }
    RenderPagedTexture *pt = &r->paging.textures[ref->texture];
    *out_pt = pt;
    return &pt->pages[ref->page];
}

// The requests of the frame that last completed in this slot.
static void read_requests(NphRenderer *r)
{
    RenderPaging *p = &r->paging;
    const u32 fs = r->frame_slot;
    if (p->request_frame[fs] == 0U) {
        return;
    }
    const u64 frame = p->request_frame[fs] - 1U;
    const u32 *words = (const u32 *)nph_gpu_buffer_map(r->dev, p->request_readback[fs]);
    const u32 count = nph_min_u32(p->request_words[fs], p->words_used);
    for (u32 w = 0; w < count; ++w) {
        u32 bits = words[w];
        const u32 owner = p->word_owner[w];
        if (bits == 0U || owner == 0U) {
            continue;
        }
        RenderPagedTexture *pt = &p->textures[owner - 1U];
        const u32 at = w - pt->words.offset;
        if (at == 0U) {
            continue; // the first word holds no page
        }
        while (bits != 0U) {
            const u32 bit = nph_ctz_u32(bits);
            bits &= bits - 1U;
            const u32 page = (at - 1U) * 32U + bit;
            if (page < pt->page_count) {
                pt->pages[page].requested = frame;
            }
        }
    }
    p->request_frame[fs] = 0U;
}

void render_paging_frame_begin(NphRenderer *r)
{
    RenderPaging *p = &r->paging;
    p->frame_active = 0;
    p->frame_requests = NPH_GPU_RG_BUFFER_NULL;
    if (!p->supported) {
        return;
    }
    NphGpuDevice *dev = r->dev;
    const u64 frame = nph_gpu_frame_index(dev);
    const u32 fs = r->frame_slot;
    nph_mutex_lock(&p->lock);
    p->frame_index = frame;
    p->installed = 0U;
    p->evicted = 0U;
    // The staging the slot's last frame copied from is free: it completed.
    for (u32 i = 0; i < p->staging_done_count[fs]; ++i) {
        p->staging_free[p->staging_free_count++] = p->staging_done[fs][i];
    }
    p->staging_done_count[fs] = 0U;
    read_requests(r);
    // Pages bound in an earlier frame: their copies in this frame's upload
    // batch, resident from this frame. The others keep their places.
    u32 kept = 0U;
    for (u32 i = 0; i < p->install_count; ++i) {
        const RenderPageRef ref = p->installs[i];
        RenderPagedTexture *pt = NULL;
        RenderPage *pg = ref_page(r, &ref, &pt);
        if (pg == NULL || pg->state != (u8)NPH_RENDER_PAGE_INSTALLING) {
            continue;
        }
        if (pg->frame == NOT_BOUND || pg->frame >= frame) {
            p->installs[kept++] = ref;
            continue;
        }
        u32 x = 0U;
        u32 y = 0U;
        const u32 level = page_level(pt, ref.page, &x, &y);
        const NphResult res =
            nph_gpu_upload_sparse_page(dev, r->textures[ref.texture].image, level, x, y, p->staging,
                                       (u64)pg->staging * PAGE_BYTES);
        if (res != NPH_RESULT_OK) {
            p->installs[kept++] = ref; // tried again next frame
            continue;
        }
        p->staging_done[fs][p->staging_done_count[fs]++] = pg->staging;
        pg->staging = NO_STAGING;
        pg->state = (u8)NPH_RENDER_PAGE_RESIDENT;
        ++pt->resident[level];
        update_full_level(p, pt);
        ++p->installed;
    }
    p->install_count = kept;
    // The paged textures' first resident levels this frame; the materials
    // are written again where one changed.
    if (p->level_changed) {
        for (u32 i = 0; i < r->desc.max_textures; ++i) {
            p->frame_level[i] =
                p->textures[i].pages != NULL ? (u8)p->textures[i].full_level : (u8)0U;
        }
        p->level_changed = 0;
        r->material_version += 1U;
    }
    NphGpuPagePoolStats st;
    nph_gpu_page_pool_stats(p->pool, &st);
    p->resident_pages = st.pages_used;
    // The frame's record.
    NphRenderPagingView *view =
        (NphRenderPagingView *)render_frame_at(r, r->layout.paging_view_offset);
    memset(view, 0, sizeof *view);
    view->records = nph_gpu_buffer_address(dev, p->records);
    view->requests = nph_gpu_buffer_address(dev, p->requests);
    view->max_anisotropy = r->desc.max_anisotropy;
    view->feedback_phase = feedback_pixel(frame);
    p->frame_active = p->paged_count != 0U ? 1 : 0;
    nph_mutex_unlock(&p->lock);
}

NphResult render_paging_frame_bind(NphRenderer *r)
{
    RenderPaging *p = &r->paging;
    if (!p->supported) {
        return NPH_RESULT_OK;
    }
    nph_mutex_lock(&p->lock);
    const u32 moves = r->desc.max_page_moves;
    const u64 frame = p->frame_index;
    u32 n = 0U;
    // Unbindings due: no frame in flight was recorded with the page's bit.
    u32 kept = 0U;
    for (u32 i = 0; i < p->eviction_count; ++i) {
        const RenderPageRef ref = p->evictions[i];
        RenderPagedTexture *pt = NULL;
        RenderPage *pg = ref_page(r, &ref, &pt);
        if (pg == NULL || pg->state != (u8)NPH_RENDER_PAGE_EVICTING) {
            continue; // destroyed with its texture, or resident again
        }
        if (frame < pg->frame + NPH_GPU_FRAMES_IN_FLIGHT || p->evicted == moves) {
            p->evictions[kept++] = ref;
            continue;
        }
        u32 x = 0U;
        u32 y = 0U;
        NphGpuSparseBind *b = &p->binds[n++];
        b->image = r->textures[ref.texture].image;
        b->mip = page_level(pt, ref.page, &x, &y);
        b->x = x;
        b->y = y;
        b->pool = NULL;
        b->page = NPH_GPU_PAGE_NULL;
        nph_gpu_page_free(r->dev, p->pool, pg->memory);
        pg->memory = NPH_GPU_PAGE_NULL;
        pg->state = (u8)NPH_RENDER_PAGE_ABSENT;
        pg->frame = NOT_BOUND;
        ++p->evicted;
    }
    p->eviction_count = kept;
    // Installs to bind, oldest first.
    u32 bound = 0U;
    for (u32 i = 0; i < p->install_count && bound < moves; ++i) {
        const RenderPageRef ref = p->installs[i];
        RenderPagedTexture *pt = NULL;
        RenderPage *pg = ref_page(r, &ref, &pt);
        if (pg == NULL || pg->state != (u8)NPH_RENDER_PAGE_INSTALLING || pg->frame != NOT_BOUND) {
            continue;
        }
        u32 x = 0U;
        u32 y = 0U;
        NphGpuSparseBind *b = &p->binds[n++];
        b->image = r->textures[ref.texture].image;
        b->mip = page_level(pt, ref.page, &x, &y);
        b->x = x;
        b->y = y;
        b->pool = p->pool;
        b->page = pg->memory;
        pg->frame = frame;
        ++bound;
    }
    NphResult res = NPH_RESULT_OK;
    if (n != 0U) {
        res = nph_gpu_sparse_bind(r->dev, p->binds, n, NULL);
        if (res != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RENDER, "%s: binding %u texture pages failed: %s", r->name, n,
                          nph_result_string(res));
        }
    }
    nph_mutex_unlock(&p->lock);
    return res;
}

static void clear_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderPaging *p = &r->paging;
    nph_gpu_cmd_fill_buffer(cmd, nph_gpu_rg_buffer(graph, p->frame_requests), 0U,
                            (u64)nph_max_u32(p->words_used, 1U) * sizeof(u32), 0U);
}

static void readback_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderPaging *p = &r->paging;
    const NphGpuBufferCopy region = {0U, 0U, (u64)nph_max_u32(p->words_used, 1U) * sizeof(u32)};
    nph_gpu_cmd_copy_buffer(cmd, nph_gpu_rg_buffer(graph, p->frame_requests),
                            nph_gpu_rg_buffer(graph, p->frame_readback), &region, 1U);
}

static NphGpuRgPass *transfer_pass(NphGpuRenderGraph *graph, NphRenderer *r, const char *name,
                                   NphGpuRgExecuteFn execute)
{
    NphGpuRgPassDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = NPH_GPU_RG_PASS_TRANSFER;
    d.execute = execute;
    d.user = r;
    return nph_gpu_rg_add_pass(graph, &d);
}

NphResult render_paging_declare_clear(NphRenderer *r, NphGpuRenderGraph *graph)
{
    RenderPaging *p = &r->paging;
    if (!p->frame_active) {
        return NPH_RESULT_OK;
    }
    p->frame_requests = nph_gpu_rg_import_buffer(graph, p->requests);
    NphGpuRgPass *pass = transfer_pass(graph, r, "paging requests clear", clear_execute);
    if (pass == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_write_buffer(pass, p->frame_requests, NPH_GPU_RG_USE_TRANSFER_DST);
    return NPH_RESULT_OK;
}

NphResult render_paging_declare_readback(NphRenderer *r, NphGpuRenderGraph *graph)
{
    RenderPaging *p = &r->paging;
    if (!p->frame_active || nph_gpu_rg_buffer_is_null(p->frame_requests)) {
        return NPH_RESULT_OK;
    }
    NphGpuRgPass *pass = transfer_pass(graph, r, "paging requests readback", readback_execute);
    if (pass == NULL) {
        return NPH_RESULT_FULL;
    }
    p->frame_readback = nph_gpu_rg_import_buffer(graph, p->request_readback[r->frame_slot]);
    nph_gpu_rg_read_buffer(pass, p->frame_requests, NPH_GPU_RG_USE_TRANSFER_SRC);
    nph_gpu_rg_write_buffer(pass, p->frame_readback, NPH_GPU_RG_USE_TRANSFER_DST);
    p->request_frame[r->frame_slot] = p->frame_index + 1U;
    p->request_words[r->frame_slot] = p->words_used;
    return NPH_RESULT_OK;
}

u64 render_paging_view_address(const NphRenderer *r)
{
    return r->paging.frame_active ? r->frame_addresses[r->frame_slot] + r->layout.paging_view_offset
                                  : 0U;
}
