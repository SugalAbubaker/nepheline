// SPDX-License-Identifier: Apache-2.0
// The voxels of a group's triangles (cluster_lod_internal.h lod_voxelize;
// render/voxel.h; docs/render.md "Voxels"; ADR-0061).
//
// Each cell of the grid the triangles overlap (Akenine-Möller, "Fast 3D
// Triangle-Box Overlap Testing", JGT 2001) is probed by a jittered grid of
// NPH_RENDER_VOXEL_RAYS^2 rays along each axis, every ray tested against
// every triangle of the cell over the cell's own extent: a ray hits where
// any triangle crosses it inside the cell, so the share of rays an axis
// hits is the cell's coverage along that axis, the union of its triangles'
// projections, which counts geometry hidden behind other geometry once
// (Heitz & Neyret, HPG 2012). Each ray also keeps its first and last hit,
// the surfaces seen from the two ends: the voxel's look is averaged over
// those (a quarter of the rays, every other one each way, when they hit),
// what is seen of the cell from outside rather than what is hidden in it.
// The footprint lies within the box of every hit point, widened by half a
// ray's spacing (so that a sheet across the cell keeps the cell's whole
// extent). A surface's (its normals aligned) is that box: it closes up with
// its neighbours'. An aggregate's is smaller, sized so that its projected
// area along each axis is that axis's coverage: the ellipsoid inscribed in
// a box (whose projected area varies with direction as an isotropic cloud's
// does not, render/voxel.h) where an ellipsoid can reach the coverages, a
// box where it cannot. It lies at a place within the hits' box hashed from
// the cell: geometry spread through a cell is no more in its middle than
// anywhere else, and footprints that all sat in their cells' middles would
// line up along each axis, so that a ray down a column of cells would meet
// them all or none, where the pieces they stand for are independent from
// cell to cell (Cook et al., SIGGRAPH 2007, place their stand-ins at random
// for the same reason). Geometry gathered in a corner keeps it: the box it
// lies in is small. A needle's (the hits strung along one line) is a capsule
// along that line between the hits' farthest points on it, of the radius of
// their spread across it (the hits lie on the needle's surface): its
// projected area then follows the needle's as the view turns, where an
// ellipsoid's would not fall to its section end-on.
//
// The rays' jitter (a table of offsets each cell reads from its own hashed
// shift) and the footprints' places are hashes of the cell, so a regular
// structure does not alias with the grid, and the result is the same for
// the same input.
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/scalar.h>

#include "cluster_lod_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAYS      NPH_RENDER_VOXEL_RAYS
#define AXIS_RAYS (RAYS * RAYS)

// The share of rays below which an axis counts as one ray's worth (an
// edge-on sheet stops none).
#define MIN_COVERAGE (1.0 / (f64)AXIS_RAYS)

typedef struct CellRef {
    u64 key;
    u32 source;
    u32 pad;
} CellRef;

static u64 cell_key(i64 x, i64 y, i64 z)
{
    const i64 bias = LOD_VOXEL_COORD_LIMIT;
    return ((u64)(z + bias) << 42U) | ((u64)(y + bias) << 21U) | (u64)(x + bias);
}

static void key_coords(u64 key, i32 c[3])
{
    const i64 bias = LOD_VOXEL_COORD_LIMIT;
    c[0] = (i32)((i64)(key & 0x1FFFFFU) - bias);
    c[1] = (i32)((i64)((key >> 21U) & 0x1FFFFFU) - bias);
    c[2] = (i32)((i64)((key >> 42U) & 0x1FFFFFU) - bias);
}

static int compare_refs(const void *x, const void *y)
{
    const CellRef *p = (const CellRef *)x;
    const CellRef *q = (const CellRef *)y;
    if (p->key != q->key) {
        return p->key < q->key ? -1 : 1;
    }
    return p->source < q->source ? -1 : (p->source > q->source ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Triangles against cells
// ---------------------------------------------------------------------------

static void sub3(const f64 *a, const f64 *b, f64 *out)
{
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static void cross3(const f64 *a, const f64 *b, f64 *out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static f64 dot3(const f64 *a, const f64 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Whether the triangle `v` (relative to the box's centre) and the cube of
// half-extent `half` about the origin are separated along `axis`. (The 3x3
// arrays the functions below read are not const: C17 does not convert a
// pointer to an array to one to a const array, C23 does.)
static bool separated(f64 v[3][3], const f64 *axis, f64 half)
{
    const f64 p0 = dot3(v[0], axis);
    const f64 p1 = dot3(v[1], axis);
    const f64 p2 = dot3(v[2], axis);
    const f64 lo = nph_f64_min(p0, nph_f64_min(p1, p2));
    const f64 hi = nph_f64_max(p0, nph_f64_max(p1, p2));
    const f64 r = half * (nph_f64_abs(axis[0]) + nph_f64_abs(axis[1]) + nph_f64_abs(axis[2]));
    return lo > r || hi < -r;
}

// The separating-axis test of Akenine-Möller (2001): the box's three
// normals, the triangle's normal, and the nine cross products of their
// edges. A triangle touching the box's boundary overlaps it.
static bool tri_box_overlap(const f64 center[3], f64 half, f64 tri[3][3])
{
    f64 v[3][3];
    for (u32 k = 0; k < 3U; ++k) {
        sub3(tri[k], center, v[k]);
    }
    f64 e[3][3];
    sub3(v[1], v[0], e[0]);
    sub3(v[2], v[1], e[1]);
    sub3(v[0], v[2], e[2]);
    for (u32 j = 0; j < 3U; ++j) {
        f64 unit[3] = {0.0, 0.0, 0.0};
        unit[j] = 1.0;
        if (separated(v, unit, half)) {
            return false;
        }
        for (u32 i = 0; i < 3U; ++i) {
            f64 axis[3];
            cross3(unit, e[i], axis);
            if ((axis[0] != 0.0 || axis[1] != 0.0 || axis[2] != 0.0) && separated(v, axis, half)) {
                return false;
            }
        }
    }
    f64 normal[3];
    cross3(e[0], e[1], normal);
    return !separated(v, normal, half);
}

// A source's corners in double precision; false for a triangle of no area
// (nothing a ray can hit) or of a submesh that never voxelizes.
static bool source_corners(const LodVoxelInput *in, u32 s, f64 p[3][3])
{
    const LodVoxelSource *src = &in->sources[s];
    for (u32 k = 0; k < 3U; ++k) {
        const NphVec3 q = in->positions[src->v[k]];
        p[k][0] = (f64)q.x;
        p[k][1] = (f64)q.y;
        p[k][2] = (f64)q.z;
    }
    if (in->sampler->submesh_surface[src->submesh] == NPH_RENDER_VOXEL_SURFACE_NONE) {
        return false;
    }
    f64 e0[3];
    f64 e1[3];
    f64 n[3];
    sub3(p[1], p[0], e0);
    sub3(p[2], p[0], e1);
    cross3(e0, e1, n);
    return dot3(n, n) > 0.0;
}

// The range of cells the source's bounds span, false past the coordinate
// limit.
static bool source_cells(f64 p[3][3], f64 cell, i64 lo[3], i64 hi[3])
{
    for (u32 a = 0; a < 3U; ++a) {
        const f64 mn = nph_f64_min(p[0][a], nph_f64_min(p[1][a], p[2][a]));
        const f64 mx = nph_f64_max(p[0][a], nph_f64_max(p[1][a], p[2][a]));
        const f64 l = floor(mn / cell);
        const f64 h = floor(mx / cell);
        if (!(l > -(f64)LOD_VOXEL_COORD_LIMIT) || !(h < (f64)LOD_VOXEL_COORD_LIMIT - 1.0)) {
            return false;
        }
        lo[a] = (i64)l;
        hi[a] = (i64)h;
    }
    return true;
}

// Calls `visit(ctx, key)` for each cell of edge `cell` source `s` overlaps;
// stops at the first that does not return NPH_RESULT_OK.
typedef NphResult (*CellVisit)(void *ctx, u64 key, u32 source);

static NphResult visit_cells(const LodVoxelInput *in, u32 s, f64 cell, CellVisit visit, void *ctx)
{
    f64 p[3][3];
    if (!source_corners(in, s, p)) {
        return NPH_RESULT_OK;
    }
    i64 lo[3];
    i64 hi[3];
    if (!source_cells(p, cell, lo, hi)) {
        return NPH_RESULT_OVERFLOW;
    }
    const f64 half = 0.5 * cell;
    for (i64 z = lo[2]; z <= hi[2]; ++z) {
        for (i64 y = lo[1]; y <= hi[1]; ++y) {
            for (i64 x = lo[0]; x <= hi[0]; ++x) {
                const f64 center[3] = {((f64)x + 0.5) * cell, ((f64)y + 0.5) * cell,
                                       ((f64)z + 0.5) * cell};
                if (tri_box_overlap(center, half, p)) {
                    NPH_TRY(visit(ctx, cell_key(x, y, z), s));
                }
            }
        }
    }
    return NPH_RESULT_OK;
}

typedef struct CountCtx {
    NphHashMap set;
    u32 limit;
} CountCtx;

static NphResult count_visit(void *ctx, u64 key, u32 source)
{
    (void)source;
    CountCtx *c = (CountCtx *)ctx;
    if (nph_hashmap_contains_u64(&c->set, key)) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(nph_hashmap_insert_u64(&c->set, key, NULL));
    // Past the limit the count is not needed exactly.
    return nph_hashmap_len(&c->set) > c->limit ? NPH_RESULT_FULL : NPH_RESULT_OK;
}

NphResult lod_voxel_count(const LodVoxelInput *in, f64 cell, u32 limit, u32 *out_count)
{
    NPH_ASSERT(in != NULL && out_count != NULL && cell > 0.0);
    *out_count = 0U;
    CountCtx ctx;
    ctx.limit = limit;
    const NphHashMapDesc desc = nph_hashmap_desc_u64(0U);
    NPH_TRY(nph_hashmap_init(&ctx.set, in->scratch, &desc));
    NphResult r = NPH_RESULT_OK;
    for (u32 s = 0; s < in->source_count && r == NPH_RESULT_OK; ++s) {
        r = visit_cells(in, s, cell, count_visit, &ctx);
    }
    *out_count = nph_hashmap_len(&ctx.set);
    nph_hashmap_free(&ctx.set);
    return r == NPH_RESULT_FULL ? NPH_RESULT_OK : r;
}

typedef struct RefCtx {
    NPH_ARRAY(CellRef) refs;
} RefCtx;

static NphResult ref_visit(void *ctx, u64 key, u32 source)
{
    RefCtx *c = (RefCtx *)ctx;
    const CellRef ref = {key, source, 0U};
    return NPH_ARRAY_PUSH(&c->refs, ref);
}

// ---------------------------------------------------------------------------
// One cell's rays
// ---------------------------------------------------------------------------

// A ray of a cell: its first and last hit along its axis, as depths in cell
// units, the sources hit there, and their barycentrics (of corners 1 and 2).
typedef struct Ray {
    u32 stamp; // the cell it was last hit in (1 + its run's index)
    u32 near_source;
    u32 far_source;
    f32 near_depth;
    f32 far_depth;
    f32 near_b[2];
    f32 far_b[2];
} Ray;

// A 64-bit mix (splitmix64's finaliser, Steele et al., "Fast Splittable
// Pseudorandom Number Generators", OOPSLA 2014).
static u64 mix64(u64 x)
{
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return x;
}

// The rays' jitter within their strata: a table of AXIS_RAYS offsets in [0,
// 1)^2, which each cell and axis reads from a hashed shift, so that each
// stratum's ray lies at a place of its own in every cell (one hash a cell
// and axis rather than one a ray).
static void jitter_table(f64 *table)
{
    for (u32 k = 0; k < AXIS_RAYS; ++k) {
        const u64 h = mix64(0x9E3779B97F4A7C15ULL * ((u64)k + 1U));
        table[(u64)k * 2U] = (f64)(h & 0xFFFFFFU) / 16777216.0;
        table[(u64)k * 2U + 1U] = (f64)((h >> 24U) & 0xFFFFFFU) / 16777216.0;
    }
}

typedef struct CellState {
    const LodVoxelInput *in;
    f64 cell;
    u64 key;
    f64 lo[3]; // the cell's lowest corner, mesh units
    u32 stamp;
    Ray *rays;        // 3 * AXIS_RAYS
    f64 bounds_lo[3]; // the hits' box, cell units
    f64 bounds_hi[3];
    // The hits' points' sums and products (cell units), for their spread.
    f64 point_sum[3];
    f64 point_products[3][3];
    f64 points;
    u32 hits[3];
    const f64 *jitter; // jitter_table's
    u32 shift[3];      // per axis: the cell's offset into it, rows in the high byte
} CellState;

// Ray (i, j) of axis `a` across the cell's face, in cell units.
static f64 ray_u(const CellState *st, u32 a, u32 i, u32 j)
{
    const u32 k =
        ((i + st->shift[a]) & (RAYS - 1U)) + RAYS * ((j + (st->shift[a] >> 8U)) & (RAYS - 1U));
    return ((f64)i + st->jitter[(u64)k * 2U]) / (f64)RAYS;
}

static f64 ray_w(const CellState *st, u32 a, u32 i, u32 j)
{
    const u32 k =
        ((i + st->shift[a]) & (RAYS - 1U)) + RAYS * ((j + (st->shift[a] >> 8U)) & (RAYS - 1U));
    return ((f64)j + st->jitter[(u64)k * 2U + 1U]) / (f64)RAYS;
}

static f64 edge2(f64 ax, f64 ay, f64 bx, f64 by, f64 px, f64 py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Source `s`'s hits along axis `a` of the cell. `p` its corners in cell
// units relative to the cell's lowest corner. Returns whether any ray hit.
static bool rasterise(CellState *st, u32 s, f64 p[3][3], u32 a)
{
    const u32 u = (a + 1U) % 3U;
    const u32 w = (a + 2U) % 3U;
    const f64 area = edge2(p[0][u], p[0][w], p[1][u], p[1][w], p[2][u], p[2][w]);
    if (area == 0.0) {
        return false; // edge-on: it covers no ray of this axis
    }
    const f64 inv_area = 1.0 / area;
    const f64 mn_u = nph_f64_min(p[0][u], nph_f64_min(p[1][u], p[2][u]));
    const f64 mx_u = nph_f64_max(p[0][u], nph_f64_max(p[1][u], p[2][u]));
    const f64 mn_w = nph_f64_min(p[0][w], nph_f64_min(p[1][w], p[2][w]));
    const f64 mx_w = nph_f64_max(p[0][w], nph_f64_max(p[1][w], p[2][w]));
    if (mx_u < 0.0 || mn_u >= 1.0 || mx_w < 0.0 || mn_w >= 1.0) {
        return false;
    }
    const i32 i0 = (i32)nph_f64_clamp(floor(mn_u * (f64)RAYS), 0.0, (f64)RAYS - 1.0);
    const i32 i1 = (i32)nph_f64_clamp(floor(mx_u * (f64)RAYS), 0.0, (f64)RAYS - 1.0);
    const i32 j0 = (i32)nph_f64_clamp(floor(mn_w * (f64)RAYS), 0.0, (f64)RAYS - 1.0);
    const i32 j1 = (i32)nph_f64_clamp(floor(mx_w * (f64)RAYS), 0.0, (f64)RAYS - 1.0);
    const LodVoxelSource *src = &st->in->sources[s];
    const bool masked =
        st->in->sampler->submesh_surface[src->submesh] == NPH_RENDER_VOXEL_SURFACE_MASKED;
    const f32 footprint = (f32)(st->cell / (f64)RAYS);
    bool any = false;
    for (i32 j = j0; j <= j1; ++j) {
        for (i32 i = i0; i <= i1; ++i) {
            const u32 r = (u32)i + (u32)j * RAYS;
            const f64 qu = ray_u(st, a, (u32)i, (u32)j);
            const f64 qw = ray_w(st, a, (u32)i, (u32)j);
            const f64 b0 = edge2(p[1][u], p[1][w], p[2][u], p[2][w], qu, qw) * inv_area;
            const f64 b1 = edge2(p[2][u], p[2][w], p[0][u], p[0][w], qu, qw) * inv_area;
            const f64 b2 = edge2(p[0][u], p[0][w], p[1][u], p[1][w], qu, qw) * inv_area;
            if (b0 < 0.0 || b1 < 0.0 || b2 < 0.0) {
                continue;
            }
            const f64 d = b0 * p[0][a] + b1 * p[1][a] + b2 * p[2][a];
            // The cell's own extent along the axis, half open so that a sheet
            // on a face between two cells is in one of them.
            if (!(d >= 0.0 && d < 1.0)) {
                continue;
            }
            if (masked) {
                const NphVec3 b = nph_vec3((f32)b0, (f32)b1, (f32)b2);
                if (!st->in->sampler->covers(st->in->sampler->user, src->submesh, src->v, b,
                                             footprint)) {
                    continue;
                }
            }
            any = true;
            Ray *ray = &st->rays[a * AXIS_RAYS + r];
            const f32 df = (f32)d;
            if (ray->stamp != st->stamp) {
                ray->stamp = st->stamp;
                ray->near_source = s;
                ray->far_source = s;
                ray->near_depth = df;
                ray->far_depth = df;
                ray->near_b[0] = (f32)b1;
                ray->near_b[1] = (f32)b2;
                ray->far_b[0] = (f32)b1;
                ray->far_b[1] = (f32)b2;
                st->hits[a] += 1U;
            } else {
                // Sources come in index order: a tie keeps the first.
                if (df < ray->near_depth) {
                    ray->near_source = s;
                    ray->near_depth = df;
                    ray->near_b[0] = (f32)b1;
                    ray->near_b[1] = (f32)b2;
                }
                if (df > ray->far_depth) {
                    ray->far_source = s;
                    ray->far_depth = df;
                    ray->far_b[0] = (f32)b1;
                    ray->far_b[1] = (f32)b2;
                }
            }
            f64 point[3];
            point[a] = d;
            point[u] = qu;
            point[w] = qw;
            st->points += 1.0;
            for (u32 k = 0; k < 3U; ++k) {
                st->point_sum[k] += point[k];
                for (u32 q = 0; q < 3U; ++q) {
                    st->point_products[k][q] += point[k] * point[q];
                }
            }
            for (u32 k = 0; k < 3U; ++k) {
                st->bounds_lo[k] = nph_f64_min(st->bounds_lo[k], point[k]);
                st->bounds_hi[k] = nph_f64_max(st->bounds_hi[k], point[k]);
            }
        }
    }
    return any;
}

// The symmetric 3 x 3 matrix `m`'s eigenvector of the largest eigenvalue,
// by cyclic Jacobi rotations (Golub & Van Loan, "Matrix Computations",
// §8.5), and that eigenvalue.
static f64 dominant_eigen(f64 m_in[3][3], f64 vec[3])
{
    f64 m[3][3];
    f64 v[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    memcpy(m, m_in, sizeof m);
    for (u32 sweep = 0; sweep < 32U; ++sweep) {
        const f64 off = m[0][1] * m[0][1] + m[0][2] * m[0][2] + m[1][2] * m[1][2];
        if (off < 1e-30) {
            break;
        }
        for (u32 p = 0; p < 2U; ++p) {
            for (u32 q = p + 1U; q < 3U; ++q) {
                if (m[p][q] == 0.0) {
                    continue;
                }
                const f64 theta = (m[q][q] - m[p][p]) / (2.0 * m[p][q]);
                const f64 t =
                    (theta >= 0.0 ? 1.0 : -1.0) / (nph_f64_abs(theta) + sqrt(theta * theta + 1.0));
                const f64 c = 1.0 / sqrt(t * t + 1.0);
                const f64 sn = t * c;
                for (u32 k = 0; k < 3U; ++k) {
                    const f64 mkp = m[k][p];
                    const f64 mkq = m[k][q];
                    m[k][p] = c * mkp - sn * mkq;
                    m[k][q] = sn * mkp + c * mkq;
                }
                for (u32 k = 0; k < 3U; ++k) {
                    const f64 mpk = m[p][k];
                    const f64 mqk = m[q][k];
                    m[p][k] = c * mpk - sn * mqk;
                    m[q][k] = sn * mpk + c * mqk;
                }
                for (u32 k = 0; k < 3U; ++k) {
                    const f64 vkp = v[k][p];
                    const f64 vkq = v[k][q];
                    v[k][p] = c * vkp - sn * vkq;
                    v[k][q] = sn * vkp + c * vkq;
                }
            }
        }
    }
    u32 best = 0U;
    for (u32 k = 1U; k < 3U; ++k) {
        if (m[k][k] > m[best][best]) {
            best = k;
        }
    }
    for (u32 k = 0; k < 3U; ++k) {
        vec[k] = v[k][best];
    }
    return m[best][best];
}

// What the cell's rays saw, sampled from the sources at their first and
// last hits (every other ray each way, or every ray where those saw none).
typedef struct CellLook {
    f64 color[3];
    f64 roughness;
    f64 metallic;
    f64 occlusion;
    f64 moment[3][3];
    f64 moment_weight;
    u32 count;
    // The submeshes seen, with their hits (a cell holds few).
    u32 submesh[8];
    u32 votes[8];
    u32 submeshes;
} CellLook;

static void look_add(CellState *st, CellLook *look, u32 s, const f32 b12[2])
{
    const LodVoxelSource *src = &st->in->sources[s];
    const NphVec3 b = nph_vec3(1.0F - b12[0] - b12[1], b12[0], b12[1]);
    NphRenderVoxelSample sample;
    memset(&sample, 0, sizeof sample);
    // Every other ray: their spacing is the samples' footprint.
    const f32 footprint = (f32)(2.0 * st->cell / (f64)RAYS);
    st->in->sampler->sample(st->in->sampler->user, src->submesh, src->v, b, footprint, &sample);
    look->color[0] += (f64)sample.base_color.x;
    look->color[1] += (f64)sample.base_color.y;
    look->color[2] += (f64)sample.base_color.z;
    look->roughness += (f64)sample.roughness;
    look->metallic += (f64)sample.metallic;
    look->occlusion += (f64)sample.occlusion;
    const f64 n[3] = {(f64)sample.normal.x, (f64)sample.normal.y, (f64)sample.normal.z};
    const f64 len2 = dot3(n, n);
    if (len2 > 0.0 && nph_f64_is_finite(len2)) {
        const f64 inv = 1.0 / sqrt(len2);
        const f64 u[3] = {n[0] * inv, n[1] * inv, n[2] * inv};
        // A ray along an axis sees a surface in proportion to its normal's
        // component along it; over the three axes, |nx| + |ny| + |nz|: each
        // hit weighed by its reciprocal counts the surface by its area.
        const f64 wgt = 1.0 / (nph_f64_abs(u[0]) + nph_f64_abs(u[1]) + nph_f64_abs(u[2]));
        for (u32 r = 0; r < 3U; ++r) {
            for (u32 c = 0; c < 3U; ++c) {
                look->moment[r][c] += wgt * u[r] * u[c];
            }
        }
        look->moment_weight += wgt;
    }
    look->count += 1U;
    u32 k = 0U;
    while (k < look->submeshes && look->submesh[k] != src->submesh) {
        ++k;
    }
    if (k == look->submeshes && k < 8U) {
        look->submesh[k] = src->submesh;
        look->votes[k] = 0U;
        look->submeshes += 1U;
    }
    if (k < look->submeshes) {
        look->votes[k] += 1U;
    }
}

static void look_rays(CellState *st, CellLook *look, bool every)
{
    for (u32 a = 0; a < 3U; ++a) {
        for (u32 r = 0; r < AXIS_RAYS; ++r) {
            const Ray *ray = &st->rays[a * AXIS_RAYS + r];
            if (ray->stamp != st->stamp) {
                continue;
            }
            const u32 i = r % RAYS;
            const u32 j = r / RAYS;
            if (!every && ((i | j) & 1U) != 0U) {
                continue;
            }
            look_add(st, look, ray->near_source, ray->near_b);
            look_add(st, look, ray->far_source, ray->far_b);
        }
    }
}

// A footprint's extents `e` (cell units) within `room` whose projected area
// along each axis is that axis's coverage: `area` times the product of the
// other two extents (1 for a box, pi / 4 for the ellipsoid inscribed in it),
// so e_x = sqrt(c_y c_z / (area c_x)) and its turns. Where that passes the
// room (a sheet across the cell at a slant covers two axes whole, which no
// box short of the whole cell does), the extent is the room's and the
// others grow until each coverage is met: a ray the geometry stops is not
// let through, at the price of stopping some edge-on ones. Returns whether
// every coverage is met.
static bool footprint_extents(const f64 cov[3], const f64 room[3], f64 area, f64 e[3])
{
    const f64 least = 1.0 / (f64)NPH_RENDER_VOXEL_BOX_STEPS;
    for (u32 a = 0; a < 3U; ++a) {
        const u32 b = (a + 1U) % 3U;
        const u32 c = (a + 2U) % 3U;
        e[a] = nph_f64_clamp(sqrt(cov[b] * cov[c] / (area * cov[a])), least, room[a]);
    }
    for (u32 pass = 0; pass < 2U; ++pass) {
        for (u32 a = 0; a < 3U; ++a) {
            const u32 b = (a + 1U) % 3U;
            const u32 c = (a + 2U) % 3U;
            if (area * e[b] * e[c] < cov[a]) {
                e[b] = nph_f64_min(room[b], cov[a] / (area * e[c]));
                if (area * e[b] * e[c] < cov[a]) {
                    e[c] = nph_f64_min(room[c], cov[a] / (area * e[b]));
                }
            }
        }
    }
    bool met = true;
    for (u32 a = 0; a < 3U; ++a) {
        met = met && area * e[(a + 1U) % 3U] * e[(a + 2U) % 3U] >= cov[a] * (1.0 - 1e-6);
    }
    return met;
}

// A cell's geometry is a surface, whose footprint is its bounds, where its
// normals' second moment along one axis is at least this (a sheet's is 1)
// and the bounds' projection along the axis of most coverage is within
// SURFACE_FILL of that coverage: a sheet across the cell fills its bounds'
// shadow, a thin sliver across it at a slant does not.
#define SURFACE_MOMENT 0.9
#define SURFACE_FILL   1.25

// A cell's hits are strung along one line where their spread's largest
// eigenvalue is at least this share of the whole.
#define NEEDLE_SPREAD 0.9
// And a capsule fits them where its projected area along each axis is within
// this factor of the axis's coverage.
#define NEEDLE_FIT 2.0

// The point of ray `r` of axis `a` at `depth` (cell units).
static void ray_point(const CellState *st, u32 a, u32 r, f64 depth, f64 out[3])
{
    const u32 i = r % RAYS;
    const u32 j = r / RAYS;
    out[a] = depth;
    out[(a + 1U) % 3U] = ray_u(st, a, i, j);
    out[(a + 2U) % 3U] = ray_w(st, a, i, j);
}

// The capsule of a needle's cell, if its hits are strung along one line:
// its axis's ends (cell units, within the cell) and its radius, where a
// capsule of that radius between them stops about as many of each axis's
// rays as the cell's geometry does (a capsule's projected area along axis a
// is 2 r L sin(a) + pi r^2).
static bool needle_capsule(const CellState *st, const f64 cov[3], f64 out_a[3], f64 out_b[3],
                           f64 *out_radius)
{
    if (st->points < 8.0) {
        return false;
    }
    f64 c[3][3];
    f64 trace = 0.0;
    for (u32 r = 0; r < 3U; ++r) {
        for (u32 q = 0; q < 3U; ++q) {
            c[r][q] = st->point_products[r][q] / st->points -
                      (st->point_sum[r] / st->points) * (st->point_sum[q] / st->points);
        }
        trace += c[r][r];
    }
    f64 line[3];
    const f64 along_spread = dominant_eigen(c, line);
    if (!(trace > 0.0) || along_spread < NEEDLE_SPREAD * trace) {
        return false;
    }
    // The ends: the hits' farthest points along the line, through their
    // centroid (the rays' first and last hits, on the needle's surface).
    f64 centre[3];
    for (u32 a = 0; a < 3U; ++a) {
        centre[a] = st->point_sum[a] / st->points;
    }
    f64 lo = NPH_F64_MAX;
    f64 hi = -NPH_F64_MAX;
    for (u32 a = 0; a < 3U; ++a) {
        for (u32 r = 0; r < AXIS_RAYS; ++r) {
            const Ray *ray = &st->rays[a * AXIS_RAYS + r];
            if (ray->stamp != st->stamp) {
                continue;
            }
            for (u32 e = 0; e < 2U; ++e) {
                f64 q[3];
                ray_point(st, a, r, (f64)(e == 0U ? ray->near_depth : ray->far_depth), q);
                const f64 x = (q[0] - centre[0]) * line[0] + (q[1] - centre[1]) * line[1] +
                              (q[2] - centre[2]) * line[2];
                lo = nph_f64_min(lo, x);
                hi = nph_f64_max(hi, x);
            }
        }
    }
    const f64 length = hi - lo;
    if (!(length > 0.0)) {
        return false;
    }
    const f64 *along = line;
    // The radius: the hits lie on the needle's surface, so their spread
    // across the line is its radius squared (a measure that holds with few
    // hits, where a needle a hundredth of the cell's face across meets two or
    // three of an axis's rays, and its coverage is known only to about 40 %).
    const f64 r = sqrt(nph_f64_max(trace - along_spread, 0.0));
    if (!(r > 0.0)) {
        return false;
    }
    for (u32 a = 0; a < 3U; ++a) {
        const f64 sine = sqrt(nph_f64_max(1.0 - along[a] * along[a], 0.0));
        const f64 area = 2.0 * r * length * sine + NPH_F64_PI * r * r;
        if (area > NEEDLE_FIT * cov[a] && area > 2.0 / (f64)AXIS_RAYS) {
            return false; // more than the needle shows along this axis
        }
        if (area * NEEDLE_FIT < cov[a]) {
            return false;
        }
    }
    for (u32 a = 0; a < 3U; ++a) {
        out_a[a] = nph_f64_clamp(centre[a] + lo * line[a], 0.0, 1.0);
        out_b[a] = nph_f64_clamp(centre[a] + hi * line[a], 0.0, 1.0);
    }
    *out_radius = r;
    return true;
}

// The cell's voxel from its rays.
static void cell_voxel(CellState *st, LodVoxelCell *out)
{
    CellLook look;
    memset(&look, 0, sizeof look);
    look_rays(st, &look, false);
    if (look.count == 0U) {
        look_rays(st, &look, true);
    }
    NPH_ASSERT(look.count != 0U);
    const f64 inv = 1.0 / (f64)look.count;
    NphRenderVoxelDesc d;
    memset(&d, 0, sizeof d);
    d.albedo = nph_vec3((f32)(look.color[0] * inv), (f32)(look.color[1] * inv),
                        (f32)(look.color[2] * inv));
    d.roughness = (f32)(look.roughness * inv);
    d.metallic = (f32)(look.metallic * inv);
    d.occlusion = (f32)(look.occlusion * inv);
    f64 lambda = 1.0 / 3.0;
    if (look.moment_weight > 0.0) {
        f64 m[3][3];
        for (u32 r = 0; r < 3U; ++r) {
            for (u32 c = 0; c < 3U; ++c) {
                m[r][c] = look.moment[r][c] / look.moment_weight;
            }
        }
        f64 axis[3];
        lambda = dominant_eigen(m, axis);
        d.axis = nph_vec3((f32)axis[0], (f32)axis[1], (f32)axis[2]);
        d.kappa = nph_render_voxel_kappa_from_moment((f32)lambda);
    } else {
        d.axis = nph_vec3_unit_z();
        d.kappa = 0.0F;
    }
    // The footprint, in steps of the cell, within the hits' box widened by
    // half a ray's spacing.
    f64 cov[3];
    f64 bl[3];
    f64 bh[3];
    f64 room[3];
    f64 e[3];
    const f64 widen = 0.5 / (f64)RAYS;
    const f64 steps = (f64)NPH_RENDER_VOXEL_BOX_STEPS;
    for (u32 a = 0; a < 3U; ++a) {
        cov[a] = nph_f64_max((f64)st->hits[a] / (f64)AXIS_RAYS, MIN_COVERAGE);
        bl[a] = nph_f64_max(st->bounds_lo[a] - widen, 0.0);
        bh[a] = nph_f64_min(st->bounds_hi[a] + widen, 1.0);
        room[a] = bh[a] - bl[a];
        e[a] = room[a];
    }
    u32 most = 0U;
    for (u32 a = 1U; a < 3U; ++a) {
        if (cov[a] > cov[most]) {
            most = a;
        }
    }
    const bool surface =
        lambda >= SURFACE_MOMENT &&
        room[(most + 1U) % 3U] * room[(most + 2U) % 3U] <= SURFACE_FILL * cov[most];
    f64 end_a[3];
    f64 end_b[3];
    f64 radius = 0.0;
    const bool needle = !surface && needle_capsule(st, cov, end_a, end_b, &radius);
    d.shape = NPH_RENDER_VOXEL_SHAPE_BOX;
    if (needle) {
        // Its ends are its box's opposite corners, the one of lower z first.
        d.shape = NPH_RENDER_VOXEL_SHAPE_CAPSULE;
        d.radius = (f32)radius;
        const bool swap = end_a[2] > end_b[2];
        const f64 *first = swap ? end_b : end_a;
        const f64 *second = swap ? end_a : end_b;
        d.diagonal = (first[0] > second[0] ? 1U : 0U) | (first[1] > second[1] ? 2U : 0U);
        for (u32 a = 0; a < 3U; ++a) {
            bl[a] = nph_f64_min(first[a], second[a]);
            bh[a] = nph_f64_max(first[a], second[a]);
        }
    } else if (!surface) {
        if (footprint_extents(cov, room, 0.25 * NPH_F64_PI, e)) {
            d.shape = NPH_RENDER_VOXEL_SHAPE_ELLIPSOID;
        } else {
            (void)footprint_extents(cov, room, 1.0, e);
        }
    }
    const u64 place = mix64(st->key ^ 0xA5A5A5A55A5A5A5AULL);
    f32 lo[3];
    f32 hi[3];
    for (u32 a = 0; a < 3U; ++a) {
        f64 ql;
        f64 qh;
        if (surface) {
            // The bounds themselves, rounded outward: a surface keeps its reach
            // to the cell's faces, where its neighbour's voxel meets it.
            ql = floor(bl[a] * steps);
            qh = ceil(bh[a] * steps);
        } else if (needle) {
            // The ends, to the nearest step (an axis along which the needle
            // does not move keeps a step's width, its diagonal near the line).
            ql = nph_f64_round(bl[a] * steps);
            qh = nph_f64_round(bh[a] * steps);
            if (qh <= ql) {
                ql = nph_f64_round(0.5 * (bl[a] + bh[a]) * steps - 0.5);
                qh = ql + 1.0;
            }
        } else {
            // To the nearest step: rounding a thin extent of a few steps
            // outward would add a step's worth of coverage to most of them.
            const f64 u = (f64)((place >> (21U * a)) & 0x1FFFFFU) / 2097152.0;
            const f64 at = bl[a] + u * nph_f64_max(room[a] - e[a], 0.0);
            ql = nph_f64_round(at * steps);
            qh = nph_f64_round((at + e[a]) * steps);
        }
        ql = nph_f64_clamp(ql, 0.0, steps - 1.0);
        qh = nph_f64_clamp(qh, ql + 1.0, steps);
        lo[a] = (f32)(ql / steps);
        hi[a] = (f32)(qh / steps);
    }
    d.box_lo = nph_vec3(lo[0], lo[1], lo[2]);
    d.box_hi = nph_vec3(hi[0], hi[1], hi[2]);
    u32 best = 0U;
    for (u32 k = 1U; k < look.submeshes; ++k) {
        if (look.votes[k] > look.votes[best]) {
            best = k;
        }
    }
    out->submesh = look.submesh[best];
    out->voxel = nph_render_voxel_pack(&d);
    key_coords(st->key, out->c);
}

static u32 find_cell(const LodVoxelCellArray *cells, u64 key)
{
    u32 lo = 0U;
    u32 hi = cells->len;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2U;
        const LodVoxelCell *c = &cells->data[mid];
        const u64 k = cell_key(c->c[0], c->c[1], c->c[2]);
        if (k == key) {
            return mid;
        }
        if (k < key) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return NPH_U32_MAX;
}

NphResult lod_voxelize(const LodVoxelInput *in, f64 cell, LodVoxelCellArray *out, u32 *source_cell)
{
    NPH_ASSERT(in != NULL && out != NULL && cell > 0.0 && in->sampler != NULL &&
               in->sampler->sample != NULL);
    NPH_ARRAY_CLEAR(out);
    NphAllocator *a = in->scratch;
    for (u32 s = 0; s < in->source_count; ++s) {
        source_cell[s] = NPH_U32_MAX;
    }
    RefCtx refs;
    NPH_ARRAY_INIT(&refs.refs, a);
    NphResult r = NPH_RESULT_OK;
    for (u32 s = 0; s < in->source_count && r == NPH_RESULT_OK; ++s) {
        r = visit_cells(in, s, cell, ref_visit, &refs);
    }
    if (r == NPH_RESULT_OK && refs.refs.len > 1U) {
        qsort(refs.refs.data, refs.refs.len, sizeof(CellRef), compare_refs);
    }
    Ray *rays = NPH_ALLOC_ARRAY_ZEROED(a, Ray, (u64)AXIS_RAYS * 3U);
    f64 *jitter = NPH_ALLOC_ARRAY(a, f64, (u64)AXIS_RAYS * 2U);
    if (r == NPH_RESULT_OK && (rays == NULL || jitter == NULL)) {
        r = NPH_RESULT_OUT_OF_MEMORY;
    }
    if (jitter != NULL) {
        jitter_table(jitter);
    }
    CellState st;
    memset(&st, 0, sizeof st);
    st.in = in;
    st.cell = cell;
    st.rays = rays;
    st.jitter = jitter;
    for (u32 i = 0; i < refs.refs.len && r == NPH_RESULT_OK;) {
        u32 j = i;
        while (j < refs.refs.len && refs.refs.data[j].key == refs.refs.data[i].key) {
            ++j;
        }
        st.key = refs.refs.data[i].key;
        st.stamp += 1U;
        i32 c[3];
        key_coords(st.key, c);
        st.points = 0.0;
        for (u32 k = 0; k < 3U; ++k) {
            st.lo[k] = (f64)c[k] * cell;
            st.bounds_lo[k] = 1.0;
            st.bounds_hi[k] = 0.0;
            st.hits[k] = 0U;
            st.point_sum[k] = 0.0;
            for (u32 q = 0; q < 3U; ++q) {
                st.point_products[k][q] = 0.0;
            }
        }
        for (u32 ax = 0; ax < 3U; ++ax) {
            st.shift[ax] = (u32)(mix64(st.key ^ ((u64)(ax + 1U) << 61U)) & 0x0F0FU);
        }
        const u32 index = out->len;
        bool any = false;
        for (u32 k = i; k < j; ++k) {
            const u32 s = refs.refs.data[k].source;
            f64 p[3][3];
            // source_corners was true when the ref was made.
            (void)source_corners(in, s, p);
            for (u32 q = 0; q < 3U; ++q) {
                for (u32 ax = 0; ax < 3U; ++ax) {
                    p[q][ax] = (p[q][ax] - st.lo[ax]) / cell;
                }
            }
            bool hit = false;
            for (u32 ax = 0; ax < 3U; ++ax) {
                hit = rasterise(&st, s, p, ax) || hit;
            }
            if (hit && source_cell[s] == NPH_U32_MAX) {
                source_cell[s] = index;
            }
            any = any || hit;
        }
        if (any) {
            LodVoxelCell vc;
            memset(&vc, 0, sizeof vc);
            cell_voxel(&st, &vc);
            r = NPH_ARRAY_PUSH(out, vc);
        }
        i = j;
    }
    // Each source's home: the cell of its centroid where that has a voxel.
    for (u32 s = 0; s < in->source_count && r == NPH_RESULT_OK; ++s) {
        const LodVoxelSource *src = &in->sources[s];
        f64 centroid[3] = {0.0, 0.0, 0.0};
        for (u32 k = 0; k < 3U; ++k) {
            const NphVec3 q = in->positions[src->v[k]];
            centroid[0] += (f64)q.x / 3.0;
            centroid[1] += (f64)q.y / 3.0;
            centroid[2] += (f64)q.z / 3.0;
        }
        const u32 home =
            find_cell(out, cell_key((i64)floor(centroid[0] / cell), (i64)floor(centroid[1] / cell),
                                    (i64)floor(centroid[2] / cell)));
        if (home != NPH_U32_MAX) {
            source_cell[s] = home;
        }
    }
    if (rays != NULL) {
        NPH_FREE_ARRAY(a, Ray, rays, (u64)AXIS_RAYS * 3U);
    }
    if (jitter != NULL) {
        NPH_FREE_ARRAY(a, f64, jitter, (u64)AXIS_RAYS * 2U);
    }
    NPH_ARRAY_FREE(&refs.refs);
    return r;
}
