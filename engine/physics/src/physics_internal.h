// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal to engine/physics: what the collision sources share.

#include <nepheline/physics/collide.h>

// A convex polyhedron as the separating axis test sees it: a hull, or a box
// whose eight corners and six planes live in the caller's storage with a
// fixed topology (collide_hull.c).
typedef struct NphHullView {
    const NphVec3 *vertices;
    const NphPlane *planes;
    const u16 *faces;
    const NphPhysicsHullEdge *edges;
    u32 vertex_count;
    u32 face_count;
    u32 edge_count;
    NphVec3 centroid;
} NphHullView;

typedef struct NphBoxViewStorage {
    NphVec3 vertices[8];
    NphPlane planes[6];
} NphBoxViewStorage;

NphHullView nph_physics_hull_view(const NphPhysicsHull *hull);
NphHullView nph_physics_box_view(NphVec3 half_extents, NphBoxViewStorage *storage);

// The box view's topology, for the test that it matches
// nph_physics_hull_make_box.
const u16 *nph_physics_box_view_faces(void);
const NphPhysicsHullEdge *nph_physics_box_view_edges(void);

// The pair functions of collide_hull.c. Poses are in the common frame; the
// manifold's normal points from the first shape to the second.
void nph_physics_collide_hull_sphere(const NphHullView *a, NphPhysicsPose pa, f32 radius,
                                     NphPhysicsPose pb, f32 speculative, NphPhysicsManifold *out);
void nph_physics_collide_hull_capsule(const NphHullView *a, NphPhysicsPose pa, f32 half_height,
                                      f32 radius, NphPhysicsPose pb, f32 speculative,
                                      NphPhysicsManifold *out);
void nph_physics_collide_boxes(NphVec3 half_a, NphPhysicsPose pa, NphVec3 half_b, NphPhysicsPose pb,
                               f32 speculative, NphPhysicsCollideCache *cache,
                               NphPhysicsManifold *out);
void nph_physics_collide_hulls(const NphHullView *a, NphPhysicsPose pa, const NphHullView *b,
                               NphPhysicsPose pb, f32 speculative, NphPhysicsCollideCache *cache,
                               NphPhysicsManifold *out);

// Manifold helpers shared by the pair functions.
NPH_INLINE void nph_physics_manifold_clear(NphPhysicsManifold *m)
{
    m->normal = nph_vec3_unit_y();
    m->point_count = 0U;
}

// Appends a point (ignored when the manifold is full).
NPH_INLINE void nph_physics_manifold_push(NphPhysicsManifold *m, NphVec3 point, f32 separation,
                                          u32 id)
{
    if (m->point_count < NPH_PHYSICS_MANIFOLD_MAX_POINTS) {
        NphPhysicsManifoldPoint *p = &m->points[m->point_count++];
        p->point = point;
        p->separation = separation;
        p->id = id;
    }
}
