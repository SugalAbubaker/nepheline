// SPDX-License-Identifier: Apache-2.0
// The mixer's 3D voices (mixer.h, spatial.h): distance and direction,
// Doppler, the air, occlusion and obstruction (from a voice's first block),
// smooth motion, distance in the ranking, and a stereo clip's width.

#include <nepheline/audio/pan.h>

#include "test_mixer_support.h"

// A spatial play desc of `clip` at `pos`, the air and Doppler off unless a
// test wants them.
static NphAudioPlayDesc tms_desc(NphAudioClip clip, NphVec3 pos)
{
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.spatial = 1;
    d.position = pos;
    d.doppler = 0.0F;
    d.air_absorption = 0.0F;
    return d;
}

NTEST(audio_mixer_spatial, distance_and_direction)
{
    // 2 m to the right through the physical law (full within 1 m): half
    // the level, all on the right. 4 m ahead: a quarter, centred.
    enum {
        FRAMES = 9600
    };
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 500.0, 48000U, TM_RATE, 0.5F, NULL);
    NphAudioPlayDesc d = tms_desc(clip, (NphVec3){2.0F, 0.0F, 0.0F});
    const NphAudioVoice v = tm_play(m, &d);
    f32 *l = test_buffer(FRAMES);
    f32 *r = test_buffer(FRAMES);
    f32 *planes[2] = {l, r};
    tm_render(m, FRAMES, planes);
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(r, FRAMES, 500.0, 48000.0), 0.25, 1e-5);
    NTEST_ASSERT_NEAR_F64(test_rms(l, FRAMES), 0.0, 1e-6);
    nph_audio_voice_set_position(m, v, (NphVec3){0.0F, 0.0F, -4.0F}, (NphVec3){0.0F, 0.0F, 0.0F});
    tm_render(m, FRAMES, planes);
    const f64 centred = 0.125 * 0.70710678;
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(l + 1024, FRAMES - 1024U, 500.0, 48000.0), centred,
                          1e-5);
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(r + 1024, FRAMES - 1024U, 500.0, 48000.0), centred,
                          1e-5);
    test_buffer_free(l, FRAMES);
    test_buffer_free(r, FRAMES);
    nph_audio_mixer_destroy(m);
}

// The frequency of x from its upward zero crossings.
static f64 tms_frequency(const f32 *x, u32 count)
{
    u32 first = 0U;
    u32 last = 0U;
    u32 crossings = 0U;
    for (u32 i = 1U; i < count; ++i) {
        if (x[i - 1U] < 0.0F && x[i] >= 0.0F) {
            if (crossings == 0U) {
                first = i;
            }
            last = i;
            crossings += 1U;
        }
    }
    return crossings > 1U ? (f64)(crossings - 1U) * 48000.0 / (f64)(last - first) : 0.0;
}

NTEST(audio_mixer_spatial, doppler_moves_the_pitch)
{
    // 10 m ahead, receding at a tenth of the speed of sound: 1 kHz heard at
    // 1/1.1 of itself; approaching, at 1/0.9.
    enum {
        FRAMES = 24000
    };
    NphAudioMixer *m = tm_mixer(1U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 1000.0, 96000U, TM_RATE, 0.5F, NULL);
    NphAudioPlayDesc d = tms_desc(clip, (NphVec3){0.0F, 0.0F, -10.0F});
    d.doppler = 1.0F;
    d.velocity = (NphVec3){0.0F, 0.0F, -0.1F * NPH_AUDIO_SPEED_OF_SOUND};
    const NphAudioVoice v = tm_play(m, &d);
    f32 *x = test_buffer(FRAMES);
    f32 *planes[1] = {x};
    tm_render(m, FRAMES, planes);
    const f64 away = tms_frequency(x, FRAMES);
    nph_audio_voice_set_position(m, v, (NphVec3){0.0F, 0.0F, -10.0F},
                                 (NphVec3){0.0F, 0.0F, 0.1F * NPH_AUDIO_SPEED_OF_SOUND});
    tm_render(m, FRAMES, planes);
    const f64 towards = tms_frequency(x + 512, FRAMES - 512U);
    NTEST_LOG("1 kHz receding: %.2f Hz, approaching: %.2f Hz", away, towards);
    NTEST_ASSERT_NEAR_F64(away, 1000.0 / 1.1, 0.5);
    NTEST_ASSERT_NEAR_F64(towards, 1000.0 / 0.9, 0.5);
    test_buffer_free(x, FRAMES);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_spatial, the_air_darkens_the_distance)
{
    // 100 m away with no attenuation curve: 8 kHz loses ISO 9613-1's
    // 7.66 dB, 500 Hz nearly nothing.
    enum {
        FRAMES = 24000
    };
    const f64 freqs[2] = {8000.0, 500.0};
    f64 loss[2];
    for (u32 k = 0U; k < 2U; ++k) {
        NphAudioMixer *m = tm_mixer(1U, 4U, 4U);
        const NphAudioClip clip = tm_tone(m, freqs[k], 48000U, TM_RATE, 0.5F, NULL);
        NphAudioPlayDesc d = tms_desc(clip, (NphVec3){0.0F, 0.0F, -100.0F});
        d.attenuation.rolloff = NPH_AUDIO_ROLLOFF_NONE;
        d.air_absorption = 1.0F;
        (void)tm_play(m, &d);
        f32 *x = test_buffer(FRAMES);
        f32 *planes[1] = {x};
        tm_render(m, FRAMES, planes);
        loss[k] = -test_db(test_tone_amplitude(x + 4800, FRAMES - 4800U, freqs[k], 48000.0) / 0.5);
        test_buffer_free(x, FRAMES);
        nph_audio_mixer_destroy(m);
    }
    NTEST_LOG("over 100 m: 8 kHz loses %.2f dB, 500 Hz %.2f dB", loss[0], loss[1]);
    NTEST_ASSERT_NEAR_F64(loss[0], 7.66, 0.01);
    NTEST_ASSERT_TRUE(loss[1] < 0.2);
}

NTEST(audio_mixer_spatial, occlusion_and_obstruction)
{
    // A tone 1 m away on a "direct" bus, sending to a "room" bus.
    // Obstruction takes 12 dB from the direct path and nothing from the
    // send; occlusion 24 dB from both. The bus meters measure a block, so
    // the tone has a cycle a block (187.5 Hz): every block's RMS is alike.
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 187.5, 96000U, TM_RATE, 0.5F, NULL);
    const NphAudioBusDesc bd = nph_audio_bus_desc_default();
    NphAudioBus direct;
    NphAudioBus room;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &direct), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &room), NPH_RESULT_OK);
    NphAudioPlayDesc d = tms_desc(clip, (NphVec3){0.0F, 0.0F, -1.0F});
    d.bus = direct;
    d.sends[0] = room;
    d.send_gains[0] = 1.0F;
    const NphAudioVoice v = tm_play(m, &d);
    f32 out[2U * 256U];
    f32 base_direct = 0.0F;
    f32 base_room = 0.0F;
    f32 level_direct = 0.0F;
    f32 level_room = 0.0F;
    for (u32 b = 0U; b < 20U; ++b) {
        nph_audio_mixer_render(m, out, 256U);
    }
    nph_audio_bus_meter(m, direct, NULL, &base_direct);
    nph_audio_bus_meter(m, room, NULL, &base_room);
    nph_audio_voice_set_occlusion(m, v, 0.0F, 1.0F);
    for (u32 b = 0U; b < 40U; ++b) {
        nph_audio_mixer_render(m, out, 256U);
    }
    nph_audio_bus_meter(m, direct, NULL, &level_direct);
    nph_audio_bus_meter(m, room, NULL, &level_room);
    const f64 obstructed_direct = test_db((f64)level_direct / (f64)base_direct);
    const f64 obstructed_room = test_db((f64)level_room / (f64)base_room);
    nph_audio_voice_set_occlusion(m, v, 1.0F, 0.0F);
    for (u32 b = 0U; b < 40U; ++b) {
        nph_audio_mixer_render(m, out, 256U);
    }
    nph_audio_bus_meter(m, direct, NULL, &level_direct);
    nph_audio_bus_meter(m, room, NULL, &level_room);
    const f64 occluded_direct = test_db((f64)level_direct / (f64)base_direct);
    const f64 occluded_room = test_db((f64)level_room / (f64)base_room);
    NTEST_LOG("obstructed: direct %.2f dB, room %.2f dB; occluded: direct %.2f dB, room %.2f dB",
              obstructed_direct, obstructed_room, occluded_direct, occluded_room);
    // The low-passes cost the tone a few hundredths too.
    NTEST_ASSERT_NEAR_F64(obstructed_direct, -12.0, 0.15);
    NTEST_ASSERT_NEAR_F64(obstructed_room, 0.0, 0.01);
    NTEST_ASSERT_NEAR_F64(occluded_direct, -24.0, 0.25);
    NTEST_ASSERT_NEAR_F64(occluded_room, -24.0, 0.25);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_spatial, a_voice_starts_as_occluded_as_it_is_played)
{
    // The same tone 1 m away twice, on buses of their own, the second played
    // occluded: dark from its first block, as dark as once it has settled
    // (a sound that begins behind a wall never starts bright). A cycle a
    // block, as above.
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 187.5, 96000U, TM_RATE, 0.5F, NULL);
    const NphAudioBusDesc bd = nph_audio_bus_desc_default();
    NphAudioBus open;
    NphAudioBus shut;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &open), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &shut), NPH_RESULT_OK);
    NphAudioPlayDesc d = tms_desc(clip, (NphVec3){0.0F, 0.0F, -1.0F});
    d.bus = open;
    (void)tm_play(m, &d);
    d.bus = shut;
    d.occlusion = 1.0F;
    (void)tm_play(m, &d);
    f32 out[2U * 256U];
    nph_audio_mixer_render(m, out, 256U);
    f32 open_first = 0.0F;
    f32 shut_first = 0.0F;
    nph_audio_bus_meter(m, open, NULL, &open_first);
    nph_audio_bus_meter(m, shut, NULL, &shut_first);
    for (u32 b = 0U; b < 40U; ++b) {
        nph_audio_mixer_render(m, out, 256U);
    }
    f32 open_late = 0.0F;
    f32 shut_late = 0.0F;
    nph_audio_bus_meter(m, open, NULL, &open_late);
    nph_audio_bus_meter(m, shut, NULL, &shut_late);
    const f64 first = test_db((f64)shut_first / (f64)open_first);
    const f64 late = test_db((f64)shut_late / (f64)open_late);
    NTEST_LOG("played occluded: %.2f dB in its first block, %.2f dB settled", first, late);
    NTEST_ASSERT_NEAR_F64(late, -24.0, 0.25);
    NTEST_ASSERT_NEAR_F64(first, late, 0.5);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_spatial, a_circling_source_does_not_click)
{
    // A 300 Hz tone 3 m away, going round the listener once a second, its
    // position set every block; occlusion and distance changing too.
    enum {
        BLOCKS = 200
    };
    const u32 frames = BLOCKS * NPH_AUDIO_BLOCK_FRAMES;
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 300.0, 96000U, TM_RATE, 0.5F, NULL);
    NphAudioPlayDesc d = tms_desc(clip, (NphVec3){0.0F, 0.0F, -3.0F});
    d.air_absorption = 1.0F;
    d.size = 1.0F;
    const NphAudioVoice v = tm_play(m, &d);
    f32 *out = test_buffer(frames * 2U);
    for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
        const f32 a = 2.0F * (f32)TEST_PI * (f32)(b * NPH_AUDIO_BLOCK_FRAMES) / 48000.0F;
        const f32 radius = 3.0F + sinf(0.5F * a);
        nph_audio_voice_set_position(m, v, (NphVec3){radius * sinf(a), 0.0F, -radius * cosf(a)},
                                     (NphVec3){0.0F, 0.0F, 0.0F});
        nph_audio_voice_set_occlusion(m, v, 0.5F + 0.5F * sinf(a), 0.0F);
        nph_audio_mixer_render(m, out + (u64)b * NPH_AUDIO_BLOCK_FRAMES * 2U,
                               NPH_AUDIO_BLOCK_FRAMES);
    }
    f32 *l = test_buffer(frames);
    for (u32 i = 0U; i < frames; ++i) {
        l[i] = out[(u64)i * 2U];
    }
    const f64 own = 0.5 * pow(2.0 * TEST_PI * 300.0 / 48000.0, 2.0);
    const f64 bend = test_largest_bend(l, frames);
    NTEST_LOG("largest bend %.3g (the tone's own at full level %.3g)", bend, own);
    NTEST_ASSERT_TRUE(bend < own);
    test_buffer_free(l, frames);
    test_buffer_free(out, frames * 2U);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_spatial, the_nearer_voice_is_the_real_one)
{
    // One real voice, two alike but for their distance.
    enum {
        FRAMES = 4800
    };
    NphAudioMixer *m = tm_mixer(1U, 4U, 1U);
    const NphAudioClip far = tm_tone(m, 300.0, 48000U, TM_RATE, 0.5F, NULL);
    const NphAudioClip near = tm_tone(m, 700.0, 48000U, TM_RATE, 0.5F, NULL);
    NphAudioPlayDesc d = tms_desc(far, (NphVec3){0.0F, 0.0F, -20.0F});
    (void)tm_play(m, &d);
    d = tms_desc(near, (NphVec3){0.0F, 0.0F, -2.0F});
    (void)tm_play(m, &d);
    f32 *x = test_buffer(FRAMES);
    f32 *planes[1] = {x};
    tm_render(m, FRAMES, planes);
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(x, FRAMES, 700.0, 48000.0), 0.25, 1e-4);
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(x, FRAMES, 300.0, 48000.0), 0.0, 1e-4);
    test_buffer_free(x, FRAMES);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_spatial, a_stereo_clip_narrows_with_distance)
{
    // A clip with its left channel only, ahead: far off, a point, so the
    // left channel comes out centred; inside its size, the layout opens
    // and the left channel leans left.
    enum {
        FRAMES = 4800
    };
    f32 *pair = test_buffer(2U * 48000U);
    for (u32 i = 0U; i < 48000U; ++i) {
        pair[(u64)i * 2U] = 0.5F * (f32)sin(2.0 * TEST_PI * 440.0 * (f64)i / 48000.0);
    }
    f64 lean[2];
    const f32 distances[2] = {500.0F, 0.5F};
    for (u32 k = 0U; k < 2U; ++k) {
        NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
        const NphAudioClip clip = tm_clip_f32(m, pair, 48000U, 2U, TM_RATE);
        NphAudioPlayDesc d = tms_desc(clip, (NphVec3){0.0F, 0.0F, -distances[k]});
        d.size = 2.0F;
        d.attenuation.rolloff = NPH_AUDIO_ROLLOFF_NONE;
        (void)tm_play(m, &d);
        f32 *l = test_buffer(FRAMES);
        f32 *r = test_buffer(FRAMES);
        f32 *planes[2] = {l, r};
        tm_render(m, FRAMES, planes);
        lean[k] = test_db(test_rms(l, FRAMES) / test_rms(r, FRAMES));
        test_buffer_free(l, FRAMES);
        test_buffer_free(r, FRAMES);
        nph_audio_mixer_destroy(m);
    }
    NTEST_LOG("the left channel's lean: %.2f dB far, %.2f dB near", lean[0], lean[1]);
    NTEST_ASSERT_NEAR_F64(lean[0], 0.0, 0.2);
    NTEST_ASSERT_TRUE(lean[1] > 6.0);
    test_buffer_free(pair, 2U * 48000U);
}
