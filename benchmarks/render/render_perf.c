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
//                       DRAM traffic (render_graph.h NphGpuRgPassTiming);
//   <scene>.gpu.span    the frame's span, from its first pass's beginning to
//                       its last one's end on any queue: the sum above
//                       while one queue runs every pass, less where async
//                       compute overlaps it.
//
//     render_perf [--filter=GLOB] [--frames=N] [--json=FILE] [--budgets=FILE]
//                 [--list] [--validation] [--sw-pixels=N] [--scenes=DIR]
//                 [--capture-dir=DIR] [--manifests=DIR] [--ref-size=WxH]
//                 [--debug-view=NAME] [--reference-only]
//
// --sw-pixels sets NphRendererDesc.sw_raster_pixels, the average triangle
// size the software raster takes (0: every cluster in hardware), for tuning
// the hybrid raster's split.
//
// Reference scenes (§10.7: the targets are measured on them): with
// --scenes=DIR, or the NPH_SCENES environment variable, every manifest in
// benchmarks/scenes whose cooked pack DIR holds (scripts/scenes.py fetches
// and cooks them) adds a scene per view, `<scene>_<view>_production`, run
// after the synthetic scenes at 3840 x 2160 through the production path
// with the runtime's defaults, lit by the scene's own lights (the
// synthetic scenes' sun is removed first) and seen from its own camera or
// the manifest's. --capture-dir writes each one's last frame there as a
// BMP, to look at what was measured. --manifests reads the manifests from
// another directory and --ref-size renders them at another size (the
// CTest smoke run of a small cooked scene; budgets hold at 3840 x 2160).
// --debug-view renders them as one of the renderer's debug views
// (base_color, normal, roughness, metallic, occlusion, direct, indirect,
// irradiance, clusters, triangles, instances) for looking at the content;
// the results are then named `<scene>_<view>_production_<debug view>`, so
// no budget applies to them. --reference-only runs the reference scenes
// alone (the GPU runner's nightly job, which runs the synthetic ones in a
// step of their own).
//
// The scenes are synthetic until Phase 10's reference scenes arrive. Each
// draws the runtime's defaults: bloom (r.bloom's weight, 0.04) on both
// pipelines, a temporal resolve (NPH_RENDER_AA_TEMPORAL, r.taa), ambient
// occlusion within 0.5 m (r.ao; where the sky lights the scene), and global
// illumination (r.gi) on the production path, and the scene's own exposure:
//
//   grid_<n>_<pipeline>   n cubes on a flat grid under a top-down camera,
//                         every one on screen, at 512 x 512: the CPU cost
//                         against the object count, for each pipeline.
//   field_4k_<pipeline>   16384 spheres (960 triangles each) and 256
//                         pillars on a ground plane at 3840 x 2160, seen
//                         from head height: the nearer rows hide most of
//                         the field, so occlusion culling does the work.
//   field_4k_no_taa       field_4k_production without temporal
//                         anti-aliasing: the tonemap pass where the other
//                         has the temporal resolve.
//   field_4k_no_bloom     field_4k_production without bloom: what the
//                         bloom chain costs.
//   field_4k_no_ao        field_4k_production without ambient occlusion:
//                         what the occlusion costs.
//   field_4k_no_gi        field_4k_production without global illumination:
//                         what the radiance cache costs under the sky.
//   field_4k_auto_exposure
//                         field_4k_production exposed automatically
//                         (NPH_RENDER_EXPOSURE_AUTO): the histogram and the
//                         metering on top of the bloom chain.

//   lights_<n>_<pipeline> the field at night, lit by n point and spot
//                         lights of 3 to 8 m range among the nearer rows
//                         (and a dim moon): the light grid's cost against
//                         the light count. `lights_1k_every_light` is the
//                         production path lighting every pixel with every
//                         light (NPH_RENDER_VIS_NO_LIGHT_GRID).
//   shadows_4k_<pipeline> the field in daylight, the sun casting shadows
//                         (docs/render.md "Shadows"): the reference traces
//                         a shadow ray per lit pixel, rebuilding the top
//                         level over every instance each frame; the
//                         production path draws the sun's virtual shadow
//                         map, whose pages stay drawn while nothing moves.
//   shadows_4k_no_gi      shadows_4k_production without global
//                         illumination: what the radiance cache costs with
//                         the sun's light to bounce (a shadow ray more for
//                         each of its rays that hits).
//   shadows_rt_4k_production
//                         the production path with the sun's shadows ray
//                         traced instead (NPH_RENDER_VIS_RT_SHADOWS): its
//                         top level kept while nothing moves.
//   shadows_moving_4k_production
//                         shadows_4k_production with one sphere moving
//                         every frame: the pages it touches are drawn
//                         again each frame.
//   shadows_walk_4k_production
//                         shadows_4k_production with the camera walking
//                         (1.4 m/s at 240 frames a second): the pages it
//                         comes to need are drawn as it moves.
//   lights_1k_shadows     lights_1k_production with every lamp (and the
//                         moon) casting shadows: a ray per pixel per light
//                         that reaches it.
//   motion_{turn,strafe,flick}_4k_production
//                         field_4k_production with motion blur (a
//                         180-degree shutter at 240 frames a second, 1/480 s)
//                         while the camera turns 60 degrees a second, strafes
//                         at 5 m/s (parallax: the layered gather), or turns
//                         360 degrees a second, each reversing every quarter
//                         second so the view stays over the field.
//   dof_4k_production     field_4k_production through a full-frame lens
//                         (24.8 mm at the camera's field of view) at f/1.4
//                         focused 5 m away: circles of confusion up to 4
//                         pixels in radius, all gathered at full resolution.
//   dof_slight_4k_production
//                         the same lens focused 2.6 m away: circles up to
//                         7.7 pixels, the most the full-resolution gather
//                         takes (NPH_RENDER_DOF_SLIGHT).
//   dof_large_4k_production
//                         the same lens focused 0.6 m away: circles at the
//                         32-pixel limit beyond 8 m, gathered at half
//                         resolution.
//
// Output and exit codes follow gpu_bench (docs/benchmarks.md): the nbench
// JSON schema with `metric` and `unit` per entry (`MB declared` for a
// pass); 0 ok, 2 usage error, 3 nothing matched, 4 a budget was exceeded
// (benchmarks/render_budgets.txt, `name max_ns` against ns_per_op), 77
// without a Vulkan device (CTest "skipped"). Validation is off unless
// --validation: these are the hardware's numbers, not the layer's.

#include <nepheline/asset/loader.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/core/alloc.h>
#include <nepheline/core/json.h>
#include <nepheline/gpu/commands.h>
#include <nepheline/gpu/device.h>
#include <nepheline/gpu/render_graph.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/render/gpu_profiler.h>
#include <nepheline/render/primitives.h>
#include <nepheline/render/renderer.h>

#include "capture.h"
#include "cooked_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXIT_SKIPPED   77
#define MAX_FRAMES     512U
#define MAX_RESULTS    2048U
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
    u32 shadows;   // SceneShadows
    u32 post;      // ScenePost
} SceneDef;

typedef enum SceneShadows {
    SHADOWS_NONE = 0,
    SHADOWS_SUN,        // the sun (or the moon) casts shadows
    SHADOWS_SUN_MOVING, // and an instance moves every frame
    SHADOWS_SUN_WALK,   // and the camera walks
    SHADOWS_ALL,        // the moon and every lamp cast shadows
} SceneShadows;

// How a scene's post-processing departs from the runtime's defaults.
typedef enum ScenePost {
    POST_DEFAULT = 0,
    POST_NO_TAA = 1U << 0U,        // the production path without temporal anti-aliasing
    POST_NO_BLOOM = 1U << 1U,      // no bloom
    POST_AUTO_EXPOSURE = 1U << 2U, // the exposure metered and adapted
    // Motion blur with the camera moving (SCENE_MOTION_*).
    POST_MOTION_TURN = 1U << 3U,   // turning 60 degrees a second
    POST_MOTION_STRAFE = 1U << 4U, // strafing at 5 m/s
    POST_MOTION_FLICK = 1U << 5U,  // turning 360 degrees a second
    // Depth of field through DOF_APERTURE, focused at DOF_FOCUS[...].
    POST_DOF = 1U << 6U,        // 5 m away
    POST_DOF_SLIGHT = 1U << 7U, // 2.6 m
    POST_DOF_LARGE = 1U << 8U,  // 0.6 m
    POST_NO_AO = 1U << 9U,      // no ambient occlusion
    POST_NO_GI = 1U << 10U,     // no global illumination
} ScenePost;

#define RUNTIME_BLOOM 0.04F // r.bloom's default
#define RUNTIME_AO    0.5F  // r.ao's default
// Motion blur: a 180-degree shutter at the §10.1 rate, and the camera's
// motions reversing every quarter second.
#define MOTION_FRAME   (1.0F / 240.0F)
#define MOTION_SHUTTER (1.0F / 480.0F)
#define MOTION_REVERSE 60U
// Depth of field: a full-frame lens (the view's default sensor) wide open.
#define DOF_APERTURE 1.4F

static const SceneDef g_scenes[] = {
    {"grid_1k_reference", SCENE_GRID, 1024U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 512U, 512U, 0U,
     SHADOWS_NONE, 0},
    {"grid_1k_production", SCENE_GRID, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 512U, 512U, 0U,
     SHADOWS_NONE, 0},
    {"grid_10k_reference", SCENE_GRID, 10240U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 512U, 512U, 0U,
     SHADOWS_NONE, 0},
    {"grid_10k_production", SCENE_GRID, 10240U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 512U, 512U, 0U,
     SHADOWS_NONE, 0},
    {"grid_100k_reference", SCENE_GRID, 102400U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 512U, 512U, 0U,
     SHADOWS_NONE, 0},
    {"grid_100k_production", SCENE_GRID, 102400U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 512U, 512U,
     0U, SHADOWS_NONE, 0},
    {"field_4k_reference", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 3840U, 2160U, 0U,
     SHADOWS_NONE, 0},
    {"field_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U,
     0U, SHADOWS_NONE, 0},
    {"field_4k_no_taa", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_NONE, POST_NO_TAA},
    {"field_4k_no_bloom", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_NONE, POST_NO_BLOOM},
    {"field_4k_no_ao", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_NONE, POST_NO_AO},
    {"field_4k_no_gi", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_NONE, POST_NO_GI},
    {"field_4k_auto_exposure", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_AUTO_EXPOSURE},
    {"lights_1k_reference", SCENE_FIELD, 16384U, 1024U, NPH_RENDER_PIPELINE_REFERENCE, 3840U, 2160U,
     0U, SHADOWS_NONE, 0},
    {"lights_1k_production", SCENE_FIELD, 16384U, 1024U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, 0},
    {"lights_1k_every_light", SCENE_FIELD, 16384U, 1024U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, (u32)NPH_RENDER_VIS_NO_LIGHT_GRID, SHADOWS_NONE, 0},
    {"lights_4k_production", SCENE_FIELD, 16384U, MAX_LIGHTS, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, 0},
    {"shadows_4k_reference", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_REFERENCE, 3840U, 2160U,
     0U, SHADOWS_SUN, 0},
    {"shadows_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U,
     0U, SHADOWS_SUN, 0},
    {"shadows_4k_no_gi", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_SUN, POST_NO_GI},
    {"shadows_rt_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, (u32)NPH_RENDER_VIS_RT_SHADOWS, SHADOWS_SUN, 0},
    {"shadows_moving_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_SUN_MOVING, 0},
    {"shadows_walk_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_SUN_WALK, 0},
    {"lights_1k_shadows", SCENE_FIELD, 16384U, 1024U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U,
     0U, SHADOWS_ALL, 0},
    {"motion_turn_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_MOTION_TURN},
    {"motion_strafe_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_MOTION_STRAFE},
    {"motion_flick_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_MOTION_FLICK},
    {"dof_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_NONE, POST_DOF},
    {"dof_slight_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_DOF_SLIGHT},
    {"dof_large_4k_production", SCENE_FIELD, 16384U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_DOF_LARGE},
};

// Reference scenes: a manifest (benchmarks/scenes/<scene>.json) and its
// views, and one measured scene per view.
#define MAX_REF_MANIFESTS 8U
#define MAX_REF_VIEWS     8U
#define REF_WIDTH         3840U
#define REF_HEIGHT        2160U

typedef struct RefView {
    char name[32];
    bool scene_camera; // the scene's own (first) camera, else eye and target
    NphDVec3 eye;
    NphDVec3 target;
    f32 yfov;
    f32 znear;
} RefView;

typedef struct RefManifest {
    char name[32];
    char pack[1024];     // the cooked pack, under the scenes directory
    char scene_key[256]; // its scene's identity (docs/cook.md "Identity")
    f32 ev100;
    f32 sky_intensity; // the default sky's (daylight next to a 30,000 lux sun) scale
    bool sun_shadows;  // the scene's first directional light casts shadows
    RefView views[MAX_REF_VIEWS];
    u32 view_count;
} RefManifest;

typedef struct RefScene {
    char name[96];
    u32 manifest;
    u32 view;
} RefScene;

static RefManifest g_manifests[MAX_REF_MANIFESTS];
static u32 g_manifest_count;
static RefScene g_ref_scenes[MAX_REF_MANIFESTS * MAX_REF_VIEWS];
static u32 g_ref_count;
static u32 g_ref_width = REF_WIDTH;
static u32 g_ref_height = REF_HEIGHT;

// --debug-view's names, in NphRenderDebugView order from BASE_COLOR.
static const char *const k_debug_views[] = {
    "base_color", "normal",     "roughness", "metallic",  "occlusion", "direct",
    "indirect",   "irradiance", "clusters",  "triangles", "instances",
};
NPH_STATIC_ASSERT(NPH_ARRAY_COUNT(k_debug_views) == (u32)NPH_RENDER_VIEW_COUNT - 1U,
                  "a name per debug view");
static NphRenderDebugView g_debug_view = NPH_RENDER_VIEW_LIT;

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
    // A sphere moved every frame (SHADOWS_SUN_MOVING), where it rests, and
    // the frame count.
    bool moving;
    NphRenderInstance mover;
    NphRenderTransform mover_rest;
    u32 move_frame;
    // The camera walking (SHADOWS_SUN_WALK): its step per frame.
    bool walking;
    NphDVec3 walk_step;
    // The camera turning or strafing for motion blur (POST_MOTION_*): radians
    // and metres per frame, and the frame count (the motions reverse).
    f32 turn;
    f64 strafe;
    u32 motion_frame;
    // Reference scenes: what loads them, made for the first; the manifest
    // whose scene is loaded (NPH_U32_MAX: none), and whether the synthetic
    // scenes' sun was removed for its lights.
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;
    u32 mount;
    CookedScene cooked;
    u32 loaded_manifest;
    bool sun_removed;
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
        if (s->shadows != (u32)SHADOWS_NONE) {
            NphRenderLightDesc sun = ctx->sun_desc;
            sun.shadows = 1;
            NPH_TRY(nph_render_light_update(ctx->renderer, ctx->sun, &sun));
        }
        if (s->shadows == (u32)SHADOWS_SUN_WALK) {
            // Along the view, level, at 1.4 m/s and 240 frames a second.
            const NphVec3 f =
                nph_quat_rotate(view->camera.orientation, nph_vec3(0.0F, 0.0F, -1.0F));
            const f64 step = 1.4 / 240.0 / nph_f64_sqrt((f64)f.x * (f64)f.x + (f64)f.z * (f64)f.z);
            ctx->walk_step = nph_dvec3((f64)f.x * step, 0.0, (f64)f.z * step);
            ctx->walking = true;
        }
        if (s->shadows == (u32)SHADOWS_SUN_MOVING) {
            // One more sphere, in front of the camera.
            ctx->mover_rest = trs(-half + 2.0, 0.5, -half + 2.0, nph_vec3_splat(1.0F), 0.0F);
            NPH_TRY(add_instance(ctx, ctx->sphere, 0U, ctx->mover_rest));
            ctx->mover = ctx->instances[ctx->instance_count - 1U];
            ctx->moving = true;
            ctx->move_frame = 0U;
        }
        return NPH_RESULT_OK;
    }
    // Night: a dim moon, a dark sky, and lamps among the nearer rows (an
    // 80 m square at the camera's corner), a quarter of them spots
    // pointing down.
    NphRenderLightDesc moon = ctx->sun_desc;
    moon.intensity = 0.2F;
    moon.shadows = s->shadows != (u32)SHADOWS_NONE ? 1 : 0;
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
        ld.shadows = s->shadows == (u32)SHADOWS_ALL ? 1 : 0;
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
    for (u32 i = 0; i < ctx->light_count; ++i) {
        nph_render_light_destroy(ctx->renderer, ctx->lights[i]);
    }
    ctx->light_count = 0U;
    ctx->moving = false;
    ctx->walking = false;
    ctx->turn = 0.0F;
    ctx->strafe = 0.0;
    // The sun as setup made it (a scene may have dimmed it or given it
    // shadows).
    NPH_IGNORE_RESULT(nph_render_light_update(ctx->renderer, ctx->sun, &ctx->sun_desc));
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

typedef struct FrameRun {
    f64 cpu_ns[MAX_FRAMES];
    u32 cpu_count;
    NphRenderFrameStats stats; // the last frame's
} FrameRun;

static NphResult render_one(Ctx *ctx, NphRenderView *view, NphGpuImage target, f64 *cpu_ns,
                            NphRenderFrameStats *stats)
{
    if (ctx->walking) {
        view->camera.position = nph_dvec3_add(view->camera.position, ctx->walk_step);
    }
    if (ctx->turn != 0.0F || ctx->strafe != 0.0) {
        const f32 sign = ((ctx->motion_frame++ / MOTION_REVERSE) & 1U) != 0U ? -1.0F : 1.0F;
        // About the world's up axis, and along the camera's right.
        view->camera.orientation = nph_quat_normalize(
            nph_quat_mul(nph_quat_from_axis_angle(nph_vec3_unit_y(), sign * ctx->turn),
                         view->camera.orientation));
        const NphVec3 right = nph_quat_rotate(view->camera.orientation, nph_vec3_unit_x());
        const f64 along = (f64)sign * ctx->strafe;
        view->camera.position = nph_dvec3_add(
            view->camera.position,
            nph_dvec3((f64)right.x * along, (f64)right.y * along, (f64)right.z * along));
    }
    if (ctx->moving) {
        // The sphere bobs a centimetre up and down.
        NphRenderTransform t = ctx->mover_rest;
        t.translation.y += (ctx->move_frame++ & 1U) != 0U ? 0.01 : 0.0;
        NPH_TRY(nph_render_instance_set_transform(ctx->renderer, ctx->mover, &t));
    }
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
static NphResult run_frames(Ctx *ctx, NphRenderView *view, NphGpuImage target, u32 frames,
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

static NphResult report(Ctx *ctx, const char *name, u32 width, u32 height,
                        NphRenderPipeline pipeline, bool shadows, FrameRun *run);

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
    view.anti_aliasing =
        s->pipeline == NPH_RENDER_PIPELINE_PRODUCTION && (s->post & (u32)POST_NO_TAA) == 0U
            ? NPH_RENDER_AA_TEMPORAL
            : NPH_RENDER_AA_NONE;
    view.bloom = (s->post & (u32)POST_NO_BLOOM) != 0U ? 0.0F : RUNTIME_BLOOM;
    view.ao_radius = (s->post & (u32)POST_NO_AO) != 0U ? 0.0F : RUNTIME_AO;
    view.gi = (s->post & (u32)POST_NO_GI) != 0U ? 0 : 1; // r.gi's default
    view.exposure = (s->post & (u32)POST_AUTO_EXPOSURE) != 0U ? NPH_RENDER_EXPOSURE_AUTO
                                                              : NPH_RENDER_EXPOSURE_MANUAL;
    if (ctx->no_sw_raster) {
        view.vis_flags |= (u32)NPH_RENDER_VIS_NO_SW_RASTER;
    }
    NphResult r = build_scene(ctx, s, &view);
    const u32 motion =
        s->post & ((u32)POST_MOTION_TURN | (u32)POST_MOTION_STRAFE | (u32)POST_MOTION_FLICK);
    if (motion != 0U) {
        view.shutter = MOTION_SHUTTER;
        view.frame_seconds = MOTION_FRAME;
        const f32 degrees = NPH_PI / 180.0F * MOTION_FRAME; // a degree a second, per frame
        ctx->turn = motion == (u32)POST_MOTION_TURN    ? 60.0F * degrees
                    : motion == (u32)POST_MOTION_FLICK ? 360.0F * degrees
                                                       : 0.0F;
        ctx->strafe = motion == (u32)POST_MOTION_STRAFE ? 5.0 * (f64)MOTION_FRAME : 0.0;
        ctx->motion_frame = 0U;
    }
    const u32 dof = s->post & ((u32)POST_DOF | (u32)POST_DOF_SLIGHT | (u32)POST_DOF_LARGE);
    if (dof != 0U) {
        view.aperture = DOF_APERTURE;
        view.focus_distance = dof == (u32)POST_DOF          ? 5.0F
                              : dof == (u32)POST_DOF_SLIGHT ? 2.6F
                                                            : 0.6F;
    }
    static FrameRun run;
    if (r == NPH_RESULT_OK) {
        r = run_frames(ctx, &view, target, frames, &run);
    }
    destroy_scene(ctx);
    nph_gpu_image_destroy(ctx->dev, target);
    NPH_TRY(r);
    return report(ctx, s->name, s->width, s->height, s->pipeline, s->shadows != (u32)SHADOWS_NONE,
                  &run);
}

// Records and prints a measured run of scene `name`: the CPU median, the
// GPU frame and each pass, and the frame's statistics.
static NphResult report(Ctx *ctx, const char *name, u32 width, u32 height,
                        NphRenderPipeline pipeline, bool shadows, FrameRun *run)
{
    if (run->stats.pipeline != pipeline) {
        fprintf(stderr, "render_perf: %s: the device has no production path; skipped\n", name);
        return NPH_RESULT_OK;
    }

    qsort(run->cpu_ns, run->cpu_count, sizeof run->cpu_ns[0], compare_f64);
    Result *cpu = add_result(ctx, name, ".cpu");
    if (cpu == NULL) {
        return NPH_RESULT_FULL;
    }
    cpu->samples = run->cpu_count;
    cpu->ns = run->cpu_ns[run->cpu_count / 2U];
    cpu->min_ns = run->cpu_ns[0];
    cpu->max_ns = run->cpu_ns[run->cpu_count - 1U];
    cpu->metric = cpu->ns;

    static NphRenderPassStats passes[MAX_PASS_STATS];
    const u32 n = nph_render_gpu_profiler_stats(ctx->profiler, passes, MAX_PASS_STATS);
    printf("%s (%u x %u, %u instances): cpu %.3f ms median", name, width, height,
           run->stats.instances, cpu->ns / 1.0e6);
    for (u32 i = 0; i < n; ++i) {
        const NphRenderPassStats *p = &passes[i];
        if (p->samples == 0U) {
            continue; // seen in an earlier scene or the warm-up, not measured here
        }
        const bool frame = strcmp(p->name, "frame") == 0;
        char suffix[96];
        (void)snprintf(suffix, sizeof suffix, frame ? ".gpu" : ".gpu.%s", p->name);
        Result *g = add_result(ctx, name, suffix);
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
            Result *p99 = add_result(ctx, name, ".gpu.p99");
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
    const NphRenderFrameStats *st = &run->stats;
    if (st->pipeline == NPH_RENDER_PIPELINE_PRODUCTION) {
        printf("    clusters %u (%u in software), triangles %u; culled instances %u frustum + "
               "%u occlusion, clusters %u frustum + %u cone + %u occlusion; lights %u, %u in "
               "view%s%s%s%s\n",
               st->vis_clusters, st->vis_clusters_sw, st->vis_triangles,
               st->vis_instances_culled_frustum, st->vis_instances_culled_occlusion,
               st->vis_meshlets_culled_frustum, st->vis_meshlets_culled_cone,
               st->vis_meshlets_culled_occlusion, st->lights, st->vis_lights,
               st->shadow_rays ? "; shadow rays" : "", st->motion_blur ? "; motion blur" : "",
               st->dof ? "; depth of field" : "",
               st->vis_overflow != 0U ? "; LISTS OVERFLOWED" : "");
        if (st->vsm) {
            printf("    virtual shadow map: pages requested %u, drawn %u, resident %u, evicted %u; "
                   "caster draws %u%s\n",
                   st->vsm_requested, st->vsm_drawn, st->vsm_resident, st->vsm_evicted,
                   st->vsm_draws, st->vsm_overflow != 0U ? "; OVERFLOWED" : "");
        }
    } else {
        printf("    draws %u, triangles %u, culled %u, lights %u%s\n", st->draws, st->triangles,
               st->culled, st->lights, st->shadow_rays ? "; shadow rays" : "");
    }
    if (shadows && !st->shadow_rays && !st->vsm) {
        fprintf(stderr, "render_perf: %s: the renderer cannot shadow; shadows were not drawn\n",
                name);
    }
    fflush(stdout); // whole lines between the log's (stderr) when both go to one file
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Reference scenes
// ---------------------------------------------------------------------------

static bool json_vec3(const NphJsonValue *object, const char *key, NphDVec3 *out)
{
    const NphJsonValue *a = nph_json_object_get(object, nph_str_from_cstr(key));
    f64 v[3];
    for (u32 i = 0; i < 3U; ++i) {
        if (nph_json_as_f64(nph_json_array_at(a, i), &v[i]) != NPH_RESULT_OK) {
            return false;
        }
    }
    *out = nph_dvec3(v[0], v[1], v[2]);
    return true;
}

static void json_string(const NphJsonValue *object, const char *key, char *out, u64 cap)
{
    NphStr v = NPH_STR("");
    NPH_IGNORE_RESULT(nph_json_as_str(nph_json_object_get(object, nph_str_from_cstr(key)), &v));
    const u64 n = v.len < cap - 1U ? v.len : cap - 1U;
    memcpy(out, v.data, (size_t)n);
    out[n] = '\0';
}

// Reads manifest `path` into `m` (its pack under `scenes`); false, with a
// message, when it does not describe a scene with views.
static bool parse_manifest(const char *path, const char *scenes, RefManifest *m)
{
    NphAllocator *a = nph_allocator_system();
    void *text = NULL;
    u64 size = 0U;
    if (nph_file_read_all(path, a, &text, &size) != NPH_RESULT_OK) {
        fprintf(stderr, "render_perf: cannot read %s\n", path);
        return false;
    }
    NphJsonDoc doc;
    const NphResult r = nph_json_parse(nph_str((const char *)text, size), a, &doc);
    bool ok = r == NPH_RESULT_OK;
    memset(m, 0, sizeof *m);
    if (ok) {
        const NphJsonValue *root = doc.root;
        char pack[512];
        json_string(root, "name", m->name, sizeof m->name);
        json_string(root, "pack", pack, sizeof pack);
        json_string(root, "scene", m->scene_key, sizeof m->scene_key);
        (void)snprintf(m->pack, sizeof m->pack, "%s/%s", scenes, pack);
        m->ev100 = (f32)nph_json_get_f64(root, NPH_STR("ev100"), 12.0);
        m->sky_intensity = (f32)nph_json_get_f64(root, NPH_STR("sky_intensity"), 1.0);
        m->sun_shadows = nph_json_get_bool(root, NPH_STR("sun_shadows"), false);
        const NphJsonValue *views = nph_json_object_get(root, NPH_STR("views"));
        for (u32 i = 0; i < MAX_REF_VIEWS; ++i) {
            const NphJsonValue *v = nph_json_array_at(views, i);
            if (v == NULL) {
                break;
            }
            RefView *rv = &m->views[m->view_count];
            json_string(v, "name", rv->name, sizeof rv->name);
            rv->scene_camera = nph_json_get_bool(v, NPH_STR("scene_camera"), false);
            rv->yfov = (f32)nph_json_get_f64(v, NPH_STR("yfov"), 0.9);
            rv->znear = (f32)nph_json_get_f64(v, NPH_STR("near"), 0.1);
            if (rv->name[0] == '\0' ||
                (!rv->scene_camera &&
                 (!json_vec3(v, "eye", &rv->eye) || !json_vec3(v, "target", &rv->target)))) {
                fprintf(stderr,
                        "render_perf: %s: view %u needs a name, and an eye and a target "
                        "or scene_camera\n",
                        path, i);
                ok = false;
                break;
            }
            m->view_count += 1U;
        }
        ok = ok && m->name[0] != '\0' && pack[0] != '\0' && m->scene_key[0] != '\0' &&
             m->view_count != 0U && m->sky_intensity > 0.0F;
    }
    if (!ok) {
        fprintf(stderr, "render_perf: %s: not a scene manifest (docs/benchmarks.md)\n", path);
    }
    nph_json_doc_free(&doc);
    nph_file_free_all(a, text, size);
    return ok;
}

// The manifests in `dir` whose packs `scenes` holds, and their scenes.
static void load_manifests(const char *dir, const char *scenes)
{
    NphDirIterator it;
    if (nph_dir_open(dir, &it) != NPH_RESULT_OK) {
        fprintf(stderr, "render_perf: no scene manifests in %s\n", dir);
        return;
    }
    NphDirEntry e;
    while (nph_dir_next(&it, &e) && g_manifest_count < MAX_REF_MANIFESTS) {
        const u64 n = strlen(e.name);
        if (e.is_directory || n < 6U || strcmp(e.name + n - 5U, ".json") != 0) {
            continue;
        }
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/%s", dir, e.name);
        RefManifest *m = &g_manifests[g_manifest_count];
        if (!parse_manifest(path, scenes, m)) {
            continue;
        }
        NphFileInfo info;
        if (nph_fs_stat(m->pack, &info) != NPH_RESULT_OK) {
            fprintf(stderr,
                    "render_perf: %s: %s is not there; its scenes are skipped "
                    "(scripts/scenes.py fetch and cook it)\n",
                    m->name, m->pack);
            continue;
        }
        for (u32 v = 0; v < m->view_count; ++v) {
            RefScene *rs = &g_ref_scenes[g_ref_count++];
            const bool debug = g_debug_view != NPH_RENDER_VIEW_LIT;
            (void)snprintf(rs->name, sizeof rs->name, "%s_%s_production%s%s", m->name,
                           m->views[v].name, debug ? "_" : "",
                           debug ? k_debug_views[(u32)g_debug_view - 1U] : "");
            rs->manifest = g_manifest_count;
            rs->view = v;
        }
        g_manifest_count += 1U;
    }
    nph_dir_close(&it);
}

static void unload_reference(Ctx *ctx)
{
    if (ctx->loaded_manifest == NPH_U32_MAX) {
        return;
    }
    cooked_scene_unload(&ctx->cooked);
    NPH_IGNORE_RESULT(nph_vfs_unmount(ctx->vfs, ctx->mount));
    ctx->loaded_manifest = NPH_U32_MAX;
}

static NphResult load_reference(Ctx *ctx, u32 manifest)
{
    if (ctx->loaded_manifest == manifest) {
        return NPH_RESULT_OK;
    }
    unload_reference(ctx);
    NphAllocator *a = nph_allocator_system();
    if (ctx->jobs == NULL) {
        NphJobSystemDesc jd;
        memset(&jd, 0, sizeof jd);
        jd.alloc = a;
        NPH_TRY(nph_job_system_create(&jd, &ctx->jobs));
        NPH_TRY(nph_vfs_create(a, &ctx->vfs));
        NphAssetLoaderDesc ld;
        memset(&ld, 0, sizeof ld);
        ld.alloc = a;
        ld.vfs = ctx->vfs;
        ld.jobs = ctx->jobs;
        NPH_TRY(nph_asset_loader_create(&ld, &ctx->loader));
    }
    if (!ctx->sun_removed) {
        // The scene brings its own sun: the renderer takes the lowest-slot
        // directional light as the sun, which the synthetic one would be.
        nph_render_light_destroy(ctx->renderer, ctx->sun);
        ctx->sun = NPH_RENDER_LIGHT_NULL;
        ctx->sun_removed = true;
    }
    const RefManifest *m = &g_manifests[manifest];
    // The sky in proportion to the scene's own sun (the default one pairs
    // with the synthetic scenes' 30,000 lux).
    NphRenderEnvironmentDesc env;
    memset(&env, 0, sizeof env);
    env.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&env.sky);
    env.intensity = m->sky_intensity;
    NPH_TRY(nph_render_set_environment(ctx->renderer, &env));
    NPH_TRY(nph_vfs_mount_pack(ctx->vfs, NPH_STR("scene"), m->pack, &ctx->mount));
    CookedSceneDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.alloc = a;
    cd.renderer = ctx->renderer;
    cd.loader = ctx->loader;
    cd.jobs = ctx->jobs;
    cd.scene = nph_guid_from_cstr(m->scene_key);
    const u64 t0 = nph_time_now_ns();
    const NphResult r = cooked_scene_load(&cd, &ctx->cooked);
    if (r != NPH_RESULT_OK) {
        NPH_IGNORE_RESULT(nph_vfs_unmount(ctx->vfs, ctx->mount));
        return r;
    }
    ctx->loaded_manifest = manifest;
    if (m->sun_shadows) {
        // glTF has no shadows to cast; the manifest says the sun casts them,
        // as a game's would (the production path: its virtual shadow map).
        for (u32 i = 0; i < ctx->cooked.light_count; ++i) {
            NphRenderLightDesc d = ctx->cooked.light_descs[i];
            if (d.type == NPH_RENDER_LIGHT_TYPE_DIRECTIONAL) {
                d.shadows = 1;
                NPH_TRY(nph_render_light_update(ctx->renderer, ctx->cooked.lights[i], &d));
                break;
            }
        }
    }
    printf("%s: loaded in %.1f s: %u instances, %u lights, %u materials\n", m->name,
           (f64)(nph_time_now_ns() - t0) / 1.0e9, ctx->cooked.instance_count,
           ctx->cooked.light_count, ctx->cooked.render_material_count);
    return NPH_RESULT_OK;
}

// Writes `target` (RGBA8) to `path` as a BMP.
static NphResult capture(Ctx *ctx, NphGpuImage target, u32 width, u32 height, const char *path)
{
    const u64 size = (u64)width * height * 4U;
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = size;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.memory = NPH_GPU_MEMORY_READBACK;
    bd.name = "render_perf capture";
    NphGpuBuffer rb = NPH_GPU_BUFFER_NULL;
    NPH_TRY(nph_gpu_buffer_create(ctx->dev, &bd, &rb));
    NphGpuCmd *cmd = NULL;
    NphResult r = nph_gpu_cmd_begin(ctx->dev, NPH_GPU_QUEUE_GRAPHICS, "capture", &cmd);
    if (r == NPH_RESULT_OK) {
        NphGpuImageBarrier b;
        memset(&b, 0, sizeof b);
        b.image = target;
        b.src_stages = (u32)NPH_GPU_STAGE_ALL;
        b.src_access = (u32)NPH_GPU_ACCESS_MEMORY_WRITE;
        b.old_layout = nph_gpu_image_layout(ctx->dev, target, 0U);
        b.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
        b.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
        b.new_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
        b.mip_count = 1U;
        b.layer_count = NPH_GPU_ALL_LAYERS;
        NphGpuBarrierDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.images = &b;
        desc.image_count = 1U;
        nph_gpu_cmd_barrier(cmd, &desc);
        NphGpuBufferImageCopy region;
        memset(&region, 0, sizeof region);
        region.layer_count = 1U;
        nph_gpu_cmd_copy_image_to_buffer(cmd, target, NPH_GPU_LAYOUT_TRANSFER_SRC, rb, &region, 1U);
        nph_gpu_cmd_buffer_barrier(cmd, rb, (u32)NPH_GPU_STAGE_TRANSFER,
                                   (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, (u32)NPH_GPU_STAGE_HOST,
                                   (u32)NPH_GPU_ACCESS_HOST_READ);
        nph_gpu_cmd_end(cmd);
        NphGpuSubmitDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.queue = NPH_GPU_QUEUE_GRAPHICS;
        sd.cmds = &cmd;
        sd.cmd_count = 1U;
        u64 value = 0U;
        r = nph_gpu_submit(ctx->dev, &sd, &value);
        if (r == NPH_RESULT_OK) {
            r = nph_gpu_timeline_wait(ctx->dev, nph_gpu_queue_timeline(ctx->dev, sd.queue), value,
                                      NPH_U64_MAX);
        }
    }
    if (r == NPH_RESULT_OK) {
        r = capture_write_bmp(path, (const u8 *)nph_gpu_buffer_map(ctx->dev, rb), width, height,
                              false);
    }
    nph_gpu_buffer_destroy(ctx->dev, rb);
    return r;
}

static NphResult run_reference(Ctx *ctx, const RefScene *rs, u32 frames, const char *capture_dir)
{
    NPH_TRY(load_reference(ctx, rs->manifest));
    const RefManifest *m = &g_manifests[rs->manifest];
    const RefView *rv = &m->views[rs->view];
    NphRenderCamera camera;
    if (rv->scene_camera && ctx->cooked.has_camera) {
        camera = ctx->cooked.camera;
    } else {
        if (rv->scene_camera) {
            fprintf(stderr, "render_perf: %s: the scene has no camera\n", rs->name);
            return NPH_RESULT_NOT_FOUND;
        }
        camera =
            nph_render_camera_look_at(rv->eye, rv->target, nph_vec3_unit_y(), rv->yfov, rv->znear);
    }
    NphGpuImageDesc td;
    memset(&td, 0, sizeof td);
    td.type = NPH_GPU_IMAGE_2D;
    td.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    td.width = g_ref_width;
    td.height = g_ref_height;
    td.mip_count = 1U;
    td.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    td.name = "render_perf target";
    NphGpuImage target = NPH_GPU_IMAGE_NULL;
    NPH_TRY(nph_gpu_image_create(ctx->dev, &td, &target));
    // The runtime's defaults, as for the synthetic production scenes.
    NphRenderView view;
    nph_render_view_default(&view, camera);
    view.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    view.ev100 = m->ev100;
    view.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
    view.bloom = RUNTIME_BLOOM;
    view.ao_radius = RUNTIME_AO;
    view.gi = 1;
    view.debug_view = g_debug_view;
    if (ctx->no_sw_raster) {
        view.vis_flags |= (u32)NPH_RENDER_VIS_NO_SW_RASTER;
    }
    static FrameRun run;
    NphResult r = run_frames(ctx, &view, target, frames, &run);
    if (r == NPH_RESULT_OK && capture_dir != NULL) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/%s.bmp", capture_dir, rs->name);
        r = capture(ctx, target, g_ref_width, g_ref_height, path);
    }
    NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(ctx->dev));
    nph_gpu_image_destroy(ctx->dev, target);
    NPH_TRY(r);
    return report(ctx, rs->name, g_ref_width, g_ref_height, NPH_RENDER_PIPELINE_PRODUCTION, false,
                  &run);
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
    // A reference scene unloads between frames in one go: Bistro destroys
    // some 8,000 GPU objects.
    dd.deferred_destroy_capacity = 16384U;
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
    if (ctx->renderer != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(ctx->dev));
        unload_reference(ctx);
    }
    nph_asset_loader_destroy(ctx->loader);
    nph_vfs_destroy(ctx->vfs);
    nph_job_system_destroy(ctx->jobs);
    if (ctx->instances != NULL) {
        destroy_scene(ctx);
        NPH_FREE_ARRAY(nph_allocator_system(), NphRenderInstance, ctx->instances, MAX_INSTANCES);
    }
    if (ctx->renderer != NULL) {
        if (!ctx->sun_removed) {
            nph_render_light_destroy(ctx->renderer, ctx->sun);
        }
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
                    "[--budgets=FILE] [--list] [--validation] [--sw-pixels=N]\n"
                    "                   [--scenes=DIR] [--capture-dir=DIR] [--manifests=DIR] "
                    "[--ref-size=WxH] [--debug-view=NAME] [--reference-only]\n");
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
    const char *scenes = getenv("NPH_SCENES");
    const char *capture_dir = NULL;
    const char *manifest_dir = NPH_SCENE_MANIFEST_DIR;
    bool reference_only = false;
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
        } else if (strncmp(argv[i], "--scenes=", 9U) == 0) {
            scenes = argv[i] + 9;
        } else if (strncmp(argv[i], "--capture-dir=", 14U) == 0) {
            capture_dir = argv[i] + 14;
        } else if (strncmp(argv[i], "--manifests=", 12U) == 0) {
            manifest_dir = argv[i] + 12;
        } else if (strncmp(argv[i], "--ref-size=", 11U) == 0) {
            char *x = NULL;
            const unsigned long w = strtoul(argv[i] + 11, &x, 10);
            const unsigned long h = x != NULL && *x == 'x' ? strtoul(x + 1, NULL, 10) : 0UL;
            if (w == 0UL || h == 0UL || w > 16384UL || h > 16384UL) {
                return usage();
            }
            g_ref_width = (u32)w;
            g_ref_height = (u32)h;
        } else if (strcmp(argv[i], "--reference-only") == 0) {
            reference_only = true;
        } else if (strncmp(argv[i], "--debug-view=", 13U) == 0) {
            g_debug_view = NPH_RENDER_VIEW_COUNT;
            for (u32 v = 0; v < NPH_ARRAY_COUNT(k_debug_views); ++v) {
                if (strcmp(argv[i] + 13, k_debug_views[v]) == 0) {
                    g_debug_view = (NphRenderDebugView)(v + 1U);
                }
            }
            if (g_debug_view == NPH_RENDER_VIEW_COUNT) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    if (scenes != NULL && scenes[0] != '\0') {
        load_manifests(manifest_dir, scenes);
    }
    frames = nph_clamp_u32(frames, 1U, MAX_FRAMES - NPH_GPU_FRAMES_IN_FLIGHT);
    u32 matched = 0U;
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_scenes) && !reference_only; ++i) {
        if (glob_match(filter, g_scenes[i].name)) {
            ++matched;
            if (list) {
                printf("%s\n", g_scenes[i].name);
            }
        }
    }
    for (u32 i = 0; i < g_ref_count; ++i) {
        if (glob_match(filter, g_ref_scenes[i].name)) {
            ++matched;
            if (list) {
                printf("%s\n", g_ref_scenes[i].name);
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
    ctx.loaded_manifest = NPH_U32_MAX;
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
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_scenes) && status == 0 && !reference_only; ++i) {
        if (!glob_match(filter, g_scenes[i].name)) {
            continue;
        }
        const NphResult r = run_scene(&ctx, &g_scenes[i], frames);
        if (r != NPH_RESULT_OK) {
            fprintf(stderr, "render_perf: %s failed: %s\n", g_scenes[i].name, nph_result_string(r));
            status = 1;
        }
    }
    for (u32 i = 0; i < g_ref_count && status == 0; ++i) {
        if (!glob_match(filter, g_ref_scenes[i].name)) {
            continue;
        }
        const NphResult r = run_reference(&ctx, &g_ref_scenes[i], frames, capture_dir);
        if (r != NPH_RESULT_OK) {
            fprintf(stderr, "render_perf: %s failed: %s\n", g_ref_scenes[i].name,
                    nph_result_string(r));
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
