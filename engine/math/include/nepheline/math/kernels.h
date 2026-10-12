// SPDX-License-Identifier: Apache-2.0
#pragma once

// Batch kernels with run-time ISA selection (docs/adr/0008-math-conventions.md
// §4, §9). The same kernel source is compiled once for the SSE4.2 baseline
// and once for AVX2 (engine/math/src/kernels_impl.h); a scalar reference
// built on geometry.h exists for testing and benchmarking. Callers pick a
// table once, typically at engine start from nph_simd_detect_level(), and
// call through it; dispatch never happens per element.
//
// Every kernel takes plain memory (arrays of storage types) and writes
// plain memory, so no wide value crosses between translation units
// compiled at different ISA levels. For the same inputs every level
// produces bit-identical outputs (ADR-0008 §6), which the tests assert.
//
// Thread-safety: every kernel is pure and NPH_THREADSAFE; the tables are
// immutable.

#include <nepheline/math/geometry.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphSimdLevel {
    NPH_SIMD_LEVEL_SCALAR = 0, // reference implementation, no SIMD
    NPH_SIMD_LEVEL_SSE4 = 1,   // the engine baseline (CLAUDE.md §3)
    NPH_SIMD_LEVEL_AVX2 = 2,   // AVX2 + FMA + F16C, OS support verified
    NPH_SIMD_LEVEL_COUNT
} NphSimdLevel;

// The highest level this CPU and operating system support, from CPUID and
// XGETBV. SCALAR is reported only on CPUs below the engine baseline.
// Thread-safety: NPH_THREADSAFE; cheap enough to call anywhere, but the
// result never changes within a process, so call it once.
NphSimdLevel nph_simd_detect_level(void);

// "scalar", "sse4", "avx2"; "invalid" otherwise.
const char *nph_simd_level_name(NphSimdLevel level);

typedef struct NphMathKernels {
    NphSimdLevel level;
    const char *name;

    // Culls `count` boxes against f. Bit i of out_visible (64 bits per word,
    // least significant first, (count + 63) / 64 words, padding bits
    // cleared) is set when box i is not outside the frustum: the same
    // predicate as nph_frustum_intersects_aabb, computed as
    // n·c + |n|·e + d >= 0 per plane with c and e the centre and half-size.
    void (*frustum_cull_aabbs)(const NphFrustum *f, const NphAABB *boxes, u32 count,
                               u64 *out_visible);

    // As frustum_cull_aabbs for spheres: n·c + d >= -r per plane.
    void (*frustum_cull_spheres)(const NphFrustum *f, const NphSphere *spheres, u32 count,
                                 u64 *out_visible);

    // out[i] = m · (in[i], 1), as nph_affine_transform_point. in and out may
    // be the same array but must not otherwise overlap.
    void (*transform_points)(const NphAffine *m, const NphVec3 *in, NphVec3 *out, u32 count);

    // out[i] = m · (in[i], 0), as nph_affine_transform_vector.
    void (*transform_vectors)(const NphAffine *m, const NphVec3 *in, NphVec3 *out, u32 count);

    // out[i] = nph_f32_to_f16(in[i]). The AVX2 table uses F16C.
    void (*f32_to_f16)(const f32 *in, u16 *out, u32 count);
} NphMathKernels;

// The kernel table for `level`. Every level is always available (the AVX2
// table is compiled into every x86-64 build); calling the AVX2 table on a
// CPU without AVX2 is undefined, so choose with nph_simd_detect_level().
// Out-of-range levels return the scalar table. Never fails.
const NphMathKernels *nph_math_kernels(NphSimdLevel level);

// nph_math_kernels(nph_simd_detect_level()).
NPH_INLINE const NphMathKernels *nph_math_kernels_best(void)
{
    return nph_math_kernels(nph_simd_detect_level());
}

#ifdef __cplusplus
}
#endif
