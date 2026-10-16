// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the profile tests: a fixture with the recording half
// initialised and a collector whose ticks are nanoseconds (tick_hz 1e9),
// and scripted event streams recorded by threads of their own, so that
// fiber migrations between lanes can be staged with exact timestamps.

#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/profile.h>
#include <nepheline/platform/thread.h>
#include <nepheline/profile/profiler.h>

#include <ntest/ntest.h>

#include <string.h>

#define TEST_TICK_HZ 1000000000ULL

// An allocator that serves `budget` allocations from `inner` and then
// fails, for walking every failure point of a creation.
typedef struct FailingAllocator {
    NphAllocator allocator;
    NphAllocator *inner;
    u32 budget;
} FailingAllocator;

static inline void *failing_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    FailingAllocator *f = (FailingAllocator *)self;
    if (f->budget == 0U) {
        return NULL;
    }
    f->budget -= 1U;
    return nph_alloc_at(f->inner, size, align, site);
}

static inline void *failing_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size,
                                    u32 align, NphCallsite site)
{
    FailingAllocator *f = (FailingAllocator *)self;
    if (f->budget == 0U) {
        return NULL;
    }
    f->budget -= 1U;
    return nph_realloc_at(f->inner, ptr, old_size, new_size, align, site);
}

static inline void failing_free(NphAllocator *self, void *ptr, u64 size, u32 align,
                                NphCallsite site)
{
    nph_free_at(((FailingAllocator *)self)->inner, ptr, size, align, site);
}

static const NphAllocatorVtable g_failing_vtable = {failing_alloc, failing_realloc, failing_free};

static inline void failing_allocator_init(FailingAllocator *f, NphAllocator *inner, u32 budget)
{
    f->allocator.vtable = &g_failing_vtable;
    f->allocator.name = "failing";
    f->inner = inner;
    f->budget = budget;
}

static inline NphProfileEvent ev(u32 type, const char *name, u64 value, u64 ticks)
{
    NphProfileEvent e;
    e.ticks = ticks;
    e.name = name;
    e.value = value;
    e.type = type;
    e.reserved = 0U;
    return e;
}

#define EV_BEGIN(name, t)  ev(NPH_PROFILE_EVENT_BEGIN, (name), 0U, (t))
#define EV_END(t)          ev(NPH_PROFILE_EVENT_END, NULL, 0U, (t))
#define EV_FRAME(index, t) ev(NPH_PROFILE_EVENT_FRAME, NULL, (index), (t))
#define EV_FIBER(fiber, t) ev(NPH_PROFILE_EVENT_FIBER, NULL, (fiber), (t))

static inline void rec(NphProfileEvent e)
{
    nph_profile_record(&e);
}

static inline void rec_counter(const char *name, f64 value, u64 ticks)
{
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    rec(ev(NPH_PROFILE_EVENT_COUNTER, name, bits, ticks));
}

// Records `events` on a thread of its own named `name`, which then
// detaches. (ntest's fatal assertions belong to the test thread, so the
// thread only reports.)
typedef struct ScriptThread {
    const char *name;
    const NphProfileEvent *events;
    u32 count;
    NphResult attach;
} ScriptThread;

static inline void script_thread_main(void *user)
{
    ScriptThread *s = (ScriptThread *)user;
    s->attach = nph_profile_thread_attach(s->name);
    for (u32 i = 0; i < s->count; ++i) {
        nph_profile_record(&s->events[i]);
    }
    nph_profile_thread_detach();
}

static inline void run_script_thread(const char *name, const NphProfileEvent *events, u32 count)
{
    ScriptThread s = {name, events, count, NPH_RESULT_UNKNOWN};
    NphThreadDesc td;
    memset(&td, 0, sizeof td);
    td.fn = script_thread_main;
    td.user = &s;
    td.name = name;
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &td), NPH_RESULT_OK);
    nph_thread_join(&t);
    NTEST_ASSERT_EQ_INT(s.attach, NPH_RESULT_OK);
}

typedef struct ProfilerFixture {
    NphDebugAllocator da;
    NphProfiler *p;
} ProfilerFixture;

static inline NphProfiler *test_profiler(NphAllocator *a, const NphProfilerDesc *override)
{
    NphProfilerDesc d;
    if (override != NULL) {
        d = *override;
    } else {
        memset(&d, 0, sizeof d);
    }
    d.alloc = a;
    if (d.tick_hz == 0U) {
        d.tick_hz = TEST_TICK_HZ;
    }
    NphProfiler *p = NULL;
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &p), NPH_RESULT_OK);
    return p;
}

static inline void profiler_fixture_setup(ProfilerFixture *f, u32 ring, const NphProfilerDesc *d)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    NphProfileConfig config;
    memset(&config, 0, sizeof config);
    config.alloc = nph_debug_allocator_allocator(&f->da);
    config.ring_capacity = ring;
    config.max_threads = 16U;
    NTEST_ASSERT_EQ_INT(nph_profile_init(&config), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("main"), NPH_RESULT_OK);
    f->p = test_profiler(nph_debug_allocator_allocator(&f->da), d);
}

static inline void profiler_fixture_teardown(ProfilerFixture *f)
{
    nph_profiler_destroy(f->p);
    f->p = NULL;
    nph_profile_shutdown();
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// The lane named `name`, or NPH_U32_MAX.
static inline u32 lane_named(const NphProfiler *p, const char *name)
{
    for (u32 i = 0; i < nph_profiler_lane_count(p); ++i) {
        if (strcmp(nph_profiler_lane_name(p, i), name) == 0) {
            return i;
        }
    }
    return NPH_U32_MAX;
}

// The index in `segs` of the first segment named `name` on `lane`
// (NPH_U32_MAX for any lane) starting at `start`, or NPH_U32_MAX.
static inline u32 find_segment(const NphProfiler *p, const NphProfileSegment *segs, u32 count,
                               const char *name, u32 lane, u64 start)
{
    for (u32 i = 0; i < count; ++i) {
        if (strcmp(nph_profiler_name(p, segs[i].name), name) == 0 &&
            (lane == NPH_U32_MAX || segs[i].lane == lane) && segs[i].start == start) {
            return i;
        }
    }
    return NPH_U32_MAX;
}
