// SPDX-License-Identifier: Apache-2.0
// Voxels (render/voxel.h; ADR-0061): the records' packing, the lobe's
// concentration, the traversal's C twin, and the shading model's terms.
#include <nepheline/core/assert.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>
#include <nepheline/render/voxel.h>

#include <math.h>

static u32 unorm8(f32 x)
{
    return (u32)nph_f32_round(nph_f32_saturate(x) * 255.0F);
}

static u32 box_step(f32 x)
{
    return (u32)nph_f32_round(nph_f32_saturate(x) * (f32)NPH_RENDER_VOXEL_BOX_STEPS);
}

NphRenderVoxel nph_render_voxel_pack(const NphRenderVoxelDesc *d)
{
    NPH_ASSERT(d != NULL);
    NphRenderVoxel v;
    const f32 lo[3] = {d->box_lo.x, d->box_lo.y, d->box_lo.z};
    const f32 hi[3] = {d->box_hi.x, d->box_hi.y, d->box_hi.z};
    v.box = 0U;
    for (u32 a = 0; a < 3U; ++a) {
        const u32 l = nph_min_u32(box_step(lo[a]), NPH_RENDER_VOXEL_BOX_STEPS - 1U);
        const u32 h = nph_clamp_u32(box_step(hi[a]), l + 1U, NPH_RENDER_VOXEL_BOX_STEPS);
        v.box |= (l << (5U * a)) | ((h - 1U) << (15U + 5U * a));
    }
    const u32 shape = nph_min_u32(d->shape, NPH_RENDER_VOXEL_SHAPE_CAPSULE);
    v.box |= shape << NPH_RENDER_VOXEL_SHAPE_SHIFT;
    u32 capsule = 0U;
    if (shape == NPH_RENDER_VOXEL_SHAPE_CAPSULE) {
        capsule = (d->diagonal & 3U) | (nph_render_voxel_radius_encode(d->radius) << 2U);
    }
    v.color = unorm8(nph_render_ref_linear_to_srgb(nph_f32_saturate(d->albedo.x))) |
              (unorm8(nph_render_ref_linear_to_srgb(nph_f32_saturate(d->albedo.y))) << 8U) |
              (unorm8(nph_render_ref_linear_to_srgb(nph_f32_saturate(d->albedo.z))) << 16U) |
              (unorm8(d->occlusion) << 24U);
    const f32 len = nph_vec3_length(d->axis);
    const NphVec3 axis = len > 0.0F && nph_f32_is_finite(len) ? nph_vec3_scale(d->axis, 1.0F / len)
                                                              : nph_vec3_unit_z();
    v.normal = nph_oct_encode_snorm16(axis);
    v.material = unorm8(d->roughness) | (unorm8(d->metallic) << 8U) |
                 (nph_render_voxel_kappa_encode(d->kappa) << 16U) | (capsule << 24U);
    return v;
}

void nph_render_voxel_unpack(NphRenderVoxel v, NphRenderVoxelDesc *out)
{
    NPH_ASSERT(out != NULL);
    f32 lo[3];
    f32 hi[3];
    for (u32 a = 0; a < 3U; ++a) {
        lo[a] = (f32)((v.box >> (5U * a)) & 31U) / (f32)NPH_RENDER_VOXEL_BOX_STEPS;
        hi[a] = (f32)(((v.box >> (15U + 5U * a)) & 31U) + 1U) / (f32)NPH_RENDER_VOXEL_BOX_STEPS;
    }
    out->box_lo = nph_vec3(lo[0], lo[1], lo[2]);
    out->box_hi = nph_vec3(hi[0], hi[1], hi[2]);
    out->shape = v.box >> NPH_RENDER_VOXEL_SHAPE_SHIFT;
    const u32 capsule = v.material >> 24U;
    out->diagonal = out->shape == NPH_RENDER_VOXEL_SHAPE_CAPSULE ? capsule & 3U : 0U;
    out->radius = out->shape == NPH_RENDER_VOXEL_SHAPE_CAPSULE
                      ? nph_render_voxel_radius_decode(capsule >> 2U)
                      : 0.0F;
    out->albedo = nph_vec3(nph_render_ref_srgb_to_linear((f32)(v.color & 0xFFU) / 255.0F),
                           nph_render_ref_srgb_to_linear((f32)((v.color >> 8U) & 0xFFU) / 255.0F),
                           nph_render_ref_srgb_to_linear((f32)((v.color >> 16U) & 0xFFU) / 255.0F));
    out->occlusion = (f32)(v.color >> 24U) / 255.0F;
    out->axis = nph_oct_decode_snorm16(v.normal);
    out->roughness = (f32)(v.material & 0xFFU) / 255.0F;
    out->metallic = (f32)((v.material >> 8U) & 0xFFU) / 255.0F;
    out->kappa = nph_render_voxel_kappa_decode((v.material >> 16U) & 0xFFU);
}

u32 nph_render_voxel_radius_encode(f32 radius)
{
    if (!(radius > 0.0F)) {
        return 0U; // also NaN
    }
    const f32 c = nph_f32_round(8.0F * (nph_f32_log2(radius) + 8.0F));
    return (u32)nph_f32_clamp(c, 0.0F, (f32)NPH_RENDER_VOXEL_RADIUS_CODES - 1.0F);
}

f32 nph_render_voxel_radius_decode(u32 code)
{
    return nph_f32_exp2((f32)nph_min_u32(code, NPH_RENDER_VOXEL_RADIUS_CODES - 1U) / 8.0F - 8.0F);
}

u32 nph_render_voxel_kappa_encode(f32 kappa)
{
    if (!(kappa > 0.0F)) {
        return 0U; // also NaN
    }
    const f32 k = nph_f32_min(kappa, NPH_RENDER_VOXEL_KAPPA_MAX);
    return nph_min_u32((u32)nph_f32_round(16.0F * nph_f32_log2(1.0F + k)), 255U);
}

f32 nph_render_voxel_kappa_decode(u32 b)
{
    return nph_f32_exp2((f32)nph_min_u32(b, 255U) / 16.0F) - 1.0F;
}

// A(kappa) = coth(kappa) - 1/kappa, the mean cosine of a vMF lobe about its
// axis, in double precision: its series near 0, where the difference cancels.
static f64 vmf_mean_cosine(f64 kappa)
{
    if (kappa < 1e-3) {
        return kappa / 3.0 - kappa * kappa * kappa / 45.0;
    }
    if (kappa > 40.0) {
        return 1.0 - 1.0 / kappa; // coth is 1 to 1e-34 here
    }
    return 1.0 / tanh(kappa) - 1.0 / kappa;
}

f32 nph_render_voxel_moment(f32 kappa)
{
    const f64 k = (f64)nph_f32_max(kappa, 0.0F);
    if (k < 1e-3) {
        return (f32)(1.0 / 3.0 + 2.0 * k * k / 45.0);
    }
    return (f32)(1.0 - 2.0 * vmf_mean_cosine(k) / k);
}

f32 nph_render_voxel_kappa_from_moment(f32 m)
{
    if (!(m > 1.0F / 3.0F)) {
        return 0.0F;
    }
    if (m >= nph_render_voxel_moment(NPH_RENDER_VOXEL_KAPPA_MAX)) {
        return NPH_RENDER_VOXEL_KAPPA_MAX;
    }
    // The moment rises with kappa: bisection over log(1 + kappa), 32 steps
    // to a part in 2^32 of the range, past a float's precision.
    f64 lo = 0.0;
    f64 hi = log(1.0 + (f64)NPH_RENDER_VOXEL_KAPPA_MAX);
    for (u32 i = 0; i < 32U; ++i) {
        const f64 mid = 0.5 * (lo + hi);
        if ((f64)nph_render_voxel_moment((f32)(exp(mid) - 1.0)) < (f64)m) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (f32)(exp(0.5 * (lo + hi)) - 1.0);
}

// ---------------------------------------------------------------------------
// The traversal
// ---------------------------------------------------------------------------

static bool brick_bit(const NphRenderVoxelBrick *b, u32 bit)
{
    return ((b->mask[bit >> 5U] >> (bit & 31U)) & 1U) != 0U;
}

// The cells set below `bit` (0..63): the voxel's rank among the brick's.
static u32 brick_rank(const NphRenderVoxelBrick *b, u32 bit)
{
    NPH_ASSERT(bit < NPH_RENDER_VOXEL_BRICK_CELLS);
    if (bit < 32U) {
        return nph_popcount_u32(b->mask[0] & ((1U << bit) - 1U));
    }
    const u32 below = (bit - 32U) & 31U;
    return nph_popcount_u32(b->mask[0]) +
           nph_popcount_u32(below == 0U ? 0U : (b->mask[1] & ((1U << below) - 1U)));
}

static f32 dot3f(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The capsule from `a` to `b` of radius `r` against the ray p + s d (any
// length of d) for s in [s_lo, s_hi]: the earliest entry of its body (a
// finite cylinder) and of its ends' spheres, the union's first point, or
// s_lo where the ray is already inside there. On a hit, *out_s and `n` (the
// surface's outward normal there, unnormalised).
static bool capsule_hit(const f32 p[3], const f32 d[3], const f32 a[3], const f32 b[3], f32 r,
                        f32 s_lo, f32 s_hi, f32 *out_s, f32 n[3])
{
    f32 ba[3];
    f32 oa[3];
    for (u32 k = 0; k < 3U; ++k) {
        ba[k] = b[k] - a[k];
        oa[k] = p[k] - a[k];
    }
    const f32 baba = dot3f(ba, ba);
    const f32 r2 = r * r;
    // Inside where the range starts.
    f32 q[3];
    for (u32 k = 0; k < 3U; ++k) {
        q[k] = oa[k] + s_lo * d[k];
    }
    f32 y = baba > 0.0F ? nph_f32_saturate(dot3f(q, ba) / baba) : 0.0F;
    f32 qn[3];
    for (u32 k = 0; k < 3U; ++k) {
        qn[k] = q[k] - y * ba[k];
    }
    f32 best = NPH_F32_MAX;
    if (dot3f(qn, qn) <= r2) {
        best = s_lo;
    } else {
        const f32 dd = dot3f(d, d);
        const f32 bard = dot3f(ba, d);
        const f32 baoa = dot3f(ba, oa);
        const f32 doa = dot3f(d, oa);
        const f32 oaoa = dot3f(oa, oa);
        // The body: |q - (q.ba / ba.ba) ba| = r on the ray, its entry where the
        // point's projection lies between the ends.
        const f32 qa = baba * dd - bard * bard;
        if (qa > 1e-12F * baba * dd) {
            const f32 qb = baba * doa - baoa * bard;
            const f32 qc = baba * oaoa - baoa * baoa - r2 * baba;
            const f32 h = qb * qb - qa * qc;
            if (h >= 0.0F) {
                const f32 sb = (-qb - nph_f32_sqrt(h)) / qa;
                const f32 yb = baoa + sb * bard;
                if (yb >= 0.0F && yb <= baba) {
                    best = sb;
                }
            }
        }
        // The ends.
        for (u32 e = 0; e < 2U; ++e) {
            f32 oc[3];
            for (u32 k = 0; k < 3U; ++k) {
                oc[k] = e == 0U ? oa[k] : oa[k] - ba[k];
            }
            const f32 hb = dot3f(d, oc);
            const f32 hc = dot3f(oc, oc) - r2;
            const f32 h = hb * hb - dd * hc;
            if (h >= 0.0F) {
                const f32 se = (-hb - nph_f32_sqrt(h)) / dd;
                best = nph_f32_min(best, se);
            }
        }
        if (!(best >= s_lo && best <= s_hi)) {
            return false;
        }
    }
    for (u32 k = 0; k < 3U; ++k) {
        q[k] = oa[k] + best * d[k];
    }
    y = baba > 0.0F ? nph_f32_saturate(dot3f(q, ba) / baba) : 0.0F;
    for (u32 k = 0; k < 3U; ++k) {
        n[k] = q[k] - y * ba[k];
    }
    *out_s = best;
    return true;
}

// The footprint of a voxel (its `box` and `material` words) in the cell of
// lowest corner `base`, against the ray o + t d (brick space; inv the
// reciprocal of d) for t in [*t0, t1]; the ray crosses the cell over
// [t_cell, t_exit], and the round shapes' tests start from t_cell so that
// their terms keep their precision however far the ray came. On a hit, *t0
// takes its t and *face the face it entered.
static bool footprint(u32 box, u32 material, const f32 base[3], f32 step, const f32 o[3],
                      const f32 d[3], const f32 inv[3], f32 t_cell, f32 t_exit, f32 *t0, f32 t1,
                      u32 *face)
{
    f32 lo[3];
    f32 hi[3];
    for (u32 a = 0; a < 3U; ++a) {
        lo[a] = base[a] + (f32)((box >> (5U * a)) & 31U) * step;
        hi[a] = base[a] + (f32)(((box >> (15U + 5U * a)) & 31U) + 1U) * step;
    }
    const u32 shape = box >> NPH_RENDER_VOXEL_SHAPE_SHIFT;
    // The ray from where it enters the cell, its t then counted from there.
    f32 p[3];
    for (u32 a = 0; a < 3U; ++a) {
        p[a] = o[a] + t_cell * d[a];
    }
    f32 n[3];
    f32 s = 0.0F;
    u32 f = 0U;
    if (shape == NPH_RENDER_VOXEL_SHAPE_CAPSULE) {
        // A capsule from the box's corner (x, y, low z) its bits name to the
        // opposite one, clipped to the cell.
        const u32 diagonal = (material >> 24U) & 3U;
        const f32 r = nph_render_voxel_radius_decode((material >> 26U) & 63U) *
                      (step * (f32)NPH_RENDER_VOXEL_BOX_STEPS);
        const f32 ca[3] = {(diagonal & 1U) != 0U ? hi[0] : lo[0],
                           (diagonal & 2U) != 0U ? hi[1] : lo[1], lo[2]};
        const f32 cb[3] = {(diagonal & 1U) != 0U ? lo[0] : hi[0],
                           (diagonal & 2U) != 0U ? lo[1] : hi[1], hi[2]};
        const f32 s_lo = nph_f32_max(*t0, t_cell) - t_cell;
        const f32 s_hi = nph_f32_min(t1, t_exit) - t_cell;
        if (!(s_lo <= s_hi) || !capsule_hit(p, d, ca, cb, r, s_lo, s_hi, &s, n)) {
            return false;
        }
    } else {
        f32 bt0 = *t0;
        f32 bt1 = t1;
        f32 enter = -NPH_F32_MAX;
        for (u32 a = 0; a < 3U; ++a) {
            const f32 ta = (lo[a] - o[a]) * inv[a];
            const f32 tb = (hi[a] - o[a]) * inv[a];
            const f32 tn = nph_f32_min(ta, tb);
            if (tn > enter) {
                enter = tn;
                f = a | (d[a] < 0.0F ? 4U : 0U);
            }
            bt0 = nph_f32_max(bt0, tn);
            bt1 = nph_f32_min(bt1, nph_f32_max(ta, tb));
        }
        if (!(bt0 <= bt1)) {
            return false;
        }
        if (shape == NPH_RENDER_VOXEL_SHAPE_BOX) {
            *t0 = bt0;
            *face = f;
            return true;
        }
        f32 q[3];
        f32 k[3];
        f32 half[3];
        f32 qq = 0.0F;
        f32 qk = 0.0F;
        f32 kk = 0.0F;
        for (u32 a = 0; a < 3U; ++a) {
            half[a] = 0.5F * (hi[a] - lo[a]);
            q[a] = (p[a] - 0.5F * (lo[a] + hi[a])) / half[a];
            k[a] = d[a] / half[a];
            qq += q[a] * q[a];
            qk += q[a] * k[a];
            kk += k[a] * k[a];
        }
        const f32 disc = qk * qk - kk * (qq - 1.0F);
        if (disc < 0.0F) {
            return false;
        }
        const f32 root = nph_f32_sqrt(disc);
        const f32 se = (-qk - root) / kk;
        const f32 sx = (-qk + root) / kk;
        if (sx < bt0 - t_cell || se > bt1 - t_cell) {
            return false;
        }
        s = nph_f32_max(se, bt0 - t_cell);
        for (u32 a = 0; a < 3U; ++a) {
            n[a] = (q[a] + s * k[a]) / half[a];
        }
    }
    // The face the normal points out of most, facing back along the ray.
    f32 best = -1.0F;
    for (u32 a = 0; a < 3U; ++a) {
        if (nph_f32_abs(n[a]) > best) {
            best = nph_f32_abs(n[a]);
            f = a | (n[a] > 0.0F ? 4U : 0U);
        }
    }
    *t0 = t_cell + s;
    *face = f;
    return true;
}

bool nph_render_voxel_trace(const NphRenderVoxelBrick *brick, const NphRenderVoxel *voxels,
                            NphVec3 origin, NphVec3 dir, f32 t_min, f32 t_max,
                            NphRenderVoxelHit *out)
{
    NPH_ASSERT(brick != NULL && voxels != NULL && out != NULL);
    const f32 h = brick->cell;
    const f32 o[3] = {origin.x - brick->origin.x, origin.y - brick->origin.y,
                      origin.z - brick->origin.z};
    f32 d[3] = {dir.x, dir.y, dir.z};
    f32 inv[3];
    for (u32 a = 0; a < 3U; ++a) {
        if (nph_f32_abs(d[a]) < 1e-20F) {
            d[a] = d[a] < 0.0F ? -1e-20F : 1e-20F;
        }
        inv[a] = 1.0F / d[a];
    }
    // The brick's box, [0, 4h] on each axis.
    const f32 side = (f32)NPH_RENDER_VOXEL_BRICK * h;
    f32 t0 = t_min;
    f32 t1 = t_max;
    for (u32 a = 0; a < 3U; ++a) {
        const f32 ta = (0.0F - o[a]) * inv[a];
        const f32 tb = (side - o[a]) * inv[a];
        t0 = nph_f32_max(t0, nph_f32_min(ta, tb));
        t1 = nph_f32_min(t1, nph_f32_max(ta, tb));
    }
    if (!(t0 <= t1)) {
        return false;
    }
    // The cell the ray enters by, the t it enters there, and the steps of
    // the traversal.
    f32 t_cell = t0;
    i32 c[3];
    i32 step[3];
    f32 next[3];
    f32 delta[3];
    for (u32 a = 0; a < 3U; ++a) {
        const f32 p = o[a] + t0 * d[a];
        const f32 cf = nph_f32_floor(p / h);
        c[a] = cf < 0.0F ? 0 : (cf > 3.0F ? 3 : (i32)cf);
        step[a] = d[a] > 0.0F ? 1 : -1;
        next[a] = ((f32)(c[a] + (step[a] > 0 ? 1 : 0)) * h - o[a]) * inv[a];
        delta[a] = h * nph_f32_abs(inv[a]);
    }
    const f32 step_h = h / (f32)NPH_RENDER_VOXEL_BOX_STEPS;
    // A ray crosses at most 4 + 3 + 3 cells of a brick.
    for (u32 iter = 0; iter < 10U; ++iter) {
        const u32 bit = (u32)c[0] + 4U * (u32)c[1] + 16U * (u32)c[2];
        if (brick_bit(brick, bit)) {
            const u32 index = brick->first_voxel + brick_rank(brick, bit);
            const f32 base[3] = {(f32)c[0] * h, (f32)c[1] * h, (f32)c[2] * h};
            f32 t = t0;
            u32 face = 0U;
            const f32 t_exit = nph_f32_min(nph_f32_min(next[0], next[1]), next[2]);
            if (footprint(voxels[index].box, voxels[index].material, base, step_h, o, d, inv,
                          t_cell, t_exit, &t, t1, &face)) {
                out->t = t;
                out->voxel = index;
                out->cell = bit;
                out->face = face;
                return true;
            }
        }
        // The nearest boundary next, the lowest axis on a tie.
        u32 a = 0U;
        if (next[1] < next[a]) {
            a = 1U;
        }
        if (next[2] < next[a]) {
            a = 2U;
        }
        if (next[a] > t1) {
            break;
        }
        c[a] += step[a];
        if (c[a] < 0 || c[a] > 3) {
            break;
        }
        t_cell = next[a];
        next[a] += delta[a];
    }
    return false;
}

// ---------------------------------------------------------------------------
// The shading model
// ---------------------------------------------------------------------------

f32 nph_render_voxel_visible(NphVec3 n, f32 kappa, NphVec3 v, NphVec3 *out_axis)
{
    NPH_ASSERT(out_axis != NULL);
    // Two lobes e^(kappa (n.m - 1)) and e^(s (v.m - 1)) multiply to one of
    // axis and sharpness from kappa n + s v (Wang et al. 2009).
    const NphVec3 r = nph_vec3_add(nph_vec3_scale(n, nph_f32_max(kappa, 0.0F)),
                                   nph_vec3_scale(v, NPH_RENDER_VOXEL_VIEW_SHARPNESS));
    const f32 k = nph_vec3_length(r);
    *out_axis = nph_vec3_scale(r, 1.0F / k);
    return k;
}

f32 nph_render_voxel_diffuse(NphVec3 n, f32 kappa, NphVec3 v, NphVec3 l)
{
    NphVec3 axis;
    const f32 k = nph_render_voxel_visible(n, kappa, v, &axis);
    // Hill's fit of a vMF lobe's mean clamped cosine (Pettineo 2016,
    // "SGIrradianceFitted" over the lobe's integral), in double precision
    // for the reference.
    const f64 lambda = (f64)k;
    const f64 c0 = 0.36;
    const f64 c1 = 1.0 / (4.0 * c0);
    const f64 eml = exp(-lambda);
    const f64 em2l = eml * eml;
    const f64 rl = 1.0 / lambda;
    const f64 scale = 1.0 + 2.0 * em2l - rl;
    const f64 bias = (eml - em2l) * rl - em2l;
    const f64 x = sqrt(1.0 - scale);
    const f64 mu = (f64)nph_vec3_dot(axis, l);
    const f64 x0 = c0 * mu;
    const f64 x1 = c1 * x;
    f64 y = nph_f64_clamp(mu, 0.0, 1.0);
    if (nph_f64_abs(x0) <= x1) {
        y = (x0 + x1) * (x0 + x1) / x;
    }
    return (f32)(scale * y + bias);
}

f32 nph_render_voxel_alpha2(f32 alpha2, f32 kappa)
{
    if (!(kappa > 0.0F)) {
        return 1.0F;
    }
    return nph_f32_min(alpha2 + 2.0F / kappa, 1.0F);
}

f32 nph_render_voxel_sh_band1(f32 kappa)
{
    return (f32)vmf_mean_cosine((f64)nph_f32_max(kappa, 0.0F));
}

f32 nph_render_voxel_sh_band2(f32 kappa)
{
    const f64 k = (f64)nph_f32_max(kappa, 0.0F);
    if (k < 1e-3) {
        return (f32)(k * k / 15.0);
    }
    return (f32)(1.0 - 3.0 * vmf_mean_cosine(k) / k);
}
