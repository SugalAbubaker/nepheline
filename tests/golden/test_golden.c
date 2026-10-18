// SPDX-License-Identifier: Apache-2.0
// Golden-image tests of the PBR reference renderer (CLAUDE.md §9 "GPU
// golden-image tests: render test scenes headlessly (lavapipe in CI, real
// GPUs locally) and compare with NVIDIA FLIP against approved baselines";
// §12 Phase 7 exit criterion; docs/golden.md; ADR-0019).
//
// Each test builds a scene, renders one 256 x 256 frame into an sRGB
// target on a headless device, reads it back, writes it to the output
// directory, and compares it with tests/golden/baselines/<name>.png with
// LDR-FLIP (tools/golden). It passes when the mean error and the 99th
// percentile both stay under the thresholds of ADR-0019. A new or changed
// image is approved with scripts/golden_approve.py, after a person has
// looked at it and at its FLIP map (<name>.flip.png beside it).
//
// Where the device has it, the production path renders every scene too
// (<name>.production.png) and is held to the same baseline and thresholds:
// the reference renderer's images are what it must reproduce (ADR-0029).
//
// The fixture's teardown keeps the gpu tests' gate: zero validation and
// synchronisation-validation errors, nothing leaked.
#include <nepheline/asset/vfs.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/render/primitives.h>
#include <nepheline/render/renderer.h>

#include "cooked_scene.h"
#include "test_gpu_support.h"

#include <golden/flip.h>
#include <golden/png.h>

#ifndef NPH_GOLDEN_BASELINE_DIR
#error "NPH_GOLDEN_BASELINE_DIR must name tests/golden/baselines"
#endif
#ifndef NPH_GOLDEN_OUTPUT_DIR
#error "NPH_GOLDEN_OUTPUT_DIR must name where rendered images are written"
#endif

#define GOLDEN_SIZE 256U

// The thresholds of ADR-0019, from docs/golden.md "Calibration": a real
// GPU's rounding against the lavapipe baselines stays under a mean of 0.021
// and a p99 of 0.052, while a 3.5 % change of brightness scores a mean of
// 0.036 to 0.062 in every lit scene and a local break a p99 near 0.2.
#define GOLDEN_MEAN_MAX 0.03
#define GOLDEN_P99_MAX  0.15F

typedef struct Golden {
    NphGpuDevice *dev;
    NphRenderer *renderer;
    NphGpuRenderGraph *graph;
    NphGpuImage target;
    u8 *pixels;
    // Cooked content, created by the tests that need it.
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;
    CookedScene scene;
    // Resources a scene creates, released by the teardown.
    NphRenderMesh meshes[8];
    u32 mesh_count;
    NphRenderMaterial materials[80];
    u32 material_count;
    NphRenderInstance instances[80];
    u32 instance_count;
    NphRenderLight lights[8];
    u32 light_count;
    NphRenderTexture textures[4];
    u32 texture_count;
    // Draws what lasts one frame (debug lines) before each render, so both
    // pipelines' frames get it.
    void (*before_frame)(struct Golden *f);
} Golden;

NTEST_SETUP(Golden)
{
    memset(f, 0, sizeof *f);
    f->dev = test_gpu_create(NULL);
    if (f->dev == NULL) {
        return;
    }
    // Small lighting tables keep lavapipe fast; anisotropic filtering is
    // off because its footprint is implementation-defined.
    NphRendererDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.alloc = nph_allocator_system();
    rd.dev = f->dev;
    rd.shader_dir = NPH_SHADER_DIR;
    rd.max_instances = 256U;
    rd.max_lights = 16U;
    rd.max_draws = 256U;
    rd.debug_lines = 4096U;
    rd.environment_size = 128U;
    rd.specular_size = 64U;
    rd.specular_mips = 6U;
    rd.specular_samples = 128U;
    rd.brdf_lut_size = 64U;
    rd.max_anisotropy = 1.0F;
    // The production path's lists, sized for these scenes.
    rd.max_visible_clusters = 65536U;
    rd.max_cull_meshlets = 65536U;
    rd.max_deferred = 32768U;
    rd.max_instance_updates = 256U;
    rd.meshlet_index_pool = 1U << 22U;
    rd.name = "golden";
    NTEST_ASSERT_EQ_INT(nph_renderer_create(&rd, &f->renderer), NPH_RESULT_OK);
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    gd.name = "golden";
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(f->dev, &gd, &f->graph), NPH_RESULT_OK);
    f->target = test_make_image_2d(
        f->dev, GOLDEN_SIZE, GOLDEN_SIZE, NPH_GPU_FORMAT_R8G8B8A8_SRGB, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC,
        "golden target");
    f->pixels = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, (u64)GOLDEN_SIZE * GOLDEN_SIZE * 4U);
    NTEST_ASSERT_NOT_NULL(f->pixels);
}

NTEST_TEARDOWN(Golden)
{
    if (f->dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(f->dev));
        cooked_scene_unload(&f->scene);
        for (u32 i = 0; i < f->instance_count; ++i) {
            nph_render_instance_destroy(f->renderer, f->instances[i]);
        }
        for (u32 i = 0; i < f->light_count; ++i) {
            nph_render_light_destroy(f->renderer, f->lights[i]);
        }
        for (u32 i = 0; i < f->material_count; ++i) {
            nph_render_material_destroy(f->renderer, f->materials[i]);
        }
        for (u32 i = 0; i < f->mesh_count; ++i) {
            nph_render_mesh_destroy(f->renderer, f->meshes[i]);
        }
        for (u32 i = 0; i < f->texture_count; ++i) {
            nph_render_texture_destroy(f->renderer, f->textures[i]);
        }
        nph_gpu_render_graph_destroy(f->graph);
        nph_renderer_destroy(f->renderer);
        nph_gpu_image_destroy(f->dev, f->target);
    }
    nph_asset_loader_destroy(f->loader);
    nph_vfs_destroy(f->vfs);
    nph_job_system_destroy(f->jobs);
    if (f->pixels != NULL) {
        NPH_FREE_ARRAY(nph_allocator_system(), u8, f->pixels, (u64)GOLDEN_SIZE * GOLDEN_SIZE * 4U);
    }
    test_gpu_destroy(f->dev);
}

#define REQUIRE_GOLDEN(f)                                                                          \
    do {                                                                                           \
        if ((f)->dev == NULL) {                                                                    \
            NTEST_SKIP("no Vulkan device available");                                              \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// The comparison
// ---------------------------------------------------------------------------

// Renders `view`, writes <output>/<file>.png, and compares it with the
// approved baseline <name>.png.
static void golden_compare(Golden *f, const char *name, const char *file, const NphRenderView *view)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    nph_gpu_rg_begin(f->graph);
    if (f->before_frame != NULL) {
        f->before_frame(f);
    }
    NTEST_ASSERT_EQ_INT(nph_render_frame(f->renderer, view, f->graph, f->target, NULL, NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const u64 bytes = (u64)GOLDEN_SIZE * GOLDEN_SIZE * 4U;
    test_read_image(f->dev, f->target, 0U, f->pixels, bytes);

    NPH_IGNORE_RESULT(nph_fs_create_directories(NPH_GOLDEN_OUTPUT_DIR));
    char out_path[512];
    snprintf(out_path, sizeof out_path, "%s/%s.png", NPH_GOLDEN_OUTPUT_DIR, file);
    NTEST_ASSERT_EQ_INT(golden_png_write(out_path, f->pixels, GOLDEN_SIZE, GOLDEN_SIZE, 3U),
                        NPH_RESULT_OK);

    char base_path[512];
    snprintf(base_path, sizeof base_path, "%s/%s.png", NPH_GOLDEN_BASELINE_DIR, name);
    GoldenImage base;
    const NphResult r = golden_png_read(base_path, nph_allocator_system(), &base);
    if (r != NPH_RESULT_OK) {
        printf("golden %s: no baseline (%s); review %s, then approve it with "
               "scripts/golden_approve.py\n",
               name, nph_result_string(r), out_path);
        NTEST_FAIL("no approved baseline for %s", name);
    }
    NTEST_ASSERT_EQ_UINT(base.width, GOLDEN_SIZE);
    NTEST_ASSERT_EQ_UINT(base.height, GOLDEN_SIZE);
    f32 *map = NPH_ALLOC_ARRAY(nph_allocator_system(), f32, (u64)GOLDEN_SIZE * GOLDEN_SIZE);
    NTEST_ASSERT_NOT_NULL(map);
    GoldenFlipStats s;
    memset(&s, 0, sizeof s);
    NTEST_ASSERT_EQ_INT(golden_flip(base.rgba, f->pixels, GOLDEN_SIZE, GOLDEN_SIZE,
                                    GOLDEN_FLIP_DEFAULT_PPD, nph_allocator_system(), map, &s),
                        NPH_RESULT_OK);
    golden_flip_magma(map, GOLDEN_SIZE, GOLDEN_SIZE, f->pixels);
    char map_path[512];
    snprintf(map_path, sizeof map_path, "%s/%s.flip.png", NPH_GOLDEN_OUTPUT_DIR, file);
    NPH_IGNORE_RESULT(golden_png_write(map_path, f->pixels, GOLDEN_SIZE, GOLDEN_SIZE, 3U));
    printf("golden %s: FLIP mean %.5f p95 %.4f p99 %.4f max %.4f (%s)\n", file, s.mean, (f64)s.p95,
           (f64)s.p99, (f64)s.max, nph_gpu_device_info(f->dev)->name);
    NPH_FREE_ARRAY(nph_allocator_system(), f32, map, (u64)GOLDEN_SIZE * GOLDEN_SIZE);
    golden_image_free(&base);
    NTEST_EXPECT_TRUE(s.mean <= GOLDEN_MEAN_MAX);
    NTEST_EXPECT_TRUE(s.p99 <= GOLDEN_P99_MAX);
}

// The scene through the reference renderer, then through the production
// path where the device has it, both against the one baseline.
static void golden_check(Golden *f, const char *name, const NphRenderView *view)
{
    NphRenderView v = *view;
    v.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    golden_compare(f, name, name, &v);
    if (nph_renderer_production_supported(f->renderer)) {
        char file[128];
        snprintf(file, sizeof file, "%s.production", name);
        v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
        golden_compare(f, name, file, &v);
    }
}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

static NphRenderMesh add_mesh(Golden *f, NphRenderMeshData *data)
{
    NphRenderSubmesh sub;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(data, "golden mesh", &sub, &md);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &md, &mesh), NPH_RESULT_OK);
    nph_render_mesh_data_free(data);
    NTEST_ASSERT_LT_UINT(f->mesh_count, 8U);
    f->meshes[f->mesh_count++] = mesh;
    return mesh;
}

static NphRenderMesh sphere_mesh(Golden *f, f32 radius)
{
    NphRenderMeshData d;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_sphere(nph_allocator_system(), radius, 48U, 24U, &d),
                        NPH_RESULT_OK);
    return add_mesh(f, &d);
}

static NphRenderMesh cube_mesh(Golden *f, f32 size)
{
    NphRenderMeshData d;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), size, &d), NPH_RESULT_OK);
    return add_mesh(f, &d);
}

static NphRenderMesh plane_mesh(Golden *f, f32 size)
{
    NphRenderMeshData d;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), size, 4U, 1.0F, &d),
                        NPH_RESULT_OK);
    return add_mesh(f, &d);
}

static NphRenderMaterial add_material(Golden *f, const NphRenderMaterialDesc *md)
{
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, md, &m), NPH_RESULT_OK);
    NTEST_ASSERT_LT_UINT(f->material_count, 80U);
    f->materials[f->material_count++] = m;
    return m;
}

static NphRenderMaterial simple_material(Golden *f, NphVec3 color, f32 metallic, f32 roughness)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4_from_vec3(color, 1.0F);
    md.metallic = metallic;
    md.roughness = roughness;
    return add_material(f, &md);
}

static void add_instance(Golden *f, NphRenderMesh mesh, NphRenderMaterial mat, NphRenderTransform t)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = t;
    NTEST_ASSERT_LT_UINT(f->instance_count, 80U);
    NTEST_ASSERT_EQ_INT(
        nph_render_instance_create(f->renderer, &id, &f->instances[f->instance_count]),
        NPH_RESULT_OK);
    ++f->instance_count;
}

static NphRenderTransform at(f64 x, f64 y, f64 z)
{
    NphRenderTransform t = nph_render_transform_identity();
    t.translation = nph_dvec3(x, y, z);
    return t;
}

static void add_light(Golden *f, const NphRenderLightDesc *ld)
{
    NTEST_ASSERT_LT_UINT(f->light_count, 8U);
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, ld, &f->lights[f->light_count]),
                        NPH_RESULT_OK);
    ++f->light_count;
}

static void sun(Golden *f, NphVec3 direction, NphVec3 color, f32 intensity)
{
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = color;
    ld.intensity = intensity;
    ld.direction = nph_vec3_normalize(direction);
    add_light(f, &ld);
}

// A sky in relative units for EV100 0 (exposure 1 / 1.2).
static void relative_sky(Golden *f)
{
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    ed.sky.zenith = nph_vec3(0.35F, 0.55F, 1.0F);
    ed.sky.horizon = nph_vec3(1.0F, 0.95F, 0.88F);
    ed.sky.ground = nph_vec3(0.30F, 0.26F, 0.22F);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
}

static void base_view(NphRenderView *v, NphDVec3 eye, NphDVec3 target, f32 yfov)
{
    nph_render_view_default(v,
                            nph_render_camera_look_at(eye, target, nph_vec3_unit_y(), yfov, 0.05F));
    v->ev100 = 0.0F;
    v->tonemapper = NPH_RENDER_TONEMAPPER_ACES;
}

// ---------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------

// The classic grid: metallic from 0 (bottom) to 1 (top), roughness from
// 0.05 (left) to 0.95 (right), on a copper base colour, under the sky and
// a key light.
NTEST_F(Golden, material_grid)
{
    REQUIRE_GOLDEN(f);
    const NphRenderMesh sphere = sphere_mesh(f, 0.42F);
    for (u32 row = 0; row < 5U; ++row) {
        for (u32 col = 0; col < 5U; ++col) {
            const NphRenderMaterial m = simple_material(f, nph_vec3(0.95F, 0.64F, 0.54F),
                                                        (f32)row / 4.0F, 0.05F + 0.225F * (f32)col);
            add_instance(f, sphere, m, at((f64)col - 2.0, (f64)row - 2.0, 0.0));
        }
    }
    relative_sky(f);
    sun(f, nph_vec3(0.4F, -0.5F, -0.75F), nph_vec3(1.0F, 0.97F, 0.92F), 2.5F);
    NphRenderView v;
    base_view(&v, nph_dvec3(0.0, 0.0, 9.0), nph_dvec3_zero(), 0.62F);
    golden_check(f, "material_grid", &v);
}

// Point and spot lights only, in colour, with ranges, over a floor: the
// falloff windows and cone edges are what the image shows.
NTEST_F(Golden, punctual_lights)
{
    REQUIRE_GOLDEN(f);
    const NphRenderMesh floor = plane_mesh(f, 10.0F);
    const NphRenderMesh cube = cube_mesh(f, 0.8F);
    const NphRenderMesh sphere = sphere_mesh(f, 0.45F);
    add_instance(f, floor, simple_material(f, nph_vec3(0.6F, 0.6F, 0.6F), 0.0F, 0.7F), at(0, 0, 0));
    add_instance(f, cube, simple_material(f, nph_vec3(0.8F, 0.8F, 0.8F), 0.0F, 0.4F),
                 at(-1.4, 0.4, 0.0));
    add_instance(f, cube, simple_material(f, nph_vec3(0.9F, 0.75F, 0.3F), 1.0F, 0.3F),
                 at(1.5, 0.4, -0.6));
    add_instance(f, sphere, simple_material(f, nph_vec3(0.7F, 0.7F, 0.75F), 0.0F, 0.15F),
                 at(0.2, 0.45, 0.9));
    add_instance(f, sphere, simple_material(f, nph_vec3(0.95F, 0.93F, 0.88F), 1.0F, 0.55F),
                 at(0.1, 0.45, -1.4));
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.color = nph_vec3(1.0F, 0.35F, 0.25F);
    ld.intensity = 6.0F;
    ld.position = nph_dvec3(-2.2, 1.2, 1.2);
    ld.range = 5.0F;
    add_light(f, &ld);
    ld.color = nph_vec3(0.3F, 1.0F, 0.45F);
    ld.intensity = 3.0F;
    ld.position = nph_dvec3(2.4, 0.6, 0.8);
    ld.range = 0.0F;
    add_light(f, &ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_SPOT;
    ld.color = nph_vec3(0.45F, 0.6F, 1.0F);
    ld.intensity = 30.0F;
    ld.position = nph_dvec3(0.0, 4.0, 1.5);
    ld.direction = nph_vec3_normalize(nph_vec3(0.0F, -1.0F, -0.45F));
    ld.inner_cone = 0.25F;
    ld.outer_cone = 0.45F;
    add_light(f, &ld);
    NphRenderView v;
    base_view(&v, nph_dvec3(0.0, 3.2, 6.5), nph_dvec3(0.0, 0.3, 0.0), 0.8F);
    v.disable_ibl = 1;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3(0.02F, 0.02F, 0.03F);
    golden_check(f, "punctual_lights", &v);
}

// Ray-traced shadows (docs/render.md "Shadows"): a sun, a point, and a spot
// light, each casting shadows, over a pillar, a turned cube, and spheres
// (one floating) on a floor under the sky: the shadows' shapes where they
// overlap and the sky's light left in them. Skipped without ray queries.
NTEST_F(Golden, shadows)
{
    REQUIRE_GOLDEN(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer)) {
        NTEST_SKIP("no ray queries on this device");
    }
    const NphRenderMesh floor = plane_mesh(f, 10.0F);
    const NphRenderMesh cube = cube_mesh(f, 1.0F);
    const NphRenderMesh sphere = sphere_mesh(f, 0.45F);
    add_instance(f, floor, simple_material(f, nph_vec3(0.6F, 0.6F, 0.6F), 0.0F, 0.7F), at(0, 0, 0));
    add_instance(f, cube, simple_material(f, nph_vec3(0.8F, 0.8F, 0.8F), 0.0F, 0.5F),
                 nph_render_transform_from_trs(nph_dvec3(-1.3, 1.2, -0.9), nph_quat_identity(),
                                               nph_vec3(0.5F, 2.4F, 0.5F)));
    add_instance(f, cube, simple_material(f, nph_vec3(0.75F, 0.25F, 0.2F), 0.0F, 0.45F),
                 nph_render_transform_from_trs(nph_dvec3(1.4, 0.4, 0.1),
                                               nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.6F),
                                               nph_vec3(0.8F, 0.8F, 0.8F)));
    add_instance(f, sphere, simple_material(f, nph_vec3(0.7F, 0.7F, 0.75F), 0.0F, 0.2F),
                 at(-0.1, 0.45, 1.1));
    add_instance(f, sphere, simple_material(f, nph_vec3(0.95F, 0.85F, 0.6F), 1.0F, 0.35F),
                 at(0.3, 1.5, -0.4));
    // The sky dimmed, so what the lights do not reach stays dark.
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    ed.sky.zenith = nph_vec3(0.35F, 0.55F, 1.0F);
    ed.sky.horizon = nph_vec3(1.0F, 0.95F, 0.88F);
    ed.sky.ground = nph_vec3(0.30F, 0.26F, 0.22F);
    ed.intensity = 0.3F;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.shadows = 1;
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 0.96F, 0.9F);
    ld.intensity = 6.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.55F, -1.0F, -0.35F));
    add_light(f, &ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.color = nph_vec3(1.0F, 0.6F, 0.3F);
    ld.intensity = 6.0F;
    ld.position = nph_dvec3(-0.4, 1.4, 2.0);
    ld.range = 6.0F;
    add_light(f, &ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_SPOT;
    ld.color = nph_vec3(0.4F, 0.6F, 1.0F);
    ld.intensity = 50.0F;
    ld.position = nph_dvec3(2.2, 3.6, 1.6);
    ld.direction = nph_vec3_normalize(nph_vec3(-0.55F, -1.0F, -0.45F));
    ld.range = 0.0F;
    ld.inner_cone = 0.3F;
    ld.outer_cone = 0.55F;
    add_light(f, &ld);
    NphRenderView v;
    base_view(&v, nph_dvec3(0.0, 3.4, 6.5), nph_dvec3(0.0, 0.4, 0.0), 0.8F);
    v.ev100 = 1.0F;
    golden_check(f, "shadows", &v);
}

// Emissive swatches doubling in radiance from 2^-8 to 2^7, in white and
// three saturated colours: the curve and the hue behaviour of a tonemapper.
static void tonemap_ramp(Golden *f, NphRenderTonemapper op, const char *name)
{
    NphRenderMeshData d;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 0.42F, 1U, 1.0F, &d),
                        NPH_RESULT_OK);
    const NphRenderMesh tile = add_mesh(f, &d);
    static const f32 hues[4][3] = {
        {1.0F, 1.0F, 1.0F}, {1.0F, 0.12F, 0.06F}, {0.1F, 1.0F, 0.12F}, {0.08F, 0.2F, 1.0F}};
    const NphRenderTransform facing = nph_render_transform_from_trs(
        nph_dvec3_zero(), nph_quat_from_rotation_x(0.5F * NPH_PI), nph_vec3_one());
    for (u32 row = 0; row < 4U; ++row) {
        for (u32 col = 0; col < 16U; ++col) {
            NphRenderMaterialDesc md;
            nph_render_material_desc_default(&md);
            md.base_color = nph_vec4(0.0F, 0.0F, 0.0F, 1.0F);
            const f32 l = nph_f32_pow(2.0F, (f32)col - 8.0F) * 1.2F; // pre-exposed 2^(col - 8)
            md.emissive = nph_vec3(hues[row][0] * l, hues[row][1] * l, hues[row][2] * l);
            NphRenderTransform t = facing;
            t.translation = nph_dvec3(((f64)col - 7.5) * 0.5, (1.5 - (f64)row) * 1.0, 0.0);
            add_instance(f, tile, add_material(f, &md), t);
        }
    }
    NphRenderView v;
    base_view(&v, nph_dvec3(0.0, 0.0, 12.0), nph_dvec3_zero(), 0.68F);
    v.tonemapper = op;
    v.disable_ibl = 1;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3(0.05F, 0.05F, 0.05F);
    golden_check(f, name, &v);
}

NTEST_F(Golden, tonemap_ramp_aces)
{
    REQUIRE_GOLDEN(f);
    tonemap_ramp(f, NPH_RENDER_TONEMAPPER_ACES, "tonemap_ramp_aces");
}

NTEST_F(Golden, tonemap_ramp_agx)
{
    REQUIRE_GOLDEN(f);
    tonemap_ramp(f, NPH_RENDER_TONEMAPPER_AGX, "tonemap_ramp_agx");
}

// Image-based lighting from an equirectangular texture: a dusk gradient
// with a bright warm patch and a cool fill, on metal and dielectric
// spheres from smooth to rough, the environment as the background.
NTEST_F(Golden, equirect_environment)
{
    REQUIRE_GOLDEN(f);
    enum {
        EW = 128,
        EH = 64
    };
    static u16 texels[EW * EH * 4];
    for (u32 y = 0; y < (u32)EH; ++y) {
        const f32 v = ((f32)y + 0.5F) / (f32)EH; // 0 up, 1 down
        for (u32 x = 0; x < (u32)EW; ++x) {
            const f32 u = ((f32)x + 0.5F) / (f32)EW;
            f32 r;
            f32 g;
            f32 b;
            if (v < 0.5F) {
                const f32 h = 1.0F - v * 2.0F; // 1 at the zenith
                r = 0.25F + 0.9F * (1.0F - h);
                g = 0.35F + 0.5F * (1.0F - h);
                b = 0.9F + 0.1F * h;
            } else {
                r = 0.18F;
                g = 0.15F;
                b = 0.12F;
            }
            // A warm patch low in the sky towards u = 0.3, and a cool one
            // opposite.
            const f32 du1 = (u - 0.3F) * 4.0F;
            const f32 dv1 = (v - 0.4F) * 8.0F;
            const f32 patch = 12.0F * nph_f32_exp(-(du1 * du1 + dv1 * dv1));
            const f32 du2 = (u - 0.8F) * 3.0F;
            const f32 dv2 = (v - 0.3F) * 5.0F;
            const f32 cool = 2.0F * nph_f32_exp(-(du2 * du2 + dv2 * dv2));
            u16 *t = &texels[((u64)y * EW + x) * 4U];
            t[0] = nph_f32_to_f16(r + patch * 1.0F + cool * 0.3F);
            t[1] = nph_f32_to_f16(g + patch * 0.6F + cool * 0.5F);
            t[2] = nph_f32_to_f16(b + patch * 0.3F + cool * 1.0F);
            t[3] = nph_f32_to_f16(1.0F);
        }
    }
    const void *mips[1] = {texels};
    const u64 sizes[1] = {sizeof texels};
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    td.width = EW;
    td.height = EH;
    td.mip_count = 1U;
    td.mips = mips;
    td.mip_sizes = sizes;
    td.name = "golden equirect";
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->renderer, &td, &f->textures[0]),
                        NPH_RESULT_OK);
    f->texture_count = 1U;
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_EQUIRECT;
    ed.texture = f->textures[0];
    ed.intensity = 0.45F;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
    const NphRenderMesh sphere = sphere_mesh(f, 0.4F);
    for (u32 i = 0; i < 5U; ++i) {
        const f32 rough = 0.05F + 0.225F * (f32)i;
        add_instance(f, sphere, simple_material(f, nph_vec3(0.97F, 0.96F, 0.92F), 1.0F, rough),
                     at((f64)i - 2.0, 0.55, 0.0));
        add_instance(f, sphere, simple_material(f, nph_vec3(0.2F, 0.35F, 0.75F), 0.0F, rough),
                     at((f64)i - 2.0, -0.55, 0.0));
    }
    NphRenderView v;
    base_view(&v, nph_dvec3(0.0, 0.6, 6.0), nph_dvec3(0.0, 0.0, 0.0), 0.75F);
    golden_check(f, "equirect_environment", &v);
}

// The debug-draw API over a dim scene: depth-tested shapes partly behind a
// cube, overlay shapes on top.
// The debug lines last one frame: the fixture draws them before each.
static void debug_lines(Golden *f)
{
    NphRenderer *r = f->renderer;
    nph_render_debug_grid(r, nph_vec3(0.0F, 0.001F, 0.0F), 0.5F, 12U, NPH_RENDER_COLOR_GRAY,
                          NPH_RENDER_DEBUG_DEPTH_TESTED);
    nph_render_debug_sphere(r, nph_vec3(0.0F, 0.5F, -0.9F), 0.6F, NPH_RENDER_COLOR_CYAN,
                            NPH_RENDER_DEBUG_DEPTH_TESTED, 48U);
    nph_render_debug_aabb(r, nph_aabb(nph_vec3(-0.55F, 0.0F, -0.55F), nph_vec3(0.55F, 1.1F, 0.55F)),
                          NPH_RENDER_COLOR_YELLOW, NPH_RENDER_DEBUG_DEPTH_TESTED);
    const NphAffine axes = nph_affine_from_translation(nph_vec3(-1.6F, 0.0F, 1.0F));
    nph_render_debug_axes(r, &axes, 0.8F, NPH_RENDER_DEBUG_OVERLAY);
    nph_render_debug_arrow(r, nph_vec3(1.8F, 1.6F, 0.6F), nph_vec3(0.6F, 0.8F, 0.4F),
                           NPH_RENDER_COLOR_MAGENTA, NPH_RENDER_DEBUG_OVERLAY);
    nph_render_debug_cross(r, nph_vec3(1.2F, 0.3F, 1.2F), 0.4F, NPH_RENDER_COLOR_RED,
                           NPH_RENDER_DEBUG_DEPTH_TESTED);
    const NphRenderCamera probe = nph_render_camera_look_at(
        nph_dvec3(-2.0, 1.2, -1.0), nph_dvec3(0.0, 0.5, 0.0), nph_vec3_unit_y(), 0.6F, 0.3F);
    nph_render_debug_frustum(r, &probe, 1.5F, 1.6F, NPH_RENDER_COLOR_GREEN,
                             NPH_RENDER_DEBUG_DEPTH_TESTED);
}

NTEST_F(Golden, debug_draw)
{
    REQUIRE_GOLDEN(f);
    const NphRenderMesh floor = plane_mesh(f, 8.0F);
    const NphRenderMesh cube = cube_mesh(f, 1.0F);
    add_instance(f, floor, simple_material(f, nph_vec3(0.3F, 0.3F, 0.32F), 0.0F, 0.9F),
                 at(0, 0, 0));
    add_instance(f, cube, simple_material(f, nph_vec3(0.5F, 0.45F, 0.4F), 0.0F, 0.6F),
                 at(0.0, 0.5, 0.0));
    relative_sky(f);
    f->before_frame = debug_lines;
    NphRenderView v;
    base_view(&v, nph_dvec3(2.6, 2.4, 3.8), nph_dvec3(0.0, 0.4, 0.0), 0.8F);
    golden_check(f, "debug_draw", &v);
}

// ---------------------------------------------------------------------------
// Cooked content: samples/pbr_viewer/content/pbr_scene.gltf through the
// cooker, the asset loader, and the bridge (every material feature)
// ---------------------------------------------------------------------------

#ifdef NPH_GOLDEN_PACK
static void load_cooked(Golden *f)
{
    NphAllocator *a = nph_allocator_system();
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = a;
    jd.worker_count = 2U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &f->jobs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_create(a, &f->vfs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("content"), NPH_GOLDEN_PACK, NULL),
                        NPH_RESULT_OK);
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = a;
    ld.vfs = f->vfs;
    ld.jobs = f->jobs;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&ld, &f->loader), NPH_RESULT_OK);
    CookedSceneDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.alloc = a;
    cd.renderer = f->renderer;
    cd.loader = f->loader;
    cd.jobs = f->jobs;
    cd.scene = nph_guid_from_cstr("pbr_scene.gltf#scene");
    NTEST_ASSERT_EQ_INT(cooked_scene_load(&cd, &f->scene), NPH_RESULT_OK);
    // The pbr_viewer's dusk sky.
    NphRenderEnvironmentDesc ed;
    memset(&ed, 0, sizeof ed);
    ed.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&ed.sky);
    ed.intensity = 0.25F;
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &ed), NPH_RESULT_OK);
}

static void cooked_view(NphRenderView *v)
{
    base_view(v, nph_dvec3(2.2, 2.6, 6.4), nph_dvec3(-0.3, 0.6, 0.0), 0.95F);
    v->ev100 = 11.0F;
}

NTEST_F(Golden, cooked_scene)
{
    REQUIRE_GOLDEN(f);
    load_cooked(f);
    NphRenderView v;
    cooked_view(&v);
    v.tonemapper = NPH_RENDER_TONEMAPPER_AGX;
    golden_check(f, "cooked_scene", &v);
}

NTEST_F(Golden, cooked_scene_normals)
{
    REQUIRE_GOLDEN(f);
    load_cooked(f);
    NphRenderView v;
    cooked_view(&v);
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    v.tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3_zero();
    golden_check(f, "cooked_scene_normals", &v);
}
#endif
