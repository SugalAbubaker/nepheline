// SPDX-License-Identifier: Apache-2.0
// Triangle meshes: building (welding, degenerate triangles, active edges),
// the tree against brute force for boxes and rays, watertight rays through
// shared edges and vertices, faces seen from behind, the validation of
// corrupted blocks, and determinism.

#include <nepheline/physics/mesh.h>

#include "test_physics_support.h"

#include <math.h>
#include <string.h>

static NphPhysicsMesh *build_mesh(NphAllocator *alloc, const NphVec3 *v, u32 nv, const u32 *idx,
                                  u32 nt, f32 weld, bool two_sided)
{
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = nv;
    d.indices = idx;
    d.triangle_count = nt;
    d.weld_tolerance = weld;
    d.two_sided = two_sided ? 1 : 0;
    NphPhysicsMesh *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_physics_mesh_validate(m, m->byte_size));
    return m;
}

// An n by n grid of `cell`-wide squares on the XZ plane, centred on the
// origin, facing up: (n + 1)^2 vertices, 2 n^2 triangles.
static void make_grid(u32 n, f32 cell, NphVec3 *v, u32 *idx)
{
    const f32 half = 0.5F * cell * (f32)n;
    for (u32 z = 0U; z <= n; ++z) {
        for (u32 x = 0U; x <= n; ++x) {
            v[z * (n + 1U) + x] = nph_vec3((f32)x * cell - half, 0.0F, (f32)z * cell - half);
        }
    }
    u32 t = 0U;
    for (u32 z = 0U; z < n; ++z) {
        for (u32 x = 0U; x < n; ++x) {
            const u32 p00 = z * (n + 1U) + x;
            const u32 p10 = p00 + 1U;
            const u32 p01 = p00 + n + 1U;
            const u32 p11 = p01 + 1U;
            const u32 tris[6] = {p00, p01, p11, p00, p11, p10};
            memcpy(&idx[6ULL * t], tris, sizeof tris);
            t += 1U;
        }
    }
}

static u32 popcount3(u8 bits)
{
    return (u32)(bits & 1U) + (u32)((bits >> 1U) & 1U) + (u32)((bits >> 2U) & 1U);
}

static u32 active_edge_count(const NphPhysicsMesh *m)
{
    u32 n = 0U;
    for (u32 t = 0U; t < m->triangle_count; ++t) {
        n += popcount3(nph_physics_mesh_edges(m)[t]);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

NTEST(mesh, a_square_has_a_flat_diagonal)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const NphVec3 v[4] = {nph_vec3(-1.0F, 0.0F, -1.0F), nph_vec3(1.0F, 0.0F, -1.0F),
                          nph_vec3(-1.0F, 0.0F, 1.0F), nph_vec3(1.0F, 0.0F, 1.0F)};
    const u32 idx[6] = {0U, 2U, 3U, 0U, 3U, 1U};
    NphPhysicsMesh *m = build_mesh(alloc, v, 4U, idx, 2U, 0.0F, false);
    NTEST_EXPECT_EQ_UINT(m->triangle_count, 2U);
    NTEST_EXPECT_EQ_UINT(m->vertex_count, 4U);
    NTEST_EXPECT_EQ_UINT(m->node_count, 1U);
    // Each triangle: two open edges, active, and the shared diagonal, flat.
    for (u32 t = 0U; t < 2U; ++t) {
        NTEST_EXPECT_EQ_UINT(popcount3(nph_physics_mesh_edges(m)[t]), 2U);
        NphVec3 p[3];
        nph_physics_mesh_triangle(m, t, p);
        const NphVec3 n = nph_vec3_cross(nph_vec3_sub(p[1], p[0]), nph_vec3_sub(p[2], p[0]));
        NTEST_EXPECT_TRUE(n.y > 0.0F); // the winding kept
    }
    const u32 *orig = nph_physics_mesh_originals(m);
    NTEST_EXPECT_TRUE((orig[0] == 0U && orig[1] == 1U) || (orig[0] == 1U && orig[1] == 0U));
    NTEST_EXPECT_TRUE(nph_vec3_near(m->bounds.min, nph_vec3(-1.0F, 0.0F, -1.0F), 0.0F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m->bounds.max, nph_vec3(1.0F, 0.0F, 1.0F), 0.0F));
    nph_physics_mesh_free(m, alloc);
    test_alloc_end(&ta);
}

NTEST(mesh, welding_joins_split_vertices)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // The square with each triangle's own copies of its corners, as an
    // exporter that splits normals writes it.
    NphVec3 v[6] = {nph_vec3(-1.0F, 0.0F, -1.0F), nph_vec3(-1.0F, 0.0F, 1.0F),
                    nph_vec3(1.0F, 0.0F, 1.0F),   nph_vec3(-1.0F, 0.0F, -1.0F),
                    nph_vec3(1.0F, 0.0F, 1.0F),   nph_vec3(1.0F, 0.0F, -1.0F)};
    const u32 idx[6] = {0U, 1U, 2U, 3U, 4U, 5U};
    // Exact copies weld with any tolerance.
    const f32 tolerances[2] = {0.0F, -1.0F};
    for (u32 i = 0U; i < 2U; ++i) {
        NphPhysicsMesh *m = build_mesh(alloc, v, 6U, idx, 2U, tolerances[i], false);
        NTEST_EXPECT_EQ_UINT(m->vertex_count, 4U);
        NTEST_EXPECT_EQ_UINT(active_edge_count(m), 4U);
        nph_physics_mesh_free(m, alloc);
    }
    // Copies 1e-7 apart weld within the default (1e-6 of the extent), not
    // exactly; 1 mm apart they stay two, and the diagonal is two open edges.
    v[3] = nph_vec3_add(v[3], nph_vec3(1e-7F, 0.0F, 0.0F));
    v[4] = nph_vec3_add(v[4], nph_vec3(0.0F, 0.0F, -1e-7F));
    NphPhysicsMesh *m = build_mesh(alloc, v, 6U, idx, 2U, 0.0F, false);
    NTEST_EXPECT_EQ_UINT(m->vertex_count, 4U);
    nph_physics_mesh_free(m, alloc);
    m = build_mesh(alloc, v, 6U, idx, 2U, -1.0F, false);
    NTEST_EXPECT_EQ_UINT(m->vertex_count, 6U);
    nph_physics_mesh_free(m, alloc);
    v[3] = nph_vec3(-0.999F, 0.0F, -1.0F);
    m = build_mesh(alloc, v, 6U, idx, 2U, 0.0F, false);
    NTEST_EXPECT_TRUE(m->vertex_count >= 5U);
    NTEST_EXPECT_EQ_UINT(active_edge_count(m), 6U);
    nph_physics_mesh_free(m, alloc);
    test_alloc_end(&ta);
}

NTEST(mesh, degenerate_triangles_are_dropped)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const NphVec3 v[6] = {nph_vec3(-1.0F, 0.0F, -1.0F), nph_vec3(1.0F, 0.0F, -1.0F),
                          nph_vec3(-1.0F, 0.0F, 1.0F),  nph_vec3(1.0F, 0.0F, 1.0F),
                          nph_vec3(0.0F, 0.0F, 0.0F),   nph_vec3(0.0F, 0.0F, 0.0F)};
    // A repeated index, the square's first half, two vertices welded into
    // one, the square's second half, a collinear triangle.
    const u32 idx[15] = {0U, 0U, 3U, 0U, 2U, 3U, 4U, 5U, 1U, 0U, 3U, 1U, 0U, 4U, 3U};
    NphPhysicsMesh *m = build_mesh(alloc, v, 6U, idx, 5U, 0.0F, false);
    NTEST_EXPECT_EQ_UINT(m->triangle_count, 2U);
    NTEST_EXPECT_EQ_UINT(m->vertex_count, 4U); // the unused centre is not kept
    const u32 *orig = nph_physics_mesh_originals(m);
    NTEST_EXPECT_TRUE((orig[0] == 1U && orig[1] == 3U) || (orig[0] == 3U && orig[1] == 1U));
    nph_physics_mesh_free(m, alloc);
    // Nothing left is an error.
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = 6U;
    d.indices = idx;
    d.triangle_count = 1U;
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(m);
    test_alloc_end(&ta);
}

NTEST(mesh, bad_descriptions_are_refused)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphVec3 v[3] = {nph_vec3(0.0F, 0.0F, 0.0F), nph_vec3(0.0F, 0.0F, 1.0F),
                    nph_vec3(1.0F, 0.0F, 0.0F)};
    u32 idx[3] = {0U, 1U, 2U};
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = 3U;
    d.indices = idx;
    d.triangle_count = 1U;
    NphPhysicsMesh *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_OK);
    nph_physics_mesh_free(m, alloc);
    NphPhysicsMeshDesc bad = d;
    bad.triangle_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&bad, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    idx[2] = 3U;
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    idx[2] = 2U;
    v[1].y = (f32)NAN;
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    v[1].y = 2.0F * NPH_PHYSICS_MAX_COORDINATE; // beyond the limit
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    v[1].y = 0.0F;
    bad = d;
    bad.weld_tolerance = (f32)NAN;
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&bad, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    bad = d;
    bad.active_edge_angle = -0.1F;
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&bad, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(NULL, alloc, &m), NPH_RESULT_INVALID_ARGUMENT);
    test_alloc_end(&ta);
}

// Two triangles sharing the edge from (0, 0, -1) to (0, 0, 1): the first
// flat at y = 0 for x < 0, the second rising (convex, `degrees` > 0) or
// falling (concave) for x > 0.
static NphPhysicsMesh *hinge_mesh(NphAllocator *alloc, f32 degrees, f32 limit_degrees,
                                  bool two_sided)
{
    const f32 a = degrees * 3.14159265F / 180.0F;
    // Convex: the second face bends down, away from the first's normal.
    const NphVec3 v[4] = {nph_vec3(0.0F, 0.0F, -1.0F), nph_vec3(0.0F, 0.0F, 1.0F),
                          nph_vec3(-1.0F, 0.0F, 0.0F),
                          nph_vec3(nph_f32_cos(a), -nph_f32_sin(a), 0.0F)};
    const u32 idx[6] = {0U, 2U, 1U, 0U, 1U, 3U};
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = 4U;
    d.indices = idx;
    d.triangle_count = 2U;
    d.active_edge_angle = limit_degrees * 3.14159265F / 180.0F;
    d.two_sided = two_sided ? 1 : 0;
    NphPhysicsMesh *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_OK);
    return m;
}

// Whether the shared edge (the one on x = 0) is active in each triangle.
static bool hinge_active(const NphPhysicsMesh *m)
{
    bool any = false;
    for (u32 t = 0U; t < 2U; ++t) {
        NphVec3 p[3];
        nph_physics_mesh_triangle(m, t, p);
        for (u32 e = 0U; e < 3U; ++e) {
            const NphVec3 a = p[e];
            const NphVec3 b = p[(e + 1U) % 3U];
            if (a.x == 0.0F && b.x == 0.0F && a.y == 0.0F && b.y == 0.0F) {
                any = any || (nph_physics_mesh_edges(m)[t] & (1U << e)) != 0U;
            }
        }
    }
    return any;
}

NTEST(mesh, active_edges_are_convex_corners_and_open_edges)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // A ridge: convex by 10 degrees is active, by 3 (under the default 5)
    // is not, nor by 10 under a 15 degree limit.
    NphPhysicsMesh *m = hinge_mesh(alloc, 10.0F, 0.0F, false);
    NTEST_EXPECT_TRUE(hinge_active(m));
    nph_physics_mesh_free(m, alloc);
    m = hinge_mesh(alloc, 3.0F, 0.0F, false);
    NTEST_EXPECT_FALSE(hinge_active(m));
    nph_physics_mesh_free(m, alloc);
    m = hinge_mesh(alloc, 10.0F, 15.0F, false);
    NTEST_EXPECT_FALSE(hinge_active(m));
    nph_physics_mesh_free(m, alloc);
    // A valley (concave) is inactive from the front; seen from both sides,
    // its back is a ridge.
    m = hinge_mesh(alloc, -30.0F, 0.0F, false);
    NTEST_EXPECT_FALSE(hinge_active(m));
    nph_physics_mesh_free(m, alloc);
    m = hinge_mesh(alloc, -30.0F, 0.0F, true);
    NTEST_EXPECT_TRUE(hinge_active(m));
    nph_physics_mesh_free(m, alloc);
    // A cube: its twelve edges convex and active, its faces' diagonals flat.
    NphVec3 v[8];
    for (u32 i = 0U; i < 8U; ++i) {
        v[i] = nph_vec3((i & 1U) != 0U ? 1.0F : -1.0F, (i & 2U) != 0U ? 1.0F : -1.0F,
                        (i & 4U) != 0U ? 1.0F : -1.0F);
    }
    // Outward-facing, two triangles a face.
    const u32 idx[36] = {0U, 2U, 3U, 0U, 3U, 1U, 4U, 5U, 7U, 4U, 7U, 6U, 0U, 1U, 5U, 0U, 5U, 4U,
                         2U, 6U, 7U, 2U, 7U, 3U, 0U, 4U, 6U, 0U, 6U, 2U, 1U, 3U, 7U, 1U, 7U, 5U};
    m = build_mesh(alloc, v, 8U, idx, 12U, 0.0F, false);
    NTEST_EXPECT_EQ_UINT(active_edge_count(m), 24U); // 12 edges, two sides each
    for (u32 t = 0U; t < 12U; ++t) {
        NTEST_EXPECT_EQ_UINT(popcount3(nph_physics_mesh_edges(m)[t]), 2U);
        NphVec3 p[3];
        nph_physics_mesh_triangle(m, t, p);
        const NphVec3 n = nph_vec3_cross(nph_vec3_sub(p[1], p[0]), nph_vec3_sub(p[2], p[0]));
        const NphVec3 c = nph_vec3_scale(nph_vec3_add(nph_vec3_add(p[0], p[1]), p[2]), 1.0F / 3.0F);
        NTEST_EXPECT_TRUE(nph_vec3_dot(n, c) > 0.0F); // outward
    }
    nph_physics_mesh_free(m, alloc);
    test_alloc_end(&ta);
}

NTEST(mesh, a_large_grid_builds_a_shallow_valid_tree)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const u32 n = TEST_SCALE >= 8U ? 200U : 64U;
    const u32 nv = (n + 1U) * (n + 1U);
    const u32 nt = 2U * n * n;
    NphVec3 *v = NPH_ALLOC_ARRAY(alloc, NphVec3, nv);
    u32 *idx = NPH_ALLOC_ARRAY(alloc, u32, 3ULL * nt);
    NTEST_ASSERT_NOT_NULL(v);
    NTEST_ASSERT_NOT_NULL(idx);
    make_grid(n, 0.5F, v, idx);
    NphPhysicsMesh *m = build_mesh(alloc, v, nv, idx, nt, 0.0F, false); // validates
    NTEST_EXPECT_EQ_UINT(m->triangle_count, nt);
    NTEST_EXPECT_EQ_UINT(m->vertex_count, nv);
    NTEST_EXPECT_TRUE(m->node_count <= nt / 3U + 1U);
    // Only the border is active: 4 n edges, each in one triangle.
    NTEST_EXPECT_EQ_UINT(active_edge_count(m), 4U * n);
    NTEST_LOG("%u triangles: %u nodes, %u bytes", nt, m->node_count, m->byte_size);
    nph_physics_mesh_free(m, alloc);
    NPH_FREE_ARRAY(alloc, NphVec3, v, nv);
    NPH_FREE_ARRAY(alloc, u32, idx, 3ULL * nt);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// Queries against brute force
// ---------------------------------------------------------------------------

#define SOUP 2000U

// Random triangles up to 1 m across in a 20 m cube.
static NphPhysicsMesh *soup(NphAllocator *alloc, u64 seed, bool two_sided)
{
    NphPcg32 r = test_rng(seed);
    NphVec3 *v = NPH_ALLOC_ARRAY(alloc, NphVec3, 3ULL * SOUP);
    u32 *idx = NPH_ALLOC_ARRAY(alloc, u32, 3ULL * SOUP);
    NTEST_ASSERT_NOT_NULL(v);
    NTEST_ASSERT_NOT_NULL(idx);
    for (u32 t = 0U; t < SOUP; ++t) {
        const NphVec3 c = test_vec3(&r, -10.0F, 10.0F);
        for (u32 k = 0U; k < 3U; ++k) {
            v[3U * t + k] = nph_vec3_add(c, test_vec3(&r, -0.5F, 0.5F));
            idx[3U * t + k] = 3U * t + k;
        }
    }
    NphPhysicsMesh *m = build_mesh(alloc, v, 3U * SOUP, idx, SOUP, -1.0F, two_sided);
    NPH_FREE_ARRAY(alloc, NphVec3, v, 3ULL * SOUP);
    NPH_FREE_ARRAY(alloc, u32, idx, 3ULL * SOUP);
    return m;
}

typedef struct Found {
    u8 hit[SOUP];
    u32 count;
    u32 stop_after;
} Found;

static bool record(void *user, u32 triangle)
{
    Found *f = user;
    f->hit[triangle] += 1U;
    f->count += 1U;
    return f->count != f->stop_after;
}

NTEST(mesh, overlap_finds_what_brute_force_finds)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsMesh *m = soup(alloc, 5U, false);
    NTEST_ASSERT_EQ_UINT(m->triangle_count, SOUP);
    NphPcg32 r = test_rng(6U);
    static Found f;
    for (u32 q = 0U; q < 200U; ++q) {
        const NphVec3 c = test_vec3(&r, -11.0F, 11.0F);
        const NphVec3 h = test_vec3(&r, 0.05F, 2.0F);
        const NphAABB box = {nph_vec3_sub(c, h), nph_vec3_add(c, h)};
        memset(&f, 0, sizeof f);
        nph_physics_mesh_overlap(m, box, record, &f);
        u32 expected = 0U;
        for (u32 t = 0U; t < SOUP; ++t) {
            NphVec3 p[3];
            nph_physics_mesh_triangle(m, t, p);
            const NphAABB tb = {nph_vec3_min(nph_vec3_min(p[0], p[1]), p[2]),
                                nph_vec3_max(nph_vec3_max(p[0], p[1]), p[2])};
            const bool overlaps = nph_aabb_overlaps(tb, box);
            expected += overlaps ? 1U : 0U;
            NTEST_EXPECT_EQ_UINT(f.hit[t], overlaps ? 1U : 0U); // each once
        }
        NTEST_EXPECT_EQ_UINT(f.count, expected);
    }
    // Returning false ends the query.
    memset(&f, 0, sizeof f);
    f.stop_after = 3U;
    nph_physics_mesh_overlap(m, m->bounds, record, &f);
    NTEST_EXPECT_EQ_UINT(f.count, 3U);
    nph_physics_mesh_free(m, alloc);
    test_alloc_end(&ta);
}

// An independent reference: Moller and Trumbore's test in 64 bits.
static f64 reference_hit(NphVec3 o, NphVec3 d, const NphVec3 p[3], bool two_sided, bool *back)
{
    const f64 e1[3] = {(f64)p[1].x - (f64)p[0].x, (f64)p[1].y - (f64)p[0].y,
                       (f64)p[1].z - (f64)p[0].z};
    const f64 e2[3] = {(f64)p[2].x - (f64)p[0].x, (f64)p[2].y - (f64)p[0].y,
                       (f64)p[2].z - (f64)p[0].z};
    const f64 dv[3] = {(f64)d.x, (f64)d.y, (f64)d.z};
    const f64 pv[3] = {dv[1] * e2[2] - dv[2] * e2[1], dv[2] * e2[0] - dv[0] * e2[2],
                       dv[0] * e2[1] - dv[1] * e2[0]};
    const f64 det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (det == 0.0) {
        return -1.0;
    }
    *back = det < 0.0; // det is -d.n: positive from the front
    if (*back && !two_sided) {
        return -1.0;
    }
    const f64 tv[3] = {(f64)o.x - (f64)p[0].x, (f64)o.y - (f64)p[0].y, (f64)o.z - (f64)p[0].z};
    const f64 u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
    if (u < 0.0 || u > 1.0) {
        return -1.0;
    }
    const f64 qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                       tv[0] * e1[1] - tv[1] * e1[0]};
    const f64 w = (dv[0] * qv[0] + dv[1] * qv[1] + dv[2] * qv[2]) / det;
    if (w < 0.0 || u + w > 1.0) {
        return -1.0;
    }
    const f64 t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
    return t >= 0.0 && t <= 1.0 ? t : -1.0;
}

NTEST(mesh, raycasts_find_the_closest_hit)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    for (u32 sided = 0U; sided < 2U; ++sided) {
        NphPhysicsMesh *m = soup(alloc, 7U + sided, sided == 1U);
        NphPcg32 r = test_rng(8U);
        u32 hits = 0U;
        for (u32 q = 0U; q < 500U; ++q) {
            const NphVec3 o = test_vec3(&r, -12.0F, 12.0F);
            NphVec3 d = nph_vec3_scale(test_unit(&r), test_range(&r, 1.0F, 30.0F));
            if ((q & 1U) == 0U) {
                // Half are aimed through a triangle, a little beyond it.
                NphVec3 p[3];
                nph_physics_mesh_triangle(m, nph_pcg32_next_bounded(&r, SOUP), p);
                const f32 u = test_range(&r, 0.05F, 0.9F);
                const f32 w = test_range(&r, 0.05F, 0.95F - u);
                const NphVec3 target =
                    nph_vec3_add(p[0], nph_vec3_add(nph_vec3_scale(nph_vec3_sub(p[1], p[0]), u),
                                                    nph_vec3_scale(nph_vec3_sub(p[2], p[0]), w)));
                d = nph_vec3_scale(nph_vec3_sub(target, o), 1.2F);
            }
            f64 best = 2.0;
            for (u32 t = 0U; t < SOUP; ++t) {
                NphVec3 p[3];
                nph_physics_mesh_triangle(m, t, p);
                bool back = false;
                const f64 f = reference_hit(o, d, p, sided == 1U, &back);
                if (f >= 0.0 && f < best) {
                    best = f;
                }
            }
            NphPhysicsMeshHit hit;
            const bool found = nph_physics_mesh_raycast(m, o, d, &hit);
            // Rays that graze an edge may go either way between the two
            // methods' roundings: compare the rest.
            if (found != (best <= 1.0)) {
                NTEST_LOG("ray %u: found %d, reference %.9f", q, found ? 1 : 0, best);
            }
            NTEST_EXPECT_EQ_INT(found ? 1 : 0, best <= 1.0 ? 1 : 0);
            if (found && best <= 1.0) {
                hits += 1U;
                NTEST_EXPECT_NEAR_F64((f64)hit.fraction, best, 1e-5);
                NphVec3 p[3];
                nph_physics_mesh_triangle(m, hit.triangle, p);
                const NphVec3 n = nph_vec3_normalize(
                    nph_vec3_cross(nph_vec3_sub(p[1], p[0]), nph_vec3_sub(p[2], p[0])));
                NTEST_EXPECT_TRUE(nph_vec3_dot(hit.normal, d) <= 0.0F); // towards the origin
                NTEST_EXPECT_NEAR_F32(nph_f32_abs(nph_vec3_dot(hit.normal, n)), 1.0F, 1e-5F);
                NTEST_EXPECT_EQ_UINT(hit.original, nph_physics_mesh_originals(m)[hit.triangle]);
            }
        }
        NTEST_LOG("%s: %u of 500 rays hit", sided == 1U ? "two-sided" : "one-sided", hits);
        NTEST_EXPECT_TRUE(hits > (sided == 1U ? 200U : 100U));
        nph_physics_mesh_free(m, alloc);
    }
    test_alloc_end(&ta);
}

NTEST(mesh, rays_through_shared_edges_and_vertices_never_slip)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    enum {
        N = 8
    };
    NphVec3 v[(N + 1) * (N + 1)];
    u32 idx[6 * N * N];
    make_grid(N, 1.0F, v, idx);
    NphPhysicsMesh *m = build_mesh(alloc, v, (N + 1) * (N + 1), idx, 2 * N * N, 0.0F, false);
    // Straight down, and slanted, through every vertex, every edge's middle,
    // and every cell's diagonal: exactly on what triangles share.
    u32 misses = 0U;
    u32 rays = 0U;
    const NphVec3 dirs[3] = {nph_vec3(0.0F, -2.0F, 0.0F), nph_vec3(0.3F, -2.0F, 0.7F),
                             nph_vec3(-1.0F, -2.0F, 1.0F)};
    for (u32 z = 1U; z < 2U * N; ++z) {
        for (u32 x = 1U; x < 2U * N; ++x) {
            const NphVec3 target =
                nph_vec3(0.5F * (f32)x - 0.5F * (f32)N, 0.0F, 0.5F * (f32)z - 0.5F * (f32)N);
            for (u32 k = 0U; k < 3U; ++k) {
                const NphVec3 o = nph_vec3_sub(target, nph_vec3_scale(dirs[k], 0.5F));
                NphPhysicsMeshHit hit;
                rays += 1U;
                if (!nph_physics_mesh_raycast(m, o, dirs[k], &hit)) {
                    misses += 1U;
                    continue;
                }
                NTEST_EXPECT_NEAR_F32(hit.fraction, 0.5F, 1e-6F);
                NTEST_EXPECT_NEAR_F32(hit.normal.y, 1.0F, 1e-6F);
            }
        }
    }
    NTEST_LOG("%u rays through shared features, %u missed", rays, misses);
    NTEST_EXPECT_EQ_UINT(misses, 0U);
    nph_physics_mesh_free(m, alloc);
    test_alloc_end(&ta);
}

NTEST(mesh, one_sided_faces_are_hit_from_the_front)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const NphVec3 v[3] = {nph_vec3(-1.0F, 0.0F, -1.0F), nph_vec3(-1.0F, 0.0F, 1.0F),
                          nph_vec3(1.0F, 0.0F, 0.0F)};
    const u32 idx[3] = {0U, 1U, 2U};
    const NphVec3 above = nph_vec3(-0.2F, 1.0F, 0.1F);
    const NphVec3 below = nph_vec3(-0.2F, -1.0F, 0.1F);
    for (u32 sided = 0U; sided < 2U; ++sided) {
        NphPhysicsMesh *m = build_mesh(alloc, v, 3U, idx, 1U, 0.0F, sided == 1U);
        NphPhysicsMeshHit hit;
        NTEST_ASSERT_TRUE(nph_physics_mesh_raycast(m, above, nph_vec3(0.0F, -2.0F, 0.0F), &hit));
        NTEST_EXPECT_NEAR_F32(hit.fraction, 0.5F, 1e-6F);
        NTEST_EXPECT_NEAR_F32(hit.normal.y, 1.0F, 1e-6F);
        NTEST_EXPECT_EQ_INT(hit.back, 0);
        const bool from_below =
            nph_physics_mesh_raycast(m, below, nph_vec3(0.0F, 2.0F, 0.0F), &hit);
        NTEST_EXPECT_EQ_INT(from_below ? 1 : 0, (int)sided);
        if (from_below) {
            NTEST_EXPECT_EQ_INT(hit.back, 1);
            NTEST_EXPECT_NEAR_F32(hit.normal.y, -1.0F, 1e-6F);
        }
        // Too short to reach, and pointing away: misses.
        NTEST_EXPECT_FALSE(nph_physics_mesh_raycast(m, above, nph_vec3(0.0F, -0.5F, 0.0F), &hit));
        NTEST_EXPECT_FALSE(nph_physics_mesh_raycast(m, above, nph_vec3(0.0F, 2.0F, 0.0F), &hit));
        nph_physics_mesh_free(m, alloc);
    }
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// Validation and determinism
// ---------------------------------------------------------------------------

NTEST(mesh, validation_rejects_corrupted_blocks)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    enum {
        N = 6
    };
    NphVec3 v[(N + 1) * (N + 1)];
    u32 idx[6 * N * N];
    make_grid(N, 1.0F, v, idx);
    NphPhysicsMesh *m = build_mesh(alloc, v, (N + 1) * (N + 1), idx, 2 * N * N, 0.0F, false);
    const u32 size = m->byte_size;
    NphPhysicsMesh *c = NPH_ALLOC(alloc, size + 16U, 16U);
    NTEST_ASSERT_NOT_NULL(c);
#define RESET() memcpy(c, m, size)
    RESET();
    NTEST_EXPECT_TRUE(nph_physics_mesh_validate(c, size));
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size - 1U)); // shorter than byte_size
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, 32U));
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(NULL, size));
    c->triangles_offset = size; // the array past the end
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    c->nodes_offset += 4U; // misaligned
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    ((u32 *)((u8 *)c + c->triangles_offset))[5] = c->vertex_count; // an index out of range
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    ((NphVec3 *)((u8 *)c + c->vertices_offset))[2].x = (f32)NAN;
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    ((u8 *)c + c->edges_offset)[3] = 0x80U;
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    NphPhysicsMeshNode *nodes = (NphPhysicsMeshNode *)((u8 *)c + c->nodes_offset);
    u32 inner_slot = NPH_U32_MAX;
    for (u32 k = 0U; k < 4U; ++k) {
        if (!nph_physics_mesh_child_is_leaf(nodes[0].child[k]) &&
            nodes[0].child[k] != NPH_PHYSICS_MESH_CHILD_EMPTY) {
            inner_slot = k;
        }
    }
    NTEST_ASSERT_TRUE(inner_slot != NPH_U32_MAX);
    nodes[0].child[inner_slot] = 0U; // the root its own child: a cycle
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    nodes[0].max_y[inner_slot] = -1.0F; // a box that does not hold its child
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    c->node_count += 1U; // a node no parent reaches
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    c->bounds.max.x = 2.0F * NPH_PHYSICS_MAX_COORDINATE; // beyond the limit
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    RESET();
    nodes[0].min_z[inner_slot] = (f32)NAN; // a box no comparison rejects
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(c, size));
    // A misaligned block.
    memcpy((u8 *)c + 4U, m, size);
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate((u8 *)c + 4U, size));
    // Random damage never crashes validation, and what passes is safe to
    // query. (Each rejection would log a warning: quiet them.)
    const NphLogLevel level = nph_log_get_level(&NPH_LOG_PHYSICS);
    nph_log_set_level(&NPH_LOG_PHYSICS, NPH_LOG_LEVEL_ERROR);
    NphPcg32 r = test_rng(9U);
    u32 passed = 0U;
    for (u32 trial = 0U; trial < 2000U; ++trial) {
        RESET();
        const u32 flips = 1U + nph_pcg32_next_bounded(&r, 4U);
        for (u32 k = 0U; k < flips; ++k) {
            const u32 at = nph_pcg32_next_bounded(&r, size);
            ((u8 *)c)[at] ^= (u8)(1U << nph_pcg32_next_bounded(&r, 8U));
        }
        if (nph_physics_mesh_validate(c, size)) {
            passed += 1U;
            static Found f;
            memset(&f, 0, sizeof f);
            nph_physics_mesh_overlap(c, c->bounds, record, &f);
            NphPhysicsMeshHit hit;
            NPH_UNUSED(nph_physics_mesh_raycast(c, nph_vec3(0.1F, 5.0F, 0.2F),
                                                nph_vec3(0.0F, -10.0F, 0.0F), &hit));
        }
    }
    NTEST_LOG("%u of 2000 damaged blocks still valid (damage to coordinates and slack)", passed);
#undef RESET
    NPH_FREE(alloc, c, size + 16U, 16U);
    nph_physics_mesh_free(m, alloc);
    // An empty slot's box must be the empty one: an overlap query would take
    // any other for a child (one triangle: the root has three empty slots).
    NphPhysicsMesh *one = build_mesh(alloc, v, (N + 1) * (N + 1), idx, 1U, 0.0F, false);
    NphPhysicsMesh *e = NPH_ALLOC(alloc, one->byte_size, 16U);
    NTEST_ASSERT_NOT_NULL(e);
    memcpy(e, one, one->byte_size);
    NTEST_EXPECT_TRUE(nph_physics_mesh_validate(e, e->byte_size));
    NphPhysicsMeshNode *root = (NphPhysicsMeshNode *)((u8 *)e + e->nodes_offset);
    NTEST_ASSERT_EQ_UINT(root->child[3], NPH_PHYSICS_MESH_CHILD_EMPTY);
    root->min_x[3] = -1.0F;
    root->min_y[3] = -1.0F;
    root->min_z[3] = -1.0F;
    root->max_x[3] = 1.0F;
    root->max_y[3] = 1.0F;
    root->max_z[3] = 1.0F;
    NTEST_EXPECT_FALSE(nph_physics_mesh_validate(e, e->byte_size));
    nph_log_set_level(&NPH_LOG_PHYSICS, level);
    NPH_FREE(alloc, e, one->byte_size, 16U);
    nph_physics_mesh_free(one, alloc);
    test_alloc_end(&ta);
}

NTEST(mesh, builds_are_deterministic)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPhysicsMesh *a = soup(alloc, 11U, false);
    NphPhysicsMesh *b = soup(alloc, 11U, false);
    NTEST_ASSERT_EQ_UINT(a->byte_size, b->byte_size);
    NTEST_EXPECT_EQ_INT(memcmp(a, b, a->byte_size), 0);
    nph_physics_mesh_free(a, alloc);
    nph_physics_mesh_free(b, alloc);
    test_alloc_end(&ta);
}
