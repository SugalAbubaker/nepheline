// SPDX-License-Identifier: Apache-2.0
// Ambient occlusion (render/renderer.h NphRenderView.ao_radius; docs/render.md
// "Ambient occlusion"; engine/render/src/ao.c), against a ground truth the
// test computes itself: each pixel's surface found by casting its ray at the
// scene's boxes, and the cosine-weighted visibility of its hemisphere
// integrated by stratified rays against them, occluders fading over the last
// part of the radius as the shader's do. And how it shades: the
// environment's light only, the diffuse part with what the occluders
// reflect back (render/reference.h nph_render_ref_ao_bounce).
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>

#include "render_internal.h"
#include "test_render_support.h"

#include <math.h>

typedef RenderFixture Ao;

NTEST_SETUP(Ao)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Ao)
{
    test_render_teardown(f);
}

// The views' size, and the ground truth's samples per axis (S x S
// cosine-weighted directions a pixel).
#define SIZE    128U
#define PIXELS  ((u64)SIZE * SIZE * 4U)
#define STRATA  24U
#define RADIUS  0.5F
#define FALLOFF 0.1F // the shader's AO_FALLOFF

#define REQUIRE_PRODUCTION(f)                                                                      \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_production_supported((f)->renderer)) {                                   \
            NTEST_SKIP("no production path on this device");                                       \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// The ground truth
// ---------------------------------------------------------------------------

typedef struct AoBox {
    NphVec3 lo;
    NphVec3 hi;
} AoBox;

typedef struct AoScene {
    AoBox boxes[4];
    NphRenderInstance instances[4];
    u32 count;
    NphVec3 eye;
    NphVec3 forward;
    NphVec3 right;
    NphVec3 up;
    f32 tan_half; // tan(yfov / 2)
} AoScene;

static f32 axis_of(NphVec3 v, u32 i)
{
    return i == 0U ? v.x : (i == 1U ? v.y : v.z);
}

// The nearest entry of a ray into a box beyond `t_min`, and the face's
// outward normal there (the slab test; Kay & Kajiya 1986).
static bool ray_box(NphVec3 o, NphVec3 d, const AoBox *b, f32 t_min, f32 *t_out, NphVec3 *n_out)
{
    f32 t0 = t_min;
    f32 t1 = 1e30F;
    u32 axis = 3U;
    f32 sign = 0.0F;
    for (u32 i = 0; i < 3U; ++i) {
        const f32 oi = axis_of(o, i);
        const f32 di = axis_of(d, i);
        const f32 lo = axis_of(b->lo, i);
        const f32 hi = axis_of(b->hi, i);
        if (nph_f32_abs(di) < 1e-12F) {
            if (oi < lo || oi > hi) {
                return false;
            }
            continue;
        }
        f32 a = (lo - oi) / di;
        f32 c = (hi - oi) / di;
        f32 s = -1.0F; // entering through the low face
        if (a > c) {
            const f32 tmp = a;
            a = c;
            c = tmp;
            s = 1.0F;
        }
        if (a > t0) {
            t0 = a;
            axis = i;
            sign = s;
        }
        t1 = nph_f32_min(t1, c);
        if (t0 > t1) {
            return false;
        }
    }
    if (axis == 3U) {
        return false; // the ray starts inside
    }
    *t_out = t0;
    *n_out = nph_vec3(axis == 0U ? sign : 0.0F, axis == 1U ? sign : 0.0F, axis == 2U ? sign : 0.0F);
    return true;
}

static bool scene_hit(const AoScene *s, NphVec3 o, NphVec3 d, f32 t_min, f32 *t, NphVec3 *n)
{
    bool hit = false;
    *t = 1e30F;
    for (u32 i = 0; i < s->count; ++i) {
        f32 ti = 0.0F;
        NphVec3 ni;
        if (ray_box(o, d, &s->boxes[i], t_min, &ti, &ni) && ti < *t) {
            *t = ti;
            *n = ni;
            hit = true;
        }
    }
    return hit;
}

// The occlusion at `x` with normal `n`: the cosine-weighted share of the
// hemisphere left open, an occluder at distance d counting
// clamp((1 - d / RADIUS) / FALLOFF, 0, 1).
static f32 truth_at(const AoScene *s, NphVec3 x, NphVec3 n)
{
    const NphVec3 t = nph_vec3_normalize(nph_vec3_perpendicular(n));
    const NphVec3 b = nph_vec3_cross(n, t);
    const NphVec3 o = nph_vec3_add(x, nph_vec3_scale(n, 1e-4F));
    f32 open = 0.0F;
    for (u32 i = 0; i < STRATA; ++i) {
        for (u32 j = 0; j < STRATA; ++j) {
            const f32 u1 = ((f32)i + 0.5F) / (f32)STRATA;
            const f32 u2 = ((f32)j + 0.5F) / (f32)STRATA;
            const f32 r = nph_f32_sqrt(u1);
            const f32 phi = 2.0F * NPH_PI * u2;
            const NphVec3 d = nph_vec3_add(nph_vec3_add(nph_vec3_scale(t, r * nph_f32_cos(phi)),
                                                        nph_vec3_scale(b, r * nph_f32_sin(phi))),
                                           nph_vec3_scale(n, nph_f32_sqrt(1.0F - u1)));
            f32 dist = 0.0F;
            NphVec3 hn;
            f32 occluded = 0.0F;
            if (scene_hit(s, o, d, 0.0F, &dist, &hn)) {
                occluded = nph_f32_clamp((1.0F - dist / RADIUS) / FALLOFF, 0.0F, 1.0F);
            }
            open += 1.0F - occluded;
        }
    }
    return open / (f32)(STRATA * STRATA);
}

// The ground truth's occlusion of each pixel (-1 where its ray meets
// nothing).
static void truth_image(const AoScene *s, f32 *out)
{
    for (u32 y = 0; y < SIZE; ++y) {
        for (u32 x = 0; x < SIZE; ++x) {
            const f32 sx = (2.0F * ((f32)x + 0.5F) / (f32)SIZE - 1.0F) * s->tan_half;
            const f32 sy = (1.0F - 2.0F * ((f32)y + 0.5F) / (f32)SIZE) * s->tan_half;
            const NphVec3 d = nph_vec3_normalize(nph_vec3_add(
                nph_vec3_add(s->forward, nph_vec3_scale(s->right, sx)), nph_vec3_scale(s->up, sy)));
            f32 t = 0.0F;
            NphVec3 n;
            out[y * SIZE + x] = scene_hit(s, s->eye, d, 0.0F, &t, &n)
                                    ? truth_at(s, nph_vec3_add(s->eye, nph_vec3_scale(d, t)), n)
                                    : -1.0F;
        }
    }
}

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------

static NphRenderMesh cube_mesh(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

static NphRenderMaterial grey(RenderFixture *f, f32 value)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(value, value, value, 1.0F);
    md.roughness = 1.0F;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

// A box of the scene, drawn and known to the ground truth.
static void add_box(RenderFixture *f, AoScene *s, NphRenderMesh mesh, NphRenderMaterial mat,
                    NphVec3 lo, NphVec3 hi)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    const NphVec3 centre = nph_vec3_scale(nph_vec3_add(lo, hi), 0.5F);
    id.transform = nph_render_transform_from_trs(nph_dvec3_from_vec3(centre), nph_quat_identity(),
                                                 nph_vec3_sub(hi, lo));
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &s->instances[s->count]),
                        NPH_RESULT_OK);
    s->boxes[s->count].lo = lo;
    s->boxes[s->count].hi = hi;
    ++s->count;
}

// The ground: a slab 40 m across with its top at y = 0.
static void add_ground(RenderFixture *f, AoScene *s, NphRenderMesh mesh, NphRenderMaterial mat)
{
    add_box(f, s, mesh, mat, nph_vec3(-20.0F, -1.0F, -20.0F), nph_vec3(20.0F, 0.0F, 20.0F));
}

// The camera, for the renderer and the ground truth alike.
static NphRenderCamera scene_camera(AoScene *s, NphVec3 eye, NphVec3 target, f32 yfov)
{
    s->eye = eye;
    s->forward = nph_vec3_normalize(nph_vec3_sub(target, eye));
    s->right = nph_vec3_normalize(nph_vec3_cross(s->forward, nph_vec3_unit_y()));
    s->up = nph_vec3_cross(s->right, s->forward);
    s->tan_half = nph_f32_tan(0.5F * yfov);
    return nph_render_camera_look_at(nph_dvec3_from_vec3(eye), nph_dvec3_from_vec3(target),
                                     nph_vec3_unit_y(), yfov, 0.05F);
}

// A unit box on the ground, seen from above and in front.
static NphRenderCamera box_on_ground(RenderFixture *f, AoScene *s, NphRenderMesh mesh,
                                     NphRenderMaterial mat)
{
    memset(s, 0, sizeof *s);
    add_ground(f, s, mesh, mat);
    add_box(f, s, mesh, mat, nph_vec3(-0.5F, 0.0F, -0.5F), nph_vec3(0.5F, 1.0F, 0.5F));
    return scene_camera(s, nph_vec3(2.2F, 2.0F, 3.2F), nph_vec3(0.0F, 0.3F, 0.0F), 0.8F);
}

static void show(RenderFixture *f, const AoScene *s, bool shown)
{
    for (u32 i = 0; i < s->count; ++i) {
        nph_render_instance_set_hidden(f->renderer, s->instances[i], !shown);
    }
}

// An environment of the same radiance in every direction.
static NphRenderTexture uniform_environment(RenderFixture *f, f32 radiance)
{
    static u16 faces[6U * 2U * 2U * 4U];
    for (u64 i = 0; i < 24U; ++i) {
        faces[i * 4U] = nph_f32_to_f16(radiance);
        faces[i * 4U + 1U] = faces[i * 4U];
        faces[i * 4U + 2U] = faces[i * 4U];
        faces[i * 4U + 3U] = nph_f32_to_f16(1.0F);
    }
    const void *mips[1] = {faces};
    const u64 sizes[1] = {sizeof faces};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.kind = NPH_RENDER_TEXTURE_CUBE;
    td.format = NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    td.width = 2U;
    td.height = 2U;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    NphRenderTexture tex = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &tex), NPH_RESULT_OK);
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_CUBE;
    ed.texture = tex;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    return tex;
}

// The production path's view of `camera` showing `debug` on black, the
// radiance times 1 / 1.2 (EV100 0) and not tonemapped.
static void ao_view(NphRenderView *v, NphRenderCamera camera, NphRenderDebugView debug, f32 radius)
{
    nph_render_view_default(v, camera);
    v->pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
    v->ev100 = 0.0F;
    v->debug_view = debug;
    v->ao_radius = radius;
}

static NphRenderFrameStats render_into(RenderFixture *f, const NphRenderView *view,
                                       NphGpuImage target, u8 *out)
{
    const NphRenderFrameStats s = test_render_frame(f, view, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, out, PIXELS);
    return s;
}

// Pixel `i`'s first channel, linear: a UNORM target holds it sRGB encoded,
// as test_frame.c reads it.
static f32 linear_at(const u8 *image, u32 i)
{
    return nph_render_ref_srgb_to_linear((f32)image[(u64)i * 4U] / 255.0F);
}

static void release(RenderFixture *f, NphGpuImage target)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_gpu_image_destroy(f->dev, target);
}

// ---------------------------------------------------------------------------
// Against the ground truth
// ---------------------------------------------------------------------------

// A box on the ground: along its foot the occlusion comes within a quarter
// of how much the ground truth is occluded, and the ground that nothing
// occludes stays open. (Behind the box's silhouette the depth cannot tell
// how thick it is, and a few pixels there are occluded as if it went on:
// screen-space occlusion's limit.)
NTEST_F(Ao, the_occlusion_comes_near_the_ground_truth)
{
    REQUIRE_PRODUCTION(f);
    static AoScene scene;
    const NphRenderCamera camera = box_on_ground(f, &scene, cube_mesh(f), grey(f, 0.5F));
    static f32 truth[SIZE * SIZE];
    truth_image(&scene, truth);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    ao_view(&v, camera, NPH_RENDER_VIEW_OCCLUSION, RADIUS);
    NTEST_EXPECT_TRUE(render_into(f, &v, target, image).ao);
    f64 err = 0.0;
    f64 shut = 0.0;
    f64 bias = 0.0;
    u32 occluded = 0U;
    f64 open_err = 0.0;
    u32 open = 0U;
    u32 open_off = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        const f32 t = truth[i];
        if (t < 0.0F) {
            continue;
        }
        const f32 e = linear_at(image, i) - t;
        if (t < 0.9F) {
            err += (f64)nph_f32_abs(e);
            shut += (f64)(1.0F - t);
            bias += (f64)e;
            ++occluded;
        } else if (t >= 0.999F) {
            open_err += (f64)nph_f32_abs(e);
            open_off += nph_f32_abs(e) > 0.03F ? 1U : 0U;
            ++open;
        }
    }
    NTEST_LOG("occluded: %u pixels, error %.3f of the occlusion, bias %+.4f; open: %u pixels, "
              "mean error %.4f, %.1f%% off by over 0.03",
              occluded, err / shut, bias / (f64)occluded, open, open_err / (f64)open,
              100.0 * (f64)open_off / (f64)open);
    NTEST_ASSERT_GT_UINT(occluded, 500U);
    NTEST_ASSERT_GT_UINT(open, 5000U);
    NTEST_EXPECT_TRUE(err < 0.25 * shut);
    NTEST_EXPECT_TRUE(nph_f64_abs(bias / (f64)occluded) < 0.08);
    NTEST_EXPECT_TRUE(open_err / (f64)open < 0.01);
    NTEST_EXPECT_TRUE((f64)open_off < 0.02 * (f64)open);
    release(f, target);
}

// The ground alone occludes nothing, out to its horizon and at the image's
// edges: the samples along a slice lie on the plane however steeply it is
// seen (ao_common.glsl ao_depth_at), and a window at the edge still holds
// every direction (ao_denoise.comp).
NTEST_F(Ao, flat_ground_stays_open)
{
    REQUIRE_PRODUCTION(f);
    static AoScene scene;
    memset(&scene, 0, sizeof scene);
    add_ground(f, &scene, cube_mesh(f), grey(f, 0.5F));
    const NphRenderCamera camera =
        scene_camera(&scene, nph_vec3(0.0F, 1.5F, 4.0F), nph_vec3(0.0F, 0.0F, -2.0F), 0.8F);
    static f32 truth[SIZE * SIZE];
    truth_image(&scene, truth);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 image[PIXELS];
    NphRenderView v;
    ao_view(&v, camera, NPH_RENDER_VIEW_OCCLUSION, RADIUS);
    NTEST_EXPECT_TRUE(render_into(f, &v, target, image).ao);
    u32 ground = 0U;
    f32 worst = 0.0F;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        if (truth[i] >= 0.0F) {
            NTEST_EXPECT_TRUE(truth[i] > 0.999F);
            worst = nph_f32_max(worst, 1.0F - linear_at(image, i));
            ++ground;
        }
    }
    NTEST_LOG("%u ground pixels, the most occluded %.4f", ground, (f64)worst);
    NTEST_ASSERT_GT_UINT(ground, SIZE * SIZE / 2U);
    NTEST_EXPECT_TRUE(worst < 0.02F);
    release(f, target);
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------

// The diffuse light under occlusion A takes back what occluders of the
// surface's albedo reflect (Jimenez et al. 2016 Eq. 10). Without occlusion
// a pixel's environment light is S + albedo K: the specular part S the same
// for either grey, the diffuse part proportional to the albedo, so two
// albedos give both. With it the pixel shows A S + b albedo K, the specular
// part occluded by A (at roughness 1 surface.glsl's specular occlusion is
// A) and the diffuse part by b, which nph_render_ref_ao_bounce gives: more
// than A for the bright grey, about A for the dark one.
NTEST_F(Ao, the_diffuse_light_takes_back_what_the_occluders_reflect)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderTexture env = uniform_environment(f, 1.0F);
    const NphRenderMesh mesh = cube_mesh(f);
    static const f32 albedo[2] = {0.8F, 0.2F};
    static AoScene scenes[2];
    // The same boxes twice, seen alike.
    (void)box_on_ground(f, &scenes[1], mesh, grey(f, albedo[1]));
    const NphRenderCamera camera = box_on_ground(f, &scenes[0], mesh, grey(f, albedo[0]));
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 occlusion[PIXELS];
    static u8 open[2][PIXELS];
    static u8 shut[2][PIXELS];
    NphRenderView v;
    for (u32 k = 0; k < 2U; ++k) {
        show(f, &scenes[k], true);
        show(f, &scenes[1U - k], false);
        if (k == 0U) {
            ao_view(&v, camera, NPH_RENDER_VIEW_OCCLUSION, RADIUS);
            NTEST_EXPECT_TRUE(render_into(f, &v, target, occlusion).ao);
        }
        ao_view(&v, camera, NPH_RENDER_VIEW_INDIRECT, 0.0F);
        NTEST_EXPECT_FALSE(render_into(f, &v, target, open[k]).ao);
        ao_view(&v, camera, NPH_RENDER_VIEW_INDIRECT, RADIUS);
        NTEST_EXPECT_TRUE(render_into(f, &v, target, shut[k]).ao);
    }
    f64 err[2] = {0.0, 0.0};
    f64 gain[2] = {0.0, 0.0};
    u32 count = 0U;
    for (u32 i = 0; i < SIZE * SIZE; ++i) {
        // Occluded pixels, with room for the bounce to show.
        const f32 a = linear_at(occlusion, i);
        if (occlusion[(u64)i * 4U] == 0U || a < 0.3F || a > 0.85F) {
            continue;
        }
        const f32 k_diffuse =
            (linear_at(open[0], i) - linear_at(open[1], i)) / (albedo[0] - albedo[1]);
        const f32 specular = linear_at(open[0], i) - albedo[0] * k_diffuse;
        for (u32 k = 0; k < 2U; ++k) {
            const f32 b = (linear_at(shut[k], i) - a * specular) / (albedo[k] * k_diffuse);
            const f32 expected = nph_render_ref_ao_bounce(a, nph_vec3(albedo[k], 0.0F, 0.0F)).x;
            err[k] += (f64)nph_f32_abs(b - expected);
            gain[k] += (f64)(b - a);
        }
        ++count;
    }
    NTEST_LOG("%u pixels: bright grey off by %.4f, above the occlusion by %.3f; dark grey off "
              "by %.4f, above it by %.3f",
              count, err[0] / (f64)count, gain[0] / (f64)count, err[1] / (f64)count,
              gain[1] / (f64)count);
    NTEST_ASSERT_GT_UINT(count, 200U);
    NTEST_EXPECT_TRUE(err[0] / (f64)count < 0.015);
    NTEST_EXPECT_TRUE(err[1] / (f64)count < 0.02);
    NTEST_EXPECT_TRUE(gain[0] / (f64)count > 0.1);
    release(f, target);
    nph_render_texture_destroy(f->renderer, env);
}

// It darkens only the environment's light: lit by a sun alone, a view
// makes no occlusion and shows the frame it shows without; lit by the
// environment too, it is nowhere brighter and darker along the box's foot.
NTEST_F(Ao, it_darkens_only_the_environments_light)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderTexture env = uniform_environment(f, 1.0F);
    static AoScene scene;
    const NphRenderCamera camera = box_on_ground(f, &scene, cube_mesh(f), grey(f, 0.5F));
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 1.0F, 1.0F);
    ld.intensity = 1.5F;
    ld.direction = nph_vec3_normalize(nph_vec3(-0.3F, -1.0F, -0.5F));
    NphRenderLight sun = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &sun), NPH_RESULT_OK);
    const NphGpuImage target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, SIZE);
    static u8 without[PIXELS];
    static u8 with[PIXELS];
    NphRenderView v;
    ao_view(&v, camera, NPH_RENDER_VIEW_LIT, 0.0F);
    v.disable_ibl = 1;
    NTEST_EXPECT_FALSE(render_into(f, &v, target, without).ao);
    v.ao_radius = RADIUS;
    NTEST_EXPECT_FALSE(render_into(f, &v, target, with).ao);
    NTEST_EXPECT_EQ_INT(memcmp(without, with, PIXELS), 0);

    v.disable_ibl = 0;
    v.ao_radius = 0.0F;
    NTEST_EXPECT_FALSE(render_into(f, &v, target, without).ao);
    v.ao_radius = RADIUS;
    NTEST_EXPECT_TRUE(render_into(f, &v, target, with).ao);
    u32 brighter = 0U;
    u32 darker = 0U;
    for (u64 i = 0; i < PIXELS; ++i) {
        brighter += with[i] > without[i] ? 1U : 0U;
        darker += with[i] + 4U < without[i] ? 1U : 0U;
    }
    NTEST_LOG("%u channels darker by over 4, %u brighter", darker, brighter);
    NTEST_EXPECT_EQ_UINT(brighter, 0U);
    NTEST_EXPECT_GT_UINT(darker, 1000U);
    release(f, target);
    nph_render_light_destroy(f->renderer, sun);
    nph_render_texture_destroy(f->renderer, env);
}

// ---------------------------------------------------------------------------
// Which frames have it
// ---------------------------------------------------------------------------

// The production path makes the occlusion for the views that show the
// environment's light, while there is an environment, and for its own
// debug view; never for the reference renderer or a radius of 0.
NTEST_F(Ao, views_that_show_it_make_it)
{
    REQUIRE_PRODUCTION(f);
    NphRenderView v;
    ao_view(&v, test_camera(), NPH_RENDER_VIEW_LIT, RADIUS);
    // No environment yet (black): only the occlusion's own view.
    for (u32 d = 0; d < (u32)NPH_RENDER_VIEW_COUNT; ++d) {
        v.debug_view = (NphRenderDebugView)d;
        NTEST_EXPECT_EQ_INT(test_render_frame(f, &v, f->target).ao,
                            d == (u32)NPH_RENDER_VIEW_OCCLUSION ? 1 : 0);
    }
    const NphRenderTexture env = uniform_environment(f, 1.0F);
    for (u32 d = 0; d < (u32)NPH_RENDER_VIEW_COUNT; ++d) {
        v.debug_view = (NphRenderDebugView)d;
        const bool shown = d == (u32)NPH_RENDER_VIEW_LIT || d == (u32)NPH_RENDER_VIEW_OCCLUSION ||
                           d == (u32)NPH_RENDER_VIEW_INDIRECT;
        NTEST_EXPECT_EQ_INT(test_render_frame(f, &v, f->target).ao, shown ? 1 : 0);
    }
    v.debug_view = NPH_RENDER_VIEW_LIT;
    v.disable_ibl = 1;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    v.disable_ibl = 0;
    v.ao_radius = 0.0F;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    v.ao_radius = RADIUS;
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    v.debug_view = NPH_RENDER_VIEW_OCCLUSION;
    NTEST_EXPECT_FALSE(test_render_frame(f, &v, f->target).ao);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_render_texture_destroy(f->renderer, env);
}

// The radius is a length: finite and not negative.
NTEST_F(Ao, the_radius_must_be_finite_and_not_negative)
{
    REQUIRE_RENDERER(f);
    const f32 bad[3] = {-0.5F, (f32)NAN, NPH_F32_INFINITY};
    NphRenderView v;
    ao_view(&v, test_camera(), NPH_RENDER_VIEW_LIT, RADIUS);
    for (u32 i = 0; i < 3U; ++i) {
        v.ao_radius = bad[i];
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        nph_gpu_rg_begin(f->graph);
        NTEST_EXPECT_EQ_INT(nph_render_frame(f->renderer, &v, f->graph, f->target, NULL, NULL),
                            NPH_RESULT_INVALID_ARGUMENT);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
}
