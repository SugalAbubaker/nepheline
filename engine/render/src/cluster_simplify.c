// SPDX-License-Identifier: Apache-2.0
// The simplification of one group of a cluster level (render/cluster_lod.h;
// docs/render.md "Levels of detail"; ADR-0060): edge collapses ranked by
// their quadric error (Garland & Heckbert, "Surface Simplification Using
// Quadric Error Metrics", SIGGRAPH 1997).
//
// Every collapse moves one vertex onto a neighbour (a half-edge collapse),
// so the result indexes the mesh's own vertices (and those added at complex
// classes) and no attribute but those added's texture coordinates is ever
// interpolated. Vertices are welded by position into classes for topology;
// the vertices of a class with one set of attributes are a wedge. Each class
// is of a kind, after the vertex kinds of meshoptimizer's simplifier
// (Kapoulkine, simplifier.cpp, 2017), which decides where it may go:
//
//   manifold  interior, one wedge: onto any neighbour
//   border    on an open edge (two of them): along one, onto the next
//   seam      on an attribute seam or a material border (two such edges,
//             one wedge each side): along the seam, each side's wedge onto
//             the neighbour's wedge on the same side
//   complex   anything else (corners of seams and open edges): along any
//             edge where the caller gives texture coordinates, else never.
//             A surface there the edge's triangles do not reach has no wedge
//             at the neighbour, so it takes one added there, its own
//             attributes at the neighbour's position and its texture
//             coordinates carried along its plane: hard edges stay hard and
//             textures do not smear (ADR-0069)
//   tangled   on a non-manifold edge: never
//   locked    shared with another group: never, so groups keep their
//             common boundary and the levels stay watertight
//
// A collapse must also keep the topology (the edge's endpoints share no
// neighbour but the opposite corners of its triangles: the link condition)
// and no triangle may turn over or collapse to a line, nor face away from
// the level-0 surface its corners stand for (the sums of the normals merged
// into their classes: a reference that does not drift as collapses add up
// over passes and levels). Collapses are made
// in passes: every class's cheapest valid collapse is found, the candidates
// are sorted, and they are made cheapest first, skipping those whose
// neighbourhood an earlier one of the pass changed, up to the cost of the
// collapse the target still needs; then the next pass. The quadrics
// accumulate from level 0 across every level, so an error is a distance
// from the mesh's own surface.
//
// A surface's quadrics do not see the border between two surfaces that look
// different move where they lie in one plane (a window in a wall, whose corner
// going turns its rectangle into a triangle), so a collapse also costs how far it
// moves the seams and open edges of the class that goes, those steps added up over
// the collapses that walk a corner along its edge. Nor do they see shading: where the
// caller gives normals, a collapse also costs what its triangles now show where the
// vertex stood, against the normal it had (after Hoppe, "New Quadric Metric for Simplifying
// Meshes with Appearance Attributes", IEEE Visualization 1999, whose attributes count in
// the error too). A normal that varies evenly across a surface comes back from the
// interpolation, so a true curve pays little for it, and a wrinkle in flat cloth pays its
// full size (ADR-0069).
#include <nepheline/core/array.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/scalar.h>

#include "cluster_lod_internal.h"

#include <stdlib.h>
#include <string.h>

// Open edges keep their shape through a perpendicular plane weighted by
// the edge's squared length times this (meshoptimizer weighs them alike).
#define BORDER_WEIGHT 10.0
// A collapse may turn no triangle's normal by more than about 75 degrees
// (this cosine): a bound on each step, since steps of a pass after pass add
// up, and a triangle that turns that far is nearly folded over its
// neighbour anyway.
#define FLIP_COSINE 0.25F
// A triangle a collapse leaves must face each of its corners' level-0
// normals within about 84 degrees (this cosine): it stands for the surface
// around each, and one that stands across it (a fin) or faces into it does
// not, however much area the normals it merges have.
#define REF_COSINE 0.1F
// How far down the sorted candidates a pass may reach, in multiples of the
// collapses its target still needs.
#define PASS_REACH 2U
// What a change of shading counts for, against a distance: the change of normal where a
// vertex stood (a chord), times the size of the triangle that now shows it, times this. A
// quarter is the least that left Bistro's images nearer level 0 than before, in every view,
// with fewer triangles (ADR-0069).
#define NORMAL_WEIGHT 0.25F
// Shading normals closer than this (a chord; about 0.06 degrees) are one normal: what
// their quantisation and the rounding of an interpolation leave.
#define NORMAL_EPSILON 1e-3F
// Texture coordinates are carried along a surface's plane from its largest
// triangle at the corner, and not from one so thin that the neighbour's position
// lies past this many of its edges.
#define UV_REACH 16.0F

// ---------------------------------------------------------------------------
// Quadrics
// ---------------------------------------------------------------------------

void lod_quadric_add_plane(LodQuadric *q, NphVec3 n, f64 d, f64 weight)
{
    const f64 a = (f64)n.x;
    const f64 b = (f64)n.y;
    const f64 c = (f64)n.z;
    q->a[0] += weight * a * a;
    q->a[1] += weight * a * b;
    q->a[2] += weight * a * c;
    q->a[3] += weight * a * d;
    q->a[4] += weight * b * b;
    q->a[5] += weight * b * c;
    q->a[6] += weight * b * d;
    q->a[7] += weight * c * c;
    q->a[8] += weight * c * d;
    q->a[9] += weight * d * d;
    q->w += weight;
}

void lod_quadric_add(LodQuadric *q, const LodQuadric *o)
{
    for (u32 i = 0; i < 10U; ++i) {
        q->a[i] += o->a[i];
    }
    q->w += o->w;
}

f64 lod_quadric_error(const LodQuadric *q, NphVec3 p)
{
    if (!(q->w > 0.0)) {
        return 0.0;
    }
    const f64 x = (f64)p.x;
    const f64 y = (f64)p.y;
    const f64 z = (f64)p.z;
    // p^T A p + 2 b^T p + c, A = [a0 a1 a2; a1 a4 a5; a2 a5 a7],
    // b = (a3, a6, a8), c = a9.
    const f64 e = x * (q->a[0] * x + 2.0 * (q->a[1] * y + q->a[2] * z + q->a[3])) +
                  y * (q->a[4] * y + 2.0 * (q->a[5] * z + q->a[6])) +
                  z * (q->a[7] * z + 2.0 * q->a[8]) + q->a[9];
    return e > 0.0 ? e / q->w : 0.0;
}

// ---------------------------------------------------------------------------
// The welded mesh
// ---------------------------------------------------------------------------

// A position as hash-map key: -0 and +0 are one position.
typedef struct PositionKey {
    f32 x, y, z;
} PositionKey;

static PositionKey position_key(NphVec3 p)
{
    PositionKey k;
    k.x = p.x + 0.0F;
    k.y = p.y + 0.0F;
    k.z = p.z + 0.0F;
    return k;
}

static NphVec3 relative(const LodMesh *m, u32 v)
{
    return nph_vec3_sub(m->positions[v], m->origin);
}

// The plane of triangle (a, b, c) in the quadrics' frame, and its area:
// false for a triangle without one.
static bool triangle_plane(const LodMesh *m, u32 a, u32 b, u32 c, NphVec3 *n, f64 *d, f64 *area)
{
    const NphVec3 pa = relative(m, a);
    const NphVec3 cross =
        nph_vec3_cross(nph_vec3_sub(relative(m, b), pa), nph_vec3_sub(relative(m, c), pa));
    const f32 len = nph_vec3_length(cross);
    if (!(len > 0.0F)) {
        return false;
    }
    *n = nph_vec3_scale(cross, 1.0F / len);
    *d = -(f64)nph_vec3_dot(*n, pa);
    *area = 0.5 * (f64)len;
    return true;
}

// Classes: the first vertex at each position names it, numbered densely in
// vertex order.
static NphResult weld_positions(LodMesh *m, NphAllocator *alloc)
{
    NphHashMapDesc d;
    memset(&d, 0, sizeof d);
    d.key_size = (u32)sizeof(PositionKey);
    d.val_size = (u32)sizeof(u32);
    NphHashMap map;
    NPH_TRY(nph_hashmap_init(&map, alloc, &d));
    NphResult r = nph_hashmap_reserve(&map, m->vertex_count);
    for (u32 v = 0; v < m->vertex_count && r == NPH_RESULT_OK; ++v) {
        const PositionKey k = position_key(m->positions[v]);
        const u32 *found = NPH_HASHMAP_FIND(&map, u32, &k);
        if (found != NULL) {
            m->class_of[v] = *found;
        } else {
            const u32 c = m->class_count;
            r = nph_hashmap_insert(&map, &k, &c);
            m->class_of[v] = c;
            m->class_count += 1U;
        }
    }
    nph_hashmap_free(&map);
    return r;
}

// Wedges: within each class, the vertices whose attributes are the same
// bytes are one, named by its first vertex.
static NphResult weld_wedges(LodMesh *m, NphAllocator *alloc, const void *attribs, u32 stride)
{
    const u32 n = m->vertex_count;
    const u32 classes = m->class_count;
    for (u32 v = 0; v < n; ++v) {
        m->wedge_of[v] = v;
    }
    if (attribs == NULL || stride == 0U || n == 0U) {
        return NPH_RESULT_OK;
    }
    // The vertices of each class, in vertex order (a counting sort).
    u32 *start = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, (u64)classes + 1U);
    u32 *members = NPH_ALLOC_ARRAY(alloc, u32, n);
    if (start == NULL || members == NULL) {
        if (start != NULL) {
            NPH_FREE_ARRAY(alloc, u32, start, (u64)classes + 1U);
        }
        if (members != NULL) {
            NPH_FREE_ARRAY(alloc, u32, members, n);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 v = 0; v < n; ++v) {
        start[m->class_of[v] + 1U] += 1U;
    }
    for (u32 c = 0; c < classes; ++c) {
        start[c + 1U] += start[c];
    }
    for (u32 v = 0; v < n; ++v) {
        const u32 c = m->class_of[v];
        members[start[c]] = v;
        start[c] += 1U;
    }
    // start[c] now ends class c's run; its beginning is start[c - 1].
    const u8 *bytes = (const u8 *)attribs;
    u32 begin = 0U;
    for (u32 c = 0; c < classes; ++c) {
        const u32 end = start[c];
        for (u32 i = begin; i < end; ++i) {
            const u32 v = members[i];
            for (u32 j = begin; j < i; ++j) {
                const u32 u = members[j];
                if (m->wedge_of[u] == u &&
                    memcmp(bytes + (u64)v * stride, bytes + (u64)u * stride, stride) == 0) {
                    m->wedge_of[v] = u;
                    break;
                }
            }
        }
        begin = end;
    }
    NPH_FREE_ARRAY(alloc, u32, start, (u64)classes + 1U);
    NPH_FREE_ARRAY(alloc, u32, members, n);
    return NPH_RESULT_OK;
}

// The level-0 quadrics: triangle planes, then the perpendicular planes of
// open edges (an edge between two classes that one triangle alone has).
static NphResult build_quadrics(LodMesh *m, NphAllocator *alloc, const u32 *indices,
                                u32 index_count)
{
    const u32 tris = index_count / 3U;
    for (u32 t = 0; t < tris; ++t) {
        const u32 a = indices[(u64)t * 3U];
        const u32 b = indices[(u64)t * 3U + 1U];
        const u32 c = indices[(u64)t * 3U + 2U];
        NphVec3 n;
        f64 d = 0.0;
        f64 area = 0.0;
        if (!triangle_plane(m, a, b, c, &n, &d, &area)) {
            continue;
        }
        lod_quadric_add_plane(&m->quadrics[m->class_of[a]], n, d, area);
        lod_quadric_add_plane(&m->quadrics[m->class_of[b]], n, d, area);
        lod_quadric_add_plane(&m->quadrics[m->class_of[c]], n, d, area);
        const NphVec3 weighted = nph_vec3_scale(n, (f32)area);
        m->normals[m->class_of[a]] = nph_vec3_add(m->normals[m->class_of[a]], weighted);
        m->normals[m->class_of[b]] = nph_vec3_add(m->normals[m->class_of[b]], weighted);
        m->normals[m->class_of[c]] = nph_vec3_add(m->normals[m->class_of[c]], weighted);
    }
    // The triangles around each class, to count each edge's triangles.
    const u32 classes = m->class_count;
    u32 *start = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, (u64)classes + 1U);
    u32 *around = NPH_ALLOC_ARRAY(alloc, u32, nph_max_u32(index_count, 1U));
    NphResult r = start != NULL && around != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 i = 0; i < index_count; ++i) {
            start[m->class_of[indices[i]] + 1U] += 1U;
        }
        for (u32 c = 0; c < classes; ++c) {
            start[c + 1U] += start[c];
        }
        for (u32 i = 0; i < index_count; ++i) {
            const u32 c = m->class_of[indices[i]];
            around[start[c]] = i / 3U;
            start[c] += 1U;
        }
        // start[c] ends class c's run; it began at start[c - 1] (0 for c 0).
        for (u32 t = 0; t < tris; ++t) {
            const u32 *tri = &indices[(u64)t * 3U];
            NphVec3 n;
            f64 d = 0.0;
            f64 area = 0.0;
            if (!triangle_plane(m, tri[0], tri[1], tri[2], &n, &d, &area)) {
                continue;
            }
            for (u32 k = 0; k < 3U; ++k) {
                const u32 ca = m->class_of[tri[k]];
                const u32 cb = m->class_of[tri[(k + 1U) % 3U]];
                if (ca == cb) {
                    continue;
                }
                // The triangles around ca that also have cb.
                u32 sharing = 0U;
                const u32 first = ca == 0U ? 0U : start[ca - 1U];
                for (u32 i = first; i < start[ca]; ++i) {
                    const u32 *o = &indices[(u64)around[i] * 3U];
                    if (m->class_of[o[0]] == cb || m->class_of[o[1]] == cb ||
                        m->class_of[o[2]] == cb) {
                        sharing += 1U;
                    }
                }
                if (sharing != 1U) {
                    continue;
                }
                const NphVec3 pa = relative(m, tri[k]);
                const NphVec3 edge = nph_vec3_sub(relative(m, tri[(k + 1U) % 3U]), pa);
                const NphVec3 side = nph_vec3_normalize_or_zero(nph_vec3_cross(edge, n));
                if (nph_vec3_dot(side, side) == 0.0F) {
                    continue;
                }
                const f64 w = (f64)nph_vec3_dot(edge, edge) * BORDER_WEIGHT;
                const f64 sd = -(f64)nph_vec3_dot(side, pa);
                lod_quadric_add_plane(&m->quadrics[ca], side, sd, w);
                lod_quadric_add_plane(&m->quadrics[cb], side, sd, w);
            }
        }
    }
    if (start != NULL) {
        NPH_FREE_ARRAY(alloc, u32, start, (u64)classes + 1U);
    }
    if (around != NULL) {
        NPH_FREE_ARRAY(alloc, u32, around, nph_max_u32(index_count, 1U));
    }
    return r;
}

NphResult lod_mesh_add_vertex(LodMesh *m, NphAllocator *alloc, u32 mesh_class)
{
    if (m->vertex_count == m->vertex_capacity) {
        const u32 capacity = m->vertex_capacity * 2U;
        u32 *class_of = NPH_ALLOC_ARRAY(alloc, u32, capacity);
        u32 *wedge_of = NPH_ALLOC_ARRAY(alloc, u32, capacity);
        if (class_of == NULL || wedge_of == NULL) {
            if (class_of != NULL) {
                NPH_FREE_ARRAY(alloc, u32, class_of, capacity);
            }
            if (wedge_of != NULL) {
                NPH_FREE_ARRAY(alloc, u32, wedge_of, capacity);
            }
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        memcpy(class_of, m->class_of, (size_t)m->vertex_count * sizeof(u32));
        memcpy(wedge_of, m->wedge_of, (size_t)m->vertex_count * sizeof(u32));
        NPH_FREE_ARRAY(alloc, u32, m->class_of, m->vertex_capacity);
        NPH_FREE_ARRAY(alloc, u32, m->wedge_of, m->vertex_capacity);
        m->class_of = class_of;
        m->wedge_of = wedge_of;
        m->vertex_capacity = capacity;
    }
    const u32 v = m->vertex_count;
    m->class_of[v] = mesh_class;
    m->wedge_of[v] = v;
    m->vertex_count = v + 1U;
    return NPH_RESULT_OK;
}

void lod_mesh_free(LodMesh *m, NphAllocator *alloc)
{
    const u32 n = nph_max_u32(m->vertex_capacity, 1U);
    const u32 c = nph_max_u32(m->class_count, 1U);
    if (m->class_of != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->class_of, n);
    }
    if (m->wedge_of != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->wedge_of, n);
    }
    if (m->quadrics != NULL) {
        NPH_FREE_ARRAY(alloc, LodQuadric, m->quadrics, c);
    }
    if (m->normals != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec3, m->normals, c);
    }
    if (m->class_vertex != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->class_vertex, c);
    }
    if (m->absorbed_next != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->absorbed_next, c);
    }
    if (m->absorbed_tail != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->absorbed_tail, c);
    }
    if (m->moved != NULL) {
        NPH_FREE_ARRAY(alloc, f32, m->moved, c);
    }
    memset(m, 0, sizeof *m);
}

NphResult lod_mesh_init(LodMesh *m, NphAllocator *alloc, const NphVec3 *positions,
                        const void *attribs, u32 attrib_stride, u32 vertex_count,
                        const u32 *indices, u32 index_count)
{
    memset(m, 0, sizeof *m);
    m->positions = positions;
    m->vertex_count = vertex_count;
    const u32 n = nph_max_u32(vertex_count, 1U);
    m->vertex_capacity = n;
    m->class_of = NPH_ALLOC_ARRAY(alloc, u32, n);
    m->wedge_of = NPH_ALLOC_ARRAY(alloc, u32, n);
    if (m->class_of == NULL || m->wedge_of == NULL) {
        lod_mesh_free(m, alloc);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    // The centre of the bounds: the quadrics' origin.
    if (vertex_count != 0U) {
        NphVec3 lo = positions[0];
        NphVec3 hi = positions[0];
        for (u32 v = 1U; v < vertex_count; ++v) {
            lo = nph_vec3_min(lo, positions[v]);
            hi = nph_vec3_max(hi, positions[v]);
        }
        m->origin = nph_vec3_scale(nph_vec3_add(lo, hi), 0.5F);
    }
    NphResult r = weld_positions(m, alloc);
    if (r == NPH_RESULT_OK) {
        r = weld_wedges(m, alloc, attribs, attrib_stride);
    }
    if (r == NPH_RESULT_OK) {
        const u32 c = nph_max_u32(m->class_count, 1U);
        m->quadrics = NPH_ALLOC_ARRAY_ZEROED(alloc, LodQuadric, c);
        m->normals = NPH_ALLOC_ARRAY_ZEROED(alloc, NphVec3, c);
        m->class_vertex = NPH_ALLOC_ARRAY(alloc, u32, c);
        m->absorbed_next = NPH_ALLOC_ARRAY(alloc, u32, c);
        m->absorbed_tail = NPH_ALLOC_ARRAY(alloc, u32, c);
        m->moved = NPH_ALLOC_ARRAY_ZEROED(alloc, f32, c);
        r = m->quadrics != NULL && m->normals != NULL && m->class_vertex != NULL &&
                    m->absorbed_next != NULL && m->absorbed_tail != NULL && m->moved != NULL
                ? NPH_RESULT_OK
                : NPH_RESULT_OUT_OF_MEMORY;
        if (r == NPH_RESULT_OK) {
            memset(m->class_vertex, 0xFF, (size_t)c * sizeof(u32));
            memset(m->absorbed_next, 0xFF, (size_t)c * sizeof(u32));
            for (u32 k = 0; k < m->class_count; ++k) {
                m->absorbed_tail[k] = k;
            }
            for (u32 v = 0; v < vertex_count; ++v) {
                if (m->class_vertex[m->class_of[v]] == NPH_U32_MAX) {
                    m->class_vertex[m->class_of[v]] = v;
                }
            }
        }
    }
    if (r == NPH_RESULT_OK) {
        r = build_quadrics(m, alloc, indices, index_count);
    }
    if (r != NPH_RESULT_OK) {
        lod_mesh_free(m, alloc);
    }
    return r;
}

// ---------------------------------------------------------------------------
// One group
// ---------------------------------------------------------------------------

typedef enum ClassKind {
    KIND_MANIFOLD = 0,
    KIND_BORDER = 1,
    KIND_SEAM = 2,
    KIND_COMPLEX = 3,
    KIND_LOCKED = 4,
    KIND_TANGLED = 5,
} ClassKind;

typedef enum EdgeKind {
    EDGE_SMOOTH = 0, // two triangles, opposite ways round, one wedge at each end, one submesh
    EDGE_SEAM = 1,   // two triangles, opposite ways round, otherwise not
    EDGE_OPEN = 2,   // one triangle
    EDGE_COMPLEX = 3,
} EdgeKind;

// One corner's edge, from its class to the next corner's.
typedef struct HalfEdge {
    u64 key;  // the classes' pair, lower first
    u32 tri;  // the triangle
    u32 from; // its corner (0..2) the edge leaves
} HalfEdge;

// A collapse of class `from` onto class `to`: which wedge of `to` each
// (wedge of `from`, submesh) takes, or one added there (REMAP_ADD).
#define MAX_REMAP 8U
#define REMAP_ADD 0xFFFFFFFFU
typedef struct Collapse {
    f64 cost;
    u32 from;
    u32 to;
    f32 moved; // how far the borders `to` takes on have come, this collapse included
    u32 remap_count;
    u32 remap_wedge[MAX_REMAP];
    u32 remap_submesh[MAX_REMAP];
    u32 remap_to[MAX_REMAP];
} Collapse;

typedef NPH_ARRAY(HalfEdge) HalfEdgeArray;
typedef NPH_ARRAY(Collapse) CollapseArray;

typedef struct Simp {
    NphAllocator *alloc;
    LodMesh *mesh;
    const u8 *locked;
    const NphVec2 *uvs;     // per vertex of the mesh (NULL: no vertex is added)
    const NphVec3 *normals; // per vertex of the mesh, or NULL
    b32 borders;
    // Local wedges (their first vertex, or LOD_ADDED_BASE and their place in
    // `added`) and classes (their mesh class).
    NphArrayU32 wedge_vertex;
    NphArrayU32 wedge_class; // local class
    NPH_ARRAY(NphVec2) wedge_uv;
    NphRenderLodAddedVertexArray added;
    NphArrayU32 class_mesh;  // mesh class
    NphVec3 *class_position; // relative to the mesh's origin
    u8 *kind;
    u8 *touched;
    u8 *touched_ever; // collapsed into another class during this group
    u32 *mark;        // per local class: scratch for neighbour sets
    // Triangles: three local wedges each, and their submeshes.
    u32 *tri;
    u32 *sub;
    u8 *dead;
    u32 tri_count;
    u32 live;
    // Around each local class: its live triangles (rebuilt each pass).
    u32 *around_start; // class_count + 1
    u32 *around;
    // Each class's half-edges by the sorted edge list: [start, end).
    HalfEdgeArray edges;
    u32 *edge_start;         // class_count + 1, into class_edges
    NphArrayU32 class_edges; // indices of edge runs touching each class
    NphArrayU32 run_start;   // the sorted edge list's runs (one per class pair)
    u8 *run_kind;
    CollapseArray candidates;
} Simp;

static u32 tri_class(const Simp *s, u32 t, u32 k)
{
    return s->wedge_class.data[s->tri[(u64)t * 3U + k]];
}

static NphVec3 class_pos(const Simp *s, u32 c)
{
    return s->class_position[c];
}

static int compare_edges(const void *a, const void *b)
{
    const HalfEdge *x = (const HalfEdge *)a;
    const HalfEdge *y = (const HalfEdge *)b;
    if (x->key != y->key) {
        return x->key < y->key ? -1 : 1;
    }
    if (x->tri != y->tri) {
        return x->tri < y->tri ? -1 : 1;
    }
    return x->from < y->from ? -1 : (x->from > y->from ? 1 : 0);
}

static int compare_collapses(const void *a, const void *b)
{
    const Collapse *x = (const Collapse *)a;
    const Collapse *y = (const Collapse *)b;
    if (x->cost != y->cost) {
        return x->cost < y->cost ? -1 : 1;
    }
    return x->from < y->from ? -1 : (x->from > y->from ? 1 : 0);
}

// The live triangles around each class, and every edge's kind.
static NphResult build_topology(Simp *s)
{
    const u32 classes = s->class_mesh.len;
    memset(s->around_start, 0, ((size_t)classes + 1U) * sizeof(u32));
    for (u32 t = 0; t < s->tri_count; ++t) {
        if (s->dead[t] != 0U) {
            continue;
        }
        for (u32 k = 0; k < 3U; ++k) {
            s->around_start[tri_class(s, t, k) + 1U] += 1U;
        }
    }
    for (u32 c = 0; c < classes; ++c) {
        s->around_start[c + 1U] += s->around_start[c];
    }
    for (u32 t = 0; t < s->tri_count; ++t) {
        if (s->dead[t] != 0U) {
            continue;
        }
        for (u32 k = 0; k < 3U; ++k) {
            const u32 c = tri_class(s, t, k);
            s->around[s->around_start[c]] = t;
            s->around_start[c] += 1U;
        }
    }
    for (u32 c = classes; c > 0U; --c) {
        s->around_start[c] = s->around_start[c - 1U];
    }
    s->around_start[0] = 0U;

    // Half-edges sorted by their classes' pair: each run is one edge.
    NPH_ARRAY_CLEAR(&s->edges);
    for (u32 t = 0; t < s->tri_count; ++t) {
        if (s->dead[t] != 0U) {
            continue;
        }
        for (u32 k = 0; k < 3U; ++k) {
            const u32 a = tri_class(s, t, k);
            const u32 b = tri_class(s, t, (k + 1U) % 3U);
            HalfEdge e;
            e.key = a < b ? ((u64)a << 32U) | b : ((u64)b << 32U) | a;
            e.tri = t;
            e.from = k;
            NPH_TRY(NPH_ARRAY_PUSH(&s->edges, e));
        }
    }
    if (s->edges.len > 1U) {
        qsort(s->edges.data, s->edges.len, sizeof(HalfEdge), compare_edges);
    }
    NPH_ARRAY_CLEAR(&s->run_start);
    for (u32 i = 0; i < s->edges.len; ++i) {
        if (i == 0U || s->edges.data[i].key != s->edges.data[i - 1U].key) {
            NPH_TRY(NPH_ARRAY_PUSH(&s->run_start, i));
        }
    }
    const u32 runs = s->run_start.len;
    NPH_TRY(NPH_ARRAY_PUSH(&s->run_start, s->edges.len));
    for (u32 r = 0; r < runs; ++r) {
        const u32 first = s->run_start.data[r];
        const u32 n = s->run_start.data[r + 1U] - first;
        EdgeKind kind = EDGE_COMPLEX;
        if (n == 1U) {
            kind = EDGE_OPEN;
        } else if (n == 2U) {
            const HalfEdge *e0 = &s->edges.data[first];
            const HalfEdge *e1 = &s->edges.data[first + 1U];
            const u32 *t0 = &s->tri[(u64)e0->tri * 3U];
            const u32 *t1 = &s->tri[(u64)e1->tri * 3U];
            const u32 a0 = t0[e0->from];
            const u32 b0 = t0[(e0->from + 1U) % 3U];
            const u32 a1 = t1[e1->from];
            const u32 b1 = t1[(e1->from + 1U) % 3U];
            const u32 ca0 = s->wedge_class.data[a0];
            const u32 ca1 = s->wedge_class.data[a1];
            if (ca0 != ca1) {
                // Opposite ways round: e1 runs from b0's class to a0's.
                const bool same = a0 == b1 && b0 == a1 && s->sub[e0->tri] == s->sub[e1->tri];
                kind = same ? EDGE_SMOOTH : EDGE_SEAM;
            }
        }
        s->run_kind[r] = (u8)kind;
    }
    // Each class's runs.
    memset(s->edge_start, 0, ((size_t)classes + 1U) * sizeof(u32));
    for (u32 r = 0; r < runs; ++r) {
        const u64 key = s->edges.data[s->run_start.data[r]].key;
        s->edge_start[(u32)(key >> 32U) + 1U] += 1U;
        s->edge_start[(u32)(key & 0xFFFFFFFFU) + 1U] += 1U;
    }
    for (u32 c = 0; c < classes; ++c) {
        s->edge_start[c + 1U] += s->edge_start[c];
    }
    NPH_TRY(NPH_ARRAY_RESIZE(&s->class_edges, s->edge_start[classes]));
    for (u32 r = 0; r < runs; ++r) {
        const u64 key = s->edges.data[s->run_start.data[r]].key;
        const u32 a = (u32)(key >> 32U);
        const u32 b = (u32)(key & 0xFFFFFFFFU);
        s->class_edges.data[s->edge_start[a]] = r;
        s->edge_start[a] += 1U;
        s->class_edges.data[s->edge_start[b]] = r;
        s->edge_start[b] += 1U;
    }
    for (u32 c = classes; c > 0U; --c) {
        s->edge_start[c] = s->edge_start[c - 1U];
    }
    s->edge_start[0] = 0U;

    // Each class's kind.
    for (u32 c = 0; c < classes; ++c) {
        if (s->locked[s->class_mesh.data[c]] != 0U) {
            s->kind[c] = (u8)KIND_LOCKED;
            continue;
        }
        u32 open = 0U;
        u32 seam = 0U;
        u32 complex = 0U;
        for (u32 i = s->edge_start[c]; i < s->edge_start[c + 1U]; ++i) {
            const u8 k = s->run_kind[s->class_edges.data[i]];
            open += k == (u8)EDGE_OPEN ? 1U : 0U;
            seam += k == (u8)EDGE_SEAM ? 1U : 0U;
            complex += k == (u8)EDGE_COMPLEX ? 1U : 0U;
        }
        // Its distinct wedges among its live corners.
        u32 w0 = NPH_U32_MAX;
        u32 w1 = NPH_U32_MAX;
        u32 wedges = 0U;
        for (u32 i = s->around_start[c]; i < s->around_start[c + 1U]; ++i) {
            const u32 t = s->around[i];
            for (u32 k = 0; k < 3U; ++k) {
                const u32 w = s->tri[(u64)t * 3U + k];
                if (s->wedge_class.data[w] != c || w == w0 || w == w1) {
                    continue;
                }
                if (w0 == NPH_U32_MAX) {
                    w0 = w;
                } else if (w1 == NPH_U32_MAX) {
                    w1 = w;
                }
                wedges += 1U;
            }
        }
        ClassKind kind = KIND_TANGLED;
        if (complex == 0U) {
            kind = KIND_COMPLEX;
            if (open == 0U && seam == 0U && wedges == 1U) {
                kind = KIND_MANIFOLD;
            } else if (open == 2U && seam == 0U && wedges == 1U) {
                kind = KIND_BORDER;
            } else if (seam == 2U && open == 0U && wedges <= 2U) {
                kind = KIND_SEAM;
            }
        }
        s->kind[c] = (u8)kind;
    }
    return NPH_RESULT_OK;
}

// The other class of edge run `r` from class `c`.
static u32 run_other(const Simp *s, u32 r, u32 c)
{
    const u64 key = s->edges.data[s->run_start.data[r]].key;
    const u32 a = (u32)(key >> 32U);
    return a == c ? (u32)(key & 0xFFFFFFFFU) : a;
}

static bool tri_has_class(const Simp *s, u32 t, u32 c)
{
    return tri_class(s, t, 0U) == c || tri_class(s, t, 1U) == c || tri_class(s, t, 2U) == c;
}

// The shading normal of local wedge `w`: an added vertex keeps its source's.
static NphVec3 wedge_normal(const Simp *s, u32 w)
{
    const u32 v = s->wedge_vertex.data[w];
    return s->normals[v >= LOD_ADDED_BASE ? s->added.data[v - LOD_ADDED_BASE].source : v];
}

// What a triangle of corners `q`, its normals `n`, shows at its point `at`, against `was`,
// over the triangle's size: a change of shading in the units of the mesh.
static f32 shown_change(const NphVec3 q[3], const NphVec3 n[3], NphVec3 at, NphVec3 was)
{
    const NphVec3 e0 = nph_vec3_sub(q[1], q[0]);
    const NphVec3 e1 = nph_vec3_sub(q[2], q[0]);
    const NphVec3 e2 = nph_vec3_sub(at, q[0]);
    const f32 d00 = nph_vec3_dot(e0, e0);
    const f32 d01 = nph_vec3_dot(e0, e1);
    const f32 d11 = nph_vec3_dot(e1, e1);
    const f32 d20 = nph_vec3_dot(e2, e0);
    const f32 d21 = nph_vec3_dot(e2, e1);
    const f32 denom = d00 * d11 - d01 * d01;
    if (!(denom > 0.0F)) {
        return 0.0F;
    }
    const f32 b1 = (d11 * d20 - d01 * d21) / denom;
    const f32 b2 = (d00 * d21 - d01 * d20) / denom;
    const f32 b0 = 1.0F - b1 - b2;
    const NphVec3 mixed = nph_vec3_add(
        nph_vec3_add(nph_vec3_scale(n[0], b0), nph_vec3_scale(n[1], b1)), nph_vec3_scale(n[2], b2));
    const f32 chord = nph_vec3_distance(nph_vec3_normalize_or_zero(mixed), was);
    if (chord < NORMAL_EPSILON) {
        return 0.0F;
    }
    const f32 d22 = nph_vec3_length_sq(nph_vec3_sub(q[2], q[1]));
    return chord * nph_f32_sqrt(nph_f32_max(nph_f32_max(d00, d11), d22));
}

// Whether `from` may collapse onto `to` (their edge run `r`), its cost, and
// the wedges' remap. False where the collapse breaks a rule.
static bool evaluate(Simp *s, u32 from, u32 to, u32 r, Collapse *out)
{
    // Compared as the bytes they are stored in (a ClassKind and an EdgeKind).
    const u32 kind = s->kind[from];
    const u32 edge = s->run_kind[r];
    const bool adding = kind == (u32)KIND_COMPLEX && s->uvs != NULL;
    if ((kind == (u32)KIND_MANIFOLD && edge != (u32)EDGE_SMOOTH) ||
        (kind == (u32)KIND_BORDER && edge != (u32)EDGE_OPEN) ||
        (kind == (u32)KIND_SEAM && edge != (u32)EDGE_SEAM) ||
        (kind == (u32)KIND_COMPLEX && (!adding || edge == (u32)EDGE_COMPLEX)) ||
        kind == (u32)KIND_LOCKED || kind == (u32)KIND_TANGLED) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->from = from;
    out->to = to;
    // The edge's triangles: the remap, and their opposite corners.
    u32 opposite[2] = {NPH_U32_MAX, NPH_U32_MAX};
    u32 opposites = 0U;
    for (u32 i = s->around_start[from]; i < s->around_start[from + 1U]; ++i) {
        const u32 t = s->around[i];
        if (!tri_has_class(s, t, to)) {
            continue;
        }
        u32 wf = 0U;
        u32 wt = 0U;
        u32 third = NPH_U32_MAX;
        for (u32 k = 0; k < 3U; ++k) {
            const u32 w = s->tri[(u64)t * 3U + k];
            const u32 c = s->wedge_class.data[w];
            if (c == from) {
                wf = w;
            } else if (c == to) {
                wt = w;
            } else {
                third = c;
            }
        }
        bool known = false;
        for (u32 m = 0; m < out->remap_count; ++m) {
            if (out->remap_wedge[m] == wf && out->remap_submesh[m] == s->sub[t]) {
                if (out->remap_to[m] != wt) {
                    // One region of `from` meeting two of `to`.
                    if (!adding) {
                        return false;
                    }
                    out->remap_to[m] = REMAP_ADD;
                }
                known = true;
            }
        }
        if (!known) {
            if (out->remap_count == MAX_REMAP) {
                return false;
            }
            out->remap_wedge[out->remap_count] = wf;
            out->remap_submesh[out->remap_count] = s->sub[t];
            out->remap_to[out->remap_count] = wt;
            out->remap_count += 1U;
        }
        if (opposites < 2U) {
            opposite[opposites] = third;
        }
        opposites += 1U;
    }
    if (opposites == 0U || opposites > 2U) {
        return false;
    }
    // The link condition: the classes next to both are the opposite corners.
    for (u32 i = s->edge_start[to]; i < s->edge_start[to + 1U]; ++i) {
        s->mark[run_other(s, s->class_edges.data[i], to)] = 1U;
    }
    bool ok = true;
    for (u32 i = s->edge_start[from]; i < s->edge_start[from + 1U] && ok; ++i) {
        const u32 c = run_other(s, s->class_edges.data[i], from);
        if (c != to && s->mark[c] != 0U && c != opposite[0] && c != opposite[1]) {
            ok = false;
        }
    }
    for (u32 i = s->edge_start[to]; i < s->edge_start[to + 1U]; ++i) {
        s->mark[run_other(s, s->class_edges.data[i], to)] = 0U;
    }
    if (!ok) {
        return false;
    }
    // The triangles that stay: every region of `from` maps onto `to`, none
    // turns over, and none becomes one the edge's opposite corners already
    // close (a tetrahedron's collapse).
    const NphVec3 p_to = class_pos(s, to);
    const NphVec3 p_from = class_pos(s, from);
    // Per surface of `from`: what its nearest triangle shows where `from` stood.
    f32 shown_distance[MAX_REMAP];
    f32 shown[MAX_REMAP];
    for (u32 m = 0; m < MAX_REMAP; ++m) {
        shown_distance[m] = NPH_F32_MAX;
        shown[m] = 0.0F;
    }
    for (u32 i = s->around_start[from]; i < s->around_start[from + 1U]; ++i) {
        const u32 t = s->around[i];
        if (tri_has_class(s, t, to)) {
            continue;
        }
        u32 corner = 0U;
        u32 others[2] = {0U, 0U};
        u32 o = 0U;
        for (u32 k = 0; k < 3U; ++k) {
            const u32 c = tri_class(s, t, k);
            if (c == from) {
                corner = k;
            } else if (o < 2U) {
                others[o] = c;
                o += 1U;
            }
        }
        const u32 wf = s->tri[(u64)t * 3U + corner];
        bool mapped = false;
        for (u32 m = 0; m < out->remap_count; ++m) {
            mapped = mapped || (out->remap_wedge[m] == wf && out->remap_submesh[m] == s->sub[t]);
        }
        if (!mapped) {
            // A surface the edge's triangles do not reach.
            if (!adding || out->remap_count == MAX_REMAP) {
                return false;
            }
            out->remap_wedge[out->remap_count] = wf;
            out->remap_submesh[out->remap_count] = s->sub[t];
            out->remap_to[out->remap_count] = REMAP_ADD;
            out->remap_count += 1U;
        }
        if (opposites == 2U && ((others[0] == opposite[0] && others[1] == opposite[1]) ||
                                (others[0] == opposite[1] && others[1] == opposite[0]))) {
            return false;
        }
        const NphVec3 p0 = class_pos(s, tri_class(s, t, 0U));
        const NphVec3 p1 = class_pos(s, tri_class(s, t, 1U));
        const NphVec3 p2 = class_pos(s, tri_class(s, t, 2U));
        NphVec3 q0 = p0;
        NphVec3 q1 = p1;
        NphVec3 q2 = p2;
        if (corner == 0U) {
            q0 = p_to;
        } else if (corner == 1U) {
            q1 = p_to;
        } else {
            q2 = p_to;
        }
        const NphVec3 n_old = nph_vec3_cross(nph_vec3_sub(p1, p0), nph_vec3_sub(p2, p0));
        const NphVec3 n_new = nph_vec3_cross(nph_vec3_sub(q1, q0), nph_vec3_sub(q2, q0));
        const f32 lo = nph_vec3_length(n_old);
        const f32 ln = nph_vec3_length(n_new);
        if (!(ln > 0.0F) || nph_vec3_dot(n_old, n_new) <= FLIP_COSINE * lo * ln) {
            return false;
        }
        // The level-0 surface the triangle will stand for, at each of its
        // corners: the corner's merged normals (`from`'s into `to`'s), each
        // of which it must face within REF_COSINE. A sum over the corners
        // would let one corner that has merged much outweigh the others, and
        // a triangle facing across the surface (a fin) or into it pass.
        const NphVec3 *normals = s->mesh->normals;
        const NphVec3 corner_normals[3] = {
            nph_vec3_add(normals[s->class_mesh.data[from]], normals[s->class_mesh.data[to]]),
            normals[s->class_mesh.data[others[0]]], normals[s->class_mesh.data[others[1]]]};
        for (u32 k = 0; k < 3U; ++k) {
            const f32 lr = nph_vec3_length(corner_normals[k]);
            if (lr > 0.0F && nph_vec3_dot(corner_normals[k], n_new) <= REF_COSINE * lr * ln) {
                return false;
            }
        }
        if (s->normals != NULL) {
            const NphVec3 at = nph_closest_point_on_triangle(q0, q1, q2, p_from);
            const f32 distance = nph_vec3_distance(at, p_from);
            for (u32 m = 0; m < out->remap_count; ++m) {
                if (out->remap_wedge[m] == wf && out->remap_submesh[m] == s->sub[t] &&
                    distance < shown_distance[m]) {
                    const NphVec3 was = wedge_normal(s, wf);
                    const NphVec3 corners[3] = {q0, q1, q2};
                    NphVec3 n[3];
                    for (u32 k = 0; k < 3U; ++k) {
                        n[k] = k != corner ? wedge_normal(s, s->tri[(u64)t * 3U + k])
                               : out->remap_to[m] != REMAP_ADD ? wedge_normal(s, out->remap_to[m])
                                                               : was;
                    }
                    shown_distance[m] = distance;
                    shown[m] = shown_change(corners, n, at, was);
                }
            }
        }
    }
    LodQuadric q = s->mesh->quadrics[s->class_mesh.data[from]];
    lod_quadric_add(&q, &s->mesh->quadrics[s->class_mesh.data[to]]);
    // Each seam or open edge of `from` comes to run from `to`: `from` stood this far
    // from where it now runs (the file's comment).
    f32 step = 0.0F;
    bool bordered = false;
    for (u32 i = s->edge_start[from]; i < s->edge_start[from + 1U] && s->borders != 0; ++i) {
        const u32 run = s->class_edges.data[i];
        const u32 other = run_other(s, run, from);
        if ((s->run_kind[run] == (u8)EDGE_SEAM || s->run_kind[run] == (u8)EDGE_OPEN) &&
            other != to) {
            const NphVec3 at =
                nph_closest_point_on_segment(p_to, class_pos(s, other), p_from, NULL);
            step = nph_f32_max(step, nph_vec3_distance(p_from, at));
            bordered = true;
        }
    }
    for (u32 m = 0; m < out->remap_count; ++m) {
        step = nph_f32_max(step, NORMAL_WEIGHT * shown[m]);
    }
    out->moved = bordered || step > 0.0F ? s->mesh->moved[s->class_mesh.data[from]] + step : 0.0F;
    out->cost = nph_f64_max(lod_quadric_error(&q, p_to), (f64)out->moved * (f64)out->moved);
    return true;
}

// The texture coordinates a surface of class `from` (its wedge, in its submesh)
// has where class `to` stands: its largest triangle there, carried along its
// plane. The wedge's own where no triangle holds them.
static NphVec2 carried_uv(const Simp *s, u32 from, u32 to, u32 wedge, u32 submesh)
{
    const NphVec3 q = class_pos(s, to);
    NphVec2 out = s->wedge_uv.data[wedge];
    f32 largest = 0.0F;
    for (u32 i = s->around_start[from]; i < s->around_start[from + 1U]; ++i) {
        const u32 t = s->around[i];
        if (s->dead[t] != 0U || s->sub[t] != submesh || tri_has_class(s, t, to)) {
            continue;
        }
        for (u32 k = 0; k < 3U; ++k) {
            if (s->tri[(u64)t * 3U + k] != wedge) {
                continue;
            }
            const u32 wb = s->tri[(u64)t * 3U + (k + 1U) % 3U];
            const u32 wc = s->tri[(u64)t * 3U + (k + 2U) % 3U];
            const NphVec3 pa = class_pos(s, s->wedge_class.data[wedge]);
            const NphVec3 e1 = nph_vec3_sub(class_pos(s, s->wedge_class.data[wb]), pa);
            const NphVec3 e2 = nph_vec3_sub(class_pos(s, s->wedge_class.data[wc]), pa);
            const NphVec3 n = nph_vec3_cross(e1, e2);
            const f32 area = nph_vec3_dot(n, n);
            if (!(area > largest)) {
                break;
            }
            const NphVec3 d = nph_vec3_sub(q, pa);
            const f32 a = nph_vec3_dot(nph_vec3_cross(d, e2), n) / area;
            const f32 b = nph_vec3_dot(nph_vec3_cross(e1, d), n) / area;
            if (nph_f32_abs(a) <= UV_REACH && nph_f32_abs(b) <= UV_REACH) {
                const NphVec2 own = s->wedge_uv.data[wedge];
                const NphVec2 ub = s->wedge_uv.data[wb];
                const NphVec2 uc = s->wedge_uv.data[wc];
                out = nph_vec2(own.x + a * (ub.x - own.x) + b * (uc.x - own.x),
                               own.y + a * (ub.y - own.y) + b * (uc.y - own.y));
                largest = area;
            }
            break;
        }
    }
    return out;
}

// A wedge added to class `to` for a surface of `from`.
static NphResult add_wedge(Simp *s, u32 from, u32 to, u32 wedge, u32 submesh, u32 *out)
{
    const NphVec2 uv = carried_uv(s, from, to, wedge, submesh);
    const u32 v = s->wedge_vertex.data[wedge];
    NphRenderLodAddedVertex a;
    a.source = v >= LOD_ADDED_BASE ? s->added.data[v - LOD_ADDED_BASE].source : v;
    a.position = s->mesh->class_vertex[s->class_mesh.data[to]];
    a.uv = uv;
    const u32 k = s->added.len;
    NPH_TRY(NPH_ARRAY_PUSH(&s->added, a));
    *out = s->wedge_vertex.len;
    const u32 added_vertex = LOD_ADDED_BASE + k;
    NPH_TRY(NPH_ARRAY_PUSH(&s->wedge_vertex, added_vertex));
    NPH_TRY(NPH_ARRAY_PUSH(&s->wedge_class, to));
    return NPH_ARRAY_PUSH(&s->wedge_uv, uv);
}

// Makes collapse `in`: the edge's triangles go, the others take `to`'s
// wedges, and `to`'s class takes `from`'s quadric.
static NphResult apply(Simp *s, const Collapse *in)
{
    Collapse resolved = *in;
    const Collapse *c = &resolved;
    for (u32 m = 0; m < resolved.remap_count; ++m) {
        if (resolved.remap_to[m] == REMAP_ADD) {
            NPH_TRY(add_wedge(s, resolved.from, resolved.to, resolved.remap_wedge[m],
                              resolved.remap_submesh[m], &resolved.remap_to[m]));
        }
    }
    for (u32 i = s->around_start[c->from]; i < s->around_start[c->from + 1U]; ++i) {
        const u32 t = s->around[i];
        if (s->dead[t] != 0U) {
            continue;
        }
        if (tri_has_class(s, t, c->to)) {
            s->dead[t] = 1U;
            s->live -= 1U;
            continue;
        }
        for (u32 k = 0; k < 3U; ++k) {
            const u32 w = s->tri[(u64)t * 3U + k];
            if (s->wedge_class.data[w] != c->from) {
                continue;
            }
            for (u32 m = 0; m < c->remap_count; ++m) {
                if (c->remap_wedge[m] == w && c->remap_submesh[m] == s->sub[t]) {
                    s->tri[(u64)t * 3U + k] = c->remap_to[m];
                    break;
                }
            }
        }
    }
    lod_quadric_add(&s->mesh->quadrics[s->class_mesh.data[c->to]],
                    &s->mesh->quadrics[s->class_mesh.data[c->from]]);
    s->mesh->normals[s->class_mesh.data[c->to]] = nph_vec3_add(
        s->mesh->normals[s->class_mesh.data[c->to]], s->mesh->normals[s->class_mesh.data[c->from]]);
    s->mesh->moved[s->class_mesh.data[c->to]] =
        nph_f32_max(s->mesh->moved[s->class_mesh.data[c->to]], c->moved);
    // `from`'s list after `to`'s.
    LodMesh *m = s->mesh;
    const u32 gt = s->class_mesh.data[c->to];
    const u32 gf = s->class_mesh.data[c->from];
    m->absorbed_next[m->absorbed_tail[gt]] = gf;
    m->absorbed_tail[gt] = m->absorbed_tail[gf];
    // What the pass may no longer touch: both classes and every neighbour
    // of the one that went (their triangles changed under them).
    s->touched[c->from] = 1U;
    s->touched[c->to] = 1U;
    s->touched_ever[c->from] = 1U;
    for (u32 i = s->edge_start[c->from]; i < s->edge_start[c->from + 1U]; ++i) {
        s->touched[run_other(s, s->class_edges.data[i], c->from)] = 1U;
    }
    return NPH_RESULT_OK;
}

static void simp_free(Simp *s)
{
    NphAllocator *a = s->alloc;
    const u32 classes = nph_max_u32(s->class_mesh.len, 1U);
    const u32 tris = nph_max_u32(s->tri_count, 1U);
    if (s->class_position != NULL) {
        NPH_FREE_ARRAY(a, NphVec3, s->class_position, classes);
    }
    if (s->kind != NULL) {
        NPH_FREE_ARRAY(a, u8, s->kind, classes);
    }
    if (s->touched != NULL) {
        NPH_FREE_ARRAY(a, u8, s->touched, classes);
    }
    if (s->touched_ever != NULL) {
        NPH_FREE_ARRAY(a, u8, s->touched_ever, classes);
    }
    if (s->mark != NULL) {
        NPH_FREE_ARRAY(a, u32, s->mark, classes);
    }
    if (s->around_start != NULL) {
        NPH_FREE_ARRAY(a, u32, s->around_start, (u64)classes + 1U);
    }
    if (s->edge_start != NULL) {
        NPH_FREE_ARRAY(a, u32, s->edge_start, (u64)classes + 1U);
    }
    if (s->tri != NULL) {
        NPH_FREE_ARRAY(a, u32, s->tri, (u64)tris * 3U);
    }
    if (s->around != NULL) {
        NPH_FREE_ARRAY(a, u32, s->around, (u64)tris * 3U);
    }
    if (s->dead != NULL) {
        NPH_FREE_ARRAY(a, u8, s->dead, tris);
    }
    if (s->run_kind != NULL) {
        NPH_FREE_ARRAY(a, u8, s->run_kind, (u64)tris * 3U);
    }
    NPH_ARRAY_FREE(&s->wedge_vertex);
    NPH_ARRAY_FREE(&s->wedge_class);
    NPH_ARRAY_FREE(&s->wedge_uv);
    NPH_ARRAY_FREE(&s->added);
    NPH_ARRAY_FREE(&s->class_mesh);
    NPH_ARRAY_FREE(&s->edges);
    NPH_ARRAY_FREE(&s->class_edges);
    NPH_ARRAY_FREE(&s->run_start);
    NPH_ARRAY_FREE(&s->candidates);
}

// The group's corners as local wedges, numbered in the order the corners
// reach them, and their classes likewise (hash maps of the group's own, so
// that groups may be simplified at once).
static NphResult simp_number(Simp *s, const LodSimplifyInput *in, NphHashMap *wedges,
                             NphHashMap *classes)
{
    const LodMesh *m = in->mesh;
    const u32 corners = in->triangle_count * 3U;
    // About a wedge a triangle where the surface is closed; more grow.
    NPH_TRY(nph_hashmap_reserve(wedges, in->triangle_count + 16U));
    NPH_TRY(nph_hashmap_reserve(classes, in->triangle_count + 16U));
    for (u32 i = 0; i < corners; ++i) {
        const u32 v = m->wedge_of[in->indices[i]];
        const u32 *known = (const u32 *)nph_hashmap_find_u64(wedges, v);
        if (known != NULL) {
            s->tri[i] = *known;
            continue;
        }
        const u32 c = m->class_of[v];
        const u32 *known_class = (const u32 *)nph_hashmap_find_u64(classes, c);
        u32 local_class = s->class_mesh.len;
        if (known_class != NULL) {
            local_class = *known_class;
        } else {
            NPH_TRY(nph_hashmap_insert_u64(classes, c, &local_class));
            NPH_TRY(NPH_ARRAY_PUSH(&s->class_mesh, c));
        }
        const u32 local_wedge = s->wedge_vertex.len;
        NPH_TRY(nph_hashmap_insert_u64(wedges, v, &local_wedge));
        NPH_TRY(NPH_ARRAY_PUSH(&s->wedge_vertex, v));
        NPH_TRY(NPH_ARRAY_PUSH(&s->wedge_class, local_class));
        const NphVec2 uv = s->uvs != NULL ? s->uvs[v] : nph_vec2(0.0F, 0.0F);
        NPH_TRY(NPH_ARRAY_PUSH(&s->wedge_uv, uv));
        s->tri[i] = local_wedge;
    }
    return NPH_RESULT_OK;
}

// The group's local wedges and classes.
static NphResult simp_init(Simp *s, const LodSimplifyInput *in)
{
    LodMesh *m = in->mesh;
    // Set first: simp_free sizes the triangle arrays by it.
    s->tri_count = in->triangle_count;
    s->tri = NPH_ALLOC_ARRAY(s->alloc, u32, (u64)nph_max_u32(in->triangle_count, 1U) * 3U);
    if (s->tri == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphHashMap wedges;
    NphHashMap class_map;
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));
    NphResult r = nph_hashmap_init(&wedges, s->alloc, &desc);
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&class_map, s->alloc, &desc);
        if (r == NPH_RESULT_OK) {
            r = simp_number(s, in, &wedges, &class_map);
            nph_hashmap_free(&class_map);
        }
        nph_hashmap_free(&wedges);
    }
    NPH_TRY(r);
    NPH_ASSERT(in->triangle_count == 0U || s->wedge_class.data != NULL);
    const u32 classes = nph_max_u32(s->class_mesh.len, 1U);
    const u32 tris = nph_max_u32(in->triangle_count, 1U);
    s->class_position = NPH_ALLOC_ARRAY(s->alloc, NphVec3, classes);
    s->kind = NPH_ALLOC_ARRAY(s->alloc, u8, classes);
    s->touched = NPH_ALLOC_ARRAY(s->alloc, u8, classes);
    s->touched_ever = NPH_ALLOC_ARRAY_ZEROED(s->alloc, u8, classes);
    s->mark = NPH_ALLOC_ARRAY_ZEROED(s->alloc, u32, classes);
    s->around_start = NPH_ALLOC_ARRAY(s->alloc, u32, (u64)classes + 1U);
    s->edge_start = NPH_ALLOC_ARRAY(s->alloc, u32, (u64)classes + 1U);
    s->around = NPH_ALLOC_ARRAY(s->alloc, u32, (u64)tris * 3U);
    s->dead = NPH_ALLOC_ARRAY_ZEROED(s->alloc, u8, tris);
    // An edge run per half-edge at most.
    s->run_kind = NPH_ALLOC_ARRAY(s->alloc, u8, (u64)tris * 3U);
    if (s->class_position == NULL || s->kind == NULL || s->touched == NULL ||
        s->touched_ever == NULL || s->mark == NULL || s->around_start == NULL ||
        s->edge_start == NULL || s->around == NULL || s->dead == NULL || s->run_kind == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 c = 0; c < s->class_mesh.len; ++c) {
        s->class_position[c] = nph_vec3_zero();
    }
    for (u32 w = 0; w < s->wedge_vertex.len; ++w) {
        s->class_position[s->wedge_class.data[w]] = relative(m, s->wedge_vertex.data[w]);
    }
    s->live = 0U;
    for (u32 t = 0; t < in->triangle_count; ++t) {
        // A triangle with two corners in one class covers nothing.
        const u32 a = tri_class(s, t, 0U);
        const u32 b = tri_class(s, t, 1U);
        const u32 c = tri_class(s, t, 2U);
        if (a == b || b == c || a == c) {
            s->dead[t] = 1U;
        } else {
            s->live += 1U;
        }
    }
    return NPH_RESULT_OK;
}

static NphResult simp_begin(Simp *s, const LodSimplifyInput *in, const NphVec2 *uvs)
{
    memset(s, 0, sizeof *s);
    s->alloc = in->scratch;
    s->mesh = in->mesh;
    s->locked = in->locked;
    s->uvs = uvs;
    s->normals = in->normals;
    s->borders = in->borders;
    s->sub = in->submeshes;
    NPH_ARRAY_INIT(&s->wedge_vertex, s->alloc);
    NPH_ARRAY_INIT(&s->wedge_class, s->alloc);
    NPH_ARRAY_INIT(&s->wedge_uv, s->alloc);
    NPH_ARRAY_INIT(&s->added, s->alloc);
    NPH_ARRAY_INIT(&s->class_mesh, s->alloc);
    NPH_ARRAY_INIT(&s->edges, s->alloc);
    NPH_ARRAY_INIT(&s->class_edges, s->alloc);
    NPH_ARRAY_INIT(&s->run_start, s->alloc);
    NPH_ARRAY_INIT(&s->candidates, s->alloc);
    return simp_init(s, in);
}

// Class `c`'s cheapest valid collapse, where it has one.
static bool cheapest_collapse(Simp *s, u32 c, Collapse *best)
{
    bool found = false;
    for (u32 i = s->edge_start[c]; i < s->edge_start[c + 1U]; ++i) {
        const u32 run = s->class_edges.data[i];
        Collapse cand;
        if (evaluate(s, c, run_other(s, run, c), run, &cand) &&
            (!found || cand.cost < best->cost || (cand.cost == best->cost && cand.to < best->to))) {
            *best = cand;
            found = true;
        }
    }
    return found;
}

NphResult lod_simplify_costs(const LodSimplifyInput *in, LodCostArray *costs)
{
    NPH_ASSERT(in != NULL && in->mesh != NULL && costs != NULL);
    Simp s;
    NphResult r = simp_begin(&s, in, in->uvs);
    if (r == NPH_RESULT_OK) {
        r = build_topology(&s);
    }
    for (u32 c = 0; c < s.class_mesh.len && r == NPH_RESULT_OK; ++c) {
        Collapse best;
        if (s.around_start[c] != s.around_start[c + 1U] && cheapest_collapse(&s, c, &best)) {
            r = NPH_ARRAY_PUSH(costs, best.cost);
        }
    }
    simp_free(&s);
    return r;
}

NphResult lod_simplify(const LodSimplifyInput *in, u32 *out_triangles, f32 *out_error)
{
    NPH_ASSERT(in != NULL && in->mesh != NULL && out_triangles != NULL && out_error != NULL);
    *out_triangles = in->triangle_count;
    *out_error = 0.0F;
    Simp s;
    NphResult r = simp_begin(&s, in, in->added != NULL ? in->uvs : NULL);
    f64 worst = 0.0;
    u32 collapses = 0U;
    while (r == NPH_RESULT_OK && s.live > in->target_triangles) {
        r = build_topology(&s);
        if (r != NPH_RESULT_OK) {
            break;
        }
        // Every class's cheapest valid collapse.
        NPH_ARRAY_CLEAR(&s.candidates);
        for (u32 c = 0; c < s.class_mesh.len && r == NPH_RESULT_OK; ++c) {
            if (s.around_start[c] == s.around_start[c + 1U]) {
                continue; // no live triangle: collapsed or isolated
            }
            Collapse best;
            if (cheapest_collapse(&s, c, &best)) {
                r = NPH_ARRAY_PUSH(&s.candidates, best);
            }
        }
        if (r != NPH_RESULT_OK || s.candidates.len == 0U) {
            break;
        }
        qsort(s.candidates.data, s.candidates.len, sizeof(Collapse), compare_collapses);
        // Up to the cost of the candidate a few times as far down the list
        // as the collapses the target still needs (each takes one or two
        // triangles): most candidates of a pass are blocked by a
        // neighbour's collapse, and the limit keeps a pass from spending an
        // expensive collapse that a later pass would find cheaper.
        const u32 needed = nph_max_u32((s.live - in->target_triangles + 1U) / 2U, 1U);
        const f64 limit = nph_f64_min(
            s.candidates.data[nph_min_u32(needed * PASS_REACH, s.candidates.len) - 1U].cost,
            in->cost_limit);
        memset(s.touched, 0, (size_t)s.class_mesh.len);
        u32 made = 0U;
        for (u32 i = 0; i < s.candidates.len && s.live > in->target_triangles; ++i) {
            const Collapse *c = &s.candidates.data[i];
            if (c->cost > limit) {
                break;
            }
            if (s.touched[c->from] != 0U || s.touched[c->to] != 0U) {
                continue;
            }
            r = apply(&s, c);
            if (r != NPH_RESULT_OK) {
                break;
            }
            worst = nph_f64_max(worst, c->cost);
            made += 1U;
        }
        if (r != NPH_RESULT_OK || made == 0U) {
            break;
        }
        collapses += made;
    }
    if (r == NPH_RESULT_OK && collapses != 0U && in->orphans != NULL) {
        // The classes left without a triangle that went into no other: a
        // piece taken away whole ends in them.
        memset(s.touched, 0, (size_t)s.class_mesh.len);
        for (u32 t = 0; t < s.tri_count; ++t) {
            if (s.dead[t] == 0U) {
                for (u32 k = 0; k < 3U; ++k) {
                    s.touched[tri_class(&s, t, k)] = 1U;
                }
            }
        }
        for (u32 c = 0; c < s.class_mesh.len && r == NPH_RESULT_OK; ++c) {
            if (s.touched[c] == 0U && s.touched_ever[c] == 0U) {
                r = NPH_ARRAY_PUSH(in->orphans, s.class_mesh.data[c]);
            }
        }
    }
    // The added vertices the live triangles still use, in the order they use them.
    u32 *added_place = NULL;
    if (r == NPH_RESULT_OK && s.added.len != 0U) {
        added_place = NPH_ALLOC_ARRAY(s.alloc, u32, s.added.len);
        r = added_place != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
        if (r == NPH_RESULT_OK) {
            memset(added_place, 0xFF, (size_t)s.added.len * sizeof(u32));
        }
    }
    if (r == NPH_RESULT_OK) {
        // The live triangles, back in the mesh's vertices.
        u32 n = 0U;
        for (u32 t = 0; t < s.tri_count && r == NPH_RESULT_OK; ++t) {
            if (s.dead[t] != 0U) {
                continue;
            }
            for (u32 k = 0; k < 3U && r == NPH_RESULT_OK; ++k) {
                u32 v = s.wedge_vertex.data[s.tri[(u64)t * 3U + k]];
                if (v >= LOD_ADDED_BASE) {
                    // A vertex added means the table was made (the analyzer cannot tell).
                    NPH_ASSERT(added_place != NULL);
                    const u32 at = v - LOD_ADDED_BASE;
                    if (added_place[at] == NPH_U32_MAX) {
                        added_place[at] = in->added->len;
                        r = NPH_ARRAY_PUSH(in->added, s.added.data[at]);
                    }
                    v = LOD_ADDED_BASE + added_place[at];
                }
                in->indices[(u64)n * 3U + k] = v;
            }
            in->submeshes[n] = in->submeshes[t];
            n += 1U;
        }
        *out_triangles = n;
        *out_error = (f32)nph_f64_sqrt(worst);
    }
    if (added_place != NULL) {
        NPH_FREE_ARRAY(s.alloc, u32, added_place, s.added.len);
    }
    simp_free(&s);
    return r;
}
