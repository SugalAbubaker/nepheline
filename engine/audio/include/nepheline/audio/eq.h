// SPDX-License-Identifier: Apache-2.0
#pragma once

// The equaliser (CLAUDE.md §8.10 "DSP: biquad filters"; docs/audio.md
// "Effects"): up to NPH_AUDIO_EQ_BANDS sections of filter.h in series on
// every channel, each band any of the filter types.
//
// Every change is smooth:
//   - a band's new frequency, Q, or gain glides there across the next
//     block (nph_audio_filter_process_glide);
//   - a band switched on or off crossfades across a block between the
//     signal with it and without it (nph_audio_smooth_crossfade), so a
//     section starting from silence is not heard;
//   - a band changed to another type crossfades from its old section's
//     output to its new one's.
//
// No allocation: the state is the struct. Processing is real-time safe.
//
// Thread-safety: an equaliser belongs to one thread at a time.

#include <nepheline/audio/filter.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_AUDIO_EQ_BANDS 6U

typedef struct NphAudioEqBand {
    NphAudioFilterType type;
    f32 freq;    // Hz (clamped by the design to (0, 0.49 rate))
    f32 q;       // 0.7071 is Butterworth (the shelves' slope 1)
    f32 gain_db; // the peak and the shelves
    b32 enabled;
} NphAudioEqBand;

typedef struct NphAudioEq {
    f32 rate;
    u32 channels;
    NphAudioEqBand target[NPH_AUDIO_EQ_BANDS];       // as last set
    NphAudioEqBand running[NPH_AUDIO_EQ_BANDS];      // what the last block ran
    NphAudioFilterCoeffs coeffs[NPH_AUDIO_EQ_BANDS]; // the last block's designs
    NphAudioFilterState state[NPH_AUDIO_EQ_BANDS][NPH_AUDIO_MAX_CHANNELS];
} NphAudioEq;

// An equaliser of `channels` channels (1 .. NPH_AUDIO_MAX_CHANNELS) at
// `rate`, every band off.
void nph_audio_eq_init(NphAudioEq *eq, u32 channels, f32 rate);

// Sets band `band` (< NPH_AUDIO_EQ_BANDS; others are ignored), heard from
// the next block processed.
void nph_audio_eq_set_band(NphAudioEq *eq, u32 band, const NphAudioEqBand *settings);

// Back to silence: every band's state cleared, the settings kept.
void nph_audio_eq_reset(NphAudioEq *eq);

// Filters `frames` frames of the equaliser's channels in place. `scratch`
// holds at least `frames` floats.
void nph_audio_eq_process(NphAudioEq *eq, f32 *const *planes, u32 frames, f32 *scratch);

#ifdef __cplusplus
}
#endif
