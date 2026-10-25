// SPDX-License-Identifier: Apache-2.0
// The echo (echo.h): repeats where and as loud as they should be, ping-pong,
// damping, a new delay time without a click, and stability.

#include <nepheline/audio/echo.h>

#include "test_audio_support.h"

#define EC_RATE 48000.0F

static NphAudioEcho ec_make(u32 channels, const NphAudioEchoParams *p)
{
    NphAudioEcho e;
    NTEST_ASSERT_EQ_INT(
        nph_audio_echo_init(&e, nph_allocator_system(), channels, EC_RATE, 1000.0F, p),
        NPH_RESULT_OK);
    return e;
}

static void ec_run(NphAudioEcho *e, f32 **planes, u32 channels, u32 frames)
{
    for (u32 i = 0U; i < frames; i += NPH_AUDIO_BLOCK_FRAMES) {
        f32 *sub[NPH_AUDIO_MAX_CHANNELS];
        for (u32 c = 0U; c < channels; ++c) {
            sub[c] = planes[c] + i;
        }
        nph_audio_echo_process(e, sub, nph_min_u32(NPH_AUDIO_BLOCK_FRAMES, frames - i));
    }
}

NTEST(audio_echo, repeats_fall_by_the_feedback)
{
    // An impulse; no damping, no high-pass: echoes at 10, 20, 30 ms, each
    // `feedback` times the one before.
    enum {
        FRAMES = 4096
    };
    NphAudioEchoParams p = nph_audio_echo_params_default();
    p.delay_ms = 10.0F;
    p.feedback = 0.5F;
    p.damping_hz = 0.0F;
    p.highpass_hz = 0.0F;
    p.wet = 1.0F;
    p.dry = 0.0F;
    NphAudioEcho e = ec_make(1U, &p);
    f32 *x = test_buffer(FRAMES);
    x[0] = 1.0F;
    f32 *planes[1] = {x};
    ec_run(&e, planes, 1U, FRAMES);
    const u32 d = 480U;
    NTEST_ASSERT_NEAR_F32(x[d], 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(x[(u64)2U * d], 0.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(x[(u64)3U * d], 0.25F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(x[d - 1U], 0.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(x[d + 1U], 0.0F, 1e-7F);
    nph_audio_echo_free(&e);
    test_buffer_free(x, FRAMES);
}

NTEST(audio_echo, ping_pong_alternates_sides)
{
    enum {
        FRAMES = 4096
    };
    NphAudioEchoParams p = nph_audio_echo_params_default();
    p.delay_ms = 10.0F;
    p.feedback = 0.5F;
    p.damping_hz = 0.0F;
    p.highpass_hz = 0.0F;
    p.wet = 1.0F;
    p.dry = 0.0F;
    p.ping_pong = 1;
    NphAudioEcho e = ec_make(2U, &p);
    f32 *l = test_buffer(FRAMES);
    f32 *r = test_buffer(FRAMES);
    l[0] = 1.0F; // the input's sum goes to the left line: 0.5
    f32 *planes[2] = {l, r};
    ec_run(&e, planes, 2U, FRAMES);
    const u32 d = 480U;
    NTEST_ASSERT_NEAR_F32(l[d], 0.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(r[d], 0.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(r[(u64)2U * d], 0.25F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(l[(u64)2U * d], 0.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(l[(u64)3U * d], 0.125F, 1e-6F);
    nph_audio_echo_free(&e);
    test_buffer_free(l, FRAMES);
    test_buffer_free(r, FRAMES);
}

// The level of the n-th repeat of a tone burst at `freq`, against the
// first's.
static f64 ec_repeat_drop(f64 freq, u32 n)
{
    enum {
        FRAMES = 48000,
        BURST = 4800
    };
    NphAudioEchoParams p = nph_audio_echo_params_default();
    p.delay_ms = 200.0F;
    p.feedback = 0.7F;
    p.damping_hz = 3000.0F;
    p.highpass_hz = 0.0F;
    p.wet = 1.0F;
    p.dry = 0.0F;
    NphAudioEcho e = ec_make(1U, &p);
    f32 *x = test_buffer(FRAMES);
    test_sine(x, BURST, freq, (f64)EC_RATE, 0.0, 0.5);
    f32 *planes[1] = {x};
    ec_run(&e, planes, 1U, FRAMES);
    const u32 d = 9600U;
    const f64 first = test_tone_amplitude(x + d + 1000U, 2400U, freq, (f64)EC_RATE);
    const f64 later = test_tone_amplitude(x + (u64)n * d + 1000U, 2400U, freq, (f64)EC_RATE);
    nph_audio_echo_free(&e);
    test_buffer_free(x, FRAMES);
    return later / first;
}

NTEST(audio_echo, damping_darkens_the_repeats)
{
    const f64 low = ec_repeat_drop(200.0, 3U);
    const f64 high = ec_repeat_drop(8000.0, 3U);
    NTEST_LOG("third repeat against the first: 200 Hz %.2f dB, 8 kHz %.2f dB", test_db(low),
              test_db(high));
    // The low tone loses the feedback only (0.7^2, -6.2 dB); the high one
    // the damping's too, twice.
    NTEST_ASSERT_NEAR_F64(test_db(low), test_db(0.49), 0.1);
    NTEST_ASSERT_TRUE(test_db(high) < test_db(low) - 6.0);
}

NTEST(audio_echo, a_new_delay_time_does_not_click)
{
    // The wet signal of a steady 300 Hz tone while the delay jumps from 120
    // to 171.1 ms (taps a third of a cycle apart, so the crossfade moves the
    // level): a smooth crossfade between taps, no step.
    enum {
        FRAMES = 48000
    };
    NphAudioEchoParams p = nph_audio_echo_params_default();
    p.delay_ms = 120.0F;
    p.feedback = 0.0F;
    p.wet = 1.0F;
    p.dry = 0.0F;
    NphAudioEcho e = ec_make(1U, &p);
    f32 *x = test_buffer(FRAMES);
    test_sine(x, FRAMES, 300.0, (f64)EC_RATE, 0.0, 0.5);
    for (u32 i = 0U; i < (u32)FRAMES; i += NPH_AUDIO_BLOCK_FRAMES) {
        if (i == 24064U) {
            p.delay_ms = 171.1F;
            nph_audio_echo_set(&e, &p);
        }
        f32 *planes[1] = {x + i};
        nph_audio_echo_process(&e, planes, nph_min_u32(NPH_AUDIO_BLOCK_FRAMES, FRAMES - i));
    }
    // Past the first tap's arrival (the tone's own start is a step).
    const f64 own = 0.5 * pow(2.0 * TEST_PI * 300.0 / 48000.0, 2.0);
    const f64 bend = test_largest_bend(x + 8000, FRAMES - 8000U);
    NTEST_LOG("largest bend %.3g (the tone's own %.3g)", bend, own);
    NTEST_ASSERT_TRUE(bend < 1.5 * own);
    nph_audio_echo_free(&e);
    test_buffer_free(x, FRAMES);
}

NTEST(audio_echo, full_feedback_stays_bounded_and_resets)
{
    enum {
        FRAMES = 96000
    };
    NphAudioEchoParams p = nph_audio_echo_params_default();
    p.delay_ms = 3.0F; // a short loop: per-sample feedback
    p.feedback = 2.0F; // clamped to 0.95
    NphAudioEcho e = ec_make(2U, &p);
    f32 *l = test_buffer(FRAMES);
    f32 *r = test_buffer(FRAMES);
    test_noise(l, FRAMES, 4U, 1.0F);
    test_noise(r, FRAMES, 5U, 1.0F);
    f32 *planes[2] = {l, r};
    ec_run(&e, planes, 2U, FRAMES);
    f32 worst = 0.0F;
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        NTEST_ASSERT_FALSE(isnan(l[i]) || isnan(r[i]));
        worst = fabsf(l[i]) > worst ? fabsf(l[i]) : worst;
    }
    NTEST_LOG("loudest output %.2f", (f64)worst);
    NTEST_ASSERT_TRUE(worst < 20.0F);
    nph_audio_echo_reset(&e);
    memset(l, 0, sizeof(f32) * 1024U);
    memset(r, 0, sizeof(f32) * 1024U);
    nph_audio_echo_process(&e, planes, 1024U);
    for (u32 i = 0U; i < 1024U; ++i) {
        NTEST_ASSERT_TRUE(l[i] == 0.0F && r[i] == 0.0F);
    }
    nph_audio_echo_free(&e);
    test_buffer_free(l, FRAMES);
    test_buffer_free(r, FRAMES);
}
