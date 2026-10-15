// SPDX-License-Identifier: Apache-2.0
#pragma once

// Procedural meshes in the renderer's vertex streams (renderer.h
// NphRenderVertexPosition, NphRenderVertexAttrib): the primitives of the
// editor (CLAUDE.md §8.13 "primitives (cube, sphere, plane, cylinder)"),
// the golden-image test scenes, and the samples.
//
// nph_render_mesh_data_build turns float vertices into the quantised
// streams the cooker writes: 16-bit unorm positions across the bounds,
// octahedral normals, snorm8 tangents, half-float UVs, RGBA8 colours.
// Tangents are generated from the UVs per triangle (Lengyel, "Computing
// Tangent Space Basis Vectors for an Arbitrary Mesh", 2001) with glTF's
// convention: the bitangent cross(n, t) * w points along decreasing v.
//
// UVs follow glTF: the origin is the top-left of the image.
//
// Costs (§1.4): every function allocates the streams from the given
// allocator; nph_render_mesh_data_free releases them.
// Thread-safety: NPH_THREADSAFE (no shared state).

#include <nepheline/render/renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphRenderVertexInput {
    NphVec3 position;
    NphVec3 normal; // unit length
    NphVec2 uv;
    NphVec4 color; // linear RGBA
} NphRenderVertexInput;

// A mesh's streams in memory, owned by `alloc`.
typedef struct NphRenderMeshData {
    NphAllocator *alloc;
    NphRenderVertexPosition *positions;
    NphRenderVertexAttrib *attribs;
    u32 *indices;
    u32 vertex_count;
    u32 index_count;
    NphVec3 position_offset;
    NphVec3 position_scale;
    NphAABB bounds;
} NphRenderMeshData;

// Quantises `vertices` and copies `indices` (a triangle list, CCW front
// faces). NPH_RESULT_INVALID_ARGUMENT for no vertices, an index count that
// is not a multiple of 3, an index out of range, or a non-finite
// position; NPH_RESULT_OUT_OF_MEMORY otherwise on failure.
NPH_MUST_USE NphResult nph_render_mesh_data_build(NphAllocator *alloc,
                                                  const NphRenderVertexInput *vertices,
                                                  u32 vertex_count, const u32 *indices,
                                                  u32 index_count, NphRenderMeshData *out);
void nph_render_mesh_data_free(NphRenderMeshData *data);

// A description of the whole mesh as one submesh with material slot 0.
// `submesh` receives the submesh record, which must outlive the call to
// nph_render_mesh_create.
void nph_render_mesh_data_desc(const NphRenderMeshData *data, const char *name,
                               NphRenderSubmesh *submesh, NphRenderMeshDesc *out);

// A cube of edge `size` centred on the origin; each face maps the whole
// texture.
NPH_MUST_USE NphResult nph_render_primitive_cube(NphAllocator *alloc, f32 size,
                                                 NphRenderMeshData *out);
// A UV sphere of `radius`: `segments` around (>= 3) and `rings` from pole
// to pole (>= 2); u wraps around, v runs from the north (+Y) pole.
NPH_MUST_USE NphResult nph_render_primitive_sphere(NphAllocator *alloc, f32 radius, u32 segments,
                                                   u32 rings, NphRenderMeshData *out);
// A square of edge `size` in the XZ plane facing +Y, split into
// `subdivisions` x `subdivisions` quads (>= 1), with UVs spanning
// [0, uv_scale].
NPH_MUST_USE NphResult nph_render_primitive_plane(NphAllocator *alloc, f32 size, u32 subdivisions,
                                                  f32 uv_scale, NphRenderMeshData *out);
// A capped cylinder of `radius` and `height` along Y centred on the
// origin, `segments` around (>= 3).
NPH_MUST_USE NphResult nph_render_primitive_cylinder(NphAllocator *alloc, f32 radius, f32 height,
                                                     u32 segments, NphRenderMeshData *out);

#ifdef __cplusplus
}
#endif
