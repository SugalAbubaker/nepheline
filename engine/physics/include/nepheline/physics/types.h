// SPDX-License-Identifier: Apache-2.0
#pragma once

// Physics basics shared by the collision layer and the world (CLAUDE.md §8.9;
// docs/physics.md; docs/adr/0042-physics.md): units, tolerances, rigid poses,
// and mass properties.
//
// Units are SI: metres, kilograms, seconds, radians. The engine is Y up and
// right-handed (ADR-0008), so gravity points along -Y by default.
//
// Precision (CLAUDE.md §8.2 "Large-world coordinates"): a body's position in
// the world is 64-bit. Everything else is 32-bit and relative: shape poses in
// their body, collision in a frame centred on one of the two bodies, the
// solver's anchors relative to the centres of mass. Contacts only exist
// between nearby shapes, so the relative values are small and keep full f32
// precision at any distance from the origin.
//
// Thread-safety: every function here is pure and NPH_THREADSAFE.

#include <nepheline/core/log.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/mat.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_PHYSICS);

// ---------------------------------------------------------------------------
// Tolerances
// ---------------------------------------------------------------------------

// The penetration contacts are allowed to keep, in metres. Pushing to exactly
// zero makes resting contacts flicker between touching and separated; a few
// millimetres of overlap are invisible (Catto, "Solver2D", 2024; Box2D's
// b2_linearSlop).
#define NPH_PHYSICS_LINEAR_SLOP 0.005F

// The largest coordinate a shape may have in its own frame, in metres: a
// sphere's radius, a box's half extent, a hull's or a mesh's vertex. Past
// it the products collision forms (edges' cross products, GJK's
// determinants) would leave f32's range, and a position resolves no finer
// than a centimetre anyway. Bodies' positions are 64-bit and unbounded
// (types.h's precision note): this bounds shapes, not worlds. Builders
// refuse more, and validation refuses blocks that hold more.
#define NPH_PHYSICS_MAX_COORDINATE 1.0e5F

// How far apart two shapes may be and still get contact points: speculative
// contacts, which the solver keeps from closing faster than the gap allows
// (Catto, "Speculative Contacts", 2013). Four slops, as in Box2D.
#define NPH_PHYSICS_SPECULATIVE_DISTANCE (4.0F * NPH_PHYSICS_LINEAR_SLOP)

// The margin a broadphase proxy's box is fattened by, in metres, so a body
// that moves a little stays inside its proxy and the tree is not touched.
#define NPH_PHYSICS_AABB_MARGIN 0.1F

// ---------------------------------------------------------------------------
// Rigid poses
// ---------------------------------------------------------------------------

// A rotation followed by a translation, 32-bit: p' = rotation * p + position.
// Shapes are placed in their body with one, and collision runs on poses
// relative to a nearby origin. `rotation` is a unit quaternion.
typedef struct NphPhysicsPose {
    NphVec3 position;
    NphQuat rotation;
} NphPhysicsPose;

NPH_STATIC_ASSERT(sizeof(NphPhysicsPose) == 28U, "a pose is seven floats");

NPH_INLINE NphPhysicsPose nph_physics_pose(NphVec3 position, NphQuat rotation)
{
    NphPhysicsPose p;
    p.position = position;
    p.rotation = rotation;
    return p;
}
NPH_INLINE NphPhysicsPose nph_physics_pose_identity(void)
{
    return nph_physics_pose(nph_vec3_zero(), nph_quat_identity());
}
NPH_INLINE NphVec3 nph_physics_pose_transform_point(NphPhysicsPose t, NphVec3 p)
{
    return nph_vec3_add(nph_quat_rotate(t.rotation, p), t.position);
}
NPH_INLINE NphVec3 nph_physics_pose_transform_vector(NphPhysicsPose t, NphVec3 v)
{
    return nph_quat_rotate(t.rotation, v);
}
NPH_INLINE NphVec3 nph_physics_pose_inverse_transform_point(NphPhysicsPose t, NphVec3 p)
{
    return nph_quat_rotate_inverse(t.rotation, nph_vec3_sub(p, t.position));
}
NPH_INLINE NphVec3 nph_physics_pose_inverse_transform_vector(NphPhysicsPose t, NphVec3 v)
{
    return nph_quat_rotate_inverse(t.rotation, v);
}
// a * b: the pose that applies b, then a (b expressed in a's parent frame).
NPH_INLINE NphPhysicsPose nph_physics_pose_mul(NphPhysicsPose a, NphPhysicsPose b)
{
    return nph_physics_pose(nph_physics_pose_transform_point(a, b.position),
                            nph_quat_mul(a.rotation, b.rotation));
}
// a^-1 * b: b expressed in a's frame (both given in the same parent frame).
NPH_INLINE NphPhysicsPose nph_physics_pose_inv_mul(NphPhysicsPose a, NphPhysicsPose b)
{
    return nph_physics_pose(nph_physics_pose_inverse_transform_point(a, b.position),
                            nph_quat_mul(nph_quat_conjugate(a.rotation), b.rotation));
}
NPH_INLINE NphPhysicsPose nph_physics_pose_inverse(NphPhysicsPose t)
{
    const NphQuat inv = nph_quat_conjugate(t.rotation);
    return nph_physics_pose(nph_vec3_neg(nph_quat_rotate(inv, t.position)), inv);
}

// ---------------------------------------------------------------------------
// Mass properties
// ---------------------------------------------------------------------------

// Mass, centre of mass, and the inertia tensor about the centre of mass, all
// expressed in one frame (a shape's, a body's). The tensor is symmetric and,
// for a solid, positive definite: kg m^2.
typedef struct NphPhysicsMassData {
    f32 mass;
    NphVec3 center;
    NphMat3 inertia;
} NphPhysicsMassData;

// Expresses `m` (given in a child frame) in the parent frame of `pose`: the
// centre moves, the tensor rotates (R I R^T). The mass is unchanged.
NphPhysicsMassData nph_physics_mass_transform(NphPhysicsMassData m, NphPhysicsPose pose);

// The mass properties of the union of two bodies of matter given in the same
// frame: masses add, the centre is the mass-weighted mean, and each tensor
// moves to the new centre by the parallel axis theorem before they add. Zero
// masses contribute nothing.
NphPhysicsMassData nph_physics_mass_add(NphPhysicsMassData a, NphPhysicsMassData b);

// The tensor of a point mass `mass` at offset `r` from the reference point
// (the parallel axis term): mass (|r|^2 E - r r^T).
NphMat3 nph_physics_inertia_offset(f32 mass, NphVec3 r);

#ifdef __cplusplus
}
#endif
