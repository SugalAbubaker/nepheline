// SPDX-License-Identifier: Apache-2.0
// The ragdoll tests of Phase 11's exit criteria (CLAUDE.md §12): ragdolls
// dropped, thrown cartwheeling, sent down stairs, piled, and crushed under
// a crate, under every solver configuration (ADR-0045).
//
// Every configuration must stay sound: finite, its joints never more than
// a few centimetres apart or more than 15 degrees past their limits,
// nothing sinking far into the floor, and at rest everything closed and
// within its limits. The soft step, at any substeps, must also never rise
// above the energy it started with and put every scene to sleep. TGS and
// XPBD gain energy at violent impacts and can leave a limb stirring (their
// rigid rows fight the contacts: ADR-0045), so theirs need only never
// double their energy and end under 2 m/s (and TGS's resting overlap is
// its 5 mm slop). The default (the soft step at 4 substeps) holds bounds
// about two and a half times what it measures (ragdolls are chaotic: a
// compiler that rounds one operation differently sends them elsewhere).
// What each run measures is logged (run with --verbose).

#include <nepheline/math/kernels.h>

#include "test_world_support.h"

#include <string.h>

#define DEG       (k_pi / 180.0F)
#define MAX_DOLLS 16U

typedef struct Dolls {
    Ragdoll r[MAX_DOLLS];
    u32 count;
    NphPhysicsBody extra[2]; // other dynamic bodies: their energy counts too
    u32 extra_count;
} Dolls;

// What a run measured: the worst over its steps, then the state at its end.
typedef struct Watch {
    f64 gap;       // the widest any joint opened (m)
    f32 overshoot; // the farthest any joint went past a limit (radians)
    f64 start;     // the energy at the start (J): kinetic, and potential above y = 0
    f64 gain;      // the most it rose above the start (J)
    f64 rise;      // the most it rose above its lowest yet (J)
    f64 sink;      // the deepest any part went below the floor at y = 0 (m)
    f32 asleep;    // when everything first slept (s; 0: never)
    f32 fastest;   // at the end: the fastest point of any part (m/s)
    f64 rest_gap;
    f32 rest_overshoot;
    f64 rest_sink;
} Watch;

// The bounds a run keeps. The rise counts energy returned by compliant
// constraints (a soft joint squeezed 4.6 mm by 30 kN as a ragdoll lands
// on its straight legs gives back 69 J), so it is bounded, not zero.
typedef struct Bounds {
    f64 gap;            // m
    f64 rest_gap;       // m
    f32 overshoot;      // degrees
    f32 rest_overshoot; // degrees
    f64 sink;           // m
    f64 rest_sink;      // m
    f64 rise;           // a share of the start
    f32 asleep;         // s: asleep before it (0: need not sleep)
} Bounds;

// The soft step's at any substeps, and TGS's and XPBD's.
static const Bounds k_sound = {0.05, 0.0025, 15.0F, 1.0F, 0.05, 0.002, 1.0, 0.0F};
static const Bounds k_rigid = {0.05, 0.0025, 15.0F, 1.0F, 0.05, 0.006, 1e30, 0.0F};

// The joint's frames in the world.
static void frames(const NphPhysicsWorld *w, const NphPhysicsJointDesc *d, NphQuat *qa, NphQuat *qb)
{
    *qa = nph_quat_mul(nph_physics_body_rotation(w, d->body_a), d->frame_a.rotation);
    *qb = nph_quat_mul(nph_physics_body_rotation(w, d->body_b), d->frame_b.rotation);
}

// How far a joint is past its limits (radians; 0 within them): a hinge's
// range, a ball joint's twist and its cone, measured as the solver does.
static f32 overshoot(const NphPhysicsWorld *w, NphPhysicsJoint j)
{
    const NphPhysicsJointDesc d = nph_physics_joint_desc(w, j);
    const f32 angle = nph_physics_joint_hinge_angle(w, j); // a ball joint's twist
    if (d.type == NPH_PHYSICS_JOINT_HINGE) {
        return nph_f32_max(0.0F, nph_f32_max(d.hinge.lower - angle, angle - d.hinge.upper));
    }
    f32 over =
        nph_f32_max(0.0F, nph_f32_max(d.ball.twist_lower - angle, angle - d.ball.twist_upper));
    NphQuat qa;
    NphQuat qb;
    frames(w, &d, &qa, &qb);
    const NphVec3 xa = nph_quat_axis_x(qa);
    const NphVec3 xb = nph_quat_axis_x(qb);
    const NphVec3 n = nph_vec3_cross(xa, xb);
    const f32 sin_phi = nph_vec3_length(n);
    if (sin_phi > 1e-6F) {
        const NphVec3 local = nph_quat_rotate_inverse(qa, nph_vec3_scale(n, 1.0F / sin_phi));
        const f32 ey = local.y / d.ball.swing_y;
        const f32 ez = local.z / d.ball.swing_z;
        const f32 limit = 1.0F / nph_f32_sqrt(ey * ey + ez * ez);
        over = nph_f32_max(over, angle_between(xa, xb) - limit);
    }
    return over;
}

static f64 body_energy(const NphPhysicsWorld *w, NphPhysicsBody b, f64 g)
{
    const NphPhysicsMassData m = nph_physics_body_mass_data(w, b);
    const NphVec3 v = nph_physics_body_linear_velocity(w, b);
    const NphVec3 omega = nph_quat_rotate_inverse(nph_physics_body_rotation(w, b),
                                                  nph_physics_body_angular_velocity(w, b));
    const f64 spin = (f64)nph_vec3_dot(omega, nph_mat3_mul_vec3(m.inertia, omega));
    return 0.5 * (f64)m.mass * (f64)nph_vec3_length_sq(v) + 0.5 * spin +
           (f64)m.mass * g * nph_physics_body_center_of_mass(w, b).y;
}

static f64 energy(const NphPhysicsWorld *w, const Dolls *d)
{
    const f64 g = -(f64)nph_physics_world_gravity(w).y;
    f64 e = 0.0;
    for (u32 k = 0U; k < d->count; ++k) {
        for (u32 i = 0U; i < RAGDOLL_BODIES; ++i) {
            e += body_energy(w, d->r[k].bodies[i], g);
        }
    }
    for (u32 i = 0U; i < d->extra_count; ++i) {
        e += body_energy(w, d->extra[i], g);
    }
    return e;
}

static void joints_now(const NphPhysicsWorld *w, const Dolls *d, f64 *gap, f32 *over)
{
    *gap = 0.0;
    *over = 0.0F;
    for (u32 k = 0U; k < d->count; ++k) {
        for (u32 i = 0U; i < RAGDOLL_JOINTS; ++i) {
            *gap = fmax(*gap, joint_gap(w, d->r[k].joints[i]));
            *over = nph_f32_max(*over, overshoot(w, d->r[k].joints[i]));
        }
    }
}

static NphAABB part_bounds(const NphPhysicsWorld *w, NphPhysicsBody b)
{
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_UINT(nph_physics_body_shapes(w, b, &shape, 1U), 1U);
    return nph_physics_shape_aabb(w, shape);
}

static f64 sink_now(const NphPhysicsWorld *w, const Dolls *d)
{
    f64 deepest = 0.0;
    for (u32 k = 0U; k < d->count; ++k) {
        for (u32 i = 0U; i < RAGDOLL_BODIES; ++i) {
            deepest = fmax(deepest, -(f64)part_bounds(w, d->r[k].bodies[i]).min.y);
        }
    }
    return deepest;
}

// The speed of the fastest point of any part (the sleep test's measure,
// Box2D's: the centre's speed, or the spin's at the shape's extent).
static f32 fastest_now(const NphPhysicsWorld *w, const Dolls *d)
{
    f32 fastest = 0.0F;
    for (u32 k = 0U; k < d->count; ++k) {
        for (u32 i = 0U; i < RAGDOLL_BODIES; ++i) {
            const NphPhysicsBody b = d->r[k].bodies[i];
            const NphAABB box = part_bounds(w, b);
            const f32 extent = 0.5F * nph_vec3_length(nph_vec3_sub(box.max, box.min));
            fastest = nph_f32_max(
                fastest,
                nph_f32_max(nph_vec3_length(nph_physics_body_linear_velocity(w, b)),
                            extent * nph_vec3_length(nph_physics_body_angular_velocity(w, b))));
        }
    }
    return fastest;
}

static bool dolls_sound(const NphPhysicsWorld *w, const Dolls *d)
{
    for (u32 k = 0U; k < d->count; ++k) {
        if (!sound(w, d->r[k].bodies, RAGDOLL_BODIES, 50.0)) {
            return false;
        }
    }
    return true;
}

// Steps `seconds`, watching.
static Watch watch(NphPhysicsWorld *w, const Dolls *d, f32 seconds)
{
    Watch m;
    memset(&m, 0, sizeof m);
    m.start = energy(w, d);
    f64 lowest = m.start;
    const u32 steps = (u32)(seconds * 240.0F);
    for (u32 i = 1U; i <= steps; ++i) {
        step(w, NULL, 1U);
        f64 gap;
        f32 over;
        joints_now(w, d, &gap, &over);
        m.gap = fmax(m.gap, gap);
        m.overshoot = nph_f32_max(m.overshoot, over);
        const f64 e = energy(w, d);
        lowest = fmin(lowest, e);
        m.gain = fmax(m.gain, e - m.start);
        m.rise = fmax(m.rise, e - lowest);
        m.sink = fmax(m.sink, sink_now(w, d));
        if (m.asleep == 0.0F && nph_physics_world_stats(w).awake_bodies == 0U) {
            m.asleep = (f32)i / 240.0F;
        }
    }
    joints_now(w, d, &m.rest_gap, &m.rest_overshoot);
    m.rest_sink = sink_now(w, d);
    m.fastest = fastest_now(w, d);
    return m;
}

static void report(u32 config, const char *what, const Watch *m)
{
    NTEST_LOG("%-9s %s: joints open %.2f mm at worst, %.3f at rest; past a limit %.2f deg at "
              "worst, %.3f at rest",
              k_configs[config].name, what, m->gap * 1e3, m->rest_gap * 1e3,
              (f64)(m->overshoot / DEG), (f64)(m->rest_overshoot / DEG));
    NTEST_LOG("%-9s   energy rose %.1f J at most (of %.0f J); sank %.2f mm at worst, %.2f at rest",
              k_configs[config].name, m->rise, m->start, m->sink * 1e3, m->rest_sink * 1e3);
    if (m->asleep > 0.0F) {
        NTEST_LOG("%-9s   asleep after %.2f s", k_configs[config].name, (f64)m->asleep);
    } else {
        NTEST_LOG("%-9s   never asleep: the fastest part at %.3f m/s at the end",
                  k_configs[config].name, (f64)m->fastest);
    }
}

static void check(const Watch *m, const Bounds *b)
{
    NTEST_EXPECT_TRUE(m->gap < b->gap);
    NTEST_EXPECT_TRUE(m->rest_gap < b->rest_gap);
    NTEST_EXPECT_TRUE(m->overshoot < b->overshoot * DEG);
    NTEST_EXPECT_TRUE(m->rest_overshoot < b->rest_overshoot * DEG);
    NTEST_EXPECT_TRUE(m->sink < b->sink);
    NTEST_EXPECT_TRUE(m->rest_sink < b->rest_sink);
    NTEST_EXPECT_TRUE(m->rise <= b->rise * m->start);
    if (b->asleep > 0.0F) {
        NTEST_EXPECT_TRUE(m->asleep > 0.0F && m->asleep < b->asleep);
    }
}

// Checks a run: sound under every configuration; under the soft step never
// above its starting energy and asleep before `asleep`, under TGS and XPBD
// never at twice it and all but still; and the default within `tight`.
static void check_run(u32 config, const NphPhysicsWorld *w, const Dolls *d, const Watch *m,
                      f32 asleep, const Bounds *tight)
{
    NTEST_EXPECT_TRUE(dolls_sound(w, d));
    if (k_configs[config].solver == NPH_PHYSICS_SOLVER_SOFT_STEP) {
        check(m, &k_sound);
        NTEST_EXPECT_TRUE(m->gain <= 1e-3 * m->start);
        NTEST_EXPECT_TRUE(m->asleep > 0.0F && m->asleep < asleep);
    } else {
        check(m, &k_rigid);
        NTEST_EXPECT_TRUE(m->gain <= m->start);
        NTEST_EXPECT_TRUE(m->asleep > 0.0F || m->fastest < 2.0F);
    }
    if (is_default(config)) {
        check(m, tight);
    }
}

// Sets the ragdoll moving as one rigid body: at `v` at its pelvis, turning
// at `omega`.
static void throw_ragdoll(NphPhysicsWorld *w, const Ragdoll *r, NphVec3 v, NphVec3 omega)
{
    const NphDVec3 about = nph_physics_body_center_of_mass(w, r->bodies[0]);
    for (u32 i = 0U; i < RAGDOLL_BODIES; ++i) {
        const NphVec3 arm =
            nph_dvec3_relative(nph_physics_body_center_of_mass(w, r->bodies[i]), about);
        nph_physics_body_set_linear_velocity(w, r->bodies[i],
                                             nph_vec3_add(v, nph_vec3_cross(omega, arm)));
        nph_physics_body_set_angular_velocity(w, r->bodies[i], omega);
    }
}

// On its back with its head towards -Z.
static NphQuat lying(void)
{
    return nph_quat_from_axis_angle(nph_vec3(1.0F, 0.0F, 0.0F), -90.0F * DEG);
}

// `count` ragdolls lying criss-cross in a heap over the origin, one every
// 0.3 m up, each a quarter turn from the last, nudged at random.
static void build_pile(NphPhysicsWorld *w, u32 count, Dolls *d)
{
    NphPcg32 rng = test_rng(2026U);
    d->count = count;
    for (u32 k = 0U; k < count; ++k) {
        const NphQuat turn = nph_quat_mul(
            nph_quat_from_axis_angle(nph_vec3(0.0F, 1.0F, 0.0F), (f32)k * 90.0F * DEG), lying());
        const NphDVec3 pelvis = nph_dvec3(0.0, 0.15 + 0.3 * (f64)k, 0.0);
        build_ragdoll(w, nph_dvec3_sub(pelvis, ragdoll_turn(turn, nph_dvec3(0.0, 1.0, 0.0))), turn,
                      &d->r[k]);
        throw_ragdoll(w, &d->r[k], test_vec3(&rng, -0.5F, 0.5F), test_vec3(&rng, -1.0F, 1.0F));
    }
}

// ---------------------------------------------------------------------------
// Falls
// ---------------------------------------------------------------------------

NTEST(ragdoll, a_dropped_ragdoll_settles_and_sleeps)
{
    // The default measures 7.6 mm, 1.1 deg, 1.7 mm sunk, 5.4 % back, 4.8 s.
    const Bounds tight = {0.02, 0.00025, 3.0F, 0.1F, 0.006, 0.0005, 0.12, 8.0F};
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // Upright in its T-pose, its feet 1 m up: it lands on its straight
        // legs, crumples, and falls over.
        Dolls d = {.count = 1U};
        build_ragdoll(s.w, nph_dvec3(0.0, 0.95, 0.0), nph_quat_identity(), &d.r[0]);
        const Watch m = watch(s.w, &d, 10.0F);
        report(config, "dropped 1 m", &m);
        check_run(config, s.w, &d, &m, 9.5F, &tight);
        scene_end(&s);
    }
}

NTEST(ragdoll, limits_hold_through_a_tumble)
{
    // The default measures 12 mm, 5.3 deg, 5.9 mm sunk, 1.3 % back, 2.0 s.
    const Bounds tight = {0.03, 0.00025, 10.0F, 0.1F, 0.015, 0.0005, 0.04, 5.0F};
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // Thrown cartwheeling at the ground: 5 m/s across and 3 down,
        // turning at 8 rad/s about its belly and 4 about its spine.
        Dolls d = {.count = 1U};
        build_ragdoll(s.w, nph_dvec3(0.0, 1.5, 0.0), nph_quat_identity(), &d.r[0]);
        throw_ragdoll(s.w, &d.r[0], nph_vec3(5.0F, -3.0F, 0.0F), nph_vec3(0.0F, 4.0F, 8.0F));
        const Watch m = watch(s.w, &d, 8.0F);
        report(config, "thrown", &m);
        check_run(config, s.w, &d, &m, 7.5F, &tight);
        scene_end(&s);
    }
}

NTEST(ragdoll, tumbles_down_stairs)
{
    // The default measures 3.0 mm, 1.3 deg, 0.1 % back, 1.9 s.
    const Bounds tight = {0.01, 0.00025, 3.5F, 0.1F, 0.002, 0.0005, 0.01, 5.0F};
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // Eight steps 0.2 m high and 0.4 m deep, down along +X.
        for (u32 i = 0U; i < 8U; ++i) {
            const f32 top = 0.2F * (f32)(8U - i);
            add_body(s.w, NPH_PHYSICS_BODY_STATIC,
                     nph_dvec3(0.4 * (f64)i + 0.2, 0.5 * (f64)top, 0.0), nph_quat_identity(),
                     nph_physics_geometry_box(nph_vec3(0.2F, 0.5F * top, 1.5F)), 0.6F, 0.0F);
        }
        // On the top step facing down them, toppling forward.
        Dolls d = {.count = 1U};
        build_ragdoll(s.w, nph_dvec3(0.2, 1.6, 0.0),
                      nph_quat_from_axis_angle(nph_vec3(0.0F, 1.0F, 0.0F), 90.0F * DEG), &d.r[0]);
        throw_ragdoll(s.w, &d.r[0], nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(0.0F, 0.0F, -2.0F));
        const Watch m = watch(s.w, &d, 10.0F);
        report(config, "down the stairs", &m);
        const NphDVec3 pelvis = nph_physics_body_position(s.w, d.r[0].bodies[0]);
        NTEST_LOG("%-9s   the pelvis ends %.2f m on and %.2f m down", k_configs[config].name,
                  pelvis.x - 0.2, 2.6 - pelvis.y);
        // It went down four steps or more.
        NTEST_EXPECT_TRUE(pelvis.x > 1.0 && pelvis.y < 1.8);
        check_run(config, s.w, &d, &m, 7.5F, &tight);
        scene_end(&s);
    }
}

// ---------------------------------------------------------------------------
// Heaps and loads
// ---------------------------------------------------------------------------

NTEST(ragdoll, a_pile_settles_without_gaining_energy)
{
    // Sixteen where the build is fast enough, four in Debug and under the
    // sanitizers. The default measures, for sixteen, 8.3 mm, 0.47 at rest,
    // 3.2 deg, 0.26 at rest, 5.1 mm sunk, 0.31 at rest, 0.2 % back, 3.4 s.
    const bool big = TEST_SCALE >= 8U;
    const u32 count = big ? 16U : 4U;
    const Bounds tight = {0.025, 0.001, 10.0F, 0.8F, 0.013, 0.0005, 0.01, big ? 10.0F : 5.0F};
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        Dolls d;
        memset(&d, 0, sizeof d);
        build_pile(s.w, count, &d);
        const Watch m = watch(s.w, &d, big ? 12.0F : 6.0F);
        report(config, big ? "a pile of 16" : "a pile of 4", &m);
        check_run(config, s.w, &d, &m, big ? 11.5F : 5.5F, &tight);
        scene_end(&s);
    }
}

NTEST(ragdoll, a_crate_lands_on_a_lying_ragdoll)
{
    // The default measures 2.1 mm, 0.1 deg, 2.2 mm sunk, 1.7 % back, 1.6 s.
    const Bounds tight = {0.006, 0.00025, 0.5F, 0.1F, 0.006, 0.0005, 0.05, 4.0F};
    for (u32 config = 0U; config < CONFIGS; ++config) {
        Scene s;
        solver_scene(&s, config);
        add_ground(s.w, 0.0);
        // On its back, and a 216 kg crate (ten times its chest) dropped
        // 0.5 m onto its chest.
        Dolls d = {.count = 1U, .extra_count = 1U};
        build_ragdoll(s.w, nph_dvec3(0.0, 0.105, 0.0), lying(), &d.r[0]);
        d.extra[0] = add_box(s.w, nph_dvec3(0.0, 1.005, -1.32), nph_vec3_splat(0.3F));
        const Watch m = watch(s.w, &d, 6.0F);
        report(config, "under a crate", &m);
        check_run(config, s.w, &d, &m, 5.0F, &tight);
        scene_end(&s);
    }
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

NTEST(ragdoll, replays_bit_for_bit_on_any_worker_count_and_kernel)
{
    // A pile under each solver, its hash every quarter second the same as
    // a single-thread run's when rerun in a new world, on 4 and 8 workers
    // (5 and 9 participants forced), and on the AVX2 kernel: six ragdolls
    // for 2 s, or three for 1 s in Debug and under the sanitizers.
    const u32 count = TEST_SCALE >= 8U ? 6U : 3U;
    const u32 checks = TEST_SCALE >= 8U ? 8U : 4U;
    const bool avx2 = nph_simd_detect_level() >= NPH_SIMD_LEVEL_AVX2;
    const u32 workers[5] = {0U, 0U, 4U, 8U, 0U};
    const u32 forced[5] = {0U, 0U, 5U, 9U, 0U};
    for (u32 solver = 0U; solver < (u32)NPH_PHYSICS_SOLVER_COUNT; ++solver) {
        u64 reference[8];
        for (u32 run = 0U; run < (avx2 ? 5U : 4U); ++run) {
            NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
            Scene s;
            NphPhysicsWorldDesc desc = nph_physics_world_desc_default();
            desc.solver = (NphPhysicsSolver)solver;
            scene_begin_with(&s, desc);
            s.w->kernels = run == 4U ? &g_phys_solver_avx2 : &g_phys_solver_sse4;
            s.w->forced_participants = forced[run];
            add_ground(s.w, 0.0);
            Dolls d;
            memset(&d, 0, sizeof d);
            build_pile(s.w, count, &d);
            for (u32 i = 0U; i < checks; ++i) {
                step(s.w, js, 60U);
                const u64 h = nph_physics_world_hash(s.w);
                if (run == 0U) {
                    reference[i] = h;
                } else {
                    NTEST_EXPECT_EQ_UINT(h, reference[i]);
                }
            }
            scene_end(&s);
            nph_job_system_destroy(js);
        }
    }
}
