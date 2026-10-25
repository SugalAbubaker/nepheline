// SPDX-License-Identifier: Apache-2.0
#pragma once

// The mixer's block kernels with run-time ISA selection (docs/audio.md
// "Kernels"; ADR-0051), on the pattern of the math module's
// (math/kernels.h, ADR-0008 §4, §9): one source compiled at the SSE4.2
// baseline and again for AVX2 (src/kernels_impl.h), and a scalar
// reference for the tests. The SSE4.2 and AVX2 tables compute every
// element with the same operations in the same order (eight lanes either
// way: NphF32x8 is one AVX register or two SSE ones), so they agree to the
// bit; the scalar reference sums in another order and agrees within
// rounding.
//
// Every kernel takes plain memory and writes plain memory; none
// allocates, locks, or keeps state, so all of them may run on the audio
// thread. `count` may be any value, including 0; pointers need no
// particular alignment, and outputs must not overlap inputs unless a
// kernel says so.
//
// Gain ramps are linear: element i of a block of `count` gets
// g0 + i * (g1 - g0) / count, so a block ramps from g0 to just short of
// g1 and the next block, starting at g1, continues without a step.
//
// Thread-safety: every kernel is pure and NPH_THREADSAFE; the tables are
// immutable.

#include <nepheline/audio/audio.h>
#include <nepheline/math/kernels.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAudioResampleBank NphAudioResampleBank; // resampler.h

typedef struct NphAudioKernels {
    NphSimdLevel level;
    const char *name;

    // dst[i] += src[i] * gain.
    void (*mix)(f32 *dst, const f32 *src, u32 count, f32 gain);

    // dst[i] += src[i] * (g0 + i * (g1 - g0) / count).
    void (*mix_ramp)(f32 *dst, const f32 *src, u32 count, f32 g0, f32 g1);

    // dst[i] *= g0 + i * (g1 - g0) / count, in place.
    void (*scale_ramp)(f32 *dst, u32 count, f32 g0, f32 g1);

    // The largest |src[i]| (0 for an empty range). NaNs are skipped.
    f32 (*peak)(const f32 *src, u32 count);

    // The sum of src[i]^2 (0 for an empty range).
    f32 (*sum_squares)(const f32 *src, u32 count);

    // One channel of interleaved samples into planar floats:
    // out[i] = in[i * stride] scaled to [-1, 1) (integers: x / 2^(bits-1)).
    // `stride` is the source's channel count (>= 1); `in` points at the
    // channel's first sample.
    void (*load_s16)(const void *in, u32 stride, f32 *out, u32 count);
    void (*load_s24)(const void *in, u32 stride, f32 *out, u32 count);
    void (*load_s32)(const void *in, u32 stride, f32 *out, u32 count);
    void (*load_f32)(const void *in, u32 stride, f32 *out, u32 count);

    // Interleaves `channels` planar channels of `count` frames into `out`
    // (count * channels values), each clamped to [-1, 1]; NaNs become 0.
    void (*store_f32)(const f32 *const *planar, u32 channels, f32 *out, u32 count);

    // As store_f32 into signed 16-bit, rounded to nearest (ties to even).
    // No dither: a caller that wants it adds the noise first.
    void (*store_s16)(const f32 *const *planar, u32 channels, i16 *out, u32 count);

    // Bandlimited interpolation (resampler.h): writes `count` frames of
    // each of `channels` channels to dst[c], the first at source position
    // `pos` and each next one `step` further (both 32.32 fixed point, in
    // frames of src). src[c][k] must be readable for every k from
    // floor(pos) - (taps / 2 - 1) to floor(pos + (count - 1) * step) +
    // taps / 2, with `taps` the bank's (so the caller keeps that much
    // history before the first position).
    void (*resample)(const NphAudioResampleBank *bank, const f32 *const *src, u32 channels, u64 pos,
                     u64 step, f32 *const *dst, u32 count);
} NphAudioKernels;

// The kernel table for `level`. Every level is compiled into every x86-64
// build; calling the AVX2 table on a CPU without AVX2 is undefined, so
// choose with nph_simd_detect_level(). Out-of-range levels return the
// scalar table. Never fails.
const NphAudioKernels *nph_audio_kernels(NphSimdLevel level);

// nph_audio_kernels(nph_simd_detect_level()).
NPH_INLINE const NphAudioKernels *nph_audio_kernels_best(void)
{
    return nph_audio_kernels(nph_simd_detect_level());
}

#ifdef __cplusplus
}
#endif
