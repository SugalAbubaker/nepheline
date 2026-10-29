// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cooked meshes (CLAUDE.md §8.8 "Meshes: vertex quantization, vertex cache
// and overdraw optimization, meshletization, bounds").
//
// A mesh asset (NPH_ASSET_TYPE_MESH) holds these blobs, all little-endian
// plain data that the loader hands out in place:
//
//     HEAD  NphMeshHeader             counts, bounds, quantization
//     POSN  NphMeshPosition[vertex]   16-bit unorm positions in the bounds
//     ATTR  NphMeshAttrib[vertex]     normal, tangent, uv, colour
//     INDX  u32[index]                triangle list over the vertices
//     SUBM  NphSubmesh[submesh]       index and meshlet ranges per material slot
//     MLET  NphMeshlet[meshlet]       vertex and triangle ranges
//     MLBD  NphMeshletBounds[meshlet] sphere and normal cone per meshlet
//     MLVX  u32[meshlet vertex]       vertex indices referenced by meshlets
//     MLTR  u8[3 * meshlet triangle]  meshlet-local triangle corners
//
// and, from version 2, a mesh's levels of detail where it has them (CLAUDE.md
// §8.6 stage 9; render/cluster_lod.h; docs/render.md "Levels of detail"):
//
//     LODS  NphMeshLodHeader          the levels' counts
//     LODC  NphMeshClusterLod[meshlet] each cluster's maker and consumer group
//     LODG  NphMeshLodGroup[group]    the groups
//     LODM  u32[group cluster]        each group's clusters in turn
//     LODN  NphMeshLodNode[node]      the hierarchy, node 0 the root
//
// With them the meshlet blobs hold every level's clusters, level 0's first
// (`level0_meshlets` of them, the ones the submeshes' meshlet ranges name);
// the records are the renderer's (render/cluster_lod.h), byte for byte.
//
// From version 3, the levels may hold voxel clusters (render/voxel.h;
// docs/render.md "Voxels"; ADR-0061), which the aggregate geometry's coarse
// levels are made of:
//
//     VOXB  NphMeshVoxelBrick[brick]  4 x 4 x 4 cells of one size and their mask
//     VOXA  NphMeshVoxel[voxel]       each occupied cell's footprint and look
//
// A voxel cluster is a meshlet with no vertices above level 0: its bricks
// are VOXB[vertex_offset, vertex_offset + triangle_count), at most
// NPH_MESH_VOXEL_CLUSTER_BRICKS of them, its triangle_offset 0, all of one
// cell size; each brick's voxels are VOXA[first_voxel, first_voxel + its
// mask's bits), the next brick's following them.
//
// From version 4, a mesh may be voxels alone (a world partition cell's
// proxy, render/cell_proxy.h; ADR-0062): no vertices and no indices (POSN,
// ATTR, INDX, MLVX and MLTR empty or absent), every meshlet a voxel cluster,
// level 0's too, and one submesh of no indices whose meshlets are level 0's.
//
// From version 5, the voxels are compact (render/voxel.h "The compact form";
// ADR-0063), the renderer's records byte for byte:
//
//     VOXC  NphMeshVoxelCluster[voxel cluster] its voxels' first word, cell, shared look
//     VOXB  NphMeshVoxelBrick[brick]  its corner in whole bricks, first voxel's rank, mask
//     VOXA  u32[word]                 the voxels, three words each (four when wide)
//
// A voxel cluster's triangle_offset names its record, its vertex_offset its
// first brick; its bricks' first voxels' ranks follow one another from 0, at
// most NPH_MESH_VOXEL_CLUSTER_VOXELS voxels, their words from its record's
// first. Versions 3 and 4, whose voxels are in the builders' form, no longer
// read when they hold voxels (NPH_RESULT_VERSION_MISMATCH: cook them again).
//
// From version 6, a mesh's levels may be apart, each a mesh asset of its own
// that loads alone (render/cluster_lod.h "Levels apart"; ADR-0063 "Stage 2,
// a level is a mesh"), as a cell's proxy is cooked. The mesh is then a head
// (NPH_MESH_FLAG_LEVELS) of no geometry: its header (bounds and submeshes;
// no vertices, indices or meshlets), its submeshes, and
//
//     LVLS  NphMeshLevel[level]       each level's mesh asset and errors, finest first
//
// and each level (NPH_MESH_FLAG_LEVEL) a mesh of voxels alone of one level
// (its LODS `levels` 1, every cluster at its level 0, no group its maker)
// whose clusters' makers, groups of the level below, are records instead:
//
//     LODK  NphMeshLodMaker[meshlet]  the sphere and error of the group that made it
//
// Positions are separate from the other attributes (§10.3 "bandwidth
// discipline"): the visibility-buffer pass of §8.6.4 reads positions only.
// Each position is three 16-bit unorm values across the mesh bounds
// (NphMeshHeader.position_offset + unorm * position_scale), which gives
// 15 µm steps on a 1 m object; normals are octahedral snorm16 pairs,
// tangents snorm8 with the handedness in w, texture coordinates half
// floats, colours RGBA8.
//
// Meshlets follow the limits of the GPU-driven pipeline (§8.6.3; Haar &
// Aaltonen, SIGGRAPH 2015; NVIDIA's mesh shader guidance): at most 64
// vertices and 124 triangles each, with a bounding sphere and a normal
// cone for backface culling (Kapoulkine, meshoptimizer, 2017).
//
// nph_mesh_view checks every count against its blob and every range
// against the arrays it indexes, so a view can be used without further
// validation. Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/asset/loader.h>
#include <nepheline/math/geometry.h>

#ifdef __cplusplus
extern "C" {
#endif

// 2: the levels of detail (LODS, LODC, LODG, LODM, LODN). 3: voxel clusters
// in the levels (VOXB, VOXA). 4: meshes of voxels alone. 5: the voxels
// compact (VOXC). 6: levels apart (LVLS, LODK). Earlier versions without
// voxels still read.
#define NPH_MESH_TYPE_VERSION 6U

#define NPH_MESH_TAG_HEADER           NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_MESH_TAG_POSITIONS        NPH_FOURCC('P', 'O', 'S', 'N')
#define NPH_MESH_TAG_ATTRIBS          NPH_FOURCC('A', 'T', 'T', 'R')
#define NPH_MESH_TAG_INDICES          NPH_FOURCC('I', 'N', 'D', 'X')
#define NPH_MESH_TAG_SUBMESHES        NPH_FOURCC('S', 'U', 'B', 'M')
#define NPH_MESH_TAG_MESHLETS         NPH_FOURCC('M', 'L', 'E', 'T')
#define NPH_MESH_TAG_MESHLET_BOUNDS   NPH_FOURCC('M', 'L', 'B', 'D')
#define NPH_MESH_TAG_MESHLET_VERTICES NPH_FOURCC('M', 'L', 'V', 'X')
#define NPH_MESH_TAG_MESHLET_TRIS     NPH_FOURCC('M', 'L', 'T', 'R')
#define NPH_MESH_TAG_LOD_HEADER       NPH_FOURCC('L', 'O', 'D', 'S')
#define NPH_MESH_TAG_LOD_CLUSTERS     NPH_FOURCC('L', 'O', 'D', 'C')
#define NPH_MESH_TAG_LOD_GROUPS       NPH_FOURCC('L', 'O', 'D', 'G')
#define NPH_MESH_TAG_LOD_MEMBERS      NPH_FOURCC('L', 'O', 'D', 'M')
#define NPH_MESH_TAG_LOD_NODES        NPH_FOURCC('L', 'O', 'D', 'N')
#define NPH_MESH_TAG_VOXEL_CLUSTERS   NPH_FOURCC('V', 'O', 'X', 'C')
#define NPH_MESH_TAG_VOXEL_BRICKS     NPH_FOURCC('V', 'O', 'X', 'B')
#define NPH_MESH_TAG_VOXELS           NPH_FOURCC('V', 'O', 'X', 'A')
#define NPH_MESH_TAG_LOD_MAKERS       NPH_FOURCC('L', 'O', 'D', 'K')
#define NPH_MESH_TAG_LEVELS           NPH_FOURCC('L', 'V', 'L', 'S')

#define NPH_MESHLET_MAX_VERTICES  64U
#define NPH_MESHLET_MAX_TRIANGLES 124U

typedef enum NphMeshFlags {
    NPH_MESH_FLAG_HAS_TANGENTS = 1U << 0U, // tangents were authored or generated
    NPH_MESH_FLAG_HAS_UV0 = 1U << 1U,
    NPH_MESH_FLAG_HAS_COLORS = 1U << 2U,
    // Version 6: a head whose levels are apart (LVLS), or one of those levels
    // (LODK); never both.
    NPH_MESH_FLAG_LEVELS = 1U << 3U,
    NPH_MESH_FLAG_LEVEL = 1U << 4U,
} NphMeshFlags;

typedef struct NphMeshHeader {
    u32 vertex_count;
    u32 index_count;
    u32 submesh_count;
    u32 meshlet_count;
    u32 meshlet_vertex_count;   // entries in MLVX
    u32 meshlet_triangle_count; // triangles in MLTR (3 bytes each)
    u32 flags;                  // NphMeshFlags
    u32 reserved0;
    NphVec3 position_offset; // dequantize: offset + unorm16 * scale
    f32 reserved1;
    NphVec3 position_scale;
    f32 reserved2;
    NphAABB bounds; // of the dequantized positions
    f32 reserved3[2];
    NphSphere sphere; // bounding sphere
} NphMeshHeader;

NPH_STATIC_ASSERT(sizeof(NphMeshHeader) == 112U, "the mesh header is 112 bytes");

typedef struct NphMeshPosition {
    u16 x, y, z;
    u16 reserved; // zero
} NphMeshPosition;

typedef struct NphMeshAttrib {
    u32 normal;  // octahedral snorm16x2 (math/pack.h nph_oct_encode_snorm16)
    u32 tangent; // snorm8x4: xyz direction, w handedness (+1 or -1)
    u32 uv0;     // f16x2
    u32 color;   // RGBA8 unorm (0xFFFFFFFF when absent)
} NphMeshAttrib;

NPH_STATIC_ASSERT(sizeof(NphMeshPosition) == 8U, "a position is 8 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshAttrib) == 16U, "an attribute record is 16 bytes");

typedef struct NphSubmesh {
    u32 index_offset; // into INDX
    u32 index_count;  // a multiple of 3
    u32 meshlet_offset;
    u32 meshlet_count;
    u32 material_slot; // index into the scene's material list for this mesh
    u32 reserved[3];
    NphAABB bounds;
    f32 reserved2[2];
} NphSubmesh;

NPH_STATIC_ASSERT(sizeof(NphSubmesh) == 64U, "a submesh is 64 bytes");

typedef struct NphMeshlet {
    u32 vertex_offset;   // into MLVX
    u32 triangle_offset; // into MLTR, in triangles
    u16 vertex_count;    // <= NPH_MESHLET_MAX_VERTICES
    u16 triangle_count;  // <= NPH_MESHLET_MAX_TRIANGLES
    u32 submesh;         // the submesh it belongs to
} NphMeshlet;

typedef struct NphMeshletBounds {
    NphVec3 center;
    f32 radius;
    i8 cone_axis[3]; // snorm8 unit vector
    i8 cone_cutoff;  // snorm8: cull when dot(view, axis) >= cutoff
    NphVec3 cone_apex;
} NphMeshletBounds;

NPH_STATIC_ASSERT(sizeof(NphMeshlet) == 16U, "a meshlet is 16 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshletBounds) == 32U, "meshlet bounds are 32 bytes");

// Levels of detail: no group (a level-0 cluster's maker), the children of a
// hierarchy node, and a child that is a group rather than a node.
#define NPH_MESH_LOD_NONE     0xFFFFFFFFU
#define NPH_MESH_LOD_CHILDREN 8U
#define NPH_MESH_LOD_LEAF     0x80000000U

typedef struct NphMeshLodHeader {
    u32 levels;              // levels of clusters (level 0 and the coarser ones)
    u32 level0_meshlets;     // clusters [0, level0_meshlets) are level 0
    u32 group_count;         // LODG
    u32 group_cluster_count; // LODM
    u32 node_count;          // LODN
    u32 brick_count;         // VOXB (version 3; zero before)
    u32 voxel_word_count;    // VOXA, in words (version 5; voxels in versions 3 and 4)
    u32 voxel_cluster_count; // VOXC (version 5; zero before)
} NphMeshLodHeader;

typedef struct NphMeshClusterLod {
    u32 group;  // the group that made it; NPH_MESH_LOD_NONE at level 0
    u32 parent; // the group that consumes it
    u32 level;
    u32 reserved;
} NphMeshClusterLod;

typedef struct NphMeshLodGroup {
    NphVec3 center; // its error's sphere, mesh space
    f32 radius;
    f32 error;          // mesh units; the root group's the largest float
    u32 level;          // its clusters' level
    u32 cluster_offset; // into LODM
    u32 cluster_count;
} NphMeshLodGroup;

typedef struct NphMeshLodNode {
    NphVec4 sphere[NPH_MESH_LOD_CHILDREN];  // xyz centre, w radius, mesh space
    f32 error[NPH_MESH_LOD_CHILDREN];       // the largest group error below
    f32 maker_error[NPH_MESH_LOD_CHILDREN]; // the smallest maker error below
    u32 child[NPH_MESH_LOD_CHILDREN];       // node, group | LEAF, or NONE
} NphMeshLodNode;

// Voxels, compact (render/voxel.h's records, byte for byte): a voxel
// cluster's record (its voxels' first word in VOXA, its cells' edge 2^cell_log2,
// the occlusion and metallic its voxels share unless it is wide), and a brick
// of 4 x 4 x 4 cells of its cluster's edge, its lowest corner `corner` * 4 *
// cell, the occupied ones set in the mask (bit x + 4y + 16z), their voxels in
// bit order from rank `first` of its cluster's. A voxel is three words (its
// footprint's box and shape, its lobe, its look), four in a wide cluster (and
// its occlusion and metallic): render/voxel.h "The compact form".
#define NPH_MESH_VOXEL_CLUSTER_BRICKS 32U
#define NPH_MESH_VOXEL_CLUSTER_VOXELS 1024U
#define NPH_MESH_VOXEL_CLUSTER_WIDE   1U // NphMeshVoxelCluster.flags
#define NPH_MESH_VOXEL_WORDS          3U // a narrow cluster's voxel, in words
#define NPH_MESH_VOXEL_WIDE_WORDS     4U

typedef struct NphMeshVoxelCluster {
    u32 first_word;
    i8 cell_log2; // -126 to 127
    u8 occlusion; // zero in a wide cluster
    u8 metallic;  // zero in a wide cluster
    u8 flags;     // NPH_MESH_VOXEL_CLUSTER_WIDE or zero
} NphMeshVoxelCluster;

typedef struct NphMeshVoxelBrick {
    i16 corner[3]; // in bricks of its cluster's cell
    u16 first;     // its first voxel's rank in its cluster
    u32 mask[2];
} NphMeshVoxelBrick;

// A level apart (version 6): a cluster's maker as a record, the sphere and
// error of the group that made it (zeros at a mesh's level 0), which the
// renderer's culls test the cluster by (render/cluster_lod.h
// NphRenderLodMaker, byte for byte).
typedef struct NphMeshLodMaker {
    NphVec3 center;
    f32 radius;
    f32 error;
} NphMeshLodMaker;

// A head's level (version 6): the mesh asset that holds it (NPH_MESH_FLAG_LEVEL),
// the smallest error of the groups that made its clusters (0 at level 0)
// and the largest of those that consume them (a root group's, the largest
// float, at the coarsest level): from where its clusters stand in for finer
// ones to where coarser ones stand in for them, as a cut decides
// (render/cluster_lod.h). At most NPH_MESH_MAX_LEVELS, the finest first,
// their maker errors rising.
#define NPH_MESH_MAX_LEVELS 16U

typedef struct NphMeshLevel {
    NphGuid mesh;
    f32 maker_error;
    f32 error;
    u32 cluster_count; // its clusters (at least 1)
    u32 reserved;      // zero
} NphMeshLevel;

NPH_STATIC_ASSERT(sizeof(NphMeshLodHeader) == 32U, "the levels' header is 32 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshLodMaker) == 20U, "a maker's record is 20 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshLevel) == 32U, "a level is 32 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshVoxelCluster) == 8U, "a voxel cluster's record is 8 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshVoxelBrick) == 16U, "a brick is 16 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshClusterLod) == 16U, "a cluster's level record is 16 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshLodGroup) == 32U, "a group is 32 bytes");
NPH_STATIC_ASSERT(sizeof(NphMeshLodNode) == 224U, "a node is 224 bytes");

typedef struct NphMeshView {
    const NphMeshHeader *header;
    const NphMeshPosition *positions;
    const NphMeshAttrib *attribs;
    const u32 *indices;
    const NphSubmesh *submeshes;
    const NphMeshlet *meshlets;
    const NphMeshletBounds *meshlet_bounds;
    const u32 *meshlet_vertices;
    const u8 *meshlet_triangles;
    // The levels of detail, all NULL without them.
    const NphMeshLodHeader *lod;
    const NphMeshClusterLod *lod_clusters; // [meshlet_count]
    const NphMeshLodGroup *lod_groups;
    const u32 *lod_members;
    const NphMeshLodNode *lod_nodes;
    // The voxel clusters' records, bricks and voxel words, NULL without them.
    const NphMeshVoxelCluster *voxel_clusters;
    const NphMeshVoxelBrick *voxel_bricks;
    const u32 *voxel_words;
    // A level apart's makers ([meshlet_count]), NULL for any other mesh.
    const NphMeshLodMaker *lod_makers;
    // A head's levels, finest first, NULL (and 0) for any other mesh.
    const NphMeshLevel *levels;
    u32 level_count;
} NphMeshView;

// Resolves and validates the blobs of a READY mesh asset.
// NPH_RESULT_TYPE_MISMATCH for another asset type, NPH_RESULT_VERSION_MISMATCH
// for a newer schema or a version 3 or 4 mesh with voxels, NPH_RESULT_CORRUPT
// when a blob is missing, too small, or a range points outside its array: the
// levels' too (every group, cluster, and child index in range; level 0
// covering the submeshes' meshlet ranges), and the voxels' (a voxel cluster's
// record, its cell in range and no flag but WIDE; its bricks, their masks not
// empty, their first voxels' ranks following one another from 0, at most
// NPH_MESH_VOXEL_CLUSTER_VOXELS voxels, their words in range; every
// footprint a box of at least one step on each axis and a known shape, a
// capsule's radius a known code; every reserved bit zero); a head's levels
// (version 6: no geometry of its own, 1 to NPH_MESH_MAX_LEVELS levels, each
// naming an asset, with clusters, its errors finite, not negative, its maker
// error at most its error and at least the finer level's) and a level
// apart's (voxels alone, of one level, its makers' records finite, their
// radii and errors not negative). That the hierarchy is a tree and its
// bounds hold is the renderer's to check (nph_render_mesh_create), as that a
// head's levels are its own: each is a mesh asset of its own, viewed alone.
NPH_MUST_USE NphResult nph_mesh_view(const NphAsset *asset, NphMeshView *out);

// The position of vertex `p` in mesh space.
NPH_INLINE NphVec3 nph_mesh_dequantize_position(const NphMeshHeader *h, NphMeshPosition p)
{
    const f32 k = 1.0F / 65535.0F;
    return nph_vec3(h->position_offset.x + (f32)p.x * k * h->position_scale.x,
                    h->position_offset.y + (f32)p.y * k * h->position_scale.y,
                    h->position_offset.z + (f32)p.z * k * h->position_scale.z);
}

#ifdef __cplusplus
}
#endif
