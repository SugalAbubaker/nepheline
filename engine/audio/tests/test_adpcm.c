// SPDX-License-Identifier: Apache-2.0
// Our ADPCM (adpcm.h): its quality on tones, noise, and transients;
// random access from any frame; channels apart; the stream's size and
// checks; and a decoder that survives garbage.

#include <nepheline/audio/adpcm.h>

#include "test_audio_support.h"

static i16 *make_tone(u64 frames, u32 channels, f64 freq, f64 amp)
{
    i16 *x = (i16 *)NPH_ALLOC(nph_allocator_system(), frames * channels * sizeof(i16), 16U);
    NTEST_ASSERT_NOT_NULL(x);
    for (u64 i = 0U; i < frames; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            const f64 v = amp * sin(2.0 * TEST_PI * freq * (1.0 + 0.5 * c) * (f64)i / 48000.0);
            x[i * channels + c] = (i16)floor(v * 32767.0 + 0.5);
        }
    }
    return x;
}

// The signal-to-noise ratio of the decoded stream against its source, in dB.
static f64 snr_db(const i16 *x, u64 frames, u32 channels, const NphAdpcmEncoded *enc)
{
    i16 *y = (i16 *)NPH_ALLOC(nph_allocator_system(), frames * channels * sizeof(i16), 16U);
    NTEST_ASSERT_NOT_NULL(y);
    nph_adpcm_decode_int(&enc->stream, 0U, (u32)frames, y);
    f64 signal = 0.0;
    f64 noise = 0.0;
    for (u64 i = 0U; i < frames * channels; ++i) {
        signal += (f64)x[i] * (f64)x[i];
        const f64 e = (f64)x[i] - (f64)y[i];
        noise += e * e;
    }
    NPH_FREE(nph_allocator_system(), y, frames * channels * sizeof(i16), 16U);
    return noise > 0.0 ? 10.0 * log10(signal / noise) : 200.0;
}

NTEST(audio_adpcm, sizes_and_validation)
{
    NTEST_ASSERT_EQ_UINT(nph_adpcm_size(1U, 0U), NPH_ADPCM_TABLE_BYTES);
    NTEST_ASSERT_EQ_UINT(nph_adpcm_size(1U, 1U), NPH_ADPCM_TABLE_BYTES + NPH_ADPCM_BLOCK_BYTES);
    NTEST_ASSERT_EQ_UINT(nph_adpcm_size(2U, 129U),
                         NPH_ADPCM_TABLE_BYTES + 4U * NPH_ADPCM_BLOCK_BYTES);
    i16 *x = make_tone(1000U, 2U, 440.0, 0.5);
    NphAdpcmEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), x, 2U, 1000U, &enc),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_adpcm_validate(&enc.stream), NPH_RESULT_OK);
    // 4.3 bits a sample.
    NTEST_ASSERT_EQ_UINT(enc.stream.size, nph_adpcm_size(2U, 1000U));
    NphAdpcmStream bad = enc.stream;
    bad.size -= 1U;
    NTEST_ASSERT_EQ_INT(nph_adpcm_validate(&bad), NPH_RESULT_CORRUPT);
    bad = enc.stream;
    bad.channels = 0U;
    NTEST_ASSERT_EQ_INT(nph_adpcm_validate(&bad), NPH_RESULT_INVALID_ARGUMENT);
    enc.data[NPH_ADPCM_TABLE_BYTES + 4U] = 0x0FU; // a shift of 15
    NTEST_ASSERT_EQ_INT(nph_adpcm_validate(&enc.stream), NPH_RESULT_CORRUPT);
    nph_adpcm_encoded_free(&enc);
    NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), x, 9U, 10U, &enc),
                        NPH_RESULT_INVALID_ARGUMENT);
    NPH_FREE(nph_allocator_system(), x, 2000U * sizeof(i16), 16U);
}

NTEST(audio_adpcm, quality_on_tones_noise_and_silence)
{
    const struct {
        f64 freq, amp, min_snr;
    } tones[4] = {
        {110.0, 0.5, 40.0},  // a low tone: the predictors' best case
        {1000.0, 0.5, 30.0}, // mid
        {6000.0, 0.25, 18.0},
        {440.0, 0.01, 22.0}, // quiet: the shift drops with the level
    };
    for (u32 t = 0U; t < 4U; ++t) {
        i16 *x = make_tone(48000U, 1U, tones[t].freq, tones[t].amp);
        NphAdpcmEncoded enc;
        NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), x, 1U, 48000U, &enc),
                            NPH_RESULT_OK);
        const f64 snr = snr_db(x, 48000U, 1U, &enc);
        NTEST_LOG("%.0f Hz at %.2f: SNR %.1f dB", tones[t].freq, tones[t].amp, snr);
        NTEST_ASSERT_TRUE(snr > tones[t].min_snr);
        nph_adpcm_encoded_free(&enc);
        NPH_FREE(nph_allocator_system(), x, 48000U * sizeof(i16), 16U);
    }
    // White noise has nothing to predict: what is left is 4-bit
    // quantisation.
    i16 noise[8192];
    u64 rng = 5U;
    for (u32 i = 0U; i < 8192U; ++i) {
        noise[i] = (i16)(test_rng_signed(&rng) * 16000.0F);
    }
    NphAdpcmEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), noise, 1U, 8192U, &enc),
                        NPH_RESULT_OK);
    const f64 snr = snr_db(noise, 8192U, 1U, &enc);
    NTEST_LOG("noise: SNR %.1f dB", snr);
    NTEST_ASSERT_TRUE(snr > 12.0);
    nph_adpcm_encoded_free(&enc);
    // Silence is exact.
    i16 silence[300] = {0};
    NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), silence, 1U, 300U, &enc),
                        NPH_RESULT_OK);
    i16 out[300];
    nph_adpcm_decode_int(&enc.stream, 0U, 300U, out);
    NTEST_ASSERT_EQ_MEM(out, silence, sizeof out);
    nph_adpcm_encoded_free(&enc);
}

NTEST(audio_adpcm, any_range_decodes_like_the_whole)
{
    const u64 frames = 5000U;
    i16 *x = make_tone(frames, 3U, 330.0, 0.6);
    NphAdpcmEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), x, 3U, frames, &enc),
                        NPH_RESULT_OK);
    i16 *whole = (i16 *)NPH_ALLOC(nph_allocator_system(), frames * 3U * sizeof(i16), 16U);
    NTEST_ASSERT_NOT_NULL(whole);
    nph_adpcm_decode_int(&enc.stream, 0U, (u32)frames, whole);
    u64 rng = 17U;
    f32 a[600];
    f32 b[600];
    f32 c[600];
    f32 *planes[3] = {a, b, c};
    for (u32 trial = 0U; trial < 200U; ++trial) {
        const u64 first = test_rng_next(&rng) % frames;
        const u32 count = 1U + (u32)(test_rng_next(&rng) % 600U);
        nph_adpcm_decode(&enc.stream, first, count, planes);
        for (u32 i = 0U; i < count; ++i) {
            for (u32 ch = 0U; ch < 3U; ++ch) {
                const f32 want = first + i < frames ? (f32)whole[(first + i) * 3U + ch] / 32768.0F
                                                    : 0.0F; // past the end: silence
                NTEST_ASSERT_TRUE(planes[ch][i] == want);
            }
        }
    }
    // The channels stay apart: each one's own tone, well reproduced.
    f64 err[3] = {0.0, 0.0, 0.0};
    for (u64 i = 0U; i < frames; ++i) {
        for (u32 ch = 0U; ch < 3U; ++ch) {
            const f64 e = (f64)x[i * 3U + ch] - (f64)whole[i * 3U + ch];
            err[ch] += e * e;
        }
    }
    for (u32 ch = 0U; ch < 3U; ++ch) {
        NTEST_ASSERT_TRUE(sqrt(err[ch] / (f64)frames) < 300.0);
    }
    NPH_FREE(nph_allocator_system(), whole, frames * 3U * sizeof(i16), 16U);
    nph_adpcm_encoded_free(&enc);
    NPH_FREE(nph_allocator_system(), x, frames * 3U * sizeof(i16), 16U);
}

NTEST(audio_adpcm, the_decoder_survives_garbage)
{
    u8 junk[NPH_ADPCM_TABLE_BYTES + 6U * NPH_ADPCM_BLOCK_BYTES];
    u64 rng = 31U;
    i16 out[2U * 400U];
    for (u32 trial = 0U; trial < 300U; ++trial) {
        for (u32 i = 0U; i < sizeof junk; ++i) {
            junk[i] = (u8)test_rng_next(&rng);
        }
        // Every size, too short included: the decoder reads silence where
        // the stream does not reach.
        const NphAdpcmStream s = {2U, 384U, junk, 1U + test_rng_next(&rng) % sizeof junk};
        nph_adpcm_decode_int(&s, test_rng_next(&rng) % 500U, 400U, out);
    }
    NTEST_ASSERT_TRUE(true); // reaching here is the test: no fault, no hang
}
