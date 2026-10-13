// SPDX-License-Identifier: Apache-2.0
#pragma once

// Command recording (CLAUDE.md §8.5 "Commands: per-thread command pools per
// frame in flight, parallel recording via jobs").
//
// nph_gpu_cmd_begin hands out a primary command buffer from the calling
// thread's pool for the current frame slot and queue; any thread may
// record its own command buffers at the same time (a job per render-graph
// pass, for instance). A pool is reset when its frame slot is recycled by
// nph_gpu_frame_begin, or by nph_gpu_device_wait_idle, so a command buffer
// is valid for one frame only.
//
// The recording functions are thin wrappers over the Vulkan commands they
// name, with the engine's enums, handles, and defaults: every pipeline
// shares the bindless descriptor set and push-constant layout, so binding
// a pipeline binds everything; vertex data is pulled from buffer addresses,
// so there is no vertex buffer binding; dynamic rendering replaces render
// passes, and beginning it sets a full-size viewport and scissor.
//
// Explicit barriers (nph_gpu_cmd_barrier) exist for code outside the
// render graph: uploads, tests, benchmarks, mip generation. The render
// graph computes its own barriers from declared uses and is the normal
// path for rendering (render_graph.h).
//
// Thread-safety: cmd_begin is NPH_THREADSAFE. Recording on one NphGpuCmd
// is single-threaded (the thread that began it).

#include <nepheline/gpu/resources.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Begins a command buffer for `queue`. NPH_RESULT_FULL when the calling
// thread has used its max_commands_per_thread for this slot and queue, or
// when more than max_command_threads threads have recorded;
// NPH_RESULT_OUT_OF_MEMORY when the pool cannot grow.
NPH_MUST_USE NphResult nph_gpu_cmd_begin(NphGpuDevice *dev, NphGpuQueueType queue, const char *name,
                                         NphGpuCmd **out);

// Ends recording. The command buffer may then be submitted once.
void nph_gpu_cmd_end(NphGpuCmd *cmd);

NphGpuQueueType nph_gpu_cmd_queue(const NphGpuCmd *cmd);
NphGpuDevice *nph_gpu_cmd_device(NphGpuCmd *cmd);

// ---------------------------------------------------------------------------
// Barriers
// ---------------------------------------------------------------------------

typedef struct NphGpuMemoryBarrier {
    u32 src_stages; // NphGpuStage bits
    u32 src_access; // NphGpuAccess bits
    u32 dst_stages;
    u32 dst_access;
} NphGpuMemoryBarrier;

typedef struct NphGpuBufferBarrier {
    NphGpuBuffer buffer;
    u32 src_stages;
    u32 src_access;
    u32 dst_stages;
    u32 dst_access;
    u64 offset;
    u64 size; // NPH_GPU_WHOLE_SIZE or 0: the rest of the buffer
} NphGpuBufferBarrier;

typedef struct NphGpuImageBarrier {
    NphGpuImage image;
    u32 src_stages;
    u32 src_access;
    NphGpuLayout old_layout; // UNDEFINED discards the contents
    u32 dst_stages;
    u32 dst_access;
    NphGpuLayout new_layout;
    u32 base_mip;
    u32 mip_count; // NPH_GPU_ALL_MIPS or 0: the rest
    u32 base_layer;
    u32 layer_count; // NPH_GPU_ALL_LAYERS or 0: the rest
    // Queue family ownership transfer: when the queues differ, the barrier
    // is a release (recorded on src_queue) or an acquire (on dst_queue),
    // decided by the queue the command buffer belongs to.
    NphGpuQueueType src_queue;
    NphGpuQueueType dst_queue;
} NphGpuImageBarrier;

typedef struct NphGpuBarrierDesc {
    const NphGpuMemoryBarrier *memory;
    u32 memory_count;
    const NphGpuBufferBarrier *buffers;
    u32 buffer_count;
    const NphGpuImageBarrier *images;
    u32 image_count;
} NphGpuBarrierDesc;

// Records a synchronization2 pipeline barrier and updates the tracked
// layout of each image's mips. Layouts are tracked per mip, not per
// layer: a barrier over some layers records its new layout for the whole
// mip, so keep every layer of a mip in one layout.
void nph_gpu_cmd_barrier(NphGpuCmd *cmd, const NphGpuBarrierDesc *desc);

// Convenience: one image barrier over all mips and layers on one queue.
void nph_gpu_cmd_image_barrier(NphGpuCmd *cmd, NphGpuImage image, u32 src_stages, u32 src_access,
                               NphGpuLayout old_layout, u32 dst_stages, u32 dst_access,
                               NphGpuLayout new_layout);

// Convenience: one buffer barrier over the whole buffer.
void nph_gpu_cmd_buffer_barrier(NphGpuCmd *cmd, NphGpuBuffer buffer, u32 src_stages, u32 src_access,
                                u32 dst_stages, u32 dst_access);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

typedef struct NphGpuAttachment {
    NphGpuImage image;
    u32 mip;
    u32 base_layer;
    u32 layer_count; // 0: 1
    NphGpuLoadOp load;
    NphGpuStoreOp store;
    NphGpuClearValue clear;
} NphGpuAttachment;

typedef struct NphGpuRenderingDesc {
    NphGpuAttachment colors[NPH_GPU_MAX_COLOR_ATTACHMENTS];
    u32 color_count;
    NphGpuAttachment depth; // image null: no depth attachment
    b32 depth_read_only;    // depth test without write (layout DEPTH_READ_ONLY)
    // Render area; zero width or height: the full attachment extent.
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} NphGpuRenderingDesc;

// Begins dynamic rendering. The attachments must already be in the
// attachment layouts (COLOR_ATTACHMENT, DEPTH_ATTACHMENT or
// DEPTH_READ_ONLY). Sets the viewport and scissor to the render area.
void nph_gpu_cmd_begin_rendering(NphGpuCmd *cmd, const NphGpuRenderingDesc *desc);
void nph_gpu_cmd_end_rendering(NphGpuCmd *cmd);

void nph_gpu_cmd_set_viewport(NphGpuCmd *cmd, f32 x, f32 y, f32 width, f32 height, f32 min_depth,
                              f32 max_depth);
void nph_gpu_cmd_set_scissor(NphGpuCmd *cmd, i32 x, i32 y, u32 width, u32 height);

// Binds a graphics or compute pipeline together with the bindless set.
void nph_gpu_cmd_bind_pipeline(NphGpuCmd *cmd, NphGpuPipeline pipeline);

// Writes `size` bytes (<= NPH_GPU_PUSH_CONSTANT_BYTES, a multiple of 4) of
// push constants from offset 0, visible to every stage.
void nph_gpu_cmd_push_constants(NphGpuCmd *cmd, const void *data, u32 size);

void nph_gpu_cmd_bind_index_buffer(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset,
                                   NphGpuIndexType type);

void nph_gpu_cmd_draw(NphGpuCmd *cmd, u32 vertex_count, u32 instance_count, u32 first_vertex,
                      u32 first_instance);
void nph_gpu_cmd_draw_indexed(NphGpuCmd *cmd, u32 index_count, u32 instance_count, u32 first_index,
                              i32 vertex_offset, u32 first_instance);
void nph_gpu_cmd_draw_indirect(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset, u32 draw_count,
                               u32 stride);
void nph_gpu_cmd_draw_indexed_indirect(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset,
                                       u32 draw_count, u32 stride);
// The GPU-driven draw of CLAUDE.md §8.6.3: the count comes from a buffer.
void nph_gpu_cmd_draw_indexed_indirect_count(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset,
                                             NphGpuBuffer count_buffer, u64 count_offset,
                                             u32 max_draw_count, u32 stride);
// Requires NphGpuFeatures.mesh_shader (asserted).
void nph_gpu_cmd_draw_mesh_tasks(NphGpuCmd *cmd, u32 x, u32 y, u32 z);

void nph_gpu_cmd_dispatch(NphGpuCmd *cmd, u32 x, u32 y, u32 z);
void nph_gpu_cmd_dispatch_indirect(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset);

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

typedef struct NphGpuBufferCopy {
    u64 src_offset;
    u64 dst_offset;
    u64 size;
} NphGpuBufferCopy;

void nph_gpu_cmd_copy_buffer(NphGpuCmd *cmd, NphGpuBuffer src, NphGpuBuffer dst,
                             const NphGpuBufferCopy *regions, u32 region_count);

typedef struct NphGpuBufferImageCopy {
    u64 buffer_offset; // tightly packed texels (or blocks) from here
    u32 mip;
    u32 base_layer;
    u32 layer_count; // 0: 1
    i32 x;
    i32 y;
    i32 z;
    u32 width; // 0: the whole mip from the offset
    u32 height;
    u32 depth;
} NphGpuBufferImageCopy;

// `layout` is TRANSFER_DST (or GENERAL) for the destination, TRANSFER_SRC
// (or GENERAL) for the source.
void nph_gpu_cmd_copy_buffer_to_image(NphGpuCmd *cmd, NphGpuBuffer src, NphGpuImage dst,
                                      NphGpuLayout dst_layout, const NphGpuBufferImageCopy *regions,
                                      u32 region_count);
void nph_gpu_cmd_copy_image_to_buffer(NphGpuCmd *cmd, NphGpuImage src, NphGpuLayout src_layout,
                                      NphGpuBuffer dst, const NphGpuBufferImageCopy *regions,
                                      u32 region_count);

// Copies one whole mip (all layers) between images of the same extent.
void nph_gpu_cmd_copy_image(NphGpuCmd *cmd, NphGpuImage src, u32 src_mip, NphGpuLayout src_layout,
                            NphGpuImage dst, u32 dst_mip, NphGpuLayout dst_layout);

// Scaled copy of one whole mip (all layers); graphics queue only.
void nph_gpu_cmd_blit_image(NphGpuCmd *cmd, NphGpuImage src, u32 src_mip, NphGpuLayout src_layout,
                            NphGpuImage dst, u32 dst_mip, NphGpuLayout dst_layout,
                            NphGpuFilter filter);

// Fills mips 1.. of a colour image from mip 0 with linear blits. Mip 0
// must be in TRANSFER_SRC; the whole image ends in `final_layout` with
// the given stage and access (SHADER_READ_ONLY for sampling). Graphics
// queue only.
void nph_gpu_cmd_generate_mips(NphGpuCmd *cmd, NphGpuImage image, NphGpuLayout final_layout,
                               u32 final_stages, u32 final_access);

void nph_gpu_cmd_fill_buffer(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset, u64 size, u32 data);

// Inline update of up to 65536 bytes (a multiple of 4) at a 4-byte offset.
void nph_gpu_cmd_update_buffer(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset, const void *data,
                               u64 size);

void nph_gpu_cmd_clear_color_image(NphGpuCmd *cmd, NphGpuImage image, NphGpuLayout layout,
                                   const NphGpuClearValue *value, u32 base_mip, u32 mip_count,
                                   u32 base_layer, u32 layer_count);
void nph_gpu_cmd_clear_depth_image(NphGpuCmd *cmd, NphGpuImage image, NphGpuLayout layout,
                                   const NphGpuClearValue *value, u32 base_mip, u32 mip_count,
                                   u32 base_layer, u32 layer_count);

// ---------------------------------------------------------------------------
// Debugging and timing
// ---------------------------------------------------------------------------

// Debug labels (VK_EXT_debug_utils), visible in RenderDoc and Nsight. No-ops
// without debug utils.
void nph_gpu_cmd_begin_label(NphGpuCmd *cmd, const char *name, f32 r, f32 g, f32 b);
void nph_gpu_cmd_end_label(NphGpuCmd *cmd);

// Writes a timestamp after the given stage completes and returns its
// query index in this frame's pool, or NPH_GPU_INVALID_INDEX when the
// pool is exhausted or the queue has no timestamp support. Read the
// result with nph_gpu_frame_timestamps two frames later.
u32 nph_gpu_cmd_write_timestamp(NphGpuCmd *cmd, u32 stage);

#ifdef __cplusplus
}
#endif
