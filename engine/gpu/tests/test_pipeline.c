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

typedef enum MeshDraw {
    MESH_DRAW_DIRECT = 0,
    MESH_DRAW_INDIRECT,
    MESH_DRAW_INDIRECT_COUNT,
} MeshDraw;

// Draws with the test mesh shader (one quad per draw record, in column
// gl_DrawID of an 8-column split) into a 64 x 64 RGBA8 image and returns
// how many columns were coloured, checking each column is either fully
// coloured or untouched.
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
    NphGpuShader mesh = test_load_shader(f->dev, "test_mesh.mesh");
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
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
