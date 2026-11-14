// SPDX-License-Identifier: Apache-2.0
// Triangles for the rays alone (ray_trim.h).
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/math/pack.h>

#include <cook/ray_trim.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void cook_ray_trim_init(CookRayTrim *trim, NphAllocator *alloc)
{
    NPH_ASSERT(trim != NULL && alloc != NULL);
    memset(trim, 0, sizeof *trim);
    NPH_ARRAY_INIT(&trim->ranges, alloc);
    NPH_ARRAY_INIT(&trim->triangles, alloc);
}

void cook_ray_trim_free(CookRayTrim *trim)
{
    NPH_ASSERT(trim != NULL);
    NPH_ARRAY_FREE(&trim->ranges);
    NPH_ARRAY_FREE(&trim->triangles);
}

u64 cook_ray_trim_key(const CookRayAlpha *alpha, u32 wrap_u, u32 wrap_v, bool nearest, f32 factor,
                      f32 cutoff)
{
    NPH_ASSERT(alpha != NULL && alpha->texels != NULL);
    u64 h = nph_hash64(alpha->texels, (u64)alpha->width * alpha->height, NPH_HASH_DEFAULT_SEED);
    h = nph_hash_combine(h, ((u64)alpha->width << 32U) | alpha->height);
    h = nph_hash_combine(h, (u64)wrap_u | ((u64)wrap_v << 8U) | ((u64)(nearest ? 1U : 0U) << 16U));
    u32 bits[2];
    memcpy(&bits[0], &factor, sizeof factor);
    memcpy(&bits[1], &cutoff, sizeof cutoff);
    h = nph_hash_combine(h, ((u64)bits[0] << 32U) | bits[1]);
    return h != 0U ? h : 1U;
}

// ---------------------------------------------------------------------------
// The test
// ---------------------------------------------------------------------------

// The texel a lookup at `i` reads of `n` under a wrap mode (Vulkan's address modes).
static u32 wrap_texel(i64 i, u32 n, u32 mode)
{
    const i64 m = (i64)n;
    if (mode == (u32)NPH_MATERIAL_WRAP_CLAMP) {
        return (u32)(i < 0 ? 0 : (i >= m ? m - 1 : i));
    }
    if (mode == (u32)NPH_MATERIAL_WRAP_MIRROR) {
        i64 k = i % (2 * m);
        k = k < 0 ? k + 2 * m : k;
        return (u32)(k < m ? k : 2 * m - 1 - k);
    }
    i64 k = i % m;
    k = k < 0 ? k + m : k;
    return (u32)k;
}

// A bilinear lookup's alpha at (u, v), 0 to 255.
static f64 bilinear_alpha(const CookRayTrimSlot *slot, f64 u, f64 v)
{
    const CookRayAlpha *a = &slot->alpha;
    const f64 x = u * (f64)a->width - 0.5;
    const f64 y = v * (f64)a->height - 0.5;
    const f64 x_floor = floor(x);
    const f64 y_floor = floor(y);
    const f64 fx = x - x_floor;
    const f64 fy = y - y_floor;
    const u32 x0 = wrap_texel((i64)x_floor, a->width, slot->wrap_u);
    const u32 x1 = wrap_texel((i64)x_floor + 1, a->width, slot->wrap_u);
    const u32 y0 = wrap_texel((i64)y_floor, a->height, slot->wrap_v);
    const u32 y1 = wrap_texel((i64)y_floor + 1, a->height, slot->wrap_v);
    const f64 a00 = (f64)a->texels[(u64)y0 * a->width + x0];
    const f64 a10 = (f64)a->texels[(u64)y0 * a->width + x1];
    const f64 a01 = (f64)a->texels[(u64)y1 * a->width + x0];
    const f64 a11 = (f64)a->texels[(u64)y1 * a->width + x1];
    return (a00 * (1.0 - fx) + a10 * fx) * (1.0 - fy) + (a01 * (1.0 - fx) + a11 * fx) * fy;
}

bool cook_ray_trim_passes(const CookRayTrimSlot *slot, f32 u, f32 v, f32 vertex_alpha)
{
    NPH_ASSERT(slot != NULL && slot->alpha.texels != NULL);
    const f64 alpha =
        (f64)slot->factor * (f64)vertex_alpha * (bilinear_alpha(slot, (f64)u, (f64)v) / 255.0);
    return !(alpha < (f64)slot->cutoff); // the shader's test (rt_mask.glsl)
}

// The least texel alpha (0 to 255) the test can pass with, 256 where none can: with a little
// in hand for the shader's float arithmetic.
static u32 least_alpha(f32 vertex_alpha, f32 factor, f32 cutoff)
{
    const f64 top = (f64)factor * ((f64)vertex_alpha * (1.0 + 1e-5) + 1e-6);
    const f64 need = (f64)cutoff * (1.0 - 1e-5) - 1e-7;
    // Every point passes, or the factor or the cutoff is no number (the shader's test then
    // passes everywhere: a NaN is not under a cutoff): nothing is cut.
    if (!(need > 0.0) || top != top) {
        return 0U;
    }
    if (!(top > 0.0)) {
        return 256U;
    }
    const f64 a = ceil(255.0 * need / top - 1e-9);
    return a > 255.0 ? 256U : (a < 0.0 ? 0U : (u32)a);
}

// ---------------------------------------------------------------------------
// A triangle's polygon
// ---------------------------------------------------------------------------

static void polygon_whole(CookRayPolygon *p)
{
    memset(p, 0, sizeof *p);
    p->count = 3U;
    p->corner[1].second = 65535U;
    p->corner[2].third = 65535U;
}

static i64 lattice_cross(CookRayCorner o, CookRayCorner a, CookRayCorner b)
{
    return ((i64)a.second - (i64)o.second) * ((i64)b.third - (i64)o.third) -
           ((i64)a.third - (i64)o.third) * ((i64)b.second - (i64)o.second);
}

// Twice the polygon's area, in the weights' units.
static i64 polygon_area2(const CookRayPolygon *p)
{
    i64 twice = 0;
    for (u32 i = 1; i + 1U < p->count; ++i) {
        twice += lattice_cross(p->corner[0], p->corner[i], p->corner[i + 1U]);
    }
    return twice;
}

static f32 polygon_share(const CookRayPolygon *p)
{
    return (f32)((f64)polygon_area2(p) / (65535.0 * 65535.0));
}

#define HULL_POINTS (COOK_RAY_TRIM_MAX_CORNERS + 8U)

// The convex hull of `n` points of a triangle's lattice (Andrew's monotone chain), into `out`:
// in the triangle's own turn (counter-clockwise in the weights' plane), no three of its corners
// in a line. Returns its corners: under 3 where the points have no area, and
// COOK_RAY_TRIM_MAX_CORNERS + 1 where they are more than a polygon holds.
static u32 lattice_hull(CookRayCorner *pts, u32 n, CookRayPolygon *out)
{
    NPH_ASSERT(n <= HULL_POINTS);
    for (u32 i = 1; i < n; ++i) { // few points: an insertion sort
        const CookRayCorner p = pts[i];
        u32 j = i;
        while (j > 0U && (pts[j - 1U].second > p.second ||
                          (pts[j - 1U].second == p.second && pts[j - 1U].third > p.third))) {
            pts[j] = pts[j - 1U];
            --j;
        }
        pts[j] = p;
    }
    u32 unique = 0U;
    for (u32 i = 0; i < n; ++i) {
        if (unique == 0U || pts[i].second != pts[unique - 1U].second ||
            pts[i].third != pts[unique - 1U].third) {
            pts[unique++] = pts[i];
        }
    }
    if (unique < 3U) {
        return unique;
    }
    CookRayCorner hull[2U * HULL_POINTS];
    u32 k = 0U;
    for (u32 i = 0; i < unique; ++i) {
        while (k >= 2U && lattice_cross(hull[k - 2U], hull[k - 1U], pts[i]) <= 0) {
            --k;
        }
        hull[k++] = pts[i];
    }
    const u32 lower = k + 1U;
    for (u32 i = unique - 1U; i-- > 0U;) {
        while (k >= lower && lattice_cross(hull[k - 2U], hull[k - 1U], pts[i]) <= 0) {
            --k;
        }
        hull[k++] = pts[i];
    }
    --k; // the last is the first again
    if (k < 3U) {
        return k;
    }
    if (k > COOK_RAY_TRIM_MAX_CORNERS) {
        return COOK_RAY_TRIM_MAX_CORNERS + 1U;
    }
    memset(out, 0, sizeof *out);
    out->count = k;
    memcpy(out->corner, hull, sizeof(CookRayCorner) * k);
    return k;
}

typedef struct ClipVertex {
    f64 x;
    f64 y;
    u32 edges; // the triangle's edges it lies on: bit i, the edge across from corner i
} ClipVertex;

static f64 clip_area(const ClipVertex *p, u32 n)
{
    f64 twice = 0.0;
    for (u32 i = 0; i < n; ++i) {
        const ClipVertex *a = &p[i];
        const ClipVertex *b = &p[(i + 1U) % n];
        twice += a->x * b->y - b->x * a->y;
    }
    return fabs(twice) * 0.5;
}

// `in` less what lies beyond d . p <= h (Sutherland and Hodgman, "Reentrant polygon
// clipping", CACM 1974); at most n + 1 corners.
static u32 clip_half_plane(const ClipVertex *in, u32 n, f64 dx, f64 dy, f64 h, ClipVertex *out)
{
    u32 m = 0U;
    for (u32 i = 0; i < n; ++i) {
        const ClipVertex a = in[i];
        const ClipVertex b = in[(i + 1U) % n];
        const f64 sa = dx * a.x + dy * a.y - h;
        const f64 sb = dx * b.x + dy * b.y - h;
        if (sa <= 0.0) {
            out[m++] = a;
        }
        if ((sa <= 0.0) != (sb <= 0.0)) {
            const f64 t = sa / (sa - sb);
            ClipVertex c;
            c.x = a.x + t * (b.x - a.x);
            c.y = a.y + t * (b.y - a.y);
            c.edges = a.edges & b.edges;
            out[m++] = c;
        }
    }
    return m;
}

static const f64 *directions(void)
{
    // Unit vectors, evenly spaced (cos and sin of 2 pi k / 32): a table, so that every
    // platform's cook cuts along the same lines.
    // clang-format off
    static const f64 k_dirs[COOK_RAY_TRIM_DIRECTIONS * 2U] = {
        1.0, 0.0, 0.9807852804032304, 0.19509032201612825,
        0.9238795325112867, 0.3826834323650898, 0.8314696123025452, 0.5555702330196022,
        0.7071067811865476, 0.7071067811865476, 0.5555702330196023, 0.8314696123025452,
        0.38268343236508984, 0.9238795325112867, 0.19509032201612833, 0.9807852804032304,
        0.0, 1.0, -0.1950903220161282, 0.9807852804032304,
        -0.3826834323650897, 0.9238795325112867, -0.555570233019602, 0.8314696123025453,
        -0.7071067811865475, 0.7071067811865476, -0.8314696123025453, 0.5555702330196022,
        -0.9238795325112867, 0.3826834323650899, -0.9807852804032304, 0.1950903220161286,
        -1.0, 0.0, -0.9807852804032304, -0.19509032201612836,
        -0.9238795325112868, -0.38268343236508967, -0.8314696123025455, -0.555570233019602,
        -0.7071067811865477, -0.7071067811865475, -0.5555702330196022, -0.8314696123025452,
        -0.38268343236509034, -0.9238795325112865, -0.19509032201612866, -0.9807852804032303,
        0.0, -1.0, 0.1950903220161283, -0.9807852804032304,
        0.38268343236509, -0.9238795325112866, 0.5555702330196018, -0.8314696123025455,
        0.7071067811865474, -0.7071067811865477, 0.8314696123025452, -0.5555702330196022,
        0.9238795325112865, -0.3826834323650904, 0.9807852804032303, -0.19509032201612872,
    };
    // clang-format on
    return k_dirs;
}

// cook_ray_trim_triangle, adding the texels it walked to `*walked`.
static f32 trim_triangle(const CookRayTrimSlot *slot, const NphVec2 *uv, f32 vertex_alpha,
                         CookRayPolygon *out, u64 *walked)
{
    polygon_whole(out);
    const CookRayAlpha *alpha = &slot->alpha;
    const u32 least = least_alpha(vertex_alpha, slot->factor, slot->cutoff);
    if (least == 0U) {
        return 1.0F;
    }
    if (least > 255U) {
        out->count = 0U;
        return 0.0F;
    }
    // Everything in the texture's texels.
    const f64 w = (f64)alpha->width;
    const f64 h = (f64)alpha->height;
    ClipVertex tri[3];
    for (u32 c = 0; c < 3U; ++c) {
        tri[c].x = (f64)uv[c].x * w;
        tri[c].y = (f64)uv[c].y * h;
        tri[c].edges = 7U & ~(1U << c);
    }
    const f64 e1x = tri[1].x - tri[0].x;
    const f64 e1y = tri[1].y - tri[0].y;
    const f64 e2x = tri[2].x - tri[0].x;
    const f64 e2y = tri[2].y - tri[0].y;
    const f64 det = e1x * e2y - e1y * e2x;
    f64 longest = 0.0;
    f64 lo[2] = {tri[0].x, tri[0].y};
    f64 hi[2] = {tri[0].x, tri[0].y};
    for (u32 c = 0; c < 3U; ++c) {
        const f64 dx = tri[(c + 1U) % 3U].x - tri[c].x;
        const f64 dy = tri[(c + 1U) % 3U].y - tri[c].y;
        const f64 len = sqrt(dx * dx + dy * dy);
        longest = len > longest ? len : longest;
        lo[0] = tri[c].x < lo[0] ? tri[c].x : lo[0];
        hi[0] = tri[c].x > hi[0] ? tri[c].x : hi[0];
        lo[1] = tri[c].y < lo[1] ? tri[c].y : lo[1];
        hi[1] = tri[c].y > hi[1] ? tri[c].y : hi[1];
    }
    // No area in the texture, coordinates that are no numbers, so long that a corner's
    // rounding would outrun the margin, or over more of the texture's repeats than is worth
    // the walk: as it is. (The comparisons are false of a NaN.)
    if (!(fabs(det) > 1e-6) || !(longest < COOK_RAY_TRIM_MAX_TEXELS) || !(fabs(lo[0]) < 1e12) ||
        !(fabs(hi[0]) < 1e12) || !(fabs(lo[1]) < 1e12) || !(fabs(hi[1]) < 1e12) ||
        !(hi[0] - lo[0] <= COOK_RAY_TRIM_MAX_REPEATS * w) ||
        !(hi[1] - lo[1] <= COOK_RAY_TRIM_MAX_REPEATS * h)) {
        return 1.0F;
    }
    // Its edges' inward normals, for the test of a texel's reach against it.
    f64 inward[3][3];
    for (u32 c = 0; c < 3U; ++c) {
        const ClipVertex *a = &tri[c];
        const ClipVertex *b = &tri[(c + 1U) % 3U];
        const ClipVertex *o = &tri[(c + 2U) % 3U];
        f64 nx = -(b->y - a->y);
        f64 ny = b->x - a->x;
        if (nx * (o->x - a->x) + ny * (o->y - a->y) < 0.0) {
            nx = -nx;
            ny = -ny;
        }
        inward[c][0] = nx;
        inward[c][1] = ny;
        inward[c][2] = -(nx * a->x + ny * a->y);
    }
    // How far what can pass reaches in each direction: the texels that can, each with the
    // lookups that read it (half a texel beyond its sides) and the margin.
    const f64 *dirs = directions();
    f64 support[COOK_RAY_TRIM_DIRECTIONS];
    for (u32 k = 0; k < COOK_RAY_TRIM_DIRECTIONS; ++k) {
        support[k] = -1e300;
    }
    const f64 reach = 0.5 + COOK_RAY_TRIM_MARGIN; // beyond a texel's sides
    const f64 half = 0.5 + reach;                 // of its reach's square
    const i64 x0 = (i64)floor(lo[0] - 1.0 - reach);
    const i64 x1 = (i64)floor(hi[0] + reach);
    const i64 y0 = (i64)floor(lo[1] - 1.0 - reach);
    const i64 y1 = (i64)floor(hi[1] + reach);
    *walked += (u64)(x1 - x0 + 1) * (u64)(y1 - y0 + 1);
    bool any = false;
    for (i64 y = y0; y <= y1; ++y) {
        const u8 *row =
            alpha->texels + (u64)wrap_texel(y, alpha->height, slot->wrap_v) * alpha->width;
        const f64 cy = (f64)y + 0.5;
        for (i64 x = x0; x <= x1; ++x) {
            if ((u32)row[wrap_texel(x, alpha->width, slot->wrap_u)] < least) {
                continue;
            }
            const f64 cx = (f64)x + 0.5;
            bool meets = true;
            for (u32 c = 0; c < 3U && meets; ++c) {
                meets = inward[c][0] * cx + inward[c][1] * cy + inward[c][2] +
                            (fabs(inward[c][0]) + fabs(inward[c][1])) * half >=
                        0.0;
            }
            if (!meets) {
                continue;
            }
            any = true;
            for (u32 k = 0; k < COOK_RAY_TRIM_DIRECTIONS; ++k) {
                const f64 dx = dirs[(u64)k * 2U];
                const f64 dy = dirs[(u64)k * 2U + 1U];
                const f64 s = dx * cx + dy * cy + (fabs(dx) + fabs(dy)) * half;
                support[k] = s > support[k] ? s : support[k];
            }
        }
    }
    if (!any) {
        out->count = 0U;
        return 0.0F;
    }
    // The cuts, the one that takes most first.
    ClipVertex poly[COOK_RAY_TRIM_MAX_CORNERS];
    ClipVertex next[COOK_RAY_TRIM_MAX_CORNERS];
    ClipVertex best[COOK_RAY_TRIM_MAX_CORNERS];
    NPH_STATIC_ASSERT(3U + COOK_RAY_TRIM_CUTS + 1U <= COOK_RAY_TRIM_MAX_CORNERS,
                      "the cuts' polygons fit");
    u32 n = 3U;
    memcpy(poly, tri, sizeof tri);
    const f64 whole = clip_area(poly, n);
    f64 area = whole;
    for (u32 cut = 0; cut < COOK_RAY_TRIM_CUTS; ++cut) {
        f64 best_area = area;
        u32 best_n = 0U;
        for (u32 k = 0; k < COOK_RAY_TRIM_DIRECTIONS; ++k) {
            const u32 m = clip_half_plane(poly, n, dirs[(u64)k * 2U], dirs[(u64)k * 2U + 1U],
                                          support[k], next);
            if (m < 3U) {
                continue;
            }
            const f64 a = clip_area(next, m);
            if (a < best_area) {
                best_area = a;
                best_n = m;
                memcpy(best, next, sizeof(ClipVertex) * m);
            }
        }
        if (best_n == 0U || area - best_area < (f64)COOK_RAY_TRIM_MIN_CUT * whole) {
            break;
        }
        n = best_n;
        memcpy(poly, best, sizeof(ClipVertex) * n);
        area = best_area;
    }
    if (n == 3U && area == whole) {
        return 1.0F;
    }
    // Its corners in 65535ths of the triangle; one on an edge of the triangle stays on it.
    CookRayCorner pts[HULL_POINTS];
    for (u32 i = 0; i < n; ++i) {
        const f64 px = poly[i].x - tri[0].x;
        const f64 py = poly[i].y - tri[0].y;
        const f64 b1 = (px * e2y - py * e2x) / det;
        const f64 b2 = (e1x * py - e1y * px) / det;
        i64 q1 = (i64)floor(b1 * 65535.0 + 0.5);
        i64 q2 = (i64)floor(b2 * 65535.0 + 0.5);
        q1 = q1 < 0 ? 0 : (q1 > 65535 ? 65535 : q1);
        q2 = q2 < 0 ? 0 : (q2 > 65535 ? 65535 : q2);
        const u32 e = poly[i].edges;
        if ((e & 2U) != 0U) {
            q1 = 0;
        }
        if ((e & 4U) != 0U) {
            q2 = 0;
        }
        if ((e & 1U) != 0U) {
            if ((e & 4U) != 0U) {
                q1 = 65535;
            } else {
                q2 = 65535 - q1;
            }
        } else if (q1 + q2 > 65535) {
            q2 = 65535 - q1;
        }
        pts[i].second = (u16)q1;
        pts[i].third = (u16)q2;
    }
    CookRayPolygon hull;
    const u32 corners = lattice_hull(pts, n, &hull);
    if (corners < 3U || corners > COOK_RAY_TRIM_MAX_CORNERS) {
        return 1.0F; // no area left to round to: as it is
    }
    *out = hull;
    return polygon_share(out);
}

f32 cook_ray_trim_triangle(const CookRayTrimSlot *slot, const NphVec2 *uv, f32 vertex_alpha,
                           CookRayPolygon *out)
{
    NPH_ASSERT(slot != NULL && uv != NULL && out != NULL && slot->alpha.texels != NULL);
    u64 walked = 0U;
    return trim_triangle(slot, uv, vertex_alpha, out, &walked);
}

// ---------------------------------------------------------------------------
// Shared edges
// ---------------------------------------------------------------------------

// An edge of a trimmed triangle: its ends' positions in order, the triangle's polygon, the
// edge (across from which of the triangle's corners), and whether the greater end is the
// edge's second (corner (edge + 2) % 3) or its first.
typedef struct TrimEdge {
    u64 lo;
    u64 hi;
    u32 polygon;
    u8 edge;
    u8 hi_is_second;
} TrimEdge;

static int edge_compare(const void *a, const void *b)
{
    const TrimEdge *x = (const TrimEdge *)a;
    const TrimEdge *y = (const TrimEdge *)b;
    if (x->lo != y->lo) {
        return x->lo < y->lo ? -1 : 1;
    }
    if (x->hi != y->hi) {
        return x->hi < y->hi ? -1 : 1;
    }
    return x->polygon < y->polygon ? -1 : (x->polygon > y->polygon ? 1 : 0);
}

static u64 position_key(NphMeshPosition p)
{
    return (u64)p.x | ((u64)p.y << 16U) | ((u64)p.z << 32U);
}

// A corner's weight of each of its triangle's corners, in 65535ths.
static void corner_weights(CookRayCorner c, u32 w[3])
{
    w[0] = 65535U - c.second - c.third;
    w[1] = c.second;
    w[2] = c.third;
}

// Where the polygon meets edge `e`, as the greater end's weights: false where it does not.
static bool edge_reach(const CookRayPolygon *p, const TrimEdge *e, u32 *out_lo, u32 *out_hi)
{
    u32 lo = 65536U;
    u32 hi = 0U;
    for (u32 i = 0; i < p->count; ++i) {
        u32 w[3];
        corner_weights(p->corner[i], w);
        if (w[e->edge] != 0U) {
            continue;
        }
        const u32 t = w[(e->edge + (e->hi_is_second ? 2U : 1U)) % 3U];
        lo = t < lo ? t : lo;
        hi = t > hi ? t : hi;
    }
    *out_lo = lo;
    *out_hi = hi;
    return lo <= hi;
}

// The point of edge `e` where the greater end's weight is `t`.
static CookRayCorner edge_point(const TrimEdge *e, u32 t)
{
    u32 w[3] = {0U, 0U, 0U};
    const u32 first = (e->edge + 1U) % 3U;
    const u32 second = (e->edge + 2U) % 3U;
    w[e->hi_is_second ? second : first] = t;
    w[e->hi_is_second ? first : second] = 65535U - t;
    CookRayCorner c;
    c.second = (u16)w[1];
    c.third = (u16)w[2];
    return c;
}

// The polygon grown to meet edge `e` from `lo` to `hi`: the hull of it and those two points
// (the whole triangle where that is more than a polygon holds).
static void polygon_reach(CookRayPolygon *p, const TrimEdge *e, u32 lo, u32 hi)
{
    CookRayCorner pts[HULL_POINTS];
    memcpy(pts, p->corner, sizeof(CookRayCorner) * p->count);
    u32 n = p->count;
    pts[n++] = edge_point(e, lo);
    pts[n++] = edge_point(e, hi);
    CookRayPolygon grown;
    const u32 corners = lattice_hull(pts, n, &grown);
    if (corners < 3U || corners > COOK_RAY_TRIM_MAX_CORNERS) {
        polygon_whole(p);
    } else {
        *p = grown;
    }
}

// Makes the polygons of triangles that share an edge meet it over the same stretch, where two
// or more reach it: each grows to the stretch they reach together, pass after pass until none
// grows (a polygon grown to a corner of its triangle reaches its other edge there too). Their
// corners on the edge are then the same points of it, which the renderer places from the
// edge's ends alone, so the polygons share whole edges and no ray passes between them.
static u32 unify_edges(TrimEdge *edges, u32 edge_count, CookRayPolygon *polys)
{
    qsort(edges, edge_count, sizeof(TrimEdge), edge_compare);
    u32 passes = 0U;
    bool changed = true;
    while (changed) {
        changed = false;
        ++passes;
        for (u32 i = 0; i < edge_count;) {
            u32 j = i + 1U;
            while (j < edge_count && edges[j].lo == edges[i].lo && edges[j].hi == edges[i].hi) {
                ++j;
            }
            u32 lo = 65536U;
            u32 hi = 0U;
            u32 reaching = 0U;
            for (u32 k = i; k < j && j - i >= 2U; ++k) {
                u32 a = 0U;
                u32 b = 0U;
                if (edge_reach(&polys[edges[k].polygon], &edges[k], &a, &b)) {
                    lo = a < lo ? a : lo;
                    hi = b > hi ? b : hi;
                    ++reaching;
                }
            }
            for (u32 k = i; k < j && reaching >= 2U; ++k) {
                u32 a = 0U;
                u32 b = 0U;
                if (edge_reach(&polys[edges[k].polygon], &edges[k], &a, &b) &&
                    (a != lo || b != hi)) {
                    polygon_reach(&polys[edges[k].polygon], &edges[k], lo, hi);
                    changed = true;
                }
            }
            i = j;
        }
    }
    return passes;
}

// ---------------------------------------------------------------------------
// A mesh
// ---------------------------------------------------------------------------

// What the test can tell triangles apart by: their corners' texture coordinates and alphas.
typedef struct TrimKey {
    u32 uv[3];
    u32 alpha;
    u32 triangle;
} TrimKey;

static int key_compare(const void *a, const void *b)
{
    const TrimKey *x = (const TrimKey *)a;
    const TrimKey *y = (const TrimKey *)b;
    const int c = memcmp(x, y, sizeof(u32) * 4U);
    return c != 0 ? c : (x->triangle < y->triangle ? -1 : (x->triangle > y->triangle ? 1 : 0));
}

// A submesh to trim: its polygons' first (a triangle each, in order).
typedef struct TrimRun {
    u32 submesh;
    u32 first_polygon;
} TrimRun;

typedef NPH_ARRAY(CookRayPolygon) PolygonArray;
typedef NPH_ARRAY(TrimRun) RunArray;

static f64 triangle_area(const NphMeshPosition *positions, const u32 *idx, NphVec3 scale)
{
    f64 corner[3][3];
    for (u32 c = 0; c < 3U; ++c) {
        const NphMeshPosition q = positions[idx[c]];
        corner[c][0] = (f64)q.x / 65535.0 * (f64)scale.x;
        corner[c][1] = (f64)q.y / 65535.0 * (f64)scale.y;
        corner[c][2] = (f64)q.z / 65535.0 * (f64)scale.z;
    }
    f64 e1[3];
    f64 e2[3];
    for (u32 k = 0; k < 3U; ++k) {
        e1[k] = corner[1][k] - corner[0][k];
        e2[k] = corner[2][k] - corner[0][k];
    }
    const f64 cx = e1[1] * e2[2] - e1[2] * e2[1];
    const f64 cy = e1[2] * e2[0] - e1[0] * e2[2];
    const f64 cz = e1[0] * e2[1] - e1[1] * e2[0];
    return 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
}

// A submesh's triangles' polygons, appended to `polys` where trimming it pays: the triangles
// the test cannot tell apart worked out once. `*out_pays` says whether they were.
static NphResult submesh_polygons(NphAllocator *alloc, const NphMeshPosition *positions,
                                  const NphMeshAttrib *attribs, const u32 *indices,
                                  const NphSubmesh *sub, NphVec3 scale, const CookRayTrimSlot *slot,
                                  PolygonArray *polys, CookRayTrimStats *stats, bool *out_pays)
{
    *out_pays = false;
    const u32 tris = sub->index_count / 3U;
    TrimKey *keys = NPH_ALLOC_ARRAY(alloc, TrimKey, tris);
    u32 *polygon_of = NPH_ALLOC_ARRAY(alloc, u32, tris);
    PolygonArray distinct;
    NPH_ARRAY_INIT(&distinct, alloc);
    NphResult r = keys != NULL && polygon_of != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 t = 0; t < tris; ++t) {
            const u32 *idx = indices + sub->index_offset + (u64)t * 3U;
            u32 alpha = 0U;
            for (u32 c = 0; c < 3U; ++c) {
                keys[t].uv[c] = attribs[idx[c]].uv0;
                alpha |= (attribs[idx[c]].color >> 24U) << (8U * c);
            }
            keys[t].alpha = alpha;
            keys[t].triangle = t;
        }
        qsort(keys, tris, sizeof(TrimKey), key_compare);
    }
    f64 area = 0.0;
    f64 area_kept = 0.0;
    u32 kept_triangles = 0U;
    f32 share = 1.0F;
    u64 walked = 0U;
    for (u32 i = 0; r == NPH_RESULT_OK && i < tris && walked <= COOK_RAY_TRIM_MAX_WALK; ++i) {
        if (i == 0U || memcmp(&keys[i], &keys[i - 1U], sizeof(u32) * 4U) != 0) {
            NphVec2 uv[3];
            u32 most = 0U;
            for (u32 c = 0; c < 3U; ++c) {
                uv[c] = nph_unpack_f16x2(keys[i].uv[c]);
                const u32 a = (keys[i].alpha >> (8U * c)) & 0xFFU;
                most = a > most ? a : most;
            }
            CookRayPolygon *p = NPH_ARRAY_PUSH_SLOT(&distinct);
            if (p == NULL) {
                r = NPH_RESULT_OUT_OF_MEMORY;
                break;
            }
            share = trim_triangle(slot, uv, (f32)most / 255.0F, p, &walked);
        }
        const u32 t = keys[i].triangle;
        polygon_of[t] = distinct.len - 1U;
        const f64 a = triangle_area(positions, indices + sub->index_offset + (u64)t * 3U, scale);
        area += a;
        area_kept += a * (f64)share;
        kept_triangles += distinct.data[distinct.len - 1U].count != 0U ? 1U : 0U;
    }
    // It pays where enough of its area goes; a submesh no ray can pass the test in anywhere
    // keeps its triangles (a geometry has some), and so does one whose walk ran out.
    if (r == NPH_RESULT_OK && walked <= COOK_RAY_TRIM_MAX_WALK && kept_triangles != 0U &&
        area > 0.0 && 1.0 - area_kept / area >= (f64)COOK_RAY_TRIM_MIN_SHARE) {
        const u32 first = polys->len;
        r = NPH_ARRAY_RESIZE(polys, first + tris);
        for (u32 t = 0; r == NPH_RESULT_OK && t < tris; ++t) {
            polys->data[first + t] = distinct.data[polygon_of[t]];
        }
        if (r == NPH_RESULT_OK) {
            *out_pays = true;
            stats->distinct += distinct.len;
        }
    }
    NPH_ARRAY_FREE(&distinct);
    if (keys != NULL) {
        NPH_FREE_ARRAY(alloc, TrimKey, keys, tris);
    }
    if (polygon_of != NULL) {
        NPH_FREE_ARRAY(alloc, u32, polygon_of, tris);
    }
    return r;
}

NphResult cook_ray_trim_build(NphAllocator *alloc, const NphMeshPosition *positions,
                              const NphMeshAttrib *attribs, u32 vertex_count, const u32 *indices,
                              u32 index_count, const NphSubmesh *submeshes, u32 submesh_count,
                              NphVec3 position_scale, const CookRayTrimSlots *slots,
                              CookRayTrim *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    NPH_ARRAY_CLEAR(&out->ranges);
    NPH_ARRAY_CLEAR(&out->triangles);
    memset(&out->stats, 0, sizeof out->stats);
    if (slots == NULL || slots->count == 0U || submesh_count == 0U || index_count == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_ASSERT(positions != NULL && attribs != NULL && indices != NULL && submeshes != NULL);
    PolygonArray polys;
    RunArray runs;
    NPH_ARRAY_INIT(&polys, alloc);
    NPH_ARRAY_INIT(&runs, alloc);
    NphResult r = NPH_RESULT_OK;
    for (u32 s = 0; s < submesh_count && r == NPH_RESULT_OK; ++s) {
        const NphSubmesh *sub = &submeshes[s];
        const CookRayTrimSlot *slot =
            sub->material_slot < slots->count ? &slots->slots[sub->material_slot] : NULL;
        if (slot == NULL || slot->key == 0U || slot->alpha.texels == NULL ||
            slot->alpha.width == 0U || slot->alpha.height == 0U || sub->index_count < 3U) {
            continue;
        }
        if (sub->index_count % 3U != 0U || sub->index_offset > index_count ||
            sub->index_count > index_count - sub->index_offset) {
            r = NPH_RESULT_INVALID_ARGUMENT;
            break;
        }
        for (u32 i = 0; i < sub->index_count; ++i) {
            if (indices[sub->index_offset + i] >= vertex_count) {
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
        }
        bool pays = false;
        const u32 first = polys.len;
        if (r == NPH_RESULT_OK) {
            r = submesh_polygons(alloc, positions, attribs, indices, sub, position_scale, slot,
                                 &polys, &out->stats, &pays);
        }
        if (r == NPH_RESULT_OK && pays) {
            TrimRun *run = NPH_ARRAY_PUSH_SLOT(&runs);
            if (run == NULL) {
                r = NPH_RESULT_OUT_OF_MEMORY;
            } else {
                run->submesh = s;
                run->first_polygon = first;
            }
        } else if (r == NPH_RESULT_OK) {
            out->stats.left += 1U;
        }
    }
    // The shared edges of every trimmed triangle that has a polygon.
    TrimEdge *edges = NULL;
    const u64 edge_cap = (u64)polys.len * 3U;
    u32 edge_count = 0U;
    if (r == NPH_RESULT_OK && runs.len != 0U) {
        edges = NPH_ALLOC_ARRAY(alloc, TrimEdge, edge_cap);
        r = edges != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 k = 0; k < runs.len && r == NPH_RESULT_OK; ++k) {
        const NphSubmesh *sub = &submeshes[runs.data[k].submesh];
        for (u32 t = 0; t < sub->index_count / 3U; ++t) {
            const u32 polygon = runs.data[k].first_polygon + t;
            if (polys.data[polygon].count == 0U) {
                continue;
            }
            const u32 *idx = indices + sub->index_offset + (u64)t * 3U;
            for (u32 e = 0; e < 3U; ++e) {
                const u64 a = position_key(positions[idx[(e + 1U) % 3U]]);
                const u64 b = position_key(positions[idx[(e + 2U) % 3U]]);
                if (a == b) {
                    continue; // no edge
                }
                TrimEdge *edge = &edges[edge_count++];
                edge->lo = a < b ? a : b;
                edge->hi = a < b ? b : a;
                edge->polygon = polygon;
                edge->edge = (u8)e;
                edge->hi_is_second = (u8)(b > a ? 1U : 0U);
            }
        }
    }
    if (r == NPH_RESULT_OK && edge_count != 0U) {
        out->stats.edge_passes = unify_edges(edges, edge_count, polys.data);
    }
    if (edges != NULL) {
        NPH_FREE_ARRAY(alloc, TrimEdge, edges, edge_cap);
    }
    // The polygons as fans, a submesh's after the one's before.
    if (r == NPH_RESULT_OK && runs.len != 0U) {
        r = NPH_ARRAY_RESIZE(&out->ranges, submesh_count);
        if (r == NPH_RESULT_OK) {
            memset(out->ranges.data, 0, NPH_ARRAY_BYTES(&out->ranges));
        }
    }
    for (u32 k = 0; k < runs.len && r == NPH_RESULT_OK; ++k) {
        const u32 s = runs.data[k].submesh;
        const NphSubmesh *sub = &submeshes[s];
        const CookRayTrimSlot *slot = &slots->slots[sub->material_slot];
        const u32 first = out->triangles.len;
        for (u32 t = 0; t < sub->index_count / 3U && r == NPH_RESULT_OK; ++t) {
            const CookRayPolygon *p = &polys.data[runs.data[k].first_polygon + t];
            const u32 *idx = indices + sub->index_offset + (u64)t * 3U;
            const f64 a = triangle_area(positions, idx, position_scale);
            out->stats.area += a;
            if (p->count == 0U) {
                out->stats.dropped += 1U;
                continue;
            }
            out->stats.area_kept += a * (f64)polygon_share(p);
            for (u32 c = 1; c + 1U < p->count && r == NPH_RESULT_OK; ++c) {
                NphMeshRayTriangle *rt = NPH_ARRAY_PUSH_SLOT(&out->triangles);
                if (rt == NULL) {
                    r = NPH_RESULT_OUT_OF_MEMORY;
                    break;
                }
                rt->parent = sub->index_offset + t * 3U;
                rt->corner[0] = (u32)p->corner[0].second | ((u32)p->corner[0].third << 16U);
                rt->corner[1] = (u32)p->corner[c].second | ((u32)p->corner[c].third << 16U);
                rt->corner[2] =
                    (u32)p->corner[c + 1U].second | ((u32)p->corner[c + 1U].third << 16U);
            }
        }
        if (r == NPH_RESULT_OK) {
            out->ranges.data[s].first = first;
            out->ranges.data[s].count = out->triangles.len - first;
            out->ranges.data[s].alpha_key = slot->key;
            out->stats.submeshes += 1U;
            out->stats.parents += sub->index_count / 3U;
        }
    }
    NPH_ARRAY_FREE(&polys);
    NPH_ARRAY_FREE(&runs);
    if (r != NPH_RESULT_OK) {
        NPH_ARRAY_CLEAR(&out->ranges);
        NPH_ARRAY_CLEAR(&out->triangles);
    }
    return r;
}
