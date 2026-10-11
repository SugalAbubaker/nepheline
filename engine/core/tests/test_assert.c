// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>

#include "test_support.h"

#include <stdio.h>
#include <string.h>

NTEST(assert, handler_round_trip)
{
    NphAssertHandler prev = NULL;
    void *prev_user = NULL;
    TestAssertRecorder rec = {0U, NULL, NULL, 0U};
    nph_assert_set_handler(test_assert_record, &rec, &prev, &prev_user);

    NphAssertHandler current = NULL;
    void *current_user = NULL;
    nph_assert_set_handler(prev, prev_user, &current, &current_user);
    NTEST_ASSERT_TRUE(current == test_assert_record);
    NTEST_ASSERT_EQ_PTR(current_user, &rec);
}

#if NPH_ASSERTS_ENABLED

NTEST(assert, failed_assert_reports_condition_and_location)
{
    TestAssertRecorder rec = {0U, NULL, NULL, 0U};
    NphAssertHandler prev = NULL;
    void *prev_user = NULL;
    nph_assert_set_handler(test_assert_record, &rec, &prev, &prev_user);

    const int x = 3;
    NPH_ASSERT(x == 4);
    const u32 line = (u32)__LINE__ - 1U;

    nph_assert_set_handler(prev, prev_user, NULL, NULL);
    NTEST_ASSERT_EQ_UINT(rec.count, 1U);
    NTEST_ASSERT_EQ_STR(rec.last_condition, "x == 4");
    NTEST_ASSERT_EQ_UINT(rec.last_line, line);
    NTEST_ASSERT_NOT_NULL(strstr(rec.last_file, "test_assert.c"));
}

typedef struct MessageRecorder {
    char message[256];
} MessageRecorder;

static NphAssertAction record_message(const NphAssertInfo *info, void *user)
{
    MessageRecorder *m = (MessageRecorder *)user;
    snprintf(m->message, sizeof m->message, "%s", info->message);
    return NPH_ASSERT_ACTION_CONTINUE;
}

NTEST(assert, message_is_formatted)
{
    MessageRecorder m;
    m.message[0] = '\0';
    NphAssertHandler prev = NULL;
    void *prev_user = NULL;
    nph_assert_set_handler(record_message, &m, &prev, &prev_user);
    NPH_ASSERT_MSG(1 + 1 == 3, "value was %d and %s", 42, "text");
    nph_assert_set_handler(prev, prev_user, NULL, NULL);
    NTEST_ASSERT_EQ_STR(m.message, "value was 42 and text");
}

NTEST(assert, passing_assert_does_not_call_handler)
{
    TestAssertRecorder rec = {0U, NULL, NULL, 0U};
    NphAssertHandler prev = NULL;
    void *prev_user = NULL;
    nph_assert_set_handler(test_assert_record, &rec, &prev, &prev_user);
    NPH_ASSERT(1 == 1);
    NPH_ASSERT_MSG(2 > 1, "never shown");
    NPH_VERIFY(true);
    nph_assert_set_handler(prev, prev_user, NULL, NULL);
    NTEST_ASSERT_EQ_UINT(rec.count, 0U);
}

NTEST(assert, unreachable_fires)
{
    NPH_TEST_EXPECT_ASSERT(NPH_UNREACHABLE());
}

#endif

NTEST(assert, verify_always_evaluates)
{
    int calls = 0;
    NPH_TEST_IGNORE_ASSERTS(NPH_VERIFY(++calls == 1));
    NPH_TEST_IGNORE_ASSERTS(NPH_VERIFY_MSG(++calls == 2, "second"));
    NTEST_ASSERT_EQ_INT(calls, 2);
}

NTEST(assert, assert_does_not_evaluate_when_disabled)
{
    // Observable only in configurations without asserts; with asserts the
    // condition is true, so this is a no-op in both cases.
    int calls = 0;
    NPH_ASSERT(++calls >= 0);
#if NPH_ASSERTS_ENABLED
    NTEST_ASSERT_EQ_INT(calls, 1);
#else
    NTEST_ASSERT_EQ_INT(calls, 0);
#endif
}
