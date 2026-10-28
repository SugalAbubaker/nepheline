// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/mat.h>

#include "test_math_support.h"

#define ITER 1000U

// ---------------------------------------------------------------------------
// NphMat3
// ---------------------------------------------------------------------------

NTEST(mat3, constructors_and_accessors)
{
    const NphMat3 m = nph_mat3_from_rows(nph_vec3(1.0F, 2.0F, 3.0F), nph_vec3(4.0F, 5.0F, 6.0F),
                                         nph_vec3(7.0F, 8.0F, 9.0F));
    NTEST_ASSERT_TRUE(nph_vec3_eq(m.c[0], nph_vec3(1.0F, 4.0F, 7.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(m.c[1], nph_vec3(2.0F, 5.0F, 8.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(m.c[2], nph_vec3(3.0F, 6.0F, 9.0F)));
    NTEST_ASSERT_TRUE(nph_mat3_eq(m, nph_mat3_from_cols(m.c[0], m.c[1], m.c[2])));
    for (u32 r = 0; r < 3U; ++r) {
        for (u32 c = 0; c < 3U; ++c) {
            NTEST_ASSERT_ULP_F32(nph_mat3_at(m, r, c), (f32)(r * 3U + c + 1U), 0U);
        }
        TM_ASSERT_VEC3_NEAR(nph_mat3_row(m, r),
                            nph_vec3((f32)(r * 3U + 1U), (f32)(r * 3U + 2U), (f32)(r * 3U + 3U)),
                            0.0F);
    }
    NTEST_ASSERT_ULP_F32(nph_mat3_trace(m), 15.0F, 0U);
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_identity(), nph_mat3_from_diagonal(nph_vec3_one())));
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_zero(), nph_mat3_from_diagonal(nph_vec3_zero())));
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_from_scale(nph_vec3(2.0F, 3.0F, 4.0F)),
                                  nph_mat3_from_diagonal(nph_vec3(2.0F, 3.0F, 4.0F))));
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_add(m, nph_mat3_zero()), m));
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_scale(m, 2.0F), nph_mat3_add(m, m)));
    NTEST_ASSERT_TRUE(nph_mat3_near(m, m, 0.0F));
    NTEST_ASSERT_FALSE(nph_mat3_near(m, nph_mat3_identity(), 1e-6F));
    NTEST_ASSERT_FALSE(nph_mat3_eq(m, nph_mat3_identity()));
}

NTEST(mat3, skew_matrix_is_the_cross_product)
{
    u64 rng = 20U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 v = tm_rng_vec3(&rng, -3.0F, 3.0F);
        const NphVec3 w = tm_rng_vec3(&rng, -3.0F, 3.0F);
        TM_ASSERT_VEC3_NEAR(nph_mat3_mul_vec3(nph_mat3_skew(v), w), nph_vec3_cross(v, w), 1e-5F);
    }
}

NTEST(mat3, multiplication_and_transpose)
{
    const NphMat3 a = nph_mat3_from_rows(nph_vec3(1.0F, 2.0F, 3.0F), nph_vec3(0.0F, 1.0F, 4.0F),
                                         nph_vec3(5.0F, 6.0F, 0.0F));
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_mul(a, nph_mat3_identity()), a));
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_mul(nph_mat3_identity(), a), a));
    // Row i of a^T is column i of a.
    const NphMat3 t = nph_mat3_transpose(a);
    for (u32 r = 0; r < 3U; ++r) {
        for (u32 c = 0; c < 3U; ++c) {
            NTEST_ASSERT_ULP_F32(nph_mat3_at(t, r, c), nph_mat3_at(a, c, r), 0U);
        }
    }
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat3_transpose(t), a));
    // mul_vec3 agrees with explicit dot products of rows.
    const NphVec3 v = nph_vec3(1.0F, -2.0F, 0.5F);
    const NphVec3 mv = nph_mat3_mul_vec3(a, v);
    for (u32 r = 0; r < 3U; ++r) {
        NTEST_ASSERT_ULP_F32(nph_vec3_at(mv, r), nph_vec3_dot(nph_mat3_row(a, r), v), 1U);
    }
    u64 rng = 21U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphMat3 x = nph_mat3_from_quat(tm_rng_unit_quat(&rng));
        const NphMat3 y = nph_mat3_from_quat(tm_rng_unit_quat(&rng));
        const NphMat3 z = nph_mat3_from_quat(tm_rng_unit_quat(&rng));
        TM_ASSERT_MAT3_NEAR(nph_mat3_mul(nph_mat3_mul(x, y), z),
                            nph_mat3_mul(x, nph_mat3_mul(y, z)), 1e-5F);
        TM_ASSERT_MAT3_NEAR(nph_mat3_transpose(nph_mat3_mul(x, y)),
                            nph_mat3_mul(nph_mat3_transpose(y), nph_mat3_transpose(x)), 1e-6F);
    }
}

NTEST(mat3, determinant_and_inverse)
{
    NTEST_ASSERT_ULP_F32(nph_mat3_determinant(nph_mat3_identity()), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_mat3_determinant(nph_mat3_from_scale(nph_vec3(2.0F, 3.0F, 4.0F))),
                         24.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_mat3_determinant(nph_mat3_from_cols(
                             nph_vec3_unit_x(), nph_vec3_unit_x(), nph_vec3_unit_z())),
                         0.0F, 0U);
    NphMat3 inv = nph_mat3_identity();
    NTEST_ASSERT_FALSE(nph_mat3_inverse(nph_mat3_zero(), &inv));
    NTEST_ASSERT_TRUE(nph_mat3_eq(inv, nph_mat3_identity())); // untouched
    NTEST_ASSERT_TRUE(nph_mat3_inverse(nph_mat3_from_scale(nph_vec3(2.0F, 4.0F, 8.0F)), &inv));
    TM_ASSERT_MAT3_NEAR(inv, nph_mat3_from_scale(nph_vec3(0.5F, 0.25F, 0.125F)), 0.0F);

    u64 rng = 22U;
    for (u32 i = 0; i < ITER; ++i) {
        NphMat3 m;
        do {
            const NphVec3 c0 = tm_rng_vec3(&rng, -1.0F, 1.0F);
            const NphVec3 c1 = tm_rng_vec3(&rng, -1.0F, 1.0F);
            const NphVec3 c2 = tm_rng_vec3(&rng, -1.0F, 1.0F);
            m = nph_mat3_from_cols(c0, c1, c2);
        } while (nph_f32_abs(nph_mat3_determinant(m)) < 0.05F);
        NTEST_ASSERT_TRUE(nph_mat3_inverse(m, &inv));
        TM_ASSERT_MAT3_NEAR(nph_mat3_mul(inv, m), nph_mat3_identity(), 1e-4F);
        TM_ASSERT_MAT3_NEAR(nph_mat3_mul(m, inv), nph_mat3_identity(), 1e-4F);
        NTEST_ASSERT_TRUE(nph_f32_approx_eq(nph_mat3_determinant(inv),
                                            1.0F / nph_mat3_determinant(m), 1e-4F, 1e-4F));
        // Rotations have unit determinant and their inverse is the transpose.
        const NphMat3 r = nph_mat3_from_quat(tm_rng_unit_quat(&rng));
        NTEST_ASSERT_NEAR_F32(nph_mat3_determinant(r), 1.0F, 1e-5F);
        NTEST_ASSERT_TRUE(nph_mat3_inverse(r, &inv));
        TM_ASSERT_MAT3_NEAR(inv, nph_mat3_transpose(r), 1e-5F);
    }
}

NTEST(mat3, quaternion_round_trip_covers_every_branch)
{
    // Dominant w, x, y, and z components select the four branches of
    // nph_quat_from_mat3.
    const NphQuat samples[] = {
        nph_quat_identity(),
        nph_quat_from_axis_angle(nph_vec3_unit_x(), NPH_PI - 0.1F),
        nph_quat_from_axis_angle(nph_vec3_unit_y(), NPH_PI - 0.1F),
        nph_quat_from_axis_angle(nph_vec3_unit_z(), NPH_PI - 0.1F),
        nph_quat_from_axis_angle(nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F)), NPH_PI),
        nph_quat_from_axis_angle(nph_vec3_normalize(nph_vec3(0.0F, 1.0F, 1.0F)), NPH_PI),
        nph_quat_from_axis_angle(nph_vec3_normalize(nph_vec3(1.0F, 0.0F, 1.0F)), NPH_PI),
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(samples); ++i) {
        const NphMat3 m = nph_mat3_from_quat(samples[i]);
        TM_ASSERT_ROTATION_NEAR(nph_quat_from_mat3(m), samples[i], 1e-5F);
    }
    u64 rng = 23U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat q = tm_rng_unit_quat(&rng);
        const NphMat3 m = nph_mat3_from_quat(q);
        // Orthonormal with positive determinant.
        TM_ASSERT_MAT3_NEAR(nph_mat3_mul(nph_mat3_transpose(m), m), nph_mat3_identity(), 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_mat3_determinant(m), 1.0F, 1e-5F);
        const NphQuat back = nph_quat_from_mat3(m);
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(back, 1e-6F));
        TM_ASSERT_ROTATION_NEAR(back, q, 1e-5F);
        TM_ASSERT_MAT3_NEAR(nph_mat3_from_axis_angle(nph_vec3_unit_z(), 0.5F),
                            nph_mat3_from_quat(nph_quat_from_rotation_z(0.5F)), 0.0F);
    }
}

NTEST(mat3, orthonormalize)
{
    u64 rng = 24U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphMat3 r = nph_mat3_from_quat(tm_rng_unit_quat(&rng));
        // Perturb by a few percent, then recover.
        const NphVec3 c0 = tm_rng_vec3(&rng, -1.0F, 1.0F);
        const NphVec3 c1 = tm_rng_vec3(&rng, -1.0F, 1.0F);
        const NphVec3 c2 = tm_rng_vec3(&rng, -1.0F, 1.0F);
        const NphMat3 noisy =
            nph_mat3_add(r, nph_mat3_scale(nph_mat3_from_cols(c0, c1, c2), 0.02F));
        const NphMat3 o = nph_mat3_orthonormalize(noisy);
        TM_ASSERT_MAT3_NEAR(nph_mat3_mul(nph_mat3_transpose(o), o), nph_mat3_identity(), 1e-5F);
        TM_ASSERT_MAT3_NEAR(o, r, 0.1F);
        NTEST_ASSERT_TRUE(nph_mat3_determinant(o) > 0.0F);
    }
    // Handedness is preserved.
    const NphMat3 left = nph_mat3_from_scale(nph_vec3(1.0F, 1.0F, -1.0F));
    const NphMat3 o = nph_mat3_orthonormalize(nph_mat3_scale(left, 3.0F));
    TM_ASSERT_MAT3_NEAR(o, left, 1e-6F);
    NTEST_ASSERT_TRUE(nph_mat3_determinant(o) < 0.0F);
}

// ---------------------------------------------------------------------------
// NphMat4
// ---------------------------------------------------------------------------

NTEST(mat4, constructors_and_accessors)
{
    const NphMat4 m = nph_mat4_from_rows(
        nph_vec4(1.0F, 2.0F, 3.0F, 4.0F), nph_vec4(5.0F, 6.0F, 7.0F, 8.0F),
        nph_vec4(9.0F, 10.0F, 11.0F, 12.0F), nph_vec4(13.0F, 14.0F, 15.0F, 16.0F));
    NTEST_ASSERT_TRUE(nph_vec4_eq(m.c[0], nph_vec4(1.0F, 5.0F, 9.0F, 13.0F)));
    NTEST_ASSERT_TRUE(nph_vec4_eq(m.c[3], nph_vec4(4.0F, 8.0F, 12.0F, 16.0F)));
    NTEST_ASSERT_TRUE(nph_mat4_eq(m, nph_mat4_from_cols(m.c[0], m.c[1], m.c[2], m.c[3])));
    for (u32 r = 0; r < 4U; ++r) {
        for (u32 c = 0; c < 4U; ++c) {
            NTEST_ASSERT_ULP_F32(nph_mat4_at(m, r, c), (f32)(r * 4U + c + 1U), 0U);
        }
        const NphVec4 row = nph_mat4_row(m, r);
        NTEST_ASSERT_ULP_F32(row.x, (f32)(r * 4U + 1U), 0U);
        NTEST_ASSERT_ULP_F32(row.w, (f32)(r * 4U + 4U), 0U);
    }
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_mat4_identity(), nph_mat4_from_diagonal(nph_vec4_one())));
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_mat4_zero(), nph_mat4_from_diagonal(nph_vec4_zero())));
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_mat4_from_mat3(nph_mat3_identity()), nph_mat4_identity()));
    const NphMat3 l = nph_mat3_from_rows(nph_vec3(1.0F, 2.0F, 3.0F), nph_vec3(4.0F, 5.0F, 6.0F),
                                         nph_vec3(7.0F, 8.0F, 9.0F));
    const NphVec3 t = nph_vec3(10.0F, 11.0F, 12.0F);
    const NphMat4 lt = nph_mat4_from_mat3_translation(l, t);
    NTEST_ASSERT_TRUE(nph_mat3_eq(nph_mat4_linear(lt), l));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_mat4_translation(lt), t));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_mat4_row(lt, 3U), nph_vec4(0.0F, 0.0F, 0.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_mat4_from_translation(t),
                                  nph_mat4_from_mat3_translation(nph_mat3_identity(), t)));
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_mat4_from_scale(nph_vec3(2.0F, 3.0F, 4.0F)),
                                  nph_mat4_from_diagonal(nph_vec4(2.0F, 3.0F, 4.0F, 1.0F))));
    const NphQuat q = nph_quat_from_rotation_y(0.8F);
    NTEST_ASSERT_TRUE(
        nph_mat4_eq(nph_mat4_from_quat(q), nph_mat4_from_mat3(nph_mat3_from_quat(q))));
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_mat4_add(m, nph_mat4_zero()), m));
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_mat4_scale(m, 2.0F), nph_mat4_add(m, m)));
    NTEST_ASSERT_TRUE(nph_mat4_near(m, m, 0.0F));
    NTEST_ASSERT_FALSE(nph_mat4_near(m, nph_mat4_identity(), 1e-6F));
    NTEST_ASSERT_FALSE(nph_mat4_eq(m, nph_mat4_identity()));
}

NTEST(mat4, multiplication_and_transforms)
{
    const NphMat4 tr = nph_mat4_from_translation(nph_vec3(1.0F, 2.0F, 3.0F));
    const NphMat4 sc = nph_mat4_from_scale(nph_vec3(2.0F, 2.0F, 2.0F));
    const NphVec3 p = nph_vec3(1.0F, 1.0F, 1.0F);
    // Translation moves points but not vectors; scale affects both.
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_point(tr, p), nph_vec3(2.0F, 3.0F, 4.0F), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_vector(tr, p), p, 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_vector(sc, p), nph_vec3(2.0F, 2.0F, 2.0F), 0.0F);
    // mul(a, b) applies b first: translate then scale doubles the translation.
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_point(nph_mat4_mul(sc, tr), p),
                        nph_vec3(4.0F, 6.0F, 8.0F), 0.0F);
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_point(nph_mat4_mul(tr, sc), p),
                        nph_vec3(3.0F, 4.0F, 5.0F), 0.0F);
    TM_ASSERT_VEC4_NEAR(nph_mat4_mul_vec4(tr, nph_vec4(1.0F, 1.0F, 1.0F, 0.5F)),
                        nph_vec4(1.5F, 2.0F, 2.5F, 0.5F), 0.0F);
    TM_ASSERT_VEC3_NEAR(
        nph_mat4_transform_point_perspective(nph_mat4_scale(nph_mat4_identity(), 2.0F), p), p,
        0.0F);

    u64 rng = 25U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphMat4 a = tm_rng_general_mat4(&rng);
        const NphMat4 b = tm_rng_general_mat4(&rng);
        const NphMat4 c = tm_rng_general_mat4(&rng);
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(a, nph_mat4_identity()), a, 0.0F);
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(nph_mat4_identity(), a), a, 0.0F);
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(nph_mat4_mul(a, b), c),
                            nph_mat4_mul(a, nph_mat4_mul(b, c)), 1e-4F);
        TM_ASSERT_MAT4_NEAR(nph_mat4_transpose(nph_mat4_mul(a, b)),
                            nph_mat4_mul(nph_mat4_transpose(b), nph_mat4_transpose(a)), 1e-5F);
        TM_ASSERT_MAT4_NEAR(nph_mat4_transpose(nph_mat4_transpose(a)), a, 0.0F);
        const NphVec4 v = tm_rng_vec4(&rng, -2.0F, 2.0F);
        const NphVec4 mv = nph_mat4_mul_vec4(a, v);
        for (u32 r = 0; r < 4U; ++r) {
            NTEST_ASSERT_NEAR_F32(nph_vec4_at(mv, r), nph_vec4_dot(nph_mat4_row(a, r), v), 1e-5F);
        }
        TM_ASSERT_VEC4_NEAR(nph_mat4_mul_vec4(nph_mat4_mul(a, b), v),
                            nph_mat4_mul_vec4(a, nph_mat4_mul_vec4(b, v)), 1e-4F);
    }
}

NTEST(mat4, determinant)
{
    NTEST_ASSERT_ULP_F32(nph_mat4_determinant(nph_mat4_identity()), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_mat4_determinant(nph_mat4_from_scale(nph_vec3(2.0F, 3.0F, 4.0F))),
                         24.0F, 0U);
    NTEST_ASSERT_ULP_F32(
        nph_mat4_determinant(nph_mat4_from_translation(nph_vec3(5.0F, 6.0F, 7.0F))), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_mat4_determinant(nph_mat4_zero()), 0.0F, 0U);
    u64 rng = 26U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphMat4 a = tm_rng_general_mat4(&rng);
        const NphMat4 b = tm_rng_general_mat4(&rng);
        NTEST_ASSERT_TRUE(nph_f32_approx_eq(nph_mat4_determinant(nph_mat4_mul(a, b)),
                                            nph_mat4_determinant(a) * nph_mat4_determinant(b),
                                            1e-4F, 1e-4F));
        NTEST_ASSERT_NEAR_F32(nph_mat4_determinant(nph_mat4_transpose(a)), nph_mat4_determinant(a),
                              1e-5F);
        const NphMat4 r = nph_mat4_from_quat(tm_rng_unit_quat(&rng));
        NTEST_ASSERT_NEAR_F32(nph_mat4_determinant(r), 1.0F, 1e-5F);
    }
}

NTEST(mat4, inverse)
{
    NphMat4 inv = nph_mat4_identity();
    NTEST_ASSERT_FALSE(nph_mat4_inverse(nph_mat4_zero(), &inv));
    NTEST_ASSERT_TRUE(nph_mat4_eq(inv, nph_mat4_identity())); // untouched
    NTEST_ASSERT_FALSE(
        nph_mat4_inverse_affine(nph_mat4_from_scale(nph_vec3(1.0F, 0.0F, 1.0F)), &inv));
    NTEST_ASSERT_TRUE(nph_mat4_eq(inv, nph_mat4_identity()));
    NTEST_ASSERT_TRUE(nph_mat4_inverse(nph_mat4_identity(), &inv));
    TM_ASSERT_MAT4_NEAR(inv, nph_mat4_identity(), 0.0F);
    NTEST_ASSERT_TRUE(
        nph_mat4_inverse(nph_mat4_from_translation(nph_vec3(1.0F, 2.0F, 3.0F)), &inv));
    TM_ASSERT_MAT4_NEAR(inv, nph_mat4_from_translation(nph_vec3(-1.0F, -2.0F, -3.0F)), 0.0F);

    u64 rng = 27U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphMat4 g = tm_rng_general_mat4(&rng);
        NTEST_ASSERT_TRUE(nph_mat4_inverse(g, &inv));
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(inv, g), nph_mat4_identity(), 2e-4F);
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(g, inv), nph_mat4_identity(), 2e-4F);

        const NphMat4 trs = tm_rng_trs_mat4(&rng, NULL, NULL, NULL);
        NphMat4 inv_a;
        NTEST_ASSERT_TRUE(nph_mat4_inverse(trs, &inv));
        NTEST_ASSERT_TRUE(nph_mat4_inverse_affine(trs, &inv_a));
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(inv, trs), nph_mat4_identity(), 1e-5F);
        TM_ASSERT_MAT4_NEAR(inv_a, inv, 1e-4F);
        NTEST_ASSERT_TRUE(nph_vec4_eq(nph_mat4_row(inv_a, 3U), nph_vec4(0.0F, 0.0F, 0.0F, 1.0F)));
        // A projection is invertible too (|det| is small but finite).
        const NphMat4 proj = nph_mat4_perspective(1.0F, 1.5F, 0.1F, 100.0F);
        NTEST_ASSERT_TRUE(nph_mat4_inverse(proj, &inv));
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(inv, proj), nph_mat4_identity(), 1e-4F);
    }
}

NTEST(mat4, trs_compose_and_decompose)
{
    u64 rng = 28U;
    for (u32 i = 0; i < ITER; ++i) {
        NphVec3 t;
        NphQuat r;
        NphVec3 s;
        const NphMat4 m = tm_rng_trs_mat4(&rng, &t, &r, &s);
        // Equal to the explicit product of the three factors.
        const NphMat4 product =
            nph_mat4_mul(nph_mat4_from_translation(t),
                         nph_mat4_mul(nph_mat4_from_quat(r), nph_mat4_from_scale(s)));
        TM_ASSERT_MAT4_NEAR(m, product, 1e-5F);
        NphVec3 t2;
        NphQuat r2;
        NphVec3 s2;
        NTEST_ASSERT_TRUE(nph_mat4_decompose_trs(m, &t2, &r2, &s2));
        TM_ASSERT_VEC3_NEAR(t2, t, 0.0F);
        TM_ASSERT_VEC3_NEAR(s2, s, 1e-5F);
        TM_ASSERT_ROTATION_NEAR(r2, r, 1e-4F);
        TM_ASSERT_MAT4_NEAR(nph_mat4_from_trs(t2, r2, s2), m, 1e-4F);
    }
    // A reflection decomposes to a negative x scale and recomposes exactly.
    const NphMat4 refl = nph_mat4_from_trs(
        nph_vec3(1.0F, 2.0F, 3.0F), nph_quat_from_rotation_z(0.4F), nph_vec3(-2.0F, 1.5F, 1.0F));
    NphVec3 t;
    NphQuat r;
    NphVec3 s;
    NTEST_ASSERT_TRUE(nph_mat4_decompose_trs(refl, &t, &r, &s));
    NTEST_ASSERT_TRUE(s.x < 0.0F);
    TM_ASSERT_VEC3_NEAR(s, nph_vec3(-2.0F, 1.5F, 1.0F), 1e-5F);
    TM_ASSERT_MAT4_NEAR(nph_mat4_from_trs(t, r, s), refl, 1e-5F);
    // A zero scale cannot be decomposed; the outputs stay untouched.
    t = nph_vec3_one();
    NTEST_ASSERT_FALSE(
        nph_mat4_decompose_trs(nph_mat4_from_scale(nph_vec3(1.0F, 0.0F, 1.0F)), &t, &r, &s));
    NTEST_ASSERT_TRUE(nph_vec3_eq(t, nph_vec3_one()));
}

NTEST(mat4, look_at)
{
    const NphVec3 eye = nph_vec3(3.0F, 2.0F, 5.0F);
    const NphVec3 target = nph_vec3(-1.0F, 0.5F, -4.0F);
    const NphMat4 view = nph_mat4_look_at(eye, target, nph_vec3_unit_y());
    // The eye goes to the origin; the target lands on the -Z axis at its distance.
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_point(view, eye), nph_vec3_zero(), 1e-5F);
    const NphVec3 vt = nph_mat4_transform_point(view, target);
    NTEST_ASSERT_NEAR_F32(vt.x, 0.0F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(vt.y, 0.0F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(vt.z, -nph_vec3_distance(eye, target), 1e-5F);
    // World up has a positive view-space y; world +X relative to a camera
    // looking down -Z has a positive view-space x (right-handed).
    NTEST_ASSERT_TRUE(nph_mat4_transform_vector(view, nph_vec3_unit_y()).y > 0.0F);
    const NphMat4 canonical =
        nph_mat4_look_at(nph_vec3_zero(), nph_vec3(0.0F, 0.0F, -1.0F), nph_vec3_unit_y());
    TM_ASSERT_MAT4_NEAR(canonical, nph_mat4_identity(), 1e-6F);
    // Rigid: the linear part is orthonormal and the inverse is the affine inverse.
    const NphMat3 l = nph_mat4_linear(view);
    TM_ASSERT_MAT3_NEAR(nph_mat3_mul(nph_mat3_transpose(l), l), nph_mat3_identity(), 1e-5F);
    NphMat4 inv;
    NTEST_ASSERT_TRUE(nph_mat4_inverse_affine(view, &inv));
    TM_ASSERT_VEC3_NEAR(nph_mat4_translation(inv), eye, 1e-4F);
    TM_ASSERT_MAT4_NEAR(nph_mat4_look_to(eye, nph_vec3_sub(target, eye), nph_vec3_unit_y()), view,
                        0.0F);
}

NTEST(mat4, perspective_infinite_reverse_z)
{
    const f32 near = 0.25F;
    const NphMat4 p = nph_mat4_perspective_infinite(NPH_HALF_PI, 1.0F, near);
    // Depth is 1 at the near plane, 0.5 at twice the distance, 0 at infinity.
    NTEST_ASSERT_ULP_F32(nph_mat4_transform_point_perspective(p, nph_vec3(0.0F, 0.0F, -near)).z,
                         1.0F, 0U);
    NTEST_ASSERT_ULP_F32(
        nph_mat4_transform_point_perspective(p, nph_vec3(0.0F, 0.0F, -2.0F * near)).z, 0.5F, 0U);
    NTEST_ASSERT_NEAR_F32(nph_mat4_transform_point_perspective(p, nph_vec3(0.0F, 0.0F, -1e7F)).z,
                          0.0F, 1e-6F);
    // Depth decreases monotonically with distance.
    f32 prev = 2.0F;
    for (u32 i = 1; i <= 64U; ++i) {
        const f32 d = near * (f32)i;
        const f32 z = nph_mat4_transform_point_perspective(p, nph_vec3(0.0F, 0.0F, -d)).z;
        NTEST_ASSERT_TRUE(z < prev && z > 0.0F);
        prev = z;
    }
    // 90 degree vertical FOV, square aspect: the frustum edge at distance d
    // is at |x| == |y| == d; +Y up in view space is -Y in clip space.
    const NphVec3 edge = nph_mat4_transform_point_perspective(p, nph_vec3(2.0F, 2.0F, -2.0F));
    NTEST_ASSERT_NEAR_F32(edge.x, 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(edge.y, -1.0F, 1e-6F);
    const NphVec3 mid = nph_mat4_transform_point_perspective(p, nph_vec3(0.5F, -0.5F, -2.0F));
    NTEST_ASSERT_NEAR_F32(mid.x, 0.25F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(mid.y, 0.25F, 1e-6F);
    // Clip w is the view distance.
    NTEST_ASSERT_ULP_F32(nph_mat4_mul_vec4(p, nph_vec4(1.0F, 1.0F, -3.0F, 1.0F)).w, 3.0F, 0U);
    // Aspect ratio scales x only.
    const NphMat4 wide = nph_mat4_perspective_infinite(NPH_HALF_PI, 2.0F, near);
    const NphVec3 w = nph_mat4_transform_point_perspective(wide, nph_vec3(2.0F, 2.0F, -2.0F));
    NTEST_ASSERT_NEAR_F32(w.x, 0.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(w.y, -1.0F, 1e-6F);
    // Invalid parameters are asserted.
    TM_EXPECT_ASSERT((void)nph_mat4_perspective_infinite(NPH_HALF_PI, 1.0F, 0.0F));
    TM_EXPECT_ASSERT((void)nph_mat4_perspective_infinite(NPH_HALF_PI, 0.0F, near));
    TM_EXPECT_ASSERT((void)nph_mat4_perspective_infinite(0.0F, 1.0F, near));
}

NTEST(mat4, perspective_finite_reverse_z)
{
    const f32 near = 0.5F;
    const f32 far = 200.0F;
    const NphMat4 p = nph_mat4_perspective(1.2F, 1.6F, near, far);
    NTEST_ASSERT_NEAR_F32(nph_mat4_transform_point_perspective(p, nph_vec3(0.0F, 0.0F, -near)).z,
                          1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_mat4_transform_point_perspective(p, nph_vec3(0.0F, 0.0F, -far)).z,
                          0.0F, 1e-6F);
    const f32 mid = nph_mat4_transform_point_perspective(p, nph_vec3(0.0F, 0.0F, -10.0F)).z;
    NTEST_ASSERT_TRUE(mid > 0.0F && mid < 1.0F);
    // With a huge far plane it converges to the infinite matrix.
    const NphMat4 big = nph_mat4_perspective(1.2F, 1.6F, near, 1e9F);
    TM_ASSERT_MAT4_NEAR(big, nph_mat4_perspective_infinite(1.2F, 1.6F, near), 1e-6F);
    // Same x/y mapping as the infinite form.
    const NphMat4 inf = nph_mat4_perspective_infinite(1.2F, 1.6F, near);
    const NphVec3 a = nph_mat4_transform_point_perspective(p, nph_vec3(1.0F, -2.0F, -7.0F));
    const NphVec3 b = nph_mat4_transform_point_perspective(inf, nph_vec3(1.0F, -2.0F, -7.0F));
    NTEST_ASSERT_NEAR_F32(a.x, b.x, 1e-6F);
    NTEST_ASSERT_NEAR_F32(a.y, b.y, 1e-6F);
    TM_EXPECT_ASSERT((void)nph_mat4_perspective(1.2F, 1.6F, near, near));
}

NTEST(mat4, orthographic_reverse_z)
{
    const NphMat4 o = nph_mat4_orthographic(-2.0F, 6.0F, -1.0F, 3.0F, 0.5F, 10.5F);
    // Corners of the box map to the clip-space cube; +Y (top) maps to -1.
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_point(o, nph_vec3(-2.0F, -1.0F, -0.5F)),
                        nph_vec3(-1.0F, 1.0F, 1.0F), 1e-6F);
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_point(o, nph_vec3(6.0F, 3.0F, -10.5F)),
                        nph_vec3(1.0F, -1.0F, 0.0F), 1e-6F);
    TM_ASSERT_VEC3_NEAR(nph_mat4_transform_point(o, nph_vec3(2.0F, 1.0F, -5.5F)),
                        nph_vec3(0.0F, 0.0F, 0.5F), 1e-6F);
    NTEST_ASSERT_ULP_F32(nph_mat4_mul_vec4(o, nph_vec4(1.0F, 1.0F, 1.0F, 1.0F)).w, 1.0F, 0U);
    TM_EXPECT_ASSERT((void)nph_mat4_orthographic(1.0F, 1.0F, -1.0F, 3.0F, 0.5F, 10.5F));
}
