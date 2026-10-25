// SPDX-License-Identifier: Apache-2.0
#pragma once

// Spatialisation (CLAUDE.md §8.10 "3D: distance attenuation curves,
// equal-power panning, Doppler, occlusion via physics raycasts"; docs/audio.md
// "3D"): what a voice's place relative to the listener does to it. The
// mixer applies these to its 3D voices every block (mixer.h); they are
// here, pure, for it and for tools.
//
// Positions are in the listener's frame (ADR-0008): metres, +x to the
// right, +y up, -z ahead. The runtime gives each 3D voice its position and
// velocity there, computed from the engine's 64-bit world positions
// (CLAUDE.md §8.2), so precision holds however far from the origin.
//
//   - Distance attenuation: an inverse law (OpenAL's "inverse distance
//     clamped": physical at a factor of 1, -6 dB per doubling), a linear
//     fade to silence, or a designer's curve in decibels.
//   - Direction: the azimuth panned by pan.h, with a spread that widens as
//     a source of some size comes near (its angular size), and fills every
//     direction inside it.
//   - Doppler: the pitch of a source approaching or receding, c / (c + v)
//     for a radial velocity v (to first order in the relative velocity).
//   - Air absorption: the loss of high frequencies over distance, after
//     ISO 9613-1 ("Acoustics - Attenuation of sound during propagation
//     outdoors", 1993) at 20 C and 50% relative humidity, where the loss
//     grows as the square of the frequency through the audible top octaves:
//     a one-pole low-pass, whose loss grows the same way, matched to the
//     standard's 0.0766 dB/m at 8 kHz.
//
// Thread-safety: pure functions, NPH_THREADSAFE.

#include <nepheline/audio/audio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphAudioRolloff {
    NPH_AUDIO_ROLLOFF_INVERSE = 0, // min / (min + factor (d - min)), d clamped to [min, max]
    NPH_AUDIO_ROLLOFF_LINEAR = 1,  // 1 within min, 0 from max, linear between
    NPH_AUDIO_ROLLOFF_CURVE = 2,   // through the points, linear in dB between them
    NPH_AUDIO_ROLLOFF_NONE = 3,    // no attenuation
    NPH_AUDIO_ROLLOFF_COUNT = 4
} NphAudioRolloff;

#define NPH_AUDIO_CURVE_POINTS 8U

typedef struct NphAudioAttenuation {
    NphAudioRolloff rolloff;
    f32 min_distance; // full level within (m)
    f32 max_distance; // INVERSE: no further fall beyond; LINEAR: silent from here
    f32 factor;       // INVERSE: the law's steepness (1: physical)
    u32 point_count;  // CURVE: 1 .. NPH_AUDIO_CURVE_POINTS
    f32 points[NPH_AUDIO_CURVE_POINTS][2]; // CURVE: (distance m, gain dB), distances rising
} NphAudioAttenuation;

// Inverse, physical: full level within 1 m, falling to 50 m.
NphAudioAttenuation nph_audio_attenuation_default(void);

// The gain (linear, 0 .. 1) at `distance` metres. Before a curve's first
// point its first value, after its last point its last.
f32 nph_audio_attenuation_gain(const NphAudioAttenuation *a, f32 distance);

// The speed of sound in air at 20 C (m/s).
#define NPH_AUDIO_SPEED_OF_SOUND 343.0F

// The pitch ratio of a source moving away from the listener at
// `radial_velocity` m/s (negative: approaching), the effect scaled by
// `factor` (0: none, 1: physical), clamped to [0.5, 2].
f32 nph_audio_doppler_ratio(f32 radial_velocity, f32 speed_of_sound, f32 factor);

// The corner (Hz) of the analog one-pole low-pass that loses what air
// does over `distance` metres, its absorption scaled by `factor` (1: ISO
// 9613-1 at 20 C and 50% humidity). Infinity (no filter) where the loss
// is nil.
f32 nph_audio_air_corner(f32 distance, f32 factor);

// The coefficient `a` of the digital one-pole y += a (x - y) at `rate`
// whose loss at 8 kHz is the air's over `distance` (solved exactly for the
// digital filter, not taken from the analog corner; below 16 kHz rates
// the match moves down to 0.35 of the rate, the loss scaled by the square
// of the frequency). 1 (no filter) where the loss is nil.
f32 nph_audio_air_coeff(f32 distance, f32 factor, f32 rate);

// The azimuth (radians, 0 ahead, positive to the right) of a direction
// (x, z) in the listener's frame; 0 for the listener's own position.
f32 nph_audio_azimuth(f32 x, f32 z);

// The angular spread (radians, 0 .. 2 pi) of a source of radius `size` at
// `distance`: the angle it subtends (pi at its surface, half of all
// directions), widening inside it to all around at its centre.
f32 nph_audio_spread(f32 size, f32 distance);

#ifdef __cplusplus
}
#endif
