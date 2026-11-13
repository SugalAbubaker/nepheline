// SPDX-License-Identifier: Apache-2.0
// Global illumination (render/renderer.h NphRenderView.gi; docs/render.md
// "Global illumination"; engine/render/src/gi.c), against ground truths the
// test computes itself by casting rays at the scene's boxes (test_boxes.h):
// the irradiance each pixel's surface receives, read through the irradiance
// debug view once the radiance cache has had frames to fill. And the cache's
// keys against their CPU twins.
#include <nepheline/math/random.h>

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

// A card over the whole ground, above the camera, under an environment of
// one radiance. Opaque, it hides nearly all the sky from the ground. Its
// material alpha tested with an alpha under the cutoff, it is cut away
// everywhere: the cache's rays pass it (rt_mask.glsl), and the ground's
// irradiance comes back to what it is with the card hidden. The card is
// flat, which no ray met while a flat mesh's place in the top level had a
// zero for its thickness (rt.c).
NTEST_F(Gi, a_cut_away_surface_hides_no_sky)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, -2.0F), 0.8F);
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 4U, 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh plane = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.0F, 0.0F, 0.0F, 0.0F);
    md.double_sided = 1;
    NphRenderMaterial black = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &black), NPH_RESULT_OK);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = plane;
    id.materials = &black;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 3.0, 0.0), nph_quat_identity(),
                                                 nph_vec3(60.0F, 60.0F, 60.0F));
    NphRenderInstance card = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &card), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 under[PIXELS];
    static u8 through[PIXELS];
    static u8 open[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    // Opaque first, so that the light the cells then hold has to come through
    // the card once it is cut away.
    render_frames(f, &v, target, under, 2U * FRAMES);
    md.alpha_mode = NPH_RENDER_ALPHA_MASK;
    NTEST_ASSERT_EQ_INT(nph_render_material_update(f->renderer, black, &md), NPH_RESULT_OK);
    // (Long enough for the cells to leave their dark behind: where light arrived by few paths,
    // as under the card's edges, the cells are slow to take a change for one, ADR-0090.)
    render_frames(f, &v, target, through, 6U * FRAMES);
    nph_render_instance_set_hidden(f->renderer, card, true);
    render_frames(f, &v, target, open, 6U * FRAMES);
    f64 sum[3] = {0.0, 0.0, 0.0};
    u32 ground = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        if (pixel_point(&scene, i, &x, &n) >= 0) {
            sum[0] += (f64)test_box_linear(under, i);
            sum[1] += (f64)test_box_linear(through, i);
            sum[2] += (f64)test_box_linear(open, i);
            ++ground;
        }
    }
    NTEST_LOG("%u ground pixels: irradiance under the opaque card %.4f, the card cut away %.4f, "
              "no card %.4f",
              ground, sum[0] / (f64)ground, sum[1] / (f64)ground, sum[2] / (f64)ground);
    NTEST_ASSERT_GT_UINT(ground, SIZE * SIZE / 2U);
    NTEST_EXPECT_TRUE(sum[2] > 0.0);
    NTEST_EXPECT_TRUE(sum[0] < 0.25 * sum[2]);
    NTEST_EXPECT_TRUE(nph_f64_abs(sum[1] / sum[2] - 1.0) < 0.03);
    nph_render_instance_destroy(f->renderer, card);
    test_box_release(f, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_material_destroy(f->renderer, black);
    nph_render_mesh_destroy(f->renderer, plane);
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
static void sun_bounces_off(RenderFixture *f, NphRenderMaterial wall)
{
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMesh mesh = test_cube_mesh(f);
    test_box_ground(f, &scene, mesh, test_grey(f, 0.0F));
    test_box_add(f, &scene, mesh, wall, nph_vec3(-1.0F, 0.0F, -0.2F), nph_vec3(1.0F, 1.5F, 0.2F));
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

NTEST_F(Gi, the_suns_light_bounces_off_a_wall)
{
    REQUIRE_GI(f);
    sun_bounces_off(f, test_grey(f, 1.0F));
}

// The same wall a rough metal: it reflects by its base colour as the white wall does, in a lobe
// where that one reflects evenly, and what a ray's hit sends on of the light it receives is its
// base colour's share whichever it is (gi_hit.glsl, ADR-0092): the ground before it has the
// white wall's light. Counted as reflecting nothing, as metals were, every surface of a scene
// whose materials keep their metalness in a texture (a factor of 1, the texture's value unread)
// bounced no light at all: a city's shade was lit by its sky alone.
NTEST_F(Gi, a_metal_wall_passes_the_suns_light_on)
{
    REQUIRE_GI(f);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    md.metallic = 1.0F;
    md.roughness = 1.0F;
    NphRenderMaterial metal = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &metal), NPH_RESULT_OK);
    sun_bounces_off(f, metal);
}

// ---------------------------------------------------------------------------
// The history (ADR-0084)
// ---------------------------------------------------------------------------

#define SLOTS (1U << 16U) // test_renderer_desc's gi_cells

// Each slot's sample count after the frames so far: its cell's whole count, beside its moments
// (the cell's word stops at its bits, ADR-0091).
static void read_counts(RenderFixture *f, u32 *counts)
{
    static u32 words[SLOTS * 4U];
    NTEST_ASSERT_EQ_UINT(f->renderer->gi.capacity, SLOTS);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, f->renderer->gi.moments, 0U, sizeof words, words);
    for (u32 i = 0; i < SLOTS; ++i) {
        counts[i] = words[i * 4U + 2U] >> 8U;
    }
}

// The frames a view is drawn before `early_counts` reads what its cells take a frame.
#define EARLY_FRAMES 8U

// The most samples any cell holds, and how many hold more than their history allows (none
// should): NPH_RENDER_GI_HISTORY samples, or NPH_RENDER_GI_HISTORY_FRAMES frames' where that is
// more, a frame's being what the cell took over the view's first EARLY_FRAMES (`early`: their
// counts then), and four deviations and a few samples for what a frame's count varies by.
static u32 most_samples(RenderFixture *f, const u32 *early, u32 *over)
{
    static u32 counts[SLOTS];
    read_counts(f, counts);
    u32 most = 0U;
    *over = 0U;
    for (u32 i = 0; i < SLOTS; ++i) {
        most = nph_max_u32(most, counts[i]);
        const f64 a_frame = (f64)early[i] / (f64)EARLY_FRAMES;
        const f64 allowed =
            nph_f64_max((f64)NPH_RENDER_GI_HISTORY,
                        (f64)NPH_RENDER_GI_HISTORY_FRAMES * (a_frame + 4.0 * sqrt(a_frame) + 4.0));
        *over += (f64)counts[i] > allowed ? 1U : 0U;
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
// 128 samples' 53 %; and the history is NPH_RENDER_GI_HISTORY_FRAMES frames' samples where
// those are more, ADR-0091: the view is drawn for more frames than that, so that a history
// that grew without bound would show); but their light's recent mean now leaves their
// estimates by far more than its spread allows, so the cells cut their histories down: past
// half the change in 4 frames, and within 4 % of the new light in 16 (the cells' means are
// 97 % of the way then, the cut histories having grown again from NPH_RENDER_GI_HISTORY_MIN
// samples; what the cells are held to be goes with them, gi_resolve.comp).
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
    render_frames(f, &v, target, image, EARLY_FRAMES);
    static u32 early[SLOTS];
    read_counts(f, early);
    render_frames(f, &v, target, image, 160U - EARLY_FRAMES);
    u32 over = 0U;
    const u32 most = most_samples(f, early, &over);
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
    NTEST_EXPECT_GE_UINT(most, (u32)NPH_RENDER_GI_HISTORY);
    NTEST_EXPECT_EQ_UINT(over, 0U);
    NTEST_EXPECT_NEAR_F64(before, dim_level, 0.02 * dim_level);
    NTEST_EXPECT_TRUE(soon - before > 0.6 * change);
    NTEST_EXPECT_NEAR_F64(after, bright_level, 0.04 * bright_level);
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
    render_frames(f, &v, target, image, EARLY_FRAMES);
    static u32 early[SLOTS];
    read_counts(f, early);
    render_frames(f, &v, target, image, 96U - EARLY_FRAMES);
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
    const u32 most = most_samples(f, early, &over);
    NTEST_LOG("%u half-shaded pixels: they wander by %.3f of a 128-sample window's level; "
              "histories of up to %u samples",
              shaded, wander / window, most);
    NTEST_ASSERT_GT_UINT(shaded, 1000U);
    NTEST_EXPECT_TRUE(wander < 0.6 * window);
    NTEST_EXPECT_GE_UINT(most, (u32)NPH_RENDER_GI_HISTORY);
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
// The cells of the finer level the ground's near half has come into take their parents' light
// from the frame that made them, the coarser level's cells that were the ground's own until now
// (gi_filter.comp, ADR-0090), and are trusted as those are; the ground keeps its light against
// the same view settled. Otherwise the environment's light, which the roof hides, made up what
// new cells lacked: rings of light followed the camera across shaded ground.
NTEST_F(Gi, a_surface_entering_a_finer_level_keeps_its_light)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.6F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial grey = test_grey(f, 0.5F);
    test_box_ground(f, &scene, mesh, grey);
    // (A black roof: what the ground's rays meet on its underside reflects nothing. A grey one's
    // light would be what the cache takes an unseen surface to receive, ADR-0090, which the two
    // cameras here see differently.)
    test_box_add(f, &scene, mesh, test_grey(f, 0.0F), nph_vec3(-3.0F, 1.0F, -7.0F),
                 nph_vec3(3.0F, 1.2F, -2.0F));
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
        const u32 base =
            ((u32)h & (gi->capacity / (u32)NPH_RENDER_GI_BUCKET - 1U)) * (u32)NPH_RENDER_GI_BUCKET;
        const u32 fp = (u32)(h >> 32U) | 1U;
        for (u32 s = base; s < base + (u32)NPH_RENDER_GI_BUCKET; ++s) {
            if (keys[s] == fp) {
                ++newcomers;
                const u32 count = cells[(u64)s * 4U + 3U] & ((1U << NPH_RENDER_GI_COUNT_BITS) - 1U);
                seeded += count >= (u32)NPH_RENDER_GI_TRUSTED ? 1U : 0U;
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
              "%.4f); %u of their pixels in the finer level's cells, %u of those cells lent "
              "their parents' light",
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

// The roof of the tests below, and whether pixel `i` of the view shows the ground under it, away
// from its edges.
static void roofed_ground(RenderFixture *f, TestBoxScene *scene)
{
    memset(scene, 0, sizeof *scene);
    const NphRenderMesh mesh = test_cube_mesh(f);
    test_box_ground(f, scene, mesh, test_grey(f, 0.5F));
    // (Black: what the ground's rays meet on its underside reflects nothing.)
    test_box_add(f, scene, mesh, test_grey(f, 0.0F), nph_vec3(-3.0F, 1.0F, -7.0F),
                 nph_vec3(3.0F, 1.2F, -2.0F));
}

static bool roof_shades(const TestBoxScene *scene, u32 i)
{
    NphVec3 x;
    NphVec3 n;
    return pixel_point(scene, i, &x, &n) == 0 && n.y >= 0.5F && nph_f32_abs(x.x) <= 2.0F &&
           x.z <= -2.5F && x.z >= -6.5F;
}

// A view the frames have never seen: none of its cells is there, and the frame that comes to it
// gives each a ray or none. Its cells begin with the light of the cells above them, which the
// same rays filled, the first level of them whose cells know their light by their samples
// (gi_filter.comp, ADR-0091): the shade under a roof is lit as the settled view from its first
// frame, a pixel a fifth of the light out (the view here has a ray a pixel and sees the ground
// at so low an angle that most of its cells have none). Begun with their own samples' means, a
// ray or two's, the cells are each the sky's light or none, some 250 % of the shade's out a
// pixel, and the next frames' samples move them by as much: the flicker of a view at a run.
NTEST_F(Gi, a_new_view_is_lit_from_its_first_frame)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.6F);
    static TestBoxScene scene;
    roofed_ground(f, &scene);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 first[PIXELS];
    static u8 settled[PIXELS];
    NphRenderView v;
    // The open ground behind, for a while; then the roof.
    const NphVec3 eye = nph_vec3(0.0F, 0.6F, 2.5F);
    gi_view(&v, test_box_camera(&scene, eye, nph_vec3(0.0F, 0.0F, 9.0F), 0.6F));
    render_frames(f, &v, target, first, FRAMES);
    gi_view(&v, test_box_camera(&scene, eye, nph_vec3(0.0F, 0.0F, -4.0F), 0.6F));
    render_frames(f, &v, target, first, 1U);
    render_frames(f, &v, target, settled, 2U * FRAMES);
    f64 sum_first = 0.0;
    f64 sum_settled = 0.0;
    f64 apart = 0.0;
    u32 shaded = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        if (!roof_shades(&scene, i)) {
            continue;
        }
        const f64 a = (f64)test_box_linear(first, i);
        const f64 b = (f64)test_box_linear(settled, i);
        sum_first += a;
        sum_settled += b;
        apart += nph_f64_abs(a - b);
        ++shaded;
    }
    NTEST_ASSERT_GT_UINT(shaded, 500U);
    const f64 open = 0.6 * (f64)EXPOSURE;
    NTEST_LOG("%u shaded pixels: %.4f in the view's first frame, %.4f settled (the open sky's "
              "%.4f); a pixel %.1f %% of the settled light out",
              shaded, sum_first / (f64)shaded, sum_settled / (f64)shaded, open,
              100.0 * apart / sum_settled);
    NTEST_EXPECT_TRUE(sum_settled / (f64)shaded < 0.5 * open); // the roof shades it
    NTEST_EXPECT_NEAR_F64(sum_first, sum_settled, 0.1 * sum_settled);
    NTEST_EXPECT_TRUE(apart < 0.3 * sum_settled);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// A camera at a run, with the rays a cell has in a real view (a ray to 16 pixels, a handful a
// cell a frame): every cell near it is new, or has a few frames' samples. Two frames drawn at
// the last pose differ by what the frame's samples moved the light: little, the cells being
// held to what their samples, their neighbours' and their parents' make out together, of which
// one frame's samples are a small share (gi_filter.comp, ADR-0091): 1.3 % of the light a
// pixel here. Where what a cell is held to be goes with its mean every frame, 4 %; and in a
// view at a run, whose cells near the camera are a frame or two old, the shade flickered by a
// fifth from one frame to the next.
NTEST_F(Gi, a_moving_cameras_frames_hold_their_light)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.6F);
    static TestBoxScene scene;
    roofed_ground(f, &scene);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 one[PIXELS];
    static u8 other[PIXELS];
    NphRenderView v;
    // Sideways past the roof's open end, 0.2 m a frame, looking under it.
    NphVec3 eye = nph_vec3(-2.4F, 0.6F, 2.5F);
    for (u32 k = 0; k < 24U; ++k) {
        eye.x += 0.2F;
        gi_view(&v, test_box_camera(&scene, eye, nph_vec3(eye.x, 0.0F, -4.0F), 0.6F));
        v.gi_rays = SIZE * SIZE / 16U;
        NTEST_EXPECT_TRUE(test_render_frame(f, &v, target).gi);
    }
    render_frames(f, &v, target, one, 1U);
    render_frames(f, &v, target, other, 1U);
    f64 sum = 0.0;
    f64 apart = 0.0;
    u32 shaded = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        if (!roof_shades(&scene, i)) {
            continue;
        }
        const f64 a = (f64)test_box_linear(one, i);
        const f64 b = (f64)test_box_linear(other, i);
        sum += 0.5 * (a + b);
        apart += nph_f64_abs(a - b);
        ++shaded;
    }
    NTEST_ASSERT_GT_UINT(shaded, 500U);
    NTEST_LOG("%u shaded pixels at %.4f: two frames at one pose, after 24 moving, differ by "
              "%.2f %% of it a pixel",
              shaded, sum / (f64)shaded, 100.0 * apart / sum);
    NTEST_EXPECT_TRUE(apart < 0.025 * sum);
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// A run past more cells than the table holds: a camera low over open ground, a metre on each
// frame, makes thousands of new cells a frame, and those behind it keep their slots for
// NPH_RENDER_GI_MAX_AGE frames. When their buckets fill, the cells the samples no longer reach
// give their slots up, the oldest first (gi_common.glsl gi_claim_cell_within, ADR-0091), and
// the frame's samples all find their cells. Without, a table once full dropped a quarter of a
// frame's samples in a run past a wall, and the wall's pixels found no cell: they took the
// sky's light, or none.
NTEST_F(Gi, a_full_table_makes_room_for_the_frames_cells)
{
    REQUIRE_GI(f);
    const NphRenderTexture env = test_uniform_environment(f, 0.5F);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    NphRenderView v;
    NphVec3 eye = nph_vec3(-60.0F, 1.5F, 0.0F);
    u32 dropped = 0U;
    u32 rays = 0U;
    const u32 frames = 120U;
    for (u32 k = 0; k < frames; ++k) {
        eye.x += 1.0F;
        gi_view(&v, test_box_camera(&scene, eye, nph_vec3(eye.x, 0.0F, -4.0F), 0.6F));
        const NphRenderFrameStats st = test_render_frame(f, &v, target);
        NTEST_EXPECT_TRUE(st.gi);
        dropped += st.vis_gi_dropped;
        rays += st.gi_rays;
    }
    const RenderGi *gi = &f->renderer->gi;
    static u32 slots[SLOTS];
    NTEST_ASSERT_EQ_UINT(gi->capacity, SLOTS);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, gi->keys, 0U, sizeof slots, slots);
    u32 used = 0U;
    for (u32 i = 0; i < SLOTS; ++i) {
        used += slots[i] != 0U ? 1U : 0U;
    }
    NTEST_LOG("%u frames, a metre on each: %u of %u slots hold a cell; %u of %u samples found "
              "no slot",
              frames, used, (u32)SLOTS, dropped, rays);
    // (The table did fill: the frames made several times its cells.)
    NTEST_EXPECT_TRUE((f64)used > 0.9 * (f64)SLOTS);
    NTEST_EXPECT_TRUE((f64)dropped < 0.002 * (f64)rays);
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
// The filter (ADR-0090)
// ---------------------------------------------------------------------------

// Black ground under a wide black roof a metre up, under an environment of one radiance: what
// reaches the ground is the strip of sky under the roof's edges, a few hundredths of its
// hemisphere, so a sample is pi L or, nearly always, nothing, and a cell's own mean after a
// thousand of them is still a tenth or more out, each cell its own way. The cells' own means
// (NphRenderView.gi_smooth off) against the filter's, both against the truth stratified rays
// measure: the filter leaves a fraction of the mottling, and the shade as bright as its samples
// make it (weights that follow each cell's own variance left it half as bright: the cells a
// path of light had reached counted for less).
typedef struct RoofResult {
    u32 shaded;
    f64 truth;    // the open share of the sky, the shaded pixels' mean
    f64 own;      // the cells' own means there
    f64 own_err;  // and their mean distance from the truth a pixel, over the truth
    f64 filtered; // the same of the filter's
    f64 filtered_err;
    f64 cells_own;      // the cells' own means' luminance, summed by their samples' counts
    f64 cells_filtered; // and the filter's
    u32 cells;
    u32 cut; // cells whose history is under the whole
} RoofResult;

// The light arriving at `x` on a surface facing `n`, over the open sky's: `strata` x `strata`
// paths that leave in cosine-weighted directions and bounce off the boxes, which reflect `albedo`
// of what they receive evenly, until they leave for the sky (or 16 bounces have left under 1 % of
// a path's light). With black boxes, the share of the sky open to the point.
static f64 roof_truth(const TestBoxScene *s, NphVec3 x, NphVec3 n, f32 albedo, u32 strata,
                      NphPcg32 *rng)
{
    f64 sum = 0.0;
    for (u32 a = 0; a < strata; ++a) {
        for (u32 b = 0; b < strata; ++b) {
            NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
            NphVec3 d = test_cosine_direction(n, a, b, strata);
            f64 weight = 1.0;
            for (u32 bounce = 0; bounce < 16U; ++bounce) {
                f32 t = 0.0F;
                NphVec3 hn;
                if (test_box_hit(s, o, d, 0.0F, &t, &hn) < 0) {
                    sum += weight;
                    break;
                }
                weight *= (f64)albedo;
                if (weight <= 0.0) {
                    break;
                }
                o = nph_vec3_add(nph_vec3_add(o, nph_vec3_scale(d, t)), nph_vec3_scale(hn, 1e-4F));
                // (Two statements: the order of two draws in one expression is the compiler's.)
                const u32 u = nph_pcg32_next_u32(rng) & 1023U;
                const u32 v = nph_pcg32_next_u32(rng) & 1023U;
                d = test_cosine_direction(hn, u, v, 1024U);
            }
        }
    }
    return sum / (f64)(strata * strata);
}

static void under_a_roof(RenderFixture *f, bool walled, f32 albedo, RoofResult *out)
{
    const f32 height = 1.0F;
    const f32 radiance = 0.6F;
    const NphRenderTexture env = test_uniform_environment(f, radiance);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMesh mesh = test_cube_mesh(f);
    const NphRenderMaterial black = test_grey(f, albedo);
    test_box_ground(f, &scene, mesh, black);
    test_box_add(f, &scene, mesh, black, nph_vec3(-3.0F, height, -7.0F),
                 nph_vec3(3.0F, height + 0.2F, -2.0F));
    if (walled) {
        // Walls on three sides: the sky shows through the side the camera looks in by alone.
        test_box_add(f, &scene, mesh, black, nph_vec3(-3.2F, 0.0F, -7.0F),
                     nph_vec3(-3.0F, height, -2.0F));
        test_box_add(f, &scene, mesh, black, nph_vec3(3.0F, 0.0F, -7.0F),
                     nph_vec3(3.2F, height, -2.0F));
        test_box_add(f, &scene, mesh, black, nph_vec3(-3.2F, 0.0F, -7.2F),
                     nph_vec3(3.2F, height, -7.0F));
        // And a curtain over the upper part of that side, which the camera looks in under.
        test_box_add(f, &scene, mesh, black, nph_vec3(-3.2F, 0.4F, -2.0F),
                     nph_vec3(3.2F, height, -1.9F));
    }
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 0.6F, 2.5F), nph_vec3(0.0F, 0.0F, -4.0F), 0.6F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 own[PIXELS];
    static u8 filtered[PIXELS];
    NphRenderView v;
    gi_view(&v, camera);
    // (Surfaces that reflect pass light between them, a bounce a frame, through histories of a
    // thousand samples: they take many times longer to settle.)
    const u32 settle = albedo > 0.0F ? 8U : 1U;
    v.gi_smooth = 0;
    render_frames(f, &v, target, own, settle * 4U * FRAMES);
    v.gi_smooth = 1;
    render_frames(f, &v, target, filtered, settle * 2U * FRAMES);
    memset(out, 0, sizeof *out);
    f64 own_err = 0.0;
    f64 filtered_err = 0.0;
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0x5EEDULL, 1ULL);
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        NphVec3 x;
        NphVec3 n;
        if (pixel_point(&scene, i, &x, &n) != 0 || n.y < 0.5F || nph_f32_abs(x.x) > 2.0F ||
            x.z > -2.5F || x.z < -6.5F) {
            continue;
        }
        // (Fewer paths where they bounce: each is many rays, and the bounces spread them.)
        const f64 truth =
            roof_truth(&scene, x, n, albedo, albedo > 0.0F ? 2U * STRATA : 6U * STRATA, &rng);
        const f64 a = (f64)test_box_linear(own, i) / (f64)(radiance * EXPOSURE);
        const f64 b = (f64)test_box_linear(filtered, i) / (f64)(radiance * EXPOSURE);
        out->truth += truth;
        out->own += a;
        out->filtered += b;
        own_err += fabs(a - truth);
        filtered_err += fabs(b - truth);
        ++out->shaded;
    }
    NTEST_EXPECT_GT_UINT(out->shaded, 1000U);
    out->own_err = own_err / out->truth;
    out->filtered_err = filtered_err / out->truth;
    out->truth /= (f64)out->shaded;
    out->own /= (f64)out->shaded;
    out->filtered /= (f64)out->shaded;
    // The cells themselves: the filter moves light between them and makes none.
    const RenderGi *gi = &f->renderer->gi;
    static u32 keys[SLOTS];
    static u32 raw[SLOTS * 4U];
    static u32 cells[SLOTS * 4U];
    NTEST_ASSERT_EQ_UINT(gi->capacity, SLOTS);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_buffer(f->dev, gi->keys, 0U, sizeof keys, keys);
    test_read_buffer(f->dev, gi->raw_cells, 0U, sizeof raw, raw);
    test_read_buffer(f->dev, gi->cells, 0U, sizeof cells, cells);
    for (u32 i = 0; i < SLOTS; ++i) {
        const u32 word = raw[(u64)i * 4U + 3U];
        const u32 count = word & ((1U << NPH_RENDER_GI_COUNT_BITS) - 1U);
        const u32 age = (word >> NPH_RENDER_GI_COUNT_BITS) & 0xFFU;
        if (keys[i] == 0U || count == 0U || age != 0U) {
            continue;
        }
        f32 e[3];
        f32 s[3];
        memcpy(e, &raw[(u64)i * 4U], sizeof e);
        memcpy(s, &cells[(u64)i * 4U], sizeof s);
        out->cells_own += (f64)count * (f64)(0.2126F * e[0] + 0.7152F * e[1] + 0.0722F * e[2]);
        out->cells_filtered += (f64)count * (f64)(0.2126F * s[0] + 0.7152F * s[1] + 0.0722F * s[2]);
        ++out->cells;
        out->cut += count < (u32)NPH_RENDER_GI_HISTORY ? 1U : 0U;
    }
    test_box_release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

static void log_roof(const char *what, const RoofResult *r)
{
    NTEST_LOG("%s: %u shaded pixels, %.4f of the sky open to them: the cells' own means %.4f (off "
              "by %.1f %% of it a pixel), filtered %.4f (%.1f %%); %u cells' light by their "
              "samples: filtered %.4f of their own; %u under a whole history",
              what, r->shaded, r->truth, r->own, 100.0 * r->own_err, r->filtered,
              100.0 * r->filtered_err, r->cells, r->cells_filtered / r->cells_own, r->cut);
}

NTEST_F(Gi, the_filter_keeps_the_light)
{
    REQUIRE_GI(f);
    RoofResult r;
    under_a_roof(f, false, 0.0F, &r);
    log_roof("a roof a metre up", &r);
    NTEST_EXPECT_TRUE(r.truth < 0.2);
    NTEST_EXPECT_TRUE(r.own_err > 0.04);
    NTEST_EXPECT_TRUE(r.filtered_err < 0.7 * r.own_err);
    NTEST_EXPECT_NEAR_F64(r.filtered, r.truth, 0.02 * r.truth);
    NTEST_EXPECT_NEAR_F64(r.cells_filtered, r.cells_own, 0.01 * r.cells_own);
}

NTEST_F(Gi, the_filter_keeps_the_light_of_few_paths)
{
    REQUIRE_GI(f);
    RoofResult r;
    under_a_roof(f, true, 0.0F, &r);
    log_roof("the roof walled on three sides", &r);
    NTEST_EXPECT_TRUE(r.truth < 0.03);
    NTEST_EXPECT_NEAR_F64(r.own, r.truth, 0.06 * r.truth);
    NTEST_EXPECT_TRUE(r.own_err < 0.27);
    NTEST_EXPECT_NEAR_F64(r.filtered, r.truth, 0.05 * r.truth);
    NTEST_EXPECT_TRUE(r.filtered_err < 0.6 * r.own_err);
    NTEST_EXPECT_NEAR_F64(r.cells_filtered, r.cells_own, 0.01 * r.cells_own);
}

NTEST_F(Gi, the_filter_keeps_the_light_of_many_bounces)
{
    REQUIRE_GI(f);
    RoofResult r;
    under_a_roof(f, true, 0.7F, &r);
    log_roof("the walled roof, its surfaces reflecting 0.7", &r);
    NTEST_EXPECT_NEAR_F64(r.own, r.truth, 0.12 * r.truth);
    NTEST_EXPECT_NEAR_F64(r.filtered, r.truth, 0.12 * r.truth);
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
    static u32 expected[(1U + NPH_RENDER_GI_ABOVE) * SIZE * SIZE];
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
        // And the cells of the levels above around it, its parents, which take its samples
        // too (ADR-0090, ADR-0091).
        const i32 level = render_gi_level(nph_vec3_length(p));
        for (i32 up = 1; up <= NPH_RENDER_GI_ABOVE && level + up < NPH_RENDER_GI_LEVELS; ++up) {
            const u64 hp = render_gi_mix(render_gi_key_at(levels, p, n, level + up));
            expected[cells++] = (u32)(hp >> 32U) | 1U;
        }
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
    // (The rest are the cells of what the update's rays met where the cache held none,
    // ADR-0090: the box's and the ground's points that no pixel's point fell in.)
    NTEST_EXPECT_TRUE((f64)known >= 0.9 * (f64)used);
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
    // Level 9 holds 2 to 4 m, its cells 1/24 m across (1/48 to 1/96 of that), and all that is
    // nearer: no cell is finer (NPH_RENDER_GI_LEVEL_MIN, ADR-0091).
    NTEST_EXPECT_EQ_INT(NPH_RENDER_GI_LEVEL_MIN, 9);
    NTEST_EXPECT_NEAR_F64((f64)levels[NPH_RENDER_GI_LEVEL_MIN].origin.w, 1.0 / 24.0, 1e-6);
    NTEST_EXPECT_EQ_INT(render_gi_level(2.0F), 9);
    NTEST_EXPECT_EQ_INT(render_gi_level(3.99F), 9);
    NTEST_EXPECT_EQ_INT(render_gi_level(4.0F), 10);
    NTEST_EXPECT_EQ_INT(render_gi_level(1.0F), 9);
    NTEST_EXPECT_EQ_INT(render_gi_level(0.5F), 9);
    NTEST_EXPECT_EQ_INT(render_gi_level(1e-9F), 9);
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
