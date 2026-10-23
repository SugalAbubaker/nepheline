// SPDX-License-Identifier: Apache-2.0
#pragma once

// Collision geometry (CLAUDE.md §8.9 "Shapes"; docs/physics.md "Geometry"):
// what a shape is, in its own frame, independent of any body or world.
//
// Every geometry is canonical in its frame and placed by a pose:
//
//   sphere   centred on the origin
//   capsule  the segment from (0, -half_height, 0) to (0, +half_height, 0),
//            swept by a ball of `radius` (Y is the long axis, as for a
//            standing character in a Y-up world)
//   box      centred on the origin, axis-aligned, with half extents
//   hull     a shared convex polyhedron (hull.h), in the frame of the
//            points it was built from
//
// Sizes are positive and finite (nph_physics_geometry_is_valid).
//
// Thread-safety: every function is pure and NPH_THREADSAFE; a hull is read,
// never written.

#include <nepheline/physics/hull.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphPhysicsShapeType {
    NPH_PHYSICS_SHAPE_SPHERE = 0,
    NPH_PHYSICS_SHAPE_CAPSULE = 1,
    NPH_PHYSICS_SHAPE_BOX = 2,
    NPH_PHYSICS_SHAPE_HULL = 3,
    NPH_PHYSICS_SHAPE_TYPE_COUNT = 4,
} NphPhysicsShapeType;

typedef struct NphPhysicsSphere {
    f32 radius;
} NphPhysicsSphere;

typedef struct NphPhysicsCapsule {
    f32 half_height; // half the length of the core segment, along Y
    f32 radius;
} NphPhysicsCapsule;

typedef struct NphPhysicsBox {
    NphVec3 half_extents;
} NphPhysicsBox;

// A shape's geometry: the type and its parameters.
typedef struct NphPhysicsGeometry {
    NphPhysicsShapeType type;
    union {
        NphPhysicsSphere sphere;
        NphPhysicsCapsule capsule;
        NphPhysicsBox box;
        const NphPhysicsHull *hull;
    };
} NphPhysicsGeometry;

NPH_INLINE NphPhysicsGeometry nph_physics_geometry_sphere(f32 radius)
{
    return (NphPhysicsGeometry){.type = NPH_PHYSICS_SHAPE_SPHERE, .sphere = {radius}};
}
NPH_INLINE NphPhysicsGeometry nph_physics_geometry_capsule(f32 half_height, f32 radius)
{
    return (NphPhysicsGeometry){.type = NPH_PHYSICS_SHAPE_CAPSULE,
                                .capsule = {half_height, radius}};
}
NPH_INLINE NphPhysicsGeometry nph_physics_geometry_box(NphVec3 half_extents)
{
    return (NphPhysicsGeometry){.type = NPH_PHYSICS_SHAPE_BOX, .box = {half_extents}};
}
NPH_INLINE NphPhysicsGeometry nph_physics_geometry_hull(const NphPhysicsHull *hull)
{
    return (NphPhysicsGeometry){.type = NPH_PHYSICS_SHAPE_HULL, .hull = hull};
}

// True for a known type with positive, finite sizes (and a hull that is not
// NULL). Shapes are created only from valid geometry.
bool nph_physics_geometry_is_valid(const NphPhysicsGeometry *g);

// The tight box around the geometry placed at `pose`.
NphAABB nph_physics_geometry_aabb(const NphPhysicsGeometry *g, NphPhysicsPose pose);

// The mass properties at uniform `density` (kg/m^3), in the geometry's own
// frame. Sphere, capsule, and box are exact closed forms; a hull's are
// precomputed by its builder (Mirtich, "Fast and Accurate Computation of
// Polyhedral Mass Properties", JGT 1996, in Eberly's formulation).
NphPhysicsMassData nph_physics_geometry_mass(const NphPhysicsGeometry *g, f32 density);

// The point of the geometry farthest along `direction` (which need not be
// unit), in its own frame, rounding included.
NphVec3 nph_physics_geometry_support(const NphPhysicsGeometry *g, NphVec3 direction);

// The radius of the largest ball centred at the geometry's centre of mass
// that fits inside it, and the distance from that centre to the farthest
// point. Continuous collision uses them to decide when a body moves fast
// enough to tunnel (docs/physics.md "Continuous collision").
f32 nph_physics_geometry_inner_radius(const NphPhysicsGeometry *g);
f32 nph_physics_geometry_outer_radius(const NphPhysicsGeometry *g);

// ---------------------------------------------------------------------------
// Support proxies for GJK
// ---------------------------------------------------------------------------

// A convex shape as the hull of a few points rounded by a radius (Box2D's
// shape proxy): a sphere is one point and its radius, a capsule two, a box
// its eight corners, a hull its vertices. GJK works on the core (the
// points) and adds the radii at the end, which keeps it exact for spheres
// and capsules and well-conditioned when the cores are far apart compared
// to their size.
typedef struct NphPhysicsProxy {
    const NphVec3 *points; // in the proxy's frame; not owned
    u32 count;             // at least 1
    f32 radius;
} NphPhysicsProxy;

// Room for the corners of a box, so sphere, capsule, and box proxies need no
// allocation.
#define NPH_PHYSICS_PROXY_STORAGE 8U

// The proxy of convex geometry. Sphere, capsule, and box points are written
// into `storage`, which must outlive the proxy; a hull's proxy points at its
// vertices.
NphPhysicsProxy nph_physics_geometry_proxy(const NphPhysicsGeometry *g,
                                           NphVec3 storage[NPH_PHYSICS_PROXY_STORAGE]);

// The index of the proxy point farthest along `direction` (the first of
// equals).
u32 nph_physics_proxy_support(const NphPhysicsProxy *p, NphVec3 direction);

#ifdef __cplusplus
}
#endif
