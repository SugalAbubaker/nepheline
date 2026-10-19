// SPDX-License-Identifier: Apache-2.0
// Depth of field (render/renderer.h NphRenderView.aperture; docs/render.md
// "Depth of field"; engine/render/src/dof.c), against the ground truth a
// thin lens gives: the average of frames seen through points spread over
// its aperture (NphRenderView.lens_offset; an accumulation buffer). A
// background behind the focus, a near box over a focused wall, the same on
// the reference renderer, and a temporally resolved view come near it, far
// nearer than the pinhole image; tiles in focus as far as any circle reaches
// are left as drawn, and sharp edges stay sharp in tiles further out of
// focus; the lens offset moves what lies off the focus plane by its parallax
// and nothing on it; and a pinhole, a debug view, and lenses that cannot
// focus do not blur. The fixture's teardown asserts zero
// validation and synchronisation-validation errors and nothing leaked.
#include <nepheline/math/scalar.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture Dof;

NTEST_SETUP(Dof)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Dof)
{
    test_render_teardown(f);
}

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)
// The ground truth's points on the lens.
#define LENS_POINTS 64U
// A sensor ten times a full frame's height, wide open: 64-pixel views then
// show circles several pixels across, as a 4K view does with a real lens.
#define SENSOR   0.24F
#define APERTURE 1.0F
// The golden angle, which turns the lens points' spiral evenly.
#define GOLDEN_ANGLE 2.39996323F
// Supersampling for the temporally resolved view's truth, per axis.
#define SS      4U
#define SS_SIZE (TEST_TARGET_SIZE * SS)
#define WIDE    256U

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

// An unlit grey: its pre-exposed value, which the tonemapper NONE passes.
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

static NphRenderInstance add_scaled_box(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
                                        NphDVec3 p, NphVec3 scale)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(p, nph_quat_identity(), scale);
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

static NphRenderInstance add_box(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
                                 NphDVec3 p, f32 scale)
{
    return add_scaled_box(f, mesh, mat, p, nph_vec3(scale, scale, scale));
}

// A view of the test camera (on +Z, 4 m from the origin) over black with the
// tonemapper NONE, its lens focused `focus` metres away.
static void dof_view(NphRenderView *v, NphRenderPipeline pipeline, f32 focus)
{
    nph_render_view_default(v, test_camera());
    v->pipeline = pipeline;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
    v->disable_ibl = 1;
    v->aperture = APERTURE;
    v->focus_distance = focus;
    v->sensor_height = SENSOR;
}

// A box whose face is in focus 3.7 m away, and a brighter one whose face is
// 10 m away, behind it and to its right.
static void focus_and_background(RenderFixture *f)
{
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_box(f, mesh, unlit(f, 0.5F), nph_dvec3(0.0, 0.0, 0.0), 0.6F);
    (void)add_box(f, mesh, unlit(f, 0.8F), nph_dvec3(2.5, 0.5, -7.5), 3.0F);
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

static NphRenderFrameStats render_into(RenderFixture *f, const NphRenderView *view,
                                       NphGpuImage target, u32 size, u8 *out)
{
    const NphRenderFrameStats s = test_render_frame(f, view, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, out, (u64)size * size * 4U);
    return s;
}

// Box-filters an image SS times the test target's size into one of it, in
// linear light.
static void downsample(const u8 *big, u8 *out)
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
                const f32 mean = nph_render_ref_linear_to_srgb(sum / (f32)(SS * SS));
                out[((u64)y * TEST_TARGET_SIZE + x) * 4U + c] =
                    (u8)nph_f32_round(nph_f32_clamp(mean, 0.0F, 1.0F) * 255.0F);
            }
        }
    }
}

// The ground truth: frames through LENS_POINTS points spread evenly over the
// aperture's disc (a spiral of radius sqrt((i + 0.5) / n), turned by the
// golden angle), averaged in linear light; each frame sampled once per pixel,
// or SS x SS times with `supersample`.
static void lens_truth(RenderFixture *f, const NphRenderView *view, bool supersample, u8 *out)
{
    static u8 frame[PIXELS];
    static u8 big[(u64)SS_SIZE * (u64)SS_SIZE * 4U];
    const NphGpuImage target =
        supersample ? test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SS_SIZE) : f->target;
    NphRenderView v = *view;
    v.aperture = 0.0F;
    v.anti_aliasing = NPH_RENDER_AA_NONE;
    // The aperture's radius: half the focal length over the f-number.
    const f32 radius = 0.5F * nph_render_view_focal_length(view) / view->aperture;
    TestAccumulator acc;
    test_accumulator_begin(&acc, PIXELS);
    for (u32 i = 0; i < LENS_POINTS; ++i) {
        const f32 r = radius * nph_f32_sqrt(((f32)i + 0.5F) / (f32)LENS_POINTS);
        const f32 angle = GOLDEN_ANGLE * (f32)i;
        v.lens_offset = nph_vec2(r * nph_f32_cos(angle), r * nph_f32_sin(angle));
        if (supersample) {
            (void)render_into(f, &v, target, SS_SIZE, big);
            downsample(big, frame);
        } else {
            (void)render_into(f, &v, target, TEST_TARGET_SIZE, frame);
        }
        test_accumulator_add(&acc, frame);
    }
    test_accumulator_end(&acc, out);
    if (supersample) {
        NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
        nph_gpu_image_destroy(f->dev, target);
    }
}

// The pinhole image and the depth-of-field one of `view` (one frame each),
// and the lens's truth.
typedef struct Comparison {
    TestImageDiff pinhole;
    TestImageDiff blurred;
} Comparison;

static Comparison compare(RenderFixture *f, const NphRenderView *view, const char *what)
{
    static u8 truth[PIXELS];
    static u8 pinhole[PIXELS];
    static u8 blurred[PIXELS];
    lens_truth(f, view, false, truth);
    NphRenderView v = *view;
    v.aperture = 0.0F;
    NTEST_EXPECT_FALSE(render_into(f, &v, f->target, TEST_TARGET_SIZE, pinhole).dof);
    NTEST_EXPECT_TRUE(render_into(f, view, f->target, TEST_TARGET_SIZE, blurred).dof);
    Comparison c;
    c.pinhole = test_image_diff(pinhole, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    c.blurred = test_image_diff(blurred, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    NTEST_LOG("%s:", what);
    test_log_diff("  pinhole", c.pinhole);
    test_log_diff("  depth of field", c.blurred);
    return c;
}

// ---------------------------------------------------------------------------
// Against the lens
// ---------------------------------------------------------------------------

// Focused on a box 3.7 m away, a brighter box 10 m away blurs into the
// circles the lens gives it (3.8 pixels in radius) behind the sharp one,
// without the sharp box's colour spreading into the blur.
NTEST_F(Dof, the_background_blurs_as_a_lens_does)
{
    REQUIRE_RENDERER(f);
    focus_and_background(f);
    NphRenderView v;
    dof_view(&v, NPH_RENDER_PIPELINE_PRODUCTION, 3.7F);
    const Comparison c = compare(f, &v, "background");
    NTEST_EXPECT_TRUE((f32)c.pinhole.mean > 1.0F);
    NTEST_EXPECT_TRUE((f32)c.blurred.mean < (f32)c.pinhole.mean * 0.4F);
}

// Focused on a wall 6 m away, a bright box 1.35 m away blurs over it
// (circles of 12 pixels), its edges letting the wall through.
NTEST_F(Dof, a_near_box_blurs_over_the_focus)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_box(f, mesh, unlit(f, 0.3F), nph_dvec3(0.0, 0.0, -4.0), 4.0F);
    (void)add_box(f, mesh, unlit(f, 0.9F), nph_dvec3(0.2, 0.0, 2.5), 0.3F);
    NphRenderView v;
    dof_view(&v, NPH_RENDER_PIPELINE_PRODUCTION, 6.0F);
    const Comparison c = compare(f, &v, "near box");
    NTEST_EXPECT_TRUE((f32)c.pinhole.mean > 1.0F);
    NTEST_EXPECT_TRUE((f32)c.blurred.mean < (f32)c.pinhole.mean * 0.4F);
}

// The reference renderer has depth of field too, from its depth image.
NTEST_F(Dof, the_reference_renderer_blurs_alike)
{
    REQUIRE_RENDERER(f);
    focus_and_background(f);
    NphRenderView v;
    dof_view(&v, NPH_RENDER_PIPELINE_REFERENCE, 3.7F);
    const Comparison c = compare(f, &v, "reference");
    NTEST_EXPECT_TRUE((f32)c.blurred.mean < (f32)c.pinhole.mean * 0.4F);
}

// The temporal resolve averages the gather's turning rings: over a
// converged history the image comes near the lens's supersampled truth.
NTEST_F(Dof, it_blurs_before_the_temporal_resolve)
{
    REQUIRE_RENDERER(f);
    if (!nph_renderer_production_supported(f->renderer)) {
        NTEST_SKIP("no production path on this device");
    }
    focus_and_background(f);
    static u8 truth[PIXELS];
    static u8 pinhole[PIXELS];
    static u8 blurred[PIXELS];
    NphRenderView v;
    dof_view(&v, NPH_RENDER_PIPELINE_PRODUCTION, 3.7F);
    lens_truth(f, &v, true, truth);
    v.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
    for (u32 pass = 0; pass < 2U; ++pass) {
        v.aperture = pass == 0U ? 0.0F : APERTURE;
        v.camera_cut = 1;
        NphRenderFrameStats s;
        memset(&s, 0, sizeof s);
        for (u32 i = 0; i < 4U * RENDER_TAA_PHASES; ++i) {
            s = i + 1U < 4U * RENDER_TAA_PHASES ? test_render_frame(f, &v, f->target)
                                                : render_into(f, &v, f->target, TEST_TARGET_SIZE,
                                                              pass == 0U ? pinhole : blurred);
            v.camera_cut = 0;
        }
        NTEST_EXPECT_TRUE(s.taa_history);
        NTEST_EXPECT_EQ_INT(s.dof, pass == 0U ? 0 : 1);
    }
    const TestImageDiff dp =
        test_image_diff(pinhole, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    const TestImageDiff db =
        test_image_diff(blurred, truth, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    test_log_diff("temporal, pinhole", dp);
    test_log_diff("temporal, depth of field", db);
    NTEST_EXPECT_TRUE((f32)db.mean < (f32)dp.mean * 0.4F);
}

// ---------------------------------------------------------------------------
// The lens
// ---------------------------------------------------------------------------

// The centroid of the pixels whose red lies within [lo, hi].
static NphVec2 centroid(const u8 *image, u32 size, u8 lo, u8 hi)
{
    f64 sx = 0.0;
    f64 sy = 0.0;
    f64 n = 0.0;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const u8 v = image[((u64)y * size + x) * 4U];
            if (v >= lo && v <= hi) {
                sx += (f64)x;
                sy += (f64)y;
                n += 1.0;
            }
        }
    }
    return n > 0.0 ? nph_vec2((f32)(sx / n), (f32)(sy / n)) : nph_vec2(-1.0F, -1.0F);
}

// Seen through a point 0.1 m to the right on the lens, a card on the focus
// plane (3.7 m away) stays where it was, and a card 9 m away moves right by
// the parallax the shear leaves it: f_px * 0.1 * (1 / 3.7 - 1 / 9) pixels.
// (Cards, 2 mm thick: a box's sides, off the focus plane, would move too.)
NTEST_F(Dof, the_lens_offset_moves_only_what_is_out_of_focus)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_scaled_box(f, mesh, unlit(f, 0.5F), nph_dvec3(-0.6, 0.0, 0.299),
                         nph_vec3(0.6F, 0.6F, 0.002F));
    (void)add_scaled_box(f, mesh, unlit(f, 0.8F), nph_dvec3(1.2, 0.0, -5.001),
                         nph_vec3(2.0F, 2.0F, 0.002F));
    const NphGpuImage wide = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, WIDE);
    u8 *centred = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, (u64)WIDE * WIDE * 4U);
    u8 *shifted = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, (u64)WIDE * WIDE * 4U);
    NTEST_ASSERT_TRUE(centred != NULL && shifted != NULL);
    NphRenderView v;
    dof_view(&v, NPH_RENDER_PIPELINE_PRODUCTION, 3.7F);
    v.aperture = 0.0F;
    (void)render_into(f, &v, wide, WIDE, centred);
    v.lens_offset = nph_vec2(0.1F, 0.0F);
    (void)render_into(f, &v, wide, WIDE, shifted);
    // The focused box (0.5, sRGB 188) and the far one (0.8, sRGB 231).
    const NphVec2 near0 = centroid(centred, WIDE, 180U, 195U);
    const NphVec2 near1 = centroid(shifted, WIDE, 180U, 195U);
    const NphVec2 far0 = centroid(centred, WIDE, 225U, 240U);
    const NphVec2 far1 = centroid(shifted, WIDE, 225U, 240U);
    const f32 focal_px = 0.5F * (f32)WIDE / nph_f32_tan(0.3F);
    const f32 parallax = focal_px * 0.1F * (1.0F / 3.7F - 1.0F / 9.0F);
    NTEST_LOG("focused card %.3f -> %.3f, far card %.3f -> %.3f (expected +%.3f)", (f64)near0.x,
              (f64)near1.x, (f64)far0.x, (f64)far1.x, (f64)parallax);
    NTEST_EXPECT_TRUE(near0.x > 0.0F && far0.x > 0.0F);
    // On the focus plane nothing moves, to the bit; off it, one sample per
    // pixel moves a card's centroid in whole and half pixels.
    NTEST_EXPECT_NEAR_F32(near1.x - near0.x, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(far1.x - far0.x, parallax, 0.55F);
    NTEST_EXPECT_NEAR_F32(far1.y - far0.y, 0.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, centred, (u64)WIDE * WIDE * 4U);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, shifted, (u64)WIDE * WIDE * 4U);
    nph_gpu_image_destroy(f->dev, wide);
}

// ---------------------------------------------------------------------------
// What it leaves alone
// ---------------------------------------------------------------------------

// A wall on the focus plane covers the left half of a wide view and the
// sky, at infinity, the right: the tiles whose neighbourhoods lie on the
// wall are left as drawn, bit for bit.
NTEST_F(Dof, tiles_in_focus_are_left_as_drawn)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = cube_mesh(f);
    // Its face 4 m away, from far left to the view's centre.
    (void)add_box(f, mesh, unlit(f, 0.4F), nph_dvec3(-1.5, 0.0, -1.5), 3.0F);
    const NphGpuImage wide = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, WIDE);
    u8 *pinhole = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, (u64)WIDE * WIDE * 4U);
    u8 *blurred = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, (u64)WIDE * WIDE * 4U);
    NTEST_ASSERT_TRUE(pinhole != NULL && blurred != NULL);
    const NphRenderPipeline pipelines[2] = {NPH_RENDER_PIPELINE_PRODUCTION,
                                            NPH_RENDER_PIPELINE_REFERENCE};
    for (u32 i = 0; i < 2U; ++i) {
        NphRenderView v;
        dof_view(&v, pipelines[i], 4.0F);
        v.aperture = 0.0F;
        (void)render_into(f, &v, wide, WIDE, pinhole);
        v.aperture = APERTURE;
        NTEST_EXPECT_TRUE(render_into(f, &v, wide, WIDE, blurred).dof);
        // The wall ends at the view's centre (column 128); the tiles of 32
        // columns up to 96 have only wall within a tile.
        u32 changed = 0U;
        for (u32 y = 0; y < WIDE; ++y) {
            changed += memcmp(pinhole + (u64)y * WIDE * 4U, blurred + (u64)y * WIDE * 4U,
                              (u64)(WIDE / 2U - (u32)NPH_RENDER_DOF_TILE) * 4U) != 0
                           ? 1U
                           : 0U;
        }
        NTEST_EXPECT_EQ_UINT(changed, 0U);
        // The sky's edge beside the wall does blur.
        NTEST_EXPECT_NE_INT(memcmp(pinhole, blurred, (u64)WIDE * WIDE * 4U), 0);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, pinhole, (u64)WIDE * WIDE * 4U);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, blurred, (u64)WIDE * WIDE * 4U);
    nph_gpu_image_destroy(f->dev, wide);
}

// A near box at the bottom right, its circles 11 pixels across in radius,
// puts every tile of the small view among those gathered at half
// resolution; a card in focus 3.7 m away, left of centre, over a wall 3.9 m
// away (sharp too, its circles a third of a pixel) keeps its edges as drawn,
// bit for bit, far from the box's blur: across an edge the composite takes
// colours only from a surface that is blurred.
NTEST_F(Dof, sharp_edges_stay_sharp_beside_a_large_blur)
{
    REQUIRE_RENDERER(f);
    const NphRenderMesh mesh = cube_mesh(f);
    (void)add_box(f, mesh, unlit(f, 0.3F), nph_dvec3(0.0, 0.0, -1.4), 3.0F);
    (void)add_scaled_box(f, mesh, unlit(f, 0.8F), nph_dvec3(-0.5, 0.0, 0.299),
                         nph_vec3(0.6F, 0.6F, 0.002F));
    (void)add_box(f, mesh, unlit(f, 0.9F), nph_dvec3(0.29, -0.29, 2.65), 0.1F);
    static u8 pinhole[PIXELS];
    static u8 blurred[PIXELS];
    const NphRenderPipeline pipelines[2] = {NPH_RENDER_PIPELINE_PRODUCTION,
                                            NPH_RENDER_PIPELINE_REFERENCE};
    for (u32 i = 0; i < 2U; ++i) {
        NphRenderView v;
        dof_view(&v, pipelines[i], 3.7F);
        v.aperture = 0.0F;
        (void)render_into(f, &v, f->target, TEST_TARGET_SIZE, pinhole);
        v.aperture = APERTURE;
        NTEST_EXPECT_TRUE(render_into(f, &v, f->target, TEST_TARGET_SIZE, blurred).dof);
        // The card spans columns 10 to 26 and rows 24 to 40; the box's blur
        // reaches no further left than column 39.
        u32 changed = 0U;
        for (u32 y = 12U; y < 52U; ++y) {
            changed += memcmp(pinhole + (u64)y * TEST_TARGET_SIZE * 4U,
                              blurred + (u64)y * TEST_TARGET_SIZE * 4U, (u64)36U * 4U) != 0
                           ? 1U
                           : 0U;
        }
        NTEST_EXPECT_EQ_UINT(changed, 0U);
        NTEST_EXPECT_NE_INT(memcmp(pinhole, blurred, PIXELS), 0);
    }
}

// A pinhole and a debug view have no depth of field: the pinhole's frame is
// the plain one, bit for bit.
NTEST_F(Dof, a_pinhole_and_debug_views_are_sharp)
{
    REQUIRE_RENDERER(f);
    focus_and_background(f);
    static u8 plain[PIXELS];
    static u8 pinhole[PIXELS];
    NphRenderView v;
    dof_view(&v, NPH_RENDER_PIPELINE_PRODUCTION, 4.0F);
    NphRenderView p = v;
    p.aperture = 0.0F;
    NTEST_EXPECT_FALSE(render_into(f, &p, f->target, TEST_TARGET_SIZE, pinhole).dof);
    nph_render_view_default(&p, test_camera());
    p.pipeline = v.pipeline;
    p.tonemapper = v.tonemapper;
    p.background = v.background;
    p.background_color = v.background_color;
    p.disable_ibl = 1;
    NTEST_EXPECT_FALSE(render_into(f, &p, f->target, TEST_TARGET_SIZE, plain).dof);
    NTEST_EXPECT_EQ_INT(memcmp(plain, pinhole, PIXELS), 0);
    NphRenderView debug = v;
    debug.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
    NTEST_EXPECT_FALSE(test_render_frame(f, &debug, f->target).dof);
}

// An aperture, a sensor, and a lens offset must be finite and not
// negative, and a lens must focus beyond its focal length.
NTEST_F(Dof, a_lens_must_be_able_to_focus)
{
    REQUIRE_RENDERER(f);
    NphRenderView good;
    dof_view(&good, NPH_RENDER_PIPELINE_PRODUCTION, 4.0F);
    // The focal length of a 0.24 m sensor under the camera's 0.6 rad: 0.388 m.
    const f32 focal = nph_render_view_focal_length(&good);
    NTEST_EXPECT_NEAR_F32(focal, 0.24F / (2.0F * nph_f32_tan(0.3F)), 1e-6F);
    NphRenderView bad[7];
    for (u32 i = 0; i < 7U; ++i) {
        bad[i] = good;
    }
    bad[0].aperture = -1.0F;
    bad[1].aperture = (f32)NAN;
    bad[2].sensor_height = -0.1F;
    bad[3].focus_distance = focal;
    bad[4].focus_distance = NPH_F32_INFINITY;
    bad[5].lens_offset = nph_vec2((f32)NAN, 0.0F);
    bad[6].aperture = 0.0F; // a lens offset alone needs a focus too
    bad[6].focus_distance = 0.0F;
    bad[6].lens_offset = nph_vec2(0.01F, 0.0F);
    for (u32 i = 0; i < 7U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        nph_gpu_rg_begin(f->graph);
        NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &bad[i], f->graph, f->target, NULL, NULL),
                            NPH_RESULT_INVALID_ARGUMENT);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
    // A pinhole needs no focus at all.
    NphRenderView pinhole = good;
    pinhole.aperture = 0.0F;
    pinhole.focus_distance = 0.0F;
    NTEST_EXPECT_FALSE(test_render_frame(f, &pinhole, f->target).dof);
}
