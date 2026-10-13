// SPDX-License-Identifier: Apache-2.0
#pragma once

// The render graph (CLAUDE.md §8.5), after O'Donnell, "FrameGraph:
// Extensible Rendering Architecture in Frostbite", GDC 2017.
//
// Each frame the renderer declares passes and the resources they read and
// write, then compiles and executes the graph:
//
//   - Passes that contribute nothing to a side effect (a present, an
//     imported resource write, or NPH_GPU_RG_PASS_SIDE_EFFECTS) are culled.
//   - Every barrier and layout transition is derived from the declared
//     uses, with synchronization2 stage and access masks that cover only
//     what the pass does, and recorded at the start of the pass that needs
//     it (per mip for images).
//   - Transient images and buffers are placed in a few large memory blocks
//     by lifetime, so resources whose lifetimes do not overlap share memory
//     (CLAUDE.md §8.5 "Transient resources are aliased in memory").
//   - Passes flagged NPH_GPU_RG_PASS_ASYNC_COMPUTE run on the compute queue
//     when the device has one, with timeline semaphores between the queues
//     and queue-family ownership transfers for exclusive images.
//   - Each pass records into its own primary command buffer, so when a
//     job system is given the passes are recorded in parallel (one job per
//     pass) and submitted in order. Barriers are computed before recording
//     starts, which is what makes the passes independent.
//   - Timestamps bracket every pass; the results of the frame that
//     completed last are available through nph_gpu_rg_timings.
//
// Resource handles (NphGpuRgImage, NphGpuRgBuffer) are valid for one frame.
// Inside an execute callback the physical handle is available through
// nph_gpu_rg_image / nph_gpu_rg_buffer, so a pass can put bindless
// indices and buffer addresses into its push constants.
//
// Costs (§1.4): the graph allocates its arrays at creation for the
// capacities in the description and nothing per frame except Vulkan
// objects for transient resources, which are cached across frames by
// description so a steady-state frame creates nothing.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY except that execute callbacks run
// on job workers when a job system is set.

#include <nepheline/gpu/commands.h>
#include <nepheline/gpu/pipeline.h>
#include <nepheline/gpu/swapchain.h>
#include <nepheline/jobs/jobs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphGpuRenderGraph NphGpuRenderGraph;
typedef struct NphGpuRgPass NphGpuRgPass;

typedef struct NphGpuRgImage {
    u32 value;
} NphGpuRgImage;
typedef struct NphGpuRgBuffer {
    u32 value;
} NphGpuRgBuffer;

#define NPH_GPU_RG_IMAGE_NULL  ((NphGpuRgImage){0U})
#define NPH_GPU_RG_BUFFER_NULL ((NphGpuRgBuffer){0U})

NPH_INLINE bool nph_gpu_rg_image_is_null(NphGpuRgImage h)
{
    return h.value == 0U;
}
NPH_INLINE bool nph_gpu_rg_buffer_is_null(NphGpuRgBuffer h)
{
    return h.value == 0U;
}

typedef struct NphGpuRenderGraphDesc {
    NphAllocator *alloc; // required
    NphJobSystem *jobs;  // optional: passes are recorded in parallel
    u32 max_passes;      // (128)
    u32 max_resources;   // transient + imported, images + buffers (256)
    u32 max_uses;        // resource uses across all passes (1024)
    u32 max_barriers;    // (1024)
    u32 transient_cache; // transient resources kept alive across frames (256)
    const char *name;
} NphGpuRenderGraphDesc;

NPH_MUST_USE NphResult nph_gpu_render_graph_create(NphGpuDevice *dev,
                                                   const NphGpuRenderGraphDesc *desc,
                                                   NphGpuRenderGraph **out);

// Waits for the device to go idle and releases the transient resources.
void nph_gpu_render_graph_destroy(NphGpuRenderGraph *graph);

// ---------------------------------------------------------------------------
// Declaring a frame
// ---------------------------------------------------------------------------

// Starts a new frame's declarations (discarding the previous frame's).
void nph_gpu_rg_begin(NphGpuRenderGraph *graph);

// A transient image or buffer owned by the graph for this frame. Null
// (logged) when max_resources is reached.
NphGpuRgImage nph_gpu_rg_create_image(NphGpuRenderGraph *graph, const NphGpuImageDesc *desc);
NphGpuRgBuffer nph_gpu_rg_create_buffer(NphGpuRenderGraph *graph, const NphGpuBufferDesc *desc);

// An existing resource, with its tracked layouts as the starting state.
NphGpuRgImage nph_gpu_rg_import_image(NphGpuRenderGraph *graph, NphGpuImage image);
NphGpuRgBuffer nph_gpu_rg_import_buffer(NphGpuRenderGraph *graph, NphGpuBuffer buffer);

typedef enum NphGpuRgPassType {
    NPH_GPU_RG_PASS_GRAPHICS = 0, // draws; attachments declared with the attachment functions
    NPH_GPU_RG_PASS_COMPUTE,
    NPH_GPU_RG_PASS_TRANSFER,
} NphGpuRgPassType;

typedef enum NphGpuRgPassFlags {
    NPH_GPU_RG_PASS_ASYNC_COMPUTE = 1U
                                    << 0U, // compute passes: run on the COMPUTE queue when distinct
    NPH_GPU_RG_PASS_SIDE_EFFECTS = 1U << 1U, // never culled
} NphGpuRgPassFlags;

typedef void (*NphGpuRgExecuteFn)(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user);

typedef struct NphGpuRgPassDesc {
    const char *name; // static string
    NphGpuRgPassType type;
    u32 flags;
    NphGpuRgExecuteFn execute; // NULL: the pass only clears/loads attachments
    void *user;
} NphGpuRgPassDesc;

// Adds a pass; passes execute in the order they are added (after culling).
// NULL (logged) when max_passes is reached.
NphGpuRgPass *nph_gpu_rg_add_pass(NphGpuRenderGraph *graph, const NphGpuRgPassDesc *desc);

// How a pass uses a resource.
typedef enum NphGpuRgUse {
    NPH_GPU_RG_USE_SAMPLED = 0,   // image: sampled in the pass's shader stages
    NPH_GPU_RG_USE_STORAGE_READ,  // image or buffer: storage read
    NPH_GPU_RG_USE_STORAGE_WRITE, // image or buffer: storage write (and read)
    NPH_GPU_RG_USE_SHADER_READ,   // buffer: read through its address (uniform or storage)
    NPH_GPU_RG_USE_SHADER_WRITE,  // buffer: written through its address
    NPH_GPU_RG_USE_INDEX,         // buffer: index buffer
    NPH_GPU_RG_USE_INDIRECT,      // buffer: indirect arguments
    NPH_GPU_RG_USE_TRANSFER_SRC,  // image or buffer: copy/blit source
    NPH_GPU_RG_USE_TRANSFER_DST,  // image or buffer: copy/blit/clear/fill destination
    NPH_GPU_RG_USE_COLOR_ATTACHMENT,
    NPH_GPU_RG_USE_DEPTH_ATTACHMENT,
    NPH_GPU_RG_USE_DEPTH_READ_ONLY, // depth test without write
    NPH_GPU_RG_USE_PRESENT,
    NPH_GPU_RG_USE_COUNT
} NphGpuRgUse;

// Declares a read of mips [base_mip, base_mip + mip_count) (NPH_GPU_ALL_MIPS:
// all). `use` is SAMPLED, STORAGE_READ, TRANSFER_SRC, or DEPTH_READ_ONLY.
void nph_gpu_rg_read_image(NphGpuRgPass *pass, NphGpuRgImage image, NphGpuRgUse use, u32 base_mip,
                           u32 mip_count);

// Declares a write of mips [base_mip, base_mip + mip_count). `use` is
// STORAGE_WRITE or TRANSFER_DST.
void nph_gpu_rg_write_image(NphGpuRgPass *pass, NphGpuRgImage image, NphGpuRgUse use, u32 base_mip,
                            u32 mip_count);

typedef struct NphGpuRgAttachment {
    u32 mip;
    u32 base_layer;
    u32 layer_count; // 0: 1
    NphGpuLoadOp load;
    NphGpuStoreOp store;
    NphGpuClearValue clear;
} NphGpuRgAttachment;

// Declares colour attachment i (i = the number of previous calls on the
// pass) and the depth attachment of a graphics pass; the graph begins and
// ends rendering around the execute callback.
void nph_gpu_rg_color_attachment(NphGpuRgPass *pass, NphGpuRgImage image,
                                 const NphGpuRgAttachment *attachment);
void nph_gpu_rg_depth_attachment(NphGpuRgPass *pass, NphGpuRgImage image,
                                 const NphGpuRgAttachment *attachment, b32 read_only);

// `use` is SHADER_READ, STORAGE_READ, INDEX, INDIRECT, or TRANSFER_SRC.
void nph_gpu_rg_read_buffer(NphGpuRgPass *pass, NphGpuRgBuffer buffer, NphGpuRgUse use);
// `use` is SHADER_WRITE, STORAGE_WRITE, or TRANSFER_DST.
void nph_gpu_rg_write_buffer(NphGpuRgPass *pass, NphGpuRgBuffer buffer, NphGpuRgUse use);

// Declares that `image` (an imported swapchain image) is presented after
// the last pass that touches it: the final transition to PRESENT is
// recorded, the graphics submit waits for the acquire and signals the
// present semaphore. The caller still calls nph_gpu_swapchain_present.
void nph_gpu_rg_present(NphGpuRenderGraph *graph, NphGpuRgImage image, NphGpuSwapchain *swapchain);

// ---------------------------------------------------------------------------
// Compile and execute
// ---------------------------------------------------------------------------

// Culls, orders, allocates transients, and computes barriers.
// NPH_RESULT_INVALID_ARGUMENT for a graph that reads a resource no earlier
// pass wrote (logged with the pass and resource names);
// NPH_RESULT_OUT_OF_MEMORY when a transient could not be allocated;
// NPH_RESULT_FULL when max_barriers is exceeded.
NPH_MUST_USE NphResult nph_gpu_rg_compile(NphGpuRenderGraph *graph);

// Records every surviving pass (in parallel with a job system) and submits
// per queue, in order. Must follow a successful compile.
NPH_MUST_USE NphResult nph_gpu_rg_execute(NphGpuRenderGraph *graph);

// Physical resources during execute (inside callbacks). Null outside.
NphGpuImage nph_gpu_rg_image(const NphGpuRenderGraph *graph, NphGpuRgImage image);
NphGpuBuffer nph_gpu_rg_buffer(const NphGpuRenderGraph *graph, NphGpuRgBuffer buffer);
NphGpuDevice *nph_gpu_rg_device(NphGpuRenderGraph *graph);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

typedef struct NphGpuRgStats {
    u32 passes;       // declared
    u32 culled;       // of those, culled
    u32 async_passes; // executed on the compute queue
    u32 submits;      // queue submits made by execute
    u32 barriers;     // image + buffer + memory barriers recorded
    u32 transient_images;
    u32 transient_buffers;
    u64 transient_bytes_requested; // sum of every transient's own size
    u64 transient_bytes_allocated; // memory actually reserved after aliasing
    u32 command_buffers;
} NphGpuRgStats;

// Statistics of the last compile + execute.
NphGpuRgStats nph_gpu_rg_stats(const NphGpuRenderGraph *graph);

// Whether the pass survived culling (valid after compile, for the frame).
bool nph_gpu_rg_pass_is_culled(const NphGpuRenderGraph *graph, const NphGpuRgPass *pass);

typedef struct NphGpuRgPassTiming {
    const char *name;
    u64 gpu_ns;
    NphGpuQueueType queue;
} NphGpuRgPassTiming;

// Per-pass GPU times of the most recent frame whose timestamps are
// available (NPH_GPU_FRAMES_IN_FLIGHT frames ago). Returns the number
// written (at most `cap`); `out_frame_index` (may be NULL) receives which
// frame. 0 when none has completed yet.
u32 nph_gpu_rg_timings(const NphGpuRenderGraph *graph, NphGpuRgPassTiming *out, u32 cap,
                       u64 *out_frame_index);

#ifdef __cplusplus
}
#endif
