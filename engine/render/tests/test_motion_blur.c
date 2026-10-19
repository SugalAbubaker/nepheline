// SPDX-License-Identifier: Apache-2.0
// The production path's motion blur (render/renderer.h NphRenderView.shutter;
// docs/render.md "Motion blur"; engine/render/src/motion_blur.c), against
// the ground truth a shutter integrates: the average of sharp frames at
// instants across the time it is open. A moving box, a turning camera, a
// still box in front of a moving one, a moving box under temporal
// anti-aliasing, and a half-open shutter come near it, far nearer than the
// sharp frame; what nothing moving reaches is left exactly as it was; no
// blur reaches past a tile; and frames without a previous production frame
// of their extent, cuts, debug views, the reference renderer, and a closed
// shutter do not blur. The fixture's teardown asserts zero validation and
// synchronisation-validation errors and nothing leaked.
#include <nepheline/math/scalar.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture MotionBlur;

NTEST_SETUP(MotionBlur)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(MotionBlur)
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
// The ground truth's instants across the shutter.
#define INSTANTS 32U
// The views' frame time, and a shutter open for all of it (360 degrees).
#define FRAME (1.0F / 240.0F)
// A wider target where the image must hold several tiles.
#define WIDE      256U
#define WIDE_SIZE ((u64)WIDE * WIDE * 4U)

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

// An unlit material: its colour is the pre-exposed value, which the
// tonemapper NONE passes through.
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

static NphRenderTransform box_at(NphDVec3 p, f32 scale)
{
    return nph_render_transform_from_trs(p, nph_quat_identity(), nph_vec3(scale, scale, scale));
}

static NphRenderInstance add_box(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
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

static void move_box(RenderFixture *f, NphRenderInstance box, NphDVec3 p, f32 scale)
{
    const NphRenderTransform t = box_at(p, scale);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, box, &t), NPH_RESULT_OK);
}

// A production view over black with the tonemapper NONE, so a pixel's value
// is the sRGB encoding of its pre-exposed colour, without anti-aliasing.
static void mb_view(NphRenderView *v, NphRenderCamera c, f32 shutter)
{
    nph_render_view_default(v, c);
    v->pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
    v->disable_ibl = 1;
    v->frame_seconds = FRAME;
    v->shutter = shutter;
}

// A camera on +Z, looking down -Z turned `yaw` radians toward +X.
static NphRenderCamera camera_turned(f32 yaw)
{
    const NphDVec3 eye = nph_dvec3(0.0, 0.0, 4.0);
    const NphDVec3 target =
        nph_dvec3(eye.x + (f64)nph_f32_sin(yaw), eye.y, eye.z - (f64)nph_f32_cos(yaw));
    return nph_render_camera_look_at(eye, target, nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F);
}

// A pixel's width at 4 m, where the boxes stand.
static f64 pixel_at_4m(u32 size)
{
    return 2.0 * (f64)nph_f32_tan(0.3F) * 4.0 / (f64)size;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// Renders a frame of `view` into `target` (`size` square) and reads it into
// `out`.
static NphRenderFrameStats render_into(RenderFixture *f, const NphRenderView *view,
                                       NphGpuImage target, u32 size, u8 *out)
{
    const NphRenderFrameStats s = test_render_frame(f, view, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, out, (u64)size * size * 4U);
    return s;
}

// Adds an image of SS x SS the test target's size, each of its pixels' SS^2
// samples averaged: an instant's area-sampled image, which a temporally
// anti-aliased view converges toward.
#define SS      4U
#define SS_SIZE (TEST_TARGET_SIZE * SS)
static void accumulator_add_supersampled(TestAccumulator *a, const u8 *big)
{
    for (u32 y = 0; y < TEST_TARGET_SIZE; ++y) {
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            for (u32 c = 0; c < 4U; ++c) {
                f32 sum = 0.0F;
                for (u32 sy = 0; sy < SS; ++sy) {
                    for (u32 sx = 0; sx < SS; ++sx) {
                        const u64 at =
                            (((u64)y * SS + sy) * (u64)SS_SIZE + (u64)x * SS + sx) * 4U + c;
                        sum += nph_render_ref_srgb_to_linear((f32)big[at] / 255.0F);
                    }
                }
                a->sum[((u64)y * TEST_TARGET_SIZE + x) * 4U + c] += sum / (f32)(SS * SS);
            }
        }
    }
    a->images += 1U;
}

// ---------------------------------------------------------------------------
// Against the shutter's average
// ---------------------------------------------------------------------------

// The ground truth of a box moving `step` metres along X a frame, at `x`
// at the frame's instant, with the shutter open `shutter` seconds: sharp
// frames at instants evenly across that time, averaged; each instant
// sampled once per pixel, or with `supersample` SS x SS times.
static void moving_box_truth(RenderFixture *f, NphRenderInstance box, f64 x, f64 step, f32 shutter,
                             bool supersample, u8 *out)
{
    static u8 instant[PIXELS];
    static u8 big[(u64)SS_SIZE * (u64)SS_SIZE * 4U];
    const NphGpuImage target =
        supersample ? test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SS_SIZE) : f->target;
    TestAccumulator acc;
    test_accumulator_begin(&acc, PIXELS);
    NphRenderView v;
    mb_view(&v, test_camera(), 0.0F);
    const f64 open = (f64)(shutter / FRAME);
    for (u32 k = 0; k < INSTANTS; ++k) {
        const f64 t = ((f64)k + 0.5) / (f64)INSTANTS - 0.5;
        move_box(f, box, nph_dvec3(x + step * open * t, 0.0, 0.0), 0.6F);
        if (supersample) {
            (void)render_into(f, &v, target, SS_SIZE, big);
            accumulator_add_supersampled(&acc, big);
        } else {
            (void)render_into(f, &v, target, TEST_TARGET_SIZE, instant);
            test_accumulator_add(&acc, instant);
        }
    }
    test_accumulator_end(&acc, out);
    if (supersample) {
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
        nph_gpu_image_destroy(f->dev, target);
    }
}

// Two frames of a box moving `step` metres a frame, the second at `x`, with
// `view`; the second, whose motion is measured from the first, is read into
// `out`.
static NphRenderFrameStats moving_box_frames(RenderFixture *f, NphRenderInstance box, f64 x,
                                             f64 step, const NphRenderView *view, u8 *out)
{
    move_box(f, box, nph_dvec3(x - step, 0.0, 0.0), 0.6F);
    (void)test_render_frame(f, view, f->target);
    move_box(f, box, nph_dvec3(x, 0.0, 0.0), 0.6F);
    return render_into(f, view, f->target, TEST_TARGET_SIZE, out);
}

// A box crossing the view at eight pixels a frame, the shutter open the
// whole frame: its edges across the motion spread into the ramps the
// instants' average has, and its interior stays its colour.
NTEST_F(MotionBlur, a_moving_box_blurs_as_the_shutter_averages)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance box =
        add_box(f, mesh, unlit(f, 0.5F), box_at(nph_dvec3(0.0, 0.0, 0.0), 0.6F));
    const f64 step = 8.0 * pixel_at_4m(TEST_TARGET_SIZE);
    static u8 truth[PIXELS];
    static u8 sharp[PIXELS];
    static u8 blurred[PIXELS];
    moving_box_truth(f, box, 0.0, step, FRAME, false, truth);
    NphRenderView v;
    mb_view(&v, test_camera(), 0.0F);
    move_box(f, box, nph_dvec3(0.0, 0.0, 0.0), 0.6F);
    (void)render_into(f, &v, f->target, TEST_TARGET_SIZE, sharp);
    mb_view(&v, test_camera(), FRAME);
    const NphRenderFrameStats s = moving_box_frames(f, box, 0.0, step, &v, blurred);
    NTEST_EXPECT_TRUE(s.motion_blur);
    const TestImageDiff ds =
        test_image_diff(sharp, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    const TestImageDiff db =
        test_image_diff(blurred, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    test_log_diff("moving box, sharp", ds);
    test_log_diff("moving box, blurred", db);
    NTEST_EXPECT_TRUE((f32)ds.mean > 1.0F); // the motion does show
    NTEST_EXPECT_TRUE((f32)db.mean < (f32)ds.mean * 0.3F);
}

// Half the frame open: the blur spans half the motion.
NTEST_F(MotionBlur, a_half_open_shutter_blurs_half_as_far)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance box =
        add_box(f, mesh, unlit(f, 0.5F), box_at(nph_dvec3(0.0, 0.0, 0.0), 0.6F));
    const f64 step = 10.0 * pixel_at_4m(TEST_TARGET_SIZE);
    static u8 truth[PIXELS];
    static u8 truth_full[PIXELS];
    static u8 blurred[PIXELS];
    moving_box_truth(f, box, 0.0, step, 0.5F * FRAME, false, truth);
    moving_box_truth(f, box, 0.0, step, FRAME, false, truth_full);
    NphRenderView v;
    mb_view(&v, test_camera(), 0.5F * FRAME);
    NTEST_EXPECT_TRUE(moving_box_frames(f, box, 0.0, step, &v, blurred).motion_blur);
    const TestImageDiff dh =
        test_image_diff(blurred, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    const TestImageDiff df =
        test_image_diff(blurred, truth_full, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    test_log_diff("half open, against its own average", dh);
    test_log_diff("half open, against the whole frame's", df);
    NTEST_EXPECT_TRUE((f32)dh.mean < (f32)df.mean * 0.5F);
}

// The camera turns six pixels a frame: the whole image moves as one, which
// the gather averages along by bilinear taps of the colour alone, and comes
// near the average of the turning camera's instants.
NTEST_F(MotionBlur, a_turning_camera_blurs_as_the_shutter_averages)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_box(f, mesh, unlit(f, 0.5F), box_at(nph_dvec3(-0.3, 0.1, 0.0), 0.5F));
    (void)add_box(f, mesh, unlit(f, 0.2F), box_at(nph_dvec3(0.8, -0.4, -2.0), 0.7F));
    (void)add_box(f, mesh, unlit(f, 0.8F), box_at(nph_dvec3(-1.5, 0.3, -5.0), 1.0F));
    // Six pixels at the centre of a 64-pixel view spanning 0.6 rad.
    const f32 turn = 6.0F * 2.0F * nph_f32_tan(0.3F) / (f32)TEST_TARGET_SIZE;
    static u8 truth[PIXELS];
    static u8 sharp[PIXELS];
    static u8 instant[PIXELS];
    static u8 blurred[PIXELS];
    TestAccumulator acc;
    test_accumulator_begin(&acc, PIXELS);
    NphRenderView v;
    for (u32 k = 0; k < INSTANTS; ++k) {
        const f32 t = ((f32)k + 0.5F) / (f32)INSTANTS - 0.5F;
        mb_view(&v, camera_turned(turn * t), 0.0F);
        (void)render_into(f, &v, f->target, TEST_TARGET_SIZE, instant);
        test_accumulator_add(&acc, instant);
    }
    test_accumulator_end(&acc, truth);
    mb_view(&v, camera_turned(0.0F), 0.0F);
    (void)render_into(f, &v, f->target, TEST_TARGET_SIZE, sharp);
    mb_view(&v, camera_turned(-turn), FRAME);
    (void)test_render_frame(f, &v, f->target);
    mb_view(&v, camera_turned(0.0F), FRAME);
    NTEST_EXPECT_TRUE(render_into(f, &v, f->target, TEST_TARGET_SIZE, blurred).motion_blur);
    const TestImageDiff ds =
        test_image_diff(sharp, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    const TestImageDiff db =
        test_image_diff(blurred, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    test_log_diff("turning camera, sharp", ds);
    test_log_diff("turning camera, blurred", db);
    NTEST_EXPECT_TRUE((f32)ds.mean > 1.0F);
    NTEST_EXPECT_TRUE((f32)db.mean < (f32)ds.mean * 0.3F);
}

// A still box in front of a moving one: the moving box blurs behind it, but
// none of its blur comes over the still box, which stays as sharp as the
// instants' average keeps it.
NTEST_F(MotionBlur, a_still_box_in_front_stays_sharp)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance box =
        add_box(f, mesh, unlit(f, 0.8F), box_at(nph_dvec3(0.0, 0.0, 0.0), 0.6F));
    // In front, half over the moving box's path.
    (void)add_box(f, mesh, unlit(f, 0.25F), box_at(nph_dvec3(0.15, 0.2, 1.5), 0.25F));
    const f64 step = 8.0 * pixel_at_4m(TEST_TARGET_SIZE);
    static u8 truth[PIXELS];
    static u8 sharp[PIXELS];
    static u8 blurred[PIXELS];
    moving_box_truth(f, box, 0.0, step, FRAME, false, truth);
    NphRenderView v;
    mb_view(&v, test_camera(), 0.0F);
    move_box(f, box, nph_dvec3(0.0, 0.0, 0.0), 0.6F);
    (void)render_into(f, &v, f->target, TEST_TARGET_SIZE, sharp);
    mb_view(&v, test_camera(), FRAME);
    NTEST_EXPECT_TRUE(moving_box_frames(f, box, 0.0, step, &v, blurred).motion_blur);
    const TestImageDiff ds =
        test_image_diff(sharp, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    const TestImageDiff db =
        test_image_diff(blurred, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    test_log_diff("behind a still box, sharp", ds);
    test_log_diff("behind a still box, blurred", db);
    NTEST_EXPECT_TRUE((f32)db.mean < (f32)ds.mean * 0.4F);
    // The still box's own pixels, those whose truth is its colour, keep it.
    const u8 front = (u8)nph_f32_round(nph_render_ref_linear_to_srgb(0.25F) * 255.0F);
    u32 kept = 0U;
    u32 inside = 0U;
    for (u64 p = 0; p < (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE; ++p) {
        if (truth[p * 4U] == front && sharp[p * 4U] == front) {
            inside += 1U;
            kept += nph_i32_abs((i32)blurred[p * 4U] - (i32)front) <= 1 ? 1U : 0U;
        }
    }
    NTEST_LOG("the still box: %u of %u pixels kept", kept, inside);
    NTEST_EXPECT_TRUE(inside > 20U);
    NTEST_EXPECT_TRUE(kept * 20U >= inside * 19U);
}

// Under temporal anti-aliasing the blur reads the resolved history: a box
// that has crossed into the view at four pixels a frame, its history
// following it, still comes near the instants' average, far nearer than the
// sharp resolve.
NTEST_F(MotionBlur, it_blurs_the_temporal_resolve)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance box =
        add_box(f, mesh, unlit(f, 0.5F), box_at(nph_dvec3(0.0, 0.0, 0.0), 0.6F));
    const f64 step = 4.0 * pixel_at_4m(TEST_TARGET_SIZE);
    const u32 frames = 2U * RENDER_TAA_PHASES;
    const f64 end = 0.0;
    static u8 truth[PIXELS];
    static u8 sharp[PIXELS];
    static u8 blurred[PIXELS];
    moving_box_truth(f, box, end, step, FRAME, true, truth);
    NphRenderView v;
    NphRenderFrameStats s;
    memset(&s, 0, sizeof s);
    for (u32 pass = 0; pass < 2U; ++pass) {
        mb_view(&v, test_camera(), pass == 0U ? 0.0F : FRAME);
        v.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
        v.camera_cut = 1; // each pass's history starts afresh
        for (u32 i = 0; i < frames; ++i) {
            const f64 x = end - step * (f64)(frames - 1U - i);
            move_box(f, box, nph_dvec3(x, 0.0, 0.0), 0.6F);
            s = i + 1U < frames
                    ? test_render_frame(f, &v, f->target)
                    : render_into(f, &v, f->target, TEST_TARGET_SIZE, pass == 0U ? sharp : blurred);
            v.camera_cut = 0;
        }
        NTEST_EXPECT_TRUE(s.taa_history);
        NTEST_EXPECT_EQ_INT(s.motion_blur, pass == 0U ? 0 : 1);
    }
    const TestImageDiff ds =
        test_image_diff(sharp, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    const TestImageDiff db =
        test_image_diff(blurred, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    test_log_diff("temporal, sharp", ds);
    test_log_diff("temporal, blurred", db);
    // The resolve's anti-aliased edges against the jittered visibility
    // buffer's classification leave about as much error as one sample per
    // pixel does in the tests above.
    NTEST_EXPECT_TRUE((f32)db.mean < (f32)ds.mean * 0.6F);
    NTEST_EXPECT_TRUE((f32)db.mean < 1.0F);
}

// ---------------------------------------------------------------------------
// Reach
// ---------------------------------------------------------------------------

// Columns [x0, x1) of the wide image `a` equal `b`'s, every channel.
static bool columns_equal(const u8 *a, const u8 *b, u32 x0, u32 x1)
{
    for (u32 y = 0; y < WIDE; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            if (memcmp(a + ((u64)y * WIDE + x) * 4U, b + ((u64)y * WIDE + x) * 4U, 4U) != 0) {
                return false;
            }
        }
    }
    return true;
}

// The columns a box covers in the wide image `a` (pixels brighter than
// black), as [first, last].
static void covered_columns(const u8 *a, u32 *first, u32 *last)
{
    *first = WIDE;
    *last = 0U;
    for (u32 y = 0; y < WIDE; ++y) {
        for (u32 x = 0; x < WIDE; ++x) {
            if (a[((u64)y * WIDE + x) * 4U] != 0U) {
                *first = nph_min_u32(*first, x);
                *last = nph_max_u32(*last, x);
            }
        }
    }
}

// In a wide view, a box moving far faster than a tile a frame blurs at
// most a tile either side of where it is, and a still box farther away than
// that is untouched, bit for bit.
NTEST_F(MotionBlur, the_blur_reaches_at_most_a_tile)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance box =
        add_box(f, mesh, unlit(f, 0.5F), box_at(nph_dvec3(0.6, 0.0, 0.0), 0.4F));
    (void)add_box(f, mesh, unlit(f, 0.3F), box_at(nph_dvec3(-0.9, 0.0, 0.0), 0.3F));
    const NphGpuImage wide = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, WIDE);
    u8 *sharp = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, WIDE_SIZE);
    u8 *blurred = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, WIDE_SIZE);
    NTEST_ASSERT_TRUE(sharp != NULL && blurred != NULL);
    // A hundred pixels a frame: a half-extent of 50, cut to a tile.
    const f64 step = 100.0 * pixel_at_4m(WIDE);
    NphRenderView v;
    mb_view(&v, test_camera(), 0.0F);
    (void)render_into(f, &v, wide, WIDE, sharp);
    mb_view(&v, test_camera(), FRAME);
    move_box(f, box, nph_dvec3(0.6 - step, 0.0, 0.0), 0.4F);
    (void)test_render_frame(f, &v, wide);
    move_box(f, box, nph_dvec3(0.6, 0.0, 0.0), 0.4F);
    NTEST_EXPECT_TRUE(render_into(f, &v, wide, WIDE, blurred).motion_blur);
    // The moving box's columns sharp, and blurred.
    u32 s0 = 0U;
    u32 s1 = 0U;
    u32 b0 = 0U;
    u32 b1 = 0U;
    covered_columns(sharp, &s0, &s1);
    covered_columns(blurred, &b0, &b1);
    NTEST_LOG("columns: the still and the moving box %u to %u sharp, %u to %u blurred", s0, s1, b0,
              b1);
    // The blur reaches past the moving box, but by no more than a tile (and
    // a pixel for rounding); the still box lies at the left beyond that.
    const u32 moving_first = WIDE / 2U;
    u32 m0 = WIDE;
    for (u32 y = 0; y < WIDE; ++y) {
        for (u32 x = moving_first; x < WIDE; ++x) {
            if (sharp[((u64)y * WIDE + x) * 4U] != 0U) {
                m0 = nph_min_u32(m0, x);
            }
        }
    }
    NTEST_ASSERT_TRUE(m0 < WIDE);
    NTEST_EXPECT_TRUE(b1 > s1);
    NTEST_EXPECT_LE_UINT(b1, s1 + (u32)NPH_RENDER_MB_TILE + 1U);
    // The still box's tiles, two tiles from any the blur reaches, are as
    // they were.
    const u32 reach0 = m0 > (u32)NPH_RENDER_MB_TILE + 1U ? m0 - (u32)NPH_RENDER_MB_TILE - 1U : 0U;
    const u32 untouched =
        reach0 > 2U * (u32)NPH_RENDER_MB_TILE ? reach0 - 2U * NPH_RENDER_MB_TILE : 0U;
    NTEST_ASSERT_TRUE(untouched > s0);
    NTEST_EXPECT_TRUE(columns_equal(sharp, blurred, 0U, untouched));
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, sharp, WIDE_SIZE);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, blurred, WIDE_SIZE);
    nph_gpu_image_destroy(f->dev, wide);
}

// ---------------------------------------------------------------------------
// When it blurs
// ---------------------------------------------------------------------------

// A closed shutter, a debug view, the reference renderer, a cut, a first
// frame, a frame after a reference one, and a frame after another extent
// do not blur; a closed shutter's frame is the plain one, bit for bit.
NTEST_F(MotionBlur, only_a_continuing_lit_production_frame_blurs)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance box =
        add_box(f, mesh, unlit(f, 0.5F), box_at(nph_dvec3(0.0, 0.0, 0.0), 0.6F));
    const f64 step = 6.0 * pixel_at_4m(TEST_TARGET_SIZE);
    static u8 plain[PIXELS];
    static u8 closed[PIXELS];
    NphRenderView v;
    mb_view(&v, test_camera(), FRAME);
    // The first production frame has nothing to measure motion against.
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).motion_blur);
    move_box(f, box, nph_dvec3(step, 0.0, 0.0), 0.6F);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, f->target).motion_blur);
    // A closed shutter.
    NphRenderView shut = v;
    shut.shutter = 0.0F;
    move_box(f, box, nph_dvec3(2.0 * step, 0.0, 0.0), 0.6F);
    NTEST_EXPECT_FALSE(render_into(f, &shut, f->target, TEST_TARGET_SIZE, closed).motion_blur);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, f->target).motion_blur);
    // A cut.
    NphRenderView cut = v;
    cut.camera_cut = 1;
    NTEST_EXPECT_FALSE(test_render_frame(f, &cut, f->target).motion_blur);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, f->target).motion_blur);
    // A debug view.
    NphRenderView debug = v;
    debug.debug_view = NPH_RENDER_VIEW_CLUSTERS;
    NTEST_EXPECT_FALSE(test_render_frame(f, &debug, f->target).motion_blur);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, f->target).motion_blur);
    // The reference renderer, and the production frame after it.
    NphRenderView reference = v;
    reference.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    NTEST_EXPECT_FALSE(test_render_frame(f, &reference, f->target).motion_blur);
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).motion_blur);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, f->target).motion_blur);
    // Another extent, and the next frame in it.
    const NphGpuImage small = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 32U);
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, small).motion_blur);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, small).motion_blur);
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).motion_blur);
    // The closed shutter's frame is the plain frame of the same scene.
    NphRenderView first = shut;
    first.camera_cut = 1;
    NTEST_EXPECT_FALSE(render_into(f, &first, f->target, TEST_TARGET_SIZE, plain).motion_blur);
    NTEST_EXPECT_EQ_INT(memcmp(plain, closed, PIXELS), 0);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_gpu_image_destroy(f->dev, small);
}

// A shutter time must be finite and not negative.
NTEST_F(MotionBlur, a_shutter_is_finite_and_not_negative)
{
    REQUIRE_PRODUCTION(f);
    const f32 bad[3] = {-1.0F, (f32)NAN, NPH_F32_INFINITY};
    for (u32 i = 0; i < 3U; ++i) {
        NphRenderView v;
        mb_view(&v, test_camera(), bad[i]);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        nph_gpu_rg_begin(f->graph);
        NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                            NPH_RESULT_INVALID_ARGUMENT);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
}

// Debug lines are drawn over a blurred frame, as over any other.
NTEST_F(MotionBlur, debug_lines_draw_over_the_blur)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh mesh = cube_mesh(f);
    const NphRenderInstance box =
        add_box(f, mesh, unlit(f, 0.5F), box_at(nph_dvec3(0.0, 0.0, 0.0), 0.6F));
    const f64 step = 6.0 * pixel_at_4m(TEST_TARGET_SIZE);
    NphRenderView v;
    mb_view(&v, test_camera(), FRAME);
    (void)test_render_frame(f, &v, f->target);
    move_box(f, box, nph_dvec3(step, 0.0, 0.0), 0.6F);
    // A red line across the view, below the box, over everything.
    NTEST_EXPECT_TRUE(nph_render_debug_line(
        f->renderer, nph_vec3(-2.0F, -0.9F, 0.0F), nph_vec3(2.0F, -0.9F, 0.0F),
        NPH_RENDER_RGBA(255U, 0U, 0U, 255U), (u32)NPH_RENDER_DEBUG_OVERLAY));
    const NphRenderFrameStats s = test_render_read(f, &v);
    NTEST_EXPECT_TRUE(s.motion_blur);
    NTEST_EXPECT_EQ_UINT(s.debug_lines, 1U);
    u32 red = 0U;
    for (u32 y = 0; y < TEST_TARGET_SIZE; ++y) {
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            const u8 *p = test_pixel(f, x, y);
            red += p[0] > 200U && p[1] < 40U && p[2] < 40U ? 1U : 0U;
        }
    }
    NTEST_EXPECT_TRUE(red >= TEST_TARGET_SIZE / 2U);
}
