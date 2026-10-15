// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/pack.h>

#include "test_cook_support.h"

#include <cook/bc.h>

// ---------------------------------------------------------------------------
// Bit writing for hand-built blocks (LSB first, as the formats are stored)
// ---------------------------------------------------------------------------

static void put_bits(u8 block[16], u32 *pos, u32 value, u32 count)
{
    for (u32 b = 0; b < count; ++b) {
        if (((value >> b) & 1U) != 0U) {
            block[(*pos + b) >> 3U] |= (u8)(1U << ((*pos + b) & 7U));
        }
    }
    *pos += count;
}

static u32 bc7_mode_of(const u8 block[16])
{
    u32 m = 0U;
    while (m < 8U && ((block[0] >> m) & 1U) == 0U) {
        ++m;
    }
    return m;
}

// ---------------------------------------------------------------------------
// The quality a line fit allows: BC7 reconstructs every subset on the
// segment between its two endpoints, so no encoding of a partition can do
// better than fitting each subset's colours with their principal line.
// That bound (over every partition) is what the quality tests compare the
// encoder with, instead of thresholds picked by hand.
// ---------------------------------------------------------------------------

// Squared distances of the subset's RGB points to their best line.
static f64 line_residual(const CookBlockRgba8 *block, const u8 *parts, u32 subset)
{
    const u8(*rgba)[4] = block->texels;
    f64 mean[3] = {0.0, 0.0, 0.0};
    u32 n = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        if (parts[i] == subset) {
            for (u32 c = 0; c < 3U; ++c) {
                mean[c] += rgba[i][c];
            }
            ++n;
        }
    }
    if (n < 2U) {
        return 0.0;
    }
    for (u32 c = 0; c < 3U; ++c) {
        mean[c] /= (f64)n;
    }
    f64 cov[3][3];
    memset(cov, 0, sizeof cov);
    for (u32 i = 0; i < 16U; ++i) {
        if (parts[i] == subset) {
            for (u32 a = 0; a < 3U; ++a) {
                for (u32 b = 0; b < 3U; ++b) {
                    cov[a][b] += ((f64)rgba[i][a] - mean[a]) * ((f64)rgba[i][b] - mean[b]);
                }
            }
        }
    }
    const f64 total = cov[0][0] + cov[1][1] + cov[2][2];
    u32 k = 0U;
    for (u32 c = 1; c < 3U; ++c) {
        k = cov[c][c] > cov[k][k] ? c : k;
    }
    f64 v[3] = {cov[0][k], cov[1][k], cov[2][k]};
    f64 lambda = 0.0;
    for (u32 it = 0; it < 100U; ++it) {
        f64 w[3];
        for (u32 a = 0; a < 3U; ++a) {
            w[a] = cov[a][0] * v[0] + cov[a][1] * v[1] + cov[a][2] * v[2];
        }
        const f64 wn = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        const f64 vn = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (wn < 1e-12 || vn < 1e-12) {
            return total;
        }
        lambda = wn / vn;
        for (u32 a = 0; a < 3U; ++a) {
            v[a] = w[a] / wn;
        }
    }
    return total > lambda ? total - lambda : 0.0;
}

static f64 psnr_of_sse(f64 sse)
{
    return sse <= 0.0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / (sse / 64.0));
}

// The best PSNR (over RGBA, alpha exact) any partition of `subsets` allows.
static f64 bc7_bound_psnr(const CookBlockRgba8 *block, u32 subsets)
{
    f64 best = 1e300;
    const u32 partitions = subsets == 1U ? 1U : 64U;
    for (u32 p = 0; p < partitions; ++p) {
        const u8 *parts = cook_bc7_partition(subsets, p);
        f64 sse = 0.0;
        for (u32 s = 0; s < subsets; ++s) {
            sse += line_residual(block, parts, s);
        }
        best = sse < best ? sse : best;
    }
    return psnr_of_sse(best);
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

NTEST(bc, partition_tables_are_well_formed)
{
    for (u32 p = 0; p < 64U; ++p) {
        const u8 *two = cook_bc7_partition(2U, p);
        u32 counts[3] = {0, 0, 0};
        for (u32 i = 0; i < 16U; ++i) {
            NTEST_ASSERT_LE_UINT(two[i], 1U);
            counts[two[i]] += 1U;
        }
        NTEST_ASSERT_GT_UINT(counts[0], 0U);
        NTEST_ASSERT_GT_UINT(counts[1], 0U);
        NTEST_ASSERT_EQ_UINT(two[0], 0U); // texel 0 is always in subset 0
        // The second subset's anchor lies in the second subset.
        NTEST_ASSERT_EQ_UINT(two[cook_bc7_anchor(2U, p, 1U)], 1U);

        const u8 *three = cook_bc7_partition(3U, p);
        memset(counts, 0, sizeof counts);
        for (u32 i = 0; i < 16U; ++i) {
            NTEST_ASSERT_LE_UINT(three[i], 2U);
            counts[three[i]] += 1U;
        }
        NTEST_ASSERT_GT_UINT(counts[0], 0U);
        NTEST_ASSERT_GT_UINT(counts[1], 0U);
        NTEST_ASSERT_GT_UINT(counts[2], 0U);
        NTEST_ASSERT_EQ_UINT(three[0], 0U);
        NTEST_ASSERT_EQ_UINT(three[cook_bc7_anchor(3U, p, 1U)], 1U);
        NTEST_ASSERT_EQ_UINT(three[cook_bc7_anchor(3U, p, 2U)], 2U);
        NTEST_ASSERT_EQ_UINT(cook_bc7_anchor(3U, p, 0U), 0U);
    }
}

// ---------------------------------------------------------------------------
// BC4 / BC5
// ---------------------------------------------------------------------------

static void bc4_round_trip(const u8 values[16], u32 max_error)
{
    u8 block[8];
    u8 back[16];
    cook_bc4_encode(values, block);
    cook_bc4_decode(block, back);
    for (u32 i = 0; i < 16U; ++i) {
        const i32 d = (i32)back[i] - (i32)values[i];
        NTEST_ASSERT_LE_UINT((u32)(d < 0 ? -d : d), max_error);
    }
}

NTEST(bc, bc4_exact_for_flat_and_two_value_blocks)
{
    u8 flat[16];
    memset(flat, 77, sizeof flat);
    bc4_round_trip(flat, 0U);
    u8 two[16];
    for (u32 i = 0; i < 16U; ++i) {
        two[i] = (i % 3U == 0U) ? 10U : 240U;
    }
    bc4_round_trip(two, 0U);
    u8 extremes[16];
    for (u32 i = 0; i < 16U; ++i) {
        extremes[i] = (i & 1U) ? 0U : 255U;
    }
    bc4_round_trip(extremes, 0U);
    u8 ramp[16];
    for (u32 i = 0; i < 16U; ++i) {
        ramp[i] = (u8)(i * 17U); // 0..255 in 16 steps: 8 levels cannot be exact
    }
    bc4_round_trip(ramp, 20U);
}

NTEST(bc, bc4_random_blocks_stay_within_the_palette_spacing)
{
    u64 rng = 11U;
    for (u32 n = 0; n < 200U; ++n) {
        u8 values[16];
        const u8 lo = (u8)(test_rng_next(&rng) % 200U);
        const u8 span = (u8)(test_rng_next(&rng) % 56U);
        for (u32 i = 0; i < 16U; ++i) {
            values[i] = (u8)(lo + test_rng_next(&rng) % (span + 1U));
        }
        // Eight levels across the span: the error is at most half a step.
        bc4_round_trip(values, (u32)span / 14U + 2U);
    }
}

NTEST(bc, bc4_decodes_hand_built_blocks_in_both_modes)
{
    // e0 > e1: six interpolated values between them.
    u8 block[8] = {200U, 60U, 0U, 0U, 0U, 0U, 0U, 0U};
    u32 pos = 16U;
    for (u32 i = 0; i < 16U; ++i) {
        put_bits(block, &pos, i % 8U, 3U);
    }
    u8 out[16];
    cook_bc4_decode(block, out);
    NTEST_ASSERT_EQ_UINT(out[0], 200U);
    NTEST_ASSERT_EQ_UINT(out[1], 60U);
    NTEST_ASSERT_EQ_UINT(out[2], (6U * 200U + 1U * 60U) / 7U);
    NTEST_ASSERT_EQ_UINT(out[7], (1U * 200U + 6U * 60U) / 7U);
    // e0 <= e1: four interpolated values, then 0 and 255.
    block[0] = 60U;
    block[1] = 200U;
    cook_bc4_decode(block, out);
    NTEST_ASSERT_EQ_UINT(out[2], (4U * 60U + 1U * 200U) / 5U);
    NTEST_ASSERT_EQ_UINT(out[5], (1U * 60U + 4U * 200U) / 5U);
    NTEST_ASSERT_EQ_UINT(out[6], 0U);
    NTEST_ASSERT_EQ_UINT(out[7], 255U);
}

NTEST(bc, bc5_packs_two_channels)
{
    u8 r[16];
    u8 g[16];
    for (u32 i = 0; i < 16U; ++i) {
        r[i] = (u8)(100U + i);
        g[i] = (u8)(200U - i * 3U);
    }
    u8 block[16];
    u8 br[16];
    u8 bg[16];
    cook_bc5_encode(r, g, block);
    cook_bc5_decode(block, br, bg);
    for (u32 i = 0; i < 16U; ++i) {
        NTEST_ASSERT_LE_F64(nph_f32_abs((f32)br[i] - (f32)r[i]), 3.0F);
        NTEST_ASSERT_LE_F64(nph_f32_abs((f32)bg[i] - (f32)g[i]), 6.0F);
    }
}

// ---------------------------------------------------------------------------
// BC7: hand-built blocks from the format description
// ---------------------------------------------------------------------------

NTEST(bc, bc7_decodes_a_hand_built_mode6_block)
{
    // Mode 6: bit 6 set; endpoints RGBA 7 bits each (e0 then e1 per
    // channel), two p-bits, then 4-bit indices with texel 0 holding 3.
    u8 block[16];
    memset(block, 0, sizeof block);
    u32 pos = 0U;
    put_bits(block, &pos, 0x40U, 7U); // mode 6
    put_bits(block, &pos, 0x7FU, 7U); // R e0 = 127 (-> 255 with p-bit 1)
    put_bits(block, &pos, 0x00U, 7U); // R e1 = 0
    put_bits(block, &pos, 0x00U, 7U); // G e0
    put_bits(block, &pos, 0x7FU, 7U); // G e1
    put_bits(block, &pos, 0x40U, 7U); // B e0 = 64
    put_bits(block, &pos, 0x40U, 7U); // B e1 = 64
    put_bits(block, &pos, 0x7FU, 7U); // A e0
    put_bits(block, &pos, 0x7FU, 7U); // A e1
    put_bits(block, &pos, 0x1U, 1U);  // p-bit e0
    put_bits(block, &pos, 0x1U, 1U);  // p-bit e1
    put_bits(block, &pos, 0x0U, 3U);  // texel 0: index 0
    for (u32 i = 1; i < 16U; ++i) {
        put_bits(block, &pos, i == 15U ? 0xFU : 0x0U, 4U); // the last texel takes endpoint 1
    }
    NTEST_ASSERT_EQ_UINT(pos, 128U);
    CookBlockRgba8 out;
    u8(*rgba)[4] = out.texels;
    NTEST_ASSERT_TRUE(cook_bc7_decode(block, &out));
    NTEST_ASSERT_EQ_UINT(rgba[0][0], 255U);
    NTEST_ASSERT_EQ_UINT(rgba[0][1], 1U); // (0 << 1 | 1) expanded from 8 bits
    NTEST_ASSERT_EQ_UINT(rgba[0][2], 129U);
    NTEST_ASSERT_EQ_UINT(rgba[0][3], 255U);
    NTEST_ASSERT_EQ_UINT(rgba[15][0], 1U);
    NTEST_ASSERT_EQ_UINT(rgba[15][1], 255U);
    NTEST_ASSERT_EQ_UINT(rgba[15][2], 129U);
    NTEST_ASSERT_EQ_UINT(rgba[7][0], 255U); // index 0 everywhere else
    // An all-zero block has no mode bit.
    memset(block, 0, sizeof block);
    NTEST_ASSERT_FALSE(cook_bc7_decode(block, &out));
}

// Mode 4 (Khronos Data Format Specification, "BC7"): mode bits, a 2-bit
// rotation, the index selector, RGB endpoints of 5 bits and alpha
// endpoints of 6, then 31 bits of 2-bit indices followed by 47 bits of
// 3-bit indices. The selector decides which set weights colour; the
// storage order of the sets never changes.
static void build_mode4(u32 selector, u8 block[16])
{
    memset(block, 0, 16U);
    u32 pos = 0U;
    put_bits(block, &pos, 0x10U, 5U); // mode 4
    put_bits(block, &pos, 0U, 2U);    // no rotation
    put_bits(block, &pos, selector, 1U);
    put_bits(block, &pos, 31U, 5U); // R e0 -> 255
    put_bits(block, &pos, 0U, 5U);  // R e1 -> 0
    put_bits(block, &pos, 0U, 5U);  // G e0 -> 0
    put_bits(block, &pos, 31U, 5U); // G e1 -> 255
    put_bits(block, &pos, 16U, 5U); // B e0 -> 132
    put_bits(block, &pos, 16U, 5U); // B e1 -> 132
    put_bits(block, &pos, 0U, 6U);  // A e0 -> 0
    put_bits(block, &pos, 63U, 6U); // A e1 -> 255
    // The 2-bit set: texel 0 (one bit) 0, texel 15 3, the others 1.
    put_bits(block, &pos, 0U, 1U);
    for (u32 i = 1; i < 16U; ++i) {
        put_bits(block, &pos, i == 15U ? 3U : 1U, 2U);
    }
    // The 3-bit set: texel 0 (two bits) 0, texel 15 7, the others 4.
    put_bits(block, &pos, 0U, 2U);
    for (u32 i = 1; i < 16U; ++i) {
        put_bits(block, &pos, i == 15U ? 7U : 4U, 3U);
    }
    NTEST_ASSERT_EQ_UINT(pos, 128U);
}

// The interpolation of the format: weights of 64ths, rounded.
static u8 lerp64(u32 a, u32 b, u32 w)
{
    return (u8)(((64U - w) * a + w * b + 32U) >> 6U);
}

NTEST(bc, bc7_mode4_index_sets_follow_the_selector)
{
    u8 block[16];
    CookBlockRgba8 out;
    u8(*rgba)[4] = out.texels;
    // Selector 0: colour takes the 2-bit set (weight 21 for index 1),
    // alpha the 3-bit set (weight 37 for index 4).
    build_mode4(0U, block);
    NTEST_ASSERT_TRUE(cook_bc7_decode(block, &out));
    NTEST_ASSERT_EQ_UINT(rgba[0][0], 255U);
    NTEST_ASSERT_EQ_UINT(rgba[0][3], 0U);
    NTEST_ASSERT_EQ_UINT(rgba[5][0], lerp64(255U, 0U, 21U));
    NTEST_ASSERT_EQ_UINT(rgba[5][1], lerp64(0U, 255U, 21U));
    NTEST_ASSERT_EQ_UINT(rgba[5][2], 132U);
    NTEST_ASSERT_EQ_UINT(rgba[5][3], lerp64(0U, 255U, 37U));
    NTEST_ASSERT_EQ_UINT(rgba[15][1], 255U);
    NTEST_ASSERT_EQ_UINT(rgba[15][3], 255U);
    // Selector 1: the same bits, the roles swapped.
    build_mode4(1U, block);
    NTEST_ASSERT_TRUE(cook_bc7_decode(block, &out));
    NTEST_ASSERT_EQ_UINT(rgba[5][0], lerp64(255U, 0U, 37U));
    NTEST_ASSERT_EQ_UINT(rgba[5][1], lerp64(0U, 255U, 37U));
    NTEST_ASSERT_EQ_UINT(rgba[5][3], lerp64(0U, 255U, 21U));
    NTEST_ASSERT_EQ_UINT(rgba[15][0], 0U);
    NTEST_ASSERT_EQ_UINT(rgba[15][3], 255U);
}

NTEST(bc, bc7_mode5_rotation_swaps_a_channel_with_alpha)
{
    // Mode 5: 7-bit colour and 8-bit alpha endpoints, two 2-bit sets.
    u8 block[16];
    memset(block, 0, sizeof block);
    u32 pos = 0U;
    put_bits(block, &pos, 0x20U, 6U); // mode 5
    put_bits(block, &pos, 1U, 2U);    // rotation 1: red and alpha trade places
    put_bits(block, &pos, 127U, 7U);  // R e0 -> 255
    put_bits(block, &pos, 127U, 7U);  // R e1
    put_bits(block, &pos, 64U, 7U);   // G -> 129
    put_bits(block, &pos, 64U, 7U);
    put_bits(block, &pos, 0U, 7U); // B -> 0
    put_bits(block, &pos, 0U, 7U);
    put_bits(block, &pos, 10U, 8U); // A e0 = 10
    put_bits(block, &pos, 10U, 8U); // A e1
    pos += 31U + 31U;               // every index 0
    NTEST_ASSERT_EQ_UINT(pos, 128U);
    CookBlockRgba8 out;
    u8(*rgba)[4] = out.texels;
    NTEST_ASSERT_TRUE(cook_bc7_decode(block, &out));
    NTEST_ASSERT_EQ_UINT(rgba[3][0], 10U);  // the stored alpha
    NTEST_ASSERT_EQ_UINT(rgba[3][1], 129U); // untouched
    NTEST_ASSERT_EQ_UINT(rgba[3][3], 255U); // the stored red
}

// ---------------------------------------------------------------------------
// BC7: the encoder against the decoder and against the line-fit bound
// ---------------------------------------------------------------------------

static f64 bc7_round_trip_psnr(const CookBlockRgba8 *in, const CookBc7Settings *settings, u32 *mode)
{
    u8 block[16];
    CookBlockRgba8 back;
    cook_bc7_encode(in, settings, block);
    NTEST_ASSERT_TRUE(cook_bc7_decode(block, &back));
    if (mode != NULL) {
        *mode = bc7_mode_of(block);
    }
    return test_psnr8(&in->texels[0][0], &back.texels[0][0], 64U);
}

NTEST(bc, bc7_encodes_flat_and_simple_blocks_exactly)
{
    CookBlockRgba8 in;
    u8(*rgba)[4] = in.texels;
    for (u32 i = 0; i < 16U; ++i) {
        rgba[i][0] = 200U;
        rgba[i][1] = 100U;
        rgba[i][2] = 50U;
        rgba[i][3] = 255U;
    }
    NTEST_ASSERT_GE_F64(bc7_round_trip_psnr(&in, NULL, NULL), 50.0);
    // Two colours split down the middle: a two-subset mode gets it exactly
    // or nearly so.
    for (u32 i = 0; i < 16U; ++i) {
        const bool left = (i % 4U) < 2U;
        rgba[i][0] = left ? 255U : 0U;
        rgba[i][1] = left ? 0U : 255U;
        rgba[i][2] = 0U;
        rgba[i][3] = 255U;
    }
    u32 mode = 0U;
    NTEST_ASSERT_GE_F64(bc7_round_trip_psnr(&in, NULL, &mode), 45.0);
    NTEST_LOG("two-colour block used mode %u", mode);
}

NTEST(bc, bc7_gradients_reach_the_line_fit_bound)
{
    // Colours on a line (a 1-D ramp): only endpoint and index precision
    // stand between the encoder and an exact result.
    CookBlockRgba8 in;
    u8(*rgba)[4] = in.texels;
    for (u32 i = 0; i < 16U; ++i) {
        rgba[i][0] = (u8)(10U + 13U * i);
        rgba[i][1] = (u8)(200U - 9U * i);
        rgba[i][2] = (u8)(40U + 5U * i);
        rgba[i][3] = 255U;
    }
    const f64 ramp = bc7_round_trip_psnr(&in, NULL, NULL);
    NTEST_LOG("1-D ramp PSNR %.1f dB", ramp);
    NTEST_ASSERT_GE_F64(ramp, 45.0);

    // Red along x and green along y: a plane of colours that no line fits,
    // so the bound itself is low; the encoder must come close to it.
    for (u32 y = 0; y < 4U; ++y) {
        for (u32 x = 0; x < 4U; ++x) {
            u8 *p = rgba[y * 4U + x];
            p[0] = (u8)(40U + x * 50U);
            p[1] = (u8)(200U - y * 40U);
            p[2] = (u8)(90U + x * 10U + y * 10U);
            p[3] = 255U;
        }
    }
    const f64 bound2 = bc7_bound_psnr(&in, 2U);
    const f64 bound3 = bc7_bound_psnr(&in, 3U);
    const f64 plane = bc7_round_trip_psnr(&in, NULL, NULL);
    CookBc7Settings all;
    memset(&all, 0, sizeof all);
    all.mode_mask = 0xFFU;
    const f64 plane_all = bc7_round_trip_psnr(&in, &all, NULL);
    NTEST_LOG("2-D gradient: %.1f dB (bound %.1f with two subsets), %.1f dB with every mode "
              "(bound %.1f with three)",
              plane, bound2, plane_all, bound3);
    NTEST_ASSERT_GE_F64(plane, bound2 - 1.0);
    NTEST_ASSERT_GE_F64(plane_all, plane);
    NTEST_ASSERT_LE_F64(plane_all, bound3 + 0.05); // a sanity check of the bound itself
}

NTEST(bc, bc7_noise_stays_near_the_line_fit_bound)
{
    CookBlockRgba8 in;
    u8(*rgba)[4] = in.texels;
    u64 rng = 5U;
    f64 worst_gap = 0.0;
    f64 worst = 99.0;
    for (u32 n = 0; n < 64U; ++n) {
        for (u32 i = 0; i < 16U; ++i) {
            rgba[i][0] = (u8)test_rng_next(&rng);
            rgba[i][1] = (u8)test_rng_next(&rng);
            rgba[i][2] = (u8)test_rng_next(&rng);
            rgba[i][3] = 255U;
        }
        const f64 psnr = bc7_round_trip_psnr(&in, NULL, NULL);
        const f64 bound = bc7_bound_psnr(&in, 2U);
        NTEST_ASSERT_LE_F64(psnr, bound + 0.05);
        worst_gap = nph_f64_max(worst_gap, bound - psnr);
        worst = nph_f64_min(worst, psnr);
    }
    NTEST_LOG("noise: worst %.1f dB, at most %.1f dB below the two-subset bound", worst, worst_gap);
    // On noise the distance off the line dominates; quantising positions
    // along it (two- and three-bit indices) costs a fraction of a dB.
    NTEST_ASSERT_LE_F64(worst_gap, 1.0);
}

NTEST(bc, bc7_alpha_blocks_keep_their_alpha)
{
    CookBlockRgba8 in;
    u8(*rgba)[4] = in.texels;
    for (u32 i = 0; i < 16U; ++i) {
        rgba[i][0] = (u8)(30U + i * 8U);
        rgba[i][1] = 120U;
        rgba[i][2] = (u8)(250U - i * 5U);
        rgba[i][3] = (u8)(i * 17U);
    }
    u32 mode = 0U;
    const f64 psnr = bc7_round_trip_psnr(&in, NULL, &mode);
    NTEST_LOG("alpha block: mode %u, PSNR %.1f dB", mode, psnr);
    NTEST_ASSERT_GE_F64(psnr, 36.0);
    NTEST_ASSERT_TRUE(mode >= 4U); // an alpha mode
    // Every alpha mode can be forced through the mask and reproduces the
    // block well (colour and alpha are both ramps); an opaque-only mask
    // falls back to mode 6, which handles alpha.
    for (u32 m = 0; m < 8U; ++m) {
        CookBc7Settings s;
        memset(&s, 0, sizeof s);
        s.mode_mask = 1U << m;
        u32 used = 99U;
        const f64 p = bc7_round_trip_psnr(&in, &s, &used);
        NTEST_LOG("forced mode %u -> mode %u, %.1f dB", m, used, p);
        NTEST_ASSERT_EQ_UINT(used, m >= 4U ? m : 6U);
        // Mode 5's 2-bit indices put four levels on a 16-step ramp: the
        // weakest mode for this block, at about 25 dB.
        NTEST_ASSERT_GE_F64(p, 22.0);
    }
}

NTEST(bc, bc7_mode4_selectors_both_round_trip)
{
    // Alpha with more detail than colour favours selector 0 (3-bit alpha);
    // colour with more detail than alpha favours selector 1.
    CookBlockRgba8 in;
    CookBlockRgba8 back;
    u8(*rgba)[4] = in.texels;
    u8 block[16];
    CookBc7Settings s;
    memset(&s, 0, sizeof s);
    s.mode_mask = 1U << 4U;
    for (u32 variant = 0; variant < 2U; ++variant) {
        for (u32 i = 0; i < 16U; ++i) {
            const u8 ramp = (u8)(i * 17U);
            const u8 step = (u8)(i < 8U ? 40U : 220U);
            rgba[i][0] = variant == 0U ? step : ramp;
            rgba[i][1] = variant == 0U ? step : (u8)(255U - ramp);
            rgba[i][2] = 128U;
            rgba[i][3] = variant == 0U ? ramp : step;
        }
        cook_bc7_encode(&in, &s, block);
        NTEST_ASSERT_EQ_UINT(bc7_mode_of(block), 4U);
        const u32 selector = (block[0] >> 7U) & 1U;
        NTEST_ASSERT_TRUE(cook_bc7_decode(block, &back));
        const f64 psnr = test_psnr8(&in.texels[0][0], &back.texels[0][0], 64U);
        NTEST_LOG("mode 4 variant %u: selector %u, %.1f dB", variant, selector, psnr);
        NTEST_ASSERT_EQ_UINT(selector, variant);
        NTEST_ASSERT_GE_F64(psnr, 30.0); // the ramp gets 8 levels over 16 steps
    }
}

NTEST(bc, bc7_every_mode_is_reachable_and_decodes_random_blocks)
{
    u64 rng = 77U;
    u32 seen = 0U;
    for (u32 n = 0; n < 2000U; ++n) {
        u8 block[16];
        for (u32 i = 0; i < 16U; ++i) {
            block[i] = (u8)test_rng_next(&rng);
        }
        CookBlockRgba8 out;
        if (cook_bc7_decode(block, &out)) {
            seen |= 1U << bc7_mode_of(block);
        }
    }
    NTEST_ASSERT_EQ_UINT(seen, 0xFFU);
}

// ---------------------------------------------------------------------------
// BC6H
// ---------------------------------------------------------------------------

static f32 half_rel_error(u16 a, u16 b)
{
    const f32 fa = nph_f16_to_f32(a);
    const f32 fb = nph_f16_to_f32(b);
    const f32 denom = nph_f32_max(nph_f32_abs(fa), 1e-3F);
    return nph_f32_abs(fa - fb) / denom;
}

NTEST(bc, bc6h_decodes_a_hand_built_mode11_block)
{
    // Mode 11: mode bits 00011, RGB endpoints of 10 bits (e0 then e1),
    // then 4-bit indices with texel 0 holding 3.
    u8 block[16];
    memset(block, 0, sizeof block);
    u32 pos = 0U;
    put_bits(block, &pos, 0x03U, 5U);
    put_bits(block, &pos, 0U, 10U);    // R e0
    put_bits(block, &pos, 1023U, 10U); // G e0 (the maximum: 0xFFFF unquantised)
    put_bits(block, &pos, 512U, 10U);  // B e0
    put_bits(block, &pos, 1023U, 10U); // R e1
    put_bits(block, &pos, 0U, 10U);    // G e1
    put_bits(block, &pos, 512U, 10U);  // B e1
    put_bits(block, &pos, 0U, 3U);
    for (u32 i = 1; i < 16U; ++i) {
        put_bits(block, &pos, i == 15U ? 15U : 0U, 4U);
    }
    NTEST_ASSERT_EQ_UINT(pos, 128U);
    CookBlockRgbHalf out;
    u16(*rgb)[3] = out.texels;
    NTEST_ASSERT_TRUE(cook_bc6h_decode(block, &out));
    // Unquantised 0xFFFF finishes as 0xFFFF * 31 / 64 = 0x7BFF, the
    // largest finite half; 512 of 1023 unquantises to (512 << 16 |
    // 0x8000) >> 10 = 0x8020 and finishes as 0x3E0F.
    NTEST_ASSERT_EQ_UINT(rgb[0][0], 0U);
    NTEST_ASSERT_EQ_UINT(rgb[0][1], 0x7BFFU);
    NTEST_ASSERT_EQ_UINT(rgb[0][2], (0x8020U * 31U) >> 6U);
    NTEST_ASSERT_EQ_UINT(rgb[15][0], 0x7BFFU);
    NTEST_ASSERT_EQ_UINT(rgb[15][1], 0U);
}

NTEST(bc, bc6h_round_trips_hdr_blocks)
{
    CookBlockRgbHalf in;
    u16(*rgb)[3] = in.texels;
    for (u32 i = 0; i < 16U; ++i) {
        rgb[i][0] = nph_f32_to_f16(1.0F);
        rgb[i][1] = nph_f32_to_f16(1.0F);
        rgb[i][2] = nph_f32_to_f16(1.0F);
    }
    u8 block[16];
    CookBlockRgbHalf out;
    u16(*back)[3] = out.texels;
    cook_bc6h_encode(&in, block);
    NTEST_ASSERT_TRUE(cook_bc6h_decode(block, &out));
    for (u32 i = 0; i < 16U; ++i) {
        for (u32 c = 0; c < 3U; ++c) {
            NTEST_ASSERT_LE_F64((f64)half_rel_error(rgb[i][c], back[i][c]), 0.01);
        }
    }
    // A gradient over two orders of magnitude.
    for (u32 i = 0; i < 16U; ++i) {
        rgb[i][0] = nph_f32_to_f16(0.5F + (f32)i * 2.0F);
        rgb[i][1] = nph_f32_to_f16(4.0F);
        rgb[i][2] = nph_f32_to_f16(30.0F - (f32)i);
    }
    cook_bc6h_encode(&in, block);
    NTEST_ASSERT_TRUE(cook_bc6h_decode(block, &out));
    f32 worst = 0.0F;
    for (u32 i = 0; i < 16U; ++i) {
        for (u32 c = 0; c < 3U; ++c) {
            worst = nph_f32_max(worst, half_rel_error(rgb[i][c], back[i][c]));
        }
    }
    NTEST_LOG("BC6H gradient: worst relative error %.3f", (f64)worst);
    // One subset and bit-space interpolation: a gradient spanning two
    // orders of magnitude cannot be exact (partitioned modes would do
    // better; docs/cook.md lists them as future work).
    NTEST_ASSERT_LE_F64((f64)worst, 0.3);
    // Zero stays zero and a non-mode-11 block is rejected.
    memset(&in, 0, sizeof in);
    cook_bc6h_encode(&in, block);
    NTEST_ASSERT_TRUE(cook_bc6h_decode(block, &out));
    NTEST_ASSERT_EQ_UINT(back[5][1], 0U);
    block[0] = 0x00U; // mode 1
    NTEST_ASSERT_FALSE(cook_bc6h_decode(block, &out));
}
