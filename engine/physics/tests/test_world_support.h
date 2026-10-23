// SPDX-License-Identifier: Apache-2.0
// Scenes for the world's tests (test_world.c, test_joint.c): a world on a
// leak-checking allocator, bodies with one shape, stepping, job systems.
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
