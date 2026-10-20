// SPDX-License-Identifier: Apache-2.0
// ntest runtime: registry, checks, runner, console and JUnit reporters.
// Design notes live in docs/ntest.md.

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <ntest/ntest.h>

#include "ntest_internal.h"

#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Growable string buffer
// ---------------------------------------------------------------------------

typedef struct NtestBuf {
    char *data;
    size_t len;
    size_t cap;
} NtestBuf;

static void buf_free(NtestBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_reserve(NtestBuf *b, size_t extra)
{
    const size_t need = b->len + extra + 1U;
    if (need <= b->cap) {
        return;
    }
    size_t cap = b->cap ? b->cap : 256U;
    while (cap < need) {
        cap *= 2U;
    }
    char *data = (char *)realloc(b->data, cap);
    if (data == NULL) {
        fputs("ntest: out of memory\n", stderr);
        abort();
    }
    b->data = data;
    b->cap = cap;
}

static void buf_append_n(NtestBuf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_append(NtestBuf *b, const char *s)
{
    buf_append_n(b, s, strlen(s));
}

NTEST_PRINTF(2, 0) static void buf_vappendf(NtestBuf *b, const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        return;
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1U, fmt, args);
    b->len += (size_t)n;
}

NTEST_PRINTF(2, 3) static void buf_appendf(NtestBuf *b, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    buf_vappendf(b, fmt, args);
    va_end(args);
}

static const char *buf_cstr(const NtestBuf *b)
{
    return b->data ? b->data : "";
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static NtestCase *g_registry = NULL;
static size_t g_registry_count = 0;

void ntest_register(NtestCase *tc)
{
    tc->next = g_registry;
    g_registry = tc;
    ++g_registry_count;
}

int ntest_compare_cases(const void *a, const void *b)
{
    const NtestCase *x = *(const NtestCase *const *)a;
    const NtestCase *y = *(const NtestCase *const *)b;
    int c = strcmp(x->file, y->file);
    if (c != 0) {
        return c;
    }
    if (x->line != y->line) {
        return x->line < y->line ? -1 : 1;
    }
    c = strcmp(x->suite, y->suite);
    if (c != 0) {
        return c;
    }
    return strcmp(x->name, y->name);
}

// ---------------------------------------------------------------------------
// Runner state
// ---------------------------------------------------------------------------

typedef enum NtestStatus {
    NTEST_STATUS_PASSED,
    NTEST_STATUS_FAILED,
    NTEST_STATUS_SKIPPED,
} NtestStatus;

typedef struct NtestResult {
    const NtestCase *tc;
    NtestStatus status;
    bool skipped;
    int failure_count;
    double seconds;
    NtestBuf failures;
    NtestBuf log;
    NtestBuf skip_reason;
} NtestResult;

// The test currently executing. Tests run strictly sequentially on one
// thread, so a single global keeps the test-side macros free of context
// parameters. Fatal checks longjmp to `escape`, which the runner arms around
// setup+body and again around teardown.
static struct {
    jmp_buf escape;
    const NtestCase *tc;
    NtestResult *result;
    void *fixture;
    bool use_color;
} g_run;

void *ntest_fixture(void)
{
    return g_run.fixture;
}

static NtestResult *current_result(const char *file, int line)
{
    if (g_run.result == NULL) {
        fprintf(stderr, "ntest: check at %s:%d is outside of any running test\n", file, line);
        abort();
    }
    return g_run.result;
}

static void begin_failure(NtestResult *r, const char *file, int line)
{
    ++r->failure_count;
    buf_appendf(&r->failures, "  %s:%d: ", file, line);
}

static void end_check(bool fatal)
{
    if (fatal) {
        longjmp(g_run.escape, 1);
    }
}

// ---------------------------------------------------------------------------
// Value formatting
// ---------------------------------------------------------------------------

static void append_int(NtestBuf *b, intmax_t v)
{
    buf_appendf(b, "%" PRIdMAX, v);
    if (v < 0 || v >= 16) {
        buf_appendf(b, " (0x%" PRIxMAX ")", (uintmax_t)v);
    }
}

static void append_uint(NtestBuf *b, uintmax_t v)
{
    buf_appendf(b, "%" PRIuMAX, v);
    if (v >= 16U) {
        buf_appendf(b, " (0x%" PRIxMAX ")", v);
    }
}

// Appends s as a C-style quoted literal, escaping control characters and
// truncating very long strings so a failure stays readable.
static void append_quoted(NtestBuf *b, const char *s)
{
    if (s == NULL) {
        buf_append(b, "NULL");
        return;
    }
    const size_t max_len = 256U;
    buf_append(b, "\"");
    size_t i = 0;
    for (; s[i] != '\0' && i < max_len; ++i) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            buf_append(b, "\\\"");
            break;
        case '\\':
            buf_append(b, "\\\\");
            break;
        case '\n':
            buf_append(b, "\\n");
            break;
        case '\r':
            buf_append(b, "\\r");
            break;
        case '\t':
            buf_append(b, "\\t");
            break;
        default:
            if (c < 0x20U || c == 0x7FU) {
                buf_appendf(b, "\\x%02x", (unsigned int)c);
            } else {
                buf_append_n(b, &s[i], 1U);
            }
            break;
        }
    }
    buf_append(b, "\"");
    if (s[i] != '\0') {
        buf_append(b, "...");
    }
}

static void append_char_desc(NtestBuf *b, char c)
{
    const unsigned char u = (unsigned char)c;
    if (u == 0U) {
        buf_append(b, "end of string");
    } else if (u >= 0x20U && u < 0x7FU) {
        buf_appendf(b, "'%c' (0x%02x)", c, (unsigned int)u);
    } else {
        buf_appendf(b, "0x%02x", (unsigned int)u);
    }
}

static void append_hex_row(NtestBuf *b, const char *label, const unsigned char *p, size_t start,
                           size_t end)
{
    buf_appendf(b, "      %s[%zu..%zu]:", label, start, end - 1U);
    for (size_t i = start; i < end; ++i) {
        buf_appendf(b, " %02x", (unsigned int)p[i]);
    }
    buf_append(b, "\n");
}

static const char *op_symbol(NtestOp op)
{
    switch (op) {
    case NTEST_OP_EQ:
        return "==";
    case NTEST_OP_NE:
        return "!=";
    case NTEST_OP_LT:
        return "<";
    case NTEST_OP_LE:
        return "<=";
    case NTEST_OP_GT:
        return ">";
    case NTEST_OP_GE:
        return ">=";
    }
    return "?";
}

static bool compare_int(intmax_t a, intmax_t b, NtestOp op)
{
    switch (op) {
    case NTEST_OP_EQ:
        return a == b;
    case NTEST_OP_NE:
        return a != b;
    case NTEST_OP_LT:
        return a < b;
    case NTEST_OP_LE:
        return a <= b;
    case NTEST_OP_GT:
        return a > b;
    case NTEST_OP_GE:
        return a >= b;
    }
    return false;
}

static bool compare_uint(uintmax_t a, uintmax_t b, NtestOp op)
{
    switch (op) {
    case NTEST_OP_EQ:
        return a == b;
    case NTEST_OP_NE:
        return a != b;
    case NTEST_OP_LT:
        return a < b;
    case NTEST_OP_LE:
        return a <= b;
    case NTEST_OP_GT:
        return a > b;
    case NTEST_OP_GE:
        return a >= b;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

bool ntest_check_bool(bool fatal, const char *file, int line, const char *expr, bool value,
                      bool expected)
{
    if (value == expected) {
        return true;
    }
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures, "expected %s to be %s\n      %s = %s\n", expr,
                expected ? "true" : "false", expr, value ? "true" : "false");
    end_check(fatal);
    return false;
}

bool ntest_check_int(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, intmax_t lhs, intmax_t rhs, NtestOp op)
{
    if (compare_int(lhs, rhs, op)) {
        return true;
    }
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures, "expected %s %s %s\n      lhs: %s = ", lhs_expr, op_symbol(op),
                rhs_expr, lhs_expr);
    append_int(&r->failures, lhs);
    buf_appendf(&r->failures, "\n      rhs: %s = ", rhs_expr);
    append_int(&r->failures, rhs);
    buf_append(&r->failures, "\n");
    end_check(fatal);
    return false;
}

bool ntest_check_uint(bool fatal, const char *file, int line, const char *lhs_expr,
                      const char *rhs_expr, uintmax_t lhs, uintmax_t rhs, NtestOp op)
{
    if (compare_uint(lhs, rhs, op)) {
        return true;
    }
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures, "expected %s %s %s\n      lhs: %s = ", lhs_expr, op_symbol(op),
                rhs_expr, lhs_expr);
    append_uint(&r->failures, lhs);
    buf_appendf(&r->failures, "\n      rhs: %s = ", rhs_expr);
    append_uint(&r->failures, rhs);
    buf_append(&r->failures, "\n");
    end_check(fatal);
    return false;
}

bool ntest_check_ptr(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, const void *lhs, const void *rhs, NtestOp op)
{
    const bool equal = lhs == rhs;
    if (op == NTEST_OP_EQ ? equal : !equal) {
        return true;
    }
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures, "expected %s %s %s\n      lhs: %s = %p\n      rhs: %s = %p\n",
                lhs_expr, op_symbol(op), rhs_expr, lhs_expr, lhs, rhs_expr, rhs);
    end_check(fatal);
    return false;
}

bool ntest_check_str(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, const char *lhs, const char *rhs, NtestOp op)
{
    const bool equal = (lhs == NULL || rhs == NULL) ? (lhs == rhs) : (strcmp(lhs, rhs) == 0);
    if (op == NTEST_OP_EQ ? equal : !equal) {
        return true;
    }
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures,
                "expected strings %s\n      lhs: %s = ", op == NTEST_OP_EQ ? "equal" : "different",
                lhs_expr);
    append_quoted(&r->failures, lhs);
    buf_appendf(&r->failures, "\n      rhs: %s = ", rhs_expr);
    append_quoted(&r->failures, rhs);
    buf_append(&r->failures, "\n");
    if (op == NTEST_OP_EQ && lhs != NULL && rhs != NULL) {
        size_t i = 0;
        while (lhs[i] != '\0' && lhs[i] == rhs[i]) {
            ++i;
        }
        buf_appendf(&r->failures, "      differ at index %zu: ", i);
        append_char_desc(&r->failures, lhs[i]);
        buf_append(&r->failures, " vs ");
        append_char_desc(&r->failures, rhs[i]);
        buf_append(&r->failures, "\n");
    }
    end_check(fatal);
    return false;
}

bool ntest_check_mem(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, const void *lhs, const void *rhs, size_t size)
{
    if (size == 0U) {
        return true;
    }
    if (lhs != NULL && rhs != NULL && memcmp(lhs, rhs, size) == 0) {
        return true;
    }
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures, "expected %s and %s to hold the same %zu bytes\n", lhs_expr, rhs_expr,
                size);
    if (lhs == NULL || rhs == NULL) {
        buf_appendf(&r->failures, "      %s is NULL\n", lhs == NULL ? lhs_expr : rhs_expr);
        end_check(fatal);
        return false;
    }
    const unsigned char *a = (const unsigned char *)lhs;
    const unsigned char *b = (const unsigned char *)rhs;
    size_t i = 0;
    while (a[i] == b[i]) {
        ++i; // memcmp != 0 guarantees a difference before `size`
    }
    buf_appendf(&r->failures, "      first difference at byte %zu: 0x%02x vs 0x%02x\n", i,
                (unsigned int)a[i], (unsigned int)b[i]);
    const size_t start = i >= 8U ? i - 8U : 0U;
    const size_t end = (start + 16U < size) ? start + 16U : size;
    append_hex_row(&r->failures, "lhs", a, start, end);
    append_hex_row(&r->failures, "rhs", b, start, end);
    end_check(fatal);
    return false;
}

bool ntest_check_near_f64(bool fatal, const char *file, int line, const char *lhs_expr,
                          const char *rhs_expr, double lhs, double rhs, double tolerance)
{
    const double diff = fabs(lhs - rhs);
    if (lhs == rhs || diff <= tolerance) {
        return true;
    }
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures,
                "expected %s ~= %s within %.17g\n      lhs: %s = %.17g\n      rhs: %s = %.17g\n"
                "      |lhs - rhs| = %.17g\n",
                lhs_expr, rhs_expr, tolerance, lhs_expr, lhs, rhs_expr, rhs, diff);
    end_check(fatal);
    return false;
}

// Maps IEEE-754 bits to an unsigned integer that increases monotonically with
// the represented value, so the ULP distance is a plain subtraction. Both
// zeros map to the same point; infinities sit just past the largest finite
// values.
static uint32_t ordered_bits_u32(uint32_t bits)
{
    const uint32_t sign = 0x80000000U;
    return (bits & sign) ? sign - (bits & ~sign) : sign + bits;
}

static uint64_t ordered_bits_u64(uint64_t bits)
{
    const uint64_t sign = 0x8000000000000000ULL;
    return (bits & sign) ? sign - (bits & ~sign) : sign + bits;
}

uint32_t ntest_ulp_distance_f32(float a, float b)
{
    if (isnan(a) || isnan(b)) {
        return UINT32_MAX;
    }
    uint32_t ia;
    uint32_t ib;
    memcpy(&ia, &a, sizeof ia);
    memcpy(&ib, &b, sizeof ib);
    const uint32_t oa = ordered_bits_u32(ia);
    const uint32_t ob = ordered_bits_u32(ib);
    return oa > ob ? oa - ob : ob - oa;
}

uint64_t ntest_ulp_distance_f64(double a, double b)
{
    if (isnan(a) || isnan(b)) {
        return UINT64_MAX;
    }
    uint64_t ia;
    uint64_t ib;
    memcpy(&ia, &a, sizeof ia);
    memcpy(&ib, &b, sizeof ib);
    const uint64_t oa = ordered_bits_u64(ia);
    const uint64_t ob = ordered_bits_u64(ib);
    return oa > ob ? oa - ob : ob - oa;
}

bool ntest_check_ulp_f32(bool fatal, const char *file, int line, const char *lhs_expr,
                         const char *rhs_expr, float lhs, float rhs, uint32_t max_ulps)
{
    const uint32_t distance = ntest_ulp_distance_f32(lhs, rhs);
    if (distance <= max_ulps) {
        return true;
    }
    uint32_t lhs_bits;
    uint32_t rhs_bits;
    memcpy(&lhs_bits, &lhs, sizeof lhs_bits);
    memcpy(&rhs_bits, &rhs, sizeof rhs_bits);
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures,
                "expected %s within %" PRIu32 " ULPs of %s\n      lhs: %s = %.9g (0x%08" PRIx32
                ")\n      rhs: %s = %.9g (0x%08" PRIx32 ")\n      distance: ",
                lhs_expr, max_ulps, rhs_expr, lhs_expr, (double)lhs, lhs_bits, rhs_expr,
                (double)rhs, rhs_bits);
    if (distance == UINT32_MAX) {
        buf_append(&r->failures, "NaN\n");
    } else {
        buf_appendf(&r->failures, "%" PRIu32 " ULPs\n", distance);
    }
    end_check(fatal);
    return false;
}

bool ntest_check_ulp_f64(bool fatal, const char *file, int line, const char *lhs_expr,
                         const char *rhs_expr, double lhs, double rhs, uint64_t max_ulps)
{
    const uint64_t distance = ntest_ulp_distance_f64(lhs, rhs);
    if (distance <= max_ulps) {
        return true;
    }
    uint64_t lhs_bits;
    uint64_t rhs_bits;
    memcpy(&lhs_bits, &lhs, sizeof lhs_bits);
    memcpy(&rhs_bits, &rhs, sizeof rhs_bits);
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    buf_appendf(&r->failures,
                "expected %s within %" PRIu64 " ULPs of %s\n      lhs: %s = %.17g (0x%016" PRIx64
                ")\n      rhs: %s = %.17g (0x%016" PRIx64 ")\n      distance: ",
                lhs_expr, max_ulps, rhs_expr, lhs_expr, lhs, lhs_bits, rhs_expr, rhs, rhs_bits);
    if (distance == UINT64_MAX) {
        buf_append(&r->failures, "NaN\n");
    } else {
        buf_appendf(&r->failures, "%" PRIu64 " ULPs\n", distance);
    }
    end_check(fatal);
    return false;
}

void ntest_fail(bool fatal, const char *file, int line, const char *fmt, ...)
{
    NtestResult *r = current_result(file, line);
    begin_failure(r, file, line);
    va_list args;
    va_start(args, fmt);
    buf_vappendf(&r->failures, fmt, args);
    va_end(args);
    buf_append(&r->failures, "\n");
    end_check(fatal);
}

void ntest_skip(const char *file, int line, const char *fmt, ...)
{
    NtestResult *r = current_result(file, line);
    r->skipped = true;
    va_list args;
    va_start(args, fmt);
    buf_vappendf(&r->skip_reason, fmt, args);
    va_end(args);
    longjmp(g_run.escape, 1);
}

void ntest_log(const char *fmt, ...)
{
    if (g_run.result == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    buf_vappendf(&g_run.result->log, fmt, args);
    va_end(args);
    buf_append(&g_run.result->log, "\n");
}

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

static double now_seconds(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static bool stdout_is_terminal(void)
{
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

static void enable_ansi_colors(void)
{
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

static void make_directory(const char *path)
{
#if defined(_WIN32)
    (void)_mkdir(path);
#else
    (void)mkdir(path, 0755);
#endif
}

// Creates every missing directory on the way to `path` (a file path).
static void make_parent_directories(const char *path)
{
    char tmp[1024];
    const size_t n = strlen(path);
    if (n >= sizeof tmp) {
        return;
    }
    memcpy(tmp, path, n + 1U);
    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            const char saved = *p;
            *p = '\0';
            make_directory(tmp);
            *p = saved;
        }
    }
}

static const char *path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

// splitmix64 (Steele, Lea & Flood, "Fast Splittable Pseudorandom Number
// Generators", OOPSLA 2014): tiny, good enough to shuffle a test list.
static uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

bool ntest_glob_match(const char *pattern, const char *str)
{
    const char *star_pattern = NULL;
    const char *star_str = NULL;
    while (*str != '\0') {
        if (*pattern == '*') {
            star_pattern = pattern++;
            star_str = str;
        } else if (*pattern == '?' || *pattern == *str) {
            ++pattern;
            ++str;
        } else if (star_pattern != NULL) {
            pattern = star_pattern + 1;
            str = ++star_str;
        } else {
            return false;
        }
    }
    while (*pattern == '*') {
        ++pattern;
    }
    return *pattern == '\0';
}

bool ntest_filter_accepts(const char *filter, const char *suite, const char *name)
{
    if (filter == NULL || *filter == '\0') {
        return true;
    }
    char full[512];
    snprintf(full, sizeof full, "%s.%s", suite, name);

    bool has_positive = false;
    bool matched_positive = false;
    const char *p = filter;
    for (;;) {
        const char *end = strchr(p, ',');
        size_t n = end != NULL ? (size_t)(end - p) : strlen(p);
        if (n > 0U) {
            char pat[256];
            if (n >= sizeof pat) {
                n = sizeof pat - 1U;
            }
            memcpy(pat, p, n);
            pat[n] = '\0';
            const bool negative = pat[0] == '-';
            const char *glob = negative ? pat + 1 : pat;
            const bool matches = ntest_glob_match(glob, full) ||
                                 (strchr(glob, '.') == NULL && ntest_glob_match(glob, suite));
            if (negative) {
                if (matches) {
                    return false;
                }
            } else {
                has_positive = true;
                if (matches) {
                    matched_positive = true;
                }
            }
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return !has_positive || matched_positive;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

// These two keep no locals, so nothing can be clobbered across the longjmp.
static void run_guarded_body(void)
{
    if (setjmp(g_run.escape) == 0) {
        if (g_run.tc->setup != NULL) {
            g_run.tc->setup(g_run.fixture);
        }
        g_run.tc->fn();
    }
}

static void run_guarded_teardown(void)
{
    if (setjmp(g_run.escape) == 0) {
        g_run.tc->teardown(g_run.fixture);
    }
}

static void run_case(const NtestCase *tc, NtestResult *result)
{
    result->tc = tc;
    result->status = NTEST_STATUS_PASSED;
    result->skipped = false;
    result->failure_count = 0;
    result->seconds = 0.0;

    void *fixture = NULL;
    if (tc->fixture_size > 0U) {
        fixture = calloc(1U, tc->fixture_size);
        if (fixture == NULL) {
            fputs("ntest: out of memory\n", stderr);
            abort();
        }
    }

    g_run.tc = tc;
    g_run.result = result;
    g_run.fixture = fixture;

    const double start = now_seconds();
    run_guarded_body();
    if (tc->teardown != NULL) {
        run_guarded_teardown();
    }
    result->seconds = now_seconds() - start;

    g_run.tc = NULL;
    g_run.result = NULL;
    g_run.fixture = NULL;
    free(fixture);

    if (result->failure_count > 0) {
        result->status = NTEST_STATUS_FAILED;
    } else if (result->skipped) {
        result->status = NTEST_STATUS_SKIPPED;
    } else {
        result->status = NTEST_STATUS_PASSED;
    }
}

// ---------------------------------------------------------------------------
// Console reporter
// ---------------------------------------------------------------------------

#define ANSI_RED    "\x1b[31m"
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_RESET  "\x1b[0m"

static const char *paint(const char *code)
{
    return g_run.use_color ? code : "";
}

static void print_lines(const char *text, const char *prefix)
{
    const char *p = text;
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        const size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);
        printf("%s%.*s\n", prefix, (int)n, p);
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
}

static void print_result(const NtestResult *r, bool quiet, bool verbose)
{
    const NtestCase *tc = r->tc;
    switch (r->status) {
    case NTEST_STATUS_PASSED:
        if (!quiet) {
            printf("%s[       OK ]%s %s.%s (%.2f ms)\n", paint(ANSI_GREEN), paint(ANSI_RESET),
                   tc->suite, tc->name, r->seconds * 1000.0);
            if (verbose && r->log.len > 0U) {
                print_lines(buf_cstr(&r->log), "      | ");
            }
        }
        break;
    case NTEST_STATUS_SKIPPED:
        if (!quiet) {
            printf("%s[  SKIPPED ]%s %s.%s: %s\n", paint(ANSI_YELLOW), paint(ANSI_RESET), tc->suite,
                   tc->name, buf_cstr(&r->skip_reason));
        }
        break;
    case NTEST_STATUS_FAILED:
        printf("%s[  FAILED  ]%s %s.%s (%.2f ms)\n", paint(ANSI_RED), paint(ANSI_RESET), tc->suite,
               tc->name, r->seconds * 1000.0);
        fputs(buf_cstr(&r->failures), stdout);
        if (r->log.len > 0U) {
            print_lines(buf_cstr(&r->log), "      | ");
        }
        break;
    }
}

static const char *plural(size_t n)
{
    return n == 1U ? "" : "s";
}

static size_t count_distinct_suites(const NtestCase *const *cases, size_t n)
{
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) {
            seen = strcmp(cases[j]->suite, cases[i]->suite) == 0;
        }
        if (!seen) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// JUnit reporter
// ---------------------------------------------------------------------------

// Appends n bytes of s to out (bounded by cap, keeping room for the
// terminator) and advances *len by n regardless, so a NULL out measures.
static void xml_emit(char *out, size_t cap, size_t *len, const char *s, size_t n)
{
    if (out != NULL && cap > 0U) {
        for (size_t k = 0; k < n; ++k) {
            if (*len + k + 1U < cap) {
                out[*len + k] = s[k];
            }
        }
    }
    *len += n;
}

size_t ntest_xml_escape(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    for (const char *p = in; *p != '\0'; ++p) {
        const unsigned char c = (unsigned char)*p;
        const char *entity = NULL;
        switch (c) {
        case '&':
            entity = "&amp;";
            break;
        case '<':
            entity = "&lt;";
            break;
        case '>':
            entity = "&gt;";
            break;
        case '"':
            entity = "&quot;";
            break;
        case '\'':
            entity = "&apos;";
            break;
        default:
            break;
        }
        if (entity != NULL) {
            xml_emit(out, cap, &len, entity, strlen(entity));
        } else {
            char ch = (char)c;
            if (c < 0x20U && c != '\t' && c != '\n' && c != '\r') {
                ch = '?';
            }
            xml_emit(out, cap, &len, &ch, 1U);
        }
    }
    if (out != NULL && cap > 0U) {
        out[len < cap ? len : cap - 1U] = '\0';
    }
    return len;
}

static void xml_append_escaped(NtestBuf *b, const char *s)
{
    const size_t n = ntest_xml_escape(s, NULL, 0U);
    buf_reserve(b, n);
    ntest_xml_escape(s, b->data + b->len, n + 1U);
    b->len += n;
}

// The JUnit "message" attribute carries the first failure line, without its
// indentation.
static void xml_append_first_line(NtestBuf *b, const char *text)
{
    while (*text == ' ') {
        ++text;
    }
    const char *nl = strchr(text, '\n');
    const size_t n = nl != NULL ? (size_t)(nl - text) : strlen(text);
    char *line = (char *)malloc(n + 1U);
    if (line == NULL) {
        return;
    }
    memcpy(line, text, n);
    line[n] = '\0';
    xml_append_escaped(b, line);
    free(line);
}

static void xml_append_case(NtestBuf *b, const NtestResult *r)
{
    const NtestCase *tc = r->tc;
    buf_append(b, "    <testcase classname=\"");
    xml_append_escaped(b, tc->suite);
    buf_append(b, "\" name=\"");
    xml_append_escaped(b, tc->name);
    buf_append(b, "\" file=\"");
    xml_append_escaped(b, tc->file);
    buf_appendf(b, "\" line=\"%d\" time=\"%.6f\"", tc->line, r->seconds);
    if (r->status == NTEST_STATUS_PASSED && r->log.len == 0U) {
        buf_append(b, "/>\n");
        return;
    }
    buf_append(b, ">\n");
    if (r->status == NTEST_STATUS_FAILED) {
        buf_append(b, "      <failure message=\"");
        xml_append_first_line(b, buf_cstr(&r->failures));
        buf_append(b, "\" type=\"assertion\">");
        xml_append_escaped(b, buf_cstr(&r->failures));
        buf_append(b, "</failure>\n");
    }
    if (r->status == NTEST_STATUS_SKIPPED) {
        buf_append(b, "      <skipped message=\"");
        xml_append_escaped(b, buf_cstr(&r->skip_reason));
        buf_append(b, "\"/>\n");
    }
    if (r->log.len > 0U) {
        buf_append(b, "      <system-out>");
        xml_append_escaped(b, buf_cstr(&r->log));
        buf_append(b, "</system-out>\n");
    }
    buf_append(b, "    </testcase>\n");
}

static bool write_junit(const char *path, const char *exe_name, const NtestResult *results,
                        size_t n, double total_seconds)
{
    size_t failures = 0;
    size_t skipped = 0;
    for (size_t i = 0; i < n; ++i) {
        failures += results[i].status == NTEST_STATUS_FAILED ? 1U : 0U;
        skipped += results[i].status == NTEST_STATUS_SKIPPED ? 1U : 0U;
    }

    char timestamp[32] = "";
    const time_t now = time(NULL);
    const struct tm *utc = gmtime(&now);
    if (utc != NULL) {
        strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", utc);
    }

    NtestBuf b = {NULL, 0, 0};
    buf_append(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites name=\"");
    xml_append_escaped(&b, exe_name);
    buf_appendf(&b, "\" tests=\"%zu\" failures=\"%zu\" skipped=\"%zu\" time=\"%.6f\">\n", n,
                failures, skipped, total_seconds);

    // One <testsuite> per distinct suite, in first-appearance order.
    for (size_t i = 0; i < n; ++i) {
        const char *suite = results[i].tc->suite;
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) {
            seen = strcmp(results[j].tc->suite, suite) == 0;
        }
        if (seen) {
            continue;
        }
        size_t s_tests = 0;
        size_t s_failures = 0;
        size_t s_skipped = 0;
        double s_seconds = 0.0;
        for (size_t k = i; k < n; ++k) {
            if (strcmp(results[k].tc->suite, suite) != 0) {
                continue;
            }
            ++s_tests;
            s_failures += results[k].status == NTEST_STATUS_FAILED ? 1U : 0U;
            s_skipped += results[k].status == NTEST_STATUS_SKIPPED ? 1U : 0U;
            s_seconds += results[k].seconds;
        }
        buf_append(&b, "  <testsuite name=\"");
        xml_append_escaped(&b, suite);
        buf_appendf(&b,
                    "\" tests=\"%zu\" failures=\"%zu\" skipped=\"%zu\" time=\"%.6f\" "
                    "timestamp=\"%s\">\n",
                    s_tests, s_failures, s_skipped, s_seconds, timestamp);
        for (size_t k = i; k < n; ++k) {
            if (strcmp(results[k].tc->suite, suite) == 0) {
                xml_append_case(&b, &results[k]);
            }
        }
        buf_append(&b, "  </testsuite>\n");
    }
    buf_append(&b, "</testsuites>\n");

    make_parent_directories(path);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "ntest: cannot open '%s' for writing\n", path);
        buf_free(&b);
        return false;
    }
    fwrite(b.data, 1U, b.len, f);
    fclose(f);
    buf_free(&b);
    return true;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

typedef struct NtestOptions {
    bool list;
    bool quiet;
    bool verbose;
    bool shuffle;
    bool fail_fast;
    int color; // -1 auto, 0 off, 1 on
    uint64_t shuffle_seed;
    uint64_t repeat;
    uint32_t shard_index;
    uint32_t shard_count; // 0: no --shard
    const char *filter;
    const char *junit_path;
} NtestOptions;

static void print_usage(FILE *out, const char *exe)
{
    fprintf(out,
            "Usage: %s [options]\n"
            "  --list              List the selected tests (suite.name per line) and exit\n"
            "  --filter=PATTERNS   Comma-separated globs ('*', '?') matched against\n"
            "                      suite.name; a pattern without '.' also matches a suite;\n"
            "                      prefix with '-' to exclude.  e.g. --filter=math.*,-math.slow_*\n"
            "  --shard=I/N         Run every N-th selected test from the I-th (0-based) in the\n"
            "                      default order: shards 0 to N-1 run each test once\n"
            "  --junit=FILE        Write a JUnit XML report (parent directories are created)\n"
            "  --repeat=N          Run the selected tests N times\n"
            "  --shuffle[=SEED]    Run in random order; the seed is printed for reproduction\n"
            "  --fail-fast         Stop after the first failing test\n"
            "  --quiet             Print only failures and the summary\n"
            "  --verbose           Also print NTEST_LOG output of passing tests\n"
            "  --color, --no-color Force color on or off (default: on for terminals unless\n"
            "                      NO_COLOR is set)\n"
            "  --help              Show this help\n"
            "Exit codes: 0 all passed, 1 a test failed, 2 usage error, 3 no tests matched\n",
            exe);
}

static bool parse_u64_arg(const char *s, uint64_t *out)
{
    if (*s == '\0' || *s == '-' || *s == '+') {
        return false;
    }
    char *end = NULL;
    const unsigned long long v = strtoull(s, &end, 10);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static bool has_prefix(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool ntest_parse_shard(const char *s, uint32_t *index, uint32_t *count)
{
    const char *slash = strchr(s, '/');
    if (slash == NULL) {
        return false;
    }
    char head[24];
    const size_t n = (size_t)(slash - s);
    if (n == 0U || n >= sizeof head) {
        return false;
    }
    memcpy(head, s, n);
    head[n] = '\0';
    uint64_t i = 0U;
    uint64_t c = 0U;
    if (!parse_u64_arg(head, &i) || !parse_u64_arg(slash + 1, &c) || c == 0U || c > 1000000U ||
        i >= c) {
        return false;
    }
    *index = (uint32_t)i;
    *count = (uint32_t)c;
    return true;
}

bool ntest_shard_accepts(size_t position, uint32_t index, uint32_t count)
{
    return position % count == index;
}

// Returns 0 on success, 2 on a usage error, -1 when --help was printed.
static int parse_options(int argc, char **argv, NtestOptions *opt)
{
    memset(opt, 0, sizeof *opt);
    opt->color = -1;
    opt->repeat = 1U;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--list") == 0) {
            opt->list = true;
        } else if (has_prefix(a, "--filter=")) {
            opt->filter = a + strlen("--filter=");
        } else if (has_prefix(a, "--shard=")) {
            if (!ntest_parse_shard(a + strlen("--shard="), &opt->shard_index, &opt->shard_count)) {
                fprintf(stderr, "ntest: --shard needs I/N with 0 <= I < N <= 1000000\n");
                return 2;
            }
        } else if (has_prefix(a, "--junit=")) {
            opt->junit_path = a + strlen("--junit=");
        } else if (has_prefix(a, "--repeat=")) {
            if (!parse_u64_arg(a + strlen("--repeat="), &opt->repeat) || opt->repeat == 0U ||
                opt->repeat > 1000000U) {
                fprintf(stderr, "ntest: --repeat needs a count between 1 and 1000000\n");
                return 2;
            }
        } else if (strcmp(a, "--shuffle") == 0) {
            opt->shuffle = true;
            opt->shuffle_seed = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL ^ (uint64_t)clock();
        } else if (has_prefix(a, "--shuffle=")) {
            opt->shuffle = true;
            if (!parse_u64_arg(a + strlen("--shuffle="), &opt->shuffle_seed)) {
                fprintf(stderr, "ntest: --shuffle needs an unsigned integer seed\n");
                return 2;
            }
        } else if (strcmp(a, "--fail-fast") == 0) {
            opt->fail_fast = true;
        } else if (strcmp(a, "--quiet") == 0) {
            opt->quiet = true;
        } else if (strcmp(a, "--verbose") == 0) {
            opt->verbose = true;
        } else if (strcmp(a, "--color") == 0) {
            opt->color = 1;
        } else if (strcmp(a, "--no-color") == 0) {
            opt->color = 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(stdout, path_basename(argv[0]));
            return -1;
        } else {
            fprintf(stderr, "ntest: unknown option '%s'\n\n", a);
            print_usage(stderr, path_basename(argv[0]));
            return 2;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int ntest_main(int argc, char **argv)
{
    NtestOptions opt;
    const int parse_rc = parse_options(argc, argv, &opt);
    if (parse_rc == -1) {
        return 0;
    }
    if (parse_rc != 0) {
        return parse_rc;
    }

    g_run.use_color =
        opt.color == 1 || (opt.color == -1 && stdout_is_terminal() && getenv("NO_COLOR") == NULL);
    if (g_run.use_color) {
        enable_ansi_colors();
    }

    const char *exe = argc > 0 ? path_basename(argv[0]) : "ntest";
    char exe_name[128];
    snprintf(exe_name, sizeof exe_name, "%s", exe);
    char *dot = strrchr(exe_name, '.');
    if (dot != NULL && dot != exe_name) {
        *dot = '\0';
    }

    if (g_registry_count == 0U) {
        fprintf(stderr, "ntest: no tests are registered in %s\n", exe_name);
        return 3;
    }

    // Deterministic default order: by file, then line. Link order is not.
    const NtestCase **all = (const NtestCase **)malloc(g_registry_count * sizeof *all);
    const NtestCase **selected = (const NtestCase **)malloc(g_registry_count * sizeof *selected);
    if (all == NULL || selected == NULL) {
        fputs("ntest: out of memory\n", stderr);
        abort();
    }
    size_t count = 0;
    for (const NtestCase *tc = g_registry; tc != NULL; tc = tc->next) {
        all[count++] = tc;
    }
    qsort((void *)all, count, sizeof *all, ntest_compare_cases);

    size_t selected_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (ntest_filter_accepts(opt.filter, all[i]->suite, all[i]->name)) {
            selected[selected_count++] = all[i];
        }
    }
    free((void *)all);
    // A shard keeps its share of the filter's selection, in the default
    // order, before any shuffle.
    const size_t matched = selected_count;
    if (opt.shard_count > 0U) {
        size_t kept = 0;
        for (size_t i = 0; i < matched; ++i) {
            if (ntest_shard_accepts(i, opt.shard_index, opt.shard_count)) {
                selected[kept++] = selected[i];
            }
        }
        selected_count = kept;
    }

    if (opt.list) {
        for (size_t i = 0; i < selected_count; ++i) {
            printf("%s.%s\n", selected[i]->suite, selected[i]->name);
        }
        free((void *)selected);
        return 0;
    }

    if (matched == 0U) {
        fprintf(stderr, "ntest: no tests match the filter '%s'\n", opt.filter ? opt.filter : "");
        free((void *)selected);
        return 3;
    }
    if (selected_count == 0U) {
        // More shards than tests: the others run them all.
        printf("ntest: shard %u/%u holds none of the %zu selected test%s\n",
               (unsigned)opt.shard_index, (unsigned)opt.shard_count, matched, plural(matched));
        free((void *)selected);
        return 0;
    }

    if (opt.shuffle) {
        uint64_t state = opt.shuffle_seed;
        for (size_t i = selected_count - 1U; i > 0U; --i) {
            const size_t j = (size_t)(splitmix64_next(&state) % (uint64_t)(i + 1U));
            const NtestCase *tmp = selected[i];
            selected[i] = selected[j];
            selected[j] = tmp;
        }
        printf("shuffle seed: %" PRIu64 "\n", opt.shuffle_seed);
    }

    const size_t total_runs = selected_count * (size_t)opt.repeat;
    NtestResult *results = (NtestResult *)calloc(total_runs, sizeof *results);
    if (results == NULL) {
        fputs("ntest: out of memory\n", stderr);
        abort();
    }

    const size_t suites = count_distinct_suites(selected, selected_count);
    if (!opt.quiet) {
        printf("[==========] Running %zu test%s from %zu suite%s", selected_count,
               plural(selected_count), suites, plural(suites));
        if (opt.repeat > 1U) {
            printf(", %" PRIu64 " times", opt.repeat);
        }
        if (opt.shard_count > 0U) {
            printf(", shard %u/%u of %zu", (unsigned)opt.shard_index, (unsigned)opt.shard_count,
                   matched);
        }
        printf(" (%s)\n", exe_name);
    }

    const double start = now_seconds();
    size_t ran = 0;
    bool stop = false;
    for (uint64_t rep = 0; rep < opt.repeat && !stop; ++rep) {
        for (size_t i = 0; i < selected_count && !stop; ++i) {
            const NtestCase *tc = selected[i];
            if (!opt.quiet) {
                printf("[ RUN      ] %s.%s\n", tc->suite, tc->name);
            }
            fflush(stdout);
            NtestResult *r = &results[ran++];
            run_case(tc, r);
            print_result(r, opt.quiet, opt.verbose);
            if (r->status == NTEST_STATUS_FAILED && opt.fail_fast) {
                stop = true;
            }
        }
    }
    const double total_seconds = now_seconds() - start;

    size_t passed = 0;
    size_t failed = 0;
    size_t skipped = 0;
    for (size_t i = 0; i < ran; ++i) {
        switch (results[i].status) {
        case NTEST_STATUS_PASSED:
            ++passed;
            break;
        case NTEST_STATUS_FAILED:
            ++failed;
            break;
        case NTEST_STATUS_SKIPPED:
            ++skipped;
            break;
        }
    }

    printf("[==========] %zu test%s ran (%.2f ms total)\n", ran, plural(ran),
           total_seconds * 1000.0);
    printf("%s[  PASSED  ]%s %zu test%s\n", paint(ANSI_GREEN), paint(ANSI_RESET), passed,
           plural(passed));
    if (skipped > 0U) {
        printf("%s[  SKIPPED ]%s %zu test%s\n", paint(ANSI_YELLOW), paint(ANSI_RESET), skipped,
               plural(skipped));
    }
    if (failed > 0U) {
        printf("%s[  FAILED  ]%s %zu test%s\n", paint(ANSI_RED), paint(ANSI_RESET), failed,
               plural(failed));
        for (size_t i = 0; i < ran; ++i) {
            if (results[i].status == NTEST_STATUS_FAILED) {
                printf("%s[  FAILED  ]%s %s.%s\n", paint(ANSI_RED), paint(ANSI_RESET),
                       results[i].tc->suite, results[i].tc->name);
            }
        }
    }
    if (stop) {
        printf("stopped after the first failure (--fail-fast)\n");
    }
    fflush(stdout);

    bool junit_ok = true;
    if (opt.junit_path != NULL) {
        junit_ok = write_junit(opt.junit_path, exe_name, results, ran, total_seconds);
    }

    for (size_t i = 0; i < ran; ++i) {
        buf_free(&results[i].failures);
        buf_free(&results[i].log);
        buf_free(&results[i].skip_reason);
    }
    free(results);
    free((void *)selected);

    if (failed > 0U) {
        return 1;
    }
    return junit_ok ? 0 : 2;
}
