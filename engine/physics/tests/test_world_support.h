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

static inline void scene_begin(Scene *s)
{
    s->alloc = test_alloc_begin(&s->ta);
    NphPhysicsWorldDesc desc = nph_physics_world_desc_default();
    desc.alloc = s->alloc;
    NTEST_ASSERT_EQ_INT(nph_physics_world_create(&desc, &s->w), NPH_RESULT_OK);
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
