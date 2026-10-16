// SPDX-License-Identifier: Apache-2.0
// nph_editor: the Nepheline editor (CLAUDE.md §8.13; docs/editor.md).
//
//     nph_editor --pack=showcase.npk --scene=level.nscene
//     nph_editor --game=game_sandbox.dll editor.layout=my_layout.ini
//     nph_editor editor.assets=my_game/assets editor.cooked=my_game/cooked
//
// The engine's options and cvars apply (--help, and `cvars` in the
// console); the editor adds editor.*. The edited world does not simulate:
// Play (F5) plays a copy of it, and Stop brings the edited one back.
//
// Exit status: 0, 1 on a failure, 2 for a bad command line, and 77 when
// the machine has no window system or no Vulkan device (CTest reports the
// smoke run as skipped).
#include <nepheline/core/log.h>
#include <nepheline/gpu/device.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/runtime/engine.h>

#include "editor.h"

#include <stdio.h>
#include <string.h>

#define EXIT_SKIPPED 77

// Log lines waiting for the console panel between two frames.
#define EDITOR_LOG_BACKLOG 4096U

static int run(const NphEngineConfig *config, EditorLogSink *log)
{
    EditorDesc ed_desc;
    memset(&ed_desc, 0, sizeof ed_desc);
    ed_desc.alloc = nph_allocator_system();
    ed_desc.log = log;
    Editor *ed = NULL;
    if (editor_create(&ed_desc, &ed) != NPH_RESULT_OK) {
        return 1;
    }
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.config = config;
    d.mode = NPH_ENGINE_MODE_WINDOWED;
    d.app_name = "Nepheline Editor";
    editor_engine_desc(ed, &d);
    NphEngine *engine = NULL;
    NphResult r = nph_engine_create(&d, &engine);
    if (r != NPH_RESULT_OK) {
        editor_destroy(ed);
        if (r == NPH_RESULT_UNSUPPORTED) {
            NPH_LOG_WARN(&LOG_EDITOR, "no window system or Vulkan device here; skipping");
            return EXIT_SKIPPED;
        }
        return 1;
    }
    int status = 1;
    r = editor_attach(ed, engine);
    if (r == NPH_RESULT_OK) {
        r = nph_engine_run(engine);
        status = r == NPH_RESULT_OK ? 0 : 1;
    }
    editor_detach(ed);
    NphGpuDevice *dev = nph_engine_device(engine);
    if (dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(dev));
        const NphGpuValidationStats vs = nph_gpu_device_validation_stats(dev);
        if (vs.errors != 0U) {
            NPH_LOG_ERROR(&LOG_EDITOR, "%llu validation errors", (unsigned long long)vs.errors);
            status = 1;
        }
    }
    nph_engine_destroy(engine);
    editor_destroy(ed);
    return status;
}

int main(int argc, char **argv)
{
    NphEngineConfig config;
    char error[NPH_ENGINE_CONFIG_ERROR_MAX];
    if (nph_engine_config_parse(argc, argv, &config, error, sizeof error) != NPH_RESULT_OK) {
        (void)fprintf(stderr, "nph_editor: %s\n\n%s", error, nph_engine_config_usage());
        return 2;
    }
    if (config.help) {
        (void)fputs(nph_engine_config_usage(), stdout);
        return 0;
    }
    if (config.headless) {
        (void)fputs("nph_editor: the editor needs a window (--headless is the player's)\n", stderr);
        return 2;
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
    EditorLogSink panel;
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    if (editor_log_sink_init(&panel, nph_allocator_system(), EDITOR_LOG_BACKLOG) != NPH_RESULT_OK) {
        nph_log_shutdown();
        return 1;
    }
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_INFO) != NPH_RESULT_OK ||
        nph_log_add_sink(editor_log_sink_fn, &panel, NPH_LOG_LEVEL_TRACE) != NPH_RESULT_OK ||
        nph_log_thread_start(&log_thread, 5U) != NPH_RESULT_OK) {
        nph_log_shutdown();
        editor_log_sink_free(&panel);
        return 1;
    }
    const int status = run(&config, &panel);
    nph_log_thread_stop(&log_thread);
    (void)nph_log_remove_sink(editor_log_sink_fn, &panel);
    nph_log_shutdown();
    editor_log_sink_free(&panel);
    return status;
}
