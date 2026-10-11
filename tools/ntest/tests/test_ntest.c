// SPDX-License-Identifier: Apache-2.0
// Unit tests for ntest itself: every assertion family in its passing form,
// fixture lifecycle, and the internal helpers. The failing forms are covered
// by test_ntest_failing.c through the ntest.cli.* tests.
#include <ntest/ntest.h>

#include "../src/ntest_internal.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

// ---- assertions ------------------------------------------------------------

NTEST(assert_bool, true_and_false)
{
    NTEST_ASSERT_TRUE(1 + 1 == 2);
    NTEST_ASSERT_FALSE(1 + 1 == 3);
    const int *null_ptr = NULL;
    NTEST_ASSERT_FALSE(null_ptr);
    NTEST_ASSERT_TRUE(NTEST_EXPECT_TRUE(true));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_FALSE(false));
}

NTEST(assert_int, comparisons)
{
    NTEST_ASSERT_EQ_INT(-5, -5);
    NTEST_ASSERT_NE_INT(-5, 5);
    NTEST_ASSERT_LT_INT(INTMAX_MIN, INTMAX_MAX);
    NTEST_ASSERT_LE_INT(3, 3);
    NTEST_ASSERT_LE_INT(2, 3);
    NTEST_ASSERT_GT_INT(1, -1);
    NTEST_ASSERT_GE_INT(0, 0);
    const int8_t small = -1;
    NTEST_ASSERT_EQ_INT(small, -1);
    NTEST_ASSERT_TRUE(NTEST_EXPECT_EQ_INT(7, 7));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NE_INT(7, 8));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_LT_INT(7, 8));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_LE_INT(8, 8));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_GT_INT(9, 8));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_GE_INT(9, 9));
}

NTEST(assert_uint, comparisons)
{
    NTEST_ASSERT_EQ_UINT(UINTMAX_MAX, UINTMAX_MAX);
    NTEST_ASSERT_NE_UINT(0U, UINTMAX_MAX);
    NTEST_ASSERT_LT_UINT(0U, 1U);
    NTEST_ASSERT_LE_UINT(1U, 1U);
    NTEST_ASSERT_GT_UINT(UINT64_MAX, UINT32_MAX);
    NTEST_ASSERT_GE_UINT(UINT32_MAX, UINT32_MAX);
    const uint8_t byte = 0xFFU;
    NTEST_ASSERT_EQ_UINT(byte, 255U);
    NTEST_ASSERT_TRUE(NTEST_EXPECT_EQ_UINT(1U, 1U));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NE_UINT(1U, 2U));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_LT_UINT(1U, 2U));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_LE_UINT(2U, 2U));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_GT_UINT(3U, 2U));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_GE_UINT(3U, 3U));
}

NTEST(assert_ptr, null_and_equality)
{
    int x = 0;
    int *p = &x;
    const int *null_ptr = NULL;
    NTEST_ASSERT_EQ_PTR(p, &x);
    NTEST_ASSERT_NE_PTR(p, null_ptr);
    NTEST_ASSERT_NULL(null_ptr);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_TRUE(NTEST_EXPECT_EQ_PTR(p, &x));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NE_PTR(p, null_ptr));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NULL(null_ptr));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NOT_NULL(p));
}

NTEST(assert_str, equality_is_null_safe)
{
    char buffer[4] = "abc";
    const char *null_str = NULL;
    NTEST_ASSERT_EQ_STR("abc", "abc");
    NTEST_ASSERT_EQ_STR(buffer, "abc");
    NTEST_ASSERT_EQ_STR("", "");
    NTEST_ASSERT_EQ_STR(null_str, NULL);
    NTEST_ASSERT_NE_STR("abc", "abd");
    NTEST_ASSERT_NE_STR("abc", "ab");
    NTEST_ASSERT_NE_STR("abc", null_str);
    NTEST_ASSERT_NE_STR(null_str, "abc");
    NTEST_ASSERT_TRUE(NTEST_EXPECT_EQ_STR("x", "x"));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NE_STR("x", "y"));
}

NTEST(assert_mem, equality)
{
    const uint8_t a[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t b[8];
    memcpy(b, a, sizeof a);
    NTEST_ASSERT_EQ_MEM(a, b, sizeof a);
    NTEST_ASSERT_EQ_MEM(a, b, 0);
    b[7] = 0xFFU;
    NTEST_ASSERT_EQ_MEM(a, b, 7); // the differing byte is outside the compared range
    NTEST_ASSERT_TRUE(NTEST_EXPECT_EQ_MEM(a, b, 7));
    const void *null_block = NULL;
    NTEST_ASSERT_EQ_MEM(null_block, null_block, 0); // zero bytes are always equal
}

NTEST(assert_float, near)
{
    NTEST_ASSERT_NEAR_F32(1.0F, 1.0F + FLT_EPSILON, 1e-6F);
    NTEST_ASSERT_NEAR_F64(0.1 + 0.2, 0.3, 1e-15);
    NTEST_ASSERT_NEAR_F64(5.0, 5.0, 0.0);
    NTEST_ASSERT_NEAR_F64(INFINITY, INFINITY, 0.0); // equal infinities pass
    NTEST_ASSERT_NEAR_F64(-1.0, 1.0, 2.0);          // the boundary is inclusive
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NEAR_F32(2.0F, 2.0F, 0.0F));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_NEAR_F64(2.0, 2.0, 0.0));
}

NTEST(assert_float, ulp)
{
    NTEST_ASSERT_ULP_F32(1.0F, 1.0F, 0);
    NTEST_ASSERT_ULP_F32(1.0F, nextafterf(1.0F, 2.0F), 1);
    NTEST_ASSERT_ULP_F32(0.0F, -0.0F, 0);
    NTEST_ASSERT_ULP_F64(1.0, nextafter(1.0, 2.0), 1);
    NTEST_ASSERT_ULP_F64(-0.0, 0.0, 0);
    NTEST_ASSERT_TRUE(NTEST_EXPECT_ULP_F32(3.0F, 3.0F, 0));
    NTEST_ASSERT_TRUE(NTEST_EXPECT_ULP_F64(3.0, 3.0, 0));
}

NTEST(assert_misc, add_failure_and_log_do_not_abort)
{
    // NTEST_LOG never affects the verdict; this test must pass.
    NTEST_LOG("logging %d, %s", 42, "fine");
    NTEST_LOG("plain line");
    int x = 1;
    NTEST_UNUSED(x);
}

NTEST(skip, leaves_the_test_immediately)
{
    NTEST_SKIP("exercising NTEST_SKIP (%d)", 1);
    NTEST_FAIL("code after NTEST_SKIP must not run");
}

// ---- fixtures ----------------------------------------------------------------

typedef struct LifecycleFixture {
    int value;
    int *untouched;
} LifecycleFixture;

static int g_setup_calls = 0;
static int g_teardown_calls = 0;

NTEST_SETUP(LifecycleFixture)
{
    ++g_setup_calls;
    f->value = 42;
}

NTEST_TEARDOWN(LifecycleFixture)
{
    NTEST_UNUSED(f);
    ++g_teardown_calls;
}

NTEST_F(LifecycleFixture, memory_is_zeroed_then_set_up)
{
    NTEST_ASSERT_EQ_INT(f->value, 42);
    NTEST_ASSERT_NULL(f->untouched);
    // Setup ran for this test and every earlier fixture test was torn down.
    NTEST_ASSERT_EQ_INT(g_setup_calls, g_teardown_calls + 1);
    f->value = 7;
}

NTEST_F(LifecycleFixture, memory_is_fresh_per_test)
{
    NTEST_ASSERT_EQ_INT(f->value, 42); // not 7 from the previous test
    NTEST_ASSERT_EQ_INT(g_setup_calls, g_teardown_calls + 1);
    NTEST_ASSERT_EQ_PTR(ntest_fixture(), f);
}

// ---- internal helpers --------------------------------------------------------

NTEST(ulp, distance_f32)
{
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(1.0F, 1.0F), 0U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(1.0F, nextafterf(1.0F, 2.0F)), 1U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(nextafterf(1.0F, 2.0F), 1.0F), 1U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(0.0F, -0.0F), 0U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(-1.0F, 1.0F), 2U * 0x3F800000U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(FLT_MIN, 0.0F), 0x00800000U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(NAN, 1.0F), UINT32_MAX);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(1.0F, NAN), UINT32_MAX);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f32(INFINITY, FLT_MAX), 1U);
}

NTEST(ulp, distance_f64)
{
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f64(1.0, 1.0), 0U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f64(1.0, nextafter(1.0, 2.0)), 1U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f64(0.0, -0.0), 0U);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f64(-1.0, 1.0), 2ULL * 0x3FF0000000000000ULL);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f64((double)NAN, 1.0), UINT64_MAX);
    NTEST_ASSERT_EQ_UINT(ntest_ulp_distance_f64((double)INFINITY, DBL_MAX), 1U);
}

NTEST(glob, matching)
{
    NTEST_ASSERT_TRUE(ntest_glob_match("", ""));
    NTEST_ASSERT_FALSE(ntest_glob_match("", "a"));
    NTEST_ASSERT_TRUE(ntest_glob_match("*", ""));
    NTEST_ASSERT_TRUE(ntest_glob_match("*", "anything"));
    NTEST_ASSERT_TRUE(ntest_glob_match("**", "anything"));
    NTEST_ASSERT_TRUE(ntest_glob_match("abc", "abc"));
    NTEST_ASSERT_FALSE(ntest_glob_match("abc", "abcd"));
    NTEST_ASSERT_TRUE(ntest_glob_match("a*", "abc"));
    NTEST_ASSERT_TRUE(ntest_glob_match("*c", "abc"));
    NTEST_ASSERT_TRUE(ntest_glob_match("a?c", "abc"));
    NTEST_ASSERT_FALSE(ntest_glob_match("a?c", "ac"));
    NTEST_ASSERT_TRUE(ntest_glob_match("*.name", "suite.name"));
    NTEST_ASSERT_TRUE(ntest_glob_match("suite.*", "suite.name"));
    NTEST_ASSERT_FALSE(ntest_glob_match("suite.x*", "suite.name"));
    NTEST_ASSERT_TRUE(ntest_glob_match("a*b*c", "a123b456c"));
    NTEST_ASSERT_FALSE(ntest_glob_match("a*b*c", "a123b456"));
    NTEST_ASSERT_TRUE(ntest_glob_match("*b*", "abc"));
}

NTEST(filter, semantics)
{
    NTEST_ASSERT_TRUE(ntest_filter_accepts(NULL, "s", "n"));
    NTEST_ASSERT_TRUE(ntest_filter_accepts("", "s", "n"));
    NTEST_ASSERT_TRUE(ntest_filter_accepts("s.n", "s", "n"));
    NTEST_ASSERT_FALSE(ntest_filter_accepts("s.x", "s", "n"));
    NTEST_ASSERT_TRUE(ntest_filter_accepts("s", "s", "n")); // bare suite
    NTEST_ASSERT_FALSE(ntest_filter_accepts("other", "s", "n"));
    NTEST_ASSERT_TRUE(ntest_filter_accepts("a.*,s.*", "s", "n"));
    NTEST_ASSERT_FALSE(ntest_filter_accepts("-s.n", "s", "n"));
    NTEST_ASSERT_TRUE(ntest_filter_accepts("-s.n", "s", "other"));
    NTEST_ASSERT_FALSE(ntest_filter_accepts("s.*,-s.n", "s", "n"));
    NTEST_ASSERT_TRUE(ntest_filter_accepts("s.*,-s.n", "s", "m"));
    NTEST_ASSERT_TRUE(ntest_filter_accepts(",,s.n,,", "s", "n")); // empty entries ignored
}

NTEST(xml, escape)
{
    char out[64];
    size_t n = ntest_xml_escape("a<b>&\"'", out, sizeof out);
    NTEST_ASSERT_EQ_STR(out, "a&lt;b&gt;&amp;&quot;&apos;");
    NTEST_ASSERT_EQ_UINT(n, strlen(out));

    n = ntest_xml_escape("x\x01y\tz\n", out, sizeof out);
    NTEST_ASSERT_EQ_STR(out, "x?y\tz\n");
    NTEST_ASSERT_EQ_UINT(n, 6U);

    // Truncation keeps the terminator and still reports the full length.
    n = ntest_xml_escape("<<", out, 3);
    NTEST_ASSERT_EQ_UINT(n, 8U);
    NTEST_ASSERT_EQ_STR(out, "&l");

    // Measuring pass.
    NTEST_ASSERT_EQ_UINT(ntest_xml_escape("abc", NULL, 0), 3U);
    NTEST_ASSERT_EQ_UINT(ntest_xml_escape("", NULL, 0), 0U);
}

NTEST(order, cases_sort_by_file_then_line)
{
    NtestCase a = {"s", "a", "b.c", 10, NULL, 0, NULL, NULL, NULL};
    NtestCase b = {"s", "b", "a.c", 99, NULL, 0, NULL, NULL, NULL};
    NtestCase c = {"s", "c", "a.c", 5, NULL, 0, NULL, NULL, NULL};
    const NtestCase *pa = &a;
    const NtestCase *pb = &b;
    const NtestCase *pc = &c;
    NTEST_ASSERT_GT_INT(ntest_compare_cases((const void *)&pa, (const void *)&pb),
                        0); // "b.c" after "a.c"
    NTEST_ASSERT_LT_INT(ntest_compare_cases((const void *)&pc, (const void *)&pb),
                        0); // same file, line 5 before 99
    NTEST_ASSERT_EQ_INT(ntest_compare_cases((const void *)&pa, (const void *)&pa), 0);
}
