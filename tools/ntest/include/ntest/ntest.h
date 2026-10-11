// SPDX-License-Identifier: Apache-2.0
#pragma once

// ntest: Nepheline's C test framework (docs/ntest.md).
//
//     NTEST(math, adds)
//     {
//         NTEST_ASSERT_EQ_INT(1 + 1, 2);
//     }
//
//     typedef struct { int *buf; } BufFixture;
//     NTEST_SETUP(BufFixture)    { f->buf = malloc(16); }
//     NTEST_TEARDOWN(BufFixture) { free(f->buf); }
//     NTEST_F(BufFixture, allocates) { NTEST_ASSERT_NOT_NULL(f->buf); }
//
// Tests register themselves at load time; link ntest::main (or call
// ntest_main from your own main) to get the runner and its command line.
//
// NTEST_ASSERT_* aborts the current test on failure (via longjmp, so the
// fixture teardown still runs). NTEST_EXPECT_* records the failure, returns
// false, and lets the test continue. Every failure prints the two operand
// expressions with their values; strings and memory also print where they
// first differ.
//
// Thread-safety: none. Tests run sequentially on the thread that called
// ntest_main. Assertions must be made from that thread.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NTEST_PRINTF(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define NTEST_PRINTF(fmt_index, first_arg)
#endif

// Silences unused-parameter errors for a fixture (or any value) a test body
// deliberately does not touch.
#define NTEST_UNUSED(x) ((void)(x))

// ---------------------------------------------------------------------------
// Registration (used by the macros below; not called directly)
// ---------------------------------------------------------------------------

typedef void (*NtestFn)(void);
typedef void (*NtestFixtureFn)(void *fixture);

typedef struct NtestCase {
    const char *suite;
    const char *name;
    const char *file;
    int line;
    NtestFn fn;
    size_t fixture_size;     // 0 for fixture-less tests
    NtestFixtureFn setup;    // NULL for fixture-less tests
    NtestFixtureFn teardown; // NULL for fixture-less tests
    struct NtestCase *next;  // registry link, owned by ntest
} NtestCase;

// Adds a test to the global registry. The case must have static storage.
void ntest_register(NtestCase *tc);

// Returns the running test's fixture memory (zero-initialised, fixture_size
// bytes, freed after teardown). NULL for fixture-less tests.
void *ntest_fixture(void);

// Runs a function when the executable loads: GCC/Clang constructor attribute,
// or an entry in MSVC's .CRT$XCU initializer section. x64 only (no leading
// underscore in the /include symbol name).
#if defined(_MSC_VER) && !defined(__clang__)
#pragma section(".CRT$XCU", read)
#define NTEST_INITIALIZER_(fn)                                                                     \
    static void fn(void);                                                                          \
    __declspec(allocate(".CRT$XCU")) void (*fn##_ptr_)(void) = fn;                                 \
    __pragma(comment(linker, "/include:" #fn "_ptr_")) static void fn(void)
#elif defined(__GNUC__) || defined(__clang__)
#define NTEST_INITIALIZER_(fn) __attribute__((constructor)) static void fn(void)
#else
#error "ntest: unsupported compiler (needs a load-time initializer mechanism)"
#endif

// ---------------------------------------------------------------------------
// Test definition
// ---------------------------------------------------------------------------

// NTEST(suite, name) { body }
#define NTEST(suite, name)                                                                         \
    static void ntest_body_##suite##_##name(void);                                                 \
    static NtestCase ntest_case_##suite##_##name = {                                               \
        #suite, #name, __FILE__, __LINE__, ntest_body_##suite##_##name, 0, NULL, NULL, NULL};      \
    NTEST_INITIALIZER_(ntest_reg_##suite##_##name)                                                 \
    {                                                                                              \
        ntest_register(&ntest_case_##suite##_##name);                                              \
    }                                                                                              \
    static void ntest_body_##suite##_##name(void)

// The fixture type is a macro argument used in declarators (`Fixture *f`),
// where parentheses are not possible; bugprone-macro-parentheses cannot
// tell type arguments from expression arguments.
// NOLINTBEGIN(bugprone-macro-parentheses)
// NTEST_SETUP(Fixture) { body using Fixture *f }
// Runs before each NTEST_F of that fixture. f is zero-initialised.
#define NTEST_SETUP(Fixture)                                                                       \
    static void ntest_setup_##Fixture(Fixture *f);                                                 \
    static void ntest_setup_thunk_##Fixture(void *fixture)                                         \
    {                                                                                              \
        ntest_setup_##Fixture((Fixture *)fixture);                                                 \
    }                                                                                              \
    static void ntest_setup_##Fixture(Fixture *f)

// NTEST_TEARDOWN(Fixture) { body using Fixture *f }
// Runs after each NTEST_F of that fixture, including after a fatal assertion.
#define NTEST_TEARDOWN(Fixture)                                                                    \
    static void ntest_teardown_##Fixture(Fixture *f);                                              \
    static void ntest_teardown_thunk_##Fixture(void *fixture)                                      \
    {                                                                                              \
        ntest_teardown_##Fixture((Fixture *)fixture);                                              \
    }                                                                                              \
    static void ntest_teardown_##Fixture(Fixture *f)

// NTEST_F(Fixture, name) { body using Fixture *f }
// The suite name is the fixture type. Both NTEST_SETUP and NTEST_TEARDOWN
// must be defined for the fixture earlier in the same translation unit.
#define NTEST_F(Fixture, name)                                                                     \
    static void ntest_body_##Fixture##_##name(Fixture *f);                                         \
    static void ntest_thunk_##Fixture##_##name(void)                                               \
    {                                                                                              \
        ntest_body_##Fixture##_##name((Fixture *)ntest_fixture());                                 \
    }                                                                                              \
    static NtestCase ntest_case_##Fixture##_##name = {#Fixture,                                    \
                                                      #name,                                       \
                                                      __FILE__,                                    \
                                                      __LINE__,                                    \
                                                      ntest_thunk_##Fixture##_##name,              \
                                                      sizeof(Fixture),                             \
                                                      ntest_setup_thunk_##Fixture,                 \
                                                      ntest_teardown_thunk_##Fixture,              \
                                                      NULL};                                       \
    NTEST_INITIALIZER_(ntest_reg_##Fixture##_##name)                                               \
    {                                                                                              \
        ntest_register(&ntest_case_##Fixture##_##name);                                            \
    }                                                                                              \
    static void ntest_body_##Fixture##_##name(Fixture *f)
// NOLINTEND(bugprone-macro-parentheses)

// ---------------------------------------------------------------------------
// Checks (called through the macros below). Each returns whether the check
// passed; with fatal == true a failure does not return.
// ---------------------------------------------------------------------------

typedef enum NtestOp {
    NTEST_OP_EQ,
    NTEST_OP_NE,
    NTEST_OP_LT,
    NTEST_OP_LE,
    NTEST_OP_GT,
    NTEST_OP_GE,
} NtestOp;

bool ntest_check_bool(bool fatal, const char *file, int line, const char *expr, bool value,
                      bool expected);
bool ntest_check_int(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, intmax_t lhs, intmax_t rhs, NtestOp op);
bool ntest_check_uint(bool fatal, const char *file, int line, const char *lhs_expr,
                      const char *rhs_expr, uintmax_t lhs, uintmax_t rhs, NtestOp op);
bool ntest_check_ptr(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, const void *lhs, const void *rhs, NtestOp op);
bool ntest_check_str(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, const char *lhs, const char *rhs, NtestOp op);
bool ntest_check_mem(bool fatal, const char *file, int line, const char *lhs_expr,
                     const char *rhs_expr, const void *lhs, const void *rhs, size_t size);
bool ntest_check_near_f64(bool fatal, const char *file, int line, const char *lhs_expr,
                          const char *rhs_expr, double lhs, double rhs, double tolerance);
bool ntest_check_ulp_f32(bool fatal, const char *file, int line, const char *lhs_expr,
                         const char *rhs_expr, float lhs, float rhs, uint32_t max_ulps);
bool ntest_check_ulp_f64(bool fatal, const char *file, int line, const char *lhs_expr,
                         const char *rhs_expr, double lhs, double rhs, uint64_t max_ulps);

// Records a failure with a printf-style message.
NTEST_PRINTF(4, 5) void ntest_fail(bool fatal, const char *file, int line, const char *fmt, ...);

// Marks the running test as skipped and leaves it immediately.
NTEST_PRINTF(3, 4) void ntest_skip(const char *file, int line, const char *fmt, ...);

// Attaches a line to the running test's log. Shown for failing tests, and for
// passing tests with --verbose. Included in the JUnit report.
NTEST_PRINTF(1, 2) void ntest_log(const char *fmt, ...);

// ---------------------------------------------------------------------------
// Assertion macros. ASSERT aborts the test; EXPECT continues and yields bool.
// Integer comparisons go through intmax_t / uintmax_t; use the UINT family
// for unsigned operands wider than 63 bits.
// ---------------------------------------------------------------------------

#define NTEST_ASSERT_TRUE(x)  ((void)ntest_check_bool(true, __FILE__, __LINE__, #x, !!(x), true))
#define NTEST_ASSERT_FALSE(x) ((void)ntest_check_bool(true, __FILE__, __LINE__, #x, !!(x), false))
#define NTEST_EXPECT_TRUE(x)  ntest_check_bool(false, __FILE__, __LINE__, #x, !!(x), true)
#define NTEST_EXPECT_FALSE(x) ntest_check_bool(false, __FILE__, __LINE__, #x, !!(x), false)

#define NTEST_CHECK_INT_(fatal, a, b, op)                                                          \
    ntest_check_int(fatal, __FILE__, __LINE__, #a, #b, (intmax_t)(a), (intmax_t)(b), op)
#define NTEST_ASSERT_EQ_INT(a, b) ((void)NTEST_CHECK_INT_(true, a, b, NTEST_OP_EQ))
#define NTEST_ASSERT_NE_INT(a, b) ((void)NTEST_CHECK_INT_(true, a, b, NTEST_OP_NE))
#define NTEST_ASSERT_LT_INT(a, b) ((void)NTEST_CHECK_INT_(true, a, b, NTEST_OP_LT))
#define NTEST_ASSERT_LE_INT(a, b) ((void)NTEST_CHECK_INT_(true, a, b, NTEST_OP_LE))
#define NTEST_ASSERT_GT_INT(a, b) ((void)NTEST_CHECK_INT_(true, a, b, NTEST_OP_GT))
#define NTEST_ASSERT_GE_INT(a, b) ((void)NTEST_CHECK_INT_(true, a, b, NTEST_OP_GE))
#define NTEST_EXPECT_EQ_INT(a, b) NTEST_CHECK_INT_(false, a, b, NTEST_OP_EQ)
#define NTEST_EXPECT_NE_INT(a, b) NTEST_CHECK_INT_(false, a, b, NTEST_OP_NE)
#define NTEST_EXPECT_LT_INT(a, b) NTEST_CHECK_INT_(false, a, b, NTEST_OP_LT)
#define NTEST_EXPECT_LE_INT(a, b) NTEST_CHECK_INT_(false, a, b, NTEST_OP_LE)
#define NTEST_EXPECT_GT_INT(a, b) NTEST_CHECK_INT_(false, a, b, NTEST_OP_GT)
#define NTEST_EXPECT_GE_INT(a, b) NTEST_CHECK_INT_(false, a, b, NTEST_OP_GE)

#define NTEST_CHECK_UINT_(fatal, a, b, op)                                                         \
    ntest_check_uint(fatal, __FILE__, __LINE__, #a, #b, (uintmax_t)(a), (uintmax_t)(b), op)
#define NTEST_ASSERT_EQ_UINT(a, b) ((void)NTEST_CHECK_UINT_(true, a, b, NTEST_OP_EQ))
#define NTEST_ASSERT_NE_UINT(a, b) ((void)NTEST_CHECK_UINT_(true, a, b, NTEST_OP_NE))
#define NTEST_ASSERT_LT_UINT(a, b) ((void)NTEST_CHECK_UINT_(true, a, b, NTEST_OP_LT))
#define NTEST_ASSERT_LE_UINT(a, b) ((void)NTEST_CHECK_UINT_(true, a, b, NTEST_OP_LE))
#define NTEST_ASSERT_GT_UINT(a, b) ((void)NTEST_CHECK_UINT_(true, a, b, NTEST_OP_GT))
#define NTEST_ASSERT_GE_UINT(a, b) ((void)NTEST_CHECK_UINT_(true, a, b, NTEST_OP_GE))
#define NTEST_EXPECT_EQ_UINT(a, b) NTEST_CHECK_UINT_(false, a, b, NTEST_OP_EQ)
#define NTEST_EXPECT_NE_UINT(a, b) NTEST_CHECK_UINT_(false, a, b, NTEST_OP_NE)
#define NTEST_EXPECT_LT_UINT(a, b) NTEST_CHECK_UINT_(false, a, b, NTEST_OP_LT)
#define NTEST_EXPECT_LE_UINT(a, b) NTEST_CHECK_UINT_(false, a, b, NTEST_OP_LE)
#define NTEST_EXPECT_GT_UINT(a, b) NTEST_CHECK_UINT_(false, a, b, NTEST_OP_GT)
#define NTEST_EXPECT_GE_UINT(a, b) NTEST_CHECK_UINT_(false, a, b, NTEST_OP_GE)

#define NTEST_CHECK_PTR_(fatal, a, b, op)                                                          \
    ntest_check_ptr(fatal, __FILE__, __LINE__, #a, #b, (const void *)(a), (const void *)(b), op)
#define NTEST_ASSERT_EQ_PTR(a, b) ((void)NTEST_CHECK_PTR_(true, a, b, NTEST_OP_EQ))
#define NTEST_ASSERT_NE_PTR(a, b) ((void)NTEST_CHECK_PTR_(true, a, b, NTEST_OP_NE))
#define NTEST_ASSERT_NULL(p)      ((void)NTEST_CHECK_PTR_(true, p, NULL, NTEST_OP_EQ))
#define NTEST_ASSERT_NOT_NULL(p)  ((void)NTEST_CHECK_PTR_(true, p, NULL, NTEST_OP_NE))
#define NTEST_EXPECT_EQ_PTR(a, b) NTEST_CHECK_PTR_(false, a, b, NTEST_OP_EQ)
#define NTEST_EXPECT_NE_PTR(a, b) NTEST_CHECK_PTR_(false, a, b, NTEST_OP_NE)
#define NTEST_EXPECT_NULL(p)      NTEST_CHECK_PTR_(false, p, NULL, NTEST_OP_EQ)
#define NTEST_EXPECT_NOT_NULL(p)  NTEST_CHECK_PTR_(false, p, NULL, NTEST_OP_NE)

// NULL-safe: two NULLs are equal, NULL and a string are different.
#define NTEST_CHECK_STR_(fatal, a, b, op)                                                          \
    ntest_check_str(fatal, __FILE__, __LINE__, #a, #b, (a), (b), op)
#define NTEST_ASSERT_EQ_STR(a, b) ((void)NTEST_CHECK_STR_(true, a, b, NTEST_OP_EQ))
#define NTEST_ASSERT_NE_STR(a, b) ((void)NTEST_CHECK_STR_(true, a, b, NTEST_OP_NE))
#define NTEST_EXPECT_EQ_STR(a, b) NTEST_CHECK_STR_(false, a, b, NTEST_OP_EQ)
#define NTEST_EXPECT_NE_STR(a, b) NTEST_CHECK_STR_(false, a, b, NTEST_OP_NE)

#define NTEST_CHECK_MEM_(fatal, a, b, n)                                                           \
    ntest_check_mem(fatal, __FILE__, __LINE__, #a, #b, (const void *)(a), (const void *)(b),       \
                    (size_t)(n))
#define NTEST_ASSERT_EQ_MEM(a, b, n) ((void)NTEST_CHECK_MEM_(true, a, b, n))
#define NTEST_EXPECT_EQ_MEM(a, b, n) NTEST_CHECK_MEM_(false, a, b, n)

// |a - b| <= tol, or a == b (so equal infinities pass). NaN never passes.
#define NTEST_CHECK_NEAR_(fatal, a, b, tol)                                                        \
    ntest_check_near_f64(fatal, __FILE__, __LINE__, #a, #b, (double)(a), (double)(b), (double)(tol))
#define NTEST_ASSERT_NEAR_F32(a, b, tol) ((void)NTEST_CHECK_NEAR_(true, a, b, tol))
#define NTEST_ASSERT_NEAR_F64(a, b, tol) ((void)NTEST_CHECK_NEAR_(true, a, b, tol))
#define NTEST_EXPECT_NEAR_F32(a, b, tol) NTEST_CHECK_NEAR_(false, a, b, tol)
#define NTEST_EXPECT_NEAR_F64(a, b, tol) NTEST_CHECK_NEAR_(false, a, b, tol)

// Distance in representable values (units in the last place). +0 and -0 are
// 0 ULPs apart. NaN never passes.
#define NTEST_ASSERT_ULP_F32(a, b, ulps)                                                           \
    ((void)ntest_check_ulp_f32(true, __FILE__, __LINE__, #a, #b, (a), (b), (uint32_t)(ulps)))
#define NTEST_ASSERT_ULP_F64(a, b, ulps)                                                           \
    ((void)ntest_check_ulp_f64(true, __FILE__, __LINE__, #a, #b, (a), (b), (uint64_t)(ulps)))
#define NTEST_EXPECT_ULP_F32(a, b, ulps)                                                           \
    ntest_check_ulp_f32(false, __FILE__, __LINE__, #a, #b, (a), (b), (uint32_t)(ulps))
#define NTEST_EXPECT_ULP_F64(a, b, ulps)                                                           \
    ntest_check_ulp_f64(false, __FILE__, __LINE__, #a, #b, (a), (b), (uint64_t)(ulps))

// NTEST_FAIL("fmt", ...) aborts the test with a message; NTEST_ADD_FAILURE
// records one and continues; NTEST_SKIP("fmt", ...) leaves the test marked
// skipped; NTEST_LOG("fmt", ...) attaches diagnostics.
#define NTEST_FAIL(...)        ntest_fail(true, __FILE__, __LINE__, __VA_ARGS__)
#define NTEST_ADD_FAILURE(...) ntest_fail(false, __FILE__, __LINE__, __VA_ARGS__)
#define NTEST_SKIP(...)        ntest_skip(__FILE__, __LINE__, __VA_ARGS__)
#define NTEST_LOG(...)         ntest_log(__VA_ARGS__)

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

// Parses the command line (see --help), runs the selected tests, prints a
// report, optionally writes JUnit XML. Returns the process exit code:
//   0  every selected test passed (skipped tests do not fail the run)
//   1  at least one test failed
//   2  command-line usage error
//   3  no tests matched the filter (or none are registered)
int ntest_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
