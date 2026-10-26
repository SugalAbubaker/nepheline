// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the tests of the levels of detail (test_vis_lod.c, the camera's
// cut; test_vsm.c, the shadow map's): meshes with the levels
// nph_render_cluster_lods_build makes, as a cooked mesh brings them.

#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>

#include "test_render_support.h"

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

static inline void lod_mesh_build(LodMesh *m, const NphRenderVertexInput *v, u32 n, const u32 *idx,
                                  u32 k)
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
static inline void lod_torus(LodMesh *m, u32 rings, u32 sides, f32 big, f32 small)
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
static inline f32 terrain_height(f32 x, f32 z)
{
    return 0.18F * nph_f32_sin(1.3F * x) * nph_f32_cos(1.1F * z) +
           0.06F * nph_f32_sin(3.7F * x + 2.9F * z);
}

static inline void lod_terrain(LodMesh *m, u32 cells, f32 half)
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

static inline void lod_mesh_free(LodMesh *m)
{
    nph_render_cluster_lods_free(&m->lods);
    nph_render_meshlets_free(&m->meshlets);
    nph_render_mesh_data_free(&m->data);
}

// The same mesh's level 0 alone, as a mesh without levels.
static inline NphRenderMeshDesc level0_desc(const LodMesh *m)
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
