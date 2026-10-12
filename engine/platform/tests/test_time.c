// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

NTEST(time, monotonic_never_decreases)
{
    u64 prev = nph_time_now_ns();
    NTEST_ASSERT_GT_UINT(prev, 0U);
    for (u32 i = 0; i < 100000U; ++i) {
        const u64 now = nph_time_now_ns();
        NTEST_ASSERT_GE_UINT(now, prev);
        prev = now;
    }
}

NTEST(time, monotonic_advances_through_sleep)
{
    const u64 t0 = nph_time_now_ns();
    nph_thread_sleep_ns(5U * NPH_TIME_NS_PER_MS);
    const u64 t1 = nph_time_now_ns();
    NTEST_ASSERT_GE_UINT(t1 - t0, 5U * NPH_TIME_NS_PER_MS);
    // Generous upper bound: a loaded CI runner may oversleep, but not by seconds.
    NTEST_ASSERT_LT_UINT(t1 - t0, 2U * NPH_TIME_NS_PER_S);
}

NTEST(time, monotonic_has_sub_millisecond_resolution)
{
    // Consecutive readings in a busy loop advance by far less than a
    // millisecond on a clock with real resolution; a coarse clock would
    // only ever jump by its 1-15 ms tick. Pre-emption can insert a large
    // gap now and then, so only the small steps are counted.
    u32 small_steps = 0U;
    u64 prev = nph_time_now_ns();
    for (u32 i = 0; i < 100000U && small_steps < 10U; ++i) {
        const u64 now = nph_time_now_ns();
        if (now != prev) {
            if (now - prev < NPH_TIME_NS_PER_MS) {
                ++small_steps;
            }
            prev = now;
        }
    }
    NTEST_ASSERT_EQ_UINT(small_steps, 10U);
}

NTEST(time, wall_clock_is_after_2025)
{
    const u64 wall = nph_time_wall_ns();
    // 2025-01-01T00:00:00Z in nanoseconds since the Unix epoch.
    NTEST_ASSERT_GT_UINT(wall, 1735689600ULL * NPH_TIME_NS_PER_S);
    // And before 2100, which catches a wrong epoch or unit.
    NTEST_ASSERT_LT_UINT(wall, 4102444800ULL * NPH_TIME_NS_PER_S);
}

NTEST(time, wall_clock_tracks_monotonic)
{
    const u64 w0 = nph_time_wall_ns();
    const u64 m0 = nph_time_now_ns();
    nph_thread_sleep_ns(20U * NPH_TIME_NS_PER_MS);
    const u64 w1 = nph_time_wall_ns();
    const u64 m1 = nph_time_now_ns();
    const u64 wall_elapsed = w1 - w0;
    const u64 mono_elapsed = m1 - m0;
    const u64 diff =
        wall_elapsed > mono_elapsed ? wall_elapsed - mono_elapsed : mono_elapsed - wall_elapsed;
    // Both clocks measured the same sleep; allow slop for pre-emption
    // between the readings and for clock slewing.
    NTEST_ASSERT_LT_UINT(diff, 20U * NPH_TIME_NS_PER_MS);
}

NTEST(time, cycles_increase)
{
    const u64 c0 = nph_time_cycles();
    u64 c1 = c0;
    for (u32 i = 0; i < 1000U && c1 == c0; ++i) {
        c1 = nph_time_cycles();
    }
    NTEST_ASSERT_GT_UINT(c1, c0);
}

NTEST(time, conversions)
{
    NTEST_ASSERT_NEAR_F64(nph_time_ns_to_s(1500000000ULL), 1.5, 1e-12);
    NTEST_ASSERT_NEAR_F64(nph_time_ns_to_ms(2500000ULL), 2.5, 1e-12);
    NTEST_ASSERT_NEAR_F64(nph_time_ns_to_us(3500ULL), 3.5, 1e-12);
    NTEST_ASSERT_EQ_UINT(nph_time_ms_to_ns(7U), 7000000ULL);
    NTEST_ASSERT_EQ_UINT(nph_time_us_to_ns(7U), 7000ULL);
    NTEST_ASSERT_EQ_UINT(nph_time_s_to_ns(0.25), 250000000ULL);
    NTEST_ASSERT_EQ_UINT(nph_time_s_to_ns(-1.0), 0U);
}

NTEST(time, stopwatch)
{
    NphStopwatch sw = nph_stopwatch_start();
    nph_thread_sleep_ns(2U * NPH_TIME_NS_PER_MS);
    const u64 lap = nph_stopwatch_lap_ns(&sw);
    NTEST_ASSERT_GE_UINT(lap, 2U * NPH_TIME_NS_PER_MS);
    // After the lap the stopwatch restarted: the new elapsed time is tiny.
    NTEST_ASSERT_LT_UINT(nph_stopwatch_elapsed_ns(&sw), lap);
    NTEST_ASSERT_GE_UINT(nph_stopwatch_elapsed_ms(&sw) >= 0.0, 1U);
}
