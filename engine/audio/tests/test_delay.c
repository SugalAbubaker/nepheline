// SPDX-License-Identifier: Apache-2.0
// The delay line (delay.h): whole and fractional delays, the ring's wrap,
// and a feedback loop run a block at a time against one run per sample.

#include <nepheline/audio/delay.h>

#include "test_audio_support.h"

typedef struct DelayFixture {
    NphAudioDelay d;
} DelayFixture;

NTEST_SETUP(DelayFixture)
{
    NTEST_ASSERT_EQ_INT(nph_audio_delay_init(&f->d, nph_allocator_system(), 1000U), NPH_RESULT_OK);
}

NTEST_TEARDOWN(DelayFixture)
{
    nph_audio_delay_free(&f->d);
}

NTEST(audio_delay, init_checks_its_size)
{
    NphAudioDelay d;
    NTEST_ASSERT_EQ_INT(nph_audio_delay_init(&d, nph_allocator_system(), 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_audio_delay_init(&d, nph_allocator_system(), (1U << 28U) + 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_audio_delay_free(&d); // safe on the zeroed line a failure leaves
    NTEST_ASSERT_EQ_INT(nph_audio_delay_init(&d, nph_allocator_system(), 1020U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(d.capacity, 1024U);
    nph_audio_delay_free(&d);
    NTEST_ASSERT_EQ_INT(nph_audio_delay_init(&d, nph_allocator_system(), 1021U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(d.capacity, 2048U);
    nph_audio_delay_free(&d);
}

NTEST_F(DelayFixture, whole_delays_return_the_past_across_the_wrap)
{
    // 5000 samples through a 1024-sample ring: every read crosses the wrap
    // somewhere.
    f32 in[100];
    f32 out[100];
    f32 history[5000];
    u32 t = 0U;
    for (u32 block = 0U; block < 50U; ++block) {
        for (u32 i = 0U; i < 100U; ++i) {
            in[i] = (f32)(t + i) * 0.001F;
        }
        if (t >= 1000U) {
            const u32 delays[3] = {100U, 377U, 1000U};
            for (u32 k = 0U; k < 3U; ++k) {
                nph_audio_delay_read(&f->d, delays[k], out, 100U);
                for (u32 i = 0U; i < 100U; ++i) {
                    NTEST_ASSERT_TRUE(out[i] == history[t + i - delays[k]]);
                }
            }
        }
        nph_audio_delay_write(&f->d, in, 100U);
        memcpy(history + t, in, sizeof in);
        t += 100U;
    }
    NTEST_ASSERT_TRUE(nph_audio_delay_tap(&f->d, 1U) == history[4999]);
    NTEST_ASSERT_TRUE(nph_audio_delay_tap(&f->d, 1000U) == history[4000]);
    nph_audio_delay_clear(&f->d);
    NTEST_ASSERT_TRUE(nph_audio_delay_tap(&f->d, 500U) == 0.0F);
}

NTEST_F(DelayFixture, fractional_delays_interpolate_smoothly)
{
    // A quadratic, which the Catmull-Rom spline reproduces exactly.
    f32 in[1000];
    for (u32 i = 0U; i < 1000U; ++i) {
        const f32 x = (f32)i * 0.01F;
        in[i] = 0.25F * x * x - x + 0.5F;
    }
    nph_audio_delay_write(&f->d, in, 1000U);
    f32 out[16];
    const f32 delays[4] = {20.0F, 20.25F, 123.5F, 999.0F};
    for (u32 k = 0U; k < 4U; ++k) {
        nph_audio_delay_read_frac(&f->d, delays[k], out, 16U);
        for (u32 i = 0U; i < 16U; ++i) {
            const f64 x = ((f64)1000.0 + (f64)i - (f64)delays[k]) * 0.01;
            NTEST_ASSERT_NEAR_F64((f64)out[i], 0.25 * x * x - x + 0.5, 2e-5);
        }
    }
    // Whole fractional delays match the whole read.
    f32 whole[16];
    nph_audio_delay_read(&f->d, 500U, whole, 16U);
    nph_audio_delay_read_frac(&f->d, 500.0F, out, 16U);
    NTEST_ASSERT_TRUE(test_same_bits(whole, out, 16U));
    // Delays too short for the block, or too long for the line, clamp.
    nph_audio_delay_read_frac(&f->d, 1.0F, out, 16U);
    nph_audio_delay_read_frac(&f->d, 18.0F, whole, 16U);
    NTEST_ASSERT_TRUE(test_same_bits(whole, out, 16U));
    nph_audio_delay_read_frac(&f->d, 5000.0F, out, 16U);
    nph_audio_delay_read(&f->d, 1000U, whole, 16U);
    NTEST_ASSERT_TRUE(test_same_bits(whole, out, 16U));
}

NTEST_F(DelayFixture, a_feedback_comb_runs_a_block_at_a_time)
{
    // y[n] = x[n] + 0.7 y[n - 300], per sample and per 64-sample block.
    NphAudioDelay per_sample;
    NTEST_ASSERT_EQ_INT(nph_audio_delay_init(&per_sample, nph_allocator_system(), 1000U),
                        NPH_RESULT_OK);
    f32 x[64];
    f32 y[64];
    f32 fb[64];
    for (u32 block = 0U; block < 100U; ++block) {
        test_noise(x, 64U, 500U + block, 1.0F);
        nph_audio_delay_read(&f->d, 300U, fb, 64U);
        for (u32 i = 0U; i < 64U; ++i) {
            y[i] = x[i] + 0.7F * fb[i];
        }
        nph_audio_delay_write(&f->d, y, 64U);
        for (u32 i = 0U; i < 64U; ++i) {
            const f32 ys = x[i] + 0.7F * nph_audio_delay_tap(&per_sample, 300U);
            nph_audio_delay_push(&per_sample, ys);
            NTEST_ASSERT_TRUE(ys == y[i]);
        }
    }
    nph_audio_delay_free(&per_sample);
}
