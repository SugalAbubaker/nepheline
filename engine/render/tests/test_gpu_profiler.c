// SPDX-License-Identifier: Apache-2.0
// The GPU profiler (render/gpu_profiler.h): per-pass windows fed from the
// render graph's timestamps, the frame total, the 99th percentile, reset,
// and the JSON report.
#include <nepheline/platform/file.h>
#include <nepheline/render/gpu_profiler.h>

#include "test_render_support.h"

typedef RenderFixture Profiler;

NTEST_SETUP(Profiler)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Profiler)
{
    test_render_teardown(f);
}

NTEST(gpu_profiler, create_checks_its_description)
{
    NphRenderGpuProfilerDesc d;
    memset(&d, 0, sizeof d);
    NphRenderGpuProfiler *p = NULL;
    NTEST_EXPECT_EQ_INT(nph_render_gpu_profiler_create(&d, &p), NPH_RESULT_INVALID_ARGUMENT);
    d.alloc = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(nph_render_gpu_profiler_create(&d, &p), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_render_gpu_profiler_frames(p), 0U);
    // Before any frame: only the (empty) frame total and span.
    NphRenderPassStats s[4];
    NTEST_ASSERT_EQ_UINT(nph_render_gpu_profiler_stats(p, s, 4U), 2U);
    NTEST_EXPECT_EQ_STR(s[0].name, "frame");
    NTEST_EXPECT_EQ_UINT(s[0].samples, 0U);
    NTEST_EXPECT_EQ_STR(s[1].name, "span");
    NTEST_EXPECT_EQ_UINT(s[1].samples, 0U);
    nph_render_gpu_profiler_destroy(p);
    nph_render_gpu_profiler_destroy(NULL);
}

NTEST_F(Profiler, collects_the_renderer_passes)
{
    REQUIRE_RENDERER(f);
    NphRenderGpuProfilerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.history = 8U;
    NphRenderGpuProfiler *p = NULL;
    NTEST_ASSERT_EQ_INT(nph_render_gpu_profiler_create(&d, &p), NPH_RESULT_OK);
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    u32 recorded = 0U;
    for (u32 frame = 0; frame < 12U; ++frame) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        recorded += nph_render_gpu_profiler_update(p, f->graph);
        // A second update in the same frame records nothing new.
        NTEST_EXPECT_EQ_UINT(nph_render_gpu_profiler_update(p, f->graph), 0U);
        nph_gpu_rg_begin(f->graph);
        NTEST_ASSERT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
    // Frames 0..9 completed while frames 2..11 began.
    NTEST_EXPECT_EQ_UINT(nph_render_gpu_profiler_frames(p), 10U);
    NTEST_EXPECT_TRUE(recorded >= 10U * 2U);
    NphRenderPassStats s[32];
    const u32 n = nph_render_gpu_profiler_stats(p, s, 32U);
    bool forward = false;
    bool tonemap = false;
    bool lut = false;
    for (u32 i = 0; i < n; ++i) {
        NTEST_EXPECT_TRUE(s[i].min_ms <= s[i].mean_ms + 1e-9 && s[i].mean_ms <= s[i].max_ms + 1e-9);
        NTEST_EXPECT_TRUE(s[i].p99_ms <= s[i].max_ms + 1e-9 && s[i].p99_ms >= s[i].min_ms - 1e-9);
        if (strcmp(s[i].name, "forward") == 0) {
            forward = true;
            NTEST_EXPECT_EQ_UINT(s[i].samples, 8U); // the window
            // The forward pass writes the HDR and depth targets.
            NTEST_EXPECT_EQ_UINT(s[i].write_bytes, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 12U);
        } else if (strcmp(s[i].name, "tonemap") == 0) {
            tonemap = true;
            // It reads the HDR target and the depth, and writes the target.
            NTEST_EXPECT_EQ_UINT(s[i].read_bytes, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 12U);
            NTEST_EXPECT_EQ_UINT(s[i].write_bytes, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U);
        } else if (strcmp(s[i].name, "brdf lut") == 0) {
            lut = true;
            NTEST_EXPECT_EQ_UINT(s[i].samples, 1U); // the first frame only
        }
    }
    NTEST_EXPECT_TRUE(forward && tonemap && lut);
    NTEST_ASSERT_TRUE(n >= 2U);
    NTEST_EXPECT_EQ_STR(s[n - 2U].name, "frame");
    NTEST_EXPECT_EQ_STR(s[n - 1U].name, "span");
    // One queue runs the passes one after another: the span holds them all
    // (and whatever separates them), so it is at least their sum, rounding
    // aside, and not much more.
    NTEST_EXPECT_EQ_UINT(s[n - 1U].samples, s[n - 2U].samples);
    NTEST_EXPECT_TRUE(s[n - 1U].mean_ms >= s[n - 2U].mean_ms * 0.999 - 1e-6);
    NTEST_EXPECT_TRUE(s[n - 1U].mean_ms <= s[n - 2U].mean_ms * 2.0 + 0.05);

    char path[512];
    snprintf(path, sizeof path, "%s/render_profiler_test.json", NPH_SHADER_DIR);
    NTEST_ASSERT_EQ_INT(nph_render_gpu_profiler_write_json(p, nph_allocator_system(), path),
                        NPH_RESULT_OK);
    void *text = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_allocator_system(), &text, &size),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(size > 0U);
    bool found = false;
    const char *t = (const char *)text;
    for (u64 i = 0; i + 9U < size && !found; ++i) {
        found = memcmp(t + i, "\"forward\"", 9U) == 0;
    }
    NTEST_EXPECT_TRUE(found);
    nph_file_free_all(nph_allocator_system(), text, size);
    NPH_IGNORE_RESULT(nph_fs_remove_file(path));

    nph_render_gpu_profiler_log(p);
    nph_render_gpu_profiler_reset(p);
    NTEST_EXPECT_EQ_UINT(nph_render_gpu_profiler_frames(p), 0U);
    NTEST_ASSERT_EQ_UINT(nph_render_gpu_profiler_stats(p, s, 32U), n);
    NTEST_EXPECT_EQ_UINT(s[0].samples, 0U);
    nph_render_gpu_profiler_destroy(p);
}
