// SPDX-License-Identifier: Apache-2.0
// Continuous collision (world.h; docs/physics.md "Continuous collision";
// ADR-0048).
//
// A dynamic body that moved farther in a step than half the inner radius of
// its thinnest shape (its centre's motion, plus how far its farthest point
// turned) may have passed through something thin that the step's
// speculative contacts did not reach. After the solver, its shapes are swept
// over the step, the centre of mass along a line and the rotation by slerp,
// against the static shapes they may meet, and the body is set back to the
// first time any comes within a slop of one; the next step's speculative
// contacts stop it (Box2D v3's continuous collision; Catto, "Continuous
// Collision", GDC 2013). Bullets also sweep against kinematic and dynamic
// bodies, at their poses at the step's end.
//
// Times of impact come from conservative advancement on GJK distances with a
// bound on rotation (Mirtich, "Impulse-based Dynamic Simulation of Rigid
// Body Systems", 1996; Bullet's btContinuousConvexCollision): the gap
// closes no faster than the centre approaches along the closest points'
// normal plus the body's spin times its reach, so each step stops short of
// the contact. Three rules keep it from stopping what is not tunnelling:
//   - a shape already within reach of a target at the start (resting on it,
//     sliding along it) is swept as a small ball at its centroid instead, so
//     it is stopped only where its middle would pass through (Box2D's
//     fallback);
//   - a triangle is swept against only when the shape's centroid comes
//     towards its plane by more than a slop over the step, so a fast body
//     sliding across a mesh's seams is never stopped at one (Box2D's rule for
//     its chain segments);
//   - an impact the step's speculative contacts stopped, which leaves the
//     shape on the target at the step's end and never through it (shape_toi),
//     is not set back.
//
// A fast body is swept in the finalize pass, against static shapes, which
// that pass does not move; a bullet after it, against bodies at their final
// poses too. Each reads only those and writes only its own pose and boxes:
// the sweeps run in parallel and their results do not depend on the order.

#include <nepheline/core/assert.h>

#include "world_internal.h"

#include <float.h>

// The gap a time of impact leaves, metres: a slop, well within the
// speculative contacts' reach, which take over from there.
#define TOI_TARGET NPH_PHYSICS_LINEAR_SLOP
// Arrived when within this beyond the target.
#define TOI_TOLERANCE (0.25F * NPH_PHYSICS_LINEAR_SLOP)
#define TOI_STEPS     32U
// A shape that starts within reach of a target is swept as a ball at its
// centroid this fraction of its inner radius wide.
#define CORE_FRACTION 0.25F
// A triangle is swept against only when the shape's centroid comes towards
// its plane by more than this over the step.
#define TRIANGLE_APPROACH NPH_PHYSICS_LINEAR_SLOP

// The body's motion over the step, in a frame whose origin is its centre of
// mass at the start (and whose axes are the world's, or a surface's).
typedef struct Sweep {
    NphVec3 dc; // the centre's motion
    NphQuat q0; // the rotation at the start and at the end
    NphQuat q1;
    f32 spin;  // the rotation's angle times the body's reach
    f32 reach; // the farthest any of its shapes reaches from the centre
} Sweep;

static void sweep_at(const Sweep *s, f32 t, NphVec3 *centre, NphQuat *rotation)
{
    *centre = nph_vec3_scale(s->dc, t);
    *rotation = t <= 0.0F ? s->q0 : (t >= 1.0F ? s->q1 : nph_quat_slerp(s->q0, s->q1, t));
}

// One of the fast body's shapes: its proxy, the ball at its centroid, and
// its frame relative to the centre of mass (body axes).
typedef struct Mover {
    const PhysShape *shape;
    NphPhysicsProxy proxy;
    NphVec3 storage[NPH_PHYSICS_PROXY_STORAGE];
    NphPhysicsProxy core;
    NphVec3 core_point;
    NphVec3 local;
    NphQuat local_q;
} Mover;

static void mover_init(Mover *mv, const PhysBody *b, const PhysShape *s)
{
    mv->shape = s;
    mv->proxy = nph_physics_geometry_proxy(&s->geometry, mv->storage);
    mv->core_point =
        s->geometry.type == NPH_PHYSICS_SHAPE_HULL ? s->geometry.hull->centroid : nph_vec3_zero();
    mv->core.points = &mv->core_point;
    mv->core.count = 1U;
    mv->core.radius = CORE_FRACTION * nph_physics_geometry_inner_radius(&s->geometry);
    mv->local = nph_vec3_sub(s->pose.position, b->local_center);
    mv->local_q = s->pose.rotation;
}

// The mover's shape frame at t (the sweep's frame).
static NphPhysicsPose mover_at(const Mover *mv, const Sweep *s, f32 t)
{
    NphVec3 c;
    NphQuat q;
    sweep_at(s, t, &c, &q);
    return nph_physics_pose(nph_vec3_add(c, nph_quat_rotate(q, mv->local)),
                            nph_quat_mul(q, mv->local_q));
}

// The first fraction of the step, before `limit`, at which proxy `m` of the
// mover comes within the target gap of proxy `t` at `pose_t` (still):
// conservative advancement, with the axis between them there (from t
// towards m; nph_physics_contact_axis) in `axis`. 0 when they start within
// it; negative for no impact. GJK runs in a frame at the moving shape, as
// the casts' does.
static f32 advance(const Mover *mv, const NphPhysicsProxy *m, const Sweep *s,
                   const NphPhysicsProxy *t, NphPhysicsPose pose_t, f32 limit, NphVec3 *axis)
{
    const f32 radius = m->radius + t->radius;
    NphPhysicsSimplexCache cache = {0U, {0U}, {0U}};
    f32 alpha = 0.0F;
    for (u32 step = 0U; step < TOI_STEPS; ++step) {
        const NphPhysicsPose pm = mover_at(mv, s, alpha);
        const NphPhysicsPose at_m = nph_physics_pose(nph_vec3_zero(), pm.rotation);
        const NphPhysicsPose at_t =
            nph_physics_pose(nph_vec3_sub(pose_t.position, pm.position), pose_t.rotation);
        NphPhysicsDistance d;
        nph_physics_distance(t, at_t, m, at_m, false, &cache, &d);
        const f32 separation = d.distance - radius;
        if (!(d.distance > 0.0F)) {
            return alpha;
        }
        *axis = d.normal;
        if (separation <= TOI_TARGET + TOI_TOLERANCE) {
            *axis = nph_physics_contact_axis(m, at_m, t, at_t, &cache, d.normal);
            return alpha;
        }
        // The gap closes no faster than the centre approaches along the
        // normal and the farthest point turns.
        const f32 closing = -nph_vec3_dot(s->dc, d.normal) + s->spin;
        if (!(closing > 0.0F)) {
            return -1.0F;
        }
        alpha += (separation - TOI_TARGET) / closing;
        if (!(alpha < limit)) {
            return -1.0F;
        }
    }
    return alpha; // a grazing approach on its way: short of the contact
}

// The gap between proxy `m` of the mover at fraction `alpha` and proxy `t`
// along `axis` (nph_physics_proxy_gap).
static f32 gap_at(const Mover *mv, const NphPhysicsProxy *m, const Sweep *s,
                  const NphPhysicsProxy *t, NphPhysicsPose pose_t, NphVec3 axis, f32 alpha)
{
    const NphPhysicsPose pm = mover_at(mv, s, alpha);
    return nph_physics_proxy_gap(
        m, nph_physics_pose(nph_vec3_zero(), pm.rotation), t,
        nph_physics_pose(nph_vec3_sub(pose_t.position, pm.position), pose_t.rotation), axis);
}

// The shape's time of impact with a still target: its own, or where it
// starts within reach, its core's (negative for none: a core that starts
// within reach too is the contacts' to push out).
//
// An impact the step's contacts already stopped is none. They reach as far
// as a pair can close in a step (phys_pair_speculative), so a body falling
// fast onto a floor ends the step on it, and set back to where it came
// within a slop it would land twice. Along the impact's axis the gap is
// linear in the fraction for a body that does not turn, and dips below the
// smaller of its ends by at most the arc's bow (r angle^2 / 8) for one that
// does: within a slop of the target at both ends with that taken off, the
// shape never went through it. One that went through a thin wall, into a
// block, or turned through either lies past.
static f32 shape_toi(const Mover *mv, const Sweep *s, const NphPhysicsProxy *t,
                     NphPhysicsPose pose_t, f32 limit)
{
    NphVec3 axis = nph_vec3_unit_y();
    const f32 f = advance(mv, &mv->proxy, s, t, pose_t, limit, &axis);
    if (f > 0.0F) {
        const f32 angle = s->reach > 0.0F ? s->spin / s->reach : 0.0F;
        const f32 bow = 0.125F * s->spin * angle;
        const f32 low = nph_f32_min(gap_at(mv, &mv->proxy, s, t, pose_t, axis, 0.0F),
                                    gap_at(mv, &mv->proxy, s, t, pose_t, axis, 1.0F));
        return low - bow >= -NPH_PHYSICS_LINEAR_SLOP ? -1.0F : f;
    }
    if (f < 0.0F) {
        return f;
    }
    const f32 g = advance(mv, &mv->core, s, t, pose_t, limit, &axis);
    return g > 0.0F ? g : -1.0F;
}

// ---------------------------------------------------------------------------
// Against a surface's triangles
// ---------------------------------------------------------------------------

typedef struct SurfaceSweep {
    const Mover *mover;
    Sweep sweep; // in a frame with the surface's axes, at the mover's centre
    bool two_sided;
    f32 best;
} SurfaceSweep;

static bool sweep_triangle(void *user, u32 t, const NphVec3 v[3], u32 active_edges)
{
    NPH_UNUSED(t);
    NPH_UNUSED(active_edges);
    SurfaceSweep *q = (SurfaceSweep *)user;
    NphVec3 n = nph_vec3_cross(nph_vec3_sub(v[1], v[0]), nph_vec3_sub(v[2], v[0]));
    const f32 len = nph_vec3_length(n);
    if (!(len > 0.0F)) {
        return true;
    }
    n = nph_vec3_scale(n, 1.0F / len);
    // The centroid's height over the plane at the start and the end.
    const NphVec3 c0 =
        nph_physics_pose_transform_point(mover_at(q->mover, &q->sweep, 0.0F), q->mover->core_point);
    const NphVec3 c1 =
        nph_physics_pose_transform_point(mover_at(q->mover, &q->sweep, 1.0F), q->mover->core_point);
    f32 h0 = nph_vec3_dot(n, nph_vec3_sub(c0, v[0]));
    f32 h1 = nph_vec3_dot(n, nph_vec3_sub(c1, v[0]));
    if (q->two_sided && h0 < 0.0F) {
        h0 = -h0;
        h1 = -h1;
    }
    // Behind a one-sided triangle, or not coming towards it (sliding along
    // its plane, or leaving it): not a tunnelling.
    if (h0 < -NPH_PHYSICS_LINEAR_SLOP || !(h0 - h1 > TRIANGLE_APPROACH)) {
        return true;
    }
    const NphPhysicsProxy tri = {v, 3U, 0.0F};
    const f32 f = shape_toi(q->mover, &q->sweep, &tri, nph_physics_pose_identity(), q->best);
    if (f >= 0.0F && f < q->best) {
        q->best = f;
    }
    return true;
}

// The mover against a static surface `s` on body `sb`, whose sweep `world`
// is in the world's axes at the mover's starting centre `start` (64 bits).
static f32 surface_toi(const Mover *mv, const Sweep *world, NphDVec3 start, const PhysBody *sb,
                       const PhysShape *s, f32 limit)
{
    // The surface's frame relative to the mover's start, in 64 bits.
    const NphQuat qs = nph_quat_mul(sb->rotation, s->pose.rotation);
    const NphDVec3 ps =
        nph_dvec3_add_vec3(sb->origin, nph_quat_rotate(sb->rotation, s->pose.position));
    const NphDVec3 origin = nph_physics_drotate_inverse(qs, nph_dvec3_sub(start, ps));
    const NphQuat inv = nph_quat_conjugate(qs);
    SurfaceSweep q;
    q.mover = mv;
    q.sweep.dc = nph_quat_rotate(inv, world->dc);
    q.sweep.q0 = nph_quat_mul(inv, world->q0);
    q.sweep.q1 = nph_quat_mul(inv, world->q1);
    q.sweep.spin = world->spin;
    q.sweep.reach = world->reach;
    q.best = limit;
    // The box the body's reach sweeps, in the surface's frame.
    const NphVec3 at = nph_dvec3_to_vec3(origin);
    const f32 grow = world->reach + TOI_TARGET + TOI_TOLERANCE +
                     2.4e-7F * nph_vec3_max_component(nph_vec3_abs(at));
    const NphVec3 end = nph_vec3_add(at, q.sweep.dc);
    const NphAABB box = {nph_vec3_sub(nph_vec3_min(at, end), nph_vec3_splat(grow)),
                         nph_vec3_add(nph_vec3_max(at, end), nph_vec3_splat(grow))};
    if (s->geometry.type == NPH_PHYSICS_SHAPE_MESH) {
        q.two_sided = (s->geometry.mesh->flags & NPH_PHYSICS_MESH_TWO_SIDED) != 0U;
        nph_physics_mesh_visit_box(s->geometry.mesh, box, origin, sweep_triangle, &q);
    } else {
        q.two_sided = false;
        nph_physics_heightfield_visit_box(s->geometry.heightfield, box, origin, sweep_triangle, &q);
    }
    return q.best < limit ? q.best : -1.0F;
}

// ---------------------------------------------------------------------------
// A fast body
// ---------------------------------------------------------------------------

typedef struct BodySweep {
    const NphPhysicsWorld *w;
    const PhysBody *b;
    u32 slot;
    NphDVec3 start; // the centre of mass at the step's start
    Sweep sweep;    // in the world's axes, at `start`
    f32 best;       // the first impact so far (1: none)
} BodySweep;

static void sweep_target(BodySweep *bs, const PhysBody *tb, const PhysShape *ts)
{
    const NphPhysicsWorld *w = bs->w;
    // Not its own shapes, nor a bullet's (bullets do not sweep against
    // each other), nor a body a joint keeps it from touching.
    if (ts->body == bs->slot ||
        (tb->type != NPH_PHYSICS_BODY_STATIC && (tb->flags & PHYS_BODY_BULLET) != 0U) ||
        !phys_joints_allow_contact(w, bs->slot, ts->body)) {
        return;
    }
    const bool surface = ts->geometry.type == NPH_PHYSICS_SHAPE_MESH ||
                         ts->geometry.type == NPH_PHYSICS_SHAPE_HEIGHTFIELD;
    NphVec3 storage[NPH_PHYSICS_PROXY_STORAGE];
    NphPhysicsProxy tp = {storage, 1U, 0.0F};
    NphPhysicsPose pose_t = nph_physics_pose_identity();
    if (!surface) {
        tp = nph_physics_geometry_proxy(&ts->geometry, storage);
        pose_t = phys_shape_pose_relative(tb, ts, bs->start);
    }
    for (u32 s = bs->b->shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
        const PhysShape *ms = &w->shapes.data[s];
        if (!nph_physics_filter_should_collide(ms->filter, ts->filter)) {
            continue;
        }
        Mover mv;
        mover_init(&mv, bs->b, ms);
        const f32 f = surface ? surface_toi(&mv, &bs->sweep, bs->start, tb, ts, bs->best)
                              : shape_toi(&mv, &bs->sweep, &tp, pose_t, bs->best);
        if (f >= 0.0F && f < bs->best) {
            bs->best = f;
        }
    }
}

static bool sweep_leaf(u32 proxy, u64 leaf, void *context)
{
    NPH_UNUSED(proxy);
    BodySweep *bs = (BodySweep *)context;
    const PhysShape *ts = &bs->w->shapes.data[(u32)leaf];
    sweep_target(bs, &bs->w->bodies.data[ts->body], ts);
    return true;
}

void phys_continuous_body(NphPhysicsWorld *w, u32 solver_index, bool bullets)
{
    const PhysSolverParams *p = &w->solver_params.data[solver_index];
    const PhysSolverBody *sb = &w->solver_bodies.data[solver_index];
    PhysBody *b = &w->bodies.data[p->body];
    if ((b->flags & PHYS_BODY_FAST) == 0U || ((b->flags & PHYS_BODY_BULLET) != 0U) != bullets) {
        return;
    }
    b->flags &= ~(u32)PHYS_BODY_FAST;
    b->flags |= PHYS_BODY_SWEPT;
    BodySweep bs;
    bs.w = w;
    bs.b = b;
    bs.slot = p->body;
    bs.start = nph_dvec3_add_vec3(b->center, nph_vec3_neg(sb->dp));
    bs.sweep.dc = sb->dp;
    bs.sweep.q0 = p->q0;
    bs.sweep.q1 = b->rotation;
    bs.sweep.reach = b->max_extent;
    bs.sweep.spin = nph_quat_angle(sb->dq) * b->max_extent;
    bs.best = 1.0F;
    // Targets in a category one of the body's shapes accepts (each pair
    // then checks both filters).
    u64 mask = 0U;
    for (u32 s = b->shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
        mask |= w->shapes.data[s].filter.mask;
    }
    // The box the body's reach sweeps over the step.
    const f32 grow = b->max_extent + TOI_TARGET + TOI_TOLERANCE;
    const NphAABB rel = {nph_vec3_sub(nph_vec3_min(nph_vec3_zero(), sb->dp), nph_vec3_splat(grow)),
                         nph_vec3_add(nph_vec3_max(nph_vec3_zero(), sb->dp), nph_vec3_splat(grow))};
    const NphAABB box = phys_world_box(bs.start, rel);
    nph_aabb_tree_query(&w->trees[PHYS_TREE_STATIC], box, mask, sweep_leaf, &bs);
    if (bullets) {
        nph_aabb_tree_query(&w->trees[PHYS_TREE_KINEMATIC], box, mask, sweep_leaf, &bs);
        nph_aabb_tree_query(&w->trees[PHYS_TREE_DYNAMIC], box, mask, sweep_leaf, &bs);
    }
    if (!(bs.best < 1.0F)) {
        return;
    }
    // Back to the first impact: the next step's contacts take it from there.
    NphVec3 c;
    NphQuat q;
    sweep_at(&bs.sweep, bs.best, &c, &q);
    b->center = nph_dvec3_add_vec3(bs.start, c);
    b->rotation = nph_quat_normalize(q);
    b->origin =
        nph_dvec3_add_vec3(b->center, nph_vec3_neg(nph_quat_rotate(b->rotation, b->local_center)));
    b->flags |= PHYS_BODY_IMPACT;
    phys_body_refresh_boxes(w, b);
}
