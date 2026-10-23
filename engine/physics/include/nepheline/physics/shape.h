// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shapes: geometry attached to a body (docs/physics.md "Shapes").
//
// A shape places a geometry (geometry.h) in its body's frame with a pose,
// gives it a density (its share of the body's mass), surface properties,
// and a collision filter. A body may have any number of shapes; each is one
// broadphase proxy. A hull a shape uses must outlive the shape.
//
// Surfaces combine per contact: friction as the geometric mean (sqrt(a b):
// either surface can make the pair slippery), restitution as the larger.
//
// Thread-safety: as the world's (world.h).

#include <nepheline/physics/body.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which shapes may touch. Two shapes in the same non-zero group always
// collide (a positive group) or never do (a negative one); otherwise each
// one's category must be in the other's mask (Box2D's filter).
typedef struct NphPhysicsFilter {
    u64 category;
    u64 mask;
    i32 group;
} NphPhysicsFilter;

// Category bit 0, every mask bit, no group.
NphPhysicsFilter nph_physics_filter_default(void);

// True when shapes with these filters may touch.
bool nph_physics_filter_should_collide(NphPhysicsFilter a, NphPhysicsFilter b);

typedef struct NphPhysicsShapeDesc {
    NphPhysicsGeometry geometry; // valid (geometry.h)
    NphPhysicsPose pose;         // in the body's frame
    f32 density;                 // kg/m^3, >= 0 (0: no mass of its own)
    f32 friction;                // >= 0
    f32 restitution;             // in [0, 1]
    NphPhysicsFilter filter;
    b32 contact_events; // report begin and end touching (world.h)
    b32 hit_events;     // report hits above the world's threshold
    u64 user_data;
} NphPhysicsShapeDesc;

// `geometry` at the body's origin: density 1000 (water), friction 0.6,
// restitution 0, the default filter, contact events on, hit events off.
NphPhysicsShapeDesc nph_physics_shape_desc_default(NphPhysicsGeometry geometry);

// Attaches a shape and updates the body's mass properties (unless they are
// set explicitly). NPH_RESULT_INVALID_ARGUMENT for invalid geometry, a bad
// pose, or a negative density, friction, or restitution;
// NPH_RESULT_NOT_FOUND for a stale body; NPH_RESULT_FULL past
// NPH_PHYSICS_MAX_OBJECTS; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_physics_shape_create(NphPhysicsWorld *world, NphPhysicsBody body,
                                                const NphPhysicsShapeDesc *desc,
                                                NphPhysicsShape *out);

// Detaches and destroys the shape with its contacts, and updates its body's
// mass. A stale handle is ignored (asserted in development builds).
void nph_physics_shape_destroy(NphPhysicsWorld *world, NphPhysicsShape shape);

bool nph_physics_shape_is_valid(const NphPhysicsWorld *world, NphPhysicsShape shape);
NphPhysicsBody nph_physics_shape_body(const NphPhysicsWorld *world, NphPhysicsShape shape);
NphPhysicsGeometry nph_physics_shape_geometry(const NphPhysicsWorld *world, NphPhysicsShape shape);
NphPhysicsPose nph_physics_shape_pose(const NphPhysicsWorld *world, NphPhysicsShape shape);

f32 nph_physics_shape_friction(const NphPhysicsWorld *world, NphPhysicsShape shape);
f32 nph_physics_shape_restitution(const NphPhysicsWorld *world, NphPhysicsShape shape);
// New surface properties apply to contacts from the next step.
void nph_physics_shape_set_friction(NphPhysicsWorld *world, NphPhysicsShape shape, f32 friction);
void nph_physics_shape_set_restitution(NphPhysicsWorld *world, NphPhysicsShape shape,
                                       f32 restitution);

NphPhysicsFilter nph_physics_shape_filter(const NphPhysicsWorld *world, NphPhysicsShape shape);
// A new filter drops contacts it no longer allows and lets new pairs form at
// the next step.
void nph_physics_shape_set_filter(NphPhysicsWorld *world, NphPhysicsShape shape,
                                  NphPhysicsFilter filter);

u64 nph_physics_shape_user_data(const NphPhysicsWorld *world, NphPhysicsShape shape);
void nph_physics_shape_set_user_data(NphPhysicsWorld *world, NphPhysicsShape shape, u64 user_data);

// The shape's tight bounds in the world, at the last step (32-bit,
// rounded outward).
NphAABB nph_physics_shape_aabb(const NphPhysicsWorld *world, NphPhysicsShape shape);

#ifdef __cplusplus
}
#endif
