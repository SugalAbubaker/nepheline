// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the physics tests: deterministic random geometry, a
// leak-checking allocator, and manifold property checks.

#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/assert.h>
#include <nepheline/math/random.h>
#include <nepheline/physics/aabb_tree.h>
#include <nepheline/physics/collide.h>
#include <nepheline/physics/geometry.h>
#include <nepheline/physics/heightfield.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/mesh.h>

#include <ntest/ntest.h>

#include <string.h>

// Work counts scale down where the sanitizers make everything slower.
#if NPH_TSAN_ENABLED
#define TEST_SCALE 1U
#elif NPH_CONFIG_DEBUG
#define TEST_SCALE 2U
#else
#define TEST_SCALE 8U
#endif

// ---------------------------------------------------------------------------
// Random geometry
// ---------------------------------------------------------------------------

static inline NphPcg32 test_rng(u64 seed)
{
    NphPcg32 r;
    nph_pcg32_seed(&r, seed, 0x9E3779B97F4A7C15ULL);
    return r;
}

static inline f32 test_range(NphPcg32 *r, f32 lo, f32 hi)
{
    return nph_pcg32_next_range_f32(r, lo, hi);
}

static inline NphVec3 test_vec3(NphPcg32 *r, f32 lo, f32 hi)
{
    return nph_vec3(test_range(r, lo, hi), test_range(r, lo, hi), test_range(r, lo, hi));
}

// A uniformly distributed unit vector.
static inline NphVec3 test_unit(NphPcg32 *r)
{
    return nph_sample_sphere_uniform(nph_vec2(nph_pcg32_next_f32(r), nph_pcg32_next_f32(r)));
}

// A uniformly distributed rotation (Shoemake, "Uniform Random Rotations",
// Graphics Gems III, 1992).
static inline NphQuat test_rotation(NphPcg32 *r)
{
    const f32 u1 = nph_pcg32_next_f32(r);
    const f32 u2 = nph_pcg32_next_f32(r) * 6.2831853F;
    const f32 u3 = nph_pcg32_next_f32(r) * 6.2831853F;
    const f32 a = nph_f32_sqrt(1.0F - u1);
    const f32 b = nph_f32_sqrt(u1);
    return nph_quat_normalize(nph_quat(a * nph_f32_sin(u2), a * nph_f32_cos(u2),
                                       b * nph_f32_sin(u3), b * nph_f32_cos(u3)));
}

static inline NphPhysicsPose test_pose(NphPcg32 *r, f32 extent)
{
    return nph_physics_pose(test_vec3(r, -extent, extent), test_rotation(r));
}

// ---------------------------------------------------------------------------
// A leak-checking allocator: every test that allocates ends with
// test_alloc_end, which fails the test on a leak.
// ---------------------------------------------------------------------------

typedef struct TestAlloc {
    NphDebugAllocator debug;
} TestAlloc;

static inline NphAllocator *test_alloc_begin(TestAlloc *t)
{
    nph_debug_allocator_init(&t->debug, nph_allocator_system());
    return nph_debug_allocator_allocator(&t->debug);
}

static inline void test_alloc_end(TestAlloc *t)
{
    const u32 leaks = nph_debug_allocator_destroy(&t->debug, NULL, NULL);
    NTEST_EXPECT_EQ_UINT(leaks, 0U);
}

// ---------------------------------------------------------------------------
// Hulls
// ---------------------------------------------------------------------------

static inline NphPhysicsHull *test_hull(NphAllocator *alloc, const NphVec3 *points, u32 count,
                                        u32 max_vertices)
{
    NphPhysicsHullDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.points = points;
    desc.point_count = count;
    desc.max_vertices = max_vertices;
    NphPhysicsHull *hull = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_build(&desc, alloc, &hull), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(hull);
    return hull;
}

// The hull of `count` random points in a ball of `radius`, squashed by
// `scale` so random hulls are not all round.
static inline NphPhysicsHull *test_random_hull(NphAllocator *alloc, NphPcg32 *r, u32 count,
                                               f32 radius, NphVec3 scale)
{
    NphVec3 pts[128];
    NTEST_ASSERT_TRUE(count <= 128U);
    for (u32 i = 0U; i < count; ++i) {
        const NphVec3 u = test_unit(r);
        const f32 d = radius * nph_f32_sqrt(nph_pcg32_next_f32(r) * 0.5F + 0.5F);
        pts[i] = nph_vec3_mul(nph_vec3_scale(u, d), scale);
    }
    return test_hull(alloc, pts, count, 0U);
}

// The signed distance of p from the hull (the largest plane distance:
// exact inside and on faces, a lower bound outside).
static inline f32 test_hull_plane_distance(const NphPhysicsHull *hull, NphVec3 p)
{
    const NphPlane *planes = nph_physics_hull_planes(hull);
    f32 m = -1e30F;
    for (u32 f = 0U; f < hull->face_count; ++f) {
        m = nph_f32_max(m, nph_plane_distance(planes[f], p));
    }
    return m;
}

// The exact distance from an outside point to the hull's surface, by brute
// force over the fan triangles of every face.
static inline f32 test_hull_surface_distance(const NphPhysicsHull *hull, NphVec3 p)
{
    const NphVec3 *v = nph_physics_hull_vertices(hull);
    const NphPhysicsHullEdge *e = nph_physics_hull_edges(hull);
    const u16 *faces = nph_physics_hull_faces(hull);
    f32 best = 1e30F;
    for (u32 f = 0U; f < hull->face_count; ++f) {
        const u32 first = faces[f];
        const NphVec3 a = v[e[first].origin];
        u32 cur = e[first].next;
        while (e[cur].next != first) {
            const NphVec3 b = v[e[cur].origin];
            const NphVec3 c = v[e[e[cur].next].origin];
            const NphVec3 q = nph_closest_point_on_triangle(a, b, c, p);
            best = nph_f32_min(best, nph_vec3_distance(p, q));
            cur = e[cur].next;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

static inline NphPhysicsMesh *test_mesh(NphAllocator *alloc, const NphVec3 *v, u32 nv,
                                        const u32 *idx, u32 nt, bool two_sided)
{
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = nv;
    d.indices = idx;
    d.triangle_count = nt;
    d.two_sided = two_sided ? 1 : 0;
    NphPhysicsMesh *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(m);
    return m;
}

#define TEST_GRID_MAX 48U

// An n by n grid of square cells `cell` metres wide from (x0, z0), facing
// +Y, at heights `y` ((n + 1)^2 samples, x-major; NULL: the plane y = 0).
// The cells' diagonals alternate, so seams run three ways and vertices join
// four or eight triangles.
static inline NphPhysicsMesh *test_grid_mesh(NphAllocator *alloc, u32 n, f32 cell, f32 x0, f32 z0,
                                             const f32 *y, bool two_sided)
{
    static NphVec3 v[(TEST_GRID_MAX + 1U) * (TEST_GRID_MAX + 1U)];
    static u32 idx[6U * TEST_GRID_MAX * TEST_GRID_MAX];
    NTEST_ASSERT_TRUE(n <= TEST_GRID_MAX);
    for (u32 i = 0U; i <= n; ++i) {
        for (u32 j = 0U; j <= n; ++j) {
            const u32 k = i * (n + 1U) + j;
            v[k] = nph_vec3(x0 + (f32)i * cell, y != NULL ? y[k] : 0.0F, z0 + (f32)j * cell);
        }
    }
    u32 k = 0U;
    for (u32 i = 0U; i < n; ++i) {
        for (u32 j = 0U; j < n; ++j) {
            const u32 a = i * (n + 1U) + j; // (x, z)
            const u32 b = a + 1U;           // (x, z + 1)
            const u32 c = a + n + 1U;       // (x + 1, z)
            const u32 d = c + 1U;           // (x + 1, z + 1)
            const u32 quad[2][6] = {{a, b, c, c, b, d}, {a, d, c, a, b, d}};
            for (u32 q = 0U; q < 6U; ++q) {
                idx[k++] = quad[(i + j) & 1U][q];
            }
        }
    }
    return test_mesh(alloc, v, (n + 1U) * (n + 1U), idx, 2U * n * n, two_sided);
}

// A valley along Z, 4 m wide: two slopes at 45 degrees meeting at a concave
// crease on the Z axis, facing up and in.
static inline NphPhysicsMesh *test_valley_mesh(NphAllocator *alloc)
{
    const NphVec3 v[6] = {nph_vec3(-2.0F, 2.0F, -2.0F), nph_vec3(-2.0F, 2.0F, 2.0F),
                          nph_vec3(0.0F, 0.0F, -2.0F),  nph_vec3(0.0F, 0.0F, 2.0F),
                          nph_vec3(2.0F, 2.0F, -2.0F),  nph_vec3(2.0F, 2.0F, 2.0F)};
    const u32 idx[12] = {0U, 1U, 2U, 2U, 1U, 3U, 2U, 3U, 4U, 4U, 3U, 5U};
    return test_mesh(alloc, v, 6U, idx, 4U, false);
}

// ---------------------------------------------------------------------------
// Heightfields
// ---------------------------------------------------------------------------

// A heightfield of `columns` x `rows` samples `spacing` apart (`cells`: NULL
// or the flags), which must build and validate.
static inline NphPhysicsHeightfield *test_heightfield(NphAllocator *alloc, u32 columns, u32 rows,
                                                      f32 spacing, const f32 *heights,
                                                      const u8 *cells)
{
    NphPhysicsHeightfieldDesc d;
    memset(&d, 0, sizeof d);
    d.heights = heights;
    d.columns = columns;
    d.rows = rows;
    d.spacing_x = spacing;
    d.spacing_z = spacing;
    d.cells = cells;
    NphPhysicsHeightfield *h = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_heightfield_build(&d, alloc, &h), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(h);
    NTEST_ASSERT_TRUE(nph_physics_heightfield_validate(h, h->byte_size));
    return h;
}

// The same triangles as a mesh, in cell order, holes left out.
static inline NphPhysicsMesh *test_heightfield_as_mesh(NphAllocator *alloc,
                                                       const NphPhysicsHeightfield *h)
{
    const u32 n = nph_physics_heightfield_triangle_count(h);
    NphVec3 *v = NPH_ALLOC_ARRAY(alloc, NphVec3, 3ULL * n);
    u32 *idx = NPH_ALLOC_ARRAY(alloc, u32, 3ULL * n);
    NTEST_ASSERT_TRUE(v != NULL && idx != NULL);
    u32 kept = 0U;
    for (u32 t = 0U; t < n; ++t) {
        if (nph_physics_heightfield_triangle(h, t, &v[3ULL * kept], NULL)) {
            for (u32 k = 0U; k < 3U; ++k) {
                idx[3ULL * kept + k] = 3U * kept + k;
            }
            kept += 1U;
        }
    }
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = 3U * kept;
    d.indices = idx;
    d.triangle_count = kept;
    d.weld_tolerance = -1.0F; // the copies of each sample are exact
    NphPhysicsMesh *m = NULL;
    NTEST_EXPECT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_OK);
    NPH_FREE_ARRAY(alloc, NphVec3, v, 3ULL * n);
    NPH_FREE_ARRAY(alloc, u32, idx, 3ULL * n);
    NTEST_ASSERT_NOT_NULL(m);
    return m;
}

// ---------------------------------------------------------------------------
// Manifolds
// ---------------------------------------------------------------------------

// Every manifold's invariants: a unit normal, at most four points, each
// within the speculative distance.
static inline void test_check_manifold(const NphPhysicsManifold *m, f32 speculative)
{
    NTEST_ASSERT_TRUE(m->point_count <= NPH_PHYSICS_MANIFOLD_MAX_POINTS);
    if (m->point_count == 0U) {
        return;
    }
    NTEST_EXPECT_NEAR_F32(nph_vec3_length(m->normal), 1.0F, 1e-4F);
    for (u32 i = 0U; i < m->point_count; ++i) {
        NTEST_EXPECT_TRUE(m->points[i].separation <= speculative + 1e-5F);
        NTEST_EXPECT_TRUE(nph_vec3_is_finite(m->points[i].point));
        for (u32 j = i + 1U; j < m->point_count; ++j) {
            NTEST_EXPECT_NE_UINT(m->points[i].id, m->points[j].id);
        }
    }
}

static inline f32 test_min_separation(const NphPhysicsManifold *m)
{
    f32 s = 1e30F;
    for (u32 i = 0U; i < m->point_count; ++i) {
        s = nph_f32_min(s, m->points[i].separation);
    }
    return s;
}
