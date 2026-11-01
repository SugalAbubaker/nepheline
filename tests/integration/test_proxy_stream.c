// SPDX-License-Identifier: Apache-2.0
// A partitioned world's proxies streamed level by level (ADR-0063 stage 3;
// runtime/proxy_stream.h), end to end across the cooker, the asset
// runtime, the job system, the renderer, and the runtime:
//
// 1. the cooker tests' grove (four cells of 8 m, two tufts of needles in
//    each) is cooked with its world partition's cells, finer than the
//    cooker's default so that each cell's proxy has levels to stream, and
//    packed;
// 2. a loader with a job system loads the scene, each cell's head and its
//    coarsest level; the renderer makes each cell with its coarsest alone
//    (nph_proxy_level_mesh_create), the grove placed once or twice, and a
//    stream is given the cells, which makes their levels in the renderer's
//    voxel pool on the jobs (or on the main thread);
// 3. frames are drawn, the stream updated before each as a game's loop
//    does, and what it holds, loads and drops is checked against the
//    views' requests and its budget, and what is drawn against the cells
//    given every level directly.
//
// The tests skip without a Vulkan device or the renderer's production path.
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/core/alloc_debug.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/time.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/proxy_stream.h>
#include <nepheline/runtime/scene_render.h>
#include <nepheline/scene/transform.h>

#include "test_cook_support.h"
#include "test_render_support.h"

#include <cook/cooker.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define GROVE_CELLS 4U
// Proxies seen from 140 m on: their finest cell a pixel there (a sixteenth
// of a metre), a few levels to a cell (from the cooker's default, 560 m,
// two).
#define GROVE_DISTANCE 140.0F
// The second grove of the budget's test lies this far beyond the first on
// -z, both seen from GROVE_VIEW on either side.
#define GROVE_APART 400.0
#define GROVE_VIEW  200.0
#define MAX_GROVES  2U
#define MAX_CELLS   (MAX_GROVES * GROVE_CELLS)

#define TARGET_SIZE   256U
#define TARGET_PIXELS ((u64)TARGET_SIZE * TARGET_SIZE)
// A narrow view on one grove (it fills most of the target), a wide one on
// two.
#define NARROW_FOV 0.1F
#define WIDE_FOV   0.4F
#define ZNEAR      0.5F

// Frames the stream must stay quiet to count as settled (its requests are
// read back NPH_GPU_FRAMES_IN_FLIGHT frames late), and the most it is given.
#define QUIET_FRAMES  (2U * NPH_GPU_FRAMES_IN_FLIGHT + 2U)
#define SETTLE_FRAMES 600U
#define SETTLE_NS     (120ULL * 1000000000ULL)

#define REQUIRE_PRODUCTION(f)                                                                      \
    do {                                                                                           \
        REQUIRE_RENDERER(&(f)->rf);                                                                \
        if (!nph_renderer_production_supported((f)->rf.renderer)) {                                \
            NTEST_SKIP("no production path on this device");                                       \
        }                                                                                          \
    } while (0)

typedef struct StreamWorld {
    NphAllocator *alloc;
    RenderFixture rf;
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;
    NphGpuImage target;
    u8 *pixels;
    NphRenderMaterial grey;
    // The cells, grove by grove, each made with its coarsest level alone.
    u32 cell_count;
    NphRenderCell cells[MAX_CELLS];
    NphRenderMesh coarsest[MAX_CELLS];
    NphProxyStreamCell stream_cells[MAX_CELLS];
    NphMeshLevel levels[MAX_CELLS][NPH_RENDER_CELL_MAX_LEVELS]; // their heads' tables
    NphGuid heads[MAX_CELLS];
    NphProxyStream *stream;
} StreamWorld;

// A renderer with a voxel pool of `pool` bytes, where the levels are made
// on the jobs (ADR-0063 stage 3c).
static void world_setup_materials(StreamWorld *f, u64 pool, u32 max_materials)
{
    memset(f, 0, sizeof *f);
    f->alloc = nph_allocator_system();
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_meshes = 128U;
    d.max_materials = max_materials;
    d.max_instances = 256U;
    d.max_cells = 16U;
    d.voxel_pool_bytes = pool;
    test_render_setup(&f->rf, &d);
}

static void world_setup(StreamWorld *f, u64 pool)
{
    world_setup_materials(f, pool, 4U);
}

NTEST_SETUP(StreamWorld)
{
    world_setup(f, 16U << 20U);
}

static void world_teardown(StreamWorld *f);

NTEST_TEARDOWN(StreamWorld)
{
    world_teardown(f);
}

// The world with a pool too small for the levels past the coarsest.
typedef StreamWorld TightPool;

NTEST_SETUP(TightPool)
{
    world_setup(f, 64U << 10U);
}

NTEST_TEARDOWN(TightPool)
{
    world_teardown(f);
}

static void world_teardown(StreamWorld *f)
{
    NphRenderer *r = f->rf.renderer;
    nph_proxy_stream_destroy(f->stream);
    for (u32 k = 0; k < f->cell_count; ++k) {
        nph_render_cell_destroy(r, f->cells[k]);
        nph_render_mesh_destroy(r, f->coarsest[k]);
    }
    if (r != NULL) {
        nph_render_material_destroy(r, f->grey);
    }
    nph_asset_loader_destroy(f->loader);
    nph_vfs_destroy(f->vfs);
    nph_job_system_destroy(f->jobs);
    if (f->pixels != NULL) {
        NPH_FREE_ARRAY(f->alloc, u8, f->pixels, TARGET_PIXELS * 4U);
        nph_gpu_image_destroy(f->rf.dev, f->target);
    }
    test_render_teardown(&f->rf);
}

// The grove's pack, cooked once for the tests that need it (a world
// partition's cook is slow under the sanitizers) and removed at exit.
static TempDir g_grove_dir;
static char g_grove_pack[1200];
static bool g_grove_cooked = false;

static void grove_remove(void)
{
    NPH_IGNORE_RESULT(nph_fs_remove_tree(g_grove_dir.dir));
}

static void grove_cook(NphAllocator *alloc, NphJobSystem *jobs)
{
    if (g_grove_cooked) {
        return;
    }
    test_temp_dir_make(&g_grove_dir, "proxy_stream");
    NTEST_ASSERT_EQ_INT(atexit(grove_remove), 0);
    char gltf[1200];
    char out_dir[1200];
    test_path_in(&g_grove_dir, "grove.gltf", gltf, sizeof gltf);
    test_path_in(&g_grove_dir, "cooked", out_dir, sizeof out_dir);
    test_path_in(&g_grove_dir, "grove.npk", g_grove_pack, sizeof g_grove_pack);
    test_write_grove(gltf);
    CookerDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.alloc = alloc;
    cd.out_dir = out_dir;
    cd.source_root = g_grove_dir.dir;
    cd.jobs = jobs;
    cook_texture_settings_default(&cd.texture);
    cook_mesh_settings_default(&cd.mesh);
    cd.mesh.voxels = 1;
    cd.hlod.enabled = 1;
    cd.hlod.cell = 8.0F;
    cd.hlod.distance = GROVE_DISTANCE;
    Cooker *cooker = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&cd, &cooker), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(cooker, gltf), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cooker_stats(cooker).failed, 0U);
    NTEST_ASSERT_EQ_INT(cooker_pack(cooker, g_grove_pack), NPH_RESULT_OK);
    cooker_destroy(cooker);
    g_grove_cooked = true;
}

// The grove's pack (cooked the first time) and a loader on it, with a job
// system; the target and a material to draw the cells with.
static void world_open(StreamWorld *f)
{
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = f->alloc;
    jd.worker_count = 2U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &f->jobs), NPH_RESULT_OK);
    grove_cook(f->alloc, f->jobs);
    NTEST_ASSERT_EQ_INT(nph_vfs_create(f->alloc, &f->vfs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("content"), g_grove_pack, NULL),
                        NPH_RESULT_OK);
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = f->alloc;
    ld.vfs = f->vfs;
    ld.jobs = f->jobs;
    ld.verify = NPH_ASSET_VERIFY_ALWAYS;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&ld, &f->loader), NPH_RESULT_OK);

    f->target = test_make_target(f->rf.dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, TARGET_SIZE);
    f->pixels = NPH_ALLOC_ARRAY(f->alloc, u8, TARGET_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(f->pixels);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.name = "grey";
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->rf.renderer, &md, &f->grey), NPH_RESULT_OK);
}

// The render mesh of a level asset, loaded at once and let go.
static NphRenderMesh level_mesh(StreamWorld *f, NphGuid guid)
{
    NphAssetHandle h = NPH_ASSET_HANDLE_NULL;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid(f->loader, guid, &h), NPH_RESULT_OK);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(
        nph_proxy_level_mesh_create(f->rf.renderer, nph_asset_get(f->loader, h), &mesh),
        NPH_RESULT_OK);
    nph_asset_release(f->loader, h);
    return mesh;
}

// The grove's cells placed `dz` along z: each made with its coarsest level
// alone and its head's levels' errors, as a game's world partition makes
// them; its head's table kept for the stream.
static void place_grove(StreamWorld *f, f64 dz)
{
    NphAssetHandle sh = NPH_ASSET_HANDLE_NULL;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid(f->loader, nph_guid_from_cstr("grove.gltf#scene"), &sh),
                        NPH_RESULT_OK);
    NphSceneView sv;
    NTEST_ASSERT_EQ_INT(nph_scene_view(nph_asset_get(f->loader, sh), &sv), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(sv.cell_count, GROVE_CELLS);
    for (u32 i = 0; i < sv.cell_count; ++i) {
        const NphSceneCell *sc = &sv.cells[i];
        const u32 k = f->cell_count;
        NTEST_ASSERT_LT_UINT(k, MAX_CELLS);
        NphAssetHandle hh = NPH_ASSET_HANDLE_NULL;
        NTEST_ASSERT_EQ_INT(nph_asset_load_guid(f->loader, sc->proxy, &hh), NPH_RESULT_OK);
        NphMeshView head;
        NTEST_ASSERT_EQ_INT(nph_mesh_view(nph_asset_get(f->loader, hh), &head), NPH_RESULT_OK);
        NTEST_ASSERT_NOT_NULL(head.levels);
        // Levels to stream: at least two past the coarsest.
        NTEST_ASSERT_GE_UINT(head.level_count, 3U);
        NTEST_ASSERT_LE_UINT(head.level_count, NPH_RENDER_CELL_MAX_LEVELS);
        memcpy(f->levels[k], head.levels, head.level_count * sizeof(NphMeshLevel));
        f->heads[k] = sc->proxy;
        const u32 count = head.level_count;
        nph_asset_release(f->loader, hh);
        f->coarsest[k] = level_mesh(f, f->levels[k][count - 1U].mesh);

        NphRenderCellDesc d;
        memset(&d, 0, sizeof d);
        d.center = nph_dvec3(((f64)sc->bounds.min.x + (f64)sc->bounds.max.x) * 0.5,
                             ((f64)sc->bounds.min.y + (f64)sc->bounds.max.y) * 0.5,
                             ((f64)sc->bounds.min.z + (f64)sc->bounds.max.z) * 0.5 + dz);
        d.extent = nph_vec3_scale(nph_vec3_sub(sc->bounds.max, sc->bounds.min), 0.5F);
        d.proxy_levels = &f->coarsest[k];
        d.proxy_level_count = 1U;
        d.proxy_materials = &f->grey;
        d.proxy_material_count = 1U;
        d.proxy_transform = nph_render_transform_identity();
        d.proxy_transform.translation =
            nph_dvec3_add(nph_dvec3_from_vec3(sc->proxy_origin), nph_dvec3(0.0, 0.0, dz));
        d.proxy_error = sc->proxy_cell;
        d.distance = sc->proxy_distance;
        f32 errors[NPH_RENDER_CELL_MAX_LEVELS];
        for (u32 l = 0; l < count; ++l) {
            errors[l] = f->levels[k][l].error;
        }
        d.proxy_level_errors = errors;
        d.proxy_level_error_count = count;
        NTEST_ASSERT_EQ_INT(nph_render_cell_create(f->rf.renderer, &d, &f->cells[k]),
                            NPH_RESULT_OK);

        NphProxyStreamCell *pc = &f->stream_cells[k];
        memset(pc, 0, sizeof *pc);
        pc->cell = f->cells[k];
        pc->coarsest = f->coarsest[k];
        pc->levels = f->levels[k];
        pc->level_count = count;
        pc->center = d.center;
        pc->extent = d.extent;
        pc->distance = d.distance;
        f->cell_count = k + 1U;
    }
    nph_asset_release(f->loader, sh);
}

// A stream of the cells, within `budget` bytes (0: none), its levels made on
// the jobs in the voxel pool, or with `on_main` on the main thread.
static void stream_make(StreamWorld *f, u64 budget, bool on_main)
{
    NphProxyStreamDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = f->alloc;
    d.renderer = f->rf.renderer;
    d.loader = f->loader;
    d.jobs = on_main ? NULL : f->jobs;
    d.max_cells = MAX_CELLS;
    d.budget_bytes = budget;
    NTEST_ASSERT_EQ_INT(nph_proxy_stream_create(&d, &f->stream), NPH_RESULT_OK);
    for (u32 k = 0; k < f->cell_count; ++k) {
        NTEST_ASSERT_EQ_INT(nph_proxy_stream_add(f->stream, &f->stream_cells[k], NULL),
                            NPH_RESULT_OK);
    }
}

static void stream_free(StreamWorld *f)
{
    nph_proxy_stream_destroy(f->stream);
    f->stream = NULL;
}

// A view of the cells from `eye` on `at`, `yfov` high, its normals drawn
// (no light nor history to tell two frames apart), at a threshold of
// `lod_error` pixels.
static NphRenderView grove_view(NphDVec3 eye, NphDVec3 at, f32 yfov, f32 lod_error)
{
    NphRenderView v;
    nph_render_view_default(
        &v, nph_render_camera_look_at(eye, at, nph_vec3(0.0F, 1.0F, 0.0F), yfov, ZNEAR));
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v.anti_aliasing = NPH_RENDER_AA_NONE;
    v.tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3_zero();
    v.disable_ibl = 1;
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    v.lod_error = lod_error;
    return v;
}

// The first grove from GROVE_VIEW out on +z, a little above.
static NphDVec3 near_eye(void)
{
    return nph_dvec3(8.0, 40.0, 8.0 + GROVE_VIEW);
}

static NphDVec3 grove_centre(f64 dz)
{
    return nph_dvec3(8.0, 1.0, 8.0 + dz);
}

// The distance from `eye` to cell `k`'s box.
static f32 box_distance(const StreamWorld *f, u32 k, NphDVec3 eye)
{
    const NphProxyStreamCell *c = &f->stream_cells[k];
    const f64 dx = nph_f64_max(nph_f64_abs(eye.x - c->center.x) - (f64)c->extent.x, 0.0);
    const f64 dy = nph_f64_max(nph_f64_abs(eye.y - c->center.y) - (f64)c->extent.y, 0.0);
    const f64 dz = nph_f64_max(nph_f64_abs(eye.z - c->center.z) - (f64)c->extent.z, 0.0);
    return (f32)nph_f64_sqrt(dx * dx + dy * dy + dz * dz);
}

// A threshold at which every cell of the first grove (the first
// GROVE_CELLS) asks for its level 0 from `eye`, `yfov` high: half of the
// largest that would, by the requests' test (render/renderer.h
// nph_render_cell_request).
static f32 fine_threshold(const StreamWorld *f, NphDVec3 eye, f32 yfov)
{
    const f32 per_unit = 0.5F * (f32)TARGET_SIZE / nph_f32_tan(0.5F * yfov);
    f32 t = NPH_F32_MAX;
    for (u32 k = 0; k < GROVE_CELLS; ++k) {
        const f32 reach = nph_f32_max(box_distance(f, k, eye) * 0.75F, ZNEAR);
        t = nph_f32_min(t, f->levels[k][0].error * per_unit / reach);
    }
    return 0.5F * t;
}

// One frame: the stream (where there is one) updated after the frame
// begins and before it is drawn, as a game's loop does.
static NphRenderFrameStats stream_frame(StreamWorld *f, const NphRenderView *v)
{
    RenderFixture *rf = &f->rf;
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(rf->dev), NPH_RESULT_OK);
    nph_gpu_rg_begin(rf->graph);
    if (f->stream != NULL) {
        const NphProxyStreamView sv = nph_proxy_stream_view(v, TARGET_SIZE);
        NTEST_ASSERT_EQ_INT(nph_proxy_stream_update(f->stream, &sv), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_render_frame(rf->renderer, v, rf->graph, f->target, NULL, &stats),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(rf->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(rf->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(rf->dev), NPH_RESULT_OK);
    return stats;
}

// A few frames, the last one read back.
static void draw(StreamWorld *f, const NphRenderView *v)
{
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        stream_frame(f, v);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
    test_read_image(f->rf.dev, f->target, 0U, f->pixels, TARGET_PIXELS * 4U);
}

typedef struct Settled {
    u32 frames;
    u32 installed; // levels given, in all
    u32 dropped;
    u32 building; // the most being made on the jobs at once
} Settled;

// Frames until the stream is quiet: nothing loading, given or dropped,
// QUIET_FRAMES running (cells may stay short: of a budget, or a failure).
static Settled settle(StreamWorld *f, const NphRenderView *v)
{
    Settled out;
    memset(&out, 0, sizeof out);
    const u64 start = nph_time_now_ns();
    u32 quiet = 0U;
    while (quiet < QUIET_FRAMES && out.frames < SETTLE_FRAMES &&
           nph_time_now_ns() - start < SETTLE_NS) {
        stream_frame(f, v);
        NphProxyStreamStats st;
        nph_proxy_stream_stats(f->stream, &st);
        out.installed += st.installed;
        out.dropped += st.dropped;
        out.building = nph_max_u32(out.building, st.building);
        quiet = st.loading == 0U && st.installed == 0U && st.dropped == 0U ? quiet + 1U : 0U;
        ++out.frames;
    }
    NTEST_LOG(
        "settled in %u frames: %u levels given, %u dropped, at most %u made on the jobs at once",
        out.frames, out.installed, out.dropped, out.building);
    NTEST_ASSERT_EQ_UINT(quiet, QUIET_FRAMES);
    return out;
}

// The finest level each of cells [first, first + count) holds, the
// coarsest and finest of them.
static void held_range(const StreamWorld *f, u32 first, u32 count, u32 *finest, u32 *coarsest)
{
    *finest = NPH_U32_MAX;
    *coarsest = 0U;
    for (u32 k = first; k < first + count; ++k) {
        const NphProxyStreamCellState cs = nph_proxy_stream_cell_state(f->stream, k);
        *finest = nph_min_u32(*finest, cs.held);
        *coarsest = nph_max_u32(*coarsest, cs.held);
    }
}

static u32 differing_pixels(const u8 *a, const u8 *b)
{
    u32 n = 0U;
    for (u64 p = 0; p < TARGET_PIXELS; ++p) {
        n += memcmp(a + p * 4U, b + p * 4U, 4U) != 0 ? 1U : 0U;
    }
    return n;
}

static u32 covered_pixels(const u8 *image)
{
    u32 n = 0U;
    for (u64 p = 0; p < TARGET_PIXELS; ++p) {
        n += (image[p * 4U] | image[p * 4U + 1U] | image[p * 4U + 2U]) != 0U ? 1U : 0U;
    }
    return n;
}

// From where every cell asks for its finest level, the stream gives each
// cell its levels one by one, coarse to fine (asked first, from the camera,
// one level coarser than the cut, then as the views ask), and settles
// holding them all; what is drawn is then the cells given every level
// directly. Destroyed, the stream gives every cell back its coarsest alone:
// the image drawn before it began.
NTEST_F(StreamWorld, cells_stream_in_the_levels_the_views_ask_for)
{
    REQUIRE_PRODUCTION(f);
    world_open(f);
    place_grove(f, 0.0);
    const NphDVec3 eye = near_eye();
    const NphRenderView v =
        grove_view(eye, grove_centre(0.0), NARROW_FOV, fine_threshold(f, eye, NARROW_FOV));
    NTEST_LOG("threshold %.3f px; cells' levels: %u, errors %.4f %.4f %.4f", (f64)v.lod_error,
              f->stream_cells[0].level_count, (f64)f->levels[0][0].error,
              (f64)f->levels[0][1].error, (f64)f->levels[0][2].error);
    u8 *coarsest = NPH_ALLOC_ARRAY(f->alloc, u8, TARGET_PIXELS * 4U);
    u8 *streamed = NPH_ALLOC_ARRAY(f->alloc, u8, TARGET_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(coarsest);
    NTEST_ASSERT_NOT_NULL(streamed);

    // The cells with their coarsest levels alone.
    draw(f, &v);
    memcpy(coarsest, f->pixels, TARGET_PIXELS * 4U);
    NTEST_ASSERT_GT_UINT(covered_pixels(coarsest), 0U);

    // Streamed until settled: every cell holds every level, as asked, each
    // made on the jobs in the voxel pool.
    u64 pool = 0U;
    u64 pool_size = 0U;
    nph_render_voxel_pool_usage(f->rf.renderer, &pool, &pool_size);
    const u64 coarsest_bytes = pool; // the cells' coarsest levels, made at once
    NTEST_EXPECT_TRUE(coarsest_bytes != 0U);
    stream_make(f, 0U, false);
    const Settled s = settle(f, &v);
    NTEST_EXPECT_GT_UINT(s.building, 0U);
    NphProxyStreamStats st;
    nph_proxy_stream_stats(f->stream, &st);
    u32 levels = 0U;
    for (u32 k = 0; k < f->cell_count; ++k) {
        const NphProxyStreamCellState cs = nph_proxy_stream_cell_state(f->stream, k);
        NTEST_EXPECT_EQ_UINT(cs.held, 0U);
        NTEST_EXPECT_EQ_UINT(cs.wanted, 0U);
        NTEST_EXPECT_FALSE(cs.loading);
        NTEST_EXPECT_FALSE(cs.failed);
        NTEST_EXPECT_EQ_UINT(nph_render_cell_request(f->rf.renderer, f->cells[k]), 0U);
        levels += f->stream_cells[k].level_count - 1U;
    }
    NTEST_EXPECT_EQ_UINT(st.cells, f->cell_count);
    NTEST_EXPECT_EQ_UINT(st.held_levels, levels);
    NTEST_EXPECT_EQ_UINT(s.installed, levels); // each once
    NTEST_EXPECT_EQ_UINT(s.dropped, 0U);
    NTEST_EXPECT_EQ_UINT(st.asked, f->cell_count);
    NTEST_EXPECT_EQ_UINT(st.short_cells, 0U);
    NTEST_EXPECT_EQ_UINT(st.failed, 0U);
    NTEST_EXPECT_TRUE(st.held_bytes > 0U);
    draw(f, &v);
    memcpy(streamed, f->pixels, TARGET_PIXELS * 4U);
    NTEST_LOG("pixels covered: %u with the coarsest levels, %u with every level",
              covered_pixels(coarsest), covered_pixels(streamed));
    NTEST_EXPECT_GT_UINT(differing_pixels(coarsest, streamed), 0U);

    nph_render_voxel_pool_usage(f->rf.renderer, &pool, &pool_size);
    NTEST_LOG("the voxel pool: %.1f KB of %.1f MB, the coarsest levels %.1f KB of it",
              (f64)pool / 1024.0, (f64)pool_size / 1048576.0, (f64)coarsest_bytes / 1024.0);
    NTEST_EXPECT_TRUE(pool > coarsest_bytes);

    // Gone, the stream leaves every cell its coarsest alone, and the pool
    // the coarsest levels once the frames in flight are done.
    stream_free(f);
    draw(f, &v);
    NTEST_EXPECT_EQ_UINT(differing_pixels(coarsest, f->pixels), 0U);
    nph_render_voxel_pool_usage(f->rf.renderer, &pool, &pool_size);
    NTEST_EXPECT_TRUE(pool == coarsest_bytes);

    // The cells given every level at once draw what the stream assembled.
    NphRenderMesh meshes[GROVE_CELLS][NPH_RENDER_CELL_MAX_LEVELS];
    for (u32 k = 0; k < f->cell_count; ++k) {
        const u32 count = f->stream_cells[k].level_count;
        for (u32 l = 0; l + 1U < count; ++l) {
            meshes[k][l] = level_mesh(f, f->levels[k][l].mesh);
        }
        meshes[k][count - 1U] = f->coarsest[k];
        NTEST_ASSERT_EQ_INT(
            nph_render_cell_set_proxy(f->rf.renderer, f->cells[k], meshes[k], count),
            NPH_RESULT_OK);
    }
    draw(f, &v);
    NTEST_EXPECT_EQ_UINT(differing_pixels(streamed, f->pixels), 0U);
    for (u32 k = 0; k < f->cell_count; ++k) {
        const u32 count = f->stream_cells[k].level_count;
        NTEST_ASSERT_EQ_INT(
            nph_render_cell_set_proxy(f->rf.renderer, f->cells[k], &f->coarsest[k], 1U),
            NPH_RESULT_OK);
        for (u32 l = 0; l + 1U < count; ++l) {
            nph_render_mesh_destroy(f->rf.renderer, meshes[k][l]);
        }
    }
    NPH_FREE_ARRAY(f->alloc, u8, streamed, TARGET_PIXELS * 4U);
    NPH_FREE_ARRAY(f->alloc, u8, coarsest, TARGET_PIXELS * 4U);
}

// Within a budget the stream holds the levels worth most: of two groves,
// one three times nearer than the other, the nearer's cells are held at
// least as fine as the farther's, and once settled nothing is loaded or
// dropped while the camera stays. Turned to look at the other grove from as
// near, the stream trades their levels, dropping what it gives up as it goes
// (the farther's finer levels, spare there), and settles within the budget
// the other way round, giving each level once.
NTEST_F(StreamWorld, within_a_budget_the_levels_worth_most_are_held)
{
    REQUIRE_PRODUCTION(f);
    world_open(f);
    place_grove(f, 0.0);          // cells 0 to 3
    place_grove(f, -GROVE_APART); // cells 4 to 7
    const NphDVec3 eye = near_eye();
    const f32 threshold = fine_threshold(f, eye, WIDE_FOV);
    const NphRenderView v = grove_view(eye, grove_centre(0.0), WIDE_FOV, threshold);
    // The far side, mirrored: the second grove as near, the first beyond it.
    const NphDVec3 far_eye = nph_dvec3(8.0, 40.0, 8.0 - GROVE_APART - GROVE_VIEW);
    const NphRenderView back = grove_view(far_eye, grove_centre(-GROVE_APART), WIDE_FOV, threshold);

    // Unlimited, the levels the views ask for and their bytes.
    stream_make(f, 0U, false);
    settle(f, &v);
    NphProxyStreamStats all;
    nph_proxy_stream_stats(f->stream, &all);
    u32 near_finest = 0U;
    u32 near_coarsest = 0U;
    u32 far_finest = 0U;
    u32 far_coarsest = 0U;
    held_range(f, 0U, GROVE_CELLS, &near_finest, &near_coarsest);
    held_range(f, GROVE_CELLS, GROVE_CELLS, &far_finest, &far_coarsest);
    NTEST_LOG("unlimited: %u levels held, %.1f KB; the near grove from level %u to %u, the far "
              "from %u to %u",
              all.held_levels, (f64)all.held_bytes / 1024.0, near_finest, near_coarsest, far_finest,
              far_coarsest);
    NTEST_EXPECT_EQ_UINT(near_coarsest, 0U);
    NTEST_EXPECT_GT_UINT(far_finest, 0U); // the far grove asks for less
    stream_free(f);

    // Half of that.
    const u64 budget = all.held_bytes / 2U;
    stream_make(f, budget, false);
    settle(f, &v);
    NphProxyStreamStats st;
    nph_proxy_stream_stats(f->stream, &st);
    held_range(f, 0U, GROVE_CELLS, &near_finest, &near_coarsest);
    held_range(f, GROVE_CELLS, GROVE_CELLS, &far_finest, &far_coarsest);
    NTEST_LOG("within %.1f KB: %u levels held, %.1f KB; the near grove from level %u to %u, the "
              "far from %u to %u",
              (f64)budget / 1024.0, st.held_levels, (f64)st.held_bytes / 1024.0, near_finest,
              near_coarsest, far_finest, far_coarsest);
    NTEST_EXPECT_TRUE(st.held_bytes <= budget);
    NTEST_EXPECT_GT_UINT(st.held_levels, 0U);
    NTEST_EXPECT_LE_UINT(near_coarsest, far_finest);
    // Still, it stays still.
    for (u32 i = 0; i < 30U; ++i) {
        stream_frame(f, &v);
        nph_proxy_stream_stats(f->stream, &st);
        NTEST_ASSERT_EQ_UINT(st.installed + st.dropped + st.loading, 0U);
    }

    // Turned round: the other way.
    u32 before[MAX_CELLS];
    for (u32 k = 0; k < f->cell_count; ++k) {
        before[k] = nph_proxy_stream_cell_state(f->stream, k).held;
    }
    const Settled turned = settle(f, &back);
    nph_proxy_stream_stats(f->stream, &st);
    u32 gained = 0U;
    for (u32 k = 0; k < f->cell_count; ++k) {
        const u32 after = nph_proxy_stream_cell_state(f->stream, k).held;
        gained += after < before[k] ? before[k] - after : 0U;
    }
    held_range(f, 0U, GROVE_CELLS, &far_finest, &far_coarsest);
    held_range(f, GROVE_CELLS, GROVE_CELLS, &near_finest, &near_coarsest);
    NTEST_LOG("turned: %u levels held, %.1f KB; the near grove from level %u to %u, the far from "
              "%u to %u; %u levels gained, %u given, %u dropped",
              st.held_levels, (f64)st.held_bytes / 1024.0, near_finest, near_coarsest, far_finest,
              far_coarsest, gained, turned.installed, turned.dropped);
    NTEST_EXPECT_TRUE(st.held_bytes <= budget);
    NTEST_EXPECT_LE_UINT(near_coarsest, far_finest);
    NTEST_EXPECT_GT_UINT(gained, 0U);
    NTEST_EXPECT_EQ_UINT(turned.installed, gained); // none given twice
    for (u32 i = 0; i < 30U; ++i) {
        stream_frame(f, &back);
        nph_proxy_stream_stats(f->stream, &st);
        NTEST_ASSERT_EQ_UINT(st.installed + st.dropped + st.loading, 0U);
    }
    NTEST_EXPECT_EQ_UINT(st.failed, 0U);
}

// A level that cannot be had leaves its cell as it is, logged and counted,
// and the others stream on: one cell names an asset no pack holds, another
// a mesh that is not a level apart (its own head). The levels are made on
// the main thread here (no jobs given), as without a voxel pool. And the
// stream's arguments are checked.
NTEST_F(StreamWorld, a_level_that_fails_leaves_its_cell_as_it_is)
{
    REQUIRE_PRODUCTION(f);
    world_open(f);
    place_grove(f, 0.0);
    const NphDVec3 eye = near_eye();
    const NphRenderView v =
        grove_view(eye, grove_centre(0.0), NARROW_FOV, fine_threshold(f, eye, NARROW_FOV));
    // Cell 0's finest level is nowhere; cell 1's next to its coarsest is its
    // head.
    f->levels[0][0].mesh = nph_guid_from_cstr("grove.gltf#nowhere");
    const u32 next = f->stream_cells[1].level_count - 2U;
    f->levels[1][next].mesh = f->heads[1];

    // The arguments.
    NphProxyStreamDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = f->alloc;
    d.renderer = f->rf.renderer;
    d.loader = f->loader;
    NphProxyStream *s = NULL;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_create(&d, &s), NPH_RESULT_INVALID_ARGUMENT); // no cells
    NTEST_EXPECT_NULL(s);
    d.max_cells = 1U;
    d.loader = NULL;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_create(&d, &s), NPH_RESULT_INVALID_ARGUMENT);
    d.loader = f->loader;
    NTEST_ASSERT_EQ_INT(nph_proxy_stream_create(&d, &s), NPH_RESULT_OK);
    NphProxyStreamCell bad = f->stream_cells[2];
    bad.level_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad, NULL), NPH_RESULT_INVALID_ARGUMENT);
    bad.level_count = NPH_RENDER_CELL_MAX_LEVELS + 1U;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad, NULL), NPH_RESULT_INVALID_ARGUMENT);
    bad = f->stream_cells[2];
    bad.cell = NPH_RENDER_CELL_NULL;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad, NULL), NPH_RESULT_INVALID_ARGUMENT);
    bad = f->stream_cells[2];
    bad.coarsest = NPH_RENDER_MESH_NULL;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad, NULL), NPH_RESULT_INVALID_ARGUMENT);
    bad = f->stream_cells[2];
    bad.distance = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NphMeshLevel nameless[NPH_RENDER_CELL_MAX_LEVELS];
    memcpy(nameless, f->levels[2], sizeof nameless);
    nameless[0].mesh = NPH_GUID_NULL;
    bad = f->stream_cells[2];
    bad.levels = nameless;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &f->stream_cells[2], NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &f->stream_cells[3], NULL), NPH_RESULT_FULL);
    nph_proxy_stream_destroy(s);

    // Streamed, the levels made on the main thread and then on the jobs: the
    // two cells stop short, the others hold every level.
    for (u32 pass = 0; pass < 2U; ++pass) {
        stream_make(f, 0U, pass == 0U);
        settle(f, &v);
        NphProxyStreamStats st;
        nph_proxy_stream_stats(f->stream, &st);
        NTEST_EXPECT_EQ_UINT(st.failed, 2U);
        const NphProxyStreamCellState c0 = nph_proxy_stream_cell_state(f->stream, 0U);
        NTEST_EXPECT_TRUE(c0.failed);
        NTEST_EXPECT_EQ_UINT(c0.held, 1U); // all but the one nowhere
        const NphProxyStreamCellState c1 = nph_proxy_stream_cell_state(f->stream, 1U);
        NTEST_EXPECT_TRUE(c1.failed);
        NTEST_EXPECT_EQ_UINT(c1.held, next + 1U); // its coarsest alone
        for (u32 k = 2; k < f->cell_count; ++k) {
            const NphProxyStreamCellState cs = nph_proxy_stream_cell_state(f->stream, k);
            NTEST_EXPECT_FALSE(cs.failed);
            NTEST_EXPECT_EQ_UINT(cs.held, 0U);
        }
        // Nothing more is tried.
        for (u32 i = 0; i < 10U; ++i) {
            stream_frame(f, &v);
        }
        nph_proxy_stream_stats(f->stream, &st);
        NTEST_EXPECT_EQ_UINT(st.failed, 2U);
        NTEST_EXPECT_EQ_UINT(st.loading, 0U);
        stream_free(f);
    }
}

// A voxel pool too small for the levels past the coarsest: the jobs find no
// room, and the levels are made on the main thread with buffers of their own
// (warned once), so that every cell still holds every level it is asked for;
// the pool holds what fits.
NTEST_F(TightPool, a_full_pool_makes_the_levels_on_the_main_thread)
{
    REQUIRE_PRODUCTION(f);
    world_open(f);
    place_grove(f, 0.0);
    const NphDVec3 eye = near_eye();
    const NphRenderView v =
        grove_view(eye, grove_centre(0.0), NARROW_FOV, fine_threshold(f, eye, NARROW_FOV));
    stream_make(f, 0U, false);
    const Settled s = settle(f, &v);
    NphProxyStreamStats st;
    nph_proxy_stream_stats(f->stream, &st);
    u64 pool = 0U;
    u64 size = 0U;
    nph_render_voxel_pool_usage(f->rf.renderer, &pool, &size);
    NTEST_LOG("levels held %u (%.1f KB); the pool %.1f of %.1f KB", st.held_levels,
              (f64)st.held_bytes / 1024.0, (f64)pool / 1024.0, (f64)size / 1024.0);
    u32 levels = 0U;
    for (u32 k = 0; k < f->cell_count; ++k) {
        NTEST_EXPECT_EQ_UINT(nph_proxy_stream_cell_state(f->stream, k).held, 0U);
        levels += f->stream_cells[k].level_count - 1U;
    }
    NTEST_EXPECT_EQ_UINT(st.held_levels, levels);
    NTEST_EXPECT_EQ_UINT(s.installed, levels);
    NTEST_EXPECT_EQ_UINT(st.failed, 0U);
    NTEST_EXPECT_TRUE(st.held_bytes > size); // more than the pool holds
    NTEST_EXPECT_TRUE(pool <= size);
}

// ---------------------------------------------------------------------------
// The runtime's own path (ADR-0072): the grove imported into a world, its
// WorldCells mirrored by a scene renderer, which makes the render cells and
// streams their levels, a frame at a time as the engine does.
// ---------------------------------------------------------------------------

typedef struct GroveScene {
    NphWorld *world;
    NphRuntimeComponents ids;
    NphTransformSystem *transforms;
    NphSceneRenderer *sr;
} GroveScene;

static void grove_scene_world(GroveScene *s, NphAllocator *alloc)
{
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = alloc;
    wd.max_entities = 4096U;
    wd.event_capacity = 1024U;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &s->world), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_runtime_register_components(s->world, &s->ids), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_transform_system_create(s->world, alloc, &s->transforms),
                        NPH_RESULT_OK);
}

static void grove_scene_make(StreamWorld *f, GroveScene *s, u64 budget)
{
    memset(s, 0, sizeof *s);
    grove_scene_world(s, f->alloc);
    NphSceneRendererDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = f->alloc;
    d.renderer = f->rf.renderer;
    d.loader = f->loader;
    d.world = s->world;
    d.jobs = f->jobs;
    d.max_cells = MAX_CELLS;
    d.proxy_budget_bytes = budget;
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_create(&d, &s->sr), NPH_RESULT_OK);
}

static void grove_scene_free(GroveScene *s)
{
    nph_scene_renderer_destroy(s->sr);
    nph_transform_system_destroy(s->transforms);
    nph_world_destroy(s->world);
}

// The grove under a new root at `dz`.
static NphEntity grove_import(StreamWorld *f, GroveScene *s, f64 dz, bool instances_only,
                              u32 *count)
{
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = s->world;
    d.loader = f->loader;
    d.scene = nph_guid_from_cstr("grove.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    d.root_transform.translation = nph_dvec3(0.0, 0.0, dz);
    d.name = "grove";
    d.instances_only = instances_only ? 1 : 0;
    NphEntity root = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, &root, count), NPH_RESULT_OK);
    return root;
}

// The world's cells' ids: how many there are, and the lowest and highest; and
// its mesh instances: how many there are, and how many name a cell.
typedef struct GroveCounts {
    u32 cells;
    u32 lowest_id;
    u32 highest_id;
    u32 instances;
    u32 instances_in_cells;
} GroveCounts;

static GroveCounts grove_counts(const GroveScene *s)
{
    GroveCounts out;
    memset(&out, 0, sizeof out);
    out.lowest_id = NPH_U32_MAX;
    for (u32 i = 0; i < nph_world_entity_slot_count(s->world); ++i) {
        const NphEntity e = nph_world_entity_at(s->world, i);
        if (nph_entity_is_null(e)) {
            continue;
        }
        const NphWorldCell *w = (const NphWorldCell *)nph_world_get(s->world, e, s->ids.world_cell);
        if (w != NULL) {
            out.cells += 1U;
            out.lowest_id = nph_min_u32(out.lowest_id, w->id);
            out.highest_id = nph_max_u32(out.highest_id, w->id);
        }
        const NphMeshRenderer *m =
            (const NphMeshRenderer *)nph_world_get(s->world, e, s->ids.mesh_renderer);
        if (m != NULL) {
            out.instances += 1U;
            out.instances_in_cells += m->cell != 0U ? 1U : 0U;
        }
    }
    return out;
}

// One frame as the engine runs it: the world's transforms, the sync, the
// streams, and the draw.
static NphRenderFrameStats grove_frame(StreamWorld *f, GroveScene *s, const NphRenderView *v,
                                       NphSceneRenderStats *out_scene)
{
    RenderFixture *rf = &f->rf;
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(rf->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(s->transforms, f->jobs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_sync(s->sr, out_scene), NPH_RESULT_OK);
    nph_world_clear_events(s->world);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_stream(s->sr, v, TARGET_SIZE), NPH_RESULT_OK);
    nph_gpu_rg_begin(rf->graph);
    NTEST_ASSERT_EQ_INT(nph_render_frame(rf->renderer, v, rf->graph, f->target, NULL, &stats),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(rf->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(rf->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(rf->dev), NPH_RESULT_OK);
    return stats;
}

// Frames until the cells are made, every instance is, and the stream is quiet.
static NphRenderFrameStats grove_settle(StreamWorld *f, GroveScene *s, const NphRenderView *v,
                                        NphSceneRenderStats *out_scene)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    const u64 start = nph_time_now_ns();
    u32 quiet = 0U;
    u32 frames = 0U;
    while (quiet < QUIET_FRAMES && frames < SETTLE_FRAMES &&
           nph_time_now_ns() - start < SETTLE_NS) {
        stats = grove_frame(f, s, v, out_scene);
        const NphProxyStream *ps = nph_scene_renderer_proxy_stream(s->sr);
        NphProxyStreamStats st;
        memset(&st, 0, sizeof st);
        if (ps != NULL) {
            nph_proxy_stream_stats(ps, &st);
        }
        const bool still = st.loading == 0U && st.installed == 0U && st.dropped == 0U &&
                           out_scene->pending == 0U && out_scene->cells_loading == 0U &&
                           out_scene->assets_loading == 0U;
        quiet = still ? quiet + 1U : 0U;
        ++frames;
    }
    NTEST_ASSERT_EQ_UINT(quiet, QUIET_FRAMES);
    return stats;
}

// The world imported, the scene renderer makes each cell with its coarsest
// level, holds its instances until then, and streams the levels the views ask
// for: the image is the one the harness's own cells and stream draw. Nearer than
// their distance the instances are drawn instead. A tile more makes four cells
// more, of ids of their own.
NTEST_F(StreamWorld, a_worlds_cells_come_from_the_scene_renderer_as_the_harness_makes_them)
{
    REQUIRE_PRODUCTION(f);
    world_open(f);
    u8 *direct = NPH_ALLOC_ARRAY(f->alloc, u8, TARGET_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(direct);
    place_grove(f, 0.0);
    const NphDVec3 eye = near_eye();
    const NphRenderView v =
        grove_view(eye, grove_centre(0.0), NARROW_FOV, fine_threshold(f, eye, NARROW_FOV));
    stream_make(f, 0U, false);
    (void)settle(f, &v);
    draw(f, &v);
    memcpy(direct, f->pixels, TARGET_PIXELS * 4U);
    u32 levels = 0U;
    for (u32 k = 0; k < f->cell_count; ++k) {
        levels += f->stream_cells[k].level_count - 1U;
    }
    // The harness's cells go: the renderer's capacity is the test's.
    stream_free(f);
    for (u32 k = 0; k < f->cell_count; ++k) {
        nph_render_cell_destroy(f->rf.renderer, f->cells[k]);
        nph_render_mesh_destroy(f->rf.renderer, f->coarsest[k]);
    }
    f->cell_count = 0U;

    GroveScene s;
    grove_scene_make(f, &s, 0U);
    u32 entities = 0U;
    const NphEntity root = grove_import(f, &s, 0.0, false, &entities);
    NTEST_EXPECT_FALSE(nph_entity_is_null(root));
    const GroveCounts counts = grove_counts(&s);
    NTEST_LOG("%u entities: %u cells, ids %u to %u; %u mesh instances, %u in cells", entities,
              counts.cells, counts.lowest_id, counts.highest_id, counts.instances,
              counts.instances_in_cells);
    NTEST_EXPECT_EQ_UINT(counts.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(counts.lowest_id, 1U);
    NTEST_EXPECT_EQ_UINT(counts.highest_id, GROVE_CELLS);
    NTEST_EXPECT_GT_UINT(counts.instances, 0U);
    // All but the ground, which voxelizes to nothing and so stands in no cell.
    NTEST_EXPECT_EQ_UINT(counts.instances_in_cells, counts.instances - 1U);
    // The harness drew the cells alone: the ground is hidden here too.
    for (u32 i = 0; i < nph_world_entity_slot_count(s.world); ++i) {
        const NphEntity e = nph_world_entity_at(s.world, i);
        NphMeshRenderer *m =
            nph_entity_is_null(e)
                ? NULL
                : NPH_WORLD_GET_MUT(s.world, e, s.ids.mesh_renderer, NphMeshRenderer);
        if (m != NULL && m->cell == 0U) {
            m->hidden = 1;
        }
    }

    NphSceneRenderStats scene;
    NphRenderFrameStats stats = grove_settle(f, &s, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(scene.cells_loading, 0U);
    NTEST_EXPECT_EQ_UINT(scene.instances, counts.instances);
    NTEST_EXPECT_EQ_UINT(scene.failed, 0U);
    NTEST_EXPECT_EQ_UINT(scene.proxy_levels, levels);
    NTEST_EXPECT_GT_UINT(stats.vis_cells_proxied, 0U);
    for (u32 id = 1U; id <= GROVE_CELLS; ++id) {
        NTEST_EXPECT_TRUE(
            nph_render_cell_is_valid(f->rf.renderer, nph_scene_renderer_cell(s.sr, id)));
    }
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_cell(s.sr, GROVE_CELLS + 1U).value, 0U);
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        (void)grove_frame(f, &s, &v, &scene);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
    test_read_image(f->rf.dev, f->target, 0U, f->pixels, TARGET_PIXELS * 4U);
    const u32 differ = differing_pixels(direct, f->pixels);
    NTEST_LOG("the harness's image and the scene renderer's: %u of %llu pixels differ, %u covered",
              differ, (unsigned long long)TARGET_PIXELS, covered_pixels(f->pixels));
    NTEST_EXPECT_GT_UINT(covered_pixels(f->pixels), 0U);
    NTEST_EXPECT_EQ_UINT(differ, 0U);

    // Near, inside their distance: the instances themselves.
    const NphDVec3 close_eye = nph_dvec3(8.0, 20.0, 40.0);
    const NphRenderView near_view = grove_view(close_eye, grove_centre(0.0), 0.4F, 1.0F);
    (void)grove_frame(f, &s, &near_view, &scene);
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        stats = grove_frame(f, &s, &near_view, &scene);
    }
    NTEST_EXPECT_EQ_UINT(stats.vis_cells_proxied, 0U);
    NTEST_EXPECT_GT_UINT(stats.vis_clusters, 0U);

    // A second tile, its cells and instances its own.
    (void)grove_import(f, &s, -GROVE_APART, true, NULL);
    const GroveCounts two = grove_counts(&s);
    NTEST_EXPECT_EQ_UINT(two.cells, 2U * GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(two.highest_id, 2U * GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(two.instances, 2U * counts.instances);
    (void)grove_settle(f, &s, &near_view, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, 2U * GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(scene.instances, two.instances);

    // Its proxies stand where it does: seen from as far as the first was, the
    // same view moved with it, they cover what the first's covered. (Its ground is
    // hidden too, and the first tile is behind that eye.)
    for (u32 i = 0; i < nph_world_entity_slot_count(s.world); ++i) {
        const NphEntity e = nph_world_entity_at(s.world, i);
        NphMeshRenderer *m =
            nph_entity_is_null(e)
                ? NULL
                : NPH_WORLD_GET_MUT(s.world, e, s.ids.mesh_renderer, NphMeshRenderer);
        if (m != NULL && m->cell == 0U) {
            m->hidden = 1;
        }
    }
    const NphDVec3 moved_eye = nph_dvec3(eye.x, eye.y, eye.z - GROVE_APART);
    const NphRenderView moved =
        grove_view(moved_eye, grove_centre(-GROVE_APART), NARROW_FOV, v.lod_error);
    stats = grove_settle(f, &s, &moved, &scene);
    NTEST_EXPECT_GT_UINT(stats.vis_cells_proxied, 0U);
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        (void)grove_frame(f, &s, &moved, &scene);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
    test_read_image(f->rf.dev, f->target, 0U, f->pixels, TARGET_PIXELS * 4U);
    const u32 first_covers = covered_pixels(direct);
    const u32 second_covers = covered_pixels(f->pixels);
    NTEST_LOG("the first tile's proxies cover %u pixels, the second's %u", first_covers,
              second_covers);
    NTEST_EXPECT_GT_UINT(second_covers, first_covers - first_covers / 50U);
    NTEST_EXPECT_LT_UINT(second_covers, first_covers + first_covers / 50U);
    grove_scene_free(&s);
    NPH_FREE_ARRAY(f->alloc, u8, direct, TARGET_PIXELS * 4U);
}

// A cell's entity gone takes its render cell and proxy with it, while its
// instances live on, drawn as themselves; its root gone takes everything. A new
// world to mirror starts afresh, a cell's proxy that does not load leaves its
// instances drawn as themselves, and a cell turned with its parent cannot follow.
NTEST_F(StreamWorld, cells_go_with_their_entities_and_leave_their_instances)
{
    REQUIRE_PRODUCTION(f);
    world_open(f);
    GroveScene s;
    grove_scene_make(f, &s, 0U);
    const NphEntity root = grove_import(f, &s, 0.0, false, NULL);
    const GroveCounts counts = grove_counts(&s);
    const NphDVec3 eye = near_eye();
    const NphRenderView v = grove_view(eye, grove_centre(0.0), NARROW_FOV, 1.0F);
    NphSceneRenderStats scene;
    (void)grove_settle(f, &s, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(scene.instances, counts.instances);

    // The first cell's entity: its proxy goes, its instances are made anew.
    NphEntity first = NPH_ENTITY_NULL;
    for (u32 i = 0; i < nph_world_entity_slot_count(s.world) && nph_entity_is_null(first); ++i) {
        const NphEntity e = nph_world_entity_at(s.world, i);
        const NphWorldCell *w =
            nph_entity_is_null(e)
                ? NULL
                : (const NphWorldCell *)nph_world_get(s.world, e, s.ids.world_cell);
        first = w != NULL && w->id == 1U ? e : first;
    }
    NTEST_ASSERT_FALSE(nph_entity_is_null(first));
    NTEST_ASSERT_EQ_INT(nph_world_despawn(s.world, first), NPH_RESULT_OK);
    const NphRenderFrameStats stats = grove_settle(f, &s, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS - 1U);
    NTEST_EXPECT_EQ_UINT(scene.instances, counts.instances);
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_cell(s.sr, 1U).value, 0U);
    NTEST_LOG("one cell gone: %u cells proxied, %u clusters", stats.vis_cells_proxied,
              stats.vis_clusters);

    // Its cell's id taken by a new cell (changed in place, it goes and comes).
    NphEntity second = NPH_ENTITY_NULL;
    for (u32 i = 0; i < nph_world_entity_slot_count(s.world) && nph_entity_is_null(second); ++i) {
        const NphEntity e = nph_world_entity_at(s.world, i);
        const NphWorldCell *w =
            nph_entity_is_null(e)
                ? NULL
                : (const NphWorldCell *)nph_world_get(s.world, e, s.ids.world_cell);
        second = w != NULL && w->id == 2U ? e : second;
    }
    NTEST_ASSERT_FALSE(nph_entity_is_null(second));
    const NphRenderCell before_change = nph_scene_renderer_cell(s.sr, 2U);
    NTEST_EXPECT_TRUE(nph_render_cell_is_valid(f->rf.renderer, before_change));
    NphWorldCell *changing = NPH_WORLD_GET_MUT(s.world, second, s.ids.world_cell, NphWorldCell);
    NTEST_ASSERT_NOT_NULL(changing);
    changing->proxy_distance *= 2.0F;
    (void)grove_settle(f, &s, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS - 1U);
    NTEST_EXPECT_EQ_UINT(scene.instances, counts.instances);
    const NphRenderCell after_change = nph_scene_renderer_cell(s.sr, 2U);
    NTEST_EXPECT_TRUE(nph_render_cell_is_valid(f->rf.renderer, after_change));
    NTEST_EXPECT_FALSE(nph_render_cell_is_valid(f->rf.renderer, before_change));
    NTEST_EXPECT_NE_UINT(after_change.value, before_change.value);

    // The rest go with the root, at once.
    NTEST_ASSERT_EQ_INT(nph_world_despawn_recursive(s.world, root), NPH_RESULT_OK);
    (void)grove_frame(f, &s, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, 0U);
    NTEST_EXPECT_EQ_UINT(scene.instances, 0U);
    NphProxyStreamStats st;
    nph_proxy_stream_stats(nph_scene_renderer_proxy_stream(s.sr), &st);
    NTEST_EXPECT_EQ_UINT(st.cells, 0U);
    NTEST_EXPECT_EQ_UINT(st.held_levels, 0U);
    NTEST_EXPECT_TRUE(st.held_bytes == 0U);

    // Imported again: the stream takes them in the slots they left.
    (void)grove_import(f, &s, 0.0, false, NULL);
    (void)grove_settle(f, &s, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(scene.instances, counts.instances);

    // Another world: the scene renderer's cells go, and that world's come.
    GroveScene other;
    memset(&other, 0, sizeof other);
    grove_scene_world(&other, f->alloc);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_set_world(s.sr, other.world), NPH_RESULT_OK);
    NphSceneRenderer *held = s.sr;
    other.sr = held;
    s.sr = NULL;
    (void)grove_import(f, &other, 0.0, false, NULL);
    (void)grove_settle(f, &other, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(scene.instances, counts.instances);

    // A root that turns its scene takes no cells.
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = other.world;
    d.loader = f->loader;
    d.scene = nph_guid_from_cstr("grove.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    d.root_transform.rotation = nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.5F);
    d.instances_only = 1;
    NphEntity turned = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, &turned, NULL), NPH_RESULT_OK);
    const GroveCounts after = grove_counts(&other);
    NTEST_EXPECT_EQ_UINT(after.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(after.instances, 2U * counts.instances);
    NTEST_EXPECT_EQ_UINT(after.instances_in_cells, counts.instances_in_cells);

    // A cell whose proxy names nothing: its instances wait for nothing.
    NphEntity orphan;
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(other.world, comps, 3U, &orphan), NPH_RESULT_OK);
    NphWorldCell w;
    nph_component_init_default(NphWorldCell_component(), &w);
    w.id = 999U;
    w.bounds_max = nph_vec3(1.0F, 1.0F, 1.0F);
    w.proxy = nph_guid_from_cstr("no such proxy");
    NTEST_ASSERT_EQ_INT(nph_world_add(other.world, orphan, other.ids.world_cell, &w),
                        NPH_RESULT_OK);
    NphEntity member;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(other.world, comps, 3U, &member), NPH_RESULT_OK);
    NphMeshRenderer m;
    memset(&m, 0, sizeof m);
    bool sampled = false;
    for (u32 i = 0; i < nph_world_entity_slot_count(other.world) && !sampled; ++i) {
        const NphEntity e = nph_world_entity_at(other.world, i);
        const NphMeshRenderer *sample =
            nph_entity_is_null(e)
                ? NULL
                : (const NphMeshRenderer *)nph_world_get(other.world, e, other.ids.mesh_renderer);
        if (sample != NULL) {
            m = *sample;
            sampled = true;
        }
    }
    NTEST_ASSERT_TRUE(sampled);
    m.cell = 999U;
    NTEST_ASSERT_EQ_INT(nph_world_add(other.world, member, other.ids.mesh_renderer, &m),
                        NPH_RESULT_OK);
    (void)grove_settle(f, &other, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(scene.instances, 2U * counts.instances + 1U);
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_cell(other.sr, 999U).value, 0U);

    // A cell that is turned (a scene file's, or a game's), whose proxy would load:
    // its box cannot follow, so it makes no render cell.
    NphWorldCell real;
    memset(&real, 0, sizeof real);
    bool found_cell = false;
    for (u32 i = 0; i < nph_world_entity_slot_count(other.world) && !found_cell; ++i) {
        const NphEntity e = nph_world_entity_at(other.world, i);
        const NphWorldCell *c =
            nph_entity_is_null(e)
                ? NULL
                : (const NphWorldCell *)nph_world_get(other.world, e, other.ids.world_cell);
        if (c != NULL && !nph_guid_is_null(c->proxy)) {
            real = *c;
            found_cell = true;
        }
    }
    NTEST_ASSERT_TRUE(found_cell);
    real.id = 998U;
    NphEntity turned_cell;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(other.world, comps, 3U, &turned_cell), NPH_RESULT_OK);
    NphLocalTransform *turned_at = NPH_WORLD_GET_MUT(
        other.world, turned_cell, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    NTEST_ASSERT_NOT_NULL(turned_at);
    turned_at->rotation = nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.5F);
    NTEST_ASSERT_EQ_INT(nph_world_add(other.world, turned_cell, other.ids.world_cell, &real),
                        NPH_RESULT_OK);
    (void)grove_settle(f, &other, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS);
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_cell(other.sr, 998U).value, 0U);

    grove_scene_free(&other);
    if (s.sr != NULL) {
        nph_scene_renderer_destroy(s.sr);
    }
    nph_transform_system_destroy(s.transforms);
    nph_world_destroy(s.world);
}

// The budget holds through the scene renderer as through the stream itself.
NTEST_F(StreamWorld, the_scene_renderers_stream_keeps_its_budget)
{
    REQUIRE_PRODUCTION(f);
    world_open(f);
    GroveScene s;
    grove_scene_make(f, &s, 32U << 10U);
    (void)grove_import(f, &s, 0.0, false, NULL);
    const NphDVec3 eye = near_eye();
    const NphRenderView v = grove_view(eye, grove_centre(0.0), NARROW_FOV, 0.01F);
    NphSceneRenderStats scene;
    (void)grove_settle(f, &s, &v, &scene);
    (void)grove_frame(f, &s, &v, &scene);
    NTEST_LOG("within 32 KiB: %u levels, %llu bytes", scene.proxy_levels,
              (unsigned long long)scene.proxy_bytes);
    NTEST_EXPECT_EQ_UINT(scene.cells, GROVE_CELLS);
    NTEST_EXPECT_TRUE(scene.proxy_bytes <= (32U << 10U));
    grove_scene_free(&s);
}

// ---------------------------------------------------------------------------
// The texture stream through the scene renderer (ADR-0072)
// ---------------------------------------------------------------------------

#define PLATE_TEXTURE 512U

// A quad of 4 m at the origin facing +z, with a checker of its own at every
// texel's level: a cooked texture large enough to have pages.
static void write_plate(const TempDir *dir)
{
    static u8 rgba[PLATE_TEXTURE * PLATE_TEXTURE * 4U];
    for (u32 y = 0; y < PLATE_TEXTURE; ++y) {
        for (u32 x = 0; x < PLATE_TEXTURE; ++x) {
            u8 *t = &rgba[((u64)y * PLATE_TEXTURE + x) * 4U];
            const bool on = ((x >> 3U) + (y >> 3U)) % 2U == 0U;
            t[0] = (u8)(x >> 1U);
            t[1] = (u8)(y >> 1U);
            t[2] = on ? 220U : 30U;
            t[3] = 255U;
        }
    }
    static u8 tga[18U + PLATE_TEXTURE * PLATE_TEXTURE * 4U];
    const u64 tga_len = test_write_tga(rgba, PLATE_TEXTURE, PLATE_TEXTURE, tga);
    char path[1200];
    test_path_in(dir, "plate.tga", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, tga, tga_len), NPH_RESULT_OK);
    const f32 positions[12] = {-2.0F, -2.0F, 0.0F, 2.0F,  -2.0F, 0.0F,
                               2.0F,  2.0F,  0.0F, -2.0F, 2.0F,  0.0F};
    const f32 normals[12] = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
                             0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    const f32 uvs[8] = {0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F};
    const u16 indices[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    u8 buffer[sizeof positions + sizeof normals + sizeof uvs + sizeof indices];
    memcpy(buffer, positions, sizeof positions);
    memcpy(buffer + 48U, normals, sizeof normals);
    memcpy(buffer + 96U, uvs, sizeof uvs);
    memcpy(buffer + 128U, indices, sizeof indices);
    test_path_in(dir, "plate.bin", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, buffer, sizeof buffer), NPH_RESULT_OK);
    static const char document[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"name\":\"plate\",\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
        "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":[{\"source\":0}],\"images\":[{\"uri\":\"plate.tga\"}],"
        "\"buffers\":[{\"uri\":\"plate.bin\",\"byteLength\":140}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":32},"
        "{\"buffer\":0,\"byteOffset\":128,\"byteLength\":12}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
        "\"min\":[-2,-2,0],\"max\":[2,2,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}";
    test_path_in(dir, "plate.gltf", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, document, sizeof document - 1U), NPH_RESULT_OK);
}

// Cooks `name` from `dir` into a pack beside it, its path in `pack`.
static void cook_pack(const TempDir *dir, const char *name, NphJobSystem *jobs, char *pack, u64 cap)
{
    char gltf[1200];
    char out_dir[1200];
    test_path_in(dir, name, gltf, sizeof gltf);
    test_path_in(dir, "cooked", out_dir, sizeof out_dir);
    test_path_in(dir, "scene.npk", pack, cap);
    CookerDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.alloc = nph_allocator_system();
    cd.out_dir = out_dir;
    cd.source_root = dir->dir;
    cd.jobs = jobs;
    cook_texture_settings_default(&cd.texture);
    cd.texture.uncompressed = 1; // quick to cook, whatever the configuration
    cook_mesh_settings_default(&cd.mesh);
    Cooker *cooker = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&cd, &cooker), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(cooker, gltf), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cooker_stats(cooker).failed, 0U);
    NTEST_ASSERT_EQ_INT(cooker_pack(cooker, pack), NPH_RESULT_OK);
    cooker_destroy(cooker);
}

// Cooks `name` from `dir` into a pack, and gives the fixture jobs, a file system and
// a loader for it.
static void cook_and_mount(StreamWorld *f, const TempDir *dir, const char *name)
{
    NphAllocator *alloc = nph_allocator_system();
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = alloc;
    jd.worker_count = 2U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &f->jobs), NPH_RESULT_OK);
    char pack[1200];
    cook_pack(dir, name, f->jobs, pack, sizeof pack);
    NTEST_ASSERT_EQ_INT(nph_vfs_create(alloc, &f->vfs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("content"), pack, NULL), NPH_RESULT_OK);
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = alloc;
    ld.vfs = f->vfs;
    ld.jobs = f->jobs;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&ld, &f->loader), NPH_RESULT_OK);
}

// With a budget the scene renderer's textures are paged and streamed: the plate's
// settles holding the levels it is seen at, and draws what its texture whole draws.
NTEST(WorldPartition, a_texture_budget_streams_a_worlds_textures_as_the_view_asks)
{
    NphAllocator *alloc = nph_allocator_system();
    StreamWorld *f = NPH_ALLOC_TYPE(alloc, StreamWorld);
    NTEST_ASSERT_NOT_NULL(f);
    world_setup(f, 0U);
    if (f->rf.dev == NULL || !nph_renderer_production_supported(f->rf.renderer) ||
        !nph_renderer_texture_paging_supported(f->rf.renderer)) {
        world_teardown(f);
        NPH_FREE_TYPE(alloc, StreamWorld, f);
        NTEST_SKIP("no paged textures on this device");
    }
    TempDir dir;
    test_temp_dir_make(&dir, "world_textures");
    write_plate(&dir);
    cook_and_mount(f, &dir, "plate.gltf");
    f->target = test_make_target(f->rf.dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, TARGET_SIZE);
    f->pixels = NPH_ALLOC_ARRAY(alloc, u8, TARGET_PIXELS * 4U);
    u8 *whole = NPH_ALLOC_ARRAY(alloc, u8, TARGET_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(f->pixels);
    NTEST_ASSERT_NOT_NULL(whole);
    NphRenderView v = grove_view(nph_dvec3(0.0, 0.0, 6.0), nph_dvec3_zero(), 0.7F, 1.0F);
    v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;

    // Twice: its texture whole, then streamed.
    for (u32 round = 0; round < 2U; ++round) {
        GroveScene s;
        memset(&s, 0, sizeof s);
        grove_scene_world(&s, alloc);
        NphSceneRendererDesc d;
        memset(&d, 0, sizeof d);
        d.alloc = alloc;
        d.renderer = f->rf.renderer;
        d.loader = f->loader;
        d.world = s.world;
        d.jobs = f->jobs;
        d.max_cells = MAX_CELLS;
        d.texture_budget_bytes = round == 1U ? (64ULL << 20U) : 0ULL;
        NTEST_ASSERT_EQ_INT(nph_scene_renderer_create(&d, &s.sr), NPH_RESULT_OK);
        NphSceneImportDesc id;
        memset(&id, 0, sizeof id);
        id.world = s.world;
        id.loader = f->loader;
        id.scene = nph_guid_from_cstr("plate.gltf#scene");
        id.root_transform = nph_local_transform_identity();
        NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&id, NULL, NULL), NPH_RESULT_OK);
        NphSceneRenderStats scene;
        (void)grove_settle(f, &s, &v, &scene);
        NTEST_EXPECT_EQ_UINT(scene.instances, 1U);
        const NphTextureStream *ts = nph_scene_renderer_texture_stream(s.sr);
        if (round == 1U) {
            NTEST_ASSERT_NOT_NULL(ts);
            NphTextureStreamStats st;
            for (u32 frame = 0; frame < SETTLE_FRAMES; ++frame) {
                (void)grove_frame(f, &s, &v, &scene);
                nph_texture_stream_stats(ts, &st);
                if (st.asked != 0U && st.loading == 0U && st.short_textures == 0U && frame > 8U) {
                    break;
                }
            }
            NTEST_LOG("the stream: %u textures, %u levels, %llu bytes", st.textures, st.held_levels,
                      (unsigned long long)st.held_bytes);
            NTEST_EXPECT_EQ_UINT(st.textures, 1U);
            NTEST_EXPECT_GT_UINT(st.held_levels, 0U);
            NTEST_EXPECT_EQ_UINT(st.short_textures, 0U);
            NTEST_EXPECT_EQ_UINT(scene.texture_levels, st.held_levels);
            NTEST_EXPECT_TRUE(scene.texture_bytes == st.held_bytes);
        } else {
            NTEST_EXPECT_NULL(ts);
        }
        for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
            (void)grove_frame(f, &s, &v, &scene);
        }
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
        test_read_image(f->rf.dev, f->target, 0U, f->pixels, TARGET_PIXELS * 4U);
        if (round == 0U) {
            memcpy(whole, f->pixels, TARGET_PIXELS * 4U);
        } else {
            NTEST_EXPECT_GT_UINT(covered_pixels(whole), (u32)(TARGET_PIXELS / 8U));
            NTEST_EXPECT_EQ_UINT(differing_pixels(whole, f->pixels), 0U);
        }
        grove_scene_free(&s);
        // A renderer's frames in flight still hold the texture: waited out.
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
    }
    NPH_FREE_ARRAY(alloc, u8, whole, TARGET_PIXELS * 4U);
    world_teardown(f);
    NPH_FREE_TYPE(alloc, StreamWorld, f);
    test_temp_dir_remove(&dir);
}

// ---------------------------------------------------------------------------
// A mesh of many materials
// ---------------------------------------------------------------------------

#define STRIP_QUADS 12U
#define STRIP_EYE   20.0

// One mesh of a row of 1 m quads along x, one primitive and one material each,
// their base colours rising in red: past a MeshRenderer's eight slots.
static void write_strip(const TempDir *dir)
{
    static f32 positions[STRIP_QUADS * 12U];
    static f32 normals[STRIP_QUADS * 12U];
    for (u32 q = 0; q < STRIP_QUADS; ++q) {
        const f32 x0 = (f32)q - (f32)STRIP_QUADS * 0.5F;
        const f32 corners[12] = {x0,        -0.5F, 0.0F, x0 + 1.0F, -0.5F, 0.0F,
                                 x0 + 1.0F, 0.5F,  0.0F, x0,        0.5F,  0.0F};
        memcpy(&positions[(u64)q * 12U], corners, sizeof corners);
        for (u32 v = 0; v < 4U; ++v) {
            normals[(u64)q * 12U + (u64)v * 3U + 2U] = 1.0F;
        }
    }
    const u16 indices[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    static u8 buffer[sizeof positions + sizeof normals + sizeof indices];
    memcpy(buffer, positions, sizeof positions);
    memcpy(buffer + sizeof positions, normals, sizeof normals);
    memcpy(buffer + sizeof positions + sizeof normals, indices, sizeof indices);
    char path[1200];
    test_path_in(dir, "strip.bin", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, buffer, sizeof buffer), NPH_RESULT_OK);

    static char doc[16384];
    u64 n = 0U;
#define APPEND(...) n += (u64)snprintf(doc + n, sizeof doc - n, __VA_ARGS__)
    APPEND("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"name\":\"strip\",\"mesh\":0}],\"meshes\":[{\"primitives\":[");
    for (u32 q = 0; q < STRIP_QUADS; ++q) {
        APPEND("%s{\"attributes\":{\"POSITION\":%u,\"NORMAL\":%u},\"indices\":%u,\"material\":%u}",
               q != 0U ? "," : "", q * 2U, q * 2U + 1U, STRIP_QUADS * 2U, q);
    }
    APPEND("]}],\"materials\":[");
    for (u32 q = 0; q < STRIP_QUADS; ++q) {
        APPEND("%s{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.4f,0.25,0.5,1]}}",
               q != 0U ? "," : "", (f64)(q + 1U) / (f64)(STRIP_QUADS + 1U) * 0.8);
    }
    APPEND("],\"buffers\":[{\"uri\":\"strip.bin\",\"byteLength\":%u}],\"bufferViews\":["
           "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u},"
           "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u},"
           "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":12}],\"accessors\":[",
           (u32)sizeof buffer, (u32)sizeof positions, (u32)sizeof positions, (u32)sizeof normals,
           (u32)(sizeof positions + sizeof normals));
    for (u32 q = 0; q < STRIP_QUADS; ++q) {
        const i32 x0 = (i32)q - (i32)(STRIP_QUADS / 2U);
        APPEND("{\"bufferView\":0,\"byteOffset\":%u,\"componentType\":5126,\"count\":4,"
               "\"type\":\"VEC3\",\"min\":[%d,-0.5,0],\"max\":[%d,0.5,0]},"
               "{\"bufferView\":1,\"byteOffset\":%u,\"componentType\":5126,\"count\":4,"
               "\"type\":\"VEC3\"},",
               q * 48U, x0, x0 + 1, q * 48U);
    }
    APPEND("{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}");
#undef APPEND
    NTEST_ASSERT_TRUE(n < sizeof doc);
    test_path_in(dir, "strip.gltf", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, doc, n), NPH_RESULT_OK);
}

// The column of quad `q`'s middle, the view `half_width` metres each way at the strip.
static u32 strip_column(u32 q, f64 half_width)
{
    const f64 x = (f64)q + 0.5 - (f64)STRIP_QUADS * 0.5;
    return (u32)((f64)TARGET_SIZE * 0.5 * (1.0 + x / half_width));
}

// The importer gives the slots past the MeshRenderer's eight a MeshMaterials, and
// the scene renderer draws them: every quad its own colour, none the default's.
NTEST(WorldPartition, a_mesh_of_many_materials_draws_each)
{
    NphAllocator *alloc = nph_allocator_system();
    StreamWorld *f = NPH_ALLOC_TYPE(alloc, StreamWorld);
    NTEST_ASSERT_NOT_NULL(f);
    world_setup_materials(f, 0U, STRIP_QUADS);
    if (f->rf.dev == NULL || !nph_renderer_production_supported(f->rf.renderer)) {
        world_teardown(f);
        NPH_FREE_TYPE(alloc, StreamWorld, f);
        NTEST_SKIP("no production path on this device");
    }
    TempDir dir;
    test_temp_dir_make(&dir, "many_materials");
    write_strip(&dir);
    cook_and_mount(f, &dir, "strip.gltf");
    f->target = test_make_target(f->rf.dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, TARGET_SIZE);
    f->pixels = NPH_ALLOC_ARRAY(alloc, u8, TARGET_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(f->pixels);

    GroveScene s;
    memset(&s, 0, sizeof s);
    grove_scene_world(&s, alloc);
    NphSceneRendererDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = alloc;
    d.renderer = f->rf.renderer;
    d.loader = f->loader;
    d.world = s.world;
    d.jobs = f->jobs;
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_create(&d, &s.sr), NPH_RESULT_OK);
    NphSceneImportDesc id;
    memset(&id, 0, sizeof id);
    id.world = s.world;
    id.loader = f->loader;
    id.scene = nph_guid_from_cstr("strip.gltf#scene");
    id.root_transform = nph_local_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&id, NULL, NULL), NPH_RESULT_OK);

    const f32 yfov = 0.7F;
    NphRenderView v = grove_view(nph_dvec3(0.0, 0.0, STRIP_EYE), nph_dvec3_zero(), yfov, 1.0F);
    v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
    NphSceneRenderStats scene;
    memset(&scene, 0, sizeof scene);
    (void)grove_settle(f, &s, &v, &scene);
    NTEST_EXPECT_EQ_UINT(scene.instances, 1U);
    NTEST_EXPECT_EQ_UINT(scene.failed, 0U);
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        (void)grove_frame(f, &s, &v, &scene);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
    test_read_image(f->rf.dev, f->target, 0U, f->pixels, TARGET_PIXELS * 4U);

    // Each quad's middle, red rising from one to the next, and the blue that says it
    // is the quad's material and not the default's white.
    const f64 half_width = tan((f64)yfov * 0.5) * STRIP_EYE;
    u32 last_red = 0U;
    for (u32 q = 0; q < STRIP_QUADS; ++q) {
        const u32 column = strip_column(q, half_width);
        const u8 *p = &f->pixels[((u64)(TARGET_SIZE / 2U) * TARGET_SIZE + column) * 4U];
        NTEST_LOG("quad %2u at column %3u: %3u %3u %3u", q, column, p[0], p[1], p[2]);
        NTEST_EXPECT_GT_UINT((u32)p[0], last_red);
        NTEST_EXPECT_TRUE(p[1] < p[2]);
        last_red = p[0];
    }

    // A slot past the eighth changed, and the MeshRenderer touched: the last quad
    // takes the first's material.
    NphEntity strip = NPH_ENTITY_NULL;
    for (u32 i = 0; i < nph_world_entity_slot_count(s.world) && nph_entity_is_null(strip); ++i) {
        const NphEntity e = nph_world_entity_at(s.world, i);
        strip = !nph_entity_is_null(e) && nph_world_get(s.world, e, s.ids.mesh_materials) != NULL
                    ? e
                    : strip;
    }
    NTEST_ASSERT_FALSE(nph_entity_is_null(strip));
    NphMeshRenderer *m = NPH_WORLD_GET_MUT(s.world, strip, s.ids.mesh_renderer, NphMeshRenderer);
    NphMeshMaterials *more =
        NPH_WORLD_GET_MUT(s.world, strip, s.ids.mesh_materials, NphMeshMaterials);
    NTEST_ASSERT_NOT_NULL(m);
    NTEST_ASSERT_NOT_NULL(more);
    nph_mesh_materials_set(more, STRIP_QUADS - NPH_MESH_RENDERER_MATERIALS - 1U, m->material0);
    (void)grove_settle(f, &s, &v, &scene);
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        (void)grove_frame(f, &s, &v, &scene);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
    test_read_image(f->rf.dev, f->target, 0U, f->pixels, TARGET_PIXELS * 4U);
    const u8 *first_quad =
        &f->pixels[((u64)(TARGET_SIZE / 2U) * TARGET_SIZE + strip_column(0U, half_width)) * 4U];
    const u8 *last_quad = &f->pixels[((u64)(TARGET_SIZE / 2U) * TARGET_SIZE +
                                      strip_column(STRIP_QUADS - 1U, half_width)) *
                                     4U];
    NTEST_LOG("the first quad %u, the last now %u", first_quad[0], last_quad[0]);
    NTEST_EXPECT_LE_UINT((u32)nph_i32_abs((i32)first_quad[0] - (i32)last_quad[0]), 2U);

    grove_scene_free(&s);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->rf.dev), NPH_RESULT_OK);
    world_teardown(f);
    NPH_FREE_TYPE(alloc, StreamWorld, f);
    test_temp_dir_remove(&dir);
}

// ---------------------------------------------------------------------------
// The streams, through the engine
// ---------------------------------------------------------------------------

typedef struct EngineWorld {
    NphDebugAllocator da;
    NphEngineConfig config;
    NphEngine *engine;
} EngineWorld;

// An engine over `pack`, offscreen, with the production path and its cvars, and a
// camera at `eye` looking along -z with a vertical field of `yfov`. NULL where there
// is no device for it.
static NphEngine *engine_open(EngineWorld *w, const char *pack, const char *const *settings,
                              u32 setting_count, NphDVec3 eye, f32 yfov, const char *scene)
{
    memset(w, 0, sizeof *w);
    nph_debug_allocator_init(&w->da, nph_allocator_system());
    w->config.packs[0] = pack;
    w->config.pack_count = 1U;
    NTEST_ASSERT_LE_UINT(setting_count, (u32)NPH_ENGINE_CONFIG_MAX_ASSIGNMENTS);
    for (u32 i = 0; i < setting_count; ++i) {
        w->config.assignments[i] = settings[i];
    }
    w->config.assignment_count = setting_count;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&w->da);
    d.config = &w->config;
    d.mode = NPH_ENGINE_MODE_OFFSCREEN;
    d.app_name = "world partition tests";
    d.offscreen_width = TARGET_SIZE;
    d.offscreen_height = TARGET_SIZE;
    const NphResult r = nph_engine_create(&d, &w->engine);
    if (r == NPH_RESULT_UNSUPPORTED) {
        return NULL;
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    NphEngine *e = w->engine;
    NphWorld *world = nph_engine_edit_world(e);
    NphSceneImportDesc id;
    memset(&id, 0, sizeof id);
    id.world = world;
    id.loader = nph_engine_loader(e);
    id.scene = nph_guid_from_cstr(scene);
    id.root_transform = nph_local_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&id, NULL, NULL), NPH_RESULT_OK);
    const NphRuntimeComponents *ids = nph_engine_components(e);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->camera};
    NphEntity camera;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(world, comps, 3U, &camera), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(world, camera, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
        ->translation = eye;
    NphCamera *lens = NPH_WORLD_GET_MUT(world, camera, ids->camera, NphCamera);
    lens->active = 1;
    lens->yfov = yfov;
    return e;
}

static void engine_close(EngineWorld *w)
{
    nph_engine_destroy(w->engine);
    w->engine = NULL;
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&w->da, NULL, NULL), 0U);
}

// `at_least` of the engine's frames, the first waiting for what it asks to load.
static const NphEngineFrameStats *engine_frames(NphEngine *e, u32 at_least)
{
    for (u32 i = 0; i < at_least; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(e), NPH_RESULT_OK);
        if (i == 0U) {
            nph_scene_renderer_wait(nph_engine_scene_renderer(e));
        }
    }
    return nph_engine_frame_stats(e);
}

// r.texture_budget_mb makes the stream, and the engine's frames run it: the view's
// levels are read in. Without the budget, there is no stream.
NTEST(WorldPartition, the_engine_runs_the_texture_stream_its_budget_asks_for)
{
    TempDir dir;
    test_temp_dir_make(&dir, "engine_textures");
    write_plate(&dir);
    NphAllocator *alloc = nph_allocator_system();
    NphJobSystem *jobs = NULL;
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = alloc;
    jd.worker_count = 2U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &jobs), NPH_RESULT_OK);
    char pack[1200];
    cook_pack(&dir, "plate.gltf", jobs, pack, sizeof pack);
    nph_job_system_destroy(jobs);

    EngineWorld w;
    const char *const budget[3] = {"r.pipeline=1", "r.taa=false", "r.texture_budget_mb=64"};
    NphEngine *e =
        engine_open(&w, pack, budget, 3U, nph_dvec3(0.0, 0.0, 6.0), 0.7F, "plate.gltf#scene");
    if (e == NULL) {
        NPH_IGNORE_RESULT(nph_debug_allocator_destroy(&w.da, NULL, NULL));
        test_temp_dir_remove(&dir);
        NTEST_SKIP("no GPU");
    }
    if (nph_renderer_texture_paging_supported(nph_engine_renderer(e))) {
        const NphEngineFrameStats *st = engine_frames(e, 64U);
        NTEST_ASSERT_NOT_NULL(nph_scene_renderer_texture_stream(nph_engine_scene_renderer(e)));
        NTEST_LOG("through the engine: %u texture levels, %llu bytes", st->scene.texture_levels,
                  (unsigned long long)st->scene.texture_bytes);
        NTEST_EXPECT_EQ_UINT(st->scene.instances, 1U);
        NTEST_EXPECT_GT_UINT(st->scene.texture_levels, 0U);
    }
    engine_close(&w);

    const char *const none[2] = {"r.pipeline=1", "r.taa=false"};
    e = engine_open(&w, pack, none, 2U, nph_dvec3(0.0, 0.0, 6.0), 0.7F, "plate.gltf#scene");
    NTEST_ASSERT_NOT_NULL(e);
    (void)engine_frames(e, 4U);
    NTEST_EXPECT_NULL(nph_scene_renderer_texture_stream(nph_engine_scene_renderer(e)));
    engine_close(&w);
    test_temp_dir_remove(&dir);
}

// A world's cells through the engine: its capacities and budgets from the cvars, and
// its frames streaming their proxies' levels.
NTEST(WorldPartition, the_engine_runs_the_cells_and_their_stream)
{
    NphAllocator *alloc = nph_allocator_system();
    NphJobSystem *jobs = NULL;
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = alloc;
    jd.worker_count = 2U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &jobs), NPH_RESULT_OK);
    grove_cook(alloc, jobs);
    nph_job_system_destroy(jobs);

    EngineWorld w;
    const char *const settings[6] = {"r.pipeline=1",       "r.taa=false",
                                     "r.max_cells=16",     "r.max_meshes=128",
                                     "r.voxel_pool_mb=16", "r.proxy_budget_mb=1"};
    NphEngine *e =
        engine_open(&w, g_grove_pack, settings, 6U, nph_dvec3(8.0, 1.0, 8.0 + GROVE_VIEW),
                    NARROW_FOV, "grove.gltf#scene");
    if (e == NULL) {
        NPH_IGNORE_RESULT(nph_debug_allocator_destroy(&w.da, NULL, NULL));
        NTEST_SKIP("no GPU");
    }
    if (nph_renderer_production_supported(nph_engine_renderer(e))) {
        const NphEngineFrameStats *st = engine_frames(e, 64U);
        NTEST_LOG("through the engine: %u cells, %u proxy levels, %llu bytes, %u cells proxied",
                  st->scene.cells, st->scene.proxy_levels,
                  (unsigned long long)st->scene.proxy_bytes, st->render.vis_cells_proxied);
        NTEST_EXPECT_EQ_UINT(st->scene.cells, GROVE_CELLS);
        NTEST_EXPECT_EQ_UINT(st->scene.cells_loading, 0U);
        NTEST_EXPECT_GT_UINT(st->scene.proxy_levels, 0U);
        NTEST_EXPECT_TRUE(st->scene.proxy_bytes <= 1000000U);
        NTEST_EXPECT_GT_UINT(st->render.vis_cells_proxied, 0U);
        NTEST_EXPECT_EQ_UINT(nph_gpu_device_validation_stats(nph_engine_device(e)).errors, 0U);
    }
    engine_close(&w);
}
