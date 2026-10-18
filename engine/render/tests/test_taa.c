// SPDX-License-Identifier: Apache-2.0
// The production path's temporal anti-aliasing (render/renderer.h
// NPH_RENDER_AA_TEMPORAL; docs/render.md "Temporal anti-aliasing";
// engine/render/src/taa.c): the jitter covers the pixel evenly and the
// filter's weights follow it; a still view converges toward the image a
// 4 x 4 supersampled reference renders, a panning camera and a moving
// object stay as close to it with no trail behind, and the history is
// dropped where it no longer applies (a cut, a resize, a frame resolved
// otherwise) and rescaled when the exposure changes; debug views and the
// reference renderer are never jittered. The fixture's teardown asserts
// zero validation and synchronisation-validation errors and nothing leaked.
#include <nepheline/math/scalar.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture Taa;

NTEST_SETUP(Taa)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Taa)
{
    test_render_teardown(f);
}

#define REQUIRE_PRODUCTION(f)                                                                      \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_production_supported((f)->renderer)) {                                   \
            NTEST_SKIP("no production path on this device");                                       \
        }                                                                                          \
    } while (0)

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)
// The ground truth's samples per pixel, per axis.
#define SS      4U
#define SS_SIZE (TEST_TARGET_SIZE * SS)
// Frames for the history to converge: four passes over the jitter sequence.
#define CONVERGE (4U * RENDER_TAA_PHASES)

// ---------------------------------------------------------------------------
// The jitter and the filter
// ---------------------------------------------------------------------------

// Eight points, each in [-0.5, 0.5), two in every quarter of the pixel,
// centred on it within a sixteenth of a pixel; then the sequence repeats.
NTEST(taa, the_jitter_covers_the_pixel_evenly)
{
    u32 quarters[4] = {0U, 0U, 0U, 0U};
    f32 mean_x = 0.0F;
    f32 mean_y = 0.0F;
    for (u32 i = 0; i < RENDER_TAA_PHASES; ++i) {
        const NphVec2 j = render_taa_jitter(i);
        NTEST_EXPECT_TRUE(j.x >= -0.5F && j.x < 0.5F);
        NTEST_EXPECT_TRUE(j.y >= -0.5F && j.y < 0.5F);
        quarters[(j.x < 0.0F ? 0U : 1U) + (j.y < 0.0F ? 0U : 2U)] += 1U;
        mean_x += j.x / (f32)RENDER_TAA_PHASES;
        mean_y += j.y / (f32)RENDER_TAA_PHASES;
        for (u32 k = 0; k < i; ++k) {
            const NphVec2 o = render_taa_jitter(k);
            NTEST_EXPECT_TRUE(o.x != j.x || o.y != j.y);
        }
        const NphVec2 again = render_taa_jitter(i + RENDER_TAA_PHASES);
        NTEST_EXPECT_ULP_F32(again.x, j.x, 0);
        NTEST_EXPECT_ULP_F32(again.y, j.y, 0);
    }
    for (u32 q = 0; q < 4U; ++q) {
        NTEST_EXPECT_EQ_UINT(quarters[q], 2U);
    }
    NTEST_EXPECT_TRUE(nph_f32_abs(mean_x) < 0.0625F);
    NTEST_EXPECT_TRUE(nph_f32_abs(mean_y) < 0.0625F);
}

// The weights sum to 1, fall with distance from the jittered centre, and
// mirror with the jitter.
NTEST(taa, the_filter_weights_follow_the_jitter)
{
    f32 w[9];
    render_taa_weights(nph_vec2(0.0F, 0.0F), w);
    f32 sum = 0.0F;
    for (u32 i = 0; i < 9U; ++i) {
        sum += w[i];
    }
    NTEST_EXPECT_NEAR_F32(sum, 1.0F, 1e-5F);
    // Centre, then the edges, then the corners.
    NTEST_EXPECT_TRUE(w[4] > w[1]);
    NTEST_EXPECT_TRUE(w[1] > w[0]);
    NTEST_EXPECT_NEAR_F32(w[1], w[3], 1e-6F);
    NTEST_EXPECT_NEAR_F32(w[0], w[8], 1e-6F);
    // The sample at offset o lies at o - jitter: with the jitter at
    // (0.4, -0.3), the right and upper samples lie nearer the centre.
    f32 m[9];
    render_taa_weights(nph_vec2(0.4F, -0.3F), w);
    render_taa_weights(nph_vec2(-0.4F, 0.3F), m);
    NTEST_EXPECT_TRUE(w[5] > w[3]);
    NTEST_EXPECT_TRUE(w[1] > w[7]);
    for (u32 i = 0; i < 9U; ++i) {
        NTEST_EXPECT_NEAR_F32(w[i], m[8U - i], 1e-6F);
    }
}

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------

static NphRenderMesh cube_mesh(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

// An unlit material: its colour is the pre-exposed value, whatever the
// exposure, and the tonemapper NONE passes it through.
static NphRenderMaterial unlit(RenderFixture *f, f32 grey)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(grey, grey, grey, 1.0F);
    md.unlit = 1;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

// A cube at `p`, turned about the view axis so its edges cross the pixel
// grid at an angle (staircases without anti-aliasing).
static NphRenderTransform turned(NphDVec3 p, f32 scale)
{
    const NphQuat q = nph_quat_mul(nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), 0.35F),
                                   nph_quat_from_axis_angle(nph_vec3(0.0F, 1.0F, 0.0F), 0.6F));
    return nph_render_transform_from_trs(p, q, nph_vec3(scale, scale, scale));
}

static NphRenderInstance add_cube(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
                                  NphRenderTransform t)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = t;
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

// A production view over black with the tonemapper NONE, so a pixel's
// value is the sRGB encoding of its pre-exposed colour.
static void taa_view(NphRenderView *v, NphRenderCamera c, NphRenderAntiAliasing aa)
{
    nph_render_view_default(v, c);
    v->pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
    v->disable_ibl = 1;
    v->anti_aliasing = aa;
}

// A camera on +Z, `x` metres along X, looking down -Z.
static NphRenderCamera camera_at(f64 x)
{
    return nph_render_camera_look_at(nph_dvec3(x, 0.0, 4.0), nph_dvec3(x, 0.0, 0.0),
                                     nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F);
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// The ground truth: the reference renderer at SS x SS the resolution, each
// pixel the average of its SS^2 samples in linear light.
static void supersampled(RenderFixture *f, const NphRenderView *view, u8 *out)
{
    const NphGpuImage big = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SS_SIZE);
    NphRenderView v = *view;
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    v.anti_aliasing = NPH_RENDER_AA_NONE;
    (void)test_render_frame(f, &v, big);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const u64 bytes = (u64)SS_SIZE * (u64)SS_SIZE * 4U;
    u8 *texels = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, bytes);
    NTEST_ASSERT_NOT_NULL(texels);
    test_read_image(f->dev, big, 0U, texels, bytes);
    for (u32 y = 0; y < TEST_TARGET_SIZE; ++y) {
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            for (u32 c = 0; c < 4U; ++c) {
                f32 sum = 0.0F;
                for (u32 sy = 0; sy < SS; ++sy) {
                    for (u32 sx = 0; sx < SS; ++sx) {
                        const u64 at =
                            (((u64)y * SS + sy) * (u64)SS_SIZE + (u64)x * SS + sx) * 4U + c;
                        sum += nph_render_ref_srgb_to_linear((f32)texels[at] / 255.0F);
                    }
                }
                const f32 mean = nph_render_ref_linear_to_srgb(sum / (f32)(SS * SS));
                out[((u64)y * TEST_TARGET_SIZE + x) * 4U + c] =
                    (u8)nph_f32_round(nph_f32_clamp(mean, 0.0F, 1.0F) * 255.0F);
            }
        }
    }
    NPH_FREE_ARRAY(nph_allocator_system(), u8, texels, bytes);
    nph_gpu_image_destroy(f->dev, big);
}

// Renders `frames` frames of `view` and reads the last one into `out`;
// returns the last frame's statistics.
static NphRenderFrameStats render_frames(RenderFixture *f, const NphRenderView *view, u32 frames,
                                         u8 *out)
{
    for (u32 i = 0; i + 1U < frames; ++i) {
        (void)test_render_frame(f, view, f->target);
    }
    const NphRenderFrameStats s = test_render_read(f, view);
    memcpy(out, f->pixels, PIXELS);
    return s;
}

typedef struct Diff {
    f64 mean;      // mean absolute difference over every pixel's RGB, in 8-bit levels
    u32 max;       // the largest
    u32 max_trail; // the largest where `truth` is black two pixels around: a trail, not an
                   // edge's own softening
} Diff;

// Whether `truth` is black at every pixel within two of (x, y).
static bool dark_around(const u8 *truth, i32 x, i32 y)
{
    const i32 last = (i32)TEST_TARGET_SIZE - 1;
    for (i32 dy = -2; dy <= 2; ++dy) {
        for (i32 dx = -2; dx <= 2; ++dx) {
            const i32 px = nph_clamp_i32(x + dx, 0, last);
            const i32 py = nph_clamp_i32(y + dy, 0, last);
            const u8 *t = truth + ((u64)py * TEST_TARGET_SIZE + (u64)px) * 4U;
            if (t[0] != 0U || t[1] != 0U || t[2] != 0U) {
                return false;
            }
        }
    }
    return true;
}

static Diff diff(const u8 *a, const u8 *truth)
{
    Diff d = {0.0, 0U, 0U};
    u64 sum = 0U;
    for (u64 p = 0; p < (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE; ++p) {
        const bool dark =
            dark_around(truth, (i32)(p % TEST_TARGET_SIZE), (i32)(p / TEST_TARGET_SIZE));
        for (u32 c = 0; c < 3U; ++c) {
            const i32 e = (i32)a[p * 4U + c] - (i32)truth[p * 4U + c];
            const u32 m = (u32)(e < 0 ? -e : e);
            sum += m;
            d.max = nph_max_u32(d.max, m);
            if (dark) {
                d.max_trail = nph_max_u32(d.max_trail, m);
            }
        }
    }
    d.mean = (f64)sum / (f64)((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 3U);
    return d;
}

static void log_diff(const char *what, Diff d)
{
    NTEST_LOG("%s: mean %.3f levels, max %u, max away from edges %u", what, d.mean, d.max,
              d.max_trail);
}

// ---------------------------------------------------------------------------
// Convergence and motion
// ---------------------------------------------------------------------------

// A still view: one sample per pixel has staircases along the cube's
// edges; after four passes over the jitter, the resolve is far nearer the
// supersampled image, and flat colour stays exact.
NTEST_F(Taa, a_still_view_converges_to_the_supersampled_image)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_cube(f, mesh, unlit(f, 0.25F), turned(nph_dvec3(0.0, 0.0, 0.0), 1.0F));
    static u8 truth[PIXELS];
    static u8 aliased[PIXELS];
    static u8 resolved[PIXELS];
    NphRenderView v;
    taa_view(&v, camera_at(0.0), NPH_RENDER_AA_NONE);
    supersampled(f, &v, truth);
    NphRenderFrameStats s = render_frames(f, &v, 1U, aliased);
    NTEST_EXPECT_FALSE(s.taa);
    taa_view(&v, camera_at(0.0), NPH_RENDER_AA_TEMPORAL);
    s = render_frames(f, &v, CONVERGE, resolved);
    NTEST_EXPECT_TRUE(s.taa);
    NTEST_EXPECT_TRUE(s.taa_history);
    const Diff da = diff(aliased, truth);
    const Diff dt = diff(resolved, truth);
    log_diff("one sample per pixel", da);
    log_diff("temporal, converged", dt);
    NTEST_EXPECT_TRUE((f32)da.mean > 0.5F); // the scene does alias
    NTEST_EXPECT_TRUE((f32)dt.mean < (f32)da.mean * 0.5F);
    // The cube's face and the background are flat: the resolve keeps them.
    const u8 *centre = resolved + ((u64)(TEST_TARGET_SIZE / 2U) * TEST_TARGET_SIZE + 32U) * 4U;
    const u8 *corner = resolved;
    const u8 *centre_truth = truth + ((u64)(TEST_TARGET_SIZE / 2U) * TEST_TARGET_SIZE + 32U) * 4U;
    NTEST_EXPECT_LE_INT(nph_i32_abs((i32)centre[0] - (i32)centre_truth[0]), 2);
    NTEST_EXPECT_LE_UINT((u32)corner[0], 1U);
}

// The camera pans 0.37 pixels a frame across cubes at two depths: the
// history follows each surface's own motion (parallax), so the image stays
// as near the supersampled one at the last position as a still one, and
// nothing trails behind the edges.
NTEST_F(Taa, a_panning_camera_stays_sharp)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderMaterial grey = unlit(f, 0.25F);
    (void)add_cube(f, mesh, grey, turned(nph_dvec3(-0.4, 0.0, 0.0), 0.8F));
    (void)add_cube(f, mesh, unlit(f, 0.12F), turned(nph_dvec3(1.2, 0.4, -3.0), 1.0F));
    // A pixel spans 2 tan(0.3) * 4 m / 64 at the near cube's depth.
    const f64 pixel = 2.0 * (f64)nph_f32_tan(0.3F) * 4.0 / (f64)TEST_TARGET_SIZE;
    const f64 step = 0.37 * pixel;
    static u8 truth[PIXELS];
    static u8 aliased[PIXELS];
    static u8 resolved[PIXELS];
    NphRenderView v;
    NphRenderFrameStats s;
    memset(&s, 0, sizeof s);
    for (u32 i = 0; i < CONVERGE; ++i) {
        taa_view(&v, camera_at(step * (f64)i), NPH_RENDER_AA_TEMPORAL);
        s = i + 1U < CONVERGE ? test_render_frame(f, &v, f->target) : test_render_read(f, &v);
    }
    memcpy(resolved, f->pixels, PIXELS);
    NTEST_EXPECT_TRUE(s.taa_history);
    taa_view(&v, camera_at(step * (f64)(CONVERGE - 1U)), NPH_RENDER_AA_NONE);
    supersampled(f, &v, truth);
    (void)render_frames(f, &v, 1U, aliased);
    const Diff da = diff(aliased, truth);
    const Diff dt = diff(resolved, truth);
    log_diff("panning, one sample per pixel", da);
    log_diff("panning, temporal", dt);
    NTEST_EXPECT_TRUE((f32)dt.mean < (f32)da.mean * 0.6F);
    NTEST_EXPECT_LE_UINT(dt.max_trail, 2U);
}

// A cube crossing a still view at 1.3 pixels a frame: the history follows
// it through its previous transform, so its edges stay anti-aliased and
// the background it leaves is black again, not a trail.
NTEST_F(Taa, a_moving_object_leaves_no_trail)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance cube =
        add_cube(f, mesh, unlit(f, 0.25F), turned(nph_dvec3(-0.6, 0.0, 0.0), 0.8F));
    const f64 pixel = 2.0 * (f64)nph_f32_tan(0.3F) * 4.0 / (f64)TEST_TARGET_SIZE;
    const f64 step = 1.3 * pixel;
    static u8 truth[PIXELS];
    static u8 aliased[PIXELS];
    static u8 resolved[PIXELS];
    NphRenderView v;
    taa_view(&v, camera_at(0.0), NPH_RENDER_AA_TEMPORAL);
    NphRenderFrameStats s;
    memset(&s, 0, sizeof s);
    NphRenderTransform t = turned(nph_dvec3(-0.6, 0.0, 0.0), 0.8F);
    for (u32 i = 0; i < CONVERGE; ++i) {
        t = turned(nph_dvec3(-0.6 + step * (f64)i, 0.0, 0.0), 0.8F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, cube, &t),
                            NPH_RESULT_OK);
        s = i + 1U < CONVERGE ? test_render_frame(f, &v, f->target) : test_render_read(f, &v);
    }
    memcpy(resolved, f->pixels, PIXELS);
    NTEST_EXPECT_TRUE(s.taa_history);
    taa_view(&v, camera_at(0.0), NPH_RENDER_AA_NONE);
    supersampled(f, &v, truth);
    (void)render_frames(f, &v, 1U, aliased);
    const Diff da = diff(aliased, truth);
    const Diff dt = diff(resolved, truth);
    log_diff("moving cube, one sample per pixel", da);
    log_diff("moving cube, temporal", dt);
    NTEST_EXPECT_TRUE((f32)dt.mean < (f32)da.mean * 0.6F);
    NTEST_EXPECT_LE_UINT(dt.max_trail, 2U);
}

// ---------------------------------------------------------------------------
// The history's lifetime
// ---------------------------------------------------------------------------

// A cut drops the history: the frame after it is, bit for bit, the frame a
// view without history renders with the same jitter, with nothing of the
// view before; the next one continues again. (Clipping alone would hide a
// stale history in flat colour, so the comparison is exact.)
NTEST_F(Taa, a_cut_drops_the_history)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_cube(f, mesh, unlit(f, 0.25F), turned(nph_dvec3(0.0, 0.0, 0.0), 1.0F));
    static u8 cut[PIXELS];
    static u8 fresh[PIXELS];
    NphRenderView a;
    taa_view(&a, camera_at(-0.3), NPH_RENDER_AA_TEMPORAL);
    NphRenderFrameStats s = test_render_frame(f, &a, f->target);
    NTEST_EXPECT_TRUE(s.taa);
    NTEST_EXPECT_FALSE(s.taa_history);
    for (u32 i = 1; i < 2U * RENDER_TAA_PHASES; ++i) {
        s = test_render_frame(f, &a, f->target);
    }
    NTEST_EXPECT_TRUE(s.taa_history);
    // Frame 16 of the sequence: jitter phase 0.
    NphRenderView b;
    taa_view(&b, camera_at(0.25), NPH_RENDER_AA_TEMPORAL);
    b.camera_cut = 1;
    s = render_frames(f, &b, 1U, cut);
    NTEST_EXPECT_TRUE(s.taa);
    NTEST_EXPECT_FALSE(s.taa_history);
    b.camera_cut = 0;
    NTEST_EXPECT_TRUE(test_render_frame(f, &b, f->target).taa_history);
    // Frames 18 to 23, then a frame without anti-aliasing (which drops the
    // history and does not advance the sequence), then frame 24: phase 0
    // again, without history.
    for (u32 i = 0; i < RENDER_TAA_PHASES - 2U; ++i) {
        (void)test_render_frame(f, &b, f->target);
    }
    NphRenderView plain = b;
    plain.anti_aliasing = NPH_RENDER_AA_NONE;
    (void)test_render_frame(f, &plain, f->target);
    s = render_frames(f, &b, 1U, fresh);
    NTEST_EXPECT_FALSE(s.taa_history);
    NTEST_EXPECT_EQ_INT(memcmp(cut, fresh, PIXELS), 0);
}

// A frame resolved otherwise (no anti-aliasing, a debug view) or into
// another extent breaks the history's continuity.
NTEST_F(Taa, the_history_continues_only_from_the_frame_before)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_cube(f, mesh, unlit(f, 0.25F), turned(nph_dvec3(0.0, 0.0, 0.0), 1.0F));
    NphRenderView v;
    taa_view(&v, camera_at(0.0), NPH_RENDER_AA_TEMPORAL);
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).taa_history);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, f->target).taa_history);
    NphRenderView plain = v;
    plain.anti_aliasing = NPH_RENDER_AA_NONE;
    NTEST_EXPECT_FALSE(test_render_frame(f, &plain, f->target).taa);
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).taa_history);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, f->target).taa_history);
    // Another extent: the history is made again at that size.
    const NphGpuImage small = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 32U);
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, small).taa_history);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, small).taa_history);
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).taa_history);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_gpu_image_destroy(f->dev, small);
}

// Debug views and the reference renderer are never jittered: their images
// are those without anti-aliasing, bit for bit.
NTEST_F(Taa, debug_views_and_the_reference_are_not_jittered)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_cube(f, mesh, unlit(f, 0.25F), turned(nph_dvec3(0.0, 0.0, 0.0), 1.0F));
    static u8 plain[PIXELS];
    static u8 temporal[PIXELS];
    const NphRenderPipeline pipelines[2] = {NPH_RENDER_PIPELINE_PRODUCTION,
                                            NPH_RENDER_PIPELINE_REFERENCE};
    const NphRenderDebugView views[2] = {NPH_RENDER_VIEW_CLUSTERS, NPH_RENDER_VIEW_LIT};
    for (u32 i = 0; i < 2U; ++i) {
        NphRenderView v;
        taa_view(&v, camera_at(0.0), NPH_RENDER_AA_NONE);
        v.pipeline = pipelines[i];
        v.debug_view = views[i];
        (void)render_frames(f, &v, 1U, plain);
        v.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
        for (u32 k = 0; k < 3U; ++k) {
            const NphRenderFrameStats s = render_frames(f, &v, 1U, temporal);
            NTEST_EXPECT_FALSE(s.taa);
            NTEST_EXPECT_EQ_INT(memcmp(plain, temporal, PIXELS), 0);
        }
    }
}

// The history is pre-exposed with its frame's exposure: when the exposure
// changes, the first frame after reads it rescaled, so the image is the
// converged one of the new exposure at once rather than a fade from the old.
// The edges of a black cube against a grey background show it: clipping
// keeps a stale history in flat colour from showing, but not where an edge
// spreads the neighbourhood from black to grey.
NTEST_F(Taa, an_exposure_change_rescales_the_history)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_cube(f, mesh, unlit(f, 0.0F), turned(nph_dvec3(0.0, 0.0, 0.0), 1.0F));
    static u8 before[PIXELS];
    static u8 after[PIXELS];
    static u8 converged[PIXELS];
    NphRenderView v;
    taa_view(&v, camera_at(0.0), NPH_RENDER_AA_TEMPORAL);
    // The background's radiance times the exposure (EV100 0: 1 / 1.2).
    v.ev100 = 0.0F;
    v.background_color = nph_vec3(0.3F, 0.3F, 0.3F);
    (void)render_frames(f, &v, CONVERGE, before);
    v.ev100 = 1.0F;
    const NphRenderFrameStats s = render_frames(f, &v, 1U, after);
    NTEST_EXPECT_TRUE(s.taa_history);
    // Converged at the new exposure: as many frames on, in the same phase.
    (void)render_frames(f, &v, CONVERGE, converged);
    const Diff db = diff(before, converged);
    const Diff da = diff(after, converged);
    log_diff("the old exposure's image", db);
    log_diff("the first frame at the new exposure", da);
    NTEST_EXPECT_TRUE((f32)db.mean > 10.0F);
    NTEST_EXPECT_TRUE((f32)da.mean < 0.3F);
    NTEST_EXPECT_LE_UINT(da.max, 16U);
}
