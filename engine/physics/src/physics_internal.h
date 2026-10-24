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

// Point `p` of a surface's frame less `origin`, in 64 bits, then rounded:
// small and exact however far it is from the surface's origin.
NPH_INLINE NphVec3 nph_physics_relative_vertex(NphVec3 p, NphDVec3 origin)
{
    return nph_vec3((f32)((f64)p.x - origin.x), (f32)((f64)p.y - origin.y),
                    (f32)((f64)p.z - origin.z));
}

// The mesh's triangles whose boxes overlap `box` (in its frame), in
// nph_physics_mesh_overlap's order, each with its vertices less `origin`
// (collide_mesh.c).
void nph_physics_mesh_visit_box(const NphPhysicsMesh *mesh, NphAABB box, NphDVec3 origin,
                                NphPhysicsSurfaceVisit visit, void *user);

// Shape `a` with pose `pose_a` placed in a surface's frame (`pose_surface`,
// both in one frame): its rotation there, and its position in 64 bits, as
// the *_local functions take them.
void nph_physics_surface_place(NphPhysicsPose pose_a, NphPhysicsPose pose_surface,
                               NphQuat *rotation, NphDVec3 *position);

// ---------------------------------------------------------------------------
// Casts (cast.c, collide_mesh.c; ADR-0047)
// ---------------------------------------------------------------------------

// Shapes this close (metres) touch: a shape cast has arrived. The last step
// along the contact's normal (nph_physics_cast_finish) closes the rest
// across a face.
#define NPH_PHYSICS_CAST_TOUCH 1e-4F
// A motion into a contact by at most this fraction of its length slides
// along it: rounding leaves a motion projected onto a contact's plane this
// far off it.
#define NPH_PHYSICS_CAST_SLIDE 1e-5F
// How far out a cast's narrow phase looks where the shapes touch, and the
// margin its swept boxes are grown by.
#define NPH_PHYSICS_CAST_REACH NPH_PHYSICS_LINEAR_SLOP

// How far apart proxies `m` and `t` are along unit `n` (from t towards m):
// m's least extent along it less t's greatest, radii included. A lower
// bound on their distance (the gap of a separating slab), and the distance
// itself across a face they touch by.
f32 nph_physics_proxy_gap(const NphPhysicsProxy *m, NphPhysicsPose pose_m, const NphPhysicsProxy *t,
                          NphPhysicsPose pose_t, NphVec3 n);

// Of `hint` (GJK's normal between proxies `m` and `t` at their poses, from
// t towards m) and the exact normal of the closest features GJK's simplex
// `features` ended on (a face against a point, or two edges), the one along
// which the two are farther apart: the axis test's measure. GJK's own is a
// thousandth of a radian off across a face; along it a large target's far
// corner stands centimetres proud.
NphVec3 nph_physics_contact_axis(const NphPhysicsProxy *m, NphPhysicsPose pose_m,
                                 const NphPhysicsProxy *t, NphPhysicsPose pose_t,
                                 const NphPhysicsSimplexCache *features, NphVec3 hint);

// Where a shape cast arrived (cast.c).
typedef struct NphPhysicsArrival {
    f32 fraction;         // of the motion
    f32 approach;         // the last step's speed of approach (0 at the start)
    NphVec3 normal;       // GJK's: unit, from the target towards the mover
    NphVec3 point;        // on the target, the cast's frame (when `have_point`)
    NphPhysicsPose mover; // the two there, in a frame at the mover
    NphPhysicsPose target;
    NphVec3 base;                    // the target's place relative to the mover's start
    NphPhysicsSimplexCache features; // GJK's last simplex that did not hold the origin
    bool known;                      // `normal` well defined: the cores apart there
    bool have_point;                 // `point` and `normal` found (GJK's cores apart at some step)
} NphPhysicsArrival;

// Conservative advancement of proxy `m` at `pose_m`, moved by `delta`,
// towards proxy `t` at `pose_t`, on GJK distances (Mirtich 1996): each step
// as far as the plane between the closest points allows, so never past the
// contact, until the two touch (or GJK's 32 steps run out on a grazing
// approach: where the last began). GJK runs in a frame at the mover's
// place, where its digits are the shapes' own. False when the motion never
// brings them together, or not before `limit`.
bool nph_physics_cast_arrive(const NphPhysicsProxy *m, NphPhysicsPose pose_m, NphVec3 delta,
                             const NphPhysicsProxy *t, NphPhysicsPose pose_t, f32 limit,
                             NphPhysicsArrival *out);

// GJK's normal a millimetre back along the motion from where the cast
// arrived, for polytopes whose cores touch there. False where there is no
// way back (they touched at the start) or it is not defined either.
bool nph_physics_cast_normal_back(const NphPhysicsProxy *m, NphPhysicsPose pose_m, NphVec3 delta,
                                  const NphPhysicsProxy *t, NphPhysicsPose pose_t,
                                  const NphPhysicsArrival *a, NphVec3 *normal);

// The hit's fraction from the arrival with the contact's `normal`: a pair
// whose motion does not go into the contact (by more than
// NPH_PHYSICS_CAST_SLIDE of its length) is not hit, so one touching at the
// start and sliding along is not; otherwise the gap left along the normal
// is crossed (exactly to a face, never past the contact). Negative for no
// hit.
f32 nph_physics_cast_finish(const NphPhysicsProxy *m, const NphPhysicsProxy *t,
                            const NphPhysicsArrival *a, NphVec3 delta, NphVec3 normal);

// nph_physics_shape_cast between convex geometries, for hits up to `limit`
// (a world query's nearest so far).
bool nph_physics_cast_convex(const NphPhysicsGeometry *a, NphPhysicsPose pose_a, NphVec3 delta,
                             const NphPhysicsGeometry *b, NphPhysicsPose pose_b, f32 limit,
                             NphPhysicsCastHit *out);

// nph_physics_raycast in the geometry's own frame.
bool nph_physics_raycast_local(const NphPhysicsGeometry *g, NphVec3 origin, NphVec3 delta,
                               NphPhysicsCastHit *out);

// A surface's triangle for a sweep, as NphPhysicsSurfaceVisit; returns the
// fraction of the sweep still wanted (the closest hit so far: what lies
// past it is skipped), or a negative value to end it.
typedef f32 (*NphPhysicsSweepVisit)(void *user, u32 triangle, const NphVec3 v[3], u32 active_edges);

// The triangles a box of half extents `extent` centred at `center` may
// touch while moving by `delta` (the surface's frame): those whose boxes
// the swept box meets, with their vertices less `origin` (64 bits). The
// mesh's walk takes nearer nodes first (mesh.c); the heightfield's takes
// its blocks and cells in the motion's direction (heightfield.c).
void nph_physics_mesh_visit_sweep(const NphPhysicsMesh *m, NphVec3 center, NphVec3 extent,
                                  NphVec3 delta, NphDVec3 origin, NphPhysicsSweepVisit visit,
                                  void *user);
void nph_physics_heightfield_visit_sweep(const NphPhysicsHeightfield *h, NphVec3 center,
                                         NphVec3 extent, NphVec3 delta, NphDVec3 origin,
                                         NphPhysicsSweepVisit visit, void *user);

// Shape casts and overlaps against a surface (collide_mesh.c), with `a`
// placed as for nph_physics_collide_mesh_local and `delta` in the
// surface's axes; casts find hits up to `limit`. A hit comes out in a
// frame with the surface's axes and its origin at a's start; its triangle
// is the mesh's input index.
bool nph_physics_cast_mesh_local(const NphPhysicsGeometry *a, NphQuat rotation, NphDVec3 position,
                                 NphVec3 delta, const NphPhysicsMesh *mesh, f32 limit,
                                 NphPhysicsCastHit *out);
bool nph_physics_cast_heightfield_local(const NphPhysicsGeometry *a, NphQuat rotation,
                                        NphDVec3 position, NphVec3 delta,
                                        const NphPhysicsHeightfield *h, f32 limit,
                                        NphPhysicsCastHit *out);
bool nph_physics_overlap_mesh_local(const NphPhysicsGeometry *a, NphQuat rotation,
                                    NphDVec3 position, const NphPhysicsMesh *mesh);
bool nph_physics_overlap_heightfield_local(const NphPhysicsGeometry *a, NphQuat rotation,
                                           NphDVec3 position, const NphPhysicsHeightfield *h);

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
