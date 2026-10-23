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

typedef struct NphTriangleViewStorage {
    NphVec3 vertices[3];
    NphPlane planes[2];
} NphTriangleViewStorage;

NphHullView nph_physics_hull_view(const NphPhysicsHull *hull);
NphHullView nph_physics_box_view(NphVec3 half_extents, NphBoxViewStorage *storage);
// A triangle as a flat polyhedron for the separating axis test (ADR-0046
// §7): its front (0, unit `normal`, the right-handed normal of v[0], v[1],
// v[2]) and back (1), and three edges, each the twin pair (v[i] v[i+1],
// v[i+1] v[i]) with the first on the front. An edge's two faces are
// opposite, so its Gauss-map arc is a half circle, which the axis test
// handles.
NphHullView nph_physics_triangle_view(const NphVec3 v[3], NphVec3 normal,
                                      NphTriangleViewStorage *storage);

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
// A contact with one of a mesh's triangles, before the mesh merges it with
// the others: a manifold with room for every point clipping makes (a box's
// face clipped to a triangle has up to seven), so the merged manifold's
// reduction, not each triangle's, chooses the four that stay.
#define NPH_TRIANGLE_CONTACT_POINTS 16U

typedef struct NphTriangleContact {
    NphVec3 normal; // from the shape towards the triangle
    u32 point_count;
    NphPhysicsManifoldPoint points[NPH_TRIANGLE_CONTACT_POINTS];
} NphTriangleContact;

NPH_INLINE void nph_triangle_contact_clear(NphTriangleContact *c)
{
    c->normal = nph_vec3_unit_y();
    c->point_count = 0U;
}

// Appends a point (ignored when the contact is full).
NPH_INLINE void nph_triangle_contact_push(NphTriangleContact *c, NphVec3 point, f32 separation,
                                          u32 id)
{
    if (c->point_count < NPH_TRIANGLE_CONTACT_POINTS) {
        NphPhysicsManifoldPoint *p = &c->points[c->point_count++];
        p->point = point;
        p->separation = separation;
        p->id = id;
    }
}

// A hull (or box) against a triangle's view at the origin, as
// nph_physics_collide_hulls but turning ties between faces to the
// triangle's (a shape lying flat on it takes the triangle's own normal),
// and keeping every point.
void nph_physics_collide_hull_triangle(const NphHullView *a, NphPhysicsPose pa,
                                       const NphHullView *triangle, f32 speculative,
                                       NphTriangleContact *out);
// The face contact of `a` against b's face `face`: a's face most opposed to
// it clipped against its sides, the separations along its normal, the
// contact's normal (from a to b) its reverse, every point kept. For a
// contact whose normal a rule set to a triangle's own (collide_mesh.c).
void nph_physics_collide_hull_face(const NphHullView *a, NphPhysicsPose pa, const NphHullView *b,
                                   NphPhysicsPose pb, u32 face, f32 speculative,
                                   NphTriangleContact *out);
// The axis test's separation of the pair: the largest over the faces of
// both and the edge pairs that form faces of the Minkowski difference. For
// the test against brute force.
f32 nph_physics_hulls_separation(const NphHullView *a, NphPhysicsPose pa, const NphHullView *b,
                                 NphPhysicsPose pb);

// collide_mesh.c's core, for the world: convex `a` with `rotation` at
// `position` in the mesh's frame, in 64 bits so the triangles keep full
// precision however far `a` is from the mesh's origin. The manifolds come
// out in a frame with the mesh's axes and its origin at a's position.
void nph_physics_collide_mesh_local(const NphPhysicsGeometry *a, NphQuat rotation,
                                    NphDVec3 position, const NphPhysicsMesh *mesh, f32 speculative,
                                    NphPhysicsMeshManifolds *out);

// A surface's triangle as the narrow phase takes it: its index, its
// vertices in the frame the query chose (counter-clockwise from the front),
// and its active edges (NPH_PHYSICS_MESH_EDGE_*). Returning false ends the
// query.
typedef bool (*NphPhysicsSurfaceVisit)(void *user, u32 triangle, const NphVec3 v[3],
                                       u32 active_edges);

// The heightfield's triangles whose boxes overlap `box` (in its frame), in
// nph_physics_heightfield_overlap's order, each with its vertices less
// `origin`, moved there in 64 bits (heightfield.c).
void nph_physics_heightfield_visit_box(const NphPhysicsHeightfield *h, NphAABB box, NphDVec3 origin,
                                       NphPhysicsSurfaceVisit visit, void *user);

// collide_mesh.c's core for a heightfield, as nph_physics_collide_mesh_local
// is a mesh's.
void nph_physics_collide_heightfield_local(const NphPhysicsGeometry *a, NphQuat rotation,
                                           NphDVec3 position, const NphPhysicsHeightfield *h,
                                           f32 speculative, NphPhysicsMeshManifolds *out);

// The inverse rotation by unit `q` of a 64-bit vector, in 64 bits.
NphDVec3 nph_physics_drotate_inverse(NphQuat q, NphDVec3 v);

// A manifold point this near the surface, or deeper, touches: it carries
// load. Points farther out are speculative and matter only if the shape
// turns onto them, so reductions keep touching points first.
#define NPH_PHYSICS_TOUCH_SEPARATION (0.1F * NPH_PHYSICS_LINEAR_SLOP)

// Combines two feature numbers into a point id.
NPH_INLINE u32 nph_physics_mix_id(u32 a, u32 b)
{
    u32 h = a * 0x9E3779B1U;
    h ^= b + 0x7F4A7C15U + (h << 6U) + (h >> 2U);
    return h;
}

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

// The watertight ray (Woop, Benthin, and Wald 2013), shared by the mesh's and
// the heightfield's raycasts: the axis the ray runs
// along most (kz), the other two (kx, ky; swapped when the ray runs down kz
// so the winding is kept), and the shear that makes it run along kz.
typedef struct NphWatertightRay {
    NphVec3 origin;
    u32 kx;
    u32 ky;
    u32 kz;
    f32 sx;
    f32 sy;
    f32 sz;
} NphWatertightRay;

NPH_INLINE NphWatertightRay nph_physics_watertight_ray(NphVec3 origin, NphVec3 delta)
{
    NphWatertightRay r;
    r.origin = origin;
    const NphVec3 a = nph_vec3_abs(delta);
    r.kz = a.x >= a.y ? (a.x >= a.z ? 0U : 2U) : (a.y >= a.z ? 1U : 2U);
    r.kx = (r.kz + 1U) % 3U;
    r.ky = (r.kx + 1U) % 3U;
    const f32 dz = nph_vec3_at(delta, r.kz);
    if (dz < 0.0F) {
        const u32 t = r.kx;
        r.kx = r.ky;
        r.ky = t;
    }
    r.sx = nph_vec3_at(delta, r.kx) / dz;
    r.sy = nph_vec3_at(delta, r.ky) / dz;
    r.sz = 1.0F / dz;
    return r;
}

// The fraction along the ray where it crosses triangle (a, b, c), or a
// negative value. `*back` is set when it crosses from behind.
NPH_INLINE f32 nph_physics_watertight_hit(const NphWatertightRay *r, NphVec3 pa, NphVec3 pb,
                                          NphVec3 pc, bool *back)
{
    const NphVec3 a = nph_vec3_sub(pa, r->origin);
    const NphVec3 b = nph_vec3_sub(pb, r->origin);
    const NphVec3 c = nph_vec3_sub(pc, r->origin);
    const f32 az = nph_vec3_at(a, r->kz);
    const f32 bz = nph_vec3_at(b, r->kz);
    const f32 cz = nph_vec3_at(c, r->kz);
    const f32 ax = nph_vec3_at(a, r->kx) - r->sx * az;
    const f32 ay = nph_vec3_at(a, r->ky) - r->sy * az;
    const f32 bx = nph_vec3_at(b, r->kx) - r->sx * bz;
    const f32 by = nph_vec3_at(b, r->ky) - r->sy * bz;
    const f32 cx = nph_vec3_at(c, r->kx) - r->sx * cz;
    const f32 cy = nph_vec3_at(c, r->ky) - r->sy * cz;
    f32 u = cx * by - cy * bx;
    f32 v = ax * cy - ay * cx;
    f32 w = bx * ay - by * ax;
    // An edge through the ray: decide it in 64 bits, so two triangles that
    // share the edge never both miss.
    if (u == 0.0F || v == 0.0F || w == 0.0F) {
        u = (f32)((f64)cx * (f64)by - (f64)cy * (f64)bx);
        v = (f32)((f64)ax * (f64)cy - (f64)ay * (f64)cx);
        w = (f32)((f64)bx * (f64)ay - (f64)by * (f64)ax);
    }
    if ((u < 0.0F || v < 0.0F || w < 0.0F) && (u > 0.0F || v > 0.0F || w > 0.0F)) {
        return -1.0F;
    }
    const f32 det = u + v + w;
    if (det == 0.0F) {
        return -1.0F;
    }
    const f32 t = (u * r->sz * az + v * r->sz * bz + w * r->sz * cz) / det;
    if (!(t >= 0.0F && t <= 1.0F)) {
        return -1.0F;
    }
    // The winding as the ray sees it: counter-clockwise (det > 0) is the
    // front, the ray running against the normal.
    *back = det < 0.0F;
    return t;
}
