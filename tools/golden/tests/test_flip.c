// SPDX-License-Identifier: Apache-2.0
// LDR-FLIP (golden/flip.h) against NVIDIA's own implementation: the
// expected values below come from flip-evaluator 1.7 on the same image
// pairs (make_flip_expectations.py builds them with the same formulas as
// flip_pair here). Beyond these, the implementation was checked on two
// 1280 x 720 renders of the pbr_viewer scene: every pixel within 4e-5 of
// the authors' (docs/golden.md).
#include <nepheline/math/scalar.h>

#include <ntest/ntest.h>

#include <golden/flip.h>
#include <string.h>

#define W 64U
#define H 48U

// {mean, error at (32, 24), error at (3, 5), max}, from flip-evaluator 1.7.
static const f64 g_expected[6][4] = {
    {0.000000, 0.000000, 0.000000, 0.000000}, // identical patterns
    {0.169160, 0.169160, 0.169160, 0.169160}, // uniform greys 128 and 140
    {0.967392, 0.967392, 0.967392, 0.967392}, // white against black
    {0.346960, 0.192322, 0.473360, 0.521215}, // a red offset of 40
    {0.290983, 0.305929, 0.019012, 0.987927}, // a checkerboard shifted by one texel
    {0.964304, 0.964304, 0.964304, 0.964304}, // reddish against greenish
};

static void pattern(u32 x, u32 y, u8 *p)
{
    p[0] = (u8)((x * 7U + y * 13U) & 255U);
    p[1] = (u8)((x * x + y) & 255U);
    p[2] = (u8)(((x ^ y) * 4U) & 255U);
    p[3] = 255U;
}

static void set(u8 *p, u8 r, u8 g, u8 b)
{
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = 255U;
}

static void flip_pair(u32 c, u8 *ref, u8 *test)
{
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            u8 *r = ref + ((u64)y * W + x) * 4U;
            u8 *t = test + ((u64)y * W + x) * 4U;
            if (c == 0U) {
                pattern(x, y, r);
                pattern(x, y, t);
            } else if (c == 1U) {
                set(r, 128U, 128U, 128U);
                set(t, 140U, 140U, 140U);
            } else if (c == 2U) {
                set(r, 255U, 255U, 255U);
                set(t, 0U, 0U, 0U);
            } else if (c == 3U) {
                pattern(x, y, r);
                pattern(x, y, t);
                t[0] = (u8)(r[0] + 40U > 255U ? 255U : r[0] + 40U);
            } else if (c == 4U) {
                const u8 a = (((x / 8U) + (y / 8U)) & 1U) != 0U ? 230U : 25U;
                const u8 b = ((((x + 1U) / 8U) + (y / 8U)) & 1U) != 0U ? 230U : 25U;
                set(r, a, a, a);
                set(t, b, b, b);
            } else {
                set(r, 200U, 80U, 60U);
                set(t, 80U, 200U, 60U);
            }
        }
    }
}

NTEST(flip, agrees_with_the_reference_implementation)
{
    static u8 ref[W * H * 4U];
    static u8 test[W * H * 4U];
    static f32 map[W * H];
    for (u32 c = 0; c < 6U; ++c) {
        flip_pair(c, ref, test);
        GoldenFlipStats s;
        NTEST_ASSERT_EQ_INT(golden_flip(ref, test, W, H, 67.0F, nph_allocator_system(), map, &s),
                            NPH_RESULT_OK);
        NTEST_EXPECT_NEAR_F64(s.mean, g_expected[c][0], 1e-4);
        NTEST_EXPECT_NEAR_F64((f64)map[24U * W + 32U], g_expected[c][1], 2e-4);
        NTEST_EXPECT_NEAR_F64((f64)map[5U * W + 3U], g_expected[c][2], 2e-4);
        NTEST_EXPECT_NEAR_F64((f64)s.max, g_expected[c][3], 2e-4);
    }
}

NTEST(flip, statistics_are_consistent)
{
    static u8 ref[W * H * 4U];
    static u8 test[W * H * 4U];
    flip_pair(4U, ref, test);
    GoldenFlipStats s;
    NTEST_ASSERT_EQ_INT(golden_flip(ref, test, W, H, 0.0F, nph_allocator_system(), NULL, &s),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(s.p50 <= s.p95 && s.p95 <= s.p99 && s.p99 <= s.max);
    NTEST_EXPECT_TRUE(s.mean > 0.0 && s.mean <= (f64)s.max);
    // The default ppd is the paper's 67, and the viewing condition matters.
    NTEST_EXPECT_NEAR_F64(s.mean, g_expected[4][0], 1e-4);
    GoldenFlipStats closer;
    NTEST_ASSERT_EQ_INT(golden_flip(ref, test, W, H, 20.0F, nph_allocator_system(), NULL, &closer),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_f64_abs(closer.mean - s.mean) > 1e-3);
}

NTEST(flip, rejects_bad_arguments)
{
    u8 px[4] = {0, 0, 0, 255};
    GoldenFlipStats s;
    NphAllocator *a = nph_allocator_system();
    NTEST_EXPECT_EQ_INT(golden_flip(NULL, px, 1U, 1U, 67.0F, a, NULL, &s),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(golden_flip(px, px, 0U, 1U, 67.0F, a, NULL, &s),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(golden_flip(px, px, 1U, 1U, -1.0F, a, NULL, &s),
                        NPH_RESULT_INVALID_ARGUMENT);
    // One pixel works: identical gives zero.
    NTEST_ASSERT_EQ_INT(golden_flip(px, px, 1U, 1U, 67.0F, a, NULL, &s), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F64(s.mean, 0.0, 0.0);
}

NTEST(flip, magma_runs_from_black_to_pale_yellow)
{
    const f32 map[3] = {0.0F, 0.5F, 1.0F};
    u8 rgba[12];
    golden_flip_magma(map, 3U, 1U, rgba);
    // matplotlib's magma: (0, 0, 4), (183, 55, 121), (252, 253, 191).
    NTEST_EXPECT_TRUE(rgba[0] <= 2U && rgba[1] <= 2U && rgba[2] <= 6U);
    NTEST_EXPECT_TRUE(rgba[4] >= 175U && rgba[4] <= 190U && rgba[5] >= 48U && rgba[5] <= 62U &&
                      rgba[6] >= 114U && rgba[6] <= 128U);
    NTEST_EXPECT_TRUE(rgba[8] >= 245U && rgba[9] >= 240U && rgba[10] >= 180U && rgba[10] <= 200U);
    NTEST_EXPECT_EQ_UINT(rgba[3], 255U);
}
