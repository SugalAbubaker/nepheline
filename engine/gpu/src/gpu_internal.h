// SPDX-License-Identifier: Apache-2.0
#pragma once

// Private structures of the gpu module. Only engine/gpu/src includes this.

#include <nepheline/core/assert.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/handle.h>
#include <nepheline/gpu/render_graph.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/gpu/vk.h>
#include <nepheline/platform/thread.h>

#include "gpu_alloc.h"

#include <string.h>

#define NPH_GPU_NAME_MAX 64U

// ---------------------------------------------------------------------------
// Logging and error helpers
// ---------------------------------------------------------------------------

#define GPU_LOG_ERROR(...) NPH_LOG_ERROR(&NPH_LOG_GPU, __VA_ARGS__)
#define GPU_LOG_WARN(...)  NPH_LOG_WARN(&NPH_LOG_GPU, __VA_ARGS__)
#define GPU_LOG_INFO(...)  NPH_LOG_INFO(&NPH_LOG_GPU, __VA_ARGS__)
#define GPU_LOG_DEBUG(...) NPH_LOG_DEBUG(&NPH_LOG_GPU, __VA_ARGS__)

// Evaluates a Vulkan call; on failure logs it and returns the mapped
// NphResult from the enclosing function.
#define VK_TRY(dev, call)                                                                          \
    do {                                                                                           \
        const VkResult vk_try_r_ = (call);                                                         \
        if (vk_try_r_ != VK_SUCCESS) {                                                             \
            return gpu_vk_fail((dev), vk_try_r_, #call);                                           \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// Resource slots
// ---------------------------------------------------------------------------

typedef struct NphGpuMipState {
    NphGpuLayout layout;
    u32 stages; // NphGpuStage bits of the last use
    u32 access; // NphGpuAccess bits of the last use
    u32 queue;  // NphGpuQueueType that owns the mip (family ownership)
} NphGpuMipState;

typedef struct NphGpuBufferSlot {
    VkBuffer vk;
    NphGpuAllocation alloc;
    void *mapped;
    VkDeviceAddress address;
    NphGpuBufferDesc desc;
    NphGpuMipState state; // last recorded use (stages, access); layout unused
    b32 transient;
    // What the current upload batch has written to the buffer, as one
    // envelope [upload_lo, upload_hi) (upload.c orders overlapping copies).
    u64 upload_batch; // the batch the envelope belongs to (0: none)
    u64 upload_lo;
    u64 upload_hi;
    char name[NPH_GPU_NAME_MAX];
} NphGpuBufferSlot;

// Cached image views for attachments: (mip, layer range) -> view.
typedef struct NphGpuViewKey {
    u32 mip;
    u32 base_layer;
    u32 layer_count;
    VkImageView view;
} NphGpuViewKey;

typedef struct NphGpuImageSlot {
    VkImage vk;
    NphGpuAllocation alloc; // NULL for external (swapchain) and transient (block-bound) images
    VkImageView view;       // whole image, sampled slot; VK_NULL_HANDLE without SAMPLED usage
    VkImageView storage_views[NPH_GPU_MAX_MIPS];
    NphGpuViewKey *attachment_views; // grown on demand under the pool lock
    u32 attachment_view_count;
    u32 attachment_view_cap;
    u32 sampled_index;
    u32 storage_index[NPH_GPU_MAX_MIPS];
    NphGpuImageDesc desc; // normalised
    VkFormat vk_format;
    VkImageAspectFlags aspect;
    NphGpuMipState state[NPH_GPU_MAX_MIPS];
    b32 external;  // owned by a swapchain: the VkImage is not destroyed here
    b32 transient; // bound into a render-graph block: no allocation of its own
    b32 exclusive; // VK_SHARING_MODE_EXCLUSIVE (ownership transfers apply)
    char name[NPH_GPU_NAME_MAX];
} NphGpuImageSlot;

typedef struct NphGpuSamplerSlot {
    VkSampler vk;
    u32 index;
    NphGpuSamplerDesc desc;
    char name[NPH_GPU_NAME_MAX];
} NphGpuSamplerSlot;

typedef struct NphGpuShaderSlot {
    VkShaderModule vk;
    NphGpuShaderInfo info;
    u32 generation;
    char name[NPH_GPU_NAME_MAX];
} NphGpuShaderSlot;

typedef struct NphGpuPipelineSlot {
    VkPipeline vk;
    b32 compute;
    NphGpuGraphicsPipelineDesc gdesc;
    NphGpuComputePipelineDesc cdesc;
    u32 push_bytes;
    u32 generation;
    char name[NPH_GPU_NAME_MAX];
} NphGpuPipelineSlot;

// A handle pool plus its slot array and a lock.
typedef struct NphGpuPool {
    NphHandlePool handles;
    u8 *slots;
    u8 *live; // 1 for issued slots, for leak reports and forced shutdown
    u32 slot_size;
    NphSpinLock lock;
} NphGpuPool;

// ---------------------------------------------------------------------------
// Deferred destruction
// ---------------------------------------------------------------------------

typedef enum NphGpuDeferredType {
    NPH_GPU_DEFERRED_BUFFER = 0,       // handle = VkBuffer, alloc
    NPH_GPU_DEFERRED_IMAGE,            // handle = VkImage, alloc (NULL: external/transient)
    NPH_GPU_DEFERRED_IMAGE_VIEW,       // handle = VkImageView
    NPH_GPU_DEFERRED_SAMPLER,          // handle = VkSampler
    NPH_GPU_DEFERRED_SHADER_MODULE,    // handle = VkShaderModule
    NPH_GPU_DEFERRED_PIPELINE,         // handle = VkPipeline
    NPH_GPU_DEFERRED_SEMAPHORE,        // handle = VkSemaphore
    NPH_GPU_DEFERRED_SWAPCHAIN,        // handle = VkSwapchainKHR
    NPH_GPU_DEFERRED_BINDLESS_SAMPLED, // index
    NPH_GPU_DEFERRED_BINDLESS_STORAGE, // index
    NPH_GPU_DEFERRED_BINDLESS_SAMPLER, // index
    NPH_GPU_DEFERRED_ALLOCATION,       // alloc only (render-graph block)
} NphGpuDeferredType;

typedef struct NphGpuDeferred {
    u32 type;
    u32 index;
    void *handle; // the Vulkan handle (a pointer on 64-bit targets)
    NphGpuAllocation alloc;
} NphGpuDeferred;

// ---------------------------------------------------------------------------
// Queues, timelines, commands, frames
// ---------------------------------------------------------------------------

struct NphGpuTimeline {
    VkSemaphore vk;
    NphAtomicU64 submitted; // highest value a submit was told to signal
    b32 in_use;
    char name[NPH_GPU_NAME_MAX];
};

typedef struct NphGpuQueue {
    VkQueue vk;
    u32 family;
    u32 index;
    u32 family_slot; // index into NphGpuDevice.families
    NphGpuTimeline *timeline;
    NphMutex lock; // vkQueueSubmit / vkQueuePresentKHR are externally synchronised
    b32 timestamps;
} NphGpuQueue;

typedef struct NphGpuCmdPool {
    VkCommandPool pool;
    VkCommandBuffer *buffers;
    u32 allocated;
    u32 used;
    b32 dirty; // used since the last reset
} NphGpuCmdPool;

struct NphGpuCmd {
    NphGpuDevice *dev;
    VkCommandBuffer vk;
    NphGpuQueueType queue;
    u32 family;
    u32 frame_slot;
    b32 recording;
    b32 ended;
    b32 rendering;
    NphGpuPipeline pipeline;
    const char *name;
};

typedef struct NphGpuFrameSlot {
    u64 frame_index; // frame that last used the slot; NPH_U64_MAX before any
    u64 queue_values[NPH_GPU_MAX_QUEUES];
    NphGpuDeferred *deferred;
    u32 deferred_count;
    u32 deferred_cap;
    NphSpinLock deferred_lock;
    VkQueryPool query_pool;
    NphAtomicU32 query_next;
    u32 query_cap;
    u64 *query_results;
    u32 query_result_count;
    b32 query_results_valid;
    u64 query_frame_index;
    u64 staging_bytes; // staging ring bytes the frame's uploads hold until it completes
} NphGpuFrameSlot;

// ---------------------------------------------------------------------------
// Uploads
// ---------------------------------------------------------------------------

typedef struct NphGpuUploadAcquire {
    NphGpuImage image;
    u32 mip;
    u32 base_layer;
    u32 layer_count;
    NphGpuLayout final_layout;
} NphGpuUploadAcquire;

typedef struct NphGpuUploadState {
    NphMutex lock;
    NphGpuBuffer staging; // the ring
    u8 *staging_mapped;
    u64 staging_size;
    u64 head;                     // next byte to write (modulo size)
    u64 tail;                     // oldest byte still in use
    u64 used;                     // bytes between tail and head
    NphGpuCmd *cmd;               // transfer command buffer of the current batch
    NphGpuCmdPool *transfer_pool; // dedicated pools: the upload lock guards them
    NphGpuCmdPool *graphics_pool;
    NphGpuCmd *cmd_storage; // 2 per frame slot: transfer + acquire
    NphGpuUploadAcquire *acquires;
    u32 acquire_count;
    u32 acquire_cap;
    u64 last_flush_value; // transfer timeline value of the last flush
    u64 batch_bytes;
    u32 batch_copies;
    u64 batch_id; // counts batches from 1: tells envelopes of this batch from stale ones
} NphGpuUploadState;

// ---------------------------------------------------------------------------
// Bindless
// ---------------------------------------------------------------------------

typedef struct NphGpuIndexAllocator {
    u32 *free_stack;
    u32 free_count;
    u32 next;
    u32 capacity;
} NphGpuIndexAllocator;

typedef struct NphGpuBindless {
    VkDescriptorSetLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
    NphGpuIndexAllocator sampled;
    NphGpuIndexAllocator storage;
    NphGpuIndexAllocator samplers;
    NphSpinLock lock;
} NphGpuBindless;

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

struct NphGpuDevice {
    NphAllocator *alloc;
    NphGpuDeviceDesc desc; // defaults filled in; strings not owned
    NphGpuDeviceInfo info;
    u32 id; // unique per device created in this process (thread slot caches)

    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice phys;
    VkDevice device;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    b32 has_debug_utils;
    b32 has_swapchain_ext;
    b32 lost;

    NphGpuQueue queue_storage[NPH_GPU_MAX_QUEUES];
    NphGpuQueue *queues[NPH_GPU_QUEUE_COUNT]; // may alias
    u32 queue_count;                          // distinct queues
    u32 families[NPH_GPU_MAX_QUEUES];         // distinct families
    u32 family_count;

    NphGpuAllocatorHandle vma;

    NphGpuPool buffers;
    NphGpuPool images;
    NphGpuPool samplers;
    NphGpuPool shaders;
    NphGpuPool pipelines;

    NphGpuTimeline *timelines;
    u32 timeline_cap;
    NphSpinLock timeline_lock;

    NphGpuBindless bindless;

    VkPipelineCache pipeline_cache;
    b32 pipeline_cache_loaded;
    char *pipeline_cache_path;
    u64 pipeline_cache_path_len;

    // Frames
    u64 frame_index;
    u32 frame_slot;
    b32 frame_active;
    b32 any_frame_begun;
    u64 last_frame_graphics_value; // graphics timeline value at the last frame_end
    NphGpuFrameSlot frames[NPH_GPU_FRAMES_IN_FLIGHT];

    // Command pools: [thread][frame slot][family slot]; commands:
    // [thread][frame slot][queue][per thread]
    u32 thread_cap;
    u64 *thread_ids;
    NphAtomicU32 thread_count;
    NphSpinLock thread_lock;
    NphGpuCmdPool *cmd_pools;
    NphGpuCmd *cmds;

    NphGpuUploadState upload;

    NphAtomicU64 val_errors;
    NphAtomicU64 val_warnings;
    NphAtomicU64 val_perf;
    NphAtomicU64 val_infos;
};

// ---------------------------------------------------------------------------
// Internal helpers (vk_util.c, device.c, resources.c, frame.c)
// ---------------------------------------------------------------------------

// Logs a failed Vulkan call and maps it; marks the device lost on
// VK_ERROR_DEVICE_LOST.
NphResult gpu_vk_fail(NphGpuDevice *dev, VkResult r, const char *what);

void gpu_set_name(NphGpuDevice *dev, VkObjectType type, u64 handle, const char *name);
void gpu_copy_name(char *dst, const char *src, const char *fallback, u32 counter);

VkFormat gpu_vk_format(NphGpuFormat format);
VkImageLayout gpu_vk_layout(NphGpuLayout layout);
VkPipelineStageFlags2 gpu_vk_stages(u32 stages);
VkAccessFlags2 gpu_vk_access(u32 access);
VkImageAspectFlags gpu_vk_aspect(NphGpuFormat format);
VkFilter gpu_vk_filter(NphGpuFilter filter);
VkCompareOp gpu_vk_compare(NphGpuCompareOp op);
VkSamplerAddressMode gpu_vk_address_mode(NphGpuAddressMode mode);
VkAttachmentLoadOp gpu_vk_load_op(NphGpuLoadOp op);
VkAttachmentStoreOp gpu_vk_store_op(NphGpuStoreOp op);
VkPipelineStageFlags2 gpu_vk_queue_all_stages(NphGpuQueueType queue);
VkShaderStageFlagBits gpu_vk_shader_stage(NphGpuShaderStage stage);

// Pools
NphResult gpu_pool_init(NphGpuPool *pool, NphAllocator *alloc, u32 capacity, u32 slot_size);
void gpu_pool_free(NphGpuPool *pool, NphAllocator *alloc);
// Allocates a slot (zeroed). NPH_RESULT_FULL when exhausted. Takes the lock.
NphResult gpu_pool_alloc(NphGpuPool *pool, u32 *out_handle, void **out_slot);
// The slot of a valid handle, or NULL (asserted) for a stale or null one.
void *gpu_pool_get(const NphGpuPool *pool, u32 handle, const char *what);
// Releases the handle. Takes the lock. Returns false for a stale handle.
bool gpu_pool_release(NphGpuPool *pool, u32 handle);
u32 gpu_pool_live(const NphGpuPool *pool);
bool gpu_pool_next_live(const NphGpuPool *pool, u32 *index, void **out_slot);

NPH_INLINE NphGpuBufferSlot *gpu_buffer(const NphGpuDevice *dev, NphGpuBuffer h)
{
    return (NphGpuBufferSlot *)gpu_pool_get(&dev->buffers, h.value, "buffer");
}
NPH_INLINE NphGpuImageSlot *gpu_image(const NphGpuDevice *dev, NphGpuImage h)
{
    return (NphGpuImageSlot *)gpu_pool_get(&dev->images, h.value, "image");
}
NPH_INLINE NphGpuSamplerSlot *gpu_sampler(const NphGpuDevice *dev, NphGpuSampler h)
{
    return (NphGpuSamplerSlot *)gpu_pool_get(&dev->samplers, h.value, "sampler");
}
NPH_INLINE NphGpuShaderSlot *gpu_shader(const NphGpuDevice *dev, NphGpuShader h)
{
    return (NphGpuShaderSlot *)gpu_pool_get(&dev->shaders, h.value, "shader");
}
NPH_INLINE NphGpuPipelineSlot *gpu_pipeline(const NphGpuDevice *dev, NphGpuPipeline h)
{
    return (NphGpuPipelineSlot *)gpu_pool_get(&dev->pipelines, h.value, "pipeline");
}

// Deferred destruction: queues the entry on the current frame slot.
void gpu_defer(NphGpuDevice *dev, const NphGpuDeferred *entry);
void gpu_defer_handle(NphGpuDevice *dev, NphGpuDeferredType type, void *handle);
// Destroys every entry queued on `slot`.
void gpu_run_deferred(NphGpuDevice *dev, u32 slot);

// Bindless slots
NphResult gpu_bindless_init(NphGpuDevice *dev);
void gpu_bindless_shutdown(NphGpuDevice *dev);
u32 gpu_bindless_alloc_sampled(NphGpuDevice *dev, VkImageView view, VkImageLayout layout);
u32 gpu_bindless_alloc_storage(NphGpuDevice *dev, VkImageView view);
u32 gpu_bindless_alloc_sampler(NphGpuDevice *dev, VkSampler sampler);
void gpu_bindless_free(NphGpuDevice *dev, NphGpuDeferredType type, u32 index);

// Images
// Registers a swapchain image as an external image handle.
NphResult gpu_image_register_external(NphGpuDevice *dev, VkImage image, const NphGpuImageDesc *in,
                                      NphGpuImage *out);
// Memory requirements of a would-be image or buffer, without creating it
// (Vulkan 1.3 vkGetDevice*MemoryRequirements), for the render graph's
// transient packing.
NphResult gpu_image_memory_requirements(NphGpuDevice *dev, const NphGpuImageDesc *in,
                                        bool concurrent, VkMemoryRequirements *out);
NphResult gpu_buffer_memory_requirements(NphGpuDevice *dev, const NphGpuBufferDesc *desc,
                                         VkMemoryRequirements *out);
// Creates a VkImage (no memory) for a transient render-graph image to be
// bound into a block; `concurrent` shares it across queue families.
NphResult gpu_image_create_unbound(NphGpuDevice *dev, const NphGpuImageDesc *in, bool concurrent,
                                   NphGpuImage *out);
NphResult gpu_image_bind_and_finish(NphGpuDevice *dev, NphGpuImage image, NphGpuAllocation block,
                                    u64 offset);
NphResult gpu_buffer_create_unbound(NphGpuDevice *dev, const NphGpuBufferDesc *desc,
                                    NphGpuBuffer *out);
NphResult gpu_buffer_bind_and_finish(NphGpuDevice *dev, NphGpuBuffer buffer, NphGpuAllocation block,
                                     u64 offset);
// Attachment view for (mip, layers); created on first use.
VkImageView gpu_image_attachment_view(NphGpuDevice *dev, NphGpuImageSlot *slot, u32 mip,
                                      u32 base_layer, u32 layer_count);
void gpu_image_set_state(NphGpuImageSlot *slot, u32 base_mip, u32 mip_count, NphGpuLayout layout,
                         u32 stages, u32 access, u32 queue);
void gpu_image_normalize_desc(NphGpuImageDesc *desc);

// Commands and frames
NphResult gpu_frames_init(NphGpuDevice *dev);
void gpu_frames_shutdown(NphGpuDevice *dev);
// Resets every command pool of `slot` and the query pool.
void gpu_frames_recycle_slot(NphGpuDevice *dev, u32 slot);
// Begins a command buffer from a specific pool (uploads use dedicated pools).
NphResult gpu_cmd_begin_from_pool(NphGpuDevice *dev, NphGpuCmdPool *pool, NphGpuCmd *cmd,
                                  NphGpuQueueType queue, const char *name);
void gpu_cmd_pool_reset(NphGpuDevice *dev, NphGpuCmdPool *pool);
void gpu_cmd_pool_destroy(NphGpuDevice *dev, NphGpuCmdPool *pool);
NphResult gpu_cmd_pool_create(NphGpuDevice *dev, NphGpuCmdPool *pool, u32 family);
// Records a VkImageMemoryBarrier2 from the engine's description.
void gpu_fill_image_barrier(const NphGpuDevice *dev, const NphGpuImageBarrier *b,
                            const NphGpuImageSlot *slot, NphGpuQueueType cmd_queue,
                            VkImageMemoryBarrier2 *out);

// The command pools reserved for uploads (behind the thread slots).
NphGpuCmdPool *gpu_upload_pool(NphGpuDevice *dev, u32 slot, NphGpuQueueType queue);

// Swapchain: the acquire and present semaphores of the acquired image, for
// nph_gpu_submit; false when no image is acquired or the requested side
// was already consumed. `ops` is NphGpuSwapchainOp bits (0: both).
bool gpu_swapchain_submit_semaphores(NphGpuSwapchain *swapchain, u32 ops, VkSemaphore *out_acquire,
                                     VkSemaphore *out_present);

// Uploads
NphResult gpu_upload_init(NphGpuDevice *dev);
void gpu_upload_shutdown(NphGpuDevice *dev);
// Called by frame_begin when `slot` has completed: releases its staging.
void gpu_upload_release_slot(NphGpuDevice *dev, u32 slot);
void gpu_upload_mark_frame_end(NphGpuDevice *dev, u32 slot);

// Timelines
NphResult gpu_timeline_create_internal(NphGpuDevice *dev, u64 initial, const char *name,
                                       NphGpuTimeline **out);
void gpu_timeline_destroy_internal(NphGpuDevice *dev, NphGpuTimeline *tl, bool deferred);

// Pipeline cache
NphResult gpu_pipeline_cache_init(NphGpuDevice *dev);
void gpu_pipeline_cache_shutdown(NphGpuDevice *dev);
// Destroys every live pipeline/shader at device shutdown (leak path).
void gpu_pipelines_shutdown(NphGpuDevice *dev);
void gpu_resources_shutdown(NphGpuDevice *dev);

NPH_INLINE u32 gpu_mips_in_range(const NphGpuImageSlot *slot, u32 base_mip, u32 mip_count)
{
    const u32 total = slot->desc.mip_count;
    if (base_mip >= total) {
        return 0U;
    }
    if (mip_count == NPH_GPU_ALL_MIPS || mip_count == 0U || base_mip + mip_count > total) {
        return total - base_mip;
    }
    return mip_count;
}

NPH_INLINE u32 gpu_layers_in_range(const NphGpuImageSlot *slot, u32 base_layer, u32 layer_count)
{
    const u32 total = slot->desc.layer_count;
    if (base_layer >= total) {
        return 0U;
    }
    if (layer_count == NPH_GPU_ALL_LAYERS || layer_count == 0U ||
        base_layer + layer_count > total) {
        return total - base_layer;
    }
    return layer_count;
}
