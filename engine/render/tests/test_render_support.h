// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the render tests: a headless device (skipped without Vulkan,
// validation gate and leak check on teardown, test_gpu_support.h), a
// renderer and a render graph on it, and helpers that render one frame
// into an offscreen target and read it back.

#include <nepheline/math/scalar.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/render/primitives.h>
#include <nepheline/render/reference.h>
#include <nepheline/render/renderer.h>

#include "test_gpu_support.h"

#define TEST_TARGET_SIZE 64U

typedef struct RenderFixture {
    NphGpuDevice *dev;
    NphRenderer *renderer;
    NphGpuRenderGraph *graph;
    NphGpuImage target; // TEST_TARGET_SIZE square, R8G8B8A8_UNORM unless a test makes its own
    u8 *pixels;         // the last readback (TEST_TARGET_SIZE^2 RGBA8)
} RenderFixture;

// A small renderer: low-resolution lighting tables keep lavapipe fast.
static inline void test_renderer_desc(NphRendererDesc *d, NphGpuDevice *dev)
{
    memset(d, 0, sizeof *d);
    d->alloc = nph_allocator_system();
    d->dev = dev;
    d->shader_dir = NPH_SHADER_DIR;
    d->max_textures = 16U;
    d->max_meshes = 16U;
    d->max_materials = 16U;
    d->max_instances = 64U;
    d->max_lights = 8U;
    d->max_draws = 64U;
    d->debug_lines = 64U;
    d->environment_size = 64U;
    d->specular_size = 32U;
    d->specular_mips = 4U;
    d->specular_samples = 64U;
    d->brdf_lut_size = 32U;
    d->max_anisotropy = 1.0F;
    // The production path's lists, sized for test scenes.
    d->max_visible_clusters = 4096U;
    d->max_cull_meshlets = 65536U;
    d->max_deferred = 4096U;
    d->max_instance_updates = 256U;
    d->meshlet_index_pool = 1U << 20U;
    // The virtual shadow map's pool: 4 MB, enough for test scenes.
    d->vsm_pages = 64U;
    d->max_vsm_meshlets = 65536U;
    d->max_vsm_draws = 65536U;
    d->name = "test renderer";
}

static inline NphGpuImage test_make_target(NphGpuDevice *dev, NphGpuFormat format, u32 size)
{
    return test_make_image_2d(dev, size, size, format, 1U,
                              (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT |
                                  (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC,
                              "test target");
}

static inline void test_render_setup(RenderFixture *f, const NphRendererDesc *override)
{
    memset(f, 0, sizeof *f);
    f->dev = test_gpu_create(NULL);
    if (f->dev == NULL) {
        return;
    }
    NphRendererDesc d;
    if (override != NULL) {
        d = *override;
        d.dev = f->dev;
    } else {
        test_renderer_desc(&d, f->dev);
    }
    NTEST_ASSERT_EQ_INT(nph_renderer_create(&d, &f->renderer), NPH_RESULT_OK);
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    gd.name = "render test graph";
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(f->dev, &gd, &f->graph), NPH_RESULT_OK);
    f->target = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, TEST_TARGET_SIZE);
    f->pixels = NPH_ALLOC_ARRAY_ZEROED(nph_allocator_system(), u8,
                                       (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U);
    NTEST_ASSERT_NOT_NULL(f->pixels);
}

static inline void test_render_teardown(RenderFixture *f)
{
    if (f->pixels != NULL) {
        NPH_FREE_ARRAY(nph_allocator_system(), u8, f->pixels,
                       (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U);
    }
    if (f->dev != NULL) {
        nph_gpu_render_graph_destroy(f->graph);
        nph_renderer_destroy(f->renderer);
        nph_gpu_image_destroy(f->dev, f->target);
    }
    test_gpu_destroy(f->dev);
}

// A camera on +Z looking at the origin.
static inline NphRenderCamera test_camera(void)
{
    return nph_render_camera_look_at(nph_dvec3(0.0, 0.0, 4.0), nph_dvec3(0.0, 0.0, 0.0),
                                     nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F);
}

// Renders one frame of `view` into `target` and returns the frame's stats.
static inline NphRenderFrameStats test_render_frame(RenderFixture *f, const NphRenderView *view,
                                                    NphGpuImage target)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    nph_gpu_rg_begin(f->graph);
    NTEST_ASSERT_EQ_INT(nph_render_frame(f->renderer, view, f->graph, target, NULL, &stats),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(f->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    return stats;
}

// Renders a frame into the fixture's target and reads it back into
// f->pixels.
static inline NphRenderFrameStats test_render_read(RenderFixture *f, const NphRenderView *view)
{
    const NphRenderFrameStats stats = test_render_frame(f, view, f->target);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, f->target, 0U, f->pixels,
                    (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U);
    return stats;
}

static inline const u8 *test_pixel(const RenderFixture *f, u32 x, u32 y)
{
    return f->pixels + ((u64)y * TEST_TARGET_SIZE + x) * 4U;
}

// Uploads a mesh built from `data` and returns its handle.
static inline NphRenderMesh test_upload_mesh(NphRenderer *r, const NphRenderMeshData *data)
{
    NphRenderSubmesh sub;
    NphRenderMeshDesc md;
    nph_render_mesh_data_desc(data, "test mesh", &sub, &md);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(r, &md, &mesh), NPH_RESULT_OK);
    return mesh;
}

// Averages of images in linear light, for ground truths made of many
// frames (motion blur's instants, depth of field's lens points): sRGB
// values, as an R8G8B8A8_UNORM target holds them.
typedef struct TestAccumulator {
    f32 *sum;
    u64 count; // values (pixels * 4)
    u32 images;
} TestAccumulator;

static inline void test_accumulator_begin(TestAccumulator *a, u64 values)
{
    a->sum = NPH_ALLOC_ARRAY_ZEROED(nph_allocator_system(), f32, values);
    NTEST_ASSERT_NOT_NULL(a->sum);
    a->count = values;
    a->images = 0U;
}

static inline void test_accumulator_add(TestAccumulator *a, const u8 *image)
{
    for (u64 i = 0; i < a->count; ++i) {
        a->sum[i] += nph_render_ref_srgb_to_linear((f32)image[i] / 255.0F);
    }
    a->images += 1U;
}

// The average, encoded back as the target holds it, into `out`; frees the
// sums.
static inline void test_accumulator_end(TestAccumulator *a, u8 *out)
{
    for (u64 i = 0; i < a->count; ++i) {
        const f32 mean = nph_render_ref_linear_to_srgb(a->sum[i] / (f32)a->images);
        out[i] = (u8)nph_f32_round(nph_f32_clamp(mean, 0.0F, 1.0F) * 255.0F);
    }
    NPH_FREE_ARRAY(nph_allocator_system(), f32, a->sum, a->count);
    a->sum = NULL;
}

typedef struct TestImageDiff {
    f64 mean; // mean absolute difference over every pixel's RGB, in 8-bit levels
    u32 max;  // the largest
} TestImageDiff;

static inline TestImageDiff test_image_diff(const u8 *a, const u8 *b, u64 pixels)
{
    TestImageDiff d = {0.0, 0U};
    u64 sum = 0U;
    for (u64 p = 0; p < pixels; ++p) {
        for (u32 c = 0; c < 3U; ++c) {
            const i32 e = (i32)a[p * 4U + c] - (i32)b[p * 4U + c];
            const u32 m = (u32)(e < 0 ? -e : e);
            sum += m;
            d.max = nph_max_u32(d.max, m);
        }
    }
    d.mean = (f64)sum / (f64)(pixels * 3U);
    return d;
}

static inline void test_log_diff(const char *what, TestImageDiff d)
{
    NTEST_LOG("%s: mean %.3f levels, max %u", what, d.mean, d.max);
}

#define REQUIRE_RENDERER(f)                                                                        \
    do {                                                                                           \
        if ((f)->dev == NULL) {                                                                    \
            NTEST_SKIP("no Vulkan device available");                                              \
        }                                                                                          \
    } while (0)
