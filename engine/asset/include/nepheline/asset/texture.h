// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cooked textures (CLAUDE.md §8.8 "Textures: mip generation in linear
// space, our own BC7/BC5/BC4/BC6H encoders, normal-map handling").
//
// A texture asset (NPH_ASSET_TYPE_TEXTURE) holds a header blob and one
// blob per mip level:
//
//     HEAD      NphTextureHeader
//     MIP [m]   the texels of mip m for every layer, layer after layer,
//               rows tightly packed (whole 4x4 blocks for BC formats)
//
// One blob per mip is what lets the streaming of §8.8 fetch the small
// mips first and the large ones on demand: each is addressable and
// compressed on its own. Formats are pinned numbers that the renderer
// maps to its GPU formats; they never depend on the gpu module's enum.
//
// Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/asset/loader.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_TEXTURE_TYPE_VERSION 1U
#define NPH_TEXTURE_TAG_HEADER   NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_TEXTURE_TAG_MIP      NPH_FOURCC('M', 'I', 'P', ' ')
#define NPH_TEXTURE_MAX_MIPS     16U
#define NPH_TEXTURE_MAX_EXTENT   16384U
#define NPH_TEXTURE_MAX_LAYERS   2048U

// Values are part of the file format and never renumbered.
typedef enum NphTextureFormat {
    NPH_TEXTURE_FORMAT_UNDEFINED = 0,
    NPH_TEXTURE_FORMAT_R8_UNORM = 1,
    NPH_TEXTURE_FORMAT_R8G8_UNORM = 2,
    NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM = 3,
    NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB = 4,
    NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT = 5,
    NPH_TEXTURE_FORMAT_BC4_UNORM = 10,
    NPH_TEXTURE_FORMAT_BC5_UNORM = 11,
    NPH_TEXTURE_FORMAT_BC6H_UFLOAT = 12,
    NPH_TEXTURE_FORMAT_BC7_UNORM = 13,
    NPH_TEXTURE_FORMAT_BC7_SRGB = 14,
} NphTextureFormat;

typedef enum NphTextureKind {
    NPH_TEXTURE_2D = 0,       // layer_count 1
    NPH_TEXTURE_2D_ARRAY = 1, // layer_count >= 1
    NPH_TEXTURE_CUBE = 2,     // layer_count a multiple of 6: +X -X +Y -Y +Z -Z
} NphTextureKind;

typedef enum NphTextureFlags {
    NPH_TEXTURE_FLAG_NORMAL_MAP = 1U << 0U, // two-channel tangent-space normals (reconstruct z)
    NPH_TEXTURE_FLAG_PREMULTIPLIED = 1U << 1U,
} NphTextureFlags;

typedef struct NphTextureHeader {
    u32 width;
    u32 height;
    u32 mip_count;   // 1..NPH_TEXTURE_MAX_MIPS
    u32 layer_count; // 1..NPH_TEXTURE_MAX_LAYERS
    u32 format;      // NphTextureFormat
    u32 kind;        // NphTextureKind
    u32 flags;       // NphTextureFlags
    u32 reserved[9];
} NphTextureHeader;

NPH_STATIC_ASSERT(sizeof(NphTextureHeader) == 64U, "the texture header is 64 bytes");

typedef struct NphTextureFormatInfo {
    const char *name;
    u32 bytes_per_block; // of one texel, or of one 4x4 block
    u32 block_size;      // texels per block side: 1, or 4 for BC formats
    u32 channels;
    b32 srgb;
    b32 compressed;
} NphTextureFormatInfo;

// Static description; UNDEFINED for unknown values.
const NphTextureFormatInfo *nph_texture_format_info(u32 format);

// Bytes of one tightly packed level of `width` x `height` texels (whole
// blocks for compressed formats). 0 for an unknown format.
u64 nph_texture_level_bytes(u32 format, u32 width, u32 height);

// The extent of mip `mip` (at least 1).
NPH_INLINE u32 nph_texture_mip_extent(u32 extent, u32 mip)
{
    const u32 e = extent >> mip;
    return e == 0U ? 1U : e;
}

// Levels in a full chain down to 1x1.
u32 nph_texture_mip_count(u32 width, u32 height);

typedef struct NphTextureView {
    const NphTextureHeader *header;
    const void *mips[NPH_TEXTURE_MAX_MIPS]; // every layer of the level, in order
    u64 mip_sizes[NPH_TEXTURE_MAX_MIPS];    // bytes of the level across all layers
} NphTextureView;

// Resolves and validates a READY texture asset: every mip blob present
// and exactly the size its format and extent imply.
NPH_MUST_USE NphResult nph_texture_view(const NphAsset *asset, NphTextureView *out);

#ifdef __cplusplus
}
#endif
