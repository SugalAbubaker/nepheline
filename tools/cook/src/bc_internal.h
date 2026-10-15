// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared between bc_decode.c (tables, bit streams, palettes) and
// bc_encode.c.

#include <cook/bc.h>

typedef struct Bc7ModeInfo {
    u8 subsets;
    u8 partition_bits;
    u8 rotation_bits;
    u8 index_selector_bits;
    u8 color_bits;
    u8 alpha_bits; // 0: opaque mode
    u8 p_bits;     // 0 none, 1 one per endpoint, 2 one shared per subset
    u8 index_bits;
    u8 index2_bits; // modes 4 and 5: the second (alpha) index set
} Bc7ModeInfo;

extern const u8 g_bc7_partition2[64][16];
extern const u8 g_bc7_partition3[64][16];
extern const u8 g_bc7_anchor2[64];
extern const u8 g_bc7_anchor3a[64];
extern const u8 g_bc7_anchor3b[64];
extern const u8 g_bc7_weights2[4];
extern const u8 g_bc7_weights3[8];
extern const u8 g_bc7_weights4[16];
extern const Bc7ModeInfo g_bc7_modes[8];

#define BC6H_MODE11_BITS 0x03U

// Inline: the encoder calls both per texel and per candidate endpoint.
NPH_INLINE const u8 *bc7_weights(u32 index_bits)
{
    return index_bits == 2U ? g_bc7_weights2 : index_bits == 3U ? g_bc7_weights3 : g_bc7_weights4;
}

// Replicates the top bits of a `bits`-wide value into the low ones.
NPH_INLINE u8 bc7_expand(u32 value, u32 bits)
{
    const u32 shifted = value << (8U - bits);
    return (u8)(shifted | (shifted >> bits));
}

u32 bc_read_bits(const u8 *block, u32 *pos, u32 count);
void bc_write_bits(u8 *block, u32 *pos, u32 count, u32 value);

void bc4_palette(u8 e0, u8 e1, u8 palette[8]);

u32 bc6h_unquantize_unsigned(u32 q, u32 bits);
u16 bc6h_finish_unsigned(u32 unquantized);
