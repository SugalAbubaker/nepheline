// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/math/random.h>

#include "test_math_support.h"

#include <string.h>

// ---------------------------------------------------------------------------
// PCG32
// ---------------------------------------------------------------------------

NTEST(pcg32, reference_vectors)
{
    // The first outputs of pcg32_srandom_r(42, 54) from O'Neill's reference
    // implementation (also reproduced with an independent Python model).
    static const u32 expected[8] = {0xA15C02B7U, 0x7B47F409U, 0xBA1D3330U, 0x83D2F293U,
                                    0xBFA4784BU, 0xCBED606EU, 0xBFC6A3ADU, 0x812FFF6DU};
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 42U, 54U);
    NTEST_ASSERT_EQ_UINT(rng.state, 0x185706B82C2E03F8ULL);
    NTEST_ASSERT_EQ_UINT(rng.inc, 0x6DU);
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&rng), expected[i]);
    }
    nph_pcg32_seed(&rng, 0U, 0U);
    NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&rng), 0xE4C14788U);
    NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&rng), 0x379C6516U);
    nph_pcg32_seed(&rng, 0x853C49E6748FEA9BULL, 0xDA3E39CB94B95BDBULL);
    NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&rng), 0x1BBEB4F2U);
    NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&rng), 0xE82E89E9U);
}

NTEST(pcg32, advance_equals_repeated_draws)
{
    NphPcg32 a;
    NphPcg32 b;
    nph_pcg32_seed(&a, 42U, 54U);
    nph_pcg32_seed(&b, 42U, 54U);
    for (u32 i = 0; i < 1000U; ++i) {
        (void)nph_pcg32_next_u32(&a);
    }
    nph_pcg32_advance(&b, 1000U);
    NTEST_ASSERT_EQ_UINT(a.state, b.state);
    NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&b), 0xEFEBEAB3U); // oracle value
    NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&a), 0xEFEBEAB3U);
    nph_pcg32_advance(&b, 0U);
    NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&a), nph_pcg32_next_u32(&b));
    // Large strides compose: advance(x) then advance(y) == advance(x + y).
    nph_pcg32_seed(&a, 7U, 9U);
    nph_pcg32_seed(&b, 7U, 9U);
    nph_pcg32_advance(&a, 123456789U);
    nph_pcg32_advance(&a, 987654321U);
    nph_pcg32_advance(&b, 123456789U + 987654321U);
    NTEST_ASSERT_EQ_UINT(a.state, b.state);
    // Wrapping around the period is a no-op: advancing by 2^64 - 1 then 1.
    nph_pcg32_advance(&a, NPH_U64_MAX);
    nph_pcg32_advance(&a, 1U);
    NTEST_ASSERT_EQ_UINT(a.state, b.state);
}

NTEST(pcg32, streams_and_reproducibility)
{
    NphPcg32 a;
    NphPcg32 b;
    nph_pcg32_seed(&a, 1U, 1U);
    nph_pcg32_seed(&b, 1U, 1U);
    for (u32 i = 0; i < 100U; ++i) {
        NTEST_ASSERT_EQ_UINT(nph_pcg32_next_u32(&a), nph_pcg32_next_u32(&b));
    }
    nph_pcg32_seed(&a, 1U, 1U);
    nph_pcg32_seed(&b, 1U, 2U);
    u32 same = 0U;
    for (u32 i = 0; i < 100U; ++i) {
        same += nph_pcg32_next_u32(&a) == nph_pcg32_next_u32(&b) ? 1U : 0U;
    }
    NTEST_ASSERT_LT_UINT(same, 3U);
    nph_pcg32_seed(&a, 1U, 1U);
    nph_pcg32_seed(&b, 2U, 1U);
    same = 0U;
    for (u32 i = 0; i < 100U; ++i) {
        same += nph_pcg32_next_u32(&a) == nph_pcg32_next_u32(&b) ? 1U : 0U;
    }
    NTEST_ASSERT_LT_UINT(same, 3U);
    // u64 is two u32 draws, high word first.
    nph_pcg32_seed(&a, 5U, 5U);
    nph_pcg32_seed(&b, 5U, 5U);
    const u64 wide = nph_pcg32_next_u64(&a);
    const u64 hi = nph_pcg32_next_u32(&b);
    const u64 lo = nph_pcg32_next_u32(&b);
    NTEST_ASSERT_EQ_UINT(wide, (hi << 32U) | lo);
}

NTEST(pcg32, bounded_is_in_range_and_unbiased)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 99U, 1U);
    for (u32 i = 0; i < 10000U; ++i) {
        NTEST_ASSERT_EQ_UINT(nph_pcg32_next_bounded(&rng, 1U), 0U);
        NTEST_ASSERT_LT_UINT(nph_pcg32_next_bounded(&rng, 7U), 7U);
        NTEST_ASSERT_LT_UINT(nph_pcg32_next_bounded(&rng, 1000003U), 1000003U);
    }
    // 10 buckets, 200 000 draws: each within 3% of the expectation.
    u32 buckets[10] = {0U};
    for (u32 i = 0; i < 200000U; ++i) {
        buckets[nph_pcg32_next_bounded(&rng, 10U)] += 1U;
    }
    for (u32 i = 0; i < 10U; ++i) {
        NTEST_ASSERT_TRUE(buckets[i] > 19400U && buckets[i] < 20600U);
    }
    // A bound just above 2^31 exercises the rejection zone heavily.
    for (u32 i = 0; i < 10000U; ++i) {
        NTEST_ASSERT_LT_UINT(nph_pcg32_next_bounded(&rng, 0x80000001U), 0x80000001U);
    }
    // The largest bound covers the whole range.
    bool saw_high = false;
    for (u32 i = 0; i < 1000U; ++i) {
        saw_high = saw_high || nph_pcg32_next_bounded(&rng, NPH_U32_MAX) > 0x80000000U;
    }
    NTEST_ASSERT_TRUE(saw_high);
}

NTEST(pcg32, floats_and_helpers)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 2024U, 10U);
    f64 sum32 = 0.0;
    f64 sum64 = 0.0;
    u32 trues = 0U;
    for (u32 i = 0; i < 1000000U; ++i) {
        const f32 f = nph_pcg32_next_f32(&rng);
        NTEST_ASSERT_TRUE(f >= 0.0F && f < 1.0F);
        sum32 += (f64)f;
        const f64 d = nph_pcg32_next_f64(&rng);
        NTEST_ASSERT_TRUE(d >= 0.0 && d < 1.0);
        sum64 += d;
        trues += nph_pcg32_next_bool(&rng) ? 1U : 0U;
    }
    NTEST_ASSERT_NEAR_F64(sum32 / 1000000.0, 0.5, 0.002);
    NTEST_ASSERT_NEAR_F64(sum64 / 1000000.0, 0.5, 0.002);
    NTEST_ASSERT_TRUE(trues > 495000U && trues < 505000U);
    // The 24-bit path can produce the largest value below 1 and 0 itself.
    bool saw_zero = false;
    bool saw_max = false;
    for (u32 i = 0; i < 100000000U && !(saw_zero && saw_max); ++i) {
        const u32 bits = nph_pcg32_next_u32(&rng) >> 8U;
        saw_zero = saw_zero || bits == 0U;
        saw_max = saw_max || bits == 0xFFFFFFU;
    }
    NTEST_ASSERT_TRUE(saw_zero && saw_max);
    for (u32 i = 0; i < 1000U; ++i) {
        const f32 r = nph_pcg32_next_range_f32(&rng, -3.0F, 2.0F);
        NTEST_ASSERT_TRUE(r >= -3.0F && r < 2.0F);
        const NphVec2 v2 = nph_pcg32_next_vec2(&rng);
        NTEST_ASSERT_TRUE(v2.x >= 0.0F && v2.x < 1.0F && v2.y >= 0.0F && v2.y < 1.0F);
        const NphVec3 v3 = nph_pcg32_next_vec3(&rng);
        NTEST_ASSERT_TRUE(v3.x >= 0.0F && v3.x < 1.0F && v3.y >= 0.0F && v3.y < 1.0F &&
                          v3.z >= 0.0F && v3.z < 1.0F);
    }
}

// A vector's coordinates are the stream's next draws in order, x first, so
// the same seed makes the same point with every compiler (whatever order it
// evaluates a call's arguments in).
NTEST(pcg32, vectors_draw_their_coordinates_in_order)
{
    NphPcg32 a;
    NphPcg32 b;
    nph_pcg32_seed(&a, 42U, 54U);
    nph_pcg32_seed(&b, 42U, 54U);
    for (u32 i = 0; i < 100U; ++i) {
        const NphVec2 v2 = nph_pcg32_next_vec2(&a);
        const f32 x2 = nph_pcg32_next_f32(&b);
        const f32 y2 = nph_pcg32_next_f32(&b);
        NTEST_ASSERT_ULP_F32(v2.x, x2, 0U);
        NTEST_ASSERT_ULP_F32(v2.y, y2, 0U);
        const NphVec3 v3 = nph_pcg32_next_vec3(&a);
        const f32 x3 = nph_pcg32_next_f32(&b);
        const f32 y3 = nph_pcg32_next_f32(&b);
        const f32 z3 = nph_pcg32_next_f32(&b);
        NTEST_ASSERT_ULP_F32(v3.x, x3, 0U);
        NTEST_ASSERT_ULP_F32(v3.y, y3, 0U);
        NTEST_ASSERT_ULP_F32(v3.z, z3, 0U);
    }
}

// ---------------------------------------------------------------------------
// Low-discrepancy sequences
// ---------------------------------------------------------------------------

NTEST(lds, radical_inverse)
{
    NTEST_ASSERT_ULP_F32(nph_radical_inverse_base2(0U), 0.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_radical_inverse_base2(1U), 0.5F, 0U);
    NTEST_ASSERT_ULP_F32(nph_radical_inverse_base2(2U), 0.25F, 0U);
    NTEST_ASSERT_ULP_F32(nph_radical_inverse_base2(3U), 0.75F, 0U);
    NTEST_ASSERT_ULP_F32(nph_radical_inverse_base2(4U), 0.125F, 0U);
    NTEST_ASSERT_ULP_F32(nph_radical_inverse_base2(NPH_U32_MAX), 1.0F - 1.0F / 16777216.0F, 0U);
    // The general routine agrees with the bit reversal in base 2.
    for (u32 i = 0; i < 5000U; ++i) {
        NTEST_ASSERT_ULP_F32(nph_radical_inverse(i, 2U), nph_radical_inverse_base2(i), 1U);
    }
    static const f32 base3[7] = {1.0F / 3.0F, 2.0F / 3.0F, 1.0F / 9.0F, 4.0F / 9.0F,
                                 7.0F / 9.0F, 2.0F / 9.0F, 5.0F / 9.0F};
    static const f32 base5[7] = {0.2F, 0.4F, 0.6F, 0.8F, 0.04F, 0.24F, 0.44F};
    for (u32 i = 0; i < 7U; ++i) {
        NTEST_ASSERT_ULP_F32(nph_radical_inverse(i + 1U, 3U), base3[i], 1U);
        NTEST_ASSERT_ULP_F32(nph_radical_inverse(i + 1U, 5U), base5[i], 1U);
    }
    for (u32 i = 0; i < 10000U; ++i) {
        const f32 r = nph_radical_inverse(i * 7919U, 7U);
        NTEST_ASSERT_TRUE(r >= 0.0F && r < 1.0F);
    }
    NTEST_ASSERT_TRUE(nph_radical_inverse(NPH_U32_MAX, 3U) < 1.0F);
}

NTEST(lds, halton_fills_strata_evenly)
{
    // Bases 2 and 3: the first 72k points put exactly k in each cell of an
    // 8 x 9 grid; with 1024 points every cell holds 14 or 15.
    u32 cells[8][9] = {{0U}};
    for (u32 i = 0; i < 1024U; ++i) {
        const NphVec2 p = nph_halton_2d(i);
        NTEST_ASSERT_TRUE(p.x >= 0.0F && p.x < 1.0F && p.y >= 0.0F && p.y < 1.0F);
        cells[(u32)(p.x * 8.0F)][(u32)(p.y * 9.0F)] += 1U;
    }
    for (u32 x = 0; x < 8U; ++x) {
        for (u32 y = 0; y < 9U; ++y) {
            NTEST_ASSERT_TRUE(cells[x][y] == 14U || cells[x][y] == 15U);
        }
    }
}

NTEST(lds, r1_r2_are_in_range_and_well_spread)
{
    NTEST_ASSERT_ULP_F32(nph_r1(0U), 0.5F, 0U);
    TM_ASSERT_VEC2_NEAR(nph_r2(0U), nph_vec2(0.5F, 0.5F), 0.0F);
    u32 cells1[16] = {0U};
    u32 cells2[8][8] = {{0U}};
    for (u32 i = 0; i < 1024U; ++i) {
        const f32 a = nph_r1(i);
        const NphVec2 p = nph_r2(i);
        NTEST_ASSERT_TRUE(a >= 0.0F && a < 1.0F);
        NTEST_ASSERT_TRUE(p.x >= 0.0F && p.x < 1.0F && p.y >= 0.0F && p.y < 1.0F);
        cells1[(u32)(a * 16.0F)] += 1U;
        cells2[(u32)(p.x * 8.0F)][(u32)(p.y * 8.0F)] += 1U;
        if (i > 0U) {
            // Consecutive points are never close (the whole point of the
            // sequence for per-frame jitter).
            NTEST_ASSERT_TRUE(nph_f32_abs(a - nph_r1(i - 1U)) > 0.3F);
            NTEST_ASSERT_TRUE(nph_vec2_distance(p, nph_r2(i - 1U)) > 0.3F);
        }
    }
    for (u32 i = 0; i < 16U; ++i) {
        NTEST_ASSERT_TRUE(cells1[i] >= 60U && cells1[i] <= 68U); // 64 expected
    }
    for (u32 x = 0; x < 8U; ++x) {
        for (u32 y = 0; y < 8U; ++y) {
            NTEST_ASSERT_TRUE(cells2[x][y] >= 8U && cells2[x][y] <= 24U); // 16 expected
        }
    }
}

// ---------------------------------------------------------------------------
// Sample warps
// ---------------------------------------------------------------------------

NTEST(sample, disk_concentric)
{
    TM_ASSERT_VEC2_NEAR(nph_sample_disk_concentric(nph_vec2(0.5F, 0.5F)), nph_vec2_zero(), 0.0F);
    // The square's corners and edge midpoints land on the unit circle.
    NTEST_ASSERT_NEAR_F32(nph_vec2_length(nph_sample_disk_concentric(nph_vec2(0.0F, 0.0F))), 1.0F,
                          1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_vec2_length(nph_sample_disk_concentric(nph_vec2(1.0F, 1.0F))), 1.0F,
                          1e-6F);
    TM_ASSERT_VEC2_NEAR(nph_sample_disk_concentric(nph_vec2(1.0F, 0.5F)), nph_vec2(1.0F, 0.0F),
                        1e-6F);
    TM_ASSERT_VEC2_NEAR(nph_sample_disk_concentric(nph_vec2(0.5F, 1.0F)), nph_vec2(0.0F, 1.0F),
                        1e-6F);
    TM_ASSERT_VEC2_NEAR(nph_sample_disk_concentric(nph_vec2(0.0F, 0.5F)), nph_vec2(-1.0F, 0.0F),
                        1e-6F);
    TM_ASSERT_VEC2_NEAR(nph_sample_disk_concentric(nph_vec2(0.5F, 0.0F)), nph_vec2(0.0F, -1.0F),
                        1e-6F);
    // Area preserving: a quarter of the samples fall within radius 1/2.
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 3U, 3U);
    u32 inner = 0U;
    for (u32 i = 0; i < 200000U; ++i) {
        const NphVec2 d = nph_sample_disk_concentric(nph_pcg32_next_vec2(&rng));
        NTEST_ASSERT_TRUE(nph_vec2_length_sq(d) <= 1.0F + 1e-6F);
        inner += nph_vec2_length_sq(d) < 0.25F ? 1U : 0U;
    }
    NTEST_ASSERT_NEAR_F64((f64)inner / 200000.0, 0.25, 0.005);
}

NTEST(sample, sphere_and_hemispheres)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 4U, 4U);
    u32 cap_sphere = 0U;
    u32 cap_hemi = 0U;
    u32 cap_cos = 0U;
    f64 mean_z_cos = 0.0;
    f64 mean_z_sphere = 0.0;
    const u32 n = 200000U;
    for (u32 i = 0; i < n; ++i) {
        const NphVec3 s = nph_sample_sphere_uniform(nph_pcg32_next_vec2(&rng));
        const NphVec3 h = nph_sample_hemisphere_uniform(nph_pcg32_next_vec2(&rng));
        const NphVec3 c = nph_sample_hemisphere_cosine(nph_pcg32_next_vec2(&rng));
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(s, 1e-5F));
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(h, 1e-5F));
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(c, 1e-5F));
        NTEST_ASSERT_TRUE(h.z >= 0.0F && c.z >= 0.0F);
        cap_sphere += s.z > 0.5F ? 1U : 0U; // cap area fraction (1 - 0.5) / 2
        cap_hemi += h.z > 0.5F ? 1U : 0U;   // (1 - 0.5)
        cap_cos += c.z > 0.5F ? 1U : 0U;    // 1 - 0.5^2
        mean_z_cos += (f64)c.z;             // E[cos] = 2/3
        mean_z_sphere += (f64)s.z;
    }
    NTEST_ASSERT_NEAR_F64((f64)cap_sphere / n, 0.25, 0.005);
    NTEST_ASSERT_NEAR_F64((f64)cap_hemi / n, 0.5, 0.005);
    NTEST_ASSERT_NEAR_F64((f64)cap_cos / n, 0.75, 0.005);
    NTEST_ASSERT_NEAR_F64(mean_z_cos / n, 2.0 / 3.0, 0.003);
    NTEST_ASSERT_NEAR_F64(mean_z_sphere / n, 0.0, 0.005);
    NTEST_ASSERT_NEAR_F32(nph_pdf_hemisphere_cosine(1.0F), NPH_INV_PI, 1e-7F);
    NTEST_ASSERT_NEAR_F32(NPH_PDF_SPHERE_UNIFORM * 4.0F * NPH_PI, 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(NPH_PDF_HEMISPHERE_UNIFORM * 2.0F * NPH_PI, 1.0F, 1e-6F);
    // Degenerate inputs stay finite.
    NTEST_ASSERT_TRUE(nph_vec3_is_finite(nph_sample_sphere_uniform(nph_vec2(0.0F, 0.0F))));
    NTEST_ASSERT_TRUE(nph_vec3_is_finite(nph_sample_sphere_uniform(nph_vec2(1.0F, 1.0F))));
    NTEST_ASSERT_TRUE(nph_vec3_is_finite(nph_sample_hemisphere_uniform(nph_vec2(1.0F, 0.0F))));
}

NTEST(sample, triangle)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 5U, 5U);
    f64 mean_u = 0.0;
    f64 mean_v = 0.0;
    const u32 n = 200000U;
    for (u32 i = 0; i < n; ++i) {
        const NphVec2 b = nph_sample_triangle_uniform(nph_pcg32_next_vec2(&rng));
        NTEST_ASSERT_TRUE(b.x >= 0.0F && b.y >= 0.0F && b.x + b.y <= 1.0F + 1e-6F);
        mean_u += (f64)b.x;
        mean_v += (f64)b.y;
    }
    NTEST_ASSERT_NEAR_F64(mean_u / n, 1.0 / 3.0, 0.003);
    NTEST_ASSERT_NEAR_F64(mean_v / n, 1.0 / 3.0, 0.003);
}

// ---------------------------------------------------------------------------
// Blue noise
// ---------------------------------------------------------------------------

static void *failing_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(size);
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    return NULL;
}

static void *failing_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                             NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(ptr);
    NPH_UNUSED(old_size);
    NPH_UNUSED(new_size);
    NPH_UNUSED(align);
    NPH_UNUSED(site);
    return NULL;
}

static void failing_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    NPH_UNUSED(self);
    NPH_UNUSED(ptr);
    NPH_UNUSED(size);
    NPH_UNUSED(align);
    NPH_UNUSED(site);
}

static const NphAllocatorVtable g_failing_vtable = {failing_alloc, failing_realloc, failing_free};

// Shortest toroidal distance between two texels.
static f32 torus_distance(u32 a, u32 b, u32 w, u32 h)
{
    const i32 dx = nph_i32_abs((i32)(a % w) - (i32)(b % w));
    const i32 dy = nph_i32_abs((i32)(a / w) - (i32)(b / w));
    const i32 wx = nph_min_i32(dx, (i32)w - dx);
    const i32 wy = nph_min_i32(dy, (i32)h - dy);
    return nph_f32_sqrt((f32)(wx * wx + wy * wy));
}

// Mean nearest-neighbour distance among the texels with rank < k.
static f32 mean_nearest_distance(const u32 *ranks, u32 w, u32 h, u32 k, f32 *out_min)
{
    const u32 n = w * h;
    f32 sum = 0.0F;
    f32 min_all = NPH_F32_INFINITY;
    for (u32 i = 0; i < n; ++i) {
        if (ranks[i] >= k) {
            continue;
        }
        f32 nearest = NPH_F32_INFINITY;
        for (u32 j = 0; j < n; ++j) {
            if (j != i && ranks[j] < k) {
                nearest = nph_f32_min(nearest, torus_distance(i, j, w, h));
            }
        }
        sum += nearest;
        min_all = nph_f32_min(min_all, nearest);
    }
    *out_min = min_all;
    return sum / (f32)k;
}

typedef struct BlueNoiseFixture {
    NphDebugAllocator debug;
    NphAllocator *alloc;
} BlueNoiseFixture;

NTEST_SETUP(BlueNoiseFixture)
{
    nph_debug_allocator_init(&f->debug, nph_allocator_system());
    f->alloc = nph_debug_allocator_allocator(&f->debug);
}

NTEST_TEARDOWN(BlueNoiseFixture)
{
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->debug, NULL, NULL), 0U);
}

NTEST_F(BlueNoiseFixture, generates_a_deterministic_permutation)
{
    enum {
        W = 32,
        H = 32,
        N = W * H
    };
    u32 ranks[N];
    u32 again[N];
    u32 other[N];
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, W, H, 1.5F, 7U, ranks), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, W, H, 1.5F, 7U, again), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, W, H, 1.5F, 8U, other), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_MEM(ranks, again, sizeof ranks);
    NTEST_ASSERT_FALSE(memcmp(ranks, other, sizeof ranks) == 0);
    // Every rank appears exactly once.
    u8 seen[N] = {0U};
    for (u32 i = 0; i < N; ++i) {
        NTEST_ASSERT_LT_UINT(ranks[i], N);
        NTEST_ASSERT_EQ_UINT(seen[ranks[i]], 0U);
        seen[ranks[i]] = 1U;
    }
}

NTEST_F(BlueNoiseFixture, early_ranks_are_evenly_spaced)
{
    enum {
        W = 32,
        H = 32,
        N = W * H,
        K = N / 8
    };
    u32 ranks[N];
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, W, H, 1.5F, 11U, ranks), NPH_RESULT_OK);
    f32 blue_min;
    const f32 blue_mean = mean_nearest_distance(ranks, W, H, K, &blue_min);
    // White noise of the same density for comparison.
    u32 white[N];
    for (u32 i = 0; i < N; ++i) {
        white[i] = i;
    }
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 11U, 11U);
    for (u32 i = N - 1U; i > 0U; --i) {
        const u32 j = nph_pcg32_next_bounded(&rng, i + 1U);
        const u32 t = white[i];
        white[i] = white[j];
        white[j] = t;
    }
    f32 white_min;
    const f32 white_mean = mean_nearest_distance(white, W, H, K, &white_min);
    NTEST_LOG("K=%u: blue min %.3f mean %.3f; white min %.3f mean %.3f", (u32)K, (f64)blue_min,
              (f64)blue_mean, (f64)white_min, (f64)white_mean);
    // At one eighth density the ideal spacing is sqrt(8) = 2.83. Blue noise
    // keeps every pair at least 2 apart and averages near the ideal; white
    // noise has adjacent pairs and a much smaller mean.
    NTEST_ASSERT_TRUE(blue_min >= 2.0F);
    NTEST_ASSERT_TRUE(blue_mean > 2.2F); // measured 2.30 on the reference seed
    NTEST_ASSERT_TRUE(white_min < 1.5F);
    NTEST_ASSERT_TRUE(blue_mean > white_mean * 1.3F);
    // At one quarter density (ideal spacing 2) the pattern is denser than the
    // Gaussian's width and occasional adjacent pairs appear, but the mean
    // spacing stays well above white noise's (about 1.0 here).
    f32 quarter_min;
    const f32 quarter_mean = mean_nearest_distance(ranks, W, H, N / 4U, &quarter_min);
    f32 quarter_white_min;
    const f32 quarter_white_mean = mean_nearest_distance(white, W, H, N / 4U, &quarter_white_min);
    NTEST_LOG("K=%u: blue min %.3f mean %.3f; white min %.3f mean %.3f", (u32)(N / 4U),
              (f64)quarter_min, (f64)quarter_mean, (f64)quarter_white_min, (f64)quarter_white_mean);
    NTEST_ASSERT_TRUE(quarter_mean > 1.3F); // measured 1.46 on the reference seed
    NTEST_ASSERT_TRUE(quarter_mean > quarter_white_mean * 1.15F); // measured 1.46 vs 1.17
}

NTEST_F(BlueNoiseFixture, non_square_and_tiny_sizes)
{
    u32 ranks[16 * 8];
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 16U, 8U, 1.5F, 1U, ranks), NPH_RESULT_OK);
    u8 seen[16 * 8] = {0U};
    for (u32 i = 0; i < 16U * 8U; ++i) {
        NTEST_ASSERT_LT_UINT(ranks[i], 16U * 8U);
        NTEST_ASSERT_EQ_UINT(seen[ranks[i]], 0U);
        seen[ranks[i]] = 1U;
    }
    u32 one = 99U;
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 1U, 1U, 1.5F, 1U, &one), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(one, 0U);
    u32 four[4];
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 2U, 2U, 0.7F, 1U, four), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(four[0] + four[1] + four[2] + four[3], 6U);
    u32 line[7];
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 7U, 1U, 1.0F, 3U, line), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(line[0] + line[1] + line[2] + line[3] + line[4] + line[5] + line[6], 21U);
}

NTEST_F(BlueNoiseFixture, rejects_bad_arguments_and_reports_oom)
{
    u32 ranks[4];
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 0U, 2U, 1.5F, 1U, ranks),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 2U, 0U, 1.5F, 1U, ranks),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 2U, 2U, 0.0F, 1U, ranks),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 2U, 2U, 1.5F, 1U, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(NULL, 2U, 2U, 1.5F, 1U, ranks),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(f->alloc, 65536U, 65536U, 1.5F, 1U, ranks),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphAllocator failing = {&g_failing_vtable, "failing"};
    NTEST_ASSERT_EQ_INT(nph_blue_noise_generate(&failing, 2U, 2U, 1.5F, 1U, ranks),
                        NPH_RESULT_OUT_OF_MEMORY);
}
