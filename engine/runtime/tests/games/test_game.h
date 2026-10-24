// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the game module tests and the test game (test_game.c) share: the
// state block's layout and the names of what the game registers.

#include <nepheline/core/types.h>
#include <nepheline/platform/input.h>

#define TEST_COUNTER_NAME "test.Counter"
#define TEST_MARKER_NAME  "test.Marker"
#define TEST_SPEED_CVAR   "test.speed"
#define TEST_PING_COMMAND "test.ping"
#define TEST_ACTION_NAME  "test.jump"

typedef struct TestGameState {
    u64 loads;   // load calls since the block was created
    u64 unloads; // unload calls since the block was created
    u64 steps;   // simulation steps seen
    u64 frames;  // frame callbacks
    u64 pings;   // console pings
    u32 build;   // TEST_GAME_VERSION of the build that loaded last
    b32 reloaded;
    b32 migrated;
    b32 reset;
    b32 unloaded_for_reload;
    u32 playing;
    NphActionId action;
    u32 last_step_input_down;
    u32 physics_at_play_begin; // the physics world was there
    u32 crate;                 // the entity (its value) play_begin made, a static body
} TestGameState;

// The layout of build 2's Counter, for reading it from the tests.
typedef struct TestCounterV2 {
    i32 value;
    f32 extra;
} TestCounterV2;
