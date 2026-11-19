// SPDX-License-Identifier: Apache-2.0
#pragma once

// Material previews (ADR-0105 decision 7; docs/cook.md "Material previews"): a lit sphere of a
// material source (cook/material.h), drawn on the CPU from the material and its images, for the
// editor's asset browser.
//
// - The images. Each slot's image is decoded, its colour data brought to linear light, reduced
//   in one pass by exact area averages to at most COOK_PREVIEW_IMAGE_EXTENT a side (never a full
//   float copy of a large image), and given its mip levels (cook/image.h cook_image_downsample).
//   A normal map is renormalised at each level, its green flipped first where its import
//   settings say DirectX's convention (cook/texture_import.h).
// - The sphere is seen straight on. Its texture coordinates wrap twice around it, so its front
//   shows one tile whole: u = 1/2 + longitude / pi, v = 1/2 - latitude / pi, through the
//   material's transform (cook_material_source_uv) as the renderer's are, a normal map read in
//   the sphere's own tangent frame (the renderer turns texture coordinates with the transform,
//   not tangents). Four samples a pixel; each texture read is filtered trilinearly at the level
//   its footprint asks for, with the slot's wraps, or nearest where the slot asks.
// - The light is the renderer's on a surface, roughly, in a neutral studio: the renderer's sky
//   model (render/renderer.h NphRenderSkyDesc) made grey, brighter overhead, so that it tints
//   no material, is the environment. Diffuse comes from its irradiance (render/reference.h
//   nph_render_ref_sh9_sky) with the occlusion's bounce (nph_render_ref_ao_bounce), and
//   specular by the split sum (Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013), from
//   its radiance in the reflected direction, blurred toward its irradiance with roughness,
//   times the renderer's DFG terms (nph_render_ref_dfg). A key light from above and to the left
//   adds the renderer's GGX terms (nph_render_ref_d_ggx, _v_smith, _f_schlick). The exposure
//   brings a white diffuse sphere's brightest point out of AgX (the view's default, ADR-0083) at
//   0.65, short of white so that bright colours keep their hue, and the sRGB curve encodes it.
// - An unlit material draws its base colour; emission adds to the light, a value of 1 at half
//   the white point's input; a masked material is cut where its alpha falls below its cutoff; a
//   blended one leaves its alpha in the picture. Outside the sphere the picture is transparent,
//   and its edge's pixels are covered as far as the sphere covers them.
//
// A preview is cached as a texture asset (asset/texture.h: one RGBA8 sRGB level) whose header's
// content hash is the key it was made for (cook_preview_save, cook_preview_read). The reader
// checks the file as the loader checks an asset (asset/format.h) and is a fuzz target.
//
// Thread-safety: pure. A studio is read-only once made, and may be shared between threads.

#include <nepheline/asset/material.h>
#include <nepheline/render/renderer.h>

#include <cook/image.h>
#include <cook/material.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_PREVIEW_SIZE         128U // pixels a side
#define COOK_PREVIEW_BYTES        ((u64)COOK_PREVIEW_SIZE * COOK_PREVIEW_SIZE * 4U)
#define COOK_PREVIEW_IMAGE_EXTENT 256U // texels a side, at most, of an image the sphere reads
#define COOK_PREVIEW_IMAGE_LEVELS 9U   // 256 down to 1
#define COOK_PREVIEW_DFG          32U  // the DFG table's side (NoV by roughness)
// The picture's version: a change to how previews are drawn changes it, and the editor's key
// holds it, so that previews cached before are drawn again.
#define COOK_PREVIEW_VERSION 1U

// The light every preview shares, made once (cook_preview_studio_init).
typedef struct CookPreviewStudio {
    NphRenderSkyDesc sky;
    NphVec3 sh[9];                                    // the sky's irradiance (cd/m^2 sr)
    NphVec2 dfg[COOK_PREVIEW_DFG * COOK_PREVIEW_DFG]; // scale and bias, NoV fastest
    NphVec3 key_direction;                            // toward the light
    NphVec3 key_illuminance;                          // lux, by channel
    f32 exposure;                                     // radiance to the tonemapper's input
    f32 emission;                                     // an emission of 1 as the tonemapper's input
} CookPreviewStudio;

void cook_preview_studio_init(CookPreviewStudio *out);

// One slot's image as the sphere reads it: RGBA32F levels, finest first; for colour data in
// linear light, for the rest as the image holds it (a normal map in [0, 1]).
typedef struct CookPreviewImage {
    CookImage levels[COOK_PREVIEW_IMAGE_LEVELS];
    u32 level_count; // 0: the slot reads no image
} CookPreviewImage;

typedef struct CookPreviewImages {
    CookPreviewImage slots[NPH_MATERIAL_TEXTURE_COUNT];
} CookPreviewImages;

// What a preview of a material reads, in memory: each slot's image file and its import
// settings, NULL where there is none.
typedef struct CookPreviewFiles {
    NphAllocator *alloc;
    void *image[NPH_MATERIAL_TEXTURE_COUNT];
    u64 image_size[NPH_MATERIAL_TEXTURE_COUNT];
    void *settings[NPH_MATERIAL_TEXTURE_COUNT];
    u64 settings_size[NPH_MATERIAL_TEXTURE_COUNT];
} CookPreviewFiles;

// Reads the files of the images `m` names, under the sources' root `root` (blocking I/O). A slot
// whose image cannot be read is left empty: the preview draws without it. NPH_RESULT_OVERFLOW
// when a path does not fit; NPH_RESULT_OUT_OF_MEMORY (nothing is kept).
NPH_MUST_USE NphResult cook_preview_files_read(NphAllocator *alloc, const char *root,
                                               const CookMaterialSource *m, CookPreviewFiles *out);
// Safe on a zero-filled set.
void cook_preview_files_free(CookPreviewFiles *f);

// The images of `files`, decoded, reduced and given their levels, as their slots read them (CPU
// work). An image that does not decode, or settings that do not read, leave the slot empty (the
// image unread) or as the image holds it (the settings unread). NPH_RESULT_OUT_OF_MEMORY (nothing
// is kept).
NPH_MUST_USE NphResult cook_preview_images_make(NphAllocator *alloc, const CookPreviewFiles *files,
                                                CookPreviewImages *out);
// Safe on a zero-filled set.
void cook_preview_images_free(CookPreviewImages *images);

// `src` (RGBA8 or RGBA32F) reduced to at most `max_extent` texels a side as RGBA32F: each texel
// the exact mean of the source block it covers, in linear light where `srgb` (the colour
// channels decoded first; alpha never is). A source within the extent is converted alone.
// NPH_RESULT_INVALID_ARGUMENT for a zero extent; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult cook_preview_reduce(NphAllocator *alloc, const CookImage *src, bool srgb,
                                           u32 max_extent, CookImage *out);

// Draws the preview of `m`, with `images` (NULL: none), into `out` (COOK_PREVIEW_BYTES: RGBA8,
// the colour sRGB-encoded and not premultiplied, the alpha linear; the top row first). The same
// inputs give the same bytes.
void cook_preview_render(const CookPreviewStudio *studio, const CookMaterialSource *m,
                         const CookPreviewImages *images, u8 *out);

// ---------------------------------------------------------------------------
// The cache file
// ---------------------------------------------------------------------------

// Writes `pixels` (COOK_PREVIEW_BYTES) to `path` as a texture asset named `guid`, made for `key`.
// The file's and the writer's errors.
NPH_MUST_USE NphResult cook_preview_save(NphAllocator *alloc, const char *path, NphGuid guid,
                                         u64 key, const u8 *pixels);

// The pixels of the preview file in `bytes`, into `out` (COOK_PREVIEW_BYTES), when it was made for
// `key`. NPH_RESULT_NOT_FOUND for a preview made for another key (stale: nothing is written);
// NPH_RESULT_CORRUPT for bytes that are no preview of this size and version. Never a crash.
NPH_MUST_USE NphResult cook_preview_read(const void *bytes, u64 size, u64 key, u8 *out);

#ifdef __cplusplus
}
#endif
