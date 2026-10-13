// SPDX-License-Identifier: Apache-2.0
// Types shared between C and GLSL (CLAUDE.md §6 "Shared C/GLSL struct
// headers live in shaders/shared/"). Include from C after the math headers
// and from GLSL after nph_bindless.glsl. Structs declared with
// NPH_SHARED_STRUCT use the engine's scalar and vector names on both
// sides; in GLSL they are macros for the built-in types. Every member is
// 4, 8, or 16 bytes with the same alignment in C and in GLSL's scalar
// layout. Keep vec3 out of shared structs; use vec4 or three floats.
#ifndef NPH_SHARED_H
#define NPH_SHARED_H

#ifdef __cplusplus
#error "nph_shared.h is a C and GLSL header"
#endif

#if defined(VULKAN) || defined(GL_core_profile)
// GLSL
#define NPH_SHARED_STRUCT(Name) struct Name
#define u32                     uint
#define i32                     int
#define f32                     float
#define u64                     uint64_t
#define NphVec2                 vec2
#define NphVec4                 vec4
#define NphMat4                 mat4
#else
// C
#include <nepheline/core/types.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/vec.h>
#define NPH_SHARED_STRUCT(Name)                                                                    \
    typedef struct Name Name;                                                                      \
    struct Name
#endif

// Push constants of the gpu test shaders (fits NPH_GPU_PUSH_CONSTANT_BYTES).
NPH_SHARED_STRUCT(NphTestPush)
{
    u64 buffer_address; // test_fill: destination; test_sample: unused
    u32 count;          // elements to write
    u32 value;          // test_fill: multiplier; test_image: pattern seed
    u32 texture_index;  // test_sample
    u32 sampler_index;  // test_sample
    u32 image_index;    // test_image: storage image slot
    u32 pad0;
    NphVec4 color; // test_color: output colour
};

// Push constants of the mesh sample (samples/gpu_mesh).
NPH_SHARED_STRUCT(NphMeshPush)
{
    NphMat4 view_proj;    // 64 bytes
    u64 vertex_address;   // vertex buffer (NphMeshVertex[])
    u64 instance_address; // instance transforms (NphMat4[])
    u32 texture_index;
    u32 sampler_index;
    u32 instance_count;
    f32 time;
};

NPH_SHARED_STRUCT(NphMeshVertex)
{
    NphVec4 position;  // xyz, w unused
    NphVec4 normal_uv; // normal xyz in xyz, u in w
    NphVec4 extra;     // v in x
};

#endif
