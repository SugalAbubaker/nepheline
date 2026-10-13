// SPDX-License-Identifier: Apache-2.0
// Command recording: fills, copies, clears, barriers, mip generation,
// per-thread pools, labels.
#include "test_gpu_support.h"

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

NTEST_F(GpuFixture, fill_copy_and_read_back)
{
    REQUIRE_DEVICE(f);
    NphGpuBuffer buf = test_make_buffer(f->dev, 1024U,
                                        (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
                                            (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                                        NPH_GPU_MEMORY_DEVICE, "filled");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "fill", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_begin_label(cmd, "fill", 1.0F, 0.0F, 0.0F);
    nph_gpu_cmd_fill_buffer(cmd, buf, 0U, NPH_GPU_WHOLE_SIZE, 0xABABABABU);
    nph_gpu_cmd_buffer_barrier(cmd, buf, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_WRITE);
    const u32 words[4] = {1U, 2U, 3U, 4U};
    nph_gpu_cmd_update_buffer(cmd, buf, 16U, words, sizeof words);
    nph_gpu_cmd_end_label(cmd);
    test_submit_wait(f->dev, cmd);
    u32 out[256];
    test_read_buffer(f->dev, buf, 0U, sizeof out, out);
    NTEST_ASSERT_EQ_UINT(out[0], 0xABABABABU);
    NTEST_ASSERT_EQ_UINT(out[3], 0xABABABABU);
    NTEST_ASSERT_EQ_UINT(out[4], 1U);
    NTEST_ASSERT_EQ_UINT(out[7], 4U);
    NTEST_ASSERT_EQ_UINT(out[8], 0xABABABABU);
    NTEST_ASSERT_EQ_UINT(out[255], 0xABABABABU);
    nph_gpu_buffer_destroy(f->dev, buf);
}

NTEST_F(GpuFixture, clear_image_and_read_back)
{
    REQUIRE_DEVICE(f);
    NphGpuImage img = test_make_image_2d(
        f->dev, 16U, 8U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC, "cleared");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "clear", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, img, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_TRANSFER,
                              (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_TRANSFER_DST);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, 0U), NPH_GPU_LAYOUT_TRANSFER_DST);
    const NphGpuClearValue red = nph_gpu_clear_color(1.0F, 0.0F, 0.0F, 1.0F);
    nph_gpu_cmd_clear_color_image(cmd, img, NPH_GPU_LAYOUT_TRANSFER_DST, &red, 0U, NPH_GPU_ALL_MIPS,
                                  0U, NPH_GPU_ALL_LAYERS);
    nph_gpu_cmd_image_barrier(cmd, img, (u32)NPH_GPU_STAGE_TRANSFER,
                              (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_TRANSFER_DST,
                              (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_READ,
                              NPH_GPU_LAYOUT_TRANSFER_SRC);
    test_submit_wait(f->dev, cmd);
    u8 pixels[16U * 8U * 4U];
    test_read_image(f->dev, img, 0U, pixels, sizeof pixels);
    for (u32 i = 0; i < 16U * 8U; ++i) {
        NTEST_ASSERT_TRUE(test_pixel_near(&pixels[i * 4U], 255U, 0U, 0U, 255U, 0U));
    }
    nph_gpu_image_destroy(f->dev, img);
}

NTEST_F(GpuFixture, depth_clear_copy_and_blit)
{
    REQUIRE_DEVICE(f);
    NphGpuImageDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.format = NPH_GPU_FORMAT_D32_SFLOAT;
    dd.width = 8U;
    dd.height = 8U;
    dd.mip_count = 1U;
    dd.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
               (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    NphGpuImage depth = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &dd, &depth), NPH_RESULT_OK);
    NphGpuImage a = test_make_image_2d(
        f->dev, 8U, 8U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC, "a");
    NphGpuImage b = test_make_image_2d(
        f->dev, 8U, 8U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC, "b");
    NphGpuImage c = test_make_image_2d(
        f->dev, 4U, 4U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC, "c");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "ops", &cmd),
                        NPH_RESULT_OK);
    const NphGpuImage all[3] = {depth, a, b};
    for (u32 i = 0; i < 3U; ++i) {
        nph_gpu_cmd_image_barrier(cmd, all[i], (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                                  NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_TRANSFER,
                                  (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_TRANSFER_DST);
    }
    nph_gpu_cmd_image_barrier(cmd, c, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_TRANSFER,
                              (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_TRANSFER_DST);
    const NphGpuClearValue dz = nph_gpu_clear_depth(0.5F, 0U);
    nph_gpu_cmd_clear_depth_image(cmd, depth, NPH_GPU_LAYOUT_TRANSFER_DST, &dz, 0U, 1U, 0U, 1U);
    const NphGpuClearValue green = nph_gpu_clear_color(0.0F, 1.0F, 0.0F, 1.0F);
    nph_gpu_cmd_clear_color_image(cmd, a, NPH_GPU_LAYOUT_TRANSFER_DST, &green, 0U, 1U, 0U, 1U);
    nph_gpu_cmd_image_barrier(cmd, a, (u32)NPH_GPU_STAGE_TRANSFER,
                              (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_TRANSFER_DST,
                              (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_READ,
                              NPH_GPU_LAYOUT_TRANSFER_SRC);
    nph_gpu_cmd_copy_image(cmd, a, 0U, NPH_GPU_LAYOUT_TRANSFER_SRC, b, 0U,
                           NPH_GPU_LAYOUT_TRANSFER_DST);
    nph_gpu_cmd_blit_image(cmd, a, 0U, NPH_GPU_LAYOUT_TRANSFER_SRC, c, 0U,
                           NPH_GPU_LAYOUT_TRANSFER_DST, NPH_GPU_FILTER_LINEAR);
    test_submit_wait(f->dev, cmd);
    u8 pb[8U * 8U * 4U];
    test_read_image(f->dev, b, 0U, pb, sizeof pb);
    NTEST_ASSERT_TRUE(test_pixel_near(&pb[0], 0U, 255U, 0U, 255U, 0U));
    u8 pc[4U * 4U * 4U];
    test_read_image(f->dev, c, 0U, pc, sizeof pc);
    NTEST_ASSERT_TRUE(test_pixel_near(&pc[15U * 4U], 0U, 255U, 0U, 255U, 1U));
    f32 dvals[64];
    test_read_image(f->dev, depth, 0U, dvals, sizeof dvals);
    NTEST_ASSERT_NEAR_F32(dvals[0], 0.5F, 0.0F);
    NTEST_ASSERT_NEAR_F32(dvals[63], 0.5F, 0.0F);
    nph_gpu_image_destroy(f->dev, depth);
    nph_gpu_image_destroy(f->dev, a);
    nph_gpu_image_destroy(f->dev, b);
    nph_gpu_image_destroy(f->dev, c);
}

NTEST_F(GpuFixture, generate_mips_averages)
{
    REQUIRE_DEVICE(f);
    NphGpuImage img = test_make_image_2d(f->dev, 8U, 8U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 0U,
                                         (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
                                             (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC |
                                             (u32)NPH_GPU_IMAGE_USAGE_SAMPLED,
                                         "mipped");
    NTEST_ASSERT_EQ_UINT(nph_gpu_image_desc(f->dev, img)->mip_count, 4U);
    // Mip 0: left half white, right half black.
    u8 texels[8U * 8U * 4U];
    for (u32 y = 0; y < 8U; ++y) {
        for (u32 x = 0; x < 8U; ++x) {
            const u8 v = x < 4U ? 255U : 0U;
            u8 *p = &texels[(y * 8U + x) * 4U];
            p[0] = v;
            p[1] = v;
            p[2] = v;
            p[3] = 255U;
        }
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, img, 0U, 0U, 1U, texels, sizeof texels),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, 0U), NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "mips", &cmd),
                        NPH_RESULT_OK);
    NphGpuImageBarrier to_src;
    memset(&to_src, 0, sizeof to_src);
    to_src.image = img;
    to_src.src_stages = (u32)NPH_GPU_STAGE_ALL;
    to_src.src_access = (u32)NPH_GPU_ACCESS_MEMORY_READ;
    to_src.old_layout = NPH_GPU_LAYOUT_SHADER_READ_ONLY;
    to_src.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
    to_src.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
    to_src.new_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
    to_src.mip_count = 1U;
    to_src.layer_count = NPH_GPU_ALL_LAYERS;
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.images = &to_src;
    bd.image_count = 1U;
    nph_gpu_cmd_barrier(cmd, &bd);
    nph_gpu_cmd_generate_mips(cmd, img, NPH_GPU_LAYOUT_SHADER_READ_ONLY,
                              (u32)NPH_GPU_STAGE_FRAGMENT_SHADER, (u32)NPH_GPU_ACCESS_SHADER_READ);
    test_submit_wait(f->dev, cmd);
    for (u32 m = 0; m < 4U; ++m) {
        NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, m), NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    }
    u8 mip3[4];
    test_read_image(f->dev, img, 3U, mip3, sizeof mip3); // 1x1: the average, mid grey
    NTEST_ASSERT_TRUE(test_pixel_near(mip3, 128U, 128U, 128U, 255U, 2U));
    u8 mip1[4U * 4U * 4U];
    test_read_image(f->dev, img, 1U, mip1, sizeof mip1); // 4x4: still white left, black right
    NTEST_ASSERT_TRUE(test_pixel_near(&mip1[0], 255U, 255U, 255U, 255U, 1U));
    NTEST_ASSERT_TRUE(test_pixel_near(&mip1[3U * 4U], 0U, 0U, 0U, 255U, 1U));
    nph_gpu_image_destroy(f->dev, img);
}

// ---------------------------------------------------------------------------
// Per-thread command pools
// ---------------------------------------------------------------------------

typedef struct RecordThread {
    NphThread thread;
    NphGpuDevice *dev;
    NphGpuBuffer buffer;
    u32 index;
    NphGpuCmd *cmd;
    NphResult result;
} RecordThread;

static void record_thread_main(void *user)
{
    RecordThread *t = (RecordThread *)user;
    t->result = nph_gpu_cmd_begin(t->dev, NPH_GPU_QUEUE_GRAPHICS, "thread", &t->cmd);
    if (t->result != NPH_RESULT_OK) {
        return;
    }
    nph_gpu_cmd_fill_buffer(t->cmd, t->buffer, (u64)t->index * 256U, 256U,
                            0x10000U * t->index + 1U);
    nph_gpu_cmd_end(t->cmd);
}

NTEST_F(GpuFixture, threads_record_into_their_own_pools)
{
    REQUIRE_DEVICE(f);
    NphGpuBuffer buf = test_make_buffer(f->dev, 8U * 256U,
                                        (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
                                            (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                                        NPH_GPU_MEMORY_DEVICE, "per-thread fills");
    RecordThread threads[8];
    for (u32 i = 0; i < 8U; ++i) {
        threads[i].dev = f->dev;
        threads[i].buffer = buf;
        threads[i].index = i;
        threads[i].cmd = NULL;
        threads[i].result = NPH_RESULT_UNKNOWN;
        NphThreadDesc td;
        td.fn = record_thread_main;
        td.user = &threads[i];
        td.name = "record";
        td.stack_size = 0U;
        NTEST_ASSERT_EQ_INT(nph_thread_create(&threads[i].thread, &td), NPH_RESULT_OK);
    }
    NphGpuCmd *cmds[8];
    for (u32 i = 0; i < 8U; ++i) {
        nph_thread_join(&threads[i].thread);
        NTEST_ASSERT_EQ_INT(threads[i].result, NPH_RESULT_OK);
        cmds[i] = threads[i].cmd;
    }
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.cmds = cmds;
    sd.cmd_count = 8U;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    u32 out[8U * 64U];
    test_read_buffer(f->dev, buf, 0U, sizeof out, out);
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_EQ_UINT(out[i * 64U], 0x10000U * i + 1U);
        NTEST_ASSERT_EQ_UINT(out[i * 64U + 63U], 0x10000U * i + 1U);
    }
    nph_gpu_buffer_destroy(f->dev, buf);
}

NTEST(gpu_commands, pools_are_bounded_per_thread_and_frame)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.max_commands_per_thread = 2U;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphGpuCmd *a = NULL;
    NphGpuCmd *b = NULL;
    NphGpuCmd *c = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "a", &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "b", &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "c", &c), NPH_RESULT_FULL);
    NTEST_ASSERT_NULL(c);
    nph_gpu_cmd_end(a);
    nph_gpu_cmd_end(b);
    // The next frame in the same slot resets the pool.
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "c", &c), NPH_RESULT_OK);
    nph_gpu_cmd_end(c);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(dev), NPH_RESULT_OK);
    test_gpu_destroy(dev);
}

NTEST(gpu_commands, thread_table_is_bounded)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.max_command_threads = 1U;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphGpuCmd *main_cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "main", &main_cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_end(main_cmd);
    RecordThread t;
    t.dev = dev;
    t.buffer = NPH_GPU_BUFFER_NULL;
    t.index = 0U;
    t.cmd = NULL;
    t.result = NPH_RESULT_OK;
    NphThreadDesc td;
    td.fn = record_thread_main;
    td.user = &t;
    td.name = "second";
    td.stack_size = 0U;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t.thread, &td), NPH_RESULT_OK);
    nph_thread_join(&t.thread);
    NTEST_ASSERT_EQ_INT(t.result, NPH_RESULT_FULL);
    test_gpu_destroy(dev);
}

NTEST_F(GpuFixture, memory_barriers_and_queue_transfers_record)
{
    REQUIRE_DEVICE(f);
    NphGpuImage img = test_make_image_2d(
        f->dev, 4U, 4U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 2U,
        (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED, "barriers");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "barriers", &cmd),
                        NPH_RESULT_OK);
    NphGpuMemoryBarrier mem;
    mem.src_stages = (u32)NPH_GPU_STAGE_TRANSFER;
    mem.src_access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
    mem.dst_stages = (u32)NPH_GPU_STAGE_COMPUTE;
    mem.dst_access = (u32)NPH_GPU_ACCESS_SHADER_READ;
    NphGpuImageBarrier images[2];
    memset(images, 0, sizeof images);
    for (u32 i = 0; i < 2U; ++i) {
        images[i].image = img;
        images[i].src_stages = (u32)NPH_GPU_STAGE_NONE;
        images[i].old_layout = NPH_GPU_LAYOUT_UNDEFINED;
        images[i].dst_stages = (u32)NPH_GPU_STAGE_FRAGMENT_SHADER;
        images[i].dst_access = (u32)NPH_GPU_ACCESS_SHADER_READ;
        images[i].new_layout = i == 0U ? NPH_GPU_LAYOUT_SHADER_READ_ONLY : NPH_GPU_LAYOUT_GENERAL;
        images[i].base_mip = i;
        images[i].mip_count = 1U;
        images[i].layer_count = NPH_GPU_ALL_LAYERS;
    }
    NphGpuBarrierDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.memory = &mem;
    desc.memory_count = 1U;
    desc.images = images;
    desc.image_count = 2U;
    nph_gpu_cmd_barrier(cmd, &desc);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, 0U), NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, 1U), NPH_GPU_LAYOUT_GENERAL);
    // Empty barrier descriptions are fine.
    memset(&desc, 0, sizeof desc);
    nph_gpu_cmd_barrier(cmd, &desc);
    test_submit_wait(f->dev, cmd);
    nph_gpu_image_destroy(f->dev, img);
}
