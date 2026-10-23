// SPDX-License-Identifier: Apache-2.0
// The contact kernels compiled with AVX2 (nph_set_avx2_sources): NphF32x8
// is one AVX register. No FMA is emitted (contraction is off, ADR-0008 §6),
// so the lanes compute exactly what the SSE4 build computes.

#if !defined(__AVX2__)
#error "solver_avx2.c must be compiled with AVX2 enabled (nph_set_avx2_sources)"
#endif

#define PHYS_KERNEL(name) phys_kernel_##name##_avx2
#include "solver_impl.h"

const PhysSolverKernels g_phys_solver_avx2 = {
    "avx2",
    PHYS_KERNEL(prepare),
    PHYS_KERNEL(warm_start),
    PHYS_KERNEL(solve),
    PHYS_KERNEL(restitution),
    PHYS_KERNEL(xpbd_positions),
    PHYS_KERNEL(xpbd_velocities),
};
