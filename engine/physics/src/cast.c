// SPDX-License-Identifier: Apache-2.0
// Ray casts, shape casts, and overlaps against one geometry (collide.h
// "Casts and overlaps"; docs/physics.md "Queries"; ADR-0047). Surfaces'
// triangles are cast in collide_mesh.c, with the contact rules.

#include <nepheline/core/assert.h>
#include <nepheline/physics/collide.h>

#include "physics_internal.h"

#include <float.h>

// Conservative advancement's steps before it settles for where it is.
#define ADVANCE_STEPS 32U
// A core distance, metres, at which GJK's normal is well defined: four
// orders above its rounding for shapes a metre across.
#define WELL_APART 1e-4F
// How far back along the motion, metres, a polytope pair's normal is taken
// where their cores touch: far enough for GJK's to be good to 1e-4, near
// enough to be the contact's own features.
#define NORMAL_BACKOFF 1e-3F

// ---------------------------------------------------------------------------
// Rays against convex geometry, in its own frame
// ---------------------------------------------------------------------------

// Where the segment o + t d (t in [0, 1]) enters the ball of radius r at the
// origin from outside. The closest point of the line to the centre, then
// Pythagoras along the line: the quadratic's formula would cancel digits
// for an origin far away compared to r (Box2D's b2RayCastCircle).
static bool ray_ball(NphVec3 o, NphVec3 d, f32 r, f32 *out_t)
{
    const f32 len = nph_vec3_length(d);
    if (!(len > 0.0F)) {
        return false;
    }
    const NphVec3 u = nph_vec3_scale(d, 1.0F / len);
    const f32 tc = -nph_vec3_dot(o, u);
    const f32 h_sq = r * r - nph_vec3_length_sq(nph_vec3_mul_add(o, u, tc));
    if (h_sq < 0.0F) {
        return false;
    }
    const f32 t = tc - nph_f32_sqrt(h_sq);
    // An entry behind the origin: the ball is behind it, or holds it.
    if (t < 0.0F || t > len) {
        return false;
    }
    *out_t = t / len;
    return true;
}

// A capsule's segment from (0, -half_height, 0) to (0, half_height, 0),
// radius r. The capsule is the union of a cylinder and two balls, and a
// segment from outside enters a union where it enters the first of its
// parts: the cylinder's side between the ends (the closest approach in the
// XZ plane, then Pythagoras, as for the ball), or a ball (whose equators
// hold the cylinder's flat ends, so those never come first).
static bool ray_capsule(NphVec3 o, NphVec3 d, f32 half_height, f32 r, f32 *out_t, NphVec3 *out_n)
{
    const f32 yc = nph_f32_clamp(o.y, -half_height, half_height);
    if (o.x * o.x + (o.y - yc) * (o.y - yc) + o.z * o.z < r * r) {
        return false; // from inside
    }
    f32 best = FLT_MAX;
    NphVec3 n = nph_vec3_unit_y();
    const f32 len = nph_f32_sqrt(d.x * d.x + d.z * d.z);
    if (len > 0.0F) {
        const f32 ux = d.x / len;
        const f32 uz = d.z / len;
        const f32 tc = -(o.x * ux + o.z * uz);
        const f32 cx = o.x + tc * ux;
        const f32 cz = o.z + tc * uz;
        const f32 h_sq = r * r - (cx * cx + cz * cz);
        if (h_sq < 0.0F) {
            return false; // the cylinder missed, and the capsule inside it
        }
        const f32 t = (tc - nph_f32_sqrt(h_sq)) / len;
        if (t >= 0.0F && t <= 1.0F && nph_f32_abs(o.y + t * d.y) <= half_height) {
            best = t;
            n = nph_vec3_normalize_or(nph_vec3(o.x + t * d.x, 0.0F, o.z + t * d.z),
                                      nph_vec3_unit_x());
        }
    } else if (o.x * o.x + o.z * o.z > r * r) {
        return false; // along the axis, outside its column
    }
    for (u32 k = 0U; k < 2U; ++k) {
        const NphVec3 c = nph_vec3(0.0F, k == 0U ? -half_height : half_height, 0.0F);
        f32 t = 0.0F;
        if (ray_ball(nph_vec3_sub(o, c), d, r, &t) && t < best) {
            best = t;
            n = nph_vec3_normalize_or(nph_vec3_sub(nph_vec3_mul_add(o, d, t), c),
                                      nph_vec3_unit_y());
        }
    }
    if (!(best <= 1.0F)) {
        return false;
    }
    *out_t = best;
    *out_n = n;
    return true;
}

// The slab test, keeping the face the segment enters by: the last slab it
// enters (Kay and Kajiya, "Ray Tracing Complex Scenes", SIGGRAPH 1986).
static bool ray_box(NphVec3 o, NphVec3 d, NphVec3 half, f32 *out_t, NphVec3 *out_n)
{
    f32 enter = -FLT_MAX;
    f32 leave = 1.0F;
    u32 axis = 3U;
    f32 side = 0.0F;
    for (u32 i = 0U; i < 3U; ++i) {
        const f32 oi = nph_vec3_at(o, i);
        const f32 di = nph_vec3_at(d, i);
        const f32 hi = nph_vec3_at(half, i);
        if (di == 0.0F) {
            if (oi < -hi || oi > hi) {
                return false; // parallel to the slab, outside it
            }
            continue;
        }
        // The face it approaches, and the opposite.
        const f32 near = di > 0.0F ? -hi : hi;
        const f32 t0 = (near - oi) / di;
        const f32 t1 = (-near - oi) / di;
        if (t0 > enter) {
            enter = t0;
            axis = i;
            side = di > 0.0F ? -1.0F : 1.0F;
        }
        leave = nph_f32_min(leave, t1);
        if (enter > leave) {
            return false;
        }
    }
    if (axis == 3U || enter < 0.0F) {
        return false; // from inside
    }
    *out_t = enter;
    *out_n = nph_vec3(axis == 0U ? side : 0.0F, axis == 1U ? side : 0.0F, axis == 2U ? side : 0.0F);
    return true;
}

// The segment clipped by the face planes (Haines 1991): it enters where it
// crosses the last plane it approaches from outside, and leaves at the
// first it crosses from inside.
static bool ray_hull(const NphPhysicsHull *h, NphVec3 o, NphVec3 d, f32 *out_t, NphVec3 *out_n)
{
    const NphPlane *planes = nph_physics_hull_planes(h);
    f32 enter = -FLT_MAX;
    f32 leave = 1.0F;
    u32 face = NPH_U32_MAX;
    for (u32 f = 0U; f < h->face_count; ++f) {
        const NphPlane p = planes[f];
        const f32 dist = nph_vec3_dot(p.normal, o) + p.d; // > 0 outside the face
        const f32 den = nph_vec3_dot(p.normal, d);
        if (den == 0.0F) {
            if (dist > 0.0F) {
                return false; // parallel to the face, outside it
            }
            continue;
        }
        const f32 t = -dist / den;
        if (den < 0.0F) {
            if (t > enter) {
                enter = t;
                face = f;
            }
        } else {
            leave = nph_f32_min(leave, t);
        }
        if (enter > leave) {
            return false;
        }
    }
    if (face == NPH_U32_MAX || enter < 0.0F) {
        return false; // from inside
    }
    *out_t = enter;
    *out_n = planes[face].normal;
    return true;
}

bool nph_physics_raycast_local(const NphPhysicsGeometry *g, NphVec3 origin, NphVec3 delta,
                               NphPhysicsCastHit *out)
{
    if (!(nph_vec3_length_sq(delta) > 0.0F)) {
        return false;
    }
    f32 t = 0.0F;
    NphVec3 n = nph_vec3_unit_y();
    u32 triangle = NPH_PHYSICS_NO_TRIANGLE;
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        if (!ray_ball(origin, delta, g->sphere.radius, &t)) {
            return false;
        }
        n = nph_vec3_normalize_or(nph_vec3_mul_add(origin, delta, t),
                                  nph_vec3_neg(nph_vec3_normalize(delta)));
        break;
    case NPH_PHYSICS_SHAPE_CAPSULE:
        if (!ray_capsule(origin, delta, g->capsule.half_height, g->capsule.radius, &t, &n)) {
            return false;
        }
        break;
    case NPH_PHYSICS_SHAPE_BOX:
        if (!ray_box(origin, delta, g->box.half_extents, &t, &n)) {
            return false;
        }
        break;
    case NPH_PHYSICS_SHAPE_HULL:
        if (!ray_hull(g->hull, origin, delta, &t, &n)) {
            return false;
        }
        break;
    case NPH_PHYSICS_SHAPE_MESH:
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD: {
        NphPhysicsMeshHit hit;
        const bool found =
            g->type == NPH_PHYSICS_SHAPE_MESH
                ? nph_physics_mesh_raycast(g->mesh, origin, delta, &hit)
                : nph_physics_heightfield_raycast(g->heightfield, origin, delta, &hit);
        if (!found) {
            return false;
        }
        t = hit.fraction;
        n = hit.normal;
        triangle = hit.original;
        break;
    }
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        return false;
    }
    out->point = nph_vec3_mul_add(origin, delta, t);
    out->normal = n;
    out->fraction = t;
    out->triangle = triangle;
    return true;
}

bool nph_physics_raycast(const NphPhysicsGeometry *g, NphPhysicsPose pose, NphVec3 origin,
                         NphVec3 delta, NphPhysicsCastHit *out)
{
    NPH_ASSERT(g != NULL && out != NULL);
    NphPhysicsCastHit local;
    if (!nph_physics_raycast_local(g, nph_physics_pose_inverse_transform_point(pose, origin),
                                   nph_physics_pose_inverse_transform_vector(pose, delta),
                                   &local)) {
        return false;
    }
    out->point = nph_vec3_mul_add(origin, delta, local.fraction);
    out->normal = nph_physics_pose_transform_vector(pose, local.normal);
    out->fraction = local.fraction;
    out->triangle = local.triangle;
    return true;
}

// ---------------------------------------------------------------------------
// Conservative advancement
// ---------------------------------------------------------------------------

f32 nph_physics_proxy_gap(const NphPhysicsProxy *m, NphPhysicsPose pose_m, const NphPhysicsProxy *t,
                          NphPhysicsPose pose_t, NphVec3 n)
{
    const u32 im =
        nph_physics_proxy_support(m, nph_quat_rotate_inverse(pose_m.rotation, nph_vec3_neg(n)));
    const u32 it = nph_physics_proxy_support(t, nph_quat_rotate_inverse(pose_t.rotation, n));
    const f32 low =
        nph_vec3_dot(nph_physics_pose_transform_point(pose_m, m->points[im]), n) - m->radius;
    const f32 high =
        nph_vec3_dot(nph_physics_pose_transform_point(pose_t, t->points[it]), n) + t->radius;
    return low - high;
}

// GJK between the cores with the mover at fraction `alpha`, in a frame at
// the mover's place: near contact its coordinates are the shapes' own size,
// where 32 bits resolve far finer than at the cast's start. `base` is the
// target's place relative to the mover's start.
static void distance_at(const NphPhysicsProxy *m, NphQuat rotation_m, const NphPhysicsProxy *t,
                        NphQuat rotation_t, NphVec3 base, NphVec3 delta, f32 alpha,
                        NphPhysicsSimplexCache *cache, NphPhysicsDistance *out)
{
    nph_physics_distance(t, nph_physics_pose(nph_vec3_mul_add(base, delta, -alpha), rotation_t), m,
                         nph_physics_pose(nph_vec3_zero(), rotation_m), false, cache, out);
}

bool nph_physics_cast_arrive(const NphPhysicsProxy *m, NphPhysicsPose pose_m, NphVec3 delta,
                             const NphPhysicsProxy *t, NphPhysicsPose pose_t, f32 limit,
                             NphPhysicsArrival *out)
{
    // GJK runs on the cores, their radii taken off its distance.
    const f32 radius = m->radius + t->radius;
    const NphVec3 base = nph_vec3_sub(pose_t.position, pose_m.position);
    NphPhysicsSimplexCache cache = {0U, {0U}, {0U}};
    f32 alpha = 0.0F;
    out->normal = nph_vec3_unit_y();
    out->point = pose_m.position;
    out->have_point = false;
    out->approach = 0.0F;
    out->base = base;
    out->features = cache;
    for (u32 step = 0U; step < ADVANCE_STEPS; ++step) {
        NphPhysicsDistance d;
        distance_at(m, pose_m.rotation, t, pose_t.rotation, base, delta, alpha, &cache, &d);
        // The closest features: where the cores touch, GJK's simplex holds
        // the origin and tells none, so the last step's stand.
        if (d.simplex_count < 4U) {
            out->features = cache;
        }
        const NphVec3 at = nph_vec3_mul_add(pose_m.position, delta, alpha);
        out->fraction = alpha;
        out->mover = nph_physics_pose(nph_vec3_zero(), pose_m.rotation);
        out->target = nph_physics_pose(nph_vec3_mul_add(base, delta, -alpha), pose_t.rotation);
        out->known = d.distance > WELL_APART;
        if (d.distance > 0.0F) {
            // On the target's surface. Where the cores touch (a step that
            // landed on a flat contact) the last one stands: the target
            // does not move.
            out->normal = d.normal;
            out->point = nph_vec3_add(at, nph_vec3_mul_add(d.point_a, d.normal, t->radius));
            out->have_point = true;
        }
        const f32 separation = d.distance - radius;
        if (!(d.distance > 0.0F && separation > NPH_PHYSICS_CAST_TOUCH)) {
            return true;
        }
        // The plane between the closest points holds the two apart until
        // the mover has crossed the gap along its normal: each step ends
        // short of the contact.
        const f32 approach = -nph_vec3_dot(delta, d.normal);
        if (!(approach > 0.0F)) {
            return false;
        }
        alpha += separation / approach;
        if (!(alpha <= limit)) {
            return false;
        }
        out->approach = approach;
    }
    // A grazing approach still on its way: where the last step began,
    // short of any contact, with GJK's normal there.
    out->known = true;
    return true;
}

bool nph_physics_cast_normal_back(const NphPhysicsProxy *m, NphPhysicsPose pose_m, NphVec3 delta,
                                  const NphPhysicsProxy *t, NphPhysicsPose pose_t,
                                  const NphPhysicsArrival *a, NphVec3 *normal)
{
    if (!(a->fraction > 0.0F) || !(a->approach > 0.0F)) {
        return false; // touching at the start: no way back
    }
    const f32 back = nph_f32_max(a->fraction - NORMAL_BACKOFF / a->approach, 0.0F);
    NphPhysicsDistance d;
    distance_at(m, pose_m.rotation, t, pose_t.rotation, a->base, delta, back, NULL, &d);
    if (!(d.distance > WELL_APART)) {
        return false;
    }
    *normal = d.normal;
    return true;
}

f32 nph_physics_cast_finish(const NphPhysicsProxy *m, const NphPhysicsProxy *t,
                            const NphPhysicsArrival *a, NphVec3 delta, NphVec3 normal)
{
    const f32 approach = -nph_vec3_dot(delta, normal);
    if (!(approach > NPH_PHYSICS_CAST_SLIDE * nph_vec3_length(delta))) {
        return -1.0F;
    }
    // The last of the gap along the contact's normal: across a face it
    // lands on the contact itself, and it never passes it.
    const f32 g = nph_physics_proxy_gap(m, a->mover, t, a->target, normal);
    return g > 0.0F ? nph_f32_min(a->fraction + g / approach, 1.0F) : a->fraction;
}

// ---------------------------------------------------------------------------
// Shape casts
// ---------------------------------------------------------------------------

static u32 deepest_point(const NphPhysicsManifold *m)
{
    u32 deepest = 0U;
    for (u32 i = 1U; i < m->point_count; ++i) {
        if (m->points[i].separation < m->points[deepest].separation) {
            deepest = i;
        }
    }
    return deepest;
}

// The contact's normal from the closest features GJK's simplex `c` ended
// on (as Box2D's separation functions take their axis from it): a face of
// the target against a point of the mover, a point against a face of the
// mover, or two edges, each exactly; oriented as `hint` (GJK's own).
// False for a point or an edge against a point, whose normals form a cone
// that GJK's estimate is in.
static bool feature_normal(const NphPhysicsProxy *m, NphPhysicsPose pose_m,
                           const NphPhysicsProxy *t, NphPhysicsPose pose_t,
                           const NphPhysicsSimplexCache *c, NphVec3 hint, NphVec3 *out)
{
    u32 ti[4];
    u32 mi[4];
    u32 nt = 0U;
    u32 nm = 0U;
    for (u32 k = 0U; k < c->count && k < 4U; ++k) {
        bool seen_t = false;
        bool seen_m = false;
        for (u32 j = 0U; j < nt; ++j) {
            seen_t = seen_t || ti[j] == c->index_a[k];
        }
        for (u32 j = 0U; j < nm; ++j) {
            seen_m = seen_m || mi[j] == c->index_b[k];
        }
        if (!seen_t && c->index_a[k] < t->count) {
            ti[nt++] = c->index_a[k];
        }
        if (!seen_m && c->index_b[k] < m->count) {
            mi[nm++] = c->index_b[k];
        }
    }
    NphVec3 tp[4];
    NphVec3 mp[4];
    for (u32 j = 0U; j < nt; ++j) {
        tp[j] = nph_physics_pose_transform_point(pose_t, t->points[ti[j]]);
    }
    for (u32 j = 0U; j < nm; ++j) {
        mp[j] = nph_physics_pose_transform_point(pose_m, m->points[mi[j]]);
    }
    NphVec3 n;
    if (nt >= 3U && nm == 1U) {
        n = nph_vec3_cross(nph_vec3_sub(tp[1], tp[0]), nph_vec3_sub(tp[2], tp[0]));
    } else if (nm >= 3U && nt == 1U) {
        n = nph_vec3_cross(nph_vec3_sub(mp[1], mp[0]), nph_vec3_sub(mp[2], mp[0]));
    } else if (nt == 2U && nm == 2U) {
        n = nph_vec3_cross(nph_vec3_sub(tp[1], tp[0]), nph_vec3_sub(mp[1], mp[0]));
    } else {
        return false;
    }
    const f32 len = nph_vec3_length(n);
    if (!(len > 1e-12F)) {
        return false; // parallel edges, or a sliver
    }
    n = nph_vec3_scale(n, 1.0F / len);
    *out = nph_vec3_dot(n, hint) < 0.0F ? nph_vec3_neg(n) : n;
    return true;
}

NphVec3 nph_physics_contact_axis(const NphPhysicsProxy *m, NphPhysicsPose pose_m,
                                 const NphPhysicsProxy *t, NphPhysicsPose pose_t,
                                 const NphPhysicsSimplexCache *features, NphVec3 hint)
{
    NphVec3 n;
    if (feature_normal(m, pose_m, t, pose_t, features, hint, &n) &&
        nph_physics_proxy_gap(m, pose_m, t, pose_t, n) >=
            nph_physics_proxy_gap(m, pose_m, t, pose_t, hint)) {
        return n;
    }
    return hint;
}

bool nph_physics_cast_convex(const NphPhysicsGeometry *a, NphPhysicsPose pose_a, NphVec3 delta,
                             const NphPhysicsGeometry *b, NphPhysicsPose pose_b, f32 limit,
                             NphPhysicsCastHit *out)
{
    NphVec3 storage_a[NPH_PHYSICS_PROXY_STORAGE];
    NphVec3 storage_b[NPH_PHYSICS_PROXY_STORAGE];
    const NphPhysicsProxy pa = nph_physics_geometry_proxy(a, storage_a);
    const NphPhysicsProxy pb = nph_physics_geometry_proxy(b, storage_b);
    NphPhysicsArrival arr;
    if (!nph_physics_cast_arrive(&pa, pose_a, delta, &pb, pose_b, limit, &arr)) {
        return false;
    }
    // Where the cores are apart at contact (a round shape's), GJK's normal
    // stands.
    NphVec3 normal = arr.normal;
    NphVec3 point = arr.point;
    if (!arr.known && arr.have_point) {
        // Polytopes, whose cores touch. Of GJK's normal (where it last had
        // one), the features' exact one, and GJK's a millimetre back, the
        // one along which the two are farthest apart: the axis test's own
        // measure.
        normal = nph_physics_contact_axis(&pa, arr.mover, &pb, arr.target, &arr.features, normal);
        const f32 best = nph_physics_proxy_gap(&pa, arr.mover, &pb, arr.target, normal);
        NphVec3 n;
        if (nph_physics_cast_normal_back(&pa, pose_a, delta, &pb, pose_b, &arr, &n) &&
            nph_physics_proxy_gap(&pa, arr.mover, &pb, arr.target, n) > best) {
            normal = n;
        }
    } else if (!arr.known) {
        // Touching at the start, cores and all: the narrow phase's normal,
        // defined at any depth.
        NphPhysicsManifold m;
        nph_physics_collide(b, pose_b, a,
                            nph_physics_pose(nph_vec3_mul_add(pose_a.position, delta, arr.fraction),
                                             pose_a.rotation),
                            NPH_PHYSICS_CAST_REACH, NULL, &m);
        if (m.point_count > 0U) {
            const NphPhysicsManifoldPoint p = m.points[deepest_point(&m)];
            normal = m.normal; // from b towards a
            // The middle of the contact, moved onto b's side.
            point = nph_vec3_mul_add(p.point, m.normal, -0.5F * p.separation);
        } else {
            return false; // no normal to judge the motion by
        }
    }
    const f32 f = nph_physics_cast_finish(&pa, &pb, &arr, delta, normal);
    if (f < 0.0F || f > limit) {
        return false;
    }
    out->point = point;
    out->normal = normal;
    out->fraction = f;
    out->triangle = NPH_PHYSICS_NO_TRIANGLE;
    return true;
}

static bool is_surface(NphPhysicsShapeType type)
{
    return type == NPH_PHYSICS_SHAPE_MESH || type == NPH_PHYSICS_SHAPE_HEIGHTFIELD;
}

bool nph_physics_shape_cast(const NphPhysicsGeometry *a, NphPhysicsPose pose_a, NphVec3 translation,
                            const NphPhysicsGeometry *b, NphPhysicsPose pose_b,
                            NphPhysicsCastHit *out)
{
    NPH_ASSERT(a != NULL && b != NULL && out != NULL);
    NPH_ASSERT(nph_physics_shape_type_is_convex(a->type));
    if (!(nph_vec3_length_sq(translation) > 0.0F)) {
        return false;
    }
    if (!is_surface(b->type)) {
        return nph_physics_cast_convex(a, pose_a, translation, b, pose_b, 1.0F, out);
    }
    NphQuat rotation;
    NphDVec3 position;
    nph_physics_surface_place(pose_a, pose_b, &rotation, &position);
    const NphVec3 delta = nph_quat_rotate_inverse(pose_b.rotation, translation);
    NphPhysicsCastHit local;
    const bool hit =
        b->type == NPH_PHYSICS_SHAPE_MESH
            ? nph_physics_cast_mesh_local(a, rotation, position, delta, b->mesh, 1.0F, &local)
            : nph_physics_cast_heightfield_local(a, rotation, position, delta, b->heightfield, 1.0F,
                                                 &local);
    if (!hit) {
        return false;
    }
    out->point = nph_vec3_add(pose_a.position, nph_quat_rotate(pose_b.rotation, local.point));
    out->normal = nph_quat_rotate(pose_b.rotation, local.normal);
    out->fraction = local.fraction;
    out->triangle = local.triangle;
    return true;
}

// ---------------------------------------------------------------------------
// Overlaps
// ---------------------------------------------------------------------------

bool nph_physics_overlap(const NphPhysicsGeometry *a, NphPhysicsPose pose_a,
                         const NphPhysicsGeometry *b, NphPhysicsPose pose_b)
{
    NPH_ASSERT(a != NULL && b != NULL);
    NPH_ASSERT(nph_physics_shape_type_is_convex(a->type));
    if (is_surface(b->type)) {
        NphQuat rotation;
        NphDVec3 position;
        nph_physics_surface_place(pose_a, pose_b, &rotation, &position);
        return b->type == NPH_PHYSICS_SHAPE_MESH
                   ? nph_physics_overlap_mesh_local(a, rotation, position, b->mesh)
                   : nph_physics_overlap_heightfield_local(a, rotation, position, b->heightfield);
    }
    NphVec3 storage_a[NPH_PHYSICS_PROXY_STORAGE];
    NphVec3 storage_b[NPH_PHYSICS_PROXY_STORAGE];
    const NphPhysicsProxy pa = nph_physics_geometry_proxy(a, storage_a);
    const NphPhysicsProxy pb = nph_physics_geometry_proxy(b, storage_b);
    NphPhysicsDistance d;
    nph_physics_distance(&pa, pose_a, &pb, pose_b, true, NULL, &d);
    return d.distance <= 0.0F;
}
