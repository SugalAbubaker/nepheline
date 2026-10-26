// SPDX-License-Identifier: Apache-2.0
// The simplification of one group of a cluster level (render/cluster_lod.h;
// docs/render.md "Levels of detail"; ADR-0060): edge collapses ranked by
// their quadric error (Garland & Heckbert, "Surface Simplification Using
// Quadric Error Metrics", SIGGRAPH 1997).
//
// Every collapse moves one vertex onto a neighbour (a half-edge collapse),
// so the result indexes the mesh's own vertices and no attribute is ever
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
//   complex   anything else (corners of seams, non-manifold edges): never
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
#include <nepheline/core/array.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
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
// How far down the sorted candidates a pass may reach, in multiples of the
// collapses its target still needs.
#define PASS_REACH 2U

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

void lod_mesh_free(LodMesh *m, NphAllocator *alloc)
{
    const u32 n = nph_max_u32(m->vertex_count, 1U);
    const u32 c = nph_max_u32(m->class_count, 1U);
    if (m->class_of != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->class_of, n);
    }
    if (m->wedge_of != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->wedge_of, n);
    }
    if (m->local_wedge != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->local_wedge, n);
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
    if (m->local_class != NULL) {
        NPH_FREE_ARRAY(alloc, u32, m->local_class, c);
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
    m->class_of = NPH_ALLOC_ARRAY(alloc, u32, n);
    m->wedge_of = NPH_ALLOC_ARRAY(alloc, u32, n);
    m->local_wedge = NPH_ALLOC_ARRAY(alloc, u32, n);
    if (m->class_of == NULL || m->wedge_of == NULL || m->local_wedge == NULL) {
        lod_mesh_free(m, alloc);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(m->local_wedge, 0xFF, (size_t)n * sizeof(u32));
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
        m->local_class = NPH_ALLOC_ARRAY(alloc, u32, c);
        m->class_vertex = NPH_ALLOC_ARRAY(alloc, u32, c);
        m->absorbed_next = NPH_ALLOC_ARRAY(alloc, u32, c);
        m->absorbed_tail = NPH_ALLOC_ARRAY(alloc, u32, c);
        r = m->quadrics != NULL && m->normals != NULL && m->local_class != NULL &&
                    m->class_vertex != NULL && m->absorbed_next != NULL && m->absorbed_tail != NULL
                ? NPH_RESULT_OK
                : NPH_RESULT_OUT_OF_MEMORY;
        if (r == NPH_RESULT_OK) {
            memset(m->local_class, 0xFF, (size_t)c * sizeof(u32));
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
// (wedge of `from`, submesh) takes.
#define MAX_REMAP 4U
typedef struct Collapse {
    f64 cost;
    u32 from;
    u32 to;
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
    // Local wedges (their first vertex) and classes (their mesh class).
    NphArrayU32 wedge_vertex;
    NphArrayU32 wedge_class; // local class
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
        ClassKind kind = KIND_COMPLEX;
        if (complex == 0U) {
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

// Whether `from` may collapse onto `to` (their edge run `r`), its cost, and
// the wedges' remap. False where the collapse breaks a rule.
static bool evaluate(Simp *s, u32 from, u32 to, u32 r, Collapse *out)
{
    // Compared as the bytes they are stored in (a ClassKind and an EdgeKind).
    const u32 kind = s->kind[from];
    const u32 edge = s->run_kind[r];
    if ((kind == (u32)KIND_MANIFOLD && edge != (u32)EDGE_SMOOTH) ||
        (kind == (u32)KIND_BORDER && edge != (u32)EDGE_OPEN) ||
        (kind == (u32)KIND_SEAM && edge != (u32)EDGE_SEAM) || kind == (u32)KIND_COMPLEX ||
        kind == (u32)KIND_LOCKED) {
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
                    return false; // one region of `from` meeting two of `to`
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
            return false;
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
        // The level-0 surface the triangle will stand for: its corners'
        // normals, `from`'s merged into `to`'s.
        const NphVec3 *normals = s->mesh->normals;
        NphVec3 ref =
            nph_vec3_add(normals[s->class_mesh.data[from]], normals[s->class_mesh.data[to]]);
        ref = nph_vec3_add(ref, normals[s->class_mesh.data[others[0]]]);
        ref = nph_vec3_add(ref, normals[s->class_mesh.data[others[1]]]);
        if (nph_vec3_dot(ref, n_new) <= 0.0F) {
            return false;
        }
    }
    LodQuadric q = s->mesh->quadrics[s->class_mesh.data[from]];
    lod_quadric_add(&q, &s->mesh->quadrics[s->class_mesh.data[to]]);
    out->cost = lod_quadric_error(&q, p_to);
    return true;
}

// Makes collapse `c`: the edge's triangles go, the others take `to`'s
// wedges, and `to`'s class takes `from`'s quadric.
static void apply(Simp *s, const Collapse *c)
{
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
    NPH_ARRAY_FREE(&s->class_mesh);
    NPH_ARRAY_FREE(&s->edges);
    NPH_ARRAY_FREE(&s->class_edges);
    NPH_ARRAY_FREE(&s->run_start);
    NPH_ARRAY_FREE(&s->candidates);
}

// The group's local wedges and classes; the mesh's scratch maps them.
static NphResult simp_init(Simp *s, const LodSimplifyInput *in)
{
    LodMesh *m = in->mesh;
    for (u32 i = 0; i < in->triangle_count * 3U; ++i) {
        const u32 v = m->wedge_of[in->indices[i]];
        if (m->local_wedge[v] != NPH_U32_MAX) {
            continue;
        }
        const u32 c = m->class_of[v];
        if (m->local_class[c] == NPH_U32_MAX) {
            m->local_class[c] = s->class_mesh.len;
            NPH_TRY(NPH_ARRAY_PUSH(&s->class_mesh, c));
        }
        m->local_wedge[v] = s->wedge_vertex.len;
        NPH_TRY(NPH_ARRAY_PUSH(&s->wedge_vertex, v));
        NPH_TRY(NPH_ARRAY_PUSH(&s->wedge_class, m->local_class[c]));
    }
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
    s->tri = NPH_ALLOC_ARRAY(s->alloc, u32, (u64)tris * 3U);
    s->around = NPH_ALLOC_ARRAY(s->alloc, u32, (u64)tris * 3U);
    s->dead = NPH_ALLOC_ARRAY_ZEROED(s->alloc, u8, tris);
    // An edge run per half-edge at most.
    s->run_kind = NPH_ALLOC_ARRAY(s->alloc, u8, (u64)tris * 3U);
    if (s->class_position == NULL || s->kind == NULL || s->touched == NULL ||
        s->touched_ever == NULL || s->mark == NULL || s->around_start == NULL ||
        s->edge_start == NULL || s->tri == NULL || s->around == NULL || s->dead == NULL ||
        s->run_kind == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 c = 0; c < s->class_mesh.len; ++c) {
        s->class_position[c] = nph_vec3_zero();
    }
    for (u32 w = 0; w < s->wedge_vertex.len; ++w) {
        s->class_position[s->wedge_class.data[w]] = relative(m, s->wedge_vertex.data[w]);
    }
    s->tri_count = in->triangle_count;
    s->live = 0U;
    for (u32 t = 0; t < in->triangle_count; ++t) {
        for (u32 k = 0; k < 3U; ++k) {
            s->tri[(u64)t * 3U + k] = m->local_wedge[m->wedge_of[in->indices[(u64)t * 3U + k]]];
        }
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

static void simp_release_scratch(Simp *s)
{
    LodMesh *m = s->mesh;
    for (u32 w = 0; w < s->wedge_vertex.len; ++w) {
        m->local_wedge[s->wedge_vertex.data[w]] = NPH_U32_MAX;
    }
    for (u32 c = 0; c < s->class_mesh.len; ++c) {
        m->local_class[s->class_mesh.data[c]] = NPH_U32_MAX;
    }
}

NphResult lod_simplify(const LodSimplifyInput *in, u32 *out_triangles, f32 *out_error)
{
    NPH_ASSERT(in != NULL && in->mesh != NULL && out_triangles != NULL && out_error != NULL);
    *out_triangles = in->triangle_count;
    *out_error = 0.0F;
    Simp s;
    memset(&s, 0, sizeof s);
    s.alloc = in->scratch;
    s.mesh = in->mesh;
    s.locked = in->locked;
    s.sub = in->submeshes;
    NPH_ARRAY_INIT(&s.wedge_vertex, s.alloc);
    NPH_ARRAY_INIT(&s.wedge_class, s.alloc);
    NPH_ARRAY_INIT(&s.class_mesh, s.alloc);
    NPH_ARRAY_INIT(&s.edges, s.alloc);
    NPH_ARRAY_INIT(&s.class_edges, s.alloc);
    NPH_ARRAY_INIT(&s.run_start, s.alloc);
    NPH_ARRAY_INIT(&s.candidates, s.alloc);
    NphResult r = simp_init(&s, in);
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
            memset(&best, 0, sizeof best);
            bool found = false;
            for (u32 i = s.edge_start[c]; i < s.edge_start[c + 1U]; ++i) {
                const u32 run = s.class_edges.data[i];
                Collapse cand;
                if (evaluate(&s, c, run_other(&s, run, c), run, &cand) &&
                    (!found || cand.cost < best.cost ||
                     (cand.cost == best.cost && cand.to < best.to))) {
                    best = cand;
                    found = true;
                }
            }
            if (found) {
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
        const f64 limit =
            s.candidates.data[nph_min_u32(needed * PASS_REACH, s.candidates.len) - 1U].cost;
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
            apply(&s, c);
            worst = nph_f64_max(worst, c->cost);
            made += 1U;
        }
        if (made == 0U) {
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
    if (r == NPH_RESULT_OK) {
        // The live triangles, back in the mesh's vertices.
        u32 n = 0U;
        for (u32 t = 0; t < s.tri_count; ++t) {
            if (s.dead[t] != 0U) {
                continue;
            }
            for (u32 k = 0; k < 3U; ++k) {
                in->indices[(u64)n * 3U + k] = s.wedge_vertex.data[s.tri[(u64)t * 3U + k]];
            }
            in->submeshes[n] = in->submeshes[t];
            n += 1U;
        }
        *out_triangles = n;
        *out_error = (f32)nph_f64_sqrt(worst);
    }
    simp_release_scratch(&s);
    simp_free(&s);
    return r;
}
