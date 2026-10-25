// SPDX-License-Identifier: Apache-2.0
// The equaliser (eq.h): its bands' responses in series, and every kind of
// change without a click.

#include <nepheline/audio/eq.h>

#include "test_audio_support.h"

#define EQ_RATE 48000.0F

static NphAudioEqBand eq_band(NphAudioFilterType type, f32 freq, f32 q, f32 gain_db)
{
    NphAudioEqBand b;
    b.type = type;
    b.freq = freq;
    b.q = q;
    b.gain_db = gain_db;
    b.enabled = 1;
    return b;
}

// Runs a tone through the equaliser in blocks and returns its steady
// amplitude (the last half second).
static f64 eq_tone_gain(NphAudioEq *eq, f64 freq)
{
    enum {
        FRAMES = 48000
    };
    f32 *x = test_buffer(FRAMES);
    test_sine(x, FRAMES, freq, (f64)EQ_RATE, 0.0, 0.5);
    f32 scratch[NPH_AUDIO_BLOCK_FRAMES];
    for (u32 b = 0U; b < (u32)FRAMES; b += NPH_AUDIO_BLOCK_FRAMES) {
        f32 *planes[1] = {x + b};
        nph_audio_eq_process(eq, planes, nph_min_u32(NPH_AUDIO_BLOCK_FRAMES, FRAMES - b), scratch);
    }
    const f64 a = test_tone_amplitude(x + FRAMES / 2, FRAMES / 2, freq, (f64)EQ_RATE) / 0.5;
    test_buffer_free(x, FRAMES);
    return a;
}

NTEST(audio_eq, bands_in_series_multiply_their_responses)
{
    const NphAudioEqBand bands[3] = {
        eq_band(NPH_AUDIO_FILTER_LOW_SHELF, 200.0F, 0.7071F, 4.0F),
        eq_band(NPH_AUDIO_FILTER_PEAK, 1500.0F, 1.5F, -7.0F),
        eq_band(NPH_AUDIO_FILTER_HIGH_SHELF, 6000.0F, 0.7071F, 3.0F),
    };
    const f64 freqs[5] = {60.0, 400.0, 1500.0, 3000.0, 12000.0};
    for (u32 f = 0U; f < 5U; ++f) {
        NphAudioEq eq;
        nph_audio_eq_init(&eq, 1U, EQ_RATE);
        f64 expected = 1.0;
        for (u32 b = 0U; b < 3U; ++b) {
            nph_audio_eq_set_band(&eq, b + 1U, &bands[b]); // bands need not be the first
            const NphAudioFilterCoeffs c = nph_audio_filter_design(
                bands[b].type, bands[b].freq, bands[b].q, bands[b].gain_db, EQ_RATE);
            expected *= nph_audio_filter_magnitude(&c, freqs[f], (f64)EQ_RATE);
        }
        const f64 got = eq_tone_gain(&eq, freqs[f]);
        NTEST_LOG("%.0f Hz: %.3f dB (expected %.3f)", freqs[f], test_db(got), test_db(expected));
        NTEST_ASSERT_NEAR_F64(test_db(got), test_db(expected), 0.02);
    }
}

NTEST(audio_eq, nothing_enabled_changes_nothing)
{
    NphAudioEq eq;
    nph_audio_eq_init(&eq, 2U, EQ_RATE);
    NphAudioEqBand off = eq_band(NPH_AUDIO_FILTER_PEAK, 1000.0F, 1.0F, 12.0F);
    off.enabled = 0;
    nph_audio_eq_set_band(&eq, 0U, &off);
    nph_audio_eq_set_band(&eq, 9U, &off); // out of range: ignored
    f32 a[256];
    f32 b[256];
    f32 ref[256];
    test_noise(a, 256U, 1U, 1.0F);
    test_noise(b, 256U, 2U, 1.0F);
    memcpy(ref, a, sizeof ref);
    f32 *planes[2] = {a, b};
    f32 scratch[256];
    nph_audio_eq_process(&eq, planes, 256U, scratch);
    NTEST_ASSERT_TRUE(test_same_bits(a, ref, 256U));
}

// A 200 Hz tone through the equaliser while `change` alters it at block
// 10 and again at block 30; the largest bend must stay the tone's own.
typedef void (*EqChange)(NphAudioEq *eq, u32 block);

static f64 eq_bend(EqChange change)
{
    enum {
        BLOCKS = 48
    };
    const u32 frames = BLOCKS * NPH_AUDIO_BLOCK_FRAMES;
    f32 *x = test_buffer(frames);
    test_sine(x, frames, 200.0, (f64)EQ_RATE, 0.0, 0.5);
    NphAudioEq eq;
    nph_audio_eq_init(&eq, 1U, EQ_RATE);
    f32 scratch[NPH_AUDIO_BLOCK_FRAMES];
    for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
        change(&eq, b);
        f32 *planes[1] = {x + (u64)b * NPH_AUDIO_BLOCK_FRAMES};
        nph_audio_eq_process(&eq, planes, NPH_AUDIO_BLOCK_FRAMES, scratch);
    }
    const f64 bend = test_largest_bend(x, frames);
    test_buffer_free(x, frames);
    return bend;
}

static void eq_switch(NphAudioEq *eq, u32 block)
{
    NphAudioEqBand lp = eq_band(NPH_AUDIO_FILTER_LOWPASS, 6000.0F, 0.7071F, 0.0F);
    if (block == 10U || block == 30U) {
        lp.enabled = block == 10U;
        nph_audio_eq_set_band(eq, 2U, &lp);
    }
}

static void eq_retype(NphAudioEq *eq, u32 block)
{
    if (block == 2U) {
        const NphAudioEqBand a = eq_band(NPH_AUDIO_FILTER_LOW_SHELF, 100.0F, 0.7071F, 3.0F);
        nph_audio_eq_set_band(eq, 0U, &a);
    }
    if (block == 20U) {
        const NphAudioEqBand b = eq_band(NPH_AUDIO_FILTER_HIGH_SHELF, 5000.0F, 0.7071F, 3.0F);
        nph_audio_eq_set_band(eq, 0U, &b);
    }
}

static void eq_sweep(NphAudioEq *eq, u32 block)
{
    // A bell moving from 2 kHz to 8 kHz and back, a step every block.
    const f32 t = (f32)(block % 24U) / 12.0F;
    const f32 f = 2000.0F * powf(4.0F, t < 1.0F ? t : 2.0F - t);
    const NphAudioEqBand bell = eq_band(NPH_AUDIO_FILTER_PEAK, f, 2.0F, 6.0F);
    nph_audio_eq_set_band(eq, 0U, &bell);
}

NTEST(audio_eq, changes_do_not_click)
{
    const f64 own = 0.5 * pow(2.0 * TEST_PI * 200.0 / (f64)EQ_RATE, 2.0);
    const f64 switching = eq_bend(eq_switch);
    const f64 retyping = eq_bend(eq_retype);
    const f64 sweeping = eq_bend(eq_sweep);
    NTEST_LOG("largest bends: switching %.3g, retyping %.3g, sweeping %.3g (the tone's own %.3g)",
              switching, retyping, sweeping, own);
    NTEST_ASSERT_TRUE(switching < 1.5 * own);
    NTEST_ASSERT_TRUE(retyping < 1.5 * own);
    NTEST_ASSERT_TRUE(sweeping < 1.5 * own);
}

NTEST(audio_eq, reset_returns_to_silence)
{
    NphAudioEq eq;
    nph_audio_eq_init(&eq, 1U, EQ_RATE);
    const NphAudioEqBand peak = eq_band(NPH_AUDIO_FILTER_PEAK, 500.0F, 8.0F, 12.0F);
    nph_audio_eq_set_band(&eq, 0U, &peak);
    f32 x[512];
    f32 scratch[512];
    test_noise(x, 512U, 3U, 1.0F);
    f32 *planes[1] = {x};
    nph_audio_eq_process(&eq, planes, 512U, scratch);
    nph_audio_eq_reset(&eq);
    memset(x, 0, sizeof x);
    nph_audio_eq_process(&eq, planes, 512U, scratch);
    for (u32 i = 0U; i < 512U; ++i) {
        NTEST_ASSERT_TRUE(x[i] == 0.0F);
    }
}
