// SPDX-License-Identifier: Apache-2.0
#pragma once

// World queries (CLAUDE.md §8.9 "Queries"; docs/physics.md "Queries";
// docs/adr/0047-physics-queries.md): what a segment, a moving shape, a placed
// shape, or a box meets among a world's shapes.
//
//   nph_physics_world_raycast_closest     a segment's closest hit
//   nph_physics_world_raycast             each shape's hit, through a
//                                         callback that can clip the segment
//   nph_physics_world_cast_shape_closest  the closest hit of a convex shape
//                                         moved without turning
//   nph_physics_world_cast_shape          each shape's hit
//   nph_physics_world_overlap_shape       the shapes a convex shape touches
//   nph_physics_world_overlap_aabb        the shapes whose bounds overlap a
//                                         box
//   nph_physics_world_collide_shape       a convex shape's contact manifolds
//                                         with the world: the planes a
//                                         character slides along, and the
//                                         depth to push it out by
//
// A query walks the broadphase's trees (static, kinematic, then dynamic
// shapes) and tests the shapes it finds exactly, as collide.h's casts and
// overlaps do. Positions are 64-bit: each shape is tested in a frame
// centred on the query's origin (a mesh or a heightfield placed in 64 bits,
// as the world's contacts place them), so a query keeps full precision
// anywhere in the world; the trees' 32-bit boxes are walked grown by the
// rounding.
//
// A hit is per shape: a convex shape's first point, a mesh's or a
// heightfield's closest triangle. Hits come in the trees' order, not by
// distance; the same world and query give the same calls in the same
// order, from any thread.
//
// Costs: no allocation and no lock; a fixed stack.
//
// Thread-safety: a query only reads the world. Any number of threads may
// query a world at once while none changes or steps it (NPH_THREADSAFE
// between steps); callbacks must not change it.

#include <nepheline/physics/collide.h>
#include <nepheline/physics/shape.h>

#ifdef __cplusplus
extern "C" {
#endif

// The kinds of bodies a query looks at.
enum {
    NPH_PHYSICS_QUERY_STATIC = 1U << NPH_PHYSICS_BODY_STATIC,
    NPH_PHYSICS_QUERY_KINEMATIC = 1U << NPH_PHYSICS_BODY_KINEMATIC,
    NPH_PHYSICS_QUERY_DYNAMIC = 1U << NPH_PHYSICS_BODY_DYNAMIC,
    NPH_PHYSICS_QUERY_ALL_BODIES = 7U,
};

// Which shapes a query sees: those whose category shares a bit with the
// query's mask and whose mask shares one with its category (Box2D's query
// filter: a shape's own filter can keep queries off it), on a body of one of
// `body_types`, other than `ignore` (a character's own body).
typedef struct NphPhysicsQueryFilter {
    u64 category;
    u64 mask;
    u32 body_types;        // NPH_PHYSICS_QUERY_*
    NphPhysicsBody ignore; // NPH_PHYSICS_BODY_NULL: none
} NphPhysicsQueryFilter;

// Category bit 0 (shapes' default), every mask bit, every kind of body, and
// none ignored. Queries take NULL for it.
NphPhysicsQueryFilter nph_physics_query_filter_default(void);

typedef struct NphPhysicsQueryHit {
    NphPhysicsShape shape;
    NphPhysicsBody body;
    NphDVec3 point; // on the shape's surface, in the world
    NphVec3 normal; // unit: the surface's there, out of it (against the motion)
    f32 fraction;   // of the translation, in [0, 1]
    u32 triangle;   // a mesh's or a heightfield's (collide.h), or NPH_PHYSICS_NO_TRIANGLE
} NphPhysicsQueryHit;

// Called with each shape's hit. Returns how the cast goes on: the hit's
// fraction to look only for nearer hits (the closest hit's search), 1 for
// every hit, or a negative value to end it.
typedef f32 (*NphPhysicsQueryCastFn)(void *user, const NphPhysicsQueryHit *hit);

// Called with each shape found. Returning false ends the query.
typedef bool (*NphPhysicsQueryShapeFn)(void *user, NphPhysicsShape shape, NphPhysicsBody body);

typedef struct NphPhysicsQueryContact {
    NphPhysicsShape shape;
    NphPhysicsBody body;
    // As nph_physics_collide makes it with the query's shape first: the
    // normal from the query's shape towards this one, the points relative to
    // the query's position (add it, in 64 bits, for the world's), in the
    // world's axes.
    NphPhysicsManifold manifold;
} NphPhysicsQueryContact;

// Called with each manifold. Returning false ends the query.
typedef bool (*NphPhysicsQueryContactFn)(void *user, const NphPhysicsQueryContact *contact);

// The segment from `origin` to origin + `translation` against the shapes
// `filter` passes, each met as by nph_physics_raycast: a segment that starts
// inside a convex shape does not hit it. False for no hit (or a zero or
// non-finite translation); otherwise the closest, the first found of equals.
bool nph_physics_world_raycast_closest(const NphPhysicsWorld *world, NphDVec3 origin,
                                       NphVec3 translation, const NphPhysicsQueryFilter *filter,
                                       NphPhysicsQueryHit *out);

// As nph_physics_world_raycast_closest, with `fn` called for each shape's
// hit, and the segment clipped as it returns.
void nph_physics_world_raycast(const NphPhysicsWorld *world, NphDVec3 origin, NphVec3 translation,
                               const NphPhysicsQueryFilter *filter, NphPhysicsQueryCastFn fn,
                               void *user);

// Convex `geometry` (valid) at `position` with `rotation`, moved by
// `translation` without turning, against the shapes `filter` passes, each
// met as by nph_physics_shape_cast: shapes it already touches stop it at
// fraction 0 only if the motion goes into them. False for no hit (or a zero
// or non-finite translation); otherwise the closest, the first found of
// equals.
bool nph_physics_world_cast_shape_closest(const NphPhysicsWorld *world,
                                          const NphPhysicsGeometry *geometry, NphDVec3 position,
                                          NphQuat rotation, NphVec3 translation,
                                          const NphPhysicsQueryFilter *filter,
                                          NphPhysicsQueryHit *out);

// As nph_physics_world_cast_shape_closest, with `fn` called for each shape's
// hit.
void nph_physics_world_cast_shape(const NphPhysicsWorld *world, const NphPhysicsGeometry *geometry,
                                  NphDVec3 position, NphQuat rotation, NphVec3 translation,
                                  const NphPhysicsQueryFilter *filter, NphPhysicsQueryCastFn fn,
                                  void *user);

// The shapes `filter` passes that convex `geometry` (valid) at `position`
// with `rotation` touches or overlaps, as nph_physics_overlap decides.
void nph_physics_world_overlap_shape(const NphPhysicsWorld *world,
                                     const NphPhysicsGeometry *geometry, NphDVec3 position,
                                     NphQuat rotation, const NphPhysicsQueryFilter *filter,
                                     NphPhysicsQueryShapeFn fn, void *user);

// The shapes `filter` passes whose bounds (nph_physics_shape_aabb) overlap
// `box` (the world's frame, 32-bit), touching included.
void nph_physics_world_overlap_aabb(const NphPhysicsWorld *world, NphAABB box,
                                    const NphPhysicsQueryFilter *filter, NphPhysicsQueryShapeFn fn,
                                    void *user);

// Convex `geometry` (valid) at `position` with `rotation`: its contact
// manifolds with the shapes `filter` passes, as the world's contacts make
// them, within `speculative_distance` (>= 0): one for a convex shape, up to
// four for a mesh or a heightfield (one call each).
void nph_physics_world_collide_shape(const NphPhysicsWorld *world,
                                     const NphPhysicsGeometry *geometry, NphDVec3 position,
                                     NphQuat rotation, f32 speculative_distance,
                                     const NphPhysicsQueryFilter *filter,
                                     NphPhysicsQueryContactFn fn, void *user);

#ifdef __cplusplus
}
#endif
