// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/platform/path.h>

#include <string.h>

static u64 last_separator(NphStr path)
{
    for (u64 i = path.len; i > 0U; --i) {
        if (nph_path_is_separator(path.data[i - 1U])) {
            return i - 1U;
        }
    }
    return NPH_STR_NPOS;
}

NphStr nph_path_basename(NphStr path)
{
    const u64 sep = last_separator(path);
    return sep == NPH_STR_NPOS ? path : nph_str_slice(path, sep + 1U, path.len);
}

NphStr nph_path_dirname(NphStr path)
{
    const u64 sep = last_separator(path);
    if (sep == NPH_STR_NPOS) {
        return nph_str_empty();
    }
    // Keep a root: "/" and "C:\".
    if (sep == 0U || (sep == 2U && path.data[1] == ':')) {
        return nph_str_slice(path, 0U, sep + 1U);
    }
    return nph_str_slice(path, 0U, sep);
}

NphStr nph_path_extension(NphStr path)
{
    const NphStr base = nph_path_basename(path);
    const u64 dot = nph_str_rfind_char(base, '.');
    if (dot == NPH_STR_NPOS || dot == 0U) {
        return nph_str_empty();
    }
    return nph_str_slice(base, dot, base.len);
}

NphStr nph_path_stem(NphStr path)
{
    const NphStr base = nph_path_basename(path);
    const u64 dot = nph_str_rfind_char(base, '.');
    if (dot == NPH_STR_NPOS || dot == 0U) {
        return base;
    }
    return nph_str_slice(base, 0U, dot);
}

bool nph_path_is_absolute(NphStr path)
{
    if (path.len == 0U) {
        return false;
    }
    if (nph_path_is_separator(path.data[0])) {
        return true; // "/x" or "\\server\share"
    }
    const char c = path.data[0];
    const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return letter && path.len >= 3U && path.data[1] == ':' && nph_path_is_separator(path.data[2]);
}

NphResult nph_path_join(char *buf, u64 cap, NphStr a, NphStr b, u64 *out_len)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    const bool b_alone = a.len == 0U || nph_path_is_absolute(b);
    const bool need_sep = !b_alone && !nph_path_is_separator(a.data[a.len - 1U]);
    const u64 total = b_alone ? b.len : a.len + (need_sep ? 1U : 0U) + b.len;
    if (out_len != NULL) {
        *out_len = total;
    }
    if (total + 1U > cap) {
        buf[0] = '\0';
        return NPH_RESULT_OVERFLOW;
    }
    u64 n = 0U;
    if (!b_alone) {
        memcpy(buf, a.data, (size_t)a.len);
        n = a.len;
        if (need_sep) {
            buf[n++] = '/';
        }
    }
    if (b.len != 0U) {
        memcpy(buf + n, b.data, (size_t)b.len);
        n += b.len;
    }
    buf[n] = '\0';
    return NPH_RESULT_OK;
}

void nph_path_normalize_separators(char *path)
{
    NPH_ASSERT(path != NULL);
    for (char *p = path; *p != '\0'; ++p) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}
