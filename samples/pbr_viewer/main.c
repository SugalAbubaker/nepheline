// SPDX-License-Identifier: Apache-2.0
// pbr_viewer: the Phase 7 sample (CLAUDE.md §12, "Renderer v1"): the forward
// PBR reference renderer (engine/render) drawing cooked content.
// samples/pbr_viewer/content/pbr_scene.gltf is cooked at build time
// (samples/CMakeLists.txt); the program loads the pack through the VFS and
// the asset loader (samples/common/cooked_scene.c) and renders it lit by
// its sun, point, and spot lights and by a procedural sky through
// image-based lighting, tonemapped into the swapchain, with debug lines
// over it and the GPU profiler's per-pass times in the log.
//
//     pbr_viewer [--frames=N] [--capture=FILE.bmp] [--profile=FILE.json]
//                [--pack=FILE] [--tonemapper=aces|agx|none]
//                [--view=lit|base|normal|roughness|metallic|occlusion|direct|indirect]
//                [--ev100=X] [--vsync] [--no-debug]
//
// Keys: 1/2/3 ACES/AgX/none, V cycles the debug views, I toggles image-based
// lighting, D the debug lines, Space pauses the orbit, the mouse wheel
// zooms, Escape quits. The orbit advances per frame, so a --frames run
// renders the same images every time (--capture keeps the last).
//
// Exit status 77: no window system or Vulkan device (CTest "skipped");
// 1: the content did not load or a validation error was reported.

#include <nepheline/asset/vfs.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/render/gpu_profiler.h>
#include <nepheline/render/renderer.h>

#include "capture.h"
#include "cooked_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "pbr_viewer", NPH_LOG_LEVEL_INFO);

#define EXIT_SKIPPED 77

// The scene's identity: the source's path under the cook root and the
// sub-asset (docs/cook.md, "Identity").
#define SCENE_GUID_KEY "pbr_scene.gltf#scene"

typedef struct Options {
    u64 max_frames;
    const char *pack;
    const char *capture;
    const char *profile;
    NphRenderTonemapper tonemapper;
    NphRenderDebugView view;
    f32 ev100;
    bool vsync;
    bool debug_lines;
} Options;

static const char *const g_view_names[NPH_RENDER_VIEW_COUNT] = {
    "lit", "base", "normal", "roughness", "metallic", "occlusion", "direct", "indirect"};
static const char *const g_tonemapper_names[3] = {"none", "aces", "agx"};

static int usage(void)
{
    fprintf(stderr, "usage: pbr_viewer [--frames=N] [--capture=FILE.bmp] [--profile=FILE.json] "
                    "[--pack=FILE]\n"
                    "                  [--tonemapper=aces|agx|none] "
                    "[--view=lit|base|normal|roughness|metallic|occlusion|direct|indirect]\n"
                    "                  [--ev100=X] [--vsync] [--no-debug]\n");
    return 2;
}

static bool parse(int argc, char **argv, Options *o)
{
    memset(o, 0, sizeof *o);
    o->pack = NPH_SAMPLE_PACK;
    o->tonemapper = NPH_RENDER_TONEMAPPER_ACES;
    o->view = NPH_RENDER_VIEW_LIT;
    o->ev100 = 10.75F; // a low sun (about 9000 lux on the ground) under a dusk sky
    o->debug_lines = true;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strncmp(a, "--frames=", 9U) == 0) {
            o->max_frames = strtoull(a + 9, NULL, 10);
        } else if (strncmp(a, "--capture=", 10U) == 0) {
            o->capture = a + 10;
        } else if (strncmp(a, "--profile=", 10U) == 0) {
            o->profile = a + 10;
        } else if (strncmp(a, "--pack=", 7U) == 0) {
            o->pack = a + 7;
        } else if (strncmp(a, "--ev100=", 8U) == 0) {
            o->ev100 = strtof(a + 8, NULL);
        } else if (strncmp(a, "--tonemapper=", 13U) == 0) {
            bool found = false;
            for (u32 t = 0; t < 3U; ++t) {
                if (strcmp(a + 13, g_tonemapper_names[t]) == 0) {
                    o->tonemapper = (NphRenderTonemapper)t;
                    found = true;
                }
            }
            if (!found) {
                return false;
            }
        } else if (strncmp(a, "--view=", 7U) == 0) {
            bool found = false;
            for (u32 v = 0; v < (u32)NPH_RENDER_VIEW_COUNT; ++v) {
                if (strcmp(a + 7, g_view_names[v]) == 0) {
                    o->view = (NphRenderDebugView)v;
                    found = true;
                }
            }
            if (!found) {
                return false;
            }
        } else if (strcmp(a, "--vsync") == 0) {
            o->vsync = true;
        } else if (strcmp(a, "--no-debug") == 0) {
            o->debug_lines = false;
        } else {
            return false;
        }
    }
    if (o->capture != NULL && o->max_frames == 0U) {
        fprintf(stderr, "pbr_viewer: --capture needs --frames (it captures the last one)\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Debug lines: a ground grid, the scene bounds, the world axes, and a gizmo
// per light.
// ---------------------------------------------------------------------------

static void draw_debug(NphRenderer *r, const CookedScene *scene)
{
    nph_render_debug_grid(r, nph_vec3(0.0F, 0.002F, 0.0F), 1.0F, 16U,
                          NPH_RENDER_RGBA(90U, 90U, 100U, 255U), NPH_RENDER_DEBUG_DEPTH_TESTED);
    nph_render_debug_aabb(r, scene->bounds, NPH_RENDER_COLOR_GRAY, NPH_RENDER_DEBUG_DEPTH_TESTED);
    const NphAffine origin = nph_affine_identity();
    nph_render_debug_axes(r, &origin, 0.5F, NPH_RENDER_DEBUG_OVERLAY);
    for (u32 i = 0; i < scene->light_count; ++i) {
        const NphRenderLightDesc *l = &scene->light_descs[i];
        if (l->type == NPH_RENDER_LIGHT_TYPE_DIRECTIONAL) {
            // An arrow along the sun's direction, above the scene.
            const NphVec3 to = nph_vec3(0.0F, 3.0F, 0.0F);
            nph_render_debug_arrow(r, nph_vec3_sub(to, nph_vec3_scale(l->direction, 1.5F)), to,
                                   NPH_RENDER_COLOR_YELLOW, NPH_RENDER_DEBUG_OVERLAY);
            continue;
        }
        const NphVec3 p = nph_dvec3_relative(l->position, nph_render_debug_origin(r));
        nph_render_debug_sphere(r, p, 0.08F, NPH_RENDER_COLOR_YELLOW, NPH_RENDER_DEBUG_OVERLAY,
                                16U);
        if (l->type == NPH_RENDER_LIGHT_TYPE_SPOT) {
            nph_render_debug_arrow(r, p, nph_vec3_add(p, nph_vec3_scale(l->direction, 0.8F)),
                                   NPH_RENDER_COLOR_CYAN, NPH_RENDER_DEBUG_OVERLAY);
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

typedef struct App {
    Options opt;
    NphAllocator *alloc;
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;
    NphWindow *window;
    NphGpuDevice *dev;
    NphGpuSwapchain *swapchain;
    NphGpuRenderGraph *graph;
    NphRenderer *renderer;
    NphRenderGpuProfiler *profiler;
    CookedScene scene;
    NphGpuBuffer capture;
    NphGpuImage capture_image;
} App;

static void capture_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    App *app = (App *)user;
    NphGpuBufferImageCopy region;
    memset(&region, 0, sizeof region); // all of mip 0, tightly packed
    nph_gpu_cmd_copy_image_to_buffer(cmd, app->capture_image, NPH_GPU_LAYOUT_TRANSFER_SRC,
                                     app->capture, &region, 1U);
}

static NphResult load_content(App *app)
{
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = app->alloc;
    NPH_TRY(nph_job_system_create(&jd, &app->jobs));
    NPH_TRY(nph_vfs_create(app->alloc, &app->vfs));
    const NphResult r = nph_vfs_mount_pack(app->vfs, NPH_STR("content"), app->opt.pack, NULL);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "cannot mount %s: %s (the build cooks it)", app->opt.pack,
                      nph_result_string(r));
        return r;
    }
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = app->alloc;
    ld.vfs = app->vfs;
    ld.jobs = app->jobs;
    return nph_asset_loader_create(&ld, &app->loader);
}

static NphResult open_device(App *app, int *status)
{
    if (nph_window_system_init() != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no window system available; skipping");
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = "Nepheline pbr_viewer";
    if (nph_window_create(app->alloc, &wd, &app->window) != NPH_RESULT_OK) {
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = app->alloc;
    dd.app_name = "pbr_viewer";
    const NphResult r = nph_gpu_device_create(&dd, &app->dev);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no Vulkan device (%s); skipping", nph_result_string(r));
        *status = r == NPH_RESULT_UNSUPPORTED ? EXIT_SKIPPED : 1;
        return r;
    }
    NphGpuSwapchainDesc scd;
    memset(&scd, 0, sizeof scd);
    scd.window = app->window;
    scd.vsync = app->opt.vsync;
    scd.prefer_srgb = 1;
    if (nph_gpu_swapchain_create(app->dev, &scd, &app->swapchain) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the window cannot be presented to; skipping");
        *status = EXIT_SKIPPED;
        return NPH_RESULT_UNSUPPORTED;
    }
    return NPH_RESULT_OK;
}

static NphResult create_renderer(App *app)
{
    NphRendererDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.alloc = app->alloc;
    rd.dev = app->dev;
    rd.shader_dir = NPH_SHADER_DIR;
    rd.name = "pbr_viewer";
    NPH_TRY(nph_renderer_create(&rd, &app->renderer));
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&ed.sky);
    ed.intensity = 0.25F; // dusk: a quarter of the clear midday sky
    NPH_TRY(nph_render_set_environment(app->renderer, &ed));
    NphRenderGpuProfilerDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.alloc = app->alloc;
    NPH_TRY(nph_render_gpu_profiler_create(&pd, &app->profiler));
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = app->alloc;
    gd.jobs = app->jobs;
    gd.name = "pbr_viewer";
    NPH_TRY(nph_gpu_render_graph_create(app->dev, &gd, &app->graph));
    CookedSceneDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.alloc = app->alloc;
    cd.renderer = app->renderer;
    cd.loader = app->loader;
    cd.jobs = app->jobs;
    cd.scene = nph_guid_from_cstr(SCENE_GUID_KEY);
    return cooked_scene_load(&cd, &app->scene);
}

static void handle_input(App *app, NphRenderView *view, bool *paused, f32 *distance, bool *lines)
{
    nph_window_poll_events();
    NphWindowEvent e;
    while (nph_window_pop_event(app->window, &e)) {
        if (e.type == NPH_WINDOW_EVENT_MOUSE_SCROLL) {
            *distance = nph_f32_clamp(*distance * (1.0F - 0.1F * (f32)e.u.scroll.dy), 1.5F, 40.0F);
            continue;
        }
        if (e.type != NPH_WINDOW_EVENT_KEY || e.u.key.action != NPH_KEY_ACTION_PRESS) {
            continue;
        }
        switch (e.u.key.key) {
        case NPH_KEY_ESCAPE:
            nph_window_set_should_close(app->window, true);
            break;
        case NPH_KEY_1:
            view->tonemapper = NPH_RENDER_TONEMAPPER_ACES;
            break;
        case NPH_KEY_2:
            view->tonemapper = NPH_RENDER_TONEMAPPER_AGX;
            break;
        case NPH_KEY_3:
            view->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
            break;
        case NPH_KEY_V:
            view->debug_view =
                (NphRenderDebugView)(((u32)view->debug_view + 1U) % (u32)NPH_RENDER_VIEW_COUNT);
            NPH_LOG_INFO(&LOG_SAMPLE, "view: %s", g_view_names[view->debug_view]);
            break;
        case NPH_KEY_I:
            view->disable_ibl = !view->disable_ibl;
            break;
        case NPH_KEY_D:
            *lines = !*lines;
            break;
        case NPH_KEY_SPACE:
            *paused = !*paused;
            break;
        default:
            break;
        }
    }
}

// One frame: returns false to stop (with *status set on failure).
static bool frame(App *app, NphRenderView *view, u64 index, bool capturing, int *status)
{
    if (nph_gpu_frame_begin(app->dev) != NPH_RESULT_OK) {
        *status = 1;
        return false;
    }
    NPH_IGNORE_RESULT(nph_render_gpu_profiler_update(app->profiler, app->graph));
    NphGpuImage backbuffer = NPH_GPU_IMAGE_NULL;
    const NphResult ar = nph_gpu_swapchain_acquire(app->swapchain, &backbuffer);
    if (ar == NPH_RESULT_EMPTY) { // minimised
        NPH_IGNORE_RESULT(nph_gpu_frame_end(app->dev));
        nph_thread_sleep_ns(16U * NPH_TIME_NS_PER_MS);
        return true;
    }
    if (ar != NPH_RESULT_OK) {
        NPH_IGNORE_RESULT(nph_gpu_frame_end(app->dev));
        *status = 1;
        return false;
    }
    nph_gpu_rg_begin(app->graph);
    NphGpuRgImage target = NPH_GPU_RG_IMAGE_NULL;
    NphRenderFrameStats stats;
    NphResult r = nph_render_frame(app->renderer, view, app->graph, backbuffer, &target, &stats);
    if (r == NPH_RESULT_OK && capturing) {
        u32 w = 0U;
        u32 h = 0U;
        nph_gpu_swapchain_extent(app->swapchain, &w, &h);
        NphGpuBufferDesc cbd;
        memset(&cbd, 0, sizeof cbd);
        cbd.size = (u64)w * h * 4U;
        cbd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
        cbd.memory = NPH_GPU_MEMORY_READBACK;
        cbd.name = "capture";
        if (nph_gpu_buffer_create(app->dev, &cbd, &app->capture) == NPH_RESULT_OK) {
            app->capture_image = backbuffer;
            NphGpuRgPassDesc pd;
            memset(&pd, 0, sizeof pd);
            pd.name = "capture";
            pd.type = NPH_GPU_RG_PASS_TRANSFER;
            pd.execute = capture_execute;
            pd.user = app;
            NphGpuRgPass *copy = nph_gpu_rg_add_pass(app->graph, &pd);
            nph_gpu_rg_read_image(copy, target, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, 1U);
            nph_gpu_rg_write_buffer(copy, nph_gpu_rg_import_buffer(app->graph, app->capture),
                                    NPH_GPU_RG_USE_TRANSFER_DST);
        }
    }
    if (r == NPH_RESULT_OK) {
        nph_gpu_rg_present(app->graph, target, app->swapchain);
        r = nph_gpu_rg_compile(app->graph);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_gpu_rg_execute(app->graph);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_gpu_swapchain_present(app->swapchain);
    }
    const NphResult er = nph_gpu_frame_end(app->dev);
    if (r != NPH_RESULT_OK || er != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "frame %llu failed: %s", (unsigned long long)index,
                      nph_result_string(r != NPH_RESULT_OK ? r : er));
        *status = 1;
        return false;
    }
    if (index == 0U) {
        NPH_LOG_INFO(&LOG_SAMPLE,
                     "%u instances, %u draws (%u blended), %u triangles, %u lights, %u debug lines",
                     stats.instances, stats.draws, stats.blended, stats.triangles, stats.lights,
                     stats.debug_lines);
    }
    return true;
}

static void write_capture(App *app, int *status)
{
    if (!nph_gpu_buffer_is_valid(app->dev, app->capture) ||
        (nph_gpu_image_desc(app->dev, app->capture_image)->usage &
         (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC) == 0U) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the swapchain images cannot be read back; no capture");
        return;
    }
    if (nph_gpu_device_wait_idle(app->dev) != NPH_RESULT_OK) {
        *status = 1;
        return;
    }
    u32 w = 0U;
    u32 h = 0U;
    nph_gpu_swapchain_extent(app->swapchain, &w, &h);
    const NphGpuFormat f = nph_gpu_swapchain_format(app->swapchain);
    const bool bgra = f == NPH_GPU_FORMAT_B8G8R8A8_SRGB || f == NPH_GPU_FORMAT_B8G8R8A8_UNORM;
    const NphResult r = capture_write_bmp(
        app->opt.capture, (const u8 *)nph_gpu_buffer_map(app->dev, app->capture), w, h, bgra);
    if (r == NPH_RESULT_OK) {
        NPH_LOG_INFO(&LOG_SAMPLE, "captured the last frame to %s", app->opt.capture);
    } else {
        NPH_LOG_ERROR(&LOG_SAMPLE, "%s: %s", app->opt.capture, nph_result_string(r));
        *status = 1;
    }
}

static void run(App *app, int *status)
{
    NphRenderView view;
    nph_render_view_default(&view,
                            nph_render_camera_look_at(nph_dvec3(0.0, 2.0, 7.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 0.9F, 0.05F));
    view.ev100 = app->opt.ev100;
    view.tonemapper = app->opt.tonemapper;
    view.debug_view = app->opt.view;
    const NphVec3 center = nph_aabb_center(app->scene.bounds);
    bool paused = false;
    bool lines = app->opt.debug_lines;
    f32 distance = 7.5F;
    f32 angle = 0.35F;
    const u64 max = app->opt.max_frames;
    const NphStopwatch clock = nph_stopwatch_start();
    NphStopwatch report = nph_stopwatch_start();
    u64 index = 0U;
    while (!nph_window_should_close(app->window) && (max == 0U || index < max)) {
        handle_input(app, &view, &paused, &distance, &lines);
        if (!paused) {
            angle += 0.004F; // per frame: --frames runs are reproducible
        }
        const NphDVec3 target = nph_dvec3_from_vec3(nph_vec3(center.x, 0.6F, center.z));
        const NphDVec3 eye = nph_dvec3_add(target, nph_dvec3((f64)(distance * nph_f32_sin(angle)),
                                                             (f64)(distance * 0.38F),
                                                             (f64)(distance * nph_f32_cos(angle))));
        view.camera = nph_render_camera_look_at(eye, target, nph_vec3_unit_y(), 0.9F, 0.05F);
        if (lines) {
            draw_debug(app->renderer, &app->scene);
        }
        const bool capturing = app->opt.capture != NULL && index + 1U == max;
        if (!frame(app, &view, index, capturing, status)) {
            return;
        }
        ++index;
        if (nph_stopwatch_elapsed_ms(&report) > 2000.0) {
            nph_render_gpu_profiler_log(app->profiler);
            report = nph_stopwatch_start();
        }
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%llu frames in %.1f ms", (unsigned long long)index,
                 nph_stopwatch_elapsed_ms(&clock));
    nph_render_gpu_profiler_log(app->profiler);
    if (app->opt.profile != NULL &&
        nph_render_gpu_profiler_write_json(app->profiler, app->alloc, app->opt.profile) !=
            NPH_RESULT_OK) {
        *status = 1;
    }
    if (app->opt.capture != NULL) {
        write_capture(app, status);
    }
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof app);
    if (!parse(argc, argv, &app.opt)) {
        return usage();
    }
    app.alloc = nph_allocator_system();
    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = app.alloc;
    if (nph_log_init(&log_config) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogConsoleSink console;
    console.color = nph_platform_console_enable_vt();
    console.with_location = false;
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_INFO) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    if (nph_log_thread_start(&log_thread, 5U) != NPH_RESULT_OK) {
        return 1;
    }

    int status = 0;
    if (load_content(&app) != NPH_RESULT_OK) {
        status = 1;
    } else if (open_device(&app, &status) == NPH_RESULT_OK) {
        if (create_renderer(&app) == NPH_RESULT_OK) {
            NPH_LOG_INFO(&LOG_SAMPLE,
                         "%s: %u instances, %u lights; keys: 1/2/3 tonemapper, V view, "
                         "I lighting, D lines, Space pause, Escape quit",
                         nph_gpu_device_info(app.dev)->name, app.scene.instance_count,
                         app.scene.light_count);
            run(&app, &status);
        } else {
            status = 1;
        }
    }

    if (app.dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(app.dev));
        cooked_scene_unload(&app.scene);
        nph_gpu_buffer_destroy(app.dev, app.capture);
        nph_gpu_render_graph_destroy(app.graph);
        nph_render_gpu_profiler_destroy(app.profiler);
        nph_renderer_destroy(app.renderer);
        nph_gpu_swapchain_destroy(app.swapchain);
        const NphGpuValidationStats vs = nph_gpu_device_validation_stats(app.dev);
        if (vs.errors != 0U) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "%llu validation errors", (unsigned long long)vs.errors);
            status = 1;
        }
        nph_gpu_device_destroy(app.dev);
    } else {
        cooked_scene_unload(&app.scene);
    }
    nph_window_destroy(app.window);
    nph_window_system_shutdown();
    nph_asset_loader_destroy(app.loader);
    nph_vfs_destroy(app.vfs);
    nph_job_system_destroy(app.jobs);
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}
