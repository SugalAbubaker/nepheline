// SPDX-License-Identifier: Apache-2.0
// Profiler benchmarks (docs/profile.md).
//
//   profile_zone_enabled        an NPH_PROFILE_BEGIN/END pair into the thread
//                               ring (drained outside the timing every 4096)
//   profile_zone_paused         the same pair while recording is paused
//   profile_zone_uninitialised  the same pair before nph_profile_init
//   profile_collect_per_event   the collector's cost per event: drain, merge,
//                               rebuild, statistics (zones of depth 3)
//   tracked_alloc_free_64       a 64-byte allocation and free through a
//                               tracked allocator over the system allocator
//   system_alloc_free_64        the same without the tracking, for reference

#include <nepheline/core/profile.h>
#include <nepheline/profile/memory.h>
#include <nepheline/profile/profiler.h>

#include <nbench/nbench.h>

#include <stdlib.h>
#include <string.h>

#if NPH_PROFILER_ENABLED

static void profile_setup(u32 ring)
{
    NphProfileConfig config;
    memset(&config, 0, sizeof config);
    config.alloc = nph_allocator_system();
    config.ring_capacity = ring;
    config.max_threads = 8U;
    if (nph_profile_init(&config) != NPH_RESULT_OK ||
        nph_profile_thread_attach("bench") != NPH_RESULT_OK) {
        abort();
    }
}

static void drain_everything(void)
{
    NphProfileEvent events[256];
    for (u32 s = 0; s < nph_profile_thread_slots(); ++s) {
        NphProfileThreadInfo info;
        while (nph_profile_drain(s, events, 256U, &info) == 256U) {
        }
    }
}

NBENCH(profile_zone_enabled)
{
    profile_setup(8192U);
    nbench_set_items_per_iteration(state, 4096U);
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < 4096U; ++i) {
            NPH_PROFILE_BEGIN("bench.zone");
            NPH_PROFILE_END();
        }
        nbench_pause_timing(state);
        drain_everything();
        nbench_resume_timing(state);
    }
    nph_profile_shutdown();
}

NBENCH(profile_zone_paused)
{
    profile_setup(8192U);
    nph_profile_set_enabled(false);
    nbench_set_items_per_iteration(state, 4096U);
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < 4096U; ++i) {
            NPH_PROFILE_BEGIN("bench.zone");
            NPH_PROFILE_END();
        }
    }
    nph_profile_shutdown();
}

NBENCH(profile_zone_uninitialised)
{
    nbench_set_items_per_iteration(state, 4096U);
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < 4096U; ++i) {
            NPH_PROFILE_BEGIN("bench.zone");
            NPH_PROFILE_END();
        }
    }
}

NBENCH(profile_collect_per_event)
{
    profile_setup(65536U);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.settle_ns = 1U; // everything recorded before the collection settles
    NphProfiler *p = NULL;
    if (nph_profiler_create(&d, &p) != NPH_RESULT_OK) {
        abort();
    }
    static const char *const names[3] = {"bench.outer", "bench.middle", "bench.inner"};
    u64 frame = 0U;
    // One frame of 2048 nested zones (12290 events) per iteration.
    nbench_set_items_per_iteration(state, 2048U * 6U + 2U);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        nph_profile_frame(frame++);
        for (u32 i = 0; i < 2048U; ++i) {
            nph_profile_begin(names[0]);
            nph_profile_begin(names[1]);
            nph_profile_begin(names[2]);
            nph_profile_end();
            nph_profile_end();
            nph_profile_end();
        }
        nph_profile_frame(frame);
        nbench_resume_timing(state);
        nbench_do_not_optimize_u64(nph_profiler_collect(p));
    }
    nph_profiler_destroy(p);
    nph_profile_shutdown();
}

#endif

NBENCH(tracked_alloc_free_64)
{
    NphTrackedAllocator t;
    nph_tracked_allocator_init(&t, nph_allocator_system(), "bench");
    NphAllocator *a = nph_tracked_allocator(&t);
    while (nbench_keep_running(state)) {
        void *p = NPH_ALLOC(a, 64U, 16U);
        nbench_do_not_optimize(p);
        NPH_FREE(a, p, 64U, 16U);
    }
}

NBENCH(system_alloc_free_64)
{
    NphAllocator *a = nph_allocator_system();
    while (nbench_keep_running(state)) {
        void *p = NPH_ALLOC(a, 64U, 16U);
        nbench_do_not_optimize(p);
        NPH_FREE(a, p, 64U, 16U);
    }
}
