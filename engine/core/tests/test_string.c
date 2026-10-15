// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/string.h>
#include <nepheline/core/string_id.h>

#include "test_support.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NTEST(str, views_and_slices)
{
    const NphStr s = NPH_STR("hello, world");
    NTEST_ASSERT_EQ_UINT(s.len, 12U);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_from_cstr("hello, world"), s));
    NTEST_ASSERT_TRUE(nph_str_is_empty(nph_str_from_cstr(NULL)));
    NTEST_ASSERT_TRUE(nph_str_is_empty(nph_str_empty()));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_slice(s, 7U, 12U), NPH_STR("world")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_slice(s, 7U, 100U), NPH_STR("world")));
    NTEST_ASSERT_TRUE(nph_str_is_empty(nph_str_slice(s, 9U, 3U)));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_prefix(s, 5U), NPH_STR("hello")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_suffix(s, 5U), NPH_STR("world")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_suffix(s, 50U), s));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_empty(), nph_str(NULL, 0U)));
}

NTEST(str, comparison)
{
    NTEST_ASSERT_TRUE(nph_str_equals(NPH_STR("abc"), NPH_STR("abc")));
    NTEST_ASSERT_FALSE(nph_str_equals(NPH_STR("abc"), NPH_STR("abd")));
    NTEST_ASSERT_FALSE(nph_str_equals(NPH_STR("abc"), NPH_STR("ab")));
    NTEST_ASSERT_TRUE(nph_str_equals_ignore_case(NPH_STR("HeLLo"), NPH_STR("hEllO")));
    NTEST_ASSERT_FALSE(nph_str_equals_ignore_case(NPH_STR("hello"), NPH_STR("hellp")));
    NTEST_ASSERT_EQ_INT(nph_str_compare(NPH_STR("a"), NPH_STR("b")), -1);
    NTEST_ASSERT_EQ_INT(nph_str_compare(NPH_STR("b"), NPH_STR("a")), 1);
    NTEST_ASSERT_EQ_INT(nph_str_compare(NPH_STR("ab"), NPH_STR("ab")), 0);
    NTEST_ASSERT_EQ_INT(nph_str_compare(NPH_STR("ab"), NPH_STR("abc")), -1);
    NTEST_ASSERT_EQ_INT(nph_str_compare(NPH_STR("abc"), NPH_STR("ab")), 1);
    NTEST_ASSERT_EQ_INT(nph_str_compare(nph_str_empty(), nph_str_empty()), 0);
}

NTEST(str, search)
{
    const NphStr s = NPH_STR("the quick brown fox");
    NTEST_ASSERT_TRUE(nph_str_starts_with(s, NPH_STR("the")));
    NTEST_ASSERT_FALSE(nph_str_starts_with(s, NPH_STR("The")));
    NTEST_ASSERT_TRUE(nph_str_ends_with(s, NPH_STR("fox")));
    NTEST_ASSERT_FALSE(nph_str_ends_with(s, NPH_STR("foxes")));
    NTEST_ASSERT_TRUE(nph_str_starts_with(s, nph_str_empty()));
    NTEST_ASSERT_TRUE(nph_str_contains(s, NPH_STR("quick")));
    NTEST_ASSERT_FALSE(nph_str_contains(s, NPH_STR("slow")));
    NTEST_ASSERT_EQ_UINT(nph_str_find(s, NPH_STR("brown")), 10U);
    NTEST_ASSERT_EQ_UINT(nph_str_find(s, NPH_STR("fox")), 16U);
    NTEST_ASSERT_EQ_UINT(nph_str_find(s, NPH_STR("foxy")), NPH_STR_NPOS);
    NTEST_ASSERT_EQ_UINT(nph_str_find(s, nph_str_empty()), 0U);
    NTEST_ASSERT_EQ_UINT(nph_str_find_char(s, 'q'), 4U);
    NTEST_ASSERT_EQ_UINT(nph_str_find_char(s, 'z'), NPH_STR_NPOS);
    NTEST_ASSERT_EQ_UINT(nph_str_rfind_char(s, 'o'), 17U);
    NTEST_ASSERT_EQ_UINT(nph_str_rfind_char(s, 'z'), NPH_STR_NPOS);
    NTEST_ASSERT_EQ_UINT(nph_str_find_char(nph_str_empty(), 'a'), NPH_STR_NPOS);
}

NTEST(str, trim_and_split)
{
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_trim(NPH_STR("  \t x y \r\n")), NPH_STR("x y")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_trim_left(NPH_STR("  x ")), NPH_STR("x ")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_trim_right(NPH_STR("  x ")), NPH_STR("  x")));
    NTEST_ASSERT_TRUE(nph_str_is_empty(nph_str_trim(NPH_STR("   "))));

    NphStr rest = NPH_STR("a,bb,,ccc");
    NphStr field;
    const char *expected[] = {"a", "bb", "", "ccc"};
    u32 n = 0U;
    while (nph_str_split_first(rest, ',', &field, &rest)) {
        NTEST_ASSERT_LT_UINT(n, 4U);
        NTEST_ASSERT_TRUE(nph_str_equals(field, nph_str_from_cstr(expected[n])));
        ++n;
    }
    NTEST_ASSERT_EQ_UINT(n, 4U);
    // A trailing separator yields a final empty field... no: "x," gives "x"
    // then an empty tail that ends the loop. Document by test.
    rest = NPH_STR("x,");
    NTEST_ASSERT_TRUE(nph_str_split_first(rest, ',', &field, &rest));
    NTEST_ASSERT_TRUE(nph_str_equals(field, NPH_STR("x")));
    NTEST_ASSERT_FALSE(nph_str_split_first(rest, ',', &field, &rest));
}

NTEST(str, parsing)
{
    i64 i;
    u64 u;
    f64 f;
    bool b;
    NTEST_ASSERT_TRUE(nph_str_parse_i64(NPH_STR("-123"), &i) && i == -123);
    NTEST_ASSERT_TRUE(nph_str_parse_i64(NPH_STR("+7"), &i) && i == 7);
    NTEST_ASSERT_TRUE(nph_str_parse_i64(NPH_STR("9223372036854775807"), &i) && i == NPH_I64_MAX);
    NTEST_ASSERT_TRUE(nph_str_parse_i64(NPH_STR("-9223372036854775808"), &i) && i == NPH_I64_MIN);
    NTEST_ASSERT_FALSE(nph_str_parse_i64(NPH_STR("9223372036854775808"), &i));
    NTEST_ASSERT_FALSE(nph_str_parse_i64(NPH_STR("-9223372036854775809"), &i));
    NTEST_ASSERT_FALSE(nph_str_parse_i64(NPH_STR(""), &i));
    NTEST_ASSERT_FALSE(nph_str_parse_i64(NPH_STR("-"), &i));
    NTEST_ASSERT_FALSE(nph_str_parse_i64(NPH_STR("12a"), &i));
    NTEST_ASSERT_FALSE(nph_str_parse_i64(NPH_STR(" 12"), &i));
    NTEST_ASSERT_TRUE(nph_str_parse_u64(NPH_STR("18446744073709551615"), &u) && u == NPH_U64_MAX);
    NTEST_ASSERT_FALSE(nph_str_parse_u64(NPH_STR("18446744073709551616"), &u));
    NTEST_ASSERT_FALSE(nph_str_parse_u64(NPH_STR("-1"), &u));
    NTEST_ASSERT_TRUE(nph_str_parse_u64(NPH_STR("0"), &u) && u == 0U);
    NTEST_ASSERT_TRUE(nph_str_parse_f64(NPH_STR("1.5"), &f) && f == 1.5);
    NTEST_ASSERT_TRUE(nph_str_parse_f64(NPH_STR("-2e3"), &f) && f == -2000.0);
    NTEST_ASSERT_FALSE(nph_str_parse_f64(NPH_STR("1.5x"), &f));
    NTEST_ASSERT_FALSE(nph_str_parse_f64(NPH_STR(""), &f));
    NTEST_ASSERT_FALSE(nph_str_parse_f64(NPH_STR(" 1"), &f));
    NTEST_ASSERT_TRUE(nph_str_parse_bool(NPH_STR("TRUE"), &b) && b);
    NTEST_ASSERT_TRUE(nph_str_parse_bool(NPH_STR("off"), &b) && !b);
    NTEST_ASSERT_TRUE(nph_str_parse_bool(NPH_STR("1"), &b) && b);
    NTEST_ASSERT_FALSE(nph_str_parse_bool(NPH_STR("maybe"), &b));
}

NTEST(str, dup_and_hash)
{
    NphAllocator *a = nph_allocator_system();
    char *copy = NULL;
    NTEST_ASSERT_EQ_INT(nph_str_dup(a, NPH_STR("abc"), &copy), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(copy, "abc");
    nph_str_free(a, copy, 3U);
    NTEST_ASSERT_EQ_INT(nph_str_dup(a, nph_str_empty(), &copy), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(copy, "");
    nph_str_free(a, copy, 0U);
    NTEST_ASSERT_EQ_UINT(nph_str_hash(NPH_STR("abc")),
                         nph_hash64("abc", 3U, NPH_HASH_DEFAULT_SEED));
    NTEST_ASSERT_EQ_UINT(nph_ascii_to_lower('Q'), 'q');
    NTEST_ASSERT_EQ_UINT(nph_ascii_to_upper('q'), 'Q');
    NTEST_ASSERT_EQ_UINT(nph_ascii_to_upper('1'), '1');
    NTEST_ASSERT_TRUE(nph_ascii_is_alpha('z') && nph_ascii_is_digit('5') &&
                      nph_ascii_is_space('\t'));
    NTEST_ASSERT_FALSE(nph_ascii_is_alpha('5') || nph_ascii_is_digit('z') ||
                       nph_ascii_is_space('a'));
}

NTEST(utf8, encode_decode_round_trip)
{
    const u32 points[] = {0x0U,   0x41U,   0x7FU,    0x80U,    0x7FFU,
                          0x800U, 0xFFFFU, 0x10000U, 0x1F600U, 0x10FFFFU};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(points); ++i) {
        char bytes[4];
        const u32 n = nph_utf8_encode(points[i], bytes);
        NTEST_ASSERT_GT_UINT(n, 0U);
        u64 pos = 0U;
        u32 cp = 0U;
        NTEST_ASSERT_TRUE(nph_utf8_decode(nph_str(bytes, n), &pos, &cp));
        NTEST_ASSERT_EQ_UINT(cp, points[i]);
        NTEST_ASSERT_EQ_UINT(pos, n);
        NTEST_ASSERT_TRUE(nph_utf8_is_valid(nph_str(bytes, n)));
    }
    char bytes[4];
    NTEST_ASSERT_EQ_UINT(nph_utf8_encode(0xD800U, bytes), 0U);
    NTEST_ASSERT_EQ_UINT(nph_utf8_encode(0x110000U, bytes), 0U);
}

NTEST(utf8, invalid_sequences_are_replaced_one_byte_at_a_time)
{
    // Overlong "A", a stray continuation, a truncated 3-byte sequence, a
    // surrogate, and a 5-byte lead.
    const char bad[] = "\xC1\x81 \x80 \xE2\x82 \xED\xA0\x80 \xF8";
    const NphStr s = nph_str(bad, sizeof bad - 1U);
    NTEST_ASSERT_FALSE(nph_utf8_is_valid(s));
    u64 pos = 0U;
    u32 cp;
    u32 replacements = 0U;
    u32 total = 0U;
    while (nph_utf8_decode(s, &pos, &cp)) {
        ++total;
        replacements += cp == NPH_UTF8_REPLACEMENT ? 1U : 0U;
    }
    NTEST_ASSERT_EQ_UINT(total, (u32)s.len);             // every byte advanced individually
    NTEST_ASSERT_EQ_UINT(replacements, (u32)s.len - 4U); // four spaces are valid
    NTEST_ASSERT_EQ_UINT(nph_utf8_count(s), s.len);

    const NphStr good = NPH_STR("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80 \xEF\xBF\xBD");
    NTEST_ASSERT_TRUE(nph_utf8_is_valid(good));
    NTEST_ASSERT_EQ_UINT(nph_utf8_count(good), 11U); // a literal U+FFFD counts as valid
    NTEST_ASSERT_TRUE(nph_utf8_is_valid(nph_str_empty()));
}

typedef struct SbFixture {
    NphDebugAllocator da;
    NphStrBuilder sb;
} SbFixture;

NTEST_SETUP(SbFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    nph_sb_init(&f->sb, nph_debug_allocator_allocator(&f->da));
}

NTEST_TEARDOWN(SbFixture)
{
    nph_sb_free(&f->sb);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

NTEST_F(SbFixture, append_and_terminate)
{
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "");
    NTEST_ASSERT_TRUE(nph_str_is_empty(nph_sb_str(&f->sb)));
    NTEST_ASSERT_EQ_INT(nph_sb_append(&f->sb, NPH_STR("abc")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&f->sb, "def"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_char(&f->sb, '!'), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append(&f->sb, nph_str_empty()), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "abcdef!");
    NTEST_ASSERT_EQ_UINT(f->sb.len, 7U);
    NTEST_ASSERT_EQ_UINT(f->sb.data[7], 0U);
    NTEST_ASSERT_EQ_INT(nph_sb_append_u64(&f->sb, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_u64(&f->sb, NPH_U64_MAX), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_i64(&f->sb, NPH_I64_MIN), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_i64(&f->sb, 42), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "abcdef!018446744073709551615-922337203685477580842");
    nph_sb_clear(&f->sb);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "");
    NTEST_ASSERT_EQ_INT(nph_sb_appendf(&f->sb, "%d-%s-%.1f", 7, "x", 2.5), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "7-x-2.5");
    NTEST_ASSERT_EQ_INT(nph_sb_append_f64(&f->sb, 0.1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "7-x-2.50.10000000000000001");
    NTEST_ASSERT_EQ_INT(nph_sb_append_utf8(&f->sb, 0x20ACU), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_utf8(&f->sb, 0xD800U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_TRUE(nph_str_ends_with(nph_sb_str(&f->sb), NPH_STR("\xE2\x82\xAC")));
    nph_sb_truncate(&f->sb, 3U);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "7-x");
    nph_sb_truncate(&f->sb, 100U);
    NTEST_ASSERT_EQ_UINT(f->sb.len, 3U);
}

// The shortest text that reads back exactly, for f64 and f32.
static void expect_shortest_f64(NphStrBuilder *sb, f64 v, const char *expect)
{
    nph_sb_clear(sb);
    NTEST_ASSERT_EQ_INT(nph_sb_append_f64_shortest(sb, v), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(sb), expect);
}

static void expect_shortest_f32(NphStrBuilder *sb, f32 v, const char *expect)
{
    nph_sb_clear(sb);
    NTEST_ASSERT_EQ_INT(nph_sb_append_f32_shortest(sb, v), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(sb), expect);
}

NTEST_F(SbFixture, shortest_float_text_round_trips)
{
    expect_shortest_f64(&f->sb, 0.1, "0.1");
    expect_shortest_f64(&f->sb, 1.0, "1");
    expect_shortest_f64(&f->sb, -0.0, "-0");
    expect_shortest_f64(&f->sb, 1.0 / 3.0, "0.3333333333333333");
    expect_shortest_f64(&f->sb, 1e300, "1e+300");
    expect_shortest_f64(&f->sb, 10.0, "10");
    expect_shortest_f64(&f->sb, 1200.0, "1200");
    expect_shortest_f64(&f->sb, 1e16, "10000000000000000");
    expect_shortest_f64(&f->sb, 1e17, "1e+17");
    expect_shortest_f64(&f->sb, 1.5e-5, "1.5e-05");
    expect_shortest_f64(&f->sb, 0.001, "0.001");
    expect_shortest_f64(&f->sb, 5e-324, "5e-324");
    expect_shortest_f64(&f->sb, (f64)INFINITY, "inf");
    expect_shortest_f64(&f->sb, -(f64)INFINITY, "-inf");
    expect_shortest_f64(&f->sb, (f64)NAN, "nan");
    expect_shortest_f32(&f->sb, 0.1F, "0.1");
    expect_shortest_f32(&f->sb, 1.0F / 3.0F, "0.33333334");
    expect_shortest_f32(&f->sb, -0.0F, "-0");
    expect_shortest_f32(&f->sb, 16777216.0F, "16777216");
    expect_shortest_f32(&f->sb, 20.0F, "20");
    expect_shortest_f32(&f->sb, 1.0e10F, "10000000000");
    expect_shortest_f32(&f->sb, 3.4028235e38F, "3.4028235e+38");
    expect_shortest_f32(&f->sb, (f32)INFINITY, "inf");
    // Every value of a sweep reads back bit for bit.
    u32 bits = 0x00000001U;
    for (u32 i = 0; i < 200000U; ++i) {
        f32 v;
        memcpy(&v, &bits, sizeof v);
        bits = bits * 2654435761U + 12345U;
        if (isnan(v) || isinf(v)) {
            continue;
        }
        nph_sb_clear(&f->sb);
        NTEST_ASSERT_EQ_INT(nph_sb_append_f32_shortest(&f->sb, v), NPH_RESULT_OK);
        const f32 back = strtof(nph_sb_cstr(&f->sb), NULL);
        NTEST_ASSERT_EQ_MEM(&back, &v, sizeof v);
        const f32 via_double = (f32)strtod(nph_sb_cstr(&f->sb), NULL);
        NTEST_ASSERT_EQ_MEM(&via_double, &v, sizeof v);
        const f64 d = (f64)v * 1.0000001;
        nph_sb_clear(&f->sb);
        NTEST_ASSERT_EQ_INT(nph_sb_append_f64_shortest(&f->sb, d), NPH_RESULT_OK);
        const f64 dback = strtod(nph_sb_cstr(&f->sb), NULL);
        NTEST_ASSERT_EQ_MEM(&dback, &d, sizeof d);
    }
}

NTEST_F(SbFixture, grows_across_many_appends)
{
    for (u32 i = 0; i < 10000U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sb_appendf(&f->sb, "%u,", i), NPH_RESULT_OK);
    }
    NTEST_ASSERT_GT_UINT(f->sb.len, 40000U);
    NTEST_ASSERT_TRUE(nph_str_starts_with(nph_sb_str(&f->sb), NPH_STR("0,1,2,")));
    NTEST_ASSERT_TRUE(nph_str_ends_with(nph_sb_str(&f->sb), NPH_STR("9999,")));
    NTEST_ASSERT_EQ_UINT(strlen(nph_sb_cstr(&f->sb)), f->sb.len);
    NTEST_ASSERT_EQ_INT(nph_sb_reserve(&f->sb, 1U << 20U), NPH_RESULT_OK);
    NTEST_ASSERT_GE_UINT(f->sb.cap, (1U << 20U) + 1U);
}

NTEST(sb, allocation_failure)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[128];
    nph_arena_init(&arena, memory, sizeof memory);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_arena_allocator(&arena));
    NTEST_ASSERT_EQ_INT(nph_sb_append(&sb, NPH_STR("0123456789")), NPH_RESULT_OK);
    u32 appended = 1U;
    while (nph_sb_append(&sb, NPH_STR("0123456789")) == NPH_RESULT_OK) {
        ++appended;
        NTEST_ASSERT_LT_UINT(appended, 100U);
    }
    NTEST_ASSERT_EQ_UINT(sb.len, appended * 10U);
    NTEST_ASSERT_EQ_UINT(strlen(nph_sb_cstr(&sb)), sb.len);
    nph_sb_free(&sb);
    nph_arena_destroy(&arena);
}

// ---------------------------------------------------------------------------
// String IDs
// ---------------------------------------------------------------------------

NTEST(str_id, ids_are_hashes_and_never_zero)
{
    const NphStrId a = nph_str_id(NPH_STR("Health"));
    const NphStrId b = nph_str_id_cstr("Health");
    const NphStrId c = nph_str_id(NPH_STR("health"));
    NTEST_ASSERT_TRUE(nph_str_id_equals(a, b));
    NTEST_ASSERT_FALSE(nph_str_id_equals(a, c));
    NTEST_ASSERT_FALSE(nph_str_id_is_null(a));
    NTEST_ASSERT_TRUE(nph_str_id_is_null(NPH_STR_ID_NULL));
    NTEST_ASSERT_EQ_UINT(a.value, nph_str_hash(NPH_STR("Health")));
    NTEST_ASSERT_FALSE(nph_str_id_is_null(nph_str_id(nph_str_empty())));
}

NTEST(str_id, table_interns_and_looks_up)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    NphStrTable table;
    NTEST_ASSERT_EQ_INT(nph_str_table_init(&table, &da.allocator, 64U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_str_table_count(&table), 0U);
    NTEST_ASSERT_NULL(nph_str_table_lookup(&table, nph_str_id(NPH_STR("x"))).data);

    NphStrId id;
    NTEST_ASSERT_EQ_INT(nph_str_table_intern(&table, NPH_STR("Transform"), &id), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_id_equals(id, nph_str_id(NPH_STR("Transform"))));
    NphStrId again;
    NTEST_ASSERT_EQ_INT(nph_str_table_intern(&table, NPH_STR("Transform"), &again), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_id_equals(id, again));
    NTEST_ASSERT_EQ_UINT(nph_str_table_count(&table), 1U);
    const NphStr text = nph_str_table_lookup(&table, id);
    NTEST_ASSERT_TRUE(nph_str_equals(text, NPH_STR("Transform")));
    NTEST_ASSERT_EQ_UINT(text.data[text.len], 0U); // stored NUL-terminated

    // Many strings, spanning several 64-byte chunks, including one bigger
    // than a chunk.
    char buffer[300];
    for (u32 i = 0; i < 500U; ++i) {
        const int n = snprintf(buffer, sizeof buffer, "name_%u", i);
        NTEST_ASSERT_EQ_INT(nph_str_table_intern(&table, nph_str(buffer, (u64)n), &id),
                            NPH_RESULT_OK);
    }
    memset(buffer, 'L', sizeof buffer);
    const NphStr long_name = nph_str(buffer, sizeof buffer);
    NTEST_ASSERT_EQ_INT(nph_str_table_intern(&table, long_name, &id), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_str_table_lookup(&table, id), long_name));
    NTEST_ASSERT_EQ_UINT(nph_str_table_count(&table), 502U);
    for (u32 i = 0; i < 500U; ++i) {
        const int n = snprintf(buffer, sizeof buffer, "name_%u", i);
        const NphStr found = nph_str_table_lookup(&table, nph_str_id(nph_str(buffer, (u64)n)));
        NTEST_ASSERT_TRUE(nph_str_equals(found, nph_str(buffer, (u64)n)));
    }
    NTEST_ASSERT_GT_UINT(table.chunks.len, 10U);
    nph_str_table_free(&table);
    nph_str_table_free(&table);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
}

NTEST(str_id, table_reports_allocation_failure)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[1024];
    nph_arena_init(&arena, memory, sizeof memory);
    NphStrTable table;
    NTEST_ASSERT_EQ_INT(nph_str_table_init(&table, nph_arena_allocator(&arena), 128U),
                        NPH_RESULT_OK);
    NphStrId id;
    u32 interned = 0U;
    char buffer[32];
    for (u32 i = 0; i < 1000U; ++i) {
        const int n = snprintf(buffer, sizeof buffer, "s%u", i);
        const NphResult r = nph_str_table_intern(&table, nph_str(buffer, (u64)n), &id);
        if (r != NPH_RESULT_OK) {
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OUT_OF_MEMORY);
            NTEST_ASSERT_TRUE(nph_str_id_is_null(id));
            break;
        }
        ++interned;
    }
    NTEST_ASSERT_GT_UINT(interned, 0U);
    NTEST_ASSERT_LT_UINT(interned, 1000U);
    NTEST_ASSERT_EQ_UINT(nph_str_table_count(&table), interned);
    nph_str_table_free(&table);
    nph_arena_destroy(&arena);
}
