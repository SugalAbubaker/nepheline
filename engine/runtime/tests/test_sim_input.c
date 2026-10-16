// SPDX-License-Identifier: Apache-2.0
// Simulation input: edges carried across frames until a step takes them,
// held state, motion accumulation.
#include <nepheline/runtime/sim_input.h>

#include <ntest/ntest.h>

#include <string.h>

typedef struct InputFixture {
    NphInput *input;
    NphActionId jump;
    NphActionId move;
    NphSimInputAccumulator acc;
} InputFixture;

NTEST_SETUP(InputFixture)
{
    NTEST_ASSERT_EQ_INT(nph_input_create(nph_allocator_system(), NULL, &f->input), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "jump", &f->jump), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_action_register(f->input, "move", &f->move), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key(f->input, f->jump, NPH_KEY_SPACE, NPH_MOD_ANY),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_input_bind_key_axis(f->input, f->move, NPH_KEY_A, NPH_KEY_D),
                        NPH_RESULT_OK);
    nph_sim_input_reset(&f->acc);
}

NTEST_TEARDOWN(InputFixture)
{
    nph_input_destroy(f->input);
}

static void key(InputFixture *f, NphKey k, NphKeyAction action)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_KEY;
    e.u.key.key = (u32)k;
    e.u.key.action = (u32)action;
    nph_input_process_event(f->input, &e);
}

static void mouse(InputFixture *f, f64 x, f64 y)
{
    NphWindowEvent e;
    memset(&e, 0, sizeof e);
    e.type = NPH_WINDOW_EVENT_MOUSE_MOVE;
    e.u.mouse_move.x = x;
    e.u.mouse_move.y = y;
    nph_input_process_event(f->input, &e);
}

// One frame: begin, the events, end, accumulate.
#define FRAME(f, events)                                                                           \
    do {                                                                                           \
        nph_input_begin_frame((f)->input);                                                         \
        events;                                                                                    \
        nph_input_end_frame((f)->input);                                                           \
        nph_sim_input_accumulate(&(f)->acc, (f)->input);                                           \
    } while (0)

NTEST_F(InputFixture, a_press_in_a_frame_without_steps_reaches_the_next_step_once)
{
    FRAME(f, key(f, NPH_KEY_SPACE, NPH_KEY_ACTION_PRESS)); // no step this frame
    FRAME(f, (void)0);                                     // nor this one
    NphSimInput in;
    nph_sim_input_take(&f->acc, &in);
    NTEST_ASSERT_EQ_UINT(in.action_count, 2U);
    NTEST_ASSERT_TRUE(nph_sim_input_pressed(&in, f->jump));
    NTEST_ASSERT_TRUE(nph_sim_input_down(&in, f->jump));
    NTEST_ASSERT_EQ_UINT(in.step, 0U);
    // The second step of the same frame: still held, no edge.
    nph_sim_input_take(&f->acc, &in);
    NTEST_ASSERT_FALSE(nph_sim_input_pressed(&in, f->jump));
    NTEST_ASSERT_TRUE(nph_sim_input_down(&in, f->jump));
    NTEST_ASSERT_EQ_UINT(in.step, 1U);
}

NTEST_F(InputFixture, a_tap_inside_one_frame_shows_both_edges)
{
    FRAME(f, (key(f, NPH_KEY_SPACE, NPH_KEY_ACTION_PRESS),
              key(f, NPH_KEY_SPACE, NPH_KEY_ACTION_RELEASE)));
    NphSimInput in;
    nph_sim_input_take(&f->acc, &in);
    NTEST_ASSERT_TRUE(nph_sim_input_pressed(&in, f->jump));
    NTEST_ASSERT_TRUE(nph_sim_input_released(&in, f->jump));
    NTEST_ASSERT_FALSE(nph_sim_input_down(&in, f->jump));
    // Two taps over two frames before a step count twice.
    FRAME(f, (key(f, NPH_KEY_SPACE, NPH_KEY_ACTION_PRESS),
              key(f, NPH_KEY_SPACE, NPH_KEY_ACTION_RELEASE)));
    FRAME(f, (key(f, NPH_KEY_SPACE, NPH_KEY_ACTION_PRESS),
              key(f, NPH_KEY_SPACE, NPH_KEY_ACTION_RELEASE)));
    nph_sim_input_take(&f->acc, &in);
    NTEST_ASSERT_EQ_UINT(in.actions[f->jump].pressed, 2U);
    NTEST_ASSERT_EQ_UINT(in.actions[f->jump].released, 2U);
}

NTEST_F(InputFixture, values_are_the_latest_and_motion_adds_up)
{
    FRAME(f, (key(f, NPH_KEY_D, NPH_KEY_ACTION_PRESS), mouse(f, 10.0, 0.0)));
    FRAME(f, mouse(f, 13.0, 4.0));
    FRAME(f, (key(f, NPH_KEY_A, NPH_KEY_ACTION_PRESS), mouse(f, 20.0, 4.0)));
    NphSimInput in;
    nph_sim_input_take(&f->acc, &in);
    NTEST_ASSERT_NEAR_F32(nph_sim_input_value(&in, f->move), 0.0F, 0.0F); // both keys: 0
    NTEST_ASSERT_TRUE(nph_sim_input_down(&in, f->move));
    // Frames 2 and 3 moved the cursor by (10, 4) together; whether frame 1
    // counts its move from the origin is the input layer's business.
    NTEST_ASSERT_NEAR_F64(in.mouse_dy, 4.0, 1e-12);
    NTEST_ASSERT_TRUE(in.mouse_dx == 10.0 || in.mouse_dx == 20.0);
    nph_sim_input_take(&f->acc, &in);
    NTEST_ASSERT_NEAR_F64(in.mouse_dx, 0.0, 0.0);
    NTEST_ASSERT_NEAR_F64(in.mouse_dy, 0.0, 0.0);
    // Unknown actions read as idle.
    NTEST_ASSERT_FALSE(nph_sim_input_down(&in, 63U));
    NTEST_ASSERT_FALSE(nph_sim_input_pressed(&in, NPH_ACTION_INVALID));
    NTEST_ASSERT_FALSE(nph_sim_input_released(&in, 7U));
    NTEST_ASSERT_NEAR_F32(nph_sim_input_value(&in, 9U), 0.0F, 0.0F);
}
