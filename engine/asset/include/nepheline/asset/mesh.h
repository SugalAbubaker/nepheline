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

#define NPH_MESH_TYPE_VERSION 1U

#define NPH_MESH_TAG_HEADER           NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_MESH_TAG_POSITIONS        NPH_FOURCC('P', 'O', 'S', 'N')
#define NPH_MESH_TAG_ATTRIBS          NPH_FOURCC('A', 'T', 'T', 'R')
#define NPH_MESH_TAG_INDICES          NPH_FOURCC('I', 'N', 'D', 'X')
#define NPH_MESH_TAG_SUBMESHES        NPH_FOURCC('S', 'U', 'B', 'M')
#define NPH_MESH_TAG_MESHLETS         NPH_FOURCC('M', 'L', 'E', 'T')
#define NPH_MESH_TAG_MESHLET_BOUNDS   NPH_FOURCC('M', 'L', 'B', 'D')
#define NPH_MESH_TAG_MESHLET_VERTICES NPH_FOURCC('M', 'L', 'V', 'X')
#define NPH_MESH_TAG_MESHLET_TRIS     NPH_FOURCC('M', 'L', 'T', 'R')

#define NPH_MESHLET_MAX_VERTICES  64U
#define NPH_MESHLET_MAX_TRIANGLES 124U

typedef enum NphMeshFlags {
    NPH_MESH_FLAG_HAS_TANGENTS = 1U << 0U, // tangents were authored or generated
    NPH_MESH_FLAG_HAS_UV0 = 1U << 1U,
    NPH_MESH_FLAG_HAS_COLORS = 1U << 2U,
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
} NphMeshView;

// Resolves and validates the blobs of a READY mesh asset.
// NPH_RESULT_TYPE_MISMATCH for another asset type, NPH_RESULT_VERSION_MISMATCH
// for a newer schema, NPH_RESULT_CORRUPT when a blob is missing, too
// small, or a range points outside its array.
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
