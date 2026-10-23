// SPDX-License-Identifier: Apache-2.0
// GJK distance with the signed-volumes subalgorithm, and EPA penetration
// (collide.h; docs/physics.md "GJK and EPA").
//
// GJK (Gilbert, Johnson, and Keerthi, "A Fast Procedure for Computing the
// Distance Between Complex Objects in Three-Dimensional Space", IEEE J.
// Robotics and Automation 1988) walks a simplex of the Minkowski difference
// A - B towards the origin. The closest point of each simplex comes from the
// signed-volumes subalgorithm of Montanari, Petrinic, and Barbieri (ACM TOG
// 2017): barycentric coordinates are ratios of signed volumes (areas,
// lengths) measured in the coordinate plane (axis) where the projection is
// largest, so the decision of which sub-simplex holds the closest point
// never divides by a vanishing quantity. Johnson's original subalgorithm is
// the classic source of GJK failures in single precision.
//
// EPA (van den Bergen, GDC 2001) grows a polytope inside the Minkowski
// difference from GJK's final tetrahedron until the face nearest the origin
// is on the boundary: that face's normal and distance are the penetration.

#include <nepheline/core/assert.h>
#include <nepheline/physics/collide.h>

#include <float.h>
#include <string.h>

typedef struct GjkVertex {
    NphVec3 a; // support point of A, in the common frame
    NphVec3 b; // of B
    NphVec3 w; // a - b
    u32 ia;
    u32 ib;
} GjkVertex;

typedef struct GjkSimplex {
    GjkVertex v[4];
    f32 lambda[4];
    u32 count;
} GjkSimplex;

static NphVec3 proxy_point(const NphPhysicsProxy *p, NphPhysicsPose pose, u32 i)
{
    return nph_physics_pose_transform_point(pose, p->points[i]);
}

static u32 proxy_support(const NphPhysicsProxy *p, NphPhysicsPose pose, NphVec3 direction)
{
    return nph_physics_proxy_support(p, nph_quat_rotate_inverse(pose.rotation, direction));
}

static GjkVertex make_vertex(const NphPhysicsProxy *a, NphPhysicsPose pa, u32 ia,
                             const NphPhysicsProxy *b, NphPhysicsPose pb, u32 ib)
{
    GjkVertex v;
    v.a = proxy_point(a, pa, ia);
    v.b = proxy_point(b, pb, ib);
    v.w = nph_vec3_sub(v.a, v.b);
    v.ia = ia;
    v.ib = ib;
    return v;
}

static f32 vec3_axis(NphVec3 v, u32 i)
{
    return i == 0U ? v.x : (i == 1U ? v.y : v.z);
}

static bool same_sign(f32 a, f32 b)
{
    return (a > 0.0F && b > 0.0F) || (a < 0.0F && b < 0.0F);
}

// ---------------------------------------------------------------------------
// Signed volumes (Montanari et al. 2017, Algorithms 2-4)
// ---------------------------------------------------------------------------

// The closest point of segment [s1, s2] to the origin as weights over the
// two points. The weights are measured along the axis of the segment's
// largest extent.
static void s1d(NphVec3 s1, NphVec3 s2, f32 lambda[2])
{
    const NphVec3 t = nph_vec3_sub(s2, s1);
    const f32 tt = nph_vec3_dot(t, t);
    if (!(tt > 0.0F)) {
        lambda[0] = 1.0F;
        lambda[1] = 0.0F;
        return;
    }
    const NphVec3 p0 = nph_vec3_mul_add(s1, t, -nph_vec3_dot(s1, t) / tt);
    const u32 axis = nph_vec3_max_abs_index(t);
    const f32 mu = vec3_axis(s1, axis) - vec3_axis(s2, axis);
    const f32 c1 = vec3_axis(p0, axis) - vec3_axis(s2, axis);
    const f32 c2 = vec3_axis(s1, axis) - vec3_axis(p0, axis);
    if (same_sign(mu, c1) && same_sign(mu, c2)) {
        lambda[0] = c1 / mu;
        lambda[1] = c2 / mu;
    } else if (same_sign(mu, c1)) {
        lambda[0] = 1.0F; // beyond s1
        lambda[1] = 0.0F;
    } else {
        lambda[0] = 0.0F;
        lambda[1] = 1.0F;
    }
}

static NphVec3 weighted2(NphVec3 s1, NphVec3 s2, const f32 l[2])
{
    return nph_vec3_add(nph_vec3_scale(s1, l[0]), nph_vec3_scale(s2, l[1]));
}

static NphVec3 weighted3(NphVec3 s1, NphVec3 s2, NphVec3 s3, const f32 l[3])
{
    return nph_vec3_add(nph_vec3_add(nph_vec3_scale(s1, l[0]), nph_vec3_scale(s2, l[1])),
                        nph_vec3_scale(s3, l[2]));
}

// The best of the edges whose opposite vertex is on the wrong side (or all
// three, for a degenerate triangle).
static void s2d_edges(const NphVec3 s[3], const bool test[3], f32 lambda[3])
{
    f32 best = FLT_MAX;
    lambda[0] = 1.0F;
    lambda[1] = 0.0F;
    lambda[2] = 0.0F;
    for (u32 j = 0U; j < 3U; ++j) {
        if (!test[j]) {
            continue;
        }
        const u32 i0 = (j + 1U) % 3U;
        const u32 i1 = (j + 2U) % 3U;
        f32 l[2];
        s1d(s[i0], s[i1], l);
        const f32 d = nph_vec3_length_sq(weighted2(s[i0], s[i1], l));
        if (d < best) {
            best = d;
            lambda[j] = 0.0F;
            lambda[i0] = l[0];
            lambda[i1] = l[1];
        }
    }
}

// Twice the signed area of the triangle (p, q, r) projected onto the
// coordinate plane (i, j).
static f32 area2(NphVec3 p, NphVec3 q, NphVec3 r, u32 i, u32 j)
{
    return (vec3_axis(q, i) - vec3_axis(p, i)) * (vec3_axis(r, j) - vec3_axis(p, j)) -
           (vec3_axis(q, j) - vec3_axis(p, j)) * (vec3_axis(r, i) - vec3_axis(p, i));
}

static void s2d(const NphVec3 s[3], f32 lambda[3])
{
    const NphVec3 n = nph_vec3_cross(nph_vec3_sub(s[1], s[0]), nph_vec3_sub(s[2], s[0]));
    const f32 nn = nph_vec3_dot(n, n);
    if (!(nn > 0.0F)) {
        const bool all[3] = {true, true, true};
        s2d_edges(s, all, lambda);
        return;
    }
    const NphVec3 p0 = nph_vec3_scale(n, nph_vec3_dot(s[0], n) / nn);
    // Project onto the coordinate plane where the triangle is largest; with
    // (i, j, k) cyclic its signed double area there is n_k.
    const u32 k = nph_vec3_max_abs_index(n);
    const u32 i = (k + 1U) % 3U;
    const u32 j = (k + 2U) % 3U;
    const f32 mu = vec3_axis(n, k);
    const f32 c[3] = {area2(p0, s[1], s[2], i, j), area2(s[0], p0, s[2], i, j),
                      area2(s[0], s[1], p0, i, j)};
    const bool wrong[3] = {!same_sign(mu, c[0]), !same_sign(mu, c[1]), !same_sign(mu, c[2])};
    if (!wrong[0] && !wrong[1] && !wrong[2]) {
        lambda[0] = c[0] / mu;
        lambda[1] = c[1] / mu;
        lambda[2] = c[2] / mu;
        return;
    }
    s2d_edges(s, wrong, lambda);
}

static f32 det3(NphVec3 a, NphVec3 b, NphVec3 c)
{
    return nph_vec3_dot(a, nph_vec3_cross(b, c));
}

// Returns true when the origin is inside the tetrahedron.
static bool s3d(const NphVec3 s[4], f32 lambda[4])
{
    const NphVec3 e1 = nph_vec3_sub(s[1], s[0]);
    const NphVec3 e2 = nph_vec3_sub(s[2], s[0]);
    const NphVec3 e3 = nph_vec3_sub(s[3], s[0]);
    const f32 det = det3(e1, e2, e3);
    // lambda_i det for the origin: the volume with the origin in place of
    // vertex i.
    const NphVec3 o0 = nph_vec3_neg(s[0]);
    const f32 c[4] = {det3(s[1], s[2], s[3]), det3(o0, e2, e3), det3(e1, o0, e3), det3(e1, e2, o0)};
    bool wrong[4];
    bool inside = det != 0.0F;
    for (u32 i = 0U; i < 4U; ++i) {
        wrong[i] = !same_sign(det, c[i]);
        inside = inside && !wrong[i];
    }
    if (inside) {
        for (u32 i = 0U; i < 4U; ++i) {
            lambda[i] = c[i] / det;
        }
        return true;
    }
    f32 best = FLT_MAX;
    for (u32 i = 0U; i < 4U; ++i) {
        lambda[i] = i == 0U ? 1.0F : 0.0F;
    }
    for (u32 f = 0U; f < 4U; ++f) {
        if (!wrong[f] && det != 0.0F) {
            continue;
        }
        NphVec3 tri[3];
        u32 map[3];
        u32 n = 0U;
        for (u32 i = 0U; i < 4U; ++i) {
            if (i != f) {
                tri[n] = s[i];
                map[n] = i;
                n += 1U;
            }
        }
        f32 l[3];
        s2d(tri, l);
        const f32 d = nph_vec3_length_sq(weighted3(tri[0], tri[1], tri[2], l));
        if (d < best) {
            best = d;
            for (u32 i = 0U; i < 4U; ++i) {
                lambda[i] = 0.0F;
            }
            lambda[map[0]] = l[0];
            lambda[map[1]] = l[1];
            lambda[map[2]] = l[2];
        }
    }
    return false;
}

// Solves for the point of the simplex nearest the origin, drops the vertices
// with zero weight, and returns the point. Sets *inside when a tetrahedron
// contains the origin.
static NphVec3 simplex_solve(GjkSimplex *s, bool *inside)
{
    *inside = false;
    f32 lambda[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    switch (s->count) {
    case 1U:
        break;
    case 2U:
        s1d(s->v[0].w, s->v[1].w, lambda);
        break;
    case 3U: {
        const NphVec3 tri[3] = {s->v[0].w, s->v[1].w, s->v[2].w};
        s2d(tri, lambda);
        break;
    }
    case 4U: {
        const NphVec3 tet[4] = {s->v[0].w, s->v[1].w, s->v[2].w, s->v[3].w};
        *inside = s3d(tet, lambda);
        break;
    }
    default:
        NPH_UNREACHABLE();
    }
    NphVec3 v = nph_vec3_zero();
    u32 kept = 0U;
    for (u32 i = 0U; i < s->count; ++i) {
        if (lambda[i] > 0.0F || (s->count == 1U)) {
            s->v[kept] = s->v[i];
            s->lambda[kept] = lambda[i];
            v = nph_vec3_mul_add(v, s->v[i].w, lambda[i]);
            kept += 1U;
        }
    }
    if (kept == 0U) {
        // Every weight underflowed: keep the first vertex.
        s->lambda[0] = 1.0F;
        v = s->v[0].w;
        kept = 1U;
    }
    s->count = kept;
    return v;
}

static void simplex_points(const GjkSimplex *s, NphVec3 *pa, NphVec3 *pb)
{
    NphVec3 a = nph_vec3_zero();
    NphVec3 b = nph_vec3_zero();
    f32 total = 0.0F;
    for (u32 i = 0U; i < s->count; ++i) {
        a = nph_vec3_mul_add(a, s->v[i].a, s->lambda[i]);
        b = nph_vec3_mul_add(b, s->v[i].b, s->lambda[i]);
        total += s->lambda[i];
    }
    if (total > 0.0F && total != 1.0F) {
        a = nph_vec3_scale(a, 1.0F / total);
        b = nph_vec3_scale(b, 1.0F / total);
    }
    *pa = a;
    *pb = b;
}

// The GJK loop on the cores. Returns true when the cores overlap (or touch
// within round-off); the final simplex is left in *s.
static bool gjk(const NphPhysicsProxy *a, NphPhysicsPose pa, const NphPhysicsProxy *b,
                NphPhysicsPose pb, NphPhysicsSimplexCache *cache, GjkSimplex *s,
                u32 *out_iterations)
{
    s->count = 0U;
    if (cache != NULL && cache->count >= 1U && cache->count <= 4U) {
        for (u32 i = 0U; i < cache->count; ++i) {
            const u32 ia = cache->index_a[i];
            const u32 ib = cache->index_b[i];
            if (ia >= a->count || ib >= b->count) {
                s->count = 0U;
                break;
            }
            s->v[s->count++] = make_vertex(a, pa, ia, b, pb, ib);
        }
    }
    if (s->count == 0U) {
        s->v[0] = make_vertex(a, pa, 0U, b, pb, 0U);
        s->count = 1U;
    }
    // The scale of the problem, for the overlap tolerance.
    const f32 scale = nph_f32_max(nph_vec3_length_sq(s->v[0].a), nph_vec3_length_sq(s->v[0].b)) +
                      nph_vec3_length_sq(s->v[0].w) + 1e-12F;
    bool inside = false;
    NphVec3 v = simplex_solve(s, &inside);
    f32 v_sq = nph_vec3_length_sq(v);
    u32 iterations = 0U;
    const u32 max_iterations = 40U;
    while (!inside && iterations < max_iterations) {
        if (v_sq <= 1e-12F * scale) {
            inside = true; // touching within round-off
            break;
        }
        iterations += 1U;
        const NphVec3 d = nph_vec3_neg(v);
        const u32 ia = proxy_support(a, pa, d);
        const u32 ib = proxy_support(b, pb, v);
        bool duplicate = false;
        for (u32 i = 0U; i < s->count; ++i) {
            duplicate = duplicate || (s->v[i].ia == ia && s->v[i].ib == ib);
        }
        if (duplicate) {
            break;
        }
        const GjkVertex w = make_vertex(a, pa, ia, b, pb, ib);
        // No meaningful progress along -v: v is within the relative
        // tolerance of the distance (van den Bergen's termination test).
        if (v_sq - nph_vec3_dot(v, w.w) <= 1e-6F * v_sq) {
            break;
        }
        s->v[s->count++] = w;
        const NphVec3 v_new = simplex_solve(s, &inside);
        const f32 v_new_sq = nph_vec3_length_sq(v_new);
        if (!inside && v_new_sq >= v_sq) {
            break; // round-off: keep what we have
        }
        v = v_new;
        v_sq = v_new_sq;
    }
    if (cache != NULL) {
        cache->count = s->count;
        for (u32 i = 0U; i < 4U; ++i) {
            cache->index_a[i] = i < s->count ? (u8)s->v[i].ia : 0U;
            cache->index_b[i] = i < s->count ? (u8)s->v[i].ib : 0U;
        }
    }
    *out_iterations = iterations;
    return inside;
}

void nph_physics_distance(const NphPhysicsProxy *a, NphPhysicsPose pose_a, const NphPhysicsProxy *b,
                          NphPhysicsPose pose_b, bool use_radii, NphPhysicsSimplexCache *cache,
                          NphPhysicsDistance *out)
{
    NPH_ASSERT(a != NULL && b != NULL && out != NULL && a->count >= 1U && b->count >= 1U);
    NPH_ASSERT_MSG(a->count <= 256U && b->count <= 256U, "proxies hold at most 256 points");
    GjkSimplex s;
    u32 iterations = 0U;
    const bool overlap = gjk(a, pose_a, b, pose_b, cache, &s, &iterations);
    NphVec3 pa;
    NphVec3 pb;
    simplex_points(&s, &pa, &pb);
    out->iterations = iterations;
    out->simplex_count = s.count;
    f32 distance = overlap ? 0.0F : nph_vec3_distance(pa, pb);
    NphVec3 normal = nph_vec3_unit_y();
    if (distance > 0.0F) {
        normal = nph_vec3_scale(nph_vec3_sub(pb, pa), 1.0F / distance);
    }
    if (use_radii) {
        const f32 ra = a->radius;
        const f32 rb = b->radius;
        if (distance > ra + rb) {
            distance -= ra + rb;
            pa = nph_vec3_mul_add(pa, normal, ra);
            pb = nph_vec3_mul_add(pb, normal, -rb);
        } else {
            // The rounded shapes touch or overlap: both points at the middle.
            const NphVec3 mid = nph_vec3_scale(
                nph_vec3_add(nph_vec3_mul_add(pa, normal, ra), nph_vec3_mul_add(pb, normal, -rb)),
                0.5F);
            pa = mid;
            pb = mid;
            distance = 0.0F;
        }
    }
    out->point_a = pa;
    out->point_b = pb;
    out->normal = normal;
    out->distance = distance;
}

// ---------------------------------------------------------------------------
// EPA
// ---------------------------------------------------------------------------

#define EPA_MAX_VERTICES 72U
#define EPA_MAX_FACES    144U
#define EPA_MAX_EDGES    96U

typedef struct EpaFace {
    NphVec3 normal;
    f32 distance;
    u8 v[3];
    u8 alive;
} EpaFace;

typedef struct Epa {
    GjkVertex vertices[EPA_MAX_VERTICES];
    EpaFace faces[EPA_MAX_FACES];
    u8 edges[EPA_MAX_EDGES][2];
    u32 vertex_count;
    u32 face_count;
    u32 edge_count;
} Epa;

static bool epa_add_face(Epa *e, u32 a, u32 b, u32 c, NphVec3 inside)
{
    if (e->face_count >= EPA_MAX_FACES) {
        return false;
    }
    const NphVec3 pa = e->vertices[a].w;
    NphVec3 n =
        nph_vec3_cross(nph_vec3_sub(e->vertices[b].w, pa), nph_vec3_sub(e->vertices[c].w, pa));
    const f32 len = nph_vec3_length(n);
    if (!(len > 1e-12F)) {
        return false;
    }
    n = nph_vec3_scale(n, 1.0F / len);
    EpaFace *f = &e->faces[e->face_count++];
    f->v[0] = (u8)a;
    f->v[1] = (u8)b;
    f->v[2] = (u8)c;
    // Orientation is kept consistent by construction; `inside` (a point of
    // the polytope's interior) only checks it for the initial tetrahedron.
    if (nph_vec3_dot(n, nph_vec3_sub(pa, inside)) < 0.0F) {
        f->v[1] = (u8)c;
        f->v[2] = (u8)b;
        n = nph_vec3_neg(n);
    }
    f->normal = n;
    f->distance = nph_vec3_dot(n, pa);
    f->alive = 1U;
    return true;
}

static void epa_add_edge(Epa *e, u32 a, u32 b)
{
    // An edge seen twice (once per direction) is between two removed faces.
    for (u32 i = 0U; i < e->edge_count; ++i) {
        if (e->edges[i][0] == b && e->edges[i][1] == a) {
            e->edges[i][0] = e->edges[e->edge_count - 1U][0];
            e->edges[i][1] = e->edges[e->edge_count - 1U][1];
            e->edge_count -= 1U;
            return;
        }
    }
    if (e->edge_count < EPA_MAX_EDGES) {
        e->edges[e->edge_count][0] = (u8)a;
        e->edges[e->edge_count][1] = (u8)b;
        e->edge_count += 1U;
    }
}

static const NphVec3 k_epa_directions[6] = {{1.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F},
                                            {0.0F, 1.0F, 0.0F}, {0.0F, -1.0F, 0.0F},
                                            {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -1.0F}};

// Grows GJK's final simplex to a tetrahedron with volume: when the cores
// only touch, GJK can end on a point, segment, or triangle through the
// origin.
static bool epa_tetrahedron(const NphPhysicsProxy *a, NphPhysicsPose pa, const NphPhysicsProxy *b,
                            NphPhysicsPose pb, GjkSimplex *s)
{
    const f32 eps = 1e-6F;
    for (u32 attempt = 0U; s->count < 4U && attempt < 16U; ++attempt) {
        NphVec3 dirs[8];
        u32 dir_count = 0U;
        if (s->count == 1U) {
            for (u32 i = 0U; i < 6U; ++i) {
                dirs[dir_count++] = k_epa_directions[i];
            }
        } else if (s->count == 2U) {
            const NphVec3 d = nph_vec3_sub(s->v[1].w, s->v[0].w);
            NphVec3 t1;
            NphVec3 t2;
            nph_vec3_orthonormal_basis(nph_vec3_normalize_or(d, nph_vec3_unit_x()), &t1, &t2);
            dirs[dir_count++] = t1;
            dirs[dir_count++] = nph_vec3_neg(t1);
            dirs[dir_count++] = t2;
            dirs[dir_count++] = nph_vec3_neg(t2);
        } else {
            const NphVec3 n = nph_vec3_cross(nph_vec3_sub(s->v[1].w, s->v[0].w),
                                             nph_vec3_sub(s->v[2].w, s->v[0].w));
            dirs[dir_count++] = n;
            dirs[dir_count++] = nph_vec3_neg(n);
        }
        bool added = false;
        for (u32 k = 0U; k < dir_count && !added; ++k) {
            const u32 ia = proxy_support(a, pa, dirs[k]);
            const u32 ib = proxy_support(b, pb, nph_vec3_neg(dirs[k]));
            const GjkVertex w = make_vertex(a, pa, ia, b, pb, ib);
            bool ok = true;
            if (s->count == 1U) {
                ok = nph_vec3_distance_sq(w.w, s->v[0].w) > eps * eps;
            } else if (s->count == 2U) {
                const NphVec3 d = nph_vec3_sub(s->v[1].w, s->v[0].w);
                const NphVec3 x = nph_vec3_cross(d, nph_vec3_sub(w.w, s->v[0].w));
                ok = nph_vec3_length_sq(x) > eps * eps * nph_vec3_length_sq(d);
            } else {
                const NphVec3 n = nph_vec3_normalize_or_zero(nph_vec3_cross(
                    nph_vec3_sub(s->v[1].w, s->v[0].w), nph_vec3_sub(s->v[2].w, s->v[0].w)));
                ok = nph_f32_abs(nph_vec3_dot(n, nph_vec3_sub(w.w, s->v[0].w))) > eps;
            }
            if (ok) {
                s->v[s->count++] = w;
                added = true;
            }
        }
        if (!added) {
            return false; // flat Minkowski difference: no volume to expand
        }
    }
    return s->count == 4U;
}

static bool epa(const NphPhysicsProxy *a, NphPhysicsPose pa, const NphPhysicsProxy *b,
                NphPhysicsPose pb, GjkSimplex *s, NphVec3 *out_normal, f32 *out_depth,
                NphVec3 *out_a, NphVec3 *out_b)
{
    if (!epa_tetrahedron(a, pa, b, pb, s)) {
        return false;
    }
    Epa e;
    e.vertex_count = 4U;
    e.face_count = 0U;
    e.edge_count = 0U;
    NphVec3 centre = nph_vec3_zero();
    for (u32 i = 0U; i < 4U; ++i) {
        e.vertices[i] = s->v[i];
        centre = nph_vec3_add(centre, s->v[i].w);
    }
    centre = nph_vec3_scale(centre, 0.25F);
    if (!epa_add_face(&e, 0U, 1U, 2U, centre) || !epa_add_face(&e, 0U, 3U, 1U, centre) ||
        !epa_add_face(&e, 0U, 2U, 3U, centre) || !epa_add_face(&e, 1U, 3U, 2U, centre)) {
        return false;
    }
    u32 best = 0U;
    for (u32 iteration = 0U; iteration < 64U; ++iteration) {
        best = NPH_U32_MAX;
        f32 best_distance = FLT_MAX;
        for (u32 f = 0U; f < e.face_count; ++f) {
            if (e.faces[f].alive != 0U && e.faces[f].distance < best_distance) {
                best_distance = e.faces[f].distance;
                best = f;
            }
        }
        if (best == NPH_U32_MAX) {
            return false;
        }
        const EpaFace face = e.faces[best];
        const u32 ia = proxy_support(a, pa, face.normal);
        const u32 ib = proxy_support(b, pb, nph_vec3_neg(face.normal));
        const GjkVertex w = make_vertex(a, pa, ia, b, pb, ib);
        const f32 gain = nph_vec3_dot(w.w, face.normal) - face.distance;
        if (gain <= nph_f32_max(1e-5F, 1e-4F * nph_f32_abs(face.distance)) ||
            e.vertex_count >= EPA_MAX_VERTICES) {
            break;
        }
        // Remove every face that sees w and stitch the horizon to it.
        const u32 wi = e.vertex_count;
        e.vertices[e.vertex_count++] = w;
        e.edge_count = 0U;
        for (u32 f = 0U; f < e.face_count; ++f) {
            EpaFace *ff = &e.faces[f];
            if (ff->alive == 0U) {
                continue;
            }
            if (nph_vec3_dot(ff->normal, nph_vec3_sub(w.w, e.vertices[ff->v[0]].w)) > 0.0F) {
                ff->alive = 0U;
                epa_add_edge(&e, ff->v[0], ff->v[1]);
                epa_add_edge(&e, ff->v[1], ff->v[2]);
                epa_add_edge(&e, ff->v[2], ff->v[0]);
            }
        }
        // Compact the dead faces away so the arrays do not fill up.
        u32 live = 0U;
        for (u32 f = 0U; f < e.face_count; ++f) {
            if (e.faces[f].alive != 0U) {
                e.faces[live++] = e.faces[f];
            }
        }
        e.face_count = live;
        bool ok = e.edge_count > 0U;
        for (u32 k = 0U; k < e.edge_count && ok; ++k) {
            ok = epa_add_face(&e, e.edges[k][0], e.edges[k][1], wi, centre);
        }
        if (!ok) {
            // Degenerate stitching: answer with the best face found so far.
            e.face_count = 0U;
            e.faces[e.face_count++] = face;
            best = 0U;
            break;
        }
    }
    const EpaFace face = e.faces[best];
    // The origin's projection on the face, as barycentric weights.
    const NphVec3 p = nph_vec3_scale(face.normal, face.distance);
    const NphVec3 v0 = e.vertices[face.v[0]].w;
    const NphVec3 v1 = e.vertices[face.v[1]].w;
    const NphVec3 v2 = e.vertices[face.v[2]].w;
    const NphVec3 n = nph_vec3_cross(nph_vec3_sub(v1, v0), nph_vec3_sub(v2, v0));
    const f32 nn = nph_vec3_dot(n, n);
    f32 l0 = 1.0F;
    f32 l1 = 0.0F;
    f32 l2 = 0.0F;
    if (nn > 0.0F) {
        l1 = nph_vec3_dot(nph_vec3_cross(nph_vec3_sub(p, v0), nph_vec3_sub(v2, v0)), n) / nn;
        l2 = nph_vec3_dot(nph_vec3_cross(nph_vec3_sub(v1, v0), nph_vec3_sub(p, v0)), n) / nn;
        l0 = 1.0F - l1 - l2;
    }
    const GjkVertex *va = &e.vertices[face.v[0]];
    const GjkVertex *vb = &e.vertices[face.v[1]];
    const GjkVertex *vc = &e.vertices[face.v[2]];
    *out_a = nph_vec3_add(nph_vec3_add(nph_vec3_scale(va->a, l0), nph_vec3_scale(vb->a, l1)),
                          nph_vec3_scale(vc->a, l2));
    *out_b = nph_vec3_add(nph_vec3_add(nph_vec3_scale(va->b, l0), nph_vec3_scale(vb->b, l1)),
                          nph_vec3_scale(vc->b, l2));
    *out_normal = face.normal;
    *out_depth = nph_f32_max(face.distance, 0.0F);
    return true;
}

bool nph_physics_penetration(const NphPhysicsProxy *a, NphPhysicsPose pose_a,
                             const NphPhysicsProxy *b, NphPhysicsPose pose_b,
                             NphPhysicsPenetration *out)
{
    NPH_ASSERT(a != NULL && b != NULL && out != NULL);
    GjkSimplex s;
    u32 iterations = 0U;
    const bool overlap = gjk(a, pose_a, b, pose_b, NULL, &s, &iterations);
    const f32 ra = a->radius;
    const f32 rb = b->radius;
    if (!overlap) {
        NphVec3 pa;
        NphVec3 pb;
        simplex_points(&s, &pa, &pb);
        const f32 d = nph_vec3_distance(pa, pb);
        if (d >= ra + rb || !(d > 0.0F)) {
            return false;
        }
        // Only the rounding overlaps: the cores' closest points decide.
        const NphVec3 n = nph_vec3_scale(nph_vec3_sub(pb, pa), 1.0F / d);
        out->normal = n;
        out->depth = ra + rb - d;
        out->point_a = nph_vec3_mul_add(pa, n, ra);
        out->point_b = nph_vec3_mul_add(pb, n, -rb);
        return true;
    }
    NphVec3 n;
    f32 depth;
    NphVec3 pa;
    NphVec3 pb;
    if (!epa(a, pose_a, b, pose_b, &s, &n, &depth, &pa, &pb)) {
        // A Minkowski difference without volume (two flat shapes in one
        // plane): report a touch along +Y rather than nothing.
        simplex_points(&s, &pa, &pb);
        n = nph_vec3_unit_y();
        depth = 0.0F;
    }
    out->normal = n;
    out->depth = depth + ra + rb;
    out->point_a = nph_vec3_mul_add(pa, n, ra);
    out->point_b = nph_vec3_mul_add(pb, n, -rb);
    return true;
}
