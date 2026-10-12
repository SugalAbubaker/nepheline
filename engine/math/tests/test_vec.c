// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/vec.h>

#include "test_math_support.h"

#define ITER 2000U

// ---------------------------------------------------------------------------
// NphVec2
// ---------------------------------------------------------------------------

NTEST(vec2, constructors_and_arithmetic)
{
    const NphVec2 a = nph_vec2(1.0F, 2.0F);
    const NphVec2 b = nph_vec2(4.0F, -6.0F);
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_splat(3.0F), nph_vec2(3.0F, 3.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_zero(), nph_vec2(0.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_one(), nph_vec2(1.0F, 1.0F)));
    const f32 arr[2] = {7.0F, 8.0F};
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_from_array(arr), nph_vec2(7.0F, 8.0F)));
    f32 out[2];
    nph_vec2_to_array(a, out);
    NTEST_ASSERT_ULP_F32(out[0], 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(out[1], 2.0F, 0U);
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_add(a, b), nph_vec2(5.0F, -4.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_sub(a, b), nph_vec2(-3.0F, 8.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_mul(a, b), nph_vec2(4.0F, -12.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_div(b, a), nph_vec2(4.0F, -3.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_scale(a, 2.0F), nph_vec2(2.0F, 4.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_neg(a), nph_vec2(-1.0F, -2.0F)));
    NTEST_ASSERT_ULP_F32(nph_vec2_dot(a, b), -8.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec2_cross(nph_vec2(1.0F, 0.0F), nph_vec2(0.0F, 1.0F)), 1.0F, 0U);
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_perp(nph_vec2(1.0F, 0.0F)), nph_vec2(0.0F, 1.0F)));
    NTEST_ASSERT_ULP_F32(nph_vec2_length_sq(nph_vec2(3.0F, 4.0F)), 25.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec2_length(nph_vec2(3.0F, 4.0F)), 5.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec2_distance_sq(a, nph_vec2(4.0F, 6.0F)), 25.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec2_distance(a, nph_vec2(4.0F, 6.0F)), 5.0F, 0U);
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_lerp(a, b, 0.0F), a));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_lerp(a, b, 1.0F), b));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_min(a, b), nph_vec2(1.0F, -6.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_max(a, b), nph_vec2(4.0F, 2.0F)));
    NTEST_ASSERT_TRUE(
        nph_vec2_eq(nph_vec2_clamp(b, nph_vec2_zero(), nph_vec2_one()), nph_vec2(1.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_abs(b), nph_vec2(4.0F, 6.0F)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_floor(nph_vec2(1.5F, -1.5F)), nph_vec2(1.0F, -2.0F)));
    NTEST_ASSERT_ULP_F32(nph_vec2_min_component(b), -6.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec2_max_component(b), 4.0F, 0U);
    NTEST_ASSERT_TRUE(nph_vec2_near(a, nph_vec2(1.0F + 1e-7F, 2.0F), 1e-6F));
    NTEST_ASSERT_FALSE(nph_vec2_near(a, nph_vec2(1.1F, 2.0F), 1e-6F));
    NTEST_ASSERT_FALSE(nph_vec2_eq(a, b));
}

NTEST(vec2, normalize)
{
    const NphVec2 n = nph_vec2_normalize(nph_vec2(3.0F, 4.0F));
    TM_ASSERT_VEC2_NEAR(n, nph_vec2(0.6F, 0.8F), 1e-7F);
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_normalize_or_zero(nph_vec2_zero()), nph_vec2_zero()));
    TM_ASSERT_VEC2_NEAR(nph_vec2_normalize_or_zero(nph_vec2(0.0F, -2.0F)), nph_vec2(0.0F, -1.0F),
                        0.0F);
    NphVec2 r = nph_vec2_one();
    TM_EXPECT_ASSERT(r = nph_vec2_normalize(nph_vec2_zero()));
    NPH_UNUSED(r);
    u64 rng = 2U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec2 v = tm_rng_vec2(&rng, -100.0F, 100.0F);
        if (nph_vec2_length_sq(v) == 0.0F) {
            continue;
        }
        NTEST_ASSERT_ULP_F32(nph_vec2_length(nph_vec2_normalize(v)), 1.0F, 2U);
    }
}

// ---------------------------------------------------------------------------
// NphVec3
// ---------------------------------------------------------------------------

NTEST(vec3, constructors_and_accessors)
{
    const NphVec3 a = nph_vec3(1.0F, 2.0F, 3.0F);
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_splat(2.0F), nph_vec3(2.0F, 2.0F, 2.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_zero(), nph_vec3(0.0F, 0.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_one(), nph_vec3(1.0F, 1.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_unit_x(), nph_vec3(1.0F, 0.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_unit_y(), nph_vec3(0.0F, 1.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_unit_z(), nph_vec3(0.0F, 0.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_from_vec2(nph_vec2(1.0F, 2.0F), 3.0F), a));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec3_xy(a), nph_vec2(1.0F, 2.0F)));
    const f32 arr[3] = {1.0F, 2.0F, 3.0F};
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_from_array(arr), a));
    f32 out[3] = {0.0F, 0.0F, 0.0F};
    nph_vec3_to_array(a, out);
    NTEST_ASSERT_EQ_MEM(out, arr, sizeof arr);
    NTEST_ASSERT_ULP_F32(nph_vec3_at(a, 0U), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_at(a, 1U), 2.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_at(a, 2U), 3.0F, 0U);
}

NTEST(vec3, arithmetic)
{
    const NphVec3 a = nph_vec3(1.0F, 2.0F, 3.0F);
    const NphVec3 b = nph_vec3(4.0F, -6.0F, 0.5F);
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_add(a, b), nph_vec3(5.0F, -4.0F, 3.5F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_sub(a, b), nph_vec3(-3.0F, 8.0F, 2.5F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_mul(a, b), nph_vec3(4.0F, -12.0F, 1.5F)));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_div(a, nph_vec3(2.0F, 4.0F, 3.0F)), nph_vec3(0.5F, 0.5F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_scale(a, -2.0F), nph_vec3(-2.0F, -4.0F, -6.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_mul_add(a, b, 2.0F), nph_vec3(9.0F, -10.0F, 4.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_neg(a), nph_vec3(-1.0F, -2.0F, -3.0F)));
    NTEST_ASSERT_ULP_F32(nph_vec3_dot(a, b), -6.5F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_length_sq(nph_vec3(1.0F, 2.0F, 2.0F)), 9.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_length(nph_vec3(1.0F, 2.0F, 2.0F)), 3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_distance_sq(a, nph_vec3(2.0F, 4.0F, 5.0F)), 9.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_distance(a, nph_vec3(2.0F, 4.0F, 5.0F)), 3.0F, 0U);
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_lerp(a, b, 0.0F), a));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_lerp(a, b, 1.0F), b));
    TM_ASSERT_VEC3_NEAR(nph_vec3_lerp(a, b, 0.5F), nph_vec3(2.5F, -2.0F, 1.75F), 1e-7F);
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_min(a, b), nph_vec3(1.0F, -6.0F, 0.5F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_max(a, b), nph_vec3(4.0F, 2.0F, 3.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_clamp(b, nph_vec3_zero(), nph_vec3_one()),
                                  nph_vec3(1.0F, 0.0F, 0.5F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_abs(b), nph_vec3(4.0F, 6.0F, 0.5F)));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_floor(nph_vec3(1.5F, -1.5F, 2.0F)), nph_vec3(1.0F, -2.0F, 2.0F)));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_ceil(nph_vec3(1.5F, -1.5F, 2.0F)), nph_vec3(2.0F, -1.0F, 2.0F)));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_round(nph_vec3(1.5F, -1.5F, 2.4F)), nph_vec3(2.0F, -2.0F, 2.0F)));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_sign(nph_vec3(-3.0F, 0.0F, 2.0F)), nph_vec3(-1.0F, 0.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_reciprocal(nph_vec3(2.0F, 4.0F, -0.5F)),
                                  nph_vec3(0.5F, 0.25F, -2.0F)));
    NTEST_ASSERT_ULP_F32(nph_vec3_hadd(a), 6.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_min_component(b), -6.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec3_max_component(b), 4.0F, 0U);
    NTEST_ASSERT_EQ_UINT(nph_vec3_max_index(nph_vec3(1.0F, 3.0F, 2.0F)), 1U);
    NTEST_ASSERT_EQ_UINT(nph_vec3_max_index(nph_vec3(1.0F, 2.0F, 3.0F)), 2U);
    NTEST_ASSERT_EQ_UINT(nph_vec3_max_index(nph_vec3(3.0F, 2.0F, 1.0F)), 0U);
    NTEST_ASSERT_EQ_UINT(nph_vec3_max_index(nph_vec3(3.0F, 1.0F, 3.0F)), 0U); // tie -> lowest
    NTEST_ASSERT_EQ_UINT(nph_vec3_max_index(nph_vec3(1.0F, 3.0F, 3.0F)), 1U);
    NTEST_ASSERT_EQ_UINT(nph_vec3_max_abs_index(nph_vec3(1.0F, -5.0F, 2.0F)), 1U);
    NTEST_ASSERT_TRUE(nph_vec3_near(a, nph_vec3(1.0F, 2.0F, 3.0F + 1e-7F), 1e-6F));
    NTEST_ASSERT_FALSE(nph_vec3_near(a, b, 1e-6F));
    NTEST_ASSERT_FALSE(nph_vec3_eq(a, b));
    NTEST_ASSERT_TRUE(nph_vec3_is_finite(a));
    NTEST_ASSERT_FALSE(nph_vec3_is_finite(nph_vec3(1.0F, NPH_F32_INFINITY, 0.0F)));
}

NTEST(vec3, cross_product_identities)
{
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_cross(nph_vec3_unit_x(), nph_vec3_unit_y()), nph_vec3_unit_z()));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_cross(nph_vec3_unit_y(), nph_vec3_unit_z()), nph_vec3_unit_x()));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_cross(nph_vec3_unit_z(), nph_vec3_unit_x()), nph_vec3_unit_y()));
    u64 rng = 3U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 a = tm_rng_vec3(&rng, -2.0F, 2.0F);
        const NphVec3 b = tm_rng_vec3(&rng, -2.0F, 2.0F);
        const NphVec3 c = nph_vec3_cross(a, b);
        // Perpendicular to both operands and anti-commutative.
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(c, a), 0.0F, 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(c, b), 0.0F, 1e-5F);
        TM_ASSERT_VEC3_NEAR(nph_vec3_cross(b, a), nph_vec3_neg(c), 0.0F);
        // Lagrange: |a x b|^2 == |a|^2 |b|^2 - (a.b)^2
        const f32 lhs = nph_vec3_length_sq(c);
        const f32 rhs =
            nph_vec3_length_sq(a) * nph_vec3_length_sq(b) - nph_f32_square(nph_vec3_dot(a, b));
        NTEST_ASSERT_TRUE(nph_f32_approx_eq(lhs, rhs, 1e-4F, 1e-5F));
    }
}

NTEST(vec3, normalize)
{
    TM_ASSERT_VEC3_NEAR(nph_vec3_normalize(nph_vec3(0.0F, 3.0F, 4.0F)), nph_vec3(0.0F, 0.6F, 0.8F),
                        1e-7F);
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_normalize_or_zero(nph_vec3_zero()), nph_vec3_zero()));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_vec3_normalize_or(nph_vec3_zero(), nph_vec3_unit_y()), nph_vec3_unit_y()));
    TM_ASSERT_VEC3_NEAR(nph_vec3_normalize_or(nph_vec3(2.0F, 0.0F, 0.0F), nph_vec3_unit_y()),
                        nph_vec3_unit_x(), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_vec3_normalize_or_zero(nph_vec3(0.0F, 0.0F, -5.0F)),
                        nph_vec3(0.0F, 0.0F, -1.0F), 0.0F);
    NphVec3 r = nph_vec3_one();
    TM_EXPECT_ASSERT(r = nph_vec3_normalize(nph_vec3_zero()));
    NPH_UNUSED(r);
    NTEST_ASSERT_TRUE(nph_vec3_is_normalized(nph_vec3_unit_x(), 1e-6F));
    NTEST_ASSERT_FALSE(nph_vec3_is_normalized(nph_vec3_splat(1.0F), 1e-6F));
    u64 rng = 4U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 v = tm_rng_vec3(&rng, -1000.0F, 1000.0F);
        if (nph_vec3_length_sq(v) == 0.0F) {
            continue;
        }
        const NphVec3 n = nph_vec3_normalize(v);
        NTEST_ASSERT_ULP_F32(nph_vec3_length(n), 1.0F, 2U);
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(n, 1e-6F));
        // Direction is preserved.
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(n, v), nph_vec3_length(v), nph_vec3_length(v) * 1e-6F);
    }
}

NTEST(vec3, project_reject_reflect_refract)
{
    const NphVec3 n = nph_vec3_unit_y();
    const NphVec3 v = nph_vec3(1.0F, -2.0F, 3.0F);
    TM_ASSERT_VEC3_NEAR(nph_vec3_project(v, n), nph_vec3(0.0F, -2.0F, 0.0F), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_vec3_reject(v, n), nph_vec3(1.0F, 0.0F, 3.0F), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_vec3_reflect(v, n), nph_vec3(1.0F, 2.0F, 3.0F), 0.0F);
    // Refraction with eta == 1 is the identity; eta < 1 bends toward the normal.
    const NphVec3 d = nph_vec3_normalize(nph_vec3(1.0F, -1.0F, 0.0F));
    TM_ASSERT_VEC3_NEAR(nph_vec3_refract(d, n, 1.0F), d, 1e-6F);
    const NphVec3 r = nph_vec3_refract(d, n, 1.0F / 1.5F);
    NTEST_ASSERT_TRUE(nph_vec3_is_normalized(r, 1e-5F));
    NTEST_ASSERT_TRUE(nph_f32_abs(r.x) < nph_f32_abs(d.x));
    NTEST_ASSERT_TRUE(r.y < 0.0F);
    // Total internal reflection: grazing exit from a dense medium.
    const NphVec3 grazing = nph_vec3_normalize(nph_vec3(0.99F, -0.1F, 0.0F));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_refract(grazing, n, 1.5F), nph_vec3_zero()));

    u64 rng = 5U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 un = tm_rng_unit_vec3(&rng);
        const NphVec3 w = tm_rng_vec3(&rng, -3.0F, 3.0F);
        const NphVec3 p = nph_vec3_project(w, un);
        const NphVec3 q = nph_vec3_reject(w, un);
        TM_ASSERT_VEC3_NEAR(nph_vec3_add(p, q), w, 1e-6F);
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(q, un), 0.0F, 1e-5F);
        const NphVec3 refl = nph_vec3_reflect(w, un);
        NTEST_ASSERT_NEAR_F32(nph_vec3_length(refl), nph_vec3_length(w), 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(refl, un), -nph_vec3_dot(w, un), 1e-5F);
    }
}

NTEST(vec3, angle)
{
    NTEST_ASSERT_NEAR_F32(nph_vec3_angle(nph_vec3_unit_x(), nph_vec3_unit_y()), NPH_HALF_PI, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_vec3_angle(nph_vec3_unit_x(), nph_vec3(-2.0F, 0.0F, 0.0F)), NPH_PI,
                          1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_vec3_angle(nph_vec3_splat(3.0F), nph_vec3_one()), 0.0F, 1e-3F);
    NTEST_ASSERT_NEAR_F32(nph_vec3_angle(nph_vec3(1.0F, 1.0F, 0.0F), nph_vec3_unit_x()),
                          NPH_QUARTER_PI, 1e-6F);
}

NTEST(vec3, orthonormal_bases)
{
    u64 rng = 6U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 n = tm_rng_unit_vec3(&rng);
        const NphVec3 p = nph_vec3_perpendicular(n);
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(p, n), 0.0F, 1e-6F);
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(p, 1e-6F));

        NphVec3 t;
        NphVec3 b;
        nph_vec3_orthonormal_basis(n, &t, &b);
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(t, 1e-5F));
        NTEST_ASSERT_TRUE(nph_vec3_is_normalized(b, 1e-5F));
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(t, n), 0.0F, 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(b, n), 0.0F, 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(t, b), 0.0F, 1e-5F);
        // Right-handed: t x b == n.
        TM_ASSERT_VEC3_NEAR(nph_vec3_cross(t, b), n, 1e-5F);
    }
    // The axis-aligned cases pick the expected axes.
    NphVec3 t;
    NphVec3 b;
    nph_vec3_orthonormal_basis(nph_vec3_unit_z(), &t, &b);
    TM_ASSERT_VEC3_NEAR(t, nph_vec3_unit_x(), 0.0F);
    TM_ASSERT_VEC3_NEAR(b, nph_vec3_unit_y(), 0.0F);
    nph_vec3_orthonormal_basis(nph_vec3(0.0F, 0.0F, -1.0F), &t, &b);
    TM_ASSERT_VEC3_NEAR(nph_vec3_cross(t, b), nph_vec3(0.0F, 0.0F, -1.0F), 1e-6F);
    NTEST_ASSERT_NEAR_F32(
        nph_vec3_dot(nph_vec3_perpendicular(nph_vec3_unit_x()), nph_vec3_unit_x()), 0.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(
        nph_vec3_dot(nph_vec3_perpendicular(nph_vec3_unit_y()), nph_vec3_unit_y()), 0.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(
        nph_vec3_dot(nph_vec3_perpendicular(nph_vec3_unit_z()), nph_vec3_unit_z()), 0.0F, 0.0F);
}

// ---------------------------------------------------------------------------
// NphVec4
// ---------------------------------------------------------------------------

NTEST(vec4, constructors_and_arithmetic)
{
    const NphVec4 a = nph_vec4(1.0F, 2.0F, 3.0F, 4.0F);
    const NphVec4 b = nph_vec4(-1.0F, 0.5F, 2.0F, -4.0F);
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_splat(2.0F), nph_vec4(2.0F, 2.0F, 2.0F, 2.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_zero(), nph_vec4(0.0F, 0.0F, 0.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_one(), nph_vec4(1.0F, 1.0F, 1.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_from_vec3(nph_vec3(1.0F, 2.0F, 3.0F), 4.0F), a));
    NTEST_ASSERT_TRUE(
        nph_vec4_eq(nph_vec4_point(nph_vec3(1.0F, 2.0F, 3.0F)), nph_vec4(1.0F, 2.0F, 3.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_direction(nph_vec3(1.0F, 2.0F, 3.0F)),
                                  nph_vec4(1.0F, 2.0F, 3.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec4_xyz(a), nph_vec3(1.0F, 2.0F, 3.0F)));
    const f32 arr[4] = {1.0F, 2.0F, 3.0F, 4.0F};
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_from_array(arr), a));
    f32 out[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    nph_vec4_to_array(a, out);
    NTEST_ASSERT_EQ_MEM(out, arr, sizeof arr);
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_ULP_F32(nph_vec4_at(a, i), arr[i], 0U);
    }
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_add(a, b), nph_vec4(0.0F, 2.5F, 5.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_sub(a, b), nph_vec4(2.0F, 1.5F, 1.0F, 8.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_mul(a, b), nph_vec4(-1.0F, 1.0F, 6.0F, -16.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_div(a, b), nph_vec4(-1.0F, 4.0F, 1.5F, -1.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_scale(a, 2.0F), nph_vec4(2.0F, 4.0F, 6.0F, 8.0F)));
    NTEST_ASSERT_TRUE(
        nph_vec4_eq(nph_vec4_mul_add(a, b, 2.0F), nph_vec4(-1.0F, 3.0F, 7.0F, -4.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_neg(a), nph_vec4(-1.0F, -2.0F, -3.0F, -4.0F)));
    NTEST_ASSERT_ULP_F32(nph_vec4_dot(a, b), -10.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec4_dot3(a, b), 6.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec4_length_sq(nph_vec4(1.0F, 1.0F, 1.0F, 1.0F)), 4.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec4_length(nph_vec4(1.0F, 1.0F, 1.0F, 1.0F)), 2.0F, 0U);
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_lerp(a, b, 0.0F), a));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_lerp(a, b, 1.0F), b));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_min(a, b), nph_vec4(-1.0F, 0.5F, 2.0F, -4.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_max(a, b), nph_vec4(1.0F, 2.0F, 3.0F, 4.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_clamp(b, nph_vec4_zero(), nph_vec4_one()),
                                  nph_vec4(0.0F, 0.5F, 1.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_abs(b), nph_vec4(1.0F, 0.5F, 2.0F, 4.0F)));
    NTEST_ASSERT_ULP_F32(nph_vec4_hadd(a), 10.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec4_min_component(b), -4.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_vec4_max_component(b), 2.0F, 0U);
    TM_ASSERT_VEC3_NEAR(nph_vec4_project(nph_vec4(2.0F, 4.0F, 6.0F, 2.0F)),
                        nph_vec3(1.0F, 2.0F, 3.0F), 0.0F);
    NTEST_ASSERT_TRUE(nph_vec4_near(a, nph_vec4(1.0F, 2.0F, 3.0F, 4.0F + 1e-7F), 1e-6F));
    NTEST_ASSERT_FALSE(nph_vec4_near(a, b, 1e-6F));
    NTEST_ASSERT_FALSE(nph_vec4_eq(a, b));
    NTEST_ASSERT_TRUE(nph_vec4_is_finite(a));
    NTEST_ASSERT_FALSE(nph_vec4_is_finite(nph_vec4(0.0F, 0.0F, 0.0F, NPH_F32_INFINITY)));
}

NTEST(vec4, normalize)
{
    TM_ASSERT_VEC4_NEAR(nph_vec4_normalize(nph_vec4(2.0F, 0.0F, 0.0F, 0.0F)),
                        nph_vec4(1.0F, 0.0F, 0.0F, 0.0F), 0.0F);
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_vec4_normalize_or_zero(nph_vec4_zero()), nph_vec4_zero()));
    TM_ASSERT_VEC4_NEAR(nph_vec4_normalize_or_zero(nph_vec4(0.0F, 0.0F, 0.0F, -3.0F)),
                        nph_vec4(0.0F, 0.0F, 0.0F, -1.0F), 0.0F);
    NphVec4 r = nph_vec4_one();
    TM_EXPECT_ASSERT(r = nph_vec4_normalize(nph_vec4_zero()));
    NPH_UNUSED(r);
    u64 rng = 7U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec4 v = tm_rng_vec4(&rng, -10.0F, 10.0F);
        if (nph_vec4_length_sq(v) == 0.0F) {
            continue;
        }
        // Four components accumulate about 3 ULPs through square, sum,
        // sqrt, divide, scale, and the final length.
        NTEST_ASSERT_ULP_F32(nph_vec4_length(nph_vec4_normalize(v)), 1.0F, 4U);
    }
}

// ---------------------------------------------------------------------------
// Integer vectors
// ---------------------------------------------------------------------------

NTEST(ivec, ivec2)
{
    const NphIVec2 a = nph_ivec2(3, -4);
    const NphIVec2 b = nph_ivec2(-1, 2);
    NTEST_ASSERT_TRUE(nph_ivec2_eq(nph_ivec2_add(a, b), nph_ivec2(2, -2)));
    NTEST_ASSERT_TRUE(nph_ivec2_eq(nph_ivec2_sub(a, b), nph_ivec2(4, -6)));
    NTEST_ASSERT_TRUE(nph_ivec2_eq(nph_ivec2_mul(a, b), nph_ivec2(-3, -8)));
    NTEST_ASSERT_TRUE(nph_ivec2_eq(nph_ivec2_scale(a, 2), nph_ivec2(6, -8)));
    NTEST_ASSERT_TRUE(nph_ivec2_eq(nph_ivec2_neg(a), nph_ivec2(-3, 4)));
    NTEST_ASSERT_TRUE(nph_ivec2_eq(nph_ivec2_min(a, b), nph_ivec2(-1, -4)));
    NTEST_ASSERT_TRUE(nph_ivec2_eq(nph_ivec2_max(a, b), nph_ivec2(3, 2)));
    NTEST_ASSERT_FALSE(nph_ivec2_eq(a, b));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_from_ivec2(a), nph_vec2(3.0F, -4.0F)));
    NTEST_ASSERT_TRUE(
        nph_ivec2_eq(nph_ivec2_from_vec2_trunc(nph_vec2(1.9F, -1.9F)), nph_ivec2(1, -1)));
    NTEST_ASSERT_TRUE(
        nph_ivec2_eq(nph_ivec2_from_vec2_floor(nph_vec2(1.9F, -1.1F)), nph_ivec2(1, -2)));
}

NTEST(ivec, ivec3)
{
    const NphIVec3 a = nph_ivec3(3, -4, 5);
    const NphIVec3 b = nph_ivec3(-1, 2, 2);
    NTEST_ASSERT_TRUE(nph_ivec3_eq(nph_ivec3_add(a, b), nph_ivec3(2, -2, 7)));
    NTEST_ASSERT_TRUE(nph_ivec3_eq(nph_ivec3_sub(a, b), nph_ivec3(4, -6, 3)));
    NTEST_ASSERT_TRUE(nph_ivec3_eq(nph_ivec3_mul(a, b), nph_ivec3(-3, -8, 10)));
    NTEST_ASSERT_TRUE(nph_ivec3_eq(nph_ivec3_scale(a, -1), nph_ivec3(-3, 4, -5)));
    NTEST_ASSERT_TRUE(nph_ivec3_eq(nph_ivec3_neg(a), nph_ivec3(-3, 4, -5)));
    NTEST_ASSERT_TRUE(nph_ivec3_eq(nph_ivec3_min(a, b), nph_ivec3(-1, -4, 2)));
    NTEST_ASSERT_TRUE(nph_ivec3_eq(nph_ivec3_max(a, b), nph_ivec3(3, 2, 5)));
    NTEST_ASSERT_EQ_INT(nph_ivec3_dot(a, b), -1);
    NTEST_ASSERT_FALSE(nph_ivec3_eq(a, b));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_from_ivec3(a), nph_vec3(3.0F, -4.0F, 5.0F)));
    NTEST_ASSERT_TRUE(
        nph_ivec3_eq(nph_ivec3_from_vec3_trunc(nph_vec3(1.9F, -1.9F, 0.5F)), nph_ivec3(1, -1, 0)));
    NTEST_ASSERT_TRUE(
        nph_ivec3_eq(nph_ivec3_from_vec3_floor(nph_vec3(1.9F, -1.1F, 0.5F)), nph_ivec3(1, -2, 0)));
}

NTEST(ivec, ivec4_and_uvec)
{
    NTEST_ASSERT_TRUE(nph_ivec4_eq(nph_ivec4_add(nph_ivec4(1, 2, 3, 4), nph_ivec4(1, 1, 1, 1)),
                                   nph_ivec4(2, 3, 4, 5)));
    NTEST_ASSERT_TRUE(nph_ivec4_eq(nph_ivec4_sub(nph_ivec4(1, 2, 3, 4), nph_ivec4(1, 1, 1, 1)),
                                   nph_ivec4(0, 1, 2, 3)));
    NTEST_ASSERT_FALSE(nph_ivec4_eq(nph_ivec4(1, 2, 3, 4), nph_ivec4(1, 2, 3, 5)));

    const NphUVec2 u2 = nph_uvec2(3U, 4U);
    NTEST_ASSERT_TRUE(nph_uvec2_eq(nph_uvec2_add(u2, nph_uvec2(1U, 1U)), nph_uvec2(4U, 5U)));
    NTEST_ASSERT_TRUE(nph_uvec2_eq(nph_uvec2_sub(u2, nph_uvec2(1U, 1U)), nph_uvec2(2U, 3U)));
    NTEST_ASSERT_TRUE(nph_uvec2_eq(nph_uvec2_mul(u2, nph_uvec2(2U, 2U)), nph_uvec2(6U, 8U)));
    NTEST_ASSERT_TRUE(nph_uvec2_eq(nph_uvec2_min(u2, nph_uvec2(5U, 1U)), nph_uvec2(3U, 1U)));
    NTEST_ASSERT_TRUE(nph_uvec2_eq(nph_uvec2_max(u2, nph_uvec2(5U, 1U)), nph_uvec2(5U, 4U)));
    NTEST_ASSERT_FALSE(nph_uvec2_eq(u2, nph_uvec2(5U, 1U)));
    NTEST_ASSERT_TRUE(nph_vec2_eq(nph_vec2_from_uvec2(u2), nph_vec2(3.0F, 4.0F)));

    const NphUVec3 u3 = nph_uvec3(3U, 4U, 5U);
    NTEST_ASSERT_TRUE(
        nph_uvec3_eq(nph_uvec3_add(u3, nph_uvec3(1U, 1U, 1U)), nph_uvec3(4U, 5U, 6U)));
    NTEST_ASSERT_TRUE(
        nph_uvec3_eq(nph_uvec3_sub(u3, nph_uvec3(1U, 1U, 1U)), nph_uvec3(2U, 3U, 4U)));
    NTEST_ASSERT_TRUE(
        nph_uvec3_eq(nph_uvec3_mul(u3, nph_uvec3(2U, 2U, 2U)), nph_uvec3(6U, 8U, 10U)));
    NTEST_ASSERT_TRUE(
        nph_uvec3_eq(nph_uvec3_min(u3, nph_uvec3(5U, 1U, 5U)), nph_uvec3(3U, 1U, 5U)));
    NTEST_ASSERT_TRUE(
        nph_uvec3_eq(nph_uvec3_max(u3, nph_uvec3(5U, 1U, 5U)), nph_uvec3(5U, 4U, 5U)));
    NTEST_ASSERT_FALSE(nph_uvec3_eq(u3, nph_uvec3(5U, 1U, 5U)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3_from_uvec3(u3), nph_vec3(3.0F, 4.0F, 5.0F)));

    NTEST_ASSERT_TRUE(
        nph_uvec4_eq(nph_uvec4_add(nph_uvec4(1U, 2U, 3U, 4U), nph_uvec4(1U, 1U, 1U, 1U)),
                     nph_uvec4(2U, 3U, 4U, 5U)));
    NTEST_ASSERT_FALSE(nph_uvec4_eq(nph_uvec4(1U, 2U, 3U, 4U), nph_uvec4(1U, 2U, 3U, 5U)));
}

// ---------------------------------------------------------------------------
// NphDVec3
// ---------------------------------------------------------------------------

NTEST(dvec3, arithmetic)
{
    const NphDVec3 a = nph_dvec3(1.0, 2.0, 3.0);
    const NphDVec3 b = nph_dvec3(-4.0, 0.5, 2.0);
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_zero(), nph_dvec3(0.0, 0.0, 0.0)));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_from_vec3(nph_vec3(1.0F, 2.0F, 3.0F)), a));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_dvec3_to_vec3(a), nph_vec3(1.0F, 2.0F, 3.0F)));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_add(a, b), nph_dvec3(-3.0, 2.5, 5.0)));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_sub(a, b), nph_dvec3(5.0, 1.5, 1.0)));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_mul(a, b), nph_dvec3(-4.0, 1.0, 6.0)));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_scale(a, 2.0), nph_dvec3(2.0, 4.0, 6.0)));
    NTEST_ASSERT_TRUE(
        nph_dvec3_eq(nph_dvec3_add_vec3(a, nph_vec3(1.0F, 1.0F, 1.0F)), nph_dvec3(2.0, 3.0, 4.0)));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_neg(a), nph_dvec3(-1.0, -2.0, -3.0)));
    NTEST_ASSERT_NEAR_F64(nph_dvec3_dot(a, b), 3.0, 0.0);
    NTEST_ASSERT_TRUE(
        nph_dvec3_eq(nph_dvec3_cross(nph_dvec3(1.0, 0.0, 0.0), nph_dvec3(0.0, 1.0, 0.0)),
                     nph_dvec3(0.0, 0.0, 1.0)));
    NTEST_ASSERT_NEAR_F64(nph_dvec3_length_sq(nph_dvec3(1.0, 2.0, 2.0)), 9.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_dvec3_length(nph_dvec3(1.0, 2.0, 2.0)), 3.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_dvec3_distance(a, nph_dvec3(2.0, 4.0, 5.0)), 3.0, 0.0);
    NTEST_ASSERT_TRUE(
        nph_dvec3_eq(nph_dvec3_normalize_or_zero(nph_dvec3_zero()), nph_dvec3_zero()));
    NTEST_ASSERT_TRUE(nph_dvec3_near(nph_dvec3_normalize_or_zero(nph_dvec3(0.0, 3.0, 4.0)),
                                     nph_dvec3(0.0, 0.6, 0.8), 1e-15));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_lerp(a, b, 0.0), a));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_lerp(a, b, 1.0), b));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_min(a, b), nph_dvec3(-4.0, 0.5, 2.0)));
    NTEST_ASSERT_TRUE(nph_dvec3_eq(nph_dvec3_max(a, b), nph_dvec3(1.0, 2.0, 3.0)));
    NTEST_ASSERT_FALSE(nph_dvec3_eq(a, b));
    NTEST_ASSERT_FALSE(nph_dvec3_near(a, b, 1e-9));
}

NTEST(dvec3, relative_positions_keep_precision_far_from_origin)
{
    // 100 km from the origin an f32 has ~8 mm of resolution; relative to a
    // camera 1 m away it has ~60 nm. The subtraction must happen in f64.
    const NphDVec3 camera = nph_dvec3(100000.3, 50.0, -100000.7);
    const NphVec3 offset = nph_vec3(0.001F, 1.0F, -0.5F);
    const NphDVec3 object = nph_dvec3_add_vec3(camera, offset);
    const NphVec3 rel = nph_dvec3_relative(object, camera);
    TM_ASSERT_VEC3_NEAR(rel, offset, 1e-6F);
    // Converting first and subtracting in f32 loses the millimetre along the
    // 100 km axis (the result is 0 or one 7.8 mm step).
    const NphVec3 lossy = nph_vec3_sub(nph_dvec3_to_vec3(object), nph_dvec3_to_vec3(camera));
    NTEST_ASSERT_TRUE(nph_f32_abs(lossy.x - 0.001F) > 1e-4F);
}
