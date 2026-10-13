// SPDX-License-Identifier: Apache-2.0
// Frames in flight, per-thread command pools, deferred destruction, and
// the timestamp query pools (docs/gpu.md "Frames").
#include <nepheline/platform/thread.h>

#include "gpu_internal.h"

// ---------------------------------------------------------------------------
// Thread slots
// ---------------------------------------------------------------------------

// Each device gets a unique id so a thread's cached slot never outlives it.
static NphAtomicU32 g_device_ids = NPH_ATOMIC_U32_INIT(0U);

typedef struct ThreadSlotCache {
    u32 device_id;
    u32 slot;
} ThreadSlotCache;

static NPH_THREAD_LOCAL ThreadSlotCache t_slot_cache = {NPH_U32_MAX, 0U};

// The upload pools live past the last ordinary thread slot.
#define UPLOAD_THREAD_SLOT(dev) ((dev)->thread_cap)

static u32 thread_slot(NphGpuDevice *dev)
{
    const u32 device_id = dev->id;
    if (t_slot_cache.device_id == device_id) {
        return t_slot_cache.slot;
    }
    const u64 tid = nph_thread_current_id();
    nph_spin_lock_acquire(&dev->thread_lock);
    u32 slot = NPH_U32_MAX;
    const u32 count = nph_atomic_load_u32(&dev->thread_count, NPH_MEMORY_ORDER_RELAXED);
    for (u32 i = 0; i < count; ++i) {
        if (dev->thread_ids[i] == tid) {
            slot = i;
            break;
        }
    }
    if (slot == NPH_U32_MAX && count < dev->thread_cap) {
        dev->thread_ids[count] = tid;
        slot = count;
        nph_atomic_store_u32(&dev->thread_count, count + 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    nph_spin_lock_release(&dev->thread_lock);
    if (slot != NPH_U32_MAX) {
        t_slot_cache.device_id = device_id;
        t_slot_cache.slot = slot;
    }
    return slot;
}

// ---------------------------------------------------------------------------
// Command pools
// ---------------------------------------------------------------------------

NphResult gpu_cmd_pool_create(NphGpuDevice *dev, NphGpuCmdPool *pool, u32 family)
{
    VkCommandPoolCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ci.queueFamilyIndex = family;
    VK_TRY(dev, vkCreateCommandPool(dev->device, &ci, NULL, &pool->pool));
    pool->buffers =
        NPH_ALLOC_ARRAY_ZEROED(dev->alloc, VkCommandBuffer, dev->desc.max_commands_per_thread);
    if (pool->buffers == NULL) {
        vkDestroyCommandPool(dev->device, pool->pool, NULL);
        pool->pool = VK_NULL_HANDLE;
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    pool->allocated = 0U;
    pool->used = 0U;
    pool->dirty = 0;
    return NPH_RESULT_OK;
}

void gpu_cmd_pool_reset(NphGpuDevice *dev, NphGpuCmdPool *pool)
{
    if (pool->pool != VK_NULL_HANDLE && pool->dirty) {
        (void)vkResetCommandPool(dev->device, pool->pool, 0U);
        pool->dirty = 0;
    }
    pool->used = 0U;
}

void gpu_cmd_pool_destroy(NphGpuDevice *dev, NphGpuCmdPool *pool)
{
    if (pool->pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev->device, pool->pool, NULL);
        pool->pool = VK_NULL_HANDLE;
    }
    if (pool->buffers != NULL) {
        NPH_FREE_ARRAY(dev->alloc, VkCommandBuffer, pool->buffers,
                       dev->desc.max_commands_per_thread);
        pool->buffers = NULL;
    }
}

NphResult gpu_cmd_begin_from_pool(NphGpuDevice *dev, NphGpuCmdPool *pool, NphGpuCmd *cmd,
                                  NphGpuQueueType queue, const char *name)
{
    if (pool->used >= dev->desc.max_commands_per_thread) {
        GPU_LOG_ERROR("command buffer pool exhausted (%u per thread per frame); raise "
                      "max_commands_per_thread",
                      dev->desc.max_commands_per_thread);
        return NPH_RESULT_FULL;
    }
    if (pool->used == pool->allocated) {
        VkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1U;
        VK_TRY(dev, vkAllocateCommandBuffers(dev->device, &ai, &pool->buffers[pool->allocated]));
        ++pool->allocated;
    }
    VkCommandBuffer vk = pool->buffers[pool->used];
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_TRY(dev, vkBeginCommandBuffer(vk, &bi));
    ++pool->used;
    pool->dirty = 1;
    memset(cmd, 0, sizeof *cmd);
    cmd->dev = dev;
    cmd->vk = vk;
    cmd->queue = queue;
    cmd->family = dev->queues[queue]->family;
    cmd->frame_slot = dev->frame_slot;
    cmd->recording = 1;
    cmd->name = name;
    if (name != NULL) {
        gpu_set_name(dev, VK_OBJECT_TYPE_COMMAND_BUFFER, (u64)(uptr)vk, name);
        if (dev->has_debug_utils && vkCmdBeginDebugUtilsLabelEXT != NULL) {
            VkDebugUtilsLabelEXT label;
            memset(&label, 0, sizeof label);
            label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label.pLabelName = name;
            label.color[0] = 0.4F;
            label.color[1] = 0.6F;
            label.color[2] = 0.9F;
            label.color[3] = 1.0F;
            vkCmdBeginDebugUtilsLabelEXT(vk, &label);
        }
    }
    return NPH_RESULT_OK;
}

static NphGpuCmdPool *pool_for(NphGpuDevice *dev, u32 thread, u32 slot, u32 family_slot)
{
    return &dev->cmd_pools[((u64)thread * NPH_GPU_FRAMES_IN_FLIGHT + slot) * dev->family_count +
                           family_slot];
}

static NphGpuCmd *cmd_for(NphGpuDevice *dev, u32 thread, u32 slot, u32 family_slot, u32 index)
{
    return &dev->cmds[(((u64)thread * NPH_GPU_FRAMES_IN_FLIGHT + slot) * dev->family_count +
                       family_slot) *
                          dev->desc.max_commands_per_thread +
                      index];
}

NphResult nph_gpu_cmd_begin(NphGpuDevice *dev, NphGpuQueueType queue, const char *name,
                            NphGpuCmd **out)
{
    NPH_ASSERT(dev != NULL && out != NULL);
    NPH_ASSERT((u32)queue < (u32)NPH_GPU_QUEUE_COUNT);
    *out = NULL;
    if (dev->lost) {
        return NPH_RESULT_DEVICE_LOST;
    }
    const u32 thread = thread_slot(dev);
    if (thread == NPH_U32_MAX) {
        GPU_LOG_ERROR("more than %u threads record commands; raise max_command_threads",
                      dev->thread_cap);
        return NPH_RESULT_FULL;
    }
    const u32 family_slot = dev->queues[queue]->family_slot;
    NphGpuCmdPool *pool = pool_for(dev, thread, dev->frame_slot, family_slot);
    if (pool->pool == VK_NULL_HANDLE) {
        NPH_TRY(gpu_cmd_pool_create(dev, pool, dev->families[family_slot]));
    }
    NphGpuCmd *cmd = cmd_for(dev, thread, dev->frame_slot, family_slot, pool->used);
    NPH_TRY(gpu_cmd_begin_from_pool(dev, pool, cmd, queue, name));
    *out = cmd;
    return NPH_RESULT_OK;
}

void nph_gpu_cmd_end(NphGpuCmd *cmd)
{
    NPH_ASSERT(cmd != NULL && cmd->recording && !cmd->ended);
    NPH_ASSERT_MSG(!cmd->rendering, "nph_gpu_cmd_end inside begin/end rendering");
    NphGpuDevice *dev = cmd->dev;
    if (cmd->name != NULL && dev->has_debug_utils && vkCmdEndDebugUtilsLabelEXT != NULL) {
        vkCmdEndDebugUtilsLabelEXT(cmd->vk);
    }
    const VkResult r = vkEndCommandBuffer(cmd->vk);
    if (r != VK_SUCCESS) {
        (void)gpu_vk_fail(dev, r, "vkEndCommandBuffer");
    }
    cmd->recording = 0;
    cmd->ended = 1;
}

NphGpuQueueType nph_gpu_cmd_queue(const NphGpuCmd *cmd)
{
    return cmd->queue;
}

NphGpuDevice *nph_gpu_cmd_device(NphGpuCmd *cmd)
{
    return cmd->dev;
}

VkCommandBuffer nph_gpu_vk_command_buffer(const NphGpuCmd *cmd)
{
    return cmd->vk;
}

// ---------------------------------------------------------------------------
// Deferred destruction
// ---------------------------------------------------------------------------

void gpu_defer(NphGpuDevice *dev, const NphGpuDeferred *entry)
{
    NphGpuFrameSlot *fs = &dev->frames[dev->frame_slot];
    nph_spin_lock_acquire(&fs->deferred_lock);
    if (fs->deferred_count == fs->deferred_cap) {
        const u32 new_cap = fs->deferred_cap * 2U;
        NphGpuDeferred *grown = (NphGpuDeferred *)NPH_REALLOC(
            dev->alloc, fs->deferred, (u64)fs->deferred_cap * sizeof(NphGpuDeferred),
            (u64)new_cap * sizeof(NphGpuDeferred), NPH_ALIGNOF(NphGpuDeferred));
        if (grown == NULL) {
            nph_spin_lock_release(&fs->deferred_lock);
            GPU_LOG_ERROR("deferred destruction queue full and could not grow; leaking an object");
            return;
        }
        GPU_LOG_WARN("deferred destruction queue grew to %u entries; raise "
                     "deferred_destroy_capacity",
                     new_cap);
        fs->deferred = grown;
        fs->deferred_cap = new_cap;
    }
    fs->deferred[fs->deferred_count++] = *entry;
    nph_spin_lock_release(&fs->deferred_lock);
}

void gpu_defer_handle(NphGpuDevice *dev, NphGpuDeferredType type, void *handle)
{
    if (handle == NULL) {
        return;
    }
    NphGpuDeferred e;
    e.type = (u32)type;
    e.index = 0U;
    e.handle = handle;
    e.alloc = NULL;
    gpu_defer(dev, &e);
}

void gpu_run_deferred(NphGpuDevice *dev, u32 slot)
{
    NphGpuFrameSlot *fs = &dev->frames[slot];
    nph_spin_lock_acquire(&fs->deferred_lock);
    NphGpuDeferred *entries = fs->deferred;
    const u32 count = fs->deferred_count;
    fs->deferred_count = 0U;
    nph_spin_lock_release(&fs->deferred_lock);
    // The entries are processed without the lock; nothing else reads them
    // and new ones go to the current slot, which is `slot` only when this
    // runs from wait_idle, where no other thread is destroying.
    for (u32 i = 0; i < count; ++i) {
        const NphGpuDeferred *e = &entries[i];
        switch ((NphGpuDeferredType)e->type) {
        case NPH_GPU_DEFERRED_BUFFER:
            if (e->alloc != NULL) {
                nph_gpu_alloc_destroy_buffer(dev->vma, (VkBuffer)e->handle, e->alloc);
            } else {
                vkDestroyBuffer(dev->device, (VkBuffer)e->handle, NULL);
            }
            break;
        case NPH_GPU_DEFERRED_IMAGE:
            if (e->alloc != NULL) {
                nph_gpu_alloc_destroy_image(dev->vma, (VkImage)e->handle, e->alloc);
            } else if (e->handle != NULL) {
                vkDestroyImage(dev->device, (VkImage)e->handle, NULL);
            }
            break;
        case NPH_GPU_DEFERRED_IMAGE_VIEW:
            vkDestroyImageView(dev->device, (VkImageView)e->handle, NULL);
            break;
        case NPH_GPU_DEFERRED_SAMPLER:
            vkDestroySampler(dev->device, (VkSampler)e->handle, NULL);
            break;
        case NPH_GPU_DEFERRED_SHADER_MODULE:
            vkDestroyShaderModule(dev->device, (VkShaderModule)e->handle, NULL);
            break;
        case NPH_GPU_DEFERRED_PIPELINE:
            vkDestroyPipeline(dev->device, (VkPipeline)e->handle, NULL);
            break;
        case NPH_GPU_DEFERRED_SEMAPHORE:
            vkDestroySemaphore(dev->device, (VkSemaphore)e->handle, NULL);
            break;
        case NPH_GPU_DEFERRED_SWAPCHAIN:
            vkDestroySwapchainKHR(dev->device, (VkSwapchainKHR)e->handle, NULL);
            break;
        case NPH_GPU_DEFERRED_BINDLESS_SAMPLED:
        case NPH_GPU_DEFERRED_BINDLESS_STORAGE:
        case NPH_GPU_DEFERRED_BINDLESS_SAMPLER:
            gpu_bindless_free(dev, (NphGpuDeferredType)e->type, e->index);
            break;
        case NPH_GPU_DEFERRED_ALLOCATION:
            nph_gpu_alloc_free(dev->vma, e->alloc);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

NphResult gpu_frames_init(NphGpuDevice *dev)
{
    NphAllocator *alloc = dev->alloc;
    dev->thread_cap = dev->desc.max_command_threads;
    dev->thread_ids = NPH_ALLOC_ARRAY_ZEROED(alloc, u64, dev->thread_cap);
    if (dev->thread_ids == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    dev->id = nph_atomic_fetch_add_u32(&g_device_ids, 1U, NPH_MEMORY_ORDER_RELAXED) + 1U;
    // Pools for every thread slot plus the upload slot.
    const u64 pool_count =
        (u64)(dev->thread_cap + 1U) * NPH_GPU_FRAMES_IN_FLIGHT * dev->family_count;
    dev->cmd_pools = NPH_ALLOC_ARRAY_ZEROED(alloc, NphGpuCmdPool, pool_count);
    dev->cmds =
        NPH_ALLOC_ARRAY_ZEROED(alloc, NphGpuCmd, pool_count * dev->desc.max_commands_per_thread);
    if (dev->cmd_pools == NULL || dev->cmds == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        NphGpuFrameSlot *fs = &dev->frames[i];
        fs->frame_index = NPH_U64_MAX;
        fs->deferred_cap = dev->desc.deferred_destroy_capacity;
        fs->deferred = NPH_ALLOC_ARRAY(alloc, NphGpuDeferred, fs->deferred_cap);
        fs->query_cap = dev->desc.timestamp_queries_per_frame;
        fs->query_results = NPH_ALLOC_ARRAY_ZEROED(alloc, u64, fs->query_cap);
        if (fs->deferred == NULL || fs->query_results == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        VkQueryPoolCreateInfo qi;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = fs->query_cap;
        VK_TRY(dev, vkCreateQueryPool(dev->device, &qi, NULL, &fs->query_pool));
        vkResetQueryPool(dev->device, fs->query_pool, 0U, fs->query_cap);
        gpu_set_name(dev, VK_OBJECT_TYPE_QUERY_POOL, (u64)(uptr)fs->query_pool,
                     i == 0U ? "timestamps frame slot 0" : "timestamps frame slot 1");
    }
    return NPH_RESULT_OK;
}

void gpu_frames_shutdown(NphGpuDevice *dev)
{
    NphAllocator *alloc = dev->alloc;
    if (dev->cmd_pools != NULL) {
        const u64 pool_count =
            (u64)(dev->thread_cap + 1U) * NPH_GPU_FRAMES_IN_FLIGHT * dev->family_count;
        for (u64 i = 0; i < pool_count; ++i) {
            gpu_cmd_pool_destroy(dev, &dev->cmd_pools[i]);
        }
        NPH_FREE_ARRAY(alloc, NphGpuCmdPool, dev->cmd_pools, pool_count);
        dev->cmd_pools = NULL;
        if (dev->cmds != NULL) {
            NPH_FREE_ARRAY(alloc, NphGpuCmd, dev->cmds,
                           pool_count * dev->desc.max_commands_per_thread);
            dev->cmds = NULL;
        }
    }
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        NphGpuFrameSlot *fs = &dev->frames[i];
        if (fs->query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(dev->device, fs->query_pool, NULL);
            fs->query_pool = VK_NULL_HANDLE;
        }
        if (fs->query_results != NULL) {
            NPH_FREE_ARRAY(alloc, u64, fs->query_results, fs->query_cap);
            fs->query_results = NULL;
        }
        if (fs->deferred != NULL) {
            NPH_ASSERT(fs->deferred_count == 0U);
            NPH_FREE_ARRAY(alloc, NphGpuDeferred, fs->deferred, fs->deferred_cap);
            fs->deferred = NULL;
        }
    }
    if (dev->thread_ids != NULL) {
        NPH_FREE_ARRAY(alloc, u64, dev->thread_ids, dev->thread_cap);
        dev->thread_ids = NULL;
    }
}

static void read_timestamps(NphGpuDevice *dev, NphGpuFrameSlot *fs)
{
    const u32 used =
        nph_min_u32(nph_atomic_load_u32(&fs->query_next, NPH_MEMORY_ORDER_RELAXED), fs->query_cap);
    fs->query_result_count = 0U;
    fs->query_results_valid = 0;
    if (used == 0U || fs->frame_index == NPH_U64_MAX) {
        return;
    }
    // Two u64 per query: value and availability. Unavailable queries (a
    // command buffer recorded but never submitted) read as 0.
    u64 *scratch = NPH_ALLOC_ARRAY(dev->alloc, u64, (u64)used * 2U);
    if (scratch == NULL) {
        return;
    }
    const VkResult r = vkGetQueryPoolResults(
        dev->device, fs->query_pool, 0U, used, (size_t)used * 2U * sizeof(u64), scratch,
        2U * sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r == VK_SUCCESS || r == VK_NOT_READY) {
        for (u32 i = 0; i < used; ++i) {
            fs->query_results[i] = scratch[(u64)i * 2U + 1U] != 0U ? scratch[(u64)i * 2U] : 0U;
        }
        fs->query_result_count = used;
        fs->query_results_valid = 1;
        fs->query_frame_index = fs->frame_index;
    }
    NPH_FREE_ARRAY(dev->alloc, u64, scratch, (u64)used * 2U);
}

void gpu_frames_recycle_slot(NphGpuDevice *dev, u32 slot)
{
    for (u32 t = 0; t <= dev->thread_cap; ++t) {
        for (u32 f = 0; f < dev->family_count; ++f) {
            gpu_cmd_pool_reset(dev, pool_for(dev, t, slot, f));
        }
    }
    NphGpuFrameSlot *fs = &dev->frames[slot];
    read_timestamps(dev, fs);
    vkResetQueryPool(dev->device, fs->query_pool, 0U, fs->query_cap);
    nph_atomic_store_u32(&fs->query_next, 0U, NPH_MEMORY_ORDER_RELAXED);
}

NphResult nph_gpu_frame_begin(NphGpuDevice *dev)
{
    NPH_ASSERT(dev != NULL);
    NPH_ASSERT_MSG(!dev->frame_active, "nph_gpu_frame_begin: a frame is already active");
    if (dev->frame_active) {
        GPU_LOG_ERROR("nph_gpu_frame_begin: a frame is already active");
        return NPH_RESULT_INVALID_STATE;
    }
    if (dev->lost) {
        return NPH_RESULT_DEVICE_LOST;
    }
    const u64 next = dev->any_frame_begun ? dev->frame_index + 1U : 0U;
    const u32 slot = (u32)(next % NPH_GPU_FRAMES_IN_FLIGHT);
    NphGpuFrameSlot *fs = &dev->frames[slot];

    // Wait for the frame that last used this slot, or, before the first
    // frame, for everything submitted outside frames.
    VkSemaphore sems[NPH_GPU_MAX_QUEUES];
    u64 values[NPH_GPU_MAX_QUEUES];
    u32 n = 0U;
    for (u32 q = 0; q < dev->queue_count; ++q) {
        NphGpuQueue *queue = &dev->queue_storage[q];
        const u64 value =
            fs->frame_index != NPH_U64_MAX
                ? fs->queue_values[q]
                : nph_atomic_load_u64(&queue->timeline->submitted, NPH_MEMORY_ORDER_ACQUIRE);
        if (value != 0U) {
            sems[n] = queue->timeline->vk;
            values[n] = value;
            ++n;
        }
    }
    if (n != 0U) {
        VkSemaphoreWaitInfo wi;
        memset(&wi, 0, sizeof wi);
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = n;
        wi.pSemaphores = sems;
        wi.pValues = values;
        const VkResult r = vkWaitSemaphores(dev->device, &wi, NPH_U64_MAX);
        if (r != VK_SUCCESS) {
            return gpu_vk_fail(dev, r, "vkWaitSemaphores(frame)");
        }
    }

    dev->frame_index = next;
    dev->frame_slot = slot;
    dev->frame_active = 1;
    dev->any_frame_begun = 1;
    gpu_upload_release_slot(dev, slot);
    gpu_run_deferred(dev, slot);
    gpu_frames_recycle_slot(dev, slot);
    fs->frame_index = next;
    for (u32 q = 0; q < NPH_GPU_MAX_QUEUES; ++q) {
        fs->queue_values[q] = 0U;
    }
    nph_gpu_alloc_set_frame(dev->vma, (u32)next);
    return NPH_RESULT_OK;
}

NphResult nph_gpu_frame_end(NphGpuDevice *dev)
{
    NPH_ASSERT(dev != NULL);
    NPH_ASSERT_MSG(dev->frame_active, "nph_gpu_frame_end without a frame");
    if (!dev->frame_active) {
        GPU_LOG_ERROR("nph_gpu_frame_end: no frame is active");
        return NPH_RESULT_INVALID_STATE;
    }
    NphResult r = nph_gpu_upload_flush(dev, NULL);
    NphGpuFrameSlot *fs = &dev->frames[dev->frame_slot];
    for (u32 q = 0; q < dev->queue_count; ++q) {
        fs->queue_values[q] = nph_atomic_load_u64(&dev->queue_storage[q].timeline->submitted,
                                                  NPH_MEMORY_ORDER_ACQUIRE);
    }
    gpu_upload_mark_frame_end(dev, dev->frame_slot);
    dev->last_frame_graphics_value = fs->queue_values[0];
    dev->frame_active = 0;
    if (dev->lost) {
        r = NPH_RESULT_DEVICE_LOST;
    }
    return r;
}

u64 nph_gpu_frame_index(const NphGpuDevice *dev)
{
    return dev->frame_index;
}

u32 nph_gpu_frame_slot(const NphGpuDevice *dev)
{
    return dev->frame_slot;
}

bool nph_gpu_frame_is_active(const NphGpuDevice *dev)
{
    return dev->frame_active != 0;
}

bool nph_gpu_frame_timestamps(const NphGpuDevice *dev, u64 *out_frame_index, const u64 **out_ticks,
                              u32 *out_count)
{
    const NphGpuFrameSlot *fs = &dev->frames[dev->frame_slot];
    if (!fs->query_results_valid) {
        return false;
    }
    if (out_frame_index != NULL) {
        *out_frame_index = fs->query_frame_index;
    }
    if (out_ticks != NULL) {
        *out_ticks = fs->query_results;
    }
    if (out_count != NULL) {
        *out_count = fs->query_result_count;
    }
    return true;
}

u32 nph_gpu_cmd_write_timestamp(NphGpuCmd *cmd, u32 stage)
{
    NPH_ASSERT(cmd != NULL && cmd->recording);
    NphGpuDevice *dev = cmd->dev;
    if (!dev->queues[cmd->queue]->timestamps) {
        return NPH_GPU_INVALID_INDEX;
    }
    NphGpuFrameSlot *fs = &dev->frames[cmd->frame_slot];
    const u32 index = nph_atomic_fetch_add_u32(&fs->query_next, 1U, NPH_MEMORY_ORDER_RELAXED);
    if (index >= fs->query_cap) {
        return NPH_GPU_INVALID_INDEX;
    }
    const VkPipelineStageFlags2 vk_stage =
        stage == 0U ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : gpu_vk_stages(stage);
    vkCmdWriteTimestamp2(cmd->vk, vk_stage, fs->query_pool, index);
    return index;
}

// The upload pools, reserved behind the ordinary thread slots.
NphGpuCmdPool *gpu_upload_pool(NphGpuDevice *dev, u32 slot, NphGpuQueueType queue)
{
    return pool_for(dev, UPLOAD_THREAD_SLOT(dev), slot, dev->queues[queue]->family_slot);
}
