// SPDX-License-Identifier: Apache-2.0
// A game module for the game module tests, built three times
// (TEST_GAME_VERSION 1, 2, 3) as successive builds of one game that change
// the way development changes a game:
//   1: a Counter component, a system that adds 1 to every Counter each
//      step, a state block, a cvar, and a console command;
//   2: Counter gains a field (`extra`, default 2.5), so the world moves to
//      the new layout; the system adds 10; a new Marker tag; the state's
//      version is unchanged, so the state block carries over;
//   3: Counter is gone, so entities lose it; the state's version changed,
//      so the block starts over.
// Every build's play_begin makes a crate (an entity with a Collider) and
// lifts every kinematic body a metre.
#include "test_game.h"

#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/game.h>

#include <string.h>

#if TEST_GAME_VERSION == 1
#define TEST_COUNTER_FIELDS(F) F(I32, value, NPH_FIELD_NO_HINTS)
#define INCREMENT              1
#elif TEST_GAME_VERSION == 2
#define TEST_COUNTER_FIELDS(F)                                                                     \
    F(I32, value, NPH_FIELD_NO_HINTS)                                                              \
    F(F32, extra, NPH_FIELD_NO_HINTS)
#define INCREMENT 10
#endif

#if TEST_GAME_VERSION <= 2
NPH_COMPONENT_STRUCT(TestCounter, TEST_COUNTER_FIELDS);
NPH_COMPONENT_DECLARE(TestCounter);
#if TEST_GAME_VERSION == 1
static const TestCounter g_counter_defaults = {0};
#else
static const TestCounter g_counter_defaults = {0, 2.5F};
#endif
NPH_COMPONENT_DEFINE(TestCounter, TEST_COUNTER_FIELDS, TEST_COUNTER_NAME, 0U, &g_counter_defaults)
#endif

#if TEST_GAME_VERSION >= 2
NPH_TAG_DECLARE(TestMarker);
NPH_TAG_DEFINE(TestMarker, TEST_MARKER_NAME, 0U)
#endif

static NPH_CVAR_FLOAT(g_speed, TEST_SPEED_CVAR, 1.0, 0.0, 100.0, "a tunable of the test game",
                      NPH_CVAR_FLAG_NONE);

// Looked up again by every build's load: ids may change across reloads.
static NphComponentId g_counter = NPH_COMPONENT_NONE;

static TestGameState *state_of(const NphGame *game)
{
    return (TestGameState *)nph_game_state(game);
}

#if TEST_GAME_VERSION <= 2
static void count(NphSystemContext *ctx)
{
    NphGame *game = (NphGame *)ctx->user;
    TestGameState *s = state_of(game);
    s->steps += 1U;
    s->last_step_input_down = nph_sim_input_down(nph_game_input(game), s->action) ? 1U : 0U;
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        TestCounter *c = NPH_CHUNK_COLUMN(&v, 0, TestCounter);
        for (u32 i = 0; i < v.count; ++i) {
            c[i].value += INCREMENT;
        }
    }
}
#endif

static NphResult ping(NphConsole *console, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    TestGameState *s = state_of((const NphGame *)user);
    s->pings += 1U;
    nph_console_printf(console, "pong from build %d", TEST_GAME_VERSION);
    return NPH_RESULT_OK;
}

static NphResult load(NphGame *game, const NphGameLoadInfo *info)
{
    TestGameState *s = state_of(game);
    s->loads += 1U;
    s->build = TEST_GAME_VERSION;
    s->reloaded = info->reloaded;
    s->migrated = info->world_migrated;
    s->reset = info->state_reset;
    g_counter = nph_game_component(game, 0U);
    NPH_TRY(nph_game_action(game, TEST_ACTION_NAME, &s->action));
    NPH_TRY(nph_game_register_cvar(game, &g_speed));
    return nph_game_register_command(game, TEST_PING_COMMAND, "answers pong", ping, game);
}

static void unload(NphGame *game, b32 reloading)
{
    TestGameState *s = state_of(game);
    s->unloads += 1U;
    s->unloaded_for_reload = reloading;
}

static NphResult add_systems(NphGame *game, NphSchedule *schedule)
{
#if TEST_GAME_VERSION <= 2
    const NphQueryTerm terms[1] = {nph_term_write(g_counter)};
    const char *const after[1] = {NPH_TRANSFORM_SYSTEM_NAME};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "test.count";
    d.run = count;
    d.user = game;
    d.terms = terms;
    d.term_count = 1U;
    d.after = after;
    d.after_count = 1U;
    return nph_schedule_add_system(schedule, &d, NULL);
#else
    NPH_UNUSED(game);
    NPH_UNUSED(schedule);
    return NPH_RESULT_OK;
#endif
}

static void frame(NphGame *game, f64 dt)
{
    NPH_UNUSED(dt);
    state_of(game)->frames += 1U;
}

static NphResult play_begin(NphGame *game)
{
    TestGameState *s = state_of(game);
    s->playing = 1U;
    s->physics_at_play_begin = nph_game_physics(game) != NULL ? 1U : 0U;
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->collider};
    NphWorld *w = nph_game_world(game);
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        const NphRigidBody *rb = NPH_WORLD_GET(w, e, ids->rigid_body, NphRigidBody);
        if (rb != NULL && rb->type == (u32)NPH_RIGID_BODY_KINEMATIC) {
            NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
                ->translation.y += 1.0;
        }
    }
    NphEntity crate;
    NPH_TRY(nph_world_spawn(w, comps, 3U, &crate));
    s->crate = crate.value;
    return NPH_RESULT_OK;
}

static void play_end(NphGame *game)
{
    state_of(game)->playing = 0U;
}

NPH_GAME_EXPORT const NphGameApi *nph_game_entry(void)
{
#if TEST_GAME_VERSION == 1
    static const NphComponentDesc *components[1];
    components[0] = TestCounter_component();
    const u32 component_count = 1U;
#elif TEST_GAME_VERSION == 2
    static const NphComponentDesc *components[2];
    components[0] = TestCounter_component();
    components[1] = TestMarker_component();
    const u32 component_count = 2U;
#else
    static const NphComponentDesc *components[1];
    components[0] = TestMarker_component();
    const u32 component_count = 1U;
#endif
    static NphGameApi api;
    api.abi_version = NPH_GAME_ABI_VERSION;
    api.state_version = TEST_GAME_VERSION <= 2 ? 1U : 2U;
    api.state_size = sizeof(TestGameState);
    api.state_align = NPH_ALIGNOF(TestGameState);
    api.name = "test_game";
    api.components = components;
    api.component_count = component_count;
    api.load = load;
    api.unload = unload;
    api.add_systems = add_systems;
    api.frame = frame;
    api.play_begin = play_begin;
    api.play_end = play_end;
    return &api;
}
