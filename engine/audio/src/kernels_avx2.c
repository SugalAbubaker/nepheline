// SPDX-License-Identifier: Apache-2.0
// The audio block kernels with AVX2 (compiled with -mavx2 -mfma -mf16c or
// /arch:AVX2, contraction still off: cmake/NphArch.cmake), selected at run
// time after CPUID.

#define AK(name) audio_kernel_##name##_avx2
#include "audio_internal.h"
#include "kernels_impl.h"

const NphAudioKernels g_audio_kernels_avx2 = AK_TABLE(NPH_SIMD_LEVEL_AVX2, "avx2");
