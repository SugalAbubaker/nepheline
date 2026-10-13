// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the gpu tests: a headless device fixture that skips
// cleanly on a machine without Vulkan (CLAUDE.md §8.5 "headless device
// tests on lavapipe") and asserts the validation gate (zero errors) and the
// leak checker (zero live resources) on teardown; shader loading from the
// build's shader directory; readback helpers.

#include <nepheline/core/assert.h>
#include <nepheline/gpu/render_graph.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/gpu/vk.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/thread.h>

#include <ntest/ntest.h>

#include <stdio.h>
#include <string.h>

#ifndef NPH_SHADER_DIR
#error "NPH_SHADER_DIR must point at the compiled shaders (nph_target_use_shaders)"
#endif

// Set by CMake (NPH_GPU_REQUIRE_VALIDATION=ON) on CI runners: the layer
// must be present, since a device that continues without it would pass the
// validation gate without validating anything.
#ifndef NPH_GPU_REQUIRE_VALIDATION
#define NPH_GPU_REQUIRE_VALIDATION 0
#endif

// ---------------------------------------------------------------------------
// Expected assertions (as in the core tests)
// ---------------------------------------------------------------------------

typedef struct TestAssertRecorder {
    u32 count;
} TestAssertRecorder;

static inline NphAssertAction test_assert_record(const NphAssertInfo *info, void *user)
{
    NPH_UNUSED(info);
    ((TestAssertRecorder *)user)->count += 1U;
    return NPH_ASSERT_ACTION_CONTINUE;
}

#if NPH_ASSERTS_ENABLED
#define NPH_TEST_EXPECT_ASSERT(stmt)                                                               \
    do {                                                                                           \
        TestAssertRecorder rec_ = {0U};                                                            \
        NphAssertHandler prev_ = NULL;                                                             \
        void *prev_user_ = NULL;                                                                   \
        nph_assert_set_handler(test_assert_record, &rec_, &prev_, &prev_user_);                    \
        stmt;                                                                                      \
        nph_assert_set_handler(prev_, prev_user_, NULL, NULL);                                     \
        NTEST_ASSERT_GE_UINT(rec_.count, 1U);                                                      \
    } while (0)
#else
#define NPH_TEST_EXPECT_ASSERT(stmt)                                                               \
    do {                                                                                           \
        stmt;                                                                                      \
    } while (0)
#endif

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

// Creates a headless device with the default (configuration-driven)
// validation. NULL when no Vulkan device is available.
static inline NphGpuDevice *test_gpu_create(const NphGpuDeviceDesc *override)
{
    NphGpuDeviceDesc desc;
    if (override != NULL) {
        desc = *override;
    } else {
        memset(&desc, 0, sizeof desc);
    }
    if (desc.alloc == NULL) {
        desc.alloc = nph_allocator_system();
    }
    if (override == NULL) {
        desc.headless = 1;
    }
    NphGpuDevice *dev = NULL;
    const NphResult r = nph_gpu_device_create(&desc, &dev);
    if (r == NPH_RESULT_UNSUPPORTED) {
        return NULL;
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(dev);
#if NPH_GPU_REQUIRE_VALIDATION && NPH_GPU_VALIDATION_ENABLED
    if (override == NULL) {
        NTEST_ASSERT_TRUE(nph_gpu_device_info(dev)->validation);
        NTEST_ASSERT_TRUE(nph_gpu_device_info(dev)->sync_validation);
    }
#endif
    return dev;
}

// The gate: no validation errors, nothing leaked, then destroy.
static inline void test_gpu_destroy(NphGpuDevice *dev)
{
    if (dev == NULL) {
        return;
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(dev), NPH_RESULT_OK);
    const NphGpuValidationStats v = nph_gpu_device_validation_stats(dev);
    NTEST_EXPECT_EQ_UINT(v.errors, 0U);
    const NphGpuLiveCounts live = nph_gpu_device_live_counts(dev);
    NTEST_EXPECT_EQ_UINT(live.buffers, 0U);
    NTEST_EXPECT_EQ_UINT(live.images, 0U);
    NTEST_EXPECT_EQ_UINT(live.samplers, 0U);
    NTEST_EXPECT_EQ_UINT(live.shaders, 0U);
    NTEST_EXPECT_EQ_UINT(live.pipelines, 0U);
    NTEST_EXPECT_EQ_UINT(live.timelines, 0U);
    nph_gpu_device_destroy(dev);
}

typedef struct GpuFixture {
    NphGpuDevice *dev;
} GpuFixture;

#define REQUIRE_DEVICE(f)                                                                          \
    do {                                                                                           \
        if ((f)->dev == NULL) {                                                                    \
            NTEST_SKIP("no Vulkan device available");                                              \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static inline NphGpuShader test_load_shader(NphGpuDevice *dev, const char *file)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.spv", NPH_SHADER_DIR, file);
    NphGpuShader shader = NPH_GPU_SHADER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_load(dev, path, file, &shader), NPH_RESULT_OK);
    return shader;
}

// Reads a compiled shader's bytes (freed with test_free_spirv).
static inline void *test_read_spirv(NphGpuDevice *dev, const char *file, u64 *out_size)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.spv", NPH_SHADER_DIR, file);
    void *data = NULL;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_gpu_device_allocator(dev), &data, out_size),
                        NPH_RESULT_OK);
    return data;
}

static inline void test_free_spirv(NphGpuDevice *dev, void *data, u64 size)
{
    nph_file_free_all(nph_gpu_device_allocator(dev), data, size);
}

// ---------------------------------------------------------------------------
// Submission and readback
// ---------------------------------------------------------------------------

// Ends, submits, and waits for a command buffer.
static inline void test_submit_wait(NphGpuDevice *dev, NphGpuCmd *cmd)
{
    nph_gpu_cmd_end(cmd);
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.queue = nph_gpu_cmd_queue(cmd);
    sd.cmds = &cmd;
    sd.cmd_count = 1U;
    u64 value = 0U;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(dev, &sd, &value), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_gpu_timeline_wait(dev, nph_gpu_queue_timeline(dev, sd.queue), value, NPH_U64_MAX),
        NPH_RESULT_OK);
}

static inline NphGpuBuffer test_make_buffer(NphGpuDevice *dev, u64 size, u32 usage,
                                            NphGpuMemory memory, const char *name)
{
    NphGpuBufferDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.size = size;
    desc.usage = usage;
    desc.memory = memory;
    desc.name = name;
    NphGpuBuffer b = NPH_GPU_BUFFER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_buffer_create(dev, &desc, &b), NPH_RESULT_OK);
    return b;
}

// Copies [offset, offset + size) of `src` into `out` through a readback buffer.
static inline void test_read_buffer(NphGpuDevice *dev, NphGpuBuffer src, u64 offset, u64 size,
                                    void *out)
{
    NphGpuBuffer rb = test_make_buffer(dev, size, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                       NPH_GPU_MEMORY_READBACK, "readback");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "readback", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_buffer_barrier(cmd, src, (u32)NPH_GPU_STAGE_ALL, (u32)NPH_GPU_ACCESS_MEMORY_WRITE,
                               (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_READ);
    NphGpuBufferCopy region;
    region.src_offset = offset;
    region.dst_offset = 0U;
    region.size = size;
    nph_gpu_cmd_copy_buffer(cmd, src, rb, &region, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, rb, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, (u32)NPH_GPU_STAGE_HOST,
                               (u32)NPH_GPU_ACCESS_HOST_READ);
    test_submit_wait(dev, cmd);
    memcpy(out, nph_gpu_buffer_map(dev, rb), size);
    nph_gpu_buffer_destroy(dev, rb);
}

// Copies one mip (all layers) of `image`, currently in its tracked layout,
// into `out` (tightly packed). The image is left in TRANSFER_SRC.
static inline void test_read_image(NphGpuDevice *dev, NphGpuImage image, u32 mip, void *out,
                                   u64 size)
{
    NphGpuBuffer rb = test_make_buffer(dev, size, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                       NPH_GPU_MEMORY_READBACK, "image readback");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "image readback", &cmd),
                        NPH_RESULT_OK);
    const NphGpuLayout current = nph_gpu_image_layout(dev, image, mip);
    if (current != NPH_GPU_LAYOUT_TRANSFER_SRC) {
        NphGpuImageBarrier b;
        memset(&b, 0, sizeof b);
        b.image = image;
        b.src_stages = (u32)NPH_GPU_STAGE_ALL;
        b.src_access = (u32)NPH_GPU_ACCESS_MEMORY_WRITE;
        b.old_layout = current;
        b.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
        b.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
        b.new_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
        b.base_mip = mip;
        b.mip_count = 1U;
        b.layer_count = NPH_GPU_ALL_LAYERS;
        NphGpuBarrierDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.images = &b;
        desc.image_count = 1U;
        nph_gpu_cmd_barrier(cmd, &desc);
    }
    NphGpuBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.mip = mip;
    region.layer_count = nph_gpu_image_desc(dev, image)->layer_count;
    nph_gpu_cmd_copy_image_to_buffer(cmd, image, NPH_GPU_LAYOUT_TRANSFER_SRC, rb, &region, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, rb, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, (u32)NPH_GPU_STAGE_HOST,
                               (u32)NPH_GPU_ACCESS_HOST_READ);
    test_submit_wait(dev, cmd);
    memcpy(out, nph_gpu_buffer_map(dev, rb), size);
    nph_gpu_buffer_destroy(dev, rb);
}

static inline NphGpuImage test_make_image_2d(NphGpuDevice *dev, u32 w, u32 h, NphGpuFormat format,
                                             u32 mips, u32 usage, const char *name)
{
    NphGpuImageDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.type = NPH_GPU_IMAGE_2D;
    desc.format = format;
    desc.width = w;
    desc.height = h;
    desc.mip_count = mips;
    desc.usage = usage;
    desc.name = name;
    NphGpuImage img = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(dev, &desc, &img), NPH_RESULT_OK);
    return img;
}

// A graphics pipeline for one colour attachment of `format`, no depth.
static inline NphGpuPipeline test_make_graphics_pipeline(NphGpuDevice *dev, NphGpuShader vert,
                                                         NphGpuShader frag, NphGpuFormat format)
{
    NphGpuGraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.vertex = vert;
    desc.fragment = frag;
    desc.color_formats[0] = format;
    desc.color_count = 1U;
    desc.name = "test graphics";
    NphGpuPipeline p = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(dev, &desc, &p), NPH_RESULT_OK);
    return p;
}

// RGBA8 pixel comparison with a tolerance per channel.
static inline bool test_pixel_near(const u8 *p, u32 r, u32 g, u32 b, u32 a, u32 tol)
{
    const u32 dr = p[0] > r ? p[0] - r : r - p[0];
    const u32 dg = p[1] > g ? p[1] - g : g - p[1];
    const u32 db = p[2] > b ? p[2] - b : b - p[2];
    const u32 da = p[3] > a ? p[3] - a : a - p[3];
    return dr <= tol && dg <= tol && db <= tol && da <= tol;
}
