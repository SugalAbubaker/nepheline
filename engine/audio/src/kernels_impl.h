// SPDX-License-Identifier: Apache-2.0
// The audio block kernels, compiled once per ISA level (kernels_sse4.c,
// kernels_avx2.c). The including unit defines AK(name); wide.h resolves
// NphF32x8 to one AVX register or two SSE ones from that unit's flags.
// Every lane does the same arithmetic in the same order at both levels,
// and the horizontal sums reduce the same halves in the same order, so the
// two tables agree to the bit (kernels.h).

#include <nepheline/audio/kernels.h>
#include <nepheline/audio/resampler.h>
#include <nepheline/math/wide.h>

#include <math.h>
#include <string.h>

#ifndef AK
#error "kernels_impl.h: define AK(name) before including"
#endif

typedef NphF32x8 AkW;

// The ramp's per-element gains for elements base .. base + 7.
static AkW AK(ramp_gains)(f32 g0, f32 dg, u32 base)
{
    const AkW lane =
        nph_f32x8_from_x4(_mm_setr_ps(0.0F, 1.0F, 2.0F, 3.0F), _mm_setr_ps(4.0F, 5.0F, 6.0F, 7.0F));
    const AkW index = nph_f32x8_add(nph_f32x8_splat((f32)base), lane);
    return nph_f32x8_add(nph_f32x8_splat(g0), nph_f32x8_mul(index, nph_f32x8_splat(dg)));
}

static void AK(mix)(f32 *dst, const f32 *src, u32 count, f32 gain)
{
    const AkW g = nph_f32x8_splat(gain);
    u32 i = 0U;
    for (; i + 8U <= count; i += 8U) {
        const AkW d = nph_f32x8_load(dst + i);
        nph_f32x8_store(dst + i, nph_f32x8_add(d, nph_f32x8_mul(nph_f32x8_load(src + i), g)));
    }
    for (; i < count; ++i) {
        dst[i] = dst[i] + src[i] * gain;
    }
}

static void AK(mix_ramp)(f32 *dst, const f32 *src, u32 count, f32 g0, f32 g1)
{
    if (count == 0U) {
        return;
    }
    const f32 dg = (g1 - g0) / (f32)count;
    u32 i = 0U;
    for (; i + 8U <= count; i += 8U) {
        const AkW g = AK(ramp_gains)(g0, dg, i);
        const AkW d = nph_f32x8_load(dst + i);
        nph_f32x8_store(dst + i, nph_f32x8_add(d, nph_f32x8_mul(nph_f32x8_load(src + i), g)));
    }
    for (; i < count; ++i) {
        const f32 g = g0 + (f32)i * dg;
        dst[i] = dst[i] + src[i] * g;
    }
}

static void AK(scale_ramp)(f32 *dst, u32 count, f32 g0, f32 g1)
{
    if (count == 0U) {
        return;
    }
    const f32 dg = (g1 - g0) / (f32)count;
    u32 i = 0U;
    for (; i + 8U <= count; i += 8U) {
        const AkW g = AK(ramp_gains)(g0, dg, i);
        nph_f32x8_store(dst + i, nph_f32x8_mul(nph_f32x8_load(dst + i), g));
    }
    for (; i < count; ++i) {
        const f32 g = g0 + (f32)i * dg;
        dst[i] = dst[i] * g;
    }
}

// The sum of the eight lanes: halves first, then pairs, the same sequence
// at both levels.
static f32 AK(hsum)(AkW v)
{
    NphF32x4 s = _mm_add_ps(nph_f32x8_lo(v), nph_f32x8_hi(v));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}

static f32 AK(hmax)(AkW v)
{
    NphF32x4 s = _mm_max_ps(nph_f32x8_lo(v), nph_f32x8_hi(v));
    s = _mm_max_ps(s, _mm_movehl_ps(s, s));
    s = _mm_max_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}

static f32 AK(peak)(const f32 *src, u32 count)
{
    // max(x, m) with m second keeps m when x is NaN (MAXPS returns its
    // second operand when either is NaN), so NaNs never win.
    AkW m = nph_f32x8_zero();
    u32 i = 0U;
    for (; i + 8U <= count; i += 8U) {
        m = nph_f32x8_max(nph_f32x8_abs(nph_f32x8_load(src + i)), m);
    }
    f32 best = AK(hmax)(m);
    for (; i < count; ++i) {
        const f32 a = fabsf(src[i]);
        if (a > best) {
            best = a;
        }
    }
    return best;
}

static f32 AK(sum_squares)(const f32 *src, u32 count)
{
    AkW acc = nph_f32x8_zero();
    u32 i = 0U;
    for (; i + 8U <= count; i += 8U) {
        const AkW x = nph_f32x8_load(src + i);
        acc = nph_f32x8_add(acc, nph_f32x8_mul(x, x));
    }
    f32 sum = AK(hsum)(acc);
    for (; i < count; ++i) {
        sum += src[i] * src[i];
    }
    return sum;
}

// The loads read samples through memcpy, so any alignment and either
// endianness of the host's loads is fine (x86-64 is little-endian, like
// the formats).

static void AK(load_s16)(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    const u64 step = (u64)stride * 2U;
    for (u32 i = 0U; i < count; ++i) {
        i16 v;
        memcpy(&v, p + (u64)i * step, sizeof v);
        out[i] = (f32)v * (1.0F / 32768.0F);
    }
}

static void AK(load_s24)(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    const u64 step = (u64)stride * 3U;
    for (u32 i = 0U; i < count; ++i) {
        const u8 *s = p + (u64)i * step;
        const u32 raw = (u32)s[0] | ((u32)s[1] << 8U) | ((u32)s[2] << 16U);
        // Sign-extend bit 23.
        const i32 v = (i32)(raw ^ 0x800000U) - 0x800000;
        out[i] = (f32)v * (1.0F / 8388608.0F);
    }
}

static void AK(load_s32)(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    const u64 step = (u64)stride * 4U;
    for (u32 i = 0U; i < count; ++i) {
        i32 v;
        memcpy(&v, p + (u64)i * step, sizeof v);
        out[i] = (f32)((f64)v * (1.0 / 2147483648.0));
    }
}

static void AK(load_f32)(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    const u64 step = (u64)stride * 4U;
    if (stride == 1U) {
        memcpy(out, in, (size_t)count * sizeof(f32));
        return;
    }
    for (u32 i = 0U; i < count; ++i) {
        memcpy(&out[i], p + (u64)i * step, sizeof(f32));
    }
}

// Clamps to [-1, 1] with NaN mapped to 0: MAXPS(x, -1) gives -1 for a NaN
// x, so the NaN test comes first.
static f32 AK(sanitize)(f32 x)
{
    if (x != x) {
        return 0.0F;
    }
    return x < -1.0F ? -1.0F : (x > 1.0F ? 1.0F : x);
}

static AkW AK(sanitize8)(AkW x)
{
    const AkW ordered = nph_f32x8_cmp_eq(x, x); // false lanes are NaN
    const AkW clamped =
        nph_f32x8_min(nph_f32x8_max(x, nph_f32x8_splat(-1.0F)), nph_f32x8_splat(1.0F));
    return nph_f32x8_and(clamped, ordered);
}

static void AK(store_f32)(const f32 *const *planar, u32 channels, f32 *out, u32 count)
{
    if (channels == 1U) {
        u32 i = 0U;
        for (; i + 8U <= count; i += 8U) {
            nph_f32x8_store(out + i, AK(sanitize8)(nph_f32x8_load(planar[0] + i)));
        }
        for (; i < count; ++i) {
            out[i] = AK(sanitize)(planar[0][i]);
        }
        return;
    }
    if (channels == 2U) {
        u32 i = 0U;
        for (; i + 4U <= count; i += 4U) {
            const NphF32x4 l = _mm_loadu_ps(planar[0] + i);
            const NphF32x4 r = _mm_loadu_ps(planar[1] + i);
            const AkW lr =
                AK(sanitize8)(nph_f32x8_from_x4(_mm_unpacklo_ps(l, r), _mm_unpackhi_ps(l, r)));
            nph_f32x8_store(out + (u64)i * 2U, lr);
        }
        for (; i < count; ++i) {
            out[(u64)i * 2U] = AK(sanitize)(planar[0][i]);
            out[(u64)i * 2U + 1U] = AK(sanitize)(planar[1][i]);
        }
        return;
    }
    for (u32 i = 0U; i < count; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            out[(u64)i * channels + c] = AK(sanitize)(planar[c][i]);
        }
    }
}

static i16 AK(to_s16)(f32 x)
{
    // nearbyintf rounds to nearest even under the default rounding mode.
    const f32 s = nearbyintf(AK(sanitize)(x) * 32767.0F);
    return (i16)s;
}

static void AK(store_s16)(const f32 *const *planar, u32 channels, i16 *out, u32 count)
{
    for (u32 i = 0U; i < count; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            out[(u64)i * channels + c] = AK(to_s16)(planar[c][i]);
        }
    }
}

// The four partial sums of one output (one per power of the position in
// the segment), reduced to one value by Horner's rule.
static f32 AK(finish)(AkW a0, AkW a1, AkW a2, AkW a3, f32 u)
{
    NphF32x4 s0 = _mm_add_ps(nph_f32x8_lo(a0), nph_f32x8_hi(a0));
    NphF32x4 s1 = _mm_add_ps(nph_f32x8_lo(a1), nph_f32x8_hi(a1));
    NphF32x4 s2 = _mm_add_ps(nph_f32x8_lo(a2), nph_f32x8_hi(a2));
    NphF32x4 s3 = _mm_add_ps(nph_f32x8_lo(a3), nph_f32x8_hi(a3));
    _MM_TRANSPOSE4_PS(s0, s1, s2, s3);
    // Lane p of `sum` is the full sum for power p.
    const NphF32x4 sum = _mm_add_ps(_mm_add_ps(s0, s1), _mm_add_ps(s2, s3));
    f32 c[4];
    _mm_storeu_ps(c, sum);
    return c[0] + u * (c[1] + u * (c[2] + u * c[3]));
}

#define AK_FRACTION_BITS (32U - NPH_AUDIO_RESAMPLE_PHASE_BITS)
#define AK_FRACTION_MASK ((1U << AK_FRACTION_BITS) - 1U)

static void AK(resample)(const NphAudioResampleBank *bank, const f32 *const *src, u32 channels,
                         u64 pos, u64 step, f32 *const *dst, u32 count)
{
    const u32 taps = bank->taps;
    const u64 half = (u64)(taps / 2U - 1U);
    const u64 rows = 4U * (u64)taps;
    const f32 inv_segment = 1.0F / (f32)(1U << AK_FRACTION_BITS);
    for (u32 n = 0U; n < count; ++n, pos += step) {
        const u64 index = pos >> 32U;
        const u32 frac = (u32)pos;
        const u32 segment = frac >> AK_FRACTION_BITS;
        const f32 u = (f32)(frac & AK_FRACTION_MASK) * inv_segment;
        const f32 *c0 = bank->coeffs + (u64)segment * rows;
        const f32 *c1 = c0 + taps;
        const f32 *c2 = c1 + taps;
        const f32 *c3 = c2 + taps;
        u32 ch = 0U;
#if NPH_WIDE_AVX
        // Channel pairs share the coefficient loads. Only with AVX: the
        // pair's eight accumulators are sixteen SSE registers, which spill
        // (40.9 ns a stereo frame against 32.5 one channel at a time).
        // A channel's sums are the same either way, so the levels still
        // agree to the bit.
        for (; ch + 2U <= channels; ch += 2U) {
            const f32 *x = src[ch] + index - half;
            const f32 *y = src[ch + 1U] + index - half;
            AkW xa0 = nph_f32x8_zero();
            AkW xa1 = nph_f32x8_zero();
            AkW xa2 = nph_f32x8_zero();
            AkW xa3 = nph_f32x8_zero();
            AkW ya0 = nph_f32x8_zero();
            AkW ya1 = nph_f32x8_zero();
            AkW ya2 = nph_f32x8_zero();
            AkW ya3 = nph_f32x8_zero();
            for (u32 j = 0U; j < taps; j += 8U) {
                const AkW k0 = nph_f32x8_load(c0 + j);
                const AkW k1 = nph_f32x8_load(c1 + j);
                const AkW k2 = nph_f32x8_load(c2 + j);
                const AkW k3 = nph_f32x8_load(c3 + j);
                const AkW sx = nph_f32x8_load(x + j);
                const AkW sy = nph_f32x8_load(y + j);
                xa0 = nph_f32x8_add(xa0, nph_f32x8_mul(k0, sx));
                xa1 = nph_f32x8_add(xa1, nph_f32x8_mul(k1, sx));
                xa2 = nph_f32x8_add(xa2, nph_f32x8_mul(k2, sx));
                xa3 = nph_f32x8_add(xa3, nph_f32x8_mul(k3, sx));
                ya0 = nph_f32x8_add(ya0, nph_f32x8_mul(k0, sy));
                ya1 = nph_f32x8_add(ya1, nph_f32x8_mul(k1, sy));
                ya2 = nph_f32x8_add(ya2, nph_f32x8_mul(k2, sy));
                ya3 = nph_f32x8_add(ya3, nph_f32x8_mul(k3, sy));
            }
            dst[ch][n] = AK(finish)(xa0, xa1, xa2, xa3, u);
            dst[ch + 1U][n] = AK(finish)(ya0, ya1, ya2, ya3, u);
        }
#endif
        for (; ch < channels; ++ch) {
            const f32 *x = src[ch] + index - half;
            AkW a0 = nph_f32x8_zero();
            AkW a1 = nph_f32x8_zero();
            AkW a2 = nph_f32x8_zero();
            AkW a3 = nph_f32x8_zero();
            for (u32 j = 0U; j < taps; j += 8U) {
                const AkW s = nph_f32x8_load(x + j);
                a0 = nph_f32x8_add(a0, nph_f32x8_mul(nph_f32x8_load(c0 + j), s));
                a1 = nph_f32x8_add(a1, nph_f32x8_mul(nph_f32x8_load(c1 + j), s));
                a2 = nph_f32x8_add(a2, nph_f32x8_mul(nph_f32x8_load(c2 + j), s));
                a3 = nph_f32x8_add(a3, nph_f32x8_mul(nph_f32x8_load(c3 + j), s));
            }
            dst[ch][n] = AK(finish)(a0, a1, a2, a3, u);
        }
    }
}

#define AK_TABLE(level_value, level_name)                                                          \
    {                                                                                              \
        (level_value), (level_name),    AK(mix),       AK(mix_ramp), AK(scale_ramp),               \
        AK(peak),      AK(sum_squares), AK(load_s16),  AK(load_s24), AK(load_s32),                 \
        AK(load_f32),  AK(store_f32),   AK(store_s16), AK(resample),                               \
    }
