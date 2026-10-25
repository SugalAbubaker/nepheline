// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers for the mixer's tests: a mixer, clips made from signals, renders
// split into channels.

#include <nepheline/audio/adpcm.h>
#include <nepheline/audio/lossless.h>
#include <nepheline/audio/mixer.h>

#include "test_audio_support.h"

#define TM_RATE 48000U

static inline NphAudioMixer *tm_mixer(u32 channels, u32 max_voices, u32 max_real)
{
    NphAudioMixerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.sample_rate = TM_RATE;
    d.channels = channels;
    d.max_voices = max_voices;
    d.max_real_voices = max_real;
    d.max_clips = 64U;
    d.max_buses = 16U;
    d.max_snapshots = 8U;
    NphAudioMixer *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_OK);
    return m;
}

// A float PCM clip of `frames` frames of `channels` interleaved samples.
static inline NphAudioClip tm_clip_f32(NphAudioMixer *m, const f32 *interleaved, u64 frames,
                                       u32 channels, u32 rate)
{
    NphAudioClipDesc d;
    memset(&d, 0, sizeof d);
    d.codec = NPH_AUDIO_CODEC_PCM;
    d.format = NPH_AUDIO_FORMAT_F32;
    d.channels = channels;
    d.sample_rate = rate;
    d.frames = frames;
    d.data = interleaved;
    d.size = frames * channels * sizeof(f32);
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, &d, &clip), NPH_RESULT_OK);
    return clip;
}

// A mono float clip of a sine.
static inline NphAudioClip tm_tone(NphAudioMixer *m, f64 freq, u64 frames, u32 rate, f32 amp,
                                   f32 **out_samples)
{
    f32 *x = test_buffer((u32)frames);
    test_sine(x, (u32)frames, freq, (f64)rate, 0.0, (f64)amp);
    const NphAudioClip clip = tm_clip_f32(m, x, frames, 1U, rate);
    if (out_samples != NULL) {
        *out_samples = x;
    } else {
        test_buffer_free(x, (u32)frames);
    }
    return clip;
}

// Renders `frames` frames and splits them into `channels` planes (each a
// test_buffer of `frames`).
static inline void tm_render(NphAudioMixer *m, u32 frames, f32 **planes)
{
    const u32 channels = nph_audio_mixer_channels(m);
    f32 *out = test_buffer(frames * channels);
    nph_audio_mixer_render(m, out, frames);
    for (u32 c = 0U; c < channels; ++c) {
        for (u32 i = 0U; i < frames; ++i) {
            planes[c][i] = out[(u64)i * channels + c];
        }
    }
    test_buffer_free(out, frames * channels);
}

static inline NphAudioVoice tm_play(NphAudioMixer *m, const NphAudioPlayDesc *d)
{
    NphAudioVoice v;
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, d, &v), NPH_RESULT_OK);
    return v;
}
