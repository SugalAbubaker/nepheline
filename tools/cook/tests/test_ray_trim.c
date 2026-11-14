// SPDX-License-Identifier: Apache-2.0
// Triangles for the rays alone (cook/ray_trim.h; ADR-0096): the test's bound against the test
// itself at random points, the polygons a card is cut to, and a mesh's cards sharing their
// edges' points.
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>

#include "test_cook_support.h"

#include <cook/ray_trim.h>

static f64 unit(u64 *rng)
{
    return (f64)(test_rng_next(rng) >> 11U) / 9007199254740992.0;
}

static CookRayTrimSlot slot_of(const u8 *texels, u32 w, u32 h, u32 wrap, f32 factor, f32 cutoff)
{
    CookRayTrimSlot s;
    memset(&s, 0, sizeof s);
    s.alpha.texels = texels;
    s.alpha.width = w;
    s.alpha.height = h;
    s.wrap_u = wrap;
    s.wrap_v = wrap;
    s.factor = factor;
    s.cutoff = cutoff;
    s.key = cook_ray_trim_key(&s.alpha, wrap, wrap, false, factor, cutoff);
    return s;
}

// A disc of full alpha, `radius` texels about (`cx`, `cy`), its rim a texel soft, added to
// `texels`.
static void add_disc(u8 *texels, u32 w, u32 h, f64 cx, f64 cy, f64 radius)
{
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f64 dx = (f64)x + 0.5 - cx;
            const f64 dy = (f64)y + 0.5 - cy;
            const f64 a = nph_f64_clamp(radius + 0.5 - sqrt(dx * dx + dy * dy), 0.0, 1.0) * 255.0;
            const u8 v = (u8)a;
            texels[(u64)y * w + x] = v > texels[(u64)y * w + x] ? v : texels[(u64)y * w + x];
        }
    }
}

// Whether the point of a triangle with weights `b1` and `b2` of its second and third corners
// lies in the polygon (its corners turn counter-clockwise in the weights' plane).
static bool polygon_holds(const CookRayPolygon *p, f64 b1, f64 b2)
{
    if (p->count < 3U) {
        return false;
    }
    const f64 px = b1 * 65535.0;
    const f64 py = b2 * 65535.0;
    for (u32 i = 0; i < p->count; ++i) {
        const CookRayCorner a = p->corner[i];
        const CookRayCorner b = p->corner[(i + 1U) % p->count];
        const f64 cross = ((f64)b.second - (f64)a.second) * (py - (f64)a.third) -
                          ((f64)b.third - (f64)a.third) * (px - (f64)a.second);
        if (cross < 0.0) {
            return false;
        }
    }
    return true;
}

// Random points of the triangle whose test passes: how many, and how many of those the
// polygon does not hold (none may be).
static u32 points_outside(const CookRayTrimSlot *slot, const NphVec2 *uv, f32 vertex_alpha,
                          const CookRayPolygon *p, u32 points, u64 *rng, u32 *out_passed)
{
    u32 outside = 0U;
    for (u32 i = 0; i < points; ++i) {
        f64 b1 = unit(rng);
        f64 b2 = unit(rng);
        if (b1 + b2 > 1.0) {
            b1 = 1.0 - b1;
            b2 = 1.0 - b2;
        }
        const f64 b0 = 1.0 - b1 - b2;
        const f32 u = (f32)((f64)uv[0].x * b0 + (f64)uv[1].x * b1 + (f64)uv[2].x * b2);
        const f32 v = (f32)((f64)uv[0].y * b0 + (f64)uv[1].y * b1 + (f64)uv[2].y * b2);
        if (!cook_ray_trim_passes(slot, u, v, vertex_alpha)) {
            continue;
        }
        *out_passed += 1U;
        outside += polygon_holds(p, b1, b2) ? 0U : 1U;
    }
    return outside;
}

NTEST(CookRayTrim, a_key_tells_alpha_tests_apart_and_is_never_zero)
{
    u8 a[16];
    u8 b[16];
    memset(a, 200, sizeof a);
    memset(b, 200, sizeof b);
    b[5] = 201U;
    const CookRayAlpha ia = {a, 4U, 4U};
    const CookRayAlpha ib = {b, 4U, 4U};
    const CookRayAlpha wide = {a, 8U, 2U};
    const u32 rep = (u32)NPH_MATERIAL_WRAP_REPEAT;
    const u32 clamp = (u32)NPH_MATERIAL_WRAP_CLAMP;
    const u64 k = cook_ray_trim_key(&ia, rep, rep, false, 1.0F, 0.5F);
    NTEST_EXPECT_TRUE(k != 0U);
    NTEST_EXPECT_TRUE(k == cook_ray_trim_key(&ia, rep, rep, false, 1.0F, 0.5F));
    NTEST_EXPECT_TRUE(k != cook_ray_trim_key(&ib, rep, rep, false, 1.0F, 0.5F));
    NTEST_EXPECT_TRUE(k != cook_ray_trim_key(&wide, rep, rep, false, 1.0F, 0.5F));
    NTEST_EXPECT_TRUE(k != cook_ray_trim_key(&ia, clamp, rep, false, 1.0F, 0.5F));
    NTEST_EXPECT_TRUE(k != cook_ray_trim_key(&ia, rep, clamp, false, 1.0F, 0.5F));
    NTEST_EXPECT_TRUE(k != cook_ray_trim_key(&ia, rep, rep, true, 1.0F, 0.5F));
    NTEST_EXPECT_TRUE(k != cook_ray_trim_key(&ia, rep, rep, false, 0.9F, 0.5F));
    NTEST_EXPECT_TRUE(k != cook_ray_trim_key(&ia, rep, rep, false, 1.0F, 0.4F));
}

NTEST(CookRayTrim, the_test_is_a_bilinear_lookup_under_each_wrap_mode)
{
    // Two texels: clear, then solid.
    const u8 texels[2] = {0U, 255U};
    CookRayTrimSlot s = slot_of(texels, 2U, 1U, (u32)NPH_MATERIAL_WRAP_REPEAT, 1.0F, 0.5F);
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, 0.25F, 0.5F, 1.0F)); // the clear texel's centre
    NTEST_EXPECT_TRUE(cook_ray_trim_passes(&s, 0.75F, 0.5F, 1.0F));
    NTEST_EXPECT_TRUE(cook_ray_trim_passes(&s, 0.5F, 0.5F, 1.0F)); // half way: not under
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, 0.49F, 0.5F, 1.0F));
    // The vertices' alpha and the factor scale it.
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, 0.75F, 0.5F, 0.4F));
    s.factor = 0.4F;
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, 0.75F, 0.5F, 1.0F));
    s.factor = 1.0F;
    // A quarter past the edge: repeat reads the clear texel, clamp holds the solid one, and
    // mirror folds back to it.
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, 1.25F, 0.5F, 1.0F));
    s.wrap_u = (u32)NPH_MATERIAL_WRAP_CLAMP;
    NTEST_EXPECT_TRUE(cook_ray_trim_passes(&s, 1.25F, 0.5F, 1.0F));
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, -3.0F, 0.5F, 1.0F));
    s.wrap_u = (u32)NPH_MATERIAL_WRAP_MIRROR;
    NTEST_EXPECT_TRUE(cook_ray_trim_passes(&s, 1.25F, 0.5F, 1.0F));
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, 1.75F, 0.5F, 1.0F));
    NTEST_EXPECT_FALSE(cook_ray_trim_passes(&s, -0.25F, 0.5F, 1.0F));
}

NTEST(CookRayTrim, a_triangle_that_cannot_be_cut_stays_whole)
{
    u8 solid[64];
    u8 half[64];
    memset(solid, 255, sizeof solid);
    memset(half, 0, sizeof half);
    memset(half, 255, 32U);
    const NphVec2 uv[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(1.0F, 0.0F), nph_vec2(0.0F, 1.0F)};
    CookRayPolygon p;
    // Nothing is cut away.
    CookRayTrimSlot s = slot_of(solid, 8U, 8U, (u32)NPH_MATERIAL_WRAP_REPEAT, 1.0F, 0.5F);
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, uv, 1.0F, &p), 1.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(p.count, 3U);
    NTEST_EXPECT_EQ_UINT(p.corner[0].second + p.corner[0].third, 0U);
    NTEST_EXPECT_EQ_UINT(p.corner[1].second, 65535U);
    NTEST_EXPECT_EQ_UINT(p.corner[2].third, 65535U);
    // A cutoff nothing is under, and a factor or a cutoff that is no number (the shader's
    // test then passes everywhere).
    s = slot_of(half, 8U, 8U, (u32)NPH_MATERIAL_WRAP_REPEAT, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, uv, 1.0F, &p), 1.0F, 0.0F);
    s = slot_of(half, 8U, 8U, (u32)NPH_MATERIAL_WRAP_REPEAT, nph_f32_from_bits(0x7FC00000U), 0.5F);
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, uv, 1.0F, &p), 1.0F, 0.0F);
    s = slot_of(half, 8U, 8U, (u32)NPH_MATERIAL_WRAP_REPEAT, 1.0F, nph_f32_from_bits(0x7FC00000U));
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, uv, 1.0F, &p), 1.0F, 0.0F);
    // No area in the texture, coordinates that are no numbers, and a triangle too long in it
    // for its corners' rounding.
    s = slot_of(half, 8U, 8U, (u32)NPH_MATERIAL_WRAP_REPEAT, 1.0F, 0.5F);
    const NphVec2 line[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(0.5F, 0.5F), nph_vec2(1.0F, 1.0F)};
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, line, 1.0F, &p), 1.0F, 0.0F);
    const NphVec2 nan[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(nph_f32_from_bits(0x7FC00000U), 0.0F),
                            nph_vec2(0.0F, 1.0F)};
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, nan, 1.0F, &p), 1.0F, 0.0F);
    const NphVec2 far[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(2000.0F, 0.0F), nph_vec2(0.0F, 1.0F)};
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, far, 1.0F, &p), 1.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(p.count, 3U);
    // And one over more of the texture's repeats than is worth the walk; two it takes.
    const NphVec2 wide[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(2.5F, 0.0F), nph_vec2(0.0F, 1.0F)};
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, wide, 1.0F, &p), 1.0F, 0.0F);
    // (Its tip stays short of the texture's top, where the lookup wraps to the solid rows.)
    const NphVec2 two[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(2.0F, 0.0F), nph_vec2(0.0F, 0.875F)};
    NTEST_EXPECT_TRUE(cook_ray_trim_triangle(&s, two, 1.0F, &p) < 1.0F);
}

NTEST(CookRayTrim, a_triangle_nothing_passes_in_has_no_polygon)
{
    u8 clear[64];
    u8 solid[64];
    memset(clear, 0, sizeof clear);
    memset(solid, 255, sizeof solid);
    const NphVec2 uv[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(1.0F, 0.0F), nph_vec2(0.0F, 1.0F)};
    CookRayPolygon p;
    CookRayTrimSlot s = slot_of(clear, 8U, 8U, (u32)NPH_MATERIAL_WRAP_REPEAT, 1.0F, 0.5F);
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, uv, 1.0F, &p), 0.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(p.count, 0U);
    // Solid texels under vertices too clear to pass with them.
    s = slot_of(solid, 8U, 8U, (u32)NPH_MATERIAL_WRAP_REPEAT, 1.0F, 0.5F);
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, uv, 0.25F, &p), 0.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(p.count, 0U);
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, uv, 0.0F, &p), 0.0F, 0.0F);
    // A leaf elsewhere in the texture than the triangle.
    u8 corner[64 * 64];
    memset(corner, 0, sizeof corner);
    add_disc(corner, 64U, 64U, 52.0, 52.0, 6.0);
    s = slot_of(corner, 64U, 64U, (u32)NPH_MATERIAL_WRAP_CLAMP, 1.0F, 0.5F);
    const NphVec2 away[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(0.6F, 0.0F), nph_vec2(0.0F, 0.6F)};
    NTEST_EXPECT_NEAR_F32(cook_ray_trim_triangle(&s, away, 1.0F, &p), 0.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(p.count, 0U);
}

NTEST(CookRayTrim, a_leaf_is_cut_out_of_its_card)
{
    // A disc of 12 texels in a texture of 64, on a triangle that holds the whole texture
    // twice over: about a twentieth of the triangle.
    u8 texels[64 * 64];
    memset(texels, 0, sizeof texels);
    add_disc(texels, 64U, 64U, 32.0, 32.0, 12.0);
    const CookRayTrimSlot s = slot_of(texels, 64U, 64U, (u32)NPH_MATERIAL_WRAP_CLAMP, 1.0F, 0.5F);
    const NphVec2 uv[3] = {nph_vec2(0.0F, 0.0F), nph_vec2(2.0F, 0.0F), nph_vec2(0.0F, 2.0F)};
    CookRayPolygon p;
    const f32 share = cook_ray_trim_triangle(&s, uv, 1.0F, &p);
    NTEST_EXPECT_TRUE(share > 0.055F && share < 0.14F);
    NTEST_EXPECT_GE_UINT(p.count, 4U);
    NTEST_EXPECT_LE_UINT(p.count, 3U + COOK_RAY_TRIM_CUTS);
    u64 rng = 0x1234ULL;
    u32 passed = 0U;
    NTEST_EXPECT_EQ_UINT(points_outside(&s, uv, 1.0F, &p, 40000U, &rng, &passed), 0U);
    NTEST_EXPECT_GT_UINT(passed, 1000U);
    // The polygon keeps its triangle's turn, and no three of its corners lie in a line.
    for (u32 i = 0; i < p.count; ++i) {
        const CookRayCorner a = p.corner[i];
        const CookRayCorner b = p.corner[(i + 1U) % p.count];
        const CookRayCorner c = p.corner[(i + 2U) % p.count];
        const i64 turn = ((i64)b.second - a.second) * ((i64)c.third - a.third) -
                         ((i64)b.third - a.third) * ((i64)c.second - a.second);
        NTEST_EXPECT_TRUE(turn > 0);
        NTEST_EXPECT_LE_UINT((u32)a.second + a.third, 65535U);
    }
    // Dimmer vertices pass less, and the polygon is no larger for them.
    CookRayPolygon dim;
    const f32 dim_share = cook_ray_trim_triangle(&s, uv, 0.7F, &dim);
    NTEST_EXPECT_TRUE(dim_share <= share);
    passed = 0U;
    NTEST_EXPECT_EQ_UINT(points_outside(&s, uv, 0.7F, &dim, 20000U, &rng, &passed), 0U);
}

NTEST(CookRayTrim, no_point_that_passes_lies_outside_its_polygon)
{
    // Random leaves, triangles, wraps, cutoffs and vertex alphas: the bound against the test.
    u8 texels[64 * 64];
    u32 trimmed = 0U;
    u32 dropped = 0U;
    u32 passed = 0U;
    for (u64 seed = 1; seed <= 400U; ++seed) {
        u64 rng = seed * 0x9E3779B97F4A7C15ULL;
        const u32 w = 4U + (u32)(unit(&rng) * 61.0);
        const u32 h = 4U + (u32)(unit(&rng) * 61.0);
        memset(texels, 0, sizeof texels);
        const u32 discs = 1U + (u32)(unit(&rng) * 4.0);
        for (u32 d = 0; d < discs; ++d) {
            add_disc(texels, w, h, unit(&rng) * (f64)w, unit(&rng) * (f64)h,
                     0.5 + unit(&rng) * 0.2 * (f64)nph_min_u32(w, h));
        }
        const u32 wrap = (u32)(unit(&rng) * 3.0);
        const f32 factor = (f32)(0.5 + unit(&rng) * 0.5);
        const f32 cutoff = (f32)(0.1 + unit(&rng) * 0.6);
        const f32 vertex_alpha = unit(&rng) < 0.5 ? 1.0F : (f32)(0.5 + unit(&rng) * 0.5);
        CookRayTrimSlot s = slot_of(texels, w, h, wrap, factor, cutoff);
        s.wrap_v = (u32)(unit(&rng) * 3.0);
        NphVec2 uv[3];
        for (u32 c = 0; c < 3U; ++c) {
            // As the mesh holds them: half floats.
            uv[c] = nph_unpack_f16x2(nph_pack_f16x2(
                nph_vec2((f32)(unit(&rng) * 3.0 - 1.0), (f32)(unit(&rng) * 3.0 - 1.0))));
        }
        CookRayPolygon p;
        const f32 share = cook_ray_trim_triangle(&s, uv, vertex_alpha, &p);
        NTEST_EXPECT_TRUE(share >= 0.0F && share <= 1.0F);
        NTEST_EXPECT_TRUE(p.count == 0U || (p.count >= 3U && p.count <= COOK_RAY_TRIM_MAX_CORNERS));
        trimmed += p.count > 3U ? 1U : 0U;
        dropped += p.count == 0U ? 1U : 0U;
        const u32 outside = points_outside(&s, uv, vertex_alpha, &p, 4000U, &rng, &passed);
        if (outside != 0U) {
            NTEST_LOG("seed %llu: %u points that pass lie outside the polygon",
                      (unsigned long long)seed, outside);
        }
        NTEST_EXPECT_EQ_UINT(outside, 0U);
    }
    // The cases are of every kind.
    NTEST_EXPECT_GT_UINT(trimmed, 50U);
    NTEST_EXPECT_GT_UINT(passed, 40000U);
    NTEST_LOG("%u of 400 triangles trimmed, %u with no polygon, %u points passed", trimmed, dropped,
              passed);
}

// ---------------------------------------------------------------------------
// A mesh
// ---------------------------------------------------------------------------

// Cards of two triangles each, a unit square of the texture on each, side by side on x.
typedef struct Cards {
    NphMeshPosition positions[4096];
    NphMeshAttrib attribs[4096];
    u32 indices[6144];
    NphSubmesh submesh;
    u32 vertex_count;
    u32 index_count;
} Cards;

// `count` cards. `split` gives each triangle vertices of its own on the diagonal (the same
// positions under other indices), and `turned` winds the second triangle from the diagonal's
// other end.
static void make_cards(Cards *c, u32 count, bool split, bool turned)
{
    memset(c, 0, sizeof *c);
    static const f32 k_uv[4][2] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}};
    for (u32 i = 0; i < count; ++i) {
        const u32 base = c->vertex_count;
        const u32 per = split ? 6U : 4U;
        for (u32 v = 0; v < per; ++v) {
            // 0 1 2 3 the corners; 4 and 5 the diagonal's ends again (0 and 2).
            const u32 corner = v < 4U ? v : (v == 4U ? 0U : 2U);
            NphMeshPosition *p = &c->positions[base + v];
            p->x = (u16)((i * 2U + (u32)k_uv[corner][0]) * (65535U / (count * 2U)));
            p->y = (u16)(k_uv[corner][1] > 0.5F ? 65535U : 0U);
            p->z = 32768U;
            c->attribs[base + v].uv0 = nph_pack_f16x2(nph_vec2(k_uv[corner][0], k_uv[corner][1]));
            c->attribs[base + v].color = 0xFFFFFFFFU;
        }
        const u32 d0 = split ? base + 4U : base;
        const u32 d2 = split ? base + 5U : base + 2U;
        u32 *ix = &c->indices[c->index_count];
        ix[0] = base;
        ix[1] = base + 1U;
        ix[2] = base + 2U;
        if (turned) {
            ix[3] = d2;
            ix[4] = base + 3U;
            ix[5] = d0;
        } else {
            ix[3] = d0;
            ix[4] = d2;
            ix[5] = base + 3U;
        }
        c->vertex_count += per;
        c->index_count += 6U;
    }
    c->submesh.index_count = c->index_count;
}

static NphResult trim_cards(const Cards *c, const CookRayTrimSlot *slot, CookRayTrim *out)
{
    const CookRayTrimSlots slots = {slot, 1U};
    return cook_ray_trim_build(nph_allocator_system(), c->positions, c->attribs, c->vertex_count,
                               c->indices, c->index_count, &c->submesh, 1U,
                               nph_vec3(8.0F, 1.0F, 1.0F), &slots, out);
}

// Where triangle `parent`'s ray triangles meet its edge between the corners at positions `a`
// and `b`: the least and greatest weights of the greater of the two positions, in 65535ths
// (false where none of their corners lies on it).
static bool reach_on_edge(const Cards *c, const CookRayTrim *trim, u32 parent, NphMeshPosition a,
                          NphMeshPosition b, u32 *out_lo, u32 *out_hi)
{
    const u64 ka = (u64)a.x | ((u64)a.y << 16U) | ((u64)a.z << 32U);
    const u64 kb = (u64)b.x | ((u64)b.y << 16U) | ((u64)b.z << 32U);
    u32 lo = 65536U;
    u32 hi = 0U;
    NPH_ARRAY_FOREACH (NphMeshRayTriangle, t, &trim->triangles) {
        if (t->parent != parent) {
            continue;
        }
        for (u32 k = 0; k < 3U; ++k) {
            u32 w[3];
            w[1] = t->corner[k] & 0xFFFFU;
            w[2] = t->corner[k] >> 16U;
            w[0] = 65535U - w[1] - w[2];
            // The corner's weights by position: on the edge where all of it is the two ends'.
            u32 wa = 0U;
            u32 wb = 0U;
            for (u32 v = 0; v < 3U; ++v) {
                const NphMeshPosition q = c->positions[c->indices[parent + v]];
                const u64 kq = (u64)q.x | ((u64)q.y << 16U) | ((u64)q.z << 32U);
                wa += kq == ka ? w[v] : 0U;
                wb += kq == kb ? w[v] : 0U;
            }
            if (wa + wb != 65535U) {
                continue;
            }
            const u32 t_hi = kb > ka ? wb : wa;
            lo = t_hi < lo ? t_hi : lo;
            hi = t_hi > hi ? t_hi : hi;
        }
    }
    *out_lo = lo;
    *out_hi = hi;
    return lo <= hi;
}

NTEST(CookRayTrim, a_meshs_cards_are_trimmed_and_share_their_diagonals_points)
{
    // A leaf across each card's diagonal, off its middle, so each triangle's own polygon
    // meets the diagonal over a stretch of its own.
    u8 texels[64 * 64];
    memset(texels, 0, sizeof texels);
    add_disc(texels, 64U, 64U, 26.0, 30.0, 9.0);
    add_disc(texels, 64U, 64U, 40.0, 37.0, 5.0);
    const CookRayTrimSlot slot =
        slot_of(texels, 64U, 64U, (u32)NPH_MATERIAL_WRAP_CLAMP, 1.0F, 0.5F);
    for (u32 variant = 0; variant < 4U; ++variant) {
        Cards *c = NPH_ALLOC_ARRAY(nph_allocator_system(), Cards, 1U);
        NTEST_ASSERT_NOT_NULL(c);
        make_cards(c, 40U, (variant & 1U) != 0U, (variant & 2U) != 0U);
        CookRayTrim trim;
        cook_ray_trim_init(&trim, nph_allocator_system());
        NTEST_ASSERT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(trim.ranges.len, 1U);
        NTEST_EXPECT_EQ_UINT(trim.ranges.data[0].first, 0U);
        NTEST_EXPECT_EQ_UINT(trim.ranges.data[0].count, trim.triangles.len);
        NTEST_EXPECT_TRUE(trim.ranges.data[0].alpha_key == slot.key);
        NTEST_EXPECT_EQ_UINT(trim.stats.submeshes, 1U);
        NTEST_EXPECT_EQ_UINT(trim.stats.parents, 80U);
        NTEST_EXPECT_EQ_UINT(trim.stats.dropped, 0U);
        NTEST_EXPECT_EQ_UINT(trim.stats.distinct, 2U); // every card is the same two triangles
        NTEST_EXPECT_TRUE(trim.stats.area_kept < 0.6 * trim.stats.area);
        NTEST_EXPECT_GE_UINT(trim.stats.edge_passes, 2U); // the diagonals grew, then held
        // Every ray triangle is part of a triangle of the submesh, its corners in it, and
        // the parents come in order.
        u32 last = 0U;
        NPH_ARRAY_FOREACH (NphMeshRayTriangle, t, &trim.triangles) {
            NTEST_ASSERT_TRUE(t->parent < c->index_count && t->parent % 3U == 0U);
            NTEST_EXPECT_GE_UINT(t->parent, last);
            last = t->parent;
            for (u32 k = 0; k < 3U; ++k) {
                NTEST_EXPECT_LE_UINT((t->corner[k] & 0xFFFFU) + (t->corner[k] >> 16U), 65535U);
            }
        }
        // Each card's two triangles meet their diagonal over the same stretch, to the
        // 65535th, whichever vertices and whichever turn each has it by.
        for (u32 i = 0; i < 40U; ++i) {
            const u32 first = i * 6U;
            const NphMeshPosition a = c->positions[c->indices[first]];
            const NphMeshPosition b = c->positions[c->indices[first + 2U]];
            u32 lo[2] = {0U, 0U};
            u32 hi[2] = {0U, 0U};
            NTEST_ASSERT_TRUE(reach_on_edge(c, &trim, first, a, b, &lo[0], &hi[0]));
            NTEST_ASSERT_TRUE(reach_on_edge(c, &trim, first + 3U, a, b, &lo[1], &hi[1]));
            NTEST_EXPECT_EQ_UINT(lo[0], lo[1]);
            NTEST_EXPECT_EQ_UINT(hi[0], hi[1]);
            NTEST_EXPECT_LT_UINT(lo[0], hi[0]);
            NTEST_EXPECT_TRUE(lo[0] > 0U && hi[0] < 65535U); // and not the whole of it
        }
        // The same again: the cook is the same every time.
        CookRayTrim again;
        cook_ray_trim_init(&again, nph_allocator_system());
        NTEST_ASSERT_EQ_INT(trim_cards(c, &slot, &again), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(again.triangles.len, trim.triangles.len);
        NTEST_EXPECT_EQ_MEM(again.triangles.data, trim.triangles.data,
                            NPH_ARRAY_BYTES(&trim.triangles));
        cook_ray_trim_free(&again);
        cook_ray_trim_free(&trim);
        NPH_FREE_ARRAY(nph_allocator_system(), Cards, c, 1U);
    }
}

NTEST(CookRayTrim, a_submesh_is_left_where_trimming_it_does_not_pay)
{
    Cards *c = NPH_ALLOC_ARRAY(nph_allocator_system(), Cards, 1U);
    NTEST_ASSERT_NOT_NULL(c);
    make_cards(c, 20U, false, false);
    u8 texels[64 * 64];
    CookRayTrim trim;
    cook_ray_trim_init(&trim, nph_allocator_system());
    // A leaf that fills most of its card: under the share that pays.
    memset(texels, 0, sizeof texels);
    add_disc(texels, 64U, 64U, 32.0, 32.0, 30.0);
    CookRayTrimSlot slot = slot_of(texels, 64U, 64U, (u32)NPH_MATERIAL_WRAP_CLAMP, 1.0F, 0.5F);
    NTEST_ASSERT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(trim.ranges.len, 0U);
    NTEST_EXPECT_EQ_UINT(trim.triangles.len, 0U);
    NTEST_EXPECT_EQ_UINT(trim.stats.left, 1U);
    // A card nothing passes anywhere in keeps its triangles: a geometry has some.
    memset(texels, 0, sizeof texels);
    slot = slot_of(texels, 64U, 64U, (u32)NPH_MATERIAL_WRAP_CLAMP, 1.0F, 0.5F);
    NTEST_ASSERT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(trim.triangles.len, 0U);
    // A slot without a key, and a mesh without slots: none.
    add_disc(texels, 64U, 64U, 32.0, 32.0, 8.0);
    slot = slot_of(texels, 64U, 64U, (u32)NPH_MATERIAL_WRAP_CLAMP, 1.0F, 0.5F);
    slot.key = 0U;
    NTEST_ASSERT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(trim.triangles.len, 0U);
    NTEST_EXPECT_EQ_INT(cook_ray_trim_build(nph_allocator_system(), c->positions, c->attribs,
                                            c->vertex_count, c->indices, c->index_count,
                                            &c->submesh, 1U, nph_vec3(8.0F, 1.0F, 1.0F), NULL,
                                            &trim),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(trim.triangles.len, 0U);
    // With its key the small leaf is trimmed; and a submesh that names a slot past the
    // slots given is left.
    slot = slot_of(texels, 64U, 64U, (u32)NPH_MATERIAL_WRAP_CLAMP, 1.0F, 0.5F);
    NTEST_ASSERT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_OK);
    NTEST_EXPECT_GT_UINT(trim.triangles.len, 0U);
    c->submesh.material_slot = 3U;
    NTEST_ASSERT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(trim.triangles.len, 0U);
    c->submesh.material_slot = 0U;
    // A range or an index out of bounds is refused, and leaves nothing.
    c->submesh.index_count = c->index_count + 3U;
    NTEST_EXPECT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_INVALID_ARGUMENT);
    c->submesh.index_count = c->index_count;
    c->indices[7] = c->vertex_count;
    NTEST_EXPECT_EQ_INT(trim_cards(c, &slot, &trim), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(trim.triangles.len, 0U);
    cook_ray_trim_free(&trim);
    NPH_FREE_ARRAY(nph_allocator_system(), Cards, c, 1U);
}
