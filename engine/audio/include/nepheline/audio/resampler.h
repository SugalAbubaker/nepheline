// SPDX-License-Identifier: Apache-2.0
#pragma once

// Bandlimited interpolation: the voices' sample-rate conversion and pitch
// (docs/audio.md "Resampling"; ADR-0051).
//
// Every output sample is the source convolved with a windowed sinc centred
// on the output's position in the source (Smith & Gossett, "A Flexible
// Sampling-Rate Conversion Method", ICASSP 1984; Smith, "Digital Audio
// Resampling Home Page", CCRMA): a Kaiser window (Kaiser, "Nonrecursive
// digital filter design using the I0-sinh window function", 1974) of beta
// 10.06 over 24 zero crossings each side, 48 taps. Its passband is flat
// within 0.005 dB to 0.44 of the source's rate (21.1 kHz of a 48 kHz
// source), it is -6 dB at the Nyquist frequency, and its stopband is at
// least 100 dB down from 0.57 of the rate.
//
// The kernel is tabulated per fraction of a sample: 16 segments, each tap a
// cubic in the position within its segment, fitted through Chebyshev
// nodes. The fit is 125 dB below the signal, so the table is exact for
// every purpose while small enough to stay in the L1 cache (12 KiB).
//
// Playing faster than the mix rate (pitch up, or a source rate above the
// mix rate: a `step` above 1) needs the cutoff lowered to the output's
// Nyquist frequency, or what lies above it aliases. The resampler keeps a
// bank of kernels per cutoff scale 2^(-k/12), k = 0..24, from 1 down to
// 1/4: each stretched by 1/scale (more taps, up to 192) and scaled to unit
// gain. A step uses the bank with the largest scale at or below 1/step,
// whose cutoff is at most a semitone (5.6%) below the ideal one: the
// passband then ends between 0.415 and 0.44 of the mix rate (19.9 to
// 21.1 kHz at 48 kHz), and nothing aliases below 0.46 of it. Steps are
// capped at 4.
//
// Positions and steps are 32.32 fixed point in source frames, so a voice's
// position accumulates exactly, block after block, with no drift.
//
// Costs (§1.4): nph_audio_resampler_create allocates the banks (670 KiB)
// and computes them (a few milliseconds); everything after is reads of
// immutable tables.
//
// Thread-safety: create and destroy are NPH_MAIN_THREAD_ONLY; a created
// resampler is immutable and every query is NPH_THREADSAFE.

#include <nepheline/audio/audio.h>
#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

// The kernel's half-width at scale 1, in zero crossings (48 taps).
#define NPH_AUDIO_RESAMPLE_ZERO_CROSSINGS 24U
// Cubic segments per source frame.
#define NPH_AUDIO_RESAMPLE_PHASE_BITS 4U
#define NPH_AUDIO_RESAMPLE_PHASES     (1U << NPH_AUDIO_RESAMPLE_PHASE_BITS)
// Banks: cutoff scales 2^(-k/12) for k = 0 .. NPH_AUDIO_RESAMPLE_BANKS - 1.
#define NPH_AUDIO_RESAMPLE_BANKS 25U
// The largest step (source frames per output frame) a bank covers.
#define NPH_AUDIO_RESAMPLE_MAX_STEP 4U
// Taps of the widest bank (scale 1/4), and so the most frames a caller
// needs on either side of a position: taps / 2 - 1 before, taps / 2 after.
#define NPH_AUDIO_RESAMPLE_MAX_TAPS 192U

// 32.32 fixed point: one source frame.
#define NPH_AUDIO_FRAME_ONE (1ULL << 32U)

// One kernel: `taps` coefficients (a multiple of 8) for each of the
// NPH_AUDIO_RESAMPLE_PHASES segments and each of the four powers of the
// position within the segment, laid out [segment][power][tap]. (kernels.h
// declares the same typedef, which C11 allows.)
typedef struct NphAudioResampleBank NphAudioResampleBank;
struct NphAudioResampleBank {
    const f32 *coeffs;
    u32 taps;
    f32 scale; // the cutoff relative to the source's Nyquist frequency
};

typedef struct NphAudioResampler NphAudioResampler;

// Computes the banks. NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_resampler_create(NphAllocator *alloc, NphAudioResampler **out);

// Safe on NULL.
void nph_audio_resampler_destroy(NphAudioResampler *resampler);

// The bank for `step` (32.32): the full-band kernel for steps up to one
// frame, otherwise the bank whose scale is the largest at or below
// 1 / step (the narrowest, for steps beyond NPH_AUDIO_RESAMPLE_MAX_STEP).
const NphAudioResampleBank *nph_audio_resampler_bank(const NphAudioResampler *resampler, u64 step);

// Bank `index` (0 .. NPH_AUDIO_RESAMPLE_BANKS - 1), or NULL.
const NphAudioResampleBank *nph_audio_resampler_bank_at(const NphAudioResampler *resampler,
                                                        u32 index);

// The 32.32 step for `ratio` source frames per output frame (the source
// rate over the mix rate, times the pitch), rounded to nearest and clamped
// to [1 / 2^32, NPH_AUDIO_RESAMPLE_MAX_STEP]. Non-finite or non-positive
// ratios give the smallest step.
u64 nph_audio_resample_step(f64 ratio);

// The source frames the kernel reads for `count` outputs from `pos` with
// `step` and a bank of `taps`: from *out_first to *out_last inclusive
// (*out_first may be negative: frames before the source's start, which the
// caller supplies as history or silence).
void nph_audio_resample_span(u64 pos, u64 step, u32 count, u32 taps, i64 *out_first, i64 *out_last);

// The kernel's value at offset `t` source frames from its centre for a
// cutoff `scale` (1 = full band), evaluated exactly in double precision:
// the function the banks tabulate, for tests and design checks.
f64 nph_audio_resample_kernel(f64 t, f64 scale);

#ifdef __cplusplus
}
#endif
