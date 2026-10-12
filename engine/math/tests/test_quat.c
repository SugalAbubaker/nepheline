// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/mat.h>
#include <nepheline/math/quat.h>

#include "test_math_support.h"

#define ITER 2000U

NTEST(quat, constructors_and_arithmetic)
{
    const NphQuat a = nph_quat(1.0F, 2.0F, 3.0F, 4.0F);
    const NphQuat b = nph_quat(-1.0F, 0.5F, 2.0F, -4.0F);
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_identity(), nph_quat(0.0F, 0.0F, 0.0F, 1.0F)));
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_from_vec4(nph_vec4(1.0F, 2.0F, 3.0F, 4.0F)), a));
    NTEST_ASSERT_TRUE(nph_vec4_eq(nph_quat_to_vec4(a), nph_vec4(1.0F, 2.0F, 3.0F, 4.0F)));
    NTEST_ASSERT_TRUE(nph_vec3_eq(nph_quat_xyz(a), nph_vec3(1.0F, 2.0F, 3.0F)));
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_from_xyz_w(nph_vec3(1.0F, 2.0F, 3.0F), 4.0F), a));
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_add(a, b), nph_quat(0.0F, 2.5F, 5.0F, 0.0F)));
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_sub(a, b), nph_quat(2.0F, 1.5F, 1.0F, 8.0F)));
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_scale(a, 2.0F), nph_quat(2.0F, 4.0F, 6.0F, 8.0F)));
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_neg(a), nph_quat(-1.0F, -2.0F, -3.0F, -4.0F)));
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_conjugate(a), nph_quat(-1.0F, -2.0F, -3.0F, 4.0F)));
    NTEST_ASSERT_ULP_F32(nph_quat_dot(a, b), -10.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_quat_length_sq(a), 30.0F, 0U);
    NTEST_ASSERT_ULP_F32(nph_quat_length(nph_quat(0.0F, 0.0F, 3.0F, 4.0F)), 5.0F, 0U);
    NTEST_ASSERT_TRUE(nph_quat_near(a, nph_quat(1.0F, 2.0F, 3.0F, 4.0F + 1e-7F), 1e-6F));
    NTEST_ASSERT_FALSE(nph_quat_near(a, b, 1e-6F));
    NTEST_ASSERT_FALSE(nph_quat_eq(a, b));
    NTEST_ASSERT_TRUE(nph_quat_near_rotation(a, nph_quat_neg(a), 0.0F));
    NTEST_ASSERT_TRUE(nph_quat_near_rotation(a, a, 0.0F));
    NTEST_ASSERT_FALSE(nph_quat_near_rotation(a, b, 1e-6F));
}

NTEST(quat, multiplication_algebra)
{
    u64 rng = 10U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat a = tm_rng_unit_quat(&rng);
        const NphQuat b = tm_rng_unit_quat(&rng);
        const NphQuat c = tm_rng_unit_quat(&rng);
        TM_ASSERT_QUAT_NEAR(nph_quat_mul(a, nph_quat_identity()), a, 0.0F);
        TM_ASSERT_QUAT_NEAR(nph_quat_mul(nph_quat_identity(), a), a, 0.0F);
        // Associative, norm-multiplicative, unit-preserving.
        TM_ASSERT_QUAT_NEAR(nph_quat_mul(nph_quat_mul(a, b), c),
                            nph_quat_mul(a, nph_quat_mul(b, c)), 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_quat_length(nph_quat_mul(a, b)), 1.0F, 1e-5F);
        // q * conj(q) == identity for unit q.
        TM_ASSERT_QUAT_NEAR(nph_quat_mul(a, nph_quat_conjugate(a)), nph_quat_identity(), 1e-5F);
    }
    // Not commutative: i * j == k but j * i == -k.
    const NphQuat qi = nph_quat(1.0F, 0.0F, 0.0F, 0.0F);
    const NphQuat qj = nph_quat(0.0F, 1.0F, 0.0F, 0.0F);
    const NphQuat qk = nph_quat(0.0F, 0.0F, 1.0F, 0.0F);
    TM_ASSERT_QUAT_NEAR(nph_quat_mul(qi, qj), qk, 0.0F);
    TM_ASSERT_QUAT_NEAR(nph_quat_mul(qj, qi), nph_quat_neg(qk), 0.0F);
    TM_ASSERT_QUAT_NEAR(nph_quat_mul(qi, qi), nph_quat(0.0F, 0.0F, 0.0F, -1.0F), 0.0F);
}

NTEST(quat, inverse_and_normalize)
{
    u64 rng = 11U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat q = nph_quat_from_vec4(tm_rng_vec4(&rng, -3.0F, 3.0F));
        if (nph_quat_length_sq(q) < 0.1F) {
            continue;
        }
        TM_ASSERT_QUAT_NEAR(nph_quat_mul(q, nph_quat_inverse(q)), nph_quat_identity(), 1e-5F);
        TM_ASSERT_QUAT_NEAR(nph_quat_mul(nph_quat_inverse(q), q), nph_quat_identity(), 1e-5F);
        const NphQuat n = nph_quat_normalize(q);
        // Four squares, a sum, a sqrt, a divide, four products, then the
        // length again: about 3 ULPs of accumulated rounding.
        NTEST_ASSERT_ULP_F32(nph_quat_length(n), 1.0F, 4U);
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(n, 1e-6F));
        TM_ASSERT_QUAT_NEAR(nph_quat_inverse(n), nph_quat_conjugate(n), 1e-6F);
        TM_ASSERT_QUAT_NEAR(nph_quat_normalize_or_identity(q), n, 0.0F);
    }
    NTEST_ASSERT_TRUE(nph_quat_eq(nph_quat_normalize_or_identity(nph_quat(0.0F, 0.0F, 0.0F, 0.0F)),
                                  nph_quat_identity()));
    NTEST_ASSERT_FALSE(nph_quat_is_normalized(nph_quat(1.0F, 1.0F, 0.0F, 0.0F), 1e-6F));
    TM_EXPECT_ASSERT((void)nph_quat_normalize(nph_quat(0.0F, 0.0F, 0.0F, 0.0F)));
    TM_EXPECT_ASSERT((void)nph_quat_inverse(nph_quat(0.0F, 0.0F, 0.0F, 0.0F)));
}

NTEST(quat, axis_angle)
{
    // A quarter turn about +Z takes +X to +Y (right-handed).
    const NphQuat q = nph_quat_from_axis_angle(nph_vec3_unit_z(), NPH_HALF_PI);
    TM_ASSERT_VEC3_NEAR(nph_quat_rotate(q, nph_vec3_unit_x()), nph_vec3_unit_y(), 1e-6F);
    TM_ASSERT_QUAT_NEAR(nph_quat_from_rotation_x(0.7F),
                        nph_quat_from_axis_angle(nph_vec3_unit_x(), 0.7F), 0.0F);
    TM_ASSERT_QUAT_NEAR(nph_quat_from_rotation_y(0.7F),
                        nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.7F), 0.0F);
    TM_ASSERT_QUAT_NEAR(nph_quat_from_rotation_z(0.7F),
                        nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.7F), 0.0F);

    NphVec3 axis;
    f32 angle;
    nph_quat_to_axis_angle(nph_quat_identity(), &axis, &angle);
    TM_ASSERT_VEC3_NEAR(axis, nph_vec3_unit_x(), 0.0F);
    NTEST_ASSERT_ULP_F32(angle, 0.0F, 0U);

    u64 rng = 12U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 ax = tm_rng_unit_vec3(&rng);
        const f32 an = tm_rng_range(&rng, 0.01F, NPH_PI - 0.01F);
        const NphQuat r = nph_quat_from_axis_angle(ax, an);
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(r, 1e-6F));
        nph_quat_to_axis_angle(r, &axis, &angle);
        TM_ASSERT_VEC3_NEAR(axis, ax, 1e-4F);
        NTEST_ASSERT_NEAR_F32(angle, an, 1e-5F);
        // -q is the same rotation and decomposes identically.
        nph_quat_to_axis_angle(nph_quat_neg(r), &axis, &angle);
        TM_ASSERT_VEC3_NEAR(axis, ax, 1e-4F);
        NTEST_ASSERT_NEAR_F32(angle, an, 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_quat_angle(r), an, 1e-4F);
    }
}

NTEST(quat, rotate_matches_matrix_and_composition)
{
    u64 rng = 13U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat a = tm_rng_unit_quat(&rng);
        const NphQuat b = tm_rng_unit_quat(&rng);
        const NphVec3 v = tm_rng_vec3(&rng, -5.0F, 5.0F);
        const NphMat3 m = nph_mat3_from_quat(a);
        TM_ASSERT_VEC3_NEAR(nph_quat_rotate(a, v), nph_mat3_mul_vec3(m, v), 1e-4F);
        // Rotation preserves length and is undone by the inverse.
        NTEST_ASSERT_NEAR_F32(nph_vec3_length(nph_quat_rotate(a, v)), nph_vec3_length(v), 1e-4F);
        TM_ASSERT_VEC3_NEAR(nph_quat_rotate_inverse(a, nph_quat_rotate(a, v)), v, 1e-4F);
        // mul(a, b) applies b first.
        TM_ASSERT_VEC3_NEAR(nph_quat_rotate(nph_quat_mul(a, b), v),
                            nph_quat_rotate(a, nph_quat_rotate(b, v)), 1e-4F);
        // The axis functions are the rotated basis vectors.
        TM_ASSERT_VEC3_NEAR(nph_quat_axis_x(a), nph_quat_rotate(a, nph_vec3_unit_x()), 1e-6F);
        TM_ASSERT_VEC3_NEAR(nph_quat_axis_y(a), nph_quat_rotate(a, nph_vec3_unit_y()), 1e-6F);
        TM_ASSERT_VEC3_NEAR(nph_quat_axis_z(a), nph_quat_rotate(a, nph_vec3_unit_z()), 1e-6F);
    }
    TM_ASSERT_VEC3_NEAR(nph_quat_rotate(nph_quat_identity(), nph_vec3(1.0F, 2.0F, 3.0F)),
                        nph_vec3(1.0F, 2.0F, 3.0F), 0.0F);
}

NTEST(quat, from_to)
{
    TM_ASSERT_QUAT_NEAR(nph_quat_from_to(nph_vec3_unit_x(), nph_vec3_unit_x()), nph_quat_identity(),
                        0.0F);
    const NphQuat flip = nph_quat_from_to(nph_vec3_unit_x(), nph_vec3(-1.0F, 0.0F, 0.0F));
    NTEST_ASSERT_TRUE(nph_quat_is_normalized(flip, 1e-6F));
    NTEST_ASSERT_NEAR_F32(nph_quat_angle(flip), NPH_PI, 1e-5F);
    TM_ASSERT_VEC3_NEAR(nph_quat_rotate(flip, nph_vec3_unit_x()), nph_vec3(-1.0F, 0.0F, 0.0F),
                        1e-6F);
    u64 rng = 14U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 from = tm_rng_unit_vec3(&rng);
        const NphVec3 to = tm_rng_unit_vec3(&rng);
        const NphQuat q = nph_quat_from_to(from, to);
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(q, 1e-5F));
        TM_ASSERT_VEC3_NEAR(nph_quat_rotate(q, from), to, 2e-5F);
        // Shortest arc: the rotation angle equals the angle between the vectors.
        NTEST_ASSERT_NEAR_F32(nph_quat_angle(q), nph_vec3_angle(from, to), 2e-4F);
    }
}

NTEST(quat, angle_between)
{
    u64 rng = 15U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat a = tm_rng_unit_quat(&rng);
        const NphVec3 ax = tm_rng_unit_vec3(&rng);
        const f32 an = tm_rng_range(&rng, 0.0F, NPH_PI);
        const NphQuat b = nph_quat_mul(nph_quat_from_axis_angle(ax, an), a);
        NTEST_ASSERT_NEAR_F32(nph_quat_angle_between(a, b), an, 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_quat_angle_between(a, nph_quat_neg(b)), an, 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_quat_angle_between(a, a), 0.0F, 0.0F);
        NTEST_ASSERT_NEAR_F32(nph_quat_angle_between(a, nph_quat_neg(a)), 0.0F, 0.0F);
        // Small angles keep their precision (the acos formulation would not).
        const NphQuat tiny = nph_quat_mul(nph_quat_from_axis_angle(ax, 1e-4F), a);
        NTEST_ASSERT_NEAR_F32(nph_quat_angle_between(a, tiny), 1e-4F, 1e-6F);
        NTEST_ASSERT_NEAR_F32(nph_quat_angle(nph_quat_from_axis_angle(ax, 1e-4F)), 1e-4F, 2e-9F);
    }
}

NTEST(quat, nlerp)
{
    u64 rng = 16U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat a = tm_rng_unit_quat(&rng);
        const NphQuat b = tm_rng_unit_quat(&rng);
        TM_ASSERT_QUAT_NEAR(nph_quat_nlerp(a, b, 0.0F), a, 1e-6F);
        TM_ASSERT_ROTATION_NEAR(nph_quat_nlerp(a, b, 1.0F), b, 1e-6F);
        const NphQuat h = nph_quat_nlerp(a, b, 0.5F);
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(h, 1e-5F));
        // Shortest arc: the midpoint is within the small angle of both ends.
        NTEST_ASSERT_TRUE(nph_quat_angle_between(a, h) <=
                          0.5F * nph_quat_angle_between(a, b) + 1e-3F);
    }
}

NTEST(quat, slerp_constant_angular_speed)
{
    u64 rng = 17U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat a = tm_rng_unit_quat(&rng);
        const NphQuat b = tm_rng_unit_quat(&rng);
        const f32 total = nph_quat_angle_between(a, b);
        TM_ASSERT_QUAT_NEAR(nph_quat_slerp(a, b, 0.0F), a, 1e-5F);
        TM_ASSERT_ROTATION_NEAR(nph_quat_slerp(a, b, 1.0F), b, 1e-5F);
        for (u32 k = 1; k < 4U; ++k) {
            const f32 t = (f32)k * 0.25F;
            const NphQuat s = nph_quat_slerp(a, b, t);
            NTEST_ASSERT_TRUE(nph_quat_is_normalized(s, 1e-5F));
            NTEST_ASSERT_NEAR_F32(nph_quat_angle_between(a, s), t * total, 1e-5F);
            NTEST_ASSERT_NEAR_F32(nph_quat_angle_between(s, b), (1.0F - t) * total, 1e-5F);
        }
        // Flipping b's sign (same rotation) changes nothing.
        TM_ASSERT_ROTATION_NEAR(nph_quat_slerp(a, nph_quat_neg(b), 0.3F),
                                nph_quat_slerp(a, b, 0.3F), 1e-5F);
    }
    // Nearly equal inputs take the nlerp path and stay finite.
    const NphQuat q = nph_quat_normalize(nph_quat(0.1F, 0.2F, 0.3F, 0.9F));
    const NphQuat r = nph_quat_normalize(nph_quat_add(q, nph_quat(1e-5F, 0.0F, 0.0F, 0.0F)));
    const NphQuat s = nph_quat_slerp(q, r, 0.5F);
    NTEST_ASSERT_TRUE(nph_quat_is_normalized(s, 1e-6F));
    TM_ASSERT_QUAT_NEAR(s, q, 1e-4F);
    TM_ASSERT_QUAT_NEAR(nph_quat_slerp(q, q, 0.7F), q, 1e-6F);
}

NTEST(quat, swing_twist)
{
    u64 rng = 18U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphQuat q = tm_rng_unit_quat(&rng);
        const NphVec3 axis = tm_rng_unit_vec3(&rng);
        NphQuat swing;
        NphQuat twist;
        nph_quat_swing_twist(q, axis, &swing, &twist);
        // q == swing * twist
        TM_ASSERT_QUAT_NEAR(nph_quat_mul(swing, twist), q, 1e-5F);
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(swing, 1e-5F));
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(twist, 1e-5F));
        // twist is about the axis; swing is about something perpendicular.
        NTEST_ASSERT_NEAR_F32(nph_vec3_length(nph_vec3_cross(nph_quat_xyz(twist), axis)), 0.0F,
                              1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(nph_quat_xyz(swing), axis), 0.0F, 1e-5F);
        // The twist leaves the axis itself where it is.
        TM_ASSERT_VEC3_NEAR(nph_quat_rotate(twist, axis), axis, 1e-5F);
    }
    // A pure twist decomposes into identity swing.
    const NphQuat tw = nph_quat_from_axis_angle(nph_vec3_unit_y(), 1.1F);
    NphQuat swing;
    NphQuat twist;
    nph_quat_swing_twist(tw, nph_vec3_unit_y(), &swing, &twist);
    TM_ASSERT_ROTATION_NEAR(twist, tw, 1e-6F);
    TM_ASSERT_ROTATION_NEAR(swing, nph_quat_identity(), 1e-6F);
    // A half turn perpendicular to the axis is pure swing.
    const NphQuat half = nph_quat_from_axis_angle(nph_vec3_unit_x(), NPH_PI);
    nph_quat_swing_twist(half, nph_vec3_unit_z(), &swing, &twist);
    TM_ASSERT_QUAT_NEAR(twist, nph_quat_identity(), 0.0F);
    TM_ASSERT_ROTATION_NEAR(swing, half, 1e-6F);
}

NTEST(quat, euler)
{
    TM_ASSERT_QUAT_NEAR(nph_quat_from_euler(nph_vec3(0.4F, 0.0F, 0.0F)),
                        nph_quat_from_rotation_x(0.4F), 1e-7F);
    TM_ASSERT_QUAT_NEAR(nph_quat_from_euler(nph_vec3(0.0F, 0.4F, 0.0F)),
                        nph_quat_from_rotation_y(0.4F), 1e-7F);
    TM_ASSERT_QUAT_NEAR(nph_quat_from_euler(nph_vec3(0.0F, 0.0F, 0.4F)),
                        nph_quat_from_rotation_z(0.4F), 1e-7F);
    // X first, then Y, then Z about the world axes.
    const NphVec3 e = nph_vec3(0.3F, -0.6F, 1.2F);
    const NphQuat expected =
        nph_quat_mul(nph_quat_from_rotation_z(e.z),
                     nph_quat_mul(nph_quat_from_rotation_y(e.y), nph_quat_from_rotation_x(e.x)));
    TM_ASSERT_QUAT_NEAR(nph_quat_from_euler(e), expected, 1e-6F);

    u64 rng = 19U;
    for (u32 i = 0; i < ITER; ++i) {
        const NphVec3 angles =
            nph_vec3(tm_rng_range(&rng, -3.1F, 3.1F), tm_rng_range(&rng, -1.4F, 1.4F),
                     tm_rng_range(&rng, -3.1F, 3.1F));
        const NphQuat q = nph_quat_from_euler(angles);
        const NphVec3 back = nph_quat_to_euler(q);
        TM_ASSERT_VEC3_NEAR(back, angles, 1e-4F);
        TM_ASSERT_ROTATION_NEAR(nph_quat_from_euler(back), q, 1e-5F);
    }
    // At the poles the x/z split is ambiguous, but the rotation round-trips.
    const NphVec3 pole = nph_vec3(0.3F, NPH_HALF_PI, 0.2F);
    const NphQuat qp = nph_quat_from_euler(pole);
    const NphVec3 ep = nph_quat_to_euler(qp);
    NTEST_ASSERT_NEAR_F32(ep.y, NPH_HALF_PI, 1e-3F);
    NTEST_ASSERT_ULP_F32(ep.z, 0.0F, 0U);
    TM_ASSERT_ROTATION_NEAR(nph_quat_from_euler(ep), qp, 1e-4F);
    const NphVec3 pole2 = nph_vec3(-0.3F, -NPH_HALF_PI, 0.9F);
    const NphQuat qp2 = nph_quat_from_euler(pole2);
    TM_ASSERT_ROTATION_NEAR(nph_quat_from_euler(nph_quat_to_euler(qp2)), qp2, 1e-4F);
}

NTEST(quat, integrate)
{
    // Constant angular velocity about Z for one second in small steps gives
    // the closed-form rotation.
    const NphVec3 omega = nph_vec3(0.0F, 0.0F, 2.0F);
    NphQuat q = nph_quat_identity();
    for (u32 i = 0; i < 1000U; ++i) {
        q = nph_quat_integrate(q, omega, 1e-3F);
        NTEST_ASSERT_TRUE(nph_quat_is_normalized(q, 1e-5F));
    }
    TM_ASSERT_ROTATION_NEAR(q, nph_quat_from_rotation_z(2.0F), 2e-3F);
    const NphQuat r = nph_quat_normalize(nph_quat(0.3F, -0.2F, 0.5F, 0.7F));
    TM_ASSERT_QUAT_NEAR(nph_quat_integrate(r, nph_vec3_zero(), 0.1F), r, 1e-6F);
}
