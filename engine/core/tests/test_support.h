// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the core tests.
//
// NPH_TEST_EXPECT_ASSERT(stmt) runs `stmt` with an assertion handler that
// records the failure and continues, then checks that exactly one assertion
// fired. In configurations without asserts the statement simply runs; the
// code under test must then take its documented fallback path (return
// NULL, ignore the call), which the surrounding test still verifies.

#include <nepheline/core/assert.h>

#include <ntest/ntest.h>

typedef struct TestAssertRecorder {
    u32 count;
    const char *last_condition;
    const char *last_file;
    u32 last_line;
} TestAssertRecorder;

static NphAssertAction test_assert_record(const NphAssertInfo *info, void *user)
{
    TestAssertRecorder *rec = (TestAssertRecorder *)user;
    rec->count += 1U;
    rec->last_condition = info->condition;
    rec->last_file = info->file;
    rec->last_line = info->line;
    return NPH_ASSERT_ACTION_CONTINUE;
}

#if NPH_ASSERTS_ENABLED
#define NPH_TEST_EXPECT_ASSERT(stmt)                                                               \
    do {                                                                                           \
        TestAssertRecorder rec_ = {0U, NULL, NULL, 0U};                                            \
        NphAssertHandler prev_ = NULL;                                                             \
        void *prev_user_ = NULL;                                                                   \
        nph_assert_set_handler(test_assert_record, &rec_, &prev_, &prev_user_);                    \
        stmt;                                                                                      \
        nph_assert_set_handler(prev_, prev_user_, NULL, NULL);                                     \
        NTEST_ASSERT_EQ_UINT(rec_.count, 1U);                                                      \
    } while (0)
#else
#define NPH_TEST_EXPECT_ASSERT(stmt)                                                               \
    do {                                                                                           \
        stmt;                                                                                      \
    } while (0)
#endif

// Runs `stmt` with assertions silenced and counted; for code paths that may
// assert more than once on purpose.
#define NPH_TEST_IGNORE_ASSERTS(stmt)                                                              \
    do {                                                                                           \
        TestAssertRecorder rec_ = {0U, NULL, NULL, 0U};                                            \
        NphAssertHandler prev_ = NULL;                                                             \
        void *prev_user_ = NULL;                                                                   \
        nph_assert_set_handler(test_assert_record, &rec_, &prev_, &prev_user_);                    \
        stmt;                                                                                      \
        nph_assert_set_handler(prev_, prev_user_, NULL, NULL);                                     \
    } while (0)

// Deterministic PRNG for randomized tests (splitmix64; Steele, Lea & Flood,
// OOPSLA 2014). Seeded explicitly so a failure is reproducible.
static inline u64 test_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Uniform in [0, bound). bound > 0.
static inline u64 test_rng_below(u64 *state, u64 bound)
{
    return test_rng_next(state) % bound;
}

// Fills `n` bytes with a pattern derived from `seed`, and checks it.
static inline void test_fill_pattern(void *p, u64 n, u64 seed)
{
    u8 *b = (u8 *)p;
    for (u64 i = 0; i < n; ++i) {
        b[i] = (u8)((seed * 31U + i) & 0xFFU);
    }
}

static inline bool test_check_pattern(const void *p, u64 n, u64 seed)
{
    const u8 *b = (const u8 *)p;
    for (u64 i = 0; i < n; ++i) {
        if (b[i] != (u8)((seed * 31U + i) & 0xFFU)) {
            return false;
        }
    }
    return true;
}
