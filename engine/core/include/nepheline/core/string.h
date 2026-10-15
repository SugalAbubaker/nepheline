// SPDX-License-Identifier: Apache-2.0
#pragma once

// Strings (CLAUDE.md §8.1 "length-prefixed string views, string builder").
//
// NphStr is a non-owning view: a pointer and a byte length, not necessarily
// NUL-terminated. Engine APIs take and return views; ownership is explicit
// through the builder or nph_str_dup. Text is UTF-8 bytes; the view
// functions operate on bytes except the nph_utf8_* helpers. Case-insensitive
// comparison is ASCII-only by design (locale-free, deterministic).
//
// NphStrBuilder is a growable buffer that always keeps a NUL terminator
// after its contents, so nph_sb_cstr is free.
//
// Thread-safety: views are values; builders belong to one thread.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphStr {
    const char *data;
    u64 len;
} NphStr;

// "not found" for the search functions.
#define NPH_STR_NPOS NPH_U64_MAX

// View of a string literal (or any char array with a trailing NUL).
#define NPH_STR(literal) ((NphStr){(literal), sizeof(literal) - 1U})

// printf helpers: printf("%" NPH_STR_FMT "\n", NPH_STR_ARG(s)).
#define NPH_STR_FMT    ".*s"
#define NPH_STR_ARG(s) (int)(s).len, (s).data

NPH_INLINE NphStr nph_str(const char *data, u64 len)
{
    const NphStr s = {data, len};
    return s;
}

NPH_INLINE NphStr nph_str_empty(void)
{
    const NphStr s = {"", 0U};
    return s;
}

// View of a NUL-terminated string (NULL gives an empty view).
NphStr nph_str_from_cstr(const char *s);

NPH_INLINE bool nph_str_is_empty(NphStr s)
{
    return s.len == 0U;
}

// Bytes [begin, end) of `s`, clamped to its length.
NphStr nph_str_slice(NphStr s, u64 begin, u64 end);
NPH_INLINE NphStr nph_str_prefix(NphStr s, u64 len)
{
    return nph_str_slice(s, 0U, len);
}
NPH_INLINE NphStr nph_str_suffix(NphStr s, u64 len)
{
    return nph_str_slice(s, len > s.len ? 0U : s.len - len, s.len);
}

bool nph_str_equals(NphStr a, NphStr b);
bool nph_str_equals_ignore_case(NphStr a, NphStr b); // ASCII case folding
// Lexicographic byte comparison: <0, 0, >0.
i32 nph_str_compare(NphStr a, NphStr b);

bool nph_str_starts_with(NphStr s, NphStr prefix);
bool nph_str_ends_with(NphStr s, NphStr suffix);
bool nph_str_contains(NphStr s, NphStr needle);

// Byte offset of the first/last occurrence, or NPH_STR_NPOS.
u64 nph_str_find(NphStr s, NphStr needle);
u64 nph_str_find_char(NphStr s, char c);
u64 nph_str_rfind_char(NphStr s, char c);

// ASCII whitespace (space, \t, \n, \r, \f, \v) trimming.
NphStr nph_str_trim(NphStr s);
NphStr nph_str_trim_left(NphStr s);
NphStr nph_str_trim_right(NphStr s);

// Splits at the first `sep`: head is before it, tail after. When `sep` is
// absent, head is the whole string and tail is empty; returns false when
// `s` is empty so a loop ends:
//
//     NphStr rest = line;
//     NphStr field;
//     while (nph_str_split_first(rest, ',', &field, &rest)) { ... }
bool nph_str_split_first(NphStr s, char sep, NphStr *out_head, NphStr *out_tail);

// nph_hash64 of the bytes with the default seed.
u64 nph_str_hash(NphStr s);

// Decimal integer parsing of the whole string (optional sign for i64, no
// whitespace, no prefixes). false on empty, malformed, or overflow.
bool nph_str_parse_i64(NphStr s, i64 *out);
bool nph_str_parse_u64(NphStr s, u64 *out);
// Floating point via strtod on a bounded copy; whole string must be consumed.
bool nph_str_parse_f64(NphStr s, f64 *out);
// true/false, yes/no, on/off, 1/0 (ASCII case-insensitive).
bool nph_str_parse_bool(NphStr s, bool *out);

// Owned NUL-terminated copy of `s` (len + 1 bytes from `alloc`). Free with
// nph_str_free.
NPH_MUST_USE NphResult nph_str_dup(NphAllocator *alloc, NphStr s, char **out);
void nph_str_free(NphAllocator *alloc, char *copy, u64 len);

NPH_INLINE char nph_ascii_to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A')); // NOLINT(bugprone-narrowing-conversions)
    }
    return c;
}
NPH_INLINE char nph_ascii_to_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A')); // NOLINT(bugprone-narrowing-conversions)
    }
    return c;
}
NPH_INLINE bool nph_ascii_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
NPH_INLINE bool nph_ascii_is_digit(char c)
{
    return c >= '0' && c <= '9';
}
NPH_INLINE bool nph_ascii_is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

#define NPH_UTF8_REPLACEMENT 0xFFFDU

// Decodes the code point at byte offset *pos and advances *pos past it.
// Invalid or truncated sequences yield NPH_UTF8_REPLACEMENT and advance by
// one byte. Returns false (without advancing) at the end of the string.
bool nph_utf8_decode(NphStr s, u64 *pos, u32 *out_codepoint);

// Encodes `codepoint` into `out` (at least 4 bytes); returns the length
// (1-4), or 0 for an invalid code point (surrogates, > 0x10FFFF).
u32 nph_utf8_encode(u32 cp, char *out);

// True if every byte sequence is well-formed UTF-8.
bool nph_utf8_is_valid(NphStr s);

// Number of code points (invalid bytes count one each).
u64 nph_utf8_count(NphStr s);

// ---------------------------------------------------------------------------
// String builder
// ---------------------------------------------------------------------------

typedef struct NphStrBuilder {
    char *data; // NUL-terminated at data[len] whenever cap > 0
    u64 len;
    u64 cap; // bytes including the terminator
    NphAllocator *alloc;
} NphStrBuilder;

void nph_sb_init(NphStrBuilder *sb, NphAllocator *alloc);
void nph_sb_free(NphStrBuilder *sb);

// Ensures room for `len` content bytes (plus the terminator).
NPH_MUST_USE NphResult nph_sb_reserve(NphStrBuilder *sb, u64 len);

NPH_MUST_USE NphResult nph_sb_append(NphStrBuilder *sb, NphStr s);
NPH_MUST_USE NphResult nph_sb_append_cstr(NphStrBuilder *sb, const char *s);
NPH_MUST_USE NphResult nph_sb_append_char(NphStrBuilder *sb, char c);
NPH_MUST_USE NphResult nph_sb_append_u64(NphStrBuilder *sb, u64 v);
NPH_MUST_USE NphResult nph_sb_append_i64(NphStrBuilder *sb, i64 v);
// %.17g, round-trippable.
NPH_MUST_USE NphResult nph_sb_append_f64(NphStrBuilder *sb, f64 v);
// The shortest decimal text that reads back (strtod, strtof) as exactly
// `v`: "0.1" rather than "0.10000000000000001", and -0 stays "-0". For text
// that people read and diff (scene files, JSON). Non-finite values are
// written as nan, inf, and -inf. The f32 text also reads back exactly
// through strtod and a conversion to float (how a JSON reader sees it).
NPH_MUST_USE NphResult nph_sb_append_f64_shortest(NphStrBuilder *sb, f64 v);
NPH_MUST_USE NphResult nph_sb_append_f32_shortest(NphStrBuilder *sb, f32 v);
NPH_PRINTF_LIKE(2, 3)
NPH_MUST_USE NphResult nph_sb_appendf(NphStrBuilder *sb, const char *fmt, ...);
NPH_MUST_USE NphResult nph_sb_append_utf8(NphStrBuilder *sb, u32 codepoint);

// Drops the contents, keeps the storage.
void nph_sb_clear(NphStrBuilder *sb);

// Truncates to `len` bytes (no-op when longer than the contents).
void nph_sb_truncate(NphStrBuilder *sb, u64 len);

NPH_INLINE NphStr nph_sb_str(const NphStrBuilder *sb)
{
    const NphStr s = {sb->data != NULL ? sb->data : "", sb->len};
    return s;
}

// NUL-terminated contents; valid until the next append. "" when empty.
NPH_INLINE const char *nph_sb_cstr(const NphStrBuilder *sb)
{
    return sb->data != NULL ? sb->data : "";
}

#ifdef __cplusplus
}
#endif
