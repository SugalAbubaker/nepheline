// SPDX-License-Identifier: Apache-2.0
// Uploads through the staging ring and the transfer queue
// (docs/gpu.md "Uploads").
#include "gpu_internal.h"

// Staging offsets satisfy every copy alignment rule (texel blocks up to 16
// bytes, optimalBufferCopyOffsetAlignment up to 256 on known hardware).
#define STAGING_ALIGN 256U

NphResult gpu_upload_init(NphGpuDevice *dev)
{
    NphGpuUploadState *up = &dev->upload;
    nph_mutex_init(&up->lock);
    NphGpuBufferDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.size = dev->desc.staging_ring_bytes;
    desc.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    desc.memory = NPH_GPU_MEMORY_UPLOAD;
    desc.name = "staging ring";
    NPH_TRY(nph_gpu_buffer_create(dev, &desc, &up->staging));
    up->staging_mapped = (u8 *)nph_gpu_buffer_map(dev, up->staging);
    up->staging_size = desc.size;
    if (up->staging_mapped == NULL) {
        GPU_LOG_ERROR("the staging ring is not host-visible");
        return NPH_RESULT_UNSUPPORTED;
    }
    up->acquire_cap = 256U;
    up->acquires = NPH_ALLOC_ARRAY(dev->alloc, NphGpuUploadAcquire, up->acquire_cap);
    up->cmd_storage =
        NPH_ALLOC_ARRAY_ZEROED(dev->alloc, NphGpuCmd, (u64)2U * NPH_GPU_FRAMES_IN_FLIGHT);
    if (up->acquires == NULL || up->cmd_storage == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    return NPH_RESULT_OK;
}

void gpu_upload_shutdown(NphGpuDevice *dev)
{
    NphGpuUploadState *up = &dev->upload;
    if (up->cmd != NULL) {
        GPU_LOG_WARN("uploads pending at device destruction are dropped");
        nph_gpu_cmd_end(up->cmd);
        up->cmd = NULL;
    }
    if (!nph_gpu_buffer_is_null(up->staging)) {
        nph_gpu_buffer_destroy(dev, up->staging);
        up->staging = NPH_GPU_BUFFER_NULL;
    }
    if (up->acquires != NULL) {
        NPH_FREE_ARRAY(dev->alloc, NphGpuUploadAcquire, up->acquires, up->acquire_cap);
        up->acquires = NULL;
    }
    if (up->cmd_storage != NULL) {
        NPH_FREE_ARRAY(dev->alloc, NphGpuCmd, up->cmd_storage, (u64)2U * NPH_GPU_FRAMES_IN_FLIGHT);
        up->cmd_storage = NULL;
    }
    nph_mutex_destroy(&up->lock);
}

void gpu_upload_release_slot(NphGpuDevice *dev, u32 slot)
{
    NphGpuUploadState *up = &dev->upload;
    NphGpuFrameSlot *fs = &dev->frames[slot];
    nph_mutex_lock(&up->lock);
    NPH_ASSERT(fs->staging_bytes <= up->used);
    up->used -= fs->staging_bytes;
    fs->staging_bytes = 0U;
    if (up->used == 0U) {
        up->head = 0U;
        up->tail = 0U;
    }
    nph_mutex_unlock(&up->lock);
}

void gpu_upload_mark_frame_end(NphGpuDevice *dev, u32 slot)
{
    NPH_UNUSED(dev);
    NPH_UNUSED(slot);
}

// Allocates `size` bytes of ring space; false when the ring is too full.
static bool ring_alloc(NphGpuUploadState *up, u64 size, u64 *out_offset)
{
    size = nph_align_up_u64(size, STAGING_ALIGN);
    if (up->used + size > up->staging_size) {
        return false;
    }
    if (up->head + size <= up->staging_size) {
        *out_offset = up->head;
        up->head += size;
        up->used += size;
        up->batch_bytes += size;
        return true;
    }
    // Wrap: the tail of the ring is wasted until the frame that owns it
    // completes.
    const u64 waste = up->staging_size - up->head;
    if (up->used + waste + size > up->staging_size) {
        return false;
    }
    up->used += waste + size;
    up->batch_bytes += waste + size;
    *out_offset = 0U;
    up->head = size;
    return true;
}

// Whether the TRANSFER queue is the graphics queue (no transfer family, or
// desc.single_queue). Then the uploads are ordered against the
// surrounding graphics work by barriers in the upload command buffer
// itself: a pipeline barrier's scopes cover everything earlier and later
// in submission order on the queue, across command buffers. With a
// distinct transfer queue the timeline-semaphore waits in
// nph_gpu_upload_flush do that instead.
static bool shared_queue(const NphGpuDevice *dev)
{
    return dev->queues[NPH_GPU_QUEUE_TRANSFER] == dev->queues[NPH_GPU_QUEUE_GRAPHICS];
}

static void memory_barrier(NphGpuCmd *cmd, u32 src_stages, u32 src_access, u32 dst_stages,
                           u32 dst_access)
{
    NphGpuMemoryBarrier m;
    m.src_stages = src_stages;
    m.src_access = src_access;
    m.dst_stages = dst_stages;
    m.dst_access = dst_access;
    NphGpuBarrierDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.memory = &m;
    desc.memory_count = 1U;
    nph_gpu_cmd_barrier(cmd, &desc);
}

static NphResult ensure_batch(NphGpuDevice *dev)
{
    NphGpuUploadState *up = &dev->upload;
    if (up->cmd != NULL) {
        return NPH_RESULT_OK;
    }
    NphGpuCmdPool *pool = gpu_upload_pool(dev, dev->frame_slot, NPH_GPU_QUEUE_TRANSFER);
    if (pool->pool == VK_NULL_HANDLE) {
        NPH_TRY(gpu_cmd_pool_create(dev, pool, dev->queues[NPH_GPU_QUEUE_TRANSFER]->family));
    }
    NphGpuCmd *cmd = &up->cmd_storage[(u64)dev->frame_slot * 2U];
    NPH_TRY(gpu_cmd_begin_from_pool(dev, pool, cmd, NPH_GPU_QUEUE_TRANSFER, "uploads"));
    if (shared_queue(dev)) {
        // Whatever the queue did before with the destinations (reads as
        // well as writes) completes before the copies overwrite them.
        memory_barrier(cmd, (u32)NPH_GPU_STAGE_ALL,
                       (u32)NPH_GPU_ACCESS_MEMORY_READ | (u32)NPH_GPU_ACCESS_MEMORY_WRITE,
                       (u32)NPH_GPU_STAGE_TRANSFER,
                       (u32)NPH_GPU_ACCESS_TRANSFER_READ | (u32)NPH_GPU_ACCESS_TRANSFER_WRITE);
    }
    up->cmd = cmd;
    up->batch_bytes = 0U;
    up->batch_copies = 0U;
    return NPH_RESULT_OK;
}

// Finds staging space for `size` bytes: in the ring, or in a dedicated
// buffer that dies with the frame. Returns the buffer and offset to copy from.
static NphResult stage(NphGpuDevice *dev, const void *data, u64 size, NphGpuBuffer *out_buffer,
                       u64 *out_offset)
{
    NphGpuUploadState *up = &dev->upload;
    u64 offset = 0U;
    if (size <= up->staging_size && ring_alloc(up, size, &offset)) {
        memcpy(up->staging_mapped + offset, data, size);
        *out_buffer = up->staging;
        *out_offset = offset;
        return NPH_RESULT_OK;
    }
    NphGpuBufferDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.size = size;
    desc.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    desc.memory = NPH_GPU_MEMORY_UPLOAD;
    desc.name = "dedicated staging";
    NphGpuBuffer tmp = NPH_GPU_BUFFER_NULL;
    NPH_TRY(nph_gpu_buffer_create(dev, &desc, &tmp));
    void *mapped = nph_gpu_buffer_map(dev, tmp);
    if (mapped == NULL) {
        nph_gpu_buffer_destroy(dev, tmp);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memcpy(mapped, data, size);
    GPU_LOG_DEBUG("upload of %llu bytes exceeds the staging ring; using a dedicated buffer",
                  (unsigned long long)size);
    *out_buffer = tmp;
    *out_offset = 0U;
    return NPH_RESULT_OK;
}

NphResult nph_gpu_upload_buffer(NphGpuDevice *dev, NphGpuBuffer dst, u64 dst_offset,
                                const void *data, u64 size)
{
    NPH_ASSERT(dev != NULL && data != NULL);
    const NphGpuBufferSlot *slot = gpu_buffer(dev, dst);
    if (slot == NULL || size == 0U || dst_offset + size > slot->desc.size ||
        dst_offset + size < dst_offset) {
        GPU_LOG_ERROR("upload of %llu bytes at %llu does not fit buffer '%s'",
                      (unsigned long long)size, (unsigned long long)dst_offset,
                      slot != NULL ? slot->name : "?");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if ((slot->desc.usage & (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST) == 0U) {
        GPU_LOG_ERROR("buffer '%s' lacks TRANSFER_DST usage", slot->name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphGpuUploadState *up = &dev->upload;
    nph_mutex_lock(&up->lock);
    NphResult r = ensure_batch(dev);
    NphGpuBuffer src = NPH_GPU_BUFFER_NULL;
    u64 src_offset = 0U;
    if (r == NPH_RESULT_OK) {
        r = stage(dev, data, size, &src, &src_offset);
    }
    if (r == NPH_RESULT_OK) {
        NphGpuBufferCopy region;
        region.src_offset = src_offset;
        region.dst_offset = dst_offset;
        region.size = size;
        nph_gpu_cmd_copy_buffer(up->cmd, src, dst, &region, 1U);
        ++up->batch_copies;
        NphGpuBufferSlot *dst_slot = gpu_buffer(dev, dst);
        dst_slot->state.stages = (u32)NPH_GPU_STAGE_TRANSFER;
        dst_slot->state.access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
        dst_slot->state.queue = (u32)NPH_GPU_QUEUE_TRANSFER;
        if (src.value != up->staging.value) {
            nph_gpu_buffer_destroy(dev, src);
        }
    }
    nph_mutex_unlock(&up->lock);
    return r;
}

static NphResult push_acquire(NphGpuDevice *dev, const NphGpuUploadAcquire *a)
{
    NphGpuUploadState *up = &dev->upload;
    if (up->acquire_count == up->acquire_cap) {
        const u32 new_cap = up->acquire_cap * 2U;
        NphGpuUploadAcquire *grown = (NphGpuUploadAcquire *)NPH_REALLOC(
            dev->alloc, up->acquires, (u64)up->acquire_cap * sizeof(NphGpuUploadAcquire),
            (u64)new_cap * sizeof(NphGpuUploadAcquire), NPH_ALIGNOF(NphGpuUploadAcquire));
        if (grown == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        up->acquires = grown;
        up->acquire_cap = new_cap;
    }
    up->acquires[up->acquire_count++] = *a;
    return NPH_RESULT_OK;
}

// The part of a mip an upload writes: everything (`whole`), or a rectangle.
typedef struct UploadRect {
    bool whole;
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} UploadRect;

static NphResult upload_image(NphGpuDevice *dev, NphGpuImage dst, u32 mip, u32 base_layer,
                              u32 layer_count, const UploadRect *rect, const void *data, u64 size);

// The transfer queue shares the graphics queue's family: no image changes
// owner between them (a device with one family makes every image
// exclusive).
static bool one_queue_family(const NphGpuDevice *dev)
{
    return dev->queues[NPH_GPU_QUEUE_TRANSFER]->family ==
           dev->queues[NPH_GPU_QUEUE_GRAPHICS]->family;
}

NphResult nph_gpu_upload_image(NphGpuDevice *dev, NphGpuImage dst, u32 mip, u32 base_layer,
                               u32 layer_count, const void *data, u64 size)
{
    NPH_ASSERT(dev != NULL && data != NULL);
    NphGpuImageSlot *slot = gpu_image(dev, dst);
    if (slot == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (layer_count == 0U) {
        layer_count = 1U;
    }
    const u64 level_bytes = nph_gpu_format_level_bytes(
        slot->desc.format, nph_gpu_mip_extent(slot->desc.width, mip),
        nph_gpu_mip_extent(slot->desc.height, mip), nph_gpu_mip_extent(slot->desc.depth, mip));
    if (mip >= slot->desc.mip_count || base_layer + layer_count > slot->desc.layer_count ||
        size != level_bytes * layer_count) {
        GPU_LOG_ERROR("upload to image '%s' mip %u layers %u+%u: %llu bytes given, %llu expected",
                      slot->name, mip, base_layer, layer_count, (unsigned long long)size,
                      (unsigned long long)(level_bytes * layer_count));
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const UploadRect whole = {true, 0U, 0U, 0U, 0U};
    return upload_image(dev, dst, mip, base_layer, layer_count, &whole, data, size);
}

NphResult nph_gpu_upload_image_region(NphGpuDevice *dev, NphGpuImage dst, u32 mip, u32 layer, u32 x,
                                      u32 y, u32 width, u32 height, const void *data, u64 size)
{
    NPH_ASSERT(dev != NULL && data != NULL);
    NphGpuImageSlot *slot = gpu_image(dev, dst);
    if (slot == NULL || mip >= slot->desc.mip_count || layer >= slot->desc.layer_count) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphGpuFormatInfo *info = nph_gpu_format_info(slot->desc.format);
    const u32 mip_w = nph_gpu_mip_extent(slot->desc.width, mip);
    const u32 mip_h = nph_gpu_mip_extent(slot->desc.height, mip);
    const u32 block = info->block_width;
    // A rectangle may end at the mip's edge with a partial block.
    const bool aligned = x % block == 0U && y % block == 0U &&
                         (width % block == 0U || x + width == mip_w) &&
                         (height % block == 0U || y + height == mip_h);
    const bool inside = width > 0U && height > 0U && x <= mip_w && y <= mip_h &&
                        width <= mip_w - x && height <= mip_h - y &&
                        nph_gpu_mip_extent(slot->desc.depth, mip) == 1U;
    const u64 bytes = nph_gpu_format_level_bytes(slot->desc.format, width, height, 1U);
    if (!aligned || !inside || size != bytes) {
        GPU_LOG_ERROR("upload to image '%s' mip %u layer %u: rectangle %u,%u %ux%u with %llu "
                      "bytes (the mip is %ux%u, the rectangle needs %llu)",
                      slot->name, mip, layer, x, y, width, height, (unsigned long long)size, mip_w,
                      mip_h, (unsigned long long)bytes);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (slot->exclusive != 0 && !one_queue_family(dev)) {
        GPU_LOG_ERROR("image '%s' is exclusive (an attachment) and the transfer queue has a family "
                      "of its own: a region upload cannot keep the texels around it",
                      slot->name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const UploadRect rect = {false, x, y, width, height};
    return upload_image(dev, dst, mip, layer, 1U, &rect, data, size);
}

static NphResult upload_image(NphGpuDevice *dev, NphGpuImage dst, u32 mip, u32 base_layer,
                              u32 layer_count, const UploadRect *rect, const void *data, u64 size)
{
    NphGpuImageSlot *slot = gpu_image(dev, dst);
    if ((slot->desc.usage & (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST) == 0U) {
        GPU_LOG_ERROR("image '%s' lacks TRANSFER_DST usage", slot->name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphGpuLayout final_layout = (slot->desc.usage & (u32)NPH_GPU_IMAGE_USAGE_SAMPLED) != 0U
                                          ? NPH_GPU_LAYOUT_SHADER_READ_ONLY
                                          : NPH_GPU_LAYOUT_GENERAL;
    NphGpuUploadState *up = &dev->upload;
    nph_mutex_lock(&up->lock);
    NphResult r = ensure_batch(dev);
    NphGpuBuffer src = NPH_GPU_BUFFER_NULL;
    u64 src_offset = 0U;
    if (r == NPH_RESULT_OK) {
        r = stage(dev, data, size, &src, &src_offset);
    }
    if (r == NPH_RESULT_OK) {
        NphGpuCmd *cmd = up->cmd;
        // Layouts are tracked per mip, so every layer of the mip makes the
        // round trip even when the copy touches a few. Shared images keep
        // the layers the upload does not touch: the transition starts from
        // the tracked layout. Exclusive images (attachments) owned by
        // another family cannot be acquired here without a release on the
        // owning queue, so their old contents are discarded: an upload into
        // an attachment replaces the whole mip. On one family nothing
        // changes owner, and a region upload keeps the rest of the mip as
        // for shared images (a whole upload still discards).
        const bool exclusive = slot->exclusive != 0;
        const bool foreign = exclusive && !one_queue_family(dev);
        const bool discard = foreign || (exclusive && rect->whole);
        NphGpuImageBarrier b;
        memset(&b, 0, sizeof b);
        b.image = dst;
        b.src_stages = (u32)NPH_GPU_STAGE_ALL;
        b.src_access = discard ? (u32)NPH_GPU_ACCESS_NONE
                               : (u32)NPH_GPU_ACCESS_MEMORY_READ | (u32)NPH_GPU_ACCESS_MEMORY_WRITE;
        b.old_layout = discard ? NPH_GPU_LAYOUT_UNDEFINED : slot->state[mip].layout;
        b.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
        b.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
        b.new_layout = NPH_GPU_LAYOUT_TRANSFER_DST;
        b.base_mip = mip;
        b.mip_count = 1U;
        b.base_layer = 0U;
        b.layer_count = NPH_GPU_ALL_LAYERS;
        b.src_queue = NPH_GPU_QUEUE_TRANSFER;
        b.dst_queue = NPH_GPU_QUEUE_TRANSFER;
        NphGpuBarrierDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.images = &b;
        desc.image_count = 1U;
        nph_gpu_cmd_barrier(cmd, &desc);

        NphGpuBufferImageCopy region;
        memset(&region, 0, sizeof region);
        region.buffer_offset = src_offset;
        region.mip = mip;
        region.base_layer = base_layer;
        region.layer_count = layer_count;
        if (!rect->whole) {
            region.x = (i32)rect->x;
            region.y = (i32)rect->y;
            region.width = rect->width;
            region.height = rect->height;
            region.depth = 1U;
        }
        nph_gpu_cmd_copy_buffer_to_image(cmd, src, dst, NPH_GPU_LAYOUT_TRANSFER_DST, &region, 1U);
        ++up->batch_copies;

        // Hand the mip to the graphics queue in its final layout: for an
        // exclusive image on another family a release here and a matching
        // acquire in the flush; otherwise a plain transition.
        b.src_stages = (u32)NPH_GPU_STAGE_TRANSFER;
        b.src_access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
        b.old_layout = NPH_GPU_LAYOUT_TRANSFER_DST;
        b.dst_stages = (u32)NPH_GPU_STAGE_ALL;
        b.dst_access = (u32)NPH_GPU_ACCESS_MEMORY_READ;
        b.new_layout = final_layout;
        b.dst_queue = NPH_GPU_QUEUE_GRAPHICS;
        nph_gpu_cmd_barrier(cmd, &desc);
        if (foreign) {
            NphGpuUploadAcquire a;
            a.image = dst;
            a.mip = mip;
            a.base_layer = base_layer;
            a.layer_count = layer_count;
            a.final_layout = final_layout;
            r = push_acquire(dev, &a);
        }
        if (src.value != up->staging.value) {
            nph_gpu_buffer_destroy(dev, src);
        }
    }
    nph_mutex_unlock(&up->lock);
    return r;
}

static NphResult record_acquires(NphGpuDevice *dev, NphGpuCmd *cmd)
{
    NphGpuUploadState *up = &dev->upload;
    for (u32 i = 0; i < up->acquire_count; ++i) {
        const NphGpuUploadAcquire *a = &up->acquires[i];
        if (!nph_gpu_image_is_valid(dev, a->image)) {
            continue; // destroyed before its upload landed
        }
        NphGpuImageBarrier b;
        memset(&b, 0, sizeof b);
        b.image = a->image;
        b.src_stages = (u32)NPH_GPU_STAGE_TRANSFER;
        b.src_access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
        b.old_layout = NPH_GPU_LAYOUT_TRANSFER_DST;
        b.dst_stages = (u32)NPH_GPU_STAGE_ALL;
        b.dst_access = (u32)NPH_GPU_ACCESS_MEMORY_READ;
        b.new_layout = a->final_layout;
        b.base_mip = a->mip;
        b.mip_count = 1U;
        b.base_layer = a->base_layer;
        b.layer_count = a->layer_count;
        b.src_queue = NPH_GPU_QUEUE_TRANSFER;
        b.dst_queue = NPH_GPU_QUEUE_GRAPHICS;
        NphGpuBarrierDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.images = &b;
        desc.image_count = 1U;
        nph_gpu_cmd_barrier(cmd, &desc);
    }
    up->acquire_count = 0U;
    return NPH_RESULT_OK;
}

NphResult nph_gpu_upload_flush(NphGpuDevice *dev, u64 *out_value)
{
    NPH_ASSERT(dev != NULL);
    if (out_value != NULL) {
        *out_value = 0U;
    }
    NphGpuUploadState *up = &dev->upload;
    nph_mutex_lock(&up->lock);
    if (up->cmd == NULL) {
        nph_mutex_unlock(&up->lock);
        return NPH_RESULT_OK;
    }
    NphGpuCmd *cmd = up->cmd;
    up->cmd = NULL;
    const bool distinct = !shared_queue(dev);
    if (!distinct) {
        // The copies are visible to everything the queue does next.
        memory_barrier(cmd, (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_WRITE,
                       (u32)NPH_GPU_STAGE_ALL,
                       (u32)NPH_GPU_ACCESS_MEMORY_READ | (u32)NPH_GPU_ACCESS_MEMORY_WRITE);
    }
    nph_gpu_cmd_end(cmd);
    // The transfer runs after the previous frame's graphics work, so an
    // upload into a resource that frame read cannot race it (CLAUDE.md
    // §10.6: the CPU is at most one frame ahead, so this costs nothing).
    NphGpuTimelineOp prev_frame;
    prev_frame.timeline = dev->queues[NPH_GPU_QUEUE_GRAPHICS]->timeline;
    prev_frame.value = dev->last_frame_graphics_value;
    prev_frame.stages = (u32)NPH_GPU_STAGE_ALL;
    NphGpuSubmitDesc submit;
    memset(&submit, 0, sizeof submit);
    submit.queue = NPH_GPU_QUEUE_TRANSFER;
    submit.cmds = &cmd;
    submit.cmd_count = 1U;
    if (prev_frame.value != 0U && distinct) {
        submit.waits = &prev_frame;
        submit.wait_count = 1U;
    }
    u64 value = 0U;
    NphResult r = nph_gpu_submit(dev, &submit, &value);
    u64 graphics_value = 0U;
    if (r == NPH_RESULT_OK && distinct) {
        // The graphics queue waits for the transfer and acquires the images;
        // every later graphics submit is ordered after this one.
        NphGpuCmdPool *pool = gpu_upload_pool(dev, dev->frame_slot, NPH_GPU_QUEUE_GRAPHICS);
        if (pool->pool == VK_NULL_HANDLE) {
            r = gpu_cmd_pool_create(dev, pool, dev->queues[NPH_GPU_QUEUE_GRAPHICS]->family);
        }
        NphGpuCmd *acquire = &up->cmd_storage[(u64)dev->frame_slot * 2U + 1U];
        if (r == NPH_RESULT_OK) {
            r = gpu_cmd_begin_from_pool(dev, pool, acquire, NPH_GPU_QUEUE_GRAPHICS,
                                        "upload acquire");
        }
        if (r == NPH_RESULT_OK) {
            r = record_acquires(dev, acquire);
            nph_gpu_cmd_end(acquire);
        }
        if (r == NPH_RESULT_OK) {
            NphGpuTimelineOp wait;
            wait.timeline = dev->queues[NPH_GPU_QUEUE_TRANSFER]->timeline;
            wait.value = value;
            wait.stages = (u32)NPH_GPU_STAGE_ALL;
            NphGpuSubmitDesc gs;
            memset(&gs, 0, sizeof gs);
            gs.queue = NPH_GPU_QUEUE_GRAPHICS;
            gs.cmds = &acquire;
            gs.cmd_count = 1U;
            gs.waits = &wait;
            gs.wait_count = 1U;
            r = nph_gpu_submit(dev, &gs, &graphics_value);
        }
    }
    up->acquire_count = 0U;
    up->last_flush_value = value;
    GPU_LOG_DEBUG("upload flush: %u copies, %llu staging bytes, transfer value %llu",
                  up->batch_copies, (unsigned long long)up->batch_bytes, (unsigned long long)value);
    if (dev->frame_active) {
        dev->frames[dev->frame_slot].staging_bytes += up->batch_bytes;
    } else if (r == NPH_RESULT_OK) {
        // Outside a frame the flush is synchronous: wait, then release the
        // staging space at once.
        r = nph_gpu_timeline_wait(dev, dev->queues[NPH_GPU_QUEUE_TRANSFER]->timeline, value,
                                  NPH_U64_MAX);
        if (r == NPH_RESULT_OK && distinct) {
            r = nph_gpu_timeline_wait(dev, dev->queues[NPH_GPU_QUEUE_GRAPHICS]->timeline,
                                      graphics_value, NPH_U64_MAX);
        }
        NPH_ASSERT(up->batch_bytes <= up->used);
        up->used -= up->batch_bytes;
        if (up->used == 0U) {
            up->head = 0U;
            up->tail = 0U;
        }
    }
    up->batch_bytes = 0U;
    up->batch_copies = 0U;
    nph_mutex_unlock(&up->lock);
    if (out_value != NULL) {
        *out_value = value;
    }
    return r;
}

u64 nph_gpu_upload_staging_free(const NphGpuDevice *dev)
{
    return dev->upload.staging_size - dev->upload.used;
}
