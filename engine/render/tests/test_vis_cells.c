// SPDX-License-Identifier: Apache-2.0
// The world partition's cells in the renderer (render/renderer.h
// nph_render_cell_create; docs/render.md "Cell proxies"; ADR-0062): a mesh of
// voxels alone (a cell's proxy) made, drawn by the production path and
// passed by the reference renderer and the rays, and refused malformed; a
// cell made and refused; its proxy drawn in place of its instances from
// afar, standing for them, and not in views without levels of detail; the
// sun's shadow map drawing the proxy or the instances per level; and the
// proxy's levels apart (ADR-0063), each a mesh, drawing it as the whole; and
// a cell's instances kept as runs of slots for the cell culls (ADR-0064). The
// fixture's teardown asserts zero validation and synchronisation-validation
// errors and nothing leaked.
#include <nepheline/jobs/jobs.h>
#include <nepheline/math/random.h>
#include <nepheline/render/cell_proxy.h>
#include <nepheline/render/cluster_lod.h>

#include "render_internal.h"
#include "test_levels_apart.h"
#include "test_lod_support.h"
#include "test_shadow_support.h"

#include <stdlib.h>
#include <string.h>

typedef RenderFixture Cells;

NTEST_SETUP(Cells)
{
    // Room for a grove's clusters.
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_visible_clusters = 1U << 16U;
    d.max_cull_meshlets = 1U << 18U;
    d.max_deferred = 1U << 16U;
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(Cells)
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

// ---------------------------------------------------------------------------
// A grove and its proxy
// ---------------------------------------------------------------------------

#define GROVE_TREES 4U
// The proxy's finest cell (its level 0's): the trees' needles are 0.02 m
// thick, in clusters of a 2 m cube.
#define GROVE_CELL 0.125F

// Four trees of needles, a mesh of triangles alone (levels without voxels),
// at the corners of a 3 m square on y = 0, and their cell's proxy built from
// them (render/cell_proxy.h), as the cooker builds it, in world space.
typedef struct Grove {
    LodMesh tree;
    NphVec3 *positions; // the tree's dequantised positions, which the levels were built over
    LodVoxelLook look;
    NphRenderVoxelSampler sampler;
    NphRenderProxyInstance placed[GROVE_TREES];
    NphRenderClusterLods proxy;
    NphRenderMeshletArray proxy_meshlets; // its records, naming their voxels compact
    NphRenderPackedVoxels proxy_packed;   // its voxels compact, as the cooker stores them
    NphRenderSubmesh proxy_sub;
    NphRenderMeshDesc proxy_desc; // a mesh of voxels alone
} Grove;

// A proxy's record as a cooked one brings it (asset/mesh.h): no vertices,
// one submesh naming material slot 0, every cluster a voxel cluster, the
// bounds its clusters' spheres', its voxels compact (`packed`, from `l`).
static void proxy_desc_of(const NphRenderClusterLods *l, const NphRenderPackedVoxels *packed,
                          const NphRenderSubmesh *sub, NphRenderMeshDesc *pd)
{
    memset(pd, 0, sizeof *pd);
    pd->submeshes = sub;
    pd->submesh_count = 1U;
    pd->position_scale = nph_vec3_one();
    const NphRenderMeshlets *c = &l->clusters;
    NphAABB bounds = nph_aabb_empty();
    for (u32 i = 0; i < c->bounds.len; ++i) {
        const NphRenderMeshletBounds *b = &c->bounds.data[i];
        bounds =
            nph_aabb_union(bounds, nph_aabb(nph_vec3_sub(b->center, nph_vec3_splat(b->radius)),
                                            nph_vec3_add(b->center, nph_vec3_splat(b->radius))));
    }
    pd->bounds = bounds;
    pd->meshlets = c->meshlets.data;
    pd->meshlet_bounds = c->bounds.data;
    pd->meshlet_count = c->meshlets.len;
    pd->cluster_lods = l->lods.data;
    pd->lod_groups = l->groups.data;
    pd->lod_group_count = l->groups.len;
    pd->lod_group_clusters = l->group_clusters.data;
    pd->lod_group_cluster_count = l->group_clusters.len;
    pd->lod_nodes = l->nodes.data;
    pd->lod_node_count = l->nodes.len;
    pd->lod_level0_meshlets = l->level0_clusters;
    pd->voxel_clusters = packed->clusters.data;
    pd->voxel_cluster_count = packed->clusters.len;
    pd->voxel_bricks = packed->bricks.data;
    pd->voxel_brick_count = packed->bricks.len;
    pd->voxel_words = packed->words.data;
    pd->voxel_word_count = packed->words.len;
}

static NphDVec3 grove_place(u32 i)
{
    return nph_dvec3((i & 1U) != 0U ? 1.5 : -1.5, 0.0, (i & 2U) != 0U ? 1.5 : -1.5);
}

static void grove_build(Grove *g)
{
    NphAllocator *a = nph_allocator_system();
    memset(g, 0, sizeof *g);
    lod_foliage_tubes_with(&g->tree, 200U, 2.0F, 31U, 6U, false);
    const NphRenderMeshData *d = &g->tree.data;
    g->positions = NPH_ALLOC_ARRAY(a, NphVec3, d->vertex_count);
    NTEST_ASSERT_NOT_NULL(g->positions);
    const f32 q = 1.0F / 65535.0F;
    for (u32 i = 0; i < d->vertex_count; ++i) {
        const NphRenderVertexPosition p = d->positions[i];
        g->positions[i] = nph_vec3(d->position_offset.x + (f32)p.x * q * d->position_scale.x,
                                   d->position_offset.y + (f32)p.y * q * d->position_scale.y,
                                   d->position_offset.z + (f32)p.z * q * d->position_scale.z);
    }
    g->look.attribs = d->attribs;
    g->look.surface[0] = (u8)NPH_RENDER_VOXEL_SURFACE_OPAQUE;
    g->sampler.user = &g->look;
    g->sampler.submesh_surface = g->look.surface;
    g->sampler.sample = lod_look_sample;
    g->sampler.covers = lod_look_covers;
    for (u32 i = 0; i < GROVE_TREES; ++i) {
        NphRenderProxyInstance *p = &g->placed[i];
        memset(p, 0, sizeof *p);
        const NphDVec3 t = grove_place(i);
        p->transform[0][0] = 1.0;
        p->transform[1][1] = 1.0;
        p->transform[2][2] = 1.0;
        p->transform[0][3] = t.x;
        p->transform[1][3] = t.y;
        p->transform[2][3] = t.z;
        p->mesh = 0U;
    }
    const NphRenderProxyMesh mesh = {&g->tree.lods, g->positions, &g->sampler, 1U};
    NphRenderCellProxyInput in;
    memset(&in, 0, sizeof in);
    in.meshes = &mesh;
    in.mesh_count = 1U;
    in.instances = g->placed;
    in.instance_count = GROVE_TREES;
    in.cell = GROVE_CELL;
    in.distance = 60.0F;
    in.scratch = a;
    nph_render_cluster_lods_init(&g->proxy, a);
    NphRenderCellProxyStats stats;
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &g->proxy, &stats), NPH_RESULT_OK);
    NTEST_LOG("the grove's proxy: %u levels, %u clusters, %u voxels", stats.levels,
              g->proxy.clusters.meshlets.len, g->proxy.voxels.len);
    NTEST_ASSERT_GT_UINT(g->proxy.clusters.meshlets.len, 0U);

    // Its voxels compact, as the cooker stores them: packed from a copy of
    // the clusters' records, which then name their own (the builder's form
    // stays whole for the levels apart below).
    nph_render_packed_voxels_init(&g->proxy_packed, a);
    NPH_ARRAY_INIT(&g->proxy_meshlets, a);
    const NphRenderMeshlets *w = &g->proxy.clusters;
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH_N(&g->proxy_meshlets, w->meshlets.data, w->meshlets.len),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_render_voxels_pack(g->proxy_meshlets.data, g->proxy_meshlets.len,
                                               g->proxy.bricks.data, g->proxy.bricks.len,
                                               g->proxy.voxels.data, g->proxy.voxels.len,
                                               &g->proxy_packed),
                        NPH_RESULT_OK);
    memset(&g->proxy_sub, 0, sizeof g->proxy_sub);
    proxy_desc_of(&g->proxy, &g->proxy_packed, &g->proxy_sub, &g->proxy_desc);
    g->proxy_desc.meshlets = g->proxy_meshlets.data;
    g->proxy_desc.name = "grove proxy";
}

static void grove_free(Grove *g)
{
    NPH_ARRAY_FREE(&g->proxy_meshlets);
    nph_render_packed_voxels_free(&g->proxy_packed);
    nph_render_cluster_lods_free(&g->proxy);
    NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, g->positions, g->tree.data.vertex_count);
    lod_mesh_free(&g->tree);
}

// The grove's proxy as its levels apart (render/cluster_lod.h "Levels
// apart"; ADR-0063), as the cooker writes them: a mesh a level, its makers
// as records, its voxels compact.
typedef struct GroveLevels {
    u32 count;
    NphRenderClusterLods lods[NPH_RENDER_CELL_MAX_LEVELS];
    NphRenderLodMakerArray makers[NPH_RENDER_CELL_MAX_LEVELS];
    NphRenderPackedVoxels packed[NPH_RENDER_CELL_MAX_LEVELS];
    NphRenderMeshDesc desc[NPH_RENDER_CELL_MAX_LEVELS];
} GroveLevels;

static void grove_levels_build(const Grove *g, GroveLevels *gl)
{
    NphAllocator *a = nph_allocator_system();
    memset(gl, 0, sizeof *gl);
    NTEST_ASSERT_LE_UINT(g->proxy.levels, NPH_RENDER_CELL_MAX_LEVELS);
    gl->count = g->proxy.levels;
    for (u32 k = 0; k < gl->count; ++k) {
        nph_render_cluster_lods_init(&gl->lods[k], a);
        NPH_ARRAY_INIT(&gl->makers[k], a);
        nph_render_packed_voxels_init(&gl->packed[k], a);
        NTEST_ASSERT_EQ_INT(
            nph_render_cluster_lods_level(&g->proxy, k, &gl->lods[k], &gl->makers[k]),
            NPH_RESULT_OK);
        NphRenderMeshlets *w = &gl->lods[k].clusters;
        NTEST_ASSERT_EQ_INT(nph_render_voxels_pack(w->meshlets.data, w->meshlets.len,
                                                   gl->lods[k].bricks.data, gl->lods[k].bricks.len,
                                                   gl->lods[k].voxels.data, gl->lods[k].voxels.len,
                                                   &gl->packed[k]),
                            NPH_RESULT_OK);
        proxy_desc_of(&gl->lods[k], &gl->packed[k], &g->proxy_sub, &gl->desc[k]);
        gl->desc[k].lod_makers = gl->makers[k].data;
        gl->desc[k].name = "grove proxy level";
    }
}

static void grove_levels_free(GroveLevels *gl)
{
    for (u32 k = 0; k < gl->count; ++k) {
        nph_render_packed_voxels_free(&gl->packed[k]);
        NPH_ARRAY_FREE(&gl->makers[k]);
        nph_render_cluster_lods_free(&gl->lods[k]);
    }
}

// The grove in the renderer: the trees' mesh, the proxy's, and a cell of
// `error` and `distance` (the proxy's own finest cell and distance, or a
// test's) whose proxy stands for the trees, which name it (with `cells`).
typedef struct GroveScene {
    NphRenderMesh tree;
    NphRenderMesh proxy;
    // The cell's proxy: `proxy`, or the levels apart's meshes from
    // `first_level` on (the finer ones not held, as a streamed proxy's
    // before they arrive).
    NphRenderMesh levels[NPH_RENDER_CELL_MAX_LEVELS];
    u32 level_count;
    u32 first_level;
    // Each level's largest consumer error, whose requests the cell answers.
    f32 level_errors[NPH_RENDER_CELL_MAX_LEVELS];
    NphRenderMaterial green;
    NphRenderCell cell;
    NphRenderInstance trees[GROVE_TREES];
} GroveScene;

static NphRenderCellDesc grove_cell_desc(const GroveScene *s, f32 error, f32 distance)
{
    NphRenderCellDesc d;
    memset(&d, 0, sizeof d);
    d.center = nph_dvec3(0.0, 0.0, 0.0);
    d.extent = nph_vec3(2.5F, 1.0F, 2.5F); // the trees' 2 m cubes at +-1.5 m
    d.proxy_levels = s->level_count != 0U ? s->levels + s->first_level : &s->proxy;
    d.proxy_level_count = s->level_count != 0U ? s->level_count - s->first_level : 1U;
    d.proxy_materials = &s->green;
    d.proxy_material_count = 1U;
    d.proxy_transform = nph_render_transform_identity();
    d.proxy_error = error;
    d.distance = distance;
    d.proxy_level_errors = s->level_count != 0U ? s->level_errors : NULL;
    d.proxy_level_error_count = s->level_count;
    return d;
}

// With `levels`, the cell's proxy is the levels apart from `first_level` on.
static void grove_scene_build_with(RenderFixture *f, const Grove *g, GroveScene *s, bool cells,
                                   f32 error, f32 distance, const GroveLevels *levels,
                                   u32 first_level)
{
    memset(s, 0, sizeof *s);
    s->first_level = first_level;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g->tree.desc, &s->tree),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g->proxy_desc, &s->proxy),
                        NPH_RESULT_OK);
    for (u32 k = 0; levels != NULL && k < levels->count; ++k) {
        NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &levels->desc[k], &s->levels[k]),
                            NPH_RESULT_OK);
        f32 e = 0.0F;
        for (u32 gi = 0; gi < levels->lods[k].groups.len; ++gi) {
            e = nph_f32_max(e, levels->lods[k].groups.data[gi].error);
        }
        s->level_errors[k] = e;
        s->level_count += 1U;
    }
    s->green = material(f, nph_vec4(0.3F, 0.5F, 0.2F, 1.0F));
    if (cells) {
        const NphRenderCellDesc cd = grove_cell_desc(s, error, distance);
        NTEST_ASSERT_EQ_INT(nph_render_cell_create(f->renderer, &cd, &s->cell), NPH_RESULT_OK);
    }
    for (u32 i = 0; i < GROVE_TREES; ++i) {
        NphRenderInstanceDesc id;
        memset(&id, 0, sizeof id);
        id.mesh = s->tree;
        id.materials = &s->green;
        id.material_count = 1U;
        id.transform = nph_render_transform_identity();
        id.transform.translation = grove_place(i);
        id.cell = s->cell;
        NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &s->trees[i]),
                            NPH_RESULT_OK);
    }
}

static void grove_scene_build(RenderFixture *f, const Grove *g, GroveScene *s, bool cells,
                              f32 error, f32 distance)
{
    grove_scene_build_with(f, g, s, cells, error, distance, NULL, 0U);
}

static void grove_scene_free(RenderFixture *f, GroveScene *s)
{
    for (u32 i = 0; i < GROVE_TREES; ++i) {
        nph_render_instance_destroy(f->renderer, s->trees[i]);
    }
    nph_render_cell_destroy(f->renderer, s->cell);
    nph_render_material_destroy(f->renderer, s->green);
    for (u32 k = 0; k < s->level_count; ++k) {
        nph_render_mesh_destroy(f->renderer, s->levels[k]);
    }
    nph_render_mesh_destroy(f->renderer, s->proxy);
    nph_render_mesh_destroy(f->renderer, s->tree);
}

// A production view from `distance` on +Z of the grove, a little above it:
// no temporal jitter, no tonemapping, no environment, over black.
static NphRenderView grove_view(f64 distance, f32 lod_error)
{
    NphRenderView v;
    nph_render_view_default(&v, nph_render_camera_look_at(nph_dvec3(0.0, 0.3 * distance, distance),
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
// come NPH_GPU_FRAMES_IN_FLIGHT frames later.
static NphRenderFrameStats grove_frames(RenderFixture *f, const NphRenderView *v)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < 4U; ++i) {
        stats = test_render_read(f, v);
    }
    return stats;
}

// The statistics of the first frame of a scene (read back
// NPH_GPU_FRAMES_IN_FLIGHT frames later): the one whose shadow map draws the
// pages its new casters touch.
static NphRenderFrameStats first_frame(RenderFixture *f, const NphRenderView *v)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        stats = test_render_read(f, v);
    }
    return stats;
}

static u32 covered_pixels(const u8 *pixels, u64 n)
{
    u32 count = 0U;
    for (u64 i = 0; i < n; ++i) {
        const u8 *p = pixels + i * 4U;
        count += p[0] != 0U || p[1] != 0U || p[2] != 0U ? 1U : 0U;
    }
    return count;
}

// A sun from above and behind the camera, unshadowed: what the views see
// is lit (the views have no environment).
static NphRenderLight grove_light(RenderFixture *f)
{
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 3.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(-0.4F, -0.7F, -0.6F));
    return make_light(f, &ld);
}

// A target of 1,024 pixels square, whose coverage counts a grove's sub-pixel
// needles with less noise than the fixture's 64 (at a threshold of 4 pixels
// its views take the cuts of 256 pixels at 1).
#define BIG_SIZE   1024U
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

// first_frame into the big target, the last frame read back.
static NphRenderFrameStats big_frames(RenderFixture *f, BigTarget *t, const NphRenderView *v)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < 1U + NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        stats = test_render_frame(f, v, t->image);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    test_read_image(f->dev, t->image, 0U, t->pixels, BIG_PIXELS * 4U);
    return stats;
}

// ---------------------------------------------------------------------------
// Meshes of voxels alone
// ---------------------------------------------------------------------------

// A cell's proxy is a mesh of voxels alone: no vertices, no indices, its
// level 0 voxel clusters too. The renderer takes it and the production path
// draws it, from near (its finest levels) to far.
NTEST_F(Cells, a_mesh_of_voxels_alone_is_drawn)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    NphRenderMesh proxy = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g.proxy_desc, &proxy), NPH_RESULT_OK);
    const NphRenderMaterial green = material(f, nph_vec4(0.3F, 0.5F, 0.2F, 1.0F));
    const NphRenderInstance inst = instance(f, proxy, green, nph_render_transform_identity());
    const NphRenderLight light = grove_light(f);
    const f64 distances[3] = {6.0, 20.0, 60.0};
    for (u32 i = 0; i < 3U; ++i) {
        const NphRenderView v = grove_view(distances[i], 1.0F);
        const NphRenderFrameStats stats = grove_frames(f, &v);
        const u32 covered = covered_pixels(f->pixels, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE);
        NTEST_LOG("from %.0f m: %u voxel clusters, %u bricks, %u triangles, %u pixels",
                  distances[i], stats.vis_voxel_clusters, stats.vis_voxel_bricks,
                  stats.vis_triangles, covered);
        NTEST_EXPECT_GT_UINT(stats.vis_voxel_clusters, 0U);
        NTEST_EXPECT_EQ_UINT(stats.vis_triangles, 0U);
        NTEST_EXPECT_GT_UINT(covered, 0U);
    }
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_material_destroy(f->renderer, green);
    nph_render_mesh_destroy(f->renderer, proxy);
    grove_free(&g);
}

// A mesh of voxels alone gives the GPU no triangles to read: a cluster with
// vertices, a submesh with indices, meshlet vertices or triangles, or one
// without its meshlets, its levels, or its voxels is refused.
NTEST_F(Cells, malformed_meshes_of_voxels_alone_are_refused)
{
    REQUIRE_RENDERER(f);
    Grove g;
    grove_build(&g);
    NphAllocator *a = nph_allocator_system();
    const u32 n = g.proxy_desc.meshlet_count;
    NphRenderMeshlet *meshlets = NPH_ALLOC_ARRAY(a, NphRenderMeshlet, n);
    NTEST_ASSERT_NOT_NULL(meshlets);
    NphRenderMeshDesc d = g.proxy_desc;
    NphRenderSubmesh sub = g.proxy_sub;
    d.meshlets = meshlets;
    d.submeshes = &sub;
    static const u32 k_vertex = 0U;
    static const u8 k_corners[3] = {0U, 0U, 0U};
#define RESET()                                                                                    \
    do {                                                                                           \
        memcpy(meshlets, g.proxy_desc.meshlets, sizeof *meshlets *n);                              \
        d = g.proxy_desc;                                                                          \
        d.meshlets = meshlets;                                                                     \
        sub = g.proxy_sub;                                                                         \
        d.submeshes = &sub;                                                                        \
    } while (0)
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
    // A cluster of level 0 with vertices: a triangle cluster without any to
    // index.
    meshlets[0].vertex_count = 1U;
    EXPECT_REFUSED();
    // A submesh with indices.
    sub.index_count = 3U;
    EXPECT_REFUSED();
    // Meshlet vertices and triangles.
    d.meshlet_vertices = &k_vertex;
    d.meshlet_vertex_count = 1U;
    EXPECT_REFUSED();
    d.meshlet_triangles = k_corners;
    d.meshlet_triangle_count = 1U;
    EXPECT_REFUSED();
    // Without its meshlets, its levels, or its voxels.
    d.meshlets = NULL;
    EXPECT_REFUSED();
    d.lod_nodes = NULL;
    d.cluster_lods = NULL;
    d.lod_groups = NULL;
    d.lod_group_clusters = NULL;
    d.lod_node_count = 0U;
    d.lod_group_count = 0U;
    d.lod_group_cluster_count = 0U;
    d.lod_level0_meshlets = 0U;
    EXPECT_REFUSED();
    d.voxel_bricks = NULL;
    d.voxel_brick_count = 0U;
    EXPECT_REFUSED();
    // Vertices without indices is no mesh of voxels alone, nor a mesh.
    d.vertex_count = 3U;
    EXPECT_REFUSED();
#undef EXPECT_REFUSED
#undef RESET
    NPH_FREE_ARRAY(a, NphRenderMeshlet, meshlets, n);
    grove_free(&g);
}

// ---------------------------------------------------------------------------
// The voxel pool (ADR-0063 stage 3c)
// ---------------------------------------------------------------------------

typedef RenderFixture VoxelPool;

NTEST_SETUP(VoxelPool)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_visible_clusters = 1U << 16U;
    d.max_cull_meshlets = 1U << 18U;
    d.max_deferred = 1U << 16U;
    d.voxel_pool_bytes = 16U << 20U;
    test_render_setup(f, &d);
}

NTEST_TEARDOWN(VoxelPool)
{
    test_render_teardown(f);
}

#define POOL_IMAGE ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)

// The grove's proxy as `mesh` draws it from 20 m, its normals (no light or
// history to tell two frames apart), into `out`.
static void pool_image(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial m, u8 *out)
{
    const NphRenderInstance inst = instance(f, mesh, m, nph_render_transform_identity());
    NphRenderView v = grove_view(20.0, 1.0F);
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    grove_frames(f, &v);
    memcpy(out, f->pixels, POOL_IMAGE);
    nph_render_instance_destroy(f->renderer, inst);
}

static NphRange pool_range_of(const RenderFixture *f, NphRenderMesh mesh)
{
    return f->renderer->meshes[render_slot_of(&f->renderer->mesh_pool, mesh.value)].voxel_range;
}

static u64 pool_used(RenderFixture *f)
{
    u64 used = 0U;
    u64 size = 0U;
    nph_render_voxel_pool_usage(f->renderer, &used, &size);
    return used;
}

typedef struct PoolUpload {
    NphRenderer *renderer;
    NphRenderVoxelMesh mesh;
    const NphRenderMeshDesc *desc;
    NphResult result;
} PoolUpload;

static void pool_upload_job(void *user)
{
    PoolUpload *u = (PoolUpload *)user;
    u->result = nph_render_voxel_mesh_upload(u->renderer, u->mesh, u->desc, nph_allocator_system());
}

// A mesh of voxels alone in the voxel pool, made at once
// (nph_render_mesh_create) or in steps with its upload on a loading job,
// draws byte for byte as the same mesh with buffers of its own (the pool
// hidden); it holds a range of the pool, not a buffer, and its range comes
// back once it is destroyed and the frames in flight are done with it.
NTEST_F(VoxelPool, a_mesh_in_the_pool_draws_as_one_with_buffers_of_its_own)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    NphRenderer *r = f->renderer;
    NphAllocator *a = nph_allocator_system();
    u8 *own = NPH_ALLOC_ARRAY(a, u8, POOL_IMAGE);
    u8 *pooled = NPH_ALLOC_ARRAY(a, u8, POOL_IMAGE);
    u8 *stepped = NPH_ALLOC_ARRAY(a, u8, POOL_IMAGE);
    NTEST_ASSERT_TRUE(own != NULL && pooled != NULL && stepped != NULL);
    const NphRenderMaterial green = material(f, nph_vec4(0.3F, 0.5F, 0.2F, 1.0F));
    u64 used = 0U;
    u64 size = 0U;
    nph_render_voxel_pool_usage(r, &used, &size);
    NTEST_EXPECT_EQ_UINT((u32)(size >> 20U), 16U);
    NTEST_EXPECT_TRUE(used == 0U);

    // Buffers of its own: the pool hidden.
    const u32 units = r->vis.voxel_pool_units;
    r->vis.voxel_pool_units = 0U;
    NphRenderMesh with_buffers = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(r, &g.proxy_desc, &with_buffers), NPH_RESULT_OK);
    r->vis.voxel_pool_units = units;
    NTEST_EXPECT_TRUE(nph_range_is_null(pool_range_of(f, with_buffers)));
    pool_image(f, with_buffers, green, own);
    NTEST_ASSERT_GT_UINT(covered_pixels(own, (u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE), 0U);

    // At once, in the pool.
    NphRenderMesh at_once = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(r, &g.proxy_desc, &at_once), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_range_is_null(pool_range_of(f, at_once)));
    const u64 one = pool_used(f);
    NTEST_EXPECT_TRUE(one >= (u64)g.proxy_desc.voxel_word_count * sizeof(u32));
    pool_image(f, at_once, green, pooled);

    // In steps, the upload on a job.
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = a;
    jd.worker_count = 2U;
    NphJobSystem *jobs = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &jobs), NPH_RESULT_OK);
    NphRenderVoxelMesh begun = NPH_RENDER_VOXEL_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_begin(r, &begun), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_render_mesh_is_valid(r, (NphRenderMesh){begun.value}));
    PoolUpload upload = {r, begun, &g.proxy_desc, NPH_RESULT_CORRUPT};
    const NphJobDecl decl = {pool_upload_job, &upload, "voxel mesh upload"};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(jobs, &decl, 1U, NPH_JOB_PRIORITY_LOW, &done), NPH_RESULT_OK);
    nph_job_wait(jobs, &done);
    NTEST_EXPECT_EQ_INT(upload.result, NPH_RESULT_OK);
    NphRenderMesh stepped_mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_finish(r, begun, &stepped_mesh), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_render_mesh_is_valid(r, stepped_mesh));
    NTEST_EXPECT_TRUE(pool_used(f) == 2U * one);
    pool_image(f, stepped_mesh, green, stepped);
    nph_job_system_destroy(jobs);

    u32 differ_pooled = 0U;
    u32 differ_stepped = 0U;
    for (u64 p = 0; p < POOL_IMAGE; p += 4U) {
        differ_pooled += memcmp(own + p, pooled + p, 4U) != 0 ? 1U : 0U;
        differ_stepped += memcmp(own + p, stepped + p, 4U) != 0 ? 1U : 0U;
    }
    NTEST_LOG("the pool: %llu bytes a mesh; pixels differing from buffers of its own: %u at once, "
              "%u in steps",
              (unsigned long long)one, differ_pooled, differ_stepped);
    NTEST_EXPECT_EQ_UINT(differ_pooled, 0U);
    NTEST_EXPECT_EQ_UINT(differ_stepped, 0U);

    // Destroyed, the ranges return once the frames in flight are done.
    nph_render_mesh_destroy(r, at_once);
    nph_render_mesh_destroy(r, stepped_mesh);
    nph_render_mesh_destroy(r, with_buffers);
    NTEST_EXPECT_TRUE(pool_used(f) == 2U * one);
    const NphRenderView v = grove_view(20.0, 1.0F);
    first_frame(f, &v);
    NTEST_EXPECT_TRUE(pool_used(f) == 0U);
    nph_render_material_destroy(r, green);
    NPH_FREE_ARRAY(a, u8, stepped, POOL_IMAGE);
    NPH_FREE_ARRAY(a, u8, pooled, POOL_IMAGE);
    NPH_FREE_ARRAY(a, u8, own, POOL_IMAGE);
    grove_free(&g);
}

// The pool's steps refuse what they cannot take and give back what they
// reserved: no pool; an upload of a mesh of triangles, of a mesh whose
// voxels lie out of bounds, or past the pool's room (where
// nph_render_mesh_create gives the mesh buffers of its own), which the
// finish answers with the upload's error; a cancel; and steps out of turn.
NTEST_F(VoxelPool, the_pools_steps_refuse_and_give_back)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    NphRenderer *r = f->renderer;
    NphAllocator *a = nph_allocator_system();
    const u32 live = nph_handle_pool_live(&r->mesh_pool);
    NphRenderVoxelMesh m = NPH_RENDER_VOXEL_MESH_NULL;
    NphRenderMesh out = NPH_RENDER_MESH_NULL;
    const u32 units = r->vis.voxel_pool_units;
    r->vis.voxel_pool_units = 0U;
    NTEST_EXPECT_EQ_INT(nph_render_voxel_mesh_begin(r, &m), NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_EQ_UINT(m.value, 0U);
    r->vis.voxel_pool_units = units;
    // A mesh of triangles.
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_begin(r, &m), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_render_voxel_mesh_upload(r, m, &g.tree.desc, a),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_voxel_mesh_finish(r, m, &out), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(out.value, 0U);
    // Past its room.
    r->vis.voxel_pool_units = 1U;
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_begin(r, &m), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_render_voxel_mesh_upload(r, m, &g.proxy_desc, a), NPH_RESULT_FULL);
    NTEST_EXPECT_EQ_INT(nph_render_voxel_mesh_finish(r, m, &out), NPH_RESULT_FULL);
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(r, &g.proxy_desc, &out), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_range_is_null(pool_range_of(f, out)));
    nph_render_mesh_destroy(r, out);
    r->vis.voxel_pool_units = units;
    NTEST_EXPECT_TRUE(pool_used(f) == 0U);
    NTEST_EXPECT_EQ_UINT(nph_handle_pool_live(&r->mesh_pool), live);

    // Voxels out of bounds: refused by the upload, after reserving nothing.
    NphRenderMeshDesc bad = g.proxy_desc;
    bad.voxel_word_count = 1U;
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_begin(r, &m), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_render_voxel_mesh_upload(r, m, &bad, a), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_TRUE(pool_used(f) == 0U);
    NTEST_EXPECT_EQ_INT(nph_render_voxel_mesh_finish(r, m, &out), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(out.value, 0U);
    NTEST_EXPECT_EQ_UINT(nph_handle_pool_live(&r->mesh_pool), live);
    // Cancelled before its upload, and after it.
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_begin(r, &m), NPH_RESULT_OK);
    nph_render_voxel_mesh_cancel(r, m);
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_begin(r, &m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_upload(r, m, &g.proxy_desc, a), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(pool_used(f) != 0U);
    nph_render_voxel_mesh_cancel(r, m);
    nph_render_voxel_mesh_cancel(r, NPH_RENDER_VOXEL_MESH_NULL);
    NTEST_EXPECT_EQ_UINT(nph_handle_pool_live(&r->mesh_pool), live);
    // Out of turn: finished before its upload, uploaded twice.
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_begin(r, &m), NPH_RESULT_OK);
    NphResult res = NPH_RESULT_OK;
    NPH_TEST_EXPECT_ASSERT(res = nph_render_voxel_mesh_finish(r, m, &out));
    NTEST_EXPECT_EQ_INT(res, NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_upload(r, m, &g.proxy_desc, a), NPH_RESULT_OK);
    NPH_TEST_EXPECT_ASSERT(res = nph_render_voxel_mesh_upload(r, m, &g.proxy_desc, a));
    NTEST_EXPECT_EQ_INT(res, NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_render_voxel_mesh_finish(r, m, &out), NPH_RESULT_OK);
    nph_render_mesh_destroy(r, out);
    // Every range back once the frames in flight are done.
    const NphRenderView v = grove_view(20.0, 1.0F);
    first_frame(f, &v);
    NTEST_EXPECT_TRUE(pool_used(f) == 0U);
    NTEST_EXPECT_EQ_UINT(nph_handle_pool_live(&r->mesh_pool), live);
    grove_free(&g);
}

// The reference renderer and the rays take triangles: they pass a mesh of
// voxels alone by, with no draws and no bottom level for it.
NTEST_F(Cells, the_reference_renderer_and_rays_pass_a_mesh_of_voxels_alone_by)
{
    REQUIRE_RENDERER(f);
    Grove g;
    grove_build(&g);
    NphRenderMesh proxy = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g.proxy_desc, &proxy), NPH_RESULT_OK);
    // Beside a floor, the top level's one instance where rays are traced.
    Scene floor;
    scene_build(f, &floor, at(0.0, -1.0, 0.0, 0.01F));
    const NphRenderInstance inst = instance(f, proxy, floor.grey, nph_render_transform_identity());
    NphRenderLightDesc sun = sun_desc(true);
    const NphRenderLight light = make_light(f, &sun);
    NphRenderView v;
    shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_REFERENCE, 0U);
    const NphRenderFrameStats first = test_render_read(f, &v);
    const NphRenderFrameStats stats = test_render_read(f, &v);
    NTEST_LOG("reference: %u draws, %u triangles; shadow rays %d, %u bottom levels built in the "
              "first frame",
              stats.draws, stats.triangles, (int)first.shadow_rays, first.shadow_builds);
    // The floor's and the occluder's draws, a submesh each; none of the proxy.
    NTEST_EXPECT_EQ_UINT(stats.draws, 2U);
    NTEST_EXPECT_EQ_UINT(stats.instances, 3U);
    if (first.shadow_rays) {
        // Their bottom levels, built in the first frame that traces; none of
        // the proxy, which the top level leaves out.
        NTEST_EXPECT_EQ_UINT(first.shadow_builds, 2U);
    }
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_instance_destroy(f->renderer, floor.floor);
    nph_render_instance_destroy(f->renderer, floor.occluder);
    nph_render_material_destroy(f->renderer, floor.grey);
    nph_render_mesh_destroy(f->renderer, floor.cube);
    nph_render_mesh_destroy(f->renderer, floor.plane);
    nph_render_mesh_destroy(f->renderer, proxy);
    grove_free(&g);
}

// The production path's top level leaves the instances of meshes of voxels
// alone out (they have no bottom level), so one made, moved, hidden, shown
// and destroyed keeps it as it was built, as a streamed cell's levels come
// and go (ADR-0063 stage 4); an instance of triangles changing rebuilds it.
NTEST_F(Cells, an_instance_of_voxels_alone_leaves_the_top_level_as_it_is)
{
    REQUIRE_RENDERER(f);
    if (!nph_renderer_ray_tracing_supported(f->renderer) ||
        !nph_renderer_production_supported(f->renderer)) {
        NTEST_SKIP("no ray queries or no production path on this device");
    }
    Grove g;
    grove_build(&g);
    NphRenderMesh proxy = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g.proxy_desc, &proxy), NPH_RESULT_OK);
    Scene floor;
    scene_build(f, &floor, at(0.0, -1.0, 0.0, 0.01F));
    NphRenderLightDesc sun = sun_desc(true);
    const NphRenderLight light = make_light(f, &sun);
    NphRenderView v;
    shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_PRODUCTION,
                (u32)NPH_RENDER_VIS_RT_SHADOWS);
    // Built, its bottom levels compacted a few frames on (where the device
    // saves anything) and rebuilt over the copies: then kept.
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rebuild);
    for (u32 i = 0; i < 12U; ++i) {
        (void)test_render_read(f, &v);
    }
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rebuild);
    NphRenderInstance inst = instance(f, proxy, floor.grey, nph_render_transform_identity());
    NphRenderFrameStats st = test_render_read(f, &v);
    NTEST_EXPECT_FALSE(st.shadow_rebuild);
    NTEST_EXPECT_EQ_UINT(st.instances, 3U);
    NphRenderTransform moved = nph_render_transform_identity();
    moved.translation = nph_dvec3(0.5, 0.0, 0.25);
    NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, inst, &moved),
                        NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rebuild);
    nph_render_instance_set_hidden(f->renderer, inst, true);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rebuild);
    nph_render_instance_set_hidden(f->renderer, inst, false);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rebuild);
    nph_render_instance_destroy(f->renderer, inst);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rebuild);
    // The occluder, of triangles, hidden and shown: rebuilt each time, then
    // kept.
    nph_render_instance_set_hidden(f->renderer, floor.occluder, true);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rebuild);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rebuild);
    nph_render_instance_set_hidden(f->renderer, floor.occluder, false);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rebuild);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rebuild);
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, floor.floor);
    nph_render_instance_destroy(f->renderer, floor.occluder);
    nph_render_material_destroy(f->renderer, floor.grey);
    nph_render_mesh_destroy(f->renderer, floor.cube);
    nph_render_mesh_destroy(f->renderer, floor.plane);
    nph_render_mesh_destroy(f->renderer, proxy);
    grove_free(&g);
}

// ---------------------------------------------------------------------------
// The cells' runs of instance slots (ADR-0064)
// ---------------------------------------------------------------------------

static int compare_u32(const void *x, const void *y)
{
    const u32 a = *(const u32 *)x;
    const u32 b = *(const u32 *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}

// Run `run` holds the slots of `instances` (`count` of them) and nothing else,
// each slot's place in it its run_entry.
static void expect_run(RenderFixture *f, const RenderRun *run, const NphRenderInstance *instances,
                       u32 count)
{
    const NphRenderer *r = f->renderer;
    NTEST_ASSERT_EQ_UINT(run->count, count);
    NTEST_ASSERT_LE_UINT(run->count, run->capacity);
    u32 want[64];
    u32 have[64];
    NTEST_ASSERT_LE_UINT(count, 64U);
    for (u32 i = 0; i < count; ++i) {
        want[i] = render_slot_of(&r->instance_pool, instances[i].value);
        const u32 slot = r->vis.member_entries[run->range.offset + i];
        have[i] = slot;
        NTEST_EXPECT_EQ_UINT(r->instances[slot].run_entry, i);
    }
    qsort(want, count, sizeof want[0], compare_u32);
    qsort(have, count, sizeof have[0], compare_u32);
    for (u32 i = 0; i < count; ++i) {
        NTEST_EXPECT_EQ_UINT(have[i], want[i]);
    }
}

// The member buffer and the cell's run record on the GPU, after a frame, as
// the CPU keeps them.
static void expect_runs_on_gpu(RenderFixture *f, u32 cell_slot)
{
    const RenderVis *v = &f->renderer->vis;
    const RenderCell *c = &f->renderer->cells[cell_slot];
    NphVisCellRun rec;
    test_read_buffer(f->dev, v->cell_runs, (u64)cell_slot * sizeof rec, sizeof rec, &rec);
    NTEST_EXPECT_EQ_UINT(rec.member_count, c->members_run.count);
    NTEST_EXPECT_EQ_UINT(rec.proxy_count, c->proxies_run.count);
    const RenderRun *runs[3] = {&c->members_run, &c->proxies_run, &v->loose};
    const u32 firsts[3] = {rec.members, rec.proxies, v->loose.range.offset};
    for (u32 k = 0; k < 3U; ++k) {
        if (runs[k]->count == 0U) {
            continue;
        }
        NTEST_EXPECT_EQ_UINT(firsts[k], runs[k]->range.offset);
        u32 gpu[64];
        NTEST_ASSERT_LE_UINT(runs[k]->count, 64U);
        test_read_buffer(f->dev, v->cell_members, (u64)runs[k]->range.offset * sizeof(u32),
                         (u64)runs[k]->count * sizeof(u32), gpu);
        NTEST_EXPECT_EQ_INT(memcmp(gpu, v->member_entries + runs[k]->range.offset,
                                   (size_t)runs[k]->count * sizeof(u32)),
                            0);
    }
}

// A cell's members, its proxy's instances and the instances of no cell are
// kept as runs of slots (vis_runs.c): each run holds its instances and
// nothing else, through growth past a run's first range (16 slots), members
// leaving from its start, middle and end, and its proxy replaced; the GPU's
// copies match after a frame; a cell destroyed gives its runs back.
NTEST_F(Cells, a_cells_instances_are_kept_as_runs_of_slots)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    GroveScene s;
    grove_scene_build_with(f, &g, &s, true, GROVE_CELL / 64.0F, 30.0F, &gl, 1U);
    Scene floor;
    scene_build(f, &floor, at(0.0, -5.0, 0.0, 0.01F));
    NphRenderer *r = f->renderer;
    const NphRenderView v = grove_view(20.0, 1.0F);
    const u32 cell_slot = render_slot_of(&r->cell_pool, s.cell.value);
    const RenderCell *c = &r->cells[cell_slot];
    // The trees, the proxy's two levels, the floor's two.
    NphRenderInstance members[48];
    u32 member_count = 0U;
    for (u32 i = 0; i < GROVE_TREES; ++i) {
        members[member_count++] = s.trees[i];
    }
    expect_run(f, &c->members_run, members, member_count);
    expect_run(f, &c->proxies_run, c->proxy, c->proxy_count);
    NTEST_EXPECT_EQ_UINT(c->proxies_run.count, 2U);
    const NphRenderInstance loose[2] = {floor.floor, floor.occluder};
    expect_run(f, &r->vis.loose, loose, 2U);
    (void)test_render_read(f, &v);
    expect_runs_on_gpu(f, cell_slot);
    // Past the first range: 40 more members move the run twice (16, 32, 64).
    for (u32 i = 0; i < 40U; ++i) {
        NphRenderInstanceDesc id;
        memset(&id, 0, sizeof id);
        id.mesh = s.tree;
        id.materials = &s.green;
        id.material_count = 1U;
        id.transform = nph_render_transform_identity();
        id.transform.translation = nph_dvec3(0.1 * (f64)i, 0.0, 0.0);
        id.cell = s.cell;
        NTEST_ASSERT_EQ_INT(nph_render_instance_create(r, &id, &members[member_count]),
                            NPH_RESULT_OK);
        ++member_count;
    }
    expect_run(f, &c->members_run, members, member_count);
    NTEST_EXPECT_EQ_UINT(c->members_run.capacity, 64U);
    (void)test_render_read(f, &v);
    expect_runs_on_gpu(f, cell_slot);
    // Members leave from the run's start (a tree), its middle and its end.
    const u32 leave[3] = {0U, 20U, member_count - 1U};
    for (u32 k = 3U; k-- > 0U;) {
        nph_render_instance_destroy(r, members[leave[k]]);
        members[leave[k]] = members[--member_count];
    }
    expect_run(f, &c->members_run, members, member_count);
    // The proxy given its finest level too: its run follows.
    NTEST_ASSERT_EQ_INT(nph_render_cell_set_proxy(r, s.cell, s.levels, s.level_count),
                        NPH_RESULT_OK);
    expect_run(f, &c->proxies_run, c->proxy, c->proxy_count);
    NTEST_EXPECT_EQ_UINT(c->proxies_run.count, s.level_count);
    (void)test_render_read(f, &v);
    expect_runs_on_gpu(f, cell_slot);
    // Every member gone (the trees among them), then the cell: its runs
    // given back.
    for (u32 i = 0; i < member_count; ++i) {
        nph_render_instance_destroy(r, members[i]);
    }
    NTEST_EXPECT_EQ_UINT(c->members_run.count, 0U);
    for (u32 i = 0; i < GROVE_TREES; ++i) {
        s.trees[i] = NPH_RENDER_INSTANCE_NULL;
    }
    grove_scene_free(f, &s);
    NTEST_EXPECT_EQ_UINT(r->cells[cell_slot].members_run.capacity, 0U);
    NTEST_EXPECT_EQ_UINT(r->cells[cell_slot].proxies_run.capacity, 0U);
    expect_run(f, &r->vis.loose, loose, 2U);
    nph_render_instance_destroy(r, floor.floor);
    nph_render_instance_destroy(r, floor.occluder);
    NTEST_EXPECT_EQ_UINT(r->vis.loose.count, 0U);
    nph_render_material_destroy(r, floor.grey);
    nph_render_mesh_destroy(r, floor.cube);
    nph_render_mesh_destroy(r, floor.plane);
    grove_levels_free(&gl);
    grove_free(&g);
}

// A field of grove cells sharing one grove's meshes: GROVE_FIELD x
// GROVE_FIELD cells GROVE_PITCH metres apart on x and z, centred on the
// origin, each with its proxy (the levels from `first_level` on) and its
// trees.
#define GROVE_FIELD 3U
#define GROVE_PITCH 12.0

typedef struct GroveField {
    NphRenderCell cells[GROVE_FIELD * GROVE_FIELD];
    NphRenderInstance trees[GROVE_FIELD * GROVE_FIELD][GROVE_TREES];
} GroveField;

static void grove_field_build(RenderFixture *f, const GroveScene *s, GroveField *out, f32 error,
                              f32 distance)
{
    memset(out, 0, sizeof *out);
    for (u32 k = 0; k < GROVE_FIELD * GROVE_FIELD; ++k) {
        const u32 column = k % GROVE_FIELD;
        const u32 row = k / GROVE_FIELD;
        const NphDVec3 at =
            nph_dvec3(((f64)column - 1.0) * GROVE_PITCH, 0.0, ((f64)row - 1.0) * GROVE_PITCH);
        NphRenderCellDesc d = grove_cell_desc(s, error, distance);
        d.center = at;
        d.proxy_transform.translation = at;
        NTEST_ASSERT_EQ_INT(nph_render_cell_create(f->renderer, &d, &out->cells[k]), NPH_RESULT_OK);
        for (u32 i = 0; i < GROVE_TREES; ++i) {
            NphRenderInstanceDesc id;
            memset(&id, 0, sizeof id);
            id.mesh = s->tree;
            id.materials = &s->green;
            id.material_count = 1U;
            id.transform = nph_render_transform_identity();
            id.transform.translation = nph_dvec3_add(at, grove_place(i));
            id.cell = out->cells[k];
            NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &out->trees[k][i]),
                                NPH_RESULT_OK);
        }
    }
}

static void grove_field_free(RenderFixture *f, GroveField *field)
{
    for (u32 k = 0; k < GROVE_FIELD * GROVE_FIELD; ++k) {
        for (u32 i = 0; i < GROVE_TREES; ++i) {
            nph_render_instance_destroy(f->renderer, field->trees[k][i]);
        }
        nph_render_cell_destroy(f->renderer, field->cells[k]);
    }
}

// View `v` drawn with the cells culled first and over every instance slot
// (RenderVis.cells_first_off): the same image, byte for byte, and the same
// cells and instances drawn as proxies, triangles and voxel clusters. The
// cells-first frame's statistics.
static NphRenderFrameStats expect_cells_first_same(RenderFixture *f, BigTarget *t,
                                                   const NphRenderView *v, u8 *every,
                                                   const char *what)
{
    f->renderer->vis.cells_first_off = 1;
    const NphRenderFrameStats b = big_frames(f, t, v);
    memcpy(every, t->pixels, BIG_PIXELS * 4U);
    f->renderer->vis.cells_first_off = 0;
    const NphRenderFrameStats a = big_frames(f, t, v);
    u32 differing = 0U;
    for (u64 i = 0; i < BIG_PIXELS * 4U; ++i) {
        differing += t->pixels[i] != every[i] ? 1U : 0U;
    }
    NTEST_LOG("%s: %u pixels covered; %u cells and %u instances drawn as proxies, %u triangles, "
              "%u voxel clusters (every slot: %u, %u, %u, %u); %u bytes differ",
              what, covered_pixels(t->pixels, BIG_PIXELS), a.vis_cells_proxied,
              a.vis_instances_proxied, a.vis_triangles, a.vis_voxel_clusters, b.vis_cells_proxied,
              b.vis_instances_proxied, b.vis_triangles, b.vis_voxel_clusters, differing);
    NTEST_EXPECT_EQ_UINT(differing, 0U);
    NTEST_EXPECT_EQ_UINT(a.vis_cells_proxied, b.vis_cells_proxied);
    NTEST_EXPECT_EQ_UINT(a.vis_instances_proxied, b.vis_instances_proxied);
    NTEST_EXPECT_EQ_UINT(a.vis_triangles, b.vis_triangles);
    NTEST_EXPECT_EQ_UINT(a.vis_voxel_clusters, b.vis_voxel_clusters);
    return a;
}

// Phase 0 culls the cells first where the renderer holds any (ADR-0064): a
// cell out of view lists nothing, one drawn as its proxy its proxy's
// instances alone. Over a field of grove cells, near and far, in view and
// behind the camera, with levels of detail and without, the image and the
// statistics are those of phase 0 over every instance slot. A member made
// outside its cell's box grows the box, so it is drawn from where the box
// it was given is out of view.
NTEST_F(Cells, culling_the_cells_first_draws_what_every_slot_does)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    BigTarget t;
    big_target_init(f, &t);
    u8 *every = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, BIG_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(every);
    // The grove's meshes (its own cell too, at the origin, among the field's).
    GroveScene s;
    grove_scene_build_with(f, &g, &s, false, GROVE_CELL / 64.0F, 30.0F, &gl, 1U);
    GroveField field;
    grove_field_build(f, &s, &field, GROVE_CELL / 64.0F, 30.0F);
    NphRenderView v = grove_view(40.0, 1.0F);
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    // From 40 m: the near row drawn as instances, the rest as proxies.
    NphRenderFrameStats st = expect_cells_first_same(f, &t, &v, every, "from 40 m");
    NTEST_EXPECT_GT_UINT(st.vis_cells_proxied, 0U);
    NTEST_EXPECT_LT_UINT(st.vis_cells_proxied, GROVE_FIELD * GROVE_FIELD);
    NTEST_EXPECT_GT_UINT(st.vis_triangles, 0U);
    // From afar: every cell as its proxy.
    NphRenderView far = grove_view(120.0, 1.0F);
    far.debug_view = NPH_RENDER_VIEW_NORMAL;
    st = expect_cells_first_same(f, &t, &far, every, "from 120 m");
    NTEST_EXPECT_EQ_UINT(st.vis_cells_proxied, GROVE_FIELD * GROVE_FIELD);
    // Turned away: nothing in view.
    NphRenderView away = v;
    away.camera = nph_render_camera_look_at(nph_dvec3(0.0, 12.0, 40.0), nph_dvec3(0.0, 12.0, 80.0),
                                            nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F);
    (void)expect_cells_first_same(f, &t, &away, every, "turned away");
    NTEST_EXPECT_EQ_UINT(covered_pixels(t.pixels, BIG_PIXELS), 0U);
    // Without levels of detail: every tree.
    NphRenderView flat = grove_view(40.0, 0.0F);
    flat.debug_view = NPH_RENDER_VIEW_NORMAL;
    st = expect_cells_first_same(f, &t, &flat, every, "without levels of detail");
    NTEST_EXPECT_EQ_UINT(st.vis_cells_proxied, 0U);
    // A member of the far corner's cell made 20 m off its box, seen from
    // where that box is out of view: the box grew over it, and it is drawn.
    const u32 corner = GROVE_FIELD * GROVE_FIELD - 1U;
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = s.tree;
    id.materials = &s.green;
    id.material_count = 1U;
    id.transform = nph_render_transform_identity();
    id.transform.translation = nph_dvec3(GROVE_PITCH + 20.0, 0.0, GROVE_PITCH);
    id.cell = field.cells[corner];
    NphRenderInstance stray = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &stray), NPH_RESULT_OK);
    const RenderCell *c =
        &f->renderer->cells[render_slot_of(&f->renderer->cell_pool, field.cells[corner].value)];
    NTEST_EXPECT_TRUE(c->extent.x >= 10.0F);
    NphRenderView stray_view = flat;
    // At the member from 8 m on x and z: the box it was given, 20 m on, lies
    // 30 degrees off the view's axis, outside its 17 degrees.
    stray_view.camera = nph_render_camera_look_at(
        nph_dvec3(GROVE_PITCH + 28.0, 3.0, GROVE_PITCH + 8.0),
        nph_dvec3(GROVE_PITCH + 20.0, 0.0, GROVE_PITCH), nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F);
    (void)expect_cells_first_same(f, &t, &stray_view, every, "a member off its box");
    NTEST_EXPECT_GT_UINT(covered_pixels(t.pixels, BIG_PIXELS), 0U);
    nph_render_instance_destroy(f->renderer, stray);
    grove_field_free(f, &field);
    grove_scene_free(f, &s);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, every, BIG_PIXELS * 4U);
    big_target_free(f, &t);
    grove_levels_free(&gl);
    grove_free(&g);
}

// The sun's shadow map drawn afresh (the sun turned and back drops every
// page) until it settles, each frame's caster draws summed, and the last
// image kept in `out`.
typedef struct MapDrawn {
    u32 pages;
    u32 draws;
    u32 voxel_draws;
} MapDrawn;

static MapDrawn map_afresh(RenderFixture *f, NphRenderLight sun, NphRenderLightDesc *ld,
                           const NphRenderView *v, u8 *out)
{
    const NphVec3 direction = ld->direction;
    ld->direction = nph_vec3_normalize(nph_vec3_add(direction, nph_vec3(0.0F, 0.0F, 0.2F)));
    NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, sun, ld), NPH_RESULT_OK);
    (void)test_render_read(f, v);
    ld->direction = direction;
    NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, sun, ld), NPH_RESULT_OK);
    MapDrawn d;
    memset(&d, 0, sizeof d);
    u32 quiet = 0U;
    for (u32 i = 0; i < 64U && quiet <= NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        const NphRenderFrameStats st = test_render_read(f, v);
        // The statistics come NPH_GPU_FRAMES_IN_FLIGHT frames late: the
        // frames before them are the turned sun's.
        if (i >= NPH_GPU_FRAMES_IN_FLIGHT) {
            d.pages += st.vsm_drawn;
            d.draws += st.vsm_draws;
            d.voxel_draws += st.vsm_voxel_draws;
        }
        quiet = i >= NPH_GPU_FRAMES_IN_FLIGHT && st.vsm_drawn == 0U ? quiet + 1U : 0U;
    }
    NTEST_ASSERT_GT_UINT(quiet, NPH_GPU_FRAMES_IN_FLIGHT);
    memcpy(out, f->pixels, PIXELS);
    return d;
}

// The sun's shadow map culls the cells first too (ADR-0064): over the field
// of grove cells, from near (its instances in the fine levels, its proxies
// in the coarse) and far, the map drawn afresh with the cells first and
// over every instance slot draws the same casters, pages and image, byte for
// byte.
NTEST_F(Cells, the_shadow_map_culls_the_cells_first_as_every_slot)
{
    REQUIRE_PRODUCTION(f);
    if (!nph_renderer_virtual_shadows_supported(f->renderer)) {
        NTEST_SKIP("no virtual shadow map on this device");
    }
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    Scene floor;
    scene_build(f, &floor, at(0.0, -5.0, 0.0, 0.01F));
    NphRenderLightDesc sun = sun_desc(true);
    const NphRenderLight light = make_light(f, &sun);
    GroveScene s;
    grove_scene_build_with(f, &g, &s, false, GROVE_CELL / 64.0F, 30.0F, &gl, 1U);
    GroveField field;
    grove_field_build(f, &s, &field, GROVE_CELL / 64.0F, 30.0F);
    static u8 every[PIXELS];
    static u8 cells[PIXELS];
    const f64 distances[2] = {20.0, 80.0};
    for (u32 k = 0; k < 2U; ++k) {
        const NphRenderCamera cam = nph_render_camera_look_at(
            nph_dvec3(0.0, 0.5 * distances[k], distances[k]), nph_dvec3(0.0, 0.0, 0.0),
            nph_vec3(0.0F, 1.0F, 0.0F), 0.9F, 0.05F);
        NphRenderView v;
        shadow_view(&v, cam, 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, 0U);
        v.lod_error = 1.0F;
        v.anti_aliasing = NPH_RENDER_AA_NONE;
        v.ao_radius = 0.0F;
        v.gi = 0;
        f->renderer->vis.cells_first_off = 1;
        const MapDrawn b = map_afresh(f, light, &sun, &v, every);
        f->renderer->vis.cells_first_off = 0;
        const MapDrawn a = map_afresh(f, light, &sun, &v, cells);
        u32 differing = 0U;
        for (u64 i = 0; i < PIXELS; ++i) {
            differing += cells[i] != every[i] ? 1U : 0U;
        }
        NTEST_LOG("from %.0f m: %u pages, %u draws, %u voxel draws drawn afresh (every slot: %u, "
                  "%u, %u); %u bytes differ",
                  distances[k], a.pages, a.draws, a.voxel_draws, b.pages, b.draws, b.voxel_draws,
                  differing);
        NTEST_EXPECT_GT_UINT(a.pages, 0U);
        NTEST_EXPECT_GT_UINT(a.draws + a.voxel_draws, 0U);
        NTEST_EXPECT_EQ_UINT(a.pages, b.pages);
        NTEST_EXPECT_EQ_UINT(a.draws, b.draws);
        NTEST_EXPECT_EQ_UINT(a.voxel_draws, b.voxel_draws);
        NTEST_EXPECT_EQ_UINT(differing, 0U);
    }
    grove_field_free(f, &field);
    grove_scene_free(f, &s);
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, floor.floor);
    nph_render_instance_destroy(f->renderer, floor.occluder);
    nph_render_material_destroy(f->renderer, floor.grey);
    nph_render_mesh_destroy(f->renderer, floor.cube);
    nph_render_mesh_destroy(f->renderer, floor.plane);
    grove_levels_free(&gl);
    grove_free(&g);
}

// ---------------------------------------------------------------------------
// Cells
// ---------------------------------------------------------------------------

// A cell's description is checked: its proxy a live mesh of voxels alone,
// placed by a rotation and a translation, its box, error and distance finite
// (and positive, but for the box's extent); an instance's cell live. A cell
// that instances name outlives them.
NTEST_F(Cells, cells_are_made_and_refused)
{
    REQUIRE_RENDERER(f);
    Grove g;
    grove_build(&g);
    GroveScene s;
    grove_scene_build(f, &g, &s, false, GROVE_CELL, 60.0F);
    NphRenderCellDesc d = grove_cell_desc(&s, GROVE_CELL, 60.0F);
    NphRenderCell cell = NPH_RENDER_CELL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_cell_create(f->renderer, &d, &cell), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_render_cell_is_valid(f->renderer, cell));
    // A cell turned and moved is fine.
    d.proxy_transform = nph_render_transform_from_trs(
        nph_dvec3(100.0, 2.0, -50.0), nph_quat_from_axis_angle(nph_vec3_unit_y(), 0.7F),
        nph_vec3_one());
    NphRenderCell turned = NPH_RENDER_CELL_NULL;
    NTEST_EXPECT_EQ_INT(nph_render_cell_create(f->renderer, &d, &turned), NPH_RESULT_OK);
    nph_render_cell_destroy(f->renderer, turned);
    NTEST_EXPECT_FALSE(nph_render_cell_is_valid(f->renderer, turned));
#define EXPECT_REFUSED()                                                                           \
    do {                                                                                           \
        NphRenderCell none = NPH_RENDER_CELL_NULL;                                                 \
        NTEST_EXPECT_EQ_INT(nph_render_cell_create(f->renderer, &d, &none),                        \
                            NPH_RESULT_INVALID_ARGUMENT);                                          \
        NTEST_EXPECT_EQ_UINT(none.value, 0U);                                                      \
        d = grove_cell_desc(&s, GROVE_CELL, 60.0F);                                                \
    } while (0)
    // A proxy of triangles, or none: a mesh of triangles, a stale one, or one
    // given twice among its meshes, no meshes, or more than a cell takes.
    // Two more meshes of voxels alone; past them, the count alone is wrong.
    NphRenderMesh meshes[NPH_RENDER_CELL_MAX_LEVELS + 1U];
    for (u32 k = 0; k < NPH_ARRAY_COUNT(meshes); ++k) {
        meshes[k] = s.proxy;
    }
    for (u32 k = 1; k < 3U; ++k) {
        NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g.proxy_desc, &meshes[k]),
                            NPH_RESULT_OK);
    }
    const NphRenderMesh second = meshes[1];
    d.proxy_levels = meshes;
    d.proxy_level_count = 3U;
    NphRenderCell three = NPH_RENDER_CELL_NULL;
    NTEST_EXPECT_EQ_INT(nph_render_cell_create(f->renderer, &d, &three), NPH_RESULT_OK);
    nph_render_cell_destroy(f->renderer, three);
    meshes[1] = s.tree;
    d.proxy_levels = meshes;
    d.proxy_level_count = 3U;
    EXPECT_REFUSED();
    meshes[1] = NPH_RENDER_MESH_NULL;
    d.proxy_levels = meshes;
    d.proxy_level_count = 3U;
    EXPECT_REFUSED();
    meshes[1] = meshes[2];
    d.proxy_levels = meshes;
    d.proxy_level_count = 3U;
    EXPECT_REFUSED();
    meshes[1] = second;
    d.proxy_levels = NULL;
    EXPECT_REFUSED();
    d.proxy_level_count = 0U;
    EXPECT_REFUSED();
    d.proxy_levels = meshes;
    d.proxy_level_count = NPH_RENDER_CELL_MAX_LEVELS + 1U;
    EXPECT_REFUSED();
    // Scaled, or mirrored.
    d.proxy_transform = nph_render_transform_from_trs(nph_dvec3(0.0, 0.0, 0.0), nph_quat_identity(),
                                                      nph_vec3_splat(2.0F));
    EXPECT_REFUSED();
    d.proxy_transform.linear = nph_mat3_from_scale(nph_vec3(-1.0F, 1.0F, 1.0F));
    EXPECT_REFUSED();
    // A box, error, or distance that is not finite, or out of range.
    d.center.x = (f64)NPH_F32_INFINITY;
    EXPECT_REFUSED();
    d.extent.y = -1.0F;
    EXPECT_REFUSED();
    d.proxy_error = 0.0F;
    EXPECT_REFUSED();
    d.distance = nph_f32_from_bits(0x7FC00000U);
    EXPECT_REFUSED();
    d.distance = -1.0F;
    EXPECT_REFUSED();
    // A streamed proxy's levels' errors: rising (the coarsest a root's, the
    // largest float) are taken; more than a cell holds, none given with a
    // count, one not finite, or falling are not.
    f32 errors[NPH_RENDER_CELL_MAX_LEVELS + 1U];
    for (u32 k = 0; k < NPH_ARRAY_COUNT(errors); ++k) {
        errors[k] = 0.25F * (f32)(1U << k);
    }
    errors[2] = NPH_F32_MAX;
    d.proxy_level_errors = errors;
    d.proxy_level_error_count = 3U;
    NphRenderCell streamed = NPH_RENDER_CELL_NULL;
    NTEST_EXPECT_EQ_INT(nph_render_cell_create(f->renderer, &d, &streamed), NPH_RESULT_OK);
    nph_render_cell_destroy(f->renderer, streamed);
    errors[2] = 1.0F;
    d.proxy_level_errors = errors;
    d.proxy_level_error_count = NPH_RENDER_CELL_MAX_LEVELS + 1U;
    EXPECT_REFUSED();
    d.proxy_level_errors = NULL;
    d.proxy_level_error_count = 2U;
    EXPECT_REFUSED();
    errors[1] = nph_f32_from_bits(0x7FC00000U);
    d.proxy_level_errors = errors;
    d.proxy_level_error_count = 3U;
    EXPECT_REFUSED();
    errors[1] = 0.125F;
    d.proxy_level_errors = errors;
    d.proxy_level_error_count = 3U;
    EXPECT_REFUSED();
#undef EXPECT_REFUSED
    // An instance's cell, live.
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = s.tree;
    id.transform = nph_render_transform_identity();
    id.cell = cell;
    NphRenderInstance member = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &member), NPH_RESULT_OK);
    // Named, the cell outlives its instance's destruction request.
    NPH_TEST_EXPECT_ASSERT(nph_render_cell_destroy(f->renderer, cell));
    NTEST_EXPECT_TRUE(nph_render_cell_is_valid(f->renderer, cell));
    nph_render_instance_destroy(f->renderer, member);
    nph_render_cell_destroy(f->renderer, cell);
    NTEST_EXPECT_FALSE(nph_render_cell_is_valid(f->renderer, cell));
    // A stale cell.
    NphRenderInstance stale = NPH_RENDER_INSTANCE_NULL;
    NTEST_EXPECT_EQ_INT(nph_render_instance_create(f->renderer, &id, &stale),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_render_cell_destroy(f->renderer, NPH_RENDER_CELL_NULL);
    nph_render_mesh_destroy(f->renderer, second);
    nph_render_mesh_destroy(f->renderer, meshes[2]);
    grove_scene_free(f, &s);
    grove_free(&g);
}

// From afar the cell's proxy stands for its trees: drawn in their place
// where the box's nearest point is past its distance and its finest error
// fine from there, with about their coverage; near, the trees. A view
// without levels of detail draws the trees from anywhere.
NTEST_F(Cells, a_cells_proxy_stands_for_its_instances_from_afar)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    // On the 1,024-pixel target (0.6 rad tall) a unit at distance one spans
    // 1,655 pixels: the proxy's finest cell is fine from 52 m at a threshold
    // of 4 pixels, which decides beside its distance, 30 m.
    const f32 distance = 30.0F;
    GroveScene s;
    grove_scene_build(f, &g, &s, true, GROVE_CELL, distance);
    const NphRenderLight light = grove_light(f);
    BigTarget t;
    big_target_init(f, &t);

    // Near, on the fixture's 64-pixel target (the finest cell fine from 13 m
    // at a pixel's threshold there): the trees.
    NphRenderView v = grove_view(12.0, 1.0F);
    NphRenderFrameStats stats = grove_frames(f, &v);
    NTEST_LOG("near: %u cells drawn as their proxies (for %u instances), %u triangles, %u voxel "
              "clusters",
              stats.vis_cells_proxied, stats.vis_instances_proxied, stats.vis_triangles,
              stats.vis_voxel_clusters);
    NTEST_EXPECT_EQ_UINT(stats.vis_cells_proxied, 0U);
    NTEST_EXPECT_EQ_UINT(stats.vis_instances_proxied, 0U);
    NTEST_EXPECT_GT_UINT(stats.vis_triangles, 0U);
    NTEST_EXPECT_EQ_UINT(stats.vis_voxel_clusters, 0U);

    // From 80 m along the view (the box's nearest point 79 m away): far
    // enough, and fine enough from there.
    v = grove_view(80.0, 4.0F);
    stats = big_frames(f, &t, &v);
    const u32 proxy_pixels = covered_pixels(t.pixels, BIG_PIXELS);
    NTEST_LOG("far: %u cells drawn as their proxies (for %u instances), %u triangles, %u voxel "
              "clusters, %u pixels",
              stats.vis_cells_proxied, stats.vis_instances_proxied, stats.vis_triangles,
              stats.vis_voxel_clusters, proxy_pixels);
    NTEST_EXPECT_EQ_UINT(stats.vis_cells_proxied, 1U);
    NTEST_EXPECT_EQ_UINT(stats.vis_instances_proxied, GROVE_TREES);
    NTEST_EXPECT_EQ_UINT(stats.vis_triangles, 0U);
    NTEST_EXPECT_GT_UINT(stats.vis_voxel_clusters, 0U);

    // Without levels of detail, the trees.
    NphRenderView flat = grove_view(80.0, 0.0F);
    stats = grove_frames(f, &flat);
    NTEST_EXPECT_EQ_UINT(stats.vis_cells_proxied, 0U);
    NTEST_EXPECT_GT_UINT(stats.vis_triangles, 0U);
    NTEST_EXPECT_EQ_UINT(stats.vis_voxel_clusters, 0U);
    grove_scene_free(f, &s);

    // The trees alone, from the same place, at their own cut: the proxy
    // covers about what they do (ADR-0062 "Stage 1, measured": a few to a
    // dozen percent more, and a few pixels of rounding either way here).
    grove_scene_build(f, &g, &s, false, GROVE_CELL, distance);
    stats = big_frames(f, &t, &v);
    const u32 tree_pixels = covered_pixels(t.pixels, BIG_PIXELS);
    NTEST_LOG("the trees alone from there: %u triangles, %u pixels", stats.vis_triangles,
              tree_pixels);
    NTEST_EXPECT_EQ_UINT(stats.vis_cells_proxied, 0U);
    NTEST_ASSERT_GT_UINT(tree_pixels, 0U);
    NTEST_EXPECT_NEAR_F64((f64)proxy_pixels / (f64)tree_pixels, 1.05, 0.3);
    grove_scene_free(f, &s);
    nph_render_light_destroy(f->renderer, light);
    big_target_free(f, &t);
    grove_free(&g);
}

// The sun's shadow map draws a cell's proxy into the levels whose texels its
// finest error fits, its instances into the others. A cell declared finer
// than every level's threshold is drawn as its proxy in every level (and,
// from anywhere past its distance, to the camera); one coarser than them all
// as its instances: the trees cast triangles alone, the proxy voxels alone.
NTEST_F(Cells, the_shadow_map_draws_a_cell_as_its_proxy_where_its_error_fits)
{
    REQUIRE_PRODUCTION(f);
    if (!nph_renderer_virtual_shadows_supported(f->renderer)) {
        NTEST_SKIP("no virtual shadow map on this device");
    }
    Grove g;
    grove_build(&g);
    Scene floor;
    scene_build(f, &floor, at(0.0, -5.0, 0.0, 0.01F));
    NphRenderLightDesc sun = sun_desc(true);
    const NphRenderLight light = make_light(f, &sun);
    NphRenderView v;
    shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, 0U);
    v.lod_error = 1.0F;

    GroveScene s;
    grove_scene_build(f, &g, &s, true, 1024.0F, 1.0e6F);
    const NphRenderFrameStats coarse = first_frame(f, &v);
    grove_scene_free(f, &s);
    grove_scene_build(f, &g, &s, true, 1.0F / 1024.0F, 0.01F);
    const NphRenderFrameStats fine = first_frame(f, &v);
    grove_scene_free(f, &s);
    NTEST_LOG("a cell coarser than every level: %u caster draws, %u of voxel clusters; %u cells "
              "drawn as their proxies",
              coarse.vsm_draws, coarse.vsm_voxel_draws, coarse.vis_cells_proxied);
    NTEST_LOG("finer than every level: %u caster draws, %u of voxel clusters; %u cells drawn as "
              "their proxies (for %u instances)",
              fine.vsm_draws, fine.vsm_voxel_draws, fine.vis_cells_proxied,
              fine.vis_instances_proxied);
    NTEST_EXPECT_TRUE(coarse.vsm && fine.vsm);
    NTEST_EXPECT_EQ_UINT(coarse.vsm_voxel_draws, 0U);
    NTEST_EXPECT_EQ_UINT(coarse.vis_cells_proxied, 0U);
    NTEST_EXPECT_GT_UINT(fine.vsm_voxel_draws, 0U);
    NTEST_EXPECT_EQ_UINT(fine.vis_cells_proxied, 1U);
    NTEST_EXPECT_EQ_UINT(fine.vis_instances_proxied, GROVE_TREES);
    // The trees' triangles leave the casters with them: the floor's and the
    // occluder's stay.
    NTEST_EXPECT_LT_UINT(fine.vsm_draws, coarse.vsm_draws);
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, floor.floor);
    nph_render_instance_destroy(f->renderer, floor.occluder);
    nph_render_material_destroy(f->renderer, floor.grey);
    nph_render_mesh_destroy(f->renderer, floor.cube);
    nph_render_mesh_destroy(f->renderer, floor.plane);
    grove_free(&g);
}

// ---------------------------------------------------------------------------
// Levels apart (ADR-0063 "Stage 2, a level is a mesh")
// ---------------------------------------------------------------------------

// The levels apart of a proxy hold its levels, record for record
// (test_levels_apart.h levels_check_all): each level's clusters, their
// makers as records and their consumers renumbered, its groups, a tree from
// node 0 the renderer takes, and nothing past the levels; a level the mesh
// does not have, or a mesh of triangles, is refused.
NTEST(cells, a_proxys_levels_apart_hold_its_levels)
{
    Grove g;
    grove_build(&g);
    NTEST_ASSERT_GE_UINT(g.proxy.levels, 3U);
    levels_check_all(&g.proxy);
    NphRenderClusterLods none;
    nph_render_cluster_lods_init(&none, nph_allocator_system());
    NphRenderLodMakerArray makers;
    NPH_ARRAY_INIT(&makers, nph_allocator_system());
    NTEST_EXPECT_EQ_INT(nph_render_cluster_lods_level(&g.proxy, g.proxy.levels, &none, &makers),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_cluster_lods_level(&g.tree.lods, 0U, &none, &makers),
                        NPH_RESULT_INVALID_ARGUMENT);
    NPH_ARRAY_FREE(&makers);
    nph_render_cluster_lods_free(&none);
    grove_free(&g);
}

// The levels of cell `cell`'s proxy whose clusters the last frame drew, a
// bit each: its instances (a level's each, its levels apart) in the visible
// list.
static u32 levels_drawn(RenderFixture *f, NphRenderCell cell)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const NphRenderer *r = f->renderer;
    NphVisCounters c;
    test_read_buffer(f->dev, r->vis.counters, 0U, sizeof c, &c);
    NTEST_ASSERT_EQ_UINT(c.overflow, 0U);
    const RenderCell *rc = &r->cells[render_slot_of(&r->cell_pool, cell.value)];
    u32 slots[NPH_RENDER_CELL_MAX_LEVELS];
    for (u32 k = 0; k < rc->proxy_count; ++k) {
        slots[k] = render_slot_of(&r->instance_pool, rc->proxy[k].value);
    }
    u32 drawn = 0U;
    const u32 n = nph_min_u32(c.clusters, r->desc.max_visible_clusters);
    if (n == 0U) {
        return 0U;
    }
    NphVisCluster *list = NPH_ALLOC_ARRAY(nph_allocator_system(), NphVisCluster, n);
    NTEST_ASSERT_NOT_NULL(list);
    test_read_buffer(f->dev, r->vis.clusters, 0U, (u64)n * sizeof *list, list);
    for (u32 i = 0; i < n; ++i) {
        for (u32 k = 0; k < rc->proxy_count; ++k) {
            drawn |= list[i].instance == slots[k] ? 1U << k : 0U;
        }
    }
    NPH_FREE_ARRAY(nph_allocator_system(), NphVisCluster, list, n);
    return drawn;
}

// A cell whose proxy is its levels apart, a mesh each, draws as the cell
// whose proxy is the whole: the same normals, bit for bit, the same voxel
// clusters and bricks, from near enough for level 0 to far enough for the
// coarsest levels alone, and counted as one cell; and the sun's shadow map
// draws the same voxel clusters into its pages.
NTEST_F(Cells, a_proxys_levels_apart_draw_it_as_the_whole)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    const NphRenderLight light = grove_light(f);
    BigTarget t;
    big_target_init(f, &t);
    u8 *whole = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, BIG_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(whole);
    // Finer than the proxy's own cell, so that it is drawn from near too.
    const f32 error = GROVE_CELL / 64.0F;
    // At a pixel's threshold the grove's level 0 gives way to level 1 from
    // 427 m and level 1 to level 2 from 853 m (the consumers' errors, its
    // cells of 0.25 and 0.5 m); from 409 and 817 m along the view the grove's
    // near and far sides straddle those, so that a cut takes two levels.
    typedef struct GroveSight {
        f64 distance;
        f32 threshold; // pixels
    } GroveSight;
    const GroveSight sights[] = {{20.0, 1.0F},   {80.0, 1.0F},  {300.0, 1.0F}, {400.0, 1.0F},
                                 {1200.0, 1.0F}, {6.0, 40.0F},  {6.0, 60.0F},  {6.0, 85.0F},
                                 {6.0, 120.0F},  {6.0, 170.0F}, {6.0, 240.0F}};
    u32 levels_seen = 0U;
    u32 mixed = 0U; // views whose cut takes two levels or more
    for (u32 i = 0; i < NPH_ARRAY_COUNT(sights); ++i) {
        // The normals: what the voxels hold, without the radiance cache's
        // noise, which differs from frame to frame.
        NphRenderView v = grove_view(sights[i].distance, sights[i].threshold);
        v.debug_view = NPH_RENDER_VIEW_NORMAL;
        GroveScene s;
        grove_scene_build_with(f, &g, &s, true, error, 1.0F, NULL, 0U);
        const NphRenderFrameStats a = big_frames(f, &t, &v);
        memcpy(whole, t.pixels, BIG_PIXELS * 4U);
        grove_scene_free(f, &s);
        grove_scene_build_with(f, &g, &s, true, error, 1.0F, &gl, 0U);
        const NphRenderFrameStats b = big_frames(f, &t, &v);
        const u32 drawn = levels_drawn(f, s.cell);
        mixed += nph_popcount_u32(drawn) >= 2U ? 1U : 0U;
        u32 differ = 0U;
        for (u64 p = 0; p < BIG_PIXELS; ++p) {
            differ += memcmp(whole + p * 4U, t.pixels + p * 4U, 4U) != 0 ? 1U : 0U;
        }
        NTEST_LOG("from %.0f m at %.0f px: the whole %u voxel clusters, %u bricks, %u pixels; its "
                  "levels apart (levels %#x drawn) %u, %u, %u; %u pixels differ",
                  sights[i].distance, (f64)sights[i].threshold, a.vis_voxel_clusters,
                  a.vis_voxel_bricks, covered_pixels(whole, BIG_PIXELS), drawn,
                  b.vis_voxel_clusters, b.vis_voxel_bricks, covered_pixels(t.pixels, BIG_PIXELS),
                  differ);
        NTEST_EXPECT_GT_UINT(a.vis_voxel_clusters, 0U);
        NTEST_EXPECT_EQ_UINT(b.vis_voxel_clusters, a.vis_voxel_clusters);
        NTEST_EXPECT_EQ_UINT(b.vis_voxel_bricks, a.vis_voxel_bricks);
        NTEST_EXPECT_EQ_UINT(a.vis_cells_proxied, 1U);
        NTEST_EXPECT_EQ_UINT(b.vis_cells_proxied, 1U);
        NTEST_EXPECT_EQ_UINT(differ, 0U);
        levels_seen += a.vis_voxel_clusters != 0U ? 1U : 0U;
        grove_scene_free(f, &s);
    }
    NTEST_EXPECT_EQ_UINT(levels_seen, NPH_ARRAY_COUNT(sights));
    NTEST_EXPECT_GE_UINT(mixed, 2U);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, whole, BIG_PIXELS * 4U);
    big_target_free(f, &t);
    nph_render_light_destroy(f->renderer, light);

    if (nph_renderer_virtual_shadows_supported(f->renderer)) {
        Scene floor;
        scene_build(f, &floor, at(0.0, -5.0, 0.0, 0.01F));
        NphRenderLightDesc sun = sun_desc(true);
        const NphRenderLight sl = make_light(f, &sun);
        NphRenderView v;
        shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, 0U);
        v.lod_error = 1.0F;
        GroveScene s;
        grove_scene_build_with(f, &g, &s, true, 1.0F / 1024.0F, 0.01F, NULL, 0U);
        const NphRenderFrameStats a = first_frame(f, &v);
        grove_scene_free(f, &s);
        grove_scene_build_with(f, &g, &s, true, 1.0F / 1024.0F, 0.01F, &gl, 0U);
        const NphRenderFrameStats b = first_frame(f, &v);
        grove_scene_free(f, &s);
        NTEST_LOG("the shadow map: the whole %u draws of voxel clusters, its levels apart %u",
                  a.vsm_voxel_draws, b.vsm_voxel_draws);
        NTEST_EXPECT_GT_UINT(a.vsm_voxel_draws, 0U);
        NTEST_EXPECT_EQ_UINT(b.vsm_voxel_draws, a.vsm_voxel_draws);
        nph_render_light_destroy(f->renderer, sl);
        nph_render_instance_destroy(f->renderer, floor.floor);
        nph_render_instance_destroy(f->renderer, floor.occluder);
        nph_render_material_destroy(f->renderer, floor.grey);
        nph_render_mesh_destroy(f->renderer, floor.cube);
        nph_render_mesh_destroy(f->renderer, floor.plane);
    }
    grove_levels_free(&gl);
    grove_free(&g);
}

// A cell whose proxy's finer levels it does not hold (a streamed proxy's
// before they arrive: ADR-0063 "The cut") draws its finest held level
// wherever a finer one would be: that level's instance takes its makers as
// fine (NPH_VIS_INSTANCE_FINEST), so the near field shows no hole. From 20 m
// at a pixel's threshold the whole draws level 0 alone; held from level 1 or
// 2 on, the cell draws that level alone, over about the pixels level 0 does
// (its voxels larger, never fewer), and the shadow map draws its voxels too.
NTEST_F(Cells, a_cells_finest_held_level_stands_for_the_finer_ones)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    NTEST_ASSERT_GE_UINT(gl.count, 3U);
    const NphRenderLight light = grove_light(f);
    BigTarget t;
    big_target_init(f, &t);
    const f32 error = GROVE_CELL / 64.0F;
    NphRenderView v = grove_view(20.0, 1.0F);
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    GroveScene s;
    grove_scene_build_with(f, &g, &s, true, error, 1.0F, &gl, 0U);
    big_frames(f, &t, &v);
    const u32 whole = covered_pixels(t.pixels, BIG_PIXELS);
    NTEST_EXPECT_EQ_UINT(levels_drawn(f, s.cell), 1U);
    grove_scene_free(f, &s);
    NTEST_ASSERT_GT_UINT(whole, 0U);
    for (u32 first = 1U; first < gl.count; ++first) {
        grove_scene_build_with(f, &g, &s, true, error, 1.0F, &gl, first);
        const NphRenderFrameStats st = big_frames(f, &t, &v);
        const u32 covered = covered_pixels(t.pixels, BIG_PIXELS);
        const u32 drawn = levels_drawn(f, s.cell);
        NTEST_LOG("held from level %u: %u voxel clusters, %u pixels (level 0's %u), levels %#x of "
                  "those held drawn",
                  first, st.vis_voxel_clusters, covered, whole, drawn);
        NTEST_EXPECT_GT_UINT(st.vis_voxel_clusters, 0U);
        NTEST_EXPECT_EQ_UINT(drawn, 1U);
        NTEST_EXPECT_GE_UINT(covered * 10U, whole * 9U);
        grove_scene_free(f, &s);
    }
    big_target_free(f, &t);
    nph_render_light_destroy(f->renderer, light);

    if (nph_renderer_virtual_shadows_supported(f->renderer)) {
        Scene floor;
        scene_build(f, &floor, at(0.0, -5.0, 0.0, 0.01F));
        NphRenderLightDesc sun = sun_desc(true);
        const NphRenderLight sl = make_light(f, &sun);
        NphRenderView sv;
        shadow_view(&sv, top_camera(), 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, 0U);
        sv.lod_error = 1.0F;
        grove_scene_build_with(f, &g, &s, true, 1.0F / 1024.0F, 0.01F, &gl, 0U);
        const NphRenderFrameStats all = first_frame(f, &sv);
        grove_scene_free(f, &s);
        for (u32 first = 1U; first < gl.count; ++first) {
            grove_scene_build_with(f, &g, &s, true, 1.0F / 1024.0F, 0.01F, &gl, first);
            const NphRenderFrameStats st = first_frame(f, &sv);
            grove_scene_free(f, &s);
            NTEST_LOG("the shadow map, held from level %u: %u draws of voxel clusters (all levels "
                      "held: %u)",
                      first, st.vsm_voxel_draws, all.vsm_voxel_draws);
            NTEST_EXPECT_GT_UINT(st.vsm_voxel_draws, 0U);
        }
        nph_render_light_destroy(f->renderer, sl);
        nph_render_instance_destroy(f->renderer, floor.floor);
        nph_render_instance_destroy(f->renderer, floor.occluder);
        nph_render_material_destroy(f->renderer, floor.grey);
        nph_render_mesh_destroy(f->renderer, floor.cube);
        nph_render_mesh_destroy(f->renderer, floor.plane);
    }
    grove_levels_free(&gl);
    grove_free(&g);
}

// A cell of a streamed proxy asks for the level its cut needs (ADR-0063
// stage 3): from each distance, the first level whose largest consumer
// error is too coarse at the box's nearest point brought a quarter nearer,
// as its C twin here finds it; nothing nearer than its distance less a
// quarter, nor with the cell out of view, nor in a view without levels of
// detail. Drawn or not: from 30 m on the cell is drawn as its proxy, from
// 22.5 m it asks.
NTEST_F(Cells, a_cell_asks_for_the_level_its_cut_needs)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    BigTarget t;
    big_target_init(f, &t);
    const f32 distance = 30.0F;
    GroveScene s;
    grove_scene_build_with(f, &g, &s, true, GROVE_CELL / 64.0F, distance, &gl, 0U);
    NTEST_LOG("the levels' errors: %.3f %.3f %.3g", (f64)s.level_errors[0], (f64)s.level_errors[1],
              (f64)s.level_errors[2]);
    // The renderer's pixels a unit at distance one spans (vis.c), at a
    // pixel's threshold.
    const f32 per_unit = 0.5F * (f32)BIG_SIZE / nph_f32_tan(0.3F);
    const f64 distances[] = {10.0, 20.0, 25.0, 40.0, 80.0, 160.0, 320.0, 640.0, 1280.0};
    u32 asked = 0U;
    for (u32 i = 0; i < NPH_ARRAY_COUNT(distances); ++i) {
        const NphRenderView v = grove_view(distances[i], 1.0F);
        big_frames(f, &t, &v);
        // The box's nearest point from the camera (grove_view), as the GPU
        // finds it, and the level the twin asks for.
        const NphVec3 cam = nph_vec3(0.0F, (f32)(0.3 * distances[i]), (f32)distances[i]);
        const NphVec3 gap = nph_vec3(nph_f32_max(nph_f32_abs(cam.x) - 2.5F, 0.0F),
                                     nph_f32_max(nph_f32_abs(cam.y) - 1.0F, 0.0F),
                                     nph_f32_max(nph_f32_abs(cam.z) - 2.5F, 0.0F));
        const f32 d = nph_vec3_length(gap);
        u32 expected = NPH_RENDER_CELL_NO_REQUEST;
        if (d >= distance * 0.75F) {
            const f32 reach = nph_f32_max(d * 0.75F, 0.05F);
            for (u32 k = 0; k < s.level_count && expected == NPH_RENDER_CELL_NO_REQUEST; ++k) {
                expected = s.level_errors[k] * per_unit > reach ? k : expected;
            }
        }
        const u32 got = nph_render_cell_request(f->renderer, s.cell);
        NTEST_LOG("from %.0f m (the box %.1f m away): asks for level %u, the twin %u", distances[i],
                  (f64)d, got, expected);
        NTEST_EXPECT_EQ_UINT(got, expected);
        NTEST_EXPECT_NE_UINT(nph_render_cell_requests_frame(f->renderer), NPH_U64_MAX);
        asked |= got != NPH_RENDER_CELL_NO_REQUEST ? 1U << got : 0U;
    }
    // Every level asked for from some distance.
    NTEST_EXPECT_EQ_UINT(asked, (1U << s.level_count) - 1U);
    // Out of view (the camera turned away), or without levels of detail:
    // nothing.
    NphRenderView away = grove_view(80.0, 1.0F);
    away.camera = nph_render_camera_look_at(nph_dvec3(0.0, 24.0, 80.0), nph_dvec3(0.0, 24.0, 160.0),
                                            nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F);
    big_frames(f, &t, &away);
    NTEST_EXPECT_EQ_UINT(nph_render_cell_request(f->renderer, s.cell), NPH_RENDER_CELL_NO_REQUEST);
    const NphRenderView flat = grove_view(80.0, 0.0F);
    big_frames(f, &t, &flat);
    NTEST_EXPECT_EQ_UINT(nph_render_cell_request(f->renderer, s.cell), NPH_RENDER_CELL_NO_REQUEST);
    // A stale cell asks for nothing.
    grove_scene_free(f, &s);
    NTEST_EXPECT_EQ_UINT(nph_render_cell_request(f->renderer, s.cell), NPH_RENDER_CELL_NO_REQUEST);
    big_target_free(f, &t);
    grove_levels_free(&gl);
    grove_free(&g);
}

// The sun's shadow map asks for a cell's level every frame, its pages drawn
// or kept (ADR-0063 stage 3): with the camera near enough that its view asks
// nothing (the cell's instances are drawn there), the request the map makes
// while it draws its pages stands once it keeps them, frame after frame. (A
// request made only where pages are drawn would come and go with them, and
// a stream giving up the level for want of one would redraw the pages that
// ask for it again.)
NTEST_F(Cells, the_shadow_map_asks_for_a_cells_level_its_pages_drawn_or_kept)
{
    REQUIRE_PRODUCTION(f);
    if (!nph_renderer_virtual_shadows_supported(f->renderer)) {
        NTEST_SKIP("no virtual shadow map on this device");
    }
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    Scene floor;
    scene_build(f, &floor, at(0.0, -5.0, 0.0, 0.01F));
    NphRenderLightDesc sun = sun_desc(true);
    const NphRenderLight light = make_light(f, &sun);
    NphRenderView v;
    shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, 0U);
    v.lod_error = 1.0F;
    // Drawn as its proxy from a million metres (the camera never), into the
    // map's levels where its finest error fits.
    GroveScene s;
    grove_scene_build_with(f, &g, &s, true, 1.0F / 1024.0F, 1.0e6F, &gl, 0U);
    u32 asked = NPH_RENDER_CELL_NO_REQUEST;
    u32 kept = 0U; // frames that drew no page
    for (u32 i = 0; i < 12U; ++i) {
        const NphRenderFrameStats st = test_render_read(f, &v);
        const u32 request = nph_render_cell_request(f->renderer, s.cell);
        NTEST_LOG("frame %u: %u pages drawn, %u resident; the cell asks for level %u", i,
                  st.vsm_drawn, st.vsm_resident, request);
        // The statistics and the requests both come NPH_GPU_FRAMES_IN_FLIGHT
        // frames late: from the first frame read back on, they match.
        if (i < NPH_GPU_FRAMES_IN_FLIGHT) {
            continue;
        }
        NTEST_EXPECT_NE_UINT(request, NPH_RENDER_CELL_NO_REQUEST);
        asked = asked == NPH_RENDER_CELL_NO_REQUEST ? request : asked;
        NTEST_EXPECT_EQ_UINT(request, asked);
        kept += st.vsm_drawn == 0U ? 1U : 0U;
    }
    NTEST_EXPECT_GT_UINT(kept, 0U);
    // Its proxy drawn into no level (finer than its finest error everywhere):
    // nothing.
    grove_scene_free(f, &s);
    grove_scene_build_with(f, &g, &s, true, 1024.0F, 1.0e6F, &gl, 0U);
    first_frame(f, &v);
    NTEST_EXPECT_EQ_UINT(nph_render_cell_request(f->renderer, s.cell), NPH_RENDER_CELL_NO_REQUEST);
    grove_scene_free(f, &s);
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, floor.floor);
    nph_render_instance_destroy(f->renderer, floor.occluder);
    nph_render_material_destroy(f->renderer, floor.grey);
    nph_render_mesh_destroy(f->renderer, floor.cube);
    nph_render_mesh_destroy(f->renderer, floor.plane);
    grove_levels_free(&gl);
    grove_free(&g);
}

// Frames until the sun's shadow map draws no page for a few frames running
// (its statistics come NPH_GPU_FRAMES_IN_FLIGHT frames late), at most 64:
// the pages drawn on the way.
static u32 map_settle(RenderFixture *f, const NphRenderView *v)
{
    u32 drawn = 0U;
    u32 quiet = 0U;
    for (u32 i = 0; i < 64U && quiet <= NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        const NphRenderFrameStats st = test_render_read(f, v);
        drawn += st.vsm_drawn;
        quiet = st.vsm_drawn == 0U ? quiet + 1U : 0U;
    }
    NTEST_ASSERT_GT_UINT(quiet, NPH_GPU_FRAMES_IN_FLIGHT);
    return drawn;
}

// The bound a cell's proxy instances' pending updates hold on the shadow
// map levels they dirty (NphVisInstanceUpdate.vsm_below; 0: every level),
// each checked to be `expected`.
static void expect_proxy_bound(RenderFixture *f, NphRenderCell cell, f32 expected)
{
    const RenderVis *vis = &f->renderer->vis;
    const RenderCell *c = &f->renderer->cells[render_slot_of(&f->renderer->cell_pool, cell.value)];
    for (u32 k = 0; k < c->proxy_count; ++k) {
        const u32 slot = render_slot_of(&f->renderer->instance_pool, c->proxy[k].value);
        const bool all = (vis->slot_state[slot] & (u8)RENDER_SLOT_ALL_LEVELS) != 0U;
        NTEST_EXPECT_NEAR_F32(all ? 0.0F : vis->vsm_below[slot], expected, 0.0F);
    }
}

// A cell's proxy's level coming or going dirties the shadow map's pages only
// in the map levels whose cut it changes (ADR-0063 stage 4): those whose
// threshold is below the makers' error of the coarser of the two finest
// levels held. The map then shows what it shows when drawn afresh with the
// levels held, and draws fewer pages than when the change dirties every
// level, as every change did. A change of another kind (another mesh) dirties
// every level.
NTEST_F(Cells, a_proxys_level_coming_dirties_only_the_map_levels_it_changes)
{
    REQUIRE_PRODUCTION(f);
    if (!nph_renderer_virtual_shadows_supported(f->renderer)) {
        NTEST_SKIP("no virtual shadow map on this device");
    }
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    NTEST_ASSERT_EQ_UINT(gl.count, 3U);
    Scene floor;
    scene_build(f, &floor, at(0.0, -5.0, 0.0, 0.01F));
    NphRenderLightDesc sun = sun_desc(true);
    const NphRenderLight light = make_light(f, &sun);
    static u8 fresh[PIXELS];
    const f64 distances[2] = {6.0, 60.0};
    for (u32 d = 0; d < 2U; ++d) {
        NphRenderView v;
        const NphRenderCamera cam = nph_render_camera_look_at(
            nph_dvec3(0.0, distances[d], 0.5 * distances[d]), nph_dvec3(0.0, 0.0, 0.0),
            nph_vec3(0.0F, 1.0F, 0.0F), 0.9F, 0.05F);
        shadow_view(&v, cam, 13.0F, NPH_RENDER_PIPELINE_PRODUCTION, 0U);
        v.lod_error = 1.0F;
        // The cell made with levels 1 and 2, drawn into every map level as
        // its proxy (its finest error fits everywhere) and never by the
        // camera: the map drawn afresh.
        GroveScene s;
        grove_scene_build_with(f, &g, &s, true, 1.0F / 1024.0F, 1.0e6F, &gl, 1U);
        (void)map_settle(f, &v);
        memcpy(fresh, f->pixels, PIXELS);
        grove_scene_free(f, &s);
        // Made with level 2 alone, then given level 1: its updates dirty
        // the map below level 2's makers' error (level 1's consumers').
        grove_scene_build_with(f, &g, &s, true, 1.0F / 1024.0F, 1.0e6F, &gl, 2U);
        (void)map_settle(f, &v);
        NTEST_ASSERT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, s.levels + 1, 2U),
                            NPH_RESULT_OK);
        expect_proxy_bound(f, s.cell, s.level_errors[1]);
        const u32 bounded = map_settle(f, &v);
        NTEST_EXPECT_LE_UINT(image_max_diff(f->pixels, fresh), SAME_TOL);
        // Given up again (the same bound), then given back dirtying every
        // level, as before.
        NTEST_ASSERT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, s.levels + 2, 1U),
                            NPH_RESULT_OK);
        expect_proxy_bound(f, s.cell, s.level_errors[1]);
        (void)map_settle(f, &v);
        NTEST_ASSERT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, s.levels + 1, 2U),
                            NPH_RESULT_OK);
        const RenderCell *c =
            &f->renderer->cells[render_slot_of(&f->renderer->cell_pool, s.cell.value)];
        for (u32 k = 0; k < c->proxy_count; ++k) {
            const u32 slot = render_slot_of(&f->renderer->instance_pool, c->proxy[k].value);
            f->renderer->vis.slot_state[slot] |= (u8)RENDER_SLOT_ALL_LEVELS;
        }
        const u32 every = map_settle(f, &v);
        NTEST_EXPECT_LE_UINT(image_max_diff(f->pixels, fresh), SAME_TOL);
        NTEST_LOG("camera %.0f m up: the levels' errors %.4f %.4f %.4g; level 1's coming drew "
                  "%u pages, %u dirtying every level",
                  distances[d], (f64)s.level_errors[0], (f64)s.level_errors[1],
                  (f64)s.level_errors[2], bounded, every);
        // Near, the map levels its pixels read take level 1 now: their pages
        // are drawn again. From afar they take level 2 as before: none.
        if (d == 0U) {
            NTEST_EXPECT_GT_UINT(bounded, 0U);
            NTEST_EXPECT_LE_UINT(bounded, every);
        } else {
            NTEST_EXPECT_EQ_UINT(bounded, 0U);
            NTEST_EXPECT_GT_UINT(every, 0U);
        }
        // Another mesh in the finest's place: every level.
        NTEST_ASSERT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, &s.proxy, 1U),
                            NPH_RESULT_OK);
        expect_proxy_bound(f, s.cell, 0.0F);
        (void)map_settle(f, &v);
        grove_scene_free(f, &s);
    }
    nph_render_light_destroy(f->renderer, light);
    nph_render_instance_destroy(f->renderer, floor.floor);
    nph_render_instance_destroy(f->renderer, floor.occluder);
    nph_render_material_destroy(f->renderer, floor.grey);
    nph_render_mesh_destroy(f->renderer, floor.cube);
    nph_render_mesh_destroy(f->renderer, floor.plane);
    grove_levels_free(&gl);
    grove_free(&g);
}

// A cell's proxy is replaced level by level, as a streamed proxy's arrive and
// go (ADR-0063 stage 3): held from level 2, then 1, then 0, then from 2
// again, the cell draws its finest held level near (where the whole draws
// level 0), the whole's cut once it holds them all, the same pixels as a cell
// made with them; its instances follow (made, kept, destroyed: nothing
// leaks), counted once. A stale cell, no meshes, a mesh of triangles or one
// given twice are refused, and the cell keeps its proxy.
NTEST_F(Cells, a_cells_proxy_is_replaced_level_by_level)
{
    REQUIRE_PRODUCTION(f);
    Grove g;
    grove_build(&g);
    GroveLevels gl;
    grove_levels_build(&g, &gl);
    NTEST_ASSERT_EQ_UINT(gl.count, 3U);
    BigTarget t;
    big_target_init(f, &t);
    u8 *whole = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, BIG_PIXELS * 4U);
    NTEST_ASSERT_NOT_NULL(whole);
    const f32 error = GROVE_CELL / 64.0F;
    NphRenderView v = grove_view(20.0, 1.0F);
    v.debug_view = NPH_RENDER_VIEW_NORMAL;
    // The cell made with every level: the image to come back to.
    GroveScene s;
    grove_scene_build_with(f, &g, &s, true, error, 1.0F, &gl, 0U);
    big_frames(f, &t, &v);
    memcpy(whole, t.pixels, BIG_PIXELS * 4U);
    grove_scene_free(f, &s);

    // Made with its coarsest level alone, then given the finer ones.
    grove_scene_build_with(f, &g, &s, true, error, 1.0F, &gl, 2U);
    big_frames(f, &t, &v);
    NTEST_EXPECT_EQ_UINT(levels_drawn(f, s.cell), 1U);
    const u32 instances = nph_handle_pool_live(&f->renderer->instance_pool);
    for (u32 first = 2U; first-- > 0U;) {
        NTEST_ASSERT_EQ_INT(
            nph_render_cell_set_proxy(f->renderer, s.cell, s.levels + first, gl.count - first),
            NPH_RESULT_OK);
        const NphRenderFrameStats st = big_frames(f, &t, &v);
        const u32 drawn = levels_drawn(f, s.cell);
        NTEST_LOG("held from level %u: levels %#x of those held drawn, %u voxel clusters, %u "
                  "pixels, %u cells counted",
                  first, drawn, st.vis_voxel_clusters, covered_pixels(t.pixels, BIG_PIXELS),
                  st.vis_cells_proxied);
        NTEST_EXPECT_EQ_UINT(drawn, 1U); // its finest held level
        NTEST_EXPECT_EQ_UINT(st.vis_cells_proxied, 1U);
        NTEST_EXPECT_EQ_UINT(nph_handle_pool_live(&f->renderer->instance_pool),
                             instances + (2U - first));
    }
    // Holding them all, the whole's image.
    u32 differ = 0U;
    for (u64 p = 0; p < BIG_PIXELS; ++p) {
        differ += memcmp(whole + p * 4U, t.pixels + p * 4U, 4U) != 0 ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(differ, 0U);
    // Back to the coarsest alone: the finer levels' instances go.
    NTEST_ASSERT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, s.levels + 2U, 1U),
                        NPH_RESULT_OK);
    big_frames(f, &t, &v);
    NTEST_EXPECT_EQ_UINT(levels_drawn(f, s.cell), 1U);
    NTEST_EXPECT_EQ_UINT(nph_handle_pool_live(&f->renderer->instance_pool), instances);
    // Refused, the proxy kept.
    const NphRenderMesh twice[2] = {s.levels[1], s.levels[1]};
    NTEST_EXPECT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, twice, 2U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, &s.tree, 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_cell_set_proxy(f->renderer, s.cell, s.levels, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_cell_set_proxy(f->renderer, NPH_RENDER_CELL_NULL, s.levels, 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(nph_handle_pool_live(&f->renderer->instance_pool), instances);
    big_frames(f, &t, &v);
    NTEST_EXPECT_EQ_UINT(levels_drawn(f, s.cell), 1U);
    grove_scene_free(f, &s);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, whole, BIG_PIXELS * 4U);
    big_target_free(f, &t);
    grove_levels_free(&gl);
    grove_free(&g);
}
