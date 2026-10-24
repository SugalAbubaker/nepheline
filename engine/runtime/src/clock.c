// SPDX-License-Identifier: Apache-2.0
// The fixed-step clock. See clock.h.
#include <nepheline/core/assert.h>
#include <nepheline/runtime/clock.h>

#include <string.h>

static u64 step_of(u32 rate_hz)
{
    NPH_ASSERT(rate_hz >= 1U && rate_hz <= NPH_FIXED_CLOCK_MAX_RATE);
    const u32 rate = nph_clamp_u32(rate_hz, 1U, NPH_FIXED_CLOCK_MAX_RATE);
    // Rounded to the nearest nanosecond: 240 Hz is 4 166 667 ns.
    return (1000000000ULL + rate / 2U) / rate;
}

void nph_fixed_clock_init(NphFixedClock *c, u32 rate_hz, u32 max_steps)
{
    NPH_ASSERT(c != NULL && max_steps >= 1U);
    memset(c, 0, sizeof *c);
    c->step_ns = step_of(rate_hz);
    c->rate_hz = nph_clamp_u32(rate_hz, 1U, NPH_FIXED_CLOCK_MAX_RATE);
    c->max_steps = max_steps >= 1U ? max_steps : 1U;
    c->time_scale = 1.0;
}

void nph_fixed_clock_set_rate(NphFixedClock *c, u32 rate_hz)
{
    NPH_ASSERT(c != NULL);
    c->step_ns = step_of(rate_hz);
    c->rate_hz = nph_clamp_u32(rate_hz, 1U, NPH_FIXED_CLOCK_MAX_RATE);
}

void nph_fixed_clock_set_time_scale(NphFixedClock *c, f64 scale)
{
    NPH_ASSERT(c != NULL && scale >= 0.0);
    c->time_scale = scale > 0.0 ? scale : 0.0;
}

u32 nph_fixed_clock_advance(NphFixedClock *c, u64 now_ns)
{
    NPH_ASSERT(c != NULL);
    if (c->last_ns == 0U) {
        c->last_ns = now_ns != 0U ? now_ns : 1U;
        c->frame_dt = 0.0;
        return 0U;
    }
    const u64 real = now_ns > c->last_ns ? now_ns - c->last_ns : 0U;
    c->last_ns = now_ns > c->last_ns ? now_ns : c->last_ns;
    c->frame_dt = (f64)real * 1e-9;
    const u64 scaled = c->time_scale == 1.0 ? real : (u64)((f64)real * c->time_scale);
    c->accumulator_ns += scaled;
    u64 steps = c->accumulator_ns / c->step_ns;
    if (steps > c->max_steps) {
        // Behind by more than the cap allows: keep the remainder below one
        // step and drop the rest, so the next frame starts caught up.
        const u64 keep = (u64)c->max_steps * c->step_ns + c->accumulator_ns % c->step_ns;
        c->dropped_ns += c->accumulator_ns - keep;
        c->accumulator_ns = keep;
        steps = c->max_steps;
    }
    c->accumulator_ns -= steps * c->step_ns;
    c->steps += steps;
    return (u32)steps;
}

f64 nph_fixed_clock_alpha(const NphFixedClock *c)
{
    NPH_ASSERT(c != NULL);
    return (f64)c->accumulator_ns / (f64)c->step_ns;
}

f64 nph_fixed_clock_blend(const NphFixedClock *c, u32 steps)
{
    NPH_ASSERT(c != NULL);
    if (steps == 0U) {
        return 1.0;
    }
    return 1.0 - (1.0 - nph_fixed_clock_alpha(c)) / (f64)steps;
}

void nph_fixed_clock_reset(NphFixedClock *c)
{
    NPH_ASSERT(c != NULL);
    c->accumulator_ns = 0U;
    c->last_ns = 0U;
}
