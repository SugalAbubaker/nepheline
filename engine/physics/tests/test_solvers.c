// SPDX-License-Identifier: Apache-2.0
// The solver comparison of CLAUDE.md §13 (ADR-0045): the soft step, TGS,
// and XPBD on the same scenes, from the same collision, with the same
// substeps. Every scene must stay sound under every solver (finite, valid,
// nothing flung away); what each measures is logged for the ADR's table
// (run with --verbose).

#include <nepheline/math/kernels.h>

#include "test_world_support.h"

// ---------------------------------------------------------------------------
// Stacking
// ---------------------------------------------------------------------------

NTEST(solvers, stacks)
{
    const u32 rows = TEST_SCALE >= 8U ? 20U : 10U;
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // A column of ten, and a pyramid beside it.
        NphPhysicsBody column[10];
        for (u32 i = 0U; i < 10U; ++i) {
            column[i] = add_box(s.w, nph_dvec3(-20.0, 0.5 + (f64)i, 0.0), nph_vec3_splat(0.5F));
        }
        NphPhysicsBody top = NPH_PHYSICS_BODY_NULL;
        for (u32 row = 0U; row < rows; ++row) {
            const u32 count = rows - row;
            for (u32 i = 0U; i < count; ++i) {
                const f64 x = ((f64)i - (f64)(count - 1U) * 0.5) * 1.05;
                top = add_box(s.w, nph_dvec3(x, 0.5 + (f64)row, 0.0), nph_vec3_splat(0.5F));
            }
        }
        u32 slept = 0U;
        for (u32 i = 1U; i <= 240U * 6U; ++i) {
            step(s.w, NULL, 1U);
            if (slept == 0U && !nph_physics_body_is_awake(s.w, top)) {
                slept = i;
            }
        }
        const NphDVec3 p = nph_physics_body_position(s.w, top);
        const NphDVec3 c = nph_physics_body_position(s.w, column[9]);
        const f64 sink = 0.5 + (f64)(rows - 1U) - p.y;
        NTEST_LOG("%-9s pyramid of %u: top %.2f mm low, %.2f mm aside, %s; column top %.2f mm low, "
                  "%.2f mm aside",
                  k_configs[config].name, rows, sink * 1e3, fabs(p.x) * 1e3,
                  slept != 0U ? "asleep" : "still awake", (9.5 - c.y) * 1e3,
                  fabs(c.x + 20.0) * 1e3);
        if (slept != 0U) {
            NTEST_LOG("%-9s   asleep after %.2f s", k_configs[config].name, (f64)slept / 240.0);
        }
        NTEST_EXPECT_TRUE(sound(s.w, column, 10U, 100.0) && sound(s.w, &top, 1U, 100.0));
        if (is_default(config)) {
            // Standing within millimetres, a millimetre aside at most, and
            // asleep. (Points that lost their warm starts to flipping ids
            // sent the column 2 to 5 mm aside: ADR-0046.)
            NTEST_EXPECT_TRUE(fabs(sink) < 0.005 && fabs(p.x) < 0.001 && slept != 0U);
            NTEST_EXPECT_TRUE(fabs(9.5 - c.y) < 0.005 && fabs(c.x + 20.0) < 0.001);
        }
        scene_end(&s);
    }
}

NTEST(solvers, mass_ratio)
{
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // A light box under one 100 times heavier.
        const NphPhysicsBody light = add_box(s.w, nph_dvec3(0.0, 0.5, 0.0), nph_vec3_splat(0.5F));
        const NphPhysicsBody heavy = add_box(s.w, nph_dvec3(0.0, 1.5, 0.0), nph_vec3_splat(0.5F));
        NphPhysicsMassData m = nph_physics_body_mass_data(s.w, heavy);
        m.mass *= 100.0F;
        m.inertia = nph_mat3_scale(m.inertia, 100.0F);
        nph_physics_body_set_mass_data(s.w, heavy, &m);
        step(s.w, NULL, 240U * 3U);
        const f64 light_sink = 0.5 - nph_physics_body_position(s.w, light).y;
        const f64 heavy_sink = 1.5 - nph_physics_body_position(s.w, heavy).y;
        NTEST_LOG("%-9s 100:1 stack: light box sinks %.2f mm, heavy %.2f mm",
                  k_configs[config].name, light_sink * 1e3, heavy_sink * 1e3);
        NTEST_EXPECT_TRUE(fabs(heavy_sink) < 0.25);
        if (is_default(config)) {
            NTEST_EXPECT_TRUE(heavy_sink < 0.004);
        }
        scene_end(&s);
    }
}

// ---------------------------------------------------------------------------
// Joints
// ---------------------------------------------------------------------------

NTEST(solvers, chains)
{
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        // A chain of 10 carrying a body 100 times a link, and a chain of 20
        // released sideways.
        NphPhysicsBody links[20];
        NphPhysicsJoint joints[20];
        const NphDVec3 top = nph_dvec3(0.0, 10.0, 0.0);
        build_chain(s.w, add_anchor(s.w, top), top, 10U, false, links, joints);
        const NphDVec3 end = nph_dvec3(0.0, 5.0, 0.0);
        const NphPhysicsBody weight =
            add_dynamic(s.w, nph_dvec3_add(end, nph_dvec3(0.0, -0.2, 0.0)),
                        nph_physics_geometry_box(nph_vec3_splat(0.2F)));
        NphPhysicsMassData m = nph_physics_body_mass_data(s.w, weight);
        const f32 scale = 100.0F * nph_physics_body_mass_data(s.w, links[0]).mass / m.mass;
        m.mass *= scale;
        m.inertia = nph_mat3_scale(m.inertia, scale);
        nph_physics_body_set_mass_data(s.w, weight, &m);
        const NphPhysicsJoint hook = ball_at(s.w, links[9], weight, end);
        NphPhysicsBody whip[20];
        NphPhysicsJoint whip_joints[20];
        const NphDVec3 whip_top = nph_dvec3(5.0, 12.0, 10.0); // clear of the heavy chain
        build_chain(s.w, add_anchor(s.w, whip_top), whip_top, 20U, true, whip, whip_joints);
        f64 worst_whip = 0.0;
        for (u32 i = 0U; i < 240U * 3U; ++i) {
            step(s.w, NULL, 1U);
            for (u32 k = 0U; k < 20U; ++k) {
                worst_whip = fmax(worst_whip, joint_gap(s.w, whip_joints[k]));
            }
        }
        f64 stretch = joint_gap(s.w, hook);
        for (u32 k = 0U; k < 10U; ++k) {
            stretch += joint_gap(s.w, joints[k]);
        }
        NTEST_LOG("%-9s heavy chain stretches %.2f mm; whipping chain opens %.2f mm at worst",
                  k_configs[config].name, stretch * 1e3, worst_whip * 1e3);
        NTEST_EXPECT_TRUE(sound(s.w, whip, 20U, 100.0) && sound(s.w, links, 10U, 100.0));
        if (is_default(config)) {
            NTEST_EXPECT_TRUE(stretch < 0.015 && worst_whip < 0.003);
        }
        scene_end(&s);
    }
}

NTEST(solvers, pendulum)
{
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        const f64 theta = 20.0 * 3.141592653589793 / 180.0;
        const NphDVec3 pivot = nph_dvec3(0.0, 5.0, 0.0);
        const NphPhysicsBody bob = add_dynamic(s.w, nph_dvec3(sin(theta), 5.0 - cos(theta), 0.0),
                                               nph_physics_geometry_sphere(0.05F));
        ball_at(s.w, add_anchor(s.w, pivot), bob, pivot);
        // The large-amplitude period: T0 (1 + theta^2 / 16 + 11 theta^4 / 3072).
        const f64 t0 = 2.0 * 3.141592653589793 * sqrt((0.4 * 0.0025 + 1.0) / 9.81);
        const f64 expected = t0 * (1.0 + theta * theta / 16.0 + 11.0 * pow(theta, 4.0) / 3072.0);
        f64 prev_x = nph_physics_body_position(s.w, bob).x;
        f64 first = 0.0;
        f64 last = 0.0;
        u32 crossed = 0U;
        f64 peak = 0.0;
        for (u32 i = 1U; i <= 240U * 21U; ++i) {
            step(s.w, NULL, 1U);
            const f64 x = nph_physics_body_position(s.w, bob).x;
            if (crossed >= 10U) {
                peak = fmax(peak, x);
            }
            if (prev_x > 0.0 && x <= 0.0) {
                const f64 t = ((f64)i - 1.0 + prev_x / (prev_x - x)) / 240.0;
                if (crossed == 0U) {
                    first = t;
                }
                last = t;
                crossed += 1U;
            }
            prev_x = x;
        }
        NTEST_ASSERT_TRUE(crossed >= 5U);
        const f64 period = (last - first) / (f64)(crossed - 1U);
        NTEST_LOG("%-9s 20 deg pendulum: period %+.3f %% off; amplitude after 10 periods %.4f",
                  k_configs[config].name, 100.0 * (period / expected - 1.0), peak / sin(theta));
        NTEST_EXPECT_NEAR_F64(period, expected, 0.02 * expected);
        if (is_default(config)) {
            NTEST_EXPECT_NEAR_F64(period, expected, 0.001 * expected);
            NTEST_EXPECT_TRUE(peak > 0.99 * sin(theta));
        }
        scene_end(&s);
    }
}

NTEST(solvers, door_slam)
{
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        const NphPhysicsBody frame = add_anchor(s.w, nph_dvec3_zero());
        const NphPhysicsBody door = add_dynamic(
            s.w, nph_dvec3(0.5, 1.0, 0.0), nph_physics_geometry_box(nph_vec3(0.5F, 1.0F, 0.04F)));
        NphPhysicsJointDesc d =
            nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, frame, door);
        d.hinge.enable_limit = 1;
        d.hinge.lower = -60.0F * k_pi / 180.0F;
        d.hinge.upper = 60.0F * k_pi / 180.0F;
        const NphPhysicsJoint j =
            join(s.w, d, nph_dvec3(0.0, 1.0, 0.0), nph_vec3(0.0F, 1.0F, 0.0F));
        const NphDVec3 hinge = nph_dvec3(0.0, 1.0, 0.0);
        swing_about(s.w, door, hinge, nph_vec3(0.0F, 6.0F, 0.0F));
        f32 over = 0.0F;
        f64 gap = 0.0;
        f32 tilt = 0.0F;
        for (u32 i = 0U; i < 240U * 2U; ++i) {
            if (i == 240U) {
                swing_about(s.w, door, hinge, nph_vec3(0.0F, -6.0F, 0.0F));
            }
            step(s.w, NULL, 1U);
            const f32 a = nph_physics_joint_hinge_angle(s.w, j);
            over = nph_f32_max(over, nph_f32_max(a - d.hinge.upper, d.hinge.lower - a));
            gap = fmax(gap, joint_gap(s.w, j));
            NphVec3 xa;
            NphVec3 xb;
            frame_axes(s.w, j, &xa, &xb);
            tilt = nph_f32_max(tilt, angle_between(xa, xb));
        }
        NTEST_LOG(
            "%-9s door slammed at 6 rad/s: past its limits by %.3f deg, axis off by %.4f deg, "
            "anchors apart %.3f mm at worst",
            k_configs[config].name, (f64)(over * 180.0F / k_pi), (f64)(tilt * 180.0F / k_pi),
            gap * 1e3);
        NTEST_EXPECT_TRUE(over < 2.0F * k_pi / 180.0F && gap < 0.01);
        if (is_default(config)) {
            NTEST_EXPECT_TRUE(over < 0.1F * k_pi / 180.0F && gap < 0.002);
        }
        scene_end(&s);
    }
}

NTEST(solvers, ragdolls)
{
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // Four ragdolls dropped from 2 m onto each other, tumbling.
        Ragdoll dolls[4];
        NphPcg32 rng = test_rng(77U);
        for (u32 k = 0U; k < 4U; ++k) {
            build_ragdoll(s.w, nph_dvec3(0.3 * (f64)k, 2.0 + 1.8 * (f64)k, 0.2 * (f64)k),
                          nph_quat_identity(), &dolls[k]);
            for (u32 b = 0U; b < RAGDOLL_BODIES; ++b) {
                nph_physics_body_set_angular_velocity(s.w, dolls[k].bodies[b],
                                                      test_vec3(&rng, -3.0F, 3.0F));
            }
        }
        f64 worst = 0.0;
        u32 slept = 0U;
        for (u32 i = 1U; i <= 240U * 8U; ++i) {
            step(s.w, NULL, 1U);
            for (u32 k = 0U; k < 4U; ++k) {
                for (u32 j = 0U; j < RAGDOLL_JOINTS; ++j) {
                    worst = fmax(worst, joint_gap(s.w, dolls[k].joints[j]));
                }
            }
            if (slept == 0U && nph_physics_world_stats(s.w).awake_bodies == 0U) {
                slept = i;
            }
        }
        NTEST_LOG("%-9s four ragdolls dropped in a heap: joints open %.2f mm at worst; %s",
                  k_configs[config].name, worst * 1e3, slept != 0U ? "all asleep" : "still moving");
        if (slept != 0U) {
            NTEST_LOG("%-9s   asleep after %.2f s", k_configs[config].name, (f64)slept / 240.0);
        }
        for (u32 k = 0U; k < 4U; ++k) {
            NTEST_EXPECT_TRUE(sound(s.w, dolls[k].bodies, RAGDOLL_BODIES, 50.0));
        }
        if (is_default(config)) {
            // The heap settles and sleeps; impacts open a joint by a few
            // centimetres for a moment at most. (A heap is chaotic: another
            // compiler's rounding sends it elsewhere, so the bound is twice
            // what one measures.)
            NTEST_EXPECT_TRUE(worst < 0.04);
            NTEST_EXPECT_TRUE(slept != 0U);
        }
        scene_end(&s);
    }
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

NTEST(solvers, bounce_and_friction)
{
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // A ball of restitution 0.5 dropped 1 m: its bounce peaks at
        // 0.25 m.
        const NphPhysicsBody ball =
            add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 1.2, 0.0), nph_quat_identity(),
                     nph_physics_geometry_sphere(0.2F), 0.6F, 0.5F);
        // Boxes on 20 and 40 degree ramps (friction 0.6: tan 20 = 0.36
        // holds, tan 40 = 0.84 slides at g (sin - 0.6 cos) = 1.80 m/s^2).
        NphPhysicsBody boxes[2];
        f32 angles[2] = {20.0F * k_pi / 180.0F, 40.0F * k_pi / 180.0F};
        for (u32 k = 0U; k < 2U; ++k) {
            const NphQuat q = nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), angles[k]);
            const NphDVec3 at = nph_dvec3(10.0 + 10.0 * (f64)k, 5.0, 0.0);
            add_body(s.w, NPH_PHYSICS_BODY_STATIC, at, q,
                     nph_physics_geometry_box(nph_vec3(4.0F, 0.1F, 2.0F)), 0.6F, 0.0F);
            const NphVec3 up = nph_quat_axis_y(q);
            boxes[k] = add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC,
                                nph_dvec3_add_vec3(at, nph_vec3_scale(up, 0.35F)), q,
                                nph_physics_geometry_box(nph_vec3_splat(0.25F)), 0.6F, 0.0F);
        }
        // Measure from 0.1 s to 1.5 s (on the ramp: it starts 4 m from the
        // edge and slides 2 m).
        f64 peak = 0.0;
        bool bounced = false;
        f64 prev_y = 1.2;
        NphDVec3 start[2];
        NphDVec3 stop[2];
        for (u32 i = 0U; i < 240U * 2U; ++i) {
            if (i == 24U) {
                start[0] = nph_physics_body_position(s.w, boxes[0]);
                start[1] = nph_physics_body_position(s.w, boxes[1]);
            }
            if (i == 360U) {
                stop[0] = nph_physics_body_position(s.w, boxes[0]);
                stop[1] = nph_physics_body_position(s.w, boxes[1]);
            }
            step(s.w, NULL, 1U);
            const f64 y = nph_physics_body_position(s.w, ball).y - 0.2;
            if (y > prev_y && prev_y < 0.05) {
                bounced = true;
            }
            if (bounced) {
                peak = fmax(peak, y);
            }
            prev_y = y;
        }
        const f64 crept = dist(stop[0], start[0]);
        const f64 slid = dist(stop[1], start[1]);
        // From rest at 0: s(t) = a t^2 / 2, so a = 2 (s(1.5) - s(0.1)) / (1.5^2 - 0.1^2).
        const f64 accel = 2.0 * slid / (1.5 * 1.5 - 0.1 * 0.1);
        NTEST_LOG(
            "%-9s bounce to %.3f m (0.25); on 20 deg crept %.3f mm; on 40 deg slid at %.3f m/s^2 "
            "(1.80)",
            k_configs[config].name, peak, crept * 1e3, accel);
        NTEST_EXPECT_TRUE(peak > 0.1 && peak < 0.4);
        NTEST_EXPECT_NEAR_F64(accel, 1.80, 0.4);
        if (is_default(config)) {
            NTEST_EXPECT_NEAR_F64(peak, 0.25, 0.01);
            NTEST_EXPECT_TRUE(crept < 0.001);
            NTEST_EXPECT_NEAR_F64(accel, 1.797, 0.02);
        }
        scene_end(&s);
    }
}

// ---------------------------------------------------------------------------
// Every solver's arithmetic agrees across ISAs and worker counts
// ---------------------------------------------------------------------------

// A pile of mixed shapes on the ground, a chain swinging into it, and a
// motorized door: contacts, joints, limits, and a motor.
static void build_mix(NphPhysicsWorld *w)
{
    add_ground(w, 0.0);
    NphPcg32 rng = test_rng(29U);
    for (u32 i = 0U; i < 30U; ++i) {
        const NphDVec3 p = nph_dvec3((f64)test_range(&rng, -2.0F, 2.0F), 0.5 + 0.6 * (f64)i,
                                     (f64)test_range(&rng, -2.0F, 2.0F));
        NphPhysicsGeometry g = nph_physics_geometry_box(test_vec3(&rng, 0.15F, 0.4F));
        if (i % 3U == 1U) {
            g = nph_physics_geometry_sphere(test_range(&rng, 0.15F, 0.35F));
        } else if (i % 3U == 2U) {
            g = nph_physics_geometry_capsule(test_range(&rng, 0.1F, 0.3F),
                                             test_range(&rng, 0.1F, 0.2F));
        }
        add_body(w, NPH_PHYSICS_BODY_DYNAMIC, p, test_rotation(&rng), g, 0.6F, 0.1F);
    }
    NphPhysicsBody links[8];
    NphPhysicsJoint joints[8];
    const NphDVec3 top = nph_dvec3(-3.0, 5.0, 0.0);
    build_chain(w, add_anchor(w, top), top, 8U, true, links, joints);
    const NphPhysicsBody frame = add_anchor(w, nph_dvec3(3.0, 0.0, 3.0));
    const NphPhysicsBody door = add_dynamic(w, nph_dvec3(3.5, 1.0, 3.0),
                                            nph_physics_geometry_box(nph_vec3(0.5F, 1.0F, 0.04F)));
    NphPhysicsJointDesc d = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, frame, door);
    d.hinge.enable_limit = 1;
    d.hinge.enable_motor = 1;
    d.hinge.motor_speed = 3.0F;
    d.hinge.max_motor_torque = 40.0F;
    join(w, d, nph_dvec3(3.0, 1.0, 3.0), nph_vec3(0.0F, 1.0F, 0.0F));
    // A motorized slider carrying a rope with a weight, and a 6-DOF arm.
    const NphDVec3 rail_at = nph_dvec3(-3.0, 4.0, -3.0);
    const NphPhysicsBody rail = add_anchor(w, rail_at);
    const NphPhysicsBody cart =
        add_dynamic(w, rail_at, nph_physics_geometry_box(nph_vec3_splat(0.2F)));
    NphPhysicsJointDesc sl = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_SLIDER, rail, cart);
    sl.slider.enable_limit = 1;
    sl.slider.enable_motor = 1;
    sl.slider.motor_speed = 1.0F;
    sl.slider.max_motor_force = 300.0F;
    join(w, sl, rail_at, nph_vec3(1.0F, 0.0F, 0.0F));
    NphPhysicsJointDesc rope = nph_physics_joint_desc_default(
        NPH_PHYSICS_JOINT_DISTANCE, cart,
        add_dynamic(w, nph_dvec3(-3.0, 3.2, -3.0), nph_physics_geometry_sphere(0.15F)));
    rope.distance.enable_limit = 1;
    NphPhysicsJoint rj;
    NTEST_ASSERT_EQ_INT(nph_physics_joint_create(w, &rope, &rj), NPH_RESULT_OK);
    const NphDVec3 hub_at = nph_dvec3(3.0, 3.0, -3.0);
    const NphPhysicsBody hub = add_anchor(w, hub_at);
    const NphPhysicsBody arm =
        add_dynamic(w, nph_dvec3(3.5, 3.0, -3.0), nph_physics_geometry_capsule(0.3F, 0.06F));
    NphPhysicsJointDesc six = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_D6, hub, arm);
    six.d6.axes[NPH_PHYSICS_D6_TWIST].motion = NPH_PHYSICS_AXIS_FREE;
    six.d6.axes[NPH_PHYSICS_D6_TWIST].enable_motor = 1;
    six.d6.axes[NPH_PHYSICS_D6_TWIST].motor_speed = 2.0F;
    six.d6.axes[NPH_PHYSICS_D6_TWIST].max_motor_force = 50.0F;
    six.d6.axes[NPH_PHYSICS_D6_SWING_Y].motion = NPH_PHYSICS_AXIS_LIMITED;
    six.d6.axes[NPH_PHYSICS_D6_SWING_Z].motion = NPH_PHYSICS_AXIS_LIMITED;
    six.d6.axes[NPH_PHYSICS_D6_LINEAR_X].motion = NPH_PHYSICS_AXIS_LIMITED;
    six.d6.axes[NPH_PHYSICS_D6_LINEAR_X].enable_spring = 1;
    join(w, six, hub_at, nph_vec3(1.0F, 0.0F, 0.0F));
}

NTEST(solvers, machines)
{
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        // A cart pushed into its slider's end at 2 m/s.
        const NphDVec3 rail_at = nph_dvec3(0.0, 3.0, 0.0);
        const NphPhysicsBody rail = add_anchor(s.w, rail_at);
        const NphPhysicsBody cart =
            add_dynamic(s.w, rail_at, nph_physics_geometry_box(nph_vec3_splat(0.2F)));
        NphPhysicsJointDesc sl =
            nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_SLIDER, rail, cart);
        sl.slider.enable_limit = 1;
        sl.slider.lower = -0.5F;
        sl.slider.upper = 0.5F;
        const NphPhysicsJoint slider = join(s.w, sl, rail_at, nph_vec3(1.0F, 0.0F, 0.0F));
        nph_physics_body_set_linear_velocity(s.w, cart, nph_vec3(2.0F, 0.0F, 0.0F));
        // A weight dropped 0.5 m onto a slack 1 m rope.
        const NphDVec3 hook = nph_dvec3(5.0, 5.0, 0.0);
        const NphPhysicsBody top = add_anchor(s.w, hook);
        const NphPhysicsBody weight =
            add_dynamic(s.w, nph_dvec3(5.0, 4.5, 0.0), nph_physics_geometry_sphere(0.1F));
        NphPhysicsJointDesc rd =
            nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_DISTANCE, top, weight);
        rd.distance.enable_limit = 1;
        NphPhysicsJoint rope;
        NTEST_ASSERT_EQ_INT(nph_physics_joint_create(s.w, &rd, &rope), NPH_RESULT_OK);
        // A bone on a 6-DOF pivot, swung into its 30 degree swing limit.
        const NphDVec3 pivot = nph_dvec3(-5.0, 5.0, 0.0);
        const NphPhysicsBody base = add_anchor(s.w, pivot);
        const NphPhysicsBody bone =
            add_dynamic(s.w, nph_dvec3(-5.0, 4.5, 0.0), nph_physics_geometry_capsule(0.4F, 0.08F));
        NphPhysicsJointDesc six = nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_D6, base, bone);
        for (u32 i = NPH_PHYSICS_D6_TWIST; i < NPH_PHYSICS_D6_AXES; ++i) {
            six.d6.axes[i].motion = NPH_PHYSICS_AXIS_LIMITED;
            six.d6.axes[i].lower = -30.0F * k_pi / 180.0F;
            six.d6.axes[i].upper = 30.0F * k_pi / 180.0F;
        }
        const NphPhysicsJoint d6 = join(s.w, six, pivot, nph_vec3(0.0F, -1.0F, 0.0F));
        const NphQuat frame = nph_physics_joint_desc(s.w, d6).frame_a.rotation;
        swing_about(s.w, bone, pivot, nph_vec3_scale(nph_quat_axis_y(frame), 6.0F));
        f32 slide = 0.0F;
        f32 rope_len = 0.0F;
        f32 swing = 0.0F;
        f64 pivot_gap = 0.0;
        for (u32 i = 0U; i < 240U * 2U; ++i) {
            step(s.w, NULL, 1U);
            slide = nph_f32_max(slide, nph_physics_joint_slider_position(s.w, slider));
            rope_len = nph_f32_max(rope_len, nph_physics_joint_length(s.w, rope));
            NphVec3 xa;
            NphVec3 xb;
            frame_axes(s.w, d6, &xa, &xb);
            swing = nph_f32_max(swing, angle_between(xa, xb));
            pivot_gap = fmax(pivot_gap, joint_gap(s.w, d6));
        }
        NTEST_LOG("%-9s slider stops at %.4f m (0.5); rope peaks at %.4f m (1); 6-DOF swing %.2f "
                  "deg (30), pivot gap %.3f mm",
                  k_configs[config].name, (f64)slide, (f64)rope_len, (f64)(swing * 180.0F / k_pi),
                  pivot_gap * 1e3);
        NTEST_EXPECT_TRUE(slide < 0.55F && rope_len < 1.05F && swing < 35.0F * k_pi / 180.0F);
        if (is_default(config)) {
            NTEST_EXPECT_NEAR_F32(slide, 0.5F, 0.002F);
            NTEST_EXPECT_TRUE(rope_len < 1.002F && pivot_gap < 0.002);
        }
        scene_end(&s);
    }
}

NTEST(solvers, sse4_and_avx2_kernels_agree_to_the_bit)
{
    if (nph_simd_detect_level() < NPH_SIMD_LEVEL_AVX2) {
        NTEST_SKIP("no AVX2 on this machine");
    }
    for (u32 solver = 0U; solver < (u32)NPH_PHYSICS_SOLVER_COUNT; ++solver) {
        u64 hashes[2];
        for (u32 k = 0U; k < 2U; ++k) {
            Scene s;
            NphPhysicsWorldDesc d = nph_physics_world_desc_default();
            d.solver = (NphPhysicsSolver)solver;
            scene_begin_with(&s, d);
            s.w->kernels = k == 0U ? &g_phys_solver_sse4 : &g_phys_solver_avx2;
            build_mix(s.w);
            step(s.w, NULL, 300U);
            hashes[k] = nph_physics_world_hash(s.w);
            scene_end(&s);
        }
        NTEST_EXPECT_EQ_UINT(hashes[0], hashes[1]);
    }
}

NTEST(solvers, results_do_not_depend_on_the_worker_count)
{
    const u32 workers[3] = {0U, 4U, 8U};
    const u32 forced[3] = {0U, 5U, 9U};
    for (u32 solver = 0U; solver < (u32)NPH_PHYSICS_SOLVER_COUNT; ++solver) {
        u64 reference = 0U;
        for (u32 run = 0U; run < 3U; ++run) {
            NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
            Scene s;
            NphPhysicsWorldDesc d = nph_physics_world_desc_default();
            d.solver = (NphPhysicsSolver)solver;
            scene_begin_with(&s, d);
            s.w->forced_participants = forced[run];
            build_mix(s.w);
            step(s.w, js, 300U);
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
}
