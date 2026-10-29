// SPDX-License-Identifier: Apache-2.0
// Sparse images' pages: the page pools and the bindings (sparse.h;
// docs/adr/0065-virtual-texturing.md).

#include <nepheline/core/alloc.h>

#include "gpu_internal.h"

#include <stdio.h>

// A page's state in its pool.
enum {
    PAGE_FREE = 0,
    PAGE_USED = 1,
    PAGE_PENDING = 2, // freed, its deferred return queued
};

struct NphGpuPagePool {
    NphGpuDevice *dev;
    NphGpuPagePool *next; // in the device's list of live pools
    NphMutex lock;        // held while a block is allocated, so a mutex
    u32 max_pages;
    u32 block_pages;
    u32 max_blocks;
    u32 block_count;
    NphGpuAllocation *blocks; // [max_blocks]
    VkDeviceMemory *memory;   // [max_blocks]
    u64 *offset;              // [max_blocks]: where the block starts in its memory
    u32 *free_pages;          // [max_pages]: pages returned, a stack
    u32 free_count;
    u32 fresh; // pages [0, fresh) have been handed out at least once
    u8 *state; // [max_pages]
    u32 used;
    u32 pending;
    // Destroyed while pages were still on their way back: the last return
    // frees the pool (the deferred entries of different frame slots may run
    // in any order at a device's wait for idle).
    b32 dead;
    b32 destroyed; // nph_gpu_page_pool_destroy was called
    char name[NPH_GPU_NAME_MAX];
};

static NphAtomicU32 g_pool_names = NPH_ATOMIC_U32_INIT(0U);

// ---------------------------------------------------------------------------
// The device's part
// ---------------------------------------------------------------------------

NphResult gpu_sparse_init(NphGpuDevice *dev)
{
    nph_spin_lock_init(&dev->page_pool_lock);
    nph_atomic_store_u64(&dev->sparse_value, 0U, NPH_MEMORY_ORDER_RELAXED);
    if (!dev->info.features.sparse_residency) {
        return NPH_RESULT_OK;
    }
    // A pool serves every sparse image, so its blocks come from the memory
    // types that every format a texture is made of allows.
    static const NphGpuFormat formats[] = {
        NPH_GPU_FORMAT_BC7_SRGB,       NPH_GPU_FORMAT_BC7_UNORM,
        NPH_GPU_FORMAT_BC6H_UFLOAT,    NPH_GPU_FORMAT_BC5_UNORM,
        NPH_GPU_FORMAT_BC4_UNORM,      NPH_GPU_FORMAT_R8G8B8A8_SRGB,
        NPH_GPU_FORMAT_R8G8B8A8_UNORM, NPH_GPU_FORMAT_R8G8_UNORM,
        NPH_GPU_FORMAT_R8_UNORM,       NPH_GPU_FORMAT_R16G16B16A16_SFLOAT,
    };
    u32 bits = 0U;
    for (u32 i = 0; i < dev->mem_props.memoryTypeCount; ++i) {
        if ((dev->mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) !=
            0U) {
            bits |= 1U << i;
        }
    }
    u32 paged = 0U;
    for (u32 i = 0; i < (u32)(sizeof formats / sizeof formats[0]); ++i) {
        if (nph_gpu_format_info(formats[i])->compressed &&
            !dev->info.features.texture_compression_bc) {
            continue;
        }
        NphGpuImageDesc d;
        memset(&d, 0, sizeof d);
        d.type = NPH_GPU_IMAGE_2D;
        d.format = formats[i];
        d.width = 1024U;
        d.height = 1024U;
        d.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
                  (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
        d.flags = (u32)NPH_GPU_IMAGE_FLAG_SPARSE;
        const u32 b = gpu_sparse_memory_bits(dev, &d);
        if (b != 0U) {
            bits &= b;
            ++paged;
        }
    }
    if (paged == 0U || bits == 0U) {
        GPU_LOG_WARN("sparse residency: no device-local memory type serves every format's "
                     "pages; sparse images disabled");
        dev->info.features.sparse_residency = 0;
        dev->sparse_queue = NULL;
        return NPH_RESULT_OK;
    }
    dev->sparse_memory_bits = bits;
    GPU_LOG_INFO("sparse residency: pages bound on the %s queue, memory types 0x%x",
                 dev->sparse_queue == dev->queues[NPH_GPU_QUEUE_GRAPHICS] ? "graphics" : "transfer",
                 bits);
    return gpu_timeline_create_internal(dev, 0U, "sparse timeline", &dev->sparse_timeline);
}

void gpu_sparse_shutdown(NphGpuDevice *dev)
{
    // Pools their owners did not destroy (the deferred entries have run).
    while (dev->page_pools != NULL) {
        NphGpuPagePool *pool = dev->page_pools;
        GPU_LOG_ERROR("leaked page pool '%s' (%u pages in use)", pool->name, pool->used);
        pool->pending = 0U;
        gpu_sparse_destroy_pool(dev, pool);
    }
    const u32 cap = dev->sparse_scratch_cap;
    NPH_FREE_ARRAY(dev->alloc, VkSparseImageMemoryBind, dev->sparse_image_binds, cap);
    NPH_FREE_ARRAY(dev->alloc, VkSparseImageMemoryBindInfo, dev->sparse_image_infos, cap);
    NPH_FREE_ARRAY(dev->alloc, VkSparseMemoryBind, dev->sparse_opaque_binds, cap);
    NPH_FREE_ARRAY(dev->alloc, VkSparseImageOpaqueMemoryBindInfo, dev->sparse_opaque_infos, cap);
    dev->sparse_image_binds = NULL;
    dev->sparse_image_infos = NULL;
    dev->sparse_opaque_binds = NULL;
    dev->sparse_opaque_infos = NULL;
    dev->sparse_scratch_cap = 0U;
    // The timeline is one of the device's and goes with them.
    dev->sparse_timeline = NULL;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

NphResult nph_gpu_image_sparse_info(const NphGpuDevice *dev, NphGpuImage image,
                                    NphGpuSparseImageInfo *out)
{
    NPH_ASSERT(dev != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    if (slot == NULL || !slot->sparse) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    out->page_width = slot->page_width;
    out->page_height = slot->page_height;
    out->tail_first_mip = slot->tail_first_mip;
    out->tail_pages = slot->tail_pages;
    return NPH_RESULT_OK;
}

static void slot_pages(const NphGpuImageSlot *slot, u32 mip, u32 *out_x, u32 *out_y)
{
    const u32 w = nph_gpu_mip_extent(slot->desc.width, mip);
    const u32 h = nph_gpu_mip_extent(slot->desc.height, mip);
    *out_x = (w + slot->page_width - 1U) / slot->page_width;
    *out_y = (h + slot->page_height - 1U) / slot->page_height;
}

void nph_gpu_image_sparse_pages(const NphGpuDevice *dev, NphGpuImage image, u32 mip,
                                u32 *out_pages_x, u32 *out_pages_y)
{
    NPH_ASSERT(dev != NULL && out_pages_x != NULL && out_pages_y != NULL);
    *out_pages_x = 0U;
    *out_pages_y = 0U;
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    if (slot != NULL && slot->sparse && mip < slot->tail_first_mip) {
        slot_pages(slot, mip, out_pages_x, out_pages_y);
    }
}

// ---------------------------------------------------------------------------
// Page pools
// ---------------------------------------------------------------------------

static void pool_free_arrays(NphAllocator *alloc, NphGpuPagePool *pool)
{
    NPH_FREE_ARRAY(alloc, NphGpuAllocation, pool->blocks, pool->max_blocks);
    NPH_FREE_ARRAY(alloc, VkDeviceMemory, pool->memory, pool->max_blocks);
    NPH_FREE_ARRAY(alloc, u64, pool->offset, pool->max_blocks);
    NPH_FREE_ARRAY(alloc, u32, pool->free_pages, pool->max_pages);
    NPH_FREE_ARRAY(alloc, u8, pool->state, pool->max_pages);
}

NphResult nph_gpu_page_pool_create(NphGpuDevice *dev, const NphGpuPagePoolDesc *desc,
                                   NphGpuPagePool **out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL);
    *out = NULL;
    if (!dev->info.features.sparse_residency) {
        return NPH_RESULT_UNSUPPORTED;
    }
    if (desc->max_pages == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAllocator *alloc = dev->alloc;
    NphGpuPagePool *pool = NPH_ALLOC_TYPE(alloc, NphGpuPagePool);
    if (pool == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    pool->dev = dev;
    pool->max_pages = desc->max_pages;
    pool->block_pages = desc->block_pages != 0U ? desc->block_pages : 1024U;
    pool->max_blocks = (pool->max_pages + pool->block_pages - 1U) / pool->block_pages;
    pool->blocks = NPH_ALLOC_ARRAY_ZEROED(alloc, NphGpuAllocation, pool->max_blocks);
    pool->memory = NPH_ALLOC_ARRAY_ZEROED(alloc, VkDeviceMemory, pool->max_blocks);
    pool->offset = NPH_ALLOC_ARRAY_ZEROED(alloc, u64, pool->max_blocks);
    pool->free_pages = NPH_ALLOC_ARRAY(alloc, u32, pool->max_pages);
    pool->state = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, pool->max_pages);
    if (pool->blocks == NULL || pool->memory == NULL || pool->offset == NULL ||
        pool->free_pages == NULL || pool->state == NULL) {
        pool_free_arrays(alloc, pool);
        NPH_FREE_TYPE(alloc, NphGpuPagePool, pool);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    nph_mutex_init(&pool->lock);
    gpu_copy_name(pool->name, desc->name, "page pool",
                  nph_atomic_fetch_add_u32(&g_pool_names, 1U, NPH_MEMORY_ORDER_RELAXED));
    nph_spin_lock_acquire(&dev->page_pool_lock);
    pool->next = dev->page_pools;
    dev->page_pools = pool;
    nph_spin_lock_release(&dev->page_pool_lock);
    *out = pool;
    return NPH_RESULT_OK;
}

void nph_gpu_page_pool_destroy(NphGpuDevice *dev, NphGpuPagePool *pool)
{
    NPH_ASSERT(dev != NULL);
    if (pool == NULL) {
        return;
    }
    NPH_ASSERT_MSG(pool->dev == dev, "page pool destroyed with another device");
    nph_mutex_lock(&pool->lock);
    pool->destroyed = 1;
    nph_mutex_unlock(&pool->lock);
    NphGpuDeferred e;
    e.type = (u32)NPH_GPU_DEFERRED_PAGE_POOL;
    e.index = 0U;
    e.handle = pool;
    e.alloc = NULL;
    gpu_defer(dev, &e);
}

void gpu_sparse_destroy_pool(NphGpuDevice *dev, NphGpuPagePool *pool)
{
    nph_spin_lock_acquire(&dev->page_pool_lock);
    NphGpuPagePool **link = &dev->page_pools;
    while (*link != NULL && *link != pool) {
        link = &(*link)->next;
    }
    if (*link == pool) {
        *link = pool->next;
    }
    nph_spin_lock_release(&dev->page_pool_lock);
    nph_mutex_lock(&pool->lock);
    if (pool->used != 0U) {
        GPU_LOG_ERROR("page pool '%s' destroyed with %u pages in use", pool->name, pool->used);
    }
    for (u32 b = 0; b < pool->block_count; ++b) {
        nph_gpu_alloc_free(dev->vma, pool->blocks[b]);
        pool->blocks[b] = NULL;
    }
    pool->block_count = 0U;
    const bool keep = pool->pending != 0U;
    pool->dead = keep ? 1 : 0;
    nph_mutex_unlock(&pool->lock);
    if (!keep) {
        nph_mutex_destroy(&pool->lock);
        pool_free_arrays(dev->alloc, pool);
        NPH_FREE_TYPE(dev->alloc, NphGpuPagePool, pool);
    }
}

// A block for pages [block_count * block_pages, ...): the last one holds
// only what the capacity leaves.
static NphResult pool_grow(NphGpuPagePool *pool)
{
    NphGpuDevice *dev = pool->dev;
    const u32 first = pool->block_count * pool->block_pages;
    const u32 pages = nph_min_u32(pool->block_pages, pool->max_pages - first);
    NphGpuAllocation block = NULL;
    const VkResult r =
        nph_gpu_alloc_allocate_block(dev->vma, (u64)pages * NPH_GPU_SPARSE_PAGE_BYTES,
                                     NPH_GPU_SPARSE_PAGE_BYTES, dev->sparse_memory_bits, &block);
    if (r != VK_SUCCESS) {
        GPU_LOG_ERROR("page pool '%s': no memory for a block of %u pages: %s", pool->name, pages,
                      nph_gpu_vk_result_name(r));
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const u32 b = pool->block_count++;
    pool->blocks[b] = block;
    nph_gpu_alloc_memory(dev->vma, block, &pool->memory[b], &pool->offset[b]);
    if (dev->has_debug_utils) {
        char name[NPH_GPU_NAME_MAX + 16U];
        snprintf(name, sizeof name, "%s block %u", pool->name, b);
        gpu_set_name(dev, VK_OBJECT_TYPE_DEVICE_MEMORY, (u64)(uptr)pool->memory[b], name);
    }
    return NPH_RESULT_OK;
}

NphResult nph_gpu_page_alloc(NphGpuDevice *dev, NphGpuPagePool *pool, NphGpuPage *out)
{
    NPH_ASSERT(dev != NULL && pool != NULL && out != NULL);
    NPH_ASSERT_MSG(pool->dev == dev && !pool->destroyed, "page from a destroyed pool");
    NPH_UNUSED(dev); // the pool knows its device; the parameter is for the asserts
    out->value = 0U;
    nph_mutex_lock(&pool->lock);
    NphResult r = NPH_RESULT_OK;
    u32 index = NPH_U32_MAX;
    if (pool->free_count != 0U) {
        index = pool->free_pages[--pool->free_count];
    } else if (pool->fresh < pool->max_pages) {
        if (pool->fresh == pool->block_count * pool->block_pages) {
            r = pool_grow(pool);
        }
        if (r == NPH_RESULT_OK) {
            index = pool->fresh++;
        }
    } else {
        r = NPH_RESULT_FULL;
    }
    if (r == NPH_RESULT_OK) {
        NPH_ASSERT(pool->state[index] == PAGE_FREE);
        pool->state[index] = PAGE_USED;
        ++pool->used;
        out->value = index + 1U;
    }
    nph_mutex_unlock(&pool->lock);
    return r;
}

void nph_gpu_page_free(NphGpuDevice *dev, NphGpuPagePool *pool, NphGpuPage page)
{
    NPH_ASSERT(dev != NULL && pool != NULL);
    if (page.value == 0U) {
        return;
    }
    const u32 index = page.value - 1U;
    nph_mutex_lock(&pool->lock);
    const bool valid =
        index < pool->max_pages && pool->state[index] == PAGE_USED && pool->destroyed == 0;
    NPH_ASSERT_MSG(valid, "page freed twice, never allocated, or from a destroyed pool");
    if (valid) {
        pool->state[index] = PAGE_PENDING;
        --pool->used;
        ++pool->pending;
    }
    nph_mutex_unlock(&pool->lock);
    if (!valid) {
        GPU_LOG_ERROR("page pool '%s': page %u freed but not in use", pool->name, page.value);
        return;
    }
    NphGpuDeferred e;
    e.type = (u32)NPH_GPU_DEFERRED_SPARSE_PAGE;
    e.index = index;
    e.handle = pool;
    e.alloc = NULL;
    gpu_defer(dev, &e);
}

void gpu_sparse_return_page(NphGpuPagePool *pool, u32 index)
{
    nph_mutex_lock(&pool->lock);
    NPH_ASSERT(pool->state[index] == PAGE_PENDING && pool->pending != 0U);
    pool->state[index] = PAGE_FREE;
    --pool->pending;
    pool->free_pages[pool->free_count++] = index;
    const bool last = pool->dead != 0 && pool->pending == 0U;
    nph_mutex_unlock(&pool->lock);
    if (last) {
        NphAllocator *alloc = pool->dev->alloc;
        nph_mutex_destroy(&pool->lock);
        pool_free_arrays(alloc, pool);
        NPH_FREE_TYPE(alloc, NphGpuPagePool, pool);
    }
}

void nph_gpu_page_pool_stats(NphGpuPagePool *pool, NphGpuPagePoolStats *out)
{
    NPH_ASSERT(pool != NULL && out != NULL);
    NphGpuPagePool *p = pool;
    nph_mutex_lock(&p->lock);
    out->pages_used = p->used;
    out->pages_pending = p->pending;
    out->blocks = p->block_count;
    u64 bytes = 0U;
    for (u32 b = 0; b < p->block_count; ++b) {
        const u32 first = b * p->block_pages;
        bytes += (u64)nph_min_u32(p->block_pages, p->max_pages - first) * NPH_GPU_SPARSE_PAGE_BYTES;
    }
    out->bytes = bytes;
    nph_mutex_unlock(&p->lock);
}

// The memory of a page in use. False (logged) for one that is not.
static bool page_memory(NphGpuPagePool *pool, NphGpuPage page, VkDeviceMemory *out_memory,
                        u64 *out_offset)
{
    const u32 index = page.value - 1U;
    nph_mutex_lock(&pool->lock);
    const bool used = index < pool->max_pages && pool->state[index] == PAGE_USED;
    if (used) {
        const u32 b = index / pool->block_pages;
        *out_memory = pool->memory[b];
        *out_offset =
            pool->offset[b] + (u64)(index % pool->block_pages) * NPH_GPU_SPARSE_PAGE_BYTES;
    }
    nph_mutex_unlock(&pool->lock);
    if (!used) {
        GPU_LOG_ERROR("page pool '%s': page %u bound but not in use", pool->name, page.value);
    }
    return used;
}

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

typedef struct SparseScratch {
    VkSparseImageMemoryBind *image_binds;
    VkSparseImageMemoryBindInfo *image_infos;
    VkSparseMemoryBind *opaque_binds;
    VkSparseImageOpaqueMemoryBindInfo *opaque_infos;
} SparseScratch;

// Room for `count` bindings of each kind (under the sparse queue's lock).
// The arrays are rewritten by every call, so growing them keeps nothing.
static NphResult scratch_for(NphGpuDevice *dev, u32 count, SparseScratch *out)
{
    if (count > dev->sparse_scratch_cap) {
        NphAllocator *alloc = dev->alloc;
        const u32 old = dev->sparse_scratch_cap;
        const u32 cap = nph_max_u32(count, old * 2U);
        VkSparseImageMemoryBind *image_binds = NPH_ALLOC_ARRAY(alloc, VkSparseImageMemoryBind, cap);
        VkSparseImageMemoryBindInfo *image_infos =
            NPH_ALLOC_ARRAY(alloc, VkSparseImageMemoryBindInfo, cap);
        VkSparseMemoryBind *opaque_binds = NPH_ALLOC_ARRAY(alloc, VkSparseMemoryBind, cap);
        VkSparseImageOpaqueMemoryBindInfo *opaque_infos =
            NPH_ALLOC_ARRAY(alloc, VkSparseImageOpaqueMemoryBindInfo, cap);
        const bool ok = image_binds != NULL && image_infos != NULL && opaque_binds != NULL &&
                        opaque_infos != NULL;
        // On failure the new arrays go; on success the old ones.
        const u32 drop = ok ? old : cap;
        NPH_FREE_ARRAY(alloc, VkSparseImageMemoryBind, ok ? dev->sparse_image_binds : image_binds,
                       drop);
        NPH_FREE_ARRAY(alloc, VkSparseImageMemoryBindInfo,
                       ok ? dev->sparse_image_infos : image_infos, drop);
        NPH_FREE_ARRAY(alloc, VkSparseMemoryBind, ok ? dev->sparse_opaque_binds : opaque_binds,
                       drop);
        NPH_FREE_ARRAY(alloc, VkSparseImageOpaqueMemoryBindInfo,
                       ok ? dev->sparse_opaque_infos : opaque_infos, drop);
        if (!ok) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        dev->sparse_image_binds = image_binds;
        dev->sparse_image_infos = image_infos;
        dev->sparse_opaque_binds = opaque_binds;
        dev->sparse_opaque_infos = opaque_infos;
        dev->sparse_scratch_cap = cap;
    }
    out->image_binds = dev->sparse_image_binds;
    out->image_infos = dev->sparse_image_infos;
    out->opaque_binds = dev->sparse_opaque_binds;
    out->opaque_infos = dev->sparse_opaque_infos;
    return NPH_RESULT_OK;
}

NphResult nph_gpu_sparse_bind(NphGpuDevice *dev, const NphGpuSparseBind *binds, u32 count,
                              u64 *out_value)
{
    NPH_ASSERT(dev != NULL && binds != NULL);
    if (out_value != NULL) {
        *out_value = 0U;
    }
    if (!dev->info.features.sparse_residency) {
        return NPH_RESULT_UNSUPPORTED;
    }
    if (count == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphGpuQueue *q = dev->sparse_queue;
    nph_mutex_lock(&q->lock);
    SparseScratch s;
    NphResult r = scratch_for(dev, count, &s);
    u32 image_binds = 0U;
    u32 image_infos = 0U;
    u32 opaque_binds = 0U;
    u32 opaque_infos = 0U;
    // Consecutive bindings of one image's pages (or of its tail) share an info.
    u32 last_image = 0U;
    bool last_tail = false;
    for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
        const NphGpuSparseBind *b = &binds[i];
        const NphGpuImageSlot *slot = gpu_image(dev, b->image);
        if (slot == NULL || !slot->sparse) {
            GPU_LOG_ERROR("sparse bind %u: not a sparse image", i);
            r = NPH_RESULT_INVALID_ARGUMENT;
            break;
        }
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u64 memory_offset = 0U;
        if (b->page.value != 0U &&
            (b->pool == NULL || !page_memory(b->pool, b->page, &memory, &memory_offset))) {
            r = NPH_RESULT_INVALID_ARGUMENT;
            break;
        }
        const bool tail = b->mip == NPH_GPU_SPARSE_TAIL;
        const bool grouped = i != 0U && last_image == b->image.value && last_tail == tail;
        if (tail) {
            if (b->x >= slot->tail_pages || b->y != 0U) {
                GPU_LOG_ERROR("sparse bind %u: image '%s' has %u tail pages, not page %u,%u", i,
                              slot->name, slot->tail_pages, b->x, b->y);
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
            VkSparseMemoryBind *m = &s.opaque_binds[opaque_binds];
            const u64 start = (u64)b->x * NPH_GPU_SPARSE_PAGE_BYTES;
            m->resourceOffset = slot->tail_offset + start;
            m->size = nph_min_u64(NPH_GPU_SPARSE_PAGE_BYTES, slot->tail_bytes - start);
            m->memory = memory;
            m->memoryOffset = memory_offset;
            m->flags = 0U;
            if (grouped) {
                ++s.opaque_infos[opaque_infos - 1U].bindCount;
            } else {
                s.opaque_infos[opaque_infos].image = slot->vk;
                s.opaque_infos[opaque_infos].bindCount = 1U;
                s.opaque_infos[opaque_infos].pBinds = m;
                ++opaque_infos;
            }
            ++opaque_binds;
        } else {
            u32 pages_x = 0U;
            u32 pages_y = 0U;
            if (b->mip < slot->tail_first_mip) {
                slot_pages(slot, b->mip, &pages_x, &pages_y);
            }
            if (b->x >= pages_x || b->y >= pages_y) {
                GPU_LOG_ERROR("sparse bind %u: image '%s' mip %u has %ux%u pages, not page %u,%u",
                              i, slot->name, b->mip, pages_x, pages_y, b->x, b->y);
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
            const u32 w = nph_gpu_mip_extent(slot->desc.width, b->mip);
            const u32 h = nph_gpu_mip_extent(slot->desc.height, b->mip);
            const u32 x0 = b->x * slot->page_width;
            const u32 y0 = b->y * slot->page_height;
            VkSparseImageMemoryBind *m = &s.image_binds[image_binds];
            m->subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            m->subresource.mipLevel = b->mip;
            m->subresource.arrayLayer = 0U;
            m->offset.x = (i32)x0;
            m->offset.y = (i32)y0;
            m->offset.z = 0;
            // A page at the level's right or bottom edge ends with it.
            m->extent.width = nph_min_u32(slot->page_width, w - x0);
            m->extent.height = nph_min_u32(slot->page_height, h - y0);
            m->extent.depth = 1U;
            m->memory = memory;
            m->memoryOffset = memory_offset;
            m->flags = 0U;
            if (grouped) {
                ++s.image_infos[image_infos - 1U].bindCount;
            } else {
                s.image_infos[image_infos].image = slot->vk;
                s.image_infos[image_infos].bindCount = 1U;
                s.image_infos[image_infos].pBinds = m;
                ++image_infos;
            }
            ++image_binds;
        }
        last_image = b->image.value;
        last_tail = tail;
    }
    u64 value = 0U;
    if (r == NPH_RESULT_OK) {
        // Each binding waits for the one before, so they complete in order
        // and the timeline only rises.
        value = nph_atomic_load_u64(&dev->sparse_value, NPH_MEMORY_ORDER_RELAXED) + 1U;
        const u64 previous = value - 1U;
        VkTimelineSemaphoreSubmitInfo ti;
        memset(&ti, 0, sizeof ti);
        ti.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        ti.waitSemaphoreValueCount = previous != 0U ? 1U : 0U;
        ti.pWaitSemaphoreValues = &previous;
        ti.signalSemaphoreValueCount = 1U;
        ti.pSignalSemaphoreValues = &value;
        VkBindSparseInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
        bi.pNext = &ti;
        bi.waitSemaphoreCount = ti.waitSemaphoreValueCount;
        bi.pWaitSemaphores = &dev->sparse_timeline->vk;
        bi.imageOpaqueBindCount = opaque_infos;
        bi.pImageOpaqueBinds = s.opaque_infos;
        bi.imageBindCount = image_infos;
        bi.pImageBinds = s.image_infos;
        bi.signalSemaphoreCount = 1U;
        bi.pSignalSemaphores = &dev->sparse_timeline->vk;
        const VkResult vr = vkQueueBindSparse(q->vk, 1U, &bi, VK_NULL_HANDLE);
        if (vr != VK_SUCCESS) {
            r = gpu_vk_fail(dev, vr, "vkQueueBindSparse");
        } else {
            nph_atomic_store_u64(&dev->sparse_value, value, NPH_MEMORY_ORDER_RELEASE);
            nph_atomic_store_u64(&dev->sparse_timeline->submitted, value, NPH_MEMORY_ORDER_RELEASE);
        }
    }
    nph_mutex_unlock(&q->lock);
    if (r == NPH_RESULT_OK) {
        // The next flush's copies, into the pages just bound, wait for it.
        gpu_upload_lock(dev);
        dev->upload.sparse_wait = nph_max_u64(dev->upload.sparse_wait, value);
        gpu_upload_unlock(dev);
        if (out_value != NULL) {
            *out_value = value;
        }
    }
    return r;
}

u64 nph_gpu_sparse_submitted(const NphGpuDevice *dev)
{
    NPH_ASSERT(dev != NULL);
    return nph_atomic_load_u64(&dev->sparse_value, NPH_MEMORY_ORDER_ACQUIRE);
}

u64 nph_gpu_sparse_completed(NphGpuDevice *dev)
{
    NPH_ASSERT(dev != NULL);
    return dev->sparse_timeline != NULL ? nph_gpu_timeline_value(dev, dev->sparse_timeline) : 0U;
}

NphResult nph_gpu_sparse_wait(NphGpuDevice *dev, u64 value, u64 timeout_ns)
{
    NPH_ASSERT(dev != NULL);
    if (value == 0U) {
        return NPH_RESULT_OK;
    }
    if (dev->sparse_timeline == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return nph_gpu_timeline_wait(dev, dev->sparse_timeline, value, timeout_ns);
}
