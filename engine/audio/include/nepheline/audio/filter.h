// SPDX-License-Identifier: Apache-2.0
#pragma once

// Filters (CLAUDE.md §8.10 "DSP: biquad filters"; docs/audio.md "Filters").
//
// The second-order filters are the biquads of the Audio EQ Cookbook
// (Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter
// coefficients", 2005): the bilinear transform, with the cutoff prewarped,
// of the analog low-pass, high-pass, band-pass, notch, all-pass, peaking,
// and shelving sections. They are realised as the topology-preserving
// state-variable filter (Zavalishin, "The Art of VA Filter Design", 2012;
// Simper, "Linear Trapezoidal Integrated SVF", Cytomic, 2013) rather than
// a direct form: the same transfer function, but its states are the
// integrators' and its coefficients are smooth functions of the cutoff, so
// the filter stays stable and free of zipper noise when its parameters
// change every block (a voice's occlusion), and it keeps its precision at
// low cutoffs, where a direct form's poles crowd against the unit circle.
//
// The one-pole low-pass smooths and darkens (air absorption): y += a (x - y)
// with a = 1 - exp(-2 pi f / rate), the impulse-invariant pole.
//
// No function allocates or locks; processing is real-time safe. States are
// plain structs owned by the caller.
//
// Thread-safety: designs are pure and NPH_THREADSAFE; a state belongs to
// one thread at a time.

#include <nepheline/audio/audio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphAudioFilterType {
    NPH_AUDIO_FILTER_LOWPASS = 0,
    NPH_AUDIO_FILTER_HIGHPASS = 1,
    NPH_AUDIO_FILTER_BANDPASS = 2, // 0 dB at the centre frequency
    NPH_AUDIO_FILTER_NOTCH = 3,
    NPH_AUDIO_FILTER_ALLPASS = 4,
    NPH_AUDIO_FILTER_PEAK = 5,       // a bell of `gain_db` at the centre
    NPH_AUDIO_FILTER_LOW_SHELF = 6,  // `gain_db` below the corner
    NPH_AUDIO_FILTER_HIGH_SHELF = 7, // `gain_db` above the corner
    NPH_AUDIO_FILTER_COUNT = 8
} NphAudioFilterType;

// The coefficients of one section: the integrators' (a1, a2, a3, with
// g = tan(pi f / rate) and k = 1 / Q folded in) and the output mix of the
// input, band, and low states (m0, m1, m2). g and k are kept for the
// analytic response.
typedef struct NphAudioFilterCoeffs {
    f32 a1, a2, a3;
    f32 m0, m1, m2;
    f32 g, k;
} NphAudioFilterCoeffs;

// The two integrator states of one channel.
typedef struct NphAudioFilterState {
    f32 ic1eq, ic2eq;
} NphAudioFilterState;

// The coefficients for `type` at `freq` Hz (clamped to (0, 0.49 rate)),
// quality `q` (clamped to [0.025, 100]; 0.7071 is Butterworth; for the
// shelves, 0.7071 is the cookbook's slope S = 1), and `gain_db` (the peak
// and the shelves only), at `rate` Hz. An invalid type gives a filter that
// passes its input unchanged.
NphAudioFilterCoeffs nph_audio_filter_design(NphAudioFilterType type, f32 freq, f32 q, f32 gain_db,
                                             f32 rate);

// The identity: output = input.
NphAudioFilterCoeffs nph_audio_filter_passthrough(void);

NPH_INLINE void nph_audio_filter_reset(NphAudioFilterState *state)
{
    state->ic1eq = 0.0F;
    state->ic2eq = 0.0F;
}

// Filters `count` samples of one channel; `out` may be `in`.
void nph_audio_filter_process(const NphAudioFilterCoeffs *coeffs, NphAudioFilterState *state,
                              const f32 *in, f32 *out, u32 count);

// The magnitude of the filter's response at `freq` Hz for `rate`, from its
// transfer function (the bilinear transform of the analog section,
// evaluated exactly): what the processing converges to on a sine.
f64 nph_audio_filter_magnitude(const NphAudioFilterCoeffs *coeffs, f64 freq, f64 rate);

// ---------------------------------------------------------------------------
// One-pole low-pass
// ---------------------------------------------------------------------------

// The coefficient `a` for a corner at `freq` Hz (clamped to [0, rate / 2]):
// 0 holds the state, 1 passes the input.
f32 nph_audio_one_pole_coeff(f32 freq, f32 rate);

// state <- state + a (in[i] - state), out[i] = state; `out` may be `in`.
void nph_audio_one_pole_process(f32 a, f32 *state, const f32 *in, f32 *out, u32 count);

#ifdef __cplusplus
}
#endif
