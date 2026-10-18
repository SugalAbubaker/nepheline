// SPDX-License-Identifier: Apache-2.0
// The production path's virtual shadow map (render/renderer.h;
// docs/render.md "Shadows"; engine/render/src/vsm.c): the sun's shadow is
// dark where the geometry says and nowhere else, within a margin of its
// texels at the edges; pages are drawn once and kept, a moving caster
// redraws its own pages alone and a camera move reuses the pages it had,
// the sun turning redraws everything, and a full pool makes the levels
// coarser until they fit (and finer again once there is room); blended
// surfaces ask for their own pages, a narrow view from afar takes a level
// that holds it; it agrees with the ray-traced shadow it replaces, and
// holds far from the origin. The fixture's teardown asserts zero validation and
// synchronisation-validation errors and nothing leaked.
#include "test_shadow_support.h"

typedef RenderFixture Vsm;

NTEST_SETUP(Vsm)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Vsm)
{
    test_render_teardown(f);
}

#define REQUIRE_VSM(f)                                                                             \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_virtual_shadows_supported((f)->renderer)) {                              \
            NTEST_SKIP("no virtual shadow map on this device");                                    \
        }                                                                                          \
    } while (0)

// A shadow map's edges move by up to its texel (a pixel and a half here),
// soften over another (the 2 x 2 filter), and shift with the receiver's
// offset along its normal: four pixels of margin.
#define VSM_GROW 4.0F

// Views of the production path with its virtual shadow map.
static void vsm_view(NphRenderView *v, NphRenderCamera c)
{
    shadow_view(v, c, 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, 0U);
}

// The map's statistics reach a frame's NphRenderFrameStats
// NPH_GPU_FRAMES_IN_FLIGHT frames late: renders the frame of interest, then
// as many more of the same view, and returns the statistics that report
// it (the image is the last frame's).
static NphRenderFrameStats frame_stats(RenderFixture *f, const NphRenderView *v)
{
    NphRenderFrameStats s = test_render_read(f, v);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        s = test_render_read(f, v);
    }
    return s;
}

static void log_stats(const char *what, const NphRenderFrameStats *s)
{
    NTEST_LOG("%s: pages requested %u, drawn %u, resident %u, evicted %u; draws %u; overflow %u",
              what, s->vsm_requested, s->vsm_drawn, s->vsm_resident, s->vsm_evicted, s->vsm_draws,
              s->vsm_overflow);
}

// The sun behind a cube, through the map: the floor the cube hides is dark,
// everything else as without shadows, away from the edges.
NTEST_F(Vsm, the_sun_shadows_what_the_geometry_says)
{
    REQUIRE_VSM(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    NphRenderView v;
    vsm_view(&v, cam);
    NphRenderFrameStats stats = render_into(f, &v, off);
    NTEST_EXPECT_FALSE(stats.vsm);
    set_shadows(f, sun, &ld, true);
    stats = render_into(f, &v, on);
    NTEST_EXPECT_TRUE(stats.vsm);
    NTEST_EXPECT_FALSE(stats.shadow_rays); // the sun is the map's alone
    const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder, VSM_GROW);
    log_check("sun behind a cube", "virtual shadow map", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 8U);
    NTEST_EXPECT_GT_UINT(c.lit, 2000U);
}

// The same on a device whose mesh shaders are switched off: the pages are
// drawn with indexed indirect draws, as on lavapipe-class devices.
NTEST(vsm, the_indirect_raster_shadows_what_the_geometry_says)
{
    static RenderFixture fx;
    RenderFixture *f = &fx;
    memset(f, 0, sizeof *f);
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.headless = 1;
    dd.disable_features = (u32)NPH_GPU_OPTIONAL_MESH_SHADER;
    f->dev = test_gpu_create(&dd);
    if (f->dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphRendererDesc rd;
    test_renderer_desc(&rd, f->dev);
    NTEST_ASSERT_EQ_INT(nph_renderer_create(&rd, &f->renderer), NPH_RESULT_OK);
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(f->dev, &gd, &f->graph), NPH_RESULT_OK);
    f->target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, TEST_TARGET_SIZE);
    f->pixels = NPH_ALLOC_ARRAY_ZEROED(nph_allocator_system(), u8, PIXELS);
    if (!nph_renderer_virtual_shadows_supported(f->renderer)) {
        test_render_teardown(f);
        NTEST_SKIP("no virtual shadow map on this device");
    }
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    NphRenderView v;
    vsm_view(&v, cam);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    render_into(f, &v, off);
    set_shadows(f, sun, &ld, true);
    const NphRenderFrameStats st = render_into(f, &v, on);
    NTEST_EXPECT_TRUE(st.vsm);
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder, VSM_GROW);
    log_check("sun behind a cube", "virtual shadow map, indirect raster", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 8U);
    NTEST_EXPECT_GT_UINT(c.lit, 2000U);
    test_render_teardown(f);
}

// A blended floor, with nothing opaque under it, shadowed by a cube out of
// the camera's view: the visibility buffer holds nothing at all, so the
// floor's pages come from the blended surfaces' own requests, and the
// shadow falls where the geometry says.
NTEST_F(Vsm, a_blended_surface_is_shadowed)
{
    REQUIRE_VSM(f);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.5F, 0.5F, 0.5F, 0.75F);
    md.metallic = 0.0F;
    md.roughness = 0.9F;
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    NphRenderMaterial glass = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &glass), NPH_RESULT_OK);
    (void)instance(f, plane, glass, at(0.0, 0.0, 0.0, 6.0F));
    // Left of the view, 3 m up: its shadow falls across the middle.
    (void)instance(f, cube, grey, at(-3.0, 3.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    NphRenderView v;
    vsm_view(&v, cam);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    render_into(f, &v, off);
    set_shadows(f, sun, &ld, true);
    const NphRenderFrameStats st = render_into(f, &v, on);
    NTEST_EXPECT_TRUE(st.vsm);
    NTEST_EXPECT_GT_UINT(st.blended, 0U);
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(-3.0F, 3.0F, 0.0F), 1.0F);
    const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder, VSM_GROW);
    log_check("blended floor", "virtual shadow map", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_EQ_UINT(c.occluder, 0U); // out of view
    NTEST_EXPECT_GT_UINT(c.shadow, 8U);
    NTEST_EXPECT_GT_UINT(c.lit, 1000U);
}

// The sun is the live directional light that casts shadows in the lowest
// slot: another such light is ray traced, and the map passes to it when
// the sun goes or stops casting, and back to a new one ahead of it.
NTEST_F(Vsm, the_sun_is_the_first_directional_light_that_casts_shadows)
{
    REQUIRE_VSM(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderView v;
    vsm_view(&v, top_camera());
    NphRenderLightDesc first = sun_desc(true);
    NphRenderLightDesc second = sun_desc(true);
    second.direction = nph_vec3(-1.0F, -1.0F, 0.0F);
    const NphRenderLight a = make_light(f, &first);
    const NphRenderLight b = make_light(f, &second);
    NphRenderFrameStats st = test_render_read(f, &v);
    NTEST_EXPECT_TRUE(st.vsm);         // a
    NTEST_EXPECT_TRUE(st.shadow_rays); // b
    nph_render_light_destroy(f->renderer, a);
    st = test_render_read(f, &v);
    NTEST_EXPECT_TRUE(st.vsm); // b
    NTEST_EXPECT_FALSE(st.shadow_rays);
    // A new one takes the freed first slot, ahead of b.
    NphRenderLightDesc third = sun_desc(true);
    const NphRenderLight c = make_light(f, &third);
    st = test_render_read(f, &v);
    NTEST_EXPECT_TRUE(st.vsm);         // c
    NTEST_EXPECT_TRUE(st.shadow_rays); // b
    set_shadows(f, c, &third, false);
    st = test_render_read(f, &v);
    NTEST_EXPECT_TRUE(st.vsm); // b again
    NTEST_EXPECT_FALSE(st.shadow_rays);
    set_shadows(f, b, &second, false);
    st = test_render_read(f, &v);
    NTEST_EXPECT_FALSE(st.vsm);
    NTEST_EXPECT_FALSE(st.shadow_rays);
}

// The pages a view needs are drawn in its first frame and kept: the next
// frames draw none.
NTEST_F(Vsm, pages_are_drawn_once_then_kept)
{
    REQUIRE_VSM(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    (void)make_light(f, &ld);
    NphRenderView v;
    vsm_view(&v, top_camera());
    NphRenderFrameStats first = frame_stats(f, &v);
    log_stats("first frame", &first);
    NTEST_EXPECT_GT_UINT(first.vsm_requested, 0U);
    NTEST_EXPECT_EQ_UINT(first.vsm_drawn, first.vsm_requested);
    NTEST_EXPECT_GT_UINT(first.vsm_draws, 0U);
    const NphRenderFrameStats later = frame_stats(f, &v);
    log_stats("a later frame", &later);
    NTEST_EXPECT_EQ_UINT(later.vsm_requested, first.vsm_requested);
    NTEST_EXPECT_EQ_UINT(later.vsm_drawn, 0U);
    NTEST_EXPECT_EQ_UINT(later.vsm_draws, 0U);
    NTEST_EXPECT_EQ_UINT(later.vsm_resident, first.vsm_requested);
    NTEST_EXPECT_EQ_UINT(later.vsm_overflow, 0U);
}

// A caster that moves redraws the pages it touches, and its shadow follows
// it; one that moves far from every page in use redraws nothing.
NTEST_F(Vsm, a_moving_caster_redraws_its_own_pages)
{
    REQUIRE_VSM(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    const NphRenderInstance far = instance(f, s.cube, s.grey, at(300.0, 1.0, 300.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    const Light l = {true, ld.direction, nph_vec3_zero()};
    NphRenderView v;
    vsm_view(&v, cam);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    // The image without shadows of the moved scene, then the moved shadow.
    NphRenderTransform t = at(0.0, 1.0, 1.2, 1.0F);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &t),
                        NPH_RESULT_OK);
    render_into(f, &v, off);
    t = at(0.0, 1.0, 0.0, 1.0F);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &t),
                        NPH_RESULT_OK);
    set_shadows(f, sun, &ld, true);
    (void)frame_stats(f, &v);
    NphRenderFrameStats still = frame_stats(f, &v);
    NTEST_EXPECT_EQ_UINT(still.vsm_drawn, 0U);
    t = at(0.0, 1.0, 1.2, 1.0F);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &t),
                        NPH_RESULT_OK);
    const NphRenderFrameStats moved = frame_stats(f, &v);
    log_stats("the occluder moved", &moved);
    NTEST_EXPECT_GT_UINT(moved.vsm_drawn, 0U);
    memcpy(on, f->pixels, PIXELS);
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 1.2F), 1.0F);
    const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder, VSM_GROW);
    log_check("moved", "virtual shadow map", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 8U);
    // Far from every page the view needs: nothing to draw.
    t = at(310.0, 1.0, 300.0, 1.0F);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, far, &t), NPH_RESULT_OK);
    const NphRenderFrameStats elsewhere = frame_stats(f, &v);
    log_stats("a far caster moved", &elsewhere);
    NTEST_EXPECT_EQ_UINT(elsewhere.vsm_drawn, 0U);
}

// The camera moving draws the pages it newly needs, and moving back draws
// none: the first view's pages were kept. A floor 100 m across, the camera
// moving 20 m, more than a page of the levels it reads (16 and 32 m).
NTEST_F(Vsm, camera_motion_reuses_the_pages_it_had)
{
    REQUIRE_VSM(f);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, plane, grey, at(0.0, 0.0, 0.0, 100.0F));
    (void)instance(f, cube, grey, at(0.0, 1.0, 0.0, 1.0F));
    (void)instance(f, cube, grey, at(20.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    (void)make_light(f, &ld);
    NphRenderView a;
    vsm_view(&a, top_camera());
    NphRenderView b;
    vsm_view(&b, nph_render_camera_look_at(nph_dvec3(20.0, 6.0, 0.0), nph_dvec3(20.0, 0.0, 0.0),
                                           nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.05F));
    const NphRenderFrameStats first = frame_stats(f, &a);
    log_stats("first view", &first);
    NTEST_EXPECT_EQ_UINT(first.vsm_drawn, first.vsm_requested);
    const NphRenderFrameStats there = frame_stats(f, &b);
    log_stats("moved 20 m", &there);
    NTEST_EXPECT_GT_UINT(there.vsm_drawn, 0U);
    NTEST_EXPECT_GT_UINT(there.vsm_resident, there.vsm_requested); // the first view's kept too
    const NphRenderFrameStats back = frame_stats(f, &a);
    log_stats("moved back", &back);
    NTEST_EXPECT_EQ_UINT(back.vsm_drawn, 0U);
    NTEST_EXPECT_EQ_UINT(back.vsm_evicted, 0U);
}

// The sun turning invalidates every page: the next frame draws all it
// needs, and the shadow falls the new way.
NTEST_F(Vsm, the_sun_turning_redraws_every_page)
{
    REQUIRE_VSM(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    NphRenderView v;
    vsm_view(&v, cam);
    (void)frame_stats(f, &v);
    // Along -X now: the shadow falls the other way.
    ld.direction = nph_vec3(-1.0F, -1.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, sun, &ld), NPH_RESULT_OK);
    const NphRenderFrameStats turned = frame_stats(f, &v);
    log_stats("the sun turned", &turned);
    NTEST_EXPECT_EQ_UINT(turned.vsm_drawn, turned.vsm_requested);
    static u8 on[PIXELS];
    static u8 off[PIXELS];
    memcpy(on, f->pixels, PIXELS);
    set_shadows(f, sun, &ld, false);
    render_into(f, &v, off);
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder, VSM_GROW);
    log_check("turned", "virtual shadow map", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 8U);
}

// A fixture with a pool of `pages`; false (after skipping) where the
// device cannot have the map.
static bool small_pool_setup(RenderFixture *f, u32 pages)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.vsm_pages = pages;
    test_render_setup(f, &d);
    return f->dev != NULL && nph_renderer_virtual_shadows_supported(f->renderer);
}

#define SMALL_POOL_SKIP(f)                                                                         \
    do {                                                                                           \
        const bool no_device_ = (f)->dev == NULL;                                                  \
        test_render_teardown(f);                                                                   \
        NTEST_SKIP(no_device_ ? "no Vulkan device available"                                       \
                              : "no virtual shadow map on this device");                           \
    } while (0)

// A frame's statistics into a 512 x 512 target (the fixture's 64 x 64 view
// spans half a page of any level: too little for the resolution bias to
// change its page count), NPH_GPU_FRAMES_IN_FLIGHT frames late as
// frame_stats.
#define BIG_TARGET 512U

static NphRenderFrameStats big_frame_stats(RenderFixture *f, const NphRenderView *v,
                                           NphGpuImage target)
{
    NphRenderFrameStats s = test_render_frame(f, v, target);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        s = test_render_frame(f, v, target);
    }
    return s;
}

// A floor 60 m across with the cube, the sun casting shadows, and a view
// straight down at it from 12 m filling a 512 x 512 target: its pixels
// span four pages each way of the level that matches them.
static void big_scene(RenderFixture *f, NphRenderView *down, NphRenderView *sky)
{
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, plane, grey, at(0.0, 0.0, 0.0, 60.0F));
    (void)instance(f, cube, grey, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    (void)make_light(f, &ld);
    vsm_view(down, nph_render_camera_look_at(nph_dvec3(0.0, 12.0, 0.0), nph_dvec3_zero(),
                                             nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.05F));
    // Straight up from the floor: nothing but the background.
    vsm_view(sky, nph_render_camera_look_at(nph_dvec3(20.0, 0.5, 20.0), nph_dvec3(20.0, 10.0, 20.0),
                                            nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.05F));
}

// A pool too small for a view: the overflow is flagged (and logged once)
// and the frame still draws; then the renderer makes the levels coarser
// until the view's pages fit. Validation stays quiet throughout.
NTEST(vsm, a_full_pool_coarsens_the_levels_until_they_fit)
{
    static RenderFixture fx;
    RenderFixture *f = &fx;
    if (!small_pool_setup(f, 8U)) {
        SMALL_POOL_SKIP(f);
    }
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, BIG_TARGET);
    NphRenderView down;
    NphRenderView sky;
    big_scene(f, &down, &sky);
    const NphRenderFrameStats first = big_frame_stats(f, &down, target);
    log_stats("eight pages, at first", &first);
    NTEST_EXPECT_GT_UINT(first.vsm_requested, 8U);
    NTEST_EXPECT_EQ_UINT(first.vsm_drawn, 8U);
    NTEST_EXPECT_NE_UINT(first.vsm_overflow & (u32)NPH_RENDER_VSM_OVERFLOW_POOL, 0U);
    NTEST_EXPECT_EQ_UINT(first.vsm_lod_bias, 0U);
    for (u32 i = 0; i < 8U; ++i) {
        (void)test_render_frame(f, &down, target);
    }
    const NphRenderFrameStats later = big_frame_stats(f, &down, target);
    log_stats("eight pages, later", &later);
    NTEST_LOG("resolution bias %u", later.vsm_lod_bias);
    NTEST_EXPECT_GT_UINT(later.vsm_lod_bias, 0U);
    NTEST_EXPECT_EQ_UINT(later.vsm_overflow, 0U);
    NTEST_EXPECT_LE_UINT(later.vsm_requested, 8U);
    NTEST_EXPECT_EQ_UINT(later.vsm_drawn, 0U); // the pages it fits in stay drawn
    nph_gpu_image_destroy(f->dev, target);
    test_render_teardown(f);
}

// After an overflow raised the resolution bias, views that would fit a
// step finer with room to spare (here, nothing but the sky) lower it
// again, though only after a run of them: the bias does not swing back
// and forth.
NTEST(vsm, the_resolution_bias_falls_once_there_is_room)
{
    static RenderFixture fx;
    RenderFixture *f = &fx;
    if (!small_pool_setup(f, 8U)) {
        SMALL_POOL_SKIP(f);
    }
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, BIG_TARGET);
    NphRenderView down;
    NphRenderView sky;
    big_scene(f, &down, &sky);
    for (u32 i = 0; i < 8U; ++i) {
        (void)test_render_frame(f, &down, target);
    }
    NphRenderFrameStats st = big_frame_stats(f, &down, target);
    const u32 raised = st.vsm_lod_bias;
    NTEST_LOG("raised to %u", raised);
    NTEST_ASSERT_TRUE(raised > 0U);
    for (u32 i = 0; i < 16U; ++i) {
        st = test_render_frame(f, &sky, target);
    }
    NTEST_EXPECT_EQ_UINT(st.vsm_requested, 0U);
    NTEST_EXPECT_EQ_UINT(st.vsm_lod_bias, raised); // not yet
    for (u32 i = 0; i < 160U; ++i) {
        st = test_render_frame(f, &sky, target);
    }
    NTEST_LOG("after 176 frames of sky: %u", st.vsm_lod_bias);
    NTEST_EXPECT_EQ_UINT(st.vsm_lod_bias, raised - 1U);
    nph_gpu_image_destroy(f->dev, target);
    test_render_teardown(f);
}

// The map's shadow against the rays it replaces, in one production view
// each: they differ only along the shadow's edges.
NTEST_F(Vsm, ray_traced_and_virtual_shadows_agree)
{
    REQUIRE_VSM(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    (void)make_light(f, &ld);
    static u8 rays[PIXELS];
    static u8 map[PIXELS];
    NphRenderView v;
    shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_PRODUCTION,
                (u32)NPH_RENDER_VIS_RT_SHADOWS);
    NphRenderFrameStats st = render_into(f, &v, rays);
    NTEST_EXPECT_TRUE(st.shadow_rays);
    NTEST_EXPECT_FALSE(st.vsm);
    vsm_view(&v, top_camera());
    st = render_into(f, &v, map);
    NTEST_EXPECT_TRUE(st.vsm);
    u32 same = 0U;
    for (u64 i = 0; i < PIXELS; i += 4U) {
        same += channel_diff(rays + i, map + i) <= SAME_TOL ? 1U : 0U;
    }
    const f32 share = (f32)same / (f32)(TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    NTEST_LOG("%.2f %% of pixels within %u levels", (f64)share * 100.0, SAME_TOL);
    NTEST_EXPECT_TRUE(share >= 0.98F);
}

// A camera 20 km up with a narrow field of view, as through a telephoto
// lens or a scope: the floor lies 14 km across the light from it, beyond
// the window of the level whose texels match its pixels (12, 16 km across),
// so the requests and the shading both take the next level whose window
// holds it (13: texels of 2 m, two pixels each), and the shadow is there.
NTEST_F(Vsm, a_narrow_view_from_afar_takes_a_level_that_holds_the_scene)
{
    REQUIRE_VSM(f);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, plane, grey, at(0.0, 0.0, 0.0, 60.0F));
    (void)instance(f, cube, grey, at(0.0, 10.0, 0.0, 20.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    // The 60 m floor fills the view.
    const f64 height = 20000.0;
    const NphRenderCamera cam = nph_render_camera_look_at(
        nph_dvec3(0.0, height, 0.0), nph_dvec3_zero(), nph_vec3(0.0F, 0.0F, -1.0F),
        2.0F * (f32)nph_f64_atan2(30.0, height), 1.0F);
    NphRenderView v;
    vsm_view(&v, cam);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    render_into(f, &v, off);
    set_shadows(f, sun, &ld, true);
    const NphRenderFrameStats st = render_into(f, &v, on);
    NTEST_EXPECT_TRUE(st.vsm);
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 10.0F, 0.0F), 20.0F);
    // Three texels of margin: the edges move by up to a texel, blur over
    // another, and shift with the receiver's offset.
    const ShadowCheck c = check_shadow_floor(on, off, &cam, &l, &occluder, 6.5F, 29.0F);
    log_check("narrow view, 20 km up", "virtual shadow map", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 8U);
    NTEST_EXPECT_GT_UINT(c.lit, 500U);
}

// The same scene and view 10,000 km from the origin give the same image:
// light space is anchored near the camera.
NTEST_F(Vsm, far_from_the_origin_the_shadow_holds)
{
    REQUIRE_VSM(f);
    const f64 far = 1.0e7;
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    (void)make_light(f, &ld);
    static u8 near_img[PIXELS];
    static u8 far_img[PIXELS];
    NphRenderView v;
    const NphRenderInstance floor0 = instance(f, plane, grey, at(0.0, 0.0, 0.0, 6.0F));
    const NphRenderInstance cube0 = instance(f, cube, grey, at(0.0, 1.0, 0.0, 1.0F));
    vsm_view(&v, top_camera());
    render_into(f, &v, near_img);
    nph_render_instance_destroy(f->renderer, floor0);
    nph_render_instance_destroy(f->renderer, cube0);
    (void)instance(f, plane, grey, at(far, 0.0, far, 6.0F));
    (void)instance(f, cube, grey, at(far, 1.0, far, 1.0F));
    vsm_view(&v, nph_render_camera_look_at(nph_dvec3(far, 6.0, far), nph_dvec3(far, 0.0, far),
                                           nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.05F));
    const NphRenderFrameStats st = render_into(f, &v, far_img);
    NTEST_EXPECT_TRUE(st.vsm);
    NTEST_EXPECT_LE_UINT(image_max_diff(near_img, far_img), SAME_TOL);
}
