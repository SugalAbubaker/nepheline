// SPDX-License-Identifier: Apache-2.0
// Joints: descriptions, a pendulum against its closed-form period, chains
// (hanging, whipping, carrying a body 100 times heavier), hinge axes,
// limits, motors, and springs, a welded cantilever, ball cones, twists, and
// friction, collision between joined bodies, destruction, sleep, kinematic
// carriers, reactions, and determinism across worker counts.

#include <nepheline/platform/cpu.h>

#include "test_world_support.h"

// ---------------------------------------------------------------------------
// Descriptions and lifetime
// ---------------------------------------------------------------------------

NTEST(joint, create_and_reject_bad_descriptions)
{
    Scene s;
    scene_begin(&s);
    const NphPhysicsBody ground = add_anchor(s.w, nph_dvec3_zero());
    const NphPhysicsBody wall = add_anchor(s.w, nph_dvec3(5.0, 0.0, 0.0));
    const NphPhysicsBody box = add_box(s.w, nph_dvec3(0.0, 1.0, 0.0), nph_vec3_splat(0.5F));
    NphPhysicsJoint j;
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_BALL, ground, box);
    // Two bodies neither of which is dynamic; one body twice.
    d.body_b = wall;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_INVALID_ARGUMENT);
    d.body_b = ground;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_INVALID_ARGUMENT);
    d.body_b = box;
    // A frame that is not a rotation; limits out of range.
    d.frame_a.rotation = nph_quat(0.0F, 0.0F, 0.0F, 2.0F);
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_INVALID_ARGUMENT);
    d.frame_a.rotation = nph_quat_identity();
    d.ball.enable_swing_limit = 1;
    d.ball.swing_y = 0.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_INVALID_ARGUMENT);
    d.ball.swing_y = 0.5F;
    d.ball.enable_twist_limit = 1;
    d.ball.twist_lower = 0.5F;
    d.ball.twist_upper = -0.5F;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_INVALID_ARGUMENT);
    d.ball.twist_lower = -0.5F;
    d.ball.twist_upper = 0.5F;
    d.ball.friction_torque = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_INVALID_ARGUMENT);
    d.ball.friction_torque = 0.0F;
    NphPhysicsJointDesc h = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, ground, box);
    h.hinge.enable_spring = 1;
    h.hinge.spring_hertz = 0.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &h, &j), NPH_RESULT_INVALID_ARGUMENT);
    NphPhysicsJointDesc sl = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_SLIDER, ground, box);
    sl.slider.enable_limit = 1;
    sl.slider.lower = 1.0F;
    sl.slider.upper = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &sl, &j), NPH_RESULT_INVALID_ARGUMENT);
    NphPhysicsJointDesc di =
        nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_DISTANCE, ground, box);
    di.distance.length = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &di, &j), NPH_RESULT_INVALID_ARGUMENT);
    di.distance.length = 1.0F;
    di.distance.enable_limit = 1;
    di.distance.min_length = 2.0F;
    di.distance.max_length = 1.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &di, &j), NPH_RESULT_INVALID_ARGUMENT);
    NphPhysicsJointDesc six = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_D6, ground, box);
    six.d6.axes[NPH_PHYSICS_D6_SWING_Y].motion = NPH_PHYSICS_AXIS_LIMITED;
    six.d6.axes[NPH_PHYSICS_D6_SWING_Y].upper = k_pi;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &six, &j), NPH_RESULT_INVALID_ARGUMENT);
    six.d6.axes[NPH_PHYSICS_D6_SWING_Y].upper = 0.5F;
    // An axis motion out of range, as a corrupted description has.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    six.d6.axes[NPH_PHYSICS_D6_LINEAR_Y].motion = (NphPhysicsAxisMotion)7;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &six, &j), NPH_RESULT_INVALID_ARGUMENT);
    // A stale body.
    const NphPhysicsBody gone = add_box(s.w, nph_dvec3(3.0, 1.0, 0.0), nph_vec3_splat(0.5F));
    nph_physics_body_destroy(s.w, gone);
    NphPhysicsJointDesc stale = d;
    stale.body_b = gone;
    NTEST_EXPECT_EQ_INT(nph_physics_joint_create(s.w, &stale, &j), NPH_RESULT_NOT_FOUND);
    // A good one: listed on both bodies, its description kept.
    d.user_data = 77U;
    NTEST_ASSERT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_physics_joint_is_valid(s.w, j));
    NTEST_EXPECT_EQ_UINT(nph_physics_joint_user_data(s.w, j), 77U);
    NphPhysicsJoint listed[2];
    NTEST_EXPECT_EQ_UINT(nph_physics_body_joints(s.w, box, listed, 2U), 1U);
    NTEST_EXPECT_EQ_UINT(listed[0].value, j.value);
    NTEST_EXPECT_EQ_UINT(nph_physics_body_joints(s.w, ground, NULL, 0U), 1U);
    const NphPhysicsJointDesc back = nph_physics_joint_desc(s.w, j);
    NTEST_EXPECT_EQ_INT((i32)back.type, (i32)NPH_PHYSICS_JOINT_BALL);
    NTEST_EXPECT_EQ_UINT(back.body_b.value, box.value);
    NTEST_EXPECT_EQ_INT(back.ball.enable_twist_limit, 1);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).joints, 1U);
    nph_physics_joint_destroy(s.w, j);
    NTEST_EXPECT_FALSE(nph_physics_joint_is_valid(s.w, j));
    NTEST_EXPECT_EQ_UINT(nph_physics_body_joints(s.w, box, NULL, 0U), 0U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).joints, 0U);
    scene_end(&s);
}

NTEST(joint, frames_at_start_the_joint_satisfied)
{
    Scene s;
    scene_begin(&s);
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    NphPcg32 rng = test_rng(3U);
    const NphPhysicsBody a =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(1.0, 2.0, 3.0), test_rotation(&rng),
                 nph_physics_geometry_box(nph_vec3(0.3F, 0.4F, 0.5F)), 0.6F, 0.0F);
    const NphPhysicsBody b =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(1.0, 3.5, 3.2), test_rotation(&rng),
                 nph_physics_geometry_box(nph_vec3(0.2F, 0.3F, 0.2F)), 0.6F, 0.0F);
    const NphDVec3 anchor = nph_dvec3(1.1, 2.8, 3.1);
    const NphPhysicsJoint j =
        join(s.w, nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_FIXED, a, b), anchor,
             nph_vec3_normalize(nph_vec3(1.0F, 2.0F, -1.0F)));
    NphDVec3 pa;
    NphDVec3 pb;
    anchors(s.w, j, &pa, &pb);
    NTEST_EXPECT_TRUE(dist(pa, anchor) < 1e-5 && dist(pb, anchor) < 1e-5);
    NphVec3 xa;
    NphVec3 xb;
    frame_axes(s.w, j, &xa, &xb);
    NTEST_EXPECT_TRUE(angle_between(xa, xb) < 1e-3F);
    // Satisfied, at rest, without gravity: nothing moves.
    const NphDVec3 a0 = nph_physics_body_position(s.w, a);
    step(s.w, NULL, 120U);
    NTEST_EXPECT_TRUE(dist(nph_physics_body_position(s.w, a), a0) < 1e-5);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Ball joints
// ---------------------------------------------------------------------------

NTEST(joint, a_pendulum_swings_at_its_period)
{
    Scene s;
    scene_begin(&s);
    // A small ball on a 1 m arm released at 5 degrees: a physical pendulum,
    // T = 2 pi sqrt((2/5 r^2 + L^2) / (g L)) (1 + theta^2 / 16).
    const f64 length = 1.0;
    const f64 r = 0.05;
    const f64 theta = 5.0 * 3.141592653589793 / 180.0;
    const NphDVec3 pivot = nph_dvec3(0.0, 5.0, 0.0);
    const NphPhysicsBody top = add_anchor(s.w, pivot);
    const NphPhysicsBody bob =
        add_dynamic(s.w, nph_dvec3(length * sin(theta), 5.0 - length * cos(theta), 0.0),
                    nph_physics_geometry_sphere((f32)r));
    const NphPhysicsJoint j = ball_at(s.w, top, bob, pivot);
    const f64 expected = 2.0 * 3.141592653589793 *
                         sqrt((0.4 * r * r + length * length) / (9.81 * length)) *
                         (1.0 + theta * theta / 16.0);
    f64 prev_x = nph_physics_body_position(s.w, bob).x;
    f64 crossings[8];
    u32 crossed = 0U;
    f64 worst_gap = 0.0;
    f64 last_peak = 0.0;
    for (u32 i = 1U; i <= 240U * 10U && crossed < 8U; ++i) {
        step(s.w, NULL, 1U);
        const f64 x = nph_physics_body_position(s.w, bob).x;
        if (crossed >= 4U) {
            last_peak = fmax(last_peak, x); // the swing after four periods
        }
        if (prev_x > 0.0 && x <= 0.0) {
            // Interpolate the moment of crossing within the step.
            crossings[crossed++] = ((f64)i - 1.0 + prev_x / (prev_x - x)) / 240.0;
        }
        prev_x = x;
        worst_gap = fmax(worst_gap, joint_gap(s.w, j));
    }
    NTEST_ASSERT_TRUE(crossed >= 5U);
    const f64 period = (crossings[crossed - 1U] - crossings[0]) / (f64)(crossed - 1U);
    NTEST_LOG("period %.5f s against %.5f (%.3f %%); worst anchor gap %.3f mm", period, expected,
              100.0 * (period / expected - 1.0), worst_gap * 1e3);
    NTEST_EXPECT_NEAR_F64(period, expected, 0.003 * expected);
    NTEST_EXPECT_TRUE(worst_gap < 1e-4);
    // It keeps its swing: four periods on, within 2 % of the release.
    NTEST_LOG("amplitude after four periods: %.4f of the release",
              last_peak / (length * sin(theta)));
    NTEST_EXPECT_TRUE(last_peak > 0.98 * length * sin(theta));
    scene_end(&s);
}

NTEST(joint, a_chain_hangs_without_stretching)
{
    Scene s;
    scene_begin(&s);
    enum {
        LINKS = 20
    };
    NphPhysicsBody links[LINKS];
    NphPhysicsJoint joints[LINKS];
    const NphDVec3 top = nph_dvec3(0.0, 12.0, 0.0);
    build_chain(s.w, add_anchor(s.w, top), top, LINKS, false, links, joints);
    step(s.w, NULL, 240U * 3U);
    const NphDVec3 last = nph_physics_body_position(s.w, links[LINKS - 1]);
    const f64 ideal = 12.0 - 0.5 * ((f64)LINKS - 0.5);
    NTEST_LOG("the bottom link hangs %.3f mm low", (ideal - last.y) * 1e3);
    // Each joint gives g / omega^2 (4.3 um at 240 Hz) per body weight it
    // carries over its effective mass, about half a link's: 1.8 mm in all.
    NTEST_EXPECT_NEAR_F64(last.y, ideal, 0.003);
    NTEST_EXPECT_NEAR_F64(last.x, 0.0, 1e-4);
    NTEST_EXPECT_NEAR_F64(last.z, 0.0, 1e-4);
    // Still: it sleeps.
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, links[LINKS - 1]));
    scene_end(&s);
}

NTEST(joint, a_falling_chain_whips_without_coming_apart)
{
    Scene s;
    scene_begin(&s);
    enum {
        LINKS = 20
    };
    NphPhysicsBody links[LINKS];
    NphPhysicsJoint joints[LINKS];
    // Laid out sideways, it falls, swings down, and whips past the bottom.
    const NphDVec3 top = nph_dvec3(0.0, 12.0, 0.0);
    build_chain(s.w, add_anchor(s.w, top), top, LINKS, true, links, joints);
    f64 worst = 0.0;
    f32 fastest = 0.0F;
    for (u32 i = 0U; i < 240U * 4U; ++i) {
        step(s.w, NULL, 1U);
        for (u32 k = 0U; k < (u32)LINKS; ++k) {
            worst = fmax(worst, joint_gap(s.w, joints[k]));
        }
        fastest = nph_f32_max(
            fastest, nph_vec3_length(nph_physics_body_linear_velocity(s.w, links[LINKS - 1])));
    }
    NTEST_LOG("worst anchor gap %.2f mm; the end's top speed %.1f m/s", worst * 1e3, (f64)fastest);
    // Free fall from the side reaches about sqrt(2 g 10) = 14 m/s at the
    // end; the whip adds to it, but nothing comes apart.
    NTEST_EXPECT_TRUE(worst < 0.01);
    NTEST_EXPECT_TRUE(fastest < 60.0F);
    scene_end(&s);
}

NTEST(joint, a_chain_carries_a_body_a_hundred_times_heavier)
{
    Scene s;
    scene_begin(&s);
    enum {
        LINKS = 10
    };
    NphPhysicsBody links[LINKS];
    NphPhysicsJoint joints[LINKS];
    const NphDVec3 top = nph_dvec3(0.0, 10.0, 0.0);
    build_chain(s.w, add_anchor(s.w, top), top, LINKS, false, links, joints);
    // A heavy box under the last link.
    const NphDVec3 end = nph_dvec3(0.0, 10.0 - 0.5 * (f64)LINKS, 0.0);
    const NphPhysicsBody weight = add_dynamic(s.w, nph_dvec3_add(end, nph_dvec3(0.0, -0.2, 0.0)),
                                              nph_physics_geometry_box(nph_vec3_splat(0.2F)));
    NphPhysicsMassData m = nph_physics_body_mass_data(s.w, weight);
    const f32 link_mass = nph_physics_body_mass_data(s.w, links[0]).mass;
    const f32 scale = 100.0F * link_mass / m.mass;
    m.mass *= scale;
    m.inertia = nph_mat3_scale(m.inertia, scale);
    nph_physics_body_set_mass_data(s.w, weight, &m);
    const NphPhysicsJoint hook = ball_at(s.w, links[LINKS - 1], weight, end);
    step(s.w, NULL, 240U * 3U);
    f64 gaps = joint_gap(s.w, hook);
    for (u32 k = 0U; k < (u32)LINKS; ++k) {
        gaps += joint_gap(s.w, joints[k]);
    }
    const NphDVec3 p = nph_physics_body_position(s.w, weight);
    NTEST_LOG("the joints open %.2f mm in all under a 100:1 load", gaps * 1e3);
    // Each joint carries about 105 link weights on half a link's effective
    // mass: 2 * 105 * g / omega^2 = 0.9 mm a joint at 240 Hz.
    NTEST_EXPECT_TRUE(gaps < 0.015);
    NTEST_EXPECT_NEAR_F64(p.x, 0.0, 1e-3);
    scene_end(&s);
}

NTEST(joint, the_ball_cone_and_twist_hold)
{
    // A bone hanging from a ball joint whose X axis points down it, pushed
    // about each of the frame's axes in turn.
    const f32 swing_y = 30.0F * k_pi / 180.0F;
    const f32 swing_z = 15.0F * k_pi / 180.0F;
    const f32 twist = 20.0F * k_pi / 180.0F;
    f32 worst[3] = {0.0F, 0.0F, 0.0F}; // the swing pushed about Y, about Z; the twist
    for (u32 axis = 0U; axis < 3U; ++axis) {
        Scene s;
        scene_begin(&s);
        const NphDVec3 pivot = nph_dvec3(0.0, 5.0, 0.0);
        const NphPhysicsBody top = add_anchor(s.w, pivot);
        const NphPhysicsBody bone =
            add_dynamic(s.w, nph_dvec3(0.0, 4.5, 0.0), nph_physics_geometry_capsule(0.4F, 0.08F));
        NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_BALL, top, bone);
        d.ball.enable_swing_limit = 1;
        d.ball.swing_y = swing_y;
        d.ball.swing_z = swing_z;
        d.ball.enable_twist_limit = 1;
        d.ball.twist_lower = -twist;
        d.ball.twist_upper = twist;
        const NphPhysicsJoint j = join(s.w, d, pivot, nph_vec3(0.0F, -1.0F, 0.0F));
        const NphPhysicsJointDesc made = nph_physics_joint_desc(s.w, j);
        const NphQuat frame = made.frame_a.rotation; // the anchor is static at identity
        const NphVec3 push = axis == 0U   ? nph_quat_axis_y(frame)
                             : axis == 1U ? nph_quat_axis_z(frame)
                                          : nph_quat_axis_x(frame);
        swing_about(s.w, bone, pivot, nph_vec3_scale(push, 6.0F));
        for (u32 i = 0U; i < 240U * 2U; ++i) {
            step(s.w, NULL, 1U);
            NphVec3 xa;
            NphVec3 xb;
            frame_axes(s.w, j, &xa, &xb);
            const f32 a = axis < 2U ? angle_between(xa, xb)
                                    : nph_f32_abs(nph_physics_joint_hinge_angle(s.w, j));
            worst[axis] = nph_f32_max(worst[axis], a);
        }
        scene_end(&s);
    }
    NTEST_LOG("worst swing about Y %.2f deg (limit 30), about Z %.2f (15); twist %.2f (20)",
              (f64)(worst[0] * 180.0F / k_pi), (f64)(worst[1] * 180.0F / k_pi),
              (f64)(worst[2] * 180.0F / k_pi));
    const f32 tol = 1.0F * k_pi / 180.0F;
    NTEST_EXPECT_TRUE(worst[0] < swing_y + tol && worst[0] > swing_y - 3.0F * tol);
    NTEST_EXPECT_TRUE(worst[1] < swing_z + tol && worst[1] > swing_z - 3.0F * tol);
    NTEST_EXPECT_TRUE(worst[2] < twist + tol && worst[2] > twist - 3.0F * tol);
}

NTEST(joint, ball_friction_slows_a_spin_at_its_torque)
{
    Scene s;
    scene_begin(&s);
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    const NphDVec3 c = nph_dvec3(0.0, 3.0, 0.0);
    const NphPhysicsBody top = add_anchor(s.w, c);
    const NphPhysicsBody ball = add_dynamic(s.w, c, nph_physics_geometry_sphere(0.3F));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_BALL, top, ball);
    d.ball.friction_torque = 1.0F;
    join(s.w, d, c, nph_vec3(1.0F, 0.0F, 0.0F));
    const NphVec3 axis = nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F));
    nph_physics_body_set_angular_velocity(s.w, ball, nph_vec3_scale(axis, 5.0F));
    step(s.w, NULL, 240U * 2U);
    // A constant torque: w = w0 - (tau / I) t.
    const f32 inertia = nph_physics_body_mass_data(s.w, ball).inertia.c[0].x;
    const f32 expected = 5.0F - 2.0F / inertia;
    const f32 got = nph_vec3_length(nph_physics_body_angular_velocity(s.w, ball));
    NTEST_LOG("spin %.4f rad/s after 2 s against %.4f", (f64)got, (f64)expected);
    NTEST_EXPECT_NEAR_F32(got, expected, 0.01F * expected);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Hinges
// ---------------------------------------------------------------------------

NTEST(joint, a_hinge_keeps_its_axis_and_its_limits)
{
    Scene s;
    scene_begin(&s);
    // A door hinged on its left edge about Y, swung hard into its limits;
    // gravity pulls it every way but about the hinge.
    const NphPhysicsBody frame = add_anchor(s.w, nph_dvec3_zero());
    const NphPhysicsBody door = add_dynamic(s.w, nph_dvec3(0.5, 1.0, 0.0),
                                            nph_physics_geometry_box(nph_vec3(0.5F, 1.0F, 0.04F)));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, frame, door);
    d.hinge.enable_limit = 1;
    d.hinge.lower = -60.0F * k_pi / 180.0F;
    d.hinge.upper = 60.0F * k_pi / 180.0F;
    const NphPhysicsJoint j = join(s.w, d, nph_dvec3(0.0, 1.0, 0.0), nph_vec3(0.0F, 1.0F, 0.0F));
    // Slammed open, then slammed shut past the frame: turns about the hinge
    // (the centre of mass moving with them).
    const NphDVec3 hinge_point = nph_dvec3(0.0, 1.0, 0.0);
    swing_about(s.w, door, hinge_point, nph_vec3(0.0F, 4.0F, 0.0F));
    f32 lowest = 0.0F;
    f32 highest = 0.0F;
    f32 tilt = 0.0F;
    f64 gap = 0.0;
    for (u32 i = 0U; i < 240U * 3U; ++i) {
        if (i == 240U) {
            swing_about(s.w, door, hinge_point, nph_vec3(0.0F, -6.0F, 0.0F));
        }
        step(s.w, NULL, 1U);
        const f32 a = nph_physics_joint_hinge_angle(s.w, j);
        lowest = nph_f32_min(lowest, a);
        highest = nph_f32_max(highest, a);
        NphVec3 xa;
        NphVec3 xb;
        frame_axes(s.w, j, &xa, &xb);
        tilt = nph_f32_max(tilt, angle_between(xa, xb));
        gap = fmax(gap, joint_gap(s.w, j));
    }
    NTEST_LOG("angle from %.2f to %.2f deg (limits +-60); axis tilt %.4f deg; anchor gap %.3f mm",
              (f64)(lowest * 180.0F / k_pi), (f64)(highest * 180.0F / k_pi),
              (f64)(tilt * 180.0F / k_pi), gap * 1e3);
    const f32 tol = 0.5F * k_pi / 180.0F;
    NTEST_EXPECT_TRUE(highest > d.hinge.upper - tol && highest < d.hinge.upper + tol);
    NTEST_EXPECT_TRUE(lowest < d.hinge.lower + tol && lowest > d.hinge.lower - tol);
    NTEST_EXPECT_TRUE(tilt < 0.05F * k_pi / 180.0F);
    // Slamming 160 kg into a limit at 3 m/s opens the anchors for a few
    // substeps (the limit's row and the anchors' converge together over
    // them); at rest they close.
    NTEST_EXPECT_TRUE(gap < 2e-3);
    const f64 rest_gap = joint_gap(s.w, j);
    NTEST_LOG("anchor gap at rest %.4f mm", rest_gap * 1e3);
    NTEST_EXPECT_TRUE(rest_gap < 5e-5);
    scene_end(&s);
}

NTEST(joint, a_hinge_motor_reaches_its_speed_within_its_torque)
{
    for (u32 run = 0U; run < 2U; ++run) {
        Scene s;
        scene_begin(&s);
        // A wheel on an axle along Z: gravity has no lever on it.
        const NphDVec3 c = nph_dvec3(0.0, 2.0, 0.0);
        const NphPhysicsBody axle = add_anchor(s.w, c);
        const NphPhysicsBody wheel = add_dynamic(s.w, c, nph_physics_geometry_sphere(0.3F));
        NphPhysicsJointDesc d =
            nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, axle, wheel);
        d.hinge.enable_motor = 1;
        d.hinge.motor_speed = 10.0F;
        d.hinge.max_motor_torque = run == 0U ? 1000.0F : 0.5F;
        const NphPhysicsJoint j = join(s.w, d, c, nph_vec3(0.0F, 0.0F, 1.0F));
        step(s.w, NULL, 240U);
        const f32 speed = nph_physics_joint_hinge_speed(s.w, j);
        if (run == 0U) {
            NTEST_EXPECT_NEAR_F32(speed, 10.0F, 0.01F);
        } else {
            // Torque-limited: w = tau t / I.
            const f32 inertia = nph_physics_body_mass_data(s.w, wheel).inertia.c[2].z;
            NTEST_LOG("torque-limited speed %.5f rad/s after 1 s against %.5f", (f64)speed,
                      (f64)(0.5F / inertia));
            NTEST_EXPECT_NEAR_F32(speed, 0.5F / inertia, 0.01F * 0.5F / inertia);
        }
        scene_end(&s);
    }
}

NTEST(joint, a_hinge_spring_rings_at_its_frequency)
{
    Scene s;
    scene_begin(&s);
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    const NphDVec3 c = nph_dvec3(0.0, 2.0, 0.0);
    const NphPhysicsBody axle = add_anchor(s.w, c);
    const NphPhysicsBody wheel = add_dynamic(s.w, c, nph_physics_geometry_sphere(0.3F));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, axle, wheel);
    d.hinge.enable_spring = 1;
    d.hinge.spring_hertz = 2.0F;
    d.hinge.spring_damping_ratio = 0.0F;
    const NphPhysicsJoint j = join(s.w, d, c, nph_vec3(0.0F, 0.0F, 1.0F));
    nph_physics_body_set_angular_velocity(s.w, wheel, nph_vec3(0.0F, 0.0F, 1.0F));
    f32 prev = 0.0F;
    f64 crossings[8];
    u32 crossed = 0U;
    f32 first_peak = 0.0F;
    f32 last_peak = 0.0F;
    for (u32 i = 1U; i <= 240U * 3U; ++i) {
        step(s.w, NULL, 1U);
        const f32 a = nph_physics_joint_hinge_angle(s.w, j);
        if (i <= 120U) {
            first_peak = nph_f32_max(first_peak, a); // the first period
        } else if (i > 240U * 3U - 120U) {
            last_peak = nph_f32_max(last_peak, a); // the sixth
        }
        if (prev > 0.0F && a <= 0.0F && crossed < 8U) {
            crossings[crossed++] = ((f64)i - 1.0 + (f64)(prev / (prev - a))) / 240.0;
        }
        prev = a;
    }
    NTEST_ASSERT_TRUE(crossed >= 4U);
    const f64 period = (crossings[crossed - 1U] - crossings[0]) / (f64)(crossed - 1U);
    NTEST_LOG("period %.4f s (0.5 for 2 Hz); amplitude %.4f then %.4f rad (1 / (4 pi) = %.4f)",
              period, (f64)first_peak, (f64)last_peak, 1.0 / (4.0 * 3.141592653589793));
    NTEST_EXPECT_NEAR_F64(period, 0.5, 0.01);
    NTEST_EXPECT_NEAR_F32(first_peak, 1.0F / (4.0F * k_pi), 0.003F);
    // Undamped, it keeps its swing but for the implicit spring's own
    // damping, a ratio of about h omega / 2 (0.0065 here): 4 % a period.
    NTEST_EXPECT_TRUE(last_peak > 0.75F * first_peak);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Fixed joints
// ---------------------------------------------------------------------------

NTEST(joint, a_welded_cantilever_holds_out_straight)
{
    Scene s;
    scene_begin(&s);
    // A 2 m, 80 kg beam welded by its end to a wall.
    const NphPhysicsBody wall = add_anchor(s.w, nph_dvec3(0.0, 5.0, 0.0));
    const NphPhysicsBody beam = add_dynamic(s.w, nph_dvec3(1.0, 5.0, 0.0),
                                            nph_physics_geometry_box(nph_vec3(1.0F, 0.1F, 0.1F)));
    join(s.w, nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_FIXED, wall, beam),
         nph_dvec3(0.0, 5.0, 0.0), nph_vec3(1.0F, 0.0F, 0.0F));
    step(s.w, NULL, 240U * 3U);
    const NphQuat q = nph_physics_body_rotation(s.w, beam);
    const NphVec3 tip_dir = nph_quat_axis_x(q);
    const f64 sag = 5.0 - (nph_physics_body_position(s.w, beam).y + (f64)tip_dir.y);
    NTEST_LOG("the tip sags %.4f mm", sag * 1e3);
    NTEST_EXPECT_TRUE(sag < 5e-4 && sag > -1e-4);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, beam));
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Sliders
// ---------------------------------------------------------------------------

// A 0.4 m box (64 kg) on a slider along world X from a static anchor.
static NphPhysicsJoint slider_rig(Scene *s, NphPhysicsSliderJoint settings, NphPhysicsBody *box)
{
    const NphDVec3 at = nph_dvec3(0.0, 3.0, 0.0);
    const NphPhysicsBody rail = add_anchor(s->w, at);
    *box = add_dynamic(s->w, at, nph_physics_geometry_box(nph_vec3_splat(0.2F)));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_SLIDER, rail, *box);
    d.slider = settings;
    return join(s->w, d, at, nph_vec3(1.0F, 0.0F, 0.0F));
}

NTEST(joint, a_slider_moves_along_its_axis_only)
{
    Scene s;
    scene_begin(&s);
    NphPhysicsBody box;
    NphPhysicsSliderJoint free;
    memset(&free, 0, sizeof free);
    const NphPhysicsJoint j = slider_rig(&s, free, &box);
    // Pushed along, across, and spun: only the push along X survives, and
    // gravity, across the axis, is held.
    nph_physics_body_set_linear_velocity(s.w, box, nph_vec3(1.0F, 2.0F, -1.0F));
    nph_physics_body_set_angular_velocity(s.w, box, nph_vec3(3.0F, -2.0F, 1.0F));
    step(s.w, NULL, 240U);
    const NphDVec3 p = nph_physics_body_position(s.w, box);
    const f32 turned =
        2.0F * nph_f32_atan2(nph_vec3_length(nph_vec3(nph_physics_body_rotation(s.w, box).x,
                                                      nph_physics_body_rotation(s.w, box).y,
                                                      nph_physics_body_rotation(s.w, box).z)),
                             nph_f32_abs(nph_physics_body_rotation(s.w, box).w));
    NTEST_LOG("after 1 s: along %.4f m, across %.4f / %.4f mm, turned %.4f deg", p.x,
              (p.y - 3.0) * 1e3, p.z * 1e3, (f64)(turned * 180.0F / k_pi));
    // The across rows took the push's sideways part at once: the box keeps
    // its first 1 m/s along X.
    NTEST_EXPECT_NEAR_F64(p.x, 1.0, 0.01);
    NTEST_EXPECT_NEAR_F64(p.y, 3.0, 1e-4);
    NTEST_EXPECT_NEAR_F64(p.z, 0.0, 1e-4);
    NTEST_EXPECT_TRUE(turned < 0.05F * k_pi / 180.0F);
    NTEST_EXPECT_NEAR_F32(nph_physics_joint_slider_position(s.w, j), (f32)p.x, 1e-4F);
    NTEST_EXPECT_NEAR_F32(nph_physics_joint_slider_speed(s.w, j), 1.0F, 0.01F);
    scene_end(&s);
}

NTEST(joint, a_slider_limit_motor_and_spring)
{
    // The limit stops a 2 m/s slide at its end.
    {
        Scene s;
        scene_begin(&s);
        NphPhysicsBody box;
        NphPhysicsSliderJoint lim;
        memset(&lim, 0, sizeof lim);
        lim.enable_limit = 1;
        lim.lower = -0.5F;
        lim.upper = 0.5F;
        const NphPhysicsJoint j = slider_rig(&s, lim, &box);
        nph_physics_body_set_linear_velocity(s.w, box, nph_vec3(2.0F, 0.0F, 0.0F));
        f32 most = 0.0F;
        for (u32 i = 0U; i < 240U; ++i) {
            step(s.w, NULL, 1U);
            most = nph_f32_max(most, nph_physics_joint_slider_position(s.w, j));
        }
        NTEST_LOG("the slide stops at %.4f m (limit 0.5)", (f64)most);
        NTEST_EXPECT_NEAR_F32(most, 0.5F, 0.002F);
        scene_end(&s);
    }
    // The motor at its speed, and limited by its force: a = F / m.
    for (u32 run = 0U; run < 2U; ++run) {
        Scene s;
        scene_begin(&s);
        NphPhysicsBody box;
        NphPhysicsSliderJoint motor;
        memset(&motor, 0, sizeof motor);
        motor.enable_motor = 1;
        motor.motor_speed = 0.5F;
        motor.max_motor_force = run == 0U ? 1e5F : 10.0F;
        const NphPhysicsJoint j = slider_rig(&s, motor, &box);
        step(s.w, NULL, 240U);
        const f32 speed = nph_physics_joint_slider_speed(s.w, j);
        if (run == 0U) {
            NTEST_EXPECT_NEAR_F32(speed, 0.5F, 0.005F);
        } else {
            const f32 expected = 10.0F / nph_physics_body_mass_data(s.w, box).mass;
            NTEST_LOG("force-limited speed %.5f m/s after 1 s against %.5f", (f64)speed,
                      (f64)expected);
            NTEST_EXPECT_NEAR_F32(speed, expected, 0.01F * expected);
        }
        scene_end(&s);
    }
    // The spring rings at its frequency.
    {
        Scene s;
        scene_begin(&s);
        NphPhysicsBody box;
        NphPhysicsSliderJoint spring;
        memset(&spring, 0, sizeof spring);
        spring.enable_spring = 1;
        spring.spring_hertz = 2.0F;
        const NphPhysicsJoint j = slider_rig(&s, spring, &box);
        nph_physics_body_set_linear_velocity(s.w, box, nph_vec3(1.0F, 0.0F, 0.0F));
        f32 prev = 0.0F;
        f64 first = 0.0;
        f64 last = 0.0;
        u32 crossed = 0U;
        for (u32 i = 1U; i <= 240U * 2U; ++i) {
            step(s.w, NULL, 1U);
            const f32 x = nph_physics_joint_slider_position(s.w, j);
            if (prev > 0.0F && x <= 0.0F) {
                const f64 t = ((f64)i - 1.0 + (f64)(prev / (prev - x))) / 240.0;
                first = crossed == 0U ? t : first;
                last = t;
                crossed += 1U;
            }
            prev = x;
        }
        NTEST_ASSERT_TRUE(crossed >= 3U);
        const f64 period = (last - first) / (f64)(crossed - 1U);
        NTEST_LOG("slider spring period %.4f s (0.5 for 2 Hz)", period);
        NTEST_EXPECT_NEAR_F64(period, 0.5, 0.005);
        scene_end(&s);
    }
}

NTEST(joint, an_elevator_rests_on_its_slider_limit)
{
    Scene s;
    scene_begin(&s);
    // A vertical slider whose lower limit is the floor of the shaft.
    const NphDVec3 at = nph_dvec3(0.0, 1.0, 0.0);
    const NphPhysicsBody shaft = add_anchor(s.w, at);
    const NphPhysicsBody car =
        add_dynamic(s.w, at, nph_physics_geometry_box(nph_vec3(0.5F, 0.1F, 0.5F)));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_SLIDER, shaft, car);
    d.slider.enable_limit = 1;
    d.slider.lower = -0.5F;
    d.slider.upper = 2.0F;
    const NphPhysicsJoint j = join(s.w, d, at, nph_vec3(0.0F, 1.0F, 0.0F));
    s.w->desc.enable_sleep = 0;
    step(s.w, NULL, 240U * 2U);
    NTEST_EXPECT_NEAR_F32(nph_physics_joint_slider_position(s.w, j), -0.5F, 0.001F);
    NphVec3 force;
    NphVec3 torque;
    nph_physics_joint_reaction(s.w, j, &force, &torque);
    const f32 weight = nph_physics_body_mass_data(s.w, car).mass * 9.81F;
    NTEST_EXPECT_NEAR_F32(force.y, weight, 0.005F * weight);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Distance joints
// ---------------------------------------------------------------------------

NTEST(joint, a_distance_rod_holds_its_length)
{
    Scene s;
    scene_begin(&s);
    // A ball on a 1 m rod, released sideways: it swings at the rod's
    // length; the rotation is free (the ball keeps its spin).
    const NphDVec3 pivot = nph_dvec3(0.0, 5.0, 0.0);
    const NphPhysicsBody top = add_anchor(s.w, pivot);
    const NphPhysicsBody ball =
        add_dynamic(s.w, nph_dvec3(1.0, 5.0, 0.0), nph_physics_geometry_sphere(0.1F));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_DISTANCE, top, ball);
    d.frame_a = nph_physics_pose_identity();
    d.frame_b = nph_physics_pose_identity();
    d.distance.length = 1.0F;
    NphPhysicsJoint j;
    NTEST_ASSERT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_OK);
    nph_physics_body_set_angular_velocity(s.w, ball, nph_vec3(0.0F, 5.0F, 0.0F));
    f32 worst = 0.0F;
    for (u32 i = 0U; i < 240U * 2U; ++i) {
        step(s.w, NULL, 1U);
        worst = nph_f32_max(worst, nph_f32_abs(nph_physics_joint_length(s.w, j) - 1.0F));
    }
    NTEST_LOG("the rod's length strays %.4f mm at worst", (f64)worst * 1e3);
    NTEST_EXPECT_TRUE(worst < 2e-4F);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_angular_velocity(s.w, ball).y, 5.0F, 0.01F);
    scene_end(&s);
}

NTEST(joint, a_rope_goes_slack_and_taut)
{
    Scene s;
    scene_begin(&s);
    // A ball on a 1 m rope (lengths 0 to 1), lifted 0.5 m above its taut
    // place: it falls freely half a metre, then the rope holds it.
    const NphDVec3 pivot = nph_dvec3(0.0, 5.0, 0.0);
    const NphPhysicsBody top = add_anchor(s.w, pivot);
    const NphPhysicsBody ball =
        add_dynamic(s.w, nph_dvec3(0.0, 4.5, 0.0), nph_physics_geometry_sphere(0.1F));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_DISTANCE, top, ball);
    d.distance.length = 1.0F;
    d.distance.enable_limit = 1;
    d.distance.min_length = 0.0F;
    d.distance.max_length = 1.0F;
    NphPhysicsJoint j;
    NTEST_ASSERT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_OK);
    // Free fall: 0.5 m takes sqrt(2 * 0.5 / g) = 0.319 s.
    step(s.w, NULL, 48U);
    const f64 fallen = 4.5 - nph_physics_body_position(s.w, ball).y;
    NTEST_EXPECT_NEAR_F64(fallen, 0.5 * 9.81 * 0.04, 0.002); // 0.2 s of free fall
    f32 longest = 0.0F;
    for (u32 i = 0U; i < 240U * 2U; ++i) {
        step(s.w, NULL, 1U);
        longest = nph_f32_max(longest, nph_physics_joint_length(s.w, j));
    }
    NTEST_LOG("the rope's length peaks at %.4f m (1)", (f64)longest);
    NTEST_EXPECT_TRUE(longest < 1.002F);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, ball).y, 4.0, 0.002);
    scene_end(&s);
}

NTEST(joint, a_distance_spring_rings_at_its_frequency)
{
    Scene s;
    scene_begin(&s);
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    const NphDVec3 pivot = nph_dvec3(0.0, 5.0, 0.0);
    const NphPhysicsBody top = add_anchor(s.w, pivot);
    const NphPhysicsBody ball =
        add_dynamic(s.w, nph_dvec3(1.0, 5.0, 0.0), nph_physics_geometry_sphere(0.1F));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_DISTANCE, top, ball);
    d.distance.length = 1.0F;
    d.distance.enable_spring = 1;
    d.distance.spring_hertz = 2.0F;
    d.distance.spring_damping_ratio = 0.0F;
    NphPhysicsJoint j;
    NTEST_ASSERT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_OK);
    nph_physics_body_set_linear_velocity(s.w, ball, nph_vec3(0.5F, 0.0F, 0.0F));
    f32 prev = 0.0F;
    f64 first = 0.0;
    f64 last = 0.0;
    u32 crossed = 0U;
    for (u32 i = 1U; i <= 240U * 2U; ++i) {
        step(s.w, NULL, 1U);
        const f32 x = nph_physics_joint_length(s.w, j) - 1.0F;
        if (prev > 0.0F && x <= 0.0F) {
            const f64 t = ((f64)i - 1.0 + (f64)(prev / (prev - x))) / 240.0;
            first = crossed == 0U ? t : first;
            last = t;
            crossed += 1U;
        }
        prev = x;
    }
    NTEST_ASSERT_TRUE(crossed >= 3U);
    const f64 period = (last - first) / (f64)(crossed - 1U);
    NTEST_LOG("distance spring period %.4f s (0.5 for 2 Hz)", period);
    NTEST_EXPECT_NEAR_F64(period, 0.5, 0.005);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// 6-DOF joints
// ---------------------------------------------------------------------------

NTEST(joint, a_d6_is_a_weld_a_slider_and_nothing)
{
    for (u32 mode = 0U; mode < 3U; ++mode) {
        Scene s;
        scene_begin(&s);
        const NphDVec3 at = nph_dvec3(0.0, 5.0, 0.0);
        const NphPhysicsBody wall = add_anchor(s.w, at);
        const NphPhysicsBody beam = add_dynamic(
            s.w, nph_dvec3(0.5, 5.0, 0.0), nph_physics_geometry_box(nph_vec3(0.5F, 0.1F, 0.1F)));
        NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_D6, wall, beam);
        if (mode == 1U) {
            d.d6.axes[NPH_PHYSICS_D6_LINEAR_X].motion = NPH_PHYSICS_AXIS_FREE;
        } else if (mode == 2U) {
            for (u32 i = 0U; i < NPH_PHYSICS_D6_AXES; ++i) {
                d.d6.axes[i].motion = NPH_PHYSICS_AXIS_FREE;
            }
        }
        join(s.w, d, at, nph_vec3(1.0F, 0.0F, 0.0F));
        nph_physics_body_set_linear_velocity(s.w, beam, nph_vec3(1.0F, 0.0F, 0.0F));
        step(s.w, NULL, 240U);
        const NphDVec3 p = nph_physics_body_position(s.w, beam);
        if (mode == 0U) {
            // Welded: the push is taken, the beam holds out straight.
            NTEST_EXPECT_NEAR_F64(p.x, 0.5, 1e-3);
            NTEST_EXPECT_NEAR_F64(p.y, 5.0, 1e-3);
        } else if (mode == 1U) {
            // A slider along X: the push carries it 1 m; gravity is held.
            NTEST_EXPECT_NEAR_F64(p.x, 1.5, 0.01);
            NTEST_EXPECT_NEAR_F64(p.y, 5.0, 1e-3);
        } else {
            // Nothing held: a second of free fall.
            NTEST_EXPECT_NEAR_F64(p.x, 1.5, 0.01);
            NTEST_EXPECT_NEAR_F64(p.y, 5.0 - 0.5 * 9.81, 0.01);
        }
        scene_end(&s);
    }
}

NTEST(joint, a_d6_holds_its_angular_limits_axis_by_axis)
{
    // A bone below a 6-DOF pivot with its X axis down it: translation
    // locked, twist within 20 degrees, swings within 30 (Y) and 15 (Z): a
    // pyramid. Pushed about each axis in turn.
    const f32 deg = k_pi / 180.0F;
    const f32 limits[3] = {20.0F * deg, 30.0F * deg, 15.0F * deg};
    f32 worst[3] = {0.0F, 0.0F, 0.0F};
    for (u32 axis = 0U; axis < 3U; ++axis) {
        Scene s;
        scene_begin(&s);
        const NphDVec3 pivot = nph_dvec3(0.0, 5.0, 0.0);
        const NphPhysicsBody top = add_anchor(s.w, pivot);
        const NphPhysicsBody bone =
            add_dynamic(s.w, nph_dvec3(0.0, 4.5, 0.0), nph_physics_geometry_capsule(0.4F, 0.08F));
        NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_D6, top, bone);
        for (u32 i = 0U; i < 3U; ++i) {
            NphPhysicsD6Axis *a = &d.d6.axes[NPH_PHYSICS_D6_TWIST + i];
            a->motion = NPH_PHYSICS_AXIS_LIMITED;
            a->lower = -limits[i];
            a->upper = limits[i];
        }
        const NphPhysicsJoint j = join(s.w, d, pivot, nph_vec3(0.0F, -1.0F, 0.0F));
        const NphQuat frame = nph_physics_joint_desc(s.w, j).frame_a.rotation;
        const NphVec3 push = axis == 0U   ? nph_quat_axis_x(frame)
                             : axis == 1U ? nph_quat_axis_y(frame)
                                          : nph_quat_axis_z(frame);
        swing_about(s.w, bone, pivot, nph_vec3_scale(push, 6.0F));
        for (u32 i = 0U; i < 240U * 2U; ++i) {
            step(s.w, NULL, 1U);
            // The angle about the pushed axis: B's frame from A's.
            const NphPhysicsJointDesc now = nph_physics_joint_desc(s.w, j);
            NphQuat r = nph_quat_mul(
                nph_quat_conjugate(
                    nph_quat_mul(nph_physics_body_rotation(s.w, top), now.frame_a.rotation)),
                nph_quat_mul(nph_physics_body_rotation(s.w, bone), now.frame_b.rotation));
            if (r.w < 0.0F) {
                r = nph_quat_neg(r);
            }
            const f32 c = axis == 0U ? r.x : axis == 1U ? r.y : r.z;
            worst[axis] = nph_f32_max(worst[axis], nph_f32_abs(2.0F * nph_f32_atan2(c, r.w)));
        }
        scene_end(&s);
    }
    NTEST_LOG("worst twist %.2f deg (20), swing Y %.2f (30), swing Z %.2f (15)",
              (f64)(worst[0] / deg), (f64)(worst[1] / deg), (f64)(worst[2] / deg));
    for (u32 i = 0U; i < 3U; ++i) {
        NTEST_EXPECT_TRUE(worst[i] < limits[i] + 1.0F * deg && worst[i] > limits[i] - 3.0F * deg);
    }
}

NTEST(joint, a_d6_drives_its_axes)
{
    Scene s;
    scene_begin(&s);
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    // A wheel on a 6-DOF hub: twist free with a motor at 3 rad/s, the rest
    // locked but translation along Y, free with a 2 Hz spring.
    const NphDVec3 at = nph_dvec3(0.0, 2.0, 0.0);
    const NphPhysicsBody hub = add_anchor(s.w, at);
    const NphPhysicsBody wheel = add_dynamic(s.w, at, nph_physics_geometry_sphere(0.3F));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_D6, hub, wheel);
    NphPhysicsD6Axis *twist = &d.d6.axes[NPH_PHYSICS_D6_TWIST];
    twist->motion = NPH_PHYSICS_AXIS_FREE;
    twist->enable_motor = 1;
    twist->motor_speed = 3.0F;
    twist->max_motor_force = 1000.0F;
    NphPhysicsD6Axis *y = &d.d6.axes[NPH_PHYSICS_D6_LINEAR_Y];
    y->motion = NPH_PHYSICS_AXIS_FREE;
    y->enable_spring = 1;
    y->spring_hertz = 2.0F;
    y->spring_damping_ratio = 0.0F;
    const NphPhysicsJoint j = join(s.w, d, at, nph_vec3(0.0F, 0.0F, 1.0F));
    // The frame's Y axis (the anchor is static at identity: the frame is
    // the world's).
    const NphVec3 frame_y = nph_quat_axis_y(nph_physics_joint_desc(s.w, j).frame_a.rotation);
    nph_physics_body_set_linear_velocity(s.w, wheel, nph_vec3_scale(frame_y, 0.5F));
    f64 prev = 0.0;
    f64 first = 0.0;
    f64 last = 0.0;
    u32 crossed = 0U;
    for (u32 i = 1U; i <= 240U * 2U; ++i) {
        step(s.w, NULL, 1U);
        const f64 yy = (f64)nph_vec3_dot(
            nph_dvec3_relative(nph_physics_body_position(s.w, wheel), at), frame_y);
        if (prev > 0.0 && yy <= 0.0) {
            const f64 t = ((f64)i - 1.0 + prev / (prev - yy)) / 240.0;
            first = crossed == 0U ? t : first;
            last = t;
            crossed += 1U;
        }
        prev = yy;
    }
    NTEST_ASSERT_TRUE(crossed >= 3U);
    const f64 period = (last - first) / (f64)(crossed - 1U);
    const f32 spin = nph_physics_body_angular_velocity(s.w, wheel).z;
    NTEST_LOG("the spring's period %.4f s (0.5); the motor's spin %.4f rad/s (3)", period,
              (f64)spin);
    NTEST_EXPECT_NEAR_F64(period, 0.5, 0.005);
    NTEST_EXPECT_NEAR_F32(spin, 3.0F, 0.01F);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Joined bodies
// ---------------------------------------------------------------------------

NTEST(joint, joined_bodies_touch_only_when_asked)
{
    for (u32 collide = 0U; collide < 2U; ++collide) {
        Scene s;
        scene_begin(&s);
        nph_physics_world_set_gravity(s.w, nph_vec3_zero());
        // Two boxes overlapping by half, joined where they overlap.
        const NphPhysicsBody a = add_box(s.w, nph_dvec3(0.0, 2.0, 0.0), nph_vec3_splat(0.5F));
        const NphPhysicsBody b = add_box(s.w, nph_dvec3(0.5, 2.0, 0.0), nph_vec3_splat(0.5F));
        step(s.w, NULL, 1U); // the contact exists before the joint
        NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).contacts, 1U);
        NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_FIXED, a, b);
        d.collide_connected = (b32)collide;
        join(s.w, d, nph_dvec3(0.25, 2.0, 0.0), nph_vec3(1.0F, 0.0F, 0.0F));
        const f64 apart = nph_physics_body_position(s.w, b).x - nph_physics_body_position(s.w, a).x;
        step(s.w, NULL, 60U);
        NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).contacts, collide);
        if (collide == 0U) {
            // Nothing pushes them apart.
            NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, b).x -
                                      nph_physics_body_position(s.w, a).x,
                                  apart, 1e-5);
        }
        scene_end(&s);
    }
}

NTEST(joint, destroying_a_joint_drops_and_reconnects)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    // A box hangs from an anchor, overlapping a second box hung beside it.
    const NphDVec3 pivot = nph_dvec3(0.0, 3.0, 0.0);
    const NphPhysicsBody top = add_anchor(s.w, pivot);
    const NphPhysicsBody box = add_box(s.w, nph_dvec3(0.0, 2.5, 0.0), nph_vec3_splat(0.3F));
    const NphPhysicsJoint j = ball_at(s.w, top, box, pivot);
    step(s.w, NULL, 240U * 2U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, box));
    nph_physics_joint_destroy(s.w, j);
    NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, box));
    step(s.w, NULL, 240U); // 1.5 m to the floor in 0.55 s
    NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, box).y < 0.5);
    // Two overlapping boxes joined without collision touch once unjoined.
    nph_physics_world_set_gravity(s.w, nph_vec3_zero());
    const NphPhysicsBody a = add_box(s.w, nph_dvec3(10.0, 5.0, 0.0), nph_vec3_splat(0.5F));
    const NphPhysicsBody b = add_box(s.w, nph_dvec3(10.6, 5.0, 0.0), nph_vec3_splat(0.5F));
    const NphPhysicsJoint weld =
        join(s.w, nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_FIXED, a, b),
             nph_dvec3(10.3, 5.0, 0.0), nph_vec3(1.0F, 0.0F, 0.0F));
    step(s.w, NULL, 10U);
    const u32 joined = nph_physics_world_stats(s.w).contacts;
    nph_physics_joint_destroy(s.w, weld);
    step(s.w, NULL, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).contacts, joined + 1U);
    scene_end(&s);
}

NTEST(joint, destroying_a_body_destroys_its_joints)
{
    Scene s;
    scene_begin(&s);
    enum {
        LINKS = 3
    };
    NphPhysicsBody links[LINKS];
    NphPhysicsJoint joints[LINKS];
    const NphDVec3 top = nph_dvec3(0.0, 5.0, 0.0);
    build_chain(s.w, add_anchor(s.w, top), top, LINKS, false, links, joints);
    step(s.w, NULL, 10U);
    nph_physics_body_destroy(s.w, links[1]);
    NTEST_EXPECT_TRUE(nph_physics_joint_is_valid(s.w, joints[0]));
    NTEST_EXPECT_FALSE(nph_physics_joint_is_valid(s.w, joints[1]));
    NTEST_EXPECT_FALSE(nph_physics_joint_is_valid(s.w, joints[2]));
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).joints, 1U);
    NTEST_EXPECT_TRUE(nph_physics_world_validate(s.w));
    // The last link, free now, falls.
    const f64 y0 = nph_physics_body_position(s.w, links[2]).y;
    step(s.w, NULL, 60U);
    NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, links[2]).y < y0 - 0.1);
    scene_end(&s);
}

NTEST(joint, joined_bodies_sleep_and_wake_together)
{
    Scene s;
    scene_begin(&s);
    enum {
        LINKS = 6
    };
    NphPhysicsBody links[LINKS];
    NphPhysicsJoint joints[LINKS];
    const NphDVec3 top = nph_dvec3(0.0, 5.0, 0.0);
    build_chain(s.w, add_anchor(s.w, top), top, LINKS, false, links, joints);
    step(s.w, NULL, 240U * 3U);
    NphPhysicsStats st = nph_physics_world_stats(s.w);
    NTEST_EXPECT_EQ_UINT(st.awake_bodies, 0U);
    NTEST_EXPECT_EQ_UINT(st.islands, 1U);
    // A push at the bottom wakes the top.
    nph_physics_body_apply_linear_impulse(s.w, links[LINKS - 1], nph_vec3(1.0F, 0.0F, 0.0F),
                                          nph_physics_body_center_of_mass(s.w, links[LINKS - 1]));
    NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, links[0]));
    st = nph_physics_world_stats(s.w);
    NTEST_EXPECT_EQ_UINT(st.awake_bodies, (u32)LINKS);
    scene_end(&s);
}

NTEST(joint, a_slow_motor_keeps_its_bodies_awake)
{
    // Each type's motor driving a body slower than it would sleep at
    // (0.05 m/s at its fastest point): it keeps going.
    const NphPhysicsJointType types[4] = {NPH_PHYSICS_JOINT_HINGE, NPH_PHYSICS_JOINT_SLIDER,
                                          NPH_PHYSICS_JOINT_DISTANCE, NPH_PHYSICS_JOINT_D6};
    for (u32 t = 0U; t < 4U; ++t) {
        Scene s;
        scene_begin(&s);
        nph_physics_world_set_gravity(s.w, nph_vec3_zero());
        const NphDVec3 at = nph_dvec3(0.0, 2.0, 0.0);
        const NphPhysicsBody base = add_anchor(s.w, at);
        const NphPhysicsBody body = add_dynamic(s.w, nph_dvec3_add(at, nph_dvec3(0.0, -1.0, 0.0)),
                                                nph_physics_geometry_box(nph_vec3_splat(0.2F)));
        NphPhysicsJointDesc d = nph_physics_joint_desc_default(types[t], base, body);
        const f32 speed = 0.02F; // m/s, or rad/s about an axis 1 m away
        switch (types[t]) {
        case NPH_PHYSICS_JOINT_HINGE:
            d.hinge.enable_motor = 1;
            d.hinge.motor_speed = speed;
            d.hinge.max_motor_torque = 10.0F;
            break;
        case NPH_PHYSICS_JOINT_SLIDER:
            d.slider.enable_motor = 1;
            d.slider.motor_speed = speed;
            d.slider.max_motor_force = 10.0F;
            break;
        case NPH_PHYSICS_JOINT_DISTANCE:
            d.distance.min_length = 0.5F;
            d.distance.max_length = 2.0F;
            d.distance.enable_limit = 1;
            d.distance.enable_motor = 1;
            d.distance.motor_speed = speed;
            d.distance.max_motor_force = 10.0F;
            break;
        default: {
            NphPhysicsD6Axis *twist = &d.d6.axes[NPH_PHYSICS_D6_TWIST];
            twist->motion = NPH_PHYSICS_AXIS_FREE;
            twist->enable_motor = 1;
            twist->motor_speed = speed;
            twist->max_motor_force = 10.0F;
            break;
        }
        }
        // The hinge and the 6-DOF turn the body about the anchor; the
        // slider and the rope (between the bodies' origins) move it along
        // the line between them.
        if (types[t] == NPH_PHYSICS_JOINT_DISTANCE) {
            NphPhysicsJoint j;
            NTEST_ASSERT_EQ_INT(nph_physics_joint_create(s.w, &d, &j), NPH_RESULT_OK);
        } else {
            const bool turns = types[t] != NPH_PHYSICS_JOINT_SLIDER;
            join(s.w, d, at, turns ? nph_vec3(0.0F, 0.0F, 1.0F) : nph_vec3(0.0F, -1.0F, 0.0F));
        }
        const NphDVec3 p0 = nph_physics_body_position(s.w, body);
        step(s.w, NULL, 240U * 2U);
        const f64 moved = dist(nph_physics_body_position(s.w, body), p0);
        NTEST_LOG("type %u: moved %.4f m in 2 s (0.04), %s", (u32)types[t], moved,
                  nph_physics_body_is_awake(s.w, body) ? "awake" : "asleep");
        NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, body));
        NTEST_EXPECT_NEAR_F64(moved, 2.0 * (f64)speed, 0.002);
        scene_end(&s);
    }
}

NTEST(joint, a_kinematic_body_carries_what_hangs_from_it)
{
    Scene s;
    scene_begin(&s);
    NphPhysicsBodyDesc kd = nph_physics_body_desc_default();
    kd.type = NPH_PHYSICS_BODY_KINEMATIC;
    kd.position = nph_dvec3(0.0, 5.0, 0.0);
    NphPhysicsBody crane;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &kd, &crane), NPH_RESULT_OK);
    const NphPhysicsBody load = add_box(s.w, nph_dvec3(0.0, 4.0, 0.0), nph_vec3_splat(0.2F));
    ball_at(s.w, crane, load, nph_dvec3(0.0, 4.0, 0.0));
    // Both fall asleep while still; the crane starting to move wakes the
    // load, which follows.
    step(s.w, NULL, 240U * 2U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, load));
    nph_physics_body_set_linear_velocity(s.w, crane, nph_vec3(1.0F, 0.0F, 0.0F));
    step(s.w, NULL, 240U * 2U);
    NTEST_EXPECT_TRUE(nph_physics_body_is_awake(s.w, load));
    const NphDVec3 p = nph_physics_body_position(s.w, load);
    NTEST_EXPECT_NEAR_F64(p.x, 2.0, 0.3); // it lags, swinging, behind the crane at x = 2
    NTEST_EXPECT_NEAR_F64(dist(p, nph_physics_body_position(s.w, crane)), 1.0, 1e-3);
    scene_end(&s);
}

NTEST(joint, the_reaction_carries_the_weight)
{
    Scene s;
    scene_begin(&s);
    const NphDVec3 pivot = nph_dvec3(0.0, 3.0, 0.0);
    const NphPhysicsBody top = add_anchor(s.w, pivot);
    const NphPhysicsBody box = add_box(s.w, nph_dvec3(0.0, 2.5, 0.0), nph_vec3_splat(0.25F));
    const NphPhysicsJoint j =
        join(s.w, nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_FIXED, top, box), pivot,
             nph_vec3(1.0F, 0.0F, 0.0F));
    // Sleep would stop the step that reports it.
    NphPhysicsWorldDesc *desc = &s.w->desc;
    desc->enable_sleep = 0;
    step(s.w, NULL, 240U);
    NphVec3 force;
    NphVec3 torque;
    nph_physics_joint_reaction(s.w, j, &force, &torque);
    const f32 weight = nph_physics_body_mass_data(s.w, box).mass * 9.81F;
    NTEST_EXPECT_NEAR_F32(force.y, weight, 0.005F * weight);
    NTEST_EXPECT_NEAR_F32(force.x, 0.0F, 1e-3F * weight);
    NTEST_EXPECT_NEAR_F32(nph_vec3_length(torque), 0.0F, 1e-3F * weight);
    scene_end(&s);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

static void build_rig(NphPhysicsWorld *w)
{
    add_ground(w, 0.0);
    // Two chains, a door, a welded pair, and loose boxes falling on them.
    for (u32 c = 0U; c < 2U; ++c) {
        NphPhysicsBody links[8];
        NphPhysicsJoint joints[8];
        const NphDVec3 top = nph_dvec3(-2.0 + 4.0 * (f64)c, 6.0, 0.0);
        build_chain(w, add_anchor(w, top), top, 8U, c == 1U, links, joints);
    }
    const NphPhysicsBody frame = add_anchor(w, nph_dvec3(0.0, 0.0, 3.0));
    const NphPhysicsBody door = add_dynamic(w, nph_dvec3(0.5, 1.0, 3.0),
                                            nph_physics_geometry_box(nph_vec3(0.5F, 1.0F, 0.04F)));
    NphPhysicsJointDesc h = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, frame, door);
    h.hinge.enable_limit = 1;
    h.hinge.enable_motor = 1;
    h.hinge.motor_speed = 2.0F;
    h.hinge.max_motor_torque = 50.0F;
    join(w, h, nph_dvec3(0.0, 1.0, 3.0), nph_vec3(0.0F, 1.0F, 0.0F));
    const NphPhysicsBody a = add_box(w, nph_dvec3(0.0, 3.0, -3.0), nph_vec3_splat(0.3F));
    const NphPhysicsBody b = add_box(w, nph_dvec3(0.6, 3.0, -3.0), nph_vec3_splat(0.3F));
    join(w, nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_FIXED, a, b),
         nph_dvec3(0.3, 3.0, -3.0), nph_vec3(1.0F, 0.0F, 0.0F));
    const NphPhysicsBody rail = add_anchor(w, nph_dvec3(-4.0, 3.0, -3.0));
    const NphPhysicsBody cart =
        add_dynamic(w, nph_dvec3(-4.0, 3.0, -3.0), nph_physics_geometry_box(nph_vec3_splat(0.2F)));
    NphPhysicsJointDesc sl = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_SLIDER, rail, cart);
    sl.slider.enable_limit = 1;
    sl.slider.enable_motor = 1;
    sl.slider.motor_speed = 1.0F;
    sl.slider.max_motor_force = 200.0F;
    join(w, sl, nph_dvec3(-4.0, 3.0, -3.0), nph_vec3_normalize(nph_vec3(1.0F, 0.2F, 0.0F)));
    NphPhysicsJointDesc rope = nph_physics_joint_desc_default(
        NPH_PHYSICS_JOINT_DISTANCE, cart,
        add_dynamic(w, nph_dvec3(-4.0, 2.2, -3.0), nph_physics_geometry_sphere(0.15F)));
    rope.distance.enable_limit = 1;
    NphPhysicsJoint rj;
    NTEST_ASSERT_EQ_INT(nph_physics_joint_create(w, &rope, &rj), NPH_RESULT_OK);
    const NphPhysicsBody hub = add_anchor(w, nph_dvec3(4.0, 2.0, -3.0));
    const NphPhysicsBody arm =
        add_dynamic(w, nph_dvec3(4.5, 2.0, -3.0), nph_physics_geometry_capsule(0.3F, 0.06F));
    NphPhysicsJointDesc six = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_D6, hub, arm);
    six.d6.axes[NPH_PHYSICS_D6_TWIST].motion = NPH_PHYSICS_AXIS_FREE;
    six.d6.axes[NPH_PHYSICS_D6_SWING_Y].motion = NPH_PHYSICS_AXIS_LIMITED;
    six.d6.axes[NPH_PHYSICS_D6_SWING_Z].motion = NPH_PHYSICS_AXIS_LIMITED;
    six.d6.axes[NPH_PHYSICS_D6_LINEAR_X].motion = NPH_PHYSICS_AXIS_LIMITED;
    six.d6.axes[NPH_PHYSICS_D6_LINEAR_X].enable_spring = 1;
    join(w, six, nph_dvec3(4.0, 2.0, -3.0), nph_vec3(1.0F, 0.0F, 0.0F));
    NphPcg32 rng = test_rng(41U);
    for (u32 i = 0U; i < 12U; ++i) {
        add_body(w, NPH_PHYSICS_BODY_DYNAMIC,
                 nph_dvec3((f64)test_range(&rng, -3.0F, 3.0F), 7.0 + 0.5 * (f64)i,
                           (f64)test_range(&rng, -3.0F, 3.0F)),
                 test_rotation(&rng), nph_physics_geometry_box(test_vec3(&rng, 0.15F, 0.3F)), 0.6F,
                 0.1F);
    }
}

NTEST(joint, results_do_not_depend_on_the_worker_count)
{
    const u32 workers[4] = {0U, 1U, 4U, 8U};
    const u32 forced[4] = {0U, 2U, 5U, 9U};
    u64 reference = 0U;
    for (u32 run = 0U; run < 4U; ++run) {
        NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
        Scene s;
        scene_begin(&s);
        s.w->forced_participants = forced[run];
        build_rig(s.w);
        step(s.w, js, 400U);
        const u64 h = nph_physics_world_hash(s.w);
        if (run == 0U) {
            reference = h;
        } else {
            NTEST_EXPECT_EQ_UINT(h, reference);
        }
        scene_end(&s);
        nph_job_system_destroy(js);
    }
}
