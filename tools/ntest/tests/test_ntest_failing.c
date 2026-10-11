// SPDX-License-Identifier: Apache-2.0
// An executable whose tests deliberately fail. tests/CMakeLists.txt runs it
// through run_check.cmake and asserts on its exit code, output, and JUnit
// report. Keep the counts in sync with those expectations:
//   2 pass (fail.passes, fail.teardown_observed), 1 skips, 8 fail.
// Test order is by line number, which fail.teardown_observed relies on.
#include <ntest/ntest.h>

#include <stdint.h>

NTEST(fail, passes)
{
    NTEST_ASSERT_TRUE(true);
}

NTEST(fail, eq_int)
{
    NTEST_ASSERT_EQ_INT(1, 2);
    NTEST_LOG("unreachable after fatal assert");
}

NTEST(fail, eq_str)
{
    NTEST_ASSERT_EQ_STR("alpha", "alpXa");
}

NTEST(fail, eq_mem)
{
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 9, 4};
    NTEST_ASSERT_EQ_MEM(a, b, sizeof a);
}

NTEST(fail, ulp)
{
    NTEST_ASSERT_ULP_F32(1.0F, 1.5F, 4);
}

NTEST(fail, near)
{
    NTEST_ASSERT_NEAR_F64(1.0, 2.0, 0.5);
}

NTEST(fail, expect_continues)
{
    NTEST_EXPECT_EQ_INT(1, 2);
    NTEST_LOG("continues after expect <html>");
    NTEST_EXPECT_TRUE(false);
}

NTEST(fail, explicit)
{
    NTEST_FAIL("explicit failure %d", 7);
}

NTEST(fail, skipped)
{
    NTEST_SKIP("not today");
}

typedef struct TeardownFixture {
    int x;
} TeardownFixture;

static int g_teardown_ran = 0;

NTEST_SETUP(TeardownFixture)
{
    f->x = 1;
}

NTEST_TEARDOWN(TeardownFixture)
{
    NTEST_UNUSED(f);
    g_teardown_ran = 1;
}

NTEST_F(TeardownFixture, teardown_runs_after_fatal)
{
    NTEST_ASSERT_EQ_INT(f->x, 2);
}

NTEST(fail, teardown_observed)
{
    // Passes only if the fixture teardown above ran despite the fatal assertion.
    NTEST_ASSERT_EQ_INT(g_teardown_ran, 1);
}
