// SPDX-License-Identifier: Apache-2.0
// The render graph: culling, barriers between compute, transfer, and
// graphics passes, transient aliasing, async compute, parallel recording,
// timings, and error reporting.
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

typedef struct FillPass {
    NphGpuPipeline pipeline;
    NphGpuRgBuffer target;
    u32 count;
    u32 value;
    u32 ran;
} FillPass;

static void fill_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    FillPass *p = (FillPass *)user;
    NphGpuDevice *dev = nph_gpu_rg_device(g);
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.buffer_address = nph_gpu_buffer_address(dev, nph_gpu_rg_buffer(g, p->target));
    push.count = p->count;
    push.value = p->value;
    nph_gpu_cmd_bind_pipeline(cmd, p->pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, (p->count + 63U) / 64U, 1U, 1U);
    ++p->ran;
}

typedef struct CopyPass {
    NphGpuRgBuffer src;
    NphGpuRgBuffer dst;
    u64 size;
    u64 dst_offset;
    u32 ran;
} CopyPass;

static void copy_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    CopyPass *p = (CopyPass *)user;
    NphGpuBufferCopy region;
    region.src_offset = 0U;
    region.dst_offset = p->dst_offset;
    region.size = p->size;
    nph_gpu_cmd_copy_buffer(cmd, nph_gpu_rg_buffer(g, p->src), nph_gpu_rg_buffer(g, p->dst),
                            &region, 1U);
    ++p->ran;
}

typedef struct ImageCopyPass {
    NphGpuRgImage src;
    NphGpuRgBuffer dst;
    u32 ran;
} ImageCopyPass;

static void image_copy_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    ImageCopyPass *p = (ImageCopyPass *)user;
    NphGpuBufferImageCopy region;
    memset(&region, 0, sizeof region);
    nph_gpu_cmd_copy_image_to_buffer(cmd, nph_gpu_rg_image(g, p->src), NPH_GPU_LAYOUT_TRANSFER_SRC,
                                     nph_gpu_rg_buffer(g, p->dst), &region, 1U);
    ++p->ran;
}

typedef struct ClearPass {
    NphGpuRgImage image;
    NphGpuClearValue color;
    u32 ran;
} ClearPass;

static void clear_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    ClearPass *p = (ClearPass *)user;
    nph_gpu_cmd_clear_color_image(cmd, nph_gpu_rg_image(g, p->image), NPH_GPU_LAYOUT_TRANSFER_DST,
                                  &p->color, 0U, NPH_GPU_ALL_MIPS, 0U, NPH_GPU_ALL_LAYERS);
    ++p->ran;
}

typedef struct DrawPass {
    NphGpuPipeline pipeline;
    NphVec4 color;
    u32 ran;
} DrawPass;

static void draw_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    DrawPass *p = (DrawPass *)user;
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.color = p->color;
    nph_gpu_cmd_bind_pipeline(cmd, p->pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
    ++p->ran;
}

static NphGpuRenderGraph *make_graph(NphGpuDevice *dev, NphJobSystem *jobs)
{
    NphGpuRenderGraphDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.jobs = jobs;
    desc.name = "test graph";
    NphGpuRenderGraph *g = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(dev, &desc, &g), NPH_RESULT_OK);
    return g;
}

static NphGpuRgPass *add_pass(NphGpuRenderGraph *g, const char *name, NphGpuRgPassType type,
                              u32 flags, NphGpuRgExecuteFn fn, void *user)
{
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = name;
    pd.type = type;
    pd.flags = flags;
    pd.execute = fn;
    pd.user = user;
    NphGpuRgPass *pass = nph_gpu_rg_add_pass(g, &pd);
    NTEST_ASSERT_NOT_NULL(pass);
    return pass;
}

static NphGpuBufferDesc transient_buffer_desc(u64 size, const char *name)
{
    NphGpuBufferDesc d;
    memset(&d, 0, sizeof d);
    d.size = size;
    d.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC |
              (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    d.name = name;
    return d;
}

NTEST_F(GpuFixture, culls_passes_nobody_consumes)
{
    REQUIRE_DEVICE(f);
    NphGpuRenderGraph *g = make_graph(f->dev, NULL);
    NphGpuBuffer out = test_make_buffer(f->dev, 256U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                        NPH_GPU_MEMORY_READBACK, "out");
    NphGpuShader shader = test_load_shader(f->dev, "test_fill.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &cd, &pipeline), NPH_RESULT_OK);

    nph_gpu_rg_begin(g);
    const NphGpuBufferDesc bd = transient_buffer_desc(256U, "unused");
    NphGpuRgBuffer unused = nph_gpu_rg_create_buffer(g, &bd);
    NphGpuRgBuffer used = nph_gpu_rg_create_buffer(g, &bd);
    NphGpuRgBuffer imported = nph_gpu_rg_import_buffer(g, out);
    NTEST_ASSERT_FALSE(nph_gpu_rg_buffer_is_null(unused));
    FillPass dead = {pipeline, unused, 64U, 1U, 0U};
    FillPass live = {pipeline, used, 64U, 5U, 0U};
    CopyPass copy = {used, imported, 256U, 0U, 0U};
    NphGpuRgPass *dead_pass = add_pass(g, "dead", NPH_GPU_RG_PASS_COMPUTE, 0U, fill_execute, &dead);
    nph_gpu_rg_write_buffer(dead_pass, unused, NPH_GPU_RG_USE_STORAGE_WRITE);
    NphGpuRgPass *live_pass = add_pass(g, "live", NPH_GPU_RG_PASS_COMPUTE, 0U, fill_execute, &live);
    nph_gpu_rg_write_buffer(live_pass, used, NPH_GPU_RG_USE_STORAGE_WRITE);
    NphGpuRgPass *copy_pass =
        add_pass(g, "copy", NPH_GPU_RG_PASS_TRANSFER, 0U, copy_execute, &copy);
    nph_gpu_rg_read_buffer(copy_pass, used, NPH_GPU_RG_USE_TRANSFER_SRC);
    nph_gpu_rg_write_buffer(copy_pass, imported, NPH_GPU_RG_USE_TRANSFER_DST);
    // A pass with no uses and no side effects is culled too; one with the
    // flag survives.
    NphGpuRgPass *empty = add_pass(g, "empty", NPH_GPU_RG_PASS_COMPUTE, 0U, NULL, NULL);
    NphGpuRgPass *kept =
        add_pass(g, "kept", NPH_GPU_RG_PASS_COMPUTE, (u32)NPH_GPU_RG_PASS_SIDE_EFFECTS, NULL, NULL);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_gpu_rg_pass_is_culled(g, dead_pass));
    NTEST_ASSERT_TRUE(nph_gpu_rg_pass_is_culled(g, empty));
    NTEST_ASSERT_FALSE(nph_gpu_rg_pass_is_culled(g, live_pass));
    NTEST_ASSERT_FALSE(nph_gpu_rg_pass_is_culled(g, copy_pass));
    NTEST_ASSERT_FALSE(nph_gpu_rg_pass_is_culled(g, kept));
    NphGpuRgStats stats = nph_gpu_rg_stats(g);
    NTEST_ASSERT_EQ_UINT(stats.passes, 5U);
    NTEST_ASSERT_EQ_UINT(stats.culled, 2U);
    NTEST_ASSERT_EQ_UINT(stats.transient_buffers, 1U); // the unused one is never realised
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_OK);
    stats = nph_gpu_rg_stats(g);
    NTEST_ASSERT_EQ_UINT(stats.submits, 1U);
    NTEST_ASSERT_EQ_UINT(stats.command_buffers, 3U);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(dead.ran, 0U);
    NTEST_ASSERT_EQ_UINT(live.ran, 1U);
    NTEST_ASSERT_EQ_UINT(copy.ran, 1U);
    const u32 *words = (const u32 *)nph_gpu_buffer_map(f->dev, out);
    for (u32 i = 0; i < 64U; ++i) {
        NTEST_ASSERT_EQ_UINT(words[i], 5U * i);
    }
    nph_gpu_render_graph_destroy(g);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, shader);
    nph_gpu_buffer_destroy(f->dev, out);
}

NTEST_F(GpuFixture, graphics_pass_renders_into_a_transient_and_copies_out)
{
    REQUIRE_DEVICE(f);
    NphGpuRenderGraph *g = make_graph(f->dev, NULL);
    NphGpuShader vert = test_load_shader(f->dev, "test_triangle.vert");
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
    NphGpuPipeline pipeline =
        test_make_graphics_pipeline(f->dev, vert, frag, NPH_GPU_FORMAT_R8G8B8A8_UNORM);
    NphGpuBuffer out =
        test_make_buffer(f->dev, 32U * 32U * 4U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                         NPH_GPU_MEMORY_READBACK, "pixels");
    for (u32 frame = 0; frame < 3U; ++frame) {
        nph_gpu_rg_begin(g);
        NphGpuImageDesc id;
        memset(&id, 0, sizeof id);
        id.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
        id.width = 32U;
        id.height = 32U;
        id.mip_count = 1U;
        id.usage =
            (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
        id.name = "colour";
        NphGpuRgImage color = nph_gpu_rg_create_image(g, &id);
        NphGpuRgBuffer dst = nph_gpu_rg_import_buffer(g, out);
        DrawPass draw = {pipeline, nph_vec4(1.0F, 0.0F, 0.0F, 1.0F), 0U};
        NphGpuRgPass *draw_pass =
            add_pass(g, "draw", NPH_GPU_RG_PASS_GRAPHICS, 0U, draw_execute, &draw);
        NphGpuRgAttachment att;
        memset(&att, 0, sizeof att);
        att.load = NPH_GPU_LOAD_OP_CLEAR;
        att.store = NPH_GPU_STORE_OP_STORE;
        att.clear = nph_gpu_clear_color(0.0F, 0.0F, 1.0F, 1.0F);
        nph_gpu_rg_color_attachment(draw_pass, color, &att);
        ImageCopyPass copy = {color, dst, 0U};
        NphGpuRgPass *copy_pass =
            add_pass(g, "copy out", NPH_GPU_RG_PASS_TRANSFER, 0U, image_copy_execute, &copy);
        nph_gpu_rg_read_image(copy_pass, color, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, NPH_GPU_ALL_MIPS);
        nph_gpu_rg_write_buffer(copy_pass, dst, NPH_GPU_RG_USE_TRANSFER_DST);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
        const u8 *px = (const u8 *)nph_gpu_buffer_map(f->dev, out);
        NTEST_ASSERT_TRUE(test_pixel_near(&px[(16U * 32U + 16U) * 4U], 255U, 0U, 0U, 255U, 0U));
        NTEST_ASSERT_TRUE(test_pixel_near(&px[0], 0U, 0U, 255U, 255U, 0U));
        // The transient image is cached across frames: no new images.
        NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).images, 1U);
        const NphGpuRgStats stats = nph_gpu_rg_stats(g);
        NTEST_ASSERT_EQ_UINT(stats.transient_images, 1U);
        NTEST_ASSERT_GE_UINT(stats.barriers, 2U);
    }
    nph_gpu_render_graph_destroy(g);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).images, 0U);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, vert);
    nph_gpu_shader_destroy(f->dev, frag);
    nph_gpu_buffer_destroy(f->dev, out);
}

NTEST_F(GpuFixture, transients_with_disjoint_lifetimes_share_memory)
{
    REQUIRE_DEVICE(f);
    NphGpuRenderGraph *g = make_graph(f->dev, NULL);
    const u64 bytes = 256U * 256U * 4U;
    NphGpuBuffer out_a = test_make_buffer(f->dev, bytes, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                          NPH_GPU_MEMORY_READBACK, "out a");
    NphGpuBuffer out_b = test_make_buffer(f->dev, bytes, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                          NPH_GPU_MEMORY_READBACK, "out b");
    nph_gpu_rg_begin(g);
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    id.width = 256U;
    id.height = 256U;
    id.mip_count = 1U;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    id.name = "a";
    NphGpuRgImage a = nph_gpu_rg_create_image(g, &id);
    id.name = "b";
    NphGpuRgImage b = nph_gpu_rg_create_image(g, &id);
    NphGpuRgBuffer ra = nph_gpu_rg_import_buffer(g, out_a);
    NphGpuRgBuffer rb = nph_gpu_rg_import_buffer(g, out_b);
    ClearPass clear_a = {a, nph_gpu_clear_color(1.0F, 0.0F, 0.0F, 1.0F), 0U};
    ClearPass clear_b = {b, nph_gpu_clear_color(0.0F, 1.0F, 0.0F, 1.0F), 0U};
    ImageCopyPass copy_a = {a, ra, 0U};
    ImageCopyPass copy_b = {b, rb, 0U};
    NphGpuRgPass *p1 =
        add_pass(g, "clear a", NPH_GPU_RG_PASS_TRANSFER, 0U, clear_execute, &clear_a);
    nph_gpu_rg_write_image(p1, a, NPH_GPU_RG_USE_TRANSFER_DST, 0U, NPH_GPU_ALL_MIPS);
    NphGpuRgPass *p2 =
        add_pass(g, "copy a", NPH_GPU_RG_PASS_TRANSFER, 0U, image_copy_execute, &copy_a);
    nph_gpu_rg_read_image(p2, a, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_write_buffer(p2, ra, NPH_GPU_RG_USE_TRANSFER_DST);
    NphGpuRgPass *p3 =
        add_pass(g, "clear b", NPH_GPU_RG_PASS_TRANSFER, 0U, clear_execute, &clear_b);
    nph_gpu_rg_write_image(p3, b, NPH_GPU_RG_USE_TRANSFER_DST, 0U, NPH_GPU_ALL_MIPS);
    NphGpuRgPass *p4 =
        add_pass(g, "copy b", NPH_GPU_RG_PASS_TRANSFER, 0U, image_copy_execute, &copy_b);
    nph_gpu_rg_read_image(p4, b, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_write_buffer(p4, rb, NPH_GPU_RG_USE_TRANSFER_DST);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_OK);
    const NphGpuRgStats stats = nph_gpu_rg_stats(g);
    NTEST_ASSERT_EQ_UINT(stats.transient_images, 2U);
    NTEST_ASSERT_GE_UINT(stats.transient_bytes_requested, 2U * bytes);
    NTEST_ASSERT_LT_UINT(stats.transient_bytes_allocated, stats.transient_bytes_requested);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const u8 *pa = (const u8 *)nph_gpu_buffer_map(f->dev, out_a);
    const u8 *pb = (const u8 *)nph_gpu_buffer_map(f->dev, out_b);
    NTEST_ASSERT_TRUE(test_pixel_near(&pa[0], 255U, 0U, 0U, 255U, 0U));
    NTEST_ASSERT_TRUE(test_pixel_near(&pa[bytes - 4U], 255U, 0U, 0U, 255U, 0U));
    NTEST_ASSERT_TRUE(test_pixel_near(&pb[0], 0U, 255U, 0U, 255U, 0U));
    NTEST_ASSERT_TRUE(test_pixel_near(&pb[bytes - 4U], 0U, 255U, 0U, 255U, 0U));
    nph_gpu_render_graph_destroy(g);
    nph_gpu_buffer_destroy(f->dev, out_a);
    nph_gpu_buffer_destroy(f->dev, out_b);
}

NTEST_F(GpuFixture, async_compute_runs_on_its_own_queue)
{
    REQUIRE_DEVICE(f);
    if (!nph_gpu_device_info(f->dev)->features.async_compute) {
        NTEST_SKIP("no async compute queue on this device");
    }
    NphGpuRenderGraph *g = make_graph(f->dev, NULL);
    NphGpuShader shader = test_load_shader(f->dev, "test_fill.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &cd, &pipeline), NPH_RESULT_OK);
    NphGpuBuffer out = test_make_buffer(f->dev, 1024U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                        NPH_GPU_MEMORY_READBACK, "async out");
    for (u32 frame = 0; frame < 2U; ++frame) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        nph_gpu_rg_begin(g);
        const NphGpuBufferDesc bd = transient_buffer_desc(1024U, "async target");
        NphGpuRgBuffer t = nph_gpu_rg_create_buffer(g, &bd);
        NphGpuRgBuffer dst = nph_gpu_rg_import_buffer(g, out);
        FillPass fill = {pipeline, t, 256U, 9U, 0U};
        CopyPass copy = {t, dst, 1024U, 0U, 0U};
        NphGpuRgPass *fp = add_pass(g, "async fill", NPH_GPU_RG_PASS_COMPUTE,
                                    (u32)NPH_GPU_RG_PASS_ASYNC_COMPUTE, fill_execute, &fill);
        nph_gpu_rg_write_buffer(fp, t, NPH_GPU_RG_USE_STORAGE_WRITE);
        NphGpuRgPass *cp = add_pass(g, "copy", NPH_GPU_RG_PASS_TRANSFER, 0U, copy_execute, &copy);
        nph_gpu_rg_read_buffer(cp, t, NPH_GPU_RG_USE_TRANSFER_SRC);
        nph_gpu_rg_write_buffer(cp, dst, NPH_GPU_RG_USE_TRANSFER_DST);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_OK);
        const NphGpuRgStats stats = nph_gpu_rg_stats(g);
        NTEST_ASSERT_EQ_UINT(stats.async_passes, 1U);
        NTEST_ASSERT_EQ_UINT(stats.submits, 2U);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
        const u32 *words = (const u32 *)nph_gpu_buffer_map(f->dev, out);
        for (u32 i = 0; i < 256U; ++i) {
            NTEST_ASSERT_EQ_UINT(words[i], 9U * i);
        }
    }
    nph_gpu_render_graph_destroy(g);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, shader);
    nph_gpu_buffer_destroy(f->dev, out);
}

NTEST_F(GpuFixture, passes_record_in_parallel_on_the_job_system)
{
    REQUIRE_DEVICE(f);
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = nph_allocator_system();
    jd.worker_count = 3U;
    NphJobSystem *js = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &js), NPH_RESULT_OK);
    NphGpuRenderGraph *g = make_graph(f->dev, js);
    NphGpuShader shader = test_load_shader(f->dev, "test_fill.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &cd, &pipeline), NPH_RESULT_OK);
    enum {
        PASSES = 8U,
        WORDS = 128U
    };
    NphGpuBuffer out =
        test_make_buffer(f->dev, PASSES * WORDS * 4U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                         NPH_GPU_MEMORY_READBACK, "parallel out");
    nph_gpu_rg_begin(g);
    NphGpuRgBuffer dst = nph_gpu_rg_import_buffer(g, out);
    FillPass fills[PASSES];
    CopyPass copies[PASSES];
    for (u32 i = 0; i < PASSES; ++i) {
        const NphGpuBufferDesc bd = transient_buffer_desc(WORDS * 4U, "piece");
        NphGpuRgBuffer t = nph_gpu_rg_create_buffer(g, &bd);
        fills[i].pipeline = pipeline;
        fills[i].target = t;
        fills[i].count = WORDS;
        fills[i].value = i + 1U;
        fills[i].ran = 0U;
        NphGpuRgPass *fp =
            add_pass(g, "fill", NPH_GPU_RG_PASS_COMPUTE, 0U, fill_execute, &fills[i]);
        nph_gpu_rg_write_buffer(fp, t, NPH_GPU_RG_USE_STORAGE_WRITE);
        copies[i].src = t;
        copies[i].dst = dst;
        copies[i].size = WORDS * 4U;
        copies[i].dst_offset = (u64)i * WORDS * 4U;
        copies[i].ran = 0U;
        NphGpuRgPass *cp =
            add_pass(g, "copy", NPH_GPU_RG_PASS_TRANSFER, 0U, copy_execute, &copies[i]);
        nph_gpu_rg_read_buffer(cp, t, NPH_GPU_RG_USE_TRANSFER_SRC);
        nph_gpu_rg_write_buffer(cp, dst, NPH_GPU_RG_USE_TRANSFER_DST);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_rg_stats(g).command_buffers, 2U * PASSES);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const u32 *words = (const u32 *)nph_gpu_buffer_map(f->dev, out);
    for (u32 i = 0; i < PASSES; ++i) {
        NTEST_ASSERT_EQ_UINT(fills[i].ran, 1U);
        for (u32 w = 0; w < WORDS; ++w) {
            NTEST_ASSERT_EQ_UINT(words[i * WORDS + w], (i + 1U) * w);
        }
    }
    nph_gpu_render_graph_destroy(g);
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, shader);
    nph_gpu_buffer_destroy(f->dev, out);
    nph_job_system_destroy(js);
}

NTEST_F(GpuFixture, reports_pass_timings_two_frames_later)
{
    REQUIRE_DEVICE(f);
    NphGpuRenderGraph *g = make_graph(f->dev, NULL);
    NphGpuBuffer out = test_make_buffer(f->dev, 4096U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                        NPH_GPU_MEMORY_READBACK, "timed out");
    for (u32 frame = 0; frame < 4U; ++frame) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        NphGpuRgPassTiming timings[8];
        u64 timed_frame = 0U;
        const u32 n = nph_gpu_rg_timings(g, timings, 8U, &timed_frame);
        if (frame >= NPH_GPU_FRAMES_IN_FLIGHT) {
            NTEST_ASSERT_EQ_UINT(n, 2U);
            NTEST_ASSERT_EQ_UINT(timed_frame, frame - NPH_GPU_FRAMES_IN_FLIGHT);
            NTEST_ASSERT_EQ_STR(timings[0].name, "clear");
            NTEST_ASSERT_EQ_STR(timings[1].name, "copy");
            NTEST_ASSERT_EQ_INT(timings[0].queue, NPH_GPU_QUEUE_GRAPHICS);
        } else {
            NTEST_ASSERT_EQ_UINT(n, 0U);
        }
        nph_gpu_rg_begin(g);
        NphGpuImageDesc id;
        memset(&id, 0, sizeof id);
        id.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
        id.width = 32U;
        id.height = 32U;
        id.mip_count = 1U;
        id.usage = (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
        NphGpuRgImage img = nph_gpu_rg_create_image(g, &id);
        NphGpuRgBuffer dst = nph_gpu_rg_import_buffer(g, out);
        ClearPass clear = {img, nph_gpu_clear_color(0.5F, 0.5F, 0.5F, 1.0F), 0U};
        ImageCopyPass copy = {img, dst, 0U};
        NphGpuRgPass *p1 =
            add_pass(g, "clear", NPH_GPU_RG_PASS_TRANSFER, 0U, clear_execute, &clear);
        nph_gpu_rg_write_image(p1, img, NPH_GPU_RG_USE_TRANSFER_DST, 0U, 1U);
        NphGpuRgPass *p2 =
            add_pass(g, "copy", NPH_GPU_RG_PASS_TRANSFER, 0U, image_copy_execute, &copy);
        nph_gpu_rg_read_image(p2, img, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, 1U);
        nph_gpu_rg_write_buffer(p2, dst, NPH_GPU_RG_USE_TRANSFER_DST);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_gpu_render_graph_destroy(g);
    nph_gpu_buffer_destroy(f->dev, out);
}

NTEST_F(GpuFixture, declaration_limits_and_errors)
{
    REQUIRE_DEVICE(f);
    NphGpuRenderGraphDesc desc;
    memset(&desc, 0, sizeof desc);
    NphGpuRenderGraph *g = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(f->dev, &desc, &g),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_gpu_render_graph_destroy(NULL);
    desc.alloc = nph_allocator_system();
    desc.max_passes = 2U;
    desc.max_resources = 2U;
    desc.max_barriers = 1U;
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(f->dev, &desc, &g), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_PTR(nph_gpu_rg_device(g), f->dev);
    nph_gpu_rg_begin(g);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(g), NPH_RESULT_INVALID_ARGUMENT); // not compiled
    const NphGpuBufferDesc bd = transient_buffer_desc(256U, "t");
    NphGpuRgBuffer b0 = nph_gpu_rg_create_buffer(g, &bd);
    NphGpuRgBuffer b1 = nph_gpu_rg_create_buffer(g, &bd);
    NphGpuRgBuffer b2 = nph_gpu_rg_create_buffer(g, &bd); // over the limit
    NTEST_ASSERT_FALSE(nph_gpu_rg_buffer_is_null(b0));
    NTEST_ASSERT_FALSE(nph_gpu_rg_buffer_is_null(b1));
    NTEST_ASSERT_TRUE(nph_gpu_rg_buffer_is_null(b2));
    NTEST_ASSERT_TRUE(nph_gpu_rg_image_is_null(nph_gpu_rg_import_image(g, NPH_GPU_IMAGE_NULL)));
    NTEST_ASSERT_TRUE(nph_gpu_rg_buffer_is_null(nph_gpu_rg_import_buffer(g, NPH_GPU_BUFFER_NULL)));
    NTEST_ASSERT_TRUE(nph_gpu_buffer_is_null(nph_gpu_rg_buffer(g, b2)));
    NTEST_ASSERT_TRUE(nph_gpu_image_is_null(nph_gpu_rg_image(g, NPH_GPU_RG_IMAGE_NULL)));
    CopyPass copy = {b0, b1, 256U, 0U, 0U};
    NphGpuRgPass *p0 = add_pass(g, "a", NPH_GPU_RG_PASS_TRANSFER, (u32)NPH_GPU_RG_PASS_SIDE_EFFECTS,
                                copy_execute, &copy);
    nph_gpu_rg_read_buffer(p0, b0, NPH_GPU_RG_USE_TRANSFER_SRC);
    nph_gpu_rg_write_buffer(p0, b1, NPH_GPU_RG_USE_TRANSFER_DST);
    nph_gpu_rg_write_buffer(p0, b2, NPH_GPU_RG_USE_TRANSFER_DST); // invalid resource: logged
    NphGpuRgPass *p1 = add_pass(g, "b", NPH_GPU_RG_PASS_TRANSFER, 0U, NULL, NULL);
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    NTEST_ASSERT_NULL(nph_gpu_rg_add_pass(g, &pd)); // over the limit
    NPH_UNUSED(p1);
    const NphGpuRgImage not_imported = {b0.value};
    nph_gpu_rg_present(g, not_imported, (NphGpuSwapchain *)&desc); // not imported: logged
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(g), NPH_RESULT_FULL);   // one barrier is not enough
    nph_gpu_render_graph_destroy(g);
}
