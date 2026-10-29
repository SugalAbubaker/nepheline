// SPDX-License-Identifier: Apache-2.0
// Voxels (render/voxel.h; ADR-0061): the records' packing, the lobe's
// concentration, the C twin of the shaders' voxel tests, and the shading
// model's terms.
#include <nepheline/core/assert.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>
#include <nepheline/render/voxel.h>

#include "cluster_lod_internal.h"

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

// ---------------------------------------------------------------------------
// The compact form (render/voxel.h; ADR-0063)
// ---------------------------------------------------------------------------

void nph_render_packed_voxels_init(NphRenderPackedVoxels *p, NphAllocator *a)
{
    NPH_ASSERT(p != NULL && a != NULL);
    NPH_ARRAY_INIT(&p->clusters, a);
    NPH_ARRAY_INIT(&p->bricks, a);
    NPH_ARRAY_INIT(&p->words, a);
}

void nph_render_packed_voxels_free(NphRenderPackedVoxels *p)
{
    if (p == NULL) {
        return;
    }
    NPH_ARRAY_FREE(&p->clusters);
    NPH_ARRAY_FREE(&p->bricks);
    NPH_ARRAY_FREE(&p->words);
}

u32 nph_render_voxel_cluster_stride(NphRenderVoxelCluster c)
{
    return (c.flags & NPH_RENDER_VOXEL_CLUSTER_WIDE) != 0U ? NPH_RENDER_VOXEL_WIDE_WORDS
                                                           : NPH_RENDER_VOXEL_WORDS;
}

f32 nph_render_voxel_cluster_cell(NphRenderVoxelCluster c)
{
    // 2^e from its bits: exact, as vis_voxel.glsl vis_voxel_cluster builds it.
    const i32 e = nph_clamp_i32((i32)c.cell_log2, NPH_RENDER_VOXEL_CELL_LOG2_MIN,
                                NPH_RENDER_VOXEL_CELL_LOG2_MAX);
    return nph_f32_from_bits((u32)(127 + e) << 23U);
}

// The two's complement value of the low `bits` bits of `v`.
static i32 signed_bits(u32 v, u32 bits)
{
    const i32 s = (i32)(v & ((1U << bits) - 1U));
    return s >= (1 << (bits - 1U)) ? s - (1 << bits) : s;
}

// A snorm16 component (its 16 bits) at `bits` bits: the nearest of the grid
// -m..m, m = 2^(bits - 1) - 1, as `bits` bits of two's complement.
static u32 snorm_narrow(u32 v16, u32 bits)
{
    const i32 m = (1 << (bits - 1U)) - 1;
    const f32 f = nph_f32_max((f32)signed_bits(v16, 16U) / 32767.0F, -1.0F);
    const i32 q = (i32)nph_f32_round(f * (f32)m);
    return (u32)q & ((1U << bits) - 1U);
}

// A `bits`-bit snorm component (two's complement) widened back to snorm16's
// 16 bits, round(|v| 32767 / m) with its sign, in integers: what
// vis_voxel.glsl vis_voxel_snorm16 computes, bit for bit.
static u32 snorm_widen(u32 v, u32 bits)
{
    const i32 s = signed_bits(v, bits);
    const i32 m = (1 << (bits - 1U)) - 1;
    const i32 a = s < 0 ? -s : s;
    const i32 q = nph_min_i32((a * 32767 + m / 2) / m, 32767);
    return (u32)(s < 0 ? -q : q) & 0xFFFFU;
}

static bool is_capsule(u32 box)
{
    return (box >> NPH_RENDER_VOXEL_SHAPE_SHIFT) == NPH_RENDER_VOXEL_SHAPE_CAPSULE;
}

// A voxel's words (render/voxel.h "The compact form"); `out[3]` only when wide.
static void pack_voxel(const NphRenderVoxel *v, bool wide, u32 out[4])
{
    const u32 nx = v->normal & 0xFFFFU;
    const u32 ny = v->normal >> 16U;
    const u32 lobe = is_capsule(v->box) ? snorm_narrow(nx, 8U) | (snorm_narrow(ny, 8U) << 8U) |
                                              ((v->material >> 24U) << 16U)
                                        : snorm_narrow(nx, 12U) | (snorm_narrow(ny, 12U) << 12U);
    out[0] = v->box;
    out[1] = lobe | (((v->material >> 16U) & 0xFFU) << 24U);
    out[2] = (v->color & 0xFFFFFFU) | ((v->material & 0xFFU) << 24U);
    out[3] = wide ? (v->color >> 24U) | (((v->material >> 8U) & 0xFFU) << 8U) : 0U;
}

NphRenderVoxel nph_render_voxel_from_packed(const u32 *words, NphRenderVoxelCluster c, u32 rank)
{
    NPH_ASSERT(words != NULL);
    const bool wide = (c.flags & NPH_RENDER_VOXEL_CLUSTER_WIDE) != 0U;
    const u32 *w = words + c.first_word + (u64)rank * nph_render_voxel_cluster_stride(c);
    const u32 extra = wide ? w[3] : (u32)c.occlusion | ((u32)c.metallic << 8U);
    const u32 lobe = w[1];
    const bool capsule = is_capsule(w[0]);
    NphRenderVoxel v;
    v.box = w[0];
    v.normal = capsule ? snorm_widen(lobe, 8U) | (snorm_widen(lobe >> 8U, 8U) << 16U)
                       : snorm_widen(lobe, 12U) | (snorm_widen(lobe >> 12U, 12U) << 16U);
    v.color = (w[2] & 0xFFFFFFU) | ((extra & 0xFFU) << 24U);
    v.material = (w[2] >> 24U) | (((extra >> 8U) & 0xFFU) << 8U) | ((lobe >> 24U) << 16U) |
                 ((capsule ? (lobe >> 16U) & 0xFFU : 0U) << 24U);
    return v;
}

NphRenderVoxelBrick nph_render_brick_from_packed(NphRenderPackedBrick b, NphRenderVoxelCluster c,
                                                 u32 first_voxel)
{
    // A whole number of bricks times a power of two: exact.
    const f32 cell = nph_render_voxel_cluster_cell(c);
    const f32 edge = 4.0F * cell;
    NphRenderVoxelBrick out;
    out.origin =
        nph_vec3((f32)b.corner[0] * edge, (f32)b.corner[1] * edge, (f32)b.corner[2] * edge);
    out.cell = cell;
    out.mask[0] = b.mask[0];
    out.mask[1] = b.mask[1];
    out.first_voxel = first_voxel;
    out.reserved = 0U;
    return out;
}

// One voxel cluster's record, bricks and words (nph_render_voxels_pack).
static NphResult pack_cluster(NphRenderMeshlet *m, const NphRenderVoxelBrick *bricks,
                              u32 brick_count, const NphRenderVoxel *voxels, u32 voxel_count,
                              NphRenderPackedVoxels *out)
{
    if (bricks == NULL || voxels == NULL || m->triangle_count > NPH_RENDER_VOXEL_CLUSTER_BRICKS ||
        m->vertex_offset > brick_count || m->triangle_count > brick_count - m->vertex_offset) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphRenderVoxelBrick *first = &bricks[m->vertex_offset];
    // Its cell, a power of two the record's exponent holds: no sign, no
    // mantissa, an exponent in range (zero, subnormals, infinities and NaN
    // have none).
    const u32 cell_bits = nph_f32_to_bits(first->cell);
    const i32 e = (i32)(cell_bits >> 23U) - 127;
    if ((cell_bits & 0x807FFFFFU) != 0U || e < NPH_RENDER_VOXEL_CELL_LOG2_MIN ||
        e > NPH_RENDER_VOXEL_CELL_LOG2_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const f32 edge = 4.0F * first->cell;
    const u32 voxel0 = first->first_voxel;
    u32 count = 0;
    for (u32 k = 0; k < m->triangle_count; ++k) {
        const NphRenderVoxelBrick *b = &bricks[m->vertex_offset + k];
        const u32 n = nph_popcount_u32(b->mask[0]) + nph_popcount_u32(b->mask[1]);
        if (n == 0U || nph_f32_to_bits(b->cell) != cell_bits || b->first_voxel != voxel0 + count ||
            b->first_voxel > voxel_count || n > voxel_count - b->first_voxel ||
            count + n > NPH_RENDER_VOXEL_CLUSTER_VOXELS) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        NphRenderPackedBrick p;
        const f32 origin[3] = {b->origin.x, b->origin.y, b->origin.z};
        for (u32 a = 0; a < 3U; ++a) {
            // Dividing by a power of two is exact: the corner is whole, in
            // 16 bits, and gives the origin back, or the brick is off its grid.
            const f32 q = origin[a] / edge;
            if (!(q == nph_f32_floor(q)) || q < -32768.0F || q > 32767.0F ||
                q * edge != origin[a]) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            p.corner[a] = (i16)q;
        }
        p.first = (u16)count;
        p.mask[0] = b->mask[0];
        p.mask[1] = b->mask[1];
        out->bricks.data[m->vertex_offset + k] = p;
        count += n;
    }
    // Narrow when its voxels share their occlusion and metallic.
    const NphRenderVoxel *v = &voxels[voxel0];
    const u32 shared = (v->color >> 24U) | (v->material & 0xFF00U);
    bool wide = false;
    for (u32 j = 1; j < count && !wide; ++j) {
        wide = ((v[j].color >> 24U) | (v[j].material & 0xFF00U)) != shared;
    }
    const u32 stride = wide ? NPH_RENDER_VOXEL_WIDE_WORDS : NPH_RENDER_VOXEL_WORDS;
    if ((u64)out->words.len + (u64)count * stride > NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphRenderVoxelCluster rec;
    rec.first_word = out->words.len;
    rec.cell_log2 = (i8)e;
    rec.occlusion = (u8)(wide ? 0U : shared & 0xFFU);
    rec.metallic = (u8)(wide ? 0U : shared >> 8U);
    rec.flags = (u8)(wide ? NPH_RENDER_VOXEL_CLUSTER_WIDE : 0U);
    const u32 at = out->words.len;
    NPH_TRY(NPH_ARRAY_RESIZE(&out->words, at + count * stride));
    for (u32 j = 0; j < count; ++j) {
        u32 w[4];
        pack_voxel(&v[j], wide, w);
        for (u32 k = 0; k < stride; ++k) {
            out->words.data[at + j * stride + k] = w[k];
        }
    }
    m->triangle_offset = out->clusters.len;
    return NPH_ARRAY_PUSH(&out->clusters, rec);
}

NphResult nph_render_voxels_pack(NphRenderMeshlet *meshlets, u32 meshlet_count,
                                 const NphRenderVoxelBrick *bricks, u32 brick_count,
                                 const NphRenderVoxel *voxels, u32 voxel_count,
                                 NphRenderPackedVoxels *out)
{
    NPH_ASSERT(out != NULL && (meshlets != NULL || meshlet_count == 0U));
    NPH_ARRAY_CLEAR(&out->clusters);
    NPH_ARRAY_CLEAR(&out->words);
    NPH_ARRAY_CLEAR(&out->bricks);
    // One packed brick a brick, at its index (zero for any no cluster names).
    NphResult r = NPH_ARRAY_RESIZE(&out->bricks, brick_count);
    for (u32 i = 0; i < meshlet_count && r == NPH_RESULT_OK; ++i) {
        NphRenderMeshlet *m = &meshlets[i];
        if (m->vertex_count == 0U && m->triangle_count != 0U) {
            r = pack_cluster(m, bricks, brick_count, voxels, voxel_count, out);
        }
    }
    if (r != NPH_RESULT_OK) {
        NPH_ARRAY_CLEAR(&out->clusters);
        NPH_ARRAY_CLEAR(&out->bricks);
        NPH_ARRAY_CLEAR(&out->words);
    }
    return r;
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
// A ray's first voxel
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
// [t_cell, t_exit] (from the cell's planes: voxel_hit), and the round
// shapes' tests start from t_cell so that their terms keep their precision
// however far the ray came. On a hit, *t0 takes its t and *face the face it
// entered.
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

// The voxel `voxel` of cell `bit` (x + 4y + 16z) of a brick of cell `h`
// against the ray o + t d (brick space; inv the reciprocal of d) for t in
// [t_min, t_max] (vis_voxel.glsl vis_voxel_hit): the cell's span along the
// ray from its own planes, then its footprint. Cells are powers of two and
// their corners small multiples of them, so base + h is exact, as is every
// footprint plane.
static bool voxel_hit(const NphRenderVoxel *voxel, u32 bit, f32 h, const f32 o[3], const f32 d[3],
                      const f32 inv[3], f32 t_min, f32 t_max, f32 *out_t, u32 *out_face)
{
    const f32 base[3] = {(f32)(bit & 3U) * h, (f32)((bit >> 2U) & 3U) * h, (f32)(bit >> 4U) * h};
    f32 enter = t_min;
    f32 leave = t_max;
    for (u32 a = 0; a < 3U; ++a) {
        const f32 ta = (base[a] - o[a]) * inv[a];
        const f32 tb = (base[a] + h - o[a]) * inv[a];
        enter = nph_f32_max(enter, nph_f32_min(ta, tb));
        leave = nph_f32_min(leave, nph_f32_max(ta, tb));
    }
    if (!(enter <= leave)) {
        return false;
    }
    f32 t = t_min;
    if (!footprint(voxel->box, voxel->material, base, h / (f32)NPH_RENDER_VOXEL_BOX_STEPS, o, d,
                   inv, enter, leave, &t, t_max, out_face)) {
        return false;
    }
    *out_t = t;
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
    // Every voxel apart, the nearest hit kept (the lowest cell on a tie).
    bool found = false;
    for (u32 bit = 0; bit < 64U; ++bit) {
        if (!brick_bit(brick, bit)) {
            continue;
        }
        const u32 index = brick->first_voxel + brick_rank(brick, bit);
        f32 t = 0.0F;
        u32 face = 0U;
        if (voxel_hit(&voxels[index], bit, h, o, d, inv, t_min, t_max, &t, &face) &&
            (!found || t < out->t)) {
            found = true;
            out->t = t;
            out->voxel = index;
            out->cell = bit;
            out->face = face;
        }
    }
    return found;
}

bool voxel_footprint_hit(const NphRenderVoxel *voxel, NphVec3 base, f32 h, NphVec3 origin,
                         NphVec3 dir, f32 t_min, f32 t_max)
{
    NPH_ASSERT(voxel != NULL && h > 0.0F);
    const f32 o[3] = {origin.x - base.x, origin.y - base.y, origin.z - base.z};
    f32 d[3] = {dir.x, dir.y, dir.z};
    f32 inv[3];
    for (u32 a = 0; a < 3U; ++a) {
        if (nph_f32_abs(d[a]) < 1e-20F) {
            d[a] = d[a] < 0.0F ? -1e-20F : 1e-20F;
        }
        inv[a] = 1.0F / d[a];
    }
    const f32 zero[3] = {0.0F, 0.0F, 0.0F};
    f32 enter = t_min;
    f32 leave = t_max;
    for (u32 a = 0; a < 3U; ++a) {
        const f32 ta = -o[a] * inv[a];
        const f32 tb = (h - o[a]) * inv[a];
        enter = nph_f32_max(enter, nph_f32_min(ta, tb));
        leave = nph_f32_min(leave, nph_f32_max(ta, tb));
    }
    if (!(enter <= leave)) {
        return false;
    }
    u32 face = 0U;
    f32 t = t_min;
    return footprint(voxel->box, voxel->material, zero, h / (f32)NPH_RENDER_VOXEL_BOX_STEPS, o, d,
                     inv, enter, leave, &t, t_max, &face);
}

bool voxel_footprint_span(const NphRenderVoxel *voxel, NphVec3 base, f32 h, NphVec3 origin,
                          NphVec3 dir, f32 t_min, f32 t_max, f32 *out_enter, f32 *out_leave)
{
    NPH_ASSERT(voxel != NULL && out_enter != NULL && out_leave != NULL && h > 0.0F);
    // The cell's own space (its corner at the origin), where the footprint's
    // planes are exact multiples of its step.
    const f32 o[3] = {origin.x - base.x, origin.y - base.y, origin.z - base.z};
    f32 d[3] = {dir.x, dir.y, dir.z};
    f32 inv[3];
    for (u32 a = 0; a < 3U; ++a) {
        if (nph_f32_abs(d[a]) < 1e-20F) {
            d[a] = d[a] < 0.0F ? -1e-20F : 1e-20F;
        }
        inv[a] = 1.0F / d[a];
    }
    const f32 zero[3] = {0.0F, 0.0F, 0.0F};
    f32 enter = t_min;
    f32 leave = t_max;
    for (u32 a = 0; a < 3U; ++a) {
        const f32 ta = -o[a] * inv[a];
        const f32 tb = (h - o[a]) * inv[a];
        enter = nph_f32_max(enter, nph_f32_min(ta, tb));
        leave = nph_f32_min(leave, nph_f32_max(ta, tb));
    }
    if (!(enter <= leave)) {
        return false;
    }
    const f32 step = h / (f32)NPH_RENDER_VOXEL_BOX_STEPS;
    u32 face = 0U;
    f32 t_in = t_min;
    if (!footprint(voxel->box, voxel->material, zero, step, o, d, inv, enter, leave, &t_in, t_max,
                   &face)) {
        return false;
    }
    // The last point inside: the first hit of the ray run back from where it
    // leaves the cell, down to where it entered the footprint.
    const f32 back_o[3] = {o[0] + leave * d[0], o[1] + leave * d[1], o[2] + leave * d[2]};
    const f32 back_d[3] = {-d[0], -d[1], -d[2]};
    const f32 back_inv[3] = {-inv[0], -inv[1], -inv[2]};
    f32 s_out = 0.0F;
    if (!footprint(voxel->box, voxel->material, zero, step, back_o, back_d, back_inv, 0.0F,
                   leave - enter, &s_out, leave - t_in, &face)) {
        s_out = 0.0F; // a graze: it leaves where it entered
    }
    *out_enter = t_in;
    *out_leave = nph_f32_max(leave - s_out, t_in);
    return true;
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
