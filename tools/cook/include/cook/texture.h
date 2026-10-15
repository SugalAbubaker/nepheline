// SPDX-License-Identifier: Apache-2.0
#pragma once

// Texture cooking (CLAUDE.md §8.8): a source image becomes a texture
// asset (engine/asset/texture.h) with a mip chain, block-compressed by
// the encoders in bc.h.
//
// The usage decides everything: colour textures are sRGB-decoded,
// mipped in linear light, and encoded as BC7 (sRGB); linear colour data
// (metallic-roughness, occlusion) as BC7 (UNORM); normal maps keep two
// channels in BC5 and are renormalised per mip; single-channel masks
// become BC4; HDR images become BC6H. Uncompressed output is available
// for every usage (RGBA8 or RGBA16F).
//
// Encoding is deterministic and parallel: with a job system the block
// rows of each level are encoded as a parallel-for; the output is
// identical either way.
//
// Thread-safety: NPH_THREADSAFE for distinct arguments.

#include <nepheline/asset/format.h>
#include <nepheline/asset/texture.h>
#include <nepheline/jobs/jobs.h>

#include <cook/bc.h>
#include <cook/image.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CookTextureUsage {
    COOK_TEXTURE_COLOR = 0,  // sRGB colour: BC7_SRGB (or RGBA8_SRGB)
    COOK_TEXTURE_LINEAR = 1, // linear colour data: BC7_UNORM (or RGBA8_UNORM)
    COOK_TEXTURE_NORMAL = 2, // tangent-space normal map: BC5 of XY (or RG8)
    COOK_TEXTURE_MASK = 3,   // one channel (the red one): BC4 (or R8)
    COOK_TEXTURE_HDR = 4,    // linear HDR: BC6H (or RGBA16F)
} CookTextureUsage;

typedef struct CookTextureSettings {
    CookTextureUsage usage;
    b32 uncompressed; // skip block compression
    b32 no_mips;      // only the base level
    u32 max_extent;   // downsample the base level until it fits (0: no limit)
    NphLzLevel lz;    // compression of the mip blobs
    CookBc7Settings bc7;
} CookTextureSettings;

// Defaults: colour usage, compressed, full mip chain, HIGH LZ.
void cook_texture_settings_default(CookTextureSettings *out);

// The runtime format a usage maps to.
NphTextureFormat cook_texture_format(CookTextureUsage usage, bool uncompressed);

// Cooks `image` into `writer` (initialised by the caller with the asset's
// type, GUID, and content hash): the HEAD blob and one MIP blob per
// level. `jobs` (may be NULL) parallelises the block encoding.
// NPH_RESULT_INVALID_ARGUMENT for an image format the usage cannot take
// (an 8-bit image for HDR, a float image for anything else).
NPH_MUST_USE NphResult cook_texture(NphAllocator *alloc, const CookImage *image,
                                    const CookTextureSettings *settings, NphJobSystem *jobs,
                                    NphAssetWriter *writer);

// Encodes one level (an RGBA8 image for the 8-bit formats, RGBA32F for
// BC6H/RGBA16F) into `out`, which holds nph_texture_level_bytes bytes.
// Exposed for the tests and the editor's previews.
NPH_MUST_USE NphResult cook_texture_encode_level(const CookImage *level,
                                                 const CookTextureSettings *settings,
                                                 NphJobSystem *jobs, void *out, u64 out_size);

#ifdef __cplusplus
}
#endif
