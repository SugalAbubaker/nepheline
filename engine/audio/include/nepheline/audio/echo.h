// SPDX-License-Identifier: Apache-2.0
#pragma once

// The echo (CLAUDE.md §8.10 "DSP: ... delay"; docs/audio.md "Effects"): a
// feedback delay on every channel.
//
// Each channel has a delay line (delay.h). What comes out of it is heard
// (wet) and fed back into it through a damping low-pass and a high-pass,
// one pole each, so repeats darken the way echoes do and low rumble does
// not build up. In ping-pong (two channels) the channels' input is summed
// into the left line and each line feeds the other, so the repeats
// alternate sides.
//
// A new delay time is reached by crossfading, over 50 ms along a
// smoothstep, from a tap at the old delay to one at the new, not by
// sliding the tap, which would sweep the echo's pitch. Wet and dry gains
// ramp across a block. Taps read between samples with the Catmull-Rom
// spline.
//
// Memory: nph_audio_echo_init allocates the lines; nothing else allocates
// or locks. Processing is real-time safe.
//
// Thread-safety: an echo belongs to one thread at a time.

#include <nepheline/audio/delay.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAudioEchoParams {
    f32 delay_ms;    // 1 ms .. the echo's maximum
    f32 feedback;    // 0 .. 0.95 (clamped)
    f32 damping_hz;  // the loop's low-pass corner (0: none)
    f32 highpass_hz; // the loop's high-pass corner (0: none)
    f32 wet;         // linear
    f32 dry;         // linear
    b32 ping_pong;   // two channels only
} NphAudioEchoParams;

// 350 ms, feedback 0.4, damping 5 kHz, high-pass 120 Hz, wet 0.5, dry 1,
// not ping-pong.
NphAudioEchoParams nph_audio_echo_params_default(void);

#define NPH_AUDIO_ECHO_FADE_MS 50.0F

typedef struct NphAudioEcho {
    NphAudioEchoParams params;
    f32 rate;
    u32 channels;
    f32 max_delay; // frames
    NphAudioDelay lines[NPH_AUDIO_MAX_CHANNELS];
    f32 delay_now;  // the tap being faded to (frames)
    f32 delay_from; // the tap being faded from
    u32 fade_left;  // frames of the tap crossfade remaining
    u32 fade_length;
    f32 lowpass_coeff;
    f32 highpass_coeff;
    f32 lowpass_state[NPH_AUDIO_MAX_CHANNELS];
    f32 highpass_state[NPH_AUDIO_MAX_CHANNELS];
    f32 wet_now;
    f32 dry_now;
} NphAudioEcho;

// An echo of `channels` channels at `rate` for delays up to `max_delay_ms`.
// NPH_RESULT_INVALID_ARGUMENT for parameters out of range;
// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_echo_init(NphAudioEcho *e, NphAllocator *alloc, u32 channels,
                                           f32 rate, f32 max_delay_ms,
                                           const NphAudioEchoParams *params);

void nph_audio_echo_free(NphAudioEcho *e);

// New parameters, heard from the next block (the delay time by a
// crossfade, the gains by ramps).
void nph_audio_echo_set(NphAudioEcho *e, const NphAudioEchoParams *params);

// Back to silence.
void nph_audio_echo_reset(NphAudioEcho *e);

// Processes `frames` frames of the echo's channels in place.
void nph_audio_echo_process(NphAudioEcho *e, f32 *const *planes, u32 frames);

#ifdef __cplusplus
}
#endif
