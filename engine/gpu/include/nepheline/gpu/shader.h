// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shader modules and SPIR-V reflection (CLAUDE.md §8.5 "Pipeline layouts
// come from SPIR-V reflection (our own minimal parser). Shaders hot
// reload").
//
// Every pipeline uses the one global layout (the bindless set plus
// NPH_GPU_PUSH_CONSTANT_BYTES of push constants), so reflection does not
// build layouts; it verifies that a shader fits the layout and reports
// what the pipeline needs to know: the stage, the entry point, the push
// constant block size, the compute workgroup size, which bindless arrays
// the shader uses, and the fragment outputs. A shader that declares a
// descriptor outside the bindless set, or a vertex input (vertex data is
// pulled from buffer addresses), is rejected with NPH_RESULT_UNSUPPORTED.
//
// The parser walks the SPIR-V instruction stream once with bounds checks
// on every word; malformed input yields NPH_RESULT_CORRUPT and never a
// crash (it is fuzzed, tests/fuzz/fuzz_spirv.c).
//
// Hot reload: nph_gpu_shader_reload replaces the module behind a handle;
// pipelines are then rebuilt with nph_gpu_pipeline_reload. The old
// module and pipelines are released when the current frame completes.
//
// Thread-safety: reflect is NPH_THREADSAFE and pure; create, reload,
// and destroy are NPH_THREADSAFE through the shader pool's lock.

#include <nepheline/gpu/resources.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphGpuShaderStage {
    NPH_GPU_SHADER_VERTEX = 0,
    NPH_GPU_SHADER_FRAGMENT,
    NPH_GPU_SHADER_COMPUTE,
    NPH_GPU_SHADER_TASK,
    NPH_GPU_SHADER_MESH,
    NPH_GPU_SHADER_STAGE_COUNT
} NphGpuShaderStage;

const char *nph_gpu_shader_stage_name(NphGpuShaderStage stage);

// Which bindless bindings a shader references (set 0).
typedef enum NphGpuBindlessUse {
    NPH_GPU_BINDLESS_SAMPLED_IMAGES = 1U << 0U, // binding 0
    NPH_GPU_BINDLESS_STORAGE_IMAGES = 1U << 1U, // binding 1
    NPH_GPU_BINDLESS_SAMPLERS = 1U << 2U,       // binding 2
} NphGpuBindlessUse;

#define NPH_GPU_ENTRY_POINT_MAX 64U

typedef struct NphGpuShaderInfo {
    NphGpuShaderStage stage;
    char entry_point[NPH_GPU_ENTRY_POINT_MAX];
    u32 spirv_version_major;
    u32 spirv_version_minor;
    u32 push_constant_bytes; // size of the push constant block (0: none)
    u32 workgroup_size[3];   // compute, task, and mesh shaders (0 when unknown)
    u32 bindless_uses;       // NphGpuBindlessUse bits
    u32 color_output_mask;   // fragment: bit i set for an output at location i
    u32 capability_count;    // OpCapability instructions seen
    b32 uses_device_address; // PhysicalStorageBufferAddresses capability
} NphGpuShaderInfo;

// Reflects a SPIR-V module. `scratch` provides the per-id table the parser
// needs for the duration of the call (released before returning).
// NPH_RESULT_CORRUPT for malformed SPIR-V; NPH_RESULT_UNSUPPORTED for a
// valid module that does not fit the engine's layout (reason logged);
// NPH_RESULT_OUT_OF_MEMORY when `scratch` is exhausted.
NPH_MUST_USE NphResult nph_gpu_spirv_reflect(const void *spirv, u64 size, NphAllocator *scratch,
                                             NphGpuShaderInfo *out);

// Creates a module from SPIR-V (reflected first; the same errors apply).
// NPH_RESULT_FULL past max_shaders.
NPH_MUST_USE NphResult nph_gpu_shader_create(NphGpuDevice *dev, const void *spirv, u64 size,
                                             const char *name, NphGpuShader *out);

// Reads `path` and creates a module from it. NPH_RESULT_NOT_FOUND or
// NPH_RESULT_IO_ERROR from the file system.
NPH_MUST_USE NphResult nph_gpu_shader_load(NphGpuDevice *dev, const char *path, const char *name,
                                           NphGpuShader *out);

// Replaces the module behind `shader` with new SPIR-V (same stage
// required: NPH_RESULT_INVALID_ARGUMENT otherwise). Pipelines keep using
// the old module until nph_gpu_pipeline_reload.
NPH_MUST_USE NphResult nph_gpu_shader_reload(NphGpuDevice *dev, NphGpuShader shader,
                                             const void *spirv, u64 size);

void nph_gpu_shader_destroy(NphGpuDevice *dev, NphGpuShader shader);
bool nph_gpu_shader_is_valid(const NphGpuDevice *dev, NphGpuShader shader);
const NphGpuShaderInfo *nph_gpu_shader_info(const NphGpuDevice *dev, NphGpuShader shader);

// Times the module was replaced, for hot-reload bookkeeping.
u32 nph_gpu_shader_generation(const NphGpuDevice *dev, NphGpuShader shader);

#ifdef __cplusplus
}
#endif
