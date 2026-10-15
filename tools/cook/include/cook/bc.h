// SPDX-License-Identifier: Apache-2.0
#pragma once

// Block-compressed texture encoders and decoders (CLAUDE.md §8.8 "our own
// BC7/BC5/BC4/BC6H encoders"): the engine's implementation of the BC4,
// BC5, BC6H, and BC7 formats (Khronos Data Format Specification 1.3,
// chapter "S3TC/BPTC"; Microsoft, "Texture Block Compression in Direct3D
// 11").
//
// Blocks are 4x4 texels in row-major order (texel 0 is the top left).
// Every encoder is deterministic: the same block gives the same bytes on
// every platform and thread, which the cooker relies on when it encodes
// block rows on the job system.
//
// Quality (docs/cook.md): BC4 and BC5 search both endpoint modes with
// least-squares refinement; BC7 evaluates modes 1, 3, 6 for opaque blocks
// and 4, 5, 6, 7 for blocks with alpha (modes 0 and 2 on request), with
// a cheap partition estimate that picks the best few partitions for a
// full encode, PCA endpoints, and least-squares refinement; BC6H encodes
// unsigned half floats in mode 11 (one subset, 10-bit endpoints). The
// decoders implement every BC7 mode (so any BC7 block decodes) and BC6H
// mode 11.
//
// Thread-safety: pure functions, NPH_THREADSAFE.

#include <cook/cook.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

// A 4x4 block of texels for the BC7 and BC6H encoders and decoders. The
// texels are wrapped in a struct rather than passed as `u8 [16][4]`
// because C17 does not convert `u8 (*)[4]` to `const u8 (*)[4]`
// implicitly (C23 does; GCC's -Wpedantic reports every such call before
// C23), so a function taking `const u8 rgba[16][4]` could not be handed a
// block its caller has just filled in without a cast.
typedef struct CookBlockRgba8 {
    u8 texels[16][4];
} CookBlockRgba8;

typedef struct CookBlockRgbHalf {
    u16 texels[16][3]; // half-float bits
} CookBlockRgbHalf;

// ---------------------------------------------------------------------------
// BC4 (one 8-bit channel, 8 bytes) and BC5 (two channels, 16 bytes)
// ---------------------------------------------------------------------------

void cook_bc4_encode(const u8 values[16], u8 out[8]);
void cook_bc4_decode(const u8 block[8], u8 out[16]);

void cook_bc5_encode(const u8 r[16], const u8 g[16], u8 out[16]);
void cook_bc5_decode(const u8 block[16], u8 r[16], u8 g[16]);

// ---------------------------------------------------------------------------
// BC7 (RGBA8, 16 bytes)
// ---------------------------------------------------------------------------

typedef struct CookBc7Settings {
    u32 mode_mask;      // bit m enables mode m; 0: the default set
    u32 max_partitions; // partitions fully encoded per two- or three-subset mode (0: 8, max 64)
    u32 refine_passes;  // least-squares passes per subset (0: 2)
} CookBc7Settings;

#define COOK_BC7_DEFAULT_MODES 0xFAU // modes 1, 3, 4, 5, 6, 7

void cook_bc7_encode(const CookBlockRgba8 *in, const CookBc7Settings *settings, u8 out[16]);

// false for an invalid block (no mode bit set); `out` is then black.
bool cook_bc7_decode(const u8 block[16], CookBlockRgba8 *out);

// ---------------------------------------------------------------------------
// BC6H unsigned (RGB half floats, 16 bytes)
// ---------------------------------------------------------------------------

// Input texels are half-float bits of non-negative, finite values.
void cook_bc6h_encode(const CookBlockRgbHalf *in, u8 out[16]);

// Decodes a mode 11 block; false (and black) for any other mode.
bool cook_bc6h_decode(const u8 block[16], CookBlockRgbHalf *out);

// ---------------------------------------------------------------------------
// Shared tables, exposed for the tests
// ---------------------------------------------------------------------------

// The subset of each texel for two-subset and three-subset partitions.
const u8 *cook_bc7_partition(u32 subsets, u32 partition);
// The anchor texel of subset `subset` (1 or 2) for a partition.
u32 cook_bc7_anchor(u32 subsets, u32 partition, u32 subset);

#ifdef __cplusplus
}
#endif
