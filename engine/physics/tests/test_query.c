// SPDX-License-Identifier: Apache-2.0
// World queries (query.h): raycasts, shape casts, overlaps, and contacts
// against brute force over every shape; callbacks that clip and end; the
// filter; the same world 10 000 km out; bodies moved and asleep; queries on
// many threads at once; and a body resting on its floor casting along it.

#include <nepheline/physics/query.h>

#include "test_world_support.h"

#include <float.h>

// ---------------------------------------------------------------------------
// A world of mixed shapes
// ---------------------------------------------------------------------------

#define CONVEX_SHAPES 180U
#define WORLD_SHAPES  (CONVEX_SHAPES + 2U)

typedef struct World {
    Scene s;
    NphPhysicsHull *hulls[2];
    NphPhysicsMesh *mesh;
    NphPhysicsHeightfield *field;
    NphPhysicsShape shapes[WORLD_SHAPES];
    NphPhysicsBody bodies[WORLD_SHAPES];
    u32 count;
    NphDVec3 at; // the world's middle
} World;

static void add_shape(World *w, NphPhysicsBodyType type, NphDVec3 p, NphQuat q,
                      NphPhysicsGeometry g, NphPhysicsFilter filter)
{
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.type = type;
    bd.position = p;
    bd.rotation = q;
    bd.gravity_scale = 0.0F;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(w->s.w, &bd, &b), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    sd.filter = filter;
    NphPhysicsShape s;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(w->s.w, b, &sd, &s), NPH_RESULT_OK);
    w->bodies[w->count] = b;
    w->shapes[w->count] = s;
    w->count += 1U;
}

// Spheres, capsules, boxes, and hulls on static, kinematic, and dynamic
// bodies around `at`, in four categories (a few masks shut queries out),
// over a mesh and a heightfield.
static void world_build(World *w, NphDVec3 at, u64 seed)
{
    scene_begin(&w->s);
    w->count = 0U;
    w->at = at;
    NphAllocator *alloc = w->s.alloc;
    NphPcg32 r = test_rng(seed);
    w->hulls[0] = test_random_hull(alloc, &r, 24U, 0.8F, nph_vec3(1.0F, 0.6F, 0.9F));
    w->hulls[1] = test_random_hull(alloc, &r, 40U, 0.7F, nph_vec3(0.7F, 1.0F, 0.5F));
    f32 heights[25U * 25U];
    for (u32 i = 0U; i < 25U * 25U; ++i) {
        heights[i] = test_range(&r, -0.5F, 0.5F);
    }
    w->mesh = test_grid_mesh(alloc, 16U, 1.0F, 0.0F, 0.0F, heights, false);
    w->field = test_heightfield(alloc, 25U, 25U, 1.0F, heights, NULL);
    for (u32 i = 0U; i < CONVEX_SHAPES; ++i) {
        NphPhysicsGeometry g;
        switch (i % 4U) {
        case 0U:
            g = nph_physics_geometry_sphere(test_range(&r, 0.2F, 1.0F));
            break;
        case 1U:
            g = nph_physics_geometry_capsule(test_range(&r, 0.1F, 0.8F),
                                             test_range(&r, 0.15F, 0.5F));
            break;
        case 2U:
            g = nph_physics_geometry_box(test_vec3(&r, 0.15F, 0.9F));
            break;
        default:
            g = nph_physics_geometry_hull(w->hulls[(i / 4U) % 2U]);
            break;
        }
        NphPhysicsFilter f = nph_physics_filter_default();
        f.category = 1ULL << (i % 4U);
        if (i % 17U == 5U) {
            f.mask = ~1ULL; // keeps the default query category off it
        }
        const NphVec3 p = test_vec3(&r, -10.0F, 10.0F);
        add_shape(w, (NphPhysicsBodyType)(i % 3U), nph_dvec3_add_vec3(at, p), test_rotation(&r), g,
                  f);
    }
    add_shape(w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_add_vec3(at, nph_vec3(-8.0F, -12.0F, -8.0F)),
              nph_quat_identity(), nph_physics_geometry_mesh(w->mesh),
              nph_physics_filter_default());
    add_shape(w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_add_vec3(at, nph_vec3(-12.0F, -14.0F, -12.0F)),
              test_rotation(&r), nph_physics_geometry_heightfield(w->field),
              nph_physics_filter_default());
}

static void world_end(World *w)
{
    nph_physics_world_destroy(w->s.w);
    w->s.w = NULL;
    nph_physics_hull_free(w->hulls[0], w->s.alloc);
    nph_physics_hull_free(w->hulls[1], w->s.alloc);
    nph_physics_mesh_free(w->mesh, w->s.alloc);
    nph_physics_heightfield_free(w->field, w->s.alloc);
    test_alloc_end(&w->s.ta);
}

// Shape i's pose relative to `origin` (32-bit, as collide.h takes it).
static NphPhysicsPose pose_from(const World *w, u32 i, NphDVec3 origin)
{
    const NphPhysicsBody b = w->bodies[i];
    const NphDVec3 bp = nph_physics_body_position(w->s.w, b);
    const NphQuat bq = nph_physics_body_rotation(w->s.w, b);
    const NphPhysicsPose sp = nph_physics_shape_pose(w->s.w, w->shapes[i]);
    const NphDVec3 p = nph_dvec3_add_vec3(bp, nph_quat_rotate(bq, sp.position));
    return nph_physics_pose(nph_dvec3_relative(p, origin), nph_quat_mul(bq, sp.rotation));
}

static bool sees(const World *w, u32 i, const NphPhysicsQueryFilter *f)
{
    const NphPhysicsFilter sf = nph_physics_shape_filter(w->s.w, w->shapes[i]);
    const NphPhysicsBodyType type = nph_physics_body_type(w->s.w, w->bodies[i]);
    return (sf.category & f->mask) != 0U && (sf.mask & f->category) != 0U &&
           ((f->body_types >> (u32)type) & 1U) != 0U && w->bodies[i].value != f->ignore.value;
}

static u32 index_of(const World *w, NphPhysicsShape s)
{
    for (u32 i = 0U; i < w->count; ++i) {
        if (w->shapes[i].value == s.value) {
            return i;
        }
    }
    return NPH_U32_MAX;
}

// A query's origin and motion: from somewhere in the world, through it.
static void random_motion(NphPcg32 *r, NphDVec3 at, NphDVec3 *origin, NphVec3 *translation)
{
    *origin = nph_dvec3_add_vec3(at, test_vec3(r, -14.0F, 14.0F));
    *translation = nph_vec3_scale(test_unit(r), test_range(r, 2.0F, 30.0F));
}

// ---------------------------------------------------------------------------
// Raycasts
// ---------------------------------------------------------------------------

// The closest hit over every shape the filter passes, by collide.h.
static bool brute_raycast(const World *w, NphDVec3 origin, NphVec3 translation,
                          const NphPhysicsQueryFilter *f, u32 *shape, NphPhysicsCastHit *best)
{
    bool found = false;
    for (u32 i = 0U; i < w->count; ++i) {
        if (!sees(w, i, f)) {
            continue;
        }
        const NphPhysicsGeometry g = nph_physics_shape_geometry(w->s.w, w->shapes[i]);
        NphPhysicsCastHit hit;
        if (nph_physics_raycast(&g, pose_from(w, i, origin), nph_vec3_zero(), translation, &hit) &&
            (!found || hit.fraction < best->fraction)) {
            *best = hit;
            *shape = i;
            found = true;
        }
    }
    return found;
}

NTEST(query, raycasts_find_the_closest_hit_brute_force_finds)
{
    World w;
    world_build(&w, nph_dvec3(3.0, -2.0, 5.0), 201U);
    NphPcg32 r = test_rng(202U);
    const NphPhysicsQueryFilter f = nph_physics_query_filter_default();
    u32 hits = 0U;
    u32 surfaces = 0U;
    for (u32 k = 0U; k < 300U * TEST_SCALE; ++k) {
        NphDVec3 origin;
        NphVec3 translation;
        random_motion(&r, w.at, &origin, &translation);
        u32 want_shape = 0U;
        NphPhysicsCastHit want;
        memset(&want, 0, sizeof want);
        const bool want_hit = brute_raycast(&w, origin, translation, &f, &want_shape, &want);
        NphPhysicsQueryHit got;
        const bool got_hit =
            nph_physics_world_raycast_closest(w.s.w, origin, translation, NULL, &got);
        NTEST_ASSERT_EQ_INT(got_hit, want_hit);
        if (!got_hit) {
            continue;
        }
        hits += 1U;
        NTEST_EXPECT_NEAR_F32(got.fraction, want.fraction, 1e-5F);
        const u32 i = index_of(&w, got.shape);
        NTEST_ASSERT_TRUE(i != NPH_U32_MAX);
        NTEST_EXPECT_EQ_UINT(got.body.value, w.bodies[i].value);
        if (i == want_shape) {
            NTEST_EXPECT_TRUE(nph_vec3_near(got.normal, want.normal, 1e-4F));
            NTEST_EXPECT_EQ_UINT(got.triangle, want.triangle);
            surfaces += i >= CONVEX_SHAPES ? 1U : 0U;
        }
        // The point on the ray, in 64 bits.
        const NphDVec3 on = nph_dvec3_add_vec3(origin, nph_vec3_scale(translation, got.fraction));
        NTEST_EXPECT_TRUE(nph_dvec3_near(got.point, on, 1e-5));
    }
    NTEST_LOG("raycasts: %u hits, %u on surfaces", hits, surfaces);
    NTEST_EXPECT_TRUE(hits > 25U * TEST_SCALE && surfaces > 10U * TEST_SCALE);
    world_end(&w);
}

typedef struct Collect {
    u32 count;
    u32 shapes[WORLD_SHAPES];
    f32 fractions[WORLD_SHAPES];
    f32 answer;   // what the callback returns: 1, 0, -1, or (negative 2) the hit's fraction
    f32 limit;    // the last fraction returned
    bool clipped; // every hit within the last limit
    const World *w;
} Collect;

static f32 collect(void *user, const NphPhysicsQueryHit *hit)
{
    Collect *c = (Collect *)user;
    if (c->count < WORLD_SHAPES) {
        c->shapes[c->count] = index_of(c->w, hit->shape);
        c->fractions[c->count] = hit->fraction;
        c->count += 1U;
    }
    c->clipped = c->clipped && hit->fraction <= c->limit;
    const f32 r = c->answer == -2.0F ? hit->fraction : c->answer;
    if (r >= 0.0F) {
        c->limit = nph_f32_min(c->limit, r);
    }
    return r;
}

NTEST(query, raycast_callbacks_see_each_hit_and_clip_or_end)
{
    World w;
    world_build(&w, nph_dvec3_zero(), 203U);
    NphPcg32 r = test_rng(204U);
    const NphPhysicsQueryFilter f = nph_physics_query_filter_default();
    static Collect c;
    u32 multiple = 0U;
    for (u32 k = 0U; k < 100U * TEST_SCALE; ++k) {
        // From outside the world, through its middle: past many shapes.
        const NphVec3 from = nph_vec3_scale(test_unit(&r), 22.0F);
        const NphVec3 aim = test_vec3(&r, -5.0F, 5.0F);
        const NphDVec3 origin = nph_dvec3_add_vec3(w.at, from);
        const NphVec3 translation = nph_vec3_scale(nph_vec3_sub(aim, from), 2.0F);
        // Returning 1: every shape's hit, once, as brute force has them.
        memset(&c, 0, sizeof c);
        c.w = &w;
        c.answer = 1.0F;
        c.limit = 1.0F;
        c.clipped = true;
        nph_physics_world_raycast(w.s.w, origin, translation, NULL, collect, &c);
        u32 want = 0U;
        for (u32 i = 0U; i < w.count; ++i) {
            const NphPhysicsGeometry g = nph_physics_shape_geometry(w.s.w, w.shapes[i]);
            NphPhysicsCastHit hit;
            if (!sees(&w, i, &f) || !nph_physics_raycast(&g, pose_from(&w, i, origin),
                                                         nph_vec3_zero(), translation, &hit)) {
                continue;
            }
            want += 1U;
            u32 seen = 0U;
            for (u32 j = 0U; j < c.count; ++j) {
                if (c.shapes[j] == i) {
                    seen += 1U;
                    NTEST_EXPECT_NEAR_F32(c.fractions[j], hit.fraction, 1e-5F);
                }
            }
            NTEST_EXPECT_EQ_UINT(seen, 1U);
        }
        NTEST_EXPECT_EQ_UINT(c.count, want);
        multiple += want > 1U ? 1U : 0U;
        if (want == 0U) {
            continue;
        }
        // Returning the hit's fraction: each later hit no farther than the
        // last.
        memset(&c, 0, sizeof c);
        c.w = &w;
        c.answer = -2.0F;
        c.limit = 1.0F;
        c.clipped = true;
        nph_physics_world_raycast(w.s.w, origin, translation, NULL, collect, &c);
        NTEST_EXPECT_TRUE(c.clipped);
        NTEST_EXPECT_TRUE(c.count >= 1U && c.count <= want);
        // Returning a negative value: the first hit only.
        memset(&c, 0, sizeof c);
        c.w = &w;
        c.answer = -1.0F;
        c.limit = 1.0F;
        c.clipped = true;
        nph_physics_world_raycast(w.s.w, origin, translation, NULL, collect, &c);
        NTEST_EXPECT_EQ_UINT(c.count, 1U);
    }
    NTEST_LOG("rays with more than one hit: %u", multiple);
    NTEST_EXPECT_TRUE(multiple > 8U * TEST_SCALE);
    world_end(&w);
}

// Three boxes in a row along X, on a static, a kinematic, and a dynamic
// body, the dynamic one in category 2 only.
NTEST(query, filters_choose_the_shapes_queries_see)
{
    World w;
    scene_begin(&w.s);
    w.count = 0U;
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3_splat(0.5F));
    NphPhysicsFilter other = nph_physics_filter_default();
    other.category = 2U;
    add_shape(&w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(2.0, 0.0, 0.0), nph_quat_identity(), box,
              nph_physics_filter_default());
    add_shape(&w, NPH_PHYSICS_BODY_KINEMATIC, nph_dvec3(4.0, 0.0, 0.0), nph_quat_identity(), box,
              nph_physics_filter_default());
    add_shape(&w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(6.0, 0.0, 0.0), nph_quat_identity(), box,
              other);
    const NphDVec3 o = nph_dvec3_zero();
    const NphVec3 d = nph_vec3(10.0F, 0.0F, 0.0F);
    NphPhysicsQueryFilter f = nph_physics_query_filter_default();
    NphPhysicsQueryHit hit;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(w.s.w, o, d, &f, &hit));
    NTEST_EXPECT_EQ_UINT(hit.shape.value, w.shapes[0].value);
    NTEST_EXPECT_NEAR_F32(hit.fraction, 0.15F, 1e-6F);
    // Kinds of bodies.
    f.body_types = NPH_PHYSICS_QUERY_KINEMATIC | NPH_PHYSICS_QUERY_DYNAMIC;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(w.s.w, o, d, &f, &hit));
    NTEST_EXPECT_EQ_UINT(hit.shape.value, w.shapes[1].value);
    // A body ignored, and a mask without the dynamic box's category.
    f.ignore = w.bodies[1];
    f.mask = 1U;
    NTEST_EXPECT_FALSE(nph_physics_world_raycast_closest(w.s.w, o, d, &f, &hit));
    // ... the dynamic box is category 2, which the query's mask must hold.
    f.mask = 2U;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(w.s.w, o, d, &f, &hit));
    NTEST_EXPECT_EQ_UINT(hit.shape.value, w.shapes[2].value);
    // Its own mask must hold the query's category too.
    NphPhysicsFilter shut = other;
    shut.mask = ~1ULL;
    nph_physics_shape_set_filter(w.s.w, w.shapes[2], shut);
    NTEST_EXPECT_FALSE(nph_physics_world_raycast_closest(w.s.w, o, d, &f, &hit));
    f.category = 4U;
    NTEST_EXPECT_TRUE(nph_physics_world_raycast_closest(w.s.w, o, d, &f, &hit));
    // A filter that sees nothing, and a zero translation.
    f.body_types = 0U;
    NTEST_EXPECT_FALSE(nph_physics_world_raycast_closest(w.s.w, o, d, &f, &hit));
    NTEST_EXPECT_FALSE(nph_physics_world_raycast_closest(w.s.w, o, nph_vec3_zero(), NULL, &hit));
    scene_end(&w.s);
}

// ---------------------------------------------------------------------------
// Shape casts
// ---------------------------------------------------------------------------

static NphPhysicsGeometry query_geometry(u32 k, const World *w)
{
    switch (k % 4U) {
    case 0U:
        return nph_physics_geometry_sphere(0.4F);
    case 1U:
        return nph_physics_geometry_capsule(0.5F, 0.3F);
    case 2U:
        return nph_physics_geometry_box(nph_vec3(0.4F, 0.3F, 0.5F));
    default:
        return nph_physics_geometry_hull(w->hulls[0]);
    }
}

NTEST(query, shape_casts_find_the_closest_hit_brute_force_finds)
{
    World w;
    world_build(&w, nph_dvec3(-7.0, 1.0, 2.0), 205U);
    NphPcg32 r = test_rng(206U);
    const NphPhysicsQueryFilter f = nph_physics_query_filter_default();
    u32 hits = 0U;
    for (u32 k = 0U; k < 160U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry g = query_geometry(k, &w);
        const NphQuat q = test_rotation(&r);
        NphDVec3 origin;
        NphVec3 translation;
        random_motion(&r, w.at, &origin, &translation);
        // Brute force, from the shapes the cast does not start in.
        bool want_hit = false;
        u32 want_shape = 0U;
        NphPhysicsCastHit want;
        memset(&want, 0, sizeof want);
        for (u32 i = 0U; i < w.count; ++i) {
            const NphPhysicsGeometry sg = nph_physics_shape_geometry(w.s.w, w.shapes[i]);
            NphPhysicsCastHit hit;
            if (sees(&w, i, &f) &&
                nph_physics_shape_cast(&g, nph_physics_pose(nph_vec3_zero(), q), translation, &sg,
                                       pose_from(&w, i, origin), &hit) &&
                (!want_hit || hit.fraction < want.fraction)) {
                want = hit;
                want_shape = i;
                want_hit = true;
            }
        }
        NphPhysicsQueryHit got;
        const bool got_hit =
            nph_physics_world_cast_shape_closest(w.s.w, &g, origin, q, translation, NULL, &got);
        NTEST_ASSERT_EQ_INT(got_hit, want_hit);
        if (!got_hit) {
            continue;
        }
        hits += 1U;
        NTEST_EXPECT_NEAR_F32(got.fraction, want.fraction, 1e-5F);
        if (index_of(&w, got.shape) == want_shape) {
            NTEST_EXPECT_TRUE(nph_vec3_near(got.normal, want.normal, 1e-3F));
            const NphDVec3 p = nph_dvec3_add_vec3(origin, want.point);
            NTEST_EXPECT_TRUE(nph_dvec3_near(got.point, p, 1e-4));
        }
    }
    NTEST_LOG("shape casts: %u hits", hits);
    NTEST_EXPECT_TRUE(hits > 20U * TEST_SCALE);
    world_end(&w);
}

// ---------------------------------------------------------------------------
// Overlaps and contacts
// ---------------------------------------------------------------------------

typedef struct Found {
    u32 count;
    u32 seen[WORLD_SHAPES];
    const World *w;
} Found;

static bool note_shape(void *user, NphPhysicsShape shape, NphPhysicsBody body)
{
    Found *f = (Found *)user;
    const u32 i = index_of(f->w, shape);
    NTEST_EXPECT_TRUE(i != NPH_U32_MAX && f->w->bodies[i].value == body.value);
    if (i != NPH_U32_MAX) {
        f->seen[i] += 1U;
        f->count += 1U;
    }
    return true;
}

NTEST(query, overlaps_find_the_shapes_brute_force_finds)
{
    World w;
    world_build(&w, nph_dvec3(1.0, 2.0, -3.0), 207U);
    NphPcg32 r = test_rng(208U);
    const NphPhysicsQueryFilter f = nph_physics_query_filter_default();
    static Found found;
    u32 touching = 0U;
    for (u32 k = 0U; k < 200U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry g = query_geometry(k, &w);
        const NphQuat q = test_rotation(&r);
        const NphDVec3 at = nph_dvec3_add_vec3(w.at, test_vec3(&r, -12.0F, 12.0F));
        memset(&found, 0, sizeof found);
        found.w = &w;
        nph_physics_world_overlap_shape(w.s.w, &g, at, q, NULL, note_shape, &found);
        for (u32 i = 0U; i < w.count; ++i) {
            const NphPhysicsGeometry sg = nph_physics_shape_geometry(w.s.w, w.shapes[i]);
            const bool want =
                sees(&w, i, &f) && nph_physics_overlap(&g, nph_physics_pose(nph_vec3_zero(), q),
                                                       &sg, pose_from(&w, i, at));
            NTEST_EXPECT_EQ_UINT(found.seen[i], want ? 1U : 0U);
            touching += want ? 1U : 0U;
        }
        // Boxes: the shapes whose bounds overlap.
        const NphVec3 lo =
            nph_dvec3_to_vec3(nph_dvec3_add_vec3(w.at, test_vec3(&r, -12.0F, 12.0F)));
        const NphAABB box = {lo, nph_vec3_add(lo, test_vec3(&r, 0.0F, 4.0F))};
        memset(&found, 0, sizeof found);
        found.w = &w;
        nph_physics_world_overlap_aabb(w.s.w, box, NULL, note_shape, &found);
        for (u32 i = 0U; i < w.count; ++i) {
            const bool want = sees(&w, i, &f) &&
                              nph_aabb_overlaps(nph_physics_shape_aabb(w.s.w, w.shapes[i]), box);
            NTEST_EXPECT_EQ_UINT(found.seen[i], want ? 1U : 0U);
        }
    }
    NTEST_LOG("overlaps: %u shapes touched", touching);
    NTEST_EXPECT_TRUE(touching > 5U * TEST_SCALE);
    world_end(&w);
}

typedef struct Contacts {
    u32 count;
    u32 shape[4U * WORLD_SHAPES];
    NphPhysicsManifold manifold[4U * WORLD_SHAPES];
    const World *w;
} Contacts;

static bool note_contact(void *user, const NphPhysicsQueryContact *c)
{
    Contacts *k = (Contacts *)user;
    if (k->count < 4U * WORLD_SHAPES) {
        k->shape[k->count] = index_of(k->w, c->shape);
        k->manifold[k->count] = c->manifold;
        k->count += 1U;
    }
    return true;
}

NTEST(query, contacts_are_the_narrow_phases)
{
    World w;
    world_build(&w, nph_dvec3(4.0, 0.0, 4.0), 209U);
    NphPcg32 r = test_rng(210U);
    static Contacts got;
    u32 manifolds = 0U;
    const f32 spec = NPH_PHYSICS_SPECULATIVE_DISTANCE;
    for (u32 k = 0U; k < 150U * TEST_SCALE; ++k) {
        const NphPhysicsGeometry g = query_geometry(k, &w);
        const NphQuat q = test_rotation(&r);
        const NphDVec3 at = nph_dvec3_add_vec3(w.at, test_vec3(&r, -12.0F, 12.0F));
        memset(&got, 0, sizeof got);
        got.w = &w;
        nph_physics_world_collide_shape(w.s.w, &g, at, q, spec, NULL, note_contact, &got);
        const NphPhysicsPose here = nph_physics_pose(nph_vec3_zero(), q);
        const NphPhysicsQueryFilter f = nph_physics_query_filter_default();
        u32 want = 0U;
        for (u32 i = 0U; i < w.count; ++i) {
            if (!sees(&w, i, &f)) {
                continue;
            }
            const NphPhysicsGeometry sg = nph_physics_shape_geometry(w.s.w, w.shapes[i]);
            const NphPhysicsPose ps = pose_from(&w, i, at);
            NphPhysicsMeshManifolds mm;
            mm.count = 0U;
            if (sg.type == NPH_PHYSICS_SHAPE_MESH) {
                nph_physics_collide_mesh(&g, here, sg.mesh, ps, spec, &mm);
            } else if (sg.type == NPH_PHYSICS_SHAPE_HEIGHTFIELD) {
                nph_physics_collide_heightfield(&g, here, sg.heightfield, ps, spec, &mm);
            } else {
                nph_physics_collide(&g, here, &sg, ps, spec, NULL, &mm.manifolds[0]);
                mm.count = mm.manifolds[0].point_count > 0U ? 1U : 0U;
            }
            u32 seen = 0U;
            for (u32 j = 0U; j < got.count; ++j) {
                if (got.shape[j] != i) {
                    continue;
                }
                NTEST_ASSERT_TRUE(seen < mm.count);
                const NphPhysicsManifold *a = &got.manifold[j];
                const NphPhysicsManifold *b = &mm.manifolds[seen];
                NTEST_EXPECT_EQ_UINT(a->point_count, b->point_count);
                NTEST_EXPECT_TRUE(nph_vec3_near(a->normal, b->normal, 1e-4F));
                for (u32 p = 0U; p < a->point_count && p < b->point_count; ++p) {
                    NTEST_EXPECT_TRUE(nph_vec3_near(a->points[p].point, b->points[p].point, 1e-4F));
                    NTEST_EXPECT_NEAR_F32(a->points[p].separation, b->points[p].separation, 1e-4F);
                }
                seen += 1U;
            }
            NTEST_EXPECT_EQ_UINT(seen, mm.count);
            want += mm.count;
        }
        NTEST_EXPECT_EQ_UINT(got.count, want);
        manifolds += want;
    }
    NTEST_LOG("contacts: %u manifolds", manifolds);
    NTEST_EXPECT_TRUE(manifolds > 5U * TEST_SCALE);
    world_end(&w);
}

// ---------------------------------------------------------------------------
// Precision, motion, sleep, threads
// ---------------------------------------------------------------------------

NTEST(query, queries_are_the_same_10000_km_out)
{
    // The same world at the origin and 10 000 km out: each query relative
    // to its world's middle gives the same answers.
    static World near_w;
    static World far_w;
    const NphDVec3 far_at = nph_dvec3(1.0e7, -2.5e6, -1.0e7);
    world_build(&near_w, nph_dvec3_zero(), 211U);
    world_build(&far_w, far_at, 211U);
    NphPcg32 r = test_rng(212U);
    u32 hits = 0U;
    for (u32 k = 0U; k < 100U * TEST_SCALE; ++k) {
        const NphVec3 from = test_vec3(&r, -14.0F, 14.0F);
        const NphVec3 d = nph_vec3_scale(test_unit(&r), test_range(&r, 2.0F, 30.0F));
        const NphPhysicsGeometry g = query_geometry(k, &near_w);
        const NphQuat q = test_rotation(&r);
        NphPhysicsQueryHit a;
        NphPhysicsQueryHit b;
        const bool ray_a =
            nph_physics_world_raycast_closest(near_w.s.w, nph_dvec3_from_vec3(from), d, NULL, &a);
        const bool ray_b = nph_physics_world_raycast_closest(
            far_w.s.w, nph_dvec3_add_vec3(far_at, from), d, NULL, &b);
        NTEST_ASSERT_EQ_INT(ray_a, ray_b);
        if (ray_a) {
            NTEST_EXPECT_EQ_UINT(index_of(&near_w, a.shape), index_of(&far_w, b.shape));
            NTEST_EXPECT_NEAR_F32(a.fraction, b.fraction, 1e-6F);
            NTEST_EXPECT_TRUE(nph_vec3_near(a.normal, b.normal, 1e-5F));
            NTEST_EXPECT_EQ_UINT(a.triangle, b.triangle);
            NTEST_EXPECT_TRUE(nph_dvec3_near(nph_dvec3_sub(b.point, far_at), a.point, 1e-5));
            hits += 1U;
        }
        const bool cast_a = nph_physics_world_cast_shape_closest(
            near_w.s.w, &g, nph_dvec3_from_vec3(from), q, d, NULL, &a);
        const bool cast_b = nph_physics_world_cast_shape_closest(
            far_w.s.w, &g, nph_dvec3_add_vec3(far_at, from), q, d, NULL, &b);
        NTEST_ASSERT_EQ_INT(cast_a, cast_b);
        if (cast_a) {
            NTEST_EXPECT_EQ_UINT(index_of(&near_w, a.shape), index_of(&far_w, b.shape));
            NTEST_EXPECT_NEAR_F32(a.fraction, b.fraction, 1e-6F);
            NTEST_EXPECT_TRUE(nph_vec3_near(a.normal, b.normal, 1e-5F));
            NTEST_EXPECT_TRUE(nph_dvec3_near(nph_dvec3_sub(b.point, far_at), a.point, 1e-5));
            hits += 1U;
        }
    }
    NTEST_LOG("far out: %u hits", hits);
    NTEST_EXPECT_TRUE(hits > 20U * TEST_SCALE);
    world_end(&near_w);
    world_end(&far_w);
}

typedef struct Count {
    u32 n;
} Count;

static bool count_shape(void *user, NphPhysicsShape shape, NphPhysicsBody body)
{
    NPH_UNUSED(shape);
    NPH_UNUSED(body);
    ((Count *)user)->n += 1U;
    return true;
}

NTEST(query, queries_see_bodies_moved_and_asleep)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    const NphPhysicsBody box = add_box(s.w, nph_dvec3(0.0, 0.5, 0.0), nph_vec3_splat(0.5F));
    step(s.w, NULL, 240U);
    NTEST_ASSERT_FALSE(nph_physics_body_is_awake(s.w, box));
    NphPhysicsQueryHit hit;
    const NphVec3 down = nph_vec3(0.0F, -10.0F, 0.0F);
    // Asleep, it is seen.
    NTEST_ASSERT_TRUE(
        nph_physics_world_raycast_closest(s.w, nph_dvec3(0.1, 5.0, 0.2), down, NULL, &hit));
    NTEST_EXPECT_EQ_UINT(hit.body.value, box.value);
    NTEST_EXPECT_NEAR_F32(hit.fraction, 0.4F, 1e-3F);
    // Moved, it is seen where it went, at once.
    nph_physics_body_set_transform(s.w, box, nph_dvec3(20.0, 0.5, 0.0), nph_quat_identity());
    NTEST_ASSERT_TRUE(
        nph_physics_world_raycast_closest(s.w, nph_dvec3(0.1, 5.0, 0.2), down, NULL, &hit));
    NTEST_EXPECT_NE_UINT(hit.body.value, box.value);
    NTEST_ASSERT_TRUE(
        nph_physics_world_raycast_closest(s.w, nph_dvec3(20.1, 5.0, 0.2), down, NULL, &hit));
    NTEST_EXPECT_EQ_UINT(hit.body.value, box.value);
    // Destroyed, it is gone.
    nph_physics_body_destroy(s.w, box);
    Count c = {0U};
    const NphPhysicsGeometry probe = nph_physics_geometry_sphere(0.6F);
    nph_physics_world_overlap_shape(s.w, &probe, nph_dvec3(20.0, 0.5, 0.0), nph_quat_identity(),
                                    NULL, count_shape, &c);
    NTEST_EXPECT_EQ_UINT(c.n, 1U); // the ground only
    scene_end(&s);
}

NTEST(query, a_resting_body_casts_along_its_floor)
{
    // A box and a capsule settled on the ground (a few millimetres in, as
    // the solver leaves them), cast along it and into it, ignoring
    // themselves: the controller's case.
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    const NphPhysicsGeometry shapes[2] = {nph_physics_geometry_box(nph_vec3_splat(0.4F)),
                                          nph_physics_geometry_capsule(0.5F, 0.3F)};
    // The capsule lying down, so it rests.
    const NphQuat turns[2] = {nph_quat_identity(),
                              nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.5F * k_pi)};
    NphPhysicsBody bodies[2];
    for (u32 k = 0U; k < 2U; ++k) {
        bodies[k] = add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(3.0 * (f64)k, 1.0, 0.0),
                             turns[k], shapes[k], 0.6F, 0.0F);
    }
    step(s.w, NULL, 360U);
    for (u32 k = 0U; k < 2U; ++k) {
        const NphDVec3 p = nph_physics_body_position(s.w, bodies[k]);
        const NphQuat q = nph_physics_body_rotation(s.w, bodies[k]);
        NphPhysicsQueryFilter f = nph_physics_query_filter_default();
        f.ignore = bodies[k];
        NphPhysicsQueryHit hit;
        NTEST_EXPECT_FALSE(nph_physics_world_cast_shape_closest(
            s.w, &shapes[k], p, q, nph_vec3(2.0F, 0.0F, 1.0F), &f, &hit));
        NTEST_ASSERT_TRUE(nph_physics_world_cast_shape_closest(
            s.w, &shapes[k], p, q, nph_vec3(1.0F, -0.1F, 0.0F), &f, &hit));
        NTEST_EXPECT_NEAR_F32(hit.fraction, 0.0F, 0.0F);
        NTEST_EXPECT_TRUE(nph_vec3_near(hit.normal, nph_vec3_unit_y(), 1e-3F));
        // Not ignoring itself, it starts inside itself: casts and rays
        // from inside a shape do not meet it.
        NTEST_EXPECT_FALSE(
            nph_physics_world_raycast_closest(s.w, p, nph_vec3(0.0F, 0.0F, 0.2F), NULL, &hit));
    }
    scene_end(&s);
}

typedef struct ParallelRays {
    const World *w;
    const NphDVec3 *origins;
    const NphVec3 *translations;
    NphPhysicsQueryHit *hits;
    bool *found;
} ParallelRays;

static void ray_range(void *user, u32 begin, u32 end)
{
    const ParallelRays *p = (const ParallelRays *)user;
    for (u32 i = begin; i < end; ++i) {
        p->found[i] = nph_physics_world_raycast_closest(p->w->s.w, p->origins[i],
                                                        p->translations[i], NULL, &p->hits[i]);
    }
}

#define PARALLEL_RAYS 2048U

NTEST(query, queries_run_on_many_threads_at_once)
{
    World w;
    world_build(&w, nph_dvec3_zero(), 213U);
    NphPcg32 r = test_rng(214U);
    static NphDVec3 origins[PARALLEL_RAYS];
    static NphVec3 translations[PARALLEL_RAYS];
    static NphPhysicsQueryHit serial[PARALLEL_RAYS];
    static NphPhysicsQueryHit parallel[PARALLEL_RAYS];
    static bool found_serial[PARALLEL_RAYS];
    static bool found_parallel[PARALLEL_RAYS];
    for (u32 i = 0U; i < PARALLEL_RAYS; ++i) {
        random_motion(&r, w.at, &origins[i], &translations[i]);
    }
    const u64 hash = nph_physics_world_hash(w.s.w);
    ParallelRays p = {&w, origins, translations, serial, found_serial};
    ray_range(&p, 0U, PARALLEL_RAYS);
    NphJobSystem *js = make_jobs(4U);
    p.hits = parallel;
    p.found = found_parallel;
    NphJobCounter counter;
    nph_job_counter_init(&counter);
    NTEST_ASSERT_EQ_INT(nph_job_parallel_for(js, PARALLEL_RAYS, 32U, ray_range, &p,
                                             NPH_JOB_PRIORITY_NORMAL, &counter),
                        NPH_RESULT_OK);
    nph_job_wait(js, &counter);
    nph_job_system_destroy(js);
    for (u32 i = 0U; i < PARALLEL_RAYS; ++i) {
        NTEST_ASSERT_EQ_INT(found_parallel[i], found_serial[i]);
        if (found_serial[i]) {
            NTEST_EXPECT_EQ_UINT(parallel[i].shape.value, serial[i].shape.value);
            NTEST_EXPECT_ULP_F32(parallel[i].fraction, serial[i].fraction, 0U);
        }
    }
    // Queries only read.
    NTEST_EXPECT_EQ_UINT(nph_physics_world_hash(w.s.w), hash);
    NTEST_EXPECT_TRUE(nph_physics_world_validate(w.s.w));
    world_end(&w);
}
