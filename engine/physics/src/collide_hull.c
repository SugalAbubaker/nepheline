// SPDX-License-Identifier: Apache-2.0
// Boxes and hulls against spheres, capsules, and each other: the separating
// axis test and contact clipping (collide.h; docs/physics.md "Hull against
// hull").
//
// Everything runs in the first shape's frame. The separating axis test
// (Gregorius, GDC 2013) tries the face normals of both polyhedra and the
// cross products of the edge pairs whose Gauss-map arcs intersect (the faces
// of the Minkowski difference), which finds the exact separation, or the
// exact penetration depth, of two convex polyhedra. The contact is then
// either a face (the incident face of the other polyhedron clipped against
// the side planes of the reference face) or an edge pair (one point).
// When the polyhedra are apart, the axis test only bounds the gap from
// below (two corners approaching each other are farther apart than any
// face or edge axis says), so GJK measures it exactly: speculative contacts
// must not stop a body short of the real surface.

#include <nepheline/core/assert.h>
#include <nepheline/math/simd.h>

#include "physics_internal.h"

#include <float.h>

#define HULL_MAX_FACES (2U * NPH_PHYSICS_HULL_MAX_VERTICES)
#define CLIP_CAPACITY  (NPH_PHYSICS_HULL_MAX_VERTICES + 8U)

// Prefer the first shape's face over the second's, and faces over edges,
// unless the other axis separates (or penetrates less) by this much: a tie
// must not flip between steps.
#define FACE_BIAS (0.1F * NPH_PHYSICS_LINEAR_SLOP)
#define EDGE_BIAS (0.5F * NPH_PHYSICS_LINEAR_SLOP)

// A GJK normal this far from the axis test's means the closest features are
// corners or an edge and a corner, not a face or an edge pair.
#define FEATURE_AGREEMENT 0.995F

enum {
    CACHE_NONE = 0,
    CACHE_FACE_A = 1,
    CACHE_FACE_B = 2,
    CACHE_EDGES = 3,
};

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

// The box's topology, identical to nph_physics_hull_make_box's (tested):
// corners by bit (x, y, z), faces -x +x -y +y -z +z.
static const u16 k_box_faces[6] = {0U, 8U, 16U, 5U, 7U, 19U};
static const NphPhysicsHullEdge k_box_edges[24] = {
    {2U, 1U, 0U, 0U},   {16U, 0U, 4U, 2U},  {4U, 3U, 4U, 0U},   {19U, 2U, 6U, 5U},
    {6U, 5U, 6U, 0U},   {20U, 4U, 2U, 3U},  {0U, 7U, 2U, 0U},   {23U, 6U, 0U, 4U},
    {10U, 9U, 1U, 1U},  {17U, 8U, 3U, 4U},  {12U, 11U, 3U, 1U}, {22U, 10U, 7U, 3U},
    {14U, 13U, 7U, 1U}, {21U, 12U, 5U, 5U}, {8U, 15U, 5U, 1U},  {18U, 14U, 1U, 2U},
    {15U, 17U, 0U, 2U}, {7U, 16U, 1U, 4U},  {1U, 19U, 5U, 2U},  {13U, 18U, 4U, 5U},
    {11U, 21U, 6U, 3U}, {3U, 20U, 7U, 5U},  {5U, 23U, 3U, 3U},  {9U, 22U, 2U, 4U},
};

const u16 *nph_physics_box_view_faces(void)
{
    return k_box_faces;
}

const NphPhysicsHullEdge *nph_physics_box_view_edges(void)
{
    return k_box_edges;
}

NphHullView nph_physics_box_view(NphVec3 h, NphBoxViewStorage *storage)
{
    for (u32 i = 0U; i < 8U; ++i) {
        storage->vertices[i] = nph_vec3((i & 1U) != 0U ? h.x : -h.x, (i & 2U) != 0U ? h.y : -h.y,
                                        (i & 4U) != 0U ? h.z : -h.z);
    }
    storage->planes[0] = nph_plane(nph_vec3(-1.0F, 0.0F, 0.0F), -h.x);
    storage->planes[1] = nph_plane(nph_vec3(1.0F, 0.0F, 0.0F), -h.x);
    storage->planes[2] = nph_plane(nph_vec3(0.0F, -1.0F, 0.0F), -h.y);
    storage->planes[3] = nph_plane(nph_vec3(0.0F, 1.0F, 0.0F), -h.y);
    storage->planes[4] = nph_plane(nph_vec3(0.0F, 0.0F, -1.0F), -h.z);
    storage->planes[5] = nph_plane(nph_vec3(0.0F, 0.0F, 1.0F), -h.z);
    NphHullView v;
    v.vertices = storage->vertices;
    v.planes = storage->planes;
    v.faces = k_box_faces;
    v.edges = k_box_edges;
    v.vertex_count = 8U;
    v.face_count = 6U;
    v.edge_count = 24U;
    v.centroid = nph_vec3_zero();
    return v;
}

// The triangle's topology: faces front (0) and back (1); half-edges in
// twin pairs v0 v1 | v1 v0, v1 v2 | v2 v1, v2 v0 | v0 v2, the first of each on
// the front, each face's loop counter-clockwise seen from outside it.
static const u16 k_triangle_faces[2] = {0U, 1U};
static const NphPhysicsHullEdge k_triangle_edges[6] = {
    {2U, 1U, 0U, 0U}, {5U, 0U, 1U, 1U}, {4U, 3U, 1U, 0U},
    {1U, 2U, 2U, 1U}, {0U, 5U, 2U, 0U}, {3U, 4U, 0U, 1U},
};

NphHullView nph_physics_triangle_view(const NphVec3 v[3], NphVec3 n,
                                      NphTriangleViewStorage *storage)
{
    for (u32 i = 0U; i < 3U; ++i) {
        storage->vertices[i] = v[i];
    }
    storage->planes[0] = nph_plane(n, -nph_vec3_dot(n, v[0]));
    storage->planes[1] = nph_plane(nph_vec3_neg(n), nph_vec3_dot(n, v[0]));
    NphHullView h;
    h.vertices = storage->vertices;
    h.planes = storage->planes;
    h.faces = k_triangle_faces;
    h.edges = k_triangle_edges;
    h.vertex_count = 3U;
    h.face_count = 2U;
    h.edge_count = 6U;
    h.centroid = nph_vec3_scale(nph_vec3_add(nph_vec3_add(v[0], v[1]), v[2]), 1.0F / 3.0F);
    return h;
}

NphHullView nph_physics_hull_view(const NphPhysicsHull *hull)
{
    NphHullView v;
    v.vertices = nph_physics_hull_vertices(hull);
    v.planes = nph_physics_hull_planes(hull);
    v.faces = nph_physics_hull_faces(hull);
    v.edges = nph_physics_hull_edges(hull);
    v.vertex_count = hull->vertex_count;
    v.face_count = hull->face_count;
    v.edge_count = hull->edge_count;
    v.centroid = hull->centroid;
    return v;
}

static NphVec3 edge_head(const NphHullView *h, const NphVec3 *vertices, u32 e)
{
    return vertices[h->edges[h->edges[e].next].origin];
}

static NphPhysicsProxy view_proxy(const NphHullView *h, const NphVec3 *vertices)
{
    NphPhysicsProxy p;
    p.points = vertices;
    p.count = h->vertex_count;
    p.radius = 0.0F;
    return p;
}

// ---------------------------------------------------------------------------
// Hull and sphere
// ---------------------------------------------------------------------------

void nph_physics_collide_hull_sphere(const NphHullView *a, NphPhysicsPose pa, f32 radius,
                                     NphPhysicsPose pb, f32 speculative, NphPhysicsManifold *out)
{
    const NphVec3 c = nph_physics_pose_inverse_transform_point(pa, pb.position);
    f32 best = -FLT_MAX;
    u32 face = 0U;
    for (u32 f = 0U; f < a->face_count; ++f) {
        const f32 s = nph_plane_distance(a->planes[f], c);
        if (s > best) {
            best = s;
            face = f;
        }
    }
    if (best > radius + speculative) {
        return;
    }
    NphVec3 n;
    NphVec3 surface_a;
    f32 sep;
    u32 id;
    if (best <= 0.0F) {
        // The centre is inside: out through the shallowest face.
        n = a->planes[face].normal;
        surface_a = nph_vec3_mul_add(c, n, -best);
        sep = best - radius;
        id = face;
    } else {
        // Outside: the closest point may be on an edge or a corner.
        const NphPhysicsProxy pa_proxy = view_proxy(a, a->vertices);
        const NphPhysicsProxy pb_proxy = {&c, 1U, 0.0F};
        NphPhysicsDistance d;
        nph_physics_distance(&pa_proxy, nph_physics_pose_identity(), &pb_proxy,
                             nph_physics_pose_identity(), false, NULL, &d);
        if (d.distance > radius + speculative) {
            return;
        }
        n = d.distance > 1e-6F ? d.normal : a->planes[face].normal;
        surface_a = d.point_a;
        sep = d.distance - radius;
        id = 0x10000U | face;
    }
    const NphVec3 surface_b = nph_vec3_mul_add(c, n, -radius);
    out->normal = nph_physics_pose_transform_vector(pa, n);
    nph_physics_manifold_push(out,
                              nph_physics_pose_transform_point(
                                  pa, nph_vec3_scale(nph_vec3_add(surface_a, surface_b), 0.5F)),
                              sep, id);
}

// ---------------------------------------------------------------------------
// Hull and capsule
// ---------------------------------------------------------------------------

// Clips the segment [p, q] to the prism over face `face` (its side planes),
// returning false when nothing is left.
static bool clip_segment_to_face(const NphHullView *h, u32 face, NphVec3 *p, NphVec3 *q)
{
    const NphVec3 n = h->planes[face].normal;
    f32 t0 = 0.0F;
    f32 t1 = 1.0F;
    const NphVec3 p0 = *p;
    const NphVec3 d = nph_vec3_sub(*q, *p);
    const u32 first = h->faces[face];
    u32 e = first;
    u32 guard = 0U;
    do {
        const NphVec3 a = h->vertices[h->edges[e].origin];
        const NphVec3 b = edge_head(h, h->vertices, e);
        const NphVec3 side = nph_vec3_cross(nph_vec3_sub(b, a), n); // outward
        const f32 s0 = nph_vec3_dot(side, nph_vec3_sub(p0, a));
        const f32 sd = nph_vec3_dot(side, d);
        // Inside where s0 + t sd <= 0.
        if (sd == 0.0F) {
            if (s0 > 0.0F) {
                return false;
            }
        } else {
            const f32 t = -s0 / sd;
            if (sd > 0.0F) {
                t1 = nph_f32_min(t1, t);
            } else {
                t0 = nph_f32_max(t0, t);
            }
            if (t0 > t1) {
                return false;
            }
        }
        e = h->edges[e].next;
        guard += 1U;
    } while (e != first && guard < h->edge_count);
    *p = nph_vec3_mul_add(p0, d, t0);
    *q = nph_vec3_mul_add(p0, d, t1);
    return true;
}

void nph_physics_collide_hull_capsule(const NphHullView *a, NphPhysicsPose pa, f32 half_height,
                                      f32 radius, NphPhysicsPose pb, f32 speculative,
                                      NphPhysicsManifold *out)
{
    const NphPhysicsPose rel = nph_physics_pose_inv_mul(pa, pb);
    const NphVec3 axis = nph_vec3_scale(nph_quat_axis_y(rel.rotation), half_height);
    const NphVec3 s0 = nph_vec3_sub(rel.position, axis);
    const NphVec3 s1 = nph_vec3_add(rel.position, axis);
    const f32 limit = radius + speculative;

    // Face axes: the segment's support against each face is its lower end.
    f32 face_sep = -FLT_MAX;
    u32 face = 0U;
    for (u32 f = 0U; f < a->face_count; ++f) {
        const NphPlane pl = a->planes[f];
        const f32 s = nph_f32_min(nph_plane_distance(pl, s0), nph_plane_distance(pl, s1));
        if (s > face_sep) {
            face_sep = s;
            face = f;
        }
    }
    if (face_sep > limit) {
        return;
    }
    // Edge axes: the segment's Gauss map is the great circle perpendicular
    // to it, which an edge's arc crosses when its two faces lie on opposite
    // sides of that circle's plane.
    const NphVec3 d = nph_vec3_sub(s1, s0);
    const f32 d_len_sq = nph_vec3_length_sq(d);
    f32 edge_sep = -FLT_MAX;
    u32 edge = 0U;
    NphVec3 edge_axis = nph_vec3_unit_y();
    if (d_len_sq > 1e-12F) {
        for (u32 e = 0U; e < a->edge_count; e += 2U) {
            const NphVec3 n1 = a->planes[a->edges[e].face].normal;
            const NphVec3 n2 = a->planes[a->edges[e + 1U].face].normal;
            if (nph_vec3_dot(n1, d) * nph_vec3_dot(n2, d) >= 0.0F) {
                continue;
            }
            const NphVec3 p = a->vertices[a->edges[e].origin];
            const NphVec3 u = nph_vec3_sub(edge_head(a, a->vertices, e), p);
            NphVec3 x = nph_vec3_cross(u, d);
            const f32 len = nph_vec3_length(x);
            if (len < 0.005F * nph_f32_sqrt(nph_vec3_length_sq(u) * d_len_sq)) {
                continue;
            }
            x = nph_vec3_scale(x, 1.0F / len);
            if (nph_vec3_dot(x, nph_vec3_sub(p, a->centroid)) < 0.0F) {
                x = nph_vec3_neg(x);
            }
            const f32 s = nph_vec3_dot(x, nph_vec3_sub(s0, p));
            if (s > edge_sep) {
                edge_sep = s;
                edge = e;
                edge_axis = x;
            }
        }
    }
    if (edge_sep > limit) {
        return;
    }
    const bool use_face = face_sep >= edge_sep - EDGE_BIAS;
    const f32 best = use_face ? face_sep : edge_sep;

    if (best > NPH_PHYSICS_LINEAR_SLOP) {
        // Apart: the exact gap from GJK, and its features when they are not
        // the axis test's.
        const NphPhysicsProxy pa_proxy = view_proxy(a, a->vertices);
        const NphVec3 seg[2] = {s0, s1};
        const NphPhysicsProxy pb_proxy = {seg, d_len_sq > 0.0F ? 2U : 1U, 0.0F};
        NphPhysicsDistance gd;
        nph_physics_distance(&pa_proxy, nph_physics_pose_identity(), &pb_proxy,
                             nph_physics_pose_identity(), false, NULL, &gd);
        if (gd.distance > limit) {
            return;
        }
        const NphVec3 sat_axis = use_face ? a->planes[face].normal : edge_axis;
        // An edge axis only bounds the gap from below; a face axis is exact
        // when GJK's features agree with it.
        if (gd.distance > 0.0F &&
            (!use_face || nph_vec3_dot(gd.normal, sat_axis) < FEATURE_AGREEMENT)) {
            const NphVec3 surface_b = nph_vec3_mul_add(gd.point_b, gd.normal, -radius);
            const NphVec3 mid = nph_vec3_scale(nph_vec3_add(gd.point_a, surface_b), 0.5F);
            out->normal = nph_physics_pose_transform_vector(pa, gd.normal);
            nph_physics_manifold_push(out, nph_physics_pose_transform_point(pa, mid),
                                      gd.distance - radius, 0x20000U);
            return;
        }
    }

    if (use_face) {
        const NphPlane pl = a->planes[face];
        NphVec3 p = s0;
        NphVec3 q = s1;
        if (!clip_segment_to_face(a, face, &p, &q)) {
            // The segment passes beside the face: its lower end decides.
            p = nph_plane_distance(pl, s0) <= nph_plane_distance(pl, s1) ? s0 : s1;
            q = p;
        }
        out->normal = nph_physics_pose_transform_vector(pa, pl.normal);
        const NphVec3 pts[2] = {p, q};
        const u32 count = nph_vec3_distance_sq(p, q) > 1e-12F ? 2U : 1U;
        for (u32 k = 0U; k < count; ++k) {
            const f32 dist = nph_plane_distance(pl, pts[k]);
            const f32 sep = dist - radius;
            if (sep <= speculative) {
                const NphVec3 mid = nph_vec3_mul_add(pts[k], pl.normal, -0.5F * (dist + radius));
                nph_physics_manifold_push(out, nph_physics_pose_transform_point(pa, mid), sep,
                                          (face << 8U) | k);
            }
        }
        return;
    }
    // Edge against the core: one point.
    const NphVec3 ep = a->vertices[a->edges[edge].origin];
    const NphVec3 eq = edge_head(a, a->vertices, edge);
    const NphPhysicsSegmentDistance sd = nph_physics_segment_distance(ep, eq, s0, s1);
    const f32 sep = nph_vec3_dot(nph_vec3_sub(sd.closest2, sd.closest1), edge_axis) - radius;
    if (sep > speculative) {
        return;
    }
    const NphVec3 surface_b = nph_vec3_mul_add(sd.closest2, edge_axis, -radius);
    const NphVec3 mid = nph_vec3_scale(nph_vec3_add(sd.closest1, surface_b), 0.5F);
    out->normal = nph_physics_pose_transform_vector(pa, edge_axis);
    nph_physics_manifold_push(out, nph_physics_pose_transform_point(pa, mid), sep, 0x10000U | edge);
}

// ---------------------------------------------------------------------------
// Hull and hull
// ---------------------------------------------------------------------------

typedef struct SatResult {
    f32 face_a_sep;
    u32 face_a;
    f32 face_b_sep;
    u32 face_b;
    f32 edge_sep;
    u32 edge_a;
    u32 edge_b;
    NphVec3 edge_axis; // in A's frame, pointing from A to B
} SatResult;

// B's vertices in A's frame, shared by the axis test and the contact.
typedef struct HullPair {
    const NphHullView *a;
    const NphHullView *b;
    NphPhysicsPose rel; // B in A's frame
    NphVec3 bv[NPH_PHYSICS_HULL_MAX_VERTICES];
} HullPair;

// The support of a vertex set along -n: the smallest projection.
static f32 min_projection(const NphVec3 *v, u32 count, NphVec3 n)
{
    f32 m = nph_vec3_dot(v[0], n);
    for (u32 i = 1U; i < count; ++i) {
        m = nph_f32_min(m, nph_vec3_dot(v[i], n));
    }
    return m;
}

static f32 face_a_separation(const HullPair *hp, u32 f)
{
    const NphPlane pl = hp->a->planes[f];
    return min_projection(hp->bv, hp->b->vertex_count, pl.normal) + pl.d;
}

// B's face f as a plane in A's frame.
static NphPlane plane_b_in_a(const HullPair *hp, u32 f)
{
    const NphPlane pl = hp->b->planes[f];
    const NphVec3 n = nph_quat_rotate(hp->rel.rotation, pl.normal);
    return nph_plane(n, pl.d - nph_vec3_dot(n, hp->rel.position));
}

static f32 face_b_separation(const HullPair *hp, u32 f)
{
    const NphPlane pl = plane_b_in_a(hp, f);
    return min_projection(hp->a->vertices, hp->a->vertex_count, pl.normal) + pl.d;
}

// The normal of the plane of B's arc cd: d x c, or for a flat edge (a
// triangle's, whose faces are opposite and whose normals' cross product
// vanishes) the cross product's limit as the angle between the faces
// closes: the edge reversed (origin less head, in A's frame).
static NphVec3 arc_plane(NphVec3 c, NphVec3 d, NphVec3 origin, NphVec3 head)
{
    const NphVec3 u = nph_vec3_cross(d, c);
    return nph_vec3_length_sq(u) > 1e-12F ? u : nph_vec3_sub(origin, head);
}

// Gregorius 2013: arcs ab and cd of the Gauss map intersect. c and d are
// B's face normals negated (the Minkowski difference A - B), dxc the
// normal of their arc's plane (arc_plane).
static bool is_minkowski_face(NphVec3 a, NphVec3 b, NphVec3 c, NphVec3 d, NphVec3 dxc)
{
    const NphVec3 bxa = nph_vec3_cross(b, a);
    const f32 cba = nph_vec3_dot(c, bxa);
    const f32 dba = nph_vec3_dot(d, bxa);
    const f32 adc = nph_vec3_dot(a, dxc);
    const f32 bdc = nph_vec3_dot(b, dxc);
    return cba * dba < 0.0F && adc * bdc < 0.0F && cba * bdc > 0.0F;
}

// The separation along the edge pair's cross product, oriented away from
// A's centroid; -FLT_MAX for nearly parallel edges, whose cross product is
// not a reliable axis (and is covered by the face axes).
static f32 edge_pair_separation(NphVec3 pa, NphVec3 ua, NphVec3 pb, NphVec3 ub, NphVec3 centroid,
                                NphVec3 *out_axis)
{
    const NphVec3 x = nph_vec3_cross(ua, ub);
    const f32 len = nph_vec3_length(x);
    if (len < 0.005F * nph_f32_sqrt(nph_vec3_length_sq(ua) * nph_vec3_length_sq(ub))) {
        return -FLT_MAX;
    }
    NphVec3 n = nph_vec3_scale(x, 1.0F / len);
    if (nph_vec3_dot(n, nph_vec3_sub(pa, centroid)) < 0.0F) {
        n = nph_vec3_neg(n);
    }
    *out_axis = n;
    return nph_vec3_dot(n, nph_vec3_sub(pb, pa));
}

// The axis test's vectorised form, for hulls of at most SIMD_MAX_VERTICES
// vertices: vertex sets in structure-of-arrays form, four projections per
// instruction; B's edges precomputed in A's frame, four Gauss-map tests per
// instruction. Every product and sum is the scalar path's, in the same
// order, so the decisions are bit-identical to it.
#define SIMD_MAX_VERTICES 64U
#define SIMD_MAX_EDGES    (3U * SIMD_MAX_VERTICES)

typedef struct SoaPoints {
    f32 x[SIMD_MAX_VERTICES];
    f32 y[SIMD_MAX_VERTICES];
    f32 z[SIMD_MAX_VERTICES];
    u32 count4; // padded to a multiple of four with copies of the last point
} SoaPoints;

typedef struct SoaEdges {
    // c = -n1, d = -n2 (B's face normals across the edge, in A's frame) and
    // u = d x c: everything the Gauss-map test needs from B.
    f32 cx[SIMD_MAX_EDGES];
    f32 cy[SIMD_MAX_EDGES];
    f32 cz[SIMD_MAX_EDGES];
    f32 dx[SIMD_MAX_EDGES];
    f32 dy[SIMD_MAX_EDGES];
    f32 dz[SIMD_MAX_EDGES];
    f32 ux[SIMD_MAX_EDGES];
    f32 uy[SIMD_MAX_EDGES];
    f32 uz[SIMD_MAX_EDGES];
    u32 edge[SIMD_MAX_EDGES]; // B's half-edge
    u32 count;
    u32 count4; // padded with zero vectors, which never pass the test
} SoaEdges;

static void soa_points(const NphVec3 *v, u32 count, SoaPoints *out)
{
    const u32 count4 = (count + 3U) & ~3U;
    for (u32 i = 0U; i < count4; ++i) {
        const NphVec3 p = v[i < count ? i : count - 1U];
        out->x[i] = p.x;
        out->y[i] = p.y;
        out->z[i] = p.z;
    }
    out->count4 = count4;
}

static f32 min_projection_soa(const SoaPoints *p, NphVec3 n)
{
    const NphF32x4 nx = nph_f32x4_splat(n.x);
    const NphF32x4 ny = nph_f32x4_splat(n.y);
    const NphF32x4 nz = nph_f32x4_splat(n.z);
    NphF32x4 m = nph_f32x4_splat(FLT_MAX);
    for (u32 i = 0U; i < p->count4; i += 4U) {
        const NphF32x4 x = nph_f32x4_load(&p->x[i]);
        const NphF32x4 y = nph_f32x4_load(&p->y[i]);
        const NphF32x4 z = nph_f32x4_load(&p->z[i]);
        const NphF32x4 d = nph_f32x4_add(nph_f32x4_add(nph_f32x4_mul(nx, x), nph_f32x4_mul(ny, y)),
                                         nph_f32x4_mul(nz, z));
        m = nph_f32x4_min(m, d);
    }
    m = nph_f32x4_min(m, NPH_F32X4_SHUFFLE(m, 2, 3, 0, 1));
    m = nph_f32x4_min(m, NPH_F32X4_SHUFFLE(m, 1, 0, 3, 2));
    return nph_f32x4_get_x(m);
}

// As edge_pair_separation, for the search: a pair that cannot beat `best`
// returns -FLT_MAX without the square root and division. With x the
// unnormalised axis and s_x its separation, s = s_x / |x| > best compares
// squares: for best < 0 any s_x >= 0 wins and a negative one wins when
// s_x^2 < best^2 |x|^2; for best >= 0 it needs s_x > 0 and s_x^2 > best^2 |x|^2.
static f32 edge_pair_separation_above(NphVec3 pa, NphVec3 ua, NphVec3 pb, NphVec3 ub,
                                      NphVec3 centroid, f32 best, NphVec3 *out_axis)
{
    NphVec3 x = nph_vec3_cross(ua, ub);
    const f32 len_sq = nph_vec3_length_sq(x);
    if (len_sq < 2.5e-5F * nph_vec3_length_sq(ua) * nph_vec3_length_sq(ub)) {
        return -FLT_MAX; // nearly parallel, as in edge_pair_separation
    }
    if (nph_vec3_dot(x, nph_vec3_sub(pa, centroid)) < 0.0F) {
        x = nph_vec3_neg(x);
    }
    const f32 sx = nph_vec3_dot(x, nph_vec3_sub(pb, pa));
    if (best > -FLT_MAX) {
        const f32 bound = best * best * len_sq;
        const bool wins =
            best < 0.0F ? (sx >= 0.0F || sx * sx < bound) : (sx > 0.0F && sx * sx > bound);
        if (!wins) {
            return -FLT_MAX;
        }
    }
    const f32 inv = 1.0F / nph_f32_sqrt(len_sq);
    *out_axis = nph_vec3_scale(x, inv);
    return sx * inv;
}

// The axis test proper. Returns false when an axis separates the pair by
// more than `limit`. The large locals live in this frame only.
static NPH_NO_INLINE bool sat_query(const HullPair *hp, f32 limit, NphPhysicsCollideCache *cache,
                                    SatResult *r)
{
    const NphHullView *a = hp->a;
    const NphHullView *b = hp->b;
    // Temporal coherence: the axis that separated the pair last time.
    if (cache != NULL && cache->type != CACHE_NONE) {
        f32 s = -FLT_MAX;
        if (cache->type == CACHE_FACE_A && cache->index_a < a->face_count) {
            s = face_a_separation(hp, cache->index_a);
        } else if (cache->type == CACHE_FACE_B && cache->index_b < b->face_count) {
            s = face_b_separation(hp, cache->index_b);
        } else if (cache->type == CACHE_EDGES && cache->index_a < a->edge_count &&
                   cache->index_b < b->edge_count) {
            const u32 ea = cache->index_a;
            const u32 eb = cache->index_b;
            const NphVec3 pa = a->vertices[a->edges[ea].origin];
            const NphVec3 pb = hp->bv[b->edges[eb].origin];
            NphVec3 axis;
            s = edge_pair_separation(pa, nph_vec3_sub(edge_head(a, a->vertices, ea), pa), pb,
                                     nph_vec3_sub(edge_head(b, hp->bv, eb), pb), a->centroid,
                                     &axis);
        }
        if (s > limit) {
            return false;
        }
    }
    const bool simd = a->vertex_count <= SIMD_MAX_VERTICES &&
                      b->vertex_count <= SIMD_MAX_VERTICES && b->edge_count / 2U <= SIMD_MAX_EDGES;
    SoaPoints sa;
    SoaPoints sb;
    if (simd) {
        soa_points(a->vertices, a->vertex_count, &sa);
        soa_points(hp->bv, b->vertex_count, &sb);
    }
    r->face_a_sep = -FLT_MAX;
    r->face_a = 0U;
    for (u32 f = 0U; f < a->face_count; ++f) {
        const NphPlane pl = a->planes[f];
        const f32 s = (simd ? min_projection_soa(&sb, pl.normal)
                            : min_projection(hp->bv, b->vertex_count, pl.normal)) +
                      pl.d;
        if (s > r->face_a_sep) {
            r->face_a_sep = s;
            r->face_a = f;
        }
    }
    if (r->face_a_sep > limit) {
        if (cache != NULL) {
            cache->type = CACHE_FACE_A;
            cache->index_a = (u16)r->face_a;
        }
        return false;
    }
    NphVec3 bn[HULL_MAX_FACES];
    r->face_b_sep = -FLT_MAX;
    r->face_b = 0U;
    for (u32 f = 0U; f < b->face_count; ++f) {
        const NphPlane pl = plane_b_in_a(hp, f);
        bn[f] = pl.normal;
        const f32 s = (simd ? min_projection_soa(&sa, pl.normal)
                            : min_projection(a->vertices, a->vertex_count, pl.normal)) +
                      pl.d;
        if (s > r->face_b_sep) {
            r->face_b_sep = s;
            r->face_b = f;
        }
    }
    if (r->face_b_sep > limit) {
        if (cache != NULL) {
            cache->type = CACHE_FACE_B;
            cache->index_b = (u16)r->face_b;
        }
        return false;
    }
    r->edge_sep = -FLT_MAX;
    r->edge_a = 0U;
    r->edge_b = 0U;
    r->edge_axis = nph_vec3_unit_y();
    if (simd) {
        SoaEdges se;
        se.count = 0U;
        for (u32 eb = 0U; eb < b->edge_count; eb += 2U) {
            const NphVec3 c = nph_vec3_neg(bn[b->edges[eb].face]);
            const NphVec3 d = nph_vec3_neg(bn[b->edges[eb + 1U].face]);
            const NphVec3 u =
                arc_plane(c, d, hp->bv[b->edges[eb].origin], edge_head(b, hp->bv, eb));
            const u32 k = se.count++;
            se.cx[k] = c.x;
            se.cy[k] = c.y;
            se.cz[k] = c.z;
            se.dx[k] = d.x;
            se.dy[k] = d.y;
            se.dz[k] = d.z;
            se.ux[k] = u.x;
            se.uy[k] = u.y;
            se.uz[k] = u.z;
            se.edge[k] = eb;
        }
        se.count4 = (se.count + 3U) & ~3U;
        for (u32 k = se.count; k < se.count4; ++k) {
            se.cx[k] = 0.0F;
            se.cy[k] = 0.0F;
            se.cz[k] = 0.0F;
            se.dx[k] = 0.0F;
            se.dy[k] = 0.0F;
            se.dz[k] = 0.0F;
            se.ux[k] = 0.0F;
            se.uy[k] = 0.0F;
            se.uz[k] = 0.0F;
            se.edge[k] = 0U;
        }
        const NphF32x4 zero = nph_f32x4_zero();
        for (u32 ea = 0U; ea < a->edge_count; ea += 2U) {
            const NphVec3 na1 = a->planes[a->edges[ea].face].normal;
            const NphVec3 na2 = a->planes[a->edges[ea + 1U].face].normal;
            const NphVec3 bxa = nph_vec3_cross(na2, na1);
            const NphF32x4 ax = nph_f32x4_splat(na1.x);
            const NphF32x4 ay = nph_f32x4_splat(na1.y);
            const NphF32x4 az = nph_f32x4_splat(na1.z);
            const NphF32x4 bx = nph_f32x4_splat(na2.x);
            const NphF32x4 by = nph_f32x4_splat(na2.y);
            const NphF32x4 bz = nph_f32x4_splat(na2.z);
            const NphF32x4 wx = nph_f32x4_splat(bxa.x);
            const NphF32x4 wy = nph_f32x4_splat(bxa.y);
            const NphF32x4 wz = nph_f32x4_splat(bxa.z);
            const NphVec3 pa = a->vertices[a->edges[ea].origin];
            const NphVec3 ua = nph_vec3_sub(edge_head(a, a->vertices, ea), pa);
            for (u32 k = 0U; k < se.count4; k += 4U) {
                const NphF32x4 cx = nph_f32x4_load(&se.cx[k]);
                const NphF32x4 cy = nph_f32x4_load(&se.cy[k]);
                const NphF32x4 cz = nph_f32x4_load(&se.cz[k]);
                const NphF32x4 dx = nph_f32x4_load(&se.dx[k]);
                const NphF32x4 dy = nph_f32x4_load(&se.dy[k]);
                const NphF32x4 dz = nph_f32x4_load(&se.dz[k]);
                const NphF32x4 ux = nph_f32x4_load(&se.ux[k]);
                const NphF32x4 uy = nph_f32x4_load(&se.uy[k]);
                const NphF32x4 uz = nph_f32x4_load(&se.uz[k]);
                // dot(c, bxa), dot(d, bxa), dot(a, u), dot(b, u), each as
                // (x x' + y y') + z z', like nph_vec3_dot.
                const NphF32x4 cba =
                    nph_f32x4_add(nph_f32x4_add(nph_f32x4_mul(cx, wx), nph_f32x4_mul(cy, wy)),
                                  nph_f32x4_mul(cz, wz));
                const NphF32x4 dba =
                    nph_f32x4_add(nph_f32x4_add(nph_f32x4_mul(dx, wx), nph_f32x4_mul(dy, wy)),
                                  nph_f32x4_mul(dz, wz));
                const NphF32x4 adc =
                    nph_f32x4_add(nph_f32x4_add(nph_f32x4_mul(ax, ux), nph_f32x4_mul(ay, uy)),
                                  nph_f32x4_mul(az, uz));
                const NphF32x4 bdc =
                    nph_f32x4_add(nph_f32x4_add(nph_f32x4_mul(bx, ux), nph_f32x4_mul(by, uy)),
                                  nph_f32x4_mul(bz, uz));
                const NphF32x4 m =
                    nph_f32x4_and(nph_f32x4_and(nph_f32x4_cmp_lt(nph_f32x4_mul(cba, dba), zero),
                                                nph_f32x4_cmp_lt(nph_f32x4_mul(adc, bdc), zero)),
                                  nph_f32x4_cmp_gt(nph_f32x4_mul(cba, bdc), zero));
                u32 mask = nph_f32x4_movemask(m);
                while (mask != 0U) {
                    const u32 lane = nph_ctz_u32(mask);
                    mask &= mask - 1U;
                    const u32 eb = se.edge[k + lane];
                    const NphVec3 pb = hp->bv[b->edges[eb].origin];
                    const NphVec3 ub = nph_vec3_sub(edge_head(b, hp->bv, eb), pb);
                    NphVec3 axis;
                    const f32 s =
                        edge_pair_separation_above(pa, ua, pb, ub, a->centroid, r->edge_sep, &axis);
                    if (s > r->edge_sep) {
                        r->edge_sep = s;
                        r->edge_a = ea;
                        r->edge_b = eb;
                        r->edge_axis = axis;
                    }
                }
            }
        }
    } else {
        for (u32 ea = 0U; ea < a->edge_count; ea += 2U) {
            const NphVec3 na1 = a->planes[a->edges[ea].face].normal;
            const NphVec3 na2 = a->planes[a->edges[ea + 1U].face].normal;
            const NphVec3 pa = a->vertices[a->edges[ea].origin];
            const NphVec3 ua = nph_vec3_sub(edge_head(a, a->vertices, ea), pa);
            for (u32 eb = 0U; eb < b->edge_count; eb += 2U) {
                const NphVec3 c = nph_vec3_neg(bn[b->edges[eb].face]);
                const NphVec3 d = nph_vec3_neg(bn[b->edges[eb + 1U].face]);
                const NphVec3 pb = hp->bv[b->edges[eb].origin];
                const NphVec3 hb = edge_head(b, hp->bv, eb);
                if (!is_minkowski_face(na1, na2, c, d, arc_plane(c, d, pb, hb))) {
                    continue;
                }
                const NphVec3 ub = nph_vec3_sub(hb, pb);
                NphVec3 axis;
                const f32 s =
                    edge_pair_separation_above(pa, ua, pb, ub, a->centroid, r->edge_sep, &axis);
                if (s > r->edge_sep) {
                    r->edge_sep = s;
                    r->edge_a = ea;
                    r->edge_b = eb;
                    r->edge_axis = axis;
                }
            }
        }
    }
    if (r->edge_sep > limit) {
        if (cache != NULL) {
            cache->type = CACHE_EDGES;
            cache->index_a = (u16)r->edge_a;
            cache->index_b = (u16)r->edge_b;
        }
        return false;
    }
    return true;
}

// The face contact: reference face `ref` of R (A when !flip), the incident
// face of the other hull clipped against its side planes. Works in A's
// frame; writes A-frame points and returns the normal from A to B. More
// than `max_points` candidates are reduced to four (the manifold's), in
// place in the free one of the two clip buffers, the frame's only large
// locals (10 KiB).
static NPH_NO_INLINE NphVec3 face_contact(const HullPair *hp, bool flip, u32 ref, f32 speculative,
                                          u32 max_points, NphPhysicsManifoldPoint *points,
                                          u32 *count)
{
    const NphHullView *rh = flip ? hp->b : hp->a;
    const NphHullView *ih = flip ? hp->a : hp->b;
    const NphVec3 *rv = flip ? hp->bv : hp->a->vertices;
    const NphVec3 *iv = flip ? hp->a->vertices : hp->bv;
    const NphPlane ref_plane = flip ? plane_b_in_a(hp, ref) : hp->a->planes[ref];
    const NphVec3 n = ref_plane.normal;
    // The incident face: the most anti-parallel to the reference normal.
    u32 inc = 0U;
    f32 best = FLT_MAX;
    for (u32 f = 0U; f < ih->face_count; ++f) {
        const NphVec3 fn = flip ? ih->planes[f].normal : plane_b_in_a(hp, f).normal;
        const f32 d = nph_vec3_dot(fn, n);
        if (d < best) {
            best = d;
            inc = f;
        }
    }
    NphPhysicsManifoldPoint buf[2][CLIP_CAPACITY];
    u32 len = 0U;
    {
        const u32 first = ih->faces[inc];
        u32 e = first;
        do {
            buf[0][len].point = iv[ih->edges[e].origin];
            buf[0][len].separation = 0.0F;
            buf[0][len].id = ih->edges[e].origin;
            len += 1U;
            e = ih->edges[e].next;
        } while (e != first && len < CLIP_CAPACITY && len < ih->edge_count);
    }
    u32 cur = 0U;
    const u32 first = rh->faces[ref];
    u32 e = first;
    u32 side = 0U;
    do {
        const NphVec3 a = rv[rh->edges[e].origin];
        const NphVec3 b = edge_head(rh, rv, e);
        const NphVec3 sn = nph_vec3_cross(nph_vec3_sub(b, a), n); // outward
        const f32 so = nph_vec3_dot(sn, a);
        const NphPhysicsManifoldPoint *in = buf[cur];
        NphPhysicsManifoldPoint *outp = buf[cur ^ 1U];
        u32 out_len = 0U;
        for (u32 i = 0U; i < len && out_len + 1U < CLIP_CAPACITY; ++i) {
            const NphPhysicsManifoldPoint p = in[i];
            const NphPhysicsManifoldPoint q = in[(i + 1U) % len];
            const f32 dp = nph_vec3_dot(sn, p.point) - so;
            const f32 dq = nph_vec3_dot(sn, q.point) - so;
            if (dp <= 0.0F) {
                outp[out_len++] = p;
            }
            if ((dp <= 0.0F) != (dq <= 0.0F)) {
                const f32 t = dp / (dp - dq);
                outp[out_len].point = nph_vec3_lerp(p.point, q.point, t);
                outp[out_len].separation = 0.0F;
                outp[out_len].id =
                    nph_physics_mix_id(nph_physics_mix_id(p.id, q.id), 0x100U + side);
                out_len += 1U;
            }
        }
        len = out_len;
        cur ^= 1U;
        e = rh->edges[e].next;
        side += 1U;
    } while (e != first && len > 0U && side < rh->edge_count);

    const u32 tag = nph_physics_mix_id((flip ? 0x80000000U : 0U) | (ref << 16U), inc);
    const NphPhysicsManifoldPoint *clipped = buf[cur];
    NphPhysicsManifoldPoint *cand = buf[cur ^ 1U];
    u32 n_out = 0U;
    for (u32 i = 0U; i < len; ++i) {
        const f32 sep = nph_plane_distance(ref_plane, clipped[i].point);
        if (sep <= speculative) {
            cand[n_out].point = nph_vec3_mul_add(clipped[i].point, n, -0.5F * sep);
            cand[n_out].separation = sep;
            cand[n_out].id = nph_physics_mix_id(tag, clipped[i].id);
            n_out += 1U;
        }
    }
    if (n_out > max_points) {
        n_out = nph_physics_manifold_reduce(cand, n_out, n);
    }
    for (u32 i = 0U; i < n_out; ++i) {
        points[i] = cand[i];
    }
    *count = n_out;
    return flip ? nph_vec3_neg(n) : n;
}

void nph_physics_collide_hull_face(const NphHullView *a, NphPhysicsPose pa, const NphHullView *b,
                                   NphPhysicsPose pb, u32 face, f32 speculative,
                                   NphTriangleContact *out)
{
    NPH_ASSERT(a->vertex_count <= NPH_PHYSICS_HULL_MAX_VERTICES &&
               b->vertex_count <= NPH_PHYSICS_HULL_MAX_VERTICES && face < b->face_count);
    HullPair hp;
    hp.a = a;
    hp.b = b;
    hp.rel = nph_physics_pose_inv_mul(pa, pb);
    for (u32 i = 0U; i < b->vertex_count; ++i) {
        hp.bv[i] = nph_physics_pose_transform_point(hp.rel, b->vertices[i]);
    }
    NphPhysicsManifoldPoint pts[NPH_TRIANGLE_CONTACT_POINTS];
    u32 count = 0U;
    const NphVec3 n =
        face_contact(&hp, true, face, speculative, NPH_TRIANGLE_CONTACT_POINTS, pts, &count);
    nph_triangle_contact_clear(out);
    out->normal = nph_physics_pose_transform_vector(pa, n);
    for (u32 i = 0U; i < count; ++i) {
        nph_triangle_contact_push(out, nph_physics_pose_transform_point(pa, pts[i].point),
                                  pts[i].separation, pts[i].id);
    }
}

f32 nph_physics_hulls_separation(const NphHullView *a, NphPhysicsPose pa, const NphHullView *b,
                                 NphPhysicsPose pb)
{
    HullPair hp;
    hp.a = a;
    hp.b = b;
    hp.rel = nph_physics_pose_inv_mul(pa, pb);
    for (u32 i = 0U; i < b->vertex_count; ++i) {
        hp.bv[i] = nph_physics_pose_transform_point(hp.rel, b->vertices[i]);
    }
    SatResult r;
    NPH_VERIFY(sat_query(&hp, FLT_MAX, NULL, &r));
    return nph_f32_max(nph_f32_max(r.face_a_sep, r.face_b_sep), r.edge_sep);
}

// The pair function, keeping up to `max_points` points. `prefer_b` turns
// the tie between faces to B's: a triangle's face, so a shape lying flat on
// it takes the triangle's own normal and its face is clipped to the
// triangle. `out` is left as it was when nothing touches.
static void collide_hulls(const NphHullView *a, NphPhysicsPose pa, const NphHullView *b,
                          NphPhysicsPose pb, f32 speculative, NphPhysicsCollideCache *cache,
                          bool prefer_b, u32 max_points, NphTriangleContact *out)
{
    NPH_ASSERT(a->vertex_count <= NPH_PHYSICS_HULL_MAX_VERTICES &&
               b->vertex_count <= NPH_PHYSICS_HULL_MAX_VERTICES);
    NPH_ASSERT(b->face_count <= HULL_MAX_FACES);
    HullPair hp;
    hp.a = a;
    hp.b = b;
    hp.rel = nph_physics_pose_inv_mul(pa, pb);
    for (u32 i = 0U; i < b->vertex_count; ++i) {
        hp.bv[i] = nph_physics_pose_transform_point(hp.rel, b->vertices[i]);
    }
    SatResult r;
    if (!sat_query(&hp, speculative, cache, &r)) {
        return;
    }
    const f32 face_sep = nph_f32_max(r.face_a_sep, r.face_b_sep);
    const bool use_edge = r.edge_sep > face_sep + EDGE_BIAS;
    const bool flip = !use_edge && (prefer_b ? r.face_b_sep >= r.face_a_sep - FACE_BIAS
                                             : r.face_b_sep > r.face_a_sep + FACE_BIAS);
    if (cache != NULL) {
        cache->type = (u8)(use_edge ? CACHE_EDGES : (flip ? CACHE_FACE_B : CACHE_FACE_A));
        cache->index_a = (u16)(use_edge ? r.edge_a : r.face_a);
        cache->index_b = (u16)(use_edge ? r.edge_b : r.face_b);
    }
    const f32 best = use_edge ? r.edge_sep : (flip ? r.face_b_sep : r.face_a_sep);

    if (best > NPH_PHYSICS_LINEAR_SLOP) {
        // Apart: GJK for the exact gap; corner contacts get its single point.
        const NphPhysicsProxy pa_proxy = view_proxy(a, a->vertices);
        const NphPhysicsProxy pb_proxy = view_proxy(b, hp.bv);
        NphPhysicsDistance gd;
        nph_physics_distance(&pa_proxy, nph_physics_pose_identity(), &pb_proxy,
                             nph_physics_pose_identity(), false, NULL, &gd);
        if (gd.distance > speculative) {
            return;
        }
        const NphVec3 sat_axis = use_edge ? r.edge_axis
                                          : (flip ? nph_vec3_neg(plane_b_in_a(&hp, r.face_b).normal)
                                                  : a->planes[r.face_a].normal);
        // An edge axis only bounds the gap from below; a face axis is exact
        // when GJK's features agree with it.
        if (gd.distance > 0.0F &&
            (use_edge || nph_vec3_dot(gd.normal, sat_axis) < FEATURE_AGREEMENT)) {
            const NphVec3 mid = nph_vec3_scale(nph_vec3_add(gd.point_a, gd.point_b), 0.5F);
            out->normal = nph_physics_pose_transform_vector(pa, gd.normal);
            nph_triangle_contact_push(out, nph_physics_pose_transform_point(pa, mid), gd.distance,
                                      0x40000000U);
            return;
        }
    }

    if (use_edge) {
        const NphVec3 pa0 = a->vertices[a->edges[r.edge_a].origin];
        const NphVec3 pa1 = edge_head(a, a->vertices, r.edge_a);
        const NphVec3 pb0 = hp.bv[b->edges[r.edge_b].origin];
        const NphVec3 pb1 = edge_head(b, hp.bv, r.edge_b);
        const NphPhysicsSegmentDistance sd = nph_physics_segment_distance(pa0, pa1, pb0, pb1);
        const f32 sep = nph_vec3_dot(nph_vec3_sub(sd.closest2, sd.closest1), r.edge_axis);
        if (sep > speculative) {
            return;
        }
        const NphVec3 mid = nph_vec3_scale(nph_vec3_add(sd.closest1, sd.closest2), 0.5F);
        out->normal = nph_physics_pose_transform_vector(pa, r.edge_axis);
        nph_triangle_contact_push(out, nph_physics_pose_transform_point(pa, mid), sep,
                                  nph_physics_mix_id(0x20000000U | r.edge_a, r.edge_b));
        return;
    }
    NphPhysicsManifoldPoint pts[NPH_TRIANGLE_CONTACT_POINTS];
    u32 count = 0U;
    const NphVec3 n =
        face_contact(&hp, flip, flip ? r.face_b : r.face_a, speculative, max_points, pts, &count);
    out->normal = nph_physics_pose_transform_vector(pa, n);
    for (u32 i = 0U; i < count; ++i) {
        nph_triangle_contact_push(out, nph_physics_pose_transform_point(pa, pts[i].point),
                                  pts[i].separation, pts[i].id);
    }
}

void nph_physics_collide_hulls(const NphHullView *a, NphPhysicsPose pa, const NphHullView *b,
                               NphPhysicsPose pb, f32 speculative, NphPhysicsCollideCache *cache,
                               NphPhysicsManifold *out)
{
    NphTriangleContact c;
    nph_triangle_contact_clear(&c);
    collide_hulls(a, pa, b, pb, speculative, cache, false, NPH_PHYSICS_MANIFOLD_MAX_POINTS, &c);
    if (c.point_count > 0U) {
        out->normal = c.normal;
        for (u32 i = 0U; i < c.point_count; ++i) {
            nph_physics_manifold_push(out, c.points[i].point, c.points[i].separation,
                                      c.points[i].id);
        }
    }
}

void nph_physics_collide_hull_triangle(const NphHullView *a, NphPhysicsPose pa,
                                       const NphHullView *triangle, f32 speculative,
                                       NphTriangleContact *out)
{
    nph_triangle_contact_clear(out);
    collide_hulls(a, pa, triangle, nph_physics_pose_identity(), speculative, NULL, true,
                  NPH_TRIANGLE_CONTACT_POINTS, out);
}

// ---------------------------------------------------------------------------
// Box and box: the fifteen axes of the oriented-box test (Gottschalk, Lin,
// and Manocha, "OBBTree", SIGGRAPH 1996; Ericson §4.4.1), whose projection
// radii are closed forms, so no Gauss-map search is needed; the contact is
// then made like any hull pair's.
// ---------------------------------------------------------------------------

typedef struct BoxAxes {
    f32 r[3][3];  // r[i][j] = A_i . B_j (in A's frame, B's axis j's component i)
    f32 ar[3][3]; // |r| + eps: near-parallel edge axes stay well defined
    f32 t[3];     // B's centre in A's frame
    f32 ha[3];
    f32 hb[3];
} BoxAxes;

// The separation along A's face axis i, with the side B is on.
static f32 box_face_a(const BoxAxes *x, u32 i, f32 *sign)
{
    const f32 rb = x->hb[0] * x->ar[i][0] + x->hb[1] * x->ar[i][1] + x->hb[2] * x->ar[i][2];
    *sign = x->t[i] >= 0.0F ? 1.0F : -1.0F;
    return nph_f32_abs(x->t[i]) - (x->ha[i] + rb);
}

static f32 box_face_b(const BoxAxes *x, u32 j, f32 *sign)
{
    const f32 tb = x->t[0] * x->r[0][j] + x->t[1] * x->r[1][j] + x->t[2] * x->r[2][j];
    const f32 ra = x->ha[0] * x->ar[0][j] + x->ha[1] * x->ar[1][j] + x->ha[2] * x->ar[2][j];
    *sign = tb >= 0.0F ? 1.0F : -1.0F;
    return nph_f32_abs(tb) - (ra + x->hb[j]);
}

// The separation along A_i x B_j (normalised); -FLT_MAX for parallel axes.
static f32 box_edge(const BoxAxes *x, u32 i, u32 j, NphVec3 *axis)
{
    const u32 i1 = (i + 1U) % 3U;
    const u32 i2 = (i + 2U) % 3U;
    const u32 j1 = (j + 1U) % 3U;
    const u32 j2 = (j + 2U) % 3U;
    const f32 len_sq = 1.0F - x->r[i][j] * x->r[i][j];
    if (len_sq < 1e-6F) {
        return -FLT_MAX;
    }
    const f32 ra = x->ha[i1] * x->ar[i2][j] + x->ha[i2] * x->ar[i1][j];
    const f32 rb = x->hb[j1] * x->ar[i][j2] + x->hb[j2] * x->ar[i][j1];
    const f32 tl = x->t[i2] * x->r[i1][j] - x->t[i1] * x->r[i2][j];
    const f32 inv_len = 1.0F / nph_f32_sqrt(len_sq);
    // A_i x B_j in A's frame, oriented from A towards B.
    f32 l[3];
    l[i] = 0.0F;
    l[i1] = -x->r[i2][j];
    l[i2] = x->r[i1][j];
    const f32 s = tl >= 0.0F ? inv_len : -inv_len;
    *axis = nph_vec3(l[0] * s, l[1] * s, l[2] * s);
    return (nph_f32_abs(tl) - (ra + rb)) * inv_len;
}

void nph_physics_collide_boxes(NphVec3 half_a, NphPhysicsPose pa, NphVec3 half_b, NphPhysicsPose pb,
                               f32 speculative, NphPhysicsCollideCache *cache,
                               NphPhysicsManifold *out)
{
    const NphPhysicsPose rel = nph_physics_pose_inv_mul(pa, pb);
    const NphMat3 rm = nph_mat3_from_quat(rel.rotation);
    BoxAxes x;
    for (u32 i = 0U; i < 3U; ++i) {
        for (u32 j = 0U; j < 3U; ++j) {
            x.r[i][j] = nph_vec3_at(rm.c[j], i);
            x.ar[i][j] = nph_f32_abs(x.r[i][j]) + 1e-6F;
        }
    }
    x.t[0] = rel.position.x;
    x.t[1] = rel.position.y;
    x.t[2] = rel.position.z;
    x.ha[0] = half_a.x;
    x.ha[1] = half_a.y;
    x.ha[2] = half_a.z;
    x.hb[0] = half_b.x;
    x.hb[1] = half_b.y;
    x.hb[2] = half_b.z;
    // Temporal coherence (the same cache layout as the hull path, with axis
    // numbers: face 2i + side, edges i and j).
    if (cache != NULL && cache->type != CACHE_NONE) {
        f32 sign;
        NphVec3 axis;
        f32 s = -FLT_MAX;
        if (cache->type == CACHE_FACE_A && cache->index_a < 6U) {
            s = box_face_a(&x, cache->index_a / 2U, &sign);
        } else if (cache->type == CACHE_FACE_B && cache->index_b < 6U) {
            s = box_face_b(&x, cache->index_b / 2U, &sign);
        } else if (cache->type == CACHE_EDGES && cache->index_a < 3U && cache->index_b < 3U) {
            s = box_edge(&x, cache->index_a, cache->index_b, &axis);
        }
        if (s > speculative) {
            return;
        }
    }
    f32 face_a_sep = -FLT_MAX;
    u32 face_a = 0U;
    for (u32 i = 0U; i < 3U; ++i) {
        f32 sign;
        const f32 s = box_face_a(&x, i, &sign);
        if (s > face_a_sep) {
            face_a_sep = s;
            face_a = 2U * i + (sign > 0.0F ? 1U : 0U);
        }
    }
    f32 face_b_sep = -FLT_MAX;
    u32 face_b = 0U;
    for (u32 j = 0U; j < 3U; ++j) {
        f32 sign;
        const f32 s = box_face_b(&x, j, &sign);
        if (s > face_b_sep) {
            face_b_sep = s;
            // A lies on B's -B_j side when B's centre is along +B_j from A's.
            face_b = 2U * j + (sign > 0.0F ? 0U : 1U);
        }
    }
    f32 edge_sep = -FLT_MAX;
    u32 edge_i = 0U;
    u32 edge_j = 0U;
    NphVec3 edge_axis = nph_vec3_unit_y();
    for (u32 i = 0U; i < 3U; ++i) {
        for (u32 j = 0U; j < 3U; ++j) {
            NphVec3 axis;
            const f32 s = box_edge(&x, i, j, &axis);
            if (s > edge_sep) {
                edge_sep = s;
                edge_i = i;
                edge_j = j;
                edge_axis = axis;
            }
        }
    }
    const f32 face_sep = nph_f32_max(face_a_sep, face_b_sep);
    const bool use_edge = edge_sep > face_sep + EDGE_BIAS;
    const bool flip = !use_edge && face_b_sep > face_a_sep + FACE_BIAS;
    const f32 best = use_edge ? edge_sep : (flip ? face_b_sep : face_a_sep);
    if (cache != NULL) {
        // A separated pair records the axis that separates it most.
        if (nph_f32_max(face_sep, edge_sep) > speculative) {
            const bool by_edge = edge_sep > face_sep;
            const bool by_b = !by_edge && face_b_sep > face_a_sep;
            cache->type = (u8)(by_edge ? CACHE_EDGES : (by_b ? CACHE_FACE_B : CACHE_FACE_A));
            cache->index_a = (u16)(by_edge ? edge_i : face_a);
            cache->index_b = (u16)(by_edge ? edge_j : face_b);
        } else {
            cache->type = CACHE_NONE;
        }
    }
    if (nph_f32_max(face_sep, edge_sep) > speculative) {
        return;
    }

    NphBoxViewStorage sa;
    NphBoxViewStorage sb;
    const NphHullView va = nph_physics_box_view(half_a, &sa);
    const NphHullView vb = nph_physics_box_view(half_b, &sb);
    HullPair hp;
    hp.a = &va;
    hp.b = &vb;
    hp.rel = rel;
    for (u32 i = 0U; i < 8U; ++i) {
        hp.bv[i] = nph_physics_pose_transform_point(rel, vb.vertices[i]);
    }
    if (best > NPH_PHYSICS_LINEAR_SLOP) {
        const NphPhysicsProxy pa_proxy = view_proxy(&va, va.vertices);
        const NphPhysicsProxy pb_proxy = view_proxy(&vb, hp.bv);
        NphPhysicsDistance gd;
        nph_physics_distance(&pa_proxy, nph_physics_pose_identity(), &pb_proxy,
                             nph_physics_pose_identity(), false, NULL, &gd);
        if (gd.distance > speculative) {
            return;
        }
        const NphVec3 sat_axis = use_edge ? edge_axis
                                          : (flip ? nph_vec3_neg(plane_b_in_a(&hp, face_b).normal)
                                                  : va.planes[face_a].normal);
        if (gd.distance > 0.0F &&
            (use_edge || nph_vec3_dot(gd.normal, sat_axis) < FEATURE_AGREEMENT)) {
            const NphVec3 mid = nph_vec3_scale(nph_vec3_add(gd.point_a, gd.point_b), 0.5F);
            out->normal = nph_physics_pose_transform_vector(pa, gd.normal);
            nph_physics_manifold_push(out, nph_physics_pose_transform_point(pa, mid), gd.distance,
                                      0x40000000U);
            return;
        }
    }
    if (use_edge) {
        // The supporting edges: A's along A_i at the corner farthest along
        // the axis, B's along B_j at the corner farthest against it.
        NphVec3 ca = nph_vec3_zero();
        for (u32 k = 0U; k < 3U; ++k) {
            if (k != edge_i) {
                const f32 s = nph_vec3_at(edge_axis, k) >= 0.0F ? x.ha[k] : -x.ha[k];
                if (k == 0U) {
                    ca.x = s;
                } else if (k == 1U) {
                    ca.y = s;
                } else {
                    ca.z = s;
                }
            }
        }
        NphVec3 ea = nph_vec3_zero();
        if (edge_i == 0U) {
            ea.x = x.ha[0];
        } else if (edge_i == 1U) {
            ea.y = x.ha[1];
        } else {
            ea.z = x.ha[2];
        }
        NphVec3 cb = rel.position;
        for (u32 m = 0U; m < 3U; ++m) {
            if (m != edge_j) {
                const f32 d = nph_vec3_dot(rm.c[m], edge_axis);
                cb = nph_vec3_mul_add(cb, rm.c[m], d >= 0.0F ? -x.hb[m] : x.hb[m]);
            }
        }
        const NphVec3 eb = nph_vec3_scale(rm.c[edge_j], x.hb[edge_j]);
        const NphPhysicsSegmentDistance sd = nph_physics_segment_distance(
            nph_vec3_sub(ca, ea), nph_vec3_add(ca, ea), nph_vec3_sub(cb, eb), nph_vec3_add(cb, eb));
        const f32 sep = nph_vec3_dot(nph_vec3_sub(sd.closest2, sd.closest1), edge_axis);
        if (sep > speculative) {
            return;
        }
        const NphVec3 mid = nph_vec3_scale(nph_vec3_add(sd.closest1, sd.closest2), 0.5F);
        out->normal = nph_physics_pose_transform_vector(pa, edge_axis);
        nph_physics_manifold_push(out, nph_physics_pose_transform_point(pa, mid), sep,
                                  nph_physics_mix_id(0x20000000U | edge_i, edge_j));
        return;
    }
    NphPhysicsManifoldPoint pts[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
    u32 count = 0U;
    const NphVec3 n = face_contact(&hp, flip, flip ? face_b : face_a, speculative,
                                   NPH_PHYSICS_MANIFOLD_MAX_POINTS, pts, &count);
    out->normal = nph_physics_pose_transform_vector(pa, n);
    for (u32 i = 0U; i < count; ++i) {
        nph_physics_manifold_push(out, nph_physics_pose_transform_point(pa, pts[i].point),
                                  pts[i].separation, pts[i].id);
    }
}
