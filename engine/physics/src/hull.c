// SPDX-License-Identifier: Apache-2.0
// Convex hulls: packing a polytope into the hull block, the box hull, the
// Quickhull builder, and the hull queries (hull.h; docs/physics.md "Convex
// hulls").
//
// The builder follows Barber, Dobkin, and Huhdanpaa's Quickhull with the
// face-merging rules John Lloyd's QuickHull3D uses (two passes: faces that
// are non-convex with respect to the larger of the pair, then with respect
// to either), so coplanar triangles become one polygon and no face is
// thinner than the tolerance. It runs in double precision on the f32 input:
// the output is f32, and the extra precision during the build keeps the
// visibility decisions consistent.

#include <nepheline/core/array.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/physics/hull.h>

#include <float.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_PHYSICS, "physics", NPH_LOG_LEVEL_INFO);

#define QH_NONE NPH_U32_MAX

static u32 align16(u32 n)
{
    return (n + 15U) & ~15U;
}

static void *hull_bytes(NphPhysicsHull *h, u32 offset)
{
    return (u8 *)h + offset;
}

// ---------------------------------------------------------------------------
// Packing: vertices and counter-clockwise face loops into a hull block
// ---------------------------------------------------------------------------

typedef struct EdgeKey {
    u32 a; // tail
    u32 b; // head
    u32 index;
} EdgeKey;

static int edge_key_compare(const void *pa, const void *pb)
{
    const EdgeKey *x = (const EdgeKey *)pa;
    const EdgeKey *y = (const EdgeKey *)pb;
    const u32 xlo = nph_min_u32(x->a, x->b);
    const u32 xhi = nph_max_u32(x->a, x->b);
    const u32 ylo = nph_min_u32(y->a, y->b);
    const u32 yhi = nph_max_u32(y->a, y->b);
    if (xlo != ylo) {
        return xlo < ylo ? -1 : 1;
    }
    if (xhi != yhi) {
        return xhi < yhi ? -1 : 1;
    }
    // The two directions of one edge: the one leaving the lower vertex first.
    if (x->a != y->a) {
        return x->a < y->a ? -1 : 1;
    }
    return 0;
}

// Eberly's subexpressions for the polyhedral mass integrals ("Polyhedral
// Mass Properties (Revisited)", Geometric Tools, 2002), per coordinate.
typedef struct MassTerms {
    f64 f1;
    f64 f2;
    f64 f3;
    f64 g0;
    f64 g1;
    f64 g2;
} MassTerms;

static void mass_subexpressions(f64 w0, f64 w1, f64 w2, MassTerms *t)
{
    const f64 temp0 = w0 + w1;
    t->f1 = temp0 + w2;
    const f64 temp1 = w0 * w0;
    const f64 temp2 = temp1 + w1 * temp0;
    t->f2 = temp2 + w2 * t->f1;
    t->f3 = w0 * temp1 + w1 * temp2 + w2 * t->f2;
    t->g0 = t->f2 + w0 * (t->f1 + w0);
    t->g1 = t->f2 + w1 * (t->f1 + w1);
    t->g2 = t->f2 + w2 * (t->f1 + w2);
}

typedef struct MassIntegrals {
    f64 v[10]; // 1, x, y, z, x^2, y^2, z^2, xy, yz, zx
} MassIntegrals;

static void mass_add_triangle(MassIntegrals *m, NphDVec3 p0, NphDVec3 p1, NphDVec3 p2)
{
    const f64 a1 = p1.x - p0.x;
    const f64 b1 = p1.y - p0.y;
    const f64 c1 = p1.z - p0.z;
    const f64 a2 = p2.x - p0.x;
    const f64 b2 = p2.y - p0.y;
    const f64 c2 = p2.z - p0.z;
    const f64 d0 = b1 * c2 - b2 * c1;
    const f64 d1 = a2 * c1 - a1 * c2;
    const f64 d2 = a1 * b2 - a2 * b1;
    MassTerms x;
    MassTerms y;
    MassTerms z;
    mass_subexpressions(p0.x, p1.x, p2.x, &x);
    mass_subexpressions(p0.y, p1.y, p2.y, &y);
    mass_subexpressions(p0.z, p1.z, p2.z, &z);
    m->v[0] += d0 * x.f1;
    m->v[1] += d0 * x.f2;
    m->v[2] += d1 * y.f2;
    m->v[3] += d2 * z.f2;
    m->v[4] += d0 * x.f3;
    m->v[5] += d1 * y.f3;
    m->v[6] += d2 * z.f3;
    m->v[7] += d0 * (p0.y * x.g0 + p1.y * x.g1 + p2.y * x.g2);
    m->v[8] += d1 * (p0.z * y.g0 + p1.z * y.g1 + p2.z * y.g2);
    m->v[9] += d2 * (p0.x * z.g0 + p1.x * z.g1 + p2.x * z.g2);
}

// Builds the block from `vertices` and the faces' vertex loops, each
// counter-clockwise seen from outside: face f is loop[offsets[f]] to
// loop[offsets[f + 1] - 1]. Every edge must border exactly two faces.
static NphResult hull_pack(const NphVec3 *vertices, u32 vertex_count, const u32 *loop,
                           const u32 *offsets, u32 face_count, f32 tolerance, NphAllocator *alloc,
                           NphPhysicsHull **out)
{
    const u32 edge_count = offsets[face_count];
    if (vertex_count < 4U || face_count < 4U || edge_count > NPH_U16_MAX ||
        vertex_count > NPH_PHYSICS_HULL_MAX_VERTICES || (edge_count & 1U) != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }

    // Pair every half-edge with its twin: sort by the undirected edge.
    EdgeKey *keys = NPH_ALLOC_ARRAY(alloc, EdgeKey, edge_count);
    u16 *out_index = NPH_ALLOC_ARRAY(alloc, u16, edge_count);
    if (keys == NULL || out_index == NULL) {
        NPH_FREE_ARRAY(alloc, EdgeKey, keys, edge_count);
        NPH_FREE_ARRAY(alloc, u16, out_index, edge_count);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 f = 0U; f < face_count; ++f) {
        const u32 n = offsets[f + 1U] - offsets[f];
        for (u32 i = 0U; i < n; ++i) {
            const u32 h = offsets[f] + i;
            keys[h].a = loop[h];
            keys[h].b = loop[offsets[f] + (i + 1U) % n];
            keys[h].index = h;
        }
    }
    qsort(keys, edge_count, sizeof(EdgeKey), edge_key_compare);
    NphResult result = NPH_RESULT_OK;
    // twin_of[h] for each loop half-edge, through the sorted pairs.
    u32 *twin_of = NPH_ALLOC_ARRAY(alloc, u32, edge_count);
    if (twin_of == NULL) {
        result = NPH_RESULT_OUT_OF_MEMORY;
    } else {
        for (u32 k = 0U; k + 1U < edge_count && result == NPH_RESULT_OK; k += 2U) {
            const EdgeKey *x = &keys[k];
            const EdgeKey *y = &keys[k + 1U];
            if (x->a != y->b || x->b != y->a) {
                result = NPH_RESULT_INVALID_ARGUMENT; // an edge without its twin
            } else {
                twin_of[x->index] = y->index;
                twin_of[y->index] = x->index;
            }
        }
    }
    NPH_FREE_ARRAY(alloc, EdgeKey, keys, edge_count);
    if (result != NPH_RESULT_OK) {
        NPH_FREE_ARRAY(alloc, u32, twin_of, edge_count);
        NPH_FREE_ARRAY(alloc, u16, out_index, edge_count);
        return result;
    }
    // Output indices: twins at 2k and 2k + 1, in face and loop order.
    for (u32 h = 0U; h < edge_count; ++h) {
        out_index[h] = NPH_U16_MAX;
    }
    u32 next_pair = 0U;
    for (u32 h = 0U; h < edge_count; ++h) {
        if (out_index[h] == NPH_U16_MAX) {
            out_index[h] = (u16)next_pair;
            out_index[twin_of[h]] = (u16)(next_pair + 1U);
            next_pair += 2U;
        }
    }

    const u32 header = (u32)sizeof(NphPhysicsHull);
    const u32 vertices_offset = header;
    const u32 planes_offset = vertices_offset + align16(vertex_count * (u32)sizeof(NphVec3));
    const u32 faces_offset = planes_offset + face_count * (u32)sizeof(NphPlane);
    const u32 edges_offset = faces_offset + align16(face_count * (u32)sizeof(u16));
    const u32 byte_size = edges_offset + align16(edge_count * (u32)sizeof(NphPhysicsHullEdge));
    NphPhysicsHull *hull = (NphPhysicsHull *)NPH_ALLOC_ZEROED(alloc, byte_size, 16U);
    if (hull == NULL) {
        NPH_FREE_ARRAY(alloc, u32, twin_of, edge_count);
        NPH_FREE_ARRAY(alloc, u16, out_index, edge_count);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    hull->byte_size = byte_size;
    hull->vertex_count = vertex_count;
    hull->edge_count = edge_count;
    hull->face_count = face_count;
    hull->vertices_offset = vertices_offset;
    hull->planes_offset = planes_offset;
    hull->faces_offset = faces_offset;
    hull->edges_offset = edges_offset;
    hull->tolerance = tolerance;

    NphVec3 *out_vertices = hull_bytes(hull, vertices_offset);
    NphPlane *out_planes = hull_bytes(hull, planes_offset);
    u16 *out_faces = hull_bytes(hull, faces_offset);
    NphPhysicsHullEdge *out_edges = hull_bytes(hull, edges_offset);

    memcpy(out_vertices, vertices, (size_t)vertex_count * sizeof(NphVec3));
    NphAABB bounds = nph_aabb_from_point(vertices[0]);
    NphDVec3 reference = nph_dvec3_zero();
    for (u32 v = 0U; v < vertex_count; ++v) {
        bounds.min = nph_vec3_min(bounds.min, vertices[v]);
        bounds.max = nph_vec3_max(bounds.max, vertices[v]);
        reference = nph_dvec3_add(reference, nph_dvec3_from_vec3(vertices[v]));
    }
    reference = nph_dvec3_scale(reference, 1.0 / (f64)vertex_count);
    hull->bounds = bounds;

    MassIntegrals mi;
    memset(&mi, 0, sizeof mi);
    for (u32 f = 0U; f < face_count; ++f) {
        const u32 first = offsets[f];
        const u32 n = offsets[f + 1U] - first;
        // Newell's normal and the loop's mean, relative to the reference
        // point for precision.
        NphDVec3 normal = nph_dvec3_zero();
        NphDVec3 mean = nph_dvec3_zero();
        for (u32 i = 0U; i < n; ++i) {
            const NphDVec3 c =
                nph_dvec3_sub(nph_dvec3_from_vec3(vertices[loop[first + i]]), reference);
            const NphDVec3 d =
                nph_dvec3_sub(nph_dvec3_from_vec3(vertices[loop[first + (i + 1U) % n]]), reference);
            normal.x += (c.y - d.y) * (c.z + d.z);
            normal.y += (c.z - d.z) * (c.x + d.x);
            normal.z += (c.x - d.x) * (c.y + d.y);
            mean = nph_dvec3_add(mean, c);
        }
        mean = nph_dvec3_scale(mean, 1.0 / (f64)n);
        normal = nph_dvec3_normalize_or_zero(normal);
        const NphDVec3 mean_abs = nph_dvec3_add(mean, reference);
        out_planes[f].normal = nph_dvec3_to_vec3(normal);
        out_planes[f].d = (f32)(-nph_dvec3_dot(normal, mean_abs));
        out_faces[f] = out_index[first];
        for (u32 i = 0U; i < n; ++i) {
            const u32 h = first + i;
            NphPhysicsHullEdge *e = &out_edges[out_index[h]];
            e->next = out_index[first + (i + 1U) % n];
            e->twin = out_index[twin_of[h]];
            e->origin = (u16)loop[h];
            e->face = (u16)f;
        }
        // The fan of the face, for the volume integrals.
        const NphDVec3 p0 = nph_dvec3_sub(nph_dvec3_from_vec3(vertices[loop[first]]), reference);
        for (u32 i = 1U; i + 1U < n; ++i) {
            const NphDVec3 p1 =
                nph_dvec3_sub(nph_dvec3_from_vec3(vertices[loop[first + i]]), reference);
            const NphDVec3 p2 =
                nph_dvec3_sub(nph_dvec3_from_vec3(vertices[loop[first + i + 1U]]), reference);
            mass_add_triangle(&mi, p0, p1, p2);
        }
    }
    NPH_FREE_ARRAY(alloc, u32, twin_of, edge_count);
    NPH_FREE_ARRAY(alloc, u16, out_index, edge_count);

    static const f64 mult[10] = {1.0 / 6.0,  1.0 / 24.0, 1.0 / 24.0,  1.0 / 24.0,  1.0 / 60.0,
                                 1.0 / 60.0, 1.0 / 60.0, 1.0 / 120.0, 1.0 / 120.0, 1.0 / 120.0};
    for (u32 i = 0U; i < 10U; ++i) {
        mi.v[i] *= mult[i];
    }
    const f64 volume = mi.v[0];
    if (!(volume > 0.0)) {
        nph_physics_hull_free(hull, alloc);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphDVec3 cm = nph_dvec3(mi.v[1] / volume, mi.v[2] / volume, mi.v[3] / volume);
    const f64 ixx = mi.v[5] + mi.v[6] - volume * (cm.y * cm.y + cm.z * cm.z);
    const f64 iyy = mi.v[4] + mi.v[6] - volume * (cm.z * cm.z + cm.x * cm.x);
    const f64 izz = mi.v[4] + mi.v[5] - volume * (cm.x * cm.x + cm.y * cm.y);
    const f64 ixy = -(mi.v[7] - volume * cm.x * cm.y);
    const f64 iyz = -(mi.v[8] - volume * cm.y * cm.z);
    const f64 ixz = -(mi.v[9] - volume * cm.z * cm.x);
    hull->volume = (f32)volume;
    const NphDVec3 centroid = nph_dvec3_add(cm, reference);
    hull->centroid = nph_dvec3_to_vec3(centroid);
    hull->inertia = nph_mat3_from_cols(nph_vec3((f32)ixx, (f32)ixy, (f32)ixz),
                                       nph_vec3((f32)ixy, (f32)iyy, (f32)iyz),
                                       nph_vec3((f32)ixz, (f32)iyz, (f32)izz));
    f32 inner = FLT_MAX;
    for (u32 f = 0U; f < face_count; ++f) {
        inner = nph_f32_min(inner, -nph_plane_distance(out_planes[f], hull->centroid));
    }
    f32 outer = 0.0F;
    for (u32 v = 0U; v < vertex_count; ++v) {
        outer = nph_f32_max(outer, nph_vec3_distance(vertices[v], hull->centroid));
    }
    hull->inner_radius = nph_f32_max(inner, 0.0F);
    hull->outer_radius = outer;
    *out = hull;
    return NPH_RESULT_OK;
}

void nph_physics_hull_free(NphPhysicsHull *hull, NphAllocator *alloc)
{
    if (hull == NULL) {
        return;
    }
    NPH_ASSERT(alloc != NULL);
    NPH_FREE(alloc, hull, hull->byte_size, 16U);
}

// ---------------------------------------------------------------------------
// The box
// ---------------------------------------------------------------------------

NphResult nph_physics_hull_make_box(NphVec3 h, NphAllocator *alloc, NphPhysicsHull **out)
{
    if (alloc == NULL || out == NULL || !(h.x > 0.0F && h.x <= NPH_PHYSICS_MAX_COORDINATE) ||
        !(h.y > 0.0F && h.y <= NPH_PHYSICS_MAX_COORDINATE) ||
        !(h.z > 0.0F && h.z <= NPH_PHYSICS_MAX_COORDINATE)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // Corner i has +x when bit 0 is set, +y with bit 1, +z with bit 2.
    NphVec3 corners[8];
    for (u32 i = 0U; i < 8U; ++i) {
        corners[i] = nph_vec3((i & 1U) != 0U ? h.x : -h.x, (i & 2U) != 0U ? h.y : -h.y,
                              (i & 4U) != 0U ? h.z : -h.z);
    }
    // -x, +x, -y, +y, -z, +z, each counter-clockwise seen from outside.
    static const u32 loops[24] = {0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4,
                                  2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    static const u32 offsets[7] = {0, 4, 8, 12, 16, 20, 24};
    // Twice the largest coordinate's round-off, like the builder's default.
    const f32 tolerance = 3.0F * FLT_EPSILON * (h.x + h.y + h.z);
    return hull_pack(corners, 8U, loops, offsets, 6U, tolerance, alloc, out);
}

// ---------------------------------------------------------------------------
// Quickhull
// ---------------------------------------------------------------------------

// Half-edges store their HEAD vertex, as in QuickHull3D: the tail is the
// head of the previous edge, so removing a redundant vertex while merging
// faces is a relinking with no vertex to update.
typedef struct QhEdge {
    u32 head;
    u32 next;
    u32 prev;
    u32 twin;
    u32 face;
} QhEdge;

typedef enum QhMark {
    QH_FACE_VISIBLE = 0, // alive (QuickHull3D's name)
    QH_FACE_NON_CONVEX = 1,
    QH_FACE_DELETED = 2,
} QhMark;

typedef struct QhFace {
    NphDVec3 normal;
    NphDVec3 centroid;
    f64 offset; // dot(normal, p) - offset is the signed distance of p
    f64 area;
    f64 far_distance; // of the farthest point in the conflict list (QH_NONE: empty)
    u32 far_point;
    u32 edge; // any half-edge of the face
    u32 vertex_count;
    u32 conflict; // the head of its conflict list (through next_conflict)
    u32 mark;
} QhFace;

typedef NPH_ARRAY(QhEdge) QhEdgeArray;
typedef NPH_ARRAY(QhFace) QhFaceArray;

typedef struct QhHorizonFrame {
    u32 face;
    u32 edge0;
    u32 current;
} QhHorizonFrame;

typedef NPH_ARRAY(QhHorizonFrame) QhStack;

typedef struct Quickhull {
    NphAllocator *alloc;
    NphDVec3 *points;
    u32 point_count;
    u32 *next_conflict; // per point
    QhEdgeArray edges;
    QhFaceArray faces;
    NphArrayU32 horizon;
    NphArrayU32 new_faces;
    NphArrayU32 unclaimed;
    QhStack stack;
    f64 tolerance;
    bool failed; // an allocation failed
} Quickhull;

static f64 qh_distance(const QhFace *f, NphDVec3 p)
{
    return nph_dvec3_dot(f->normal, p) - f->offset;
}

static NphDVec3 qh_point(const Quickhull *qh, u32 v)
{
    return qh->points[v];
}

static u32 qh_tail(const Quickhull *qh, u32 e)
{
    return qh->edges.data[qh->edges.data[e].prev].head;
}

// Newell-style normal over the fan from the first vertex, the centroid of the
// loop's vertices, and the area (QuickHull3D's computeNormal), with the
// longest-edge correction for slivers: a face with almost no area has an
// unreliable normal, so its component along the longest edge is removed.
static void qh_face_update(Quickhull *qh, u32 fi)
{
    QhFace *f = &qh->faces.data[fi];
    const QhEdge *edges = qh->edges.data;
    const u32 e0 = f->edge;
    const NphDVec3 p0 = qh_point(qh, edges[e0].head);
    const u32 e1 = edges[e0].next;
    u32 e2 = edges[e1].next;
    NphDVec3 d2 = nph_dvec3_sub(qh_point(qh, edges[e1].head), p0);
    NphDVec3 normal = nph_dvec3_zero();
    NphDVec3 centroid = nph_dvec3_add(p0, qh_point(qh, edges[e1].head));
    u32 count = 2U;
    while (e2 != e0) {
        const NphDVec3 d1 = d2;
        const NphDVec3 p2 = qh_point(qh, edges[e2].head);
        d2 = nph_dvec3_sub(p2, p0);
        normal = nph_dvec3_add(normal, nph_dvec3_cross(d1, d2));
        centroid = nph_dvec3_add(centroid, p2);
        e2 = edges[e2].next;
        count += 1U;
    }
    f->area = nph_dvec3_length(normal);
    f->vertex_count = count;
    f->centroid = nph_dvec3_scale(centroid, 1.0 / (f64)count);
    if (f->area > 0.0) {
        normal = nph_dvec3_scale(normal, 1.0 / f->area);
    }
    if (f->area < qh->tolerance * qh->tolerance * 16.0 || !(f->area > 0.0)) {
        u32 longest = e0;
        f64 longest_sq = 0.0;
        u32 e = e0;
        do {
            const f64 len_sq = nph_dvec3_length_sq(
                nph_dvec3_sub(qh_point(qh, edges[e].head), qh_point(qh, qh_tail(qh, e))));
            if (len_sq > longest_sq) {
                longest_sq = len_sq;
                longest = e;
            }
            e = edges[e].next;
        } while (e != e0);
        if (longest_sq > 0.0) {
            const NphDVec3 u = nph_dvec3_scale(nph_dvec3_sub(qh_point(qh, edges[longest].head),
                                                             qh_point(qh, qh_tail(qh, longest))),
                                               1.0 / nph_f64_sqrt(longest_sq));
            normal = nph_dvec3_normalize_or_zero(
                nph_dvec3_sub(normal, nph_dvec3_scale(u, nph_dvec3_dot(normal, u))));
        }
    }
    f->normal = normal;
    f->offset = nph_dvec3_dot(normal, f->centroid);
}

// A triangle whose loop visits v0, v1, v2 (as heads, in that order). Returns
// the face index; its first edge has head v0.
static u32 qh_add_triangle(Quickhull *qh, u32 v0, u32 v1, u32 v2)
{
    const u32 e = qh->edges.len;
    const u32 fi = qh->faces.len;
    if (NPH_ARRAY_RESERVE(&qh->edges, e + 3U) != NPH_RESULT_OK ||
        NPH_ARRAY_RESERVE(&qh->faces, fi + 1U) != NPH_RESULT_OK) {
        qh->failed = true;
        return QH_NONE;
    }
    const u32 heads[3] = {v0, v1, v2};
    for (u32 i = 0U; i < 3U; ++i) {
        QhEdge edge;
        edge.head = heads[i];
        edge.next = e + (i + 1U) % 3U;
        edge.prev = e + (i + 2U) % 3U;
        edge.twin = QH_NONE;
        edge.face = fi;
        qh->edges.data[e + i] = edge;
    }
    qh->edges.len = e + 3U;
    QhFace face;
    memset(&face, 0, sizeof face);
    face.edge = e;
    face.conflict = QH_NONE;
    face.far_point = QH_NONE;
    face.far_distance = 0.0;
    face.mark = QH_FACE_VISIBLE;
    qh->faces.data[fi] = face;
    qh->faces.len = fi + 1U;
    qh_face_update(qh, fi);
    return fi;
}

static void qh_add_conflict(Quickhull *qh, u32 fi, u32 p, f64 distance)
{
    QhFace *f = &qh->faces.data[fi];
    qh->next_conflict[p] = f->conflict;
    f->conflict = p;
    if (f->far_point == QH_NONE || distance > f->far_distance) {
        f->far_point = p;
        f->far_distance = distance;
    }
}

// Recomputes the farthest conflict point of a face whose plane or list
// changed.
static void qh_refresh_far(Quickhull *qh, u32 fi)
{
    QhFace *f = &qh->faces.data[fi];
    f->far_point = QH_NONE;
    f->far_distance = 0.0;
    for (u32 p = f->conflict; p != QH_NONE; p = qh->next_conflict[p]) {
        const f64 d = qh_distance(f, qh->points[p]);
        if (f->far_point == QH_NONE || d > f->far_distance) {
            f->far_point = p;
            f->far_distance = d;
        }
    }
}

static void qh_remove_conflict(Quickhull *qh, u32 fi, u32 p)
{
    QhFace *f = &qh->faces.data[fi];
    u32 *link = &f->conflict;
    while (*link != QH_NONE && *link != p) {
        link = &qh->next_conflict[*link];
    }
    if (*link == p) {
        *link = qh->next_conflict[p];
    }
    qh_refresh_far(qh, fi);
}

// Moves a face's conflict points to `absorbing` when they are outside it, to
// the unclaimed list otherwise (QuickHull3D's deleteFacePoints).
static void qh_release_points(Quickhull *qh, u32 fi, u32 absorbing)
{
    u32 p = qh->faces.data[fi].conflict;
    qh->faces.data[fi].conflict = QH_NONE;
    qh->faces.data[fi].far_point = QH_NONE;
    while (p != QH_NONE) {
        const u32 next = qh->next_conflict[p];
        if (absorbing != QH_NONE) {
            const f64 d = qh_distance(&qh->faces.data[absorbing], qh->points[p]);
            if (d > qh->tolerance) {
                qh_add_conflict(qh, absorbing, p, d);
                p = next;
                continue;
            }
        }
        if (NPH_ARRAY_PUSH(&qh->unclaimed, p) != NPH_RESULT_OK) {
            qh->failed = true;
        }
        p = next;
    }
}

// The distance of the face across `e` (its centroid) above e's face.
static f64 qh_opposite_distance(const Quickhull *qh, u32 e)
{
    const QhEdge *edges = qh->edges.data;
    const QhFace *f = &qh->faces.data[edges[e].face];
    const QhFace *opp = &qh->faces.data[edges[edges[e].twin].face];
    return qh_distance(f, opp->centroid);
}

static u32 qh_opposite_face(const Quickhull *qh, u32 e)
{
    return qh->edges.data[qh->edges.data[e].twin].face;
}

// Joins the loop at the vertex between hedge_prev and hedge (QuickHull3D's
// connectHalfEdges). When both border the same opposite face the vertex is
// redundant and its edges collapse; a triangle on the other side then
// disappears. Returns that face, or QH_NONE.
static u32 qh_connect(Quickhull *qh, u32 fi, u32 hedge_prev, u32 hedge)
{
    QhEdge *edges = qh->edges.data;
    u32 discarded = QH_NONE;
    if (qh_opposite_face(qh, hedge_prev) == qh_opposite_face(qh, hedge)) {
        const u32 opp_face = qh_opposite_face(qh, hedge);
        u32 hedge_opp;
        if (hedge_prev == qh->faces.data[fi].edge) {
            qh->faces.data[fi].edge = hedge;
        }
        if (qh->faces.data[opp_face].vertex_count == 3U) {
            hedge_opp = edges[edges[edges[hedge].twin].prev].twin;
            qh->faces.data[opp_face].mark = QH_FACE_DELETED;
            discarded = opp_face;
        } else {
            hedge_opp = edges[edges[hedge].twin].next;
            if (qh->faces.data[opp_face].edge == edges[hedge_opp].prev) {
                qh->faces.data[opp_face].edge = hedge_opp;
            }
            edges[hedge_opp].prev = edges[edges[hedge_opp].prev].prev;
            edges[edges[hedge_opp].prev].next = hedge_opp;
        }
        edges[hedge].prev = edges[hedge_prev].prev;
        edges[edges[hedge].prev].next = hedge;
        edges[hedge].twin = hedge_opp;
        edges[hedge_opp].twin = hedge;
        if (qh->faces.data[opp_face].mark != QH_FACE_DELETED) {
            qh_face_update(qh, opp_face);
            qh_refresh_far(qh, opp_face);
        }
    } else {
        edges[hedge_prev].next = hedge;
        edges[hedge].prev = hedge_prev;
    }
    return discarded;
}

// Face fi absorbs the face across hedge_adj (QuickHull3D's
// mergeAdjacentFace). Writes the faces that disappeared to `discarded` and
// returns their count (1 to 3).
static u32 qh_merge(Quickhull *qh, u32 fi, u32 hedge_adj, u32 discarded[3])
{
    QhEdge *edges = qh->edges.data;
    const u32 opp_face = qh_opposite_face(qh, hedge_adj);
    u32 count = 0U;
    discarded[count++] = opp_face;
    qh->faces.data[opp_face].mark = QH_FACE_DELETED;
    const u32 hedge_opp = edges[hedge_adj].twin;
    u32 hedge_adj_prev = edges[hedge_adj].prev;
    u32 hedge_adj_next = edges[hedge_adj].next;
    u32 hedge_opp_prev = edges[hedge_opp].prev;
    u32 hedge_opp_next = edges[hedge_opp].next;
    while (qh_opposite_face(qh, hedge_adj_prev) == opp_face) {
        hedge_adj_prev = edges[hedge_adj_prev].prev;
        hedge_opp_next = edges[hedge_opp_next].next;
    }
    while (qh_opposite_face(qh, hedge_adj_next) == opp_face) {
        hedge_opp_prev = edges[hedge_opp_prev].prev;
        hedge_adj_next = edges[hedge_adj_next].next;
    }
    for (u32 h = hedge_opp_next; h != edges[hedge_opp_prev].next; h = edges[h].next) {
        edges[h].face = fi;
    }
    if (hedge_adj == qh->faces.data[fi].edge) {
        qh->faces.data[fi].edge = hedge_adj_next;
    }
    u32 d = qh_connect(qh, fi, hedge_opp_prev, hedge_adj_next);
    if (d != QH_NONE) {
        discarded[count++] = d;
    }
    d = qh_connect(qh, fi, hedge_adj_prev, hedge_opp_next);
    if (d != QH_NONE) {
        discarded[count++] = d;
    }
    qh_face_update(qh, fi);
    qh_refresh_far(qh, fi);
    return count;
}

typedef enum QhMergeRule {
    QH_MERGE_NONCONVEX_WRT_LARGER = 0,
    QH_MERGE_NONCONVEX = 1,
} QhMergeRule;

// Merges one non-convex neighbour into fi; true when it did (the loop has
// changed, so the caller tries again). QuickHull3D's doAdjacentMerge.
static bool qh_merge_adjacent(Quickhull *qh, u32 fi, QhMergeRule rule)
{
    const f64 tol = qh->tolerance;
    u32 hedge = qh->faces.data[fi].edge;
    bool convex = true;
    do {
        const u32 opp_face = qh_opposite_face(qh, hedge);
        bool merge = false;
        if (rule == QH_MERGE_NONCONVEX) {
            if (qh_opposite_distance(qh, hedge) > -tol ||
                qh_opposite_distance(qh, qh->edges.data[hedge].twin) > -tol) {
                merge = true;
            }
        } else if (qh->faces.data[fi].area > qh->faces.data[opp_face].area) {
            if (qh_opposite_distance(qh, hedge) > -tol) {
                merge = true;
            } else if (qh_opposite_distance(qh, qh->edges.data[hedge].twin) > -tol) {
                convex = false;
            }
        } else {
            if (qh_opposite_distance(qh, qh->edges.data[hedge].twin) > -tol) {
                merge = true;
            } else if (qh_opposite_distance(qh, hedge) > -tol) {
                convex = false;
            }
        }
        if (merge) {
            u32 discarded[3];
            const u32 n = qh_merge(qh, fi, hedge, discarded);
            for (u32 i = 0U; i < n; ++i) {
                qh_release_points(qh, discarded[i], fi);
            }
            return true;
        }
        hedge = qh->edges.data[hedge].next;
    } while (hedge != qh->faces.data[fi].edge);
    if (!convex) {
        qh->faces.data[fi].mark = QH_FACE_NON_CONVEX;
    }
    return false;
}

// Collects the horizon seen from `eye`: deletes every face that sees it,
// depth first from `start`, and records the edges between a deleted face and
// a live one, in order around the eye (QuickHull3D's calculateHorizon,
// iterative so the depth costs heap rather than fiber stack).
static void qh_horizon(Quickhull *qh, NphDVec3 eye, u32 start)
{
    NPH_ARRAY_CLEAR(&qh->horizon);
    NPH_ARRAY_CLEAR(&qh->stack);
    qh_release_points(qh, start, QH_NONE);
    qh->faces.data[start].mark = QH_FACE_DELETED;
    QhHorizonFrame first = {start, qh->faces.data[start].edge, qh->faces.data[start].edge};
    if (NPH_ARRAY_PUSH(&qh->stack, first) != NPH_RESULT_OK) {
        qh->failed = true;
        return;
    }
    while (qh->stack.len > 0U && !qh->failed) {
        QhHorizonFrame *top = NPH_ARRAY_LAST(&qh->stack);
        const u32 e = top->current;
        top->current = qh->edges.data[e].next;
        const bool last = top->current == top->edge0;
        const u32 twin = qh->edges.data[e].twin;
        const u32 opp = qh->edges.data[twin].face;
        if (last) {
            qh->stack.len -= 1U; // pop
        }
        if (qh->faces.data[opp].mark != QH_FACE_DELETED) {
            if (qh_distance(&qh->faces.data[opp], eye) > qh->tolerance) {
                qh_release_points(qh, opp, QH_NONE);
                qh->faces.data[opp].mark = QH_FACE_DELETED;
                const QhHorizonFrame child = {opp, twin, qh->edges.data[twin].next};
                if (NPH_ARRAY_PUSH(&qh->stack, child) != NPH_RESULT_OK) {
                    qh->failed = true;
                }
            } else if (NPH_ARRAY_PUSH(&qh->horizon, e) != NPH_RESULT_OK) {
                qh->failed = true;
            }
        }
    }
}

// A triangle per horizon edge, fanning to the eye (QuickHull3D's
// addNewFaces / addAdjoiningFace).
static void qh_add_new_faces(Quickhull *qh, u32 eye)
{
    NPH_ARRAY_CLEAR(&qh->new_faces);
    u32 side_prev = QH_NONE;
    u32 side_begin = QH_NONE;
    for (u32 i = 0U; i < qh->horizon.len && !qh->failed; ++i) {
        const u32 h = qh->horizon.data[i];
        const u32 tail = qh_tail(qh, h);
        const u32 head = qh->edges.data[h].head;
        const u32 fi = qh_add_triangle(qh, eye, tail, head);
        if (fi == QH_NONE) {
            return;
        }
        QhEdge *edges = qh->edges.data;
        const u32 e0 = qh->faces.data[fi].edge; // head -> eye
        const u32 e1 = edges[e0].next;          // eye -> tail
        const u32 e2 = edges[e1].next;          // tail -> head, along the horizon
        const u32 outside = edges[h].twin;
        edges[e2].twin = outside;
        edges[outside].twin = e2;
        if (side_prev != QH_NONE) {
            edges[e1].twin = side_prev;
            edges[side_prev].twin = e1;
        } else {
            side_begin = e0;
        }
        if (NPH_ARRAY_PUSH(&qh->new_faces, fi) != NPH_RESULT_OK) {
            qh->failed = true;
            return;
        }
        side_prev = e0;
    }
    if (side_begin != QH_NONE) {
        QhEdge *edges = qh->edges.data;
        const u32 e1 = edges[side_begin].next;
        edges[e1].twin = side_prev;
        edges[side_prev].twin = e1;
    }
}

static void qh_resolve_unclaimed(Quickhull *qh)
{
    for (u32 i = 0U; i < qh->unclaimed.len; ++i) {
        const u32 p = qh->unclaimed.data[i];
        f64 best = qh->tolerance;
        u32 best_face = QH_NONE;
        for (u32 k = 0U; k < qh->new_faces.len; ++k) {
            const u32 fi = qh->new_faces.data[k];
            if (qh->faces.data[fi].mark != QH_FACE_VISIBLE) {
                continue;
            }
            const f64 d = qh_distance(&qh->faces.data[fi], qh->points[p]);
            if (d > best) {
                best = d;
                best_face = fi;
            }
        }
        if (best_face != QH_NONE) {
            qh_add_conflict(qh, best_face, p, best);
        }
    }
    NPH_ARRAY_CLEAR(&qh->unclaimed);
}

static void qh_add_point(Quickhull *qh, u32 eye, u32 eye_face)
{
    qh_remove_conflict(qh, eye_face, eye);
    NPH_ARRAY_CLEAR(&qh->unclaimed);
    qh_horizon(qh, qh->points[eye], eye_face);
    if (qh->failed) {
        return;
    }
    qh_add_new_faces(qh, eye);
    if (qh->failed) {
        return;
    }
    for (u32 k = 0U; k < qh->new_faces.len; ++k) {
        const u32 fi = qh->new_faces.data[k];
        if (qh->faces.data[fi].mark == QH_FACE_VISIBLE) {
            while (qh_merge_adjacent(qh, fi, QH_MERGE_NONCONVEX_WRT_LARGER)) {
            }
        }
    }
    for (u32 k = 0U; k < qh->new_faces.len; ++k) {
        const u32 fi = qh->new_faces.data[k];
        if (qh->faces.data[fi].mark == QH_FACE_NON_CONVEX) {
            qh->faces.data[fi].mark = QH_FACE_VISIBLE;
            while (qh_merge_adjacent(qh, fi, QH_MERGE_NONCONVEX)) {
            }
        }
    }
    qh_resolve_unclaimed(qh);
}

// Links the twins of a small set of faces by search (the initial simplex).
static void qh_link_twins(Quickhull *qh, const u32 *faces, u32 count)
{
    QhEdge *edges = qh->edges.data;
    for (u32 i = 0U; i < count; ++i) {
        u32 e = qh->faces.data[faces[i]].edge;
        do {
            if (edges[e].twin == QH_NONE) {
                const u32 tail = qh_tail(qh, e);
                for (u32 j = 0U; j < count && edges[e].twin == QH_NONE; ++j) {
                    u32 o = qh->faces.data[faces[j]].edge;
                    do {
                        if (edges[o].head == tail && qh_tail(qh, o) == edges[e].head) {
                            edges[e].twin = o;
                            edges[o].twin = e;
                            break;
                        }
                        o = edges[o].next;
                    } while (o != qh->faces.data[faces[j]].edge);
                }
            }
            e = edges[e].next;
        } while (e != qh->faces.data[faces[i]].edge);
    }
}

// The initial tetrahedron from extreme points (QuickHull3D's
// createInitialSimplex). False for a degenerate cloud.
static bool qh_initial_simplex(Quickhull *qh)
{
    const NphDVec3 *pts = qh->points;
    u32 min_v[3] = {0U, 0U, 0U};
    u32 max_v[3] = {0U, 0U, 0U};
    for (u32 i = 1U; i < qh->point_count; ++i) {
        const f64 c[3] = {pts[i].x, pts[i].y, pts[i].z};
        for (u32 a = 0U; a < 3U; ++a) {
            const f64 lo[3] = {pts[min_v[a]].x, pts[min_v[a]].y, pts[min_v[a]].z};
            const f64 hi[3] = {pts[max_v[a]].x, pts[max_v[a]].y, pts[max_v[a]].z};
            if (c[a] < lo[a]) {
                min_v[a] = i;
            }
            if (c[a] > hi[a]) {
                max_v[a] = i;
            }
        }
    }
    f64 best = 0.0;
    u32 axis = 0U;
    for (u32 a = 0U; a < 3U; ++a) {
        const f64 lo[3] = {pts[min_v[a]].x, pts[min_v[a]].y, pts[min_v[a]].z};
        const f64 hi[3] = {pts[max_v[a]].x, pts[max_v[a]].y, pts[max_v[a]].z};
        if (hi[a] - lo[a] > best) {
            best = hi[a] - lo[a];
            axis = a;
        }
    }
    if (best <= qh->tolerance) {
        return false; // coincident
    }
    u32 v[4];
    v[0] = max_v[axis];
    v[1] = min_v[axis];
    const NphDVec3 u01 = nph_dvec3_normalize_or_zero(nph_dvec3_sub(pts[v[1]], pts[v[0]]));
    f64 max_sq = 0.0;
    NphDVec3 normal = nph_dvec3_zero();
    v[2] = QH_NONE;
    for (u32 i = 0U; i < qh->point_count; ++i) {
        const NphDVec3 x = nph_dvec3_cross(u01, nph_dvec3_sub(pts[i], pts[v[0]]));
        const f64 len_sq = nph_dvec3_length_sq(x);
        if (len_sq > max_sq && i != v[0] && i != v[1]) {
            max_sq = len_sq;
            v[2] = i;
            normal = x;
        }
    }
    if (v[2] == QH_NONE || nph_f64_sqrt(max_sq) <= 100.0 * qh->tolerance) {
        return false; // collinear
    }
    normal = nph_dvec3_normalize_or_zero(normal);
    const f64 d0 = nph_dvec3_dot(pts[v[2]], normal);
    f64 max_dist = 0.0;
    v[3] = QH_NONE;
    for (u32 i = 0U; i < qh->point_count; ++i) {
        const f64 dist = nph_f64_abs(nph_dvec3_dot(pts[i], normal) - d0);
        if (dist > max_dist && i != v[0] && i != v[1] && i != v[2]) {
            max_dist = dist;
            v[3] = i;
        }
    }
    if (v[3] == QH_NONE || max_dist <= 100.0 * qh->tolerance) {
        return false; // coplanar
    }
    u32 tris[4];
    if (nph_dvec3_dot(pts[v[3]], normal) - d0 < 0.0) {
        tris[0] = qh_add_triangle(qh, v[0], v[1], v[2]);
        tris[1] = qh_add_triangle(qh, v[3], v[1], v[0]);
        tris[2] = qh_add_triangle(qh, v[3], v[2], v[1]);
        tris[3] = qh_add_triangle(qh, v[3], v[0], v[2]);
    } else {
        tris[0] = qh_add_triangle(qh, v[0], v[2], v[1]);
        tris[1] = qh_add_triangle(qh, v[3], v[0], v[1]);
        tris[2] = qh_add_triangle(qh, v[3], v[1], v[2]);
        tris[3] = qh_add_triangle(qh, v[3], v[2], v[0]);
    }
    if (qh->failed) {
        return false;
    }
    qh_link_twins(qh, tris, 4U);
    for (u32 i = 0U; i < qh->point_count; ++i) {
        if (i == v[0] || i == v[1] || i == v[2] || i == v[3]) {
            continue;
        }
        f64 far = qh->tolerance;
        u32 face = QH_NONE;
        for (u32 k = 0U; k < 4U; ++k) {
            const f64 d = qh_distance(&qh->faces.data[tris[k]], pts[i]);
            if (d > far) {
                far = d;
                face = tris[k];
            }
        }
        if (face != QH_NONE) {
            qh_add_conflict(qh, face, i, far);
        }
    }
    return true;
}

static void qh_free(Quickhull *qh)
{
    if (qh->points != NULL) {
        NPH_FREE_ARRAY(qh->alloc, NphDVec3, qh->points, qh->point_count);
    }
    if (qh->next_conflict != NULL) {
        NPH_FREE_ARRAY(qh->alloc, u32, qh->next_conflict, qh->point_count);
    }
    NPH_ARRAY_FREE(&qh->edges);
    NPH_ARRAY_FREE(&qh->faces);
    NPH_ARRAY_FREE(&qh->horizon);
    NPH_ARRAY_FREE(&qh->new_faces);
    NPH_ARRAY_FREE(&qh->unclaimed);
    NPH_ARRAY_FREE(&qh->stack);
}

// Converts the live faces into vertices and loops and packs them.
static NphResult qh_finish(Quickhull *qh, const NphVec3 *input, f32 tolerance, NphAllocator *alloc,
                           NphPhysicsHull **out)
{
    u32 live = 0U;
    u32 loop_len = 0U;
    for (u32 f = 0U; f < qh->faces.len; ++f) {
        if (qh->faces.data[f].mark != QH_FACE_DELETED) {
            live += 1U;
            loop_len += qh->faces.data[f].vertex_count;
        }
    }
    u32 *remap = NPH_ALLOC_ARRAY(alloc, u32, qh->point_count);
    u32 *loop = NPH_ALLOC_ARRAY(alloc, u32, loop_len);
    u32 *offsets = NPH_ALLOC_ARRAY(alloc, u32, live + 1U);
    NphVec3 *vertices = NPH_ALLOC_ARRAY(alloc, NphVec3, NPH_PHYSICS_HULL_MAX_VERTICES);
    NphResult result = NPH_RESULT_OK;
    if (remap == NULL || loop == NULL || offsets == NULL || vertices == NULL) {
        result = NPH_RESULT_OUT_OF_MEMORY;
    } else {
        for (u32 i = 0U; i < qh->point_count; ++i) {
            remap[i] = QH_NONE;
        }
        u32 vertex_count = 0U;
        u32 face = 0U;
        u32 cursor = 0U;
        const QhEdge *edges = qh->edges.data;
        for (u32 f = 0U; f < qh->faces.len && result == NPH_RESULT_OK; ++f) {
            const QhFace *qf = &qh->faces.data[f];
            if (qf->mark == QH_FACE_DELETED) {
                continue;
            }
            offsets[face++] = cursor;
            u32 e = qf->edge;
            do {
                const u32 p = edges[e].head;
                if (remap[p] == QH_NONE) {
                    if (vertex_count == NPH_PHYSICS_HULL_MAX_VERTICES) {
                        result = NPH_RESULT_INVALID_ARGUMENT;
                        break;
                    }
                    remap[p] = vertex_count;
                    vertices[vertex_count++] = input[p];
                }
                loop[cursor++] = remap[p];
                e = edges[e].next;
            } while (e != qf->edge);
        }
        offsets[face] = cursor;
        if (result == NPH_RESULT_OK) {
            result = hull_pack(vertices, vertex_count, loop, offsets, live, tolerance, alloc, out);
        }
    }
    if (remap != NULL) {
        NPH_FREE_ARRAY(alloc, u32, remap, qh->point_count);
    }
    if (loop != NULL) {
        NPH_FREE_ARRAY(alloc, u32, loop, loop_len);
    }
    if (offsets != NULL) {
        NPH_FREE_ARRAY(alloc, u32, offsets, live + 1U);
    }
    if (vertices != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec3, vertices, NPH_PHYSICS_HULL_MAX_VERTICES);
    }
    return result;
}

// The validator's checks (below), logging each violation only when `log`.
static bool hull_check(const void *block, u32 block_size, f32 tolerance, bool log);

// One Quickhull run at `tolerance` (the builder below).
static NphResult quickhull(const NphPhysicsHullDesc *desc, u32 budget, f64 tolerance,
                           NphAllocator *alloc, NphPhysicsHull **out)
{
    Quickhull qh;
    memset(&qh, 0, sizeof qh);
    qh.alloc = alloc;
    qh.point_count = desc->point_count;
    qh.tolerance = tolerance;
    NPH_ARRAY_INIT(&qh.edges, alloc);
    NPH_ARRAY_INIT(&qh.faces, alloc);
    NPH_ARRAY_INIT(&qh.horizon, alloc);
    NPH_ARRAY_INIT(&qh.new_faces, alloc);
    NPH_ARRAY_INIT(&qh.unclaimed, alloc);
    NPH_ARRAY_INIT(&qh.stack, alloc);
    qh.points = NPH_ALLOC_ARRAY(alloc, NphDVec3, desc->point_count);
    qh.next_conflict = NPH_ALLOC_ARRAY(alloc, u32, desc->point_count);
    if (qh.points == NULL || qh.next_conflict == NULL) {
        qh_free(&qh);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0U; i < desc->point_count; ++i) {
        qh.points[i] = nph_dvec3_from_vec3(desc->points[i]);
        qh.next_conflict[i] = QH_NONE;
    }
    if (!qh_initial_simplex(&qh)) {
        const bool oom = qh.failed;
        qh_free(&qh);
        if (!oom) {
            NPH_LOG_WARN(&NPH_LOG_PHYSICS,
                         "hull build: the %u points have no volume (coincident, collinear, or "
                         "coplanar within %g)",
                         desc->point_count, tolerance);
        }
        return oom ? NPH_RESULT_OUT_OF_MEMORY : NPH_RESULT_INVALID_ARGUMENT;
    }
    u32 hull_vertices = 4U;
    while (!qh.failed) {
        // The farthest outside point over every face: the most volume first,
        // so a budget keeps the most significant points. Ties go to the
        // lowest face index, which keeps the build deterministic.
        u32 eye_face = QH_NONE;
        f64 far = qh.tolerance;
        for (u32 f = 0U; f < qh.faces.len; ++f) {
            const QhFace *face = &qh.faces.data[f];
            if (face->mark != QH_FACE_DELETED && face->far_point != QH_NONE &&
                face->far_distance > far) {
                far = face->far_distance;
                eye_face = f;
            }
        }
        if (eye_face == QH_NONE || hull_vertices >= budget) {
            break;
        }
        const u32 eye = qh.faces.data[eye_face].far_point;
        qh_add_point(&qh, eye, eye_face);
        hull_vertices += 1U;
    }
    NphResult result = NPH_RESULT_OUT_OF_MEMORY;
    if (!qh.failed) {
        result = qh_finish(&qh, desc->points, (f32)tolerance, alloc, out);
    }
    qh_free(&qh);
    return result;
}

NphResult nph_physics_hull_build(const NphPhysicsHullDesc *desc, NphAllocator *alloc,
                                 NphPhysicsHull **out)
{
    if (desc == NULL || alloc == NULL || out == NULL || desc->points == NULL ||
        desc->point_count < 4U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 budget =
        desc->max_vertices == 0U ? NPH_PHYSICS_HULL_DEFAULT_MAX_VERTICES : desc->max_vertices;
    if (budget < 4U || budget > NPH_PHYSICS_HULL_MAX_VERTICES || !(desc->tolerance >= 0.0F) ||
        !nph_f32_is_finite(desc->tolerance)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    f64 extent = 0.0;
    {
        f64 mx = 0.0;
        f64 my = 0.0;
        f64 mz = 0.0;
        for (u32 i = 0U; i < desc->point_count; ++i) {
            const NphVec3 p = desc->points[i];
            if (!nph_vec3_is_finite(p) ||
                nph_vec3_max_component(nph_vec3_abs(p)) > NPH_PHYSICS_MAX_COORDINATE) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            mx = nph_f64_max(mx, nph_f64_abs((f64)p.x));
            my = nph_f64_max(my, nph_f64_abs((f64)p.y));
            mz = nph_f64_max(mz, nph_f64_abs((f64)p.z));
        }
        extent = mx + my + mz;
    }
    f64 tolerance = desc->tolerance > 0.0F ? (f64)desc->tolerance : 3.0 * (f64)FLT_EPSILON * extent;
    // Quickhull's merges keep each face convex within the tolerance, but a
    // cloud with features near the tolerance's scale can still leave a
    // merged face reflex beyond it (fuzzing found a sliver 4 mm thick and
    // 128 m long). Such a hull is built again at four, then sixteen times
    // the tolerance, which merges what the first could not; one that still
    // fails validation is refused.
    for (u32 attempt = 0U; attempt < 3U; ++attempt) {
        const NphResult result = quickhull(desc, budget, tolerance, alloc, out);
        if (result != NPH_RESULT_OK) {
            return result;
        }
        if (hull_check(*out, (*out)->byte_size, 0.0F, false)) {
            return NPH_RESULT_OK;
        }
        nph_physics_hull_free(*out, alloc);
        *out = NULL;
        tolerance *= 4.0;
    }
    NPH_LOG_WARN(&NPH_LOG_PHYSICS, "hull build: the %u points give no hull valid within %g",
                 desc->point_count, tolerance / 4.0);
    return NPH_RESULT_INVALID_ARGUMENT;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

u32 nph_physics_hull_support(const NphPhysicsHull *hull, NphVec3 direction)
{
    NPH_ASSERT(hull != NULL && hull->vertex_count > 0U);
    const NphVec3 *v = nph_physics_hull_vertices(hull);
    u32 best = 0U;
    f32 best_dot = nph_vec3_dot(v[0], direction);
    for (u32 i = 1U; i < hull->vertex_count; ++i) {
        const f32 d = nph_vec3_dot(v[i], direction);
        if (d > best_dot) {
            best_dot = d;
            best = i;
        }
    }
    return best;
}

u32 nph_physics_hull_face_vertex_count(const NphPhysicsHull *hull, u32 face)
{
    NPH_ASSERT(hull != NULL && face < hull->face_count);
    const NphPhysicsHullEdge *edges = nph_physics_hull_edges(hull);
    const u32 first = nph_physics_hull_faces(hull)[face];
    u32 n = 0U;
    u32 e = first;
    do {
        n += 1U;
        e = edges[e].next;
    } while (e != first && n <= hull->edge_count);
    return n;
}

// A violation: logged when the check is asked to (`log` in scope).
#define HULL_FAIL(...)                                                                             \
    do {                                                                                           \
        if (log) {                                                                                 \
            NPH_LOG_WARN(&NPH_LOG_PHYSICS, "hull validate: " __VA_ARGS__);                         \
        }                                                                                          \
        return false;                                                                              \
    } while (0)

static bool finite_in_range(f32 v)
{
    return nph_f32_abs(v) <= NPH_PHYSICS_MAX_COORDINATE; // false for NaN
}

static bool vec3_in_range(NphVec3 v)
{
    return finite_in_range(v.x) && finite_in_range(v.y) && finite_in_range(v.z);
}

static bool hull_check(const void *block, u32 block_size, f32 tolerance, bool log)
{
    const u32 header = (u32)sizeof(NphPhysicsHull);
    if (block == NULL || block_size < header || ((uptr)block & 15U) != 0U) {
        HULL_FAIL("no 16-byte aligned header in %u bytes", block_size);
    }
    const NphPhysicsHull *hull = block;
    if (hull->byte_size < header || hull->byte_size > block_size) {
        HULL_FAIL("byte_size %u outside the %u-byte block", hull->byte_size, block_size);
    }
    const u64 size = hull->byte_size;
    const u64 v_end = (u64)hull->vertices_offset + (u64)hull->vertex_count * sizeof(NphVec3);
    const u64 p_end = (u64)hull->planes_offset + (u64)hull->face_count * sizeof(NphPlane);
    const u64 f_end = (u64)hull->faces_offset + (u64)hull->face_count * sizeof(u16);
    const u64 e_end = (u64)hull->edges_offset + (u64)hull->edge_count * sizeof(NphPhysicsHullEdge);
    if (hull->vertices_offset < header || hull->planes_offset < header ||
        hull->faces_offset < header || hull->edges_offset < header || v_end > size ||
        p_end > size || f_end > size || e_end > size || (hull->vertices_offset & 3U) != 0U ||
        (hull->planes_offset & 3U) != 0U || (hull->faces_offset & 1U) != 0U ||
        (hull->edges_offset & 1U) != 0U) {
        HULL_FAIL("arrays outside the %u-byte block", hull->byte_size);
    }
    const u32 nv = hull->vertex_count;
    const u32 ne = hull->edge_count;
    const u32 nf = hull->face_count;
    // A convex polyhedron of V vertices has at most 2V - 4 faces and 3V - 6
    // edges (Euler, with three edges a face); the collision code's arrays
    // are sized by these bounds.
    if (nv < 4U || nv > NPH_PHYSICS_HULL_MAX_VERTICES || nf < 4U || nf > 2U * nv - 4U || ne < 12U ||
        (ne & 1U) != 0U || ne / 2U > 3U * nv - 6U) {
        HULL_FAIL("counts nv=%u ne=%u nf=%u", nv, ne, nf);
    }
    if ((i64)nv - (i64)(ne / 2U) + (i64)nf != 2) {
        HULL_FAIL("Euler characteristic nv - ne + nf = %lld",
                  (long long)((i64)nv - (i64)(ne / 2U) + (i64)nf));
    }
    // The header's numbers: what mass, bounds, and continuous collision read.
    const NphAABB bounds = hull->bounds;
    bool numbers = vec3_in_range(bounds.min) && vec3_in_range(bounds.max) &&
                   bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y &&
                   bounds.min.z <= bounds.max.z && vec3_in_range(hull->centroid) &&
                   hull->volume > 0.0F && nph_f32_is_finite(hull->volume) &&
                   hull->tolerance > 0.0F && hull->tolerance <= NPH_PHYSICS_MAX_COORDINATE &&
                   hull->inner_radius >= 0.0F && hull->inner_radius <= hull->outer_radius &&
                   hull->outer_radius <= 4.0F * NPH_PHYSICS_MAX_COORDINATE;
    for (u32 i = 0U; i < 3U; ++i) {
        numbers = numbers && nph_vec3_is_finite(hull->inertia.c[i]);
    }
    if (!numbers) {
        HULL_FAIL("header numbers not finite or out of range");
    }
    const f32 tol = tolerance > 0.0F ? tolerance : nph_f32_max(hull->tolerance, 1e-6F) * 8.0F;
    const NphVec3 *verts = nph_physics_hull_vertices(hull);
    for (u32 v = 0U; v < nv; ++v) {
        if (!nph_vec3_is_finite(verts[v]) || !nph_aabb_contains_point(bounds, verts[v])) {
            HULL_FAIL("vertex %u not finite or outside the bounds", v);
        }
    }
    const NphPlane *planes = nph_physics_hull_planes(hull);
    const u16 *faces = nph_physics_hull_faces(hull);
    const NphPhysicsHullEdge *edges = nph_physics_hull_edges(hull);
    for (u32 i = 0U; i < ne; ++i) {
        const NphPhysicsHullEdge *e = &edges[i];
        if (e->twin != (i ^ 1U) || e->next >= ne || e->origin >= nv || e->face >= nf) {
            HULL_FAIL("half-edge %u has bad links", i);
        }
        if (edges[e->next].face != e->face) {
            HULL_FAIL("half-edge %u: next leaves the face", i);
        }
        if (edges[e->twin].origin != edges[e->next].origin) {
            HULL_FAIL("half-edge %u: twin does not start at its head", i);
        }
        if (edges[e->twin].face == e->face) {
            HULL_FAIL("half-edge %u: both sides on face %u", i, e->face);
        }
    }
    u32 visited = 0U;
    for (u32 f = 0U; f < nf; ++f) {
        if (faces[f] >= ne || edges[faces[f]].face != f) {
            HULL_FAIL("face %u: bad first edge", f);
        }
        const NphPlane pl = planes[f];
        if (!nph_vec3_is_finite(pl.normal) ||
            nph_f32_abs(nph_vec3_length(pl.normal) - 1.0F) > 1e-4F || !nph_f32_is_finite(pl.d)) {
            HULL_FAIL("face %u: plane normal not unit", f);
        }
        u32 n = 0U;
        u32 e = faces[f];
        do {
            const u32 nx = edges[e].next;
            const NphVec3 a = verts[edges[e].origin];
            const NphVec3 b = verts[edges[nx].origin];
            const NphVec3 c = verts[edges[edges[nx].next].origin];
            if (nph_f32_abs(nph_plane_distance(pl, a)) > tol) {
                HULL_FAIL("face %u: vertex %u is %g off its plane", f, (u32)edges[e].origin,
                          (f64)nph_plane_distance(pl, a));
            }
            // Convex turn: c is left of the line a -> b in the face's plane
            // (the cross product over |b - a| is c's distance from that line).
            const NphVec3 e1 = nph_vec3_sub(b, a);
            const NphVec3 e2 = nph_vec3_sub(c, b);
            if (nph_vec3_dot(nph_vec3_cross(e1, e2), pl.normal) < -tol * nph_vec3_length(e1)) {
                HULL_FAIL("face %u: reflex corner at vertex %u", f, (u32)edges[nx].origin);
            }
            n += 1U;
            e = nx;
        } while (e != faces[f] && n <= ne);
        if (e != faces[f]) {
            HULL_FAIL("face %u: loop does not close", f);
        }
        visited += n;
    }
    if (visited != ne) {
        HULL_FAIL("face loops cover %u of %u half-edges", visited, ne);
    }
    for (u32 f = 0U; f < nf; ++f) {
        for (u32 v = 0U; v < nv; ++v) {
            if (nph_plane_distance(planes[f], verts[v]) > tol) {
                HULL_FAIL("vertex %u is %g in front of face %u", v,
                          (f64)nph_plane_distance(planes[f], verts[v]), f);
            }
        }
    }
    return true;
}

bool nph_physics_hull_validate(const void *block, u32 size, f32 tolerance)
{
    return hull_check(block, size, tolerance, true);
}
