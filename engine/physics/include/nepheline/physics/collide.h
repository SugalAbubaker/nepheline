// SPDX-License-Identifier: Apache-2.0
#pragma once

// Narrow-phase collision (CLAUDE.md §8.9 "Narrowphase"; docs/physics.md
// "Collision"): contact manifolds between two placed geometries, and the
// queries they are built from.
//
//   nph_physics_collide           the manifold of two geometries: a normal
//                                 and up to four points, with separations,
//                                 within a speculative distance
//   nph_physics_collide_triangle  a convex geometry's manifold with a
//                                 triangle
//   nph_physics_collide_mesh      a convex geometry's manifolds with a
//                                 triangle mesh: one per normal, up to four
//   nph_physics_collide_heightfield  the same with a heightfield
//   nph_physics_distance          GJK: the closest points of two convex
//                                 proxies
//   nph_physics_penetration       GJK, then EPA when the cores overlap: the
//                                 smallest translation that separates them
//   nph_physics_raycast           where a segment first meets a geometry
//   nph_physics_shape_cast        where a convex geometry moved without
//                                 turning first meets another
//   nph_physics_overlap           whether a convex geometry touches another
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
//     Game Objects", GDC 2001);
//   - triangles: spheres and capsules by closest points, boxes and hulls by
//     the axis test against the triangle as a flat polyhedron; one-sided,
//     with active edges, so bodies cross a mesh's seams without bumping
//     (ADR-0046; docs/physics.md "Meshes").
//
// Thread-safety: every function is NPH_THREADSAFE; a cache belongs to one
// shape pair and is written by the call that receives it.

#include <nepheline/physics/geometry.h>
#include <nepheline/physics/mesh.h>

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

// True when nph_physics_collide handles the pair: every pair of convex types
// (geometry.h). A convex shape meets a mesh through nph_physics_collide_mesh.
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
// how many remain: the deepest (of equals, the one farthest from their
// centre), then each the one that touches first as the shape turns about
// those kept: about the deepest, about the line through the first two (on
// either side), and on that line's other side (with none there, the point
// farthest outside the three's triangle). A candidate touches first when its
// catch angle, its height above the touching points (above the deepest, when
// none touch) over its lever arm about the turn's axis, is the least; of
// angles as small, the one with the longest arm. Touching points have no
// height, so a flat contact keeps Gregorius's (2015) choice: the most area.
// Distances are measured in the plane perpendicular to `normal`; points along
// a line keep its two ends. Counts of four or fewer are left unchanged.
u32 nph_physics_manifold_reduce(NphPhysicsManifoldPoint *points, u32 count, NphVec3 normal);

// ---------------------------------------------------------------------------
// Triangles and meshes (ADR-0046; docs/physics.md "Meshes")
// ---------------------------------------------------------------------------

// A triangle: counter-clockwise seen from its front, the side it collides
// from unless two-sided.
typedef struct NphPhysicsTriangle {
    NphVec3 vertices[3];
    // NPH_PHYSICS_MESH_EDGE_* (mesh.h): the edges that are edges of the
    // surface. A contact on the face, on an inactive edge (one the surface
    // continues across flat or concave), or on a vertex whose normal crosses
    // none of its active edges takes the face's normal, so a body sliding
    // over a seam is not pushed back by the next triangle. A lone
    // triangle's edges are all active.
    u32 active_edges;
    b32 two_sided;
} NphPhysicsTriangle;

// The manifold of convex `a` at `pose_a` and triangle `t` (its vertices in
// the same frame), the normal from `a` towards the triangle. A one-sided
// triangle gives nothing when a's centre is behind its plane, or when the
// contact would push `a` through it from behind; a two-sided one faces a's
// centre. Point ids name the features of `a` and the triangle only, so
// callers mix in a triangle's identity. A degenerate triangle gives
// nothing.
void nph_physics_collide_triangle(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                                  const NphPhysicsTriangle *t, f32 speculative_distance,
                                  NphPhysicsManifold *out);

#define NPH_PHYSICS_MESH_MAX_MANIFOLDS 4U

// A convex shape's manifolds against a mesh, one per surface direction.
typedef struct NphPhysicsMeshManifolds {
    u32 count; // 0 to NPH_PHYSICS_MESH_MAX_MANIFOLDS
    NphPhysicsManifold manifolds[NPH_PHYSICS_MESH_MAX_MANIFOLDS]; // the deepest first
} NphPhysicsMeshManifolds;

// Convex `a` at `pose_a` against `mesh` at `pose_mesh`: each triangle under
// a's box collided as by nph_physics_collide_triangle (with the mesh's
// active edges and sides), the manifolds whose normals agree within 5
// degrees merged (their points reduced to four, each manifold taking its
// deepest member's normal), and the four with the deepest points kept
// (ADR-0046 §8). Normals point from `a` towards the mesh; point ids mix the
// triangle's index (in leaf order) with its features. The triangles are
// moved next to `a` in 64 bits, so a mesh keeps full precision far from its
// origin; as for nph_physics_collide, the poses' frame should be near `a`.
void nph_physics_collide_mesh(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                              const NphPhysicsMesh *mesh, NphPhysicsPose pose_mesh,
                              f32 speculative_distance, NphPhysicsMeshManifolds *out);

// Convex `a` at `pose_a` against heightfield `h` at `pose_h`: as
// nph_physics_collide_mesh, with the cells under a's box found by
// arithmetic (heightfield.h). Point ids mix the triangle's index (2 x cell
// + 0 or 1) with its features.
void nph_physics_collide_heightfield(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                                     const NphPhysicsHeightfield *h, NphPhysicsPose pose_h,
                                     f32 speculative_distance, NphPhysicsMeshManifolds *out);

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

// ---------------------------------------------------------------------------
// Casts and overlaps (docs/physics.md "Queries"; ADR-0047)
// ---------------------------------------------------------------------------

// A hit's triangle on convex geometry, which has none.
#define NPH_PHYSICS_NO_TRIANGLE 0xFFFFFFFFU

// Where a segment or a moving shape first meets a geometry.
typedef struct NphPhysicsCastHit {
    NphVec3 point;  // on the geometry's surface
    NphVec3 normal; // unit: the surface's there, out of it (against the motion)
    f32 fraction;   // of the motion, in [0, 1]
    // The triangle met: a mesh's index in its input (mesh.h's originals), a
    // heightfield's 2 x cell + 0 or 1, or NPH_PHYSICS_NO_TRIANGLE.
    u32 triangle;
} NphPhysicsCastHit;

// Where the segment from `origin` to origin + `delta` first meets `g` at
// `pose` (all in one frame). Exact for spheres and capsules (closest
// approach, then Pythagoras: no cancellation for a far origin; Ericson
// §5.3.2 and §5.3.7), boxes (the slab test), and hulls (the segment clipped
// by their face planes: Haines, "Fast Ray-Convex Polyhedron Intersection",
// Graphics Gems II, 1991); meshes and heightfields by their own watertight
// raycasts, one-sided ones from the front only. Convex geometry is met
// from outside: a segment that starts inside it does not hit it (Box2D's
// rule; overlaps find such shapes), one that starts on its surface and
// goes in hits at fraction 0. False for no hit, or a zero `delta`.
bool nph_physics_raycast(const NphPhysicsGeometry *g, NphPhysicsPose pose, NphVec3 origin,
                         NphVec3 delta, NphPhysicsCastHit *out);

// Convex `a` at `pose_a`, moved by `translation` without turning, against
// `b` at `pose_b` (any type): where they first touch. Conservative
// advancement on GJK distances (Mirtich, "Impulse-based Dynamic Simulation
// of Rigid Body Systems", PhD thesis, 1996; as Catto's b2ShapeCast, Box2D
// v3.1): each step moves `a` as far as the plane between the closest points
// allows, so it never passes the contact, until the two are within 0.01 mm;
// a last step along the contact's normal then closes what is left of the
// gap across a face. GJK runs in a frame at a's place, so its digits are
// the shapes' own however long the cast. The hit's point is on b's
// surface; its normal is b's there (towards `a`): GJK's for round shapes;
// for polytopes, whose cores touch at contact, the exact normal of the
// closest features on GJK's last simplex (a face against a point, or two
// edges), or GJK's a millimetre back, whichever separates the two farther.
//
// A pair already touching or overlapping meets at fraction 0 when the
// motion goes into the contact (its normal then the narrow phase's,
// nph_physics_collide, which is defined at any depth), and not at all when
// the motion leaves it or slides along it (into it by at most 1e-5 of its
// length: a motion projected onto the contact): a body resting on a floor
// casts along it freely. A mesh's or a heightfield's triangles follow the
// contact rules (ADR-0046): one side, and the face's normal on an inactive
// edge, so a shape crossing a flat seam is not stopped by the next
// triangle's edge. A grazing approach that has not arrived after 32 steps
// stops where it is, short of any contact. False for no hit, or a zero
// translation.
bool nph_physics_shape_cast(const NphPhysicsGeometry *a, NphPhysicsPose pose_a, NphVec3 translation,
                            const NphPhysicsGeometry *b, NphPhysicsPose pose_b,
                            NphPhysicsCastHit *out);

// As nph_physics_shape_cast, with `a` rounded by `margin` (metres, >= 0:
// every point within the margin of `a` taken as part of it): the cast stops
// where `a` comes within the margin of `b` (to the 0.1 mm touch), measured
// along the true closest direction, whatever the shapes, and "touching" is
// being within the margin. A mover that keeps a skin off what it meets
// (character.h) casts with its skin. The hit's point is on `b`'s surface.
bool nph_physics_shape_cast_margin(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                                   NphVec3 translation, f32 margin, const NphPhysicsGeometry *b,
                                   NphPhysicsPose pose_b, NphPhysicsCastHit *out);

// True when convex `a` at `pose_a` touches or overlaps `b` at `pose_b`: GJK
// on convex geometry (radii included); a mesh or a heightfield overlaps
// what touches one of its triangles, from either side (not what lies
// wholly behind a one-sided surface).
bool nph_physics_overlap(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                         const NphPhysicsGeometry *b, NphPhysicsPose pose_b);

#ifdef __cplusplus
}
#endif
