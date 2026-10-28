// SPDX-License-Identifier: Apache-2.0
// The lossless codec (lossless.h): bit-exact round trips over signals,
// depths, channel counts, and block sizes; the stereo modes; incremental
// decoding a mixer block at a time and skips; validation; and a decoder
// that survives garbage.

#include <nepheline/audio/lossless.h>

#include "test_audio_support.h"

typedef enum Signal {
    SIGNAL_SILENCE,
    SIGNAL_DC,
    SIGNAL_SINE,
    SIGNAL_HIGH_SINE,
    SIGNAL_NOISE,
    SIGNAL_SQUARE,
    SIGNAL_IMPULSES,
    SIGNAL_CHIRP,
    SIGNAL_MUSIC,
    SIGNAL_WASTED,
    SIGNAL_EXTREMES,
    SIGNAL_COUNT
} Signal;

static const char *g_signal_names[SIGNAL_COUNT] = {
    "silence",  "dc",    "sine",  "high sine", "noise",    "square",
    "impulses", "chirp", "music", "wasted",    "extremes",
};

// One sample of `signal` at frame i of channel c, at `bits` bits.
static i32 signal_value(Signal signal, u64 i, u32 c, u32 bits, u64 *rng)
{
    const f64 full = (f64)((1 << (bits - 1U)) - 1);
    const i32 hi = (1 << (bits - 1U)) - 1;
    const i32 lo = -(1 << (bits - 1U));
    const f64 t = (f64)i / 48000.0;
    f64 v = 0.0;
    switch (signal) {
    case SIGNAL_SILENCE:
        return 0;
    case SIGNAL_DC:
        return hi / 3;
    case SIGNAL_SINE:
        v = 0.5 * sin(2.0 * TEST_PI * (440.0 + 110.0 * c) * t);
        break;
    case SIGNAL_HIGH_SINE:
        v = 0.9 * sin(2.0 * TEST_PI * 19000.0 * t + (f64)c);
        break;
    case SIGNAL_NOISE:
        v = 0.99 * (f64)test_rng_signed(rng);
        break;
    case SIGNAL_SQUARE:
        v = ((i / (37U + c)) & 1U) != 0U ? 0.999 : -0.999;
        break;
    case SIGNAL_IMPULSES:
        return (i % 997U) == c ? hi : 0;
    case SIGNAL_CHIRP:
        v = 0.7 * sin(2.0 * TEST_PI * (20.0 + 9000.0 * t) * t);
        break;
    case SIGNAL_MUSIC: {
        // Harmonics under a decaying envelope, a little noise, channels
        // correlated as in a stereo mix.
        const f64 env = exp(-3.0 * fmod(t, 0.5));
        for (u32 h = 1U; h <= 6U; ++h) {
            v += env * 0.12 / (f64)h * sin(2.0 * TEST_PI * 220.0 * h * t + 0.3 * h);
        }
        v += 0.01 * (f64)test_rng_signed(rng) +
             (c == 1U ? 0.02 * sin(2.0 * TEST_PI * 3.0 * t) : 0.0);
        break;
    }
    case SIGNAL_WASTED: {
        // Low bits that are always zero (an 8-bit source in a wider word).
        const i32 shift = (i32)bits / 3;
        const i32 m = (i32)(0.6 * sin(2.0 * TEST_PI * 300.0 * t) * full) >> shift;
        return m * (1 << shift);
    }
    case SIGNAL_EXTREMES:
        return ((i + c) & 1U) != 0U ? hi : lo;
    case SIGNAL_COUNT:
        break;
    }
    const f64 q = floor(v * full + 0.5);
    return (i32)(q > (f64)hi ? (f64)hi : (q < (f64)lo ? (f64)lo : q));
}

static i32 *make_signal(Signal signal, u64 frames, u32 channels, u32 bits, u64 seed)
{
    const u64 n = frames * channels;
    i32 *x = (i32 *)NPH_ALLOC(nph_allocator_system(), (n == 0U ? 1U : n) * sizeof(i32), 16U);
    NTEST_ASSERT_NOT_NULL(x);
    u64 rng = seed;
    for (u64 i = 0U; i < frames; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            x[i * channels + c] = signal_value(signal, i, c, bits, &rng);
        }
    }
    return x;
}

static void free_signal(i32 *x, u64 frames, u32 channels)
{
    const u64 n = frames * channels;
    NPH_FREE(nph_allocator_system(), x, (n == 0U ? 1U : n) * sizeof(i32), 16U);
}

// Encodes, validates, decodes whole, and compares exactly. Returns the
// stream's size over the PCM size at the same depth (packed).
static f64 round_trip(const i32 *x, u64 frames, u32 channels, u32 bits, u32 block, bool exhaustive)
{
    NphLosslessParams params = {block, 0U, exhaustive ? 1 : 0};
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(
        nph_lossless_encode(nph_allocator_system(), x, channels, bits, frames, &params, &enc),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_lossless_validate(&enc.stream), NPH_RESULT_OK);
    const u64 n = frames * channels;
    i32 *y = (i32 *)NPH_ALLOC(nph_allocator_system(), (n == 0U ? 1U : n) * sizeof(i32), 16U);
    NTEST_ASSERT_NOT_NULL(y);
    NTEST_ASSERT_EQ_INT(nph_lossless_decode_all(&enc.stream, y), NPH_RESULT_OK);
    for (u64 i = 0U; i < n; ++i) {
        if (x[i] != y[i]) {
            NTEST_FAIL("sample %llu: %d decoded as %d", (unsigned long long)i, x[i], y[i]);
        }
    }
    const f64 pcm = (f64)n * (f64)bits / 8.0;
    const f64 ratio = pcm > 0.0 ? (f64)enc.stream.size / pcm : 0.0;
    NPH_FREE(nph_allocator_system(), y, (n == 0U ? 1U : n) * sizeof(i32), 16U);
    nph_lossless_encoded_free(&enc);
    return ratio;
}

NTEST(audio_lossless, every_signal_round_trips_at_every_depth)
{
    const u32 depths[4] = {8U, 16U, 20U, 24U};
    for (u32 s = 0U; s < (u32)SIGNAL_COUNT; ++s) {
        for (u32 d = 0U; d < 4U; ++d) {
            const u64 frames = 9000U; // two full blocks and a partial one
            i32 *x = make_signal((Signal)s, frames, 2U, depths[d], 1234U + s);
            const f64 ratio = round_trip(x, frames, 2U, depths[d], 0U, false);
            NTEST_LOG("%-9s %2u-bit stereo: %.1f%% of PCM", g_signal_names[s], depths[d],
                      100.0 * ratio);
            free_signal(x, frames, 2U);
        }
    }
}

NTEST(audio_lossless, channel_counts_and_block_sizes)
{
    const u32 channel_counts[5] = {1U, 2U, 3U, 6U, 8U};
    const u32 blocks[5] = {0U, 1152U, 256U, 1U, NPH_LOSSLESS_MAX_BLOCK};
    const u64 lengths[4] = {1U, 255U, 4096U, 70001U};
    for (u32 ci = 0U; ci < 5U; ++ci) {
        for (u32 bi = 0U; bi < 5U; ++bi) {
            for (u32 li = 0U; li < 4U; ++li) {
                if (blocks[bi] == 1U && lengths[li] > 4096U) {
                    continue; // a block per frame: long enough already
                }
                i32 *x = make_signal(SIGNAL_MUSIC, lengths[li], channel_counts[ci], 16U, 7U + li);
                (void)round_trip(x, lengths[li], channel_counts[ci], 16U, blocks[bi], false);
                free_signal(x, lengths[li], channel_counts[ci]);
            }
        }
    }
    // The smallest depth, and an empty stream.
    i32 *x = make_signal(SIGNAL_NOISE, 3000U, 1U, NPH_LOSSLESS_MIN_BITS, 3U);
    (void)round_trip(x, 3000U, 1U, NPH_LOSSLESS_MIN_BITS, 0U, true);
    free_signal(x, 3000U, 1U);
    i32 empty = 0;
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(
        nph_lossless_encode(nph_allocator_system(), &empty, 2U, 16U, 0U, NULL, &enc),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(enc.stream.block_count, 0U);
    NTEST_ASSERT_EQ_INT(nph_lossless_validate(&enc.stream), NPH_RESULT_OK);
    nph_lossless_encoded_free(&enc);
}

NTEST(audio_lossless, predictable_signals_compress_and_noise_does_not_grow)
{
    i32 *sine = make_signal(SIGNAL_SINE, 48000U, 2U, 16U, 1U);
    const f64 sine_ratio = round_trip(sine, 48000U, 2U, 16U, 0U, false);
    i32 *noise = make_signal(SIGNAL_NOISE, 48000U, 2U, 16U, 2U);
    const f64 noise_ratio = round_trip(noise, 48000U, 2U, 16U, 0U, false);
    i32 *silence = make_signal(SIGNAL_SILENCE, 48000U, 2U, 16U, 3U);
    const f64 silence_ratio = round_trip(silence, 48000U, 2U, 16U, 0U, false);
    NTEST_LOG("sine %.1f%%, noise %.1f%%, silence %.3f%%", 100.0 * sine_ratio, 100.0 * noise_ratio,
              100.0 * silence_ratio);
    NTEST_ASSERT_TRUE(sine_ratio < 0.25);     // a clean tone is mostly predictable
    NTEST_ASSERT_TRUE(noise_ratio < 1.005);   // full-scale noise costs no more than PCM
    NTEST_ASSERT_TRUE(silence_ratio < 0.002); // silence is a header per block
    free_signal(sine, 48000U, 2U);
    free_signal(noise, 48000U, 2U);
    free_signal(silence, 48000U, 2U);
}

NTEST(audio_lossless, correlated_stereo_uses_a_side_channel)
{
    // Identical channels: any side-based mode makes one subframe constant.
    enum {
        FRAMES = 4096
    };
    i32 *x = make_signal(SIGNAL_MUSIC, FRAMES, 2U, 16U, 5U);
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        x[(u64)i * 2U + 1U] = x[(u64)i * 2U];
    }
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 2U, 16U, FRAMES, NULL, &enc),
                        NPH_RESULT_OK);
    const u8 *block = enc.stream.data;
    NTEST_ASSERT_NE_UINT(block[10], NPH_LOSSLESS_STEREO_INDEPENDENT);
    // The mono equivalent costs about as much: the side is free.
    NphLosslessEncoded mono;
    i32 *left = make_signal(SIGNAL_MUSIC, FRAMES, 1U, 16U, 5U);
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        left[i] = x[(u64)i * 2U];
    }
    NTEST_ASSERT_EQ_INT(
        nph_lossless_encode(nph_allocator_system(), left, 1U, 16U, FRAMES, NULL, &mono),
        NPH_RESULT_OK);
    NTEST_ASSERT_LT_UINT(enc.stream.size, mono.stream.size + 64U);
    (void)round_trip(x, FRAMES, 2U, 16U, 0U, false);
    nph_lossless_encoded_free(&enc);
    nph_lossless_encoded_free(&mono);
    free_signal(x, FRAMES, 2U);
    free_signal(left, FRAMES, 1U);
}

NTEST(audio_lossless, incremental_decoding_and_skips_match_the_whole)
{
    const u64 frames = 20000U;
    i32 *x = make_signal(SIGNAL_MUSIC, frames, 2U, 24U, 11U);
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 2U, 24U, frames, NULL, &enc),
                        NPH_RESULT_OK);
    // Read every block in uneven pieces, as a voice would.
    NphLosslessDecoder dec;
    i32 a[300];
    i32 b[300];
    i32 *planes[2] = {a, b};
    u64 frame = 0U;
    u64 rng = 99U;
    for (u32 blk = 0U; blk < enc.stream.block_count; ++blk) {
        const u64 start = enc.stream.block_offsets[blk];
        const u64 end = blk + 1U < enc.stream.block_count ? enc.stream.block_offsets[blk + 1U]
                                                          : enc.stream.size;
        NTEST_ASSERT_EQ_INT(
            nph_lossless_decoder_begin(&dec, enc.stream.data + start, end - start, 2U, 24U),
            NPH_RESULT_OK);
        while (nph_lossless_decoder_remaining(&dec) > 0U) {
            const u32 want = 1U + (u32)(test_rng_next(&rng) % 300U);
            const u32 n = nph_min_u32(want, nph_lossless_decoder_remaining(&dec));
            NTEST_ASSERT_EQ_INT(nph_lossless_decoder_read_int(&dec, planes, n), NPH_RESULT_OK);
            for (u32 i = 0U; i < n; ++i) {
                NTEST_ASSERT_EQ_INT(a[i], x[(frame + i) * 2U]);
                NTEST_ASSERT_EQ_INT(b[i], x[(frame + i) * 2U + 1U]);
            }
            frame += n;
        }
    }
    NTEST_ASSERT_EQ_UINT(frame, frames);
    // A skip within a block, then floats.
    const u64 end1 = enc.stream.block_offsets[2];
    NTEST_ASSERT_EQ_INT(nph_lossless_decoder_begin(&dec,
                                                   enc.stream.data + enc.stream.block_offsets[1],
                                                   end1 - enc.stream.block_offsets[1], 2U, 24U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_lossless_decoder_skip(&dec, 1000U), NPH_RESULT_OK);
    f32 fa[64];
    f32 fb[64];
    f32 *fplanes[2] = {fa, fb};
    i32 scratch[128];
    NTEST_ASSERT_EQ_INT(nph_lossless_decoder_read(&dec, fplanes, 64U, scratch), NPH_RESULT_OK);
    for (u32 i = 0U; i < 64U; ++i) {
        const u64 at = ((u64)NPH_LOSSLESS_DEFAULT_BLOCK + 1000U + i) * 2U;
        NTEST_ASSERT_TRUE(fa[i] == (f32)x[at] / 8388608.0F);
        NTEST_ASSERT_TRUE(fb[i] == (f32)x[at + 1U] / 8388608.0F);
    }
    nph_lossless_encoded_free(&enc);
    free_signal(x, frames, 2U);
}

NTEST(audio_lossless, encoding_is_deterministic)
{
    i32 *x = make_signal(SIGNAL_CHIRP, 10000U, 2U, 16U, 21U);
    NphLosslessEncoded a;
    NphLosslessEncoded b;
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 2U, 16U, 10000U, NULL, &a),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 2U, 16U, 10000U, NULL, &b),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(a.stream.size, b.stream.size);
    NTEST_ASSERT_EQ_MEM(a.stream.data, b.stream.data, a.stream.size);
    nph_lossless_encoded_free(&a);
    nph_lossless_encoded_free(&b);
    free_signal(x, 10000U, 2U);
}

NTEST(audio_lossless, the_encoder_refuses_bad_input)
{
    i32 x[4] = {0, 1, 2, 40000};
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 1U, 16U, 4U, NULL, &enc),
                        NPH_RESULT_INVALID_ARGUMENT); // 40000 does not fit 16 bits
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 0U, 16U, 1U, NULL, &enc),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 9U, 16U, 0U, NULL, &enc),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 1U, 25U, 4U, NULL, &enc),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 1U, 3U, 4U, NULL, &enc),
                        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(audio_lossless, validation_finds_damage)
{
    i32 *x = make_signal(SIGNAL_MUSIC, 12000U, 2U, 16U, 4U);
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 2U, 16U, 12000U, NULL, &enc),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_lossless_validate(&enc.stream), NPH_RESULT_OK);
    // A flipped bit anywhere fails a CRC.
    const u64 positions[3] = {5U, enc.stream.size / 2U, enc.stream.size - 1U};
    for (u32 p = 0U; p < 3U; ++p) {
        enc.data[positions[p]] ^= 0x10U;
        NTEST_ASSERT_EQ_INT(nph_lossless_validate(&enc.stream), NPH_RESULT_CORRUPT);
        enc.data[positions[p]] ^= 0x10U;
    }
    NTEST_ASSERT_EQ_INT(nph_lossless_validate(&enc.stream), NPH_RESULT_OK);
    // Offsets out of order, frames that do not match the blocks.
    NphLosslessStream bad = enc.stream;
    u32 offsets[3] = {enc.stream.block_offsets[0], enc.stream.block_offsets[2],
                      enc.stream.block_offsets[1]};
    bad.block_offsets = offsets;
    NTEST_ASSERT_EQ_INT(nph_lossless_validate(&bad), NPH_RESULT_CORRUPT);
    bad = enc.stream;
    bad.frames = 99999U;
    NTEST_ASSERT_EQ_INT(nph_lossless_validate(&bad), NPH_RESULT_INVALID_ARGUMENT);
    bad = enc.stream;
    bad.size -= 1U;
    NTEST_ASSERT_EQ_INT(nph_lossless_validate(&bad), NPH_RESULT_CORRUPT);
    nph_lossless_encoded_free(&enc);
    free_signal(x, 12000U, 2U);
}

NTEST(audio_lossless, the_decoder_survives_garbage)
{
    // Damaged real blocks and random bytes: the decoder may fail, never
    // fault, never loop for ever, and never produce out-of-range samples.
    i32 *x = make_signal(SIGNAL_MUSIC, 4096U, 2U, 16U, 8U);
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(nph_lossless_encode(nph_allocator_system(), x, 2U, 16U, 4096U, NULL, &enc),
                        NPH_RESULT_OK);
    u8 *copy = (u8 *)NPH_ALLOC(nph_allocator_system(), enc.stream.size, 16U);
    NTEST_ASSERT_NOT_NULL(copy);
    u64 rng = 2024U;
    i32 a[256];
    i32 b[256];
    i32 *planes[2] = {a, b};
    u32 failures = 0U;
    for (u32 trial = 0U; trial < 400U; ++trial) {
        memcpy(copy, enc.stream.data, enc.stream.size);
        const u32 flips = 1U + (u32)(test_rng_next(&rng) % 8U);
        for (u32 f = 0U; f < flips; ++f) {
            const u64 at = test_rng_next(&rng) % enc.stream.size;
            copy[at] ^= (u8)(1U + test_rng_next(&rng) % 255U);
        }
        if (trial % 4U == 0U) {
            // Random bytes after a plausible header.
            for (u64 i = 20U; i < enc.stream.size; ++i) {
                copy[i] = (u8)test_rng_next(&rng);
            }
        }
        NphLosslessDecoder dec;
        if (nph_lossless_decoder_begin(&dec, copy, enc.stream.size, 2U, 16U) != NPH_RESULT_OK) {
            ++failures;
            continue;
        }
        while (nph_lossless_decoder_remaining(&dec) > 0U) {
            const u32 n = nph_min_u32(256U, nph_lossless_decoder_remaining(&dec));
            if (nph_lossless_decoder_read_int(&dec, planes, n) != NPH_RESULT_OK) {
                ++failures;
                break;
            }
            // Each subframe's samples are range-checked as they decode, so
            // even a damaged block's left and right stay near the depth (a
            // side channel undone can reach three times it).
            for (u32 i = 0U; i < n; ++i) {
                NTEST_ASSERT_TRUE(a[i] >= -(1 << 18) && a[i] <= (1 << 18));
                NTEST_ASSERT_TRUE(b[i] >= -(1 << 18) && b[i] <= (1 << 18));
            }
        }
        // Reading a failed decoder is refused, not a fault.
        NTEST_ASSERT_TRUE(dec.failed == 0 || nph_lossless_decoder_remaining(&dec) == 0U);
    }
    NTEST_LOG("%u of 400 damaged blocks were refused", failures);
    NTEST_ASSERT_GT_UINT(failures, 100U);
    NPH_FREE(nph_allocator_system(), copy, enc.stream.size, 16U);
    nph_lossless_encoded_free(&enc);
    free_signal(x, 4096U, 2U);
}
