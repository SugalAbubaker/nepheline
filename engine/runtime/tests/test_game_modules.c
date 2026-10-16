// SPDX-License-Identifier: Apache-2.0
// Game modules end to end, against the engine as a shared library (as the
// player and the editor run it): a build loads and registers its
// components, cvar, command, action, and systems, and simulates; newer
// builds are swapped in, the world moving to new component layouts with
// every entity handle kept, the state block kept or reset, tuned cvars
// carried over; a broken build leaves the running one in place; play-in-
// editor plays a copy of the world with the same handles; pipelined and
// sequential frames agree.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/platform/dylib.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/runtime/engine.h>

#include <ntest/ntest.h>

#include "games/test_game.h"

#include <stdio.h>
#include <string.h>

#if !defined(TEST_GAME_V1) || !defined(TEST_GAME_V2) || !defined(TEST_GAME_V3)
#error "TEST_GAME_V1, V2, and V3 name the test game's three builds"
#endif

typedef struct GameFixture {
    NphDebugAllocator da;
    char dir[512];
    char game[600]; // the file the "build" writes
    NphEngine *engine;
} GameFixture;

// Writes `build` over the game's file, as a build would.
static void install(GameFixture *f, const char *build)
{
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(build, nph_allocator_system(), &data, &size),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(f->game, data, size), NPH_RESULT_OK);
    nph_file_free_all(nph_allocator_system(), data, size);
}

static void setup(GameFixture *f, const char *build, bool pipelined)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    char temp[400];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(temp, sizeof temp, &len), NPH_RESULT_OK);
    (void)snprintf(f->dir, sizeof f->dir, "%s/nph-game-tests-%llu", temp,
                   (unsigned long long)nph_platform_process_id());
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
    (void)snprintf(f->game, sizeof f->game, "%s/test_game%s", f->dir, nph_dylib_extension());
    install(f, build);
    static const char *const lockstep[3] = {"rt.lockstep=true", "rt.pipelined=true",
                                            "game.hot_reload=false"};
    static const char *const sequential[3] = {"rt.lockstep=true", "rt.pipelined=false",
                                              "game.hot_reload=false"};
    NphEngineConfig config;
    memset(&config, 0, sizeof config);
    config.game = f->game;
    config.headless = 1;
    for (u32 i = 0; i < 3U; ++i) {
        config.assignments[i] = pipelined ? lockstep[i] : sequential[i];
    }
    config.assignment_count = 3U;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.config = &config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "game module tests";
    NTEST_ASSERT_EQ_INT(nph_engine_create(&d, &f->engine), NPH_RESULT_OK);
}

static void teardown(GameFixture *f)
{
    nph_engine_destroy(f->engine);
    f->engine = NULL;
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
    NPH_IGNORE_RESULT(nph_fs_remove_tree(f->dir));
}

static TestGameState *state(GameFixture *f)
{
    NphGame *game = nph_engine_game(f->engine);
    NTEST_ASSERT_NOT_NULL(game);
    TestGameState *s = (TestGameState *)nph_game_state(game);
    NTEST_ASSERT_NOT_NULL(s);
    return s;
}

static void frames(GameFixture *f, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(f->engine), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_wait(f->engine), NPH_RESULT_OK);
}

// Spawns `n` placed entities with the game's Counter set to `value`.
static void spawn_counters(GameFixture *f, NphEntity *out, u32 n, i32 value)
{
    NphWorld *w = nph_engine_edit_world(f->engine);
    const NphComponentId counter = nph_game_component(nph_engine_game(f->engine), 0U);
    NTEST_ASSERT_NE_UINT(counter, NPH_COMPONENT_NONE);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, counter};
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &out[i]), NPH_RESULT_OK);
        i32 *c = (i32 *)nph_world_get_mut(w, out[i], counter);
        *c = value;
    }
}

static i32 counter_of(NphWorld *w, NphEntity e)
{
    const NphComponentId id = nph_world_find_component(w, NPH_STR(TEST_COUNTER_NAME));
    NTEST_ASSERT_NE_UINT(id, NPH_COMPONENT_NONE);
    const i32 *c = (const i32 *)nph_world_get(w, e, id);
    NTEST_ASSERT_NOT_NULL(c);
    return *c;
}

static NphCvar *find_cvar(GameFixture *f, const char *name)
{
    return nph_cvar_find(nph_engine_cvars(f->engine), nph_str_from_cstr(name));
}

static bool has_command(GameFixture *f, const char *name)
{
    NphConsole *c = nph_engine_console(f->engine);
    for (u32 i = 0; i < nph_console_command_count(c); ++i) {
        if (strcmp(nph_console_command_name(c, i), name) == 0) {
            return true;
        }
    }
    return false;
}

NTEST(GameModules, a_build_loads_and_simulates)
{
    static GameFixture fx;
    setup(&fx, TEST_GAME_V1, true);
    const NphGameApi *api = nph_engine_game_api(fx.engine);
    NTEST_ASSERT_NOT_NULL(api);
    NTEST_EXPECT_EQ_STR(api->name, "test_game");
    TestGameState *s = state(&fx);
    NTEST_EXPECT_EQ_UINT(s->loads, 1U);
    NTEST_EXPECT_EQ_UINT(s->build, 1U);
    NTEST_EXPECT_FALSE(s->reloaded);
    NTEST_EXPECT_TRUE(s->reset);
    NTEST_EXPECT_NOT_NULL(find_cvar(&fx, TEST_SPEED_CVAR));
    NTEST_EXPECT_TRUE(has_command(&fx, TEST_PING_COMMAND));
    NphActionId action = 0U;
    NTEST_EXPECT_EQ_INT(
        nph_input_action_find(nph_engine_input(fx.engine), TEST_ACTION_NAME, &action),
        NPH_RESULT_OK);
    NphEntity e[3];
    spawn_counters(&fx, e, 3U, 0);
    // Frames before play run the game's frame callback, not its systems.
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(s->steps, 0U);
    NTEST_EXPECT_EQ_UINT(s->frames, 2U);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(s->playing, 1U);
    frames(&fx, 5U);
    NTEST_EXPECT_EQ_UINT(s->steps, 5U);
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_EXPECT_EQ_INT(counter_of(nph_engine_world(fx.engine), e[i]), 5);
    }
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(fx.engine)->total_steps, 5U);
    NTEST_EXPECT_EQ_INT(
        nph_console_execute(nph_engine_console(fx.engine), NPH_STR(TEST_PING_COMMAND)),
        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(s->pings, 1U);
    // Pausing stops the steps; a single step goes through.
    nph_engine_set_paused(fx.engine, true);
    frames(&fx, 3U);
    NTEST_EXPECT_EQ_UINT(s->steps, 5U);
    nph_engine_step_once(fx.engine);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(s->steps, 6U);
    nph_engine_play_end(fx.engine);
    NTEST_EXPECT_EQ_UINT(s->playing, 0U);
    teardown(&fx);
}

NTEST(GameModules, reloads_move_the_world_and_keep_what_should_survive)
{
    static GameFixture fx;
    setup(&fx, TEST_GAME_V1, true);
    NphEntity e[4];
    spawn_counters(&fx, e, 4U, 0);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    frames(&fx, 3U);
    NphCvar *speed = find_cvar(&fx, TEST_SPEED_CVAR);
    NTEST_ASSERT_NOT_NULL(speed);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_float(nph_engine_cvars(fx.engine), speed, 3.5), NPH_RESULT_OK);
    // Nothing changed: no reload.
    bool reloaded = true;
    NTEST_ASSERT_EQ_INT(nph_engine_reload_game(fx.engine, &reloaded), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(reloaded);

    // Build 2: Counter gains a field, the system adds 10, a new tag.
    install(&fx, TEST_GAME_V2);
    NTEST_ASSERT_EQ_INT(nph_engine_reload_game(fx.engine, &reloaded), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(reloaded);
    NTEST_EXPECT_EQ_UINT(nph_engine_game_reloads(fx.engine), 1U);
    TestGameState *s = state(&fx);
    NTEST_EXPECT_EQ_UINT(s->build, 2U);
    NTEST_EXPECT_EQ_UINT(s->loads, 2U); // the same block: build 1 counted one
    NTEST_EXPECT_EQ_UINT(s->unloads, 1U);
    NTEST_EXPECT_TRUE(s->unloaded_for_reload);
    NTEST_EXPECT_TRUE(s->reloaded);
    NTEST_EXPECT_TRUE(s->migrated);
    NTEST_EXPECT_FALSE(s->reset);
    NTEST_EXPECT_EQ_UINT(s->steps, 3U);
    NTEST_EXPECT_EQ_UINT(s->playing, 1U); // the world kept playing
    // Every entity kept its handle and its value; the new field has its
    // default.
    NphWorld *w = nph_engine_world(fx.engine);
    const NphComponentId counter = nph_game_component(nph_engine_game(fx.engine), 0U);
    NTEST_ASSERT_EQ_UINT(nph_world_component_desc(w, counter)->size, sizeof(TestCounterV2));
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_TRUE(nph_world_is_alive(w, e[i]));
        const TestCounterV2 *c = (const TestCounterV2 *)nph_world_get(w, e[i], counter);
        NTEST_ASSERT_NOT_NULL(c);
        NTEST_EXPECT_EQ_INT(c->value, 3);
        NTEST_EXPECT_NEAR_F32(c->extra, 2.5F, 0.0F);
        NTEST_EXPECT_TRUE(nph_world_has(w, e[i], NPH_COMPONENT_ID_WORLD_TRANSFORM));
    }
    NTEST_EXPECT_NE_UINT(nph_game_component(nph_engine_game(fx.engine), 1U), NPH_COMPONENT_NONE);
    // The tuned cvar carried over to the new build's cvar.
    speed = find_cvar(&fx, TEST_SPEED_CVAR);
    NTEST_ASSERT_NOT_NULL(speed);
    NTEST_EXPECT_NEAR_F64(nph_cvar_get_float(speed), 3.5, 0.0);
    NTEST_EXPECT_TRUE(has_command(&fx, TEST_PING_COMMAND));
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_INT(counter_of(nph_engine_world(fx.engine), e[0]), 23);

    // Build 3: Counter is gone, the state's version changed.
    install(&fx, TEST_GAME_V3);
    NTEST_ASSERT_EQ_INT(nph_engine_reload_game(fx.engine, &reloaded), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(reloaded);
    s = state(&fx);
    NTEST_EXPECT_EQ_UINT(s->build, 3U);
    NTEST_EXPECT_TRUE(s->reset);
    NTEST_EXPECT_TRUE(s->migrated);
    NTEST_EXPECT_EQ_UINT(s->loads, 1U); // a new block
    NTEST_EXPECT_EQ_UINT(s->steps, 0U);
    w = nph_engine_world(fx.engine);
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(w, NPH_STR(TEST_COUNTER_NAME)),
                         NPH_COMPONENT_NONE);
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_EXPECT_TRUE(nph_world_is_alive(w, e[i]));
    }
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(state(&fx)->steps, 0U); // build 3 has no systems
    NTEST_EXPECT_NEAR_F64(nph_cvar_get_float(find_cvar(&fx, TEST_SPEED_CVAR)), 3.5, 0.0);
    NTEST_EXPECT_EQ_UINT(nph_engine_game_reloads(fx.engine), 2U);
    teardown(&fx);
}

NTEST(GameModules, a_broken_build_keeps_the_running_one)
{
    static GameFixture fx;
    setup(&fx, TEST_GAME_V1, true);
    NphEntity e[1];
    spawn_counters(&fx, e, 1U, 0);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    frames(&fx, 1U);
    static const char garbage[] = "not a shared library";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(fx.game, garbage, sizeof garbage), NPH_RESULT_OK);
    bool reloaded = true;
    NTEST_EXPECT_NE_INT(nph_engine_reload_game(fx.engine, &reloaded), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(reloaded);
    NTEST_ASSERT_NOT_NULL(nph_engine_game_api(fx.engine));
    NTEST_EXPECT_EQ_UINT(state(&fx)->build, 1U);
    NTEST_EXPECT_EQ_UINT(nph_engine_game_reloads(fx.engine), 0U);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_INT(counter_of(nph_engine_world(fx.engine), e[0]), 3);
    // A good build after the broken one goes in.
    install(&fx, TEST_GAME_V2);
    NTEST_ASSERT_EQ_INT(nph_engine_reload_game(fx.engine, &reloaded), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(reloaded);
    NTEST_EXPECT_EQ_UINT(state(&fx)->build, 2U);
    teardown(&fx);
}

NTEST(GameModules, play_in_editor_plays_a_copy_with_the_same_handles)
{
    static GameFixture fx;
    setup(&fx, TEST_GAME_V1, true);
    NphEntity e[2];
    spawn_counters(&fx, e, 2U, 7);
    NphWorld *edit = nph_engine_edit_world(fx.engine);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, true), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_is_playing(fx.engine));
    NphWorld *play = nph_engine_world(fx.engine);
    NTEST_ASSERT_TRUE(play != edit);
    NTEST_EXPECT_TRUE(nph_engine_edit_world(fx.engine) == edit);
    frames(&fx, 4U);
    for (u32 i = 0; i < 2U; ++i) {
        NTEST_EXPECT_TRUE(nph_world_is_alive(play, e[i]));
        NTEST_EXPECT_EQ_INT(counter_of(play, e[i]), 11);
        NTEST_EXPECT_EQ_INT(counter_of(edit, e[i]), 7); // untouched
    }
    NTEST_EXPECT_EQ_INT(nph_engine_play_begin(fx.engine, true), NPH_RESULT_INVALID_STATE);
    // Scenes do not load into a world being played.
    NTEST_EXPECT_EQ_INT(nph_engine_clear_scene(fx.engine), NPH_RESULT_INVALID_STATE);
    nph_engine_play_end(fx.engine);
    NTEST_EXPECT_FALSE(nph_engine_is_playing(fx.engine));
    NTEST_EXPECT_TRUE(nph_engine_world(fx.engine) == edit);
    NTEST_EXPECT_EQ_UINT(state(&fx)->playing, 0U);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_INT(counter_of(edit, e[0]), 7);

    // A reload that changes the components while playing a copy stops
    // playing first, then moves the edited world.
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, true), NPH_RESULT_OK);
    frames(&fx, 1U);
    install(&fx, TEST_GAME_V2);
    bool reloaded = false;
    NTEST_ASSERT_EQ_INT(nph_engine_reload_game(fx.engine, &reloaded), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(reloaded);
    NTEST_EXPECT_FALSE(nph_engine_is_playing(fx.engine));
    NphWorld *moved = nph_engine_edit_world(fx.engine);
    NTEST_EXPECT_TRUE(nph_engine_world(fx.engine) == moved);
    NTEST_EXPECT_EQ_INT(counter_of(moved, e[1]), 7);
    teardown(&fx);
}

NTEST(GameModules, pipelined_and_sequential_frames_agree)
{
    i32 values[2][3];
    u64 steps[2];
    for (u32 mode = 0; mode < 2U; ++mode) {
        static GameFixture fx;
        setup(&fx, TEST_GAME_V1, mode == 0U);
        NphEntity e[3];
        spawn_counters(&fx, e, 3U, 0);
        NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
        frames(&fx, 17U);
        for (u32 i = 0; i < 3U; ++i) {
            values[mode][i] = counter_of(nph_engine_world(fx.engine), e[i]);
        }
        steps[mode] = state(&fx)->steps;
        teardown(&fx);
    }
    NTEST_EXPECT_EQ_UINT(steps[0], steps[1]);
    NTEST_EXPECT_EQ_UINT(steps[0], 17U);
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_EXPECT_EQ_INT(values[0][i], values[1][i]);
    }
}

NTEST(GameModules, unloading_and_loading_again)
{
    static GameFixture fx;
    setup(&fx, TEST_GAME_V1, true);
    NphEntity e[1];
    spawn_counters(&fx, e, 1U, 0);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    frames(&fx, 2U);
    nph_engine_unload_game(fx.engine);
    NTEST_EXPECT_NULL(nph_engine_game(fx.engine));
    NTEST_EXPECT_NULL(find_cvar(&fx, TEST_SPEED_CVAR));
    NTEST_EXPECT_FALSE(has_command(&fx, TEST_PING_COMMAND));
    // The world keeps the game's data; nothing simulates it.
    frames(&fx, 3U);
    NTEST_EXPECT_EQ_INT(counter_of(nph_engine_world(fx.engine), e[0]), 2);
    NTEST_ASSERT_EQ_INT(nph_engine_load_game(fx.engine, fx.game), NPH_RESULT_OK);
    TestGameState *s = state(&fx);
    NTEST_EXPECT_FALSE(s->reloaded);
    NTEST_EXPECT_FALSE(s->migrated); // the same layouts
    NTEST_EXPECT_EQ_UINT(s->loads, 2U);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_INT(counter_of(nph_engine_world(fx.engine), e[0]), 3);
    teardown(&fx);
}
