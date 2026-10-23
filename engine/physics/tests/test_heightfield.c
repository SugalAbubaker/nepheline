// SPDX-License-Identifier: Apache-2.0
// Heightfields (ADR-0046 §4): the grid's layout and triangles, refused
// descriptions, active edges (borders, holes' rims, convex folds), the
// blocks' height ranges, boxes and rays against brute force, rays through
// shared edges and corners, faces seen from below, heights, the validation
// of corrupted blocks, determinism, and collision equal to the same
// triangles built as a mesh.

#include <nepheline/core/log.h>
#include <nepheline/physics/heightfield.h>

#include "physics_internal.h"
#include "test_physics_support.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define GRID_MAX 33U // samples a side, at most

// Random heights on a `columns` x `rows` grid: quantized to `step` (0 for
// none), so neighbouring triangles' folds are either flat or steep.
static void random_heights(NphPcg32 *r, u32 columns, u32 rows, f32 range, f32 step, f32 *out)
{
    for (u32 s = 0U; s < columns * rows; ++s) {
        const f32 y = test_range(r, -range, range);
        out[s] = step > 0.0F ? step * nph_f32_round(y / step) : y;
    }
}

// Some cells holes, some flipped.
static void random_cells(NphPcg32 *r, u32 columns, u32 rows, u32 hole_percent, u8 *out)
{
    for (u32 c = 0U; c < (columns - 1U) * (rows - 1U); ++c) {
        u8 flags = 0U;
        if (nph_pcg32_next_bounded(r, 100U) < hole_percent) {
            flags |= NPH_PHYSICS_HEIGHTFIELD_HOLE;
        }
        if (nph_pcg32_next_bounded(r, 2U) != 0U) {
            flags |= NPH_PHYSICS_HEIGHTFIELD_FLIP;
        }
        out[c] = flags;
    }
    out[0] = 0U; // never all holes
}

static NphVec3 triangle_normal(const NphVec3 v[3])
{
    return nph_vec3_normalize(nph_vec3_cross(nph_vec3_sub(v[1], v[0]), nph_vec3_sub(v[2], v[0])));
}

NTEST(heightfield, lays_out_its_grid_and_triangles)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // 5 x 4 samples half a metre apart, height 0.1 i + 0.01 j.
    f32 heights[20];
    for (u32 j = 0U; j < 4U; ++j) {
        for (u32 i = 0U; i < 5U; ++i) {
            heights[j * 5U + i] = 0.1F * (f32)i + 0.01F * (f32)j;
        }
    }
    u8 cells[12] = {0U};
    cells[5] = NPH_PHYSICS_HEIGHTFIELD_HOLE;
    cells[6] = NPH_PHYSICS_HEIGHTFIELD_FLIP;
    NphPhysicsHeightfield *h = test_heightfield(alloc, 5U, 4U, 0.5F, heights, cells);
    NTEST_EXPECT_EQ_UINT(h->columns, 5U);
    NTEST_EXPECT_EQ_UINT(h->rows, 4U);
    NTEST_EXPECT_EQ_UINT(nph_physics_heightfield_triangle_count(h), 24U);
    NTEST_EXPECT_EQ_UINT(h->block_columns, 1U);
    NTEST_EXPECT_EQ_UINT(h->block_rows, 1U);
    NTEST_EXPECT_TRUE(nph_vec3_near(h->bounds.min, nph_vec3(0.0F, 0.0F, 0.0F), 0.0F));
    NTEST_EXPECT_TRUE(nph_vec3_near(h->bounds.max, nph_vec3(2.0F, 0.43F, 1.5F), 1e-6F));
    // Cell 0's first triangle: corners (0, 0), (0, 1), (1, 0), facing up.
    NphVec3 v[3];
    u32 active = 0U;
    NTEST_ASSERT_TRUE(nph_physics_heightfield_triangle(h, 0U, v, &active));
    NTEST_EXPECT_TRUE(nph_vec3_near(v[0], nph_vec3(0.0F, 0.0F, 0.0F), 1e-7F));
    NTEST_EXPECT_TRUE(nph_vec3_near(v[1], nph_vec3(0.0F, 0.01F, 0.5F), 1e-7F));
    NTEST_EXPECT_TRUE(nph_vec3_near(v[2], nph_vec3(0.5F, 0.1F, 0.0F), 1e-7F));
    // Every triangle faces up and lies over its cell; the hole has none.
    for (u32 t = 0U; t < 24U; ++t) {
        const u32 c = t / 2U;
        const bool hole = c == 5U;
        NTEST_ASSERT_EQ_INT(nph_physics_heightfield_triangle(h, t, v, NULL) ? 1 : 0, hole ? 0 : 1);
        if (hole) {
            continue;
        }
        NTEST_EXPECT_TRUE(triangle_normal(v).y > 0.0F);
        const u32 ci = c % 4U;
        const u32 cj = c / 4U;
        const f32 x0 = 0.5F * (f32)ci;
        const f32 z0 = 0.5F * (f32)cj;
        for (u32 k = 0U; k < 3U; ++k) {
            NTEST_EXPECT_TRUE(v[k].x >= x0 - 1e-6F && v[k].x <= x0 + 0.5F + 1e-6F);
            NTEST_EXPECT_TRUE(v[k].z >= z0 - 1e-6F && v[k].z <= z0 + 0.5F + 1e-6F);
        }
    }
    // The flipped cell's triangles share the other diagonal: corner (i, j)
    // with (i + 1, j + 1).
    NTEST_ASSERT_TRUE(nph_physics_heightfield_triangle(h, 12U, v, NULL));
    NTEST_EXPECT_TRUE(nph_vec3_near(v[2], nph_vec3(1.5F, 0.32F, 1.0F), 1e-6F));
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, bad_descriptions_are_refused)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    f32 heights[9] = {0.0F};
    u8 cells[4] = {0U};
    NphPhysicsHeightfieldDesc d;
    memset(&d, 0, sizeof d);
    d.heights = heights;
    d.columns = 3U;
    d.rows = 3U;
    d.spacing_x = 1.0F;
    d.spacing_z = 1.0F;
    d.cells = cells;
    NphPhysicsHeightfield *h = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_heightfield_build(&d, alloc, &h), NPH_RESULT_OK);
    nph_physics_heightfield_free(h, alloc);
    NphPhysicsHeightfieldDesc bad = d;
    bad.columns = 1U;
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&bad, alloc, &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    bad = d;
    bad.rows = NPH_PHYSICS_HEIGHTFIELD_MAX_SAMPLES + 1U;
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&bad, alloc, &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    bad = d;
    bad.spacing_x = 0.0F;
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&bad, alloc, &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    bad = d;
    bad.spacing_z = (f32)NAN;
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&bad, alloc, &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    bad = d;
    bad.spacing_x = NPH_PHYSICS_MAX_COORDINATE; // two cells: past the limit
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&bad, alloc, &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    bad = d;
    bad.active_edge_angle = -0.1F;
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&bad, alloc, &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    heights[4] = (f32)NAN;
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&d, alloc, &h), NPH_RESULT_INVALID_ARGUMENT);
    heights[4] = 2.0F * NPH_PHYSICS_MAX_COORDINATE;
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&d, alloc, &h), NPH_RESULT_INVALID_ARGUMENT);
    heights[4] = 0.0F;
    cells[2] = 0x10U; // not a flag
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&d, alloc, &h), NPH_RESULT_INVALID_ARGUMENT);
    memset(cells, NPH_PHYSICS_HEIGHTFIELD_HOLE, sizeof cells); // all holes
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(&d, alloc, &h), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_physics_heightfield_build(NULL, alloc, &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(h);
    test_alloc_end(&ta);
}

// The active edges of triangle t.
static u32 edges_of(const NphPhysicsHeightfield *h, u32 t)
{
    NphVec3 v[3];
    u32 active = 0U;
    NTEST_ASSERT_TRUE(nph_physics_heightfield_triangle(h, t, v, &active));
    return active;
}

NTEST(heightfield, active_edges_are_borders_rims_and_convex_folds)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    // A flat 4 x 4 grid of cells: only the border's edges are active, each
    // border triangle's edge on the border.
    f32 flat[25] = {0.0F};
    NphPhysicsHeightfield *h = test_heightfield(alloc, 5U, 5U, 1.0F, flat, NULL);
    u32 total = 0U;
    for (u32 t = 0U; t < 32U; ++t) {
        const u32 e = edges_of(h, t);
        total += (e & 1U) + ((e >> 1U) & 1U) + ((e >> 2U) & 1U);
    }
    NTEST_EXPECT_EQ_UINT(total, 16U); // four cells a side, one edge each
    // The middle cell's triangles have none.
    NTEST_EXPECT_EQ_UINT(edges_of(h, 2U * 5U), 0U);
    NTEST_EXPECT_EQ_UINT(edges_of(h, 2U * 5U + 1U), 0U);
    nph_physics_heightfield_free(h, alloc);
    // A hole: its four neighbours' edges on its rim are active.
    u8 cells[16] = {0U};
    cells[5] = NPH_PHYSICS_HEIGHTFIELD_HOLE; // cell (1, 1)
    h = test_heightfield(alloc, 5U, 5U, 1.0F, flat, cells);
    // Cell (1, 0) below it: its top side (B-D) is its second triangle's
    // edge 1; cell (0, 1) to its left: its right side (C-D), second
    // triangle's edge 2.
    NTEST_EXPECT_TRUE((edges_of(h, 2U * 1U + 1U) & NPH_PHYSICS_MESH_EDGE_1) != 0U);
    NTEST_EXPECT_TRUE((edges_of(h, 2U * 4U + 1U) & NPH_PHYSICS_MESH_EDGE_2) != 0U);
    nph_physics_heightfield_free(h, alloc);
    // A ridge along X at row 2 (convex) and, on a second field, a valley
    // (concave): the ridge's edges are active, the valley's are not.
    for (u32 shape = 0U; shape < 2U; ++shape) {
        f32 fold[25];
        for (u32 j = 0U; j < 5U; ++j) {
            for (u32 i = 0U; i < 5U; ++i) {
                const f32 rise = 1.0F - 0.5F * nph_f32_abs((f32)j - 2.0F);
                fold[j * 5U + i] = shape == 0U ? rise : -rise;
            }
        }
        h = test_heightfield(alloc, 5U, 5U, 1.0F, fold, NULL);
        // Cell (1, 1)'s second triangle has the cell's top side (row 2) as
        // its edge 1.
        const bool on = (edges_of(h, 2U * 5U + 1U) & NPH_PHYSICS_MESH_EDGE_1) != 0U;
        NTEST_EXPECT_EQ_INT(on ? 1 : 0, shape == 0U ? 1 : 0);
        nph_physics_heightfield_free(h, alloc);
    }
    // A ridge folded 3 degrees (each side 1.5): flat at the default 5,
    // active at 1.
    f32 gentle[25];
    for (u32 j = 0U; j < 5U; ++j) {
        for (u32 i = 0U; i < 5U; ++i) {
            gentle[j * 5U + i] = -tanf(1.5F * 3.14159265F / 180.0F) * nph_f32_abs((f32)j - 2.0F);
        }
    }
    h = test_heightfield(alloc, 5U, 5U, 1.0F, gentle, NULL);
    NTEST_EXPECT_EQ_UINT(edges_of(h, 2U * 5U + 1U) & NPH_PHYSICS_MESH_EDGE_1, 0U);
    nph_physics_heightfield_free(h, alloc);
    NphPhysicsHeightfieldDesc d;
    memset(&d, 0, sizeof d);
    d.heights = gentle;
    d.columns = 5U;
    d.rows = 5U;
    d.spacing_x = 1.0F;
    d.spacing_z = 1.0F;
    d.active_edge_angle = 1.0F * 3.14159265F / 180.0F;
    NTEST_ASSERT_EQ_INT(nph_physics_heightfield_build(&d, alloc, &h), NPH_RESULT_OK);
    NTEST_EXPECT_NE_UINT(edges_of(h, 2U * 5U + 1U) & NPH_PHYSICS_MESH_EDGE_1, 0U);
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, active_edges_match_the_mesh_builders)
{
    // The same triangles built as a mesh mark the same edges (quantized
    // heights keep every fold flat or past 14 degrees, far from the
    // threshold where 32 and 64 bits could differ).
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(3U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    for (u32 trial = 0U; trial < 4U; ++trial) {
        const u32 n = 6U + 5U * trial;
        random_heights(&r, n, n, 1.0F, 0.25F, heights);
        random_cells(&r, n, n, 10U, cells);
        NphPhysicsHeightfield *h = test_heightfield(alloc, n, n, 0.5F, heights, cells);
        NphPhysicsMesh *m = test_heightfield_as_mesh(alloc, h);
        // The mesh's triangles in leaf order: match each by its vertices.
        u32 matched = 0U;
        const u8 *mesh_edges = nph_physics_mesh_edges(m);
        for (u32 t = 0U; t < m->triangle_count; ++t) {
            NphVec3 mv[3];
            nph_physics_mesh_triangle(m, t, mv);
            // The cell under the triangle's centre.
            const NphVec3 c =
                nph_vec3_scale(nph_vec3_add(nph_vec3_add(mv[0], mv[1]), mv[2]), 1.0F / 3.0F);
            const u32 ci = (u32)(c.x / 0.5F);
            const u32 cj = (u32)(c.z / 0.5F);
            for (u32 k = 0U; k < 2U; ++k) {
                const u32 ht = 2U * (cj * (n - 1U) + ci) + k;
                NphVec3 hv[3];
                u32 active = 0U;
                if (!nph_physics_heightfield_triangle(h, ht, hv, &active)) {
                    continue;
                }
                // The same triangle, maybe starting at another corner.
                for (u32 rot = 0U; rot < 3U; ++rot) {
                    if (nph_vec3_near(mv[0], hv[rot], 1e-6F) &&
                        nph_vec3_near(mv[1], hv[(rot + 1U) % 3U], 1e-6F) &&
                        nph_vec3_near(mv[2], hv[(rot + 2U) % 3U], 1e-6F)) {
                        // Edge e of the mesh's is edge e + rot of ours.
                        u32 turned = 0U;
                        for (u32 e = 0U; e < 3U; ++e) {
                            turned |= ((active >> ((e + rot) % 3U)) & 1U) << e;
                        }
                        NTEST_EXPECT_EQ_UINT(mesh_edges[t], turned);
                        matched += 1U;
                    }
                }
            }
        }
        NTEST_EXPECT_EQ_UINT(matched, m->triangle_count);
        nph_physics_mesh_free(m, alloc);
        nph_physics_heightfield_free(h, alloc);
    }
    test_alloc_end(&ta);
}

NTEST(heightfield, blocks_hold_their_cells_heights)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(5U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    random_heights(&r, 21U, 18U, 3.0F, 0.0F, heights);
    random_cells(&r, 21U, 18U, 20U, cells);
    // Block (1, 1) all holes.
    for (u32 j = 8U; j < 16U; ++j) {
        for (u32 i = 8U; i < 16U; ++i) {
            cells[j * 20U + i] = NPH_PHYSICS_HEIGHTFIELD_HOLE;
        }
    }
    NphPhysicsHeightfield *h = test_heightfield(alloc, 21U, 18U, 0.25F, heights, cells);
    NTEST_EXPECT_EQ_UINT(h->block_columns, 3U);
    NTEST_EXPECT_EQ_UINT(h->block_rows, 3U);
    const f32 *blocks = nph_physics_heightfield_blocks(h);
    for (u32 bj = 0U; bj < 3U; ++bj) {
        for (u32 bi = 0U; bi < 3U; ++bi) {
            f32 lo = FLT_MAX;
            f32 hi = -FLT_MAX;
            for (u32 j = 8U * bj; j < nph_min_u32(8U * bj + 8U, 17U); ++j) {
                for (u32 i = 8U * bi; i < nph_min_u32(8U * bi + 8U, 20U); ++i) {
                    if ((cells[j * 20U + i] & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
                        continue;
                    }
                    for (u32 k = 0U; k < 4U; ++k) {
                        const f32 y = heights[(j + (k & 1U)) * 21U + i + (k >> 1U)];
                        lo = nph_f32_min(lo, y);
                        hi = nph_f32_max(hi, y);
                    }
                }
            }
            NTEST_EXPECT_NEAR_F32(blocks[2ULL * (bj * 3U + bi)], lo, 0.0F);
            NTEST_EXPECT_NEAR_F32(blocks[2ULL * (bj * 3U + bi) + 1U], hi, 0.0F);
        }
    }
    NTEST_EXPECT_NEAR_F32(blocks[8], FLT_MAX, 0.0F); // the all-hole block, (1, 1)
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

typedef struct Found {
    u32 count;
    u32 list[4096];
} Found;

static bool record(void *user, u32 t)
{
    Found *f = user;
    NTEST_ASSERT_TRUE(f->count < 4096U);
    f->list[f->count++] = t;
    return true;
}

NTEST(heightfield, overlap_finds_what_brute_force_finds)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(7U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    random_heights(&r, GRID_MAX, 29U, 2.0F, 0.0F, heights);
    random_cells(&r, GRID_MAX, 29U, 15U, cells);
    NphPhysicsHeightfield *h = test_heightfield(alloc, GRID_MAX, 29U, 0.5F, heights, cells);
    const u32 n = nph_physics_heightfield_triangle_count(h);
    static Found f;
    static bool expected[2U * GRID_MAX * GRID_MAX];
    for (u32 trial = 0U; trial < 300U * TEST_SCALE; ++trial) {
        const NphVec3 c = test_vec3(&r, -2.0F, 18.0F);
        const NphVec3 e = test_vec3(&r, 0.0F, 3.0F);
        const NphAABB box = {nph_vec3_sub(c, e), nph_vec3_add(c, e)};
        f.count = 0U;
        nph_physics_heightfield_overlap(h, box, record, &f);
        u32 want = 0U;
        for (u32 t = 0U; t < n; ++t) {
            NphVec3 v[3];
            expected[t] = false;
            if (nph_physics_heightfield_triangle(h, t, v, NULL)) {
                const NphAABB tb = {nph_vec3_min(nph_vec3_min(v[0], v[1]), v[2]),
                                    nph_vec3_max(nph_vec3_max(v[0], v[1]), v[2])};
                expected[t] = nph_aabb_overlaps(tb, box);
                want += expected[t] ? 1U : 0U;
            }
        }
        NTEST_ASSERT_EQ_UINT(f.count, want);
        for (u32 k = 0U; k < f.count; ++k) {
            NTEST_ASSERT_TRUE(expected[f.list[k]]);
            expected[f.list[k]] = false; // each once
        }
    }
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

// An independent reference: Moller and Trumbore's test in 64 bits, from the
// front only.
static f64 reference_hit(NphVec3 o, NphVec3 d, const NphVec3 p[3])
{
    const f64 e1[3] = {(f64)p[1].x - (f64)p[0].x, (f64)p[1].y - (f64)p[0].y,
                       (f64)p[1].z - (f64)p[0].z};
    const f64 e2[3] = {(f64)p[2].x - (f64)p[0].x, (f64)p[2].y - (f64)p[0].y,
                       (f64)p[2].z - (f64)p[0].z};
    const f64 dv[3] = {(f64)d.x, (f64)d.y, (f64)d.z};
    const f64 pv[3] = {dv[1] * e2[2] - dv[2] * e2[1], dv[2] * e2[0] - dv[0] * e2[2],
                       dv[0] * e2[1] - dv[1] * e2[0]};
    const f64 det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (!(det > 0.0)) {
        return -1.0; // edge-on or from behind
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

NTEST(heightfield, raycasts_find_the_closest_hit)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(11U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    random_heights(&r, GRID_MAX, GRID_MAX, 1.5F, 0.0F, heights);
    random_cells(&r, GRID_MAX, GRID_MAX, 10U, cells);
    NphPhysicsHeightfield *h = test_heightfield(alloc, GRID_MAX, GRID_MAX, 0.5F, heights, cells);
    const u32 n = nph_physics_heightfield_triangle_count(h);
    u32 hits = 0U;
    for (u32 trial = 0U; trial < 400U * TEST_SCALE; ++trial) {
        // Long and short rays from above and the sides, some downward.
        const NphVec3 o = test_vec3(&r, -3.0F, 19.0F);
        NphVec3 d = nph_vec3_scale(test_unit(&r), test_range(&r, 0.5F, 30.0F));
        if ((trial & 1U) != 0U) {
            d.y = -nph_f32_abs(d.y) - 1.0F;
        }
        if ((trial % 7U) == 0U) {
            d = nph_vec3(0.0F, -10.0F, 0.0F); // straight down
        }
        f64 best = 2.0;
        for (u32 t = 0U; t < n; ++t) {
            NphVec3 v[3];
            if (nph_physics_heightfield_triangle(h, t, v, NULL)) {
                const f64 f = reference_hit(o, d, v);
                if (f >= 0.0 && f < best) {
                    best = f;
                }
            }
        }
        NphPhysicsMeshHit hit;
        const bool got = nph_physics_heightfield_raycast(h, o, d, &hit);
        if (best <= 1.0) {
            NTEST_ASSERT_TRUE(got);
            NTEST_EXPECT_NEAR_F64((f64)hit.fraction, best, 1e-4);
            NTEST_EXPECT_TRUE(nph_vec3_dot(hit.normal, d) < 0.0F);
            NTEST_EXPECT_EQ_INT(hit.back, 0);
            hits += 1U;
        } else if (got) {
            // Grazing edges the reference rejects (det within rounding).
            NTEST_EXPECT_TRUE(hit.fraction <= 1.0F);
        }
    }
    NTEST_LOG("%u hits", hits);
    NTEST_EXPECT_TRUE(hits > 30U * TEST_SCALE);
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, rays_through_shared_edges_and_corners_never_slip)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(13U);
    static f32 heights[GRID_MAX * GRID_MAX];
    random_heights(&r, 17U, 17U, 1.0F, 0.0F, heights);
    NphPhysicsHeightfield *h = test_heightfield(alloc, 17U, 17U, 0.5F, heights, NULL);
    u32 misses = 0U;
    // Straight down onto every corner and every edge's midpoint, and
    // slanted through every corner inside the grid (blocks' corners and
    // sides among them).
    for (u32 j = 0U; j < 17U; ++j) {
        for (u32 i = 0U; i < 17U; ++i) {
            for (u32 k = 0U; k < 4U; ++k) {
                const f32 x = 0.5F * (f32)i + (k == 1U ? 0.25F : 0.0F);
                const f32 z = 0.5F * (f32)j + (k == 2U ? 0.25F : 0.0F);
                if (x > 8.0F || z > 8.0F) {
                    continue;
                }
                // Slanted only through corners triangles share all round: at
                // the border a ray through the last corner leaves the surface
                // there, and rounding says whether it touched.
                if (k == 3U && (i == 0U || j == 0U || i == 16U || j == 16U)) {
                    continue;
                }
                NphVec3 start = nph_vec3(x, 5.0F, z);
                NphVec3 d = nph_vec3(0.0F, -10.0F, 0.0F);
                if (k == 3U) {
                    // Through the corner point at half the segment.
                    d = nph_vec3(0.3F, -2.0F, -0.2F);
                    const NphVec3 corner = nph_vec3(x, heights[j * 17U + i], z);
                    start = nph_vec3_sub(corner, nph_vec3_scale(d, 0.5F));
                }
                NphPhysicsMeshHit hit;
                if (!nph_physics_heightfield_raycast(h, start, d, &hit)) {
                    misses += 1U;
                    NTEST_LOG("missed corner (%u, %u), kind %u, from (%.7f %.7f %.7f)", i, j, k,
                              (f64)start.x, (f64)start.y, (f64)start.z);
                }
            }
        }
    }
    NTEST_EXPECT_EQ_UINT(misses, 0U);
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, faces_are_hit_from_above_only)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    f32 heights[16] = {0.0F};
    NphPhysicsHeightfield *h = test_heightfield(alloc, 4U, 4U, 1.0F, heights, NULL);
    NphPhysicsMeshHit hit;
    NTEST_EXPECT_TRUE(nph_physics_heightfield_raycast(h, nph_vec3(1.3F, 1.0F, 1.7F),
                                                      nph_vec3(0.0F, -2.0F, 0.0F), &hit));
    NTEST_EXPECT_NEAR_F32(hit.fraction, 0.5F, 1e-6F);
    NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, nph_vec3_unit_y(), 1e-6F));
    NTEST_EXPECT_FALSE(nph_physics_heightfield_raycast(h, nph_vec3(1.3F, -1.0F, 1.7F),
                                                       nph_vec3(0.0F, 2.0F, 0.0F), &hit));
    // Parallel to the surface, above it: nothing; off the grid: nothing.
    NTEST_EXPECT_FALSE(nph_physics_heightfield_raycast(h, nph_vec3(-1.0F, 0.5F, 1.0F),
                                                       nph_vec3(5.0F, 0.0F, 0.0F), &hit));
    NTEST_EXPECT_FALSE(nph_physics_heightfield_raycast(h, nph_vec3(5.0F, 1.0F, 1.0F),
                                                       nph_vec3(0.0F, -2.0F, 0.0F), &hit));
    NTEST_EXPECT_FALSE(
        nph_physics_heightfield_raycast(h, nph_vec3(1.0F, 1.0F, 1.0F), nph_vec3_zero(), &hit));
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, heights_are_the_surfaces)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(17U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    random_heights(&r, 12U, 9U, 2.0F, 0.0F, heights);
    random_cells(&r, 12U, 9U, 20U, cells);
    NphPhysicsHeightfield *h = test_heightfield(alloc, 12U, 9U, 0.75F, heights, cells);
    for (u32 trial = 0U; trial < 500U; ++trial) {
        const f32 x = test_range(&r, -0.5F, 8.75F);
        const f32 z = test_range(&r, -0.5F, 6.5F);
        f32 y = 0.0F;
        NphVec3 n;
        const bool on = nph_physics_heightfield_height_at(h, x, z, &y, &n);
        NphPhysicsMeshHit hit;
        const bool hit_down = nph_physics_heightfield_raycast(h, nph_vec3(x, 10.0F, z),
                                                              nph_vec3(0.0F, -20.0F, 0.0F), &hit);
        NTEST_ASSERT_EQ_INT(on ? 1 : 0, hit_down ? 1 : 0);
        if (on) {
            NTEST_EXPECT_NEAR_F32(y, 10.0F - 20.0F * hit.fraction, 1e-4F);
            NTEST_EXPECT_TRUE(nph_vec3_near(n, hit.normal, 1e-4F));
        }
    }
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, validation_rejects_corrupted_blocks)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(19U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    random_heights(&r, 19U, 11U, 1.0F, 0.0F, heights);
    random_cells(&r, 19U, 11U, 10U, cells);
    NphPhysicsHeightfield *h = test_heightfield(alloc, 19U, 11U, 0.5F, heights, cells);
    const u32 size = h->byte_size;
    NphPhysicsHeightfield *c = NPH_ALLOC(alloc, size + 16U, 16U);
    NTEST_ASSERT_NOT_NULL(c);
    const NphLogLevel level = nph_log_get_level(&NPH_LOG_PHYSICS);
    nph_log_set_level(&NPH_LOG_PHYSICS, NPH_LOG_LEVEL_ERROR); // each rejection warns
#define RESET() memcpy(c, h, size)
    RESET();
    NTEST_EXPECT_TRUE(nph_physics_heightfield_validate(c, size));
    NTEST_EXPECT_FALSE(nph_physics_heightfield_validate(c, size - 1U)); // shorter than byte_size
    NTEST_EXPECT_FALSE(nph_physics_heightfield_validate(c, 40U));
    NTEST_EXPECT_FALSE(nph_physics_heightfield_validate(NULL, size));
    memcpy((u8 *)c + 4U, h, size);
    NTEST_EXPECT_FALSE(nph_physics_heightfield_validate((u8 *)c + 4U, size)); // misaligned
    for (u32 kind = 0U; kind < 9U; ++kind) {
        RESET();
        void *ys_bytes = (u8 *)c + c->heights_offset;
        void *blocks_bytes = (u8 *)c + c->blocks_offset;
        f32 *ys = ys_bytes;
        f32 *blocks = blocks_bytes;
        switch (kind) {
        case 0U:
            c->columns = 1U;
            break;
        case 1U:
            c->spacing_x = 0.0F;
            break;
        case 2U:
            c->blocks_offset = size; // past the end
            break;
        case 3U:
            c->block_columns += 1U;
            break;
        case 4U:
            ys[7] = (f32)NAN;
            break;
        case 5U:
            ys[7] = c->bounds.max.y + 1.0F; // above the bounds
            break;
        case 6U:
            blocks[1] = blocks[0]; // a block lower than its cells
            break;
        case 7U:
            c->bounds.max.x += 1.0F; // not the grid's
            break;
        default:
            c->flags = 1U;
            break;
        }
        NTEST_EXPECT_FALSE(nph_physics_heightfield_validate(c, size));
    }
    // Random damage never crashes validation, and what passes is safe to
    // query.
    u32 passed = 0U;
    for (u32 trial = 0U; trial < 2000U; ++trial) {
        RESET();
        const u32 flips = 1U + nph_pcg32_next_bounded(&r, 4U);
        for (u32 k = 0U; k < flips; ++k) {
            ((u8 *)c)[nph_pcg32_next_bounded(&r, size)] ^=
                (u8)(1U << nph_pcg32_next_bounded(&r, 8U));
        }
        if (nph_physics_heightfield_validate(c, size)) {
            passed += 1U;
            static Found f;
            f.count = 0U;
            nph_physics_heightfield_overlap(c, c->bounds, record, &f);
            NphPhysicsMeshHit hit;
            NPH_UNUSED(nph_physics_heightfield_raycast(c, nph_vec3(0.1F, 5.0F, 0.2F),
                                                       nph_vec3(8.0F, -10.0F, 4.0F), &hit));
        }
    }
#undef RESET
    nph_log_set_level(&NPH_LOG_PHYSICS, level);
    NTEST_LOG("%u of 2000 damaged blocks still valid (damage to cells and slack)", passed);
    NPH_FREE(alloc, c, size + 16U, 16U);
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, builds_are_deterministic)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(23U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    random_heights(&r, 25U, 14U, 2.0F, 0.0F, heights);
    random_cells(&r, 25U, 14U, 10U, cells);
    NphPhysicsHeightfield *a = test_heightfield(alloc, 25U, 14U, 0.3F, heights, cells);
    NphPhysicsHeightfield *b = test_heightfield(alloc, 25U, 14U, 0.3F, heights, cells);
    NTEST_ASSERT_EQ_UINT(a->byte_size, b->byte_size);
    NTEST_EXPECT_TRUE(memcmp(a, b, a->byte_size) == 0);
    nph_physics_heightfield_free(a, alloc);
    nph_physics_heightfield_free(b, alloc);
    test_alloc_end(&ta);
}

// ---------------------------------------------------------------------------
// Collision: as the same triangles built as a mesh
// ---------------------------------------------------------------------------

NTEST(heightfield, collides_as_the_same_mesh)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphPcg32 r = test_rng(29U);
    static f32 heights[GRID_MAX * GRID_MAX];
    static u8 cells[GRID_MAX * GRID_MAX];
    // Quantized heights: folds flat or steep, so both builders mark the
    // same edges, and normals' clusters cannot depend on visiting order.
    random_heights(&r, 21U, 21U, 0.75F, 0.25F, heights);
    random_cells(&r, 21U, 21U, 8U, cells);
    NphPhysicsHeightfield *h = test_heightfield(alloc, 21U, 21U, 0.5F, heights, cells);
    NphPhysicsMesh *m = test_heightfield_as_mesh(alloc, h);
    NphPhysicsHull *hull = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(nph_vec3(0.3F, 0.2F, 0.4F), alloc, &hull),
                        NPH_RESULT_OK);
    const NphPhysicsGeometry shapes[4] = {
        nph_physics_geometry_sphere(0.4F), nph_physics_geometry_capsule(0.4F, 0.25F),
        nph_physics_geometry_box(nph_vec3(0.45F, 0.3F, 0.35F)), nph_physics_geometry_hull(hull)};
    const NphPhysicsPose home = nph_physics_pose_identity();
    u32 compared = 0U;
    for (u32 trial = 0U; trial < 250U * TEST_SCALE; ++trial) {
        const NphPhysicsGeometry *g = &shapes[trial % 4U];
        const f32 x = test_range(&r, 0.5F, 9.5F);
        const f32 z = test_range(&r, 0.5F, 9.5F);
        f32 ground = 0.0F;
        if (!nph_physics_heightfield_height_at(h, x, z, &ground, NULL)) {
            continue;
        }
        const NphPhysicsPose pose = nph_physics_pose(
            nph_vec3(x, ground + test_range(&r, 0.0F, 0.45F), z), test_rotation(&r));
        NphPhysicsMeshManifolds a;
        NphPhysicsMeshManifolds b;
        nph_physics_collide_heightfield(g, pose, h, home, NPH_PHYSICS_SPECULATIVE_DISTANCE, &a);
        nph_physics_collide_mesh(g, pose, m, home, NPH_PHYSICS_SPECULATIVE_DISTANCE, &b);
        // The same contacts, grouped as the triangles' order groups them: a
        // manifold takes the normals within 5 degrees of its first member's,
        // so contacts whose normals chain within 5 degrees (a capsule's edge
        // contacts on a ridge) make one manifold or two, by which triangle
        // comes first, and among more than four the deepest four are kept.
        // Both find the same deepest point and the same directions.
        NTEST_ASSERT_EQ_INT(a.count > 0U ? 1 : 0, b.count > 0U ? 1 : 0);
        if (a.count == 0U) {
            continue;
        }
        f32 deepest_a = FLT_MAX;
        f32 deepest_b = FLT_MAX;
        for (u32 k = 0U; k < a.count; ++k) {
            test_check_manifold(&a.manifolds[k], NPH_PHYSICS_SPECULATIVE_DISTANCE);
            deepest_a = nph_f32_min(deepest_a, test_min_separation(&a.manifolds[k]));
        }
        for (u32 k = 0U; k < b.count; ++k) {
            deepest_b = nph_f32_min(deepest_b, test_min_separation(&b.manifolds[k]));
        }
        NTEST_EXPECT_NEAR_F32(deepest_a, deepest_b, 1e-4F);
        if (a.count < NPH_PHYSICS_MESH_MAX_MANIFOLDS && b.count < NPH_PHYSICS_MESH_MAX_MANIFOLDS) {
            for (u32 side = 0U; side < 2U; ++side) {
                const NphPhysicsMeshManifolds *one = side == 0U ? &a : &b;
                const NphPhysicsMeshManifolds *other = side == 0U ? &b : &a;
                for (u32 k = 0U; k < one->count; ++k) {
                    f32 best = -1.0F;
                    for (u32 q = 0U; q < other->count; ++q) {
                        best = nph_f32_max(best, nph_vec3_dot(one->manifolds[k].normal,
                                                              other->manifolds[q].normal));
                    }
                    NTEST_EXPECT_TRUE(best > 0.98480775F); // within 10 degrees
                    compared += 1U;
                }
            }
        }
    }
    NTEST_LOG("%u manifolds compared", compared);
    NTEST_EXPECT_TRUE(compared > 100U * TEST_SCALE);
    nph_physics_hull_free(hull, alloc);
    nph_physics_mesh_free(m, alloc);
    nph_physics_heightfield_free(h, alloc);
    test_alloc_end(&ta);
}

NTEST(heightfield, keeps_full_precision_far_from_its_origin)
{
    // A field 10 km long whose heights repeat every 8 cells: a box placed in
    // 64 bits over its far end gets, to the bit, what it gets over the same
    // cells at its start (the world's path: collide_mesh.c's core).
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    const u32 cols = 4001U;
    const u32 rows = 9U;
    const f32 spacing = 2.5F;
    f32 *heights = NPH_ALLOC_ARRAY(alloc, f32, (u64)cols * rows);
    NTEST_ASSERT_NOT_NULL(heights);
    NphPcg32 r = test_rng(31U);
    f32 pattern[8 * 9];
    for (u32 k = 0U; k < 8U * 9U; ++k) {
        pattern[k] = test_range(&r, -0.4F, 0.4F);
    }
    for (u32 j = 0U; j < rows; ++j) {
        for (u32 i = 0U; i < cols; ++i) {
            heights[(u64)j * cols + i] = pattern[j * 8U + i % 8U];
        }
    }
    NphPhysicsHeightfield *h = test_heightfield(alloc, cols, rows, spacing, heights, NULL);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(1.4F, 0.6F, 1.1F));
    const f64 far = 3992.0 * (f64)spacing; // a multiple of the pattern: 9980 m
    for (u32 trial = 0U; trial < 50U; ++trial) {
        const f32 x = test_range(&r, 3.0F, 15.0F);
        const f32 z = test_range(&r, 3.0F, 15.0F);
        f32 ground = 0.0F;
        NTEST_ASSERT_TRUE(nph_physics_heightfield_height_at(h, x, z, &ground, NULL));
        const NphQuat q = test_rotation(&r);
        const NphDVec3 near = nph_dvec3((f64)x, (f64)ground + 0.45, (f64)z);
        NphPhysicsMeshManifolds home;
        NphPhysicsMeshManifolds away;
        nph_physics_collide_heightfield_local(&box, q, near, h, 0.02F, &home);
        nph_physics_collide_heightfield_local(&box, q, nph_dvec3(near.x + far, near.y, near.z), h,
                                              0.02F, &away);
        NTEST_ASSERT_EQ_UINT(home.count, away.count);
        for (u32 k = 0U; k < home.count; ++k) {
            const NphPhysicsManifold *a = &home.manifolds[k];
            const NphPhysicsManifold *b = &away.manifolds[k];
            NTEST_ASSERT_EQ_UINT(a->point_count, b->point_count);
            NTEST_EXPECT_TRUE(nph_vec3_near(a->normal, b->normal, 0.0F)); // to the bit
            for (u32 p = 0U; p < a->point_count; ++p) {
                NTEST_EXPECT_TRUE(nph_vec3_near(a->points[p].point, b->points[p].point, 0.0F));
                NTEST_EXPECT_NEAR_F32(a->points[p].separation, b->points[p].separation, 0.0F);
            }
        }
    }
    nph_physics_heightfield_free(h, alloc);
    NPH_FREE_ARRAY(alloc, f32, heights, (u64)cols * rows);
    test_alloc_end(&ta);
}
