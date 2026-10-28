// SPDX-License-Identifier: Apache-2.0
// Levels of detail (mesh.h): the cooker's mesh through the renderer's
// builder (render/cluster_lod.h), over the positions and attributes the GPU
// sees, so a cooked mesh's levels are the ones the renderer would build. The
// cooked records (asset/mesh.h) and the renderer's are the same bytes.
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>

#include <cook/cook.h>
#include <cook/mesh.h>
#include <cook/voxel.h>
#include <stddef.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphMeshClusterLod) == sizeof(NphRenderClusterLod) &&
                      offsetof(NphMeshClusterLod, group) == offsetof(NphRenderClusterLod, group) &&
                      offsetof(NphMeshClusterLod, parent) ==
                          offsetof(NphRenderClusterLod, parent) &&
                      offsetof(NphMeshClusterLod, level) == offsetof(NphRenderClusterLod, level),
                  "cluster level layouts agree");
NPH_STATIC_ASSERT(sizeof(NphMeshLodGroup) == sizeof(NphRenderLodGroup) &&
                      offsetof(NphMeshLodGroup, center) == offsetof(NphRenderLodGroup, center) &&
                      offsetof(NphMeshLodGroup, radius) == offsetof(NphRenderLodGroup, radius) &&
                      offsetof(NphMeshLodGroup, error) == offsetof(NphRenderLodGroup, error) &&
                      offsetof(NphMeshLodGroup, level) == offsetof(NphRenderLodGroup, level) &&
                      offsetof(NphMeshLodGroup, cluster_offset) ==
                          offsetof(NphRenderLodGroup, cluster_offset) &&
                      offsetof(NphMeshLodGroup, cluster_count) ==
                          offsetof(NphRenderLodGroup, cluster_count),
                  "group layouts agree");
NPH_STATIC_ASSERT(sizeof(NphMeshLodNode) == sizeof(NphRenderLodNode) &&
                      offsetof(NphMeshLodNode, sphere) == offsetof(NphRenderLodNode, sphere) &&
                      offsetof(NphMeshLodNode, error) == offsetof(NphRenderLodNode, error) &&
                      offsetof(NphMeshLodNode, maker_error) ==
                          offsetof(NphRenderLodNode, maker_error) &&
                      offsetof(NphMeshLodNode, child) == offsetof(NphRenderLodNode, child),
                  "node layouts agree");
NPH_STATIC_ASSERT(sizeof(NphMeshVoxelBrick) == sizeof(NphRenderVoxelBrick) &&
                      offsetof(NphMeshVoxelBrick, origin) ==
                          offsetof(NphRenderVoxelBrick, origin) &&
                      offsetof(NphMeshVoxelBrick, cell) == offsetof(NphRenderVoxelBrick, cell) &&
                      offsetof(NphMeshVoxelBrick, mask) == offsetof(NphRenderVoxelBrick, mask) &&
                      offsetof(NphMeshVoxelBrick, first_voxel) ==
                          offsetof(NphRenderVoxelBrick, first_voxel),
                  "brick layouts agree");
NPH_STATIC_ASSERT(sizeof(NphMeshVoxel) == sizeof(NphRenderVoxel) &&
                      offsetof(NphMeshVoxel, color) == offsetof(NphRenderVoxel, color) &&
                      offsetof(NphMeshVoxel, normal) == offsetof(NphRenderVoxel, normal) &&
                      offsetof(NphMeshVoxel, material) == offsetof(NphRenderVoxel, material) &&
                      offsetof(NphMeshVoxel, box) == offsetof(NphRenderVoxel, box),
                  "voxel layouts agree");
NPH_STATIC_ASSERT(NPH_MESH_VOXEL_CLUSTER_BRICKS == NPH_RENDER_VOXEL_CLUSTER_BRICKS,
                  "voxel cluster limits agree");
NPH_STATIC_ASSERT(NPH_MESH_LOD_NONE == NPH_RENDER_LOD_NONE &&
                      NPH_MESH_LOD_LEAF == NPH_RENDER_LOD_LEAF &&
                      NPH_MESH_LOD_CHILDREN == NPH_RENDER_LOD_NODE_CHILDREN,
                  "level constants agree");

// Each level's clusters (voxel ones apart), triangles, voxels and their
// cells, and the errors of the groups that made them, a debug log line a
// level (nph_cook --verbose): where a mesh's levels stop shrinking, or jump.
// Then what a view drawing the mesh at each power-of-two error draws of it
// (log_cuts): where the far cuts stay large. Shipping compiles DEBUG lines
// out, and the counting with them.
#if NPH_CONFIG_SHIPPING
static void log_levels(const CookMesh *mesh, const NphRenderClusterLods *l)
{
    (void)mesh;
    (void)l;
}

static void log_cuts(const CookMesh *mesh, const NphRenderClusterLods *l, NphAllocator *alloc)
{
    (void)mesh;
    (void)l;
    (void)alloc;
}
#else
static void log_levels(const CookMesh *mesh, const NphRenderClusterLods *l)
{
    if (!NPH_LOG_ENABLED(&NPH_LOG_COOK, NPH_LOG_LEVEL_DEBUG)) {
        return;
    }
    for (u32 level = 0; level < l->levels; ++level) {
        u32 clusters = 0U;
        u32 voxel_clusters = 0U;
        u64 triangles = 0U;
        u64 voxels = 0U;
        f32 cell_lo = NPH_F32_MAX;
        f32 cell_hi = 0.0F;
        f32 error_lo = NPH_F32_MAX;
        f32 error_hi = 0.0F;
        for (u32 c = 0; c < l->lods.len; ++c) {
            const NphRenderClusterLod *lod = &l->lods.data[c];
            if (lod->level != level) {
                continue;
            }
            const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
            clusters += 1U;
            if (m->vertex_count == 0U) {
                // A voxel cluster: its bricks from its vertex offset.
                voxel_clusters += 1U;
                for (u32 k = 0; k < m->triangle_count; ++k) {
                    const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
                    voxels += nph_popcount_u32(br->mask[0]) + nph_popcount_u32(br->mask[1]);
                    cell_lo = nph_f32_min(cell_lo, br->cell);
                    cell_hi = nph_f32_max(cell_hi, br->cell);
                }
            } else {
                triangles += m->triangle_count;
            }
            if (lod->group != NPH_RENDER_LOD_NONE) {
                const f32 e = l->groups.data[lod->group].error;
                error_lo = nph_f32_min(error_lo, e);
                error_hi = nph_f32_max(error_hi, e);
            }
        }
        if (error_lo > error_hi) {
            error_lo = 0.0F; // level 0: no maker
            error_hi = 0.0F;
        }
        if (cell_lo > cell_hi) {
            cell_lo = 0.0F;
        }
        NPH_LOG_DEBUG(&NPH_LOG_COOK,
                      "mesh %s level %u: %u clusters (%u of voxels), %llu triangles, %llu voxels "
                      "(cells %.4g to %.4g), made at errors %.4g to %.4g",
                      mesh->name, level, clusters, voxel_clusters, (unsigned long long)triangles,
                      (unsigned long long)voxels, (f64)cell_lo, (f64)cell_hi, (f64)error_lo,
                      (f64)error_hi);
    }
}

static void log_cuts(const CookMesh *mesh, const NphRenderClusterLods *l, NphAllocator *alloc)
{
    if (!NPH_LOG_ENABLED(&NPH_LOG_COOK, NPH_LOG_LEVEL_DEBUG)) {
        return;
    }
    NphArrayU32 table;
    NPH_ARRAY_INIT(&table, alloc);
    i32 exp0 = 0;
    u32 count = 0U;
    if (nph_render_lod_cuts(l->lods.data, l->lods.len, l->groups.data, l->groups.len,
                            NPH_RENDER_LOD_MAX_CUT_ENTRIES, &exp0, &count,
                            &table) == NPH_RESULT_OK) {
        for (u32 i = 0; i < count; ++i) {
            u32 clusters = 0U;
            u32 voxel_clusters = 0U;
            u64 triangles = 0U;
            u64 bricks = 0U;
            u64 voxels = 0U;
            for (u32 e = table.data[i]; e < table.data[i + 1U]; ++e) {
                const NphRenderMeshlet *m = &l->clusters.meshlets.data[table.data[e]];
                ++clusters;
                if (m->vertex_count == 0U) {
                    ++voxel_clusters;
                    bricks += m->triangle_count;
                    for (u32 k = 0; k < m->triangle_count; ++k) {
                        const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
                        voxels += nph_popcount_u32(br->mask[0]) + nph_popcount_u32(br->mask[1]);
                    }
                } else {
                    triangles += m->triangle_count;
                }
            }
            NPH_LOG_DEBUG(&NPH_LOG_COOK,
                          "mesh %s cut 2^%d: %u clusters (%u of voxels), %llu triangles, %llu "
                          "bricks, %llu voxels",
                          mesh->name, exp0 + (i32)i, clusters, voxel_clusters,
                          (unsigned long long)triangles, (unsigned long long)bricks,
                          (unsigned long long)voxels);
        }
    }
    NPH_ARRAY_FREE(&table);
}
#endif

NphResult cook_mesh_build_lods(const CookMesh *mesh, const CookMeshlets *meshlets,
                               const NphVec3 *positions, const NphMeshAttrib *attribs,
                               const NphMeshletBounds *bounds, const CookMeshSettings *settings,
                               const struct CookVoxelMaterials *voxels, NphJobSystem *jobs,
                               NphRenderClusterLods *out)
{
    NPH_ASSERT(mesh != NULL && meshlets != NULL && settings != NULL && out != NULL);
    NphAllocator *alloc = mesh->alloc;
    const u32 sn = mesh->submeshes.len;
    // The voxelizer's materials, a surface per submesh.
    u8 *surfaces = NULL;
    CookVoxelSampler cvs;
    NphRenderVoxelSampler sampler;
    const bool with_voxels = settings->voxels && voxels != NULL;
    if (with_voxels) {
        surfaces = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, nph_max_u32(sn, 1U));
        if (surfaces == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        cook_voxel_sampler_init(&cvs, mesh, voxels, positions, attribs, surfaces, &sampler);
    }
    // Level 0 as the builder takes it: the meshlets with the bounds of the
    // dequantised positions.
    NphRenderMeshlets level0;
    nph_render_meshlets_init(&level0, alloc);
    NphRenderIndexRange *ranges = NPH_ALLOC_ARRAY(alloc, NphRenderIndexRange, nph_max_u32(sn, 1U));
    NphResult r = ranges != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    const u32 mn = meshlets->meshlets.len;
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&level0.meshlets, mn);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&level0.bounds, mn);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&level0.vertices, meshlets->vertices.data, meshlets->vertices.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&level0.triangles, meshlets->triangles.data, meshlets->triangles.len);
    }
    if (r == NPH_RESULT_OK) {
        if (mn != 0U) {
            memcpy(level0.meshlets.data, meshlets->meshlets.data, (size_t)mn * sizeof(NphMeshlet));
            memcpy(level0.bounds.data, bounds, (size_t)mn * sizeof(NphMeshletBounds));
        }
        for (u32 s = 0; s < sn; ++s) {
            ranges[s].index_offset = mesh->submeshes.data[s].index_offset;
            ranges[s].index_count = mesh->submeshes.data[s].index_count;
        }
        NphRenderClusterLodInput in;
        memset(&in, 0, sizeof in);
        in.positions = positions;
        // Seams where vertices at one position differ in what the GPU reads.
        in.attribs = attribs;
        in.attrib_stride = (u32)sizeof(NphMeshAttrib);
        in.vertex_count = mesh->vertices.len;
        in.indices = mesh->indices.data;
        in.index_count = mesh->indices.len;
        in.ranges = ranges;
        in.range_count = sn;
        in.meshlets = &level0;
        in.max_vertices = settings->meshlet_vertices;
        in.max_triangles = settings->meshlet_triangles;
        in.cone_weight = settings->meshlet_cone_weight;
        in.voxels = with_voxels ? &sampler : NULL;
        in.scratch = alloc;
        in.jobs = jobs;
        r = nph_render_cluster_lods_build(&in, out);
        if (r == NPH_RESULT_OK) {
            log_levels(mesh, out);
            log_cuts(mesh, out, alloc);
        }
    }
    if (surfaces != NULL) {
        NPH_FREE_ARRAY(alloc, u8, surfaces, nph_max_u32(sn, 1U));
    }
    if (ranges != NULL) {
        NPH_FREE_ARRAY(alloc, NphRenderIndexRange, ranges, nph_max_u32(sn, 1U));
    }
    nph_render_meshlets_free(&level0);
    return r;
}
