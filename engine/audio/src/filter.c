// SPDX-License-Identifier: Apache-2.0
// The state-variable biquad and the one-pole low-pass (filter.h).

#include <nepheline/audio/filter.h>

#include <math.h>

#define FILTER_PI 3.14159265358979323846

NphAudioFilterCoeffs nph_audio_filter_passthrough(void)
{
    NphAudioFilterCoeffs c;
    c.a1 = 1.0F;
    c.a2 = 0.0F;
    c.a3 = 0.0F;
    c.m0 = 1.0F;
    c.m1 = 0.0F;
    c.m2 = 0.0F;
    c.g = 0.0F;
    c.k = 2.0F;
    return c;
}

// Simper's parameterisation of every section (Cytomic 2013, the "optimised
// 2" form): g prewarps the corner, k = 1 / Q damps it, and m0, m1, m2 mix
// the input, the band-pass state, and the low-pass state into the output.
NphAudioFilterCoeffs nph_audio_filter_design(NphAudioFilterType type, f32 freq, f32 q, f32 gain_db,
                                             f32 rate)
{
    if (!(rate > 0.0F) || (u32)type >= (u32)NPH_AUDIO_FILTER_COUNT) {
        return nph_audio_filter_passthrough();
    }
    const f64 nyquist_guard = 0.49 * (f64)rate;
    f64 f = (f64)freq;
    if (!(f > 0.0)) {
        f = 1e-3;
    }
    if (f > nyquist_guard) {
        f = nyquist_guard;
    }
    f64 qq = (f64)q;
    if (!(qq >= 0.025)) {
        qq = 0.025;
    }
    if (qq > 100.0) {
        qq = 100.0;
    }
    const f64 big_a = pow(10.0, (f64)gain_db / 40.0);
    f64 g = tan(FILTER_PI * f / (f64)rate);
    f64 k = 1.0 / qq;
    f64 m0 = 0.0;
    f64 m1 = 0.0;
    f64 m2 = 0.0;
    switch (type) {
    case NPH_AUDIO_FILTER_LOWPASS:
        m2 = 1.0;
        break;
    case NPH_AUDIO_FILTER_HIGHPASS:
        m0 = 1.0;
        m1 = -k;
        m2 = -1.0;
        break;
    case NPH_AUDIO_FILTER_BANDPASS:
        m1 = k;
        break;
    case NPH_AUDIO_FILTER_NOTCH:
        m0 = 1.0;
        m1 = -k;
        break;
    case NPH_AUDIO_FILTER_ALLPASS:
        m0 = 1.0;
        m1 = -2.0 * k;
        break;
    case NPH_AUDIO_FILTER_PEAK:
        k = 1.0 / (qq * big_a);
        m0 = 1.0;
        m1 = k * (big_a * big_a - 1.0);
        break;
    case NPH_AUDIO_FILTER_LOW_SHELF:
        g /= sqrt(big_a);
        m0 = 1.0;
        m1 = k * (big_a - 1.0);
        m2 = big_a * big_a - 1.0;
        break;
    case NPH_AUDIO_FILTER_HIGH_SHELF:
        g *= sqrt(big_a);
        m0 = big_a * big_a;
        m1 = k * (1.0 - big_a) * big_a;
        m2 = 1.0 - big_a * big_a;
        break;
    case NPH_AUDIO_FILTER_COUNT:
        break;
    }
    const f64 a1 = 1.0 / (1.0 + g * (g + k));
    const f64 a2 = g * a1;
    const f64 a3 = g * a2;
    NphAudioFilterCoeffs c;
    c.a1 = (f32)a1;
    c.a2 = (f32)a2;
    c.a3 = (f32)a3;
    c.m0 = (f32)m0;
    c.m1 = (f32)m1;
    c.m2 = (f32)m2;
    c.g = (f32)g;
    c.k = (f32)k;
    return c;
}

void nph_audio_filter_process(const NphAudioFilterCoeffs *coeffs, NphAudioFilterState *state,
                              const f32 *in, f32 *out, u32 count)
{
    const f32 a1 = coeffs->a1;
    const f32 a2 = coeffs->a2;
    const f32 a3 = coeffs->a3;
    const f32 m0 = coeffs->m0;
    const f32 m1 = coeffs->m1;
    const f32 m2 = coeffs->m2;
    f32 ic1 = state->ic1eq;
    f32 ic2 = state->ic2eq;
    for (u32 i = 0U; i < count; ++i) {
        const f32 v0 = in[i];
        const f32 v3 = v0 - ic2;
        const f32 v1 = a1 * ic1 + a2 * v3;
        const f32 v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0F * v1 - ic1;
        ic2 = 2.0F * v2 - ic2;
        out[i] = m0 * v0 + m1 * v1 + m2 * v2;
    }
    state->ic1eq = ic1;
    state->ic2eq = ic2;
}

void nph_audio_filter_process_glide(const NphAudioFilterCoeffs *from,
                                    const NphAudioFilterCoeffs *to, NphAudioFilterState *state,
                                    const f32 *in, f32 *out, u32 count)
{
    if (count == 0U || !(from->g > 0.0F) || !(to->g > 0.0F)) {
        nph_audio_filter_process(to, state, in, out, count);
        return;
    }
    // The cutoff moves in log frequency: a constant ratio per sample (the
    // root in double precision; the products drift by an ulp or so a step,
    // and the next block starts again from `to` exactly).
    const f32 ratio = (f32)pow((f64)to->g / (f64)from->g, 1.0 / (f64)count);
    const f32 step = 1.0F / (f32)count;
    const f32 dk = (to->k - from->k) * step;
    const f32 dm0 = (to->m0 - from->m0) * step;
    const f32 dm1 = (to->m1 - from->m1) * step;
    const f32 dm2 = (to->m2 - from->m2) * step;
    f32 g = from->g;
    f32 k = from->k;
    f32 m0 = from->m0;
    f32 m1 = from->m1;
    f32 m2 = from->m2;
    f32 ic1 = state->ic1eq;
    f32 ic2 = state->ic2eq;
    for (u32 i = 0U; i < count; ++i) {
        g *= ratio;
        k += dk;
        m0 += dm0;
        m1 += dm1;
        m2 += dm2;
        const f32 a1 = 1.0F / (1.0F + g * (g + k));
        const f32 a2 = g * a1;
        const f32 a3 = g * a2;
        const f32 v0 = in[i];
        const f32 v3 = v0 - ic2;
        const f32 v1 = a1 * ic1 + a2 * v3;
        const f32 v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0F * v1 - ic1;
        ic2 = 2.0F * v2 - ic2;
        out[i] = m0 * v0 + m1 * v1 + m2 * v2;
    }
    state->ic1eq = ic1;
    state->ic2eq = ic2;
}

// Complex arithmetic for the transfer function (MSVC's C has no _Complex).
typedef struct FilterComplex {
    f64 re, im;
} FilterComplex;

static FilterComplex fc_make(f64 re, f64 im)
{
    FilterComplex z;
    z.re = re;
    z.im = im;
    return z;
}

static FilterComplex fc_mul(FilterComplex a, FilterComplex b)
{
    return fc_make(a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re);
}

static FilterComplex fc_div(FilterComplex a, FilterComplex b)
{
    const f64 d = b.re * b.re + b.im * b.im;
    return fc_make((a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d);
}

f64 nph_audio_filter_magnitude(const NphAudioFilterCoeffs *coeffs, f64 freq, f64 rate)
{
    // The integrator outputs as transfer functions of z^-1 = e^(-jw): the
    // bilinear transform of 1/(s^2 + k s + 1) (low) and s/(s^2 + k s + 1)
    // (band), with s = (1/g)(1 - z^-1)/(1 + z^-1).
    const f64 g = (f64)coeffs->g;
    const f64 k = (f64)coeffs->k;
    if (!(g > 0.0)) {
        return fabs((f64)coeffs->m0); // the pass-through: integrators at rest
    }
    const f64 w = 2.0 * FILTER_PI * freq / rate;
    const FilterComplex z1 = fc_make(cos(w), -sin(w)); // z^-1
    const FilterComplex z2 = fc_mul(z1, z1);           // z^-2
    const FilterComplex den =
        fc_make(1.0 + g * k + g * g + 2.0 * (g * g - 1.0) * z1.re + (1.0 - g * k + g * g) * z2.re,
                2.0 * (g * g - 1.0) * z1.im + (1.0 - g * k + g * g) * z2.im);
    const FilterComplex one_plus = fc_make(1.0 + z1.re, z1.im);
    const FilterComplex low_num = fc_mul(fc_make(g * g, 0.0), fc_mul(one_plus, one_plus));
    const FilterComplex band_num = fc_make(g * (1.0 - z2.re), -g * z2.im);
    const FilterComplex low = fc_div(low_num, den);
    const FilterComplex band = fc_div(band_num, den);
    const f64 m0 = (f64)coeffs->m0;
    const f64 m1 = (f64)coeffs->m1;
    const f64 m2 = (f64)coeffs->m2;
    const FilterComplex h = fc_make(m0 + m1 * band.re + m2 * low.re, m1 * band.im + m2 * low.im);
    return sqrt(h.re * h.re + h.im * h.im);
}

f32 nph_audio_one_pole_coeff(f32 freq, f32 rate)
{
    if (!(rate > 0.0F) || !(freq > 0.0F)) {
        return 0.0F;
    }
    const f64 f = (f64)freq > 0.5 * (f64)rate ? 0.5 * (f64)rate : (f64)freq;
    return (f32)(1.0 - exp(-2.0 * FILTER_PI * f / (f64)rate));
}

void nph_audio_one_pole_process(f32 a, f32 *state, const f32 *in, f32 *out, u32 count)
{
    f32 y = *state;
    for (u32 i = 0U; i < count; ++i) {
        y += a * (in[i] - y);
        out[i] = y;
    }
    *state = y;
}
