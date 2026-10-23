// SPDX-License-Identifier: Apache-2.0
// The dynamic AABB tree: structure through random edits, box and ray
// queries and box casts against brute force, enlargement and rebuilds,
// category masks, growth, and determinism.

#include "test_physics_support.h"

#include <stdlib.h>

#define MAX_PROXIES 2048U

typedef struct Model {
    NphAABB boxes[MAX_PROXIES];
    u64 categories[MAX_PROXIES];
    u32 proxy[MAX_PROXIES];
    bool live[MAX_PROXIES];
} Model;

static NphAABB random_box(NphPcg32 *r, f32 world, f32 size)
{
    const NphVec3 c = test_vec3(r, -world, world);
    const NphVec3 e = test_vec3(r, 0.01F, size);
    return nph_aabb(nph_vec3_sub(c, e), nph_vec3_add(c, e));
}

static bool overlaps(NphAABB a, NphAABB b)
{
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y &&
           a.min.z <= b.max.z && b.min.z <= a.max.z;
}

typedef struct Hits {
    u32 count;
    u64 ids[MAX_PROXIES];
} Hits;

static bool collect(u32 proxy, u64 user_data, void *context)
{
    NPH_UNUSED(proxy);
    Hits *h = (Hits *)context;
    h->ids[h->count++] = user_data;
    return true;
}

static int compare_u64(const void *a, const void *b)
{
    const u64 x = *(const u64 *)a;
    const u64 y = *(const u64 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void sort_hits(Hits *h)
{
    if (h->count > 1U) {
        qsort(h->ids, h->count, sizeof(u64), compare_u64);
    }
}

// The tree's answer to a box query equals the brute-force one.
static void expect_query(const NphAabbTree *t, const Model *m, NphAABB q, u64 mask)
{
    static Hits got;
    static Hits want;
    got.count = 0U;
    want.count = 0U;
    nph_aabb_tree_query(t, q, mask, collect, &got);
    for (u32 i = 0U; i < MAX_PROXIES; ++i) {
        if (m->live[i] && (m->categories[i] & mask) != 0U && overlaps(m->boxes[i], q)) {
            want.ids[want.count++] = i;
        }
    }
    sort_hits(&got);
    sort_hits(&want);
    NTEST_ASSERT_EQ_UINT(got.count, want.count);
    NTEST_EXPECT_EQ_MEM(got.ids, want.ids, (u64)got.count * sizeof(u64));
}

NTEST(aabb_tree, random_edits_keep_queries_exact)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    static Model m;
    memset(&m, 0, sizeof m);
    NphAabbTree tree;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&tree, alloc, 4U), NPH_RESULT_OK);
    NphPcg32 rng = test_rng(41U);
    const u32 steps = 1500U * TEST_SCALE;
    for (u32 step = 0U; step < steps; ++step) {
        const u32 i = nph_pcg32_next_bounded(&rng, MAX_PROXIES);
        const u32 op = nph_pcg32_next_bounded(&rng, 4U);
        if (!m.live[i]) {
            m.boxes[i] = random_box(&rng, 50.0F, 2.0F);
            m.categories[i] = 1ULL << nph_pcg32_next_bounded(&rng, 4U);
            NTEST_ASSERT_EQ_INT(
                nph_aabb_tree_create_proxy(&tree, m.boxes[i], m.categories[i], i, &m.proxy[i]),
                NPH_RESULT_OK);
            m.live[i] = true;
        } else if (op == 0U) {
            nph_aabb_tree_destroy_proxy(&tree, m.proxy[i]);
            m.live[i] = false;
        } else if (op == 1U) {
            m.boxes[i] = random_box(&rng, 50.0F, 2.0F);
            nph_aabb_tree_move_proxy(&tree, m.proxy[i], m.boxes[i]);
        } else if (op == 2U) {
            // A small motion: the new box in place, no restructuring.
            const NphAABB moved = nph_aabb_translate(m.boxes[i], test_vec3(&rng, -0.3F, 0.3F));
            nph_aabb_tree_enlarge_proxy(&tree, m.proxy[i], moved);
            m.boxes[i] = moved;
        } else {
            m.categories[i] = 1ULL << nph_pcg32_next_bounded(&rng, 4U);
            nph_aabb_tree_set_category_bits(&tree, m.proxy[i], m.categories[i]);
        }
        if (step % 97U == 0U) {
            NTEST_ASSERT_TRUE(nph_aabb_tree_validate(&tree));
            expect_query(&tree, &m, random_box(&rng, 50.0F, 10.0F), ~0ULL);
            expect_query(&tree, &m, random_box(&rng, 50.0F, 10.0F), 0x5ULL);
        }
        if (step % 500U == 499U) {
            u32 leaves = 0U;
            NTEST_ASSERT_EQ_INT(nph_aabb_tree_rebuild(&tree, (step / 500U) % 2U == 0U, &leaves),
                                NPH_RESULT_OK);
            NTEST_ASSERT_TRUE(nph_aabb_tree_validate(&tree));
            expect_query(&tree, &m, random_box(&rng, 50.0F, 20.0F), ~0ULL);
        }
    }
    u32 live = 0U;
    for (u32 i = 0U; i < MAX_PROXIES; ++i) {
        live += m.live[i] ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(tree.proxy_count, live);
    nph_aabb_tree_free(&tree);
    test_alloc_end(&ta);
}

typedef struct RayHits {
    u32 count;
    u64 ids[MAX_PROXIES];
    const Model *model;
    bool closest;
    f32 best_t;
    u64 best_id;
} RayHits;

static f32 ray_entry(NphVec3 o, NphVec3 d, NphAABB b, f32 max_t, bool *hit)
{
    f32 t0 = 0.0F;
    *hit = nph_ray_intersects_aabb(nph_ray(o, d), b, max_t, &t0, NULL);
    return t0;
}

static f32 ray_collect(const NphAabbTreeRay *ray, u32 proxy, u64 user_data, void *context)
{
    NPH_UNUSED(proxy);
    RayHits *h = (RayHits *)context;
    if (!h->closest) {
        h->ids[h->count++] = user_data;
        return ray->max_fraction;
    }
    bool hit = false;
    const f32 t = ray_entry(ray->origin, ray->translation, h->model->boxes[user_data],
                            ray->max_fraction, &hit);
    if (hit && t < h->best_t) {
        h->best_t = t;
        h->best_id = user_data;
        return t;
    }
    return ray->max_fraction;
}

NTEST(aabb_tree, raycasts_match_brute_force)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    static Model m;
    memset(&m, 0, sizeof m);
    NphAabbTree tree;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&tree, alloc, 0U), NPH_RESULT_OK);
    NphPcg32 rng = test_rng(43U);
    for (u32 i = 0U; i < 1000U; ++i) {
        m.boxes[i] = random_box(&rng, 30.0F, 1.5F);
        m.categories[i] = 1U;
        m.live[i] = true;
        NTEST_ASSERT_EQ_INT(nph_aabb_tree_create_proxy(&tree, m.boxes[i], 1U, i, &m.proxy[i]),
                            NPH_RESULT_OK);
    }
    static RayHits got;
    for (u32 k = 0U; k < 100U; ++k) {
        NphAabbTreeRay ray;
        ray.origin = test_vec3(&rng, -40.0F, 40.0F);
        ray.translation = nph_vec3_scale(test_unit(&rng), 80.0F);
        ray.max_fraction = test_range(&rng, 0.2F, 1.0F);
        // Every box the ray enters, in any order.
        got.count = 0U;
        got.closest = false;
        got.model = &m;
        nph_aabb_tree_raycast(&tree, &ray, ~0ULL, ray_collect, &got);
        static Hits want;
        want.count = 0U;
        f32 best_t = 1e30F;
        u64 best_id = NPH_U64_MAX;
        for (u32 i = 0U; i < 1000U; ++i) {
            bool hit = false;
            const f32 t =
                ray_entry(ray.origin, ray.translation, m.boxes[i], ray.max_fraction, &hit);
            if (hit) {
                want.ids[want.count++] = i;
                if (t < best_t) {
                    best_t = t;
                    best_id = i;
                }
            }
        }
        Hits g;
        g.count = got.count;
        memcpy(g.ids, got.ids, (size_t)got.count * sizeof(u64));
        sort_hits(&g);
        sort_hits(&want);
        NTEST_ASSERT_EQ_UINT(g.count, want.count);
        NTEST_EXPECT_EQ_MEM(g.ids, want.ids, (u64)g.count * sizeof(u64));
        // The closest box, clipping the ray as hits come in.
        got.closest = true;
        got.best_t = 1e30F;
        got.best_id = NPH_U64_MAX;
        nph_aabb_tree_raycast(&tree, &ray, ~0ULL, ray_collect, &got);
        if (best_id != NPH_U64_MAX) {
            NTEST_EXPECT_NEAR_F32(got.best_t, best_t, 1e-6F);
        } else {
            NTEST_EXPECT_EQ_UINT(got.best_id, NPH_U64_MAX);
        }
    }
    nph_aabb_tree_free(&tree);
    test_alloc_end(&ta);
}

// Every leaf reached, without clipping.
static f32 cast_collect(const NphAabbTreeRay *ray, u32 proxy, u64 user_data, void *context)
{
    NPH_UNUSED(proxy);
    Hits *h = (Hits *)context;
    h->ids[h->count++] = user_data;
    return ray->max_fraction;
}

NTEST(aabb_tree, box_casts_meet_every_box_the_swept_box_meets)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    static Model m;
    memset(&m, 0, sizeof m);
    NphAabbTree tree;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&tree, alloc, 0U), NPH_RESULT_OK);
    NphPcg32 rng = test_rng(47U);
    for (u32 i = 0U; i < 1000U; ++i) {
        m.boxes[i] = random_box(&rng, 30.0F, 1.5F);
        m.categories[i] = 1ULL << (i % 3U);
        NTEST_ASSERT_EQ_INT(
            nph_aabb_tree_create_proxy(&tree, m.boxes[i], m.categories[i], i, &m.proxy[i]),
            NPH_RESULT_OK);
    }
    static Hits got;
    static Hits want;
    for (u32 k = 0U; k < 100U; ++k) {
        NphAabbTreeRay ray;
        ray.origin = test_vec3(&rng, -40.0F, 40.0F);
        ray.translation = nph_vec3_scale(test_unit(&rng), test_range(&rng, 1.0F, 80.0F));
        ray.max_fraction = test_range(&rng, 0.2F, 1.0F);
        // A point, a thin box, and boxes up to 3 m across.
        const NphVec3 extent = k % 5U == 0U ? nph_vec3_zero()
                                            : nph_vec3_mul(test_vec3(&rng, 0.0F, 1.5F),
                                                           k % 5U == 1U ? nph_vec3(1.0F, 0.0F, 1.0F)
                                                                        : nph_vec3_splat(1.0F));
        const u64 mask = k % 4U == 0U ? 2ULL : ~0ULL;
        got.count = 0U;
        nph_aabb_tree_box_cast(&tree, &ray, extent, mask, cast_collect, &got);
        want.count = 0U;
        for (u32 i = 0U; i < 1000U; ++i) {
            const NphAABB grown = {nph_vec3_sub(m.boxes[i].min, extent),
                                   nph_vec3_add(m.boxes[i].max, extent)};
            if ((m.categories[i] & mask) != 0U &&
                nph_ray_intersects_aabb(nph_ray(ray.origin, ray.translation), grown,
                                        ray.max_fraction, NULL, NULL)) {
                want.ids[want.count++] = i;
            }
        }
        sort_hits(&got);
        sort_hits(&want);
        NTEST_ASSERT_EQ_UINT(got.count, want.count);
        NTEST_EXPECT_EQ_MEM(got.ids, want.ids, (u64)got.count * sizeof(u64));
        // A zero extent walks as the raycast does.
        if (k % 5U == 0U) {
            static Hits rays;
            rays.count = 0U;
            nph_aabb_tree_raycast(&tree, &ray, mask, cast_collect, &rays);
            sort_hits(&rays);
            NTEST_ASSERT_EQ_UINT(rays.count, got.count);
            NTEST_EXPECT_EQ_MEM(rays.ids, got.ids, (u64)got.count * sizeof(u64));
        }
    }
    nph_aabb_tree_free(&tree);
    test_alloc_end(&ta);
}

NTEST(aabb_tree, rebuild_restores_quality)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphAabbTree tree;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&tree, alloc, 0U), NPH_RESULT_OK);
    // Inserted in sorted order along a line: the worst case for incremental
    // insertion.
    for (u32 i = 0U; i < 1024U; ++i) {
        const f32 x = (f32)i;
        const NphAABB b = nph_aabb(nph_vec3(x, 0.0F, 0.0F), nph_vec3(x + 0.9F, 1.0F, 1.0F));
        u32 proxy = 0U;
        NTEST_ASSERT_EQ_INT(nph_aabb_tree_create_proxy(&tree, b, 1U, i, &proxy), NPH_RESULT_OK);
    }
    const u32 height = nph_aabb_tree_height(&tree);
    const f32 ratio = nph_aabb_tree_area_ratio(&tree);
    // Rotations keep even this order far from a 1024-deep chain.
    NTEST_LOG("incremental height %u, area ratio %.2f", height, (f64)ratio);
    NTEST_EXPECT_TRUE(height <= 64U);
    u32 leaves = 0U;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_rebuild(&tree, true, &leaves), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(leaves, 1024U);
    NTEST_ASSERT_TRUE(nph_aabb_tree_validate(&tree));
    NTEST_EXPECT_TRUE(nph_aabb_tree_area_ratio(&tree) <= ratio * 1.0001F);
    NTEST_EXPECT_TRUE(nph_aabb_tree_height(&tree) <= 12U);
    // Nothing enlarged: a partial rebuild does nothing.
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_rebuild(&tree, false, &leaves), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(leaves, 0U);
    nph_aabb_tree_free(&tree);
    test_alloc_end(&ta);
}

NTEST(aabb_tree, partial_rebuild_touches_only_enlarged_subtrees)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphAabbTree tree;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&tree, alloc, 0U), NPH_RESULT_OK);
    NphPcg32 rng = test_rng(47U);
    static u32 proxies[512];
    for (u32 i = 0U; i < 512U; ++i) {
        NTEST_ASSERT_EQ_INT(
            nph_aabb_tree_create_proxy(&tree, random_box(&rng, 100.0F, 1.0F), 1U, i, &proxies[i]),
            NPH_RESULT_OK);
    }
    u32 leaves = 0U;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_rebuild(&tree, true, &leaves), NPH_RESULT_OK);
    // Enlarge three proxies: the rebuild places far fewer items than 512.
    for (u32 k = 0U; k < 3U; ++k) {
        const u32 p = proxies[(u64)k * 100U];
        const NphAABB grown = nph_aabb_grow(nph_aabb_tree_proxy_aabb(&tree, p), 2.0F);
        nph_aabb_tree_enlarge_proxy(&tree, p, grown);
    }
    NTEST_ASSERT_TRUE(nph_aabb_tree_validate(&tree));
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_rebuild(&tree, false, &leaves), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(leaves > 0U && leaves < 100U);
    NTEST_ASSERT_TRUE(nph_aabb_tree_validate(&tree));
    nph_aabb_tree_free(&tree);
    test_alloc_end(&ta);
}

NTEST(aabb_tree, identical_edits_build_identical_trees)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphAabbTree a;
    NphAabbTree b;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&a, alloc, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&b, alloc, 0U), NPH_RESULT_OK);
    for (u32 pass = 0U; pass < 2U; ++pass) {
        NphAabbTree *t = pass == 0U ? &a : &b;
        NphPcg32 rng = test_rng(53U);
        static u32 proxies[300];
        for (u32 i = 0U; i < 300U; ++i) {
            NTEST_ASSERT_EQ_INT(
                nph_aabb_tree_create_proxy(t, random_box(&rng, 20.0F, 1.0F), 1U, i, &proxies[i]),
                NPH_RESULT_OK);
        }
        for (u32 i = 0U; i < 300U; i += 3U) {
            nph_aabb_tree_move_proxy(t, proxies[i], random_box(&rng, 20.0F, 1.0F));
        }
        for (u32 i = 1U; i < 300U; i += 7U) {
            nph_aabb_tree_destroy_proxy(t, proxies[i]);
        }
        u32 leaves = 0U;
        NTEST_ASSERT_EQ_INT(nph_aabb_tree_rebuild(t, true, &leaves), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_UINT(a.node_capacity, b.node_capacity);
    NTEST_EXPECT_EQ_UINT(a.root, b.root);
    NTEST_EXPECT_EQ_MEM(a.nodes, b.nodes, (u64)a.node_capacity * sizeof(NphAabbTreeNode));
    nph_aabb_tree_free(&a);
    nph_aabb_tree_free(&b);
    test_alloc_end(&ta);
}

NTEST(aabb_tree, empty_and_single)
{
    TestAlloc ta;
    NphAllocator *alloc = test_alloc_begin(&ta);
    NphAabbTree tree;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_init(&tree, alloc, 0U), NPH_RESULT_OK);
    static Hits hits;
    hits.count = 0U;
    nph_aabb_tree_query(&tree, nph_aabb(nph_vec3_splat(-1.0F), nph_vec3_one()), ~0ULL, collect,
                        &hits);
    NTEST_EXPECT_EQ_UINT(hits.count, 0U);
    NTEST_EXPECT_EQ_UINT(nph_aabb_tree_height(&tree), 0U);
    NTEST_EXPECT_TRUE(nph_aabb_tree_validate(&tree));
    u32 proxy = 0U;
    NTEST_ASSERT_EQ_INT(nph_aabb_tree_create_proxy(&tree, nph_aabb(nph_vec3_zero(), nph_vec3_one()),
                                                   2U, 77U, &proxy),
                        NPH_RESULT_OK);
    nph_aabb_tree_query(&tree, nph_aabb(nph_vec3_splat(0.5F), nph_vec3_splat(2.0F)), 2U, collect,
                        &hits);
    NTEST_ASSERT_EQ_UINT(hits.count, 1U);
    NTEST_EXPECT_EQ_UINT(hits.ids[0], 77U);
    NTEST_EXPECT_EQ_UINT(nph_aabb_tree_proxy_user_data(&tree, proxy), 77U);
    // A mask without its category finds nothing.
    hits.count = 0U;
    nph_aabb_tree_query(&tree, nph_aabb(nph_vec3_splat(0.5F), nph_vec3_splat(2.0F)), 1U, collect,
                        &hits);
    NTEST_EXPECT_EQ_UINT(hits.count, 0U);
    // Bad boxes are refused.
    const NphAABB inverted = nph_aabb(nph_vec3_one(), nph_vec3_zero());
    NTEST_EXPECT_EQ_INT(nph_aabb_tree_create_proxy(&tree, inverted, 1U, 0U, &proxy),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_aabb_tree_free(&tree);
    test_alloc_end(&ta);
}
