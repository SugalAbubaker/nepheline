// SPDX-License-Identifier: Apache-2.0
// The audio block kernels at the SSE4.2 baseline (NphF32x8 is a pair of SSE
// registers here).

#define AK(name) audio_kernel_##name##_sse4
#include "audio_internal.h"
#include "kernels_impl.h"

const NphAudioKernels g_audio_kernels_sse4 = AK_TABLE(NPH_SIMD_LEVEL_SSE4, "sse4");
