// SPDX-License-Identifier: Apache-2.0
// The many lights (render/renderer.h NphRenderManyLight; docs/render.md "Many lights";
// many_lights.c; ADR-0073): the near fields against ordinary lights of that range, the
// far fields against a ground truth the test computes by casting rays at the scene's
// boxes (test_boxes.h), what a change does, and the walk on the GPU against its CPU
// twin.
#include <nepheline/math/random.h>
#include <nepheline/render/light_tree.h>

#include "render_internal.h"
#include "test_boxes.h"

#include <math.h>

typedef RenderFixture Many;

#define MANY_CAPACITY 512U

NTEST_SETUP(Many)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_lights = NPH_RENDER_MAX_LIGHTS;
    d.max_many_lights = MANY_CAPACITY;
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(Many)
{
    test_render_teardown(f);
}

#define SIZE   TEST_BOX_SIZE
#define PIXELS TEST_BOX_PIXELS
// EV100 0: radiance times 1 / 1.2.
#define EXPOSURE (1.0F / 1.2F)

static f32 next_in(NphPcg32 *r, f32 lo, f32 hi)
{
    return lo + (hi - lo) * nph_pcg32_next_f32(r);
}

// `count` positions in the box from `lo` to `hi`, the same every time.
static void scatter(NphVec3 *out, u32 count, NphVec3 lo, NphVec3 hi)
{
    NphPcg32 r;
    nph_pcg32_seed(&r, 7U, 11U);
    for (u32 i = 0; i < count; ++i) {
        // One draw to a statement: C leaves the order within one expression open.
        const f32 x = next_in(&r, lo.x, hi.x);
        const f32 y = next_in(&r, lo.y, hi.y);
        const f32 z = next_in(&r, lo.z, hi.z);
        out[i] = nph_vec3(x, y, z);
    }
}

static NphRenderManyLightDesc many_light(NphVec3 at, f32 intensity, f32 near_range)
{
    NphRenderManyLightDesc d;
    memset(&d, 0, sizeof d);
    d.position = nph_dvec3_from_vec3(at);
    d.color = nph_vec3(1.0F, 1.0F, 1.0F);
    d.intensity = intensity;
    d.near_range = near_range;
    return d;
}

static NphRenderLight plain_light(RenderFixture *f, NphVec3 at, f32 intensity, f32 range)
{
    NphRenderLightDesc d;
    memset(&d, 0, sizeof d);
    d.type = NPH_RENDER_LIGHT_TYPE_POINT;
    d.color = nph_vec3(1.0F, 1.0F, 1.0F);
    d.intensity = intensity;
    d.position = nph_dvec3_from_vec3(at);
    d.direction = nph_vec3(0.0F, -1.0F, 0.0F);
    d.range = range;
    NphRenderLight l = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &d, &l), NPH_RESULT_OK);
    return l;
}

static void lit_view(NphRenderView *v, NphRenderCamera camera)
{
    test_box_view(v, camera, NPH_RENDER_VIEW_LIT);
    v->disable_ibl = 1;
}

// ---------------------------------------------------------------------------
// The near fields
// ---------------------------------------------------------------------------

// Within its near range a many light is lit by the window a range gives, so that
// without the cache a set of them draws what ordinary lights of that range draw, bit
// for bit but for the target's rounding. That holds only if the table hands each
// pixel every light that reaches it, once: 400 lights in a table whose entries they
// must share.
NTEST_F(Many, the_near_fields_light_as_lights_of_that_range_do)
{
    REQUIRE_PRODUCTION(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 12.0F, 8.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.9F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    enum {
        COUNT = 400
    };
    static NphVec3 at[COUNT];
    scatter(at, COUNT, nph_vec3(-8.0F, 0.1F, -8.0F), nph_vec3(8.0F, 0.6F, 8.0F));
    NphRenderView v;
    lit_view(&v, camera);

    static NphRenderLight plain[COUNT];
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        plain[i] = plain_light(f, at[i], 0.4F, 0.75F);
    }
    static u8 ordinary[PIXELS];
    (void)test_render_frame(f, &v, target);
    (void)test_box_render(f, &v, target, ordinary);
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        nph_render_light_destroy(f->renderer, plain[i]);
    }

    static NphRenderManyLight many[COUNT];
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        const NphRenderManyLightDesc d = many_light(at[i], 0.4F, 0.75F);
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &many[i]), NPH_RESULT_OK);
    }
    static u8 near[PIXELS];
    (void)test_render_frame(f, &v, target);
    const NphRenderFrameStats st = test_box_render(f, &v, target, near);
    NTEST_EXPECT_EQ_UINT(st.many_lights, (u32)COUNT);
    NTEST_EXPECT_EQ_UINT(st.lights, 0U);

    u32 worst = 0U;
    u32 lit = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const u32 a = ordinary[(u64)i * 4U];
        const u32 b = near[(u64)i * 4U];
        worst = nph_max_u32(worst, a > b ? a - b : b - a);
        lit += a > 8U ? 1U : 0U;
    }
    NTEST_LOG("%u pixels lit; the near fields within %u levels of ordinary lights", lit, worst);
    NTEST_EXPECT_GT_UINT(lit, SIZE * SIZE / 8U);
    NTEST_EXPECT_LE_UINT(worst, 2U);

    // Off in the view: they draw nothing.
    v.disable_many_lights = 1;
    (void)test_render_frame(f, &v, target);
    const NphRenderFrameStats off = test_box_render(f, &v, target, near);
    NTEST_EXPECT_EQ_UINT(off.many_lights, 0U);
    u32 brightest = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        brightest = nph_max_u32(brightest, near[(u64)i * 4U]);
    }
    NTEST_EXPECT_EQ_UINT(brightest, 0U);
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        nph_render_many_light_destroy(f->renderer, many[i]);
    }
    test_box_release(f, target);
}

// A light whose cells share a table entry is in its list once, or a pixel there would
// light with it twice. The table has 1,024 entries for so few lights. Two lights, so
// that the set's origin is between them and the first can be anywhere about it: the
// first at a corner of a block of eight cells, two of which hash alike (never two
// neighbours along an axis: the hash multiplies by odd numbers, so they differ).
NTEST_F(Many, a_light_is_listed_once_in_an_entry_its_cells_share)
{
    REQUIRE_PRODUCTION(f);
    const f32 near_range = 0.1F;
    const f32 cell = 2.0F * near_range;
    const u32 mask = 1023U;
    bool found = false;
    NphVec3 at = nph_vec3_zero();
    for (i32 cz = -90; cz <= 89 && !found; ++cz) {
        for (i32 cx = -90; cx <= 89 && !found; ++cx) {
            u32 entries[8];
            u32 n = 0U;
            for (i32 y = -1; y <= 0; ++y) {
                for (i32 z = cz; z <= cz + 1; ++z) {
                    for (i32 x = cx; x <= cx + 1; ++x) {
                        entries[n++] = render_many_table_entry(x, y, z, mask);
                    }
                }
            }
            for (u32 a = 0; a < 8U && !found; ++a) {
                for (u32 b = a + 1U; b < 8U && !found; ++b) {
                    found = entries[a] == entries[b];
                }
            }
            if (found) {
                at = nph_vec3((f32)(cx + 1) * cell, 0.05F, (f32)(cz + 1) * cell);
            }
        }
    }
    NTEST_ASSERT_TRUE(found);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3_add(at, nph_vec3(0.0F, 0.5F, 0.2F)), at, 0.9F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    NphRenderView v;
    lit_view(&v, camera);
    const NphVec3 other = nph_vec3_neg(at);
    NphRenderLight plain[2] = {plain_light(f, at, 0.02F, near_range),
                               plain_light(f, other, 0.02F, near_range)};
    static u8 ordinary[PIXELS];
    (void)test_render_frame(f, &v, target);
    (void)test_box_render(f, &v, target, ordinary);
    nph_render_light_destroy(f->renderer, plain[0]);
    nph_render_light_destroy(f->renderer, plain[1]);
    const NphRenderManyLightDesc d0 = many_light(at, 0.02F, near_range);
    const NphRenderManyLightDesc d1 = many_light(other, 0.02F, near_range);
    NphRenderManyLight many[2];
    NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d0, &many[0]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d1, &many[1]), NPH_RESULT_OK);
    static u8 near[PIXELS];
    (void)test_render_frame(f, &v, target);
    (void)test_box_render(f, &v, target, near);
    u32 worst = 0U;
    u32 lit = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const u32 a = ordinary[(u64)i * 4U];
        const u32 b = near[(u64)i * 4U];
        worst = nph_max_u32(worst, a > b ? a - b : b - a);
        lit += a > 8U ? 1U : 0U;
    }
    NTEST_LOG("%u pixels lit; within %u levels of ordinary lights", lit, worst);
    NTEST_EXPECT_GT_UINT(lit, 100U);
    NTEST_EXPECT_LE_UINT(worst, 2U);
    nph_render_many_light_destroy(f->renderer, many[0]);
    nph_render_many_light_destroy(f->renderer, many[1]);
    test_box_release(f, target);
}

// ---------------------------------------------------------------------------
// The far fields
// ---------------------------------------------------------------------------

#define FAR_LIGHTS 64U
// Frames the cache fills over (a cell takes several samples a frame at this size).
#define FAR_FRAMES 96U
#define FAR_NEAR   0.5F
// About half of what the 8-bit target holds, at the ground.
#define FAR_INTENSITY 1.0F

// The irradiance at (x, n) from the lights the boxes let reach it: the sum a many light
// would be sampled for, each by its share beyond its near range.
static f32 far_irradiance(const TestBoxScene *s, const NphVec3 *lights, u32 count, f32 intensity,
                          f32 near_range, NphVec3 x, NphVec3 n)
{
    f64 sum = 0.0;
    const NphVec3 from = nph_vec3_add(x, nph_vec3_scale(n, 1e-3F));
    const f32 k = 1.0F / (near_range * near_range);
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 to = nph_vec3_sub(lights[i], from);
        const f32 d2 = nph_vec3_dot(to, to);
        const f32 d = nph_f32_sqrt(d2);
        const NphVec3 l = nph_vec3_scale(to, 1.0F / d);
        const f32 nol = nph_vec3_dot(n, l);
        if (nol <= 0.0F) {
            continue;
        }
        f32 t = 0.0F;
        NphVec3 hit_n;
        const i32 hit = test_box_hit(s, from, l, 0.0F, &t, &hit_n);
        if (hit >= 0 && t < d) {
            continue;
        }
        const f32 q = d2 * k;
        const f32 window = nph_f32_clamp(1.0F - q * q, 0.0F, 1.0F);
        sum += (f64)(intensity * (1.0F - window * window) * nol / d2);
    }
    return (f32)sum;
}

// The share of pixels of the ground and its boxes within `tolerance` of the truth,
// where the truth is above `floor`, and the mean of the error.
typedef struct FarError {
    u32 compared;
    u32 within;
    f64 mean;
    f64 signed_mean;
} FarError;

static FarError compare_far(const TestBoxScene *s, const NphVec3 *lights, const u8 *image,
                            f32 tolerance, f32 floor)
{
    FarError e;
    memset(&e, 0, sizeof e);
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(s, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(s, s->eye, d, 0.0F, &t, &n) < 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(s->eye, nph_vec3_scale(d, t));
        const f32 truth = far_irradiance(s, lights, FAR_LIGHTS, FAR_INTENSITY, FAR_NEAR, x, n) *
                          (1.0F / 3.14159265F) * EXPOSURE;
        if (truth < floor) {
            continue;
        }
        const f32 got = test_box_linear(image, i);
        const f32 err = (got - truth) / truth;
        e.compared += 1U;
        e.within += nph_f32_abs(err) <= tolerance ? 1U : 0U;
        e.mean += (f64)nph_f32_abs(err);
        e.signed_mean += (f64)err;
    }
    if (e.compared != 0U) {
        e.mean /= (f64)e.compared;
        e.signed_mean /= (f64)e.compared;
    }
    return e;
}

// Black surfaces, so that nothing bounces: what the cache holds is the far fields'
// irradiance alone, shadowed by the boxes, and the irradiance view shows it.
NTEST_F(Many, the_far_fields_arrive_shadowed_through_the_cache)
{
    REQUIRE_PRODUCTION(f);
    if (!f->renderer->gi.supported) {
        NTEST_SKIP("no radiance cache on this device (ray queries)");
    }
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMesh cube = test_cube_mesh(f);
    const NphRenderMaterial black = test_grey(f, 0.0F);
    test_box_ground(f, &scene, cube, black);
    test_box_add(f, &scene, cube, black, nph_vec3(-2.0F, 0.0F, -1.0F), nph_vec3(-0.5F, 2.0F, 1.0F));
    test_box_add(f, &scene, cube, black, nph_vec3(1.0F, 0.0F, 0.5F), nph_vec3(2.5F, 1.0F, 2.0F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 7.0F, 7.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[FAR_LIGHTS];
    scatter(at, FAR_LIGHTS, nph_vec3(-5.0F, 3.0F, -5.0F), nph_vec3(5.0F, 6.0F, 5.0F));
    static NphRenderManyLight many[FAR_LIGHTS];
    for (u32 i = 0; i < FAR_LIGHTS; ++i) {
        const NphRenderManyLightDesc d = many_light(at[i], FAR_INTENSITY, FAR_NEAR);
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &many[i]), NPH_RESULT_OK);
    }
    NphRenderView v;
    lit_view(&v, camera);
    v.debug_view = NPH_RENDER_VIEW_IRRADIANCE;
    v.gi = 1;
    for (u32 i = 0; i + 1U < FAR_FRAMES; ++i) {
        NTEST_EXPECT_TRUE(test_render_frame(f, &v, target).gi);
    }
    static u8 image[PIXELS];
    (void)test_box_render(f, &v, target, image);
    const FarError e = compare_far(&scene, at, image, 0.15F, 0.01F);
    NTEST_LOG("%u pixels: %.1f %% within 15 %% of the truth, mean error %.2f %%, bias %+.2f %%",
              e.compared, 100.0 * (f64)e.within / (f64)e.compared, 100.0 * e.mean,
              100.0 * e.signed_mean);
    NTEST_EXPECT_GT_UINT(e.compared, SIZE * SIZE / 2U);
    NTEST_EXPECT_GT_UINT(e.within, e.compared - e.compared / 10U);
    NTEST_EXPECT_TRUE(nph_f64_abs(e.signed_mean) < 0.05);
    for (u32 i = 0; i < FAR_LIGHTS; ++i) {
        nph_render_many_light_destroy(f->renderer, many[i]);
    }
    test_box_release(f, target);
}

// The partition of unity: near and far fields together are the light. Lights low over a
// rough ground, so that much of what a pixel gets is from within their near ranges and
// much from beyond, against the same lights as ordinary ones of unlimited range. Nothing
// bounces (the sky is black, and no surface faces another).
NTEST_F(Many, the_near_and_far_fields_add_up_to_the_light)
{
    REQUIRE_PRODUCTION(f);
    if (!f->renderer->gi.supported) {
        NTEST_SKIP("no radiance cache on this device (ray queries)");
    }
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 2.5F, 3.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.9F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    enum {
        COUNT = 48
    };
    static NphVec3 at[COUNT];
    scatter(at, COUNT, nph_vec3(-3.0F, 0.3F, -3.0F), nph_vec3(3.0F, 0.6F, 3.0F));
    NphRenderView v;
    lit_view(&v, camera);

    static NphRenderLight plain[COUNT];
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        plain[i] = plain_light(f, at[i], 0.02F, 0.0F);
    }
    static u8 ordinary[PIXELS];
    (void)test_box_render(f, &v, target, ordinary);
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        nph_render_light_destroy(f->renderer, plain[i]);
    }

    static NphRenderManyLight many[COUNT];
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        const NphRenderManyLightDesc d = many_light(at[i], 0.02F, 0.5F);
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &many[i]), NPH_RESULT_OK);
    }
    v.gi = 1;
    for (u32 i = 0; i + 1U < FAR_FRAMES; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static u8 split[PIXELS];
    (void)test_box_render(f, &v, target, split);
    f64 sum_err = 0.0;
    f64 sum_abs = 0.0;
    u32 count = 0U;
    u32 within = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const f32 a = test_box_linear(ordinary, i);
        const f32 b = test_box_linear(split, i);
        if (a < 0.02F || a > 0.95F) {
            continue; // the background, and what the target clips
        }
        const f32 err = (b - a) / a;
        sum_err += (f64)err;
        sum_abs += (f64)nph_f32_abs(err);
        within += nph_f32_abs(err) <= 0.2F ? 1U : 0U;
        ++count;
    }
    NTEST_ASSERT_GT_UINT(count, SIZE * SIZE / 4U);
    NTEST_LOG(
        "%u pixels: %.1f %% within 20 %% of the lights whole, mean error %.2f %%, bias %+.2f %%",
        count, 100.0 * (f64)within / (f64)count, 100.0 * sum_abs / (f64)count,
        100.0 * sum_err / (f64)count);
    NTEST_EXPECT_GT_UINT(within, count - count / 10U);
    NTEST_EXPECT_TRUE(nph_f64_abs(sum_err / (f64)count) < 0.05);
    for (u32 i = 0; i < (u32)COUNT; ++i) {
        nph_render_many_light_destroy(f->renderer, many[i]);
    }
    test_box_release(f, target);
}

// A metal has no diffuse to take the far fields' irradiance: what lights it beyond the
// near range is the cell's light taken as arriving evenly from everywhere, as a white
// furnace would: about the irradiance over pi.
NTEST_F(Many, a_metal_is_lit_by_the_far_fields)
{
    REQUIRE_PRODUCTION(f);
    if (!f->renderer->gi.supported) {
        NTEST_SKIP("no radiance cache on this device (ray queries)");
    }
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    md.metallic = 1.0F;
    md.roughness = 0.8F;
    NphRenderMaterial metal = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &metal), NPH_RESULT_OK);
    test_box_ground(f, &scene, test_cube_mesh(f), metal);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 7.0F, 7.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static NphVec3 at[FAR_LIGHTS];
    scatter(at, FAR_LIGHTS, nph_vec3(-5.0F, 3.0F, -5.0F), nph_vec3(5.0F, 6.0F, 5.0F));
    static NphRenderManyLight many[FAR_LIGHTS];
    for (u32 i = 0; i < FAR_LIGHTS; ++i) {
        const NphRenderManyLightDesc d = many_light(at[i], FAR_INTENSITY, FAR_NEAR);
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &many[i]), NPH_RESULT_OK);
    }
    NphRenderView v;
    lit_view(&v, camera);
    v.gi = 1;
    for (u32 i = 0; i + 1U < FAR_FRAMES; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    static u8 image[PIXELS];
    (void)test_box_render(f, &v, target, image);
    f64 ratio = 0.0;
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const NphVec3 d = test_box_pixel_ray(&scene, i);
        f32 t = 0.0F;
        NphVec3 n;
        if (test_box_hit(&scene, scene.eye, d, 0.0F, &t, &n) < 0) {
            continue;
        }
        const NphVec3 x = nph_vec3_add(scene.eye, nph_vec3_scale(d, t));
        const f32 truth = far_irradiance(&scene, at, FAR_LIGHTS, FAR_INTENSITY, FAR_NEAR, x, n) *
                          (1.0F / 3.14159265F) * EXPOSURE;
        if (truth > 0.02F) {
            ratio += (f64)(test_box_linear(image, i) / truth);
            ++count;
        }
    }
    NTEST_ASSERT_GT_UINT(count, SIZE * SIZE / 2U);
    NTEST_LOG("a metal against the irradiance over pi: %.3f", ratio / (f64)count);
    NTEST_EXPECT_TRUE(ratio / (f64)count > 0.5);
    NTEST_EXPECT_TRUE(ratio / (f64)count < 1.1);
    for (u32 i = 0; i < FAR_LIGHTS; ++i) {
        nph_render_many_light_destroy(f->renderer, many[i]);
    }
    test_box_release(f, target);
}

// A mesh of no rays stays drawn and casts nothing: the ground beside it gets what it
// would without it, and with rays it does not.
NTEST_F(Many, a_mesh_without_rays_is_drawn_and_casts_nothing)
{
    REQUIRE_PRODUCTION(f);
    if (!f->renderer->gi.supported) {
        NTEST_SKIP("no radiance cache on this device (ray queries)");
    }
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh with_rays = test_upload_mesh(f->renderer, &data);
    NphRenderSubmesh sub;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(&data, "no rays", &sub, &md);
    md.no_rays = 1;
    NphRenderMesh without = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &without), NPH_RESULT_OK);
    nph_render_mesh_data_free(&data);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    const NphRenderMaterial black = test_grey(f, 0.0F);
    test_box_ground(f, &scene, with_rays, black);
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 7.0F, 7.0F), nph_vec3(0.0F, 0.0F, 0.0F), 0.8F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    // A wall, and one light on the far side of it: the near side is in its shadow.
    const NphVec3 light = nph_vec3(0.0F, 2.0F, -3.0F);
    const NphRenderManyLightDesc d = many_light(light, 30.0F, FAR_NEAR);
    NphRenderManyLight many = NPH_RENDER_MANY_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &many), NPH_RESULT_OK);
    NphRenderView v;
    lit_view(&v, camera);
    v.debug_view = NPH_RENDER_VIEW_IRRADIANCE;
    v.gi = 1;

    f32 near_side[2];
    static u8 image[PIXELS];
    for (u32 round = 0; round < 2U; ++round) {
        NphRenderInstanceDesc id;
        memset(&id, 0, sizeof id);
        id.mesh = round == 0U ? with_rays : without;
        id.materials = &black;
        id.material_count = 1U;
        id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 2.0, 0.0), nph_quat_identity(),
                                                     nph_vec3(12.0F, 4.0F, 0.2F));
        NphRenderInstance wall = NPH_RENDER_INSTANCE_NULL;
        NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &wall), NPH_RESULT_OK);
        for (u32 i = 0; i < FAR_FRAMES; ++i) {
            (void)test_render_frame(f, &v, target);
        }
        (void)test_box_render(f, &v, target, image);
        // The ground a little on the near side of the wall, in the middle of the view.
        const NphVec3 ground = nph_vec3(0.0F, 0.0F, 2.0F);
        const NphVec3 to_eye = nph_vec3_sub(ground, scene.eye);
        f32 best = -1.0F;
        u32 best_i = 0U;
        for (u32 i = 0; i < SIZE * SIZE; ++i) {
            const f32 c = nph_vec3_dot(test_box_pixel_ray(&scene, i), nph_vec3_normalize(to_eye));
            if (c > best) {
                best = c;
                best_i = i;
            }
        }
        near_side[round] = test_box_linear(image, best_i);
        nph_render_instance_destroy(f->renderer, wall);
    }
    // In the wall's shadow only a mesh with rays can cast one.
    const f32 d2 = 5.0F * 5.0F + 2.0F * 2.0F;
    const f32 cosine = 2.0F / nph_f32_sqrt(d2);
    const f32 unshadowed = 30.0F * cosine / d2 * (1.0F / 3.14159265F) * EXPOSURE;
    NTEST_LOG("beyond the wall: with rays %.4f, without %.4f, the light unshadowed %.4f",
              (f64)near_side[0], (f64)near_side[1], (f64)unshadowed);
    NTEST_EXPECT_LT_UINT((u32)(near_side[0] * 1000.0F), (u32)(unshadowed * 100.0F));
    NTEST_EXPECT_TRUE(nph_f32_abs(near_side[1] - unshadowed) < unshadowed * 0.15F);
    nph_render_many_light_destroy(f->renderer, many);
    test_box_release(f, target);
}

// ---------------------------------------------------------------------------
// What a change does, and what is refused
// ---------------------------------------------------------------------------

NTEST_F(Many, a_change_rebuilds_the_set_and_the_last_to_go_leaves_none)
{
    REQUIRE_PRODUCTION(f);
    static TestBoxScene scene;
    memset(&scene, 0, sizeof scene);
    test_box_ground(f, &scene, test_cube_mesh(f), test_grey(f, 0.5F));
    const NphRenderCamera camera =
        test_box_camera(&scene, nph_vec3(0.0F, 6.0F, 0.01F), nph_vec3(0.0F, 0.0F, 0.0F), 0.9F);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    NphRenderManyLightDesc d = many_light(nph_vec3(-1.0F, 0.2F, 0.0F), 1.0F, 1.0F);
    NphRenderManyLight light = NPH_RENDER_MANY_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &light), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_render_many_light_is_valid(f->renderer, light));
    NphRenderView v;
    lit_view(&v, camera);
    static u8 image[PIXELS];
    const NphRenderFrameStats first = test_box_render(f, &v, target, image);
    NTEST_EXPECT_EQ_UINT(first.many_lights, 1U);
    // Which half of the image the light is over.
    u64 left = 0U;
    u64 right = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const u32 column = i % SIZE;
        (column < SIZE / 2U ? &left : &right)[0] += image[(u64)i * 4U];
    }
    NTEST_EXPECT_TRUE(left > 4U * right && left > 0U);

    // It moves: the next frame's set has it there.
    d.position = nph_dvec3(1.0, 0.2, 0.0);
    NTEST_ASSERT_EQ_INT(nph_render_many_light_update(f->renderer, light, &d), NPH_RESULT_OK);
    NphRenderManyLightDesc back;
    NTEST_ASSERT_EQ_INT(nph_render_many_light_desc(f->renderer, light, &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(back.position.x == 1.0);
    (void)test_box_render(f, &v, target, image);
    left = 0U;
    right = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const u32 column = i % SIZE;
        (column < SIZE / 2U ? &left : &right)[0] += image[(u64)i * 4U];
    }
    NTEST_EXPECT_TRUE(right > 4U * left && right > 0U);

    // It goes: none are left, and the handle is stale.
    nph_render_many_light_destroy(f->renderer, light);
    NTEST_EXPECT_FALSE(nph_render_many_light_is_valid(f->renderer, light));
    NTEST_EXPECT_EQ_INT(nph_render_many_light_update(f->renderer, light, &d),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_many_light_desc(f->renderer, light, &back),
                        NPH_RESULT_INVALID_ARGUMENT);
    const NphRenderFrameStats none = test_box_render(f, &v, target, image);
    NTEST_EXPECT_EQ_UINT(none.many_lights, 0U);
    u32 brightest = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        brightest = nph_max_u32(brightest, image[(u64)i * 4U]);
    }
    NTEST_EXPECT_EQ_UINT(brightest, 0U);
    test_box_release(f, target);
}

NTEST_F(Many, what_is_refused)
{
    if (f->dev == NULL) {
        NTEST_SKIP("no Vulkan device");
    }
    NphRenderManyLight l = NPH_RENDER_MANY_LIGHT_NULL;
    const NphRenderManyLightDesc good = many_light(nph_vec3_zero(), 1.0F, 1.0F);
    NphRenderManyLightDesc d = good;
    d.near_range = 0.0F;
    NTEST_EXPECT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &l),
                        NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.intensity = -1.0F;
    NTEST_EXPECT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &l),
                        NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.range = 0.5F; // below the near range
    NTEST_EXPECT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &l),
                        NPH_RESULT_INVALID_ARGUMENT);
    d = good;
    d.position.x = (f64)nph_f32_sqrt(-1.0F);
    NTEST_EXPECT_EQ_INT(nph_render_many_light_create(f->renderer, &d, &l),
                        NPH_RESULT_INVALID_ARGUMENT);
    static NphRenderManyLight all[MANY_CAPACITY];
    for (u32 i = 0; i < MANY_CAPACITY; ++i) {
        NTEST_ASSERT_EQ_INT(nph_render_many_light_create(f->renderer, &good, &all[i]),
                            NPH_RESULT_OK);
    }
    NTEST_EXPECT_EQ_INT(nph_render_many_light_create(f->renderer, &good, &l), NPH_RESULT_FULL);
    for (u32 i = 0; i < MANY_CAPACITY; ++i) {
        nph_render_many_light_destroy(f->renderer, all[i]);
    }
}

// A renderer without room for any pays for none, and refuses them.
NTEST(ManyNone, a_renderer_without_room_refuses_them)
{
    static RenderFixture fx;
    test_render_setup(&fx, NULL);
    if (fx.dev == NULL) {
        NTEST_SKIP("no Vulkan device");
    }
    const NphRenderManyLightDesc d = many_light(nph_vec3_zero(), 1.0F, 1.0F);
    NphRenderManyLight l = NPH_RENDER_MANY_LIGHT_NULL;
    NTEST_EXPECT_EQ_INT(nph_render_many_light_create(fx.renderer, &d, &l), NPH_RESULT_FULL);
    NTEST_EXPECT_FALSE(nph_render_many_light_is_valid(fx.renderer, l));
    test_render_teardown(&fx);
}

// ---------------------------------------------------------------------------
// The walk on the GPU against its CPU twin
// ---------------------------------------------------------------------------

// many_light_test.comp's input and output.
typedef struct WalkPush {
    u64 view;
    u64 cases;
    u64 results;
    u32 count;
    u32 pad;
} WalkPush;

NTEST_F(Many, the_walk_on_the_gpu_is_its_cpu_twin)
{
    if (f->dev == NULL) {
        NTEST_SKIP("no Vulkan device");
    }
    NphGpuDevice *dev = f->dev;
    enum {
        LIGHTS = 1500,
        CASES = 4096
    };
    static NphDVec3 positions[LIGHTS];
    static f32 powers[LIGHTS];
    NphPcg32 r;
    nph_pcg32_seed(&r, 3U, 5U);
    for (u32 i = 0; i < (u32)LIGHTS; ++i) {
        const f32 x = next_in(&r, -13.0F, 13.0F);
        const f32 y = next_in(&r, 0.0F, 10.0F);
        const f32 z = next_in(&r, -5.0F, 5.0F);
        positions[i] = nph_dvec3((f64)x, (f64)y, (f64)z);
        powers[i] = next_in(&r, 0.5F, 3.0F);
    }
    NphRenderLightTreeInput in;
    memset(&in, 0, sizeof in);
    in.positions = positions;
    in.powers = powers;
    in.count = LIGHTS;
    in.nearest = 0.5F;
    NphRenderLightTree tree;
    NTEST_ASSERT_EQ_INT(nph_render_light_tree_build(&in, nph_allocator_system(), &tree),
                        NPH_RESULT_OK);

    const u32 storage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    NphGpuBuffer nodes_buf =
        test_make_buffer(dev, (u64)tree.node_count * sizeof(NphRenderLightTreeNode), storage,
                         NPH_GPU_MEMORY_UPLOAD, "walk nodes");
    memcpy(nph_gpu_buffer_map(dev, nodes_buf), tree.nodes,
           (u64)tree.node_count * sizeof(NphRenderLightTreeNode));
    NphGpuBuffer view_buf = test_make_buffer(dev, sizeof(NphRenderManyView), storage,
                                             NPH_GPU_MEMORY_UPLOAD, "walk view");
    NphRenderManyView view;
    memset(&view, 0, sizeof view);
    view.nodes = nph_gpu_buffer_address(dev, nodes_buf);
    view.near2 = tree.nearest2;
    memcpy(nph_gpu_buffer_map(dev, view_buf), &view, sizeof view);
    NphGpuBuffer case_buf = test_make_buffer(dev, (u64)CASES * sizeof(NphVec4), storage,
                                             NPH_GPU_MEMORY_UPLOAD, "walk cases");
    NphGpuBuffer result_buf =
        test_make_buffer(dev, (u64)CASES * 8U, storage, NPH_GPU_MEMORY_READBACK, "walk results");
    NphVec4 *cases = (NphVec4 *)nph_gpu_buffer_map(dev, case_buf);
    for (u32 i = 0; i < (u32)CASES; ++i) {
        const f32 x = next_in(&r, -20.0F, 20.0F);
        const f32 y = next_in(&r, -2.0F, 14.0F);
        const f32 z = next_in(&r, -10.0F, 10.0F);
        cases[i] = nph_vec4(x, y, z, nph_pcg32_next_f32(&r));
    }

    NphGpuShader shader = test_load_shader(dev, "many_light_test.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    cd.name = "many light test";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(dev, &cd, &pipeline), NPH_RESULT_OK);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "many light walk", &cmd),
                        NPH_RESULT_OK);
    WalkPush push;
    memset(&push, 0, sizeof push);
    push.view = nph_gpu_buffer_address(dev, view_buf);
    push.cases = nph_gpu_buffer_address(dev, case_buf);
    push.results = nph_gpu_buffer_address(dev, result_buf);
    push.count = CASES;
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, ((u32)CASES + 63U) / 64U, 1U, 1U);
    test_submit_wait(dev, cmd);

    const u32 *results = (const u32 *)nph_gpu_buffer_map(dev, result_buf);
    u32 wrong_light = 0U;
    u32 wrong_pdf = 0U;
    for (u32 i = 0; i < (u32)CASES; ++i) {
        const NphVec3 x = nph_vec3(cases[i].x, cases[i].y, cases[i].z);
        f32 pdf = 0.0F;
        const u32 light = nph_render_light_tree_sample(&tree, x, cases[i].w, &pdf);
        const u32 got = results[(u64)i * 2U];
        f32 got_pdf = 0.0F;
        memcpy(&got_pdf, &results[(u64)i * 2U + 1U], sizeof got_pdf);
        // Rounding may differ by a hair where `u` lands on a choice's edge: a
        // neighbour along the curve then, at much the same probability.
        wrong_light += got != light ? 1U : 0U;
        wrong_pdf += got == light && nph_f32_abs(got_pdf - pdf) > pdf * 1e-3F ? 1U : 0U;
    }
    NTEST_LOG("%u walks: %u took another light, %u another probability", (u32)CASES, wrong_light,
              wrong_pdf);
    NTEST_EXPECT_LE_UINT(wrong_light, (u32)CASES / 500U);
    NTEST_EXPECT_EQ_UINT(wrong_pdf, 0U);

    nph_gpu_pipeline_destroy(dev, pipeline);
    nph_gpu_shader_destroy(dev, shader);
    nph_gpu_buffer_destroy(dev, nodes_buf);
    nph_gpu_buffer_destroy(dev, view_buf);
    nph_gpu_buffer_destroy(dev, case_buf);
    nph_gpu_buffer_destroy(dev, result_buf);
    nph_render_light_tree_free(&tree);
}
