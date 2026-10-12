// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/kernels.h>
#include <nepheline/math/pack.h>

#include "kernels_tables.h"

#include <string.h>

#if NPH_COMPILER_MSVC
#include <intrin.h>
#else
#include <cpuid.h>
#endif

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

static void cpuid(u32 leaf, u32 subleaf, u32 out[4])
{
#if NPH_COMPILER_MSVC
    int regs[4];
    __cpuidex(regs, (int)leaf, (int)subleaf);
    out[0] = (u32)regs[0];
    out[1] = (u32)regs[1];
    out[2] = (u32)regs[2];
    out[3] = (u32)regs[3];
#else
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
#endif
}

// XCR0: which register state the OS saves and restores. Bits 1 (SSE) and 2
// (AVX) must both be set before AVX instructions may be used.
static u64 xgetbv0(void)
{
#if NPH_COMPILER_MSVC
    return (u64)_xgetbv(0);
#else
    u32 eax;
    u32 edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0U));
    return ((u64)edx << 32U) | eax;
#endif
}

NphSimdLevel nph_simd_detect_level(void)
{
    u32 r[4];
    cpuid(0U, 0U, r);
    const u32 max_leaf = r[0];
    if (max_leaf < 1U) {
        return NPH_SIMD_LEVEL_SCALAR;
    }
    cpuid(1U, 0U, r);
    const u32 ecx1 = r[2];
    const bool sse41 = (ecx1 & (1U << 19U)) != 0U;
    const bool sse42 = (ecx1 & (1U << 20U)) != 0U;
    const bool fma = (ecx1 & (1U << 12U)) != 0U;
    const bool osxsave = (ecx1 & (1U << 27U)) != 0U;
    const bool avx = (ecx1 & (1U << 28U)) != 0U;
    const bool f16c = (ecx1 & (1U << 29U)) != 0U;
    if (!sse41 || !sse42) {
        return NPH_SIMD_LEVEL_SCALAR;
    }
    bool avx2 = false;
    if (max_leaf >= 7U) {
        cpuid(7U, 0U, r);
        avx2 = (r[1] & (1U << 5U)) != 0U;
    }
    if (avx && avx2 && fma && f16c && osxsave && (xgetbv0() & 0x6U) == 0x6U) {
        return NPH_SIMD_LEVEL_AVX2;
    }
    return NPH_SIMD_LEVEL_SSE4;
}

const char *nph_simd_level_name(NphSimdLevel level)
{
    switch (level) {
    case NPH_SIMD_LEVEL_SCALAR:
        return "scalar";
    case NPH_SIMD_LEVEL_SSE4:
        return "sse4";
    case NPH_SIMD_LEVEL_AVX2:
        return "avx2";
    case NPH_SIMD_LEVEL_COUNT:
        break;
    }
    return "invalid";
}

// ---------------------------------------------------------------------------
// Scalar reference kernels. Same arithmetic, same order as wide.h.
// ---------------------------------------------------------------------------

static void scalar_frustum_cull_aabbs(const NphFrustum *f, const NphAABB *boxes, u32 count,
                                      u64 *out_visible)
{
    const u32 words = (count + 63U) / 64U;
    memset(out_visible, 0, (size_t)words * sizeof(u64));
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 c = nph_vec3_scale(nph_vec3_add(boxes[i].min, boxes[i].max), 0.5F);
        const NphVec3 e = nph_vec3_scale(nph_vec3_sub(boxes[i].max, boxes[i].min), 0.5F);
        bool visible = true;
        for (u32 k = 0; k < NPH_FRUSTUM_PLANE_COUNT; ++k) {
            const NphPlane p = f->planes[k];
            f32 r = p.normal.x * c.x;
            r = r + p.normal.y * c.y;
            r = r + p.normal.z * c.z;
            r = r + p.d;
            f32 a = nph_f32_abs(p.normal.x) * e.x;
            a = a + nph_f32_abs(p.normal.y) * e.y;
            a = a + nph_f32_abs(p.normal.z) * e.z;
            visible = visible && (r + a >= 0.0F);
        }
        if (visible) {
            out_visible[i / 64U] |= (u64)1U << (i % 64U);
        }
    }
}

static void scalar_frustum_cull_spheres(const NphFrustum *f, const NphSphere *spheres, u32 count,
                                        u64 *out_visible)
{
    const u32 words = (count + 63U) / 64U;
    memset(out_visible, 0, (size_t)words * sizeof(u64));
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 c = spheres[i].center;
        bool visible = true;
        for (u32 k = 0; k < NPH_FRUSTUM_PLANE_COUNT; ++k) {
            const NphPlane p = f->planes[k];
            f32 r = p.normal.x * c.x;
            r = r + p.normal.y * c.y;
            r = r + p.normal.z * c.z;
            r = r + p.d;
            visible = visible && (r >= -spheres[i].radius);
        }
        if (visible) {
            out_visible[i / 64U] |= (u64)1U << (i % 64U);
        }
    }
}

static void scalar_transform_points(const NphAffine *m, const NphVec3 *in, NphVec3 *out, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        out[i] = nph_affine_transform_point(*m, in[i]);
    }
}

static void scalar_transform_vectors(const NphAffine *m, const NphVec3 *in, NphVec3 *out, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        out[i] = nph_affine_transform_vector(*m, in[i]);
    }
}

const NphMathKernels g_nph_math_kernels_scalar = {
    NPH_SIMD_LEVEL_SCALAR,     "scalar",
    scalar_frustum_cull_aabbs, scalar_frustum_cull_spheres,
    scalar_transform_points,   scalar_transform_vectors,
    nph_f32_to_f16_array,
};

const NphMathKernels *nph_math_kernels(NphSimdLevel level)
{
    switch (level) {
    case NPH_SIMD_LEVEL_SSE4:
        return &g_nph_math_kernels_sse4;
    case NPH_SIMD_LEVEL_AVX2:
        return &g_nph_math_kernels_avx2;
    case NPH_SIMD_LEVEL_SCALAR:
    case NPH_SIMD_LEVEL_COUNT:
        break;
    }
    return &g_nph_math_kernels_scalar;
}
