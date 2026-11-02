// SPDX-License-Identifier: Apache-2.0
// The global bindless descriptor set (engine/gpu/src/bindless.c), declared
// for every shader. Shaders receive image and sampler indices and buffer
// addresses through push constants and reach everything through these
// arrays (CLAUDE.md §8.5 "Bindless").
//
//   set 0, binding 0: sampled images (texture2D and friends alias the slot)
//   set 0, binding 1: storage images (one slot per mip)
//   set 0, binding 2: samplers
//
// Buffers are reached through GL_EXT_buffer_reference with the address in
// the push constants; declare them with NPH_BUFFER_REF(Name) { ... }.
#ifndef NPH_BINDLESS_GLSL
#define NPH_BINDLESS_GLSL

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_image_load_formatted : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D nph_textures_2d[];
layout(set = 0, binding = 0) uniform texture2DArray nph_textures_2d_array[];
layout(set = 0, binding = 0) uniform textureCube nph_textures_cube[];
layout(set = 0, binding = 0) uniform texture3D nph_textures_3d[];
layout(set = 0, binding = 0) uniform utexture2D nph_utextures_2d[];

layout(set = 0, binding = 1) uniform image2D nph_images_2d[];
layout(set = 0, binding = 1) uniform image2DArray nph_images_2d_array[];
layout(set = 0, binding = 1) uniform image3D nph_images_3d[];
layout(set = 0, binding = 1) uniform uimage2D nph_uimages_2d[];

layout(set = 0, binding = 2) uniform sampler nph_samplers[];

// A buffer reference block with std430 layout at 16-byte alignment.
#define NPH_BUFFER_REF(Name) layout(buffer_reference, scalar, buffer_reference_align = 16) buffer Name

// Sampling helpers: `tex` and `smp` are bindless indices (non-uniform safe).
vec4 nph_sample_2d(uint tex, uint smp, vec2 uv)
{
    return texture(sampler2D(nph_textures_2d[nonuniformEXT(tex)], nph_samplers[nonuniformEXT(smp)]), uv);
}

vec4 nph_sample_2d_lod(uint tex, uint smp, vec2 uv, float lod)
{
    return textureLod(sampler2D(nph_textures_2d[nonuniformEXT(tex)], nph_samplers[nonuniformEXT(smp)]), uv, lod);
}

vec4 nph_sample_2d_grad(uint tex, uint smp, vec2 uv, vec2 ddx_uv, vec2 ddy_uv)
{
    return textureGrad(sampler2D(nph_textures_2d[nonuniformEXT(tex)], nph_samplers[nonuniformEXT(smp)]), uv, ddx_uv, ddy_uv);
}

vec4 nph_fetch_2d(uint tex, ivec2 texel, int lod)
{
    return texelFetch(nph_textures_2d[nonuniformEXT(tex)], texel, lod);
}

#endif
