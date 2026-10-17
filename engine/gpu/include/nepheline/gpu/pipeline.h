// SPDX-License-Identifier: Apache-2.0
#pragma once

// Graphics and compute pipelines (CLAUDE.md §8.5 "Pipelines: a pipeline
// cache persisted to disk ... Shaders hot reload").
//
// Pipelines are built against the global layout and dynamic rendering, so
// a graphics description is the shader stages, the fixed-function state
// the engine uses, and the attachment formats; there is no vertex input
// state (vertex pulling) and no render pass. Viewport and scissor are
// always dynamic.
//
// The pipeline cache is one VkPipelineCache loaded from
// NphGpuDeviceDesc.pipeline_cache_path at device creation (after
// validating the file's checksum and the driver's header) and written
// back on nph_gpu_pipeline_cache_save and at device destruction, through
// a temporary file and a rename so a crash never leaves a torn file.
//
// Hot reload: a pipeline remembers its description; nph_gpu_pipeline_reload
// rebuilds it from the shaders' current modules and retires the old
// pipeline when the frame completes. The handle does not change, so
// nothing that holds it needs to know.
//
// Thread-safety: NPH_THREADSAFE through the pipeline pool's lock; pipeline
// creation itself may take milliseconds and is meant for loading jobs.

#include <nepheline/gpu/shader.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphGpuTopology {
    NPH_GPU_TOPOLOGY_TRIANGLE_LIST = 0,
    NPH_GPU_TOPOLOGY_TRIANGLE_STRIP,
    NPH_GPU_TOPOLOGY_LINE_LIST,
    NPH_GPU_TOPOLOGY_LINE_STRIP,
    NPH_GPU_TOPOLOGY_POINT_LIST,
} NphGpuTopology;

typedef enum NphGpuCullMode {
    NPH_GPU_CULL_NONE = 0,
    NPH_GPU_CULL_BACK,
    NPH_GPU_CULL_FRONT,
} NphGpuCullMode;

// Right-handed, Y-up with the Y flip in the projection (ADR-0008) makes
// counter-clockwise triangles front-facing, so CCW is the default (0).
typedef enum NphGpuFrontFace {
    NPH_GPU_FRONT_FACE_CCW = 0,
    NPH_GPU_FRONT_FACE_CW,
} NphGpuFrontFace;

typedef enum NphGpuPolygonMode {
    NPH_GPU_POLYGON_FILL = 0,
    NPH_GPU_POLYGON_LINE, // requires NphGpuFeatures.fill_mode_non_solid
} NphGpuPolygonMode;

typedef enum NphGpuBlendFactor {
    NPH_GPU_BLEND_ZERO = 0,
    NPH_GPU_BLEND_ONE,
    NPH_GPU_BLEND_SRC_COLOR,
    NPH_GPU_BLEND_ONE_MINUS_SRC_COLOR,
    NPH_GPU_BLEND_DST_COLOR,
    NPH_GPU_BLEND_ONE_MINUS_DST_COLOR,
    NPH_GPU_BLEND_SRC_ALPHA,
    NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA,
    NPH_GPU_BLEND_DST_ALPHA,
    NPH_GPU_BLEND_ONE_MINUS_DST_ALPHA,
} NphGpuBlendFactor;

typedef enum NphGpuBlendOp {
    NPH_GPU_BLEND_OP_ADD = 0,
    NPH_GPU_BLEND_OP_SUBTRACT,
    NPH_GPU_BLEND_OP_REVERSE_SUBTRACT,
    NPH_GPU_BLEND_OP_MIN,
    NPH_GPU_BLEND_OP_MAX,
} NphGpuBlendOp;

#define NPH_GPU_COLOR_MASK_R   1U
#define NPH_GPU_COLOR_MASK_G   2U
#define NPH_GPU_COLOR_MASK_B   4U
#define NPH_GPU_COLOR_MASK_A   8U
#define NPH_GPU_COLOR_MASK_ALL 15U

// Zero-filled: no blending, all channels written.
typedef struct NphGpuBlendState {
    b32 enable;
    NphGpuBlendFactor src_color;
    NphGpuBlendFactor dst_color;
    NphGpuBlendOp color_op;
    NphGpuBlendFactor src_alpha;
    NphGpuBlendFactor dst_alpha;
    NphGpuBlendOp alpha_op;
    u32 write_mask; // NPH_GPU_COLOR_MASK_* bits; 0 means all
} NphGpuBlendState;

// Zero-filled: no depth test, no write. With reverse-Z (ADR-0008) the
// usual test is GREATER (or GREATER_OR_EQUAL) against a buffer cleared
// to 0.
typedef struct NphGpuDepthState {
    b32 test;
    b32 write;
    NphGpuCompareOp compare;
    b32 bias_enable;
    f32 bias_constant;
    f32 bias_slope;
    f32 bias_clamp;
    b32 clamp; // depth clamp (requires NphGpuFeatures.depth_clamp)
} NphGpuDepthState;

typedef struct NphGpuGraphicsPipelineDesc {
    // Either vertex (+ optional fragment), or mesh (+ optional task and
    // fragment). A missing fragment shader makes a depth-only pipeline.
    NphGpuShader vertex;
    NphGpuShader fragment;
    NphGpuShader task;
    NphGpuShader mesh;
    NphGpuTopology topology;
    NphGpuPolygonMode polygon_mode;
    NphGpuCullMode cull_mode;
    NphGpuFrontFace front_face;
    u32 samples; // 0: 1
    NphGpuFormat color_formats[NPH_GPU_MAX_COLOR_ATTACHMENTS];
    u32 color_count;
    NphGpuBlendState blend[NPH_GPU_MAX_COLOR_ATTACHMENTS];
    NphGpuFormat depth_format; // UNDEFINED: no depth attachment
    NphGpuDepthState depth;
    const char *name;
} NphGpuGraphicsPipelineDesc;

// Specialization constants a compute pipeline can set: the values of the
// shader's constants with constant_id 0 to count - 1.
#define NPH_GPU_MAX_SPECIALIZATION 8U

typedef struct NphGpuComputePipelineDesc {
    NphGpuShader shader;
    // Values for the shader's specialization constants with constant_id 0
    // to specialization_count - 1, 32 bits each (a uint, an int, a float's
    // bits, or a bool as 0 or 1); constants past the count keep their
    // defaults. One module thus gives pipelines compiled for different
    // values, each with its dead branches removed and registers allocated
    // for what remains.
    u32 specialization[NPH_GPU_MAX_SPECIALIZATION];
    u32 specialization_count;
    const char *name;
} NphGpuComputePipelineDesc;

// NPH_RESULT_INVALID_ARGUMENT for a stage mismatch (a non-vertex shader in
// `vertex`, both vertex and mesh set, no stage at all), more than
// NPH_GPU_MAX_SPECIALIZATION constants, or an unsupported state (line
// polygons without the feature); NPH_RESULT_FULL past
// max_pipelines; NPH_RESULT_UNKNOWN when the driver rejects the pipeline
// (logged).
NPH_MUST_USE NphResult nph_gpu_pipeline_create_graphics(NphGpuDevice *dev,
                                                        const NphGpuGraphicsPipelineDesc *desc,
                                                        NphGpuPipeline *out);
NPH_MUST_USE NphResult nph_gpu_pipeline_create_compute(NphGpuDevice *dev,
                                                       const NphGpuComputePipelineDesc *desc,
                                                       NphGpuPipeline *out);

// Rebuilds the pipeline from its description and the shaders' current
// modules (after nph_gpu_shader_reload). On failure the previous pipeline
// stays in use and the error is returned.
NPH_MUST_USE NphResult nph_gpu_pipeline_reload(NphGpuDevice *dev, NphGpuPipeline pipeline);

void nph_gpu_pipeline_destroy(NphGpuDevice *dev, NphGpuPipeline pipeline);
bool nph_gpu_pipeline_is_valid(const NphGpuDevice *dev, NphGpuPipeline pipeline);
bool nph_gpu_pipeline_is_compute(const NphGpuDevice *dev, NphGpuPipeline pipeline);

// The largest push constant block among the pipeline's stages.
u32 nph_gpu_pipeline_push_constant_bytes(const NphGpuDevice *dev, NphGpuPipeline pipeline);

// Times the pipeline was rebuilt.
u32 nph_gpu_pipeline_generation(const NphGpuDevice *dev, NphGpuPipeline pipeline);

// ---------------------------------------------------------------------------
// Pipeline cache
// ---------------------------------------------------------------------------

// Writes the cache to NphGpuDeviceDesc.pipeline_cache_path.
// NPH_RESULT_UNSUPPORTED without a path; NPH_RESULT_IO_ERROR from the file
// system.
NPH_MUST_USE NphResult nph_gpu_pipeline_cache_save(NphGpuDevice *dev);

// Whether the cache was primed from the file at device creation.
bool nph_gpu_pipeline_cache_loaded(const NphGpuDevice *dev);

// Bytes the driver's cache currently holds.
u64 nph_gpu_pipeline_cache_size(const NphGpuDevice *dev);

#ifdef __cplusplus
}
#endif
