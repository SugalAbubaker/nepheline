// SPDX-License-Identifier: Apache-2.0
#pragma once

// JSON (CLAUDE.md §8.1 "a simple text format (JSON subset) for editor and
// scene files").
//
// Strict RFC 8259 JSON: objects, arrays, strings with escapes (including
// \uXXXX surrogate pairs), numbers, true/false/null, any value at the top
// level. No comments, no trailing commas, no NaN/Infinity: the files are
// written by tools and diffed in git, so one canonical form matters more
// than hand-editing conveniences.
//
// The reader builds a document tree whose nodes and strings live in
// chunks owned by the NphJsonDoc (one free call). Nesting is limited to
// NPH_JSON_MAX_DEPTH, numbers to NPH_JSON_MAX_NUMBER_LEN characters, so
// hostile input cannot exhaust the stack; the parser is fuzzed
// (tests/fuzz). The writer emits either compact or pretty (two-space)
// text from a tree, and a streaming writer emits directly without a tree.
//
// Thread-safety: a document and a writer belong to one thread.

#include <nepheline/core/alloc.h>
#include <nepheline/core/array.h>
#include <nepheline/core/result.h>
#include <nepheline/core/string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_JSON_MAX_DEPTH      128U
#define NPH_JSON_MAX_NUMBER_LEN 64U

typedef enum NphJsonType {
    NPH_JSON_NULL,
    NPH_JSON_BOOL,
    NPH_JSON_NUMBER,
    NPH_JSON_STRING,
    NPH_JSON_ARRAY,
    NPH_JSON_OBJECT,
} NphJsonType;

typedef struct NphJsonValue NphJsonValue;
struct NphJsonValue {
    NphStr key;    // member name when the parent is an object (NUL-terminated)
    NphStr string; // NPH_JSON_STRING: the unescaped text (NUL-terminated)
    NphJsonValue *first_child;
    NphJsonValue *last_child;
    NphJsonValue *next; // next sibling
    f64 number;         // NPH_JSON_NUMBER, always set
    i64 integer;        // NPH_JSON_NUMBER when is_integer
    u32 count;          // children of an array or object
    u8 type;            // NphJsonType
    u8 is_integer;      // the number had no fraction or exponent and fits i64
    u8 boolean;
};

typedef struct NphJsonDoc {
    NphJsonValue *root;
    NphArrayPtr chunks; // owned blocks holding nodes and strings
    u8 *chunk_cursor;
    u64 chunk_remaining;
    NphAllocator *alloc;
    u32 node_count;
    // Set by nph_json_parse on NPH_RESULT_PARSE_ERROR.
    u32 error_line;            // 1-based
    u32 error_column;          // 1-based, in bytes
    const char *error_message; // static text
} NphJsonDoc;

// Parses `text` into *out (which this call initialises). On
// NPH_RESULT_PARSE_ERROR the error_* fields describe the failure; the doc
// must still be freed. NPH_RESULT_OUT_OF_MEMORY on allocation failure.
NPH_MUST_USE NphResult nph_json_parse(NphStr text, NphAllocator *alloc, NphJsonDoc *out);

// An empty document for building a tree with the nph_json_new_* functions;
// set doc->root to the top value.
void nph_json_doc_init(NphJsonDoc *doc, NphAllocator *alloc);
void nph_json_doc_free(NphJsonDoc *doc);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Member `key` of an object, or NULL (also when `object` is not an object
// or is NULL). The first match wins for duplicate keys.
const NphJsonValue *nph_json_object_get(const NphJsonValue *object, NphStr key);
// Element `index` of an array, or NULL. O(index).
const NphJsonValue *nph_json_array_at(const NphJsonValue *array, u32 index);

// Lenient member access with fallbacks (missing or wrong type gives the
// fallback). Numbers are converted; a float with a fraction is truncated
// by get_i64.
bool nph_json_get_bool(const NphJsonValue *object, NphStr key, bool fallback);
i64 nph_json_get_i64(const NphJsonValue *object, NphStr key, i64 fallback);
f64 nph_json_get_f64(const NphJsonValue *object, NphStr key, f64 fallback);
NphStr nph_json_get_str(const NphJsonValue *object, NphStr key, NphStr fallback);

// Strict conversions: NPH_RESULT_TYPE_MISMATCH when the value is NULL or
// another type (as_i64 also rejects numbers that are not integers).
NPH_MUST_USE NphResult nph_json_as_bool(const NphJsonValue *v, bool *out);
NPH_MUST_USE NphResult nph_json_as_i64(const NphJsonValue *v, i64 *out);
NPH_MUST_USE NphResult nph_json_as_f64(const NphJsonValue *v, f64 *out);
NPH_MUST_USE NphResult nph_json_as_str(const NphJsonValue *v, NphStr *out);

// (`child` is a declarator and cannot be parenthesised.)
// NOLINTBEGIN(bugprone-macro-parentheses)
#define NPH_JSON_FOREACH(child, parent)                                                            \
    for (const NphJsonValue *child = (parent)->first_child; child != NULL; child = child->next)
// NOLINTEND(bugprone-macro-parentheses)

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

// Each returns a node owned by `doc`, or NULL when out of memory.
NphJsonValue *nph_json_new_null(NphJsonDoc *doc);
NphJsonValue *nph_json_new_bool(NphJsonDoc *doc, bool value);
NphJsonValue *nph_json_new_i64(NphJsonDoc *doc, i64 value);
NphJsonValue *nph_json_new_f64(NphJsonDoc *doc, f64 value);
NphJsonValue *nph_json_new_str(NphJsonDoc *doc, NphStr value); // copied
NphJsonValue *nph_json_new_array(NphJsonDoc *doc);
NphJsonValue *nph_json_new_object(NphJsonDoc *doc);

// Appends `value` (a node of the same doc, not yet attached) to `array`.
NPH_MUST_USE NphResult nph_json_array_push(NphJsonValue *array, NphJsonValue *value);
// Sets member `key` of `object` to `value`, replacing an existing member
// of that name (the key is copied into the doc).
NPH_MUST_USE NphResult nph_json_object_set(NphJsonDoc *doc, NphJsonValue *object, NphStr key,
                                           NphJsonValue *value);

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// Appends `value` as JSON text. Pretty output uses two-space indents and
// one member or element per line. Non-finite numbers are written as null.
NPH_MUST_USE NphResult nph_json_write(const NphJsonValue *value, NphStrBuilder *out, bool pretty);

// Streaming writer: emits well-formed JSON without building a tree. Misuse
// (a value where a key is required, mismatched end) is asserted.
typedef struct NphJsonWriter {
    NphStrBuilder *out;
    bool pretty;
    u32 depth;
    u8 is_object[NPH_JSON_MAX_DEPTH]; // container kind per level
    u8 has_items[NPH_JSON_MAX_DEPTH]; // whether a comma is needed
    bool expect_value;                // a key was written; a value must follow
} NphJsonWriter;

void nph_json_writer_init(NphJsonWriter *w, NphStrBuilder *out, bool pretty);
NPH_MUST_USE NphResult nph_json_writer_begin_object(NphJsonWriter *w);
NPH_MUST_USE NphResult nph_json_writer_end_object(NphJsonWriter *w);
NPH_MUST_USE NphResult nph_json_writer_begin_array(NphJsonWriter *w);
NPH_MUST_USE NphResult nph_json_writer_end_array(NphJsonWriter *w);
NPH_MUST_USE NphResult nph_json_writer_key(NphJsonWriter *w, NphStr key);
NPH_MUST_USE NphResult nph_json_writer_null(NphJsonWriter *w);
NPH_MUST_USE NphResult nph_json_writer_bool(NphJsonWriter *w, bool value);
NPH_MUST_USE NphResult nph_json_writer_i64(NphJsonWriter *w, i64 value);
NPH_MUST_USE NphResult nph_json_writer_f64(NphJsonWriter *w, f64 value);
NPH_MUST_USE NphResult nph_json_writer_str(NphJsonWriter *w, NphStr value);

// Appends `s` as a quoted, escaped JSON string.
NPH_MUST_USE NphResult nph_json_write_escaped(NphStrBuilder *out, NphStr s);

#ifdef __cplusplus
}
#endif
