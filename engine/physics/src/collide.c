// SPDX-License-Identifier: Apache-2.0
// The narrow-phase dispatcher, the closed-form pairs of spheres, capsules,
// and boxes with spheres, segment distance, and manifold reduction
// (collide.h; docs/physics.md "Collision").

#include <nepheline/core/assert.h>

#include "physics_internal.h"

#include <float.h>

// ---------------------------------------------------------------------------
// Segment distance (Ericson, "Real-Time Collision Detection", §5.1.9)
// ---------------------------------------------------------------------------

NphPhysicsSegmentDistance nph_physics_segment_distance(NphVec3 p1, NphVec3 q1, NphVec3 p2,
                                                       NphVec3 q2)
{
    const NphVec3 d1 = nph_vec3_sub(q1, p1);
    const NphVec3 d2 = nph_vec3_sub(q2, p2);
    const NphVec3 r = nph_vec3_sub(p1, p2);
    const f32 a = nph_vec3_dot(d1, d1);
    const f32 e = nph_vec3_dot(d2, d2);
    const f32 f = nph_vec3_dot(d2, r);
    const f32 eps = 1e-12F;
    f32 s = 0.0F;
    f32 t = 0.0F;
    if (a <= eps && e <= eps) {
        s = 0.0F;
        t = 0.0F;
    } else if (a <= eps) {
        t = nph_f32_saturate(f / e);
    } else {
        const f32 c = nph_vec3_dot(d1, r);
        if (e <= eps) {
            s = nph_f32_saturate(-c / a);
        } else {
            const f32 b = nph_vec3_dot(d1, d2);
            const f32 denom = a * e - b * b;
            // Parallel segments (denom 0) take s = 0 and the nearest point of
            // the other segment to p1.
            s = denom > 0.0F ? nph_f32_saturate((b * f - c * e) / denom) : 0.0F;
            t = (b * s + f) / e;
            if (t < 0.0F) {
                t = 0.0F;
                s = nph_f32_saturate(-c / a);
            } else if (t > 1.0F) {
                t = 1.0F;
                s = nph_f32_saturate((b - c) / a);
            }
        }
    }
    NphPhysicsSegmentDistance out;
    out.closest1 = nph_vec3_mul_add(p1, d1, s);
    out.closest2 = nph_vec3_mul_add(p2, d2, t);
    out.fraction1 = s;
    out.fraction2 = t;
    out.distance_squared = nph_vec3_distance_sq(out.closest1, out.closest2);
    return out;
}

// ---------------------------------------------------------------------------
// Manifold reduction: Gregorius ("Robust Contact Creation for Physics
// Simulations", GDC 2015), by catch angle
// ---------------------------------------------------------------------------

static f32 signed_area(NphVec3 a, NphVec3 b, NphVec3 c, NphVec3 normal)
{
    return nph_vec3_dot(nph_vec3_cross(nph_vec3_sub(b, a), nph_vec3_sub(c, a)), normal);
}

// Points within this of the deepest are as deep: they differ by rounding.
#define REDUCE_LEVEL (0.002F * NPH_PHYSICS_LINEAR_SLOP)
// Catch angles within this fraction of the least are as small.
#define REDUCE_ANGLE_TIE 0.1F
// Candidates nearer a line than this fraction of its length turn with it,
// and never catch the shape turning about it; those nearer the deepest
// than this (metres) are at its place.
#define REDUCE_ON_LINE  1e-3F
#define REDUCE_AT_POINT 1e-6F

static f32 plane_distance_sq(NphVec3 a, NphVec3 b, NphVec3 normal)
{
    return nph_vec3_length_sq(nph_vec3_reject(nph_vec3_sub(a, b), normal));
}

static void swap_points(NphPhysicsManifoldPoint *points, u32 i, u32 j)
{
    const NphPhysicsManifoldPoint t = points[i];
    points[i] = points[j];
    points[j] = t;
}

// An axis the shape may turn about on its contact's first points: the
// deepest alone, or a line through it.
typedef struct ReduceAxis {
    NphVec3 origin;
    NphVec3 across; // a line's: unit, in the contact plane, across it
    NphVec3 normal;
    bool about_point;
    f32 side;    // a line's: +1 or -1, candidates on that side only; 0, either
    f32 min_arm; // arms at most this cannot catch the turn
} ReduceAxis;

// A candidate's lever arm about the axis (squared about a point: a
// distance, kept free of the square root), or none when it cannot catch
// the turn.
static f32 reduce_arm(const ReduceAxis *ax, NphVec3 p)
{
    const NphVec3 d = nph_vec3_sub(p, ax->origin);
    f32 arm;
    if (ax->about_point) {
        arm = nph_vec3_length_sq(nph_vec3_reject(d, ax->normal));
    } else {
        const f32 a = nph_vec3_dot(d, ax->across);
        arm = ax->side == 0.0F ? nph_f32_abs(a) : ax->side * a;
    }
    return arm > ax->min_arm ? arm : 0.0F;
}

// The candidate in points[from, end) that touches first as the shape turns
// about the axis: the least catch angle, height above `level` over arm, and
// of angles within REDUCE_ANGLE_TIE of it, the longest arm. A level
// candidate (no height) has the least, and of several the longest arm is
// found in the one pass, as a flat contact's points all are. Angles are
// compared multiplied out (no division), squared about a point. `end` when
// no candidate has an arm.
static u32 catches_first(const NphPhysicsManifoldPoint *points, u32 from, u32 end, f32 level,
                         const ReduceAxis *ax)
{
    u32 level_best = end;
    f32 level_arm = 0.0F;
    u32 least = end;
    f32 least_height = 0.0F;
    f32 least_arm = 0.0F;
    for (u32 i = from; i < end; ++i) {
        const f32 arm = reduce_arm(ax, points[i].point);
        if (!(arm > 0.0F)) {
            continue;
        }
        f32 height = points[i].separation - level;
        if (!(height > 0.0F)) {
            if (arm > level_arm) {
                level_arm = arm;
                level_best = i;
            }
            continue;
        }
        height = ax->about_point ? height * height : height;
        if (least == end || height * least_arm < least_height * arm) {
            least = i;
            least_height = height;
            least_arm = arm;
        }
    }
    if (level_best != end || least == end) {
        return level_best;
    }
    const f32 tie = ax->about_point ? (1.0F + REDUCE_ANGLE_TIE) * (1.0F + REDUCE_ANGLE_TIE)
                                    : 1.0F + REDUCE_ANGLE_TIE;
    u32 best = least;
    f32 best_arm = least_arm;
    for (u32 i = from; i < end; ++i) {
        const f32 arm = reduce_arm(ax, points[i].point);
        if (arm > best_arm) {
            f32 height = points[i].separation - level;
            height = ax->about_point ? height * height : height;
            if (height * least_arm <= tie * least_height * arm) {
                best_arm = arm;
                best = i;
            }
        }
    }
    return best;
}

u32 nph_physics_manifold_reduce(NphPhysicsManifoldPoint *points, u32 count, NphVec3 normal)
{
    if (count <= NPH_PHYSICS_MANIFOLD_MAX_POINTS) {
        return count;
    }
    // Each point kept is swapped to the front: points[0, k) are kept and
    // points[k, count) are the candidates.
    //
    // Gregorius keeps the deepest point, the farthest from it, the one that
    // makes the largest triangle with both, and the one farthest outside
    // it: the most area for the points that touch. Points only near the
    // surface matter as much: as the shape turns on the points that touch,
    // the first a turn brings down is the one that catches it, and a point
    // left out sinks unseen. Across a mesh's seams, points centimetres
    // apart stand tenths of a millimetre apart from their triangles: a box
    // rocking on a terrain kept the farther, sank 0.35 mm at the nearer
    // every rock, and rocked for good. So after the deepest, each point is
    // the one that touches first as the shape turns about those kept: the
    // least catch angle, its height over its lever arm about the turn's
    // axis, and of angles as small, the longest arm. Heights are taken
    // above the touching points (above the deepest, when none touch), so
    // touching points have none, and a flat contact keeps Gregorius's
    // choice.
    //
    // 1. The deepest: of the points as deep, the one farthest from their
    // centre in the contact plane: a corner of the contact, not a point
    // inside it (a mesh's seams cross a box's face at its depth).
    f32 deepest = points[0].separation;
    NphVec3 centre = points[0].point;
    for (u32 i = 1U; i < count; ++i) {
        deepest = nph_f32_min(deepest, points[i].separation);
        centre = nph_vec3_add(centre, points[i].point);
    }
    centre = nph_vec3_scale(centre, 1.0F / (f32)count);
    {
        u32 best = 0U;
        f32 far = -1.0F;
        for (u32 i = 0U; i < count; ++i) {
            const f32 dd = plane_distance_sq(points[i].point, centre, normal);
            if (points[i].separation <= deepest + REDUCE_LEVEL && dd > far) {
                far = dd;
                best = i;
            }
        }
        swap_points(points, 0U, best);
    }
    // The level the others' heights are taken from: every touching point
    // is on it, already caught.
    const f32 level = nph_f32_max(deepest, 0.0F) + REDUCE_LEVEL;
    const NphVec3 p0 = points[0].point;
    // 2. The first to touch as the shape turns about the deepest: with other
    // points level, the farthest of them (the contact's line); else the
    // one it would tip onto. Points all at the deepest's place are one.
    ReduceAxis ax = {p0, nph_vec3_zero(), normal, true, 0.0F, REDUCE_AT_POINT * REDUCE_AT_POINT};
    const u32 second = catches_first(points, 1U, count, level, &ax);
    if (second == count) {
        return 1U;
    }
    swap_points(points, 1U, second);
    // 3. The first to touch as it turns about the line through the two, on
    // either side. Points along the line (a capsule or a box's edge lying
    // across a mesh's triangles) turn with it: the line's two ends are the
    // contact.
    const NphVec3 p1 = points[1].point;
    const NphVec3 line = nph_vec3_reject(nph_vec3_sub(p1, p0), normal);
    const f32 line_length = nph_vec3_length(line);
    ax.about_point = false;
    ax.across = nph_vec3_scale(nph_vec3_cross(normal, line), 1.0F / line_length);
    ax.min_arm = REDUCE_ON_LINE * line_length;
    const u32 third = catches_first(points, 2U, count, level, &ax);
    if (third == count) {
        return 2U;
    }
    swap_points(points, 2U, third);
    // 4. The first to touch on the line's other side; with none there, the
    // point farthest outside the three's triangle: it adds the most area.
    const NphVec3 p2 = points[2].point;
    ax.side = nph_vec3_dot(nph_vec3_sub(p2, p0), ax.across) < 0.0F ? 1.0F : -1.0F;
    u32 fourth = catches_first(points, 3U, count, level, &ax);
    if (fourth == count) {
        const f32 sign = signed_area(p0, p1, p2, normal) < 0.0F ? -1.0F : 1.0F;
        f32 best_out = -FLT_MAX;
        fourth = 3U;
        for (u32 i = 3U; i < count; ++i) {
            const NphVec3 p = points[i].point;
            const f32 out = -nph_f32_min3(sign * signed_area(p0, p1, p, normal),
                                          sign * signed_area(p1, p2, p, normal),
                                          sign * signed_area(p2, p0, p, normal));
            if (out > best_out) {
                best_out = out;
                fourth = i;
            }
        }
    }
    swap_points(points, 3U, fourth);
    return 4U;
}

// ---------------------------------------------------------------------------
// Spheres and capsules
// ---------------------------------------------------------------------------

// Two balls (sphere-sphere, or the nearest points of capsule cores): one
// point midway between the surfaces.
static void collide_balls(NphVec3 ca, f32 ra, NphVec3 cb, f32 rb, f32 speculative, u32 id,
                          NphVec3 fallback_normal, NphPhysicsManifold *out)
{
    const NphVec3 d = nph_vec3_sub(cb, ca);
    const f32 dist_sq = nph_vec3_length_sq(d);
    const f32 limit = ra + rb + speculative;
    if (dist_sq > limit * limit) {
        return;
    }
    const f32 dist = nph_f32_sqrt(dist_sq);
    const NphVec3 n = dist > 1e-9F ? nph_vec3_scale(d, 1.0F / dist) : fallback_normal;
    const NphVec3 sa = nph_vec3_mul_add(ca, n, ra);
    const NphVec3 sb = nph_vec3_mul_add(cb, n, -rb);
    out->normal = n;
    nph_physics_manifold_push(out, nph_vec3_scale(nph_vec3_add(sa, sb), 0.5F), dist - ra - rb, id);
}

static void capsule_segment(f32 half_height, NphPhysicsPose pose, NphVec3 *p, NphVec3 *q)
{
    const NphVec3 axis = nph_vec3_scale(nph_quat_axis_y(pose.rotation), half_height);
    *p = nph_vec3_sub(pose.position, axis);
    *q = nph_vec3_add(pose.position, axis);
}

static void collide_spheres(f32 ra, NphPhysicsPose pa, f32 rb, NphPhysicsPose pb, f32 speculative,
                            NphPhysicsManifold *out)
{
    collide_balls(pa.position, ra, pb.position, rb, speculative, 0U, nph_vec3_unit_y(), out);
}

static void collide_capsule_sphere(const NphPhysicsCapsule *a, NphPhysicsPose pa, f32 rb,
                                   NphPhysicsPose pb, f32 speculative, NphPhysicsManifold *out)
{
    NphVec3 p;
    NphVec3 q;
    capsule_segment(a->half_height, pa, &p, &q);
    f32 t;
    const NphVec3 c = nph_closest_point_on_segment(p, q, pb.position, &t);
    // A centre on the core: push out perpendicular to the axis.
    const NphVec3 fallback = nph_quat_axis_x(pa.rotation);
    collide_balls(c, a->radius, pb.position, rb, speculative, 0U, fallback, out);
}

static void collide_capsules(const NphPhysicsCapsule *a, NphPhysicsPose pa,
                             const NphPhysicsCapsule *b, NphPhysicsPose pb, f32 speculative,
                             NphPhysicsManifold *out)
{
    NphVec3 a0;
    NphVec3 a1;
    NphVec3 b0;
    NphVec3 b1;
    capsule_segment(a->half_height, pa, &a0, &a1);
    capsule_segment(b->half_height, pb, &b0, &b1);
    const f32 ra = a->radius;
    const f32 rb = b->radius;
    const NphPhysicsSegmentDistance sd = nph_physics_segment_distance(a0, a1, b0, b1);
    const f32 limit = ra + rb + speculative;
    if (sd.distance_squared > limit * limit) {
        return;
    }
    const NphVec3 da = nph_vec3_sub(a1, a0);
    const NphVec3 db = nph_vec3_sub(b1, b0);
    const f32 la_sq = nph_vec3_length_sq(da);
    const f32 lb_sq = nph_vec3_length_sq(db);
    // The normal: along the closest points, or, when the cores cross, the
    // common perpendicular, pointing from A's centre towards B's.
    const f32 dist = nph_f32_sqrt(sd.distance_squared);
    NphVec3 n;
    if (dist > 1e-6F) {
        n = nph_vec3_scale(nph_vec3_sub(sd.closest2, sd.closest1), 1.0F / dist);
    } else {
        NphVec3 c = nph_vec3_cross(da, db);
        if (nph_vec3_length_sq(c) <= 1e-12F * nph_f32_max(la_sq * lb_sq, 1e-12F)) {
            c = la_sq > 0.0F ? nph_vec3_perpendicular(nph_vec3_normalize(da)) : nph_vec3_unit_y();
        }
        n = nph_vec3_normalize_or(c, nph_vec3_unit_y());
        if (nph_vec3_dot(n, nph_vec3_sub(pb.position, pa.position)) < 0.0F) {
            n = nph_vec3_neg(n);
        }
    }
    // Nearly parallel cores that overlap along their length get two points,
    // at the ends of the overlap, so a capsule lying on another does not
    // balance on one point (Box2D's capsule manifold).
    const f32 cross_sq = nph_vec3_length_sq(nph_vec3_cross(da, db));
    if (la_sq > 1e-12F && lb_sq > 1e-12F && cross_sq <= 1e-4F * la_sq * lb_sq) {
        const f32 la = nph_f32_sqrt(la_sq);
        const NphVec3 ua = nph_vec3_scale(da, 1.0F / la);
        const f32 t0 = nph_vec3_dot(nph_vec3_sub(b0, a0), ua);
        const f32 t1 = nph_vec3_dot(nph_vec3_sub(b1, a0), ua);
        const f32 lo = nph_f32_max(0.0F, nph_f32_min(t0, t1));
        const f32 hi = nph_f32_min(la, nph_f32_max(t0, t1));
        if (hi - lo > NPH_PHYSICS_LINEAR_SLOP) {
            const f32 ts[2] = {lo, hi};
            out->normal = n;
            for (u32 k = 0U; k < 2U; ++k) {
                const NphVec3 pa_core = nph_vec3_mul_add(a0, ua, ts[k]);
                f32 tb;
                const NphVec3 pb_core = nph_closest_point_on_segment(b0, b1, pa_core, &tb);
                const f32 sep = nph_vec3_dot(nph_vec3_sub(pb_core, pa_core), n) - ra - rb;
                if (sep <= speculative) {
                    const NphVec3 sa = nph_vec3_mul_add(pa_core, n, ra);
                    const NphVec3 sb = nph_vec3_mul_add(pb_core, n, -rb);
                    nph_physics_manifold_push(out, nph_vec3_scale(nph_vec3_add(sa, sb), 0.5F), sep,
                                              k);
                }
            }
            if (out->point_count > 0U) {
                return;
            }
        }
    }
    const NphVec3 sa = nph_vec3_mul_add(sd.closest1, n, ra);
    const NphVec3 sb = nph_vec3_mul_add(sd.closest2, n, -rb);
    out->normal = n;
    nph_physics_manifold_push(out, nph_vec3_scale(nph_vec3_add(sa, sb), 0.5F), dist - ra - rb, 2U);
}

// ---------------------------------------------------------------------------
// Box and sphere: the clamp is exact (Ericson §5.1.3)
// ---------------------------------------------------------------------------

static void collide_box_sphere(const NphPhysicsBox *a, NphPhysicsPose pa, f32 rb, NphPhysicsPose pb,
                               f32 speculative, NphPhysicsManifold *out)
{
    const NphVec3 h = a->half_extents;
    const NphVec3 c = nph_physics_pose_inverse_transform_point(pa, pb.position);
    const NphVec3 q = nph_vec3_clamp(c, nph_vec3_neg(h), h);
    const NphVec3 d = nph_vec3_sub(c, q);
    const f32 dist_sq = nph_vec3_length_sq(d);
    NphVec3 n_local;
    NphVec3 surface_a;
    f32 sep;
    u32 id;
    if (dist_sq > 0.0F) {
        // Centre outside: the clamp is the closest point.
        const f32 limit = rb + speculative;
        if (dist_sq > limit * limit) {
            return;
        }
        const f32 dist = nph_f32_sqrt(dist_sq);
        n_local = nph_vec3_scale(d, 1.0F / dist);
        surface_a = q;
        sep = dist - rb;
        id = 0U;
    } else {
        // Centre inside: out through the nearest face.
        const NphVec3 depth = nph_vec3_sub(h, nph_vec3_abs(c));
        u32 axis = 0U;
        if (depth.y < depth.x && depth.y <= depth.z) {
            axis = 1U;
        } else if (depth.z < depth.x && depth.z < depth.y) {
            axis = 2U;
        }
        const f32 s = nph_vec3_at(c, axis) < 0.0F ? -1.0F : 1.0F;
        n_local = nph_vec3_zero();
        surface_a = c;
        if (axis == 0U) {
            n_local.x = s;
            surface_a.x = s * h.x;
        } else if (axis == 1U) {
            n_local.y = s;
            surface_a.y = s * h.y;
        } else {
            n_local.z = s;
            surface_a.z = s * h.z;
        }
        sep = -nph_vec3_at(depth, axis) - rb;
        id = 1U + axis;
    }
    const NphVec3 surface_b = nph_vec3_mul_add(c, n_local, -rb);
    const NphVec3 mid = nph_vec3_scale(nph_vec3_add(surface_a, surface_b), 0.5F);
    out->normal = nph_physics_pose_transform_vector(pa, n_local);
    nph_physics_manifold_push(out, nph_physics_pose_transform_point(pa, mid), sep, id);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

bool nph_physics_collide_supported(NphPhysicsShapeType a, NphPhysicsShapeType b)
{
    return nph_physics_shape_type_is_convex(a) && nph_physics_shape_type_is_convex(b);
}

static NphHullView view_of(const NphPhysicsGeometry *g, NphBoxViewStorage *storage)
{
    if (g->type == NPH_PHYSICS_SHAPE_BOX) {
        return nph_physics_box_view(g->box.half_extents, storage);
    }
    return nph_physics_hull_view(g->hull);
}

// Pairs with a's type >= b's type.
static void collide_ordered(const NphPhysicsGeometry *a, NphPhysicsPose pa,
                            const NphPhysicsGeometry *b, NphPhysicsPose pb, f32 speculative,
                            NphPhysicsCollideCache *cache, NphPhysicsManifold *out)
{
    switch (a->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        collide_spheres(a->sphere.radius, pa, b->sphere.radius, pb, speculative, out);
        return;
    case NPH_PHYSICS_SHAPE_CAPSULE:
        if (b->type == NPH_PHYSICS_SHAPE_SPHERE) {
            collide_capsule_sphere(&a->capsule, pa, b->sphere.radius, pb, speculative, out);
        } else {
            collide_capsules(&a->capsule, pa, &b->capsule, pb, speculative, out);
        }
        return;
    case NPH_PHYSICS_SHAPE_BOX:
    case NPH_PHYSICS_SHAPE_HULL: {
        if (a->type == NPH_PHYSICS_SHAPE_BOX && b->type == NPH_PHYSICS_SHAPE_SPHERE) {
            collide_box_sphere(&a->box, pa, b->sphere.radius, pb, speculative, out);
            return;
        }
        if (a->type == NPH_PHYSICS_SHAPE_BOX && b->type == NPH_PHYSICS_SHAPE_BOX) {
            nph_physics_collide_boxes(a->box.half_extents, pa, b->box.half_extents, pb, speculative,
                                      cache, out);
            return;
        }
        NphBoxViewStorage sa;
        const NphHullView va = view_of(a, &sa);
        if (b->type == NPH_PHYSICS_SHAPE_SPHERE) {
            nph_physics_collide_hull_sphere(&va, pa, b->sphere.radius, pb, speculative, out);
        } else if (b->type == NPH_PHYSICS_SHAPE_CAPSULE) {
            nph_physics_collide_hull_capsule(&va, pa, b->capsule.half_height, b->capsule.radius, pb,
                                             speculative, out);
        } else {
            NphBoxViewStorage sb;
            const NphHullView vb = view_of(b, &sb);
            nph_physics_collide_hulls(&va, pa, &vb, pb, speculative, cache, out);
        }
        return;
    }
    case NPH_PHYSICS_SHAPE_MESH:        // nph_physics_collide_mesh
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD: // nph_physics_collide_heightfield
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
    NPH_ASSERT_MSG(false, "nph_physics_collide: unsupported pair %u, %u", (u32)a->type,
                   (u32)b->type);
}

void nph_physics_collide(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                         const NphPhysicsGeometry *b, NphPhysicsPose pose_b,
                         f32 speculative_distance, NphPhysicsCollideCache *cache,
                         NphPhysicsManifold *out)
{
    NPH_ASSERT(a != NULL && b != NULL && out != NULL);
    NPH_ASSERT(nph_physics_collide_supported(a->type, b->type));
    nph_physics_manifold_clear(out);
    const f32 speculative = nph_f32_max(speculative_distance, 0.0F);
    if ((u32)a->type >= (u32)b->type) {
        collide_ordered(a, pose_a, b, pose_b, speculative, cache, out);
        return;
    }
    // The other order: same points, the normal reversed.
    collide_ordered(b, pose_b, a, pose_a, speculative, cache, out);
    out->normal = nph_vec3_neg(out->normal);
}
