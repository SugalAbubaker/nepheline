// SPDX-License-Identifier: Apache-2.0
#pragma once

// Mesh cooking (CLAUDE.md §8.8 "Meshes: vertex quantization, vertex cache
// and overdraw optimization, meshletization, bounds. All written by us").
//
// A CookMesh is the importer's intermediate form: full-precision vertices
// (position, normal, tangent with handedness, one UV set, colour), a u32
// triangle list, and submeshes that map index ranges to material slots.
// cook_mesh_process runs the pipeline and writes a mesh asset
// (engine/asset/mesh.h):
//
//   1. validation and removal of degenerate triangles;
//   2. flat normals when the source has none (glTF 2.0 §3.7.2.1: "client
//      implementations MUST calculate flat normals"): every triangle gets
//      its own corners, which the weld below merges again where they are
//      identical;
//   3. welding of identical vertices (exact attribute match), which also
//      drops vertices no triangle references;
//   4. tangents when the source has UVs but no tangents, by the default
//      MikkTSpace rules (Mikkelsen, "Simulation of Wrinkled Surfaces
//      Revisited", 2008; the algorithm of the reference mikktspace.c):
//      per vertex, the triangles around it that share edges and UV
//      orientation average their projected, normalised dP/du weighted by
//      the corner angle; a vertex whose triangles form more than one such
//      group (a mirrored UV seam) is split;
//   5. vertex cache optimisation per submesh (Forsyth, "Linear-Speed
//      Vertex Cache Optimisation", 2006), then overdraw reduction by
//      sorting runs of triangles by their outward facing (the cluster
//      pass of Sander, Nehab & Barczak, "Fast Triangle Reordering for
//      Vertex Locality and Reduced Overdraw", SIGGRAPH 2007), then vertex
//      fetch optimisation (vertices in order of first use);
//   6. meshlets of at most 64 vertices and 124 triangles, grown over
//      triangle adjacency with a spatial and normal-cone score and seeded
//      by the nearest unused triangle (Kapoulkine, meshoptimizer's
//      clusterizer), independent of the index order step 5 chose;
//   7. quantisation to the runtime layout (16-bit unorm positions over
//      the bounds, octahedral normals, snorm8 tangents, half UVs, RGBA8
//      colours); every bound (mesh, submesh, meshlet sphere and cone) is
//      computed from the dequantised positions, which are the positions
//      the GPU culls and rasterises;
//   8. levels of detail where the mesh has more than one meshlet (CLAUDE.md
//      §8.6 stage 9; render/cluster_lod.h; docs/render.md "Levels of
//      detail"), built by the renderer's builder from the dequantised
//      positions and the quantised attributes (vertices at one position
//      whose records differ are a seam the levels keep): every level's
//      clusters go into the meshlet blobs, level 0's first, with the
//      levels' records beside them; with `voxels` set and the mesh's
//      materials given (cook/voxel.h), aggregate geometry's coarser levels
//      are voxel clusters (render/voxel.h; ADR-0061), their bricks and
//      voxels in VOXB and VOXA.
//
// The tangent convention is glTF's: bitangent = cross(normal, tangent.xyz)
// * tangent.w points along decreasing v (up in the image), because glTF
// normal textures have +Y up while its texture coordinates (like
// Vulkan's) grow downwards. Authored glTF tangents follow the same rule
// and are kept as they are.
//
// Every step is deterministic: the same input gives the same bytes on a
// given platform and build.
//
// Thread-safety: a CookMesh belongs to one thread.

#include <nepheline/asset/format.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/core/array.h>
#include <nepheline/math/vec.h>
#include <nepheline/render/cluster_lod.h>

#include <cook/cook.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CookVertex {
    NphVec3 position;
    NphVec3 normal;
    NphVec4 tangent; // xyz and handedness in w (+1 or -1)
    NphVec2 uv0;
    NphVec4 color; // linear RGBA (glTF COLOR_0 is linear)
} CookVertex;

typedef struct CookSubmesh {
    u32 index_offset;
    u32 index_count;
    u32 material_slot;
} CookSubmesh;

typedef enum CookMeshFlags {
    COOK_MESH_HAS_NORMALS = 1U << 0U,
    COOK_MESH_HAS_TANGENTS = 1U << 1U,
    COOK_MESH_HAS_UV0 = 1U << 2U,
    COOK_MESH_HAS_COLORS = 1U << 3U,
} CookMeshFlags;

typedef NPH_ARRAY(CookVertex) CookVertexArray;
typedef NPH_ARRAY(CookSubmesh) CookSubmeshArray;

#define COOK_MESH_NAME_MAX 128U

typedef struct CookMesh {
    NphAllocator *alloc;
    CookVertexArray vertices;
    NphArrayU32 indices;
    CookSubmeshArray submeshes;
    u32 flags; // CookMeshFlags: which attributes the vertices carry
    char name[COOK_MESH_NAME_MAX];
} CookMesh;

typedef struct CookMeshSettings {
    b32 no_optimize;         // keep the source triangle and vertex order (steps 5 skipped)
    u32 meshlet_vertices;    // 0: NPH_MESHLET_MAX_VERTICES
    u32 meshlet_triangles;   // 0: NPH_MESHLET_MAX_TRIANGLES
    f32 meshlet_cone_weight; // 0..1: how much normal-cone tightness counts against distance
    NphLzLevel lz;
    b32 no_lods; // level 0 alone: no levels of detail (step 8 skipped)
    // Voxel levels for aggregate geometry where the mesh's materials are
    // given (step 8; ADR-0061): off by default until the voxel raster draws
    // the reference scenes' foliage faster than their stalled triangles.
    b32 voxels;
} CookMeshSettings;

// Defaults: optimised, the format's meshlet limits, cone weight 0.25,
// HIGH compression.
void cook_mesh_settings_default(CookMeshSettings *out);

void cook_mesh_init(CookMesh *mesh, NphAllocator *alloc);
void cook_mesh_free(CookMesh *mesh);

// Appends a submesh: `vertices` are copied and `indices` (relative to
// them) are offset into the mesh's index list. NPH_RESULT_INVALID_ARGUMENT
// for an index out of range or a count that is not a multiple of three.
NPH_MUST_USE NphResult cook_mesh_add_submesh(CookMesh *mesh, const CookVertex *vertices,
                                             u32 vertex_count, const u32 *indices, u32 index_count,
                                             u32 material_slot);

// Positions beyond this magnitude (a million kilometres) are refused:
// below it every edge, cross product, and quantisation step the pipeline
// computes stays a finite float (16 * (1e9)^4 < FLT_MAX).
#define COOK_MESH_MAX_COORDINATE 1e9F

// Checks every index and submesh range. NPH_RESULT_CORRUPT when an index
// is out of range, a count is not a multiple of three, the submeshes do
// not tile the index list in order, or a vertex has a non-finite value or
// a position coordinate beyond COOK_MESH_MAX_COORDINATE.
NPH_MUST_USE NphResult cook_mesh_validate(const CookMesh *mesh);

// ---------------------------------------------------------------------------
// The pipeline steps, exposed for the tests and the editor; cook_mesh_process
// runs them in order.
// ---------------------------------------------------------------------------

// Removes triangles with a repeated index or zero area.
NPH_MUST_USE NphResult cook_mesh_remove_degenerate(CookMesh *mesh);

typedef enum CookNormalMode {
    COOK_NORMALS_FLAT = 0,   // the face normal on each triangle's own corners (glTF's rule)
    COOK_NORMALS_SMOOTH = 1, // area-weighted average over the triangles sharing a vertex
} CookNormalMode;

// Replaces every normal and sets COOK_MESH_HAS_NORMALS. FLAT gives every
// triangle its own three vertices (weld afterwards to share the identical
// ones); SMOOTH keeps the vertices.
NPH_MUST_USE NphResult cook_mesh_generate_normals(CookMesh *mesh, CookNormalMode mode);

// Merges vertices whose attributes are bitwise equal (attributes the
// flags do not claim are cleared first; -0 equals +0) and drops vertices
// no triangle references. Surviving vertices keep their relative order.
NPH_MUST_USE NphResult cook_mesh_weld(CookMesh *mesh);

// MikkTSpace tangents (see the header comment) from the normals and UVs;
// generates flat normals first when the mesh has none. Vertices whose
// corners need different tangents are split. NPH_RESULT_INVALID_ARGUMENT
// without UVs.
NPH_MUST_USE NphResult cook_mesh_generate_tangents(CookMesh *mesh);

// Step 5: cache order, overdraw order, and fetch order.
NPH_MUST_USE NphResult cook_mesh_optimize(CookMesh *mesh);

// ---------------------------------------------------------------------------
// Meshlets
// ---------------------------------------------------------------------------

typedef NPH_ARRAY(NphMeshlet) CookMeshletArray;
typedef NPH_ARRAY(NphMeshletBounds) CookMeshletBoundsArray;

typedef struct CookMeshlets {
    CookMeshletArray meshlets;
    CookMeshletBoundsArray bounds; // of the mesh's full-precision positions
    NphArrayU32 vertices;          // MLVX: mesh vertex indices
    NphArrayU8 triangles;          // MLTR: meshlet-local corners, three per triangle
} CookMeshlets;

void cook_meshlets_init(CookMeshlets *m, NphAllocator *alloc);
void cook_meshlets_free(CookMeshlets *m);

// Builds the meshlets of every submesh, in submesh order. `max_vertices`
// (3..64) and `max_triangles` (1..124) are the limits per meshlet, 0
// meaning the format's maximums; `cone_weight` (0..1) trades spatial
// compactness for narrower normal cones. NPH_RESULT_INVALID_ARGUMENT for
// limits outside those ranges.
NPH_MUST_USE NphResult cook_mesh_build_meshlets(const CookMesh *mesh, u32 max_vertices,
                                                u32 max_triangles, f32 cone_weight,
                                                CookMeshlets *out);

// The bounding sphere and normal cone of one meshlet: `vertices` are its
// MLVX entries (indices into `positions`), `triangles` its local corners.
// The cone is conservative for the test the GPU runs on the stored
// values: cull when dot(normalize(apex - camera), axis) >= cutoff, with
// axis and cutoff decoded from snorm8 (docs/cook.md derives it). A cone
// that cannot cull (normals spread too far) has a zero axis and cutoff 1.
void cook_meshlet_bounds(const NphVec3 *positions, const u32 *vertices, const u8 *triangles,
                         u32 triangle_count, NphMeshletBounds *out);

// ---------------------------------------------------------------------------
// Writing and the whole pipeline
// ---------------------------------------------------------------------------

struct CookVoxelMaterials; // cook/voxel.h

// The levels of detail of a quantised mesh (step 8) into `out`
// (initialised by the caller): its level-0 `meshlets` with `bounds` of the
// dequantised `positions`, and `attribs` as written, under `settings`'
// meshlet limits; voxels for its aggregate geometry where `settings` ask
// for them and `voxels` (one material per slot; may be NULL) are given.
NPH_MUST_USE NphResult cook_mesh_build_lods(const CookMesh *mesh, const CookMeshlets *meshlets,
                                            const NphVec3 *positions, const NphMeshAttrib *attribs,
                                            const NphMeshletBounds *bounds,
                                            const CookMeshSettings *settings,
                                            const struct CookVoxelMaterials *voxels,
                                            NphRenderClusterLods *out);

typedef struct CookMeshStats {
    u32 source_vertices;
    u32 source_triangles;
    u32 vertices;
    u32 triangles;
    u32 meshlets;   // level 0's
    u32 lod_levels; // levels of detail written (1: level 0 alone)
    // The coarsest level's size: what a distant instance draws at least.
    u32 lod_coarsest_triangles;
    u32 lod_coarsest_clusters;
    // The voxel clusters, bricks, and voxels written (0 without voxels).
    u32 voxel_clusters;
    u32 voxel_bricks;
    u32 voxels;
    f32 acmr_before; // average cache miss ratio (32-entry FIFO) per triangle
    f32 acmr_after;
} CookMeshStats;

// Quantises the mesh and writes the HEAD, POSN, ATTR, INDX, SUBM, MLET,
// MLBD, MLVX, and MLTR blobs into `writer`, and the levels of detail
// (step 8) unless `settings` say not to, with their voxels (VOXB, VOXA)
// where cook_mesh_build_lods makes some; the meshlet bounds written are
// recomputed from the dequantised positions. `stats`, where not NULL,
// receives the levels written (1: level 0 alone), the coarsest level's
// triangles and clusters, and the voxels'.
NPH_MUST_USE NphResult cook_mesh_write(const CookMesh *mesh, const CookMeshlets *meshlets,
                                       const CookMeshSettings *settings,
                                       const struct CookVoxelMaterials *voxels,
                                       NphAssetWriter *writer, CookMeshStats *stats);

// The whole pipeline: validate, clean, complete the attributes, optimise,
// build meshlets, write. `voxels` (may be NULL) are the mesh's materials
// for voxel levels. `stats` (may be NULL) receives what happened.
NPH_MUST_USE NphResult cook_mesh_process(CookMesh *mesh, const CookMeshSettings *settings,
                                         const struct CookVoxelMaterials *voxels,
                                         NphAssetWriter *writer, CookMeshStats *stats);

// The average cache miss ratio of a triangle list for a FIFO vertex cache
// of `cache_size` entries (at most 64): misses per triangle (3 is worst,
// 0.5 the ideal for a regular grid).
f32 cook_mesh_acmr(const u32 *indices, u32 index_count, u32 cache_size);

#ifdef __cplusplus
}
#endif
