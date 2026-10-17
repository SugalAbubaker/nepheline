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
    return c;
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
    }
    expect_tally("projected", &projected);
    expect_tally("in frustum", &frustum);
    expect_tally("cone culled", &cone);
    expect_tally("front facing", &facing);
    expect_tally("triangle culled", &triangle);
    expect_tally("hzb occluded", &occluded);
    NTEST_EXPECT_EQ_UINT(rect_wrong, 0U);
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
