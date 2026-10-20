// SPDX-License-Identifier: Apache-2.0
// Ambient occlusion (render/renderer.h NphRenderView.ao_radius; docs/render.md
// "Ambient occlusion"; engine/render/src/ao.c), against a ground truth the
// test computes itself: each pixel's surface found by casting its ray at the
// scene's boxes, and the cosine-weighted visibility of its hemisphere
// integrated by stratified rays against them, occluders fading over the last
// part of the radius as the shader's do. And how it shades: the
// environment's light only, the diffuse part with what the occluders
// reflect back (render/reference.h nph_render_ref_ao_bounce).
#include "render_internal.h"
#include "test_boxes.h"

#include <math.h>

typedef RenderFixture Ao;

NTEST_SETUP(Ao)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Ao)
{
    test_render_teardown(f);
}

// The ground truth's samples per axis (S x S cosine-weighted directions a
// pixel), and the occlusion's radius and fade.
#define STRATA  24U
#define RADIUS  0.5F
#define FALLOFF 0.1F // the shader's AO_FALLOFF
#define SIZE    TEST_BOX_SIZE
#define PIXELS  TEST_BOX_PIXELS

// ---------------------------------------------------------------------------
// The ground truth
// ---------------------------------------------------------------------------

// The occlusion at `x` with normal `n`: the cosine-weighted share of the
// hemisphere left open, an occluder at distance d counting
// clamp((1 - d / RADIUS) / FALLOFF, 0, 1).
static f32 truth_at(const TestBoxScene *s, NphVec3 x, NphVec3 n)
{
    const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
    f32 open = 0.0F;
    for (u32 i = 0; i < STRATA; ++i) {
        for (u32 j = 0; j < STRATA; ++j) {
            f32 dist = 0.0F;
            NphVec3 hn;
            f32 occluded = 0.0F;
            if (test_box_hit(s, o, test_cosine_direction(n, i, j, STRATA), 0.0F, &dist, &hn) >= 0) {
                occluded = nph_f32_clamp((1.0F - dist / RADIUS) / FALLOFF, 0.0F, 1.0F);
            }
            open += 1.0F - occluded;
        }
    }
    return open / (f32)(STRATA * STRATA);
}

// The ground truth's occlusion of each pixel (-1 where its ray meets
// nothing).
static void truth_image(const TestBoxScene *s, f32 *out)
{
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(s, i);
        f32 t = 0.0F;
        NphVec3 n;
        out[i] = test_box_hit(s, s->eye, d, 0.0F, &t, &n) >= 0
                     ? truth_at(s, nph_vec3_add(s->eye, nph_vec3_scale(d, t)), n)
                     : -1.0F;
    }
}

// The production view of the boxes (test_box_view) with the occlusion's
// radius.
static void ao_view(NphRenderView *v, NphRenderCamera camera, NphRenderDebugView debug, f32 radius)
{
    test_box_view(v, camera, debug);
    v->ao_radius = radius;
}

// ---------------------------------------------------------------------------
// Against the ground truth
// ---------------------------------------------------------------------------

// A box on the ground: along its foot the occlusion comes within a quarter
// of how much the ground truth is occluded, and the ground that nothing
// occludes stays open. (Behind the box's silhouette the depth cannot tell
// how thick it is, and a few pixels there are occluded as if it went on:
// screen-space occlusion's limit.)
NTEST_F(Ao, the_occlusion_comes_near_the_ground_truth)
{
    REQUIRE_PRODUCTION(f);
    static TestBoxScene scene;
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, test_cube_mesh(f), grey, grey);
    static f32 truth[SIZE * SIZE];
    truth_image(&scene, truth);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    ao_view(&v, camera, NPH_RENDER_VIEW_OCCLUSION, RADIUS);
    NTEST_EXPECT_TRUE(test_box_render(f, &v, target, image).ao);
    f64 err = 0.0;
    f64 shut = 0.0;
    f64 bias = 0.0;
    u32 occluded = 0U;
    f64 open_err = 0.0;
    u32 open = 0U;
    u32 open_off = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const f32 t = truth[i];
        if (t < 0.0F) {
            continue;
        }
        const f32 e = test_box_linear(image, i) - t;
        if (t < 0.9F) {
            err += (f64)nph_f32_abs(e);
            shut += (f64)(1.0F - t);
            bias += (f64)e;
            ++occluded;
        } else if (t >= 0.999F) {
            open_err += (f64)nph_f32_abs(e);
            open_off += nph_f32_abs(e) > 0.03F ? 1U : 0U;
            ++open;
        }
    }
    NTEST_LOG("occluded: %u pixels, error %.3f of the occlusion, bias %+.4f; open: %u pixels, "
              "mean error %.4f, %.1f%% off by over 0.03",
              occluded, err / shut, bias / (f64)occluded, open, open_err / (f64)open,
              100.0 * (f64)open_off / (f64)open);
    NTEST_ASSERT_GT_UINT(occluded, 500U);
    NTEST_ASSERT_GT_UINT(open, 5000U);
    NTEST_EXPECT_TRUE(err < 0.25 * shut);
    NTEST_EXPECT_TRUE(nph_f64_abs(bias / (f64)occluded) < 0.08);
    NTEST_EXPECT_TRUE(open_err / (f64)open < 0.01);
    NTEST_EXPECT_TRUE((f64)open_off < 0.02 * (f64)open);
    test_box_release(f, target);
}

// The ground alone occludes nothing, out to its horizon and at the image's
// edges: the samples along a slice lie on the plane however steeply it is
// seen (ao_common.glsl ao_depth_at), and a window at the edge still holds
// every direction (ao_denoise.comp).
NTEST_F(Ao, flat_ground_stays_open)
{
    REQUIRE_PRODUCTION(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, -2.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    ao_view(&v, camera, NPH_RENDER_VIEW_OCCLUSION, RADIUS);
    NTEST_EXPECT_TRUE(test_box_render(f, &v, target, image).ao);
    // Every pixel whose ray meets the ground should be open: nothing else is
    // there to occlude it.
    u32 ground = 0U;
    f32 worst = 0.0F;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) >= 0) {
            worst = nph_f32_max(worst, 1.0F - test_box_linear(image, i));
            ++ground;
        }
    }
    NTEST_LOG("%u ground pixels, the most occluded %.4f", ground, (f64)worst);
    NTEST_ASSERT_GT_UINT(ground, SIZE * SIZE / 2U);
    NTEST_EXPECT_TRUE(worst < 0.02F);
    test_box_release(f, target);
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------

// The diffuse light under occlusion A takes back what occluders of the
// surface's albedo reflect (Jimenez et al. 2016 Eq. 10). Without occlusion
// a pixel's environment light is S + albedo K: the specular part S the same
// for either grey, the diffuse part proportional to the albedo, so two
// albedos give both. With it the pixel shows A S + b albedo K, the specular
// part occluded by A (at roughness 1 surface.glsl's specular occlusion is
// A) and the diffuse part by b, which nph_render_ref_ao_bounce gives: more
// than A for the bright grey, about A for the dark one.
NTEST_F(Ao, the_diffuse_light_takes_back_what_the_occluders_reflect)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderTexture env = test_uniform_environment(f, 1.0F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static const f32 albedo[2] = {0.8F, 0.2F};
    static TestBoxScene scenes[2];
    // The same boxes twice, seen alike.
    const NphRenderMaterial dark = test_grey(f, albedo[1]);
    (void)test_box_on_ground(f, &scenes[1], mesh, dark, dark);
    const NphRenderMaterial bright = test_grey(f, albedo[0]);
    const NphRenderCamera camera = test_box_on_ground(f, &scenes[0], mesh, bright, bright);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 occlusion[PIXELS];
    static u8 open[2][PIXELS];
    static u8 shut[2][PIXELS];
    NphRenderView v;
    for (u32 k = 0; k < 2U; ++k) {
        test_box_show(f, &scenes[k], true);
        test_box_show(f, &scenes[1U - k], false);
        if (k == 0U) {
            ao_view(&v, camera, NPH_RENDER_VIEW_OCCLUSION, RADIUS);
            NTEST_EXPECT_TRUE(test_box_render(f, &v, target, occlusion).ao);
        }
        ao_view(&v, camera, NPH_RENDER_VIEW_INDIRECT, 0.0F);
        NTEST_EXPECT_FALSE(test_box_render(f, &v, target, open[k]).ao);
        ao_view(&v, camera, NPH_RENDER_VIEW_INDIRECT, RADIUS);
        NTEST_EXPECT_TRUE(test_box_render(f, &v, target, shut[k]).ao);
    }
    f64 err[2] = {0.0, 0.0};
    f64 gain[2] = {0.0, 0.0};
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        // Occluded pixels, with room for the bounce to show.
        const f32 a = test_box_linear(occlusion, i);
        if (occlusion[(u64)i * 4U] == 0U || a < 0.3F || a > 0.85F) {
            continue;
        }
        const f32 k_diffuse =
            (test_box_linear(open[0], i) - test_box_linear(open[1], i)) / (albedo[0] - albedo[1]);
        const f32 specular = test_box_linear(open[0], i) - albedo[0] * k_diffuse;
        for (u32 k = 0; k < 2U; ++k) {
            const f32 b = (test_box_linear(shut[k], i) - a * specular) / (albedo[k] * k_diffuse);
            const f32 expected = nph_render_ref_ao_bounce(a, nph_vec3(albedo[k], 0.0F, 0.0F)).x;
            err[k] += (f64)nph_f32_abs(b - expected);
            gain[k] += (f64)(b - a);
        }
        ++count;
    }
    NTEST_LOG("%u pixels: bright grey off by %.4f, above the occlusion by %.3f; dark grey off "
              "by %.4f, above it by %.3f",
              count, err[0] / (f64)count, gain[0] / (f64)count, err[1] / (f64)count,
              gain[1] / (f64)count);
    NTEST_ASSERT_GT_UINT(count, 200U);
    NTEST_EXPECT_TRUE(err[0] / (f64)count < 0.015);
    NTEST_EXPECT_TRUE(err[1] / (f64)count < 0.02);
    NTEST_EXPECT_TRUE(gain[0] / (f64)count > 0.1);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// It darkens only the environment's light: lit by a sun alone, a view
// makes no occlusion and shows the frame it shows without; lit by the
// environment too, it is nowhere brighter and darker along the box's foot.
NTEST_F(Ao, it_darkens_only_the_environments_light)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderTexture env = test_uniform_environment(f, 1.0F);
    static TestBoxScene scene;
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, test_cube_mesh(f), grey, grey);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 1.0F, 1.0F);
    ld.intensity = 1.5F;
    ld.direction = nph_vec3_normalize(nph_vec3(-0.3F, -1.0F, -0.5F));
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    NphRenderView v;
    ao_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    v.disable_ibl = 1;
    NTEST_EXPECT_FALSE(test_box_render(f, &v, target, without).ao);
    v.ao_radius = RADIUS;
    NTEST_EXPECT_FALSE(test_box_render(f, &v, target, with).ao);
    NTEST_EXPECT_EQ_INT(memcmp(without, with, PIXELS), 0);

    v.disable_ibl = 0;
    v.ao_radius = 0.0F;
    NTEST_EXPECT_FALSE(test_box_render(f, &v, target, without).ao);
    v.ao_radius = RADIUS;
    NTEST_EXPECT_TRUE(test_box_render(f, &v, target, with).ao);
    u32 brighter = 0U;
    u32 darker = 0U;
    for (u64 i = 0; i < PIXELS; ++i) {
        brighter += with[i] > without[i] ? 1U : 0U;
        darker += with[i] + 4U < without[i] ? 1U : 0U;
    }
    NTEST_LOG("%u channels darker by over 4, %u brighter", darker, brighter);
    NTEST_EXPECT_EQ_UINT(brighter, 0U);
    NTEST_EXPECT_GT_UINT(darker, 1000U);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// ---------------------------------------------------------------------------
// Which frames have it
// ---------------------------------------------------------------------------

// The production path makes the occlusion for the views that show the
// environment's light, while there is an environment, and for its own
// debug view; never for the reference renderer or a radius of 0.
NTEST_F(Ao, views_that_show_it_make_it)
{
    REQUIRE_PRODUCTION(f);
    NphRenderView v;
    ao_view(&v, test_camera(), NPH_RENDER_VIEW_LIT, RADIUS);
    // No environment yet (black): only the occlusion's own view.
    for (u32 d = 0; d < (u32)NPH_RENDER_VIEW_COUNT; ++d) {
        v.debug_view = (NphRenderDebugView)d;
        NTEST_EXPECT_EQ_INT(test_render_frame(f, &v, f->target).ao,
                            d == (u32)NPH_RENDER_VIEW_OCCLUSION ? 1 : 0);
    }
    const NphRenderTexture env = test_uniform_environment(f, 1.0F);
    for (u32 d = 0; d < (u32)NPH_RENDER_VIEW_COUNT; ++d) {
        v.debug_view = (NphRenderDebugView)d;
        const bool shown = d == (u32)NPH_RENDER_VIEW_LIT || d == (u32)NPH_RENDER_VIEW_OCCLUSION ||
                           d == (u32)NPH_RENDER_VIEW_INDIRECT;
        NTEST_EXPECT_EQ_INT(test_render_frame(f, &v, f->target).ao, shown ? 1 : 0);
    }
    v.debug_view = NPH_RENDER_VIEW_LIT;
    v.disable_ibl = 1;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    v.disable_ibl = 0;
    v.ao_radius = 0.0F;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    v.ao_radius = RADIUS;
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    v.debug_view = NPH_RENDER_VIEW_OCCLUSION;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// The radius is a length: finite and not negative.
NTEST_F(Ao, the_radius_must_be_finite_and_not_negative)
{
    REQUIRE_RENDERER(f);
    const f32 bad[3] = {-0.5F, (f32)NAN, NPH_F32_INFINITY};
    NphRenderView v;
    ao_view(&v, test_camera(), NPH_RENDER_VIEW_LIT, RADIUS);
    for (u32 i = 0; i < 3U; ++i) {
        v.ao_radius = bad[i];
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        nph_gpu_rg_begin(f->graph);
        NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                            NPH_RESULT_INVALID_ARGUMENT);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
}
