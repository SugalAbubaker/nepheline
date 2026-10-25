// SPDX-License-Identifier: Apache-2.0
// Real-time scopes (realtime.h): what a scope reports, and what it does not.

#include <nepheline/core/alloc.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/realtime.h>

#include "test_support.h"

#include <string.h>

NTEST(realtime, violation_names)
{
    NTEST_ASSERT_EQ_STR(nph_realtime_violation_name(NPH_REALTIME_VIOLATION_ALLOC), "alloc");
    NTEST_ASSERT_EQ_STR(nph_realtime_violation_name(NPH_REALTIME_VIOLATION_LOCK), "lock");
    NTEST_ASSERT_EQ_STR(nph_realtime_violation_name(NPH_REALTIME_VIOLATION_WAIT), "wait");
    NTEST_ASSERT_EQ_STR(nph_realtime_violation_name(NPH_REALTIME_VIOLATION_SLEEP), "sleep");
    NTEST_ASSERT_EQ_STR(nph_realtime_violation_name(NPH_REALTIME_VIOLATION_IO), "io");
    NTEST_ASSERT_EQ_STR(nph_realtime_violation_name(NPH_REALTIME_VIOLATION_COUNT), "invalid");
}

#if NPH_ASSERTS_ENABLED

typedef struct RealtimeLog {
    u32 count;
    u32 by_kind[NPH_REALTIME_VIOLATION_COUNT];
    const char *last_what;
    NphCallsite last_site;
} RealtimeLog;

static void record_violation(NphRealtimeViolation kind, const char *what, NphCallsite site,
                             void *user)
{
    RealtimeLog *log = (RealtimeLog *)user;
    log->count += 1U;
    if ((u32)kind < NPH_REALTIME_VIOLATION_COUNT) {
        log->by_kind[kind] += 1U;
    }
    log->last_what = what;
    log->last_site = site;
}

typedef struct RealtimeFixture {
    RealtimeLog log;
    NphRealtimeHook previous;
    void *previous_user;
} RealtimeFixture;

NTEST_SETUP(RealtimeFixture)
{
    nph_realtime_set_hook(record_violation, &f->log, &f->previous, &f->previous_user);
}

NTEST_TEARDOWN(RealtimeFixture)
{
    // A failed assertion inside a scope must not leave the runner's thread
    // in one.
    while (nph_realtime_active()) {
        nph_realtime_exit();
    }
    nph_realtime_set_hook(f->previous, f->previous_user, NULL, NULL);
}

NTEST_F(RealtimeFixture, allocations_outside_a_scope_are_fine)
{
    NphAllocator *sys = nph_allocator_system();
    void *p = NPH_ALLOC(sys, 64U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    p = NPH_REALLOC(sys, p, 64U, 128U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    NPH_FREE(sys, p, 128U, 16U);
    NTEST_ASSERT_FALSE(nph_realtime_active());
    NTEST_ASSERT_EQ_UINT(f->log.count, 0U);
}

NTEST_F(RealtimeFixture, allocations_inside_a_scope_are_reported_with_their_callsite)
{
    NphAllocator *sys = nph_allocator_system();
    const u64 before = nph_realtime_violations();
    nph_realtime_enter();
    NTEST_ASSERT_TRUE(nph_realtime_active());
    const u32 line = (u32)__LINE__ + 1U;
    void *p = NPH_ALLOC(sys, 32U, 16U);
    nph_realtime_exit();
    NTEST_ASSERT_NOT_NULL(p); // reported, not refused: the caller still gets its memory
    NTEST_ASSERT_EQ_UINT(f->log.count, 1U);
    NTEST_ASSERT_EQ_UINT(f->log.by_kind[NPH_REALTIME_VIOLATION_ALLOC], 1U);
    NTEST_ASSERT_EQ_STR(f->log.last_what, "nph_alloc");
    NTEST_ASSERT_EQ_UINT(f->log.last_site.line, line);
    NTEST_ASSERT_NOT_NULL(strstr(f->log.last_site.file, "test_realtime.c"));
    NTEST_ASSERT_EQ_UINT(nph_realtime_violations() - before, 1U);

    nph_realtime_enter();
    p = NPH_REALLOC(sys, p, 32U, 64U, 16U);
    NPH_FREE(sys, p, 64U, 16U);
    void *z = NPH_ALLOC_ZEROED(sys, 16U, 16U);
    NPH_FREE(sys, z, 16U, 16U);
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(f->log.by_kind[NPH_REALTIME_VIOLATION_ALLOC], 5U);
    NTEST_ASSERT_EQ_STR(f->log.last_what, "nph_free");
}

NTEST_F(RealtimeFixture, freeing_null_is_not_a_violation)
{
    nph_realtime_enter();
    NPH_FREE(nph_allocator_system(), NULL, 16U, 16U);
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(f->log.count, 0U);
}

NTEST_F(RealtimeFixture, scopes_nest)
{
    NTEST_UNUSED(f);
    nph_realtime_enter();
    nph_realtime_enter();
    nph_realtime_exit();
    NTEST_ASSERT_TRUE(nph_realtime_active());
    nph_realtime_exit();
    NTEST_ASSERT_FALSE(nph_realtime_active());
}

NTEST_F(RealtimeFixture, spin_locks_inside_a_scope_are_reported)
{
    NphSpinLock lock = NPH_SPIN_LOCK_INIT;
    nph_realtime_enter();
    nph_spin_lock_acquire(&lock);
    nph_spin_lock_release(&lock);
    // A try costs no wait: it is allowed.
    NTEST_ASSERT_TRUE(nph_spin_lock_try_acquire(&lock));
    nph_spin_lock_release(&lock);
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(f->log.count, 1U);
    NTEST_ASSERT_EQ_UINT(f->log.by_kind[NPH_REALTIME_VIOLATION_LOCK], 1U);
    NTEST_ASSERT_EQ_STR(f->log.last_what, "nph_spin_lock_acquire");
}

NTEST_F(RealtimeFixture, checks_outside_a_scope_report_nothing)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "test");
    NTEST_ASSERT_EQ_UINT(f->log.count, 0U);
    nph_realtime_enter();
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "test");
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(f->log.by_kind[NPH_REALTIME_VIOLATION_IO], 1U);
}

NTEST(realtime, without_a_hook_a_violation_fails_an_assertion)
{
    NphRealtimeHook previous = NULL;
    void *previous_user = NULL;
    nph_realtime_set_hook(NULL, NULL, &previous, &previous_user);
    void *p = NULL;
    nph_realtime_enter();
    NPH_TEST_EXPECT_ASSERT(p = NPH_ALLOC(nph_allocator_system(), 8U, 16U));
    nph_realtime_exit();
    NPH_FREE(nph_allocator_system(), p, 8U, 16U);
    nph_realtime_set_hook(previous, previous_user, NULL, NULL);
}

NTEST(realtime, unbalanced_exit_asserts_and_stays_outside)
{
    NPH_TEST_EXPECT_ASSERT(nph_realtime_exit());
    NTEST_ASSERT_FALSE(nph_realtime_active());
}

#else

NTEST(realtime, scopes_compile_to_nothing_without_asserts)
{
    nph_realtime_enter();
    NTEST_ASSERT_FALSE(nph_realtime_active());
    void *p = NPH_ALLOC(nph_allocator_system(), 8U, 16U);
    NTEST_ASSERT_NOT_NULL(p);
    NPH_FREE(nph_allocator_system(), p, 8U, 16U);
    nph_realtime_exit();
    NTEST_ASSERT_EQ_UINT(nph_realtime_violations(), 0U);
}

#endif
