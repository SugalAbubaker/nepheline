// SPDX-License-Identifier: Apache-2.0
// The fixed-step clock: step counts, the cap, time scale, alpha, resets.
#include <nepheline/runtime/clock.h>

#include <ntest/ntest.h>

#define MS 1000000ULL

NTEST(clock, steps_follow_real_time)
{
    NphFixedClock c;
    nph_fixed_clock_init(&c, 100U, 8U); // 10 ms steps
    NTEST_ASSERT_EQ_UINT(c.step_ns, 10U * MS);
    NTEST_ASSERT_NEAR_F64(nph_fixed_clock_step_seconds(&c), 0.01, 1e-15);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1000U * MS), 0U); // starts the clock
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1004U * MS), 0U);
    NTEST_ASSERT_NEAR_F64(nph_fixed_clock_alpha(&c), 0.4, 1e-12);
    NTEST_ASSERT_NEAR_F64(c.frame_dt, 0.004, 1e-12);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1013U * MS), 1U); // 13 ms owed
    NTEST_ASSERT_NEAR_F64(nph_fixed_clock_alpha(&c), 0.3, 1e-12);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1040U * MS), 3U); // 30 ms owed
    NTEST_ASSERT_EQ_UINT(c.steps, 4U);
    // Time going backwards counts as none.
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1030U * MS), 0U);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1050U * MS), 1U);
    NTEST_ASSERT_EQ_UINT(c.dropped_ns, 0U);
}

// Drawing at the blend of the last batch, the world is shown one step
// behind real time: it advances by exactly each frame's time, whatever the
// frames take, from several steps to none.
NTEST(clock, the_blend_draws_at_the_pace_of_real_time)
{
    NphFixedClock c;
    nph_fixed_clock_init(&c, 100U, 64U); // 10 ms steps
    NTEST_ASSERT_NEAR_F64(nph_fixed_clock_blend(&c, 0U), 1.0, 0.0);
    u64 now = 1000U * MS;
    (void)nph_fixed_clock_advance(&c, now);
    const u64 frames[12] = {3U * MS,  4U * MS,  7U * MS, 25U * MS, 1U * MS,  9U * MS,
                            10U * MS, 33U * MS, 2U * MS, 2U * MS,  16U * MS, 6U * MS};
    u64 simulated = 0U; // steps
    u32 last_batch = 0U;
    f64 drawn_before = 0.0;
    for (u32 i = 0; i < 12U; ++i) {
        now += frames[i];
        const u32 steps = nph_fixed_clock_advance(&c, now);
        simulated += steps;
        last_batch = steps != 0U ? steps : last_batch;
        if (last_batch == 0U) {
            continue; // nothing simulated yet
        }
        const f64 blend = nph_fixed_clock_blend(&c, last_batch);
        NTEST_ASSERT_TRUE(blend >= 0.0 && blend < 1.0);
        const f64 after = (f64)simulated * 0.01;
        const f64 before = after - (f64)last_batch * 0.01;
        const f64 drawn = before + blend * (after - before);
        const f64 real = (f64)(now - 1000U * MS) * 1e-9;
        NTEST_EXPECT_NEAR_F64(drawn, real - 0.01, 1e-12);
        if (i > 0U && drawn_before > 0.0) {
            NTEST_EXPECT_NEAR_F64(drawn - drawn_before, (f64)frames[i] * 1e-9, 1e-12);
        }
        drawn_before = drawn;
    }
}

NTEST(clock, a_stall_is_capped_and_the_excess_dropped)
{
    NphFixedClock c;
    nph_fixed_clock_init(&c, 100U, 4U);
    (void)nph_fixed_clock_advance(&c, 1U * MS);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1001U * MS + 7U * MS), 4U);
    // 1007 ms owed: 4 steps run, 7 ms stay owed, 960 ms are dropped.
    NTEST_ASSERT_EQ_UINT(c.accumulator_ns, 7U * MS);
    NTEST_ASSERT_EQ_UINT(c.dropped_ns, 960U * MS);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1011U * MS), 1U);
}

NTEST(clock, rates_round_to_the_nearest_nanosecond)
{
    NphFixedClock c;
    nph_fixed_clock_init(&c, 240U, 8U);
    NTEST_ASSERT_EQ_UINT(c.step_ns, 4166667U);
    NTEST_ASSERT_EQ_UINT(c.rate_hz, 240U);
    nph_fixed_clock_set_rate(&c, 60U);
    NTEST_ASSERT_EQ_UINT(c.step_ns, 16666667U);
    nph_fixed_clock_set_rate(&c, NPH_FIXED_CLOCK_MAX_RATE);
    NTEST_ASSERT_EQ_UINT(c.step_ns, 100000U);
    // 240 Hz over one simulated second: exactly 240 steps in total.
    nph_fixed_clock_init(&c, 240U, 1000U);
    u32 total = 0;
    (void)nph_fixed_clock_advance(&c, 5U);
    for (u64 t = 1U; t <= 1000U; ++t) {
        total += nph_fixed_clock_advance(&c, 5U + t * MS);
    }
    NTEST_ASSERT_EQ_UINT(total, 239U); // 1 s minus the rounding: 239.99992 steps owed
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 5U + 1001U * MS), 1U);
}

NTEST(clock, time_scale_stretches_and_pauses)
{
    NphFixedClock c;
    nph_fixed_clock_init(&c, 100U, 8U);
    (void)nph_fixed_clock_advance(&c, 1U);
    nph_fixed_clock_set_time_scale(&c, 0.5);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1U + 40U * MS), 2U); // 20 ms simulated
    NTEST_ASSERT_NEAR_F64(c.frame_dt, 0.04, 1e-12);                       // 40 ms real
    nph_fixed_clock_set_time_scale(&c, 0.0);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1U + 1040U * MS), 0U);
    nph_fixed_clock_set_time_scale(&c, 2.0);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1U + 1050U * MS), 2U);
    // A reset forgets the owed time and restarts on the next advance.
    nph_fixed_clock_reset(&c);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1U + 5000U * MS), 0U);
    NTEST_ASSERT_EQ_UINT(nph_fixed_clock_advance(&c, 1U + 5005U * MS), 1U);
    NTEST_ASSERT_EQ_UINT(c.dropped_ns, 0U);
}
