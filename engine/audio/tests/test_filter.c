// SPDX-License-Identifier: Apache-2.0
// The filters (filter.h): every section's response against the Audio EQ
// Cookbook's own formulas, the processing against the response, precision
// at a low corner, stability under modulation, and the one-pole.

#include <nepheline/audio/filter.h>

#include "test_audio_support.h"

// The cookbook's direct-form coefficients (Bristow-Johnson 2005), computed
// independently of filter.c, and their magnitude response.
typedef struct Rbj {
    f64 b0, b1, b2, a0, a1, a2;
} Rbj;

static Rbj rbj_design(NphAudioFilterType type, f64 f0, f64 q, f64 gain_db, f64 fs)
{
    const f64 a = pow(10.0, gain_db / 40.0);
    const f64 w0 = 2.0 * TEST_PI * f0 / fs;
    const f64 cw = cos(w0);
    const f64 alpha = sin(w0) / (2.0 * q);
    const f64 ra = 2.0 * sqrt(a) * alpha;
    Rbj r = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    switch (type) {
    case NPH_AUDIO_FILTER_LOWPASS:
        r = (Rbj){(1.0 - cw) / 2.0, 1.0 - cw,  (1.0 - cw) / 2.0,
                  1.0 + alpha,      -2.0 * cw, 1.0 - alpha};
        break;
    case NPH_AUDIO_FILTER_HIGHPASS:
        r = (Rbj){(1.0 + cw) / 2.0, -(1.0 + cw), (1.0 + cw) / 2.0,
                  1.0 + alpha,      -2.0 * cw,   1.0 - alpha};
        break;
    case NPH_AUDIO_FILTER_BANDPASS:
        r = (Rbj){alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha};
        break;
    case NPH_AUDIO_FILTER_NOTCH:
        r = (Rbj){1.0, -2.0 * cw, 1.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha};
        break;
    case NPH_AUDIO_FILTER_ALLPASS:
        r = (Rbj){1.0 - alpha, -2.0 * cw, 1.0 + alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha};
        break;
    case NPH_AUDIO_FILTER_PEAK:
        r = (Rbj){1.0 + alpha * a, -2.0 * cw, 1.0 - alpha * a,
                  1.0 + alpha / a, -2.0 * cw, 1.0 - alpha / a};
        break;
    case NPH_AUDIO_FILTER_LOW_SHELF:
        r = (Rbj){a * ((a + 1.0) - (a - 1.0) * cw + ra), 2.0 * a * ((a - 1.0) - (a + 1.0) * cw),
                  a * ((a + 1.0) - (a - 1.0) * cw - ra), (a + 1.0) + (a - 1.0) * cw + ra,
                  -2.0 * ((a - 1.0) + (a + 1.0) * cw),   (a + 1.0) + (a - 1.0) * cw - ra};
        break;
    case NPH_AUDIO_FILTER_HIGH_SHELF:
        r = (Rbj){a * ((a + 1.0) + (a - 1.0) * cw + ra), -2.0 * a * ((a - 1.0) + (a + 1.0) * cw),
                  a * ((a + 1.0) + (a - 1.0) * cw - ra), (a + 1.0) - (a - 1.0) * cw + ra,
                  2.0 * ((a - 1.0) - (a + 1.0) * cw),    (a + 1.0) - (a - 1.0) * cw - ra};
        break;
    case NPH_AUDIO_FILTER_COUNT:
        break;
    }
    return r;
}

static f64 rbj_magnitude(const Rbj *r, f64 f, f64 fs)
{
    const f64 w = 2.0 * TEST_PI * f / fs;
    const f64 nr = r->b0 + r->b1 * cos(w) + r->b2 * cos(2.0 * w);
    const f64 ni = -r->b1 * sin(w) - r->b2 * sin(2.0 * w);
    const f64 dr = r->a0 + r->a1 * cos(w) + r->a2 * cos(2.0 * w);
    const f64 di = -r->a1 * sin(w) - r->a2 * sin(2.0 * w);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

NTEST(audio_filter, every_section_is_the_cookbooks)
{
    const f64 fs = 48000.0;
    const f32 corners[4] = {40.0F, 440.0F, 3000.0F, 15000.0F};
    const f32 qs[3] = {0.5F, 0.7071068F, 4.0F};
    const f32 gains[3] = {-12.0F, 3.0F, 18.0F};
    f64 worst = 0.0;
    for (u32 t = 0U; t < (u32)NPH_AUDIO_FILTER_COUNT; ++t) {
        for (u32 ci = 0U; ci < 4U; ++ci) {
            for (u32 qi = 0U; qi < 3U; ++qi) {
                for (u32 gi = 0U; gi < 3U; ++gi) {
                    const NphAudioFilterCoeffs c = nph_audio_filter_design(
                        (NphAudioFilterType)t, corners[ci], qs[qi], gains[gi], (f32)fs);
                    // The cookbook from the same f32 parameters the design saw.
                    const Rbj r = rbj_design((NphAudioFilterType)t, (f64)corners[ci], (f64)qs[qi],
                                             (f64)gains[gi], fs);
                    for (u32 k = 0U; k <= 60U; ++k) {
                        const f64 f = 20.0 * pow(1000.0, (f64)k / 60.0); // 20 Hz .. 20 kHz
                        const f64 want = rbj_magnitude(&r, f, fs);
                        if (want < 1e-3) {
                            continue; // in a notch's null: the decibels are meaningless
                        }
                        const f64 got = nph_audio_filter_magnitude(&c, f, fs);
                        const f64 diff = fabs(test_db(got) - test_db(want));
                        worst = diff > worst ? diff : worst;
                    }
                }
            }
        }
    }
    NTEST_LOG("worst difference from the cookbook: %.3g dB", worst);
    NTEST_ASSERT_LT_INT((i32)(worst * 1e4), 10); // 0.001 dB: the f32 coefficients
}

// Runs a tone through the filter and measures the steady state.
static f64 measured_gain_db(const NphAudioFilterCoeffs *c, f64 f, f64 fs, u32 settle, u32 span)
{
    const u32 total = settle + span;
    f32 *x = test_buffer(total);
    test_sine(x, total, f, fs, 0.0, 0.5);
    NphAudioFilterState s;
    nph_audio_filter_reset(&s);
    nph_audio_filter_process(c, &s, x, x, total); // in place
    // The tone's frequency in the measured span starts at sample `settle`.
    const f64 amp = test_tone_amplitude(x + settle, span, f, fs);
    test_buffer_free(x, total);
    return test_db(amp / 0.5);
}

NTEST(audio_filter, processing_converges_to_the_response)
{
    const f64 fs = 48000.0;
    const struct {
        NphAudioFilterType type;
        f32 corner, q, gain;
    } cases[6] = {
        {NPH_AUDIO_FILTER_LOWPASS, 1000.0F, 0.7071068F, 0.0F},
        {NPH_AUDIO_FILTER_HIGHPASS, 250.0F, 1.5F, 0.0F},
        {NPH_AUDIO_FILTER_BANDPASS, 2000.0F, 2.0F, 0.0F},
        {NPH_AUDIO_FILTER_PEAK, 3000.0F, 1.0F, 9.0F},
        {NPH_AUDIO_FILTER_LOW_SHELF, 200.0F, 0.7071068F, -6.0F},
        {NPH_AUDIO_FILTER_HIGH_SHELF, 8000.0F, 0.7071068F, 4.5F},
    };
    const f64 tones[5] = {97.0, 503.0, 1999.0, 6007.0, 15013.0};
    for (u32 i = 0U; i < 6U; ++i) {
        const NphAudioFilterCoeffs c = nph_audio_filter_design(cases[i].type, cases[i].corner,
                                                               cases[i].q, cases[i].gain, (f32)fs);
        for (u32 t = 0U; t < 5U; ++t) {
            const f64 want = test_db(nph_audio_filter_magnitude(&c, tones[t], fs));
            const f64 got = measured_gain_db(&c, tones[t], fs, 8192U, 8192U);
            NTEST_ASSERT_NEAR_F64(got, want, 0.01);
        }
    }
    // The corner of a Butterworth low-pass is -3.01 dB.
    const NphAudioFilterCoeffs lp =
        nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, 1000.0F, 0.7071068F, 0.0F, (f32)fs);
    NTEST_ASSERT_NEAR_F64(test_db(nph_audio_filter_magnitude(&lp, 1000.0, fs)), -3.0103, 1e-3);
}

NTEST(audio_filter, a_low_corner_keeps_its_precision)
{
    // A 20 Hz high-pass at 48 kHz: poles 0.9963 from the origin, where a
    // direct form in f32 loses its corner; the state-variable form keeps it.
    const f64 fs = 48000.0;
    const NphAudioFilterCoeffs c =
        nph_audio_filter_design(NPH_AUDIO_FILTER_HIGHPASS, 20.0F, 0.7071068F, 0.0F, (f32)fs);
    NTEST_ASSERT_NEAR_F64(measured_gain_db(&c, 20.0, fs, 96000U, 96000U), -3.0103, 0.02);
    NTEST_ASSERT_NEAR_F64(measured_gain_db(&c, 10.0, fs, 96000U, 96000U),
                          test_db(nph_audio_filter_magnitude(&c, 10.0, fs)), 0.02);
}

NTEST(audio_filter, modulation_every_block_stays_bounded)
{
    const f32 fs = 48000.0F;
    enum {
        BLOCK = 64,
        BLOCKS = 4000
    };
    f32 x[BLOCK];
    u64 rng = 42U;
    NphAudioFilterState s;
    nph_audio_filter_reset(&s);
    f32 worst = 0.0F;
    for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
        // A corner anywhere from 50 Hz to 20 kHz and a Q up to 10, new every
        // block, on full-scale noise.
        const f32 corner = 50.0F * powf(400.0F, 0.5F + 0.5F * test_rng_signed(&rng));
        const f32 q = 0.5F + 4.75F * (1.0F + test_rng_signed(&rng));
        const NphAudioFilterCoeffs c =
            nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, corner, q, 0.0F, fs);
        test_noise(x, BLOCK, 1000U + b, 1.0F);
        nph_audio_filter_process(&c, &s, x, x, BLOCK);
        for (u32 i = 0U; i < (u32)BLOCK; ++i) {
            NTEST_ASSERT_FALSE(isnan(x[i]));
            worst = fabsf(x[i]) > worst ? fabsf(x[i]) : worst;
        }
    }
    NTEST_LOG("loudest output under modulation: %.2f", (f64)worst);
    NTEST_ASSERT_TRUE(worst < 40.0F); // a resonance of Q 10 on noise, not a blow-up
}

NTEST(audio_filter, the_passthrough_and_invalid_designs_change_nothing)
{
    f32 x[64];
    f32 y[64];
    test_noise(x, 64U, 3U, 1.0F);
    const NphAudioFilterCoeffs designs[3] = {
        nph_audio_filter_passthrough(),
        nph_audio_filter_design(NPH_AUDIO_FILTER_COUNT, 1000.0F, 1.0F, 0.0F, 48000.0F),
        nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, 1000.0F, 1.0F, 0.0F, 0.0F),
    };
    for (u32 d = 0U; d < 3U; ++d) {
        NphAudioFilterState s;
        nph_audio_filter_reset(&s);
        nph_audio_filter_process(&designs[d], &s, x, y, 64U);
        NTEST_ASSERT_TRUE(test_same_bits(x, y, 64U));
        NTEST_ASSERT_NEAR_F64(nph_audio_filter_magnitude(&designs[d], 1000.0, 48000.0), 1.0, 0.0);
    }
    // Out-of-range parameters are clamped, not trusted.
    const NphAudioFilterCoeffs c =
        nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, 1e9F, -1.0F, 0.0F, 48000.0F);
    NTEST_ASSERT_FALSE(isnan(c.a1) || isnan(c.a2) || isnan(c.a3));
    NTEST_ASSERT_TRUE(nph_audio_filter_magnitude(&c, 100.0, 48000.0) > 0.9);
}

NTEST(audio_filter, one_pole_low_pass)
{
    const f32 fs = 48000.0F;
    const f32 a = nph_audio_one_pole_coeff(1000.0F, fs);
    NTEST_ASSERT_NEAR_F64((f64)a, 1.0 - exp(-2.0 * TEST_PI * 1000.0 / 48000.0), 1e-7);
    NTEST_ASSERT_TRUE(nph_audio_one_pole_coeff(0.0F, fs) == 0.0F);
    NTEST_ASSERT_TRUE(nph_audio_one_pole_coeff(100.0F, 0.0F) == 0.0F);
    // A step settles to the input.
    f32 x[4800];
    for (u32 i = 0U; i < 4800U; ++i) {
        x[i] = 1.0F;
    }
    f32 state = 0.0F;
    nph_audio_one_pole_process(a, &state, x, x, 4800U);
    NTEST_ASSERT_NEAR_F32(state, 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(x[4799], 1.0F, 1e-6F);
    // A tone at the corner: |H| = a / |1 - (1 - a) e^-jw|.
    f32 *tone = test_buffer(48000U);
    test_sine(tone, 48000U, 1000.0, 48000.0, 0.0, 1.0);
    state = 0.0F;
    nph_audio_one_pole_process(a, &state, tone, tone, 48000U);
    const f64 w = 2.0 * TEST_PI * 1000.0 / 48000.0;
    const f64 b = 1.0 - (f64)a;
    const f64 want =
        (f64)a / sqrt((1.0 - b * cos(w)) * (1.0 - b * cos(w)) + b * sin(w) * b * sin(w));
    NTEST_ASSERT_NEAR_F64(test_tone_amplitude(tone + 24000, 24000U, 1000.0, 48000.0), want, 1e-4);
    test_buffer_free(tone, 48000U);
}
