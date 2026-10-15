// SPDX-License-Identifier: Apache-2.0
#pragma once

// Source images for the cooker (CLAUDE.md §8.8 "Textures (via stb_image):
// mip generation in linear space ... normal-map handling").
//
// A CookImage is a plain RGBA raster: 8-bit (as decoded from PNG, JPEG,
// TGA, BMP) or 32-bit float (from Radiance HDR, and the working format for
// every operation that filters). Decoding is the only thing stb_image
// does; conversions, mip filtering, and normal-map handling are the
// engine's own, so they are deterministic and documented:
//
// - sRGB decoding and encoding use the exact piecewise transfer function
//   (IEC 61966-2-1); alpha is never transfer-coded.
// - Downsampling is the exact area average: each destination texel is the
//   mean of the source region it covers, with fractional weights where an
//   odd size splits a texel (a 5-wide row becomes a 2-wide row, each texel
//   covering two and a half), so every level keeps the image's mean and
//   nothing shifts. It runs in linear light, which is why the pipeline
//   converts to float first (mips of sRGB data filtered in gamma space
//   come out too dark).
// - Normal maps are filtered as vectors and renormalised after each
//   level, so a mip's normals stay unit length.
//
// Thread-safety: an image belongs to one thread; the functions are pure
// in their inputs and NPH_THREADSAFE.

#include <nepheline/core/result.h>

#include <cook/bc.h>
#include <cook/cook.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CookImageFormat {
    COOK_IMAGE_RGBA8 = 0,   // 4 bytes per texel
    COOK_IMAGE_RGBA32F = 1, // 16 bytes per texel
} CookImageFormat;

typedef struct CookImage {
    NphAllocator *alloc;
    u32 width;
    u32 height;
    CookImageFormat format;
    u32 source_channels; // channels the file had (1..4); 4 after any conversion
    void *pixels;        // row-major, top row first, tightly packed
    u64 size;            // bytes
} CookImage;

#define COOK_IMAGE_MAX_EXTENT 16384U

NPH_INLINE u32 cook_image_bytes_per_texel(CookImageFormat format)
{
    return format == COOK_IMAGE_RGBA32F ? 16U : 4U;
}

// A zero-filled image of the given size. NPH_RESULT_INVALID_ARGUMENT for
// a zero or oversized extent.
NPH_MUST_USE NphResult cook_image_create(NphAllocator *alloc, u32 width, u32 height,
                                         CookImageFormat format, CookImage *out);

// Releases the pixels. Safe on a zero-filled image.
void cook_image_free(CookImage *img);

// Decodes an image file held in memory. 8-bit formats produce RGBA8 (gray
// and gray+alpha are expanded), Radiance HDR produces RGBA32F.
// NPH_RESULT_PARSE_ERROR when the bytes are not a supported image,
// NPH_RESULT_UNSUPPORTED past COOK_IMAGE_MAX_EXTENT.
NPH_MUST_USE NphResult cook_image_decode(NphAllocator *alloc, const void *data, u64 size,
                                         CookImage *out);

// The extent and channel count an image file declares, from its header
// alone (nothing is decoded or allocated). NPH_RESULT_PARSE_ERROR when the
// bytes are not a supported image. Lets a caller refuse an image before
// paying for it (a fuzzer, or a cook with a memory budget).
NPH_MUST_USE NphResult cook_image_info(const void *data, u64 size, u32 *out_width, u32 *out_height,
                                       u32 *out_channels);

// Reads and decodes the file at `path`.
NPH_MUST_USE NphResult cook_image_load(NphAllocator *alloc, const char *path, CookImage *out);

// True when the bytes are a Radiance HDR file.
bool cook_image_data_is_hdr(const void *data, u64 size);

// The sRGB transfer function and its inverse, on [0, 1].
f32 cook_srgb_to_linear(f32 encoded);
f32 cook_linear_to_srgb(f32 linear);

// RGBA8 -> RGBA32F; with `srgb` the colour channels are decoded to
// linear light (alpha is scaled only). A float source is copied.
NPH_MUST_USE NphResult cook_image_to_f32(NphAllocator *alloc, const CookImage *src, bool srgb,
                                         CookImage *out);

// RGBA32F -> RGBA8; with `srgb` the colour channels are encoded. Values
// are clamped to [0, 1] and rounded to nearest. An 8-bit source is copied.
NPH_MUST_USE NphResult cook_image_to_rgba8(NphAllocator *alloc, const CookImage *src, bool srgb,
                                           CookImage *out);

// The next mip level of an RGBA32F image: half the size (rounded down,
// at least 1) by exact area averaging. NPH_RESULT_INVALID_ARGUMENT for a
// 1x1 source or an 8-bit one.
NPH_MUST_USE NphResult cook_image_downsample(NphAllocator *alloc, const CookImage *src,
                                             CookImage *out);

// Halves an RGBA32F image until both extents are at most `max_extent`
// (in place: the image is replaced).
NPH_MUST_USE NphResult cook_image_limit_extent(NphAllocator *alloc, CookImage *img, u32 max_extent);

// Treats RGB as a tangent-space normal in [0, 1] encoding, renormalises
// each texel, and writes it back (RGBA32F in place). Zero vectors become
// +Z.
void cook_image_renormalize_normals(CookImage *img);

// Copies a 4x4 block of RGBA8 texels at (x, y) into `out` (16 texels, row
// major), replicating the last row and column past the image's edge, so
// partial blocks at the right and bottom edges encode cleanly.
void cook_image_block_rgba8(const CookImage *img, u32 x, u32 y, CookBlockRgba8 *out);

// As cook_image_block_rgba8 for RGBA32F sources, converting the colour
// channels to half-float bits (for BC6H).
void cook_image_block_half(const CookImage *img, u32 x, u32 y, CookBlockRgbHalf *out);

#ifdef __cplusplus
}
#endif
