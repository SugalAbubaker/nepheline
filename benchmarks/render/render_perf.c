// SPDX-License-Identifier: Apache-2.0
// render_perf: the renderer on a device, scene by scene (CLAUDE.md §10.7:
// "every renderer PR must state its cost in both milliseconds and DRAM
// bytes"; §12 Phase 10: "constant CPU cost vs object count").
//
// Each scene renders frames of its view held still until the GPU's clock
// has settled (its boost from the idle clock of the scene's load took up to
// four seconds), warm-up frames (instance uploads, the environment's
// image-based-lighting passes, the scene's motion), then measured frames,
// and reports:
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
//                       compute overlaps it;
//   <scene>.gpu.<pass>.p99, <scene>.gpu.span.p99
//                       their 99th percentiles. The passes' do not add up
//                       to the frame's: a pass's slowest frames are not
//                       every pass's.
//
//     render_perf [--filter=GLOB] [--frames=N] [--json=FILE] [--budgets=FILE]
//                 [--list] [--validation] [--sw-pixels=N] [--scenes=DIR]
//                 [--capture-dir=DIR] [--manifests=DIR] [--ref-size=WxH]
//                 [--debug-view=NAME] [--reference-only] [--lod-error=PX] [--lists=N]
//                 [--vsm-trace] [--memory-trace] [--no-cells] [--stream[=MB]]
//                 [--stream-trace] [--stream-loads=N] [--voxel-pool=MB] [--tiles=WxH]
//                 [--paged-textures] [--texture-budget=MB]
//
// --sw-pixels sets NphRendererDesc.sw_raster_pixels, the average triangle
// size the software raster takes (0: every cluster in hardware), for tuning
// the hybrid raster's split. --lists=N (a power of two up to 16) makes
// every list of the production path and the shadow map that can drop work
// N times its default, for what a scene that overflows them needs (its
// results then do not compare with the budgets). --lod-error sets the
// reference scenes' NphRenderView.lod_error (0: every mesh at level 0), for
// comparing a scene with its levels of detail and without. --vsm-trace
// prints the shadow map's statistics every frame, warm-up included (they
// report the frame NPH_GPU_FRAMES_IN_FLIGHT before): how its page budget
// draws a burst of pages. --no-cells loads the reference scenes without
// their world partition's cells (ADR-0062): every instance drawn as itself,
// no proxy, for comparing a scene with its cells and without. --stream
// streams the cells' proxies cooked as their levels apart (ADR-0063 stage 3;
// runtime/proxy_stream.h): each cell loaded with its coarsest level alone,
// its finer levels as the views want them, within MB megabytes past the
// coarsest (no limit without); before a view is measured the stream is let
// settle (frames until nothing loads, is given or is dropped, at most
// PRIME_MAX_NS), and each view's report then gives what the stream holds
// and did, and its main-thread time a frame; --stream-trace prints the
// stream's work every frame, and --stream-loads sets its loads at once
// (NphProxyStreamDesc.max_loads; 0: the stream's 64). --voxel-pool sets
// the renderer's voxel pool (NphRendererDesc.voxel_pool_bytes; 0 none, the
// default without --stream, 1536 with it), where meshes of voxels alone
// live a range each and a streamed level is made on the loader's jobs.
// --tiles lays each scene out W by H times side by side, its first tile
// where it was and the others around it (cooked_scene.h
// CookedSceneDesc.tiles_x), each tile's instances and cells its own: an
// open world of the game's size from one reference scene (ADR-0063 stage
// 4). After each scene, a line reports the GPU memory the process holds
// (nph_gpu_device_memory_stats): device-local in use against the OS's
// budget for it; --memory-trace reports it every 16th frame, warm-up
// included, where bottom levels are built. --paged-textures pages the
// scenes' textures (ADR-0065; CookedSceneDesc.paged_textures) and lets every
// page be installed before a view is measured: what the clamped samples and
// the requests cost where every page is resident.
//
// Reference scenes (§10.7: the targets are measured on them): with
// --scenes=DIR, or the NPH_SCENES environment variable, every manifest in
// benchmarks/scenes whose cooked pack DIR holds (scripts/scenes.py fetches
// and cooks them) adds a scene per view, `<scene>_<view>_production`, run
// after the synthetic scenes at 3840 x 2160 through the production path
// with the runtime's defaults, lit by the scene's own lights (the
// synthetic scenes' sun is removed first) and seen from its own camera or
// the manifest's, still or moving as the view says (walking along it, level,
// turning about the world's up axis, or both; one step a measured frame as
// at 240 frames a second, warm-up included, so the caches meet the motion
// in their steady state). --capture-dir writes each one's last frame there as a
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
//   lod_4k_production     a rolling terrain 400 m across (131,072
//                         triangles) and 1,024 rocks on it (65,024 triangles
//                         each: 67 million in all), both with levels of
//                         detail (docs/render.md "Levels of detail"), seen
//                         from head height across the diagonal: each part
//                         drawn at the level a pixel's error allows.
//   lod_4k_level0         the same at level 0 (NphRenderView.lod_error 0):
//                         what the levels save.
//   lod_shadows_walk_4k_production
//                         lod_4k_production with the sun's virtual shadow
//                         map and the camera walking (1.4 m/s at 240 frames
//                         a second): the pages it comes to need are drawn,
//                         each caster at the map's cut for the page's level.
//   lod_shadows_sun_4k_production
//                         lod_4k_production with the sun moving across the
//                         sky every frame, at a day-night cycle's pace (a
//                         24-minute day): every page of the map is drawn
//                         again each frame, each caster at its cut.
//   foliage_4k_production the terrain with 1,024 shrubs on it (4,000
//                         six-sided needles each, 80,000 triangles: 82
//                         million in all) instead of rocks, their levels'
//                         stalled groups voxels (docs/render.md "Voxels";
//                         ADR-0061), from head height across the diagonal.
//   foliage_4k_triangles  the same shrubs with levels of detail but no
//                         voxels: the stalled triangle levels they drew
//                         before.
//   foliage_4k_level0     the same at level 0.
//   foliage_shadows_walk_4k_production, foliage_shadows_walk_4k_triangles
//                         foliage_4k_production and foliage_4k_triangles
//                         with the sun's virtual shadow map and the camera
//                         walking: the shrubs cast at the map's cuts, their
//                         voxel clusters through the voxel casters' raster
//                         (ADR-0061, stage 4), or their stalled triangles.
//   foliage_shadows_sun_4k_production, foliage_shadows_sun_4k_triangles
//                         the same with the sun moving every frame: every
//                         page drawn again each frame.
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
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/render/cluster_lod.h>
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
#define MAX_RESULTS    4096U // the synthetic scenes write some 2,300
#define MAX_PASS_STATS 96U
#define WARMUP_FRAMES  24U
#define WARMUP_NS      (500ULL * 1000000ULL) // and at least this long
// Before them, frames of the view held still until the GPU's clock has
// settled: a GPU idle through a scene's load starts it at an idle clock,
// and its boost back to full clock took from under half a second to four
// seconds on the RTX 3060 Ti (driver 621.01, 2026-10-21), where frames
// measured on the way read 40% slow. It has settled when the mean GPU frame
// time of CLOCK_STEADY windows in a row has fallen by no more than
// CLOCK_SETTLED of the window's before each (the boost pauses on the way,
// for a window at a time), at most CLOCK_MAX_NS.
#define CLOCK_WINDOW  64U
#define CLOCK_STEADY  3U
#define CLOCK_SETTLED 0.01
#define CLOCK_MAX_NS  (10000ULL * 1000000ULL)
#define MAX_INSTANCES (102400U + 16384U + 256U + 1U)
// Meshes: a scene's shared meshes and its cells' proxies, each a mesh a level
// where its levels are apart (ADR-0063; the landscape's 384 proxies make
// about 2,400), beside the synthetic scenes'.
#define MAX_MESHES 16384U
#define MAX_LIGHTS 4000U
// The many lights' scenes': their near range, and how many at most.
#define MANY_NEAR_RANGE 0.5F
#define MAX_MANY_LIGHTS 10000U
// Per tile past the first (--tiles), the limits grow by a tile's instances,
// its cells' levels and its cells, at most (the landscape's 47,863
// instances, about 2,400 levels and 384 cells, rounded up): the renderer's
// per-instance buffers are those of the world tiled, not of many more.
#define TILE_INSTANCES 49152U
#define TILE_MESHES    4096U
#define TILE_CELLS     512U

typedef enum SceneKind {
    SCENE_GRID = 0,
    SCENE_FIELD,
    SCENE_LOD,     // the terrain and its rocks, with levels of detail
    SCENE_FOLIAGE, // the terrain and shrubs of needles, with voxel levels
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
    SHADOWS_SUN,         // the sun (or the moon) casts shadows
    SHADOWS_SUN_MOVING,  // and an instance moves every frame
    SHADOWS_SUN_WALK,    // and the camera walks
    SHADOWS_SUN_TURNING, // and the sun moves every frame (every page drawn again)
    SHADOWS_ALL,         // the moon and every lamp cast shadows
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
    POST_DOF = 1U << 6U,          // 5 m away
    POST_DOF_SLIGHT = 1U << 7U,   // 2.6 m
    POST_DOF_LARGE = 1U << 8U,    // 0.6 m
    POST_NO_AO = 1U << 9U,        // no ambient occlusion
    POST_NO_GI = 1U << 10U,       // no global illumination
    POST_LOD_LEVEL0 = 1U << 11U,  // every mesh at level 0 (NphRenderView.lod_error 0)
    POST_NO_VOXELS = 1U << 12U,   // the foliage's levels without voxels
    POST_MANY_LIGHTS = 1U << 13U, // its lights are many lights (NphRenderManyLight; ADR-0073)
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
    // Ten times as many lights in the same place cost the near fields ten times as many
    // lights a pixel, and the cache's update nothing (ADR-0073).
    {"many_lights_1k_production", SCENE_FIELD, 16384U, 1000U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_MANY_LIGHTS},
    {"many_lights_10k_production", SCENE_FIELD, 16384U, MAX_MANY_LIGHTS,
     NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U, SHADOWS_NONE, POST_MANY_LIGHTS},
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
    {"lod_4k_production", SCENE_LOD, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_NONE, POST_DEFAULT},
    {"lod_4k_level0", SCENE_LOD, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U, 0U,
     SHADOWS_NONE, POST_LOD_LEVEL0},
    {"lod_shadows_walk_4k_production", SCENE_LOD, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_SUN_WALK, POST_DEFAULT},
    {"lod_shadows_sun_4k_production", SCENE_LOD, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_SUN_TURNING, POST_DEFAULT},
    {"foliage_4k_production", SCENE_FOLIAGE, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U,
     2160U, 0U, SHADOWS_NONE, POST_DEFAULT},
    {"foliage_4k_triangles", SCENE_FOLIAGE, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U,
     0U, SHADOWS_NONE, POST_NO_VOXELS},
    {"foliage_4k_level0", SCENE_FOLIAGE, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION, 3840U, 2160U,
     0U, SHADOWS_NONE, POST_LOD_LEVEL0},
    {"foliage_shadows_walk_4k_production", SCENE_FOLIAGE, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION,
     3840U, 2160U, 0U, SHADOWS_SUN_WALK, POST_DEFAULT},
    {"foliage_shadows_walk_4k_triangles", SCENE_FOLIAGE, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION,
     3840U, 2160U, 0U, SHADOWS_SUN_WALK, POST_NO_VOXELS},
    {"foliage_shadows_sun_4k_production", SCENE_FOLIAGE, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION,
     3840U, 2160U, 0U, SHADOWS_SUN_TURNING, POST_DEFAULT},
    {"foliage_shadows_sun_4k_triangles", SCENE_FOLIAGE, 1024U, 0U, NPH_RENDER_PIPELINE_PRODUCTION,
     3840U, 2160U, 0U, SHADOWS_SUN_TURNING, POST_NO_VOXELS},
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
    f32 yaw;  // degrees the camera is turned about the world's up axis first
    f32 walk; // metres a second along the view, level (0: still)
    f32 turn; // degrees a second about the world's up axis, one way (0: none)
} RefView;

typedef struct RefManifest {
    char name[32];
    char pack[1024];     // the cooked pack, under the scenes directory
    char scene_key[256]; // its scene's identity (docs/cook.md "Identity")
    f32 ev100;
    f32 sky_intensity; // the default sky's (daylight next to a 30,000 lux sun) scale
    bool sun_shadows;  // the scene's first directional light casts shadows
    // Its point lights as many lights (ADR-0073), this near range, room for so many.
    f32 light_near_range;
    u32 many_lights;
    // `lists`: what its views need of every list that can drop work (--lists'
    // multiple). A scene that says so fails where they overflow: dropped work
    // flatters a frame, so it would pass a gate it does not hold. 0: unsaid.
    u32 lists;
    // Its world (`tiles`, docs/benchmarks.md): count tiles side by side, unless
    // --tiles says otherwise, and what a tile asks of the renderer's sizes.
    u32 tiles_x;
    u32 tiles_z;
    NphDVec3 tile_pitch; // 0: the first tile's instances' extent
    u32 tile_instances;
    u32 tile_meshes;
    u32 tile_cells;
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
static f32 g_lod_error = -1.0F;     // --lod-error (negative: the view's default)
static u32 g_lists = 1U;            // --lists: the production path's list capacities, times this
static bool g_vsm_trace = false;    // --vsm-trace: every frame's shadow map statistics
static bool g_memory_trace = false; // --memory-trace: the GPU memory every 16th frame
static bool g_no_cells = false;     // --no-cells: the scenes' cells not loaded
static bool g_stream = false;       // --stream: the cells' proxies streamed
static u64 g_stream_budget = 0U;    // --stream=MB: their budget past the coarsest (0: none)
static bool g_stream_trace = false; // --stream-trace: the stream's work every frame
// --paged-textures: textures paged (ADR-0065), every page installed before a
// view is measured: what the clamped samples and the requests cost.
// --texture-budget=MB: paged, their levels streamed within the budget
// (runtime/texture_stream.h) until the stream settles.
static bool g_paged_textures = false;
static u64 g_texture_budget = 0U;
static i64 g_voxel_pool_mb = -1; // --voxel-pool=MB (-1: 1536 with --stream, else 0)
static u32 g_stream_loads = 0U;  // --stream-loads=N: levels loading at once (0: the stream's)
static u32 g_tiles_x = 0U;       // --tiles=WxH: the scenes laid out W by H (ADR-0063 stage 4;
static u32 g_tiles_z = 0U;       // 0: as each manifest says)

// The most a view waits for the stream to settle before it is measured.
#define PRIME_MAX_NS (60ULL * 1000000000ULL)
static u32 g_trace_frame = 0U;

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
    // The levels-of-detail scene's meshes, made the first time it runs.
    NphRenderMesh terrain;
    NphRenderMesh rock;
    NphRenderMesh shrub;           // with voxel levels
    NphRenderMesh shrub_triangles; // without
    NphRenderMaterial materials[4];
    NphRenderLight sun;
    NphRenderLightDesc sun_desc;
    NphRenderLight lights[MAX_LIGHTS];
    u32 light_count;
    NphRenderManyLight many_lights[MAX_MANY_LIGHTS];
    u32 many_light_count;
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
    // The sun moving across the sky (SHADOWS_SUN_TURNING), and the frame
    // count.
    bool sun_turning;
    u32 sun_frame;
    // The camera turning or strafing for motion blur (POST_MOTION_*), or a
    // reference view turning: radians and metres per frame, the frame count,
    // and whether the motions reverse (the synthetic ones, to stay over the
    // field).
    f32 turn;
    f64 strafe;
    u32 motion_frame;
    bool motion_reverses;
    // Reference scenes: what loads them, made for the first; the manifest
    // whose scene is loaded (NPH_U32_MAX: none), and whether the synthetic
    // scenes' sun was removed for its lights.
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;
    u32 mount;
    CookedScene cooked;
    // The stream's main-thread time a frame (--stream), over a view's
    // measured frames, and the reference view's height it is told.
    f64 stream_ns_sum;
    f64 stream_ns_max;
    u32 stream_frames;
    u32 view_height;
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

// ---------------------------------------------------------------------------
// The levels-of-detail scene's meshes
// ---------------------------------------------------------------------------

#define TERRAIN_CELLS 256U   // quads a side: 131,072 triangles
#define TERRAIN_HALF  200.0F // metres from the centre to a side
#define ROCK_SEGMENTS 256U   // a UV sphere's: 65,024 triangles
#define ROCK_RINGS    128U

// Gentle hills (a field's, not a valley's walls), so that the rocks are
// seen far across it rather than hidden behind the nearest rise.
static f32 terrain_height(f32 x, f32 z)
{
    return 1.5F * nph_f32_sin(x / 37.0F) * nph_f32_cos(z / 41.0F) +
           0.6F * nph_f32_sin(x / 11.0F + z / 13.0F) + 0.25F * nph_f32_sin(x / 3.1F - z / 2.7F);
}

// A rock's radius along a unit direction: a sphere's, bumped by three
// octaves of sines.
static f32 rock_radius(NphVec3 d)
{
    f32 r = 0.5F;
    f32 amp = 0.09F;
    f32 freq = 2.3F;
    for (u32 o = 0; o < 3U; ++o) {
        r += amp * nph_f32_sin(freq * d.x + 1.7F * (f32)o) * nph_f32_sin(freq * d.y + 0.6F) *
             nph_f32_cos(freq * d.z - 1.1F * (f32)o);
        amp *= 0.45F;
        freq *= 2.6F;
    }
    return r;
}

// Area-weighted vertex normals of a triangle list.
static void smooth_normals(NphRenderVertexInput *v, u32 n, const u32 *idx, u32 k)
{
    for (u32 i = 0; i < n; ++i) {
        v[i].normal = nph_vec3_zero();
    }
    for (u32 t = 0; t < k; t += 3U) {
        const NphVec3 a = v[idx[t]].position;
        const NphVec3 e1 = nph_vec3_sub(v[idx[t + 1U]].position, a);
        const NphVec3 e2 = nph_vec3_sub(v[idx[t + 2U]].position, a);
        const NphVec3 face = nph_vec3_cross(e1, e2);
        for (u32 c = 0; c < 3U; ++c) {
            v[idx[t + c]].normal = nph_vec3_add(v[idx[t + c]].normal, face);
        }
    }
    for (u32 i = 0; i < n; ++i) {
        v[i].normal = nph_vec3_normalize_or(v[i].normal, nph_vec3_unit_y());
    }
}

// The shrubs' material as the voxelizer reads it (render/voxel.h): opaque,
// a needle's green, the vertices' interpolated normals.
typedef struct NeedleLook {
    const NphRenderVertexAttrib *attribs;
    u8 surface[1];
} NeedleLook;

static void needle_sample(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint,
                          NphRenderVoxelSample *out)
{
    (void)submesh;
    (void)footprint;
    const NphRenderVertexAttrib *a = ((const NeedleLook *)user)->attribs;
    const NphVec3 n =
        nph_vec3_add(nph_vec3_add(nph_vec3_scale(nph_oct_decode_snorm16(a[v[0]].normal), b.x),
                                  nph_vec3_scale(nph_oct_decode_snorm16(a[v[1]].normal), b.y)),
                     nph_vec3_scale(nph_oct_decode_snorm16(a[v[2]].normal), b.z));
    out->base_color = nph_vec3(0.12F, 0.22F, 0.07F);
    out->normal = nph_vec3_normalize_or(n, nph_vec3_unit_y());
    out->roughness = 0.8F;
    out->metallic = 0.0F;
    out->occlusion = 1.0F;
}

static bool needle_covers(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint)
{
    (void)user;
    (void)submesh;
    (void)v;
    (void)b;
    (void)footprint;
    return true;
}

// Uploads a mesh with its meshlets and levels of detail, built here as the
// cooker builds them: with voxel levels where `voxels`.
static NphResult upload_lod_mesh(Ctx *ctx, const NphRenderVertexInput *v, u32 n, const u32 *idx,
                                 u32 k, const char *name, bool voxels, NphRenderMesh *out)
{
    NphAllocator *a = nph_allocator_system();
    NphRenderMeshData data;
    NPH_TRY(nph_render_mesh_data_build(a, v, n, idx, k, &data));
    NphVec3 *positions = NPH_ALLOC_ARRAY(a, NphVec3, data.vertex_count);
    NphRenderMeshlets meshlets;
    nph_render_meshlets_init(&meshlets, a);
    NphRenderClusterLods lods;
    nph_render_cluster_lods_init(&lods, a);
    NphResult r = positions != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    const NphRenderIndexRange range = {0U, data.index_count};
    if (r == NPH_RESULT_OK) {
        const f32 q = 1.0F / 65535.0F;
        for (u32 i = 0; i < data.vertex_count; ++i) {
            const NphRenderVertexPosition p = data.positions[i];
            positions[i] = nph_vec3(data.position_offset.x + (f32)p.x * q * data.position_scale.x,
                                    data.position_offset.y + (f32)p.y * q * data.position_scale.y,
                                    data.position_offset.z + (f32)p.z * q * data.position_scale.z);
        }
        NphRenderMeshletInput in;
        memset(&in, 0, sizeof in);
        in.positions = positions;
        in.vertex_count = data.vertex_count;
        in.indices = data.indices;
        in.index_count = data.index_count;
        in.ranges = &range;
        in.range_count = 1U;
        in.cone_weight = 0.25F;
        in.scratch = a;
        r = nph_render_meshlets_build(&in, &meshlets);
    }
    if (r == NPH_RESULT_OK) {
        NphRenderClusterLodInput li;
        memset(&li, 0, sizeof li);
        li.positions = positions;
        li.attribs = data.attribs;
        li.attrib_stride = (u32)sizeof(NphRenderVertexAttrib);
        li.vertex_count = data.vertex_count;
        li.indices = data.indices;
        li.index_count = data.index_count;
        li.ranges = &range;
        li.range_count = 1U;
        li.meshlets = &meshlets;
        li.cone_weight = 0.25F;
        li.scratch = a;
        NeedleLook look;
        look.attribs = data.attribs;
        look.surface[0] = (u8)NPH_RENDER_VOXEL_SURFACE_OPAQUE;
        NphRenderVoxelSampler sampler;
        memset(&sampler, 0, sizeof sampler);
        sampler.user = &look;
        sampler.submesh_surface = look.surface;
        sampler.sample = needle_sample;
        sampler.covers = needle_covers;
        li.voxels = voxels ? &sampler : NULL;
        const u64 t0 = nph_time_now_ns();
        r = nph_render_cluster_lods_build(&li, &lods);
        fprintf(stderr,
                "render_perf: %s: %u triangles, %u clusters in %u levels (%u voxels in %u "
                "bricks; %.2f s)\n",
                name, data.index_count / 3U, lods.clusters.meshlets.len, lods.levels,
                lods.voxels.len, lods.bricks.len, (f64)(nph_time_now_ns() - t0) / 1.0e9);
    }
    NphRenderPackedVoxels packed;
    nph_render_packed_voxels_init(&packed, a);
    if (r == NPH_RESULT_OK && lods.bricks.len != 0U) {
        // The voxels compact, as the cooker stores them (render/voxel.h).
        NphRenderMeshlets *c = &lods.clusters;
        r = nph_render_voxels_pack(c->meshlets.data, c->meshlets.len, lods.bricks.data,
                                   lods.bricks.len, lods.voxels.data, lods.voxels.len, &packed);
    }
    if (r == NPH_RESULT_OK) {
        NphRenderSubmesh sub;
        NphRenderMeshDesc md;
        nph_render_mesh_data_desc(&data, name, &sub, &md);
        const NphRenderMeshlets *c = &lods.clusters;
        md.meshlets = c->meshlets.data;
        md.meshlet_bounds = c->bounds.data;
        md.meshlet_count = c->meshlets.len;
        md.meshlet_vertices = c->vertices.data;
        md.meshlet_vertex_count = c->vertices.len;
        md.meshlet_triangles = c->triangles.data;
        md.meshlet_triangle_count = c->triangles.len / 3U;
        md.cluster_lods = lods.lods.data;
        md.lod_groups = lods.groups.data;
        md.lod_group_count = lods.groups.len;
        md.lod_group_clusters = lods.group_clusters.data;
        md.lod_group_cluster_count = lods.group_clusters.len;
        md.lod_nodes = lods.nodes.data;
        md.lod_node_count = lods.nodes.len;
        md.lod_level0_meshlets = lods.level0_clusters;
        if (lods.bricks.len != 0U) {
            md.voxel_clusters = packed.clusters.data;
            md.voxel_cluster_count = packed.clusters.len;
            md.voxel_bricks = packed.bricks.data;
            md.voxel_brick_count = packed.bricks.len;
            md.voxel_words = packed.words.data;
            md.voxel_word_count = packed.words.len;
        }
        r = nph_render_mesh_create(ctx->renderer, &md, out);
    }
    nph_render_packed_voxels_free(&packed);
    nph_render_cluster_lods_free(&lods);
    nph_render_meshlets_free(&meshlets);
    if (positions != NULL) {
        NPH_FREE_ARRAY(a, NphVec3, positions, data.vertex_count);
    }
    nph_render_mesh_data_free(&data);
    return r;
}

static NphResult make_terrain(Ctx *ctx)
{
    NphAllocator *a = nph_allocator_system();
    const u32 side = TERRAIN_CELLS + 1U;
    const u32 n = side * side;
    const u32 k = TERRAIN_CELLS * TERRAIN_CELLS * 6U;
    NphRenderVertexInput *v = NPH_ALLOC_ARRAY(a, NphRenderVertexInput, n);
    u32 *idx = NPH_ALLOC_ARRAY(a, u32, k);
    NphResult r = v != NULL && idx != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        const f32 step = 2.0F * TERRAIN_HALF / (f32)TERRAIN_CELLS;
        for (u32 i = 0; i < side; ++i) {
            for (u32 j = 0; j < side; ++j) {
                const f32 x = -TERRAIN_HALF + step * (f32)j;
                const f32 z = -TERRAIN_HALF + step * (f32)i;
                NphRenderVertexInput *p = &v[i * side + j];
                p->position = nph_vec3(x, terrain_height(x, z), z);
                p->uv = nph_vec2((f32)j / 8.0F, (f32)i / 8.0F);
                p->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
            }
        }
        u32 t = 0U;
        for (u32 i = 0; i < TERRAIN_CELLS; ++i) {
            for (u32 j = 0; j < TERRAIN_CELLS; ++j) {
                const u32 v00 = i * side + j;
                const u32 v10 = v00 + side;
                // Counter-clockwise from above.
                idx[t++] = v00;
                idx[t++] = v10;
                idx[t++] = v10 + 1U;
                idx[t++] = v00;
                idx[t++] = v10 + 1U;
                idx[t++] = v00 + 1U;
            }
        }
        smooth_normals(v, n, idx, k);
        r = upload_lod_mesh(ctx, v, n, idx, k, "lod terrain", false, &ctx->terrain);
    }
    if (v != NULL) {
        NPH_FREE_ARRAY(a, NphRenderVertexInput, v, n);
    }
    if (idx != NULL) {
        NPH_FREE_ARRAY(a, u32, idx, k);
    }
    return r;
}

static NphResult make_rock(Ctx *ctx)
{
    NphAllocator *a = nph_allocator_system();
    // (segments + 1) x (rings + 1) vertices: the last column at the first's
    // positions with u = 1, the poles a row each (seams the levels keep).
    const u32 cols = ROCK_SEGMENTS + 1U;
    const u32 n = cols * (ROCK_RINGS + 1U);
    const u32 k = ROCK_SEGMENTS * (ROCK_RINGS - 1U) * 6U;
    NphRenderVertexInput *v = NPH_ALLOC_ARRAY(a, NphRenderVertexInput, n);
    u32 *idx = NPH_ALLOC_ARRAY(a, u32, k);
    NphResult r = v != NULL && idx != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 i = 0; i <= ROCK_RINGS; ++i) {
            const f32 theta = NPH_PI * (f32)i / (f32)ROCK_RINGS;
            for (u32 j = 0; j <= ROCK_SEGMENTS; ++j) {
                const f32 phi = 2.0F * NPH_PI * (f32)(j % ROCK_SEGMENTS) / (f32)ROCK_SEGMENTS;
                const NphVec3 d =
                    nph_vec3(nph_f32_sin(theta) * nph_f32_cos(phi), nph_f32_cos(theta),
                             nph_f32_sin(theta) * nph_f32_sin(phi));
                NphRenderVertexInput *p = &v[i * cols + j];
                p->position = nph_vec3_scale(d, rock_radius(d));
                p->uv = nph_vec2((f32)j / (f32)ROCK_SEGMENTS, (f32)i / (f32)ROCK_RINGS);
                p->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
            }
        }
        u32 t = 0U;
        for (u32 i = 0; i < ROCK_RINGS; ++i) {
            for (u32 j = 0; j < ROCK_SEGMENTS; ++j) {
                const u32 v00 = i * cols + j;
                const u32 v10 = v00 + cols;
                // Counter-clockwise from outside; one triangle at each pole.
                if (i != 0U) {
                    idx[t++] = v00;
                    idx[t++] = v00 + 1U;
                    idx[t++] = v10;
                }
                if (i != ROCK_RINGS - 1U) {
                    idx[t++] = v00 + 1U;
                    idx[t++] = v10 + 1U;
                    idx[t++] = v10;
                }
            }
        }
        smooth_normals(v, n, idx, t);
        r = upload_lod_mesh(ctx, v, n, idx, t, "lod rock", false, &ctx->rock);
    }
    if (v != NULL) {
        NPH_FREE_ARRAY(a, NphRenderVertexInput, v, n);
    }
    if (idx != NULL) {
        NPH_FREE_ARRAY(a, u32, idx, k);
    }
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

// A shrub: SHRUB_NEEDLES closed needles, each a smooth-shaded tube of six
// sides 0.4 m long and 16 mm across, in a 3 m box (from its base at the
// origin): its levels stall on them.
#define SHRUB_NEEDLES 4000U
#define SHRUB_SIDES   6U

static NphResult make_shrub(Ctx *ctx, bool voxels, NphRenderMesh *out)
{
    NphAllocator *a = nph_allocator_system();
    const u32 per = 2U * SHRUB_SIDES;
    const u32 n = SHRUB_NEEDLES * per;
    const u32 k = SHRUB_NEEDLES * (2U * SHRUB_SIDES + 2U * (SHRUB_SIDES - 2U)) * 3U;
    NphRenderVertexInput *v = NPH_ALLOC_ARRAY(a, NphRenderVertexInput, n);
    u32 *idx = NPH_ALLOC_ARRAY(a, u32, k);
    NphResult r = v != NULL && idx != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        u32 t = 0U;
        for (u32 i = 0; i < SHRUB_NEEDLES; ++i) {
            const u32 h0 = hash_u32(i * 3U + 1U);
            const u32 h1 = hash_u32(i * 3U + 2U);
            const u32 h2 = hash_u32(i * 3U + 3U);
            const NphVec3 c = nph_vec3(3.0F * ((f32)(h0 & 0xFFFFU) / 65535.0F - 0.5F),
                                       3.0F * (f32)(h0 >> 16U) / 65535.0F,
                                       3.0F * ((f32)(h1 & 0xFFFFU) / 65535.0F - 0.5F));
            const f32 z = 2.0F * (f32)(h1 >> 16U) / 65535.0F - 1.0F;
            const f32 phi = 2.0F * NPH_PI * (f32)(h2 & 0xFFFFU) / 65535.0F;
            const f32 rr = nph_f32_sqrt(nph_f32_max(1.0F - z * z, 0.0F));
            const NphVec3 d = nph_vec3(rr * nph_f32_cos(phi), rr * nph_f32_sin(phi), z);
            const NphVec3 up = nph_f32_abs(d.z) < 0.9F ? nph_vec3_unit_z() : nph_vec3_unit_x();
            const NphVec3 tt = nph_vec3_normalize(nph_vec3_cross(up, d));
            const NphVec3 bb = nph_vec3_cross(d, tt);
            const u32 base = i * per;
            for (u32 ring = 0; ring < 2U; ++ring) {
                for (u32 j = 0; j < SHRUB_SIDES; ++j) {
                    const f32 angle = 2.0F * NPH_PI * (f32)j / (f32)SHRUB_SIDES;
                    const NphVec3 off = nph_vec3_add(nph_vec3_scale(tt, nph_f32_cos(angle)),
                                                     nph_vec3_scale(bb, nph_f32_sin(angle)));
                    const NphVec3 end = nph_vec3_scale(d, ring == 0U ? -0.2F : 0.2F);
                    NphRenderVertexInput *p = &v[base + ring * SHRUB_SIDES + j];
                    p->position = nph_vec3_add(nph_vec3_add(c, end), nph_vec3_scale(off, 0.008F));
                    p->normal = off;
                    p->uv = nph_vec2(0.0F, 0.0F);
                    p->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
                }
            }
            for (u32 j = 0; j < SHRUB_SIDES; ++j) {
                const u32 j1 = (j + 1U) % SHRUB_SIDES;
                idx[t++] = base + j;
                idx[t++] = base + j1;
                idx[t++] = base + SHRUB_SIDES + j1;
                idx[t++] = base + j;
                idx[t++] = base + SHRUB_SIDES + j1;
                idx[t++] = base + SHRUB_SIDES + j;
            }
            for (u32 j = 1; j + 1U < SHRUB_SIDES; ++j) {
                idx[t++] = base;
                idx[t++] = base + j + 1U;
                idx[t++] = base + j;
                idx[t++] = base + SHRUB_SIDES;
                idx[t++] = base + SHRUB_SIDES + j;
                idx[t++] = base + SHRUB_SIDES + j + 1U;
            }
        }
        r = upload_lod_mesh(ctx, v, n, idx, t, voxels ? "shrub (voxels)" : "shrub", voxels, out);
    }
    if (v != NULL) {
        NPH_FREE_ARRAY(a, NphRenderVertexInput, v, n);
    }
    if (idx != NULL) {
        NPH_FREE_ARRAY(a, u32, idx, k);
    }
    return r;
}

// The sun's shadows a daylight scene asks for (s->shadows): the camera
// walking for SHADOWS_SUN_WALK, along the view, level, at 1.4 m/s and 240
// frames a second; the sun moving for SHADOWS_SUN_TURNING (advance_motion).
static NphResult sun_shadows(Ctx *ctx, const SceneDef *s, const NphRenderView *view)
{
    if (s->shadows != (u32)SHADOWS_NONE) {
        NphRenderLightDesc sun = ctx->sun_desc;
        sun.shadows = 1;
        NPH_TRY(nph_render_light_update(ctx->renderer, ctx->sun, &sun));
    }
    if (s->shadows == (u32)SHADOWS_SUN_TURNING) {
        ctx->sun_turning = true;
        ctx->sun_frame = 0U;
    }
    if (s->shadows == (u32)SHADOWS_SUN_WALK) {
        const NphVec3 f = nph_quat_rotate(view->camera.orientation, nph_vec3(0.0F, 0.0F, -1.0F));
        const f64 step = 1.4 / 240.0 / nph_f64_sqrt((f64)f.x * (f64)f.x + (f64)f.z * (f64)f.z);
        ctx->walk_step = nph_dvec3((f64)f.x * step, 0.0, (f64)f.z * step);
        ctx->walking = true;
    }
    return NPH_RESULT_OK;
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
    if (s->kind == SCENE_FOLIAGE) {
        if (ctx->terrain.value == 0U) {
            NPH_TRY(make_terrain(ctx));
        }
        const bool voxels = (s->post & (u32)POST_NO_VOXELS) == 0U;
        NphRenderMesh *shrub = voxels ? &ctx->shrub : &ctx->shrub_triangles;
        if (shrub->value == 0U) {
            NPH_TRY(make_shrub(ctx, voxels, shrub));
        }
        NPH_TRY(
            add_instance(ctx, ctx->terrain, 3U, trs(0.0, 0.0, 0.0, nph_vec3_splat(1.0F), 0.0F)));
        // Shrubs on the rocks' jittered 10 m grid, 0.7 to 1.3 times their
        // size, turned about the vertical.
        for (u32 i = 0; i < s->count; ++i) {
            const u32 h = hash_u32(i * 7U + 0x51EDU);
            const u32 column = i % side;
            const u32 row = i / side;
            const f64 x = -160.0 + 10.0 * ((f64)column + (f64)(h & 255U) / 255.0);
            const f64 z = -160.0 + 10.0 * ((f64)row + (f64)((h >> 8U) & 255U) / 255.0);
            const f32 scale = 0.7F + 0.6F * (f32)((h >> 16U) & 255U) / 255.0F;
            const f64 y = (f64)terrain_height((f32)x, (f32)z);
            NPH_TRY(add_instance(ctx, *shrub, h >> 24U,
                                 trs(x, y, z, nph_vec3_splat(scale), (f32)(h & 1023U) * 0.01F)));
        }
        const f64 eye_x = -165.0;
        const f64 eye_z = -165.0;
        const f64 eye_y = (f64)terrain_height((f32)eye_x, (f32)eye_z) + 1.7;
        view->camera = nph_render_camera_look_at(nph_dvec3(eye_x, eye_y, eye_z),
                                                 nph_dvec3(0.0, eye_y - 4.0, 0.0),
                                                 nph_vec3_unit_y(), 0.9F, 0.1F);
        return sun_shadows(ctx, s, view);
    }
    if (s->kind == SCENE_LOD) {
        if (ctx->terrain.value == 0U) {
            NPH_TRY(make_terrain(ctx));
            NPH_TRY(make_rock(ctx));
        }
        NPH_TRY(
            add_instance(ctx, ctx->terrain, 3U, trs(0.0, 0.0, 0.0, nph_vec3_splat(1.0F), 0.0F)));
        // Rocks on a jittered 10 m grid over the middle 320 m, 0.8 to 3.2 m
        // across, a third sunk into the ground.
        for (u32 i = 0; i < s->count; ++i) {
            const u32 h = hash_u32(i * 7U + 0x51EDU);
            const u32 column = i % side;
            const u32 row = i / side;
            const f64 x = -160.0 + 10.0 * ((f64)column + (f64)(h & 255U) / 255.0);
            const f64 z = -160.0 + 10.0 * ((f64)row + (f64)((h >> 8U) & 255U) / 255.0);
            const f32 scale = 0.8F + 2.4F * (f32)((h >> 16U) & 255U) / 255.0F;
            const f64 y = (f64)terrain_height((f32)x, (f32)z) + 0.15 * (f64)scale;
            NPH_TRY(add_instance(ctx, ctx->rock, h >> 24U,
                                 trs(x, y, z, nph_vec3_splat(scale), (f32)(h & 1023U) * 0.01F)));
        }
        // Head height at one corner, looking across the diagonal.
        const f64 eye_x = -165.0;
        const f64 eye_z = -165.0;
        const f64 eye_y = (f64)terrain_height((f32)eye_x, (f32)eye_z) + 1.7;
        view->camera = nph_render_camera_look_at(nph_dvec3(eye_x, eye_y, eye_z),
                                                 nph_dvec3(0.0, eye_y - 4.0, 0.0),
                                                 nph_vec3_unit_y(), 0.9F, 0.1F);
        return sun_shadows(ctx, s, view);
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
        NPH_TRY(sun_shadows(ctx, s, view));
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
        if ((s->post & (u32)POST_MANY_LIGHTS) != 0U) {
            NphRenderManyLightDesc md;
            memset(&md, 0, sizeof md);
            md.position = nph_dvec3(-half + 80.0 * (f64)(h & 0xFFFFU) / 65535.0,
                                    0.5 + 2.5 * (f64)(g >> 24U) / 255.0,
                                    -half + 80.0 * (f64)(h >> 16U) / 65535.0);
            md.intensity = (1.0F + 4.0F * (f32)((g >> 16U) & 255U) / 255.0F) *
                           (f32)MAX_MANY_LIGHTS / (f32)s->lights;
            md.color = nph_vec3(1.0F, 0.6F + 0.4F * (f32)(g & 255U) / 255.0F, 0.5F);
            md.near_range = MANY_NEAR_RANGE;
            NPH_TRY(nph_render_many_light_create(ctx->renderer, &md,
                                                 &ctx->many_lights[ctx->many_light_count]));
            ++ctx->many_light_count;
            continue;
        }
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
    for (u32 i = 0; i < ctx->many_light_count; ++i) {
        nph_render_many_light_destroy(ctx->renderer, ctx->many_lights[i]);
    }
    ctx->many_light_count = 0U;
    ctx->moving = false;
    ctx->walking = false;
    ctx->sun_turning = false;
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

// The scene's motion takes a frame's step: the camera walking or turning,
// the sphere bobbing.
static NphResult advance_motion(Ctx *ctx, NphRenderView *view)
{
    if (ctx->walking) {
        view->camera.position = nph_dvec3_add(view->camera.position, ctx->walk_step);
    }
    if (ctx->turn != 0.0F || ctx->strafe != 0.0) {
        const u32 frame = ctx->motion_frame++;
        const f32 sign =
            ctx->motion_reverses && ((frame / MOTION_REVERSE) & 1U) != 0U ? -1.0F : 1.0F;
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
    if (ctx->sun_turning) {
        // About the east-west axis, a turn in 24 minutes of 240 frames a
        // second: every frame's direction differs, which drops every page.
        const f32 angle = 2.0F * NPH_PI / (24.0F * 60.0F * 240.0F) * (f32)(++ctx->sun_frame);
        NphRenderLightDesc sun = ctx->sun_desc;
        sun.shadows = 1;
        sun.direction = nph_quat_rotate(nph_quat_from_axis_angle(nph_vec3_unit_x(), angle),
                                        ctx->sun_desc.direction);
        NPH_TRY(nph_render_light_update(ctx->renderer, ctx->sun, &sun));
    }
    if (ctx->moving) {
        // The sphere bobs a centimetre up and down.
        NphRenderTransform t = ctx->mover_rest;
        t.translation.y += (ctx->move_frame++ & 1U) != 0U ? 0.01 : 0.0;
        NPH_TRY(nph_render_instance_set_transform(ctx->renderer, ctx->mover, &t));
    }
    return NPH_RESULT_OK;
}

// One frame, the scene's motion first taking its step where `advance` says.
static NphResult render_one(Ctx *ctx, NphRenderView *view, NphGpuImage target, bool advance,
                            f64 *cpu_ns, NphRenderFrameStats *stats)
{
    if (advance) {
        NPH_TRY(advance_motion(ctx, view));
    }
    NPH_TRY(nph_gpu_frame_begin(ctx->dev));
    NPH_IGNORE_RESULT(nph_render_gpu_profiler_update(ctx->profiler, ctx->graph));
    nph_gpu_rg_begin(ctx->graph);
    if (ctx->cooked.stream != NULL || ctx->cooked.texture_stream != NULL ||
        cooked_scene_installing(&ctx->cooked)) {
        // The cells' levels, after the frame began (a level's uploads go with
        // it) and before it is drawn; and paged textures' pages or levels.
        const u64 s0 = nph_time_now_ns();
        NPH_TRY(cooked_scene_update(&ctx->cooked, view, ctx->view_height));
        const f64 ns = (f64)(nph_time_now_ns() - s0);
        ctx->stream_ns_sum += ns;
        ctx->stream_ns_max = ns > ctx->stream_ns_max ? ns : ctx->stream_ns_max;
        ctx->stream_frames += 1U;
        if (g_stream_trace && ctx->cooked.texture_stream != NULL) {
            NphTextureStreamStats ts;
            nph_texture_stream_stats(ctx->cooked.texture_stream, &ts);
            printf("    texture stream frame %u: update %.3f ms; installed %u, evicted %u; "
                   "loading %u, short %u, asked %u; held %u levels (%.1f MB)\n",
                   g_trace_frame, ns / 1.0e6, ts.installed, ts.evicted, ts.loading,
                   ts.short_textures, ts.asked, ts.held_levels, (f64)ts.held_bytes / 1.0e6);
        }
        if (g_stream_trace && ctx->cooked.stream != NULL) {
            NphProxyStreamStats ps;
            nph_proxy_stream_stats(ctx->cooked.stream, &ps);
            printf("    stream frame %u: update %.3f ms; installed %u (%.2f MB), dropped %u; "
                   "loading %u, short %u, asked %u; held %u levels (%.1f MB)\n",
                   g_trace_frame, ns / 1.0e6, ps.installed, (f64)ps.installed_bytes / 1.0e6,
                   ps.dropped, ps.loading, ps.short_cells, ps.asked, ps.held_levels,
                   (f64)ps.held_bytes / 1.0e6);
        }
    }
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
    if (g_vsm_trace && r == NPH_RESULT_OK) {
        // The statistics are a frame NPH_GPU_FRAMES_IN_FLIGHT back.
        printf("    vsm frame %u: requested %u, wanted %u, drawn %u (budget %u), resident %u, "
               "evicted %u; meshlets %u, nodes %u, draws %u, voxel draws %u; bias %u, "
               "overflow %u\n",
               g_trace_frame, stats->vsm_requested, stats->vsm_wanted, stats->vsm_drawn,
               stats->vsm_budget, stats->vsm_resident, stats->vsm_evicted, stats->vsm_meshlets,
               stats->vsm_nodes, stats->vsm_draws, stats->vsm_voxel_draws, stats->vsm_lod_bias,
               stats->vsm_overflow);
    }
    if (g_memory_trace && r == NPH_RESULT_OK && g_trace_frame % 16U == 0U) {
        const NphGpuMemoryStats m = nph_gpu_device_memory_stats(ctx->dev);
        printf("    memory frame %u: %.3f GB device-local in use (budget %.3f GB), %.3f GB "
               "host-visible\n",
               g_trace_frame, (f64)m.device_local_used / 1.0e9, (f64)m.device_local_budget / 1.0e9,
               (f64)m.host_visible_used / 1.0e9);
    }
    ++g_trace_frame;
    return r != NPH_RESULT_OK ? r : er;
}

// Frames of the view as it stands until the stream has settled: nothing
// loading, given or dropped, a few frames running (its requests are read
// back NPH_GPU_FRAMES_IN_FLIGHT frames late; cells may stay short of a
// budget), or PRIME_MAX_NS spent; reported.
static NphResult prime_stream(Ctx *ctx, NphRenderView *view, NphGpuImage target, FrameRun *run)
{
    const u64 start = nph_time_now_ns();
    f64 cpu = 0.0;
    u32 frames = 0U;
    u32 quiet = 0U;
    while (quiet < 2U * NPH_GPU_FRAMES_IN_FLIGHT + 2U && nph_time_now_ns() - start < PRIME_MAX_NS) {
        NPH_TRY(render_one(ctx, view, target, false, &cpu, &run->stats));
        NphProxyStreamStats ps;
        nph_proxy_stream_stats(ctx->cooked.stream, &ps);
        quiet = ps.loading == 0U && ps.installed == 0U && ps.dropped == 0U ? quiet + 1U : 0U;
        ++frames;
    }
    NphProxyStreamStats ps;
    nph_proxy_stream_stats(ctx->cooked.stream, &ps);
    printf("    stream settled in %u frames, %.1f s%s: %u levels held past their coarsest "
           "(%.1f MB)\n",
           frames, (f64)(nph_time_now_ns() - start) / 1.0e9, quiet == 0U ? " (NOT SETTLED)" : "",
           ps.held_levels, (f64)ps.held_bytes / 1.0e6);
    return NPH_RESULT_OK;
}

// Frames until the paged textures settle: every page resident, or, with
// --texture-budget, the stream's levels (the pixels asking for them a frame
// or two late): the installs done and none landing for a few frames.
static NphResult prime_pages(Ctx *ctx, NphRenderView *view, NphGpuImage target, FrameRun *run)
{
    const u64 start = nph_time_now_ns();
    f64 cpu = 0.0;
    u32 frames = 0U;
    u32 quiet = 0U;
    NphTextureStreamStats ts;
    memset(&ts, 0, sizeof ts);
    while (quiet < 2U * NPH_GPU_FRAMES_IN_FLIGHT + 2U && nph_time_now_ns() - start < PRIME_MAX_NS) {
        NPH_TRY(render_one(ctx, view, target, false, &cpu, &run->stats));
        bool still = cooked_scene_installing(&ctx->cooked) || run->stats.pages_installed != 0U;
        if (ctx->cooked.texture_stream != NULL) {
            nph_texture_stream_stats(ctx->cooked.texture_stream, &ts);
            still = still || ts.installed != 0U || ts.evicted != 0U || ts.loading != 0U ||
                    run->stats.pages_evicted != 0U;
        }
        quiet = still ? 0U : quiet + 1U;
        ++frames;
    }
    printf("    pages resident in %u frames, %.1f s%s: %u pages (%.1f MB)\n", frames,
           (f64)(nph_time_now_ns() - start) / 1.0e9, quiet == 0U ? " (NOT SETTLED)" : "",
           run->stats.pages_resident, (f64)run->stats.pages_resident * 65536.0 / 1.0e6);
    if (ctx->cooked.texture_stream != NULL) {
        printf("    textures streamed: %u of %u asked for, %u levels held (%.1f MB of %.1f), %u "
               "short of what they want\n",
               ts.asked, ts.textures, ts.held_levels, (f64)ts.held_bytes / 1.0e6,
               (f64)g_texture_budget / 1.0e6, ts.short_textures);
    }
    return NPH_RESULT_OK;
}

// The mean GPU time of the frames the profiler holds (its "frame" total), 0
// before it holds one.
static f64 frame_mean_ms(Ctx *ctx)
{
    static NphRenderPassStats passes[MAX_PASS_STATS];
    const u32 n = nph_render_gpu_profiler_stats(ctx->profiler, passes, MAX_PASS_STATS);
    for (u32 i = 0; i < n; ++i) {
        if (strcmp(passes[i].name, "frame") == 0) {
            return passes[i].samples != 0U ? passes[i].mean_ms : 0.0;
        }
    }
    return 0.0;
}

// Frames of the view as it stands (no motion, so a moving scene's measured
// path does not depend on how long this takes) until the GPU's clock has
// settled: windows of CLOCK_WINDOW frames, until CLOCK_STEADY in a row each
// take at least (1 - CLOCK_SETTLED) of the one before (CLOCK_* above).
static NphResult settle_clock(Ctx *ctx, NphRenderView *view, NphGpuImage target, FrameRun *run)
{
    const u64 start = nph_time_now_ns();
    f64 cpu = 0.0;
    f64 previous = 0.0;
    u32 steady = 0U;
    nph_render_gpu_profiler_reset(ctx->profiler);
    while (steady < CLOCK_STEADY && nph_time_now_ns() - start < CLOCK_MAX_NS) {
        for (u32 i = 0; i < CLOCK_WINDOW; ++i) {
            NPH_TRY(render_one(ctx, view, target, false, &cpu, &run->stats));
        }
        const f64 mean = frame_mean_ms(ctx);
        nph_render_gpu_profiler_reset(ctx->profiler);
        steady = previous > 0.0 && mean >= previous * (1.0 - CLOCK_SETTLED) ? steady + 1U : 0U;
        previous = mean;
    }
    return NPH_RESULT_OK;
}

// The clock settled, a warm-up with the scene's motion, then `frames`
// measured frames. The profiler records a frame NPH_GPU_FRAMES_IN_FLIGHT
// frames after it was submitted, so it is reset that many frames into the
// measurement and fed that many frames past it.
static NphResult run_frames(Ctx *ctx, NphRenderView *view, NphGpuImage target, u32 frames,
                            FrameRun *run)
{
    NPH_TRY(settle_clock(ctx, view, target, run));
    const u64 start = nph_time_now_ns();
    f64 cpu = 0.0;
    for (u32 i = 0; i < WARMUP_FRAMES || nph_time_now_ns() - start < WARMUP_NS; ++i) {
        NPH_TRY(render_one(ctx, view, target, true, &cpu, &run->stats));
    }
    run->cpu_count = 0U;
    ctx->stream_ns_sum = 0.0;
    ctx->stream_ns_max = 0.0;
    ctx->stream_frames = 0U;
    for (u32 i = 0; i < frames + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        if (i == NPH_GPU_FRAMES_IN_FLIGHT) {
            nph_render_gpu_profiler_reset(ctx->profiler);
        }
        NPH_TRY(render_one(ctx, view, target, true, &cpu, &run->stats));
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
    if ((s->post & (u32)POST_LOD_LEVEL0) != 0U) {
        view.lod_error = 0.0F;
    }
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
        ctx->motion_reverses = true;
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
        }
        char p99_suffix[100];
        (void)snprintf(p99_suffix, sizeof p99_suffix, "%s.p99", suffix);
        Result *p99 = add_result(ctx, name, p99_suffix);
        if (p99 == NULL) {
            return NPH_RESULT_FULL;
        }
        p99->samples = p->samples;
        p99->ns = p->p99_ms * 1.0e6;
        p99->metric = p99->ns;
    }
    for (u32 i = 0; i < n; ++i) {
        const NphRenderPassStats *p = &passes[i];
        if (p->samples != 0U && strcmp(p->name, "frame") != 0) {
            printf("    %-28s %8.3f ms  %8.3f ms p99  %9.1f MB read  %9.1f MB written\n", p->name,
                   p->mean_ms, p->p99_ms, (f64)p->read_bytes / 1.0e6, (f64)p->write_bytes / 1.0e6);
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
        if (st->vis_lod_groups != 0U || st->vis_lod_nodes != 0U) {
            printf("    levels of detail: %u groups tested, %u hierarchy nodes visited in %u steps "
                   "a phase, %u clusters a finer level stood in for\n",
                   st->vis_lod_groups, st->vis_lod_nodes, st->vis_lod_steps,
                   st->vis_lod_clusters_culled);
        }
        if (st->vis_voxel_clusters != 0U) {
            printf("    voxels: %u clusters, %u bricks\n", st->vis_voxel_clusters,
                   st->vis_voxel_bricks);
        }
        if (st->vis_cells_proxied != 0U) {
            printf("    cells: %u drawn as their proxies, for %u instances\n",
                   st->vis_cells_proxied, st->vis_instances_proxied);
        }
        if (ctx->cooked.stream != NULL) {
            NphProxyStreamStats ps;
            nph_proxy_stream_stats(ctx->cooked.stream, &ps);
            printf("    stream: %u cells, %u levels held past their coarsest (%.1f MB), %u "
                   "asked, %u short, %u loading, last frame %u installed (%.1f MB) and %u "
                   "dropped, %u failed; update %.3f ms mean, %.3f ms max a frame\n",
                   ps.cells, ps.held_levels, (f64)ps.held_bytes / 1.0e6, ps.asked, ps.short_cells,
                   ps.loading, ps.installed, (f64)ps.installed_bytes / 1.0e6, ps.dropped, ps.failed,
                   ctx->stream_frames != 0U ? ctx->stream_ns_sum / ctx->stream_frames / 1.0e6 : 0.0,
                   ctx->stream_ns_max / 1.0e6);
        }
        if (ctx->cooked.texture_stream != NULL) {
            NphTextureStreamStats ts;
            nph_texture_stream_stats(ctx->cooked.texture_stream, &ts);
            printf("    textures: %u streamed, %u asked, %u levels held past their small ones "
                   "(%.1f MB), %u short, %u loading; last frame %u pages installed, %u evicted",
                   ts.textures, ts.asked, ts.held_levels, (f64)ts.held_bytes / 1.0e6,
                   ts.short_textures, ts.loading, ts.installed, ts.evicted);
            if (ctx->cooked.stream == NULL) {
                printf("; update %.3f ms mean, %.3f ms max a frame",
                       ctx->stream_frames != 0U ? ctx->stream_ns_sum / ctx->stream_frames / 1.0e6
                                                : 0.0,
                       ctx->stream_ns_max / 1.0e6);
            }
            printf("\n");
        }
        if (st->vsm) {
            printf("    virtual shadow map: pages requested %u, wanted %u, drawn %u (budget %u), "
                   "resident %u, evicted %u; caster meshlets %u (%u hierarchy nodes), draws %u "
                   "(and %u of voxel clusters); resolution bias %u%s\n",
                   st->vsm_requested, st->vsm_wanted, st->vsm_drawn, st->vsm_budget,
                   st->vsm_resident, st->vsm_evicted, st->vsm_meshlets, st->vsm_nodes,
                   st->vsm_draws, st->vsm_voxel_draws, st->vsm_lod_bias,
                   st->vsm_overflow != 0U ? "; OVERFLOWED" : "");
        }
        if (st->shadow_blas_bytes != 0U) {
            printf("    ray tracing: bottom levels %.1f MB (compacted after their builds)\n",
                   (f64)st->shadow_blas_bytes / 1.0e6);
        }
    } else {
        printf("    draws %u, triangles %u, culled %u, lights %u%s\n", st->draws, st->triangles,
               st->culled, st->lights, st->shadow_rays ? "; shadow rays" : "");
    }
    // What the process holds of the GPU's memory once the scene has run, its
    // transient targets included. Past the device-local budget (the OS's
    // share for the process, VK_EXT_memory_budget), allocations may fail or
    // slow the frame: the OS keeps what does not fit in system memory.
    const NphGpuMemoryStats mem = nph_gpu_device_memory_stats(ctx->dev);
    printf("    gpu memory: %.2f GB device-local in use (budget %.2f GB), %.2f GB host-visible "
           "(budget %.2f GB); %u allocations in %.2f GB of blocks\n",
           (f64)mem.device_local_used / 1.0e9, (f64)mem.device_local_budget / 1.0e9,
           (f64)mem.host_visible_used / 1.0e9, (f64)mem.host_visible_budget / 1.0e9,
           mem.allocation_count, (f64)mem.block_bytes / 1.0e9);
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
// A two-element array as x and z.
static bool json_vec2(const NphJsonValue *object, const char *key, NphDVec3 *out)
{
    const NphJsonValue *a = nph_json_object_get(object, nph_str_from_cstr(key));
    f64 v[2];
    for (u32 i = 0; i < 2U; ++i) {
        if (nph_json_as_f64(nph_json_array_at(a, i), &v[i]) != NPH_RESULT_OK) {
            return false;
        }
    }
    *out = nph_dvec3(v[0], 0.0, v[1]);
    return true;
}

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
        m->light_near_range = (f32)nph_json_get_f64(root, NPH_STR("light_near_range"), 0.0);
        m->many_lights = (u32)nph_json_get_f64(root, NPH_STR("many_lights"), 0.0);
        m->sun_shadows = nph_json_get_bool(root, NPH_STR("sun_shadows"), false);
        m->lists = (u32)nph_json_get_f64(root, NPH_STR("lists"), 0.0);
        m->tiles_x = 1U;
        m->tiles_z = 1U;
        const NphJsonValue *tiles = nph_json_object_get(root, NPH_STR("tiles"));
        if (tiles != NULL) {
            NphDVec3 count = nph_dvec3(1.0, 0.0, 1.0);
            NphDVec3 pitch = nph_dvec3(0.0, 0.0, 0.0);
            ok = json_vec2(tiles, "count", &count) && json_vec2(tiles, "pitch", &pitch);
            m->tiles_x = ok ? (u32)count.x : 1U;
            m->tiles_z = ok ? (u32)count.z : 1U;
            m->tile_pitch = pitch;
            m->tile_instances = (u32)nph_json_get_f64(tiles, NPH_STR("instances"), 0.0);
            m->tile_meshes = (u32)nph_json_get_f64(tiles, NPH_STR("meshes"), 0.0);
            m->tile_cells = (u32)nph_json_get_f64(tiles, NPH_STR("cells"), 0.0);
            ok = ok && m->tiles_x != 0U && m->tiles_z != 0U && m->tile_instances != 0U;
        }
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
            rv->yaw = (f32)nph_json_get_f64(v, NPH_STR("yaw"), 0.0);
            rv->walk = (f32)nph_json_get_f64(v, NPH_STR("walk"), 0.0);
            rv->turn = (f32)nph_json_get_f64(v, NPH_STR("turn"), 0.0);
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
    cd.light_near_range = m->light_near_range;
    cd.no_cells = g_no_cells;
    cd.stream = g_stream;
    cd.stream_budget = g_stream_budget;
    cd.stream_loads = g_stream_loads;
    cd.tiles_x = g_tiles_x != 0U ? g_tiles_x : m->tiles_x;
    cd.tiles_z = g_tiles_z != 0U ? g_tiles_z : m->tiles_z;
    cd.tile_pitch = m->tile_pitch;
    cd.paged_textures = g_paged_textures;
    cd.texture_budget = g_texture_budget;
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
    printf("%s: loaded in %.1f s: %u instances, %u lights, %u materials, %u cells with proxies\n",
           m->name, (f64)(nph_time_now_ns() - t0) / 1.0e9, ctx->cooked.instance_count,
           ctx->cooked.light_count, ctx->cooked.render_material_count, ctx->cooked.proxy_count);
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
    if (rv->yaw != 0.0F) {
        camera.orientation = nph_quat_normalize(
            nph_quat_mul(nph_quat_from_axis_angle(nph_vec3_unit_y(), rv->yaw * (NPH_PI / 180.0F)),
                         camera.orientation));
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
    if (g_lod_error >= 0.0F) {
        view.lod_error = g_lod_error;
    }
    if (ctx->no_sw_raster) {
        view.vis_flags |= (u32)NPH_RENDER_VIS_NO_SW_RASTER;
    }
    if (rv->walk != 0.0F) {
        // Along the view, level, as the synthetic walk.
        const NphVec3 f = nph_quat_rotate(camera.orientation, nph_vec3(0.0F, 0.0F, -1.0F));
        const f64 step = (f64)rv->walk * (f64)MOTION_FRAME /
                         nph_f64_sqrt((f64)f.x * (f64)f.x + (f64)f.z * (f64)f.z);
        ctx->walk_step = nph_dvec3((f64)f.x * step, 0.0, (f64)f.z * step);
        ctx->walking = true;
    }
    if (rv->turn != 0.0F) {
        ctx->turn = rv->turn * (NPH_PI / 180.0F) * MOTION_FRAME;
        ctx->strafe = 0.0;
        ctx->motion_frame = 0U;
        ctx->motion_reverses = false;
    }
    static FrameRun run;
    ctx->view_height = g_ref_height; // the stream's view (--stream)
    NphResult r =
        ctx->cooked.stream != NULL ? prime_stream(ctx, &view, target, &run) : NPH_RESULT_OK;
    if (r == NPH_RESULT_OK && g_paged_textures) {
        r = prime_pages(ctx, &view, target, &run);
    }
    if (r == NPH_RESULT_OK) {
        r = run_frames(ctx, &view, target, frames, &run);
    }
    ctx->walking = false;
    ctx->turn = 0.0F;
    if (r == NPH_RESULT_OK && capture_dir != NULL) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/%s.bmp", capture_dir, rs->name);
        r = capture(ctx, target, g_ref_width, g_ref_height, path);
    }
    NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(ctx->dev));
    nph_gpu_image_destroy(ctx->dev, target);
    NPH_TRY(r);
    NPH_TRY(report(ctx, rs->name, g_ref_width, g_ref_height, NPH_RENDER_PIPELINE_PRODUCTION, false,
                   &run));
    if (m->lists != 0U && (run.stats.vis_overflow != 0U || run.stats.vsm_overflow != 0U)) {
        fprintf(stderr,
                "render_perf: %s: lists overflowed at the manifest's size (%u): the "
                "frame dropped work\n",
                rs->name, g_lists > 1U ? g_lists : m->lists);
        return NPH_RESULT_FULL;
    }
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

static NphResult setup(Ctx *ctx, bool validation, f64 sw_pixels, const char *filter)
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
    // some 8,000 GPU objects, a streamed landscape tens of thousands (its
    // level meshes, a dozen buffers each).
    dd.deferred_destroy_capacity = 1U << 17U;
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
    // A tiled scene's instances, cells and streamed levels are each tile's:
    // room for the largest world of the manifests the filter takes, or for
    // --tiles of the largest scene.
    const u32 more_tiles = g_tiles_x != 0U ? g_tiles_x * g_tiles_z - 1U : 0U;
    rd.max_instances = MAX_INSTANCES + more_tiles * TILE_INSTANCES;
    rd.max_meshes = MAX_MESHES + more_tiles * TILE_MESHES;
    rd.max_cells = 4096U + more_tiles * TILE_CELLS;
    for (u32 i = 0; i < g_ref_count && g_tiles_x == 0U; ++i) {
        const RefManifest *m = &g_manifests[g_ref_scenes[i].manifest];
        const u32 tiles = m->tiles_x * m->tiles_z;
        if (glob_match(filter, g_ref_scenes[i].name)) {
            rd.max_many_lights = nph_max_u32(rd.max_many_lights, m->many_lights);
        }
        if (tiles > 1U && glob_match(filter, g_ref_scenes[i].name)) {
            rd.max_instances = nph_max_u32(rd.max_instances, tiles * m->tile_instances);
            rd.max_meshes = nph_max_u32(rd.max_meshes, MAX_MESHES + tiles * m->tile_meshes);
            rd.max_cells = nph_max_u32(rd.max_cells, 4096U + tiles * m->tile_cells);
        }
    }
    rd.max_draws = MAX_INSTANCES;    // the reference path draws every grid cube
    rd.max_lights = MAX_LIGHTS + 1U; // and the sun
    // Only where a scene that runs has them: they add pipelines to make.
    for (u32 i = 0; i < NPH_ARRAY_COUNT(g_scenes); ++i) {
        if ((g_scenes[i].post & (u32)POST_MANY_LIGHTS) != 0U &&
            glob_match(filter, g_scenes[i].name)) {
            rd.max_many_lights = nph_max_u32(rd.max_many_lights, MAX_MANY_LIGHTS);
        }
    }
    rd.voxel_pool_bytes =
        (u64)(g_voxel_pool_mb >= 0 ? g_voxel_pool_mb : (g_stream ? 1536 : 0)) * 1000000ULL;
    // lod_4k_level0 defers some 400,000 clusters to phase 1 (the default
    // holds 262,144).
    rd.max_deferred = 1U << 20U;
    if (g_paged_textures && g_texture_budget == 0U) {
        // Every page at once, before measuring: a large binding and staging
        // (the stream's 32 moves a frame would take Bistro 1,600 frames).
        rd.max_page_moves = 1024U;
        rd.page_staging_slots = 4096U;
        rd.texture_page_bytes = NPH_GIB(4);
    } else if (g_paged_textures) {
        // Streamed: the renderer's own moves a frame and staging; the pool
        // holds the budget and the textures' small levels (a page or a few
        // each).
        rd.texture_page_bytes = g_texture_budget + NPH_MIB(512);
    }
    // --lists, or the most a manifest the filter takes says its views need.
    u32 lists = g_lists;
    for (u32 i = 0; i < g_ref_count; ++i) {
        if (glob_match(filter, g_ref_scenes[i].name)) {
            lists = nph_max_u32(lists, g_manifests[g_ref_scenes[i].manifest].lists);
        }
    }
    nph_renderer_desc_scale_lists(&rd, lists);
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
        nph_render_mesh_destroy(ctx->renderer, ctx->terrain);
        nph_render_mesh_destroy(ctx->renderer, ctx->rock);
        nph_render_mesh_destroy(ctx->renderer, ctx->shrub);
        nph_render_mesh_destroy(ctx->renderer, ctx->shrub_triangles);
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
                    "[--ref-size=WxH] [--debug-view=NAME] [--reference-only] "
                    "[--lod-error=PX] [--lists=N] [--vsm-trace] [--memory-trace] [--stream[=MB]] "
                    "[--stream-trace] [--stream-loads=N] [--voxel-pool=MB] [--tiles=WxH] "
                    "[--no-cells] [--paged-textures] [--texture-budget=MB]\n");
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
        } else if (strncmp(argv[i], "--lod-error=", 12U) == 0) {
            g_lod_error = (f32)strtod(argv[i] + 12, NULL);
        } else if (strncmp(argv[i], "--lists=", 8U) == 0) {
            g_lists = (u32)strtoul(argv[i] + 8, NULL, 10);
            if (g_lists == 0U || g_lists > 16U || (g_lists & (g_lists - 1U)) != 0U) {
                return usage();
            }
        } else if (strcmp(argv[i], "--vsm-trace") == 0) {
            g_vsm_trace = true;
        } else if (strcmp(argv[i], "--memory-trace") == 0) {
            g_memory_trace = true;
        } else if (strcmp(argv[i], "--no-cells") == 0) {
            g_no_cells = true;
        } else if (strcmp(argv[i], "--stream") == 0) {
            g_stream = true;
        } else if (strncmp(argv[i], "--stream=", 9U) == 0) {
            g_stream = true;
            g_stream_budget = (u64)strtoull(argv[i] + 9, NULL, 10) * 1000000ULL;
        } else if (strcmp(argv[i], "--stream-trace") == 0) {
            g_stream_trace = true;
        } else if (strcmp(argv[i], "--paged-textures") == 0) {
            g_paged_textures = true;
        } else if (strncmp(argv[i], "--texture-budget=", 17U) == 0) {
            g_paged_textures = true;
            g_texture_budget = (u64)strtoull(argv[i] + 17, NULL, 10) * 1000000ULL;
        } else if (strncmp(argv[i], "--voxel-pool=", 13U) == 0) {
            g_voxel_pool_mb = (i64)strtoll(argv[i] + 13, NULL, 10);
        } else if (strncmp(argv[i], "--stream-loads=", 15U) == 0) {
            g_stream_loads = (u32)strtoul(argv[i] + 15, NULL, 10);
        } else if (strncmp(argv[i], "--tiles=", 8U) == 0) {
            char *x = NULL;
            const unsigned long w = strtoul(argv[i] + 8, &x, 10);
            const unsigned long h = x != NULL && *x == 'x' ? strtoul(x + 1, NULL, 10) : 0UL;
            if (w == 0UL || h == 0UL || w > 64UL || h > 64UL || w * h > 64UL) {
                return usage();
            }
            g_tiles_x = (u32)w;
            g_tiles_z = (u32)h;
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
    const NphResult sr = setup(&ctx, validation, sw_pixels, filter);
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
