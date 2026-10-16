// SPDX-License-Identifier: Apache-2.0
// Input for fixed steps: accumulation across frames, consumption by steps.
// See sim_input.h.
#include <nepheline/core/assert.h>
#include <nepheline/runtime/sim_input.h>

#include <string.h>

void nph_sim_input_reset(NphSimInputAccumulator *acc)
{
    NPH_ASSERT(acc != NULL);
    memset(acc, 0, sizeof *acc);
}

void nph_sim_input_accumulate(NphSimInputAccumulator *acc, const NphInput *input)
{
    NPH_ASSERT(acc != NULL && input != NULL);
    NphSimInput *p = &acc->pending;
    const u32 count = nph_min_u32(nph_input_action_count(input), NPH_SIM_INPUT_MAX_ACTIONS);
    for (u32 a = count; a < p->action_count; ++a) {
        memset(&p->actions[a], 0, sizeof p->actions[a]); // actions that went away
    }
    p->action_count = count;
    for (u32 a = 0; a < count; ++a) {
        NphSimAction *s = &p->actions[a];
        s->down = nph_input_action_down(input, a) ? 1 : 0;
        s->value = nph_input_action_value(input, a);
        s->pressed += nph_input_action_pressed(input, a) ? 1U : 0U;
        s->released += nph_input_action_released(input, a) ? 1U : 0U;
    }
    f64 dx = 0.0;
    f64 dy = 0.0;
    nph_input_mouse_delta(input, &dx, &dy);
    p->mouse_dx += dx;
    p->mouse_dy += dy;
    nph_input_scroll(input, &dx, &dy);
    p->scroll_dx += dx;
    p->scroll_dy += dy;
}

void nph_sim_input_take(NphSimInputAccumulator *acc, NphSimInput *out)
{
    NPH_ASSERT(acc != NULL && out != NULL);
    NphSimInput *p = &acc->pending;
    p->step = acc->steps++;
    memcpy(out, p, sizeof *out);
    for (u32 a = 0; a < p->action_count; ++a) {
        p->actions[a].pressed = 0U;
        p->actions[a].released = 0U;
    }
    p->mouse_dx = 0.0;
    p->mouse_dy = 0.0;
    p->scroll_dx = 0.0;
    p->scroll_dy = 0.0;
}
