// SPDX-License-Identifier: Apache-2.0
// Pipelines end to end: compute through buffer addresses, graphics through
// dynamic rendering, bindless sampling and storage images, hot reload, and
// the pipeline cache file.
#include <nepheline/platform/file.h>

#include "test_gpu_support.h"

#include <nph_shared.h>

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

static NphGpuPipeline make_compute(NphGpuDevice *dev, NphGpuShader shader)
{
    NphGpuComputePipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.shader = shader;
    desc.name = "test compute";
    NphGpuPipeline p = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(dev, &desc, &p), NPH_RESULT_OK);
    return p;
}

// Runs a fill-style compute pipeline over COUNT invocations and reads the
// buffer back.
#define SPEC_COUNT 256U

static void run_fill(NphGpuDevice *dev, NphGpuPipeline pipeline, u32 value, u32 *out)
{
    NphGpuBuffer buf =
        test_make_buffer(dev, (u64)SPEC_COUNT * 4U,
                         (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                         NPH_GPU_MEMORY_DEVICE, "specialization target");
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.buffer_address = nph_gpu_buffer_address(dev, buf);
    push.count = SPEC_COUNT;
    push.value = value;
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_COMPUTE, "specialize", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, SPEC_COUNT / 64U, 1U, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, buf, (u32)NPH_GPU_STAGE_COMPUTE,
                               (u32)NPH_GPU_ACCESS_SHADER_WRITE, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_READ);
    test_submit_wait(dev, cmd);
    test_read_buffer(dev, buf, 0U, (u64)SPEC_COUNT * 4U, out);
    nph_gpu_buffer_destroy(dev, buf);
}

// One module, three pipelines: the defaults, every constant set, and only
// the first (the others keep their defaults); a reload keeps the values,
// and more constants than the limit are refused.
NTEST_F(GpuFixture, specialization_constants_pick_the_variant)
{
    REQUIRE_DEVICE(f);
    NphGpuShader shader = test_load_shader(f->dev, "test_specialize.comp");
    NphGpuComputePipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.shader = shader;
    desc.name = "specialized";
    NphGpuPipeline defaults = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &desc, &defaults), NPH_RESULT_OK);
    const f32 four = 4.0F;
    desc.specialization[0] = 5U;
    desc.specialization[1] = 1U;
    memcpy(&desc.specialization[2], &four, sizeof four);
    desc.specialization_count = 3U;
    NphGpuPipeline all = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &desc, &all), NPH_RESULT_OK);
    desc.specialization[0] = 0U;
    desc.specialization_count = 1U;
    NphGpuPipeline first = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &desc, &first), NPH_RESULT_OK);
    desc.specialization_count = NPH_GPU_MAX_SPECIALIZATION + 1U;
    NphGpuPipeline refused = NPH_GPU_PIPELINE_NULL;
    NTEST_EXPECT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &desc, &refused),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_FALSE(nph_gpu_pipeline_is_valid(f->dev, refused));

    static u32 out[SPEC_COUNT];
    run_fill(f->dev, defaults, 3U, out);
    for (u32 i = 0; i < SPEC_COUNT; ++i) {
        NTEST_ASSERT_EQ_UINT(out[i], 3U * i + 7U + 1U);
    }
    run_fill(f->dev, all, 3U, out);
    for (u32 i = 0; i < SPEC_COUNT; ++i) {
        NTEST_ASSERT_EQ_UINT(out[i], 6U * i + 5U + 4U);
    }
    run_fill(f->dev, first, 3U, out);
    for (u32 i = 0; i < SPEC_COUNT; ++i) {
        NTEST_ASSERT_EQ_UINT(out[i], 3U * i + 0U + 1U);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_reload(f->dev, all), NPH_RESULT_OK);
    run_fill(f->dev, all, 3U, out);
    for (u32 i = 0; i < SPEC_COUNT; ++i) {
        NTEST_ASSERT_EQ_UINT(out[i], 6U * i + 5U + 4U);
    }
    nph_gpu_pipeline_destroy(f->dev, first);
    nph_gpu_pipeline_destroy(f->dev, all);
    nph_gpu_pipeline_destroy(f->dev, defaults);
    nph_gpu_shader_destroy(f->dev, shader);
}

// With shader statistics asked for, a compiled pipeline reports what the
// driver knows of it (registers, instructions); without, nothing.
NTEST(gpu_pipeline, statistics_describe_the_compiled_shader)
{
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.headless = 1;
    dd.shader_statistics = 1;
    NphGpuDevice *dev = test_gpu_create(&dd);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphGpuShader shader = test_load_shader(dev, "test_fill.comp");
    NphGpuPipeline p = make_compute(dev, shader);
    NphGpuShaderStatistic stats[32];
    const u32 n = nph_gpu_pipeline_statistics(dev, p, stats, 32U);
    if (!nph_gpu_device_info(dev)->features.shader_statistics) {
        NTEST_EXPECT_EQ_UINT(n, 0U);
    } else {
        NTEST_EXPECT_GT_UINT(n, 0U);
        for (u32 i = 0; i < n; ++i) {
            NTEST_EXPECT_NE_UINT((u32)strlen(stats[i].name), 0U);
            NTEST_LOG("%s: %s = %llu", stats[i].executable, stats[i].name,
                      (unsigned long long)stats[i].value);
        }
        // A short buffer takes the first ones.
        NphGpuShaderStatistic one;
        NTEST_EXPECT_EQ_UINT(nph_gpu_pipeline_statistics(dev, p, &one, 1U), 1U);
        NTEST_EXPECT_EQ_STR(one.name, stats[0].name);
    }
    NTEST_EXPECT_EQ_UINT(nph_gpu_pipeline_statistics(dev, NPH_GPU_PIPELINE_NULL, stats, 32U), 0U);
    nph_gpu_pipeline_destroy(dev, p);
    nph_gpu_shader_destroy(dev, shader);
    test_gpu_destroy(dev);
}

// Renders `vertex_count` vertices with `pipeline` into a fresh RGBA8 image
// of the given size, cleared to black, and reads the pixels back.
static void render_to_pixels(NphGpuDevice *dev, NphGpuPipeline pipeline, const NphTestPush *push,
                             u32 size, u32 vertex_count, u8 *pixels)
{
    NphGpuImage target = test_make_image_2d(dev, size, size, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
                                            (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT |
                                                (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC,
                                            "render target");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "render", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, target, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_COLOR_OUTPUT,
                              (u32)NPH_GPU_ACCESS_COLOR_WRITE, NPH_GPU_LAYOUT_COLOR_ATTACHMENT);
    NphGpuRenderingDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.colors[0].image = target;
    rd.colors[0].load = NPH_GPU_LOAD_OP_CLEAR;
    rd.colors[0].store = NPH_GPU_STORE_OP_STORE;
    rd.colors[0].clear = nph_gpu_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
    rd.color_count = 1U;
    nph_gpu_cmd_begin_rendering(cmd, &rd);
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, push, sizeof *push);
    nph_gpu_cmd_draw(cmd, vertex_count, 1U, 0U, 0U);
    nph_gpu_cmd_end_rendering(cmd);
    nph_gpu_cmd_image_barrier(cmd, target, (u32)NPH_GPU_STAGE_COLOR_OUTPUT,
                              (u32)NPH_GPU_ACCESS_COLOR_WRITE, NPH_GPU_LAYOUT_COLOR_ATTACHMENT,
                              (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_READ,
                              NPH_GPU_LAYOUT_TRANSFER_SRC);
    test_submit_wait(dev, cmd);
    test_read_image(dev, target, 0U, pixels, (u64)size * size * 4U);
    nph_gpu_image_destroy(dev, target);
}

NTEST_F(GpuFixture, compute_pipeline_writes_through_a_buffer_address)
{
    REQUIRE_DEVICE(f);
    NphGpuShader shader = test_load_shader(f->dev, "test_fill.comp");
    NphGpuPipeline pipeline = make_compute(f->dev, shader);
    NTEST_ASSERT_TRUE(nph_gpu_pipeline_is_valid(f->dev, pipeline));
    NTEST_ASSERT_TRUE(nph_gpu_pipeline_is_compute(f->dev, pipeline));
    NTEST_ASSERT_EQ_UINT(nph_gpu_pipeline_push_constant_bytes(f->dev, pipeline),
                         (u32)sizeof(NphTestPush));
    NTEST_ASSERT_EQ_UINT(nph_gpu_pipeline_generation(f->dev, pipeline), 1U);
    NTEST_ASSERT_NE_PTR(nph_gpu_vk_pipeline(f->dev, pipeline), VK_NULL_HANDLE);
    enum {
        COUNT = 1024U
    };
    NphGpuBuffer buf =
        test_make_buffer(f->dev, (u64)COUNT * 4U,
                         (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                         NPH_GPU_MEMORY_DEVICE, "fill target");
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.buffer_address = nph_gpu_buffer_address(f->dev, buf);
    push.count = COUNT;
    push.value = 3U;
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_COMPUTE, "fill", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, COUNT / 64U, 1U, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, buf, (u32)NPH_GPU_STAGE_COMPUTE,
                               (u32)NPH_GPU_ACCESS_SHADER_WRITE, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_READ);
    test_submit_wait(f->dev, cmd);
    u32 out[COUNT];
    test_read_buffer(f->dev, buf, 0U, sizeof out, out);
    for (u32 i = 0; i < COUNT; ++i) {
        NTEST_ASSERT_EQ_UINT(out[i], 3U * i);
    }
    nph_gpu_buffer_destroy(f->dev, buf);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, shader);
}

NTEST_F(GpuFixture, graphics_pipeline_draws_a_triangle)
{
    REQUIRE_DEVICE(f);
    NphGpuShader vert = test_load_shader(f->dev, "test_triangle.vert");
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
    NphGpuPipeline pipeline =
        test_make_graphics_pipeline(f->dev, vert, frag, NPH_GPU_FORMAT_R8G8B8A8_UNORM);
    NTEST_ASSERT_FALSE(nph_gpu_pipeline_is_compute(f->dev, pipeline));
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.color = nph_vec4(1.0F, 0.0F, 0.0F, 1.0F);
    u8 pixels[64U * 64U * 4U];
    render_to_pixels(f->dev, pipeline, &push, 64U, 3U, pixels);
    NTEST_ASSERT_TRUE(
        test_pixel_near(&pixels[((u64)32U * 64U + 32U) * 4U], 255U, 0U, 0U, 255U, 0U));
    NTEST_ASSERT_TRUE(test_pixel_near(&pixels[0], 0U, 0U, 0U, 255U, 0U));
    NTEST_ASSERT_TRUE(test_pixel_near(&pixels[((u64)63U * 64U + 63U) * 4U], 0U, 0U, 0U, 255U, 0U));
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, vert);
    nph_gpu_shader_destroy(f->dev, frag);
}

// One vertex module, two pipelines: the specialization constant turns on a
// clip distance that cuts away the right half of a viewport-covering
// triangle, so the constant reaches the vertex stage and the device clips.
NTEST_F(GpuFixture, graphics_specialization_and_clip_distances)
{
    REQUIRE_DEVICE(f);
    if (!nph_gpu_device_info(f->dev)->features.clip_distance) {
        NTEST_SKIP("no clip distances on this device");
    }
    NphGpuShader vert = test_load_shader(f->dev, "test_clip.vert");
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
    NphGpuGraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.vertex = vert;
    desc.fragment = frag;
    desc.color_formats[0] = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.color_count = 1U;
    desc.name = "test clip off";
    NphGpuPipeline whole = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &desc, &whole), NPH_RESULT_OK);
    desc.specialization[0] = 1U;
    desc.specialization_count = 1U;
    desc.name = "test clip on";
    NphGpuPipeline half = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &desc, &half), NPH_RESULT_OK);
    desc.specialization_count = NPH_GPU_MAX_SPECIALIZATION + 1U;
    NphGpuPipeline refused = NPH_GPU_PIPELINE_NULL;
    NTEST_EXPECT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &desc, &refused),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.color = nph_vec4(0.0F, 0.0F, 1.0F, 1.0F);
    u8 pixels[64U * 64U * 4U];
    render_to_pixels(f->dev, whole, &push, 64U, 3U, pixels);
    NTEST_EXPECT_TRUE(test_pixel_near(&pixels[((u64)32U * 64U + 8U) * 4U], 0U, 0U, 255U, 255U, 0U));
    NTEST_EXPECT_TRUE(
        test_pixel_near(&pixels[((u64)32U * 64U + 56U) * 4U], 0U, 0U, 255U, 255U, 0U));
    render_to_pixels(f->dev, half, &push, 64U, 3U, pixels);
    NTEST_EXPECT_TRUE(test_pixel_near(&pixels[((u64)32U * 64U + 8U) * 4U], 0U, 0U, 255U, 255U, 0U));
    NTEST_EXPECT_TRUE(test_pixel_near(&pixels[((u64)32U * 64U + 56U) * 4U], 0U, 0U, 0U, 255U, 0U));
    nph_gpu_pipeline_destroy(f->dev, whole);
    nph_gpu_pipeline_destroy(f->dev, half);
    nph_gpu_shader_destroy(f->dev, vert);
    nph_gpu_shader_destroy(f->dev, frag);
}

typedef enum MeshDraw {
    MESH_DRAW_DIRECT = 0,
    MESH_DRAW_INDIRECT,
    MESH_DRAW_INDIRECT_COUNT,
} MeshDraw;

// Draws with a test mesh shader (one quad per draw record, in column
// gl_DrawID of an 8-column split) into a 64 x 64 RGBA8 image, with green as
// the push-constant colour, and returns how many columns were coloured,
// checking each column is either fully coloured or untouched.
static u32 mesh_draw_columns(NphGpuDevice *dev, NphGpuPipeline pipeline, MeshDraw mode,
                             NphGpuBuffer args, NphGpuBuffer count)
{
    NphGpuImage target = test_make_image_2d(dev, 64U, 64U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
                                            (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT |
                                                (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC,
                                            "mesh target");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "mesh draw", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, target, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_COLOR_OUTPUT,
                              (u32)NPH_GPU_ACCESS_COLOR_WRITE, NPH_GPU_LAYOUT_COLOR_ATTACHMENT);
    NphGpuRenderingDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.colors[0].image = target;
    rd.colors[0].load = NPH_GPU_LOAD_OP_CLEAR;
    rd.colors[0].store = NPH_GPU_STORE_OP_STORE;
    rd.colors[0].clear = nph_gpu_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
    rd.color_count = 1U;
    nph_gpu_cmd_begin_rendering(cmd, &rd);
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.color = nph_vec4(0.0F, 1.0F, 0.0F, 1.0F);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    switch (mode) {
    case MESH_DRAW_DIRECT:
        nph_gpu_cmd_draw_mesh_tasks(cmd, 1U, 1U, 1U);
        break;
    case MESH_DRAW_INDIRECT:
        nph_gpu_cmd_draw_mesh_tasks_indirect(cmd, args, 0U, 3U, 16U);
        break;
    case MESH_DRAW_INDIRECT_COUNT:
        nph_gpu_cmd_draw_mesh_tasks_indirect_count(cmd, args, 0U, count, 0U, 4U, 16U);
        break;
    }
    nph_gpu_cmd_end_rendering(cmd);
    nph_gpu_cmd_image_barrier(cmd, target, (u32)NPH_GPU_STAGE_COLOR_OUTPUT,
                              (u32)NPH_GPU_ACCESS_COLOR_WRITE, NPH_GPU_LAYOUT_COLOR_ATTACHMENT,
                              (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_READ,
                              NPH_GPU_LAYOUT_TRANSFER_SRC);
    test_submit_wait(dev, cmd);
    static u8 pixels[64U * 64U * 4U];
    test_read_image(dev, target, 0U, pixels, sizeof pixels);
    nph_gpu_image_destroy(dev, target);
    u32 columns = 0U;
    u32 mismatches = 0U;
    for (u32 c = 0; c < 8U; ++c) {
        const bool lit = test_pixel_near(&pixels[((u64)32U * 64U + (u64)c * 8U + 4U) * 4U], 0U,
                                         255U, 0U, 255U, 0U);
        // Every pixel of a column agrees with its centre.
        for (u32 y = 0; y < 64U; ++y) {
            for (u32 x = c * 8U; x < c * 8U + 8U; ++x) {
                const u8 *p = &pixels[((u64)y * 64U + x) * 4U];
                const bool ok = lit ? test_pixel_near(p, 0U, 255U, 0U, 255U, 0U)
                                    : test_pixel_near(p, 0U, 0U, 0U, 255U, 0U);
                if (!ok && mismatches++ == 0U) {
                    NTEST_LOG("mode %d: pixel (%u, %u) is (%u, %u, %u, %u) in a column that is %s",
                              (int)mode, x, y, p[0], p[1], p[2], p[3], lit ? "lit" : "dark");
                }
            }
        }
        // The draws fill columns from the left with no gaps.
        if (lit) {
            NTEST_EXPECT_EQ_UINT(columns, c);
            ++columns;
        }
    }
    NTEST_EXPECT_EQ_UINT(mismatches, 0U);
    return columns;
}

NTEST_F(GpuFixture, mesh_shaders_draw_directly_indirectly_and_with_a_count)
{
    REQUIRE_DEVICE(f);
    const NphGpuDeviceInfo *info = nph_gpu_device_info(f->dev);
    if (!info->features.mesh_shader) {
        NTEST_SKIP("no mesh shaders on %s", info->name);
    }
    NTEST_ASSERT_GE_UINT(info->limits.max_mesh_output_vertices, 256U); // the API's minimum
    NTEST_ASSERT_GE_UINT(info->limits.max_mesh_output_primitives, 256U);
    NTEST_ASSERT_GE_UINT(info->limits.max_mesh_workgroup_count[0], 65535U);
    // The colour comes per primitive from the mesh stage, so the draws are
    // checked on drivers with NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS
    // too (the next test covers the fragment stage's push constants).
    NphGpuShader mesh = test_load_shader(f->dev, "test_mesh_color.mesh");
    NphGpuShader frag = test_load_shader(f->dev, "test_prim_color.frag");
    NphGpuGraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.mesh = mesh;
    desc.fragment = frag;
    desc.color_formats[0] = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.color_count = 1U;
    desc.name = "test mesh";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &desc, &pipeline), NPH_RESULT_OK);

    // Four records of one workgroup each, 16 bytes apart; the count buffer
    // says two.
    NphGpuBuffer args = test_make_buffer(f->dev, 64U, (u32)NPH_GPU_BUFFER_USAGE_INDIRECT,
                                         NPH_GPU_MEMORY_UPLOAD, "mesh args");
    NphGpuBuffer count = test_make_buffer(f->dev, 4U, (u32)NPH_GPU_BUFFER_USAGE_INDIRECT,
                                          NPH_GPU_MEMORY_UPLOAD, "mesh count");
    u32 *a = (u32 *)nph_gpu_buffer_map(f->dev, args);
    for (u32 i = 0; i < 4U; ++i) {
        a[i * 4U + 0U] = 1U;
        a[i * 4U + 1U] = 1U;
        a[i * 4U + 2U] = 1U;
        a[i * 4U + 3U] = 0xDEADBEEFU; // padding the stride skips
    }
    *(u32 *)nph_gpu_buffer_map(f->dev, count) = 2U;

    NTEST_EXPECT_EQ_UINT(mesh_draw_columns(f->dev, pipeline, MESH_DRAW_DIRECT, NPH_GPU_BUFFER_NULL,
                                           NPH_GPU_BUFFER_NULL),
                         1U);
    NTEST_EXPECT_EQ_UINT(
        mesh_draw_columns(f->dev, pipeline, MESH_DRAW_INDIRECT, args, NPH_GPU_BUFFER_NULL), 3U);
    NTEST_EXPECT_EQ_UINT(mesh_draw_columns(f->dev, pipeline, MESH_DRAW_INDIRECT_COUNT, args, count),
                         2U);

    nph_gpu_buffer_destroy(f->dev, count);
    nph_gpu_buffer_destroy(f->dev, args);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, mesh);
    nph_gpu_shader_destroy(f->dev, frag);
}

// The columns (bit c of eight) lit in a 64 x 64 target after one command
// buffer of draws with test_column.vert and test_green.frag, which `record`
// records between the start and the end of rendering.
typedef void (*ColumnDraws)(NphGpuCmd *cmd, const void *user);

static u32 draw_columns(NphGpuDevice *dev, NphGpuPipeline pipeline, ColumnDraws record,
                        const void *user)
{
    NphGpuImage target = test_make_image_2d(dev, 64U, 64U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
                                            (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT |
                                                (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC,
                                            "column target");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "column draws", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, target, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_COLOR_OUTPUT,
                              (u32)NPH_GPU_ACCESS_COLOR_WRITE, NPH_GPU_LAYOUT_COLOR_ATTACHMENT);
    NphGpuRenderingDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.colors[0].image = target;
    rd.colors[0].load = NPH_GPU_LOAD_OP_CLEAR;
    rd.colors[0].store = NPH_GPU_STORE_OP_STORE;
    rd.colors[0].clear = nph_gpu_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
    rd.color_count = 1U;
    nph_gpu_cmd_begin_rendering(cmd, &rd);
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    record(cmd, user);
    nph_gpu_cmd_end_rendering(cmd);
    nph_gpu_cmd_image_barrier(cmd, target, (u32)NPH_GPU_STAGE_COLOR_OUTPUT,
                              (u32)NPH_GPU_ACCESS_COLOR_WRITE, NPH_GPU_LAYOUT_COLOR_ATTACHMENT,
                              (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_READ,
                              NPH_GPU_LAYOUT_TRANSFER_SRC);
    test_submit_wait(dev, cmd);
    static u8 pixels[64U * 64U * 4U];
    test_read_image(dev, target, 0U, pixels, sizeof pixels);
    nph_gpu_image_destroy(dev, target);
    u32 lit = 0U;
    for (u32 c = 0; c < 8U; ++c) {
        if (test_pixel_near(&pixels[((u64)32U * 64U + (u64)c * 8U + 4U) * 4U], 0U, 255U, 0U, 255U,
                            0U)) {
            lit |= 1U << c;
        }
    }
    return lit;
}

typedef struct ColumnBuffers {
    NphGpuBuffer records; // VkDrawIndirectCommand per column, 16 bytes apart
    NphGpuBuffer indexed; // VkDrawIndexedIndirectCommand per column, 20 bytes apart
    NphGpuBuffer indices; // u16 0 to 5
    NphGpuBuffer counts;  // u32: 0, 2
} ColumnBuffers;

// An indirect-count draw that draws nothing (its count is 0), then plain
// indirect draws of columns 1 and 2; the same with indices, then column 5.
static void stale_count_draws(NphGpuCmd *cmd, const void *user)
{
    const ColumnBuffers *b = (const ColumnBuffers *)user;
    nph_gpu_cmd_draw_indirect_count(cmd, b->records, 0U, b->counts, 0U, 8U, 16U);
    nph_gpu_cmd_draw_indirect(cmd, b->records, 16U, 2U, 16U);
    nph_gpu_cmd_bind_index_buffer(cmd, b->indices, 0U, NPH_GPU_INDEX_U16);
    nph_gpu_cmd_draw_indexed_indirect_count(cmd, b->indexed, 0U, b->counts, 0U, 8U, 20U);
    nph_gpu_cmd_draw_indexed_indirect(cmd, b->indexed, (u64)5U * 20U, 1U, 20U);
}

// Indirect-count draws whose counts are 2: columns 0 and 1, then 4 and 5.
static void count_draws(NphGpuCmd *cmd, const void *user)
{
    const ColumnBuffers *b = (const ColumnBuffers *)user;
    nph_gpu_cmd_draw_indirect_count(cmd, b->records, 0U, b->counts, 4U, 8U, 16U);
    nph_gpu_cmd_bind_index_buffer(cmd, b->indices, 0U, NPH_GPU_INDEX_U16);
    nph_gpu_cmd_draw_indexed_indirect_count(cmd, b->indexed, (u64)4U * 20U, b->counts, 4U, 4U, 20U);
}

// Indirect draws with and without a count buffer, in one command buffer:
// each draws exactly its own records, wherever it comes. A plain indirect
// draw after a count draw is the case NPH_GPU_QUIRK_INDIRECT_DRAW_STALE_COUNT
// gets wrong on lavapipe without the RHI's count table (it drew nothing).
NTEST_F(GpuFixture, an_indirect_draw_after_a_count_draw_draws)
{
    REQUIRE_DEVICE(f);
    NphGpuShader vert = test_load_shader(f->dev, "test_column.vert");
    NphGpuShader frag = test_load_shader(f->dev, "test_green.frag");
    NphGpuGraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.vertex = vert;
    desc.fragment = frag;
    desc.color_formats[0] = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.color_count = 1U;
    desc.name = "test columns";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &desc, &pipeline), NPH_RESULT_OK);
    ColumnBuffers b;
    b.records = test_make_buffer(f->dev, (u64)8U * 16U, (u32)NPH_GPU_BUFFER_USAGE_INDIRECT,
                                 NPH_GPU_MEMORY_UPLOAD, "column records");
    b.indexed = test_make_buffer(f->dev, (u64)8U * 20U, (u32)NPH_GPU_BUFFER_USAGE_INDIRECT,
                                 NPH_GPU_MEMORY_UPLOAD, "column indexed records");
    b.indices = test_make_buffer(f->dev, (u64)6U * 2U, (u32)NPH_GPU_BUFFER_USAGE_INDEX,
                                 NPH_GPU_MEMORY_UPLOAD, "column indices");
    b.counts = test_make_buffer(f->dev, 8U, (u32)NPH_GPU_BUFFER_USAGE_INDIRECT,
                                NPH_GPU_MEMORY_UPLOAD, "column counts");
    u32 *rec = (u32 *)nph_gpu_buffer_map(f->dev, b.records);
    u32 *ind = (u32 *)nph_gpu_buffer_map(f->dev, b.indexed);
    for (u32 c = 0; c < 8U; ++c) {
        // vertexCount, instanceCount, firstVertex, firstInstance (the column).
        rec[c * 4U + 0U] = 6U;
        rec[c * 4U + 1U] = 1U;
        rec[c * 4U + 2U] = 0U;
        rec[c * 4U + 3U] = c;
        // indexCount, instanceCount, firstIndex, vertexOffset, firstInstance.
        ind[c * 5U + 0U] = 6U;
        ind[c * 5U + 1U] = 1U;
        ind[c * 5U + 2U] = 0U;
        ind[c * 5U + 3U] = 0U;
        ind[c * 5U + 4U] = c;
    }
    u16 *idx = (u16 *)nph_gpu_buffer_map(f->dev, b.indices);
    for (u16 i = 0; i < 6U; ++i) {
        idx[i] = i;
    }
    u32 *counts = (u32 *)nph_gpu_buffer_map(f->dev, b.counts);
    counts[0] = 0U;
    counts[1] = 2U;
    const NphGpuDeviceInfo *info = nph_gpu_device_info(f->dev);
    NTEST_LOG("%s: quirks 0x%x", info->name, info->quirks);
    const u32 after_count = draw_columns(f->dev, pipeline, stale_count_draws, &b);
    const u32 counted = draw_columns(f->dev, pipeline, count_draws, &b);
    NTEST_LOG("columns: 0x%02x after a count draw, 0x%02x counted", after_count, counted);
    NTEST_EXPECT_EQ_UINT(after_count, (1U << 1U) | (1U << 2U) | (1U << 5U));
    NTEST_EXPECT_EQ_UINT(counted, (1U << 0U) | (1U << 1U) | (1U << 4U) | (1U << 5U));
    nph_gpu_buffer_destroy(f->dev, b.counts);
    nph_gpu_buffer_destroy(f->dev, b.indices);
    nph_gpu_buffer_destroy(f->dev, b.indexed);
    nph_gpu_buffer_destroy(f->dev, b.records);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, vert);
    nph_gpu_shader_destroy(f->dev, frag);
}

// The fragment shader of a mesh pipeline reads the push constants the
// command buffer pushed. Lavapipe's read zeros (the drawn quad came out
// (0, 0, 0, 0) on CI); that driver carries the quirk and skips.
NTEST_F(GpuFixture, mesh_pipeline_fragment_shaders_read_push_constants)
{
    REQUIRE_DEVICE(f);
    const NphGpuDeviceInfo *info = nph_gpu_device_info(f->dev);
    if (!info->features.mesh_shader) {
        NTEST_SKIP("no mesh shaders on %s", info->name);
    }
    if ((info->quirks & (u32)NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS) != 0U) {
        NTEST_SKIP("%s (%s) has NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS", info->name,
                   info->driver_info);
    }
    NphGpuShader mesh = test_load_shader(f->dev, "test_mesh.mesh");
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
    NphGpuGraphicsPipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.mesh = mesh;
    desc.fragment = frag;
    desc.color_formats[0] = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.color_count = 1U;
    desc.name = "test mesh, fragment push constants";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &desc, &pipeline), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(mesh_draw_columns(f->dev, pipeline, MESH_DRAW_DIRECT, NPH_GPU_BUFFER_NULL,
                                           NPH_GPU_BUFFER_NULL),
                         1U);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, mesh);
    nph_gpu_shader_destroy(f->dev, frag);
}

NTEST_F(GpuFixture, fragment_shader_samples_a_bindless_texture)
{
    REQUIRE_DEVICE(f);
    NphGpuImage tex = test_make_image_2d(
        f->dev, 4U, 4U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST, "texture");
    u8 texels[4U * 4U * 4U];
    for (u32 i = 0; i < 16U; ++i) {
        texels[(u64)i * 4U] = 10U;
        texels[(u64)i * 4U + 1U] = 200U;
        texels[(u64)i * 4U + 2U] = 30U;
        texels[(u64)i * 4U + 3U] = 255U;
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, tex, 0U, 0U, 1U, texels, sizeof texels),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    NphGpuSampler sampler = NPH_GPU_SAMPLER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(f->dev, &sd, &sampler), NPH_RESULT_OK);
    NphGpuShader vert = test_load_shader(f->dev, "test_fullscreen.vert");
    NphGpuShader frag = test_load_shader(f->dev, "test_sample.frag");
    NphGpuPipeline pipeline =
        test_make_graphics_pipeline(f->dev, vert, frag, NPH_GPU_FORMAT_R8G8B8A8_UNORM);
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.texture_index = nph_gpu_image_sampled_index(f->dev, tex);
    push.sampler_index = nph_gpu_sampler_index(f->dev, sampler);
    u8 pixels[8U * 8U * 4U];
    render_to_pixels(f->dev, pipeline, &push, 8U, 3U, pixels);
    for (u32 i = 0; i < 64U; ++i) {
        NTEST_ASSERT_TRUE(test_pixel_near(&pixels[(u64)i * 4U], 10U, 200U, 30U, 255U, 1U));
    }
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, vert);
    nph_gpu_shader_destroy(f->dev, frag);
    nph_gpu_sampler_destroy(f->dev, sampler);
    nph_gpu_image_destroy(f->dev, tex);
}

NTEST_F(GpuFixture, compute_shader_writes_a_bindless_storage_image)
{
    REQUIRE_DEVICE(f);
    NphGpuImage img = test_make_image_2d(
        f->dev, 16U, 16U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC, "storage");
    NphGpuShader shader = test_load_shader(f->dev, "test_image.comp");
    NphGpuPipeline pipeline = make_compute(f->dev, shader);
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.image_index = nph_gpu_image_storage_index(f->dev, img, 0U);
    push.value = 77U;
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "pattern", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, img, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_COMPUTE,
                              (u32)NPH_GPU_ACCESS_SHADER_WRITE, NPH_GPU_LAYOUT_GENERAL);
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, 2U, 2U, 1U);
    nph_gpu_cmd_image_barrier(cmd, img, (u32)NPH_GPU_STAGE_COMPUTE,
                              (u32)NPH_GPU_ACCESS_SHADER_WRITE, NPH_GPU_LAYOUT_GENERAL,
                              (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_READ,
                              NPH_GPU_LAYOUT_TRANSFER_SRC);
    test_submit_wait(f->dev, cmd);
    u8 pixels[16U * 16U * 4U];
    test_read_image(f->dev, img, 0U, pixels, sizeof pixels);
    for (u32 y = 0; y < 16U; ++y) {
        for (u32 x = 0; x < 16U; ++x) {
            NTEST_ASSERT_TRUE(
                test_pixel_near(&pixels[((u64)y * 16U + x) * 4U], x, y, 77U, 255U, 0U));
        }
    }
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, shader);
    nph_gpu_image_destroy(f->dev, img);
}

static f32 srgb_to_linear(f32 c)
{
    return c <= 0.04045F ? c / 12.92F : nph_f32_pow((c + 0.055F) / 1.055F, 2.4F);
}

// An image sampled through a view of another format (sampled_format):
// test_image.comp stores (x, y, 77, 255) / 255 through the RGBA8 UNORM
// storage view, and texelFetch through the sRGB sampled view returns those
// bytes decoded; descriptions whose formats cannot share an image are
// refused.
NTEST_F(GpuFixture, an_image_is_sampled_in_another_format)
{
    REQUIRE_DEVICE(f);
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    d.sampled_format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    d.width = 16U;
    d.height = 16U;
    d.mip_count = 1U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    d.name = "mutable";
    NphGpuImage img = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &d, &img), NPH_RESULT_OK);
    NphGpuShader write = test_load_shader(f->dev, "test_image.comp");
    NphGpuShader fetch = test_load_shader(f->dev, "test_texel_fetch.comp");
    NphGpuPipeline write_pipeline = make_compute(f->dev, write);
    NphGpuPipeline fetch_pipeline = make_compute(f->dev, fetch);
    const u64 bytes = (u64)16U * 16U * sizeof(NphVec4);
    NphGpuBuffer out = test_make_buffer(
        f->dev, bytes, (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
        NPH_GPU_MEMORY_DEVICE, "texels");
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.image_index = nph_gpu_image_storage_index(f->dev, img, 0U);
    push.value = 77U;
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "mutable", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, img, (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                              NPH_GPU_LAYOUT_UNDEFINED, (u32)NPH_GPU_STAGE_COMPUTE,
                              (u32)NPH_GPU_ACCESS_SHADER_WRITE, NPH_GPU_LAYOUT_GENERAL);
    nph_gpu_cmd_bind_pipeline(cmd, write_pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, 2U, 2U, 1U);
    nph_gpu_cmd_image_barrier(cmd, img, (u32)NPH_GPU_STAGE_COMPUTE,
                              (u32)NPH_GPU_ACCESS_SHADER_WRITE, NPH_GPU_LAYOUT_GENERAL,
                              (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_READ,
                              NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    memset(&push, 0, sizeof push);
    push.buffer_address = nph_gpu_buffer_address(f->dev, out);
    push.texture_index = nph_gpu_image_sampled_index(f->dev, img);
    push.count = 16U;
    push.value = 16U;
    nph_gpu_cmd_bind_pipeline(cmd, fetch_pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, 2U, 2U, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, out, (u32)NPH_GPU_STAGE_COMPUTE,
                               (u32)NPH_GPU_ACCESS_SHADER_WRITE, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_READ);
    test_submit_wait(f->dev, cmd);
    NphVec4 texels[16U * 16U];
    test_read_buffer(f->dev, out, 0U, bytes, texels);
    for (u32 y = 0; y < 16U; ++y) {
        for (u32 x = 0; x < 16U; ++x) {
            const NphVec4 t = texels[y * 16U + x];
            NTEST_ASSERT_NEAR_F32(t.x, srgb_to_linear((f32)x / 255.0F), 2e-3F);
            NTEST_ASSERT_NEAR_F32(t.y, srgb_to_linear((f32)y / 255.0F), 2e-3F);
            NTEST_ASSERT_NEAR_F32(t.z, srgb_to_linear(77.0F / 255.0F), 2e-3F);
            NTEST_ASSERT_NEAR_F32(t.w, 1.0F, 1e-6F);
        }
    }
    // Texels of another size, a sampled format without sampling, depth.
    NphGpuImage bad = NPH_GPU_IMAGE_NULL;
    d.sampled_format = NPH_GPU_FORMAT_R32G32_UINT;
    NTEST_EXPECT_EQ_INT(nph_gpu_image_create(f->dev, &d, &bad), NPH_RESULT_INVALID_ARGUMENT);
    d.sampled_format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_STORAGE;
    NTEST_EXPECT_EQ_INT(nph_gpu_image_create(f->dev, &d, &bad), NPH_RESULT_INVALID_ARGUMENT);
    d.format = NPH_GPU_FORMAT_D32_SFLOAT;
    d.sampled_format = NPH_GPU_FORMAT_R32_SFLOAT;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT;
    NTEST_EXPECT_EQ_INT(nph_gpu_image_create(f->dev, &d, &bad), NPH_RESULT_INVALID_ARGUMENT);
    nph_gpu_buffer_destroy(f->dev, out);
    nph_gpu_pipeline_destroy(f->dev, fetch_pipeline);
    nph_gpu_pipeline_destroy(f->dev, write_pipeline);
    nph_gpu_shader_destroy(f->dev, fetch);
    nph_gpu_shader_destroy(f->dev, write);
    nph_gpu_image_destroy(f->dev, img);
}

NTEST_F(GpuFixture, pipeline_descriptions_are_validated)
{
    REQUIRE_DEVICE(f);
    NphGpuShader vert = test_load_shader(f->dev, "test_triangle.vert");
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
    NphGpuShader comp = test_load_shader(f->dev, "test_fill.comp");
    NphGpuGraphicsPipelineDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.color_formats[0] = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    gd.color_count = 1U;
    NphGpuPipeline p = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &gd, &p),
                        NPH_RESULT_INVALID_ARGUMENT); // no stage
    gd.vertex = frag;                                 // wrong stage
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &gd, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    gd.vertex = vert;
    gd.fragment = vert;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &gd, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    gd.fragment = frag;
    gd.mesh = vert; // both vertex and mesh
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &gd, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    gd.mesh = NPH_GPU_SHADER_NULL;
    gd.polygon_mode = NPH_GPU_POLYGON_LINE;
    const NphResult lr = nph_gpu_pipeline_create_graphics(f->dev, &gd, &p);
    if (nph_gpu_device_info(f->dev)->features.fill_mode_non_solid) {
        NTEST_ASSERT_EQ_INT(lr, NPH_RESULT_OK);
        nph_gpu_pipeline_destroy(f->dev, p);
    } else {
        NTEST_ASSERT_EQ_INT(lr, NPH_RESULT_UNSUPPORTED);
    }
    // Depth-only pipeline (no fragment shader) with every fixed-function knob.
    memset(&gd, 0, sizeof gd);
    gd.vertex = vert;
    gd.depth_format = NPH_GPU_FORMAT_D32_SFLOAT;
    gd.depth.test = 1;
    gd.depth.write = 1;
    gd.depth.compare = NPH_GPU_COMPARE_GREATER;
    gd.depth.bias_enable = 1;
    gd.depth.bias_constant = 1.0F;
    gd.depth.bias_slope = 1.5F;
    gd.depth.clamp = 1;
    gd.cull_mode = NPH_GPU_CULL_FRONT;
    gd.front_face = NPH_GPU_FRONT_FACE_CW;
    gd.topology = NPH_GPU_TOPOLOGY_TRIANGLE_STRIP;
    gd.name = "depth only";
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &gd, &p), NPH_RESULT_OK);
    nph_gpu_pipeline_destroy(f->dev, p);
    // Blending on two attachments.
    memset(&gd, 0, sizeof gd);
    gd.vertex = vert;
    gd.fragment = frag;
    gd.color_count = 2U;
    gd.color_formats[0] = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    gd.color_formats[1] = NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    gd.blend[0].enable = 1;
    gd.blend[0].src_color = NPH_GPU_BLEND_SRC_ALPHA;
    gd.blend[0].dst_color = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
    gd.blend[0].color_op = NPH_GPU_BLEND_OP_ADD;
    gd.blend[0].src_alpha = NPH_GPU_BLEND_ONE;
    gd.blend[0].dst_alpha = NPH_GPU_BLEND_ZERO;
    gd.blend[0].alpha_op = NPH_GPU_BLEND_OP_MAX;
    gd.blend[1].write_mask = NPH_GPU_COLOR_MASK_R | NPH_GPU_COLOR_MASK_G;
    gd.topology = NPH_GPU_TOPOLOGY_LINE_LIST;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_graphics(f->dev, &gd, &p), NPH_RESULT_OK);
    nph_gpu_pipeline_destroy(f->dev, p);
    // Compute with a non-compute shader.
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = vert;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &cd, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    cd.shader = comp;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &cd, &p), NPH_RESULT_OK);
    nph_gpu_pipeline_destroy(f->dev, p);
    nph_gpu_pipeline_destroy(f->dev, NPH_GPU_PIPELINE_NULL);
    NTEST_ASSERT_FALSE(nph_gpu_pipeline_is_valid(f->dev, p));
    u32 gen = 1U;
    NPH_TEST_EXPECT_ASSERT(gen = nph_gpu_pipeline_generation(f->dev, p));
    NTEST_ASSERT_EQ_UINT(gen, 0U);
    NphResult rr = NPH_RESULT_OK;
    NPH_TEST_EXPECT_ASSERT(rr = nph_gpu_pipeline_reload(f->dev, p));
    NTEST_ASSERT_EQ_INT(rr, NPH_RESULT_INVALID_ARGUMENT);
    nph_gpu_shader_destroy(f->dev, vert);
    nph_gpu_shader_destroy(f->dev, frag);
    nph_gpu_shader_destroy(f->dev, comp);
}

NTEST_F(GpuFixture, pipelines_hot_reload)
{
    REQUIRE_DEVICE(f);
    NphGpuShader vert = test_load_shader(f->dev, "test_fullscreen.vert");
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
    NphGpuPipeline pipeline =
        test_make_graphics_pipeline(f->dev, vert, frag, NPH_GPU_FORMAT_R8G8B8A8_UNORM);
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.color = nph_vec4(0.0F, 0.0F, 1.0F, 1.0F);
    u8 pixels[4U * 4U * 4U];
    render_to_pixels(f->dev, pipeline, &push, 4U, 3U, pixels);
    NTEST_ASSERT_TRUE(test_pixel_near(&pixels[0], 0U, 0U, 255U, 255U, 0U));

    // Swap the fragment shader for the sampling one and rebuild.
    NphGpuImage tex = test_make_image_2d(
        f->dev, 1U, 1U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST, "one texel");
    const u8 texel[4] = {40U, 80U, 120U, 255U};
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_image(f->dev, tex, 0U, 0U, 1U, texel, sizeof texel),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    NphGpuSampler sampler = NPH_GPU_SAMPLER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(f->dev, &sd, &sampler), NPH_RESULT_OK);
    u64 size = 0U;
    void *spirv = test_read_spirv(f->dev, "test_sample.frag", &size);
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_reload(f->dev, frag, spirv, size), NPH_RESULT_OK);
    test_free_spirv(f->dev, spirv, size);
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_reload(f->dev, pipeline), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_pipeline_generation(f->dev, pipeline), 2U);
    push.texture_index = nph_gpu_image_sampled_index(f->dev, tex);
    push.sampler_index = nph_gpu_sampler_index(f->dev, sampler);
    render_to_pixels(f->dev, pipeline, &push, 4U, 3U, pixels);
    NTEST_ASSERT_TRUE(test_pixel_near(&pixels[0], 40U, 80U, 120U, 255U, 1U));

    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, vert);
    nph_gpu_shader_destroy(f->dev, frag);
    nph_gpu_sampler_destroy(f->dev, sampler);
    nph_gpu_image_destroy(f->dev, tex);
}

NTEST(gpu_pipeline, cache_persists_to_disk)
{
    char dir[512];
    u64 dir_len = 0U;
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(dir, sizeof dir, &dir_len), NPH_RESULT_OK);
    char path[600];
    snprintf(path, sizeof path, "%s/nph_gpu_test_pipeline_cache.bin", dir);
    (void)nph_fs_remove_file(path);

    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.pipeline_cache_path = path;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_ASSERT_FALSE(nph_gpu_pipeline_cache_loaded(dev));
    NphGpuShader shader = test_load_shader(dev, "test_fill.comp");
    NphGpuPipeline pipeline = make_compute(dev, shader);
    NTEST_ASSERT_GT_UINT(nph_gpu_pipeline_cache_size(dev), 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_cache_save(dev), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_fs_exists(path));
    nph_gpu_pipeline_destroy(dev, pipeline);
    nph_gpu_shader_destroy(dev, shader);
    test_gpu_destroy(dev); // saves again

    dev = test_gpu_create(&desc);
    NTEST_ASSERT_NOT_NULL(dev);
    NTEST_ASSERT_TRUE(nph_gpu_pipeline_cache_loaded(dev));
    shader = test_load_shader(dev, "test_fill.comp");
    pipeline = make_compute(dev, shader);
    nph_gpu_pipeline_destroy(dev, pipeline);
    nph_gpu_shader_destroy(dev, shader);
    test_gpu_destroy(dev);

    // A corrupt file is ignored, not trusted.
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, "garbage garbage garbage garbage", 31U),
                        NPH_RESULT_OK);
    dev = test_gpu_create(&desc);
    NTEST_ASSERT_NOT_NULL(dev);
    NTEST_ASSERT_FALSE(nph_gpu_pipeline_cache_loaded(dev));
    test_gpu_destroy(dev);

    // Without a path there is nothing to save.
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    dev = test_gpu_create(&desc);
    NTEST_ASSERT_NOT_NULL(dev);
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_cache_save(dev), NPH_RESULT_UNSUPPORTED);
    test_gpu_destroy(dev);
    (void)nph_fs_remove_file(path);
}
