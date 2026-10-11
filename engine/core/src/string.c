// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/string.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

NphStr nph_str_from_cstr(const char *s)
{
    return s != NULL ? nph_str(s, (u64)strlen(s)) : nph_str_empty();
}

NphStr nph_str_slice(NphStr s, u64 begin, u64 end)
{
    if (end > s.len) {
        end = s.len;
    }
    if (begin > end) {
        begin = end;
    }
    return nph_str(s.data + begin, end - begin);
}

bool nph_str_equals(NphStr a, NphStr b)
{
    return a.len == b.len && (a.len == 0U || memcmp(a.data, b.data, (size_t)a.len) == 0);
}

bool nph_str_equals_ignore_case(NphStr a, NphStr b)
{
    if (a.len != b.len) {
        return false;
    }
    for (u64 i = 0; i < a.len; ++i) {
        if (nph_ascii_to_lower(a.data[i]) != nph_ascii_to_lower(b.data[i])) {
            return false;
        }
    }
    return true;
}

i32 nph_str_compare(NphStr a, NphStr b)
{
    const u64 n = nph_min_u64(a.len, b.len);
    const int c = n > 0U ? memcmp(a.data, b.data, (size_t)n) : 0;
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return a.len < b.len ? -1 : (a.len > b.len ? 1 : 0);
}

bool nph_str_starts_with(NphStr s, NphStr prefix)
{
    return prefix.len <= s.len && nph_str_equals(nph_str_prefix(s, prefix.len), prefix);
}

bool nph_str_ends_with(NphStr s, NphStr suffix)
{
    return suffix.len <= s.len && nph_str_equals(nph_str_suffix(s, suffix.len), suffix);
}

u64 nph_str_find(NphStr s, NphStr needle)
{
    if (needle.len == 0U) {
        return 0U;
    }
    if (needle.len > s.len) {
        return NPH_STR_NPOS;
    }
    const char first = needle.data[0];
    const u64 last_start = s.len - needle.len;
    for (u64 i = 0; i <= last_start; ++i) {
        if (s.data[i] == first && memcmp(s.data + i, needle.data, (size_t)needle.len) == 0) {
            return i;
        }
    }
    return NPH_STR_NPOS;
}

bool nph_str_contains(NphStr s, NphStr needle)
{
    return nph_str_find(s, needle) != NPH_STR_NPOS;
}

u64 nph_str_find_char(NphStr s, char c)
{
    if (s.len == 0U) {
        return NPH_STR_NPOS;
    }
    const char *p = (const char *)memchr(s.data, c, (size_t)s.len);
    return p != NULL ? (u64)(p - s.data) : NPH_STR_NPOS;
}

u64 nph_str_rfind_char(NphStr s, char c)
{
    for (u64 i = s.len; i > 0U; --i) {
        if (s.data[i - 1U] == c) {
            return i - 1U;
        }
    }
    return NPH_STR_NPOS;
}

NphStr nph_str_trim_left(NphStr s)
{
    u64 i = 0U;
    while (i < s.len && nph_ascii_is_space(s.data[i])) {
        ++i;
    }
    return nph_str_slice(s, i, s.len);
}

NphStr nph_str_trim_right(NphStr s)
{
    u64 end = s.len;
    while (end > 0U && nph_ascii_is_space(s.data[end - 1U])) {
        --end;
    }
    return nph_str_slice(s, 0U, end);
}

NphStr nph_str_trim(NphStr s)
{
    return nph_str_trim_right(nph_str_trim_left(s));
}

bool nph_str_split_first(NphStr s, char sep, NphStr *out_head, NphStr *out_tail)
{
    if (s.len == 0U) {
        *out_head = nph_str_empty();
        *out_tail = nph_str_empty();
        return false;
    }
    const u64 at = nph_str_find_char(s, sep);
    if (at == NPH_STR_NPOS) {
        *out_head = s;
        *out_tail = nph_str(s.data + s.len, 0U);
    } else {
        *out_head = nph_str_slice(s, 0U, at);
        *out_tail = nph_str_slice(s, at + 1U, s.len);
    }
    return true;
}

u64 nph_str_hash(NphStr s)
{
    return nph_hash64(s.data, s.len, NPH_HASH_DEFAULT_SEED);
}

bool nph_str_parse_u64(NphStr s, u64 *out)
{
    if (s.len == 0U) {
        return false;
    }
    u64 v = 0U;
    for (u64 i = 0; i < s.len; ++i) {
        const char c = s.data[i];
        if (!nph_ascii_is_digit(c)) {
            return false;
        }
        const u64 d = (u64)(c - '0');
        if (v > (NPH_U64_MAX - d) / 10U) {
            return false;
        }
        v = v * 10U + d;
    }
    *out = v;
    return true;
}

bool nph_str_parse_i64(NphStr s, i64 *out)
{
    bool negative = false;
    if (s.len > 0U && (s.data[0] == '-' || s.data[0] == '+')) {
        negative = s.data[0] == '-';
        s = nph_str_slice(s, 1U, s.len);
    }
    u64 magnitude;
    if (!nph_str_parse_u64(s, &magnitude)) {
        return false;
    }
    if (negative) {
        if (magnitude > (u64)NPH_I64_MAX + 1U) {
            return false;
        }
        *out = magnitude == (u64)NPH_I64_MAX + 1U ? NPH_I64_MIN : -(i64)magnitude;
    } else {
        if (magnitude > (u64)NPH_I64_MAX) {
            return false;
        }
        *out = (i64)magnitude;
    }
    return true;
}

bool nph_str_parse_f64(NphStr s, f64 *out)
{
    char buffer[64];
    if (s.len == 0U || s.len >= sizeof buffer || nph_ascii_is_space(s.data[0])) {
        return false;
    }
    memcpy(buffer, s.data, (size_t)s.len);
    buffer[s.len] = '\0';
    char *end = NULL;
    const f64 v = strtod(buffer, &end);
    if (end != buffer + s.len) {
        return false;
    }
    *out = v;
    return true;
}

bool nph_str_parse_bool(NphStr s, bool *out)
{
    static const char *const truthy[] = {"true", "yes", "on", "1"};
    static const char *const falsy[] = {"false", "no", "off", "0"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(truthy); ++i) {
        if (nph_str_equals_ignore_case(s, nph_str_from_cstr(truthy[i]))) {
            *out = true;
            return true;
        }
    }
    for (u32 i = 0; i < NPH_ARRAY_COUNT(falsy); ++i) {
        if (nph_str_equals_ignore_case(s, nph_str_from_cstr(falsy[i]))) {
            *out = false;
            return true;
        }
    }
    return false;
}

NphResult nph_str_dup(NphAllocator *alloc, NphStr s, char **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    char *copy = (char *)NPH_ALLOC(alloc, s.len + 1U, 1U);
    if (copy == NULL) {
        *out = NULL;
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (s.len > 0U) {
        memcpy(copy, s.data, (size_t)s.len);
    }
    copy[s.len] = '\0';
    *out = copy;
    return NPH_RESULT_OK;
}

void nph_str_free(NphAllocator *alloc, char *copy, u64 len)
{
    NPH_FREE(alloc, copy, len + 1U, 1U);
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

bool nph_utf8_decode(NphStr s, u64 *pos, u32 *out_codepoint)
{
    const u64 i = *pos;
    if (i >= s.len) {
        return false;
    }
    const u8 *p = (const u8 *)s.data + i;
    const u64 remaining = s.len - i;
    const u8 b0 = p[0];
    u32 cp;
    u64 need;
    u32 min;
    if (b0 < 0x80U) {
        *out_codepoint = b0;
        *pos = i + 1U;
        return true;
    } else if ((b0 & 0xE0U) == 0xC0U) {
        cp = b0 & 0x1FU;
        need = 1U;
        min = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        cp = b0 & 0x0FU;
        need = 2U;
        min = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        cp = b0 & 0x07U;
        need = 3U;
        min = 0x10000U;
    } else {
        *out_codepoint = NPH_UTF8_REPLACEMENT;
        *pos = i + 1U;
        return true;
    }
    if (remaining < need + 1U) {
        *out_codepoint = NPH_UTF8_REPLACEMENT;
        *pos = i + 1U;
        return true;
    }
    for (u64 k = 1; k <= need; ++k) {
        if ((p[k] & 0xC0U) != 0x80U) {
            *out_codepoint = NPH_UTF8_REPLACEMENT;
            *pos = i + 1U;
            return true;
        }
        cp = (cp << 6U) | (p[k] & 0x3FU);
    }
    // Overlong encodings, surrogates, and out-of-range values are invalid.
    if (cp < min || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
        *out_codepoint = NPH_UTF8_REPLACEMENT;
        *pos = i + 1U;
        return true;
    }
    *out_codepoint = cp;
    *pos = i + need + 1U;
    return true;
}

u32 nph_utf8_encode(u32 cp, char *out)
{
    u8 *o = (u8 *)out;
    if (cp < 0x80U) {
        o[0] = (u8)cp;
        return 1U;
    }
    if (cp < 0x800U) {
        o[0] = (u8)(0xC0U | (cp >> 6U));
        o[1] = (u8)(0x80U | (cp & 0x3FU));
        return 2U;
    }
    if (cp >= 0xD800U && cp <= 0xDFFFU) {
        return 0U;
    }
    if (cp < 0x10000U) {
        o[0] = (u8)(0xE0U | (cp >> 12U));
        o[1] = (u8)(0x80U | ((cp >> 6U) & 0x3FU));
        o[2] = (u8)(0x80U | (cp & 0x3FU));
        return 3U;
    }
    if (cp <= 0x10FFFFU) {
        o[0] = (u8)(0xF0U | (cp >> 18U));
        o[1] = (u8)(0x80U | ((cp >> 12U) & 0x3FU));
        o[2] = (u8)(0x80U | ((cp >> 6U) & 0x3FU));
        o[3] = (u8)(0x80U | (cp & 0x3FU));
        return 4U;
    }
    return 0U;
}

bool nph_utf8_is_valid(NphStr s)
{
    u64 pos = 0U;
    while (pos < s.len) {
        const u64 before = pos;
        u32 cp;
        (void)nph_utf8_decode(s, &pos, &cp);
        // The decoder rejects a sequence by yielding U+FFFD and advancing
        // one byte; a genuine U+FFFD in the input is three bytes long, and a
        // valid single byte is ASCII, never U+FFFD.
        if (cp == NPH_UTF8_REPLACEMENT && pos - before == 1U) {
            return false;
        }
    }
    return true;
}

u64 nph_utf8_count(NphStr s)
{
    u64 pos = 0U;
    u64 count = 0U;
    u32 cp;
    while (nph_utf8_decode(s, &pos, &cp)) {
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

#define SB_MIN_CAP 32U

void nph_sb_init(NphStrBuilder *sb, NphAllocator *alloc)
{
    NPH_ASSERT(sb != NULL && alloc != NULL);
    sb->data = NULL;
    sb->len = 0U;
    sb->cap = 0U;
    sb->alloc = alloc;
}

void nph_sb_free(NphStrBuilder *sb)
{
    if (sb == NULL || sb->alloc == NULL) {
        return;
    }
    if (sb->data != NULL) {
        NPH_FREE(sb->alloc, sb->data, sb->cap, 1U);
    }
    sb->data = NULL;
    sb->len = 0U;
    sb->cap = 0U;
}

NphResult nph_sb_reserve(NphStrBuilder *sb, u64 len)
{
    NPH_ASSERT(sb != NULL && sb->alloc != NULL);
    if (len == NPH_U64_MAX) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const u64 needed = len + 1U;
    if (needed <= sb->cap) {
        return NPH_RESULT_OK;
    }
    u64 new_cap = nph_max_u64(sb->cap * 2U, SB_MIN_CAP);
    if (new_cap < needed) {
        new_cap = needed;
    }
    char *data = (char *)NPH_REALLOC(sb->alloc, sb->data, sb->cap, new_cap, 1U);
    if (data == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (sb->cap == 0U) {
        data[0] = '\0';
    }
    sb->data = data;
    sb->cap = new_cap;
    return NPH_RESULT_OK;
}

NphResult nph_sb_append(NphStrBuilder *sb, NphStr s)
{
    if (s.len == 0U) {
        return nph_sb_reserve(sb, sb->len); // guarantees a terminator exists
    }
    if (s.len > NPH_U64_MAX - sb->len - 1U) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NPH_TRY(nph_sb_reserve(sb, sb->len + s.len));
    memcpy(sb->data + sb->len, s.data, (size_t)s.len);
    sb->len += s.len;
    sb->data[sb->len] = '\0';
    return NPH_RESULT_OK;
}

NphResult nph_sb_append_cstr(NphStrBuilder *sb, const char *s)
{
    return nph_sb_append(sb, nph_str_from_cstr(s));
}

NphResult nph_sb_append_char(NphStrBuilder *sb, char c)
{
    return nph_sb_append(sb, nph_str(&c, 1U));
}

NphResult nph_sb_append_u64(NphStrBuilder *sb, u64 v)
{
    char buffer[24];
    u32 n = 0U;
    do {
        buffer[sizeof buffer - 1U - n] = (char)('0' + (v % 10U));
        v /= 10U;
        ++n;
    } while (v != 0U);
    return nph_sb_append(sb, nph_str(buffer + sizeof buffer - n, n));
}

NphResult nph_sb_append_i64(NphStrBuilder *sb, i64 v)
{
    if (v < 0) {
        NPH_TRY(nph_sb_append_char(sb, '-'));
        // -(i64 min) overflows; go through the unsigned magnitude.
        return nph_sb_append_u64(sb, (u64)0U - (u64)v);
    }
    return nph_sb_append_u64(sb, (u64)v);
}

NphResult nph_sb_append_f64(NphStrBuilder *sb, f64 v)
{
    return nph_sb_appendf(sb, "%.17g", v);
}

NphResult nph_sb_appendf(NphStrBuilder *sb, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(args);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphResult r = nph_sb_reserve(sb, sb->len + (u64)n);
    if (r != NPH_RESULT_OK) {
        va_end(args);
        return r;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1U, fmt, args);
    va_end(args);
    sb->len += (u64)n;
    return NPH_RESULT_OK;
}

NphResult nph_sb_append_utf8(NphStrBuilder *sb, u32 codepoint)
{
    char bytes[4];
    const u32 n = nph_utf8_encode(codepoint, bytes);
    if (n == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return nph_sb_append(sb, nph_str(bytes, n));
}

void nph_sb_clear(NphStrBuilder *sb)
{
    sb->len = 0U;
    if (sb->data != NULL) {
        sb->data[0] = '\0';
    }
}

void nph_sb_truncate(NphStrBuilder *sb, u64 len)
{
    if (len < sb->len) {
        sb->len = len;
        sb->data[len] = '\0';
    }
}
