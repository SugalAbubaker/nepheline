// SPDX-License-Identifier: Apache-2.0
#pragma once

// The vocabulary shared by every gpu header (CLAUDE.md §8.5): handles,
// formats, resource descriptions, and the synchronisation enums used by
// explicit barriers. The module is a thin, Vulkan-specific RHI
// (docs/adr/0014-gpu-module.md): the names mirror Vulkan's concepts one to
// one, but no Vulkan header is exposed, so modules to the right of gpu
// (render, scene, editor) never include one (CLAUDE.md §1.8). The one
// escape hatch, vk.h, is for the editor's UI backend, the gpu tests, and
// the GPU benchmarks.
//
// Handles are 32-bit generational values issued by the device
// (core/handle.h): a stale handle, one kept past destroy, is detected and
// asserted rather than dereferenced. Zero is never a valid handle, so a
// zero-filled struct holds "no resource".

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// Frames the CPU may record ahead of the GPU. Two keeps the CPU at most one
// frame ahead (CLAUDE.md §10.6) and bounds every per-frame pool.
#define NPH_GPU_FRAMES_IN_FLIGHT 2U

#define NPH_GPU_MAX_COLOR_ATTACHMENTS 8U
#define NPH_GPU_MAX_MIPS              16U
#define NPH_GPU_MAX_QUEUES            3U

// Bytes of push constants every pipeline receives (the Vulkan minimum
// guarantee; every pipeline shares one layout so it is the same for all).
#define NPH_GPU_PUSH_CONSTANT_BYTES 128U

// "Not an index" for bindless slots and query indices.
#define NPH_GPU_INVALID_INDEX NPH_U32_MAX
// "The rest of the buffer" / "all remaining mips or layers".
#define NPH_GPU_WHOLE_SIZE NPH_U64_MAX
#define NPH_GPU_ALL_MIPS   NPH_U32_MAX
#define NPH_GPU_ALL_LAYERS NPH_U32_MAX

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

typedef struct NphGpuBuffer {
    u32 value;
} NphGpuBuffer;
typedef struct NphGpuImage {
    u32 value;
} NphGpuImage;
typedef struct NphGpuSampler {
    u32 value;
} NphGpuSampler;
typedef struct NphGpuShader {
    u32 value;
} NphGpuShader;
typedef struct NphGpuPipeline {
    u32 value;
} NphGpuPipeline;

#define NPH_GPU_BUFFER_NULL   ((NphGpuBuffer){0U})
#define NPH_GPU_IMAGE_NULL    ((NphGpuImage){0U})
#define NPH_GPU_SAMPLER_NULL  ((NphGpuSampler){0U})
#define NPH_GPU_SHADER_NULL   ((NphGpuShader){0U})
#define NPH_GPU_PIPELINE_NULL ((NphGpuPipeline){0U})

NPH_INLINE bool nph_gpu_buffer_is_null(NphGpuBuffer h)
{
    return h.value == 0U;
}
NPH_INLINE bool nph_gpu_image_is_null(NphGpuImage h)
{
    return h.value == 0U;
}
NPH_INLINE bool nph_gpu_sampler_is_null(NphGpuSampler h)
{
    return h.value == 0U;
}
NPH_INLINE bool nph_gpu_shader_is_null(NphGpuShader h)
{
    return h.value == 0U;
}
NPH_INLINE bool nph_gpu_pipeline_is_null(NphGpuPipeline h)
{
    return h.value == 0U;
}

// ---------------------------------------------------------------------------
// Queues
// ---------------------------------------------------------------------------

// The device exposes up to three queues. GRAPHICS also does compute,
// transfer, and present. COMPUTE is the async compute queue (CLAUDE.md
// §10.3.6): a dedicated family when the hardware has one, otherwise a
// second graphics-family queue, otherwise the graphics queue itself.
// TRANSFER is a transfer-only family when present (uploads overlap with
// rendering, §8.5 "Memory"), otherwise the graphics queue.
typedef enum NphGpuQueueType {
    NPH_GPU_QUEUE_GRAPHICS = 0,
    NPH_GPU_QUEUE_COMPUTE = 1,
    NPH_GPU_QUEUE_TRANSFER = 2,
    NPH_GPU_QUEUE_COUNT = 3,
} NphGpuQueueType;

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

// The formats the engine uses. The bandwidth-first formats of CLAUDE.md
// §10.3.5 (R11G11B10, RGB9E5, BC-compressed textures, FP16) are all here;
// anything else is added when a subsystem needs it.
typedef enum NphGpuFormat {
    NPH_GPU_FORMAT_UNDEFINED = 0,
    // 8-bit
    NPH_GPU_FORMAT_R8_UNORM,
    NPH_GPU_FORMAT_R8_UINT,
    NPH_GPU_FORMAT_R8G8_UNORM,
    NPH_GPU_FORMAT_R8G8B8A8_UNORM,
    NPH_GPU_FORMAT_R8G8B8A8_SRGB,
    NPH_GPU_FORMAT_B8G8R8A8_UNORM,
    NPH_GPU_FORMAT_B8G8R8A8_SRGB,
    // 16-bit
    NPH_GPU_FORMAT_R16_UNORM,
    NPH_GPU_FORMAT_R16_UINT,
    NPH_GPU_FORMAT_R16_SFLOAT,
    NPH_GPU_FORMAT_R16G16_SFLOAT,
    NPH_GPU_FORMAT_R16G16_UNORM,
    NPH_GPU_FORMAT_R16G16B16A16_UNORM,
    NPH_GPU_FORMAT_R16G16B16A16_SFLOAT,
    // 32-bit
    NPH_GPU_FORMAT_R32_UINT,
    NPH_GPU_FORMAT_R32_SFLOAT,
    NPH_GPU_FORMAT_R32G32_UINT,
    NPH_GPU_FORMAT_R32G32_SFLOAT,
    NPH_GPU_FORMAT_R32G32B32A32_UINT,
    NPH_GPU_FORMAT_R32G32B32A32_SFLOAT,
    // 64-bit (the visibility buffer: 32-bit depth + 32-bit id, §8.6.4)
    NPH_GPU_FORMAT_R64_UINT,
    // Packed
    NPH_GPU_FORMAT_B10G11R11_UFLOAT,
    NPH_GPU_FORMAT_E5B9G9R9_UFLOAT,
    NPH_GPU_FORMAT_A2B10G10R10_UNORM,
    // Depth
    NPH_GPU_FORMAT_D16_UNORM,
    NPH_GPU_FORMAT_D32_SFLOAT,
    NPH_GPU_FORMAT_D24_UNORM_S8_UINT,
    NPH_GPU_FORMAT_D32_SFLOAT_S8_UINT,
    // Block compressed (4x4 blocks)
    NPH_GPU_FORMAT_BC1_RGBA_UNORM,
    NPH_GPU_FORMAT_BC1_RGBA_SRGB,
    NPH_GPU_FORMAT_BC3_UNORM,
    NPH_GPU_FORMAT_BC3_SRGB,
    NPH_GPU_FORMAT_BC4_UNORM,
    NPH_GPU_FORMAT_BC5_UNORM,
    NPH_GPU_FORMAT_BC6H_UFLOAT,
    NPH_GPU_FORMAT_BC7_UNORM,
    NPH_GPU_FORMAT_BC7_SRGB,
    // Vertices (acceleration-structure builds; appended, so earlier values
    // keep their meaning)
    NPH_GPU_FORMAT_R32G32B32_SFLOAT,
    NPH_GPU_FORMAT_COUNT
} NphGpuFormat;

typedef enum NphGpuAspect {
    NPH_GPU_ASPECT_COLOR = 1U << 0U,
    NPH_GPU_ASPECT_DEPTH = 1U << 1U,
    NPH_GPU_ASPECT_STENCIL = 1U << 2U,
} NphGpuAspect;

typedef struct NphGpuFormatInfo {
    const char *name;
    u32 bytes_per_block; // bytes of one texel, or of one block for compressed formats
    u32 block_width;     // texels per block side (1 for uncompressed)
    u32 block_height;
    u32 aspects; // NphGpuAspect bits
    b32 srgb;
    b32 compressed;
} NphGpuFormatInfo;

// Static description of a format. Never fails; out-of-range values map
// to UNDEFINED.
const NphGpuFormatInfo *nph_gpu_format_info(NphGpuFormat format);

NPH_INLINE const char *nph_gpu_format_name(NphGpuFormat format)
{
    return nph_gpu_format_info(format)->name;
}
NPH_INLINE bool nph_gpu_format_is_depth(NphGpuFormat format)
{
    return (nph_gpu_format_info(format)->aspects & (u32)NPH_GPU_ASPECT_DEPTH) != 0U;
}

// Bytes of one tightly packed mip level of the given extent (rounded up to
// whole blocks for compressed formats).
u64 nph_gpu_format_level_bytes(NphGpuFormat format, u32 width, u32 height, u32 depth);

// Extent of mip `mip` of a level-0 extent (at least 1 in every dimension).
NPH_INLINE u32 nph_gpu_mip_extent(u32 extent, u32 mip)
{
    const u32 e = extent >> mip;
    return e == 0U ? 1U : e;
}

// Number of levels in a full mip chain.
u32 nph_gpu_mip_count(u32 width, u32 height, u32 depth);

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

// Where a buffer lives. Every buffer gets a device address (CLAUDE.md §8.5
// "Buffers are accessed via buffer device address"), so none of these
// affects how shaders reach it.
typedef enum NphGpuMemory {
    NPH_GPU_MEMORY_DEVICE = 0, // VRAM; filled through uploads or GPU writes
    NPH_GPU_MEMORY_UPLOAD, // host-visible, write-combined, persistently mapped: staging, per-frame
                           // data
    NPH_GPU_MEMORY_READBACK, // host-visible, cached: results the CPU reads
} NphGpuMemory;

typedef enum NphGpuBufferUsage {
    NPH_GPU_BUFFER_USAGE_TRANSFER_SRC = 1U << 0U,
    NPH_GPU_BUFFER_USAGE_TRANSFER_DST = 1U << 1U,
    NPH_GPU_BUFFER_USAGE_STORAGE = 1U << 2U, // read or written by shaders (through its address)
    NPH_GPU_BUFFER_USAGE_INDEX = 1U << 3U,
    NPH_GPU_BUFFER_USAGE_INDIRECT = 1U << 4U,
    NPH_GPU_BUFFER_USAGE_UNIFORM = 1U << 5U,
    // Read by acceleration-structure builds: vertices, indices, instances
    // (accel.h; NphGpuFeatures.ray_query).
    NPH_GPU_BUFFER_USAGE_ACCEL_INPUT = 1U << 6U,
} NphGpuBufferUsage;

typedef struct NphGpuBufferDesc {
    u64 size;            // bytes, > 0
    u32 usage;           // NphGpuBufferUsage bits
    NphGpuMemory memory; // zero-filled: NPH_GPU_MEMORY_DEVICE
    const char *name;    // optional; copied (63 chars kept); shown by validation and debuggers
} NphGpuBufferDesc;

// GPU memory in use, from the allocator (device.h: nph_gpu_device_memory_stats).
typedef struct NphGpuMemoryStats {
    u64 device_local_used;   // bytes of device-local heaps in use by this process
    u64 device_local_budget; // bytes the OS lets this process use (the heap size without
                             // memory_budget)
    u64 host_visible_used;
    u64 host_visible_budget;
    u64 block_bytes;      // bytes of memory blocks the allocator holds
    u32 allocation_count; // live allocations
} NphGpuMemoryStats;

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

typedef enum NphGpuImageType {
    NPH_GPU_IMAGE_2D = 0, // layer_count > 1 makes it a 2D array
    NPH_GPU_IMAGE_3D,
    NPH_GPU_IMAGE_CUBE, // layer_count is a multiple of 6 (6: one cube; 6n: a cube array)
} NphGpuImageType;

typedef enum NphGpuImageUsage {
    NPH_GPU_IMAGE_USAGE_TRANSFER_SRC = 1U << 0U,
    NPH_GPU_IMAGE_USAGE_TRANSFER_DST = 1U << 1U,
    NPH_GPU_IMAGE_USAGE_SAMPLED = 1U << 2U,          // gets a bindless sampled-image slot
    NPH_GPU_IMAGE_USAGE_STORAGE = 1U << 3U,          // gets a bindless storage-image slot per mip
    NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT = 1U << 4U, // rendered to
    NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT = 1U << 5U, // depth or depth-stencil target
} NphGpuImageUsage;

typedef enum NphGpuImageFlags {
    // Sparse residency (docs/adr/0065): the image is created without memory,
    // and its pages (the format's standard sparse block, 64 KiB) and its mip
    // tail are bound to pages of an NphGpuPagePool by nph_gpu_sparse_bind
    // (sparse.h). 2D, one layer, one sample, and no usage beyond SAMPLED,
    // TRANSFER_DST and TRANSFER_SRC; NphGpuFeatures.sparse_residency. The
    // image stays in NPH_GPU_LAYOUT_GENERAL, shared by every queue family,
    // so a page can be copied while the others are sampled.
    NPH_GPU_IMAGE_FLAG_SPARSE = 1U << 0U,
} NphGpuImageFlags;

typedef struct NphGpuImageDesc {
    NphGpuImageType type;
    NphGpuFormat format;
    u32 width;
    u32 height;
    u32 depth;       // 0: 1 (only 3D images use more)
    u32 mip_count;   // 0: the full chain
    u32 layer_count; // 0: 1
    u32 samples;     // 0: 1 (MSAA counts are powers of two up to the device limit)
    u32 usage;       // NphGpuImageUsage bits
    u32 flags;       // NphGpuImageFlags bits
    // UNDEFINED: `format`. Otherwise the format the sampled view and the
    // colour-attachment views read the image as, of the same texel size (a
    // 64-bit image sampled and rendered to as two 32-bit channels, an RGBA8
    // one sampled as sRGB): the image is created mutable, and SAMPLED and
    // COLOR_ATTACHMENT need only be supported by this format, the other
    // uses by `format`.
    NphGpuFormat sampled_format;
    const char *name;
} NphGpuImageDesc;

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

typedef enum NphGpuFilter {
    NPH_GPU_FILTER_NEAREST = 0,
    NPH_GPU_FILTER_LINEAR,
} NphGpuFilter;

typedef enum NphGpuAddressMode {
    NPH_GPU_ADDRESS_REPEAT = 0,
    NPH_GPU_ADDRESS_MIRRORED_REPEAT,
    NPH_GPU_ADDRESS_CLAMP_TO_EDGE,
    NPH_GPU_ADDRESS_CLAMP_TO_BORDER,
} NphGpuAddressMode;

typedef enum NphGpuCompareOp {
    NPH_GPU_COMPARE_NEVER = 0,
    NPH_GPU_COMPARE_LESS,
    NPH_GPU_COMPARE_EQUAL,
    NPH_GPU_COMPARE_LESS_OR_EQUAL,
    NPH_GPU_COMPARE_GREATER,
    NPH_GPU_COMPARE_NOT_EQUAL,
    NPH_GPU_COMPARE_GREATER_OR_EQUAL,
    NPH_GPU_COMPARE_ALWAYS,
} NphGpuCompareOp;

typedef enum NphGpuBorderColor {
    NPH_GPU_BORDER_TRANSPARENT_BLACK = 0,
    NPH_GPU_BORDER_OPAQUE_BLACK,
    NPH_GPU_BORDER_OPAQUE_WHITE,
} NphGpuBorderColor;

// Filter reduction (VK_EXT_sampler_filter_minmax, core 1.2 as optional):
// MIN and MAX build depth pyramids for Hi-Z culling in one sample per
// texel (§8.6.3). Available when NphGpuFeatures.sampler_filter_minmax.
typedef enum NphGpuReduction {
    NPH_GPU_REDUCTION_WEIGHTED_AVERAGE = 0,
    NPH_GPU_REDUCTION_MIN,
    NPH_GPU_REDUCTION_MAX,
} NphGpuReduction;

// "No upper LOD clamp" for max_lod.
#define NPH_GPU_LOD_CLAMP_NONE 1000.0F

typedef struct NphGpuSamplerDesc {
    NphGpuFilter min_filter;
    NphGpuFilter mag_filter;
    NphGpuFilter mip_filter;
    NphGpuAddressMode address_u;
    NphGpuAddressMode address_v;
    NphGpuAddressMode address_w;
    f32 max_anisotropy; // 0 or 1: off; clamped to the device limit
    f32 lod_bias;
    f32 min_lod;
    f32 max_lod; // 0: NPH_GPU_LOD_CLAMP_NONE
    b32 compare_enable;
    NphGpuCompareOp compare_op;
    NphGpuBorderColor border;
    NphGpuReduction reduction;
    const char *name;
} NphGpuSamplerDesc;

// ---------------------------------------------------------------------------
// Synchronisation vocabulary (explicit barriers in commands.h; the render
// graph derives all of this itself from declared resource uses)
// ---------------------------------------------------------------------------

typedef enum NphGpuStage {
    NPH_GPU_STAGE_NONE = 0,
    NPH_GPU_STAGE_INDIRECT = 1U << 0U, // reading indirect draw/dispatch arguments
    NPH_GPU_STAGE_INDEX_INPUT = 1U << 1U,
    NPH_GPU_STAGE_VERTEX_SHADER = 1U << 2U,
    NPH_GPU_STAGE_FRAGMENT_SHADER = 1U << 3U,
    NPH_GPU_STAGE_DEPTH_STENCIL = 1U << 4U, // early and late fragment tests
    NPH_GPU_STAGE_COLOR_OUTPUT = 1U << 5U,
    NPH_GPU_STAGE_COMPUTE = 1U << 6U,
    NPH_GPU_STAGE_TRANSFER = 1U << 7U, // copies, blits, clears, fills
    NPH_GPU_STAGE_HOST = 1U << 8U,
    NPH_GPU_STAGE_TASK_SHADER = 1U << 9U,
    NPH_GPU_STAGE_MESH_SHADER = 1U << 10U,
    NPH_GPU_STAGE_ALL_GRAPHICS = 1U << 11U,
    NPH_GPU_STAGE_ALL = 1U << 12U,
    NPH_GPU_STAGE_ACCEL_BUILD = 1U << 13U, // acceleration-structure builds
} NphGpuStage;

typedef enum NphGpuAccess {
    NPH_GPU_ACCESS_NONE = 0,
    NPH_GPU_ACCESS_INDIRECT_READ = 1U << 0U,
    NPH_GPU_ACCESS_INDEX_READ = 1U << 1U,
    NPH_GPU_ACCESS_SHADER_READ = 1U << 2U, // uniform, storage, and sampled reads
    NPH_GPU_ACCESS_SHADER_WRITE = 1U << 3U,
    NPH_GPU_ACCESS_COLOR_READ = 1U << 4U,
    NPH_GPU_ACCESS_COLOR_WRITE = 1U << 5U,
    NPH_GPU_ACCESS_DEPTH_READ = 1U << 6U,
    NPH_GPU_ACCESS_DEPTH_WRITE = 1U << 7U,
    NPH_GPU_ACCESS_TRANSFER_READ = 1U << 8U,
    NPH_GPU_ACCESS_TRANSFER_WRITE = 1U << 9U,
    NPH_GPU_ACCESS_HOST_READ = 1U << 10U,
    NPH_GPU_ACCESS_HOST_WRITE = 1U << 11U,
    NPH_GPU_ACCESS_MEMORY_READ = 1U << 12U,
    NPH_GPU_ACCESS_MEMORY_WRITE = 1U << 13U,
    NPH_GPU_ACCESS_ACCEL_READ = 1U << 14U,  // ray queries, builds reading bottom levels or scratch
    NPH_GPU_ACCESS_ACCEL_WRITE = 1U << 15U, // builds writing a structure or scratch
} NphGpuAccess;

typedef enum NphGpuLayout {
    NPH_GPU_LAYOUT_UNDEFINED = 0, // contents may be discarded by the next transition
    NPH_GPU_LAYOUT_GENERAL,       // storage images, and anything else
    NPH_GPU_LAYOUT_COLOR_ATTACHMENT,
    NPH_GPU_LAYOUT_DEPTH_ATTACHMENT,
    NPH_GPU_LAYOUT_DEPTH_READ_ONLY, // depth test without write, or sampled depth
    NPH_GPU_LAYOUT_SHADER_READ_ONLY,
    NPH_GPU_LAYOUT_TRANSFER_SRC,
    NPH_GPU_LAYOUT_TRANSFER_DST,
    NPH_GPU_LAYOUT_PRESENT,
    NPH_GPU_LAYOUT_COUNT
} NphGpuLayout;

const char *nph_gpu_layout_name(NphGpuLayout layout);

// ---------------------------------------------------------------------------
// Attachments and clears (shared by commands.h and render_graph.h)
// ---------------------------------------------------------------------------

typedef enum NphGpuLoadOp {
    NPH_GPU_LOAD_OP_LOAD = 0,
    NPH_GPU_LOAD_OP_CLEAR,
    NPH_GPU_LOAD_OP_DONT_CARE,
} NphGpuLoadOp;

typedef enum NphGpuStoreOp {
    NPH_GPU_STORE_OP_STORE = 0,
    NPH_GPU_STORE_OP_DONT_CARE,
    // The store does not access the attachment (Vulkan 1.3
    // VK_ATTACHMENT_STORE_OP_NONE): for attachments a pass only reads. STORE
    // and DONT_CARE are writes as far as synchronization goes; a read-only
    // depth attachment always stores with NONE (nph_gpu_cmd_begin_rendering).
    NPH_GPU_STORE_OP_NONE,
} NphGpuStoreOp;

typedef union NphGpuClearValue {
    f32 color_f32[4];
    u32 color_u32[4];
    i32 color_i32[4];
    struct {
        f32 depth;
        u32 stencil;
    } ds;
} NphGpuClearValue;

NPH_INLINE NphGpuClearValue nph_gpu_clear_color(f32 r, f32 g, f32 b, f32 a)
{
    NphGpuClearValue v;
    v.color_f32[0] = r;
    v.color_f32[1] = g;
    v.color_f32[2] = b;
    v.color_f32[3] = a;
    return v;
}

// Reverse-Z (ADR-0008): the far plane is depth 0, so a cleared depth buffer
// holds 0 and the test is GREATER.
NPH_INLINE NphGpuClearValue nph_gpu_clear_depth(f32 depth, u32 stencil)
{
    NphGpuClearValue v;
    v.color_u32[0] = 0U;
    v.color_u32[1] = 0U;
    v.color_u32[2] = 0U;
    v.color_u32[3] = 0U;
    v.ds.depth = depth;
    v.ds.stencil = stencil;
    return v;
}

typedef enum NphGpuIndexType {
    NPH_GPU_INDEX_U16 = 0,
    NPH_GPU_INDEX_U32,
} NphGpuIndexType;

#ifdef __cplusplus
}
#endif
