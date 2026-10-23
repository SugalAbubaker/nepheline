// SPDX-License-Identifier: Apache-2.0
#pragma once

// Heightfields (CLAUDE.md §8.9 "Shapes"; docs/physics.md "Heightfields";
// ADR-0046 §4): terrain as a regular grid of heights, which a heightfield
// shape collides with, and the builder that makes one.
//
// A heightfield is one block of memory, like a mesh (mesh.h): the
// NphPhysicsHeightfield header and its arrays at byte offsets, with no
// pointer, so it can be copied, written to a cooked file, and used straight
// from a memory-mapped one (CLAUDE.md §8.8). Many shapes, in any number of
// worlds, may share one; it must outlive every shape that uses it.
//
// Sample (i, j), for column i < columns and row j < rows, stands at
// (i spacing_x, its height, j spacing_z) in the heightfield's frame. The
// cell between samples (i, j) and (i + 1, j + 1) is two triangles, split
// along the diagonal from (i, j + 1) to (i + 1, j), or along the other one
// when the cell is flipped; a hole has none. Cell c = j (columns - 1) + i
// holds triangles 2c and 2c + 1, the first the one with corner (i, j).
// Triangles collide from above (+Y): a heightfield is one-sided.
//
// The builder marks each triangle's active edges as a mesh's builder does
// (mesh.h; ADR-0046 §5): the grid's border, a hole's rim, and edges convex
// by more than a threshold are active; flat and concave ones are not, so
// bodies cross cells without bumping. It also keeps the lowest and highest
// height of each block of 8 x 8 cells: a box finds its cells by arithmetic,
// and a ray passes over whole blocks.
//
// Thread-safety: building allocates from the caller's allocator; a finished
// heightfield is immutable and every query is NPH_THREADSAFE.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/physics/mesh.h>
#include <nepheline/physics/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// The most samples along either axis (16384 cells: a cell's triangles' indices fit
// 32 bits), and a block's side, in cells.
#define NPH_PHYSICS_HEIGHTFIELD_MAX_SAMPLES 16385U
#define NPH_PHYSICS_HEIGHTFIELD_BLOCK       8U

// A cell's flags in the description: no triangles, or the other diagonal.
// In the block each cell's byte also holds its triangles' active edges
// (NPH_PHYSICS_MESH_EDGE_*): the first's in bits 2 to 4, the second's in
// bits 5 to 7.
enum {
    NPH_PHYSICS_HEIGHTFIELD_HOLE = 1U << 0U,
    NPH_PHYSICS_HEIGHTFIELD_FLIP = 1U << 1U,
};

// The header. Offsets are in bytes from the start of the header, each array
// 16-byte aligned.
typedef struct NphPhysicsHeightfield {
    u32 byte_size; // the whole block, header included
    u32 columns;   // samples along X
    u32 rows;      // samples along Z
    u32 flags;     // reserved: 0
    f32 spacing_x; // metres between samples along X
    f32 spacing_z;
    u32 heights_offset; // f32[columns * rows], sample (i, j) at j * columns + i
    u32 cells_offset;   // u8[(columns - 1) * (rows - 1)]: hole, flip, active edges
    u32 blocks_offset;  // f32[2 * block_columns * block_rows]: lowest, highest
    u32 block_columns;  // blocks of cells along X
    u32 block_rows;
    u32 reserved;
    NphAABB bounds; // of the samples
    u32 reserved2[2];
} NphPhysicsHeightfield;

NPH_STATIC_ASSERT(sizeof(NphPhysicsHeightfield) == 80U,
                  "the header is 80 bytes, so the arrays after it stay aligned");

NPH_INLINE const f32 *nph_physics_heightfield_heights(const NphPhysicsHeightfield *h)
{
    const f32 *p = (const void *)((const u8 *)h + h->heights_offset);
    return p;
}
NPH_INLINE const u8 *nph_physics_heightfield_cells(const NphPhysicsHeightfield *h)
{
    return (const u8 *)h + h->cells_offset;
}
// Block (bi, bj)'s lowest height at 2 (bj block_columns + bi), its highest
// after it; an all-hole block holds (FLT_MAX, -FLT_MAX).
NPH_INLINE const f32 *nph_physics_heightfield_blocks(const NphPhysicsHeightfield *h)
{
    const f32 *p = (const void *)((const u8 *)h + h->blocks_offset);
    return p;
}

// Cells and triangles: 2 (columns - 1) (rows - 1) triangles, holes'
// included (they have no vertices).
NPH_INLINE u32 nph_physics_heightfield_triangle_count(const NphPhysicsHeightfield *h)
{
    return 2U * (h->columns - 1U) * (h->rows - 1U);
}

// Triangle `t`'s vertices, counter-clockwise seen from above, and its
// active edges (edge e from vertex e to e + 1). False, and nothing
// written, for a hole's.
bool nph_physics_heightfield_triangle(const NphPhysicsHeightfield *h, u32 t, NphVec3 out[3],
                                      u32 *active_edges);

typedef struct NphPhysicsHeightfieldDesc {
    // columns * rows heights, sample (i, j) at j * columns + i; finite and
    // within NPH_PHYSICS_MAX_COORDINATE.
    const f32 *heights;
    u32 columns; // 2 to NPH_PHYSICS_HEIGHTFIELD_MAX_SAMPLES
    u32 rows;
    // Positive; the grid's extent along each axis within
    // NPH_PHYSICS_MAX_COORDINATE.
    f32 spacing_x;
    f32 spacing_z;
    // NULL, or (columns - 1) * (rows - 1) cells, cell (i, j) at
    // j (columns - 1) + i: NPH_PHYSICS_HEIGHTFIELD_HOLE and _FLIP.
    const u8 *cells;
    // Edges between triangles meeting at a convex angle above this
    // (radians) are active; 0 uses NPH_PHYSICS_MESH_DEFAULT_ACTIVE_ANGLE.
    f32 active_edge_angle;
} NphPhysicsHeightfieldDesc;

// Builds desc's grid into one block from `alloc`. NPH_RESULT_INVALID_ARGUMENT
// for counts or spacings out of range, a height that is not finite or past
// the coordinate limit, cell flags other than HOLE and FLIP, an angle
// outside [0, pi], or every cell a hole; NPH_RESULT_OUT_OF_MEMORY. Release
// it with nph_physics_heightfield_free and the same allocator.
// Deterministic: the same input gives the same bytes.
NPH_MUST_USE NphResult nph_physics_heightfield_build(const NphPhysicsHeightfieldDesc *desc,
                                                     NphAllocator *alloc,
                                                     NphPhysicsHeightfield **out);

// Releases a heightfield made by nph_physics_heightfield_build. NULL is
// ignored.
void nph_physics_heightfield_free(NphPhysicsHeightfield *heightfield, NphAllocator *alloc);

// Checks a block of `size` bytes before use (CLAUDE.md §8.8 "Robustness"):
// a 16-byte aligned header, `byte_size` within `size`, the counts and
// spacings in range, the arrays inside the block and aligned, the bounds
// the grid's own, every height finite and inside them, and every block's
// lowest and highest heights holding its cells' (or the empty pair for an
// all-hole block). A heightfield it accepts is safe for every function of
// the module. Never reads past `size`; logs the first violation.
bool nph_physics_heightfield_validate(const void *block, u32 size);

// ---------------------------------------------------------------------------
// Queries (in the heightfield's frame)
// ---------------------------------------------------------------------------

// Visits the triangles whose boxes overlap `box`, block by block and cell by
// cell in rows (deterministic); `visit` returning false ends the query.
void nph_physics_heightfield_overlap(const NphPhysicsHeightfield *h, NphAABB box,
                                     NphPhysicsMeshVisit visit, void *user);

// The closest triangle the segment from `origin` to origin + `delta`
// crosses from above, by the watertight test (Woop, Benthin, and Wald, JCGT
// 2013) on the cells the segment passes over, found by walking the blocks
// and then their cells (Amanatides and Woo, "A Fast Voxel Traversal
// Algorithm for Ray Tracing", Eurographics 1987). The hit's `original` is
// its triangle index and `back` is never set. False for no hit.
bool nph_physics_heightfield_raycast(const NphPhysicsHeightfield *h, NphVec3 origin, NphVec3 delta,
                                     NphPhysicsMeshHit *hit);

// The surface's height at (x, z) and its normal there (the triangle's
// under the point; NULL to skip). False outside the grid or over a hole.
bool nph_physics_heightfield_height_at(const NphPhysicsHeightfield *h, f32 x, f32 z, f32 *height,
                                       NphVec3 *normal);

#ifdef __cplusplus
}
#endif
