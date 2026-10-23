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

static void PHYS_KERNEL(warm_start)(PhysSolverBody *bodies, PhysContactLanes *g,
                                    PhysPointLanes *points)
{
    BodyLanes a;
    BodyLanes b;
    PHYS_KERNEL(gather)(bodies, g->body_a, false, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, false, &b);
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
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        PHYS_KERNEL(apply)(&a, &b, ma, mb, PHYS_KERNEL(load)(p->normal_impulse), nx, ny, nz,
                           p->ang_a_n[0], p->ang_b_n[0]);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, PHYS_KERNEL(load)(p->tangent_impulse[0]), t1x, t1y, t1z,
                           p->ang_a_t1[0], p->ang_b_t1[0]);
        PHYS_KERNEL(apply)(&a, &b, ma, mb, PHYS_KERNEL(load)(p->tangent_impulse[1]), t2x, t2y, t2z,
                           p->ang_a_t2[0], p->ang_b_t2[0]);
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
static void PHYS_KERNEL(prepare)(const PhysSolverBody *bodies, const PhysContactLanes *g,
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

static void PHYS_KERNEL(solve)(PhysSolverBody *bodies, PhysContactLanes *g, PhysPointLanes *points,
                               f32 inv_h, f32 push_max, bool use_bias)
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
    const W8 soft_bias = use_bias ? PHYS_KERNEL(load)(g->bias_rate) : zero;
    const W8 soft_mass = use_bias ? PHYS_KERNEL(load)(g->mass_scale) : one;
    const W8 soft_impulse = use_bias ? PHYS_KERNEL(load)(g->impulse_scale) : zero;
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        // The current separation: the bodies' displacements this step move
        // the fixed anchors (rotated by the bodies' rotations this step).
        W8 pax;
        W8 pay;
        W8 paz;
        W8 pbx;
        W8 pby;
        W8 pbz;
        PHYS_KERNEL(rotate)(&a, PHYS_KERNEL(load)(p->anchor_a[0]),
                            PHYS_KERNEL(load)(p->anchor_a[1]), PHYS_KERNEL(load)(p->anchor_a[2]),
                            &pax, &pay, &paz);
        PHYS_KERNEL(rotate)(&b, PHYS_KERNEL(load)(p->anchor_b[0]),
                            PHYS_KERNEL(load)(p->anchor_b[1]), PHYS_KERNEL(load)(p->anchor_b[2]),
                            &pbx, &pby, &pbz);
        const W8 dx = nph_f32x8_add(nph_f32x8_sub(b.dpx, a.dpx), nph_f32x8_sub(pbx, pax));
        const W8 dy = nph_f32x8_add(nph_f32x8_sub(b.dpy, a.dpy), nph_f32x8_sub(pby, pay));
        const W8 dz = nph_f32x8_add(nph_f32x8_sub(b.dpz, a.dpz), nph_f32x8_sub(pbz, paz));
        const W8 s = nph_f32x8_add(PHYS_KERNEL(dot)(dx, dy, dz, nx, ny, nz),
                                   PHYS_KERNEL(load)(p->adjusted_separation));
        // Apart: close at most the gap (speculative). Overlapping: the
        // spring's push, capped, with its softness; or rigid when relaxing.
        const W8 apart = nph_f32x8_cmp_gt(s, zero);
        const W8 bias = nph_f32x8_select(apart, nph_f32x8_mul(s, vinv_h),
                                         nph_f32x8_max(nph_f32x8_mul(soft_bias, s), min_bias));
        const W8 mass_scale = nph_f32x8_select(apart, one, soft_mass);
        const W8 impulse_scale = nph_f32x8_select(apart, zero, soft_impulse);
        W8 vx;
        W8 vy;
        W8 vz;
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        const W8 vn = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
        const W8 accumulated = PHYS_KERNEL(load)(p->normal_impulse);
        const W8 impulse =
            nph_f32x8_sub(nph_f32x8_neg(nph_f32x8_mul(
                              nph_f32x8_mul(PHYS_KERNEL(load)(p->normal_mass), mass_scale),
                              nph_f32x8_add(vn, bias))),
                          nph_f32x8_mul(impulse_scale, accumulated));
        const W8 total = nph_f32x8_max(nph_f32x8_add(accumulated, impulse), zero);
        const W8 delta = nph_f32x8_sub(total, accumulated);
        nph_f32x8_store(p->normal_impulse, total);
        nph_f32x8_store(p->max_normal_impulse,
                        nph_f32x8_max(PHYS_KERNEL(load)(p->max_normal_impulse), total));
        PHYS_KERNEL(apply)(&a, &b, ma, mb, delta, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
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
        // The friction circle: |impulse| <= mu * normal impulse.
        const W8 limit = nph_f32x8_mul(friction, PHYS_KERNEL(load)(p->normal_impulse));
        const W8 len =
            nph_f32x8_sqrt(nph_f32x8_add(nph_f32x8_mul(new1, new1), nph_f32x8_mul(new2, new2)));
        const W8 over = nph_f32x8_cmp_gt(len, limit);
        const W8 scale = nph_f32x8_select(over, nph_f32x8_div(limit, len), one);
        new1 = nph_f32x8_mul(new1, scale);
        new2 = nph_f32x8_mul(new2, scale);
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
// threshold and carried load bounce with the pair's restitution.
static void PHYS_KERNEL(restitution)(PhysSolverBody *bodies, PhysContactLanes *g,
                                     PhysPointLanes *points, f32 threshold)
{
    BodyLanes a;
    BodyLanes b;
    PHYS_KERNEL(gather)(bodies, g->body_a, false, &a);
    PHYS_KERNEL(gather)(bodies, g->body_b, false, &b);
    const W8 zero = nph_f32x8_zero();
    const W8 ma = PHYS_KERNEL(load)(g->inv_mass_a);
    const W8 mb = PHYS_KERNEL(load)(g->inv_mass_b);
    const W8 nx = PHYS_KERNEL(load)(g->normal[0]);
    const W8 ny = PHYS_KERNEL(load)(g->normal[1]);
    const W8 nz = PHYS_KERNEL(load)(g->normal[2]);
    const W8 e = PHYS_KERNEL(load)(g->restitution);
    const W8 vthreshold = nph_f32x8_splat(-threshold);
    for (u32 k = 0U; k < g->point_count; ++k) {
        PhysPointLanes *p = &points[k];
        const W8 rel = PHYS_KERNEL(load)(p->relative_velocity);
        const W8 active =
            nph_f32x8_and(nph_f32x8_cmp_lt(rel, vthreshold),
                          nph_f32x8_cmp_gt(PHYS_KERNEL(load)(p->max_normal_impulse), zero));
        if (nph_f32x8_movemask(active) == 0U) {
            continue;
        }
        W8 vx;
        W8 vy;
        W8 vz;
        PHYS_KERNEL(relative_velocity)(&a, &b, p, &vx, &vy, &vz);
        const W8 vn = PHYS_KERNEL(dot)(vx, vy, vz, nx, ny, nz);
        const W8 accumulated = PHYS_KERNEL(load)(p->normal_impulse);
        const W8 impulse = nph_f32x8_neg(nph_f32x8_mul(PHYS_KERNEL(load)(p->normal_mass),
                                                       nph_f32x8_add(vn, nph_f32x8_mul(e, rel))));
        const W8 total = nph_f32x8_max(nph_f32x8_add(accumulated, impulse), zero);
        const W8 delta = nph_f32x8_select(active, nph_f32x8_sub(total, accumulated), zero);
        nph_f32x8_store(p->normal_impulse, nph_f32x8_add(accumulated, delta));
        nph_f32x8_store(p->max_normal_impulse,
                        nph_f32x8_max(PHYS_KERNEL(load)(p->max_normal_impulse),
                                      nph_f32x8_add(accumulated, delta)));
        PHYS_KERNEL(apply)(&a, &b, ma, mb, delta, nx, ny, nz, p->ang_a_n[0], p->ang_b_n[0]);
    }
    PHYS_KERNEL(scatter)(bodies, g->body_a, g->write_mask_a, &a);
    PHYS_KERNEL(scatter)(bodies, g->body_b, g->write_mask_b, &b);
}
