// SPDX-License-Identifier: Apache-2.0
// gpu_triangle: the bring-up triangle (CLAUDE.md §8.6 stage 1 "clear
// screen -> triangle"). A window, a device with validation, a swapchain,
// one graphics pipeline without any buffers, and a one-pass render graph
// that clears the swapchain image, draws, and presents.
//
//     gpu_triangle [--frames=N] [--vsync]
//
// Exit status 77 means the machine has no window system or no Vulkan
// device (CTest reports the sample as skipped); 1 means a validation error
// was reported, which the Phase 5 exit criteria forbid.

#include <nepheline/gpu/render_graph.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "gpu_triangle", NPH_LOG_LEVEL_INFO);

#define EXIT_SKIPPED 77

typedef struct Triangle {
    NphGpuPipeline pipeline;
} Triangle;

static void draw_triangle(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    const Triangle *t = (const Triangle *)user;
    nph_gpu_cmd_bind_pipeline(cmd, t->pipeline);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
}

static NphResult load_shader(NphGpuDevice *dev, const char *file, NphGpuShader *out)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.spv", NPH_SHADER_DIR, file);
    return nph_gpu_shader_load(dev, path, file, out);
}

int main(int argc, char **argv)
{
    u64 max_frames = 0U;
    bool vsync = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--frames=", 9U) == 0) {
            max_frames = strtoull(argv[i] + 9, NULL, 10);
        } else if (strcmp(argv[i], "--vsync") == 0) {
            vsync = true;
        } else {
            fprintf(stderr, "usage: gpu_triangle [--frames=N] [--vsync]\n");
            return 2;
        }
    }

    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = nph_allocator_system();
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
    NphWindow *window = NULL;
    NphGpuDevice *dev = NULL;
    NphGpuSwapchain *swapchain = NULL;
    NphGpuRenderGraph *graph = NULL;
    NphGpuShader vert = NPH_GPU_SHADER_NULL;
    NphGpuShader frag = NPH_GPU_SHADER_NULL;
    Triangle triangle = {NPH_GPU_PIPELINE_NULL};

    if (nph_window_system_init() != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no window system available; skipping");
        status = EXIT_SKIPPED;
        goto done;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = "Nepheline gpu_triangle";
    if (nph_window_create(nph_allocator_system(), &wd, &window) != NPH_RESULT_OK) {
        status = EXIT_SKIPPED;
        goto done;
    }
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.app_name = "gpu_triangle";
    const NphResult dr = nph_gpu_device_create(&dd, &dev);
    if (dr != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no Vulkan device (%s); skipping", nph_result_string(dr));
        status = dr == NPH_RESULT_UNSUPPORTED ? EXIT_SKIPPED : 1;
        goto done;
    }
    NphGpuSwapchainDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.window = window;
    sd.vsync = vsync;
    sd.prefer_srgb = 1;
    if (nph_gpu_swapchain_create(dev, &sd, &swapchain) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the window cannot be presented to; skipping");
        status = EXIT_SKIPPED;
        goto done;
    }
    if (load_shader(dev, "triangle.vert", &vert) != NPH_RESULT_OK ||
        load_shader(dev, "triangle.frag", &frag) != NPH_RESULT_OK) {
        status = 1;
        goto done;
    }
    NphGpuGraphicsPipelineDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.vertex = vert;
    pd.fragment = frag;
    pd.color_formats[0] = nph_gpu_swapchain_format(swapchain);
    pd.color_count = 1U;
    pd.name = "triangle";
    if (nph_gpu_pipeline_create_graphics(dev, &pd, &triangle.pipeline) != NPH_RESULT_OK) {
        status = 1;
        goto done;
    }
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    gd.name = "triangle graph";
    if (nph_gpu_render_graph_create(dev, &gd, &graph) != NPH_RESULT_OK) {
        status = 1;
        goto done;
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%s, %s present mode; Escape quits", nph_gpu_device_info(dev)->name,
                 nph_gpu_swapchain_present_mode_name(swapchain));

    u64 frame = 0U;
    NphStopwatch run = nph_stopwatch_start();
    NphStopwatch report = nph_stopwatch_start();
    u64 report_frame = 0U;
    while (!nph_window_should_close(window) && (max_frames == 0U || frame < max_frames)) {
        if (nph_gpu_frame_begin(dev) != NPH_RESULT_OK) {
            status = 1;
            break;
        }
        nph_window_poll_events();
        NphWindowEvent e;
        while (nph_window_pop_event(window, &e)) {
            if (e.type == NPH_WINDOW_EVENT_KEY && e.u.key.key == NPH_KEY_ESCAPE &&
                e.u.key.action == NPH_KEY_ACTION_PRESS) {
                nph_window_set_should_close(window, true);
            }
        }
        NphGpuImage backbuffer = NPH_GPU_IMAGE_NULL;
        const NphResult ar = nph_gpu_swapchain_acquire(swapchain, &backbuffer);
        if (ar == NPH_RESULT_EMPTY) {
            (void)nph_gpu_frame_end(dev);
            nph_thread_sleep_ns(16U * NPH_TIME_NS_PER_MS);
            continue;
        }
        if (ar != NPH_RESULT_OK) {
            status = 1;
            (void)nph_gpu_frame_end(dev);
            break;
        }
        nph_gpu_rg_begin(graph);
        NphGpuRgImage target = nph_gpu_rg_import_image(graph, backbuffer);
        NphGpuRgPassDesc pass_desc;
        memset(&pass_desc, 0, sizeof pass_desc);
        pass_desc.name = "triangle";
        pass_desc.type = NPH_GPU_RG_PASS_GRAPHICS;
        pass_desc.execute = draw_triangle;
        pass_desc.user = &triangle;
        NphGpuRgPass *pass = nph_gpu_rg_add_pass(graph, &pass_desc);
        NphGpuRgAttachment att;
        memset(&att, 0, sizeof att);
        att.load = NPH_GPU_LOAD_OP_CLEAR;
        att.store = NPH_GPU_STORE_OP_STORE;
        att.clear = nph_gpu_clear_color(0.05F, 0.05F, 0.08F, 1.0F);
        nph_gpu_rg_color_attachment(pass, target, &att);
        nph_gpu_rg_present(graph, target, swapchain);
        if (nph_gpu_rg_compile(graph) != NPH_RESULT_OK ||
            nph_gpu_rg_execute(graph) != NPH_RESULT_OK ||
            nph_gpu_swapchain_present(swapchain) != NPH_RESULT_OK ||
            nph_gpu_frame_end(dev) != NPH_RESULT_OK) {
            status = 1;
            break;
        }
        ++frame;
        if (nph_stopwatch_elapsed_ms(&report) >= 1000.0) {
            NphGpuRgPassTiming timings[4];
            const u32 n = nph_gpu_rg_timings(graph, timings, 4U, NULL);
            NPH_LOG_INFO(&LOG_SAMPLE, "%.0f fps, pass '%s' %.3f ms on the GPU",
                         (f64)(frame - report_frame) / (nph_stopwatch_elapsed_ms(&report) / 1000.0),
                         n != 0U ? timings[0].name : "?",
                         n != 0U ? (f64)timings[0].gpu_ns / 1.0e6 : 0.0);
            report = nph_stopwatch_start();
            report_frame = frame;
        }
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%llu frames in %.1f ms", (unsigned long long)frame,
                 nph_stopwatch_elapsed_ms(&run));

done:
    if (dev != NULL) {
        (void)nph_gpu_device_wait_idle(dev);
        nph_gpu_render_graph_destroy(graph);
        nph_gpu_pipeline_destroy(dev, triangle.pipeline);
        nph_gpu_shader_destroy(dev, vert);
        nph_gpu_shader_destroy(dev, frag);
        nph_gpu_swapchain_destroy(swapchain);
        const NphGpuValidationStats v = nph_gpu_device_validation_stats(dev);
        if (v.errors != 0U) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "%llu validation errors", (unsigned long long)v.errors);
            status = 1;
        }
        nph_gpu_device_destroy(dev);
    }
    nph_window_destroy(window);
    nph_window_system_shutdown();
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}
