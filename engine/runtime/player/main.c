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
#include <nepheline/runtime/engine.h>

#include <stdio.h>
#include <string.h>

#define EXIT_SKIPPED 77

NPH_LOG_CATEGORY_DEFINE(LOG_PLAYER, "player", NPH_LOG_LEVEL_INFO);

static int run(const NphEngineConfig *config)
{
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.config = config;
    d.mode = NPH_ENGINE_MODE_WINDOWED;
    d.app_name = "Nepheline";
    d.play_on_start = 1;
    NphEngine *engine = NULL;
    NphResult r = nph_engine_create(&d, &engine);
    if (r == NPH_RESULT_UNSUPPORTED) {
        NPH_LOG_WARN(&LOG_PLAYER, "no window system or Vulkan device here; skipping");
        return EXIT_SKIPPED;
    }
    if (r != NPH_RESULT_OK) {
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
