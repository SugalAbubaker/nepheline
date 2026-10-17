// SPDX-License-Identifier: Apache-2.0
// 64-bit unsigned storage images in the global bindless set (binding 1,
// the storage-image array, aliased with the declarations of
// nph_bindless.glsl): the visibility buffer of the GPU-driven renderer
// (docs/render.md "The production path"). A separate include because a
// module that declares a 64-bit image needs the Int64ImageEXT capability,
// which only devices with VK_EXT_shader_image_atomic_int64 accept.
// Image atomics need a format qualifier, hence r64ui.
#ifndef NPH_BINDLESS_INT64_GLSL
#define NPH_BINDLESS_INT64_GLSL

#extension GL_EXT_shader_image_int64 : require

layout(set = 0, binding = 1, r64ui) uniform u64image2D nph_u64images_2d[];

#endif
