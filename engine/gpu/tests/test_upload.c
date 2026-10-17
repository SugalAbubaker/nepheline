// SPDX-License-Identifier: Apache-2.0
// Uploads: the staging ring, dedicated staging, image uploads across
// queues, and frame-based release.
#include "test_gpu_support.h"

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

static void fill_pattern(u32 *words, u32 count, u32 seed)
{
    for (u32 i = 0; i < count; ++i) {
        words[i] = seed ^ (i * 2654435761U);
    }
}

NTEST_F(GpuFixture, buffer_upload_round_trips)
{
    REQUIRE_DEVICE(f);
    enum {
        WORDS = 16384U
    };
    u32 *src = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, WORDS);
    u32 *out = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, WORDS);
    fill_pattern(src, WORDS, 7U);
    NphGpuBuffer buf = test_make_buffer(f->dev, (u64)WORDS * 4U,
                                        (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
                                            (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC |
                                            (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                                        NPH_GPU_MEMORY_DEVICE, "uploaded");
    u64 value = 1U;
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, &value), NPH_RESULT_OK); // nothing pending
    NTEST_ASSERT_EQ_UINT(value, 0U);
    const u64 free_before = nph_gpu_upload_staging_free(f->dev);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, buf, 0U, src, (u64)WORDS * 4U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_LT_UINT(nph_gpu_upload_staging_free(f->dev), free_before);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, &value), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(value, 0U);
    // Synchronous outside a frame: the ring is free again.
    NTEST_ASSERT_EQ_UINT(nph_gpu_upload_staging_free(f->dev), free_before);
    test_read_buffer(f->dev, buf, 0U, (u64)WORDS * 4U, out);
    NTEST_ASSERT_EQ_MEM(out, src, (u64)WORDS * 4U);
    // Partial update at an offset.
    const u32 patch[4] = {1U, 2U, 3U, 4U};
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, buf, 64U, patch, sizeof patch),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    test_read_buffer(f->dev, buf, 0U, (u64)WORDS * 4U, out);
    NTEST_ASSERT_EQ_UINT(out[15], src[15]);
    NTEST_ASSERT_EQ_UINT(out[16], 1U);
    NTEST_ASSERT_EQ_UINT(out[19], 4U);
    NTEST_ASSERT_EQ_UINT(out[20], src[20]);
    nph_gpu_buffer_destroy(f->dev, buf);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, src, WORDS);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, out, WORDS);
}

// Waiting for the device covers uploads that were queued but never
// flushed: they are submitted, not dropped with the recycled pools.
NTEST_F(GpuFixture, wait_idle_submits_pending_uploads)
{
    REQUIRE_DEVICE(f);
    u32 src[256];
    u32 out[256];
    fill_pattern(src, 256U, 11U);
    NphGpuBuffer buf = test_make_buffer(f->dev, sizeof src,
                                        (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
                                            (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                                        NPH_GPU_MEMORY_DEVICE, "pending");
    const u64 free_before = nph_gpu_upload_staging_free(f->dev);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, buf, 0U, src, sizeof src), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_gpu_upload_staging_free(f->dev), free_before);
    u64 value = 1U;
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, &value), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(value, 0U); // nothing was left pending
    test_read_buffer(f->dev, buf, 0U, sizeof src, out);
    NTEST_EXPECT_EQ_MEM(out, src, sizeof src);
    // The same inside a frame.
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    fill_pattern(src, 256U, 12U);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, buf, 0U, src, sizeof src), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, buf, 0U, sizeof src, out);
    NTEST_EXPECT_EQ_MEM(out, src, sizeof src);
    nph_gpu_buffer_destroy(f->dev, buf);
}

// Uploads queued between frames survive the next frame_begin, which
// recycles the command pool they were recorded into.
NTEST_F(GpuFixture, uploads_between_frames_survive_frame_begin)
{
    REQUIRE_DEVICE(f);
    u32 src[256];
    u32 out[256];
    NphGpuBuffer buf = test_make_buffer(f->dev, sizeof src,
                                        (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
                                            (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                                        NPH_GPU_MEMORY_DEVICE, "between frames");
    for (u32 round = 0; round < 3U; ++round) {
        fill_pattern(src, 256U, 100U + round);
        NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, buf, 0U, src, sizeof src), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
        test_read_buffer(f->dev, buf, 0U, sizeof src, out);
        NTEST_EXPECT_EQ_MEM(out, src, sizeof src);
    }
    nph_gpu_buffer_destroy(f->dev, buf);
}

// Uploads that overlap inside one batch land in the order they were made:
// copies in one command buffer are unordered, so the upload path puts a
// barrier between a copy and the earlier ones it overlaps (a record
// rewritten, or a range freed and handed out again before the flush).
// Synchronisation validation found the renderer's scene tables doing both.
typedef struct UploadSpan {
    u32 first;
    u32 count;
} UploadSpan;

static void overlapping_uploads(NphGpuDevice *dev, bool in_frame)
{
    enum {
        WORDS = 1024U
    };
    NphGpuBuffer buf = test_make_buffer(dev, (u64)WORDS * 4U,
                                        (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
                                            (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                                        NPH_GPU_MEMORY_DEVICE, "overlapping uploads");
    // Two batches. The first appends, overlaps both earlier spans, lands
    // inside the envelope without touching the span before it, and
    // overlaps again; the second rewrites everything and patches it.
    static const UploadSpan batches[2][5] = {
        {{0U, 256U}, {256U, 256U}, {128U, 256U}, {0U, 64U}, {32U, 128U}},
        {{0U, WORDS}, {500U, 100U}, {520U, 10U}, {1000U, 24U}, {0U, 1U}},
    };
    u32 expect[WORDS];
    u32 words[WORDS];
    u32 out[WORDS];
    memset(expect, 0, sizeof expect);
    u32 seed = 1U;
    for (u32 b = 0; b < 2U; ++b) {
        if (in_frame) {
            NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(dev), NPH_RESULT_OK);
        }
        for (u32 u = 0; u < 5U; ++u) {
            const UploadSpan span = batches[b][u];
            fill_pattern(words, span.count, seed++);
            NTEST_ASSERT_EQ_INT(
                nph_gpu_upload_buffer(dev, buf, (u64)span.first * 4U, words, (u64)span.count * 4U),
                NPH_RESULT_OK);
            memcpy(&expect[span.first], words, (size_t)span.count * 4U);
        }
        NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
        if (in_frame) {
            NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(dev), NPH_RESULT_OK);
        }
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(dev), NPH_RESULT_OK);
        test_read_buffer(dev, buf, 0U, sizeof out, out);
        NTEST_EXPECT_EQ_MEM(out, expect, sizeof expect);
    }
    nph_gpu_buffer_destroy(dev, buf);
}

NTEST_F(GpuFixture, overlapping_uploads_in_one_batch_land_in_order)
{
    REQUIRE_DEVICE(f);
    overlapping_uploads(f->dev, false);
    overlapping_uploads(f->dev, true);
}

NTEST(gpu_upload, overlapping_uploads_on_a_single_queue_land_in_order)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.single_queue = 1;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    overlapping_uploads(dev, false);
    overlapping_uploads(dev, true);
    test_gpu_destroy(dev);
}

NTEST_F(GpuFixture, upload_arguments_are_validated)
{
    REQUIRE_DEVICE(f);
    NphGpuBuffer buf = test_make_buffer(f->dev, 64U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                        NPH_GPU_MEMORY_DEVICE, "small");
    NphGpuBuffer no_dst = test_make_buffer(f->dev, 64U, (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                                           NPH_GPU_MEMORY_DEVICE, "no transfer dst");
    u8 bytes[64];
    memset(bytes, 1, sizeof bytes);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, buf, 32U, bytes, 64U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, buf, 0U, bytes, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(f->dev, no_dst, 0U, bytes, 64U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphGpuImage img = test_make_image_2d(f->dev, 4U, 4U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
                                         (u32)NPH_GPU_IMAGE_USAGE_SAMPLED, "no transfer dst");
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, img, 0U, 0U, 1U, bytes, 64U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphGpuImage ok = test_make_image_2d(
        f->dev, 4U, 4U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST, "ok");
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, ok, 0U, 0U, 1U, bytes, 32U),
                        NPH_RESULT_INVALID_ARGUMENT); // wrong size
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, ok, 1U, 0U, 1U, bytes, 64U),
                        NPH_RESULT_INVALID_ARGUMENT); // no such mip
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    nph_gpu_buffer_destroy(f->dev, buf);
    nph_gpu_buffer_destroy(f->dev, no_dst);
    nph_gpu_image_destroy(f->dev, img);
    nph_gpu_image_destroy(f->dev, ok);
}

NTEST(gpu_upload, large_uploads_bypass_the_ring)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.staging_ring_bytes = NPH_KIB(256);
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    enum {
        WORDS = 256U * 1024U
    }; // 1 MiB: four times the ring
    u32 *src = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, WORDS);
    u32 *out = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, WORDS);
    fill_pattern(src, WORDS, 99U);
    NphGpuBuffer buf = test_make_buffer(dev, (u64)WORDS * 4U,
                                        (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST |
                                            (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                                        NPH_GPU_MEMORY_DEVICE, "big");
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(dev, buf, 0U, src, (u64)WORDS * 4U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
    test_read_buffer(dev, buf, 0U, (u64)WORDS * 4U, out);
    NTEST_ASSERT_EQ_MEM(out, src, (u64)WORDS * 4U);
    nph_gpu_buffer_destroy(dev, buf);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, src, WORDS);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, out, WORDS);
    test_gpu_destroy(dev);
}

NTEST(gpu_upload, ring_wraps_across_frames)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.staging_ring_bytes = NPH_KIB(256);
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    enum {
        WORDS = 20U * 1024U
    }; // 80 KiB per frame: the ring wraps every ~3 frames
    const u64 bytes = (u64)WORDS * 4U;
    const u64 quarter = bytes / 4U;
    u32 *src = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, WORDS);
    u32 *out = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, WORDS);
    NphGpuBuffer buf = test_make_buffer(
        dev, bytes, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
        NPH_GPU_MEMORY_DEVICE, "chunk");
    for (u32 frame = 0; frame < 12U; ++frame) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(dev), NPH_RESULT_OK);
        fill_pattern(src, WORDS, frame);
        // Four uploads of a quarter each; the ring holds about three frames.
        for (u32 q = 0; q < 4U; ++q) {
            NTEST_ASSERT_EQ_INT(
                nph_gpu_upload_buffer(dev, buf, q * quarter, src + (u64)q * (WORDS / 4U), quarter),
                NPH_RESULT_OK);
        }
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(dev), NPH_RESULT_OK); // flushes
        NTEST_ASSERT_LE_UINT(nph_gpu_upload_staging_free(dev), NPH_KIB(256));
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_upload_staging_free(dev), NPH_KIB(256));
    test_read_buffer(dev, buf, 0U, bytes, out);
    fill_pattern(src, WORDS, 11U);
    NTEST_ASSERT_EQ_MEM(out, src, bytes);
    nph_gpu_buffer_destroy(dev, buf);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, src, WORDS);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, out, WORDS);
    test_gpu_destroy(dev);
}

NTEST_F(GpuFixture, image_uploads_land_in_shader_read_only)
{
    REQUIRE_DEVICE(f);
    NphGpuImageDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.width = 8U;
    desc.height = 8U;
    desc.mip_count = 2U;
    desc.layer_count = 2U;
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
                 (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    desc.name = "layered";
    NphGpuImage img = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_OK);
    u8 mip0[2U * 8U * 8U * 4U];
    u8 mip1[2U * 4U * 4U * 4U];
    for (u32 i = 0; i < sizeof mip0; ++i) {
        mip0[i] = (u8)(i * 7U);
    }
    for (u32 i = 0; i < sizeof mip1; ++i) {
        mip1[i] = (u8)(200U - i);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, img, 0U, 0U, 2U, mip0, sizeof mip0),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, img, 1U, 0U, 2U, mip1, sizeof mip1),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, 0U), NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, 1U), NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    u8 out0[sizeof mip0];
    u8 out1[sizeof mip1];
    test_read_image(f->dev, img, 0U, out0, sizeof out0);
    test_read_image(f->dev, img, 1U, out1, sizeof out1);
    NTEST_ASSERT_EQ_MEM(out0, mip0, sizeof mip0);
    NTEST_ASSERT_EQ_MEM(out1, mip1, sizeof mip1);
    // Re-uploading one layer leaves the other alone.
    u8 patch[4U * 4U * 4U];
    memset(patch, 0x55, sizeof patch);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, img, 1U, 1U, 1U, patch, sizeof patch),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    test_read_image(f->dev, img, 1U, out1, sizeof out1);
    NTEST_ASSERT_EQ_MEM(out1, mip1, sizeof patch);
    NTEST_ASSERT_EQ_MEM(out1 + sizeof patch, patch, sizeof patch);
    nph_gpu_image_destroy(f->dev, img);
}

// A texture that changes in parts keeps the rest of the mip: rectangles
// anywhere in it, refusals for bad rectangles, block alignment, and
// attachments, on `dev`.
static void region_uploads(NphGpuDevice *dev)
{
    NphGpuImageDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.width = 16U;
    desc.height = 8U;
    desc.mip_count = 1U;
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
                 (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    desc.name = "atlas";
    NphGpuImage img = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(dev, &desc, &img), NPH_RESULT_OK);
    u32 whole[16U * 8U];
    fill_pattern(whole, 16U * 8U, 0xA5A5A5A5U);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(dev, img, 0U, 0U, 1U, whole, sizeof whole),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
    // A 5 x 3 rectangle at (9, 4), then one at the corner.
    u32 patch[5U * 3U];
    fill_pattern(patch, 5U * 3U, 0x12345678U);
    NTEST_ASSERT_EQ_INT(
        nph_gpu_upload_image_region(dev, img, 0U, 0U, 9U, 4U, 5U, 3U, patch, sizeof patch),
        NPH_RESULT_OK);
    const u32 corner = 0xDEADBEEFU;
    NTEST_ASSERT_EQ_INT(
        nph_gpu_upload_image_region(dev, img, 0U, 0U, 15U, 7U, 1U, 1U, &corner, sizeof corner),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(dev, img, 0U), NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    u32 out[16U * 8U];
    test_read_image(dev, img, 0U, out, sizeof out);
    for (u32 y = 0; y < 8U; ++y) {
        for (u32 x = 0; x < 16U; ++x) {
            u32 want = whole[y * 16U + x];
            if (x >= 9U && x < 14U && y >= 4U && y < 7U) {
                want = patch[(y - 4U) * 5U + (x - 9U)];
            } else if (x == 15U && y == 7U) {
                want = corner;
            }
            NTEST_ASSERT_EQ_UINT(out[y * 16U + x], want);
        }
    }
    // Outside the mip, empty, or the wrong size: refused, nothing queued.
    NTEST_EXPECT_EQ_INT(
        nph_gpu_upload_image_region(dev, img, 0U, 0U, 12U, 0U, 5U, 1U, patch, (u64)5U * 4U),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_gpu_upload_image_region(dev, img, 0U, 0U, 0U, 0U, 0U, 1U, patch, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_gpu_upload_image_region(dev, img, 0U, 0U, 0U, 0U, 2U, 2U, patch, 12U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_gpu_upload_image_region(dev, img, 1U, 0U, 0U, 0U, 1U, 1U, patch, 4U),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_gpu_image_destroy(dev, img);

    // Block-compressed: rectangles on block boundaries only.
    desc.format = NPH_GPU_FORMAT_BC4_UNORM;
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    desc.name = "bc4";
    if (nph_gpu_device_info(dev)->features.texture_compression_bc &&
        nph_gpu_image_create(dev, &desc, &img) == NPH_RESULT_OK) {
        u8 blocks[2U * 8U];
        memset(blocks, 0x3C, sizeof blocks);
        NTEST_EXPECT_EQ_INT(
            nph_gpu_upload_image_region(dev, img, 0U, 0U, 4U, 4U, 8U, 4U, blocks, sizeof blocks),
            NPH_RESULT_OK);
        NTEST_EXPECT_EQ_INT(
            nph_gpu_upload_image_region(dev, img, 0U, 0U, 2U, 4U, 8U, 4U, blocks, sizeof blocks),
            NPH_RESULT_INVALID_ARGUMENT);
        NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
        nph_gpu_image_destroy(dev, img);
    }

    // An attachment is exclusive. Where the transfer queue has a family of
    // its own, an upload cannot take it from its owner and keep the rest of
    // the mip: refused. On one family (lavapipe) nothing changes owner,
    // every image is exclusive, and the sampled image above went through.
    const bool transfer_family = nph_gpu_vk_queue_family(dev, NPH_GPU_QUEUE_TRANSFER) !=
                                 nph_gpu_vk_queue_family(dev, NPH_GPU_QUEUE_GRAPHICS);
    NTEST_LOG("the transfer queue has %s family", transfer_family ? "its own" : "the graphics");
    desc.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    desc.name = "attachment";
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(dev, &desc, &img), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(
        nph_gpu_upload_image_region(dev, img, 0U, 0U, 0U, 0U, 1U, 1U, &corner, sizeof corner),
        transfer_family ? NPH_RESULT_INVALID_ARGUMENT : NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
    nph_gpu_image_destroy(dev, img);
}

NTEST_F(GpuFixture, region_uploads_keep_the_rest_of_the_mip)
{
    REQUIRE_DEVICE(f);
    region_uploads(f->dev);
}

// The same on one queue family, as on lavapipe: every image is exclusive
// there and nothing changes owner, so a region upload keeps the rest of the
// mip for any image. The Linux CI found the editor's font atlas refused;
// desc.single_queue reproduces the layout on any device.
NTEST(gpu_upload, region_uploads_on_a_single_queue)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.single_queue = 1;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    region_uploads(dev);
    test_gpu_destroy(dev);
}

// Three frames, each uploading a buffer and then reading it back on the
// graphics queue, and the graphics work that follows sees the upload
// without any wait of its own.
static void frame_uploads_feed_graphics_work(NphGpuDevice *dev)
{
    NphGpuBuffer buf = test_make_buffer(
        dev, 1024U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
        NPH_GPU_MEMORY_DEVICE, "frame upload");
    NphGpuBuffer rb = test_make_buffer(dev, 1024U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                       NPH_GPU_MEMORY_READBACK, "frame readback");
    u32 words[256];
    for (u32 frame = 0; frame < 3U; ++frame) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(dev), NPH_RESULT_OK);
        fill_pattern(words, 256U, frame + 100U);
        NTEST_ASSERT_EQ_INT(nph_gpu_upload_buffer(dev, buf, 0U, words, sizeof words),
                            NPH_RESULT_OK);
        u64 upload_value = 0U;
        NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, &upload_value), NPH_RESULT_OK);
        NTEST_ASSERT_GT_UINT(upload_value, 0U);
        NphGpuCmd *cmd = NULL;
        NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "consume", &cmd),
                            NPH_RESULT_OK);
        NphGpuBufferCopy region = {0U, 0U, sizeof words};
        nph_gpu_cmd_copy_buffer(cmd, buf, rb, &region, 1U);
        nph_gpu_cmd_end(cmd);
        NphGpuSubmitDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.cmds = &cmd;
        sd.cmd_count = 1U;
        NTEST_ASSERT_EQ_INT(nph_gpu_submit(dev, &sd, NULL), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(dev), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(dev), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_MEM(nph_gpu_buffer_map(dev, rb), words, sizeof words);
    }
    nph_gpu_buffer_destroy(dev, buf);
    nph_gpu_buffer_destroy(dev, rb);
}

NTEST_F(GpuFixture, uploads_inside_frames_feed_graphics_work)
{
    REQUIRE_DEVICE(f);
    frame_uploads_feed_graphics_work(f->dev);
}

// The same on one queue: no transfer timeline to wait on, so the upload
// command buffer must carry the barriers itself. Synchronisation
// validation on lavapipe found this missing (a read-after-write hazard on
// the uploaded buffer); desc.single_queue reproduces that layout on any
// device.
NTEST(gpu_upload, uploads_feed_graphics_work_on_a_single_queue)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.single_queue = 1;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_ASSERT_FALSE(nph_gpu_device_info(dev)->features.async_compute);
    NTEST_ASSERT_FALSE(nph_gpu_device_info(dev)->features.dedicated_transfer);
    frame_uploads_feed_graphics_work(dev);
    test_gpu_destroy(dev);
}
