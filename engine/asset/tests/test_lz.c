// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/lz.h>

#include "test_asset_support.h"

typedef struct LzFixture {
    NphAllocator *alloc;
    void *workspace_fast;
    void *workspace_high;
} LzFixture;

NTEST_SETUP(LzFixture)
{
    f->alloc = nph_allocator_system();
    f->workspace_fast = NPH_ALLOC(f->alloc, nph_lz_workspace_size(NPH_LZ_LEVEL_FAST), 16U);
    f->workspace_high = NPH_ALLOC(f->alloc, nph_lz_workspace_size(NPH_LZ_LEVEL_HIGH), 16U);
    NTEST_ASSERT_NOT_NULL(f->workspace_fast);
    NTEST_ASSERT_NOT_NULL(f->workspace_high);
}

NTEST_TEARDOWN(LzFixture)
{
    NPH_FREE(f->alloc, f->workspace_fast, nph_lz_workspace_size(NPH_LZ_LEVEL_FAST), 16U);
    NPH_FREE(f->alloc, f->workspace_high, nph_lz_workspace_size(NPH_LZ_LEVEL_HIGH), 16U);
}

// Compresses at `level`, checks the bound, decompresses, compares.
// Returns the compressed size.
static u64 round_trip(LzFixture *f, const u8 *src, u64 size, NphLzLevel level)
{
    const u64 bound = nph_lz_compress_bound(size);
    NTEST_ASSERT_GE_UINT(bound, size + 1U);
    u8 *packed = (u8 *)NPH_ALLOC(f->alloc, bound, 16U);
    u8 *back = (u8 *)NPH_ALLOC(f->alloc, nph_max_u64(size, 1U), 16U);
    NTEST_ASSERT_NOT_NULL(packed);
    NTEST_ASSERT_NOT_NULL(back);
    void *ws = level == NPH_LZ_LEVEL_HIGH ? f->workspace_high : f->workspace_fast;
    u64 packed_size = 0U;
    NTEST_ASSERT_EQ_INT(nph_lz_compress(src, size, packed, bound, level, ws, &packed_size),
                        NPH_RESULT_OK);
    NTEST_ASSERT_GE_UINT(packed_size, 1U);
    NTEST_ASSERT_LE_UINT(packed_size, bound);
    memset(back, 0xA5, (size_t)nph_max_u64(size, 1U));
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(packed, packed_size, back, size), NPH_RESULT_OK);
    if (size != 0U) {
        NTEST_ASSERT_EQ_MEM(back, src, size);
    }
    // The compressed stream must decode to exactly `size` bytes: asking
    // for one more or one fewer is a corruption.
    if (size != 0U) {
        NTEST_ASSERT_EQ_INT(nph_lz_decompress(packed, packed_size, back, size - 1U),
                            NPH_RESULT_CORRUPT);
    }
    u8 *bigger = (u8 *)NPH_ALLOC(f->alloc, size + 1U, 16U);
    NTEST_ASSERT_NOT_NULL(bigger);
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(packed, packed_size, bigger, size + 1U),
                        NPH_RESULT_CORRUPT);
    NPH_FREE(f->alloc, bigger, size + 1U, 16U);
    NPH_FREE(f->alloc, packed, bound, 16U);
    NPH_FREE(f->alloc, back, nph_max_u64(size, 1U), 16U);
    return packed_size;
}

NTEST_F(LzFixture, empty_and_tiny_inputs)
{
    NTEST_ASSERT_EQ_UINT(round_trip(f, NULL, 0U, NPH_LZ_LEVEL_FAST), 1U);
    NTEST_ASSERT_EQ_UINT(round_trip(f, NULL, 0U, NPH_LZ_LEVEL_HIGH), 1U);
    u8 small[64];
    test_fill_pattern(small, sizeof small, 3U);
    for (u64 n = 1U; n <= sizeof small; ++n) {
        round_trip(f, small, n, NPH_LZ_LEVEL_FAST);
        round_trip(f, small, n, NPH_LZ_LEVEL_HIGH);
    }
}

NTEST_F(LzFixture, zeros_compress_to_almost_nothing)
{
    const u64 size = NPH_MIB(1);
    u8 *zeros = (u8 *)NPH_ALLOC_ZEROED(f->alloc, size, 16U);
    NTEST_ASSERT_NOT_NULL(zeros);
    const u64 fast = round_trip(f, zeros, size, NPH_LZ_LEVEL_FAST);
    const u64 high = round_trip(f, zeros, size, NPH_LZ_LEVEL_HIGH);
    // A run of zeros is one literal plus matches of maximal length: a
    // few bytes per 64 KiB at most.
    NTEST_ASSERT_LT_UINT(fast, size / 100U);
    NTEST_ASSERT_LT_UINT(high, size / 100U);
    NTEST_LOG("1 MiB of zeros: fast %llu bytes, high %llu bytes", (unsigned long long)fast,
              (unsigned long long)high);
    NPH_FREE(f->alloc, zeros, size, 16U);
}

NTEST_F(LzFixture, random_data_stays_within_the_bound)
{
    const u64 size = NPH_KIB(300);
    u8 *data = (u8 *)NPH_ALLOC(f->alloc, size, 16U);
    NTEST_ASSERT_NOT_NULL(data);
    u64 rng = 99U;
    for (u64 i = 0; i < size; ++i) {
        data[i] = (u8)test_rng_next(&rng);
    }
    const u64 fast = round_trip(f, data, size, NPH_LZ_LEVEL_FAST);
    const u64 high = round_trip(f, data, size, NPH_LZ_LEVEL_HIGH);
    NTEST_ASSERT_GE_UINT(fast, size); // incompressible costs a little more
    NTEST_ASSERT_LE_UINT(fast, size + size / 100U);
    NTEST_ASSERT_LE_UINT(high, size + size / 100U);
    NPH_FREE(f->alloc, data, size, 16U);
}

NTEST_F(LzFixture, text_compresses_and_high_beats_fast)
{
    const u64 size = NPH_KIB(512);
    u8 *data = (u8 *)NPH_ALLOC(f->alloc, size, 16U);
    NTEST_ASSERT_NOT_NULL(data);
    test_fill_text(data, size, 7U);
    const u64 fast = round_trip(f, data, size, NPH_LZ_LEVEL_FAST);
    const u64 high = round_trip(f, data, size, NPH_LZ_LEVEL_HIGH);
    NTEST_LOG("512 KiB of text: fast %llu bytes (%.1f%%), high %llu bytes (%.1f%%)",
              (unsigned long long)fast, 100.0 * (f64)fast / (f64)size, (unsigned long long)high,
              100.0 * (f64)high / (f64)size);
    NTEST_ASSERT_LT_UINT(fast, size / 2U);
    NTEST_ASSERT_LE_UINT(high, fast);
    NPH_FREE(f->alloc, data, size, 16U);
}

NTEST_F(LzFixture, short_offsets_and_overlapping_matches)
{
    // Period-p patterns produce matches with offset p < 8, which the
    // decoder must copy a byte at a time.
    u8 data[4096];
    for (u32 period = 1U; period <= 9U; ++period) {
        for (u32 i = 0; i < sizeof data; ++i) {
            data[i] = (u8)('a' + (i % period));
        }
        const u64 fast = round_trip(f, data, sizeof data, NPH_LZ_LEVEL_FAST);
        const u64 high = round_trip(f, data, sizeof data, NPH_LZ_LEVEL_HIGH);
        NTEST_ASSERT_LT_UINT(fast, 64U);
        NTEST_ASSERT_LT_UINT(high, 64U);
    }
}

NTEST_F(LzFixture, matches_across_the_whole_window)
{
    // A random block repeated after exactly 65535 bytes is matchable;
    // after 65536 it is not (the offset would not fit).
    const u64 block = 4096U;
    u8 *data = (u8 *)NPH_ALLOC(f->alloc, 65536U + block * 2U, 16U);
    NTEST_ASSERT_NOT_NULL(data);
    u64 rng = 5U;
    for (u64 i = 0; i < 65536U + block * 2U; ++i) {
        data[i] = (u8)test_rng_next(&rng);
    }
    memcpy(data + 65535U, data, (size_t)block); // distance 65535: reachable
    const u64 size = 65535U + block;
    const u64 fast = round_trip(f, data, size, NPH_LZ_LEVEL_FAST);
    const u64 high = round_trip(f, data, size, NPH_LZ_LEVEL_HIGH);
    NTEST_ASSERT_LT_UINT(high, size - block / 2U); // the repeat was found
    NTEST_LOG("window edge: fast %llu, high %llu of %llu", (unsigned long long)fast,
              (unsigned long long)high, (unsigned long long)size);
    NPH_FREE(f->alloc, data, 65536U + block * 2U, 16U);
}

NTEST_F(LzFixture, mixed_structured_data)
{
    // Something like a vertex buffer: slowly varying 16-bit values with a
    // fixed stride.
    const u64 size = NPH_KIB(256);
    u8 *data = (u8 *)NPH_ALLOC(f->alloc, size, 16U);
    NTEST_ASSERT_NOT_NULL(data);
    for (u64 i = 0; i < size / 2U; ++i) {
        const u16 v = (u16)(1000U + (i % 8U) * 37U + (i / 64U));
        data[i * 2U] = (u8)(v & 0xFFU);
        data[i * 2U + 1U] = (u8)(v >> 8U);
    }
    const u64 fast = round_trip(f, data, size, NPH_LZ_LEVEL_FAST);
    const u64 high = round_trip(f, data, size, NPH_LZ_LEVEL_HIGH);
    NTEST_ASSERT_LT_UINT(high, size);
    NTEST_ASSERT_LE_UINT(high, fast);
    NPH_FREE(f->alloc, data, size, 16U);
}

NTEST_F(LzFixture, hand_built_streams_decode_as_the_format_says)
{
    NTEST_UNUSED(f);
    // Token 0x50: 5 literals, no match (the final sequence).
    const u8 lits[] = {0x50U, 'h', 'e', 'l', 'l', 'o'};
    char out[16];
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(lits, sizeof lits, out, 5U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_MEM(out, "hello", 5U);

    // "ab" then a match of length 4 + 2 = 6 at offset 2, then the final
    // literal 'z': "ab" + "ababab" + "z".
    const u8 seq[] = {0x22U, 'a', 'b', 0x02U, 0x00U, 0x10U, 'z'};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(seq, sizeof seq, out, 9U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_MEM(out,
                        "abababab"
                        "z",
                        9U);

    // Extended literal length: 15 + 3 = 18 literals.
    u8 ext[1U + 1U + 18U];
    ext[0] = 0xF0U;
    ext[1] = 3U;
    for (u32 i = 0; i < 18U; ++i) {
        ext[2U + i] = (u8)('A' + i);
    }
    char out18[18];
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(ext, sizeof ext, out18, 18U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_MEM(out18, "ABCDEFGHIJKLMNOPQR", 18U);

    // Extended match length: 1 literal, then 4 + 15 + 255 + 2 = 276 bytes
    // of offset-1 copies, then the mandatory final literal-only token
    // (empty here: a block never ends on a match).
    const u8 longm[] = {0x1FU, 'x', 0x01U, 0x00U, 0xFFU, 0x02U, 0x00U};
    u8 out277[277];
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(longm, sizeof longm, out277, 277U), NPH_RESULT_OK);
    for (u32 i = 0; i < 277U; ++i) {
        NTEST_ASSERT_EQ_UINT(out277[i], 'x');
    }
}

NTEST_F(LzFixture, decoder_rejects_malformed_streams)
{
    NTEST_UNUSED(f);
    char out[64];
    // Empty input.
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(NULL, 0U, out, 0U), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(NULL, 0U, NULL, 0U), NPH_RESULT_CORRUPT);
    // The stream of an empty input decodes into no destination at all,
    // and a literal it does not have room for is refused.
    const u8 empty_stream[] = {0x00U};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(empty_stream, 1U, NULL, 0U), NPH_RESULT_OK);
    const u8 one_literal[] = {0x10U, 'a'};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(one_literal, sizeof one_literal, NULL, 0U),
                        NPH_RESULT_CORRUPT);
    // Literals past the end of the input.
    const u8 short_lits[] = {0x50U, 'a', 'b'};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(short_lits, sizeof short_lits, out, 5U),
                        NPH_RESULT_CORRUPT);
    // Offset of zero.
    const u8 zero_off[] = {0x10U, 'a', 0x00U, 0x00U, 0x00U};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(zero_off, sizeof zero_off, out, 5U), NPH_RESULT_CORRUPT);
    // Offset before the start of the output.
    const u8 far_off[] = {0x10U, 'a', 0x02U, 0x00U, 0x00U};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(far_off, sizeof far_off, out, 5U), NPH_RESULT_CORRUPT);
    // A match that would overrun the output.
    const u8 overrun[] = {0x1FU, 'a', 0x01U, 0x00U, 0xFFU, 0xFFU, 0x00U};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(overrun, sizeof overrun, out, sizeof out),
                        NPH_RESULT_CORRUPT);
    // A truncated offset.
    const u8 cut[] = {0x10U, 'a', 0x01U};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(cut, sizeof cut, out, 5U), NPH_RESULT_CORRUPT);
    // A truncated length extension.
    const u8 cut_ext[] = {0xF0U, 0xFFU};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(cut_ext, sizeof cut_ext, out, 5U), NPH_RESULT_CORRUPT);
    // A block that ends on a match (no final literal token).
    const u8 ends_on_match[] = {0x12U, 'a', 0x01U, 0x00U};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(ends_on_match, sizeof ends_on_match, out, 7U),
                        NPH_RESULT_CORRUPT);
    // Output too small for the literals.
    const u8 five[] = {0x50U, 'h', 'e', 'l', 'l', 'o'};
    NTEST_ASSERT_EQ_INT(nph_lz_decompress(five, sizeof five, out, 3U), NPH_RESULT_CORRUPT);
}

NTEST_F(LzFixture, compressor_reports_a_small_output_buffer)
{
    u8 data[1000];
    u64 rng = 1U;
    for (u32 i = 0; i < sizeof data; ++i) {
        data[i] = (u8)test_rng_next(&rng);
    }
    u8 small[500];
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_lz_compress(data, sizeof data, small, sizeof small, NPH_LZ_LEVEL_FAST,
                                        f->workspace_fast, &size),
                        NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_EQ_INT(nph_lz_compress(data, sizeof data, small, sizeof small, NPH_LZ_LEVEL_HIGH,
                                        f->workspace_high, &size),
                        NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_EQ_UINT(nph_lz_compress_bound(NPH_LZ_MAX_INPUT_SIZE + 1U), 0U);
    NTEST_ASSERT_EQ_INT(nph_lz_compress(data, NPH_LZ_MAX_INPUT_SIZE + 1U, small, sizeof small,
                                        NPH_LZ_LEVEL_FAST, f->workspace_fast, &size),
                        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST_F(LzFixture, every_size_up_to_a_few_hundred_bytes_round_trips)
{
    // Sizes around the encoder's limits (13 bytes, the last 5 literals,
    // the 12-byte match floor) are where off-by-one bugs live.
    u8 data[300];
    for (u32 i = 0; i < sizeof data; ++i) {
        data[i] = (u8)("abcabcabdabcabcabe"[i % 18U]);
    }
    for (u64 n = 0U; n <= sizeof data; ++n) {
        round_trip(f, data, n, NPH_LZ_LEVEL_FAST);
        round_trip(f, data, n, NPH_LZ_LEVEL_HIGH);
    }
}
