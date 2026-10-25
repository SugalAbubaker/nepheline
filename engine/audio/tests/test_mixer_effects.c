// SPDX-License-Identifier: Apache-2.0
// The mixer's insert effects (mixer.h "Effects"): a chain is its effects in
// order, to the bit; coming in, bypass, and removal crossfade; sends feed
// reverberation; the master limiter holds; parameters arrive; capacities
// and bus removal.

#include <nepheline/audio/pan.h>

#include "test_mixer_support.h"

static NphAudioBus tme_bus(NphAudioMixer *m)
{
    const NphAudioBusDesc bd = nph_audio_bus_desc_default();
    NphAudioBus b;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(m, &bd, &b), NPH_RESULT_OK);
    return b;
}

static NphAudioEffect tme_effect(NphAudioMixer *m, NphAudioBus bus, const NphAudioEffectDesc *d)
{
    NphAudioEffect e;
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(m, bus, d, &e), NPH_RESULT_OK);
    return e;
}

NTEST(audio_mixer_effects, a_chain_is_its_effects_in_order)
{
    // A centred mono voice copied at the mix rate: the bus holds it times
    // cos(pi/4) on both sides, then an equaliser and a compressor. The
    // effects come in over silence; the same effects made alone, fed the
    // same silence and then the same blocks, agree to the bit.
    enum {
        BLOCKS = 40
    };
    const u32 frames = BLOCKS * NPH_AUDIO_BLOCK_FRAMES;
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    f32 *x = test_buffer(frames);
    test_noise(x, frames, 7U, 0.6F);
    const NphAudioClip clip = tm_clip_f32(m, x, frames, 1U, TM_RATE);
    const NphAudioBus bus = tme_bus(m);
    NphAudioEffectDesc eq = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_EQ);
    eq.eq[1] = (NphAudioEqBand){NPH_AUDIO_FILTER_PEAK, 2500.0F, 1.2F, 7.0F, 1};
    eq.eq[3] = (NphAudioEqBand){NPH_AUDIO_FILTER_HIGHPASS, 120.0F, 0.7071F, 0.0F, 1};
    NphAudioEffectDesc comp = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_COMPRESSOR);
    comp.compressor.threshold_db = -20.0F;
    (void)tme_effect(m, bus, &eq);
    (void)tme_effect(m, bus, &comp);
    f32 silence[2U * 2U * NPH_AUDIO_BLOCK_FRAMES];
    nph_audio_mixer_render(m, silence, 2U * NPH_AUDIO_BLOCK_FRAMES);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.bus = bus;
    (void)tm_play(m, &d);
    f32 *l = test_buffer(frames);
    f32 *r = test_buffer(frames);
    f32 *planes[2] = {l, r};
    tm_render(m, frames, planes);
    // The reference.
    NphAudioEq ref_eq;
    nph_audio_eq_init(&ref_eq, 2U, (f32)TM_RATE);
    for (u32 b = 0U; b < NPH_AUDIO_EQ_BANDS; ++b) {
        nph_audio_eq_set_band(&ref_eq, b, &eq.eq[b]);
    }
    NphAudioCompressor ref_comp;
    nph_audio_compressor_init(&ref_comp, 2U, (f32)TM_RATE, &comp.compressor);
    NphAudioPanner panner;
    NTEST_ASSERT_EQ_INT(nph_audio_panner_init(&panner, NPH_AUDIO_LAYOUT_STEREO), NPH_RESULT_OK);
    f32 g[NPH_AUDIO_MAX_CHANNELS];
    nph_audio_pan(&panner, 0.0F, 0.0F, g);
    f32 *rl = test_buffer(frames);
    f32 *rr = test_buffer(frames);
    for (u32 i = 0U; i < frames; ++i) {
        rl[i] = x[i] * g[0];
        rr[i] = x[i] * g[1];
    }
    f32 scratch[NPH_AUDIO_BLOCK_FRAMES];
    for (u32 b = 0U; b < 2U; ++b) {
        f32 zl[NPH_AUDIO_BLOCK_FRAMES] = {0.0F};
        f32 zr[NPH_AUDIO_BLOCK_FRAMES] = {0.0F};
        f32 *zero[2] = {zl, zr};
        nph_audio_eq_process(&ref_eq, zero, NPH_AUDIO_BLOCK_FRAMES, scratch);
        nph_audio_compressor_process(&ref_comp, zero, NPH_AUDIO_BLOCK_FRAMES);
    }
    for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
        f32 *ref[2] = {rl + (u64)b * NPH_AUDIO_BLOCK_FRAMES, rr + (u64)b * NPH_AUDIO_BLOCK_FRAMES};
        nph_audio_eq_process(&ref_eq, ref, NPH_AUDIO_BLOCK_FRAMES, scratch);
        nph_audio_compressor_process(&ref_comp, ref, NPH_AUDIO_BLOCK_FRAMES);
    }
    for (u32 i = 0U; i < frames; ++i) {
        NTEST_ASSERT_TRUE(l[i] == rl[i] && r[i] == rr[i]);
    }
    test_buffer_free(rl, frames);
    test_buffer_free(rr, frames);
    test_buffer_free(l, frames);
    test_buffer_free(r, frames);
    test_buffer_free(x, frames);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_effects, coming_in_bypass_and_removal_do_not_click)
{
    // A 200 Hz tone through a bus whose echo (damped, no feedback, a 1 ms
    // repeat at half level: a comb, so its presence is plain in the level)
    // comes in, is bypassed, comes back, and is destroyed.
    enum {
        BLOCKS = 60
    };
    const u32 frames = BLOCKS * NPH_AUDIO_BLOCK_FRAMES;
    NphAudioMixer *m = tm_mixer(1U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 200.0, 96000U, TM_RATE, 0.4F, NULL);
    const NphAudioBus bus = tme_bus(m);
    NphAudioEffectDesc echo = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_ECHO);
    echo.echo.delay_ms = 1.0F;
    echo.echo.feedback = 0.0F;
    echo.echo.wet = 0.5F;
    echo.echo.dry = 1.0F;
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.bus = bus;
    (void)tm_play(m, &d);
    f32 *out = test_buffer(frames);
    NphAudioEffect e = {0U};
    for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
        if (b == 5U) {
            e = tme_effect(m, bus, &echo);
        }
        if (b == 20U) {
            nph_audio_effect_set_bypass(m, e, true);
        }
        if (b == 30U) {
            nph_audio_effect_set_bypass(m, e, false);
        }
        if (b == 45U) {
            nph_audio_effect_destroy(m, e);
            NTEST_ASSERT_FALSE(nph_audio_effect_is_valid(m, e));
        }
        nph_audio_mixer_render(m, out + (u64)b * NPH_AUDIO_BLOCK_FRAMES, NPH_AUDIO_BLOCK_FRAMES);
        nph_audio_mixer_update(m);
    }
    const f64 own = 0.4 * 1.5 * pow(2.0 * TEST_PI * 200.0 / 48000.0, 2.0);
    const f64 bend = test_largest_bend(out, frames);
    NTEST_LOG("largest bend %.3g (the tone's own, at its loudest, %.3g)", bend, own);
    NTEST_ASSERT_TRUE(bend < 1.2 * own);
    // And the effect was heard: the comb raises a 200 Hz tone by nearly
    // half (1 + 0.5 cos(2 pi 200 * 1 ms)).
    const f64 with =
        test_tone_amplitude(out + (u64)12U * NPH_AUDIO_BLOCK_FRAMES, 1920U, 200.0, 48000.0);
    const f64 without =
        test_tone_amplitude(out + (u64)22U * NPH_AUDIO_BLOCK_FRAMES, 1920U, 200.0, 48000.0);
    NTEST_ASSERT_NEAR_F64(with / without, sqrt(1.25 + cos(2.0 * TEST_PI * 0.2)), 0.01);
    test_buffer_free(out, frames);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_effects, a_reverb_on_a_send_bus_rings_on)
{
    enum {
        FRAMES = 48000
    };
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 500.0, 4800U, TM_RATE, 0.5F, NULL); // 0.1 s
    const NphAudioBus room = tme_bus(m);
    NphAudioEffectDesc rv = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_REVERB);
    rv.reverb.decay_s = 1.0F;
    (void)tme_effect(m, room, &rv);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.sends[0] = room;
    d.send_gains[0] = 0.8F;
    (void)tm_play(m, &d);
    f32 *l = test_buffer(FRAMES);
    f32 *r = test_buffer(FRAMES);
    f32 *planes[2] = {l, r};
    tm_render(m, FRAMES, planes);
    // After the tone: the tail, falling.
    const f64 early = test_rms(l + 9600, 4800U);
    const f64 later = test_rms(l + 28800, 4800U);
    NTEST_LOG("the tail: %.1f dB at 0.2 s, %.1f dB at 0.6 s", test_db(early), test_db(later));
    NTEST_ASSERT_TRUE(early > 0.003);
    NTEST_ASSERT_TRUE(later < early * 0.25 && later > 0.0);
    test_buffer_free(l, FRAMES);
    test_buffer_free(r, FRAMES);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_effects, the_master_limiter_holds_and_meters)
{
    enum {
        FRAMES = 24000
    };
    NphAudioMixer *m = tm_mixer(2U, 8U, 8U);
    const NphAudioEffect lim = tme_effect(m, NPH_AUDIO_MASTER_BUS,
                                          &(NphAudioEffectDesc){
                                              .type = NPH_AUDIO_EFFECT_LIMITER,
                                              .limiter = {-1.0F, 80.0F, 1.5F},
                                          });
    for (u32 k = 0U; k < 4U; ++k) {
        const NphAudioClip c = tm_tone(m, 300.0 + 170.0 * k, 48000U, TM_RATE, 0.6F, NULL);
        NphAudioPlayDesc d = nph_audio_play_desc_default(c);
        (void)tm_play(m, &d);
    }
    f32 *l = test_buffer(FRAMES);
    f32 *r = test_buffer(FRAMES);
    f32 *planes[2] = {l, r};
    tm_render(m, FRAMES, planes);
    // From the limiter's second block: in its first, coming in, the signal
    // without it is still heard.
    f32 worst = 0.0F;
    for (u32 i = NPH_AUDIO_BLOCK_FRAMES; i < (u32)FRAMES; ++i) {
        worst = fabsf(l[i]) > worst ? fabsf(l[i]) : worst;
        worst = fabsf(r[i]) > worst ? fabsf(r[i]) : worst;
    }
    const f32 meter = nph_audio_effect_meter(m, lim);
    NTEST_LOG("loudest sample %.4f (ceiling %.4f); the limiter's reduction %.1f dB", (f64)worst,
              (f64)nph_audio_db_to_gain(-1.0F), (f64)meter);
    NTEST_ASSERT_TRUE(worst <= nph_audio_db_to_gain(-1.0F));
    NTEST_ASSERT_TRUE(meter > 3.0F);
    test_buffer_free(l, FRAMES);
    test_buffer_free(r, FRAMES);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_effects, parameters_arrive)
{
    // A 1 kHz tone through an equaliser band raised to +12 dB at 1 kHz by
    // set_param: the level follows once the band has glided there.
    enum {
        FRAMES = 24000
    };
    NphAudioMixer *m = tm_mixer(1U, 4U, 4U);
    const NphAudioClip clip = tm_tone(m, 1000.0, 96000U, TM_RATE, 0.1F, NULL);
    const NphAudioBus bus = tme_bus(m);
    const NphAudioEffect e = tme_effect(m, bus, &(NphAudioEffectDesc){.type = NPH_AUDIO_EFFECT_EQ});
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.bus = bus;
    (void)tm_play(m, &d);
    nph_audio_effect_set_param(m, e, NPH_AUDIO_EQ_PARAM(2, NPH_AUDIO_EQ_TYPE),
                               (f32)NPH_AUDIO_FILTER_PEAK);
    nph_audio_effect_set_param(m, e, NPH_AUDIO_EQ_PARAM(2, NPH_AUDIO_EQ_FREQ), 1000.0F);
    nph_audio_effect_set_param(m, e, NPH_AUDIO_EQ_PARAM(2, NPH_AUDIO_EQ_Q), 1.0F);
    nph_audio_effect_set_param(m, e, NPH_AUDIO_EQ_PARAM(2, NPH_AUDIO_EQ_GAIN), 12.0F);
    nph_audio_effect_set_param(m, e, NPH_AUDIO_EQ_PARAM(2, NPH_AUDIO_EQ_ENABLED), 1.0F);
    nph_audio_effect_set_param(m, e, 999U, 1.0F);   // no such parameter: ignored
    nph_audio_effect_set_param(m, e, 3U, (f32)NAN); // not a number: ignored
    f32 *out = test_buffer(FRAMES);
    f32 *planes[1] = {out};
    tm_render(m, FRAMES, planes);
    const f64 level = test_db(test_tone_amplitude(out + 12000, 12000U, 1000.0, 48000.0) / 0.1);
    NTEST_LOG("1 kHz through the raised band: %.3f dB", level);
    NTEST_ASSERT_NEAR_F64(level, 12.0, 0.05);
    test_buffer_free(out, FRAMES);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_mixer_effects, capacities_and_bus_removal)
{
    NphAudioMixer *m = tm_mixer(2U, 4U, 4U);
    const NphAudioBus bus = tme_bus(m);
    const NphAudioEffectDesc eq = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_EQ);
    NphAudioEffect e[NPH_AUDIO_BUS_EFFECTS];
    for (u32 k = 0U; k < NPH_AUDIO_BUS_EFFECTS; ++k) {
        e[k] = tme_effect(m, bus, &eq);
    }
    NphAudioEffect extra;
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(m, bus, &eq, &extra), NPH_RESULT_FULL);
    // A destroyed effect keeps its place until the audio thread lets go.
    nph_audio_effect_destroy(m, e[3]);
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(m, bus, &eq, &extra), NPH_RESULT_FULL);
    f32 block[2U * NPH_AUDIO_BLOCK_FRAMES];
    nph_audio_mixer_render(m, block, NPH_AUDIO_BLOCK_FRAMES);
    nph_audio_mixer_update(m);
    e[3] = tme_effect(m, bus, &eq);
    NphAudioEffectDesc bad = eq;
    bad.type = NPH_AUDIO_EFFECT_COUNT;
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(m, bus, &bad, &extra), NPH_RESULT_INVALID_ARGUMENT);
    const NphAudioBus stale = {0xFFFFFFFFU};
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(m, stale, &eq, &extra),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphAudioEffectDesc limiter = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_LIMITER);
    limiter.limiter.lookahead_ms = 100.0F; // beyond the maximum
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(m, NPH_AUDIO_MASTER_BUS, &limiter, &extra),
                        NPH_RESULT_INVALID_ARGUMENT);
    // Destroying the bus releases its effects: stale at once, their slots
    // free again once the audio thread has let go.
    nph_audio_bus_destroy(m, bus);
    for (u32 k = 0U; k < NPH_AUDIO_BUS_EFFECTS; ++k) {
        NTEST_ASSERT_FALSE(nph_audio_effect_is_valid(m, e[k]));
    }
    f32 out[2U * NPH_AUDIO_BLOCK_FRAMES];
    nph_audio_mixer_render(m, out, NPH_AUDIO_BLOCK_FRAMES);
    nph_audio_mixer_update(m);
    const NphAudioBus again = tme_bus(m);
    for (u32 k = 0U; k < NPH_AUDIO_BUS_EFFECTS; ++k) {
        e[k] = tme_effect(m, again, &eq);
    }
    // A meter of a type without one, and of a stale handle, reads 0.
    NTEST_ASSERT_TRUE(nph_audio_effect_meter(m, e[0]) == 0.0F);
    NTEST_ASSERT_TRUE(nph_audio_effect_meter(m, extra) == 0.0F);
    nph_audio_mixer_destroy(m); // frees the effects still on buses
}
