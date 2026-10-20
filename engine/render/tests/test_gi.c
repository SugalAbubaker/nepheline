// SPDX-License-Identifier: Apache-2.0
// Global illumination (render/renderer.h NphRenderView.gi; docs/render.md
// "Global illumination"; engine/render/src/gi.c), against ground truths the
// test computes itself by casting rays at the scene's boxes (test_boxes.h):
// the irradiance each pixel's surface receives, read through the irradiance
// debug view once the radiance cache has had frames to fill. And the cache's
// keys against their CPU twins.
#include "render_internal.h"
#include "test_boxes.h"

#include <math.h>
#include <stdlib.h>

typedef RenderFixture Gi;

NTEST_SETUP(Gi)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Gi)
{
    test_render_teardown(f);
}

#define SIZE   TEST_BOX_SIZE
#define PIXELS TEST_BOX_PIXELS
// The ground truths' samples per axis (S x S cosine-weighted directions a
// pixel), and the frames the cache fills over (its history holds 128
// samples; at this size a cell takes several a frame).
#define STRATA 24U
#define FRAMES 48U
// EV100 0: radiance times 1 / 1.2.
#define EXPOSURE (1.0F / 1.2F)

#define REQUIRE_GI(f)                                                                              \
    do {                                                                                           \
        REQUIRE_PRODUCTION(f);                                                                     \
        if (!(f)->renderer->gi.supported) {                                                        \
            NTEST_SKIP("no radiance cache on this device (ray queries)");                          \
        }                                                                                          \
    } while (0)

// The irradiance view of the boxes with global illumination, and the frame
// after `frames` of them from the same camera.
static void gi_view(NphRenderView *v, NphRenderCamera camera)
{
    test_box_view(v, camera, NPH_RENDER_VIEW_IRRADIANCE);
    v->gi = 1;
}

static void render_frames(RenderFixture *f, const NphRenderView *v, NphGpuImage target, u8 *out,
                          u32 frames)
{
    for (u32 i = 0; i + 1U < frames; ++i) {
        NTEST_EXPECT_TRUE(test_render_frame(f, v, target).gi);
    }
    NTEST_EXPECT_TRUE(test_box_render(f, v, target, out).gi);
}

// The point each pixel's ray meets (its box's index, -1 for none).
static i32 pixel_point(const TestBoxScene *s, u32 i, NphVec3 *x, NphVec3 *n)
{
    const NphVec3 d = test_box_pixel_ray(s, i);
    f32 t = 0.0F;
    const i32 hit = test_box_hit(s, s->eye, d, 0.0F, &t, n);
    *x = nph_vec3_add(s->eye, nph_vec3_scale(d, t));
    return hit;
}

// ---------------------------------------------------------------------------
// The environment's light
// ---------------------------------------------------------------------------

// Under an environment of one radiance L, the ground alone gets pi L from
// every direction of its hemisphere: the cache's irradiance is the
// environment's, as the shading takes it without the cache.
NTEST_F(Gi, an_open_ground_gets_the_skys_irradiance)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, -2.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 cached[PIXELS];
    static u8 plain[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, cached, 4U);
    v.gi = 0;
    NTEST_EXPECT_FALSE(test_box_render(f, &v, target, plain).gi);
    u32 ground = 0U;
    u32 worst = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        if (pixel_point(&scene, i, &x, &n) >= 0) {
            const u32 a = cached[(u64)i * 4U];
            const u32 b = plain[(u64)i * 4U];
            worst = nph_max_u32(worst, a > b ? a - b : b - a);
            ++ground;
        }
    }
    NTEST_LOG("%u ground pixels: the cache within %u levels of the environment's irradiance",
              ground, worst);
    NTEST_ASSERT_GT_UINT(ground, SIZE * SIZE / 2U);
    NTEST_EXPECT_LE_UINT(worst, 1U);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// A black box on a black ground under an environment of one radiance L:
// what reaches a point is L from the directions the boxes leave open, so its
// irradiance over pi L is the cosine-weighted share of its hemisphere they
// leave open, which stratified rays measure here.
NTEST_F(Gi, the_boxes_occlude_the_sky)
{
    REQUIRE_GI(f);
    const f32 radiance = 0.6F;
    const NphRenderTexture env = test_uniform_environment(f, radiance);
    static TestBoxScene scene;
    const NphRenderMaterial black = test_grey(f, 0.0F);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, test_cube_mesh(f), black, black);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, image, FRAMES);
    f64 err = 0.0;
    f64 shut = 0.0;
    f64 bias = 0.0;
    u32 occluded = 0U;
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        if (pixel_point(&scene, i, &x, &n) < 0) {
            continue;
        }
        const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
        u32 open = 0U;
        for (u32 a = 0; a < STRATA; ++a) {
            for (u32 b = 0; b < STRATA; ++b) {
                f32 t = 0.0F;
                NphVec3 hn;
                open += test_box_hit(&scene, o, test_cosine_direction(n, a, b, STRATA), 0.0F, &t,
                                     &hn) < 0
                            ? 1U
                            : 0U;
            }
        }
        const f32 truth = (f32)open / (f32)(STRATA * STRATA);
        const f32 got = test_box_linear(image, i) / (radiance * EXPOSURE);
        if (truth < 0.9F) {
            err += (f64)nph_f32_abs(got - truth);
            shut += (f64)(1.0F - truth);
            bias += (f64)(got - truth);
            ++occluded;
        }
        ++count;
    }
    NTEST_LOG("%u pixels, %u occluded: error %.3f of the occlusion, bias %+.4f", count, occluded,
              err / shut, bias / (f64)occluded);
    NTEST_ASSERT_GT_UINT(occluded, 500U);
    NTEST_EXPECT_TRUE(err < 0.25 * shut);
    NTEST_EXPECT_TRUE(nph_f64_abs(bias / (f64)occluded) < 0.05);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// ---------------------------------------------------------------------------
// The sun's light, reflected
// ---------------------------------------------------------------------------

// A white wall in the sun on a black ground, under no sky: what reaches the
// ground before the wall is the wall's sunlit face, a Lambertian reflector
// of radiance (1 / pi) E cos, wherever the sun reaches it; rays find it, and
// shadow rays the sun, here as in the cache's update. The irradiance view
// shows the reflected light's irradiance over pi.
NTEST_F(Gi, the_suns_light_bounces_off_a_wall)
{
    REQUIRE_GI(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMesh mesh = test_cube_mesh(f);
    test_box_ground(f, &scene, mesh, test_grey(f, 0.0F));
    test_box_add(f, &scene, mesh, test_grey(f, 1.0F), nph_vec3(-1.0F, 0.0F, -0.2F),
                 nph_vec3(1.0F, 1.5F, 0.2F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.3F, 1.6F, 3.0F), nph_vec3(0.0F, 0.2F, 0.0F), 0.8F);
    const NphVec3 sun_direction = nph_vec3_normalize(nph_vec3(0.3F, -0.6F, -0.74F));
    const f32 illuminance = 3.0F;
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 1.0F, 1.0F);
    ld.intensity = illuminance;
    ld.direction = sun_direction;
    ld.shadows = 1;
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, image, FRAMES);
    const NphVec3 to_sun = nph_vec3_neg(sun_direction);
    f64 err = 0.0;
    f64 lit = 0.0;
    f64 bias = 0.0;
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        // The ground before the wall.
        if (pixel_point(&scene, i, &x, &n) != 0 || x.z < 0.2F) {
            continue;
        }
        const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
        f32 sum = 0.0F;
        for (u32 a = 0; a < STRATA; ++a) {
            for (u32 b = 0; b < STRATA; ++b) {
                const NphVec3 d = test_cosine_direction(n, a, b, STRATA);
                f32 t = 0.0F;
                NphVec3 hn;
                if (test_box_hit(&scene, o, d, 0.0F, &t, &hn) != 1) {
                    continue; // the black ground, or nothing
                }
                const f32 cosine = nph_vec3_dot(hn, to_sun);
                if (cosine <= 0.0F) {
                    continue;
                }
                const NphVec3 h =
                    nph_vec3_add(nph_vec3_add(o, nph_vec3_scale(d, t)), nph_vec3_scale(hn, 1e-4F));
                f32 ts = 0.0F;
                NphVec3 sn;
                if (test_box_hit(&scene, h, to_sun, 0.0F, &ts, &sn) < 0) {
                    sum += illuminance * cosine * NPH_INV_PI;
                }
            }
        }
        // The reflected light's mean radiance over the cosine-weighted
        // directions: the irradiance over pi.
        const f32 truth = sum / (f32)(STRATA * STRATA);
        const f32 got = test_box_linear(image, i) / EXPOSURE;
        err += (f64)nph_f32_abs(got - truth);
        lit += (f64)truth;
        bias += (f64)(got - truth);
        ++count;
    }
    NTEST_LOG("%u ground pixels: error %.3f of the reflected light, bias %+.4f of it", count,
              err / lit, bias / lit);
    NTEST_ASSERT_GT_UINT(count, 2000U);
    NTEST_EXPECT_TRUE(err < 0.2 * lit);
    NTEST_EXPECT_TRUE(nph_f64_abs(bias) < 0.08 * lit);
    test_box_release(f, target);
    nph_render_light_destroy(f->renderer, sun);
}

// ---------------------------------------------------------------------------
// The keys
// ---------------------------------------------------------------------------

static int compare_u32(const void *a, const void *b)
{
    const u32 x = *(const u32 *)a;
    const u32 y = *(const u32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static bool contains(const u32 *sorted, u32 count, u32 value)
{
    return bsearch(&value, sorted, count, sizeof(u32), compare_u32) != NULL;
}

// After a frame, the cache's slots hold the fingerprints of the cells the
// view's pixels lie in, as gi.c's twins of the shader's keys work them out
// (each level's camera cell in double precision; the point camera
// relative): every slot one of them, and nearly every cell a slot (a point
// on a cell's face may land either side of it on the GPU).
NTEST_F(Gi, the_cache_holds_the_cells_the_view_sees)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    static TestBoxScene scene;
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    const NphRenderCamera camera = test_box_on_ground(f, &scene, test_cube_mesh(f), grey, grey);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    NphRenderView v;
    gi_view(&v, camera);
    NTEST_EXPECT_TRUE(test_render_frame(f, &v, target).gi);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const RenderGi *gi = &f->renderer->gi;
    static u32 slots[1U << 16U];
    NTEST_ASSERT_LE_UINT(gi->capacity, (u32)(sizeof slots / sizeof slots[0]));
    test_read_buffer(f->dev, gi->keys, 0U, (u64)gi->capacity * sizeof(u32), slots);

    // The expected fingerprints, of every pixel's point.
    static NphRenderGiLevel levels[NPH_RENDER_GI_LEVELS];
    render_gi_levels(camera.position, 48.0, levels);
    static u32 expected[SIZE * SIZE];
    u32 cells = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        if (pixel_point(&scene, i, &x, &n) < 0) {
            continue;
        }
        const NphVec3 p = nph_vec3_sub(x, scene.eye);
        const u64 h = render_gi_mix(render_gi_key(levels, p, n));
        expected[cells++] = (u32)(h >> 32U) | 1U;
    }
    qsort(expected, cells, sizeof(u32), compare_u32);
    u32 distinct = 0U;
    for (u32 i = 0; i < cells; ++i) {
        if (i == 0U || expected[i] != expected[i - 1U]) {
            expected[distinct++] = expected[i];
        }
    }
    static u32 held[1U << 16U];
    u32 used = 0U;
    u32 known = 0U;
    for (u32 i = 0; i < gi->capacity; ++i) {
        if (slots[i] != 0U) {
            held[used++] = slots[i];
            known += contains(expected, distinct, slots[i]) ? 1U : 0U;
        }
    }
    qsort(held, used, sizeof(u32), compare_u32);
    u32 found = 0U;
    for (u32 i = 0; i < distinct; ++i) {
        found += contains(held, used, expected[i]) ? 1U : 0U;
    }
    NTEST_LOG("%u cells from %u pixels; %u slots used, %u of them expected; %u cells found",
              distinct, cells, used, known, found);
    NTEST_ASSERT_GT_UINT(distinct, 1000U);
    NTEST_EXPECT_TRUE((f64)known >= 0.98 * (f64)used);
    NTEST_EXPECT_TRUE((f64)found >= 0.98 * (f64)distinct);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// The levels put a cell 1 / (2 scale) to 1 / scale of its distance across,
// and each level's camera cell and offset rebuild the camera's position.
NTEST(gi, levels_and_keys)
{
    NphRenderGiLevel levels[NPH_RENDER_GI_LEVELS];
    const NphDVec3 camera = nph_dvec3(1.0e7 + 0.3, -2.5e6 - 0.7, 42.125);
    render_gi_levels(camera, 48.0, levels);
    const f64 c[3] = {camera.x, camera.y, camera.z};
    for (u32 l = 0; l < (u32)NPH_RENDER_GI_LEVELS; ++l) {
        const f64 size = ldexp(1.0, (i32)l - NPH_RENDER_GI_LEVEL_BIAS) / 48.0;
        NTEST_EXPECT_NEAR_F64((f64)levels[l].origin.w, size, size * 1e-6);
        const f32 o[3] = {levels[l].origin.x, levels[l].origin.y, levels[l].origin.z};
        for (u32 a = 0; a < 3U; ++a) {
            NTEST_EXPECT_TRUE(o[a] >= 0.0F && (f64)o[a] < size * 1.0001);
            // Where the cell's index fits its 32 bits, the two rebuild the
            // camera (finer levels keep the index's low bits only).
            if (nph_f64_abs(c[a] / size) < 2.0e9) {
                NTEST_EXPECT_NEAR_F64((f64)(i32)levels[l].cell[a] * size + (f64)o[a], c[a],
                                      size * 1e-5);
            }
        }
    }
    // Level 8 holds 1 to 2 m, its cells 1/48 m across (1/48 to 1/96 of
    // that).
    NTEST_EXPECT_EQ_INT(render_gi_level(1.0F), 8);
    NTEST_EXPECT_EQ_INT(render_gi_level(1.99F), 8);
    NTEST_EXPECT_EQ_INT(render_gi_level(2.0F), 9);
    NTEST_EXPECT_EQ_INT(render_gi_level(0.5F), 7);
    NTEST_EXPECT_EQ_INT(render_gi_level(1e-9F), 0);
    NTEST_EXPECT_EQ_INT(render_gi_level(1e30F), NPH_RENDER_GI_LEVELS - 1);
    // Two points of one cell share a key; across its face, or facing
    // another octant, they do not.
    const NphVec3 up = nph_vec3(0.0F, 1.0F, 0.0F);
    const NphVec3 p = nph_vec3(1.2F, -0.4F, 0.3F);
    const f32 size = levels[render_gi_level(nph_vec3_length(p))].origin.w;
    const u64 k = render_gi_key(levels, p, up);
    NTEST_EXPECT_TRUE(render_gi_key(levels, nph_vec3_add(p, nph_vec3(3.0F * size, 0, 0)), up) != k);
    NTEST_EXPECT_TRUE(render_gi_key(levels, p, nph_vec3(0.0F, -1.0F, 0.0F)) != k);
    NTEST_EXPECT_EQ_UINT((u32)(k >> 51U) & 31U, (u32)render_gi_level(nph_vec3_length(p)));
    // The mix spreads keys that differ in one bit over the whole hash.
    const u64 a = render_gi_mix(k);
    const u64 b = render_gi_mix(k ^ 1U);
    u32 differ = 0U;
    for (u32 bit = 0; bit < 64U; ++bit) {
        differ += ((a ^ b) >> bit) & 1U ? 1U : 0U;
    }
    NTEST_EXPECT_GT_UINT(differ, 16U);
    // The update's block keeps the samples within the budget.
    NTEST_EXPECT_EQ_UINT(render_gi_stride(128U, 128U, 65536U), 1U);
    const u32 s = render_gi_stride(3840U, 2160U, 65536U);
    NTEST_EXPECT_TRUE((u64)((3840U + s - 1U) / s) * ((2160U + s - 1U) / s) <= 65536U);
    NTEST_EXPECT_TRUE((u64)((3840U + s - 2U) / (s - 1U)) * ((2160U + s - 2U) / (s - 1U)) > 65536U);
}

// ---------------------------------------------------------------------------
// Which frames have it
// ---------------------------------------------------------------------------

// A production view asking for it takes it where its image shows the
// diffuse indirect light (lit, the indirect and the irradiance debug
// views); the reference renderer never.
NTEST_F(Gi, views_that_show_it_take_it)
{
    REQUIRE_GI(f);
    NphRenderView v;
    test_box_view(&v, test_camera(), NPH_RENDER_VIEW_LIT);
    v.gi = 1;
    for (u32 d = 0; d < (u32)NPH_RENDER_VIEW_COUNT; ++d) {
        v.debug_view = (NphRenderDebugView)d;
        const bool shown = d == (u32)NPH_RENDER_VIEW_LIT || d == (u32)NPH_RENDER_VIEW_INDIRECT ||
                           d == (u32)NPH_RENDER_VIEW_IRRADIANCE;
        NTEST_EXPECT_EQ_INT(test_render_frame(f, &v, f->target).gi, shown ? 1 : 0);
    }
    v.debug_view = NPH_RENDER_VIEW_LIT;
    v.gi = 0;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).gi);
    v.gi = 1;
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).gi);
}
