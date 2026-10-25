// SPDX-License-Identifier: Apache-2.0
// The resampler (resampler.h): its banks, its table against the exact
// kernel, and its quality measured on tones: a flat passband, no aliasing
// when it plays fast, no images when it plays slow.

#include <nepheline/audio/kernels.h>
#include <nepheline/audio/resampler.h>

#include "test_audio_support.h"

typedef struct ResamplerFixture {
    NphAudioResampler *rs;
} ResamplerFixture;

NTEST_SETUP(ResamplerFixture)
{
    NTEST_ASSERT_EQ_INT(nph_audio_resampler_create(nph_allocator_system(), &f->rs), NPH_RESULT_OK);
}

NTEST_TEARDOWN(ResamplerFixture)
{
    nph_audio_resampler_destroy(f->rs);
}

NTEST_F(ResamplerFixture, banks_widen_as_the_cutoff_falls)
{
    const NphAudioResampleBank *b0 = nph_audio_resampler_bank_at(f->rs, 0U);
    NTEST_ASSERT_EQ_UINT(b0->taps, 2U * NPH_AUDIO_RESAMPLE_ZERO_CROSSINGS);
    NTEST_ASSERT_TRUE(b0->scale == 1.0F);
    NTEST_ASSERT_EQ_UINT(nph_audio_resampler_bank_at(f->rs, 12U)->taps, 96U);
    NTEST_ASSERT_NEAR_F32(nph_audio_resampler_bank_at(f->rs, 12U)->scale, 0.5F, 1e-7F);
    NTEST_ASSERT_EQ_UINT(nph_audio_resampler_bank_at(f->rs, 24U)->taps,
                         NPH_AUDIO_RESAMPLE_MAX_TAPS);
    NTEST_ASSERT_NULL(nph_audio_resampler_bank_at(f->rs, NPH_AUDIO_RESAMPLE_BANKS));
    f32 previous = 2.0F;
    for (u32 k = 0U; k < NPH_AUDIO_RESAMPLE_BANKS; ++k) {
        const NphAudioResampleBank *b = nph_audio_resampler_bank_at(f->rs, k);
        NTEST_ASSERT_EQ_UINT(b->taps % 8U, 0U);
        NTEST_ASSERT_TRUE(b->scale < previous);
        NTEST_ASSERT_GE_UINT(b->taps, (u32)(2.0F * 24.0F / b->scale));
        previous = b->scale;
    }
}

NTEST_F(ResamplerFixture, steps_pick_the_widest_bank_that_cannot_alias)
{
    const u64 one = NPH_AUDIO_FRAME_ONE;
    NTEST_ASSERT_EQ_UINT(nph_audio_resample_step(1.0), one);
    NTEST_ASSERT_EQ_UINT(nph_audio_resample_step(0.5), one / 2U);
    NTEST_ASSERT_EQ_UINT(nph_audio_resample_step(-1.0), 1U);
    NTEST_ASSERT_EQ_UINT(nph_audio_resample_step((f64)NAN), 1U);
    NTEST_ASSERT_EQ_UINT(nph_audio_resample_step(1e-30), 1U);
    NTEST_ASSERT_EQ_UINT(nph_audio_resample_step(100.0), 4U * one);
    NTEST_ASSERT_EQ_PTR(nph_audio_resampler_bank(f->rs, one / 3U),
                        nph_audio_resampler_bank_at(f->rs, 0U));
    NTEST_ASSERT_EQ_PTR(nph_audio_resampler_bank(f->rs, one),
                        nph_audio_resampler_bank_at(f->rs, 0U));
    NTEST_ASSERT_EQ_PTR(nph_audio_resampler_bank(f->rs, one + 1U),
                        nph_audio_resampler_bank_at(f->rs, 1U));
    NTEST_ASSERT_EQ_PTR(nph_audio_resampler_bank(f->rs, 2U * one),
                        nph_audio_resampler_bank_at(f->rs, 12U));
    NTEST_ASSERT_EQ_PTR(nph_audio_resampler_bank(f->rs, 2U * one + 1U),
                        nph_audio_resampler_bank_at(f->rs, 13U));
    NTEST_ASSERT_EQ_PTR(nph_audio_resampler_bank(f->rs, 4U * one),
                        nph_audio_resampler_bank_at(f->rs, 24U));
    NTEST_ASSERT_EQ_PTR(nph_audio_resampler_bank(f->rs, 9U * one),
                        nph_audio_resampler_bank_at(f->rs, 24U));
    // Every bank's cutoff is at or below 1 / step for the steps it serves.
    for (u32 i = 1U; i <= 300U; ++i) {
        const f64 ratio = 1.0 + 3.0 * (f64)i / 300.0;
        const NphAudioResampleBank *b =
            nph_audio_resampler_bank(f->rs, nph_audio_resample_step(ratio));
        NTEST_ASSERT_TRUE((f64)b->scale <= 1.0 / ratio + 1e-7);
        NTEST_ASSERT_TRUE((f64)b->scale > 0.94387 / ratio); // within a semitone
    }
}

NTEST(audio_resampler, spans_cover_the_kernel)
{
    i64 first = 0;
    i64 last = 0;
    const u64 pos = (10ULL << 32U) | (1ULL << 31U); // 10.5
    nph_audio_resample_span(pos, NPH_AUDIO_FRAME_ONE, 4U, 48U, &first, &last);
    NTEST_ASSERT_EQ_INT(first, -13);
    NTEST_ASSERT_EQ_INT(last, 37);
    nph_audio_resample_span(pos, NPH_AUDIO_FRAME_ONE, 0U, 48U, &first, &last);
    NTEST_ASSERT_EQ_INT(last, first - 1);
}

NTEST_F(ResamplerFixture, the_table_is_the_kernel)
{
    const u32 banks[4] = {0U, 7U, 12U, 24U};
    u64 rng = 1234U;
    for (u32 bi = 0U; bi < 4U; ++bi) {
        const NphAudioResampleBank *b = nph_audio_resampler_bank_at(f->rs, banks[bi]);
        const u32 taps = b->taps;
        f64 worst = 0.0;
        for (u32 trial = 0U; trial < 2000U; ++trial) {
            const f64 phi = (f64)(test_rng_next(&rng) >> 11U) / 9007199254740992.0;
            const u32 j = (u32)(test_rng_next(&rng) % taps);
            const u32 segment = (u32)(phi * (f64)NPH_AUDIO_RESAMPLE_PHASES);
            const f64 u = phi * (f64)NPH_AUDIO_RESAMPLE_PHASES - (f64)segment;
            const f32 *rows = b->coeffs + (u64)segment * 4U * taps;
            const f64 approx =
                (f64)rows[j] + u * ((f64)rows[taps + j] +
                                    u * ((f64)rows[2U * taps + j] + u * (f64)rows[3U * taps + j]));
            const f64 exact =
                nph_audio_resample_kernel(phi + (f64)taps / 2.0 - 1.0 - (f64)j, (f64)b->scale);
            const f64 e = fabs(approx - exact);
            worst = e > worst ? e : worst;
        }
        NTEST_LOG("bank %u (%u taps): worst table error %.3g", banks[bi], taps, worst);
        NTEST_ASSERT_LT_INT((i32)(worst * 1e9), 500); // 5e-7: the f32 coefficients' rounding
    }
    // The kernel itself: one at its centre, zero at the other integers (at
    // scale 1), zero outside its support.
    NTEST_ASSERT_NEAR_F64(nph_audio_resample_kernel(0.0, 1.0), 1.0, 1e-15);
    NTEST_ASSERT_NEAR_F64(nph_audio_resample_kernel(3.0, 1.0), 0.0, 1e-15);
    NTEST_ASSERT_NEAR_F64(nph_audio_resample_kernel(24.0, 1.0), 0.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_audio_resample_kernel(40.0, 0.5), 0.0, 1e-3);
    NTEST_ASSERT_NEAR_F64(nph_audio_resample_kernel(48.0, 0.5), 0.0, 0.0);
    NTEST_ASSERT_NEAR_F64(nph_audio_resample_kernel(1.0, 2.0), 0.0, 0.0); // invalid scale
}

// Resamples `src` (with at least the widest bank's history before index 0
// and enough after) from position 0 into `out`.
static void resample_from_zero(const NphAudioResampler *rs, const f32 *src, f64 ratio, f32 *out,
                               u32 count)
{
    const u64 step = nph_audio_resample_step(ratio);
    const NphAudioResampleBank *bank = nph_audio_resampler_bank(rs, step);
    const f32 *chans[1] = {src};
    f32 *dst[1] = {out};
    nph_audio_kernels_best()->resample(bank, chans, 1U, 0U, step, dst, count);
}

enum {
    RS_HISTORY = NPH_AUDIO_RESAMPLE_MAX_TAPS,
    RS_OUT = 4096,
    RS_SRC = 4 * RS_OUT + 2 * RS_HISTORY
};

NTEST_F(ResamplerFixture, constants_and_whole_frames_pass_unchanged)
{
    f32 *buf = test_buffer(RS_SRC);
    f32 *out = test_buffer(RS_OUT);
    for (u32 i = 0U; i < (u32)RS_SRC; ++i) {
        buf[i] = 0.5F;
    }
    const f64 ratios[6] = {0.25, 0.73, 1.0, 1.31, 2.0, 3.7};
    for (u32 r = 0U; r < 6U; ++r) {
        resample_from_zero(f->rs, buf + RS_HISTORY, ratios[r] + 1e-7, out, 512U);
        for (u32 i = 0U; i < 512U; ++i) {
            NTEST_ASSERT_NEAR_F32(out[i], 0.5F, 3e-6F);
        }
    }
    // Whole frames at the full-band kernel are the samples themselves.
    test_noise(buf, RS_SRC, 77U, 0.8F);
    resample_from_zero(f->rs, buf + RS_HISTORY, 1.0, out, RS_OUT);
    for (u32 i = 0U; i < (u32)RS_OUT; ++i) {
        NTEST_ASSERT_NEAR_F32(out[i], buf[RS_HISTORY + i], 2e-6F);
    }
    test_buffer_free(buf, RS_SRC);
    test_buffer_free(out, RS_OUT);
}

// A tone at `nu` cycles per source frame, resampled at `ratio`: the
// output's amplitude at nu * ratio cycles per output frame, and what is
// left once that tone is fitted out (relative to the amplitude, in dB).
static void measure_tone(const NphAudioResampler *rs, f64 nu, f64 ratio, f64 *out_gain_db,
                         f64 *out_residual_db)
{
    f32 *buf = test_buffer(RS_SRC);
    f32 *out = test_buffer(RS_OUT);
    test_sine(buf, RS_SRC, nu, 1.0, -(f64)RS_HISTORY, 1.0);
    resample_from_zero(rs, buf + RS_HISTORY, ratio, out, RS_OUT);
    const f64 fo = nu * ((f64)nph_audio_resample_step(ratio) / (f64)NPH_AUDIO_FRAME_ONE);
    // Fit a sin(w i) + b cos(w i) and measure the rest.
    f64 ss = 0.0;
    f64 cc = 0.0;
    f64 sc = 0.0;
    f64 xs = 0.0;
    f64 xc = 0.0;
    for (u32 i = 0U; i < (u32)RS_OUT; ++i) {
        const f64 w = 2.0 * TEST_PI * fo * (f64)i;
        const f64 s = sin(w);
        const f64 c = cos(w);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += (f64)out[i] * s;
        xc += (f64)out[i] * c;
    }
    const f64 det = ss * cc - sc * sc;
    const f64 a = (xs * cc - xc * sc) / det;
    const f64 b = (xc * ss - xs * sc) / det;
    f64 err = 0.0;
    for (u32 i = 0U; i < (u32)RS_OUT; ++i) {
        const f64 w = 2.0 * TEST_PI * fo * (f64)i;
        const f64 e = (f64)out[i] - (a * sin(w) + b * cos(w));
        err += e * e;
    }
    const f64 amp = sqrt(a * a + b * b);
    *out_gain_db = test_db(amp);
    // The residual's RMS against the tone's (amp / sqrt 2).
    *out_residual_db = test_db(sqrt(err / (f64)RS_OUT) / (amp / sqrt(2.0)));
    test_buffer_free(buf, RS_SRC);
    test_buffer_free(out, RS_OUT);
}

NTEST_F(ResamplerFixture, the_passband_is_flat_and_clean)
{
    const f64 ratios[7] = {0.5, 44100.0 / 48000.0, 1.0, 48000.0 / 44100.0, 1.5, 2.0, 3.0};
    const f64 out_freqs[5] = {0.01, 0.1, 0.2, 0.3, 0.4}; // cycles per output frame
    for (u32 r = 0U; r < 7U; ++r) {
        for (u32 fi = 0U; fi < 5U; ++fi) {
            const f64 nu = out_freqs[fi] / ratios[r];
            if (nu > 0.43) {
                continue; // above the source's own band
            }
            f64 gain = 0.0;
            f64 residual = 0.0;
            measure_tone(f->rs, nu, ratios[r], &gain, &residual);
            NTEST_LOG("ratio %.4f tone %.3f: gain %+.5f dB, residual %.1f dB", ratios[r],
                      out_freqs[fi], gain, residual);
            NTEST_ASSERT_LT_INT((i32)(fabs(gain) * 1e4), 100); // within 0.01 dB
            NTEST_ASSERT_LT_INT((i32)residual, -95);           // images, aliases, rounding
        }
    }
}

NTEST_F(ResamplerFixture, playing_fast_does_not_alias)
{
    // Tones the output cannot hold (above its Nyquist frequency) must be
    // filtered out, not folded back.
    const f64 ratios[4] = {1.5, 2.0, 3.0, 4.0};
    const f64 out_freqs[4] = {0.6, 0.75, 0.9, 1.2};
    f32 *buf = test_buffer(RS_SRC);
    f32 *out = test_buffer(RS_OUT);
    for (u32 r = 0U; r < 4U; ++r) {
        for (u32 fi = 0U; fi < 4U; ++fi) {
            const f64 nu = out_freqs[fi] / ratios[r];
            if (nu > 0.49) {
                continue;
            }
            test_sine(buf, RS_SRC, nu, 1.0, -(f64)RS_HISTORY, 1.0);
            resample_from_zero(f->rs, buf + RS_HISTORY, ratios[r], out, RS_OUT);
            const f64 level = test_db(test_rms(out, RS_OUT) * sqrt(2.0));
            NTEST_LOG("ratio %.2f tone at %.2f of the output rate: %.1f dB", ratios[r],
                      out_freqs[fi], level);
            NTEST_ASSERT_LT_INT((i32)level, -95);
        }
    }
    test_buffer_free(buf, RS_SRC);
    test_buffer_free(out, RS_OUT);
}
