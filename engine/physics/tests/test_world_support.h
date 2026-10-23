// SPDX-License-Identifier: Apache-2.0
// Scenes for the world's tests (test_world.c, test_joint.c, test_solvers.c,
// test_ragdoll.c): a world on a leak-checking allocator, bodies with one
// shape, stepping, job systems, joints, the solver configurations, and
// ragdolls.
#pragma once

#include <nepheline/jobs/jobs.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/joint.h>
#include <nepheline/physics/shape.h>
#include <nepheline/physics/world.h>

#include "test_physics_support.h"
#include "world_internal.h"

#define DT (1.0F / 240.0F)

typedef struct Scene {
    TestAlloc ta;
    NphAllocator *alloc;
    NphPhysicsWorld *w;
} Scene;

// A world from `desc` (its allocator replaced by the scene's).
static inline void scene_begin_with(Scene *s, NphPhysicsWorldDesc desc)
{
    s->alloc = test_alloc_begin(&s->ta);
    desc.alloc = s->alloc;
    NTEST_ASSERT_EQ_INT(nph_physics_world_create(&desc, &s->w), NPH_RESULT_OK);
}

static inline void scene_begin(Scene *s)
{
    scene_begin_with(s, nph_physics_world_desc_default());
}

static inline void scene_end(Scene *s)
{
    NTEST_EXPECT_TRUE(nph_physics_world_validate(s->w));
    nph_physics_world_destroy(s->w);
    test_alloc_end(&s->ta);
}

static inline NphPhysicsBody add_body(NphPhysicsWorld *w, NphPhysicsBodyType type,
                                      NphDVec3 position, NphQuat rotation,
                                      NphPhysicsGeometry geometry, f32 friction, f32 restitution)
{
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.type = type;
    bd.position = position;
    bd.rotation = rotation;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(w, &bd, &b), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(geometry);
    sd.friction = friction;
    sd.restitution = restitution;
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(w, b, &sd, &shape), NPH_RESULT_OK);
    return b;
}

// A static slab whose top face is the plane y = top.
static inline NphPhysicsBody add_ground(NphPhysicsWorld *w, f64 top)
{
    return add_body(w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(0.0, top - 1.0, 0.0), nph_quat_identity(),
                    nph_physics_geometry_box(nph_vec3(50.0F, 1.0F, 50.0F)), 0.6F, 0.0F);
}

static inline NphPhysicsBody add_box(NphPhysicsWorld *w, NphDVec3 p, NphVec3 half)
{
    return add_body(w, NPH_PHYSICS_BODY_DYNAMIC, p, nph_quat_identity(),
                    nph_physics_geometry_box(half), 0.6F, 0.0F);
}

static inline void step(NphPhysicsWorld *w, NphJobSystem *js, u32 steps)
{
    for (u32 i = 0U; i < steps; ++i) {
        NTEST_ASSERT_EQ_INT(nph_physics_world_step(w, DT, js), NPH_RESULT_OK);
    }
}

static inline NphJobSystem *make_jobs(u32 workers)
{
    NphJobSystemDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.worker_count = workers;
    d.pinning = NPH_JOB_PIN_NEVER;
    NphJobSystem *js = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&d, &js), NPH_RESULT_OK);
    return js;
}

// ---------------------------------------------------------------------------
// Joints (test_joint.c, test_solvers.c)
// ---------------------------------------------------------------------------

static const f32 k_pi = 3.14159265F;

// A static body without shapes: an anchor in the world.
static inline NphPhysicsBody add_anchor(NphPhysicsWorld *w, NphDVec3 p)
{
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.type = NPH_PHYSICS_BODY_STATIC;
    bd.position = p;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(w, &bd, &b), NPH_RESULT_OK);
    return b;
}

static inline NphPhysicsBody add_dynamic(NphPhysicsWorld *w, NphDVec3 p, NphPhysicsGeometry g)
{
    return add_body(w, NPH_PHYSICS_BODY_DYNAMIC, p, nph_quat_identity(), g, 0.6F, 0.0F);
}

// A joint described by `d`, its frames meeting at `anchor` with X along
// `axis` for the bodies' current poses.
static inline NphPhysicsJoint join(NphPhysicsWorld *w, NphPhysicsJointDesc d, NphDVec3 anchor,
                                   NphVec3 axis)
{
    nph_physics_joint_frames_at(w, d.body_a, d.body_b, anchor, axis, &d.frame_a, &d.frame_b);
    NphPhysicsJoint j;
    NTEST_ASSERT_EQ_INT(nph_physics_joint_create(w, &d, &j), NPH_RESULT_OK);
    return j;
}

static inline NphPhysicsJoint ball_at(NphPhysicsWorld *w, NphPhysicsBody a, NphPhysicsBody b,
                                      NphDVec3 anchor)
{
    return join(w, nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_BALL, a, b), anchor,
                nph_vec3(1.0F, 0.0F, 0.0F));
}

static inline f64 dist(NphDVec3 a, NphDVec3 b)
{
    const NphDVec3 d = nph_dvec3_sub(a, b);
    return sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

// The world positions of a joint's two anchors.
static inline void anchors(const NphPhysicsWorld *w, NphPhysicsJoint j, NphDVec3 *pa, NphDVec3 *pb)
{
    const NphPhysicsJointDesc d = nph_physics_joint_desc(w, j);
    *pa = nph_dvec3_add_vec3(
        nph_physics_body_position(w, d.body_a),
        nph_quat_rotate(nph_physics_body_rotation(w, d.body_a), d.frame_a.position));
    *pb = nph_dvec3_add_vec3(
        nph_physics_body_position(w, d.body_b),
        nph_quat_rotate(nph_physics_body_rotation(w, d.body_b), d.frame_b.position));
}

static inline f64 joint_gap(const NphPhysicsWorld *w, NphPhysicsJoint j)
{
    NphDVec3 pa;
    NphDVec3 pb;
    anchors(w, j, &pa, &pb);
    return dist(pa, pb);
}

// The frames' X axes in the world.
static inline void frame_axes(const NphPhysicsWorld *w, NphPhysicsJoint j, NphVec3 *xa, NphVec3 *xb)
{
    const NphPhysicsJointDesc d = nph_physics_joint_desc(w, j);
    *xa = nph_quat_axis_x(nph_quat_mul(nph_physics_body_rotation(w, d.body_a), d.frame_a.rotation));
    *xb = nph_quat_axis_x(nph_quat_mul(nph_physics_body_rotation(w, d.body_b), d.frame_b.rotation));
}

// Sets a body turning at `w` about `point`: its centre of mass moves with
// the turn, so a joint there has nothing to take out.
static inline void swing_about(NphPhysicsWorld *w, NphPhysicsBody b, NphDVec3 point, NphVec3 omega)
{
    const NphVec3 arm = nph_dvec3_relative(nph_physics_body_center_of_mass(w, b), point);
    nph_physics_body_set_angular_velocity(w, b, omega);
    nph_physics_body_set_linear_velocity(w, b, nph_vec3_cross(omega, arm));
}

static inline f32 angle_between(NphVec3 a, NphVec3 b)
{
    return nph_f32_atan2(nph_vec3_length(nph_vec3_cross(a, b)), nph_vec3_dot(a, b));
}

// A chain of capsule links hanging (or lying along +X) from an anchor.
static inline void build_chain(NphPhysicsWorld *w, NphPhysicsBody top, NphDVec3 start, u32 links,
                               bool horizontal, NphPhysicsBody *out_links,
                               NphPhysicsJoint *out_joints)
{
    const f64 spacing = 0.5;
    NphPhysicsBody prev = top;
    for (u32 i = 0U; i < links; ++i) {
        const f64 along = spacing * ((f64)i + 0.5);
        const NphDVec3 center = horizontal ? nph_dvec3_add(start, nph_dvec3(along, 0.0, 0.0))
                                           : nph_dvec3_add(start, nph_dvec3(0.0, -along, 0.0));
        const NphQuat q = horizontal
                              ? nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), 0.5F * k_pi)
                              : nph_quat_identity();
        const NphPhysicsBody link = add_body(w, NPH_PHYSICS_BODY_DYNAMIC, center, q,
                                             nph_physics_geometry_capsule(0.2F, 0.05F), 0.6F, 0.0F);
        const f64 at = spacing * (f64)i;
        const NphDVec3 anchor = horizontal ? nph_dvec3_add(start, nph_dvec3(at, 0.0, 0.0))
                                           : nph_dvec3_add(start, nph_dvec3(0.0, -at, 0.0));
        out_joints[i] = ball_at(w, prev, link, anchor);
        out_links[i] = link;
        prev = link;
    }
}

// ---------------------------------------------------------------------------
// Solvers (test_solvers.c, test_ragdoll.c)
// ---------------------------------------------------------------------------

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

static inline void solver_scene(Scene *s, u32 config)
{
    NphPhysicsWorldDesc d = nph_physics_world_desc_default();
    d.solver = k_configs[config].solver;
    d.substeps = k_configs[config].substeps;
    scene_begin_with(s, d);
}

// The default configuration: where the quality thresholds apply.
static inline bool is_default(u32 config)
{
    return config == 0U;
}

// Every body finite and within `radius` of the origin.
static inline bool sound(const NphPhysicsWorld *w, const NphPhysicsBody *bodies, u32 count,
                         f64 radius)
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
// Ragdolls (test_solvers.c, test_ragdoll.c)
// ---------------------------------------------------------------------------

#define RAGDOLL_BODIES 11U
#define RAGDOLL_JOINTS 10U

// A ragdoll of eleven bodies: pelvis (bodies[0]), chest, head, upper and
// lower arms and legs, ball joints with cones and twists at the spine,
// neck, shoulders, and hips, hinges at the elbows and knees. It stands
// 1.7 m tall in a T-pose facing +Z, its feet 5 cm above `base`, turned by
// `turn` about `base`: 66 kg of water-dense parts.
typedef struct Ragdoll {
    NphPhysicsBody bodies[RAGDOLL_BODIES];
    NphPhysicsJoint joints[RAGDOLL_JOINTS];
} Ragdoll;

// `v` turned by `q`, in 64 bits (exactly `v` for the identity).
static inline NphDVec3 ragdoll_turn(NphQuat q, NphDVec3 v)
{
    const NphDVec3 u = nph_dvec3((f64)q.x, (f64)q.y, (f64)q.z);
    const NphDVec3 t = nph_dvec3_scale(nph_dvec3_cross(u, v), 2.0);
    return nph_dvec3_add(nph_dvec3_add(v, nph_dvec3_scale(t, (f64)q.w)), nph_dvec3_cross(u, t));
}

static inline void build_ragdoll(NphPhysicsWorld *w, NphDVec3 base, NphQuat turn, Ragdoll *r)
{
    const f32 deg = k_pi / 180.0F;
    const NphQuat side = nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), 90.0F * deg);
    const struct {
        f64 x, y;
        NphPhysicsGeometry g;
        bool sideways;
    } parts[RAGDOLL_BODIES] = {
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
    for (u32 i = 0U; i < RAGDOLL_BODIES; ++i) {
        r->bodies[i] = add_body(
            w, NPH_PHYSICS_BODY_DYNAMIC,
            nph_dvec3_add(base, ragdoll_turn(turn, nph_dvec3(parts[i].x, parts[i].y, 0.0))),
            nph_quat_mul(turn, parts[i].sideways ? side : nph_quat_identity()), parts[i].g, 0.6F,
            0.0F);
    }
    // Each joint's bodies, anchor, and X axis in the T-pose; a ball joint's
    // swing half angles (about its frame's Y and Z) and twist, a hinge's
    // range. The elbows and knees bend the anatomical way: the forearms
    // forward, the shins back. Every joint has 2 N m of friction, as a limp
    // body's muscles do: without it a dangling limb swings for ever.
    const NphVec3 up = nph_vec3(0.0F, 1.0F, 0.0F);
    const NphVec3 down = nph_vec3(0.0F, -1.0F, 0.0F);
    const NphVec3 left = nph_vec3(-1.0F, 0.0F, 0.0F);
    const NphVec3 right = nph_vec3(1.0F, 0.0F, 0.0F);
    const struct {
        u32 a, b;
        f64 x, y;
        NphVec3 axis;
        bool hinge;
        f32 p0, p1, p2; // degrees: swing_y, swing_z, twist; or lower, upper
    } joints[RAGDOLL_JOINTS] = {
        {0U, 1U, 0.0, 1.12, up, false, 30.0F, 20.0F, 20.0F},     // spine
        {1U, 2U, 0.0, 1.5, up, false, 40.0F, 40.0F, 45.0F},      // neck
        {1U, 3U, -0.17, 1.42, left, false, 80.0F, 60.0F, 45.0F}, // shoulders
        {1U, 4U, 0.17, 1.42, right, false, 80.0F, 60.0F, 45.0F},
        {3U, 5U, -0.48, 1.4, up, true, 0.0F, 140.0F, 0.0F}, // elbows
        {4U, 6U, 0.48, 1.4, down, true, 0.0F, 140.0F, 0.0F},
        {0U, 7U, -0.1, 0.9, down, false, 60.0F, 30.0F, 30.0F}, // hips
        {0U, 8U, 0.1, 0.9, down, false, 60.0F, 30.0F, 30.0F},
        {7U, 9U, -0.1, 0.46, right, true, 0.0F, 140.0F, 0.0F}, // knees
        {8U, 10U, 0.1, 0.46, right, true, 0.0F, 140.0F, 0.0F},
    };
    for (u32 i = 0U; i < RAGDOLL_JOINTS; ++i) {
        const NphPhysicsBody a = r->bodies[joints[i].a];
        const NphPhysicsBody b = r->bodies[joints[i].b];
        NphPhysicsJointDesc d = nph_physics_joint_desc_default(
            joints[i].hinge ? NPH_PHYSICS_JOINT_HINGE : NPH_PHYSICS_JOINT_BALL, a, b);
        if (joints[i].hinge) {
            d.hinge.enable_limit = 1;
            d.hinge.lower = joints[i].p0 * deg;
            d.hinge.upper = joints[i].p1 * deg;
            d.hinge.enable_motor = 1; // a motor to standstill: friction
            d.hinge.max_motor_torque = 2.0F;
        } else {
            d.ball.enable_swing_limit = 1;
            d.ball.swing_y = joints[i].p0 * deg;
            d.ball.swing_z = joints[i].p1 * deg;
            d.ball.enable_twist_limit = 1;
            d.ball.twist_lower = -joints[i].p2 * deg;
            d.ball.twist_upper = joints[i].p2 * deg;
            d.ball.friction_torque = 2.0F;
        }
        const NphDVec3 at =
            nph_dvec3_add(base, ragdoll_turn(turn, nph_dvec3(joints[i].x, joints[i].y, 0.0)));
        r->joints[i] = join(w, d, at, nph_quat_rotate(turn, joints[i].axis));
    }
}
