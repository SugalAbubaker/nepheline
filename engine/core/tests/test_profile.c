// SPDX-License-Identifier: Apache-2.0
// Single-threaded behaviour of the profiler's recording half: lifecycle,
// event order and contents, the drop count, thread names, retirement, and
// re-initialisation. Threads, fibers, and the collector are tested in
// engine/profile, which has platform threads.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/profile.h>

#include "test_support.h"

#include <string.h>

#if NPH_PROFILER_ENABLED

typedef struct ProfileFixture {
    NphDebugAllocator da;
    NphProfileEvent events[64];
} ProfileFixture;

NTEST_SETUP(ProfileFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    const NphProfileConfig config = {nph_debug_allocator_allocator(&f->da), 8U, 4U};
    NTEST_ASSERT_EQ_INT(nph_profile_init(&config), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_profile_is_initialized());
    NTEST_ASSERT_TRUE(nph_profile_is_enabled());
}

NTEST_TEARDOWN(ProfileFixture)
{
    nph_profile_shutdown();
    NTEST_ASSERT_FALSE(nph_profile_is_initialized());
    NTEST_ASSERT_FALSE(nph_profile_is_enabled());
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// Drains every slot into `out`; returns the total and the thread found.
static u32 drain_all(NphProfileEvent *out, u32 cap, NphProfileThreadInfo *last)
{
    u32 total = 0;
    for (u32 s = 0; s < nph_profile_thread_slots(); ++s) {
        NphProfileThreadInfo info;
        const u32 n = nph_profile_drain(s, out + total, cap - total, &info);
        if (info.id != 0U) {
            *last = info;
        }
        total += n;
    }
    return total;
}

NTEST_F(ProfileFixture, events_are_recorded_in_order_with_their_contents)
{
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("main"), NPH_RESULT_OK);
    const u64 before = nph_profile_ticks();
    nph_profile_frame(7U);
    nph_profile_begin("outer");
    nph_profile_fiber(3U);
    nph_profile_counter("entities", 12.5);
    nph_profile_end();
    const u64 after = nph_profile_ticks();

    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 5U);
    NTEST_ASSERT_EQ_UINT(info.id, 1U);
    NTEST_ASSERT_EQ_STR(info.name, "main");
    NTEST_ASSERT_FALSE(info.retired);
    NTEST_ASSERT_EQ_UINT(info.dropped, 0U);

    NTEST_ASSERT_EQ_UINT(f->events[0].type, NPH_PROFILE_EVENT_FRAME);
    NTEST_ASSERT_EQ_UINT(f->events[0].value, 7U);
    NTEST_ASSERT_EQ_UINT(f->events[1].type, NPH_PROFILE_EVENT_BEGIN);
    NTEST_ASSERT_EQ_STR(f->events[1].name, "outer");
    NTEST_ASSERT_EQ_UINT(f->events[2].type, NPH_PROFILE_EVENT_FIBER);
    NTEST_ASSERT_EQ_UINT(f->events[2].value, 3U);
    NTEST_ASSERT_EQ_UINT(f->events[3].type, NPH_PROFILE_EVENT_COUNTER);
    NTEST_ASSERT_EQ_STR(f->events[3].name, "entities");
    f64 value;
    memcpy(&value, &f->events[3].value, sizeof value);
    NTEST_ASSERT_NEAR_F64(value, 12.5, 0.0);
    NTEST_ASSERT_EQ_UINT(f->events[4].type, NPH_PROFILE_EVENT_END);
    NTEST_ASSERT_NULL(f->events[4].name);
    for (u32 i = 0; i < 5U; ++i) {
        NTEST_ASSERT_GE_UINT(f->events[i].ticks, before);
        NTEST_ASSERT_LE_UINT(f->events[i].ticks, after);
        NTEST_ASSERT_EQ_UINT(f->events[i].reserved, 0U);
        if (i > 0U) {
            NTEST_ASSERT_GE_UINT(f->events[i].ticks, f->events[i - 1U].ticks);
        }
    }
    // Drained events are gone.
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 0U);
}

NTEST_F(ProfileFixture, the_first_event_attaches_a_thread_with_a_default_name)
{
    nph_profile_begin("lazy");
    nph_profile_end();
    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 2U);
    NTEST_ASSERT_EQ_STR(info.name, "thread 1");
    // Attaching again renames the lane without a new ring.
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("renamed main thread with a long name"),
                        NPH_RESULT_OK);
    nph_profile_begin("x");
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 1U);
    NTEST_ASSERT_EQ_UINT(info.id, 1U);
    NTEST_ASSERT_EQ_UINT(strlen(info.name), NPH_PROFILE_THREAD_NAME_MAX - 1U);
    NTEST_ASSERT_EQ_INT(strncmp(info.name, "renamed main thread", 19U), 0);
}

NTEST_F(ProfileFixture, a_full_ring_drops_and_counts)
{
    // The fixture's rings hold 8 events.
    for (u32 i = 0; i < 11U; ++i) {
        nph_profile_frame(i);
    }
    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 8U);
    NTEST_ASSERT_EQ_UINT(info.dropped, 3U);
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_EQ_UINT(f->events[i].value, i); // the oldest are kept
    }
    // Room again: recording resumes, the drop count stays cumulative.
    nph_profile_frame(100U);
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 1U);
    NTEST_ASSERT_EQ_UINT(f->events[0].value, 100U);
    NTEST_ASSERT_EQ_UINT(info.dropped, 3U);
}

NTEST_F(ProfileFixture, drain_respects_its_capacity)
{
    for (u32 i = 0; i < 6U; ++i) {
        nph_profile_frame(i);
    }
    NphProfileThreadInfo info;
    u32 got = 0;
    for (u32 s = 0; s < nph_profile_thread_slots(); ++s) {
        got += nph_profile_drain(s, f->events, 4U, &info);
    }
    NTEST_ASSERT_EQ_UINT(got, 4U);
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 2U);
    NTEST_ASSERT_EQ_UINT(f->events[0].value, 4U);
    // Out-of-range and empty slots describe nothing.
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(1000U, f->events, 4U, &info), 0U);
    NTEST_ASSERT_EQ_UINT(info.id, 0U);
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(3U, f->events, 4U, &info), 0U);
    NTEST_ASSERT_EQ_UINT(info.id, 0U);
}

NTEST_F(ProfileFixture, a_detached_thread_is_freed_once_drained)
{
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("short-lived"), NPH_RESULT_OK);
    nph_profile_begin("last words");
    nph_profile_thread_detach();
    nph_profile_thread_detach(); // a second detach is a no-op

    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, f->events, 64U, &info), 1U);
    NTEST_ASSERT_TRUE(info.retired);
    NTEST_ASSERT_EQ_STR(f->events[0].name, "last words");
    // The slot was released: the next event attaches a new thread there.
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, f->events, 64U, &info), 0U);
    NTEST_ASSERT_EQ_UINT(info.id, 0U);
    nph_profile_begin("again");
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, f->events, 64U, &info), 1U);
    NTEST_ASSERT_EQ_UINT(info.id, 2U);
    NTEST_ASSERT_FALSE(info.retired);
}

NTEST_F(ProfileFixture, a_retired_thread_with_more_than_one_drain_of_events_waits)
{
    for (u32 i = 0; i < 6U; ++i) {
        nph_profile_frame(i);
    }
    nph_profile_thread_detach();
    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, f->events, 4U, &info), 4U);
    NTEST_ASSERT_TRUE(info.retired);
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, f->events, 4U, &info), 2U);
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, f->events, 4U, &info), 0U);
    NTEST_ASSERT_EQ_UINT(info.id, 0U);
}

NTEST_F(ProfileFixture, pausing_records_nothing)
{
    nph_profile_set_enabled(false);
    NTEST_ASSERT_FALSE(nph_profile_is_enabled());
    nph_profile_begin("hidden");
    nph_profile_end();
    const NphProfileEvent prepared = {42U, "prepared", 0U, NPH_PROFILE_EVENT_BEGIN, 0U};
    nph_profile_record(&prepared);
    NphProfileThreadInfo info;
    memset(&info, 0, sizeof info);
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 0U);
    NTEST_ASSERT_EQ_UINT(info.id, 0U); // not even attached
    nph_profile_set_enabled(true);
    nph_profile_record(&prepared);
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 1U);
    NTEST_ASSERT_EQ_UINT(f->events[0].ticks, 42U); // its own timestamp
    NTEST_ASSERT_EQ_STR(f->events[0].name, "prepared");
}

NTEST_F(ProfileFixture, thread_registry_full)
{
    // The fixture allows four threads; this test thread is one. Ids are
    // never reused, so a full registry can be shown with one thread by
    // retiring and re-attaching without draining.
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("t"), NPH_RESULT_OK);
        nph_profile_thread_detach();
    }
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("fifth"), NPH_RESULT_FULL);
    nph_profile_begin("dropped"); // no ring: silently not recorded
    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(drain_all(f->events, 64U, &info), 0U);
    // The drain released the retired slots.
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("fifth"), NPH_RESULT_OK);
}

NTEST(profile, init_validation_and_uninitialised_calls)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    NphAllocator *a = nph_debug_allocator_allocator(&da);
    const NphProfileConfig bad_ring = {a, 12U, 4U};
    NTEST_ASSERT_EQ_INT(nph_profile_init(&bad_ring), NPH_RESULT_INVALID_ARGUMENT);
    const NphProfileConfig one = {a, 1U, 4U};
    NTEST_ASSERT_EQ_INT(nph_profile_init(&one), NPH_RESULT_INVALID_ARGUMENT);
    const NphProfileConfig too_many = {a, 8U, NPH_PROFILE_MAX_THREADS + 1U};
    NTEST_ASSERT_EQ_INT(nph_profile_init(&too_many), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_FALSE(nph_profile_is_initialized());

    // Before init: nothing records, nothing allocates.
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("x"), NPH_RESULT_INVALID_STATE);
    nph_profile_begin("nothing");
    nph_profile_end();
    nph_profile_frame(1U);
    nph_profile_counter("c", 1.0);
    nph_profile_set_enabled(true); // ignored before init
    NTEST_ASSERT_FALSE(nph_profile_is_enabled());
    NTEST_ASSERT_EQ_UINT(nph_profile_thread_slots(), 0U);
    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, NULL, 0U, &info), 0U);
    nph_profile_thread_detach();
    nph_profile_shutdown(); // safe without init

    const NphProfileConfig defaults = {a, 0U, 0U};
    NTEST_ASSERT_EQ_INT(nph_profile_init(&defaults), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_profile_init(&defaults), NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_UINT(nph_profile_thread_slots(), 64U);
    nph_profile_shutdown();
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
}

NTEST(profile, a_thread_from_an_earlier_init_attaches_again)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    const NphProfileConfig config = {nph_debug_allocator_allocator(&da), 8U, 4U};
    NTEST_ASSERT_EQ_INT(nph_profile_init(&config), NPH_RESULT_OK);
    nph_profile_begin("first life");
    nph_profile_shutdown(); // frees this thread's ring

    NTEST_ASSERT_EQ_INT(nph_profile_init(&config), NPH_RESULT_OK);
    nph_profile_begin("second life"); // must not touch the freed ring
    NphProfileEvent e[4];
    memset(e, 0, sizeof e);
    NphProfileThreadInfo info;
    u32 n = 0;
    for (u32 s = 0; s < nph_profile_thread_slots(); ++s) {
        n += nph_profile_drain(s, e + n, 4U - n, &info);
    }
    NTEST_ASSERT_EQ_UINT(n, 1U);
    NTEST_ASSERT_EQ_STR(e[0].name, "second life");
    nph_profile_shutdown();
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
}

#else // !NPH_PROFILER_ENABLED

NTEST(profile, compiled_out_in_this_configuration)
{
    NphProfileConfig config;
    memset(&config, 0, sizeof config);
    config.alloc = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(nph_profile_init(&config), NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_FALSE(nph_profile_is_initialized());
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("x"), NPH_RESULT_INVALID_STATE);
    nph_profile_set_enabled(true);
    NTEST_ASSERT_FALSE(nph_profile_is_enabled());
    NPH_PROFILE_BEGIN("compiled out");
    NPH_PROFILE_END();
    NPH_PROFILE_FRAME(1U);
    NPH_PROFILE_FIBER(2U);
    NPH_PROFILE_COUNTER("c", 3);
    nph_profile_begin("x");
    nph_profile_end();
    nph_profile_frame(1U);
    nph_profile_fiber(2U);
    nph_profile_counter("c", 3.0);
    const NphProfileEvent e = {1U, "e", 0U, NPH_PROFILE_EVENT_BEGIN, 0U};
    nph_profile_record(&e);
    nph_profile_thread_detach();
    NTEST_ASSERT_EQ_UINT(nph_profile_thread_slots(), 0U);
    NphProfileThreadInfo info;
    NTEST_ASSERT_EQ_UINT(nph_profile_drain(0U, NULL, 0U, &info), 0U);
    NTEST_ASSERT_EQ_UINT(info.id, 0U);
    nph_profile_shutdown();
    NTEST_ASSERT_GT_UINT(nph_profile_ticks(), 0U);
}

#endif

NTEST(profile, ticks_advance)
{
    const u64 a = nph_profile_ticks();
    volatile u64 sink = 0;
    for (u32 i = 0; i < 10000U; ++i) {
        sink += i;
    }
    NPH_UNUSED(sink);
    const u64 b = nph_profile_ticks();
    NTEST_ASSERT_GT_UINT(b, a);
}
