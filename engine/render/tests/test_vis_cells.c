// SPDX-License-Identifier: Apache-2.0
// The world partition's cells in the renderer (render/renderer.h
// nph_render_cell_create; docs/render.md "Cell proxies"; ADR-0062): a mesh of
// voxels alone (a cell's proxy) made, drawn by the production path and
// passed by the reference renderer and the rays, and refused malformed; a
// cell made and refused; its proxy drawn in place of its instances from
// afar, standing for them, and not in views without levels of detail; and
// the sun's shadow map drawing the proxy or the instances per level. The
// fixture's teardown asserts zero validation and synchronisation-validation
// errors and nothing leaked.
#include <nepheline/math/random.h>
#include <nepheline/render/cell_proxy.h>
#include <nepheline/render/cluster_lod.h>

#include "test_lod_support.h"
#include "test_shadow_support.h"

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
    NphRenderPackedVoxels proxy_packed; // its voxels compact, as the cooker stores them
    NphRenderSubmesh proxy_sub;
    NphRenderMeshDesc proxy_desc; // a mesh of voxels alone
} Grove;

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

    // Its record as a cooked one brings it (asset/mesh.h version 4): no
    // vertices, one submesh naming material slot 0, every cluster a voxel
    // cluster, the bounds its clusters' spheres'.
    NphRenderMeshDesc *pd = &g->proxy_desc;
    memset(pd, 0, sizeof *pd);
    memset(&g->proxy_sub, 0, sizeof g->proxy_sub);
    pd->submeshes = &g->proxy_sub;
    pd->submesh_count = 1U;
    pd->position_scale = nph_vec3_one();
    const NphRenderMeshlets *c = &g->proxy.clusters;
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
    pd->cluster_lods = g->proxy.lods.data;
    pd->lod_groups = g->proxy.groups.data;
    pd->lod_group_count = g->proxy.groups.len;
    pd->lod_group_clusters = g->proxy.group_clusters.data;
    pd->lod_group_cluster_count = g->proxy.group_clusters.len;
    pd->lod_nodes = g->proxy.nodes.data;
    pd->lod_node_count = g->proxy.nodes.len;
    pd->lod_level0_meshlets = g->proxy.level0_clusters;
    nph_render_packed_voxels_init(&g->proxy_packed, a);
    NphRenderMeshlets *w = &g->proxy.clusters;
    NTEST_ASSERT_EQ_INT(nph_render_voxels_pack(w->meshlets.data, w->meshlets.len,
                                               g->proxy.bricks.data, g->proxy.bricks.len,
                                               g->proxy.voxels.data, g->proxy.voxels.len,
                                               &g->proxy_packed),
                        NPH_RESULT_OK);
    pd->voxel_clusters = g->proxy_packed.clusters.data;
    pd->voxel_cluster_count = g->proxy_packed.clusters.len;
    pd->voxel_bricks = g->proxy_packed.bricks.data;
    pd->voxel_brick_count = g->proxy_packed.bricks.len;
    pd->voxel_words = g->proxy_packed.words.data;
    pd->voxel_word_count = g->proxy_packed.words.len;
    pd->name = "grove proxy";
}

static void grove_free(Grove *g)
{
    nph_render_packed_voxels_free(&g->proxy_packed);
    nph_render_cluster_lods_free(&g->proxy);
    NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, g->positions, g->tree.data.vertex_count);
    lod_mesh_free(&g->tree);
}

// The grove in the renderer: the trees' mesh, the proxy's, and a cell of
// `error` and `distance` (the proxy's own finest cell and distance, or a
// test's) whose proxy stands for the trees, which name it (with `cells`).
typedef struct GroveScene {
    NphRenderMesh tree;
    NphRenderMesh proxy;
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
    d.proxy = s->proxy;
    d.proxy_materials = &s->green;
    d.proxy_material_count = 1U;
    d.proxy_transform = nph_render_transform_identity();
    d.proxy_error = error;
    d.distance = distance;
    return d;
}

static void grove_scene_build(RenderFixture *f, const Grove *g, GroveScene *s, bool cells,
                              f32 error, f32 distance)
{
    memset(s, 0, sizeof *s);
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g->tree.desc, &s->tree),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, &g->proxy_desc, &s->proxy),
                        NPH_RESULT_OK);
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

static void grove_scene_free(RenderFixture *f, GroveScene *s)
{
    for (u32 i = 0; i < GROVE_TREES; ++i) {
        nph_render_instance_destroy(f->renderer, s->trees[i]);
    }
    nph_render_cell_destroy(f->renderer, s->cell);
    nph_render_material_destroy(f->renderer, s->green);
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
    // A proxy of triangles, or none.
    d.proxy = s.tree;
    EXPECT_REFUSED();
    d.proxy = NPH_RENDER_MESH_NULL;
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
