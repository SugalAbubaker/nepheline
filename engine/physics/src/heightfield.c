// SPDX-License-Identifier: Apache-2.0
// Heightfields (heightfield.h; docs/physics.md "Heightfields"; ADR-0046 §4):
// the builder (the triangles' active edges and the blocks' height ranges),
// the validation of blocks read from files, and the box, ray, and height
// queries.

#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/physics/heightfield.h>

#include "physics_internal.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define HEADER_BYTES ((u32)sizeof(NphPhysicsHeightfield))
#define BLOCK        NPH_PHYSICS_HEIGHTFIELD_BLOCK
#define CELL_FLAGS   ((u32)NPH_PHYSICS_HEIGHTFIELD_HOLE | (u32)NPH_PHYSICS_HEIGHTFIELD_FLIP)
// A cell's byte: its flags, then the first triangle's active edges, then
// the second's.
#define EDGES_SHIFT 2U

// The corners of cell (i, j), corner k at sample (i + (k >> 1), j + (k & 1)):
// A (i, j), B (i, j + 1), C (i + 1, j), D (i + 1, j + 1). Each triangle's
// corners counter-clockwise seen from above (+Y), by diagonal: split B-C,
// (A, B, C) and (C, B, D); flipped, A-D, (A, B, D) and (A, D, C).
static const u8 k_corners[2][2][3] = {
    {{0U, 1U, 2U}, {2U, 1U, 3U}},
    {{0U, 1U, 3U}, {0U, 3U, 2U}},
};

static u32 cell_index(const NphPhysicsHeightfield *h, u32 i, u32 j)
{
    return j * (h->columns - 1U) + i;
}

// ---------------------------------------------------------------------------
// Triangles
// ---------------------------------------------------------------------------

// Corner k of cell (i, j), less `origin`, in 64 bits then rounded: small and
// exact however far the cell is from the heightfield's origin.
static NphVec3 corner_at(const NphPhysicsHeightfield *h, u32 i, u32 j, u32 k, NphDVec3 origin)
{
    const u32 ci = i + (k >> 1U);
    const u32 cj = j + (k & 1U);
    const f32 y = nph_physics_heightfield_heights(h)[(u64)cj * h->columns + ci];
    return nph_vec3((f32)((f64)ci * (f64)h->spacing_x - origin.x), (f32)((f64)y - origin.y),
                    (f32)((f64)cj * (f64)h->spacing_z - origin.z));
}

static void triangle_at(const NphPhysicsHeightfield *h, u32 i, u32 j, u32 cell, u32 k,
                        NphDVec3 origin, NphVec3 out[3])
{
    const u32 flip = (cell & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 1U : 0U;
    for (u32 v = 0U; v < 3U; ++v) {
        out[v] = corner_at(h, i, j, k_corners[flip][k][v], origin);
    }
}

static u32 triangle_edges(u32 cell, u32 k)
{
    return (cell >> (EDGES_SHIFT + 3U * k)) & NPH_PHYSICS_MESH_EDGES;
}

bool nph_physics_heightfield_triangle(const NphPhysicsHeightfield *h, u32 t, NphVec3 out[3],
                                      u32 *active_edges)
{
    NPH_ASSERT(h != NULL && out != NULL && t < nph_physics_heightfield_triangle_count(h));
    const u32 c = t >> 1U;
    const u32 i = c % (h->columns - 1U);
    const u32 j = c / (h->columns - 1U);
    const u32 cell = nph_physics_heightfield_cells(h)[c];
    if ((cell & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
        return false;
    }
    triangle_at(h, i, j, cell, t & 1U, nph_dvec3_zero(), out);
    if (active_edges != NULL) {
        *active_edges = triangle_edges(cell, t & 1U);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The builder
// ---------------------------------------------------------------------------

// The description's grid, for the builder's arithmetic in 64 bits.
typedef struct Grid {
    const f32 *heights;
    const u8 *cells; // may be NULL
    u32 columns;
    u32 rows;
    f64 sx;
    f64 sz;
} Grid;

static u32 grid_cell(const Grid *g, u32 i, u32 j)
{
    return g->cells != NULL ? g->cells[(u64)j * (g->columns - 1U) + i] : 0U;
}

static NphDVec3 grid_corner(const Grid *g, u32 i, u32 j, u32 k)
{
    const u32 ci = i + (k >> 1U);
    const u32 cj = j + (k & 1U);
    return nph_dvec3((f64)ci * g->sx, (f64)g->heights[(u64)cj * g->columns + ci], (f64)cj * g->sz);
}

static NphDVec3 grid_normal(const Grid *g, u32 i, u32 j, u32 k)
{
    const u32 flip = (grid_cell(g, i, j) & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 1U : 0U;
    const NphDVec3 a = grid_corner(g, i, j, k_corners[flip][k][0]);
    const NphDVec3 b = grid_corner(g, i, j, k_corners[flip][k][1]);
    const NphDVec3 c = grid_corner(g, i, j, k_corners[flip][k][2]);
    const NphDVec3 e1 = nph_dvec3_sub(b, a);
    const NphDVec3 e2 = nph_dvec3_sub(c, a);
    const NphDVec3 n =
        nph_dvec3(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
    const f64 len = sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return nph_dvec3(n.x / len, n.y / len, n.z / len); // never zero: spacings are positive
}

// The triangle across the edge from corner p to corner q of triangle k of
// cell (i, j), and its corner off that edge: false at the grid's border or
// a hole. The diagonal's neighbour is the cell's other triangle; a side's,
// the triangle of the next cell holding that side (the left side A-B is
// always a cell's first triangle, the right side C-D its second; the bottom
// A-C and top B-D change with the diagonal).
static bool grid_neighbour(const Grid *g, u32 i, u32 j, u32 k, u32 p, u32 q, u32 *ni, u32 *nj,
                           u32 *nk, u32 *far)
{
    const u32 flip = (grid_cell(g, i, j) & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 1U : 0U;
    const u32 pair = (1U << p) | (1U << q);
    const u32 diagonal = flip != 0U ? 0x9U : 0x6U; // {A, D} or {B, C}
    u32 ci = i;
    u32 cj = j;
    u32 side_far_mask = 0U; // the neighbour's corners on the shared side
    if (pair == diagonal) {
        *ni = i;
        *nj = j;
        *nk = k ^ 1U;
        side_far_mask = pair;
    } else if (pair == 0x3U) { // left, A-B: the cell to the left, its right side C-D
        if (i == 0U) {
            return false;
        }
        ci = i - 1U;
        *nk = 1U;
        side_far_mask = 0xCU;
    } else if (pair == 0xCU) { // right, C-D: the cell to the right, its left side A-B
        if (i + 2U >= g->columns) {
            return false;
        }
        ci = i + 1U;
        *nk = 0U;
        side_far_mask = 0x3U;
    } else if (pair == 0x5U) { // bottom, A-C: the cell below, its top side B-D
        if (j == 0U) {
            return false;
        }
        cj = j - 1U;
        *nk = (grid_cell(g, ci, cj) & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 0U : 1U;
        side_far_mask = 0xAU;
    } else { // top, B-D: the cell above, its bottom side A-C
        if (j + 2U >= g->rows) {
            return false;
        }
        cj = j + 1U;
        *nk = (grid_cell(g, ci, cj) & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 1U : 0U;
        side_far_mask = 0x5U;
    }
    if (pair != diagonal) {
        *ni = ci;
        *nj = cj;
    }
    const u32 ncell = grid_cell(g, *ni, *nj);
    if ((ncell & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
        return false;
    }
    const u32 nflip = (ncell & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 1U : 0U;
    u32 c = 0U;
    for (u32 v = 0U; v < 3U; ++v) {
        c = k_corners[nflip][*nk][v];
        if ((side_far_mask & (1U << c)) == 0U) {
            break; // the corner off the shared side
        }
    }
    *far = c;
    return true;
}

// The active edges of triangle k of cell (i, j), by the mesh builder's rule
// (Jolt's ActiveEdges::IsEdgeActive): an edge with no triangle across it, or
// one convex by more than the threshold (the far corner of the triangle
// across it below this one's plane).
static u32 grid_active_edges(const Grid *g, u32 i, u32 j, u32 k, f64 cos_limit)
{
    const u32 flip = (grid_cell(g, i, j) & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 1U : 0U;
    const NphDVec3 n1 = grid_normal(g, i, j, k);
    u32 active = 0U;
    for (u32 e = 0U; e < 3U; ++e) {
        const u32 p = k_corners[flip][k][e];
        const u32 q = k_corners[flip][k][(e + 1U) % 3U];
        u32 ni = 0U;
        u32 nj = 0U;
        u32 nk = 0U;
        u32 far = 0U;
        bool on = true;
        if (grid_neighbour(g, i, j, k, p, q, &ni, &nj, &nk, &far)) {
            const NphDVec3 n2 = grid_normal(g, ni, nj, nk);
            const f64 cos_angle = n1.x * n2.x + n1.y * n2.y + n1.z * n2.z;
            const NphDVec3 d = nph_dvec3_sub(grid_corner(g, ni, nj, far), grid_corner(g, i, j, p));
            on = cos_angle < cos_limit && n1.x * d.x + n1.y * d.y + n1.z * d.z < 0.0;
        }
        if (on) {
            active |= 1U << e;
        }
    }
    return active;
}

static bool desc_valid(const NphPhysicsHeightfieldDesc *d)
{
    if (d == NULL || d->heights == NULL || d->columns < 2U || d->rows < 2U ||
        d->columns > NPH_PHYSICS_HEIGHTFIELD_MAX_SAMPLES ||
        d->rows > NPH_PHYSICS_HEIGHTFIELD_MAX_SAMPLES || !(d->spacing_x > 0.0F) ||
        !(d->spacing_z > 0.0F) ||
        !((f64)(d->columns - 1U) * (f64)d->spacing_x <= (f64)NPH_PHYSICS_MAX_COORDINATE) ||
        !((f64)(d->rows - 1U) * (f64)d->spacing_z <= (f64)NPH_PHYSICS_MAX_COORDINATE) ||
        !(d->active_edge_angle >= 0.0F && d->active_edge_angle <= 3.14159265F)) {
        return false;
    }
    const u64 samples = (u64)d->columns * d->rows;
    for (u64 s = 0U; s < samples; ++s) {
        if (!(nph_f32_abs(d->heights[s]) <= NPH_PHYSICS_MAX_COORDINATE)) {
            return false; // also NaN
        }
    }
    if (d->cells != NULL) {
        const u64 cells = (u64)(d->columns - 1U) * (d->rows - 1U);
        bool any = false;
        for (u64 c = 0U; c < cells; ++c) {
            if ((d->cells[c] & ~CELL_FLAGS) != 0U) {
                return false;
            }
            any = any || (d->cells[c] & NPH_PHYSICS_HEIGHTFIELD_HOLE) == 0U;
        }
        if (!any) {
            return false;
        }
    }
    return true;
}

static u64 align16(u64 v)
{
    return (v + 15U) & ~(u64)15U;
}

NphResult nph_physics_heightfield_build(const NphPhysicsHeightfieldDesc *desc, NphAllocator *alloc,
                                        NphPhysicsHeightfield **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    *out = NULL;
    if (!desc_valid(desc)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 cols = desc->columns;
    const u32 rows = desc->rows;
    const u64 samples = (u64)cols * rows;
    const u32 cells_x = cols - 1U;
    const u32 cells_z = rows - 1U;
    const u64 cells = (u64)cells_x * cells_z;
    const u32 blocks_x = (cells_x + BLOCK - 1U) / BLOCK;
    const u32 blocks_z = (cells_z + BLOCK - 1U) / BLOCK;
    const u64 heights_offset = HEADER_BYTES;
    const u64 cells_offset = align16(heights_offset + samples * sizeof(f32));
    const u64 blocks_offset = align16(cells_offset + cells);
    const u64 bytes = align16(blocks_offset + 2ULL * blocks_x * blocks_z * sizeof(f32));
    if (bytes > NPH_U32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphPhysicsHeightfield *h = NPH_ALLOC_ZEROED(alloc, (size_t)bytes, 16U);
    if (h == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    h->byte_size = (u32)bytes;
    h->columns = cols;
    h->rows = rows;
    h->spacing_x = desc->spacing_x;
    h->spacing_z = desc->spacing_z;
    h->heights_offset = (u32)heights_offset;
    h->cells_offset = (u32)cells_offset;
    h->blocks_offset = (u32)blocks_offset;
    h->block_columns = blocks_x;
    h->block_rows = blocks_z;
    void *heights_bytes = (u8 *)h + heights_offset;
    f32 *heights = heights_bytes;
    memcpy(heights, desc->heights, (size_t)(samples * sizeof(f32)));
    f32 lo = FLT_MAX;
    f32 hi = -FLT_MAX;
    for (u64 s = 0U; s < samples; ++s) {
        lo = nph_f32_min(lo, heights[s]);
        hi = nph_f32_max(hi, heights[s]);
    }
    h->bounds =
        (NphAABB){nph_vec3(0.0F, lo, 0.0F), nph_vec3((f32)((f64)cells_x * (f64)desc->spacing_x), hi,
                                                     (f32)((f64)cells_z * (f64)desc->spacing_z))};
    // Each cell's flags and its triangles' active edges.
    const Grid g = {desc->heights, desc->cells,          cols,
                    rows,          (f64)desc->spacing_x, (f64)desc->spacing_z};
    const f64 cos_limit =
        cos(desc->active_edge_angle > 0.0F ? (f64)desc->active_edge_angle
                                           : (f64)NPH_PHYSICS_MESH_DEFAULT_ACTIVE_ANGLE);
    u8 *cell_bytes = (u8 *)h + cells_offset;
    for (u32 j = 0U; j < cells_z; ++j) {
        for (u32 i = 0U; i < cells_x; ++i) {
            const u32 flags = grid_cell(&g, i, j);
            u32 byte = flags;
            if ((flags & NPH_PHYSICS_HEIGHTFIELD_HOLE) == 0U) {
                byte |= grid_active_edges(&g, i, j, 0U, cos_limit) << EDGES_SHIFT;
                byte |= grid_active_edges(&g, i, j, 1U, cos_limit) << (EDGES_SHIFT + 3U);
            }
            cell_bytes[(u64)j * cells_x + i] = (u8)byte;
        }
    }
    // Each block's lowest and highest height over its cells' corners.
    void *blocks_bytes = (u8 *)h + blocks_offset;
    f32 *blocks = blocks_bytes;
    for (u32 bj = 0U; bj < blocks_z; ++bj) {
        for (u32 bi = 0U; bi < blocks_x; ++bi) {
            f32 b_lo = FLT_MAX;
            f32 b_hi = -FLT_MAX;
            const u32 i_end = nph_min_u32(bi * BLOCK + BLOCK, cells_x);
            const u32 j_end = nph_min_u32(bj * BLOCK + BLOCK, cells_z);
            for (u32 j = bj * BLOCK; j < j_end; ++j) {
                for (u32 i = bi * BLOCK; i < i_end; ++i) {
                    if ((cell_bytes[(u64)j * cells_x + i] & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
                        continue;
                    }
                    for (u32 k = 0U; k < 4U; ++k) {
                        const f32 y = heights[(u64)(j + (k & 1U)) * cols + i + (k >> 1U)];
                        b_lo = nph_f32_min(b_lo, y);
                        b_hi = nph_f32_max(b_hi, y);
                    }
                }
            }
            blocks[2ULL * ((u64)bj * blocks_x + bi)] = b_lo;
            blocks[2ULL * ((u64)bj * blocks_x + bi) + 1U] = b_hi;
        }
    }
    NPH_ASSERT_MSG(nph_physics_heightfield_validate(h, h->byte_size),
                   "heightfield build produced an invalid heightfield");
    *out = h;
    return NPH_RESULT_OK;
}

void nph_physics_heightfield_free(NphPhysicsHeightfield *heightfield, NphAllocator *alloc)
{
    if (heightfield != NULL) {
        NPH_FREE(alloc, heightfield, heightfield->byte_size, 16U);
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

#define FIELD_FAIL(...)                                                                            \
    do {                                                                                           \
        NPH_LOG_WARN(&NPH_LOG_PHYSICS, "heightfield validate: " __VA_ARGS__);                      \
        return false;                                                                              \
    } while (0)

static bool range_ok(u64 offset, u64 bytes, u64 size)
{
    return offset >= HEADER_BYTES && (offset & 15U) == 0U && offset + bytes <= size;
}

bool nph_physics_heightfield_validate(const void *block, u32 size)
{
    if (block == NULL || size < HEADER_BYTES || ((uptr)block & 15U) != 0U) {
        FIELD_FAIL("no 16-byte aligned header in %u bytes", size);
    }
    const NphPhysicsHeightfield *h = block;
    if (h->byte_size < HEADER_BYTES || h->byte_size > size) {
        FIELD_FAIL("byte_size %u outside the %u-byte block", h->byte_size, size);
    }
    const u32 cols = h->columns;
    const u32 rows = h->rows;
    if (cols < 2U || rows < 2U || cols > NPH_PHYSICS_HEIGHTFIELD_MAX_SAMPLES ||
        rows > NPH_PHYSICS_HEIGHTFIELD_MAX_SAMPLES || h->flags != 0U || !(h->spacing_x > 0.0F) ||
        !(h->spacing_z > 0.0F) ||
        !((f64)(cols - 1U) * (f64)h->spacing_x <= (f64)NPH_PHYSICS_MAX_COORDINATE) ||
        !((f64)(rows - 1U) * (f64)h->spacing_z <= (f64)NPH_PHYSICS_MAX_COORDINATE)) {
        FIELD_FAIL("counts %u x %u or spacings %g, %g out of range", cols, rows, (f64)h->spacing_x,
                   (f64)h->spacing_z);
    }
    const u32 cells_x = cols - 1U;
    const u32 cells_z = rows - 1U;
    const u32 blocks_x = (cells_x + BLOCK - 1U) / BLOCK;
    const u32 blocks_z = (cells_z + BLOCK - 1U) / BLOCK;
    const u64 bytes = h->byte_size;
    if (h->block_columns != blocks_x || h->block_rows != blocks_z ||
        !range_ok(h->heights_offset, (u64)cols * rows * sizeof(f32), bytes) ||
        !range_ok(h->cells_offset, (u64)cells_x * cells_z, bytes) ||
        !range_ok(h->blocks_offset, 2ULL * blocks_x * blocks_z * sizeof(f32), bytes)) {
        FIELD_FAIL("arrays outside the %u-byte block", h->byte_size);
    }
    // The bounds are the grid's own along X and Z, and hold every height.
    const NphAABB b = h->bounds;
    const f32 x_end = (f32)((f64)cells_x * (f64)h->spacing_x);
    const f32 z_end = (f32)((f64)cells_z * (f64)h->spacing_z);
    if (b.min.x != 0.0F || b.min.z != 0.0F || b.max.x != x_end || b.max.z != z_end ||
        !(b.min.y <= b.max.y) || !(nph_f32_abs(b.min.y) <= NPH_PHYSICS_MAX_COORDINATE) ||
        !(nph_f32_abs(b.max.y) <= NPH_PHYSICS_MAX_COORDINATE)) {
        FIELD_FAIL("bounds not the grid's");
    }
    const f32 *heights = nph_physics_heightfield_heights(h);
    for (u64 s = 0U; s < (u64)cols * rows; ++s) {
        if (!(heights[s] >= b.min.y && heights[s] <= b.max.y)) {
            FIELD_FAIL("height %llu not finite or outside the bounds", (unsigned long long)s);
        }
    }
    // Every block's range holds its cells' corners (an all-hole block
    // holds the empty pair, which no query overlaps).
    const u8 *cell_bytes = nph_physics_heightfield_cells(h);
    const f32 *blocks = nph_physics_heightfield_blocks(h);
    for (u32 bj = 0U; bj < blocks_z; ++bj) {
        for (u32 bi = 0U; bi < blocks_x; ++bi) {
            const f32 b_lo = blocks[2ULL * ((u64)bj * blocks_x + bi)];
            const f32 b_hi = blocks[2ULL * ((u64)bj * blocks_x + bi) + 1U];
            bool any = false;
            const u32 i_end = nph_min_u32(bi * BLOCK + BLOCK, cells_x);
            const u32 j_end = nph_min_u32(bj * BLOCK + BLOCK, cells_z);
            for (u32 j = bj * BLOCK; j < j_end; ++j) {
                for (u32 i = bi * BLOCK; i < i_end; ++i) {
                    if ((cell_bytes[(u64)j * cells_x + i] & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
                        continue;
                    }
                    any = true;
                    for (u32 k = 0U; k < 4U; ++k) {
                        const f32 y = heights[(u64)(j + (k & 1U)) * cols + i + (k >> 1U)];
                        if (!(y >= b_lo && y <= b_hi)) {
                            FIELD_FAIL("block (%u, %u) does not hold its cells", bi, bj);
                        }
                    }
                }
            }
            if (!any && !(b_lo == FLT_MAX && b_hi == -FLT_MAX)) {
                FIELD_FAIL("block (%u, %u) is all holes but not empty", bi, bj);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Boxes
// ---------------------------------------------------------------------------

// A cell range [lo, hi] along one axis for [a, b] in the grid's frame:
// false when it misses the grid.
static bool cell_range(f32 a, f32 b, f32 spacing, u32 cells, u32 *lo, u32 *hi)
{
    const f32 end = (f32)((f64)cells * (f64)spacing);
    if (!(a <= b) || b < 0.0F || a > end) {
        return false;
    }
    const f32 fa = nph_f32_floor(a / spacing);
    const f32 fb = nph_f32_floor(b / spacing);
    *lo = fa <= 0.0F ? 0U : nph_min_u32((u32)fa, cells - 1U);
    *hi = fb <= 0.0F ? 0U : nph_min_u32((u32)fb, cells - 1U);
    return true;
}

static f32 cell_low(const NphPhysicsHeightfield *h, u32 i, u32 j, f32 *high)
{
    const f32 *heights = nph_physics_heightfield_heights(h);
    const u64 r0 = (u64)j * h->columns + i;
    const u64 r1 = r0 + h->columns;
    const f32 a = heights[r0];
    const f32 b = heights[r1];
    const f32 c = heights[r0 + 1U];
    const f32 d = heights[r1 + 1U];
    *high = nph_f32_max(nph_f32_max(a, b), nph_f32_max(c, d));
    return nph_f32_min(nph_f32_min(a, b), nph_f32_min(c, d));
}

// The triangles of the cells under `box`, block by block and cell by cell
// in rows. A triangle spans its cell in X and Z, and the cells are the ones
// the box overlaps there, so only its heights can miss the box: its
// vertices are made (in 64 bits, less `origin`) only for a visit that wants
// them (`v` NULL otherwise).
static void visit_cells(const NphPhysicsHeightfield *h, NphAABB box, NphDVec3 origin, bool vertices,
                        NphPhysicsSurfaceVisit visit, void *user)
{
    u32 i_lo = 0U;
    u32 i_hi = 0U;
    u32 j_lo = 0U;
    u32 j_hi = 0U;
    const u32 cells_x = h->columns - 1U;
    if (!cell_range(box.min.x, box.max.x, h->spacing_x, cells_x, &i_lo, &i_hi) ||
        !cell_range(box.min.z, box.max.z, h->spacing_z, h->rows - 1U, &j_lo, &j_hi) ||
        !(box.min.y <= box.max.y) || box.max.y < h->bounds.min.y || box.min.y > h->bounds.max.y) {
        return;
    }
    const u8 *cells = nph_physics_heightfield_cells(h);
    const f32 *blocks = nph_physics_heightfield_blocks(h);
    const f32 *heights = nph_physics_heightfield_heights(h);
    for (u32 bj = j_lo / BLOCK; bj <= j_hi / BLOCK; ++bj) {
        for (u32 bi = i_lo / BLOCK; bi <= i_hi / BLOCK; ++bi) {
            const u64 slot = 2ULL * ((u64)bj * h->block_columns + bi);
            if (blocks[slot + 1U] < box.min.y || blocks[slot] > box.max.y) {
                continue; // the block's heights miss the box (or it is all holes)
            }
            const u32 j0 = nph_max_u32(j_lo, bj * BLOCK);
            const u32 j1 = nph_min_u32(j_hi, bj * BLOCK + BLOCK - 1U);
            const u32 i0 = nph_max_u32(i_lo, bi * BLOCK);
            const u32 i1 = nph_min_u32(i_hi, bi * BLOCK + BLOCK - 1U);
            for (u32 j = j0; j <= j1; ++j) {
                for (u32 i = i0; i <= i1; ++i) {
                    const u32 c = cell_index(h, i, j);
                    const u32 cell = cells[c];
                    if ((cell & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
                        continue;
                    }
                    // The corners' heights, A B C D (k_corners' order).
                    const u64 r0 = (u64)j * h->columns + i;
                    const f32 y[4] = {heights[r0], heights[r0 + h->columns], heights[r0 + 1U],
                                      heights[r0 + h->columns + 1U]};
                    const u32 flip = (cell & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U ? 1U : 0U;
                    for (u32 k = 0U; k < 2U; ++k) {
                        const u8 *corner = k_corners[flip][k];
                        const f32 a = y[corner[0]];
                        const f32 b = y[corner[1]];
                        const f32 d = y[corner[2]];
                        if (nph_f32_max(nph_f32_max(a, b), d) < box.min.y ||
                            nph_f32_min(nph_f32_min(a, b), d) > box.max.y) {
                            continue;
                        }
                        NphVec3 v[3];
                        if (vertices) {
                            triangle_at(h, i, j, cell, k, origin, v);
                        }
                        if (!visit(user, 2U * c + k, vertices ? v : NULL,
                                   triangle_edges(cell, k))) {
                            return;
                        }
                    }
                }
            }
        }
    }
}

void nph_physics_heightfield_visit_box(const NphPhysicsHeightfield *h, NphAABB box, NphDVec3 origin,
                                       NphPhysicsSurfaceVisit visit, void *user)
{
    visit_cells(h, box, origin, true, visit, user);
}

typedef struct OverlapAdapter {
    NphPhysicsMeshVisit visit;
    void *user;
} OverlapAdapter;

static bool overlap_adapter(void *user, u32 triangle, const NphVec3 v[3], u32 active_edges)
{
    NPH_UNUSED(v);
    NPH_UNUSED(active_edges);
    const OverlapAdapter *a = user;
    return a->visit(a->user, triangle);
}

void nph_physics_heightfield_overlap(const NphPhysicsHeightfield *h, NphAABB box,
                                     NphPhysicsMeshVisit visit, void *user)
{
    NPH_ASSERT(h != NULL && visit != NULL);
    OverlapAdapter a = {visit, user};
    visit_cells(h, box, nph_dvec3_zero(), false, overlap_adapter, &a);
}

// ---------------------------------------------------------------------------
// Sweeps
// ---------------------------------------------------------------------------

// Where a box of half extents `extent` centred at `center`, moving by
// `delta`, first meets the box [lo, hi]: the slab test of its centre
// against the box grown by its extent. FLT_MAX for a miss before `limit`.
static f32 sweep_entry(NphVec3 lo, NphVec3 hi, NphVec3 center, NphVec3 extent, NphVec3 delta,
                       f32 limit)
{
    const NphAABB box = {nph_vec3_sub(lo, extent), nph_vec3_add(hi, extent)};
    f32 t = 0.0F;
    return nph_ray_intersects_aabb(nph_ray(center, delta), box, limit, &t, NULL) ? t : FLT_MAX;
}

// The k-th of [lo, hi] in the motion's direction along an axis.
static u32 in_motion_order(u32 lo, u32 hi, u32 k, bool backwards)
{
    return backwards ? hi - k : lo + k;
}

void nph_physics_heightfield_visit_sweep(const NphPhysicsHeightfield *h, NphVec3 center,
                                         NphVec3 extent, NphVec3 delta, NphDVec3 origin,
                                         NphPhysicsSweepVisit visit, void *user)
{
    // The cells under the swept box's bounds.
    const NphVec3 end = nph_vec3_add(center, delta);
    const NphVec3 lo = nph_vec3_sub(nph_vec3_min(center, end), extent);
    const NphVec3 hi = nph_vec3_add(nph_vec3_max(center, end), extent);
    u32 i_lo = 0U;
    u32 i_hi = 0U;
    u32 j_lo = 0U;
    u32 j_hi = 0U;
    if (!cell_range(lo.x, hi.x, h->spacing_x, h->columns - 1U, &i_lo, &i_hi) ||
        !cell_range(lo.z, hi.z, h->spacing_z, h->rows - 1U, &j_lo, &j_hi) ||
        hi.y < h->bounds.min.y || lo.y > h->bounds.max.y) {
        return;
    }
    const u8 *cells = nph_physics_heightfield_cells(h);
    const f32 *blocks = nph_physics_heightfield_blocks(h);
    const f32 sx = h->spacing_x;
    const f32 sz = h->spacing_z;
    // Blocks, then their cells, in the direction the motion runs along X and
    // Z: near ones come first, and what the closest hit so far is short of
    // is skipped.
    const bool back_x = delta.x < 0.0F;
    const bool back_z = delta.z < 0.0F;
    f32 best = 1.0F;
    const u32 bi_lo = i_lo / BLOCK;
    const u32 bi_hi = i_hi / BLOCK;
    const u32 bj_lo = j_lo / BLOCK;
    const u32 bj_hi = j_hi / BLOCK;
    for (u32 bjk = 0U; bjk <= bj_hi - bj_lo; ++bjk) {
        const u32 bj = in_motion_order(bj_lo, bj_hi, bjk, back_z);
        for (u32 bik = 0U; bik <= bi_hi - bi_lo; ++bik) {
            const u32 bi = in_motion_order(bi_lo, bi_hi, bik, back_x);
            const u64 slot = 2ULL * ((u64)bj * h->block_columns + bi);
            if (!(blocks[slot] <= blocks[slot + 1U])) {
                continue; // all holes
            }
            const u32 i0 = nph_max_u32(i_lo, bi * BLOCK);
            const u32 i1 = nph_min_u32(i_hi, bi * BLOCK + BLOCK - 1U);
            const u32 j0 = nph_max_u32(j_lo, bj * BLOCK);
            const u32 j1 = nph_min_u32(j_hi, bj * BLOCK + BLOCK - 1U);
            const NphVec3 block_lo = nph_vec3((f32)i0 * sx, blocks[slot], (f32)j0 * sz);
            const NphVec3 block_hi =
                nph_vec3((f32)(i1 + 1U) * sx, blocks[slot + 1U], (f32)(j1 + 1U) * sz);
            if (sweep_entry(block_lo, block_hi, center, extent, delta, best) > best) {
                continue;
            }
            for (u32 jk = 0U; jk <= j1 - j0; ++jk) {
                const u32 j = in_motion_order(j0, j1, jk, back_z);
                for (u32 ik = 0U; ik <= i1 - i0; ++ik) {
                    const u32 i = in_motion_order(i0, i1, ik, back_x);
                    const u32 c = cell_index(h, i, j);
                    const u32 cell = cells[c];
                    if ((cell & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
                        continue;
                    }
                    f32 high = 0.0F;
                    const f32 low = cell_low(h, i, j, &high);
                    if (sweep_entry(nph_vec3((f32)i * sx, low, (f32)j * sz),
                                    nph_vec3((f32)(i + 1U) * sx, high, (f32)(j + 1U) * sz), center,
                                    extent, delta, best) > best) {
                        continue;
                    }
                    for (u32 k = 0U; k < 2U; ++k) {
                        NphVec3 v[3];
                        triangle_at(h, i, j, cell, k, origin, v);
                        const f32 want = visit(user, 2U * c + k, v, triangle_edges(cell, k));
                        if (want < 0.0F) {
                            return;
                        }
                        best = nph_f32_min(best, want);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Rays
// ---------------------------------------------------------------------------

// A walk over a grid's cells (or blocks) in the XZ plane, in the order a ray
// crosses them (Amanatides and Woo 1987). The crossing of the next line is
// computed from the cell's index each step, not accumulated, so rounding
// cannot drift along a long ray.
typedef struct Walk {
    NphVec3 origin;
    NphVec3 delta;
    f32 inv_x;
    f32 inv_z;
    f32 size_x; // a cell's (or block's) extent
    f32 size_z;
    u32 i;
    u32 j;
    u32 i_lo; // the cells the walk may visit
    u32 i_hi;
    u32 j_lo;
    u32 j_hi;
    f32 slack; // how near a corner, in metres, the walk counts as through it
} Walk;

static u32 walk_index(f32 x, f32 size, u32 lo, u32 hi)
{
    const f32 f = nph_f32_floor(x / size);
    if (!(f > (f32)lo)) {
        return lo;
    }
    return f >= (f32)hi ? hi : (u32)f;
}

static void walk_start(Walk *w, f32 t)
{
    w->i = walk_index(w->origin.x + t * w->delta.x, w->size_x, w->i_lo, w->i_hi);
    w->j = walk_index(w->origin.z + t * w->delta.z, w->size_z, w->j_lo, w->j_hi);
}

// The fractions where the ray leaves the current cell across X and Z.
static f32 walk_exit_x(const Walk *w)
{
    if (w->delta.x > 0.0F) {
        return ((f32)(w->i + 1U) * w->size_x - w->origin.x) * w->inv_x;
    }
    if (w->delta.x < 0.0F) {
        return ((f32)w->i * w->size_x - w->origin.x) * w->inv_x;
    }
    return FLT_MAX;
}

static f32 walk_exit_z(const Walk *w)
{
    if (w->delta.z > 0.0F) {
        return ((f32)(w->j + 1U) * w->size_z - w->origin.z) * w->inv_z;
    }
    if (w->delta.z < 0.0F) {
        return ((f32)w->j * w->size_z - w->origin.z) * w->inv_z;
    }
    return FLT_MAX;
}

// Steps to the next cell; false past the walk's cells.
static bool walk_step(Walk *w, f32 exit_x, f32 exit_z)
{
    if (exit_x <= exit_z) {
        if (w->delta.x > 0.0F) {
            if (w->i >= w->i_hi) {
                return false;
            }
            w->i += 1U;
        } else {
            if (w->i <= w->i_lo) {
                return false;
            }
            w->i -= 1U;
        }
    } else {
        if (w->delta.z > 0.0F) {
            if (w->j >= w->j_hi) {
                return false;
            }
            w->j += 1U;
        } else {
            if (w->j <= w->j_lo) {
                return false;
            }
            w->j -= 1U;
        }
    }
    return true;
}

// Whether the ray leaves the cell through its corner, to rounding: one
// line is crossed first and the other within the walk's slack of it. Then
// the watertight test (which shears the ray, where the walk steps it) may
// find the hit in either neighbour across the corner, though the walk
// visits one of them at most.
static bool walk_corner(const Walk *w, f32 exit_x, f32 exit_z)
{
    if (exit_x == FLT_MAX || exit_z == FLT_MAX) {
        return false;
    }
    if (exit_x <= exit_z) {
        const f32 z = w->origin.z + exit_x * w->delta.z;
        const f32 line = w->delta.z > 0.0F ? (f32)(w->j + 1U) * w->size_z : (f32)w->j * w->size_z;
        return nph_f32_abs(z - line) <= w->slack;
    }
    const f32 x = w->origin.x + exit_z * w->delta.x;
    const f32 line = w->delta.x > 0.0F ? (f32)(w->i + 1U) * w->size_x : (f32)w->i * w->size_x;
    return nph_f32_abs(x - line) <= w->slack;
}

// The neighbour of (i, j) one step along X (axis 0) or Z (1) in the ray's
// direction: false past [0, last].
static bool walk_neighbour(const Walk *w, u32 axis, u32 last_i, u32 last_j, u32 *ni, u32 *nj)
{
    *ni = w->i;
    *nj = w->j;
    if (axis == 0U) {
        if (w->delta.x > 0.0F ? w->i >= last_i : w->i == 0U) {
            return false;
        }
        *ni = w->delta.x > 0.0F ? w->i + 1U : w->i - 1U;
        return true;
    }
    if (w->delta.z > 0.0F ? w->j >= last_j : w->j == 0U) {
        return false;
    }
    *nj = w->delta.z > 0.0F ? w->j + 1U : w->j - 1U;
    return true;
}

// Whether the ray's heights over [ta, tb] reach [low, high], with a little
// slack for rounding.
static bool heights_meet(const Walk *w, f32 ta, f32 tb, f32 low, f32 high)
{
    const f32 ya = w->origin.y + ta * w->delta.y;
    const f32 yb = w->origin.y + tb * w->delta.y;
    const f32 slack = 1e-4F + 1e-6F * nph_f32_max(nph_f32_abs(low), nph_f32_abs(high));
    return nph_f32_min(ya, yb) <= high + slack && nph_f32_max(ya, yb) >= low - slack;
}

typedef struct RayHit {
    f32 fraction;
    u32 triangle;
} RayHit;

// The closest front hit of the ray on cell (i, j), over [ta, tb].
static bool ray_cell(const NphPhysicsHeightfield *h, const NphWatertightRay *ray, const Walk *w,
                     u32 i, u32 j, f32 ta, f32 tb, RayHit *hit)
{
    const u32 c = cell_index(h, i, j);
    const u32 cell = nph_physics_heightfield_cells(h)[c];
    if ((cell & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
        return false;
    }
    f32 high;
    const f32 low = cell_low(h, i, j, &high);
    if (!heights_meet(w, ta, tb, low, high)) {
        return false;
    }
    bool found = false;
    for (u32 k = 0U; k < 2U; ++k) {
        NphVec3 v[3];
        triangle_at(h, i, j, cell, k, nph_dvec3_zero(), v);
        bool back = false;
        const f32 f = nph_physics_watertight_hit(ray, v[0], v[1], v[2], &back);
        // Equal fractions keep the first found: deterministic.
        if (f >= 0.0F && !back && (!found || f < hit->fraction)) {
            hit->fraction = f;
            hit->triangle = 2U * c + k;
            found = true;
        }
    }
    return found;
}

// The closest front hit of the ray on the cells of one block over [ta, tb].
static bool ray_block(const NphPhysicsHeightfield *h, const NphWatertightRay *ray,
                      const Walk *blocks, f32 ta, f32 tb, RayHit *hit)
{
    Walk w = *blocks;
    w.size_x = h->spacing_x;
    w.size_z = h->spacing_z;
    w.i_lo = blocks->i * BLOCK;
    w.j_lo = blocks->j * BLOCK;
    w.i_hi = nph_min_u32(w.i_lo + BLOCK - 1U, h->columns - 2U);
    w.j_hi = nph_min_u32(w.j_lo + BLOCK - 1U, h->rows - 2U);
    walk_start(&w, ta);
    f32 t = ta;
    for (;;) {
        const f32 ex = walk_exit_x(&w);
        const f32 ez = walk_exit_z(&w);
        const f32 t_exit = nph_f32_min(nph_f32_min(ex, ez), tb);
        // The cells come in the ray's order: the first hit is the closest.
        if (ray_cell(h, ray, &w, w.i, w.j, t, t_exit, hit)) {
            return true;
        }
        // Through a corner, both neighbours across it, wherever they are in
        // the grid (the next block's walk starts past the corner).
        if (walk_corner(&w, ex, ez) && nph_f32_min(ex, ez) <= tb + 1e-6F) {
            for (u32 axis = 0U; axis < 2U; ++axis) {
                u32 ci = 0U;
                u32 cj = 0U;
                if (walk_neighbour(&w, axis, h->columns - 2U, h->rows - 2U, &ci, &cj) &&
                    ray_cell(h, ray, &w, ci, cj, t_exit, t_exit, hit)) {
                    return true;
                }
            }
        }
        if (t_exit >= tb || !walk_step(&w, ex, ez)) {
            return false;
        }
        t = t_exit;
    }
}

// The fractions [t0, t1] of the segment inside box b, by the slab test:
// false for a miss.
static bool clip_segment(NphAABB b, NphVec3 o, NphVec3 d, f32 *t0, f32 *t1)
{
    f32 lo = 0.0F;
    f32 hi = 1.0F;
    for (u32 a = 0U; a < 3U; ++a) {
        const f32 oa = nph_vec3_at(o, a);
        const f32 da = nph_vec3_at(d, a);
        const f32 mn = nph_vec3_at(b.min, a);
        const f32 mx = nph_vec3_at(b.max, a);
        if (nph_f32_abs(da) < 1e-30F) {
            if (oa < mn || oa > mx) {
                return false;
            }
            continue;
        }
        f32 ta = (mn - oa) / da;
        f32 tb = (mx - oa) / da;
        if (ta > tb) {
            const f32 s = ta;
            ta = tb;
            tb = s;
        }
        lo = nph_f32_max(lo, ta);
        hi = nph_f32_min(hi, tb);
        if (!(lo <= hi)) {
            return false;
        }
    }
    *t0 = lo;
    *t1 = hi;
    return true;
}

bool nph_physics_heightfield_raycast(const NphPhysicsHeightfield *h, NphVec3 origin, NphVec3 delta,
                                     NphPhysicsMeshHit *hit)
{
    NPH_ASSERT(h != NULL && hit != NULL);
    f32 t0;
    f32 t1;
    // The grid's box, a little grown so rounding at its faces loses nothing.
    const NphAABB box = nph_aabb_grow(h->bounds, 1e-4F);
    if (!nph_vec3_is_finite(origin) || !nph_vec3_is_finite(delta) ||
        nph_vec3_length_sq(delta) == 0.0F || !clip_segment(box, origin, delta, &t0, &t1)) {
        return false;
    }
    const NphWatertightRay ray = nph_physics_watertight_ray(origin, delta);
    Walk blocks;
    blocks.origin = origin;
    blocks.delta = delta;
    blocks.inv_x = delta.x != 0.0F ? 1.0F / delta.x : 0.0F;
    blocks.inv_z = delta.z != 0.0F ? 1.0F / delta.z : 0.0F;
    blocks.size_x = (f32)BLOCK * h->spacing_x;
    blocks.size_z = (f32)BLOCK * h->spacing_z;
    blocks.i_lo = 0U;
    blocks.j_lo = 0U;
    blocks.i_hi = h->block_columns - 1U;
    blocks.j_hi = h->block_rows - 1U;
    // Rounding in the walk's crossings, at the grid's scale.
    blocks.slack = 8.0F * FLT_EPSILON *
                   (nph_f32_abs(origin.x) + nph_f32_abs(origin.z) + nph_f32_abs(delta.x) +
                    nph_f32_abs(delta.z) + h->bounds.max.x + h->bounds.max.z);
    walk_start(&blocks, t0);
    const f32 *ranges = nph_physics_heightfield_blocks(h);
    f32 t = t0;
    RayHit found = {0.0F, 0U};
    for (;;) {
        const f32 ex = walk_exit_x(&blocks);
        const f32 ez = walk_exit_z(&blocks);
        const f32 t_exit = nph_f32_min(nph_f32_min(ex, ez), t1);
        const u64 slot = 2ULL * ((u64)blocks.j * h->block_columns + blocks.i);
        if (ranges[slot] <= ranges[slot + 1U] &&
            heights_meet(&blocks, t, t_exit, ranges[slot], ranges[slot + 1U]) &&
            ray_block(h, &ray, &blocks, t, t_exit, &found)) {
            break;
        }
        // Through a block's corner, both blocks across it.
        bool hit_corner = false;
        if (t_exit < t1 && walk_corner(&blocks, ex, ez)) {
            for (u32 axis = 0U; axis < 2U && !hit_corner; ++axis) {
                Walk corner = blocks;
                if (!walk_neighbour(&blocks, axis, h->block_columns - 1U, h->block_rows - 1U,
                                    &corner.i, &corner.j)) {
                    continue;
                }
                const u64 cs = 2ULL * ((u64)corner.j * h->block_columns + corner.i);
                hit_corner = ranges[cs] <= ranges[cs + 1U] &&
                             heights_meet(&corner, t_exit, t_exit, ranges[cs], ranges[cs + 1U]) &&
                             ray_block(h, &ray, &corner, t_exit, t_exit, &found);
            }
        }
        if (hit_corner) {
            break;
        }
        if (t_exit >= t1 || !walk_step(&blocks, ex, ez)) {
            return false;
        }
        t = t_exit;
    }
    NphVec3 v[3];
    NPH_VERIFY(nph_physics_heightfield_triangle(h, found.triangle, v, NULL));
    hit->fraction = found.fraction;
    hit->normal = nph_vec3_normalize_or_zero(
        nph_vec3_cross(nph_vec3_sub(v[1], v[0]), nph_vec3_sub(v[2], v[0])));
    hit->triangle = found.triangle;
    hit->original = found.triangle;
    hit->back = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Heights
// ---------------------------------------------------------------------------

bool nph_physics_heightfield_height_at(const NphPhysicsHeightfield *h, f32 x, f32 z, f32 *height,
                                       NphVec3 *normal)
{
    NPH_ASSERT(h != NULL && height != NULL);
    u32 i = 0U;
    u32 i_hi = 0U;
    u32 j = 0U;
    u32 j_hi = 0U;
    if (!cell_range(x, x, h->spacing_x, h->columns - 1U, &i, &i_hi) ||
        !cell_range(z, z, h->spacing_z, h->rows - 1U, &j, &j_hi)) {
        return false;
    }
    const u32 cell = nph_physics_heightfield_cells(h)[cell_index(h, i, j)];
    if ((cell & NPH_PHYSICS_HEIGHTFIELD_HOLE) != 0U) {
        return false;
    }
    // Which triangle: by the cell coordinates (u, v) against the diagonal.
    const f32 u = x / h->spacing_x - (f32)i;
    const f32 v = z / h->spacing_z - (f32)j;
    const bool flip = (cell & NPH_PHYSICS_HEIGHTFIELD_FLIP) != 0U;
    const u32 k = flip ? (u <= v ? 0U : 1U) : (u + v <= 1.0F ? 0U : 1U);
    NphVec3 p[3];
    triangle_at(h, i, j, cell, k, nph_dvec3_zero(), p);
    const NphVec3 n = nph_vec3_cross(nph_vec3_sub(p[1], p[0]), nph_vec3_sub(p[2], p[0]));
    // The plane through the triangle at (x, z): n.y is positive (the
    // triangles face up and spacings are positive).
    *height = p[0].y - (n.x * (x - p[0].x) + n.z * (z - p[0].z)) / n.y;
    if (normal != NULL) {
        *normal = nph_vec3_normalize(n);
    }
    return true;
}
