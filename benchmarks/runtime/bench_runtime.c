// SPDX-License-Identifier: Apache-2.0
// Runtime benchmarks (docs/runtime.md).
//
//   engine_frame_headless            one frame of a headless engine playing an
//                                    empty world in lockstep: the sync point,
//                                    a one-step simulation job started, and
//                                    the previous one waited for
//                                    (rt.pipelined), the profiler's collection
//   engine_frame_headless_10k        the same with 10,000 placed entities that
//                                    do not move: the cost a static world adds
//   engine_frame_headless_10k_moving the same with every LocalTransform
//                                    written before each frame (the writes are
//                                    not timed): transforms at the sync point
//                                    and in the step
//   engine_play_copy_10k             play-in-editor on a 10,000-entity world:
//                                    the copy with every handle kept, the
//                                    world switch, and stopping
//   sim_input_frame                  one frame of input into the accumulator
//                                    and one step's snapshot taken
//   console_set_cvar                 a cvar set through the console, its
//                                    echo going to the log's rings as in an
//                                    engine (delivered outside the timing)

#include <nepheline/runtime/console.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/sim_input.h>

#include <nbench/nbench.h>

#include <stdlib.h>
#include <string.h>

#define ENTITIES 10000U

static NphEngine *headless_engine(void)
{
    static const char *const assignments[2] = {"rt.lockstep=true", "rt.max_entities=65536"};
    NphEngineConfig config;
    memset(&config, 0, sizeof config);
    config.headless = 1;
    config.assignments[0] = assignments[0];
    config.assignments[1] = assignments[1];
    config.assignment_count = 2U;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.config = &config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "runtime bench";
    d.play_on_start = 1;
    NphEngine *e = NULL;
    if (nph_engine_create(&d, &e) != NPH_RESULT_OK) {
        abort();
    }
    return e;
}

static void spawn(NphEngine *e, u32 count, NphEntity *out)
{
    const NphComponentId comps[2] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    if (nph_world_spawn_batch(nph_engine_edit_world(e), comps, 2U, count, out) != NPH_RESULT_OK) {
        abort();
    }
}

static void frame(NphEngine *e)
{
    if (nph_engine_frame(e) != NPH_RESULT_OK) {
        abort();
    }
}

NBENCH(engine_frame_headless)
{
    NphEngine *e = headless_engine();
    for (u32 i = 0; i < 8U; ++i) {
        frame(e);
    }
    while (nbench_keep_running(state)) {
        frame(e);
    }
    nph_engine_destroy(e);
}

NBENCH(engine_frame_headless_10k)
{
    NphEngine *e = headless_engine();
    NphEntity *entities = (NphEntity *)malloc(sizeof(NphEntity) * ENTITIES);
    if (entities == NULL) {
        abort();
    }
    nph_engine_play_end(e);
    spawn(e, ENTITIES, entities);
    if (nph_engine_play_begin(e, false) != NPH_RESULT_OK) {
        abort();
    }
    for (u32 i = 0; i < 8U; ++i) {
        frame(e);
    }
    while (nbench_keep_running(state)) {
        frame(e);
    }
    nph_engine_destroy(e);
    free(entities);
}

NBENCH(engine_frame_headless_10k_moving)
{
    NphEngine *e = headless_engine();
    NphEntity *entities = (NphEntity *)malloc(sizeof(NphEntity) * ENTITIES);
    if (entities == NULL) {
        abort();
    }
    nph_engine_play_end(e);
    spawn(e, ENTITIES, entities);
    if (nph_engine_play_begin(e, false) != NPH_RESULT_OK) {
        abort();
    }
    NphWorld *w = nph_engine_world(e);
    f64 x = 0.0;
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        if (nph_engine_wait(e) != NPH_RESULT_OK) {
            abort();
        }
        x += 0.001;
        for (u32 i = 0; i < ENTITIES; ++i) {
            NPH_WORLD_GET_MUT(w, entities[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
                ->translation.x = x;
        }
        nbench_resume_timing(state);
        frame(e);
    }
    nph_engine_destroy(e);
    free(entities);
}

NBENCH(engine_play_copy_10k)
{
    NphEngine *e = headless_engine();
    NphEntity *entities = (NphEntity *)malloc(sizeof(NphEntity) * ENTITIES);
    if (entities == NULL) {
        abort();
    }
    nph_engine_play_end(e);
    spawn(e, ENTITIES, entities);
    frame(e);
    while (nbench_keep_running(state)) {
        if (nph_engine_play_begin(e, true) != NPH_RESULT_OK) {
            abort();
        }
        nph_engine_play_end(e);
    }
    nph_engine_destroy(e);
    free(entities);
}

NBENCH(sim_input_frame)
{
    NphInput *input = NULL;
    NphInputDesc d;
    memset(&d, 0, sizeof d);
    if (nph_input_create(nph_allocator_system(), &d, &input) != NPH_RESULT_OK) {
        abort();
    }
    NphActionId action = 0U;
    for (u32 i = 0; i < 16U; ++i) {
        char name[32];
        name[0] = 'a';
        name[1] = (char)('a' + i);
        name[2] = '\0';
        if (nph_input_action_register(input, name, &action) != NPH_RESULT_OK ||
            nph_input_bind_key(input, action, (NphKey)((u32)NPH_KEY_A + i), NPH_MOD_ANY) !=
                NPH_RESULT_OK) {
            abort();
        }
    }
    NphSimInputAccumulator acc;
    nph_sim_input_reset(&acc);
    NphSimInput out;
    u32 checksum = 0U;
    while (nbench_keep_running(state)) {
        nph_input_begin_frame(input);
        nph_input_end_frame(input);
        nph_sim_input_accumulate(&acc, input);
        nph_sim_input_take(&acc, &out);
        checksum += out.action_count;
    }
    nbench_do_not_optimize(&checksum);
    nph_input_destroy(input);
}

NBENCH(console_set_cvar)
{
    NphLogConfig lc;
    memset(&lc, 0, sizeof lc);
    lc.alloc = nph_allocator_system();
    if (nph_log_init(&lc) != NPH_RESULT_OK) {
        abort();
    }
    NphCvarRegistry cvars;
    if (nph_cvar_registry_init(&cvars, nph_allocator_system()) != NPH_RESULT_OK) {
        abort();
    }
    static NPH_CVAR_INT(rate, "sim.rate", 240, 1, 10000, "steps per second", 0);
    rate.registered = 0U;
    NphConsole *console = NULL;
    if (nph_cvar_register(&cvars, &rate) != NPH_RESULT_OK ||
        nph_console_create(nph_allocator_system(), &cvars, &console) != NPH_RESULT_OK) {
        abort();
    }
    u32 i = 0U;
    while (nbench_keep_running(state)) {
        const NphStr line = (i++ & 1U) != 0U ? NPH_STR("sim.rate 120") : NPH_STR("sim.rate 240");
        if (nph_console_execute(console, line) != NPH_RESULT_OK) {
            abort();
        }
        if ((i & 255U) == 0U) {
            nbench_pause_timing(state);
            (void)nph_log_flush(0U); // no sinks: the records are dropped
            nbench_resume_timing(state);
        }
    }
    nph_console_destroy(console);
    nph_cvar_registry_free(&cvars);
    nph_log_shutdown();
}
