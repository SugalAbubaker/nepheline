// SPDX-License-Identifier: Apache-2.0
// The contact kernels, eight contacts a call, compiled once per ISA level
// (solver_sse4.c, solver_avx2.c). The including unit defines
// PHYS_KERNEL(name); wide.h resolves NphF32x8 to one AVX register or two SSE
// ones from that unit's flags. Every lane does the same arithmetic in the
// same order at every level, so the results agree to the bit (ADR-0008 §6).
//
// The soft step (Catto, "Solver2D", 2024): within a substep the solve pass
// uses soft contacts (a spring of the world's frequency and damping, the
// current separation estimated from the bodies' displacements with fixed
// anchors, pushing out no faster than the push limit), and the relax pass
// solves the same rows rigidly without bias, removing the velocity the
// springs added. Speculative points (separation > 0) only stop the
// approach. Friction after the normal rows, as a coupled 2D impulse
// clamped to the friction circle.

#include <nepheline/math/wide.h>

#include "world_internal.h"

#ifndef PHYS_KERNEL
#error "solver_impl.h: define PHYS_KERNEL(name) before including"
#endif

typedef NphF32x8 W8;

typedef struct BodyLanes {
    W8 vx, vy, vz;
    W8 wx, wy, wz;
    W8 dpx, dpy, dpz;
    W8 qx, qy, qz, qw;
} BodyLanes;

static W8 PHYS_KERNEL(load)(const f32 *p)
{
    return nph_f32x8_load(p);
}

// A body is four 16-byte rows (v, w, dp, dq: solver bodies are laid out for
// this), so eight lanes of a row come from two 4x4 transposes of the eight
// bodies' rows: no scalar shuffling through memory, whose narrow stores
// would stall the wide loads that read them back.
static void PHYS_KERNEL(transpose_row)(const PhysSolverBody *bodies, const u32 *index, u32 row,
                                       W8 *x, W8 *y, W8 *z, W8 *w)
{
    NphF32x4 r[8];
    for (u32 l = 0U; l < PHYS_LANES; ++l) {
        r[l] = _mm_loadu_ps(bodies[index[l]].rows[row]);
    }
    _MM_TRANSPOSE4_PS(r[0], r[1], r[2], r[3]);
    _MM_TRANSPOSE4_PS(r[4], r[5], r[6], r[7]);
    *x = nph_f32x8_from_x4(r[0], r[4]);
    *y = nph_f32x8_from_x4(r[1], r[5]);
    *z = nph_f32x8_from_x4(r[2], r[6]);
    if (w != NULL) {
        *w = nph_f32x8_from_x4(r[3], r[7]);
    }
}

static void PHYS_KERNEL(gather)(const PhysSolverBody *bodies, const u32 *index, bool positions,
                                BodyLanes *out)
{
    PHYS_KERNEL(transpose_row)(bodies, index, 0U, &out->vx, &out->vy, &out->vz, NULL);
    PHYS_KERNEL(transpose_row)(bodies, index, 1U, &out->wx, &out->wy, &out->wz, NULL);
    if (positions) {
        PHYS_KERNEL(transpose_row)(bodies, index, 2U, &out->dpx, &out->dpy, &out->dpz, NULL);
        PHYS_KERNEL(transpose_row)(bodies, index, 3U, &out->qx, &out->qy, &out->qz, &out->qw);
    }
}

// Writes the velocities of the lanes in `mask` back: lanes of static,
// kinematic, and empty bodies keep theirs (and are shared, so they must not
// be written). The rows' fourth floats are padding.
static void PHYS_KERNEL(scatter)(PhysSolverBody *bodies, const u32 *index, u32 mask,
                                 const BodyLanes *b)
{
    NphF32x4 v[8];
    NphF32x4 w[8];
    v[0] = nph_f32x8_lo(b->vx);
    v[1] = nph_f32x8_lo(b->vy);
    v[2] = nph_f32x8_lo(b->vz);
    v[3] = _mm_setzero_ps();
    v[4] = nph_f32x8_hi(b->vx);
    v[5] = nph_f32x8_hi(b->vy);
    v[6] = nph_f32x8_hi(b->vz);
    v[7] = _mm_setzero_ps();
    w[0] = nph_f32x8_lo(b->wx);
    w[1] = nph_f32x8_lo(b->wy);
    w[2] = nph_f32x8_lo(b->wz);
    w[3] = _mm_setzero_ps();
    w[4] = nph_f32x8_hi(b->wx);
    w[5] = nph_f32x8_hi(b->wy);
    w[6] = nph_f32x8_hi(b->wz);
    w[7] = _mm_setzero_ps();
    _MM_TRANSPOSE4_PS(v[0], v[1], v[2], v[3]);
    _MM_TRANSPOSE4_PS(v[4], v[5], v[6], v[7]);
    _MM_TRANSPOSE4_PS(w[0], w[1], w[2], w[3]);
    _MM_TRANSPOSE4_PS(w[4], w[5], w[6], w[7]);
    for (u32 l = 0U; l < PHYS_LANES; ++l) {
        if ((mask & (1U << l)) != 0U) {
            _mm_storeu_ps(bodies[index[l]].rows[0], v[l]);
            _mm_storeu_ps(bodies[index[l]].rows[1], w[l]);
        }
    }
}

static W8 PHYS_KERNEL(dot)(W8 ax, W8 ay, W8 az, W8 bx, W8 by, W8 bz)
{
    return nph_f32x8_add(nph_f32x8_add(nph_f32x8_mul(ax, bx), nph_f32x8_mul(ay, by)),
                         nph_f32x8_mul(az, bz));
}

// (w x r) for the lanes.
static void PHYS_KERNEL(cross)(W8 ax, W8 ay, W8 az, W8 bx, W8 by, W8 bz, W8 *ox, W8 *oy, W8 *oz)
{
    *ox = nph_f32x8_sub(nph_f32x8_mul(ay, bz), nph_f32x8_mul(az, by));
    *oy = nph_f32x8_sub(nph_f32x8_mul(az, bx), nph_f32x8_mul(ax, bz));
    *oz = nph_f32x8_sub(nph_f32x8_mul(ax, by), nph_f32x8_mul(ay, bx));
}

// The relative velocity at a point: vB + wB x rB - vA - wA x rA.
static void PHYS_KERNEL(relative_velocity)(const BodyLanes *a, const BodyLanes *b,
                                           const PhysPointLanes *p, W8 *dx, W8 *dy, W8 *dz)
{
    const W8 rax = PHYS_KERNEL(load)(p->anchor_a[0]);
    const W8 ray = PHYS_KERNEL(load)(p->anchor_a[1]);
    const W8 raz = PHYS_KERNEL(load)(p->anchor_a[2]);
    const W8 rbx = PHYS_KERNEL(load)(p->anchor_b[0]);
    const W8 rby = PHYS_KERNEL(load)(p->anchor_b[1]);
    const W8 rbz = PHYS_KERNEL(load)(p->anchor_b[2]);
    W8 cax;
    W8 cay;
    W8 caz;
    W8 cbx;
    W8 cby;
    W8 cbz;
    PHYS_KERNEL(cross)(a->wx, a->wy, a->wz, rax, ray, raz, &cax, &cay, &caz);
    PHYS_KERNEL(cross)(b->wx, b->wy, b->wz, rbx, rby, rbz, &cbx, &cby, &cbz);
    *dx = nph_f32x8_sub(nph_f32x8_add(b->vx, cbx), nph_f32x8_add(a->vx, cax));
    *dy = nph_f32x8_sub(nph_f32x8_add(b->vy, cby), nph_f32x8_add(a->vy, cay));
    *dz = nph_f32x8_sub(nph_f32x8_add(b->vz, cbz), nph_f32x8_add(a->vz, caz));
}

// An impulse `lambda` along axis (x, y, z): A loses it, B gains it; the
// angular parts come premultiplied by the inverse inertia, each three rows
// of lanes (x, y, z) from `ang_a` and `ang_b` on.
static void PHYS_KERNEL(apply)(BodyLanes *a, BodyLanes *b, W8 ma, W8 mb, W8 lambda, W8 x, W8 y,
                               W8 z, const f32 *ang_a, const f32 *ang_b)
{
    const W8 la = nph_f32x8_mul(ma, lambda);
    const W8 lb = nph_f32x8_mul(mb, lambda);
    a->vx = nph_f32x8_sub(a->vx, nph_f32x8_mul(la, x));
    a->vy = nph_f32x8_sub(a->vy, nph_f32x8_mul(la, y));
    a->vz = nph_f32x8_sub(a->vz, nph_f32x8_mul(la, z));
    b->vx = nph_f32x8_add(b->vx, nph_f32x8_mul(lb, x));
    b->vy = nph_f32x8_add(b->vy, nph_f32x8_mul(lb, y));
    b->vz = nph_f32x8_add(b->vz, nph_f32x8_mul(lb, z));
    const f32 *a_y = ang_a + PHYS_LANES;
    const f32 *a_z = a_y + PHYS_LANES;
    const f32 *b_y = ang_b + PHYS_LANES;
    const f32 *b_z = b_y + PHYS_LANES;
    a->wx = nph_f32x8_sub(a->wx, nph_f32x8_mul(lambda, PHYS_KERNEL(load)(ang_a)));
    a->wy = nph_f32x8_sub(a->wy, nph_f32x8_mul(lambda, PHYS_KERNEL(load)(a_y)));
    a->wz = nph_f32x8_sub(a->wz, nph_f32x8_mul(lambda, PHYS_KERNEL(load)(a_z)));
    b->wx = nph_f32x8_add(b->wx, nph_f32x8_mul(lambda, PHYS_KERNEL(load)(ang_b)));
    b->wy = nph_f32x8_add(b->wy, nph_f32x8_mul(lambda, PHYS_KERNEL(load)(b_y)));
    b->wz = nph_f32x8_add(b->wz, nph_f32x8_mul(lambda, PHYS_KERNEL(load)(b_z)));
}

// Each point starts the substep with the impulses it ended the last with
// (Catto, "Iterative Dynamics with Temporal Coherence", GDC 2005). Under
// TGS, after a hit (`impacted`: a point approached faster than
// PHYS_IMPACT_SPEED, or bounced), with as much of them as pushes none of
// the lane's points apart: what stopped an approach that is over launches
// the bodies. A box stopped dead on a floor keeps its weight's worth, a
// heavy body still coming down on a light one the impulses that slow it,
// and a bounce none. Four points share a load in many ways, and rigid rows
// cannot take back a share that moves nothing: warm-starting all of a hit
// tipped boxes dropped flat from a metre over (prepare_group says why the
// soft step warm-starts all of it).
static void PHYS_KERNEL(warm_start)(PhysSolverBody *bodies, PhysContactLanes *g,
                                    PhysPointLanes *points)
{
    BodyLanes a;
    BodyLanes b;
    PHYS_KERNEL(gather)(bodies, g->body_a, false, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, false, &b);
    const W8 zero = nph_f32x8_zero();
    const W8 one = nph_f32x8_splat(1.0F);
    const W8 ma = PHYS_KERNEL(load)(g->inv_mass_a);
    const W8 mb = PHYS_KERNEL(load)(g->inv_mass_b);
    const W8 nx = PHYS_KERNEL(load)(g->normal[0]);
    const W8 ny = PHYS_KERNEL(load)(g->normal[1]);
    const W8 nz = PHYS_KERNEL(load)(g->normal[2]);
    const W8 t1x = PHYS_KERNEL(load)(g->tangent1[0]);
    const W8 t1y = PHYS_KERNEL(load)(g->tangent1[1]);
    const W8 t1z = PHYS_KERNEL(load)(g->tangent1[2]);
    const W8 t2x = PHYS_KERNEL(load)(g->tangent2[0]);
    const W8 t2y = PHYS_KERNEL(load)(g->tangent2[1]);
    const W8 t2z = PHYS_KERNEL(load)(g->tangent2[2]);
    const W8 hit = PHYS_KERNEL(load)(g->impacted);
    const bool any_hit = nph_f32x8_movemask(hit) != 0U;
    W8 scale = one;
    if (any_hit) {
        // The normal speeds now and after the whole warm start (on copies:
        // impulses add linearly), and the share of it that pushes no point
        // apart faster than it was already moving apart.
        BodyLanes a1 = a;
        BodyLanes b1 = b;
        for (u32 k = 0U; k < g->point_count; ++k) {
            const PhysPointLanes *p = &points[k];
            PHYS_KERNEL(apply)(&a1, &b1, ma, mb, PHYS_KERNEL(load)(p->normal_impulse), nx, ny, nz,
                               p->ang_a_n[0], p->ang_b_n[0]);
            PHYS_KERNEL(apply)(&a1, &b1, ma, mb, PHYS_KERNEL(load)(p->tangent_impulse[0]), t1x, t1y,
                               t1z, p->ang_a_t1[0], p->ang_b_t1[0]);
            PHYS_KERNEL(apply)(&a1, &b1, ma, mb, PHYS_KERNEL(load)(p->tangent_impulse[1]), t2x, t2y,
                               t2z, p->ang_a_t2[0], p->ang_b_t2[0]);
        }
        W8 share = one;
        for (u32 k = 0U; k < g->point_count; ++k) {
            const PhysPointLanes *p = &points[k];
            W8 vx;
            W8 vy;
            W8 vz;
            PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
            const W8 before = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
            PHYS_KERNEL(relative_velocity)(&a1, &b1, p, &vx, &vy, &vz);
            const W8 after = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
            const W8 allowed = nph_f32x8_max(before, zero);
            const W8 pushed = nph_f32x8_cmp_gt(after, allowed);
            const W8 part =
                nph_f32x8_div(nph_f32x8_sub(allowed, before),
                              nph_f32x8_select(pushed, nph_f32x8_sub(after, before), one));
            share = nph_f32x8_min(share, nph_f32x8_select(pushed, part, one));
        }
        scale = nph_f32x8_select(hit, share, one);
        nph_f32x8_store(g->impacted, zero);
    }
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        W8 normal = PHYS_KERNEL(load)(p->normal_impulse);
        W8 tangent1 = PHYS_KERNEL(load)(p->tangent_impulse[0]);
        W8 tangent2 = PHYS_KERNEL(load)(p->tangent_impulse[1]);
        if (any_hit) {
            normal = nph_f32x8_mul(normal, scale);
            tangent1 = nph_f32x8_mul(tangent1, scale);
            tangent2 = nph_f32x8_mul(tangent2, scale);
            nph_f32x8_store(p->normal_impulse, normal);
            nph_f32x8_store(p->tangent_impulse[0], tangent1);
            nph_f32x8_store(p->tangent_impulse[1], tangent2);
        }
        PHYS_KERNEL(apply)(&a, &b, ma, mb, normal, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, tangent1, t1x, t1y, t1z, p->ang_a_t1[0], p->ang_b_t1[0]);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, tangent2, t2x, t2y, t2z, p->ang_a_t2[0], p->ang_b_t2[0]);
    }
    PHYS_KERNEL(scatter)(bodies, g->body_a, g->write_mask_a, &a);
    PHYS_KERNEL(scatter)(bodies, g->body_b, g->write_mask_b, &b);
}

// m v for the lanes' matrices (column-major, column * 3 + row).
static void PHYS_KERNEL(mat3_mul)(const W8 m[9], const W8 v[3], W8 out[3])
{
    for (u32 r = 0U; r < 3U; ++r) {
        out[r] =
            nph_f32x8_add(nph_f32x8_add(nph_f32x8_mul(m[r], v[0]), nph_f32x8_mul(m[3U + r], v[1])),
                          nph_f32x8_mul(m[6U + r], v[2]));
    }
}

// Derives a group's point rows from what prepare_group gathered (the
// anchors, raw separations, and last step's normal impulses in the rows;
// the inverse inertias, tangent impulses, and point masks in `in`): the
// angular terms I^-1 (r x axis), the effective masses, the approach speed
// for restitution, and the warm-start impulses on this step's tangents.
// Points a lane does not have keep zero masses and change nothing.
static void PHYS_KERNEL(prepare)(const PhysSolverBody *bodies, PhysContactLanes *g,
                                 PhysPointLanes *points, const PhysPrepareLanes *in)
{
    BodyLanes a;
    BodyLanes b;
    PHYS_KERNEL(gather)(bodies, g->body_a, false, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, false, &b);
    const W8 zero = nph_f32x8_zero();
    const W8 one = nph_f32x8_splat(1.0F);
    const W8 mass =
        nph_f32x8_add(PHYS_KERNEL(load)(g->inv_mass_a), PHYS_KERNEL(load)(g->inv_mass_b));
    W8 axes[3][3]; // the normal and the tangents, by component
    for (u32 c = 0U; c < 3U; ++c) {
        axes[0][c] = PHYS_KERNEL(load)(g->normal[c]);
        axes[1][c] = PHYS_KERNEL(load)(g->tangent1[c]);
        axes[2][c] = PHYS_KERNEL(load)(g->tangent2[c]);
    }
    W8 ia[9];
    W8 ib[9];
    for (u32 e = 0U; e < 9U; ++e) {
        ia[e] = PHYS_KERNEL(load)(in->inv_inertia_a[e]);
        ib[e] = PHYS_KERNEL(load)(in->inv_inertia_b[e]);
    }
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        const W8 active = nph_f32x8_cmp_gt(PHYS_KERNEL(load)(in->active[k]), zero);
        W8 ra[3];
        W8 rb[3];
        for (u32 c = 0U; c < 3U; ++c) {
            ra[c] = PHYS_KERNEL(load)(p->anchor_a[c]);
            rb[c] = PHYS_KERNEL(load)(p->anchor_b[c]);
        }
        // The separation less the anchors' gap along the normal: the solve
        // adds the gap back as the bodies move.
        const W8 gap =
            PHYS_KERNEL(dot)(nph_f32x8_sub(rb[0], ra[0]), nph_f32x8_sub(rb[1], ra[1]),
                             nph_f32x8_sub(rb[2], ra[2]), axes[0][0], axes[0][1], axes[0][2]);
        nph_f32x8_store(p->adjusted_separation,
                        nph_f32x8_sub(PHYS_KERNEL(load)(p->adjusted_separation), gap));
        f32(*const ang_a[3])[PHYS_LANES] = {p->ang_a_n, p->ang_a_t1, p->ang_a_t2};
        f32(*const ang_b[3])[PHYS_LANES] = {p->ang_b_n, p->ang_b_t1, p->ang_b_t2};
        W8 rxa[3][3]; // r x axis, per axis
        W8 rxb[3][3];
        W8 aa[3][3]; // I^-1 (r x axis)
        W8 ab[3][3];
        for (u32 x = 0U; x < 3U; ++x) {
            PHYS_KERNEL(cross)(ra[0], ra[1], ra[2], axes[x][0], axes[x][1], axes[x][2], &rxa[x][0],
                               &rxa[x][1], &rxa[x][2]);
            PHYS_KERNEL(cross)(rb[0], rb[1], rb[2], axes[x][0], axes[x][1], axes[x][2], &rxb[x][0],
                               &rxb[x][1], &rxb[x][2]);
            PHYS_KERNEL(mat3_mul)(ia, rxa[x], aa[x]);
            PHYS_KERNEL(mat3_mul)(ib, rxb[x], ab[x]);
            for (u32 c = 0U; c < 3U; ++c) {
                nph_f32x8_store(ang_a[x][c], aa[x][c]);
                nph_f32x8_store(ang_b[x][c], ab[x][c]);
            }
        }
        const W8 kn = nph_f32x8_add(
            nph_f32x8_add(mass, PHYS_KERNEL(dot)(rxa[0][0], rxa[0][1], rxa[0][2], aa[0][0],
                                                 aa[0][1], aa[0][2])),
            PHYS_KERNEL(dot)(rxb[0][0], rxb[0][1], rxb[0][2], ab[0][0], ab[0][1], ab[0][2]));
        nph_f32x8_store(p->normal_mass,
                        nph_f32x8_select(nph_f32x8_and(active, nph_f32x8_cmp_gt(kn, zero)),
                                         nph_f32x8_div(one, kn), zero));
        // The 2x2 effective mass of the two tangents (the linear parts
        // cancel off the diagonal: t1 and t2 are orthogonal), inverted.
        const W8 k11 = nph_f32x8_add(
            nph_f32x8_add(mass, PHYS_KERNEL(dot)(rxa[1][0], rxa[1][1], rxa[1][2], aa[1][0],
                                                 aa[1][1], aa[1][2])),
            PHYS_KERNEL(dot)(rxb[1][0], rxb[1][1], rxb[1][2], ab[1][0], ab[1][1], ab[1][2]));
        const W8 k22 = nph_f32x8_add(
            nph_f32x8_add(mass, PHYS_KERNEL(dot)(rxa[2][0], rxa[2][1], rxa[2][2], aa[2][0],
                                                 aa[2][1], aa[2][2])),
            PHYS_KERNEL(dot)(rxb[2][0], rxb[2][1], rxb[2][2], ab[2][0], ab[2][1], ab[2][2]));
        const W8 k12 = nph_f32x8_add(
            PHYS_KERNEL(dot)(rxa[1][0], rxa[1][1], rxa[1][2], aa[2][0], aa[2][1], aa[2][2]),
            PHYS_KERNEL(dot)(rxb[1][0], rxb[1][1], rxb[1][2], ab[2][0], ab[2][1], ab[2][2]));
        const W8 det = nph_f32x8_sub(nph_f32x8_mul(k11, k22), nph_f32x8_mul(k12, k12));
        const W8 inv = nph_f32x8_select(nph_f32x8_and(active, nph_f32x8_cmp_gt(det, zero)),
                                        nph_f32x8_div(one, det), zero);
        nph_f32x8_store(p->tangent_mass[0], nph_f32x8_mul(k22, inv));
        nph_f32x8_store(p->tangent_mass[1], nph_f32x8_mul(nph_f32x8_neg(k12), inv));
        nph_f32x8_store(p->tangent_mass[2], nph_f32x8_mul(k11, inv));
        // The normal approach speed before the step, for restitution.
        W8 vx;
        W8 vy;
        W8 vz;
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        nph_f32x8_store(
            p->relative_velocity,
            nph_f32x8_select(
                active, PHYS_KERNEL(dot)(vx, vy, vz, axes[0][0], axes[0][1], axes[0][2]), zero));
        // Warm start: last step's tangential impulse on this step's tangents.
        const W8 tx = PHYS_KERNEL(load)(in->tangent_impulse[k][0]);
        const W8 ty = PHYS_KERNEL(load)(in->tangent_impulse[k][1]);
        const W8 tz = PHYS_KERNEL(load)(in->tangent_impulse[k][2]);
        nph_f32x8_store(p->tangent_impulse[0],
                        PHYS_KERNEL(dot)(tx, ty, tz, axes[1][0], axes[1][1], axes[1][2]));
        nph_f32x8_store(p->tangent_impulse[1],
                        PHYS_KERNEL(dot)(tx, ty, tz, axes[2][0], axes[2][1], axes[2][2]));
    }
    // Impacts (PHYS_IMPACT_SPEED).
    const W8 impact_speed = nph_f32x8_splat(-PHYS_IMPACT_SPEED);
    W8 impact = zero;
    for (u32 k = 0U; k < g->point_count; ++k) {
        impact = nph_f32x8_or(
            impact, nph_f32x8_cmp_lt(PHYS_KERNEL(load)(points[k].relative_velocity), impact_speed));
    }
    nph_f32x8_store(g->impact_lanes, impact);
}

// The friction circle, |impulse| <= `limit` (mu times the normal impulse),
// for the tangent impulse (new1, new2) that would stop the points' slip
// (vt1, vt2). Inside the circle it stands (sticking). Past it, kinetic
// friction must oppose the slip itself (Coulomb's law, the maximal
// dissipation): scaling the stopping impulse down to the circle keeps its
// direction, which the coupling of rotation in the effective mass turns
// away from the slip (a box sliding on its four corners lost 6 % of its
// friction that way, and spun). The edge's direction blends the two by
// r = limit / |stopping impulse|: hard sliding (r near 0) opposes the slip
// exactly, and barely past the circle (r near 1) it stays the stopping
// impulse's, continuous with sticking, where the slip's own direction is
// noise. The two are less than 90 degrees apart (K is positive definite),
// so the blend never vanishes.
static void PHYS_KERNEL(friction_circle)(W8 vt1, W8 vt2, W8 limit, W8 *new1, W8 *new2)
{
    const W8 zero = nph_f32x8_zero();
    const W8 one = nph_f32x8_splat(1.0F);
    const W8 len =
        nph_f32x8_sqrt(nph_f32x8_add(nph_f32x8_mul(*new1, *new1), nph_f32x8_mul(*new2, *new2)));
    const W8 over = nph_f32x8_cmp_gt(len, limit);
    const W8 slip = nph_f32x8_sqrt(nph_f32x8_add(nph_f32x8_mul(vt1, vt1), nph_f32x8_mul(vt2, vt2)));
    const W8 sliding = nph_f32x8_and(over, nph_f32x8_cmp_gt(slip, zero));
    const W8 r = nph_f32x8_div(limit, nph_f32x8_select(over, len, one));
    const W8 stop_share = nph_f32x8_div(r, nph_f32x8_select(over, len, one));
    const W8 slip_share =
        nph_f32x8_div(nph_f32x8_sub(one, r), nph_f32x8_select(sliding, slip, one));
    const W8 d1 = nph_f32x8_sub(nph_f32x8_mul(stop_share, *new1), nph_f32x8_mul(slip_share, vt1));
    const W8 d2 = nph_f32x8_sub(nph_f32x8_mul(stop_share, *new2), nph_f32x8_mul(slip_share, vt2));
    const W8 dlen = nph_f32x8_sqrt(nph_f32x8_add(nph_f32x8_mul(d1, d1), nph_f32x8_mul(d2, d2)));
    const W8 to_edge = nph_f32x8_div(limit, nph_f32x8_select(sliding, dlen, one));
    *new1 = nph_f32x8_select(sliding, nph_f32x8_mul(d1, to_edge),
                             nph_f32x8_select(over, nph_f32x8_mul(*new1, r), *new1));
    *new2 = nph_f32x8_select(sliding, nph_f32x8_mul(d2, to_edge),
                             nph_f32x8_select(over, nph_f32x8_mul(*new2, r), *new2));
}

// v rotated by the lanes' quaternions: v + 2 w (u x v) + 2 u x (u x v), as
// nph_quat_rotate.
static void PHYS_KERNEL(rotate)(const BodyLanes *q, W8 vx, W8 vy, W8 vz, W8 *ox, W8 *oy, W8 *oz)
{
    const W8 two = nph_f32x8_splat(2.0F);
    W8 tx;
    W8 ty;
    W8 tz;
    PHYS_KERNEL(cross)(q->qx, q->qy, q->qz, vx, vy, vz, &tx, &ty, &tz);
    tx = nph_f32x8_mul(tx, two);
    ty = nph_f32x8_mul(ty, two);
    tz = nph_f32x8_mul(tz, two);
    W8 cx;
    W8 cy;
    W8 cz;
    PHYS_KERNEL(cross)(q->qx, q->qy, q->qz, tx, ty, tz, &cx, &cy, &cz);
    *ox = nph_f32x8_add(nph_f32x8_add(vx, nph_f32x8_mul(tx, q->qw)), cx);
    *oy = nph_f32x8_add(nph_f32x8_add(vy, nph_f32x8_mul(ty, q->qw)), cy);
    *oz = nph_f32x8_add(nph_f32x8_add(vz, nph_f32x8_mul(tz, q->qw)), cz);
}

// A point's current separation: the bodies' displacements this step move
// the fixed anchors (rotated by the bodies' rotations this step).
static W8 PHYS_KERNEL(separation)(const BodyLanes *a, const BodyLanes *b, const PhysPointLanes *p,
                                  W8 nx, W8 ny, W8 nz)
{
    W8 pax;
    W8 pay;
    W8 paz;
    W8 pbx;
    W8 pby;
    W8 pbz;
    PHYS_KERNEL(rotate)(a, PHYS_KERNEL(load)(p->anchor_a[0]), PHYS_KERNEL(load)(p->anchor_a[1]),
                        PHYS_KERNEL(load)(p->anchor_a[2]), &pax, &pay, &paz);
    PHYS_KERNEL(rotate)(b, PHYS_KERNEL(load)(p->anchor_b[0]), PHYS_KERNEL(load)(p->anchor_b[1]),
                        PHYS_KERNEL(load)(p->anchor_b[2]), &pbx, &pby, &pbz);
    const W8 dx = nph_f32x8_add(nph_f32x8_sub(b->dpx, a->dpx), nph_f32x8_sub(pbx, pax));
    const W8 dy = nph_f32x8_add(nph_f32x8_sub(b->dpy, a->dpy), nph_f32x8_sub(pby, pay));
    const W8 dz = nph_f32x8_add(nph_f32x8_sub(b->dpz, a->dpz), nph_f32x8_sub(pbz, paz));
    return nph_f32x8_add(PHYS_KERNEL(dot)(dx, dy, dz, nx, ny, nz),
                         PHYS_KERNEL(load)(p->adjusted_separation));
}

// A normal row at separation `s`: apart, it closes at most the gap
// (speculative); overlapping, the spring's push, capped, with its softness,
// or rigid when relaxing (the soft terms one, zero, zero). `slop` (TGS's
// Baumgarte slop; 0 for the soft step) is the overlap the bias leaves
// alone.
static void PHYS_KERNEL(normal_row)(W8 s, W8 inv_h, W8 slop, W8 soft_bias, W8 push_max,
                                    W8 soft_mass, W8 soft_impulse, W8 *bias, W8 *mass_scale,
                                    W8 *impulse_scale)
{
    const W8 zero = nph_f32x8_zero();
    const W8 apart = nph_f32x8_cmp_gt(s, zero);
    const W8 overlap = nph_f32x8_min(nph_f32x8_add(s, slop), zero);
    *bias = nph_f32x8_select(apart, nph_f32x8_mul(s, inv_h),
                             nph_f32x8_max(nph_f32x8_mul(soft_bias, overlap), push_max));
    *mass_scale = nph_f32x8_select(apart, nph_f32x8_splat(1.0F), soft_mass);
    *impulse_scale = nph_f32x8_select(apart, zero, soft_impulse);
}

static void PHYS_KERNEL(solve)(PhysSolverBody *bodies, PhysContactLanes *g, PhysPointLanes *points,
                               f32 inv_h, f32 push_max, f32 slop, bool use_bias)
{
    BodyLanes a;
    BodyLanes b;
    PHYS_KERNEL(gather)(bodies, g->body_a, true, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, true, &b);
    const W8 zero = nph_f32x8_zero();
    const W8 one = nph_f32x8_splat(1.0F);
    const W8 ma = PHYS_KERNEL(load)(g->inv_mass_a);
    const W8 mb = PHYS_KERNEL(load)(g->inv_mass_b);
    const W8 nx = PHYS_KERNEL(load)(g->normal[0]);
    const W8 ny = PHYS_KERNEL(load)(g->normal[1]);
    const W8 nz = PHYS_KERNEL(load)(g->normal[2]);
    const W8 vinv_h = nph_f32x8_splat(inv_h);
    const W8 min_bias = nph_f32x8_splat(-push_max);
    const W8 vslop = nph_f32x8_splat(slop);
    const W8 soft_bias = use_bias ? PHYS_KERNEL(load)(g->bias_rate) : zero;
    const W8 soft_mass = use_bias ? PHYS_KERNEL(load)(g->mass_scale) : one;
    const W8 soft_impulse = use_bias ? PHYS_KERNEL(load)(g->impulse_scale) : zero;
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        W8 bias;
        W8 mass_scale;
        W8 impulse_scale;
        PHYS_KERNEL(normal_row)(PHYS_KERNEL(separation)(&a, &b, p, nx, ny, nz), vinv_h, vslop,
                                soft_bias, min_bias, soft_mass, soft_impulse, &bias, &mass_scale,
                                &impulse_scale);
        W8 vx;
        W8 vy;
        W8 vz;
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        const W8 vn = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
        const W8 row = nph_f32x8_mul(PHYS_KERNEL(load)(p->normal_mass), mass_scale);
        const W8 accumulated = PHYS_KERNEL(load)(p->normal_impulse);
        const W8 impulse = nph_f32x8_sub(nph_f32x8_neg(nph_f32x8_mul(row, nph_f32x8_add(vn, bias))),
                                         nph_f32x8_mul(impulse_scale, accumulated));
        const W8 total = nph_f32x8_max(nph_f32x8_add(accumulated, impulse), zero);
        const W8 delta = nph_f32x8_sub(total, accumulated);
        nph_f32x8_store(p->normal_impulse, total);
        nph_f32x8_store(p->max_normal_impulse,
                        nph_f32x8_max(PHYS_KERNEL(load)(p->max_normal_impulse), total));
        PHYS_KERNEL(apply)(&a, &b, ma, mb, delta, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
    }
    const W8 impact = PHYS_KERNEL(load)(g->impact_lanes);
    // Under TGS a hit scales the next warm start (warm_start).
    if (g->scale_hits != 0U && nph_f32x8_movemask(impact) != 0U) {
        nph_f32x8_store(g->impacted, nph_f32x8_or(PHYS_KERNEL(load)(g->impacted), impact));
    }
    if (g->point_count > 1U && nph_f32x8_movemask(impact) != 0U) {
        // The rows as the pass made them (the bodies have not moved).
        W8 row_bias[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
        W8 row_mass[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
        W8 row_soft[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
        for (u32 k = 0U; k < g->point_count; ++k) {
            W8 mass_scale;
            PHYS_KERNEL(normal_row)(PHYS_KERNEL(separation)(&a, &b, &points[k], nx, ny, nz), vinv_h,
                                    vslop, soft_bias, min_bias, soft_mass, soft_impulse,
                                    &row_bias[k], &mass_scale, &row_soft[k]);
            row_mass[k] = nph_f32x8_mul(PHYS_KERNEL(load)(points[k].normal_mass), mass_scale);
        }
        for (u32 sweep = 0U; sweep < PHYS_IMPACT_SWEEPS; ++sweep) {
            for (u32 k = 0U; k < g->point_count; ++k) {
                PhysPointLanes *p = &points[k];
                W8 vx;
                W8 vy;
                W8 vz;
                PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
                const W8 vn = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
                const W8 accumulated = PHYS_KERNEL(load)(p->normal_impulse);
                const W8 impulse = nph_f32x8_sub(
                    nph_f32x8_neg(nph_f32x8_mul(row_mass[k], nph_f32x8_add(vn, row_bias[k]))),
                    nph_f32x8_mul(row_soft[k], accumulated));
                const W8 total = nph_f32x8_select(
                    impact, nph_f32x8_max(nph_f32x8_add(accumulated, impulse), zero), accumulated);
                const W8 delta = nph_f32x8_sub(total, accumulated);
                nph_f32x8_store(p->normal_impulse, total);
                nph_f32x8_store(p->max_normal_impulse,
                                nph_f32x8_max(PHYS_KERNEL(load)(p->max_normal_impulse), total));
                PHYS_KERNEL(apply)(&a, &b, ma, mb, delta, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
            }
        }
    }
    const W8 t1x = PHYS_KERNEL(load)(g->tangent1[0]);
    const W8 t1y = PHYS_KERNEL(load)(g->tangent1[1]);
    const W8 t1z = PHYS_KERNEL(load)(g->tangent1[2]);
    const W8 t2x = PHYS_KERNEL(load)(g->tangent2[0]);
    const W8 t2y = PHYS_KERNEL(load)(g->tangent2[1]);
    const W8 t2z = PHYS_KERNEL(load)(g->tangent2[2]);
    const W8 friction = PHYS_KERNEL(load)(g->friction);
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        W8 vx;
        W8 vy;
        W8 vz;
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        const W8 vt1 = PHYS_KERNEL(dot)(vx, vy, vz, t1x, t1y, t1z);
        const W8 vt2 = PHYS_KERNEL(dot)(vx, vy, vz, t2x, t2y, t2z);
        const W8 m00 = PHYS_KERNEL(load)(p->tangent_mass[0]);
        const W8 m01 = PHYS_KERNEL(load)(p->tangent_mass[1]);
        const W8 m11 = PHYS_KERNEL(load)(p->tangent_mass[2]);
        const W8 old1 = PHYS_KERNEL(load)(p->tangent_impulse[0]);
        const W8 old2 = PHYS_KERNEL(load)(p->tangent_impulse[1]);
        W8 new1 =
            nph_f32x8_sub(old1, nph_f32x8_add(nph_f32x8_mul(m00, vt1), nph_f32x8_mul(m01, vt2)));
        W8 new2 =
            nph_f32x8_sub(old2, nph_f32x8_add(nph_f32x8_mul(m01, vt1), nph_f32x8_mul(m11, vt2)));
        PHYS_KERNEL(friction_circle)(
            vt1, vt2, nph_f32x8_mul(friction, PHYS_KERNEL(load)(p->normal_impulse)), &new1, &new2);
        nph_f32x8_store(p->tangent_impulse[0], new1);
        nph_f32x8_store(p->tangent_impulse[1], new2);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, nph_f32x8_sub(new1, old1), t1x, t1y, t1z, p->ang_a_t1[0],
                           p->ang_b_t1[0]);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, nph_f32x8_sub(new2, old2), t2x, t2y, t2z, p->ang_a_t2[0],
                           p->ang_b_t2[0]);
    }
    PHYS_KERNEL(scatter)(bodies, g->body_a, g->write_mask_a, &a);
    PHYS_KERNEL(scatter)(bodies, g->body_b, g->write_mask_b, &b);
}

// Restitution after the substeps: points that approached faster than the
// threshold and carried load leave at the pair's restitution of that speed
// (Box2D v3's). Points still a little apart count: the relax pass held their
// approach to the gap, slowing them before they touched, and a ball left to
// land on the next step came back at half its speed. One pass over a
// manifold's points stops them one at a time, which spun boxes landing flat
// (5 rad/s from half a metre); more sweeps over those lanes converge them.
static void PHYS_KERNEL(restitution)(PhysSolverBody *bodies, PhysContactLanes *g,
                                     PhysPointLanes *points, f32 threshold)
{
    const W8 zero = nph_f32x8_zero();
    const W8 ma = PHYS_KERNEL(load)(g->inv_mass_a);
    const W8 mb = PHYS_KERNEL(load)(g->inv_mass_b);
    const W8 nx = PHYS_KERNEL(load)(g->normal[0]);
    const W8 ny = PHYS_KERNEL(load)(g->normal[1]);
    const W8 nz = PHYS_KERNEL(load)(g->normal[2]);
    const W8 e = PHYS_KERNEL(load)(g->restitution);
    const W8 vthreshold = nph_f32x8_splat(-threshold);
    W8 loaded[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
    W8 bounced = zero;
    for (u32 k = 0U; k < g->point_count; ++k) {
        loaded[k] = nph_f32x8_and(
            nph_f32x8_cmp_lt(PHYS_KERNEL(load)(points[k].relative_velocity), vthreshold),
            nph_f32x8_cmp_gt(PHYS_KERNEL(load)(points[k].max_normal_impulse), zero));
        bounced = nph_f32x8_or(bounced, loaded[k]);
    }
    if (nph_f32x8_movemask(bounced) == 0U) {
        return;
    }
    BodyLanes a;
    BodyLanes b;
    PHYS_KERNEL(gather)(bodies, g->body_a, false, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, false, &b);
    const u32 sweeps = g->point_count > 1U ? 1U + PHYS_IMPACT_SWEEPS : 1U;
    for (u32 sweep = 0U; sweep < sweeps; ++sweep) {
        for (u32 k = 0U; k < g->point_count; ++k) {
            PhysPointLanes *p = &points[k];
            if (nph_f32x8_movemask(loaded[k]) == 0U) {
                continue;
            }
            W8 vx;
            W8 vy;
            W8 vz;
            PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
            const W8 vn = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
            const W8 rel = PHYS_KERNEL(load)(p->relative_velocity);
            const W8 accumulated = PHYS_KERNEL(load)(p->normal_impulse);
            const W8 impulse = nph_f32x8_neg(nph_f32x8_mul(
                PHYS_KERNEL(load)(p->normal_mass), nph_f32x8_add(vn, nph_f32x8_mul(e, rel))));
            const W8 total = nph_f32x8_max(nph_f32x8_add(accumulated, impulse), zero);
            const W8 delta = nph_f32x8_select(loaded[k], nph_f32x8_sub(total, accumulated), zero);
            nph_f32x8_store(p->normal_impulse, nph_f32x8_add(accumulated, delta));
            nph_f32x8_store(p->max_normal_impulse,
                            nph_f32x8_max(PHYS_KERNEL(load)(p->max_normal_impulse),
                                          nph_f32x8_add(accumulated, delta)));
            PHYS_KERNEL(apply)(&a, &b, ma, mb, delta, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
        }
    }
    // A bounce is the step's alone, not the next step's warm start.
    if (g->scale_hits != 0U) {
        nph_f32x8_store(g->impacted, nph_f32x8_or(PHYS_KERNEL(load)(g->impacted), bounced));
    }
    PHYS_KERNEL(scatter)(bodies, g->body_a, g->write_mask_a, &a);
    PHYS_KERNEL(scatter)(bodies, g->body_b, g->write_mask_b, &b);
}

// ---------------------------------------------------------------------------
// XPBD (Mueller, Macklin, Chentanez, Jeschke, and Kim, "Detailed Rigid Body
// Simulation with Extended Position Based Dynamics", SCA 2020): the
// comparison candidate of ADR-0045. Positions move by the constraints'
// corrections, velocities follow from the positions, and a velocity pass
// then applies dynamic friction and restitution.
// ---------------------------------------------------------------------------

// The bodies' displacements and rotations (rows 2 and 3) alone.
static void PHYS_KERNEL(gather_positions)(const PhysSolverBody *bodies, const u32 *index,
                                          BodyLanes *out)
{
    PHYS_KERNEL(transpose_row)(bodies, index, 2U, &out->dpx, &out->dpy, &out->dpz, NULL);
    PHYS_KERNEL(transpose_row)(bodies, index, 3U, &out->qx, &out->qy, &out->qz, &out->qw);
}

// Writes the displacements and rotations of the lanes in `mask` back.
static void PHYS_KERNEL(scatter_positions)(PhysSolverBody *bodies, const u32 *index, u32 mask,
                                           const BodyLanes *b)
{
    NphF32x4 p[8];
    NphF32x4 q[8];
    p[0] = nph_f32x8_lo(b->dpx);
    p[1] = nph_f32x8_lo(b->dpy);
    p[2] = nph_f32x8_lo(b->dpz);
    p[3] = _mm_setzero_ps();
    p[4] = nph_f32x8_hi(b->dpx);
    p[5] = nph_f32x8_hi(b->dpy);
    p[6] = nph_f32x8_hi(b->dpz);
    p[7] = _mm_setzero_ps();
    q[0] = nph_f32x8_lo(b->qx);
    q[1] = nph_f32x8_lo(b->qy);
    q[2] = nph_f32x8_lo(b->qz);
    q[3] = nph_f32x8_lo(b->qw);
    q[4] = nph_f32x8_hi(b->qx);
    q[5] = nph_f32x8_hi(b->qy);
    q[6] = nph_f32x8_hi(b->qz);
    q[7] = nph_f32x8_hi(b->qw);
    _MM_TRANSPOSE4_PS(p[0], p[1], p[2], p[3]);
    _MM_TRANSPOSE4_PS(p[4], p[5], p[6], p[7]);
    _MM_TRANSPOSE4_PS(q[0], q[1], q[2], q[3]);
    _MM_TRANSPOSE4_PS(q[4], q[5], q[6], q[7]);
    for (u32 l = 0U; l < PHYS_LANES; ++l) {
        if ((mask & (1U << l)) != 0U) {
            _mm_storeu_ps(bodies[index[l]].rows[2], p[l]);
            _mm_storeu_ps(bodies[index[l]].rows[3], q[l]);
        }
    }
}

// The lanes' rotations turned by the small rotation vector (tx, ty, tz):
// q + (t, 0) q / 2, normalized.
static void PHYS_KERNEL(turn)(BodyLanes *q, W8 tx, W8 ty, W8 tz)
{
    const W8 half = nph_f32x8_splat(0.5F);
    const W8 one = nph_f32x8_splat(1.0F);
    const W8 x = q->qx;
    const W8 y = q->qy;
    const W8 z = q->qz;
    const W8 w = q->qw;
    const W8 nx = nph_f32x8_add(
        x, nph_f32x8_mul(half,
                         nph_f32x8_add(nph_f32x8_mul(tx, w),
                                       nph_f32x8_sub(nph_f32x8_mul(ty, z), nph_f32x8_mul(tz, y)))));
    const W8 ny = nph_f32x8_add(
        y, nph_f32x8_mul(half,
                         nph_f32x8_add(nph_f32x8_mul(ty, w),
                                       nph_f32x8_sub(nph_f32x8_mul(tz, x), nph_f32x8_mul(tx, z)))));
    const W8 nz = nph_f32x8_add(
        z, nph_f32x8_mul(half,
                         nph_f32x8_add(nph_f32x8_mul(tz, w),
                                       nph_f32x8_sub(nph_f32x8_mul(tx, y), nph_f32x8_mul(ty, x)))));
    const W8 nw = nph_f32x8_sub(w, nph_f32x8_mul(half, PHYS_KERNEL(dot)(tx, ty, tz, x, y, z)));
    const W8 len_sq =
        nph_f32x8_add(PHYS_KERNEL(dot)(nx, ny, nz, nx, ny, nz), nph_f32x8_mul(nw, nw));
    const W8 inv = nph_f32x8_div(one, nph_f32x8_sqrt(len_sq));
    q->qx = nph_f32x8_mul(nx, inv);
    q->qy = nph_f32x8_mul(ny, inv);
    q->qz = nph_f32x8_mul(nz, inv);
    q->qw = nph_f32x8_mul(nw, inv);
}

// A positional impulse `lambda` along (x, y, z) at the anchors: B moves by
// it, A against it, both turning by the premultiplied angular terms.
static void PHYS_KERNEL(move)(BodyLanes *a, BodyLanes *b, W8 ma, W8 mb, W8 lambda, W8 x, W8 y, W8 z,
                              const f32 *ang_a, const f32 *ang_b)
{
    const W8 la = nph_f32x8_mul(ma, lambda);
    const W8 lb = nph_f32x8_mul(mb, lambda);
    a->dpx = nph_f32x8_sub(a->dpx, nph_f32x8_mul(la, x));
    a->dpy = nph_f32x8_sub(a->dpy, nph_f32x8_mul(la, y));
    a->dpz = nph_f32x8_sub(a->dpz, nph_f32x8_mul(la, z));
    b->dpx = nph_f32x8_add(b->dpx, nph_f32x8_mul(lb, x));
    b->dpy = nph_f32x8_add(b->dpy, nph_f32x8_mul(lb, y));
    b->dpz = nph_f32x8_add(b->dpz, nph_f32x8_mul(lb, z));
    const f32 *a_y = ang_a + PHYS_LANES;
    const f32 *a_z = a_y + PHYS_LANES;
    const f32 *b_y = ang_b + PHYS_LANES;
    const f32 *b_z = b_y + PHYS_LANES;
    const W8 neg = nph_f32x8_neg(lambda);
    PHYS_KERNEL(turn)(a, nph_f32x8_mul(neg, PHYS_KERNEL(load)(ang_a)),
                      nph_f32x8_mul(neg, PHYS_KERNEL(load)(a_y)),
                      nph_f32x8_mul(neg, PHYS_KERNEL(load)(a_z)));
    PHYS_KERNEL(turn)(b, nph_f32x8_mul(lambda, PHYS_KERNEL(load)(ang_b)),
                      nph_f32x8_mul(lambda, PHYS_KERNEL(load)(b_y)),
                      nph_f32x8_mul(lambda, PHYS_KERNEL(load)(b_z)));
}

// The anchors' world offsets from the centres now, for bodies `a` and `b`.
static void PHYS_KERNEL(anchors_now)(const BodyLanes *a, const BodyLanes *b,
                                     const PhysPointLanes *p, W8 ra[3], W8 rb[3])
{
    PHYS_KERNEL(rotate)(a, PHYS_KERNEL(load)(p->anchor_a[0]), PHYS_KERNEL(load)(p->anchor_a[1]),
                        PHYS_KERNEL(load)(p->anchor_a[2]), &ra[0], &ra[1], &ra[2]);
    PHYS_KERNEL(rotate)(b, PHYS_KERNEL(load)(p->anchor_b[0]), PHYS_KERNEL(load)(p->anchor_b[1]),
                        PHYS_KERNEL(load)(p->anchor_b[2]), &rb[0], &rb[1], &rb[2]);
}

// The position pass: each point's penetration closed by moving the bodies
// (a rigid constraint, one iteration a substep), then static friction:
// the points' slip this substep cancelled while its multiplier stays inside
// the friction circle of the normal's. The multipliers (an impulse times
// the substep) are this substep's; the pre-solve normal speed is kept for
// restitution.
static void PHYS_KERNEL(xpbd_positions)(PhysSolverBody *bodies, const PhysSolverBody *prev,
                                        PhysContactLanes *g, PhysPointLanes *points)
{
    BodyLanes a;
    BodyLanes b;
    BodyLanes pa;
    BodyLanes pb;
    PHYS_KERNEL(gather)(bodies, g->body_a, true, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, true, &b);
    PHYS_KERNEL(gather_positions)(prev, g->body_a, &pa);
    PHYS_KERNEL(gather_positions)(prev, g->body_b, &pb);
    const W8 zero = nph_f32x8_zero();
    const W8 ma = PHYS_KERNEL(load)(g->inv_mass_a);
    const W8 mb = PHYS_KERNEL(load)(g->inv_mass_b);
    const W8 nx = PHYS_KERNEL(load)(g->normal[0]);
    const W8 ny = PHYS_KERNEL(load)(g->normal[1]);
    const W8 nz = PHYS_KERNEL(load)(g->normal[2]);
    const W8 t1x = PHYS_KERNEL(load)(g->tangent1[0]);
    const W8 t1y = PHYS_KERNEL(load)(g->tangent1[1]);
    const W8 t1z = PHYS_KERNEL(load)(g->tangent1[2]);
    const W8 t2x = PHYS_KERNEL(load)(g->tangent2[0]);
    const W8 t2y = PHYS_KERNEL(load)(g->tangent2[1]);
    const W8 t2z = PHYS_KERNEL(load)(g->tangent2[2]);
    const W8 friction = PHYS_KERNEL(load)(g->friction);
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        W8 vx;
        W8 vy;
        W8 vz;
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        nph_f32x8_store(p->substep_velocity, PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz));
        W8 ra[3];
        W8 rb[3];
        PHYS_KERNEL(anchors_now)(&a, &b, p, ra, rb);
        const W8 dx = nph_f32x8_add(nph_f32x8_sub(b.dpx, a.dpx), nph_f32x8_sub(rb[0], ra[0]));
        const W8 dy = nph_f32x8_add(nph_f32x8_sub(b.dpy, a.dpy), nph_f32x8_sub(rb[1], ra[1]));
        const W8 dz = nph_f32x8_add(nph_f32x8_sub(b.dpz, a.dpz), nph_f32x8_sub(rb[2], ra[2]));
        const W8 s = nph_f32x8_add(PHYS_KERNEL(dot)(dx, dy, dz, nx, ny, nz),
                                   PHYS_KERNEL(load)(p->adjusted_separation));
        const W8 lambda = nph_f32x8_select(
            nph_f32x8_cmp_lt(s, zero),
            nph_f32x8_mul(nph_f32x8_neg(s), PHYS_KERNEL(load)(p->normal_mass)), zero);
        nph_f32x8_store(p->normal_impulse, lambda);
        nph_f32x8_store(p->max_normal_impulse,
                        nph_f32x8_max(PHYS_KERNEL(load)(p->max_normal_impulse), lambda));
        PHYS_KERNEL(move)(&a, &b, ma, mb, lambda, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
        // The slip: the contact points' relative motion this substep.
        W8 ra_prev[3];
        W8 rb_prev[3];
        PHYS_KERNEL(anchors_now)(&a, &b, p, ra, rb);
        PHYS_KERNEL(anchors_now)(&pa, &pb, p, ra_prev, rb_prev);
        const W8 sx = nph_f32x8_sub(
            nph_f32x8_add(nph_f32x8_sub(b.dpx, pb.dpx), nph_f32x8_sub(rb[0], rb_prev[0])),
            nph_f32x8_add(nph_f32x8_sub(a.dpx, pa.dpx), nph_f32x8_sub(ra[0], ra_prev[0])));
        const W8 sy = nph_f32x8_sub(
            nph_f32x8_add(nph_f32x8_sub(b.dpy, pb.dpy), nph_f32x8_sub(rb[1], rb_prev[1])),
            nph_f32x8_add(nph_f32x8_sub(a.dpy, pa.dpy), nph_f32x8_sub(ra[1], ra_prev[1])));
        const W8 sz = nph_f32x8_sub(
            nph_f32x8_add(nph_f32x8_sub(b.dpz, pb.dpz), nph_f32x8_sub(rb[2], rb_prev[2])),
            nph_f32x8_add(nph_f32x8_sub(a.dpz, pa.dpz), nph_f32x8_sub(ra[2], ra_prev[2])));
        const W8 s1 = PHYS_KERNEL(dot)(sx, sy, sz, t1x, t1y, t1z);
        const W8 s2 = PHYS_KERNEL(dot)(sx, sy, sz, t2x, t2y, t2z);
        const W8 m00 = PHYS_KERNEL(load)(p->tangent_mass[0]);
        const W8 m01 = PHYS_KERNEL(load)(p->tangent_mass[1]);
        const W8 m11 = PHYS_KERNEL(load)(p->tangent_mass[2]);
        W8 l1 = nph_f32x8_neg(nph_f32x8_add(nph_f32x8_mul(m00, s1), nph_f32x8_mul(m01, s2)));
        W8 l2 = nph_f32x8_neg(nph_f32x8_add(nph_f32x8_mul(m01, s1), nph_f32x8_mul(m11, s2)));
        const W8 cone = nph_f32x8_mul(friction, lambda);
        const W8 within = nph_f32x8_cmp_le(
            nph_f32x8_add(nph_f32x8_mul(l1, l1), nph_f32x8_mul(l2, l2)), nph_f32x8_mul(cone, cone));
        l1 = nph_f32x8_select(within, l1, zero);
        l2 = nph_f32x8_select(within, l2, zero);
        nph_f32x8_store(p->tangent_impulse[0], l1);
        nph_f32x8_store(p->tangent_impulse[1], l2);
        PHYS_KERNEL(move)(&a, &b, ma, mb, l1, t1x, t1y, t1z, p->ang_a_t1[0], p->ang_b_t1[0]);
        PHYS_KERNEL(move)(&a, &b, ma, mb, l2, t2x, t2y, t2z, p->ang_a_t2[0], p->ang_b_t2[0]);
    }
    // An impact (a point approaching faster than PHYS_IMPACT_SPEED) moves
    // the points out one at a time: the first moves the most, and the turn
    // that leaves tipped boxes landing flat by 8 degrees from 10 m. More
    // sweeps of the normal corrections, their multipliers accumulated and
    // kept at or above zero, share it as the rows' coupling demands; resting
    // and sliding lanes are left as they were.
    if (g->point_count > 1U) {
        const W8 impact = PHYS_KERNEL(load)(g->impact_lanes);
        for (u32 sweep = 0U; sweep < PHYS_IMPACT_SWEEPS && nph_f32x8_movemask(impact) != 0U;
             ++sweep) {
            for (u32 k = 0U; k < g->point_count; ++k) {
                PhysPointLanes *p = &points[k];
                const W8 s = PHYS_KERNEL(separation)(&a, &b, p, nx, ny, nz);
                const W8 accumulated = PHYS_KERNEL(load)(p->normal_impulse);
                const W8 total = nph_f32x8_select(
                    impact,
                    nph_f32x8_max(nph_f32x8_sub(accumulated, nph_f32x8_mul(s, PHYS_KERNEL(load)(
                                                                                  p->normal_mass))),
                                  zero),
                    accumulated);
                nph_f32x8_store(p->normal_impulse, total);
                nph_f32x8_store(p->max_normal_impulse,
                                nph_f32x8_max(PHYS_KERNEL(load)(p->max_normal_impulse), total));
                PHYS_KERNEL(move)(&a, &b, ma, mb, nph_f32x8_sub(total, accumulated), nx, ny, nz,
                                  p->ang_a_n[0], p->ang_b_n[0]);
            }
        }
    }
    PHYS_KERNEL(scatter_positions)(bodies, g->body_a, g->write_mask_a, &a);
    PHYS_KERNEL(scatter_positions)(bodies, g->body_b, g->write_mask_b, &b);
}

// The velocity pass, on the velocities the positions gave: dynamic
// friction takes at most mu times the normal multiplier over the substep
// off the points' sliding, and touching points leave at the restitution of
// their pre-solve speed when they approached faster than the threshold,
// else at rest (which also takes back the speed the position pass added).
static void PHYS_KERNEL(xpbd_velocities)(PhysSolverBody *bodies, PhysContactLanes *g,
                                         PhysPointLanes *points, f32 inv_h, f32 threshold)
{
    BodyLanes a;
    BodyLanes b;
    PHYS_KERNEL(gather)(bodies, g->body_a, true, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, true, &b);
    const W8 zero = nph_f32x8_zero();
    const W8 vinv_h = nph_f32x8_splat(inv_h);
    const W8 vthreshold = nph_f32x8_splat(-threshold);
    const W8 ma = PHYS_KERNEL(load)(g->inv_mass_a);
    const W8 mb = PHYS_KERNEL(load)(g->inv_mass_b);
    const W8 nx = PHYS_KERNEL(load)(g->normal[0]);
    const W8 ny = PHYS_KERNEL(load)(g->normal[1]);
    const W8 nz = PHYS_KERNEL(load)(g->normal[2]);
    const W8 t1x = PHYS_KERNEL(load)(g->tangent1[0]);
    const W8 t1y = PHYS_KERNEL(load)(g->tangent1[1]);
    const W8 t1z = PHYS_KERNEL(load)(g->tangent1[2]);
    const W8 t2x = PHYS_KERNEL(load)(g->tangent2[0]);
    const W8 t2y = PHYS_KERNEL(load)(g->tangent2[1]);
    const W8 t2z = PHYS_KERNEL(load)(g->tangent2[2]);
    const W8 friction = PHYS_KERNEL(load)(g->friction);
    const W8 e = PHYS_KERNEL(load)(g->restitution);
    // The friction each point took this pass, for the impact sweeps.
    W8 took1[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
    W8 took2[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
    for (u32 k = 0U; k < NPH_PHYSICS_MANIFOLD_MAX_POINTS; ++k) {
        took1[k] = zero;
        took2[k] = zero;
    }
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        const W8 lambda = PHYS_KERNEL(load)(p->normal_impulse);
        const W8 active = nph_f32x8_cmp_gt(lambda, zero);
        if (nph_f32x8_movemask(active) == 0U) {
            continue;
        }
        W8 vx;
        W8 vy;
        W8 vz;
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        const W8 vt1 = PHYS_KERNEL(dot)(vx, vy, vz, t1x, t1y, t1z);
        const W8 vt2 = PHYS_KERNEL(dot)(vx, vy, vz, t2x, t2y, t2z);
        // Dynamic friction: the impulse that stops the sliding, within
        // mu lambda / h.
        W8 f1 =
            nph_f32x8_neg(nph_f32x8_add(nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[0]), vt1),
                                        nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[1]), vt2)));
        W8 f2 =
            nph_f32x8_neg(nph_f32x8_add(nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[1]), vt1),
                                        nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[2]), vt2)));
        PHYS_KERNEL(friction_circle)(
            vt1, vt2, nph_f32x8_mul(nph_f32x8_mul(friction, lambda), vinv_h), &f1, &f2);
        f1 = nph_f32x8_select(active, f1, zero);
        f2 = nph_f32x8_select(active, f2, zero);
        took1[k] = f1;
        took2[k] = f2;
        PHYS_KERNEL(apply)(&a, &b, ma, mb, f1, t1x, t1y, t1z, p->ang_a_t1[0], p->ang_b_t1[0]);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, f2, t2x, t2y, t2z, p->ang_a_t2[0], p->ang_b_t2[0]);
        // Restitution, from the normal speed after friction.
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        const W8 vn = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
        const W8 before = PHYS_KERNEL(load)(p->substep_velocity);
        const W8 bounce = nph_f32x8_select(nph_f32x8_cmp_lt(before, vthreshold),
                                           nph_f32x8_mul(nph_f32x8_neg(e), before), zero);
        const W8 dn = nph_f32x8_select(
            active, nph_f32x8_mul(nph_f32x8_sub(bounce, vn), PHYS_KERNEL(load)(p->normal_mass)),
            zero);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, dn, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
    }
    // An impact's velocities come from positions moved one point at a time,
    // and one pass of four rows each stopping its own point, its friction
    // within mu times the hit's multiplier, spun a box landing flat about
    // its vertical to 7 rad/s. More sweeps of friction (accumulated, within
    // the cone) and the normal rows converge them; others are left alone.
    if (g->point_count > 1U) {
        const W8 impact = PHYS_KERNEL(load)(g->impact_lanes);
        for (u32 sweep = 0U; sweep < PHYS_IMPACT_SWEEPS && nph_f32x8_movemask(impact) != 0U;
             ++sweep) {
            for (u32 k = 0U; k < g->point_count; ++k) {
                PhysPointLanes *p = &points[k];
                const W8 lambda = PHYS_KERNEL(load)(p->normal_impulse);
                const W8 active = nph_f32x8_and(impact, nph_f32x8_cmp_gt(lambda, zero));
                W8 vx;
                W8 vy;
                W8 vz;
                PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
                const W8 vt1 = PHYS_KERNEL(dot)(vx, vy, vz, t1x, t1y, t1z);
                const W8 vt2 = PHYS_KERNEL(dot)(vx, vy, vz, t2x, t2y, t2z);
                W8 new1 = nph_f32x8_sub(
                    took1[k],
                    nph_f32x8_add(nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[0]), vt1),
                                  nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[1]), vt2)));
                W8 new2 = nph_f32x8_sub(
                    took2[k],
                    nph_f32x8_add(nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[1]), vt1),
                                  nph_f32x8_mul(PHYS_KERNEL(load)(p->tangent_mass[2]), vt2)));
                PHYS_KERNEL(friction_circle)(
                    vt1, vt2, nph_f32x8_mul(nph_f32x8_mul(friction, lambda), vinv_h), &new1, &new2);
                const W8 d1 = nph_f32x8_select(active, nph_f32x8_sub(new1, took1[k]), zero);
                const W8 d2 = nph_f32x8_select(active, nph_f32x8_sub(new2, took2[k]), zero);
                took1[k] = nph_f32x8_add(took1[k], d1);
                took2[k] = nph_f32x8_add(took2[k], d2);
                PHYS_KERNEL(apply)(&a, &b, ma, mb, d1, t1x, t1y, t1z, p->ang_a_t1[0],
                                   p->ang_b_t1[0]);
                PHYS_KERNEL(apply)(&a, &b, ma, mb, d2, t2x, t2y, t2z, p->ang_a_t2[0],
                                   p->ang_b_t2[0]);
                PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
                const W8 vn = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
                const W8 before = PHYS_KERNEL(load)(p->substep_velocity);
                const W8 bounce = nph_f32x8_select(nph_f32x8_cmp_lt(before, vthreshold),
                                                   nph_f32x8_mul(nph_f32x8_neg(e), before), zero);
                const W8 dn = nph_f32x8_select(
                    active,
                    nph_f32x8_mul(nph_f32x8_sub(bounce, vn), PHYS_KERNEL(load)(p->normal_mass)),
                    zero);
                PHYS_KERNEL(apply)(&a, &b, ma, mb, dn, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
            }
        }
    }
    PHYS_KERNEL(scatter)(bodies, g->body_a, g->write_mask_a, &a);
    PHYS_KERNEL(scatter)(bodies, g->body_b, g->write_mask_b, &b);
}
