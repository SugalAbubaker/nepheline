// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/affine.h>

#include "test_math_support.h"

#define ITER 1000U

NTEST(affine, constructors_and_accessors)
{
    const NphVec4 r0 = nph_vec4(1.0F, 2.0F, 3.0F, 4.0F);
    const NphVec4 r1 = nph_vec4(5.0F, 6.0F, 7.0F, 8.0F);
    const NphVec4 r2 = nph_vec4(9.0F, 10.0F, 11.0F, 12.0F);
    const NphAffine a = nph_affine_from_rows(r0, r1, r2);
    NTEST_ASSERT_TRUE(nph_vec4_eq(a.r[0], r0) && nph_vec4_eq(a.r[1], r1) &&
                      nph_vec4_eq(a.r[2], r2));
    // Columns are read down the rows; the translation is the last column.
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_affine_column(a, 0U), nph_vec3(1.0F, 5.0F, 9.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_affine_column(a, 2U), nph_vec3(3.0F, 7.0F, 11.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_affine_column(a, 3U), nph_vec3(4.0F, 8.0F, 12.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_affine_translation(a), nph_vec3(4.0F, 8.0F, 12.0F)));
    const NphMat3 l = nph_affine_linear(a);
    NTEST_ASSERT_TRUE(
        nph_mat3_eq(l, nph_mat3_from_rows(nph_vec3(1.0F, 2.0F, 3.0F), nph_vec3(5.0F, 6.0F, 7.0F),
                                          nph_vec3(9.0F, 10.0F, 11.0F))));
    NTEST_ASSERT_TRUE(
        nph_affine_eq(nph_affine_from_mat3_translation(l, nph_vec3(4.0F, 8.0F, 12.0F)), a));
    NTEST_ASSERT_TRUE(
        nph_affine_eq(nph_affine_from_mat3(l), nph_affine_with_translation(a, nph_vec3_zero())));
    NTEST_ASSERT_TRUE(nph_vec3_eq(
        nph_affine_translation(nph_affine_with_translation(a, nph_vec3_one())), nph_vec3_one()));
    // Round trip through NphMat4 and its implied last row.
    const NphMat4 m = nph_affine_to_mat4(a);
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_mat4_row(m, 0U), r0));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_mat4_row(m, 3U), nph_vec4(0.0F, 0.0F, 0.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_affine_eq(nph_affine_from_mat4(m), a));
    NTEST_ASSERT_TRUE(nph_mat4_eq(nph_affine_to_mat4(nph_affine_identity()), nph_mat4_identity()));
    NTEST_ASSERT_TRUE(
        nph_affine_eq(nph_affine_from_translation(nph_vec3(1.0F, 2.0F, 3.0F)),
                      nph_affine_from_mat4(nph_mat4_from_translation(nph_vec3(1.0F, 2.0F, 3.0F)))));
    NTEST_ASSERT_TRUE(
        nph_affine_eq(nph_affine_from_scale(nph_vec3(1.0F, 2.0F, 3.0F)),
                      nph_affine_from_mat4(nph_mat4_from_scale(nph_vec3(1.0F, 2.0F, 3.0F)))));
    const NphQuat q = nph_quat_from_rotation_x(0.9F);
    NTEST_ASSERT_TRUE(
        nph_affine_eq(nph_affine_from_quat(q), nph_affine_from_mat4(nph_mat4_from_quat(q))));
    NTEST_ASSERT_TRUE(nph_affine_near(a, a, 0.0F));
    NTEST_ASSERT_FALSE(nph_affine_near(a, nph_affine_identity(), 1e-6F));
    NTEST_ASSERT_FALSE(nph_affine_eq(a, nph_affine_identity()));
}

NTEST(affine, agrees_with_mat4)
{
    u64 rng = 30U;
    for (u32 i = 0; i < ITER; ++i) {
        NphVec3 t;
        NphQuat r;
        NphVec3 s;
        const NphMat4 ma = tm_rng_trs_mat4(&rng, &t, &r, &s);
        const NphMat4 mb = tm_rng_trs_mat4(&rng, NULL, NULL, NULL);
        const NphAffine a = nph_affine_from_mat4(ma);
        const NphAffine b = nph_affine_from_mat4(mb);
        TM_ASSERT_AFFINE_NEAR(nph_affine_from_trs(t, r, s), a, 0.0F);
        // Products and transforms match the 4x4 versions.
        TM_ASSERT_AFFINE_NEAR(nph_affine_mul(a, b), nph_affine_from_mat4(nph_mat4_mul(ma, mb)),
                              1e-4F);
        const NphVec3 p = tm_rng_vec3(&rng, -5.0F, 5.0F);
        TM_ASSERT_VEC3_NEAR(nph_affine_transform_point(a, p), nph_mat4_transform_point(ma, p),
                            1e-4F);
        TM_ASSERT_VEC3_NEAR(nph_affine_transform_vector(a, p), nph_mat4_transform_vector(ma, p),
                            1e-4F);
        TM_ASSERT_VEC3_NEAR(nph_affine_transform_point(nph_affine_mul(a, b), p),
                            nph_affine_transform_point(a, nph_affine_transform_point(b, p)), 1e-3F);
        TM_ASSERT_AFFINE_NEAR(nph_affine_mul(a, nph_affine_identity()), a, 0.0F);
        TM_ASSERT_AFFINE_NEAR(nph_affine_mul(nph_affine_identity(), a), a, 0.0F);
        // Decomposition recovers the factors.
        NphVec3 t2;
        NphQuat r2;
        NphVec3 s2;
        NTEST_ASSERT_TRUE(nph_affine_decompose_trs(a, &t2, &r2, &s2));
        TM_ASSERT_VEC3_NEAR(t2, t, 0.0F);
        TM_ASSERT_VEC3_NEAR(s2, s, 1e-5F);
        TM_ASSERT_ROTATION_NEAR(r2, r, 1e-4F);
    }
}

NTEST(affine, inverse)
{
    NphAffine inv = nph_affine_identity();
    NTEST_ASSERT_FALSE(nph_affine_inverse(nph_affine_from_scale(nph_vec3(1.0F, 0.0F, 1.0F)), &inv));
    NTEST_ASSERT_TRUE(nph_affine_eq(inv, nph_affine_identity())); // untouched
    u64 rng = 31U;
    for (u32 i = 0; i < ITER; ++i) {
        NphVec3 t;
        NphQuat r;
        const NphMat4 m = tm_rng_trs_mat4(&rng, &t, &r, NULL);
        const NphAffine a = nph_affine_from_mat4(m);
        NTEST_ASSERT_TRUE(nph_affine_inverse(a, &inv));
        TM_ASSERT_AFFINE_NEAR(nph_affine_mul(inv, a), nph_affine_identity(), 1e-5F);
        TM_ASSERT_AFFINE_NEAR(nph_affine_mul(a, inv), nph_affine_identity(), 1e-5F);
        NphMat4 minv;
        NTEST_ASSERT_TRUE(nph_mat4_inverse(m, &minv));
        TM_ASSERT_AFFINE_NEAR(inv, nph_affine_from_mat4(minv), 1e-4F);
        // Rigid transforms invert by transposition.
        const NphAffine rigid = nph_affine_from_trs(t, r, nph_vec3_one());
        NphAffine rigid_inv;
        NTEST_ASSERT_TRUE(nph_affine_inverse(rigid, &rigid_inv));
        TM_ASSERT_AFFINE_NEAR(nph_affine_inverse_rigid(rigid), rigid_inv, 1e-5F);
        TM_ASSERT_AFFINE_NEAR(nph_affine_mul(nph_affine_inverse_rigid(rigid), rigid),
                              nph_affine_identity(), 1e-5F);
    }
}
