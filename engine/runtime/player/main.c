// SPDX-License-Identifier: Apache-2.0
// nph_player: runs a game module on the engine (CLAUDE.md §8.12;
// docs/runtime.md "The player").
//
//     nph_player --game=game_sandbox.dll --pack=showcase.npk
//     nph_player --game=game_sandbox.so --headless --frames=600 sim.rate=120
//
// The world plays from the first frame. In development builds a rebuild of
// the game module is swapped in while it runs (game.hot_reload). Every
// option and cvar: --help, and `help` / `cvars` in the console (--script).
//
// Exit status: 0, 1 on a failure, 2 for a bad command line, and 77 when
// the machine has no window system or no Vulkan device (CTest reports the
// smoke run as skipped).
#include <nepheline/core/log.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>
#include <nepheline/runtime/engine.h>

#include <stdio.h>
#include <string.h>

#define EXIT_SKIPPED 77

NPH_LOG_CATEGORY_DEFINE(LOG_PLAYER, "player", NPH_LOG_LEVEL_INFO);

// The frame rate in the window's title, twice a second: the frames counted
// over the real time they took (with r.vsync=false the display's refresh
// rate does not cap it).
static NPH_CVAR_BOOL(g_title_fps, "player.title_fps", false,
                     "show the frame rate and the frame time in the window's title",
                     NPH_CVAR_FLAG_NONE);

typedef struct TitleClock {
    u64 since_ns; // when the frames being counted began (0: not yet)
    u64 frames;
} TitleClock;

// The engine's sync hook: once a frame, on the engine's thread.
static void title_fps(NphEngine *engine, void *user)
{
    TitleClock *tc = (TitleClock *)user;
    NphWindow *w = nph_engine_window(engine);
    if (w == NULL || !nph_cvar_get_bool(&g_title_fps)) {
        return;
    }
    const u64 now = nph_time_now_ns();
    if (tc->since_ns == 0U) {
        tc->since_ns = now;
        return;
    }
    tc->frames += 1U;
    const u64 elapsed = now - tc->since_ns;
    if (elapsed < NPH_TIME_NS_PER_S / 2U) {
        return;
    }
    const f64 seconds = (f64)elapsed / (f64)NPH_TIME_NS_PER_S;
    char title[96];
    (void)snprintf(title, sizeof title, "Nepheline - %.0f fps (%.2f ms)", (f64)tc->frames / seconds,
                   1000.0 * seconds / (f64)tc->frames);
    nph_window_set_title(w, title);
    tc->since_ns = now;
    tc->frames = 0U;
}

static int run(const NphEngineConfig *config)
{
    TitleClock clock;
    memset(&clock, 0, sizeof clock);
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.config = config;
    d.mode = NPH_ENGINE_MODE_WINDOWED;
    d.app_name = "Nepheline";
    d.play_on_start = 1;
    d.hooks.sync = title_fps;
    d.hooks.user = &clock;
    NphEngine *engine = NULL;
    NphResult r = nph_engine_create(&d, &engine);
    if (r == NPH_RESULT_UNSUPPORTED) {
        NPH_LOG_WARN(&LOG_PLAYER, "no window system or Vulkan device here; skipping");
        return EXIT_SKIPPED;
    }
    if (r != NPH_RESULT_OK) {
        return 1;
    }
    // Registered after creation: the command line's value waits for it.
    if (nph_cvar_register(nph_engine_cvars(engine), &g_title_fps) != NPH_RESULT_OK) {
        nph_engine_destroy(engine);
        return 1;
    }
    r = nph_engine_run(engine);
    int status = r == NPH_RESULT_OK ? 0 : 1;
    NphGpuDevice *dev = nph_engine_device(engine);
    if (dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(dev));
        const NphGpuValidationStats vs = nph_gpu_device_validation_stats(dev);
        if (vs.errors != 0U) {
            NPH_LOG_ERROR(&LOG_PLAYER, "%llu validation errors", (unsigned long long)vs.errors);
            status = 1;
        }
    }
    const NphEngineFrameStats *s = nph_engine_frame_stats(engine);
    NPH_LOG_INFO(&LOG_PLAYER, "%llu frames, %llu simulation steps", (unsigned long long)s->frame,
                 (unsigned long long)s->total_steps);
    NPH_LOG_INFO(
        &LOG_PLAYER,
        "the last frame: %.2f ms; %u instances (%u waiting, %u failed), %u lights, %u cells "
        "(%u loading); proxy levels %u, %.1f MB; texture levels %u, %.1f MB",
        s->frame_ms, s->scene.instances, s->scene.pending, s->scene.failed, s->scene.lights,
        s->scene.cells, s->scene.cells_loading, s->scene.proxy_levels,
        (f64)s->scene.proxy_bytes / 1.0e6, s->scene.texture_levels,
        (f64)s->scene.texture_bytes / 1.0e6);
    nph_engine_destroy(engine);
    return status;
}

int main(int argc, char **argv)
{
    NphEngineConfig config;
    char error[NPH_ENGINE_CONFIG_ERROR_MAX];
    if (nph_engine_config_parse(argc, argv, &config, error, sizeof error) != NPH_RESULT_OK) {
        (void)fprintf(stderr, "nph_player: %s\n\n%s", error, nph_engine_config_usage());
        return 2;
    }
    if (config.help) {
        (void)fputs(nph_engine_config_usage(), stdout);
        return 0;
    }
    NphLogConfig lc;
    memset(&lc, 0, sizeof lc);
    lc.alloc = nph_allocator_system();
    if (nph_log_init(&lc) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogConsoleSink console;
    console.color = nph_platform_console_enable_vt();
    console.with_location = false;
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_INFO) != NPH_RESULT_OK ||
        nph_log_thread_start(&log_thread, 5U) != NPH_RESULT_OK) {
        nph_log_shutdown();
        return 1;
    }
    const int status = run(&config);
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}
