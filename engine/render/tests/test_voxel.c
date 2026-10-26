// SPDX-License-Identifier: Apache-2.0
// Voxels (render/voxel.h; docs/render.md "Voxels"; ADR-0061): the records,
// the traversal against a brute-force search of the footprints, the
// shading terms against Monte Carlo integrals of their definitions, the
// voxelizer's footprints and looks against the geometry they stand for, and
// the voxel levels the cluster builder makes of foliage. CPU only.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>
#include <nepheline/render/voxel.h>

#include <ntest/ntest.h>

#include "cluster_lod_internal.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static NphVec3 random_unit(NphPcg32 *rng)
{
    const f32 z = 2.0F * nph_pcg32_next_f32(rng) - 1.0F;
    const f32 phi = 2.0F * NPH_PI * nph_pcg32_next_f32(rng);
    const f32 r = nph_f32_sqrt(nph_f32_max(1.0F - z * z, 0.0F));
    return nph_vec3(r * nph_f32_cos(phi), r * nph_f32_sin(phi), z);
}

// A frame around unit `n`: two unit tangents.
static void tangents(NphVec3 n, NphVec3 *t, NphVec3 *b)
{
    const NphVec3 up = nph_f32_abs(n.z) < 0.9F ? nph_vec3_unit_z() : nph_vec3_unit_x();
    *t = nph_vec3_normalize(nph_vec3_cross(up, n));
    *b = nph_vec3_cross(n, *t);
}

// A direction from the von Mises-Fisher distribution about unit `n` with
// concentration `kappa` (Wood, "Simulation of the von Mises Fisher
// Distribution", 1994: the cosine to the axis by inversion).
static NphVec3 sample_vmf(NphPcg32 *rng, NphVec3 n, f64 kappa)
{
    const f64 u = (f64)nph_pcg32_next_f32(rng);
    f64 w;
    if (kappa < 1e-6) {
        w = 2.0 * u - 1.0;
    } else {
        w = 1.0 + log(u + (1.0 - u) * exp(-2.0 * kappa)) / kappa;
    }
    w = nph_f64_clamp(w, -1.0, 1.0);
    const f64 phi = 2.0 * NPH_F64_PI * (f64)nph_pcg32_next_f32(rng);
    const f64 s = sqrt(nph_f64_max(1.0 - w * w, 0.0));
    NphVec3 t;
    NphVec3 b;
    tangents(n, &t, &b);
    return nph_vec3_add(nph_vec3_scale(n, (f32)w),
                        nph_vec3_add(nph_vec3_scale(t, (f32)(s * cos(phi))),
                                     nph_vec3_scale(b, (f32)(s * sin(phi)))));
}

// A ray against a triangle, either side (Möller & Trumbore, "Fast, Minimum
// Storage Ray/Triangle Intersection", JGT 1997), in double precision.
static bool ray_triangle(NphVec3 o, NphVec3 d, NphVec3 a, NphVec3 b, NphVec3 c, f64 *out_t)
{
    const f64 e1[3] = {(f64)b.x - (f64)a.x, (f64)b.y - (f64)a.y, (f64)b.z - (f64)a.z};
    const f64 e2[3] = {(f64)c.x - (f64)a.x, (f64)c.y - (f64)a.y, (f64)c.z - (f64)a.z};
    const f64 dd[3] = {(f64)d.x, (f64)d.y, (f64)d.z};
    const f64 p[3] = {dd[1] * e2[2] - dd[2] * e2[1], dd[2] * e2[0] - dd[0] * e2[2],
                      dd[0] * e2[1] - dd[1] * e2[0]};
    const f64 det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (nph_f64_abs(det) < 1e-300) {
        return false;
    }
    const f64 inv = 1.0 / det;
    const f64 s[3] = {(f64)o.x - (f64)a.x, (f64)o.y - (f64)a.y, (f64)o.z - (f64)a.z};
    const f64 u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    const f64 q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                      s[0] * e1[1] - s[1] * e1[0]};
    const f64 v = (dd[0] * q[0] + dd[1] * q[1] + dd[2] * q[2]) * inv;
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }
    *out_t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return *out_t >= 0.0;
}

// ---------------------------------------------------------------------------
// The records
// ---------------------------------------------------------------------------

NTEST(Voxel, a_voxel_packs_to_its_encodings)
{
    NphRenderVoxelDesc d;
    memset(&d, 0, sizeof d);
    d.box_lo = nph_vec3(0.1F, 0.25F, 0.5F);
    d.box_hi = nph_vec3(0.6F, 1.0F, 0.52F);
    d.shape = NPH_RENDER_VOXEL_SHAPE_CAPSULE;
    d.diagonal = 2U;
    d.radius = 0.1F;
    d.albedo = nph_vec3(0.2F, 0.5F, 0.9F);
    d.occlusion = 0.75F;
    d.axis = nph_vec3_normalize(nph_vec3(1.0F, 2.0F, 3.0F));
    d.kappa = 30.0F;
    d.roughness = 0.4F;
    d.metallic = 0.1F;
    const NphRenderVoxel v = nph_render_voxel_pack(&d);
    NphRenderVoxelDesc u;
    nph_render_voxel_unpack(v, &u);
    // The nearest 32nds, a step apart at least (0.52 rounds up past 0.5).
    NTEST_EXPECT_NEAR_F32(u.box_lo.x, 3.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_lo.y, 8.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_lo.z, 16.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_hi.x, 19.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_hi.y, 1.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_hi.z, 17.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_EQ_UINT(u.shape, NPH_RENDER_VOXEL_SHAPE_CAPSULE);
    NTEST_EXPECT_EQ_UINT(u.diagonal, 2U);
    // 2^(c / 8 - 8) nearest 0.1: c = 8 (8 + log2 0.1) = 37.4, to 37.
    NTEST_EXPECT_NEAR_F32(u.radius, nph_f32_exp2(37.0F / 8.0F - 8.0F), 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.albedo.x, 0.2F, 0.005F);
    NTEST_EXPECT_NEAR_F32(u.albedo.y, 0.5F, 0.005F);
    NTEST_EXPECT_NEAR_F32(u.albedo.z, 0.9F, 0.005F);
    NTEST_EXPECT_NEAR_F32(u.occlusion, 0.75F, 0.5F / 255.0F + 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_dot(u.axis, d.axis) > 0.99999F);
    // Half a byte's step of log2(1 + kappa) is a part in 2^(1/32) - 1.
    NTEST_EXPECT_NEAR_F32((1.0F + u.kappa) / 31.0F, 1.0F, 0.022F);
    NTEST_EXPECT_NEAR_F32(u.roughness, 0.4F, 0.5F / 255.0F + 1e-6F);
    NTEST_EXPECT_NEAR_F32(u.metallic, 0.1F, 0.5F / 255.0F + 1e-6F);
    // An empty box is held to a step.
    d.box_lo = nph_vec3(0.5F, 0.5F, 1.0F);
    d.box_hi = nph_vec3(0.5F, 0.4F, 1.0F);
    d.shape = NPH_RENDER_VOXEL_SHAPE_BOX;
    nph_render_voxel_unpack(nph_render_voxel_pack(&d), &u);
    NTEST_EXPECT_NEAR_F32(u.box_hi.x - u.box_lo.x, 1.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_hi.y - u.box_lo.y, 1.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_lo.z, 31.0F / 32.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(u.box_hi.z, 1.0F, 1e-7F);
    NTEST_EXPECT_EQ_UINT(u.shape, NPH_RENDER_VOXEL_SHAPE_BOX);
    NTEST_EXPECT_NEAR_F32(u.radius, 0.0F, 0.0F);
    // A capsule's radius is held to [1/256, 1/2], and each code is the
    // nearest for its own radius.
    d.shape = NPH_RENDER_VOXEL_SHAPE_CAPSULE;
    d.radius = 0.0F;
    nph_render_voxel_unpack(nph_render_voxel_pack(&d), &u);
    NTEST_EXPECT_NEAR_F32(u.radius, 1.0F / 256.0F, 1e-9F);
    d.radius = 2.0F;
    nph_render_voxel_unpack(nph_render_voxel_pack(&d), &u);
    NTEST_EXPECT_NEAR_F32(u.radius, 0.5F, 1e-7F);
    for (u32 c = 0; c < NPH_RENDER_VOXEL_RADIUS_CODES; ++c) {
        NTEST_EXPECT_EQ_UINT(nph_render_voxel_radius_encode(nph_render_voxel_radius_decode(c)), c);
    }
}

NTEST(Voxel, the_concentrations_bytes_and_moments_invert)
{
    for (u32 b = 0; b < 256U; ++b) {
        NTEST_EXPECT_EQ_UINT(nph_render_voxel_kappa_encode(nph_render_voxel_kappa_decode(b)), b);
    }
    NTEST_EXPECT_EQ_UINT(nph_render_voxel_kappa_encode(NPH_RENDER_VOXEL_KAPPA_MAX), 255U);
    NTEST_EXPECT_EQ_UINT(nph_render_voxel_kappa_encode(-1.0F), 0U);
    NTEST_EXPECT_NEAR_F32(nph_render_voxel_moment(0.0F), 1.0F / 3.0F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(nph_render_voxel_moment(1e4F), 1.0F - 2.0F / 1e4F, 1e-6F);
    const f32 ks[] = {0.01F, 0.1F, 1.0F, 3.0F, 10.0F, 100.0F, 1000.0F, 30000.0F};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(ks); ++i) {
        const f32 m = nph_render_voxel_moment(ks[i]);
        const f32 k = nph_render_voxel_kappa_from_moment(m);
        NTEST_LOG("kappa %g: moment %.7f -> %g", (f64)ks[i], (f64)m, (f64)k);
        NTEST_EXPECT_NEAR_F32(k / ks[i], 1.0F, 2e-3F);
    }
    NTEST_EXPECT_EQ_INT((int)(nph_render_voxel_kappa_from_moment(0.2F) * 1000.0F), 0);
    NTEST_EXPECT_NEAR_F32(nph_render_voxel_kappa_from_moment(1.0F), NPH_RENDER_VOXEL_KAPPA_MAX,
                          0.0F);
}

// ---------------------------------------------------------------------------
// The traversal
// ---------------------------------------------------------------------------

static NphRenderVoxel box_voxel(u32 lo, u32 hi, u32 shape)
{
    NphRenderVoxel v;
    memset(&v, 0, sizeof v);
    for (u32 a = 0; a < 3U; ++a) {
        v.box |= (lo << (5U * a)) | ((hi - 1U) << (15U + 5U * a));
    }
    v.box |= shape << NPH_RENDER_VOXEL_SHAPE_SHIFT;
    return v;
}

NTEST(Voxel, a_ray_meets_the_first_footprint_it_crosses)
{
    // A brick of half-unit cells from (1, 2, 3): cell (1, 2, 3) whole, cell
    // (2, 2, 3) a box over its middle half.
    NphRenderVoxelBrick brick;
    memset(&brick, 0, sizeof brick);
    brick.origin = nph_vec3(1.0F, 2.0F, 3.0F);
    brick.cell = 0.5F;
    const u32 full = 1U + 4U * 2U + 16U * 3U; // bit 57
    const u32 part = 2U + 4U * 2U + 16U * 3U; // bit 58
    brick.mask[full >> 5U] |= 1U << (full & 31U);
    brick.mask[part >> 5U] |= 1U << (part & 31U);
    NphRenderVoxel voxels[2] = {box_voxel(0U, 32U, NPH_RENDER_VOXEL_SHAPE_BOX),
                                box_voxel(8U, 24U, NPH_RENDER_VOXEL_SHAPE_BOX)};
    // Through both cells' centres along +x: the whole cell first.
    const f32 y = 2.0F + 2.5F * 0.5F;
    const f32 z = 3.0F + 3.5F * 0.5F;
    NphRenderVoxelHit hit;
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(-10.0F, y, z),
                                             nph_vec3(1.0F, 0.0F, 0.0F), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_NEAR_F32(hit.t, 11.5F, 1e-5F);
    NTEST_EXPECT_EQ_UINT(hit.voxel, 0U);
    NTEST_EXPECT_EQ_UINT(hit.cell, full);
    NTEST_EXPECT_EQ_UINT(hit.face, 0U);
    // Along -x: the middle box's high face first.
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(10.0F, y, z),
                                             nph_vec3(-1.0F, 0.0F, 0.0F), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_NEAR_F32(hit.t, 10.0F - 2.375F, 1e-5F);
    NTEST_EXPECT_EQ_UINT(hit.voxel, 1U);
    NTEST_EXPECT_EQ_UINT(hit.face, 4U);
    // Past the middle box's edge, on to the whole cell's high face.
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(10.0F, 3.05F, z),
                                             nph_vec3(-1.0F, 0.0F, 0.0F), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_NEAR_F32(hit.t, 8.0F, 1e-5F);
    NTEST_EXPECT_EQ_UINT(hit.voxel, 0U);
    // The range bounds the hit.
    NTEST_EXPECT_FALSE(nph_render_voxel_trace(&brick, voxels, nph_vec3(-10.0F, y, z),
                                              nph_vec3(1.0F, 0.0F, 0.0F), 0.0F, 11.4F, &hit));
    NTEST_EXPECT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(-10.0F, y, z),
                                             nph_vec3(1.0F, 0.0F, 0.0F), 0.0F, 11.5F, &hit));
    // A ray that starts inside a footprint meets it where it starts.
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(1.75F, y, z),
                                             nph_vec3(0.0F, 1.0F, 0.0F), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_NEAR_F32(hit.t, 0.0F, 0.0F);
    // Beside the brick: nothing.
    NTEST_EXPECT_FALSE(nph_render_voxel_trace(&brick, voxels, nph_vec3(-10.0F, 0.0F, z),
                                              nph_vec3(1.0F, 0.0F, 0.0F), 0.0F, 1e30F, &hit));
    // The middle footprint as an ellipsoid: its centre's ray meets it at the
    // same face; one through the box's corner passes it.
    voxels[1] = box_voxel(8U, 24U, NPH_RENDER_VOXEL_SHAPE_ELLIPSOID);
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(10.0F, y, z),
                                             nph_vec3(-1.0F, 0.0F, 0.0F), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_NEAR_F32(hit.t, 10.0F - 2.375F, 1e-5F);
    NTEST_EXPECT_EQ_UINT(hit.voxel, 1U);
    NTEST_EXPECT_EQ_UINT(hit.face, 4U);
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(10.0F, 3.13F, 4.63F),
                                             nph_vec3(-1.0F, 0.0F, 0.0F), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_EQ_UINT(hit.voxel, 0U);
    NTEST_EXPECT_NEAR_F32(hit.t, 8.0F, 1e-5F);
    // As a capsule of radius 1/16 of the cell from the box's corner (lo, lo,
    // lo) to (hi, hi, hi): a ray down its axis (from the high end, where no
    // other voxel lies) meets its round end a radius past the corner; one
    // across its middle meets its side.
    voxels[1] = box_voxel(8U, 24U, NPH_RENDER_VOXEL_SHAPE_CAPSULE);
    voxels[1].material = 0U | (32U << 26U); // diagonal 0, radius 2^(32 / 8 - 8) = 1/16
    const NphVec3 lo = nph_vec3(2.125F, 3.125F, 4.625F);
    const NphVec3 hi = nph_vec3(2.375F, 3.375F, 4.875F);
    const NphVec3 mid = nph_vec3_scale(nph_vec3_add(lo, hi), 0.5F);
    const NphVec3 along = nph_vec3_normalize(nph_vec3_sub(hi, lo));
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3_add(mid, along),
                                             nph_vec3_neg(along), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_EQ_UINT(hit.voxel, 1U);
    NTEST_EXPECT_NEAR_F32(hit.t, 1.0F - nph_vec3_distance(mid, hi) - 0.5F / 16.0F, 1e-5F);
    NphVec3 across = nph_vec3_normalize(nph_vec3(1.0F, -1.0F, 0.0F));
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3_sub(mid, across), across,
                                             0.0F, 1e30F, &hit));
    NTEST_EXPECT_EQ_UINT(hit.voxel, 1U);
    NTEST_EXPECT_NEAR_F32(hit.t, 1.0F - 0.5F / 16.0F, 1e-5F);
    // Beside its side, the ray passes it to the whole cell behind.
    const NphVec3 beside =
        nph_vec3_add(mid, nph_vec3_scale(nph_vec3_normalize(nph_vec3(1.0F, 1.0F, -2.0F)), 0.06F));
    NTEST_ASSERT_TRUE(nph_render_voxel_trace(&brick, voxels, nph_vec3(10.0F, beside.y, beside.z),
                                             nph_vec3(-1.0F, 0.0F, 0.0F), 0.0F, 1e30F, &hit));
    NTEST_EXPECT_EQ_UINT(hit.voxel, 0U);
}

// The squared distance from p to the segment a b (double precision).
static f64 segment_distance2(const f64 p[3], const f64 a[3], const f64 b[3])
{
    f64 ab[3];
    f64 ap[3];
    for (u32 k = 0; k < 3U; ++k) {
        ab[k] = b[k] - a[k];
        ap[k] = p[k] - a[k];
    }
    const f64 len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    const f64 y =
        len2 > 0.0 ? nph_f64_clamp((ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2, 0.0, 1.0)
                   : 0.0;
    f64 d2 = 0.0;
    for (u32 k = 0; k < 3U; ++k) {
        const f64 q = ap[k] - y * ab[k];
        d2 += q * q;
    }
    return d2;
}

// The ray's interval inside the capsule a b of radius r: the distance from
// the segment, a convex function of t, has its least at the ternary search's
// end, and each side's root is bisected. False where it never comes within r.
static bool capsule_interval(const f64 o[3], const f64 d[3], const f64 a[3], const f64 b[3], f64 r,
                             f64 *out_in, f64 *out_out)
{
    f64 lo = -1e6;
    f64 hi = 1e6;
    for (u32 i = 0; i < 300U; ++i) {
        const f64 m1 = lo + (hi - lo) / 3.0;
        const f64 m2 = hi - (hi - lo) / 3.0;
        f64 p1[3];
        f64 p2[3];
        for (u32 k = 0; k < 3U; ++k) {
            p1[k] = o[k] + m1 * d[k];
            p2[k] = o[k] + m2 * d[k];
        }
        if (segment_distance2(p1, a, b) < segment_distance2(p2, a, b)) {
            hi = m2;
        } else {
            lo = m1;
        }
    }
    const f64 best = 0.5 * (lo + hi);
    f64 pb[3];
    for (u32 k = 0; k < 3U; ++k) {
        pb[k] = o[k] + best * d[k];
    }
    if (segment_distance2(pb, a, b) > r * r) {
        return false;
    }
    for (u32 side = 0; side < 2U; ++side) {
        f64 inside = best;
        f64 outside = side == 0U ? -1e6 : 1e6;
        for (u32 i = 0; i < 200U; ++i) {
            const f64 m = 0.5 * (inside + outside);
            f64 pm[3];
            for (u32 k = 0; k < 3U; ++k) {
                pm[k] = o[k] + m * d[k];
            }
            if (segment_distance2(pm, a, b) <= r * r) {
                inside = m;
            } else {
                outside = m;
            }
        }
        if (side == 0U) {
            *out_in = inside;
        } else {
            *out_out = inside;
        }
    }
    return true;
}

// The footprint of voxel `v` in cell `bit` of `brick` against the ray, as a
// search of every footprint would test it (double precision).
static bool footprint_hit(const NphRenderVoxelBrick *brick, NphRenderVoxel v, u32 bit, NphVec3 o,
                          NphVec3 d, f64 t_min, f64 t_max, f64 *out_t)
{
    const f64 h = (f64)brick->cell;
    const u32 c[3] = {bit & 3U, (bit >> 2U) & 3U, bit >> 4U};
    const f64 org[3] = {(f64)o.x, (f64)o.y, (f64)o.z};
    const f64 dir[3] = {(f64)d.x, (f64)d.y, (f64)d.z};
    const f64 base[3] = {(f64)brick->origin.x, (f64)brick->origin.y, (f64)brick->origin.z};
    f64 lo[3];
    f64 hi[3];
    f64 t0 = t_min;
    f64 t1 = t_max;
    for (u32 a = 0; a < 3U; ++a) {
        lo[a] = base[a] + ((f64)c[a] + (f64)((v.box >> (5U * a)) & 31U) / 32.0) * h;
        hi[a] = base[a] + ((f64)c[a] + (f64)(((v.box >> (15U + 5U * a)) & 31U) + 1U) / 32.0) * h;
        if (dir[a] == 0.0) {
            if (org[a] < lo[a] || org[a] > hi[a]) {
                t1 = -1.0; // outside the box (a capsule reaches past it)
            }
            continue;
        }
        const f64 ta = (lo[a] - org[a]) / dir[a];
        const f64 tb = (hi[a] - org[a]) / dir[a];
        t0 = nph_f64_max(t0, nph_f64_min(ta, tb));
        t1 = nph_f64_min(t1, nph_f64_max(ta, tb));
    }
    const u32 shape = v.box >> NPH_RENDER_VOXEL_SHAPE_SHIFT;
    if (t0 > t1 && shape != NPH_RENDER_VOXEL_SHAPE_CAPSULE) {
        return false;
    }
    if (shape == NPH_RENDER_VOXEL_SHAPE_CAPSULE) {
        const u32 diag = (v.material >> 24U) & 3U;
        const f64 r = pow(2.0, (f64)((v.material >> 26U) & 63U) / 8.0 - 8.0) * h;
        const f64 a[3] = {(diag & 1U) != 0U ? hi[0] : lo[0], (diag & 2U) != 0U ? hi[1] : lo[1],
                          lo[2]};
        const f64 b[3] = {(diag & 1U) != 0U ? lo[0] : hi[0], (diag & 2U) != 0U ? lo[1] : hi[1],
                          hi[2]};
        // Clipped to the cell, not the box.
        f64 c0 = t_min;
        f64 c1 = t_max;
        for (u32 ax = 0; ax < 3U; ++ax) {
            const f64 cl = base[ax] + (f64)c[ax] * h;
            const f64 ch = cl + h;
            if (dir[ax] == 0.0) {
                if (org[ax] < cl || org[ax] > ch) {
                    return false;
                }
                continue;
            }
            const f64 ta = (cl - org[ax]) / dir[ax];
            const f64 tb = (ch - org[ax]) / dir[ax];
            c0 = nph_f64_max(c0, nph_f64_min(ta, tb));
            c1 = nph_f64_min(c1, nph_f64_max(ta, tb));
        }
        f64 cin = 0.0;
        f64 cout = 0.0;
        if (c0 > c1 || !capsule_interval(org, dir, a, b, r, &cin, &cout) || cout < c0 || cin > c1) {
            return false;
        }
        *out_t = nph_f64_max(cin, c0);
        return true;
    }
    if (shape == NPH_RENDER_VOXEL_SHAPE_ELLIPSOID) {
        f64 qq = 0.0;
        f64 qk = 0.0;
        f64 kk = 0.0;
        for (u32 a = 0; a < 3U; ++a) {
            const f64 half = 0.5 * (hi[a] - lo[a]);
            const f64 q = (org[a] - 0.5 * (lo[a] + hi[a])) / half;
            const f64 k = dir[a] / half;
            qq += q * q;
            qk += q * k;
            kk += k * k;
        }
        const f64 disc = qk * qk - kk * (qq - 1.0);
        if (disc < 0.0) {
            return false;
        }
        const f64 te = (-qk - sqrt(disc)) / kk;
        const f64 tx = (-qk + sqrt(disc)) / kk;
        if (tx < t0 || te > t1) {
            return false;
        }
        t0 = nph_f64_max(te, t0);
    }
    *out_t = t0;
    return true;
}

NTEST(Voxel, the_traversal_meets_the_nearest_footprint_of_random_bricks)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 61U, 1U);
    NphRenderVoxel voxels[64];
    u32 rays = 0U;
    u32 hits = 0U;
    u32 mismatched = 0U;
    f64 worst = 0.0;
    for (u32 trial = 0; trial < 400U; ++trial) {
        NphRenderVoxelBrick brick;
        memset(&brick, 0, sizeof brick);
        brick.cell = ldexpf(1.0F, (int)(nph_pcg32_next_u32(&rng) % 8U) - 4);
        brick.origin = nph_vec3((nph_pcg32_next_f32(&rng) - 0.5F) * 20.0F,
                                (nph_pcg32_next_f32(&rng) - 0.5F) * 20.0F,
                                (nph_pcg32_next_f32(&rng) - 0.5F) * 20.0F);
        u32 count = 0U;
        u32 cell_of[64];
        for (u32 bit = 0; bit < 64U; ++bit) {
            if (nph_pcg32_next_f32(&rng) < 0.5F) {
                brick.mask[bit >> 5U] |= 1U << (bit & 31U);
                NphRenderVoxel v;
                memset(&v, 0, sizeof v);
                for (u32 a = 0; a < 3U; ++a) {
                    const u32 l = nph_pcg32_next_u32(&rng) % 32U;
                    const u32 h = l + 1U + nph_pcg32_next_u32(&rng) % (32U - l);
                    v.box |= (l << (5U * a)) | ((h - 1U) << (15U + 5U * a));
                }
                const u32 shape = nph_pcg32_next_u32(&rng) % 3U;
                v.box |= shape << NPH_RENDER_VOXEL_SHAPE_SHIFT;
                if (shape == NPH_RENDER_VOXEL_SHAPE_CAPSULE) {
                    v.material =
                        (nph_pcg32_next_u32(&rng) % 4U) |
                        ((nph_pcg32_next_u32(&rng) % NPH_RENDER_VOXEL_RADIUS_CODES) << 26U);
                }
                cell_of[count] = bit;
                voxels[count++] = v;
            }
        }
        const f32 side = 4.0F * brick.cell;
        const NphVec3 centre = nph_vec3_add(brick.origin, nph_vec3_splat(0.5F * side));
        for (u32 k = 0; k < 64U; ++k) {
            // From around the brick, toward a point of it; one ray in eight
            // along an axis, so that zero components are taken too.
            const NphVec3 from =
                nph_vec3_add(centre, nph_vec3_scale(random_unit(&rng), 3.0F * side));
            const NphVec3 to =
                nph_vec3_add(brick.origin, nph_vec3(nph_pcg32_next_f32(&rng) * side,
                                                    nph_pcg32_next_f32(&rng) * side,
                                                    nph_pcg32_next_f32(&rng) * side));
            NphVec3 d = nph_vec3_normalize(nph_vec3_sub(to, from));
            if (k % 8U == 0U) {
                const u32 a = nph_pcg32_next_u32(&rng) % 3U;
                f32 dd[3] = {0.0F, 0.0F, 0.0F};
                dd[a] = nph_vec3_at(d, a) < 0.0F ? -1.0F : 1.0F;
                d = nph_vec3(dd[0], dd[1], dd[2]);
            }
            NphRenderVoxelHit hit;
            const bool got = nph_render_voxel_trace(&brick, voxels, from, d, 0.0F, 1e30F, &hit);
            f64 best = NPH_F64_MAX;
            u32 best_voxel = NPH_U32_MAX;
            for (u32 i = 0; i < count; ++i) {
                f64 t = 0.0;
                if (footprint_hit(&brick, voxels[i], cell_of[i], from, d, 0.0, 1e30, &t) &&
                    t < best) {
                    best = t;
                    best_voxel = i;
                }
            }
            rays += 1U;
            const f64 tol = 1e-4 * (f64)side;
            if (got != (best_voxel != NPH_U32_MAX)) {
                mismatched += 1U;
                NTEST_LOG("trial %u ray %u: traversal %s, search %s (voxel %u at %.9g)", trial, k,
                          got ? "hit" : "missed", best_voxel != NPH_U32_MAX ? "hit" : "missed",
                          best_voxel, best);
                continue;
            }
            if (!got) {
                continue;
            }
            hits += 1U;
            const f64 err = nph_f64_abs((f64)hit.t - best);
            worst = nph_f64_max(worst, err / (f64)side);
            if (err > tol) {
                mismatched += 1U;
                NTEST_LOG("trial %u ray %u: t %.9g against %.9g (voxel %u against %u)", trial, k,
                          (f64)hit.t, best, hit.voxel, best_voxel);
            } else if (hit.voxel != best_voxel) {
                // Two footprints met within the rounding: either is the first.
                f64 t = 0.0;
                NTEST_EXPECT_TRUE(footprint_hit(&brick, voxels[hit.voxel], cell_of[hit.voxel], from,
                                                d, 0.0, 1e30, &t) &&
                                  nph_f64_abs(t - best) <= tol);
            }
        }
    }
    NTEST_LOG("%u rays, %u hits, %u mismatched, worst t error %.3g of a brick", rays, hits,
              mismatched, worst);
    NTEST_EXPECT_GT_UINT(hits, rays / 10U);
    NTEST_EXPECT_EQ_UINT(mismatched, 0U);
}

// ---------------------------------------------------------------------------
// The shading terms
// ---------------------------------------------------------------------------

// The diffuse response's definition, by Monte Carlo: normals from the vMF
// lobe, each weighed by its cosine to the viewer.
static f64 diffuse_integral(NphPcg32 *rng, NphVec3 n, f64 kappa, NphVec3 v, NphVec3 l, u32 samples)
{
    f64 num = 0.0;
    f64 den = 0.0;
    for (u32 i = 0; i < samples; ++i) {
        const NphVec3 m = sample_vmf(rng, n, kappa);
        const f64 mv = nph_f64_max((f64)nph_vec3_dot(m, v), 0.0);
        num += mv * nph_f64_max((f64)nph_vec3_dot(m, l), 0.0);
        den += mv;
    }
    return num / den;
}

// The model's error against its definition (docs/render.md "Voxels"): its
// mean over lobes and directions is 0.7 % of a facing light's response, at
// most 5 % for a viewer within 60 degrees of the lobe's axis, and at most
// 8.3 % at a grazing view (84 degrees, a lobe of kappa near 16), where the
// viewer's Gaussian does not cut the lobe at its horizon as the cosine does
// (measured by quadrature over a grid of views and lights; the tests' Monte
// Carlo adds its own noise of about 0.002).
NTEST(Voxel, the_diffuse_response_is_its_integral)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 7U, 3U);
    const f32 kappas[] = {0.0F, 0.5F, 2.0F, 8.0F, 16.0F, 32.0F, 200.0F, 5000.0F};
    f64 worst = 0.0;
    f64 worst_within_60 = 0.0;
    f64 sum = 0.0;
    u32 count = 0U;
    for (u32 k = 0; k < NPH_ARRAY_COUNT(kappas); ++k) {
        f64 worst_k = 0.0;
        for (u32 trial = 0; trial < 8U; ++trial) {
            const NphVec3 n = nph_vec3_unit_z();
            NphVec3 v = random_unit(&rng);
            v = v.z < 0.0F ? nph_vec3(v.x, v.y, -v.z) : v; // the lobe faces the viewer
            const NphVec3 l = random_unit(&rng);
            const f64 want = diffuse_integral(&rng, n, (f64)kappas[k], v, l, 120000U);
            const f64 err = nph_f64_abs((f64)nph_render_voxel_diffuse(n, kappas[k], v, l) - want);
            worst_k = nph_f64_max(worst_k, err);
            if (v.z >= 0.5F) {
                worst_within_60 = nph_f64_max(worst_within_60, err);
            }
            sum += err;
            count += 1U;
        }
        NTEST_LOG("kappa %g: worst |error| %.4f", (f64)kappas[k], worst_k);
        worst = nph_f64_max(worst, worst_k);
    }
    // A narrow lobe is the cosine of its axis.
    for (u32 trial = 0; trial < 64U; ++trial) {
        const NphVec3 n = random_unit(&rng);
        NphVec3 v = random_unit(&rng);
        v = nph_vec3_dot(v, n) < 0.0F ? nph_vec3_neg(v) : v;
        const NphVec3 l = random_unit(&rng);
        const f32 got = nph_render_voxel_diffuse(n, NPH_RENDER_VOXEL_KAPPA_MAX, v, l);
        NTEST_EXPECT_NEAR_F32(got, nph_f32_max(nph_vec3_dot(n, l), 0.0F), 0.02F);
    }
    NTEST_LOG("|error|: mean %.4f, worst %.4f, worst within 60 degrees %.4f", sum / (f64)count,
              worst, worst_within_60);
    NTEST_EXPECT_TRUE(sum / (f64)count < 0.015);
    NTEST_EXPECT_TRUE(worst < 0.09);
    NTEST_EXPECT_TRUE(worst_within_60 < 0.055);
}

NTEST(Voxel, the_lobes_harmonic_bands_are_its_legendre_means)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 11U, 5U);
    const f32 kappas[] = {0.0F, 0.3F, 1.0F, 4.0F, 20.0F, 300.0F};
    for (u32 k = 0; k < NPH_ARRAY_COUNT(kappas); ++k) {
        f64 p1 = 0.0;
        f64 p2 = 0.0;
        const u32 samples = 400000U;
        for (u32 i = 0; i < samples; ++i) {
            const f64 c = (f64)sample_vmf(&rng, nph_vec3_unit_z(), (f64)kappas[k]).z;
            p1 += c;
            p2 += 0.5 * (3.0 * c * c - 1.0);
        }
        p1 /= (f64)samples;
        p2 /= (f64)samples;
        NTEST_LOG("kappa %g: P1 %.4f / %.4f, P2 %.4f / %.4f", (f64)kappas[k], p1,
                  (f64)nph_render_voxel_sh_band1(kappas[k]), p2,
                  (f64)nph_render_voxel_sh_band2(kappas[k]));
        NTEST_EXPECT_NEAR_F64((f64)nph_render_voxel_sh_band1(kappas[k]), p1, 0.005);
        NTEST_EXPECT_NEAR_F64((f64)nph_render_voxel_sh_band2(kappas[k]), p2, 0.005);
    }
    NTEST_EXPECT_NEAR_F32(nph_render_voxel_alpha2(0.25F, 0.0F), 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(nph_render_voxel_alpha2(0.25F, 8.0F), 0.5F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(nph_render_voxel_alpha2(0.25F, 1.0F), 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(nph_render_voxel_alpha2(0.01F, NPH_RENDER_VOXEL_KAPPA_MAX), 0.01F, 1e-4F);
}

// ---------------------------------------------------------------------------
// The voxelizer
// ---------------------------------------------------------------------------

typedef struct VoxelFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} VoxelFixture;

typedef VoxelFixture VoxelBuild;

NTEST_SETUP(VoxelBuild)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(VoxelBuild)
{
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// Triangles to voxelize: positions, and the sources over them.
typedef struct Geometry {
    NPH_ARRAY(NphVec3) positions;
    LodVoxelSourceArray sources;
} Geometry;

static void geo_init(Geometry *g, NphAllocator *a)
{
    NPH_ARRAY_INIT(&g->positions, a);
    NPH_ARRAY_INIT(&g->sources, a);
}

static void geo_free(Geometry *g)
{
    NPH_ARRAY_FREE(&g->positions);
    NPH_ARRAY_FREE(&g->sources);
}

static void geo_tri(Geometry *g, NphVec3 p0, NphVec3 p1, NphVec3 p2, u32 submesh)
{
    const u32 base = g->positions.len;
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&g->positions, p0));
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&g->positions, p1));
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&g->positions, p2));
    const LodVoxelSource s = {{base, base + 1U, base + 2U}, submesh};
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&g->sources, s));
}

// The parallelogram o, o + u, o + u + v, o + v as two triangles.
static void geo_quad(Geometry *g, NphVec3 o, NphVec3 u, NphVec3 v, u32 submesh)
{
    const NphVec3 ou = nph_vec3_add(o, u);
    const NphVec3 ouv = nph_vec3_add(ou, v);
    const NphVec3 ov = nph_vec3_add(o, v);
    geo_tri(g, o, ou, ouv, submesh);
    geo_tri(g, o, ouv, ov, submesh);
}

// A needle: a thin triangle `length` long and `width` wide at `c` along a
// random direction.
static void geo_needle(Geometry *g, NphPcg32 *rng, NphVec3 c, f32 length, f32 width, u32 submesh)
{
    const NphVec3 d = random_unit(rng);
    NphVec3 t;
    NphVec3 b;
    tangents(d, &t, &b);
    const f32 phi = 2.0F * NPH_PI * nph_pcg32_next_f32(rng);
    const NphVec3 w =
        nph_vec3_add(nph_vec3_scale(t, nph_f32_cos(phi)), nph_vec3_scale(b, nph_f32_sin(phi)));
    const NphVec3 tip = nph_vec3_add(c, nph_vec3_scale(d, 0.5F * length));
    const NphVec3 root = nph_vec3_sub(c, nph_vec3_scale(d, 0.5F * length));
    geo_tri(g, tip, nph_vec3_add(root, nph_vec3_scale(w, 0.5F * width)),
            nph_vec3_sub(root, nph_vec3_scale(w, 0.5F * width)), submesh);
}

// The test's materials: a colour per submesh (or one rising with x), the
// triangle's own normal, roughness 0.5; a masked submesh covers where x is
// at least `mask_x`.
typedef struct TestLook {
    const NphVec3 *positions;
    NphVec3 color[2];
    bool by_x;
    f32 mask_x;
    u8 surface[2];
} TestLook;

static NphVec3 point_of(const NphVec3 *pos, const u32 v[3], NphVec3 b)
{
    return nph_vec3_add(
        nph_vec3_add(nph_vec3_scale(pos[v[0]], b.x), nph_vec3_scale(pos[v[1]], b.y)),
        nph_vec3_scale(pos[v[2]], b.z));
}

static void look_sample(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint,
                        NphRenderVoxelSample *out)
{
    (void)footprint;
    const TestLook *t = (const TestLook *)user;
    const NphVec3 *pos = t->positions;
    const NphVec3 p = point_of(pos, v, b);
    out->base_color =
        t->by_x ? nph_vec3(nph_f32_saturate(p.x / 4.0F), 0.0F, nph_f32_saturate(1.0F - p.x / 4.0F))
                : t->color[submesh];
    out->normal = nph_vec3_normalize(
        nph_vec3_cross(nph_vec3_sub(pos[v[1]], pos[v[0]]), nph_vec3_sub(pos[v[2]], pos[v[0]])));
    out->roughness = 0.5F;
    out->metallic = 0.0F;
    out->occlusion = 1.0F;
}

static bool look_covers(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint)
{
    (void)submesh;
    (void)footprint;
    const TestLook *t = (const TestLook *)user;
    return point_of(t->positions, v, b).x >= t->mask_x;
}

static NphRenderVoxelSampler test_sampler(TestLook *t)
{
    NphRenderVoxelSampler s;
    memset(&s, 0, sizeof s);
    s.user = t;
    s.submesh_surface = t->surface;
    s.sample = look_sample;
    s.covers = look_covers;
    return s;
}

static void look_init(TestLook *t, const NphVec3 *positions)
{
    memset(t, 0, sizeof *t);
    t->positions = positions;
    t->color[0] = nph_vec3(0.8F, 0.1F, 0.1F);
    t->color[1] = nph_vec3(0.1F, 0.8F, 0.1F);
    t->surface[0] = NPH_RENDER_VOXEL_SURFACE_OPAQUE;
    t->surface[1] = NPH_RENDER_VOXEL_SURFACE_OPAQUE;
}

static NphResult voxelize(const Geometry *g, const NphRenderVoxelSampler *sampler, NphAllocator *a,
                          f64 cell, LodVoxelCellArray *cells)
{
    LodVoxelInput in;
    memset(&in, 0, sizeof in);
    in.positions = g->positions.data;
    in.sources = g->sources.data;
    in.source_count = g->sources.len;
    in.sampler = sampler;
    in.scratch = a;
    u32 *home = NPH_ALLOC_ARRAY(a, u32, nph_max_u32(g->sources.len, 1U));
    const NphResult r = lod_voxelize(&in, cell, cells, home);
    NPH_FREE_ARRAY(a, u32, home, nph_max_u32(g->sources.len, 1U));
    return r;
}

// A ray against one cell's footprint (double precision), as
// footprint_hit has it for a brick.
static bool cell_hit(const LodVoxelCell *c, f64 cell, NphVec3 o, NphVec3 d, f64 *out_t)
{
    NphRenderVoxelBrick brick;
    memset(&brick, 0, sizeof brick);
    brick.origin = nph_vec3((f32)((f64)c->c[0] * cell), (f32)((f64)c->c[1] * cell),
                            (f32)((f64)c->c[2] * cell));
    brick.cell = (f32)cell;
    return footprint_hit(&brick, c->voxel, 0U, o, d, 0.0, 1e30, out_t);
}

static bool cells_hit(const LodVoxelCellArray *cells, f64 cell, NphVec3 o, NphVec3 d)
{
    for (u32 i = 0; i < cells->len; ++i) {
        f64 t = 0.0;
        if (cell_hit(&cells->data[i], cell, o, d, &t)) {
            return true;
        }
    }
    return false;
}

static bool triangles_hit(const Geometry *g, NphVec3 o, NphVec3 d)
{
    for (u32 s = 0; s < g->sources.len; ++s) {
        const u32 *v = g->sources.data[s].v;
        f64 t = 0.0;
        if (ray_triangle(o, d, g->positions.data[v[0]], g->positions.data[v[1]],
                         g->positions.data[v[2]], &t)) {
            return true;
        }
    }
    return false;
}

NTEST_F(VoxelBuild, a_sheet_across_cells_is_closed_and_thin)
{
    Geometry g;
    geo_init(&g, f->a);
    // z = 0.3 from (0.2, 0.2) to (3.8, 3.8): sixteen cells of edge 1.
    geo_quad(&g, nph_vec3(0.2F, 0.2F, 0.3F), nph_vec3(3.6F, 0.0F, 0.0F), nph_vec3(0.0F, 3.6F, 0.0F),
             0U);
    TestLook look;
    look_init(&look, g.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 1.0, &cells), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cells.len, 16U);
    for (u32 i = 0; i < cells.len; ++i) {
        const LodVoxelCell *c = &cells.data[i];
        NphRenderVoxelDesc d;
        nph_render_voxel_unpack(c->voxel, &d);
        NTEST_EXPECT_EQ_UINT(d.shape, NPH_RENDER_VOXEL_SHAPE_BOX);
        NTEST_EXPECT_EQ_INT(c->c[2], 0);
        // Thin across the sheet, holding it.
        NTEST_EXPECT_TRUE(d.box_lo.z <= 0.3F && d.box_hi.z >= 0.3F &&
                          d.box_hi.z - d.box_lo.z <= 3.0F / 32.0F);
        // An inner cell's box spans it.
        if (c->c[0] >= 1 && c->c[0] <= 2 && c->c[1] >= 1 && c->c[1] <= 2) {
            NTEST_EXPECT_NEAR_F32(d.box_lo.x, 0.0F, 0.0F);
            NTEST_EXPECT_NEAR_F32(d.box_hi.x, 1.0F, 0.0F);
            NTEST_EXPECT_NEAR_F32(d.box_lo.y, 0.0F, 0.0F);
            NTEST_EXPECT_NEAR_F32(d.box_hi.y, 1.0F, 0.0F);
        }
        NTEST_EXPECT_TRUE(nph_f32_abs(d.axis.z) > 0.999F);
        NTEST_EXPECT_TRUE(d.kappa >= NPH_RENDER_VOXEL_KAPPA_MAX);
        NTEST_EXPECT_NEAR_F32(d.albedo.x, 0.8F, 0.006F);
        NTEST_EXPECT_NEAR_F32(d.albedo.y, 0.1F, 0.003F);
        NTEST_EXPECT_NEAR_F32(d.roughness, 0.5F, 0.003F);
    }
    // Closed: every ray across the sheet meets a footprint.
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 3U, 9U);
    u32 holes = 0U;
    for (u32 k = 0; k < 4000U; ++k) {
        const NphVec3 o = nph_vec3(0.2F + 3.6F * nph_pcg32_next_f32(&rng),
                                   0.2F + 3.6F * nph_pcg32_next_f32(&rng), 5.0F);
        holes += cells_hit(&cells, 1.0, o, nph_vec3(0.0F, 0.0F, -1.0F)) ? 0U : 1U;
    }
    NTEST_EXPECT_EQ_UINT(holes, 0U);
    NPH_ARRAY_FREE(&cells);
    geo_free(&g);
}

NTEST_F(VoxelBuild, a_slanted_sheet_has_no_holes)
{
    Geometry g;
    geo_init(&g, f->a);
    // The plane x + y = 2 for z in [0.1, 3.9], x in [-0.4, 2.4].
    geo_quad(&g, nph_vec3(-0.4F, 2.4F, 0.1F), nph_vec3(2.8F, -2.8F, 0.0F),
             nph_vec3(0.0F, 0.0F, 3.8F), 0U);
    TestLook look;
    look_init(&look, g.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 1.0, &cells), NPH_RESULT_OK);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 5U, 9U);
    const NphVec3 dirs[3] = {nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(0.0F, 1.0F, 0.0F),
                             nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F))};
    u32 holes = 0U;
    u32 rays = 0U;
    for (u32 di = 0; di < 3U; ++di) {
        for (u32 k = 0; k < 1500U; ++k) {
            // A point of the sheet away from its edges, the ray from behind it.
            const f32 x = -0.2F + 2.4F * nph_pcg32_next_f32(&rng);
            const NphVec3 p = nph_vec3(x, 2.0F - x, 0.3F + 3.4F * nph_pcg32_next_f32(&rng));
            const NphVec3 o = nph_vec3_sub(p, nph_vec3_scale(dirs[di], 6.0F));
            holes += cells_hit(&cells, 1.0, o, dirs[di]) ? 0U : 1U;
            rays += 1U;
        }
    }
    NTEST_LOG("%u cells, %u of %u rays through the sheet missed", cells.len, holes, rays);
    NTEST_EXPECT_EQ_UINT(holes, 0U);
    NPH_ARRAY_FREE(&cells);
    geo_free(&g);
}

// The share of a grid of parallel rays along `d` across the cube [lo, lo +
// side]^3 that meet the triangles, and that meet the voxels' footprints.
static void coverage(const Geometry *g, const LodVoxelCellArray *cells, f64 cell, NphVec3 lo,
                     f32 side, NphVec3 d, u32 n, f64 *out_tris, f64 *out_voxels)
{
    NphVec3 t;
    NphVec3 b;
    tangents(d, &t, &b);
    const NphVec3 centre = nph_vec3_add(lo, nph_vec3_splat(0.5F * side));
    const f32 reach = side; // the rays' square covers the cube's shadow from any side
    u32 tri_hits = 0U;
    u32 vox_hits = 0U;
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f32 u = ((f32)i + 0.5F) / (f32)n * 2.0F - 1.0F;
            const f32 v = ((f32)j + 0.5F) / (f32)n * 2.0F - 1.0F;
            const NphVec3 o = nph_vec3_add(
                nph_vec3_sub(centre, nph_vec3_scale(d, 2.0F * side)),
                nph_vec3_add(nph_vec3_scale(t, u * reach), nph_vec3_scale(b, v * reach)));
            tri_hits += triangles_hit(g, o, d) ? 1U : 0U;
            vox_hits += cells_hit(cells, cell, o, d) ? 1U : 0U;
        }
    }
    *out_tris = (f64)tri_hits / (f64)(n * n);
    *out_voxels = (f64)vox_hits / (f64)(n * n);
}

NTEST_F(VoxelBuild, a_needle_clouds_coverage_is_kept_from_every_side)
{
    Geometry g;
    geo_init(&g, f->a);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 17U, 1U);
    // A sparse cloud: 2,000 needles 0.6 long, 0.02 wide in a cube of 4 cells.
    for (u32 i = 0; i < 2000U; ++i) {
        const NphVec3 c =
            nph_vec3(0.3F + 3.4F * nph_pcg32_next_f32(&rng), 0.3F + 3.4F * nph_pcg32_next_f32(&rng),
                     0.3F + 3.4F * nph_pcg32_next_f32(&rng));
        geo_needle(&g, &rng, c, 0.6F, 0.02F, 0U);
    }
    TestLook look;
    look_init(&look, g.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 1.0, &cells), NPH_RESULT_OK);
    u32 round = 0U;
    for (u32 i = 0; i < cells.len; ++i) {
        round += (cells.data[i].voxel.box >> NPH_RENDER_VOXEL_SHAPE_SHIFT) ==
                         NPH_RENDER_VOXEL_SHAPE_ELLIPSOID
                     ? 1U
                     : 0U;
    }
    NTEST_LOG("%u cells, %u of them ellipsoids", cells.len, round);
    NTEST_EXPECT_GE_UINT(round * 10U, cells.len * 9U);
    const NphVec3 dirs[] = {
        nph_vec3(1.0F, 0.0F, 0.0F),
        nph_vec3(0.0F, 0.0F, -1.0F),
        nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F)),
        nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 1.0F)),
        nph_vec3_normalize(nph_vec3(-0.3F, 0.8F, 0.52F)),
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(dirs); ++i) {
        f64 tris = 0.0;
        f64 voxels = 0.0;
        coverage(&g, &cells, 1.0, nph_vec3_zero(), 4.0F, dirs[i], 48U, &tris, &voxels);
        NTEST_LOG("direction (%.2f %.2f %.2f): the needles stop %.3f of the rays, the voxels %.3f",
                  (f64)dirs[i].x, (f64)dirs[i].y, (f64)dirs[i].z, tris, voxels);
        NTEST_EXPECT_NEAR_F64(voxels / tris, 1.0, 0.12);
    }
    NPH_ARRAY_FREE(&cells);
    geo_free(&g);
}

// A thin closed needle from `a` to `b`: a hexagonal prism of radius `r`, as
// near round as the landscape's tubes are.
static void geo_prism(Geometry *g, NphVec3 a, NphVec3 b, f32 r)
{
    const NphVec3 axis = nph_vec3_normalize(nph_vec3_sub(b, a));
    NphVec3 t;
    NphVec3 u;
    tangents(axis, &t, &u);
    NphVec3 ring[2][6];
    for (u32 e = 0; e < 2U; ++e) {
        for (u32 k = 0; k < 6U; ++k) {
            const f32 phi = 2.0F * NPH_PI * (f32)k / 6.0F;
            ring[e][k] = nph_vec3_add(e == 0U ? a : b,
                                      nph_vec3_add(nph_vec3_scale(t, r * nph_f32_cos(phi)),
                                                   nph_vec3_scale(u, r * nph_f32_sin(phi))));
        }
    }
    for (u32 k = 1U; k + 1U < 6U; ++k) {
        geo_tri(g, ring[0][0], ring[0][k + 1U], ring[0][k], 0U);
        geo_tri(g, ring[1][0], ring[1][k], ring[1][k + 1U], 0U);
    }
    for (u32 k = 0; k < 6U; ++k) {
        const u32 n = (k + 1U) % 6U;
        geo_tri(g, ring[0][k], ring[0][n], ring[1][n], 0U);
        geo_tri(g, ring[0][k], ring[1][n], ring[1][k], 0U);
    }
}

NTEST_F(VoxelBuild, a_needles_voxel_is_a_capsule_that_turns_with_it)
{
    Geometry g;
    geo_init(&g, f->a);
    const NphVec3 a = nph_vec3(0.15F, 0.2F, 0.1F);
    const NphVec3 b = nph_vec3(0.85F, 0.75F, 0.9F);
    geo_prism(&g, a, b, 0.02F);
    TestLook look;
    look_init(&look, g.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 1.0, &cells), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cells.len, 1U);
    NphRenderVoxelDesc d;
    nph_render_voxel_unpack(cells.data[0].voxel, &d);
    NTEST_EXPECT_EQ_UINT(d.shape, NPH_RENDER_VOXEL_SHAPE_CAPSULE);
    NTEST_LOG("diagonal %u, radius %.4f (the prism's %.4f)", d.diagonal, (f64)d.radius, 0.02);
    // Along the needle, across it, and between: the share of a grid of rays
    // over the cell that each stops.
    const NphVec3 axis = nph_vec3_normalize(nph_vec3_sub(b, a));
    NphVec3 t;
    NphVec3 u;
    tangents(axis, &t, &u);
    const NphVec3 dirs[] = {axis,
                            t,
                            nph_vec3_normalize(nph_vec3_add(axis, t)),
                            nph_vec3_normalize(nph_vec3_add(nph_vec3_scale(axis, 0.3F), u)),
                            nph_vec3(1.0F, 0.0F, 0.0F),
                            nph_vec3(0.0F, 0.0F, 1.0F)};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(dirs); ++i) {
        f64 tris = 0.0;
        f64 voxels = 0.0;
        coverage(&g, &cells, 1.0, nph_vec3_zero(), 1.0F, dirs[i], 96U, &tris, &voxels);
        NTEST_LOG("direction (%.2f %.2f %.2f): the needle stops %.4f of the rays, its voxel %.4f",
                  (f64)dirs[i].x, (f64)dirs[i].y, (f64)dirs[i].z, tris, voxels);
        if (i == 0U) {
            // End-on: a dot of the needle's section.
            NTEST_EXPECT_TRUE(voxels < 0.01 && tris < 0.01);
        } else {
            NTEST_EXPECT_NEAR_F64(voxels / tris, 1.0, 0.2);
        }
    }
    NPH_ARRAY_FREE(&cells);
    geo_free(&g);
}

NTEST_F(VoxelBuild, a_voxels_look_is_what_its_rays_see)
{
    Geometry g;
    geo_init(&g, f->a);
    // Two sheets across one cell: red below, green above. Rays along z see
    // red from below and green from above; the others meet them edge-on.
    geo_quad(&g, nph_vec3(0.0F, 0.0F, 0.25F), nph_vec3(1.0F, 0.0F, 0.0F),
             nph_vec3(0.0F, 1.0F, 0.0F), 0U);
    geo_quad(&g, nph_vec3(0.0F, 0.0F, 0.75F), nph_vec3(1.0F, 0.0F, 0.0F),
             nph_vec3(0.0F, 1.0F, 0.0F), 1U);
    TestLook look;
    look_init(&look, g.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 1.0, &cells), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cells.len, 1U);
    NphRenderVoxelDesc d;
    nph_render_voxel_unpack(cells.data[0].voxel, &d);
    NTEST_EXPECT_NEAR_F32(d.albedo.x, 0.45F, 0.005F);
    NTEST_EXPECT_NEAR_F32(d.albedo.y, 0.45F, 0.005F);
    NTEST_EXPECT_NEAR_F32(d.albedo.z, 0.1F, 0.005F);
    NTEST_EXPECT_EQ_UINT(cells.data[0].submesh, 0U); // a tie: the lower
    // Its box holds both sheets.
    NTEST_EXPECT_TRUE(d.box_lo.z <= 0.25F && d.box_hi.z >= 0.75F);
    NPH_ARRAY_FREE(&cells);
    geo_free(&g);

    // Quads facing every way: an isotropic lobe.
    geo_init(&g, f->a);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 23U, 1U);
    for (u32 i = 0; i < 400U; ++i) {
        const NphVec3 c =
            nph_vec3(0.2F + 0.6F * nph_pcg32_next_f32(&rng), 0.2F + 0.6F * nph_pcg32_next_f32(&rng),
                     0.2F + 0.6F * nph_pcg32_next_f32(&rng));
        NphVec3 t;
        NphVec3 b;
        tangents(random_unit(&rng), &t, &b);
        geo_quad(&g, nph_vec3_sub(c, nph_vec3_scale(nph_vec3_add(t, b), 0.04F)),
                 nph_vec3_scale(t, 0.08F), nph_vec3_scale(b, 0.08F), 0U);
    }
    look.positions = g.positions.data;
    look.by_x = true;
    NPH_ARRAY_INIT(&cells, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 1.0, &cells), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cells.len, 1U);
    nph_render_voxel_unpack(cells.data[0].voxel, &d);
    NTEST_LOG("isotropic quads: kappa %.3f (moment %.3f), red %.3f", (f64)d.kappa,
              (f64)nph_render_voxel_moment(d.kappa), (f64)d.albedo.x);
    NTEST_EXPECT_TRUE(d.kappa < 1.5F);
    // The colour rising with x, averaged over a cloud centred at x = 0.5.
    NTEST_EXPECT_NEAR_F32(d.albedo.x, 0.125F, 0.02F);
    NPH_ARRAY_FREE(&cells);
    geo_free(&g);
}

NTEST_F(VoxelBuild, a_masked_surface_lets_rays_through_where_it_is_cut_out)
{
    Geometry g;
    geo_init(&g, f->a);
    geo_quad(&g, nph_vec3(0.0F, 0.0F, 0.5F), nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(0.0F, 1.0F, 0.0F),
             0U);
    TestLook look;
    look_init(&look, g.positions.data);
    look.surface[0] = NPH_RENDER_VOXEL_SURFACE_MASKED;
    look.mask_x = 0.5F;
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    LodVoxelCellArray cells;
    NPH_ARRAY_INIT(&cells, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 1.0, &cells), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cells.len, 1U);
    NphRenderVoxelDesc d;
    nph_render_voxel_unpack(cells.data[0].voxel, &d);
    // The half that covers, and the box over it alone.
    NTEST_EXPECT_NEAR_F32(d.box_lo.x, 0.5F, 1.0F / 32.0F);
    NTEST_EXPECT_NEAR_F32(d.box_hi.x, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(d.box_lo.y, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(d.box_hi.y, 1.0F, 0.0F);
    NPH_ARRAY_FREE(&cells);
    geo_free(&g);
}

NTEST_F(VoxelBuild, the_same_triangles_give_the_same_voxels)
{
    Geometry g;
    geo_init(&g, f->a);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 29U, 1U);
    for (u32 i = 0; i < 600U; ++i) {
        const NphVec3 c = nph_vec3(2.0F * nph_pcg32_next_f32(&rng), 2.0F * nph_pcg32_next_f32(&rng),
                                   2.0F * nph_pcg32_next_f32(&rng));
        geo_needle(&g, &rng, c, 0.3F, 0.02F, 0U);
    }
    TestLook look;
    look_init(&look, g.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    LodVoxelCellArray a;
    LodVoxelCellArray b;
    NPH_ARRAY_INIT(&a, f->a);
    NPH_ARRAY_INIT(&b, f->a);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 0.25, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(voxelize(&g, &sampler, f->a, 0.25, &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(a.len, b.len);
    NTEST_EXPECT_GT_UINT(a.len, 100U);
    NTEST_EXPECT_EQ_INT(memcmp(a.data, b.data, (size_t)a.len * sizeof(LodVoxelCell)), 0);
    // The count of cells the triangles overlap bounds the cells hit.
    LodVoxelInput in;
    memset(&in, 0, sizeof in);
    in.positions = g.positions.data;
    in.sources = g.sources.data;
    in.source_count = g.sources.len;
    in.sampler = &sampler;
    in.scratch = f->a;
    u32 n = 0U;
    NTEST_ASSERT_EQ_INT(lod_voxel_count(&in, 0.25, NPH_U32_MAX, &n), NPH_RESULT_OK);
    NTEST_EXPECT_GE_UINT(n, a.len);
    NTEST_ASSERT_EQ_INT(lod_voxel_count(&in, 0.25, 10U, &n), NPH_RESULT_OK);
    NTEST_EXPECT_GT_UINT(n, 10U);
    NPH_ARRAY_FREE(&a);
    NPH_ARRAY_FREE(&b);
    geo_free(&g);
}

// ---------------------------------------------------------------------------
// The voxel levels
// ---------------------------------------------------------------------------

typedef struct LodMeshData {
    NPH_ARRAY(NphVec3) positions;
    NphArrayU32 indices;
    NphRenderIndexRange ranges[2]; // its submeshes
    u32 range_count;
} LodMeshData;

// Foliage: `count` separate needles in the cube of `side` from `corner`,
// each a thin closed triangular prism (eight triangles), as needles are
// modelled: a closed piece collapses no further than a tetrahedron, so the
// levels stall on them as they do on the landscape's pines.
static void foliage_add(LodMeshData *m, NphPcg32 *rng, u32 count, NphVec3 corner, f32 side)
{
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 c = nph_vec3_add(corner, nph_vec3(side * nph_pcg32_next_f32(rng),
                                                        side * nph_pcg32_next_f32(rng),
                                                        side * nph_pcg32_next_f32(rng)));
        const NphVec3 d = random_unit(rng);
        NphVec3 t;
        NphVec3 b;
        tangents(d, &t, &b);
        const NphVec3 half = nph_vec3_scale(d, 0.05F * side);
        const f32 r = 0.002F * side;
        const u32 base = m->positions.len;
        for (u32 end = 0; end < 2U; ++end) {
            const NphVec3 at = end == 0U ? nph_vec3_sub(c, half) : nph_vec3_add(c, half);
            for (u32 k = 0; k < 3U; ++k) {
                const f32 phi = 2.0F * NPH_PI * (f32)k / 3.0F;
                const NphVec3 off = nph_vec3_add(nph_vec3_scale(t, r * nph_f32_cos(phi)),
                                                 nph_vec3_scale(b, r * nph_f32_sin(phi)));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->positions, nph_vec3_add(at, off)));
            }
        }
        // The two caps, then the three sides, wound outward.
        const u32 q[24] = {base,      base + 2U, base + 1U, base + 3U, base + 4U, base + 5U,
                           base,      base + 1U, base + 4U, base,      base + 4U, base + 3U,
                           base + 1U, base + 2U, base + 5U, base + 1U, base + 5U, base + 4U,
                           base + 2U, base,      base + 3U, base + 2U, base + 3U, base + 5U};
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&m->indices, q, 24U));
    }
}

// A mesh of `count` needles in a cube of `side` (foliage_add).
static void foliage_mesh(LodMeshData *m, NphAllocator *a, u32 count, f32 side, u64 seed)
{
    NPH_ARRAY_INIT(&m->positions, a);
    NPH_ARRAY_INIT(&m->indices, a);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, seed, 1U);
    foliage_add(m, &rng, count, nph_vec3_zero(), side);
    m->ranges[0].index_offset = 0U;
    m->ranges[0].index_count = m->indices.len;
    m->range_count = 1U;
}

static void lod_mesh_data_free(LodMeshData *m)
{
    NPH_ARRAY_FREE(&m->positions);
    NPH_ARRAY_FREE(&m->indices);
}

static NphResult build_with(const LodMeshData *m, const NphRenderVoxelSampler *sampler,
                            NphAllocator *a, NphRenderMeshlets *meshlets,
                            NphRenderClusterLods *lods)
{
    nph_render_meshlets_init(meshlets, a);
    nph_render_cluster_lods_init(lods, a);
    NphRenderMeshletInput mi;
    memset(&mi, 0, sizeof mi);
    mi.positions = m->positions.data;
    mi.vertex_count = m->positions.len;
    mi.indices = m->indices.data;
    mi.index_count = m->indices.len;
    mi.ranges = m->ranges;
    mi.range_count = m->range_count;
    mi.cone_weight = 0.25F;
    mi.scratch = a;
    NPH_TRY(nph_render_meshlets_build(&mi, meshlets));
    NphRenderClusterLodInput in;
    memset(&in, 0, sizeof in);
    in.positions = m->positions.data;
    in.vertex_count = m->positions.len;
    in.indices = m->indices.data;
    in.index_count = m->indices.len;
    in.ranges = m->ranges;
    in.range_count = m->range_count;
    in.meshlets = meshlets;
    in.cone_weight = 0.25F;
    in.voxels = sampler;
    in.scratch = a;
    return nph_render_cluster_lods_build(&in, lods);
}

// Every voxel cluster's bricks and voxels lie in the arrays, one after
// another, with masks that count them; the errors never fall going up.
static void check_voxel_levels(const NphRenderClusterLods *l, u32 *out_voxel_clusters)
{
    u32 voxel_clusters = 0U;
    u32 next_brick = 0U;
    u32 next_voxel = 0U;
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        if (m->vertex_count != 0U) {
            continue;
        }
        voxel_clusters += 1U;
        NTEST_EXPECT_GT_UINT(l->lods.data[c].level, 0U);
        NTEST_ASSERT_EQ_UINT(m->vertex_offset, next_brick);
        NTEST_ASSERT_TRUE(m->triangle_count >= 1U &&
                          m->triangle_count <= NPH_RENDER_VOXEL_CLUSTER_BRICKS);
        u32 voxels = 0U;
        for (u32 k = 0; k < m->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
            const u32 n = nph_popcount_u32(br->mask[0]) + nph_popcount_u32(br->mask[1]);
            NTEST_EXPECT_GT_UINT(n, 0U);
            NTEST_ASSERT_EQ_UINT(br->first_voxel, next_voxel);
            next_voxel += n;
            voxels += n;
            // A power-of-two cell, on its grid.
            NTEST_EXPECT_EQ_UINT(nph_f32_to_bits(br->cell) & 0x007FFFFFU, 0U);
            NTEST_EXPECT_NEAR_F32(nph_f32_fract(br->origin.x / (4.0F * br->cell)), 0.0F, 0.0F);
        }
        NTEST_EXPECT_LE_UINT(voxels, NPH_RENDER_VOXEL_CLUSTER_VOXELS);
        next_brick += m->triangle_count;
        // Its bounds hold its footprints' boxes.
        const NphRenderMeshletBounds *bd = &l->clusters.bounds.data[c];
        NTEST_EXPECT_EQ_INT(bd->cone_cutoff, 127);
        for (u32 k = 0; k < m->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
            u32 index = br->first_voxel;
            for (u32 bit = 0; bit < 64U; ++bit) {
                if (((br->mask[bit >> 5U] >> (bit & 31U)) & 1U) == 0U) {
                    continue;
                }
                NphRenderVoxelDesc d;
                nph_render_voxel_unpack(l->voxels.data[index++], &d);
                const NphVec3 cell_lo = nph_vec3_add(
                    br->origin, nph_vec3_scale(nph_vec3((f32)(bit & 3U), (f32)((bit >> 2U) & 3U),
                                                        (f32)(bit >> 4U)),
                                               br->cell));
                const NphVec3 lo = nph_vec3_add(cell_lo, nph_vec3_scale(d.box_lo, br->cell));
                const NphVec3 hi = nph_vec3_add(cell_lo, nph_vec3_scale(d.box_hi, br->cell));
                const NphVec3 corners[2] = {lo, hi};
                for (u32 q = 0; q < 8U; ++q) {
                    const NphVec3 p =
                        nph_vec3(corners[q & 1U].x, corners[(q >> 1U) & 1U].y, corners[q >> 2U].z);
                    NTEST_EXPECT_TRUE(nph_vec3_distance(p, bd->center) <= bd->radius * 1.00001F);
                }
            }
        }
    }
    NTEST_EXPECT_EQ_UINT(next_brick, l->bricks.len);
    NTEST_EXPECT_EQ_UINT(next_voxel, l->voxels.len);
    // A group's error is at least its makers'.
    for (u32 g = 0; g < l->groups.len; ++g) {
        const NphRenderLodGroup *rg = &l->groups.data[g];
        for (u32 i = 0; i < rg->cluster_count; ++i) {
            const u32 c = l->group_clusters.data[rg->cluster_offset + i];
            const u32 maker = l->lods.data[c].group;
            if (maker != NPH_RENDER_LOD_NONE) {
                NTEST_EXPECT_TRUE(rg->error >= l->groups.data[maker].error);
            }
        }
    }
    *out_voxel_clusters = voxel_clusters;
}

NTEST_F(VoxelBuild, stalled_foliage_becomes_voxels_up_to_a_root)
{
    LodMeshData m;
    foliage_mesh(&m, f->a, 2000U, 4.0F, 31U);
    TestLook look;
    look_init(&look, m.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    NphRenderMeshlets meshlets;
    NphRenderClusterLods lods;
    NTEST_ASSERT_EQ_INT(build_with(&m, &sampler, f->a, &meshlets, &lods), NPH_RESULT_OK);
    u32 voxel_clusters = 0U;
    check_voxel_levels(&lods, &voxel_clusters);
    NTEST_EXPECT_GT_UINT(voxel_clusters, 0U);
    // The coarsest level is a handful of clusters, all voxels.
    u32 coarsest = 0U;
    u32 coarsest_voxels = 0U;
    for (u32 c = 0; c < lods.lods.len; ++c) {
        if (lods.lods.data[c].level == lods.levels - 1U) {
            coarsest += 1U;
            coarsest_voxels += lods.clusters.meshlets.data[c].vertex_count == 0U ? 1U : 0U;
        }
    }
    NTEST_LOG("%u levels, %u clusters (%u voxel clusters, %u bricks, %u voxels); the coarsest "
              "level %u clusters",
              lods.levels, lods.clusters.meshlets.len, voxel_clusters, lods.bricks.len,
              lods.voxels.len, coarsest);
    NTEST_EXPECT_LE_UINT(coarsest, 8U);
    NTEST_EXPECT_EQ_UINT(coarsest_voxels, coarsest);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);

    // Without a sampler the levels are triangles alone.
    NTEST_ASSERT_EQ_INT(build_with(&m, NULL, f->a, &meshlets, &lods), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(lods.bricks.len, 0U);
    NTEST_EXPECT_EQ_UINT(lods.voxels.len, 0U);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    lod_mesh_data_free(&m);
}

// A clump of needles across the mesh's origin (a submesh of its own, so no
// meshlet joins it to the rest), beside a forest that keeps the levels
// going: as the clump's voxels coarsen they stop falling below the eight
// cells they lie across (the grid of every cell size divides each axis at
// zero), however few voxels its group has, and its group takes that count
// rather than failing.
NTEST_F(VoxelBuild, a_few_voxels_across_the_origin_still_coarsen)
{
    LodMeshData m;
    NPH_ARRAY_INIT(&m.positions, f->a);
    NPH_ARRAY_INIT(&m.indices, f->a);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 5U, 1U);
    foliage_add(&m, &rng, 2000U, nph_vec3(10.0F, 10.0F, 10.0F), 4.0F);
    m.ranges[0].index_offset = 0U;
    m.ranges[0].index_count = m.indices.len;
    foliage_add(&m, &rng, 40U, nph_vec3(-0.05F, -0.05F, -0.05F), 0.1F);
    m.ranges[1].index_offset = m.ranges[0].index_count;
    m.ranges[1].index_count = m.indices.len - m.ranges[0].index_count;
    m.range_count = 2U;
    TestLook look;
    look_init(&look, m.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    NphRenderMeshlets meshlets;
    NphRenderClusterLods lods;
    NTEST_ASSERT_EQ_INT(build_with(&m, &sampler, f->a, &meshlets, &lods), NPH_RESULT_OK);
    u32 voxel_clusters = 0U;
    check_voxel_levels(&lods, &voxel_clusters);
    // The clump's coarsest voxel cluster: at most eight voxels, a brick
    // of them on each side of the origin's planes.
    u32 fewest = NPH_U32_MAX;
    u32 straddling = 0U;
    for (u32 c = 0; c < lods.clusters.meshlets.len; ++c) {
        const NphRenderMeshlet *mc = &lods.clusters.meshlets.data[c];
        const NphRenderMeshletBounds *bd = &lods.clusters.bounds.data[c];
        if (mc->vertex_count != 0U || nph_vec3_length(bd->center) > 1.0F) {
            continue;
        }
        u32 voxels = 0U;
        for (u32 k = 0; k < mc->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &lods.bricks.data[mc->vertex_offset + k];
            voxels += nph_popcount_u32(br->mask[0]) + nph_popcount_u32(br->mask[1]);
        }
        fewest = nph_min_u32(fewest, voxels);
        straddling += 1U;
    }
    NTEST_LOG("%u levels, %u voxel clusters; the clump's %u, the fewest voxels %u", lods.levels,
              voxel_clusters, straddling, fewest);
    NTEST_EXPECT_GT_UINT(straddling, 1U);
    NTEST_EXPECT_LE_UINT(fewest, 8U);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    lod_mesh_data_free(&m);
}

NTEST_F(VoxelBuild, a_sampler_is_checked)
{
    LodMeshData m;
    foliage_mesh(&m, f->a, 100U, 4.0F, 37U);
    TestLook look;
    look_init(&look, m.positions.data);
    NphRenderVoxelSampler sampler = test_sampler(&look);
    NphRenderMeshlets meshlets;
    NphRenderClusterLods lods;
    sampler.sample = NULL;
    NTEST_EXPECT_EQ_INT(build_with(&m, &sampler, f->a, &meshlets, &lods),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    sampler = test_sampler(&look);
    look.surface[0] = NPH_RENDER_VOXEL_SURFACE_MASKED;
    sampler.covers = NULL;
    NTEST_EXPECT_EQ_INT(build_with(&m, &sampler, f->a, &meshlets, &lods),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    sampler = test_sampler(&look);
    look.surface[0] = 7U;
    NTEST_EXPECT_EQ_INT(build_with(&m, &sampler, f->a, &meshlets, &lods),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    lod_mesh_data_free(&m);
}

NTEST_F(VoxelBuild, a_surface_that_simplifies_stays_triangles)
{
    // A wavy sheet of 64 x 64 quads: its levels simplify to the end.
    LodMeshData m;
    NPH_ARRAY_INIT(&m.positions, f->a);
    NPH_ARRAY_INIT(&m.indices, f->a);
    const u32 n = 64U;
    for (u32 y = 0; y <= n; ++y) {
        for (u32 x = 0; x <= n; ++x) {
            const f32 fx = (f32)x / (f32)n;
            const f32 fy = (f32)y / (f32)n;
            const NphVec3 p = nph_vec3(4.0F * fx, 4.0F * fy,
                                       0.2F * nph_f32_sin(6.0F * fx) * nph_f32_cos(5.0F * fy));
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m.positions, p));
        }
    }
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const u32 v00 = y * (n + 1U) + x;
            const u32 q[6] = {v00, v00 + 1U, v00 + n + 2U, v00, v00 + n + 2U, v00 + n + 1U};
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&m.indices, q, 6U));
        }
    }
    m.ranges[0].index_offset = 0U;
    m.ranges[0].index_count = m.indices.len;
    m.range_count = 1U;
    TestLook look;
    look_init(&look, m.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    NphRenderMeshlets meshlets;
    NphRenderClusterLods lods;
    NTEST_ASSERT_EQ_INT(build_with(&m, &sampler, f->a, &meshlets, &lods), NPH_RESULT_OK);
    NTEST_LOG("%u levels, %u clusters, %u bricks", lods.levels, lods.clusters.meshlets.len,
              lods.bricks.len);
    NTEST_EXPECT_GE_UINT(lods.levels, 5U);
    NTEST_EXPECT_EQ_UINT(lods.bricks.len, 0U);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    lod_mesh_data_free(&m);
}

// The renderer's choice, on the CPU: whether the camera at `camera` draws
// cluster `c` (render/cluster_lod.h; test_cluster_lod.c's twin).
static f32 group_pixels(const NphRenderClusterLods *l, u32 g, NphVec3 camera, f32 ppu)
{
    if (g == NPH_RENDER_LOD_NONE) {
        return 0.0F;
    }
    const NphRenderLodGroup *rg = &l->groups.data[g];
    if (rg->error == NPH_F32_MAX) {
        return NPH_F32_MAX;
    }
    return nph_render_lod_projected_error(rg->center, rg->radius, rg->error, 1.0F, camera, ppu,
                                          0.01F);
}

static bool cluster_drawn(const NphRenderClusterLods *l, u32 c, NphVec3 camera, f32 ppu)
{
    const NphRenderClusterLod *lod = &l->lods.data[c];
    return group_pixels(l, lod->group, camera, ppu) <= 1.0F &&
           group_pixels(l, lod->parent, camera, ppu) > 1.0F;
}

// Whether the ray meets the cut's geometry: its triangle clusters' triangles
// or its voxel clusters' footprints.
static bool cut_hit(const NphRenderClusterLods *l, const NphVec3 *positions, const NphArrayU32 *cut,
                    NphVec3 o, NphVec3 d)
{
    for (u32 i = 0; i < cut->len; ++i) {
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[cut->data[i]];
        // The cluster's sphere first (d is unit).
        const NphRenderMeshletBounds *bd = &l->clusters.bounds.data[cut->data[i]];
        const NphVec3 oc = nph_vec3_sub(bd->center, o);
        const f32 along = nph_vec3_dot(oc, d);
        if (nph_vec3_dot(oc, oc) - along * along > bd->radius * bd->radius * 1.0001F) {
            continue;
        }
        if (m->vertex_count == 0U) {
            for (u32 k = 0; k < m->triangle_count; ++k) {
                NphRenderVoxelHit hit;
                if (nph_render_voxel_trace(&l->bricks.data[m->vertex_offset + k], l->voxels.data, o,
                                           d, 0.0F, 1e30F, &hit)) {
                    return true;
                }
            }
            continue;
        }
        for (u32 t = 0; t < m->triangle_count; ++t) {
            NphVec3 p[3];
            for (u32 k = 0; k < 3U; ++k) {
                const u8 corner =
                    l->clusters.triangles.data[((u64)m->triangle_offset + t) * 3U + k];
                p[k] = positions[l->clusters.vertices.data[m->vertex_offset + corner]];
            }
            f64 tt = 0.0;
            if (ray_triangle(o, d, p[0], p[1], p[2], &tt)) {
                return true;
            }
        }
    }
    return false;
}

NTEST_F(VoxelBuild, every_cut_through_the_voxel_levels_keeps_the_coverage)
{
    LodMeshData m;
    foliage_mesh(&m, f->a, 2000U, 4.0F, 41U);
    TestLook look;
    look_init(&look, m.positions.data);
    const NphRenderVoxelSampler sampler = test_sampler(&look);
    NphRenderMeshlets meshlets;
    NphRenderClusterLods lods;
    NTEST_ASSERT_EQ_INT(build_with(&m, &sampler, f->a, &meshlets, &lods), NPH_RESULT_OK);
    // Level 0 alone, and cuts from cameras ever farther along one direction
    // (4K at 90 degrees: 1,920 pixels a unit at a distance of one).
    NphArrayU32 cut;
    NPH_ARRAY_INIT(&cut, f->a);
    for (u32 c = 0; c < lods.level0_clusters; ++c) {
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&cut, c));
    }
    const NphVec3 dir = nph_vec3_normalize(nph_vec3(-0.4F, -0.5F, -0.77F));
    const NphVec3 centre = nph_vec3_splat(2.0F);
    NphVec3 t;
    NphVec3 b;
    tangents(dir, &t, &b);
    const u32 n = 96U;
    u32 level0 = 0U;
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const NphVec3 o = nph_vec3_add(
                nph_vec3_sub(centre, nph_vec3_scale(dir, 10.0F)),
                nph_vec3_add(nph_vec3_scale(t, (((f32)i + 0.5F) / (f32)n - 0.5F) * 6.0F),
                             nph_vec3_scale(b, (((f32)j + 0.5F) / (f32)n - 0.5F) * 6.0F)));
            level0 += cut_hit(&lods, m.positions.data, &cut, o, dir) ? 1U : 0U;
        }
    }
    const f32 distances[] = {20.0F, 80.0F, 300.0F, 1200.0F};
    u32 voxel_cuts = 0U;
    for (u32 di = 0; di < NPH_ARRAY_COUNT(distances); ++di) {
        const NphVec3 camera = nph_vec3_sub(centre, nph_vec3_scale(dir, distances[di]));
        NPH_ARRAY_CLEAR(&cut);
        u32 voxel_clusters = 0U;
        u32 triangles = 0U;
        for (u32 c = 0; c < lods.clusters.meshlets.len; ++c) {
            if (cluster_drawn(&lods, c, camera, 1920.0F)) {
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&cut, c));
                const NphRenderMeshlet *ml = &lods.clusters.meshlets.data[c];
                voxel_clusters += ml->vertex_count == 0U ? 1U : 0U;
                triangles += ml->vertex_count == 0U ? 0U : ml->triangle_count;
            }
        }
        u32 covered = 0U;
        for (u32 j = 0; j < n; ++j) {
            for (u32 i = 0; i < n; ++i) {
                const NphVec3 o = nph_vec3_add(
                    nph_vec3_sub(centre, nph_vec3_scale(dir, 10.0F)),
                    nph_vec3_add(nph_vec3_scale(t, (((f32)i + 0.5F) / (f32)n - 0.5F) * 6.0F),
                                 nph_vec3_scale(b, (((f32)j + 0.5F) / (f32)n - 0.5F) * 6.0F)));
                covered += cut_hit(&lods, m.positions.data, &cut, o, dir) ? 1U : 0U;
            }
        }
        NTEST_LOG("distance %.0f: %u clusters (%u of voxels, %u triangles), coverage %u against "
                  "level 0's %u",
                  (f64)distances[di], cut.len, voxel_clusters, triangles, covered, level0);
        voxel_cuts += voxel_clusters != 0U ? 1U : 0U;
        NTEST_EXPECT_NEAR_F64((f64)covered / (f64)level0, 1.0, 0.05);
    }
    NTEST_EXPECT_GE_UINT(voxel_cuts, 2U);
    NPH_ARRAY_FREE(&cut);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    lod_mesh_data_free(&m);
}
