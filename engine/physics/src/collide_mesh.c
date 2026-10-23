// SPDX-License-Identifier: Apache-2.0
// Convex shapes against triangles and triangle meshes (collide.h;
// docs/physics.md "Meshes"; ADR-0046 §5-§8).
//
// Each triangle is collided on its own, in a frame with its mesh's axes
// centred on the convex shape, so coordinates stay small however far the
// triangle lies from the mesh's origin:
//   - spheres and capsules by closest points (Ericson, "Real-Time Collision
//     Detection", 2005, §5.1.5 and §5.1.9), which are exact; a capsule lying
//     along the face gets two points, the ends of its part over the
//     triangle. (The axis test has no in-plane axes for a segment lying in
//     a flat triangle's plane, so it would touch a capsule lying beside the
//     triangle.)
//   - boxes and hulls by the separating axis test against the triangle as a
//     flat polyhedron (nph_physics_triangle_view), then clipping.
// Then the triangle's rules:
//   - one side: a triangle collides from its front only. A shape whose
//     centre is behind it gets nothing, and so does a contact that would push
//     a shape through it; a two-sided triangle turns its front to the shape.
//   - active edges: a contact whose deepest point lies inside the face, on an
//     inactive edge, or on a vertex whose active edges its normal does not
//     cross takes the face's normal, so a body sliding across a flat seam is
//     not pushed back by the next triangle's edge (Jolt's active edges,
//     after Bullet's internal-edge utility). The contact is then made again
//     against the face, as Jolt makes the manifold between the two faces
//     along the corrected normal: the shape's supporting feature clipped to
//     the triangle, the separations along the normal. A shape that is not
//     over the triangle gets nothing from it (its neighbour carries it), and
//     its points stay true support points: no point beside a sphere to brake
//     its roll, none outside a box's face to take a corner's place.
// A mesh's triangles under the shape give a manifold each. Those whose
// normals agree within 5 degrees merge (Jolt's manifold reduction), each
// keeps four points (Gregorius 2015), and the four with the deepest points
// are the pair's (ADR-0046 §8).

#include <nepheline/core/assert.h>
#include <nepheline/physics/mesh.h>

#include "physics_internal.h"

#include <float.h>

// Manifolds whose normals agree within 5 degrees merge.
#define MERGE_COS 0.99619470F
// Points of one merged manifold closer than this are one point: two
// triangles' copies of a point where a seam crosses the shape, which agree
// to rounding. (Wider would merge a corner into a crossing just beside it,
// and the corner's place is the one that matters.)
#define MERGE_DISTANCE 1e-4F
// A merged manifold gathers this many points before it is compacted.
#define CLUSTER_POINTS (8U * NPH_PHYSICS_MANIFOLD_MAX_POINTS)
// The normals followed at once; past them, the shallowest gives way.
#define MAX_CLUSTERS 8U
// A capsule's face contact wins over its nearest edge unless the edge is
// nearer by this much, as the hull test prefers faces (EDGE_BIAS).
#define FACE_PREFERENCE (0.5F * NPH_PHYSICS_LINEAR_SLOP)
// A point this near an edge's line, in metres, lies on the edge.
#define EDGE_TOLERANCE 1e-4F
// A normal this far below the face (its cosine with the face's normal)
// would push the shape through the triangle from behind.
#define AGAINST_FACE (-1e-4F)
// A hull contact's normal this near the face's (a quarter of a degree) is
// the face's.
#define FACE_COS 0.99999F
// A capsule this near parallel to an edge (the sine squared) lies along it.
#define PARALLEL_SIN_SQ 1e-4F

// Where on the triangle a contact lies: the face, the inside of edge i
// (from vertex i to vertex i + 1), or vertex i.
enum {
    FEATURE_FACE = 0U,
    FEATURE_EDGE = 1U,   // + the edge
    FEATURE_VERTEX = 4U, // + the vertex
};

// A triangle ready to collide: in the local frame, its front towards the
// shape.
typedef struct Tri {
    NphVec3 v[3];
    NphVec3 n;  // unit, the front
    u32 active; // NPH_PHYSICS_MESH_EDGE_*
} Tri;

NphDVec3 nph_physics_drotate_inverse(NphQuat q, NphDVec3 v)
{
    // v + 2 w (u x v) + 2 u x (u x v), with u the inverse's vector part.
    const NphDVec3 u = nph_dvec3(-(f64)q.x, -(f64)q.y, -(f64)q.z);
    const NphDVec3 t = nph_dvec3_scale(nph_dvec3_cross(u, v), 2.0);
    return nph_dvec3_add(nph_dvec3_add(v, nph_dvec3_scale(t, (f64)q.w)), nph_dvec3_cross(u, t));
}

// ---------------------------------------------------------------------------
// The triangle
// ---------------------------------------------------------------------------

// The unit normal of v, in 64 bits: a sliver's cross product keeps few of
// its digits in 32. Zero for a degenerate triangle.
static NphVec3 triangle_normal(const NphVec3 v[3])
{
    const f64 ax = (f64)v[1].x - (f64)v[0].x;
    const f64 ay = (f64)v[1].y - (f64)v[0].y;
    const f64 az = (f64)v[1].z - (f64)v[0].z;
    const f64 bx = (f64)v[2].x - (f64)v[0].x;
    const f64 by = (f64)v[2].y - (f64)v[0].y;
    const f64 bz = (f64)v[2].z - (f64)v[0].z;
    const NphDVec3 c = nph_dvec3(ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx);
    const f64 len = nph_dvec3_length(c);
    if (!(len > 0.0)) {
        return nph_vec3_zero();
    }
    return nph_vec3((f32)(c.x / len), (f32)(c.y / len), (f32)(c.z / len));
}

// Prepares triangle v (local frame) for a shape whose centre is `center`:
// false when it is degenerate, or one-sided with the centre behind it. A
// two-sided triangle with the centre behind it is reversed (v0 v2 v1, whose
// edges are the old 2, 1, and 0).
static bool prepare(Tri *t, const NphVec3 v[3], u32 active, bool two_sided, NphVec3 center)
{
    const NphVec3 n = triangle_normal(v);
    if (nph_vec3_length_sq(n) == 0.0F) {
        return false;
    }
    const bool behind = nph_vec3_dot(n, nph_vec3_sub(center, v[0])) < 0.0F;
    if (behind && !two_sided) {
        return false;
    }
    t->v[0] = v[0];
    if (!behind) {
        t->v[1] = v[1];
        t->v[2] = v[2];
        t->n = n;
        t->active = active;
    } else {
        t->v[1] = v[2];
        t->v[2] = v[1];
        t->n = nph_vec3_neg(n);
        t->active = ((active >> 2U) & 1U) | (active & 2U) | ((active & 1U) << 2U);
    }
    return true;
}

// Edge e's outward normal in the triangle's plane, as long as the edge.
static NphVec3 edge_outward(const Tri *t, u32 e)
{
    return nph_vec3_cross(nph_vec3_sub(t->v[(e + 1U) % 3U], t->v[e]), t->n);
}

static bool edge_active(const Tri *t, u32 e)
{
    return ((t->active >> e) & 1U) != 0U;
}

// The point of the triangle closest to p, and the feature it lies on
// (Ericson §5.1.5). Inside the face, p's projection along the normal.
static NphVec3 closest_on_triangle(const Tri *t, NphVec3 p, u32 *feature)
{
    const NphVec3 a = t->v[0];
    const NphVec3 b = t->v[1];
    const NphVec3 c = t->v[2];
    const NphVec3 ab = nph_vec3_sub(b, a);
    const NphVec3 ac = nph_vec3_sub(c, a);
    const NphVec3 ap = nph_vec3_sub(p, a);
    const f32 d1 = nph_vec3_dot(ab, ap);
    const f32 d2 = nph_vec3_dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        *feature = FEATURE_VERTEX + 0U;
        return a;
    }
    const NphVec3 bp = nph_vec3_sub(p, b);
    const f32 d3 = nph_vec3_dot(ab, bp);
    const f32 d4 = nph_vec3_dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        *feature = FEATURE_VERTEX + 1U;
        return b;
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        *feature = FEATURE_EDGE + 0U;
        return nph_vec3_mul_add(a, ab, d1 / (d1 - d3));
    }
    const NphVec3 cp = nph_vec3_sub(p, c);
    const f32 d5 = nph_vec3_dot(ab, cp);
    const f32 d6 = nph_vec3_dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        *feature = FEATURE_VERTEX + 2U;
        return c;
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        *feature = FEATURE_EDGE + 2U;
        return nph_vec3_mul_add(a, ac, d2 / (d2 - d6));
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && d4 - d3 >= 0.0F && d5 - d6 >= 0.0F) {
        *feature = FEATURE_EDGE + 1U;
        return nph_vec3_mul_add(b, nph_vec3_sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    *feature = FEATURE_FACE;
    return nph_vec3_mul_add(p, t->n, -nph_vec3_dot(t->n, ap));
}

// The feature holding point p, which lies on or near the triangle: the edge
// whose line it is within EDGE_TOLERANCE of (or beyond), the vertex of two
// such edges, or else the face.
static u32 feature_at(const Tri *t, NphVec3 p)
{
    u32 near = 0U;
    for (u32 e = 0U; e < 3U; ++e) {
        const NphVec3 m = edge_outward(t, e);
        if (nph_vec3_dot(m, nph_vec3_sub(p, t->v[e])) >= -EDGE_TOLERANCE * nph_vec3_length(m)) {
            near |= 1U << e;
        }
    }
    switch (near) {
    case 0U:
        return FEATURE_FACE;
    case 1U:
        return FEATURE_EDGE + 0U;
    case 2U:
        return FEATURE_EDGE + 1U;
    case 4U:
        return FEATURE_EDGE + 2U;
    case 3U:
        return FEATURE_VERTEX + 1U; // edges 0 and 1 meet at vertex 1
    case 6U:
        return FEATURE_VERTEX + 2U;
    case 5U:
        return FEATURE_VERTEX + 0U;
    default:
        break;
    }
    // Near all three: a triangle smaller than the tolerance. Its nearest
    // vertex.
    u32 best = 0U;
    for (u32 i = 1U; i < 3U; ++i) {
        if (nph_vec3_distance_sq(p, t->v[i]) < nph_vec3_distance_sq(p, t->v[best])) {
            best = i;
        }
    }
    return FEATURE_VERTEX + best;
}

// Whether a contact on `feature` keeps its normal `n` (from the triangle
// towards the shape): on an active edge it leans across, or on a vertex
// whose active edge it leans across. Vertex-region normals lean across at
// least one of the vertex's edges, and which one tells the edge the contact
// belongs to: where a flat seam meets the mesh's open boundary, a normal
// across the seam is the seam's.
static bool keeps_normal(const Tri *t, u32 feature, NphVec3 n)
{
    if (feature >= FEATURE_VERTEX) {
        const u32 i = feature - FEATURE_VERTEX;
        const u32 out = i;            // edge i leaves vertex i
        const u32 in = (i + 2U) % 3U; // and edge i + 2 arrives
        return (edge_active(t, out) && nph_vec3_dot(n, edge_outward(t, out)) > 0.0F) ||
               (edge_active(t, in) && nph_vec3_dot(n, edge_outward(t, in)) > 0.0F);
    }
    if (feature >= FEATURE_EDGE) {
        const u32 e = feature - FEATURE_EDGE;
        return edge_active(t, e) && nph_vec3_dot(n, edge_outward(t, e)) >= 0.0F;
    }
    return false;
}

// What the triangle's rules make of a contact.
typedef enum Rule {
    RULE_DROP = 0, // it would push the shape through from behind
    RULE_KEEP = 1, // its own normal stands
    RULE_FACE = 2, // the face's normal: made again against the face
} Rule;

// The triangle's rules on a contact at `feature` whose normal `n` points
// from the triangle towards the shape.
static Rule settle(const Tri *t, u32 feature, NphVec3 n)
{
    if (nph_vec3_dot(n, t->n) < AGAINST_FACE) {
        return RULE_DROP;
    }
    return keeps_normal(t, feature, n) ? RULE_KEEP : RULE_FACE;
}

// Whether point p, on or near the plane, lies over the triangle: inside its
// three side planes moved out by `grow`.
static bool over_triangle(const Tri *t, NphVec3 p, f32 grow)
{
    for (u32 e = 0U; e < 3U; ++e) {
        const NphVec3 m = edge_outward(t, e);
        if (nph_vec3_dot(m, nph_vec3_sub(p, t->v[e])) > grow * nph_vec3_length(m)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Spheres, capsules, boxes, and hulls against one triangle
// ---------------------------------------------------------------------------

// A round shape's contact point: on the line from the core point along the
// normal `n` (from the triangle towards the shape), midway between its
// surface and the triangle.
static NphVec3 round_point(NphVec3 core, NphVec3 n, f32 radius, f32 separation)
{
    return nph_vec3_mul_add(core, n, -(radius + 0.5F * separation));
}

static f32 min_separation(const NphTriangleContact *m)
{
    f32 s = FLT_MAX;
    for (u32 i = 0U; i < m->point_count; ++i) {
        s = nph_f32_min(s, m->points[i].separation);
    }
    return s;
}

static void sphere_triangle(const Tri *t, NphVec3 c, f32 radius, f32 speculative,
                            NphTriangleContact *out)
{
    u32 feature;
    const NphVec3 q = closest_on_triangle(t, c, &feature);
    const NphVec3 d = nph_vec3_sub(c, q);
    const f32 dist_sq = nph_vec3_length_sq(d);
    const f32 limit = radius + speculative;
    if (dist_sq > limit * limit) {
        return;
    }
    const f32 dist = nph_f32_sqrt(dist_sq);
    // A centre on the triangle (not behind it: prepare) goes out the front.
    NphVec3 n = dist > 1e-9F ? nph_vec3_scale(d, 1.0F / dist) : t->n;
    f32 sep = dist - radius;
    const Rule rule = settle(t, feature, n);
    if (rule == RULE_DROP) {
        return;
    }
    if (rule == RULE_FACE) {
        // Along the face's normal the sphere's lowest point is under its
        // centre: a centre beside the triangle is its neighbour's (to the
        // tolerance, so a centre over a seam belongs to both sides).
        const f32 h = nph_vec3_dot(t->n, nph_vec3_sub(c, t->v[0]));
        if (!over_triangle(t, nph_vec3_mul_add(c, t->n, -h), EDGE_TOLERANCE)) {
            return;
        }
        n = t->n;
        sep = h - radius;
    }
    out->normal = nph_vec3_neg(n);
    nph_triangle_contact_push(out, round_point(c, n, radius, sep), sep, 0U);
}

// The part [x_lo, x_hi] of the core p0 + x d (x in [0, 1]) over the
// triangle: inside its three side planes, moved out by `grow`. False when
// none of it is.
static bool core_over(const Tri *t, NphVec3 p0, NphVec3 d, f32 grow, f32 *x_lo, f32 *x_hi)
{
    f32 lo = 0.0F;
    f32 hi = 1.0F;
    for (u32 e = 0U; e < 3U; ++e) {
        const NphVec3 m = edge_outward(t, e);
        const f32 s0 = nph_vec3_dot(m, nph_vec3_sub(p0, t->v[e])) - grow * nph_vec3_length(m);
        const f32 sd = nph_vec3_dot(m, d);
        // Inside where s0 + x sd <= 0.
        if (sd == 0.0F) {
            if (s0 > 0.0F) {
                return false;
            }
        } else {
            const f32 x = -s0 / sd;
            if (sd > 0.0F) {
                hi = nph_f32_min(hi, x);
            } else {
                lo = nph_f32_max(lo, x);
            }
            if (lo > hi) {
                return false;
            }
        }
    }
    *x_lo = lo;
    *x_hi = hi;
    return true;
}

// A capsule against the face: the ends of the core's part [x_lo, x_hi] over
// it (those within the speculative distance), or one end when the part is
// shorter than a merge: the lower, or on a tie the core's own end (a
// capsule lying flat over a seam must keep its ends).
static void capsule_face(const Tri *t, NphVec3 p0, NphVec3 d, f32 x_lo, f32 x_hi, f32 radius,
                         f32 speculative, NphTriangleContact *out)
{
    const f32 h0 = nph_vec3_dot(t->n, nph_vec3_sub(p0, t->v[0]));
    const f32 hd = nph_vec3_dot(t->n, d);
    const f32 xs[2] = {x_lo, x_hi};
    const f32 hs[2] = {h0 + x_lo * hd, h0 + x_hi * hd};
    const bool both = (x_hi - x_lo) * nph_vec3_length(d) > MERGE_DISTANCE;
    u32 lower = hs[0] < hs[1] ? 0U : 1U;
    if (hs[0] == hs[1]) {
        lower = x_hi >= 1.0F ? 1U : 0U;
    }
    out->normal = nph_vec3_neg(t->n);
    for (u32 k = 0U; k < 2U; ++k) {
        const f32 sep = hs[k] - radius;
        if ((both || k == lower) && sep <= speculative) {
            const NphVec3 x = nph_vec3_mul_add(p0, d, xs[k]);
            nph_triangle_contact_push(out, round_point(x, t->n, radius, sep), sep, k);
        }
    }
}

static void capsule_triangle(const Tri *t, NphVec3 p0, NphVec3 p1, f32 radius, f32 speculative,
                             NphTriangleContact *out)
{
    const NphVec3 d = nph_vec3_sub(p1, p0);
    // The nearest edge (first of equals).
    NphPhysicsSegmentDistance best = nph_physics_segment_distance(p0, p1, t->v[0], t->v[1]);
    u32 edge = 0U;
    for (u32 e = 1U; e < 3U; ++e) {
        const NphPhysicsSegmentDistance sd =
            nph_physics_segment_distance(p0, p1, t->v[e], t->v[(e + 1U) % 3U]);
        if (sd.distance_squared < best.distance_squared) {
            best = sd;
            edge = e;
        }
    }
    const f32 edge_dist = nph_f32_sqrt(best.distance_squared);
    // The face, when part of the core is over it no farther than the edge.
    f32 x_lo = 0.0F;
    f32 x_hi = 0.0F;
    if (core_over(t, p0, d, 0.0F, &x_lo, &x_hi)) {
        const f32 h0 = nph_vec3_dot(t->n, nph_vec3_sub(p0, t->v[0]));
        const f32 hd = nph_vec3_dot(t->n, d);
        const f32 low = nph_f32_min(h0 + x_lo * hd, h0 + x_hi * hd);
        if (low <= edge_dist + FACE_PREFERENCE) {
            if (low - radius <= speculative) {
                capsule_face(t, p0, d, x_lo, x_hi, radius, speculative, out);
            }
            return;
        }
    }
    if (edge_dist > radius + speculative) {
        return;
    }
    // The nearest edge, or its end.
    u32 feature = FEATURE_EDGE + edge;
    if (best.fraction2 <= 0.0F) {
        feature = FEATURE_VERTEX + edge;
    } else if (best.fraction2 >= 1.0F) {
        feature = FEATURE_VERTEX + (edge + 1U) % 3U;
    }
    const NphVec3 n = edge_dist > 1e-9F ? nph_vec3_scale(nph_vec3_sub(best.closest1, best.closest2),
                                                         1.0F / edge_dist)
                                        : t->n;
    const Rule rule = settle(t, feature, n);
    if (rule == RULE_DROP) {
        return;
    }
    if (rule == RULE_FACE) {
        // Against the face: the part of the core over it (to the tolerance,
        // so a core over a seam belongs to both sides); a core beside it is
        // its neighbour's. A face contact far deeper than the true distance
        // is a core straddling the plane beside the triangle (hull_triangle).
        if (core_over(t, p0, d, EDGE_TOLERANCE, &x_lo, &x_hi)) {
            capsule_face(t, p0, d, x_lo, x_hi, radius, speculative, out);
            if (min_separation(out) < edge_dist - radius - NPH_PHYSICS_LINEAR_SLOP) {
                out->point_count = 0U;
            }
        }
        return;
    }
    out->normal = nph_vec3_neg(n);
    // Lying along the edge: the ends of the overlap, so a capsule against an
    // edge does not turn about one point (as two parallel capsules do).
    const NphVec3 a = t->v[edge];
    const NphVec3 u = nph_vec3_sub(t->v[(edge + 1U) % 3U], a);
    const f32 dd = nph_vec3_length_sq(d);
    const f32 uu = nph_vec3_length_sq(u);
    if (dd > 1e-12F && nph_vec3_length_sq(nph_vec3_cross(d, u)) <= PARALLEL_SIN_SQ * dd * uu) {
        const f32 len = nph_f32_sqrt(uu);
        const NphVec3 dir = nph_vec3_scale(u, 1.0F / len);
        const f32 a0 = nph_vec3_dot(nph_vec3_sub(p0, a), dir);
        const f32 a1 = nph_vec3_dot(nph_vec3_sub(p1, a), dir);
        const f32 lo = nph_f32_max(0.0F, nph_f32_min(a0, a1));
        const f32 hi = nph_f32_min(len, nph_f32_max(a0, a1));
        if (hi - lo > NPH_PHYSICS_LINEAR_SLOP) {
            const f32 ends[2] = {lo, hi};
            for (u32 k = 0U; k < 2U; ++k) {
                const NphVec3 q = nph_vec3_mul_add(a, dir, ends[k]);
                f32 s;
                const NphVec3 x = nph_closest_point_on_segment(p0, p1, q, &s);
                const f32 sep = nph_vec3_distance(x, q) - radius;
                if (sep <= speculative) {
                    nph_triangle_contact_push(out, round_point(x, n, radius, sep), sep, 2U + k);
                }
            }
            if (out->point_count > 0U) {
                return;
            }
        }
    }
    const f32 sep = edge_dist - radius;
    nph_triangle_contact_push(out, round_point(best.closest1, n, radius, sep), sep, 4U);
}

static void hull_triangle(const NphHullView *a, NphPhysicsPose pa, const Tri *t, f32 speculative,
                          NphTriangleContact *out)
{
    NphTriangleViewStorage storage;
    const NphHullView b = nph_physics_triangle_view(t->v, t->n, &storage);
    nph_physics_collide_hull_triangle(a, pa, &b, speculative, out);
    // The face's own normal already (the pair function prefers the
    // triangle's face): the contact is the face's.
    if (out->point_count == 0U || nph_vec3_dot(out->normal, t->n) <= -FACE_COS) {
        return;
    }
    u32 deepest = 0U;
    for (u32 i = 1U; i < out->point_count; ++i) {
        if (out->points[i].separation < out->points[deepest].separation) {
            deepest = i;
        }
    }
    // The deepest point's place on the triangle: half its separation along
    // the normal from the midpoint, for every kind of contact the pair
    // function makes.
    const f32 depth = out->points[deepest].separation;
    const NphVec3 on = nph_vec3_mul_add(out->points[deepest].point, out->normal, 0.5F * depth);
    const Rule rule = settle(t, feature_at(t, on), nph_vec3_neg(out->normal));
    if (rule == RULE_KEEP) {
        return;
    }
    if (rule == RULE_FACE) {
        // Made again against the face (the view's face 0): the shape's face
        // most opposed to it, clipped to the triangle. A shape that comes out
        // far deeper than the axis test found it straddles the triangle's
        // plane beside it (pressed against an open edge, at a vertex whose
        // edges in this triangle are inside the mesh): its contact belongs
        // to the triangle's neighbours.
        nph_physics_collide_hull_face(a, pa, &b, nph_physics_pose_identity(), 0U, speculative, out);
        if (out->point_count == 0U || min_separation(out) >= depth - NPH_PHYSICS_LINEAR_SLOP) {
            return;
        }
    }
    out->point_count = 0U;
}

// The convex shape in the local frame: at the origin, with its rotation.
// Holds a box's view, so it is not copied once made.
typedef struct Probe {
    const NphPhysicsGeometry *g;
    NphPhysicsPose pose;
    NphVec3 center;  // its centroid, for the side test
    NphVec3 core[2]; // a capsule's segment
    f32 radius;
    NphBoxViewStorage box;
    NphHullView view; // boxes and hulls
} Probe;

static void probe_init(Probe *p, const NphPhysicsGeometry *g, NphQuat rotation)
{
    p->g = g;
    p->pose = nph_physics_pose(nph_vec3_zero(), rotation);
    p->center = nph_vec3_zero();
    p->radius = 0.0F;
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        p->radius = g->sphere.radius;
        return;
    case NPH_PHYSICS_SHAPE_CAPSULE: {
        const NphVec3 axis = nph_vec3_scale(nph_quat_axis_y(rotation), g->capsule.half_height);
        p->core[0] = nph_vec3_neg(axis);
        p->core[1] = axis;
        p->radius = g->capsule.radius;
        return;
    }
    case NPH_PHYSICS_SHAPE_BOX:
        p->view = nph_physics_box_view(g->box.half_extents, &p->box);
        return;
    case NPH_PHYSICS_SHAPE_HULL:
        p->view = nph_physics_hull_view(g->hull);
        p->center = nph_quat_rotate(rotation, g->hull->centroid);
        return;
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
    NPH_ASSERT_MSG(false, "physics: shape type %u against a triangle", (u32)g->type);
}

static void probe_triangle(const Probe *p, const Tri *t, f32 speculative, NphTriangleContact *out)
{
    nph_triangle_contact_clear(out);
    switch (p->g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        sphere_triangle(t, nph_vec3_zero(), p->radius, speculative, out);
        return;
    case NPH_PHYSICS_SHAPE_CAPSULE:
        if (p->g->capsule.half_height > 0.0F) {
            capsule_triangle(t, p->core[0], p->core[1], p->radius, speculative, out);
        } else {
            sphere_triangle(t, nph_vec3_zero(), p->radius, speculative, out);
        }
        return;
    case NPH_PHYSICS_SHAPE_BOX:
    case NPH_PHYSICS_SHAPE_HULL:
        hull_triangle(&p->view, p->pose, t, speculative, out);
        return;
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
}

void nph_physics_collide_triangle(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                                  const NphPhysicsTriangle *t, f32 speculative_distance,
                                  NphPhysicsManifold *out)
{
    NPH_ASSERT(a != NULL && t != NULL && out != NULL);
    NPH_ASSERT(nph_physics_shape_type_is_convex(a->type));
    nph_physics_manifold_clear(out);
    Probe p;
    probe_init(&p, a, pose_a.rotation);
    NphVec3 v[3];
    for (u32 i = 0U; i < 3U; ++i) {
        v[i] = nph_vec3_sub(t->vertices[i], pose_a.position);
    }
    Tri tri;
    if (!prepare(&tri, v, t->active_edges & NPH_PHYSICS_MESH_EDGES, t->two_sided != 0, p.center)) {
        return;
    }
    NphTriangleContact c;
    probe_triangle(&p, &tri, nph_f32_max(speculative_distance, 0.0F), &c);
    c.point_count = nph_physics_manifold_reduce(c.points, c.point_count, c.normal);
    if (c.point_count > 0U) {
        out->normal = c.normal;
    }
    for (u32 i = 0U; i < c.point_count; ++i) {
        nph_physics_manifold_push(out, nph_vec3_add(c.points[i].point, pose_a.position),
                                  c.points[i].separation, c.points[i].id);
    }
}

// ---------------------------------------------------------------------------
// Meshes: every triangle under the shape, the manifolds merged by normal
// ---------------------------------------------------------------------------

// Manifolds merged into one. A triangle joins the first whose first
// member's normal is within 5 degrees of its own: a test against fixed
// geometry, so a resting body's triangles group the same way every step.
// The normal is the average of the members', each weighted by how near it
// comes (nothing at the speculative distance, more the deeper it reaches),
// so it turns smoothly as a body rocks and as triangles come and go. (The
// deepest member's normal jumps between two members about as deep, step
// after step: a box on a curved terrain rocked on it and never slept.)
typedef struct Cluster {
    NphVec3 first;  // the first member's normal, for membership
    NphVec3 normal; // from the shape towards the mesh: the sum's direction
    NphVec3 sum;    // the members' weighted normals
    f32 depth;      // the least separation
    u32 count;
    NphPhysicsManifoldPoint points[CLUSTER_POINTS];
} Cluster;

typedef struct MeshQuery {
    const NphVec3 *vertices;
    const u32 *triangles;
    const u8 *edges;
    NphDVec3 position; // the shape's, in the mesh's frame
    f32 speculative;
    bool two_sided;
    bool single; // a sphere: one point per normal
    u32 cluster_count;
    Probe probe;
    Cluster clusters[MAX_CLUSTERS];
} MeshQuery;

// The directions in the contact plane along which a full cluster keeps its
// farthest points: every 45 degrees, so each corner of a rectangle (a box's
// face) is the farthest along at least two of them.
static const f32 k_compact[8][2] = {
    {1.0F, 0.0F},  {0.70710678F, 0.70710678F},   {0.0F, 1.0F},  {-0.70710678F, 0.70710678F},
    {-1.0F, 0.0F}, {-0.70710678F, -0.70710678F}, {0.0F, -1.0F}, {0.70710678F, -0.70710678F},
};

// Compacts a full cluster to the points the final reduction can want, in
// their order: the deepest; of the touching points, the farthest along
// eight directions in the contact plane; of the speculative ones, along
// four. Reducing to four as it fills would lose corners for good (four of
// the first sixteen points need not hold a corner of the whole contact: a
// box over a fine mesh brings dozens), and keeping extremes alone, touching
// or not, gave a supporting point's place to a far speculative one.
static void cluster_compact(Cluster *c)
{
    const NphVec3 t1 = nph_vec3_perpendicular(c->normal);
    const NphVec3 t2 = nph_vec3_cross(c->normal, t1);
    bool keep[CLUSTER_POINTS];
    u32 deepest = 0U;
    for (u32 i = 0U; i < c->count; ++i) {
        keep[i] = false;
        if (c->points[i].separation < c->points[deepest].separation) {
            deepest = i;
        }
    }
    keep[deepest] = true;
    for (u32 pass = 0U; pass < 2U; ++pass) {
        const bool touching = pass == 0U;
        for (u32 k = 0U; k < 8U; k += touching ? 1U : 2U) {
            const NphVec3 dir = nph_vec3_add(nph_vec3_scale(t1, k_compact[k][0]),
                                             nph_vec3_scale(t2, k_compact[k][1]));
            u32 far = NPH_U32_MAX;
            f32 far_d = -FLT_MAX;
            for (u32 i = 0U; i < c->count; ++i) {
                const f32 d = nph_vec3_dot(c->points[i].point, dir);
                if ((c->points[i].separation <= NPH_PHYSICS_TOUCH_SEPARATION) == touching &&
                    d > far_d) {
                    far_d = d;
                    far = i;
                }
            }
            if (far != NPH_U32_MAX) {
                keep[far] = true;
            }
        }
    }
    u32 n = 0U;
    for (u32 i = 0U; i < c->count; ++i) {
        if (keep[i]) {
            c->points[n++] = c->points[i];
        }
    }
    c->count = n;
}

static void cluster_add(MeshQuery *q, const NphTriangleContact *m, u32 triangle)
{
    f32 depth = FLT_MAX;
    for (u32 i = 0U; i < m->point_count; ++i) {
        depth = nph_f32_min(depth, m->points[i].separation);
    }
    Cluster *c = NULL;
    for (u32 k = 0U; k < q->cluster_count; ++k) {
        if (nph_vec3_dot(q->clusters[k].first, m->normal) >= MERGE_COS) {
            c = &q->clusters[k];
            break;
        }
    }
    if (c == NULL) {
        if (q->cluster_count < MAX_CLUSTERS) {
            c = &q->clusters[q->cluster_count++];
        } else {
            // Full: the shallowest gives way to a deeper one.
            u32 s = 0U;
            for (u32 k = 1U; k < MAX_CLUSTERS; ++k) {
                if (q->clusters[k].depth >= q->clusters[s].depth) {
                    s = k;
                }
            }
            if (!(depth < q->clusters[s].depth)) {
                return;
            }
            c = &q->clusters[s];
        }
        c->first = m->normal;
        c->sum = nph_vec3_zero();
        c->depth = depth;
        c->count = 0U;
    }
    c->sum = nph_vec3_mul_add(c->sum, m->normal, nph_f32_max(q->speculative - depth, 0.0F) + 1e-6F);
    c->normal = nph_vec3_normalize_or(c->sum, m->normal);
    c->depth = nph_f32_min(c->depth, depth);
    if (q->single) {
        // A sphere touches a surface at one point: the deepest (the first
        // of equals), placed again along the merged normal at the end.
        if (c->count == 0U || m->points[0].separation < c->points[0].separation) {
            c->points[0] = m->points[0];
            c->points[0].id = nph_physics_mix_id(triangle, m->points[0].id);
            c->count = 1U;
        }
        return;
    }
    for (u32 i = 0U; i < m->point_count; ++i) {
        NphPhysicsManifoldPoint p = m->points[i];
        p.id = nph_physics_mix_id(triangle, p.id);
        // A point two triangles share (where a seam crosses the shape) is
        // one point: the first's, as deep as either.
        bool merged = false;
        for (u32 j = 0U; j < c->count && !merged; ++j) {
            if (nph_vec3_distance_sq(c->points[j].point, p.point) <=
                MERGE_DISTANCE * MERGE_DISTANCE) {
                c->points[j].separation = nph_f32_min(c->points[j].separation, p.separation);
                merged = true;
            }
        }
        if (merged) {
            continue;
        }
        if (c->count == CLUSTER_POINTS) {
            cluster_compact(c);
        }
        c->points[c->count++] = p;
    }
}

static bool visit_triangle(void *user, u32 t)
{
    MeshQuery *q = (MeshQuery *)user;
    const u32 *tri = q->triangles + 3ULL * t;
    NphVec3 v[3];
    for (u32 i = 0U; i < 3U; ++i) {
        // Moved next to the shape in 64 bits, then rounded: small and exact.
        const NphVec3 w = q->vertices[tri[i]];
        v[i] = nph_vec3((f32)((f64)w.x - q->position.x), (f32)((f64)w.y - q->position.y),
                        (f32)((f64)w.z - q->position.z));
    }
    Tri prepared;
    if (!prepare(&prepared, v, q->edges[t], q->two_sided, q->probe.center)) {
        return true;
    }
    NphTriangleContact m;
    probe_triangle(&q->probe, &prepared, q->speculative, &m);
    if (m.point_count > 0U) {
        cluster_add(q, &m, t);
    }
    return true;
}

void nph_physics_collide_mesh_local(const NphPhysicsGeometry *a, NphQuat rotation,
                                    NphDVec3 position, const NphPhysicsMesh *mesh, f32 speculative,
                                    NphPhysicsMeshManifolds *out)
{
    NPH_ASSERT(a != NULL && mesh != NULL && out != NULL);
    NPH_ASSERT(nph_physics_shape_type_is_convex(a->type));
    out->count = 0U;
    MeshQuery q;
    q.vertices = nph_physics_mesh_vertices(mesh);
    q.triangles = nph_physics_mesh_triangles(mesh);
    q.edges = nph_physics_mesh_edges(mesh);
    q.position = position;
    q.speculative = nph_f32_max(speculative, 0.0F);
    q.two_sided = (mesh->flags & NPH_PHYSICS_MESH_TWO_SIDED) != 0U;
    q.single = a->type == NPH_PHYSICS_SHAPE_SPHERE ||
               (a->type == NPH_PHYSICS_SHAPE_CAPSULE && !(a->capsule.half_height > 0.0F));
    q.cluster_count = 0U;
    probe_init(&q.probe, a, rotation);
    // The shape's box in the mesh's frame, grown by the speculative
    // distance and by the rounding of its position to 32 bits.
    const NphVec3 at = nph_vec3((f32)position.x, (f32)position.y, (f32)position.z);
    const f32 rounding = 2.4e-7F * nph_vec3_max_component(nph_vec3_abs(at));
    const NphAABB box = nph_aabb_grow(nph_physics_geometry_aabb(a, nph_physics_pose(at, rotation)),
                                      q.speculative + rounding);
    nph_physics_mesh_overlap(mesh, box, visit_triangle, &q);
    // Each merged manifold reduced to four points; the deepest four, the
    // first found of equals first.
    u32 order[MAX_CLUSTERS];
    for (u32 k = 0U; k < q.cluster_count; ++k) {
        Cluster *c = &q.clusters[k];
        c->count = nph_physics_manifold_reduce(c->points, c->count, c->normal);
        u32 j = k;
        while (j > 0U && q.clusters[order[j - 1U]].depth > c->depth) {
            order[j] = order[j - 1U];
            j -= 1U;
        }
        order[j] = k;
    }
    const u32 keep = nph_min_u32(q.cluster_count, NPH_PHYSICS_MESH_MAX_MANIFOLDS);
    for (u32 i = 0U; i < keep; ++i) {
        const Cluster *c = &q.clusters[order[i]];
        NphPhysicsManifold *m = &out->manifolds[i];
        m->normal = c->normal;
        m->point_count = c->count;
        for (u32 j = 0U; j < c->count; ++j) {
            m->points[j] = c->points[j];
        }
        if (q.single) {
            // The sphere's point under its centre along the merged normal:
            // its push passes through the centre.
            const f32 sep = m->points[0].separation;
            m->points[0].point = nph_vec3_scale(m->normal, q.probe.radius + 0.5F * sep);
        }
    }
    out->count = keep;
}

void nph_physics_collide_mesh(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                              const NphPhysicsMesh *mesh, NphPhysicsPose pose_mesh,
                              f32 speculative_distance, NphPhysicsMeshManifolds *out)
{
    NPH_ASSERT(a != NULL && mesh != NULL && out != NULL);
    const NphQuat rotation = nph_quat_mul(nph_quat_conjugate(pose_mesh.rotation), pose_a.rotation);
    const NphDVec3 d = nph_dvec3((f64)pose_a.position.x - (f64)pose_mesh.position.x,
                                 (f64)pose_a.position.y - (f64)pose_mesh.position.y,
                                 (f64)pose_a.position.z - (f64)pose_mesh.position.z);
    nph_physics_collide_mesh_local(a, rotation, nph_physics_drotate_inverse(pose_mesh.rotation, d),
                                   mesh, speculative_distance, out);
    for (u32 i = 0U; i < out->count; ++i) {
        NphPhysicsManifold *m = &out->manifolds[i];
        m->normal = nph_quat_rotate(pose_mesh.rotation, m->normal);
        for (u32 j = 0U; j < m->point_count; ++j) {
            m->points[j].point = nph_vec3_add(
                pose_a.position, nph_quat_rotate(pose_mesh.rotation, m->points[j].point));
        }
    }
}
