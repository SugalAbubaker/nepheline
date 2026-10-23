// SPDX-License-Identifier: Apache-2.0
// The solver comparison of CLAUDE.md §13 (ADR-0045): the soft step, TGS,
// and XPBD on the same scenes, from the same collision, with the same
// substeps. Every scene must stay sound under every solver (finite, valid,
// nothing flung away); what each measures is logged for the ADR's table
// (run with --verbose).

#include <nepheline/math/kernels.h>

#include "test_world_support.h"

// The configurations compared: each solver at the default 4 substeps, then
// the soft step at 8 (its stiffnesses scale with the substep rate) and XPBD
// at 16 (it converges with substeps), the last two where the build is fast
// enough (not Debug or the sanitizers). Quality thresholds hold the default
// (the soft step at 4); every configuration must stay sound.
typedef struct SolverConfig {
    NphPhysicsSolver solver;
    u32 substeps;
    const char *name;
} SolverConfig;

static const SolverConfig k_configs[] = {
    {NPH_PHYSICS_SOLVER_SOFT_STEP, 4U, "soft 4"}, {NPH_PHYSICS_SOLVER_TGS, 4U, "TGS 4"},
    {NPH_PHYSICS_SOLVER_XPBD, 4U, "XPBD 4"},      {NPH_PHYSICS_SOLVER_SOFT_STEP, 8U, "soft 8"},
    {NPH_PHYSICS_SOLVER_XPBD, 16U, "XPBD 16"},
};

#define CONFIGS (TEST_SCALE >= 8U ? (u32)NPH_ARRAY_COUNT(k_configs) : 3U)

static void solver_scene(Scene *s, u32 config)
{
    NphPhysicsWorldDesc d = nph_physics_world_desc_default();
    d.solver = k_configs[config].solver;
    d.substeps = k_configs[config].substeps;
    scene_begin_with(s, d);
}

// The default configuration: where the quality thresholds apply.
static bool is_default(u32 config)
{
    return config == 0U;
}

// Every body finite and within `radius` of the origin.
static bool sound(const NphPhysicsWorld *w, NphPhysicsBody *bodies, u32 count, f64 radius)
{
    for (u32 i = 0U; i < count; ++i) {
        const NphDVec3 p = nph_physics_body_position(w, bodies[i]);
        if (!(fabs(p.x) < radius && fabs(p.y) < radius && fabs(p.z) < radius)) {
            return false;
        }
    }
    return true;
}

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
            // Standing within millimetres, and asleep.
            NTEST_EXPECT_TRUE(fabs(sink) < 0.005 && fabs(p.x) < 0.003 && slept != 0U);
            NTEST_EXPECT_TRUE(fabs(9.5 - c.y) < 0.005 && fabs(c.x + 20.0) < 0.003);
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

// A ragdoll of eleven bodies: pelvis, chest, head, upper and lower arms
// and legs, ball joints with cones and twists at the spine, neck,
// shoulders, and hips, hinges at the elbows and knees, bending the
// anatomical way (the forearms forward, the shins back). Every joint has
// 2 N m of friction, as a limp body's muscles do: without it a dangling
// limb swings for ever and keeps a heap awake under any solver.
typedef struct Ragdoll {
    NphPhysicsBody bodies[11];
    NphPhysicsJoint joints[10];
} Ragdoll;

static void ragdoll_ball(NphPhysicsWorld *w, Ragdoll *r, u32 joint, u32 a, u32 b, NphDVec3 at,
                         NphVec3 axis, f32 swing_y, f32 swing_z, f32 twist)
{
    NphPhysicsJointDesc d =
        nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_BALL, r->bodies[a], r->bodies[b]);
    d.ball.enable_swing_limit = 1;
    d.ball.swing_y = swing_y;
    d.ball.swing_z = swing_z;
    d.ball.enable_twist_limit = 1;
    d.ball.twist_lower = -twist;
    d.ball.twist_upper = twist;
    d.ball.friction_torque = 2.0F;
    r->joints[joint] = join(w, d, at, axis);
}

static void ragdoll_hinge(NphPhysicsWorld *w, Ragdoll *r, u32 joint, u32 a, u32 b, NphDVec3 at,
                          NphVec3 axis, f32 lower, f32 upper)
{
    NphPhysicsJointDesc d =
        nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_HINGE, r->bodies[a], r->bodies[b]);
    d.hinge.enable_limit = 1;
    d.hinge.lower = lower;
    d.hinge.upper = upper;
    d.hinge.enable_motor = 1; // a motor to standstill: friction
    d.hinge.max_motor_torque = 2.0F;
    r->joints[joint] = join(w, d, at, axis);
}

static void build_ragdoll(NphPhysicsWorld *w, NphDVec3 base, Ragdoll *r)
{
    const f32 deg = k_pi / 180.0F;
    const NphQuat side = nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), 90.0F * deg);
    struct {
        f64 x, y;
        NphPhysicsGeometry g;
        bool sideways;
    } parts[11] = {
        {0.0, 1.0, nph_physics_geometry_box(nph_vec3(0.15F, 0.1F, 0.1F)), false},   // pelvis
        {0.0, 1.32, nph_physics_geometry_box(nph_vec3(0.16F, 0.17F, 0.1F)), false}, // chest
        {0.0, 1.62, nph_physics_geometry_sphere(0.1F), false},                      // head
        {-0.33, 1.4, nph_physics_geometry_capsule(0.11F, 0.05F), true},             // upper arms
        {0.33, 1.4, nph_physics_geometry_capsule(0.11F, 0.05F), true},
        {-0.62, 1.4, nph_physics_geometry_capsule(0.11F, 0.045F), true}, // forearms
        {0.62, 1.4, nph_physics_geometry_capsule(0.11F, 0.045F), true},
        {-0.1, 0.68, nph_physics_geometry_capsule(0.14F, 0.07F), false}, // thighs
        {0.1, 0.68, nph_physics_geometry_capsule(0.14F, 0.07F), false},
        {-0.1, 0.25, nph_physics_geometry_capsule(0.14F, 0.06F), false}, // shins
        {0.1, 0.25, nph_physics_geometry_capsule(0.14F, 0.06F), false},
    };
    for (u32 i = 0U; i < 11U; ++i) {
        r->bodies[i] =
            add_body(w, NPH_PHYSICS_BODY_DYNAMIC,
                     nph_dvec3_add(base, nph_dvec3(parts[i].x, parts[i].y, 0.0)),
                     parts[i].sideways ? side : nph_quat_identity(), parts[i].g, 0.6F, 0.0F);
    }
    const NphVec3 up = nph_vec3(0.0F, 1.0F, 0.0F);
    const NphVec3 down = nph_vec3(0.0F, -1.0F, 0.0F);
    const NphVec3 left = nph_vec3(-1.0F, 0.0F, 0.0F);
    const NphVec3 right = nph_vec3(1.0F, 0.0F, 0.0F);
    ragdoll_ball(w, r, 0U, 0U, 1U, nph_dvec3_add(base, nph_dvec3(0.0, 1.12, 0.0)), up, 30.0F * deg,
                 20.0F * deg, 20.0F * deg);
    ragdoll_ball(w, r, 1U, 1U, 2U, nph_dvec3_add(base, nph_dvec3(0.0, 1.5, 0.0)), up, 40.0F * deg,
                 40.0F * deg, 45.0F * deg);
    ragdoll_ball(w, r, 2U, 1U, 3U, nph_dvec3_add(base, nph_dvec3(-0.17, 1.42, 0.0)), left,
                 80.0F * deg, 60.0F * deg, 45.0F * deg);
    ragdoll_ball(w, r, 3U, 1U, 4U, nph_dvec3_add(base, nph_dvec3(0.17, 1.42, 0.0)), right,
                 80.0F * deg, 60.0F * deg, 45.0F * deg);
    ragdoll_hinge(w, r, 4U, 3U, 5U, nph_dvec3_add(base, nph_dvec3(-0.48, 1.4, 0.0)), up, 0.0F,
                  140.0F * deg);
    ragdoll_hinge(w, r, 5U, 4U, 6U, nph_dvec3_add(base, nph_dvec3(0.48, 1.4, 0.0)), down, 0.0F,
                  140.0F * deg);
    ragdoll_ball(w, r, 6U, 0U, 7U, nph_dvec3_add(base, nph_dvec3(-0.1, 0.9, 0.0)), down,
                 60.0F * deg, 30.0F * deg, 30.0F * deg);
    ragdoll_ball(w, r, 7U, 0U, 8U, nph_dvec3_add(base, nph_dvec3(0.1, 0.9, 0.0)), down, 60.0F * deg,
                 30.0F * deg, 30.0F * deg);
    ragdoll_hinge(w, r, 8U, 7U, 9U, nph_dvec3_add(base, nph_dvec3(-0.1, 0.46, 0.0)), right, 0.0F,
                  140.0F * deg);
    ragdoll_hinge(w, r, 9U, 8U, 10U, nph_dvec3_add(base, nph_dvec3(0.1, 0.46, 0.0)), right, 0.0F,
                  140.0F * deg);
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
                          &dolls[k]);
            for (u32 b = 0U; b < 11U; ++b) {
                nph_physics_body_set_angular_velocity(s.w, dolls[k].bodies[b],
                                                      test_vec3(&rng, -3.0F, 3.0F));
            }
        }
        f64 worst = 0.0;
        u32 slept = 0U;
        for (u32 i = 1U; i <= 240U * 8U; ++i) {
            step(s.w, NULL, 1U);
            for (u32 k = 0U; k < 4U; ++k) {
                for (u32 j = 0U; j < 10U; ++j) {
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
            NTEST_EXPECT_TRUE(sound(s.w, dolls[k].bodies, 11U, 50.0));
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
