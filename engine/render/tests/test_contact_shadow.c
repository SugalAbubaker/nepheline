// SPDX-License-Identifier: Apache-2.0
// Contact shadows (render/renderer.h NphRenderView.contact_shadow; docs/render.md "Contact
// shadows"; ADR-0076; shaders/render/ao.comp sun_blocked): each pixel's march towards the sun
// through the depths, against a ground truth the test computes itself, each pixel's surface found
// by casting its ray at the scene's boxes, and from there a ray at the sun.
#include "render_internal.h"
#include "test_boxes.h"
#include "test_lod_support.h"

#include <math.h>

typedef RenderFixture Contact;

NTEST_SETUP(Contact)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Contact)
{
    test_render_teardown(f);
}

#define REQUIRE_CONTACT(f)                                                                         \
    do {                                                                                           \
        REQUIRE_PRODUCTION(f);                                                                     \
        if (!nph_renderer_virtual_shadows_supported((f)->renderer)) {                              \
            NTEST_SKIP("no virtual shadow map on this device");                                    \
        }                                                                                          \
    } while (0)

#define SIZE   TEST_BOX_SIZE
#define PIXELS TEST_BOX_PIXELS
// The march, in pixels' widths: NphRenderView's default.
#define MARCH 24.0F
// A surface with its sun and with the march's result dark enough to tell from it: the sun gives
// more than this share of what a pixel shows.
#define LIT_SHARE 0.5F

// The sun, low so that a little height casts a long shadow, travelling along `x` and `z`.
static NphRenderLightDesc sun_from(f32 x, f32 z)
{
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 8.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(x, -0.3F, z));
    ld.shadows = 1;
    return ld;
}

static NphRenderLightDesc low_sun(void)
{
    return sun_from(-0.8F, -0.6F);
}

static NphRenderLight make_sun(RenderFixture *f, const NphRenderLightDesc *ld)
{
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, ld, &sun), NPH_RESULT_OK);
    return sun;
}

// The production view of the boxes, with the occlusion it needs, without a temporal resolve to
// turn its slices or move its pixels: the same frame twice is the same image.
static void contact_view(NphRenderView *v, NphRenderCamera camera, NphRenderDebugView debug,
                         f32 march)
{
    test_box_view(v, camera, debug);
    v->ao_radius = 0.5F;
    v->anti_aliasing = NPH_RENDER_AA_NONE;
    v->contact_shadow = march;
}

// A frame's image, after the frame that settles what the production path culls and the frames the
// map's pages take to be drawn.
static NphRenderFrameStats settled(RenderFixture *f, const NphRenderView *v, NphGpuImage target,
                                   u8 *out)
{
    NphRenderFrameStats s = test_box_render(f, v, target, out);
    for (u32 i = 0; i < 3U; ++i) {
        s = test_box_render(f, v, target, out);
    }
    return s;
}

// What a pixel sees: -1 nothing, 0 a surface facing the sun that it reaches, 1 one facing it that
// a box hides from it, 2 one facing away.
static i32 truth_at(const TestBoxScene *s, NphVec3 to_sun, u32 i)
{
    const NphVec3 d = test_box_pixel_ray(s, i);
    f32 t = 0.0F;
    NphVec3 n;
    if (test_box_hit(s, s->eye, d, 0.0F, &t, &n) < 0) {
        return -1;
    }
    if (nph_vec3_dot(n, to_sun) <= 0.0F) {
        return 2;
    }
    const NphVec3 x = nph_vec3_add(s->eye, nph_vec3_scale(d, t));
    const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
    f32 hit = 0.0F;
    NphVec3 hn;
    return test_box_hit(s, o, to_sun, 0.0F, &hit, &hn) >= 0 ? 1 : 0;
}

// Whether the pixel's four neighbours are what it is: only those are sure of a pixel's truth, a
// pixel's centre being all the march and the map are asked about.
static bool truth_settled(const TestBoxScene *s, NphVec3 to_sun, u32 i, i32 truth)
{
    const u32 x = i % SIZE;
    const u32 y = i / SIZE;
    if (x == 0U || y == 0U || x + 1U == SIZE || y + 1U == SIZE) {
        return false;
    }
    return truth_at(s, to_sun, i - 1U) == truth && truth_at(s, to_sun, i + 1U) == truth &&
           truth_at(s, to_sun, i - SIZE) == truth && truth_at(s, to_sun, i + SIZE) == truth;
}

typedef struct Against {
    u32 lit;      // pixels the sun reaches
    u32 hidden;   // pixels it does not, of those facing it
    u32 missed;   // of the hidden ones, lit all the same
    u32 darkened; // of the lit ones, dark all the same
} Against;

// An image of lit surfaces against the truth: a pixel counts as lit above `level`, in linear
// light.
static Against against_truth(const TestBoxScene *s, NphVec3 to_sun, const u8 *image, f32 level)
{
    Against a;
    memset(&a, 0, sizeof a);
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const i32 truth = truth_at(s, to_sun, i);
        if ((truth != 0 && truth != 1) || !truth_settled(s, to_sun, i, truth)) {
            continue;
        }
        const bool lit = test_box_linear(image, i) > level;
        if (truth == 0) {
            ++a.lit;
            a.darkened += lit ? 0U : 1U;
        } else {
            ++a.hidden;
            a.missed += lit ? 1U : 0U;
        }
    }
    return a;
}

// A bar 1.5 cm tall on the ground, about two pixels' widths here, too low for the map: the
// receiver's offset along its normal, of 1.5 of its texels, lifts the ground to about the bar's
// top, at the level the sun's slope on the ground gives (ADR-0080). The march sees what the map
// cannot, and darkens nothing else.
NTEST_F(Contact, it_shows_what_the_map_lifts_the_ground_above)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, grey);
    test_box_add(f, &scene, mesh, grey, nph_vec3(-0.35F, 0.0F, -0.2F),
                 nph_vec3(0.35F, 0.015F, 0.2F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(-0.3F, 0.9F, -0.8F), nph_vec3(-0.1F, 0.0F, -0.1F), 0.8F);
    const NphRenderLightDesc ld = low_sun();
    const NphRenderLight sun = make_sun(f, &ld);
    const NphVec3 to_sun = nph_vec3_neg(ld.direction);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    NphRenderFrameStats s = settled(f, &v, target, without);
    NTEST_EXPECT_TRUE(s.ao);
    NTEST_EXPECT_TRUE(s.vsm);
    v.contact_shadow = MARCH;
    (void)settled(f, &v, target, with);

    // The sun's share of a lit ground pixel: the level half way down to the shadow.
    const f32 ground = test_box_linear(without, (SIZE / 8U) * SIZE + SIZE / 8U);
    const Against off = against_truth(&scene, to_sun, without, LIT_SHARE * ground);
    const Against on = against_truth(&scene, to_sun, with, LIT_SHARE * ground);
    NTEST_LOG("ground %.3f. Without: %u lit (%u dark), %u hidden (%u lit). With: %u lit (%u dark), "
              "%u hidden (%u lit)",
              (f64)ground, off.lit, off.darkened, off.hidden, off.missed, on.lit, on.darkened,
              on.hidden, on.missed);
    NTEST_ASSERT_GT_UINT(off.hidden, 200U);
    NTEST_EXPECT_TRUE(off.missed > off.hidden / 2U);
    // The far end of a shadow is the march's least sure: a step must land in the bar's height.
    NTEST_EXPECT_TRUE(on.missed * 3U < off.missed * 2U);
    NTEST_EXPECT_TRUE(on.darkened <= off.darkened + on.lit / 1000U);

    // A march of a pixel or two reaches less of it.
    v.contact_shadow = 2.0F;
    (void)settled(f, &v, target, with);
    const Against shorter = against_truth(&scene, to_sun, with, LIT_SHARE * ground);
    NTEST_LOG("A march of 2: %u hidden (%u lit)", shorter.hidden, shorter.missed);
    NTEST_EXPECT_TRUE(shorter.missed > on.missed + on.missed / 2U);

    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// A plane seen at a grazing angle under a low sun, from every side: a marched point lies just
// above it, and the plane under a pixel's centre half a pixel away rises by far more than that. It
// does not shadow itself, out to its horizon, the march's own rounding allowed for, and however
// long a march is asked for: it is bounded, and stays in front of the camera.
NTEST_F(Contact, a_plane_does_not_shadow_itself_however_it_is_seen)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 0.3F, 8.0F), nph_vec3(1.0F, 0.0F, 0.0F), 0.8F);
    NphRenderLightDesc ld = low_sun();
    const NphRenderLight sun = make_sun(f, &ld);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    static const f32 k_ways[4][2] = {{-0.8F, -0.6F}, {0.8F, -0.6F}, {-0.8F, 0.6F}, {0.8F, 0.6F}};
    static const f32 k_marches[2] = {MARCH, 1e6F};
    for (u32 w = 0; w < 4U; ++w) {
        ld = sun_from(k_ways[w][0], k_ways[w][1]);
        NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, sun, &ld), NPH_RESULT_OK);
        NphRenderView v;
        contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
        (void)settled(f, &v, target, without);
        for (u32 m = 0; m < 2U; ++m) {
            v.contact_shadow = k_marches[m];
            (void)settled(f, &v, target, with);
            u32 ground = 0U;
            u32 changed = 0U;
            for (u32 i = 0; i < SIZE * SIZE; ++i) {
                f32 t = 0.0F;
                NphVec3 n;
                if (test_box_hit(&scene, scene.eye, test_box_pixel_ray(&scene, i), 0.0F, &t, &n) >=
                    0) {
                    ++ground;
                    changed += without[(u64)i * 4U] != with[(u64)i * 4U] ? 1U : 0U;
                }
            }
            NTEST_LOG("sun %u, march %.0f: %u ground pixels, %u changed", w, (f64)k_marches[m],
                      ground, changed);
            NTEST_ASSERT_GT_UINT(ground, SIZE * SIZE / 3U);
            NTEST_EXPECT_EQ_UINT(changed, 0U);
        }
    }
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// A box in the air: around its outline the ground lies far behind it, so a march that passes
// behind the box in the image is not behind anything that blocks the sun. Only what the map
// shadows is dark.
NTEST_F(Contact, what_stands_far_in_front_does_not_shadow)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, grey);
    test_box_add(f, &scene, mesh, grey, nph_vec3(-0.5F, 1.2F, -0.5F), nph_vec3(0.5F, 1.7F, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(2.0F, 3.5F, 3.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphRenderLightDesc ld = low_sun();
    const NphRenderLight sun = make_sun(f, &ld);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    (void)settled(f, &v, target, without);
    v.contact_shadow = MARCH;
    (void)settled(f, &v, target, with);
    u32 differ = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        differ += without[(u64)i * 4U] != with[(u64)i * 4U] ? 1U : 0U;
    }
    NTEST_LOG("%u pixels differ", differ);
    NTEST_EXPECT_EQ_UINT(differ, 0U);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// Where the sun grazes a wall the camera faces, the map's texels lie stretched across it, and a
// ledge's shadow down the wall steps along its edge: the map takes a level finer there, as fine in
// area on the wall as a pixel (ADR-0080). The map alone, against rays the test casts: few pixels
// lit that the ledge hides, or dark that the sun reaches.
NTEST_F(Contact, a_wall_the_sun_grazes_takes_a_finer_map)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, grey);
    test_box_add(f, &scene, mesh, grey, nph_vec3(-3.0F, 0.0F, -0.3F), nph_vec3(3.0F, 3.0F, 0.0F));
    test_box_add(f, &scene, mesh, grey, nph_vec3(-1.5F, 2.0F, 0.0F), nph_vec3(1.5F, 2.08F, 0.25F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 1.5F, 0.0F), 0.8F);
    // Travelling along the wall and down it: 0.15 of the sun's light on the wall.
    NphRenderLightDesc ld = low_sun();
    ld.direction = nph_vec3_normalize(nph_vec3(0.85F, -0.5F, -0.15F));
    const NphRenderLight sun = make_sun(f, &ld);
    const NphVec3 to_sun = nph_vec3_neg(ld.direction);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    const NphRenderFrameStats s = settled(f, &v, target, image);
    NTEST_EXPECT_TRUE(s.vsm);
    // The sun's share of a lit wall pixel: the level half way down to the shadow.
    const f32 wall = test_box_linear(image, (SIZE / 4U) * SIZE + SIZE / 2U);
    const Against a = against_truth(&scene, to_sun, image, LIT_SHARE * wall);
    NTEST_LOG("wall %.3f: %u lit (%u dark), %u hidden (%u lit)", (f64)wall, a.lit, a.darkened,
              a.hidden, a.missed);
    NTEST_ASSERT_GT_UINT(a.hidden, 200U);
    NTEST_ASSERT_GT_UINT(a.lit, 2000U);
    // The distance's level alone missed 13.7 % of the hidden pixels and darkened 29 lit ones; the
    // slope's, 6.0 % and none.
    NTEST_EXPECT_TRUE(a.missed * 100U < a.hidden * 9U);
    NTEST_EXPECT_TRUE(a.darkened <= a.lit / 1000U);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// Where the sun grazes the wall harder still (4.6 degrees: 0.08 of its light), a texel of the
// map lies six pixels long down the wall even a level finer, and the ledge's shadow is a row of
// steps with light between them that every level steps elsewhere. In a frame that has the rays'
// scene (here for a lamp too dim to see, whose shadows are rays') the occlusion's pass traces
// the sun for such a pixel and the resolve leaves the map unread there (ao_rt.comp; ADR-0094):
// against rays the test casts, next to nothing lit that the ledge hides or dark that the sun
// reaches, where the map alone, in the same view without the lamp, leaves many times as much.
NTEST_F(Contact, a_wall_the_sun_grazes_hard_takes_the_suns_rays)
{
    REQUIRE_CONTACT(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, grey);
    test_box_add(f, &scene, mesh, grey, nph_vec3(-3.0F, 0.0F, -0.3F), nph_vec3(3.0F, 3.0F, 0.0F));
    test_box_add(f, &scene, mesh, grey, nph_vec3(-1.5F, 2.0F, 0.0F), nph_vec3(1.5F, 2.08F, 0.25F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 1.5F, 0.0F), 0.8F);
    NphRenderLightDesc ld = low_sun();
    ld.direction = nph_vec3_normalize(nph_vec3(0.85F, -0.5F, -0.08F));
    const NphRenderLight sun = make_sun(f, &ld);
    const NphVec3 to_sun = nph_vec3_neg(ld.direction);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    // (And a third time with the rays' scene but the view not asking for the sun's rays,
    // as views do by default: the map alone again.)
    Against found[3];
    NphRenderLight lamp = NPH_RENDER_LIGHT_NULL;
    for (u32 rays = 0; rays < 3U; ++rays) {
        if (rays == 1U) {
            NphRenderLightDesc pd;
            memset(&pd, 0, sizeof pd);
            pd.type = NPH_RENDER_LIGHT_TYPE_POINT;
            pd.color = nph_vec3_one();
            pd.intensity = 1e-4F;
            pd.position = nph_dvec3(0.0, 2.5, 3.0);
            pd.shadows = 1;
            NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &pd, &lamp), NPH_RESULT_OK);
        }
        NphRenderView v;
        contact_view(&v, camera, NPH_RENDER_VIEW_LIT, MARCH);
        v.sun_rays = rays == 1U ? 1 : 0;
        const NphRenderFrameStats s = settled(f, &v, target, image);
        NTEST_EXPECT_TRUE(s.vsm);
        NTEST_EXPECT_EQ_INT(s.shadow_rays ? 1 : 0, rays != 0U ? 1 : 0);
        const f32 wall = test_box_linear(image, (SIZE / 4U) * SIZE + SIZE / 2U);
        found[rays] = against_truth(&scene, to_sun, image, LIT_SHARE * wall);
        NTEST_LOG("%s, wall %.3f: %u lit (%u dark), %u hidden (%u lit)",
                  rays == 0U   ? "the map alone"
                  : rays == 1U ? "with the rays' scene and the sun's rays asked for"
                               : "with the rays' scene, the sun left to its map",
                  (f64)wall, found[rays].lit, found[rays].darkened, found[rays].hidden,
                  found[rays].missed);
        NTEST_ASSERT_GT_UINT(found[rays].hidden, 200U);
        NTEST_ASSERT_GT_UINT(found[rays].lit, 2000U);
    }
    NTEST_EXPECT_TRUE(found[1].missed * 100U <= found[1].hidden * 2U);
    NTEST_EXPECT_TRUE(found[1].darkened * 200U <= found[1].lit);
    NTEST_EXPECT_TRUE((found[1].missed + found[1].darkened) * 3U <
                      found[0].missed + found[0].darkened);
    // Left to its map, the sun's shadows are the map's again (the lamp's own light is nothing).
    NTEST_EXPECT_EQ_UINT(found[2].missed, found[0].missed);
    NTEST_EXPECT_EQ_UINT(found[2].darkened, found[0].darkened);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, lamp);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// A cloth in folds five centimetres deep, `cells` x `cells` quads across [-half, half] in x and
// z, with its levels of detail: the coarser ones smooth the folds away.
static void folded_cloth(LodMesh *m, u32 cells, f32 half)
{
    NphAllocator *a = nph_allocator_system();
    const u32 side = cells + 1U;
    const u32 n = side * side;
    NphRenderVertexInput *v = NPH_ALLOC_ARRAY(a, NphRenderVertexInput, n);
    u32 *idx = NPH_ALLOC_ARRAY(a, u32, (u64)cells * cells * 6U);
    NTEST_ASSERT_TRUE(v != NULL && idx != NULL);
    const f32 step = 2.0F * half / (f32)cells;
    const f32 depth = 0.05F;
    const f32 turn = 2.0F * NPH_PI / 0.6F; // a fold every 60 cm
    for (u32 i = 0; i < side; ++i) {
        for (u32 j = 0; j < side; ++j) {
            const f32 x = -half + step * (f32)j;
            const f32 z = -half + step * (f32)i;
            NphRenderVertexInput *q = &v[i * side + j];
            q->position = nph_vec3(x, depth * nph_f32_sin(turn * x), z);
            q->normal =
                nph_vec3_normalize(nph_vec3(-depth * turn * nph_f32_cos(turn * x), 1.0F, 0.0F));
            q->uv = nph_vec2((f32)j / (f32)cells, (f32)i / (f32)cells);
            q->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
        }
    }
    u32 k = 0U;
    for (u32 i = 0; i < cells; ++i) {
        for (u32 j = 0; j < cells; ++j) {
            const u32 v00 = i * side + j;
            // Counter-clockwise seen from above (+y).
            idx[k++] = v00;
            idx[k++] = v00 + side;
            idx[k++] = v00 + side + 1U;
            idx[k++] = v00;
            idx[k++] = v00 + side + 1U;
            idx[k++] = v00 + 1U;
        }
    }
    lod_mesh_build(m, v, n, idx, k);
    NPH_FREE_ARRAY(a, NphRenderVertexInput, v, n);
    NPH_FREE_ARRAY(a, u32, idx, (u64)cells * cells * 6U);
}

// A view far enough off draws a coarser level of a mesh, smoother than the mesh, and the rays'
// scene holds the mesh itself. A ray that left the level's surface where the mesh lies above it
// met the mesh: on Bistro's awnings, patches of shadow under folds the level had smoothed away,
// which came and went as the level changed with the camera's distance. A pixel that shows a
// coarser level (NPH_VIS_CLUSTER_COARSE) finds the mesh along its own ray first, and its ray to
// the sun starts there (ao_pass.glsl; ADR-0094): a cloth in folds, under a sun high enough to
// reach all of every fold, is lit at a coarser level wherever it is at its own.
NTEST_F(Contact, a_coarser_level_takes_no_shadow_of_its_own_mesh)
{
    REQUIRE_CONTACT(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    LodMesh m;
    folded_cloth(&m, 96U, 2.0F);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &grey;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 0.0, 0.0), nph_quat_identity(),
                                                 nph_vec3(1.0F, 1.0F, 1.0F));
    NphRenderInstance instance = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &instance), NPH_RESULT_OK);
    // The sun 17 degrees off the cloth's normal, its folds' slopes at most 28: no fold shades
    // another. The rays' scene is in the frame for a lamp too dim to see.
    NphRenderLightDesc ld = low_sun();
    ld.direction = nph_vec3_normalize(nph_vec3(0.25F, -1.0F, 0.15F));
    const NphRenderLight sun = make_sun(f, &ld);
    NphRenderLightDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.type = NPH_RENDER_LIGHT_TYPE_POINT;
    pd.color = nph_vec3_one();
    pd.intensity = 1e-4F;
    pd.position = nph_dvec3(0.0, 2.5, 3.0);
    pd.shadows = 1;
    NphRenderLight lamp = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &pd, &lamp), NPH_RESULT_OK);
    const NphRenderCamera camera = nph_render_camera_look_at(
        nph_dvec3(0.0, 3.5, 4.5), nph_dvec3(0.0, 0.0, 0.0), nph_vec3_unit_y(), 0.8F, 0.05F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 fine[PIXELS];
    static u8 coarse[PIXELS];
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, MARCH);
    v.sun_rays = 1;
    v.lod_error = 0.0F;
    const NphRenderFrameStats s0 = settled(f, &v, target, fine);
    // Six pixels' error: 20 cm here, four times the folds' depth.
    v.lod_error = 6.0F;
    const NphRenderFrameStats s1 = settled(f, &v, target, coarse);
    NTEST_EXPECT_TRUE(s0.shadow_rays);
    NTEST_EXPECT_TRUE(s1.shadow_rays);
    NTEST_EXPECT_EQ_UINT(s0.vis_triangles, m.data.index_count / 3U);
    NTEST_EXPECT_LT_UINT(s1.vis_triangles * 4U, s0.vis_triangles);
    // The cloth's pixels, by the light its own level shows, and those of them a coarser level
    // shows much darker.
    const f32 centre = test_box_linear(fine, (SIZE / 2U) * SIZE + SIZE / 2U);
    u32 cloth = 0U;
    u32 darker = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const f32 own = test_box_linear(fine, i);
        if (own > 0.5F * centre) {
            ++cloth;
            darker += test_box_linear(coarse, i) < 0.6F * own ? 1U : 0U;
        }
    }
    NTEST_LOG("%u triangles of %u drawn; of the cloth's %u lit pixels, %u much darker at the "
              "coarser level",
              s1.vis_triangles, s0.vis_triangles, cloth, darker);
    NTEST_ASSERT_GT_UINT(cloth, 4000U);
    NTEST_EXPECT_LE_UINT(darker * 100U, cloth);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, lamp);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_instance_destroy(f->renderer, instance);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
    nph_render_texture_destroy(f->renderer, env);
}

// Level ground 40 m across with its top at y = 0, as test_box_ground's, of a mesh whose own space
// lies far from where its instance stands: its vertices `far` metres out on every axis, and the
// instance as far back (a city's tile, modelled where it lies in its city).
static void far_ground(RenderFixture *f, LodMesh *m, f32 far, NphRenderMaterial mat,
                       NphRenderMesh *mesh, NphRenderInstance *instance)
{
    enum {
        CELLS = 8,
        SIDE = CELLS + 1
    };
    static NphRenderVertexInput v[SIDE * SIDE];
    static u32 idx[CELLS * CELLS * 6];
    for (u32 i = 0; i < (u32)SIDE; ++i) {
        for (u32 j = 0; j < (u32)SIDE; ++j) {
            NphRenderVertexInput *q = &v[i * (u32)SIDE + j];
            q->position = nph_vec3(far - 20.0F + 40.0F * (f32)j / (f32)CELLS, far,
                                   far - 20.0F + 40.0F * (f32)i / (f32)CELLS);
            q->normal = nph_vec3_unit_y();
            q->uv = nph_vec2((f32)j / (f32)CELLS, (f32)i / (f32)CELLS);
            q->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
        }
    }
    u32 k = 0U;
    for (u32 i = 0; i < (u32)CELLS; ++i) {
        for (u32 j = 0; j < (u32)CELLS; ++j) {
            const u32 v00 = i * (u32)SIDE + j;
            // Counter-clockwise seen from above (+y).
            idx[k++] = v00;
            idx[k++] = v00 + (u32)SIDE;
            idx[k++] = v00 + (u32)SIDE + 1U;
            idx[k++] = v00;
            idx[k++] = v00 + (u32)SIDE + 1U;
            idx[k++] = v00 + 1U;
        }
    }
    lod_mesh_build(m, v, (u32)(SIDE * SIDE), idx, k);
    const NphRenderMeshDesc d = level0_desc(m);
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &d, mesh), NPH_RESULT_OK);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = *mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(-(f64)far, -(f64)far, -(f64)far),
                                                 nph_quat_identity(), nph_vec3(1.0F, 1.0F, 1.0F));
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, instance), NPH_RESULT_OK);
}

// The rays' scene keeps its origin, the anchor, where the camera was when it was built, and a
// ray's own offset off its surface counts units in the last place of the point's coordinates
// there (rt_common.glsl nph_offset_ray): next to nothing for ground level with the anchor, while
// a pixel's point comes from its depth, only as exact as a float tens of metres from the camera
// is. The city's road, 30 to 50 m off with the anchor 16 m behind the camera, shadowed itself
// in patches that changed every frame; the pass now starts its rays off the surface by a share of
// the point's distance (ao_pass.glsl AO_RAY_SLACK). Here, the sun's rays from a camera that has
// left the anchor, over ground level with it, of a mesh modelled far from its own origin: open
// ground is lit, all of it, evenly. (This scene is too small to shadow itself without the slack;
// that is measured in the city, docs/adr/0094. A ray that left from the wrong place, the
// anchor's offset lost or turned, would start under this ground.)
NTEST_F(Contact, ground_level_with_the_rays_anchor_does_not_shadow_itself)
{
    REQUIRE_CONTACT(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    LodMesh m;
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NphRenderInstance instance = NPH_RENDER_INSTANCE_NULL;
    far_ground(f, &m, 2000.0F, grey, &mesh, &instance);
    // The ground truth's ground: the same slab's top.
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    scene.boxes[0].lo = nph_vec3(-20.0F, -1.0F, -20.0F);
    scene.boxes[0].hi = nph_vec3(20.0F, 0.0F, 20.0F);
    scene.count = 1U;
    const NphRenderLightDesc ld = low_sun();
    const NphRenderLight sun = make_sun(f, &ld);
    const NphVec3 to_sun = nph_vec3_neg(ld.direction);
    NphRenderLightDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.type = NPH_RENDER_LIGHT_TYPE_POINT;
    pd.color = nph_vec3_one();
    pd.intensity = 1e-4F;
    pd.position = nph_dvec3(0.0, 2.5, 3.0);
    pd.shadows = 1;
    NphRenderLight lamp = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &pd, &lamp), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    // The first frames that trace anchor the rays' scene at their camera: 4 cm above the ground.
    NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 0.04F, 15.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, MARCH);
    v.sun_rays = 1;
    NphRenderFrameStats s = settled(f, &v, target, image);
    NTEST_EXPECT_TRUE(s.shadow_rays);
    // Then the ground 15 to 35 m off, from 7 m away from the anchor: the scene is not rebuilt.
    camera =
        test_box_camera(&scene, nph_vec3(0.0F, 6.0F, 12.0F), nph_vec3(0.0F, 0.0F, -15.0F), 0.8F);
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, MARCH);
    v.sun_rays = 1;
    s = settled(f, &v, target, image);
    NTEST_EXPECT_TRUE(s.shadow_rays);
    f32 brightest = 0.0F;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        brightest = nph_f32_max(brightest, test_box_linear(image, i));
    }
    // Open ground under one sun is of one light: a pixel some of whose window's rays met the
    // ground itself shows less of it.
    const Against found = against_truth(&scene, to_sun, image, 0.9F * brightest);
    NTEST_LOG("ground at %.3f: %u lit pixels, %u of them under nine tenths of it", (f64)brightest,
              found.lit, found.darkened);
    NTEST_ASSERT_GT_UINT(found.lit, 4000U);
    NTEST_EXPECT_EQ_UINT(found.darkened, 0U);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, lamp);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_instance_destroy(f->renderer, instance);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
    nph_render_texture_destroy(f->renderer, env);
}

// A surface seen as aslant as the sun lights it keeps the distance's level: a pixel lies stretched
// across it as much as a texel does (ADR-0080). The ground, seen from a little above it under a sun
// a little higher, asks for no more pages than its distances alone would.
NTEST_F(Contact, the_ground_seen_as_aslant_as_lit_keeps_its_level)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, grey);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 0.6F, 6.0F), nph_vec3(0.0F, 0.0F, -4.0F), 0.8F);
    NphRenderLightDesc ld = low_sun();
    ld.direction = nph_vec3_normalize(nph_vec3(0.2F, -0.27F, -1.0F)); // 15 degrees up
    const NphRenderLight sun = make_sun(f, &ld);
    // Pages at the tests' 128 pixels are a few; at 512, enough to count.
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 512U);
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    for (u32 i = 0; i < 4U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    const NphRenderFrameStats s = test_render_frame(f, &v, target);
    NTEST_EXPECT_TRUE(s.vsm);
    NTEST_LOG("%u pages asked for", s.vsm_requested);
    // 37 on the RTX 3060 Ti: the near ground, seen more steeply than lit, takes a level finer, as
    // it should. Without the view's slope all of it did, 77; the distance's level alone asked
    // for 27.
    NTEST_EXPECT_GT_UINT(s.vsm_requested, 0U);
    NTEST_EXPECT_LE_UINT(s.vsm_requested, 50U);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// The march's answer travels in the signs of the occlusion's values: what the occlusion shows, and
// shades the indirect light by, is the same with it, though the march finds something: a bar beside
// the box too low for the map (as in it_shows_what_the_map_lifts_the_ground_above).
NTEST_F(Contact, the_occlusion_is_the_same_with_it)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    static TestBoxScene scene;
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, mesh, grey, grey);
    test_box_add(f, &scene, mesh, grey, nph_vec3(0.8F, 0.0F, 0.6F), nph_vec3(1.5F, 0.015F, 1.0F));
    const NphRenderLightDesc ld = low_sun();
    const NphRenderLight sun = make_sun(f, &ld);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    NphRenderView v;
    const NphRenderDebugView views[2] = {NPH_RENDER_VIEW_OCCLUSION, NPH_RENDER_VIEW_INDIRECT};
    for (u32 i = 0; i < 2U; ++i) {
        contact_view(&v, camera, views[i], 0.0F);
        (void)settled(f, &v, target, without);
        v.contact_shadow = MARCH;
        (void)settled(f, &v, target, with);
        NTEST_EXPECT_EQ_INT(memcmp(without, with, PIXELS), 0);
    }
    // And there was something to find: the lit view differs.
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    (void)settled(f, &v, target, without);
    v.contact_shadow = MARCH;
    (void)settled(f, &v, target, with);
    NTEST_EXPECT_NE_INT(memcmp(without, with, PIXELS), 0);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// Voxels read the occlusion and the march's answer by a resolve of their own: the same there.
NTEST_F(Contact, voxels_take_it_as_triangles_do)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 17U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &grey;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 0.0, 0.0), nph_quat_identity(),
                                                 nph_vec3(1.0F, 1.0F, 1.0F));
    NphRenderInstance instance = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &instance), NPH_RESULT_OK);
    const NphRenderLightDesc ld = low_sun();
    const NphRenderLight sun = make_sun(f, &ld);
    const NphRenderCamera camera = nph_render_camera_look_at(
        nph_dvec3(-3.0, 1.0, -8.0), nph_dvec3(0.0, 0.0, 0.0), nph_vec3_unit_y(), 0.8F, 0.05F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_OCCLUSION, 0.0F);
    v.lod_error = 1.0F;
    (void)settled(f, &v, target, without);
    v.contact_shadow = MARCH;
    const NphRenderFrameStats s = settled(f, &v, target, with);
    NTEST_EXPECT_GT_UINT(s.vis_voxel_clusters, 0U);
    NTEST_EXPECT_EQ_INT(memcmp(without, with, PIXELS), 0);

    v.debug_view = NPH_RENDER_VIEW_LIT;
    v.contact_shadow = 0.0F;
    (void)settled(f, &v, target, without);
    v.contact_shadow = MARCH;
    (void)settled(f, &v, target, with);
    u32 brighter = 0U;
    u32 darker = 0U;
    for (u64 i = 0; i < PIXELS; i += 4U) {
        brighter += with[i] > without[i] ? 1U : 0U;
        darker += with[i] + 4U < without[i] ? 1U : 0U;
    }
    NTEST_LOG("%u pixels darker by over 4, %u brighter", darker, brighter);
    NTEST_EXPECT_EQ_UINT(brighter, 0U);
    NTEST_EXPECT_GT_UINT(darker, 10U);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_instance_destroy(f->renderer, instance);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
    nph_render_texture_destroy(f->renderer, env);
}

// It refines the sun's virtual shadow map, and comes with the occlusion: with either gone, the
// march changes nothing.
NTEST_F(Contact, it_needs_the_map_and_the_occlusion)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    static TestBoxScene scene;
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, test_cube_mesh(f), grey, grey);
    NphRenderLightDesc ld = low_sun();
    ld.shadows = 0;
    const NphRenderLight sun = make_sun(f, &ld);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    NphRenderView v;
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    NphRenderFrameStats s = settled(f, &v, target, without);
    NTEST_EXPECT_TRUE(s.ao);
    NTEST_EXPECT_FALSE(s.vsm);
    v.contact_shadow = MARCH;
    (void)settled(f, &v, target, with);
    NTEST_EXPECT_EQ_INT(memcmp(without, with, PIXELS), 0);

    ld.shadows = 1;
    NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, sun, &ld), NPH_RESULT_OK);
    contact_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    v.ao_radius = 0.0F;
    s = settled(f, &v, target, without);
    NTEST_EXPECT_FALSE(s.ao);
    NTEST_EXPECT_TRUE(s.vsm);
    v.contact_shadow = MARCH;
    (void)settled(f, &v, target, with);
    NTEST_EXPECT_EQ_INT(memcmp(without, with, PIXELS), 0);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// The march is a length: finite and not negative.
NTEST_F(Contact, the_march_must_be_finite_and_not_negative)
{
    REQUIRE_RENDERER(f);
    const f32 bad[3] = {-1.0F, (f32)NAN, NPH_F32_INFINITY};
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    NTEST_EXPECT_TRUE(v.contact_shadow > 0.0F);
    for (u32 i = 0; i < 3U; ++i) {
        v.contact_shadow = bad[i];
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        nph_gpu_rg_begin(f->graph);
        NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                            NPH_RESULT_INVALID_ARGUMENT);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
}
