// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/scalar.h>

#include "test_math_support.h"

NTEST(scalar, constants_are_consistent)
{
    NTEST_ASSERT_ULP_F32(NPH_TAU, 2.0F * NPH_PI, 1U);
    NTEST_ASSERT_ULP_F32(NPH_HALF_PI, 0.5F * NPH_PI, 1U);
    NTEST_ASSERT_ULP_F32(NPH_QUARTER_PI, 0.25F * NPH_PI, 1U);
    NTEST_ASSERT_ULP_F32(NPH_INV_PI * NPH_PI, 1.0F, 2U);
    NTEST_ASSERT_ULP_F32(NPH_INV_TAU * NPH_TAU, 1.0F, 2U);
    NTEST_ASSERT_ULP_F32(NPH_SQRT2 * NPH_SQRT2, 2.0F, 2U);
    NTEST_ASSERT_ULP_F32(NPH_INV_SQRT2 * NPH_SQRT2, 1.0F, 2U);
    NTEST_ASSERT_ULP_F32(NPH_SQRT3 * NPH_SQRT3, 3.0F, 2U);
    NTEST_ASSERT_ULP_F32(nph_f32_exp(1.0F), NPH_E, 2U);
    NTEST_ASSERT_ULP_F32(nph_f32_log(2.0F), NPH_LN2, 2U);
    NTEST_ASSERT_ULP_F32(NPH_DEG_TO_RAD * NPH_RAD_TO_DEG, 1.0F, 2U);
    NTEST_ASSERT_NEAR_F64(NPH_F64_DEG_TO_RAD * 180.0, NPH_F64_PI, 1e-15);
    NTEST_ASSERT_NEAR_F64(NPH_F64_RAD_TO_DEG * NPH_F64_TAU, 360.0, 1e-12);
    NTEST_ASSERT_TRUE(1.0F + NPH_F32_EPSILON != 1.0F);
    NTEST_ASSERT_TRUE(1.0F + NPH_F32_EPSILON * 0.5F == 1.0F);
    NTEST_ASSERT_TRUE(nph_f32_is_inf(NPH_F32_INFINITY));
    NTEST_ASSERT_TRUE(nph_f32_is_finite(NPH_F32_MAX));
    NTEST_ASSERT_TRUE(NPH_F32_MIN_NORMAL > 0.0F);
    NTEST_ASSERT_TRUE(nph_f64_is_finite(NPH_F64_MAX));
    NTEST_ASSERT_FALSE(nph_f64_is_finite(NPH_F64_INFINITY));
    NTEST_ASSERT_TRUE(1.0 + NPH_F64_EPSILON != 1.0);
}

NTEST(scalar, min_max_clamp)
{
    NTEST_ASSERT_EQ_INT((int)nph_f32_min(1.0F, 2.0F), 1);
    NTEST_ASSERT_EQ_INT((int)nph_f32_max(1.0F, 2.0F), 2);
    NTEST_ASSERT_ULP_F32(nph_f32_min3(3.0F, -1.0F, 2.0F), -1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_max3(3.0F, -1.0F, 2.0F), 3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_clamp(5.0F, 0.0F, 1.0F), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_clamp(-5.0F, 0.0F, 1.0F), 0.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_clamp(0.25F, 0.0F, 1.0F), 0.25F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_saturate(1.5F), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_saturate(-0.5F), 0.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_saturate(0.5F), 0.5F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_abs(-3.0F), 3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_square(-3.0F), 9.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_sign(-0.1F), -1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_sign(0.1F), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_sign(0.0F), 0.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_sign(-0.0F), 0.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_copysign(2.0F, -0.0F), -2.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_copysign(-2.0F, 1.0F), 2.0F, 0U);

    NTEST_ASSERT_NEAR_F64(nph_f64_min(1.0, 2.0), 1.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_max(1.0, 2.0), 2.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_clamp(5.0, 0.0, 1.0), 1.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_clamp(-5.0, 0.0, 1.0), 0.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_clamp(0.5, 0.0, 1.0), 0.5, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_abs(-2.5), 2.5, 0.0);
}

NTEST(scalar, lerp_is_exact_at_endpoints)
{
    // The (1 - t) a + t b form returns exactly a and b at t == 0 and 1 even
    // when a + (b - a) t would not.
    const f32 a = 0.1F;
    const f32 b = 1e8F;
    NTEST_ASSERT_ULP_F32(nph_f32_lerp(a, b, 0.0F), a, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_lerp(a, b, 1.0F), b, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_lerp(2.0F, 4.0F, 0.5F), 3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_inverse_lerp(2.0F, 4.0F, 3.0F), 0.5F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_remap(5.0F, 0.0F, 10.0F, 100.0F, 200.0F), 150.0F, 1U);
    NTEST_ASSERT_NEAR_F64(nph_f64_lerp(2.0, 4.0, 0.25), 2.5, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_lerp(0.1, 1e16, 1.0), 1e16, 0.0);
}

NTEST(scalar, smoothstep_and_step)
{
    NTEST_ASSERT_ULP_F32(nph_f32_smoothstep(0.0F, 1.0F, -1.0F), 0.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_smoothstep(0.0F, 1.0F, 2.0F), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_smoothstep(0.0F, 1.0F, 0.5F), 0.5F, 0U);
    NTEST_ASSERT_NEAR_F32(nph_f32_smoothstep(2.0F, 4.0F, 2.5F), 0.15625F, 1e-7F);
    // Monotonic across the ramp.
    f32 prev = -1.0F;
    for (u32 i = 0; i <= 100U; ++i) {
        const f32 v = nph_f32_smoothstep(0.0F, 1.0F, (f32)i / 100.0F);
        NTEST_ASSERT_TRUE(v >= prev);
        prev = v;
    }
    NTEST_ASSERT_ULP_F32(nph_f32_step(1.0F, 0.5F), 0.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_step(1.0F, 1.0F), 1.0F, 0U);
}

NTEST(scalar, rounding)
{
    NTEST_ASSERT_ULP_F32(nph_f32_floor(-1.5F), -2.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_ceil(-1.5F), -1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_round(2.5F), 3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_round(-2.5F), -3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_round_even(2.5F), 2.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_round_even(3.5F), 4.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_round_even(-2.5F), -2.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_trunc(-1.7F), -1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_trunc(1.7F), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_fract(1.25F), 0.25F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_fract(-1.25F), 0.75F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_mod(5.5F, 2.0F), 1.5F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_mod(-5.5F, 2.0F), -1.5F, 0U);
    NTEST_ASSERT_NEAR_F64(nph_f64_floor(-1.5), -2.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_ceil(-1.5), -1.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_f64_round(2.5), 3.0, 0.0);
}

NTEST(scalar, wrap)
{
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap(5.0F, 0.0F, 4.0F), 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap(-1.0F, 0.0F, 4.0F), 3.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap(4.0F, 0.0F, 4.0F), 0.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap(2.0F, 0.0F, 4.0F), 2.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap_angle(NPH_PI + 0.5F), -NPH_PI + 0.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap_angle(-NPH_PI - 0.5F), NPH_PI - 0.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap_angle(3.0F * NPH_TAU + 0.25F), 0.25F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(nph_f32_wrap_angle(0.25F), 0.25F, 0.0F);
    // Always inside [-pi, pi).
    u64 rng = 1U;
    for (u32 i = 0; i < 1000U; ++i) {
        const f32 w = nph_f32_wrap_angle(tm_rng_range(&rng, -100.0F, 100.0F));
        NTEST_ASSERT_TRUE(w >= -NPH_PI && w < NPH_PI);
    }
}

NTEST(scalar, roots_powers_logs)
{
    NTEST_ASSERT_ULP_F32(nph_f32_sqrt(16.0F), 4.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_rsqrt(16.0F), 0.25F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_pow(2.0F, 10.0F), 1024.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_exp2(3.0F), 8.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_log2(8.0F), 3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_exp(0.0F), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32_log(1.0F), 0.0F, 0U);
    NTEST_ASSERT_NEAR_F64(nph_f64_sqrt(2.0), 1.4142135623730951, 1e-16);
}

NTEST(scalar, trigonometry)
{
    NTEST_ASSERT_NEAR_F32(nph_f32_sin(NPH_HALF_PI), 1.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(nph_f32_cos(NPH_PI), -1.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(nph_f32_tan(NPH_QUARTER_PI), 1.0F, 1e-6F);
    f32 s;
    f32 c;
    nph_f32_sincos(1.0F, &s, &c);
    NTEST_ASSERT_ULP_F32(s, nph_f32_sin(1.0F), 0U);
    NTEST_ASSERT_ULP_F32(c, nph_f32_cos(1.0F), 0U);
    NTEST_ASSERT_NEAR_F32(nph_f32_asin(1.0F), NPH_HALF_PI, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_acos(-1.0F), NPH_PI, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_atan(1.0F), NPH_QUARTER_PI, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_atan2(1.0F, -1.0F), 3.0F * NPH_QUARTER_PI, 1e-6F);
    NTEST_ASSERT_NEAR_F64(nph_f64_sin(NPH_F64_PI / 2.0), 1.0, 1e-15);
    NTEST_ASSERT_NEAR_F64(nph_f64_cos(NPH_F64_PI), -1.0, 1e-15);
    NTEST_ASSERT_NEAR_F64(nph_f64_atan2(1.0, 1.0), NPH_F64_PI / 4.0, 1e-15);
    // Rounding noise beyond [-1, 1] must not produce NaN.
    NTEST_ASSERT_FALSE(nph_f32_is_nan(nph_f32_asin(1.0000002F)));
    NTEST_ASSERT_FALSE(nph_f32_is_nan(nph_f32_acos(-1.0000002F)));
    NTEST_ASSERT_NEAR_F32(nph_f32_acos(1.0000002F), 0.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(nph_f32_radians(180.0F), NPH_PI, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_f32_degrees(NPH_HALF_PI), 90.0F, 1e-4F);
}

NTEST(scalar, classification_and_bits)
{
    NTEST_ASSERT_TRUE(nph_f32_is_nan(nph_f32_from_bits(0x7FC00000U)));
    NTEST_ASSERT_FALSE(nph_f32_is_nan(1.0F));
    NTEST_ASSERT_TRUE(nph_f32_is_finite(1.0F));
    NTEST_ASSERT_FALSE(nph_f32_is_finite(NPH_F32_INFINITY));
    NTEST_ASSERT_FALSE(nph_f32_is_finite(nph_f32_from_bits(0x7FC00000U)));
    NTEST_ASSERT_TRUE(nph_f32_is_inf(-NPH_F32_INFINITY));
    NTEST_ASSERT_FALSE(nph_f32_is_inf(NPH_F32_MAX));
    NTEST_ASSERT_EQ_UINT(nph_f32_to_bits(1.0F), 0x3F800000U);
    NTEST_ASSERT_EQ_UINT(nph_f32_to_bits(-2.0F), 0xC0000000U);
    NTEST_ASSERT_ULP_F32(nph_f32_from_bits(0x3F800000U), 1.0F, 0U);
    NTEST_ASSERT_EQ_UINT(nph_f64_to_bits(1.0), 0x3FF0000000000000ULL);
    NTEST_ASSERT_NEAR_F64(nph_f64_from_bits(0x4000000000000000ULL), 2.0, 0.0);
}

NTEST(scalar, ulp_distance)
{
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(1.0F, 1.0F), 0U);
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(0.0F, -0.0F), 0U);
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(1.0F, nph_f32_from_bits(0x3F800001U)), 1U);
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(nph_f32_from_bits(0x3F800001U), 1.0F), 1U);
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(1.0F, 1.0F + NPH_F32_EPSILON), 1U);
    // Across zero: the smallest positive and negative denormals are 2 apart.
    NTEST_ASSERT_EQ_UINT(
        nph_f32_ulp_distance(nph_f32_from_bits(1U), nph_f32_from_bits(0x80000001U)), 2U);
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(-1.0F, 1.0F), 2U * 0x3F800000U);
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(1.0F, nph_f32_from_bits(0x7FC00000U)), NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(nph_f32_ulp_distance(-NPH_F32_MAX, NPH_F32_MAX), 2U * 0x7F7FFFFFU);
}

NTEST(scalar, approximate_comparison)
{
    NTEST_ASSERT_TRUE(nph_f32_approx_eq(1.0F, 1.0F, 0.0F, 0.0F));
    NTEST_ASSERT_TRUE(nph_f32_approx_eq(1.0F, 1.0F + 1e-7F, 1e-6F, 0.0F));
    NTEST_ASSERT_TRUE(nph_f32_approx_eq(1e6F, 1e6F + 1.0F, 0.0F, 1e-5F));
    NTEST_ASSERT_FALSE(nph_f32_approx_eq(1e6F, 1e6F + 100.0F, 0.0F, 1e-5F));
    NTEST_ASSERT_FALSE(nph_f32_approx_eq(1.0F, 2.0F, 0.5F, 0.1F));
    NTEST_ASSERT_TRUE(nph_f32_approx_eq(NPH_F32_INFINITY, NPH_F32_INFINITY, 0.0F, 0.0F));
    NTEST_ASSERT_FALSE(nph_f32_approx_eq(nph_f32_from_bits(0x7FC00000U), 1.0F, 1.0F, 1.0F));
    NTEST_ASSERT_TRUE(nph_f32_near(1.0F, 1.5F, 0.5F));
    NTEST_ASSERT_FALSE(nph_f32_near(1.0F, 1.5F, 0.4F));
    NTEST_ASSERT_TRUE(nph_f32_near(NPH_F32_INFINITY, NPH_F32_INFINITY, 0.0F));
    NTEST_ASSERT_TRUE(nph_f32_near_zero(-1e-8F, 1e-7F));
    NTEST_ASSERT_FALSE(nph_f32_near_zero(1e-6F, 1e-7F));
    NTEST_ASSERT_TRUE(nph_f64_near(1.0, 1.0 + 1e-12, 1e-11));
    NTEST_ASSERT_FALSE(nph_f64_near(1.0, 1.1, 1e-11));
}

NTEST(scalar, integer_helpers)
{
    NTEST_ASSERT_EQ_INT(nph_i32_abs(-5), 5);
    NTEST_ASSERT_EQ_INT(nph_i32_abs(5), 5);
    NTEST_ASSERT_EQ_INT(nph_i32_sign(-5), -1);
    NTEST_ASSERT_EQ_INT(nph_i32_sign(0), 0);
    NTEST_ASSERT_EQ_INT(nph_i32_sign(9), 1);
    NTEST_ASSERT_EQ_INT(nph_i32_div_floor(7, 2), 3);
    NTEST_ASSERT_EQ_INT(nph_i32_div_floor(-7, 2), -4);
    NTEST_ASSERT_EQ_INT(nph_i32_div_floor(7, -2), -4);
    NTEST_ASSERT_EQ_INT(nph_i32_div_floor(-7, -2), 3);
    NTEST_ASSERT_EQ_INT(nph_i32_div_floor(-8, 2), -4);
    NTEST_ASSERT_EQ_INT(nph_i32_mod_floor(7, 3), 1);
    NTEST_ASSERT_EQ_INT(nph_i32_mod_floor(-7, 3), 2);
    NTEST_ASSERT_EQ_INT(nph_i32_mod_floor(7, -3), -2);
    NTEST_ASSERT_EQ_INT(nph_i32_mod_floor(-9, 3), 0);
    // floor_div and mod_floor agree with a * q + r == a across signs.
    for (i32 a = -20; a <= 20; ++a) {
        for (i32 b = -5; b <= 5; ++b) {
            if (b == 0) {
                continue;
            }
            const i32 q = nph_i32_div_floor(a, b);
            const i32 r = nph_i32_mod_floor(a, b);
            NTEST_ASSERT_EQ_INT(q * b + r, a);
            NTEST_ASSERT_TRUE(b > 0 ? (r >= 0 && r < b) : (r <= 0 && r > b));
        }
    }
    NTEST_ASSERT_EQ_UINT(nph_u32_div_ceil(0U, 4U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_u32_div_ceil(1U, 4U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_u32_div_ceil(4U, 4U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_u32_div_ceil(5U, 4U), 2U);
    NTEST_ASSERT_EQ_UINT(nph_u32_div_ceil(NPH_U32_MAX, 2U), 0x80000000U);
    NTEST_ASSERT_EQ_UINT(nph_u64_div_ceil(0U, 3U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_u64_div_ceil(7U, 3U), 3U);
    NTEST_ASSERT_EQ_UINT(nph_u64_div_ceil(NPH_U64_MAX, 2U), 0x8000000000000000ULL);
}
