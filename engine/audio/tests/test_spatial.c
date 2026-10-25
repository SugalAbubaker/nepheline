// SPDX-License-Identifier: Apache-2.0
// Spatialisation's functions (spatial.h): the attenuation laws, Doppler,
// the air against ISO 9613-1, directions, and spread.

#include <nepheline/audio/spatial.h>

#include "test_audio_support.h"

NTEST(audio_spatial, the_inverse_law_and_its_clamps)
{
    NphAudioAttenuation a = nph_audio_attenuation_default();
    a.min_distance = 2.0F;
    a.max_distance = 32.0F;
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 0.5F), 1.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 2.0F), 1.0F, 1e-7F);
    // Physical: -6 dB a doubling.
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 4.0F), 0.5F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 16.0F), 0.125F, 1e-7F);
    // No further fall past the maximum.
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 1000.0F), 1.0F / 16.0F, 1e-7F);
    // A steeper factor.
    a.factor = 3.0F;
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 4.0F), 2.0F / (2.0F + 6.0F), 1e-7F);
    // None.
    a.rolloff = NPH_AUDIO_ROLLOFF_NONE;
    NTEST_ASSERT_TRUE(nph_audio_attenuation_gain(&a, 500.0F) == 1.0F);
}

NTEST(audio_spatial, linear_and_curves)
{
    NphAudioAttenuation a = nph_audio_attenuation_default();
    a.rolloff = NPH_AUDIO_ROLLOFF_LINEAR;
    a.min_distance = 10.0F;
    a.max_distance = 30.0F;
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 5.0F), 1.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 20.0F), 0.5F, 1e-7F);
    NTEST_ASSERT_TRUE(nph_audio_attenuation_gain(&a, 30.0F) == 0.0F);
    NTEST_ASSERT_TRUE(nph_audio_attenuation_gain(&a, 99.0F) == 0.0F);
    // A designer's curve: linear in dB between its points.
    a.rolloff = NPH_AUDIO_ROLLOFF_CURVE;
    a.point_count = 3U;
    a.points[0][0] = 5.0F;
    a.points[0][1] = 0.0F;
    a.points[1][0] = 15.0F;
    a.points[1][1] = -20.0F;
    a.points[2][0] = 40.0F;
    a.points[2][1] = -30.0F;
    NTEST_ASSERT_NEAR_F32(nph_audio_attenuation_gain(&a, 1.0F), 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F64(test_db((f64)nph_audio_attenuation_gain(&a, 10.0F)), -10.0, 1e-4);
    NTEST_ASSERT_NEAR_F64(test_db((f64)nph_audio_attenuation_gain(&a, 27.5F)), -25.0, 1e-4);
    NTEST_ASSERT_NEAR_F64(test_db((f64)nph_audio_attenuation_gain(&a, 100.0F)), -30.0, 1e-4);
    a.point_count = 0U;
    NTEST_ASSERT_TRUE(nph_audio_attenuation_gain(&a, 10.0F) == 1.0F);
}

NTEST(audio_spatial, doppler)
{
    const f32 c = NPH_AUDIO_SPEED_OF_SOUND;
    // A tenth of the speed of sound, towards and away.
    NTEST_ASSERT_NEAR_F32(nph_audio_doppler_ratio(-0.1F * c, c, 1.0F), 1.0F / 0.9F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_doppler_ratio(0.1F * c, c, 1.0F), 1.0F / 1.1F, 1e-6F);
    // Scaled, off, clamped, and through the barrier.
    NTEST_ASSERT_NEAR_F32(nph_audio_doppler_ratio(-0.1F * c, c, 0.5F), 1.0F / 0.95F, 1e-6F);
    NTEST_ASSERT_TRUE(nph_audio_doppler_ratio(-0.1F * c, c, 0.0F) == 1.0F);
    NTEST_ASSERT_TRUE(nph_audio_doppler_ratio(-0.9F * c, c, 1.0F) == 2.0F);
    NTEST_ASSERT_TRUE(nph_audio_doppler_ratio(-1.5F * c, c, 1.0F) == 2.0F); // supersonic
    NTEST_ASSERT_TRUE(nph_audio_doppler_ratio(5.0F * c, c, 1.0F) == 0.5F);
    NTEST_ASSERT_TRUE(nph_audio_doppler_ratio((f32)NAN, c, 1.0F) == 1.0F);
}

// The one-pole's loss at `f` for a corner `fc` (dB).
static f64 sp_one_pole_loss(f64 f, f64 fc)
{
    return 10.0 * log10(1.0 + (f / fc) * (f / fc));
}

NTEST(audio_spatial, the_air_follows_iso_9613)
{
    // Matched at 8 kHz (0.0766 dB/m), and within a few hundredths of a
    // decibel of the standard's 4 kHz (0.0229 dB/m) for the first 20 m,
    // where its losses are small.
    for (u32 d = 5U; d <= 20U; d += 5U) {
        const f64 fc = (f64)nph_audio_air_corner((f32)d, 1.0F);
        NTEST_ASSERT_NEAR_F64(sp_one_pole_loss(8000.0, fc), 0.0766 * (f64)d, 1e-4);
        NTEST_ASSERT_NEAR_F64(sp_one_pole_loss(4000.0, fc), 0.0229 * (f64)d, 0.03);
    }
    // Twice the absorption is twice the distance; none is no filter.
    NTEST_ASSERT_NEAR_F32(nph_audio_air_corner(10.0F, 2.0F), nph_audio_air_corner(20.0F, 1.0F),
                          0.01F);
    NTEST_ASSERT_TRUE(isinf(nph_audio_air_corner(10.0F, 0.0F)));
    NTEST_ASSERT_TRUE(isinf(nph_audio_air_corner(0.0F, 1.0F)));
}

// The digital one-pole's loss at `f` (dB) for its coefficient `a`.
static f64 sp_digital_loss(f64 f, f64 a, f64 rate)
{
    const f64 w = 2.0 * TEST_PI * f / rate;
    const f64 b = 1.0 - a;
    return -10.0 * log10(a * a / (1.0 - 2.0 * b * cos(w) + b * b));
}

NTEST(audio_spatial, the_mixers_air_filter_loses_what_air_does)
{
    // The digital filter itself meets the standard at 8 kHz, at 48 and
    // 44.1 kHz, near and far; at 16 kHz the match moves to 5.6 kHz.
    const f32 rates[2] = {48000.0F, 44100.0F};
    for (u32 r = 0U; r < 2U; ++r) {
        for (u32 d = 1U; d <= 200U; d *= 3U) {
            const f32 a = nph_audio_air_coeff((f32)d, 1.0F, rates[r]);
            NTEST_ASSERT_NEAR_F64(sp_digital_loss(8000.0, (f64)a, (f64)rates[r]), 0.0766 * d, 1e-3);
        }
    }
    const f32 a = nph_audio_air_coeff(50.0F, 1.0F, 16000.0F);
    const f64 f = 0.35 * 16000.0;
    NTEST_ASSERT_NEAR_F64(sp_digital_loss(f, (f64)a, 16000.0),
                          0.0766 * 50.0 * (f / 8000.0) * (f / 8000.0), 1e-3);
    NTEST_ASSERT_TRUE(nph_audio_air_coeff(10.0F, 0.0F, 48000.0F) == 1.0F);
    NTEST_ASSERT_TRUE(nph_audio_air_coeff(0.0F, 1.0F, 48000.0F) == 1.0F);
}

NTEST(audio_spatial, directions_and_spread)
{
    const f32 pi = (f32)TEST_PI;
    NTEST_ASSERT_NEAR_F32(nph_audio_azimuth(0.0F, -1.0F), 0.0F, 1e-7F); // ahead
    NTEST_ASSERT_NEAR_F32(nph_audio_azimuth(1.0F, 0.0F), 0.5F * pi, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_azimuth(-1.0F, 0.0F), -0.5F * pi, 1e-6F);
    NTEST_ASSERT_NEAR_F32(fabsf(nph_audio_azimuth(0.0F, 1.0F)), pi, 1e-6F); // behind
    NTEST_ASSERT_TRUE(nph_audio_azimuth(0.0F, 0.0F) == 0.0F);
    // A point has none; a sphere subtends twice its half-angle, half the
    // directions at its surface, all at its centre, continuously.
    NTEST_ASSERT_TRUE(nph_audio_spread(0.0F, 3.0F) == 0.0F);
    NTEST_ASSERT_NEAR_F32(nph_audio_spread(1.0F, 2.0F), 2.0F * asinf(0.5F), 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_spread(1.0F, 1.0F), pi, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_spread(1.0F, 0.0F), 2.0F * pi, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_spread(1.0F, 1.0001F), nph_audio_spread(1.0F, 0.9999F), 0.03F);
}
