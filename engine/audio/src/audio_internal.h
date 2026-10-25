// SPDX-License-Identifier: Apache-2.0
#pragma once

// Declarations shared by the audio module's translation units.

#include <nepheline/audio/kernels.h>

extern const NphAudioKernels g_audio_kernels_scalar;
extern const NphAudioKernels g_audio_kernels_sse4;
extern const NphAudioKernels g_audio_kernels_avx2;
