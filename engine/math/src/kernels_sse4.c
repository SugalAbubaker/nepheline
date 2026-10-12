// SPDX-License-Identifier: Apache-2.0
// The batch kernels at the SSE4.2 baseline (NphF32x8 is a pair of SSE
// registers here).

#define NPH_KERNEL(name) nph_kernel_##name##_sse4
#include "kernels_impl.h"
#include "kernels_tables.h"

const NphMathKernels nph_math_kernels_sse4 = {
    NPH_SIMD_LEVEL_SSE4,
    "sse4",
    NPH_KERNEL(frustum_cull_aabbs),
    NPH_KERNEL(frustum_cull_spheres),
    NPH_KERNEL(transform_points),
    NPH_KERNEL(transform_vectors),
    NPH_KERNEL(f32_to_f16),
};
