// SPDX-License-Identifier: Apache-2.0
#pragma once

// KHR_materials_pbrSpecularGlossiness, converted to the metallic-roughness
// model the runtime shades. The conversion is the one the extension's
// specification publishes (Khronos, KHR_materials_pbrSpecularGlossiness,
// "examples/convert-between-workflows", after Babylon.js), in linear light:
//
// - specular-glossiness splits reflected light into a diffuse colour and a
//   specular one (F0); metallic-roughness derives both from one base colour,
//   a dielectric reflecting 0.04 (F0) and a metal its base colour, with the
//   diffuse colour the base colour times (1 - 0.04) (1 - metallic);
// - the metallic share is the root of the quadratic those relations give
//   between the perceived brightnesses (sqrt(0.299 r^2 + 0.587 g^2 +
//   0.114 b^2)) of the diffuse and specular colours, 0 when the specular is
//   below a dielectric's;
// - the base colour blends the colour read from the diffuse and the one
//   read from the specular by the metallic share squared;
// - roughness is one minus glossiness; the diffuse alpha is the opacity.
//
// Exact for dielectrics (a 0.04 specular gives the diffuse colour back with
// metallic 0) and for metals (a black diffuse gives the specular colour,
// metallic 1); between, the brightness model decides, as in the source.
//
// Thread-safety: pure functions, NPH_THREADSAFE.

#include <nepheline/core/result.h>
#include <nepheline/math/vec.h>

#include <cook/image.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CookSpecGloss {
    NphVec4 diffuse;  // linear RGBA (the diffuse factor, alpha the opacity)
    NphVec3 specular; // linear RGB (the specular factor)
    f32 glossiness;
} CookSpecGloss;

typedef struct CookMetalRough {
    NphVec4 base_color; // linear RGBA
    f32 metallic;
    f32 roughness;
} CookMetalRough;

// One material's (or one texel's) conversion. Inputs outside [0, 1] are
// clamped.
CookMetalRough cook_specgloss_to_metalrough(CookSpecGloss sg);

// The textures of a material that has them: `diffuse` (RGBA8, sRGB colour
// and linear alpha) and `specgloss` (RGBA8, sRGB specular colour and
// linear glossiness in alpha), either NULL when the material has none,
// times `factors`, converted texel by texel into `out_base` (RGBA8, sRGB
// colour, linear alpha) and `out_mr` (RGBA8, linear: G roughness, B
// metallic, R and A 255). The outputs take the larger extent of the inputs
// on each axis; an input of another extent is sampled bilinearly (in
// linear light) at the output's texel centres, wrapping as a repeating
// texture does. NPH_RESULT_INVALID_ARGUMENT when both images are NULL or one
// is not RGBA8.
NPH_MUST_USE NphResult cook_specgloss_textures(NphAllocator *alloc, const CookImage *diffuse,
                                               const CookImage *specgloss,
                                               const CookSpecGloss *factors, CookImage *out_base,
                                               CookImage *out_mr);

#ifdef __cplusplus
}
#endif
