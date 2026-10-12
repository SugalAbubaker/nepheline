// SPDX-License-Identifier: Apache-2.0
#pragma once

// The batch kernel bodies, compiled once per ISA level. The including
// translation unit defines NPH_KERNEL(name) to add its suffix; wide.h then
// resolves NphF32x8 to AVX or paired SSE according to that unit's flags.
// Every kernel performs the same arithmetic in the same order at every
// level (and as the scalar reference in kernels.c), so the outputs are
// bit-identical.

#include <nepheline/math/pack.h>
#include <nepheline/math/wide.h>

#include <string.h>

#ifndef NPH_KERNEL
#error "kernels_impl.h: define NPH_KERNEL(name) before including"
#endif

// Centre and half-size from SoA min/max: c = (min + max) * 0.5,
// e = (max - min) * 0.5, the same expressions the scalar reference uses.
static void NPH_KERNEL(center_extent8)(NphVec3x8 bmin, NphVec3x8 bmax, NphVec3x8 *out_center,
                                       NphVec3x8 *out_extent)
{
    const NphF32x8 half = nph_f32x8_splat(0.5F);
    *out_center = nph_vec3x8_scale(nph_vec3x8_add(bmin, bmax), half);
    *out_extent = nph_vec3x8_scale(nph_vec3x8_sub(bmax, bmin), half);
}

// Eight consecutive boxes are sixteen consecutive NphVec3 (min0 max0 min1
// max1 ...). The AoS transpose yields (min0 max0 min1 max1) per component
// and one shuffle each separates the even (min) and odd (max) slots: no
// scalar gather.
static void NPH_KERNEL(load_aabbs8)(const NphAABB *boxes, NphVec3x8 *out_center,
                                    NphVec3x8 *out_extent)
{
    const NphVec3 *v = &boxes[0].min;
    const NphVec3x4 g0 = nph_vec3x4_load_aos(v);      // boxes 0, 1
    const NphVec3x4 g1 = nph_vec3x4_load_aos(v + 4);  // boxes 2, 3
    const NphVec3x4 g2 = nph_vec3x4_load_aos(v + 8);  // boxes 4, 5
    const NphVec3x4 g3 = nph_vec3x4_load_aos(v + 12); // boxes 6, 7
    const NphVec3x4 min_lo = nph_vec3x4_from_lanes(NPH_F32X4_SHUFFLE2(g0.x, g1.x, 0, 2, 0, 2),
                                                   NPH_F32X4_SHUFFLE2(g0.y, g1.y, 0, 2, 0, 2),
                                                   NPH_F32X4_SHUFFLE2(g0.z, g1.z, 0, 2, 0, 2));
    const NphVec3x4 max_lo = nph_vec3x4_from_lanes(NPH_F32X4_SHUFFLE2(g0.x, g1.x, 1, 3, 1, 3),
                                                   NPH_F32X4_SHUFFLE2(g0.y, g1.y, 1, 3, 1, 3),
                                                   NPH_F32X4_SHUFFLE2(g0.z, g1.z, 1, 3, 1, 3));
    const NphVec3x4 min_hi = nph_vec3x4_from_lanes(NPH_F32X4_SHUFFLE2(g2.x, g3.x, 0, 2, 0, 2),
                                                   NPH_F32X4_SHUFFLE2(g2.y, g3.y, 0, 2, 0, 2),
                                                   NPH_F32X4_SHUFFLE2(g2.z, g3.z, 0, 2, 0, 2));
    const NphVec3x4 max_hi = nph_vec3x4_from_lanes(NPH_F32X4_SHUFFLE2(g2.x, g3.x, 1, 3, 1, 3),
                                                   NPH_F32X4_SHUFFLE2(g2.y, g3.y, 1, 3, 1, 3),
                                                   NPH_F32X4_SHUFFLE2(g2.z, g3.z, 1, 3, 1, 3));
    NPH_KERNEL(center_extent8)(nph_vec3x8_from_x4(min_lo, min_hi),
                               nph_vec3x8_from_x4(max_lo, max_hi), out_center, out_extent);
}

// The last, partial group: slots past `n` repeat the last box so no
// uninitialised lane is read.
static void NPH_KERNEL(load_aabbs_tail)(const NphAABB *boxes, u32 n, NphVec3x8 *out_center,
                                        NphVec3x8 *out_extent)
{
    NphAABB tmp[8];
    for (u32 k = 0; k < 8U; ++k) {
        tmp[k] = boxes[k < n ? k : n - 1U];
    }
    NPH_KERNEL(load_aabbs8)(tmp, out_center, out_extent);
}

static void NPH_KERNEL(frustum_cull_aabbs)(const NphFrustum *f, const NphAABB *boxes, u32 count,
                                           u64 *out_visible)
{
    const u32 words = (count + 63U) / 64U;
    memset(out_visible, 0, (size_t)words * sizeof(u64));
    for (u32 i = 0; i < count; i += 8U) {
        const u32 n = nph_min_u32(8U, count - i);
        NphVec3x8 center;
        NphVec3x8 extent;
        if (n == 8U) {
            NPH_KERNEL(load_aabbs8)(boxes + i, &center, &extent);
        } else {
            NPH_KERNEL(load_aabbs_tail)(boxes + i, n, &center, &extent);
        }
        const u32 mask = nph_frustum_cull_aabb_x8(f, center, extent) & ((1U << n) - 1U);
        out_visible[i / 64U] |= (u64)mask << (i % 64U);
    }
}

// A sphere is one register (x y z r); a 4x4 transpose of four spheres gives
// their SoA form directly.
static void NPH_KERNEL(load_spheres8)(const NphSphere *spheres, NphVec3x8 *out_center,
                                      NphF32x8 *out_radius)
{
    NphSimdMat4 a;
    NphSimdMat4 b;
    for (u32 k = 0; k < 4U; ++k) {
        a.c[k] = _mm_loadu_ps(&spheres[k].center.x);
        b.c[k] = _mm_loadu_ps(&spheres[k + 4U].center.x);
    }
    a = nph_simd_mat4_transpose(a);
    b = nph_simd_mat4_transpose(b);
    *out_center = nph_vec3x8_from_x4(nph_vec3x4_from_lanes(a.c[0], a.c[1], a.c[2]),
                                     nph_vec3x4_from_lanes(b.c[0], b.c[1], b.c[2]));
    *out_radius = nph_f32x8_from_x4(a.c[3], b.c[3]);
}

static void NPH_KERNEL(frustum_cull_spheres)(const NphFrustum *f, const NphSphere *spheres,
                                             u32 count, u64 *out_visible)
{
    const u32 words = (count + 63U) / 64U;
    memset(out_visible, 0, (size_t)words * sizeof(u64));
    for (u32 i = 0; i < count; i += 8U) {
        const u32 n = nph_min_u32(8U, count - i);
        NphVec3x8 center;
        NphF32x8 radius;
        if (n == 8U) {
            NPH_KERNEL(load_spheres8)(spheres + i, &center, &radius);
        } else {
            NphSphere tmp[8];
            for (u32 k = 0; k < 8U; ++k) {
                tmp[k] = spheres[i + (k < n ? k : n - 1U)];
            }
            NPH_KERNEL(load_spheres8)(tmp, &center, &radius);
        }
        const u32 mask = nph_frustum_cull_sphere_x8(f, center, radius) & ((1U << n) - 1U);
        out_visible[i / 64U] |= (u64)mask << (i % 64U);
    }
}

static void NPH_KERNEL(transform_points)(const NphAffine *m, const NphVec3 *in, NphVec3 *out,
                                         u32 count)
{
    u32 i = 0;
    for (; i + 8U <= count; i += 8U) {
        nph_vec3x8_store_aos(out + i, nph_vec3x8_transform_point(m, nph_vec3x8_load_aos(in + i)));
    }
    if (i < count) {
        // Tail: gather into a padded group, scatter the valid slots.
        NphVec3 tmp[8];
        const u32 n = count - i;
        for (u32 k = 0; k < 8U; ++k) {
            tmp[k] = in[i + (k < n ? k : n - 1U)];
        }
        nph_vec3x8_store_aos(tmp, nph_vec3x8_transform_point(m, nph_vec3x8_load_aos(tmp)));
        for (u32 k = 0; k < n; ++k) {
            out[i + k] = tmp[k];
        }
    }
}

static void NPH_KERNEL(transform_vectors)(const NphAffine *m, const NphVec3 *in, NphVec3 *out,
                                          u32 count)
{
    u32 i = 0;
    for (; i + 8U <= count; i += 8U) {
        nph_vec3x8_store_aos(out + i, nph_vec3x8_transform_vector(m, nph_vec3x8_load_aos(in + i)));
    }
    if (i < count) {
        NphVec3 tmp[8];
        const u32 n = count - i;
        for (u32 k = 0; k < 8U; ++k) {
            tmp[k] = in[i + (k < n ? k : n - 1U)];
        }
        nph_vec3x8_store_aos(tmp, nph_vec3x8_transform_vector(m, nph_vec3x8_load_aos(tmp)));
        for (u32 k = 0; k < n; ++k) {
            out[i + k] = tmp[k];
        }
    }
}

static void NPH_KERNEL(f32_to_f16)(const f32 *in, u16 *out, u32 count)
{
#if NPH_WIDE_AVX
    // F16C rounds to nearest even with full subnormal and NaN handling, the
    // same result as nph_f32_to_f16 (quiet NaNs keep their top payload bits).
    u32 i = 0;
    for (; i + 8U <= count; i += 8U) {
        const __m128i h = _mm256_cvtps_ph(_mm256_loadu_ps(in + i), _MM_FROUND_TO_NEAREST_INT);
        _mm_storeu_si128((__m128i *)(out + i), h);
    }
    for (; i < count; ++i) {
        out[i] = nph_f32_to_f16(in[i]);
    }
#else
    nph_f32_to_f16_array(in, out, count);
#endif
}
