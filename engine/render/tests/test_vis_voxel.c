// SPDX-License-Identifier: Apache-2.0
// Voxels in the production path (render/voxel.h; docs/render.md "Voxels";
// ADR-0061): the shaders' voxel tests against their C twin, ray by ray, and
// the resolve's search for a pixel's voxel against them; a foliage mesh's
// voxel clusters drawn where the CPU's rays through the same clusters meet
// them, pixel for pixel; the coverage of the voxel levels against level 0's
// from near to far; the voxel resolve's look against the voxels'; the light
// a voxel level reflects against the triangles it stands for; the voxel
// passes, present only with voxel meshes; and malformed voxel clusters
// refused. The fixture's teardown asserts zero validation and
// synchronisation-validation errors and nothing leaked.
#include <nepheline/gpu/render_graph.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>
#include <nepheline/render/voxel.h>

#include "render_internal.h"
#include "test_lod_support.h"

#include <math.h>

typedef RenderFixture VoxelGpu;

NTEST_SETUP(VoxelGpu)
{
    // Room for a grid of foliage's clusters.
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_visible_clusters = 1U << 16U;
    d.max_cull_meshlets = 1U << 18U;
    d.max_deferred = 1U << 16U;
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(VoxelGpu)
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

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE)

static NphVec3 random_unit(NphPcg32 *rng)
{
    const f32 z = 2.0F * nph_pcg32_next_f32(rng) - 1.0F;
    const f32 phi = 2.0F * NPH_PI * nph_pcg32_next_f32(rng);
    const f32 r = nph_f32_sqrt(nph_f32_max(1.0F - z * z, 0.0F));
    return nph_vec3(r * nph_f32_cos(phi), r * nph_f32_sin(phi), z);
}

// ---------------------------------------------------------------------------
// The voxels' tests against their C twin
// ---------------------------------------------------------------------------

#define TWIN_BRICKS 256U
#define TWIN_RAYS   64U // a brick

NTEST(VoxelGpu, the_voxel_tests_match_their_cpu_twin)
{
    NphGpuDevice *dev = test_gpu_create(NULL);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphAllocator *a = nph_allocator_system();
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 61U, 2U);
    // Random bricks of every shape, as the CPU test makes them.
    NphRenderVoxelBrick *bricks = NPH_ALLOC_ARRAY_ZEROED(a, NphRenderVoxelBrick, TWIN_BRICKS);
    const u32 voxels_n = TWIN_BRICKS * 64U;
    NphRenderVoxel *voxels = NPH_ALLOC_ARRAY_ZEROED(a, NphRenderVoxel, voxels_n);
    const u32 cases_n = TWIN_BRICKS * TWIN_RAYS;
    NphVisVoxelCase *cases = NPH_ALLOC_ARRAY_ZEROED(a, NphVisVoxelCase, cases_n);
    NTEST_ASSERT_TRUE(bricks != NULL && voxels != NULL && cases != NULL);
    u32 voxel_count = 0U;
    for (u32 k = 0; k < TWIN_BRICKS; ++k) {
        NphRenderVoxelBrick *brick = &bricks[k];
        brick->cell = ldexpf(1.0F, (int)(nph_pcg32_next_u32(&rng) % 8U) - 4);
        brick->origin = nph_vec3((nph_pcg32_next_f32(&rng) - 0.5F) * 20.0F,
                                 (nph_pcg32_next_f32(&rng) - 0.5F) * 20.0F,
                                 (nph_pcg32_next_f32(&rng) - 0.5F) * 20.0F);
        brick->first_voxel = voxel_count;
        for (u32 bit = 0; bit < 64U; ++bit) {
            if (nph_pcg32_next_f32(&rng) >= 0.5F) {
                continue;
            }
            brick->mask[bit >> 5U] |= 1U << (bit & 31U);
            NphRenderVoxel v;
            memset(&v, 0, sizeof v);
            for (u32 ax = 0; ax < 3U; ++ax) {
                const u32 l = nph_pcg32_next_u32(&rng) % 32U;
                const u32 h = l + 1U + nph_pcg32_next_u32(&rng) % (32U - l);
                v.box |= (l << (5U * ax)) | ((h - 1U) << (15U + 5U * ax));
            }
            const u32 shape = nph_pcg32_next_u32(&rng) % 3U;
            v.box |= shape << NPH_RENDER_VOXEL_SHAPE_SHIFT;
            if (shape == NPH_RENDER_VOXEL_SHAPE_CAPSULE) {
                v.material = (nph_pcg32_next_u32(&rng) % 4U) |
                             ((nph_pcg32_next_u32(&rng) % NPH_RENDER_VOXEL_RADIUS_CODES) << 26U);
            }
            voxels[voxel_count++] = v;
        }
        const f32 side = 4.0F * brick->cell;
        const NphVec3 centre = nph_vec3_add(brick->origin, nph_vec3_splat(0.5F * side));
        // Directions of a 64th of the brick, so that it lies past t = 1, as
        // the raster's rays from the near plane meet voxels.
        const f32 speed = side / 64.0F;
        for (u32 r = 0; r < TWIN_RAYS; ++r) {
            const NphVec3 from =
                nph_vec3_add(centre, nph_vec3_scale(random_unit(&rng), 3.0F * side));
            const NphVec3 to =
                nph_vec3_add(brick->origin, nph_vec3(nph_pcg32_next_f32(&rng) * side,
                                                     nph_pcg32_next_f32(&rng) * side,
                                                     nph_pcg32_next_f32(&rng) * side));
            NphVec3 d = nph_vec3_normalize(nph_vec3_sub(to, from));
            if (r % 8U == 0U) {
                // Along an axis: zero components.
                const u32 ax = nph_pcg32_next_u32(&rng) % 3U;
                f32 dd[3] = {0.0F, 0.0F, 0.0F};
                dd[ax] = nph_vec3_at(d, ax) < 0.0F ? -1.0F : 1.0F;
                d = nph_vec3(dd[0], dd[1], dd[2]);
            }
            NphVisVoxelCase *c = &cases[k * TWIN_RAYS + r];
            c->origin = nph_vec4_from_vec3(from, 1.0F);
            c->dir = nph_vec4_from_vec3(nph_vec3_scale(d, speed), NPH_F32_MAX);
            c->brick = k;
        }
    }

    const u32 storage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
    NphGpuBuffer brick_buf = test_make_buffer(dev, (u64)TWIN_BRICKS * sizeof(NphRenderVoxelBrick),
                                              storage, NPH_GPU_MEMORY_UPLOAD, "twin bricks");
    NphGpuBuffer voxel_buf = test_make_buffer(dev, (u64)nph_max_u32(voxel_count, 1U) * 16U, storage,
                                              NPH_GPU_MEMORY_UPLOAD, "twin voxels");
    NphGpuBuffer case_buf = test_make_buffer(dev, (u64)cases_n * sizeof(NphVisVoxelCase), storage,
                                             NPH_GPU_MEMORY_UPLOAD, "twin cases");
    NphGpuBuffer result_buf = test_make_buffer(dev, (u64)cases_n * sizeof(NphVisVoxelResult),
                                               storage, NPH_GPU_MEMORY_READBACK, "twin results");
    memcpy(nph_gpu_buffer_map(dev, brick_buf), bricks, (u64)TWIN_BRICKS * sizeof *bricks);
    memcpy(nph_gpu_buffer_map(dev, voxel_buf), voxels, (u64)voxel_count * sizeof *voxels);
    memcpy(nph_gpu_buffer_map(dev, case_buf), cases, (u64)cases_n * sizeof *cases);

    NphGpuShader shader = test_load_shader(dev, "vis_voxel_test.comp");
    NphGpuComputePipelineDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.shader = shader;
    cd.name = "vis voxel test";
    NphGpuPipeline pipeline = NPH_GPU_PIPELINE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(dev, &cd, &pipeline), NPH_RESULT_OK);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "vis voxel", &cmd),
                        NPH_RESULT_OK);
    NphVisVoxelPush push;
    memset(&push, 0, sizeof push);
    push.cases = nph_gpu_buffer_address(dev, case_buf);
    push.results = nph_gpu_buffer_address(dev, result_buf);
    push.bricks = nph_gpu_buffer_address(dev, brick_buf);
    push.voxels = nph_gpu_buffer_address(dev, voxel_buf);
    push.count = cases_n;
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, (cases_n + 63U) / 64U, 1U, 1U);
    test_submit_wait(dev, cmd);
    const NphVisVoxelResult *results =
        (const NphVisVoxelResult *)nph_gpu_buffer_map(dev, result_buf);

    // The same rays through the C twin: the same hits, at the same t within
    // the rounding of a few operations; a ray that grazes a footprint's edge
    // within that rounding may hit on one side and miss on the other. The
    // resolve's search from each hit's depth meets that hit exactly.
    u32 hits = 0U;
    u32 found_wrong = 0U;
    u32 voxel_wrong = 0U;
    u32 face_wrong = 0U;
    u32 search_wrong = 0U;
    f64 worst = 0.0;
    for (u32 i = 0; i < cases_n; ++i) {
        const NphVisVoxelCase *c = &cases[i];
        const NphRenderVoxelBrick *brick = &bricks[c->brick];
        NphRenderVoxelHit hit;
        const bool got = nph_render_voxel_trace(
            brick, voxels, nph_vec3(c->origin.x, c->origin.y, c->origin.z),
            nph_vec3(c->dir.x, c->dir.y, c->dir.z), c->origin.w, c->dir.w, &hit);
        const bool gpu = (results[i].face & 256U) != 0U;
        if (gpu && (results[i].face & 512U) == 0U) {
            search_wrong += 1U;
        }
        if (got != gpu) {
            found_wrong += 1U;
            continue;
        }
        if (!got) {
            continue;
        }
        hits += 1U;
        const f64 side = 4.0 * (f64)brick->cell;
        const f64 speed = (f64)nph_vec3_length(nph_vec3(c->dir.x, c->dir.y, c->dir.z));
        worst = nph_f64_max(worst, nph_f64_abs((f64)results[i].t - (f64)hit.t) * speed / side);
        if (results[i].voxel != hit.voxel) {
            voxel_wrong += 1U;
        } else if ((results[i].face & 255U) != hit.face) {
            face_wrong += 1U;
        }
    }
    NTEST_LOG("%u rays, %u hits; %u disagree on a hit, %u on the voxel, %u on the face; worst t "
              "%.3g of a brick; the search missed %u",
              cases_n, hits, found_wrong, voxel_wrong, face_wrong, worst, search_wrong);
    NTEST_EXPECT_GT_UINT(hits, cases_n / 10U);
    NTEST_EXPECT_LE_UINT(found_wrong, cases_n / 2000U);
    NTEST_EXPECT_LE_UINT(voxel_wrong, cases_n / 2000U);
    NTEST_EXPECT_LE_UINT(face_wrong, cases_n / 2000U);
    NTEST_EXPECT_TRUE(worst < 1e-4);
    NTEST_EXPECT_EQ_UINT(search_wrong, 0U);

    nph_gpu_pipeline_destroy(dev, pipeline);
    nph_gpu_shader_destroy(dev, shader);
    nph_gpu_buffer_destroy(dev, brick_buf);
    nph_gpu_buffer_destroy(dev, voxel_buf);
    nph_gpu_buffer_destroy(dev, case_buf);
    nph_gpu_buffer_destroy(dev, result_buf);
    NPH_FREE_ARRAY(a, NphRenderVoxelBrick, bricks, TWIN_BRICKS);
    NPH_FREE_ARRAY(a, NphRenderVoxel, voxels, voxels_n);
    NPH_FREE_ARRAY(a, NphVisVoxelCase, cases, cases_n);
    test_gpu_destroy(dev);
}

// ---------------------------------------------------------------------------
// A foliage mesh in the renderer
// ---------------------------------------------------------------------------

static NphRenderMaterial grey_material(RenderFixture *f)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.7F, 0.7F, 0.7F, 1.0F);
    md.roughness = 0.7F;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

static NphRenderInstance instance(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = nph_render_transform_from_trs(nph_dvec3(0.0, 0.0, 0.0), nph_quat_identity(),
                                                 nph_vec3(1.0F, 1.0F, 1.0F));
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

// A production view from `distance` on +Z of the origin: no temporal
// jitter, no tonemapping, no environment, over black.
static NphRenderView plain_view(f64 distance, f32 lod_error)
{
    NphRenderView v;
    nph_render_view_default(&v, nph_render_camera_look_at(nph_dvec3(0.0, 0.0, distance),
                                                          nph_dvec3(0.0, 0.0, 0.0),
                                                          nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F));
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v.anti_aliasing = NPH_RENDER_AA_NONE;
    v.ev100 = 0.0F;
    v.tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3_zero();
    v.disable_ibl = 1;
    v.lod_error = lod_error;
    return v;
}

// A few frames, so that occlusion has a pyramid; the last one's statistics
// come two frames later.
static NphRenderFrameStats render_frames(RenderFixture *f, const NphRenderView *v)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < 4U; ++i) {
        stats = test_render_read(f, v);
    }
    return stats;
}

static bool covered(const u8 *p)
{
    return p[0] != 0U || p[1] != 0U || p[2] != 0U;
}

// The coverage and the light are measured on a bigger target over a grid of
// instances turned every way, whose pixels count a foliage's sub-pixel
// needles with less noise.
#define GRID 4U

static void grid_instances(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
                           NphRenderInstance *out)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 23U, 1U);
    for (u32 i = 0; i < GRID * GRID; ++i) {
        const NphVec3 axis = random_unit(&rng);
        const NphQuat q = nph_quat_from_axis_angle(axis, 2.0F * NPH_PI * nph_pcg32_next_f32(&rng));
        const u32 column = i % GRID;
        const u32 row = i / GRID;
        NphRenderInstanceDesc id;
        memset(&id, 0, sizeof id);
        id.mesh = mesh;
        id.materials = &mat;
        id.material_count = 1U;
        id.transform = nph_render_transform_from_trs(
            nph_dvec3(5.0 * ((f64)column - 1.5), 5.0 * ((f64)row - 1.5), 0.0), q,
            nph_vec3(1.0F, 1.0F, 1.0F));
        out[i] = NPH_RENDER_INSTANCE_NULL;
        NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &out[i]), NPH_RESULT_OK);
    }
}

static void grid_destroy(RenderFixture *f, NphRenderInstance *list)
{
    for (u32 i = 0; i < GRID * GRID; ++i) {
        nph_render_instance_destroy(f->renderer, list[i]);
    }
}

#define BIG_SIZE   256U
#define BIG_PIXELS ((u64)BIG_SIZE * BIG_SIZE)

typedef struct BigTarget {
    NphGpuImage image;
    u8 *pixels;
} BigTarget;

static void big_target_init(RenderFixture *f, BigTarget *t)
{
    t->image = test_make_target(f->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, BIG_SIZE);
    t->pixels = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, BIG_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(t->pixels);
}

static void big_target_free(RenderFixture *f, BigTarget *t)
{
    nph_gpu_image_destroy(f->dev, t->image);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, t->pixels, BIG_PIXELS * 4U);
}

static NphRenderFrameStats render_big(RenderFixture *f, BigTarget *t, const NphRenderView *v)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < 4U; ++i) {
        stats = test_render_frame(f, v, t->image);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, t->image, 0U, t->pixels, BIG_PIXELS * 4U);
    return stats;
}

static u32 coverage(const u8 *pixels, u64 n)
{
    u32 c = 0U;
    for (u64 i = 0; i < n; ++i) {
        c += covered(pixels + i * 4U) ? 1U : 0U;
    }
    return c;
}

static u32 expected_byte(f32 c)
{
    const f32 e = nph_render_ref_linear_to_srgb(nph_f32_saturate(c));
    return (u32)(e * 255.0F + 0.5F);
}

// The debug views' colour of a key (vis_resolve.glsl vis_debug_color).
static void debug_color(u32 key, u32 out[3])
{
    u32 h = (key ^ 61U) ^ (key >> 16U);
    h *= 9U;
    h ^= h >> 4U;
    h *= 0x27d4eb2dU;
    h ^= h >> 15U;
    out[0] = expected_byte(0.2F + 0.8F * (f32)(h & 0xFFU) / 255.0F);
    out[1] = expected_byte(0.2F + 0.8F * (f32)((h >> 8U) & 0xFFU) / 255.0F);
    out[2] = expected_byte(0.2F + 0.8F * (f32)((h >> 16U) & 0xFFU) / 255.0F);
}

// A triangle against a ray, either side (Möller & Trumbore 1997), double
// precision: its t, or false.
static bool ray_triangle(const f64 o[3], const f64 d[3], NphVec3 a, NphVec3 b, NphVec3 c, f64 *t)
{
    const f64 e1[3] = {(f64)b.x - (f64)a.x, (f64)b.y - (f64)a.y, (f64)b.z - (f64)a.z};
    const f64 e2[3] = {(f64)c.x - (f64)a.x, (f64)c.y - (f64)a.y, (f64)c.z - (f64)a.z};
    const f64 p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                      d[0] * e2[1] - d[1] * e2[0]};
    const f64 det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (nph_f64_abs(det) < 1e-300) {
        return false;
    }
    const f64 inv = 1.0 / det;
    const f64 s[3] = {o[0] - (f64)a.x, o[1] - (f64)a.y, o[2] - (f64)a.z};
    const f64 u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    const f64 q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                      s[0] * e1[1] - s[1] * e1[0]};
    const f64 w = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (w < 0.0 || u + w > 1.0) {
        return false;
    }
    *t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return true;
}

// The visible clusters the last frame drew.
static NphVisCluster *visible_clusters(RenderFixture *f, u32 *out_count)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const RenderVis *v = &f->renderer->vis;
    NphVisCounters c;
    test_read_buffer(f->dev, v->counters, 0U, sizeof c, &c);
    NTEST_ASSERT_EQ_UINT(c.overflow, 0U);
    NTEST_ASSERT_LE_UINT(c.clusters, f->renderer->desc.max_visible_clusters);
    *out_count = c.clusters;
    NphVisCluster *list =
        NPH_ALLOC_ARRAY(nph_allocator_system(), NphVisCluster, nph_max_u32(c.clusters, 1U));
    NTEST_ASSERT_NOT_NULL(list);
    if (c.clusters != 0U) {
        test_read_buffer(f->dev, v->clusters, 0U, (u64)c.clusters * sizeof *list, list);
    }
    return list;
}

// The positions the GPU sees (vis_position).
static NphVec3 *dequantised(const LodMesh *m)
{
    NphVec3 *p = NPH_ALLOC_ARRAY(nph_allocator_system(), NphVec3, m->data.vertex_count);
    NTEST_ASSERT_NOT_NULL(p);
    const f32 q = 1.0F / 65535.0F;
    for (u32 i = 0; i < m->data.vertex_count; ++i) {
        const NphRenderVertexPosition s = m->data.positions[i];
        p[i] = nph_vec3(m->data.position_offset.x + (f32)s.x * q * m->data.position_scale.x,
                        m->data.position_offset.y + (f32)s.y * q * m->data.position_scale.y,
                        m->data.position_offset.z + (f32)s.z * q * m->data.position_scale.z);
    }
    return p;
}

// The visibility buffer's value the CPU expects at a pixel: the nearest of
// the visible clusters' primitives along the pixel's ray from the camera
// at `eye` (mesh space, the instance at the origin) in direction `d` (to the
// pixel's point on the near plane: a hit at t lies at depth 1 / t), as the
// atomic max of (depth bits, cluster << 7 | primitive) keeps it; 0 for none.
static u64 expected_texel(const LodMesh *m, const NphVec3 *positions, const NphVisCluster *list,
                          u32 count, NphVec3 eye, NphVec3 d)
{
    const NphRenderMeshlets *c = &m->lods.clusters;
    const f64 o[3] = {(f64)eye.x, (f64)eye.y, (f64)eye.z};
    const f64 dd[3] = {(f64)d.x, (f64)d.y, (f64)d.z};
    u64 best = 0U;
    for (u32 g = 0; g < count; ++g) {
        const NphRenderMeshlet *ml = &c->meshlets.data[list[g].meshlet];
        if (ml->vertex_count == 0U) {
            for (u32 k = 0; k < ml->triangle_count; ++k) {
                NphRenderVoxelHit hit;
                if (nph_render_voxel_trace(&m->lods.bricks.data[ml->vertex_offset + k],
                                           m->lods.voxels.data, eye, d, 1.0F, 3.402823466e38F,
                                           &hit)) {
                    const u64 v = ((u64)nph_f32_to_bits(1.0F / hit.t) << 32U) | ((u64)g << 7U) | k;
                    best = v > best ? v : best;
                }
            }
            continue;
        }
        for (u32 t = 0; t < ml->triangle_count; ++t) {
            const u8 *corner = &c->triangles.data[((u64)ml->triangle_offset + t) * 3U];
            const u32 *vx = &c->vertices.data[ml->vertex_offset];
            f64 th = 0.0;
            if (ray_triangle(o, dd, positions[vx[corner[0]]], positions[vx[corner[1]]],
                             positions[vx[corner[2]]], &th) &&
                th >= 1.0) {
                const u64 v = ((u64)nph_f32_to_bits((f32)(1.0 / th)) << 32U) | ((u64)g << 7U) | t;
                best = v > best ? v : best;
            }
        }
    }
    return best;
}

// The rays of the view's pixels, as the shaders make them (vis_voxel.glsl
// vis_voxel_ray): through the inverse of the camera-relative view-projection
// the frame takes (no jitter: anti-aliasing is off).
static void pixel_rays(const NphRenderView *v, NphVec3 *out)
{
    const NphMat3 rot = nph_mat3_from_quat(nph_quat_normalize_or_identity(v->camera.orientation));
    const NphMat4 view_m = nph_mat4_from_mat3(nph_mat3_transpose(rot));
    const NphMat4 proj = nph_mat4_perspective_infinite(v->camera.yfov, 1.0F, v->camera.znear);
    NphMat4 inv = nph_mat4_identity();
    NTEST_ASSERT_TRUE(nph_mat4_inverse(nph_mat4_mul(proj, view_m), &inv));
    for (u32 y = 0; y < TEST_TARGET_SIZE; ++y) {
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            const f32 qx = ((f32)x + 0.5F) * (1.0F / (f32)TEST_TARGET_SIZE) * 2.0F - 1.0F;
            const f32 qy = ((f32)y + 0.5F) * (1.0F / (f32)TEST_TARGET_SIZE) * 2.0F - 1.0F;
            const NphVec4 p = nph_mat4_mul_vec4(inv, nph_vec4(qx, qy, 1.0F, 1.0F));
            out[y * TEST_TARGET_SIZE + x] = nph_vec3_scale(nph_vec3(p.x, p.y, p.z), 1.0F / p.w);
        }
    }
}

NTEST_F(VoxelGpu, voxel_clusters_draw_where_their_rays_meet_them)
{
    REQUIRE_PRODUCTION(f);
    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 7U);
    NTEST_ASSERT_GT_UINT(m.lods.bricks.len, 0U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderMaterial mat = grey_material(f);
    const NphRenderInstance inst = instance(f, mesh, mat);
    NphVec3 *positions = dequantised(&m);
    NphVec3 *rays = NPH_ALLOC_ARRAY(nph_allocator_system(), NphVec3, PIXELS);
    NTEST_ASSERT_NOT_NULL(rays);

    // Near, where triangles and voxels mix in the cut, and far, where the
    // voxels are coarser.
    static const f64 k_distances[3] = {7.0, 12.0, 24.0};
    u32 voxel_pixels = 0U;
    for (u32 di = 0; di < 3U; ++di) {
        NphRenderView v = plain_view(k_distances[di], 1.0F);
        v.debug_view = NPH_RENDER_VIEW_TRIANGLES;
        const NphRenderFrameStats stats = render_frames(f, &v);
        u32 count = 0U;
        NphVisCluster *list = visible_clusters(f, &count);
        pixel_rays(&v, rays);
        const NphVec3 eye = nph_vec3(0.0F, 0.0F, (f32)k_distances[di]);
        u32 wrong = 0U;
        u32 hits = 0U;
        u32 voxels_seen = 0U;
        for (u32 i = 0; i < PIXELS; ++i) {
            const u64 texel = expected_texel(&m, positions, list, count, eye, rays[i]);
            const u8 *p = f->pixels + (u64)i * 4U;
            u32 want[3] = {0U, 0U, 0U};
            if (texel != 0U) {
                hits += 1U;
                const u32 g = (u32)(texel & 0xFFFFFFFFU) >> 7U;
                const NphRenderMeshlet *ml = &m.lods.clusters.meshlets.data[list[g].meshlet];
                voxels_seen += ml->vertex_count == 0U ? 1U : 0U;
                debug_color((u32)(texel & 0xFFFFFFFFU), want);
            }
            // The HDR target's rounding (B10G11R11's blue keeps five bits of
            // mantissa), which debug views pass through: two keys' colours
            // lie that close on all three channels about once in 2^15.
            bool same = true;
            for (u32 ch = 0; ch < 3U; ++ch) {
                const u32 d = p[ch] > want[ch] ? p[ch] - want[ch] : want[ch] - p[ch];
                same = same && d <= 4U;
            }
            wrong += same ? 0U : 1U;
        }
        NTEST_LOG("at %.0f m: %u clusters (%u voxel clusters, %u bricks); %u pixels covered, %u "
                  "by voxels; %u differ from the CPU's",
                  k_distances[di], count, stats.vis_voxel_clusters, stats.vis_voxel_bricks, hits,
                  voxels_seen, wrong);
        // Rays that graze a footprint or a triangle's edge within the
        // rounding may land on either side.
        NTEST_EXPECT_LE_UINT(wrong, hits / 50U + 2U);
        voxel_pixels += voxels_seen;
        NPH_FREE_ARRAY(nph_allocator_system(), NphVisCluster, list, nph_max_u32(count, 1U));
    }
    NTEST_EXPECT_GT_UINT(voxel_pixels, 200U);

    NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, rays, PIXELS);
    NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, positions, m.data.vertex_count);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
}

NTEST_F(VoxelGpu, the_voxel_levels_keep_the_coverage)
{
    REQUIRE_PRODUCTION(f);
    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 11U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderMaterial mat = grey_material(f);
    NphRenderInstance grid[GRID * GRID];
    grid_instances(f, mesh, mat, grid);
    BigTarget t;
    big_target_init(f, &t);
    static const f64 k_distances[4] = {24.0, 40.0, 64.0, 120.0};
    for (u32 di = 0; di < 4U; ++di) {
        NphRenderView v = plain_view(k_distances[di], 0.0F);
        v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
        render_big(f, &t, &v);
        const u32 a = coverage(t.pixels, BIG_PIXELS);
        v.lod_error = 1.0F;
        const NphRenderFrameStats stats = render_big(f, &t, &v);
        const u32 b = coverage(t.pixels, BIG_PIXELS);
        NTEST_LOG("at %.0f m: level 0 covers %u pixels, the cut %u (%+.1f %%; %u voxel clusters, "
                  "%u bricks)",
                  k_distances[di], a, b, 100.0 * ((f64)b / (f64)nph_max_u32(a, 1U) - 1.0),
                  stats.vis_voxel_clusters, stats.vis_voxel_bricks);
        NTEST_EXPECT_GT_UINT(a, 800U);
        NTEST_EXPECT_GT_UINT(stats.vis_voxel_clusters, 0U);
        // The footprints match the needles' coverage along the voxels' axes
        // (render/voxel.h); seen obliquely, as the turned instances are, the
        // first voxel levels of six-sided needles cover 7 % more and the
        // coarsest 5 % less (measured, 2026-10-27; ADR-0061 "Accuracy").
        NTEST_EXPECT_TRUE(nph_f64_abs((f64)b - (f64)a) <= 0.08 * (f64)a);
    }
    big_target_free(f, &t);
    grid_destroy(f, grid);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
}

NTEST_F(VoxelGpu, the_voxel_resolve_shows_the_voxels_look)
{
    REQUIRE_PRODUCTION(f);
    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 13U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderMaterial mat = grey_material(f);
    const NphRenderInstance inst = instance(f, mesh, mat);
    // Far enough that the cut is voxels alone: every covered pixel shows the
    // voxelizer's albedo, not the instance's grey.
    NphRenderView v = plain_view(30.0, 1.0F);
    v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
    const NphRenderFrameStats stats = render_frames(f, &v);
    NTEST_EXPECT_GT_UINT(stats.vis_voxel_clusters, 0U);
    const NphVec3 albedo = LOD_VOXEL_ALBEDO;
    const u32 want[3] = {expected_byte(albedo.x), expected_byte(albedo.y), expected_byte(albedo.z)};
    const u32 grey = expected_byte(0.7F);
    u32 covered_n = 0U;
    u32 as_voxels = 0U;
    u32 as_triangles = 0U;
    for (u64 i = 0; i < PIXELS; ++i) {
        const u8 *p = f->pixels + i * 4U;
        if (!covered(p)) {
            continue;
        }
        covered_n += 1U;
        // sRGB8's rounding, twice (the voxel's albedo, then the target).
        bool voxel = true;
        for (u32 ch = 0; ch < 3U; ++ch) {
            const u32 d = p[ch] > want[ch] ? p[ch] - want[ch] : want[ch] - p[ch];
            voxel = voxel && d <= 2U;
        }
        as_voxels += voxel ? 1U : 0U;
        as_triangles += p[0] == grey && p[1] == grey && p[2] == grey ? 1U : 0U;
    }
    NTEST_LOG("%u pixels covered: %u with the voxels' albedo, %u with the triangles' grey "
              "(%u voxel clusters)",
              covered_n, as_voxels, as_triangles, stats.vis_voxel_clusters);
    NTEST_EXPECT_GT_UINT(covered_n, 50U);
    NTEST_EXPECT_EQ_UINT(as_voxels + as_triangles, covered_n);
    NTEST_EXPECT_GT_UINT(as_voxels, covered_n / 2U);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
}

// Whether the frame the render graph's timestamps last came back for had
// the pass `name`.
static bool frame_has_pass(RenderFixture *f, const char *name)
{
    NphGpuRgPassTiming timings[256];
    const u32 n = nph_gpu_rg_timings(f->graph, timings, 256U, NULL);
    NTEST_EXPECT_GT_UINT(n, 0U);
    for (u32 i = 0; i < n; ++i) {
        if (strcmp(timings[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

// A frame costs the voxels nothing until the renderer holds a mesh with
// voxel levels: one with levels but no voxels adds none of the voxel passes
// (and its resolve is compiled without the voxel test), one with voxels adds
// them, and they leave with it.
NTEST_F(VoxelGpu, the_voxel_passes_come_and_go_with_voxel_meshes)
{
    REQUIRE_PRODUCTION(f);
    static const char *const k_passes[3] = {"vis voxel raster 0", "vis voxel raster 1",
                                            "vis voxel resolve"};
    const NphRenderMaterial mat = grey_material(f);
    LodMesh plain;
    lod_foliage_tubes_with(&plain, 300U, 4.0F, 7U, 6U, false);
    NphRenderMesh plain_mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &plain.desc, &plain_mesh),
                        NPH_RESULT_OK);
    const NphRenderInstance plain_inst = instance(f, plain_mesh, mat);
    const NphRenderView v = plain_view(30.0, 1.0F);
    NphRenderFrameStats stats = render_frames(f, &v);
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_EXPECT_FALSE(frame_has_pass(f, k_passes[i]));
    }
    NTEST_EXPECT_GT_UINT(stats.vis_clusters, 0U);
    NTEST_EXPECT_EQ_UINT(stats.vis_voxel_clusters, 0U);

    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 13U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderInstance inst = instance(f, mesh, mat);
    stats = render_frames(f, &v);
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_EXPECT_TRUE(frame_has_pass(f, k_passes[i]));
    }
    NTEST_EXPECT_GT_UINT(stats.vis_voxel_clusters, 0U);

    nph_render_instance_destroy(f->renderer, inst);
    nph_render_mesh_destroy(f->renderer, mesh);
    stats = render_frames(f, &v);
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_EXPECT_FALSE(frame_has_pass(f, k_passes[i]));
    }
    NTEST_EXPECT_GT_UINT(stats.vis_clusters, 0U);
    NTEST_EXPECT_EQ_UINT(stats.vis_voxel_clusters, 0U);
    nph_render_instance_destroy(f->renderer, plain_inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, plain_mesh);
    lod_mesh_free(&m);
    lod_mesh_free(&plain);
}

// A mesh's voxel clusters are checked as the raster splats them, a
// cluster's voxels one run: its bricks of one cell size, each brick's voxels
// following the brick before's. And the GPU's reads stay in the streams: a
// brick's voxels past them, an empty mask, a cell that is no power of two.
NTEST_F(VoxelGpu, malformed_voxel_clusters_are_refused)
{
    REQUIRE_RENDERER(f);
    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 13U);
    // A voxel cluster of two bricks or more.
    const NphRenderMeshlet *cl = m.lods.clusters.meshlets.data;
    u32 pick = NPH_U32_MAX;
    for (u32 i = m.lods.level0_clusters; i < m.lods.clusters.meshlets.len; ++i) {
        if (cl[i].vertex_count == 0U && cl[i].triangle_count >= 2U) {
            pick = i;
            break;
        }
    }
    NTEST_ASSERT_NE_UINT(pick, NPH_U32_MAX);
    NphAllocator *a = nph_allocator_system();
    const u32 n = m.lods.bricks.len;
    NphRenderVoxelBrick *bricks = NPH_ALLOC_ARRAY(a, NphRenderVoxelBrick, n);
    NTEST_ASSERT_NOT_NULL(bricks);
    NphRenderMeshDesc d = m.desc;
    d.voxel_bricks = bricks;
    const NphRenderVoxelBrick *first = &bricks[cl[pick].vertex_offset];
    NphRenderVoxelBrick *second = &bricks[cl[pick].vertex_offset + 1U];
#define RESET() memcpy(bricks, m.lods.bricks.data, sizeof *bricks *n)
#define EXPECT_REFUSED()                                                                           \
    do {                                                                                           \
        NphRenderMesh none = NPH_RENDER_MESH_NULL;                                                 \
        NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &d, &none),                        \
                            NPH_RESULT_INVALID_ARGUMENT);                                          \
        RESET();                                                                                   \
    } while (0)
    RESET();
    NphRenderMesh ok = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &d, &ok), NPH_RESULT_OK);
    nph_render_mesh_destroy(f->renderer, ok);
    // Another cell size, a power of two still.
    second->cell *= 2.0F;
    EXPECT_REFUSED();
    // Voxels that do not follow the first brick's (its own, in range).
    second->first_voxel = first->first_voxel;
    EXPECT_REFUSED();
    second->first_voxel = d.voxel_count;
    EXPECT_REFUSED();
    second->mask[0] = 0U;
    second->mask[1] = 0U;
    EXPECT_REFUSED();
    second->cell = 0.3F;
    EXPECT_REFUSED();
#undef EXPECT_REFUSED
#undef RESET
    NPH_FREE_ARRAY(a, NphRenderVoxelBrick, bricks, n);
    lod_mesh_free(&m);
}

// The image's linear luminance, summed: the light it reflects to the camera.
static f64 total_luminance(const u8 *pixels, u64 n)
{
    f64 sum = 0.0;
    for (u64 i = 0; i < n; ++i) {
        const u8 *p = pixels + i * 4U;
        const f64 r = (f64)nph_render_ref_srgb_to_linear((f32)p[0] / 255.0F);
        const f64 g = (f64)nph_render_ref_srgb_to_linear((f32)p[1] / 255.0F);
        const f64 b = (f64)nph_render_ref_srgb_to_linear((f32)p[2] / 255.0F);
        sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
    }
    return sum;
}

NTEST_F(VoxelGpu, voxels_reflect_the_light_their_triangles_do)
{
    REQUIRE_PRODUCTION(f);
    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 17U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    // The instance's material matches the voxelizer's look, so that the two
    // differ by their shading alone.
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    const NphVec3 albedo = LOD_VOXEL_ALBEDO;
    md.base_color = nph_vec4(albedo.x, albedo.y, albedo.z, 1.0F);
    md.roughness = 0.7F;
    NphRenderMaterial mat = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &mat), NPH_RESULT_OK);
    NphRenderInstance grid[GRID * GRID];
    grid_instances(f, mesh, mat, grid);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 1.0F, 1.0F);
    ld.intensity = 3.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(-0.4F, -0.7F, -0.6F));
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    BigTarget t;
    big_target_init(f, &t);
    static const f64 k_distances[3] = {24.0, 40.0, 64.0};
    for (u32 di = 0; di < 3U; ++di) {
        // The light reflected, and the pixels covered (the base colour's
        // view: a lit needle facing away from the light is black).
        NphRenderView v = plain_view(k_distances[di], 0.0F);
        render_big(f, &t, &v);
        const f64 a = total_luminance(t.pixels, BIG_PIXELS);
        v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
        render_big(f, &t, &v);
        const u32 ca = coverage(t.pixels, BIG_PIXELS);
        v.lod_error = 1.0F;
        render_big(f, &t, &v);
        const u32 cb = coverage(t.pixels, BIG_PIXELS);
        v.debug_view = NPH_RENDER_VIEW_LIT;
        const NphRenderFrameStats stats = render_big(f, &t, &v);
        const f64 b = total_luminance(t.pixels, BIG_PIXELS);
        // A covered pixel's mean light, so that the levels' coverage, which
        // the coverage test holds to its own bound, does not count twice.
        const f64 ma = a / (f64)nph_max_u32(ca, 1U);
        const f64 mb = b / (f64)nph_max_u32(cb, 1U);
        NTEST_LOG("at %.0f m: level 0 %.4f a pixel over %u, the cut %.4f over %u (%u voxel "
                  "clusters): %+.1f %% a pixel, %+.1f %% in all",
                  k_distances[di], ma, ca, mb, cb, stats.vis_voxel_clusters,
                  100.0 * (mb / nph_f64_max(ma, 1e-9) - 1.0),
                  100.0 * (b / nph_f64_max(a, 1e-9) - 1.0));
        NTEST_EXPECT_GT_UINT(stats.vis_voxel_clusters, 0U);
        // The visible lobe takes the viewer's cosine for the normals'
        // visibility, which a needle's few facets, smooth-shaded, spread
        // flatter: the first voxel levels reflect 10 % more a pixel seen
        // obliquely (measured, 2026-10-27; ADR-0061 "Accuracy").
        NTEST_EXPECT_TRUE(ma > 0.0 && nph_f64_abs(mb / ma - 1.0) <= 0.12);
    }
    big_target_free(f, &t);
    nph_render_light_destroy(f->renderer, light);
    grid_destroy(f, grid);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
}

// ---------------------------------------------------------------------------
// The voxel resolve's shading against its C twin
// ---------------------------------------------------------------------------

// The light a voxel reflects toward the camera from a directional light of
// illuminance `e` from unit direction `l`, with no environment or occlusion
// (vis_voxel_resolve.glsl, surface.glsl nph_surface_shade_voxel), before
// exposure: the visible lobe's diffuse response (render/voxel.h's twin) and
// GGX of the lobe's spread. The split-sum's energy term is taken as one
// (a few tenths of a per cent of the specular at these roughnesses).
static NphVec3 voxel_radiance(const NphRenderVoxelDesc *d, NphVec3 axis, NphVec3 v, NphVec3 l,
                              f32 e)
{
    const NphVec3 n = nph_vec3_dot(axis, v) < 0.0F ? nph_vec3_neg(axis) : axis;
    const f32 r0 = nph_f32_clamp(d->roughness, 0.045F, 1.0F);
    const f32 a2 = d->kappa > 0.0F ? nph_f32_min(r0 * r0 * r0 * r0 + 2.0F / d->kappa, 1.0F) : 1.0F;
    const f32 nov = nph_f32_max(nph_vec3_dot(n, v), 1e-4F);
    const f32 nov_raw = nph_vec3_dot(n, v);
    const f32 metallic = nph_f32_saturate(d->metallic);
    const NphVec3 f0 = nph_vec3_lerp(nph_vec3_splat(0.04F), d->albedo, metallic);
    const NphVec3 diffuse = nph_vec3_scale(d->albedo, (1.0F - metallic) / NPH_PI);
    const f32 response = nph_f32_max(nph_render_voxel_diffuse(n, d->kappa, v, l), 0.0F);
    const f32 nol = nph_vec3_dot(n, l);
    NphVec3 out = nph_vec3_scale(diffuse, response);
    if (nol > 0.0F) {
        const f32 vol = nph_vec3_dot(v, l);
        const f32 inv_h = 1.0F / nph_f32_sqrt(nph_f32_max(2.0F + 2.0F * vol, 1e-12F));
        const f32 noh = nph_f32_clamp((nov_raw + nol) * inv_h, 0.0F, 1.0F);
        const f32 voh = nph_f32_clamp((1.0F + vol) * inv_h, 0.0F, 1.0F);
        const f32 dd = (noh * a2 - noh) * noh + 1.0F;
        const f32 smith_v = nph_f32_sqrt(nov * nov * (1.0F - a2) + a2);
        const f32 gv = nol * smith_v;
        const f32 gl = nov * nph_f32_sqrt(nol * nol * (1.0F - a2) + a2);
        const f32 dv = 0.5F * a2 / (NPH_PI * dd * dd * nph_f32_max(gv + gl, 1e-8F));
        const f32 x = 1.0F - voh;
        const f32 x5 = x * x * x * x * x;
        const NphVec3 fresnel =
            nph_vec3_add(f0, nph_vec3_scale(nph_vec3_sub(nph_vec3_one(), f0), x5));
        out = nph_vec3_add(out, nph_vec3_scale(fresnel, dv * nol));
    }
    return nph_vec3_scale(out, e);
}

NTEST_F(VoxelGpu, the_voxel_resolve_shades_as_its_cpu_twin)
{
    REQUIRE_PRODUCTION(f);
    LodMesh m;
    lod_foliage(&m, 1500U, 4.0F, 19U);
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &m.desc, &mesh), NPH_RESULT_OK);
    const NphRenderMaterial mat = grey_material(f);
    const NphRenderInstance inst = instance(f, mesh, mat);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3(1.0F, 1.0F, 1.0F);
    ld.intensity = 3.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(-0.4F, -0.7F, -0.6F));
    NphRenderLight light = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, &ld, &light), NPH_RESULT_OK);
    const NphVec3 l = nph_vec3_neg(ld.direction);
    NphVec3 *positions = dequantised(&m);
    NphVec3 *rays = NPH_ALLOC_ARRAY(nph_allocator_system(), NphVec3, PIXELS);
    NTEST_ASSERT_NOT_NULL(rays);
    static const f64 k_distances[2] = {12.0, 24.0};
    u32 compared = 0U;
    for (u32 di = 0; di < 2U; ++di) {
        NphRenderView v = plain_view(k_distances[di], 1.0F);
        v.debug_view = NPH_RENDER_VIEW_TRIANGLES;
        render_frames(f, &v);
        u32 count = 0U;
        NphVisCluster *list = visible_clusters(f, &count);
        v.debug_view = NPH_RENDER_VIEW_LIT;
        render_frames(f, &v);
        pixel_rays(&v, rays);
        const NphVec3 eye = nph_vec3(0.0F, 0.0F, (f32)k_distances[di]);
        const f32 exposure = 1.0F / 1.2F;
        f64 sum_gpu = 0.0;
        f64 sum_cpu = 0.0;
        f64 worst = 0.0;
        u32 n = 0U;
        for (u32 i = 0; i < PIXELS; ++i) {
            const u64 texel = expected_texel(&m, positions, list, count, eye, rays[i]);
            if (texel == 0U) {
                continue;
            }
            const u32 g = (u32)(texel & 0xFFFFFFFFU) >> 7U;
            const u32 k = (u32)texel & 127U;
            const NphRenderMeshlet *ml = &m.lods.clusters.meshlets.data[list[g].meshlet];
            NphRenderVoxelHit h;
            if (ml->vertex_count != 0U ||
                !nph_render_voxel_trace(&m.lods.bricks.data[ml->vertex_offset + k],
                                        m.lods.voxels.data, eye, rays[i], 1.0F, 3.4e38F, &h)) {
                continue;
            }
            NphRenderVoxelDesc vd;
            nph_render_voxel_unpack(m.lods.voxels.data[h.voxel], &vd);
            const NphVec3 view = nph_vec3_normalize(nph_vec3_neg(rays[i]));
            const NphVec3 c =
                nph_vec3_scale(voxel_radiance(&vd, vd.axis, view, l, ld.intensity), exposure);
            const u8 *p = f->pixels + (u64)i * 4U;
            const f64 cpu = 0.2126 * (f64)nph_f32_saturate(c.x) +
                            0.7152 * (f64)nph_f32_saturate(c.y) +
                            0.0722 * (f64)nph_f32_saturate(c.z);
            const f64 gpu = 0.2126 * (f64)nph_render_ref_srgb_to_linear((f32)p[0] / 255.0F) +
                            0.7152 * (f64)nph_render_ref_srgb_to_linear((f32)p[1] / 255.0F) +
                            0.0722 * (f64)nph_render_ref_srgb_to_linear((f32)p[2] / 255.0F);
            sum_gpu += gpu;
            sum_cpu += cpu;
            worst = nph_f64_max(worst, nph_f64_abs(gpu - cpu) / nph_f64_max(cpu, 0.02));
            n += 1U;
        }
        NTEST_LOG("at %.0f m: %u voxel pixels, GPU %.4f against the CPU's %.4f (%+.2f %%), worst "
                  "pixel %.1f %%",
                  k_distances[di], n, sum_gpu / nph_f64_max((f64)n, 1.0),
                  sum_cpu / nph_f64_max((f64)n, 1.0),
                  100.0 * (sum_gpu / nph_f64_max(sum_cpu, 1e-9) - 1.0), 100.0 * worst);
        // The HDR target's rounding (dithered) and the 8-bit target's.
        NTEST_EXPECT_TRUE(n > 50U && nph_f64_abs(sum_gpu / sum_cpu - 1.0) <= 0.01);
        NTEST_EXPECT_TRUE(worst <= 0.08);
        compared += n;
        NPH_FREE_ARRAY(nph_allocator_system(), NphVisCluster, list, nph_max_u32(count, 1U));
    }
    NTEST_EXPECT_GT_UINT(compared, 100U);
    NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, rays, PIXELS);
    NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, positions, m.data.vertex_count);
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, mat);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
}
