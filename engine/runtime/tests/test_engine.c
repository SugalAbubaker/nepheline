// SPDX-License-Identifier: Apache-2.0
// The engine without a game module (test_game_modules.c covers those):
// its lifecycle and leaks, configuration precedence and the user config,
// scenes, console commands, play without a game, the view, and an
// offscreen engine drawing the cooked showcase on the GPU.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/platform.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/scene/scene_io.h>

#include "test_gpu_support.h"

#include <stdio.h>
#include <string.h>

typedef struct EngineFixture {
    NphDebugAllocator da;
    NphEngineConfig config;
    char dir[512];
    NphEngine *engine;
} EngineFixture;

static void path_in(const EngineFixture *f, const char *name, char *out, u64 cap)
{
    (void)snprintf(out, (size_t)cap, "%s/%s", f->dir, name);
}

static void prepare(EngineFixture *f)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    char temp[400];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(temp, sizeof temp, &len), NPH_RESULT_OK);
    (void)snprintf(f->dir, sizeof f->dir, "%s/nph-engine-tests-%llu", temp,
                   (unsigned long long)nph_platform_process_id());
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
    f->config.headless = 1;
}

static NphResult start(EngineFixture *f, NphEngineMode mode)
{
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.config = &f->config;
    d.mode = mode;
    d.app_name = "engine tests";
    d.offscreen_width = 96U;
    d.offscreen_height = 64U;
    return nph_engine_create(&d, &f->engine);
}

static void finish(EngineFixture *f)
{
    nph_engine_destroy(f->engine);
    f->engine = NULL;
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
    NPH_IGNORE_RESULT(nph_fs_remove_tree(f->dir));
}

static void write_text(const char *path, const char *text)
{
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, text, strlen(text)), NPH_RESULT_OK);
}

static i64 cvar_int(NphEngine *e, const char *name)
{
    const NphCvar *c = nph_cvar_find(nph_engine_cvars(e), nph_str_from_cstr(name));
    NTEST_ASSERT_NOT_NULL(c);
    return nph_cvar_get_int(c);
}

static NphEntity spawn_named(NphWorld *w, const char *name, f64 x)
{
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &e), NPH_RESULT_OK);
    NphName *n = NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_NAME, NphName);
    nph_text_set(&n->text, nph_str_from_cstr(name));
    NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation.x = x;
    return e;
}

NTEST(Engine, a_headless_engine_runs_and_leaks_nothing)
{
    static EngineFixture fx;
    prepare(&fx);
    static const char *const assignments[1] = {"rt.max_frames=4"};
    fx.config.assignments[0] = assignments[0];
    fx.config.assignment_count = 1U;
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_WINDOWED), NPH_RESULT_OK); // headless wins
    NphEngine *e = fx.engine;
    NTEST_EXPECT_EQ_INT(nph_engine_mode(e), NPH_ENGINE_MODE_HEADLESS);
    NTEST_EXPECT_NULL(nph_engine_window(e));
    NTEST_EXPECT_NULL(nph_engine_device(e));
    NTEST_EXPECT_NULL(nph_engine_renderer(e));
    NTEST_EXPECT_NULL(nph_engine_scene_renderer(e));
    NTEST_EXPECT_NULL(nph_engine_game(e));
    NTEST_EXPECT_NOT_NULL(nph_engine_world(e));
    NTEST_EXPECT_NOT_NULL(nph_engine_jobs(e));
    NTEST_EXPECT_NOT_NULL(nph_engine_loader(e));
    NTEST_EXPECT_NOT_NULL(nph_engine_schedule(e));
    NTEST_EXPECT_TRUE(nph_engine_world(e) == nph_engine_edit_world(e));
    NTEST_EXPECT_NE_UINT(nph_engine_components(e)->mesh_renderer, NPH_COMPONENT_NONE);
    NTEST_ASSERT_EQ_INT(nph_engine_run(e), NPH_RESULT_OK); // rt.max_frames
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->frame, 4U);
    NTEST_EXPECT_TRUE(nph_engine_should_quit(e));
    NTEST_EXPECT_EQ_UINT(nph_engine_clock(e)->rate_hz, 240U);
    finish(&fx);
}

NTEST(Engine, configuration_takes_the_command_line_over_files_and_saves_the_user_config)
{
    static EngineFixture fx;
    prepare(&fx);
    char user[600];
    char extra[600];
    path_in(&fx, "user.cfg", user, sizeof user);
    path_in(&fx, "extra.cfg", extra, sizeof extra);
    write_text(user, "# saved last time\nr.width = 1600\nsim.rate = 120\n");
    write_text(extra, "sim.rate = 60\nrt.max_fps = 30\n");
    fx.config.user_config = user;
    fx.config.configs[0] = extra;
    fx.config.config_count = 1U;
    static const char *const assignments[2] = {"sim.rate=90", "later.registered=7"};
    fx.config.assignments[0] = assignments[0];
    fx.config.assignments[1] = assignments[1];
    fx.config.assignment_count = 2U;
    fx.config.max_frames = 3U;
    fx.config.has_max_frames = 1;
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cvar_int(fx.engine, "r.width"), 1600);    // the user config
    NTEST_EXPECT_EQ_INT(cvar_int(fx.engine, "rt.max_fps"), 30);   // a --config file
    NTEST_EXPECT_EQ_INT(cvar_int(fx.engine, "sim.rate"), 90);     // the command line
    NTEST_EXPECT_EQ_INT(cvar_int(fx.engine, "rt.max_frames"), 3); // --frames
    // Changed while running; written back at exit (SAVED cvars only).
    NphCvar *height = nph_cvar_find(nph_engine_cvars(fx.engine), NPH_STR("r.height"));
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(nph_engine_cvars(fx.engine), height, 900), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_cvar_set_int(nph_engine_cvars(fx.engine),
                         nph_cvar_find(nph_engine_cvars(fx.engine), NPH_STR("rt.max_fps")), 0),
        NPH_RESULT_OK);
    nph_engine_destroy(fx.engine);
    fx.engine = NULL;
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(user, nph_allocator_system(), &data, &size),
                        NPH_RESULT_OK);
    const NphStr text = nph_str((const char *)data, size);
    NTEST_EXPECT_TRUE(nph_str_contains(text, NPH_STR("r.height = 900")));
    NTEST_EXPECT_TRUE(nph_str_contains(text, NPH_STR("r.width = 1600")));
    NTEST_EXPECT_TRUE(nph_str_contains(text, NPH_STR("rt.max_fps = 0")));
    NTEST_EXPECT_FALSE(nph_str_contains(text, NPH_STR("sim.rate"))); // not SAVED
    nph_file_free_all(nph_allocator_system(), data, size);

    // A --config file that does not exist stops the engine from starting.
    char missing[600];
    path_in(&fx, "missing.cfg", missing, sizeof missing);
    fx.config.configs[0] = missing;
    fx.config.user_config = NULL;
    NphEngine *broken = NULL;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&fx.da);
    d.config = &fx.config;
    NTEST_EXPECT_EQ_INT(nph_engine_create(&d, &broken), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_NULL(broken);
    finish(&fx);
}

NTEST(Engine, scenes_save_load_and_clear)
{
    static EngineFixture fx;
    prepare(&fx);
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NphWorld *w = nph_engine_edit_world(e);
    const NphEntity a = spawn_named(w, "alpha", 1.0);
    const NphEntity b = spawn_named(w, "beta", 2.0);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, b, a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    char text_path[600];
    char bin_path[600];
    path_in(&fx, "level.scene", text_path, sizeof text_path);
    path_in(&fx, "level.bscene", bin_path, sizeof bin_path);
    NTEST_ASSERT_EQ_INT(nph_engine_save_scene(e, text_path), NPH_RESULT_OK);
    char temp_path[620];
    (void)snprintf(temp_path, sizeof temp_path, "%s.tmp", text_path);
    NTEST_EXPECT_FALSE(nph_fs_exists(temp_path)); // written aside, then renamed
    NTEST_ASSERT_EQ_INT(nph_engine_clear_scene(e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 0U);
    NTEST_ASSERT_EQ_INT(nph_engine_load_scene(e, text_path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 2U);
    // The same file again: entities are added to what the world holds.
    NTEST_ASSERT_EQ_INT(nph_engine_load_scene(e, text_path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 4U);
    // A binary scene is recognised by its magic.
    NphBinWriter bw;
    nph_bin_writer_init(&bw, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_binary(w, NULL, &bw, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_file_write_all(bin_path, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw)),
        NPH_RESULT_OK);
    nph_bin_writer_free(&bw);
    NTEST_ASSERT_EQ_INT(nph_engine_clear_scene(e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_load_scene(e, bin_path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 4U);
    // The loaded entities are placed by the next frame.
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_wait(e), NPH_RESULT_OK);
    u32 placed = 0U;
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity x = nph_world_entity_at(w, i);
        const NphWorldTransform *wt =
            NPH_WORLD_GET(w, x, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
        if (wt != NULL && wt->translation.x == 3.0) { // beta under alpha
            placed += 1U;
        }
    }
    NTEST_EXPECT_EQ_UINT(placed, 2U);
    // Failures leave the world as it was.
    char missing[600];
    path_in(&fx, "missing.scene", missing, sizeof missing);
    NTEST_EXPECT_EQ_INT(nph_engine_load_scene(e, missing), NPH_RESULT_NOT_FOUND);
    char bad[600];
    path_in(&fx, "bad.scene", bad, sizeof bad);
    write_text(bad, "{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": "
                    "{\"Nope\": {}}}]}");
    NTEST_EXPECT_EQ_INT(nph_engine_load_scene(e, bad), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 4U);
    finish(&fx);
}

NTEST(Engine, console_commands_drive_the_engine)
{
    static EngineFixture fx;
    prepare(&fx);
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NphConsole *c = nph_engine_console(e);
    NphWorld *edit = nph_engine_edit_world(e);
    (void)spawn_named(edit, "thing", 0.0);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("stats")), NPH_RESULT_OK);
    char path[600];
    path_in(&fx, "console.scene", path, sizeof path);
    char line[700];
    (void)snprintf(line, sizeof line, "scene.save \"%s\"", path);
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, nph_str_from_cstr(line)), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_fs_exists(path));
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("scene.save")), NPH_RESULT_INVALID_ARGUMENT);
    // play / pause / step / stop.
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("play")), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_is_playing(e));
    NTEST_EXPECT_TRUE(nph_engine_world(e) != edit);
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("pause")), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_is_paused(e));
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("step")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->steps, 1U);
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("stop")), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_engine_is_playing(e));
    NTEST_EXPECT_TRUE(nph_engine_world(e) == edit);
    // No game to reload.
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("reload")), NPH_RESULT_OK);
    // A trace of the frames so far, when profiling is on.
    if (nph_engine_profiler(e) != NULL) {
        path_in(&fx, "trace.json", path, sizeof path);
        (void)snprintf(line, sizeof line, "trace \"%s\"", path);
        NTEST_EXPECT_EQ_INT(nph_console_execute(c, nph_str_from_cstr(line)), NPH_RESULT_OK);
        NTEST_EXPECT_TRUE(nph_fs_exists(path));
    }
    NTEST_EXPECT_EQ_INT(nph_console_execute(c, NPH_STR("quit")), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_should_quit(e));
    finish(&fx);
}

NTEST(Engine, the_world_steps_and_the_view_follows_the_override)
{
    static EngineFixture fx;
    prepare(&fx);
    static const char *const assignments[1] = {"rt.lockstep=true"};
    fx.config.assignments[0] = assignments[0];
    fx.config.assignment_count = 1U;
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    // Not playing: no steps.
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->steps, 0U);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, false), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_world(e) == nph_engine_edit_world(e));
    for (u32 i = 0; i < 6U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_wait(e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_engine_frame_stats(e)->total_steps, 6U);
    NTEST_EXPECT_EQ_INT(nph_engine_load_scene(e, "anything"), NPH_RESULT_INVALID_STATE);
    nph_engine_play_end(e);
    // The view override.
    NphRenderView v;
    nph_render_view_default(&v,
                            nph_render_camera_look_at(nph_dvec3(5.0, 5.0, 5.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 1.0F, 0.2F));
    v.ev100 = 11.0F;
    nph_engine_set_view(e, &v);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->ev100, 11.0F, 0.0F);
    NTEST_EXPECT_NEAR_F64(nph_engine_view(e)->camera.position.x, 5.0, 0.0);
    nph_engine_set_view(e, NULL);
    finish(&fx);
}

// The r.* cvars set the production path's switches, the bloom, the
// exposure, the ambient occlusion, and the global illumination on whichever
// view a frame renders.
NTEST(Engine, the_render_cvars_set_the_views_switches)
{
    static EngineFixture fx;
    prepare(&fx);
    static const char *const assignments[11] = {
        "r.occlusion=false",
        "r.sw_raster=false",
        "r.virtual_shadows=false",
        "r.taa=false",
        "r.bloom=0.25",
        "r.auto_exposure=true",
        "r.exposure_compensation=1.5",
        "r.exposure_speed=3.0",
        "r.ao=1.25",
        "r.gi=false",
        "rt.lockstep=true",
    };
    for (u32 i = 0; i < 11U; ++i) {
        fx.config.assignments[i] = assignments[i];
    }
    fx.config.assignment_count = 11U;
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    const NphRenderView *view = nph_engine_view(e);
    NTEST_EXPECT_EQ_UINT(view->vis_flags, (u32)NPH_RENDER_VIS_NO_OCCLUSION |
                                              (u32)NPH_RENDER_VIS_NO_SW_RASTER |
                                              (u32)NPH_RENDER_VIS_RT_SHADOWS);
    NTEST_EXPECT_EQ_INT((int)view->anti_aliasing, (int)NPH_RENDER_AA_NONE);
    NTEST_EXPECT_NEAR_F32(view->bloom, 0.25F, 0.0F);
    NTEST_EXPECT_EQ_INT((int)view->exposure, (int)NPH_RENDER_EXPOSURE_AUTO);
    NTEST_EXPECT_NEAR_F32(view->exposure_compensation, 1.5F, 0.0F);
    NTEST_EXPECT_NEAR_F32(view->exposure_speed, 3.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(view->ao_radius, 1.25F, 0.0F);
    NTEST_EXPECT_FALSE(view->gi);
    // In lockstep a frame lasts one step (sim.rate 240), whatever the clock.
    NTEST_EXPECT_NEAR_F32(view->frame_seconds, 1.0F / 240.0F, 1e-9F);
    finish(&fx);
}

// By default a view has bloom (r.bloom 0.04), the camera's own exposure
// (r.auto_exposure off), ambient occlusion within half a metre (r.ao), and
// global illumination (r.gi), and its frame time is the clock's.
NTEST(Engine, views_have_bloom_and_a_manual_exposure_by_default)
{
    static EngineFixture fx;
    prepare(&fx);
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->frame_seconds, 0.0F, 0.0F); // no previous frame
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    const NphRenderView *view = nph_engine_view(e);
    NTEST_EXPECT_NEAR_F32(view->bloom, 0.04F, 1e-7F);
    NTEST_EXPECT_EQ_INT((int)view->exposure, (int)NPH_RENDER_EXPOSURE_MANUAL);
    NTEST_EXPECT_NEAR_F32(view->exposure_compensation, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(view->exposure_speed, 1.5F, 0.0F);
    NTEST_EXPECT_NEAR_F32(view->ao_radius, 0.5F, 0.0F);
    NTEST_EXPECT_TRUE(view->gi);
    NTEST_EXPECT_TRUE(view->frame_seconds > 0.0F);
    finish(&fx);
}

// A view's shutter reaches the renderer, and r.motion_blur (on by default)
// closes it while off.
NTEST(Engine, r_motion_blur_closes_the_shutter_while_off)
{
    static EngineFixture fx;
    prepare(&fx);
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NphRenderView v;
    nph_render_view_default(&v,
                            nph_render_camera_look_at(nph_dvec3(5.0, 5.0, 5.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 1.0F, 0.2F));
    v.shutter = 1.0F / 480.0F;
    nph_engine_set_view(e, &v);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->shutter, 1.0F / 480.0F, 0.0F);
    NphCvar *mb = nph_cvar_find(nph_engine_cvars(e), NPH_STR("r.motion_blur"));
    NTEST_ASSERT_NOT_NULL(mb);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(nph_engine_cvars(e), mb, 0), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->shutter, 0.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(nph_engine_cvars(e), mb, 1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->shutter, 1.0F / 480.0F, 0.0F);
    nph_engine_set_view(e, NULL);
    finish(&fx);
}

// A view's lens reaches the renderer, and r.dof (on by default) makes it a
// pinhole while off.
NTEST(Engine, r_dof_makes_the_lens_a_pinhole_while_off)
{
    static EngineFixture fx;
    prepare(&fx);
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NphRenderView v;
    nph_render_view_default(&v,
                            nph_render_camera_look_at(nph_dvec3(5.0, 5.0, 5.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 1.0F, 0.2F));
    v.aperture = 2.0F;
    v.focus_distance = 8.66F;
    nph_engine_set_view(e, &v);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->aperture, 2.0F, 0.0F);
    NphCvar *dof = nph_cvar_find(nph_engine_cvars(e), NPH_STR("r.dof"));
    NTEST_ASSERT_NOT_NULL(dof);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(nph_engine_cvars(e), dof, 0), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->aperture, 0.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_bool(nph_engine_cvars(e), dof, 1), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->aperture, 2.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(nph_engine_view(e)->focus_distance, 8.66F, 0.0F);
    nph_engine_set_view(e, NULL);
    finish(&fx);
}

// Temporal anti-aliasing is on by default (r.taa), and a switch between the
// world's camera and an override is a camera cut, once.
NTEST(Engine, views_are_anti_aliased_and_a_camera_switch_cuts)
{
    static EngineFixture fx;
    prepare(&fx);
    NTEST_ASSERT_EQ_INT(start(&fx, NPH_ENGINE_MODE_HEADLESS), NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT((int)nph_engine_view(e)->anti_aliasing, (int)NPH_RENDER_AA_TEMPORAL);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_engine_view(e)->camera_cut);
    NphRenderView v;
    nph_render_view_default(&v, nph_render_camera_look_at(nph_dvec3(0.0, 1.0, 5.0),
                                                          nph_dvec3(0.0, 0.0, 0.0),
                                                          nph_vec3(0.0F, 1.0F, 0.0F), 0.8F, 0.1F));
    nph_engine_set_view(e, &v);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_view(e)->camera_cut);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_engine_view(e)->camera_cut);
    // A cut the host asks for applies once, though its view stays.
    v.camera_cut = 1;
    nph_engine_set_view(e, &v);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_view(e)->camera_cut);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_engine_view(e)->camera_cut);
    nph_engine_set_view(e, NULL);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_view(e)->camera_cut);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_engine_view(e)->camera_cut);
    finish(&fx);
}

static u8 *read_target(NphEngine *e, u64 bytes)
{
    NphGpuDevice *dev = nph_engine_device(e);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(dev), NPH_RESULT_OK);
    u8 *pixels = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, bytes);
    NTEST_ASSERT_NOT_NULL(pixels);
    test_read_image(dev, nph_engine_offscreen_target(e), 0U, pixels, bytes);
    return pixels;
}

// An offscreen engine with r.pipeline=`pipeline` draws the cooked
// showcase: imported into the world, mirrored into the renderer once its
// assets are in, and rendered into the engine's target under the
// validation gate. Returns the pixels (the caller frees them), or NULL
// without a Vulkan device.
static u8 *draw_showcase(u32 pipeline, u64 bytes)
{
    static EngineFixture fx;
    prepare(&fx);
    fx.config.headless = 0;
    fx.config.packs[0] = NPH_TEST_PACK;
    fx.config.pack_count = 1U;
    static char assignment[32];
    (void)snprintf(assignment, sizeof assignment, "r.pipeline=%u", pipeline);
    fx.config.assignments[0] = assignment;
    // The pipelines are compared pixel for pixel: one sample per pixel
    // (render_tests hold the temporal resolve to its own reference).
    fx.config.assignments[1] = "r.taa=false";
    fx.config.assignment_count = 2U;
    const NphResult started = start(&fx, NPH_ENGINE_MODE_OFFSCREEN);
    if (started == NPH_RESULT_UNSUPPORTED) {
        finish(&fx);
        return NULL;
    }
    NTEST_ASSERT_EQ_INT(started, NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NTEST_EXPECT_NULL(nph_engine_window(e));
    NTEST_EXPECT_NULL(nph_engine_swapchain(e));
    NTEST_ASSERT_NOT_NULL(nph_engine_renderer(e));
    // The sky alone first.
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    u8 *sky = read_target(e, bytes);
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = nph_engine_edit_world(e);
    d.loader = nph_engine_loader(e);
    d.scene = nph_guid_from_cstr("showcase.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, NULL, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    nph_scene_renderer_wait(nph_engine_scene_renderer(e));
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    // The scene covers a good part of the view the sky filled.
    u8 *scene = read_target(e, bytes);
    u32 covered = 0U;
    for (u64 i = 0; i < bytes; i += 4U) {
        covered += memcmp(scene + i, sky + i, 3U) != 0 ? 1U : 0U;
    }
    NTEST_EXPECT_GT_UINT(covered, 96U * 64U / 10U);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, sky, bytes);
    const bool production =
        pipeline == 1U && nph_renderer_production_supported(nph_engine_renderer(e));
    if (production) {
        // The GPU's counters arrive NPH_GPU_FRAMES_IN_FLIGHT frames late.
        for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
            NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
        }
    }
    const NphEngineFrameStats *st = nph_engine_frame_stats(e);
    NTEST_EXPECT_EQ_UINT(st->scene.instances, 2U);
    NTEST_EXPECT_EQ_UINT(st->scene.lights, 1U);
    NTEST_EXPECT_EQ_UINT(st->render.instances, 2U);
    NTEST_EXPECT_EQ_INT((int)st->render.pipeline, production ? (int)NPH_RENDER_PIPELINE_PRODUCTION
                                                             : (int)NPH_RENDER_PIPELINE_REFERENCE);
    if (production) {
        NTEST_EXPECT_GE_UINT(st->render.vis_clusters, 2U);
        NTEST_EXPECT_GT_UINT(st->render.vis_triangles, 0U);
    } else {
        NTEST_EXPECT_GE_UINT(st->render.draws, 1U);
    }
    const NphGpuValidationStats vs = nph_gpu_device_validation_stats(nph_engine_device(e));
    NTEST_EXPECT_EQ_UINT(vs.errors, 0U);
    NTEST_EXPECT_EQ_UINT(vs.warnings, 0U);
    finish(&fx);
    return scene;
}

// The showcase through both pipelines: each draws it, and the images agree
// (the production path's own tests hold it to the reference much tighter).
NTEST(Engine, an_offscreen_engine_draws_the_world)
{
    const u64 bytes = (u64)96U * 64U * 4U;
    u8 *reference = draw_showcase(0U, bytes);
    if (reference == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    u8 *production = draw_showcase(1U, bytes);
    NTEST_ASSERT_NOT_NULL(production);
    u64 within = 0U;
    u32 max = 0U;
    for (u64 i = 0; i < bytes; ++i) {
        const u32 diff = (u32)(reference[i] > production[i] ? reference[i] - production[i]
                                                            : production[i] - reference[i]);
        max = diff > max ? diff : max;
        within += diff <= 3U ? 1U : 0U;
    }
    NTEST_LOG("production vs reference: %.2f %% of channels within 3, max %u",
              (f64)within * 100.0 / (f64)bytes, max);
    NTEST_EXPECT_GE_UINT(within, bytes * 99U / 100U);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, reference, bytes);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, production, bytes);
}

// A windowed engine, as the player runs it: frames into the swapchain with
// passes recorded in parallel, the showcase drawn, and depth-tested debug
// lines every frame, under the validation gate. It caught the renderer's
// tonemap pass storing the depth it only reads (STORE_OP_DONT_CARE, a write
// for synchronization) with nothing ordering that store before the next
// frame's clear; read-only attachments now store with NONE. Skipped
// without a window or a device.
NTEST(Engine, a_windowed_engine_presents_without_validation_messages)
{
    static EngineFixture fx;
    prepare(&fx);
    fx.config.headless = 0;
    fx.config.packs[0] = NPH_TEST_PACK;
    fx.config.pack_count = 1U;
    const NphResult started = start(&fx, NPH_ENGINE_MODE_WINDOWED);
    if (started == NPH_RESULT_UNSUPPORTED) {
        finish(&fx);
        NTEST_SKIP("no window system or Vulkan device");
    }
    NTEST_ASSERT_EQ_INT(started, NPH_RESULT_OK);
    NphEngine *e = fx.engine;
    NTEST_ASSERT_NOT_NULL(nph_engine_swapchain(e));
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = nph_engine_edit_world(e);
    d.loader = nph_engine_loader(e);
    d.scene = nph_guid_from_cstr("showcase.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, NULL, NULL), NPH_RESULT_OK);
    NphRenderer *r = nph_engine_renderer(e);
    for (u32 i = 0; i < 30U; ++i) {
        nph_render_debug_grid(r, nph_vec3_zero(), 1.0F, 8U, NPH_RENDER_COLOR_GRAY, 0U);
        NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_wait(e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(nph_engine_device(e)), NPH_RESULT_OK);
    const NphGpuValidationStats vs = nph_gpu_device_validation_stats(nph_engine_device(e));
    NTEST_EXPECT_EQ_UINT(vs.errors, 0U);
    NTEST_EXPECT_EQ_UINT(vs.warnings, 0U);
    finish(&fx);
}
