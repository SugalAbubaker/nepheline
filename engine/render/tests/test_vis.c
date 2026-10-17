// SPDX-License-Identifier: Apache-2.0
// The production path (render/renderer.h NPH_RENDER_PIPELINE_PRODUCTION;
// docs/render.md "The production path"): the GPU-driven visibility-buffer
// renderer, checked against the reference renderer it must agree with,
// and against itself with its culling switched off, on another raster
// path, and far from the origin. The fixture's teardown asserts zero
// validation and synchronisation-validation errors and nothing leaked.
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture Vis;

NTEST_SETUP(Vis)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Vis)
{
    test_render_teardown(f);
}

#define REQUIRE_PRODUCTION(f)                                                                      \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_production_supported((f)->renderer)) {                                   \
            NTEST_SKIP("no production path on this device");                                       \
        }                                                                                          \
    } while (0)

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------

static NphRenderMesh mesh_of(RenderFixture *f, NphResult (*make)(NphRenderMeshData *))
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(make(&data), NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

static NphResult make_cube(NphRenderMeshData *out)
{
    return nph_render_primitive_cube(nph_allocator_system(), 1.0F, out);
}

static NphResult make_sphere(NphRenderMeshData *out)
{
    return nph_render_primitive_sphere(nph_allocator_system(), 0.5F, 24U, 12U, out);
}

static NphResult make_plane(NphRenderMeshData *out)
{
    return nph_render_primitive_plane(nph_allocator_system(), 1.0F, 4U, 1.0F, out);
}

static NphRenderMaterial material(RenderFixture *f, NphVec4 color, f32 metallic, f32 roughness)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = color;
    md.metallic = metallic;
    md.roughness = roughness;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

static NphRenderInstance instance(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
                                  NphRenderTransform t)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = t;
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

static NphRenderTransform at(f64 x, f64 y, f64 z, f32 scale)
{
    return nph_render_transform_from_trs(nph_dvec3(x, y, z), nph_quat_identity(),
                                         nph_vec3(scale, scale, scale));
}

// A lit scene around the origin: a cube, a sphere, and a floor, under a sun
// and the default sky, offset by `origin`.
typedef struct Scene {
    NphRenderMesh cube;
    NphRenderMesh sphere;
    NphRenderMesh plane;
    NphRenderMaterial red;
    NphRenderMaterial gold;
    NphRenderMaterial floor;
    NphRenderInstance instances[3];
    NphRenderLight sun;
} Scene;

static void scene_build(RenderFixture *f, Scene *s, NphDVec3 origin)
{
    memset(s, 0, sizeof *s);
    s->cube = mesh_of(f, make_cube);
    s->sphere = mesh_of(f, make_sphere);
    s->plane = mesh_of(f, make_plane);
    s->red = material(f, nph_vec4(0.8F, 0.1F, 0.1F, 1.0F), 0.0F, 0.6F);
    s->gold = material(f, nph_vec4(1.0F, 0.78F, 0.34F, 1.0F), 1.0F, 0.3F);
    s->floor = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F), 0.0F, 0.9F);
    NphRenderTransform t = at(origin.x - 0.6, origin.y, origin.z, 1.0F);
    t.linear = nph_mat3_mul(nph_mat3_from_quat(nph_quat_from_axis_angle(
                                nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F)), 0.6F)),
                            t.linear);
    s->instances[0] = instance(f, s->cube, s->red, t);
    s->instances[1] =
        instance(f, s->sphere, s->gold, at(origin.x + 0.7, origin.y + 0.1, origin.z, 1.2F));
    NphRenderTransform floor = at(origin.x, origin.y - 0.8, origin.z, 6.0F);
    s->instances[2] = instance(f, s->plane, s->floor, floor);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 0.95F, 0.9F);
    ld.intensity = 30000.0F;
    ld.direction = nph_vec3(-0.4F, -1.0F, -0.6F);
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &s->sun), NPH_RESULT_OK);
    NphRenderEnvironmentDesc env;
    memset(&env, 0, sizeof env);
    env.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&env.sky);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &env), NPH_RESULT_OK);
}

static void scene_view(NphRenderView *v, NphDVec3 origin, NphRenderPipeline pipeline)
{
    const NphRenderCamera c =
        nph_render_camera_look_at(nph_dvec3(origin.x + 0.4, origin.y + 1.2, origin.z + 3.5), origin,
                                  nph_vec3(0.0F, 1.0F, 0.0F), 0.9F, 0.05F);
    nph_render_view_default(v, c);
    v->ev100 = 13.0F;
    v->pipeline = pipeline;
}

// Renders `frames` frames and keeps the last image in `out`.
static NphRenderFrameStats render_into(RenderFixture *f, const NphRenderView *v, u32 frames,
                                       u8 *out)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < frames; ++i) {
        stats = test_render_read(f, v);
    }
    memcpy(out, f->pixels, PIXELS);
    return stats;
}

// How two images differ: the share of pixels whose channels all agree
// within `tol` levels, and the mean absolute difference over all channels.
typedef struct ImageDiff {
    f32 within;
    f32 mean;
    u32 max;
} ImageDiff;

static ImageDiff image_diff(const u8 *a, const u8 *b, u32 tol)
{
    ImageDiff d = {0.0F, 0.0F, 0U};
    u64 sum = 0U;
    u32 ok = 0U;
    for (u32 i = 0; i < TEST_TARGET_SIZE * TEST_TARGET_SIZE; ++i) {
        u32 worst = 0U;
        for (u32 c = 0; c < 3U; ++c) {
            const u32 x = a[i * 4U + c];
            const u32 y = b[i * 4U + c];
            const u32 e = x > y ? x - y : y - x;
            worst = nph_max_u32(worst, e);
            sum += e;
        }
        d.max = nph_max_u32(d.max, worst);
        ok += worst <= tol ? 1U : 0U;
    }
    d.within = (f32)ok / (f32)(TEST_TARGET_SIZE * TEST_TARGET_SIZE);
    d.mean = (f32)sum / (f32)(TEST_TARGET_SIZE * TEST_TARGET_SIZE * 3U);
    return d;
}

static u32 lit_pixels(const u8 *pixels, const u8 *background)
{
    u32 n = 0U;
    for (u32 i = 0; i < TEST_TARGET_SIZE * TEST_TARGET_SIZE; ++i) {
        n += memcmp(pixels + (u64)i * 4U, background + (u64)i * 4U, 3U) != 0 ? 1U : 0U;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The culling math against its C twins
// ---------------------------------------------------------------------------

static NphVec3 random_unit(NphPcg32 *rng)
{
    for (;;) {
        const NphVec3 v = nph_vec3(nph_pcg32_next_range_f32(rng, -1.0F, 1.0F),
                                   nph_pcg32_next_range_f32(rng, -1.0F, 1.0F),
                                   nph_pcg32_next_range_f32(rng, -1.0F, 1.0F));
        const f32 len = nph_vec3_length(v);
        if (len > 0.1F && len <= 1.0F) {
            return nph_vec3_scale(v, 1.0F / len);
        }
    }
}

// A random case for every test of vis_math_test.comp: spheres in view
// space (some reaching the near plane), cones, triangles (large ones, some
// behind the camera, and ones a pixel or two across), and HZB rectangles.
static NphVisMathCase random_case(NphPcg32 *rng, f32 width, f32 height, u32 i)
{
    NphVisMathCase c;
    memset(&c, 0, sizeof c);
    c.sphere = nph_vec4(
        nph_pcg32_next_range_f32(rng, -6.0F, 6.0F), nph_pcg32_next_range_f32(rng, -6.0F, 6.0F),
        nph_pcg32_next_range_f32(rng, -20.0F, 1.0F), nph_pcg32_next_range_f32(rng, 0.01F, 3.0F));
    const NphVec3 apex =
        nph_vec3_scale(random_unit(rng), nph_pcg32_next_range_f32(rng, 0.0F, 6.0F));
    c.cone = nph_vec4_from_vec3(apex, nph_pcg32_next_range_f32(rng, -1.0F, 1.0F));
    c.axis = nph_vec4_from_vec3(random_unit(rng), 0.0F);
    NphVec4 v[3];
    if ((i & 1U) == 0U) {
        for (u32 k = 0; k < 3U; ++k) {
            v[k] = nph_vec4(nph_pcg32_next_range_f32(rng, -3.0F, 3.0F),
                            nph_pcg32_next_range_f32(rng, -3.0F, 3.0F), 0.5F,
                            nph_pcg32_next_range_f32(rng, -0.5F, 3.0F));
        }
    } else {
        // Within a couple of pixels of a random point, in front.
        const f32 cx = nph_pcg32_next_range_f32(rng, -1.0F, 1.0F);
        const f32 cy = nph_pcg32_next_range_f32(rng, -1.0F, 1.0F);
        for (u32 k = 0; k < 3U; ++k) {
            const f32 w = nph_pcg32_next_range_f32(rng, 0.5F, 3.0F);
            const f32 dx = nph_pcg32_next_range_f32(rng, -1.5F, 1.5F) * 2.0F / width;
            const f32 dy = nph_pcg32_next_range_f32(rng, -1.5F, 1.5F) * 2.0F / height;
            v[k] = nph_vec4((cx + dx) * w, (cy + dy) * w, 0.5F * w, w);
        }
    }
    c.c0 = v[0];
    c.c1 = v[1];
    c.c2 = v[2];
    const f32 x0 = nph_pcg32_next_range_f32(rng, -0.1F, 1.0F);
    const f32 y0 = nph_pcg32_next_range_f32(rng, -0.1F, 1.0F);
    const f32 sx = nph_pcg32_next_range_f32(rng, 0.0F, 0.6F) * nph_pcg32_next_f32(rng);
    const f32 sy = nph_pcg32_next_range_f32(rng, 0.0F, 0.6F) * nph_pcg32_next_f32(rng);
    c.rect = nph_vec4(x0, y0, x0 + sx, y0 + sy);
    c.depth = nph_pcg32_next_f32(rng);
    c.mirrored = nph_pcg32_next_u32(rng) & 1U;
    // A light of every type, cones narrow to flat, and a view distance
    // from half the near plane to past the last bin.
    c.light_position = nph_vec4(
        nph_pcg32_next_range_f32(rng, -10.0F, 10.0F), nph_pcg32_next_range_f32(rng, -10.0F, 10.0F),
        nph_pcg32_next_range_f32(rng, -10.0F, 10.0F), nph_pcg32_next_range_f32(rng, 0.1F, 20.0F));
    c.light_direction = nph_vec4_from_vec3(random_unit(rng), (f32)(i % 3U));
    const f32 outer = nph_pcg32_next_range_f32(rng, 0.01F, 0.5F * NPH_PI);
    const f32 distance = 0.1F * nph_f32_pow(2.0F, nph_pcg32_next_range_f32(rng, -1.0F, 21.0F));
    c.light_cone = nph_vec4(nph_f32_cos(outer), nph_f32_sin(outer), distance, 0.0F);
    return c;
}

// How near vis_project_sphere_clipped's decisions come to switching (a
// bound from the tangent or from the near plane's circle, the camera
// inside the sphere or not), and the circle's conditioning; relative.
static f64 clip_margin(NphVec4 sphere, f32 znear)
{
    const f64 nz = -(f64)znear;
    const f64 r = (f64)sphere.w;
    const f64 z = (f64)sphere.z;
    f64 m = nph_f64_abs(z + r - nz);
    const f64 dz = nz - z;
    m = nph_f64_min(m, nph_f64_abs(r * r - dz * dz) / (r * r));
    for (u32 axis = 0; axis < 2U; ++axis) {
        const f64 a = axis == 0U ? (f64)sphere.x : (f64)sphere.y;
        const f64 t2 = a * a + z * z - r * r;
        m = nph_f64_min(m, nph_f64_abs(t2) / (a * a + z * z + r * r));
        if (t2 > 0.0) {
            const f64 len = nph_f64_sqrt(a * a + z * z);
            const f64 ct = nph_f64_sqrt(t2) / len;
            const f64 st = r / len;
            m = nph_f64_min(m, nph_f64_abs((st * a + ct * z) * ct - nz));
            m = nph_f64_min(m, nph_f64_abs((-st * a + ct * z) * ct - nz));
        }
    }
    return m;
}

// Whether two vectors agree to `tol` relative to the larger of 1 and each
// wanted component's magnitude.
static bool near_vec4(NphVec4 got, NphVec4 want, f32 tol)
{
    const f32 g[4] = {got.x, got.y, got.z, got.w};
    const f32 w[4] = {want.x, want.y, want.z, want.w};
    for (u32 k = 0; k < 4U; ++k) {
        if (!(nph_f32_abs(g[k] - w[k]) <= tol * nph_f32_max(1.0F, nph_f32_abs(w[k])))) {
            return false;
        }
    }
    return true;
}

// Per test: cases decided either way (so the random cases exercise both)
// and disagreements outside the margin single precision resolves.
typedef struct TwinTally {
    u32 yes;
    u32 no;
    u32 skipped;
    u32 wrong;
} TwinTally;

static void tally(TwinTally *t, bool gpu, bool cpu, f64 margin, f64 tolerance)
{
    if (margin < tolerance) {
        t->skipped += 1U;
        return;
    }
    t->yes += cpu ? 1U : 0U;
    t->no += cpu ? 0U : 1U;
    t->wrong += gpu != cpu ? 1U : 0U;
}

static void expect_tally(const char *name, const TwinTally *t)
{
    NTEST_LOG("%s: %u yes, %u no, %u too close to call, %u wrong", name, t->yes, t->no, t->skipped,
              t->wrong);
    NTEST_EXPECT_EQ_UINT(t->wrong, 0U);
    NTEST_EXPECT_GT_UINT(t->yes, 100U);
    NTEST_EXPECT_GT_UINT(t->no, 100U);
}

NTEST_F(Vis, culling_math_matches_its_cpu_twins)
{
    REQUIRE_PRODUCTION(f);
    enum {
        CASES = 8192U
    };
    NphGpuDevice *dev = f->dev;
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0x5EEDC0DEULL, 7U);
    const f32 width = 200.0F;
    const f32 height = 120.0F;

    // The view: random planes, the viewport, a 60-degree projection, and a
    // random pyramid of the size the renderer gives that viewport.
    static NphVisViewData view;
    memset(&view, 0, sizeof view);
    // Random planes that all keep the origin inside, so spheres near it
    // pass and far ones fail.
    for (u32 i = 0; i < 5U; ++i) {
        view.frustum[i] =
            nph_vec4_from_vec3(random_unit(&rng), nph_pcg32_next_range_f32(&rng, 1.0F, 5.0F));
    }
    view.viewport = nph_vec4(width, height, 1.0F / width, 1.0F / height);
    const f32 p11 = -1.0F / nph_f32_tan(0.5F);
    view.projection = nph_vec4(-p11 * height / width, p11, 0.1F, 0.0F);
    const u32 hzb_w = ((u32)width + 1U) / 2U;
    const u32 hzb_h = ((u32)height + 1U) / 2U;
    const u32 mips = nph_gpu_mip_count(hzb_w, hzb_h, 1U);
    NphGpuImageDesc hd;
    memset(&hd, 0, sizeof hd);
    hd.type = NPH_GPU_IMAGE_2D;
    hd.format = NPH_GPU_FORMAT_R32_SFLOAT;
    hd.width = hzb_w;
    hd.height = hzb_h;
    hd.mip_count = mips;
    hd.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    hd.name = "twin hzb";
    NphGpuImage hzb_image = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(dev, &hd, &hzb_image), NPH_RESULT_OK);
    f32 *levels[16];
    NTEST_ASSERT_LE_UINT(mips, 16U);
    for (u32 m = 0; m < mips; ++m) {
        const u32 w = nph_max_u32(hzb_w >> m, 1U);
        const u32 h = nph_max_u32(hzb_h >> m, 1U);
        levels[m] = NPH_ALLOC_ARRAY(nph_allocator_system(), f32, (u64)w * h);
        for (u32 t = 0; t < w * h; ++t) {
            levels[m][t] = nph_pcg32_next_f32(&rng);
        }
        NTEST_ASSERT_EQ_INT(
            nph_gpu_upload_image(dev, hzb_image, m, 0U, 1U, levels[m], (u64)w * h * sizeof(f32)),
            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(dev, NULL), NPH_RESULT_OK);
    view.hzb = nph_vec4((f32)hzb_w, (f32)hzb_h, (f32)mips, 0.0F);
    view.hzb_index = nph_gpu_image_sampled_index(dev, hzb_image);

    const u32 storage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    NphGpuBuffer view_buf =
        test_make_buffer(dev, sizeof view, storage, NPH_GPU_MEMORY_UPLOAD, "twin view");
    NphGpuBuffer case_buf = test_make_buffer(dev, (u64)CASES * sizeof(NphVisMathCase), storage,
                                             NPH_GPU_MEMORY_UPLOAD, "twin cases");
    NphGpuBuffer result_buf = test_make_buffer(dev, (u64)CASES * sizeof(NphVisMathResult), storage,
                                               NPH_GPU_MEMORY_READBACK, "twin results");
    memcpy(nph_gpu_buffer_map(dev, view_buf), &view, sizeof view);
    NphVisMathCase *cases = (NphVisMathCase *)nph_gpu_buffer_map(dev, case_buf);
    for (u32 i = 0; i < CASES; ++i) {
        cases[i] = random_case(&rng, width, height, i);
    }

    NphGpuShader shader = test_load_shader(dev, "vis_math_test.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    cd.name = "vis math test";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(dev, &cd, &pipeline), NPH_RESULT_OK);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "vis math", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_image_barrier(cmd, hzb_image, (u32)NPH_GPU_STAGE_ALL,
                              (u32)NPH_GPU_ACCESS_MEMORY_WRITE, NPH_GPU_LAYOUT_SHADER_READ_ONLY,
                              (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_READ,
                              NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    NphVisMathPush push;
    memset(&push, 0, sizeof push);
    push.view = nph_gpu_buffer_address(dev, view_buf);
    push.cases = nph_gpu_buffer_address(dev, case_buf);
    push.results = nph_gpu_buffer_address(dev, result_buf);
    push.count = CASES;
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, (CASES + 63U) / 64U, 1U, 1U);
    test_submit_wait(dev, cmd);
    const NphVisMathResult *results = (const NphVisMathResult *)nph_gpu_buffer_map(dev, result_buf);

    const NphRenderRefHzb ref_hzb = {(const f32 *const *)levels, hzb_w, hzb_h, mips,
                                     nph_vec2(width, height)};
    TwinTally projected = {0U, 0U, 0U, 0U};
    TwinTally frustum = {0U, 0U, 0U, 0U};
    TwinTally cone = {0U, 0U, 0U, 0U};
    TwinTally facing = {0U, 0U, 0U, 0U};
    TwinTally triangle = {0U, 0U, 0U, 0U};
    TwinTally occluded = {0U, 0U, 0U, 0U};
    u32 rect_wrong = 0U;
    u32 clipped_compared = 0U;
    u32 clipped_wrong = 0U;
    u32 spheres_wrong = 0U;
    u32 zbins_compared = 0U;
    u32 zbins_wrong = 0U;
    for (u32 i = 0; i < CASES; ++i) {
        const NphVisMathCase *c = &cases[i];
        const NphVisMathResult *r = &results[i];
        const NphVec3 centre = nph_vec3(c->sphere.x, c->sphere.y, c->sphere.z);
        NphVec4 rect;
        f32 depth = 0.0F;
        const bool proj =
            nph_render_ref_vis_project_sphere(centre, c->sphere.w, view.projection.x,
                                              view.projection.y, view.projection.z, &rect, &depth);
        const f64 near_margin =
            nph_f64_abs(-(f64)c->sphere.z - (f64)c->sphere.w - (f64)view.projection.z);
        tally(&projected, r->projected != 0U, proj, near_margin, 1e-4);
        // The rectangle is ill-conditioned close to the near plane; compare
        // where the sphere is well in front.
        if (proj && r->projected != 0U && near_margin > 0.05) {
            const f32 got[5] = {r->rect.x, r->rect.y, r->rect.z, r->rect.w, r->depth};
            const f32 want[5] = {rect.x, rect.y, rect.z, rect.w, depth};
            for (u32 k = 0; k < 5U; ++k) {
                const f32 tol = 1e-4F * nph_f32_max(1.0F, nph_f32_abs(want[k]));
                if (nph_f32_abs(got[k] - want[k]) > tol && rect_wrong++ == 0U) {
                    NTEST_LOG("case %u: rect component %u is %.7g, want %.7g", i, k, (f64)got[k],
                              (f64)want[k]);
                }
            }
        }
        const f64 fm = nph_render_ref_vis_frustum_margin(view.frustum, centre, c->sphere.w);
        tally(&frustum, r->in_frustum != 0U, fm >= 0.0, nph_f64_abs(fm), 1e-4);
        const NphVec3 apex = nph_vec3(c->cone.x, c->cone.y, c->cone.z);
        const NphVec3 axis = nph_vec3(c->axis.x, c->axis.y, c->axis.z);
        const f64 cm = nph_render_ref_vis_cone_margin(apex, axis, c->cone.w);
        const bool apex_off = nph_vec3_length(apex) > 0.0F;
        tally(&cone, r->cone_culled != 0U, apex_off && cm >= 0.0, nph_f64_abs(cm), 1e-4);
        const f64 det = nph_render_ref_vis_facing_det(c->c0, c->c1, c->c2);
        const NphVec4 corners[3] = {c->c0, c->c1, c->c2};
        f64 scale = 1.0;
        for (u32 k = 0; k < 3U; ++k) {
            scale *= nph_f64_abs((f64)corners[k].x) + nph_f64_abs((f64)corners[k].y) +
                     nph_f64_abs((f64)corners[k].w);
        }
        // Single precision resolves the determinant to a few ulps of its
        // largest term, and box edges to well under 1/1000 pixel.
        tally(&facing, r->front_facing != 0U, (det < 0.0) != (c->mirrored != 0U),
              nph_f64_abs(det) / scale, 2e-6);
        f64 facing_margin = 0.0;
        f64 box_margin = 0.0;
        const bool tc =
            nph_render_ref_vis_cull_triangle(c->c0, c->c1, c->c2, c->mirrored != 0U,
                                             nph_vec2(width, height), &facing_margin, &box_margin);
        tally(&triangle, r->tri_culled != 0U, tc,
              nph_f64_min(facing_margin / 2e-6, box_margin / 1e-3), 1.0);
        f64 hm = 0.0;
        const bool occ = nph_render_ref_vis_hzb_occluded(&ref_hzb, c->rect, c->depth, &hm);
        tally(&occluded, r->hzb_occluded != 0U, occ, hm, 1e-4);

        // The light culling: the clipped rectangle of spheres reaching in
        // front of the near plane, the sphere around a light, the bins.
        if (-(f64)c->sphere.z + (f64)c->sphere.w > (f64)view.projection.z &&
            clip_margin(c->sphere, view.projection.z) > 1e-3) {
            const NphVec4 want = nph_render_ref_vis_project_sphere_clipped(
                centre, c->sphere.w, view.projection.x, view.projection.y, view.projection.z);
            clipped_compared += 1U;
            if (!near_vec4(r->clipped_rect, want, 1e-3F) && clipped_wrong++ == 0U) {
                NTEST_LOG("case %u: clipped rect (%g %g %g %g), want (%g %g %g %g)", i,
                          (f64)r->clipped_rect.x, (f64)r->clipped_rect.y, (f64)r->clipped_rect.z,
                          (f64)r->clipped_rect.w, (f64)want.x, (f64)want.y, (f64)want.z,
                          (f64)want.w);
            }
        }
        const u32 type = (u32)c->light_direction.w;
        const NphVec4 sphere = nph_render_ref_vis_light_sphere(
            nph_vec3(c->light_position.x, c->light_position.y, c->light_position.z),
            nph_vec3(c->light_direction.x, c->light_direction.y, c->light_direction.z),
            c->light_position.w, type, c->light_cone.x, c->light_cone.y);
        const bool at_switch = type == (u32)NPH_RENDER_LIGHT_TYPE_SPOT &&
                               nph_f32_abs(c->light_cone.x - 0.70710678F) < 1e-6F;
        if (!at_switch && !near_vec4(r->light_sphere, sphere, 1e-5F) && spheres_wrong++ == 0U) {
            NTEST_LOG("case %u: light sphere (%g %g %g %g), want (%g %g %g %g)", i,
                      (f64)r->light_sphere.x, (f64)r->light_sphere.y, (f64)r->light_sphere.z,
                      (f64)r->light_sphere.w, (f64)sphere.x, (f64)sphere.y, (f64)sphere.z,
                      (f64)sphere.w);
        }
        f64 zm = 0.0;
        const u32 zbin =
            nph_render_ref_zbin(c->light_cone.z, view.projection.z,
                                (f32)NPH_RENDER_ZBINS / (f32)NPH_RENDER_ZBIN_OCTAVES, &zm);
        if (zm > 1e-3) {
            zbins_compared += 1U;
            if (r->zbin != zbin && zbins_wrong++ == 0U) {
                NTEST_LOG("case %u: distance %g in bin %u, want %u", i, (f64)c->light_cone.z,
                          r->zbin, zbin);
            }
        }
    }
    expect_tally("projected", &projected);
    expect_tally("in frustum", &frustum);
    expect_tally("cone culled", &cone);
    expect_tally("front facing", &facing);
    expect_tally("triangle culled", &triangle);
    expect_tally("hzb occluded", &occluded);
    NTEST_EXPECT_EQ_UINT(rect_wrong, 0U);
    NTEST_LOG("light culling: %u clipped rectangles, %u bins compared", clipped_compared,
              zbins_compared);
    NTEST_EXPECT_GT_UINT(clipped_compared, 1000U);
    NTEST_EXPECT_EQ_UINT(clipped_wrong, 0U);
    NTEST_EXPECT_EQ_UINT(spheres_wrong, 0U);
    NTEST_EXPECT_GT_UINT(zbins_compared, 1000U);
    NTEST_EXPECT_EQ_UINT(zbins_wrong, 0U);
    for (u32 m = 0; m < mips; ++m) {
        const u32 w = nph_max_u32(hzb_w >> m, 1U);
        const u32 h = nph_max_u32(hzb_h >> m, 1U);
        NPH_FREE_ARRAY(nph_allocator_system(), f32, levels[m], (u64)w * h);
    }
    nph_gpu_pipeline_destroy(dev, pipeline);
    nph_gpu_shader_destroy(dev, shader);
    nph_gpu_buffer_destroy(dev, result_buf);
    nph_gpu_buffer_destroy(dev, case_buf);
    nph_gpu_buffer_destroy(dev, view_buf);
    nph_gpu_image_destroy(dev, hzb_image);
}

// The depth pyramid the renderer builds, read back after a frame into an
// awkward 100 x 60 target (mips 50 x 30, 25 x 15, 12 x 7, 6 x 3, 3 x 1,
// 1 x 1: odd sources at three steps): every texel past mip 0 is exactly the
// minimum of the texels it covers, and every texel of a mip lies under one
// of the next. The twin test above found the pyramid built as if Vulkan
// rounded mips up, which dropped the last row and column of odd mips.
NTEST_F(Vis, the_depth_pyramid_is_the_minimum_of_what_it_covers)
{
    REQUIRE_PRODUCTION(f);
    Scene s;
    scene_build(f, &s, nph_dvec3_zero());
    NphRenderView v;
    scene_view(&v, nph_dvec3_zero(), NPH_RENDER_PIPELINE_PRODUCTION);
    NphGpuImage target =
        test_make_image_2d(f->dev, 100U, 60U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
                           (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT, "pyramid target");
    for (u32 i = 0; i < 2U; ++i) {
        (void)test_render_frame(f, &v, target);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const RenderVis *vis = &f->renderer->vis;
    NTEST_ASSERT_EQ_UINT(vis->hzb_width, 50U);
    NTEST_ASSERT_EQ_UINT(vis->hzb_height, 30U);
    NTEST_ASSERT_EQ_UINT(vis->hzb_mips, 6U);
    enum {
        MIPS = 6U
    };
    f32 *levels[MIPS];
    u32 widths[MIPS];
    u32 heights[MIPS];
    for (u32 m = 0; m < MIPS; ++m) {
        widths[m] = nph_max_u32(vis->hzb_width >> m, 1U);
        heights[m] = nph_max_u32(vis->hzb_height >> m, 1U);
        const u64 bytes = (u64)widths[m] * heights[m] * sizeof(f32);
        levels[m] = NPH_ALLOC_ARRAY(nph_allocator_system(), f32, (u64)widths[m] * heights[m]);
        NTEST_ASSERT_NOT_NULL(levels[m]);
        test_read_image(f->dev, vis->hzb, m, levels[m], bytes);
    }
    // Mip 0 holds the scene: depths in [0, 1], the far plane (0) where the
    // sky shows, nearer where the floor and the objects are.
    u32 covered = 0U;
    for (u32 t = 0; t < widths[0] * heights[0]; ++t) {
        NTEST_ASSERT_TRUE(levels[0][t] >= 0.0F && levels[0][t] <= 1.0F);
        covered += levels[0][t] > 0.0F ? 1U : 0U;
    }
    NTEST_EXPECT_GT_UINT(covered, widths[0] * heights[0] / 4U);
    u32 wrong = 0U;
    for (u32 m = 1; m < MIPS; ++m) {
        const u32 sw = widths[m - 1U];
        const u32 sh = heights[m - 1U];
        static u8 seen[50U * 30U];
        memset(seen, 0, sizeof seen);
        for (u32 y = 0; y < heights[m]; ++y) {
            for (u32 x = 0; x < widths[m]; ++x) {
                const u32 x1 = x == widths[m] - 1U ? sw - 1U : 2U * x + 1U;
                const u32 y1 = y == heights[m] - 1U ? sh - 1U : 2U * y + 1U;
                f32 want = levels[m - 1U][(u64)nph_min_u32(2U * y, sh - 1U) * sw +
                                          nph_min_u32(2U * x, sw - 1U)];
                for (u32 sy = nph_min_u32(2U * y, sh - 1U); sy <= y1; ++sy) {
                    for (u32 sx = nph_min_u32(2U * x, sw - 1U); sx <= x1; ++sx) {
                        want = nph_f32_min(want, levels[m - 1U][(u64)sy * sw + sx]);
                        seen[sy * sw + sx] = 1U;
                    }
                }
                const f32 got = levels[m][(u64)y * widths[m] + x];
                if (got != want && wrong++ == 0U) {
                    NTEST_LOG("mip %u texel (%u, %u): %.7g, want %.7g", m, x, y, (f64)got,
                              (f64)want);
                }
            }
        }
        for (u32 t = 0; t < sw * sh; ++t) {
            NTEST_EXPECT_EQ_UINT(seen[t], 1U);
        }
    }
    NTEST_EXPECT_EQ_UINT(wrong, 0U);
    for (u32 m = 0; m < MIPS; ++m) {
        NPH_FREE_ARRAY(nph_allocator_system(), f32, levels[m], (u64)widths[m] * heights[m]);
    }
    nph_gpu_image_destroy(f->dev, target);
}

// A wall in front of spheres, then moved aside: phase 0 defers the spheres
// (last frame's pyramid has them behind the wall) and phase 1 draws them.
// Returns the pyramid that frame leaves, every mip back to back, and the
// lit pixels of the image.
typedef struct Pyramid {
    f32 *texels;
    u64 count;
    u32 lit;
} Pyramid;

static Pyramid pyramid_after_a_reveal(RenderFixture *f, bool full_rebuild)
{
    f->renderer->vis.hzb_full_rebuild = full_rebuild;
    NphRenderEnvironmentDesc env;
    memset(&env, 0, sizeof env);
    env.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&env.sky);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &env), NPH_RESULT_OK);
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    static u8 img[PIXELS];
    static u8 sky[PIXELS];
    render_into(f, &v, 1U, sky);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh sphere = mesh_of(f, make_sphere);
    const NphRenderMaterial m = material(f, nph_vec4(0.7F, 0.7F, 0.7F, 1.0F), 0.0F, 0.6F);
    NphRenderTransform wall = at(0.0, 0.0, 1.0, 1.0F);
    wall.linear = nph_mat3_from_scale(nph_vec3(4.0F, 4.0F, 0.2F));
    const NphRenderInstance w = instance(f, cube, m, wall);
    for (u32 i = 0; i < 16U; ++i) {
        (void)instance(f, sphere, m,
                       at(-1.2 + 0.8 * (f64)(i % 4U), -1.2 + 0.8 * (f64)(i >> 2U), -1.0, 0.6F));
    }
    render_into(f, &v, 3U, img);
    const NphRenderTransform aside = at(60.0, 0.0, 1.0, 1.0F);
    NTEST_EXPECT_EQ_INT(nph_render_instance_set_transform(f->renderer, w, &aside), NPH_RESULT_OK);
    render_into(f, &v, 1U, img);
    Pyramid out;
    memset(&out, 0, sizeof out);
    out.lit = lit_pixels(img, sky);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const RenderVis *vis = &f->renderer->vis;
    for (u32 mip = 0; mip < vis->hzb_mips; ++mip) {
        out.count +=
            (u64)nph_max_u32(vis->hzb_width >> mip, 1U) * nph_max_u32(vis->hzb_height >> mip, 1U);
    }
    out.texels = NPH_ALLOC_ARRAY(nph_allocator_system(), f32, out.count);
    NTEST_ASSERT_NOT_NULL(out.texels);
    u64 at_texel = 0U;
    for (u32 mip = 0; mip < vis->hzb_mips; ++mip) {
        const u64 n =
            (u64)nph_max_u32(vis->hzb_width >> mip, 1U) * nph_max_u32(vis->hzb_height >> mip, 1U);
        test_read_image(f->dev, vis->hzb, mip, out.texels + at_texel, n * sizeof(f32));
        at_texel += n;
    }
    return out;
}

// The pyramid after phase 1 rebuilds only the tiles phase 1 drew into; it
// must be the pyramid a full rebuild gives, texel for texel.
NTEST(vis, the_pyramid_after_phase_1_is_a_full_rebuild_where_it_matters)
{
    RenderFixture tiled;
    RenderFixture full;
    test_render_setup(&tiled, NULL);
    if (tiled.dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    if (!nph_renderer_production_supported(tiled.renderer)) {
        test_render_teardown(&tiled);
        NTEST_SKIP("no production path on this device");
    }
    const Pyramid a = pyramid_after_a_reveal(&tiled, false);
    test_render_teardown(&tiled);
    test_render_setup(&full, NULL);
    const Pyramid b = pyramid_after_a_reveal(&full, true);
    test_render_teardown(&full);
    // Phase 1 drew the spheres: the image is not the bare sky.
    NTEST_EXPECT_GT_UINT(a.lit, 100U);
    NTEST_EXPECT_EQ_UINT(a.lit, b.lit);
    NTEST_ASSERT_EQ_UINT(a.count, b.count);
    u64 wrong = 0U;
    for (u64 i = 0; i < a.count; ++i) {
        wrong += a.texels[i] != b.texels[i] ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(wrong, 0U);
    NPH_FREE_ARRAY(nph_allocator_system(), f32, a.texels, a.count);
    NPH_FREE_ARRAY(nph_allocator_system(), f32, b.texels, b.count);
}

NTEST_F(Vis, production_matches_the_reference)
{
    REQUIRE_PRODUCTION(f);
    Scene s;
    scene_build(f, &s, nph_dvec3_zero());
    static u8 ref[PIXELS];
    static u8 vis[PIXELS];
    NphRenderView v;
    scene_view(&v, nph_dvec3_zero(), NPH_RENDER_PIPELINE_REFERENCE);
    render_into(f, &v, 1U, ref);
    scene_view(&v, nph_dvec3_zero(), NPH_RENDER_PIPELINE_PRODUCTION);
    const NphRenderFrameStats stats = render_into(f, &v, 3U, vis);
    NTEST_EXPECT_EQ_INT((int)stats.pipeline, (int)NPH_RENDER_PIPELINE_PRODUCTION);
    const ImageDiff d = image_diff(ref, vis, 3U);
    NTEST_LOG("reference vs production: %.2f %% within 3 levels, mean %.3f, max %u",
              (f64)d.within * 100.0, (f64)d.mean, d.max);
    // Interpolation (the rasteriser's against analytic barycentrics) and
    // the positions' arithmetic (an affine on the CPU against the split
    // translation on the GPU) differ in the last bits, which moves a few
    // silhouette pixels; everything else agrees to within rounding.
    NTEST_EXPECT_TRUE(d.within >= 0.97F);
    NTEST_EXPECT_TRUE(d.mean <= 1.0F);
    // The GPU's counters arrive two frames later: three frames in, the
    // first frame's are back, and every meshlet of the three meshes was
    // either drawn or culled.
    NTEST_EXPECT_GT_UINT(stats.vis_clusters, 0U);
    NTEST_EXPECT_GT_UINT(stats.vis_triangles, 0U);
    NTEST_EXPECT_EQ_UINT(stats.vis_overflow, 0U);
}

NTEST_F(Vis, culling_never_changes_the_image)
{
    REQUIRE_PRODUCTION(f);
    // A wall hides a grid of spheres behind it: occlusion culling must
    // remove them without changing a pixel.
    const NphRenderMesh sphere = mesh_of(f, make_sphere);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMaterial m = material(f, nph_vec4(0.6F, 0.6F, 0.7F, 1.0F), 0.0F, 0.5F);
    NphRenderTransform wall = at(0.0, 0.0, 1.0, 1.0F);
    wall.linear = nph_mat3_from_scale(nph_vec3(6.0F, 6.0F, 0.2F));
    NphRenderInstance insts[18];
    insts[0] = instance(f, cube, m, wall);
    for (u32 i = 0; i < 16U; ++i) {
        insts[1U + i] =
            instance(f, sphere, m, at(-1.5 + (f64)(i % 4U), -1.5 + (f64)(i >> 2U), -2.0, 0.8F));
    }
    // A finely divided plane in front of the wall facing away from the
    // camera: every meshlet's normal cone points away, so all are culled
    // before the raster (a cube is one meshlet holding all six face
    // normals, and a UV sphere's meshlets come out as rings: neither can).
    NphRenderMeshData grid;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 24U, 1.0F, &grid),
                        NPH_RESULT_OK);
    const NphRenderMesh plane = test_upload_mesh(f->renderer, &grid);
    nph_render_mesh_data_free(&grid);
    insts[17] = instance(f, plane, m,
                         nph_render_transform_from_trs(nph_dvec3(0.0, 0.0, 2.0),
                                                       nph_quat_from_rotation_x(-0.5F * NPH_PI),
                                                       nph_vec3(1.5F, 1.5F, 1.5F)));
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 10000.0F;
    ld.direction = nph_vec3(0.0F, 0.0F, -1.0F);
    NphRenderLight light;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v.ev100 = 12.0F;
    static u8 culled[PIXELS];
    static u8 coned[PIXELS];
    static u8 plain[PIXELS];
    // Everything on: the spheres go at the instance level.
    const NphRenderFrameStats stats = render_into(f, &v, 4U, culled);
    NTEST_LOG("occluded instances %u, meshlets: frustum %u cone %u occluded %u, clusters %u",
              stats.vis_instances_culled_occlusion, stats.vis_meshlets_culled_frustum,
              stats.vis_meshlets_culled_cone, stats.vis_meshlets_culled_occlusion,
              stats.vis_clusters);
    NTEST_EXPECT_EQ_UINT(stats.vis_instances_culled_occlusion, 16U);
    NTEST_EXPECT_GT_UINT(stats.vis_meshlets_culled_cone, 0U); // the plane facing away
    NTEST_EXPECT_EQ_UINT(stats.vis_clusters, 1U);             // the wall alone
    // No occlusion: the spheres reach the meshlet tests and are drawn.
    v.vis_flags = (u32)NPH_RENDER_VIS_NO_OCCLUSION;
    const NphRenderFrameStats cones = render_into(f, &v, 3U, coned);
    NTEST_LOG("without occlusion: cone %u, clusters %u", cones.vis_meshlets_culled_cone,
              cones.vis_clusters);
    NTEST_EXPECT_EQ_UINT(cones.vis_instances_culled_occlusion, 0U);
    NTEST_EXPECT_EQ_UINT(cones.vis_meshlets_culled_cone, stats.vis_meshlets_culled_cone);
    NTEST_EXPECT_GT_UINT(cones.vis_clusters, stats.vis_clusters);
    // Nothing but the frustum.
    v.vis_flags = (u32)NPH_RENDER_VIS_NO_OCCLUSION | (u32)NPH_RENDER_VIS_NO_CONE;
    const NphRenderFrameStats all = render_into(f, &v, 3U, plain);
    NTEST_EXPECT_EQ_UINT(all.vis_meshlets_culled_cone, 0U);
    NTEST_EXPECT_GT_UINT(all.vis_clusters, cones.vis_clusters);
    NTEST_EXPECT_EQ_MEM(culled, plain, PIXELS);
    NTEST_EXPECT_EQ_MEM(coned, plain, PIXELS);
    for (u32 i = 0; i < 18U; ++i) {
        nph_render_instance_destroy(f->renderer, insts[i]);
    }
}

NTEST_F(Vis, the_frustum_culls_what_is_off_screen)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh sphere = mesh_of(f, make_sphere);
    const NphRenderMaterial m = material(f, nph_vec4(1.0F, 1.0F, 1.0F, 1.0F), 0.0F, 0.5F);
    instance(f, sphere, m, at(0.0, 0.0, 0.0, 1.0F));
    for (u32 i = 0; i < 5U; ++i) {
        instance(f, sphere, m, at(100.0 + (f64)i, 0.0, 0.0, 1.0F)); // far to the right
    }
    instance(f, sphere, m, at(0.0, 0.0, 10.0, 1.0F)); // behind the camera
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    static u8 img[PIXELS];
    const NphRenderFrameStats stats = render_into(f, &v, 3U, img);
    NTEST_EXPECT_EQ_UINT(stats.vis_instances_culled_frustum, 6U);
    NTEST_EXPECT_EQ_UINT(stats.instances, 7U);
}

NTEST(vis, the_indirect_raster_matches_mesh_shaders)
{
    // The same scene on a device with mesh shaders and on one where they
    // are switched off (the indirect-draw path lavapipe-class devices
    // take): the images are the same.
    static u8 images[2][PIXELS];
    u32 rendered = 0U;
    bool mesh_shaders = false;
    for (u32 pass = 0; pass < 2U; ++pass) {
        RenderFixture fx;
        memset(&fx, 0, sizeof fx);
        NphGpuDeviceDesc dd;
        memset(&dd, 0, sizeof dd);
        dd.alloc = nph_allocator_system();
        dd.headless = 1;
        dd.disable_features = pass == 0U ? 0U : (u32)NPH_GPU_OPTIONAL_MESH_SHADER;
        fx.dev = test_gpu_create(&dd);
        if (fx.dev == NULL) {
            NTEST_SKIP("no Vulkan device available");
        }
        NphRendererDesc rd;
        test_renderer_desc(&rd, fx.dev);
        NTEST_ASSERT_EQ_INT(nph_renderer_create(&rd, &fx.renderer), NPH_RESULT_OK);
        NphGpuRenderGraphDesc gd;
        memset(&gd, 0, sizeof gd);
        gd.alloc = nph_allocator_system();
        NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(fx.dev, &gd, &fx.graph), NPH_RESULT_OK);
        fx.target = test_make_target(fx.dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, TEST_TARGET_SIZE);
        fx.pixels = NPH_ALLOC_ARRAY_ZEROED(nph_allocator_system(), u8, PIXELS);
        if (nph_renderer_production_supported(fx.renderer)) {
            NTEST_LOG("pass %u: %s", pass,
                      fx.renderer->vis.mesh_shaders ? "mesh shaders" : "indirect draws");
            mesh_shaders = mesh_shaders || fx.renderer->vis.mesh_shaders;
            Scene s;
            scene_build(&fx, &s, nph_dvec3_zero());
            NphRenderView v;
            scene_view(&v, nph_dvec3_zero(), NPH_RENDER_PIPELINE_PRODUCTION);
            render_into(&fx, &v, 2U, images[pass]);
            ++rendered;
        }
        test_render_teardown(&fx);
    }
    if (rendered != 2U) {
        NTEST_SKIP("no production path on this device");
    }
    if (!mesh_shaders) {
        NTEST_SKIP("no mesh-shader path on this device to compare with");
    }
    const ImageDiff d = image_diff(images[0], images[1], 0U);
    NTEST_LOG("mesh vs indirect: %.2f %% identical, max %u", (f64)d.within * 100.0, d.max);
    NTEST_EXPECT_TRUE(d.within >= 0.999F);
}

NTEST_F(Vis, positions_hold_far_from_the_origin)
{
    REQUIRE_PRODUCTION(f);
    // The scene and the camera 10,000 km out: the split translations keep
    // every bit that matters, so the image matches the one at the origin.
    Scene s;
    const NphDVec3 far = nph_dvec3(1.0e7, 2.5e6, -7.0e6);
    scene_build(f, &s, nph_dvec3_zero());
    static u8 near_img[PIXELS];
    static u8 far_img[PIXELS];
    NphRenderView v;
    scene_view(&v, nph_dvec3_zero(), NPH_RENDER_PIPELINE_PRODUCTION);
    render_into(f, &v, 1U, near_img);
    for (u32 i = 0; i < 3U; ++i) {
        nph_render_instance_destroy(f->renderer, s.instances[i]);
    }
    nph_render_light_destroy(f->renderer, s.sun);
    Scene t;
    t.cube = s.cube;
    NphRenderTransform c = at(far.x - 0.6, far.y, far.z, 1.0F);
    c.linear = nph_mat3_mul(nph_mat3_from_quat(nph_quat_from_axis_angle(
                                nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F)), 0.6F)),
                            c.linear);
    t.instances[0] = instance(f, s.cube, s.red, c);
    t.instances[1] = instance(f, s.sphere, s.gold, at(far.x + 0.7, far.y + 0.1, far.z, 1.2F));
    t.instances[2] = instance(f, s.plane, s.floor, at(far.x, far.y - 0.8, far.z, 6.0F));
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 0.95F, 0.9F);
    ld.intensity = 30000.0F;
    ld.direction = nph_vec3(-0.4F, -1.0F, -0.6F);
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &t.sun), NPH_RESULT_OK);
    scene_view(&v, far, NPH_RENDER_PIPELINE_PRODUCTION);
    render_into(f, &v, 2U, far_img);
    const ImageDiff d = image_diff(near_img, far_img, 2U);
    NTEST_LOG("origin vs 10,000 km: %.2f %% within 2 levels, mean %.3f", (f64)d.within * 100.0,
              (f64)d.mean);
    NTEST_EXPECT_TRUE(d.within >= 0.99F);
}

NTEST_F(Vis, instances_move_hide_and_go)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMaterial m = material(f, nph_vec4(1.0F, 1.0F, 1.0F, 1.0F), 0.0F, 1.0F);
    NphRenderEnvironmentDesc env;
    memset(&env, 0, sizeof env);
    env.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&env.sky);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &env), NPH_RESULT_OK);
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    static u8 empty[PIXELS];
    static u8 img[PIXELS];
    render_into(f, &v, 1U, empty);
    const NphRenderInstance a = instance(f, cube, m, at(-0.8, 0.0, 0.0, 0.6F));
    render_into(f, &v, 1U, img);
    const u32 left = lit_pixels(img, empty);
    NTEST_EXPECT_GT_UINT(left, 100U);
    // The left half holds it, the right half does not.
    NTEST_EXPECT_NE_INT(memcmp(test_pixel(f, 16U, 32U), empty + ((u64)32U * 64U + 16U) * 4U, 3U),
                        0);
    NTEST_EXPECT_EQ_MEM(test_pixel(f, 48U, 32U), empty + ((u64)32U * 64U + 48U) * 4U, 3U);
    // Moved to the right.
    const NphRenderTransform right = at(0.8, 0.0, 0.0, 0.6F);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, a, &right), NPH_RESULT_OK);
    render_into(f, &v, 1U, img);
    NTEST_EXPECT_EQ_MEM(test_pixel(f, 16U, 32U), empty + ((u64)32U * 64U + 16U) * 4U, 3U);
    NTEST_EXPECT_NE_INT(memcmp(test_pixel(f, 48U, 32U), empty + ((u64)32U * 64U + 48U) * 4U, 3U),
                        0);
    // Hidden, then shown.
    nph_render_instance_set_hidden(f->renderer, a, true);
    render_into(f, &v, 1U, img);
    NTEST_EXPECT_EQ_MEM(img, empty, PIXELS);
    nph_render_instance_set_hidden(f->renderer, a, false);
    render_into(f, &v, 1U, img);
    NTEST_EXPECT_EQ_UINT(lit_pixels(img, empty), left);
    // Destroyed, and its slot reused in the same frame by another.
    nph_render_instance_destroy(f->renderer, a);
    const NphRenderInstance b = instance(f, cube, m, at(-0.8, 0.0, 0.0, 0.6F));
    render_into(f, &v, 1U, img);
    NTEST_EXPECT_EQ_UINT(lit_pixels(img, empty), left);
    nph_render_instance_destroy(f->renderer, b);
    render_into(f, &v, 1U, img);
    NTEST_EXPECT_EQ_MEM(img, empty, PIXELS);
}

// Every instance destroyed and as many created again before the next frame
// (a level swap): the destroyed ones' material slots wait two frames for
// the GPU, so the slots and ranges must hold both populations. render_perf
// found the range allocator sized for one.
NTEST_F(Vis, a_whole_population_is_replaced_between_frames)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMaterial m = material(f, nph_vec4(1.0F, 1.0F, 1.0F, 1.0F), 0.0F, 1.0F);
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    // The fixture's capacity: 64 instances, in an 8 x 8 grid.
    enum {
        COUNT = 64U
    };
    NphRenderInstance insts[COUNT];
    static u8 first[PIXELS];
    static u8 second[PIXELS];
    for (u32 round = 0; round < 3U; ++round) {
        for (u32 i = 0; i < COUNT; ++i) {
            const f64 x = -1.4 + 0.4 * (f64)(i % 8U);
            const f64 y = -1.4 + 0.4 * (f64)(i >> 3U);
            insts[i] = instance(f, cube, m, at(x, y, 0.0, 0.25F));
        }
        render_into(f, &v, 2U, round == 0U ? first : second);
        if (round > 0U) {
            NTEST_EXPECT_EQ_MEM(second, first, PIXELS);
        }
        for (u32 i = 0; i < COUNT; ++i) {
            nph_render_instance_destroy(f->renderer, insts[i]);
        }
    }
}

// A field of small spheres and pillars far enough away that most clusters'
// triangles are a pixel or two across: the software raster draws those and
// the hardware raster the rest, and the image is the one the hardware
// raster alone draws.
NTEST_F(Vis, the_software_raster_draws_what_the_hardware_raster_draws)
{
    REQUIRE_PRODUCTION(f);
    const NphRenderMesh sphere = mesh_of(f, make_sphere);
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMaterial gold = material(f, nph_vec4(1.0F, 0.78F, 0.34F, 1.0F), 1.0F, 0.3F);
    const NphRenderMaterial red = material(f, nph_vec4(0.8F, 0.1F, 0.1F, 1.0F), 0.0F, 0.6F);
    NphRenderEnvironmentDesc env;
    memset(&env, 0, sizeof env);
    env.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&env.sky);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &env), NPH_RESULT_OK);
    for (u32 i = 0; i < 48U; ++i) {
        const f64 x = -3.5 + (f64)(i % 8U);
        const f64 z = -6.0 - 1.5 * (f64)(i >> 3U);
        if (i % 5U == 0U) {
            NphRenderTransform t = at(x, 0.0, z, 0.5F);
            t.linear = nph_mat3_mul(
                nph_mat3_from_quat(nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.3F * (f32)i)),
                t.linear);
            (void)instance(f, cube, red, t);
        } else {
            (void)instance(f, sphere, gold, at(x, 0.0, z, 0.7F));
        }
    }
    NphRenderView v;
    nph_render_view_default(&v, nph_render_camera_look_at(nph_dvec3(0.0, 1.5, 2.0),
                                                          nph_dvec3(0.0, 0.0, -8.0),
                                                          nph_vec3_unit_y(), 0.9F, 0.05F));
    v.ev100 = 13.0F;
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    static u8 hardware[PIXELS];
    static u8 hybrid[PIXELS];
    v.vis_flags = (u32)NPH_RENDER_VIS_NO_SW_RASTER;
    const NphRenderFrameStats hw = render_into(f, &v, 4U, hardware);
    v.vis_flags = 0U;
    const NphRenderFrameStats sw = render_into(f, &v, 4U, hybrid);
    NTEST_EXPECT_EQ_UINT(hw.vis_clusters_sw, 0U);
    NTEST_EXPECT_GT_UINT(sw.vis_clusters_sw, sw.vis_clusters / 2U);
    NTEST_EXPECT_EQ_UINT(sw.vis_clusters, hw.vis_clusters);
    const ImageDiff d = image_diff(hardware, hybrid, 2U);
    NTEST_LOG("%u of %u clusters in software; hardware vs hybrid: %.2f %% within 2 levels, "
              "mean %.3f, max %u",
              sw.vis_clusters_sw, sw.vis_clusters, (f64)d.within * 100.0, (f64)d.mean, d.max);
    NTEST_EXPECT_TRUE(d.within >= 0.995F);
}

// A finely divided plane filling the view, seen from above at a slant, its
// clusters small enough that nearly all go to the software raster (the
// renderer here takes any cluster that fits in 64 pixels; a few reach past
// the screen's edge and stay in hardware): no pixel shows the background
// through it, between software triangles or where software meets hardware,
// and the image is the hardware raster's. Neighbouring triangles share edges, and
// the top-left rule must give each pixel centre to one of them; a raster
// that gave an edge's centres to neither would leave a crack.
NTEST(vis, the_software_raster_leaves_no_cracks)
{
    RenderFixture fx;
    NphRendererDesc rd;
    test_renderer_desc(&rd, NULL);
    rd.sw_raster_pixels = 64.0F;
    test_render_setup(&fx, &rd);
    if (fx.dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    if (!nph_renderer_production_supported(fx.renderer)) {
        test_render_teardown(&fx);
        NTEST_SKIP("no production path on this device");
    }
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 1.0F, 128U, 1.0F, &data),
                        NPH_RESULT_OK);
    const NphRenderMesh plane = test_upload_mesh(fx.renderer, &data);
    nph_render_mesh_data_free(&data);
    const NphRenderMaterial grey = material(&fx, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F), 0.0F, 0.8F);
    NphRenderTransform t = at(0.0, 0.0, 0.0, 40.0F);
    t.linear = nph_mat3_mul(nph_mat3_from_quat(nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.37F)),
                            t.linear);
    (void)instance(&fx, plane, grey, t);
    NphRenderView v;
    nph_render_view_default(&v, nph_render_camera_look_at(nph_dvec3(0.3, 12.0, 6.0),
                                                          nph_dvec3(-0.4, 0.0, -2.0),
                                                          nph_vec3_unit_y(), 0.5F, 0.05F));
    v.ev100 = 13.0F;
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    // Magenta bright enough to survive the exposure: what a crack shows.
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3(20000.0F, 0.0F, 20000.0F);
    static u8 hybrid[PIXELS];
    static u8 hardware[PIXELS];
    // Four frames: the GPU's counters arrive two frames late.
    const NphRenderFrameStats sw = render_into(&fx, &v, 4U, hybrid);
    v.vis_flags = (u32)NPH_RENDER_VIS_NO_SW_RASTER;
    (void)render_into(&fx, &v, 4U, hardware);
    NTEST_EXPECT_GE_UINT(sw.vis_clusters_sw * 10U, sw.vis_clusters * 9U);
    NTEST_EXPECT_GT_UINT(sw.vis_clusters, 8U);
    u32 holes = 0U;
    for (u32 i = 0; i < TEST_TARGET_SIZE * TEST_TARGET_SIZE; ++i) {
        const u8 *px = &hybrid[(u64)i * 4U];
        holes += px[0] > 150U && px[1] < 40U && px[2] > 150U ? 1U : 0U;
    }
    const ImageDiff d = image_diff(hardware, hybrid, 2U);
    NTEST_LOG("%u of %u clusters in software: %u background pixels; hardware vs software: "
              "%.2f %% within 2 levels, max %u",
              sw.vis_clusters_sw, sw.vis_clusters, holes, (f64)d.within * 100.0, d.max);
    NTEST_EXPECT_EQ_UINT(holes, 0U);
    NTEST_EXPECT_TRUE(d.within >= 0.995F);
    test_render_teardown(&fx);
}

NTEST_F(Vis, alpha_modes_and_sides_match_the_reference)
{
    REQUIRE_PRODUCTION(f);
    // A masked plane with a checker texture, a blended plane in front, and
    // a double-sided plane seen from behind, mirrored: every case the raster
    // and the transparent pass decide differently from opaque geometry.
    const NphRenderMesh plane = mesh_of(f, make_plane);
    u8 checker[4U * 4U * 4U];
    for (u32 i = 0; i < 16U; ++i) {
        const bool on = ((i % 4U) + (i / 4U)) % 2U == 0U;
        checker[i * 4U + 0U] = 255U;
        checker[i * 4U + 1U] = on ? 255U : 64U;
        checker[i * 4U + 2U] = 255U;
        checker[i * 4U + 3U] = on ? 255U : 0U;
    }
    const void *mips[1] = {checker};
    const u64 sizes[1] = {sizeof checker};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    td.width = 4U;
    td.height = 4U;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    NphRenderTexture tex;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &tex), NPH_RESULT_OK);
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.alpha_mode = NPH_RENDER_ALPHA_MASK;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = tex;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].nearest = 1;
    NphRenderMaterial masked;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &masked), NPH_RESULT_OK);
    nph_render_material_desc_default(&md);
    md.alpha_mode = NPH_RENDER_ALPHA_BLEND;
    md.base_color = nph_vec4(0.1F, 0.4F, 1.0F, 0.5F);
    NphRenderMaterial blended;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &blended), NPH_RESULT_OK);
    nph_render_material_desc_default(&md);
    md.double_sided = 1;
    md.base_color = nph_vec4(0.2F, 1.0F, 0.2F, 1.0F);
    NphRenderMaterial two_sided;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &two_sided), NPH_RESULT_OK);

    // Planes face +Y; a quarter turn about X faces the camera on +Z.
    NphRenderTransform t = nph_render_transform_from_trs(
        nph_dvec3(-0.6, 0.0, 0.0), nph_quat_from_rotation_x(0.5F * NPH_PI), nph_vec3_one());
    instance(f, plane, masked, t);
    t.translation = nph_dvec3(0.0, 0.0, 0.5);
    t.linear = nph_mat3_mul(t.linear, nph_mat3_from_scale(nph_vec3(1.5F, 1.0F, 1.5F)));
    instance(f, plane, blended, t);
    // Facing away (-Z) and mirrored in X: its back face shows.
    NphRenderTransform back = nph_render_transform_from_trs(
        nph_dvec3(0.7, 0.0, -0.2), nph_quat_from_rotation_x(-0.5F * NPH_PI), nph_vec3_one());
    back.linear = nph_mat3_mul(nph_mat3_from_scale(nph_vec3(-1.0F, 1.0F, 1.0F)), back.linear);
    instance(f, plane, two_sided, back);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.color = nph_vec3_one();
    ld.intensity = 50.0F;
    ld.position = nph_dvec3(0.0, 0.5, 2.0);
    NphRenderLight light;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);

    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.ev100 = 4.0F;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3(30.0F, 30.0F, 30.0F);
    static u8 ref[PIXELS];
    static u8 vis[PIXELS];
    render_into(f, &v, 1U, ref);
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    const NphRenderFrameStats stats = render_into(f, &v, 1U, vis);
    NTEST_EXPECT_EQ_UINT(stats.blended, 1U); // the CPU draws only the blended plane
    const ImageDiff d = image_diff(ref, vis, 3U);
    NTEST_LOG("alpha modes and sides: %.2f %% within 3 levels, mean %.3f, max %u",
              (f64)d.within * 100.0, (f64)d.mean, d.max);
    NTEST_EXPECT_TRUE(d.within >= 0.97F);
    NTEST_EXPECT_TRUE(d.mean <= 1.5F);
}

NTEST_F(Vis, debug_views_show_clusters_triangles_and_instances)
{
    REQUIRE_PRODUCTION(f);
    Scene s;
    scene_build(f, &s, nph_dvec3_zero());
    NphRenderView v;
    scene_view(&v, nph_dvec3_zero(), NPH_RENDER_PIPELINE_PRODUCTION);
    v.tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    static u8 img[PIXELS];
    u32 distinct[3] = {0U, 0U, 0U};
    const NphRenderDebugView views[3] = {NPH_RENDER_VIEW_INSTANCES, NPH_RENDER_VIEW_CLUSTERS,
                                         NPH_RENDER_VIEW_TRIANGLES};
    for (u32 k = 0; k < 3U; ++k) {
        v.debug_view = views[k];
        render_into(f, &v, 1U, img);
        // Count distinct colours on one row through the scene.
        u32 seen[64];
        u32 n = 0U;
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            const u8 *p = img + ((u64)40U * TEST_TARGET_SIZE + x) * 4U;
            const u32 c = (u32)p[0] | ((u32)p[1] << 8U) | ((u32)p[2] << 16U);
            bool found = false;
            for (u32 i = 0; i < n; ++i) {
                found = found || seen[i] == c;
            }
            if (!found && n < 64U) {
                seen[n++] = c;
            }
        }
        distinct[k] = n;
    }
    NTEST_LOG("distinct colours on a row: instances %u, clusters %u, triangles %u", distinct[0],
              distinct[1], distinct[2]);
    NTEST_EXPECT_GE_UINT(distinct[0], 3U); // the background and at least two instances
    NTEST_EXPECT_GE_UINT(distinct[1], distinct[0]);
    NTEST_EXPECT_GT_UINT(distinct[2], distinct[1]);
}

NTEST_F(Vis, meshlets_given_or_built_draw_the_same)
{
    REQUIRE_PRODUCTION(f);
    // A sphere created with meshlets built by the caller (as a cooked mesh
    // brings them) and one left to the renderer: the same image.
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_sphere(nph_allocator_system(), 0.5F, 24U, 12U, &data),
                        NPH_RESULT_OK);
    NphRenderSubmesh sub;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(&data, "given meshlets", &sub, &md);
    NphVec3 *positions = NPH_ALLOC_ARRAY(nph_allocator_system(), NphVec3, data.vertex_count);
    for (u32 i = 0; i < data.vertex_count; ++i) {
        const NphRenderVertexPosition p = data.positions[i];
        const f32 k = 1.0F / 65535.0F;
        positions[i] = nph_vec3(data.position_offset.x + (f32)p.x * k * data.position_scale.x,
                                data.position_offset.y + (f32)p.y * k * data.position_scale.y,
                                data.position_offset.z + (f32)p.z * k * data.position_scale.z);
    }
    const NphRenderIndexRange range = {0U, data.index_count};
    NphRenderMeshletInput in;
    memset(&in, 0, sizeof in);
    in.positions = positions;
    in.vertex_count = data.vertex_count;
    in.indices = data.indices;
    in.index_count = data.index_count;
    in.ranges = &range;
    in.range_count = 1U;
    in.max_triangles = 32U; // smaller than the renderer would build
    in.scratch = nph_allocator_system();
    NphRenderMeshlets ml;
    nph_render_meshlets_init(&ml, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_render_meshlets_build(&in, &ml), NPH_RESULT_OK);
    md.meshlets = ml.meshlets.data;
    md.meshlet_bounds = ml.bounds.data;
    md.meshlet_count = ml.meshlets.len;
    md.meshlet_vertices = ml.vertices.data;
    md.meshlet_vertex_count = ml.vertices.len;
    md.meshlet_triangles = ml.triangles.data;
    md.meshlet_triangle_count = ml.triangles.len / 3U;
    NphRenderMesh given;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &given), NPH_RESULT_OK);

    // Meshlets that would read outside the streams are refused.
    NphRenderMeshDesc bad = md;
    NphRenderMeshlet broken = ml.meshlets.data[0];
    broken.vertex_offset = ml.vertices.len;
    bad.meshlets = &broken;
    bad.meshlet_count = 1U;
    NphRenderMesh none;
    NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &bad, &none),
                        NPH_RESULT_INVALID_ARGUMENT);
    broken = ml.meshlets.data[0];
    broken.submesh = 1U;
    NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &bad, &none),
                        NPH_RESULT_INVALID_ARGUMENT);
    broken = ml.meshlets.data[0];
    broken.triangle_count = 125U;
    NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &bad, &none),
                        NPH_RESULT_INVALID_ARGUMENT);

    md.meshlets = NULL;
    NphRenderMesh built;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &built), NPH_RESULT_OK);
    nph_render_meshlets_free(&ml);
    NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, positions, data.vertex_count);
    nph_render_mesh_data_free(&data);

    const NphRenderMaterial m = material(f, nph_vec4(0.9F, 0.9F, 0.9F, 1.0F), 0.0F, 0.4F);
    NphRenderView v;
    nph_render_view_default(&v, test_camera());
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    static u8 a[PIXELS];
    static u8 b[PIXELS];
    NphRenderInstance i = instance(f, given, m, at(0.0, 0.0, 0.0, 2.0F));
    render_into(f, &v, 1U, a);
    nph_render_instance_destroy(f->renderer, i);
    i = instance(f, built, m, at(0.0, 0.0, 0.0, 2.0F));
    render_into(f, &v, 1U, b);
    nph_render_instance_destroy(f->renderer, i);
    NTEST_EXPECT_EQ_MEM(a, b, PIXELS);
}

// ---------------------------------------------------------------------------
// The light grid
// ---------------------------------------------------------------------------

// The light tests render into a 250 x 150 target: 16 x 10 light tiles, the
// last column and row partial.
#define LIGHT_W      250U
#define LIGHT_H      150U
#define LIGHT_PIXELS ((u64)LIGHT_W * LIGHT_H * 4U)

typedef struct LightFixture {
    RenderFixture f;
    NphGpuImage target;
} LightFixture;

static bool light_setup(LightFixture *lf, u32 max_lights)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_lights = max_lights;
    test_render_setup(&lf->f, &d);
    lf->target = NPH_GPU_IMAGE_NULL;
    if (lf->f.dev == NULL || !nph_renderer_production_supported(lf->f.renderer)) {
        return false;
    }
    lf->target = test_make_image_2d(lf->f.dev, LIGHT_W, LIGHT_H, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 1U,
                                    (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT |
                                        (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC,
                                    "light test target");
    return true;
}

static void light_teardown(LightFixture *lf)
{
    if (lf->f.dev != NULL) {
        nph_gpu_image_destroy(lf->f.dev, lf->target);
    }
    test_render_teardown(&lf->f);
}

#define REQUIRE_LIGHTS(lf, max_lights)                                                             \
    do {                                                                                           \
        if (!light_setup((lf), (max_lights))) {                                                    \
            const bool no_device = (lf)->f.dev == NULL;                                            \
            light_teardown(lf);                                                                    \
            NTEST_SKIP(no_device ? "no Vulkan device available"                                    \
                                 : "no production path on this device");                           \
        }                                                                                          \
    } while (0)

static NphRenderCamera light_camera(void)
{
    return nph_render_camera_look_at(nph_dvec3(0.0, 1.5, 5.0), nph_dvec3(0.0, 0.0, 0.0),
                                     nph_vec3(0.0F, 1.0F, 0.0F), 0.9F, 0.05F);
}

static void light_view(NphRenderView *v, u32 vis_flags)
{
    nph_render_view_default(v, light_camera());
    v->ev100 = 7.0F; // an interior
    v->pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v->disable_ibl = 1;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3(1.0F, 2.0F, 3.0F);
    v->vis_flags = vis_flags;
}

// Renders `frames` frames into the light target and reads the last back.
static NphRenderFrameStats light_render(LightFixture *lf, const NphRenderView *v, u32 frames,
                                        u8 *out)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < frames; ++i) {
        stats = test_render_frame(&lf->f, v, lf->target);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(lf->f.dev), NPH_RESULT_OK);
    test_read_image(lf->f.dev, lf->target, 0U, out, LIGHT_PIXELS);
    return stats;
}

static ImageDiff light_diff(const u8 *a, const u8 *b, u32 tol)
{
    ImageDiff d = {0.0F, 0.0F, 0U};
    u64 sum = 0U;
    u32 ok = 0U;
    for (u32 i = 0; i < LIGHT_W * LIGHT_H; ++i) {
        u32 worst = 0U;
        for (u32 c = 0; c < 3U; ++c) {
            const u32 x = a[i * 4U + c];
            const u32 y = b[i * 4U + c];
            const u32 e = x > y ? x - y : y - x;
            worst = nph_max_u32(worst, e);
            sum += e;
        }
        d.max = nph_max_u32(d.max, worst);
        ok += worst <= tol ? 1U : 0U;
    }
    d.within = (f32)ok / (f32)(LIGHT_W * LIGHT_H);
    d.mean = (f32)sum / (f32)(LIGHT_W * LIGHT_H * 3U);
    return d;
}

static NphRenderLightDesc point_desc(NphDVec3 p, f32 range, f32 intensity, NphVec3 color)
{
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.position = p;
    ld.range = range;
    ld.intensity = intensity;
    ld.color = color;
    return ld;
}

static NphRenderLight make_light(RenderFixture *f, const NphRenderLightDesc *ld)
{
    NphRenderLight l = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, ld, &l), NPH_RESULT_OK);
    return l;
}

static NphRenderLight make_point(RenderFixture *f, NphDVec3 p, f32 range, f32 intensity)
{
    const NphRenderLightDesc ld = point_desc(p, range, intensity, nph_vec3_one());
    return make_light(f, &ld);
}

// A random light of finite range in the room: points and spots, around
// and behind the camera.
static NphRenderLightDesc random_light(NphPcg32 *rng)
{
    const NphDVec3 p = nph_dvec3((f64)nph_pcg32_next_range_f32(rng, -6.0F, 6.0F),
                                 (f64)nph_pcg32_next_range_f32(rng, -0.95F, 2.5F),
                                 (f64)nph_pcg32_next_range_f32(rng, -4.0F, 7.0F));
    const NphVec3 color = nph_vec3(nph_pcg32_next_range_f32(rng, 0.2F, 1.0F),
                                   nph_pcg32_next_range_f32(rng, 0.2F, 1.0F),
                                   nph_pcg32_next_range_f32(rng, 0.2F, 1.0F));
    NphRenderLightDesc ld = point_desc(p, nph_pcg32_next_range_f32(rng, 0.3F, 3.0F),
                                       nph_pcg32_next_range_f32(rng, 20.0F, 400.0F), color);
    if (nph_pcg32_next_u32(rng) % 3U == 0U) {
        ld.type = NPH_RENDER_LIGHT_TYPE_SPOT;
        ld.direction =
            nph_vec3_normalize(nph_vec3_add(random_unit(rng), nph_vec3(0.0F, -1.5F, 0.0F)));
        ld.outer_cone = nph_pcg32_next_range_f32(rng, 0.15F, 1.5F);
        ld.inner_cone = ld.outer_cone * nph_pcg32_next_range_f32(rng, 0.0F, 0.9F);
    }
    return ld;
}

// A room: a floor, a back wall, and spheres and cubes between them.
static void light_room(RenderFixture *f)
{
    const NphRenderMesh cube = mesh_of(f, make_cube);
    const NphRenderMesh sphere = mesh_of(f, make_sphere);
    const NphRenderMesh plane = mesh_of(f, make_plane);
    const NphRenderMaterial white = material(f, nph_vec4(0.8F, 0.8F, 0.8F, 1.0F), 0.0F, 0.7F);
    const NphRenderMaterial metal = material(f, nph_vec4(0.9F, 0.7F, 0.4F, 1.0F), 1.0F, 0.35F);
    (void)instance(f, plane, white, at(0.0, -1.0, 0.0, 12.0F));
    NphRenderTransform wall = at(0.0, 1.0, -4.0, 1.0F);
    wall.linear = nph_mat3_from_scale(nph_vec3(12.0F, 6.0F, 0.2F));
    (void)instance(f, cube, white, wall);
    for (u32 i = 0; i < 12U; ++i) {
        const u32 row = i / 4U;
        const f64 x = -3.0 + 2.0 * (f64)(i % 4U);
        const f64 z = -2.5 + 1.6 * (f64)row;
        (void)instance(f, (i & 1U) != 0U ? sphere : cube, (i % 3U) == 0U ? metal : white,
                       at(x, -0.5, z, 0.9F));
    }
}

// The grid read back after a frame: the counts, the sorted lights, the
// bins, and the tile masks.
typedef struct GridReadback {
    u32 globals;
    u32 locals;
    NphRenderLightData *lights; // the sorted locals
    u32 *zbins;                 // NPH_RENDER_ZBINS
    u32 *tiles;                 // tiles_x * tiles_y * words
    u32 tiles_x;
    u32 tiles_y;
    u32 words;
} GridReadback;

static GridReadback grid_read(RenderFixture *f)
{
    NphAllocator *a = nph_allocator_system();
    const RenderVis *v = &f->renderer->vis;
    GridReadback g;
    memset(&g, 0, sizeof g);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NphVisCounters c;
    test_read_buffer(f->dev, v->counters, 0U, sizeof c, &c);
    g.globals = c.lights_global;
    g.locals = c.lights_local;
    g.tiles_x = v->light_tiles_x;
    g.tiles_y = v->light_tiles_y;
    g.words = v->light_words;
    g.lights = NPH_ALLOC_ARRAY(a, NphRenderLightData, nph_max_u32(g.locals, 1U));
    g.zbins = NPH_ALLOC_ARRAY(a, u32, NPH_RENDER_ZBINS);
    g.tiles = NPH_ALLOC_ARRAY(a, u32, (u64)g.tiles_x * g.tiles_y * g.words);
    NTEST_ASSERT_TRUE(g.lights != NULL && g.zbins != NULL && g.tiles != NULL);
    if (g.locals != 0U) {
        test_read_buffer(f->dev, v->light_grid, v->light_layout.locals,
                         (u64)g.locals * sizeof(NphRenderLightData), g.lights);
    }
    test_read_buffer(f->dev, v->light_grid, v->light_layout.zbins,
                     (u64)NPH_RENDER_ZBINS * sizeof(u32), g.zbins);
    test_read_buffer(f->dev, v->light_tiles, 0U, (u64)g.tiles_x * g.tiles_y * g.words * sizeof(u32),
                     g.tiles);
    return g;
}

static void grid_free(GridReadback *g)
{
    NphAllocator *a = nph_allocator_system();
    NPH_FREE_ARRAY(a, NphRenderLightData, g->lights, nph_max_u32(g->locals, 1U));
    NPH_FREE_ARRAY(a, u32, g->zbins, NPH_RENDER_ZBINS);
    NPH_FREE_ARRAY(a, u32, g->tiles, (u64)g->tiles_x * g->tiles_y * g->words);
}

static bool grid_tile_has(const GridReadback *g, u32 tx, u32 ty, u32 light)
{
    const u32 word =
        g->tiles[(u64)(light / 32U) * g->tiles_x * g->tiles_y + (u64)ty * g->tiles_x + tx];
    return ((word >> (light % 32U)) & 1U) != 0U;
}

static bool grid_bin_has(const GridReadback *g, u32 bin, u32 light)
{
    const u32 first = g->zbins[bin] & 0xFFFFU;
    const u32 last = g->zbins[bin] >> 16U;
    return first <= light && light <= last;
}

// What the grid must hold for a light, from the C twins: its tiles and
// its distance bins (without the shader's slack, so the GPU's must
// contain these).
typedef struct LightBounds {
    u32 tile_lo[2];
    u32 tile_hi[2];
    u32 bin_lo;
    u32 bin_hi;
} LightBounds;

static LightBounds light_bounds(const NphRenderLightDesc *ld, const NphRenderCamera *cam)
{
    const NphMat3 rot = nph_mat3_from_quat(nph_quat_normalize_or_identity(cam->orientation));
    const NphMat3 view = nph_mat3_transpose(rot);
    const NphMat4 proj =
        nph_mat4_perspective_infinite(cam->yfov, (f32)LIGHT_W / (f32)LIGHT_H, cam->znear);
    const NphVec3 rel = nph_dvec3_relative(ld->position, cam->position);
    const NphVec3 dir = ld->type == NPH_RENDER_LIGHT_TYPE_POINT ? nph_vec3(0.0F, -1.0F, 0.0F)
                                                                : nph_vec3_normalize(ld->direction);
    const NphVec4 s =
        nph_render_ref_vis_light_sphere(rel, dir, ld->range, (u32)ld->type,
                                        nph_f32_cos(ld->outer_cone), nph_f32_sin(ld->outer_cone));
    const NphVec3 c = nph_mat3_mul_vec3(view, nph_vec3(s.x, s.y, s.z));
    const NphVec4 rect =
        nph_render_ref_vis_project_sphere_clipped(c, s.w, proj.c[0].x, proj.c[1].y, cam->znear);
    LightBounds b;
    const f32 tiles[2] = {(f32)LIGHT_W / (f32)NPH_RENDER_LIGHT_TILE,
                          (f32)LIGHT_H / (f32)NPH_RENDER_LIGHT_TILE};
    const f32 lo[2] = {rect.x, rect.y};
    const f32 hi[2] = {rect.z, rect.w};
    for (u32 k = 0; k < 2U; ++k) {
        const f32 last = nph_f32_ceil(tiles[k]) - 1.0F;
        b.tile_lo[k] = (u32)nph_f32_clamp(nph_f32_floor(lo[k] * tiles[k]), 0.0F, last);
        b.tile_hi[k] = (u32)nph_f32_clamp(nph_f32_floor(hi[k] * tiles[k]), 0.0F, last);
    }
    const f32 scale = (f32)NPH_RENDER_ZBINS / (f32)NPH_RENDER_ZBIN_OCTAVES;
    f64 margin = 0.0;
    b.bin_lo = nph_render_ref_zbin(nph_f32_max(-c.z - s.w, cam->znear), cam->znear, scale, &margin);
    b.bin_hi = nph_render_ref_zbin(-c.z + s.w, cam->znear, scale, &margin);
    return b;
}

// A few lights where the grid's bounds are known: each must be in every
// tile and bin its sphere reaches, and in no tile more than one beyond;
// the lights that reach every pixel and those out of view are told apart.
NTEST(vis, the_light_grid_bounds_each_light)
{
    LightFixture lf;
    REQUIRE_LIGHTS(&lf, 16U);
    RenderFixture *f = &lf.f;
    light_room(f);
    const NphRenderCamera cam = light_camera();
    NphRenderLightDesc locals[4];
    locals[0] = point_desc(nph_dvec3(-2.0, 0.0, 0.0), 0.8F, 100.0F, nph_vec3_one());
    locals[1] = point_desc(nph_dvec3(2.0, 0.5, -1.0), 1.2F, 100.0F, nph_vec3_one());
    locals[2] = point_desc(nph_dvec3(0.0, 2.0, 1.0), 3.0F, 300.0F, nph_vec3_one());
    locals[2].type = NPH_RENDER_LIGHT_TYPE_SPOT;
    locals[2].direction = nph_vec3(0.0F, -1.0F, 0.0F);
    locals[2].outer_cone = 0.4F;
    locals[2].inner_cone = 0.2F;
    // The camera inside its range.
    locals[3] = point_desc(nph_dvec3(0.2, 1.4, 5.1), 1.0F, 20.0F, nph_vec3_one());
    for (u32 i = 0; i < 4U; ++i) {
        (void)make_light(f, &locals[i]);
    }
    NphRenderLightDesc sun;
    memset(&sun, 0, sizeof sun);
    sun.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    sun.direction = nph_vec3(0.3F, -1.0F, -0.2F);
    sun.intensity = 50.0F;
    sun.color = nph_vec3_one();
    (void)make_light(f, &sun);
    (void)make_point(f, nph_dvec3(0.0, 3.0, 0.0), 0.0F, 10.0F); // unlimited range
    // Out of view: behind the camera, and far to the right.
    (void)make_point(f, nph_dvec3(0.0, 1.5, 9.0), 1.0F, 10.0F);
    (void)make_point(f, nph_dvec3(30.0, 0.0, 0.0), 1.0F, 10.0F);

    NphRenderView v;
    light_view(&v, 0U);
    static u8 img[LIGHT_PIXELS];
    const NphRenderFrameStats stats = light_render(&lf, &v, 3U, img);
    NTEST_EXPECT_EQ_UINT(stats.lights, 8U);
    NTEST_EXPECT_EQ_UINT(stats.vis_lights, 6U);
    GridReadback g = grid_read(f);
    NTEST_EXPECT_EQ_UINT(g.globals, 2U);
    NTEST_ASSERT_EQ_UINT(g.locals, 4U);
    NTEST_EXPECT_EQ_UINT(g.tiles_x, 16U);
    NTEST_EXPECT_EQ_UINT(g.tiles_y, 10U);

    u32 min_bin = NPH_U32_MAX;
    u32 max_bin = 0U;
    for (u32 i = 0; i < 4U; ++i) {
        // Find the light among the sorted ones by its position.
        const NphVec3 rel = nph_dvec3_relative(locals[i].position, cam.position);
        u32 j = NPH_U32_MAX;
        for (u32 k = 0; k < g.locals; ++k) {
            const NphVec3 p =
                nph_vec3(g.lights[k].position.x, g.lights[k].position.y, g.lights[k].position.z);
            if (nph_vec3_length(nph_vec3_sub(p, rel)) < 1e-4F) {
                j = k;
            }
        }
        NTEST_ASSERT_NE_UINT(j, NPH_U32_MAX);
        const LightBounds b = light_bounds(&locals[i], &cam);
        min_bin = nph_min_u32(min_bin, b.bin_lo);
        max_bin = nph_max_u32(max_bin, b.bin_hi);
        u32 missing = 0U;
        u32 stray = 0U;
        for (u32 ty = 0; ty < g.tiles_y; ++ty) {
            for (u32 tx = 0; tx < g.tiles_x; ++tx) {
                const bool inside = tx >= b.tile_lo[0] && tx <= b.tile_hi[0] &&
                                    ty >= b.tile_lo[1] && ty <= b.tile_hi[1];
                const bool near = tx + 1U >= b.tile_lo[0] && tx <= b.tile_hi[0] + 1U &&
                                  ty + 1U >= b.tile_lo[1] && ty <= b.tile_hi[1] + 1U;
                const bool has = grid_tile_has(&g, tx, ty, j);
                missing += inside && !has ? 1U : 0U;
                stray += !near && has ? 1U : 0U;
            }
        }
        u32 bins_missing = 0U;
        for (u32 bin = b.bin_lo; bin <= b.bin_hi; ++bin) {
            bins_missing += grid_bin_has(&g, bin, j) ? 0U : 1U;
        }
        NTEST_LOG("light %u (sorted %u): tiles [%u, %u] x [%u, %u], bins [%u, %u]: %u tiles "
                  "missing, %u stray, %u bins missing",
                  i, j, b.tile_lo[0], b.tile_hi[0], b.tile_lo[1], b.tile_hi[1], b.bin_lo, b.bin_hi,
                  missing, stray, bins_missing);
        NTEST_EXPECT_EQ_UINT(missing, 0U);
        NTEST_EXPECT_EQ_UINT(stray, 0U);
        NTEST_EXPECT_EQ_UINT(bins_missing, 0U);
    }
    // Bins nearer or farther than every light are empty.
    u32 nonempty_outside = 0U;
    for (u32 bin = 0; bin < NPH_RENDER_ZBINS; ++bin) {
        const bool outside = bin + 2U < min_bin || bin > max_bin + 2U;
        const u32 first = g.zbins[bin] & 0xFFFFU;
        const u32 last = g.zbins[bin] >> 16U;
        nonempty_outside += outside && first <= last ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(nonempty_outside, 0U);
    grid_free(&g);
    light_teardown(&lf);
}

// Hundreds of lights of finite range, around and behind the camera and one
// holding it, with one of unlimited range and the sun: lighting each pixel
// with the lights its bin and tile may meet must give the image of
// lighting it with every light (the grid only skips lights that contribute
// exactly nothing), up to the order of the sums.
NTEST(vis, the_light_grid_never_changes_the_image)
{
    LightFixture lf;
    REQUIRE_LIGHTS(&lf, 320U);
    RenderFixture *f = &lf.f;
    light_room(f);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0x11647ULL, 3U);
    for (u32 i = 0; i < 300U; ++i) {
        const NphRenderLightDesc ld = random_light(&rng);
        (void)make_light(f, &ld);
    }
    (void)make_point(f, nph_dvec3(0.1, 1.4, 4.9), 1.5F, 30.0F); // holds the camera
    (void)make_point(f, nph_dvec3(0.0, 3.0, 0.0), 0.0F, 15.0F); // unlimited range
    NphRenderLightDesc sun;
    memset(&sun, 0, sizeof sun);
    sun.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    sun.direction = nph_vec3(0.3F, -1.0F, -0.2F);
    sun.intensity = 100.0F;
    sun.color = nph_vec3_one();
    (void)make_light(f, &sun);

    static u8 all[LIGHT_PIXELS];
    static u8 grid[LIGHT_PIXELS];
    static u8 ref[LIGHT_PIXELS];
    NphRenderView v;
    light_view(&v, (u32)NPH_RENDER_VIS_NO_LIGHT_GRID);
    (void)light_render(&lf, &v, 2U, all);
    light_view(&v, 0U);
    const NphRenderFrameStats stats = light_render(&lf, &v, 3U, grid);
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    (void)light_render(&lf, &v, 1U, ref);
    const ImageDiff d = light_diff(all, grid, 0U);
    NTEST_LOG("every light vs the grid: %.2f %% identical, mean %.4f, max %u",
              (f64)d.within * 100.0, (f64)d.mean, d.max);
    NTEST_EXPECT_LE_UINT(d.max, 1U);
    NTEST_EXPECT_TRUE(d.within >= 0.99F);
    const ImageDiff dr = light_diff(ref, grid, 3U);
    NTEST_LOG("reference vs the grid: %.2f %% within 3 levels, mean %.3f, max %u",
              (f64)dr.within * 100.0, (f64)dr.mean, dr.max);
    NTEST_EXPECT_TRUE(dr.within >= 0.97F);
    NTEST_EXPECT_TRUE(dr.mean <= 1.0F);
    // The scene is lit by the lights, not just the sun: a dark image would
    // pass the comparisons above trivially.
    u32 bright = 0U;
    for (u32 i = 0; i < LIGHT_W * LIGHT_H; ++i) {
        const u8 *px = &grid[(u64)i * 4U];
        bright += (u32)px[0] + px[1] + px[2] > 240U ? 1U : 0U;
    }
    NTEST_LOG("%u of %u pixels bright; %u lights in view", bright, LIGHT_W * LIGHT_H,
              stats.vis_lights);
    NTEST_EXPECT_GT_UINT(bright, LIGHT_W * LIGHT_H / 20U);
    NTEST_EXPECT_GT_UINT(stats.vis_lights, 50U);
    NTEST_EXPECT_LT_UINT(stats.vis_lights, 303U);
    light_teardown(&lf);
}

// Lights move, change, go, and come between frames: the GPU table follows,
// and the grid's image stays that of every light.
NTEST(vis, lights_move_change_and_go)
{
    LightFixture lf;
    REQUIRE_LIGHTS(&lf, 64U);
    RenderFixture *f = &lf.f;
    light_room(f);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0xF00DULL, 5U);
    NphRenderLight lights[48];
    for (u32 i = 0; i < 48U; ++i) {
        const NphRenderLightDesc ld = random_light(&rng);
        lights[i] = make_light(f, &ld);
    }
    static u8 all[LIGHT_PIXELS];
    static u8 grid[LIGHT_PIXELS];
    static u8 first[LIGHT_PIXELS];
    NphRenderView v;
    for (u32 step = 0; step < 4U; ++step) {
        if (step == 1U) {
            // Move a third, recolour a third.
            for (u32 i = 0; i < 32U; ++i) {
                const NphRenderLightDesc ld = random_light(&rng);
                NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, lights[i], &ld),
                                    NPH_RESULT_OK);
            }
        } else if (step == 2U) {
            // Destroy some and create others in their slots.
            for (u32 i = 0; i < 16U; ++i) {
                nph_render_light_destroy(f->renderer, lights[(u64)i * 3U]);
            }
            for (u32 i = 0; i < 16U; ++i) {
                const NphRenderLightDesc ld = random_light(&rng);
                lights[(u64)i * 3U] = make_light(f, &ld);
            }
        } else if (step == 3U) {
            for (u32 i = 0; i < 48U; ++i) {
                nph_render_light_destroy(f->renderer, lights[i]);
            }
        }
        light_view(&v, (u32)NPH_RENDER_VIS_NO_LIGHT_GRID);
        (void)light_render(&lf, &v, 1U, all);
        light_view(&v, 0U);
        (void)light_render(&lf, &v, 1U, grid);
        const ImageDiff d = light_diff(all, grid, 0U);
        NTEST_LOG("step %u: every light vs the grid: %.2f %% identical, max %u", step,
                  (f64)d.within * 100.0, d.max);
        NTEST_EXPECT_LE_UINT(d.max, 1U);
        if (step == 0U) {
            memcpy(first, grid, LIGHT_PIXELS);
        }
    }
    // With every light gone the room is dark (no environment), and it was
    // not before.
    u32 lit = 0U;
    for (u32 i = 0; i < LIGHT_W * LIGHT_H; ++i) {
        lit += memcmp(&first[(u64)i * 4U], &grid[(u64)i * 4U], 3U) != 0 ? 1U : 0U;
    }
    NTEST_EXPECT_GT_UINT(lit, LIGHT_W * LIGHT_H / 10U);
    light_teardown(&lf);
}

NTEST(vis, more_lights_than_a_view_can_sort_are_refused)
{
    NphGpuDevice *dev = test_gpu_create(NULL);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphRendererDesc d;
    test_renderer_desc(&d, dev);
    d.max_lights = (u32)NPH_RENDER_MAX_LIGHTS + 1U;
    NphRenderer *r = NULL;
    NTEST_EXPECT_EQ_INT(nph_renderer_create(&d, &r), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(r);
    test_gpu_destroy(dev);
}

// The shading tiles: every 8 x 8 tile lands in exactly one bin; the sky
// bin's tiles show the background alone and the others a surface
// somewhere. The target's last column and row of tiles are partial.
NTEST(vis, shading_tiles_are_binned_by_what_they_hold)
{
    LightFixture lf;
    REQUIRE_LIGHTS(&lf, 8U);
    RenderFixture *f = &lf.f;
    Scene s;
    scene_build(f, &s, nph_dvec3_zero());
    NphRenderView v;
    scene_view(&v, nph_dvec3_zero(), NPH_RENDER_PIPELINE_PRODUCTION);
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3(3000.0F, 6000.0F, 9000.0F);
    static u8 img[LIGHT_PIXELS];
    const NphRenderFrameStats stats = light_render(&lf, &v, 3U, img);
    const RenderVis *vis = &f->renderer->vis;
    const u32 tiles_x = (LIGHT_W + 7U) / 8U;
    const u32 tiles_y = (LIGHT_H + 7U) / 8U;
    NTEST_ASSERT_EQ_UINT(vis->shade_tiles_x, tiles_x);
    NTEST_ASSERT_EQ_UINT(vis->shade_tiles_y, tiles_y);
    NphVisCounters c;
    test_read_buffer(f->dev, vis->counters, 0U, sizeof c, &c);
    NTEST_EXPECT_EQ_UINT(c.tiles[NPH_VIS_BIN_SKY] + c.tiles[NPH_VIS_BIN_LIT], tiles_x * tiles_y);
    NTEST_EXPECT_GT_UINT(c.tiles[NPH_VIS_BIN_SKY], 0U);
    NTEST_EXPECT_GT_UINT(c.tiles[NPH_VIS_BIN_LIT], 0U);
    NTEST_EXPECT_EQ_UINT(stats.vis_tiles_sky + stats.vis_tiles_lit, tiles_x * tiles_y);
    static u32 lists[NPH_VIS_BINS][((LIGHT_W + 7U) / 8U) * ((LIGHT_H + 7U) / 8U)];
    static u8 seen[((LIGHT_W + 7U) / 8U) * ((LIGHT_H + 7U) / 8U)];
    memset(seen, 0, sizeof seen);
    // The background as the image shows it: a pixel of a sky tile.
    const u8 *background = NULL;
    u32 wrong = 0U;
    for (u32 b = 0; b < (u32)NPH_VIS_BINS; ++b) {
        test_read_buffer(f->dev, vis->tile_lists, (u64)b * tiles_x * tiles_y * sizeof(u32),
                         (u64)c.tiles[b] * sizeof(u32), lists[b]);
        for (u32 i = 0; i < c.tiles[b]; ++i) {
            const u32 tx = lists[b][i] & 0xFFFFU;
            const u32 ty = lists[b][i] >> 16U;
            NTEST_ASSERT_LT_UINT(tx, tiles_x);
            NTEST_ASSERT_LT_UINT(ty, tiles_y);
            seen[ty * tiles_x + tx] += 1U;
            if (b == (u32)NPH_VIS_BIN_SKY && background == NULL) {
                background = &img[((u64)ty * 8U * LIGHT_W + (u64)tx * 8U) * 4U];
            }
        }
    }
    NTEST_ASSERT_NOT_NULL(background);
    for (u32 t = 0; t < tiles_x * tiles_y; ++t) {
        wrong += seen[t] != 1U ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(wrong, 0U);
    u32 sky_with_surface = 0U;
    u32 lit_without = 0U;
    for (u32 b = 0; b < (u32)NPH_VIS_BINS; ++b) {
        for (u32 i = 0; i < c.tiles[b]; ++i) {
            const u32 tx = lists[b][i] & 0xFFFFU;
            const u32 ty = lists[b][i] >> 16U;
            u32 surface = 0U;
            for (u32 y = ty * 8U; y < nph_min_u32(ty * 8U + 8U, LIGHT_H); ++y) {
                for (u32 x = tx * 8U; x < nph_min_u32(tx * 8U + 8U, LIGHT_W); ++x) {
                    surface +=
                        memcmp(&img[((u64)y * LIGHT_W + x) * 4U], background, 3U) != 0 ? 1U : 0U;
                }
            }
            sky_with_surface += b == (u32)NPH_VIS_BIN_SKY && surface != 0U ? 1U : 0U;
            lit_without += b == (u32)NPH_VIS_BIN_LIT && surface == 0U ? 1U : 0U;
        }
    }
    NTEST_LOG("%u sky tiles, %u lit; %u sky tiles show a surface, %u lit ones none",
              c.tiles[NPH_VIS_BIN_SKY], c.tiles[NPH_VIS_BIN_LIT], sky_with_surface, lit_without);
    NTEST_EXPECT_EQ_UINT(sky_with_surface, 0U);
    NTEST_EXPECT_EQ_UINT(lit_without, 0U);
    light_teardown(&lf);
}
