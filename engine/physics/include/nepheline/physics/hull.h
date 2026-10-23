// SPDX-License-Identifier: Apache-2.0
#pragma once

// Convex hulls (CLAUDE.md §8.9 "Shapes"; docs/physics.md "Convex hulls"):
// the immutable polyhedra the hull shape collides with, and the Quickhull
// builder that makes them from a point cloud.
//
// A hull is one block of memory: the NphPhysicsHull header followed by its
// arrays, which the header locates by byte offsets rather than pointers. The
// block holds no pointer at all, so it can be copied, written to a cooked
// file, and used straight from a memory-mapped one (CLAUDE.md §8.8: no
// parsing step). Many shapes, in any number of worlds, may share one hull;
// it must outlive every shape that uses it.
//
// Topology is a half-edge mesh (Gregorius, "Implementing Quickhull", GDC
// 2014): half-edges come in twin pairs (2k, 2k + 1), each knows the next
// half-edge around its face (counter-clockwise seen from outside), the vertex
// it starts at, and its face. Faces are convex polygons, not only triangles:
// coplanar triangles are merged during the build, so a box has six faces and
// twelve edges, which is what the separating axis test and contact clipping
// want (docs/physics.md "Hull against hull").
//
// The builder is Quickhull (Barber, Dobkin, and Huhdanpaa, "The Quickhull
// Algorithm for Convex Hulls", ACM TOMS 1996) with face merging so the
// output is convex within the tolerance and free of slivers (Gregorius 2014;
// the merge rules of Lloyd's QuickHull3D). Points are added farthest first,
// so a vertex budget yields the hull of the most significant points: an
// inner approximation that loses the least volume.
//
// Thread-safety: building allocates from the caller's allocator; a finished
// hull is immutable and every read is NPH_THREADSAFE.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/physics/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// The most vertices a hull may have, and the builder's default budget.
// Collision cost grows with the vertex and edge counts (the edge-pair search
// of the separating axis test is quadratic in edges), so game hulls stay
// small; 64 vertices approximate any rounded shape to well under a percent.
#define NPH_PHYSICS_HULL_MAX_VERTICES         256U
#define NPH_PHYSICS_HULL_DEFAULT_MAX_VERTICES 64U

// A half-edge. `twin` is always the index with its lowest bit flipped.
typedef struct NphPhysicsHullEdge {
    u16 next;   // the next half-edge around `face`
    u16 twin;   // the opposite half-edge, which bounds the neighbouring face
    u16 origin; // the vertex the half-edge starts at
    u16 face;   // the face the half-edge bounds
} NphPhysicsHullEdge;

NPH_STATIC_ASSERT(sizeof(NphPhysicsHullEdge) == 8U, "a half-edge is four u16");

// The header. Offsets are in bytes from the start of the header.
typedef struct NphPhysicsHull {
    u32 byte_size; // the whole block, header included
    u32 vertex_count;
    u32 edge_count; // half-edges: twice the number of edges
    u32 face_count;
    u32 vertices_offset; // NphVec3[vertex_count]
    u32 planes_offset;   // NphPlane[face_count]: outward unit normals, n.p + d = 0 on the face
    u32 faces_offset;    // u16[face_count]: a half-edge of each face
    u32 edges_offset;    // NphPhysicsHullEdge[edge_count]
    NphAABB bounds;      // of the vertices
    NphVec3 centroid;    // of the solid: its centre of mass at uniform density
    f32 volume;
    NphMat3 inertia;  // about the centroid, for density 1 (kg m^2 per kg/m^3)
    f32 inner_radius; // the smallest distance from the centroid to a face plane
    f32 outer_radius; // the largest distance from the centroid to a vertex
    f32 tolerance;    // the build's distance tolerance
    u32 reserved[2];
} NphPhysicsHull;

NPH_STATIC_ASSERT(sizeof(NphPhysicsHull) == 128U,
                  "the header is 128 bytes, so the arrays after it stay aligned");

// The bytes at `offset` in the hull's block; the accessors below type them.
NPH_INLINE const void *nph_physics_hull_bytes(const NphPhysicsHull *h, u32 offset)
{
    return (const u8 *)h + offset;
}
NPH_INLINE const NphVec3 *nph_physics_hull_vertices(const NphPhysicsHull *h)
{
    const NphVec3 *v = nph_physics_hull_bytes(h, h->vertices_offset);
    return v;
}
NPH_INLINE const NphPlane *nph_physics_hull_planes(const NphPhysicsHull *h)
{
    const NphPlane *p = nph_physics_hull_bytes(h, h->planes_offset);
    return p;
}
NPH_INLINE const u16 *nph_physics_hull_faces(const NphPhysicsHull *h)
{
    const u16 *f = nph_physics_hull_bytes(h, h->faces_offset);
    return f;
}
NPH_INLINE const NphPhysicsHullEdge *nph_physics_hull_edges(const NphPhysicsHull *h)
{
    const NphPhysicsHullEdge *e = nph_physics_hull_bytes(h, h->edges_offset);
    return e;
}

typedef struct NphPhysicsHullDesc {
    const NphVec3 *points; // the cloud; need not be convex, unique, or ordered
    u32 point_count;       // at least 4
    // The vertex budget: 0 for NPH_PHYSICS_HULL_DEFAULT_MAX_VERTICES, at
    // most NPH_PHYSICS_HULL_MAX_VERTICES (at least 4).
    u32 max_vertices;
    // Distances below this count as zero (coplanar faces merge, points this
    // close to the hull are inside). 0 derives it from the cloud's extent:
    // 3 * FLT_EPSILON * (max|x| + max|y| + max|z|), the round-off bound of
    // a plane distance (Barber et al. 1996 §4; Gregorius 2014).
    f32 tolerance;
} NphPhysicsHullDesc;

// Builds the hull of desc->points into one block from `alloc`, which also
// provides the build's temporary memory (released before returning).
// NPH_RESULT_INVALID_ARGUMENT for fewer than 4 points, a non-finite
// coordinate, a bad budget, or a cloud with no volume (every point within
// the tolerance of one plane: use a box for a thin plate);
// NPH_RESULT_OUT_OF_MEMORY. Release the hull with nph_physics_hull_free and
// the same allocator. Deterministic: the same points give the same hull.
NPH_MUST_USE NphResult nph_physics_hull_build(const NphPhysicsHullDesc *desc, NphAllocator *alloc,
                                              NphPhysicsHull **out);

// The hull of an axis-aligned box centred on the origin, made directly (its
// exact topology, no Quickhull). NPH_RESULT_INVALID_ARGUMENT for a
// non-positive or non-finite half extent; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_physics_hull_make_box(NphVec3 half_extents, NphAllocator *alloc,
                                                 NphPhysicsHull **out);

// Releases a hull made by nph_physics_hull_build or _make_box. NULL is
// ignored.
void nph_physics_hull_free(NphPhysicsHull *hull, NphAllocator *alloc);

// The index of the vertex farthest along `direction` (the first of equals).
u32 nph_physics_hull_support(const NphPhysicsHull *hull, NphVec3 direction);

// The number of vertices of face `face`.
u32 nph_physics_hull_face_vertex_count(const NphPhysicsHull *hull, u32 face);

// Checks the invariants: twin pairing, closed face loops, every face's
// vertices on its plane and its loop convex, every vertex behind every
// plane, Euler's V - E + F = 2, and the header's offsets inside the block,
// all within `tolerance` (0: the hull's own). For tests and for hulls read
// from files. Logs the first violation.
bool nph_physics_hull_validate(const NphPhysicsHull *hull, f32 tolerance);

#ifdef __cplusplus
}
#endif
