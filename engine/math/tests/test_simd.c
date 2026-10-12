// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/simd.h>

#include "test_math_support.h"

#define ITER 2000U

// Exact (bit-for-bit, with +0 == -0) comparison of a register with a
// storage vector, logging both on failure.
#define ASSERT_X4_EQ(reg, vec)                                                                     \
    do {                                                                                           \
        const NphVec4 got_ = nph_f32x4_store_vec4(reg);                                            \
        const NphVec4 want_ = (vec);                                                               \
        if (!nph_vec4_eq(got_, want_)) {                                                           \
            NTEST_FAIL("%s != %s: (%.9g, %.9g, %.9g, %.9g) vs (%.9g, %.9g, %.9g, %.9g)", #reg,     \
                       #vec, (f64)got_.x, (f64)got_.y, (f64)got_.z, (f64)got_.w, (f64)want_.x,     \
                       (f64)want_.y, (f64)want_.z, (f64)want_.w);                                  \
        }                                                                                          \
    } while (0)

NTEST(simd, load_store_and_lanes)
{
    const NphVec4 v = nph_vec4(1.0F, 2.0F, 3.0F, 4.0F);
    ASSERT_X4_EQ(nph_f32x4_load_vec4(v), v);
    ASSERT_X4_EQ(nph_f32x4_set(1.0F, 2.0F, 3.0F, 4.0F), v);
    ASSERT_X4_EQ(nph_f32x4_splat(7.0F), nph_vec4_splat(7.0F));
    ASSERT_X4_EQ(nph_f32x4_zero(), nph_vec4_zero());
    const f32 arr[4] = {1.0F, 2.0F, 3.0F, 4.0F};
    ASSERT_X4_EQ(nph_f32x4_load(arr), v);
    NPH_ALIGNAS(16) f32 aligned[4] = {4.0F, 3.0F, 2.0F, 1.0F};
    ASSERT_X4_EQ(nph_f32x4_load_aligned(aligned), nph_vec4(4.0F, 3.0F, 2.0F, 1.0F));
    f32 out[4];
    nph_f32x4_store(out, nph_f32x4_load_vec4(v));
    NTEST_ASSERT_EQ_MEM(out, arr, sizeof arr);
    nph_f32x4_store_aligned(aligned, nph_f32x4_load_vec4(v));
    NTEST_ASSERT_EQ_MEM(aligned, arr, sizeof arr);
    ASSERT_X4_EQ(nph_f32x4_load_vec3(nph_vec3(1.0F, 2.0F, 3.0F)), nph_vec4(1.0F, 2.0F, 3.0F, 0.0F));
    ASSERT_X4_EQ(nph_f32x4_load_vec3_w(nph_vec3(1.0F, 2.0F, 3.0F), 9.0F),
                 nph_vec4(1.0F, 2.0F, 3.0F, 9.0F));
    ASSERT_X4_EQ(nph_f32x4_load_point(nph_vec3(1.0F, 2.0F, 3.0F)),
                 nph_vec4(1.0F, 2.0F, 3.0F, 1.0F));
    NTEST_ASSERT_TRUE(
        nph_vec3_eq(nph_f32x4_store_vec3(nph_f32x4_load_vec4(v)), nph_vec3(1.0F, 2.0F, 3.0F)));
    const NphQuat q = nph_quat(0.1F, 0.2F, 0.3F, 0.9F);
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_f32x4_store_quat(nph_f32x4_load_quat(q)), q));
    ASSERT_X4_EQ(nph_f32x4_load_plane(nph_plane(nph_vec3(1.0F, 2.0F, 3.0F), 4.0F)), v);
    NTEST_ASSERT_ULP_F32(nph_f32x4_get_x(nph_f32x4_load_vec4(v)), 1.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32x4_get_y(nph_f32x4_load_vec4(v)), 2.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32x4_get_z(nph_f32x4_load_vec4(v)), 3.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_f32x4_get_w(nph_f32x4_load_vec4(v)), 4.0F, 0U);
    ASSERT_X4_EQ(nph_f32x4_splat_x(nph_f32x4_load_vec4(v)), nph_vec4_splat(1.0F));
    ASSERT_X4_EQ(nph_f32x4_splat_y(nph_f32x4_load_vec4(v)), nph_vec4_splat(2.0F));
    ASSERT_X4_EQ(nph_f32x4_splat_z(nph_f32x4_load_vec4(v)), nph_vec4_splat(3.0F));
    ASSERT_X4_EQ(nph_f32x4_splat_w(nph_f32x4_load_vec4(v)), nph_vec4_splat(4.0F));
    ASSERT_X4_EQ(nph_f32x4_set_w(nph_f32x4_load_vec4(v), 0.5F), nph_vec4(1.0F, 2.0F, 3.0F, 0.5F));
    ASSERT_X4_EQ(NPH_F32X4_SHUFFLE(nph_f32x4_load_vec4(v), 3, 2, 1, 0),
                 nph_vec4(4.0F, 3.0F, 2.0F, 1.0F));
    ASSERT_X4_EQ(NPH_F32X4_SHUFFLE2(nph_f32x4_load_vec4(v), nph_f32x4_splat(9.0F), 1, 1, 0, 3),
                 nph_vec4(2.0F, 2.0F, 9.0F, 9.0F));
}

NTEST(simd, arithmetic_is_bit_identical_to_scalar)
{
    u64 rng = 60U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec4 a = tm_rng_vec4(&rng, -100.0F, 100.0F);
        const NphVec4 b = tm_rng_vec4(&rng, -100.0F, 100.0F);
        const NphVec4 c = tm_rng_vec4(&rng, -100.0F, 100.0F);
        const NphVec4 pos = nph_vec4_abs(a);
        const f32 s = tm_rng_range(&rng, -3.0F, 3.0F);
        const NphF32x4 va = nph_f32x4_load_vec4(a);
        const NphF32x4 vb = nph_f32x4_load_vec4(b);
        const NphF32x4 vc = nph_f32x4_load_vec4(c);
        const NphF32x4 vp = nph_f32x4_load_vec4(pos);
        ASSERT_X4_EQ(nph_f32x4_add(va, vb), nph_vec4_add(a, b));
        ASSERT_X4_EQ(nph_f32x4_sub(va, vb), nph_vec4_sub(a, b));
        ASSERT_X4_EQ(nph_f32x4_mul(va, vb), nph_vec4_mul(a, b));
        ASSERT_X4_EQ(nph_f32x4_div(va, vb), nph_vec4_div(a, b));
        ASSERT_X4_EQ(nph_f32x4_scale(va, s), nph_vec4_scale(a, s));
        ASSERT_X4_EQ(nph_f32x4_mul_add(va, vb, vc), nph_vec4_add(nph_vec4_mul(a, b), c));
        ASSERT_X4_EQ(nph_f32x4_neg_mul_add(va, vb, vc), nph_vec4_sub(c, nph_vec4_mul(a, b)));
        ASSERT_X4_EQ(nph_f32x4_neg(va), nph_vec4_neg(a));
        ASSERT_X4_EQ(nph_f32x4_abs(va), nph_vec4_abs(a));
        ASSERT_X4_EQ(nph_f32x4_min(va, vb), nph_vec4_min(a, b));
        ASSERT_X4_EQ(nph_f32x4_max(va, vb), nph_vec4_max(a, b));
        ASSERT_X4_EQ(nph_f32x4_clamp(va, nph_f32x4_splat(-1.0F), nph_f32x4_splat(1.0F)),
                     nph_vec4_clamp(a, nph_vec4_splat(-1.0F), nph_vec4_splat(1.0F)));
        ASSERT_X4_EQ(nph_f32x4_sqrt(vp), nph_vec4(nph_f32_sqrt(pos.x), nph_f32_sqrt(pos.y),
                                                  nph_f32_sqrt(pos.z), nph_f32_sqrt(pos.w)));
        ASSERT_X4_EQ(nph_f32x4_rsqrt(vp), nph_vec4(nph_f32_rsqrt(pos.x), nph_f32_rsqrt(pos.y),
                                                   nph_f32_rsqrt(pos.z), nph_f32_rsqrt(pos.w)));
        ASSERT_X4_EQ(nph_f32x4_floor(va), nph_vec4(nph_f32_floor(a.x), nph_f32_floor(a.y),
                                                   nph_f32_floor(a.z), nph_f32_floor(a.w)));
        ASSERT_X4_EQ(nph_f32x4_ceil(va), nph_vec4(nph_f32_ceil(a.x), nph_f32_ceil(a.y),
                                                  nph_f32_ceil(a.z), nph_f32_ceil(a.w)));
        ASSERT_X4_EQ(nph_f32x4_round_even(va),
                     nph_vec4(nph_f32_round_even(a.x), nph_f32_round_even(a.y),
                              nph_f32_round_even(a.z), nph_f32_round_even(a.w)));
        ASSERT_X4_EQ(nph_f32x4_trunc(va), nph_vec4(nph_f32_trunc(a.x), nph_f32_trunc(a.y),
                                                   nph_f32_trunc(a.z), nph_f32_trunc(a.w)));
        const f32 t = tm_rng_f32(&rng);
        ASSERT_X4_EQ(nph_f32x4_lerp(va, vb, nph_f32x4_splat(t)), nph_vec4_lerp(a, b, t));
        ASSERT_X4_EQ(nph_f32x4_hadd4(va), nph_vec4_splat(nph_vec4_hadd(a)));
        ASSERT_X4_EQ(nph_f32x4_hadd3(va), nph_vec4_splat(nph_vec3_hadd(nph_vec4_xyz(a))));
        // Fast approximations are close, not exact.
        const NphVec4 rf = nph_f32x4_store_vec4(nph_f32x4_rsqrt_fast(vp));
        const NphVec4 cf = nph_f32x4_store_vec4(nph_f32x4_rcp_fast(vp));
        for (u32 k = 0; k < 4U; ++k) {
            const f32 p = nph_vec4_at(pos, k);
            if (p > 1e-6F) {
                NTEST_ASSERT_TRUE(
                    nph_f32_approx_eq(nph_vec4_at(rf, k), nph_f32_rsqrt(p), 0.0F, 2e-6F));
                NTEST_ASSERT_TRUE(nph_f32_approx_eq(nph_vec4_at(cf, k), 1.0F / p, 0.0F, 2e-6F));
            }
        }
    }
    // Round-half-even and truncation at the awkward values.
    ASSERT_X4_EQ(nph_f32x4_round_even(nph_f32x4_set(0.5F, 1.5F, -0.5F, -2.5F)),
                 nph_vec4(0.0F, 2.0F, -0.0F, -2.0F));
    ASSERT_X4_EQ(nph_f32x4_trunc(nph_f32x4_set(1.7F, -1.7F, 0.2F, -0.2F)),
                 nph_vec4(1.0F, -1.0F, 0.0F, -0.0F));
}

NTEST(simd, comparisons_masks_and_select)
{
    const NphF32x4 a = nph_f32x4_set(1.0F, 2.0F, 3.0F, 4.0F);
    const NphF32x4 b = nph_f32x4_set(1.0F, 1.0F, 5.0F, 4.0F);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_cmp_eq(a, b)), 0x9U);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_cmp_ne(a, b)), 0x6U);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_cmp_lt(a, b)), 0x4U);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_cmp_le(a, b)), 0xDU);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_cmp_gt(a, b)), 0x2U);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_cmp_ge(a, b)), 0xBU);
    NTEST_ASSERT_TRUE(nph_f32x4_any(nph_f32x4_cmp_lt(a, b)));
    NTEST_ASSERT_FALSE(nph_f32x4_all(nph_f32x4_cmp_lt(a, b)));
    NTEST_ASSERT_TRUE(nph_f32x4_all(nph_f32x4_cmp_eq(a, a)));
    NTEST_ASSERT_FALSE(nph_f32x4_any(nph_f32x4_cmp_ne(a, a)));
    NTEST_ASSERT_TRUE(nph_f32x4_eq(a, a));
    NTEST_ASSERT_FALSE(nph_f32x4_eq(a, b));
    const NphF32x4 mask = nph_f32x4_cmp_gt(a, b);
    ASSERT_X4_EQ(nph_f32x4_select(mask, a, b), nph_vec4(1.0F, 2.0F, 5.0F, 4.0F));
    const NphF32x4 lt = nph_f32x4_cmp_lt(a, b);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_and(mask, lt)), 0x0U);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_or(mask, lt)), 0x6U);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_xor(mask, lt)), 0x6U);
    NTEST_ASSERT_EQ_UINT(nph_f32x4_movemask(nph_f32x4_andnot(mask, lt)), 0x4U);
    // Integer lanes.
    i32 ints[4];
    nph_i32x4_store(ints, nph_f32x4_to_i32x4_trunc(nph_f32x4_set(1.9F, -1.9F, 2.5F, -0.1F)));
    NTEST_ASSERT_EQ_INT(ints[0], 1);
    NTEST_ASSERT_EQ_INT(ints[1], -1);
    NTEST_ASSERT_EQ_INT(ints[2], 2);
    NTEST_ASSERT_EQ_INT(ints[3], 0);
    ASSERT_X4_EQ(nph_i32x4_to_f32x4(nph_i32x4_set(1, -2, 3, -4)),
                 nph_vec4(1.0F, -2.0F, 3.0F, -4.0F));
}

NTEST(simd, vector_semantics_match_vec_h)
{
    u64 rng = 61U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 a = tm_rng_vec3(&rng, -10.0F, 10.0F);
        const NphVec3 b = tm_rng_vec3(&rng, -10.0F, 10.0F);
        const NphVec4 a4 = tm_rng_vec4(&rng, -10.0F, 10.0F);
        const NphVec4 b4 = tm_rng_vec4(&rng, -10.0F, 10.0F);
        const NphF32x4 va = nph_f32x4_load_vec3(a);
        const NphF32x4 vb = nph_f32x4_load_vec3(b);
        ASSERT_X4_EQ(nph_f32x4_dot3(va, vb), nph_vec4_splat(nph_vec3_dot(a, b)));
        NTEST_ASSERT_ULP_F32(nph_f32x4_dot3_scalar(va, vb), nph_vec3_dot(a, b), 0U);
        ASSERT_X4_EQ(nph_f32x4_dot4(nph_f32x4_load_vec4(a4), nph_f32x4_load_vec4(b4)),
                     nph_vec4_splat(nph_vec4_dot(a4, b4)));
        NTEST_ASSERT_ULP_F32(
            nph_f32x4_dot4_scalar(nph_f32x4_load_vec4(a4), nph_f32x4_load_vec4(b4)),
            nph_vec4_dot(a4, b4), 0U);
        ASSERT_X4_EQ(nph_f32x4_cross3(va, vb), nph_vec4_from_vec3(nph_vec3_cross(a, b), 0.0F));
        ASSERT_X4_EQ(nph_f32x4_length3_sq(va), nph_vec4_splat(nph_vec3_length_sq(a)));
        ASSERT_X4_EQ(nph_f32x4_length3(va), nph_vec4_splat(nph_vec3_length(a)));
        ASSERT_X4_EQ(nph_f32x4_length4_sq(nph_f32x4_load_vec4(a4)),
                     nph_vec4_splat(nph_vec4_length_sq(a4)));
        ASSERT_X4_EQ(nph_f32x4_length4(nph_f32x4_load_vec4(a4)),
                     nph_vec4_splat(nph_vec4_length(a4)));
        if (nph_vec3_length_sq(a) > 0.0F) {
            ASSERT_X4_EQ(nph_f32x4_normalize3(va), nph_vec4_from_vec3(nph_vec3_normalize(a), 0.0F));
        }
        if (nph_vec4_length_sq(a4) > 0.0F) {
            ASSERT_X4_EQ(nph_f32x4_normalize4(nph_f32x4_load_vec4(a4)), nph_vec4_normalize(a4));
        }
        const NphPlane p = nph_plane_from_point_normal(b, nph_vec3_normalize(a));
        ASSERT_X4_EQ(
            nph_f32x4_plane_distance(nph_f32x4_load_plane(p),
                                     nph_f32x4_load_point(b4.x == 0.0F ? a : nph_vec4_xyz(b4))),
            nph_vec4_splat(nph_plane_distance(p, b4.x == 0.0F ? a : nph_vec4_xyz(b4))));
        ASSERT_X4_EQ(nph_f32x4_project(nph_f32x4_load_vec4(nph_vec4(a4.x, a4.y, a4.z, 2.0F))),
                     nph_vec4(a4.x / 2.0F, a4.y / 2.0F, a4.z / 2.0F, 1.0F));
    }
}

NTEST(simd, quaternion_semantics_match_quat_h)
{
    ASSERT_X4_EQ(nph_f32x4_quat_identity(), nph_quat_to_vec4(nph_quat_identity()));
    u64 rng = 62U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat a = tm_rng_unit_quat(&rng);
        const NphQuat b = tm_rng_unit_quat(&rng);
        const NphVec3 v = tm_rng_vec3(&rng, -10.0F, 10.0F);
        const NphF32x4 qa = nph_f32x4_load_quat(a);
        const NphF32x4 qb = nph_f32x4_load_quat(b);
        ASSERT_X4_EQ(nph_f32x4_quat_conjugate(qa), nph_quat_to_vec4(nph_quat_conjugate(a)));
        ASSERT_X4_EQ(nph_f32x4_quat_mul(qa, qb), nph_quat_to_vec4(nph_quat_mul(a, b)));
        ASSERT_X4_EQ(nph_f32x4_quat_rotate(qa, nph_f32x4_load_vec3(v)),
                     nph_vec4_from_vec3(nph_quat_rotate(a, v), 0.0F));
        ASSERT_X4_EQ(nph_f32x4_normalize4(nph_f32x4_quat_mul(qa, qb)),
                     nph_quat_to_vec4(nph_quat_normalize(nph_quat_mul(a, b))));
    }
}

NTEST(simd, mat4_matches_mat_h)
{
    NphMat4 id;
    nph_simd_mat4_store(&id, nph_simd_mat4_identity());
    NTEST_ASSERT_TRUE(nph_mat4_eq(id, nph_mat4_identity()));
    u64 rng = 63U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphMat4 a = tm_rng_general_mat4(&rng);
        const NphMat4 b = tm_rng_general_mat4(&rng);
        const NphVec4 v = tm_rng_vec4(&rng, -5.0F, 5.0F);
        const NphVec3 p = tm_rng_vec3(&rng, -5.0F, 5.0F);
        const NphSimdMat4 sa = nph_simd_mat4_load(&a);
        const NphSimdMat4 sb = nph_simd_mat4_load(&b);
        NphMat4 back;
        nph_simd_mat4_store(&back, sa);
        NTEST_ASSERT_TRUE(nph_mat4_eq(back, a));
        ASSERT_X4_EQ(nph_simd_mat4_mul_vec4(sa, nph_f32x4_load_vec4(v)), nph_mat4_mul_vec4(a, v));
        NphMat4 prod;
        nph_simd_mat4_store(&prod, nph_simd_mat4_mul(sa, sb));
        NTEST_ASSERT_TRUE(nph_mat4_eq(prod, nph_mat4_mul(a, b)));
        ASSERT_X4_EQ(nph_simd_mat4_transform_point(sa, nph_f32x4_load_vec3(p)),
                     nph_mat4_mul_vec4(a, nph_vec4_point(p)));
        ASSERT_X4_EQ(nph_simd_mat4_transform_vector(sa, nph_f32x4_load_vec3(p)),
                     nph_mat4_mul_vec4(a, nph_vec4_direction(p)));
        NphMat4 tr;
        nph_simd_mat4_store(&tr, nph_simd_mat4_transpose(sa));
        NTEST_ASSERT_TRUE(nph_mat4_eq(tr, nph_mat4_transpose(a)));
        // Inverse: a different operation order, so within tolerance.
        NphSimdMat4 sinv;
        NTEST_ASSERT_TRUE(nph_simd_mat4_inverse(sa, &sinv));
        NphMat4 inv;
        nph_simd_mat4_store(&inv, sinv);
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(inv, a), nph_mat4_identity(), 2e-4F);
        NphMat4 scalar_inv;
        NTEST_ASSERT_TRUE(nph_mat4_inverse(a, &scalar_inv));
        TM_ASSERT_MAT4_NEAR(inv, scalar_inv, 2e-4F);
        // TRS matrices are the common case and better conditioned.
        const NphMat4 trs = tm_rng_trs_mat4(&rng, NULL, NULL, NULL);
        NTEST_ASSERT_TRUE(nph_simd_mat4_inverse(nph_simd_mat4_load(&trs), &sinv));
        nph_simd_mat4_store(&inv, sinv);
        TM_ASSERT_MAT4_NEAR(nph_mat4_mul(inv, trs), nph_mat4_identity(), 1e-5F);
    }
    const NphMat4 singular = nph_mat4_from_scale(nph_vec3(1.0F, 0.0F, 1.0F));
    NphSimdMat4 untouched = nph_simd_mat4_identity();
    NTEST_ASSERT_FALSE(nph_simd_mat4_inverse(nph_simd_mat4_load(&singular), &untouched));
    nph_simd_mat4_store(&id, untouched);
    NTEST_ASSERT_TRUE(nph_mat4_eq(id, nph_mat4_identity()));
    const NphMat4 proj = nph_mat4_perspective(1.0F, 1.5F, 0.1F, 100.0F);
    NphSimdMat4 pinv;
    NTEST_ASSERT_TRUE(nph_simd_mat4_inverse(nph_simd_mat4_load(&proj), &pinv));
    NphMat4 pinv_m;
    nph_simd_mat4_store(&pinv_m, pinv);
    TM_ASSERT_MAT4_NEAR(nph_mat4_mul(pinv_m, proj), nph_mat4_identity(), 1e-4F);
}

NTEST(simd, affine_matches_affine_h)
{
    NphAffine id;
    nph_simd_affine_store(&id, nph_simd_affine_identity());
    NTEST_ASSERT_TRUE(nph_affine_eq(id, nph_affine_identity()));
    u64 rng = 64U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphAffine a = nph_affine_from_mat4(tm_rng_trs_mat4(&rng, NULL, NULL, NULL));
        const NphAffine b = nph_affine_from_mat4(tm_rng_trs_mat4(&rng, NULL, NULL, NULL));
        const NphVec3 p = tm_rng_vec3(&rng, -5.0F, 5.0F);
        const NphSimdAffine sa = nph_simd_affine_load(&a);
        const NphSimdAffine sb = nph_simd_affine_load(&b);
        NphAffine back;
        nph_simd_affine_store(&back, sa);
        NTEST_ASSERT_TRUE(nph_affine_eq(back, a));
        ASSERT_X4_EQ(nph_simd_affine_transform_point(sa, nph_f32x4_load_vec3(p)),
                     nph_vec4_point(nph_affine_transform_point(a, p)));
        ASSERT_X4_EQ(nph_simd_affine_transform_vector(sa, nph_f32x4_load_vec3(p)),
                     nph_vec4_direction(nph_affine_transform_vector(a, p)));
        NphAffine prod;
        nph_simd_affine_store(&prod, nph_simd_affine_mul(sa, sb));
        NTEST_ASSERT_TRUE(nph_affine_eq(prod, nph_affine_mul(a, b)));
        NphMat4 m;
        nph_simd_mat4_store(&m, nph_simd_affine_to_mat4(sa));
        NTEST_ASSERT_TRUE(nph_mat4_eq(m, nph_affine_to_mat4(a)));
    }
}
