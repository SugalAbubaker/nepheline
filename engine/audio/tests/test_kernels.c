// SPDX-License-Identifier: Apache-2.0
// The block kernels (kernels.h): each level against the scalar reference,
// SSE4.2 against AVX2 to the bit, and the edge cases (tails, NaNs,
// clamping, format extremes).

#include <nepheline/audio/kernels.h>
#include <nepheline/audio/resampler.h>

#include "test_audio_support.h"

#include <stdint.h>

// Lengths that exercise the vector body, the scalar tail, and both.
static const u32 g_lengths[] = {0U, 1U, 7U, 8U, 9U, 31U, 256U, 263U};

NTEST(audio_kernels, tables_name_their_level)
{
    NTEST_ASSERT_EQ_STR(nph_audio_kernels(NPH_SIMD_LEVEL_SCALAR)->name, "scalar");
    NTEST_ASSERT_EQ_STR(nph_audio_kernels(NPH_SIMD_LEVEL_SSE4)->name, "sse4");
    NTEST_ASSERT_EQ_STR(nph_audio_kernels(NPH_SIMD_LEVEL_AVX2)->name, "avx2");
    NTEST_ASSERT_EQ_STR(nph_audio_kernels(NPH_SIMD_LEVEL_COUNT)->name, "scalar");
    NTEST_ASSERT_EQ_INT(nph_audio_kernels_best()->level, nph_simd_detect_level());
}

// mix, mix_ramp, and scale_ramp do one multiply and one add per element in
// the same order at every level, so all levels agree to the bit.
NTEST(audio_kernels, mixing_agrees_to_the_bit_at_every_level)
{
    NphSimdLevel levels[NPH_SIMD_LEVEL_COUNT];
    const u32 nlevels = test_simd_levels(levels);
    f32 *src = test_buffer(300U);
    f32 *ref = test_buffer(300U);
    f32 *out = test_buffer(300U);
    for (u32 li = 0U; li < NPH_ARRAY_COUNT(g_lengths); ++li) {
        const u32 n = g_lengths[li];
        for (u32 op = 0U; op < 3U; ++op) {
            test_noise(src, 300U, 11U + li, 1.0F);
            test_noise(ref, 300U, 97U + li, 0.5F);
            for (u32 l = 0U; l < nlevels; ++l) {
                const NphAudioKernels *k = nph_audio_kernels(levels[l]);
                test_noise(out, 300U, 97U + li, 0.5F);
                if (op == 0U) {
                    k->mix(out, src + 1U, n, 0.37F);
                } else if (op == 1U) {
                    k->mix_ramp(out, src + 1U, n, 0.9F, -0.25F);
                } else {
                    k->scale_ramp(out + 3U, n, 1.0F, 0.0F);
                }
                if (l == 0U) {
                    memcpy(ref, out, 300U * sizeof(f32));
                } else {
                    NTEST_ASSERT_TRUE(test_same_bits(out, ref, 300U));
                }
            }
        }
    }
    test_buffer_free(src, 300U);
    test_buffer_free(ref, 300U);
    test_buffer_free(out, 300U);
}

NTEST(audio_kernels, ramps_continue_across_blocks)
{
    const NphAudioKernels *k = nph_audio_kernels_best();
    f32 ones[512];
    for (u32 i = 0U; i < 512U; ++i) {
        ones[i] = 1.0F;
    }
    f32 out[512] = {0.0F};
    // Two blocks: 0 -> 1, then 1 -> 0.5. The gains step evenly within each
    // block and the second starts where the first was heading.
    k->mix_ramp(out, ones, 256U, 0.0F, 1.0F);
    k->mix_ramp(out + 256, ones, 256U, 1.0F, 0.5F);
    NTEST_ASSERT_NEAR_F32(out[0], 0.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(out[255], 255.0F / 256.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(out[256], 1.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(out[511], 1.0F - 0.5F * 255.0F / 256.0F, 1e-6F);
    for (u32 i = 1U; i < 512U; ++i) {
        NTEST_ASSERT_TRUE(fabsf(out[i] - out[i - 1U]) <= 1.0F / 256.0F + 1e-6F);
    }
}

NTEST(audio_kernels, peak_and_energy)
{
    NphSimdLevel levels[NPH_SIMD_LEVEL_COUNT];
    const u32 nlevels = test_simd_levels(levels);
    f32 x[263];
    test_noise(x, 263U, 5U, 0.75F);
    x[100] = -0.99F;
    for (u32 li = 0U; li < NPH_ARRAY_COUNT(g_lengths); ++li) {
        const u32 n = g_lengths[li];
        f64 ref_energy = 0.0;
        f32 ref_peak = 0.0F;
        for (u32 i = 0U; i < n; ++i) {
            ref_energy += (f64)x[i] * (f64)x[i];
            ref_peak = fabsf(x[i]) > ref_peak ? fabsf(x[i]) : ref_peak;
        }
        f32 sse_energy = 0.0F;
        for (u32 l = 0U; l < nlevels; ++l) {
            const NphAudioKernels *k = nph_audio_kernels(levels[l]);
            NTEST_ASSERT_TRUE(k->peak(x, n) == ref_peak);
            const f32 e = k->sum_squares(x, n);
            NTEST_ASSERT_NEAR_F64((f64)e, ref_energy, 1e-5 * (ref_energy + 1e-9));
            if (levels[l] == NPH_SIMD_LEVEL_SSE4) {
                sse_energy = e;
            } else if (levels[l] == NPH_SIMD_LEVEL_AVX2) {
                NTEST_ASSERT_TRUE(e == sse_energy);
            }
        }
    }
    // NaNs never win the peak.
    f32 y[19];
    test_noise(y, 19U, 9U, 0.5F);
    y[3] = NAN;
    y[17] = NAN;
    y[12] = 0.875F;
    for (u32 l = 0U; l < nlevels; ++l) {
        NTEST_ASSERT_TRUE(nph_audio_kernels(levels[l])->peak(y, 19U) == 0.875F);
    }
}

NTEST(audio_kernels, loads_scale_and_deinterleave)
{
    NphSimdLevel levels[NPH_SIMD_LEVEL_COUNT];
    const u32 nlevels = test_simd_levels(levels);
    // Interleaved stereo, extremes first.
    const i16 s16[8] = {-32768, 32767, 0, -1, 16384, -16384, 1, 2};
    u8 s24[12 + 1];
    const i32 s24_values[4] = {-8388608, 8388607, -1, 4194304};
    for (u32 i = 0U; i < 4U; ++i) {
        const u32 v = (u32)s24_values[i];
        s24[1U + i * 3U] = (u8)(v & 0xFFU);
        s24[2U + i * 3U] = (u8)((v >> 8U) & 0xFFU);
        s24[3U + i * 3U] = (u8)((v >> 16U) & 0xFFU);
    }
    const i32 s32[4] = {INT32_MIN, INT32_MAX, 0, -1073741824};
    const f32 f32s[4] = {0.25F, -0.5F, 1.5F, -2.0F};
    for (u32 l = 0U; l < nlevels; ++l) {
        const NphAudioKernels *k = nph_audio_kernels(levels[l]);
        f32 out[4];
        k->load_s16(s16, 2U, out, 4U); // left channel
        NTEST_ASSERT_TRUE(out[0] == -1.0F && out[1] == 0.0F && out[2] == 0.5F);
        NTEST_ASSERT_NEAR_F32(out[3], 1.0F / 32768.0F, 0.0F);
        k->load_s16(s16 + 1, 2U, out, 4U); // right channel
        NTEST_ASSERT_NEAR_F32(out[0], 32767.0F / 32768.0F, 0.0F);
        NTEST_ASSERT_NEAR_F32(out[1], -1.0F / 32768.0F, 0.0F);
        // A misaligned start: s24 begins at an odd address.
        k->load_s24(s24 + 1, 1U, out, 4U);
        NTEST_ASSERT_TRUE(out[0] == -1.0F);
        NTEST_ASSERT_NEAR_F32(out[1], 8388607.0F / 8388608.0F, 0.0F);
        NTEST_ASSERT_NEAR_F32(out[2], -1.0F / 8388608.0F, 0.0F);
        NTEST_ASSERT_TRUE(out[3] == 0.5F);
        k->load_s32(s32, 1U, out, 4U);
        NTEST_ASSERT_TRUE(out[0] == -1.0F && out[1] == 1.0F && out[2] == 0.0F && out[3] == -0.5F);
        k->load_f32(f32s, 1U, out, 4U);
        NTEST_ASSERT_TRUE(out[0] == 0.25F && out[3] == -2.0F); // floats are not clamped
        k->load_f32(f32s + 1, 2U, out, 2U);
        NTEST_ASSERT_TRUE(out[0] == -0.5F && out[1] == -2.0F);
    }
}

NTEST(audio_kernels, stores_interleave_clamp_and_sanitise)
{
    NphSimdLevel levels[NPH_SIMD_LEVEL_COUNT];
    const u32 nlevels = test_simd_levels(levels);
    enum {
        FRAMES = 21
    };
    f32 planes[8][FRAMES];
    for (u32 c = 0U; c < 8U; ++c) {
        test_noise(planes[c], FRAMES, 100U + c, 1.5F);
    }
    planes[0][3] = NAN;
    planes[1][4] = 7.0F;
    planes[1][5] = -0.0F;
    const f32 *ptrs[8];
    for (u32 c = 0U; c < 8U; ++c) {
        ptrs[c] = planes[c];
    }
    const u32 channel_counts[4] = {1U, 2U, 3U, 8U};
    for (u32 ci = 0U; ci < 4U; ++ci) {
        const u32 ch = channel_counts[ci];
        f32 ref[FRAMES * 8];
        i16 ref16[FRAMES * 8];
        for (u32 l = 0U; l < nlevels; ++l) {
            const NphAudioKernels *k = nph_audio_kernels(levels[l]);
            f32 out[FRAMES * 8];
            i16 out16[FRAMES * 8];
            k->store_f32(ptrs, ch, out, FRAMES);
            k->store_s16(ptrs, ch, out16, FRAMES);
            for (u32 i = 0U; i < (u32)FRAMES; ++i) {
                for (u32 c = 0U; c < ch; ++c) {
                    const f32 x = planes[c][i];
                    const f32 want = isnan(x) ? 0.0F : (x < -1.0F ? -1.0F : (x > 1.0F ? 1.0F : x));
                    NTEST_ASSERT_TRUE(out[i * ch + c] == want);
                }
            }
            if (l == 0U) {
                memcpy(ref, out, sizeof ref);
                memcpy(ref16, out16, sizeof ref16);
            } else {
                NTEST_ASSERT_TRUE(test_same_bits(out, ref, FRAMES * ch));
                NTEST_ASSERT_EQ_MEM(out16, ref16, sizeof(i16) * FRAMES * ch);
            }
        }
    }
    // s16 rounds to nearest and saturates.
    const f32 values[6] = {0.5F / 32767.0F, 1.5F / 32767.0F, 2.0F, -2.0F, -0.25F, NAN};
    const f32 *one[1] = {values};
    i16 got[6];
    nph_audio_kernels_best()->store_s16(one, 1U, got, 6U);
    NTEST_ASSERT_EQ_INT(got[0], 0); // a tie goes to even
    NTEST_ASSERT_EQ_INT(got[1], 2);
    NTEST_ASSERT_EQ_INT(got[2], 32767);
    NTEST_ASSERT_EQ_INT(got[3], -32767);
    NTEST_ASSERT_EQ_INT(got[4], -8192);
    NTEST_ASSERT_EQ_INT(got[5], 0);
}

// The resampler kernels: SSE4.2 and AVX2 agree to the bit; the scalar
// reference sums in another order and agrees to rounding.
NTEST(audio_kernels, resampling_agrees_across_levels)
{
    NphAudioResampler *rs = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_resampler_create(nph_allocator_system(), &rs), NPH_RESULT_OK);
    NphSimdLevel levels[NPH_SIMD_LEVEL_COUNT];
    const u32 nlevels = test_simd_levels(levels);
    enum {
        SRC = 2048,
        OUT = 300,
        PAD = NPH_AUDIO_RESAMPLE_MAX_TAPS
    };
    f32 *src[8];
    f32 *ref[8];
    f32 *out[8];
    f32 *sse[8];
    for (u32 c = 0U; c < 8U; ++c) {
        src[c] = test_buffer(SRC + 2U * PAD);
        test_noise(src[c], SRC + 2U * PAD, 31U * c + 7U, 0.9F);
        ref[c] = test_buffer(OUT);
        out[c] = test_buffer(OUT);
        sse[c] = test_buffer(OUT);
    }
    const f64 ratios[5] = {1.0, 0.918750, 1.088435, 2.5, 3.99};
    const u32 channel_counts[4] = {1U, 2U, 3U, 8U};
    for (u32 ri = 0U; ri < 5U; ++ri) {
        const u64 step = nph_audio_resample_step(ratios[ri]);
        const NphAudioResampleBank *bank = nph_audio_resampler_bank(rs, step);
        // A start between frames, past the history the widest bank needs.
        const u64 pos = ((u64)(PAD / 2U) << 32U) + 0x6A5F3C21ULL;
        for (u32 ci = 0U; ci < 4U; ++ci) {
            const u32 ch = channel_counts[ci];
            const f32 *const *s = (const f32 *const *)src;
            bool have_sse = false;
            for (u32 l = 0U; l < nlevels; ++l) {
                const NphAudioKernels *k = nph_audio_kernels(levels[l]);
                f32 *const *dst = (levels[l] == NPH_SIMD_LEVEL_SCALAR) ? ref : out;
                k->resample(bank, s, ch, pos, step, dst, OUT);
                if (levels[l] == NPH_SIMD_LEVEL_SCALAR) {
                    continue;
                }
                for (u32 c = 0U; c < ch; ++c) {
                    for (u32 i = 0U; i < (u32)OUT; ++i) {
                        NTEST_ASSERT_NEAR_F32(out[c][i], ref[c][i], 2e-5F);
                    }
                }
                for (u32 c = 0U; c < ch; ++c) {
                    if (levels[l] == NPH_SIMD_LEVEL_SSE4) {
                        memcpy(sse[c], out[c], OUT * sizeof(f32));
                    } else if (have_sse) {
                        NTEST_ASSERT_TRUE(test_same_bits(out[c], sse[c], OUT));
                    }
                }
                have_sse = have_sse || levels[l] == NPH_SIMD_LEVEL_SSE4;
            }
        }
    }
    for (u32 c = 0U; c < 8U; ++c) {
        test_buffer_free(src[c], SRC + 2U * PAD);
        test_buffer_free(ref[c], OUT);
        test_buffer_free(out[c], OUT);
        test_buffer_free(sse[c], OUT);
    }
    nph_audio_resampler_destroy(rs);
}
