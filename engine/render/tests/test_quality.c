// SPDX-License-Identifier: Apache-2.0
// The quality tiers (render/renderer.h NphRenderQuality, nph_render_view_quality; ADR-0079;
// docs/render.md "Quality tiers"): a tier sets its fields and leaves the others, the reference
// tier is the default view, and the high tier, every one of its rays at once, draws the boxes
// under a sun and a sky as the reference tier does.
#include "render_internal.h"
#include "test_boxes.h"

#include <string.h>

typedef RenderFixture Quality;

NTEST_SETUP(Quality)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Quality)
{
    test_render_teardown(f);
}

#define SIZE   TEST_BOX_SIZE
#define PIXELS TEST_BOX_PIXELS
// Frames for the radiance cache to fill (its history holds 128 samples; at this size a cell takes
// several a frame). The reference tier has twice as many before its image, so that the cache is as
// full for both images.
#define FRAMES 48U

static NphRenderCamera any_camera(void)
{
    return nph_render_camera_look_at(nph_dvec3(0.0, 1.0, 5.0), nph_dvec3(0.0, 0.0, 0.0),
                                     nph_vec3_unit_y(), 0.8F, 0.1F);
}

// Every field of two views alike (field by field: the struct has padding).
static bool same_view(const NphRenderView *a, const NphRenderView *b)
{
    const NphRenderCamera *ca = &a->camera;
    const NphRenderCamera *cb = &b->camera;
    return ca->position.x == cb->position.x && ca->position.y == cb->position.y &&
           ca->position.z == cb->position.z && ca->orientation.x == cb->orientation.x &&
           ca->orientation.y == cb->orientation.y && ca->orientation.z == cb->orientation.z &&
           ca->orientation.w == cb->orientation.w && ca->yfov == cb->yfov &&
           ca->znear == cb->znear && a->ev100 == b->ev100 && a->tonemapper == b->tonemapper &&
           a->background == b->background && a->background_color.x == b->background_color.x &&
           a->background_color.y == b->background_color.y &&
           a->background_color.z == b->background_color.z && a->debug_view == b->debug_view &&
           a->disable_ibl == b->disable_ibl && a->disable_many_lights == b->disable_many_lights &&
           a->reflections == b->reflections && a->contact_shadow == b->contact_shadow &&
           a->many_restir == b->many_restir && a->pipeline == b->pipeline &&
           a->vis_flags == b->vis_flags && a->anti_aliasing == b->anti_aliasing &&
           a->camera_cut == b->camera_cut && a->exposure == b->exposure &&
           a->exposure_compensation == b->exposure_compensation &&
           a->exposure_speed == b->exposure_speed && a->frame_seconds == b->frame_seconds &&
           a->bloom == b->bloom && a->shutter == b->shutter && a->aperture == b->aperture &&
           a->focus_distance == b->focus_distance && a->sensor_height == b->sensor_height &&
           a->lens_offset.x == b->lens_offset.x && a->lens_offset.y == b->lens_offset.y &&
           a->ao_radius == b->ao_radius && a->lod_error == b->lod_error && a->gi == b->gi &&
           a->gi_rays == b->gi_rays;
}

NTEST(quality, the_reference_tier_is_the_default_view)
{
    NphRenderView a;
    nph_render_view_default(&a, any_camera());
    NphRenderView b = a;
    nph_render_view_quality(&b, NPH_RENDER_QUALITY_REFERENCE);
    NTEST_EXPECT_TRUE(same_view(&a, &b));
}

// A tier sets what it decides and nothing else; the reference tier after it gives a view back its
// defaults there, and the view's own flags stay.
NTEST(quality, a_tier_sets_its_fields_and_leaves_the_others)
{
    NphRenderView v;
    nph_render_view_default(&v, any_camera());
    v.ev100 = 3.0F;
    v.ao_radius = 0.7F;
    v.lod_error = 0.5F;
    v.contact_shadow = 12.0F;
    v.gi = 1;
    v.vis_flags = (u32)NPH_RENDER_VIS_NO_CONE;
    const NphRenderView before = v;
    nph_render_view_quality(&v, NPH_RENDER_QUALITY_HIGH);
    NTEST_EXPECT_EQ_INT((int)v.reflections, (int)NPH_RENDER_REFLECTIONS_RAYS);
    NTEST_EXPECT_EQ_UINT(v.vis_flags, (u32)NPH_RENDER_VIS_NO_CONE | (u32)NPH_RENDER_VIS_RT_SHADOWS);
    NTEST_EXPECT_EQ_UINT(v.gi_rays, 4U * 65536U);
    NphRenderView rest = v;
    rest.reflections = before.reflections;
    rest.vis_flags = before.vis_flags;
    rest.gi_rays = before.gi_rays;
    NTEST_EXPECT_TRUE(same_view(&rest, &before));
    // A tier past the last is the last.
    NphRenderView past = before;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    nph_render_view_quality(&past, (NphRenderQuality)(NPH_RENDER_QUALITY_COUNT + 3U));
    NTEST_EXPECT_TRUE(same_view(&past, &v));
    nph_render_view_quality(&v, NPH_RENDER_QUALITY_REFERENCE);
    NTEST_EXPECT_TRUE(same_view(&v, &before));
}

// The high tier draws the boxes as the reference tier does: under a low sun and an even sky, with a
// smooth metal box, the lit pixels' mean within 4 %. Its sun's shadows are rays where the
// reference's are its map's, its smooth surfaces' reflections a ray a pixel, and the cache traces
// four times the rays.
NTEST_F(Quality, the_high_tier_draws_what_the_reference_does)
{
    REQUIRE_PRODUCTION(f);
    if (!f->renderer->gi.supported) {
        NTEST_SKIP("no radiance cache on this device (ray queries)");
    }
    const NphRenderTexture env = test_uniform_environment(f, 0.3F);
    static TestBoxScene scene;
    const NphRenderMesh cube = test_cube_mesh(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.9F, 0.9F, 0.9F, 1.0F);
    md.metallic = 1.0F;
    md.roughness = 0.1F;
    NphRenderMaterial metal = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &metal), NPH_RESULT_OK);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, cube, test_grey(f, 0.6F), metal);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 4.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(-0.6F, -0.5F, -0.4F));
    ld.shadows = 1;
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    NphRenderView v;
    test_box_view(&v, camera, NPH_RENDER_VIEW_LIT);
    v.gi = 1;
    static u8 reference[PIXELS];
    for (u32 i = 0; i + 1U < 2U * FRAMES; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    const NphRenderFrameStats rs = test_box_render(f, &v, target, reference);
    NTEST_EXPECT_TRUE(rs.vsm);
    nph_render_view_quality(&v, NPH_RENDER_QUALITY_HIGH);
    static u8 high[PIXELS];
    for (u32 i = 0; i + 1U < FRAMES; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    const NphRenderFrameStats hs = test_box_render(f, &v, target, high);
    NTEST_EXPECT_TRUE(hs.gi);
    NTEST_EXPECT_FALSE(hs.vsm);
    NTEST_EXPECT_TRUE(hs.shadow_rays);
    NTEST_EXPECT_GE_UINT(hs.gi_rays, rs.gi_rays);
    f64 sum_reference = 0.0;
    f64 sum_high = 0.0;
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) < 0) {
            continue;
        }
        sum_reference += (f64)test_box_linear(reference, i);
        sum_high += (f64)test_box_linear(high, i);
        ++count;
    }
    NTEST_ASSERT_GT_UINT(count, SIZE * SIZE / 2U);
    const f64 m_reference = sum_reference / (f64)count;
    const f64 m_high = sum_high / (f64)count;
    NTEST_LOG("the boxes' lit mean: %.4f in the reference tier, %.4f in the high", m_reference,
              m_high);
    NTEST_EXPECT_TRUE(m_reference > 0.05);
    NTEST_EXPECT_TRUE(m_high > m_reference * 0.96);
    NTEST_EXPECT_TRUE(m_high < m_reference * 1.04);
    nph_render_light_destroy(f->renderer, sun);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}
