// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/pack.h>

#include "test_math_support.h"

// ---------------------------------------------------------------------------
// binary16
// ---------------------------------------------------------------------------

typedef struct HalfVector {
    u16 half;
    f32 value;
} HalfVector;

// Values computed independently with Python's struct '<e' format.
static const HalfVector g_half_vectors[] = {
    {0x3C00U, 1.0F},
    {0x3555U, 0.3332519531F},
    {0xC000U, -2.0F},
    {0x7BFFU, 65504.0F},
    {0x0001U, 5.960464478e-08F},
    {0x03FFU, 6.097555161e-05F},
    {0x0400U, 6.103515625e-05F},
    {0x3800U, 0.5F},
    {0x4248U, 3.140625F},
    {0x5640U, 100.0F},
    {0x2E66U, 0.0999755859375F},
};

NTEST(half, decode_vectors)
{
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_half_vectors); ++i) {
        NTEST_ASSERT_ULP_F32(nph_f16_to_f32(g_half_vectors[i].half), g_half_vectors[i].value, 0U);
    }
    NTEST_ASSERT_EQ_UINT(nph_f32_to_bits(nph_f16_to_f32(0x8000U)), 0x80000000U); // -0
    NTEST_ASSERT_EQ_UINT(nph_f32_to_bits(nph_f16_to_f32(0x0000U)), 0U);
    NTEST_ASSERT_TRUE(nph_f32_is_inf(nph_f16_to_f32(NPH_F16_INF)));
    NTEST_ASSERT_TRUE(nph_f16_to_f32(0xFC00U) < 0.0F && nph_f32_is_inf(nph_f16_to_f32(0xFC00U)));
    NTEST_ASSERT_TRUE(nph_f32_is_nan(nph_f16_to_f32(NPH_F16_NAN)));
    NTEST_ASSERT_TRUE(nph_f32_is_nan(nph_f16_to_f32(0x7C01U)));
    NTEST_ASSERT_ULP_F32(nph_f16_to_f32(NPH_F16_ONE), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f16_to_f32(0x7BFFU), NPH_F16_MAX, 0U);
    NTEST_ASSERT_ULP_F32(nph_f16_to_f32(0x3C01U) - 1.0F, NPH_F16_EPSILON, 0U);
}

NTEST(half, encode_vectors_and_rounding)
{
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_half_vectors); ++i) {
        NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(g_half_vectors[i].value), g_half_vectors[i].half);
    }
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(0.1F), 0x2E66U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(-0.0F), 0x8000U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(0.0F), 0x0000U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(NPH_F32_INFINITY), 0x7C00U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(-NPH_F32_INFINITY), 0xFC00U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(1e30F), 0x7C00U);    // overflow
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(65520.0F), 0x7C00U); // halfway to 65536, ties to even (inf)
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(65519.0F), 0x7BFFU);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(1e-8F), 0x0000U); // below half the smallest subnormal
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(5.9604645e-08F), 0x0001U); // 2^-24
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(2.9802322e-08F), 0x0000U); // 2^-25: tie, rounds to even (0)
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(4.4703484e-08F), 0x0001U); // 1.5 * 2^-25 rounds up
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(-4.4703484e-08F), 0x8001U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(6.1035156e-05F), 0x0400U);  // smallest normal
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(6.0975552e-05F), 0x03FFU);  // largest subnormal
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(1.00048828125F), 0x3C00U);  // 1 + 2^-11: tie, even
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(1.000732421875F), 0x3C01U); // 1 + 1.5 * 2^-11: up
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(1.001464843750F), 0x3C02U); // 1 + 3 * 2^-11: tie, even (2)
    // NaN stays NaN with its sign.
    const u16 nan = nph_f32_to_f16(nph_f32_from_bits(0x7FC00000U));
    NTEST_ASSERT_TRUE((nan & 0x7C00U) == 0x7C00U && (nan & 0x3FFU) != 0U);
    const u16 neg_nan = nph_f32_to_f16(nph_f32_from_bits(0xFFC00001U));
    NTEST_ASSERT_TRUE((neg_nan & 0xFC00U) == 0xFC00U && (neg_nan & 0x3FFU) != 0U);
    // Subnormal carry into the smallest normal.
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(nph_f16_to_f32(0x03FFU) + 3.1e-08F), 0x0400U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(nph_f16_to_f32(0x03FFU) + 2.9e-08F), 0x03FFU);
}

NTEST(half, every_half_round_trips_exactly)
{
    for (u32 h = 0; h < 65536U; ++h) {
        const f32 f = nph_f16_to_f32((u16)h);
        if (nph_f32_is_nan(f)) {
            NTEST_ASSERT_TRUE(nph_f32_is_nan(nph_f16_to_f32(nph_f32_to_f16(f))));
            continue;
        }
        NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(f), h);
    }
}

// Independent reference: the binary16 bit patterns of non-negative values
// are ordered like the values, so the nearest half to x is found by binary
// search over the decoder, with ties broken toward the even pattern.
static u16 reference_f32_to_f16(f32 x)
{
    if (nph_f32_is_nan(x)) {
        return 0x7E00U;
    }
    const u32 sign = x < 0.0F || (x == 0.0F && nph_f32_to_bits(x) != 0U) ? 0x8000U : 0U;
    const f32 a = nph_f32_abs(x);
    if (a >= 65520.0F) {
        return (u16)(sign | 0x7C00U);
    }
    // Largest pattern whose value is <= a.
    u32 lo = 0U;
    u32 hi = 0x7BFFU;
    while (lo < hi) {
        const u32 mid = (lo + hi + 1U) / 2U;
        if (nph_f16_to_f32((u16)mid) <= a) {
            lo = mid;
        } else {
            hi = mid - 1U;
        }
    }
    const f32 below = nph_f16_to_f32((u16)lo);
    const f32 above = nph_f16_to_f32((u16)(lo + 1U)); // 0x7C00 (inf) at the top is fine as a bound
    const f64 d_below = (f64)a - (f64)below;
    const f64 d_above = (f64)above - (f64)a;
    u32 r = lo;
    if (d_above < d_below || (d_above == d_below && (lo & 1U) != 0U)) {
        r = lo + 1U;
    }
    return (u16)(sign | r);
}

NTEST(half, encode_matches_reference_on_random_floats)
{
    u64 rng = 50U;
    for (u32 i = 0; i < 1000000U; ++i) {
        // Random bit patterns cover every exponent, including subnormals and
        // values far outside the half range.
        const f32 x = nph_f32_from_bits((u32)tm_rng_next(&rng));
        if (nph_f32_is_nan(x)) {
            continue;
        }
        const u16 got = nph_f32_to_f16(x);
        const u16 want = reference_f32_to_f16(x);
        if (got != want) {
            NTEST_FAIL("f32 %.9g (0x%08X): got 0x%04X, reference 0x%04X", (f64)x,
                       nph_f32_to_bits(x), got, want);
        }
    }
    // And a dense sweep of the half range itself.
    for (u32 i = 0; i < 200000U; ++i) {
        const f32 x = tm_rng_range(&rng, -70000.0F, 70000.0F);
        NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(x), reference_f32_to_f16(x));
    }
    for (u32 i = 0; i < 200000U; ++i) {
        const f32 x = tm_rng_range(&rng, -1e-4F, 1e-4F);
        NTEST_ASSERT_EQ_UINT(nph_f32_to_f16(x), reference_f32_to_f16(x));
    }
}

NTEST(half, arrays_and_packs)
{
    const f32 in[5] = {1.0F, -2.5F, 0.1F, 65504.0F, 0.0F};
    u16 h[5];
    f32 back[5];
    nph_f32_to_f16_array(in, h, 5U);
    nph_f16_to_f32_array(h, back, 5U);
    for (u32 i = 0; i < 5U; ++i) {
        NTEST_ASSERT_EQ_UINT(h[i], nph_f32_to_f16(in[i]));
        NTEST_ASSERT_ULP_F32(back[i], nph_f16_to_f32(h[i]), 0U);
    }
    const u32 p2 = nph_pack_f16x2(nph_vec2(1.0F, -2.0F));
    NTEST_ASSERT_EQ_UINT(p2, 0xC0003C00U);
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_unpack_f16x2(p2), nph_vec2(1.0F, -2.0F)));
    const u64 p4 = nph_pack_f16x4(nph_vec4(1.0F, -2.0F, 0.5F, 100.0F));
    NTEST_ASSERT_EQ_UINT(p4, 0x56403800C0003C00ULL);
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_unpack_f16x4(p4), nph_vec4(1.0F, -2.0F, 0.5F, 100.0F)));
}

// ---------------------------------------------------------------------------
// Normalised integers
// ---------------------------------------------------------------------------

NTEST(norm, unorm_and_snorm_follow_vulkan_rules)
{
    NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm8(0.0F), 0U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm8(1.0F), 255U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm8(0.5F), 128U); // 127.5 rounds up
    NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm8(-3.0F), 0U);  // clamped
    NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm8(7.0F), 255U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm16(1.0F), 65535U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm16(0.5F), 32768U);
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm8(1.0F), 127);
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm8(-1.0F), -127);
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm8(-5.0F), -127); // clamped
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm8(0.0F), 0);
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm8(0.5F), 64); // 63.5 rounds away from zero
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm8(-0.5F), -64);
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm16(1.0F), 32767);
    NTEST_ASSERT_EQ_INT(nph_f32_to_snorm16(-1.0F), -32767);
    // Both most-negative codes decode to -1.
    NTEST_ASSERT_ULP_F32(nph_snorm8_to_f32((i8)-128), -1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_snorm8_to_f32((i8)-127), -1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_snorm8_to_f32(127), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_snorm16_to_f32((i16)-32768), -1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_snorm16_to_f32(32767), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_unorm8_to_f32(255U), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_unorm16_to_f32(65535U), 1.0F, 0U);
    // Every code round-trips through float exactly.
    for (u32 v = 0; v < 256U; ++v) {
        NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm8(nph_unorm8_to_f32((u8)v)), v);
        NTEST_ASSERT_EQ_INT(nph_f32_to_snorm8(nph_snorm8_to_f32((i8)(u8)v)),
                            v == 128U ? -127 : (i8)(u8)v);
    }
    for (u32 v = 0; v < 65536U; ++v) {
        NTEST_ASSERT_EQ_UINT(nph_f32_to_unorm16(nph_unorm16_to_f32((u16)v)), v);
        NTEST_ASSERT_EQ_INT(nph_f32_to_snorm16(nph_snorm16_to_f32((i16)(u16)v)),
                            v == 32768U ? -32767 : (i16)(u16)v);
    }
}

NTEST(norm, packed_vectors)
{
    const u32 rgba = nph_pack_unorm8x4(nph_vec4(1.0F, 0.0F, 0.5F, 1.0F));
    NTEST_ASSERT_EQ_UINT(rgba, 0xFF8000FFU); // x in the low byte
    TM_ASSERT_VEC4_NEAR(nph_unpack_unorm8x4(rgba), nph_vec4(1.0F, 0.0F, 128.0F / 255.0F, 1.0F),
                        0.0F);
    const u32 sn = nph_pack_snorm8x4(nph_vec4(1.0F, -1.0F, 0.0F, 0.5F));
    NTEST_ASSERT_EQ_UINT(sn, 0x4000817FU);
    TM_ASSERT_VEC4_NEAR(nph_unpack_snorm8x4(sn), nph_vec4(1.0F, -1.0F, 0.0F, 64.0F / 127.0F), 0.0F);
    const u32 un16 = nph_pack_unorm16x2(nph_vec2(0.0F, 1.0F));
    NTEST_ASSERT_EQ_UINT(un16, 0xFFFF0000U);
    TM_ASSERT_VEC2_NEAR(nph_unpack_unorm16x2(un16), nph_vec2(0.0F, 1.0F), 0.0F);
    const u32 sn16 = nph_pack_snorm16x2(nph_vec2(-1.0F, 1.0F));
    NTEST_ASSERT_EQ_UINT(sn16, 0x7FFF8001U);
    TM_ASSERT_VEC2_NEAR(nph_unpack_snorm16x2(sn16), nph_vec2(-1.0F, 1.0F), 0.0F);
    u64 rng = 51U;
    for (u32 i = 0; i < 10000U; ++i) {
        const NphVec4 v = tm_rng_vec4(&rng, 0.0F, 1.0F);
        TM_ASSERT_VEC4_NEAR(nph_unpack_unorm8x4(nph_pack_unorm8x4(v)), v, 0.5F / 255.0F + 1e-6F);
        const NphVec4 s = tm_rng_vec4(&rng, -1.0F, 1.0F);
        TM_ASSERT_VEC4_NEAR(nph_unpack_snorm8x4(nph_pack_snorm8x4(s)), s, 0.5F / 127.0F + 1e-6F);
        const NphVec2 u2 = tm_rng_vec2(&rng, 0.0F, 1.0F);
        TM_ASSERT_VEC2_NEAR(nph_unpack_unorm16x2(nph_pack_unorm16x2(u2)), u2,
                            0.5F / 65535.0F + 1e-7F);
        const NphVec2 s2 = tm_rng_vec2(&rng, -1.0F, 1.0F);
        TM_ASSERT_VEC2_NEAR(nph_unpack_snorm16x2(nph_pack_snorm16x2(s2)), s2,
                            0.5F / 32767.0F + 1e-7F);
    }
}

// ---------------------------------------------------------------------------
// Octahedral normals
// ---------------------------------------------------------------------------

NTEST(oct, axes_round_trip_exactly)
{
    const NphVec3 axes[6] = {nph_vec3_unit_x(),           nph_vec3_unit_y(),
                             nph_vec3_unit_z(),           nph_vec3(-1.0F, 0.0F, 0.0F),
                             nph_vec3(0.0F, -1.0F, 0.0F), nph_vec3(0.0F, 0.0F, -1.0F)};
    for (u32 i = 0; i < 6U; ++i) {
        TM_ASSERT_VEC3_NEAR(nph_oct_decode(nph_oct_encode(axes[i])), axes[i], 0.0F);
        TM_ASSERT_VEC3_NEAR(nph_oct_decode_snorm16(nph_oct_encode_snorm16(axes[i])), axes[i], 0.0F);
        TM_ASSERT_VEC3_NEAR(nph_oct_decode_snorm8(nph_oct_encode_snorm8(axes[i])), axes[i], 0.0F);
    }
    TM_ASSERT_VEC2_NEAR(nph_oct_encode(nph_vec3_unit_z()), nph_vec2_zero(), 0.0F);
    TM_ASSERT_VEC2_NEAR(nph_oct_encode(nph_vec3_unit_x()), nph_vec2(1.0F, 0.0F), 0.0F);
    // -Z folds to the corners of the square.
    const NphVec2 e = nph_oct_encode(nph_vec3(0.0F, 0.0F, -1.0F));
    NTEST_ASSERT_TRUE(nph_f32_abs(e.x) == 1.0F && nph_f32_abs(e.y) == 1.0F);
}

NTEST(oct, random_normals_within_error_bounds)
{
    u64 rng = 52U;
    f32 worst_f32 = 0.0F;
    f32 worst_16 = 0.0F;
    f32 worst_8 = 0.0F;
    for (u32 i = 0; i < 200000U; ++i) {
        const NphVec3 n = tm_rng_unit_vec3(&rng);
        const NphVec2 e = nph_oct_encode(n);
        NTEST_ASSERT_TRUE(e.x >= -1.0F && e.x <= 1.0F && e.y >= -1.0F && e.y <= 1.0F);
        const NphVec3 d = nph_oct_decode(e);
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(d, 1e-5F));
        worst_f32 = nph_f32_max(worst_f32, nph_vec3_angle(n, d));
        worst_16 = nph_f32_max(
            worst_16, nph_vec3_angle(n, nph_oct_decode_snorm16(nph_oct_encode_snorm16(n))));
        worst_8 = nph_f32_max(worst_8,
                              nph_vec3_angle(n, nph_oct_decode_snorm8(nph_oct_encode_snorm8(n))));
    }
    NTEST_LOG("worst angular error: f32 %.3g rad, snorm16 %.3g deg, snorm8 %.3g deg",
              (f64)worst_f32, (f64)nph_f32_degrees(worst_16), (f64)nph_f32_degrees(worst_8));
    NTEST_ASSERT_TRUE(worst_f32 < 2e-6F);
    NTEST_ASSERT_TRUE(worst_16 < nph_f32_radians(0.006F));
    NTEST_ASSERT_TRUE(worst_8 < nph_f32_radians(1.0F)); // Cigolle et al.: oct16 max 0.95 deg
}

// ---------------------------------------------------------------------------
// Packed HDR
// ---------------------------------------------------------------------------

NTEST(hdr, r11g11b10f)
{
    // Exactly representable values round-trip; red sits in the low bits.
    const NphVec3 v = nph_vec3(1.0F, 0.5F, 1024.0F);
    const u32 p = nph_pack_r11g11b10f(v);
    TM_ASSERT_VEC3_NEAR(nph_unpack_r11g11b10f(p), v, 0.0F);
    NTEST_ASSERT_EQ_UINT(p & 0x7FFU, 0x3C0U); // 1.0 as 5e6m
    NTEST_ASSERT_EQ_UINT(nph_pack_r11g11b10f(nph_vec3(0.0F, 1.0F, 0.0F)), 0x3C0U << 11U);
    NTEST_ASSERT_EQ_UINT(nph_pack_r11g11b10f(nph_vec3(0.0F, 0.0F, 1.0F)),
                         0x1E0U << 22U); // 1.0 as 5e5m
    NTEST_ASSERT_EQ_UINT(nph_pack_r11g11b10f(nph_vec3_zero()), 0U);
    // Negatives clamp to zero, infinities and NaN survive, overflow saturates to inf.
    NTEST_ASSERT_EQ_UINT(nph_pack_r11g11b10f(nph_vec3(-1.0F, -1e-9F, -0.0F)), 0U);
    const NphVec3 inf =
        nph_unpack_r11g11b10f(nph_pack_r11g11b10f(nph_vec3(NPH_F32_INFINITY, 1e9F, 70000.0F)));
    NTEST_ASSERT_TRUE(nph_f32_is_inf(inf.x) && nph_f32_is_inf(inf.y) && nph_f32_is_inf(inf.z));
    const NphVec3 nan = nph_unpack_r11g11b10f(nph_pack_r11g11b10f(
        nph_vec3(nph_f32_from_bits(0x7FC00000U), 1.0F, nph_f32_from_bits(0xFFC00000U))));
    NTEST_ASSERT_TRUE(nph_f32_is_nan(nan.x) && nan.y == 1.0F && nph_f32_is_nan(nan.z));
    // 65504 is above the largest 6-bit-mantissa value (65024) and nearer to 2^16: infinity.
    NTEST_ASSERT_TRUE(nph_f32_is_inf(
        nph_unpack_r11g11b10f(nph_pack_r11g11b10f(nph_vec3(65504.0F, 0.0F, 0.0F))).x));
    NTEST_ASSERT_ULP_F32(
        nph_unpack_r11g11b10f(nph_pack_r11g11b10f(nph_vec3(65024.0F, 0.0F, 0.0F))).x, 65024.0F, 0U);
    // Relative error within half an ulp of the 6-bit (red, green) and 5-bit (blue) mantissas.
    u64 rng = 53U;
    for (u32 i = 0; i < 100000U; ++i) {
        const NphVec3 c = nph_vec3(nph_f32_exp2(tm_rng_range(&rng, -13.0F, 15.5F)),
                                   nph_f32_exp2(tm_rng_range(&rng, -13.0F, 15.5F)),
                                   nph_f32_exp2(tm_rng_range(&rng, -13.0F, 15.5F)));
        const NphVec3 d = nph_unpack_r11g11b10f(nph_pack_r11g11b10f(c));
        NTEST_ASSERT_TRUE(nph_f32_abs(d.x - c.x) <= c.x * (1.0F / 128.0F) + 1e-7F);
        NTEST_ASSERT_TRUE(nph_f32_abs(d.y - c.y) <= c.y * (1.0F / 128.0F) + 1e-7F);
        NTEST_ASSERT_TRUE(nph_f32_abs(d.z - c.z) <= c.z * (1.0F / 64.0F) + 1e-7F);
    }
    // Monotonic in each channel.
    u32 prev = 0U;
    for (u32 i = 1; i <= 2000U; ++i) {
        const u32 r = nph_pack_r11g11b10f(nph_vec3((f32)i * 0.37F, 0.0F, 0.0F)) & 0x7FFU;
        NTEST_ASSERT_TRUE(r >= prev);
        prev = r;
    }
}

NTEST(hdr, rgb9e5)
{
    // (1, 0.5, 0.25): shared exponent 16, mantissas 256, 128, 64.
    const u32 p = nph_pack_rgb9e5(nph_vec3(1.0F, 0.5F, 0.25F));
    NTEST_ASSERT_EQ_UINT(p >> 27U, 16U);
    NTEST_ASSERT_EQ_UINT(p & 0x1FFU, 256U);
    NTEST_ASSERT_EQ_UINT((p >> 9U) & 0x1FFU, 128U);
    NTEST_ASSERT_EQ_UINT((p >> 18U) & 0x1FFU, 64U);
    TM_ASSERT_VEC3_NEAR(nph_unpack_rgb9e5(p), nph_vec3(1.0F, 0.5F, 0.25F), 0.0F);
    NTEST_ASSERT_EQ_UINT(nph_pack_rgb9e5(nph_vec3_zero()), 0U);
    NTEST_ASSERT_EQ_UINT(nph_pack_rgb9e5(nph_vec3(-1.0F, -2.0F, -3.0F)), 0U);
    NTEST_ASSERT_EQ_UINT(nph_pack_rgb9e5(nph_vec3(nph_f32_from_bits(0x7FC00000U), 0.0F, 0.0F)), 0U);
    // Saturation at the format maximum with the top exponent.
    const u32 big = nph_pack_rgb9e5(nph_vec3(1e9F, 0.0F, NPH_RGB9E5_MAX));
    NTEST_ASSERT_EQ_UINT(big >> 27U, 31U);
    NTEST_ASSERT_EQ_UINT(big & 0x1FFU, 511U);
    TM_ASSERT_VEC3_NEAR(nph_unpack_rgb9e5(big), nph_vec3(NPH_RGB9E5_MAX, 0.0F, NPH_RGB9E5_MAX),
                        0.0F);
    // The mantissa overflow case: 511.6/512 * 2^e rounds to 512 and bumps the exponent.
    const u32 bump = nph_pack_rgb9e5(nph_vec3(0.9995F, 0.0F, 0.0F));
    NTEST_ASSERT_EQ_UINT(bump >> 27U, 16U);
    NTEST_ASSERT_EQ_UINT(bump & 0x1FFU, 256U);
    // Small values use the lowest exponent (denormal-like, no implicit bit).
    const u32 tiny = nph_pack_rgb9e5(nph_vec3(1e-6F, 0.0F, 0.0F));
    NTEST_ASSERT_EQ_UINT(tiny >> 27U, 0U);
    NTEST_ASSERT_NEAR_F32(nph_unpack_rgb9e5(tiny).x, 1e-6F, 0.5F / 16777216.0F);
    // The absolute error is bounded by half a step of the shared exponent.
    u64 rng = 54U;
    for (u32 i = 0; i < 100000U; ++i) {
        const NphVec3 c = nph_vec3(nph_f32_exp2(tm_rng_range(&rng, -12.0F, 15.9F)),
                                   nph_f32_exp2(tm_rng_range(&rng, -12.0F, 15.9F)),
                                   nph_f32_exp2(tm_rng_range(&rng, -12.0F, 15.9F)));
        const NphVec3 d = nph_unpack_rgb9e5(nph_pack_rgb9e5(c));
        const f32 max_c = nph_vec3_max_component(c);
        const f32 step =
            nph_f32_max(max_c / 256.0F, 1.0F / 16777216.0F); // 2^(floor(log2 max)+1-9) <= max/256
        NTEST_ASSERT_TRUE(nph_f32_abs(d.x - c.x) <= 0.5F * step + 1e-9F);
        NTEST_ASSERT_TRUE(nph_f32_abs(d.y - c.y) <= 0.5F * step + 1e-9F);
        NTEST_ASSERT_TRUE(nph_f32_abs(d.z - c.z) <= 0.5F * step + 1e-9F);
    }
}
