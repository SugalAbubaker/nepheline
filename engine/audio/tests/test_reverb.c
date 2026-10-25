// SPDX-License-Identifier: Apache-2.0
// The reverberator (reverb.h): decay times measured as acoustics measures
// them (Schroeder's backward integration, "New Method of Measuring
// Reverberation Time", JASA 37(3), 1965), echo density (Abel & Huang, "A
// Simple, Robust Measure of Reverberation Echo Density", AES 121st
// Convention, 2006), decorrelated outputs, stability, and changes without a
// click.

#include <nepheline/audio/filter.h>
#include <nepheline/audio/reverb.h>

#include "test_audio_support.h"

#define RV_RATE 48000.0F

static NphAudioReverbParams rv_params(void)
{
    NphAudioReverbParams p = nph_audio_reverb_params_default();
    p.early = 0.0F;
    p.late = 1.0F;
    p.dry = 0.0F;
    p.predelay_ms = 0.0F;
    p.modulation = 0.0F;
    return p;
}

// The reverberator's impulse response, `frames` long, on `channels`
// channels (the impulse in every channel).
static void rv_impulse(const NphAudioReverbParams *p, u32 channels, f32 **out, u32 frames)
{
    NphAudioReverb r;
    NTEST_ASSERT_EQ_INT(nph_audio_reverb_init(&r, nph_allocator_system(), channels, RV_RATE, p),
                        NPH_RESULT_OK);
    for (u32 c = 0U; c < channels; ++c) {
        memset(out[c], 0, sizeof(f32) * frames);
        out[c][0] = 1.0F;
    }
    for (u32 i = 0U; i < frames; i += NPH_AUDIO_BLOCK_FRAMES) {
        f32 *sub[NPH_AUDIO_MAX_CHANNELS];
        for (u32 c = 0U; c < channels; ++c) {
            sub[c] = out[c] + i;
        }
        nph_audio_reverb_process(&r, sub, nph_min_u32(NPH_AUDIO_BLOCK_FRAMES, frames - i));
    }
    nph_audio_reverb_free(&r);
}

// T60 from the energy decay curve of x: a line fitted to its level from -5
// to -35 dB (T30), extended to 60 dB.
static f64 rv_t60(const f32 *x, u32 count)
{
    f64 *edc = (f64 *)NPH_ALLOC(nph_allocator_system(), sizeof(f64) * count, 16U);
    NTEST_ASSERT_NOT_NULL(edc);
    f64 sum = 0.0;
    for (u32 i = count; i-- > 0U;) {
        sum += (f64)x[i] * (f64)x[i];
        edc[i] = sum;
    }
    const f64 total = edc[0];
    f64 st = 0.0;
    f64 sl = 0.0;
    f64 stt = 0.0;
    f64 stl = 0.0;
    f64 n = 0.0;
    for (u32 i = 0U; i < count; ++i) {
        const f64 level = 10.0 * log10(edc[i] / total);
        if (level <= -5.0 && level >= -35.0) {
            const f64 t = (f64)i / (f64)RV_RATE;
            st += t;
            sl += level;
            stt += t * t;
            stl += t * level;
            n += 1.0;
        }
    }
    NPH_FREE(nph_allocator_system(), edc, sizeof(f64) * count, 16U);
    const f64 slope = (n * stl - st * sl) / (n * stt - st * st); // dB a second
    return -60.0 / slope;
}

// x through a fourth-order Butterworth section pair of `type` at `freq`.
static void rv_band(f32 *x, u32 count, NphAudioFilterType type, f32 freq)
{
    const NphAudioFilterCoeffs c = nph_audio_filter_design(type, freq, 0.7071F, 0.0F, RV_RATE);
    for (u32 pass = 0U; pass < 2U; ++pass) {
        NphAudioFilterState s;
        nph_audio_filter_reset(&s);
        nph_audio_filter_process(&c, &s, x, x, count);
    }
}

NTEST(audio_reverb, decays_in_the_times_it_is_set_to)
{
    enum {
        FRAMES = 4 * 48000
    };
    NphAudioReverbParams p = rv_params();
    p.decay_s = 1.5F;
    p.damping = 0.4F; // 0.6 s at the top
    p.crossover_hz = 1000.0F;
    f32 *ir = test_buffer(FRAMES);
    f32 *band = test_buffer(FRAMES);
    f32 *planes[1] = {ir};
    rv_impulse(&p, 1U, planes, FRAMES);
    // Well below the crossover the shelves are their DC gains; well above,
    // their Nyquist gains.
    memcpy(band, ir, sizeof(f32) * FRAMES);
    rv_band(band, FRAMES, NPH_AUDIO_FILTER_LOWPASS, 80.0F);
    const f64 low = rv_t60(band, FRAMES);
    memcpy(band, ir, sizeof(f32) * FRAMES);
    rv_band(band, FRAMES, NPH_AUDIO_FILTER_HIGHPASS, 14000.0F);
    const f64 high = rv_t60(band, FRAMES);
    NTEST_LOG("T60: %.3f s below 80 Hz (set 1.5), %.3f s above 14 kHz (set 0.6)", low, high);
    NTEST_ASSERT_NEAR_F64(low, 1.5, 0.15);
    NTEST_ASSERT_NEAR_F64(high, 0.6, 0.08);
    test_buffer_free(ir, FRAMES);
    test_buffer_free(band, FRAMES);
}

NTEST(audio_reverb, a_longer_decay_and_a_larger_room)
{
    // The same measure at another setting: the decay time follows the
    // parameter, whatever the size.
    enum {
        FRAMES = 6 * 48000
    };
    NphAudioReverbParams p = rv_params();
    p.decay_s = 3.0F;
    p.damping = 1.0F;
    p.size = 1.8F;
    f32 *ir = test_buffer(FRAMES);
    f32 *planes[1] = {ir};
    rv_impulse(&p, 1U, planes, FRAMES);
    const f64 t60 = rv_t60(ir, FRAMES);
    NTEST_LOG("T60 %.3f s (set 3.0, size 1.8)", t60);
    NTEST_ASSERT_NEAR_F64(t60, 3.0, 0.3);
    test_buffer_free(ir, FRAMES);
}

NTEST(audio_reverb, the_tail_is_dense)
{
    // Abel & Huang's normalised echo density: the fraction of a window's
    // samples beyond one standard deviation, over Gaussian noise's
    // (erfc(1/sqrt 2) = 0.3173). Near 1 the tail sounds like noise, not
    // like discrete echoes.
    enum {
        FRAMES = 24000,
        WINDOW = 960
    };
    NphAudioReverbParams p = rv_params();
    p.modulation = 0.3F;
    f32 *ir = test_buffer(FRAMES);
    f32 *planes[1] = {ir};
    rv_impulse(&p, 1U, planes, FRAMES);
    for (u32 start = 4800U; start + WINDOW <= 14400U; start += WINDOW) {
        const f64 sigma = test_rms(ir + start, WINDOW);
        u32 beyond = 0U;
        for (u32 i = 0U; i < (u32)WINDOW; ++i) {
            beyond += fabs((f64)ir[start + i]) > sigma ? 1U : 0U;
        }
        const f64 ned = (f64)beyond / (f64)WINDOW / 0.3173;
        NTEST_LOG("echo density at %3u ms: %.2f", start / 48U, ned);
        NTEST_ASSERT_TRUE(ned > 0.8);
    }
    test_buffer_free(ir, FRAMES);
}

static f64 rv_correlation(const f32 *a, const f32 *b, u32 count)
{
    f64 ab = 0.0;
    f64 aa = 0.0;
    f64 bb = 0.0;
    for (u32 i = 0U; i < count; ++i) {
        ab += (f64)a[i] * (f64)b[i];
        aa += (f64)a[i] * (f64)a[i];
        bb += (f64)b[i] * (f64)b[i];
    }
    return ab / sqrt(aa * bb);
}

NTEST(audio_reverb, outputs_are_uncorrelated_and_width_blends_them)
{
    enum {
        FRAMES = 48000
    };
    f32 *l = test_buffer(FRAMES);
    f32 *r = test_buffer(FRAMES);
    f32 *planes[2] = {l, r};
    NphAudioReverbParams p = rv_params();
    rv_impulse(&p, 2U, planes, FRAMES);
    const f64 wide = rv_correlation(l + 4800, r + 4800, FRAMES - 4800U);
    const f64 balance = test_db(test_rms(l, FRAMES) / test_rms(r, FRAMES));
    p.width = 0.0F;
    rv_impulse(&p, 2U, planes, FRAMES);
    const f64 narrow = rv_correlation(l + 4800, r + 4800, FRAMES - 4800U);
    NTEST_LOG("correlation of the tails: width 1 %.3f, width 0 %.3f; balance %.2f dB", wide, narrow,
              balance);
    NTEST_ASSERT_TRUE(fabs(wide) < 0.15);
    NTEST_ASSERT_TRUE(narrow > 0.999);
    NTEST_ASSERT_TRUE(fabs(balance) < 0.5);
    test_buffer_free(l, FRAMES);
    test_buffer_free(r, FRAMES);
}

NTEST(audio_reverb, dry_only_passes_the_input)
{
    NphAudioReverbParams p = rv_params();
    p.late = 0.0F;
    p.dry = 1.0F;
    NphAudioReverb r;
    NTEST_ASSERT_EQ_INT(nph_audio_reverb_init(&r, nph_allocator_system(), 2U, RV_RATE, &p),
                        NPH_RESULT_OK);
    f32 a[512];
    f32 b[512];
    f32 ra[512];
    test_noise(a, 512U, 11U, 1.0F);
    test_noise(b, 512U, 12U, 1.0F);
    memcpy(ra, a, sizeof ra);
    f32 *planes[2] = {a, b};
    nph_audio_reverb_process(&r, planes, 512U);
    NTEST_ASSERT_TRUE(test_same_bits(a, ra, 512U));
    nph_audio_reverb_free(&r);
}

NTEST(audio_reverb, long_noise_stays_bounded_and_reset_silences)
{
    enum {
        FRAMES = 10 * 48000
    };
    NphAudioReverbParams p = nph_audio_reverb_params_default();
    p.decay_s = 30.0F; // the longest
    p.damping = 1.0F;
    p.modulation = 1.0F;
    NphAudioReverb r;
    NTEST_ASSERT_EQ_INT(nph_audio_reverb_init(&r, nph_allocator_system(), 2U, RV_RATE, &p),
                        NPH_RESULT_OK);
    f32 *l = test_buffer(FRAMES);
    f32 *rr = test_buffer(FRAMES);
    test_noise(l, FRAMES, 21U, 0.5F);
    test_noise(rr, FRAMES, 22U, 0.5F);
    for (u32 i = 0U; i < (u32)FRAMES; i += NPH_AUDIO_BLOCK_FRAMES) {
        f32 *planes[2] = {l + i, rr + i};
        nph_audio_reverb_process(&r, planes, NPH_AUDIO_BLOCK_FRAMES);
    }
    // The tail's level settles: the last second no louder than the one
    // before it by more than its fluctuation.
    const f64 last = test_rms(l + FRAMES - 48000, 48000U);
    const f64 before = test_rms(l + FRAMES - 96000, 48000U);
    NTEST_LOG("RMS of the last two seconds: %.3f, %.3f", before, last);
    NTEST_ASSERT_TRUE(last < before * 1.2);
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        NTEST_ASSERT_FALSE(isnan(l[i]) || isnan(rr[i]));
    }
    nph_audio_reverb_reset(&r);
    memset(l, 0, sizeof(f32) * 4096U);
    memset(rr, 0, sizeof(f32) * 4096U);
    f32 *planes[2] = {l, rr};
    nph_audio_reverb_process(&r, planes, 4096U);
    for (u32 i = 0U; i < 4096U; ++i) {
        NTEST_ASSERT_TRUE(l[i] == 0.0F && rr[i] == 0.0F);
    }
    nph_audio_reverb_free(&r);
    test_buffer_free(l, FRAMES);
    test_buffer_free(rr, FRAMES);
}

NTEST(audio_reverb, a_new_size_or_predelay_does_not_click)
{
    // A 150 Hz tone, faded in, into the reverberator; at block 200 the size
    // and the pre-delay jump. A switch of taps without a crossfade would
    // step, and a step is broadband: above 12 kHz, where the tone and its
    // reverberation have nothing, the output must stay silent.
    enum {
        BLOCKS = 300
    };
    const u32 frames = BLOCKS * NPH_AUDIO_BLOCK_FRAMES;
    NphAudioReverbParams p = rv_params();
    p.early = 0.4F;
    p.modulation = 0.3F;
    NphAudioReverb r;
    NTEST_ASSERT_EQ_INT(nph_audio_reverb_init(&r, nph_allocator_system(), 2U, RV_RATE, &p),
                        NPH_RESULT_OK);
    f32 *l = test_buffer(frames);
    f32 *rr = test_buffer(frames);
    test_sine(l, frames, 150.0, (f64)RV_RATE, 0.0, 0.3);
    for (u32 i = 0U; i < 4800U; ++i) {
        l[i] *= (f32)(0.5 - 0.5 * cos(TEST_PI * (f64)i / 4800.0));
    }
    memcpy(rr, l, sizeof(f32) * frames);
    for (u32 b = 0U; b < (u32)BLOCKS; ++b) {
        if (b == 200U) {
            p.size = 1.6F;
            p.predelay_ms = 40.0F;
            nph_audio_reverb_set(&r, &p);
        }
        f32 *planes[2] = {l + (u64)b * NPH_AUDIO_BLOCK_FRAMES,
                          rr + (u64)b * NPH_AUDIO_BLOCK_FRAMES};
        nph_audio_reverb_process(&r, planes, NPH_AUDIO_BLOCK_FRAMES);
    }
    rv_band(l, frames, NPH_AUDIO_FILTER_HIGHPASS, 12000.0F);
    f32 worst = 0.0F;
    for (u32 i = 100U * NPH_AUDIO_BLOCK_FRAMES; i < frames; ++i) {
        worst = fabsf(l[i]) > worst ? fabsf(l[i]) : worst;
    }
    NTEST_LOG("loudest output above 12 kHz around the change: %.3g", (f64)worst);
    NTEST_ASSERT_TRUE(worst < 1e-3F);
    nph_audio_reverb_free(&r);
    test_buffer_free(l, frames);
    test_buffer_free(rr, frames);
}
