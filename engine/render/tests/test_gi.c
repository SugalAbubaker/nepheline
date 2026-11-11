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
// The history (ADR-0084)
// ---------------------------------------------------------------------------

#define SLOTS (1U << 16U) // test_renderer_desc's gi_cells

// Each slot's sample count after the frames so far, from its cell's word.
static void read_counts(RenderFixture *f, u32 *counts)
{
    static u32 words[SLOTS * 4U];
    NTEST_ASSERT_EQ_UINT(f->renderer->gi.capacity, SLOTS);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, f->renderer->gi.cells, 0U, sizeof words, words);
    for (u32 i = 0; i < SLOTS; ++i) {
        counts[i] = words[i * 4U + 3U] & ((1U << NPH_RENDER_GI_COUNT_BITS) - 1U);
    }
}

// The most samples any cell holds, and how many hold more than their history allows (none
// should).
static u32 most_samples(RenderFixture *f, u32 *over)
{
    static u32 counts[SLOTS];
    read_counts(f, counts);
    u32 most = 0U;
    *over = 0U;
    for (u32 i = 0; i < SLOTS; ++i) {
        most = nph_max_u32(most, counts[i]);
        *over += counts[i] > (u32)NPH_RENDER_GI_HISTORY ? 1U : 0U;
    }
    return most;
}

// The mean of the pixels' linear values where the scene has a surface.
static f64 surface_mean(const TestBoxScene *s, const u8 *image)
{
    f64 sum = 0.0;
    u32 n = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 nrm;
        if (pixel_point(s, i, &x, &nrm) >= 0) {
            sum += (f64)test_box_linear(image, i);
            ++n;
        }
    }
    NTEST_ASSERT_GT_UINT(n, SIZE * SIZE / 2U);
    return sum / (f64)n;
}

// An open ground under an environment of one radiance, which then doubles. Its cells have filled
// their histories, over which they would take dozens of frames to follow (some 20 samples a frame
// reach each here: 1,024 samples' history leaves 92 % of the change after 4 frames, and the old
// 128 samples' 53 %); but their light's recent mean now leaves their estimates by far more than
// its spread allows, so the cells cut their histories down: past half the change in 4 frames, and
// within 3 % of the new light in 16.
NTEST_F(Gi, a_cell_follows_a_change_of_its_light)
{
    REQUIRE_GI(f);
    const NphRenderTexture dim = test_uniform_environment(f, 0.25F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    // A narrow view: each cell is some pixels across and takes dozens of samples a frame.
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, -2.0F), 0.2F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, image, 64U);
    u32 over = 0U;
    const u32 most = most_samples(f, &over);
    const f64 before = surface_mean(&scene, image);
    const NphRenderTexture bright = test_uniform_environment(f, 0.5F);
    render_frames(f, &v, target, image, 4U);
    const f64 soon = surface_mean(&scene, image);
    render_frames(f, &v, target, image, 12U);
    const f64 after = surface_mean(&scene, image);
    const f64 dim_level = 0.25 * (f64)EXPOSURE;
    const f64 bright_level = 0.5 * (f64)EXPOSURE;
    const f64 change = bright_level - dim_level;
    NTEST_LOG("histories of up to %u samples; the ground at %.4f under the first sky (%.4f); "
              "under the second (%.4f), %.4f after 4 frames (%.0f %% of the change) and %.4f "
              "after 16",
              most, before, dim_level, bright_level, soon, 100.0 * (soon - before) / change, after);
    NTEST_EXPECT_EQ_UINT(most, (u32)NPH_RENDER_GI_HISTORY);
    NTEST_EXPECT_EQ_UINT(over, 0U);
    NTEST_EXPECT_NEAR_F64(before, dim_level, 0.02 * dim_level);
    NTEST_EXPECT_TRUE(soon - before > 0.6 * change);
    NTEST_EXPECT_NEAR_F64(after, bright_level, 0.03 * bright_level);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, bright);
    nph_render_texture_destroy(f->renderer, dim);
}

// A box on a black ground under an environment of one radiance L, close up where it shades the
// ground: a sample there is pi L or nothing, as its ray leaves or meets the box, so a cell that
// sees a share p of its sky open has samples spread by L sqrt(p (1 - p)) about its mean, in the
// irradiance view's units. A history of h samples leaves its estimate wandering by about
// 1 / sqrt(2 h - 1) of that: a 128-sample window's level is the bound the history's whole
// NPH_RENDER_GI_HISTORY samples come well under (0.35 of it in theory; measured over 64 frames,
// whose correlation hides some of a long history's wander, 0.2, where 128 samples' measure 0.8).
NTEST_F(Gi, a_still_light_is_averaged_over_the_whole_history)
{
    REQUIRE_GI(f);
    const f32 radiance = 0.6F;
    const NphRenderTexture env = test_uniform_environment(f, radiance);
    static TestBoxScene scene;
    const NphRenderMaterial black = test_grey(f, 0.0F);
    (void)test_box_on_ground(f, &scene, test_cube_mesh(f), black, black);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(2.2F, 2.0F, 3.2F), nph_vec3(0.6F, 0.0F, 0.6F), 0.3F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, image, 96U);
    static f64 sum[SIZE * SIZE];
    static f64 sum2[SIZE * SIZE];
    memset(sum, 0, sizeof sum);
    memset(sum2, 0, sizeof sum2);
    const u32 frames = 64U;
    for (u32 k = 0; k < frames; ++k) {
        NTEST_EXPECT_TRUE(test_box_render(f, &v, target, image).gi);
        for (u32 i = 0; i < SIZE * SIZE; ++i) {
            const f64 x = (f64)test_box_linear(image, i);
            sum[i] += x;
            sum2[i] += x * x;
        }
    }
    const f64 open = (f64)(radiance * EXPOSURE);
    f64 wander = 0.0;
    f64 window = 0.0;
    u32 shaded = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const f64 mean = sum[i] / (f64)frames;
        const f64 p = mean / open;
        if (p < 0.2 || p > 0.8) {
            continue; // open or shut: little spread to average
        }
        wander += sqrt(nph_f64_max(sum2[i] / (f64)frames - mean * mean, 0.0));
        window += open * sqrt(p * (1.0 - p)) / sqrt(2.0 * 128.0 - 1.0);
        ++shaded;
    }
    u32 over = 0U;
    const u32 most = most_samples(f, &over);
    NTEST_LOG("%u half-shaded pixels: they wander by %.3f of a 128-sample window's level; "
              "histories of up to %u samples",
              shaded, wander / window, most);
    NTEST_ASSERT_GT_UINT(shaded, 1000U);
    NTEST_EXPECT_TRUE(wander < 0.6 * window);
    NTEST_EXPECT_EQ_UINT(most, (u32)NPH_RENDER_GI_HISTORY);
    NTEST_EXPECT_EQ_UINT(over, 0U);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// The same half-shaded cells while they are young: until a cell's variance has seen
// NPH_RENDER_GI_VARIANCE_FRAMES frames the test of its light cannot trust it, so it never cuts the
// history, which is the cell's samples as young cells' always were. A frame later, no cell holds
// fewer samples than before (where a test of an unsettled variance cuts noisy cells' histories, and
// leaves young cells, which a walking camera keeps making, noisier than they were).
NTEST_F(Gi, a_young_cell_keeps_all_its_samples)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.6F);
    static TestBoxScene scene;
    const NphRenderMaterial black = test_grey(f, 0.0F);
    (void)test_box_on_ground(f, &scene, test_cube_mesh(f), black, black);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(2.2F, 2.0F, 3.2F), nph_vec3(0.6F, 0.0F, 0.6F), 0.3F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, image, 8U);
    static u32 before[SLOTS];
    static u32 after[SLOTS];
    read_counts(f, before);
    NTEST_EXPECT_TRUE(test_box_render(f, &v, target, image).gi);
    read_counts(f, after);
    u32 grew = 0U;
    u32 fell = 0U;
    for (u32 i = 0; i < SLOTS; ++i) {
        grew += after[i] > before[i] ? 1U : 0U;
        fell += after[i] < before[i] ? 1U : 0U;
    }
    NTEST_LOG("a ninth frame: %u cells took samples, %u lost some", grew, fell);
    NTEST_ASSERT_GT_UINT(grew, 500U);
    NTEST_EXPECT_EQ_UINT(fell, 0U);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// The cells' size follows the distance from the camera, so a camera walking toward a surface
// brings it into a finer level of cells, which hold no samples there yet (ADR-0087). Shaded
// ground under a roof, settled from 2 m further back, the camera then moved and a frame drawn.
// The cells of the finer level the ground's near half has come into hold their neighbouring
// level's light as NPH_RENDER_GI_SEED samples from the frame that made them (gi_seed), and the
// ground keeps its light against the same view settled: any cell with a sample is trusted.
// Otherwise the environment's light, which the roof hides, made up what new cells lacked: rings
// of light followed the camera across shaded ground.
NTEST_F(Gi, a_surface_entering_a_finer_level_keeps_its_light)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.6F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    test_box_ground(f, &scene, mesh, grey);
    test_box_add(f, &scene, mesh, grey, nph_vec3(-3.0F, 1.0F, -7.0F), nph_vec3(3.0F, 1.2F, -2.0F));
    const NphVec3 target_at = nph_vec3(0.0F, 0.0F, -4.0F);
    const NphVec3 far_eye = nph_vec3(0.0F, 0.6F, 2.5F);
    NphRenderCamera camera = test_box_camera(&scene, far_eye, target_at, 0.6F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 early[PIXELS];
    static u8 settled[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, settled, FRAMES);
    // 2 m nearer: the shade's near half, 4 to 6 m away, now 2 to 4 m away.
    camera = test_box_camera(&scene, nph_vec3(0.0F, 0.6F, 0.5F), target_at, 0.6F);
    gi_view(&v, camera);
    render_frames(f, &v, target, early, 1U);

    // The new level's cells, as the frame left them: those of the shaded ground's points whose
    // level is finer now than from the first camera, found by gi.c's twins of the keys.
    const RenderGi *gi = &f->renderer->gi;
    static u32 keys[SLOTS];
    static u32 cells[SLOTS * 4U];
    NTEST_ASSERT_LE_UINT(gi->capacity, (u32)SLOTS);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, gi->keys, 0U, (u64)gi->capacity * sizeof(u32), keys);
    test_read_buffer(f->dev, gi->cells, 0U, (u64)gi->capacity * 16U, cells);
    static NphRenderGiLevel levels[NPH_RENDER_GI_LEVELS];
    render_gi_levels(camera.position, 48.0, levels);
    u32 newcomers = 0U;
    u32 seeded = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        if (pixel_point(&scene, i, &x, &n) != 0 || n.y < 0.5F || nph_f32_abs(x.x) > 2.0F ||
            x.z > -2.5F || x.z < -6.5F) {
            continue;
        }
        const NphVec3 p = nph_vec3_sub(x, scene.eye);
        if (render_gi_level(nph_vec3_length(p)) >= render_gi_level(nph_vec3_distance(x, far_eye))) {
            continue;
        }
        const u64 h = render_gi_mix(render_gi_key(levels, p, n));
        const u32 base = ((u32)h & (gi->capacity / (u32)NPH_RENDER_GI_BUCKET - 1U)) *
                         (u32)NPH_RENDER_GI_BUCKET;
        const u32 fp = (u32)(h >> 32U) | 1U;
        for (u32 s = base; s < base + (u32)NPH_RENDER_GI_BUCKET; ++s) {
            if (keys[s] == fp) {
                ++newcomers;
                const u32 count = cells[(u64)s * 4U + 3U] & ((1U << NPH_RENDER_GI_COUNT_BITS) - 1U);
                seeded += count >= (u32)NPH_RENDER_GI_SEED ? 1U : 0U;
                break;
            }
        }
    }
    render_frames(f, &v, target, settled, FRAMES);

    // The ground under the roof, away from its edges.
    f64 sum_early = 0.0;
    f64 sum_settled = 0.0;
    u32 shaded = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        if (pixel_point(&scene, i, &x, &n) != 0 || n.y < 0.5F || nph_f32_abs(x.x) > 2.0F ||
            x.z > -2.5F || x.z < -6.5F) {
            continue;
        }
        sum_early += (f64)test_box_linear(early, i);
        sum_settled += (f64)test_box_linear(settled, i);
        ++shaded;
    }
    NTEST_ASSERT_GT_UINT(shaded, 1000U);
    const f64 mean_early = sum_early / (f64)shaded;
    const f64 mean_settled = sum_settled / (f64)shaded;
    const f64 open = 0.6 * (f64)EXPOSURE;
    NTEST_LOG("%u shaded pixels: %.4f the frame after the move, %.4f settled (the open sky's "
              "%.4f); %u of their pixels in the finer level's cells, %u of those cells holding "
              "a seed's samples",
              shaded, mean_early, mean_settled, open, newcomers, seeded);
    NTEST_EXPECT_TRUE(mean_settled < 0.7 * open); // the roof shades it
    NTEST_EXPECT_NEAR_F64(mean_early, mean_settled, 0.08 * mean_settled);
    NTEST_ASSERT_GT_UINT(newcomers, 300U);
    // (Not all: seen at a grazing angle, a pixel of the ground covers several cells along it, and
    // the coarser level's cells a pixel's point did not fall in hold nothing to give.)
    NTEST_EXPECT_TRUE((f64)seeded >= 0.3 * (f64)newcomers);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// After a still light has filled the cells, each pixel of the irradiance view is the four cells
// around its point blended (gi_common.glsl gi_surface_blend, ADR-0085) as gi.c's CPU twin blends
// them from the table's own fingerprints and cells: within a level of 8-bit rounding (the GPU
// rebuilds its point from the visibility buffer where the test casts a ray, and the blend is
// continuous, so the two points' gap moves the light little). A lookup of the one cell that holds
// the point strays from it by the cells' differences across the half-shaded ground.
NTEST_F(Gi, a_pixel_blends_the_cells_around_its_point)
{
    REQUIRE_GI(f);
    const f32 radiance = 0.6F;
    const NphRenderTexture env = test_uniform_environment(f, radiance);
    static TestBoxScene scene;
    const NphRenderMaterial black = test_grey(f, 0.0F);
    (void)test_box_on_ground(f, &scene, test_cube_mesh(f), black, black);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(2.2F, 2.0F, 3.2F), nph_vec3(0.6F, 0.0F, 0.6F), 0.3F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    render_frames(f, &v, target, image, 48U);
    // The table as the last frame's shading read it.
    const RenderGi *gi = &f->renderer->gi;
    static u32 keys[SLOTS];
    static u32 cells[SLOTS * 4U];
    NTEST_ASSERT_EQ_UINT(gi->capacity, SLOTS);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, gi->keys, 0U, sizeof keys, keys);
    test_read_buffer(f->dev, gi->cells, 0U, sizeof cells, cells);
    static NphRenderGiLevel levels[NPH_RENDER_GI_LEVELS];
    render_gi_levels(camera.position, 48.0, levels);
    u32 compared = 0U;
    u32 within = 0U;
    u32 worst = 0U;
    f64 spread = 0.0;
    for (u32 row = 1; row + 1U < SIZE; ++row) {
        for (u32 col = 1; col + 1U < SIZE; ++col) {
            const u32 i = row * SIZE + col;
            NphVec3 x;
            NphVec3 n;
            const i32 hit = pixel_point(&scene, i, &x, &n);
            if (hit < 0) {
                continue;
            }
            // Away from the surfaces' edges: the pixel's neighbours see the same face.
            bool inside = true;
            for (u32 d = 0; d < 9U && inside; ++d) {
                NphVec3 xd;
                NphVec3 nd;
                const u32 id = (row + d / 3U - 1U) * SIZE + (col + d % 3U - 1U);
                inside = pixel_point(&scene, id, &xd, &nd) == hit && nph_vec3_dot(nd, n) > 0.999F;
            }
            if (!inside) {
                continue;
            }
            const NphVec3 p = nph_vec3_sub(x, scene.eye);
            const NphVec3 facing = nph_vec3_dot(n, p) > 0.0F ? nph_vec3_scale(n, -1.0F) : n;
            const NphVec4 e = render_gi_blend(levels, keys, cells, SLOTS, p, facing);
            if (e.w < 1.0F) {
                continue; // the environment's irradiance makes up the rest
            }
            const f32 linear = e.x * (NPH_INV_PI * EXPOSURE);
            const u32 want = (u32)(255.0F * nph_render_ref_linear_to_srgb(linear) + 0.5F);
            const u32 got = image[(u64)i * 4U];
            const u32 d = want > got ? want - got : got - want;
            worst = nph_max_u32(worst, d);
            within += d <= 1U ? 1U : 0U;
            spread += (f64)linear;
            ++compared;
        }
    }
    NTEST_LOG("%u pixels against the CPU's blend: %u within a level, the worst %u levels off "
              "(the light %.3f on average)",
              compared, within, worst, spread / (f64)nph_max_u32(compared, 1U));
    NTEST_ASSERT_GT_UINT(compared, 4000U);
    NTEST_EXPECT_TRUE((f64)within >= 0.99 * (f64)compared);
    NTEST_EXPECT_LE_UINT(worst, 3U);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
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

// A view's own budget of the cache's rays (NphRenderView.gi_rays; ADR-0079): none takes the
// renderer's, any other the lattice it makes over the target, at most that many. On the tests'
// 64-pixel target the renderer's 65,536 is every pixel, so the views ask for fewer.
NTEST_F(Gi, a_view_takes_its_own_budget_of_rays)
{
    REQUIRE_GI(f);
    NphRenderView v;
    test_box_view(&v, test_camera(), NPH_RENDER_VIEW_LIT);
    v.gi = 1;
    const u32 budgets[3] = {0U, 256U, 1024U};
    u32 traced[3] = {0U, 0U, 0U};
    for (u32 b = 0; b < 3U; ++b) {
        v.gi_rays = budgets[b];
        const u32 asked = budgets[b] != 0U ? budgets[b] : f->renderer->desc.gi_rays;
        const u32 stride = render_gi_stride(TEST_TARGET_SIZE, TEST_TARGET_SIZE, asked);
        const u32 across = (TEST_TARGET_SIZE + stride - 1U) / stride;
        const NphRenderFrameStats s = test_render_frame(f, &v, f->target);
        NTEST_EXPECT_TRUE(s.gi);
        NTEST_EXPECT_EQ_UINT(s.gi_rays, across * across);
        NTEST_EXPECT_LE_UINT(s.gi_rays, asked);
        traced[b] = s.gi_rays;
    }
    NTEST_EXPECT_EQ_UINT(traced[0], TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    NTEST_EXPECT_LT_UINT(traced[1], traced[2]);
    NTEST_EXPECT_LT_UINT(traced[2], traced[0]);
    v.gi = 0;
    NTEST_EXPECT_EQ_UINT(test_render_frame(f, &v, f->target).gi_rays, 0U);
}
