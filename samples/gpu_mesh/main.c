// SPDX-License-Identifier: Apache-2.0
// gpu_mesh: the Phase 5 exit sample (CLAUDE.md §12: "textured spinning
// mesh with zero validation and sync errors"). Spinning textured cubes:
//
//   - vertices and indices uploaded through the staging ring and pulled
//     from a buffer address in the vertex shader (no vertex input state);
//   - a procedural checkerboard texture with a mip chain generated on the
//     GPU, sampled through the bindless set;
//   - a reverse-Z depth buffer that is a transient of the render graph;
//   - per-instance transforms computed by an async compute pass every
//     frame, consumed by the draw pass on the graphics queue;
//   - shader hot reload: rebuild the shaders (cmake --build ... --target
//     nph_shaders_samples) while it runs and the pipeline is rebuilt.
//
//     gpu_mesh [--frames=N] [--instances=N] [--vsync]
//
// Exit status 77: no window system or Vulkan device (CTest "skipped");
// 1: a validation error was reported.

#include <nepheline/gpu/render_graph.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>

#include <nph_shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "gpu_mesh", NPH_LOG_LEVEL_INFO);

#define EXIT_SKIPPED  77
#define TEXTURE_SIZE  256U
#define MAX_INSTANCES 4096U

// ---------------------------------------------------------------------------
// Geometry: a unit cube, 24 vertices (one normal per face), 36 indices
// ---------------------------------------------------------------------------

static void make_cube(NphMeshVertex *v, u16 *indices)
{
    static const f32 normals[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                      {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
    static const f32 tangents[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, -1},
                                       {0, 0, 1}, {1, 0, 0},  {1, 0, 0}};
    for (u32 face = 0; face < 6U; ++face) {
        const f32 *n = normals[face];
        const f32 *t = tangents[face];
        const f32 b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2],
                          n[0] * t[1] - n[1] * t[0]};
        for (u32 corner = 0; corner < 4U; ++corner) {
            const f32 u = (corner == 1U || corner == 2U) ? 1.0F : 0.0F;
            const f32 w = (corner >= 2U) ? 1.0F : 0.0F;
            const f32 su = u * 2.0F - 1.0F;
            const f32 sw = w * 2.0F - 1.0F;
            NphMeshVertex *out = &v[(u64)face * 4U + corner];
            out->position = nph_vec4(n[0] + t[0] * su + b[0] * sw, n[1] + t[1] * su + b[1] * sw,
                                     n[2] + t[2] * su + b[2] * sw, 1.0F);
            out->normal_uv = nph_vec4(n[0], n[1], n[2], u);
            out->extra = nph_vec4(w, 0.0F, 0.0F, 0.0F);
        }
        const u16 base = (u16)(face * 4U);
        u16 *i = &indices[(u64)face * 6U];
        i[0] = base;
        i[1] = (u16)(base + 1U);
        i[2] = (u16)(base + 2U);
        i[3] = base;
        i[4] = (u16)(base + 2U);
        i[5] = (u16)(base + 3U);
    }
}

static void make_checkerboard(u8 *texels)
{
    for (u32 y = 0; y < TEXTURE_SIZE; ++y) {
        for (u32 x = 0; x < TEXTURE_SIZE; ++x) {
            const u32 cell = ((x / 32U) + (y / 32U)) & 1U;
            const u32 fine = ((x / 4U) + (y / 4U)) & 1U;
            u8 *p = &texels[((u64)y * TEXTURE_SIZE + x) * 4U];
            p[0] = (u8)(cell ? 235U : 40U);
            p[1] = (u8)(cell ? 180U : 90U);
            p[2] = (u8)(cell ? 60U : 200U);
            if (fine) {
                p[0] = (u8)(p[0] - p[0] / 8U);
                p[1] = (u8)(p[1] - p[1] / 8U);
                p[2] = (u8)(p[2] - p[2] / 8U);
            }
            p[3] = 255U;
        }
    }
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

typedef struct Scene {
    NphGpuDevice *dev;
    NphGpuPipeline anim_pipeline;
    NphGpuPipeline mesh_pipeline;
    NphGpuBuffer vertices;
    NphGpuBuffer indices;
    NphGpuBuffer instances;
    NphGpuImage texture;
    NphGpuSampler sampler;
    NphGpuRgBuffer rg_instances;
    NphMeshPush push;
} Scene;

static void anim_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    Scene *s = (Scene *)user;
    nph_gpu_cmd_bind_pipeline(cmd, s->anim_pipeline);
    nph_gpu_cmd_push_constants(cmd, &s->push, sizeof s->push);
    nph_gpu_cmd_dispatch(cmd, (s->push.instance_count + 63U) / 64U, 1U, 1U);
}

static void draw_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    Scene *s = (Scene *)user;
    nph_gpu_cmd_bind_pipeline(cmd, s->mesh_pipeline);
    nph_gpu_cmd_bind_index_buffer(cmd, s->indices, 0U, NPH_GPU_INDEX_U16);
    nph_gpu_cmd_push_constants(cmd, &s->push, sizeof s->push);
    nph_gpu_cmd_draw_indexed(cmd, 36U, s->push.instance_count, 0U, 0, 0U);
}

// ---------------------------------------------------------------------------
// Shader hot reload: the .spv files are polled for a newer write time.
// ---------------------------------------------------------------------------

typedef struct WatchedShader {
    const char *file;
    char path[512];
    u64 modified_ns;
    NphGpuShader shader;
    NphGpuPipeline *pipelines[2];
    u32 pipeline_count;
} WatchedShader;

static NphResult watched_load(NphGpuDevice *dev, WatchedShader *w, const char *file)
{
    w->file = file;
    snprintf(w->path, sizeof w->path, "%s/%s.spv", NPH_SHADER_DIR, file);
    NphFileInfo info;
    if (nph_fs_stat(w->path, &info) == NPH_RESULT_OK) {
        w->modified_ns = info.modified_ns;
    }
    return nph_gpu_shader_load(dev, w->path, file, &w->shader);
}

static void watched_poll(NphGpuDevice *dev, WatchedShader *w)
{
    NphFileInfo info;
    if (nph_fs_stat(w->path, &info) != NPH_RESULT_OK || info.modified_ns == w->modified_ns) {
        return;
    }
    w->modified_ns = info.modified_ns;
    void *data = NULL;
    u64 size = 0U;
    if (nph_file_read_all(w->path, nph_allocator_system(), &data, &size) != NPH_RESULT_OK) {
        return;
    }
    NphResult r = nph_gpu_shader_reload(dev, w->shader, data, size);
    nph_file_free_all(nph_allocator_system(), data, size);
    for (u32 i = 0; i < w->pipeline_count && r == NPH_RESULT_OK; ++i) {
        r = nph_gpu_pipeline_reload(dev, *w->pipelines[i]);
    }
    if (r == NPH_RESULT_OK) {
        NPH_LOG_INFO(&LOG_SAMPLE, "hot reload: %s (generation %u)", w->file,
                     nph_gpu_shader_generation(dev, w->shader));
    } else {
        NPH_LOG_WARN(&LOG_SAMPLE, "hot reload of %s failed: %s; keeping the previous pipeline",
                     w->file, nph_result_string(r));
    }
}

// ---------------------------------------------------------------------------
// Setup: buffers, texture with mips, pipelines
// ---------------------------------------------------------------------------

static NphResult setup_scene(Scene *s, u32 instance_count, NphGpuFormat color_format,
                             WatchedShader *shaders)
{
    NphGpuDevice *dev = s->dev;
    NphAllocator *alloc = nph_allocator_system();

    NphMeshVertex *vertices = NPH_ALLOC_ARRAY(alloc, NphMeshVertex, 24U);
    u16 *indices = NPH_ALLOC_ARRAY(alloc, u16, 36U);
    u8 *texels = NPH_ALLOC_ARRAY(alloc, u8, (u64)TEXTURE_SIZE * TEXTURE_SIZE * 4U);
    if (vertices == NULL || indices == NULL || texels == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    make_cube(vertices, indices);
    make_checkerboard(texels);

    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = 24U * sizeof(NphMeshVertex);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.name = "cube vertices";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &s->vertices));
    bd.size = 36U * sizeof(u16);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_INDEX | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.name = "cube indices";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &s->indices));
    bd.size = (u64)instance_count * sizeof(NphMat4);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    bd.name = "instance transforms";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &s->instances));
    NPH_TRY(nph_gpu_upload_buffer(dev, s->vertices, 0U, vertices, 24U * sizeof(NphMeshVertex)));
    NPH_TRY(nph_gpu_upload_buffer(dev, s->indices, 0U, indices, 36U * sizeof(u16)));

    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    id.width = TEXTURE_SIZE;
    id.height = TEXTURE_SIZE;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
               (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    id.name = "checkerboard";
    NPH_TRY(nph_gpu_image_create(dev, &id, &s->texture));
    NPH_TRY(nph_gpu_upload_image(dev, s->texture, 0U, 0U, 1U, texels,
                                 (u64)TEXTURE_SIZE * TEXTURE_SIZE * 4U));
    NPH_TRY(nph_gpu_upload_flush(dev, NULL));
    NPH_FREE_ARRAY(alloc, NphMeshVertex, vertices, 24U);
    NPH_FREE_ARRAY(alloc, u16, indices, 36U);
    NPH_FREE_ARRAY(alloc, u8, texels, (u64)TEXTURE_SIZE * TEXTURE_SIZE * 4U);

    // Mip chain: mip 0 is in SHADER_READ_ONLY after the upload.
    NphGpuCmd *cmd = NULL;
    NPH_TRY(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "generate mips", &cmd));
    NphGpuImageBarrier to_src;
    memset(&to_src, 0, sizeof to_src);
    to_src.image = s->texture;
    to_src.src_stages = (u32)NPH_GPU_STAGE_ALL;
    to_src.src_access = (u32)NPH_GPU_ACCESS_MEMORY_READ;
    to_src.old_layout = NPH_GPU_LAYOUT_SHADER_READ_ONLY;
    to_src.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
    to_src.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
    to_src.new_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
    to_src.mip_count = 1U;
    to_src.layer_count = NPH_GPU_ALL_LAYERS;
    NphGpuBarrierDesc barrier;
    memset(&barrier, 0, sizeof barrier);
    barrier.images = &to_src;
    barrier.image_count = 1U;
    nph_gpu_cmd_barrier(cmd, &barrier);
    nph_gpu_cmd_generate_mips(cmd, s->texture, NPH_GPU_LAYOUT_SHADER_READ_ONLY,
                              (u32)NPH_GPU_STAGE_FRAGMENT_SHADER, (u32)NPH_GPU_ACCESS_SHADER_READ);
    nph_gpu_cmd_end(cmd);
    NphGpuSubmitDesc submit;
    memset(&submit, 0, sizeof submit);
    submit.cmds = &cmd;
    submit.cmd_count = 1U;
    NPH_TRY(nph_gpu_submit(dev, &submit, NULL));
    NPH_TRY(nph_gpu_device_wait_idle(dev));

    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    sd.max_anisotropy = 8.0F;
    sd.name = "trilinear aniso";
    NPH_TRY(nph_gpu_sampler_create(dev, &sd, &s->sampler));

    NPH_TRY(watched_load(dev, &shaders[0], "mesh.vert"));
    NPH_TRY(watched_load(dev, &shaders[1], "mesh.frag"));
    NPH_TRY(watched_load(dev, &shaders[2], "mesh_anim.comp"));

    NphGpuGraphicsPipelineDesc gp;
    memset(&gp, 0, sizeof gp);
    gp.vertex = shaders[0].shader;
    gp.fragment = shaders[1].shader;
    gp.cull_mode = NPH_GPU_CULL_BACK;
    gp.color_formats[0] = color_format;
    gp.color_count = 1U;
    gp.depth_format = NPH_GPU_FORMAT_D32_SFLOAT;
    gp.depth.test = 1;
    gp.depth.write = 1;
    gp.depth.compare = NPH_GPU_COMPARE_GREATER; // reverse-Z
    gp.name = "mesh";
    NPH_TRY(nph_gpu_pipeline_create_graphics(dev, &gp, &s->mesh_pipeline));
    NphGpuComputePipelineDesc cp;
    memset(&cp, 0, sizeof cp);
    cp.shader = shaders[2].shader;
    cp.name = "instance animation";
    NPH_TRY(nph_gpu_pipeline_create_compute(dev, &cp, &s->anim_pipeline));
    shaders[0].pipelines[0] = &s->mesh_pipeline;
    shaders[0].pipeline_count = 1U;
    shaders[1].pipelines[0] = &s->mesh_pipeline;
    shaders[1].pipeline_count = 1U;
    shaders[2].pipelines[0] = &s->anim_pipeline;
    shaders[2].pipeline_count = 1U;

    s->push.vertex_address = nph_gpu_buffer_address(dev, s->vertices);
    s->push.instance_address = nph_gpu_buffer_address(dev, s->instances);
    s->push.texture_index = nph_gpu_image_sampled_index(dev, s->texture);
    s->push.sampler_index = nph_gpu_sampler_index(dev, s->sampler);
    s->push.instance_count = instance_count;
    return NPH_RESULT_OK;
}

static void destroy_scene(Scene *s, WatchedShader *shaders)
{
    NphGpuDevice *dev = s->dev;
    nph_gpu_pipeline_destroy(dev, s->mesh_pipeline);
    nph_gpu_pipeline_destroy(dev, s->anim_pipeline);
    for (u32 i = 0; i < 3U; ++i) {
        nph_gpu_shader_destroy(dev, shaders[i].shader);
    }
    nph_gpu_sampler_destroy(dev, s->sampler);
    nph_gpu_image_destroy(dev, s->texture);
    nph_gpu_buffer_destroy(dev, s->vertices);
    nph_gpu_buffer_destroy(dev, s->indices);
    nph_gpu_buffer_destroy(dev, s->instances);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    u64 max_frames = 0U;
    u32 instance_count = 12U;
    bool vsync = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--frames=", 9U) == 0) {
            max_frames = strtoull(argv[i] + 9, NULL, 10);
        } else if (strncmp(argv[i], "--instances=", 12U) == 0) {
            instance_count = (u32)strtoul(argv[i] + 12, NULL, 10);
        } else if (strcmp(argv[i], "--vsync") == 0) {
            vsync = true;
        } else {
            fprintf(stderr, "usage: gpu_mesh [--frames=N] [--instances=N] [--vsync]\n");
            return 2;
        }
    }
    instance_count = nph_clamp_u32(instance_count, 1U, MAX_INSTANCES);

    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = nph_allocator_system();
    if (nph_log_init(&log_config) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogConsoleSink console;
    console.color = nph_platform_console_enable_vt();
    console.with_location = false;
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_INFO) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    if (nph_log_thread_start(&log_thread, 5U) != NPH_RESULT_OK) {
        return 1;
    }

    int status = 0;
    NphWindow *window = NULL;
    NphGpuDevice *dev = NULL;
    NphGpuSwapchain *swapchain = NULL;
    NphGpuRenderGraph *graph = NULL;
    Scene scene;
    WatchedShader shaders[3];
    memset(&scene, 0, sizeof scene);
    memset(shaders, 0, sizeof shaders);
    bool scene_ready = false;

    if (nph_window_system_init() != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no window system available; skipping");
        status = EXIT_SKIPPED;
        goto done;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = "Nepheline gpu_mesh";
    if (nph_window_create(nph_allocator_system(), &wd, &window) != NPH_RESULT_OK) {
        status = EXIT_SKIPPED;
        goto done;
    }
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.app_name = "gpu_mesh";
    const NphResult dr = nph_gpu_device_create(&dd, &dev);
    if (dr != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no Vulkan device (%s); skipping", nph_result_string(dr));
        status = dr == NPH_RESULT_UNSUPPORTED ? EXIT_SKIPPED : 1;
        goto done;
    }
    NphGpuSwapchainDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.window = window;
    sd.vsync = vsync;
    sd.prefer_srgb = 1;
    if (nph_gpu_swapchain_create(dev, &sd, &swapchain) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the window cannot be presented to; skipping");
        status = EXIT_SKIPPED;
        goto done;
    }
    scene.dev = dev;
    const NphResult sr =
        setup_scene(&scene, instance_count, nph_gpu_swapchain_format(swapchain), shaders);
    if (sr != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "scene setup failed: %s", nph_result_string(sr));
        status = 1;
        scene_ready = true; // partial resources are released below
        goto done;
    }
    scene_ready = true;
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    gd.name = "mesh graph";
    if (nph_gpu_render_graph_create(dev, &gd, &graph) != NPH_RESULT_OK) {
        status = 1;
        goto done;
    }
    const NphGpuDeviceInfo *info = nph_gpu_device_info(dev);
    NPH_LOG_INFO(&LOG_SAMPLE, "%s: %u cubes, %s present mode, async compute %s; Escape quits",
                 info->name, instance_count, nph_gpu_swapchain_present_mode_name(swapchain),
                 info->features.async_compute ? "on its own queue" : "on the graphics queue");

    u64 frame = 0U;
    const NphStopwatch run = nph_stopwatch_start();
    NphStopwatch report = nph_stopwatch_start();
    NphStopwatch watch = nph_stopwatch_start();
    u64 report_frame = 0U;
    while (!nph_window_should_close(window) && (max_frames == 0U || frame < max_frames)) {
        if (nph_gpu_frame_begin(dev) != NPH_RESULT_OK) {
            status = 1;
            break;
        }
        nph_window_poll_events();
        NphWindowEvent e;
        while (nph_window_pop_event(window, &e)) {
            if (e.type == NPH_WINDOW_EVENT_KEY && e.u.key.key == NPH_KEY_ESCAPE &&
                e.u.key.action == NPH_KEY_ACTION_PRESS) {
                nph_window_set_should_close(window, true);
            }
        }
        if (nph_stopwatch_elapsed_ms(&watch) >= 500.0) {
            for (u32 i = 0; i < 3U; ++i) {
                watched_poll(dev, &shaders[i]);
            }
            watch = nph_stopwatch_start();
        }
        NphGpuImage backbuffer = NPH_GPU_IMAGE_NULL;
        const NphResult ar = nph_gpu_swapchain_acquire(swapchain, &backbuffer);
        if (ar == NPH_RESULT_EMPTY) {
            NPH_IGNORE_RESULT(nph_gpu_frame_end(dev));
            nph_thread_sleep_ns(16U * NPH_TIME_NS_PER_MS);
            continue;
        }
        if (ar != NPH_RESULT_OK) {
            status = 1;
            NPH_IGNORE_RESULT(nph_gpu_frame_end(dev));
            break;
        }
        u32 width = 0U;
        u32 height = 0U;
        nph_gpu_swapchain_extent(swapchain, &width, &height);

        // Camera (right-handed, Y up, reverse-Z infinite projection: ADR-0008).
        const f32 time = (f32)(nph_stopwatch_elapsed_ms(&run) / 1000.0);
        const NphMat4 view = nph_mat4_look_at(
            nph_vec3(0.0F, 2.8F, 8.0F), nph_vec3(0.0F, 0.0F, 0.0F), nph_vec3(0.0F, 1.0F, 0.0F));
        const NphMat4 proj =
            nph_mat4_perspective_infinite(60.0F * NPH_DEG_TO_RAD, (f32)width / (f32)height, 0.1F);
        scene.push.view_proj = nph_mat4_mul(proj, view);
        scene.push.time = time;

        nph_gpu_rg_begin(graph);
        NphGpuRgImage target = nph_gpu_rg_import_image(graph, backbuffer);
        NphGpuImageDesc depth_desc;
        memset(&depth_desc, 0, sizeof depth_desc);
        depth_desc.format = NPH_GPU_FORMAT_D32_SFLOAT;
        depth_desc.width = width;
        depth_desc.height = height;
        depth_desc.mip_count = 1U;
        depth_desc.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT;
        depth_desc.name = "depth";
        NphGpuRgImage depth = nph_gpu_rg_create_image(graph, &depth_desc);
        NphGpuRgBuffer instances = nph_gpu_rg_import_buffer(graph, scene.instances);

        NphGpuRgPassDesc pd;
        memset(&pd, 0, sizeof pd);
        pd.name = "animate";
        pd.type = NPH_GPU_RG_PASS_COMPUTE;
        pd.flags = (u32)NPH_GPU_RG_PASS_ASYNC_COMPUTE;
        pd.execute = anim_execute;
        pd.user = &scene;
        NphGpuRgPass *anim = nph_gpu_rg_add_pass(graph, &pd);
        nph_gpu_rg_write_buffer(anim, instances, NPH_GPU_RG_USE_STORAGE_WRITE);

        memset(&pd, 0, sizeof pd);
        pd.name = "cubes";
        pd.type = NPH_GPU_RG_PASS_GRAPHICS;
        pd.execute = draw_execute;
        pd.user = &scene;
        NphGpuRgPass *draw = nph_gpu_rg_add_pass(graph, &pd);
        NphGpuRgAttachment color;
        memset(&color, 0, sizeof color);
        color.load = NPH_GPU_LOAD_OP_CLEAR;
        color.store = NPH_GPU_STORE_OP_STORE;
        color.clear = nph_gpu_clear_color(0.04F, 0.05F, 0.09F, 1.0F);
        nph_gpu_rg_color_attachment(draw, target, &color);
        NphGpuRgAttachment ds;
        memset(&ds, 0, sizeof ds);
        ds.load = NPH_GPU_LOAD_OP_CLEAR;
        ds.store = NPH_GPU_STORE_OP_DONT_CARE;
        ds.clear = nph_gpu_clear_depth(0.0F, 0U); // reverse-Z: far is 0
        nph_gpu_rg_depth_attachment(draw, depth, &ds, 0);
        nph_gpu_rg_read_buffer(draw, instances, NPH_GPU_RG_USE_SHADER_READ);
        nph_gpu_rg_present(graph, target, swapchain);

        if (nph_gpu_rg_compile(graph) != NPH_RESULT_OK ||
            nph_gpu_rg_execute(graph) != NPH_RESULT_OK ||
            nph_gpu_swapchain_present(swapchain) != NPH_RESULT_OK ||
            nph_gpu_frame_end(dev) != NPH_RESULT_OK) {
            status = 1;
            break;
        }
        ++frame;
        if (nph_stopwatch_elapsed_ms(&report) >= 1000.0) {
            NphGpuRgPassTiming timings[4];
            const u32 n = nph_gpu_rg_timings(graph, timings, 4U, NULL);
            char text[256];
            u32 len = 0U;
            for (u32 i = 0; i < n && len < sizeof text; ++i) {
                len += (u32)snprintf(text + len, sizeof text - len, "%s%s %.3f ms",
                                     i == 0U ? "" : ", ", timings[i].name,
                                     (f64)timings[i].gpu_ns / 1.0e6);
            }
            const NphGpuRgStats st = nph_gpu_rg_stats(graph);
            NPH_LOG_INFO(&LOG_SAMPLE, "%.0f fps; GPU: %s; %u barriers, %u submits",
                         (f64)(frame - report_frame) / (nph_stopwatch_elapsed_ms(&report) / 1000.0),
                         n != 0U ? text : "(no timings yet)", st.barriers, st.submits);
            report = nph_stopwatch_start();
            report_frame = frame;
        }
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%llu frames in %.1f ms", (unsigned long long)frame,
                 nph_stopwatch_elapsed_ms(&run));

done:
    if (dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(dev));
        nph_gpu_render_graph_destroy(graph);
        if (scene_ready) {
            destroy_scene(&scene, shaders);
        }
        nph_gpu_swapchain_destroy(swapchain);
        const NphGpuValidationStats v = nph_gpu_device_validation_stats(dev);
        if (v.errors != 0U) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "%llu validation errors", (unsigned long long)v.errors);
            status = 1;
        }
        nph_gpu_device_destroy(dev);
    }
    nph_window_destroy(window);
    nph_window_system_shutdown();
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}
