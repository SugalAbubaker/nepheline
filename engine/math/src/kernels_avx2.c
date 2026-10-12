// SPDX-License-Identifier: Apache-2.0
// The batch kernels compiled with AVX2 + FMA + F16C enabled (see
// nph_set_avx2_sources in cmake/NphArch.cmake). NphF32x8 is one AVX
// register here. No FMA instructions are emitted: contraction is off
// (ADR-0008 §6) and the code never asks for them.

#if !defined(__AVX2__)
#error "kernels_avx2.c must be compiled with AVX2 enabled (nph_set_avx2_sources)"
#endif

#define NPH_KERNEL(name) nph_kernel_##name##_avx2
#include "kernels_impl.h"
#include "kernels_tables.h"

const NphMathKernels nph_math_kernels_avx2 = {
    NPH_SIMD_LEVEL_AVX2,
    "avx2",
    NPH_KERNEL(frustum_cull_aabbs),
    NPH_KERNEL(frustum_cull_spheres),
    NPH_KERNEL(transform_points),
    NPH_KERNEL(transform_vectors),
    NPH_KERNEL(f32_to_f16),
};
