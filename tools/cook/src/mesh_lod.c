// SPDX-License-Identifier: Apache-2.0
// Levels of detail (mesh.h): the cooker's mesh through the renderer's
// builder (render/cluster_lod.h), over the positions and attributes the GPU
// sees, so a cooked mesh's levels are the ones the renderer would build. The
// cooked records (asset/mesh.h) and the renderer's are the same bytes.
#include <nepheline/core/assert.h>
#include <nepheline/render/cluster_lod.h>

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
