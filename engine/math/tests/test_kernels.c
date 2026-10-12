// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/kernels.h>
#include <nepheline/math/pack.h>

#include "test_math_support.h"

#include <string.h>

static const u32 g_counts[] = {0U, 1U, 7U, 8U, 9U, 63U, 64U, 65U, 100U, 1000U, 1031U};

#define MAX_COUNT 1031U

NTEST(kernels, detection_and_tables)
{
    const NphSimdLevel level = nph_simd_detect_level();
    NTEST_LOG("detected SIMD level: %s", nph_simd_level_name(level));
    // Every x86-64 CPU the engine supports has SSE4.2 (CLAUDE.md §3).
    NTEST_ASSERT_GE_INT(level, NPH_SIMD_LEVEL_SSE4);
    NTEST_ASSERT_LT_INT(level, NPH_SIMD_LEVEL_COUNT);
    NTEST_ASSERT_EQ_STR(nph_simd_level_name(NPH_SIMD_LEVEL_SCALAR), "scalar");
    NTEST_ASSERT_EQ_STR(nph_simd_level_name(NPH_SIMD_LEVEL_SSE4), "sse4");
    NTEST_ASSERT_EQ_STR(nph_simd_level_name(NPH_SIMD_LEVEL_AVX2), "avx2");
    NTEST_ASSERT_EQ_STR(nph_simd_level_name(NPH_SIMD_LEVEL_COUNT), "invalid");
    NTEST_ASSERT_EQ_STR(nph_simd_level_name((NphSimdLevel)99), "invalid");
    for (int l = 0; l < NPH_SIMD_LEVEL_COUNT; ++l) {
        const NphMathKernels *k = nph_math_kernels((NphSimdLevel)l);
        NTEST_ASSERT_NOT_NULL(k);
        NTEST_ASSERT_EQ_INT(k->level, l);
        NTEST_ASSERT_EQ_STR(k->name, nph_simd_level_name((NphSimdLevel)l));
        NTEST_ASSERT_NOT_NULL(k->frustum_cull_aabbs);
        NTEST_ASSERT_NOT_NULL(k->frustum_cull_spheres);
        NTEST_ASSERT_NOT_NULL(k->transform_points);
        NTEST_ASSERT_NOT_NULL(k->transform_vectors);
        NTEST_ASSERT_NOT_NULL(k->f32_to_f16);
    }
    NTEST_ASSERT_EQ_INT(nph_math_kernels(NPH_SIMD_LEVEL_COUNT)->level, NPH_SIMD_LEVEL_SCALAR);
    NTEST_ASSERT_EQ_INT(nph_math_kernels((NphSimdLevel)77)->level, NPH_SIMD_LEVEL_SCALAR);
    NTEST_ASSERT_EQ_INT(nph_math_kernels_best()->level, level);
}

// Runs every kernel of `k` against the scalar table on the same inputs and
// requires bit-identical outputs, including the padding bits of the
// visibility words and the tail elements of partial groups.
static void check_level_against_scalar(NphSimdLevel level, u64 seed)
{
    const NphMathKernels *scalar = nph_math_kernels(NPH_SIMD_LEVEL_SCALAR);
    const NphMathKernels *k = nph_math_kernels(level);
    NTEST_LOG("checking %s against scalar", k->name);

    const NphMat4 view =
        nph_mat4_look_at(nph_vec3(2.0F, 3.0F, 9.0F), nph_vec3_zero(), nph_vec3_unit_y());
    const NphMat4 proj = nph_mat4_perspective_infinite(1.1F, 1.5F, 0.3F);
    const NphMat4 clip = nph_mat4_mul(proj, view);
    const NphFrustum f = nph_frustum_from_matrix(&clip);

    static NphAABB boxes[MAX_COUNT];
    static NphSphere spheres[MAX_COUNT];
    static NphVec3 points[MAX_COUNT];
    static NphVec3 want_pts[MAX_COUNT];
    static NphVec3 got_pts[MAX_COUNT];
    static f32 floats[MAX_COUNT];
    static u16 want_h[MAX_COUNT];
    static u16 got_h[MAX_COUNT];
    u64 want_bits[(MAX_COUNT + 63U) / 64U];
    u64 got_bits[(MAX_COUNT + 63U) / 64U];

    u64 rng = seed;
    for (u32 i = 0; i < MAX_COUNT; ++i) {
        const NphVec3 c = tm_rng_vec3(&rng, -30.0F, 30.0F);
        boxes[i] = nph_aabb_from_center_extents(c, tm_rng_vec3(&rng, 0.1F, 4.0F));
        spheres[i] = nph_sphere(c, tm_rng_range(&rng, 0.1F, 4.0F));
        points[i] = tm_rng_vec3(&rng, -100.0F, 100.0F);
        floats[i] = nph_f32_from_bits((u32)tm_rng_next(&rng));
    }
    const NphAffine m = nph_affine_from_mat4(tm_rng_trs_mat4(&rng, NULL, NULL, NULL));

    for (u32 ci = 0; ci < NPH_ARRAY_COUNT(g_counts); ++ci) {
        const u32 count = g_counts[ci];
        const u32 words = (count + 63U) / 64U;
        memset(want_bits, 0xAA, sizeof want_bits);
        memset(got_bits, 0x55, sizeof got_bits);
        scalar->frustum_cull_aabbs(&f, boxes, count, want_bits);
        k->frustum_cull_aabbs(&f, boxes, count, got_bits);
        NTEST_ASSERT_EQ_MEM(got_bits, want_bits, words * sizeof(u64));
        if (count % 64U != 0U) {
            NTEST_ASSERT_EQ_UINT(got_bits[words - 1U] >> (count % 64U), 0U); // padding cleared
        }
        scalar->frustum_cull_spheres(&f, spheres, count, want_bits);
        k->frustum_cull_spheres(&f, spheres, count, got_bits);
        NTEST_ASSERT_EQ_MEM(got_bits, want_bits, words * sizeof(u64));

        scalar->transform_points(&m, points, want_pts, count);
        k->transform_points(&m, points, got_pts, count);
        for (u32 i = 0; i < count; ++i) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(got_pts[i], want_pts[i]));
        }
        scalar->transform_vectors(&m, points, want_pts, count);
        k->transform_vectors(&m, points, got_pts, count);
        for (u32 i = 0; i < count; ++i) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(got_pts[i], want_pts[i]));
        }
        // In place.
        memcpy(got_pts, points, count * sizeof(NphVec3));
        k->transform_points(&m, got_pts, got_pts, count);
        scalar->transform_points(&m, points, want_pts, count);
        for (u32 i = 0; i < count; ++i) {
            NTEST_ASSERT_TRUE(nph_vec3_eq(got_pts[i], want_pts[i]));
        }

        scalar->f32_to_f16(floats, want_h, count);
        k->f32_to_f16(floats, got_h, count);
        for (u32 i = 0; i < count; ++i) {
            if (nph_f32_is_nan(floats[i])) {
                NTEST_ASSERT_TRUE(nph_f32_is_nan(nph_f16_to_f32(got_h[i])));
                NTEST_ASSERT_EQ_UINT(got_h[i] & 0x8000U, want_h[i] & 0x8000U);
            } else {
                NTEST_ASSERT_EQ_UINT(got_h[i], want_h[i]);
            }
        }
    }
    // A sizeable visible fraction so the comparison is not vacuous.
    scalar->frustum_cull_aabbs(&f, boxes, MAX_COUNT, want_bits);
    u32 visible = 0U;
    for (u32 w = 0; w < (MAX_COUNT + 63U) / 64U; ++w) {
        visible += nph_popcount_u64(want_bits[w]);
    }
    NTEST_LOG("%u of %u boxes visible", visible, (u32)MAX_COUNT);
    NTEST_ASSERT_GT_UINT(visible, 50U);
    NTEST_ASSERT_LT_UINT(visible, MAX_COUNT - 50U);
}

NTEST(kernels, sse4_matches_scalar)
{
    check_level_against_scalar(NPH_SIMD_LEVEL_SSE4, 80U);
}

NTEST(kernels, avx2_matches_scalar)
{
    if (nph_simd_detect_level() < NPH_SIMD_LEVEL_AVX2) {
        NTEST_SKIP("this CPU has no AVX2");
    }
    check_level_against_scalar(NPH_SIMD_LEVEL_AVX2, 81U);
}

NTEST(kernels, scalar_cull_agrees_with_geometry_away_from_boundaries)
{
    const NphMat4 proj = nph_mat4_perspective(1.0F, 1.0F, 1.0F, 50.0F);
    const NphFrustum f = nph_frustum_from_matrix(&proj);
    const NphMathKernels *scalar = nph_math_kernels(NPH_SIMD_LEVEL_SCALAR);
    u64 rng = 82U;
    u32 checked = 0U;
    for (u32 i = 0; i < 4000U; ++i) {
        const NphVec3 c =
            nph_vec3(tm_rng_range(&rng, -40.0F, 40.0F), tm_rng_range(&rng, -40.0F, 40.0F),
                     tm_rng_range(&rng, -70.0F, 5.0F));
        const NphVec3 e = tm_rng_vec3(&rng, 0.1F, 5.0F);
        const NphAABB b = nph_aabb_from_center_extents(c, e);
        f32 margin = NPH_F32_INFINITY;
        for (u32 p = 0; p < f.plane_count; ++p) {
            margin =
                nph_f32_min(margin, nph_f32_abs(nph_plane_distance(f.planes[p], c) +
                                                nph_vec3_dot(nph_vec3_abs(f.planes[p].normal), e)));
        }
        if (margin < 1e-3F) {
            continue;
        }
        u64 bit = 0U;
        scalar->frustum_cull_aabbs(&f, &b, 1U, &bit);
        NTEST_ASSERT_EQ_UINT(bit, nph_frustum_intersects_aabb(&f, b) ? 1U : 0U);
        checked += 1U;
    }
    NTEST_ASSERT_GT_UINT(checked, 3000U);
}
