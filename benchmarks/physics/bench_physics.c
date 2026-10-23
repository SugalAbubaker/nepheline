// SPDX-License-Identifier: Apache-2.0
// Physics benchmarks (CLAUDE.md §1.5, §8.9; docs/physics.md "Performance").
//
// The collision layer, per call: each row cycles through 64 prepared
// configurations so branch predictors and caches see a realistic mix rather
// than one memorised pair.
//
//   hull_build_1000_64v          Quickhull of 1000 random points, 64-vertex budget
//   hull_build_box_8             Quickhull of a box's 8 corners
//   collide_spheres              sphere-sphere, overlapping
//   collide_capsules             capsule-capsule, random poses near contact
//   collide_box_sphere           box-sphere, random poses near contact
//   collide_box_box_resting      a box resting on a box (four points)
//   collide_box_box_random       boxes in random overlapping poses
//   collide_box_box_cached_apart separated boxes ended by the cached axis
//   collide_hull_hull_32v        two 32-vertex hulls, random overlapping poses
//   collide_hull_capsule_32v     a 32-vertex hull and a capsule near contact
//   gjk_hull_hull_32v            GJK distance between separated 32-vertex hulls
//   gjk_hull_hull_32v_cached     the same, warm-started from the last simplex
//   epa_box_hull_32v             penetration (GJK, then EPA) of overlapping pairs
//   tree_insert_10k              one proxy into a 10k-proxy tree (insert + remove)
//   tree_move_10k                one proxy moved in a 10k-proxy tree
//   tree_query_10k               a 2 m box query in a 10k-proxy tree
//   tree_raycast_10k             a 50 m closest-hit ray in a 10k-proxy tree
//   tree_rebuild_10k             a full binned-SAH rebuild of 10k proxies (per proxy)
//   mesh_build_grid_128          a 128 x 128 grid (32768 triangles): welding,
//                                active edges, the tree (per triangle)
//   mesh_raycast_grid_128        a closest-hit ray down onto the grid
//   mesh_overlap_grid_128        the triangles under a 1 m box on the grid
//   collide_mesh_box             a box resting on a grid of 0.5 m cells, anywhere
//                                and turned any way (each call: its triangles,
//                                merged into one manifold)
//   collide_mesh_sphere          a ball, the same
//   collide_mesh_capsule         a capsule lying on the grid, the same
//   collide_mesh_hull_32v        a 32-vertex hull, the same
//   heightfield_build_256        a 257 x 257 heightfield: active edges and
//                                blocks (per cell)
//   heightfield_raycast_256      a closest-hit ray down onto it
//   heightfield_raycast_256_long a ray 100 m across it, low and falling
//   heightfield_overlap_256      the triangles under a 1 m box on it
//   collide_heightfield_box      the collide_mesh_ rows' scenes on a flat
//   collide_heightfield_sphere   heightfield of 0.5 m cells
//   collide_heightfield_capsule
//   collide_heightfield_hull_32v

#include <nepheline/math/random.h>
#include <nepheline/physics/aabb_tree.h>
#include <nepheline/physics/collide.h>
#include <nepheline/physics/heightfield.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/mesh.h>

#include <nbench/nbench.h>

#include <string.h>

#define PAIRS        64U
#define TREE_PROXIES 10000U

static NphPcg32 bench_rng(u64 seed)
{
    NphPcg32 r;
    nph_pcg32_seed(&r, seed, 7U);
    return r;
}

static NphVec3 rand_vec3(NphPcg32 *r, f32 lo, f32 hi)
{
    return nph_vec3(nph_pcg32_next_range_f32(r, lo, hi), nph_pcg32_next_range_f32(r, lo, hi),
                    nph_pcg32_next_range_f32(r, lo, hi));
}

static NphVec3 rand_unit(NphPcg32 *r)
{
    return nph_sample_sphere_uniform(nph_vec2(nph_pcg32_next_f32(r), nph_pcg32_next_f32(r)));
}

static NphQuat rand_rotation(NphPcg32 *r)
{
    const f32 u1 = nph_pcg32_next_f32(r);
    const f32 u2 = nph_pcg32_next_f32(r) * 6.2831853F;
    const f32 u3 = nph_pcg32_next_f32(r) * 6.2831853F;
    const f32 a = nph_f32_sqrt(1.0F - u1);
    const f32 b = nph_f32_sqrt(u1);
    return nph_quat_normalize(nph_quat(a * nph_f32_sin(u2), a * nph_f32_cos(u2),
                                       b * nph_f32_sin(u3), b * nph_f32_cos(u3)));
}

static NphPhysicsHull *make_hull(NphPcg32 *r, u32 points, u32 budget)
{
    NphVec3 pts[1000];
    for (u32 i = 0U; i < points; ++i) {
        pts[i] = nph_vec3_scale(rand_unit(r), nph_pcg32_next_range_f32(r, 0.7F, 1.0F));
    }
    NphPhysicsHullDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.points = pts;
    desc.point_count = points;
    desc.max_vertices = budget;
    NphPhysicsHull *hull = NULL;
    if (nph_physics_hull_build(&desc, nph_allocator_system(), &hull) != NPH_RESULT_OK) {
        return NULL;
    }
    return hull;
}

// Pose pairs at a centre distance in [lo, hi]: contact or near it.
static void make_pairs(NphPcg32 *r, f32 lo, f32 hi, NphPhysicsPose a[PAIRS],
                       NphPhysicsPose b[PAIRS])
{
    for (u32 i = 0U; i < PAIRS; ++i) {
        a[i] = nph_physics_pose(rand_vec3(r, -0.1F, 0.1F), rand_rotation(r));
        b[i] = nph_physics_pose(
            nph_vec3_add(a[i].position,
                         nph_vec3_scale(rand_unit(r), nph_pcg32_next_range_f32(r, lo, hi))),
            rand_rotation(r));
    }
}

static void bench_collide(NbenchState *state, const NphPhysicsGeometry *ga,
                          const NphPhysicsGeometry *gb, const NphPhysicsPose *pa,
                          const NphPhysicsPose *pb, bool cache)
{
    NphPhysicsCollideCache caches[PAIRS];
    memset(caches, 0, sizeof caches);
    NphPhysicsManifold m;
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            nph_physics_collide(ga, pa[i], gb, pb[i], NPH_PHYSICS_SPECULATIVE_DISTANCE,
                                cache ? &caches[i] : NULL, &m);
            nbench_do_not_optimize(&m);
        }
    }
}

NBENCH(hull_build_1000_64v)
{
    NphPcg32 r = bench_rng(1U);
    static NphVec3 pts[1000];
    for (u32 i = 0U; i < 1000U; ++i) {
        pts[i] = nph_vec3_scale(rand_unit(&r), nph_pcg32_next_range_f32(&r, 0.0F, 1.0F));
    }
    NphPhysicsHullDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.points = pts;
    desc.point_count = 1000U;
    desc.max_vertices = 64U;
    while (nbench_keep_running(state)) {
        NphPhysicsHull *hull = NULL;
        if (nph_physics_hull_build(&desc, nph_allocator_system(), &hull) == NPH_RESULT_OK) {
            nbench_do_not_optimize(hull);
            nph_physics_hull_free(hull, nph_allocator_system());
        }
    }
}

NBENCH(hull_build_box_8)
{
    NphVec3 pts[8];
    for (u32 i = 0U; i < 8U; ++i) {
        pts[i] = nph_vec3((i & 1U) != 0U ? 1.0F : -1.0F, (i & 2U) != 0U ? 1.0F : -1.0F,
                          (i & 4U) != 0U ? 1.0F : -1.0F);
    }
    NphPhysicsHullDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.points = pts;
    desc.point_count = 8U;
    while (nbench_keep_running(state)) {
        NphPhysicsHull *hull = NULL;
        if (nph_physics_hull_build(&desc, nph_allocator_system(), &hull) == NPH_RESULT_OK) {
            nbench_do_not_optimize(hull);
            nph_physics_hull_free(hull, nph_allocator_system());
        }
    }
}

NBENCH(collide_spheres)
{
    NphPcg32 r = bench_rng(2U);
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 0.5F, 1.0F, pa, pb);
    const NphPhysicsGeometry g = nph_physics_geometry_sphere(0.5F);
    bench_collide(state, &g, &g, pa, pb, false);
}

NBENCH(collide_capsules)
{
    NphPcg32 r = bench_rng(3U);
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 0.3F, 1.2F, pa, pb);
    const NphPhysicsGeometry g = nph_physics_geometry_capsule(0.5F, 0.25F);
    bench_collide(state, &g, &g, pa, pb, false);
}

NBENCH(collide_box_sphere)
{
    NphPcg32 r = bench_rng(4U);
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 0.5F, 1.0F, pa, pb);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    const NphPhysicsGeometry ball = nph_physics_geometry_sphere(0.4F);
    bench_collide(state, &box, &ball, pa, pb, false);
}

NBENCH(collide_box_box_resting)
{
    NphPcg32 r = bench_rng(5U);
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        pa[i] = nph_physics_pose_identity();
        pb[i] =
            nph_physics_pose(nph_vec3(nph_pcg32_next_range_f32(&r, -0.2F, 0.2F), 0.995F,
                                      nph_pcg32_next_range_f32(&r, -0.2F, 0.2F)),
                             nph_quat_from_rotation_y(nph_pcg32_next_range_f32(&r, 0.0F, 1.5F)));
    }
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    bench_collide(state, &box, &box, pa, pb, true);
}

NBENCH(collide_box_box_random)
{
    NphPcg32 r = bench_rng(6U);
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 0.6F, 1.2F, pa, pb);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    bench_collide(state, &box, &box, pa, pb, false);
}

NBENCH(collide_box_box_cached_apart)
{
    NphPcg32 r = bench_rng(7U);
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 2.0F, 2.5F, pa, pb);
    const NphPhysicsGeometry box = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    bench_collide(state, &box, &box, pa, pb, true);
}

NBENCH(collide_hull_hull_32v)
{
    NphPcg32 r = bench_rng(8U);
    NphPhysicsHull *ha = make_hull(&r, 200U, 32U);
    NphPhysicsHull *hb = make_hull(&r, 200U, 32U);
    if (ha == NULL || hb == NULL) {
        return;
    }
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 1.2F, 1.9F, pa, pb);
    const NphPhysicsGeometry ga = nph_physics_geometry_hull(ha);
    const NphPhysicsGeometry gb = nph_physics_geometry_hull(hb);
    bench_collide(state, &ga, &gb, pa, pb, true);
    nph_physics_hull_free(ha, nph_allocator_system());
    nph_physics_hull_free(hb, nph_allocator_system());
}

NBENCH(collide_hull_capsule_32v)
{
    NphPcg32 r = bench_rng(9U);
    NphPhysicsHull *ha = make_hull(&r, 200U, 32U);
    if (ha == NULL) {
        return;
    }
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 0.8F, 1.6F, pa, pb);
    const NphPhysicsGeometry ga = nph_physics_geometry_hull(ha);
    const NphPhysicsGeometry gb = nph_physics_geometry_capsule(0.5F, 0.3F);
    bench_collide(state, &ga, &gb, pa, pb, false);
    nph_physics_hull_free(ha, nph_allocator_system());
}

static void bench_gjk(NbenchState *state, bool cached)
{
    NphPcg32 r = bench_rng(10U);
    NphPhysicsHull *ha = make_hull(&r, 200U, 32U);
    NphPhysicsHull *hb = make_hull(&r, 200U, 32U);
    if (ha == NULL || hb == NULL) {
        return;
    }
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 2.2F, 3.0F, pa, pb);
    const NphPhysicsGeometry ga = nph_physics_geometry_hull(ha);
    const NphPhysicsGeometry gb = nph_physics_geometry_hull(hb);
    NphVec3 sa[8];
    NphVec3 sb[8];
    const NphPhysicsProxy xa = nph_physics_geometry_proxy(&ga, sa);
    const NphPhysicsProxy xb = nph_physics_geometry_proxy(&gb, sb);
    NphPhysicsSimplexCache caches[PAIRS];
    memset(caches, 0, sizeof caches);
    NphPhysicsDistance d;
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            nph_physics_distance(&xa, pa[i], &xb, pb[i], true, cached ? &caches[i] : NULL, &d);
            nbench_do_not_optimize(&d);
        }
    }
    nph_physics_hull_free(ha, nph_allocator_system());
    nph_physics_hull_free(hb, nph_allocator_system());
}

NBENCH(gjk_hull_hull_32v)
{
    bench_gjk(state, false);
}

NBENCH(gjk_hull_hull_32v_cached)
{
    bench_gjk(state, true);
}

NBENCH(epa_box_hull_32v)
{
    NphPcg32 r = bench_rng(11U);
    NphPhysicsHull *hb = make_hull(&r, 200U, 32U);
    if (hb == NULL) {
        return;
    }
    NphPhysicsPose pa[PAIRS];
    NphPhysicsPose pb[PAIRS];
    make_pairs(&r, 0.3F, 0.9F, pa, pb);
    const NphPhysicsGeometry ga = nph_physics_geometry_box(nph_vec3(0.5F, 0.5F, 0.5F));
    const NphPhysicsGeometry gb = nph_physics_geometry_hull(hb);
    NphVec3 sa[8];
    NphVec3 sb[8];
    const NphPhysicsProxy xa = nph_physics_geometry_proxy(&ga, sa);
    const NphPhysicsProxy xb = nph_physics_geometry_proxy(&gb, sb);
    NphPhysicsPenetration p;
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            const bool hit = nph_physics_penetration(&xa, pa[i], &xb, pb[i], &p);
            nbench_do_not_optimize(&p);
            nbench_do_not_optimize_u64(hit ? 1U : 0U);
        }
    }
    nph_physics_hull_free(hb, nph_allocator_system());
}

// ---------------------------------------------------------------------------
// The tree
// ---------------------------------------------------------------------------

static NphAABB rand_box(NphPcg32 *r)
{
    const NphVec3 c = rand_vec3(r, -200.0F, 200.0F);
    const NphVec3 e = rand_vec3(r, 0.2F, 1.0F);
    return nph_aabb(nph_vec3_sub(c, e), nph_vec3_add(c, e));
}

static bool fill_tree(NphAabbTree *t, NphPcg32 *r, u32 *proxies)
{
    if (nph_aabb_tree_init(t, nph_allocator_system(), 2U * TREE_PROXIES) != NPH_RESULT_OK) {
        return false;
    }
    for (u32 i = 0U; i < TREE_PROXIES; ++i) {
        if (nph_aabb_tree_create_proxy(t, rand_box(r), 1U, i, &proxies[i]) != NPH_RESULT_OK) {
            return false;
        }
    }
    return true;
}

NBENCH(tree_insert_10k)
{
    NphPcg32 r = bench_rng(12U);
    static u32 proxies[TREE_PROXIES];
    NphAabbTree tree;
    if (!fill_tree(&tree, &r, proxies)) {
        return;
    }
    NphAABB boxes[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        boxes[i] = rand_box(&r);
    }
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        u32 ids[PAIRS];
        for (u32 i = 0U; i < PAIRS; ++i) {
            if (nph_aabb_tree_create_proxy(&tree, boxes[i], 1U, 0U, &ids[i]) != NPH_RESULT_OK) {
                ids[i] = NPH_AABB_TREE_NULL;
            }
        }
        for (u32 i = 0U; i < PAIRS; ++i) {
            if (ids[i] != NPH_AABB_TREE_NULL) {
                nph_aabb_tree_destroy_proxy(&tree, ids[i]);
            }
        }
    }
    nph_aabb_tree_free(&tree);
}

NBENCH(tree_move_10k)
{
    NphPcg32 r = bench_rng(13U);
    static u32 proxies[TREE_PROXIES];
    NphAabbTree tree;
    if (!fill_tree(&tree, &r, proxies)) {
        return;
    }
    NphAABB boxes[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        boxes[i] = rand_box(&r);
    }
    u32 next = 0U;
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            nph_aabb_tree_move_proxy(&tree, proxies[next], boxes[i]);
            next = (next + 1U) % TREE_PROXIES;
        }
    }
    nph_aabb_tree_free(&tree);
}

static bool count_hit(u32 proxy, u64 user_data, void *context)
{
    NPH_UNUSED(proxy);
    NPH_UNUSED(user_data);
    *(u32 *)context += 1U;
    return true;
}

NBENCH(tree_query_10k)
{
    NphPcg32 r = bench_rng(14U);
    static u32 proxies[TREE_PROXIES];
    NphAabbTree tree;
    if (!fill_tree(&tree, &r, proxies)) {
        return;
    }
    u32 leaves = 0U;
    if (nph_aabb_tree_rebuild(&tree, true, &leaves) != NPH_RESULT_OK) {
        return;
    }
    NphAABB queries[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        const NphVec3 c = rand_vec3(&r, -200.0F, 200.0F);
        queries[i] = nph_aabb(nph_vec3_sub(c, nph_vec3_one()), nph_vec3_add(c, nph_vec3_one()));
    }
    u32 hits = 0U;
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            nph_aabb_tree_query(&tree, queries[i], ~0ULL, count_hit, &hits);
        }
    }
    nbench_do_not_optimize_u64(hits);
    nph_aabb_tree_free(&tree);
}

static f32 clip_ray(const NphAabbTreeRay *ray, u32 proxy, u64 user_data, void *context)
{
    NPH_UNUSED(proxy);
    NPH_UNUSED(user_data);
    NPH_UNUSED(context);
    // Every box counts as hit at its midpoint along the ray: the closest-hit
    // pattern's clipping.
    return ray->max_fraction * 0.5F;
}

NBENCH(tree_raycast_10k)
{
    NphPcg32 r = bench_rng(15U);
    static u32 proxies[TREE_PROXIES];
    NphAabbTree tree;
    if (!fill_tree(&tree, &r, proxies)) {
        return;
    }
    u32 leaves = 0U;
    if (nph_aabb_tree_rebuild(&tree, true, &leaves) != NPH_RESULT_OK) {
        return;
    }
    NphAabbTreeRay rays[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        rays[i].origin = rand_vec3(&r, -200.0F, 200.0F);
        rays[i].translation = nph_vec3_scale(rand_unit(&r), 50.0F);
        rays[i].max_fraction = 1.0F;
    }
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            nph_aabb_tree_raycast(&tree, &rays[i], ~0ULL, clip_ray, NULL);
        }
    }
    nph_aabb_tree_free(&tree);
}

NBENCH(tree_rebuild_10k)
{
    NphPcg32 r = bench_rng(16U);
    static u32 proxies[TREE_PROXIES];
    NphAabbTree tree;
    if (!fill_tree(&tree, &r, proxies)) {
        return;
    }
    nbench_set_items_per_iteration(state, TREE_PROXIES);
    while (nbench_keep_running(state)) {
        u32 leaves = 0U;
        if (nph_aabb_tree_rebuild(&tree, true, &leaves) != NPH_RESULT_OK) {
            break;
        }
        nbench_do_not_optimize_u64(leaves);
    }
    nph_aabb_tree_free(&tree);
}

// ---------------------------------------------------------------------------
// The world, per step (sleep off where the scene must stay awake)
//
//   world_pyramid_20_st      a 20-row pyramid (210 boxes) resting, one thread
//   world_pyramid_20_mt      the same on the job system (all cores)
//   world_pyramids_16_mt     sixteen of them side by side (3360 boxes)
//   world_pile_1000_st       1000 boxes, spheres, and capsules settled in a bin
//   world_pile_1000_mt       the same on the job system
//   world_mesh_pile_1000_st  the pile on a mesh floor (0.5 m cells), one thread
//   world_mesh_pile_1000_mt  the same on the job system
//   world_terrain_pile_1000_mt  the pile on a rippled mesh, on the job system
//   world_heightfield_pile_1000_mt  the same on the same surface as a
//                            heightfield
//   world_sleeping_10k       10k sleeping boxes and one awake ball
//   world_chains_st          16 chains of 25 ball-jointed links swinging, one thread
//   world_chains_mt          the same on the job system
//   world_ragdolls_32_st     32 ragdolls (352 bodies, 320 joints) lying in heaps, one thread
//   world_ragdolls_32_mt     the same on the job system
//   *_tgs, *_xpbd            the pyramid, the pile, the chains, and the ragdolls with
//                            the other solvers of the §13 comparison (ADR-0045)
//   *_soft8, *_xpbd16        the soft step at 8 substeps, XPBD at 16
// ---------------------------------------------------------------------------

#include <nepheline/jobs/jobs.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/joint.h>
#include <nepheline/physics/shape.h>
#include <nepheline/physics/world.h>

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

// An n by n grid of `cell` metre cells centred on the origin, facing +Y, its
// cells' diagonals alternating; rippled by `ripple` metres (0: flat).
static NphPhysicsMesh *bench_grid(u32 n, f32 cell, f32 ripple)
{
    const u32 nv = (n + 1U) * (n + 1U);
    NphVec3 *v = NPH_ALLOC_ARRAY(nph_allocator_system(), NphVec3, nv);
    u32 *idx = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, 6ULL * n * n);
    NphPhysicsMesh *mesh = NULL;
    if (v != NULL && idx != NULL) {
        const f32 x0 = -0.5F * cell * (f32)n;
        for (u32 i = 0U; i <= n; ++i) {
            for (u32 j = 0U; j <= n; ++j) {
                const f32 x = x0 + cell * (f32)i;
                const f32 z = x0 + cell * (f32)j;
                const f32 y = ripple * (nph_f32_sin(0.9F * x) * nph_f32_cos(0.7F * z) +
                                        0.25F * nph_f32_sin(3.1F * x + 1.3F * z));
                v[i * (n + 1U) + j] = nph_vec3(x, y, z);
            }
        }
        u32 k = 0U;
        for (u32 i = 0U; i < n; ++i) {
            for (u32 j = 0U; j < n; ++j) {
                const u32 a = i * (n + 1U) + j;
                const u32 quad[2][6] = {{a, a + 1U, a + n + 1U, a + n + 1U, a + 1U, a + n + 2U},
                                        {a, a + n + 2U, a + n + 1U, a, a + 1U, a + n + 2U}};
                for (u32 q = 0U; q < 6U; ++q) {
                    idx[k++] = quad[(i + j) & 1U][q];
                }
            }
        }
        NphPhysicsMeshDesc d;
        memset(&d, 0, sizeof d);
        d.vertices = v;
        d.vertex_count = nv;
        d.indices = idx;
        d.triangle_count = 2U * n * n;
        if (nph_physics_mesh_build(&d, nph_allocator_system(), &mesh) != NPH_RESULT_OK) {
            mesh = NULL;
        }
    }
    if (v != NULL) {
        NPH_FREE_ARRAY(nph_allocator_system(), NphVec3, v, nv);
    }
    if (idx != NULL) {
        NPH_FREE_ARRAY(nph_allocator_system(), u32, idx, 6ULL * n * n);
    }
    return mesh;
}

NBENCH(mesh_build_grid_128)
{
    while (nbench_keep_running(state)) {
        NphPhysicsMesh *mesh = bench_grid(128U, 0.5F, 0.3F);
        nbench_do_not_optimize(mesh);
        nph_physics_mesh_free(mesh, nph_allocator_system());
    }
    nbench_set_items_per_iteration(state, 2ULL * 128U * 128U);
}

NBENCH(mesh_raycast_grid_128)
{
    NphPhysicsMesh *mesh = bench_grid(128U, 0.5F, 0.3F);
    if (mesh == NULL) {
        return;
    }
    NphPcg32 r = bench_rng(31U);
    NphVec3 origins[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        origins[i] = nph_vec3(nph_pcg32_next_range_f32(&r, -30.0F, 30.0F), 5.0F,
                              nph_pcg32_next_range_f32(&r, -30.0F, 30.0F));
    }
    const NphVec3 delta = nph_vec3(0.3F, -10.0F, 0.2F);
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            NphPhysicsMeshHit hit;
            nbench_do_not_optimize_u64(
                nph_physics_mesh_raycast(mesh, origins[i], delta, &hit) ? 1U : 0U);
        }
    }
    nph_physics_mesh_free(mesh, nph_allocator_system());
}

static bool count_triangle(void *user, u32 triangle)
{
    *(u32 *)user += triangle != 0U ? 1U : 0U;
    return true;
}

NBENCH(mesh_overlap_grid_128)
{
    NphPhysicsMesh *mesh = bench_grid(128U, 0.5F, 0.3F);
    if (mesh == NULL) {
        return;
    }
    NphPcg32 r = bench_rng(32U);
    NphAABB boxes[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        const NphVec3 c = nph_vec3(nph_pcg32_next_range_f32(&r, -30.0F, 30.0F), 0.0F,
                                   nph_pcg32_next_range_f32(&r, -30.0F, 30.0F));
        boxes[i] =
            nph_aabb(nph_vec3_sub(c, nph_vec3_splat(0.5F)), nph_vec3_add(c, nph_vec3_splat(0.5F)));
    }
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            u32 n = 0U;
            nph_physics_mesh_overlap(mesh, boxes[i], count_triangle, &n);
            nbench_do_not_optimize_u64(n);
        }
    }
    nph_physics_mesh_free(mesh, nph_allocator_system());
}

// One shape resting on a flat 0.5 m grid, 64 places and headings.
static void bench_collide_mesh(NbenchState *state, const NphPhysicsGeometry *g, f32 rest,
                               NphQuat lie)
{
    NphPhysicsMesh *mesh = bench_grid(64U, 0.5F, 0.0F);
    if (mesh == NULL) {
        return;
    }
    NphPcg32 r = bench_rng(33U);
    NphPhysicsPose poses[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        poses[i] = nph_physics_pose(
            nph_vec3(nph_pcg32_next_range_f32(&r, -10.0F, 10.0F), rest - 0.001F,
                     nph_pcg32_next_range_f32(&r, -10.0F, 10.0F)),
            nph_quat_mul(nph_quat_from_rotation_y(nph_pcg32_next_range_f32(&r, 0.0F, 6.28F)), lie));
    }
    NphPhysicsMeshManifolds mm;
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            nph_physics_collide_mesh(g, poses[i], mesh, nph_physics_pose_identity(),
                                     NPH_PHYSICS_SPECULATIVE_DISTANCE, &mm);
            nbench_do_not_optimize(&mm);
        }
    }
    nph_physics_mesh_free(mesh, nph_allocator_system());
}

NBENCH(collide_mesh_box)
{
    const NphPhysicsGeometry g = nph_physics_geometry_box(nph_vec3(0.4F, 0.25F, 0.3F));
    bench_collide_mesh(state, &g, 0.25F, nph_quat_identity());
}

NBENCH(collide_mesh_sphere)
{
    const NphPhysicsGeometry g = nph_physics_geometry_sphere(0.3F);
    bench_collide_mesh(state, &g, 0.3F, nph_quat_identity());
}

NBENCH(collide_mesh_capsule)
{
    const NphPhysicsGeometry g = nph_physics_geometry_capsule(0.4F, 0.15F);
    bench_collide_mesh(state, &g, 0.15F, nph_quat_from_axis_angle(nph_vec3_unit_z(), 1.5707963F));
}

// The grid of bench_grid as a heightfield: (n + 1)^2 samples from its first
// at the origin (placed at -n cell / 2 to centre it), the cells' diagonals
// alternating as the mesh's.
static NphPhysicsHeightfield *bench_field(u32 n, f32 cell, f32 ripple)
{
    const u32 samples = (n + 1U) * (n + 1U);
    f32 *heights = NPH_ALLOC_ARRAY(nph_allocator_system(), f32, samples);
    u8 *cells = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, (u64)n * n);
    NphPhysicsHeightfield *h = NULL;
    if (heights != NULL && cells != NULL) {
        const f32 x0 = -0.5F * cell * (f32)n;
        for (u32 j = 0U; j <= n; ++j) {
            for (u32 i = 0U; i <= n; ++i) {
                const f32 x = x0 + cell * (f32)i;
                const f32 z = x0 + cell * (f32)j;
                heights[j * (n + 1U) + i] =
                    ripple * (nph_f32_sin(0.9F * x) * nph_f32_cos(0.7F * z) +
                              0.25F * nph_f32_sin(3.1F * x + 1.3F * z));
            }
        }
        for (u32 c = 0U; c < n * n; ++c) {
            cells[c] = ((c / n + c % n) & 1U) != 0U ? NPH_PHYSICS_HEIGHTFIELD_FLIP : 0U;
        }
        NphPhysicsHeightfieldDesc d;
        memset(&d, 0, sizeof d);
        d.heights = heights;
        d.columns = n + 1U;
        d.rows = n + 1U;
        d.spacing_x = cell;
        d.spacing_z = cell;
        d.cells = cells;
        if (nph_physics_heightfield_build(&d, nph_allocator_system(), &h) != NPH_RESULT_OK) {
            h = NULL;
        }
    }
    if (heights != NULL) {
        NPH_FREE_ARRAY(nph_allocator_system(), f32, heights, samples);
    }
    if (cells != NULL) {
        NPH_FREE_ARRAY(nph_allocator_system(), u8, cells, (u64)n * n);
    }
    return h;
}

NBENCH(heightfield_build_256)
{
    while (nbench_keep_running(state)) {
        NphPhysicsHeightfield *h = bench_field(256U, 0.5F, 0.3F);
        nbench_do_not_optimize(h);
        nph_physics_heightfield_free(h, nph_allocator_system());
    }
    nbench_set_items_per_iteration(state, 256ULL * 256U);
}

NBENCH(heightfield_raycast_256)
{
    NphPhysicsHeightfield *h = bench_field(256U, 0.5F, 0.3F);
    if (h == NULL) {
        return;
    }
    // As the mesh's: rays down onto the middle 60 m, in the field's frame.
    NphPcg32 r = bench_rng(31U);
    NphVec3 origins[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        origins[i] = nph_vec3(64.0F + nph_pcg32_next_range_f32(&r, -30.0F, 30.0F), 5.0F,
                              64.0F + nph_pcg32_next_range_f32(&r, -30.0F, 30.0F));
    }
    const NphVec3 delta = nph_vec3(0.3F, -10.0F, 0.2F);
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            NphPhysicsMeshHit hit;
            nbench_do_not_optimize_u64(
                nph_physics_heightfield_raycast(h, origins[i], delta, &hit) ? 1U : 0U);
        }
    }
    nph_physics_heightfield_free(h, nph_allocator_system());
}

NBENCH(heightfield_raycast_256_long)
{
    NphPhysicsHeightfield *h = bench_field(256U, 0.5F, 0.3F);
    if (h == NULL) {
        return;
    }
    // Low and long: 100 m across the field, a metre above the swells,
    // falling to meet them (the blocks pass over most of the way).
    NphPcg32 r = bench_rng(35U);
    NphVec3 origins[PAIRS];
    NphVec3 deltas[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        const f32 a = nph_pcg32_next_range_f32(&r, 0.0F, 6.2831853F);
        origins[i] = nph_vec3(64.0F + 50.0F * nph_f32_cos(a), 1.0F, 64.0F + 50.0F * nph_f32_sin(a));
        deltas[i] = nph_vec3(-100.0F * nph_f32_cos(a), -2.0F, -100.0F * nph_f32_sin(a));
    }
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            NphPhysicsMeshHit hit;
            nbench_do_not_optimize_u64(
                nph_physics_heightfield_raycast(h, origins[i], deltas[i], &hit) ? 1U : 0U);
        }
    }
    nph_physics_heightfield_free(h, nph_allocator_system());
}

NBENCH(heightfield_overlap_256)
{
    NphPhysicsHeightfield *h = bench_field(256U, 0.5F, 0.3F);
    if (h == NULL) {
        return;
    }
    NphPcg32 r = bench_rng(32U);
    NphAABB boxes[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        const NphVec3 c = nph_vec3(64.0F + nph_pcg32_next_range_f32(&r, -30.0F, 30.0F), 0.0F,
                                   64.0F + nph_pcg32_next_range_f32(&r, -30.0F, 30.0F));
        boxes[i] =
            nph_aabb(nph_vec3_sub(c, nph_vec3_splat(0.5F)), nph_vec3_add(c, nph_vec3_splat(0.5F)));
    }
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            u32 n = 0U;
            nph_physics_heightfield_overlap(h, boxes[i], count_triangle, &n);
            nbench_do_not_optimize_u64(n);
        }
    }
    nph_physics_heightfield_free(h, nph_allocator_system());
}

// One shape resting on a flat heightfield of 0.5 m cells, 64 places and
// headings: bench_collide_mesh's scene.
static void bench_collide_field(NbenchState *state, const NphPhysicsGeometry *g, f32 rest,
                                NphQuat lie)
{
    NphPhysicsHeightfield *h = bench_field(64U, 0.5F, 0.0F);
    if (h == NULL) {
        return;
    }
    NphPcg32 r = bench_rng(33U);
    NphPhysicsPose poses[PAIRS];
    for (u32 i = 0U; i < PAIRS; ++i) {
        poses[i] = nph_physics_pose(
            nph_vec3(nph_pcg32_next_range_f32(&r, -10.0F, 10.0F), rest - 0.001F,
                     nph_pcg32_next_range_f32(&r, -10.0F, 10.0F)),
            nph_quat_mul(nph_quat_from_rotation_y(nph_pcg32_next_range_f32(&r, 0.0F, 6.28F)), lie));
    }
    const NphPhysicsPose at = nph_physics_pose(nph_vec3(-16.0F, 0.0F, -16.0F), nph_quat_identity());
    NphPhysicsMeshManifolds mm;
    nbench_set_items_per_iteration(state, PAIRS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0U; i < PAIRS; ++i) {
            nph_physics_collide_heightfield(g, poses[i], h, at, NPH_PHYSICS_SPECULATIVE_DISTANCE,
                                            &mm);
            nbench_do_not_optimize(&mm);
        }
    }
    nph_physics_heightfield_free(h, nph_allocator_system());
}

NBENCH(collide_heightfield_box)
{
    const NphPhysicsGeometry g = nph_physics_geometry_box(nph_vec3(0.4F, 0.25F, 0.3F));
    bench_collide_field(state, &g, 0.25F, nph_quat_identity());
}

NBENCH(collide_heightfield_sphere)
{
    const NphPhysicsGeometry g = nph_physics_geometry_sphere(0.3F);
    bench_collide_field(state, &g, 0.3F, nph_quat_identity());
}

NBENCH(collide_heightfield_capsule)
{
    const NphPhysicsGeometry g = nph_physics_geometry_capsule(0.4F, 0.15F);
    bench_collide_field(state, &g, 0.15F, nph_quat_from_axis_angle(nph_vec3_unit_z(), 1.5707963F));
}

NBENCH(collide_heightfield_hull_32v)
{
    NphPcg32 r = bench_rng(34U);
    NphPhysicsHull *hull = make_hull(&r, 200U, 32U);
    if (hull == NULL) {
        return;
    }
    const NphPhysicsGeometry g = nph_physics_geometry_hull(hull);
    const NphVec3 low = nph_physics_geometry_support(&g, nph_vec3(0.0F, -1.0F, 0.0F));
    bench_collide_field(state, &g, -low.y, nph_quat_identity());
    nph_physics_hull_free(hull, nph_allocator_system());
}

NBENCH(collide_mesh_hull_32v)
{
    NphPcg32 r = bench_rng(34U);
    NphPhysicsHull *h = make_hull(&r, 200U, 32U);
    if (h == NULL) {
        return;
    }
    // Resting on its lowest vertex or face, wherever that is.
    const NphPhysicsGeometry g = nph_physics_geometry_hull(h);
    const NphVec3 low = nph_physics_geometry_support(&g, nph_vec3(0.0F, -1.0F, 0.0F));
    bench_collide_mesh(state, &g, -low.y, nph_quat_identity());
    nph_physics_hull_free(h, nph_allocator_system());
}

#define WORLD_DT (1.0F / 240.0F)

// Debug builds run these as CTest's smoke test, under the address
// sanitizer: the scenes shrink tenfold (their numbers are only recorded
// from Profile builds).
#if NPH_CONFIG_DEBUG
#define WORLD_SCALE  10U
#define PYRAMID_ROWS 10U
#else
#define WORLD_SCALE  1U
#define PYRAMID_ROWS 20U
#endif

static NphJobSystem *bench_jobs(void)
{
    static NphJobSystem *js = NULL;
    if (js == NULL) {
        NphJobSystemDesc d;
        memset(&d, 0, sizeof d);
        d.alloc = nph_allocator_system();
        if (nph_job_system_create(&d, &js) != NPH_RESULT_OK) {
            js = NULL;
        }
    }
    return js;
}

// A solver of the §13 comparison and its substeps (0: the default).
typedef struct BenchSolver {
    NphPhysicsSolver solver;
    u32 substeps;
} BenchSolver;

static const BenchSolver k_soft = {NPH_PHYSICS_SOLVER_SOFT_STEP, 0U};

static NphPhysicsWorld *bench_world(bool sleep, BenchSolver solver)
{
    NphPhysicsWorldDesc d = nph_physics_world_desc_default();
    d.alloc = nph_allocator_system();
    d.enable_sleep = sleep ? 1 : 0;
    d.solver = solver.solver;
    if (solver.substeps != 0U) {
        d.substeps = solver.substeps;
    }
    NphPhysicsWorld *w = NULL;
    if (nph_physics_world_create(&d, &w) != NPH_RESULT_OK) {
        return NULL;
    }
    return w;
}

static void bench_body(NphPhysicsWorld *w, NphPhysicsBodyType type, NphDVec3 p, NphQuat q,
                       NphPhysicsGeometry g)
{
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.type = type;
    bd.position = p;
    bd.rotation = q;
    NphPhysicsBody b;
    if (nph_physics_body_create(w, &bd, &b) != NPH_RESULT_OK) {
        return;
    }
    const NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    NphPhysicsShape s;
    NPH_IGNORE_RESULT(nph_physics_shape_create(w, b, &sd, &s));
}

static void bench_ground(NphPhysicsWorld *w, f32 half)
{
    bench_body(w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(0.0, -1.0, 0.0), nph_quat_identity(),
               nph_physics_geometry_box(nph_vec3(half, 1.0F, half)));
}

static void bench_steps(NphPhysicsWorld *w, NphJobSystem *js, u32 n)
{
    for (u32 i = 0U; i < n; ++i) {
        NPH_IGNORE_RESULT(nph_physics_world_step(w, WORLD_DT, js));
    }
}

static NphPhysicsWorld *pyramids(u32 rows, u32 copies, BenchSolver solver)
{
    NphPhysicsWorld *w = bench_world(false, solver);
    if (w == NULL) {
        return NULL;
    }
    bench_ground(w, 50.0F);
    for (u32 k = 0U; k < copies; ++k) {
        const f64 z = 5.0 * ((f64)k - 0.5 * (f64)(copies - 1U));
        for (u32 row = 0U; row < rows; ++row) {
            const u32 count = rows - row;
            for (u32 i = 0U; i < count; ++i) {
                const f64 x = ((f64)i - (f64)(count - 1U) * 0.5) * 1.05;
                bench_body(w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(x, 0.5 + (f64)row, z),
                           nph_quat_identity(), nph_physics_geometry_box(nph_vec3_splat(0.5F)));
            }
        }
    }
    return w;
}

static void bench_world_steps(NbenchState *state, NphPhysicsWorld *w, NphJobSystem *js)
{
    if (w == NULL) {
        return;
    }
    bench_steps(w, js, 240U); // settle
    while (nbench_keep_running(state)) {
        NPH_IGNORE_RESULT(nph_physics_world_step(w, WORLD_DT, js));
    }
    nph_physics_world_destroy(w);
}

NBENCH(world_pyramid_20_st)
{
    bench_world_steps(state, pyramids(PYRAMID_ROWS, 1U, k_soft), NULL);
}

NBENCH(world_pyramid_20_mt)
{
    bench_world_steps(state, pyramids(PYRAMID_ROWS, 1U, k_soft), bench_jobs());
}

NBENCH(world_pyramids_16_mt)
{
    bench_world_steps(state, pyramids(PYRAMID_ROWS, 16U / (WORLD_SCALE > 1U ? 8U : 1U), k_soft),
                      bench_jobs());
}

// The pile of a thousand shapes in a bin, on the box ground or on `floor`
// placed at `at` (a mesh or a heightfield, which the bin's walls stand on).
static void bench_pile_on(NbenchState *state, NphJobSystem *js, BenchSolver solver,
                          const NphPhysicsGeometry *floor, NphDVec3 at)
{
    NphPhysicsWorld *w = bench_world(false, solver);
    if (w == NULL) {
        return;
    }
    if (floor != NULL) {
        bench_body(w, NPH_PHYSICS_BODY_STATIC, at, nph_quat_identity(), *floor);
    } else {
        bench_ground(w, 30.0F);
    }
    // A 10 m bin, its walls down into a rippled floor's hollows.
    for (u32 k = 0U; k < 4U; ++k) {
        const f64 s = k < 2U ? 1.0 : -1.0;
        const NphDVec3 p =
            k % 2U == 0U ? nph_dvec3(5.25 * s, 4.6, 0.0) : nph_dvec3(0.0, 4.6, 5.25 * s);
        const NphVec3 h = k % 2U == 0U ? nph_vec3(0.25F, 5.0F, 5.5F) : nph_vec3(5.5F, 5.0F, 0.25F);
        bench_body(w, NPH_PHYSICS_BODY_STATIC, p, nph_quat_identity(), nph_physics_geometry_box(h));
    }
    NphPcg32 r = bench_rng(17U);
    for (u32 i = 0U; i < 1000U / WORLD_SCALE; ++i) {
        const NphDVec3 p =
            nph_dvec3((f64)nph_pcg32_next_range_f32(&r, -4.0F, 4.0F), 1.0 + 0.05 * (f64)i,
                      (f64)nph_pcg32_next_range_f32(&r, -4.0F, 4.0F));
        NphPhysicsGeometry g;
        if (i % 3U == 0U) {
            g = nph_physics_geometry_box(rand_vec3(&r, 0.15F, 0.35F));
        } else if (i % 3U == 1U) {
            g = nph_physics_geometry_sphere(nph_pcg32_next_range_f32(&r, 0.15F, 0.3F));
        } else {
            g = nph_physics_geometry_capsule(nph_pcg32_next_range_f32(&r, 0.1F, 0.3F),
                                             nph_pcg32_next_range_f32(&r, 0.1F, 0.2F));
        }
        bench_body(w, NPH_PHYSICS_BODY_DYNAMIC, p, rand_rotation(&r), g);
    }
    bench_steps(w, bench_jobs(), 960U / WORLD_SCALE); // fall and settle
    while (nbench_keep_running(state)) {
        NPH_IGNORE_RESULT(nph_physics_world_step(w, WORLD_DT, js));
    }
    nph_physics_world_destroy(w);
}

static void bench_pile(NbenchState *state, NphJobSystem *js, BenchSolver solver)
{
    bench_pile_on(state, js, solver, NULL, nph_dvec3_zero());
}

// The pile on a flat mesh of 0.5 m cells (12 m across, 1152 triangles), and
// on a rippled one (30 cm swells): the narrow phase against triangles.
static void bench_mesh_pile(NbenchState *state, NphJobSystem *js, f32 ripple)
{
    NphPhysicsMesh *floor = bench_grid(24U, 0.5F, ripple);
    if (floor == NULL) {
        return;
    }
    const NphPhysicsGeometry g = nph_physics_geometry_mesh(floor);
    bench_pile_on(state, js, k_soft, &g, nph_dvec3_zero());
    nph_physics_mesh_free(floor, nph_allocator_system());
}

// The rippled pile on the same surface as a heightfield (25 x 25 samples).
NBENCH(world_heightfield_pile_1000_mt)
{
    NphPhysicsHeightfield *floor = bench_field(24U, 0.5F, 0.3F);
    if (floor == NULL) {
        return;
    }
    const NphPhysicsGeometry g = nph_physics_geometry_heightfield(floor);
    bench_pile_on(state, bench_jobs(), k_soft, &g, nph_dvec3(-6.0, 0.0, -6.0));
    nph_physics_heightfield_free(floor, nph_allocator_system());
}

NBENCH(world_mesh_pile_1000_st)
{
    bench_mesh_pile(state, NULL, 0.0F);
}

NBENCH(world_mesh_pile_1000_mt)
{
    bench_mesh_pile(state, bench_jobs(), 0.0F);
}

NBENCH(world_terrain_pile_1000_mt)
{
    bench_mesh_pile(state, bench_jobs(), 0.3F);
}

NBENCH(world_pile_1000_st)
{
    bench_pile(state, NULL, k_soft);
}

NBENCH(world_pile_1000_mt)
{
    bench_pile(state, bench_jobs(), k_soft);
}

NBENCH(world_sleeping_10k)
{
    NphPhysicsWorld *w = bench_world(true, k_soft);
    if (w == NULL) {
        return;
    }
    bench_ground(w, 150.0F);
    for (u32 i = 0U; i < 10000U / WORLD_SCALE; ++i) {
        const f64 x = (f64)(i % 100U) * 2.0 - 99.0;
        const f64 z = (f64)(u32)(i / 100U) * 2.0 - 99.0; // row index
        bench_body(w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(x, 0.5, z), nph_quat_identity(),
                   nph_physics_geometry_box(nph_vec3_splat(0.5F)));
    }
    bench_steps(w, bench_jobs(), 240U); // everything falls asleep
    // One ball keeps bouncing far from the boxes.
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3(140.0, 3.0, 140.0);
    bd.enable_sleep = 0;
    NphPhysicsBody ball;
    if (nph_physics_body_create(w, &bd, &ball) == NPH_RESULT_OK) {
        NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(nph_physics_geometry_sphere(0.5F));
        sd.restitution = 1.0F;
        NphPhysicsShape s;
        NPH_IGNORE_RESULT(nph_physics_shape_create(w, ball, &sd, &s));
    }
    while (nbench_keep_running(state)) {
        NPH_IGNORE_RESULT(nph_physics_world_step(w, WORLD_DT, bench_jobs()));
    }
    nph_physics_world_destroy(w);
}

// Sixteen chains of 25 capsule links, released sideways: they swing for the
// whole run (400 joints, sleep off).
static void bench_chains(NbenchState *state, NphJobSystem *js, BenchSolver solver)
{
    NphPhysicsWorld *w = bench_world(false, solver);
    if (w == NULL) {
        return;
    }
    const u32 links = 25U / (WORLD_SCALE > 1U ? 5U : 1U);
    for (u32 c = 0U; c < 16U; ++c) {
        const NphDVec3 top = nph_dvec3(0.0, 20.0, 2.0 * (f64)c);
        NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
        bd.type = NPH_PHYSICS_BODY_STATIC;
        bd.position = top;
        NphPhysicsBody prev;
        if (nph_physics_body_create(w, &bd, &prev) != NPH_RESULT_OK) {
            break;
        }
        for (u32 i = 0U; i < links; ++i) {
            bd = nph_physics_body_desc_default();
            bd.position = nph_dvec3_add(top, nph_dvec3(0.5 * ((f64)i + 0.5), 0.0, 0.0));
            bd.rotation = nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), 1.5707963F);
            NphPhysicsBody link;
            if (nph_physics_body_create(w, &bd, &link) != NPH_RESULT_OK) {
                break;
            }
            const NphPhysicsShapeDesc sd =
                nph_physics_shape_desc_default(nph_physics_geometry_capsule(0.2F, 0.05F));
            NphPhysicsShape shape;
            NPH_IGNORE_RESULT(nph_physics_shape_create(w, link, &sd, &shape));
            NphPhysicsJointDesc jd =
                nph_physics_joint_desc_default(NPH_PHYSICS_JOINT_BALL, prev, link);
            nph_physics_joint_frames_at(w, prev, link,
                                        nph_dvec3_add(top, nph_dvec3(0.5 * (f64)i, 0.0, 0.0)),
                                        nph_vec3(1.0F, 0.0F, 0.0F), &jd.frame_a, &jd.frame_b);
            NphPhysicsJoint j;
            NPH_IGNORE_RESULT(nph_physics_joint_create(w, &jd, &j));
            prev = link;
        }
    }
    bench_steps(w, js, 60U); // swinging down
    while (nbench_keep_running(state)) {
        NPH_IGNORE_RESULT(nph_physics_world_step(w, WORLD_DT, js));
    }
    nph_physics_world_destroy(w);
}

NBENCH(world_chains_st)
{
    bench_chains(state, NULL, k_soft);
}

NBENCH(world_chains_mt)
{
    bench_chains(state, bench_jobs(), k_soft);
}

NBENCH(world_pyramid_20_st_tgs)
{
    bench_world_steps(state, pyramids(PYRAMID_ROWS, 1U, (BenchSolver){NPH_PHYSICS_SOLVER_TGS, 0U}),
                      NULL);
}

NBENCH(world_pyramid_20_st_xpbd)
{
    bench_world_steps(state, pyramids(PYRAMID_ROWS, 1U, (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 0U}),
                      NULL);
}

NBENCH(world_pile_1000_mt_tgs)
{
    bench_pile(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_TGS, 0U});
}

NBENCH(world_pile_1000_mt_xpbd)
{
    bench_pile(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 0U});
}

NBENCH(world_chains_st_tgs)
{
    bench_chains(state, NULL, (BenchSolver){NPH_PHYSICS_SOLVER_TGS, 0U});
}

NBENCH(world_chains_st_xpbd)
{
    bench_chains(state, NULL, (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 0U});
}

NBENCH(world_pyramid_20_st_soft8)
{
    bench_world_steps(
        state, pyramids(PYRAMID_ROWS, 1U, (BenchSolver){NPH_PHYSICS_SOLVER_SOFT_STEP, 8U}), NULL);
}

NBENCH(world_pyramid_20_st_xpbd16)
{
    bench_world_steps(
        state, pyramids(PYRAMID_ROWS, 1U, (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 16U}), NULL);
}

NBENCH(world_pile_1000_mt_soft8)
{
    bench_pile(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_SOFT_STEP, 8U});
}

NBENCH(world_pile_1000_mt_xpbd16)
{
    bench_pile(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 16U});
}

NBENCH(world_chains_st_soft8)
{
    bench_chains(state, NULL, (BenchSolver){NPH_PHYSICS_SOLVER_SOFT_STEP, 8U});
}

NBENCH(world_chains_st_xpbd16)
{
    bench_chains(state, NULL, (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 16U});
}

// The ragdoll of the tests (test_world_support.h): eleven parts in a
// T-pose facing +Z, 1.7 m tall, its pelvis at `pelvis` and turned by
// `turn` about it; ball joints with cones, twists, and 2 N m of friction
// at the spine, neck, shoulders, and hips, hinges with ranges and the same
// friction at the elbows and knees.
static void bench_ragdoll(NphPhysicsWorld *w, NphDVec3 pelvis, NphQuat turn)
{
    const f32 deg = 3.14159265F / 180.0F;
    const NphQuat side = nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), 90.0F * deg);
    // Offsets from the pelvis; boxes (half extents), a sphere, capsules
    // (half height, radius).
    const struct {
        f32 x, y;
        NphPhysicsGeometry g;
        bool sideways;
    } parts[11] = {
        {0.0F, 0.0F, nph_physics_geometry_box(nph_vec3(0.15F, 0.1F, 0.1F)), false},
        {0.0F, 0.32F, nph_physics_geometry_box(nph_vec3(0.16F, 0.17F, 0.1F)), false},
        {0.0F, 0.62F, nph_physics_geometry_sphere(0.1F), false},
        {-0.33F, 0.4F, nph_physics_geometry_capsule(0.11F, 0.05F), true},
        {0.33F, 0.4F, nph_physics_geometry_capsule(0.11F, 0.05F), true},
        {-0.62F, 0.4F, nph_physics_geometry_capsule(0.11F, 0.045F), true},
        {0.62F, 0.4F, nph_physics_geometry_capsule(0.11F, 0.045F), true},
        {-0.1F, -0.32F, nph_physics_geometry_capsule(0.14F, 0.07F), false},
        {0.1F, -0.32F, nph_physics_geometry_capsule(0.14F, 0.07F), false},
        {-0.1F, -0.75F, nph_physics_geometry_capsule(0.14F, 0.06F), false},
        {0.1F, -0.75F, nph_physics_geometry_capsule(0.14F, 0.06F), false},
    };
    NphPhysicsBody bodies[11];
    for (u32 i = 0U; i < 11U; ++i) {
        NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
        bd.position = nph_dvec3_add_vec3(
            pelvis, nph_quat_rotate(turn, nph_vec3(parts[i].x, parts[i].y, 0.0F)));
        bd.rotation = nph_quat_mul(turn, parts[i].sideways ? side : nph_quat_identity());
        if (nph_physics_body_create(w, &bd, &bodies[i]) != NPH_RESULT_OK) {
            return;
        }
        const NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(parts[i].g);
        NphPhysicsShape shape;
        NPH_IGNORE_RESULT(nph_physics_shape_create(w, bodies[i], &sd, &shape));
    }
    const NphVec3 up = nph_vec3(0.0F, 1.0F, 0.0F);
    const NphVec3 down = nph_vec3(0.0F, -1.0F, 0.0F);
    const NphVec3 left = nph_vec3(-1.0F, 0.0F, 0.0F);
    const NphVec3 right = nph_vec3(1.0F, 0.0F, 0.0F);
    const struct {
        u32 a, b;
        f32 x, y;
        NphVec3 axis;
        bool hinge;
        f32 p0, p1, p2; // degrees: swing_y, swing_z, twist; or lower, upper
    } joints[10] = {
        {0U, 1U, 0.0F, 0.12F, up, false, 30.0F, 20.0F, 20.0F},
        {1U, 2U, 0.0F, 0.5F, up, false, 40.0F, 40.0F, 45.0F},
        {1U, 3U, -0.17F, 0.42F, left, false, 80.0F, 60.0F, 45.0F},
        {1U, 4U, 0.17F, 0.42F, right, false, 80.0F, 60.0F, 45.0F},
        {3U, 5U, -0.48F, 0.4F, up, true, 0.0F, 140.0F, 0.0F},
        {4U, 6U, 0.48F, 0.4F, down, true, 0.0F, 140.0F, 0.0F},
        {0U, 7U, -0.1F, -0.1F, down, false, 60.0F, 30.0F, 30.0F},
        {0U, 8U, 0.1F, -0.1F, down, false, 60.0F, 30.0F, 30.0F},
        {7U, 9U, -0.1F, -0.54F, right, true, 0.0F, 140.0F, 0.0F},
        {8U, 10U, 0.1F, -0.54F, right, true, 0.0F, 140.0F, 0.0F},
    };
    for (u32 i = 0U; i < 10U; ++i) {
        const NphPhysicsBody a = bodies[joints[i].a];
        const NphPhysicsBody b = bodies[joints[i].b];
        NphPhysicsJointDesc jd = nph_physics_joint_desc_default(
            joints[i].hinge ? NPH_PHYSICS_JOINT_HINGE : NPH_PHYSICS_JOINT_BALL, a, b);
        if (joints[i].hinge) {
            jd.hinge.enable_limit = 1;
            jd.hinge.lower = joints[i].p0 * deg;
            jd.hinge.upper = joints[i].p1 * deg;
            jd.hinge.enable_motor = 1;
            jd.hinge.max_motor_torque = 2.0F;
        } else {
            jd.ball.enable_swing_limit = 1;
            jd.ball.swing_y = joints[i].p0 * deg;
            jd.ball.swing_z = joints[i].p1 * deg;
            jd.ball.enable_twist_limit = 1;
            jd.ball.twist_lower = -joints[i].p2 * deg;
            jd.ball.twist_upper = joints[i].p2 * deg;
            jd.ball.friction_torque = 2.0F;
        }
        const NphDVec3 at = nph_dvec3_add_vec3(
            pelvis, nph_quat_rotate(turn, nph_vec3(joints[i].x, joints[i].y, 0.0F)));
        nph_physics_joint_frames_at(w, a, b, at, nph_quat_rotate(turn, joints[i].axis), &jd.frame_a,
                                    &jd.frame_b);
        NphPhysicsJoint j;
        NPH_IGNORE_RESULT(nph_physics_joint_create(w, &jd, &j));
    }
}

// Thirty-two ragdolls (352 bodies, 320 joints) dropped on their backs in
// four criss-cross heaps of eight, each a quarter turn from the one below,
// measured once the heaps lie still (sleep off).
static void bench_ragdolls(NbenchState *state, NphJobSystem *js, BenchSolver solver)
{
    NphPhysicsWorld *w = bench_world(false, solver);
    if (w == NULL) {
        return;
    }
    bench_ground(w, 30.0F);
    const NphQuat lying = nph_quat_from_axis_angle(nph_vec3(1.0F, 0.0F, 0.0F), -1.5707963F);
    const u32 count = WORLD_SCALE > 1U ? 4U : 32U;
    for (u32 k = 0U; k < count; ++k) {
        const u32 layer = k / 4U;
        const f64 x = k % 2U == 0U ? -2.5 : 2.5;
        const f64 z = k % 4U < 2U ? -2.5 : 2.5;
        const NphQuat turn = nph_quat_mul(
            nph_quat_from_axis_angle(nph_vec3(0.0F, 1.0F, 0.0F), (f32)layer * 1.5707963F), lying);
        bench_ragdoll(w, nph_dvec3(x, 0.15 + 0.3 * (f64)layer, z), turn);
    }
    bench_steps(w, bench_jobs(), 960U / WORLD_SCALE); // fall and settle
    while (nbench_keep_running(state)) {
        NPH_IGNORE_RESULT(nph_physics_world_step(w, WORLD_DT, js));
    }
    nph_physics_world_destroy(w);
}

NBENCH(world_ragdolls_32_st)
{
    bench_ragdolls(state, NULL, k_soft);
}

NBENCH(world_ragdolls_32_mt)
{
    bench_ragdolls(state, bench_jobs(), k_soft);
}

NBENCH(world_ragdolls_32_mt_tgs)
{
    bench_ragdolls(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_TGS, 0U});
}

NBENCH(world_ragdolls_32_mt_xpbd)
{
    bench_ragdolls(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 0U});
}

NBENCH(world_ragdolls_32_mt_soft8)
{
    bench_ragdolls(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_SOFT_STEP, 8U});
}

NBENCH(world_ragdolls_32_mt_xpbd16)
{
    bench_ragdolls(state, bench_jobs(), (BenchSolver){NPH_PHYSICS_SOLVER_XPBD, 16U});
}
