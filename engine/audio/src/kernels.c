// SPDX-License-Identifier: Apache-2.0
// The kernel tables' selection, and the scalar reference: the plain loops
// the SIMD tables are tested against (kernels.h).

#include <nepheline/audio/kernels.h>
#include <nepheline/audio/resampler.h>

#include "audio_internal.h"

#include <math.h>
#include <string.h>

static void scalar_mix(f32 *dst, const f32 *src, u32 count, f32 gain)
{
    for (u32 i = 0U; i < count; ++i) {
        dst[i] += src[i] * gain;
    }
}

static void scalar_mix_ramp(f32 *dst, const f32 *src, u32 count, f32 g0, f32 g1)
{
    if (count == 0U) {
        return;
    }
    const f32 dg = (g1 - g0) / (f32)count;
    for (u32 i = 0U; i < count; ++i) {
        dst[i] += src[i] * (g0 + (f32)i * dg);
    }
}

static void scalar_scale_ramp(f32 *dst, u32 count, f32 g0, f32 g1)
{
    if (count == 0U) {
        return;
    }
    const f32 dg = (g1 - g0) / (f32)count;
    for (u32 i = 0U; i < count; ++i) {
        dst[i] *= g0 + (f32)i * dg;
    }
}

static f32 scalar_peak(const f32 *src, u32 count)
{
    f32 best = 0.0F;
    for (u32 i = 0U; i < count; ++i) {
        const f32 a = fabsf(src[i]);
        if (a > best) {
            best = a;
        }
    }
    return best;
}

static f32 scalar_sum_squares(const f32 *src, u32 count)
{
    f32 sum = 0.0F;
    for (u32 i = 0U; i < count; ++i) {
        sum += src[i] * src[i];
    }
    return sum;
}

static void scalar_load_s16(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    for (u32 i = 0U; i < count; ++i) {
        i16 v;
        memcpy(&v, p + (u64)i * stride * 2U, sizeof v);
        out[i] = (f32)v / 32768.0F;
    }
}

static void scalar_load_s24(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    for (u32 i = 0U; i < count; ++i) {
        const u8 *s = p + (u64)i * stride * 3U;
        i32 v = (i32)((u32)s[0] | ((u32)s[1] << 8U) | ((u32)s[2] << 16U));
        if (v >= 0x800000) {
            v -= 0x1000000;
        }
        out[i] = (f32)v / 8388608.0F;
    }
}

static void scalar_load_s32(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    for (u32 i = 0U; i < count; ++i) {
        i32 v;
        memcpy(&v, p + (u64)i * stride * 4U, sizeof v);
        out[i] = (f32)((f64)v / 2147483648.0);
    }
}

static void scalar_load_f32(const void *in, u32 stride, f32 *out, u32 count)
{
    const u8 *p = (const u8 *)in;
    for (u32 i = 0U; i < count; ++i) {
        memcpy(&out[i], p + (u64)i * stride * 4U, sizeof(f32));
    }
}

static f32 scalar_sanitize(f32 x)
{
    if (isnan(x)) {
        return 0.0F;
    }
    return fminf(fmaxf(x, -1.0F), 1.0F);
}

static void scalar_store_f32(const f32 *const *planar, u32 channels, f32 *out, u32 count)
{
    for (u32 i = 0U; i < count; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            out[(u64)i * channels + c] = scalar_sanitize(planar[c][i]);
        }
    }
}

static void scalar_store_s16(const f32 *const *planar, u32 channels, i16 *out, u32 count)
{
    for (u32 i = 0U; i < count; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            out[(u64)i * channels + c] = (i16)nearbyintf(scalar_sanitize(planar[c][i]) * 32767.0F);
        }
    }
}

// The textbook form: each tap's coefficient by Horner's rule, then the dot
// product, one channel at a time.
static void scalar_resample(const NphAudioResampleBank *bank, const f32 *const *src, u32 channels,
                            u64 pos, u64 step, f32 *const *dst, u32 count)
{
    const u32 taps = bank->taps;
    const u32 fraction_bits = 32U - NPH_AUDIO_RESAMPLE_PHASE_BITS;
    const u32 fraction_mask = (1U << fraction_bits) - 1U;
    for (u32 n = 0U; n < count; ++n, pos += step) {
        const u64 index = pos >> 32U;
        const u32 frac = (u32)pos;
        const u32 segment = frac >> fraction_bits;
        const f32 u = (f32)(frac & fraction_mask) / (f32)(1U << fraction_bits);
        const f32 *rows = bank->coeffs + (u64)segment * 4U * taps;
        for (u32 c = 0U; c < channels; ++c) {
            const f32 *x = src[c] + index - (taps / 2U - 1U);
            f32 acc = 0.0F;
            for (u32 j = 0U; j < taps; ++j) {
                const f32 k = rows[j] + u * (rows[taps + j] +
                                             u * (rows[2U * taps + j] + u * rows[3U * taps + j]));
                acc += k * x[j];
            }
            dst[c][n] = acc;
        }
    }
}

const NphAudioKernels g_audio_kernels_scalar = {
    NPH_SIMD_LEVEL_SCALAR, "scalar",           scalar_mix,       scalar_mix_ramp, scalar_scale_ramp,
    scalar_peak,           scalar_sum_squares, scalar_load_s16,  scalar_load_s24, scalar_load_s32,
    scalar_load_f32,       scalar_store_f32,   scalar_store_s16, scalar_resample,
};

const NphAudioKernels *nph_audio_kernels(NphSimdLevel level)
{
    switch (level) {
    case NPH_SIMD_LEVEL_SSE4:
        return &g_audio_kernels_sse4;
    case NPH_SIMD_LEVEL_AVX2:
        return &g_audio_kernels_avx2;
    case NPH_SIMD_LEVEL_SCALAR:
    case NPH_SIMD_LEVEL_COUNT:
        break;
    }
    return &g_audio_kernels_scalar;
}
