// SPDX-License-Identifier: Apache-2.0
// Spatialisation (spatial.h).

#include <nepheline/audio/spatial.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

#define SP_PI 3.14159265358979323846

// ISO 9613-1's pure-tone absorption at 8 kHz, 20 C, 50% relative humidity.
#define SP_AIR_8K_DB_PER_M 0.0766

NphAudioAttenuation nph_audio_attenuation_default(void)
{
    NphAudioAttenuation a;
    memset(&a, 0, sizeof a);
    a.rolloff = NPH_AUDIO_ROLLOFF_INVERSE;
    a.min_distance = 1.0F;
    a.max_distance = 50.0F;
    a.factor = 1.0F;
    return a;
}

f32 nph_audio_attenuation_gain(const NphAudioAttenuation *a, f32 distance)
{
    NPH_ASSERT(a != NULL);
    const f32 d = distance > 0.0F ? distance : 0.0F;
    const f32 lo = a->min_distance > 0.0F ? a->min_distance : 0.0F;
    const f32 hi = a->max_distance > lo ? a->max_distance : lo;
    switch (a->rolloff) {
    case NPH_AUDIO_ROLLOFF_INVERSE: {
        if (lo <= 0.0F) {
            return 1.0F;
        }
        const f32 c = d < lo ? lo : (d > hi ? hi : d);
        const f32 f = a->factor > 0.0F ? a->factor : 0.0F;
        return lo / (lo + f * (c - lo));
    }
    case NPH_AUDIO_ROLLOFF_LINEAR:
        if (d <= lo) {
            return 1.0F;
        }
        if (d >= hi) {
            return 0.0F;
        }
        return 1.0F - (d - lo) / (hi - lo);
    case NPH_AUDIO_ROLLOFF_CURVE: {
        const u32 n =
            a->point_count < NPH_AUDIO_CURVE_POINTS ? a->point_count : NPH_AUDIO_CURVE_POINTS;
        if (n == 0U) {
            return 1.0F;
        }
        if (d <= a->points[0][0]) {
            return nph_audio_db_to_gain(a->points[0][1]);
        }
        for (u32 i = 1U; i < n; ++i) {
            if (d <= a->points[i][0]) {
                const f32 d0 = a->points[i - 1U][0];
                const f32 d1 = a->points[i][0];
                const f32 t = d1 > d0 ? (d - d0) / (d1 - d0) : 1.0F;
                return nph_audio_db_to_gain(a->points[i - 1U][1] +
                                            t * (a->points[i][1] - a->points[i - 1U][1]));
            }
        }
        return nph_audio_db_to_gain(a->points[n - 1U][1]);
    }
    case NPH_AUDIO_ROLLOFF_NONE:
    case NPH_AUDIO_ROLLOFF_COUNT:
        break;
    }
    return 1.0F;
}

f32 nph_audio_doppler_ratio(f32 radial_velocity, f32 speed_of_sound, f32 factor)
{
    if (!(speed_of_sound > 0.0F) || !(factor > 0.0F) || !isfinite(radial_velocity)) {
        return 1.0F;
    }
    const f32 den = speed_of_sound + factor * radial_velocity;
    if (!(den > 0.0F)) {
        return 2.0F; // approaching at the speed of sound or faster: the highest
    }
    const f32 r = speed_of_sound / den;
    return r < 0.5F ? 0.5F : (r > 2.0F ? 2.0F : r);
}

f32 nph_audio_air_corner(f32 distance, f32 factor)
{
    // A one-pole low-pass loses 10 log10(1 + (f / fc)^2) dB at f: set equal
    // to the air's loss at 8 kHz over the distance and solved for fc.
    const f64 loss_db = SP_AIR_8K_DB_PER_M * (f64)(factor > 0.0F ? factor : 0.0F) *
                        (f64)(distance > 0.0F ? distance : 0.0F);
    const f64 excess = pow(10.0, loss_db / 10.0) - 1.0;
    if (!(excess > 1e-12)) {
        return INFINITY;
    }
    return (f32)(8000.0 / sqrt(excess));
}

f32 nph_audio_air_coeff(f32 distance, f32 factor, f32 rate)
{
    if (!(rate > 0.0F)) {
        return 1.0F;
    }
    const f64 f = 8000.0 < 0.35 * (f64)rate ? 8000.0 : 0.35 * (f64)rate;
    const f64 loss_db = SP_AIR_8K_DB_PER_M * (f / 8000.0) * (f / 8000.0) *
                        (f64)(factor > 0.0F ? factor : 0.0F) *
                        (f64)(distance > 0.0F ? distance : 0.0F);
    // |H|^2 = (1 - b)^2 / (1 - 2 b cos w + b^2) = g^2 at w, with b = 1 - a:
    // a quadratic in b, whose root in [0, 1) is the pole.
    const f64 g2 = pow(10.0, -loss_db / 10.0);
    const f64 a2 = 1.0 - g2;
    if (!(a2 > 1e-9)) {
        return 1.0F;
    }
    const f64 cw = cos(2.0 * SP_PI * f / (f64)rate);
    const f64 h = 1.0 - g2 * cw;
    const f64 b = (h - sqrt(h * h - a2 * a2)) / a2;
    return (f32)(1.0 - b);
}

f32 nph_audio_azimuth(f32 x, f32 z)
{
    if (x == 0.0F && z == 0.0F) {
        return 0.0F;
    }
    return atan2f(x, -z);
}

f32 nph_audio_spread(f32 size, f32 distance)
{
    if (!(size > 0.0F)) {
        return 0.0F;
    }
    if (!(distance > size)) {
        // Inside: from half of all directions at its surface (where the
        // formula below reaches pi) to all of them at its centre.
        const f32 d = distance > 0.0F ? distance : 0.0F;
        return (f32)(SP_PI * (2.0 - (f64)(d / size)));
    }
    return 2.0F * asinf(size / distance);
}
