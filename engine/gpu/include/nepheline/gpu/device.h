// SPDX-License-Identifier: Apache-2.0
#pragma once

// The GPU device (CLAUDE.md §8.5): the Vulkan instance, the selected
// physical device, the logical device with its queues, the memory
// allocator, the global bindless descriptor set, the per-frame pools, and
// the validation gate. Everything else in the module hangs off it.
//
// Lifecycle: nph_gpu_device_create loads the Vulkan loader through volk,
// creates an instance (with the validation and synchronisation-validation
// layers in Debug and Development builds, CLAUDE.md §1.3), scores the
// physical devices, requires the Vulkan 1.3 baseline (dynamic rendering,
// synchronization2, timeline semaphores, descriptor indexing, buffer
// device address) and records the optional features it finds (mesh
// shaders, ray queries, 64-bit image atomics, ...). A machine without a
// loader or without a conforming device gets NPH_RESULT_UNSUPPORTED with
// the reason logged, so tests and tools can skip rather than fail.
//
// Frames: nph_gpu_frame_begin / nph_gpu_frame_end bound the CPU to
// NPH_GPU_FRAMES_IN_FLIGHT frames ahead of the GPU. Beginning a frame waits
// until the frame that last used the same slot has finished on every
// queue, then recycles that slot's command pools, staging memory, query
// results, and deferred destructions. Resources destroyed during a frame
// are released when that frame completes, so a resource may be destroyed
// the moment the CPU no longer needs it.
//
// Submission is explicit (nph_gpu_submit) and synchronised with timeline
// semaphores only (CLAUDE.md §8.5 "transfer-queue uploads use
// timeline-semaphore sync"): every queue owns a timeline that each submit
// signals, and a submit may wait on and signal any number of timelines.
// Swapchain binary semaphores are handled by passing the swapchain.
//
// Costs the caller can see (§1.4): create allocates everything up front
// (handle pools, command pool tables, staging ring, query pools) from the
// allocator in the description; the only later allocations are Vulkan
// objects and GPU memory for the resources the caller creates. Submit
// takes a lock per queue (vkQueueSubmit is externally synchronised);
// resource creation takes a spin lock per pool.
//
// Thread-safety: create, destroy, wait_idle, frame_begin, and frame_end
// are NPH_MAIN_THREAD_ONLY (the thread that owns the frame). Submit, the
// timeline functions, and the statistics are NPH_THREADSAFE.

#include <nepheline/core/log.h>
#include <nepheline/gpu/types.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_CATEGORY_DECLARE(NPH_LOG_GPU);

typedef struct NphGpuDevice NphGpuDevice;
typedef struct NphGpuCmd NphGpuCmd;
typedef struct NphGpuSwapchain NphGpuSwapchain;
typedef struct NphGpuTimeline NphGpuTimeline;

// ---------------------------------------------------------------------------
// Description
// ---------------------------------------------------------------------------

typedef enum NphGpuToggle {
    NPH_GPU_TOGGLE_DEFAULT = 0, // follow the build configuration
    NPH_GPU_TOGGLE_OFF,
    NPH_GPU_TOGGLE_ON,
} NphGpuToggle;

#define NPH_GPU_DEVICE_AUTO NPH_U32_MAX

typedef struct NphGpuDeviceDesc {
    NphAllocator *alloc;  // required
    const char *app_name; // NULL: "Nepheline"
    // No surface support: tests, the cooker, benchmarks. Swapchains cannot
    // be created on a headless device.
    b32 headless;
    // The Khronos validation layer, and its synchronisation validation.
    // DEFAULT: on when NPH_GPU_VALIDATION_ENABLED (Debug, Development). A
    // request for ON on a machine without the layer is logged and ignored.
    NphGpuToggle validation;
    NphGpuToggle sync_validation;
    // Break into the debugger (when one is attached) on a validation error.
    b32 break_on_validation_error;
    // Physical device to use, by enumeration index; NPH_GPU_DEVICE_AUTO (and
    // 0 when the struct is zero-filled: see the note) picks the best score:
    // discrete > integrated > virtual > CPU, then device-local memory.
    // Zero-filled descriptions mean AUTO; pass device_index + 1 in
    // `device_index_plus_one` to force a device.
    u32 device_index_plus_one;
    // Pipeline cache file, loaded at create and written at destroy (and by
    // nph_gpu_pipeline_cache_save). NULL: no persistence.
    const char *pipeline_cache_path;
    // Capacities. Zero picks the default in parentheses. These are hard
    // limits: creation past them returns NPH_RESULT_FULL.
    u32 max_buffers;                 // (65536)
    u32 max_images;                  // (16384)
    u32 max_samplers;                // (1024)
    u32 max_shaders;                 // (4096)
    u32 max_pipelines;               // (4096)
    u32 max_timelines;               // (64)
    u32 max_command_threads;         // threads that may record commands (64)
    u32 max_commands_per_thread;     // command buffers per thread per frame per queue (32)
    u32 timestamp_queries_per_frame; // (1024)
    u32 bindless_sampled_images; // slots in the global descriptor set (65536, clamped to the limit)
    u32 bindless_storage_images; // (16384, clamped)
    u32 bindless_samplers;       // (1024, clamped)
    u64 staging_ring_bytes;      // persistently mapped upload ring (64 MiB)
    u32 deferred_destroy_capacity; // destructions queued per frame (4096)
} NphGpuDeviceDesc;

// ---------------------------------------------------------------------------
// Device information
// ---------------------------------------------------------------------------

typedef enum NphGpuDeviceType {
    NPH_GPU_DEVICE_TYPE_OTHER = 0,
    NPH_GPU_DEVICE_TYPE_INTEGRATED,
    NPH_GPU_DEVICE_TYPE_DISCRETE,
    NPH_GPU_DEVICE_TYPE_VIRTUAL,
    NPH_GPU_DEVICE_TYPE_CPU, // lavapipe and other software implementations
} NphGpuDeviceType;

const char *nph_gpu_device_type_name(NphGpuDeviceType type);

// Optional capabilities beyond the 1.3 baseline. All of them are detected
// and enabled when present; code checks the flag before using one.
typedef struct NphGpuFeatures {
    b32 mesh_shader;           // VK_EXT_mesh_shader (task + mesh stages)
    b32 ray_query;             // VK_KHR_ray_query + acceleration structures
    b32 image_atomic_int64;    // VK_EXT_shader_image_atomic_int64 (the software rasteriser, §8.6.4)
    b32 sampler_filter_minmax; // MIN/MAX reduction samplers (Hi-Z)
    b32 shader_float16;        // 16-bit arithmetic in shaders (§10.3.5)
    b32 shader_int8;           // 8-bit integers in shaders
    b32 shader_int64;          // 64-bit integers in shaders
    b32 storage_8bit;          // 8-bit storage buffer access (quantised vertices)
    b32 storage_16bit;         // 16-bit storage buffer access
    b32 depth_clamp;           // shadow rendering
    b32 fill_mode_non_solid;   // wireframe debug draw
    b32 texture_compression_bc; // BC1-7
    b32 sampler_anisotropy;     //
    b32 memory_budget;          // VK_EXT_memory_budget: live heap budgets in the stats
    b32 calibrated_timestamps;  // VK_EXT_calibrated_timestamps (CPU/GPU timeline alignment)
    b32 pipeline_statistics;    // pipeline statistics queries
    b32 async_compute;          // the COMPUTE queue is distinct from GRAPHICS
    b32 dedicated_transfer;     // the TRANSFER queue is a transfer-only family
    b32 timestamps_on_compute;  // the COMPUTE queue supports timestamps
    b32 timestamps_on_transfer; // the TRANSFER queue supports timestamps
} NphGpuFeatures;

typedef struct NphGpuLimits {
    u32 max_image_dimension_2d;
    u32 max_image_dimension_3d;
    u32 max_image_dimension_cube;
    u32 max_image_layers;
    u32 max_color_attachments;
    u32 max_push_constant_bytes; // the device's; the layout uses NPH_GPU_PUSH_CONSTANT_BYTES
    u32 max_draw_indirect_count;
    u32 max_compute_workgroup_invocations;
    u32 max_compute_workgroup_size[3];
    u32 max_compute_workgroup_count[3];
    u32 subgroup_size;
    u32 max_sample_count; // largest supported colour + depth sample count
    f32 max_anisotropy;
    f32 timestamp_period_ns; // nanoseconds per timestamp tick
    u64 min_uniform_buffer_offset_alignment;
    u64 min_storage_buffer_offset_alignment;
    u64 optimal_buffer_copy_offset_alignment;
    u64 non_coherent_atom_size;
    u32 bindless_sampled_images; // the sizes actually used for the global set
    u32 bindless_storage_images;
    u32 bindless_samplers;
} NphGpuLimits;

typedef struct NphGpuDeviceInfo {
    char name[256];
    char driver_name[256];
    char driver_info[256];
    u32 vendor_id;
    u32 device_id;
    NphGpuDeviceType type;
    u32 api_version_major;
    u32 api_version_minor;
    u32 api_version_patch;
    u32 driver_version; // vendor-specific packing
    u64 device_local_bytes;
    u64 host_visible_bytes;
    NphGpuFeatures features;
    NphGpuLimits limits;
    b32 validation;            // the validation layer is active
    b32 sync_validation;       // and synchronisation validation is on
    b32 debug_utils;           // object names and labels are delivered to tools
    u32 physical_device_count; // devices the instance enumerated
    u32 physical_device_index; // which one was chosen
} NphGpuDeviceInfo;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Creates the device. NPH_RESULT_UNSUPPORTED when there is no Vulkan loader,
// no instance can be created, or no physical device meets the baseline
// (each reason is logged at error level); NPH_RESULT_INVALID_ARGUMENT for a
// NULL allocator; NPH_RESULT_OUT_OF_MEMORY when a Vulkan object or an
// allocation fails. `*out` is NULL on failure.
NPH_MUST_USE NphResult nph_gpu_device_create(const NphGpuDeviceDesc *desc, NphGpuDevice **out);

// Waits for the GPU, saves the pipeline cache, releases every deferred
// destruction, reports leaked resources (handles never destroyed) at error
// level, and destroys the device. Safe on NULL.
void nph_gpu_device_destroy(NphGpuDevice *dev);

// Waits until every queue is idle and runs every deferred destruction.
// NPH_RESULT_DEVICE_LOST if the device was lost.
NPH_MUST_USE NphResult nph_gpu_device_wait_idle(NphGpuDevice *dev);

const NphGpuDeviceInfo *nph_gpu_device_info(const NphGpuDevice *dev);
NphAllocator *nph_gpu_device_allocator(NphGpuDevice *dev);

// ---------------------------------------------------------------------------
// Validation gate and statistics
// ---------------------------------------------------------------------------

// Messages delivered by the validation layer since creation. The tests
// require errors == 0 (CLAUDE.md §8.5 "a gate requiring zero validation and
// sync-validation messages"); synchronisation validation reports as errors.
typedef struct NphGpuValidationStats {
    u64 errors;
    u64 warnings;
    u64 performance_warnings;
    u64 infos;
} NphGpuValidationStats;

NphGpuValidationStats nph_gpu_device_validation_stats(const NphGpuDevice *dev);

// Handles currently alive, for the leak checker and tests.
typedef struct NphGpuLiveCounts {
    u32 buffers;
    u32 images;
    u32 samplers;
    u32 shaders;
    u32 pipelines;
    u32 timelines;
    u32 pending_destroys; // queued for a frame that has not completed
} NphGpuLiveCounts;

NphGpuLiveCounts nph_gpu_device_live_counts(const NphGpuDevice *dev);

NphGpuMemoryStats nph_gpu_device_memory_stats(const NphGpuDevice *dev);

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Begins frame N: waits for frame N - NPH_GPU_FRAMES_IN_FLIGHT to complete
// on every queue, then recycles its slot. NPH_RESULT_DEVICE_LOST if the
// wait reports a lost device. A frame already active is asserted in
// development and refused with NPH_RESULT_INVALID_STATE everywhere.
NPH_MUST_USE NphResult nph_gpu_frame_begin(NphGpuDevice *dev);

// Ends the frame: flushes pending uploads and records, per queue, the
// timeline value the frame's work reaches, which the matching
// nph_gpu_frame_begin waits for. No active frame is asserted in development
// and refused with NPH_RESULT_INVALID_STATE everywhere.
NPH_MUST_USE NphResult nph_gpu_frame_end(NphGpuDevice *dev);

// The index of the frame being recorded (counts from 0; valid between
// begin and end), and its slot in [0, NPH_GPU_FRAMES_IN_FLIGHT).
u64 nph_gpu_frame_index(const NphGpuDevice *dev);
u32 nph_gpu_frame_slot(const NphGpuDevice *dev);
bool nph_gpu_frame_is_active(const NphGpuDevice *dev);

// Timestamp results of the frame that last completed in the current slot
// (frame N - NPH_GPU_FRAMES_IN_FLIGHT), as raw ticks indexed by the values
// nph_gpu_cmd_write_timestamp returned during that frame; multiply by
// NphGpuLimits.timestamp_period_ns. Valid until the next frame_begin.
// Returns false (and nothing) when no frame has completed in the slot.
bool nph_gpu_frame_timestamps(const NphGpuDevice *dev, u64 *out_frame_index, const u64 **out_ticks,
                              u32 *out_count);

// ---------------------------------------------------------------------------
// Timelines and submission
// ---------------------------------------------------------------------------

// A timeline semaphore: a monotonically increasing 64-bit counter the GPU
// signals and the CPU or another queue waits on.
NPH_MUST_USE NphResult nph_gpu_timeline_create(NphGpuDevice *dev, u64 initial_value,
                                               const char *name, NphGpuTimeline **out);
// Destroys a timeline from nph_gpu_timeline_create. A queue's own timeline
// (nph_gpu_queue_timeline) is the device's: destroying it is asserted in
// development and ignored, with an error logged, everywhere.
void nph_gpu_timeline_destroy(NphGpuDevice *dev, NphGpuTimeline *timeline);

// The current counter value.
u64 nph_gpu_timeline_value(NphGpuDevice *dev, const NphGpuTimeline *timeline);

// Blocks until the counter reaches `value`. NPH_RESULT_BUSY after
// `timeout_ns` (NPH_U64_MAX: forever), NPH_RESULT_DEVICE_LOST on loss.
NPH_MUST_USE NphResult nph_gpu_timeline_wait(NphGpuDevice *dev, const NphGpuTimeline *timeline,
                                             u64 value, u64 timeout_ns);

// Signals the counter from the CPU (it must only ever increase).
NPH_MUST_USE NphResult nph_gpu_timeline_signal(NphGpuDevice *dev, NphGpuTimeline *timeline,
                                               u64 value);

// Each queue's own timeline: every submit on the queue signals the next
// value, which nph_gpu_submit returns. Owned by the device.
NphGpuTimeline *nph_gpu_queue_timeline(NphGpuDevice *dev, NphGpuQueueType queue);

// The timeline value the queue's most recent submit will signal.
u64 nph_gpu_queue_submitted_value(NphGpuDevice *dev, NphGpuQueueType queue);

typedef struct NphGpuTimelineOp {
    NphGpuTimeline *timeline;
    u64 value;
    u32 stages; // NphGpuStage bits: for a wait, the stages that wait; for a signal, the stages that
                // must finish
} NphGpuTimelineOp;

typedef struct NphGpuSubmitDesc {
    NphGpuQueueType queue;
    NphGpuCmd *const *cmds; // ended command buffers recorded for `queue`, executed in order
    u32 cmd_count;
    const NphGpuTimelineOp *waits;
    u32 wait_count;
    const NphGpuTimelineOp *signals;
    u32 signal_count;
    // When set, the submit waits for the swapchain's acquired image and
    // signals its present semaphore, so the next nph_gpu_swapchain_present
    // can proceed. `swapchain_ops` restricts that to one side
    // (NphGpuSwapchainOp bits; 0 means both), for a renderer whose first
    // and last uses of the image are in different submits.
    NphGpuSwapchain *swapchain;
    u32 swapchain_ops;
} NphGpuSubmitDesc;

typedef enum NphGpuSwapchainOp {
    NPH_GPU_SWAPCHAIN_WAIT_ACQUIRE = 1U << 0U,
    NPH_GPU_SWAPCHAIN_SIGNAL_PRESENT = 1U << 1U,
} NphGpuSwapchainOp;

// Submits command buffers. Besides the explicit operations the submit
// waits, on its first graphics-queue use after an upload flush, for the
// uploads to land, and always signals the queue's own timeline with the
// next value (returned through `out_value`, which may be NULL).
// NPH_RESULT_DEVICE_LOST on loss; NPH_RESULT_INVALID_ARGUMENT when a
// command buffer was recorded for a different queue.
NPH_MUST_USE NphResult nph_gpu_submit(NphGpuDevice *dev, const NphGpuSubmitDesc *desc,
                                      u64 *out_value);

#ifdef __cplusplus
}
#endif
