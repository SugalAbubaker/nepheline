// SPDX-License-Identifier: Apache-2.0
// The many lights' per-pixel reservoirs (render/renderer.h NphRenderView.many_restir;
// docs/render.md "Many lights"; ADR-0074), against a ground truth the test computes by
// casting a ray at the scene's boxes for every light from every pixel (test_boxes.h).
//
// What a reservoir estimates is a sum over the lights that see the pixel, so that is
// what is compared: the mean of many frames, over the pixels of a region, where a wall
// hides half of the lights. An estimator that weighs lights it has not looked for shows
// here as light through the wall, however good its picture looks in an open room.
#include <nepheline/math/random.h>

#include "render_internal.h"
#include "test_boxes.h"

#include <math.h>

typedef RenderFixture Restir;

#define RESTIR_LIGHTS 64U

NTEST_SETUP(Restir)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_lights = NPH_RENDER_MAX_LIGHTS;
    d.max_many_lights = RESTIR_LIGHTS;
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(Restir)
{
    test_render_teardown(f);
}

#define SIZE   TEST_BOX_SIZE
#define PIXELS TEST_BOX_PIXELS
// EV100 0: radiance times 1 / 1.2.
#define EXPOSURE (1.0F / 1.2F)
// Beyond this all of a light is its far field at the ground (they are 3 m up or more).
#define RESTIR_NEAR 0.5F

// Frames until the reservoirs have filled, and frames then averaged.
#define RESTIR_WARM 256U
#define RESTIR_MEAN 96U

#define RESTIR_INTENSITY 2.0F

static f32 next_in(NphPcg32 *r, f32 lo, f32 hi)
{
    return lo + (hi - lo) * nph_pcg32_next_f32(r);
}

// `count` positions in the box from `lo` to `hi`, the same every time, and each at least half a
// metre off the plane z = 0, where the wall stands: a light flush with its face is seen or not
// by a rounding.
static void scatter(NphVec3 *out, u32 count, NphVec3 lo, NphVec3 hi)
{
    NphPcg32 r;
    nph_pcg32_seed(&r, 7U, 11U);
    for (u32 i = 0; i < count; ++i) {
        // One draw to a statement: C leaves the order within one expression open.
        const f32 x = next_in(&r, lo.x, hi.x);
        const f32 y = next_in(&r, lo.y, hi.y);
        const f32 z = next_in(&r, lo.z, hi.z);
        out[i] = nph_vec3(x, y, z + (z >= 0.0F ? 0.5F : -0.5F));
    }
}

// A surface of the shading's model (surface.glsl) with no texture.
typedef struct RestirMaterial {
    f32 base;
    f32 metallic;
    f32 roughness;
} RestirMaterial;

static NphRenderMaterial restir_material(RenderFixture *f, RestirMaterial m)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(m.base, m.base, m.base, 1.0F);
    md.metallic = m.metallic;
    md.roughness = m.roughness;
    NphRenderMaterial out = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &out), NPH_RESULT_OK);
    return out;
}

// The specular lobe's single scattering (nph_lit_reflect over its energy term, and over N.L), and
// the factor the energy term adds for what scatters more than once.
static void restir_spec(RestirMaterial m, NphVec3 n, NphVec3 v, NphVec3 l, f32 *spec, f32 *energy)
{
    const f32 nol = nph_vec3_dot(n, l);
    const f32 nov = nph_f32_max(nph_vec3_dot(n, v), 1e-4F);
    const NphVec3 h = nph_vec3_normalize(nph_vec3_add(v, l));
    const f32 noh = nph_f32_clamp(nph_vec3_dot(n, h), 0.0F, 1.0F);
    const f32 voh = nph_f32_clamp(nph_vec3_dot(v, h), 0.0F, 1.0F);
    const f32 a = m.roughness * m.roughness;
    const f32 a2 = a * a;
    const f32 d = (noh * a2 - noh) * noh + 1.0F;
    const f32 gv = nol * nph_f32_sqrt(nov * nov * (1.0F - a2) + a2);
    const f32 gl = nov * nph_f32_sqrt(nol * nol * (1.0F - a2) + a2);
    const f32 dv = 0.5F * a2 / (NPH_PI * d * d * nph_f32_max(gv + gl, 1e-8F));
    const NphVec2 dfg = nph_render_ref_dfg(nov, m.roughness, 256U);
    const f32 ess = nph_f32_max(dfg.x + dfg.y, 1e-4F);
    const f32 f0 = 0.04F + (m.base - 0.04F) * m.metallic;
    const f32 x = 1.0F - voh;
    const f32 x2 = x * x;
    *spec = dv * (f0 + (1.0F - f0) * (x2 * x2 * x));
    *energy = 1.0F + f0 * (1.0F / ess - 1.0F);
}

// What the shading's indirect light takes of the cache's irradiance over pi: what scatters more
// than once and the diffuse the single scattering leaves (surface.glsl, Fdez-Aguera 2019).
static f32 restir_cache_share(RestirMaterial m, f32 nov)
{
    const NphVec2 dfg = nph_render_ref_dfg(nov, m.roughness, 256U);
    const f32 f0 = 0.04F + (m.base - 0.04F) * m.metallic;
    const f32 fr = nph_f32_max(1.0F - m.roughness, f0) - f0;
    const f32 x = 1.0F - nov;
    const f32 k_s = f0 + fr * (x * x * x * x * x);
    const f32 fss_ess = k_s * dfg.x + dfg.y;
    const f32 ems = 1.0F - (dfg.x + dfg.y);
    const f32 f_avg = f0 + (1.0F - f0) / 21.0F;
    const f32 fms_ems = fss_ess * f_avg / (1.0F - ems * f_avg) * ems;
    const f32 k_d = m.base * (1.0F - m.metallic) * (1.0F - fss_ess - fms_ems);
    return fms_ems + k_d;
}

// The pre-exposed radiance the far fields of the lights the boxes let through reflect from (x, n)
// towards the eye, by a ray from there to each: all of it by the reservoirs if `restir` says so,
// or their single scattering with the rest by the cache.
static f32 restir_truth(const TestBoxScene *s, const NphVec3 *lights, u32 count,
                        NphRenderRestir restir, RestirMaterial m, NphVec3 x, NphVec3 n)
{
    const NphVec3 from = nph_vec3_add(x, nph_vec3_scale(n, 1e-3F));
    const NphVec3 v = nph_vec3_normalize(nph_vec3_sub(s->eye, x));
    const f32 k = 1.0F / (RESTIR_NEAR * RESTIR_NEAR);
    const f32 diffuse = m.base * (1.0F - m.metallic) / NPH_PI;
    f64 lit = 0.0;
    f64 irradiance = 0.0;
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 to = nph_vec3_sub(lights[i], from);
        const f32 d2 = nph_vec3_dot(to, to);
        const f32 dist = nph_f32_sqrt(d2);
        const NphVec3 l = nph_vec3_scale(to, 1.0F / dist);
        const f32 nol = nph_vec3_dot(n, l);
        if (nol <= 0.0F) {
            continue;
        }
        f32 t = 0.0F;
        NphVec3 hit_n;
        const i32 hit = test_box_hit(s, from, l, 0.0F, &t, &hit_n);
        if (hit >= 0 && t < dist) {
            continue;
        }
        const f32 q = d2 * k;
        const f32 window = nph_f32_clamp(1.0F - q * q, 0.0F, 1.0F);
        const f32 e = RESTIR_INTENSITY * (1.0F - window * window) * nol / d2;
        f32 spec = 0.0F;
        f32 energy = 1.0F;
        restir_spec(m, n, v, l, &spec, &energy);
        irradiance += (f64)e;
        lit += (f64)(e * (restir == NPH_RENDER_RESTIR_ALL ? diffuse + spec * energy : spec));
    }
    if (restir != NPH_RENDER_RESTIR_ALL) {
        lit += irradiance * (f64)(restir_cache_share(m, nph_vec3_dot(n, v)) / NPH_PI);
    }
    return (f32)lit * EXPOSURE;
}

static void restir_image(RenderFixture *f, const NphRenderView *v, NphGpuImage target, f32 *out)
{
    static u8 image[PIXELS];
    (void)test_box_render(f, v, target, image);
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        out[i] = test_box_linear(image, i);
    }
}

// The mean of `frames` frames' images, linear.
static void restir_mean(RenderFixture *f, const NphRenderView *v, NphGpuImage target, u32 frames,
                        f32 *mean)
{
    static f32 one[SIZE * SIZE];
    memset(mean, 0, (size_t)SIZE * SIZE * sizeof(f32));
    for (u32 n = 0; n < frames; ++n) {
        restir_image(f, v, target, one);
        for (u32 i = 0; i < SIZE * SIZE; ++i) {
            mean[i] += one[i];
        }
    }
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        mean[i] /= (f32)frames;
    }
}

typedef struct RestirError {
    u32 compared;
    f64 bias;     // over the pixels' truth, their mean's error
    f64 scatter;  // the root mean square of a pixel's relative error
    f64 got_sum;  // of what was drawn
    f64 true_sum; // of the truth
} RestirError;

// The pixels of the ground whose truth is at least `floor` and at most what the target holds.
static RestirError restir_compare(const TestBoxScene *s, const NphVec3 *lights,
                                  NphRenderRestir restir, RestirMaterial m, const f32 *mean,
                                  f32 floor)
{
    RestirError e;
    memset(&e, 0, sizeof e);
    f64 squares = 0.0;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(s, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(s, s->eye, d, 0.0F, &t, &n) != 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(s->eye, nph_vec3_scale(d, t));
        const f32 truth = restir_truth(s, lights, RESTIR_LIGHTS, restir, m, x, n);
        if (truth < floor || truth > 0.9F) {
            continue;
        }
        const f32 err = (mean[i] - truth) / truth;
        e.compared += 1U;
        e.got_sum += (f64)mean[i];
        e.true_sum += (f64)truth;
        squares += (f64)(err * err);
    }
    if (e.compared != 0U) {
        e.bias = e.got_sum / e.true_sum - 1.0;
        e.scatter = sqrt(squares / (f64)e.compared);
    }
    return e;
}

static void restir_lights(RenderFixture *f, const NphVec3 *at, u32 count, NphRenderManyLight *out)
{
    for (u32 i = 0; i < count; ++i) {
        NphRenderManyLightDesc d;
        memset(&d, 0, sizeof d);
        d.position = nph_dvec3_from_vec3(at[i]);
        d.color = nph_vec3(1.0F, 1.0F, 1.0F);
        d.intensity = RESTIR_INTENSITY;
        d.near_range = RESTIR_NEAR;
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &out[i]), NPH_RESULT_OK);
    }
}

static void restir_view(NphRenderView *v, NphRenderCamera camera, NphRenderRestir restir)
{
    test_box_view(v, camera, NPH_RENDER_VIEW_LIT);
    v->disable_ibl = 1;
    v->gi = 1;
    v->many_restir = restir;
}

static bool restir_supported(RenderFixture *f)
{
    return f->renderer->gi.supported && f->renderer->rt.supported;
}

#define REQUIRE_RESTIR(f)                                                                          \
    do {                                                                                           \
        REQUIRE_PRODUCTION(f);                                                                     \
        if (!restir_supported(f)) {                                                                \
            NTEST_SKIP("no rays on this device");                                                  \
        }                                                                                          \
    } while (0)

// A ground of the material, a black wall across it as tall as the lights are high, and the
// camera over the near half. The ground sees only the wall, which is black, and the sky, which
// is black: nothing bounces.
static void restir_walled(RenderFixture *f, TestBoxScene *scene, RestirMaterial ground,
                          NphRenderCamera *camera)
{
    memset(scene, 0, sizeof *scene);
    const NphRenderMesh cube = test_cube_mesh(f);
    test_box_add(f, scene, cube, restir_material(f, ground), nph_vec3(-20.0F, -1.0F, -20.0F),
                 nph_vec3(20.0F, 0.0F, 20.0F));
    const RestirMaterial black = {0.0F, 0.0F, 1.0F};
    test_box_add(f, scene, cube, restir_material(f, black), nph_vec3(-20.0F, 0.0F, -0.1F),
                 nph_vec3(20.0F, 9.0F, 0.1F));
    *camera = test_box_camera(scene, nph_vec3(0.0F, 7.0F, 8.0F), nph_vec3(0.0F, 0.0F, 2.5F), 0.8F);
}

static const NphVec3 k_restir_lo = {-5.0F, 3.0F, -5.0F};
static const NphVec3 k_restir_hi = {5.0F, 6.0F, 5.0F};

// Half of the lights behind the wall, the reservoirs filled, then the mean of many frames.
static RestirError restir_run(RenderFixture *f, NphRenderRestir restir, RestirMaterial ground,
                              const char *what)
{
    static TestBoxScene scene;
    NphRenderCamera camera;
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, restir);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static f32 mean[SIZE * SIZE];
    restir_mean(f, &v, target, RESTIR_MEAN, mean);
    const RestirError e = restir_compare(&scene, at, restir, ground, mean, 0.02F);
    NTEST_LOG("%s: %u pixels: drew %.1f %% of the truth, a pixel within %.1f %% of its own", what,
              e.compared, 100.0 * (1.0 + e.bias), 100.0 * e.scatter);
    NTEST_ASSERT_GT_UINT(e.compared, SIZE * SIZE / 8U);
    test_box_release(f, target);
    return e;
}

// All of the far fields' light by reservoirs on a diffuse ground, half of the lights behind
// the wall: their share must not arrive.
NTEST_F(Restir, all_of_the_light_is_the_lights_the_pixel_sees)
{
    REQUIRE_RESTIR(f);
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    const RestirError e = restir_run(f, NPH_RENDER_RESTIR_ALL, ground, "all, grey");
    NTEST_EXPECT_TRUE(fabs(e.bias) < 0.05);
    NTEST_EXPECT_TRUE(e.scatter < 0.35);
}

// A metal is all highlight: what the reservoirs bring, weighed by the lobe, with what scatters
// more than once from the cache. A polished one's lobe reaches few lights.
NTEST_F(Restir, a_metal_shows_the_highlights_of_the_lights_it_sees)
{
    REQUIRE_RESTIR(f);
    const RestirMaterial ground = {1.0F, 1.0F, 0.5F};
    const RestirError e = restir_run(f, NPH_RENDER_RESTIR_SPECULAR, ground, "specular, metal");
    NTEST_EXPECT_TRUE(fabs(e.bias) < 0.08);
    NTEST_EXPECT_TRUE(e.scatter < 0.45);
}

// A rough metal scatters much more than once. That is the cache's to give (its irradiance times
// the scattering's share), so a reservoir taking the highlight whole would count it twice: at this
// roughness about 40 % too bright.
NTEST_F(Restir, a_rough_metal_counts_its_scattering_once)
{
    REQUIRE_RESTIR(f);
    const RestirMaterial ground = {1.0F, 1.0F, 0.9F};
    const RestirError e = restir_run(f, NPH_RENDER_RESTIR_SPECULAR, ground, "specular, rough");
    NTEST_EXPECT_TRUE(fabs(e.bias) < 0.08);
    NTEST_EXPECT_TRUE(e.scatter < 0.35);
}

// The far fields' diffuse light stays the cache's when the reservoirs take the highlights.
NTEST_F(Restir, the_highlights_leave_the_diffuse_light_to_the_cache)
{
    REQUIRE_RESTIR(f);
    const RestirMaterial ground = {0.7F, 0.0F, 0.6F};
    const RestirError e = restir_run(f, NPH_RENDER_RESTIR_SPECULAR, ground, "specular, grey");
    NTEST_EXPECT_TRUE(fabs(e.bias) < 0.08);
    NTEST_EXPECT_TRUE(e.scatter < 0.30);
}

// The reservoirs follow the surface as the camera comes closer: each frame's image against that
// frame's truth, and no more rays than a still camera's. The depth changes by 2 % a frame, so a
// reservoir looked for anywhere but where its point was fails its depth test and starts again,
// and those few take their turns far more often.
NTEST_F(Restir, a_moving_camera_keeps_its_reservoirs)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphVec3 eye = nph_vec3(0.0F, 7.0F, 15.0F);
    const NphVec3 target = nph_vec3(0.0F, 0.0F, 9.5F);
    camera = test_box_camera(&scene, eye, target, 0.8F);
    const NphGpuImage image_target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_ALL);
    u64 still = 0U;
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        const NphRenderFrameStats s = test_render_frame(f, &v, image_target);
        still += i >= RESTIR_WARM - 32U ? s.vis_many_rays : 0U;
    }
    u64 moving = 0U;
    u32 moving_frames = 0U;
    f64 got = 0.0;
    f64 truth = 0.0;
    static f32 image[SIZE * SIZE];
    for (u32 i = 0; i < 32U; ++i) {
        const NphVec3 step = nph_vec3(0.0F, 0.0F, -0.25F * (f32)(i + 1U));
        camera = test_box_camera(&scene, nph_vec3_add(eye, step), nph_vec3_add(target, step), 0.8F);
        v.camera = camera;
        if ((i & 7U) == 7U) {
            restir_image(f, &v, image_target, image);
            const RestirError e =
                restir_compare(&scene, at, NPH_RENDER_RESTIR_ALL, ground, image, 0.02F);
            got += e.got_sum;
            truth += e.true_sum;
        } else {
            const NphRenderFrameStats s = test_render_frame(f, &v, image_target);
            moving += s.vis_many_rays;
            moving_frames += 1U;
        }
    }
    const f64 still_rate = (f64)still / 32.0;
    const f64 moving_rate = (f64)moving / (f64)moving_frames;
    NTEST_LOG("drew %.1f %% of the truth moving; %.0f rays a frame moving, %.0f still",
              100.0 * got / truth, moving_rate, still_rate);
    NTEST_ASSERT_TRUE(truth > 0.0);
    NTEST_EXPECT_TRUE(fabs(got / truth - 1.0) < 0.15);
    NTEST_EXPECT_TRUE(moving_rate < 2.0 * still_rate);
    test_box_release(f, image_target);
}

// What the reservoirs and the tiles' lists hold are indices into the set: a change that rebuilds
// it starts them again, or they would light with the old weights.
NTEST_F(Restir, a_changed_set_of_lights_starts_the_reservoirs_again)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_ALL);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    // Most of the lights go: what is left is a little of the light, by few candidates' weights.
    for (u32 i = 8U; i < RESTIR_LIGHTS; ++i) {
        nph_render_many_light_destroy(f->renderer, many[i]);
    }
    for (u32 i = 8U; i < RESTIR_LIGHTS; ++i) {
        // Far enough to count for nothing, so that the truth may take all the positions.
        at[i] = nph_vec3(0.0F, 1e4F, 0.0F);
    }
    for (u32 i = 0; i < 64U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static f32 mean[SIZE * SIZE];
    restir_mean(f, &v, target, RESTIR_MEAN, mean);
    const RestirError e = restir_compare(&scene, at, NPH_RENDER_RESTIR_ALL, ground, mean, 0.005F);
    NTEST_LOG("%u pixels: drew %.1f %% of the truth", e.compared, 100.0 * (1.0 + e.bias));
    NTEST_ASSERT_GT_UINT(e.compared, SIZE * SIZE / 8U);
    NTEST_EXPECT_TRUE(fabs(e.bias) < 0.10);
    test_box_release(f, target);
}

// A surface uncovered is a new surface: the reservoir of what stood in front of it has another
// surface's weight (here a roof two metres nearer the lights), and must not carry over. Around
// the box what its shadow hid comes into view more slowly: a reservoir's memory (ADR-0074).
NTEST_F(Restir, what_was_behind_something_starts_again)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphRenderMesh cube = test_cube_mesh(f);
    test_box_add(f, &scene, cube, restir_material(f, ground), nph_vec3(-3.0F, 0.0F, 2.0F),
                 nph_vec3(3.0F, 2.0F, 5.0F));
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_ALL);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    // The pixels the box covers, and those of its top alone: the sides' depth is close to the
    // ground's where they meet it.
    static u8 covered[SIZE * SIZE];
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        covered[i] = test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) == 2 && n.y > 0.5F ? 1U : 0U;
    }
    nph_render_instance_set_hidden(f->renderer, scene.instances[2], true);
    scene.count = 2U; // the truth's scene, too
    for (u32 i = 0; i < 64U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static f32 mean[SIZE * SIZE];
    restir_mean(f, &v, target, RESTIR_MEAN, mean);
    f64 got = 0.0;
    f64 truth = 0.0;
    u32 compared = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (covered[i] != 0U && test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) == 0) {
            const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
            const f32 tr =
                restir_truth(&scene, at, RESTIR_LIGHTS, NPH_RENDER_RESTIR_ALL, ground, x, n);
            if (tr <= 0.9F) {
                got += (f64)mean[i];
                truth += (f64)tr;
                compared += 1U;
            }
        }
    }
    NTEST_ASSERT_GT_UINT(compared, 256U);
    NTEST_LOG("%u pixels uncovered: drew %.1f %% of the truth", compared, 100.0 * got / truth);
    NTEST_EXPECT_TRUE(fabs(got / truth - 1.0) < 0.08);
    test_box_release(f, target);
}

// A reservoir's light gives the pixel its colour as well as its weight, so the light it holds
// must change as it weighs others, in proportion to what each delivers: half of the lights red
// and half green of a luminance, against each channel's truth, and the pixels' lights read
// twice, far enough apart for most to have changed. (A pixel's colour is one light's at a time:
// ADR-0074 says what that costs.)
NTEST_F(Restir, a_reservoir_takes_the_lights_colours_too)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    const NphVec3 red = nph_vec3(1.0F / 0.2126F, 0.0F, 0.0F);
    const NphVec3 green = nph_vec3(0.0F, 1.0F / 0.7152F, 0.0F);
    for (u32 i = 0; i < RESTIR_LIGHTS; ++i) {
        NphRenderManyLightDesc d;
        memset(&d, 0, sizeof d);
        d.position = nph_dvec3_from_vec3(at[i]);
        d.color = (i & 1U) != 0U ? red : green;
        d.intensity = 0.25F * RESTIR_INTENSITY;
        d.near_range = RESTIR_NEAR;
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &many[i]), NPH_RESULT_OK);
    }
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_ALL);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    // Each frame's reservoirs are one buffer's and then the other's; both frames are even.
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const u64 bytes = (u64)SIZE * SIZE * 8U;
    static u32 first[SIZE * SIZE * 2U];
    static u32 second[SIZE * SIZE * 2U];
    const u32 buffer = f->renderer->many.reservoir_at;
    test_read_buffer(f->dev, f->renderer->many.reservoirs[buffer], 0U, bytes, first);
    static u8 image[PIXELS];
    static f32 sum_red[SIZE * SIZE];
    static f32 sum_green[SIZE * SIZE];
    // An even number of frames, the mean's, and enough for several turns.
    const u32 frames = 192U;
    for (u32 n = 0; n < frames; ++n) {
        (void)test_box_render(f, &v, target, image);
        for (u32 i = 0; i < SIZE * SIZE; ++i) {
            sum_red[i] += nph_render_ref_srgb_to_linear((f32)image[(u64)i * 4U] / 255.0F);
            sum_green[i] += nph_render_ref_srgb_to_linear((f32)image[(u64)i * 4U + 1U] / 255.0F);
        }
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(f->renderer->many.reservoir_at, buffer);
    test_read_buffer(f->dev, f->renderer->many.reservoirs[buffer], 0U, bytes, second);
    u32 changed = 0U;
    u32 held = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const u32 was = first[(u64)i * 2U] & NPH_RENDER_RESTIR_NO_LIGHT;
        const u32 is = second[(u64)i * 2U] & NPH_RENDER_RESTIR_NO_LIGHT;
        if (was != NPH_RENDER_RESTIR_NO_LIGHT && is != NPH_RENDER_RESTIR_NO_LIGHT) {
            held += 1U;
            changed += was != is ? 1U : 0U;
        }
    }
    // Each channel's truth is its lights' alone, scaled from the test's intensity.
    static NphVec3 only_red[RESTIR_LIGHTS];
    static NphVec3 only_green[RESTIR_LIGHTS];
    for (u32 i = 0; i < RESTIR_LIGHTS; ++i) {
        only_red[i] = (i & 1U) != 0U ? at[i] : nph_vec3(0.0F, 1e4F, 0.0F);
        only_green[i] = (i & 1U) != 0U ? nph_vec3(0.0F, 1e4F, 0.0F) : at[i];
    }
    f64 got_red = 0.0;
    f64 got_green = 0.0;
    f64 true_red = 0.0;
    f64 true_green = 0.0;
    u32 compared = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) != 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
        const f32 tr =
            0.25F / 0.2126F *
            restir_truth(&scene, only_red, RESTIR_LIGHTS, NPH_RENDER_RESTIR_ALL, ground, x, n);
        const f32 tg =
            0.25F / 0.7152F *
            restir_truth(&scene, only_green, RESTIR_LIGHTS, NPH_RENDER_RESTIR_ALL, ground, x, n);
        if (tr < 0.05F || tr > 0.9F || tg < 0.02F || tg > 0.9F) {
            continue;
        }
        got_red += (f64)(sum_red[i] / (f32)frames);
        got_green += (f64)(sum_green[i] / (f32)frames);
        true_red += (f64)tr;
        true_green += (f64)tg;
        compared += 1U;
    }
    NTEST_ASSERT_GT_UINT(compared, SIZE * SIZE / 8U);
    NTEST_LOG("%u pixels: drew %.1f %% of the red, %.1f %% of the green; %u of %u pixels' lights "
              "changed in %u frames",
              compared, 100.0 * got_red / true_red, 100.0 * got_green / true_green, changed, held,
              frames);
    NTEST_EXPECT_TRUE(fabs(got_red / true_red - 1.0) < 0.10);
    NTEST_EXPECT_TRUE(fabs(got_green / true_green - 1.0) < 0.10);
    NTEST_EXPECT_TRUE(changed > held / 4U);
    test_box_release(f, target);
}

// Which light a reservoir holds does not change its weight, only its colour, so a reservoir that
// took every candidate it saw would be right in brightness. Two lights: a red one over a polished
// metal's highlight, and a green one off to the side that the lists offer more often, being the
// stronger, though it delivers almost nothing here. Where the highlight is, a reservoir must
// hold the red light nearly always: in proportion to what each delivers, not to how often it
// comes up.
NTEST_F(Restir, a_reservoir_holds_a_light_for_what_it_delivers)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 1.0F, 0.25F};
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    // Mirrored in the ground from the camera, over the pixels around (0, 0, 5).
    const NphVec3 at_red = nph_vec3(0.0F, 4.8F, 2.9F);
    const NphVec3 at_green = nph_vec3(-4.0F, 3.0F, 3.0F);
    const f32 scale_red = 0.1F;
    const f32 scale_green = 0.4F;
    static NphRenderManyLight many[2];
    for (u32 i = 0; i < 2U; ++i) {
        NphRenderManyLightDesc d;
        memset(&d, 0, sizeof d);
        d.position = nph_dvec3_from_vec3(i == 0U ? at_red : at_green);
        d.color =
            i == 0U ? nph_vec3(1.0F / 0.2126F, 0.0F, 0.0F) : nph_vec3(0.0F, 1.0F / 0.7152F, 0.0F);
        d.intensity = (i == 0U ? scale_red : scale_green) * RESTIR_INTENSITY;
        d.near_range = RESTIR_NEAR;
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &many[i]), NPH_RESULT_OK);
    }
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_SPECULAR);
    // Long enough to be near what it settles on: a short mean over so few tiles reads 10 % low.
    for (u32 i = 0; i < 4U * RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static u8 image[PIXELS];
    static f32 sum_red[SIZE * SIZE];
    static f32 sum_green[SIZE * SIZE];
    for (u32 n = 0; n < RESTIR_MEAN; ++n) {
        (void)test_box_render(f, &v, target, image);
        for (u32 i = 0; i < SIZE * SIZE; ++i) {
            sum_red[i] += nph_render_ref_srgb_to_linear((f32)image[(u64)i * 4U] / 255.0F);
            sum_green[i] += nph_render_ref_srgb_to_linear((f32)image[(u64)i * 4U + 1U] / 255.0F);
        }
    }
    static NphVec3 only_red[RESTIR_LIGHTS];
    static NphVec3 only_green[RESTIR_LIGHTS];
    for (u32 i = 0; i < RESTIR_LIGHTS; ++i) {
        only_red[i] = i == 0U ? at_red : nph_vec3(0.0F, 1e4F, 0.0F);
        only_green[i] = i == 0U ? at_green : nph_vec3(0.0F, 1e4F, 0.0F);
    }
    f64 got_red = 0.0;
    f64 got_green = 0.0;
    f64 true_red = 0.0;
    f64 true_green = 0.0;
    u32 compared = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) != 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
        const f32 tr = scale_red * restir_truth(&scene, only_red, RESTIR_LIGHTS,
                                                NPH_RENDER_RESTIR_SPECULAR, ground, x, n);
        const f32 tg = scale_green * restir_truth(&scene, only_green, RESTIR_LIGHTS,
                                                  NPH_RENDER_RESTIR_SPECULAR, ground, x, n);
        // Where the highlight is, its luminances: a channel's value is its luminance over the
        // eye's weight, and the highlight's must not clip there.
        if (tr / 0.2126F > 0.8F || tr < 0.002F || tg > 0.02F * tr) {
            continue;
        }
        got_red += (f64)(sum_red[i] / (f32)RESTIR_MEAN) * 0.2126;
        got_green += (f64)(sum_green[i] / (f32)RESTIR_MEAN) * 0.7152;
        true_red += (f64)tr;
        true_green += (f64)tg;
        compared += 1U;
    }
    NTEST_LOG(
        "%u pixels of the highlight: of their light, red drew %.1f %% of its truth, green %.2f "
        "%% of the red where the truth is %.2f %%",
        compared, 100.0 * got_red / true_red, 100.0 * got_green / got_red,
        100.0 * true_green / true_red);
    NTEST_ASSERT_GT_UINT(compared, 128U);
    NTEST_EXPECT_TRUE(fabs(got_red / true_red - 1.0) < 0.15);
    NTEST_EXPECT_TRUE(got_green < 0.05 * got_red);
    test_box_release(f, target);
}

// A reservoir's weight still means something under another mode, but its light was chosen for
// that mode's target: a reservoir that had taken only highlights holds lights that a diffuse
// surface gets few of, with weights to match, for as long as its memory lasts. They start again,
// and the image is right soon after.
NTEST_F(Restir, another_mode_starts_the_reservoirs_again)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_SPECULAR);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    v.many_restir = NPH_RENDER_RESTIR_ALL;
    (void)test_render_frame(f, &v, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const u64 bytes = (u64)SIZE * SIZE * 8U;
    static u32 words[SIZE * SIZE * 2U];
    test_read_buffer(f->dev, f->renderer->many.reservoirs[f->renderer->many.reservoir_at], 0U,
                     bytes, words);
    u32 most = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        most = nph_max_u32(most, (words[(u64)i * 2U] >> NPH_RENDER_RESTIR_M_SHIFT) &
                                     NPH_RENDER_RESTIR_M_MASK);
    }
    NTEST_LOG("the most any reservoir has weighed a frame after the change: %u", most);
    NTEST_EXPECT_TRUE(most < NPH_RENDER_RESTIR_FEW);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static f32 mean[SIZE * SIZE];
    restir_mean(f, &v, target, RESTIR_MEAN, mean);
    const RestirError e = restir_compare(&scene, at, NPH_RENDER_RESTIR_ALL, ground, mean, 0.02F);
    NTEST_LOG("%u pixels: drew %.1f %% of the truth", e.compared, 100.0 * (1.0 + e.bias));
    NTEST_EXPECT_TRUE(fabs(e.bias) < 0.10);
    test_box_release(f, target);
}

// A wall that comes between the pixels and all of their lights: each reservoir looks for its own
// light again within a few turns, and finding it gone starts again. By the weights alone the
// light would take many turns to fade.
NTEST_F(Restir, what_comes_between_a_pixel_and_its_lights_shows)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    // All behind the wall.
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    for (u32 i = 0; i < RESTIR_LIGHTS; ++i) {
        at[i].z = -nph_f32_abs(at[i].z);
    }
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_ALL);
    nph_render_instance_set_hidden(f->renderer, scene.instances[1], true);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static f32 before[SIZE * SIZE];
    restir_mean(f, &v, target, 16U, before);
    nph_render_instance_set_hidden(f->renderer, scene.instances[1], false);
    // Every tile looks for its light every fourth turn.
    for (u32 i = 0; i < (4U << (u32)NPH_RENDER_RESTIR_FULL_BITS) + 16U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static f32 after[SIZE * SIZE];
    restir_mean(f, &v, target, 8U, after);
    f64 was = 0.0;
    f64 is = 0.0;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        // The rows below the wall's foot.
        if (i / SIZE >= SIZE / 2U) {
            was += (f64)before[i];
            is += (f64)after[i];
        }
    }
    NTEST_LOG("the ground's light with the wall up: %.1f %% of what it was", 100.0 * is / was);
    NTEST_ASSERT_TRUE(was > 1.0);
    NTEST_EXPECT_TRUE(is < 0.05 * was);
    test_box_release(f, target);
}

// A target of another size has reservoirs of its size, and coming back they start again.
NTEST_F(Restir, the_reservoirs_follow_the_targets_size)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    const NphGpuImage small = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE / 2U);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_ALL);
    for (u32 i = 0; i < 32U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    for (u32 i = 0; i < 32U; ++i) {
        (void)test_render_frame(f, &v, small);
    }
    NTEST_EXPECT_EQ_UINT(f->renderer->many.reservoir_width, SIZE / 2U);
    for (u32 i = 0; i < RESTIR_WARM; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    NTEST_EXPECT_EQ_UINT(f->renderer->many.reservoir_width, SIZE);
    static f32 mean[SIZE * SIZE];
    restir_mean(f, &v, target, RESTIR_MEAN, mean);
    const RestirError e = restir_compare(&scene, at, NPH_RENDER_RESTIR_ALL, ground, mean, 0.02F);
    NTEST_LOG("%u pixels: drew %.1f %% of the truth", e.compared, 100.0 * (1.0 + e.bias));
    NTEST_EXPECT_TRUE(fabs(e.bias) < 0.05);
    test_box_release(f, small);
    test_box_release(f, target);
}

// With none of the lights' highlights asked for, or none of the view showing them, no rays are
// asked for and nothing of the reservoirs is made.
NTEST_F(Restir, a_view_that_does_not_show_them_has_no_reservoirs)
{
    REQUIRE_RESTIR(f);
    static TestBoxScene scene;
    NphRenderCamera camera;
    const RestirMaterial ground = {1.0F, 0.0F, 1.0F};
    restir_walled(f, &scene, ground, &camera);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[RESTIR_LIGHTS];
    scatter(at, RESTIR_LIGHTS, k_restir_lo, k_restir_hi);
    static NphRenderManyLight many[RESTIR_LIGHTS];
    restir_lights(f, at, RESTIR_LIGHTS, many);
    NphRenderView v;
    restir_view(&v, camera, NPH_RENDER_RESTIR_OFF);
    for (u32 i = 0; i < 4U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    NTEST_EXPECT_FALSE(f->renderer->many.frame_restir);
    NTEST_EXPECT_TRUE(nph_gpu_buffer_is_null(f->renderer->many.reservoirs[0]));
    restir_view(&v, camera, NPH_RENDER_RESTIR_SPECULAR);
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    for (u32 i = 0; i < 4U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    NTEST_EXPECT_FALSE(f->renderer->many.frame_restir);
    v.debug_view = NPH_RENDER_VIEW_LIT;
    for (u32 i = 0; i < 4U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    NTEST_EXPECT_TRUE(f->renderer->many.frame_restir);
    NTEST_EXPECT_FALSE(nph_gpu_buffer_is_null(f->renderer->many.reservoirs[0]));
    test_box_release(f, target);
}
