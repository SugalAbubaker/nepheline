#pragma once

// Glass whose transmission varies per texel (docs/cook.md "Glass"; ADR-0107): the base colour
// texture the cooker makes for it from KHR_materials_transmission's texture.

#include <nepheline/core/result.h>

#include <cook/image.h>

#ifdef __cplusplus
extern "C" {
#endif

// The base colour texture of glass whose transmission varies per texel: `base` (RGBA8, sRGB
// colour and linear alpha; NULL where the material has none: white and opaque) with its alpha
// times the share of the light behind it each texel keeps, 1 - `factor` * T, T the red channel
// of `transmission` (RGBA8, linear; glTF's transmission texture). The colour stays the input's:
// the renderer scales it by that share texel by texel, and tints what is behind the glass by it
// unscaled (NPH_MATERIAL_FLAG_TRANSMISSION_TEXELS). `factor` is clamped to [0, 1]. The output
// takes the larger extent of the inputs on each axis; an input of another extent is sampled
// bilinearly, in linear light, at the output's texel centres, wrapping as a repeating texture
// does (cook_image_sample_grid), and one of the output's extent is taken texel for texel.
// NPH_RESULT_INVALID_ARGUMENT when `transmission` is NULL, an input is not RGBA8, or `factor` is
// not finite. Allocates `out` (cook_image_free) from `alloc`.
NPH_MUST_USE NphResult cook_glass_texture(NphAllocator *alloc, const CookImage *base,
                                          const CookImage *transmission, f32 factor,
                                          CookImage *out);

#ifdef __cplusplus
}
#endif
