// SPDX-License-Identifier: Apache-2.0
// The resampler's kernel banks (resampler.h): the Kaiser-windowed sinc per
// cutoff scale, tabulated as cubic segments.

#include <nepheline/audio/resampler.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

// Kaiser's beta for a 100 dB stopband: 0.1102 (A - 8.7) (Kaiser 1974;
// Oppenheim & Schafer, "Discrete-Time Signal Processing", 3rd ed., §7.6).
#define RS_KAISER_BETA 10.06126

#define RS_PI 3.14159265358979323846

struct NphAudioResampler {
    NphAllocator *alloc;
    f32 *coeffs;
    u64 coeff_count;
    // The largest step (32.32) each bank serves without aliasing:
    // floor(2^(k/12) * 2^32), so a step on a boundary takes the next bank.
    u64 max_step[NPH_AUDIO_RESAMPLE_BANKS];
    NphAudioResampleBank banks[NPH_AUDIO_RESAMPLE_BANKS];
};

// The modified Bessel function of the first kind, order 0, by its power
// series (Abramowitz & Stegun 9.6.12); converges for every argument the
// window takes (0 .. beta).
static f64 rs_bessel_i0(f64 x)
{
    const f64 q = 0.25 * x * x;
    f64 sum = 1.0;
    f64 term = 1.0;
    for (u32 k = 1U; k < 200U; ++k) {
        term *= q / ((f64)k * (f64)k);
        sum += term;
        if (term < 1e-17 * sum) {
            break;
        }
    }
    return sum;
}

f64 nph_audio_resample_kernel(f64 t, f64 scale)
{
    if (!(scale > 0.0) || scale > 1.0) {
        return 0.0;
    }
    const f64 half = (f64)NPH_AUDIO_RESAMPLE_ZERO_CROSSINGS / scale;
    if (!(fabs(t) < half)) {
        return 0.0;
    }
    const f64 r = t / half;
    const f64 window =
        rs_bessel_i0(RS_KAISER_BETA * sqrt(1.0 - r * r)) / rs_bessel_i0(RS_KAISER_BETA);
    const f64 x = RS_PI * scale * t;
    const f64 sinc = x == 0.0 ? 1.0 : sin(x) / x;
    return scale * sinc * window;
}

static u32 rs_bank_taps(f64 scale)
{
    const f64 width = 2.0 * (f64)NPH_AUDIO_RESAMPLE_ZERO_CROSSINGS / scale;
    const u32 taps = (u32)ceil(width - 1e-9);
    return nph_align_up_u32(taps, 8U);
}

static f64 rs_bank_scale(u32 k)
{
    return pow(2.0, -(f64)k / 12.0);
}

// The monomial coefficients of the cubic through (u_k, y_k) at the four
// Chebyshev nodes of [0, 1]: inv is the inverse of their Vandermonde matrix
// (inv[p][k] maps y_k to the coefficient of u^p).
static void rs_inverse_vandermonde(f64 inv[4][4], f64 nodes[4])
{
    f64 m[4][8];
    for (u32 k = 0U; k < 4U; ++k) {
        nodes[k] = 0.5 - 0.5 * cos(RS_PI * (2.0 * (f64)k + 1.0) / 8.0);
        f64 power = 1.0;
        for (u32 p = 0U; p < 4U; ++p) {
            m[k][p] = power;
            m[k][4U + p] = k == p ? 1.0 : 0.0;
            power *= nodes[k];
        }
    }
    // Gauss-Jordan with partial pivoting on [V | I].
    for (u32 c = 0U; c < 4U; ++c) {
        u32 pivot = c;
        for (u32 r = c + 1U; r < 4U; ++r) {
            if (fabs(m[r][c]) > fabs(m[pivot][c])) {
                pivot = r;
            }
        }
        if (pivot != c) {
            for (u32 k = 0U; k < 8U; ++k) {
                const f64 tmp = m[c][k];
                m[c][k] = m[pivot][k];
                m[pivot][k] = tmp;
            }
        }
        const f64 d = m[c][c];
        for (u32 k = 0U; k < 8U; ++k) {
            m[c][k] /= d;
        }
        for (u32 r = 0U; r < 4U; ++r) {
            if (r != c) {
                const f64 f = m[r][c];
                for (u32 k = 0U; k < 8U; ++k) {
                    m[r][k] -= f * m[c][k];
                }
            }
        }
    }
    // V^-1 maps values to coefficients: coeff_p = sum_k inv[p][k] y_k.
    for (u32 p = 0U; p < 4U; ++p) {
        for (u32 k = 0U; k < 4U; ++k) {
            inv[p][k] = m[p][4U + k];
        }
    }
}

NphResult nph_audio_resampler_create(NphAllocator *alloc, NphAudioResampler **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    *out = NULL;
    NphAudioResampler *r = NPH_ALLOC_TYPE(alloc, NphAudioResampler);
    if (r == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    r->alloc = alloc;
    u64 total = 0U;
    for (u32 k = 0U; k < NPH_AUDIO_RESAMPLE_BANKS; ++k) {
        total += (u64)NPH_AUDIO_RESAMPLE_PHASES * 4U * rs_bank_taps(rs_bank_scale(k));
    }
    r->coeffs = (f32 *)NPH_ALLOC(alloc, total * sizeof(f32), 64U);
    if (r->coeffs == NULL) {
        NPH_FREE_TYPE(alloc, NphAudioResampler, r);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    r->coeff_count = total;

    f64 inv[4][4];
    f64 nodes[4];
    rs_inverse_vandermonde(inv, nodes);
    const f64 phases = (f64)NPH_AUDIO_RESAMPLE_PHASES;
    f32 *cursor = r->coeffs;
    for (u32 k = 0U; k < NPH_AUDIO_RESAMPLE_BANKS; ++k) {
        const f64 scale = rs_bank_scale(k);
        const u32 taps = rs_bank_taps(scale);
        const f64 centre = (f64)taps / 2.0 - 1.0; // taps is even
        for (u32 p = 0U; p < NPH_AUDIO_RESAMPLE_PHASES; ++p) {
            f32 *rows = cursor + (u64)p * 4U * taps;
            for (u32 j = 0U; j < taps; ++j) {
                f64 y[4];
                for (u32 n = 0U; n < 4U; ++n) {
                    const f64 frac = ((f64)p + nodes[n]) / phases;
                    y[n] = nph_audio_resample_kernel(frac + centre - (f64)j, scale);
                }
                for (u32 power = 0U; power < 4U; ++power) {
                    const f64 c = inv[power][0] * y[0] + inv[power][1] * y[1] +
                                  inv[power][2] * y[2] + inv[power][3] * y[3];
                    rows[(u64)power * taps + j] = (f32)c;
                }
            }
        }
        r->banks[k].coeffs = cursor;
        r->banks[k].taps = taps;
        r->banks[k].scale = (f32)scale;
        r->max_step[k] = (u64)floor(pow(2.0, (f64)k / 12.0) * (f64)NPH_AUDIO_FRAME_ONE);
        cursor += (u64)NPH_AUDIO_RESAMPLE_PHASES * 4U * taps;
    }
    NPH_ASSERT(cursor == r->coeffs + total);
    // Bank 0 serves every step up to one frame exactly.
    r->max_step[0] = NPH_AUDIO_FRAME_ONE;
    *out = r;
    return NPH_RESULT_OK;
}

void nph_audio_resampler_destroy(NphAudioResampler *resampler)
{
    if (resampler == NULL) {
        return;
    }
    NPH_FREE(resampler->alloc, resampler->coeffs, resampler->coeff_count * sizeof(f32), 64U);
    NPH_FREE_TYPE(resampler->alloc, NphAudioResampler, resampler);
}

const NphAudioResampleBank *nph_audio_resampler_bank(const NphAudioResampler *resampler, u64 step)
{
    NPH_ASSERT(resampler != NULL);
    for (u32 k = 0U; k < NPH_AUDIO_RESAMPLE_BANKS; ++k) {
        if (step <= resampler->max_step[k]) {
            return &resampler->banks[k];
        }
    }
    return &resampler->banks[NPH_AUDIO_RESAMPLE_BANKS - 1U];
}

const NphAudioResampleBank *nph_audio_resampler_bank_at(const NphAudioResampler *resampler,
                                                        u32 index)
{
    NPH_ASSERT(resampler != NULL);
    return index < NPH_AUDIO_RESAMPLE_BANKS ? &resampler->banks[index] : NULL;
}

u64 nph_audio_resample_step(f64 ratio)
{
    if (!(ratio > 0.0) || !isfinite(ratio)) {
        return 1U;
    }
    const f64 max = (f64)NPH_AUDIO_RESAMPLE_MAX_STEP;
    if (ratio >= max) {
        return (u64)NPH_AUDIO_RESAMPLE_MAX_STEP * NPH_AUDIO_FRAME_ONE;
    }
    const f64 fixed = floor(ratio * (f64)NPH_AUDIO_FRAME_ONE + 0.5);
    return fixed < 1.0 ? 1U : (u64)fixed;
}

void nph_audio_resample_span(u64 pos, u64 step, u32 count, u32 taps, i64 *out_first, i64 *out_last)
{
    NPH_ASSERT(out_first != NULL && out_last != NULL && taps >= 2U);
    const i64 first = (i64)(pos >> 32U) - (i64)(taps / 2U - 1U);
    *out_first = first;
    if (count == 0U) {
        *out_last = first - 1;
        return;
    }
    const u64 last_pos = pos + (u64)(count - 1U) * step;
    *out_last = (i64)(last_pos >> 32U) + (i64)(taps / 2U);
}
