// SPDX-License-Identifier: Apache-2.0
#pragma once

// Acceleration structures for ray queries (VK_KHR_acceleration_structure,
// VK_KHR_ray_query; docs/gpu.md "Ray queries"). Available when
// NphGpuFeatures.ray_query is set; every function here asserts it.
//
// A bottom-level structure holds triangles; a top-level one holds instances
// of bottom-level ones, read from a buffer of NphGpuAccelInstance records
// that the GPU itself may write. Each structure lives in a buffer the
// device allocates for it. Shaders reach a top-level structure by its
// device address (GLSL: `accelerationStructureEXT(address)` under
// GL_EXT_ray_query), as they reach everything else in the engine, so no
// descriptor is involved.
//
// Building is recorded into a command buffer of a graphics or compute
// queue (nph_gpu_cmd_build_blas, nph_gpu_cmd_build_tlas), with a scratch
// buffer the caller provides: nph_gpu_accel_*_sizes say how large the
// structure and the scratch must be, and the scratch's address (the
// buffer's address plus the offset) must be a multiple of
// NphGpuLimits.accel_scratch_alignment. Inputs (vertices, indices,
// instances) need NPH_GPU_BUFFER_USAGE_ACCEL_INPUT, scratch buffers
// NPH_GPU_BUFFER_USAGE_STORAGE. In the render graph, a pass declares the
// structure's buffer (nph_gpu_accel_buffer) with NPH_GPU_RG_USE_ACCEL_BUILD
// when it builds it and NPH_GPU_RG_USE_ACCEL_READ when its shaders trace
// against it, the inputs with NPH_GPU_RG_USE_ACCEL_INPUT, and the scratch
// with NPH_GPU_RG_USE_ACCEL_SCRATCH; a top-level structure depends on its
// instances' bottom-level ones, which a build pass declares read with
// NPH_GPU_RG_USE_ACCEL_READ too.
//
// Destruction is deferred like a buffer's.
//
// Thread-safety: NPH_THREADSAFE for creation, destruction, and the
// accessors; the command functions follow their command buffer's rules.

#include <nepheline/gpu/commands.h>
#include <nepheline/gpu/device.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphGpuAccel {
    u32 value;
} NphGpuAccel;

#define NPH_GPU_ACCEL_NULL ((NphGpuAccel){0U})

typedef enum NphGpuAccelType {
    NPH_GPU_ACCEL_BOTTOM = 0, // triangles
    NPH_GPU_ACCEL_TOP = 1,    // instances of bottom-level structures
} NphGpuAccelType;

typedef enum NphGpuAccelBuildFlags {
    NPH_GPU_ACCEL_PREFER_TRACE = 1U << 0U, // a slower build for faster rays (static geometry)
    NPH_GPU_ACCEL_PREFER_BUILD = 1U << 1U, // a faster build (rebuilt every frame)
    // Its compacted size can be queried and it copied into a structure of
    // that size (nph_gpu_cmd_query_accel_compacted_size,
    // nph_gpu_cmd_copy_accel_compact).
    NPH_GPU_ACCEL_ALLOW_COMPACTION = 1U << 2U,
} NphGpuAccelBuildFlags;

// One geometry of a bottom-level structure: an indexed triangle list.
typedef struct NphGpuAccelTriangles {
    NphGpuBuffer vertices; // positions from vertex_offset, vertex_stride bytes apart
    u64 vertex_offset;
    u32 vertex_stride;
    u32 vertex_count;           // one more than the largest index
    NphGpuFormat vertex_format; // one with NPH_GPU_FORMAT_FEATURE_ACCEL_VERTEX; a fourth
                                // component is ignored
    NphGpuBuffer indices;       // u32 indices, three per triangle
    u64 index_offset;           // a multiple of 4
    u32 triangle_count;
    b32 opaque; // rays never consider these triangles transparent
} NphGpuAccelTriangles;

// A top-level structure's instance, laid out as VkAccelerationStructureInstanceKHR
// (64 bytes), so shaders and the CPU may write them alike.
typedef struct NphGpuAccelInstance {
    f32 transform[3][4];  // rows of the object-to-world affine transform
    u32 index_and_mask;   // bits 0-23: the custom index a hit reports; bits 24-31: the ray mask
    u32 offset_and_flags; // bits 0-23: shader binding table offset (0); bits 24-31:
                          // NPH_GPU_ACCEL_INSTANCE_* flags
    u64 blas;             // nph_gpu_accel_address of a bottom-level structure (0: none)
} NphGpuAccelInstance;

NPH_STATIC_ASSERT(sizeof(NphGpuAccelInstance) == 64U, "an instance is 64 bytes");

// Instance flags (NphGpuAccelInstance.offset_and_flags >> 24).
#define NPH_GPU_ACCEL_INSTANCE_NO_FACE_CULL 1U // both sides of the triangles are hit
#define NPH_GPU_ACCEL_INSTANCE_FLIP_FACING  2U // clockwise triangles face forward
#define NPH_GPU_ACCEL_INSTANCE_OPAQUE       4U // every triangle opaque
#define NPH_GPU_ACCEL_INSTANCE_NO_OPAQUE    8U // no triangle opaque

typedef struct NphGpuAccelSizes {
    u64 accel;   // bytes of the structure (NphGpuAccelDesc.size)
    u64 scratch; // bytes of scratch a build needs
} NphGpuAccelSizes;

// The sizes of a bottom-level structure over `count` geometries, built with
// NphGpuAccelBuildFlags `flags`. NPH_RESULT_INVALID_ARGUMENT for no
// geometry or a vertex format the device cannot build from.
NPH_MUST_USE NphResult nph_gpu_accel_blas_sizes(NphGpuDevice *dev,
                                                const NphGpuAccelTriangles *geometries, u32 count,
                                                u32 flags, NphGpuAccelSizes *out);

// The sizes of a top-level structure of up to `max_instances` instances.
NPH_MUST_USE NphResult nph_gpu_accel_tlas_sizes(NphGpuDevice *dev, u32 max_instances, u32 flags,
                                                NphGpuAccelSizes *out);

typedef struct NphGpuAccelDesc {
    NphGpuAccelType type;
    u64 size; // NphGpuAccelSizes.accel
    const char *name;
} NphGpuAccelDesc;

// Creates an empty structure with its buffer; its contents are undefined
// until a build. NPH_RESULT_INVALID_ARGUMENT for a zero size;
// NPH_RESULT_FULL past max_accels; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_gpu_accel_create(NphGpuDevice *dev, const NphGpuAccelDesc *desc,
                                            NphGpuAccel *out);
void nph_gpu_accel_destroy(NphGpuDevice *dev, NphGpuAccel accel);
bool nph_gpu_accel_is_valid(const NphGpuDevice *dev, NphGpuAccel accel);

// The device address shaders and NphGpuAccelInstance.blas use.
u64 nph_gpu_accel_address(const NphGpuDevice *dev, NphGpuAccel accel);
// The buffer the structure lives in, for render-graph declarations.
NphGpuBuffer nph_gpu_accel_buffer(const NphGpuDevice *dev, NphGpuAccel accel);

// Records a full build of a bottom-level structure from `count` geometries
// (the same ones its sizes were queried with, or no larger).
void nph_gpu_cmd_build_blas(NphGpuCmd *cmd, NphGpuAccel dst, const NphGpuAccelTriangles *geometries,
                            u32 count, u32 flags, NphGpuBuffer scratch, u64 scratch_offset);

// Records a full build of a top-level structure from `count` instances (at
// most its max_instances) read at `offset` of `instances` (16-byte aligned).
void nph_gpu_cmd_build_tlas(NphGpuCmd *cmd, NphGpuAccel dst, NphGpuBuffer instances, u64 offset,
                            u32 count, u32 flags, NphGpuBuffer scratch, u64 scratch_offset);

// Records a query of a bottom level's compacted size into the frame's
// queries (NphGpuDeviceDesc.accel_queries_per_frame): `accel` built with
// NPH_GPU_ACCEL_ALLOW_COMPACTION, its build ordered before by a barrier
// from acceleration-structure writes to acceleration-structure reads at
// NPH_GPU_STAGE_ACCEL_BUILD. Returns the query's index, or
// NPH_GPU_INVALID_INDEX when the frame's queries are taken (query it in a
// later frame). The result: nph_gpu_frame_accel_sizes, from the frame
// NPH_GPU_FRAMES_IN_FLIGHT later. Thread-safe across command buffers.
u32 nph_gpu_cmd_query_accel_compacted_size(NphGpuCmd *cmd, NphGpuAccel accel);

// The compacted sizes queried by the frame that last completed in the
// current slot (frame N - NPH_GPU_FRAMES_IN_FLIGHT), indexed by the values
// nph_gpu_cmd_query_accel_compacted_size returned; 0 for a query whose
// command buffer was not submitted. False before such a frame, or without
// ray queries. Valid from nph_gpu_frame_begin to the next.
bool nph_gpu_frame_accel_sizes(const NphGpuDevice *dev, u64 *out_frame_index, const u64 **out_sizes,
                               u32 *out_count);

// Records a copy of `src` (built with NPH_GPU_ACCEL_ALLOW_COMPACTION, the
// build ordered before) into `dst`, compacted: `dst` was created with at
// least the queried compacted size. The copy writes `dst` at
// NPH_GPU_STAGE_ACCEL_BUILD (NPH_GPU_ACCESS_ACCEL_WRITE); `dst` then
// serves wherever `src` did, after a barrier, and `src` may be destroyed.
void nph_gpu_cmd_copy_accel_compact(NphGpuCmd *cmd, NphGpuAccel src, NphGpuAccel dst);

#ifdef __cplusplus
}
#endif
