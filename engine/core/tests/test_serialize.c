// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/serialize.h>

#include "test_support.h"

#include <string.h>

#define MAGIC 0x4E504853U // "NPHS"

typedef struct BinFixture {
    NphDebugAllocator da;
    NphBinWriter w;
} BinFixture;

NTEST_SETUP(BinFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    nph_bin_writer_init(&f->w, nph_debug_allocator_allocator(&f->da));
}

NTEST_TEARDOWN(BinFixture)
{
    nph_bin_writer_free(&f->w);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(BinFixture, scalars_round_trip_little_endian)
{
    NTEST_ASSERT_EQ_INT(nph_bin_write_u8(&f->w, 0xABU), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_u16(&f->w, 0x1234U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_u32(&f->w, 0xDEADBEEFU), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_u64(&f->w, 0x0102030405060708ULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_i8(&f->w, -1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_i16(&f->w, -2), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_i32(&f->w, -3), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_i64(&f->w, NPH_I64_MIN), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_f32(&f->w, 1.5F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_f64(&f->w, -0.125), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_bool(&f->w, true), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_str(&f->w, NPH_STR("hi")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_align(&f->w, 16U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_bin_writer_len(&f->w) % 16U, 0U);
    const u8 *d = nph_bin_writer_data(&f->w);
    NTEST_ASSERT_EQ_UINT(d[0], 0xABU);
    NTEST_ASSERT_EQ_UINT(d[1], 0x34U); // little-endian
    NTEST_ASSERT_EQ_UINT(d[2], 0x12U);
    NTEST_ASSERT_EQ_UINT(d[3], 0xEFU);
    NTEST_ASSERT_EQ_UINT(d[7], 0x08U);
    NTEST_ASSERT_EQ_UINT(d[14], 0x01U);

    NphBinReader r = nph_bin_reader(d, nph_bin_writer_len(&f->w));
    u8 a;
    u16 b;
    u32 c;
    u64 e;
    i8 g;
    i16 h;
    i32 i;
    i64 j;
    f32 k;
    f64 l;
    bool m;
    NphStr s;
    NTEST_ASSERT_EQ_INT(nph_bin_read_u8(&r, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_u16(&r, &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_u32(&r, &c), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_u64(&r, &e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_i8(&r, &g), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_i16(&r, &h), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_i32(&r, &i), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_i64(&r, &j), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_f32(&r, &k), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_f64(&r, &l), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_bool(&r, &m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_str(&r, &s), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_align(&r, 16U), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_bin_reader_at_end(&r));
    NTEST_ASSERT_EQ_UINT(a, 0xABU);
    NTEST_ASSERT_EQ_UINT(b, 0x1234U);
    NTEST_ASSERT_EQ_UINT(c, 0xDEADBEEFU);
    NTEST_ASSERT_EQ_UINT(e, 0x0102030405060708ULL);
    NTEST_ASSERT_EQ_INT(g, -1);
    NTEST_ASSERT_EQ_INT(h, -2);
    NTEST_ASSERT_EQ_INT(i, -3);
    NTEST_ASSERT_EQ_INT(j, NPH_I64_MIN);
    NTEST_ASSERT_TRUE(k == 1.5F);
    NTEST_ASSERT_TRUE(l == -0.125);
    NTEST_ASSERT_TRUE(m);
    NTEST_ASSERT_TRUE(nph_str_equals(s, NPH_STR("hi")));
    NTEST_ASSERT_EQ_PTR(s.data, (const char *)d + 47); // a view, not a copy (43 bytes + u32 length)
}

NTEST_F(BinFixture, reader_never_reads_past_the_end)
{
    NTEST_ASSERT_EQ_INT(nph_bin_write_u32(&f->w, 7U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_u8(&f->w, 1U), NPH_RESULT_OK);
    NphBinReader r = nph_bin_reader(nph_bin_writer_data(&f->w), 5U);
    u64 big;
    NTEST_ASSERT_EQ_INT(nph_bin_read_u64(&r, &big), NPH_RESULT_END_OF_STREAM);
    NTEST_ASSERT_EQ_UINT(r.pos, 0U); // unchanged
    u32 v;
    NTEST_ASSERT_EQ_INT(nph_bin_read_u32(&r, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v, 7U);
    NTEST_ASSERT_EQ_UINT(nph_bin_reader_remaining(&r), 1U);
    u8 bytes[2];
    NTEST_ASSERT_EQ_INT(nph_bin_read_bytes(&r, bytes, 2U), NPH_RESULT_END_OF_STREAM);
    NTEST_ASSERT_EQ_INT(nph_bin_read_skip(&r, 2U), NPH_RESULT_END_OF_STREAM);
    NTEST_ASSERT_EQ_INT(nph_bin_read_align(&r, 8U), NPH_RESULT_END_OF_STREAM);
    NphStr s;
    NTEST_ASSERT_EQ_INT(nph_bin_read_str(&r, &s), NPH_RESULT_END_OF_STREAM);
    NTEST_ASSERT_EQ_INT(nph_bin_read_bytes(&r, bytes, 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(bytes[0], 1U);
    NTEST_ASSERT_TRUE(nph_bin_reader_at_end(&r));
    NTEST_ASSERT_EQ_INT(nph_bin_read_bytes(&r, bytes, 0U), NPH_RESULT_OK);
    // A string whose length field exceeds the data is refused without moving.
    nph_bin_writer_clear(&f->w);
    NTEST_ASSERT_EQ_INT(nph_bin_write_u32(&f->w, 1000U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_u8(&f->w, 'x'), NPH_RESULT_OK);
    r = nph_bin_reader(nph_bin_writer_data(&f->w), nph_bin_writer_len(&f->w));
    NTEST_ASSERT_EQ_INT(nph_bin_read_str(&r, &s), NPH_RESULT_END_OF_STREAM);
    NTEST_ASSERT_EQ_UINT(r.pos, 0U);
    NphBinReader empty = nph_bin_reader(NULL, 0U);
    NTEST_ASSERT_TRUE(nph_bin_reader_at_end(&empty));
    NTEST_ASSERT_EQ_INT(nph_bin_read_u8(&empty, bytes), NPH_RESULT_END_OF_STREAM);
}

NTEST_F(BinFixture, header_sections_checksum)
{
    NTEST_ASSERT_EQ_INT(nph_bin_write_header(&f->w, MAGIC, 3U), NPH_RESULT_OK);
    NphBinSection section;
    NTEST_ASSERT_EQ_INT(nph_bin_write_section_begin(&f->w, 1U, &section), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_str(&f->w, NPH_STR("payload one")), NPH_RESULT_OK);
    nph_bin_write_section_end(&f->w, &section);
    NTEST_ASSERT_EQ_INT(nph_bin_write_section_begin(&f->w, 2U, &section), NPH_RESULT_OK);
    for (u32 i = 0; i < 100U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_bin_write_u32(&f->w, i), NPH_RESULT_OK);
    }
    nph_bin_write_section_end(&f->w, &section);
    NTEST_ASSERT_EQ_INT(nph_bin_write_section_begin(&f->w, 3U, &section), NPH_RESULT_OK);
    nph_bin_write_section_end(&f->w, &section); // empty section
    NTEST_ASSERT_EQ_INT(nph_bin_write_checksum(&f->w), NPH_RESULT_OK);

    const u8 *data = nph_bin_writer_data(&f->w);
    const u64 len = nph_bin_writer_len(&f->w);
    u64 payload_len = 0U;
    NTEST_ASSERT_EQ_INT(nph_bin_verify_checksum(data, len, &payload_len), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(payload_len, len - 4U);

    NphBinReader r = nph_bin_reader(data, payload_len);
    u32 version = 0U;
    NTEST_ASSERT_EQ_INT(nph_bin_read_header(&r, MAGIC, 1U, 5U, &version), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(version, 3U);
    u32 tag;
    u32 size;
    NTEST_ASSERT_EQ_INT(nph_bin_read_section(&r, &tag, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(tag, 1U);
    NTEST_ASSERT_EQ_UINT(size, 4U + 11U);
    NTEST_ASSERT_EQ_INT(nph_bin_read_skip(&r, size),
                        NPH_RESULT_OK); // an unknown section is skippable
    NTEST_ASSERT_EQ_INT(nph_bin_read_section(&r, &tag, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(tag, 2U);
    NTEST_ASSERT_EQ_UINT(size, 400U);
    u32 v;
    NTEST_ASSERT_EQ_INT(nph_bin_read_u32(&r, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v, 0U);
    NTEST_ASSERT_EQ_INT(nph_bin_read_skip(&r, size - 4U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_section(&r, &tag, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(tag, 3U);
    NTEST_ASSERT_EQ_UINT(size, 0U);
    NTEST_ASSERT_TRUE(nph_bin_reader_at_end(&r));
    NTEST_ASSERT_EQ_INT(nph_bin_read_section(&r, &tag, &size), NPH_RESULT_END_OF_STREAM);

    // Version range and magic checks.
    r = nph_bin_reader(data, payload_len);
    NTEST_ASSERT_EQ_INT(nph_bin_read_header(&r, MAGIC, 4U, 5U, &version),
                        NPH_RESULT_VERSION_MISMATCH);
    NTEST_ASSERT_EQ_UINT(version, 3U);
    r = nph_bin_reader(data, payload_len);
    NTEST_ASSERT_EQ_INT(nph_bin_read_header(&r, MAGIC + 1U, 0U, 9U, NULL), NPH_RESULT_CORRUPT);
    r = nph_bin_reader(data, 6U);
    NTEST_ASSERT_EQ_INT(nph_bin_read_header(&r, MAGIC, 0U, 9U, NULL), NPH_RESULT_END_OF_STREAM);
    NTEST_ASSERT_EQ_UINT(r.pos, 0U);

    // Corruption: a flipped byte fails the checksum; a section size beyond
    // the data is rejected.
    u8 *mutable_copy = (u8 *)NPH_ALLOC(nph_allocator_system(), len, 16U);
    memcpy(mutable_copy, data, (size_t)len);
    mutable_copy[20] ^= 0x01U;
    NTEST_ASSERT_EQ_INT(nph_bin_verify_checksum(mutable_copy, len, NULL), NPH_RESULT_CORRUPT);
    mutable_copy[20] ^= 0x01U;
    NTEST_ASSERT_EQ_INT(nph_bin_verify_checksum(mutable_copy, len, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_verify_checksum(mutable_copy, 3U, NULL), NPH_RESULT_CORRUPT);
    r = nph_bin_reader(mutable_copy, 16U); // header + first section head, truncated payload
    NTEST_ASSERT_EQ_INT(nph_bin_read_header(&r, MAGIC, 0U, 9U, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_section(&r, &tag, &size), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_UINT(r.pos, 8U);
    NPH_FREE(nph_allocator_system(), mutable_copy, len, 16U);
}

NTEST_F(BinFixture, patching_and_growth)
{
    NTEST_ASSERT_EQ_INT(nph_bin_write_u32(&f->w, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_write_u64(&f->w, 0U), NPH_RESULT_OK);
    for (u32 i = 0; i < 10000U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_bin_write_u16(&f->w, (u16)i), NPH_RESULT_OK);
    }
    nph_bin_patch_u32(&f->w, 0U, 10000U);
    nph_bin_patch_u64(&f->w, 4U, 0xFFFFFFFFFFFFFFFFULL);
    NphBinReader r = nph_bin_reader(nph_bin_writer_data(&f->w), nph_bin_writer_len(&f->w));
    u32 count;
    u64 marker;
    NTEST_ASSERT_EQ_INT(nph_bin_read_u32(&r, &count), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_bin_read_u64(&r, &marker), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(count, 10000U);
    NTEST_ASSERT_EQ_UINT(marker, 0xFFFFFFFFFFFFFFFFULL);
    for (u32 i = 0; i < 10000U; ++i) {
        u16 v;
        NTEST_ASSERT_EQ_INT(nph_bin_read_u16(&r, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v, (u16)i);
    }
    NTEST_ASSERT_TRUE(nph_bin_reader_at_end(&r));
    NTEST_ASSERT_EQ_INT(nph_bin_writer_reserve(&f->w, 1U << 20U), NPH_RESULT_OK);
    NTEST_ASSERT_GE_UINT(f->w.cap, 1U << 20U);
    nph_bin_writer_clear(&f->w);
    NTEST_ASSERT_EQ_UINT(nph_bin_writer_len(&f->w), 0U);
    NTEST_ASSERT_EQ_INT(nph_bin_write_bytes(&f->w, NULL, 0U), NPH_RESULT_OK);
}

NTEST(serialize, writer_allocation_failure)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[256];
    nph_arena_init(&arena, memory, sizeof memory);
    NphBinWriter w;
    nph_bin_writer_init(&w, nph_arena_allocator(&arena));
    u32 written = 0U;
    while (nph_bin_write_u64(&w, written) == NPH_RESULT_OK) {
        ++written;
        NTEST_ASSERT_LT_UINT(written, 1000U);
    }
    NTEST_ASSERT_GT_UINT(written, 8U);
    NTEST_ASSERT_EQ_UINT(nph_bin_writer_len(&w), (u64)written * 8U);
    NphBinReader r = nph_bin_reader(nph_bin_writer_data(&w), nph_bin_writer_len(&w));
    for (u32 i = 0; i < written; ++i) {
        u64 v;
        NTEST_ASSERT_EQ_INT(nph_bin_read_u64(&r, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v, i);
    }
    nph_bin_writer_free(&w);
    nph_bin_writer_free(&w);
    nph_arena_destroy(&arena);
}
