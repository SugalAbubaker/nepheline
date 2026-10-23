// SPDX-License-Identifier: Apache-2.0
// The contact kernels at the SSE4.2 baseline (NphF32x8 is a pair of SSE
// registers here).

#define PHYS_KERNEL(name) phys_kernel_##name##_sse4
#include "solver_impl.h"

const PhysSolverKernels g_phys_solver_sse4 = {
    "sse4",
    PHYS_KERNEL(prepare),
    PHYS_KERNEL(warm_start),
    PHYS_KERNEL(solve),
    PHYS_KERNEL(restitution),
    PHYS_KERNEL(xpbd_positions),
    PHYS_KERNEL(xpbd_velocities),
};
