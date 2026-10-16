// SPDX-License-Identifier: Apache-2.0
// The job system's instrumentation through the collector: a zone per job,
// a lane per worker, and jobs that park on counters and resume on other
// workers drawn as the pieces each worker ran.
#include <nepheline/jobs/jobs.h>

#include "test_profile_support.h"

#include <stdlib.h>

#if NPH_PROFILER_ENABLED

#define PARENTS        64U
#define CHILDREN       4U
#define TOTAL_SEGMENTS 4096U

typedef struct JobsCtx {
    NphJobSystem *js;
    NphAtomicU32 children_done;
    NphAtomicU32 failures;
} JobsCtx;

static void child_job(void *user)
{
    JobsCtx *ctx = (JobsCtx *)user;
    volatile u32 sink = 0;
    for (u32 i = 0; i < 2000U; ++i) {
        sink += i;
    }
    NPH_UNUSED(sink);
    nph_atomic_fetch_add_u32(&ctx->children_done, 1U, NPH_MEMORY_ORDER_RELAXED);
}

static void parent_job(void *user)
{
    JobsCtx *ctx = (JobsCtx *)user;
    NphJobDecl decls[CHILDREN];
    for (u32 i = 0; i < CHILDREN; ++i) {
        decls[i].fn = child_job;
        decls[i].user = ctx;
        decls[i].name = "test.child";
    }
    NphJobCounter counter = NPH_JOB_COUNTER_INIT;
    if (nph_job_run(ctx->js, decls, CHILDREN, NPH_JOB_PRIORITY_NORMAL, &counter) != NPH_RESULT_OK) {
        nph_atomic_fetch_add_u32(&ctx->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
        return;
    }
    // Parks this fiber; it may resume on another worker.
    nph_job_wait(ctx->js, &counter);
}

static int compare_lane_depth_start(const void *a, const void *b)
{
    const NphProfileSegment *x = (const NphProfileSegment *)a;
    const NphProfileSegment *y = (const NphProfileSegment *)b;
    if (x->lane != y->lane) {
        return x->lane < y->lane ? -1 : 1;
    }
    if (x->depth != y->depth) {
        return x->depth < y->depth ? -1 : 1;
    }
    return x->start < y->start ? -1 : (x->start > y->start ? 1 : 0);
}

NTEST(profiler_jobs, jobs_that_park_and_migrate_are_drawn_consistently)
{
    static ProfilerFixture fx; // see test_profiler.c
    memset(&fx, 0, sizeof fx);
    nph_debug_allocator_init(&fx.da, nph_allocator_system());
    NphProfileConfig config;
    memset(&config, 0, sizeof config);
    config.alloc = nph_debug_allocator_allocator(&fx.da);
    config.ring_capacity = 16384U;
    config.max_threads = 32U;
    NTEST_ASSERT_EQ_INT(nph_profile_init(&config), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("main"), NPH_RESULT_OK);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&fx.da);
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &fx.p), NPH_RESULT_OK);

    JobsCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = nph_allocator_system();
    jd.worker_count = 4U;
    jd.fiber_count = 96U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &ctx.js), NPH_RESULT_OK);

    NPH_PROFILE_FRAME(1U);
    NphJobDecl parents[PARENTS];
    for (u32 i = 0; i < PARENTS; ++i) {
        parents[i].fn = parent_job;
        parents[i].user = &ctx;
        parents[i].name = "test.parent";
    }
    NphJobCounter all = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(ctx.js, parents, PARENTS, NPH_JOB_PRIORITY_NORMAL, &all),
                        NPH_RESULT_OK);
    nph_job_wait(ctx.js, &all);
    nph_job_system_destroy(ctx.js); // the workers detach from the profiler
    NPH_PROFILE_FRAME(2U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&ctx.failures, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&ctx.children_done, NPH_MEMORY_ORDER_RELAXED),
                         PARENTS * CHILDREN);

    nph_profiler_forget_names(fx.p); // process everything
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(fx.p), 1U);
    NphProfilerStats st;
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 0U);
    NTEST_ASSERT_EQ_UINT(st.dropped_events, 0U);
    NTEST_ASSERT_GE_UINT(st.lanes, 5U); // main + four workers (+ the I/O thread)

    NphProfileSegment *segs = (NphProfileSegment *)NPH_ALLOC(
        nph_allocator_system(), (u64)TOTAL_SEGMENTS * sizeof(NphProfileSegment),
        NPH_ALIGNOF(NphProfileSegment));
    NTEST_ASSERT_NOT_NULL(segs);
    const u32 n = nph_profiler_frame_segments(fx.p, 0U, segs, TOTAL_SEGMENTS);
    NTEST_ASSERT_LT_UINT(n, TOTAL_SEGMENTS);
    const u32 parent_id = nph_profiler_find_name(fx.p, NPH_STR("test.parent"));
    const u32 child_id = nph_profiler_find_name(fx.p, NPH_STR("test.child"));
    NTEST_ASSERT_NE_UINT(parent_id, NPH_PROFILER_NO_NAME);
    NTEST_ASSERT_NE_UINT(child_id, NPH_PROFILER_NO_NAME);
    u32 parents_begun = 0;
    u32 parents_ended = 0;
    u32 children = 0;
    for (u32 i = 0; i < n; ++i) {
        const NphProfileSegment *s = &segs[i];
        NTEST_ASSERT_LE_UINT(s->start, s->end);
        if (s->name == parent_id) {
            NTEST_ASSERT_EQ_UINT(s->depth, 0U);
            parents_begun += (s->flags & NPH_PROFILE_SEGMENT_CONTINUED) == 0U ? 1U : 0U;
            parents_ended += (s->flags & NPH_PROFILE_SEGMENT_CONTINUES) == 0U ? 1U : 0U;
        } else if (s->name == child_id) {
            // Children run on their own fibers, or inline under a waiting
            // parent when no fiber is free; either way, whole.
            NTEST_ASSERT_EQ_UINT(s->flags, 0U);
            children += 1U;
        }
    }
    NTEST_ASSERT_EQ_UINT(parents_begun, PARENTS);
    NTEST_ASSERT_EQ_UINT(parents_ended, PARENTS);
    NTEST_ASSERT_EQ_UINT(children, PARENTS * CHILDREN);

    // On one lane, segments of one depth never overlap: each is work that
    // thread did, one piece at a time.
    qsort(segs, n, sizeof *segs, compare_lane_depth_start);
    for (u32 i = 1; i < n; ++i) {
        if (segs[i].lane == segs[i - 1U].lane && segs[i].depth == segs[i - 1U].depth) {
            NTEST_ASSERT_LE_UINT(segs[i - 1U].end, segs[i].start);
        }
    }
    NPH_FREE(nph_allocator_system(), segs, (u64)TOTAL_SEGMENTS * sizeof(NphProfileSegment),
             NPH_ALIGNOF(NphProfileSegment));
    profiler_fixture_teardown(&fx);
}

#else

NTEST(profiler_jobs, compiled_out)
{
    NTEST_SKIP("the profiler is compiled out of this configuration");
}

#endif
