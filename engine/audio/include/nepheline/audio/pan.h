// SPDX-License-Identifier: Apache-2.0
#pragma once

// Panning: the gains that place a voice among the output's speakers
// (CLAUDE.md §8.10 "equal-power panning"; docs/audio.md "Panning").
//
// Every panner keeps the total power of a source constant wherever it is
// placed (the sum of the squared gains is 1), so a sound moving around the
// listener neither swells nor dips.
//
//   - Stereo pans by the source's lateral position, the sine of its
//     azimuth, with the sine-cosine (equal-power) law: centred is -3 dB in
//     each channel, and a source behind folds to its mirror image in front,
//     as it must on two channels.
//   - Quad, 5.1, and 7.1 use pairwise vector base amplitude panning (Pulkki,
//     "Virtual Sound Source Positioning Using Vector Base Amplitude
//     Panning", JAES 45(6), 1997) over the ring of full-range speakers: the
//     source sits between the two speakers that bracket its direction, with
//     the gains that sum their direction vectors to it, normalised to unit
//     power. The low-frequency channel gets none.
//   - Mono gets everything in its one channel.
//
// A source with `spread` (radians, 0 for a point, up to 2 pi for all
// around) is the sum of virtual sources spread evenly over that arc, each
// panned as above (Pulkki, "Uniform Spreading of Amplitude Panned Virtual
// Sources", WASPAA 1999), and normalised to unit power: a wide source
// covers more speakers, and one spread all around comes from everywhere.
//
// Thread-safety: a panner is immutable after init; everything is pure and
// NPH_THREADSAFE.

#include <nepheline/audio/audio.h>

#ifdef __cplusplus
extern "C" {
#endif

// The virtual sources a spread source is made of.
#define NPH_AUDIO_PAN_SPREAD_SOURCES 16U

typedef struct NphAudioPanner {
    NphAudioLayout layout;
    u32 channels;
    u32 ring_count;                              // full-range speakers
    u32 ring_channel[NPH_AUDIO_MAX_CHANNELS];    // their channels, by rising azimuth
    f32 ring_azimuth[NPH_AUDIO_MAX_CHANNELS];    // their azimuths, in (-pi, pi]
    f32 ring_inverse[NPH_AUDIO_MAX_CHANNELS][4]; // per pair (k, k+1): the inverse speaker matrix
} NphAudioPanner;

// The equal-power stereo gains for `pan` in [-1, 1] (clamped; -1 left).
void nph_audio_pan_stereo(f32 pan, f32 *out_left, f32 *out_right);

// Prepares the panner for `layout`. NPH_RESULT_INVALID_ARGUMENT for an
// invalid layout.
NPH_MUST_USE NphResult nph_audio_panner_init(NphAudioPanner *panner, NphAudioLayout layout);

// The gain of each of the layout's channels (gains[c], c < channels; the
// rest of the array is left alone) for a source at `azimuth` (radians, 0
// ahead, positive to the right; any value, wrapped) with angular `spread`
// (clamped to [0, 2 pi]).
void nph_audio_pan(const NphAudioPanner *panner, f32 azimuth, f32 spread,
                   f32 gains[NPH_AUDIO_MAX_CHANNELS]);

#ifdef __cplusplus
}
#endif
