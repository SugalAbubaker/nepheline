// SPDX-License-Identifier: Apache-2.0
#pragma once

// Path strings: pure functions on UTF-8 paths, no I/O. Both separators are
// accepted on input ('/' and '\\'); joins use '/' which every supported OS
// accepts, so a path built here is portable and stable in cooked data and
// logs. Nothing here allocates: views point into the input, and the
// builders write into a caller buffer.
//
// Thread-safety: NPH_THREADSAFE (pure).

#include <nepheline/core/result.h>
#include <nepheline/core/string.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_INLINE bool nph_path_is_separator(char c)
{
    return c == '/' || c == '\\';
}

// The last component: "a/b/c.txt" -> "c.txt", "a/b/" -> "".
NphStr nph_path_basename(NphStr path);

// Everything before the last separator, without it: "a/b/c.txt" -> "a/b",
// "c.txt" -> "", "/c" -> "/", "C:\\x" -> "C:\\".
NphStr nph_path_dirname(NphStr path);

// The extension including the dot, from the basename: "a/b.tar.gz" ->
// ".gz", "a.b/c" -> "", ".hidden" -> "" (a leading dot is not an extension).
NphStr nph_path_extension(NphStr path);

// The basename without its extension: "a/b.tar.gz" -> "b.tar".
NphStr nph_path_stem(NphStr path);

// True for "/x" and "C:/x" / "C:\\x" / "\\\\server\\share".
bool nph_path_is_absolute(NphStr path);

// Writes "a/b" into `buf` (NUL terminated, at most `cap` bytes): exactly one
// '/' between the parts unless `a` is empty or already ends in a separator,
// or `b` is absolute (then `b` alone). NPH_RESULT_OVERFLOW when it does not
// fit; *out_len (may be NULL) receives the length that would be written.
NphResult nph_path_join(char *buf, u64 cap, NphStr a, NphStr b, u64 *out_len);

// Converts separators to '/' in place.
void nph_path_normalize_separators(char *path);

#ifdef __cplusplus
}
#endif
