// SPDX-License-Identifier: Apache-2.0
#pragma once

// The per-level kernel tables, each defined in its own translation unit so
// that only that unit is compiled with the matching ISA flags.

#include <nepheline/math/kernels.h>

extern const NphMathKernels nph_math_kernels_scalar;
extern const NphMathKernels nph_math_kernels_sse4;
extern const NphMathKernels nph_math_kernels_avx2;
