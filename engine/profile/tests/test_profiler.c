// SPDX-License-Identifier: Apache-2.0
// The collector: zones into segments, frame cuts, fiber migration, repairs
// of broken streams, interning, statistics, the history, and real threads.
// Synthetic streams carry explicit timestamps (ticks are nanoseconds here),
// so every expected segment is exact.
#include "profile_internal.h"
#include "test_profile_support.h"

#if NPH_PROFILER_ENABLED

NTEST_SETUP(ProfilerFixture)
{
    profiler_fixture_setup(f, 4096U, NULL);
}

NTEST_TEARDOWN(ProfilerFixture)
{
    profiler_fixture_teardown(f);
}

NTEST_F(ProfilerFixture, nested_zones_become_segments_inside_frames)
{
    rec(EV_FRAME(1U, 100U));
    rec(EV_BEGIN("outer", 110U));
    rec(EV_BEGIN("inner", 120U));
    rec(EV_END(130U));
    rec(EV_END(140U));
    rec(EV_FRAME(2U, 200U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(f->p), 1U);

    NphProfilerFrame frame;
    NTEST_ASSERT_TRUE(nph_profiler_frame(f->p, 0U, &frame));
    NTEST_ASSERT_EQ_UINT(frame.index, 1U);
    NTEST_ASSERT_EQ_UINT(frame.start, 100U);
    NTEST_ASSERT_EQ_UINT(frame.end, 200U);
    NTEST_ASSERT_NEAR_F64(frame.duration_ms, 1e-4, 1e-12);
    NTEST_ASSERT_EQ_UINT(frame.segment_count, 2U);
    NTEST_ASSERT_EQ_UINT(frame.dropped_events, 0U);

    NphProfileSegment segs[8];
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(f->p, 0U, segs, 8U), 2U);
    // In the order they ended: the inner zone first.
    NTEST_ASSERT_EQ_STR(nph_profiler_name(f->p, segs[0].name), "inner");
    NTEST_ASSERT_EQ_UINT(segs[0].start, 120U);
    NTEST_ASSERT_EQ_UINT(segs[0].end, 130U);
    NTEST_ASSERT_EQ_UINT(segs[0].depth, 1U);
    NTEST_ASSERT_EQ_UINT(segs[0].flags, 0U);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(f->p, segs[1].name), "outer");
    NTEST_ASSERT_EQ_UINT(segs[1].start, 110U);
    NTEST_ASSERT_EQ_UINT(segs[1].end, 140U);
    NTEST_ASSERT_EQ_UINT(segs[1].depth, 0U);
    NTEST_ASSERT_EQ_STR(nph_profiler_lane_name(f->p, segs[0].lane), "main");

    NphProfilerStats st;
    nph_profiler_stats(f->p, &st);
    NTEST_ASSERT_EQ_UINT(st.frames, 1U);
    NTEST_ASSERT_GE_UINT(st.events, 6U);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 0U);
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    NTEST_ASSERT_GE_UINT(st.lanes, 1U);
    NTEST_ASSERT_EQ_UINT(nph_profiler_tick_hz(f->p), TEST_TICK_HZ);
    NTEST_ASSERT_NEAR_F64(nph_profiler_ticks_to_ms(f->p, 1000000U), 1.0, 1e-12);
}

NTEST_F(ProfilerFixture, zones_running_at_a_frame_boundary_are_cut)
{
    rec(EV_FRAME(1U, 100U));
    rec(EV_BEGIN("long", 150U));
    rec(EV_FRAME(2U, 200U));
    rec(EV_END(250U));
    rec(EV_FRAME(3U, 300U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 2U);
    NphProfileSegment segs[4];
    // Age 1 is frame 1, age 0 frame 2.
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(f->p, 1U, segs, 4U), 1U);
    NTEST_ASSERT_EQ_UINT(segs[0].start, 150U);
    NTEST_ASSERT_EQ_UINT(segs[0].end, 200U);
    NTEST_ASSERT_EQ_UINT(segs[0].flags, NPH_PROFILE_SEGMENT_CONTINUES);
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(f->p, 0U, segs, 4U), 1U);
    NTEST_ASSERT_EQ_UINT(segs[0].start, 200U);
    NTEST_ASSERT_EQ_UINT(segs[0].end, 250U);
    NTEST_ASSERT_EQ_UINT(segs[0].flags, NPH_PROFILE_SEGMENT_CONTINUED);

    // One call in frame 1, a continuation (no call) in frame 2.
    NphProfilerZoneStats z[8];
    const u32 n = nph_profiler_zone_stats(f->p, z, 8U);
    u32 found = NPH_U32_MAX;
    for (u32 i = 0; i < n; ++i) {
        if (strcmp(z[i].name, "long") == 0) {
            found = i;
        }
    }
    NTEST_ASSERT_NE_UINT(found, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(z[found].frames, 2U);
    NTEST_ASSERT_NEAR_F64(z[found].calls_per_frame, 0.5, 1e-12);
    // Per-frame totals are kept as floats: compare relative to the value.
    NTEST_ASSERT_NEAR_F64(z[found].mean_ms, 50.0 * 1e-6, 1e-12 * 50.0);
    NTEST_ASSERT_NEAR_F64(z[found].last_ms, 50.0 * 1e-6, 1e-12 * 50.0);
}

NTEST_F(ProfilerFixture, a_fiber_that_migrates_is_drawn_on_both_threads)
{
    rec(EV_FRAME(1U, 50U));
    // Thread A starts a job on fiber 5, which parks at 150.
    const NphProfileEvent a[] = {EV_FIBER(5U, 100U), EV_BEGIN("job", 110U),
                                 EV_FIBER(NPH_PROFILE_NO_FIBER, 150U)};
    run_script_thread("worker a", a, 3U);
    // Thread B resumes fiber 5 at 200; the job finishes there.
    const NphProfileEvent b[] = {EV_FIBER(5U, 200U), EV_BEGIN("child", 210U), EV_END(220U),
                                 EV_END(250U), EV_FIBER(NPH_PROFILE_NO_FIBER, 260U)};
    run_script_thread("worker b", b, 5U);
    rec(EV_FRAME(2U, 1000U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);

    const u32 la = lane_named(f->p, "worker a");
    const u32 lb = lane_named(f->p, "worker b");
    NTEST_ASSERT_NE_UINT(la, NPH_U32_MAX);
    NTEST_ASSERT_NE_UINT(lb, NPH_U32_MAX);
    NphProfileSegment segs[8];
    const u32 n = nph_profiler_frame_segments(f->p, 0U, segs, 8U);
    NTEST_ASSERT_EQ_UINT(n, 3U);
    const u32 on_a = find_segment(f->p, segs, n, "job", la, 110U);
    NTEST_ASSERT_NE_UINT(on_a, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[on_a].end, 150U);
    NTEST_ASSERT_EQ_UINT(segs[on_a].flags, NPH_PROFILE_SEGMENT_CONTINUES);
    const u32 on_b = find_segment(f->p, segs, n, "job", lb, 200U);
    NTEST_ASSERT_NE_UINT(on_b, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[on_b].end, 250U);
    NTEST_ASSERT_EQ_UINT(segs[on_b].flags, NPH_PROFILE_SEGMENT_CONTINUED);
    const u32 child = find_segment(f->p, segs, n, "child", lb, 210U);
    NTEST_ASSERT_NE_UINT(child, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[child].depth, 1U);
    NTEST_ASSERT_EQ_UINT(segs[child].end, 220U);

    NphProfilerStats st;
    nph_profiler_stats(f->p, &st);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 0U);
}

NTEST_F(ProfilerFixture, a_thread_stack_is_suspended_while_its_thread_runs_a_fiber)
{
    rec(EV_FRAME(1U, 0U));
    const NphProfileEvent t[] = {EV_BEGIN("thread zone", 10U),        EV_FIBER(2U, 20U),
                                 EV_BEGIN("fiber zone", 30U),         EV_END(40U),
                                 EV_FIBER(NPH_PROFILE_NO_FIBER, 50U), EV_END(60U)};
    run_script_thread("t", t, 6U);
    rec(EV_FRAME(2U, 100U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    NphProfileSegment segs[8];
    const u32 n = nph_profiler_frame_segments(f->p, 0U, segs, 8U);
    NTEST_ASSERT_EQ_UINT(n, 3U);
    const u32 first = find_segment(f->p, segs, n, "thread zone", NPH_U32_MAX, 10U);
    NTEST_ASSERT_NE_UINT(first, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[first].end, 20U);
    NTEST_ASSERT_EQ_UINT(segs[first].flags, NPH_PROFILE_SEGMENT_CONTINUES);
    const u32 second = find_segment(f->p, segs, n, "thread zone", NPH_U32_MAX, 50U);
    NTEST_ASSERT_NE_UINT(second, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[second].end, 60U);
    NTEST_ASSERT_EQ_UINT(segs[second].flags, NPH_PROFILE_SEGMENT_CONTINUED);
    const u32 fz = find_segment(f->p, segs, n, "fiber zone", NPH_U32_MAX, 30U);
    NTEST_ASSERT_NE_UINT(fz, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[fz].depth, 0U); // a fiber's stack starts empty
}

NTEST_F(ProfilerFixture, a_fiber_seen_on_two_lanes_is_repaired)
{
    rec(EV_FRAME(1U, 0U));
    // Thread A runs fiber 3 and never reports leaving it (a lost event).
    const NphProfileEvent a[] = {EV_FIBER(3U, 10U), EV_BEGIN("stuck", 20U)};
    run_script_thread("a", a, 2U);
    const NphProfileEvent b[] = {EV_FIBER(3U, 40U), EV_END(50U)};
    run_script_thread("b", b, 2U);
    rec(EV_FRAME(2U, 100U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    NphProfilerStats st;
    nph_profiler_stats(f->p, &st);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 1U);
    NphProfileSegment segs[8];
    const u32 n = nph_profiler_frame_segments(f->p, 0U, segs, 8U);
    NTEST_ASSERT_EQ_UINT(n, 2U);
    const u32 on_a = find_segment(f->p, segs, n, "stuck", lane_named(f->p, "a"), 20U);
    NTEST_ASSERT_NE_UINT(on_a, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[on_a].end, 40U);
    const u32 on_b = find_segment(f->p, segs, n, "stuck", lane_named(f->p, "b"), 40U);
    NTEST_ASSERT_NE_UINT(on_b, NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(segs[on_b].end, 50U);
}

NTEST_F(ProfilerFixture, unbalanced_streams_are_repaired_and_counted)
{
    rec(EV_FRAME(1U, 0U));
    rec(EV_END(5U)); // nothing open
    rec(EV_BEGIN("a", 10U));
    rec(EV_END(20U));
    rec(ev(NPH_PROFILE_EVENT_NONE, NULL, 0U, 25U)); // not a valid type
    rec(EV_FRAME(2U, 30U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    NphProfilerStats st;
    nph_profiler_stats(f->p, &st);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 2U);
    NphProfilerFrame frame;
    NTEST_ASSERT_TRUE(nph_profiler_frame(f->p, 0U, &frame));
    NTEST_ASSERT_EQ_UINT(frame.segment_count, 1U);
}

NTEST(profiler, zones_deeper_than_the_limit_are_dropped_with_their_ends)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.max_depth = 2U;
    profiler_fixture_setup(&fx, 4096U, &d);
    rec(EV_FRAME(1U, 0U));
    rec(EV_BEGIN("a", 10U));
    rec(EV_BEGIN("b", 20U));
    rec(EV_BEGIN("c", 30U)); // past the limit
    rec(EV_END(40U));        // c's: swallowed
    rec(EV_END(50U));        // b
    rec(EV_END(60U));        // a
    rec(EV_FRAME(2U, 100U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 1U);
    NphProfileSegment segs[4];
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(fx.p, 0U, segs, 4U), 2U);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(fx.p, segs[0].name), "b");
    NTEST_ASSERT_EQ_UINT(segs[0].end, 50U);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(fx.p, segs[1].name), "a");
    NTEST_ASSERT_EQ_UINT(segs[1].end, 60U);
    NphProfilerStats st;
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 1U);
    profiler_fixture_teardown(&fx);
}

NTEST(profiler, lost_events_close_the_open_zones_at_the_next_frame)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    profiler_fixture_setup(&fx, 8U, NULL); // rings of 8 events
    rec(EV_FRAME(1U, 10U));
    rec(EV_BEGIN("a", 20U));
    for (u32 i = 0; i < 7U; ++i) {
        rec_counter("c", (f64)i, 30U + i); // the last one does not fit
    }
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 0U);
    rec(EV_FRAME(2U, 100U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 1U);
    NphProfilerFrame frame;
    NTEST_ASSERT_TRUE(nph_profiler_frame(fx.p, 0U, &frame));
    NTEST_ASSERT_EQ_UINT(frame.dropped_events, 1U); // seen during frame 1
    NphProfileSegment segs[4];
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(fx.p, 0U, segs, 4U), 1U);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(fx.p, segs[0].name), "a");
    NTEST_ASSERT_EQ_UINT(segs[0].end, 100U);
    NTEST_ASSERT_EQ_UINT(segs[0].flags, 0U); // closed, not continued
    NphProfilerStats st;
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_EQ_UINT(st.dropped_events, 1U);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 1U);
    profiler_fixture_teardown(&fx);
}

NTEST_F(ProfilerFixture, counters_keep_each_frames_last_sample)
{
    rec_counter("before any frame", 1.0, 5U); // ignored: no frame is open
    rec(EV_FRAME(1U, 10U));
    rec_counter("draws", 1.0, 11U);
    rec_counter("draws", 5.0, 12U);
    rec(EV_FRAME(2U, 20U));
    rec_counter("draws", 3.0, 21U);
    rec(EV_FRAME(3U, 30U));
    rec(EV_FRAME(4U, 40U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 3U);
    NphProfilerCounterStats c[4];
    const u32 n = nph_profiler_counter_stats(f->p, c, 4U);
    NTEST_ASSERT_EQ_UINT(n, 1U);
    NTEST_ASSERT_EQ_STR(c[0].name, "draws");
    NTEST_ASSERT_EQ_UINT(c[0].frames, 2U);
    NTEST_ASSERT_NEAR_F64(c[0].last, 3.0, 0.0);
    NTEST_ASSERT_NEAR_F64(c[0].mean, 4.0, 0.0);
    NTEST_ASSERT_NEAR_F64(c[0].min, 3.0, 0.0);
    NTEST_ASSERT_NEAR_F64(c[0].max, 5.0, 0.0);
}

NTEST(profiler, zone_statistics_slide_over_the_window)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.history_frames = 4U;
    profiler_fixture_setup(&fx, 4096U, &d);
    // Frame k (k = 1..10) runs "a" for k * 10 ticks and, in even frames,
    // "b" twice for 5 ticks.
    u64 t = 1000U;
    for (u64 k = 1U; k <= 10U; ++k) {
        rec(EV_FRAME(k, t));
        rec(EV_BEGIN("a", t + 1U));
        rec(EV_END(t + 1U + k * 10U));
        if (k % 2U == 0U) {
            rec(EV_BEGIN("b", t + 200U));
            rec(EV_END(t + 205U));
            rec(EV_BEGIN("b", t + 300U));
            rec(EV_END(t + 305U));
        }
        t += 1000U;
    }
    rec(EV_FRAME(11U, t));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 10U);
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(fx.p), 4U); // the window

    NphProfilerZoneStats z[8];
    const u32 n = nph_profiler_zone_stats(fx.p, z, 8U);
    NTEST_ASSERT_GE_UINT(n, 2U);
    // Sorted by mean: a (frames 7..10: 70+80+90+100 = 340 / 4) before b.
    NTEST_ASSERT_EQ_STR(z[0].name, "a");
    NTEST_ASSERT_NEAR_F64(z[0].mean_ms, 85.0 * 1e-6, 1e-10);
    NTEST_ASSERT_NEAR_F64(z[0].max_ms, 100.0 * 1e-6, 1e-10);
    NTEST_ASSERT_NEAR_F64(z[0].last_ms, 100.0 * 1e-6, 1e-10);
    NTEST_ASSERT_EQ_UINT(z[0].frames, 4U);
    NTEST_ASSERT_NEAR_F64(z[0].calls_per_frame, 1.0, 1e-12);
    NTEST_ASSERT_EQ_STR(z[1].name, "b");
    NTEST_ASSERT_EQ_UINT(z[1].frames, 2U); // frames 8 and 10
    NTEST_ASSERT_NEAR_F64(z[1].mean_ms, 10.0 * 2.0 / 4.0 * 1e-6, 1e-10);
    NTEST_ASSERT_NEAR_F64(z[1].calls_per_frame, 1.0, 1e-12);

    f32 times[8];
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_times(fx.p, times, 8U), 4U);
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_NEAR_F64((f64)times[i], 1000.0 * 1e-6, 1e-9);
    }
    NphProfilerFrame frame;
    NTEST_ASSERT_TRUE(nph_profiler_frame(fx.p, 3U, &frame));
    NTEST_ASSERT_EQ_UINT(frame.index, 7U);
    NTEST_ASSERT_FALSE(nph_profiler_frame(fx.p, 4U, &frame));
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(fx.p, 4U, NULL, 0U), 0U);
    profiler_fixture_teardown(&fx);
}

NTEST(profiler, frames_whose_segments_were_overwritten_leave_the_history)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.max_segments = 4U;
    profiler_fixture_setup(&fx, 4096U, &d);
    u64 t = 100U;
    for (u64 k = 1U; k <= 3U; ++k) {
        rec(EV_FRAME(k, t));
        for (u32 i = 0; i < 3U; ++i) {
            rec(EV_BEGIN("z", t + 10U * (u64)i + 1U));
            rec(EV_END(t + 10U * (u64)i + 5U));
        }
        t += 100U;
    }
    rec(EV_FRAME(4U, t));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 3U);
    // Nine segments through a ring of four: only frame 3's are whole.
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(fx.p), 1U);
    NphProfilerFrame frame;
    NTEST_ASSERT_TRUE(nph_profiler_frame(fx.p, 0U, &frame));
    NTEST_ASSERT_EQ_UINT(frame.index, 3U);
    NphProfileSegment segs[4];
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(fx.p, 0U, segs, 2U), 2U); // capped
    NTEST_ASSERT_EQ_UINT(segs[0].start, 301U);
    profiler_fixture_teardown(&fx);
}

NTEST_F(ProfilerFixture, names_are_interned_and_outlive_their_pointers)
{
    char *dynamic = (char *)NPH_ALLOC(nph_allocator_system(), 32U, 1U);
    NTEST_ASSERT_NOT_NULL(dynamic);
    memcpy(dynamic, "dynamic zone", 13U);
    rec(EV_FRAME(1U, 10U));
    rec(EV_BEGIN(dynamic, 11U));
    rec(EV_END(12U));
    rec(EV_FRAME(2U, 20U));
    // The memory holding the name is about to change: collect first.
    nph_profiler_forget_names(f->p);
    memcpy(dynamic, "reused", 7U);
    rec(EV_BEGIN(dynamic, 21U));
    rec(EV_END(22U));
    rec(EV_FRAME(3U, 30U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    NPH_FREE(nph_allocator_system(), dynamic, 32U, 1U);

    NphProfileSegment segs[2];
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(f->p, 1U, segs, 2U), 1U);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(f->p, segs[0].name), "dynamic zone");
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(f->p, 0U, segs, 2U), 1U);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(f->p, segs[0].name), "reused");
    NTEST_ASSERT_NE_UINT(nph_profiler_find_name(f->p, NPH_STR("reused")), NPH_PROFILER_NO_NAME);
    NTEST_ASSERT_EQ_UINT(nph_profiler_find_name(f->p, NPH_STR("never")), NPH_PROFILER_NO_NAME);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(f->p, 9999U), "?");
    NTEST_ASSERT_EQ_STR(nph_profiler_lane_name(f->p, 9999U), "?");

    // The same text at another address shares the id.
    char copy[16];
    memcpy(copy, "reused", 7U);
    rec(EV_BEGIN(copy, 31U));
    rec(EV_END(32U));
    rec(EV_FRAME(4U, 40U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    NphProfileSegment again;
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(f->p, 0U, &again, 1U), 1U);
    NTEST_ASSERT_EQ_UINT(again.name, segs[0].name);
}

NTEST(profiler, names_beyond_the_table_are_unnamed)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.max_names = 2U;
    profiler_fixture_setup(&fx, 4096U, &d);
    rec(EV_FRAME(1U, 10U));
    rec(EV_BEGIN("one", 11U));
    rec(EV_END(12U));
    rec(EV_BEGIN("two", 13U));
    rec(EV_END(14U));
    rec(EV_BEGIN("three", 15U));
    rec(EV_END(16U));
    rec(EV_FRAME(2U, 20U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 1U);
    NphProfileSegment segs[4];
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(fx.p, 0U, segs, 4U), 3U);
    NTEST_ASSERT_EQ_STR(nph_profiler_name(fx.p, segs[2].name), "?");
    NphProfilerStats st;
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_EQ_UINT(st.names, 2U);
    NTEST_ASSERT_GE_UINT(st.anomalies, 1U);
    profiler_fixture_teardown(&fx);
}

NTEST_F(ProfilerFixture, reset_discards_the_history_and_open_zones)
{
    rec(EV_FRAME(1U, 10U));
    rec(EV_BEGIN("open", 11U));
    rec(EV_FRAME(2U, 20U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    nph_profiler_reset(f->p);
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(f->p), 0U);
    NphProfilerZoneStats z[4];
    NTEST_ASSERT_EQ_UINT(nph_profiler_zone_stats(f->p, z, 4U), 0U);
    // The zone left open before the reset does not come back.
    rec(EV_FRAME(3U, 30U));
    rec(EV_FRAME(4U, 40U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(f->p), 1U);
    NphProfilerFrame frame;
    NTEST_ASSERT_TRUE(nph_profiler_frame(f->p, 0U, &frame));
    NTEST_ASSERT_EQ_UINT(frame.index, 3U);
    NTEST_ASSERT_EQ_UINT(frame.segment_count, 0U);
}

NTEST(profiler, young_events_wait_for_the_settle_window)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.settle_ns = 3600ULL * 1000000000ULL; // an hour
    d.tick_hz = 1000000000ULL;
    profiler_fixture_setup(&fx, 4096U, &d);
    const u64 now = nph_profile_ticks();
    rec(EV_FRAME(1U, now));
    rec(EV_FRAME(2U, now + 1U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 0U);
    NphProfilerStats st;
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_GE_UINT(st.pending, 2U);
    nph_profiler_forget_names(fx.p); // processes everything
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(fx.p), 1U);
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    profiler_fixture_teardown(&fx);
}

NTEST(profiler, threads_beyond_the_lanes_are_counted_not_drawn)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.max_lanes = 1U;
    profiler_fixture_setup(&fx, 4096U, &d);
    rec(EV_FRAME(1U, 10U));
    const NphProfileEvent other[] = {EV_BEGIN("elsewhere", 11U), EV_END(12U)};
    run_script_thread("other", other, 2U);
    rec(EV_FRAME(2U, 20U));
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(fx.p), 1U);
    NTEST_ASSERT_EQ_UINT(nph_profiler_lane_count(fx.p), 1U);
    NphProfilerStats st;
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 2U);
    profiler_fixture_teardown(&fx);
}

NTEST(profiler, creation_validation)
{
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    NphProfiler *p = NULL;
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &p), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(p);
    d.alloc = nph_allocator_system();
    d.max_depth = 256U;
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &p), NPH_RESULT_INVALID_ARGUMENT);
    d.max_depth = 0U;
    d.max_lanes = NPH_U16_MAX;
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &p), NPH_RESULT_INVALID_ARGUMENT);
    d.max_lanes = 0U;
    // The real counter rate, from the CPU or measured.
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &p), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(nph_profiler_tick_hz(p), 100000000U);
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(p), 0U);
    NphProfilerFrame frame;
    NTEST_ASSERT_FALSE(nph_profiler_frame(p, 0U, &frame));
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_times(p, NULL, 0U), 0U);
    NphProfilerZoneStats z;
    NTEST_ASSERT_EQ_UINT(nph_profiler_zone_stats(p, &z, 1U), 0U);
    NphProfilerCounterStats c;
    NTEST_ASSERT_EQ_UINT(nph_profiler_counter_stats(p, &c, 1U), 0U);
    nph_profiler_destroy(p);
    nph_profiler_destroy(NULL);
}

NTEST(profiler, creation_fails_cleanly_when_memory_runs_out)
{
    // Every allocation of creation fails in turn; nothing may leak.
    for (u32 budget = 0; budget < 64U; ++budget) {
        NphDebugAllocator da;
        nph_debug_allocator_init(&da, nph_allocator_system());
        FailingAllocator fail;
        failing_allocator_init(&fail, nph_debug_allocator_allocator(&da), budget);
        NphProfilerDesc d;
        memset(&d, 0, sizeof d);
        d.alloc = &fail.allocator;
        d.tick_hz = TEST_TICK_HZ;
        d.history_frames = 4U;
        d.max_segments = 16U;
        d.max_names = 8U;
        d.max_lanes = 2U;
        d.max_fibers = 4U;
        NphProfiler *p = NULL;
        const NphResult r = nph_profiler_create(&d, &p);
        if (r == NPH_RESULT_OK) {
            nph_profiler_destroy(p);
        } else {
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OUT_OF_MEMORY);
            NTEST_ASSERT_NULL(p);
        }
        NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
    }
}

// ---------------------------------------------------------------------------
// Real threads
// ---------------------------------------------------------------------------

typedef struct BusyThread {
    NphAtomicU32 *go;
    u32 zones;
    const char *name;
    NphResult attach;
} BusyThread;

static void busy_main(void *user)
{
    BusyThread *b = (BusyThread *)user;
    b->attach = nph_profile_thread_attach(b->name);
    while (nph_atomic_load_u32(b->go, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
        nph_thread_yield();
    }
    for (u32 i = 0; i < b->zones; ++i) {
        NPH_PROFILE_BEGIN("busy");
        NPH_PROFILE_BEGIN("busy.inner");
        volatile u32 sink = 0;
        for (u32 j = 0; j < 100U; ++j) {
            sink += j;
        }
        NPH_UNUSED(sink);
        NPH_PROFILE_END();
        NPH_PROFILE_END();
    }
    nph_profile_thread_detach();
}

NTEST(profiler, real_threads_merge_into_consistent_lanes)
{
    // Static: a failed assertion skips the teardown, and the next test
    // must not meet rings owned by a dead stack frame.
    static ProfilerFixture fx;
    memset(&fx, 0, sizeof fx);
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.tick_hz = 0U; // the real rate
    nph_debug_allocator_init(&fx.da, nph_allocator_system());
    NphProfileConfig config;
    memset(&config, 0, sizeof config);
    config.alloc = nph_debug_allocator_allocator(&fx.da);
    config.ring_capacity = 8192U;
    config.max_threads = 16U;
    NTEST_ASSERT_EQ_INT(nph_profile_init(&config), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_profile_thread_attach("main"), NPH_RESULT_OK);
    d.alloc = nph_debug_allocator_allocator(&fx.da);
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &fx.p), NPH_RESULT_OK);

    NphAtomicU32 go = NPH_ATOMIC_U32_INIT(0U);
    static const char *const names[4] = {"busy 0", "busy 1", "busy 2", "busy 3"};
    BusyThread busy[4];
    NphThread threads[4];
    NPH_PROFILE_FRAME(1U);
    for (u32 i = 0; i < 4U; ++i) {
        busy[i].go = &go;
        busy[i].zones = 1000U;
        busy[i].name = names[i];
        busy[i].attach = NPH_RESULT_UNKNOWN;
        NphThreadDesc td;
        memset(&td, 0, sizeof td);
        td.fn = busy_main;
        td.user = &busy[i];
        td.name = names[i];
        NTEST_ASSERT_EQ_INT(nph_thread_create(&threads[i], &td), NPH_RESULT_OK);
    }
    nph_atomic_store_u32(&go, 1U, NPH_MEMORY_ORDER_RELEASE);
    for (u32 i = 0; i < 4U; ++i) {
        nph_thread_join(&threads[i]);
        NTEST_ASSERT_EQ_INT(busy[i].attach, NPH_RESULT_OK);
    }
    NPH_PROFILE_FRAME(2U);
    nph_profiler_forget_names(fx.p); // everything, settled or not
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(fx.p), 1U);

    NphProfilerFrame frame;
    NTEST_ASSERT_TRUE(nph_profiler_frame(fx.p, 0U, &frame));
    NTEST_ASSERT_EQ_UINT(frame.segment_count, 8000U);
    NphProfileSegment *segs = (NphProfileSegment *)NPH_ALLOC(
        nph_allocator_system(), 8000U * sizeof(NphProfileSegment), NPH_ALIGNOF(NphProfileSegment));
    NTEST_ASSERT_NOT_NULL(segs);
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_segments(fx.p, 0U, segs, 8000U), 8000U);
    // Per lane, segments of one depth never overlap, inner ones lie inside
    // an outer one, and everything is inside the frame.
    u32 per_lane[16] = {0};
    for (u32 i = 0; i < 8000U; ++i) {
        const NphProfileSegment *s = &segs[i];
        NTEST_ASSERT_LE_UINT(s->start, s->end);
        NTEST_ASSERT_GE_UINT(s->start, frame.start);
        NTEST_ASSERT_LE_UINT(s->end, frame.end);
        NTEST_ASSERT_EQ_UINT(s->flags, 0U);
        NTEST_ASSERT_LT_UINT(s->lane, 16U);
        per_lane[s->lane] += 1U;
        if (i + 2U < 8000U && segs[i + 2U].lane == s->lane && segs[i + 2U].depth == s->depth) {
            NTEST_ASSERT_LE_UINT(s->end, segs[i + 2U].start + 1U);
        }
    }
    u32 busy_lanes = 0;
    for (u32 i = 0; i < 16U; ++i) {
        if (per_lane[i] != 0U) {
            NTEST_ASSERT_EQ_UINT(per_lane[i], 2000U);
            busy_lanes += 1U;
        }
    }
    NTEST_ASSERT_EQ_UINT(busy_lanes, 4U);
    NPH_FREE(nph_allocator_system(), segs, 8000U * sizeof(NphProfileSegment),
             NPH_ALIGNOF(NphProfileSegment));
    NphProfilerStats st;
    nph_profiler_stats(fx.p, &st);
    NTEST_ASSERT_EQ_UINT(st.anomalies, 0U);
    NTEST_ASSERT_EQ_UINT(st.dropped_events, 0U);
    profiler_fixture_teardown(&fx);
}

#else // !NPH_PROFILER_ENABLED

NTEST(profiler, collects_nothing_when_compiled_out)
{
    NphProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.tick_hz = TEST_TICK_HZ;
    NphProfiler *p = NULL;
    NTEST_ASSERT_EQ_INT(nph_profiler_create(&d, &p), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_profiler_collect(p), 0U);
    NTEST_ASSERT_EQ_UINT(nph_profiler_frame_count(p), 0U);
    nph_profiler_destroy(p);
}

#endif
