// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the tests of the levels of detail (test_vis_lod.c, the camera's
// cut; test_vsm.c, the shadow map's; test_vis_voxel.c, voxels): meshes with
// the levels nph_render_cluster_lods_build makes, as a cooked mesh brings
// them, with voxel levels where their sampler is given.

#include <nepheline/math/pack.h>
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>

#include "test_render_support.h"

// ---------------------------------------------------------------------------
// A mesh with levels
// ---------------------------------------------------------------------------

// A mesh with its levels of detail as a cooked mesh brings them: `desc`
// names every level's clusters and the hierarchy, and its voxels compact
// (`packed`; the levels' form stays in `lods`).
typedef struct LodMesh {
    NphRenderMeshData data;
    NphRenderMeshlets meshlets; // level 0
    NphRenderClusterLods lods;
    NphRenderPackedVoxels packed;
    NphRenderSubmesh sub;
    NphRenderMeshDesc desc;
} LodMesh;

// The voxelizer's material for the tests' meshes: opaque, of one look
// (LOD_VOXEL_ALBEDO, roughness 0.7, metallic 0), with the normals the
// triangles shade with (the vertices' interpolated, as the cooker's sampler
// has them).
#define LOD_VOXEL_ALBEDO nph_vec3(0.3F, 0.5F, 0.2F)

typedef struct LodVoxelLook {
    const NphRenderVertexAttrib *attribs;
    u8 surface[1];
} LodVoxelLook;

static inline void lod_look_sample(void *user, u32 submesh, const u32 v[3], NphVec3 b,
                                   f32 footprint, NphRenderVoxelSample *out)
{
    (void)submesh;
    (void)footprint;
    const NphRenderVertexAttrib *a = ((const LodVoxelLook *)user)->attribs;
    const NphVec3 n =
        nph_vec3_add(nph_vec3_add(nph_vec3_scale(nph_oct_decode_snorm16(a[v[0]].normal), b.x),
                                  nph_vec3_scale(nph_oct_decode_snorm16(a[v[1]].normal), b.y)),
                     nph_vec3_scale(nph_oct_decode_snorm16(a[v[2]].normal), b.z));
    out->base_color = LOD_VOXEL_ALBEDO;
    out->normal = nph_vec3_normalize_or(n, nph_vec3_unit_z());
    out->roughness = 0.7F;
    out->metallic = 0.0F;
    out->occlusion = 1.0F;
}

static inline bool lod_look_covers(void *user, u32 submesh, const u32 v[3], NphVec3 b,
                                   f32 footprint)
{
    (void)user;
    (void)submesh;
    (void)v;
    (void)b;
    (void)footprint;
    return true;
}

// The mesh of `n` vertices and `k` indices with its levels, voxels among
// them where `voxels` and the levels' collapses stall on aggregate geometry.
static inline void lod_mesh_build_with(LodMesh *m, const NphRenderVertexInput *v, u32 n,
                                       const u32 *idx, u32 k, bool voxels)
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
    LodVoxelLook look;
    look.attribs = m->data.attribs;
    look.surface[0] = (u8)NPH_RENDER_VOXEL_SURFACE_OPAQUE;
    NphRenderVoxelSampler sampler;
    memset(&sampler, 0, sizeof sampler);
    sampler.user = &look;
    sampler.submesh_surface = look.surface;
    sampler.sample = lod_look_sample;
    sampler.covers = lod_look_covers;
    li.voxels = voxels ? &sampler : NULL;
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
    nph_render_packed_voxels_init(&m->packed, a);
    if (m->lods.bricks.len != 0U) {
        NphRenderMeshlets *w = &m->lods.clusters;
        NTEST_ASSERT_EQ_INT(nph_render_voxels_pack(w->meshlets.data, w->meshlets.len,
                                                   m->lods.bricks.data, m->lods.bricks.len,
                                                   m->lods.voxels.data, m->lods.voxels.len,
                                                   &m->packed),
                            NPH_RESULT_OK);
        m->desc.voxel_clusters = m->packed.clusters.data;
        m->desc.voxel_cluster_count = m->packed.clusters.len;
        m->desc.voxel_bricks = m->packed.bricks.data;
        m->desc.voxel_brick_count = m->packed.bricks.len;
        m->desc.voxel_words = m->packed.words.data;
        m->desc.voxel_word_count = m->packed.words.len;
    }
}

static inline void lod_mesh_build(LodMesh *m, const NphRenderVertexInput *v, u32 n, const u32 *idx,
                                  u32 k)
{
    lod_mesh_build_with(m, v, n, idx, k, false);
}

// Foliage: `count` closed thin needles in the cube of `side` centred on the
// origin, each a smooth-shaded tube of `sides` sides (a curve's bevel, as
// the landscape's pines are modelled), which the levels cannot simplify, so
// their coarser levels become voxels (ADR-0061), or stay stalled triangles
// without `voxels`.
static inline void lod_foliage_tubes_with(LodMesh *m, u32 count, f32 side, u64 seed, u32 sides,
                                          bool voxels)
{
    NphAllocator *a = nph_allocator_system();
    const u32 per = 2U * sides;                      // two rings
    const u32 tris = 2U * sides + 2U * (sides - 2U); // the sides, then the caps
    const u32 n = count * per;
    NphRenderVertexInput *v = NPH_ALLOC_ARRAY(a, NphRenderVertexInput, n);
    u32 *idx = NPH_ALLOC_ARRAY(a, u32, (u64)count * tris * 3U);
    NTEST_ASSERT_TRUE(v != NULL && idx != NULL);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, seed, 1U);
    u32 k = 0U;
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 c =
            nph_vec3_scale(nph_vec3_sub(nph_pcg32_next_vec3(&rng), nph_vec3_splat(0.5F)), side);
        const f32 z = 2.0F * nph_pcg32_next_f32(&rng) - 1.0F;
        const f32 phi = 2.0F * NPH_PI * nph_pcg32_next_f32(&rng);
        const f32 rr = nph_f32_sqrt(nph_f32_max(1.0F - z * z, 0.0F));
        const NphVec3 d = nph_vec3(rr * nph_f32_cos(phi), rr * nph_f32_sin(phi), z);
        const NphVec3 up = nph_f32_abs(d.z) < 0.9F ? nph_vec3_unit_z() : nph_vec3_unit_x();
        const NphVec3 t = nph_vec3_normalize(nph_vec3_cross(up, d));
        const NphVec3 b = nph_vec3_cross(d, t);
        const u32 base = i * per;
        for (u32 ring = 0; ring < 2U; ++ring) {
            for (u32 j = 0; j < sides; ++j) {
                const f32 angle = 2.0F * NPH_PI * (f32)j / (f32)sides;
                const NphVec3 off = nph_vec3_add(nph_vec3_scale(t, nph_f32_cos(angle)),
                                                 nph_vec3_scale(b, nph_f32_sin(angle)));
                const NphVec3 end = nph_vec3_scale(d, (ring == 0U ? -0.05F : 0.05F) * side);
                NphRenderVertexInput *p = &v[base + ring * sides + j];
                p->position =
                    nph_vec3_add(nph_vec3_add(c, end), nph_vec3_scale(off, 0.002F * side));
                p->normal = off;
                p->uv = nph_vec2(0.0F, 0.0F);
                p->color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
            }
        }
        // The sides, wound outward (counter-clockwise from outside), then the
        // caps as fans.
        for (u32 j = 0; j < sides; ++j) {
            const u32 j1 = (j + 1U) % sides;
            const u32 a0 = base + j;
            const u32 a1 = base + j1;
            const u32 b0 = base + sides + j;
            const u32 b1 = base + sides + j1;
            idx[k++] = a0;
            idx[k++] = a1;
            idx[k++] = b1;
            idx[k++] = a0;
            idx[k++] = b1;
            idx[k++] = b0;
        }
        for (u32 j = 1; j + 1U < sides; ++j) {
            idx[k++] = base;
            idx[k++] = base + j + 1U;
            idx[k++] = base + j;
            idx[k++] = base + sides;
            idx[k++] = base + sides + j;
            idx[k++] = base + sides + j + 1U;
        }
    }
    lod_mesh_build_with(m, v, n, idx, k, voxels);
    NPH_FREE_ARRAY(a, NphRenderVertexInput, v, n);
    NPH_FREE_ARRAY(a, u32, idx, (u64)count * tris * 3U);
}

static inline void lod_foliage_tubes(LodMesh *m, u32 count, f32 side, u64 seed, u32 sides)
{
    lod_foliage_tubes_with(m, count, side, seed, sides, true);
}

// Foliage of needles of six sides (lod_foliage_tubes).
static inline void lod_foliage(LodMesh *m, u32 count, f32 side, u64 seed)
{
    lod_foliage_tubes(m, count, side, seed, 6U);
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

// Deepens a mesh's hierarchy by `extra` node levels above its root, a node of
// one child each over what the old root held, as if it had many levels.
static inline void lod_mesh_deepen(LodMesh *m, u32 extra)
{
    for (u32 i = 0; i < extra; ++i) {
        const NphRenderLodNode old = m->lods.nodes.data[0];
        NphRenderLodNode top;
        memset(&top, 0, sizeof top);
        f32 error = 0.0F;
        f32 makers = NPH_F32_MAX;
        for (u32 c = 0; c < NPH_RENDER_LOD_NODE_CHILDREN; ++c) {
            top.child[c] = NPH_RENDER_LOD_NONE;
            if (old.child[c] != NPH_RENDER_LOD_NONE) {
                error = nph_f32_max(error, old.error[c]);
                makers = nph_f32_min(makers, old.maker_error[c]);
            }
        }
        top.child[0] = m->lods.nodes.len;
        top.sphere[0] = nph_vec4(0.0F, 0.0F, 0.0F, 1000.0F);
        top.error[0] = error;
        top.maker_error[0] = makers;
        NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&m->lods.nodes, old), NPH_RESULT_OK);
        m->lods.nodes.data[0] = top;
    }
    m->desc.lod_nodes = m->lods.nodes.data;
    m->desc.lod_node_count = m->lods.nodes.len;
}

static inline void lod_mesh_free(LodMesh *m)
{
    nph_render_packed_voxels_free(&m->packed);
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
