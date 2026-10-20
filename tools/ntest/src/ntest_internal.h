// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal helpers of the ntest runtime, exposed for ntest's own unit tests.
// Not part of the public API.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Glob match with '*' (any run) and '?' (any single char). Anchored at both ends.
bool ntest_glob_match(const char *pattern, const char *str);

// Applies a --filter value to "suite.name". Comma-separated patterns; a
// leading '-' excludes; a pattern without '.' also matches the bare suite.
// NULL or "" accepts everything.
bool ntest_filter_accepts(const char *filter, const char *suite, const char *name);

// Parses a --shard value "I/N" (decimal, 0 <= I < N <= 1000000) into
// `index` and `count`; false, leaving both alone, for anything else.
bool ntest_parse_shard(const char *s, uint32_t *index, uint32_t *count);

// Whether shard `index` of `count` runs the selected test at `position` in
// the default order: every count-th from the index-th, so the shards 0 to
// count - 1 together run each selected test exactly once.
bool ntest_shard_accepts(size_t position, uint32_t index, uint32_t count);

// Number of representable values between a and b. Returns UINT32_MAX /
// UINT64_MAX if either operand is NaN. The two zeros are 0 apart.
uint32_t ntest_ulp_distance_f32(float a, float b);
uint64_t ntest_ulp_distance_f64(double a, double b);

// Writes the XML-escaped form of `in` to `out` (at most `cap` bytes, always
// NUL-terminated when cap > 0) and returns the full escaped length, like
// snprintf. Control characters other than tab, LF, and CR become '?'.
size_t ntest_xml_escape(const char *in, char *out, size_t cap);

// qsort comparator over `const NtestCase *` elements: by file, line, suite, name.
int ntest_compare_cases(const void *a, const void *b);

#ifdef __cplusplus
}
#endif
