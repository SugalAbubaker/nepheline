// SPDX-License-Identifier: Apache-2.0
// The solver's step: solver bodies, constraint groups by colour, the stages
// run in lockstep on the job system, integration, and the finalize pass
// (docs/physics.md "The solver"; ADR-0043).
//
// The stages are one list, unrolled for the step: prepare bodies, prepare
// joints and contacts, then per substep integrate velocities, warm start,
// solve, integrate positions, relax (each constraint stage once per colour,
// colours in order, a colour's joints and contacts together), then
// restitution, store, finalize. Each participating thread
// starts at its own share of the current stage's blocks and claims blocks
// one at a time, forward and then backward from there, so a thread keeps
// working the same blocks, and their data stays in its cache, substep after
// substep; the calling thread then claims whatever is left, so a helper the
// job system has not started, or the OS has preempted, holds back only the
// blocks it took. Whichever thread finishes a stage's last block publishes
// the next stage. Helpers spin between stages instead of parking, because a
// step has a hundred or more stages of a few microseconds each and a wake
// costs more than a stage (Box2D v3's solver stages, on our job system). As
// many threads take part as the step has work for (`solver_participants`).

#include <nepheline/core/assert.h>
#include <nepheline/core/profile.h>
#include <nepheline/math/kernels.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "world_internal.h"

#include <string.h>

#define PHYS_SYNC_DONE NPH_U32_MAX

// Pauses a thread spins through waiting for the next stage before it also
// yields: about ten stages' worth. Past that, a participant is likely
// preempted (more threads than cores, or another program) and holding a
// block, and yielding gives it a core instead of every spinner burning its
// slice.
#define PHYS_SPINS_BEFORE_YIELD 1024U

const PhysSolverKernels *phys_solver_kernels_best(void)
{
    return nph_simd_detect_level() >= NPH_SIMD_LEVEL_AVX2 ? &g_phys_solver_avx2
                                                          : &g_phys_solver_sse4;
}

PhysSoftness phys_make_soft(f32 hertz, f32 zeta, f32 h)
{
    PhysSoftness s;
    if (hertz <= 0.0F) {
        s.bias_rate = 0.0F;
        s.mass_scale = 1.0F;
        s.impulse_scale = 0.0F;
        return s;
    }
    const f32 omega = 2.0F * 3.14159265F * hertz;
    const f32 a1 = 2.0F * zeta + h * omega;
    const f32 a2 = h * omega * a1;
    const f32 a3 = 1.0F / (1.0F + a2);
    s.bias_rate = omega / a1;
    s.mass_scale = a2 * a3;
    s.impulse_scale = a3;
    return s;
}

typedef struct StepContext {
    NphPhysicsWorld *w;
    PhysStage *stages;
    u32 stage_count;
    u32 participants; // the calling thread and its helpers
    NphAtomicU32 *claims;
    NphAtomicU32 sync;   // the stage being worked, plus one; PHYS_SYNC_DONE at the end
    NphAtomicU32 joined; // participants that have started
    f32 dt;
    f32 h;
    f32 inv_h;
    f32 push_max;
    f32 slop; // TGS: the overlap its Baumgarte bias leaves alone
    f32 restitution_threshold;
    PhysSoftness contact_soft;
    PhysSoftness static_soft;
    PhysJointStep joint_step;
    u32 dummy; // the solver body of static and sleeping-kinematic lanes
} StepContext;

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

static NphVec3 vec3_without(NphVec3 v, u32 axis)
{
    if (axis == 0U) {
        v.x = 0.0F;
    } else if (axis == 1U) {
        v.y = 0.0F;
    } else {
        v.z = 0.0F;
    }
    return v;
}

static NphMat3 lock_inertia(NphMat3 m, u32 locks)
{
    // A locked rotation axis takes no angular impulse: its row and column of
    // the inverse inertia are zero.
    for (u32 axis = 0U; axis < 3U; ++axis) {
        if ((locks & ((u32)NPH_PHYSICS_LOCK_ANGULAR_X << axis)) == 0U) {
            continue;
        }
        m.c[axis] = nph_vec3_zero();
        for (u32 k = 0U; k < 3U; ++k) {
            m.c[k] = vec3_without(m.c[k], axis);
        }
    }
    return m;
}

// The gyroscopic term w x I w vanishes for an inertia that is a multiple
// of the identity (a sphere, a cube); within a millionth of the trace it is
// a millionth of the precession of a genuinely uneven body, far below what
// a step can show.
static bool inertia_is_isotropic(NphMat3 m)
{
    const f32 tol = 1e-6F * (nph_f32_abs(m.c[0].x) + nph_f32_abs(m.c[1].y) + nph_f32_abs(m.c[2].z));
    return nph_f32_abs(m.c[1].y - m.c[0].x) <= tol && nph_f32_abs(m.c[2].z - m.c[0].x) <= tol &&
           nph_f32_abs(m.c[0].y) <= tol && nph_f32_abs(m.c[0].z) <= tol &&
           nph_f32_abs(m.c[1].z) <= tol;
}

static void prepare_body(StepContext *ctx, u32 i)
{
    NphPhysicsWorld *w = ctx->w;
    const u32 slot = w->awake_bodies.data[i];
    const PhysBody *b = &w->bodies.data[slot];
    PhysSolverBody *sb = &w->solver_bodies.data[i];
    PhysSolverParams *p = &w->solver_params.data[i];
    memset(sb, 0, sizeof *sb);
    sb->v = b->linear_velocity;
    sb->w = b->angular_velocity;
    sb->dq = nph_quat_identity();
    memset(p, 0, sizeof *p);
    p->body = slot;
    p->q0 = b->rotation;
    p->locks = b->locks;
    const f32 max_rotation = 0.25F * 3.14159265F * ctx->inv_h;
    p->max_angular_speed_sq = max_rotation * max_rotation;
    p->max_linear_speed_sq = w->desc.max_linear_speed * w->desc.max_linear_speed;
    if (b->type == NPH_PHYSICS_BODY_DYNAMIC) {
        p->flags = PHYS_PARAM_DYNAMIC;
        if ((b->flags & PHYS_BODY_GYROSCOPIC) != 0U && !inertia_is_isotropic(b->inertia)) {
            p->flags |= PHYS_PARAM_GYROSCOPIC;
        }
        p->linear_accel = nph_vec3_add(nph_vec3_scale(w->desc.gravity, b->gravity_scale),
                                       nph_vec3_scale(b->force, b->inv_mass));
        const NphMat3 r = nph_mat3_from_quat(b->rotation);
        p->inv_inertia = lock_inertia(
            nph_mat3_mul(nph_mat3_mul(r, b->inv_inertia), nph_mat3_transpose(r)), b->locks);
        p->angular_accel = nph_mat3_mul_vec3(p->inv_inertia, b->torque);
        p->inertia_local = b->inertia;
        // Damping as v / (1 + h c), the Pade fit of exp(-c h) (Box2D).
        p->linear_damping = 1.0F / (1.0F + ctx->h * b->linear_damping);
        p->angular_damping = 1.0F / (1.0F + ctx->h * b->angular_damping);
    }
}

// The implicit gyroscopic step in body space (Catto, "Numerical Methods",
// GDC 2015): one Newton iteration on I w2 + h w2 x I w2 = I w1, which
// conserves angular momentum's magnitude well and never gains energy, where
// the explicit torque w x I w would. It needs the body's current rotation
// `q`: the same iteration in the world with the step's starting inertia
// lost ten times the energy in the intermediate-axis test.
static NphVec3 gyroscopic(NphVec3 w_world, NphQuat q, NphMat3 inertia, f32 h)
{
    const NphVec3 w = nph_quat_rotate_inverse(q, w_world);
    const NphVec3 iw = nph_mat3_mul_vec3(inertia, w);
    const NphVec3 f = nph_vec3_scale(nph_vec3_cross(w, iw), h);
    // The Jacobian I + h (skew(w) I - skew(I w)), by columns: skew(w) I
    // has columns w x I_k, and skew(I w) has columns (I w) x e_k.
    const NphVec3 e[3] = {nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(0.0F, 1.0F, 0.0F),
                          nph_vec3(0.0F, 0.0F, 1.0F)};
    NphVec3 j[3];
    for (u32 k = 0U; k < 3U; ++k) {
        j[k] = nph_vec3_mul_add(
            inertia.c[k], nph_vec3_sub(nph_vec3_cross(w, inertia.c[k]), nph_vec3_cross(iw, e[k])),
            h);
    }
    // Cramer's rule for J x = f.
    const NphVec3 c12 = nph_vec3_cross(j[1], j[2]);
    const f32 det = nph_vec3_dot(j[0], c12);
    if (!(det > 0.0F)) {
        return w_world; // J is I plus a small term: positive unless the step is absurd
    }
    const f32 inv_det = 1.0F / det;
    const NphVec3 x = nph_vec3(nph_vec3_dot(f, c12) * inv_det,
                               nph_vec3_dot(j[0], nph_vec3_cross(f, j[2])) * inv_det,
                               nph_vec3_dot(j[0], nph_vec3_cross(j[1], f)) * inv_det);
    return nph_quat_rotate(q, nph_vec3_sub(w, x));
}

static NphVec3 lock_linear(NphVec3 v, u32 locks)
{
    if ((locks & NPH_PHYSICS_LOCK_LINEAR_X) != 0U) {
        v.x = 0.0F;
    }
    if ((locks & NPH_PHYSICS_LOCK_LINEAR_Y) != 0U) {
        v.y = 0.0F;
    }
    if ((locks & NPH_PHYSICS_LOCK_LINEAR_Z) != 0U) {
        v.z = 0.0F;
    }
    return v;
}

static NphVec3 lock_angular(NphVec3 w, u32 locks)
{
    if ((locks & NPH_PHYSICS_LOCK_ANGULAR_X) != 0U) {
        w.x = 0.0F;
    }
    if ((locks & NPH_PHYSICS_LOCK_ANGULAR_Y) != 0U) {
        w.y = 0.0F;
    }
    if ((locks & NPH_PHYSICS_LOCK_ANGULAR_Z) != 0U) {
        w.z = 0.0F;
    }
    return w;
}

static void integrate_velocity(const StepContext *ctx, u32 i)
{
    const NphPhysicsWorld *w = ctx->w;
    const PhysSolverParams *p = &w->solver_params.data[i];
    if ((p->flags & PHYS_PARAM_DYNAMIC) == 0U) {
        return; // kinematic: moves by its velocity only
    }
    PhysSolverBody *sb = &w->solver_bodies.data[i];
    const f32 h = ctx->h;
    NphVec3 v = nph_vec3_scale(nph_vec3_mul_add(sb->v, p->linear_accel, h), p->linear_damping);
    NphVec3 av = nph_vec3_scale(nph_vec3_mul_add(sb->w, p->angular_accel, h), p->angular_damping);
    if ((p->flags & PHYS_PARAM_GYROSCOPIC) != 0U) {
        // The rotation now: both factors are unit (nph_quat_integrate
        // normalizes), so the product is unit to rounding and needs no
        // normalizing.
        av = gyroscopic(av, nph_quat_mul(sb->dq, p->q0), p->inertia_local, h);
    }
    const f32 v_sq = nph_vec3_length_sq(v);
    if (v_sq > p->max_linear_speed_sq) {
        v = nph_vec3_scale(v, nph_f32_sqrt(p->max_linear_speed_sq / v_sq));
    }
    const f32 w_sq = nph_vec3_length_sq(av);
    if (w_sq > p->max_angular_speed_sq) {
        av = nph_vec3_scale(av, nph_f32_sqrt(p->max_angular_speed_sq / w_sq));
    }
    sb->v = lock_linear(v, p->locks);
    sb->w = lock_angular(av, p->locks);
}

static void integrate_position(const StepContext *ctx, u32 i)
{
    const NphPhysicsWorld *w = ctx->w;
    const PhysSolverParams *p = &w->solver_params.data[i];
    PhysSolverBody *sb = &w->solver_bodies.data[i];
    sb->v = lock_linear(sb->v, p->locks);
    sb->w = lock_angular(sb->w, p->locks);
    sb->dp = nph_vec3_mul_add(sb->dp, sb->v, ctx->h);
    sb->dq = nph_quat_integrate(sb->dq, sb->w, ctx->h);
}

// XPBD: the positions at the substep's start kept, then the bodies moved
// by their velocities (gravity, forces, and damping applied first).
static void xpbd_predict(const StepContext *ctx, u32 i)
{
    const NphPhysicsWorld *w = ctx->w;
    const PhysSolverBody *sb = &w->solver_bodies.data[i];
    PhysSolverBody *prev = &w->solver_prev.data[i];
    prev->dp = sb->dp;
    prev->dq = sb->dq;
    integrate_velocity(ctx, i);
    integrate_position(ctx, i);
}

// XPBD: the velocities the corrected positions imply (a kinematic body's
// are its own).
static void xpbd_derive(const StepContext *ctx, u32 i)
{
    const NphPhysicsWorld *w = ctx->w;
    const PhysSolverParams *p = &w->solver_params.data[i];
    if ((p->flags & PHYS_PARAM_DYNAMIC) == 0U) {
        return;
    }
    PhysSolverBody *sb = &w->solver_bodies.data[i];
    const PhysSolverBody *prev = &w->solver_prev.data[i];
    sb->v = lock_linear(nph_vec3_scale(nph_vec3_sub(sb->dp, prev->dp), ctx->inv_h), p->locks);
    NphQuat d = nph_quat_mul(sb->dq, nph_quat_conjugate(prev->dq));
    if (d.w < 0.0F) {
        d = nph_quat_neg(d);
    }
    sb->w = lock_angular(nph_vec3_scale(nph_vec3(d.x, d.y, d.z), 2.0F * ctx->inv_h), p->locks);
}

static void finalize_body(const StepContext *ctx, u32 i)
{
    NphPhysicsWorld *w = ctx->w;
    const PhysSolverParams *p = &w->solver_params.data[i];
    const PhysSolverBody *sb = &w->solver_bodies.data[i];
    PhysBody *b = &w->bodies.data[p->body];
    b->linear_velocity = sb->v;
    b->angular_velocity = sb->w;
    b->rotation = nph_quat_normalize(nph_quat_mul(sb->dq, p->q0));
    b->center = nph_dvec3_add_vec3(b->center, sb->dp);
    b->origin =
        nph_dvec3_add_vec3(b->center, nph_vec3_neg(nph_quat_rotate(b->rotation, b->local_center)));
    b->force = nph_vec3_zero();
    b->torque = nph_vec3_zero();
    // Still enough to sleep: the speed of the fastest point (Box2D's metric).
    const f32 speed = nph_f32_max(nph_vec3_length(sb->v), b->max_extent * nph_vec3_length(sb->w));
    if ((b->flags & PHYS_BODY_ENABLE_SLEEP) == 0U || speed > b->sleep_threshold) {
        b->sleep_time = 0.0F;
    } else {
        b->sleep_time += ctx->dt;
    }
    // Shapes that left their proxies get new fattened boxes; the trees grow
    // after the stages, in a fixed order.
    for (u32 s = b->shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
        PhysShape *shape = &w->shapes.data[s];
        shape->aabb = phys_shape_world_aabb(b, shape);
        const NphAABB fat = shape->fat_aabb;
        const NphAABB box = shape->aabb;
        const bool inside = fat.min.x <= box.min.x && fat.min.y <= box.min.y &&
                            fat.min.z <= box.min.z && box.max.x <= fat.max.x &&
                            box.max.y <= fat.max.y && box.max.z <= fat.max.z;
        if (!inside) {
            shape->fat_aabb = nph_aabb_grow(box, NPH_PHYSICS_AABB_MARGIN);
            shape->flags |= PHYS_SHAPE_ENLARGED;
            b->flags |= PHYS_BODY_ENLARGED;
        }
    }
}

// ---------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------

static NphVec3 lanes3(const f32 a[3][PHYS_LANES], u32 l)
{
    return nph_vec3(a[0][l], a[1][l], a[2][l]);
}

static void set_lanes3(f32 a[3][PHYS_LANES], u32 l, NphVec3 v)
{
    a[0][l] = v.x;
    a[1][l] = v.y;
    a[2][l] = v.z;
}

static void set_lanes_mat3(f32 a[9][PHYS_LANES], u32 l, NphMat3 m)
{
    set_lanes3(&a[0], l, m.c[0]);
    set_lanes3(&a[3], l, m.c[1]);
    set_lanes3(&a[6], l, m.c[2]);
}

static void prepare_group(const StepContext *ctx, u32 index)
{
    NphPhysicsWorld *w = ctx->w;
    PhysContactLanes *g = &w->groups.data[index];
    PhysPointLanes *pts = &w->points.data[g->first_point];
    // The header was laid out when the groups were built; everything else
    // starts at zero, which is what an empty lane or point must hold.
    const u32 lane_count = g->lane_count;
    const u32 point_count = g->point_count;
    const u32 first_point = g->first_point;
    u32 contacts[PHYS_LANES];
    memcpy(contacts, g->contact, sizeof contacts);
    memset(g, 0, sizeof *g);
    memset(pts, 0, (size_t)point_count * sizeof(PhysPointLanes));
    g->lane_count = lane_count;
    g->point_count = point_count;
    g->first_point = first_point;
    for (u32 l = 0U; l < PHYS_LANES; ++l) {
        g->contact[l] = contacts[l];
        g->body_a[l] = ctx->dummy;
        g->body_b[l] = ctx->dummy;
    }
    // The lanes' inputs, then the arithmetic eight lanes at a time.
    PhysPrepareLanes in;
    memset(&in, 0, sizeof in);
    for (u32 l = 0U; l < lane_count; ++l) {
        const PhysContact *c = &w->contacts.data[contacts[l]];
        const PhysBody *a = &w->bodies.data[c->body_a];
        const PhysBody *b = &w->bodies.data[c->body_b];
        const bool dyn_a = a->type == NPH_PHYSICS_BODY_DYNAMIC;
        const bool dyn_b = b->type == NPH_PHYSICS_BODY_DYNAMIC;
        const u32 ia = a->awake_slot != PHYS_NULL ? a->awake_slot : ctx->dummy;
        const u32 ib = b->awake_slot != PHYS_NULL ? b->awake_slot : ctx->dummy;
        g->body_a[l] = ia;
        g->body_b[l] = ib;
        g->write_mask_a |= dyn_a ? 1U << l : 0U;
        g->write_mask_b |= dyn_b ? 1U << l : 0U;
        const f32 ma = dyn_a ? a->inv_mass : 0.0F;
        const f32 mb = dyn_b ? b->inv_mass : 0.0F;
        const NphMat3 ii_a = dyn_a ? w->solver_params.data[ia].inv_inertia : nph_mat3_zero();
        const NphMat3 ii_b = dyn_b ? w->solver_params.data[ib].inv_inertia : nph_mat3_zero();
        g->inv_mass_a[l] = ma;
        g->inv_mass_b[l] = mb;
        const NphVec3 n = c->normal;
        const NphVec3 t1 = nph_vec3_perpendicular(n);
        const NphVec3 t2 = nph_vec3_cross(n, t1);
        set_lanes3(g->normal, l, n);
        set_lanes3(g->tangent1, l, t1);
        set_lanes3(g->tangent2, l, t2);
        g->friction[l] = c->friction;
        g->restitution[l] = c->restitution;
        const PhysSoftness soft = (!dyn_a || !dyn_b) ? ctx->static_soft : ctx->contact_soft;
        g->bias_rate[l] = soft.bias_rate;
        g->mass_scale[l] = soft.mass_scale;
        g->impulse_scale[l] = soft.impulse_scale;
        set_lanes_mat3(in.inv_inertia_a, l, ii_a);
        set_lanes_mat3(in.inv_inertia_b, l, ii_b);
        for (u32 k = 0U; k < c->point_count; ++k) {
            const PhysContactPoint *cp = &c->points[k];
            PhysPointLanes *p = &pts[k];
            set_lanes3(p->anchor_a, l, cp->anchor_a);
            set_lanes3(p->anchor_b, l, cp->anchor_b);
            p->adjusted_separation[l] = cp->separation;
            p->normal_impulse[l] = cp->normal_impulse;
            set_lanes3(in.tangent_impulse[k], l, cp->tangent_impulse);
            in.active[k][l] = 1.0F;
        }
    }
    w->kernels->prepare(w->solver_bodies.data, g, pts, &in);
}

static void store_group(const StepContext *ctx, u32 index)
{
    NphPhysicsWorld *w = ctx->w;
    const PhysContactLanes *g = &w->groups.data[index];
    const PhysPointLanes *pts = &w->points.data[g->first_point];
    for (u32 l = 0U; l < g->lane_count; ++l) {
        PhysContact *c = &w->contacts.data[g->contact[l]];
        const NphVec3 t1 = lanes3(g->tangent1, l);
        const NphVec3 t2 = lanes3(g->tangent2, l);
        for (u32 k = 0U; k < c->point_count; ++k) {
            PhysContactPoint *cp = &c->points[k];
            const PhysPointLanes *p = &pts[k];
            cp->normal_impulse = p->normal_impulse[l];
            cp->max_normal_impulse = p->max_normal_impulse[l];
            cp->relative_velocity = p->relative_velocity[l];
            cp->tangent_impulse = nph_vec3_add(nph_vec3_scale(t1, p->tangent_impulse[0][l]),
                                               nph_vec3_scale(t2, p->tangent_impulse[1][l]));
        }
    }
}

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

static void run_joint_block(StepContext *ctx, const PhysStage *s, u32 block)
{
    NphPhysicsWorld *w = ctx->w;
    const u32 first = s->joint_first + block * s->joint_block_size;
    const u32 last = nph_min_u32(first + s->joint_block_size, s->joint_first + s->joint_count);
    PhysSolverBody *bodies = w->solver_bodies.data;
    for (u32 i = first; i < last; ++i) {
        PhysJoint *j = &w->joints.data[w->step_joints.data[i]];
        switch ((PhysStageType)s->type) {
        case PHYS_STAGE_PREPARE_CONTACTS:
            phys_joint_prepare(w, &ctx->joint_step, j);
            break;
        case PHYS_STAGE_WARM_START:
            phys_joint_warm_start(bodies, j);
            break;
        case PHYS_STAGE_SOLVE:
        case PHYS_STAGE_RELAX:
            phys_joint_solve(bodies, &ctx->joint_step, j, s->type == PHYS_STAGE_SOLVE);
            break;
        case PHYS_STAGE_XPBD_POSITIONS:
            phys_joint_xpbd_positions(bodies, &ctx->joint_step, j);
            break;
        case PHYS_STAGE_XPBD_VELOCITIES:
            phys_joint_xpbd_velocities(bodies, &ctx->joint_step, j);
            break;
        case PHYS_STAGE_XPBD_PREDICT:
        case PHYS_STAGE_XPBD_DERIVE:
        case PHYS_STAGE_PREPARE_BODIES:
        case PHYS_STAGE_INTEGRATE_VELOCITIES:
        case PHYS_STAGE_INTEGRATE_POSITIONS:
        case PHYS_STAGE_RESTITUTION:
        case PHYS_STAGE_STORE:
        case PHYS_STAGE_FINALIZE:
        default:
            NPH_ASSERT_MSG(false, "physics: stage %u has no joints", s->type);
            break;
        }
    }
}

static void run_block(StepContext *ctx, const PhysStage *s, u32 block)
{
    if (block < s->joint_blocks) {
        run_joint_block(ctx, s, block);
        return;
    }
    block -= s->joint_blocks;
    NphPhysicsWorld *w = ctx->w;
    const u32 first = s->first + block * s->block_size;
    const u32 last = nph_min_u32(first + s->block_size, s->first + s->count);
    const PhysSolverKernels *k = w->kernels;
    PhysSolverBody *bodies = w->solver_bodies.data;
    for (u32 i = first; i < last; ++i) {
        switch ((PhysStageType)s->type) {
        case PHYS_STAGE_PREPARE_BODIES:
            prepare_body(ctx, i);
            break;
        case PHYS_STAGE_PREPARE_CONTACTS:
            prepare_group(ctx, i);
            break;
        case PHYS_STAGE_INTEGRATE_VELOCITIES:
            integrate_velocity(ctx, i);
            break;
        case PHYS_STAGE_WARM_START: {
            PhysContactLanes *g = &w->groups.data[i];
            k->warm_start(bodies, g, &w->points.data[g->first_point]);
            break;
        }
        case PHYS_STAGE_SOLVE:
        case PHYS_STAGE_RELAX: {
            PhysContactLanes *g = &w->groups.data[i];
            k->solve(bodies, g, &w->points.data[g->first_point], ctx->inv_h, ctx->push_max,
                     ctx->slop, s->type == PHYS_STAGE_SOLVE);
            break;
        }
        case PHYS_STAGE_INTEGRATE_POSITIONS:
            integrate_position(ctx, i);
            break;
        case PHYS_STAGE_RESTITUTION: {
            PhysContactLanes *g = &w->groups.data[i];
            k->restitution(bodies, g, &w->points.data[g->first_point], ctx->restitution_threshold);
            break;
        }
        case PHYS_STAGE_STORE:
            store_group(ctx, i);
            break;
        case PHYS_STAGE_FINALIZE:
            finalize_body(ctx, i);
            break;
        case PHYS_STAGE_XPBD_PREDICT:
            xpbd_predict(ctx, i);
            break;
        case PHYS_STAGE_XPBD_POSITIONS: {
            PhysContactLanes *g = &w->groups.data[i];
            k->xpbd_positions(bodies, w->solver_prev.data, g, &w->points.data[g->first_point]);
            break;
        }
        case PHYS_STAGE_XPBD_DERIVE:
            xpbd_derive(ctx, i);
            break;
        case PHYS_STAGE_XPBD_VELOCITIES: {
            PhysContactLanes *g = &w->groups.data[i];
            k->xpbd_velocities(bodies, g, &w->points.data[g->first_point], ctx->inv_h,
                               ctx->restitution_threshold);
            break;
        }
        default:
            NPH_ASSERT_MSG(false, "physics: unknown solver stage %u", s->type);
            break;
        }
    }
}

// As many threads as the step's work pays for. Its cost on one thread is
// about 600 ns per point row (eight lanes), 290 ns per body, and 1200 ns
// per joint over a step of four substeps, and a thread pays for itself with
// about 450 ns of work in each stage it joins (measured on the bench scenes,
// docs/physics.md "Threads"); the ratio holds across machines, which scale
// both alike.
static u32 solver_participants(const NphPhysicsWorld *w, u32 available)
{
    if (w->forced_participants != 0U) {
        return nph_min_u32(w->forced_participants, available);
    }
    u64 colors = 0U; // constraint stages per pass
    for (u32 c = 0U; c <= PHYS_GRAPH_COLORS; ++c) {
        colors += (w->color_group_count[c] > 0U || w->color_joint_count[c] > 0U) ? 1U : 0U;
    }
    const u64 stages = 4U + (u64)w->desc.substeps * (2U + 3U * colors) + colors;
    const u64 work_ns = (u64)w->points.len * 600U + (u64)w->awake_bodies.len * 290U +
                        (u64)w->step_joints.len * 1200U;
    return (u32)nph_min_u64(1U + work_ns / (stages * 450U), available);
}

static bool claim_block(NphAtomicU32 *claim)
{
    // Read before writing: a block taken already costs a shared read, not
    // the line's ownership.
    return nph_atomic_load_u32(claim, NPH_MEMORY_ORDER_RELAXED) == 0U &&
           nph_atomic_exchange_u32(claim, 1U, NPH_MEMORY_ORDER_RELAXED) == 0U;
}

// Runs blocks of stage `index` as participant `p`: forward from its share
// while the blocks are free (on into the share of a participant that has
// not started), then backward; participant 0 then takes whatever is left.
// The thread that completes the stage publishes the next one.
static void execute_stage(StepContext *ctx, u32 index, u32 p)
{
    PhysStage *s = &ctx->stages[index];
    NphAtomicU32 *claims = ctx->claims + s->claim_first;
    const u32 blocks = s->block_count;
    const u32 start = (u32)(((u64)p * blocks) / ctx->participants);
    u32 completed = 0U;
    for (u32 b = start; b < blocks && claim_block(&claims[b]); ++b) {
        run_block(ctx, s, b);
        completed += 1U;
    }
    for (u32 b = start; b > 0U && claim_block(&claims[b - 1U]); --b) {
        run_block(ctx, s, b - 1U);
        completed += 1U;
    }
    if (p == 0U) {
        for (u32 b = 0U; b < blocks; ++b) {
            if (claim_block(&claims[b])) {
                run_block(ctx, s, b);
                completed += 1U;
            }
        }
    }
    if (completed == 0U) {
        return;
    }
    // The last increment reads every earlier one (a release sequence), so
    // the publication below carries all the stage's writes.
    const u32 done =
        nph_atomic_fetch_add_u32(&s->done, completed, NPH_MEMORY_ORDER_ACQ_REL) + completed;
    if (done == blocks) {
        const u32 next = index + 1U < ctx->stage_count ? index + 2U : PHYS_SYNC_DONE;
        nph_atomic_store_u32(&ctx->sync, next, NPH_MEMORY_ORDER_RELEASE);
    }
}

// Works the stages as they are published until the last is done.
static void participate(StepContext *ctx, u32 p)
{
    u32 seen = 0U;
    u32 spins = 0U;
    for (;;) {
        const u32 sync = nph_atomic_load_u32(&ctx->sync, NPH_MEMORY_ORDER_ACQUIRE);
        if (sync == PHYS_SYNC_DONE) {
            return;
        }
        if (sync != seen) {
            seen = sync;
            spins = 0U;
            execute_stage(ctx, sync - 1U, p);
            continue;
        }
        if (++spins < PHYS_SPINS_BEFORE_YIELD) {
            nph_cpu_relax();
        } else {
            nph_thread_yield();
        }
    }
}

static void helper_job(void *user)
{
    StepContext *ctx = (StepContext *)user;
    const u32 p = nph_atomic_fetch_add_u32(&ctx->joined, 1U, NPH_MEMORY_ORDER_RELAXED);
    NPH_ASSERT(p < ctx->participants);
    participate(ctx, p);
}

// Blocks hold at least `min_block` items, and a stage has at most four
// blocks per participant: enough to even out, few enough to stay cheap.
static void split_blocks(u32 participants, u32 count, u32 min_block, u32 *size, u32 *blocks)
{
    if (count == 0U) {
        *size = 1U;
        *blocks = 0U;
        return;
    }
    const u32 max_blocks = 4U * participants;
    *size = nph_max_u32(nph_max_u32(min_block, 1U), (count + max_blocks - 1U) / max_blocks);
    *blocks = (count + *size - 1U) / *size;
}

// A stage of `joint_count` step joints (from `joint_first`) and `count`
// items (from `first`); none when both are empty.
static NphResult push_mixed_stage(NphPhysicsWorld *w, u32 participants, PhysStageType type,
                                  u32 joint_first, u32 joint_count, u32 joint_min_block, u32 first,
                                  u32 count, u32 min_block)
{
    if (count == 0U && joint_count == 0U) {
        return NPH_RESULT_OK;
    }
    PhysStage *s = NPH_ARRAY_PUSH_SLOT(&w->stages);
    if (s == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(s, 0, sizeof *s);
    s->type = (u32)type;
    s->first = first;
    s->count = count;
    s->joint_first = joint_first;
    s->joint_count = joint_count;
    u32 blocks = 0U;
    split_blocks(participants, joint_count, joint_min_block, &s->joint_block_size,
                 &s->joint_blocks);
    split_blocks(participants, count, min_block, &s->block_size, &blocks);
    s->block_count = s->joint_blocks + blocks;
    s->claim_first = w->claims.len;
    return NPH_ARRAY_RESIZE(&w->claims, w->claims.len + s->block_count);
}

static NphResult push_stage(NphPhysicsWorld *w, u32 participants, PhysStageType type, u32 first,
                            u32 count, u32 min_block)
{
    return push_mixed_stage(w, participants, type, 0U, 0U, 1U, first, count, min_block);
}

// One constraint pass: the overflow's joints, then its contacts (one block
// each: they share bodies), then every colour in order, its joints and
// contacts together. Restitution has no joints.
static NphResult push_contact_pass(NphPhysicsWorld *w, u32 participants, PhysStageType type)
{
    const bool joints = type != PHYS_STAGE_RESTITUTION;
    const u32 overflow_joints = joints ? w->color_joint_count[PHYS_GRAPH_OVERFLOW] : 0U;
    NPH_TRY(push_mixed_stage(w, 1U, type, w->color_joint_first[PHYS_GRAPH_OVERFLOW],
                             overflow_joints, overflow_joints, 0U, 0U, 1U));
    NPH_TRY(push_stage(w, 1U, type, w->color_group_first[PHYS_GRAPH_OVERFLOW],
                       w->color_group_count[PHYS_GRAPH_OVERFLOW],
                       w->color_group_count[PHYS_GRAPH_OVERFLOW]));
    for (u32 c = 0U; c < PHYS_GRAPH_COLORS; ++c) {
        NPH_TRY(push_mixed_stage(w, participants, type, w->color_joint_first[c],
                                 joints ? w->color_joint_count[c] : 0U, 8U, w->color_group_first[c],
                                 w->color_group_count[c], 4U));
    }
    return NPH_RESULT_OK;
}

// The graph's joints in one array for the stages: by colour, the overflow
// last (as the groups).
static NphResult build_step_joints(NphPhysicsWorld *w)
{
    NPH_ARRAY_CLEAR(&w->step_joints);
    for (u32 c = 0U; c <= PHYS_GRAPH_COLORS; ++c) {
        const NphArrayU32 *list =
            c == PHYS_GRAPH_OVERFLOW ? &w->overflow_joints : &w->colors[c].joints;
        w->color_joint_first[c] = w->step_joints.len;
        w->color_joint_count[c] = list->len;
        for (u32 i = 0U; i < list->len; ++i) {
            NPH_TRY(NPH_ARRAY_PUSH(&w->step_joints, list->data[i]));
        }
    }
    return NPH_RESULT_OK;
}

// Lays out the groups: eight contacts of a colour each (one each for the
// overflow), points after them.
static NphResult build_groups(NphPhysicsWorld *w)
{
    u32 groups = 0U;
    u32 points = 0U;
    for (u32 c = 0U; c <= PHYS_GRAPH_COLORS; ++c) {
        const NphArrayU32 *list = c == PHYS_GRAPH_OVERFLOW ? &w->overflow : &w->colors[c].contacts;
        const u32 per = c == PHYS_GRAPH_OVERFLOW ? 1U : PHYS_LANES;
        w->color_group_first[c] = groups;
        w->color_group_count[c] = (list->len + per - 1U) / per;
        groups += w->color_group_count[c];
    }
    NPH_TRY(NPH_ARRAY_RESIZE(&w->groups, groups));
    u32 g = 0U;
    for (u32 c = 0U; c <= PHYS_GRAPH_COLORS; ++c) {
        const NphArrayU32 *list = c == PHYS_GRAPH_OVERFLOW ? &w->overflow : &w->colors[c].contacts;
        const u32 per = c == PHYS_GRAPH_OVERFLOW ? 1U : PHYS_LANES;
        for (u32 i = 0U; i < list->len; i += per) {
            PhysContactLanes *group = &w->groups.data[g++];
            const u32 lanes = nph_min_u32(per, list->len - i);
            group->lane_count = lanes;
            group->point_count = 0U;
            for (u32 l = 0U; l < PHYS_LANES; ++l) {
                group->contact[l] = l < lanes ? list->data[i + l] : PHYS_NULL;
                if (l < lanes) {
                    group->point_count = nph_max_u32(
                        group->point_count, w->contacts.data[list->data[i + l]].point_count);
                }
            }
            group->first_point = points;
            points += group->point_count;
        }
    }
    return NPH_ARRAY_RESIZE(&w->points, points);
}

// After the stages, in the awake list's order: proxies that grew, move
// events, kinematic bodies that push sleepers awake.
static NphResult post_finalize(NphPhysicsWorld *w)
{
    // A motor driving towards a speed keeps its bodies awake: slow as they
    // may still turn, they are being driven.
    for (u32 i = 0U; i < w->step_joints.len; ++i) {
        const PhysJoint *j = &w->joints.data[w->step_joints.data[i]];
        if (j->type == NPH_PHYSICS_JOINT_HINGE && j->hinge.enable_motor != 0 &&
            j->hinge.motor_speed != 0.0F && j->hinge.max_motor_torque > 0.0F) {
            w->bodies.data[j->body_a].sleep_time = 0.0F;
            w->bodies.data[j->body_b].sleep_time = 0.0F;
        }
    }
    NPH_TRY(NPH_ARRAY_RESERVE(&w->move_events, w->awake_bodies.len));
    for (u32 i = 0U; i < w->awake_bodies.len; ++i) {
        const u32 slot = w->awake_bodies.data[i];
        PhysBody *b = &w->bodies.data[slot];
        if ((b->flags & PHYS_BODY_ENLARGED) != 0U) {
            b->flags &= ~(u32)PHYS_BODY_ENLARGED;
            for (u32 s = b->shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
                PhysShape *shape = &w->shapes.data[s];
                if ((shape->flags & PHYS_SHAPE_ENLARGED) != 0U) {
                    shape->flags &= ~(u32)PHYS_SHAPE_ENLARGED;
                    nph_aabb_tree_enlarge_proxy(&w->trees[shape->tree], shape->proxy,
                                                shape->fat_aabb);
                    NPH_TRY(phys_move_buffer_push(w, s));
                }
            }
        }
        NphPhysicsBodyMoveEvent e;
        e.body = (NphPhysicsBody){b->handle};
        e.fell_asleep = 0U;
        e.user_data = b->user_data;
        e.position = b->origin;
        e.rotation = b->rotation;
        b->move_event = w->move_events.len;
        NPH_TRY(NPH_ARRAY_PUSH(&w->move_events, e));
    }
    // A moving kinematic body wakes what it touches: a sleeper on a
    // platform must ride it.
    for (u32 i = 0U; i < w->awake_bodies.len; ++i) {
        const u32 slot = w->awake_bodies.data[i];
        const PhysBody *b = &w->bodies.data[slot];
        if (b->type != NPH_PHYSICS_BODY_KINEMATIC || b->sleep_time > 0.0F) {
            continue;
        }
        for (u32 key = b->contact_head; key != PHYS_NULL;) {
            const PhysContact *c = &w->contacts.data[key >> 1U];
            const u32 other = (key & 1U) == 0U ? c->body_b : c->body_a;
            key = c->next[key & 1U];
            if ((c->flags & PHYS_CONTACT_TOUCHING) != 0U &&
                (w->bodies.data[other].flags & PHYS_BODY_AWAKE) == 0U) {
                NPH_TRY(phys_wake_body(w, other));
            }
        }
        for (u32 key = b->joint_head; key != PHYS_NULL;) {
            const PhysJoint *j = &w->joints.data[key >> 1U];
            const u32 other = (key & 1U) == 0U ? j->body_b : j->body_a;
            key = j->next[key & 1U];
            if ((w->bodies.data[other].flags & PHYS_BODY_AWAKE) == 0U) {
                NPH_TRY(phys_wake_body(w, other));
            }
        }
    }
    return NPH_RESULT_OK;
}

static NphResult hit_events(NphPhysicsWorld *w)
{
    for (u32 c = 0U; c <= PHYS_GRAPH_COLORS; ++c) {
        const NphArrayU32 *list = c == PHYS_GRAPH_OVERFLOW ? &w->overflow : &w->colors[c].contacts;
        for (u32 i = 0U; i < list->len; ++i) {
            const PhysContact *contact = &w->contacts.data[list->data[i]];
            if ((contact->flags & PHYS_CONTACT_HIT_EVENTS) == 0U) {
                continue;
            }
            f32 best = w->hit_threshold;
            u32 best_point = PHYS_NULL;
            for (u32 k = 0U; k < contact->point_count; ++k) {
                const PhysContactPoint *p = &contact->points[k];
                const f32 approach = -p->relative_velocity;
                if (approach > best && p->max_normal_impulse > 0.0F) {
                    best = approach;
                    best_point = k;
                }
            }
            if (best_point == PHYS_NULL) {
                continue;
            }
            NphPhysicsContactHitEvent e;
            e.shape_a = phys_shape_handle(w, contact->shape_a);
            e.shape_b = phys_shape_handle(w, contact->shape_b);
            e.point = nph_dvec3_add_vec3(w->bodies.data[contact->body_a].center,
                                         contact->points[best_point].anchor_a);
            e.normal = contact->normal;
            e.approach_speed = best;
            NPH_TRY(NPH_ARRAY_PUSH(&w->hit_events, e));
        }
    }
    return NPH_RESULT_OK;
}

NphResult phys_solve(NphPhysicsWorld *w, f32 dt, NphJobSystem *js)
{
    const u32 n = w->awake_bodies.len;
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    // Solver bodies: the awake bodies in their list's order, then the dummy
    // (at rest, never written) for static and sleeping kinematic lanes.
    NPH_TRY(NPH_ARRAY_RESIZE(&w->solver_bodies, n + 1U));
    NPH_TRY(NPH_ARRAY_RESIZE(&w->solver_params, n));
    PhysSolverBody *dummy = &w->solver_bodies.data[n];
    memset(dummy, 0, sizeof *dummy);
    dummy->dq = nph_quat_identity();
    const NphPhysicsSolver solver = w->desc.solver;
    if (solver == NPH_PHYSICS_SOLVER_XPBD) {
        NPH_TRY(NPH_ARRAY_RESIZE(&w->solver_prev, n + 1U));
        w->solver_prev.data[n] = *dummy;
    }
    NPH_TRY(build_groups(w));
    NPH_TRY(build_step_joints(w));

    StepContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.w = w;
    ctx.dt = dt;
    ctx.h = dt / (f32)w->desc.substeps;
    ctx.inv_h = 1.0F / ctx.h;
    ctx.push_max = w->desc.contact_push_max_velocity;
    ctx.restitution_threshold = w->desc.restitution_threshold;
    ctx.dummy = n;
    // Contacts at most a quarter of the substep rate stiff; contacts against
    // static and kinematic bodies twice as stiff (they take the whole
    // correction) (Catto, "Solver2D").
    // Zero stiffnesses are relative to the substep rate: an eighth for
    // contacts, a quarter (the most) for joints.
    const f32 contact_hertz =
        w->desc.contact_hertz > 0.0F ? w->desc.contact_hertz : 0.125F * ctx.inv_h;
    const f32 joint_hertz = w->desc.joint_hertz > 0.0F ? w->desc.joint_hertz : 0.25F * ctx.inv_h;
    const f32 hertz = nph_f32_min(contact_hertz, 0.25F * ctx.inv_h);
    ctx.contact_soft = phys_make_soft(hertz, w->desc.contact_damping_ratio, ctx.h);
    ctx.static_soft = phys_make_soft(2.0F * hertz, w->desc.contact_damping_ratio, ctx.h);
    ctx.joint_step.h = ctx.h;
    ctx.joint_step.inv_h = ctx.inv_h;
    ctx.joint_step.soft = phys_make_soft(nph_f32_min(joint_hertz, 0.25F * ctx.inv_h),
                                         w->desc.joint_damping_ratio, ctx.h);
    ctx.joint_step.dummy = n;
    if (solver == NPH_PHYSICS_SOLVER_TGS) {
        // Rigid rows that take a fifth of the error a substep (Baumgarte's
        // factor), the push capped as the soft step's.
        // A linear slop of overlap is left alone (Box2D v2's, PhysX's rest
        // offset), so resting contacts do not push.
        const PhysSoftness hard = {0.2F * ctx.inv_h, 1.0F, 0.0F};
        ctx.contact_soft = hard;
        ctx.static_soft = hard;
        ctx.joint_step.soft = hard;
        ctx.slop = NPH_PHYSICS_LINEAR_SLOP;
    }
    w->last_substep = ctx.h;

    // Helpers when there is enough work to share, as many as it can use,
    // and never more threads than the machine runs at once: spinners
    // sharing a core wait on each other's time slices.
    u32 participants = 1U;
    if (js != NULL) {
        const u32 workers = nph_job_system_worker_count(js);
        const u32 others = nph_job_worker_index() != NPH_JOB_WORKER_NONE ? workers - 1U : workers;
        const u32 cores = nph_max_u32(nph_cpu_info()->logical_cores, 1U);
        participants = solver_participants(
            w, nph_min_u32(1U + nph_min_u32(others, NPH_JOB_MAX_WORKERS - 1U), cores));
    }

    NPH_ARRAY_CLEAR(&w->stages);
    NPH_ARRAY_CLEAR(&w->claims);
    const u32 p = participants;
    NPH_TRY(push_stage(w, p, PHYS_STAGE_PREPARE_BODIES, 0U, n, 32U));
    NPH_TRY(push_mixed_stage(w, p, PHYS_STAGE_PREPARE_CONTACTS, 0U, w->step_joints.len, 8U, 0U,
                             w->groups.len, 4U));
    for (u32 s = 0U; s < w->desc.substeps; ++s) {
        if (solver == NPH_PHYSICS_SOLVER_XPBD) {
            NPH_TRY(push_stage(w, p, PHYS_STAGE_XPBD_PREDICT, 0U, n, 32U));
            NPH_TRY(push_contact_pass(w, p, PHYS_STAGE_XPBD_POSITIONS));
            NPH_TRY(push_stage(w, p, PHYS_STAGE_XPBD_DERIVE, 0U, n, 32U));
            NPH_TRY(push_contact_pass(w, p, PHYS_STAGE_XPBD_VELOCITIES));
            continue;
        }
        NPH_TRY(push_stage(w, p, PHYS_STAGE_INTEGRATE_VELOCITIES, 0U, n, 32U));
        NPH_TRY(push_contact_pass(w, p, PHYS_STAGE_WARM_START));
        NPH_TRY(push_contact_pass(w, p, PHYS_STAGE_SOLVE));
        NPH_TRY(push_stage(w, p, PHYS_STAGE_INTEGRATE_POSITIONS, 0U, n, 32U));
        if (solver == NPH_PHYSICS_SOLVER_SOFT_STEP) {
            NPH_TRY(push_contact_pass(w, p, PHYS_STAGE_RELAX));
        }
    }
    if (solver == NPH_PHYSICS_SOLVER_TGS) {
        // TGS's velocity iteration: the bias velocity taken back once.
        NPH_TRY(push_contact_pass(w, p, PHYS_STAGE_RELAX));
    }
    if (solver != NPH_PHYSICS_SOLVER_XPBD) {
        NPH_TRY(push_contact_pass(w, p, PHYS_STAGE_RESTITUTION));
    }
    NPH_TRY(push_stage(w, p, PHYS_STAGE_STORE, 0U, w->groups.len, 8U));
    NPH_TRY(push_stage(w, p, PHYS_STAGE_FINALIZE, 0U, n, 32U));
    if (w->claims.len > 0U) {
        memset(w->claims.data, 0, (size_t)w->claims.len * sizeof *w->claims.data);
    }
    ctx.stages = w->stages.data;
    ctx.stage_count = w->stages.len;
    ctx.claims = w->claims.data;
    ctx.participants = participants;
    nph_atomic_store_u32(&ctx.sync, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&ctx.joined, 1U, NPH_MEMORY_ORDER_RELAXED);

    u32 helpers = participants - 1U;
    NphJobCounter helpers_done = NPH_JOB_COUNTER_INIT;
    if (helpers > 0U) {
        NphJobDecl decls[NPH_JOB_MAX_WORKERS];
        for (u32 i = 0U; i < helpers; ++i) {
            decls[i].fn = helper_job;
            decls[i].user = &ctx;
            decls[i].name = "physics.solver";
        }
        if (nph_job_run(js, decls, helpers, NPH_JOB_PRIORITY_HIGH, &helpers_done) !=
            NPH_RESULT_OK) {
            helpers = 0U; // the pool is full: work alone
        }
    }
    nph_atomic_store_u32(&ctx.sync, ctx.stage_count > 0U ? 1U : PHYS_SYNC_DONE,
                         NPH_MEMORY_ORDER_RELEASE);
    participate(&ctx, 0U);
    if (helpers > 0U) {
        nph_job_wait(js, &helpers_done);
    }
    w->stats.workers = nph_atomic_load_u32(&ctx.joined, NPH_MEMORY_ORDER_RELAXED);

    const u64 t = nph_time_now_ns();
    NPH_TRY(post_finalize(w));
    NPH_TRY(hit_events(w));
    w->stats.finalize_ms = (f64)(nph_time_now_ns() - t) * 1e-6;
    return NPH_RESULT_OK;
}
