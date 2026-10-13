// SPDX-License-Identifier: Apache-2.0
// The swapchain on a real window: acquire, render, present, resize, vsync,
// and presentation through the render graph. Skipped on a headless machine
// or without Vulkan surface support.
#include <nepheline/platform/window.h>

#include "test_gpu_support.h"

typedef struct SwapFixture {
    NphWindow *window;
    NphGpuDevice *dev;
    NphGpuSwapchain *swapchain;
} SwapFixture;

NTEST_SETUP(SwapFixture)
{
    if (nph_window_system_init() != NPH_RESULT_OK) {
        return;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = "Nepheline gpu test";
    wd.width = 320U;
    wd.height = 240U;
    if (nph_window_create(nph_allocator_system(), &wd, &f->window) != NPH_RESULT_OK) {
        return;
    }
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.headless = 0;
    if (nph_gpu_device_create(&dd, &f->dev) != NPH_RESULT_OK) {
        f->dev = NULL;
        return;
    }
    NphGpuSwapchainDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.window = f->window;
    sd.vsync = 0;
    sd.prefer_srgb = 1;
    sd.name = "test swapchain";
    if (nph_gpu_swapchain_create(f->dev, &sd, &f->swapchain) != NPH_RESULT_OK) {
        f->swapchain = NULL;
    }
}

NTEST_TEARDOWN(SwapFixture)
{
    nph_gpu_swapchain_destroy(f->swapchain);
    test_gpu_destroy(f->dev);
    nph_window_destroy(f->window);
    nph_window_system_shutdown();
}

#define REQUIRE_SWAPCHAIN(f)                                                                       \
    do {                                                                                           \
        if ((f)->swapchain == NULL) {                                                              \
            NTEST_SKIP("no window, no Vulkan device, or no presentable surface");                  \
        }                                                                                          \
    } while (0)

// One frame: acquire, clear to `color`, present. Returns false when the
// window had no area.
static bool clear_frame(SwapFixture *f, const NphGpuClearValue *color)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    nph_window_poll_events();
    NphGpuImage image = NPH_GPU_IMAGE_NULL;
    const NphResult r = nph_gpu_swapchain_acquire(f->swapchain, &image);
    if (r == NPH_RESULT_EMPTY) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
        return false;
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_gpu_image_is_valid(f->dev, image));
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, image, 0U), NPH_GPU_LAYOUT_UNDEFINED);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "clear frame", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, image, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_TRANSFER,
                              (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_TRANSFER_DST);
    nph_gpu_cmd_clear_color_image(cmd, image, NPH_GPU_LAYOUT_TRANSFER_DST, color, 0U, 1U, 0U, 1U);
    nph_gpu_cmd_image_barrier(cmd, image, (u32)NPH_GPU_STAGE_TRANSFER,
                              (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_TRANSFER_DST,
                              (u32)NPH_GPU_STAGE_ALL, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_PRESENT);
    nph_gpu_cmd_end(cmd);
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.cmds = &cmd;
    sd.cmd_count = 1U;
    sd.swapchain = f->swapchain;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_swapchain_present(f->swapchain), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    return true;
}

NTEST_F(SwapFixture, presents_resizes_and_toggles_vsync)
{
    REQUIRE_SWAPCHAIN(f);
    NTEST_ASSERT_NE_INT(nph_gpu_swapchain_format(f->swapchain), NPH_GPU_FORMAT_UNDEFINED);
    NTEST_ASSERT_GE_UINT(nph_gpu_swapchain_image_count(f->swapchain), 2U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_swapchain_generation(f->swapchain), 1U);
    NTEST_ASSERT_FALSE(nph_gpu_swapchain_vsync(f->swapchain));
    NTEST_LOG("swapchain: %s, %s, %u images",
              nph_gpu_format_name(nph_gpu_swapchain_format(f->swapchain)),
              nph_gpu_swapchain_present_mode_name(f->swapchain),
              nph_gpu_swapchain_image_count(f->swapchain));
    u32 w = 0U;
    u32 h = 0U;
    nph_gpu_swapchain_extent(f->swapchain, &w, &h);
    u32 fw = 0U;
    u32 fh = 0U;
    nph_window_get_framebuffer_size(f->window, &fw, &fh);
    NTEST_ASSERT_EQ_UINT(w, fw);
    NTEST_ASSERT_EQ_UINT(h, fh);

    const NphGpuClearValue blue = nph_gpu_clear_color(0.1F, 0.2F, 0.9F, 1.0F);
    u32 presented = 0U;
    for (u32 i = 0; i < 6U; ++i) {
        if (clear_frame(f, &blue)) {
            ++presented;
        }
    }
    NTEST_ASSERT_GE_UINT(presented, 1U);

    // Resize: the next acquire recreates the swapchain at the new size.
    const u32 gen = nph_gpu_swapchain_generation(f->swapchain);
    nph_window_set_size(f->window, 400U, 300U);
    nph_window_poll_events();
    nph_window_get_framebuffer_size(f->window, &fw, &fh);
    if (fw != w || fh != h) {
        for (u32 i = 0; i < 3U; ++i) {
            (void)clear_frame(f, &blue);
        }
        nph_gpu_swapchain_extent(f->swapchain, &w, &h);
        NTEST_ASSERT_EQ_UINT(w, fw);
        NTEST_ASSERT_EQ_UINT(h, fh);
        NTEST_ASSERT_GT_UINT(nph_gpu_swapchain_generation(f->swapchain), gen);
    } else {
        NTEST_LOG("the window system kept the old size; resize not exercised");
    }

    // Vsync toggles the present mode (recreation at the next acquire).
    const u32 gen2 = nph_gpu_swapchain_generation(f->swapchain);
    nph_gpu_swapchain_set_vsync(f->swapchain, true);
    nph_gpu_swapchain_set_vsync(f->swapchain, true); // idempotent
    NTEST_ASSERT_TRUE(nph_gpu_swapchain_vsync(f->swapchain));
    if (clear_frame(f, &blue)) {
        NTEST_ASSERT_EQ_UINT(nph_gpu_swapchain_generation(f->swapchain), gen2 + 1U);
        NTEST_ASSERT_EQ_STR(nph_gpu_swapchain_present_mode_name(f->swapchain), "fifo");
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
}

NTEST_F(SwapFixture, misuse_is_reported)
{
    REQUIRE_SWAPCHAIN(f);
    NTEST_ASSERT_EQ_INT(nph_gpu_swapchain_present(f->swapchain), NPH_RESULT_INVALID_ARGUMENT);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, NULL, &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_end(cmd);
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.cmds = &cmd;
    sd.cmd_count = 1U;
    sd.swapchain = f->swapchain; // nothing acquired
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NphGpuSwapchainDesc scd;
    memset(&scd, 0, sizeof scd);
    NphGpuSwapchain *sc = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_swapchain_create(f->dev, &scd, &sc), NPH_RESULT_INVALID_ARGUMENT);
    nph_gpu_swapchain_destroy(NULL);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
}

NTEST(gpu_swapchain, headless_devices_refuse_swapchains)
{
    if (nph_window_system_init() != NPH_RESULT_OK) {
        NTEST_SKIP("no display available");
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.width = 64U;
    wd.height = 64U;
    NphWindow *window = NULL;
    if (nph_window_create(nph_allocator_system(), &wd, &window) != NPH_RESULT_OK) {
        nph_window_system_shutdown();
        NTEST_SKIP("no window");
    }
    NphGpuDevice *dev = test_gpu_create(NULL);
    if (dev != NULL) {
        NphGpuSwapchainDesc scd;
        memset(&scd, 0, sizeof scd);
        scd.window = window;
        NphGpuSwapchain *sc = NULL;
        NTEST_ASSERT_EQ_INT(nph_gpu_swapchain_create(dev, &scd, &sc), NPH_RESULT_UNSUPPORTED);
        NTEST_ASSERT_NULL(sc);
        test_gpu_destroy(dev);
    }
    nph_window_destroy(window);
    nph_window_system_shutdown();
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
}

typedef struct NoopPass {
    u32 ran;
} NoopPass;

static void noop_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    NPH_UNUSED(cmd);
    ((NoopPass *)user)->ran += 1U;
}

NTEST_F(SwapFixture, render_graph_presents)
{
    REQUIRE_SWAPCHAIN(f);
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    NphGpuRenderGraph *g = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(f->dev, &gd, &g), NPH_RESULT_OK);
    NoopPass noop = {0U};
    u32 presented = 0U;
    for (u32 frame = 0; frame < 4U; ++frame) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        nph_window_poll_events();
        NphGpuImage image = NPH_GPU_IMAGE_NULL;
        const NphResult r = nph_gpu_swapchain_acquire(f->swapchain, &image);
        if (r == NPH_RESULT_EMPTY) {
            NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
            continue;
        }
        NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
        nph_gpu_rg_begin(g);
        NphGpuRgImage back = nph_gpu_rg_import_image(g, image);
        NphGpuRgPassDesc pd;
        memset(&pd, 0, sizeof pd);
        pd.name = "clear";
        pd.type = NPH_GPU_RG_PASS_GRAPHICS;
        pd.execute = noop_execute;
        pd.user = &noop;
        NphGpuRgPass *pass = nph_gpu_rg_add_pass(g, &pd);
        NphGpuRgAttachment att;
        memset(&att, 0, sizeof att);
        att.load = NPH_GPU_LOAD_OP_CLEAR;
        att.store = NPH_GPU_STORE_OP_STORE;
        att.clear = nph_gpu_clear_color(0.9F, 0.4F, 0.1F, 1.0F);
        nph_gpu_rg_color_attachment(pass, back, &att);
        nph_gpu_rg_present(g, back, f->swapchain);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, image, 0U), NPH_GPU_LAYOUT_PRESENT);
        NTEST_ASSERT_EQ_INT(nph_gpu_swapchain_present(f->swapchain), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
        ++presented;
    }
    NTEST_ASSERT_EQ_UINT(noop.ran, presented);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_gpu_render_graph_destroy(g);
}
