// SPDX-License-Identifier: Apache-2.0
// Transforms: the math against 4x4 references, the hierarchy (links,
// depths, cycles, despawns), and the transform system (correctness,
// change detection, parallel equals serial, large-world precision).

#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>

#include "test_scene_support.h"

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

static NphMat4 world_to_mat4(const NphWorldTransform *t)
{
    NphMat4 m = nph_mat4_from_mat3(nph_world_transform_linear(t));
    m.c[3] = nph_vec4((f32)t->translation.x, (f32)t->translation.y, (f32)t->translation.z, 1.0F);
    return m;
}

static NphMat4 local_to_mat4(const NphLocalTransform *l)
{
    return nph_mat4_from_trs(nph_dvec3_to_vec3(l->translation), l->rotation, l->scale);
}

static NphLocalTransform random_local(u64 *rng, f32 extent)
{
    const f32 a = (f32)test_rng_below(rng, 6283U) / 1000.0F;
    // Each draw a statement of its own (as one call's arguments, they would be
    // made in an order each compiler chooses).
    const f32 ax = (f32)test_rng_below(rng, 100U) - 49.5F;
    const f32 ay = (f32)test_rng_below(rng, 100U) + 1.0F;
    const f32 az = (f32)test_rng_below(rng, 100U) - 49.5F;
    const NphVec3 axis = nph_vec3_normalize(nph_vec3(ax, ay, az));
    const f64 x = ((f64)test_rng_below(rng, 2001U) - 1000.0) / 1000.0 * (f64)extent;
    const f64 y = ((f64)test_rng_below(rng, 2001U) - 1000.0) / 1000.0 * (f64)extent;
    const f64 z = ((f64)test_rng_below(rng, 2001U) - 1000.0) / 1000.0 * (f64)extent;
    const f32 s = 0.5F + (f32)test_rng_below(rng, 100U) / 100.0F;
    return nph_local_transform(nph_dvec3(x, y, z), nph_quat_from_axis_angle(axis, a),
                               nph_vec3(s, s * 1.25F, s * 0.75F));
}

NTEST(transform, math_matches_4x4_references)
{
    u64 rng = 42U;
    for (u32 i = 0; i < 200U; ++i) {
        const NphLocalTransform parent_local = random_local(&rng, 10.0F);
        const NphLocalTransform child_local = random_local(&rng, 5.0F);
        const NphWorldTransform parent = nph_world_transform_from_local(&parent_local);
        NTEST_ASSERT_TRUE(
            nph_mat4_near(world_to_mat4(&parent), local_to_mat4(&parent_local), 1e-4F));
        const NphWorldTransform child = nph_world_transform_compose(&parent, &child_local);
        const NphMat4 expect =
            nph_mat4_mul(local_to_mat4(&parent_local), local_to_mat4(&child_local));
        NTEST_ASSERT_TRUE(nph_mat4_near(world_to_mat4(&child), expect, 1e-3F));

        // A point goes through the same transform.
        const NphVec3 p = nph_vec3(0.5F, -1.0F, 2.0F);
        const NphDVec3 wp = nph_world_transform_point(&child, p);
        const NphVec3 ep = nph_mat4_transform_point(expect, p);
        NTEST_ASSERT_NEAR_F64(wp.x, (f64)ep.x, 1e-3);
        NTEST_ASSERT_NEAR_F64(wp.y, (f64)ep.y, 1e-3);
        NTEST_ASSERT_NEAR_F64(wp.z, (f64)ep.z, 1e-3);

        // Camera-relative: the translation minus the origin, same linear part.
        const NphDVec3 origin = nph_dvec3(1.0, 2.0, 3.0);
        const NphAffine rel = nph_world_transform_relative(&child, origin);
        NTEST_ASSERT_NEAR_F32(rel.r[0].w, (f32)(child.translation.x - 1.0), 1e-5F);
        NTEST_ASSERT_NEAR_F32(rel.r[2].w, (f32)(child.translation.z - 3.0), 1e-5F);
        NTEST_ASSERT_NEAR_F32(rel.r[1].x, child.x_axis.y, 0.0F);
        NTEST_ASSERT_NEAR_F32(rel.r[0].z, child.z_axis.x, 0.0F);
    }
    const NphWorldTransform id = nph_world_transform_identity();
    const NphLocalTransform lid = nph_local_transform_identity();
    const NphWorldTransform from_id = nph_world_transform_from_local(&lid);
    NTEST_EXPECT_TRUE(nph_component_fields_equal(NphWorldTransform_component(), &id, &from_id));
}

NTEST(transform, large_world_precision)
{
    // A parent 30 000 km from the origin and a child 1 mm from it: the
    // child's world position keeps the millimetre in 64 bits, to the f64
    // resolution there (2^-28 m, about 4 nm).
    NphLocalTransform pl = nph_local_transform_identity();
    pl.translation = nph_dvec3(1.0e7, -2.5e6, 3.0e7);
    const NphWorldTransform parent = nph_world_transform_from_local(&pl);
    NphLocalTransform cl = nph_local_transform_identity();
    cl.translation = nph_dvec3(0.001, 0.0, -0.001);
    const NphWorldTransform child = nph_world_transform_compose(&parent, &cl);
    NTEST_EXPECT_NEAR_F64(child.translation.x - 1.0e7, 0.001, 1e-8);
    NTEST_EXPECT_NEAR_F64(child.translation.z - 3.0e7, -0.001, 1e-8);
    // Relative to a camera next to it, in 32 bits, still exact to well
    // below a millimetre.
    const NphAffine rel = nph_world_transform_relative(&child, nph_dvec3(1.0e7, -2.5e6, 3.0e7));
    NTEST_EXPECT_NEAR_F32(rel.r[0].w, 0.001F, 1e-7F);
    NTEST_EXPECT_NEAR_F32(rel.r[2].w, -0.001F, 1e-7F);
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

static NphEntity spawn_tf(NphWorld *w)
{
    const NphComponentId comps[2] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 2U, &e), NPH_RESULT_OK);
    return e;
}

static u32 child_count(const NphWorld *w, NphEntity e)
{
    u32 n = 0U;
    for (NphEntity c = nph_world_first_child(w, e); !nph_entity_is_null(c);
         c = nph_world_next_sibling(w, c)) {
        n += 1U;
    }
    return n;
}

NTEST(transform, set_parent_links_and_depths)
{
    NphWorld *w = test_world_create(0U, NULL);
    const NphEntity root = spawn_tf(w);
    const NphEntity a = spawn_tf(w);
    const NphEntity b = spawn_tf(w);
    const NphEntity c = spawn_tf(w);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, a, root), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, b, root), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, c, a), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w, a), root));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_first_child(w, root), a)); // attach order
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_next_sibling(w, a), b));
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_next_sibling(w, b)));
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, root), 0U);
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, a), 1U);
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, c), 2U);
    NTEST_EXPECT_EQ_UINT(child_count(w, root), 2U);

    // Cycles and bad arguments change nothing.
    NTEST_EXPECT_EQ_INT(nph_world_set_parent(w, root, c), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_world_set_parent(w, a, a), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_world_set_parent(w, NPH_ENTITY_NULL, a), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(w, root)));

    // Moving a subtree renumbers it; the old parent's list closes up.
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, a, b), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, a), 2U);
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, c), 3U);
    NTEST_EXPECT_EQ_UINT(child_count(w, root), 1U);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_first_child(w, root), b));
    NTEST_EXPECT_EQ_INT(nph_world_set_parent(w, a, b), NPH_RESULT_OK); // no-op
    NTEST_EXPECT_EQ_UINT(child_count(w, b), 1U);

    // Back to a root.
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, a, NPH_ENTITY_NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, a), 0U);
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, c), 1U);
    NTEST_EXPECT_EQ_UINT(child_count(w, b), 0U);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(w, a)));
    nph_world_destroy(w);
}

NTEST(transform, middle_child_unlinks_cleanly)
{
    NphWorld *w = test_world_create(0U, NULL);
    const NphEntity p = spawn_tf(w);
    NphEntity kids[4];
    for (u32 i = 0; i < 4U; ++i) {
        kids[i] = spawn_tf(w);
        NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, kids[i], p), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, kids[1]), NPH_RESULT_OK); // middle
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, kids[3]), NPH_RESULT_OK); // last
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, kids[0]), NPH_RESULT_OK); // first
    NTEST_EXPECT_EQ_UINT(child_count(w, p), 1U);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_first_child(w, p), kids[2]));
    const NphEntity late = spawn_tf(w);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, late, p), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_next_sibling(w, kids[2]), late));
    nph_world_destroy(w);
}

NTEST(transform, despawn_orphans_and_recursive_despawn)
{
    NphWorld *w = test_world_create(0U, NULL);
    const NphEntity root = spawn_tf(w);
    const NphEntity a = spawn_tf(w);
    const NphEntity b = spawn_tf(w);
    const NphEntity c = spawn_tf(w);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, a, root), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, b, a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, c, a), NPH_RESULT_OK);
    // Despawning a orphans b and c: they become roots.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, a), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(w, b)));
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, c), 0U);
    NTEST_EXPECT_EQ_UINT(child_count(w, root), 0U);

    // A subtree goes at once.
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, b, root), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, c, b), NPH_RESULT_OK);
    const NphEntity other = spawn_tf(w);
    NTEST_ASSERT_EQ_INT(nph_world_despawn_recursive(w, root), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, root));
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, b));
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, c));
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, other));
    NTEST_EXPECT_EQ_INT(nph_world_despawn_recursive(w, root), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_world_despawn_recursive(w, other), NPH_RESULT_OK); // no children

    // Removing Hierarchy detaches too.
    const NphEntity p = spawn_tf(w);
    const NphEntity k = spawn_tf(w);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, k, p), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_remove(w, p, NPH_COMPONENT_ID_HIERARCHY), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(w, k)));
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, k), 0U);
    nph_world_destroy(w);
}

NTEST(transform, depth_is_bounded)
{
    NphWorld *w = test_world_create(4096U, NULL);
    NphEntity prev = spawn_tf(w);
    const NphEntity top = prev;
    for (u32 d = 1; d <= NPH_HIERARCHY_MAX_DEPTH; ++d) {
        const NphEntity e = spawn_tf(w);
        NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e, prev), NPH_RESULT_OK);
        prev = e;
    }
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, prev), NPH_HIERARCHY_MAX_DEPTH);
    const NphEntity too_deep = spawn_tf(w);
    NTEST_EXPECT_EQ_INT(nph_world_set_parent(w, too_deep, prev), NPH_RESULT_OVERFLOW);
    // Hanging the whole chain under another root would also overflow.
    const NphEntity other = spawn_tf(w);
    NTEST_EXPECT_EQ_INT(nph_world_set_parent(w, top, other), NPH_RESULT_OVERFLOW);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(w, top)));
    nph_world_destroy(w);
}

// ---------------------------------------------------------------------------
// The transform system
// ---------------------------------------------------------------------------

// A forest: `roots` roots, each with a chain and a fan of children, so
// several depth levels and many entities per level exist.
typedef struct Forest {
    NphEntity *e;
    u32 count;
} Forest;

static void build_forest(NphWorld *w, u32 roots, u32 depth, u32 fan, Forest *out, u64 seed)
{
    const u32 per_root = 1U + depth * fan;
    out->count = roots * per_root;
    out->e = NPH_ALLOC_ARRAY(nph_allocator_system(), NphEntity, out->count);
    NTEST_ASSERT_NOT_NULL(out->e);
    u64 rng = seed;
    u32 k = 0U;
    for (u32 r = 0; r < roots; ++r) {
        NphEntity parent = spawn_tf(w);
        const NphLocalTransform l = random_local(&rng, 50.0F);
        NTEST_ASSERT_EQ_INT(nph_world_set(w, parent, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &l),
                            NPH_RESULT_OK);
        out->e[k++] = parent;
        for (u32 d = 0; d < depth; ++d) {
            NphEntity next = NPH_ENTITY_NULL;
            for (u32 f = 0; f < fan; ++f) {
                const NphEntity c = spawn_tf(w);
                const NphLocalTransform cl = random_local(&rng, 2.0F);
                NTEST_ASSERT_EQ_INT(nph_world_set(w, c, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &cl),
                                    NPH_RESULT_OK);
                NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, c, parent), NPH_RESULT_OK);
                out->e[k++] = c;
                next = f == 0U ? c : next;
            }
            parent = next;
        }
    }
}

static void free_forest(Forest *f)
{
    NPH_FREE_ARRAY(nph_allocator_system(), NphEntity, f->e, f->count);
}

// The world transform computed by walking up the parents (the reference).
static NphWorldTransform reference_world(const NphWorld *w, NphEntity e)
{
    const NphLocalTransform *l =
        NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    const NphEntity p = nph_world_parent(w, e);
    if (nph_entity_is_null(p)) {
        return nph_world_transform_from_local(l);
    }
    const NphWorldTransform pw = reference_world(w, p);
    return nph_world_transform_compose(&pw, l);
}

static void check_forest(const NphWorld *w, const Forest *f)
{
    for (u32 i = 0; i < f->count; ++i) {
        const NphWorldTransform expect = reference_world(w, f->e[i]);
        const NphWorldTransform *got =
            NPH_WORLD_GET(w, f->e[i], NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
        NTEST_ASSERT_TRUE(nph_component_fields_equal(NphWorldTransform_component(), &expect, got));
    }
}

NTEST(transform, system_matches_the_reference_and_skips_static_content)
{
    NphWorld *w = test_world_create(1U << 16U, NULL);
    Forest f;
    build_forest(w, 40U, 6U, 5U, &f, 7U);
    NphTransformSystem *ts;
    NTEST_ASSERT_EQ_INT(nph_transform_system_create(w, nph_allocator_system(), &ts), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, NULL), NPH_RESULT_OK);
    check_forest(w, &f);
    NphTransformStats st;
    nph_transform_system_stats(ts, &st);
    NTEST_EXPECT_EQ_UINT(st.roots_updated, 40U);
    NTEST_EXPECT_EQ_UINT(st.children_updated, f.count - 40U);
    NTEST_EXPECT_EQ_UINT(st.levels, 6U);

    // Nothing changed: nothing is recomputed.
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, NULL), NPH_RESULT_OK);
    nph_transform_system_stats(ts, &st);
    NTEST_EXPECT_EQ_UINT(st.roots_updated, 0U);
    NTEST_EXPECT_EQ_UINT(st.children_updated, 0U);
    NTEST_EXPECT_GT_UINT(st.chunks_skipped, 0U);

    // Moving one root recomputes it (and whatever shares its chunk) and its
    // subtree, and nothing else's world transform changes.
    NphLocalTransform *l =
        NPH_WORLD_GET_MUT(w, f.e[0], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    l->translation.x += 10.0;
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, NULL), NPH_RESULT_OK);
    check_forest(w, &f);
    nph_transform_system_stats(ts, &st);
    NTEST_EXPECT_GT_UINT(st.roots_updated, 0U);
    NTEST_EXPECT_GE_UINT(st.children_updated, 6U * 5U);

    // Moving a leaf recomputes its chunk (whatever else lives there, a
    // root included) and their subtrees, not the rest.
    NphLocalTransform *leaf = NPH_WORLD_GET_MUT(
        w, f.e[f.count - 1U], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    leaf->scale.x = 3.0F;
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, NULL), NPH_RESULT_OK);
    check_forest(w, &f);
    nph_transform_system_stats(ts, &st);
    NTEST_EXPECT_LE_UINT(st.roots_updated, 8U);
    NTEST_EXPECT_GT_UINT(st.children_updated, 0U);
    NTEST_EXPECT_LT_UINT(st.roots_updated + st.children_updated, f.count / 2U);

    // Reparenting is a change too.
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, f.e[1], f.e[f.count - 1U]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, NULL), NPH_RESULT_OK);
    check_forest(w, &f);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, f.e[1], NPH_ENTITY_NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, NULL), NPH_RESULT_OK);
    check_forest(w, &f);

    nph_transform_system_destroy(ts);
    free_forest(&f);
    nph_world_destroy(w);
}

NTEST(transform, parallel_equals_serial)
{
    NphJobSystem *js = test_jobs_create(test_worker_count(8U));
    NphWorld *a = test_world_create(1U << 17U, NULL);
    NphWorld *b = test_world_create(1U << 17U, NULL);
    Forest fa;
    Forest fb;
    const u32 roots = 100U * TEST_SCALE;
    build_forest(a, roots, 5U, 8U, &fa, 99U);
    build_forest(b, roots, 5U, 8U, &fb, 99U);
    NphTransformSystem *ta;
    NphTransformSystem *tb;
    NTEST_ASSERT_EQ_INT(nph_transform_system_create(a, nph_allocator_system(), &ta), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_transform_system_create(b, nph_allocator_system(), &tb), NPH_RESULT_OK);
    for (u32 frame = 0; frame < 4U; ++frame) {
        // Move every 7th root in both worlds.
        for (u32 i = 0; i < fa.count; i += 7U * 41U) {
            NphLocalTransform *la =
                NPH_WORLD_GET_MUT(a, fa.e[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
            NphLocalTransform *lb =
                NPH_WORLD_GET_MUT(b, fb.e[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
            la->rotation =
                nph_quat_normalize(nph_quat_mul(la->rotation, nph_quat_from_rotation_y(0.1F)));
            *lb = *la;
        }
        NTEST_ASSERT_EQ_INT(nph_transform_system_update(ta, js), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_transform_system_update(tb, NULL), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(nph_world_hash(a), nph_world_hash(b));
    }
    check_forest(a, &fa);
    nph_transform_system_destroy(ta);
    nph_transform_system_destroy(tb);
    free_forest(&fa);
    free_forest(&fb);
    nph_world_destroy(a);
    nph_world_destroy(b);
    nph_job_system_destroy(js);
}

NTEST(transform, parents_without_transforms_count_as_identity)
{
    NphWorld *w = test_world_create(0U, NULL);
    NphEntity group;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, NULL, 0U, &group), NPH_RESULT_OK);
    const NphEntity child = spawn_tf(w);
    NphLocalTransform l = nph_local_transform_identity();
    l.translation = nph_dvec3(1.0, 2.0, 3.0);
    NTEST_ASSERT_EQ_INT(nph_world_set(w, child, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &l),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, child, group), NPH_RESULT_OK);
    NphTransformSystem *ts;
    NTEST_ASSERT_EQ_INT(nph_transform_system_create(w, nph_allocator_system(), &ts), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, NULL), NPH_RESULT_OK);
    const NphWorldTransform *wt =
        NPH_WORLD_GET(w, child, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    NTEST_EXPECT_NEAR_F64(wt->translation.y, 2.0, 0.0);
    nph_transform_system_destroy(ts);
    nph_world_destroy(w);
}

NTEST(transform, system_in_a_schedule)
{
    NphJobSystem *js = test_jobs_create(test_worker_count(4U));
    NphWorld *w = test_world_create(1U << 16U, NULL);
    Forest f;
    build_forest(w, 64U, 4U, 4U, &f, 5U);
    NphTransformSystem *ts;
    NTEST_ASSERT_EQ_INT(nph_transform_system_create(w, nph_allocator_system(), &ts), NPH_RESULT_OK);
    NphScheduleDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.world = w;
    sd.jobs = js;
    sd.alloc = nph_allocator_system();
    NphSchedule *s;
    NTEST_ASSERT_EQ_INT(nph_schedule_create(&sd, &s), NPH_RESULT_OK);
    const NphSystemDesc d = nph_transform_system_desc(ts);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 1.0 / 240.0), NPH_RESULT_OK);
    check_forest(w, &f);
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 1.0 / 240.0), NPH_RESULT_OK);
    NphTransformStats st;
    nph_transform_system_stats(ts, &st);
    NTEST_EXPECT_EQ_UINT(st.roots_updated + st.children_updated, 0U);
    nph_schedule_destroy(s);
    nph_transform_system_destroy(ts);
    free_forest(&f);
    nph_world_destroy(w);
    nph_job_system_destroy(js);
}
