// SPDX-License-Identifier: Apache-2.0
// Acceleration structures and ray queries (gpu/accel.h): bottom levels from
// float and 16-bit unorm vertices, a top level of their instances written
// by the CPU, and rays traced against it in a compute shader, with what
// each ray hits checked; and the usages refused without the feature.
#include <nepheline/gpu/accel.h>

#include "test_gpu_support.h"

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

#define REQUIRE_RAY_QUERY(f)                                                                       \
    do {                                                                                           \
        REQUIRE_DEVICE(f);                                                                         \
        if (!nph_gpu_device_info((f)->dev)->features.ray_query) {                                  \
            NTEST_SKIP("no ray queries on this device");                                           \
        }                                                                                          \
    } while (0)

typedef struct RayPush {
    u64 tlas;
    u64 rays;
    u64 hits;
    u32 count;
    u32 pad;
} RayPush;

static NphGpuBuffer upload_buffer(NphGpuDevice *dev, const void *data, u64 size, u32 usage,
                                  const char *name)
{
    NphGpuBuffer b = test_make_buffer(dev, size, usage, NPH_GPU_MEMORY_UPLOAD, name);
    memcpy(nph_gpu_buffer_map(dev, b), data, (size_t)size);
    return b;
}

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1U) / a * a;
}

static void accel_barrier(NphGpuCmd *cmd, u32 dst_stages)
{
    const NphGpuMemoryBarrier m = {(u32)NPH_GPU_STAGE_ACCEL_BUILD, (u32)NPH_GPU_ACCESS_ACCEL_WRITE,
                                   dst_stages, (u32)NPH_GPU_ACCESS_ACCEL_READ};
    NphGpuBarrierDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.memory = &m;
    bd.memory_count = 1U;
    nph_gpu_cmd_barrier(cmd, &bd);
}

static NphGpuAccelInstance instance_of(u64 blas, f32 x, u32 custom_index)
{
    NphGpuAccelInstance in;
    memset(&in, 0, sizeof in);
    in.transform[0][0] = 1.0F;
    in.transform[1][1] = 1.0F;
    in.transform[2][2] = 1.0F;
    in.transform[0][3] = x;
    in.index_and_mask = custom_index | (0xFFU << 24U);
    in.offset_and_flags = NPH_GPU_ACCEL_INSTANCE_NO_FACE_CULL << 24U;
    in.blas = blas;
    return in;
}

// Two bottom levels, one triangle each: float vertices at the origin and,
// where the device builds from them, 16-bit unorm ones (the renderer's
// quantised positions); three instances in a top level; rays straight
// down onto each, from behind, past them, and short of them.
NTEST_F(GpuFixture, ray_queries_hit_instanced_triangles)
{
    REQUIRE_RAY_QUERY(f);
    NphGpuDevice *dev = f->dev;
    const u32 input = (u32)NPH_GPU_BUFFER_USAGE_ACCEL_INPUT | (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    const f32 positions[9] = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    const u16 quantised[12] = {0U, 0U, 0U, 0U, 65535U, 0U, 0U, 0U, 0U, 65535U, 0U, 0U};
    const u32 indices[3] = {0U, 1U, 2U};
    NphGpuBuffer vb = upload_buffer(dev, positions, sizeof positions, input, "triangle vertices");
    NphGpuBuffer qb = upload_buffer(dev, quantised, sizeof quantised, input, "unorm vertices");
    NphGpuBuffer ib = upload_buffer(dev, indices, sizeof indices, input, "triangle indices");

    const bool unorm = nph_gpu_format_supports(dev, NPH_GPU_FORMAT_R16G16B16A16_UNORM,
                                               (u32)NPH_GPU_FORMAT_FEATURE_ACCEL_VERTEX);
    NTEST_LOG("R16G16B16A16_UNORM vertices: %s", unorm ? "yes" : "no");
    NphGpuAccelTriangles tri[2];
    memset(tri, 0, sizeof tri);
    tri[0].vertices = vb;
    tri[0].vertex_stride = 12U;
    tri[0].vertex_count = 3U;
    tri[0].vertex_format = NPH_GPU_FORMAT_R32G32B32_SFLOAT;
    tri[0].indices = ib;
    tri[0].triangle_count = 1U;
    tri[0].opaque = 1;
    tri[1] = tri[0];
    tri[1].vertices = qb;
    tri[1].vertex_stride = 8U;
    tri[1].vertex_format = NPH_GPU_FORMAT_R16G16B16A16_UNORM;
    const u32 blas_count = unorm ? 2U : 1U;

    NphGpuAccel blas[2] = {NPH_GPU_ACCEL_NULL, NPH_GPU_ACCEL_NULL};
    NphGpuAccelSizes sizes[2];
    u64 scratch_size = 0U;
    const u64 align = nph_max_u32(nph_gpu_device_info(dev)->limits.accel_scratch_alignment, 1U);
    for (u32 i = 0; i < blas_count; ++i) {
        NTEST_ASSERT_EQ_INT(
            nph_gpu_accel_blas_sizes(dev, &tri[i], 1U, (u32)NPH_GPU_ACCEL_PREFER_TRACE, &sizes[i]),
            NPH_RESULT_OK);
        NTEST_EXPECT_GT_UINT((u32)sizes[i].accel, 0U);
        NphGpuAccelDesc ad = {NPH_GPU_ACCEL_BOTTOM, sizes[i].accel, "test blas"};
        NTEST_ASSERT_EQ_INT(nph_gpu_accel_create(dev, &ad, &blas[i]), NPH_RESULT_OK);
        NTEST_EXPECT_NE_UINT((u32)(nph_gpu_accel_address(dev, blas[i]) != 0U), 0U);
        scratch_size = align_up(scratch_size, align) + sizes[i].scratch;
    }
    NphGpuAccelSizes tsizes;
    NTEST_ASSERT_EQ_INT(nph_gpu_accel_tlas_sizes(dev, 3U, (u32)NPH_GPU_ACCEL_PREFER_BUILD, &tsizes),
                        NPH_RESULT_OK);
    const u64 tlas_scratch = align_up(scratch_size, align);
    scratch_size = tlas_scratch + tsizes.scratch;
    NphGpuAccelDesc td = {NPH_GPU_ACCEL_TOP, tsizes.accel, "test tlas"};
    NphGpuAccel tlas = NPH_GPU_ACCEL_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_accel_create(dev, &td, &tlas), NPH_RESULT_OK);
    NphGpuBuffer scratch =
        test_make_buffer(dev, scratch_size + align, (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                         NPH_GPU_MEMORY_DEVICE, "test scratch");

    // Instances: the float triangle at x = 0 (index 7) and x = 5 (index 9);
    // the unorm one at x = 10 (index 11), where the device has it.
    NphGpuAccelInstance instances[3];
    instances[0] = instance_of(nph_gpu_accel_address(dev, blas[0]), 0.0F, 7U);
    instances[1] = instance_of(nph_gpu_accel_address(dev, blas[0]), 5.0F, 9U);
    instances[2] = instance_of(unorm ? nph_gpu_accel_address(dev, blas[1]) : 0U, 10.0F, 11U);
    NphGpuBuffer inst = upload_buffer(dev, instances, sizeof instances, input, "test instances");

    // The scratch's address must be aligned, so the first build starts past
    // the buffer's start by what it takes.
    const u64 base = nph_gpu_buffer_address(dev, scratch);
    const u64 lead = align_up(base, align) - base;
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "accel builds", &cmd),
                        NPH_RESULT_OK);
    u64 at = lead;
    for (u32 i = 0; i < blas_count; ++i) {
        at = lead + align_up(at - lead, align);
        nph_gpu_cmd_build_blas(cmd, blas[i], &tri[i], 1U, (u32)NPH_GPU_ACCEL_PREFER_TRACE, scratch,
                               at);
        at += sizes[i].scratch;
    }
    accel_barrier(cmd, (u32)NPH_GPU_STAGE_ACCEL_BUILD);
    nph_gpu_cmd_build_tlas(cmd, tlas, inst, 0U, 3U, (u32)NPH_GPU_ACCEL_PREFER_BUILD, scratch,
                           lead + tlas_scratch);
    accel_barrier(cmd, (u32)NPH_GPU_STAGE_COMPUTE);

    enum {
        RAYS = 7U
    };
    const f32 rays[RAYS][8] = {
        {0.25F, 0.25F, 1.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F},  // onto index 7 at t = 1
        {5.25F, 0.25F, 1.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F},  // onto index 9
        {10.25F, 0.25F, 1.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F}, // onto index 11 (unorm)
        {0.25F, 0.25F, -2.0F, 10.0F, 0.0F, 0.0F, 1.0F, 0.0F},  // from behind, at t = 2
        {2.0F, 2.0F, 1.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F},    // between them: nothing
        {0.25F, 0.25F, 1.0F, 0.5F, 0.0F, 0.0F, -1.0F, 0.0F},   // too short: nothing
        {0.9F, 0.9F, 1.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F},    // outside the hypotenuse
    };
    NphGpuBuffer rb =
        upload_buffer(dev, rays, sizeof rays, (u32)NPH_GPU_BUFFER_USAGE_STORAGE, "test rays");
    NphGpuBuffer hb = test_make_buffer(dev, (u64)RAYS * 16U, (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                                       NPH_GPU_MEMORY_READBACK, "test hits");
    NphGpuShader shader = test_load_shader(dev, "test_ray_query.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    cd.name = "ray query";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(dev, &cd, &pipeline), NPH_RESULT_OK);
    RayPush push;
    memset(&push, 0, sizeof push);
    push.tlas = nph_gpu_accel_address(dev, tlas);
    push.rays = nph_gpu_buffer_address(dev, rb);
    push.hits = nph_gpu_buffer_address(dev, hb);
    push.count = RAYS;
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, 1U, 1U, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, hb, (u32)NPH_GPU_STAGE_COMPUTE,
                               (u32)NPH_GPU_ACCESS_SHADER_WRITE, (u32)NPH_GPU_STAGE_HOST,
                               (u32)NPH_GPU_ACCESS_HOST_READ);
    test_submit_wait(dev, cmd);

    const f32 *hits = (const f32 *)nph_gpu_buffer_map(dev, hb);
    for (u32 i = 0; i < RAYS; ++i) {
        const f32 *h = hits + (u64)i * 4U;
        NTEST_LOG("ray %u: t %g, index %g, triangle %g, front %g", i, (f64)h[0], (f64)h[1],
                  (f64)h[2], (f64)h[3]);
    }
    NTEST_EXPECT_NEAR_F32(hits[0], 1.0F, 1e-5F);
    NTEST_EXPECT_NEAR_F32(hits[1], 7.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(hits[2], 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(hits[4], 1.0F, 1e-5F);
    NTEST_EXPECT_NEAR_F32(hits[5], 9.0F, 0.0F);
    if (unorm) {
        // 65535 / 65535 is exactly 1: the same triangle, decoded.
        NTEST_EXPECT_NEAR_F32(hits[8], 1.0F, 1e-5F);
        NTEST_EXPECT_NEAR_F32(hits[9], 11.0F, 0.0F);
    } else {
        NTEST_EXPECT_NEAR_F32(hits[8], -1.0F, 0.0F); // the instance names no structure
    }
    NTEST_EXPECT_NEAR_F32(hits[12], 2.0F, 1e-5F);
    NTEST_EXPECT_NEAR_F32(hits[13], 7.0F, 0.0F);
    // The two sides face opposite ways.
    NTEST_EXPECT_TRUE(hits[3] != hits[15]);
    NTEST_EXPECT_NEAR_F32(hits[16], -1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(hits[20], -1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(hits[24], -1.0F, 0.0F);

    nph_gpu_pipeline_destroy(dev, pipeline);
    nph_gpu_shader_destroy(dev, shader);
    nph_gpu_buffer_destroy(dev, hb);
    nph_gpu_buffer_destroy(dev, rb);
    nph_gpu_buffer_destroy(dev, inst);
    nph_gpu_buffer_destroy(dev, scratch);
    nph_gpu_accel_destroy(dev, tlas);
    for (u32 i = 0; i < blas_count; ++i) {
        nph_gpu_accel_destroy(dev, blas[i]);
    }
    nph_gpu_buffer_destroy(dev, ib);
    nph_gpu_buffer_destroy(dev, qb);
    nph_gpu_buffer_destroy(dev, vb);
    NTEST_EXPECT_FALSE(nph_gpu_accel_is_valid(dev, tlas));
}

// Bad descriptions are refused: no geometry, a vertex format the device
// cannot build from, a zero size, too many instances.
NTEST_F(GpuFixture, acceleration_structures_refuse_bad_descriptions)
{
    REQUIRE_RAY_QUERY(f);
    NphGpuDevice *dev = f->dev;
    NphGpuAccelSizes sizes;
    NTEST_EXPECT_EQ_INT(nph_gpu_accel_blas_sizes(dev, NULL, 0U, 0U, &sizes),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphGpuAccelTriangles tri;
    memset(&tri, 0, sizeof tri);
    tri.vertex_stride = 4U;
    tri.vertex_count = 3U;
    tri.vertex_format = NPH_GPU_FORMAT_D32_SFLOAT;
    tri.triangle_count = 1U;
    NTEST_EXPECT_EQ_INT(nph_gpu_accel_blas_sizes(dev, &tri, 1U, 0U, &sizes),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphGpuAccelDesc ad = {NPH_GPU_ACCEL_BOTTOM, 0U, "empty"};
    NphGpuAccel a = NPH_GPU_ACCEL_NULL;
    NTEST_EXPECT_EQ_INT(nph_gpu_accel_create(dev, &ad, &a), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_FALSE(nph_gpu_accel_is_valid(dev, a));
    NTEST_EXPECT_EQ_INT(nph_gpu_accel_tlas_sizes(dev, 0U, 0U, &sizes), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_gpu_accel_tlas_sizes(dev, NPH_U32_MAX, 0U, &sizes),
                        nph_gpu_device_info(dev)->limits.accel_max_instances < NPH_U32_MAX
                            ? NPH_RESULT_INVALID_ARGUMENT
                            : NPH_RESULT_OK);
}

// Without ray queries, buffers for acceleration-structure builds are refused
// (their Vulkan usage needs the extension).
NTEST(gpu_accel, build_inputs_need_ray_queries)
{
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.headless = 1;
    dd.disable_features = (u32)NPH_GPU_OPTIONAL_RAY_QUERY;
    NphGpuDevice *dev = test_gpu_create(&dd);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_EXPECT_FALSE(nph_gpu_device_info(dev)->features.ray_query);
    NTEST_EXPECT_EQ_UINT(nph_gpu_device_info(dev)->limits.accel_scratch_alignment, 0U);
    NTEST_EXPECT_FALSE(nph_gpu_format_supports(dev, NPH_GPU_FORMAT_R32G32B32_SFLOAT,
                                               (u32)NPH_GPU_FORMAT_FEATURE_ACCEL_VERTEX));
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = 64U;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_ACCEL_INPUT;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = "refused";
    NphGpuBuffer b = NPH_GPU_BUFFER_NULL;
    NTEST_EXPECT_EQ_INT(nph_gpu_buffer_create(dev, &bd, &b), NPH_RESULT_INVALID_ARGUMENT);
    test_gpu_destroy(dev);
}
