// SPDX-License-Identifier: Apache-2.0
#pragma once

// Triangle meshes (CLAUDE.md §8.9 "Shapes"; docs/physics.md "Meshes";
// ADR-0046): the immutable triangle sets a mesh shape collides with, under a
// four-wide bounding volume hierarchy, and the builder that makes them.
//
// A mesh is one block of memory, like a hull (hull.h): the NphPhysicsMesh
// header followed by its arrays, located by byte offsets, with no pointer at
// all, so it can be copied, written to a cooked file, and used straight from
// a memory-mapped one (CLAUDE.md §8.8). Many shapes, in any number of
// worlds, may share one mesh; it must outlive every shape that uses it.
//
// A triangle collides from its front, the side its vertices run
// counter-clockwise from (its normal, right-handed), unless the mesh is
// two-sided. Each triangle keeps the index the builder's input gave it
// (`originals`), which queries report, so a game can find its material.
//
// The builder welds vertices closer than a tolerance, drops degenerate
// triangles, and marks each triangle's active edges (ADR-0046 §5): an edge
// is active when it bounds one triangle, more than two, or two meeting at a
// convex angle above a threshold (5 degrees by default). Flat and concave
// edges are inactive, and a contact that touches only inactive edges takes
// the triangle's own normal, so bodies slide across seams without bumping.
// The tree is built by binned SAH (Wald, "On Fast Construction of SAH-based
// Bounding Volume Hierarchies", IEEE RT 2007) with leaves of at most four
// triangles, then collapsed into four-wide nodes whose child boxes are laid
// out as structure-of-arrays, so a node meets a box or a ray in a few SIMD
// instructions (Dammertz, Hanika, and Keller, "Shallow Bounding Volume
// Hierarchies for Fast SIMD Ray Tracing of Incoherent Rays", EGSR 2008).
//
// Thread-safety: building allocates from the caller's allocator; a finished
// mesh is immutable and every query is NPH_THREADSAFE.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/physics/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// The most triangles a mesh may hold (a leaf stores its first triangle in
// 28 bits), and the most a leaf holds.
#define NPH_PHYSICS_MESH_MAX_TRIANGLES  (1U << 28U)
#define NPH_PHYSICS_MESH_LEAF_TRIANGLES 4U

// A triangle's active edges: edge i runs from its vertex i to vertex i + 1.
enum {
    NPH_PHYSICS_MESH_EDGE_0 = 1U << 0U,
    NPH_PHYSICS_MESH_EDGE_1 = 1U << 1U,
    NPH_PHYSICS_MESH_EDGE_2 = 1U << 2U,
    NPH_PHYSICS_MESH_EDGES = 7U,
};

// The mesh's flags.
enum {
    NPH_PHYSICS_MESH_TWO_SIDED = 1U << 0U,
};

// A node's child: an inner node's index, a leaf, or nothing.
#define NPH_PHYSICS_MESH_CHILD_LEAF  0x80000000U
#define NPH_PHYSICS_MESH_CHILD_EMPTY 0xFFFFFFFFU

// A leaf child holds its first triangle (in leaf order) and how many follow.
NPH_INLINE u32 nph_physics_mesh_leaf(u32 first, u32 count)
{
    return NPH_PHYSICS_MESH_CHILD_LEAF | (first << 2U) | (count - 1U);
}
NPH_INLINE bool nph_physics_mesh_child_is_leaf(u32 child)
{
    return child != NPH_PHYSICS_MESH_CHILD_EMPTY && (child & NPH_PHYSICS_MESH_CHILD_LEAF) != 0U;
}
NPH_INLINE u32 nph_physics_mesh_leaf_first(u32 child)
{
    return (child & ~NPH_PHYSICS_MESH_CHILD_LEAF) >> 2U;
}
NPH_INLINE u32 nph_physics_mesh_leaf_count(u32 child)
{
    return (child & 3U) + 1U;
}

// A node: its four children's boxes, structure-of-arrays, and the children.
// An empty child's box is inverted (min > max), so nothing overlaps it.
typedef struct NphPhysicsMeshNode {
    f32 min_x[4];
    f32 min_y[4];
    f32 min_z[4];
    f32 max_x[4];
    f32 max_y[4];
    f32 max_z[4];
    u32 child[4];
} NphPhysicsMeshNode;

NPH_STATIC_ASSERT(sizeof(NphPhysicsMeshNode) == 112U, "a node is seven rows of four");

// The header. Offsets are in bytes from the start of the header; node 0 is
// the root.
typedef struct NphPhysicsMesh {
    u32 byte_size; // the whole block, header included
    u32 vertex_count;
    u32 triangle_count;
    u32 node_count;
    u32 nodes_offset;     // NphPhysicsMeshNode[node_count], 16-byte aligned
    u32 vertices_offset;  // NphVec3[vertex_count]
    u32 triangles_offset; // u32[3 * triangle_count]: vertex indices, in leaf order
    u32 edges_offset;     // u8[triangle_count]: active edges (NPH_PHYSICS_MESH_EDGE_*)
    u32 originals_offset; // u32[triangle_count]: each triangle's index in the input
    u32 flags;            // NPH_PHYSICS_MESH_*
    NphAABB bounds;       // of the vertices
} NphPhysicsMesh;

NPH_STATIC_ASSERT(sizeof(NphPhysicsMesh) == 64U,
                  "the header is 64 bytes, so the nodes after it stay aligned");

NPH_INLINE const void *nph_physics_mesh_bytes(const NphPhysicsMesh *m, u32 offset)
{
    return (const u8 *)m + offset;
}
NPH_INLINE const NphPhysicsMeshNode *nph_physics_mesh_nodes(const NphPhysicsMesh *m)
{
    const NphPhysicsMeshNode *n = nph_physics_mesh_bytes(m, m->nodes_offset);
    return n;
}
NPH_INLINE const NphVec3 *nph_physics_mesh_vertices(const NphPhysicsMesh *m)
{
    const NphVec3 *v = nph_physics_mesh_bytes(m, m->vertices_offset);
    return v;
}
NPH_INLINE const u32 *nph_physics_mesh_triangles(const NphPhysicsMesh *m)
{
    const u32 *t = nph_physics_mesh_bytes(m, m->triangles_offset);
    return t;
}
NPH_INLINE const u8 *nph_physics_mesh_edges(const NphPhysicsMesh *m)
{
    const u8 *e = nph_physics_mesh_bytes(m, m->edges_offset);
    return e;
}
NPH_INLINE const u32 *nph_physics_mesh_originals(const NphPhysicsMesh *m)
{
    const u32 *o = nph_physics_mesh_bytes(m, m->originals_offset);
    return o;
}

// Triangle `t`'s vertices (in leaf order), counter-clockwise seen from its
// front.
void nph_physics_mesh_triangle(const NphPhysicsMesh *m, u32 t, NphVec3 out[3]);

typedef struct NphPhysicsMeshDesc {
    const NphVec3 *vertices; // finite
    u32 vertex_count;
    const u32 *indices; // three per triangle, each below vertex_count
    u32 triangle_count; // 1 to NPH_PHYSICS_MESH_MAX_TRIANGLES
    // Vertices closer than this become one, so the triangles that share
    // them become neighbours (a mesh exported with split normals repeats a
    // vertex for each face around it). 0 derives it from the extent: 1e-6
    // of the largest coordinate's magnitude; negative welds only exact
    // copies.
    f32 weld_tolerance;
    // Edges between neighbours meeting at a convex angle above this
    // (radians) are active; 0 uses NPH_PHYSICS_MESH_DEFAULT_ACTIVE_ANGLE.
    f32 active_edge_angle;
    b32 two_sided; // triangles collide from both sides
} NphPhysicsMeshDesc;

#define NPH_PHYSICS_MESH_DEFAULT_ACTIVE_ANGLE (5.0F * 3.14159265F / 180.0F)

// Builds desc's triangles into one block from `alloc`, which also provides
// the build's temporary memory (released before returning). Degenerate
// triangles (repeated vertices after welding, or no area) are dropped; at
// least one must remain. NPH_RESULT_INVALID_ARGUMENT for no triangles, an
// index out of range, a non-finite coordinate or setting, or nothing left
// after dropping; NPH_RESULT_OUT_OF_MEMORY. Release the mesh with
// nph_physics_mesh_free and the same allocator. Deterministic: the same
// input gives the same bytes.
NPH_MUST_USE NphResult nph_physics_mesh_build(const NphPhysicsMeshDesc *desc, NphAllocator *alloc,
                                              NphPhysicsMesh **out);

// Releases a mesh made by nph_physics_mesh_build. NULL is ignored.
void nph_physics_mesh_free(NphPhysicsMesh *mesh, NphAllocator *alloc);

// Checks a block of `size` bytes (at least the header) before use: the
// offsets and counts inside the block and aligned, every index in range,
// every child an inner node after its parent or a leaf within the
// triangles, every triangle in exactly one leaf, finite coordinates, and
// each child's box holding what it contains. For meshes read from files;
// never reads past `size`. Logs the first violation.
bool nph_physics_mesh_validate(const void *block, u32 size);

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

// Called for each triangle (in leaf order) whose box overlaps the query's;
// returning false ends the query.
typedef bool (*NphPhysicsMeshVisit)(void *user, u32 triangle);

// Visits the triangles whose boxes overlap `box` (in the mesh's frame), in
// tree order.
void nph_physics_mesh_overlap(const NphPhysicsMesh *m, NphAABB box, NphPhysicsMeshVisit visit,
                              void *user);

typedef struct NphPhysicsMeshHit {
    f32 fraction;   // along the ray, in [0, 1]
    NphVec3 normal; // the face's unit normal, towards the side the ray came from
    u32 triangle;   // in leaf order
    u32 original;   // the input's index
    b32 back;       // hit from behind (two-sided meshes only)
} NphPhysicsMeshHit;

// The closest triangle the segment from `origin` to origin + `delta` (the
// mesh's frame) crosses, by the watertight test of Woop, Benthin, and Wald
// ("Watertight Ray/Triangle Intersection", JCGT 2013): a ray through an edge
// or a vertex hits one of the triangles that share it, never none. A
// one-sided mesh is hit from the front only. False when nothing is hit.
bool nph_physics_mesh_raycast(const NphPhysicsMesh *m, NphVec3 origin, NphVec3 delta,
                              NphPhysicsMeshHit *hit);

#ifdef __cplusplus
}
#endif
