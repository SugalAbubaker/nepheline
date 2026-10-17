// SPDX-License-Identifier: Apache-2.0
// The engine: lifecycle, worlds, the frame. See engine.h, docs/runtime.md,
// and docs/adr/0023-runtime.md.
#include <nepheline/core/assert.h>
#include <nepheline/core/profile.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/scene/scene_io.h>

#include "engine_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef NPH_SHADER_DIR
#error "nph_runtime is built with NPH_SHADER_DIR (nph_target_use_shaders)"
#endif

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

#if NPH_CONFIG_SHIPPING
#define DEV_DEFAULT 0
#else
#define DEV_DEFAULT 1
#endif

#define CVAR_SAVED_LATCHED (NPH_CVAR_FLAG_SAVED | NPH_CVAR_FLAG_LATCHED)

static const NPH_CVAR_INT(k_max_frames, "rt.max_frames", 0, 0, NPH_I64_MAX,
                          "stop after this many frames (0: never)", NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_BOOL(k_pipelined, "rt.pipelined", 1,
                           "simulate the next frame while this one renders", NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_INT(k_max_fps, "rt.max_fps", 0, 0, 10000,
                          "frame rate cap (0: none beyond the swapchain's)", NPH_CVAR_FLAG_SAVED);
static const NPH_CVAR_BOOL(k_lockstep, "rt.lockstep", 0,
                           "one simulation step per frame, whatever the time (tests, debugging)",
                           NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_INT(k_max_entities, "rt.max_entities", 65536, 1024, 1 << 24,
                          "entities a world holds", NPH_CVAR_FLAG_LATCHED);
static const NPH_CVAR_INT(k_world_events, "rt.world_events", 16384, 64, 1 << 22,
                          "world events kept between frames", NPH_CVAR_FLAG_LATCHED);
static const NPH_CVAR_INT(k_workers, "rt.workers", 0, 0, 64,
                          "job workers (0: one per core but one)", NPH_CVAR_FLAG_LATCHED);
static const NPH_CVAR_INT(k_sim_rate, "sim.rate", 240, 1, NPH_FIXED_CLOCK_MAX_RATE,
                          "simulation steps per second", NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_INT(k_sim_max_steps, "sim.max_steps", 8, 1, ENGINE_MAX_STEPS,
                          "steps one frame may take; time beyond is dropped", NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_FLOAT(k_sim_time_scale, "sim.time_scale", 1.0, 0.0, 100.0,
                            "simulated seconds per real second", NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_BOOL(k_vsync, "r.vsync", 1, "wait for the display's refresh",
                           CVAR_SAVED_LATCHED);
static const NPH_CVAR_INT(k_width, "r.width", 1280, 320, 16384, "window width", CVAR_SAVED_LATCHED);
static const NPH_CVAR_INT(k_height, "r.height", 720, 240, 16384, "window height",
                          CVAR_SAVED_LATCHED);
static const NPH_CVAR_INT(k_pipeline, "r.pipeline", 1, 0, 1,
                          "0: the forward reference renderer, 1: the GPU-driven production one",
                          NPH_CVAR_FLAG_SAVED);
static const NPH_CVAR_BOOL(k_occlusion, "r.occlusion", 1,
                           "occlusion culling against the depth pyramid (production)",
                           NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_BOOL(k_cone_culling, "r.cone_culling", 1,
                           "back-facing cluster culling by normal cone (production)",
                           NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_BOOL(k_sw_raster, "r.sw_raster", 1,
                           "small triangles through the compute rasteriser (production)",
                           NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_BOOL(k_hot_reload, "game.hot_reload", DEV_DEFAULT,
                           "swap in new builds of the game module", NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_INT(k_reload_poll_ms, "game.reload_poll_ms", 250, 10, 10000,
                          "how often the game module's file is checked", NPH_CVAR_FLAG_NONE);
static const NPH_CVAR_BOOL(k_prof_enabled, "prof.enabled", DEV_DEFAULT,
                           "record CPU profiling zones", NPH_CVAR_FLAG_LATCHED);
static const NPH_CVAR_INT(k_prof_history, "prof.history", 240, 16, 4096,
                          "frames of profile history", NPH_CVAR_FLAG_LATCHED);

// The engine's cvars are its members (two engines may coexist), each
// starting as a copy of its static template.
static NphResult register_cvars(NphEngine *e)
{
    EngineCvars *cv = &e->cv;
    NphCvar *const slots[] = {
        &cv->max_frames,    &cv->pipelined,      &cv->max_fps,        &cv->lockstep,
        &cv->max_entities,  &cv->world_events,   &cv->workers,        &cv->sim_rate,
        &cv->sim_max_steps, &cv->sim_time_scale, &cv->vsync,          &cv->width,
        &cv->height,        &cv->hot_reload,     &cv->reload_poll_ms, &cv->prof_enabled,
        &cv->prof_history,  &cv->pipeline,       &cv->occlusion,      &cv->cone_culling,
        &cv->sw_raster,
    };
    const NphCvar *const templates[] = {
        &k_max_frames,   &k_pipelined,    &k_max_fps,  &k_lockstep,      &k_max_entities,
        &k_world_events, &k_workers,      &k_sim_rate, &k_sim_max_steps, &k_sim_time_scale,
        &k_vsync,        &k_width,        &k_height,   &k_hot_reload,    &k_reload_poll_ms,
        &k_prof_enabled, &k_prof_history, &k_pipeline, &k_occlusion,     &k_cone_culling,
        &k_sw_raster,
    };
    NPH_STATIC_ASSERT(NPH_ARRAY_COUNT(slots) == NPH_ARRAY_COUNT(templates),
                      "one template per engine cvar");
    for (u32 i = 0; i < NPH_ARRAY_COUNT(slots); ++i) {
        *slots[i] = *templates[i];
        NPH_TRY(nph_cvar_register(&e->cvars, slots[i]));
    }
    return NPH_RESULT_OK;
}

static NphResult apply_config_file(NphEngine *e, const char *path, bool optional)
{
    NphAllocator *scratch = engine_alloc(e, ENGINE_ALLOC_ENGINE);
    void *data = NULL;
    u64 size = 0U;
    NphResult r = nph_file_read_all(path, scratch, &data, &size);
    if (r == NPH_RESULT_NOT_FOUND && optional) {
        return NPH_RESULT_OK;
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", path, nph_result_string(r));
        return r;
    }
    u32 line = 0U;
    r = nph_cvar_apply_config(&e->cvars, nph_str((const char *)data, size), &line);
    nph_file_free_all(scratch, data, size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s:%u: not a \"name = value\" line", path, line);
    }
    return r;
}

static NphResult apply_configuration(NphEngine *e)
{
    const NphEngineConfig *c = &e->config;
    if (c->user_config != NULL) {
        NPH_TRY(apply_config_file(e, c->user_config, true));
    }
    for (u32 i = 0; i < c->config_count; ++i) {
        NPH_TRY(apply_config_file(e, c->configs[i], false));
    }
    for (u32 i = 0; i < c->assignment_count; ++i) {
        const NphResult r =
            nph_cvar_apply_assignment(&e->cvars, nph_str_from_cstr(c->assignments[i]));
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", c->assignments[i], nph_result_string(r));
            return r;
        }
    }
    if (c->has_max_frames) {
        NPH_TRY(nph_cvar_force_from_string(&e->cvars, &e->cv.max_frames,
                                           NPH_STR("0"))); // reset, then the option
        NPH_TRY(nph_cvar_set_int(&e->cvars, &e->cv.max_frames, (i64)c->max_frames));
    }
    return NPH_RESULT_OK;
}

static void write_user_config(NphEngine *e)
{
    if (e->config.user_config == NULL || !e->cvars_ready) {
        return;
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, engine_alloc(e, ENGINE_ALLOC_ENGINE));
    NphResult r = nph_cvar_write_config(&e->cvars, &sb);
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(e->config.user_config, sb.data, sb.len);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "%s: %s", e->config.user_config, nph_result_string(r));
    }
    nph_sb_free(&sb);
}

// ---------------------------------------------------------------------------
// Worlds
// ---------------------------------------------------------------------------

static NphResult world_desc_create(NphEngine *e, u32 capacity, const NphWorld *registry_from,
                                   NphWorld **out)
{
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = engine_alloc(e, ENGINE_ALLOC_WORLD);
    wd.max_entities = capacity;
    wd.event_capacity = (u32)nph_cvar_get_int(&e->cv.world_events);
    wd.registry_from = registry_from;
    return nph_world_create(&wd, out);
}

NphResult engine_world_create(NphEngine *e, const GameComponentSet *set, NphWorld **out)
{
    *out = NULL;
    NphWorld *w = NULL;
    NPH_TRY(world_desc_create(e, (u32)nph_cvar_get_int(&e->cv.max_entities), NULL, &w));
    NphResult r = nph_runtime_register_components(w, &e->ids);
    for (u32 i = 0; r == NPH_RESULT_OK && set != NULL && i < set->count; ++i) {
        NphComponentId id;
        r = nph_world_register_component(w, &set->items[i].desc, &id);
    }
    if (r != NPH_RESULT_OK) {
        nph_world_destroy(w);
        return r;
    }
    *out = w;
    return NPH_RESULT_OK;
}

NphResult engine_world_copy(NphEngine *e, const NphWorld *src, const GameComponentSet *set,
                            NphWorld **out)
{
    *out = NULL;
    NPH_PROFILE_BEGIN(set != NULL ? "runtime.world_migrate" : "runtime.world_copy");
    NphAllocator *scratch = engine_alloc(e, ENGINE_ALLOC_ENGINE);
    NphWorld *dst = NULL;
    NphResult r = set != NULL ? engine_world_create(e, set, &dst)
                              : world_desc_create(e, nph_world_entity_capacity(src), src, &dst);
    if (r == NPH_RESULT_OK) {
        r = nph_world_adopt_handles(dst, src);
    }
    const u32 n = nph_world_entity_count(src);
    NphEntity *order = NULL;
    if (r == NPH_RESULT_OK) {
        order = NPH_ALLOC_ARRAY(scratch, NphEntity, nph_max_u32(n, 1U));
        r = order != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    NphSceneResult res;
    memset(&res, 0, sizeof res);
    if (r == NPH_RESULT_OK) {
        NphSceneSaveDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.scratch = scratch;
        sd.out_entities = order;
        sd.out_capacity = n;
        NphSceneLoadDesc ld;
        memset(&ld, 0, sizeof ld);
        ld.scratch = scratch;
        ld.handles = order;
        if (set != NULL) {
            // Fields matched by name; components the new build dropped go.
            NphStrBuilder sb;
            nph_sb_init(&sb, scratch);
            r = nph_scene_save_text(src, &sd, &sb, &res);
            if (r == NPH_RESULT_OK) {
                ld.handle_count = res.entity_count;
                ld.skip_unknown_components = 1;
                r = nph_scene_load_text(dst, nph_sb_str(&sb), &ld, &res);
            }
            nph_sb_free(&sb);
        } else {
            NphBinWriter bw;
            nph_bin_writer_init(&bw, scratch);
            r = nph_scene_save_binary(src, &sd, &bw, &res);
            if (r == NPH_RESULT_OK) {
                ld.handle_count = res.entity_count;
                r = nph_scene_load_binary(dst, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw),
                                          &ld, &res);
            }
            nph_bin_writer_free(&bw);
        }
        if (r != NPH_RESULT_OK && res.error[0] != '\0') {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "copying the world: %s", res.error);
        }
    }
    if (order != NULL) {
        NPH_FREE_ARRAY(scratch, NphEntity, order, nph_max_u32(n, 1U));
    }
    if (r != NPH_RESULT_OK) {
        nph_world_destroy(dst);
    } else {
        *out = dst;
    }
    NPH_PROFILE_END();
    return r;
}

static void forget_names(NphEngine *e)
{
    if (e->profiler != NULL) {
        nph_profiler_forget_names(e->profiler);
    }
}

static NphResult schedule_create(NphEngine *e, bool with_game)
{
    NphScheduleDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.world = e->world;
    sd.jobs = e->jobs;
    sd.alloc = engine_alloc(e, ENGINE_ALLOC_WORLD);
    NPH_TRY(nph_schedule_create(&sd, &e->schedule));
    const NphSystemDesc td = nph_transform_system_desc(e->transforms);
    NphResult r = nph_schedule_add_system(e->schedule, &td, NULL);
    if (r == NPH_RESULT_OK && with_game) {
        r = game_add_systems(e, e->schedule);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_schedule_build(e->schedule);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the schedule: %s", nph_schedule_error(e->schedule));
        }
    }
    return r;
}

NphResult engine_schedule_rebuild(NphEngine *e)
{
    // System names belong to the schedule; the profiler forgets them first.
    forget_names(e);
    nph_schedule_destroy(e->schedule);
    e->schedule = NULL;
    if (e->world == NULL || e->transforms == NULL) {
        return NPH_RESULT_OK;
    }
    const NphResult r = schedule_create(e, true);
    if (r == NPH_RESULT_OK) {
        return NPH_RESULT_OK;
    }
    // The game's systems do not schedule: the world still updates its
    // transforms, and the failure is the game's to fix.
    forget_names(e);
    nph_schedule_destroy(e->schedule);
    e->schedule = NULL;
    NPH_TRY(schedule_create(e, false));
    return r;
}

NphResult engine_world_activate(NphEngine *e, NphWorld *world, bool edit, NphWorld **out_previous)
{
    NphWorld *old_active = e->world;
    NphWorld *previous = edit ? e->edit_world : e->play_world;
    if (edit) {
        e->edit_world = world;
    } else {
        e->play_world = world;
    }
    e->world = e->play_world != NULL ? e->play_world : e->edit_world;
    *out_previous = previous;
    if (e->world == old_active) {
        return NPH_RESULT_OK;
    }
    // Everything that queries the active world follows it.
    forget_names(e);
    nph_schedule_destroy(e->schedule);
    e->schedule = NULL;
    nph_transform_system_destroy(e->transforms);
    e->transforms = NULL;
    NphResult r = NPH_RESULT_OK;
    if (e->world != NULL) {
        r = nph_transform_system_create(e->world, engine_alloc(e, ENGINE_ALLOC_WORLD),
                                        &e->transforms);
        if (r == NPH_RESULT_OK) {
            r = engine_schedule_rebuild(e);
        }
        if (r == NPH_RESULT_OK && e->scene_renderer != NULL) {
            r = nph_scene_renderer_set_world(e->scene_renderer, e->world);
        }
    }
    if (e->hooks.world_replaced != NULL && old_active != NULL) {
        e->hooks.world_replaced(e, old_active, e->world, e->hooks.user);
    }
    return r;
}

// ---------------------------------------------------------------------------
// The simulation
// ---------------------------------------------------------------------------

static void sim_job(void *user)
{
    NphEngine *e = (NphEngine *)user;
    NPH_PROFILE_BEGIN("runtime.simulate");
    const f64 dt = nph_fixed_clock_step_seconds(&e->clock);
    for (u32 i = 0; i < e->sim_steps; ++i) {
        e->game.input = &e->step_inputs[i];
        const NphResult r = nph_schedule_run(e->schedule, dt);
        e->game.step += 1U;
        if (r != NPH_RESULT_OK) {
            e->sim_result = r;
            break;
        }
    }
    NPH_PROFILE_END();
}

NphResult engine_sim_wait(NphEngine *e)
{
    if (e->sim_in_flight) {
        nph_job_wait(e->jobs, &e->sim_counter);
        e->sim_in_flight = false;
    }
    const NphResult r = e->sim_result;
    e->sim_result = NPH_RESULT_OK;
    return r;
}

// Decides this frame's steps and starts them.
static NphResult sim_kick(NphEngine *e, u64 now)
{
    NphFixedClock *clock = &e->clock;
    const u32 rate = (u32)nph_cvar_get_int(&e->cv.sim_rate);
    if (clock->rate_hz != rate) {
        nph_fixed_clock_set_rate(clock, rate);
    }
    clock->max_steps = (u32)nph_cvar_get_int(&e->cv.sim_max_steps);
    nph_fixed_clock_set_time_scale(clock, nph_cvar_get_float(&e->cv.sim_time_scale));
    // The clock always advances, so resuming does not owe the paused time.
    u32 steps = nph_fixed_clock_advance(clock, now);
    if (nph_cvar_get_bool(&e->cv.lockstep)) {
        steps = 1U;
    }
    if (!e->playing) {
        steps = 0U;
    } else if (e->paused) {
        steps = e->step_once ? 1U : 0U;
    }
    e->step_once = false;
    steps = nph_min_u32(steps, ENGINE_MAX_STEPS);
    e->stats.steps = steps;
    e->stats.dropped_ns = clock->dropped_ns;
    if (steps == 0U || e->schedule == NULL) {
        return NPH_RESULT_OK;
    }
    for (u32 i = 0; i < steps; ++i) {
        nph_sim_input_take(&e->sim_acc, &e->step_inputs[i]);
    }
    e->sim_steps = steps;
    e->sim_result = NPH_RESULT_OK;
    e->stats.total_steps += steps;
    if (!nph_cvar_get_bool(&e->cv.pipelined)) {
        sim_job(e);
        return NPH_RESULT_OK;
    }
    const NphJobDecl decl = {sim_job, e, "runtime.simulate"};
    NPH_TRY(nph_job_run(e->jobs, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &e->sim_counter));
    e->sim_in_flight = true;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Play
// ---------------------------------------------------------------------------

NphResult nph_engine_play_begin(NphEngine *e, bool copy)
{
    NPH_ASSERT(e != NULL);
    if (e->playing) {
        return NPH_RESULT_INVALID_STATE;
    }
    NPH_TRY(engine_sim_wait(e));
    if (copy) {
        NphWorld *play = NULL;
        NPH_TRY(engine_world_copy(e, e->edit_world, NULL, &play));
        NphWorld *previous = NULL;
        const NphResult r = engine_world_activate(e, play, false, &previous);
        NPH_ASSERT(previous == NULL);
        if (r != NPH_RESULT_OK) {
            NphWorld *undo = NULL;
            NPH_IGNORE_RESULT(engine_world_activate(e, NULL, false, &undo));
            nph_world_destroy(undo);
            return r;
        }
    }
    nph_fixed_clock_reset(&e->clock);
    nph_sim_input_reset(&e->sim_acc);
    e->playing = true;
    e->paused = false;
    e->stats.total_steps = 0U;
    const NphResult r = game_play_begin(e);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "play_begin failed: %s", nph_result_string(r));
        nph_engine_play_end(e);
    }
    return r;
}

void nph_engine_play_end(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    if (!e->playing) {
        return;
    }
    NPH_IGNORE_RESULT(engine_sim_wait(e));
    game_play_end(e);
    e->playing = false;
    e->paused = false;
    if (e->play_world != NULL) {
        NphWorld *previous = NULL;
        NPH_IGNORE_RESULT(engine_world_activate(e, NULL, false, &previous));
        nph_world_destroy(previous);
    }
}

bool nph_engine_is_playing(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->playing;
}

void nph_engine_set_paused(NphEngine *e, bool paused)
{
    NPH_ASSERT(e != NULL);
    e->paused = paused;
}

bool nph_engine_is_paused(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->paused;
}

void nph_engine_step_once(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    e->step_once = true;
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

static void poll_events(NphEngine *e)
{
    nph_input_begin_frame(e->input);
    if (e->window != NULL) {
        nph_window_poll_events();
        NphWindowEvent ev;
        while (nph_window_pop_event(e->window, &ev)) {
            if (e->hooks.event != NULL && e->hooks.event(e, &ev, e->hooks.user)) {
                continue;
            }
            nph_input_process_event(e->input, &ev);
        }
        nph_input_poll_gamepads(e->input);
        if (nph_window_should_close(e->window)) {
            e->quit = true;
        }
    }
    nph_input_end_frame(e->input);
    nph_sim_input_accumulate(&e->sim_acc, e->input);
}

// The view to render: the override, or the world's camera. The r.*
// cvars choose the pipeline and its culling for either.
static void choose_view(NphEngine *e)
{
    if (e->has_view_override) {
        e->view = e->view_override;
    } else if (e->scene_renderer != NULL) {
        (void)nph_scene_renderer_view(e->scene_renderer, &e->view);
    }
    e->view.pipeline = (NphRenderPipeline)nph_cvar_get_int(&e->cv.pipeline);
    e->view.vis_flags = 0U;
    if (!nph_cvar_get_bool(&e->cv.occlusion)) {
        e->view.vis_flags |= (u32)NPH_RENDER_VIS_NO_OCCLUSION;
    }
    if (!nph_cvar_get_bool(&e->cv.cone_culling)) {
        e->view.vis_flags |= (u32)NPH_RENDER_VIS_NO_CONE;
    }
    if (!nph_cvar_get_bool(&e->cv.sw_raster)) {
        e->view.vis_flags |= (u32)NPH_RENDER_VIS_NO_SW_RASTER;
    }
}

// The sync point: the world is quiescent from here to the simulation's
// start.
static NphResult sync_point(NphEngine *e, f64 dt)
{
    NPH_PROFILE_BEGIN("runtime.sync");
    const u64 t0 = nph_time_now_ns();
    NphResult r = engine_sim_wait(e);
    e->stats.sim_wait_ms = nph_time_ns_to_ms(nph_time_now_ns() - t0);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the simulation failed (%s); paused", nph_result_string(r));
        e->paused = true;
    }
    if (nph_cvar_get_bool(&e->cv.hot_reload)) {
        bool reloaded = false;
        NPH_IGNORE_RESULT(game_poll_reload(e, false, &reloaded)); // logged; the old build runs on
    }
    r = NPH_RESULT_OK;
    if (e->dev != NULL) {
        r = nph_gpu_frame_begin(e->dev);
    }
    if (r == NPH_RESULT_OK) {
        game_frame(e, dt);
        if (e->hooks.sync != NULL) {
            e->hooks.sync(e, e->hooks.user);
        }
        r = nph_transform_system_update(e->transforms, e->jobs);
    }
    if (r == NPH_RESULT_OK && e->scene_renderer != NULL) {
        r = nph_scene_renderer_sync(e->scene_renderer, &e->stats.scene);
    }
    nph_world_clear_events(e->world);
    if (r == NPH_RESULT_OK && e->renderer != NULL) {
        nph_render_debug_flip(e->renderer);
    }
    choose_view(e);
    e->stats.sync_ms = nph_time_ns_to_ms(nph_time_now_ns() - t0);
    NPH_PROFILE_END();
    return r;
}

static NphResult render(NphEngine *e)
{
    if (e->renderer == NULL) {
        return NPH_RESULT_OK;
    }
    NPH_PROFILE_BEGIN("runtime.render");
    const u64 t0 = nph_time_now_ns();
    NphGpuImage backbuffer = NPH_GPU_IMAGE_NULL;
    NphResult r = NPH_RESULT_OK;
    if (e->swapchain != NULL) {
        r = nph_gpu_swapchain_acquire(e->swapchain, &backbuffer);
        if (r == NPH_RESULT_EMPTY) { // minimised: nothing to draw into
            NPH_PROFILE_END();
            return nph_gpu_frame_end(e->dev);
        }
    }
    NphGpuImage scene = e->swapchain != NULL ? backbuffer : e->offscreen;
    NphGpuImage custom = NPH_GPU_IMAGE_NULL;
    if (e->hooks.scene_target != NULL && e->hooks.scene_target(e, &custom, e->hooks.user)) {
        scene = custom;
    }
    if (r == NPH_RESULT_OK) {
        NPH_IGNORE_RESULT(nph_render_gpu_profiler_update(e->gpu_profiler, e->graph));
        nph_gpu_rg_begin(e->graph);
        NphGpuRgImage scene_rg = NPH_GPU_RG_IMAGE_NULL;
        r = nph_render_frame(e->renderer, &e->view, e->graph, scene, &scene_rg, &e->stats.render);
        NphGpuRgImage back_rg = NPH_GPU_RG_IMAGE_NULL;
        if (r == NPH_RESULT_OK && e->swapchain != NULL) {
            back_rg = scene.value == backbuffer.value
                          ? scene_rg
                          : nph_gpu_rg_import_image(e->graph, backbuffer);
        }
        if (r == NPH_RESULT_OK && e->hooks.render != NULL) {
            e->hooks.render(e, e->graph, back_rg, scene_rg, e->hooks.user);
        }
        if (r == NPH_RESULT_OK && e->swapchain != NULL) {
            nph_gpu_rg_present(e->graph, back_rg, e->swapchain);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_gpu_rg_compile(e->graph);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_gpu_rg_execute(e->graph);
        }
        if (r == NPH_RESULT_OK && e->swapchain != NULL) {
            r = nph_gpu_swapchain_present(e->swapchain);
        }
    }
    const NphResult er = nph_gpu_frame_end(e->dev);
    e->stats.render_ms = nph_time_ns_to_ms(nph_time_now_ns() - t0);
    NPH_PROFILE_END();
    return r != NPH_RESULT_OK ? r : er;
}

static void limit_rate(NphEngine *e, u64 frame_start)
{
    const i64 max_fps = nph_cvar_get_int(&e->cv.max_fps);
    if (max_fps <= 0) {
        return;
    }
    const u64 budget = NPH_TIME_NS_PER_S / (u64)max_fps;
    const u64 spent = nph_time_now_ns() - frame_start;
    if (spent < budget) {
        nph_thread_sleep_ns(budget - spent);
    }
}

NphResult nph_engine_frame(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    const u64 start = nph_time_now_ns();
    NPH_PROFILE_FRAME(e->stats.frame);
    const f64 dt = e->last_frame_ns != 0U ? nph_time_ns_to_s(start - e->last_frame_ns) : 0.0;
    e->last_frame_ns = start;
    poll_events(e);
    NphResult r = sync_point(e, dt);
    if (r == NPH_RESULT_OK) {
        r = sim_kick(e, nph_time_now_ns());
    }
    if (r == NPH_RESULT_OK) {
        r = render(e);
    } else if (e->dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_frame_end(e->dev));
    }
    if (e->profiler != NULL) {
        NPH_IGNORE_RESULT(nph_profiler_collect(e->profiler));
    }
    limit_rate(e, start);
    e->stats.frame += 1U;
    e->stats.frame_ms = nph_time_ns_to_ms(nph_time_now_ns() - start);
    const i64 max_frames = nph_cvar_get_int(&e->cv.max_frames);
    if (max_frames > 0 && e->stats.frame >= (u64)max_frames) {
        e->quit = true;
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "frame %llu failed: %s", (unsigned long long)e->stats.frame,
                      nph_result_string(r));
    }
    return r;
}

NphResult nph_engine_run(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    NphResult r = NPH_RESULT_OK;
    while (!e->quit && r == NPH_RESULT_OK) {
        r = nph_engine_frame(e);
    }
    const NphResult w = engine_sim_wait(e);
    return r != NPH_RESULT_OK ? r : w;
}

NphResult nph_engine_wait(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    const NphResult r = engine_sim_wait(e);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the simulation failed (%s); paused", nph_result_string(r));
        e->paused = true;
    }
    return r;
}

void nph_engine_quit(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    e->quit = true;
}

bool nph_engine_should_quit(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->quit;
}

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------

NphResult nph_engine_load_scene(NphEngine *e, const char *path)
{
    NPH_ASSERT(e != NULL && path != NULL);
    if (e->playing) {
        return NPH_RESULT_INVALID_STATE;
    }
    NPH_TRY(engine_sim_wait(e));
    NphAllocator *scratch = engine_alloc(e, ENGINE_ALLOC_ENGINE);
    void *data = NULL;
    u64 size = 0U;
    NphResult r = nph_file_read_all(path, scratch, &data, &size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", path, nph_result_string(r));
        return r;
    }
    u32 magic = 0U;
    if (size >= sizeof magic) {
        memcpy(&magic, data, sizeof magic);
    }
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.scratch = scratch;
    NphSceneResult res;
    r = magic == NPH_SCENE_BINARY_MAGIC
            ? nph_scene_load_binary(e->edit_world, data, size, &ld, &res)
            : nph_scene_load_text(e->edit_world, nph_str((const char *)data, size), &ld, &res);
    nph_file_free_all(scratch, data, size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", path, res.error);
        return r;
    }
    NPH_LOG_INFO(&NPH_LOG_RUNTIME, "%s: %u entities", path, res.entity_count);
    return NPH_RESULT_OK;
}

NphResult nph_engine_save_scene(NphEngine *e, const char *path)
{
    NPH_ASSERT(e != NULL && path != NULL);
    NPH_TRY(engine_sim_wait(e));
    NphStrBuilder sb;
    nph_sb_init(&sb, engine_alloc(e, ENGINE_ALLOC_ENGINE));
    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.scratch = engine_alloc(e, ENGINE_ALLOC_ENGINE);
    NphSceneResult res;
    NphResult r = nph_scene_save_text(e->edit_world, &sd, &sb, &res);
    // Written next to the file and renamed over it, so a failure never
    // leaves half a scene.
    char temp[GAME_PATH_MAX];
    const int n = snprintf(temp, sizeof temp, "%s.tmp", path);
    if (r == NPH_RESULT_OK && (n <= 0 || (u64)n >= sizeof temp)) {
        r = NPH_RESULT_OVERFLOW;
    }
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(temp, sb.data, sb.len);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_fs_rename(temp, path);
    }
    nph_sb_free(&sb);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", path, nph_result_string(r));
        return r;
    }
    NPH_LOG_INFO(&NPH_LOG_RUNTIME, "%s: %u entities saved", path, res.entity_count);
    return NPH_RESULT_OK;
}

NphResult nph_engine_clear_scene(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    if (e->playing) {
        return NPH_RESULT_INVALID_STATE;
    }
    NPH_TRY(engine_sim_wait(e));
    nph_world_clear(e->edit_world);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

// Copies a console argument into a NUL-terminated path.
static bool arg_path(NphConsole *c, const NphStr *args, u32 count, char *out, u64 cap)
{
    if (count != 2U || args[1].len == 0U || args[1].len >= cap) {
        nph_console_printf(c, "usage: %.*s <path>", NPH_STR_ARG(args[0]));
        return false;
    }
    memcpy(out, args[1].data, (size_t)args[1].len);
    out[args[1].len] = '\0';
    return true;
}

static NphResult cmd_quit(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(c);
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    nph_engine_quit((NphEngine *)user);
    return NPH_RESULT_OK;
}

static NphResult cmd_play(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(c);
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    return nph_engine_play_begin((NphEngine *)user, true);
}

static NphResult cmd_stop(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(c);
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    nph_engine_play_end((NphEngine *)user);
    return NPH_RESULT_OK;
}

static NphResult cmd_pause(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    NphEngine *e = (NphEngine *)user;
    nph_engine_set_paused(e, !e->paused);
    nph_console_printf(c, e->paused ? "paused" : "running");
    return NPH_RESULT_OK;
}

static NphResult cmd_step(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(c);
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    nph_engine_step_once((NphEngine *)user);
    return NPH_RESULT_OK;
}

static NphResult cmd_reload(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    bool reloaded = false;
    NPH_TRY(nph_engine_reload_game((NphEngine *)user, &reloaded));
    nph_console_printf(c, reloaded ? "reloaded" : "no newer build");
    return NPH_RESULT_OK;
}

static NphResult cmd_scene_load(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    char path[GAME_PATH_MAX];
    if (!arg_path(c, args, count, path, sizeof path)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return nph_engine_load_scene((NphEngine *)user, path);
}

static NphResult cmd_scene_save(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    char path[GAME_PATH_MAX];
    if (!arg_path(c, args, count, path, sizeof path)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return nph_engine_save_scene((NphEngine *)user, path);
}

static NphResult cmd_trace(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NphEngine *e = (NphEngine *)user;
    char path[GAME_PATH_MAX];
    if (!arg_path(c, args, count, path, sizeof path)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (e->profiler == NULL) {
        nph_console_printf(c, "profiling is off (prof.enabled)");
        return NPH_RESULT_INVALID_STATE;
    }
    return nph_profiler_save_chrome_trace(e->profiler, 0U, engine_alloc(e, ENGINE_ALLOC_ENGINE),
                                          path);
}

static NphResult cmd_stats(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(args);
    NPH_UNUSED(count);
    const NphEngine *e = (const NphEngine *)user;
    const NphEngineFrameStats *s = &e->stats;
    nph_console_printf(c,
                       "frame %llu: %.2f ms (sync %.2f, sim wait %.2f, render %.2f); %u steps, "
                       "%llu in all; %u entities, %u instances, %u lights",
                       (unsigned long long)s->frame, s->frame_ms, s->sync_ms, s->sim_wait_ms,
                       s->render_ms, s->steps, (unsigned long long)s->total_steps,
                       nph_world_entity_count(e->world), s->scene.instances, s->scene.lights);
    return NPH_RESULT_OK;
}

typedef struct EngineCommand {
    const char *name;
    const char *help;
    NphConsoleCommandFn fn;
} EngineCommand;

static const EngineCommand g_commands[] = {
    {"quit", "end the run after this frame", cmd_quit},
    {"play", "play a copy of the world", cmd_play},
    {"stop", "stop playing", cmd_stop},
    {"pause", "pause or resume the simulation", cmd_pause},
    {"step", "simulate one step while paused", cmd_step},
    {"reload", "swap in a newer build of the game module now", cmd_reload},
    {"scene.load", "load a scene file into the world: scene.load <path>", cmd_scene_load},
    {"scene.save", "save the world as a text scene: scene.save <path>", cmd_scene_save},
    {"trace", "write a Chrome trace of the recent frames: trace <path>", cmd_trace},
    {"stats", "print the last frame's statistics", cmd_stats},
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static const char *const g_alloc_tags[ENGINE_ALLOC_COUNT] = {"engine", "world", "render", "assets",
                                                             "game"};

static NphResult start_profiling(NphEngine *e)
{
    if (!nph_cvar_get_bool(&e->cv.prof_enabled) && !nph_profile_is_initialized()) {
        return NPH_RESULT_OK;
    }
    if (!nph_profile_is_initialized()) {
        NphProfileConfig pc;
        memset(&pc, 0, sizeof pc);
        pc.alloc = engine_alloc(e, ENGINE_ALLOC_ENGINE);
        const NphResult r = nph_profile_init(&pc);
        if (r == NPH_RESULT_UNSUPPORTED) {
            return NPH_RESULT_OK; // a build without the profiler
        }
        NPH_TRY(r);
        e->owns_profiling = true;
        e->profile_attached = nph_profile_thread_attach("main") == NPH_RESULT_OK;
    }
    NphProfilerDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.alloc = engine_alloc(e, ENGINE_ALLOC_ENGINE);
    pd.history_frames = (u32)nph_cvar_get_int(&e->cv.prof_history);
    NPH_TRY(nph_profiler_create(&pd, &e->profiler));
    for (u32 i = 0; i < ENGINE_ALLOC_COUNT; ++i) {
        NPH_TRY(nph_profiler_track_memory(e->profiler, &e->tracked[i]));
    }
    return NPH_RESULT_OK;
}

static NphResult start_content(NphEngine *e, const NphEngineDesc *desc)
{
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = engine_alloc(e, ENGINE_ALLOC_ENGINE);
    jd.worker_count = (u32)nph_cvar_get_int(&e->cv.workers);
    NPH_TRY(nph_job_system_create(&jd, &e->jobs));
    NPH_TRY(nph_vfs_create(engine_alloc(e, ENGINE_ALLOC_ASSETS), &e->vfs));
    for (u32 i = 0; i < e->config.pack_count; ++i) {
        const NphResult r = nph_vfs_mount_pack(e->vfs, nph_str_empty(), e->config.packs[i], NULL);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", e->config.packs[i], nph_result_string(r));
            return r;
        }
    }
    for (u32 i = 0; i < e->config.content_count; ++i) {
        const NphResult r =
            nph_vfs_mount_directory(e->vfs, nph_str_empty(), e->config.contents[i], NULL);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", e->config.contents[i], nph_result_string(r));
            return r;
        }
    }
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = engine_alloc(e, ENGINE_ALLOC_ASSETS);
    ld.vfs = e->vfs;
    ld.jobs = e->jobs;
    ld.resolve = desc->resolve;
    ld.resolve_user = desc->resolve_user;
    return nph_asset_loader_create(&ld, &e->loader);
}

static NphResult start_window(NphEngine *e)
{
    if (!nph_window_system_is_initialized()) {
        const NphResult r = nph_window_system_init();
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "no window system (%s)", nph_result_string(r));
            return NPH_RESULT_UNSUPPORTED;
        }
        e->owns_window_system = true;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = e->app_name;
    wd.width = (u32)nph_cvar_get_int(&e->cv.width);
    wd.height = (u32)nph_cvar_get_int(&e->cv.height);
    return nph_window_create(engine_alloc(e, ENGINE_ALLOC_ENGINE), &wd, &e->window);
}

static NphResult start_gpu(NphEngine *e)
{
    NphAllocator *alloc = engine_alloc(e, ENGINE_ALLOC_RENDER);
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = alloc;
    dd.app_name = e->app_name;
    dd.headless = e->mode == NPH_ENGINE_MODE_OFFSCREEN;
    NphResult r = nph_gpu_device_create(&dd, &e->dev);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "no Vulkan device (%s)", nph_result_string(r));
        return r == NPH_RESULT_UNSUPPORTED ? r : NPH_RESULT_UNSUPPORTED;
    }
    if (e->mode == NPH_ENGINE_MODE_WINDOWED) {
        NphGpuSwapchainDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.window = e->window;
        sd.vsync = nph_cvar_get_bool(&e->cv.vsync);
        // UNORM: the renderer encodes sRGB itself, and a UI drawn over the
        // scene (the editor's) writes its colours as they are.
        sd.prefer_srgb = 0;
        sd.name = "engine swapchain";
        NPH_TRY(nph_gpu_swapchain_create(e->dev, &sd, &e->swapchain));
    } else {
        NphGpuImageDesc id;
        memset(&id, 0, sizeof id);
        id.type = NPH_GPU_IMAGE_2D;
        id.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
        id.width = e->offscreen_width;
        id.height = e->offscreen_height;
        id.mip_count = 1U;
        id.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT |
                   (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
        id.name = "engine offscreen target";
        NPH_TRY(nph_gpu_image_create(e->dev, &id, &e->offscreen));
    }
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = alloc;
    gd.jobs = e->jobs;
    gd.name = "engine frame";
    NPH_TRY(nph_gpu_render_graph_create(e->dev, &gd, &e->graph));
    NphRendererDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.alloc = alloc;
    rd.dev = e->dev;
    rd.shader_dir = e->shader_dir;
    rd.name = e->app_name;
    NPH_TRY(nph_renderer_create(&rd, &e->renderer));
    NphRenderGpuProfilerDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.alloc = alloc;
    pd.history = (u32)nph_cvar_get_int(&e->cv.prof_history);
    r = nph_render_gpu_profiler_create(&pd, &e->gpu_profiler);
    return r;
}

static NphResult start_world(NphEngine *e)
{
    NPH_TRY(engine_world_create(e, NULL, &e->edit_world));
    e->world = e->edit_world;
    NPH_TRY(
        nph_transform_system_create(e->world, engine_alloc(e, ENGINE_ALLOC_WORLD), &e->transforms));
    NPH_TRY(engine_schedule_rebuild(e));
    if (e->renderer != NULL) {
        NphSceneRendererDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.alloc = engine_alloc(e, ENGINE_ALLOC_RENDER);
        sd.renderer = e->renderer;
        sd.loader = e->loader;
        sd.world = e->world;
        NPH_TRY(nph_scene_renderer_create(&sd, &e->scene_renderer));
    }
    nph_fixed_clock_init(&e->clock, (u32)nph_cvar_get_int(&e->cv.sim_rate),
                         (u32)nph_cvar_get_int(&e->cv.sim_max_steps));
    nph_sim_input_reset(&e->sim_acc);
    nph_render_view_default(&e->view,
                            nph_render_camera_look_at(nph_dvec3(0.0, 2.0, 8.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 0.9F, 0.1F));
    return NPH_RESULT_OK;
}

static NphResult run_script(NphEngine *e, const char *path)
{
    NphAllocator *scratch = engine_alloc(e, ENGINE_ALLOC_ENGINE);
    void *data = NULL;
    u64 size = 0U;
    NphResult r = nph_file_read_all(path, scratch, &data, &size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: %s", path, nph_result_string(r));
        return r;
    }
    u32 line = 0U;
    r = nph_console_execute_script(e->console, nph_str((const char *)data, size), &line);
    nph_file_free_all(scratch, data, size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s:%u: %s", path, line, nph_result_string(r));
    }
    return r;
}

static NphResult startup(NphEngine *e, const NphEngineDesc *desc)
{
    NPH_TRY(nph_cvar_registry_init(&e->cvars, engine_alloc(e, ENGINE_ALLOC_ENGINE)));
    e->cvars_ready = true;
    NPH_TRY(register_cvars(e));
    NPH_TRY(nph_console_create(engine_alloc(e, ENGINE_ALLOC_ENGINE), &e->cvars, &e->console));
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_commands); ++i) {
        NPH_TRY(nph_console_register(e->console, g_commands[i].name, g_commands[i].help,
                                     g_commands[i].fn, e));
    }
    NPH_TRY(apply_configuration(e));
    NPH_TRY(start_profiling(e));
    NPH_TRY(start_content(e, desc));
    NphInputDesc id;
    memset(&id, 0, sizeof id);
    NPH_TRY(nph_input_create(engine_alloc(e, ENGINE_ALLOC_ENGINE), &id, &e->input));
    if (e->mode == NPH_ENGINE_MODE_WINDOWED) {
        NPH_TRY(start_window(e));
    }
    if (e->mode != NPH_ENGINE_MODE_HEADLESS) {
        NPH_TRY(start_gpu(e));
    }
    NPH_TRY(start_world(e));
    if (e->config.game != NULL) {
        NPH_TRY(game_load(e, e->config.game));
    }
    if (e->config.scene != NULL) {
        NPH_TRY(nph_engine_load_scene(e, e->config.scene));
    }
    if (desc->play_on_start) {
        NPH_TRY(nph_engine_play_begin(e, false));
    }
    if (e->config.script != NULL) {
        NPH_TRY(run_script(e, e->config.script));
    }
    NPH_LOG_INFO(&NPH_LOG_RUNTIME, "%s up: %s, %u job workers%s%s", e->app_name,
                 e->mode == NPH_ENGINE_MODE_WINDOWED    ? "windowed"
                 : e->mode == NPH_ENGINE_MODE_OFFSCREEN ? "offscreen"
                                                        : "headless",
                 nph_job_system_worker_count(e->jobs), e->game.api != NULL ? ", game " : "",
                 e->game.api != NULL ? e->game.api->name : "");
    return NPH_RESULT_OK;
}

NphResult nph_engine_create(const NphEngineDesc *desc, NphEngine **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    NphAllocator *backing = desc->alloc != NULL ? desc->alloc : nph_allocator_system();
    NphEngine *e = NPH_ALLOC_TYPE(backing, NphEngine);
    if (e == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    e->backing = backing;
    for (u32 i = 0; i < ENGINE_ALLOC_COUNT; ++i) {
        nph_tracked_allocator_init(&e->tracked[i], backing, g_alloc_tags[i]);
    }
    if (desc->config != NULL) {
        e->config = *desc->config;
    }
    e->mode = e->config.headless ? NPH_ENGINE_MODE_HEADLESS : desc->mode;
    e->hooks = desc->hooks;
    (void)snprintf(e->app_name, sizeof e->app_name, "%s",
                   desc->app_name != NULL ? desc->app_name : "Nepheline");
    (void)snprintf(e->shader_dir, sizeof e->shader_dir, "%s",
                   desc->shader_dir != NULL ? desc->shader_dir : NPH_SHADER_DIR);
    e->offscreen_width = desc->offscreen_width != 0U ? desc->offscreen_width : 1280U;
    e->offscreen_height = desc->offscreen_height != 0U ? desc->offscreen_height : 720U;
    e->game.engine = e;
    NPH_ARRAY_INIT(&e->game.cvars, engine_alloc(e, ENGINE_ALLOC_GAME));
    const NphResult r = startup(e, desc);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the engine did not start: %s", nph_result_string(r));
        nph_engine_destroy(e);
        return r;
    }
    *out = e;
    return NPH_RESULT_OK;
}

void nph_engine_destroy(NphEngine *e)
{
    if (e == NULL) {
        return;
    }
    NPH_IGNORE_RESULT(engine_sim_wait(e));
    if (e->dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(e->dev));
    }
    if (e->edit_world != NULL) {
        nph_engine_play_end(e);
        game_unload(e);
    }
    if (e->profiler != NULL && e->config.trace != NULL) {
        NPH_IGNORE_RESULT(nph_profiler_collect(e->profiler));
        const NphResult tr = nph_profiler_save_chrome_trace(
            e->profiler, 0U, engine_alloc(e, ENGINE_ALLOC_ENGINE), e->config.trace);
        if (tr != NPH_RESULT_OK) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME, "%s: %s", e->config.trace, nph_result_string(tr));
        }
    }
    write_user_config(e);
    // The worlds and what queries them.
    nph_scene_renderer_destroy(e->scene_renderer);
    forget_names(e);
    nph_schedule_destroy(e->schedule);
    nph_transform_system_destroy(e->transforms);
    nph_world_destroy(e->play_world);
    nph_world_destroy(e->edit_world);
    e->world = NULL;
    game_free(e);
    // The GPU, the window, input.
    nph_render_gpu_profiler_destroy(e->gpu_profiler);
    nph_renderer_destroy(e->renderer);
    nph_gpu_render_graph_destroy(e->graph);
    if (e->dev != NULL) {
        nph_gpu_image_destroy(e->dev, e->offscreen);
        nph_gpu_swapchain_destroy(e->swapchain);
        nph_gpu_device_destroy(e->dev);
    }
    nph_input_destroy(e->input);
    nph_window_destroy(e->window);
    if (e->owns_window_system) {
        nph_window_system_shutdown();
    }
    // Content and threads.
    nph_asset_loader_destroy(e->loader);
    nph_vfs_destroy(e->vfs);
    nph_job_system_destroy(e->jobs);
    // Profiling.
    nph_profiler_destroy(e->profiler);
    if (e->profile_attached) {
        nph_profile_thread_detach();
    }
    if (e->owns_profiling) {
        nph_profile_shutdown();
    }
    // Settings.
    nph_console_destroy(e->console);
    if (e->cvars_ready) {
        nph_cvar_registry_free(&e->cvars);
    }
    for (u32 i = 0; i < ENGINE_ALLOC_COUNT; ++i) {
        NphMemoryStats ms;
        nph_tracked_allocator_stats(&e->tracked[i], &ms);
        if (ms.bytes != 0U) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the engine leaked %llu bytes (%llu blocks) of \"%s\"",
                          (unsigned long long)ms.bytes, (unsigned long long)ms.allocations,
                          g_alloc_tags[i]);
        }
    }
    NPH_FREE_TYPE(e->backing, NphEngine, e);
}

// ---------------------------------------------------------------------------
// The game module
// ---------------------------------------------------------------------------

NphResult nph_engine_load_game(NphEngine *e, const char *path)
{
    NPH_ASSERT(e != NULL && path != NULL);
    NPH_TRY(engine_sim_wait(e));
    return game_load(e, path);
}

void nph_engine_unload_game(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    NPH_IGNORE_RESULT(engine_sim_wait(e));
    game_unload(e);
}

NphResult nph_engine_reload_game(NphEngine *e, bool *out_reloaded)
{
    NPH_ASSERT(e != NULL);
    bool reloaded = false;
    NPH_TRY(engine_sim_wait(e));
    const NphResult r = game_poll_reload(e, true, &reloaded);
    if (out_reloaded != NULL) {
        *out_reloaded = reloaded;
    }
    return r;
}

NphGame *nph_engine_game(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->game.api != NULL ? &e->game : NULL;
}

const NphGameApi *nph_engine_game_api(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->game.api;
}

u32 nph_engine_game_reloads(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->game.reloads;
}

// ---------------------------------------------------------------------------
// The view and accessors
// ---------------------------------------------------------------------------

void nph_engine_set_view(NphEngine *e, const NphRenderView *view)
{
    NPH_ASSERT(e != NULL);
    e->has_view_override = view != NULL;
    if (view != NULL) {
        e->view_override = *view;
    }
}

const NphRenderView *nph_engine_view(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return &e->view;
}

NphWorld *nph_engine_world(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->world;
}

NphWorld *nph_engine_edit_world(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->edit_world;
}

const NphRuntimeComponents *nph_engine_components(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return &e->ids;
}

NphEngineMode nph_engine_mode(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->mode;
}

NphAllocator *nph_engine_allocator(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return engine_alloc(e, ENGINE_ALLOC_ENGINE);
}

NphCvarRegistry *nph_engine_cvars(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return &e->cvars;
}

NphConsole *nph_engine_console(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->console;
}

NphJobSystem *nph_engine_jobs(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->jobs;
}

NphVfs *nph_engine_vfs(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->vfs;
}

NphAssetLoader *nph_engine_loader(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->loader;
}

NphInput *nph_engine_input(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->input;
}

NphSchedule *nph_engine_schedule(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->schedule;
}

NphProfiler *nph_engine_profiler(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->profiler;
}

NphWindow *nph_engine_window(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->window;
}

NphGpuDevice *nph_engine_device(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->dev;
}

NphGpuSwapchain *nph_engine_swapchain(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->swapchain;
}

NphRenderer *nph_engine_renderer(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->renderer;
}

NphRenderGpuProfiler *nph_engine_gpu_profiler(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->gpu_profiler;
}

NphSceneRenderer *nph_engine_scene_renderer(NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->scene_renderer;
}

NphGpuImage nph_engine_offscreen_target(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return e->offscreen;
}

const NphEngineFrameStats *nph_engine_frame_stats(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return &e->stats;
}

const NphFixedClock *nph_engine_clock(const NphEngine *e)
{
    NPH_ASSERT(e != NULL);
    return &e->clock;
}
