// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scenes of axis-aligned boxes that the renderer draws and the tests cast
// rays at themselves (the slab test), for ground truths computed on the CPU
// (test_ao.c, test_gi.c): the boxes, a camera both agree on, the camera's
// ray through each pixel, and the production view the tests read back.

#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>

#include "test_render_support.h"

// The views' size.
#define TEST_BOX_SIZE   128U
#define TEST_BOX_PIXELS ((u64)TEST_BOX_SIZE * TEST_BOX_SIZE * 4U)

#define REQUIRE_PRODUCTION(f)                                                                      \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_production_supported((f)->renderer)) {                                   \
            NTEST_SKIP("no production path on this device");                                       \
        }                                                                                          \
    } while (0)

typedef struct TestBox {
    NphVec3 lo;
    NphVec3 hi;
} TestBox;

typedef struct TestBoxScene {
    TestBox boxes[4];
    NphRenderInstance instances[4];
    u32 count;
    NphVec3 eye;
    NphVec3 forward;
    NphVec3 right;
    NphVec3 up;
    f32 tan_half; // tan(yfov / 2)
} TestBoxScene;

static inline f32 test_box_axis(NphVec3 v, u32 i)
{
    return i == 0U ? v.x : (i == 1U ? v.y : v.z);
}

// The nearest entry of a ray into a box beyond `t_min`, and the face's
// outward normal there (the slab test; Kay & Kajiya 1986).
static inline bool test_ray_box(NphVec3 o, NphVec3 d, const TestBox *b, f32 t_min, f32 *t_out,
                                NphVec3 *n_out)
{
    f32 t0 = t_min;
    f32 t1 = 1e30F;
    u32 axis = 3U;
    f32 sign = 0.0F;
    for (u32 i = 0; i < 3U; ++i) {
        const f32 oi = test_box_axis(o, i);
        const f32 di = test_box_axis(d, i);
        const f32 lo = test_box_axis(b->lo, i);
        const f32 hi = test_box_axis(b->hi, i);
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

// The nearest box a ray meets beyond `t_min` (its index; -1 for none), the
// distance, and the face's outward normal.
static inline i32 test_box_hit(const TestBoxScene *s, NphVec3 o, NphVec3 d, f32 t_min, f32 *t,
                               NphVec3 *n)
{
    i32 hit = -1;
    *t = 1e30F;
    for (u32 i = 0; i < s->count; ++i) {
        f32 ti = 0.0F;
        NphVec3 ni;
        if (test_ray_box(o, d, &s->boxes[i], t_min, &ti, &ni) && ti < *t) {
            *t = ti;
            *n = ni;
            hit = (i32)i;
        }
    }
    return hit;
}

// The direction of the camera's ray through the centre of pixel `i`.
static inline NphVec3 test_box_pixel_ray(const TestBoxScene *s, u32 i)
{
    const u32 column = i % TEST_BOX_SIZE;
    const u32 row = i / TEST_BOX_SIZE;
    const f32 sx = (2.0F * ((f32)column + 0.5F) / (f32)TEST_BOX_SIZE - 1.0F) * s->tan_half;
    const f32 sy = (1.0F - 2.0F * ((f32)row + 0.5F) / (f32)TEST_BOX_SIZE) * s->tan_half;
    return nph_vec3_normalize(nph_vec3_add(nph_vec3_add(s->forward, nph_vec3_scale(s->right, sx)),
                                           nph_vec3_scale(s->up, sy)));
}

// The direction of stratum (i, j) of `strata` x `strata` in the
// cosine-weighted hemisphere around `n` (Malley's method from the stratum's
// centre): every stratum stands for an equal share of the cosine.
static inline NphVec3 test_cosine_direction(NphVec3 n, u32 i, u32 j, u32 strata)
{
    const NphVec3 t = nph_vec3_normalize(nph_vec3_perpendicular(n));
    const NphVec3 b = nph_vec3_cross(n, t);
    const f32 u1 = ((f32)i + 0.5F) / (f32)strata;
    const f32 u2 = ((f32)j + 0.5F) / (f32)strata;
    const f32 r = nph_f32_sqrt(u1);
    const f32 phi = 2.0F * NPH_PI * u2;
    return nph_vec3_add(nph_vec3_add(nph_vec3_scale(t, r * nph_f32_cos(phi)),
                                     nph_vec3_scale(b, r * nph_f32_sin(phi))),
                        nph_vec3_scale(n, nph_f32_sqrt(1.0F - u1)));
}

// ---------------------------------------------------------------------------
// Drawing them
// ---------------------------------------------------------------------------

static inline NphRenderMesh test_cube_mesh(RenderFixture *f)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

// A rough dielectric grey (roughness 1).
static inline NphRenderMaterial test_grey(RenderFixture *f, f32 value)
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
static inline void test_box_add(RenderFixture *f, TestBoxScene *s, NphRenderMesh mesh,
                                NphRenderMaterial mat, NphVec3 lo, NphVec3 hi)
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
static inline void test_box_ground(RenderFixture *f, TestBoxScene *s, NphRenderMesh mesh,
                                   NphRenderMaterial mat)
{
    test_box_add(f, s, mesh, mat, nph_vec3(-20.0F, -1.0F, -20.0F), nph_vec3(20.0F, 0.0F, 20.0F));
}

// The camera, for the renderer and the ground truth alike.
static inline NphRenderCamera test_box_camera(TestBoxScene *s, NphVec3 eye, NphVec3 target,
                                              f32 yfov)
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
static inline NphRenderCamera test_box_on_ground(RenderFixture *f, TestBoxScene *s,
                                                 NphRenderMesh mesh, NphRenderMaterial ground,
                                                 NphRenderMaterial box)
{
    memset(s, 0, sizeof *s);
    test_box_ground(f, s, mesh, ground);
    test_box_add(f, s, mesh, box, nph_vec3(-0.5F, 0.0F, -0.5F), nph_vec3(0.5F, 1.0F, 0.5F));
    return test_box_camera(s, nph_vec3(2.2F, 2.0F, 3.2F), nph_vec3(0.0F, 0.3F, 0.0F), 0.8F);
}

static inline void test_box_show(RenderFixture *f, const TestBoxScene *s, bool shown)
{
    for (u32 i = 0; i < s->count; ++i) {
        nph_render_instance_set_hidden(f->renderer, s->instances[i], !shown);
    }
}

// An environment of the same radiance in every direction.
static inline NphRenderTexture test_uniform_environment(RenderFixture *f, f32 radiance)
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
static inline void test_box_view(NphRenderView *v, NphRenderCamera camera, NphRenderDebugView debug)
{
    nph_render_view_default(v, camera);
    v->pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v->tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
    v->ev100 = 0.0F;
    v->debug_view = debug;
}

static inline NphRenderFrameStats test_box_render(RenderFixture *f, const NphRenderView *view,
                                                  NphGpuImage target, u8 *out)
{
    const NphRenderFrameStats s = test_render_frame(f, view, target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, target, 0U, out, TEST_BOX_PIXELS);
    return s;
}

// Pixel `i`'s first channel, linear: a UNORM target holds it sRGB encoded,
// as test_frame.c reads it.
static inline f32 test_box_linear(const u8 *image, u32 i)
{
    return nph_render_ref_srgb_to_linear((f32)image[(u64)i * 4U] / 255.0F);
}

static inline void test_box_release(RenderFixture *f, NphGpuImage target)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    nph_gpu_image_destroy(f->dev, target);
}
