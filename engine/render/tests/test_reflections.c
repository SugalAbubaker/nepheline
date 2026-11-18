// SPDX-License-Identifier: Apache-2.0
// Reflections (render/renderer.h NphRenderView.reflections; docs/render.md "Reflections"; ADR-0077;
// shaders/render/reflect_rays.comp, reflect.comp), against what a mirror shows by a ray the test
// casts itself, and against a ray a pixel from the renderer.
#include "render_internal.h"
#include "test_boxes.h"
#include "test_lod_support.h"

typedef RenderFixture Reflect;

NTEST_SETUP(Reflect)
{
    // The production's cells (2^18, and as many for reflections): cells are of the world, and at
    // the tests' 128 pixels each pixel is a cell or more, so the test renderer's would be full with
    // two views' and these tests are about what cells hold, not how many fit.
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.gi_cells = 1U << 18U;
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(Reflect)
{
    test_render_teardown(f);
}

#define REQUIRE_REFLECTIONS(f)                                                                     \
    do {                                                                                           \
        REQUIRE_PRODUCTION(f);                                                                     \
        if (!(f)->renderer->gi.supported) {                                                        \
            NTEST_SKIP("no radiance cache on this device (ray queries)");                          \
        }                                                                                          \
    } while (0)

#define SIZE   TEST_BOX_SIZE
#define PIXELS TEST_BOX_PIXELS
// Frames for cells to fill: every listed tile has its turn in 32.
#define SETTLE 96U

static NphRenderMaterial surface(RenderFixture *f, NphVec3 color, f32 metallic, f32 roughness)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(color.x, color.y, color.z, 1.0F);
    md.metallic = metallic;
    md.roughness = roughness;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

// The production view of the boxes with the radiance cache, without a temporal resolve to move its
// pixels.
static void reflect_view(NphRenderView *v, NphRenderCamera camera, NphRenderReflections mode)
{
    test_box_view(v, camera, NPH_RENDER_VIEW_LIT);
    v->anti_aliasing = NPH_RENDER_AA_NONE;
    v->gi = 1;
    v->reflections = mode;
}

static NphRenderFrameStats settled(RenderFixture *f, const NphRenderView *v, NphGpuImage target,
                                   u8 *out, u32 frames)
{
    NphRenderFrameStats s;
    memset(&s, 0, sizeof s);
    for (u32 i = 0; i + 1U < frames; ++i) {
        s = test_render_frame(f, v, target);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    return test_box_render(f, v, target, out);
}

// The mean of the pixels' first channels, linear, over those `mask` selects.
static f64 mean_of(const u8 *image, const bool *mask)
{
    f64 sum = 0.0;
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        if (mask[i]) {
            sum += (f64)test_box_linear(image, i);
            ++count;
        }
    }
    return count != 0U ? sum / (f64)count : 0.0;
}

// What a mirror's pixels show (a_mirror_shows_what_its_rays_meet's scene): of those whose rays
// along their mirror directions meet the wall (`seen`) and of those whose rays leave for the
// sky (`open`), neither near the wall's outline, how many are red, with reflections (`with`) and
// without.
typedef struct MirrorCounts {
    u32 seen;
    u32 seen_red;
    u32 seen_dark; // of `seen`, those that show nothing: an empty reflection
    u32 off_red;
    u32 open;
    u32 open_red;
} MirrorCounts;

static MirrorCounts mirror_counts(const TestBoxScene *scene, const u8 *with, const u8 *without)
{
    MirrorCounts c;
    memset(&c, 0, sizeof c);
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(scene, scene->eye, d, 0.0F, &t, &n) != 0) {
            continue; // the mirror alone
        }
        const NphVec3 x = nph_vec3_add(scene->eye, nph_vec3_scale(d, t));
        const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
        const NphVec3 r = nph_vec3_sub(d, nph_vec3_scale(n, 2.0F * nph_vec3_dot(d, n)));
        // Of the rays around it, all to the wall or none: not near its outline.
        u32 met = 0U;
        for (i32 a = -1; a <= 1; ++a) {
            for (i32 b = -1; b <= 1; ++b) {
                const NphVec3 w = nph_vec3_normalize(
                    nph_vec3_add(r, nph_vec3((f32)a * 0.05F, (f32)b * 0.05F, 0.0F)));
                f32 hit = 0.0F;
                NphVec3 hn;
                met += test_box_hit(scene, o, w, 0.0F, &hit, &hn) == 1 ? 1U : 0U;
            }
        }
        const u8 *px = with + (u64)i * 4U;
        const bool red_here = px[0] > (u32)px[1] + 24U;
        const bool red_off = without[(u64)i * 4U] > (u32)without[(u64)i * 4U + 1U] + 24U;
        if (met == 9U) {
            ++c.seen;
            c.seen_red += red_here ? 1U : 0U;
            c.seen_dark += px[0] < 24U && px[1] < 24U && px[2] < 24U ? 1U : 0U;
            c.off_red += red_off ? 1U : 0U;
        } else if (met == 0U) {
            ++c.open;
            c.open_red += red_here ? 1U : 0U;
        }
    }
    return c;
}

// A mirror on the ground, a red wall standing on it, and a black sky: a pixel of the mirror shows
// the wall exactly where a ray the test casts along its mirror direction meets the wall, off by
// about a cell (a dozen pixels' width at most): those next to the edge are not asked about.
NTEST_F(Reflect, a_mirror_shows_what_its_rays_meet)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMaterial mirror = surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.05F);
    const NphRenderMaterial red = surface(f, nph_vec3(1.0F, 0.05F, 0.05F), 0.0F, 1.0F);
    test_box_ground(f, &scene, mesh, mirror);
    test_box_add(f, &scene, mesh, red, nph_vec3(-3.0F, 0.0F, -2.0F), nph_vec3(3.0F, 3.0F, -1.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.3F, -1.0F, -0.6F)); // onto the wall's face
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 with[PIXELS];
    static u8 without[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_CACHE);
    const NphRenderFrameStats s = settled(f, &v, target, with, SETTLE);
    NTEST_EXPECT_TRUE(s.gi);
    NTEST_EXPECT_EQ_UINT(s.vis_reflect_dropped, 0U); // with room enough
    v.reflections = NPH_RENDER_REFLECTIONS_OFF;
    (void)settled(f, &v, target, without, 4U);

    const MirrorCounts c = mirror_counts(&scene, with, without);
    NTEST_LOG(
        "rays to the wall: %u pixels, %u red (%u without reflections); to the sky: %u, %u red",
        c.seen, c.seen_red, c.off_red, c.open, c.open_red);
    NTEST_ASSERT_GT_UINT(c.seen, 300U);
    NTEST_ASSERT_GT_UINT(c.open, 300U);
    NTEST_EXPECT_TRUE(c.seen_red * 20U >= c.seen * 19U);
    NTEST_EXPECT_EQ_UINT(c.off_red, 0U);
    NTEST_EXPECT_TRUE(c.open_red * 20U <= c.open);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// The mirror's scene with a wall whose colour comes from a 2 x 2 texture, red at its top left and
// green elsewhere, sampled nearest, through the coordinates' transform `uv`: what the mirror shows
// of it from an empty cache, by its cells (NPH_RENDER_REFLECTIONS_CACHE: the cache's update's rays)
// or by a ray a pixel (NPH_RENDER_REFLECTIONS_RAYS: reflect_rays.comp's).
static MirrorCounts textured_wall(RenderFixture *f, const NphRenderUvTransform *uv,
                                  NphRenderReflections mode)
{
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMaterial mirror = surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.05F);
    // sRGB 63 is linear 0.05: the red texel is the plain wall's red.
    static const u8 texels[16] = {255U, 63U,  63U, 255U, 63U, 255U, 63U, 255U,
                                  63U,  255U, 63U, 255U, 63U, 255U, 63U, 255U};
    const void *mips[1] = {texels};
    const u64 sizes[1] = {sizeof texels};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    td.width = 2U;
    td.height = 2U;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    NphRenderTexture tex = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &tex), NPH_RESULT_OK);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.metallic = 0.0F;
    md.roughness = 1.0F;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = tex;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].nearest = 1;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].wrap_u = NPH_RENDER_WRAP_CLAMP;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].wrap_v = NPH_RENDER_WRAP_CLAMP;
    md.uv = *uv;
    NphRenderMaterial wall = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &wall), NPH_RESULT_OK);
    test_box_ground(f, &scene, mesh, mirror);
    test_box_add(f, &scene, mesh, wall, nph_vec3(-3.0F, 0.0F, -2.0F), nph_vec3(3.0F, 3.0F, -1.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.3F, -1.0F, -0.6F)); // onto the wall's face
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 with[PIXELS];
    static u8 without[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, mode);
    (void)settled(f, &v, target, with, SETTLE);
    v.reflections = NPH_RENDER_REFLECTIONS_OFF;
    (void)settled(f, &v, target, without, 4U);
    const MirrorCounts c = mirror_counts(&scene, with, without);
    NTEST_LOG("rays to the wall: %u pixels, %u red (%u without reflections); to the sky: %u, %u "
              "red",
              c.seen, c.seen_red, c.off_red, c.open, c.open_red);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, tex);
    return c;
}

// The wall's red from its texture through its material's transform (ADR-0105): the cache's rays
// shade their hits (gi_hit.glsl, which the rays a pixel share) at the coordinates the transform
// makes. It takes every coordinate of the wall's faces (each a [0, 1] square) into the red
// texel, so the mirror shows the wall red.
NTEST_F(Reflect, a_mirror_shows_its_walls_texture_through_the_walls_transform)
{
    REQUIRE_REFLECTIONS(f);
    // (u, v) to (u / 4 + 1/8, v / 4 + 1/8): every face's coordinates in the top left texel.
    const NphRenderUvTransform into_red = {0.25F, 0.0F, 0.125F, 0.0F, 0.25F, 0.125F};
    const MirrorCounts c = textured_wall(f, &into_red, NPH_RENDER_REFLECTIONS_CACHE);
    NTEST_ASSERT_GT_UINT(c.seen, 300U);
    NTEST_EXPECT_EQ_UINT(c.off_red, 0U);
    NTEST_EXPECT_TRUE(c.seen_red * 20U >= c.seen * 19U);
}

// The same by a ray a pixel (ADR-0089): reflect_rays.comp shades its hits as the cache's update
// does, through the variant with the transforms (k_uv) while a material has one.
NTEST_F(Reflect, a_mirror_by_a_ray_a_pixel_shows_its_walls_texture_through_the_walls_transform)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderUvTransform into_red = {0.25F, 0.0F, 0.125F, 0.0F, 0.25F, 0.125F};
    const MirrorCounts c = textured_wall(f, &into_red, NPH_RENDER_REFLECTIONS_RAYS);
    NTEST_ASSERT_GT_UINT(c.seen, 300U);
    NTEST_EXPECT_EQ_UINT(c.off_red, 0U);
    NTEST_EXPECT_TRUE(c.seen_red * 20U >= c.seen * 19U);
    NTEST_EXPECT_TRUE(f->renderer->gi.reflect_uv.value != 0U);
}

// A mirror's wall changes in place (ADR-0105), after the view has settled on it as it was: from
// the identity's coordinates, three quarters green, to the transform into the red texel. `taa`:
// with the temporal resolve, as a view by default has it. Returns the mirror's red pixels of
// those whose rays meet the wall, on the frames `after` (1-based, rising) after the change.
static void follow_change(RenderFixture *f, NphRenderReflections mode, bool taa, const u32 *after,
                          u32 count, MirrorCounts *out)
{
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMaterial mirror = surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.05F);
    static const u8 texels[16] = {255U, 63U,  63U, 255U, 63U, 255U, 63U, 255U,
                                  63U,  255U, 63U, 255U, 63U, 255U, 63U, 255U};
    const void *mips[1] = {texels};
    const u64 sizes[1] = {sizeof texels};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    td.width = 2U;
    td.height = 2U;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    NphRenderTexture tex = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &tex), NPH_RESULT_OK);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.metallic = 0.0F;
    md.roughness = 1.0F;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = tex;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].nearest = 1;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].wrap_u = NPH_RENDER_WRAP_CLAMP;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].wrap_v = NPH_RENDER_WRAP_CLAMP;
    NphRenderMaterial wall = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &wall), NPH_RESULT_OK);
    test_box_ground(f, &scene, mesh, mirror);
    test_box_add(f, &scene, mesh, wall, nph_vec3(-3.0F, 0.0F, -2.0F), nph_vec3(3.0F, 3.0F, -1.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.3F, -1.0F, -0.6F)); // onto the wall's face
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 with[PIXELS];
    static u8 without[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, mode);
    if (taa) {
        v.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
    }
    NphRenderView off = v;
    off.reflections = NPH_RENDER_REFLECTIONS_OFF;
    (void)settled(f, &off, target, without, 4U);
    (void)settled(f, &v, target, with, SETTLE);
    const MirrorCounts before = mirror_counts(&scene, with, without);
    NTEST_LOG("before the change: %u of %u red", before.seen_red, before.seen);
    NTEST_ASSERT_GT_UINT(before.seen, 300U);
    NTEST_EXPECT_TRUE(before.seen_red * 2U < before.seen); // three quarters green
    const NphRenderUvTransform into_red = {0.25F, 0.0F, 0.125F, 0.0F, 0.25F, 0.125F};
    md.uv = into_red;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, wall, &md), NPH_RESULT_OK);
    u32 done = 0U;
    for (u32 k = 0; k < count; ++k) {
        (void)settled(f, &v, target, with, after[k] - done);
        done = after[k];
        out[k] = mirror_counts(&scene, with, without);
        NTEST_LOG("%u frame(s) after: %u of %u red", done, out[k].seen_red, out[k].seen);
    }
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, tex);
}

// By a ray a pixel (the default), what the mirror shows of a material changed in place changes the
// frame after, with the temporal resolve on: each pixel's ray meets the material as it now is, and
// the history follows a change that large at once.
NTEST_F(Reflect, a_mirror_by_a_ray_a_pixel_shows_a_material_changed_in_place_at_once)
{
    REQUIRE_REFLECTIONS(f);
    static const u32 after[1] = {1U};
    MirrorCounts c[1];
    follow_change(f, NPH_RENDER_REFLECTIONS_RAYS, true, after, 1U, c);
    NTEST_EXPECT_TRUE(c[0].seen_red * 20U >= c[0].seen * 19U);
    NTEST_EXPECT_EQ_UINT(c[0].seen_dark, 0U);
}

// By the reflection cells, the same a frame later (ADR-0105): the frame that marks the change gives
// every tile its turn, and the next frame's resolve drops the cells' histories before it takes
// those rays, so the cells hold them alone. The frame of the change shows the cells as they were,
// not empty ones. Without the restart the cells held the wall as it was for some 250 frames
// (their histories, 128 samples, at a turn every 32 frames).
NTEST_F(Reflect, the_reflection_cells_show_a_material_changed_in_place_a_frame_later)
{
    REQUIRE_REFLECTIONS(f);
    static const u32 after[3] = {1U, 2U, 64U};
    MirrorCounts c[3];
    follow_change(f, NPH_RENDER_REFLECTIONS_CACHE, false, after, 3U, c);
    NTEST_EXPECT_EQ_UINT(c[0].seen_dark, 0U);
    NTEST_EXPECT_TRUE(c[1].seen_red * 20U >= c[1].seen * 19U);
    // And the cells go on filling as any do: still the material as it now is.
    NTEST_EXPECT_TRUE(c[2].seen_red * 20U >= c[2].seen * 19U);
}

// The same under the temporal resolve: the cells show the change a frame later, and the history of
// the mirror's pixels, whose own material did not change, follows them as it follows any light
// that changes: within 16 frames here (a quarter of the way the frame after, nine tenths in 4).
NTEST_F(Reflect, the_reflection_cells_change_shows_through_the_temporal_resolve_soon)
{
    REQUIRE_REFLECTIONS(f);
    static const u32 after[2] = {1U, 16U};
    MirrorCounts c[2];
    follow_change(f, NPH_RENDER_REFLECTIONS_CACHE, true, after, 2U, c);
    NTEST_EXPECT_EQ_UINT(c[0].seen_dark, 0U);
    NTEST_EXPECT_TRUE(c[1].seen_red * 20U >= c[1].seen * 19U);
}

// The same wall without the transform: three quarters of each face are green, and so is most of
// the wall in the mirror, so the test above's red is the transform's. Each test draws from an
// empty cache: one that drew the wall otherwise holds that for some 100 to 200 frames here before
// it follows a material changed in place.
NTEST_F(Reflect, a_mirror_shows_its_walls_texture_as_its_coordinates_lay_it)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderUvTransform identity = nph_render_uv_identity();
    const MirrorCounts c = textured_wall(f, &identity, NPH_RENDER_REFLECTIONS_CACHE);
    NTEST_ASSERT_GT_UINT(c.seen, 300U);
    NTEST_EXPECT_EQ_UINT(c.off_red, 0U);
    NTEST_EXPECT_TRUE(c.seen_red * 2U < c.seen);
}

// ---------------------------------------------------------------------------
// A ray a pixel (ADR-0089)
// ---------------------------------------------------------------------------

// The mirror's scene again, its ground of the given roughness: the ground, a red wall standing on
// it in the sun, a black sky. Returns the sun.
static NphRenderLight wall_scene(RenderFixture *f, TestBoxScene *scene, f32 ground_roughness)
{
    const NphRenderMesh mesh = test_cube_mesh(f);
    memset(scene, 0, sizeof *scene);
    test_box_ground(f, scene, mesh,
                    surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, ground_roughness));
    test_box_add(f, scene, mesh, surface(f, nph_vec3(1.0F, 0.05F, 0.05F), 0.0F, 1.0F),
                 nph_vec3(-3.0F, 0.0F, -2.0F), nph_vec3(3.0F, 3.0F, -1.5F));
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.3F, -1.0F, -0.6F)); // onto the wall's face
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    return sun;
}

// A mirror takes its own ray alone: its lobe smears what it shows by less than a pixel, so the
// pixels around it have nothing to add but a blur (reflect_gather.comp). The wall's edge in the
// mirror is then a pixel wide: of the mirror's pixels, few are between the wall's red and the
// sky's black: none here, where gathered two pixels out a sixth of those by the edge are.
NTEST_F(Reflect, a_mirror_by_a_ray_a_pixel_stays_sharp)
{
    REQUIRE_REFLECTIONS(f);
    static TestBoxScene scene;
    const NphRenderLight sun = wall_scene(f, &scene, 0.05F);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_RAYS);
    (void)settled(f, &v, target, image, 4U);
    // The mirror's pixels whose mirror rays pass within a third of a metre of the wall's top edge
    // (y = 3 on the plane z = -1.5), and what the wall's red is in the mirror: the mean of those
    // whose rays meet the wall well below it.
    static f32 near_edge[SIZE * SIZE];
    static f32 reds[SIZE * SIZE];
    u32 near_count = 0U;
    u32 red_count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) != 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
        const NphVec3 r = nph_vec3_sub(d, nph_vec3_scale(n, 2.0F * nph_vec3_dot(d, n)));
        if (r.z >= -1e-4F) {
            continue;
        }
        const f32 s = (-1.5F - x.z) / r.z;
        const f32 height = x.y + s * r.y;
        if (s <= 0.0F || nph_f32_abs(x.x + s * r.x) > 2.5F) {
            continue;
        }
        if (height > 2.65F && height < 3.35F) {
            near_edge[near_count++] = test_box_linear(image, i);
        } else if (height > 1.5F && height < 2.5F) {
            reds[red_count++] = test_box_linear(image, i);
        }
    }
    NTEST_ASSERT_GT_UINT(near_count, 300U);
    NTEST_ASSERT_GT_UINT(red_count, 300U);
    f64 red_sum = 0.0;
    for (u32 i = 0; i < red_count; ++i) {
        red_sum += (f64)reds[i];
    }
    const f32 red = (f32)(red_sum / (f64)red_count);
    u32 between = 0U;
    for (u32 i = 0; i < near_count; ++i) {
        between += near_edge[i] > 0.2F * red && near_edge[i] < 0.8F * red ? 1U : 0U;
    }
    NTEST_LOG("the wall in the mirror %.4f; of %u pixels whose rays pass by its top edge, %u lie "
              "between it and the sky",
              (f64)red, near_count, between);
    NTEST_EXPECT_TRUE(red > 0.02F);
    NTEST_EXPECT_TRUE(between * 12U <= near_count);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// A ground just smooth enough for rays, whose lobe smears the wall's edge over several pixels: one
// ray a pixel is then one sample of that smear, another each frame, and a pixel's own would differ
// from frame to frame by most of the edge's contrast. Gathered with its neighbours' on the same
// surface (reflect_gather.comp), two frames differ by a fraction of that.
NTEST_F(Reflect, a_ray_a_pixel_is_gathered_with_its_neighbours)
{
    REQUIRE_REFLECTIONS(f);
    static TestBoxScene scene;
    const NphRenderLight sun = wall_scene(f, &scene, 0.22F);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 one[PIXELS];
    static u8 next[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_RAYS);
    (void)settled(f, &v, target, one, 8U);
    (void)test_box_render(f, &v, target, next);
    f64 level = 0.0;
    f64 differ = 0.0;
    u32 ground = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) != 0) {
            continue;
        }
        const f64 a = (f64)test_box_linear(one, i);
        const f64 b = (f64)test_box_linear(next, i);
        level += 0.5 * (a + b);
        differ += nph_f64_abs(a - b);
        ++ground;
    }
    NTEST_LOG("%u ground pixels: mean %.5f, two frames differ by %.5f a pixel (%.1f %% of it)",
              ground, level / (f64)ground, differ / (f64)ground, 100.0 * differ / level);
    NTEST_ASSERT_GT_UINT(ground, 4000U);
    NTEST_EXPECT_TRUE(level > 0.0);
    NTEST_EXPECT_TRUE(differ < 0.09 * level);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// A plate over the ground, both just smooth enough for rays: the plate's top shows the black sky,
// the ground seen beside it the red wall. The plate's pixels along its outline have the ground's
// two pixels away on the screen, but not on their plane, and take none of their light: they are as
// dark as the plate's middle. (Taken, the wall's red would come over the outline.)
NTEST_F(Reflect, a_ray_a_pixel_gathers_on_its_own_surface)
{
    REQUIRE_REFLECTIONS(f);
    static TestBoxScene scene;
    const NphRenderLight sun = wall_scene(f, &scene, 0.22F);
    test_box_add(f, &scene, test_cube_mesh(f),
                 surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.22F),
                 nph_vec3(-0.6F, 0.45F, 2.5F), nph_vec3(0.6F, 0.5F, 2.75F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_RAYS);
    (void)settled(f, &v, target, image, 8U);
    // Each pixel's surface: 0 the ground, 2 the plate's top, 3 anything else.
    static u8 what[SIZE * SIZE];
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        const i32 hit =
            test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n);
        what[i] = hit == 0 ? 0U : (hit == 2 && n.y > 0.5F ? 2U : 3U);
    }
    // The plate's pixels with the plate two above and two below them (the far and near edges show
    // other parts of the scene): those with the ground within two pixels beside them, those with
    // the plate all around, and the ground's beside the plate.
    f64 edge = 0.0;
    f64 middle = 0.0;
    f64 beside = 0.0;
    u32 edges = 0U;
    u32 middles = 0U;
    u32 besides = 0U;
    for (u32 y = 2U; y + 2U < SIZE; ++y) {
        for (u32 x = 2U; x + 2U < SIZE; ++x) {
            const u32 i = y * SIZE + x;
            bool column = true; // the same surface two above and below
            for (i32 dy = -2; dy <= 2; ++dy) {
                column = column && what[(u32)((i32)y + dy) * SIZE + x] == what[i];
            }
            u32 plate = 0U;
            u32 ground = 0U;
            for (i32 dx = -2; dx <= 2; ++dx) {
                const u8 w = what[y * SIZE + (u32)((i32)x + dx)];
                plate += w == 2U ? 1U : 0U;
                ground += w == 0U ? 1U : 0U;
            }
            const f64 value = (f64)test_box_linear(image, i);
            if (!column || plate + ground != 5U) {
                continue;
            }
            if (what[i] == 2U && plate == 5U) {
                middle += value;
                ++middles;
            } else if (what[i] == 2U) {
                edge += value;
                ++edges;
            } else if (plate != 0U) {
                beside += value;
                ++besides;
            }
        }
    }
    NTEST_LOG(
        "the plate: %u pixels at its sides %.5f, %u in its middle %.5f; the ground beside it, "
        "%u pixels, %.5f",
        edges, edges != 0U ? edge / (f64)edges : 0.0, middles,
        middles != 0U ? middle / (f64)middles : 0.0, besides,
        besides != 0U ? beside / (f64)besides : 0.0);
    NTEST_ASSERT_GT_UINT(edges, 12U);
    NTEST_ASSERT_GT_UINT(middles, 60U);
    NTEST_ASSERT_GT_UINT(besides, 12U);
    // The ground beside the plate shows the wall, the plate much less, and its sides no more than
    // its middle by a twentieth of the difference.
    const f64 contrast = beside / (f64)besides - middle / (f64)middles;
    NTEST_EXPECT_TRUE(contrast > 0.2);
    NTEST_EXPECT_TRUE(edge / (f64)edges < middle / (f64)middles + 0.05 * contrast);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// The slots of a table of `capacity` fingerprints that are taken.
static u32 slots_taken(RenderFixture *f, NphGpuBuffer keys, u32 capacity)
{
    NphAllocator *a = nph_allocator_system();
    u32 *slots = NPH_ALLOC_ARRAY(a, u32, capacity);
    NTEST_ASSERT_NOT_NULL(slots);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, keys, 0U, (u64)capacity * sizeof(u32), slots);
    u32 taken = 0U;
    for (u32 i = 0; i < capacity; ++i) {
        taken += slots[i] != 0U ? 1U : 0U;
    }
    NPH_FREE_ARRAY(a, u32, slots, capacity);
    return taken;
}

// Under temporal anti-aliasing a mirror shows its own cells, every frame (ADR-0086): the mirror and
// the red wall above, settled through the temporal resolve, against the same view without it. At
// the tests' 128 pixels a reflection cell is smaller than a pixel, and the mirror lies on the
// cells' faces (y = 0), so its cells are what the frame's jitter would most easily move: keyed by
// the jittered point, they fill in one jitter offset of the eight (a tile's turn, one frame in 32,
// comes at the same one), and a point on a face rounds to either side of it as each frame's
// jitter goes. Nor may a lookup take a neighbouring direction now and then (as a random pick once
// did): the flat mirror's rays seldom fill one. Each falls back on the dimmer light the irradiance
// cell has gathered: the wall's reflection dims and flickers, and the frames without the jitter
// fill cells of their own.
NTEST_F(Reflect, a_mirror_under_temporal_anti_aliasing_shows_its_own_cells)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMaterial mirror = surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.05F);
    const NphRenderMaterial red = surface(f, nph_vec3(1.0F, 0.05F, 0.05F), 0.0F, 1.0F);
    test_box_ground(f, &scene, mesh, mirror);
    test_box_add(f, &scene, mesh, red, nph_vec3(-3.0F, 0.0F, -2.0F), nph_vec3(3.0F, 3.0F, -1.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.3F, -1.0F, -0.6F));
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 temporal[PIXELS];
    static u8 next[PIXELS];
    static u8 still[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_CACHE);
    v.anti_aliasing = NPH_RENDER_AA_TEMPORAL;
    (void)settled(f, &v, target, temporal, SETTLE);
    (void)settled(f, &v, target, next, 1U);
    const RenderGi *gi = &f->renderer->gi;
    const u32 cells_jittered = slots_taken(f, gi->reflect_keys, gi->reflect_capacity);
    v.anti_aliasing = NPH_RENDER_AA_NONE;
    (void)settled(f, &v, target, still, 4U);
    const u32 cells_still = slots_taken(f, gi->reflect_keys, gi->reflect_capacity);

    // The mirror's pixels whose rays around their own all meet the wall (as above), away from its
    // outline, where the temporal resolve's own filtering moves nothing.
    static bool on_wall[SIZE * SIZE];
    memset(on_wall, 0, sizeof on_wall);
    u32 seen = 0U;
    f64 moved = 0.0;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) != 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
        const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
        const NphVec3 r = nph_vec3_sub(d, nph_vec3_scale(n, 2.0F * nph_vec3_dot(d, n)));
        u32 met = 0U;
        for (i32 a = -1; a <= 1; ++a) {
            for (i32 b = -1; b <= 1; ++b) {
                const NphVec3 w = nph_vec3_normalize(
                    nph_vec3_add(r, nph_vec3((f32)a * 0.05F, (f32)b * 0.05F, 0.0F)));
                f32 hit = 0.0F;
                NphVec3 hn;
                met += test_box_hit(&scene, o, w, 0.0F, &hit, &hn) == 1 ? 1U : 0U;
            }
        }
        if (met == 9U) {
            on_wall[i] = true;
            ++seen;
            const i32 a = temporal[(u64)i * 4U];
            const i32 b = next[(u64)i * 4U];
            moved += (f64)(a > b ? a - b : b - a);
        }
    }
    const f64 m_temporal = mean_of(temporal, on_wall);
    const f64 m_still = mean_of(still, on_wall);
    moved /= (f64)nph_max_u32(seen, 1U);
    NTEST_LOG(
        "%u pixels of the wall's reflection: red %.4f with the temporal resolve, %.4f without; "
        "%.2f levels from a frame to the next; reflection cells %u, then %u without the jitter",
        seen, m_temporal, m_still, moved, cells_jittered, cells_still);
    NTEST_ASSERT_GT_UINT(seen, 300U);
    NTEST_EXPECT_TRUE(m_still > 0.3);
    NTEST_EXPECT_TRUE(nph_f64_abs(m_temporal / m_still - 1.0) < 0.05);
    NTEST_EXPECT_TRUE(moved < 1.0);
    // The frames without the jitter find the cells the jittered ones filled.
    NTEST_EXPECT_TRUE(cells_still <= cells_jittered + cells_jittered / 100U);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// The mean red of a mirror's pixels whose rays around their own all meet the wall (as above), and
// how many there are, in `image` of the scene's camera.
static f64 wall_red(const TestBoxScene *scene, const u8 *image, u32 *count)
{
    static bool on_wall[SIZE * SIZE];
    memset(on_wall, 0, sizeof on_wall);
    u32 seen = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(scene, scene->eye, d, 0.0F, &t, &n) != 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(scene->eye, nph_vec3_scale(d, t));
        const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
        const NphVec3 r = nph_vec3_sub(d, nph_vec3_scale(n, 2.0F * nph_vec3_dot(d, n)));
        u32 met = 0U;
        for (i32 a = -1; a <= 1; ++a) {
            for (i32 b = -1; b <= 1; ++b) {
                const NphVec3 w = nph_vec3_normalize(
                    nph_vec3_add(r, nph_vec3((f32)a * 0.05F, (f32)b * 0.05F, 0.0F)));
                f32 hit = 0.0F;
                NphVec3 hn;
                met += test_box_hit(scene, o, w, 0.0F, &hit, &hn) == 1 ? 1U : 0U;
            }
        }
        if (met == 9U) {
            on_wall[i] = true;
            ++seen;
        }
    }
    *count = seen;
    return mean_of(image, on_wall);
}

// A camera's move gives a mirror's pixels new directions and new cells, which a tile's turn, one
// frame in 32, would leave unfilled for as long (ADR-0087). The mirror and red wall above, settled,
// then the camera a step aside, which gives every pixel new directions and cells: four frames
// after it, the tiles' rays have filled them (reflect_want), against the same view settled.
NTEST_F(Reflect, a_mirror_shows_its_wall_soon_after_the_camera_moves)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMaterial mirror = surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.05F);
    const NphRenderMaterial red = surface(f, nph_vec3(1.0F, 0.05F, 0.05F), 0.0F, 1.0F);
    test_box_ground(f, &scene, mesh, mirror);
    test_box_add(f, &scene, mesh, red, nph_vec3(-3.0F, 0.0F, -2.0F), nph_vec3(3.0F, 3.0F, -1.5F));
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.3F, -1.0F, -0.6F));
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 soon[PIXELS];
    static u8 later[PIXELS];
    NphRenderView v;
    reflect_view(&v, test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3_zero(), 0.8F),
                 NPH_RENDER_REFLECTIONS_CACHE);
    (void)settled(f, &v, target, later, SETTLE);

    reflect_view(
        &v, test_box_camera(&scene, nph_vec3(0.6F, 1.5F, 3.8F), nph_vec3(0.1F, 0.0F, 0.0F), 0.8F),
        NPH_RENDER_REFLECTIONS_CACHE);
    (void)settled(f, &v, target, soon, 4U);
    u32 seen = 0U;
    const f64 m_soon = wall_red(&scene, soon, &seen);
    (void)settled(f, &v, target, later, SETTLE);
    const f64 m_later = wall_red(&scene, later, &seen);
    NTEST_LOG("%u pixels of the wall's reflection: red %.4f four frames after the move, %.4f "
              "settled",
              seen, m_soon, m_later);
    NTEST_ASSERT_GT_UINT(seen, 300U);
    NTEST_EXPECT_TRUE(m_later > 0.3);
    NTEST_EXPECT_TRUE(nph_f64_abs(m_soon / m_later - 1.0) < 0.1);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// Under a sky of the same light everywhere, with nothing to hide it, the cells gather what the
// sky's specular gives: the three modes agree, smooth or rough, metal or not.
NTEST_F(Reflect, an_open_surface_under_an_even_sky_loses_nothing)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static const struct {
        f32 metallic;
        f32 roughness;
    } k_surfaces[4] = {{1.0F, 0.05F}, {1.0F, 0.6F}, {0.0F, 0.1F}, {0.0F, 0.8F}};
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    static u8 rays[PIXELS];
    static bool ground[SIZE * SIZE];
    for (u32 k = 0; k < 4U; ++k) {
        static TestBoxScene scene;
        memset(&scene, 0, sizeof scene);
        test_box_ground(f, &scene, mesh,
                        surface(f, nph_vec3(0.8F, 0.8F, 0.8F), k_surfaces[k].metallic,
                                k_surfaces[k].roughness));
        const NphRenderCamera camera =
            test_box_camera(&scene, nph_vec3(0.0F, 3.0F, 3.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
        for (u32 i = 0; i < SIZE * SIZE; ++i) {
            f32 t = 0.0F;
            NphVec3 n;
            ground[i] =
                test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) >= 0;
        }
        NphRenderView v;
        reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_OFF);
        (void)settled(f, &v, target, off, SETTLE);
        v.reflections = NPH_RENDER_REFLECTIONS_CACHE;
        (void)settled(f, &v, target, on, SETTLE);
        v.reflections = NPH_RENDER_REFLECTIONS_RAYS;
        (void)settled(f, &v, target, rays, 4U);
        const f64 m_off = mean_of(off, ground);
        const f64 m_on = mean_of(on, ground);
        const f64 m_rays = mean_of(rays, ground);
        NTEST_LOG("metallic %.0f, roughness %.2f: without %.4f, cells %.4f, rays %.4f",
                  (f64)k_surfaces[k].metallic, (f64)k_surfaces[k].roughness, m_off, m_on, m_rays);
        NTEST_EXPECT_TRUE(m_off > 0.05);
        NTEST_EXPECT_TRUE(nph_f64_abs(m_on / m_off - 1.0) < 0.06);
        NTEST_EXPECT_TRUE(nph_f64_abs(m_rays / m_off - 1.0) < 0.06);
        test_box_show(f, &scene, false);
    }
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// A smooth metal ground under an even sky, and a black card over it, beyond the camera's view. Its
// material alpha tested with an alpha under the cutoff, the card is cut away everywhere: the
// reflections' rays pass it (rt_mask.glsl), and the ground shows the sky as with the card hidden,
// from cells filled while the card was there and from a ray a pixel. Opaque, the card is what the
// ground mirrors. (The rays a pixel hold nothing from one frame to the next, so each of theirs is
// what its own frame's rays met; the cells keep what they gathered, so theirs comes first.)
NTEST_F(Reflect, a_cut_away_surface_is_not_reflected)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, surface(f, nph_vec3(0.8F, 0.8F, 0.8F), 1.0F, 0.05F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 3.0F, 3.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    static bool ground[SIZE * SIZE];
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        ground[i] =
            test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) >= 0;
    }
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 4U, 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh plane = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.0F, 0.0F, 0.0F, 0.0F);
    md.alpha_mode = NPH_RENDER_ALPHA_MASK;
    md.double_sided = 1;
    NphRenderMaterial black = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &black), NPH_RESULT_OK);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = plane;
    id.materials = &black;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 6.0, -6.0), nph_quat_identity(),
                                                 nph_vec3(80.0F, 80.0F, 80.0F));
    NphRenderInstance card = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &card), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 cells[PIXELS];
    static u8 through[PIXELS];
    static u8 open[PIXELS];
    static u8 under[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_CACHE);
    (void)settled(f, &v, target, cells, SETTLE);
    v.reflections = NPH_RENDER_REFLECTIONS_RAYS;
    (void)settled(f, &v, target, through, 4U);
    nph_render_instance_set_hidden(f->renderer, card, true);
    (void)settled(f, &v, target, open, 4U);
    nph_render_instance_set_hidden(f->renderer, card, false);
    md.alpha_mode = NPH_RENDER_ALPHA_OPAQUE;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, black, &md), NPH_RESULT_OK);
    (void)settled(f, &v, target, under, 4U);
    const f64 m_cells = mean_of(cells, ground);
    const f64 m_through = mean_of(through, ground);
    const f64 m_open = mean_of(open, ground);
    const f64 m_under = mean_of(under, ground);
    NTEST_LOG("the mirror with the card cut away: cells %.4f, rays %.4f; no card: rays %.4f; under "
              "the opaque card: rays %.4f",
              m_cells, m_through, m_open, m_under);
    NTEST_EXPECT_TRUE(m_open > 0.05);
    NTEST_EXPECT_TRUE(nph_f64_abs(m_cells / m_open - 1.0) < 0.06);
    NTEST_EXPECT_TRUE(nph_f64_abs(m_through / m_open - 1.0) < 0.06);
    NTEST_EXPECT_TRUE(m_under < 0.4 * m_open);
    nph_render_instance_destroy(f->renderer, card);
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_material_destroy(f->renderer, black);
    nph_render_mesh_destroy(f->renderer, plane);
    nph_render_texture_destroy(f->renderer, env);
}

// A dark floor of roughness 0.22, a sunlit white wall standing on it and a black sky: all the floor
// shows is what it reflects, and a ray a pixel a frame is one sample of a lobe that covers the wall
// or misses it. Its ray gives a fifth of what such a floor reflects and what its irradiance cell
// has gathered the rest (gi_reflect_ray_share, ADR-0092), so a pixel's light changes from frame
// to frame by 0.0007 where the rays alone change it by 0.0030: less than a floor of roughness 0.1
// changes (0.0013), all of whose reflection is its ray's and whose lobe is half as wide.
NTEST_F(Reflect, a_rough_floors_reflection_holds_between_frames)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderMesh mesh = test_cube_mesh(f);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.3F, -1.0F, -0.6F)); // onto the wall's face
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    const NphRenderMaterial white = surface(f, nph_vec3(0.9F, 0.9F, 0.9F), 0.0F, 1.0F);
    static TestBoxScene scenes[2];
    f64 wander[2] = {0.0, 0.0};
    f64 level[2] = {0.0, 0.0};
    const f32 roughness[2] = {0.22F, 0.1F};
    for (u32 k = 0; k < 2U; ++k) {
        TestBoxScene *scene = &scenes[k];
        memset(scene, 0, sizeof *scene);
        test_box_ground(f, scene, mesh,
                        surface(f, nph_vec3(0.02F, 0.02F, 0.02F), 0.0F, roughness[k]));
        test_box_add(f, scene, mesh, white, nph_vec3(-1.0F, 0.0F, -2.0F),
                     nph_vec3(1.0F, 2.0F, -1.5F));
        if (k == 0U) {
            test_box_show(f, &scenes[0], true);
        } else {
            test_box_show(f, &scenes[0], false);
            test_box_show(f, &scenes[1], true);
        }
        const NphRenderCamera camera =
            test_box_camera(scene, nph_vec3(0.0F, 1.2F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
        NphRenderView v;
        reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_RAYS);
        static u8 image[PIXELS];
        (void)settled(f, &v, target, image, SETTLE);
        static f64 sum[SIZE * SIZE];
        static f64 sum2[SIZE * SIZE];
        memset(sum, 0, sizeof sum);
        memset(sum2, 0, sizeof sum2);
        const u32 frames = 16U;
        for (u32 i = 0; i < frames; ++i) {
            (void)settled(f, &v, target, image, 1U);
            for (u32 p = 0; p < SIZE * SIZE; ++p) {
                const f64 g = (f64)test_box_linear(image, p);
                sum[p] += g;
                sum2[p] += g * g;
            }
        }
        u32 count = 0U;
        for (u32 p = 0; p < SIZE * SIZE; ++p) {
            NphVec3 x;
            NphVec3 n;
            f32 t = 0.0F;
            // The floor before the wall, where the wall's reflection lies.
            if (test_box_hit(scene, scene->eye, test_box_pixel_ray(scene, p), 0.0F, &t, &n) != 0) {
                continue;
            }
            x = nph_vec3_add(scene->eye, nph_vec3_scale(test_box_pixel_ray(scene, p), t));
            if (nph_f32_abs(x.x) > 1.5F || x.z < -1.4F || x.z > 1.5F) {
                continue;
            }
            const f64 mean = sum[p] / (f64)frames;
            wander[k] += sqrt(nph_f64_max(sum2[p] / (f64)frames - mean * mean, 0.0));
            level[k] += mean;
            ++count;
        }
        NTEST_ASSERT_GT_UINT(count, 500U);
        wander[k] /= (f64)count;
        level[k] /= (f64)count;
    }
    NTEST_LOG(
        "a floor of roughness 0.22: %.4f, wandering by %.4f a pixel between frames (%.1f %%); "
        "of 0.1: %.4f, by %.4f (%.1f %%)",
        level[0], wander[0], 100.0 * wander[0] / level[0], level[1], wander[1],
        100.0 * wander[1] / level[1]);
    NTEST_EXPECT_TRUE(level[0] > 0.002);
    NTEST_EXPECT_TRUE(wander[1] > 0.0005); // the rays' light does change
    NTEST_EXPECT_TRUE(wander[0] < wander[1]);
    test_box_show(f, &scenes[1], false);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// A room with no way out, a sky outside it, and a sun that does not reach in: nothing in it can
// reflect the sky, which the environment's specular would have it do. The rays say how much is
// left, and the cells come near it. The walls' faces are rough and metal: all they show is what
// they reflect. (They reflect half the light: what a ray's hit reflects of the cache's light is
// its base colour's share, a metal's too, ADR-0092, and walls that reflect nine tenths pass the
// sky's light along the room until it is seven tenths of the open sky's.)
NTEST_F(Reflect, what_is_hidden_from_the_sky_does_not_reflect_it)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 1.0F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial metal = surface(f, nph_vec3(0.5F, 0.5F, 0.5F), 1.0F, 0.6F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_add(f, &scene, mesh, metal, nph_vec3(-3.0F, -1.0F, -3.0F), nph_vec3(3.0F, 0.0F, 3.0F));
    test_box_add(f, &scene, mesh, metal, nph_vec3(-3.0F, 3.0F, -3.0F), nph_vec3(3.0F, 4.0F, 3.0F));
    test_box_add(f, &scene, mesh, metal, nph_vec3(-4.0F, 0.0F, -3.0F), nph_vec3(-3.0F, 3.0F, 3.0F));
    test_box_add(f, &scene, mesh, metal, nph_vec3(3.0F, 0.0F, -3.0F), nph_vec3(4.0F, 3.0F, 3.0F));
    // The two ends are the camera's near plane's business: the view never leaves the room's axis.
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 2.0F), nph_vec3(0.0F, 1.0F, -2.0F), 0.6F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    static bool inside[SIZE * SIZE];
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        inside[i] =
            test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) >= 0;
    }
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_OFF);
    (void)settled(f, &v, target, off, SETTLE);
    v.reflections = NPH_RENDER_REFLECTIONS_CACHE;
    (void)settled(f, &v, target, on, SETTLE);
    const f64 m_off = mean_of(off, inside);
    const f64 m_on = mean_of(on, inside);
    NTEST_LOG("in the room: without %.4f, with %.4f", m_off, m_on);
    NTEST_EXPECT_TRUE(m_off > 0.1);
    NTEST_EXPECT_TRUE(m_on < 0.7 * m_off);
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// What a smooth pixel leaves is taken away with the frame's reflection. A tile holding a smooth
// pixel is looked through whole, so that a pixel of it no longer smooth would take what it left
// frames ago: here a mirror all over goes, and a rough ground takes its place with a smooth bar
// across it, whose tiles stay listed. Those pixels show what they showed before there was a mirror.
NTEST_F(Reflect, what_a_pixel_left_goes_with_its_frame)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial shiny = surface(f, nph_vec3(0.9F, 0.9F, 0.9F), 1.0F, 0.05F);
    static TestBoxScene rough_scene;
    static TestBoxScene smooth_scene;
    memset(&rough_scene, 0, sizeof rough_scene);
    memset(&smooth_scene, 0, sizeof smooth_scene);
    test_box_ground(f, &rough_scene, mesh, surface(f, nph_vec3(0.05F, 0.05F, 0.05F), 0.0F, 1.0F));
    test_box_add(f, &rough_scene, mesh, shiny, nph_vec3(-3.0F, 0.0F, -0.02F),
                 nph_vec3(3.0F, 0.02F, 0.02F));
    test_box_ground(f, &smooth_scene, mesh, shiny);
    const NphRenderCamera camera =
        test_box_camera(&rough_scene, nph_vec3(0.0F, 3.0F, 3.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 before[PIXELS];
    static u8 after[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_CACHE);
    test_box_show(f, &smooth_scene, false);
    (void)settled(f, &v, target, before, 48U);
    test_box_show(f, &rough_scene, false);
    test_box_show(f, &smooth_scene, true);
    (void)settled(f, &v, target, after, 4U);
    test_box_show(f, &rough_scene, true);
    test_box_show(f, &smooth_scene, false);
    (void)settled(f, &v, target, after, 1U);
    u32 worst = 0U;
    for (u64 i = 0; i < PIXELS; i += 4U) {
        const u32 a = before[i];
        const u32 b = after[i];
        worst = nph_max_u32(worst, a > b ? a - b : b - a);
    }
    NTEST_LOG("the worst pixel is %u levels off", worst);
    NTEST_EXPECT_TRUE(worst <= 6U);
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// A spot of a mirror shows one thing from one side and another from the other: its cell is for a
// direction as well as for a place. Seen from the side of a red wall, the spots around the middle
// show the wall; seen from as far off, at right angles to it, the black sky, not what the first
// view filled. (Cells grow with the distance from the camera: from as far, they are the same.)
NTEST_F(Reflect, a_cell_holds_one_direction)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.05F));
    test_box_add(f, &scene, mesh, surface(f, nph_vec3(1.0F, 0.05F, 0.05F), 0.0F, 1.0F),
                 nph_vec3(3.0F, 0.0F, -3.0F), nph_vec3(3.5F, 3.0F, 3.0F));
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.6F, -1.0F, 0.3F)); // onto the wall's face
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    const NphRenderCamera facing_wall =
        test_box_camera(&scene, nph_vec3(-3.0F, 1.5F, 0.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    reflect_view(&v, facing_wall, NPH_RENDER_REFLECTIONS_CACHE);
    (void)settled(f, &v, target, image, SETTLE);
    u32 middle_first = 0U;
    u32 red_first = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        if (test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) == 0) {
            const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
            if (nph_f32_abs(x.x) < 1.0F && nph_f32_abs(x.z) < 1.0F) {
                ++middle_first;
                red_first += image[(u64)i * 4U] > (u32)image[(u64)i * 4U + 1U] + 24U ? 1U : 0U;
            }
        }
    }
    const NphRenderCamera across =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, -3.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    reflect_view(&v, across, NPH_RENDER_REFLECTIONS_CACHE);
    (void)settled(f, &v, target, image, SETTLE);
    u32 middle = 0U;
    u32 red = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        if (test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) == 0) {
            const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
            if (nph_f32_abs(x.x) < 1.0F && nph_f32_abs(x.z) < 1.0F) {
                ++middle;
                red += image[(u64)i * 4U] > (u32)image[(u64)i * 4U + 1U] + 24U ? 1U : 0U;
            }
        }
    }
    NTEST_LOG("the middle facing the wall: %u pixels, %u red; across it: %u pixels, %u red",
              middle_first, red_first, middle, red);
    NTEST_ASSERT_GT_UINT(middle_first, 500U);
    NTEST_ASSERT_GT_UINT(middle, 500U);
    NTEST_EXPECT_TRUE(red_first * 10U >= middle_first * 9U);
    NTEST_EXPECT_TRUE(red * 10U <= middle);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// The floor, ceiling and walls closing a room of x in [-3, 3], y in [0, 3] and z in [-3, 3] in
// front of the back wall a test adds itself (z in [-3, -2]): five instances of `material`.
static void close_room(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial material,
                       NphRenderInstance walls[5])
{
    static const f32 k_walls[5][6] = {
        {-4.0F, -1.0F, -3.0F, 4.0F, 0.0F, 4.0F}, // floor
        {-4.0F, 3.0F, -3.0F, 4.0F, 4.0F, 4.0F},  // ceiling
        {-4.0F, 0.0F, -3.0F, -3.0F, 3.0F, 4.0F}, // left
        {3.0F, 0.0F, -3.0F, 4.0F, 3.0F, 4.0F},   // right
        {-3.0F, 0.0F, 3.0F, 3.0F, 3.0F, 4.0F},   // behind the camera
    };
    for (u32 i = 0; i < 5U; ++i) {
        const NphVec3 lo = nph_vec3(k_walls[i][0], k_walls[i][1], k_walls[i][2]);
        const NphVec3 hi = nph_vec3(k_walls[i][3], k_walls[i][4], k_walls[i][5]);
        NphRenderInstanceDesc id;
        memset(&id, 0, sizeof id);
        id.mesh = mesh;
        id.materials = &material;
        id.material_count = 1U;
        id.transform = nph_render_transform_from_trs(
            nph_dvec3_from_vec3(nph_vec3_scale(nph_vec3_add(lo, hi), 0.5F)), nph_quat_identity(),
            nph_vec3_sub(hi, lo));
        NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &walls[i]), NPH_RESULT_OK);
    }
}

// A unit cube no ray stops at, for the blended tests' sheets: a hair in front of a wall, as the
// palace's decals are, so that the cells they read are the wall's, which the rays find (a sheet
// the rays met would hide the room from them).
static NphRenderMesh sheet_mesh(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    NphRenderSubmesh sub;
    NphRenderMeshDesc desc;
    nph_render_mesh_data_desc(&data, "the sheet", &sub, &desc);
    desc.no_rays = 1;
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &desc, &mesh), NPH_RESULT_OK);
    nph_render_mesh_data_free(&data);
    return mesh;
}

// Which pixels show box 1 of `scene` (the sheet of the blended tests), and how many.
static u32 sheet_mask(TestBoxScene *scene, bool *on_sheet)
{
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        on_sheet[i] =
            test_box_hit(scene, scene->eye, test_box_pixel_ray(scene, i), 0.0F, &t, &n) == 1;
        count += on_sheet[i] ? 1U : 0U;
    }
    return count;
}

// Before its cell has filled, a smooth surface in a room with no way out does not shine with a
// sky it cannot see: the sky along its direction is no brighter than what its irradiance cell has
// gathered.
NTEST_F(Reflect, a_new_cell_does_not_show_a_hidden_sky)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 1.0F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial metal = surface(f, nph_vec3(0.9F, 0.9F, 0.9F), 1.0F, 0.05F);
    const NphRenderMaterial dark = surface(f, nph_vec3(0.1F, 0.1F, 0.1F), 0.0F, 1.0F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_add(f, &scene, mesh, metal, nph_vec3(-3.0F, 0.0F, -3.0F), nph_vec3(3.0F, 3.0F, -2.0F));
    NphRenderInstance walls[5];
    close_room(f, mesh, dark, walls);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 2.0F), nph_vec3(0.0F, 1.5F, -2.0F), 0.6F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 off[PIXELS];
    static u8 first[PIXELS];
    static bool metal_pixels[SIZE * SIZE];
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        f32 t = 0.0F;
        NphVec3 n;
        metal_pixels[i] =
            test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) == 0;
        count += metal_pixels[i] ? 1U : 0U;
    }
    NTEST_ASSERT_GT_UINT(count, 1000U);
    NphRenderView v;
    // The irradiance cells fill, the reflection cells have had no ray.
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_OFF);
    (void)settled(f, &v, target, off, 24U);
    v.reflections = NPH_RENDER_REFLECTIONS_CACHE;
    (void)settled(f, &v, target, first, 1U);
    const f64 m_off = mean_of(off, metal_pixels);
    const f64 m_first = mean_of(first, metal_pixels);
    NTEST_LOG("the first frame with reflections: %.4f, with the sky's specular %.4f", m_first,
              m_off);
    NTEST_EXPECT_TRUE(m_off > 0.2);
    NTEST_EXPECT_TRUE(m_first < 0.25 * m_off);
    for (u32 i = 0; i < 5U; ++i) {
        nph_render_instance_destroy(f->renderer, walls[i]);
    }
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// A moving camera leaves new directions behind it every frame, and their cells a table of their
// own: however many, they cannot crowd out the irradiance's, which show as blotches of sky where
// they do.
NTEST_F(Reflect, a_moving_camera_leaves_the_irradiance_cells_alone)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, surface(f, nph_vec3(0.9F, 0.9F, 0.9F), 1.0F, 0.05F));
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    RenderGi *gi = &f->renderer->gi;
    u32 taken[2] = {0U, 0U};
    for (u32 pass = 0; pass < 2U; ++pass) {
        for (u32 i = 0; i < 64U; ++i) {
            const f32 a = 0.1F * (f32)i;
            const NphRenderCamera camera = test_box_camera(
                &scene, nph_vec3(3.0F * nph_f32_sin(a), 1.5F, 3.0F * nph_f32_cos(a)),
                nph_vec3_zero(), 0.8F);
            NphRenderView v;
            reflect_view(&v, camera,
                         pass == 0U ? NPH_RENDER_REFLECTIONS_OFF : NPH_RENDER_REFLECTIONS_CACHE);
            (void)test_render_frame(f, &v, target);
        }
        taken[pass] = slots_taken(f, gi->keys, gi->capacity);
    }
    const u32 reflecting = slots_taken(f, gi->reflect_keys, gi->reflect_capacity);
    NTEST_LOG("irradiance cells %u, then %u over the same walk; reflection cells %u", taken[0],
              taken[1], reflecting);
    NTEST_ASSERT_GT_UINT(taken[0], 0U);
    NTEST_ASSERT_GT_UINT(reflecting, 0U);
    NTEST_EXPECT_TRUE(taken[1] <= taken[0] + taken[0] / 10U);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// A voxel is rough, and reflects as a rough triangle does: what its irradiance cell has gathered,
// not the sky, in either mode. Under an even sky the needles hide some of it from each other, so
// metal voxels, whose light is all specular, come out darker than with the sky's specular, and far
// from black.
NTEST_F(Reflect, voxels_reflect_their_cells_not_the_sky)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    LodMesh m;
    lod_foliage_metal(&m, 1500U, 4.0F, 17U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderMaterial grey = surface(f, nph_vec3(0.5F, 0.5F, 0.5F), 1.0F, 0.7F);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &grey;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 0.0, 0.0), nph_quat_identity(),
                                                 nph_vec3(1.0F, 1.0F, 1.0F));
    NphRenderInstance instance = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &instance), NPH_RESULT_OK);
    const NphRenderCamera camera = nph_render_camera_look_at(
        nph_dvec3(-3.0, 1.0, -8.0), nph_dvec3(0.0, 0.0, 0.0), nph_vec3_unit_y(), 0.8F, 0.05F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_OFF);
    v.lod_error = 1.0F;
    const NphRenderFrameStats s = settled(f, &v, target, off, SETTLE);
    NTEST_EXPECT_GT_UINT(s.vis_voxel_clusters, 0U);
    v.reflections = NPH_RENDER_REFLECTIONS_CACHE;
    (void)settled(f, &v, target, on, SETTLE);
    static u8 rays[PIXELS];
    v.reflections = NPH_RENDER_REFLECTIONS_RAYS;
    (void)settled(f, &v, target, rays, SETTLE);
    static bool shown[SIZE * SIZE];
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        shown[i] = off[(u64)i * 4U] != 0U;
        count += shown[i] ? 1U : 0U;
    }
    const f64 m_off = mean_of(off, shown);
    const f64 m_on = mean_of(on, shown);
    const f64 m_rays = mean_of(rays, shown);
    NTEST_LOG("%u voxels' pixels: without %.4f, with cells %.4f, with rays %.4f", count, m_off,
              m_on, m_rays);
    NTEST_ASSERT_GT_UINT(count, 200U);
    // (Needles hide each other from the sky: what the sky's specular does not see, the cells do.
    // All of a metal's light is its specular, so it shows all of that, and far from all is hidden.)
    NTEST_EXPECT_TRUE(m_on <= m_off * 0.9);
    NTEST_EXPECT_TRUE(m_on >= m_off * 0.35);
    // Nothing here is smooth, so the rays' mode traces nothing and shades as the cells' does.
    NTEST_EXPECT_TRUE(m_rays <= m_on * 1.02);
    NTEST_EXPECT_TRUE(m_rays >= m_on * 0.98);
    test_box_release(f, target);
    nph_render_instance_destroy(f->renderer, instance);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
    nph_render_texture_destroy(f->renderer, env);
}

// A blended surface has no cell of its own to reflect (no ray starts from it): smooth, it shows
// the sky along its direction, no brighter than what the cell around it has gathered, so that in a
// room with no way out it does not shine with a sky it cannot see.
NTEST_F(Reflect, a_blended_mirror_does_not_show_a_hidden_sky)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 1.0F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial dark = surface(f, nph_vec3(0.1F, 0.1F, 0.1F), 0.0F, 1.0F);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.9F, 0.9F, 0.9F, 0.5F);
    md.metallic = 1.0F;
    md.roughness = 0.05F;
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    NphRenderMaterial sheet = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &sheet), NPH_RESULT_OK);
    // A closed room, a wall behind the sheet, whose cell it shares (a sheet in the air has none).
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_add(f, &scene, mesh, dark, nph_vec3(-3.0F, 0.0F, -3.0F), nph_vec3(3.0F, 3.0F, -2.0F));
    test_box_add(f, &scene, sheet_mesh(f), sheet, nph_vec3(-1.0F, 0.5F, -2.0F),
                 nph_vec3(1.0F, 2.5F, -1.98F));
    NphRenderInstance walls[5];
    close_room(f, mesh, dark, walls);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 2.0F), nph_vec3(0.0F, 1.5F, -2.0F), 0.6F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    static bool on_sheet[SIZE * SIZE];
    NTEST_ASSERT_GT_UINT(sheet_mask(&scene, on_sheet), 200U);
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_OFF);
    (void)settled(f, &v, target, off, SETTLE);
    v.reflections = NPH_RENDER_REFLECTIONS_CACHE;
    (void)settled(f, &v, target, on, SETTLE);
    const f64 m_off = mean_of(off, on_sheet);
    const f64 m_on = mean_of(on, on_sheet);
    NTEST_LOG("on the sheet: without %.4f, with %.4f", m_off, m_on);
    NTEST_EXPECT_TRUE(m_off > 0.05);
    NTEST_EXPECT_TRUE(m_on < 0.8 * m_off);
    for (u32 i = 0; i < 5U; ++i) {
        nph_render_instance_destroy(f->renderer, walls[i]);
    }
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// With no sky, a blended mirror shows what is gathered around it, as reflect.comp falls back on for
// a solid one: in a closed room lit by a glowing wall behind the camera, light it shows next to
// nothing of without reflections, and less than a solid mirror in its place, which sees the glow
// itself. (The cells' rays see the sun, emission and their bounces: a glow, as no point light.)
NTEST_F(Reflect, a_blended_mirror_with_no_sky_shows_what_is_gathered)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial white = surface(f, nph_vec3(0.6F, 0.6F, 0.6F), 0.0F, 1.0F);
    const NphRenderMaterial solid = surface(f, nph_vec3(0.9F, 0.9F, 0.9F), 1.0F, 0.05F);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.9F, 0.9F, 0.9F, 0.95F);
    md.metallic = 1.0F;
    md.roughness = 0.05F;
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    NphRenderMaterial sheet = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &sheet), NPH_RESULT_OK);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_add(f, &scene, mesh, white, nph_vec3(-3.0F, 0.0F, -3.0F), nph_vec3(3.0F, 3.0F, -2.0F));
    const NphVec3 lo = nph_vec3(-1.0F, 0.5F, -2.0F);
    const NphVec3 hi = nph_vec3(1.0F, 2.5F, -1.98F);
    test_box_add(f, &scene, sheet_mesh(f), sheet, lo, hi);
    // A solid mirror in the sheet's place, shown in its turn.
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &solid;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(
        nph_dvec3_from_vec3(nph_vec3_scale(nph_vec3_add(lo, hi), 0.5F)), nph_quat_identity(),
        nph_vec3_sub(hi, lo));
    NphRenderInstance mirror = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &mirror), NPH_RESULT_OK);
    nph_render_instance_set_hidden(f->renderer, mirror, true);
    NphRenderInstance walls[5];
    close_room(f, mesh, white, walls);
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.0F, 0.0F, 0.0F, 1.0F);
    md.emissive = nph_vec3(0.8F, 0.8F, 0.8F);
    NphRenderMaterial glow = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &glow), NPH_RESULT_OK);
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &glow;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 1.5, 2.975), nph_quat_identity(),
                                                 nph_vec3(6.0F, 3.0F, 0.05F));
    NphRenderInstance panel = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &panel), NPH_RESULT_OK);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 2.0F), nph_vec3(0.0F, 1.5F, -2.0F), 0.6F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    static u8 solid_on[PIXELS];
    static bool on_sheet[SIZE * SIZE];
    NTEST_ASSERT_GT_UINT(sheet_mask(&scene, on_sheet), 200U);
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_OFF);
    v.disable_ibl = 1;
    (void)settled(f, &v, target, off, SETTLE);
    v.reflections = NPH_RENDER_REFLECTIONS_CACHE;
    (void)settled(f, &v, target, on, SETTLE);
    nph_render_instance_set_hidden(f->renderer, scene.instances[1], true);
    nph_render_instance_set_hidden(f->renderer, mirror, false);
    (void)settled(f, &v, target, solid_on, SETTLE);
    const f64 m_off = mean_of(off, on_sheet);
    const f64 m_on = mean_of(on, on_sheet);
    const f64 m_solid = mean_of(solid_on, on_sheet);
    NTEST_LOG("on the sheet: without %.4f, with %.4f; a solid mirror there %.4f", m_off, m_on,
              m_solid);
    NTEST_EXPECT_TRUE(m_solid > 0.2);
    NTEST_EXPECT_TRUE(m_on > 3.0 * m_off);
    NTEST_EXPECT_TRUE(m_on < m_solid);
    nph_render_instance_destroy(f->renderer, panel);
    nph_render_instance_destroy(f->renderer, mirror);
    for (u32 i = 0; i < 5U; ++i) {
        nph_render_instance_destroy(f->renderer, walls[i]);
    }
    test_box_release(f, target);
}

// A renderer whose reflection cells' table is one bucket.
typedef RenderFixture ReflectFull;

NTEST_SETUP(ReflectFull)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.gi_cells = (u32)NPH_RENDER_GI_BUCKET; // and as many for reflections: one bucket
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(ReflectFull)
{
    test_render_teardown(f);
}

// A table too small for what a view reflects drops the rays of the cells it cannot take, and the
// frame's statistics count them: every ray but those of the cells its bucket holds.
NTEST_F(ReflectFull, a_full_table_counts_the_rays_it_drops)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, surface(f, nph_vec3(0.95F, 0.95F, 0.95F), 1.0F, 0.05F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_CACHE);
    u64 dropped = 0U;
    for (u32 i = 0; i < 64U; ++i) {
        dropped += test_render_frame(f, &v, target).vis_reflect_dropped;
    }
    NTEST_EXPECT_EQ_UINT(f->renderer->gi.reflect_capacity, (u32)NPH_RENDER_GI_BUCKET);
    // A mirror's every pixel traces once in 32 frames, a cell or so each: thousands of cells.
    NTEST_LOG("%llu rays dropped over 64 frames", (unsigned long long)dropped);
    NTEST_EXPECT_TRUE(dropped > 1000U);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// The passes come with what they serve: nothing smooth, no tile listed and no ray; a view without
// the radiance cache, or with them off, or a debug view, no passes at all.
NTEST_F(Reflect, the_cost_follows_the_smooth_pixels)
{
    REQUIRE_REFLECTIONS(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    static TestBoxScene scene;
    static TestBoxScene mirror_scene;
    memset(&scene, 0, sizeof scene);
    memset(&mirror_scene, 0, sizeof mirror_scene);
    test_box_ground(f, &scene, mesh, surface(f, nph_vec3(0.5F, 0.5F, 0.5F), 0.0F, 1.0F));
    test_box_ground(f, &mirror_scene, mesh, surface(f, nph_vec3(0.9F, 0.9F, 0.9F), 1.0F, 0.05F));
    test_box_show(f, &mirror_scene, false);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 3.0F, 3.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    NphRenderView v;
    reflect_view(&v, camera, NPH_RENDER_REFLECTIONS_CACHE);
    (void)test_render_frame(f, &v, target);
    NTEST_EXPECT_TRUE(f->renderer->gi.frame_reflect != 0);
    u32 args[4];
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, f->renderer->gi.reflect_args, 0U, sizeof args, args);
    NTEST_LOG("all rough: %u tiles listed", args[3]);
    NTEST_EXPECT_EQ_UINT(args[3], 0U);
    NTEST_EXPECT_EQ_UINT(args[0], 0U);
    test_box_show(f, &scene, false);
    test_box_show(f, &mirror_scene, true);
    (void)test_render_frame(f, &v, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, f->renderer->gi.reflect_args, 0U, sizeof args, args);
    NTEST_LOG("all smooth: %u tiles listed, %u workgroups", args[3], args[0]);
    NTEST_EXPECT_GT_UINT(args[3], 100U);
    NTEST_EXPECT_EQ_UINT(args[0], args[3]);
    test_box_show(f, &mirror_scene, false);
    test_box_show(f, &scene, true);
    v.reflections = NPH_RENDER_REFLECTIONS_OFF;
    (void)test_render_frame(f, &v, target);
    NTEST_EXPECT_FALSE(f->renderer->gi.frame_reflect != 0);
    v.reflections = NPH_RENDER_REFLECTIONS_CACHE;
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    (void)test_render_frame(f, &v, target);
    NTEST_EXPECT_FALSE(f->renderer->gi.frame_reflect != 0);
    v.debug_view = NPH_RENDER_VIEW_LIT;
    v.gi = 0;
    (void)test_render_frame(f, &v, target);
    NTEST_EXPECT_FALSE(f->renderer->gi.frame_active != 0);
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// The mode is one of the three.
NTEST_F(Reflect, the_mode_must_be_known)
{
    REQUIRE_RENDERER(f);
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    NTEST_EXPECT_EQ_INT((int)v.reflections, (int)NPH_RENDER_REFLECTIONS_RAYS);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    v.reflections = (NphRenderReflections)3;
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    nph_gpu_rg_begin(f->graph);
    NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
}
