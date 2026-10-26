// SPDX-License-Identifier: Apache-2.0
// Device creation, selection, information, timelines, frames, and the
// validation and leak gates.
#include <nepheline/platform/window.h>

#include "test_gpu_support.h"

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

NTEST(gpu_device, rejects_a_null_allocator)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    NphGpuDevice *dev = (NphGpuDevice *)&desc;
    NTEST_ASSERT_EQ_INT(nph_gpu_device_create(&desc, &dev), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(dev);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_create(NULL, &dev), NPH_RESULT_INVALID_ARGUMENT);
    nph_gpu_device_destroy(NULL); // allowed
}

NTEST_F(GpuFixture, reports_a_conforming_device)
{
    REQUIRE_DEVICE(f);
    const NphGpuDeviceInfo *info = nph_gpu_device_info(f->dev);
    NTEST_LOG("device: %s (%s) driver %s %s", info->name, nph_gpu_device_type_name(info->type),
              info->driver_name, info->driver_info);
    NTEST_ASSERT_NE_STR(info->name, "");
    NTEST_ASSERT_TRUE(info->api_version_major > 1U ||
                      (info->api_version_major == 1U && info->api_version_minor >= 3U));
    NTEST_ASSERT_GE_UINT(info->limits.max_push_constant_bytes, NPH_GPU_PUSH_CONSTANT_BYTES);
    NTEST_ASSERT_TRUE(info->limits.timestamp_period_ns > 0.0F);
    NTEST_ASSERT_GE_UINT(info->limits.bindless_sampled_images, 256U);
    NTEST_ASSERT_GE_UINT(info->limits.bindless_storage_images, 64U);
    NTEST_ASSERT_GE_UINT(info->limits.bindless_samplers, 16U);
    NTEST_ASSERT_GE_UINT(info->limits.max_color_attachments, 4U);
    NTEST_ASSERT_GE_UINT(info->limits.max_image_dimension_2d, 4096U);
    NTEST_ASSERT_GE_UINT(info->physical_device_count, 1U);
    NTEST_ASSERT_LT_UINT(info->physical_device_index, info->physical_device_count);
    NTEST_ASSERT_NE_STR(nph_gpu_device_type_name(info->type), "?");
    // An out-of-range value exercises the "?" fallback on purpose.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    NTEST_ASSERT_EQ_STR(nph_gpu_device_type_name((NphGpuDeviceType)99), "?");
    NTEST_ASSERT_NOT_NULL(nph_gpu_device_allocator(f->dev));
    NTEST_LOG("features: async compute %d, dedicated transfer %d, mesh %d, ray query %d",
              info->features.async_compute, info->features.dedicated_transfer,
              info->features.mesh_shader, info->features.ray_query);
}

// The format query reports what the core of Vulkan promises for every
// format (Vulkan's "Required Format Support"), and the formats the renderer
// asks about on the reference GPU and lavapipe.
NTEST_F(GpuFixture, formats_report_what_they_can_do)
{
    REQUIRE_DEVICE(f);
    NphGpuDevice *dev = f->dev;
    const u32 rgba8 =
        (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED | (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED_LINEAR |
        (u32)NPH_GPU_FORMAT_FEATURE_STORAGE | (u32)NPH_GPU_FORMAT_FEATURE_COLOR_ATTACHMENT |
        (u32)NPH_GPU_FORMAT_FEATURE_BLEND;
    NTEST_EXPECT_TRUE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, rgba8));
    NTEST_EXPECT_TRUE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_R16G16B16A16_SFLOAT, rgba8));
    NTEST_EXPECT_TRUE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_D32_SFLOAT,
                                              (u32)NPH_GPU_FORMAT_FEATURE_DEPTH_ATTACHMENT));
    NTEST_EXPECT_TRUE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_R32_UINT,
                                              (u32)NPH_GPU_FORMAT_FEATURE_STORAGE_ATOMIC));
    NTEST_EXPECT_FALSE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM,
                                               (u32)NPH_GPU_FORMAT_FEATURE_DEPTH_ATTACHMENT));
    NTEST_EXPECT_FALSE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_UNDEFINED, 0U));
    NTEST_EXPECT_FALSE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_COUNT, 0U));
    // B10G11R11: what the production path's HDR target needs, by device.
    NTEST_LOG("%s: B10G11R11 storage %d, blend %d", nph_gpu_device_info(dev)->name,
              nph_gpu_format_supports(dev, NPH_GPU_FORMAT_B10G11R11_UFLOAT,
                                      (u32)NPH_GPU_FORMAT_FEATURE_STORAGE),
              nph_gpu_format_supports(dev, NPH_GPU_FORMAT_B10G11R11_UFLOAT,
                                      (u32)NPH_GPU_FORMAT_FEATURE_BLEND));
}

// disable_features switches optional capabilities off on any device, so the
// renderer's fallback paths run on hardware that has the fast ones.
NTEST(gpu_device, optional_features_can_be_disabled)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.headless = 1;
    NphGpuDevice *full = test_gpu_create(&desc);
    if (full == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    const NphGpuFeatures have = nph_gpu_device_info(full)->features;
    const NphGpuLimits have_limits = nph_gpu_device_info(full)->limits;
    NTEST_LOG("mesh %d, ray query %d, image atomic64 %d, buffer atomic64 %d, float16 %d, "
              "minmax %d, first instance %d, subgroup vote %d ballot %d arithmetic %d, "
              "geometry shader %d",
              have.mesh_shader, have.ray_query, have.image_atomic_int64, have.buffer_atomic_int64,
              have.shader_float16, have.sampler_filter_minmax, have.draw_indirect_first_instance,
              have.subgroup_vote, have.subgroup_ballot, have.subgroup_arithmetic,
              have.geometry_shader);
    if (have.mesh_shader) {
        NTEST_EXPECT_GE_UINT(have_limits.max_mesh_output_vertices, 256U);
        NTEST_EXPECT_GE_UINT(have_limits.max_mesh_workgroup_invocations, 128U);
        NTEST_EXPECT_GE_UINT(have_limits.max_mesh_workgroup_total_count, 1U << 22U);
    } else {
        NTEST_EXPECT_EQ_UINT(have_limits.max_mesh_output_vertices, 0U);
    }
    test_gpu_destroy(full);

    desc.disable_features =
        (u32)NPH_GPU_OPTIONAL_MESH_SHADER | (u32)NPH_GPU_OPTIONAL_RAY_QUERY |
        (u32)NPH_GPU_OPTIONAL_IMAGE_ATOMIC_INT64 | (u32)NPH_GPU_OPTIONAL_BUFFER_ATOMIC_INT64 |
        (u32)NPH_GPU_OPTIONAL_SHADER_FLOAT16 | (u32)NPH_GPU_OPTIONAL_SAMPLER_FILTER_MINMAX;
    NphGpuDevice *bare = test_gpu_create(&desc);
    NTEST_ASSERT_NOT_NULL(bare);
    const NphGpuDeviceInfo *info = nph_gpu_device_info(bare);
    NTEST_EXPECT_FALSE(info->features.mesh_shader);
    NTEST_EXPECT_FALSE(info->features.ray_query);
    NTEST_EXPECT_FALSE(info->features.image_atomic_int64);
    NTEST_EXPECT_FALSE(info->features.buffer_atomic_int64);
    NTEST_EXPECT_FALSE(info->features.shader_float16);
    NTEST_EXPECT_FALSE(info->features.sampler_filter_minmax);
    NTEST_EXPECT_EQ_UINT(info->limits.max_mesh_output_vertices, 0U);
    // What is not optional stays as it was.
    NTEST_EXPECT_EQ_INT(info->features.subgroup_ballot, have.subgroup_ballot);
    NTEST_EXPECT_EQ_INT(info->features.texture_compression_bc, have.texture_compression_bc);
    NTEST_EXPECT_EQ_INT(info->features.geometry_shader, have.geometry_shader);
    // A mesh-shader draw is refused (asserted in development builds), not
    // called through a missing entry point.
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(bare, NPH_GPU_QUEUE_GRAPHICS, "no mesh", &cmd),
                        NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(nph_gpu_cmd_draw_mesh_tasks(cmd, 1U, 1U, 1U));
    test_submit_wait(bare, cmd);
    test_gpu_destroy(bare);
}

NTEST_F(GpuFixture, statistics_start_at_zero)
{
    REQUIRE_DEVICE(f);
    const NphGpuValidationStats v = nph_gpu_device_validation_stats(f->dev);
    NTEST_ASSERT_EQ_UINT(v.errors, 0U);
    const NphGpuLiveCounts live = nph_gpu_device_live_counts(f->dev);
    NTEST_ASSERT_EQ_UINT(live.buffers, 0U);
    NTEST_ASSERT_EQ_UINT(live.images, 0U);
    NTEST_ASSERT_EQ_UINT(live.pending_destroys, 0U);
    const NphGpuMemoryStats mem = nph_gpu_device_memory_stats(f->dev);
    NTEST_ASSERT_GT_UINT(mem.device_local_budget + mem.host_visible_budget, 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
}

NTEST(gpu_device, selects_by_index_and_rejects_bad_indices)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.headless = 1;
    desc.device_index_plus_one = 1000U;
    NphGpuDevice *dev = NULL;
    NphResult r = nph_gpu_device_create(&desc, &dev);
    if (r == NPH_RESULT_UNSUPPORTED && dev == NULL) {
        // Either no Vulkan at all or the index was rejected: try index 0.
        desc.device_index_plus_one = 1U;
        r = nph_gpu_device_create(&desc, &dev);
        if (r == NPH_RESULT_UNSUPPORTED) {
            NTEST_SKIP("no Vulkan device available");
        }
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_info(dev)->physical_device_index, 0U);
    test_gpu_destroy(dev);
}

NTEST(gpu_device, validation_can_be_forced_off)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.headless = 1;
    desc.validation = NPH_GPU_TOGGLE_OFF;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_ASSERT_FALSE(nph_gpu_device_info(dev)->validation);
    NTEST_ASSERT_FALSE(nph_gpu_device_info(dev)->sync_validation);
    test_gpu_destroy(dev);
}

NTEST(gpu_device, windowed_creation_needs_the_window_system)
{
    if (nph_window_system_is_initialized()) {
        NTEST_SKIP("window system already initialised by another test");
    }
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.headless = 0;
    NphGpuDevice *dev = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_device_create(&desc, &dev), NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_NULL(dev);
}

NTEST(gpu_device, reports_leaks_at_destruction)
{
    NphGpuDevice *dev = test_gpu_create(NULL);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    (void)test_make_buffer(dev, 256U, (u32)NPH_GPU_BUFFER_USAGE_STORAGE, NPH_GPU_MEMORY_DEVICE,
                           "leaked on purpose");
    (void)test_make_image_2d(dev, 4U, 4U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
                             (u32)NPH_GPU_IMAGE_USAGE_SAMPLED, "leaked image");
    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    NphGpuSampler s = NPH_GPU_SAMPLER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(dev, &sd, &s), NPH_RESULT_OK);
    const NphGpuLiveCounts live = nph_gpu_device_live_counts(dev);
    NTEST_ASSERT_EQ_UINT(live.buffers, 1U);
    NTEST_ASSERT_EQ_UINT(live.images, 1U);
    NTEST_ASSERT_EQ_UINT(live.samplers, 1U);
    // The leaks are logged and released by the device; validation must
    // still be clean afterwards (nothing is left for Vulkan to complain
    // about).
    nph_gpu_device_destroy(dev);
}

// ---------------------------------------------------------------------------
// Timelines
// ---------------------------------------------------------------------------

NTEST_F(GpuFixture, timelines_signal_and_wait)
{
    REQUIRE_DEVICE(f);
    NphGpuTimeline *tl = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_create(f->dev, 5U, "test timeline", &tl), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_timeline_value(f->dev, tl), 5U);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_wait(f->dev, tl, 5U, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_wait(f->dev, tl, 6U, 1000U), NPH_RESULT_BUSY);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_signal(f->dev, tl, 7U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_timeline_value(f->dev, tl), 7U);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_wait(f->dev, tl, 6U, NPH_U64_MAX), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).timelines, 1U);
    nph_gpu_timeline_destroy(f->dev, tl);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).timelines, 0U);
    nph_gpu_timeline_destroy(f->dev, NULL);
}

NTEST(gpu_device, timeline_pool_is_bounded)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.max_timelines = 4U; // raised to queues + 1 by the device
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphGpuTimeline *tls[8];
    u32 made = 0U;
    for (u32 i = 0; i < 8U; ++i) {
        if (nph_gpu_timeline_create(dev, 0U, NULL, &tls[i]) != NPH_RESULT_OK) {
            break;
        }
        ++made;
    }
    NTEST_ASSERT_GE_UINT(made, 1U);
    NTEST_ASSERT_LT_UINT(made, 8U);
    NphGpuTimeline *extra = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_create(dev, 0U, NULL, &extra), NPH_RESULT_FULL);
    for (u32 i = 0; i < made; ++i) {
        nph_gpu_timeline_destroy(dev, tls[i]);
    }
    test_gpu_destroy(dev);
}

NTEST_F(GpuFixture, queue_timelines_advance_per_submit)
{
    REQUIRE_DEVICE(f);
    NTEST_ASSERT_EQ_UINT(nph_gpu_queue_submitted_value(f->dev, NPH_GPU_QUEUE_GRAPHICS), 0U);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "empty", &cmd),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_queue(cmd), NPH_GPU_QUEUE_GRAPHICS);
    NTEST_ASSERT_EQ_PTR(nph_gpu_cmd_device(cmd), f->dev);
    nph_gpu_cmd_end(cmd);
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.cmds = &cmd;
    sd.cmd_count = 1U;
    u64 value = 0U;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, &value), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(value, 1U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_queue_submitted_value(f->dev, NPH_GPU_QUEUE_GRAPHICS), 1U);
    NphGpuTimeline *tl = nph_gpu_queue_timeline(f->dev, NPH_GPU_QUEUE_GRAPHICS);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_wait(f->dev, tl, 1U, NPH_U64_MAX), NPH_RESULT_OK);
    // The queue's timeline is not the caller's to destroy: asserted in
    // development, refused in every configuration.
    NPH_TEST_EXPECT_ASSERT(nph_gpu_timeline_destroy(f->dev, tl));
    NTEST_ASSERT_EQ_PTR(nph_gpu_queue_timeline(f->dev, NPH_GPU_QUEUE_GRAPHICS), tl);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_wait(f->dev, tl, 1U, NPH_U64_MAX), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    // Submitting a command buffer twice, or for the wrong queue, is refused.
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_COMPUTE, "compute", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_end(cmd);
    sd.queue = NPH_GPU_QUEUE_GRAPHICS;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, &value), NPH_RESULT_INVALID_ARGUMENT);
    sd.queue = NPH_GPU_QUEUE_COMPUTE;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, &value), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
}

NTEST_F(GpuFixture, submit_waits_on_user_timelines)
{
    REQUIRE_DEVICE(f);
    NphGpuTimeline *tl = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_create(f->dev, 0U, "gate", &tl), NPH_RESULT_OK);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, NULL, &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_end(cmd);
    NphGpuTimelineOp wait = {tl, 1U, (u32)NPH_GPU_STAGE_ALL};
    NphGpuTimelineOp signal = {tl, 2U, (u32)NPH_GPU_STAGE_ALL};
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.cmds = &cmd;
    sd.cmd_count = 1U;
    sd.waits = &wait;
    sd.wait_count = 1U;
    sd.signals = &signal;
    sd.signal_count = 1U;
    NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, NULL), NPH_RESULT_OK);
    // Blocked on the host signal: value 2 is not reached yet.
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_wait(f->dev, tl, 2U, 2000000U), NPH_RESULT_BUSY);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_signal(f->dev, tl, 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_timeline_wait(f->dev, tl, 2U, NPH_U64_MAX), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_gpu_timeline_destroy(f->dev, tl);
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

NTEST_F(GpuFixture, frames_alternate_slots_and_recycle)
{
    REQUIRE_DEVICE(f);
    NTEST_ASSERT_FALSE(nph_gpu_frame_is_active(f->dev));
    for (u32 i = 0; i < 6U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        NTEST_ASSERT_TRUE(nph_gpu_frame_is_active(f->dev));
        NTEST_ASSERT_EQ_UINT(nph_gpu_frame_index(f->dev), i);
        NTEST_ASSERT_EQ_UINT(nph_gpu_frame_slot(f->dev), i % NPH_GPU_FRAMES_IN_FLIGHT);
        NphGpuCmd *cmd = NULL;
        NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "frame", &cmd),
                            NPH_RESULT_OK);
        const u32 t0 = nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
        const u32 t1 = nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
        NTEST_ASSERT_EQ_UINT(t0, 0U);
        NTEST_ASSERT_EQ_UINT(t1, 1U);
        nph_gpu_cmd_end(cmd);
        NphGpuSubmitDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.cmds = &cmd;
        sd.cmd_count = 1U;
        NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, NULL), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
        NTEST_ASSERT_FALSE(nph_gpu_frame_is_active(f->dev));
        // The timestamps of the frame that last used this slot are readable
        // from the following frame in the slot.
        u64 frame = 0U;
        const u64 *ticks = NULL;
        u32 count = 0U;
        const bool have = nph_gpu_frame_timestamps(f->dev, &frame, &ticks, &count);
        if (i >= NPH_GPU_FRAMES_IN_FLIGHT) {
            NTEST_ASSERT_TRUE(have);
            NTEST_ASSERT_EQ_UINT(frame, i - NPH_GPU_FRAMES_IN_FLIGHT);
            NTEST_ASSERT_EQ_UINT(count, 2U);
            NTEST_ASSERT_LE_UINT(ticks[0], ticks[1]);
        } else {
            NTEST_ASSERT_FALSE(have);
        }
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
}

// A wait for the device between frames recycles every slot, which reads
// the slot's results; the next frame in the slot recycles it again: the
// results of the frame that last used it stand (they were lost before:
// read twice, the second time from a pool reset since).
NTEST_F(GpuFixture, timestamps_survive_a_wait_between_frames)
{
    REQUIRE_DEVICE(f);
    for (u32 i = 0; i < 2U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        if (i >= NPH_GPU_FRAMES_IN_FLIGHT) {
            u64 frame = 0U;
            const u64 *ticks = NULL;
            u32 count = 0U;
            // Checked without returning: the frame must still end.
            NTEST_EXPECT_TRUE(nph_gpu_frame_timestamps(f->dev, &frame, &ticks, &count));
            NTEST_EXPECT_EQ_UINT(frame, i - NPH_GPU_FRAMES_IN_FLIGHT);
            NTEST_EXPECT_EQ_UINT(count, 2U);
        }
        NphGpuCmd *cmd = NULL;
        NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "frame", &cmd),
                            NPH_RESULT_OK);
        (void)nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
        (void)nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
        nph_gpu_cmd_end(cmd);
        NphGpuSubmitDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.cmds = &cmd;
        sd.cmd_count = 1U;
        NTEST_ASSERT_EQ_INT(nph_gpu_submit(f->dev, &sd, NULL), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    }
}

NTEST_F(GpuFixture, frame_misuse_is_asserted)
{
    REQUIRE_DEVICE(f);
    // Asserted in development; refused with INVALID_STATE in every configuration.
    NphResult r = NPH_RESULT_OK;
    NPH_TEST_EXPECT_ASSERT(r = nph_gpu_frame_end(f->dev));
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_STATE);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(r = nph_gpu_frame_begin(f->dev));
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_INVALID_STATE);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
}

NTEST_F(GpuFixture, deferred_destruction_waits_for_the_frame)
{
    REQUIRE_DEVICE(f);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    NphGpuBuffer b = test_make_buffer(f->dev, 1024U, (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                                      NPH_GPU_MEMORY_DEVICE, "short-lived");
    nph_gpu_buffer_destroy(f->dev, b);
    NTEST_ASSERT_FALSE(nph_gpu_buffer_is_valid(f->dev, b));
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).buffers, 0U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).pending_destroys, 1U);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK); // other slot: still pending
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).pending_destroys, 1U);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK); // same slot: released
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).pending_destroys, 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
}
