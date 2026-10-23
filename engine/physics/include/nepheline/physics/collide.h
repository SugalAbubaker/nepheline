// SPDX-License-Identifier: Apache-2.0
#pragma once

// Narrow-phase collision (CLAUDE.md §8.9 "Narrowphase"; docs/physics.md
// "Collision"): contact manifolds between two placed geometries, and the
// queries they are built from.
//
//   nph_physics_collide      the manifold of two geometries: a normal and up
//                            to four points, with separations, within a
//                            speculative distance
//   nph_physics_distance     GJK: the closest points of two convex proxies
//   nph_physics_penetration  GJK, then EPA when the cores overlap: the
//                            smallest translation that separates them
//
// Both poses are given in one frame, and every output is in that frame. The
// world uses a frame centred on one of the two bodies, so the coordinates are
// small and keep full precision anywhere in a 64-bit world (types.h).
//
// The algorithms (docs/physics.md has the details and the reasoning):
//   - spheres and capsules: closed forms on points and segments (Ericson,
//     "Real-Time Collision Detection", 2005, §5.1);
//   - boxes and hulls: the separating axis test over face normals and the
//     edge pairs that form faces of the Minkowski difference (Gregorius,
//     "The Separating Axis Test between Convex Polyhedra", GDC 2013), then
//     clipping the incident face against the reference face (Sutherland and
//     Hodgman 1974), reduced to four points that keep the largest area
//     (Gregorius, "Robust Contact Creation for Physics Simulations", GDC
//     2015). When the hulls are apart, GJK gives the exact gap the
//     speculative contacts need;
//   - GJK with the signed-volumes subalgorithm (Montanari, Petrinic, and
//     Barbieri, "Improving the GJK Algorithm for Faster and More Reliable
//     Distance Queries Between Convex Objects", ACM TOG 2017) and EPA (van
//     den Bergen, "Proximity Queries and Penetration Depth Computation on 3D
//     Game Objects", GDC 2001).
//
// Thread-safety: every function is NPH_THREADSAFE; a cache belongs to one
// shape pair and is written by the call that receives it.

#include <nepheline/physics/geometry.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Manifolds
// ---------------------------------------------------------------------------

#define NPH_PHYSICS_MANIFOLD_MAX_POINTS 4U

typedef struct NphPhysicsManifoldPoint {
    NphVec3 point;  // midway between the two surfaces
    f32 separation; // along the normal; negative when the shapes overlap
    // The features that produced the point. Equal ids across two calls mean
    // the same contact, which is how the world carries a point's impulses
    // from one step to the next (warm starting).
    u32 id;
} NphPhysicsManifoldPoint;

typedef struct NphPhysicsManifold {
    NphVec3 normal;  // unit, from shape A towards shape B
    u32 point_count; // 0: farther apart than the speculative distance
    NphPhysicsManifoldPoint points[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
} NphPhysicsManifold;

// What the previous call on a shape pair found, so the next one can start
// there: when the axis that separated the pair last time still separates it
// by more than the speculative distance, the test ends after one axis
// (Gregorius 2013, "temporal coherence"). Zero-initialised: no history.
typedef struct NphPhysicsCollideCache {
    u8 type; // internal: which kind of axis
    u8 reserved;
    u16 index_a;
    u16 index_b;
    u16 reserved2;
} NphPhysicsCollideCache;

NPH_STATIC_ASSERT(sizeof(NphPhysicsCollideCache) == 8U, "the collision cache is eight bytes");

// True when nph_physics_collide handles the pair (every pair of the shape
// types in geometry.h).
bool nph_physics_collide_supported(NphPhysicsShapeType a, NphPhysicsShapeType b);

// The contact manifold of `a` at `pose_a` and `b` at `pose_b`: points whose
// separation is at most `speculative_distance` (metres, >= 0), at most four,
// spread to cover the contact area. `cache` may be NULL. Pairs given in
// either order work: (box, sphere) and (sphere, box) give the same points
// with opposite normals.
void nph_physics_collide(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                         const NphPhysicsGeometry *b, NphPhysicsPose pose_b,
                         f32 speculative_distance, NphPhysicsCollideCache *cache,
                         NphPhysicsManifold *out);

// Reduces `count` candidate points to at most four, in place, and returns
// how many remain: the deepest, the one farthest from it, the one that makes
// the largest triangle with both, and the one that adds the most area beyond
// that triangle (Gregorius 2015). Areas are measured in the plane
// perpendicular to `normal`. Counts of four or fewer are left unchanged.
u32 nph_physics_manifold_reduce(NphPhysicsManifoldPoint *points, u32 count, NphVec3 normal);

// ---------------------------------------------------------------------------
// Closest points
// ---------------------------------------------------------------------------

typedef struct NphPhysicsSegmentDistance {
    NphVec3 closest1; // on segment 1
    NphVec3 closest2; // on segment 2
    f32 fraction1;    // closest1 = p1 + fraction1 (q1 - p1)
    f32 fraction2;
    f32 distance_squared;
} NphPhysicsSegmentDistance;

// The closest points of segments [p1, q1] and [p2, q2] (Ericson §5.1.9).
// Degenerate segments (points) are handled.
NphPhysicsSegmentDistance nph_physics_segment_distance(NphVec3 p1, NphVec3 q1, NphVec3 p2,
                                                       NphVec3 q2);

// A GJK simplex remembered between calls on the same pair: the vertex
// indices of its points in each proxy. Zero-initialised: no history.
typedef struct NphPhysicsSimplexCache {
    u32 count; // 0 to 4
    u8 index_a[4];
    u8 index_b[4];
} NphPhysicsSimplexCache;

typedef struct NphPhysicsDistance {
    NphVec3 point_a; // the closest point of A (on its rounded surface with radii)
    NphVec3 point_b;
    NphVec3 normal;    // unit, from A towards B; +Y when the shapes touch or overlap
    f32 distance;      // >= 0, and 0 when they touch or overlap
    u32 iterations;    // of the GJK loop
    u32 simplex_count; // 4 (or fewer, when the origin lies on it): the cores overlap
} NphPhysicsDistance;

// The distance between two convex proxies (GJK). With `use_radii` the
// proxies' radii round their cores and the result is between the rounded
// surfaces; without, between the cores. `cache` may be NULL; with one, the
// search starts from the previous simplex and the final one is stored.
void nph_physics_distance(const NphPhysicsProxy *a, NphPhysicsPose pose_a, const NphPhysicsProxy *b,
                          NphPhysicsPose pose_b, bool use_radii, NphPhysicsSimplexCache *cache,
                          NphPhysicsDistance *out);

typedef struct NphPhysicsPenetration {
    NphVec3 normal;  // unit, from A towards B: translating B by depth * normal separates them
    f32 depth;       // > 0
    NphVec3 point_a; // the deepest point of A in B, on A's (rounded) surface
    NphVec3 point_b; // the deepest point of B in A
} NphPhysicsPenetration;

// The penetration of two rounded convex proxies: false when they do not
// overlap. GJK decides; when only the rounding overlaps, its closest points
// give the answer, and when the cores overlap EPA expands the polytope to the
// face of the Minkowski difference nearest the origin.
bool nph_physics_penetration(const NphPhysicsProxy *a, NphPhysicsPose pose_a,
                             const NphPhysicsProxy *b, NphPhysicsPose pose_b,
                             NphPhysicsPenetration *out);

#ifdef __cplusplus
}
#endif
