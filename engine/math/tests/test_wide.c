// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/kernels.h>
#include <nepheline/math/wide.h>

#include "test_math_support.h"

#define ITER 1000U

// This translation unit is compiled at the SSE4.2 baseline, so NphF32x8 is
// the paired-register fallback here; the AVX form is exercised through the
// AVX2 kernel table in test_kernels.c.

static void fill8(u64 *rng, f32 lo, f32 hi, f32 out[8])
{
    for (u32 i = 0; i < 8U; ++i) {
        out[i] = tm_rng_range(rng, lo, hi);
    }
}

#define ASSERT_X8_LANES(reg, expr)                                                                 \
    do {                                                                                           \
        f32 got_[8];                                                                               \
        nph_f32x8_store(got_, reg);                                                                \
        for (u32 k = 0; k < 8U; ++k) {                                                             \
            const f32 want_ = (expr);                                                              \
            if (!(got_[k] == want_)) {                                                             \
                NTEST_FAIL("%s lane %u: %.9g vs %.9g", #reg, k, (f64)got_[k], (f64)want_);         \
            }                                                                                      \
        }                                                                                          \
    } while (0)

NTEST(wide, f32x8_lanes_match_scalar)
{
    NTEST_ASSERT_EQ_INT(NPH_WIDE_AVX, 0); // baseline unit: the paired form
    u64 rng = 70U;
    for (u32 i = 0; i < ITER; ++i) {
        f32 a[8];
        f32 b[8];
        f32 c[8];
        fill8(&rng, -50.0F, 50.0F, a);
        fill8(&rng, -50.0F, 50.0F, b);
        fill8(&rng, -50.0F, 50.0F, c);
        const NphF32x8 va = nph_f32x8_load(a);
        const NphF32x8 vb = nph_f32x8_load(b);
        const NphF32x8 vc = nph_f32x8_load(c);
        ASSERT_X8_LANES(va, a[k]);
        ASSERT_X8_LANES(nph_f32x8_add(va, vb), a[k] + b[k]);
        ASSERT_X8_LANES(nph_f32x8_sub(va, vb), a[k] - b[k]);
        ASSERT_X8_LANES(nph_f32x8_mul(va, vb), a[k] * b[k]);
        ASSERT_X8_LANES(nph_f32x8_div(va, vb), a[k] / b[k]);
        ASSERT_X8_LANES(nph_f32x8_mul_add(va, vb, vc), a[k] * b[k] + c[k]);
        ASSERT_X8_LANES(nph_f32x8_min(va, vb), nph_f32_min(a[k], b[k]));
        ASSERT_X8_LANES(nph_f32x8_max(va, vb), nph_f32_max(a[k], b[k]));
        ASSERT_X8_LANES(nph_f32x8_neg(va), -a[k]);
        ASSERT_X8_LANES(nph_f32x8_abs(va), nph_f32_abs(a[k]));
        ASSERT_X8_LANES(nph_f32x8_sqrt(nph_f32x8_abs(va)), nph_f32_sqrt(nph_f32_abs(a[k])));
        ASSERT_X8_LANES(nph_f32x8_rsqrt(nph_f32x8_abs(va)), nph_f32_rsqrt(nph_f32_abs(a[k])));
        ASSERT_X8_LANES(nph_f32x8_floor(va), nph_f32_floor(a[k]));
        ASSERT_X8_LANES(nph_f32x8_ceil(va), nph_f32_ceil(a[k]));
        const f32 t = tm_rng_f32(&rng);
        ASSERT_X8_LANES(nph_f32x8_lerp(va, vb, nph_f32x8_splat(t)), nph_f32_lerp(a[k], b[k], t));
        ASSERT_X8_LANES(nph_f32x8_splat(t), t);
        ASSERT_X8_LANES(nph_f32x8_zero(), 0.0F);
        ASSERT_X8_LANES(nph_f32x8_select(nph_f32x8_cmp_lt(va, vb), va, vb),
                        nph_f32_min(a[k], b[k]));
        u32 lt = 0U;
        u32 le = 0U;
        u32 gt = 0U;
        u32 ge = 0U;
        u32 eq = 0U;
        for (u32 k = 0; k < 8U; ++k) {
            lt |= (a[k] < b[k] ? 1U : 0U) << k;
            le |= (a[k] <= b[k] ? 1U : 0U) << k;
            gt |= (a[k] > b[k] ? 1U : 0U) << k;
            ge |= (a[k] >= b[k] ? 1U : 0U) << k;
            eq |= (a[k] == b[k] ? 1U : 0U) << k;
        }
        NTEST_ASSERT_EQ_UINT(nph_f32x8_movemask(nph_f32x8_cmp_lt(va, vb)), lt);
        NTEST_ASSERT_EQ_UINT(nph_f32x8_movemask(nph_f32x8_cmp_le(va, vb)), le);
        NTEST_ASSERT_EQ_UINT(nph_f32x8_movemask(nph_f32x8_cmp_gt(va, vb)), gt);
        NTEST_ASSERT_EQ_UINT(nph_f32x8_movemask(nph_f32x8_cmp_ge(va, vb)), ge);
        NTEST_ASSERT_EQ_UINT(nph_f32x8_movemask(nph_f32x8_cmp_eq(va, vb)), eq);
        NTEST_ASSERT_EQ_UINT(
            nph_f32x8_movemask(nph_f32x8_and(nph_f32x8_cmp_lt(va, vb), nph_f32x8_cmp_le(va, vb))),
            lt);
        NTEST_ASSERT_EQ_UINT(
            nph_f32x8_movemask(nph_f32x8_or(nph_f32x8_cmp_lt(va, vb), nph_f32x8_cmp_gt(va, vb))),
            lt | gt);
        NTEST_ASSERT_EQ_UINT(
            nph_f32x8_movemask(nph_f32x8_xor(nph_f32x8_cmp_le(va, vb), nph_f32x8_cmp_lt(va, vb))),
            eq);
        NTEST_ASSERT_EQ_UINT(nph_f32x8_movemask(nph_f32x8_andnot(nph_f32x8_cmp_lt(va, vb),
                                                                 nph_f32x8_cmp_le(va, vb))),
                             eq);
        NTEST_ASSERT_TRUE(nph_f32x8_all(nph_f32x8_cmp_eq(va, va)));
        NTEST_ASSERT_FALSE(nph_f32x8_any(nph_f32x8_cmp_lt(va, va)));
        // Halves round-trip.
        const NphF32x8 rebuilt = nph_f32x8_from_x4(nph_f32x8_lo(va), nph_f32x8_hi(va));
        ASSERT_X8_LANES(rebuilt, a[k]);
        NTEST_ASSERT_TRUE(nph_f32x4_eq(nph_f32x8_lo(va), nph_f32x4_load(a)));
        NTEST_ASSERT_TRUE(nph_f32x4_eq(nph_f32x8_hi(va), nph_f32x4_load(a + 4)));
    }
}

NTEST(wide, vec3x4_transposes_and_ops)
{
    u64 rng = 71U;
    for (u32 i = 0; i < ITER; ++i) {
        NphVec3 a[4];
        NphVec3 b[4];
        for (u32 k = 0; k < 4U; ++k) {
            a[k] = tm_rng_vec3(&rng, -10.0F, 10.0F);
            b[k] = tm_rng_vec3(&rng, -10.0F, 10.0F);
        }
        const NphVec3x4 va = nph_vec3x4_load_aos(a);
        const NphVec3x4 vb = nph_vec3x4_load_aos(b);
        NphVec3 back[4];
        nph_vec3x4_store_aos(back, va);
        for (u32 k = 0; k < 4U; ++k) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(back[k], a[k]));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(va, k), a[k]));
        }
        // SoA load/store agree with the AoS transpose.
        f32 x[4];
        f32 y[4];
        f32 z[4];
        nph_vec3x4_store_soa(x, y, z, va);
        for (u32 k = 0; k < 4U; ++k) {
            NTEST_ASSERT_TRUE(x[k] == a[k].x && y[k] == a[k].y && z[k] == a[k].z);
        }
        nph_vec3x4_store_aos(back, nph_vec3x4_load_soa(x, y, z));
        for (u32 k = 0; k < 4U; ++k) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(back[k], a[k]));
        }
        const NphF32x4 s = nph_f32x4_load_vec4(tm_rng_vec4(&rng, -3.0F, 3.0F));
        const NphVec4 sv = nph_f32x4_store_vec4(s);
        const f32 t = tm_rng_f32(&rng);
        const NphAffine m = nph_affine_from_mat4(tm_rng_trs_mat4(&rng, NULL, NULL, NULL));
        const NphVec3x4 r_add = nph_vec3x4_add(va, vb);
        const NphVec3x4 r_sub = nph_vec3x4_sub(va, vb);
        const NphVec3x4 r_mul = nph_vec3x4_mul(va, vb);
        const NphVec3x4 r_scale = nph_vec3x4_scale(va, s);
        const NphVec3x4 r_mad = nph_vec3x4_mul_add(va, vb, s);
        const NphVec3x4 r_neg = nph_vec3x4_neg(va);
        const NphVec3x4 r_min = nph_vec3x4_min(va, vb);
        const NphVec3x4 r_max = nph_vec3x4_max(va, vb);
        const NphVec3x4 r_cross = nph_vec3x4_cross(va, vb);
        const NphVec3x4 r_norm = nph_vec3x4_normalize(va);
        const NphVec3x4 r_lerp = nph_vec3x4_lerp(va, vb, nph_f32x4_splat(t));
        const NphVec3x4 r_pt = nph_vec3x4_transform_point(&m, va);
        const NphVec3x4 r_vec = nph_vec3x4_transform_vector(&m, va);
        const NphVec3x4 r_sel = nph_vec3x4_select(nph_f32x4_cmp_lt(va.x, vb.x), va, vb);
        const NphVec4 dot = nph_f32x4_store_vec4(nph_vec3x4_dot(va, vb));
        const NphVec4 len = nph_f32x4_store_vec4(nph_vec3x4_length(va));
        const NphVec4 len_sq = nph_f32x4_store_vec4(nph_vec3x4_length_sq(va));
        for (u32 k = 0; k < 4U; ++k) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_add, k), nph_vec3_add(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_sub, k), nph_vec3_sub(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_mul, k), nph_vec3_mul(a[k], b[k])));
            NTEST_ASSERT_TRUE(
                nph_vec3_eq(nph_vec3x4_get(r_scale, k), nph_vec3_scale(a[k], nph_vec4_at(sv, k))));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_mad, k),
                                          nph_vec3_mul_add(a[k], b[k], nph_vec4_at(sv, k))));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_neg, k), nph_vec3_neg(a[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_min, k), nph_vec3_min(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_max, k), nph_vec3_max(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_cross, k), nph_vec3_cross(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_norm, k), nph_vec3_normalize(a[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_lerp, k), nph_vec3_lerp(a[k], b[k], t)));
            NTEST_ASSERT_TRUE(
                nph_vec3_eq(nph_vec3x4_get(r_pt, k), nph_affine_transform_point(m, a[k])));
            NTEST_ASSERT_TRUE(
                nph_vec3_eq(nph_vec3x4_get(r_vec, k), nph_affine_transform_vector(m, a[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(r_sel, k), a[k].x < b[k].x ? a[k] : b[k]));
            NTEST_ASSERT_ULP_F32(nph_vec4_at(dot, k), nph_vec3_dot(a[k], b[k]), 0U);
            NTEST_ASSERT_ULP_F32(nph_vec4_at(len, k), nph_vec3_length(a[k]), 0U);
            NTEST_ASSERT_ULP_F32(nph_vec4_at(len_sq, k), nph_vec3_length_sq(a[k]), 0U);
        }
        NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(nph_vec3x4_splat(a[0]), 3U), a[0]));
        NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x4_get(nph_vec3x4_zero(), 1U), nph_vec3_zero()));
    }
}

NTEST(wide, vec3x8_matches_vec3x4_pairs)
{
    u64 rng = 72U;
    for (u32 i = 0; i < ITER; ++i) {
        NphVec3 a[8];
        NphVec3 b[8];
        for (u32 k = 0; k < 8U; ++k) {
            a[k] = tm_rng_vec3(&rng, -10.0F, 10.0F);
            b[k] = tm_rng_vec3(&rng, -10.0F, 10.0F);
        }
        const NphVec3x8 va = nph_vec3x8_load_aos(a);
        const NphVec3x8 vb = nph_vec3x8_load_aos(b);
        NphVec3 back[8];
        nph_vec3x8_store_aos(back, va);
        for (u32 k = 0; k < 8U; ++k) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(back[k], a[k]));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(va, k), a[k]));
        }
        f32 x[8];
        f32 y[8];
        f32 z[8];
        nph_vec3x8_store_soa(x, y, z, va);
        nph_vec3x8_store_aos(back, nph_vec3x8_load_soa(x, y, z));
        for (u32 k = 0; k < 8U; ++k) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(back[k], a[k]));
        }
        NTEST_ASSERT_TRUE(nph_vec3_eq(
            nph_vec3x8_get(nph_vec3x8_from_x4(nph_vec3x8_lo(va), nph_vec3x8_hi(va)), 5U), a[5]));
        f32 s8[8];
        fill8(&rng, -3.0F, 3.0F, s8);
        const NphF32x8 s = nph_f32x8_load(s8);
        const f32 t = tm_rng_f32(&rng);
        const NphAffine m = nph_affine_from_mat4(tm_rng_trs_mat4(&rng, NULL, NULL, NULL));
        const NphVec3x8 r_add = nph_vec3x8_add(va, vb);
        const NphVec3x8 r_sub = nph_vec3x8_sub(va, vb);
        const NphVec3x8 r_mul = nph_vec3x8_mul(va, vb);
        const NphVec3x8 r_scale = nph_vec3x8_scale(va, s);
        const NphVec3x8 r_mad = nph_vec3x8_mul_add(va, vb, s);
        const NphVec3x8 r_neg = nph_vec3x8_neg(va);
        const NphVec3x8 r_min = nph_vec3x8_min(va, vb);
        const NphVec3x8 r_max = nph_vec3x8_max(va, vb);
        const NphVec3x8 r_cross = nph_vec3x8_cross(va, vb);
        const NphVec3x8 r_norm = nph_vec3x8_normalize(va);
        const NphVec3x8 r_lerp = nph_vec3x8_lerp(va, vb, nph_f32x8_splat(t));
        const NphVec3x8 r_pt = nph_vec3x8_transform_point(&m, va);
        const NphVec3x8 r_vec = nph_vec3x8_transform_vector(&m, va);
        const NphVec3x8 r_sel = nph_vec3x8_select(nph_f32x8_cmp_lt(va.x, vb.x), va, vb);
        f32 dot[8];
        f32 len[8];
        f32 len_sq[8];
        nph_f32x8_store(dot, nph_vec3x8_dot(va, vb));
        nph_f32x8_store(len, nph_vec3x8_length(va));
        nph_f32x8_store(len_sq, nph_vec3x8_length_sq(va));
        for (u32 k = 0; k < 8U; ++k) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_add, k), nph_vec3_add(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_sub, k), nph_vec3_sub(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_mul, k), nph_vec3_mul(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_scale, k), nph_vec3_scale(a[k], s8[k])));
            NTEST_ASSERT_TRUE(
                nph_vec3_eq(nph_vec3x8_get(r_mad, k), nph_vec3_mul_add(a[k], b[k], s8[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_neg, k), nph_vec3_neg(a[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_min, k), nph_vec3_min(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_max, k), nph_vec3_max(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_cross, k), nph_vec3_cross(a[k], b[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_norm, k), nph_vec3_normalize(a[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_lerp, k), nph_vec3_lerp(a[k], b[k], t)));
            NTEST_ASSERT_TRUE(
                nph_vec3_eq(nph_vec3x8_get(r_pt, k), nph_affine_transform_point(m, a[k])));
            NTEST_ASSERT_TRUE(
                nph_vec3_eq(nph_vec3x8_get(r_vec, k), nph_affine_transform_vector(m, a[k])));
            NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(r_sel, k), a[k].x < b[k].x ? a[k] : b[k]));
            NTEST_ASSERT_ULP_F32(dot[k], nph_vec3_dot(a[k], b[k]), 0U);
            NTEST_ASSERT_ULP_F32(len[k], nph_vec3_length(a[k]), 0U);
            NTEST_ASSERT_ULP_F32(len_sq[k], nph_vec3_length_sq(a[k]), 0U);
        }
        NTEST_ASSERT_TRUE(nph_vec3_eq(nph_vec3x8_get(nph_vec3x8_splat(a[0]), 7U), a[0]));
    }
}

NTEST(wide, frustum_culls_agree_with_scalar_kernel_and_geometry)
{
    const NphVec3 eye = nph_vec3(3.0F, 4.0F, 5.0F);
    const NphMat4 view = nph_mat4_look_at(eye, nph_vec3_zero(), nph_vec3_unit_y());
    const NphMat4 proj = nph_mat4_perspective(1.2F, 1.6F, 0.5F, 60.0F);
    const NphMat4 clip = nph_mat4_mul(proj, view);
    const NphFrustum f = nph_frustum_from_matrix(&clip);
    const NphMathKernels *scalar = nph_math_kernels(NPH_SIMD_LEVEL_SCALAR);
    u64 rng = 73U;
    u32 visible_total = 0U;
    for (u32 i = 0; i < ITER; ++i) {
        NphAABB boxes[8];
        NphSphere spheres[8];
        NphVec3 c[8];
        NphVec3 e[8];
        f32 r[8];
        for (u32 k = 0; k < 8U; ++k) {
            c[k] = tm_rng_vec3(&rng, -40.0F, 40.0F);
            e[k] = tm_rng_vec3(&rng, 0.1F, 5.0F);
            r[k] = tm_rng_range(&rng, 0.1F, 5.0F);
            boxes[k] = nph_aabb_from_center_extents(c[k], e[k]);
            spheres[k] = nph_sphere(c[k], r[k]);
        }
        // The wide forms take centres and extents computed exactly as the
        // kernels compute them from min/max, so the comparison is bit-exact.
        NphVec3 kc[8];
        NphVec3 ke[8];
        for (u32 k = 0; k < 8U; ++k) {
            kc[k] = nph_vec3_scale(nph_vec3_add(boxes[k].min, boxes[k].max), 0.5F);
            ke[k] = nph_vec3_scale(nph_vec3_sub(boxes[k].max, boxes[k].min), 0.5F);
        }
        u64 want_boxes = 0U;
        u64 want_spheres = 0U;
        scalar->frustum_cull_aabbs(&f, boxes, 8U, &want_boxes);
        scalar->frustum_cull_spheres(&f, spheres, 8U, &want_spheres);
        const u32 got8 =
            nph_frustum_cull_aabb_x8(&f, nph_vec3x8_load_aos(kc), nph_vec3x8_load_aos(ke));
        const u32 got4_lo =
            nph_frustum_cull_aabb_x4(&f, nph_vec3x4_load_aos(kc), nph_vec3x4_load_aos(ke));
        const u32 got4_hi =
            nph_frustum_cull_aabb_x4(&f, nph_vec3x4_load_aos(kc + 4), nph_vec3x4_load_aos(ke + 4));
        NTEST_ASSERT_EQ_UINT(got8, (u32)want_boxes);
        NTEST_ASSERT_EQ_UINT(got4_lo | (got4_hi << 4U), (u32)want_boxes);
        const u32 sph8 = nph_frustum_cull_sphere_x8(&f, nph_vec3x8_load_aos(c), nph_f32x8_load(r));
        const u32 sph4_lo =
            nph_frustum_cull_sphere_x4(&f, nph_vec3x4_load_aos(c), nph_f32x4_load(r));
        const u32 sph4_hi =
            nph_frustum_cull_sphere_x4(&f, nph_vec3x4_load_aos(c + 4), nph_f32x4_load(r + 4));
        NTEST_ASSERT_EQ_UINT(sph8, (u32)want_spheres);
        NTEST_ASSERT_EQ_UINT(sph4_lo | (sph4_hi << 4U), (u32)want_spheres);
        // Away from plane boundaries the kernels agree with geometry.h.
        for (u32 k = 0; k < 8U; ++k) {
            f32 min_margin = NPH_F32_INFINITY;
            for (u32 p = 0; p < f.plane_count; ++p) {
                const NphPlane pl = f.planes[p];
                const f32 reach = nph_vec3_dot(nph_vec3_abs(pl.normal), e[k]);
                min_margin =
                    nph_f32_min(min_margin, nph_f32_abs(nph_plane_distance(pl, c[k]) + reach));
                min_margin =
                    nph_f32_min(min_margin, nph_f32_abs(nph_plane_distance(pl, c[k]) + r[k]));
            }
            if (min_margin < 1e-3F) {
                continue;
            }
            NTEST_ASSERT_TRUE(((got8 >> k) & 1U) ==
                              (nph_frustum_intersects_aabb(&f, boxes[k]) ? 1U : 0U));
            NTEST_ASSERT_TRUE(((sph8 >> k) & 1U) ==
                              (nph_frustum_intersects_sphere(&f, spheres[k]) ? 1U : 0U));
            visible_total += (got8 >> k) & 1U;
        }
    }
    NTEST_ASSERT_GT_UINT(visible_total, 100U);
    NTEST_ASSERT_LT_UINT(visible_total, 8U * ITER - 100U);
}
