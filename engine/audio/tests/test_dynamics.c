// SPDX-License-Identifier: Apache-2.0
// Dynamics (dynamics.h): the compressor's curve, levels, and time
// constants; the limiter's ceiling (between samples too), transparency,
// latency, and smooth gain.

#include <nepheline/audio/dynamics.h>

#include "test_audio_support.h"

#define DY_RATE 48000.0F

NTEST(audio_dynamics, the_compressor_curve_is_giannoulis)
{
    NphAudioCompressorParams p = nph_audio_compressor_params_default();
    p.threshold_db = -20.0F;
    p.ratio = 4.0F;
    p.knee_db = 0.0F;
    // Hard knee: nothing below, (x - T)(1 - 1/R) above.
    NTEST_ASSERT_NEAR_F32(nph_audio_compressor_curve(&p, -30.0F), 0.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_compressor_curve(&p, -20.0F), 0.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_compressor_curve(&p, -8.0F), 9.0F, 1e-5F);
    // Soft knee: the quadratic in [T - W/2, T + W/2], continuous at both
    // ends, a quarter of the way at the centre.
    p.knee_db = 10.0F;
    NTEST_ASSERT_NEAR_F32(nph_audio_compressor_curve(&p, -25.0F), 0.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_compressor_curve(&p, -15.0F), 5.0F * 0.75F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(nph_audio_compressor_curve(&p, -20.0F), (1.0F - 0.25F) * 25.0F / 20.0F,
                          1e-5F);
    // Ratio 1 compresses nothing.
    p.ratio = 1.0F;
    NTEST_ASSERT_NEAR_F32(nph_audio_compressor_curve(&p, 0.0F), 0.0F, 1e-6F);
}

// The level in dB of the last `count` frames' peak.
static f64 dy_peak_db(const f32 *x, u32 count)
{
    f32 peak = 0.0F;
    for (u32 i = 0U; i < count; ++i) {
        peak = fabsf(x[i]) > peak ? fabsf(x[i]) : peak;
    }
    return test_db((f64)peak);
}

NTEST(audio_dynamics, the_compressor_settles_on_its_curve)
{
    enum {
        FRAMES = 96000
    };
    NphAudioCompressorParams p = nph_audio_compressor_params_default();
    p.threshold_db = -18.0F;
    p.ratio = 4.0F;
    p.knee_db = 0.0F;
    p.attack_ms = 1.0F; // fast enough to reach each peak's reduction
    p.release_ms = 200.0F;
    p.makeup_db = 2.0F;
    NphAudioCompressor c;
    nph_audio_compressor_init(&c, 2U, DY_RATE, &p);
    f32 *l = test_buffer(FRAMES);
    f32 *r = test_buffer(FRAMES);
    test_sine(l, FRAMES, 1000.0, (f64)DY_RATE, 0.0, 0.5);  // -6 dBFS
    test_sine(r, FRAMES, 1000.0, (f64)DY_RATE, 0.0, 0.05); // -26 dBFS, linked to the left
    for (u32 b = 0U; b < (u32)FRAMES; b += NPH_AUDIO_BLOCK_FRAMES) {
        f32 *planes[2] = {l + b, r + b};
        nph_audio_compressor_process(&c, planes, NPH_AUDIO_BLOCK_FRAMES);
    }
    // -6 dB in is 12 dB over the threshold: 3 dB over out, plus makeup:
    // -13 dB. With a fast attack the detector reaches each peak's
    // reduction, and the slow release holds it between peaks.
    const f64 left = dy_peak_db(l + FRAMES / 2, FRAMES / 2);
    const f64 right = dy_peak_db(r + FRAMES / 2, FRAMES / 2);
    NTEST_LOG("compressed: left %.2f dB, right %.2f dB (deepest reduction %.2f dB)", left, right,
              (f64)c.deepest_db);
    NTEST_ASSERT_NEAR_F64(left, -13.0, 0.25);
    // Linked: the right channel gets the left's reduction.
    NTEST_ASSERT_NEAR_F64(right - left, -20.0, 0.01);
    test_buffer_free(l, FRAMES);
    test_buffer_free(r, FRAMES);
}

NTEST(audio_dynamics, the_compressor_attacks_and_releases_at_its_time_constants)
{
    // A level step from -60 dB to -6 dB and back, on DC so the detector
    // sees a constant level: the reduction moves 63% of the way in one time
    // constant.
    NphAudioCompressorParams p = nph_audio_compressor_params_default();
    p.threshold_db = -30.0F;
    p.ratio = 10.0F;
    p.knee_db = 0.0F;
    p.attack_ms = 20.0F;
    p.release_ms = 250.0F;
    NphAudioCompressor c;
    nph_audio_compressor_init(&c, 1U, DY_RATE, &p);
    const f32 target = nph_audio_compressor_curve(&p, 20.0F * log10f(0.5F));
    const u32 tau_a = (u32)(0.020F * DY_RATE);
    const u32 tau_r = (u32)(0.250F * DY_RATE);
    f32 x[1];
    f32 *planes[1] = {x};
    for (u32 i = 0U; i < tau_a; ++i) {
        x[0] = 0.5F;
        nph_audio_compressor_process(&c, planes, 1U);
    }
    NTEST_ASSERT_NEAR_F32(c.reduction_db / target, 0.6321F, 0.01F);
    for (u32 i = 0U; i < 10U * tau_a; ++i) {
        x[0] = 0.5F;
        nph_audio_compressor_process(&c, planes, 1U);
    }
    const f32 full = c.reduction_db;
    NTEST_ASSERT_NEAR_F32(full, target, 0.01F);
    for (u32 i = 0U; i < tau_r; ++i) {
        x[0] = 0.001F;
        nph_audio_compressor_process(&c, planes, 1U);
    }
    NTEST_ASSERT_NEAR_F32(c.reduction_db / full, 0.3679F, 0.01F);
}

NTEST(audio_dynamics, makeup_changes_ramp)
{
    NphAudioCompressorParams p = nph_audio_compressor_params_default();
    p.threshold_db = 0.0F; // never compresses this signal
    NphAudioCompressor c;
    nph_audio_compressor_init(&c, 1U, DY_RATE, &p);
    f32 x[256];
    for (u32 i = 0U; i < 256U; ++i) {
        x[i] = 0.25F;
    }
    p.makeup_db = 6.0206F;
    nph_audio_compressor_set(&c, &p);
    f32 *planes[1] = {x};
    nph_audio_compressor_process(&c, planes, 256U);
    // From 1 to 2 across the block, linearly.
    NTEST_ASSERT_NEAR_F32(x[0], 0.25F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(x[128], 0.25F * 1.5F, 1e-4F);
    for (u32 i = 1U; i < 256U; ++i) {
        NTEST_ASSERT_TRUE(x[i] >= x[i - 1U]);
    }
}

static NphAudioLimiter dy_limiter(u32 channels, f32 ceiling_db)
{
    NphAudioLimiterParams p = nph_audio_limiter_params_default();
    p.ceiling_db = ceiling_db;
    NphAudioLimiter l;
    NTEST_ASSERT_EQ_INT(nph_audio_limiter_init(&l, nph_allocator_system(), channels, DY_RATE, &p),
                        NPH_RESULT_OK);
    return l;
}

NTEST(audio_dynamics, the_limiter_holds_its_ceiling)
{
    // Full-scale noise 12 dB hot, and a burst over a quiet tone.
    enum {
        FRAMES = 48000
    };
    NphAudioLimiter l = dy_limiter(2U, -1.0F);
    f32 *a = test_buffer(FRAMES);
    f32 *b = test_buffer(FRAMES);
    test_noise(a, FRAMES, 5U, 4.0F);
    test_sine(b, FRAMES, 440.0, (f64)DY_RATE, 0.0, 0.2);
    for (u32 i = 20000U; i < 21000U; ++i) {
        b[i] *= 20.0F;
    }
    for (u32 i = 0U; i < (u32)FRAMES; i += NPH_AUDIO_BLOCK_FRAMES) {
        f32 *planes[2] = {a + i, b + i};
        nph_audio_limiter_process(&l, planes, nph_min_u32(NPH_AUDIO_BLOCK_FRAMES, FRAMES - i));
    }
    const f32 ceiling = nph_audio_db_to_gain(-1.0F);
    f32 worst = 0.0F;
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        worst = fabsf(a[i]) > worst ? fabsf(a[i]) : worst;
        worst = fabsf(b[i]) > worst ? fabsf(b[i]) : worst;
    }
    NTEST_LOG("loudest sample %.6f against the ceiling %.6f", (f64)worst, (f64)ceiling);
    NTEST_ASSERT_TRUE(worst <= ceiling);
    nph_audio_limiter_free(&l);
    test_buffer_free(a, FRAMES);
    test_buffer_free(b, FRAMES);
}

NTEST(audio_dynamics, the_limiter_catches_the_peaks_between_samples)
{
    // A 12 kHz tone (four samples a cycle) at 45 degrees: every sample is
    // at 0.707 of the peak, which falls between them. A sample-peak limiter
    // would let it through 3 dB over. It starts and ends in silence inside
    // the buffer, so the limiter sees both edges as the measure will.
    enum {
        FRAMES = 24000,
        TONE = 20000
    };
    NphAudioLimiter l = dy_limiter(1U, -1.0F);
    f32 *x = test_buffer(FRAMES);
    for (u32 i = 0U; i < (u32)TONE; ++i) {
        x[i] = (f32)(1.2 * sin(2.0 * TEST_PI * 12000.0 * (f64)i / 48000.0 + TEST_PI / 4.0));
    }
    const f32 before = nph_audio_true_peak(&l, x, FRAMES);
    NTEST_ASSERT_NEAR_F64(test_db((f64)before), test_db(1.2), 0.3);
    f32 *planes[1] = {x};
    for (u32 i = 0U; i < (u32)FRAMES; i += NPH_AUDIO_BLOCK_FRAMES) {
        planes[0] = x + i;
        nph_audio_limiter_process(&l, planes, nph_min_u32(NPH_AUDIO_BLOCK_FRAMES, FRAMES - i));
    }
    // The output's true peak, measured as the limiter's detector measured
    // the input (from the same silence before it: a measure started
    // mid-signal would see a step there that the signal does not have).
    const f32 after = nph_audio_true_peak(&l, x, FRAMES);
    NTEST_LOG("true peak: %.3f dB in, %.3f dB out", test_db((f64)before), test_db((f64)after));
    NTEST_ASSERT_TRUE(test_db((f64)after) <= -1.0 + 0.01);
    nph_audio_limiter_free(&l);
    test_buffer_free(x, FRAMES);
}

NTEST(audio_dynamics, the_limiter_is_transparent_below_its_ceiling)
{
    enum {
        FRAMES = 8192
    };
    NphAudioLimiter l = dy_limiter(1U, -1.0F);
    const u32 latency = nph_audio_limiter_latency(&l);
    NTEST_ASSERT_EQ_UINT(latency, (u32)ceilf(1.5F * 48.0F) - 1U + NPH_AUDIO_LIMITER_TAPS / 2U);
    f32 *x = test_buffer(FRAMES);
    f32 *ref = test_buffer(FRAMES);
    test_noise(ref, FRAMES, 9U, 0.4F); // true peaks well below -1 dB
    memcpy(x, ref, sizeof(f32) * FRAMES);
    f32 *planes[1] = {x};
    for (u32 i = 0U; i < (u32)FRAMES; i += NPH_AUDIO_BLOCK_FRAMES) {
        planes[0] = x + i;
        nph_audio_limiter_process(&l, planes, NPH_AUDIO_BLOCK_FRAMES);
    }
    // The input, delayed by the latency, untouched.
    for (u32 i = 0U; i < latency; ++i) {
        NTEST_ASSERT_TRUE(x[i] == 0.0F);
    }
    for (u32 i = latency; i < (u32)FRAMES; ++i) {
        NTEST_ASSERT_NEAR_F32(x[i], ref[i - latency], 1e-7F);
    }
    nph_audio_limiter_free(&l);
    test_buffer_free(x, FRAMES);
    test_buffer_free(ref, FRAMES);
}

NTEST(audio_dynamics, the_limiter_gain_ramps_and_recovers)
{
    // A level that jumps 19 dB over the ceiling and back, as DC so the gain
    // can be read off every sample: it falls in a ramp across the
    // lookahead, holds, then recovers at the release rate.
    enum {
        FRAMES = 48000,
        JUMP = 12000
    };
    NphAudioLimiterParams p = nph_audio_limiter_params_default();
    p.release_ms = 50.0F;
    NphAudioLimiter l;
    NTEST_ASSERT_EQ_INT(nph_audio_limiter_init(&l, nph_allocator_system(), 1U, DY_RATE, &p),
                        NPH_RESULT_OK);
    f32 *x = test_buffer(FRAMES);
    f32 *in = test_buffer(FRAMES);
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        in[i] = (i >= JUMP && i < 2U * JUMP) ? 8.0F : 0.5F;
    }
    memcpy(x, in, sizeof(f32) * FRAMES);
    f32 *planes[1] = {x};
    for (u32 i = 0U; i < (u32)FRAMES; i += NPH_AUDIO_BLOCK_FRAMES) {
        planes[0] = x + i;
        nph_audio_limiter_process(&l, planes, nph_min_u32(NPH_AUDIO_BLOCK_FRAMES, FRAMES - i));
    }
    const u32 latency = nph_audio_limiter_latency(&l);
    f64 last = 1.0;
    f64 worst_step = 0.0;
    f64 lowest = 1.0;
    for (u32 i = latency + 1U; i < (u32)FRAMES; ++i) {
        const f64 g = (f64)x[i] / (f64)in[i - latency];
        worst_step = fabs(g - last) > worst_step ? fabs(g - last) : worst_step;
        last = g;
        lowest = g < lowest ? g : lowest;
    }
    // The ramp's step: the gain's fall spread over the lookahead's frames.
    const f64 ceiling = (f64)nph_audio_db_to_gain(-1.0F);
    const f64 ramp_step = (1.0 - ceiling / 8.0) / (f64)l.lookahead;
    // Steady, the gain is what the level needs; at the jumps a little less:
    // a step's reconstruction overshoots between samples (the interpolator's
    // Gibbs ringing), and a true-peak limiter must catch that too.
    const u32 mid = JUMP + JUMP / 2U + latency;
    const f64 steady = (f64)x[mid] / (f64)in[mid - latency];
    NTEST_LOG("steady gain %.5f, lowest %.5f, largest step %.5f (a lookahead ramp's %.5f)", steady,
              lowest, worst_step, ramp_step);
    NTEST_ASSERT_NEAR_F64(steady, ceiling / 8.0, 1e-5);
    NTEST_ASSERT_TRUE(lowest > ceiling / 8.0 / 1.15);
    NTEST_ASSERT_TRUE(worst_step <= ramp_step * 1.2);
    // Recovered half a second (ten release times) after the level fell.
    NTEST_ASSERT_NEAR_F64((f64)x[FRAMES - 1] / (f64)in[FRAMES - 1 - latency], 1.0, 1e-3);
    nph_audio_limiter_free(&l);
    test_buffer_free(x, FRAMES);
    test_buffer_free(in, FRAMES);
}

NTEST(audio_dynamics, the_limiter_refuses_impossible_settings)
{
    NphAudioLimiter l;
    NphAudioLimiterParams p = nph_audio_limiter_params_default();
    p.lookahead_ms = 0.1F;
    NTEST_ASSERT_EQ_INT(nph_audio_limiter_init(&l, nph_allocator_system(), 2U, DY_RATE, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    p = nph_audio_limiter_params_default();
    NTEST_ASSERT_EQ_INT(nph_audio_limiter_init(&l, NULL, 2U, DY_RATE, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_audio_limiter_init(&l, nph_allocator_system(), 0U, DY_RATE, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_audio_limiter_free(&l); // safe after a refusal
}
