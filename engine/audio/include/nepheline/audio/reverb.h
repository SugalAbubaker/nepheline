// SPDX-License-Identifier: Apache-2.0
#pragma once

// The reverberator (CLAUDE.md §8.10 "reverb (Schroeder 1962; feedback
// delay network after Jot & Chaigne 1991)"; docs/audio.md "Effects").
//
// The structure (Valimaki et al., "Fifty Years of Artificial
// Reverberation", IEEE TASLP 20(5), 2012, reviews the lineage):
//   - the channels' sum waits out the pre-delay;
//   - early reflections: eight taps of the pre-delayed signal, spread over
//     the first 60 ms (scaled by the size), alternating sides, falling in
//     level (Moorer, "About This Reverberation Business", CMJ 3(2), 1979);
//   - diffusion: four Schroeder all-passes in series ("Natural Sounding
//     Artificial Reverberation", JAES 10(3), 1962) smear the input into a
//     dense attack, with the lengths and gains of Dattorro ("Effect Design,
//     Part 1", JAES 45(9), 1997);
//   - the tail: a feedback delay network of 16 lines (Jot & Chaigne,
//     "Digital Delay Networks for Designing Artificial Reverberators", AES
//     90th Convention, 1991) of mutually prime lengths, mixed by the
//     orthonormal 16 x 16 Hadamard matrix (a fast Walsh-Hadamard
//     transform: 64 additions a frame), so the network loses nothing by
//     itself;
//   - the decay: each line passes through a first-order shelf whose gains
//     at DC and Nyquist make that line lose exactly 60 dB in the low and
//     the high decay time (Jot, "An Analysis/Synthesis Approach to
//     Real-Time Artificial Reverberation", ICASSP 1992), crossing over at
//     a corner;
//   - four of the lines wander slowly (a fraction of a millisecond, under
//     1 Hz, read between samples by Catmull-Rom) so that the modes do not
//     ring (Dattorro 1997);
//   - each output channel taps the lines with its own row of the Hadamard
//     matrix: the channels are mutually uncorrelated, and a width blends
//     them towards the same signal.
//
// Parameter changes glide (the decay times and the crossover over about
// 50 ms; levels by block ramps); a new size takes new line lengths with
// a block's crossfade from the old taps.
//
// Memory: nph_audio_reverb_init allocates the lines and the work space;
// nothing else allocates or locks. Processing is real-time safe.
//
// Thread-safety: a reverberator belongs to one thread at a time.

#include <nepheline/audio/delay.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAudioReverbParams {
    f32 size;         // 0.25 .. 2: scales every length (1: lines of 30 to 84 ms)
    f32 decay_s;      // the low frequencies' T60, 0.1 .. 30 s
    f32 damping;      // the high frequencies' T60 as a fraction of decay_s, 0.05 .. 1
    f32 crossover_hz; // where the two decay times meet, 100 .. 10000
    f32 predelay_ms;  // 0 .. 250
    f32 diffusion;    // 0 .. 1
    f32 modulation;   // 0 .. 1: how far the wandering lines wander
    f32 early;        // the early reflections' level (linear)
    f32 late;         // the tail's level (linear)
    f32 width;        // 0 (every channel the same) .. 1 (uncorrelated)
    f32 dry;          // the input's level (linear; 0 on a send bus)
} NphAudioReverbParams;

// A medium room: size 1, decay 1.6 s, damping 0.5, crossover 3 kHz,
// pre-delay 12 ms, diffusion 0.8, modulation 0.3, early 0.3, late 0.5,
// width 1, dry 0.
NphAudioReverbParams nph_audio_reverb_params_default(void);

#define NPH_AUDIO_REVERB_LINES 16U
#define NPH_AUDIO_REVERB_TAPS  8U // early reflections

typedef struct NphAudioReverb {
    NphAudioReverbParams params; // the targets
    f32 rate;
    u32 channels;
    // What the last block ran: the gliding parameters and the levels.
    f32 decay_now;
    f32 damping_now;
    f32 crossover_now;
    f32 early_now;
    f32 late_now;
    f32 dry_now;
    f32 width_now;
    f32 size_now;     // the size the lengths were made for
    f32 size_from;    // the size being faded from
    u32 predelay_now; // frames
    u32 predelay_from;
    b32 fading; // a new size or pre-delay crossfades in across this block
    NphAudioDelay lines[NPH_AUDIO_REVERB_LINES];
    u32 length[NPH_AUDIO_REVERB_LINES];      // frames
    u32 length_from[NPH_AUDIO_REVERB_LINES]; // the old lengths, while a new size fades in
    f32 out_sign[NPH_AUDIO_MAX_CHANNELS][NPH_AUDIO_REVERB_LINES]; // each output's Hadamard row
    f32 b0[NPH_AUDIO_REVERB_LINES];                               // the decay shelves
    f32 b1[NPH_AUDIO_REVERB_LINES];
    f32 a1[NPH_AUDIO_REVERB_LINES];
    f32 shelf_x[NPH_AUDIO_REVERB_LINES]; // their states
    f32 shelf_y[NPH_AUDIO_REVERB_LINES];
    f32 lfo_phase; // the wandering lines' (radians)
    f32 lfo_step;
    NphAudioDelay predelay;
    NphAudioDelay diffuser[4];
    u32 diffuser_length[4];
    u32 tap_time[NPH_AUDIO_REVERB_TAPS]; // frames after the pre-delay
    f32 *work;                           // NPH_AUDIO_REVERB_LINES * NPH_AUDIO_BLOCK_FRAMES
    f32 *input;                          // NPH_AUDIO_BLOCK_FRAMES: the diffused input
    f32 *early_l; // NPH_AUDIO_BLOCK_FRAMES each: the early reflections' two sides
    f32 *early_r;
    f32 *scratch; // NPH_AUDIO_BLOCK_FRAMES: an old tap while a size fades
    NphAllocator *alloc;
} NphAudioReverb;

// A reverberator with `channels` outputs (and inputs) at `rate`.
// NPH_RESULT_INVALID_ARGUMENT for parameters out of range;
// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_reverb_init(NphAudioReverb *r, NphAllocator *alloc, u32 channels,
                                             f32 rate, const NphAudioReverbParams *params);

void nph_audio_reverb_free(NphAudioReverb *r);

// New parameters, heard from the next block. Values out of range are
// clamped.
void nph_audio_reverb_set(NphAudioReverb *r, const NphAudioReverbParams *params);

// Back to silence.
void nph_audio_reverb_reset(NphAudioReverb *r);

// Processes `frames` frames of the reverberator's channels in place.
void nph_audio_reverb_process(NphAudioReverb *r, f32 *const *planes, u32 frames);

#ifdef __cplusplus
}
#endif
