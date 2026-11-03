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

// A bar 2.5 cm tall on the ground, three pixels' widths here, too low for the map: the receiver's
// offset along its normal, of 1.5 of its texels, lifts the ground to about the bar's top. The march
// sees what the map cannot, and darkens nothing else.
NTEST_F(Contact, it_shows_what_the_map_lifts_the_ground_above)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, mesh, grey);
    test_box_add(f, &scene, mesh, grey, nph_vec3(-0.25F, 0.0F, -0.15F),
                 nph_vec3(0.25F, 0.025F, 0.15F));
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

// The march's answer travels in the signs of the occlusion's values: what the occlusion shows, and
// shades the indirect light by, is the same with it, though the march finds something.
NTEST_F(Contact, the_occlusion_is_the_same_with_it)
{
    REQUIRE_CONTACT(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.02F);
    static TestBoxScene scene;
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, test_cube_mesh(f), grey, grey);
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
