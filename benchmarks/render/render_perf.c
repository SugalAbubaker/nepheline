// SPDX-License-Identifier: Apache-2.0
// render_perf: the renderer on a device, scene by scene (CLAUDE.md §10.7:
// "every renderer PR must state its cost in both milliseconds and DRAM
// bytes"; §12 Phase 10: "constant CPU cost vs object count").
//
// Each scene renders warm-up frames (instance uploads, the environment's
// image-based-lighting passes, the GPU's clocks), then measured frames, and
// reports:
//
//   <scene>.cpu         the median CPU time of nph_render_frame itself: the
//                       renderer's cost to the main thread (the records,
//                       the draw list, the pass declarations), excluding
//                       the render graph's compile and submit;
//   <scene>.gpu         the mean GPU time of the frame (every pass), and
//   <scene>.gpu.p99     its 99th percentile (§10.1 measures the 1 % low);
//   <scene>.gpu.<pass>  each render-graph pass's mean GPU time, with its
//                       declared footprint: the bytes of the resource
//                       ranges it reads and writes, an upper bound on its
//                       DRAM traffic (render_graph.h NphGpuRgPassTiming).
//
//     render_perf [--filter=GLOB] [--frames=N] [--json=FILE] [--budgets=FILE]
//                 [--list] [--validation] [--sw-pixels=N]
//
// --sw-pixels sets NphRendererDesc.sw_raster_pixels, the average triangle
// size the software raster takes (0: every cluster in hardware), for tuning
// the hybrid raster's split.
//
// The scenes are synthetic until Phase 10's reference scenes arrive:
//
//   grid_<n>_<pipeline>   n cubes on a flat grid under a top-down camera,
//                         every one on screen, at 512 x 512: the CPU cost
//                         against the object count, for each pipeline.
//   field_4k_<pipeline>   16384 spheres (960 triangles each) and 256
//                         pillars on a ground plane at 3840 x 2160, seen
//                         from head height: the nearer rows hide most of
//                         the field, so occlusion culling does the work.
//   field_4k_no_tile_bins the production field with every shading tile
//                         through the full resolve (NPH_RENDER_VIS_NO_TILE_BINS).
//   lights_<n>_<pipeline> the field at night, lit by n point and spot
//                         lights of 3 to 8 m range among the nearer rows
//                         (and a dim moon): the light grid's cost against
//                         the light count. `lights_1k_every_light` is the
//                         production path lighting every pixel with every
//                         light (NPH_RENDER_VIS_NO_LIGHT_GRID).
//
// Output and exit codes follow gpu_bench (docs/benchmarks.md): the nbench
// JSON schema with `metric` and `unit` per entry (`MB declared` for a
// pass); 0 ok, 2 usage error, 3 nothing matched, 4 a budget was exceeded
// (benchmarks/render_budgets.txt, `name max_ns` against ns_per_op), 77
// without a Vulkan device (CTest "skipped"). Validation is off unless
// --validation: these are the hardware's numbers, not the layer's.

#include <nepheline/core/alloc.h>
#include <nepheline/gpu/device.h>
#include <nepheline/gpu/render_graph.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/render/gpu_profiler.h>
#include <nepheline/render/primitives.h>
#include <nepheline/render/renderer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXIT_SKIPPED   77
#define MAX_FRAMES     512U
#define MAX_RESULTS    512U
#define MAX_PASS_STATS 96U
#define WARMUP_FRAMES  24U
#define WARMUP_NS      (500ULL * 1000000ULL) // and at least this long, for the clocks
#define MAX_INSTANCES  (102400U + 16384U + 256U + 1U)
#define MAX_LIGHTS     4000U

typedef enum SceneKind {
    SCENE_GRID = 0,
    SCENE_FIELD,
} SceneKind;

typedef struct SceneDef {
    const char *name;
    SceneKind kind;
    u32 count;  // grid: cubes; field: spheres
    u32 lights; // field: local lights at night (0: daylight)
    NphRenderPipeline pipeline;
    u32 width;
    u32 height;
    u32 vis_flags; // NphRenderVisFlags
} SceneDef;

static const SceneDef g_scenes[] = {
    {"grid_1k_reference", SCENE_GRID, 1024U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 512U, 512U, 0U},
    {"grid_1k_production", SCENE_GRID, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 512U, 512U, 0U},
    {"grid_10k_reference", SCENE_GRID, 10240U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 512U, 512U, 0U},
    {"grid_10k_production", SCENE_GRID, 10240U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 512U, 512U, 0U},
    {"grid_100k_reference", SCENE_GRID, 102400U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 512U, 512U, 0U},
    {"grid_100k_production", SCENE_GRID, 102400U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 512U, 512U,
     0U},
    {"field_4k_reference", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 3840U, 2160U,
     0U},
    {"field_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U,
     0U},
    {"field_4k_no_tile_bins", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U,
     (u32)NPH_RENDER_VIS_NO_TILE_BINS},
    {"lights_1k_reference", SCENE_FIELD, 16384U, 1024U, NPH_RENDER_PIPELINE_REFERENCE, 3840U, 2160U,
     0U},
    {"lights_1k_production", SCENE_FIELD, 16384U, 1024U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U},
    {"lights_1k_every_light", SCENE_FIELD, 16384U, 1024U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, (u32)NPH_RENDER_VIS_NO_LIGHT_GRID},
    {"lights_4k_production", SCENE_FIELD, 16384U, MAX_LIGHTS, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U},
};

typedef struct Result {
    char name[128];
    u32 samples;
    f64 ns;     // median (CPU) or mean (GPU) per frame
    f64 min_ns; // (0 when not tracked)
    f64 max_ns;
    f64 metric;
    const char *unit;
} Result;

typedef struct Ctx {
    NphGpuDevice *dev;
    NphGpuRenderGraph *graph;
    NphRenderGpuProfiler *profiler;
    NphRenderer *renderer;
    NphRenderMesh cube;
    NphRenderMesh sphere;
    NphRenderMesh plane;
    NphRenderMaterial materials[4];
    NphRenderLight sun;
    NphRenderLightDesc sun_desc;
    NphRenderLight lights[MAX_LIGHTS];
    u32 light_count;
    NphRenderInstance *instances;
    u32 instance_count;
    Result results[MAX_RESULTS];
    u32 result_count;
    bool no_sw_raster; // --sw-pixels=0
} Ctx;

static int compare_f64(const void *a, const void *b)
{
    const f64 x = *(const f64 *)a;
    const f64 y = *(const f64 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static bool glob_match(const char *pattern, const char *s)
{
    if (*pattern == '\0') {
        return *s == '\0';
    }
    if (*pattern == '*') {
        for (const char *t = s;; ++t) {
            if (glob_match(pattern + 1, t)) {
                return true;
            }
            if (*t == '\0') {
                return false;
            }
        }
    }
    return *s != '\0' && *pattern == *s && glob_match(pattern + 1, s + 1);
}

static Result *add_result(Ctx *ctx, const char *scene, const char *suffix)
{
    if (ctx->result_count == MAX_RESULTS) {
        return NULL;
    }
    Result *r = &ctx->results[ctx->result_count++];
    memset(r, 0, sizeof *r);
    (void)snprintf(r->name, sizeof r->name, "%s%s", scene, suffix);
    // Pass names have spaces; the budgets file splits on whitespace.
    for (char *c = r->name; *c != '\0'; ++c) {
        if (*c == ' ') {
            *c = '_';
        }
    }
    r->unit = "ns";
    return r;
}

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

static NphResult upload_mesh(Ctx *ctx, NphResult made, NphRenderMeshData *data, const char *name,
                             NphRenderMesh *out)
{
    NPH_TRY(made);
    NphRenderSubmesh sub;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(data, name, &sub, &md);
    const NphResult r = nph_render_mesh_create(ctx->renderer, &md, out);
    nph_render_mesh_data_free(data);
    return r;
}

static NphResult create_content(Ctx *ctx)
{
    NphAllocator *a = nph_allocator_system();
    NphRenderMeshData data;
    NPH_TRY(upload_mesh(ctx, nph_render_primitive_cube(a, 1.0F, &data), &data, "cube", &ctx->cube));
    // 32 x 16 segments: 960 triangles, 9 meshlets.
    NPH_TRY(upload_mesh(ctx, nph_render_primitive_sphere(a, 0.5F, 32U, 16U, &data), &data, "sphere",
                        &ctx->sphere));
    NPH_TRY(upload_mesh(ctx, nph_render_primitive_plane(a, 1.0F, 16U, 64.0F, &data), &data,
                        "ground", &ctx->plane));
    const NphVec4 colors[4] = {nph_vec4(0.8F, 0.1F, 0.1F, 1.0F), nph_vec4(1.0F, 0.78F, 0.34F, 1.0F),
                               nph_vec4(0.2F, 0.5F, 0.9F, 1.0F), nph_vec4(0.5F, 0.5F, 0.5F, 1.0F)};
    for (u32 i = 0; i < 4U; ++i) {
        NphRenderMaterialDesc md;
        nph_render_material_desc_default(&md);
        md.base_color = colors[i];
        md.metallic = i == 1U ? 1.0F : 0.0F;
        md.roughness = 0.35F + 0.15F * (f32)i;
        NPH_TRY(nph_render_material_create(ctx->renderer, &md, &ctx->materials[i]));
    }
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 0.95F, 0.9F);
    ld.intensity = 30000.0F;
    ld.direction = nph_vec3(-0.4F, -1.0F, -0.6F);
    ctx->sun_desc = ld;
    NPH_TRY(nph_render_light_create(ctx->renderer, &ld, &ctx->sun));
    NphRenderEnvironmentDesc env;
    memset(&env, 0, sizeof env);
    env.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&env.sky);
    return nph_render_set_environment(ctx->renderer, &env);
}

static NphResult add_instance(Ctx *ctx, NphRenderMesh mesh, u32 material, NphRenderTransform t)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &ctx->materials[material % 4U];
    id.material_count = 1U;
    id.transform = t;
    NPH_TRY(nph_render_instance_create(ctx->renderer, &id, &ctx->instances[ctx->instance_count]));
    ++ctx->instance_count;
    return NPH_RESULT_OK;
}

static NphRenderTransform trs(f64 x, f64 y, f64 z, NphVec3 scale, f32 yaw)
{
    return nph_render_transform_from_trs(nph_dvec3(x, y, z),
                                         nph_quat_from_axis_angle(nph_vec3_unit_y(), yaw), scale);
}

// A cheap, fixed hash for per-instance variety (Wang 1997's integer hash).
static u32 hash_u32(u32 x)
{
    x = (x ^ 61U) ^ (x >> 16U);
    x *= 9U;
    x ^= x >> 4U;
    x *= 0x27d4eb2dU;
    return x ^ (x >> 15U);
}

static NphResult build_scene(Ctx *ctx, const SceneDef *s, NphRenderView *view)
{
    const u32 side = (u32)nph_f64_ceil(nph_f64_sqrt((f64)s->count));
    if (s->kind == SCENE_GRID) {
        // Cubes 0.6 m across on a 1 m grid, the camera straight above with
        // the whole grid in its frustum.
        const f64 half = (f64)side * 0.5;
        for (u32 i = 0; i < s->count; ++i) {
            const u32 row = i / side;
            const f64 x = (f64)(i % side) - half + 0.5;
            const f64 z = (f64)row - half + 0.5;
            NPH_TRY(add_instance(ctx, ctx->cube, i, trs(x, 0.0, z, nph_vec3_splat(0.6F), 0.0F)));
        }
        const f64 height = half / (f64)nph_f32_tan(0.45F) + 2.0; // fov 0.9 rad
        view->camera = nph_render_camera_look_at(nph_dvec3(0.0, height, 0.001), nph_dvec3_zero(),
                                                 nph_vec3_unit_y(), 0.9F, 0.1F);
        return NPH_RESULT_OK;
    }
    // The field: spheres on a 2.5 m grid, every 64th cell a pillar instead
    // (a 1.5 x 8 x 1.5 m box), on a ground plane 400 m across.
    NPH_TRY(add_instance(ctx, ctx->plane, 3U,
                         trs(0.0, 0.0, 0.0, nph_vec3(400.0F, 1.0F, 400.0F), 0.0F)));
    const f64 spacing = 2.5;
    const f64 half = (f64)side * spacing * 0.5;
    for (u32 i = 0; i < s->count; ++i) {
        const u32 h = hash_u32(i);
        const u32 row = i / side;
        const f64 x = (f64)(i % side) * spacing - half;
        const f64 z = (f64)row * spacing - half;
        if ((h & 63U) == 0U) {
            NPH_TRY(add_instance(ctx, ctx->cube, h >> 8U,
                                 trs(x, 4.0, z, nph_vec3(1.5F, 8.0F, 1.5F), (f32)(h & 255U))));
        } else {
            const f32 scale = 0.8F + 0.6F * (f32)((h >> 8U) & 255U) / 255.0F;
            NPH_TRY(add_instance(ctx, ctx->sphere, h >> 16U,
                                 trs(x, (f64)scale * 0.5, z, nph_vec3_splat(scale), 0.0F)));
        }
    }
    // Head height at one corner, looking across the field's diagonal.
    view->camera =
        nph_render_camera_look_at(nph_dvec3(-half - 4.0, 1.7, -half - 4.0),
                                  nph_dvec3(half, 0.5, half), nph_vec3_unit_y(), 0.9F, 0.1F);
    if (s->lights == 0U) {
        return NPH_RESULT_OK;
    }
    // Night: a dim moon, a dark sky, and lamps among the nearer rows (an
    // 80 m square at the camera's corner), a quarter of them spots
    // pointing down.
    NphRenderLightDesc moon = ctx->sun_desc;
    moon.intensity = 0.2F;
    NPH_TRY(nph_render_light_update(ctx->renderer, ctx->sun, &moon));
    view->ev100 = 5.0F;
    view->disable_ibl = 1;
    view->background = NPH_RENDER_BACKGROUND_SOLID;
    view->background_color = nph_vec3(0.02F, 0.03F, 0.05F);
    for (u32 i = 0; i < s->lights; ++i) {
        const u32 h = hash_u32(i * 3U + 0x9E37U);
        const u32 g = hash_u32(h);
        NphRenderLightDesc ld;
        memset(&ld, 0, sizeof ld);
        ld.type = (g & 3U) == 0U ? NPH_RENDER_LIGHT_TYPE_SPOT : NPH_RENDER_LIGHT_TYPE_POINT;
        ld.position = nph_dvec3(-half + 80.0 * (f64)(h & 0xFFFFU) / 65535.0,
                                0.5 + 2.5 * (f64)(g >> 24U) / 255.0,
                                -half + 80.0 * (f64)(h >> 16U) / 65535.0);
        ld.range = 3.0F + 5.0F * (f32)((g >> 8U) & 255U) / 255.0F;
        ld.intensity = 300.0F + 2700.0F * (f32)((g >> 16U) & 255U) / 255.0F;
        ld.color = nph_vec3(1.0F, 0.6F + 0.4F * (f32)(g & 255U) / 255.0F, 0.5F);
        ld.direction = nph_vec3(0.0F, -1.0F, 0.0F);
        ld.inner_cone = 0.3F;
        ld.outer_cone = 0.7F;
        NPH_TRY(nph_render_light_create(ctx->renderer, &ld, &ctx->lights[ctx->light_count]));
        ++ctx->light_count;
    }
    return NPH_RESULT_OK;
}

static void destroy_scene(Ctx *ctx)
{
    for (u32 i = 0; i < ctx->instance_count; ++i) {
        nph_render_instance_destroy(ctx->renderer, ctx->instances[i]);
    }
    ctx->instance_count = 0U;
    if (ctx->light_count != 0U) {
        for (u32 i = 0; i < ctx->light_count; ++i) {
            nph_render_light_destroy(ctx->renderer, ctx->lights[i]);
        }
        ctx->light_count = 0U;
        NPH_IGNORE_RESULT(nph_render_light_update(ctx->renderer, ctx->sun, &ctx->sun_desc));
    }
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

typedef struct FrameRun {
    f64 cpu_ns[MAX_FRAMES];
    u32 cpu_count;
    NphRenderFrameStats stats; // the last frame's
} FrameRun;

static NphResult render_one(Ctx *ctx, const NphRenderView *view, NphGpuImage target, f64 *cpu_ns,
                            NphRenderFrameStats *stats)
{
    NPH_TRY(nph_gpu_frame_begin(ctx->dev));
    NPH_IGNORE_RESULT(nph_render_gpu_profiler_update(ctx->profiler, ctx->graph));
    nph_gpu_rg_begin(ctx->graph);
    const u64 t0 = nph_time_now_ns();
    NphResult r = nph_render_frame(ctx->renderer, view, ctx->graph, target, NULL, stats);
    *cpu_ns = (f64)(nph_time_now_ns() - t0);
    if (r == NPH_RESULT_OK) {
        r = nph_gpu_rg_compile(ctx->graph);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_gpu_rg_execute(ctx->graph);
    }
    const NphResult er = nph_gpu_frame_end(ctx->dev);
    return r != NPH_RESULT_OK ? r : er;
}

// Warm-up, then `frames` measured frames. The profiler records a frame
// NPH_GPU_FRAMES_IN_FLIGHT frames after it was submitted, so it is reset
// that many frames into the measurement and fed that many frames past it.
static NphResult run_frames(Ctx *ctx, const NphRenderView *view, NphGpuImage target, u32 frames,
                            FrameRun *run)
{
    const u64 start = nph_time_now_ns();
    f64 cpu = 0.0;
    for (u32 i = 0; i < WARMUP_FRAMES || nph_time_now_ns() - start < WARMUP_NS; ++i) {
        NPH_TRY(render_one(ctx, view, target, &cpu, &run->stats));
    }
    run->cpu_count = 0U;
    for (u32 i = 0; i < frames + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        if (i == NPH_GPU_FRAMES_IN_FLIGHT) {
            nph_render_gpu_profiler_reset(ctx->profiler);
        }
        NPH_TRY(render_one(ctx, view, target, &cpu, &run->stats));
        if (i < frames) {
            run->cpu_ns[run->cpu_count++] = cpu;
        }
    }
    // The last measured frame's timestamps.
    NPH_TRY(nph_gpu_device_wait_idle(ctx->dev));
    NPH_TRY(nph_gpu_frame_begin(ctx->dev));
    NPH_IGNORE_RESULT(nph_render_gpu_profiler_update(ctx->profiler, ctx->graph));
    return nph_gpu_frame_end(ctx->dev);
}

static NphResult run_scene(Ctx *ctx, const SceneDef *s, u32 frames)
{
    NphGpuImageDesc td;
    memset(&td, 0, sizeof td);
    td.type = NPH_GPU_IMAGE_2D;
    td.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    td.width = s->width;
    td.height = s->height;
    td.mip_count = 1U;
    td.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    td.name = "render_perf target";
    NphGpuImage target = NPH_GPU_IMAGE_NULL;
    NPH_TRY(nph_gpu_image_create(ctx->dev, &td, &target));
    NphRenderView view;
    nph_render_view_default(&view,
                            nph_render_camera_look_at(nph_dvec3(0.0, 1.0, 5.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 0.9F, 0.1F));
    view.pipeline = s->pipeline;
    view.ev100 = 13.0F;
    view.vis_flags = s->vis_flags;
    if (ctx->no_sw_raster) {
        view.vis_flags |= (u32)NPH_RENDER_VIS_NO_SW_RASTER;
    }
    NphResult r = build_scene(ctx, s, &view);
    static FrameRun run;
    if (r == NPH_RESULT_OK) {
        r = run_frames(ctx, &view, target, frames, &run);
    }
    destroy_scene(ctx);
    nph_gpu_image_destroy(ctx->dev, target);
    NPH_TRY(r);
    if (run.stats.pipeline != s->pipeline) {
        fprintf(stderr, "render_perf: %s: the device has no production path; skipped\n", s->name);
        return NPH_RESULT_OK;
    }

    qsort(run.cpu_ns, run.cpu_count, sizeof run.cpu_ns[0], compare_f64);
    Result *cpu = add_result(ctx, s->name, ".cpu");
    if (cpu == NULL) {
        return NPH_RESULT_FULL;
    }
    cpu->samples = run.cpu_count;
    cpu->ns = run.cpu_ns[run.cpu_count / 2U];
    cpu->min_ns = run.cpu_ns[0];
    cpu->max_ns = run.cpu_ns[run.cpu_count - 1U];
    cpu->metric = cpu->ns;

    static NphRenderPassStats passes[MAX_PASS_STATS];
    const u32 n = nph_render_gpu_profiler_stats(ctx->profiler, passes, MAX_PASS_STATS);
    printf("%s (%u x %u, %u instances): cpu %.3f ms median", s->name, s->width, s->height,
           run.stats.instances, cpu->ns / 1.0e6);
    for (u32 i = 0; i < n; ++i) {
        const NphRenderPassStats *p = &passes[i];
        if (p->samples == 0U) {
            continue; // seen in an earlier scene or the warm-up, not measured here
        }
        const bool frame = strcmp(p->name, "frame") == 0;
        char suffix[96];
        (void)snprintf(suffix, sizeof suffix, frame ? ".gpu" : ".gpu.%s", p->name);
        Result *g = add_result(ctx, s->name, suffix);
        if (g == NULL) {
            return NPH_RESULT_FULL;
        }
        g->samples = p->samples;
        g->ns = p->mean_ms * 1.0e6;
        g->min_ns = p->min_ms * 1.0e6;
        g->max_ns = p->max_ms * 1.0e6;
        g->metric = (f64)(p->read_bytes + p->write_bytes) / 1.0e6;
        g->unit = "MB declared";
        if (frame) {
            printf(", gpu %.3f ms mean, %.3f ms p99, %.1f MB declared\n", p->mean_ms, p->p99_ms,
                   g->metric);
            Result *p99 = add_result(ctx, s->name, ".gpu.p99");
            if (p99 == NULL) {
                return NPH_RESULT_FULL;
            }
            p99->samples = p->samples;
            p99->ns = p->p99_ms * 1.0e6;
            p99->metric = p99->ns;
        }
    }
    for (u32 i = 0; i < n; ++i) {
        const NphRenderPassStats *p = &passes[i];
        if (p->samples != 0U && strcmp(p->name, "frame") != 0) {
            printf("    %-28s %8.3f ms  %9.1f MB read  %9.1f MB written\n", p->name, p->mean_ms,
                   (f64)p->read_bytes / 1.0e6, (f64)p->write_bytes / 1.0e6);
        }
    }
    const NphRenderFrameStats *st = &run.stats;
    if (st->pipeline == NPH_RENDER_PIPELINE_PRODUCTION) {
        printf("    clusters %u (%u in software), triangles %u; culled instances %u frustum + "
               "%u occlusion, clusters %u frustum + %u cone + %u occlusion; lights %u, %u in "
               "view%s\n",
               st->vis_clusters, st->vis_clusters_sw, st->vis_triangles,
               st->vis_instances_culled_frustum, st->vis_instances_culled_occlusion,
               st->vis_meshlets_culled_frustum, st->vis_meshlets_culled_cone,
               st->vis_meshlets_culled_occlusion, st->lights, st->vis_lights,
               st->vis_overflow != 0U ? "; LISTS OVERFLOWED" : "");
    } else {
        printf("    draws %u, triangles %u, culled %u, lights %u\n", st->draws, st->triangles,
               st->culled, st->lights);
    }
    fflush(stdout); // whole lines between the log's (stderr) when both go to one file
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static int write_json(const char *path, const Ctx *ctx)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "render_perf: cannot write %s\n", path);
        return 2;
    }
    char stamp[32] = "unknown";
    const time_t now = (time_t)(nph_time_wall_ns() / NPH_TIME_NS_PER_S);
    const struct tm *utc = gmtime(&now);
    if (utc != NULL) {
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", utc);
    }
#if NPH_CONFIG_DEBUG
    const char *config = "Debug";
#elif NPH_CONFIG_DEVELOPMENT
    const char *config = "Development";
#elif NPH_CONFIG_PROFILE
    const char *config = "Profile";
#else
    const char *config = "Shipping";
#endif
    const NphGpuDeviceInfo *info = nph_gpu_device_info(ctx->dev);
    fprintf(f,
            "{\n  \"schema\": 1,\n  \"executable\": \"render_perf\",\n  \"timestamp\": \"%s\",\n",
            stamp);
    fprintf(f, "  \"config\": \"%s\",\n", config);
    fprintf(f,
            "  \"host\": {\"os\": \"%s\", \"arch\": \"x86_64\", \"gpu\": \"%s\", \"driver\": "
            "\"%s %s\", \"api\": \"%u.%u.%u\"},\n",
            nph_platform_os_name(), info->name, info->driver_name, info->driver_info,
            info->api_version_major, info->api_version_minor, info->api_version_patch);
    fprintf(f, "  \"benchmarks\": [\n");
    for (u32 i = 0; i < ctx->result_count; ++i) {
        const Result *r = &ctx->results[i];
        fprintf(f,
                "    {\"name\": \"%s\", \"label\": \"\", \"iterations\": 1, "
                "\"items_per_iteration\": 1, \"repeats\": %u, \"ns_per_op\": %.3f, "
                "\"min_ns_per_op\": %.3f, \"max_ns_per_op\": %.3f, \"metric\": %.3f, "
                "\"unit\": \"%s\"}%s\n",
                r->name, r->samples, r->ns, r->min_ns, r->max_ns, r->metric, r->unit,
                i + 1U < ctx->result_count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}

static int check_budgets(const char *path, const Ctx *ctx)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "render_perf: cannot read budgets %s\n", path);
        return 2;
    }
    int status = 0;
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        char name[128];
        if (sscanf(line, "%127s", name) <= 0) {
            continue;
        }
        const char *rest = strstr(line, name) + strlen(name);
        char *end = NULL;
        const double max_ns = strtod(rest, &end);
        if (end == rest || !(max_ns > 0.0)) {
            fprintf(stderr, "render_perf: %s: expected `name max_ns`, got: %s", path, line);
            fclose(f);
            return 2;
        }
        for (u32 i = 0; i < ctx->result_count; ++i) {
            const Result *r = &ctx->results[i];
            if (strcmp(r->name, name) == 0 && r->ns > max_ns) {
                fprintf(stderr, "BUDGET EXCEEDED: %s %.3f ms > %.3f ms\n", name, r->ns / 1.0e6,
                        max_ns / 1.0e6);
                status = 4;
            }
        }
    }
    fclose(f);
    return status;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static NphResult setup(Ctx *ctx, bool validation, f64 sw_pixels)
{
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.app_name = "render_perf";
    dd.headless = 1;
    dd.validation = validation ? NPH_GPU_TOGGLE_ON : NPH_GPU_TOGGLE_OFF;
    // The renderer logs its pipelines' registers and spills (stderr).
    dd.shader_statistics = 1;
    NPH_TRY(nph_gpu_device_create(&dd, &ctx->dev));
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    gd.name = "render_perf";
    NPH_TRY(nph_gpu_render_graph_create(ctx->dev, &gd, &ctx->graph));
    NphRenderGpuProfilerDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.alloc = nph_allocator_system();
    pd.history = MAX_FRAMES;
    pd.max_passes = MAX_PASS_STATS;
    NPH_TRY(nph_render_gpu_profiler_create(&pd, &ctx->profiler));
    NphRendererDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.alloc = nph_allocator_system();
    rd.dev = ctx->dev;
    rd.shader_dir = NPH_SHADER_DIR;
    rd.max_instances = MAX_INSTANCES;
    rd.max_draws = MAX_INSTANCES;    // the reference path draws every grid cube
    rd.max_lights = MAX_LIGHTS + 1U; // and the sun
    if (sw_pixels >= 0.0) {
        // Zero would pick the default: none goes through the view's flag.
        rd.sw_raster_pixels = (f32)sw_pixels;
        ctx->no_sw_raster = sw_pixels == 0.0;
    }
    rd.name = "render_perf";
    NPH_TRY(nph_renderer_create(&rd, &ctx->renderer));
    ctx->instances = NPH_ALLOC_ARRAY(nph_allocator_system(), NphRenderInstance, MAX_INSTANCES);
    if (ctx->instances == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    return create_content(ctx);
}

static void teardown(Ctx *ctx)
{
    if (ctx->instances != NULL) {
        destroy_scene(ctx);
        NPH_FREE_ARRAY(nph_allocator_system(), NphRenderInstance, ctx->instances, MAX_INSTANCES);
    }
    if (ctx->renderer != NULL) {
        nph_render_light_destroy(ctx->renderer, ctx->sun);
        for (u32 i = 0; i < 4U; ++i) {
            nph_render_material_destroy(ctx->renderer, ctx->materials[i]);
        }
        nph_render_mesh_destroy(ctx->renderer, ctx->plane);
        nph_render_mesh_destroy(ctx->renderer, ctx->sphere);
        nph_render_mesh_destroy(ctx->renderer, ctx->cube);
    }
    nph_renderer_destroy(ctx->renderer);
    nph_render_gpu_profiler_destroy(ctx->profiler);
    nph_gpu_render_graph_destroy(ctx->graph);
    nph_gpu_device_destroy(ctx->dev);
}

static int usage(void)
{
    fprintf(stderr, "usage: render_perf [--filter=GLOB] [--frames=N] [--json=FILE] "
                    "[--budgets=FILE] [--list] [--validation] [--sw-pixels=N]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *filter = "*";
    const char *json = NULL;
    const char *budgets = NULL;
    u32 frames = 120U;
    bool list = false;
    bool validation = false;
    f64 sw_pixels = -1.0;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--filter=", 9U) == 0) {
            filter = argv[i] + 9;
        } else if (strncmp(argv[i], "--frames=", 9U) == 0) {
            frames = (u32)strtoul(argv[i] + 9, NULL, 10);
        } else if (strncmp(argv[i], "--json=", 7U) == 0) {
            json = argv[i] + 7;
        } else if (strncmp(argv[i], "--budgets=", 10U) == 0) {
            budgets = argv[i] + 10;
        } else if (strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (strncmp(argv[i], "--sw-pixels=", 12U) == 0) {
            sw_pixels = strtod(argv[i] + 12, NULL);
        } else if (strcmp(argv[i], "--validation") == 0) {
            validation = true;
        } else {
            return usage();
        }
    }
    frames = nph_clamp_u32(frames, 1U, MAX_FRAMES - NPH_GPU_FRAMES_IN_FLIGHT);
    u32 matched = 0U;
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_scenes); ++i) {
        if (glob_match(filter, g_scenes[i].name)) {
            ++matched;
            if (list) {
                printf("%s\n", g_scenes[i].name);
            }
        }
    }
    if (list) {
        return 0;
    }
    if (matched == 0U) {
        fprintf(stderr, "render_perf: no scene matches '%s'\n", filter);
        return 3;
    }

    static Ctx ctx;
    const NphResult sr = setup(&ctx, validation, sw_pixels);
    if (sr == NPH_RESULT_UNSUPPORTED && ctx.dev == NULL) {
        fprintf(stderr, "render_perf: no Vulkan device; skipping\n");
        return EXIT_SKIPPED;
    }
    int status = 0;
    if (sr != NPH_RESULT_OK) {
        fprintf(stderr, "render_perf: setup failed: %s\n", nph_result_string(sr));
        status = 1;
    }
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_scenes) && status == 0; ++i) {
        if (!glob_match(filter, g_scenes[i].name)) {
            continue;
        }
        const NphResult r = run_scene(&ctx, &g_scenes[i], frames);
        if (r != NPH_RESULT_OK) {
            fprintf(stderr, "render_perf: %s failed: %s\n", g_scenes[i].name, nph_result_string(r));
            status = 1;
        }
    }
    if (status == 0 && ctx.dev != NULL && validation &&
        nph_gpu_device_validation_stats(ctx.dev).errors != 0U) {
        fprintf(stderr, "render_perf: validation errors were reported\n");
        status = 1;
    }
    if (status == 0 && json != NULL) {
        status = write_json(json, &ctx);
    }
    if (status == 0 && budgets != NULL) {
        status = check_budgets(budgets, &ctx);
    }
    teardown(&ctx);
    return status;
}
