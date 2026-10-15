// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/json.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_CHUNK_SIZE   (16U * 1024U)
#define JSON_CHUNK_HEADER 16U

// ---------------------------------------------------------------------------
// Document storage: chunks of nodes and strings, freed together.
// ---------------------------------------------------------------------------

void nph_json_doc_init(NphJsonDoc *doc, NphAllocator *alloc)
{
    NPH_ASSERT(doc != NULL && alloc != NULL);
    memset(doc, 0, sizeof *doc);
    doc->alloc = alloc;
    NPH_ARRAY_INIT(&doc->chunks, alloc);
}

void nph_json_doc_free(NphJsonDoc *doc)
{
    if (doc == NULL || doc->alloc == NULL) {
        return;
    }
    NPH_ARRAY_FOREACH (void *, chunk, &doc->chunks) {
        u64 size;
        memcpy(&size, *chunk, sizeof size);
        NPH_FREE(doc->alloc, *chunk, size, 16U);
    }
    NPH_ARRAY_FREE(&doc->chunks);
    memset(doc, 0, sizeof *doc);
}

static void *doc_alloc(NphJsonDoc *doc, u64 bytes, u32 align)
{
    u64 padding = nph_align_up_u64((uptr)doc->chunk_cursor, align) - (uptr)doc->chunk_cursor;
    if (doc->chunk_cursor == NULL || bytes + padding > doc->chunk_remaining) {
        const u64 size = nph_max_u64((u64)JSON_CHUNK_SIZE, bytes + JSON_CHUNK_HEADER + align);
        u8 *chunk = (u8 *)NPH_ALLOC(doc->alloc, size, 16U);
        if (chunk == NULL) {
            return NULL;
        }
        if (NPH_ARRAY_PUSH(&doc->chunks, (void *)chunk) != NPH_RESULT_OK) {
            NPH_FREE(doc->alloc, chunk, size, 16U);
            return NULL;
        }
        memcpy(chunk, &size, sizeof size);
        doc->chunk_cursor = chunk + JSON_CHUNK_HEADER;
        doc->chunk_remaining = size - JSON_CHUNK_HEADER;
        padding = 0U;
    }
    u8 *p = doc->chunk_cursor + padding;
    doc->chunk_cursor += padding + bytes;
    doc->chunk_remaining -= padding + bytes;
    return p;
}

static NphJsonValue *new_node(NphJsonDoc *doc, NphJsonType type)
{
    NphJsonValue *v = (NphJsonValue *)doc_alloc(doc, sizeof *v, NPH_ALIGNOF(NphJsonValue));
    if (v == NULL) {
        return NULL;
    }
    memset(v, 0, sizeof *v);
    v->type = (u8)type;
    doc->node_count += 1U;
    return v;
}

// Copies `s` into the doc with a NUL terminator.
static bool doc_copy_str(NphJsonDoc *doc, NphStr s, NphStr *out)
{
    char *copy = (char *)doc_alloc(doc, s.len + 1U, 1U);
    if (copy == NULL) {
        return false;
    }
    if (s.len > 0U) {
        memcpy(copy, s.data, (size_t)s.len);
    }
    copy[s.len] = '\0';
    *out = nph_str(copy, s.len);
    return true;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

typedef struct Parser {
    NphJsonDoc *doc;
    const char *text;
    u64 len;
    u64 pos;
    u32 depth;
    NphResult result;
} Parser;

static NphResult fail(Parser *p, const char *message)
{
    if (p->result == NPH_RESULT_OK) {
        p->result = NPH_RESULT_PARSE_ERROR;
        u32 line = 1U;
        u32 column = 1U;
        const u64 limit = nph_min_u64(p->pos, p->len);
        for (u64 i = 0; i < limit; ++i) {
            if (p->text[i] == '\n') {
                ++line;
                column = 1U;
            } else {
                ++column;
            }
        }
        p->doc->error_line = line;
        p->doc->error_column = column;
        p->doc->error_message = message;
    }
    return p->result;
}

static NphResult fail_oom(Parser *p)
{
    if (p->result == NPH_RESULT_OK) {
        p->result = NPH_RESULT_OUT_OF_MEMORY;
        p->doc->error_message = "out of memory";
    }
    return p->result;
}

static void skip_whitespace(Parser *p)
{
    while (p->pos < p->len) {
        const char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++p->pos;
        } else {
            break;
        }
    }
}

static bool peek(const Parser *p, char c)
{
    return p->pos < p->len && p->text[p->pos] == c;
}

static NphResult parse_value(Parser *p, NphJsonValue **out);

static u32 hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return (u32)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (u32)(c - 'a') + 10U;
    }
    if (c >= 'A' && c <= 'F') {
        return (u32)(c - 'A') + 10U;
    }
    return 0xFFFFFFFFU;
}

static bool parse_hex4(Parser *p, u32 *out)
{
    if (p->len - p->pos < 4U) {
        return false;
    }
    u32 v = 0U;
    for (u32 i = 0; i < 4U; ++i) {
        const u32 h = hex_value(p->text[p->pos + i]);
        if (h == 0xFFFFFFFFU) {
            return false;
        }
        v = (v << 4U) | h;
    }
    p->pos += 4U;
    *out = v;
    return true;
}

// Parses a string token (the opening quote is at pos) into doc storage.
static NphResult parse_string(Parser *p, NphStr *out)
{
    NPH_ASSERT(peek(p, '"'));
    ++p->pos;
    // Two passes: measure the unescaped length, then copy. Pass 1 validates.
    const u64 start = p->pos;
    u64 unescaped = 0U;
    for (;;) {
        if (p->pos >= p->len) {
            return fail(p, "unterminated string");
        }
        const u8 c = (u8)p->text[p->pos];
        if (c == '"') {
            break;
        }
        if (c < 0x20U) {
            return fail(p, "control character in string");
        }
        if (c != '\\') {
            ++p->pos;
            ++unescaped;
            continue;
        }
        ++p->pos;
        if (p->pos >= p->len) {
            return fail(p, "unterminated escape");
        }
        const char e = p->text[p->pos++];
        switch (e) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            ++unescaped;
            break;
        case 'u': {
            u32 cp;
            if (!parse_hex4(p, &cp)) {
                return fail(p, "bad \\u escape");
            }
            if (cp >= 0xD800U && cp <= 0xDBFFU) {
                // High surrogate: a low surrogate escape must follow.
                u32 low;
                if (p->len - p->pos < 6U || p->text[p->pos] != '\\' ||
                    p->text[p->pos + 1U] != 'u') {
                    return fail(p, "lone high surrogate");
                }
                p->pos += 2U;
                if (!parse_hex4(p, &low) || low < 0xDC00U || low > 0xDFFFU) {
                    return fail(p, "bad low surrogate");
                }
                cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
            } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                return fail(p, "lone low surrogate");
            }
            char bytes[4];
            unescaped += nph_utf8_encode(cp, bytes);
            break;
        }
        default:
            p->pos -= 2U; // report at the backslash
            return fail(p, "bad escape");
        }
    }
    const u64 end = p->pos; // at the closing quote
    ++p->pos;

    char *copy = (char *)doc_alloc(p->doc, unescaped + 1U, 1U);
    if (copy == NULL) {
        return fail_oom(p);
    }
    u64 w = 0U;
    for (u64 i = start; i < end;) {
        const char c = p->text[i];
        if (c != '\\') {
            copy[w++] = c;
            ++i;
            continue;
        }
        const char e = p->text[i + 1U];
        i += 2U;
        switch (e) {
        case '"':
            copy[w++] = '"';
            break;
        case '\\':
            copy[w++] = '\\';
            break;
        case '/':
            copy[w++] = '/';
            break;
        case 'b':
            copy[w++] = '\b';
            break;
        case 'f':
            copy[w++] = '\f';
            break;
        case 'n':
            copy[w++] = '\n';
            break;
        case 'r':
            copy[w++] = '\r';
            break;
        case 't':
            copy[w++] = '\t';
            break;
        default: { // 'u', validated in pass 1
            u32 cp = 0U;
            for (u32 k = 0; k < 4U; ++k) {
                cp = (cp << 4U) | hex_value(p->text[i + k]);
            }
            i += 4U;
            if (cp >= 0xD800U && cp <= 0xDBFFU) {
                u32 low = 0U;
                for (u32 k = 0; k < 4U; ++k) {
                    low = (low << 4U) | hex_value(p->text[i + 2U + k]);
                }
                i += 6U;
                cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
            }
            w += nph_utf8_encode(cp, copy + w);
            break;
        }
        }
    }
    copy[w] = '\0';
    *out = nph_str(copy, w);
    return NPH_RESULT_OK;
}

static NphResult parse_number(Parser *p, NphJsonValue *v)
{
    const u64 start = p->pos;
    bool is_integer = true;
    if (peek(p, '-')) {
        ++p->pos;
    }
    if (peek(p, '0')) {
        ++p->pos;
    } else if (p->pos < p->len && p->text[p->pos] >= '1' && p->text[p->pos] <= '9') {
        while (p->pos < p->len && nph_ascii_is_digit(p->text[p->pos])) {
            ++p->pos;
        }
    } else {
        return fail(p, "bad number");
    }
    if (peek(p, '.')) {
        is_integer = false;
        ++p->pos;
        if (p->pos >= p->len || !nph_ascii_is_digit(p->text[p->pos])) {
            return fail(p, "bad fraction");
        }
        while (p->pos < p->len && nph_ascii_is_digit(p->text[p->pos])) {
            ++p->pos;
        }
    }
    if (peek(p, 'e') || peek(p, 'E')) {
        is_integer = false;
        ++p->pos;
        if (peek(p, '+') || peek(p, '-')) {
            ++p->pos;
        }
        if (p->pos >= p->len || !nph_ascii_is_digit(p->text[p->pos])) {
            return fail(p, "bad exponent");
        }
        while (p->pos < p->len && nph_ascii_is_digit(p->text[p->pos])) {
            ++p->pos;
        }
    }
    const u64 len = p->pos - start;
    if (len >= NPH_JSON_MAX_NUMBER_LEN) {
        return fail(p, "number too long");
    }
    char buffer[NPH_JSON_MAX_NUMBER_LEN];
    memcpy(buffer, p->text + start, (size_t)len);
    buffer[len] = '\0';
    char *end = NULL;
    v->number = strtod(buffer, &end);
    if (end != buffer + len) {
        return fail(p, "bad number");
    }
    // "-0" is the integer 0 but the double -0.0, so it stays a double and
    // writes back as "-0" rather than "0" (as does any negative number that
    // underflows to -0.0): writing then re-parsing is a fixed point.
    if (is_integer && nph_str_parse_i64(nph_str(buffer, len), &v->integer) &&
        !(v->integer == 0 && buffer[0] == '-')) {
        v->is_integer = 1U;
    }
    return NPH_RESULT_OK;
}

static bool match_literal(Parser *p, const char *literal)
{
    const u64 n = (u64)strlen(literal);
    if (p->len - p->pos < n || memcmp(p->text + p->pos, literal, (size_t)n) != 0) {
        return false;
    }
    p->pos += n;
    return true;
}

static void append_child(NphJsonValue *parent, NphJsonValue *child)
{
    if (parent->last_child != NULL) {
        parent->last_child->next = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
    parent->count += 1U;
}

static NphResult parse_container(Parser *p, NphJsonValue *container, bool is_object)
{
    const char close = is_object ? '}' : ']';
    if (++p->depth > NPH_JSON_MAX_DEPTH) {
        return fail(p, "nesting too deep");
    }
    ++p->pos; // the opening bracket
    skip_whitespace(p);
    if (peek(p, close)) {
        ++p->pos;
        --p->depth;
        return NPH_RESULT_OK;
    }
    for (;;) {
        skip_whitespace(p);
        NphStr key = nph_str_empty();
        if (is_object) {
            if (!peek(p, '"')) {
                return fail(p, "expected a member name");
            }
            NPH_TRY(parse_string(p, &key));
            skip_whitespace(p);
            if (!peek(p, ':')) {
                return fail(p, "expected ':'");
            }
            ++p->pos;
            skip_whitespace(p);
        }
        // parse_value writes *out on every NPH_RESULT_OK path; the assert
        // states that contract for the static analyzer, which cannot always
        // see through the recursion.
        NphJsonValue *child = NULL;
        NPH_TRY(parse_value(p, &child));
        NPH_ASSERT(child != NULL);
        child->key = key;
        append_child(container, child);
        skip_whitespace(p);
        if (peek(p, ',')) {
            ++p->pos;
            continue;
        }
        if (peek(p, close)) {
            ++p->pos;
            --p->depth;
            return NPH_RESULT_OK;
        }
        return fail(p, is_object ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

static NphResult parse_value(Parser *p, NphJsonValue **out)
{
    skip_whitespace(p);
    if (p->pos >= p->len) {
        return fail(p, "unexpected end of input");
    }
    const char c = p->text[p->pos];
    NphJsonType type;
    if (c == '{') {
        type = NPH_JSON_OBJECT;
    } else if (c == '[') {
        type = NPH_JSON_ARRAY;
    } else if (c == '"') {
        type = NPH_JSON_STRING;
    } else if (c == '-' || nph_ascii_is_digit(c)) {
        type = NPH_JSON_NUMBER;
    } else if (c == 't' || c == 'f') {
        type = NPH_JSON_BOOL;
    } else if (c == 'n') {
        type = NPH_JSON_NULL;
    } else {
        return fail(p, "unexpected character");
    }
    NphJsonValue *v = new_node(p->doc, type);
    if (v == NULL) {
        return fail_oom(p);
    }
    switch (type) {
    case NPH_JSON_OBJECT:
        NPH_TRY(parse_container(p, v, true));
        break;
    case NPH_JSON_ARRAY:
        NPH_TRY(parse_container(p, v, false));
        break;
    case NPH_JSON_STRING:
        NPH_TRY(parse_string(p, &v->string));
        break;
    case NPH_JSON_NUMBER:
        NPH_TRY(parse_number(p, v));
        break;
    case NPH_JSON_BOOL:
        if (match_literal(p, "true")) {
            v->boolean = 1U;
        } else if (!match_literal(p, "false")) {
            return fail(p, "bad literal");
        }
        break;
    case NPH_JSON_NULL:
        if (!match_literal(p, "null")) {
            return fail(p, "bad literal");
        }
        break;
    }
    *out = v;
    return NPH_RESULT_OK;
}

NphResult nph_json_parse(NphStr text, NphAllocator *alloc, NphJsonDoc *out)
{
    NPH_ASSERT(out != NULL && alloc != NULL);
    nph_json_doc_init(out, alloc);
    Parser p = {out, text.data != NULL ? text.data : "", text.len, 0U, 0U, NPH_RESULT_OK};
    NphJsonValue *root = NULL;
    NphResult r = parse_value(&p, &root);
    if (r == NPH_RESULT_OK) {
        skip_whitespace(&p);
        if (p.pos < p.len) {
            r = fail(&p, "trailing characters");
        }
    }
    out->root = r == NPH_RESULT_OK ? root : NULL;
    return r;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

const NphJsonValue *nph_json_object_get(const NphJsonValue *object, NphStr key)
{
    if (object == NULL || object->type != NPH_JSON_OBJECT) {
        return NULL;
    }
    NPH_JSON_FOREACH(child, object)
    {
        if (nph_str_equals(child->key, key)) {
            return child;
        }
    }
    return NULL;
}

const NphJsonValue *nph_json_array_at(const NphJsonValue *array, u32 index)
{
    if (array == NULL || array->type != NPH_JSON_ARRAY || index >= array->count) {
        return NULL;
    }
    const NphJsonValue *child = array->first_child;
    for (u32 i = 0; i < index && child != NULL; ++i) {
        child = child->next;
    }
    return child;
}

NphResult nph_json_as_bool(const NphJsonValue *v, bool *out)
{
    if (v == NULL || v->type != NPH_JSON_BOOL) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    *out = v->boolean != 0U;
    return NPH_RESULT_OK;
}

NphResult nph_json_as_i64(const NphJsonValue *v, i64 *out)
{
    if (v == NULL || v->type != NPH_JSON_NUMBER || !v->is_integer) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    *out = v->integer;
    return NPH_RESULT_OK;
}

NphResult nph_json_as_f64(const NphJsonValue *v, f64 *out)
{
    if (v == NULL || v->type != NPH_JSON_NUMBER) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    *out = v->number;
    return NPH_RESULT_OK;
}

NphResult nph_json_as_str(const NphJsonValue *v, NphStr *out)
{
    if (v == NULL || v->type != NPH_JSON_STRING) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    *out = v->string;
    return NPH_RESULT_OK;
}

bool nph_json_get_bool(const NphJsonValue *object, NphStr key, bool fallback)
{
    bool out;
    return nph_json_as_bool(nph_json_object_get(object, key), &out) == NPH_RESULT_OK ? out
                                                                                     : fallback;
}

i64 nph_json_get_i64(const NphJsonValue *object, NphStr key, i64 fallback)
{
    const NphJsonValue *v = nph_json_object_get(object, key);
    if (v == NULL || v->type != NPH_JSON_NUMBER) {
        return fallback;
    }
    if (v->is_integer) {
        return v->integer;
    }
    if (v->number >= -9.2233720368547758e18 && v->number < 9.2233720368547758e18) {
        return (i64)v->number;
    }
    return fallback;
}

f64 nph_json_get_f64(const NphJsonValue *object, NphStr key, f64 fallback)
{
    f64 out;
    return nph_json_as_f64(nph_json_object_get(object, key), &out) == NPH_RESULT_OK ? out
                                                                                    : fallback;
}

NphStr nph_json_get_str(const NphJsonValue *object, NphStr key, NphStr fallback)
{
    NphStr out;
    return nph_json_as_str(nph_json_object_get(object, key), &out) == NPH_RESULT_OK ? out
                                                                                    : fallback;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

NphJsonValue *nph_json_new_null(NphJsonDoc *doc)
{
    return new_node(doc, NPH_JSON_NULL);
}

NphJsonValue *nph_json_new_bool(NphJsonDoc *doc, bool value)
{
    NphJsonValue *v = new_node(doc, NPH_JSON_BOOL);
    if (v != NULL) {
        v->boolean = value ? 1U : 0U;
    }
    return v;
}

NphJsonValue *nph_json_new_i64(NphJsonDoc *doc, i64 value)
{
    NphJsonValue *v = new_node(doc, NPH_JSON_NUMBER);
    if (v != NULL) {
        v->integer = value;
        v->number = (f64)value;
        v->is_integer = 1U;
    }
    return v;
}

NphJsonValue *nph_json_new_f64(NphJsonDoc *doc, f64 value)
{
    NphJsonValue *v = new_node(doc, NPH_JSON_NUMBER);
    if (v != NULL) {
        v->number = value;
    }
    return v;
}

NphJsonValue *nph_json_new_str(NphJsonDoc *doc, NphStr value)
{
    NphJsonValue *v = new_node(doc, NPH_JSON_STRING);
    if (v != NULL && !doc_copy_str(doc, value, &v->string)) {
        return NULL;
    }
    return v;
}

NphJsonValue *nph_json_new_array(NphJsonDoc *doc)
{
    return new_node(doc, NPH_JSON_ARRAY);
}

NphJsonValue *nph_json_new_object(NphJsonDoc *doc)
{
    return new_node(doc, NPH_JSON_OBJECT);
}

NphResult nph_json_array_push(NphJsonValue *array, NphJsonValue *value)
{
    NPH_ASSERT(array != NULL && value != NULL);
    if (array->type != NPH_JSON_ARRAY) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    append_child(array, value);
    return NPH_RESULT_OK;
}

NphResult nph_json_object_set(NphJsonDoc *doc, NphJsonValue *object, NphStr key,
                              NphJsonValue *value)
{
    NPH_ASSERT(doc != NULL && object != NULL && value != NULL);
    if (object->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    if (!doc_copy_str(doc, key, &value->key)) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    // Replace an existing member in place to keep the order stable.
    NphJsonValue *prev = NULL;
    for (NphJsonValue *child = object->first_child; child != NULL; child = child->next) {
        if (nph_str_equals(child->key, key)) {
            value->next = child->next;
            if (prev != NULL) {
                prev->next = value;
            } else {
                object->first_child = value;
            }
            if (object->last_child == child) {
                object->last_child = value;
            }
            return NPH_RESULT_OK;
        }
        prev = child;
    }
    append_child(object, value);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

NphResult nph_json_write_escaped(NphStrBuilder *out, NphStr s)
{
    NPH_TRY(nph_sb_append_char(out, '"'));
    u64 run_start = 0U;
    for (u64 i = 0; i < s.len; ++i) {
        const u8 c = (u8)s.data[i];
        const char *escape = NULL;
        char buffer[8];
        switch (c) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        default:
            if (c < 0x20U) {
                snprintf(buffer, sizeof buffer, "\\u%04x", (unsigned)c);
                escape = buffer;
            }
            break;
        }
        if (escape != NULL) {
            NPH_TRY(nph_sb_append(out, nph_str_slice(s, run_start, i)));
            NPH_TRY(nph_sb_append_cstr(out, escape));
            run_start = i + 1U;
        }
    }
    NPH_TRY(nph_sb_append(out, nph_str_slice(s, run_start, s.len)));
    return nph_sb_append_char(out, '"');
}

static NphResult write_number(NphStrBuilder *out, const NphJsonValue *v)
{
    if (v->is_integer) {
        return nph_sb_append_i64(out, v->integer);
    }
    if (isnan(v->number) || isinf(v->number)) {
        return nph_sb_append_cstr(out, "null");
    }
    return nph_sb_append_f64(out, v->number);
}

static NphResult write_indent(NphStrBuilder *out, u32 depth)
{
    NPH_TRY(nph_sb_append_char(out, '\n'));
    for (u32 i = 0; i < depth; ++i) {
        NPH_TRY(nph_sb_append_cstr(out, "  "));
    }
    return NPH_RESULT_OK;
}

static NphResult write_value(const NphJsonValue *v, NphStrBuilder *out, bool pretty, u32 depth)
{
    switch ((NphJsonType)v->type) {
    case NPH_JSON_NULL:
        return nph_sb_append_cstr(out, "null");
    case NPH_JSON_BOOL:
        return nph_sb_append_cstr(out, v->boolean ? "true" : "false");
    case NPH_JSON_NUMBER:
        return write_number(out, v);
    case NPH_JSON_STRING:
        return nph_json_write_escaped(out, v->string);
    case NPH_JSON_ARRAY:
    case NPH_JSON_OBJECT: {
        const bool is_object = v->type == NPH_JSON_OBJECT;
        NPH_TRY(nph_sb_append_char(out, is_object ? '{' : '['));
        if (v->first_child == NULL) {
            return nph_sb_append_char(out, is_object ? '}' : ']');
        }
        if (depth >= NPH_JSON_MAX_DEPTH) {
            return NPH_RESULT_OVERFLOW;
        }
        bool first = true;
        NPH_JSON_FOREACH(child, v)
        {
            if (!first) {
                NPH_TRY(nph_sb_append_char(out, ','));
            }
            first = false;
            if (pretty) {
                NPH_TRY(write_indent(out, depth + 1U));
            }
            if (is_object) {
                NPH_TRY(nph_json_write_escaped(out, child->key));
                NPH_TRY(nph_sb_append_cstr(out, pretty ? ": " : ":"));
            }
            NPH_TRY(write_value(child, out, pretty, depth + 1U));
        }
        if (pretty) {
            NPH_TRY(write_indent(out, depth));
        }
        return nph_sb_append_char(out, is_object ? '}' : ']');
    }
    }
    return NPH_RESULT_TYPE_MISMATCH;
}

NphResult nph_json_write(const NphJsonValue *value, NphStrBuilder *out, bool pretty)
{
    NPH_ASSERT(value != NULL && out != NULL);
    return write_value(value, out, pretty, 0U);
}

// --- streaming writer ---

void nph_json_writer_init(NphJsonWriter *w, NphStrBuilder *out, bool pretty)
{
    NPH_ASSERT(w != NULL && out != NULL);
    memset(w, 0, sizeof *w);
    w->out = out;
    w->pretty = pretty;
}

// Emits the separator and indentation before a value or key at the
// current level.
static NphResult writer_before_item(NphJsonWriter *w, bool is_key)
{
    if (w->depth == 0U) {
        NPH_ASSERT_MSG(!is_key, "a key needs an enclosing object");
        return is_key ? NPH_RESULT_INVALID_ARGUMENT : NPH_RESULT_OK;
    }
    const u32 level = w->depth - 1U;
    const bool in_object = w->is_object[level] != 0U;
    if (w->expect_value) {
        NPH_ASSERT_MSG(!is_key, "a value must follow a key");
        if (is_key) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        w->expect_value = false;
        return NPH_RESULT_OK; // the key already emitted the separator
    }
    NPH_ASSERT_MSG(in_object == is_key, in_object ? "objects take keys" : "arrays take values");
    if (in_object != is_key) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (w->has_items[level]) {
        NPH_TRY(nph_sb_append_char(w->out, ','));
    }
    w->has_items[level] = 1U;
    if (w->pretty) {
        NPH_TRY(write_indent(w->out, w->depth));
    }
    return NPH_RESULT_OK;
}

static NphResult writer_begin(NphJsonWriter *w, bool is_object)
{
    NPH_TRY(writer_before_item(w, false));
    if (w->depth >= NPH_JSON_MAX_DEPTH) {
        return NPH_RESULT_OVERFLOW;
    }
    NPH_TRY(nph_sb_append_char(w->out, is_object ? '{' : '['));
    w->is_object[w->depth] = is_object ? 1U : 0U;
    w->has_items[w->depth] = 0U;
    w->depth += 1U;
    return NPH_RESULT_OK;
}

static NphResult writer_end(NphJsonWriter *w, bool is_object)
{
    NPH_ASSERT_MSG(w->depth > 0U && !w->expect_value, "nothing to end");
    if (w->depth == 0U || w->expect_value) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 level = w->depth - 1U;
    NPH_ASSERT_MSG((w->is_object[level] != 0U) == is_object, "mismatched end");
    if ((w->is_object[level] != 0U) != is_object) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    w->depth -= 1U;
    if (w->pretty && w->has_items[level]) {
        NPH_TRY(write_indent(w->out, w->depth));
    }
    return nph_sb_append_char(w->out, is_object ? '}' : ']');
}

NphResult nph_json_writer_begin_object(NphJsonWriter *w)
{
    return writer_begin(w, true);
}
NphResult nph_json_writer_end_object(NphJsonWriter *w)
{
    return writer_end(w, true);
}
NphResult nph_json_writer_begin_array(NphJsonWriter *w)
{
    return writer_begin(w, false);
}
NphResult nph_json_writer_end_array(NphJsonWriter *w)
{
    return writer_end(w, false);
}

NphResult nph_json_writer_key(NphJsonWriter *w, NphStr key)
{
    NPH_TRY(writer_before_item(w, true));
    NPH_TRY(nph_json_write_escaped(w->out, key));
    NPH_TRY(nph_sb_append_cstr(w->out, w->pretty ? ": " : ":"));
    w->expect_value = true;
    return NPH_RESULT_OK;
}

NphResult nph_json_writer_null(NphJsonWriter *w)
{
    NPH_TRY(writer_before_item(w, false));
    return nph_sb_append_cstr(w->out, "null");
}

NphResult nph_json_writer_bool(NphJsonWriter *w, bool value)
{
    NPH_TRY(writer_before_item(w, false));
    return nph_sb_append_cstr(w->out, value ? "true" : "false");
}

NphResult nph_json_writer_i64(NphJsonWriter *w, i64 value)
{
    NPH_TRY(writer_before_item(w, false));
    return nph_sb_append_i64(w->out, value);
}

NphResult nph_json_writer_f64(NphJsonWriter *w, f64 value)
{
    NPH_TRY(writer_before_item(w, false));
    if (isnan(value) || isinf(value)) {
        return nph_sb_append_cstr(w->out, "null");
    }
    return nph_sb_append_f64(w->out, value);
}

NphResult nph_json_writer_str(NphJsonWriter *w, NphStr value)
{
    NPH_TRY(writer_before_item(w, false));
    return nph_json_write_escaped(w->out, value);
}
