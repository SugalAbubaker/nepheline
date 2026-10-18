// SPDX-License-Identifier: Apache-2.0
// Bloom and the exposure (render/renderer.h NphRenderView.bloom and
// .exposure; docs/render.md "Bloom and exposure"; engine/render/src/post.c):
// the metering's mean between its percentiles and the EV100 it gives, as
// the shaders compute them; bloom leaves a uniform image as it was and
// spreads a highlight without changing the image's energy, on both
// pipelines and under temporal anti-aliasing; the automatic exposure
// settles on the EV100 that exposes the frame's mean luminance as an
// averaging meter would, offset by the compensation; a debug view is
// neither bloomed nor metered. The fixture's teardown asserts zero
// validation and synchronisation-validation errors.
#include <nepheline/math/scalar.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture Post;

NTEST_SETUP(Post)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Post)
{
    test_render_teardown(f);
}

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)

// ---------------------------------------------------------------------------
// The metering's arithmetic
// ---------------------------------------------------------------------------

static f32 bin_centre(u32 b)
{
    return (f32)NPH_RENDER_EXPOSURE_MIN_LOG2 +
           ((f32)b + 0.5F) * ((f32)NPH_RENDER_EXPOSURE_RANGE_LOG2 / (f32)NPH_RENDER_EXPOSURE_BINS);
}

// One bin gives its centre; the darkest half and the brightest twentieth
// are left out.
NTEST(post, the_metering_takes_the_middle_of_the_histogram)
{
    u32 h[NPH_RENDER_EXPOSURE_BINS];
    memset(h, 0, sizeof h);
    h[70] = 1000U;
    f32 mean = 0.0F;
    u32 count = 0U;
    render_exposure_average(h, &mean, &count);
    NTEST_EXPECT_EQ_UINT(count, 1000U);
    NTEST_EXPECT_NEAR_F32(mean, bin_centre(70U), 1e-5F);
    // Half the pixels dark: the meter reads the bright half alone.
    h[10] = 1000U;
    h[70] = 0U;
    h[100] = 1000U;
    render_exposure_average(h, &mean, &count);
    NTEST_EXPECT_EQ_UINT(count, 2000U);
    NTEST_EXPECT_NEAR_F32(mean, bin_centre(100U), 1e-5F);
    // A few highlights (2 %) above the 95th percentile do not count.
    memset(h, 0, sizeof h);
    h[60] = 980U;
    h[127] = 20U;
    render_exposure_average(h, &mean, &count);
    NTEST_EXPECT_NEAR_F32(mean, bin_centre(60U), 1e-5F);
    // Nothing counted: the bottom of the range.
    memset(h, 0, sizeof h);
    render_exposure_average(h, &mean, &count);
    NTEST_EXPECT_EQ_UINT(count, 0U);
    NTEST_EXPECT_NEAR_F32(mean, (f32)NPH_RENDER_EXPOSURE_MIN_LOG2, 1e-6F);
}

// EV100 = log2(L 100 / 12.5): a metered mean luminance L comes out as
// L * exposure = 1 / 9.6 of the HDR scale, whatever the exposure it was
// metered with; the compensation brightens by stops; the range clamps.
NTEST(post, the_metered_ev100_exposes_the_mean_like_an_averaging_meter)
{
    const f32 luminance = 1000.0F; // cd/m^2
    const f32 metered_at = nph_render_exposure_from_ev100(9.0F);
    const f32 average = nph_f32_log2(luminance * metered_at);
    const f32 ev100 = render_exposure_target_ev100(average, metered_at, 0.0F);
    NTEST_EXPECT_NEAR_F32(ev100, nph_f32_log2(luminance * 8.0F), 1e-4F);
    NTEST_EXPECT_NEAR_F32(luminance * nph_render_exposure_from_ev100(ev100), 1.0F / 9.6F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(render_exposure_target_ev100(average, metered_at, 1.0F), ev100 - 1.0F,
                          1e-4F);
    NTEST_EXPECT_NEAR_F32(render_exposure_target_ev100(40.0F, 1.0F, 0.0F), 20.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(render_exposure_target_ev100(-40.0F, 1.0F, 0.0F), -6.0F, 1e-6F);
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

static f32 linear_of(u8 v)
{
    return nph_render_ref_srgb_to_linear((f32)v / 255.0F);
}

// A view over a solid background, the tonemapper NONE: a pixel is the sRGB
// encoding of its pre-exposed colour.
static void post_view(NphRenderView *v, NphRenderPipeline pipeline, f32 background)
{
    nph_render_view_default(v, test_camera());
    v->pipeline = pipeline;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    // EV100 0: the exposure is 1 / 1.2.
    v->ev100 = 0.0F;
    v->background_color = nph_vec3(background * 1.2F, background * 1.2F, background * 1.2F);
    v->disable_ibl = 1;
}

static void render_into(RenderFixture *f, const NphRenderView *v, u32 frames, u8 *out)
{
    for (u32 i = 0; i + 1U < frames; ++i) {
        (void)test_render_frame(f, v, f->target);
    }
    (void)test_render_read(f, v);
    memcpy(out, f->pixels, PIXELS);
}

// The bloom of a uniform image is the image: every filter sums to one, and
// the chain's sum is scaled back to one blur. On both pipelines, and on the
// production path under temporal anti-aliasing (whose history's and the
// chain's random rounding move a level or two).
NTEST_F(Post, bloom_keeps_a_uniform_image)
{
    REQUIRE_RENDERER(f);
    static u8 plain[PIXELS];
    static u8 bloomed[PIXELS];
    for (u32 i = 0; i < 3U; ++i) {
        const bool production = i != 0U;
        if (production && !nph_renderer_production_supported(f->renderer)) {
            continue;
        }
        NphRenderView v;
        post_view(&v, production ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE,
                  0.3F);
        v.anti_aliasing = i == 2U ? NPH_RENDER_AA_TEMPORAL : NPH_RENDER_AA_NONE;
        render_into(f, &v, 4U, plain);
        v.bloom = 0.5F;
        render_into(f, &v, 4U, bloomed);
        u32 worst = 0U;
        for (u64 p = 0; p < PIXELS; ++p) {
            const i32 d = (i32)plain[p] - (i32)bloomed[p];
            worst = nph_max_u32(worst, (u32)(d < 0 ? -d : d));
        }
        NTEST_LOG("pass %u: largest difference %u levels", i, worst);
        NTEST_EXPECT_LE_UINT(worst, 2U);
    }
}

// A small bright square over black: bloom brightens its surroundings in
// every direction alike, falling with distance, and the image's energy
// stays where it was (the mix takes from the square what it spreads).
NTEST_F(Post, bloom_spreads_a_highlight_and_keeps_the_energy)
{
    REQUIRE_RENDERER(f);
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.9F, 0.9F, 0.9F, 1.0F);
    md.unlit = 1;
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    // A cube 0.25 m across at 4 m: about six pixels.
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 0.0, 0.0), nph_quat_identity(),
                                                 nph_vec3(0.25F, 0.25F, 0.25F));
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    static u8 plain[PIXELS];
    static u8 bloomed[PIXELS];
    NphRenderView v;
    post_view(&v, NPH_RENDER_PIPELINE_REFERENCE, 0.0F);
    render_into(f, &v, 3U, plain);
    v.bloom = 0.3F;
    render_into(f, &v, 3U, bloomed);
    f64 before = 0.0;
    f64 after = 0.0;
    for (u64 p = 0; p < PIXELS; p += 4U) {
        before += (f64)linear_of(plain[p]);
        after += (f64)linear_of(bloomed[p]);
    }
    NTEST_LOG("energy: %.3f without bloom, %.3f with", before, after);
    NTEST_EXPECT_TRUE(before > 1.0);
    NTEST_EXPECT_TRUE(nph_f64_abs(after - before) < before * 0.05);
    // The glow, 8 pixels out along each axis from the centre, is lit,
    // alike in the four directions, and dimmer at 16.
    const u32 c = TEST_TARGET_SIZE / 2U;
    const f32 right = linear_of(bloomed[((u64)c * TEST_TARGET_SIZE + c + 8U) * 4U]);
    const f32 left = linear_of(bloomed[((u64)c * TEST_TARGET_SIZE + c - 8U) * 4U]);
    const f32 down = linear_of(bloomed[((u64)(c + 8U) * TEST_TARGET_SIZE + c) * 4U]);
    const f32 up = linear_of(bloomed[((u64)(c - 8U) * TEST_TARGET_SIZE + c) * 4U]);
    const f32 far = linear_of(bloomed[((u64)c * TEST_TARGET_SIZE + c + 16U) * 4U]);
    NTEST_LOG("glow at 8 px: %.4f %.4f %.4f %.4f; at 16 px: %.4f", (f64)right, (f64)left, (f64)down,
              (f64)up, (f64)far);
    NTEST_EXPECT_EQ_UINT((u32)plain[((u64)c * TEST_TARGET_SIZE + c + 8U) * 4U], 0U);
    NTEST_EXPECT_TRUE(right > 0.0F && left > 0.0F && down > 0.0F && up > 0.0F);
    const f32 lo = nph_f32_min(nph_f32_min(right, left), nph_f32_min(down, up));
    const f32 hi = nph_f32_max(nph_f32_max(right, left), nph_f32_max(down, up));
    NTEST_EXPECT_TRUE(hi < lo * 1.5F);
    NTEST_EXPECT_TRUE(far < lo);
}

// The automatic exposure settles on the EV100 that shows a uniform
// background of luminance L as an averaging meter would (L * exposure =
// 1 / 9.6), on both pipelines, a stop brighter with a compensation of 1;
// the metering arrives NPH_GPU_FRAMES_IN_FLIGHT frames late and the
// exposure moves at the view's speed.
NTEST_F(Post, the_automatic_exposure_settles_on_the_meter)
{
    REQUIRE_RENDERER(f);
    for (u32 i = 0; i < 2U; ++i) {
        const bool production = i != 0U;
        if (production && !nph_renderer_production_supported(f->renderer)) {
            continue;
        }
        NphRenderView v;
        post_view(&v, production ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE,
                  0.0F);
        const f32 luminance = 2000.0F;
        v.background_color = nph_vec3(luminance, luminance, luminance);
        v.exposure = NPH_RENDER_EXPOSURE_AUTO;
        v.ev100 = 4.0F;
        // A second a frame at 4 e-foldings a second: within 0.02 EV in two frames.
        v.frame_seconds = 1.0F;
        v.exposure_speed = 4.0F;
        const f32 expected = nph_f32_log2(luminance * 8.0F);
        NphRenderFrameStats s = test_render_frame(f, &v, f->target);
        NTEST_EXPECT_NEAR_F32(s.ev100, 4.0F, 1e-6F); // nothing metered yet
        for (u32 k = 0; k < 8U; ++k) {
            s = test_render_frame(f, &v, f->target);
        }
        NTEST_LOG("pipeline %u: settled at EV100 %.3f (metered %.3f; expected %.3f)", i,
                  (f64)s.ev100, (f64)s.metered_ev100, (f64)expected);
        // The meter reads the background's bin: an eighth of a stop.
        NTEST_EXPECT_NEAR_F32(s.metered_ev100, expected, 0.07F);
        NTEST_EXPECT_NEAR_F32(s.ev100, s.metered_ev100, 0.02F);
        s = test_render_read(f, &v);
        const f32 shown = linear_of(f->pixels[0]);
        NTEST_EXPECT_NEAR_F32(shown, 1.0F / 9.6F, 0.01F);
        v.exposure_compensation = 1.0F;
        for (u32 k = 0; k < 8U; ++k) {
            s = test_render_frame(f, &v, f->target);
        }
        NTEST_EXPECT_NEAR_F32(s.ev100, expected - 1.0F, 0.08F);
        // A manual view takes its own EV100 again.
        v.exposure = NPH_RENDER_EXPOSURE_MANUAL;
        s = test_render_frame(f, &v, f->target);
        NTEST_EXPECT_NEAR_F32(s.ev100, 4.0F, 1e-6F);
    }
}

// A debug view shows the scene's data as they are: no bloom, and no
// metering, the automatic exposure holding where the lit view left it.
NTEST_F(Post, debug_views_are_neither_bloomed_nor_metered)
{
    REQUIRE_RENDERER(f);
    static u8 plain[PIXELS];
    static u8 bloomed[PIXELS];
    for (u32 i = 0; i < 2U; ++i) {
        const bool production = i != 0U;
        if (production && !nph_renderer_production_supported(f->renderer)) {
            continue;
        }
        NphRenderView v;
        post_view(&v, production ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE,
                  0.0F);
        v.background_color = nph_vec3(2000.0F, 2000.0F, 2000.0F);
        v.exposure = NPH_RENDER_EXPOSURE_AUTO;
        v.ev100 = 4.0F;
        v.frame_seconds = 1.0F;
        v.exposure_speed = 4.0F;
        v.bloom = 1.0F;
        NphRenderFrameStats s;
        memset(&s, 0, sizeof s);
        for (u32 k = 0; k < 8U; ++k) {
            s = test_render_frame(f, &v, f->target);
        }
        NTEST_EXPECT_TRUE(s.bloom);
        const f32 settled = s.ev100;
        NTEST_EXPECT_TRUE(settled > 10.0F); // metered: far from 4
        v.debug_view = NPH_RENDER_VIEW_NORMAL;
        // The meterings still in flight arrive (the lit frames' last), then
        // nothing moves the exposure: metered, the normals' values would
        // move it by stops.
        for (u32 k = 0; k < NPH_GPU_FRAMES_IN_FLIGHT + 1U; ++k) {
            s = test_render_frame(f, &v, f->target);
            NTEST_EXPECT_FALSE(s.bloom);
        }
        const f32 held = s.ev100;
        NTEST_EXPECT_NEAR_F32(held, settled, 0.07F); // within the meter's bin
        for (u32 k = 0; k < 4U; ++k) {
            s = test_render_frame(f, &v, f->target);
            NTEST_EXPECT_FALSE(s.bloom);
            NTEST_EXPECT_NEAR_F32(s.ev100, held, 1e-3F);
        }
        render_into(f, &v, 2U, bloomed);
        v.bloom = 0.0F;
        render_into(f, &v, 2U, plain);
        NTEST_EXPECT_EQ_INT(memcmp(plain, bloomed, PIXELS), 0);
    }
}
