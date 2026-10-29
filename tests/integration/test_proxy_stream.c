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
//    stream is given the cells;
// 3. frames are drawn, the stream updated before each as a game's loop
//    does, and what it holds, loads and drops is checked against the
//    views' requests and its budget, and what is drawn against the cells
//    given every level directly.
//
// The tests skip without a Vulkan device or the renderer's production path.
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/platform/time.h>
#include <nepheline/runtime/proxy_stream.h>

#include "test_cook_support.h"
#include "test_render_support.h"

#include <cook/cooker.h>
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

NTEST_SETUP(StreamWorld)
{
    memset(f, 0, sizeof *f);
    f->alloc = nph_allocator_system();
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_meshes = 128U;
    d.max_materials = 4U;
    d.max_instances = 256U;
    d.max_cells = 16U;
    test_render_setup(&f->rf, &d);
}

NTEST_TEARDOWN(StreamWorld)
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

static void stream_make(StreamWorld *f, u64 budget)
{
    NphProxyStreamDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = f->alloc;
    d.renderer = f->rf.renderer;
    d.loader = f->loader;
    d.max_cells = MAX_CELLS;
    d.budget_bytes = budget;
    NTEST_ASSERT_EQ_INT(nph_proxy_stream_create(&d, &f->stream), NPH_RESULT_OK);
    for (u32 k = 0; k < f->cell_count; ++k) {
        NTEST_ASSERT_EQ_INT(nph_proxy_stream_add(f->stream, &f->stream_cells[k]), NPH_RESULT_OK);
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
        quiet = st.loading == 0U && st.installed == 0U && st.dropped == 0U ? quiet + 1U : 0U;
        ++out.frames;
    }
    NTEST_LOG("settled in %u frames: %u levels given, %u dropped", out.frames, out.installed,
              out.dropped);
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

    // Streamed until settled: every cell holds every level, as asked.
    stream_make(f, 0U);
    const Settled s = settle(f, &v);
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

    // Gone, the stream leaves every cell its coarsest alone.
    stream_free(f);
    draw(f, &v);
    NTEST_EXPECT_EQ_UINT(differing_pixels(coarsest, f->pixels), 0U);

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
    stream_make(f, 0U);
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
    stream_make(f, budget);
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
// a mesh that is not a level apart (its own head). And the stream's
// arguments are checked.
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
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad), NPH_RESULT_INVALID_ARGUMENT);
    bad.level_count = NPH_RENDER_CELL_MAX_LEVELS + 1U;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad), NPH_RESULT_INVALID_ARGUMENT);
    bad = f->stream_cells[2];
    bad.cell = NPH_RENDER_CELL_NULL;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad), NPH_RESULT_INVALID_ARGUMENT);
    bad = f->stream_cells[2];
    bad.coarsest = NPH_RENDER_MESH_NULL;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad), NPH_RESULT_INVALID_ARGUMENT);
    bad = f->stream_cells[2];
    bad.distance = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad), NPH_RESULT_INVALID_ARGUMENT);
    NphMeshLevel nameless[NPH_RENDER_CELL_MAX_LEVELS];
    memcpy(nameless, f->levels[2], sizeof nameless);
    nameless[0].mesh = NPH_GUID_NULL;
    bad = f->stream_cells[2];
    bad.levels = nameless;
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &bad), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &f->stream_cells[2]), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_proxy_stream_add(s, &f->stream_cells[3]), NPH_RESULT_FULL);
    nph_proxy_stream_destroy(s);

    // Streamed: the two cells stop short, the others hold every level.
    stream_make(f, 0U);
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
}
