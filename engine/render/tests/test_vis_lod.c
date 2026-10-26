// SPDX-License-Identifier: Apache-2.0
// The production path's levels of detail (render/cluster_lod.h;
// docs/render.md "Levels of detail"): meshes with the levels
// nph_render_cluster_lods_build makes, drawn through the hierarchy's steps
// (vis_cull_nodes.comp) and the meshlet cull's cut. The clusters drawn are
// checked against the cut the CPU computes for the same view; a threshold of
// zero against the mesh without levels; a cut that mixes levels for holes;
// an instance revealed in phase 1 for its whole cut; and the queues' overflow
// and the validation of broken levels. The fixture's teardown asserts zero
// validation and synchronisation-validation errors and nothing leaked.
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>

#include "render_internal.h"
#include "test_render_support.h"

typedef RenderFixture VisLod;

NTEST_SETUP(VisLod)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(VisLod)
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
// A mesh with levels
// ---------------------------------------------------------------------------

// A mesh with its levels of detail as a cooked mesh brings them: `desc`
// names every level's clusters and the hierarchy.
typedef struct LodMesh {
    NphRenderMeshData data;
    NphRenderMeshlets meshlets; // level 0
    NphRenderClusterLods lods;
    NphRenderSubmesh sub;
    NphRenderMeshDesc desc;
} LodMesh;

static void lod_mesh_build(LodMesh *m, const NphRenderVertexInput *v, u32 n, const u32 *idx, u32 k)
{
    NphAllocator *a = nph_allocator_system();
    memset(m, 0, sizeof *m);
    NTEST_ASSERT_EQ_INT(nph_render_mesh_data_build(a, v, n, idx, k, &m->data), NPH_RESULT_OK);

    // The positions the GPU sees, for the meshlets and the levels.
    NphVec3 *positions = NPH_ALLOC_ARRAY(a, NphVec3, m->data.vertex_count);
    NTEST_ASSERT_NOT_NULL(positions);
    const f32 q = 1.0F / 65535.0F;
    for (u32 i = 0; i < m->data.vertex_count; ++i) {
        const NphRenderVertexPosition p = m->data.positions[i];
        positions[i] =
            nph_vec3(m->data.position_offset.x + (f32)p.x * q * m->data.position_scale.x,
                     m->data.position_offset.y + (f32)p.y * q * m->data.position_scale.y,
                     m->data.position_offset.z + (f32)p.z * q * m->data.position_scale.z);
    }
    const NphRenderIndexRange range = {0U, m->data.index_count};
    NphRenderMeshletInput in;
    memset(&in, 0, sizeof in);
    in.positions = positions;
    in.vertex_count = m->data.vertex_count;
    in.indices = m->data.indices;
    in.index_count = m->data.index_count;
    in.ranges = &range;
    in.range_count = 1U;
    in.cone_weight = 0.25F;
    in.scratch = a;
    nph_render_meshlets_init(&m->meshlets, a);
    NTEST_ASSERT_EQ_INT(nph_render_meshlets_build(&in, &m->meshlets), NPH_RESULT_OK);
    NphRenderClusterLodInput li;
    memset(&li, 0, sizeof li);
    li.positions = positions;
    li.attribs = m->data.attribs;
    li.attrib_stride = (u32)sizeof(NphRenderVertexAttrib);
    li.vertex_count = m->data.vertex_count;
    li.indices = m->data.indices;
    li.index_count = m->data.index_count;
    li.ranges = &range;
    li.range_count = 1U;
    li.meshlets = &m->meshlets;
    li.cone_weight = 0.25F;
    li.scratch = a;
    nph_render_cluster_lods_init(&m->lods, a);
    NTEST_ASSERT_EQ_INT(nph_render_cluster_lods_build(&li, &m->lods), NPH_RESULT_OK);
    NPH_FREE_ARRAY(a, NphVec3, positions, m->data.vertex_count);

    nph_render_mesh_data_desc(&m->data, "lod torus", &m->sub, &m->desc);
    const NphRenderMeshlets *c = &m->lods.clusters;
    m->desc.meshlets = c->meshlets.data;
    m->desc.meshlet_bounds = c->bounds.data;
    m->desc.meshlet_count = c->meshlets.len;
    m->desc.meshlet_vertices = c->vertices.data;
    m->desc.meshlet_vertex_count = c->vertices.len;
    m->desc.meshlet_triangles = c->triangles.data;
    m->desc.meshlet_triangle_count = c->triangles.len / 3U;
    m->desc.cluster_lods = m->lods.lods.data;
    m->desc.lod_groups = m->lods.groups.data;
    m->desc.lod_group_count = m->lods.groups.len;
    m->desc.lod_group_clusters = m->lods.group_clusters.data;
    m->desc.lod_group_cluster_count = m->lods.group_clusters.len;
    m->desc.lod_nodes = m->lods.nodes.data;
    m->desc.lod_node_count = m->lods.nodes.len;
    m->desc.lod_level0_meshlets = m->lods.level0_clusters;
}

// A torus of `rings` x `sides` quads around the y axis, closed and seamless.
static void lod_torus(LodMesh *m, u32 rings, u32 sides, f32 big, f32 small)
{
    NphAllocator *a = nph_allocator_system();
    const u32 n = rings * sides;
    NphRenderVertexInput *v = NPH_ALLOC_ARRAY(a, NphRenderVertexInput, n);
    u32 *idx = NPH_ALLOC_ARRAY(a, u32, (u64)n * 6U);
    NTEST_ASSERT_TRUE(v != NULL && idx != NULL);
    for (u32 i = 0; i < rings; ++i) {
        const f32 ta = 2.0F * NPH_PI * (f32)i / (f32)rings;
        for (u32 j = 0; j < sides; ++j) {
            const f32 tb = 2.0F * NPH_PI * (f32)j / (f32)sides;
            const NphVec3 ring = nph_vec3(big * nph_f32_cos(ta), 0.0F, big * nph_f32_sin(ta));
            const NphVec3 normal = nph_vec3(nph_f32_cos(tb) * nph_f32_cos(ta), nph_f32_sin(tb),
                                            nph_f32_cos(tb) * nph_f32_sin(ta));
            NphRenderVertexInput *p = &v[i * sides + j];
            p->position = nph_vec3_add(ring, nph_vec3_scale(normal, small));
            p->normal = normal;
            p->uv = nph_vec2((f32)i / (f32)rings, (f32)j / (f32)sides);
            p->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
        }
    }
    u32 k = 0U;
    for (u32 i = 0; i < rings; ++i) {
        for (u32 j = 0; j < sides; ++j) {
            const u32 i1 = (i + 1U) % rings;
            const u32 j1 = (j + 1U) % sides;
            const u32 v00 = i * sides + j;
            const u32 v10 = i1 * sides + j;
            const u32 v01 = i * sides + j1;
            const u32 v11 = i1 * sides + j1;
            // Counter-clockwise seen from outside the tube.
            idx[k++] = v00;
            idx[k++] = v01;
            idx[k++] = v11;
            idx[k++] = v00;
            idx[k++] = v11;
            idx[k++] = v10;
        }
    }
    lod_mesh_build(m, v, n, idx, k);
    NPH_FREE_ARRAY(a, NphRenderVertexInput, v, n);
    NPH_FREE_ARRAY(a, u32, idx, (u64)n * 6U);
}

// A sheet of `cells` x `cells` quads across [-half, half] in x and z, its
// height rolling (a terrain's single surface: through a crack in it there
// is nothing but the background).
static f32 terrain_height(f32 x, f32 z)
{
    return 0.18F * nph_f32_sin(1.3F * x) * nph_f32_cos(1.1F * z) +
           0.06F * nph_f32_sin(3.7F * x + 2.9F * z);
}

static void lod_terrain(LodMesh *m, u32 cells, f32 half)
{
    NphAllocator *a = nph_allocator_system();
    const u32 side = cells + 1U;
    const u32 n = side * side;
    NphRenderVertexInput *v = NPH_ALLOC_ARRAY(a, NphRenderVertexInput, n);
    u32 *idx = NPH_ALLOC_ARRAY(a, u32, (u64)cells * cells * 6U);
    NTEST_ASSERT_TRUE(v != NULL && idx != NULL);
    const f32 step = 2.0F * half / (f32)cells;
    for (u32 i = 0; i < side; ++i) {
        for (u32 j = 0; j < side; ++j) {
            const f32 x = -half + step * (f32)j;
            const f32 z = -half + step * (f32)i;
            const f32 e = 0.01F;
            const f32 dx = (terrain_height(x + e, z) - terrain_height(x - e, z)) / (2.0F * e);
            const f32 dz = (terrain_height(x, z + e) - terrain_height(x, z - e)) / (2.0F * e);
            NphRenderVertexInput *p = &v[i * side + j];
            p->position = nph_vec3(x, terrain_height(x, z), z);
            p->normal = nph_vec3_normalize(nph_vec3(-dx, 1.0F, -dz));
            p->uv = nph_vec2((f32)j / (f32)cells, (f32)i / (f32)cells);
            p->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
        }
    }
    u32 k = 0U;
    for (u32 i = 0; i < cells; ++i) {
        for (u32 j = 0; j < cells; ++j) {
            const u32 v00 = i * side + j;
            const u32 v01 = v00 + 1U;
            const u32 v10 = v00 + side;
            const u32 v11 = v10 + 1U;
            // Counter-clockwise seen from above (+y).
            idx[k++] = v00;
            idx[k++] = v10;
            idx[k++] = v11;
            idx[k++] = v00;
            idx[k++] = v11;
            idx[k++] = v01;
        }
    }
    lod_mesh_build(m, v, n, idx, k);
    NPH_FREE_ARRAY(a, NphRenderVertexInput, v, n);
    NPH_FREE_ARRAY(a, u32, idx, (u64)cells * cells * 6U);
}

static void lod_mesh_free(LodMesh *m)
{
    nph_render_cluster_lods_free(&m->lods);
    nph_render_meshlets_free(&m->meshlets);
    nph_render_mesh_data_free(&m->data);
}

// The same mesh's level 0 alone, as a mesh without levels.
static NphRenderMeshDesc level0_desc(const LodMesh *m)
{
    NphRenderMeshDesc d = m->desc;
    d.meshlet_count = m->lods.level0_clusters;
    d.cluster_lods = NULL;
    d.lod_groups = NULL;
    d.lod_group_count = 0U;
    d.lod_group_clusters = NULL;
    d.lod_group_cluster_count = 0U;
    d.lod_nodes = NULL;
    d.lod_node_count = 0U;
    d.lod_level0_meshlets = 0U;
    return d;
}

static NphRenderMesh create_mesh(RenderFixture *f, const NphRenderMeshDesc *d)
{
    NphRenderMesh mesh = NPH_RENDER_MESH_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_mesh_create(f->renderer, d, &mesh), NPH_RESULT_OK);
    return mesh;
}

static NphRenderMaterial grey_material(RenderFixture *f)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = nph_vec4(0.7F, 0.7F, 0.7F, 1.0F);
    md.roughness = 0.6F;
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

static void sky(RenderFixture *f)
{
    NphRenderEnvironmentDesc env;
    memset(&env, 0, sizeof env);
    env.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&env.sky);
    NTEST_ASSERT_EQ_INT(nph_render_set_environment(f->renderer, &env), NPH_RESULT_OK);
}

static NphRenderView production_view(NphRenderCamera c)
{
    NphRenderView v;
    nph_render_view_default(&v, c);
    v.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    v.ev100 = 13.0F;
    return v;
}

static NphRenderFrameStats render_into(RenderFixture *f, const NphRenderView *v, u32 frames,
                                       u8 *out)
{
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    for (u32 i = 0; i < frames; ++i) {
        stats = test_render_read(f, v);
    }
    if (out != NULL) {
        memcpy(out, f->pixels, PIXELS);
    }
    return stats;
}

// The clusters the last frame drew (the visible list), as meshlet indices
// of instance slot `slot`, a count each (a cluster drawn twice counts 2).
static void drawn_clusters(RenderFixture *f, u32 slot, u32 meshlets, u32 *counts)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    const RenderVis *v = &f->renderer->vis;
    NphVisCounters c;
    test_read_buffer(f->dev, v->counters, 0U, sizeof c, &c);
    NTEST_ASSERT_LE_UINT(c.clusters, f->renderer->desc.max_visible_clusters);
    NTEST_ASSERT_EQ_UINT(c.overflow, 0U);
    memset(counts, 0, sizeof(u32) * meshlets);
    if (c.clusters == 0U) {
        return;
    }
    NphVisCluster *list = NPH_ALLOC_ARRAY(nph_allocator_system(), NphVisCluster, c.clusters);
    NTEST_ASSERT_NOT_NULL(list);
    test_read_buffer(f->dev, v->clusters, 0U, (u64)c.clusters * sizeof *list, list);
    for (u32 i = 0; i < c.clusters; ++i) {
        if (list[i].instance == slot) {
            NTEST_ASSERT_LT_UINT(list[i].meshlet, meshlets);
            counts[list[i].meshlet] += 1U;
        }
    }
    NPH_FREE_ARRAY(nph_allocator_system(), NphVisCluster, list, c.clusters);
}

// ---------------------------------------------------------------------------
// The cut against the CPU's
// ---------------------------------------------------------------------------

// A group's decision as the CPU makes it for an instance at `t` (rotation and
// uniform scale) and a camera at `eye` (C twin: nph_render_lod_projected_error),
// and whether it lies too close to the threshold for the GPU's rounding to
// be held to it.
typedef struct CpuCut {
    const NphRenderClusterLods *lods;
    NphDVec3 position;
    NphMat3 rotation;
    f32 scale;
    NphDVec3 eye;
    f32 pixels_per_unit;
    f32 znear;
    f32 threshold;
} CpuCut;

static bool cpu_too_coarse(const CpuCut *c, u32 group, bool *ambiguous)
{
    const NphRenderLodGroup *g = &c->lods->groups.data[group];
    if (g->error == NPH_F32_MAX) {
        return true;
    }
    const NphVec3 local = nph_mat3_mul_vec3(c->rotation, nph_vec3_scale(g->center, c->scale));
    const NphVec3 centre =
        nph_dvec3_relative(nph_dvec3_add(c->position, nph_dvec3_from_vec3(local)), c->eye);
    const f32 pixels = nph_render_lod_projected_error(
        centre, g->radius, g->error, c->scale, nph_vec3_zero(), c->pixels_per_unit, c->znear);
    if (nph_f32_abs(pixels - c->threshold) <= 1e-3F * c->threshold) {
        *ambiguous = true;
    }
    return pixels > c->threshold;
}

typedef struct CutCompare {
    u32 expected;  // clusters the CPU's cut draws
    u32 drawn;     // clusters the GPU drew
    u32 missing;   // expected, not drawn
    u32 extra;     // drawn, not expected
    u32 twice;     // drawn more than once
    u32 ambiguous; // left out of the comparison
    u32 levels;    // the levels the drawn clusters come from, a bit each
    u32 triangles;
} CutCompare;

static CutCompare compare_cut(const CpuCut *c, const u32 *counts)
{
    CutCompare r;
    memset(&r, 0, sizeof r);
    const NphRenderClusterLods *l = c->lods;
    for (u32 k = 0; k < l->clusters.meshlets.len; ++k) {
        const NphRenderClusterLod *lod = &l->lods.data[k];
        bool ambiguous = false;
        const bool fine =
            lod->group == NPH_RENDER_LOD_NONE || !cpu_too_coarse(c, lod->group, &ambiguous);
        const bool consumed_coarse = cpu_too_coarse(c, lod->parent, &ambiguous);
        const bool expected = fine && consumed_coarse;
        r.expected += expected ? 1U : 0U;
        r.drawn += counts[k] != 0U ? 1U : 0U;
        r.twice += counts[k] > 1U ? 1U : 0U;
        if (counts[k] != 0U) {
            r.levels |= 1U << nph_min_u32(lod->level, 31U);
            r.triangles += l->clusters.meshlets.data[k].triangle_count;
        }
        if (ambiguous) {
            r.ambiguous += 1U;
        } else if (expected && counts[k] == 0U) {
            r.missing += 1U;
        } else if (!expected && counts[k] != 0U) {
            r.extra += 1U;
        }
    }
    return r;
}

static u32 bit_count(u32 v)
{
    u32 n = 0U;
    for (; v != 0U; v &= v - 1U) {
        ++n;
    }
    return n;
}

// The clusters drawn are the CPU's cut, cluster for cluster, for cameras
// that see the torus near on one side and far on the other (a cut mixing
// levels), at several thresholds, and for a rotated and scaled instance;
// the whole torus in view and the other culling off, so that the cut is all
// that decides. Clusters whose groups project within a thousandth of the
// threshold are left out (the GPU's rounding may take them either way).
NTEST_F(VisLod, the_clusters_drawn_are_the_cpus_cut)
{
    REQUIRE_PRODUCTION(f);
    sky(f);
    LodMesh m;
    lod_torus(&m, 192U, 64U, 1.0F, 0.35F);
    NTEST_LOG("torus: %u triangles, %u clusters in %u levels (%u at level 0), %u groups, %u nodes",
              m.data.index_count / 3U, m.lods.clusters.meshlets.len, m.lods.levels,
              m.lods.level0_clusters, m.lods.groups.len, m.lods.nodes.len);
    NTEST_ASSERT_GE_UINT(m.lods.levels, 5U);
    const NphRenderMesh mesh = create_mesh(f, &m.desc);
    const NphRenderMaterial mat = grey_material(f);
    u32 *counts = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, m.lods.clusters.meshlets.len);
    NTEST_ASSERT_NOT_NULL(counts);

    typedef struct Case {
        NphDVec3 eye;
        NphDVec3 target;
        f32 threshold;
        f32 angle; // the instance's turn about (1, 1, 0)
        f32 scale;
    } Case;
    const Case cases[] = {
        {{0.0, 0.7, 2.9}, {0.0, 0.0, 0.0}, 1.0F, 0.0F, 1.0F},
        {{0.0, 0.7, 2.9}, {0.0, 0.0, 0.0}, 0.25F, 0.0F, 1.0F},
        {{0.0, 0.7, 2.9}, {0.0, 0.0, 0.0}, 0.05F, 0.0F, 1.0F},
        {{2.2, 1.6, 1.4}, {0.0, -0.1, 0.0}, 0.5F, 0.0F, 1.0F},
        {{0.0, 1.1, 4.6}, {0.0, 0.0, 0.0}, 0.3F, 0.7F, 1.7F},
        {{0.0, 6.0, 0.4}, {0.0, 0.0, 0.0}, 0.15F, 0.0F, 1.0F},
    };
    u32 mixed = 0U;
    for (u32 ci = 0; ci < NPH_ARRAY_COUNT(cases); ++ci) {
        const Case *k = &cases[ci];
        NphRenderTransform t = at(0.0, 0.0, 0.0, k->scale);
        const NphMat3 rotation = nph_mat3_from_quat(
            nph_quat_from_axis_angle(nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 0.0F)), k->angle));
        t.linear = nph_mat3_mul(rotation, t.linear);
        const NphRenderInstance inst = instance(f, mesh, mat, t);
        const NphRenderCamera cam =
            nph_render_camera_look_at(k->eye, k->target, nph_vec3_unit_y(), 1.2F, 0.05F);
        NphRenderView v = production_view(cam);
        v.lod_error = k->threshold;
        v.vis_flags = (u32)NPH_RENDER_VIS_NO_OCCLUSION | (u32)NPH_RENDER_VIS_NO_CONE;
        // Four frames: the stats arrive two frames late.
        const NphRenderFrameStats s = render_into(f, &v, 4U, NULL);
        const u32 slot = render_slot_of(&f->renderer->instance_pool, inst.value);
        drawn_clusters(f, slot, m.lods.clusters.meshlets.len, counts);
        CpuCut cc;
        cc.lods = &m.lods;
        cc.position = nph_dvec3_zero();
        cc.rotation = rotation;
        cc.scale = k->scale;
        cc.eye = k->eye;
        cc.pixels_per_unit = 0.5F * (f32)TEST_TARGET_SIZE / nph_f32_tan(0.5F * 1.2F);
        cc.znear = 0.05F;
        cc.threshold = k->threshold;
        const CutCompare r = compare_cut(&cc, counts);
        NTEST_LOG("case %u (threshold %.2f px): %u of %u clusters drawn (%u triangles, levels "
                  "%#x), %u missing, %u extra, %u twice, %u ambiguous; %u nodes visited, %u "
                  "clusters a finer level stood in for, %u culled by the frustum",
                  ci, (f64)k->threshold, r.drawn, r.expected, r.triangles, r.levels, r.missing,
                  r.extra, r.twice, r.ambiguous, s.vis_lod_nodes, s.vis_lod_clusters_culled,
                  s.vis_meshlets_culled_frustum);
        NTEST_EXPECT_EQ_UINT(s.vis_meshlets_culled_frustum, 0U);
        NTEST_EXPECT_EQ_UINT(r.missing, 0U);
        NTEST_EXPECT_EQ_UINT(r.extra, 0U);
        NTEST_EXPECT_EQ_UINT(r.twice, 0U);
        NTEST_EXPECT_LE_UINT(r.ambiguous * 50U, r.expected);
        NTEST_EXPECT_GT_UINT(s.vis_lod_nodes, 0U);
        mixed += bit_count(r.levels) >= 2U ? 1U : 0U;
        nph_render_instance_destroy(f->renderer, inst);
    }
    // Most of the views cut through several levels at once.
    NTEST_EXPECT_GE_UINT(mixed, 4U);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, counts, m.lods.clusters.meshlets.len);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
}

// ---------------------------------------------------------------------------
// Level 0, distance, and the frame's steps
// ---------------------------------------------------------------------------

// A threshold of zero draws level 0 through the meshes' own meshlets, as a
// mesh without levels: the same image, byte for byte, and no hierarchy
// walked. The frame takes as many steps as the deepest live hierarchy has
// levels, and none once the mesh is gone.
NTEST_F(VisLod, a_threshold_of_zero_draws_level_0)
{
    REQUIRE_PRODUCTION(f);
    sky(f);
    LodMesh m;
    lod_torus(&m, 96U, 32U, 1.0F, 0.35F);
    const NphRenderMesh with = create_mesh(f, &m.desc);
    const NphRenderMeshDesc d0 = level0_desc(&m);
    const NphRenderMesh without = create_mesh(f, &d0);
    const u32 slot = render_slot_of(&f->renderer->mesh_pool, with.value);
    const u32 depth = f->renderer->meshes[slot].lod_depth;
    NTEST_EXPECT_GE_UINT(depth, 2U);
    NTEST_EXPECT_EQ_UINT(f->renderer->vis.lod_depths[depth - 1U], 1U);
    const NphRenderMaterial mat = grey_material(f);
    NphRenderView v = production_view(nph_render_camera_look_at(
        nph_dvec3(0.0, 1.5, 3.0), nph_dvec3_zero(), nph_vec3_unit_y(), 0.9F, 0.05F));
    v.lod_error = 0.0F;
    // Every cluster in view and none culled, so that every triangle counts.
    v.vis_flags = (u32)NPH_RENDER_VIS_NO_OCCLUSION | (u32)NPH_RENDER_VIS_NO_CONE;
    static u8 a[PIXELS];
    static u8 b[PIXELS];
    NphRenderInstance i = instance(f, with, mat, at(0.0, 0.0, 0.0, 1.0F));
    const NphRenderFrameStats sa = render_into(f, &v, 3U, a);
    NTEST_EXPECT_EQ_UINT(f->renderer->vis.frame.lod_steps, 0U);
    nph_render_instance_destroy(f->renderer, i);
    i = instance(f, without, mat, at(0.0, 0.0, 0.0, 1.0F));
    const NphRenderFrameStats sb = render_into(f, &v, 3U, b);
    nph_render_instance_destroy(f->renderer, i);
    NTEST_EXPECT_EQ_MEM(a, b, PIXELS);
    NTEST_EXPECT_EQ_UINT(sa.vis_lod_nodes, 0U);
    NTEST_EXPECT_EQ_UINT(sa.vis_lod_clusters_culled, 0U);
    NTEST_EXPECT_EQ_UINT(sa.vis_triangles, sb.vis_triangles);
    NTEST_EXPECT_EQ_UINT(sa.vis_triangles, m.data.index_count / 3U);
    // A threshold above zero walks the hierarchy, a step per two node levels.
    v.lod_error = 1.0F;
    (void)render_into(f, &v, 1U, NULL);
    NTEST_EXPECT_EQ_UINT(f->renderer->vis.frame.lod_steps, (depth + 1U) / 2U);
    nph_render_mesh_destroy(f->renderer, with);
    NTEST_EXPECT_EQ_UINT(f->renderer->vis.lod_depths[depth - 1U], 0U);
    (void)render_into(f, &v, 1U, NULL);
    NTEST_EXPECT_EQ_UINT(f->renderer->vis.frame.lod_steps, 0U);
    nph_render_mesh_destroy(f->renderer, without);
    lod_mesh_free(&m);
}

// Farther away, coarser levels: the triangles drawn fall with distance, to
// the coarsest level's at the far end.
NTEST_F(VisLod, distance_draws_coarser_levels)
{
    REQUIRE_PRODUCTION(f);
    sky(f);
    LodMesh m;
    lod_torus(&m, 128U, 48U, 1.0F, 0.35F);
    const NphRenderMesh mesh = create_mesh(f, &m.desc);
    const NphRenderMaterial mat = grey_material(f);
    const NphRenderInstance inst = instance(f, mesh, mat, at(0.0, 0.0, 0.0, 1.0F));
    // The coarsest level's triangles.
    u32 coarsest = 0U;
    for (u32 k = 0; k < m.lods.clusters.meshlets.len; ++k) {
        if (m.lods.lods.data[k].level == m.lods.levels - 1U) {
            coarsest += m.lods.clusters.meshlets.data[k].triangle_count;
        }
    }
    const f64 distances[] = {2.5, 5.0, 10.0, 20.0, 40.0, 400.0};
    u32 last = NPH_U32_MAX;
    u32 tris[NPH_ARRAY_COUNT(distances)];
    for (u32 i = 0; i < NPH_ARRAY_COUNT(distances); ++i) {
        const NphRenderCamera cam =
            nph_render_camera_look_at(nph_dvec3(0.0, 0.3 * distances[i], distances[i]),
                                      nph_dvec3_zero(), nph_vec3_unit_y(), 0.6F, 0.05F);
        NphRenderView v = production_view(cam);
        v.vis_flags = (u32)NPH_RENDER_VIS_NO_OCCLUSION | (u32)NPH_RENDER_VIS_NO_CONE;
        const NphRenderFrameStats s = render_into(f, &v, 4U, NULL);
        tris[i] = s.vis_triangles;
        NTEST_LOG("at %.1f: %u triangles (level 0 has %u, the coarsest level %u)", distances[i],
                  s.vis_triangles, m.data.index_count / 3U, coarsest);
        NTEST_EXPECT_LE_UINT(s.vis_triangles, last);
        last = s.vis_triangles;
    }
    NTEST_EXPECT_LT_UINT(tris[0], m.data.index_count / 3U);
    NTEST_EXPECT_EQ_UINT(tris[NPH_ARRAY_COUNT(distances) - 1U], coarsest);
    NTEST_EXPECT_LT_UINT(tris[2] * 2U, tris[0]);
    nph_render_instance_destroy(f->renderer, inst);
    nph_render_mesh_destroy(f->renderer, mesh);
    lod_mesh_free(&m);
}

// ---------------------------------------------------------------------------
// Holes
// ---------------------------------------------------------------------------

#define HOLE_SIZE 256U

// A fixture on a device with mesh shaders or with them switched off (the
// indirect raster lavapipe-class devices take): false without a device or a
// production path, or without the path asked for.
static bool raster_fixture(RenderFixture *fx, bool mesh_shaders)
{
    memset(fx, 0, sizeof *fx);
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.headless = 1;
    dd.disable_features = mesh_shaders ? 0U : (u32)NPH_GPU_OPTIONAL_MESH_SHADER;
    fx->dev = test_gpu_create(&dd);
    if (fx->dev == NULL) {
        return false;
    }
    NphRendererDesc rd;
    test_renderer_desc(&rd, fx->dev);
    NTEST_ASSERT_EQ_INT(nph_renderer_create(&rd, &fx->renderer), NPH_RESULT_OK);
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(nph_gpu_render_graph_create(fx->dev, &gd, &fx->graph), NPH_RESULT_OK);
    fx->target = test_make_target(fx->dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, TEST_TARGET_SIZE);
    fx->pixels = NPH_ALLOC_ARRAY_ZEROED(nph_allocator_system(), u8, PIXELS);
    NTEST_ASSERT_NOT_NULL(fx->pixels);
    return nph_renderer_production_supported(fx->renderer) &&
           (fx->renderer->vis.mesh_shaders != 0) == mesh_shaders;
}

// Whether a pixel is the background's colour (within two levels: the
// tonemapper's output for the solid background, read from a frame of it).
static bool is_background(const u8 *p, const u8 *background)
{
    for (u32 c = 0; c < 3U; ++c) {
        const i32 e = (i32)p[c] - (i32)background[c];
        if (e < -2 || e > 2) {
            return false;
        }
    }
    return true;
}

// The background pixels of `cut` more than two pixels inside `level0`'s
// outline (no background in the 5 x 5 around them there); `inside` counts
// the pixels that are.
static u32 count_holes(const u8 *level0, const u8 *cut, const u8 *background, u32 *inside)
{
    u32 holes = 0U;
    *inside = 0U;
    for (u32 y = 2U; y + 2U < HOLE_SIZE; ++y) {
        for (u32 x = 2U; x + 2U < HOLE_SIZE; ++x) {
            bool interior = true;
            for (u32 dy = 0; dy < 5U && interior; ++dy) {
                for (u32 dx = 0; dx < 5U && interior; ++dx) {
                    interior = !is_background(
                        level0 + ((u64)(y + dy - 2U) * HOLE_SIZE + (x + dx - 2U)) * 4U, background);
                }
            }
            if (interior) {
                *inside += 1U;
                holes += is_background(cut + ((u64)y * HOLE_SIZE + x) * 4U, background) ? 1U : 0U;
            }
        }
    }
    return holes;
}

typedef struct HoleScene {
    const char *name;
    bool terrain;
    NphRenderTransform transform;
    NphDVec3 eye;
    NphDVec3 target;
    f32 yfov;
} HoleScene;

// A cut mixing levels is watertight: a terrain sheet seen at a grazing angle
// (near fine, far coarse; nothing behind it but the background) and a torus
// up close, against a magenta background that shows through any crack, on
// both raster paths (mesh shaders, indirect draws) and with the software
// raster on and off. The outline moves with the levels by up to the
// threshold, so only pixels more than two pixels inside level 0's outline
// count.
NTEST(vis_lod, a_mixed_cut_leaves_no_holes)
{
    NphRenderTransform tilted = at(0.0, 0.0, 0.0, 1.0F);
    tilted.linear = nph_mat3_from_quat(nph_quat_from_axis_angle(nph_vec3_unit_x(), 0.25F));
    const HoleScene scenes[2] = {
        {"terrain", true, at(0.0, 0.0, 0.0, 1.0F), {0.2, 0.75, 3.6}, {0.0, -0.1, -1.0}, 1.0F},
        {"torus", false, tilted, {0.7, 0.5, 1.55}, {-0.3, -0.1, -0.2}, 1.1F},
    };
    const u64 bytes = (u64)HOLE_SIZE * HOLE_SIZE * 4U;
    u8 *level0 = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, bytes);
    u8 *cut = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, bytes);
    NTEST_ASSERT_TRUE(level0 != NULL && cut != NULL);
    LodMesh meshes[2];
    lod_terrain(&meshes[0], 128U, 4.0F);
    lod_torus(&meshes[1], 192U, 64U, 1.0F, 0.35F);
    u32 paths = 0U;
    for (u32 pass = 0; pass < 2U; ++pass) {
        RenderFixture fx;
        if (!raster_fixture(&fx, pass == 0U)) {
            test_render_teardown(&fx);
            continue;
        }
        paths += 1U;
        sky(&fx);
        const NphGpuImage target =
            test_make_target(fx.dev, NPH_GPU_FORMAT_R8G8B8A8_UNORM, HOLE_SIZE);
        const NphRenderMaterial mat = grey_material(&fx);
        for (u32 si = 0; si < 2U; ++si) {
            const HoleScene *sc = &scenes[si];
            const LodMesh *m = &meshes[sc->terrain ? 0 : 1];
            NphRenderView v = production_view(
                nph_render_camera_look_at(sc->eye, sc->target, nph_vec3_unit_y(), sc->yfov, 0.02F));
            v.background = NPH_RENDER_BACKGROUND_SOLID;
            v.background_color = nph_vec3(20000.0F, 0.0F, 20000.0F);
            // The background alone, for its colour.
            (void)test_render_frame(&fx, &v, target);
            NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(fx.dev), NPH_RESULT_OK);
            test_read_image(fx.dev, target, 0U, cut, bytes);
            u8 background[4];
            memcpy(background, cut, sizeof background);
            const NphRenderMesh mesh = create_mesh(&fx, &m->desc);
            const NphRenderInstance inst = instance(&fx, mesh, mat, sc->transform);
            const u32 flags[2] = {0U, (u32)NPH_RENDER_VIS_NO_SW_RASTER};
            const f32 thresholds[2] = {1.0F, 4.0F};
            for (u32 fi = 0; fi < 2U; ++fi) {
                v.vis_flags = flags[fi];
                v.lod_error = 0.0F;
                for (u32 i = 0; i < 2U; ++i) {
                    (void)test_render_frame(&fx, &v, target);
                }
                NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(fx.dev), NPH_RESULT_OK);
                test_read_image(fx.dev, target, 0U, level0, bytes);
                for (u32 ti = 0; ti < 2U; ++ti) {
                    v.lod_error = thresholds[ti];
                    NphRenderFrameStats st;
                    memset(&st, 0, sizeof st);
                    for (u32 i = 0; i < 4U; ++i) {
                        st = test_render_frame(&fx, &v, target);
                    }
                    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(fx.dev), NPH_RESULT_OK);
                    test_read_image(fx.dev, target, 0U, cut, bytes);
                    u32 inside = 0U;
                    const u32 holes = count_holes(level0, cut, background, &inside);
                    NTEST_LOG("%s, %s, %s raster, threshold %.0f px: %u of %u triangles, %u "
                              "clusters a finer level stood in for; %u background pixels "
                              "among %u inside the outline",
                              sc->name, pass == 0U ? "mesh shaders" : "indirect draws",
                              fi == 0U ? "hybrid" : "hardware", (f64)thresholds[ti],
                              st.vis_triangles, m->data.index_count / 3U,
                              st.vis_lod_clusters_culled, holes, inside);
                    NTEST_EXPECT_GT_UINT(inside, HOLE_SIZE * HOLE_SIZE / 8U);
                    NTEST_EXPECT_LT_UINT(st.vis_triangles, m->data.index_count / 3U);
                    NTEST_EXPECT_GT_UINT(st.vis_lod_clusters_culled, 0U);
                    NTEST_EXPECT_EQ_UINT(holes, 0U);
                }
            }
            nph_render_instance_destroy(fx.renderer, inst);
            nph_render_mesh_destroy(fx.renderer, mesh);
        }
        nph_gpu_image_destroy(fx.dev, target);
        test_render_teardown(&fx);
    }
    NPH_FREE_ARRAY(nph_allocator_system(), u8, level0, bytes);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, cut, bytes);
    lod_mesh_free(&meshes[0]);
    lod_mesh_free(&meshes[1]);
    if (paths == 0U) {
        NTEST_SKIP("no production path on this device");
    }
}

// ---------------------------------------------------------------------------
// Phase 1
// ---------------------------------------------------------------------------

// An instance phase 0 defers (last frame's depth hid it) walks its hierarchy
// in phase 1, against this frame's depth: the first frame after the wall in
// front of it moves away draws it whole, as the frames after do.
NTEST_F(VisLod, an_instance_revealed_in_phase_1_draws_its_whole_cut)
{
    REQUIRE_PRODUCTION(f);
    sky(f);
    LodMesh m;
    lod_torus(&m, 96U, 32U, 1.0F, 0.35F);
    const NphRenderMesh mesh = create_mesh(f, &m.desc);
    NphRenderMeshData cube_data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &cube_data),
                        NPH_RESULT_OK);
    const NphRenderMesh cube = test_upload_mesh(f->renderer, &cube_data);
    nph_render_mesh_data_free(&cube_data);
    const NphRenderMaterial mat = grey_material(f);
    NphRenderTransform wall = at(0.0, 0.0, 2.0, 1.0F);
    wall.linear = nph_mat3_from_scale(nph_vec3(6.0F, 6.0F, 0.2F));
    const NphRenderInstance w = instance(f, cube, mat, wall);
    (void)instance(f, mesh, mat, at(0.0, 0.0, -1.0, 1.0F));
    NphRenderView v = production_view(test_camera());
    v.debug_view = NPH_RENDER_VIEW_INSTANCES;
    static u8 first[PIXELS];
    static u8 later[PIXELS];
    (void)render_into(f, &v, 3U, NULL);
    const NphRenderTransform aside = at(60.0, 0.0, 2.0, 1.0F);
    NTEST_EXPECT_EQ_INT(nph_render_instance_set_transform(f->renderer, w, &aside), NPH_RESULT_OK);
    (void)render_into(f, &v, 1U, first);
    NphVisCounters c;
    test_read_buffer(f->dev, f->renderer->vis.counters, 0U, sizeof c, &c);
    const NphRenderFrameStats s = render_into(f, &v, 3U, later);
    NTEST_LOG("the frame after the reveal: %u instances deferred, %u nodes visited",
              c.deferred_instances, c.lod_nodes_visited);
    NTEST_EXPECT_GE_UINT(c.deferred_instances, 1U);
    NTEST_EXPECT_GT_UINT(c.lod_nodes_visited, 0U);
    NTEST_EXPECT_GT_UINT(s.vis_triangles, 0U);
    NTEST_EXPECT_EQ_MEM(first, later, PIXELS);
    lod_mesh_free(&m);
}

// ---------------------------------------------------------------------------
// Limits and validation
// ---------------------------------------------------------------------------

// More hierarchy work than max_lod_nodes holds: the work past it is dropped
// and the overflow reported, with no error.
NTEST(vis_lod, a_full_hierarchy_queue_is_reported)
{
    RenderFixture fx;
    NphRendererDesc rd;
    test_renderer_desc(&rd, NULL);
    rd.max_lod_nodes = 64U;
    test_render_setup(&fx, &rd);
    if (fx.dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    if (!nph_renderer_production_supported(fx.renderer)) {
        test_render_teardown(&fx);
        NTEST_SKIP("no production path on this device");
    }
    sky(&fx);
    LodMesh m;
    lod_torus(&m, 96U, 32U, 1.0F, 0.35F);
    const NphRenderMesh mesh = create_mesh(&fx, &m.desc);
    const NphRenderMaterial mat = grey_material(&fx);
    for (u32 i = 0; i < 48U; ++i) {
        const u32 column = i % 8U;
        const u32 row = i / 8U;
        (void)instance(&fx, mesh, mat, at(-3.0 + (f64)column, -2.5 + (f64)row, -6.0, 0.4F));
    }
    NphRenderView v = production_view(test_camera());
    // A threshold so small that every level's tree is walked to its leaves.
    v.lod_error = 0.001F;
    const NphRenderFrameStats s = render_into(&fx, &v, 4U, NULL);
    NTEST_LOG("overflow bits %#x; %u nodes visited", s.vis_overflow, s.vis_lod_nodes);
    NTEST_EXPECT_NE_UINT(s.vis_overflow & (u32)NPH_RENDER_VIS_OVERFLOW_LOD_NODES, 0U);
    NTEST_EXPECT_LE_UINT(s.vis_lod_nodes, 64U * 2U * 16U);
    lod_mesh_free(&m);
    test_render_teardown(&fx);
}

// Levels whose indices would read outside their streams, a hierarchy that is
// not a tree, or a group whose numbers in its node differ from its record
// are refused; the levels without their parts too.
NTEST_F(VisLod, broken_levels_are_refused)
{
    REQUIRE_RENDERER(f);
    LodMesh m;
    lod_torus(&m, 48U, 16U, 1.0F, 0.35F);
    NTEST_ASSERT_GE_UINT(m.lods.nodes.len, 2U);
    NTEST_ASSERT_GE_UINT(m.lods.levels, 2U);
    NphAllocator *a = nph_allocator_system();
    const u32 nodes = m.lods.nodes.len;
    const u32 groups = m.lods.groups.len;
    const u32 clusters = m.lods.clusters.meshlets.len;
    NphRenderLodNode *tree = NPH_ALLOC_ARRAY(a, NphRenderLodNode, nodes);
    NphRenderLodGroup *recs = NPH_ALLOC_ARRAY(a, NphRenderLodGroup, groups);
    NphRenderClusterLod *places = NPH_ALLOC_ARRAY(a, NphRenderClusterLod, clusters);
    NTEST_ASSERT_TRUE(tree != NULL && recs != NULL && places != NULL);
    NphRenderMeshDesc d = m.desc;
    d.lod_nodes = tree;
    d.lod_groups = recs;
    d.cluster_lods = places;
#define RESET()                                                                                    \
    do {                                                                                           \
        memcpy(tree, m.lods.nodes.data, sizeof *tree *nodes);                                      \
        memcpy(recs, m.lods.groups.data, sizeof *recs *groups);                                    \
        memcpy(places, m.lods.lods.data, sizeof *places *clusters);                                \
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

    // The root's first child that is a node, and a group leaf below it.
    u32 node_child = NPH_U32_MAX;
    for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
        const u32 c = tree[0].child[i];
        if (c != NPH_RENDER_LOD_NONE && (c & NPH_RENDER_LOD_LEAF) == 0U) {
            node_child = i;
            break;
        }
    }
    NTEST_ASSERT_NE_UINT(node_child, NPH_U32_MAX);
    // A child node out of range, the root as a child (a cycle), a node
    // reached twice.
    tree[0].child[node_child] = nodes;
    EXPECT_REFUSED();
    tree[tree[0].child[node_child]].child[0] = 0U;
    EXPECT_REFUSED();
    const u32 other = (node_child + 1U) % NPH_RENDER_LOD_NODE_CHILDREN;
    tree[0].child[other] = tree[0].child[node_child];
    tree[0].sphere[other] = tree[0].sphere[node_child];
    tree[0].error[other] = tree[0].error[node_child];
    EXPECT_REFUSED();
    // A node no walk reaches (the root has more than one child here).
    u32 root_children = 0U;
    for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
        root_children += tree[0].child[i] != NPH_RENDER_LOD_NONE ? 1U : 0U;
    }
    NTEST_ASSERT_GE_UINT(root_children, 2U);
    for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
        tree[0].child[i] = NPH_RENDER_LOD_NONE;
    }
    tree[0].child[0] = m.lods.nodes.data[0].child[node_child];
    tree[0].sphere[0] = m.lods.nodes.data[0].sphere[node_child];
    tree[0].error[0] = m.lods.nodes.data[0].error[node_child];
    EXPECT_REFUSED();
    // A leaf naming no group; a leaf whose numbers differ from its group's.
    u32 leaf_node = NPH_U32_MAX;
    u32 leaf_child = 0U;
    for (u32 n = 0; n < nodes && leaf_node == NPH_U32_MAX; ++n) {
        for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
            const u32 c = tree[n].child[i];
            if (c != NPH_RENDER_LOD_NONE && (c & NPH_RENDER_LOD_LEAF) != 0U) {
                leaf_node = n;
                leaf_child = i;
                break;
            }
        }
    }
    NTEST_ASSERT_NE_UINT(leaf_node, NPH_U32_MAX);
    tree[leaf_node].child[leaf_child] = groups | NPH_RENDER_LOD_LEAF;
    EXPECT_REFUSED();
    tree[leaf_node].error[leaf_child] =
        nph_f32_from_bits(nph_f32_to_bits(tree[leaf_node].error[leaf_child]) + 1U);
    EXPECT_REFUSED();
    tree[leaf_node].sphere[leaf_child].w *= 1.5F;
    EXPECT_REFUSED();
    // A maker error above the group's (the walk would skip clusters it must
    // draw); one below is only looser.
    if (tree[leaf_node].maker_error[leaf_child] > 0.0F) {
        tree[leaf_node].maker_error[leaf_child] *= 0.5F;
        NphRenderMesh looser = NPH_RENDER_MESH_NULL;
        NTEST_EXPECT_EQ_INT(nph_render_mesh_create(f->renderer, &d, &looser), NPH_RESULT_OK);
        nph_render_mesh_destroy(f->renderer, looser);
        RESET();
    }
    tree[leaf_node].maker_error[leaf_child] = tree[leaf_node].maker_error[leaf_child] * 2.0F + 1.0F;
    EXPECT_REFUSED();
    // A node's error below a child's, its maker error above one.
    const u32 inner = m.lods.nodes.data[0].child[node_child];
    tree[0].error[node_child] = 0.0F;
    EXPECT_REFUSED();
    tree[0].maker_error[node_child] = NPH_F32_MAX;
    EXPECT_REFUSED();
    tree[inner].maker_error[0] = -1.0F;
    EXPECT_REFUSED();
    // A group's clusters past the list; a non-finite error.
    recs[0].cluster_count = m.lods.group_clusters.len + 1U;
    EXPECT_REFUSED();
    recs[0].error = NPH_F32_INFINITY;
    EXPECT_REFUSED();
    // A level-0 cluster with a maker; a cluster made by no group there is.
    places[0].group = 0U;
    EXPECT_REFUSED();
    places[clusters - 1U].group = groups;
    EXPECT_REFUSED();
    places[0].parent = groups;
    EXPECT_REFUSED();
    // Level 0 larger than every level; the levels without meshlets.
    d.lod_level0_meshlets = clusters + 1U;
    EXPECT_REFUSED();
    d.lod_level0_meshlets = m.desc.lod_level0_meshlets;
    d.meshlets = NULL;
    EXPECT_REFUSED();
    d.meshlets = m.desc.meshlets;
    // Parts of the levels without the hierarchy.
    d.lod_nodes = NULL;
    d.lod_node_count = 0U;
    EXPECT_REFUSED();
#undef RESET
#undef EXPECT_REFUSED
    NPH_FREE_ARRAY(a, NphRenderLodNode, tree, nodes);
    NPH_FREE_ARRAY(a, NphRenderLodGroup, recs, groups);
    NPH_FREE_ARRAY(a, NphRenderClusterLod, places, clusters);
    lod_mesh_free(&m);
}
