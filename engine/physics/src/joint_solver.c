// SPDX-License-Identifier: Apache-2.0
// The joints' arithmetic: preparation for a step, the warm start, and the
// soft and rigid solves of each substep (docs/physics.md "Joints";
// ADR-0044).
//
// As for contacts, a joint's anchors and frames are fixed at the step's
// start, relative to the centres of mass and in world axes, and the
// bodies' displacements and rotations since then (the solver bodies' dp
// and dq) move them: the error inside the step needs no collision and no
// 64-bit arithmetic. Each row is a soft constraint while solving (the
// world's joint stiffness) and rigid while relaxing; limits are one-sided
// and speculative, as contacts are: apart, they only stop the approach
// (Catto, "Solver2D", 2024). Within a joint the rows run motor or spring,
// then limits, then the locked axes, then the anchors last, so the most
// important constraint is the one left exact (Box2D's order).
//
// Joints are few beside contacts and differ by type, so they are scalar
// and compiled once: every ISA gets the same bits.

#include <nepheline/core/assert.h>

#include "world_internal.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Small linear algebra
// ---------------------------------------------------------------------------

// x with K x = b for a symmetric K by columns (Cramer's rule); zero when K
// is singular (no mass on either side).
static NphVec3 solve3(const NphVec3 k[3], NphVec3 b)
{
    const NphVec3 c12 = nph_vec3_cross(k[1], k[2]);
    const f32 det = nph_vec3_dot(k[0], c12);
    if (!(det > 0.0F)) {
        return nph_vec3_zero();
    }
    const f32 inv = 1.0F / det;
    return nph_vec3(nph_vec3_dot(b, c12) * inv, nph_vec3_dot(k[0], nph_vec3_cross(b, k[2])) * inv,
                    nph_vec3_dot(k[0], nph_vec3_cross(k[1], b)) * inv);
}

// The effective mass of the anchors meeting: K = (mA + mB) I - [rA]x IA^-1
// [rA]x - [rB]x IB^-1 [rB]x, by columns.
static void point_mass(const PhysJoint *j, NphVec3 ra, NphVec3 rb, NphVec3 k[3])
{
    const NphVec3 e[3] = {nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(0.0F, 1.0F, 0.0F),
                          nph_vec3(0.0F, 0.0F, 1.0F)};
    const f32 m = j->inv_mass_a + j->inv_mass_b;
    for (u32 c = 0U; c < 3U; ++c) {
        const NphVec3 ta = nph_mat3_mul_vec3(j->inv_inertia_a, nph_vec3_cross(ra, e[c]));
        const NphVec3 tb = nph_mat3_mul_vec3(j->inv_inertia_b, nph_vec3_cross(rb, e[c]));
        k[c] = nph_vec3_sub(nph_vec3_sub(nph_vec3_scale(e[c], m), nph_vec3_cross(ra, ta)),
                            nph_vec3_cross(rb, tb));
    }
}

// The angular effective mass IA^-1 + IB^-1, by columns.
static void angular_mass(const PhysJoint *j, NphVec3 k[3])
{
    for (u32 c = 0U; c < 3U; ++c) {
        k[c] = nph_vec3_add(j->inv_inertia_a.c[c], j->inv_inertia_b.c[c]);
    }
}

// 1 / (axis . (IA^-1 + IB^-1) axis): the effective mass of one angular row.
static f32 axial_mass(const PhysJoint *j, NphVec3 axis)
{
    const f32 k = nph_vec3_dot(axis, nph_mat3_mul_vec3(j->inv_inertia_a, axis)) +
                  nph_vec3_dot(axis, nph_mat3_mul_vec3(j->inv_inertia_b, axis));
    return k > 0.0F ? 1.0F / k : 0.0F;
}

// B's rotation from A's frame about A's X axis, in [-pi, pi]: the twist of
// the swing-twist decomposition of conj(qa) qb.
static f32 twist_angle(NphQuat qa, NphQuat qb)
{
    NphQuat r = nph_quat_mul(nph_quat_conjugate(qa), qb);
    if (r.w < 0.0F) {
        r = nph_quat_neg(r);
    }
    return 2.0F * nph_f32_atan2(r.x, r.w);
}

static f32 unwind(f32 a)
{
    const f32 pi = 3.14159265F;
    if (a > pi) {
        return a - 2.0F * pi;
    }
    if (a < -pi) {
        return a + 2.0F * pi;
    }
    return a;
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

enum {
    // Set each step: which ends take impulses (static and kinematic ends
    // never do; the dummy body is shared and must not be written).
    PHYS_JOINT_WRITE_A = 1U << 8U,
    PHYS_JOINT_WRITE_B = 1U << 9U,
};

typedef struct JointState {
    PhysSolverBody *a;
    PhysSolverBody *b;
    NphVec3 ra; // the anchors from the centres of mass, now
    NphVec3 rb;
    NphQuat qa; // the frames' rotations, now
    NphQuat qb;
} JointState;

static JointState joint_state(PhysSolverBody *bodies, const PhysJoint *j)
{
    JointState s;
    s.a = &bodies[j->solver_a];
    s.b = &bodies[j->solver_b];
    s.ra = nph_quat_rotate(s.a->dq, j->anchor_a);
    s.rb = nph_quat_rotate(s.b->dq, j->anchor_b);
    s.qa = nph_quat_mul(s.a->dq, j->frame_q_a);
    s.qb = nph_quat_mul(s.b->dq, j->frame_q_b);
    return s;
}

// A linear impulse `p` at the anchors: B gains it, A loses it.
static void apply_linear(const PhysJoint *j, JointState *s, NphVec3 p)
{
    if ((j->flags & PHYS_JOINT_WRITE_A) != 0U) {
        s->a->v = nph_vec3_mul_add(s->a->v, p, -j->inv_mass_a);
        s->a->w =
            nph_vec3_sub(s->a->w, nph_mat3_mul_vec3(j->inv_inertia_a, nph_vec3_cross(s->ra, p)));
    }
    if ((j->flags & PHYS_JOINT_WRITE_B) != 0U) {
        s->b->v = nph_vec3_mul_add(s->b->v, p, j->inv_mass_b);
        s->b->w =
            nph_vec3_add(s->b->w, nph_mat3_mul_vec3(j->inv_inertia_b, nph_vec3_cross(s->rb, p)));
    }
}

// An angular impulse `l`: B gains it, A loses it.
static void apply_angular(const PhysJoint *j, JointState *s, NphVec3 l)
{
    if ((j->flags & PHYS_JOINT_WRITE_A) != 0U) {
        s->a->w = nph_vec3_sub(s->a->w, nph_mat3_mul_vec3(j->inv_inertia_a, l));
    }
    if ((j->flags & PHYS_JOINT_WRITE_B) != 0U) {
        s->b->w = nph_vec3_add(s->b->w, nph_mat3_mul_vec3(j->inv_inertia_b, l));
    }
}

static NphVec3 relative_angular(const JointState *s)
{
    return nph_vec3_sub(s->b->w, s->a->w);
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

// The coefficients of one soft row: speculative when `c` (the gap, positive
// apart) is open, soft while solving, rigid while relaxing.
typedef struct RowSoft {
    f32 bias;
    f32 mass_scale;
    f32 impulse_scale;
} RowSoft;

static RowSoft limit_soft(f32 c, const PhysJointStep *step, bool use_bias)
{
    RowSoft r = {0.0F, 1.0F, 0.0F};
    if (c > 0.0F) {
        r.bias = c * step->inv_h; // close at most the gap this substep
    } else if (use_bias) {
        r.bias = step->soft.bias_rate * c;
        r.mass_scale = step->soft.mass_scale;
        r.impulse_scale = step->soft.impulse_scale;
    }
    return r;
}

// A one-sided angular row: the gap `c` closes at the rate dot(wA - wB,
// axis) (so the impulse turns B against `axis`); the impulse stays >= 0.
static void solve_angular_limit(const PhysJoint *j, JointState *s, NphVec3 axis, f32 c,
                                f32 *accumulated, const PhysJointStep *step, bool use_bias)
{
    const RowSoft r = limit_soft(c, step, use_bias);
    const f32 cdot = -nph_vec3_dot(relative_angular(s), axis);
    const f32 impulse =
        -axial_mass(j, axis) * r.mass_scale * (cdot + r.bias) - r.impulse_scale * *accumulated;
    const f32 total = nph_f32_max(*accumulated + impulse, 0.0F);
    const f32 delta = total - *accumulated;
    *accumulated = total;
    apply_angular(j, s, nph_vec3_scale(axis, -delta));
}

// The anchors meet (every joint): three rows at once.
static void solve_point(PhysJoint *j, JointState *s, const PhysJointStep *step, bool use_bias)
{
    const NphVec3 va = nph_vec3_add(s->a->v, nph_vec3_cross(s->a->w, s->ra));
    const NphVec3 vb = nph_vec3_add(s->b->v, nph_vec3_cross(s->b->w, s->rb));
    const NphVec3 cdot = nph_vec3_sub(vb, va);
    NphVec3 bias = nph_vec3_zero();
    f32 mass_scale = 1.0F;
    f32 impulse_scale = 0.0F;
    if (use_bias) {
        const NphVec3 c =
            nph_vec3_add(nph_vec3_add(nph_vec3_sub(s->b->dp, s->a->dp), nph_vec3_sub(s->rb, s->ra)),
                         j->delta_center);
        bias = nph_vec3_scale(c, step->soft.bias_rate);
        mass_scale = step->soft.mass_scale;
        impulse_scale = step->soft.impulse_scale;
    }
    NphVec3 k[3];
    point_mass(j, s->ra, s->rb, k);
    const NphVec3 x = solve3(k, nph_vec3_add(cdot, bias));
    const NphVec3 impulse = nph_vec3_sub(nph_vec3_scale(x, -mass_scale),
                                         nph_vec3_scale(j->linear_impulse, impulse_scale));
    j->linear_impulse = nph_vec3_add(j->linear_impulse, impulse);
    apply_linear(j, s, impulse);
}

// The frames align (fixed): three angular rows, the error the rotation
// vector from A's frame to B's (twice the vector part of the error
// quaternion, the shorter way round).
static void solve_rotation_lock(PhysJoint *j, JointState *s, const PhysJointStep *step,
                                bool use_bias)
{
    const NphVec3 cdot = relative_angular(s);
    NphVec3 bias = nph_vec3_zero();
    f32 mass_scale = 1.0F;
    f32 impulse_scale = 0.0F;
    if (use_bias) {
        NphQuat e = nph_quat_mul(s->qb, nph_quat_conjugate(s->qa));
        if (e.w < 0.0F) {
            e = nph_quat_neg(e);
        }
        bias = nph_vec3_scale(nph_vec3(e.x, e.y, e.z), 2.0F * step->soft.bias_rate);
        mass_scale = step->soft.mass_scale;
        impulse_scale = step->soft.impulse_scale;
    }
    NphVec3 k[3];
    angular_mass(j, k);
    const NphVec3 x = solve3(k, nph_vec3_add(cdot, bias));
    const NphVec3 impulse = nph_vec3_sub(nph_vec3_scale(x, -mass_scale),
                                         nph_vec3_scale(j->angular_impulse, impulse_scale));
    j->angular_impulse = nph_vec3_add(j->angular_impulse, impulse);
    apply_angular(j, s, impulse);
}

// The X axes align (hinge): two angular rows about A's Y and Z, whose error
// is the cross product of the axes (zero aligned). The accumulated impulses
// are kept as the two rows' scalars (angular_impulse.x, .y), so the warm
// start applies them about the current axes and never about the hinge.
static void solve_axis_lock(PhysJoint *j, JointState *s, const PhysJointStep *step, bool use_bias)
{
    const NphVec3 p1 = nph_quat_axis_y(s->qa);
    const NphVec3 p2 = nph_quat_axis_z(s->qa);
    const NphVec3 dw = relative_angular(s);
    f32 c1 = 0.0F;
    f32 c2 = 0.0F;
    f32 mass_scale = 1.0F;
    f32 impulse_scale = 0.0F;
    if (use_bias) {
        const NphVec3 e = nph_vec3_cross(nph_quat_axis_x(s->qa), nph_quat_axis_x(s->qb));
        c1 = step->soft.bias_rate * nph_vec3_dot(e, p1);
        c2 = step->soft.bias_rate * nph_vec3_dot(e, p2);
        mass_scale = step->soft.mass_scale;
        impulse_scale = step->soft.impulse_scale;
    }
    const NphVec3 ip1 = nph_vec3_add(nph_mat3_mul_vec3(j->inv_inertia_a, p1),
                                     nph_mat3_mul_vec3(j->inv_inertia_b, p1));
    const NphVec3 ip2 = nph_vec3_add(nph_mat3_mul_vec3(j->inv_inertia_a, p2),
                                     nph_mat3_mul_vec3(j->inv_inertia_b, p2));
    const f32 k11 = nph_vec3_dot(p1, ip1);
    const f32 k12 = nph_vec3_dot(p1, ip2);
    const f32 k22 = nph_vec3_dot(p2, ip2);
    const f32 det = k11 * k22 - k12 * k12;
    if (!(det > 0.0F)) {
        return;
    }
    const f32 inv = 1.0F / det;
    const f32 b1 = nph_vec3_dot(dw, p1) + c1;
    const f32 b2 = nph_vec3_dot(dw, p2) + c2;
    const f32 x1 = (k22 * b1 - k12 * b2) * inv;
    const f32 x2 = (k11 * b2 - k12 * b1) * inv;
    const f32 i1 = -mass_scale * x1 - impulse_scale * j->angular_impulse.x;
    const f32 i2 = -mass_scale * x2 - impulse_scale * j->angular_impulse.y;
    j->angular_impulse.x += i1;
    j->angular_impulse.y += i2;
    apply_angular(j, s, nph_vec3_add(nph_vec3_scale(p1, i1), nph_vec3_scale(p2, i2)));
}

static void solve_hinge(PhysJoint *j, JointState *s, const PhysJointStep *step, bool use_bias)
{
    const NphPhysicsHingeJoint *h = &j->hinge;
    const NphVec3 axis = nph_quat_axis_x(s->qa);
    const f32 angle = twist_angle(s->qa, s->qb);
    const f32 mass = axial_mass(j, axis);
    // The spring is a force: once a substep, in the solve. Solved again
    // while relaxing it would double its implicit damping (measured: an
    // undamped 2 Hz spring lost a third of its swing in five periods).
    if (h->enable_spring != 0 && use_bias) {
        const PhysSoftness *soft = &j->spring_soft;
        const f32 c = unwind(angle - h->spring_target);
        const f32 cdot = nph_vec3_dot(relative_angular(s), axis);
        const f32 impulse = -soft->mass_scale * mass * (cdot + soft->bias_rate * c) -
                            soft->impulse_scale * j->spring_impulse;
        j->spring_impulse += impulse;
        apply_angular(j, s, nph_vec3_scale(axis, impulse));
    }
    if (h->enable_motor != 0) {
        const f32 cdot = nph_vec3_dot(relative_angular(s), axis) - h->motor_speed;
        const f32 max_impulse = step->h * h->max_motor_torque;
        const f32 old = j->motor_impulse;
        j->motor_impulse = nph_f32_clamp(old - mass * cdot, -max_impulse, max_impulse);
        apply_angular(j, s, nph_vec3_scale(axis, j->motor_impulse - old));
    }
    if (h->enable_limit != 0) {
        // The lower limit turns B forward (its gap closes as B turns back),
        // the upper one back.
        solve_angular_limit(j, s, nph_vec3_neg(axis), angle - h->lower, &j->lower_impulse, step,
                            use_bias);
        solve_angular_limit(j, s, axis, h->upper - angle, &j->upper_impulse, step, use_bias);
    }
    solve_axis_lock(j, s, step, use_bias);
}

// A ball joint's cone: the angle between the X axes stays within the
// ellipse of half angles swing_y and swing_z, measured along the swing's
// own axis (exact for a circular cone).
static void solve_swing(PhysJoint *j, JointState *s, const PhysJointStep *step, bool use_bias)
{
    const NphPhysicsBallJoint *b = &j->ball;
    const NphVec3 xa = nph_quat_axis_x(s->qa);
    const NphVec3 xb = nph_quat_axis_x(s->qb);
    const NphVec3 n = nph_vec3_cross(xa, xb);
    const f32 sin_phi = nph_vec3_length(n);
    if (!(sin_phi > 1e-6F)) {
        return; // on the axis: as far from the cone as it gets
    }
    const NphVec3 axis = nph_vec3_scale(n, 1.0F / sin_phi);
    const NphVec3 local = nph_quat_rotate_inverse(s->qa, axis);
    const f32 ey = local.y / b->swing_y;
    const f32 ez = local.z / b->swing_z;
    const f32 limit = 1.0F / nph_f32_sqrt(ey * ey + ez * ez);
    const f32 phi = nph_f32_atan2(sin_phi, nph_vec3_dot(xa, xb));
    solve_angular_limit(j, s, axis, limit - phi, &j->swing_impulse, step, use_bias);
}

static void solve_ball(PhysJoint *j, JointState *s, const PhysJointStep *step, bool use_bias)
{
    const NphPhysicsBallJoint *b = &j->ball;
    if (b->friction_torque > 0.0F) {
        // Friction: a motor to zero relative rotation, its impulse within a
        // ball of the torque's size.
        NphVec3 k[3];
        angular_mass(j, k);
        const NphVec3 x = solve3(k, relative_angular(s));
        const NphVec3 old = j->friction_impulse;
        NphVec3 total = nph_vec3_sub(old, x);
        const f32 max_impulse = step->h * b->friction_torque;
        const f32 len_sq = nph_vec3_length_sq(total);
        if (len_sq > max_impulse * max_impulse) {
            total = nph_vec3_scale(total, max_impulse / nph_f32_sqrt(len_sq));
        }
        j->friction_impulse = total;
        apply_angular(j, s, nph_vec3_sub(total, old));
    }
    if (b->enable_swing_limit != 0) {
        solve_swing(j, s, step, use_bias);
    }
    if (b->enable_twist_limit != 0) {
        const NphVec3 sum = nph_vec3_add(nph_quat_axis_x(s->qa), nph_quat_axis_x(s->qb));
        const f32 len = nph_vec3_length(sum);
        if (len > 1e-3F) {
            const NphVec3 axis = nph_vec3_scale(sum, 1.0F / len);
            const f32 twist = twist_angle(s->qa, s->qb);
            solve_angular_limit(j, s, nph_vec3_neg(axis), twist - b->twist_lower, &j->lower_impulse,
                                step, use_bias);
            solve_angular_limit(j, s, axis, b->twist_upper - twist, &j->upper_impulse, step,
                                use_bias);
        }
    }
}

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

void phys_joint_prepare(NphPhysicsWorld *w, const PhysJointStep *step, PhysJoint *j)
{
    const PhysBody *a = &w->bodies.data[j->body_a];
    const PhysBody *b = &w->bodies.data[j->body_b];
    const bool dyn_a = a->type == NPH_PHYSICS_BODY_DYNAMIC;
    const bool dyn_b = b->type == NPH_PHYSICS_BODY_DYNAMIC;
    j->solver_a = a->awake_slot != PHYS_NULL ? a->awake_slot : step->dummy;
    j->solver_b = b->awake_slot != PHYS_NULL ? b->awake_slot : step->dummy;
    j->flags &= ~(u32)(PHYS_JOINT_WRITE_A | PHYS_JOINT_WRITE_B);
    j->flags |= (dyn_a ? (u32)PHYS_JOINT_WRITE_A : 0U) | (dyn_b ? (u32)PHYS_JOINT_WRITE_B : 0U);
    j->inv_mass_a = dyn_a ? a->inv_mass : 0.0F;
    j->inv_mass_b = dyn_b ? b->inv_mass : 0.0F;
    j->inv_inertia_a = dyn_a ? w->solver_params.data[j->solver_a].inv_inertia : nph_mat3_zero();
    j->inv_inertia_b = dyn_b ? w->solver_params.data[j->solver_b].inv_inertia : nph_mat3_zero();
    j->anchor_a = nph_quat_rotate(a->rotation, nph_vec3_sub(j->frame_a.position, a->local_center));
    j->anchor_b = nph_quat_rotate(b->rotation, nph_vec3_sub(j->frame_b.position, b->local_center));
    j->delta_center = nph_dvec3_relative(b->center, a->center);
    j->frame_q_a = nph_quat_mul(a->rotation, j->frame_a.rotation);
    j->frame_q_b = nph_quat_mul(b->rotation, j->frame_b.rotation);
    // What is switched off starts from nothing when switched on.
    if (j->type == NPH_PHYSICS_JOINT_HINGE) {
        const NphPhysicsHingeJoint *h = &j->hinge;
        if (h->enable_limit == 0) {
            j->lower_impulse = 0.0F;
            j->upper_impulse = 0.0F;
        }
        if (h->enable_motor == 0) {
            j->motor_impulse = 0.0F;
        }
        if (h->enable_spring == 0) {
            j->spring_impulse = 0.0F;
        }
        j->spring_soft = phys_make_soft(h->enable_spring != 0 ? h->spring_hertz : 0.0F,
                                        h->spring_damping_ratio, step->h);
    } else if (j->type == NPH_PHYSICS_JOINT_BALL) {
        const NphPhysicsBallJoint *bj = &j->ball;
        if (bj->enable_swing_limit == 0) {
            j->swing_impulse = 0.0F;
        }
        if (bj->enable_twist_limit == 0) {
            j->lower_impulse = 0.0F;
            j->upper_impulse = 0.0F;
        }
        if (!(bj->friction_torque > 0.0F)) {
            j->friction_impulse = nph_vec3_zero();
        }
    }
}

NphVec3 phys_joint_angular_impulse(const PhysJoint *j, NphQuat qa, NphQuat qb)
{
    // The fixed joint keeps a world vector; the other types keep scalars
    // about axes rebuilt from the frames.
    NphVec3 angular = j->angular_impulse;
    if (j->type == NPH_PHYSICS_JOINT_HINGE) {
        angular = nph_vec3_add(nph_vec3_scale(nph_quat_axis_y(qa), j->angular_impulse.x),
                               nph_vec3_scale(nph_quat_axis_z(qa), j->angular_impulse.y));
        const f32 axial =
            j->lower_impulse - j->upper_impulse + j->motor_impulse + j->spring_impulse;
        angular = nph_vec3_mul_add(angular, nph_quat_axis_x(qa), axial);
    } else if (j->type == NPH_PHYSICS_JOINT_BALL) {
        angular = j->friction_impulse;
        const NphVec3 xa = nph_quat_axis_x(qa);
        const NphVec3 xb = nph_quat_axis_x(qb);
        const NphVec3 n = nph_vec3_cross(xa, xb);
        const f32 sin_phi = nph_vec3_length(n);
        if (j->swing_impulse != 0.0F && sin_phi > 1e-6F) {
            angular = nph_vec3_mul_add(angular, n, -j->swing_impulse / sin_phi);
        }
        const NphVec3 sum = nph_vec3_add(xa, xb);
        const f32 len = nph_vec3_length(sum);
        if (len > 1e-3F) {
            angular = nph_vec3_mul_add(angular, sum, (j->lower_impulse - j->upper_impulse) / len);
        }
    }
    return angular;
}

void phys_joint_warm_start(PhysSolverBody *bodies, PhysJoint *j)
{
    JointState s = joint_state(bodies, j);
    apply_angular(j, &s, phys_joint_angular_impulse(j, s.qa, s.qb));
    apply_linear(j, &s, j->linear_impulse);
}

void phys_joint_solve(PhysSolverBody *bodies, const PhysJointStep *step, PhysJoint *j,
                      bool use_bias)
{
    JointState s = joint_state(bodies, j);
    switch ((NphPhysicsJointType)j->type) {
    case NPH_PHYSICS_JOINT_BALL:
        solve_ball(j, &s, step, use_bias);
        break;
    case NPH_PHYSICS_JOINT_HINGE:
        solve_hinge(j, &s, step, use_bias);
        break;
    case NPH_PHYSICS_JOINT_FIXED:
        solve_rotation_lock(j, &s, step, use_bias);
        break;
    case NPH_PHYSICS_JOINT_TYPE_COUNT:
    default:
        NPH_ASSERT_MSG(false, "physics: unknown joint type %u", j->type);
        return;
    }
    solve_point(j, &s, step, use_bias);
}

// ---------------------------------------------------------------------------
// XPBD (Mueller et al., "Detailed Rigid Body Simulation with Extended
// Position Based Dynamics", SCA 2020; ADR-0045): the joints' errors are
// closed by moving the bodies, rigidly (the spring compliantly), and the
// motor, the spring's damper, and friction act on the velocities the
// positions give. Multipliers are kept as impulses (lambda over the
// substep) for the reaction.
// ---------------------------------------------------------------------------

// q turned by the small rotation vector t: q + (t, 0) q / 2, normalized.
static NphQuat turn(NphQuat q, NphVec3 t)
{
    const NphQuat r = nph_quat(
        0.5F * (t.x * q.w + (t.y * q.z - t.z * q.y)), 0.5F * (t.y * q.w + (t.z * q.x - t.x * q.z)),
        0.5F * (t.z * q.w + (t.x * q.y - t.y * q.x)), -0.5F * (t.x * q.x + t.y * q.y + t.z * q.z));
    return nph_quat_normalize(nph_quat_add(q, r));
}

static void refresh(const PhysJoint *j, JointState *s)
{
    s->ra = nph_quat_rotate(s->a->dq, j->anchor_a);
    s->rb = nph_quat_rotate(s->b->dq, j->anchor_b);
    s->qa = nph_quat_mul(s->a->dq, j->frame_q_a);
    s->qb = nph_quat_mul(s->b->dq, j->frame_q_b);
}

// A positional impulse `p` at the anchors: B moves by it, A against it.
static void move_linear(const PhysJoint *j, JointState *s, NphVec3 p)
{
    if ((j->flags & PHYS_JOINT_WRITE_A) != 0U) {
        s->a->dp = nph_vec3_mul_add(s->a->dp, p, -j->inv_mass_a);
        s->a->dq = turn(
            s->a->dq, nph_vec3_neg(nph_mat3_mul_vec3(j->inv_inertia_a, nph_vec3_cross(s->ra, p))));
    }
    if ((j->flags & PHYS_JOINT_WRITE_B) != 0U) {
        s->b->dp = nph_vec3_mul_add(s->b->dp, p, j->inv_mass_b);
        s->b->dq = turn(s->b->dq, nph_mat3_mul_vec3(j->inv_inertia_b, nph_vec3_cross(s->rb, p)));
    }
    refresh(j, s);
}

// A positional angular impulse `l`: B turns by it, A against it.
static void move_angular(const PhysJoint *j, JointState *s, NphVec3 l)
{
    if ((j->flags & PHYS_JOINT_WRITE_A) != 0U) {
        s->a->dq = turn(s->a->dq, nph_vec3_neg(nph_mat3_mul_vec3(j->inv_inertia_a, l)));
    }
    if ((j->flags & PHYS_JOINT_WRITE_B) != 0U) {
        s->b->dq = turn(s->b->dq, nph_mat3_mul_vec3(j->inv_inertia_b, l));
    }
    refresh(j, s);
}

// A one-sided angular row: the gap `c` (>= 0 allowed) closes as B turns
// about `axis`; a negative gap turns B back by it. The accumulated impulse
// keeps the soft step's sign (B gets -impulse about `axis`).
static void xpbd_limit(const PhysJoint *j, JointState *s, NphVec3 axis, f32 c, f32 *accumulated,
                       f32 inv_h)
{
    if (!(c < 0.0F)) {
        *accumulated = 0.0F;
        return;
    }
    const f32 lambda = c * axial_mass(j, axis);
    *accumulated = -lambda * inv_h;
    move_angular(j, s, nph_vec3_scale(axis, lambda));
}

static void xpbd_point(PhysJoint *j, JointState *s, f32 inv_h)
{
    const NphVec3 c =
        nph_vec3_add(nph_vec3_add(nph_vec3_sub(s->b->dp, s->a->dp), nph_vec3_sub(s->rb, s->ra)),
                     j->delta_center);
    NphVec3 k[3];
    point_mass(j, s->ra, s->rb, k);
    const NphVec3 p = solve3(k, nph_vec3_neg(c));
    j->linear_impulse = nph_vec3_scale(p, inv_h);
    move_linear(j, s, p);
}

void phys_joint_xpbd_positions(PhysSolverBody *bodies, const PhysJointStep *step, PhysJoint *j)
{
    JointState s = joint_state(bodies, j);
    const f32 inv_h = step->inv_h;
    NphVec3 k[3];
    switch ((NphPhysicsJointType)j->type) {
    case NPH_PHYSICS_JOINT_BALL: {
        const NphPhysicsBallJoint *b = &j->ball;
        if (b->enable_swing_limit != 0) {
            const NphVec3 xa = nph_quat_axis_x(s.qa);
            const NphVec3 xb = nph_quat_axis_x(s.qb);
            const NphVec3 n = nph_vec3_cross(xa, xb);
            const f32 sin_phi = nph_vec3_length(n);
            if (sin_phi > 1e-6F) {
                const NphVec3 axis = nph_vec3_scale(n, 1.0F / sin_phi);
                const NphVec3 local = nph_quat_rotate_inverse(s.qa, axis);
                const f32 ey = local.y / b->swing_y;
                const f32 ez = local.z / b->swing_z;
                const f32 limit = 1.0F / nph_f32_sqrt(ey * ey + ez * ez);
                const f32 phi = nph_f32_atan2(sin_phi, nph_vec3_dot(xa, xb));
                xpbd_limit(j, &s, axis, limit - phi, &j->swing_impulse, inv_h);
            }
        }
        if (b->enable_twist_limit != 0) {
            const NphVec3 sum = nph_vec3_add(nph_quat_axis_x(s.qa), nph_quat_axis_x(s.qb));
            const f32 len = nph_vec3_length(sum);
            if (len > 1e-3F) {
                const NphVec3 axis = nph_vec3_scale(sum, 1.0F / len);
                const f32 lower = twist_angle(s.qa, s.qb) - b->twist_lower;
                xpbd_limit(j, &s, nph_vec3_neg(axis), lower, &j->lower_impulse, inv_h);
                const f32 upper = b->twist_upper - twist_angle(s.qa, s.qb);
                xpbd_limit(j, &s, axis, upper, &j->upper_impulse, inv_h);
            }
        }
        break;
    }
    case NPH_PHYSICS_JOINT_HINGE: {
        const NphPhysicsHingeJoint *h = &j->hinge;
        if (h->enable_spring != 0) {
            // A compliant row: alpha = 1 / k with k the angular stiffness
            // (omega^2 over the axial inverse mass), alpha / h^2 in the
            // update (one iteration: the multiplier starts at zero).
            const NphVec3 axis = nph_quat_axis_x(s.qa);
            const f32 mass = axial_mass(j, axis);
            if (mass > 0.0F) {
                const f32 omega = 2.0F * 3.14159265F * h->spring_hertz;
                const f32 w = 1.0F / mass;
                const f32 alpha = w / (omega * omega * step->h * step->h);
                const f32 c = unwind(twist_angle(s.qa, s.qb) - h->spring_target);
                const f32 lambda = -c / (w + alpha);
                j->spring_impulse = lambda * inv_h;
                move_angular(j, &s, nph_vec3_scale(axis, lambda));
            }
        }
        if (h->enable_limit != 0) {
            const NphVec3 axis = nph_quat_axis_x(s.qa);
            xpbd_limit(j, &s, nph_vec3_neg(axis), twist_angle(s.qa, s.qb) - h->lower,
                       &j->lower_impulse, inv_h);
            xpbd_limit(j, &s, axis, h->upper - twist_angle(s.qa, s.qb), &j->upper_impulse, inv_h);
        }
        // The X axes aligned.
        const NphVec3 e = nph_vec3_cross(nph_quat_axis_x(s.qa), nph_quat_axis_x(s.qb));
        angular_mass(j, k);
        const NphVec3 l = solve3(k, nph_vec3_neg(e));
        j->angular_impulse = nph_vec3(nph_vec3_dot(l, nph_quat_axis_y(s.qa)) * inv_h,
                                      nph_vec3_dot(l, nph_quat_axis_z(s.qa)) * inv_h, 0.0F);
        move_angular(j, &s, l);
        break;
    }
    case NPH_PHYSICS_JOINT_FIXED: {
        NphQuat q = nph_quat_mul(s.qb, nph_quat_conjugate(s.qa));
        if (q.w < 0.0F) {
            q = nph_quat_neg(q);
        }
        angular_mass(j, k);
        const NphVec3 l = solve3(k, nph_vec3(-2.0F * q.x, -2.0F * q.y, -2.0F * q.z));
        j->angular_impulse = nph_vec3_scale(l, inv_h);
        move_angular(j, &s, l);
        break;
    }
    case NPH_PHYSICS_JOINT_TYPE_COUNT:
    default:
        NPH_ASSERT_MSG(false, "physics: unknown joint type %u", j->type);
        return;
    }
    xpbd_point(j, &s, inv_h);
}

void phys_joint_xpbd_velocities(PhysSolverBody *bodies, const PhysJointStep *step, PhysJoint *j)
{
    JointState s = joint_state(bodies, j);
    if (j->type == NPH_PHYSICS_JOINT_HINGE) {
        const NphPhysicsHingeJoint *h = &j->hinge;
        const NphVec3 axis = nph_quat_axis_x(s.qa);
        const f32 mass = axial_mass(j, axis);
        if (h->enable_spring != 0 && h->spring_damping_ratio > 0.0F) {
            // The spring's damper: 2 zeta omega of the relative speed, at
            // most all of it, this substep.
            const f32 omega = 2.0F * 3.14159265F * h->spring_hertz;
            const f32 share = nph_f32_min(2.0F * h->spring_damping_ratio * omega * step->h, 1.0F);
            const f32 cdot = nph_vec3_dot(relative_angular(&s), axis);
            apply_angular(j, &s, nph_vec3_scale(axis, -share * mass * cdot));
        }
        if (h->enable_motor != 0) {
            const f32 cdot = nph_vec3_dot(relative_angular(&s), axis) - h->motor_speed;
            const f32 max_impulse = step->h * h->max_motor_torque;
            j->motor_impulse = nph_f32_clamp(-mass * cdot, -max_impulse, max_impulse);
            apply_angular(j, &s, nph_vec3_scale(axis, j->motor_impulse));
        }
    } else if (j->type == NPH_PHYSICS_JOINT_BALL && j->ball.friction_torque > 0.0F) {
        NphVec3 k[3];
        angular_mass(j, k);
        NphVec3 impulse = nph_vec3_neg(solve3(k, relative_angular(&s)));
        const f32 max_impulse = step->h * j->ball.friction_torque;
        const f32 len_sq = nph_vec3_length_sq(impulse);
        if (len_sq > max_impulse * max_impulse) {
            impulse = nph_vec3_scale(impulse, max_impulse / nph_f32_sqrt(len_sq));
        }
        j->friction_impulse = impulse;
        apply_angular(j, &s, impulse);
    }
}

f32 phys_joint_angle(const NphPhysicsWorld *w, const PhysJoint *j)
{
    const PhysBody *a = &w->bodies.data[j->body_a];
    const PhysBody *b = &w->bodies.data[j->body_b];
    return twist_angle(nph_quat_mul(a->rotation, j->frame_a.rotation),
                       nph_quat_mul(b->rotation, j->frame_b.rotation));
}
