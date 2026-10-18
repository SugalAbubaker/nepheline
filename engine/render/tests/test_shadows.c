// SPDX-License-Identifier: Apache-2.0
// Ray-traced shadows (render/renderer.h NphRenderLightDesc.shadows;
// docs/render.md "Shadows"): on both pipelines, what an occluder hides from
// a light is dark exactly where the geometry says (every pixel classified
// analytically, away from the shadow's edges) and nothing else changes; a
// shadow ray stops at its light; the structures follow the scene (instances
// that move, hide, and go; meshes created later); and a renderer without
// ray tracing draws the same lights unshadowed. Skipped where the device
// has no ray queries; the fixture's teardown asserts zero validation and
// synchronisation-validation errors and nothing leaked.
#include "test_shadow_support.h"

typedef RenderFixture Shadows;

// The production path's sun ray traced too: these tests are the rays'
// (test_vsm.c has the virtual shadow map's).
#define RT_FLAGS ((u32)NPH_RENDER_VIS_RT_SHADOWS)

// Ray-traced shadows are exact: half a pixel covers the raster's sampling.
#define RT_GROW 0.5F

NTEST_SETUP(Shadows)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Shadows)
{
    test_render_teardown(f);
}

#define REQUIRE_RAY_TRACING(f)                                                                     \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_ray_tracing_supported((f)->renderer)) {                                  \
            NTEST_SKIP("no ray queries on this device");                                           \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The sun behind a cube: on both pipelines, the floor the cube hides from
// it is black, and every other pixel is as it was without shadows.
NTEST_F(Shadows, an_occluder_darkens_exactly_what_it_hides)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        NphRenderView v;
        shadow_view(&v, cam, 13.0F, k_pipelines[k], RT_FLAGS);
        set_shadows(f, sun, &ld, false);
        NphRenderFrameStats stats = render_into(f, &v, off);
        NTEST_EXPECT_FALSE(stats.shadow_rays);
        set_shadows(f, sun, &ld, true);
        stats = render_into(f, &v, on);
        NTEST_EXPECT_TRUE(stats.shadow_rays);
        NTEST_EXPECT_EQ_INT((int)stats.pipeline, (int)k_pipelines[k]);
        const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder, RT_GROW);
        log_check("sun behind a cube", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 100U);
        NTEST_EXPECT_GT_UINT(c.lit, 2000U);
        NTEST_EXPECT_GT_UINT(c.occluder, 100U);
    }
}

// A caster whose record follows a hidden instance's and a destroyed one's
// still casts, on both pipelines. Vulkan allows inactive instance records
// anywhere, but lavapipe drops an active one that follows them, so the
// renderer keeps the active records first (rt.c); released slots wait 16
// allocations before reuse, so the four instances take slots 0 to 3.
NTEST_F(Shadows, a_caster_after_hidden_and_freed_slots_still_casts)
{
    REQUIRE_RAY_TRACING(f);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, plane, grey, at(0.0, 0.0, 0.0, 6.0F));
    const NphRenderInstance hidden = instance(f, cube, grey, at(-2.0, 1.0, -2.0, 0.5F));
    nph_render_instance_set_hidden(f->renderer, hidden, true);
    const NphRenderInstance gone = instance(f, cube, grey, at(2.0, 1.0, 2.0, 0.5F));
    nph_render_instance_destroy(f->renderer, gone);
    (void)instance(f, cube, grey, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        NphRenderView v;
        shadow_view(&v, cam, 13.0F, k_pipelines[k], RT_FLAGS);
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        render_into(f, &v, on);
        const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder, RT_GROW);
        log_check("after inactive records", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 100U);
    }
}

// A point light under a cube: the cube shadows the floor below it, but not
// once it is above the light, where an unbounded ray would still meet it.
NTEST_F(Shadows, a_shadow_ray_stops_at_its_light)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(1.0, 0.5, 0.0, 0.4F));
    NphRenderLightDesc ld = point_desc(nph_dvec3(1.0, 1.0, 0.0), false);
    const NphRenderLight lamp = make_light(f, &ld);
    const NphRenderCamera cam = side_camera();
    const Light l = {false, nph_vec3_zero(), nph_vec3(1.0F, 1.0F, 0.0F)};
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        NphRenderView v;
        shadow_view(&v, cam, 5.0F, k_pipelines[k], RT_FLAGS);
        // Between the light and the floor.
        const NphRenderTransform below = at(1.0, 0.5, 0.0, 0.4F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &below),
                            NPH_RESULT_OK);
        set_shadows(f, lamp, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, lamp, &ld, true);
        render_into(f, &v, on);
        const Box between = box_at(nph_vec3(1.0F, 0.5F, 0.0F), 0.4F);
        ShadowCheck c = check_shadow(on, off, &cam, &l, &between, RT_GROW);
        log_check("cube under a point light", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 30U);
        // Above the light: nothing changes.
        const NphRenderTransform above = at(1.0, 2.0, 0.0, 0.5F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &above),
                            NPH_RESULT_OK);
        set_shadows(f, lamp, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, lamp, &ld, true);
        render_into(f, &v, on);
        const Box beyond = box_at(nph_vec3(1.0F, 2.0F, 0.0F), 0.5F);
        c = check_shadow(on, off, &cam, &l, &beyond, RT_GROW);
        log_check("cube over a point light", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_EQ_UINT(c.shadow, 0U);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, off), SAME_TOL);
    }
}

// The structures follow the scene: a mesh's bottom level is built once, in
// the first frame that traces; the top level each frame, so the shadow
// moves, hides, and goes with its instance, and a mesh created later casts
// one from its first frame.
NTEST_F(Shadows, the_structures_follow_the_scene)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    const Light l = {true, ld.direction, nph_vec3_zero()};
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    NphRenderView v;
    shadow_view(&v, cam, 13.0F, NPH_RENDER_PIPELINE_REFERENCE, RT_FLAGS);

    // Frames without shadows build nothing; the first with builds both
    // meshes, and the next nothing.
    NphRenderFrameStats stats = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);
    set_shadows(f, sun, &ld, true);
    stats = test_render_read(f, &v);
    NTEST_EXPECT_TRUE(stats.shadow_rays);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 2U);
    stats = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);
    // A mesh created and destroyed between frames is never built.
    const NphRenderMesh gone = mesh_of(f, make_cube);
    nph_render_mesh_destroy(f->renderer, gone);
    stats = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);

    for (u32 k = 0; k < pipeline_count(f); ++k) {
        shadow_view(&v, cam, 13.0F, k_pipelines[k], RT_FLAGS);
        NphRenderTransform t = at(0.0, 1.0, 1.2, 1.0F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &t),
                            NPH_RESULT_OK);
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        render_into(f, &v, on);
        const Box moved = box_at(nph_vec3(0.0F, 1.0F, 1.2F), 1.0F);
        ShadowCheck c = check_shadow(on, off, &cam, &l, &moved, RT_GROW);
        log_check("moved", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 100U);

        // Hidden: no shadow, no occluder; shown again: as before.
        nph_render_instance_set_hidden(f->renderer, s.occluder, true);
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        render_into(f, &v, on);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, off), SAME_TOL);
        nph_render_instance_set_hidden(f->renderer, s.occluder, false);

        // Gone, and a new mesh's instance in its place: built in the frame
        // that first traces after it, and casting from that frame.
        nph_render_instance_destroy(f->renderer, s.occluder);
        nph_render_mesh_destroy(f->renderer, s.cube);
        s.cube = mesh_of(f, make_cube);
        s.occluder = instance(f, s.cube, s.grey, at(0.0, 1.0, 0.0, 1.0F));
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        stats = test_render_read(f, &v);
        NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 1U);
        memcpy(on, f->pixels, PIXELS);
        const Box again = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
        c = check_shadow(on, off, &cam, &l, &again, RT_GROW);
        log_check("a new mesh", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 100U);
    }
}

// The production path's top level stays as it was built while the camera
// moves within 16 m of where it was (the shadows right from the new
// viewpoint all the same), and is rebuilt around the camera beyond that,
// when an instance changes, after a frame that traced nothing, and after
// the reference drew a frame.
NTEST_F(Shadows, the_top_level_is_rebuilt_only_when_it_must_be)
{
    REQUIRE_RAY_TRACING(f);
    if (!nph_renderer_production_supported(f->renderer)) {
        NTEST_SKIP("no production path on this device");
    }
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    // Straight down from 6, 14 (8 m on), and 30 m (24 m on), the field of
    // view narrowed to keep the floor in frame.
    const f64 heights[3] = {6.0, 14.0, 30.0};
    const f32 fovs[3] = {0.9F, 0.45F, 0.2F};
    NphRenderView views[3];
    static u8 off[3][PIXELS];
    static u8 on[PIXELS];
    for (u32 i = 0; i < 3U; ++i) {
        const NphRenderCamera c =
            nph_render_camera_look_at(nph_dvec3(0.0, heights[i], 0.0), nph_dvec3(0.0, 0.0, 0.0),
                                      nph_vec3(0.0F, 0.0F, -1.0F), fovs[i], 0.05F);
        shadow_view(&views[i], c, 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, RT_FLAGS);
        render_into(f, &views[i], off[i]);
    }
    set_shadows(f, sun, &ld, true);
    // Built once, then kept.
    NphRenderFrameStats stats = test_render_read(f, &views[0]);
    NTEST_EXPECT_TRUE(stats.shadow_rebuild);
    stats = test_render_read(f, &views[0]);
    NTEST_EXPECT_FALSE(stats.shadow_rebuild);
    ShadowCheck c = check_shadow(f->pixels, off[0], &views[0].camera, &l, &occluder, RT_GROW);
    log_check("built at 6 m", "production", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 100U);
    // 8 m on: the same structure, from a camera 8 m from its anchor.
    stats = test_render_read(f, &views[1]);
    NTEST_EXPECT_FALSE(stats.shadow_rebuild);
    memcpy(on, f->pixels, PIXELS);
    c = check_shadow(on, off[1], &views[1].camera, &l, &occluder, RT_GROW);
    log_check("8 m from the anchor", "production", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 50U);
    // 24 m on: rebuilt around the camera, then kept.
    stats = test_render_read(f, &views[2]);
    NTEST_EXPECT_TRUE(stats.shadow_rebuild);
    stats = test_render_read(f, &views[2]);
    NTEST_EXPECT_FALSE(stats.shadow_rebuild);
    c = check_shadow(f->pixels, off[2], &views[2].camera, &l, &occluder, RT_GROW);
    log_check("rebuilt at 30 m", "production", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 50U);
    // An instance changed: rebuilt.
    nph_render_instance_set_hidden(f->renderer, s.occluder, true);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    NTEST_EXPECT_LE_UINT(image_max_diff(f->pixels, off[2]), SAME_TOL);
    nph_render_instance_set_hidden(f->renderer, s.occluder, false);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    NTEST_EXPECT_FALSE(test_render_read(f, &views[2]).shadow_rebuild);
    // A frame without shadows, then one with: rebuilt (the instances may
    // have changed unseen).
    set_shadows(f, sun, &ld, false);
    NTEST_EXPECT_FALSE(test_render_read(f, &views[2]).shadow_rebuild);
    set_shadows(f, sun, &ld, true);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    // The reference rebuilds every frame; the production path after it.
    NphRenderView ref = views[2];
    ref.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    NTEST_EXPECT_TRUE(test_render_read(f, &ref).shadow_rebuild);
    NTEST_EXPECT_TRUE(test_render_read(f, &ref).shadow_rebuild);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    NTEST_EXPECT_FALSE(test_render_read(f, &views[2]).shadow_rebuild);
}

// Shadow rays are traced while some light casts shadows, and only then.
NTEST_F(Shadows, rays_are_traced_while_a_light_casts_shadows)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderView v;
    shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_REFERENCE, RT_FLAGS);
    NphRenderLightDesc a_desc = sun_desc(true);
    NphRenderLightDesc b_desc = point_desc(nph_dvec3(0.0, 3.0, 0.0), true);
    const NphRenderLight a = make_light(f, &a_desc);
    const NphRenderLight b = make_light(f, &b_desc);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rays);
    nph_render_light_destroy(f->renderer, a);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rays);
    set_shadows(f, b, &b_desc, false);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rays);
    // An update that keeps the shadows on counts the light once.
    set_shadows(f, b, &b_desc, true);
    set_shadows(f, b, &b_desc, true);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rays);
    nph_render_light_destroy(f->renderer, b);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rays);
}

// Without ray tracing (turned off here; absent on some devices) a light
// that casts shadows draws exactly as one that does not.
NTEST(shadows, a_renderer_without_ray_tracing_draws_them_unshadowed)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.disable_ray_tracing = 1;
    RenderFixture fx;
    RenderFixture *f = &fx;
    test_render_setup(f, &d);
    if (f->dev == NULL) {
        test_render_teardown(f);
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_EXPECT_FALSE(nph_renderer_ray_tracing_supported(f->renderer));
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        NphRenderView v;
        shadow_view(&v, top_camera(), 13.0F, k_pipelines[k], RT_FLAGS);
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        const NphRenderFrameStats stats = render_into(f, &v, on);
        NTEST_EXPECT_FALSE(stats.shadow_rays);
        NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, off), SAME_TOL);
    }
    test_render_teardown(f);
}
