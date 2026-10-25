// SPDX-License-Identifier: Apache-2.0
// The mixer (mixer.h), feature by feature, offline: gains and panning to the
// bit, pitch and clip rates on tones, sample-accurate starts, loops, ends,
// fades, buses, sends, virtual voices, ducking, snapshots, every codec,
// clips removed under their voices, full rings, determinism, and any render
// size.

#include <nepheline/audio/pan.h>

#include "test_mixer_support.h"

NTEST(audio_mixer, creation_checks_its_limits)
{
    NphAudioMixerDesc d;
    memset(&d, 0, sizeof d);
    NphAudioMixer *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_INVALID_ARGUMENT);
    d.alloc = nph_allocator_system();
    d.channels = 3U;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_INVALID_ARGUMENT);
    d.channels = 0U;
    d.sample_rate = 1000U;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_INVALID_ARGUMENT);
    d.sample_rate = 0U;
    d.max_voices = 4U;
    d.max_real_voices = 8U;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_INVALID_ARGUMENT);
    d.max_real_voices = 0U;
    d.max_voices = 0U;
    d.command_capacity = 100U; // not a power of two
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_INVALID_ARGUMENT);
    d.command_capacity = 0U;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_audio_mixer_sample_rate(m), 48000U);
    NTEST_ASSERT_EQ_UINT(nph_audio_mixer_channels(m), 2U);
    // Silence until something plays.
    f32 out[2 * 300];
    nph_audio_mixer_render(m, out, 300U);
    for (u32 i = 0U; i < 600U; ++i) {
        NTEST_ASSERT_TRUE(out[i] == 0.0F);
    }
    nph_audio_mixer_destroy(m);
    nph_audio_mixer_destroy(NULL);
}

NTEST(audio_mixer, a_centred_mono_voice_is_equal_power)
{
    NphAudioMixer *m = tm_mixer(2U, 16U, 8U);
    f32 *x = NULL;
    const NphAudioClip clip = tm_tone(m, 1000.0, 24000U, TM_RATE, 0.5F, &x);
    const NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    (void)tm_play(m, &d);
    f32 *l = test_buffer(4800U);
    f32 *r = test_buffer(4800U);
    f32 *planes[2] = {l, r};
    tm_render(m, 4800U, planes);
    // The panner's gains for straight ahead: the equal-power law's
    // cos(pi/4), normalised to unit power.
    NphAudioPanner stereo;
    NTEST_ASSERT_EQ_INT(nph_audio_panner_init(&stereo, NPH_AUDIO_LAYOUT_STEREO), NPH_RESULT_OK);
    f32 g[NPH_AUDIO_MAX_CHANNELS];
    nph_audio_pan(&stereo, 0.0F, 0.0F, g);
    NTEST_ASSERT_NEAR_F32(g[0], 0.70710678F, 1e-7F);
    NTEST_ASSERT_TRUE(g[0] == g[1]);
    // At the mix rate on whole frames the clip is copied, and every gain is
    // constant: the output is the samples times those gains, to the bit.
    for (u32 i = 0U; i < 4800U; ++i) {
        NTEST_ASSERT_TRUE(l[i] == x[i] * g[0]);
        NTEST_ASSERT_TRUE(r[i] == x[i] * g[1]);
    }
    test_buffer_free(l, 4800U);
    test_buffer_free(r, 4800U);
    test_buffer_free(x, 24000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, volume_and_pan)
{
    NphAudioMixer *m = tm_mixer(2U, 16U, 8U);
    f32 *x = NULL;
    const NphAudioClip clip = tm_tone(m, 440.0, 24000U, TM_RATE, 0.8F, &x);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.volume = 0.5F;
    d.pan = -1.0F;
    (void)tm_play(m, &d);
    f32 *l = test_buffer(2048U);
    f32 *r = test_buffer(2048U);
    f32 *planes[2] = {l, r};
    tm_render(m, 2048U, planes);
    for (u32 i = 0U; i < 2048U; ++i) {
        NTEST_ASSERT_NEAR_F32(l[i], x[i] * 0.5F, 1e-7F);
        NTEST_ASSERT_NEAR_F32(r[i], 0.0F, 1e-7F);
    }
    test_buffer_free(l, 2048U);
    test_buffer_free(r, 2048U);
    test_buffer_free(x, 24000U);
    nph_audio_mixer_destroy(m);
}

// The gain with which channel `c` of an `in_channels` clip reaches each of
// the output's channels, from a steady value played on that channel alone.
static void tm_channel_gains(u32 in_channels, u32 c, u32 out_channels, f32 pan,
                             f32 gains[NPH_AUDIO_MAX_CHANNELS])
{
    enum {
        FRAMES = 2048,
        RENDER = 1024
    };
    NphAudioMixer *m = tm_mixer(out_channels, 4U, 4U);
    f32 *x = test_buffer(FRAMES * in_channels);
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        x[(u64)i * in_channels + c] = 0.25F;
    }
    const NphAudioClip clip = tm_clip_f32(m, x, FRAMES, in_channels, TM_RATE);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.pan = pan;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(RENDER * out_channels);
    nph_audio_mixer_render(m, out, RENDER);
    for (u32 o = 0U; o < NPH_AUDIO_MAX_CHANNELS; ++o) {
        gains[o] = o < out_channels ? out[(u64)(RENDER / 2) * out_channels + o] / 0.25F : 0.0F;
    }
    test_buffer_free(out, RENDER * out_channels);
    test_buffer_free(x, FRAMES * in_channels);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, multichannel_clips_map_speaker_to_speaker)
{
    // Each row: a clip's channel, and the gains it must reach the output's
    // channels with (the WAVE orders of audio.h).
    typedef struct Row {
        u32 in_channels;
        u32 channel;
        u32 out_channels;
        f32 gains[NPH_AUDIO_MAX_CHANNELS];
    } Row;
    const f32 h = 0.70710678F;
    const Row rows[] = {
        // Matching layouts: channel to channel, the LFE included.
        {2U, 0U, 2U, {1.0F, 0.0F}},
        {2U, 1U, 2U, {0.0F, 1.0F}},
        {4U, 2U, 4U, {0.0F, 0.0F, 1.0F, 0.0F}},
        {6U, 2U, 6U, {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F}},
        {6U, 3U, 6U, {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F}},
        {6U, 5U, 6U, {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}},
        {8U, 4U, 8U, {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F}},
        {8U, 7U, 8U, {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F}},
        // A stereo clip keeps its width on every layout.
        {2U, 0U, 4U, {1.0F, 0.0F, 0.0F, 0.0F}},
        {2U, 1U, 6U, {0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
        {2U, 1U, 8U, {0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
        // 5.1's surrounds are 7.1's sides; the centre and LFE stay theirs.
        {6U, 4U, 8U, {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}},
        {6U, 2U, 8U, {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}},
        {8U, 6U, 6U, {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}},
        // Down to stereo: the centre splits at -3 dB, the rest go to their
        // side, the LFE nowhere.
        {6U, 2U, 2U, {h, h}},
        {6U, 3U, 2U, {0.0F, 0.0F}},
        {6U, 4U, 2U, {1.0F, 0.0F}},
        {6U, 5U, 2U, {0.0F, 1.0F}},
        {4U, 2U, 2U, {1.0F, 0.0F}},
        {8U, 5U, 2U, {0.0F, 1.0F}},
        {8U, 6U, 2U, {1.0F, 0.0F}},
        // Quad has no centre: between its fronts.
        {6U, 2U, 4U, {h, h, 0.0F, 0.0F}},
        {6U, 3U, 4U, {0.0F, 0.0F, 0.0F, 0.0F}},
        // Mono takes every channel whole.
        {2U, 1U, 1U, {1.0F}},
        {6U, 2U, 1U, {1.0F}},
    };
    for (u32 r = 0U; r < NPH_ARRAY_COUNT(rows); ++r) {
        f32 g[NPH_AUDIO_MAX_CHANNELS];
        tm_channel_gains(rows[r].in_channels, rows[r].channel, rows[r].out_channels, 0.0F, g);
        for (u32 o = 0U; o < rows[r].out_channels; ++o) {
            if (fabsf(g[o] - rows[r].gains[o]) > 1e-6F) {
                NTEST_FAIL("row %u: channel %u of %u to output %u of %u: gain %.7f, expected %.7f",
                           r, rows[r].channel, rows[r].in_channels, o, rows[r].out_channels,
                           (f64)g[o], (f64)rows[r].gains[o]);
            }
        }
    }
    // Speakers the output lacks, among several: panned between the two that
    // bracket them, at unit power, mostly into the nearer. 5.1's surround
    // (-110 degrees) on quad lies between its front left (-45) and back left
    // (-135); 7.1's back left (-150) on 5.1 between the surrounds (-110 and
    // +110, behind).
    {
        f32 g[NPH_AUDIO_MAX_CHANNELS];
        tm_channel_gains(6U, 4U, 4U, 0.0F, g);
        NTEST_ASSERT_NEAR_F32(g[0] * g[0] + g[2] * g[2], 1.0F, 1e-5F);
        NTEST_ASSERT_TRUE(g[2] > g[0] && g[0] > 0.0F);
        NTEST_ASSERT_TRUE(g[1] == 0.0F && g[3] == 0.0F);
        tm_channel_gains(8U, 4U, 6U, 0.0F, g);
        NTEST_ASSERT_NEAR_F32(g[4] * g[4] + g[5] * g[5], 1.0F, 1e-5F);
        NTEST_ASSERT_TRUE(g[4] > g[5] && g[5] > 0.0F);
        for (u32 o = 0U; o < 4U; ++o) {
            NTEST_ASSERT_TRUE(g[o] == 0.0F);
        }
    }
    // The pan is a balance: it lowers the other side, linearly, and leaves
    // its own side and the centre whole.
    {
        f32 g[NPH_AUDIO_MAX_CHANNELS];
        tm_channel_gains(2U, 0U, 2U, 0.5F, g);
        NTEST_ASSERT_NEAR_F32(g[0], 0.5F, 1e-6F);
        tm_channel_gains(2U, 1U, 2U, 0.5F, g);
        NTEST_ASSERT_NEAR_F32(g[1], 1.0F, 1e-6F);
        tm_channel_gains(6U, 2U, 6U, -0.75F, g);
        NTEST_ASSERT_NEAR_F32(g[2], 1.0F, 1e-6F);
        tm_channel_gains(6U, 5U, 6U, -0.75F, g);
        NTEST_ASSERT_NEAR_F32(g[5], 0.25F, 1e-6F);
    }
}

NTEST(audio_mixer, pitch_and_clip_rates_play_at_the_right_speed)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    // A 1 kHz tone recorded at 24 kHz stays 1 kHz at a 48 kHz mix; at pitch
    // 2 a 48 kHz recording of 1 kHz plays at 2 kHz.
    const NphAudioClip slow = tm_tone(m, 1000.0, 24000U, 24000U, 0.5F, NULL);
    const NphAudioClip fast = tm_tone(m, 1000.0, 48000U, TM_RATE, 0.5F, NULL);
    NphAudioPlayDesc d = nph_audio_play_desc_default(slow);
    NphAudioVoice v = tm_play(m, &d);
    f32 *out = test_buffer(9600U);
    f32 *planes[1] = {out};
    tm_render(m, 9600U, planes);
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(out + 960, 8000U, 1000.0, 48000.0), 0.5, 1e-3);
    nph_audio_voice_stop(m, v, 0.0F);
    tm_render(m, 512U, planes);
    d = nph_audio_play_desc_default(fast);
    d.pitch = 2.0F;
    (void)tm_play(m, &d);
    tm_render(m, 9600U, planes);
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(out + 960, 8000U, 2000.0, 48000.0), 0.5, 1e-3);
    NTEST_ASSERT_TRUE(test_tone_amplitude(out + 960, 8000U, 1000.0, 48000.0) < 1e-3);
    test_buffer_free(out, 9600U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, delays_are_sample_accurate_and_starts_keep_their_attack)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    f32 *x = NULL;
    const NphAudioClip clip = tm_tone(m, 300.0, 4800U, TM_RATE, 0.9F, &x);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.delay_frames = 1000U;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(3000U);
    f32 *planes[1] = {out};
    tm_render(m, 3000U, planes);
    for (u32 i = 0U; i < 1000U; ++i) {
        NTEST_ASSERT_TRUE(out[i] == 0.0F);
    }
    // From the clip's first frame at full gain: the very first sample is
    // the clip's, unramped.
    for (u32 i = 0U; i < 2000U; ++i) {
        NTEST_ASSERT_TRUE(out[1000U + i] == x[i]);
    }
    test_buffer_free(out, 3000U);
    test_buffer_free(x, 4800U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, a_start_mid_clip_fades_in_over_a_block)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    f32 *dc = test_buffer(4000U);
    for (u32 i = 0U; i < 4000U; ++i) {
        dc[i] = 0.5F;
    }
    const NphAudioClip clip = tm_clip_f32(m, dc, 4000U, 1U, TM_RATE);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.start_frame = 1000U;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(512U);
    f32 *planes[1] = {out};
    tm_render(m, 512U, planes);
    NTEST_ASSERT_TRUE(out[0] == 0.0F);
    NTEST_ASSERT_NEAR_F32(out[128], 0.25F, 1e-6F); // halfway through the block's ramp
    for (u32 i = 1U; i < NPH_AUDIO_BLOCK_FRAMES; ++i) {
        NTEST_ASSERT_TRUE(out[i] > out[i - 1U]);
    }
    for (u32 i = NPH_AUDIO_BLOCK_FRAMES; i < 512U; ++i) {
        NTEST_ASSERT_NEAR_F32(out[i], 0.5F, 1e-6F);
    }
    test_buffer_free(out, 512U);
    test_buffer_free(dc, 4000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, loops_repeat_their_region_and_one_shots_end)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    f32 ramp[100];
    for (u32 i = 0U; i < 100U; ++i) {
        ramp[i] = (f32)i / 128.0F;
    }
    NphAudioClipDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.codec = NPH_AUDIO_CODEC_PCM;
    cd.format = NPH_AUDIO_FORMAT_F32;
    cd.channels = 1U;
    cd.sample_rate = TM_RATE;
    cd.frames = 100U;
    cd.data = ramp;
    cd.size = sizeof ramp;
    cd.loop_start = 20U;
    cd.loop_end = 80U;
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, &cd, &clip), NPH_RESULT_OK);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.loop = 1;
    const NphAudioVoice looping = tm_play(m, &d);
    f32 *out = test_buffer(2048U);
    f32 *planes[1] = {out};
    tm_render(m, 2048U, planes);
    // Frames 0 .. 79, then 20 .. 79 for ever.
    for (u32 i = 0U; i < 2048U; ++i) {
        const u32 f = i < 80U ? i : 20U + (i - 20U) % 60U;
        NTEST_ASSERT_TRUE(out[i] == ramp[f]);
    }
    NTEST_ASSERT_TRUE(nph_audio_voice_is_playing(m, looping));
    nph_audio_voice_stop(m, looping, 0.0F);
    // A one-shot plays its 100 frames, then ends, and its handle goes.
    d.loop = 0;
    const NphAudioVoice once = tm_play(m, &d);
    tm_render(m, 1024U, planes);
    nph_audio_mixer_update(m);
    NTEST_ASSERT_FALSE(nph_audio_voice_is_playing(m, once));
    NTEST_ASSERT_FALSE(nph_audio_voice_is_playing(m, looping));
    NphAudioMixerStats st;
    nph_audio_mixer_stats(m, &st);
    NTEST_ASSERT_EQ_UINT(st.voices_playing, 0U);
    test_buffer_free(out, 2048U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, stops_fade_and_end)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    f32 *dc = test_buffer(48000U);
    for (u32 i = 0U; i < 48000U; ++i) {
        dc[i] = 1.0F;
    }
    const NphAudioClip clip = tm_clip_f32(m, dc, 48000U, 1U, TM_RATE);
    const NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    const NphAudioVoice v = tm_play(m, &d);
    f32 *out = test_buffer(8192U);
    f32 *planes[1] = {out};
    tm_render(m, 512U, planes);
    nph_audio_voice_stop(m, v, 0.05F); // 2400 frames
    tm_render(m, 4096U, planes);
    // A linear fade, ramped a block at a time: exact at block boundaries,
    // and the last block (where it reaches zero) ends it.
    NTEST_ASSERT_NEAR_F32(out[0], 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(out[1200], 0.5F, 1e-3F); // halfway down
    NTEST_ASSERT_TRUE(out[2400] < 0.03F);
    for (u32 i = 1U; i < 2560U; ++i) {
        NTEST_ASSERT_TRUE(out[i] <= out[i - 1U]); // monotonic: no click
    }
    for (u32 i = 2560U; i < 4096U; ++i) {
        NTEST_ASSERT_TRUE(out[i] == 0.0F);
    }
    NTEST_ASSERT_TRUE(nph_audio_voice_is_playing(m, v)); // until the update hears it
    nph_audio_mixer_update(m);
    NTEST_ASSERT_FALSE(nph_audio_voice_is_playing(m, v));
    // Stopping a stale handle does nothing.
    nph_audio_voice_stop(m, v, 0.0F);
    test_buffer_free(out, 8192U);
    test_buffer_free(dc, 48000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, buses_scale_mute_and_send)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    f32 *dc = test_buffer(48000U);
    for (u32 i = 0U; i < 48000U; ++i) {
        dc[i] = 0.5F;
    }
    const NphAudioClip clip = tm_clip_f32(m, dc, 48000U, 1U, TM_RATE);
    NphAudioBusDesc bd = nph_audio_bus_desc_default();
    bd.volume = 0.5F;
    bd.name = "effects";
    NphAudioBus effects;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &effects), NPH_RESULT_OK);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.bus = effects;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(1024U);
    f32 *planes[1] = {out};
    tm_render(m, 1024U, planes);
    NTEST_ASSERT_NEAR_F32(out[1000], 0.25F, 1e-6F);
    // A send to a second bus adds its share there.
    NphAudioBus reverb;
    bd.volume = 1.0F;
    bd.name = "reverb";
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &reverb), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_set_send(m, effects, 0U, reverb, 0.5F), NPH_RESULT_OK);
    tm_render(m, 1024U, planes);
    NTEST_ASSERT_NEAR_F32(out[1000], 0.25F + 0.125F, 1e-6F);
    // Sends may not make cycles, or leave the master.
    NTEST_ASSERT_EQ_INT(nph_audio_bus_set_send(m, reverb, 0U, effects, 1.0F),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_set_send(m, effects, 1U, effects, 1.0F),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_set_send(m, NPH_AUDIO_MASTER_BUS, 0U, reverb, 1.0F),
                        NPH_RESULT_INVALID_ARGUMENT);
    // Muting fades over a block, then silence.
    nph_audio_bus_set_mute(m, effects, true);
    tm_render(m, 1024U, planes);
    NTEST_ASSERT_TRUE(out[0] > 0.3F);
    for (u32 i = NPH_AUDIO_BLOCK_FRAMES; i < 1024U; ++i) {
        NTEST_ASSERT_TRUE(out[i] == 0.0F);
    }
    // The meters saw it.
    f32 peak = 1.0F;
    f32 rms = 1.0F;
    nph_audio_bus_meter(m, effects, &peak, &rms);
    NTEST_ASSERT_TRUE(peak == 0.0F && rms == 0.0F);
    nph_audio_bus_set_mute(m, effects, false);
    tm_render(m, 1024U, planes);
    nph_audio_bus_meter(m, effects, &peak, &rms);
    NTEST_ASSERT_NEAR_F32(peak, 0.25F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(rms, 0.25F, 1e-5F);
    // Destroying a bus moves its voices to its parent.
    nph_audio_bus_destroy(m, effects);
    NTEST_ASSERT_FALSE(nph_audio_bus_is_valid(m, effects));
    tm_render(m, 1024U, planes);
    NTEST_ASSERT_NEAR_F32(out[1000], 0.5F, 1e-6F);
    nph_audio_mixer_update(m);
    test_buffer_free(out, 1024U);
    test_buffer_free(dc, 48000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, virtual_voices_come_back_where_they_would_be)
{
    // One real voice: the louder plays, the quieter waits in silence; when
    // the first stops the second fades in at its own place in its clip.
    NphAudioMixer *m = tm_mixer(1U, 16U, 1U);
    f32 *a = NULL;
    f32 *b = NULL;
    const NphAudioClip ca = tm_tone(m, 500.0, 48000U, TM_RATE, 0.8F, &a);
    const NphAudioClip cb = tm_tone(m, 700.0, 48000U, TM_RATE, 0.3F, &b);
    NphAudioPlayDesc da = nph_audio_play_desc_default(ca);
    NphAudioPlayDesc db = nph_audio_play_desc_default(cb);
    db.volume = 0.5F;
    const NphAudioVoice va = tm_play(m, &da);
    (void)tm_play(m, &db);
    f32 *out = test_buffer(48000U);
    f32 *planes[1] = {out};
    tm_render(m, 4096U, planes);
    NphAudioMixerStats st;
    nph_audio_mixer_stats(m, &st);
    NTEST_ASSERT_EQ_UINT(st.voices_real, 1U);
    NTEST_ASSERT_EQ_UINT(st.voices_virtual, 1U);
    for (u32 i = 0U; i < 4096U; ++i) {
        NTEST_ASSERT_TRUE(out[i] == a[i]); // only a
    }
    nph_audio_voice_stop(m, va, 0.0F);
    tm_render(m, 4096U, planes); // a fades over a block, b fades in
    // After both fades, b exactly as if it had played all along.
    for (u32 i = 2U * NPH_AUDIO_BLOCK_FRAMES; i < 4096U; ++i) {
        NTEST_ASSERT_NEAR_F32(out[i], b[4096U + i] * 0.5F, 1e-6F);
    }
    test_buffer_free(out, 48000U);
    test_buffer_free(a, 48000U);
    test_buffer_free(b, 48000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, priority_outranks_loudness)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 1U);
    const NphAudioClip loud = tm_tone(m, 500.0, 48000U, TM_RATE, 0.9F, NULL);
    f32 *q = NULL;
    const NphAudioClip quiet = tm_tone(m, 900.0, 48000U, TM_RATE, 0.1F, &q);
    NphAudioPlayDesc d = nph_audio_play_desc_default(loud);
    d.priority = 10U;
    (void)tm_play(m, &d);
    d = nph_audio_play_desc_default(quiet);
    d.priority = 250U;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(1024U);
    f32 *planes[1] = {out};
    tm_render(m, 1024U, planes);
    for (u32 i = 0U; i < 1024U; ++i) {
        NTEST_ASSERT_TRUE(out[i] == q[i]);
    }
    test_buffer_free(out, 1024U);
    test_buffer_free(q, 48000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, ducking_lowers_a_bus_while_another_sounds)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    f32 *dc = test_buffer(96000U);
    for (u32 i = 0U; i < 96000U; ++i) {
        dc[i] = 0.25F;
    }
    const NphAudioClip clip = tm_clip_f32(m, dc, 96000U, 1U, TM_RATE);
    NphAudioBusDesc bd = nph_audio_bus_desc_default();
    NphAudioBus music;
    NphAudioBus dialogue;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &music), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &dialogue), NPH_RESULT_OK);
    nph_audio_bus_set_duck(m, music, dialogue, -12.0F, -40.0F, 0.01F, 0.1F);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.bus = music;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(48000U);
    f32 *planes[1] = {out};
    tm_render(m, 4800U, planes);
    f32 peak = 0.0F;
    nph_audio_bus_meter(m, music, &peak, NULL);
    NTEST_ASSERT_NEAR_F32(peak, 0.25F, 1e-6F); // nothing to duck under
    d.bus = dialogue;
    const NphAudioVoice line = tm_play(m, &d);
    tm_render(m, 9600U, planes); // 0.2 s: twenty attack times
    nph_audio_bus_meter(m, music, &peak, NULL);
    NTEST_ASSERT_NEAR_F64(test_db((f64)peak / 0.25), -12.0, 0.05);
    nph_audio_voice_stop(m, line, 0.0F);
    tm_render(m, 48000U, planes); // a second: ten release times
    nph_audio_bus_meter(m, music, &peak, NULL);
    NTEST_ASSERT_NEAR_F64(test_db((f64)peak / 0.25), 0.0, 0.01);
    test_buffer_free(out, 48000U);
    test_buffer_free(dc, 96000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, snapshots_fade_their_bus_changes_in_and_out)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 8U);
    f32 *x = NULL;
    const NphAudioClip clip = tm_tone(m, 3000.0, 96000U, TM_RATE, 0.5F, &x);
    NphAudioBusDesc bd = nph_audio_bus_desc_default();
    NphAudioBus effects;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &effects), NPH_RESULT_OK);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.bus = effects;
    (void)tm_play(m, &d);
    const NphAudioSnapshotEntry entries[1] = {{effects, -6.0F, 0.0F}};
    NphAudioSnapshot pause;
    NTEST_ASSERT_EQ_INT(nph_audio_snapshot_create(m, entries, 1U, &pause), NPH_RESULT_OK);
    f32 *out = test_buffer(24000U);
    f32 *planes[1] = {out};
    nph_audio_snapshot_set_weight(m, pause, 1.0F, 0.1F);
    tm_render(m, 24000U, planes); // half a second: the fade is done
    NTEST_ASSERT_NEAR_F64(test_db(test_tone_amplitude(out + 12000, 12000U, 3000.0, 48000.0) / 0.5),
                          -6.0, 0.01);
    // A low-pass snapshot darkens: 3 kHz through a 500 Hz corner.
    const NphAudioSnapshotEntry dark[1] = {{effects, 0.0F, 500.0F}};
    NphAudioSnapshot muffle;
    NTEST_ASSERT_EQ_INT(nph_audio_snapshot_create(m, dark, 1U, &muffle), NPH_RESULT_OK);
    nph_audio_snapshot_set_weight(m, pause, 0.0F, 0.0F);
    nph_audio_snapshot_set_weight(m, muffle, 1.0F, 0.0F);
    tm_render(m, 24000U, planes);
    const f64 level = test_db(test_tone_amplitude(out + 12000, 12000U, 3000.0, 48000.0) / 0.5);
    NTEST_LOG("3 kHz through the 500 Hz snapshot: %.1f dB", level);
    NTEST_ASSERT_TRUE(level < -28.0);
    nph_audio_snapshot_set_weight(m, muffle, 0.0F, 0.05F);
    tm_render(m, 24000U, planes);
    NTEST_ASSERT_NEAR_F64(test_db(test_tone_amplitude(out + 12000, 12000U, 3000.0, 48000.0) / 0.5),
                          0.0, 0.01);
    // What snapshots refuse.
    NphAudioSnapshot bad;
    const NphAudioSnapshotEntry stale[1] = {{{0xFFFFFFU}, 0.0F, 0.0F}};
    NTEST_ASSERT_EQ_INT(nph_audio_snapshot_create(m, stale, 1U, &bad), NPH_RESULT_INVALID_ARGUMENT);
    test_buffer_free(out, 24000U);
    test_buffer_free(x, 96000U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, filters_switch_on_and_off_without_a_click)
{
    // A steady 200 Hz tone, whose own bend is 0.5 (2 pi 200 / 48000)^2 =
    // 3.4e-4. A filter that started from silence while the tone played
    // would step by a fraction of the tone's amplitude, a bend a thousand
    // times that; a corner that stepped once a block, without gliding,
    // ten times (0.003 here, from the phase response's jumps).
    enum {
        BLOCKS = 48
    };
    const f64 own = 0.5 * pow(2.0 * TEST_PI * 200.0 / 48000.0, 2.0);
    for (u32 where = 0U; where < 2U; ++where) {
        NphAudioMixer *m = tm_mixer(1U, 4U, 4U);
        const NphAudioClip clip = tm_tone(m, 200.0, 96000U, TM_RATE, 0.5F, NULL);
        NphAudioBusDesc bd = nph_audio_bus_desc_default();
        NphAudioBus bus;
        NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &bus), NPH_RESULT_OK);
        const NphAudioSnapshotEntry dark[1] = {{bus, 0.0F, 3000.0F}};
        NphAudioSnapshot snapshot;
        NTEST_ASSERT_EQ_INT(nph_audio_snapshot_create(m, dark, 1U, &snapshot), NPH_RESULT_OK);
        NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
        d.bus = bus;
        const NphAudioVoice v = tm_play(m, &d);
        f32 *out = test_buffer(BLOCKS * NPH_AUDIO_BLOCK_FRAMES);
        for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
            if (where == 0U && b == 6U) {
                nph_audio_voice_set_filters(m, v, 6000.0F, 80.0F); // the voice's filters on
            }
            if (where == 0U && b == 30U) {
                nph_audio_voice_set_filters(m, v, 0.0F, 0.0F); // and off
            }
            if (where == 1U && b == 6U) {
                nph_audio_snapshot_set_weight(m, snapshot, 1.0F, 0.02F); // the bus's low-pass on
            }
            if (where == 1U && b == 30U) {
                nph_audio_snapshot_set_weight(m, snapshot, 0.0F, 0.02F); // and off
            }
            nph_audio_mixer_render(m, out + (u64)b * NPH_AUDIO_BLOCK_FRAMES,
                                   NPH_AUDIO_BLOCK_FRAMES);
        }
        const f64 bend = test_largest_bend(out, BLOCKS * NPH_AUDIO_BLOCK_FRAMES);
        NTEST_LOG("%s filters: largest bend %.3g (the tone's own %.3g)",
                  where == 0U ? "voice" : "bus", bend, own);
        NTEST_ASSERT_TRUE(bend < 1.5 * own);
        // And the filters did their work: the tone is through them
        // unchanged in level (both corners far from 200 Hz).
        NTEST_ASSERT_NEAR_F64(
            test_tone_amplitude(out + (u64)20U * NPH_AUDIO_BLOCK_FRAMES, 2400U, 200.0, 48000.0),
            0.5, 0.02);
        test_buffer_free(out, BLOCKS * NPH_AUDIO_BLOCK_FRAMES);
        nph_audio_mixer_destroy(m);
    }
}

// The same signal as PCM 16-bit, lossless (small blocks, so loops and
// starts cross them), and ADPCM, each played the same way in a mixer of its
// own.
enum {
    CODEC_FRAMES = 9000,
    CODEC_RENDER = 30000
};

static void render_codec(const NphAudioClipDesc *cd, f32 **out)
{
    NphAudioMixer *m = tm_mixer(2U, 16U, 8U);
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, cd, &clip), NPH_RESULT_OK);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.start_frame = 500U;
    d.loop = 1;
    d.pitch = 1.01F; // so it resamples
    (void)tm_play(m, &d);
    tm_render(m, CODEC_RENDER, out);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, every_codec_plays_its_clip)
{
    i16 *pcm = (i16 *)NPH_ALLOC(nph_allocator_system(), (u64)CODEC_FRAMES * 2U * sizeof(i16), 16U);
    i32 *ints = (i32 *)NPH_ALLOC(nph_allocator_system(), (u64)CODEC_FRAMES * 2U * sizeof(i32), 16U);
    NTEST_ASSERT_NOT_NULL(pcm);
    NTEST_ASSERT_NOT_NULL(ints);
    for (u32 i = 0U; i < (u32)CODEC_FRAMES; ++i) {
        const f64 t = (f64)i / 48000.0;
        pcm[(u64)i * 2U] = (i16)(12000.0 * sin(2.0 * TEST_PI * 440.0 * t));
        pcm[(u64)i * 2U + 1U] = (i16)(9000.0 * sin(2.0 * TEST_PI * 660.0 * t + 0.4));
        ints[(u64)i * 2U] = pcm[(u64)i * 2U];
        ints[(u64)i * 2U + 1U] = pcm[(u64)i * 2U + 1U];
    }
    NphAudioClipDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.codec = NPH_AUDIO_CODEC_PCM;
    cd.format = NPH_AUDIO_FORMAT_S16;
    cd.channels = 2U;
    cd.sample_rate = TM_RATE;
    cd.frames = CODEC_FRAMES;
    cd.data = pcm;
    cd.size = (u64)CODEC_FRAMES * 2U * sizeof(i16);
    cd.loop_start = 1234U;
    cd.loop_end = 7777U;
    f32 *renders[3][2];
    for (u32 k = 0U; k < 3U; ++k) {
        renders[k][0] = test_buffer(CODEC_RENDER);
        renders[k][1] = test_buffer(CODEC_RENDER);
    }
    render_codec(&cd, renders[0]);
    const NphLosslessParams lp = {300U, 0U, 0};
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(
        nph_lossless_encode(nph_allocator_system(), ints, 2U, 16U, CODEC_FRAMES, &lp, &enc),
        NPH_RESULT_OK);
    NphAudioClipDesc ld = cd;
    ld.codec = NPH_AUDIO_CODEC_LOSSLESS;
    ld.data = enc.stream.data;
    ld.size = enc.stream.size;
    ld.bits = 16U;
    ld.block_frames = enc.stream.block_frames;
    ld.block_offsets = enc.stream.block_offsets;
    ld.block_count = enc.stream.block_count;
    render_codec(&ld, renders[1]);
    nph_lossless_encoded_free(&enc);
    NphAdpcmEncoded aenc;
    NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), pcm, 2U, CODEC_FRAMES, &aenc),
                        NPH_RESULT_OK);
    NphAudioClipDesc ad = cd;
    ad.codec = NPH_AUDIO_CODEC_ADPCM;
    ad.data = aenc.stream.data;
    ad.size = aenc.stream.size;
    render_codec(&ad, renders[2]);
    nph_adpcm_encoded_free(&aenc);
    // Lossless is the PCM, exactly; ADPCM close to it.
    for (u32 c = 0U; c < 2U; ++c) {
        NTEST_ASSERT_TRUE(test_same_bits(renders[1][c], renders[0][c], CODEC_RENDER));
        f64 signal = 0.0;
        f64 noise = 0.0;
        for (u32 i = 0U; i < (u32)CODEC_RENDER; ++i) {
            signal += (f64)renders[0][c][i] * (f64)renders[0][c][i];
            const f64 e = (f64)renders[2][c][i] - (f64)renders[0][c][i];
            noise += e * e;
        }
        const f64 snr = 10.0 * log10(signal / noise);
        NTEST_LOG("ADPCM channel %u through the mixer: SNR %.1f dB", c, snr);
        NTEST_ASSERT_TRUE(snr > 40.0);
    }
    for (u32 k = 0U; k < 3U; ++k) {
        test_buffer_free(renders[k][0], CODEC_RENDER);
        test_buffer_free(renders[k][1], CODEC_RENDER);
    }
    NPH_FREE(nph_allocator_system(), pcm, (u64)CODEC_FRAMES * 2U * sizeof(i16), 16U);
    NPH_FREE(nph_allocator_system(), ints, (u64)CODEC_FRAMES * 2U * sizeof(i32), 16U);
}

NTEST(audio_mixer, destroying_a_clip_stops_its_voices_then_frees_it)
{
    NphAudioMixer *m = tm_mixer(1U, 16U, 2U);
    const NphAudioClip clip = tm_tone(m, 440.0, 48000U, TM_RATE, 0.5F, NULL);
    NphAudioVoice voices[4];
    for (u32 i = 0U; i < 4U; ++i) {
        const NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
        voices[i] = tm_play(m, &d); // two real, two virtual
    }
    f32 *out = test_buffer(1024U);
    f32 *planes[1] = {out};
    tm_render(m, 1024U, planes);
    nph_audio_clip_destroy(m, clip);
    NTEST_ASSERT_FALSE(nph_audio_clip_is_valid(m, clip)); // stale at once
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    NphAudioVoice refused;
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &d, &refused), NPH_RESULT_INVALID_ARGUMENT);
    tm_render(m, 1024U, planes);
    // Faded within a block, then silent.
    for (u32 i = NPH_AUDIO_BLOCK_FRAMES; i < 1024U; ++i) {
        NTEST_ASSERT_TRUE(out[i] == 0.0F);
    }
    nph_audio_mixer_update(m); // the voices' ends and the clip's release
    for (u32 i = 0U; i < 4U; ++i) {
        NTEST_ASSERT_FALSE(nph_audio_voice_is_playing(m, voices[i]));
    }
    NTEST_ASSERT_TRUE(nph_audio_clip_duration(m, clip) == 0.0);
    test_buffer_free(out, 1024U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, bad_clips_and_voices_are_refused)
{
    NphAudioMixer *m = tm_mixer(1U, 2U, 2U);
    f32 x[64] = {0.0F};
    NphAudioClipDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.codec = NPH_AUDIO_CODEC_PCM;
    cd.format = NPH_AUDIO_FORMAT_F32;
    cd.channels = 1U;
    cd.sample_rate = TM_RATE;
    cd.frames = 64U;
    cd.data = x;
    cd.size = sizeof x;
    NphAudioClip clip;
    NphAudioClipDesc bad = cd;
    bad.size = 10U;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, &bad, &clip), NPH_RESULT_INVALID_ARGUMENT);
    bad = cd;
    bad.loop_start = 50U;
    bad.loop_end = 40U;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, &bad, &clip), NPH_RESULT_INVALID_ARGUMENT);
    bad = cd;
    bad.codec = NPH_AUDIO_CODEC_LOSSLESS; // no valid stream
    NTEST_ASSERT_TRUE(nph_audio_clip_create(m, &bad, &clip) != NPH_RESULT_OK);
    bad = cd;
    bad.sample_rate = 100U;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, &bad, &clip), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, &cd, &clip), NPH_RESULT_OK);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.start_frame = 64U;
    NphAudioVoice v;
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &d, &v), NPH_RESULT_INVALID_ARGUMENT);
    d.start_frame = 0U;
    d.bus.value = 0x123U;
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &d, &v), NPH_RESULT_INVALID_ARGUMENT);
    d.bus = NPH_AUDIO_MASTER_BUS;
    // Two voice slots: a third is refused.
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &d, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &d, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &d, &v), NPH_RESULT_FULL);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, a_full_command_ring_refuses_and_counts)
{
    NphAudioMixerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.channels = 1U;
    d.command_capacity = 8U;
    NphAudioMixer *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_OK);
    const NphAudioClip clip = tm_tone(m, 440.0, 4800U, TM_RATE, 0.5F, NULL);
    const NphAudioPlayDesc pd = nph_audio_play_desc_default(clip);
    NphAudioVoice v;
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &pd, &v), NPH_RESULT_OK);
    for (u32 i = 0U; i < 20U; ++i) {
        nph_audio_voice_set_volume(m, v, 0.5F);
    }
    NphAudioMixerStats st;
    nph_audio_mixer_stats(m, &st);
    NTEST_ASSERT_GT_UINT(st.commands_dropped, 0U);
    NphAudioVoice w;
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &pd, &w), NPH_RESULT_FULL);
    // A render drains the ring, and plays resume.
    f32 out[512];
    nph_audio_mixer_render(m, out, 512U);
    NTEST_ASSERT_EQ_INT(nph_audio_voice_play(m, &pd, &w), NPH_RESULT_OK);
    nph_audio_mixer_destroy(m);
}

// Renders a fixed scene; with `chunks`, in pieces of varying size.
static void render_scene(f32 *out, u32 frames, bool chunks)
{
    NphAudioMixer *m = tm_mixer(2U, 32U, 8U);
    const NphAudioClip a = tm_tone(m, 220.0, 48000U, TM_RATE, 0.4F, NULL);
    const NphAudioClip b = tm_tone(m, 330.0, 32000U, 32000U, 0.4F, NULL);
    NphAudioBusDesc bd = nph_audio_bus_desc_default();
    NphAudioBus bus;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &bus), NPH_RESULT_OK);
    for (u32 i = 0U; i < 12U; ++i) {
        NphAudioPlayDesc d = nph_audio_play_desc_default((i & 1U) != 0U ? a : b);
        d.pan = -1.0F + 0.18F * (f32)i;
        d.pitch = 0.8F + 0.05F * (f32)i;
        d.volume = 0.2F + 0.05F * (f32)i;
        d.delay_frames = 37U * i;
        d.loop = (i % 3U) == 0U;
        d.lowpass_hz = (i % 4U) == 1U ? 2000.0F : 0.0F;
        d.bus = (i % 2U) == 0U ? bus : NPH_AUDIO_MASTER_BUS;
        (void)tm_play(m, &d);
    }
    u32 done = 0U;
    u32 piece = 1U;
    while (done < frames) {
        const u32 n = chunks ? nph_min_u32(piece, frames - done) : frames - done;
        nph_audio_mixer_render(m, out + (u64)done * 2U, n);
        done += n;
        piece = (piece * 7U + 13U) % 700U + 1U;
    }
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer, rendering_is_deterministic_in_any_pieces)
{
    enum {
        FRAMES = 24000
    };
    f32 *a = test_buffer(FRAMES * 2U);
    f32 *b = test_buffer(FRAMES * 2U);
    f32 *c = test_buffer(FRAMES * 2U);
    render_scene(a, FRAMES, false);
    render_scene(b, FRAMES, false);
    render_scene(c, FRAMES, true);
    NTEST_ASSERT_TRUE(test_same_bits(a, b, FRAMES * 2U));
    NTEST_ASSERT_TRUE(test_same_bits(a, c, FRAMES * 2U));
    f64 energy = 0.0;
    for (u32 i = 0U; i < (u32)FRAMES * 2U; ++i) {
        energy += (f64)a[i] * (f64)a[i];
    }
    NTEST_ASSERT_TRUE(energy > 10.0); // the scene is not silence
    test_buffer_free(a, FRAMES * 2U);
    test_buffer_free(b, FRAMES * 2U);
    test_buffer_free(c, FRAMES * 2U);
}
