// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/json.h>

#include "test_support.h"

#include <math.h>
#include <string.h>

typedef struct JsonFixture {
    NphDebugAllocator da;
    NphAllocator *a;
    NphJsonDoc doc;
    NphStrBuilder sb;
} JsonFixture;

NTEST_SETUP(JsonFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
    nph_json_doc_init(&f->doc, f->a);
    nph_sb_init(&f->sb, f->a);
}

NTEST_TEARDOWN(JsonFixture)
{
    nph_json_doc_free(&f->doc);
    nph_sb_free(&f->sb);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

static NphResult parse(JsonFixture *f, const char *text)
{
    nph_json_doc_free(&f->doc);
    return nph_json_parse(nph_str_from_cstr(text), f->a, &f->doc);
}

NTEST_F(JsonFixture, parses_scalars_at_top_level)
{
    NTEST_ASSERT_EQ_INT(parse(f, " null "), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->type, NPH_JSON_NULL);
    NTEST_ASSERT_EQ_INT(parse(f, "true"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->type, NPH_JSON_BOOL);
    NTEST_ASSERT_EQ_UINT(f->doc.root->boolean, 1U);
    NTEST_ASSERT_EQ_INT(parse(f, "false"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->boolean, 0U);
    NTEST_ASSERT_EQ_INT(parse(f, "-42"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->type, NPH_JSON_NUMBER);
    NTEST_ASSERT_EQ_UINT(f->doc.root->is_integer, 1U);
    NTEST_ASSERT_EQ_INT(f->doc.root->integer, -42);
    NTEST_ASSERT_TRUE(f->doc.root->number == -42.0);
    NTEST_ASSERT_EQ_INT(parse(f, "1.5e3"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->is_integer, 0U);
    NTEST_ASSERT_TRUE(f->doc.root->number == 1500.0);
    NTEST_ASSERT_EQ_INT(parse(f, "0"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(f->doc.root->integer, 0);
    NTEST_ASSERT_EQ_INT(parse(f, "9223372036854775807"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(f->doc.root->integer, NPH_I64_MAX);
    NTEST_ASSERT_EQ_INT(parse(f, "9223372036854775808"), NPH_RESULT_OK); // too big: float only
    NTEST_ASSERT_EQ_UINT(f->doc.root->is_integer, 0U);
    NTEST_ASSERT_EQ_INT(parse(f, "\"text\""), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->type, NPH_JSON_STRING);
    NTEST_ASSERT_TRUE(nph_str_equals(f->doc.root->string, NPH_STR("text")));
    NTEST_ASSERT_EQ_UINT(f->doc.root->string.data[4], 0U);
}

NTEST_F(JsonFixture, parses_nested_structure)
{
    const char *text = "{\n"
                       "  \"name\": \"scene\",\n"
                       "  \"version\": 3,\n"
                       "  \"scale\": 0.5,\n"
                       "  \"visible\": true,\n"
                       "  \"parent\": null,\n"
                       "  \"tags\": [\"a\", \"b\", []],\n"
                       "  \"transform\": {\"pos\": [1, 2, 3], \"empty\": {}}\n"
                       "}";
    NTEST_ASSERT_EQ_INT(parse(f, text), NPH_RESULT_OK);
    const NphJsonValue *root = f->doc.root;
    NTEST_ASSERT_EQ_UINT(root->type, NPH_JSON_OBJECT);
    NTEST_ASSERT_EQ_UINT(root->count, 7U);
    NTEST_ASSERT_TRUE(
        nph_str_equals(nph_json_get_str(root, NPH_STR("name"), NPH_STR("?")), NPH_STR("scene")));
    NTEST_ASSERT_EQ_INT(nph_json_get_i64(root, NPH_STR("version"), -1), 3);
    NTEST_ASSERT_TRUE(nph_json_get_f64(root, NPH_STR("scale"), 0.0) == 0.5);
    NTEST_ASSERT_EQ_INT(nph_json_get_i64(root, NPH_STR("scale"), -1), 0); // truncated
    NTEST_ASSERT_TRUE(nph_json_get_bool(root, NPH_STR("visible"), false));
    NTEST_ASSERT_FALSE(nph_json_get_bool(root, NPH_STR("missing"), false));
    NTEST_ASSERT_EQ_INT(nph_json_get_i64(root, NPH_STR("name"), 7), 7); // wrong type
    NTEST_ASSERT_EQ_UINT(nph_json_object_get(root, NPH_STR("parent"))->type, NPH_JSON_NULL);
    NTEST_ASSERT_NULL(nph_json_object_get(root, NPH_STR("nope")));
    NTEST_ASSERT_NULL(nph_json_object_get(NULL, NPH_STR("x")));

    const NphJsonValue *tags = nph_json_object_get(root, NPH_STR("tags"));
    NTEST_ASSERT_EQ_UINT(tags->type, NPH_JSON_ARRAY);
    NTEST_ASSERT_EQ_UINT(tags->count, 3U);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_json_array_at(tags, 1U)->string, NPH_STR("b")));
    NTEST_ASSERT_EQ_UINT(nph_json_array_at(tags, 2U)->count, 0U);
    NTEST_ASSERT_NULL(nph_json_array_at(tags, 3U));
    NTEST_ASSERT_NULL(nph_json_array_at(root, 0U));
    u32 n = 0U;
    NPH_JSON_FOREACH(child, tags)
    {
        ++n;
    }
    NTEST_ASSERT_EQ_UINT(n, 3U);

    const NphJsonValue *pos =
        nph_json_object_get(nph_json_object_get(root, NPH_STR("transform")), NPH_STR("pos"));
    i64 z;
    NTEST_ASSERT_EQ_INT(nph_json_as_i64(nph_json_array_at(pos, 2U), &z), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(z, 3);
    bool b;
    NphStr s;
    f64 d;
    NTEST_ASSERT_EQ_INT(nph_json_as_bool(pos, &b), NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_EQ_INT(nph_json_as_str(pos, &s), NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_EQ_INT(nph_json_as_f64(pos, &d), NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_EQ_INT(nph_json_as_i64(nph_json_object_get(root, NPH_STR("scale")), &z),
                        NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_EQ_INT(nph_json_as_i64(NULL, &z), NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_GT_UINT(f->doc.node_count, 12U);
}

NTEST_F(JsonFixture, string_escapes_round_trip)
{
    const char *text = "\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t \\u0041 \\u00e9 \\u20ac \\ud83d\\ude00\"";
    NTEST_ASSERT_EQ_INT(parse(f, text), NPH_RESULT_OK);
    const NphStr expected = NPH_STR("a\"b\\c/d\b\f\n\r\t A \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80");
    NTEST_ASSERT_TRUE(nph_str_equals(f->doc.root->string, expected));
    NTEST_ASSERT_EQ_INT(nph_json_write(f->doc.root, &f->sb, false), NPH_RESULT_OK);
    // The writer uses the short escapes and leaves UTF-8 as bytes; a control
    // character without a short form becomes \u00XX.
    NTEST_ASSERT_EQ_STR(
        nph_sb_cstr(&f->sb),
        "\"a\\\"b\\\\c/d\\b\\f\\n\\r\\t A \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\"");
    nph_sb_clear(&f->sb);
    NTEST_ASSERT_EQ_INT(nph_json_write_escaped(&f->sb, NPH_STR("\x01x")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "\"\\u0001x\"");
}

NTEST_F(JsonFixture, rejects_malformed_input_with_positions)
{
    typedef struct Bad {
        const char *text;
        u32 line;
        u32 column;
    } Bad;
    const Bad cases[] = {
        {"", 1U, 1U},
        {"{", 1U, 2U},
        {"[1,]", 1U, 4U},
        {"{\"a\" 1}", 1U, 6U},
        {"{a: 1}", 1U, 2U},
        {"[1 2]", 1U, 4U},
        {"tru", 1U, 1U},
        {"nul", 1U, 1U},
        {"01", 1U, 2U},
        {"1.", 1U, 3U},
        {"1e", 1U, 3U},
        {"-", 1U, 2U},
        {"\"abc", 1U, 5U},
        {"\"a\\x\"", 1U, 3U},
        {"\"\\u12\"", 1U, 4U},
        {"\"\\ud800\"", 1U, 8U},
        {"\"\\udc00\"", 1U, 8U},
        {"\"\\ud800\\u0041\"", 1U, 14U},
        {"\"tab\there\"", 1U, 5U},
        {"[1] x", 1U, 5U},
        {"{\n  \"a\": 1,\n  \"b\": ?\n}", 3U, 8U},
        {"NaN", 1U, 1U},
        {"[1,,2]", 1U, 4U},
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(cases); ++i) {
        const NphResult r = parse(f, cases[i].text);
        if (r != NPH_RESULT_PARSE_ERROR) {
            NTEST_FAIL("case %u (%s): expected a parse error, got %s", i, cases[i].text,
                       nph_result_string(r));
        }
        NTEST_ASSERT_NULL(f->doc.root);
        NTEST_ASSERT_NOT_NULL(f->doc.error_message);
        if (f->doc.error_line != cases[i].line || f->doc.error_column != cases[i].column) {
            NTEST_FAIL("case %u (%s): error at %u:%u (%s), expected %u:%u", i, cases[i].text,
                       f->doc.error_line, f->doc.error_column, f->doc.error_message, cases[i].line,
                       cases[i].column);
        }
    }
}

// Negative zero keeps its sign through a write and a re-parse: "-0" is a
// double, not the integer 0, and so is a negative number that underflows
// (the input the JSON fuzzer reduced its failure to).
NTEST_F(JsonFixture, negative_zero_round_trips_as_a_double)
{
    static const char *const inputs[] = {"-0", "-1.5e-3333333333333333333333", "-0.0"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(inputs); ++i) {
        NTEST_ASSERT_EQ_INT(parse(f, inputs[i]), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(f->doc.root->is_integer, 0U);
        NTEST_ASSERT_TRUE(f->doc.root->number == 0.0 && signbit(f->doc.root->number));
        nph_sb_clear(&f->sb);
        NTEST_ASSERT_EQ_INT(nph_json_write(f->doc.root, &f->sb, false), NPH_RESULT_OK);
        NTEST_ASSERT_TRUE(nph_str_equals(nph_sb_str(&f->sb), NPH_STR("-0")));
        NphJsonDoc again;
        NTEST_ASSERT_EQ_INT(nph_json_parse(nph_sb_str(&f->sb), f->a, &again), NPH_RESULT_OK);
        const bool still_double = again.root->is_integer == 0U && signbit(again.root->number);
        nph_json_doc_free(&again);
        NTEST_ASSERT_TRUE(still_double);
    }
    // Zero without a sign stays the integer 0.
    NTEST_ASSERT_EQ_INT(parse(f, "0"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->is_integer, 1U);
}

NTEST_F(JsonFixture, nesting_and_number_limits)
{
    char deep[NPH_JSON_MAX_DEPTH * 2U + 2U];
    for (u32 i = 0; i < NPH_JSON_MAX_DEPTH; ++i) {
        deep[i] = '[';
        deep[NPH_JSON_MAX_DEPTH + i] = ']';
    }
    deep[(u64)NPH_JSON_MAX_DEPTH * 2U] = '\0';
    NTEST_ASSERT_EQ_INT(parse(f, deep), NPH_RESULT_OK);
    char deeper[NPH_JSON_MAX_DEPTH * 2U + 4U];
    for (u32 i = 0; i < NPH_JSON_MAX_DEPTH + 1U; ++i) {
        deeper[i] = '[';
        deeper[NPH_JSON_MAX_DEPTH + 1U + i] = ']';
    }
    deeper[(u64)NPH_JSON_MAX_DEPTH * 2U + 2U] = '\0';
    NTEST_ASSERT_EQ_INT(parse(f, deeper), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_STR(f->doc.error_message, "nesting too deep");

    char number[NPH_JSON_MAX_NUMBER_LEN + 2U];
    memset(number, '1', sizeof number - 1U);
    number[sizeof number - 1U] = '\0';
    NTEST_ASSERT_EQ_INT(parse(f, number), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_STR(f->doc.error_message, "number too long");
    number[NPH_JSON_MAX_NUMBER_LEN - 1U] = '\0';
    NTEST_ASSERT_EQ_INT(parse(f, number), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->doc.root->is_integer, 0U); // 63 digits: float only
}

NTEST_F(JsonFixture, build_write_and_reparse)
{
    NphJsonValue *root = nph_json_new_object(&f->doc);
    NTEST_ASSERT_NOT_NULL(root);
    NphJsonValue *list = nph_json_new_array(&f->doc);
    NTEST_ASSERT_EQ_INT(nph_json_array_push(list, nph_json_new_i64(&f->doc, 1)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_array_push(list, nph_json_new_f64(&f->doc, 0.25)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_array_push(list, nph_json_new_bool(&f->doc, false)),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_array_push(list, nph_json_new_null(&f->doc)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_object_set(&f->doc, root, NPH_STR("list"), list), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_object_set(&f->doc, root, NPH_STR("name"),
                                            nph_json_new_str(&f->doc, NPH_STR("x\ny"))),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_object_set(&f->doc, root, NPH_STR("name"),
                                            nph_json_new_str(&f->doc, NPH_STR("replaced"))),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(root->count, 2U);
    NTEST_ASSERT_EQ_INT(nph_json_array_push(root, list), NPH_RESULT_TYPE_MISMATCH);
    NTEST_ASSERT_EQ_INT(nph_json_object_set(&f->doc, list, NPH_STR("k"), list),
                        NPH_RESULT_TYPE_MISMATCH);
    f->doc.root = root;

    NTEST_ASSERT_EQ_INT(nph_json_write(root, &f->sb, false), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb),
                        "{\"list\":[1,0.25,false,null],\"name\":\"replaced\"}");
    nph_sb_clear(&f->sb);
    NTEST_ASSERT_EQ_INT(nph_json_write(root, &f->sb, true), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "{\n"
                                             "  \"list\": [\n"
                                             "    1,\n"
                                             "    0.25,\n"
                                             "    false,\n"
                                             "    null\n"
                                             "  ],\n"
                                             "  \"name\": \"replaced\"\n"
                                             "}");
    // Round trip through the parser gives the same compact text.
    NphJsonDoc again;
    NTEST_ASSERT_EQ_INT(nph_json_parse(nph_sb_str(&f->sb), f->a, &again), NPH_RESULT_OK);
    nph_sb_clear(&f->sb);
    NTEST_ASSERT_EQ_INT(nph_json_write(again.root, &f->sb, false), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb),
                        "{\"list\":[1,0.25,false,null],\"name\":\"replaced\"}");
    nph_json_doc_free(&again);
    // Non-finite numbers become null; empty containers stay compact when pretty.
    nph_sb_clear(&f->sb);
    NphJsonValue *odd = nph_json_new_array(&f->doc);
    NTEST_ASSERT_EQ_INT(nph_json_array_push(odd, nph_json_new_f64(&f->doc, (f64)INFINITY)),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_array_push(odd, nph_json_new_object(&f->doc)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_write(odd, &f->sb, true), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "[\n  null,\n  {}\n]");
}

NTEST_F(JsonFixture, inline_arrays_stay_on_one_line)
{
    NphJsonWriter w;
    nph_json_writer_init(&w, &f->sb, true);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("v")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array_inline(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f32_shortest(&w, 1.5F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array(&w), NPH_RESULT_OK); // nested: inline too
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, 2), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, 3), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("e")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array_inline(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("n")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array(&w), NPH_RESULT_OK); // not inline again
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, 4), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb),
                        "{\n  \"v\": [1.5, [2, 3]],\n  \"e\": [],\n  \"n\": [\n    4\n  ]\n}");
    // Compact output is unaffected.
    nph_sb_clear(&f->sb);
    nph_json_writer_init(&w, &f->sb, false);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array_inline(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, 1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, 2), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "[1,2]");
}

NTEST_F(JsonFixture, numbers_are_written_in_shortest_form)
{
    NphJsonWriter w;
    nph_json_writer_init(&w, &f->sb, false);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f64_shortest(&w, 0.1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f32_shortest(&w, 0.1F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f32_shortest(&w, (f32)NAN), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f64_shortest(&w, (f64)INFINITY), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f32_shortest(&w, -2.5F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f64(&w, 0.1), NPH_RESULT_OK); // the fast, long form
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "[0.1,0.1,null,null,-2.5,0.10000000000000001]");
}

NTEST_F(JsonFixture, streaming_writer)
{
    NphJsonWriter w;
    nph_json_writer_init(&w, &f->sb, false);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("a")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, -5), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("b")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_str(&w, NPH_STR("q\"")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f64(&w, 2.5), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_bool(&w, true), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_null(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("c")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_f64(&w, (f64)NAN), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb),
                        "{\"a\":-5,\"b\":[\"q\\\"\",2.5,true,null,{}],\"c\":null}");
    NTEST_ASSERT_EQ_UINT(w.depth, 0U);
    // Misuse is rejected.
    NphResult r = NPH_RESULT_OK;
    NPH_TEST_EXPECT_ASSERT(r = nph_json_writer_end_object(&w));
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_ARGUMENT);
    NPH_TEST_EXPECT_ASSERT(r = nph_json_writer_key(&w, NPH_STR("x")));
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_ARGUMENT);
    nph_sb_clear(&f->sb);
    nph_json_writer_init(&w, &f->sb, true);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array(&w), NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(r = nph_json_writer_key(&w, NPH_STR("x")));
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, 1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("k")), NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(r = nph_json_writer_end_object(&w)); // a value must follow the key
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, 2), NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(r = nph_json_writer_end_array(&w)); // mismatched
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_object(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(nph_sb_cstr(&f->sb), "[\n  1,\n  {\n    \"k\": 2\n  }\n]");
}

NTEST(json, allocation_failure_is_reported)
{
    NphArena arena;
    NPH_ALIGNAS(16) u8 memory[4096];
    nph_arena_init(&arena, memory, sizeof memory);
    NphJsonDoc doc;
    // Thousands of elements need more than one 16 KiB chunk... the arena
    // cannot even hold the first chunk, so parsing fails cleanly.
    NTEST_ASSERT_EQ_INT(nph_json_parse(NPH_STR("[1, 2, 3]"), nph_arena_allocator(&arena), &doc),
                        NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_NULL(doc.root);
    NTEST_ASSERT_EQ_STR(doc.error_message, "out of memory");
    nph_json_doc_free(&doc);
    nph_json_doc_init(&doc, nph_arena_allocator(&arena));
    NTEST_ASSERT_NULL(nph_json_new_object(&doc));
    nph_json_doc_free(&doc);
    nph_json_doc_free(&doc);
    nph_arena_destroy(&arena);
}

NTEST(json, large_document)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NphJsonWriter w;
    nph_json_writer_init(&w, &sb, false);
    NTEST_ASSERT_EQ_INT(nph_json_writer_begin_array(&w), NPH_RESULT_OK);
    for (u32 i = 0; i < 20000U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_json_writer_begin_object(&w), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("id")), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_json_writer_i64(&w, i), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_json_writer_key(&w, NPH_STR("name")), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_json_writer_str(&w, NPH_STR("entity \"n\"")), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_json_writer_end_object(&w), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_json_writer_end_array(&w), NPH_RESULT_OK);
    NphJsonDoc doc;
    NTEST_ASSERT_EQ_INT(nph_json_parse(nph_sb_str(&sb), nph_allocator_system(), &doc),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(doc.root->count, 20000U);
    NTEST_ASSERT_EQ_INT(nph_json_get_i64(nph_json_array_at(doc.root, 19999U), NPH_STR("id"), -1),
                        19999);
    NTEST_ASSERT_GT_UINT(doc.chunks.len, 4U);
    nph_json_doc_free(&doc);
    nph_sb_free(&sb);
}
