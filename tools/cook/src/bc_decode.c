// SPDX-License-Identifier: Apache-2.0
// Block decoders (bc.h) and the tables the encoders share: the reference
// against which the encoders are tested, and what the editor will use to
// preview cooked textures on the CPU.
#include <nepheline/core/assert.h>

#include "bc_internal.h"

#include <cook/bc.h>
#include <string.h>

// ---------------------------------------------------------------------------
// BC7 tables (Khronos Data Format Specification, "BC7 partition tables" and
// "BC7 anchor index tables"; Microsoft, "BC7 Format", Direct3D 11).
// ---------------------------------------------------------------------------

const u8 g_bc7_partition2[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0},
    {0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0},
    {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0},
    {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0},
    {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1},
    {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1},
    {0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0},
    {0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1},
    {0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1},
    {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0},
    {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1},
    {0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1},
    {0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1},
    {0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1},
    {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0},
    {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1},
};

const u8 g_bc7_partition3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2},
    {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2},
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2},
    {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2},
    {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2},
    {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0},
    {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2},
    {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2},
    {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0},
    {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0},
    {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1},
    {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1},
    {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2},
    {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2},
    {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1},
    {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};

// The anchor texel of the second subset (two-subset partitions).
const u8 g_bc7_anchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                              15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                              15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                              6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};

// The anchor texels of the second and third subsets (three-subset partitions).
const u8 g_bc7_anchor3a[64] = {3, 3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                               3, 3,  8,  15, 3, 3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                               8, 15, 3,  5,  6, 10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                               3, 15, 5,  5,  5, 8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
const u8 g_bc7_anchor3b[64] = {15, 8, 8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                               15, 8, 15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                               15, 3, 15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                               15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

const u8 g_bc7_weights2[4] = {0, 21, 43, 64};
const u8 g_bc7_weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const u8 g_bc7_weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// Per-mode layout (Microsoft, "BC7 Format", mode table).
const Bc7ModeInfo g_bc7_modes[8] = {
    // subsets, partition_bits, rotation_bits, index_selector_bits, color_bits, alpha_bits,
    // p_bits (per endpoint 1, shared 2, none 0), index_bits, index2_bits
    {3, 4, 0, 0, 4, 0, 1, 3, 0}, // 0
    {2, 6, 0, 0, 6, 0, 2, 3, 0}, // 1
    {3, 6, 0, 0, 5, 0, 0, 2, 0}, // 2
    {2, 6, 0, 0, 7, 0, 1, 2, 0}, // 3
    {1, 0, 2, 1, 5, 6, 0, 2, 3}, // 4
    {1, 0, 2, 0, 7, 8, 0, 2, 2}, // 5
    {1, 0, 0, 0, 7, 7, 1, 4, 0}, // 6
    {2, 6, 0, 0, 5, 5, 1, 2, 0}, // 7
};

const u8 *cook_bc7_partition(u32 subsets, u32 partition)
{
    static const u8 one[16] = {0};
    NPH_ASSERT(partition < 64U);
    if (subsets == 2U) {
        return g_bc7_partition2[partition];
    }
    if (subsets == 3U) {
        return g_bc7_partition3[partition];
    }
    return one;
}

u32 cook_bc7_anchor(u32 subsets, u32 partition, u32 subset)
{
    NPH_ASSERT(partition < 64U);
    if (subset == 0U) {
        return 0U;
    }
    if (subsets == 2U) {
        return g_bc7_anchor2[partition];
    }
    return subset == 1U ? g_bc7_anchor3a[partition] : g_bc7_anchor3b[partition];
}

// ---------------------------------------------------------------------------
// Bit streams (LSB first, as the formats are specified)
// ---------------------------------------------------------------------------

u32 bc_read_bits(const u8 *block, u32 *pos, u32 count)
{
    u32 value = 0U;
    for (u32 i = 0; i < count; ++i) {
        const u32 bit = *pos + i;
        value |= (u32)((block[bit >> 3U] >> (bit & 7U)) & 1U) << i;
    }
    *pos += count;
    return value;
}

void bc_write_bits(u8 *block, u32 *pos, u32 count, u32 value)
{
    for (u32 i = 0; i < count; ++i) {
        const u32 bit = *pos + i;
        if ((value >> i) & 1U) {
            block[bit >> 3U] |= (u8)(1U << (bit & 7U));
        }
    }
    *pos += count;
}

// ---------------------------------------------------------------------------
// BC1
// ---------------------------------------------------------------------------

static void bc1_expand(u32 c, u32 out[4])
{
    const u32 r = (c >> 11U) & 31U;
    const u32 g = (c >> 5U) & 63U;
    const u32 b = c & 31U;
    // Bit replication: equal to the exact r * 255 / 31 rounded, for every r.
    out[0] = (r << 3U) | (r >> 2U);
    out[1] = (g << 2U) | (g >> 4U);
    out[2] = (b << 3U) | (b >> 2U);
    out[3] = 255U;
}

void cook_bc1_decode(const u8 block[8], bool three_color, CookBlockRgba8 *out)
{
    const u32 c0 = (u32)block[0] | ((u32)block[1] << 8U);
    const u32 c1 = (u32)block[2] | ((u32)block[3] << 8U);
    u32 palette[4][4];
    bc1_expand(c0, palette[0]);
    bc1_expand(c1, palette[1]);
    const bool four = !three_color || c0 > c1;
    for (u32 ch = 0; ch < 3U; ++ch) {
        const u32 a = palette[0][ch];
        const u32 b = palette[1][ch];
        if (four) {
            palette[2][ch] = (2U * a + b + 1U) / 3U;
            palette[3][ch] = (a + 2U * b + 1U) / 3U;
        } else {
            palette[2][ch] = (a + b + 1U) / 2U;
            palette[3][ch] = 0U;
        }
    }
    palette[2][3] = 255U;
    palette[3][3] = four ? 255U : 0U;
    const u32 indices =
        (u32)block[4] | ((u32)block[5] << 8U) | ((u32)block[6] << 16U) | ((u32)block[7] << 24U);
    for (u32 i = 0; i < 16U; ++i) {
        const u32 k = (indices >> (2U * i)) & 3U;
        for (u32 ch = 0; ch < 4U; ++ch) {
            out->texels[i][ch] = (u8)palette[k][ch];
        }
    }
}

// ---------------------------------------------------------------------------
// BC4 / BC5
// ---------------------------------------------------------------------------

void bc4_palette(u8 e0, u8 e1, u8 palette[8])
{
    palette[0] = e0;
    palette[1] = e1;
    if (e0 > e1) {
        for (u32 i = 1; i < 7U; ++i) {
            palette[i + 1U] = (u8)(((7U - i) * e0 + i * e1) / 7U);
        }
    } else {
        for (u32 i = 1; i < 5U; ++i) {
            palette[i + 1U] = (u8)(((5U - i) * e0 + i * e1) / 5U);
        }
        palette[6] = 0U;
        palette[7] = 255U;
    }
}

void cook_bc4_decode(const u8 block[8], u8 out[16])
{
    u8 palette[8];
    bc4_palette(block[0], block[1], palette);
    u32 pos = 16U;
    for (u32 i = 0; i < 16U; ++i) {
        out[i] = palette[bc_read_bits(block, &pos, 3U)];
    }
}

void cook_bc5_decode(const u8 block[16], u8 r[16], u8 g[16])
{
    cook_bc4_decode(block, r);
    cook_bc4_decode(block + 8, g);
}

// ---------------------------------------------------------------------------
// BC7
// ---------------------------------------------------------------------------

bool cook_bc7_decode(const u8 block[16], CookBlockRgba8 *out)
{
    u8(*rgba)[4] = out->texels;
    memset(out->texels, 0, sizeof out->texels);
    u32 mode = 0U;
    while (mode < 8U && ((block[0] >> mode) & 1U) == 0U) {
        ++mode;
    }
    if (mode >= 8U) {
        return false;
    }
    const Bc7ModeInfo *m = &g_bc7_modes[mode];
    u32 pos = mode + 1U;
    const u32 partition = bc_read_bits(block, &pos, m->partition_bits);
    const u32 rotation = bc_read_bits(block, &pos, m->rotation_bits);
    const u32 index_selector = bc_read_bits(block, &pos, m->index_selector_bits);

    // Endpoints: for each channel, every subset's two endpoints.
    u32 endpoints[3][2][4]; // [subset][endpoint][channel]
    const u32 channels = m->alpha_bits != 0U ? 4U : 3U;
    for (u32 c = 0; c < channels; ++c) {
        const u32 bits = c == 3U ? m->alpha_bits : m->color_bits;
        for (u32 s = 0; s < m->subsets; ++s) {
            endpoints[s][0][c] = bc_read_bits(block, &pos, bits);
            endpoints[s][1][c] = bc_read_bits(block, &pos, bits);
        }
    }
    // P-bits extend every channel by one bit.
    u32 pbits[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    if (m->p_bits == 1U) {
        for (u32 s = 0; s < m->subsets; ++s) {
            pbits[s][0] = bc_read_bits(block, &pos, 1U);
            pbits[s][1] = bc_read_bits(block, &pos, 1U);
        }
    } else if (m->p_bits == 2U) {
        for (u32 s = 0; s < m->subsets; ++s) {
            const u32 p = bc_read_bits(block, &pos, 1U);
            pbits[s][0] = p;
            pbits[s][1] = p;
        }
    }
    u8 palette_lo[3][2][4]; // expanded endpoints [subset][endpoint][channel]
    for (u32 s = 0; s < m->subsets; ++s) {
        for (u32 e = 0; e < 2U; ++e) {
            for (u32 c = 0; c < 4U; ++c) {
                if (c == 3U && m->alpha_bits == 0U) {
                    palette_lo[s][e][c] = 255U;
                    continue;
                }
                const u32 bits = c == 3U ? m->alpha_bits : m->color_bits;
                u32 v = endpoints[s][e][c];
                u32 total_bits = bits;
                if (m->p_bits != 0U) {
                    v = (v << 1U) | pbits[s][e];
                    total_bits += 1U;
                }
                palette_lo[s][e][c] = bc7_expand(v, total_bits);
            }
        }
    }

    // Indices: the anchor texel of each subset is stored one bit short.
    // Modes 4 and 5 hold a second set of 16 (texel 0 its anchor) after the
    // first; the two sets are always stored in this order, index_bits wide
    // and then index2_bits wide, whatever the index selector says.
    const u8 *parts = cook_bc7_partition(m->subsets, partition);
    u32 indices[16];
    for (u32 i = 0; i < 16U; ++i) {
        const u32 s = parts[i];
        const bool anchor = i == cook_bc7_anchor(m->subsets, partition, s);
        indices[i] = bc_read_bits(block, &pos, anchor ? m->index_bits - 1U : m->index_bits);
    }
    u32 indices2[16];
    if (m->index2_bits != 0U) {
        for (u32 i = 0; i < 16U; ++i) {
            indices2[i] = bc_read_bits(block, &pos, i == 0U ? m->index2_bits - 1U : m->index2_bits);
        }
    }
    // Which set weights colour and which alpha: the first set does both
    // in the other modes; in modes 4 and 5 colour takes the first set and
    // alpha the second, unless mode 4's index selector swaps them
    // (Khronos Data Format Specification, "BC7", mode 4).
    const u32 *color_idx = indices;
    const u32 *alpha_idx = indices;
    u32 color_bits = m->index_bits;
    u32 alpha_bits = m->index_bits;
    if (m->index2_bits != 0U) {
        const bool swap = m->index_selector_bits != 0U && index_selector != 0U;
        color_idx = swap ? indices2 : indices;
        alpha_idx = swap ? indices : indices2;
        color_bits = swap ? m->index2_bits : m->index_bits;
        alpha_bits = swap ? m->index_bits : m->index2_bits;
    }
    const u8 *wc = bc7_weights(color_bits);
    const u8 *wa = bc7_weights(alpha_bits);
    for (u32 i = 0; i < 16U; ++i) {
        const u32 s = parts[i];
        for (u32 c = 0; c < 4U; ++c) {
            const u32 w = c == 3U ? wa[alpha_idx[i]] : wc[color_idx[i]];
            const u32 lo = palette_lo[s][0][c];
            const u32 hi = palette_lo[s][1][c];
            rgba[i][c] = (u8)(((64U - w) * lo + w * hi + 32U) >> 6U);
        }
        // Rotation swaps alpha with one colour channel.
        if (rotation != 0U) {
            const u8 t = rgba[i][3];
            rgba[i][3] = rgba[i][rotation - 1U];
            rgba[i][rotation - 1U] = t;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// BC6H unsigned, mode 11
// ---------------------------------------------------------------------------

u32 bc6h_unquantize_unsigned(u32 q, u32 bits)
{
    if (bits >= 16U) {
        return q;
    }
    if (q == 0U) {
        return 0U;
    }
    if (q == (1U << bits) - 1U) {
        return 0xFFFFU;
    }
    return ((q << 16U) + 0x8000U) >> bits;
}

u16 bc6h_finish_unsigned(u32 unquantized)
{
    return (u16)((unquantized * 31U) >> 6U);
}

bool cook_bc6h_decode(const u8 block[16], CookBlockRgbHalf *out)
{
    u16(*rgb)[3] = out->texels;
    memset(out->texels, 0, sizeof out->texels);
    u32 pos = 0U;
    const u32 mode = bc_read_bits(block, &pos, 5U);
    if (mode != BC6H_MODE11_BITS) {
        return false;
    }
    u32 e0[3];
    u32 e1[3];
    for (u32 c = 0; c < 3U; ++c) {
        e0[c] = bc_read_bits(block, &pos, 10U);
    }
    for (u32 c = 0; c < 3U; ++c) {
        e1[c] = bc_read_bits(block, &pos, 10U);
    }
    u32 lo[3];
    u32 hi[3];
    for (u32 c = 0; c < 3U; ++c) {
        lo[c] = bc6h_unquantize_unsigned(e0[c], 10U);
        hi[c] = bc6h_unquantize_unsigned(e1[c], 10U);
    }
    for (u32 i = 0; i < 16U; ++i) {
        const u32 w = g_bc7_weights4[bc_read_bits(block, &pos, i == 0U ? 3U : 4U)];
        for (u32 c = 0; c < 3U; ++c) {
            const u32 v = ((64U - w) * lo[c] + w * hi[c] + 32U) >> 6U;
            rgb[i][c] = bc6h_finish_unsigned(v);
        }
    }
    return true;
}
