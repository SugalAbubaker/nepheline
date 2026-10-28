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
// holds far from the origin; a caster with levels of detail, drawn at the
// map's cut, shadows as its level 0 does, and so does one drawn as voxels
// (in the map itself, texel by texel); the depth pyramid's requests for
// blocks of pixels are every pixel's own. The fixture's teardown asserts
// zero validation and synchronisation-validation errors and nothing leaked.
#include <nepheline/math/random.h>

#include "render_internal.h"
#include "test_lod_support.h"
#include "test_shadow_support.h"

#include <nph_vsm.h>

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
    NTEST_LOG("%s: pages requested %u, drawn %u, resident %u, evicted %u; caster meshlets %u (%u "
              "nodes), draws %u; overflow %u",
              what, s->vsm_requested, s->vsm_drawn, s->vsm_resident, s->vsm_evicted,
              s->vsm_meshlets, s->vsm_nodes, s->vsm_draws, s->vsm_overflow);
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

// A fixture with a pool of `pages` and room for `draws` caster draws a
// frame; false (after skipping) where the device cannot have the map.
static bool small_lists_setup(RenderFixture *f, u32 pages, u32 draws)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.vsm_pages = pages;
    d.max_vsm_draws = draws;
    test_render_setup(f, &d);
    return f->dev != NULL && nph_renderer_virtual_shadows_supported(f->renderer);
}

#define BIG_PIXELS ((u64)BIG_TARGET * BIG_TARGET * 4U)

// Pixels within this many of a shadow's edge are not compared: the map's
// texel and its filter's (VSM_GROW).
#define BIG_EDGE 4

static bool big_dark(const u8 *img, i32 x, i32 y)
{
    return channel_max(img + ((u64)y * BIG_TARGET + (u64)x) * 4U) <= DARK_MAX;
}

// From the eye's height near the floor's edge across it: its pixels lie
// from half a metre to 54 m away, in many levels' pages.
static void across_view(NphRenderView *v)
{
    vsm_view(v, nph_render_camera_look_at(nph_dvec3(0.0, 1.7, 24.0), nph_dvec3(0.0, 0.0, -20.0),
                                          nph_vec3(0.0F, 1.0F, 0.0F), 0.9F, 0.05F));
}

// The counters of the frame just rendered (white box: its statistics copy).
static NphVsmCounters frame_counters(RenderFixture *f)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NphVsmCounters c;
    memcpy(&c, nph_gpu_buffer_map(f->dev, f->renderer->vsm.stats[f->renderer->frame_slot]),
           sizeof c);
    return c;
}

// Room for eight caster draws a frame, about a page's each, where the view
// wants several times that many pages: the first frame, before any frame
// has measured what a page costs, overflows the list (its pages are not
// read, and are drawn again); from then on each frame draws what the list
// holds at what a page of each level costs, the coarse levels first (every
// wanted page of the levels above the one the list runs out in, none below
// it), nothing overflows, the resolution stays, and once the pages are
// drawn the image is the one a list with room for all of them gives.
NTEST(vsm, a_burst_of_pages_is_drawn_over_frames_within_the_lists)
{
    static RenderFixture fx;
    RenderFixture *f = &fx;
    static u8 roomy[BIG_PIXELS];
    static u8 tight[BIG_PIXELS];
    if (!small_lists_setup(f, 512U, 65536U)) {
        SMALL_POOL_SKIP(f);
    }
    NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, BIG_TARGET);
    NphRenderView down;
    NphRenderView sky;
    big_scene(f, &down, &sky);
    NphRenderView across;
    across_view(&across);
    const NphRenderFrameStats all = big_frame_stats(f, &across, target);
    log_stats("room for every draw", &all);
    NTEST_EXPECT_EQ_UINT(all.vsm_overflow, 0U);
    NTEST_EXPECT_EQ_UINT(all.vsm_drawn, all.vsm_requested);
    const u32 pages = all.vsm_drawn;
    for (u32 i = 0; i < 2U; ++i) {
        (void)test_render_frame(f, &across, target);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, roomy, BIG_PIXELS);
    nph_gpu_image_destroy(f->dev, target);
    test_render_teardown(f);

    if (!small_lists_setup(f, 512U, 8U)) {
        SMALL_POOL_SKIP(f);
    }
    target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, BIG_TARGET);
    big_scene(f, &down, &sky);
    NTEST_LOG("the view wants %u pages, about a draw each", pages);
    NTEST_ASSERT_TRUE(pages >= 24U);
    u32 frames = 0U;
    u32 drawn = 0U;
    u32 bounded = 0U; // frames whose wanted pages outran the budget
    bool coarse_first = true;
    for (; frames < 64U; ++frames) {
        (void)test_render_frame(f, &across, target);
        const NphVsmCounters c = frame_counters(f);
        if (frames == 0U) {
            NTEST_EXPECT_NE_UINT(c.overflow & (u32)NPH_VSM_OVERFLOW_DRAWS, 0U);
            continue;
        }
        NTEST_EXPECT_EQ_UINT(c.overflow, 0U);
        drawn += c.render_count;
        if (c.wanted_count == 0U) {
            break;
        }
        if (c.wanted_count > c.budget) {
            ++bounded;
            // The level the budget ran out in: those above it drawn whole,
            // those below it not at all.
            i32 partial = -1;
            for (i32 level = NPH_VSM_LEVELS - 1; level >= 0 && partial < 0; --level) {
                if (c.level_render[level] < c.level_wanted[level]) {
                    partial = level;
                }
            }
            for (i32 level = 0; level < (i32)NPH_VSM_LEVELS; ++level) {
                coarse_first = coarse_first &&
                               (level < partial ? c.level_render[level] == 0U
                                                : level == partial || c.level_render[level] ==
                                                                          c.level_wanted[level]);
            }
        }
    }
    NTEST_LOG("drawn over %u frames after the first (%u past the budget): %u pages", frames,
              bounded, drawn);
    NTEST_EXPECT_LT_UINT(frames, 64U);
    NTEST_EXPECT_GT_UINT(bounded, 2U);
    NTEST_EXPECT_TRUE(coarse_first);
    NTEST_EXPECT_EQ_UINT(drawn, pages);
    const NphRenderFrameStats st = big_frame_stats(f, &across, target);
    log_stats("room for eight draws, at last", &st);
    NTEST_EXPECT_EQ_UINT(st.vsm_lod_bias, 0U);
    NTEST_EXPECT_EQ_UINT(st.vsm_resident, pages);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, tight, BIG_PIXELS);
    // Away from the shadows' edges, the same image. The pages sit elsewhere
    // in the pool (the order they take pages in follows atomics), where a
    // GPU's raster may round a texel's depth the other way: at an edge, a
    // pixel may then differ.
    u32 differ = 0U;
    u32 edge_differ = 0U;
    for (i32 y = 0; y < (i32)BIG_TARGET; ++y) {
        for (i32 x = 0; x < (i32)BIG_TARGET; ++x) {
            const bool d = big_dark(roomy, x, y);
            bool edge = false;
            for (i32 dy = -BIG_EDGE; dy <= BIG_EDGE && !edge; ++dy) {
                for (i32 dx = -BIG_EDGE; dx <= BIG_EDGE && !edge; ++dx) {
                    const i32 qx = x + dx;
                    const i32 qy = y + dy;
                    edge = qx >= 0 && qy >= 0 && qx < (i32)BIG_TARGET && qy < (i32)BIG_TARGET &&
                           big_dark(roomy, qx, qy) != d;
                }
            }
            const u64 at_px = ((u64)y * BIG_TARGET + (u64)x) * 4U;
            const bool differs = channel_diff(roomy + at_px, tight + at_px) > SAME_TOL;
            differ += !edge && differs ? 1U : 0U;
            edge_differ += edge && differs ? 1U : 0U;
        }
    }
    NTEST_LOG("%u pixels differ from the image with room for every draw away from the edges, %u "
              "along them",
              differ, edge_differ);
    NTEST_EXPECT_EQ_UINT(differ, 0U);
    nph_gpu_image_destroy(f->dev, target);
    test_render_teardown(f);
}

// Room for 32 caster draws a frame, the sun turning every frame (which drops
// every page, so each frame wants every page the view needs): at full
// resolution the pages outrun the budget frame after frame, as no burst
// does, so the renderer raises the resolution bias until a frame's pages
// fit, and does not lower it again while the sun turns, a step finer
// wanting more pages than the budget holds.
NTEST(vsm, a_sun_turning_past_the_lists_coarsens_the_levels_once)
{
    static RenderFixture fx;
    RenderFixture *f = &fx;
    if (!small_lists_setup(f, 512U, 32U)) {
        SMALL_POOL_SKIP(f);
    }
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, BIG_TARGET);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, plane, grey, at(0.0, 0.0, 0.0, 60.0F));
    (void)instance(f, cube, grey, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    const NphRenderLight sun = make_light(f, &ld);
    NphRenderView v;
    across_view(&v);
    NphRenderFrameStats st;
    memset(&st, 0, sizeof st);
    u32 bias = 0U;
    u32 changed_at = 0U; // the last frame whose bias differed from the one before
    bool lowered = false;
    for (u32 i = 0; i < 200U; ++i) {
        const f32 a = 0.001F * (f32)i;
        ld.direction = nph_vec3(nph_f32_cos(a), -1.0F, nph_f32_sin(a));
        NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, sun, &ld), NPH_RESULT_OK);
        st = test_render_frame(f, &v, target);
        if (st.vsm_lod_bias != bias) {
            lowered = lowered || st.vsm_lod_bias < bias;
            bias = st.vsm_lod_bias;
            changed_at = i;
        }
    }
    log_stats("the sun turning, at last", &st);
    NTEST_LOG("resolution bias %u from frame %u; pages wanted %u, budget %u", bias, changed_at,
              st.vsm_wanted, st.vsm_budget);
    NTEST_EXPECT_GT_UINT(bias, 0U);
    NTEST_EXPECT_FALSE(lowered);
    NTEST_EXPECT_LT_UINT(changed_at, 60U);
    NTEST_EXPECT_EQ_UINT(st.vsm_overflow, 0U);
    NTEST_EXPECT_LE_UINT(st.vsm_wanted, st.vsm_budget);
    NTEST_EXPECT_EQ_UINT(st.vsm_drawn, st.vsm_wanted); // every page, every frame
    NTEST_EXPECT_GT_UINT(st.vsm_drawn, 0U);
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

// ---------------------------------------------------------------------------
// Casters with levels of detail
// ---------------------------------------------------------------------------

// The torus's view: 256 x 256 (the fixture's 64 x 64 pixels span 9 cm of
// the floor, too few across the ring to tell its inside from its edges).
#define LOD_TARGET 256U
#define LOD_PIXELS ((u64)LOD_TARGET * LOD_TARGET * 4U)
// Pixels within this many of the level-0 shadow's edge are not compared:
// the cut moves an edge by up to half a texel along the light (seven tenths
// of one on the floor, a texel being one or two pixels), and the map's
// filter softens it over another, as VSM_GROW allows the analytic checks.
#define LOD_EDGE 4

// Renders `frames` frames of `v` into `target` and reads the last back into
// `out` (LOD_PIXELS); returns the last frame's statistics, which report the
// frame NPH_GPU_FRAMES_IN_FLIGHT before it.
static NphRenderFrameStats lod_frames(RenderFixture *f, const NphRenderView *v, NphGpuImage target,
                                      u32 frames, u8 *out)
{
    NphRenderFrameStats s;
    memset(&s, 0, sizeof s);
    for (u32 i = 0; i < frames; ++i) {
        s = test_render_frame(f, v, target);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, out, LOD_PIXELS);
    return s;
}

static bool lod_dark(const u8 *img, i32 x, i32 y)
{
    return channel_max(img + ((u64)y * LOD_TARGET + (u64)x) * 4U) <= DARK_MAX;
}

// A torus with levels of detail (9,216 triangles at level 0) above the
// camera's view and to its side: only its shadow is seen, a ring on the
// floor. The map draws the torus at the cut whose error is within half a
// texel of each level (vsm_cull.comp): the shadow is its level-0 shadow but
// along the edges, inside as well as out (a crack in the cut would let light
// through), from fewer draws. Turning the levels on redraws every page, as
// turning them off would.
NTEST_F(Vsm, a_caster_with_levels_shadows_as_its_level_0)
{
    REQUIRE_VSM(f);
    LodMesh torus;
    lod_torus(&torus, 96U, 48U, 0.8F, 0.25F);
    NphRenderMesh caster = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &torus.desc, &caster), NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(torus.lods.levels, 3U);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, plane, grey, at(0.0, 0.0, 0.0, 6.0F));
    // 4.5 m up, 3.5 m to the left: outside the view from 6 m, its shadow
    // centred at x = 1 (the sun travels along +X at 45 degrees).
    const NphRenderInstance inst = instance(f, caster, grey, at(-3.5, 4.5, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(true);
    (void)make_light(f, &ld);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, LOD_TARGET);
    static u8 level0[LOD_PIXELS];
    static u8 cut[LOD_PIXELS];
    NphRenderView v;
    vsm_view(&v, top_camera());
    v.lod_error = 0.0F; // every caster at level 0
    // The first frame's statistics (every page drawn), then a settled image.
    const NphRenderFrameStats s0 = lod_frames(f, &v, target, 1U + NPH_GPU_FRAMES_IN_FLIGHT, level0);
    log_stats("level 0", &s0);
    NTEST_EXPECT_TRUE(s0.vsm);
    NTEST_EXPECT_EQ_UINT(s0.vsm_overflow, 0U);
    (void)lod_frames(f, &v, target, 2U, level0);
    v.lod_error = 1.0F;
    const NphRenderFrameStats s1 = lod_frames(f, &v, target, 1U + NPH_GPU_FRAMES_IN_FLIGHT, cut);
    log_stats("levels", &s1);
    NTEST_EXPECT_EQ_UINT(s1.vsm_overflow, 0U);
    NTEST_EXPECT_GT_UINT(s1.vsm_drawn, 0U);
    NTEST_EXPECT_EQ_UINT(s1.vsm_drawn, s0.vsm_drawn); // every page again
    NTEST_EXPECT_LT_UINT(s1.vsm_draws * 4U, s0.vsm_draws * 3U);
    (void)lod_frames(f, &v, target, 2U, cut);
    u32 compared = 0U;
    u32 deep = 0U; // compared pixels in the shadow
    u32 wrong = 0U;
    u32 edge_diffs = 0U;
    for (i32 y = 0; y < (i32)LOD_TARGET; ++y) {
        for (i32 x = 0; x < (i32)LOD_TARGET; ++x) {
            const bool dark = lod_dark(level0, x, y);
            bool edge = false;
            for (i32 dy = -LOD_EDGE; dy <= LOD_EDGE && !edge; ++dy) {
                for (i32 dx = -LOD_EDGE; dx <= LOD_EDGE && !edge; ++dx) {
                    const i32 qx = x + dx;
                    const i32 qy = y + dy;
                    edge = qx >= 0 && qy >= 0 && qx < (i32)LOD_TARGET && qy < (i32)LOD_TARGET &&
                           lod_dark(level0, qx, qy) != dark;
                }
            }
            const u64 at_px = ((u64)y * LOD_TARGET + (u64)x) * 4U;
            const bool differs = channel_diff(level0 + at_px, cut + at_px) > SAME_TOL;
            if (edge) {
                edge_diffs += differs ? 1U : 0U;
                continue;
            }
            ++compared;
            deep += dark ? 1U : 0U;
            wrong += differs ? 1U : 0U;
        }
    }
    NTEST_LOG("%u pixels compared (%u in the shadow), %u differ; %u differ along the edges",
              compared, deep, wrong, edge_diffs);
    NTEST_EXPECT_EQ_UINT(wrong, 0U);
    NTEST_EXPECT_GT_UINT(deep, 2000U);
    NTEST_EXPECT_GT_UINT(compared - deep, 40000U);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_mesh_destroy(f->renderer, caster);
    nph_gpu_image_destroy(f->dev, target);
    lod_mesh_free(&torus);
}

// A torus sixteen times as large (a ring 26 m across), above the
// view and to its side, of which the floor the camera sees takes a few
// pages: the walk down its hierarchy hands on the clusters near those
// pages, not the whole cut of each level it reaches (which at the finest
// levels is level 0), so the casters' meshlets tested are fewer than one
// such cut, and its shadow is still its level 0's.
NTEST_F(Vsm, a_big_caster_hands_on_only_its_clusters_near_the_pages_drawn)
{
    REQUIRE_VSM(f);
    // Finer than the torus above (65,536 triangles), so that its hierarchy's
    // groups are pieces of the ring the pages can tell apart.
    LodMesh torus;
    lod_torus(&torus, 256U, 128U, 0.8F, 0.25F);
    NphRenderMesh caster = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &torus.desc, &caster), NPH_RESULT_OK);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    const NphRenderInstance floor0 = instance(f, plane, grey, at(0.0, 0.0, 0.0, 40.0F));
    // Its ring's shadow (the sun along +X at 45 degrees) crosses the view.
    const NphRenderInstance inst = instance(f, caster, grey, at(-14.0, 12.0, 6.0, 16.0F));
    NphRenderLightDesc ld = sun_desc(true);
    const NphRenderLight sun = make_light(f, &ld);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, LOD_TARGET);
    static u8 level0[LOD_PIXELS];
    static u8 cut[LOD_PIXELS];
    NphRenderView v;
    vsm_view(&v, top_camera());
    v.lod_error = 0.0F; // every caster at level 0
    const NphRenderFrameStats s0 = lod_frames(f, &v, target, 1U + NPH_GPU_FRAMES_IN_FLIGHT, level0);
    log_stats("level 0", &s0);
    (void)lod_frames(f, &v, target, 2U, level0);
    v.lod_error = 1.0F;
    const NphRenderFrameStats s1 = lod_frames(f, &v, target, 1U + NPH_GPU_FRAMES_IN_FLIGHT, cut);
    log_stats("levels", &s1);
    (void)lod_frames(f, &v, target, 2U, cut);
    const u32 level0_clusters = torus.lods.level0_clusters;
    NTEST_LOG("level 0: %u clusters; the walk handed on %u meshlets from %u nodes", level0_clusters,
              s1.vsm_meshlets, s1.vsm_nodes);
    NTEST_EXPECT_EQ_UINT(s1.vsm_overflow, 0U);
    NTEST_EXPECT_GT_UINT(s1.vsm_drawn, 0U);
    NTEST_EXPECT_GT_UINT(s1.vsm_nodes, 0U);
    NTEST_EXPECT_GT_UINT(s1.vsm_meshlets, 0U);
    // Fewer than two thirds of one level's whole cut (level 0's, which the
    // finest levels take), where every level reached handed on all of it.
    NTEST_EXPECT_LE_UINT(s1.vsm_meshlets * 3U, level0_clusters * 2U);
    // Its shadow, which the floor shows in part, as its level 0's.
    u32 dark = 0U;
    u32 differ = 0U;
    for (i32 y = 0; y < (i32)LOD_TARGET; ++y) {
        for (i32 x = 0; x < (i32)LOD_TARGET; ++x) {
            const u64 at_px = ((u64)y * LOD_TARGET + (u64)x) * 4U;
            dark += lod_dark(level0, x, y) ? 1U : 0U;
            bool edge = false;
            for (i32 dy = -LOD_EDGE; dy <= LOD_EDGE && !edge; ++dy) {
                for (i32 dx = -LOD_EDGE; dx <= LOD_EDGE && !edge; ++dx) {
                    const i32 qx = x + dx;
                    const i32 qy = y + dy;
                    edge = qx >= 0 && qy >= 0 && qx < (i32)LOD_TARGET && qy < (i32)LOD_TARGET &&
                           lod_dark(level0, qx, qy) != lod_dark(level0, x, y);
                }
            }
            differ += !edge && channel_diff(level0 + at_px, cut + at_px) > SAME_TOL ? 1U : 0U;
        }
    }
    NTEST_LOG("%u pixels in its shadow, %u differ from level 0's away from its edges", dark,
              differ);
    NTEST_EXPECT_GT_UINT(dark, 2000U);
    NTEST_EXPECT_EQ_UINT(differ, 0U);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_instance_destroy(f->renderer, floor0);
    nph_render_mesh_destroy(f->renderer, caster);
    nph_gpu_image_destroy(f->dev, target);
    lod_mesh_free(&torus);
}

// ---------------------------------------------------------------------------
// Voxel casters
// ---------------------------------------------------------------------------

// The pool's extent in texels, and its depths read back through its
// sampled binding (vsm_pool_test.comp) into `out`.
#define POOL_W 8192U
NPH_STATIC_ASSERT(POOL_W == NPH_VSM_POOL_ROW * NPH_VSM_PAGE, "the pool's width in texels");

static u32 pool_height(const RenderFixture *f)
{
    const u32 rows =
        (f->renderer->vsm.pool_pages + (u32)NPH_VSM_POOL_ROW - 1U) / (u32)NPH_VSM_POOL_ROW;
    return rows * (u32)NPH_VSM_PAGE;
}

static void read_pool(RenderFixture *f, NphGpuPipeline pipeline, f32 *out)
{
    NphGpuDevice *dev = f->dev;
    const RenderVsm *v = &f->renderer->vsm;
    const u32 height = pool_height(f);
    const u64 size = (u64)POOL_W * height * sizeof(f32);
    NphGpuBuffer buf = test_make_buffer(dev, size, (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                                        NPH_GPU_MEMORY_READBACK, "pool readback");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "pool readback", &cmd),
                        NPH_RESULT_OK);
    // Depth images are sampled read-only (their bindless descriptors say so).
    const NphGpuLayout current = nph_gpu_image_layout(dev, v->pool, 0U);
    if (current != NPH_GPU_LAYOUT_DEPTH_READ_ONLY) {
        NphGpuImageBarrier b;
        memset(&b, 0, sizeof b);
        b.image = v->pool;
        b.src_stages = (u32)NPH_GPU_STAGE_ALL;
        b.src_access = (u32)NPH_GPU_ACCESS_MEMORY_WRITE;
        b.old_layout = current;
        b.dst_stages = (u32)NPH_GPU_STAGE_COMPUTE;
        b.dst_access = (u32)NPH_GPU_ACCESS_SHADER_READ;
        b.new_layout = NPH_GPU_LAYOUT_DEPTH_READ_ONLY;
        b.mip_count = 1U;
        b.layer_count = NPH_GPU_ALL_LAYERS;
        NphGpuBarrierDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.images = &b;
        desc.image_count = 1U;
        nph_gpu_cmd_barrier(cmd, &desc);
    }
    NphVsmPoolTestPush push;
    memset(&push, 0, sizeof push);
    push.dst = nph_gpu_buffer_address(dev, buf);
    push.pool_index = nph_gpu_image_sampled_index(dev, v->pool);
    push.sampler_index = nph_gpu_sampler_index(dev, f->renderer->clamp_sampler);
    push.width = POOL_W;
    push.height = height;
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, (POOL_W + 15U) / 16U, (height + 15U) / 16U, 1U);
    test_submit_wait(dev, cmd);
    memcpy(out, nph_gpu_buffer_map(dev, buf), (size_t)size);
    nph_gpu_buffer_destroy(dev, buf);
}

// The light a frame's floor loses to shadow: each pixel's shortfall from
// the brightest, in linear light, summed (in pixels' worth of light).
static f64 floor_shadow(const u8 *img)
{
    const u64 n = (u64)LOD_TARGET * LOD_TARGET;
    f64 lit = 0.0;
    for (u64 i = 0; i < n; ++i) {
        lit = nph_f64_max(lit, (f64)nph_render_ref_srgb_to_linear((f32)img[i * 4U + 1U] / 255.0F));
    }
    f64 lost = 0.0;
    for (u64 i = 0; i < n; ++i) {
        lost += lit - (f64)nph_render_ref_srgb_to_linear((f32)img[i * 4U + 1U] / 255.0F);
    }
    return lit > 0.0 ? lost / lit : 0.0;
}

// A shadow map's casters, compared texel by texel with another frame's:
// the pages both hold, their texels where either frame has a caster nearer
// the light than the floor (VSM_FOLIAGE_GAP or more apart: the foliage
// hangs metres over it), and the depths where both do.
#define VSM_FOLIAGE_GAP 0.5

typedef struct CasterCompare {
    u32 pages;
    u32 both;  // texels both frames have a caster in
    u32 only0; // the first frame's alone
    u32 only1;
    f64 mean_error; // where both have one: the second's depth from the first's, in texels
    f64 max_error;
} CasterCompare;

static CasterCompare compare_casters(const f32 *pool0, const NphVsmEntry *entries0,
                                     const f32 *pool1, const NphVsmEntry *entries1)
{
    CasterCompare c;
    memset(&c, 0, sizeof c);
    const u32 held = NPH_VSM_ENTRY_VALID | NPH_VSM_ENTRY_DIRTY;
    f64 sum = 0.0;
    for (u32 e = 0; e < (u32)NPH_VSM_LEVELS * (u32)NPH_VSM_LEVEL_ENTRIES; ++e) {
        const NphVsmEntry *ea = &entries0[e];
        const NphVsmEntry *eb = &entries1[e];
        if ((ea->state & held) != NPH_VSM_ENTRY_VALID ||
            (eb->state & held) != NPH_VSM_ENTRY_VALID || ea->page_x != eb->page_x ||
            ea->page_y != eb->page_y) {
            continue;
        }
        c.pages += 1U;
        // A level's depth range is its side (vsm.c): 4 m at level 0,
        // doubling; its texel that side over its 128 pages of 128 texels.
        const u32 level = e / (u32)NPH_VSM_LEVEL_ENTRIES;
        const f64 side = 4.0 * (f64)(1U << level);
        const f64 texel = side / ((f64)NPH_VSM_WINDOW * (f64)NPH_VSM_PAGE);
        const u32 pa = ea->state & NPH_VSM_ENTRY_PAGE;
        const u32 pb = eb->state & NPH_VSM_ENTRY_PAGE;
        for (u32 y = 0; y < (u32)NPH_VSM_PAGE; ++y) {
            for (u32 x = 0; x < (u32)NPH_VSM_PAGE; ++x) {
                const u64 ia = ((u64)(pa / (u32)NPH_VSM_POOL_ROW) * NPH_VSM_PAGE + y) * POOL_W +
                               (u64)(pa % (u32)NPH_VSM_POOL_ROW) * NPH_VSM_PAGE + x;
                const u64 ib = ((u64)(pb / (u32)NPH_VSM_POOL_ROW) * NPH_VSM_PAGE + y) * POOL_W +
                               (u64)(pb % (u32)NPH_VSM_POOL_ROW) * NPH_VSM_PAGE + x;
                const f64 d0 = (f64)pool0[ia];
                const f64 d1 = (f64)pool1[ib];
                if (d0 == d1) {
                    continue; // the same caster in both (the floor), or none
                }
                const f64 apart = (d1 - d0) * side; // metres along the light
                if (nph_f64_abs(apart) < VSM_FOLIAGE_GAP) {
                    c.both += 1U;
                    sum += nph_f64_abs(apart) / texel;
                    c.max_error = nph_f64_max(c.max_error, nph_f64_abs(apart) / texel);
                } else if (apart > 0.0) {
                    c.only0 += 1U;
                } else {
                    c.only1 += 1U;
                }
            }
        }
    }
    c.mean_error = c.both != 0U ? sum / (f64)c.both : 0.0;
    return c;
}

// The voxel casters' scene: a grid of foliage (1,500 needles in a cube of
// 4 m, each turned at random) high over the floor and to the side of a
// camera looking straight down at it, far enough that a pixel spans four of
// the foliage's finest voxel cells. The map's pages for the floor then have
// texels of two to four of those cells, and draw the foliage at its cuts of
// voxel clusters (within half a texel: vsm_cull.comp), through the voxel
// casters' raster (ADR-0061, stage 4).
#define FOLIAGE_GRID  5U
#define FOLIAGE_COUNT (FOLIAGE_GRID * FOLIAGE_GRID)

typedef struct VoxelShadowScene {
    LodMesh mesh;
    f32 cell;   // the finest voxel cell
    f32 radius; // the mesh's reach from its origin
    NphRenderMesh caster;
    NphRenderInstance grid[FOLIAGE_COUNT];
    NphDVec3 at[FOLIAGE_COUNT];
    NphQuat turn[FOLIAGE_COUNT];
    NphRenderLightDesc sun;
    NphRenderCamera camera;
    f64 height;
} VoxelShadowScene;

static void voxel_shadow_scene(RenderFixture *f, VoxelShadowScene *s)
{
    memset(s, 0, sizeof *s);
    lod_foliage(&s->mesh, 1500U, 4.0F, 7U);
    NTEST_ASSERT_GT_UINT(s->mesh.lods.bricks.len, 0U);
    s->cell = NPH_F32_MAX;
    for (u32 i = 0; i < s->mesh.lods.bricks.len; ++i) {
        s->cell = nph_f32_min(s->cell, s->mesh.lods.bricks.data[i].cell);
    }
    const NphRenderMeshlets *cl = &s->mesh.lods.clusters;
    for (u32 i = 0; i < cl->bounds.len; ++i) {
        const NphRenderMeshletBounds *b = &cl->bounds.data[i];
        s->radius = nph_f32_max(s->radius, nph_vec3_length(b->center) + b->radius);
    }
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &s->mesh.desc, &s->caster),
                        NPH_RESULT_OK);
    const f32 fov = 0.5F;
    const f32 tan_half = tanf(0.5F * fov);
    s->height = (f64)(4.0F * s->cell * (f32)LOD_TARGET / (2.0F * tan_half));
    s->camera = nph_render_camera_look_at(nph_dvec3(0.0, s->height, 0.0), nph_dvec3(0.0, 0.0, 0.0),
                                          nph_vec3(0.0F, 0.0F, -1.0F), fov, 0.05F);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    (void)instance(f, plane, grey, at(0.0, 0.0, 0.0, (f32)(3.0 * s->height * (f64)tan_half)));
    // Five by five, 6 m apart, beyond the view's edge at their height (the
    // sun travels along +X at 45 degrees: a shadow lands that height along
    // X), their shadows around the view's centre.
    const f64 spacing = 6.0;
    const f64 reach = 0.5 * spacing * (f64)(FOLIAGE_GRID - 1U) + 2.0;
    const f64 up = ((f64)tan_half * s->height + reach) / (1.0 + (f64)tan_half) + 4.0;
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 41U, 1U);
    for (u32 i = 0; i < FOLIAGE_COUNT; ++i) {
        const f32 z = 2.0F * nph_pcg32_next_f32(&rng) - 1.0F;
        const f32 phi = 2.0F * NPH_PI * nph_pcg32_next_f32(&rng);
        const f32 rr = nph_f32_sqrt(nph_f32_max(1.0F - z * z, 0.0F));
        s->turn[i] =
            nph_quat_from_axis_angle(nph_vec3(rr * nph_f32_cos(phi), rr * nph_f32_sin(phi), z),
                                     2.0F * NPH_PI * nph_pcg32_next_f32(&rng));
        const u32 column = i % FOLIAGE_GRID;
        const u32 row = i / FOLIAGE_GRID;
        const f64 gx = spacing * ((f64)column - 0.5 * (f64)(FOLIAGE_GRID - 1U));
        const f64 gz = spacing * ((f64)row - 0.5 * (f64)(FOLIAGE_GRID - 1U));
        s->at[i] = nph_dvec3(gx - up, up, gz);
        s->grid[i] = instance(
            f, s->caster, grey,
            nph_render_transform_from_trs(s->at[i], s->turn[i], nph_vec3(1.0F, 1.0F, 1.0F)));
    }
    s->sun = sun_desc(true);
    (void)make_light(f, &s->sun);
}

static void voxel_shadow_scene_free(RenderFixture *f, VoxelShadowScene *s)
{
    for (u32 i = 0; i < FOLIAGE_COUNT; ++i) {
        nph_render_instance_destroy(f->renderer, s->grid[i]);
    }
    nph_render_mesh_destroy(f->renderer, s->caster);
    lod_mesh_free(&s->mesh);
}

// The map after a few frames of `v`: the floor's image in `img`, the pool's
// depths in `pool`, and the page table in `entries`.
#define VSM_ENTRY_COUNT 262144U
NPH_STATIC_ASSERT(VSM_ENTRY_COUNT == NPH_VSM_LEVELS * NPH_VSM_LEVEL_ENTRIES, "the page table");

static NphRenderFrameStats capture_map(RenderFixture *f, const NphRenderView *v, NphGpuImage target,
                                       NphGpuPipeline pool_reader, u8 *img, f32 *pool,
                                       NphVsmEntry *entries)
{
    const NphRenderFrameStats s = lod_frames(f, v, target, 1U + NPH_GPU_FRAMES_IN_FLIGHT, img);
    (void)lod_frames(f, v, target, 2U, img);
    read_pool(f, pool_reader, pool);
    test_read_buffer(f->dev, f->renderer->vsm.entries, 0U,
                     (u64)VSM_ENTRY_COUNT * sizeof(NphVsmEntry), entries);
    return s;
}

static NphGpuPipeline pool_reader(RenderFixture *f, NphGpuShader *shader)
{
    *shader = test_load_shader(f->dev, "vsm_pool_test.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = *shader;
    cd.name = "vsm pool test";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &cd, &pipeline), NPH_RESULT_OK);
    return pipeline;
}

// The cut the map draws a caster of scale 1 at in `level` (vsm_cull.comp's
// VSM_LOD_TEXELS: the largest power-of-two threshold within half the
// level's texel, in single precision as the shader works it out), or
// NPH_U32_MAX for level 0.
static u32 map_cut(u32 level, i32 exp0, u32 count)
{
    const f32 page = (f32)(4.0 * (f64)(1U << level) / (f64)NPH_VSM_WINDOW);
    const f32 allowed = 0.5F * page / (f32)NPH_VSM_PAGE;
    const i32 e = (i32)((nph_f32_to_bits(allowed) >> 23U) & 0xFFU) - 127 - exp0;
    return e >= 0 ? nph_min_u32((u32)e, count - 1U) : NPH_U32_MAX;
}

// The sun's ray from world point `p` along `d` against the cut's clusters
// (`cut` entries of `table` from `first`) of every instance of the scene,
// through the C twin of the shaders' voxel tests (nph_render_voxel_trace):
// the nearest hit's distance along the ray, or NPH_F64_MAX. A cluster of
// triangles in the cut counts in `*triangles`.
static f64 sun_ray(const VoxelShadowScene *s, const u32 *cut, u32 count, NphDVec3 p, NphVec3 d,
                   u32 *triangles)
{
    const NphRenderMeshlets *cl = &s->mesh.lods.clusters;
    f32 best = NPH_F32_MAX;
    for (u32 i = 0; i < FOLIAGE_COUNT; ++i) {
        const NphVec3 o = nph_quat_rotate_inverse(
            s->turn[i],
            nph_vec3((f32)(p.x - s->at[i].x), (f32)(p.y - s->at[i].y), (f32)(p.z - s->at[i].z)));
        const NphVec3 dir = nph_quat_rotate_inverse(s->turn[i], d);
        // The instance's reach, then each cluster's sphere.
        const f32 along = -nph_vec3_dot(o, dir);
        if (nph_vec3_length_sq(o) - along * along > s->radius * s->radius) {
            continue;
        }
        for (u32 k = 0; k < count; ++k) {
            const u32 c = cut[k];
            const NphRenderMeshlet *ml = &cl->meshlets.data[c];
            if (ml->vertex_count != 0U) {
                *triangles += 1U;
                continue;
            }
            const NphVec3 oc = nph_vec3_sub(cl->bounds.data[c].center, o);
            const f32 tc = nph_vec3_dot(oc, dir);
            const f32 r = cl->bounds.data[c].radius;
            if (nph_vec3_length_sq(oc) - tc * tc > r * r) {
                continue;
            }
            for (u32 b = 0; b < ml->triangle_count; ++b) {
                NphRenderVoxelHit hit;
                if (nph_render_voxel_trace(&s->mesh.lods.bricks.data[ml->vertex_offset + b],
                                           s->mesh.lods.voxels.data, o, dir, -NPH_F32_MAX, best,
                                           &hit)) {
                    best = nph_f32_min(best, hit.t);
                }
            }
        }
    }
    return best == NPH_F32_MAX ? NPH_F64_MAX : (f64)best;
}

// The voxel casters draw the map's texels where the sun's rays through the
// clusters drawn meet their voxels, at those hits' depths: every page the
// map holds, texel by texel, against the CPU's rays through the same cut's
// voxel clusters (light space and depth as vsm.c sets them up; the rays
// from each texel's centre). The floor below them is told apart by depth.
NTEST_F(Vsm, voxel_casters_draw_where_the_suns_rays_meet_them)
{
    REQUIRE_VSM(f);
    static VoxelShadowScene s;
    voxel_shadow_scene(f, &s);
    NphGpuShader shader = NPH_GPU_SHADER_NULL;
    const NphGpuPipeline reader = pool_reader(f, &shader);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, LOD_TARGET);
    static u8 img[LOD_PIXELS];
    const u64 texels = (u64)POOL_W * pool_height(f);
    NphAllocator *a = nph_allocator_system();
    f32 *pool = NPH_ALLOC_ARRAY(a, f32, texels);
    NphVsmEntry *entries = NPH_ALLOC_ARRAY(a, NphVsmEntry, VSM_ENTRY_COUNT);
    NTEST_ASSERT_TRUE(pool != NULL && entries != NULL);
    NphRenderView v;
    vsm_view(&v, s.camera);
    v.lod_error = 1.0F;
    const NphRenderFrameStats st = capture_map(f, &v, target, reader, img, pool, entries);
    log_stats("voxels", &st);
    NTEST_EXPECT_EQ_UINT(st.vsm_overflow, 0U);
    NTEST_EXPECT_GT_UINT(st.vsm_voxel_draws, 0U);

    // The map's cuts (vis_scene.c upload_lods) and light space (vsm.c).
    NphArrayU32 table;
    NPH_ARRAY_INIT(&table, a);
    i32 exp0 = 0;
    u32 cuts = 0U;
    NTEST_ASSERT_EQ_INT(nph_render_lod_cuts(s.mesh.lods.lods.data, s.mesh.lods.lods.len,
                                            s.mesh.lods.groups.data, s.mesh.lods.groups.len,
                                            NPH_RENDER_LOD_MAX_CUT_ENTRIES, &exp0, &cuts, &table),
                        NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(cuts, 0U);
    const NphVec3 d = nph_vec3_normalize(s.sun.direction);
    const NphVec3 up =
        nph_f32_abs(d.y) < 0.99F ? nph_vec3(0.0F, 1.0F, 0.0F) : nph_vec3(1.0F, 0.0F, 0.0F);
    const NphVec3 u = nph_vec3_normalize(nph_vec3_cross(d, up));
    const NphVec3 w = nph_vec3_cross(u, d);
    const NphDVec3 anchor = f->renderer->vsm.anchor;
    const u32 held = NPH_VSM_ENTRY_VALID | NPH_VSM_ENTRY_DIRTY;
    u32 pages = 0U;
    u32 both = 0U;
    u32 missed = 0U; // the CPU's hits the map lacks
    u32 extra = 0U;  // the map's voxels the CPU does not meet
    u32 triangles = 0U;
    u32 cut_seen = NPH_U32_MAX; // the first page's level's cut, and that level
    u32 level_seen = 0U;
    f64 worst = 0.0;
    f64 sum = 0.0;
    for (u32 e = 0; e < VSM_ENTRY_COUNT; ++e) {
        if ((entries[e].state & held) != NPH_VSM_ENTRY_VALID) {
            continue;
        }
        const u32 level = e / (u32)NPH_VSM_LEVEL_ENTRIES;
        const u32 cut = map_cut(level, exp0, cuts);
        NTEST_ASSERT_NE_UINT(cut, NPH_U32_MAX);
        if (pages == 0U) {
            cut_seen = cut;
            level_seen = level;
        }
        pages += 1U;
        const u32 *list = table.data + table.data[cut];
        const u32 count = table.data[cut + 1U] - table.data[cut];
        const f64 side = 4.0 * (f64)(1U << level);
        const f64 page = side / (f64)NPH_VSM_WINDOW;
        const f64 texel = page / (f64)NPH_VSM_PAGE;
        const f64 reference = f->renderer->vsm.depth_reference[level];
        const u32 phys = entries[e].state & NPH_VSM_ENTRY_PAGE;
        for (u32 y = 0; y < (u32)NPH_VSM_PAGE; ++y) {
            for (u32 x = 0; x < (u32)NPH_VSM_PAGE; ++x) {
                // The texel's centre in light space from the anchor (pages
                // count from it), on its depth-0 plane.
                const f64 lu = ((f64)entries[e].page_x + ((f64)x + 0.5) / NPH_VSM_PAGE) * page;
                const f64 lv = ((f64)entries[e].page_y + ((f64)y + 0.5) / NPH_VSM_PAGE) * page;
                const NphDVec3 p = {anchor.x + lu * (f64)u.x + lv * (f64)w.x,
                                    anchor.y + lu * (f64)u.y + lv * (f64)w.y,
                                    anchor.z + lu * (f64)u.z + lv * (f64)w.z};
                const f64 hit = sun_ray(&s, list, count, p, d, &triangles);
                // The floor's depth along it, and the map's.
                const f64 floor_at = -p.y / (f64)d.y;
                const u64 i = ((u64)(phys / (u32)NPH_VSM_POOL_ROW) * NPH_VSM_PAGE + y) * POOL_W +
                              (u64)(phys % (u32)NPH_VSM_POOL_ROW) * NPH_VSM_PAGE + x;
                const f64 stored = ((f64)pool[i] - 0.5) * side + reference;
                const bool cpu = hit < floor_at - VSM_FOLIAGE_GAP;
                const bool gpu = stored < floor_at - VSM_FOLIAGE_GAP;
                if (cpu && gpu) {
                    both += 1U;
                    const f64 err = nph_f64_abs(stored - hit) / texel;
                    sum += err;
                    worst = nph_f64_max(worst, err);
                } else if (cpu) {
                    missed += 1U;
                } else if (gpu) {
                    extra += 1U;
                }
            }
        }
    }
    NTEST_LOG("%u pages (the first of level %u, at cut %u of %u); %u texels the voxels cover on "
              "both, %u the map misses, %u it adds; depth %.5f texels from the CPU's on average, "
              "%.5f at worst",
              pages, level_seen, cut_seen, cuts, both, missed, extra,
              sum / (f64)nph_max_u32(both, 1U), worst);
    NTEST_EXPECT_EQ_UINT(triangles, 0U); // the cut is voxels alone
    NTEST_ASSERT_GT_UINT(both, 2000U);
    // Rays that graze a footprint within the rounding may land either side.
    NTEST_EXPECT_LE_UINT(missed + extra, both / 100U + 2U);
    NTEST_EXPECT_TRUE(worst < 0.01);

    NPH_ARRAY_FREE(&table);
    NPH_FREE_ARRAY(a, f32, pool, texels);
    NPH_FREE_ARRAY(a, NphVsmEntry, entries, VSM_ENTRY_COUNT);
    nph_gpu_pipeline_destroy(f->dev, reader);
    nph_gpu_shader_destroy(f->dev, shader);
    nph_gpu_image_destroy(f->dev, target);
    voxel_shadow_scene_free(f, &s);
}

// Against level 0's triangles in the same scene, texel by texel in the map:
// the voxels cover about the texels the needles do (their footprints keep
// the needles' coverage; seen along the sun, obliquely to the voxels' grid,
// as VoxelGpu.the_voxel_levels_keep_the_coverage sees them from the
// camera), at about their depth, and the floor loses about as much light.
NTEST_F(Vsm, a_voxel_caster_shadows_as_its_level_0)
{
    REQUIRE_VSM(f);
    static VoxelShadowScene s;
    voxel_shadow_scene(f, &s);
    NphGpuShader shader = NPH_GPU_SHADER_NULL;
    const NphGpuPipeline reader = pool_reader(f, &shader);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, LOD_TARGET);
    static u8 level0[LOD_PIXELS];
    static u8 voxels[LOD_PIXELS];
    const u64 texels = (u64)POOL_W * pool_height(f);
    NphAllocator *a = nph_allocator_system();
    f32 *pool0 = NPH_ALLOC_ARRAY(a, f32, texels);
    f32 *pool1 = NPH_ALLOC_ARRAY(a, f32, texels);
    NphVsmEntry *entries0 = NPH_ALLOC_ARRAY(a, NphVsmEntry, VSM_ENTRY_COUNT);
    NphVsmEntry *entries1 = NPH_ALLOC_ARRAY(a, NphVsmEntry, VSM_ENTRY_COUNT);
    NTEST_ASSERT_TRUE(pool0 != NULL && pool1 != NULL && entries0 != NULL && entries1 != NULL);
    NphRenderView v;
    vsm_view(&v, s.camera);
    v.tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v.lod_error = 0.0F; // every caster at level 0
    const NphRenderFrameStats s0 = capture_map(f, &v, target, reader, level0, pool0, entries0);
    v.lod_error = 1.0F;
    const NphRenderFrameStats s1 = capture_map(f, &v, target, reader, voxels, pool1, entries1);
    log_stats("level 0", &s0);
    log_stats("voxels", &s1);
    NTEST_LOG("finest voxel cell %.4f m, the camera %.1f m up; voxel draws %u, then %u",
              (f64)s.cell, s.height, s0.vsm_voxel_draws, s1.vsm_voxel_draws);
    NTEST_EXPECT_EQ_UINT(s0.vsm_overflow, 0U);
    NTEST_EXPECT_EQ_UINT(s1.vsm_overflow, 0U);
    NTEST_EXPECT_EQ_UINT(s0.vsm_voxel_draws, 0U);
    NTEST_EXPECT_GT_UINT(s1.vsm_voxel_draws, 0U);

    const CasterCompare c = compare_casters(pool0, entries0, pool1, entries1);
    const u32 n0 = c.both + c.only0;
    const u32 n1 = c.both + c.only1;
    NTEST_LOG("%u pages; the needles cover %u texels, the voxels %u (%+.1f %%), %u of them both; "
              "there the voxels' depth lies %.2f texels from the needles' on average, %.2f at "
              "worst",
              c.pages, n0, n1, 100.0 * ((f64)n1 / (f64)nph_max_u32(n0, 1U) - 1.0), c.both,
              c.mean_error, c.max_error);
    NTEST_ASSERT_GT_UINT(n0, 2000U);
    NTEST_EXPECT_TRUE(nph_f64_abs((f64)n1 - (f64)n0) <= 0.08 * (f64)n0);
    const f64 lost0 = floor_shadow(level0);
    const f64 lost1 = floor_shadow(voxels);
    NTEST_LOG("the floor loses %.1f pixels' light to the needles' shadow, %.1f to the voxels' "
              "(%+.1f %%)",
              lost0, lost1, 100.0 * (lost1 / nph_f64_max(lost0, 1.0) - 1.0));
    NTEST_EXPECT_TRUE(lost0 > 500.0);
    NTEST_EXPECT_TRUE(nph_f64_abs(lost1 - lost0) <= 0.08 * lost0);

    NPH_FREE_ARRAY(a, f32, pool0, texels);
    NPH_FREE_ARRAY(a, f32, pool1, texels);
    NPH_FREE_ARRAY(a, NphVsmEntry, entries0, VSM_ENTRY_COUNT);
    NPH_FREE_ARRAY(a, NphVsmEntry, entries1, VSM_ENTRY_COUNT);
    nph_gpu_pipeline_destroy(f->dev, reader);
    nph_gpu_shader_destroy(f->dev, shader);
    nph_gpu_image_destroy(f->dev, target);
    voxel_shadow_scene_free(f, &s);
}
#undef FOLIAGE_COUNT
#undef FOLIAGE_GRID

// ---------------------------------------------------------------------------
// The depth pyramid's requests for blocks of pixels
// ---------------------------------------------------------------------------

// A depth image (odd extents: the last row's and column's blocks are
// clamped) of a view looking down -z (camera-relative space is view space),
// its projection's x and y moved as the frame's jitter moves them.
#define BLOCK_W       255U
#define BLOCK_H       143U
#define BLOCK_NEAR    0.1F
#define BLOCK_TAN     0.55F // tan(yfov / 2)
#define BLOCK_SHIFT_X (0.37F * 2.0F / (f32)BLOCK_W)
#define BLOCK_SHIFT_Y (-0.21F * 2.0F / (f32)BLOCK_H)
#define BLOCK_WORDS   ((u32)NPH_VSM_LEVELS * NPH_VSM_LEVEL_WORDS)

typedef struct BlockBox {
    NphVec3 lo;
    NphVec3 hi;
} BlockBox;

// The view ray through NDC (x, y), z = -1: a point t along it is t metres
// in front (depth near / t).
static NphVec3 block_ray(f32 x, f32 y)
{
    const f32 p00 = (f32)BLOCK_H / ((f32)BLOCK_W * BLOCK_TAN);
    const f32 p11 = -1.0F / BLOCK_TAN; // Vulkan's y down
    return nph_vec3((x + BLOCK_SHIFT_X) / p00, (y + BLOCK_SHIFT_Y) / p11, -1.0F);
}

// The nearest t > 0 where the ray meets the box (slabs), else `best`.
static f32 block_hit_box(NphVec3 dir, const BlockBox *b, f32 best)
{
    f32 t0 = 0.0F;
    f32 t1 = best;
    const f32 d[3] = {dir.x, dir.y, dir.z};
    const f32 lo[3] = {b->lo.x, b->lo.y, b->lo.z};
    const f32 hi[3] = {b->hi.x, b->hi.y, b->hi.z};
    for (u32 k = 0; k < 3U; ++k) {
        const f32 a = lo[k] / d[k];
        const f32 c = hi[k] / d[k];
        t0 = nph_f32_max(t0, nph_f32_min(a, c));
        t1 = nph_f32_min(t1, nph_f32_max(a, c));
    }
    return t0 <= t1 && t0 > 0.0F ? t0 : best;
}

// Ground to the horizon (grazing far off), boxes near, mid-way and far, a
// wall beyond 2 km, foliage-like noise (every pixel its own distance, or
// the background), and the background.
static void block_depths(f32 *depths)
{
    static const BlockBox boxes[] = {
        {{-0.45F, -0.5F, -1.3F}, {-0.1F, 0.25F, -0.55F}},
        {{0.5F, -1.6F, -9.0F}, {3.0F, 1.5F, -7.0F}},
        {{-30.0F, -1.6F, -130.0F}, {-8.0F, 25.0F, -90.0F}},
        {{-3000.0F, -1.6F, -2600.0F}, {-200.0F, 900.0F, -2500.0F}},
    };
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0xB10C5EEDULL, 3U);
    for (u32 y = 0; y < BLOCK_H; ++y) {
        for (u32 x = 0; x < BLOCK_W; ++x) {
            const NphVec3 dir = block_ray(((f32)x + 0.5F) * 2.0F / (f32)BLOCK_W - 1.0F,
                                          ((f32)y + 0.5F) * 2.0F / (f32)BLOCK_H - 1.0F);
            f32 t = 1.0e9F;
            if (dir.y < 0.0F) {
                t = -1.6F / dir.y;
            }
            for (u32 b = 0; b < sizeof boxes / sizeof boxes[0]; ++b) {
                t = block_hit_box(dir, &boxes[b], t);
            }
            const u32 noise = nph_pcg32_next_u32(&rng);
            if (x >= 150U && x < 220U && y >= 20U && y < 70U) {
                t = (noise & 1U) != 0U ? 1.0e9F : 6.0F + (f32)(noise >> 8U) * (19.0F / 16777216.0F);
            }
            depths[y * BLOCK_W + x] = t < 1.0e8F ? BLOCK_NEAR / t : 0.0F;
        }
    }
}

// The virtual shadow map's view for the image: light space's axes from the
// light's direction, the rays (nph_vsm.h) of the image's projection, and
// levels around a camera off their pages' grid. `lod_scale` picks the
// levels the pixels take.
static void block_view(NphVsmView *vv, NphVec3 light, f32 lod_scale, u64 requests)
{
    memset(vv, 0, sizeof *vv);
    const NphVec3 d = nph_vec3_normalize(light);
    const NphVec3 u = nph_vec3_normalize(nph_vec3_cross(d, nph_vec3(0.0F, 1.0F, 0.0F)));
    const NphVec3 w = nph_vec3_cross(u, d);
    vv->axis_u = nph_vec4_from_vec3(u, 0.0F);
    vv->axis_v = nph_vec4_from_vec3(w, 0.0F);
    vv->axis_d = nph_vec4_from_vec3(d, 0.0F);
    // A pixel's point is (near / depth) (x', y', -1), (x', y') its ray's
    // (block_ray): in light space (ray_x x + ray_y y + ray_o) / depth.
    const f32 p00 = (f32)BLOCK_H / ((f32)BLOCK_W * BLOCK_TAN);
    const f32 p11 = -1.0F / BLOCK_TAN;
    const NphVec3 cx = nph_vec3(u.x, w.x, d.x);
    const NphVec3 cy = nph_vec3(u.y, w.y, d.y);
    const NphVec3 cz = nph_vec3(u.z, w.z, d.z);
    vv->ray_x = nph_vec4_from_vec3(nph_vec3_scale(cx, BLOCK_NEAR / p00), 0.0F);
    vv->ray_y = nph_vec4_from_vec3(nph_vec3_scale(cy, BLOCK_NEAR / p11), 0.0F);
    const NphVec3 o = nph_vec3_sub(nph_vec3_add(nph_vec3_scale(cx, BLOCK_SHIFT_X / p00),
                                                nph_vec3_scale(cy, BLOCK_SHIFT_Y / p11)),
                                   cz);
    vv->ray_o = nph_vec4_from_vec3(nph_vec3_scale(o, BLOCK_NEAR), 0.0F);
    // Level 0's window 8 m across, the camera at (0.37, -0.53) m in light
    // space: off every level's page grid.
    for (u32 i = 0; i < (u32)NPH_VSM_LEVELS; ++i) {
        const f64 page = 8.0 * (f64)(1U << i) / (f64)NPH_VSM_WINDOW;
        const f64 wx = nph_f64_floor(0.37 / page) - (f64)NPH_VSM_WINDOW * 0.5;
        const f64 wy = nph_f64_floor(-0.53 / page) - (f64)NPH_VSM_WINDOW * 0.5;
        NphVsmLevel *l = &vv->levels[i];
        l->origin = nph_vec4((f32)(0.37 / page - wx), (f32)(-0.53 / page - wy), 0.5F, 0.0F);
        l->window_x = (i32)wx;
        l->window_y = (i32)wy;
        l->inv_page_size = (f32)(1.0 / page);
        l->depth_scale = (f32)(1.0 / (page * (f64)NPH_VSM_WINDOW));
        l->page_size = (f32)page;
    }
    vv->lod_scale = lod_scale;
    vv->margin = 2.0F / (f32)NPH_VSM_PAGE;
    vv->requests = requests;
}

// The pages the image's blocks request (into `blocks`) and the pages its
// pixels request one at a time (into `pixels`), under a view of the light.
static void block_requests(RenderFixture *f, NphGpuPipeline pipeline, NphVec3 light, f32 lod_scale,
                           u32 *blocks, u32 *pixels)
{
    NphGpuDevice *dev = f->dev;
    const u32 storage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    const u32 bits_usage =
        storage | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    const u64 bits_size = (u64)BLOCK_WORDS * sizeof(u32);
    NphGpuBuffer view_buf =
        test_make_buffer(dev, sizeof(NphVsmView), storage, NPH_GPU_MEMORY_UPLOAD, "block view");
    NphGpuBuffer depth_buf = test_make_buffer(dev, (u64)BLOCK_W * BLOCK_H * sizeof(f32), storage,
                                              NPH_GPU_MEMORY_UPLOAD, "block depths");
    NphGpuBuffer block_bits =
        test_make_buffer(dev, bits_size, bits_usage, NPH_GPU_MEMORY_DEVICE, "block requests");
    NphGpuBuffer pixel_bits =
        test_make_buffer(dev, bits_size, bits_usage, NPH_GPU_MEMORY_DEVICE, "pixel requests");
    block_view((NphVsmView *)nph_gpu_buffer_map(dev, view_buf), light, lod_scale,
               nph_gpu_buffer_address(dev, block_bits));
    block_depths((f32 *)nph_gpu_buffer_map(dev, depth_buf));

    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "block requests", &cmd),
                        NPH_RESULT_OK);
    const NphGpuBuffer bits[2] = {block_bits, pixel_bits};
    for (u32 i = 0; i < 2U; ++i) {
        nph_gpu_cmd_fill_buffer(cmd, bits[i], 0U, NPH_GPU_WHOLE_SIZE, 0U);
        nph_gpu_cmd_buffer_barrier(cmd, bits[i], (u32)NPH_GPU_STAGE_TRANSFER,
                                   (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, (u32)NPH_GPU_STAGE_COMPUTE,
                                   (u32)NPH_GPU_ACCESS_SHADER_READ |
                                       (u32)NPH_GPU_ACCESS_SHADER_WRITE);
    }
    NphVsmTestPush push;
    memset(&push, 0, sizeof push);
    push.view = nph_gpu_buffer_address(dev, view_buf);
    push.depths = nph_gpu_buffer_address(dev, depth_buf);
    push.reference = nph_gpu_buffer_address(dev, pixel_bits);
    push.width = BLOCK_W;
    push.height = BLOCK_H;
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    // An invocation per block, 16 x 16 to a workgroup.
    nph_gpu_cmd_dispatch(cmd, ((BLOCK_W + 1U) / 2U + 15U) / 16U, ((BLOCK_H + 1U) / 2U + 15U) / 16U,
                         1U);
    test_submit_wait(dev, cmd);
    test_read_buffer(dev, block_bits, 0U, bits_size, blocks);
    test_read_buffer(dev, pixel_bits, 0U, bits_size, pixels);
    nph_gpu_buffer_destroy(dev, pixel_bits);
    nph_gpu_buffer_destroy(dev, block_bits);
    nph_gpu_buffer_destroy(dev, depth_buf);
    nph_gpu_buffer_destroy(dev, view_buf);
}

static u32 popcount_u32(u32 v)
{
    u32 n = 0U;
    for (; v != 0U; v &= v - 1U) {
        ++n;
    }
    return n;
}

// The depth pyramid's requests for 2 x 2 blocks of pixels (vsm_block.glsl:
// one request for a block on one surface, each pixel's own elsewhere,
// gathered across the subgroup) against every pixel requesting its own
// (vsm_request), over an image built to take every way through them:
// surfaces across pages, their margins and corners and the levels'
// boundaries, edges between near and far surfaces, the background, ground
// grazing far off, noise as in foliage, and (with a level scale that makes
// the pixels' levels fine) points beyond the window of their level, and of
// every level. The two bitmasks are the same, bit for bit.
NTEST_F(Vsm, block_requests_are_each_pixels)
{
    REQUIRE_VSM(f);
    NphGpuShader shader = test_load_shader(f->dev, "vsm_request_test.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    cd.name = "vsm request test";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &cd, &pipeline), NPH_RESULT_OK);
    // A pixel's footprint per metre over level 0's texel (vsm.c): the
    // image's (a few pages at each level), 16 times finer (pages some 8
    // pixels across: many blocks across their edges and corners), and 300
    // times finer (most points beyond their level's window).
    const f32 texel0 = 8.0F / (f32)(NPH_VSM_WINDOW * NPH_VSM_PAGE);
    const f32 scale = 2.0F * BLOCK_TAN / (f32)BLOCK_H / texel0;
    const struct {
        NphVec3 light;
        f32 lod_scale;
        u32 pages; // at least
        const char *what;
    } cases[] = {
        {{0.31F, -0.8F, -0.52F}, scale, 16U, "the image's levels"},
        {{-0.62F, -0.35F, 0.7F}, scale, 16U, "a low sun"},
        {{0.31F, -0.8F, -0.52F}, scale / 16.0F, 300U, "small pages"},
        {{-0.62F, -0.35F, 0.7F}, scale / 16.0F, 300U, "small pages, a low sun"},
        {{0.31F, -0.8F, -0.52F}, scale / 300.0F, 2000U, "levels too fine for their windows"},
    };
    static u32 blocks[BLOCK_WORDS];
    static u32 pixels[BLOCK_WORDS];
    for (u32 c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        block_requests(f, pipeline, cases[c].light, cases[c].lod_scale, blocks, pixels);
        u32 pages = 0U;
        u32 levels = 0U;
        u32 wrong = 0U;
        for (u32 l = 0; l < (u32)NPH_VSM_LEVELS; ++l) {
            u32 level_pages = 0U;
            for (u32 i = 0; i < (u32)NPH_VSM_LEVEL_WORDS; ++i) {
                const u32 k = l * (u32)NPH_VSM_LEVEL_WORDS + i;
                level_pages += popcount_u32(pixels[k]);
                if (blocks[k] != pixels[k] && wrong++ < 4U) {
                    NTEST_LOG("%s: level %u, row %u, word %u: blocks 0x%08x, pixels 0x%08x",
                              cases[c].what, l, i / (u32)NPH_VSM_ROW_WORDS,
                              i % (u32)NPH_VSM_ROW_WORDS, blocks[k], pixels[k]);
                }
            }
            pages += level_pages;
            levels += level_pages != 0U ? 1U : 0U;
        }
        NTEST_LOG("%s: %u pages over %u levels", cases[c].what, pages, levels);
        NTEST_EXPECT_EQ_UINT(wrong, 0U);
        NTEST_EXPECT_GE_UINT(pages, cases[c].pages);
        NTEST_EXPECT_GT_UINT(levels, 4U);
    }
    nph_gpu_pipeline_destroy(f->dev, pipeline);
    nph_gpu_shader_destroy(f->dev, shader);
}
