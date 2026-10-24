// SPDX-License-Identifier: Apache-2.0
// Meshes in the world (ADR-0046): mesh shapes on static and kinematic
// bodies only; shapes resting and sleeping on a mesh floor; boxes and
// capsules sliding, spheres and capsules rolling across seams without a
// bump (the internal-edge exit test, against a floor whose edges are all
// open, which bumps); a box settling in a crease on two manifolds (a child
// contact at work) and keeping its impulses across them when its points'
// ids change; a kinematic platform carrying a box; events once per
// pair; invariants and determinism through a pile on a terrain; and the
// same motion with the mesh, or the world, far from its origin. Then
// heightfields (world_heightfield): on static and kinematic bodies only;
// shapes resting and sleeping; sliding and rolling across cells without a
// bump; falling through holes; a pile on a heightfield terrain keeping its
// course; and far from the world's origin.

#include "test_world_support.h"

#include <float.h>
#include <math.h>

static NphQuat yaw(f32 angle)
{
    return nph_quat_from_axis_angle(nph_vec3_unit_y(), angle);
}

// A capsule's long axis (Y) laid along X.
static NphQuat lying(void)
{
    return nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.5F * k_pi);
}

static NphPhysicsBody add_mesh_body(NphPhysicsWorld *w, NphPhysicsBodyType type, NphDVec3 p,
                                    const NphPhysicsMesh *m)
{
    return add_body(w, type, p, nph_quat_identity(), nph_physics_geometry_mesh(m), 0.6F, 0.0F);
}

static f64 horizontal(NphVec3 v)
{
    return sqrt((f64)v.x * (f64)v.x + (f64)v.z * (f64)v.z);
}

NTEST(world_mesh, mesh_shapes_go_on_static_and_kinematic_bodies)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *mesh = test_grid_mesh(ma, 4U, 1.0F, -2.0F, -2.0F, NULL, false);
    Scene s;
    scene_begin(&s);
    const NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(nph_physics_geometry_mesh(mesh));
    NphPhysicsShape shape;
    // A dynamic body cannot carry one: a surface has no mass to move it.
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    NphPhysicsBody body;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &body), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(s.w, body, &sd, &shape),
                        NPH_RESULT_INVALID_ARGUMENT);
    // Static and kinematic bodies can; the shape's box is the mesh's,
    // placed (a quarter turn about Y keeps the square).
    const NphPhysicsBodyType types[2] = {NPH_PHYSICS_BODY_STATIC, NPH_PHYSICS_BODY_KINEMATIC};
    for (u32 k = 0U; k < 2U; ++k) {
        bd.type = types[k];
        bd.position = nph_dvec3(10.0, 1.0, 5.0 * (f64)k);
        bd.rotation = yaw(0.5F * k_pi);
        NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &body), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, body, &sd, &shape), NPH_RESULT_OK);
        const NphAABB box = nph_physics_shape_aabb(s.w, shape);
        NTEST_EXPECT_TRUE(
            nph_vec3_near(box.min, nph_vec3(8.0F, 1.0F, 5.0F * (f32)k - 2.0F), 1e-5F));
        NTEST_EXPECT_TRUE(
            nph_vec3_near(box.max, nph_vec3(12.0F, 1.0F, 5.0F * (f32)k + 2.0F), 1e-5F));
    }
    // No mesh: invalid geometry.
    const NphPhysicsShapeDesc none =
        nph_physics_shape_desc_default(nph_physics_geometry_mesh(NULL));
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(s.w, body, &none, &shape),
                        NPH_RESULT_INVALID_ARGUMENT);
    scene_end(&s);
    nph_physics_mesh_free(mesh, ma);
    test_alloc_end(&mta);
}

NTEST(world_mesh, shapes_rest_and_sleep_on_a_mesh_floor)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *mesh = test_grid_mesh(ma, 16U, 0.5F, -4.0F, -4.0F, NULL, false);
    Scene s;
    scene_begin(&s);
    add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), mesh);
    // Dropped 5 cm: a box turned across the seams, a ball over a vertex, a
    // capsule lying across a cell.
    const NphQuat box_turn = yaw(0.4F);
    const NphQuat cap_turn = nph_quat_mul(yaw(0.7F), lying());
    const NphPhysicsBody bodies[3] = {
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.3, 0.3, -0.2), box_turn,
                 nph_physics_geometry_box(nph_vec3(0.5F, 0.25F, 0.4F)), 0.6F, 0.0F),
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-2.0, 0.35, 1.5), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.3F), 0.6F, 0.0F),
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(2.1, 0.15, -2.2), cap_turn,
                 nph_physics_geometry_capsule(0.4F, 0.1F), 0.6F, 0.0F)};
    const NphDVec3 starts[3] = {nph_dvec3(0.3, 0.25, -0.2), nph_dvec3(-2.0, 0.3, 1.5),
                                nph_dvec3(2.1, 0.1, -2.2)};
    for (u32 i = 0U; i < 240U * 3U; ++i) {
        step(s.w, NULL, 1U);
        if (i == 96U) {
            // Landed, not yet asleep: three pairs, a box's four points, the
            // ball's one, the capsule's two.
            const NphPhysicsStats st = nph_physics_world_stats(s.w);
            NTEST_EXPECT_EQ_UINT(st.touching_contacts, 3U);
            NTEST_EXPECT_EQ_UINT(st.manifold_points, 7U);
        }
        if (i % 60U == 0U) {
            NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
        }
    }
    for (u32 k = 0U; k < 3U; ++k) {
        NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, bodies[k]));
        const NphDVec3 p = nph_physics_body_position(s.w, bodies[k]);
        NTEST_EXPECT_NEAR_F64(p.x, starts[k].x, 2e-3);
        NTEST_EXPECT_NEAR_F64(p.z, starts[k].z, 2e-3);
        NTEST_EXPECT_NEAR_F64(p.y, starts[k].y, 6e-3);
    }
    const NphQuat q = nph_physics_body_rotation(s.w, bodies[0]);
    NTEST_EXPECT_TRUE(nph_quat_angle(nph_quat_mul(nph_quat_conjugate(box_turn), q)) < 2e-3F);
    scene_end(&s);
    nph_physics_mesh_free(mesh, ma);
    test_alloc_end(&mta);
}

// The fine floor of the sliding test with its triangles apart: every one has
// vertices of its own, moved a micrometre off its neighbours', so no edge
// is shared and every edge is open, hence active.
static NphPhysicsMesh *unwelded_floor(NphAllocator *alloc, u32 n, f32 cell, f32 x0, f32 z0)
{
    static NphVec3 v[6U * 40U * 40U];
    static u32 idx[6U * 40U * 40U];
    NTEST_ASSERT_TRUE(n <= 40U);
    u32 k = 0U;
    for (u32 i = 0U; i < n; ++i) {
        for (u32 j = 0U; j < n; ++j) {
            const f32 x = x0 + (f32)i * cell;
            const f32 z = z0 + (f32)j * cell;
            const NphVec3 c[4] = {nph_vec3(x, 0.0F, z), nph_vec3(x, 0.0F, z + cell),
                                  nph_vec3(x + cell, 0.0F, z), nph_vec3(x + cell, 0.0F, z + cell)};
            const u32 quad[6] = {0U, 1U, 2U, 2U, 1U, 3U};
            for (u32 q = 0U; q < 6U; ++q) {
                const f32 off = 1e-6F * (f32)(1U + k % 3U);
                v[k] = nph_vec3(c[quad[q]].x + off, 0.0F, c[quad[q]].z - off);
                idx[k] = k;
                k += 1U;
            }
        }
    }
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = k;
    d.indices = idx;
    d.triangle_count = k / 3U;
    d.weld_tolerance = -1.0F; // exact copies only, and there are none
    NphPhysicsMesh *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_mesh_build(&d, alloc, &m), NPH_RESULT_OK);
    return m;
}

typedef struct Glide {
    f64 lift;      // the largest rise or fall from the settled height, m
    f64 turn;      // the largest change of the angular velocity, rad/s
    f64 slow;      // the largest relative change of the horizontal speed
    f64 wander;    // the distance off the straight line at the end, m
    f64 travelled; // along the line, m
} Glide;

// One body on `floor` (placed at `at`) started along `dir` at `speed`
// (rolling when `rolls`, with the angular velocity that matches), measured
// over two seconds.
static Glide glide(NphPhysicsGeometry floor, NphDVec3 at, NphPhysicsGeometry g, NphQuat q, f64 rest,
                   f32 friction, bool rolls, f32 radius, NphVec3 dir, f32 speed)
{
    Scene s;
    scene_begin(&s);
    add_body(s.w, NPH_PHYSICS_BODY_STATIC, at, nph_quat_identity(), floor, 0.6F, 0.0F);
    const NphDVec3 start = nph_dvec3(-3.5, rest - 0.0005, -1.0);
    const NphPhysicsBody b = add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, start, q, g, friction, 0.0F);
    const NphVec3 v0 = nph_vec3_scale(dir, speed);
    const NphVec3 w0 = rolls ? nph_vec3_scale(nph_vec3_cross(nph_vec3_unit_y(), v0), 1.0F / radius)
                             : nph_vec3_zero();
    nph_physics_body_set_linear_velocity(s.w, b, v0);
    nph_physics_body_set_angular_velocity(s.w, b, w0);
    // A few steps to settle the half millimetre into the floor first.
    step(s.w, NULL, 12U);
    const f64 height = nph_physics_body_position(s.w, b).y;
    Glide out;
    memset(&out, 0, sizeof out);
    for (u32 i = 0U; i < 480U; ++i) {
        step(s.w, NULL, 1U);
        const NphVec3 v = nph_physics_body_linear_velocity(s.w, b);
        const NphVec3 w = nph_physics_body_angular_velocity(s.w, b);
        out.lift = fmax(out.lift, fabs(nph_physics_body_position(s.w, b).y - height));
        out.turn = fmax(out.turn, (f64)nph_vec3_length(nph_vec3_sub(w, w0)));
        out.slow = fmax(out.slow, fabs(horizontal(v) / (f64)speed - 1.0));
    }
    const NphDVec3 d = nph_dvec3_sub(nph_physics_body_position(s.w, b), start);
    out.travelled = d.x * (f64)dir.x + d.z * (f64)dir.z;
    out.wander = fabs(d.x * (f64)dir.z - d.z * (f64)dir.x);
    NTEST_LOG("lift %.5f m, turn %.4f rad/s, slow %.5f, wander %.4f m over %.3f m", out.lift,
              out.turn, out.slow, out.wander, out.travelled);
    scene_end(&s);
    return out;
}

// A box and a capsule sliding frictionless, and a ball and a capsule
// rolling, across `floor`: any bump shows as lift, a turn, or lost speed.
// (Not a vertical speed: a rolling shape's anchor turns up off the floor
// within a step, so its last substeps go unsupported and it ends each step
// falling at half g dt while its height holds, on any ground.)
static void glide_all(NphPhysicsGeometry floor, NphDVec3 at, NphVec3 dir, f32 speed)
{
    const f32 heading = atan2f(dir.z, dir.x);
    const NphQuat across = nph_quat_mul(yaw(0.5F * k_pi - heading), lying()); // axis across it
    Glide g[4];
    g[0] = glide(floor, at, nph_physics_geometry_box(nph_vec3(0.3F, 0.15F, 0.2F)), yaw(0.3F), 0.15,
                 0.0F, false, 0.0F, dir, speed);
    g[1] = glide(floor, at, nph_physics_geometry_capsule(0.3F, 0.1F), across, 0.1, 0.0F, false,
                 0.0F, dir, speed);
    g[2] = glide(floor, at, nph_physics_geometry_sphere(0.25F), nph_quat_identity(), 0.25, 0.8F,
                 true, 0.25F, dir, speed);
    g[3] = glide(floor, at, nph_physics_geometry_capsule(0.3F, 0.1F), across, 0.1, 0.8F, true, 0.1F,
                 dir, speed);
    for (u32 k = 0U; k < 4U; ++k) {
        NTEST_EXPECT_TRUE(g[k].lift < 1e-3);
        NTEST_EXPECT_TRUE(g[k].turn < 0.05);
        NTEST_EXPECT_TRUE(g[k].slow < 0.01);
        NTEST_EXPECT_TRUE(g[k].wander < 0.01);
        // Two seconds and the twelve settling steps.
        NTEST_EXPECT_NEAR_F64(g[k].travelled, (f64)speed * 492.0 / 240.0, 0.06);
    }
}

NTEST(world_mesh, bodies_slide_and_roll_across_seams_without_bumping)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    // Quarter-metre cells: a body crosses a seam or a vertex every few steps,
    // along a heading that meets all three seam directions.
    NphPhysicsMesh *floor = test_grid_mesh(ma, 40U, 0.25F, -5.0F, -5.0F, NULL, false);
    const NphVec3 dir = nph_vec3_normalize(nph_vec3(0.956F, 0.0F, 0.292F));
    const f32 speed = 3.0F;
    glide_all(nph_physics_geometry_mesh(floor), nph_dvec3_zero(), dir, speed);
    // The same slide over open edges bumps: what the test above measures is
    // the active edges at work, not a floor too smooth to matter.
    NphPhysicsMesh *open = unwelded_floor(ma, 40U, 0.25F, -5.0F, -5.0F);
    const Glide bumpy = glide(nph_physics_geometry_mesh(open), nph_dvec3_zero(),
                              nph_physics_geometry_box(nph_vec3(0.3F, 0.15F, 0.2F)), yaw(0.3F),
                              0.15, 0.0F, false, 0.0F, dir, speed);
    NTEST_EXPECT_TRUE(bumpy.lift > 2e-3 || bumpy.turn > 0.2 || bumpy.slow > 0.05);
    nph_physics_mesh_free(open, ma);
    nph_physics_mesh_free(floor, ma);
    test_alloc_end(&mta);
}

NTEST(world_mesh, a_box_settles_in_a_crease_on_two_manifolds)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *valley = test_valley_mesh(ma);
    Scene s;
    scene_begin(&s);
    add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), valley);
    // A cube turned to rest a face on each slope, dropped 5 cm.
    const NphQuat turn = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.25F * k_pi);
    const f64 rest = 0.3 * sqrt(2.0);
    const NphPhysicsBody cube =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, rest + 0.05, 0.3), turn,
                 nph_physics_geometry_box(nph_vec3_splat(0.3F)), 0.6F, 0.0F);
    for (u32 i = 0U; i < 240U * 2U; ++i) {
        step(s.w, NULL, 1U);
        if (i == 96U) {
            // One pair, two manifolds of four: the pair's contact and a
            // child.
            const NphPhysicsStats st = nph_physics_world_stats(s.w);
            NTEST_EXPECT_EQ_UINT(st.contacts, 1U);
            NTEST_EXPECT_EQ_UINT(st.touching_contacts, 1U);
            NTEST_EXPECT_EQ_UINT(st.manifold_points, 8U);
            NTEST_EXPECT_EQ_UINT(s.w->child_contacts, NPH_PHYSICS_MESH_MAX_MANIFOLDS - 1U);
        }
        if (i % 60U == 0U) {
            NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
        }
    }
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, cube));
    const NphDVec3 p = nph_physics_body_position(s.w, cube);
    NTEST_EXPECT_NEAR_F64(p.x, 0.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(p.z, 0.3, 1e-3);
    NTEST_EXPECT_NEAR_F64(p.y, rest, 6e-3);
    const NphQuat q = nph_physics_body_rotation(s.w, cube);
    NTEST_EXPECT_TRUE(nph_quat_angle(nph_quat_mul(nph_quat_conjugate(turn), q)) < 2e-3F);
    scene_end(&s);
    nph_physics_mesh_free(valley, ma);
    test_alloc_end(&mta);
}

// Warm starting by place across a pair's manifolds (contact.c): the cube in
// the crease, its two manifolds' points robbed of their ids, steps as its
// twin does, to the bit (each corner on the crease is in both manifolds, a
// hair apart: each takes its own manifold's impulses); moved 2 cm along the
// crease as well, its contact starts cold.
NTEST(world_mesh, points_keep_their_impulses_across_a_pairs_manifolds)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *valley = test_valley_mesh(ma);
    const NphQuat turn = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.25F * k_pi);
    Scene s[3];
    for (u32 k = 0U; k < 3U; ++k) {
        NphPhysicsWorldDesc d = nph_physics_world_desc_default();
        d.enable_sleep = 0;
        scene_begin_with(&s[k], d);
        add_mesh_body(s[k].w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), valley);
        add_body(s[k].w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 0.3 * sqrt(2.0), 0.3), turn,
                 nph_physics_geometry_box(nph_vec3_splat(0.3F)), 0.6F, 0.0F);
        step(s[k].w, NULL, 240U);
        NTEST_ASSERT_EQ_UINT(nph_physics_world_stats(s[k].w).manifold_points, 8U);
    }
    NTEST_ASSERT_EQ_UINT(nph_physics_world_hash(s[0].w), nph_physics_world_hash(s[1].w));
    scramble_contact_points(s[1].w, nph_vec3_zero());
    scramble_contact_points(s[2].w, nph_vec3(0.0F, 0.0F, 0.02F));
    for (u32 k = 0U; k < 3U; ++k) {
        step(s[k].w, NULL, 1U);
    }
    NTEST_EXPECT_EQ_UINT(nph_physics_world_hash(s[0].w), nph_physics_world_hash(s[1].w));
    NTEST_EXPECT_NE_UINT(nph_physics_world_hash(s[0].w), nph_physics_world_hash(s[2].w));
    for (u32 k = 0U; k < 3U; ++k) {
        scene_end(&s[k]);
    }
    nph_physics_mesh_free(valley, ma);
    test_alloc_end(&mta);
}

// The matching itself, on the crease: a corner on the crease is in both
// slopes' manifolds, under a millimetre apart. Taken out of one manifold
// (and every id scrambled), its new point there finds the other manifold's
// copy within the matching distance and must not take its impulses, which
// push along the other slope's normal: it starts cold, and the copy's own
// new point keeps them. Each manifold in turn loses the corner, so either
// order of the records is met.
NTEST(world_mesh, points_take_impulses_only_from_their_own_manifold)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *valley = test_valley_mesh(ma);
    const NphQuat turn = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.25F * k_pi);
    for (u32 from = 0U; from < 2U; ++from) {
        Scene s;
        NphPhysicsWorldDesc d = nph_physics_world_desc_default();
        d.enable_sleep = 0;
        scene_begin_with(&s, d);
        add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), valley);
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 0.3 * sqrt(2.0), 0.3), turn,
                 nph_physics_geometry_box(nph_vec3_splat(0.3F)), 0.6F, 0.0F);
        step(s.w, NULL, 240U);
        NTEST_ASSERT_EQ_UINT(s.w->awake_contacts.len, 1U);
        const u32 pair = s.w->awake_contacts.data[0];
        PhysContact *rec[2] = {&s.w->contacts.data[pair], NULL};
        NTEST_ASSERT_NE_UINT(rec[0]->child, PHYS_NULL);
        rec[1] = &s.w->contacts.data[rec[0]->child];
        NTEST_ASSERT_TRUE(rec[0]->point_count == 4U && rec[1]->point_count == 4U);
        // The corner: the nearest two points across the manifolds.
        u32 bi = 0U;
        u32 bj = 0U;
        f32 bd = FLT_MAX;
        for (u32 i = 0U; i < 4U; ++i) {
            for (u32 j = 0U; j < 4U; ++j) {
                const f32 dd =
                    nph_vec3_distance_sq(rec[0]->points[i].anchor_a, rec[1]->points[j].anchor_a);
                if (dd < bd) {
                    bd = dd;
                    bi = i;
                    bj = j;
                }
            }
        }
        NTEST_ASSERT_TRUE(bd < 1e-6F); // within a millimetre: inside the matching distance
        PhysContact *robbed = rec[from];
        const u32 taken = from == 0U ? bi : bj;
        const PhysContactPoint copy = rec[from ^ 1U]->points[from == 0U ? bj : bi];
        const NphVec3 at = robbed->points[taken].anchor_a;
        const NphVec3 robbed_normal = robbed->normal;
        NTEST_ASSERT_TRUE(copy.normal_impulse > 0.0F);
        robbed->points[taken] = robbed->points[robbed->point_count - 1U];
        robbed->point_count -= 1U;
        scramble_contact_points(s.w, nph_vec3_zero());
        phys_contact_update(s.w, pair);
        // The corner's new points, in the robbed manifold and the other.
        u32 found = 0U;
        for (u32 k = pair; k != PHYS_NULL; k = s.w->contacts.data[k].child) {
            const PhysContact *r = &s.w->contacts.data[k];
            const bool own = nph_vec3_dot(r->normal, robbed_normal) > 0.99F;
            for (u32 j = 0U; j < r->point_count; ++j) {
                const PhysContactPoint *q = &r->points[j];
                if (nph_vec3_distance_sq(q->anchor_a, at) > 1e-6F) {
                    continue;
                }
                found += 1U;
                if (own) {
                    NTEST_EXPECT_NEAR_F32(q->normal_impulse, 0.0F, 0.0F);
                    NTEST_EXPECT_NEAR_F32(nph_vec3_length_sq(q->tangent_impulse), 0.0F, 0.0F);
                } else {
                    NTEST_EXPECT_NEAR_F32(q->normal_impulse, copy.normal_impulse, 0.0F);
                }
            }
        }
        NTEST_EXPECT_EQ_UINT(found, 2U);
        scene_end(&s);
    }
    nph_physics_mesh_free(valley, ma);
    test_alloc_end(&mta);
}

NTEST(world_mesh, kinematic_meshes_carry_what_rests_on_them)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *deck = test_grid_mesh(ma, 4U, 1.0F, -2.0F, -2.0F, NULL, false);
    Scene s;
    scene_begin(&s);
    const NphPhysicsBody platform =
        add_mesh_body(s.w, NPH_PHYSICS_BODY_KINEMATIC, nph_dvec3_zero(), deck);
    const NphPhysicsBody box =
        add_box(s.w, nph_dvec3(-0.5, 0.3 - 0.0005, 0.2), nph_vec3_splat(0.3F));
    step(s.w, NULL, 240U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, box));
    // The platform sets off at 1 m/s: the box slips until friction has it
    // up to speed (v^2 / (2 mu g), 8.5 cm), then rides along.
    nph_physics_body_set_linear_velocity(s.w, platform, nph_vec3(1.0F, 0.0F, 0.0F));
    step(s.w, NULL, 480U);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, platform).x, 2.0, 1e-3);
    const NphDVec3 p = nph_physics_body_position(s.w, box);
    NTEST_EXPECT_NEAR_F64(p.x, -0.5 + 2.0 - 0.085, 0.03);
    NTEST_EXPECT_NEAR_F64(p.y, 0.3, 6e-3);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_linear_velocity(s.w, box).x, 1.0, 0.01);
    scene_end(&s);
    nph_physics_mesh_free(deck, ma);
    test_alloc_end(&mta);
}

NTEST(world_mesh, mesh_pairs_report_their_events_once)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *valley = test_valley_mesh(ma);
    Scene s;
    scene_begin(&s);
    const NphPhysicsBody ground =
        add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), valley);
    NphPhysicsShape ground_shape;
    NTEST_ASSERT_EQ_UINT(nph_physics_body_shapes(s.w, ground, &ground_shape, 1U), 1U);
    // A cube falling 30 cm into the crease meets both slopes, together.
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.position = nph_dvec3(0.0, 0.3 * sqrt(2.0) + 0.3, 0.0);
    bd.rotation = nph_quat_from_axis_angle(nph_vec3_unit_z(), 0.25F * k_pi);
    NphPhysicsBody cube;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &cube), NPH_RESULT_OK);
    NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_box(nph_vec3_splat(0.3F)));
    sd.hit_events = 1;
    NphPhysicsShape cube_shape;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, cube, &sd, &cube_shape), NPH_RESULT_OK);
    u32 begins = 0U;
    u32 hits = 0U;
    u32 most_points = 0U;
    for (u32 i = 0U; i < 240U; ++i) {
        step(s.w, NULL, 1U);
        const NphPhysicsEvents e = nph_physics_world_events(s.w);
        for (u32 k = 0U; k < e.begin_count; ++k) {
            const bool ours = (e.begin[k].shape_a.value == cube_shape.value &&
                               e.begin[k].shape_b.value == ground_shape.value) ||
                              (e.begin[k].shape_b.value == cube_shape.value &&
                               e.begin[k].shape_a.value == ground_shape.value);
            begins += ours ? 1U : 0U;
        }
        // One hit at most a step for the pair, however many of its
        // manifolds approach.
        NTEST_EXPECT_TRUE(e.hit_count <= 1U);
        hits += e.hit_count;
        NTEST_EXPECT_EQ_UINT(e.end_count, 0U);
        most_points = nph_max_u32(most_points, nph_physics_world_stats(s.w).manifold_points);
    }
    // One begin; a hit for each slope, the first a step or so before the
    // other (whichever rounding brings in first), or one for both.
    NTEST_EXPECT_EQ_UINT(begins, 1U);
    NTEST_EXPECT_TRUE(hits >= 1U && hits <= 2U);
    NTEST_EXPECT_TRUE(most_points > NPH_PHYSICS_MANIFOLD_MAX_POINTS); // both manifolds touched
    // Lifted away: one end.
    nph_physics_body_set_transform(s.w, cube, nph_dvec3(0.0, 5.0, 0.0), nph_quat_identity());
    step(s.w, NULL, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_events(s.w).end_count, 1U);
    scene_end(&s);
    nph_physics_mesh_free(valley, ma);
    test_alloc_end(&mta);
}

// The terrain's height: a bowl (what rolls stays on it), long swells, and
// short ripples, over 12 m by 12 m, nowhere steeper than 24 degrees (below
// the friction angle, so a box can rest anywhere on it).
static f64 terrain_height(f64 x, f64 z)
{
    return 0.015 * (x * x + z * z) + 0.15 * sin(0.9 * x) * cos(0.7 * z) +
           0.04 * sin(3.1 * x + 1.3 * z);
}

static NphPhysicsMesh *terrain(NphAllocator *alloc)
{
    static f32 y[49U * 49U];
    for (u32 i = 0U; i <= 48U; ++i) {
        for (u32 j = 0U; j <= 48U; ++j) {
            y[i * 49U + j] = (f32)terrain_height(-6.0 + 0.25 * (f64)i, -6.0 + 0.25 * (f64)j);
        }
    }
    return test_grid_mesh(alloc, 48U, 0.25F, -6.0F, -6.0F, y, false);
}

// Boxes, spheres, and capsules dropped over the terrain, one every 20 cm of
// height above it (their spread keeps them apart).
static void drop_mixed(NphPhysicsWorld *w, u32 count, u64 seed)
{
    NphPcg32 rng = test_rng(seed);
    for (u32 i = 0U; i < count; ++i) {
        const f64 x = (f64)test_range(&rng, -4.5F, 4.5F);
        const f64 z = (f64)test_range(&rng, -4.5F, 4.5F);
        const NphDVec3 p = nph_dvec3(x, terrain_height(x, z) + 0.7 + 0.2 * (f64)i, z);
        NphPhysicsGeometry g;
        switch (i % 3U) {
        case 0U:
            g = nph_physics_geometry_box(test_vec3(&rng, 0.15F, 0.4F));
            break;
        case 1U:
            g = nph_physics_geometry_sphere(test_range(&rng, 0.15F, 0.35F));
            break;
        default:
            g = nph_physics_geometry_capsule(test_range(&rng, 0.1F, 0.4F),
                                             test_range(&rng, 0.1F, 0.25F));
            break;
        }
        add_body(w, NPH_PHYSICS_BODY_DYNAMIC, p, test_rotation(&rng), g, 0.5F, 0.1F);
    }
}

NTEST(world_mesh, a_pile_on_a_terrain_keeps_the_invariants_and_its_course)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *land = terrain(ma);
    const u32 bodies = 12U * TEST_SCALE;
    const u32 workers[4] = {0U, 1U, 4U, 8U};
    const u32 forced[4] = {0U, 2U, 5U, 9U};
    u64 reference = 0U;
    for (u32 run = 0U; run < 4U; ++run) {
        NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
        Scene s;
        scene_begin(&s);
        s.w->forced_participants = forced[run];
        const NphPhysicsBody ground =
            add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), land);
        drop_mixed(s.w, bodies, 31U);
        NphPcg32 rng = test_rng(32U);
        for (u32 i = 0U; i < 600U; ++i) {
            step(s.w, js, 1U);
            if (run == 0U && i % 50U == 0U) {
                NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
            }
            if (i % 97U == 0U) {
                // Teleport a new box into the pile.
                const NphPhysicsBody b =
                    add_box(s.w, nph_dvec3(0.0, 6.0, 0.0), test_vec3(&rng, 0.2F, 0.4F));
                nph_physics_body_set_transform(s.w, b, nph_dvec3(0.5, 1.5, 0.5),
                                               test_rotation(&rng));
            }
        }
        const u64 h = nph_physics_world_hash(s.w);
        if (run == 0U) {
            reference = h;
            // Everything still on the terrain, nothing fallen through.
            NphPhysicsStats st = nph_physics_world_stats(s.w);
            NTEST_LOG("%u bodies, %u pairs (%u touching, %u points), %u children", st.bodies,
                      st.contacts, st.touching_contacts, st.manifold_points, s.w->child_contacts);
        } else {
            NTEST_EXPECT_EQ_UINT(h, reference);
        }
        // The terrain goes, and every child contact with it.
        nph_physics_body_destroy(s.w, ground);
        NTEST_EXPECT_EQ_UINT(s.w->child_contacts, 0U);
        NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
        step(s.w, js, 10U);
        scene_end(&s);
        nph_job_system_destroy(js);
    }
    nph_physics_mesh_free(land, ma);
    test_alloc_end(&mta);
}

// The bodies over the terrain (within `margin` of its edge) whose centres
// are below its surface: none, unless something fell through.
static u32 under_terrain(const NphPhysicsWorld *w, f64 margin)
{
    u32 under = 0U;
    for (u32 i = 0U; i < w->bodies.len; ++i) {
        const PhysBody *b = &w->bodies.data[i];
        if (b->handle != 0U && b->type == NPH_PHYSICS_BODY_DYNAMIC &&
            fabs(b->center.x) < 6.0 - margin && fabs(b->center.z) < 6.0 - margin &&
            !(b->center.y > terrain_height(b->center.x, b->center.z) - 0.005)) {
            under += 1U;
        }
    }
    return under;
}

NTEST(world_mesh, nothing_falls_through_a_terrain)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *land = terrain(ma);
    Scene s;
    scene_begin(&s);
    add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), land);
    // Dropped as high as 20 m (landing at 20 m/s, 8 cm a step) onto the
    // terrain and onto each other: through the one-sided surface, none.
    const u32 count = 12U * TEST_SCALE;
    drop_mixed(s.w, count, 41U);
    for (u32 i = 0U; i < 240U * 6U; i += 10U) {
        step(s.w, NULL, 10U);
        NTEST_ASSERT_EQ_UINT(under_terrain(s.w, 0.1), 0U);
    }
    // Some leave over the rim when the pile hits them hard enough.
    u32 on = 0U;
    for (u32 i = 0U; i < s.w->bodies.len; ++i) {
        const PhysBody *b = &s.w->bodies.data[i];
        on += b->handle != 0U && b->type == NPH_PHYSICS_BODY_DYNAMIC && fabs(b->center.x) < 6.0 &&
                      fabs(b->center.z) < 6.0
                  ? 1U
                  : 0U;
    }
    NTEST_LOG("%u of %u still on the terrain", on, count);
    NTEST_EXPECT_TRUE(on * 2U >= count);
    scene_end(&s);
    nph_physics_mesh_free(land, ma);
    test_alloc_end(&mta);
}

NTEST(world_mesh, boxes_come_to_rest_on_a_terrain)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *land = terrain(ma);
    Scene s;
    scene_begin(&s);
    add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), land);
    // Boxes, which do not roll, across the swells and ripples: they settle
    // on slopes, seams, and each other, and every one sleeps. Most do in a
    // few seconds; a box whose face lies on a swell's crown rolls on it as
    // a rocking chair's runner rolls on the floor, losing nothing but what
    // the mesh's seams take from each swing, and the last of forty took
    // 17 s in sixty such scenes. Dropped from up to 12 m onto each other,
    // one may be knocked off the terrain's edge (in two of those scenes):
    // it falls for good, and is not waited for.
    const u32 count = 5U * TEST_SCALE;
    NphPcg32 rng = test_rng(51U);
    NphPhysicsBody boxes[40];
    NTEST_ASSERT_TRUE(count <= 40U);
    for (u32 i = 0U; i < count; ++i) {
        const f64 x = (f64)test_range(&rng, -4.0F, 4.0F);
        const f64 z = (f64)test_range(&rng, -4.0F, 4.0F);
        boxes[i] = add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC,
                            nph_dvec3(x, terrain_height(x, z) + 0.6 + 0.3 * (f64)i, z),
                            test_rotation(&rng),
                            nph_physics_geometry_box(test_vec3(&rng, 0.15F, 0.4F)), 0.6F, 0.0F);
    }
    u32 asleep = 0U;
    u32 gone = 0U;
    u32 most = 0U; // the step by which nine in ten slept
    u32 k = 0U;
    for (; k < 240U * 30U && asleep + gone < count; ++k) {
        NTEST_ASSERT_EQ_INT(nph_physics_world_step(s.w, DT, NULL), NPH_RESULT_OK);
        asleep = 0U;
        gone = 0U;
        for (u32 i = 0U; i < count; ++i) {
            const NphDVec3 p = nph_physics_body_position(s.w, boxes[i]);
            if (!(fabs(p.x) < 6.0 && fabs(p.z) < 6.0)) {
                gone += 1U;
            } else if (!nph_physics_body_is_awake(s.w, boxes[i])) {
                asleep += 1U;
            }
        }
        most = most == 0U && asleep * 10U >= (count - gone) * 9U ? k + 1U : most;
    }
    const NphPhysicsStats st = nph_physics_world_stats(s.w);
    NTEST_LOG("%u of %u boxes asleep after %.2f s (nine in ten after %.2f s), %u off the edge, %u "
              "awake islands",
              asleep, count, (f64)k * (f64)DT, (f64)most * (f64)DT, gone, st.awake_islands);
    NTEST_EXPECT_EQ_UINT(asleep + gone, count);
    NTEST_EXPECT_TRUE(gone <= 1U);
    NTEST_EXPECT_EQ_UINT(under_terrain(s.w, 0.1), 0U);
    scene_end(&s);
    nph_physics_mesh_free(land, ma);
    test_alloc_end(&mta);
}

// Boxes that once rocked on the terrain for good, each alone where it
// rocked (as found by sixty scenes of boxes dropped on it, 30 s in): one
// box's face across a seam kept, of its points, one a few tenths of a
// millimetre farther from its triangle than its neighbour a few
// centimetres away, and the neighbour sank unseen each time the box rocked
// onto it; another's edge across a ridge brought points in a line whose
// middle, the first to touch, the cluster's compaction dropped
// (collide.c, collide_mesh.c).
typedef struct RockedBox {
    f32 half[3];
    f64 origin[3];
    f32 rotation[4];
    f32 v[3];
    f32 w[3];
} RockedBox;

static const RockedBox k_rocked[4] = {
    {{0.263268948F, 0.226608664F, 0.222357884F},
     {-0.813163389042, 0.571290270816, 3.77859713725},
     {-0.451463401F, 0.0914111361F, 0.885298073F, 0.0638146326F},
     {0.015386682F, 0.00232245517F, 0.013443823F},
     {0.0589347668F, -0.0187158678F, -0.0642214641F}},
    {{0.327025175F, 0.194660679F, 0.266864359F},
     {1.15041730939, 0.464570692377, 0.0276558416508},
     {0.289247036F, 0.26828891F, 0.660987973F, 0.638319731F},
     {-0.0272281449F, -0.00970178191F, 0.0312481225F},
     {0.0967307165F, -0.00144110061F, 0.0838418081F}},
    {{0.347755432F, 0.29911828F, 0.28294307F},
     {-0.408639715776, 0.29973526791, -1.42467261583},
     {0.0443149097F, 0.744474769F, -0.665915489F, 0.0187128782F},
     {0.0202726182F, 0.00157122873F, -0.0195992794F},
     {-0.069420062F, 0.0102136862F, -0.0709844679F}},
    {{0.342876196F, 0.200958028F, 0.385083854F},
     {-0.150285051786, 0.266297361238, -1.89414262585},
     {0.772569954F, 0.021445239F, -0.632060647F, -0.0563473739F},
     {-0.0210849512F, 0.00245124567F, 0.0179050211F},
     {0.0871955901F, -0.0154111581F, 0.104606755F}},
};

NTEST(world_mesh, boxes_resting_on_a_terrain_never_rock_for_good)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsMesh *land = terrain(ma);
    // Each placed at rest, moving as it was, and moving twice as fast: it
    // sleeps within 10 s (from 0.5 s, the least, to 6.6 s here; nine of the
    // twelve never slept before).
    f64 slowest = 0.0;
    for (u32 c = 0U; c < 4U; ++c) {
        const RockedBox *r = &k_rocked[c];
        for (u32 kick = 0U; kick < 3U; ++kick) {
            Scene s;
            scene_begin(&s);
            add_mesh_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), land);
            const NphPhysicsBody b = add_body(
                s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(r->origin[0], r->origin[1], r->origin[2]),
                nph_quat_normalize(
                    nph_quat(r->rotation[0], r->rotation[1], r->rotation[2], r->rotation[3])),
                nph_physics_geometry_box(nph_vec3(r->half[0], r->half[1], r->half[2])), 0.6F, 0.0F);
            const f32 scale = (f32)kick;
            nph_physics_body_set_linear_velocity(
                s.w, b, nph_vec3_scale(nph_vec3(r->v[0], r->v[1], r->v[2]), scale));
            nph_physics_body_set_angular_velocity(
                s.w, b, nph_vec3_scale(nph_vec3(r->w[0], r->w[1], r->w[2]), scale));
            u32 k = 0U;
            while (k < 240U * 10U && nph_physics_body_is_awake(s.w, b)) {
                NTEST_ASSERT_EQ_INT(nph_physics_world_step(s.w, DT, NULL), NPH_RESULT_OK);
                k += 1U;
            }
            NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, b));
            slowest = nph_f64_max(slowest, (f64)k * (f64)DT);
            NTEST_EXPECT_EQ_UINT(under_terrain(s.w, 0.1), 0U);
            scene_end(&s);
        }
    }
    NTEST_LOG("the slowest asleep after %.2f s", slowest);
    nph_physics_mesh_free(land, ma);
    test_alloc_end(&mta);
}

// One box slides from (1, 0.2, 0.5) over `floor` placed at `at`, in a world
// where everything is offset by `world`: its path, relative to `world`.
static NphDVec3 slide_on(NphPhysicsGeometry floor, NphDVec3 at, NphDVec3 world)
{
    Scene s;
    scene_begin(&s);
    add_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_add(world, at), nph_quat_identity(), floor,
             0.6F, 0.0F);
    const NphPhysicsBody b = add_body(
        s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3_add(world, nph_dvec3(1.0, 0.2 - 0.0005, 0.5)),
        yaw(0.2F), nph_physics_geometry_box(nph_vec3(0.3F, 0.2F, 0.25F)), 0.3F, 0.0F);
    nph_physics_body_set_linear_velocity(s.w, b, nph_vec3(2.0F, 0.0F, 1.0F));
    step(s.w, NULL, 360U);
    const NphDVec3 p = nph_dvec3_sub(nph_physics_body_position(s.w, b), world);
    scene_end(&s);
    return p;
}

NTEST(world_mesh, meshes_far_from_their_origin_or_the_worlds_behave_as_at_home)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    // The same floor three ways: at home; with its vertices 10 km along X
    // and its body 10 km back (exact: every coordinate is a multiple of a
    // half); and at home in a world 10 000 km out.
    NphPhysicsMesh *home = test_grid_mesh(ma, 24U, 0.5F, -6.0F, -6.0F, NULL, false);
    NphPhysicsMesh *shifted = test_grid_mesh(ma, 24U, 0.5F, 10000.0F - 6.0F, -6.0F, NULL, false);
    const NphDVec3 a =
        slide_on(nph_physics_geometry_mesh(home), nph_dvec3_zero(), nph_dvec3_zero());
    const NphDVec3 b = slide_on(nph_physics_geometry_mesh(shifted), nph_dvec3(-10000.0, 0.0, 0.0),
                                nph_dvec3_zero());
    const NphDVec3 c =
        slide_on(nph_physics_geometry_mesh(home), nph_dvec3_zero(), nph_dvec3(1.0e7, 0.0, -3.0e6));
    NTEST_LOG("home (%.5f %.5f %.5f) shifted (%.5f %.5f %.5f) far (%.5f %.5f %.5f)", a.x, a.y, a.z,
              b.x, b.y, b.z, c.x, c.y, c.z);
    NTEST_EXPECT_TRUE(a.x > 1.5); // it slid (0.85 m before friction stops it)
    NTEST_EXPECT_NEAR_F64(b.x, a.x, 5e-4);
    NTEST_EXPECT_NEAR_F64(b.y, a.y, 5e-4);
    NTEST_EXPECT_NEAR_F64(b.z, a.z, 5e-4);
    NTEST_EXPECT_NEAR_F64(c.x, a.x, 5e-4);
    NTEST_EXPECT_NEAR_F64(c.y, a.y, 5e-4);
    NTEST_EXPECT_NEAR_F64(c.z, a.z, 5e-4);
    nph_physics_mesh_free(home, ma);
    nph_physics_mesh_free(shifted, ma);
    test_alloc_end(&mta);
}

// ---------------------------------------------------------------------------
// Heightfields
// ---------------------------------------------------------------------------

// The terrain as a heightfield: the same heights at the same places (its
// first sample at (-6, -6)), cells flipped alternately as the mesh's are.
static NphPhysicsHeightfield *terrain_field(NphAllocator *alloc)
{
    static f32 y[49U * 49U];
    static u8 cells[48U * 48U];
    for (u32 j = 0U; j <= 48U; ++j) {
        for (u32 i = 0U; i <= 48U; ++i) {
            y[j * 49U + i] = (f32)terrain_height(-6.0 + 0.25 * (f64)i, -6.0 + 0.25 * (f64)j);
        }
    }
    for (u32 c = 0U; c < 48U * 48U; ++c) {
        cells[c] = ((c / 48U + c % 48U) & 1U) != 0U ? NPH_PHYSICS_HEIGHTFIELD_FLIP : 0U;
    }
    return test_heightfield(alloc, 49U, 49U, 0.25F, y, cells);
}

static NphPhysicsBody add_field_body(NphPhysicsWorld *w, NphPhysicsBodyType type, NphDVec3 p,
                                     const NphPhysicsHeightfield *h)
{
    return add_body(w, type, p, nph_quat_identity(), nph_physics_geometry_heightfield(h), 0.6F,
                    0.0F);
}

NTEST(world_heightfield, heightfields_go_on_static_and_kinematic_bodies)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    static f32 flat[9U * 9U];
    NphPhysicsHeightfield *h = test_heightfield(ma, 9U, 9U, 0.5F, flat, NULL);
    Scene s;
    scene_begin(&s);
    const NphPhysicsShapeDesc sd =
        nph_physics_shape_desc_default(nph_physics_geometry_heightfield(h));
    NphPhysicsShape shape;
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    NphPhysicsBody body;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &body), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(s.w, body, &sd, &shape),
                        NPH_RESULT_INVALID_ARGUMENT);
    const NphPhysicsBodyType types[2] = {NPH_PHYSICS_BODY_STATIC, NPH_PHYSICS_BODY_KINEMATIC};
    for (u32 k = 0U; k < 2U; ++k) {
        bd.type = types[k];
        bd.position = nph_dvec3(10.0, 1.0, 5.0 * (f64)k);
        NTEST_ASSERT_EQ_INT(nph_physics_body_create(s.w, &bd, &body), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, body, &sd, &shape), NPH_RESULT_OK);
        const NphAABB box = nph_physics_shape_aabb(s.w, shape);
        NTEST_EXPECT_TRUE(nph_vec3_near(box.min, nph_vec3(10.0F, 1.0F, 5.0F * (f32)k), 1e-5F));
        NTEST_EXPECT_TRUE(
            nph_vec3_near(box.max, nph_vec3(14.0F, 1.0F, 5.0F * (f32)k + 4.0F), 1e-5F));
    }
    const NphPhysicsShapeDesc none =
        nph_physics_shape_desc_default(nph_physics_geometry_heightfield(NULL));
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(s.w, body, &none, &shape),
                        NPH_RESULT_INVALID_ARGUMENT);
    scene_end(&s);
    nph_physics_heightfield_free(h, ma);
    test_alloc_end(&mta);
}

NTEST(world_heightfield, shapes_rest_and_sleep_on_a_heightfield)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    static f32 flat[17U * 17U];
    NphPhysicsHeightfield *h = test_heightfield(ma, 17U, 17U, 0.5F, flat, NULL);
    Scene s;
    scene_begin(&s);
    add_field_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(-4.0, 0.0, -4.0), h);
    // As on the mesh floor: a box turned across the cells, a ball over a
    // corner, a capsule lying across a cell, dropped 5 cm.
    const NphQuat box_turn = yaw(0.4F);
    const NphQuat cap_turn = nph_quat_mul(yaw(0.7F), lying());
    const NphPhysicsBody bodies[3] = {
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.3, 0.3, -0.2), box_turn,
                 nph_physics_geometry_box(nph_vec3(0.5F, 0.25F, 0.4F)), 0.6F, 0.0F),
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-2.0, 0.35, 1.5), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.3F), 0.6F, 0.0F),
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(2.1, 0.15, -2.2), cap_turn,
                 nph_physics_geometry_capsule(0.4F, 0.1F), 0.6F, 0.0F)};
    const NphDVec3 starts[3] = {nph_dvec3(0.3, 0.25, -0.2), nph_dvec3(-2.0, 0.3, 1.5),
                                nph_dvec3(2.1, 0.1, -2.2)};
    for (u32 i = 0U; i < 240U * 3U; ++i) {
        step(s.w, NULL, 1U);
        if (i == 96U) {
            // Landed, not yet asleep: the box's four points, the ball's one,
            // the capsule's two (its ends, however many cells it lies on).
            const NphPhysicsStats st = nph_physics_world_stats(s.w);
            NTEST_EXPECT_EQ_UINT(st.touching_contacts, 3U);
            NTEST_EXPECT_EQ_UINT(st.manifold_points, 7U);
        }
        if (i % 60U == 0U) {
            NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
        }
    }
    for (u32 k = 0U; k < 3U; ++k) {
        NTEST_EXPECT_FALSE(nph_physics_body_is_awake(s.w, bodies[k]));
        const NphDVec3 p = nph_physics_body_position(s.w, bodies[k]);
        NTEST_EXPECT_NEAR_F64(p.x, starts[k].x, 2e-3);
        NTEST_EXPECT_NEAR_F64(p.z, starts[k].z, 2e-3);
        NTEST_EXPECT_NEAR_F64(p.y, starts[k].y, 6e-3);
    }
    scene_end(&s);
    nph_physics_heightfield_free(h, ma);
    test_alloc_end(&mta);
}

NTEST(world_heightfield, bodies_slide_and_roll_across_cells_without_bumping)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    // The mesh test's floor as a heightfield: 10 m of quarter-metre cells,
    // half of them flipped.
    static f32 flat[41U * 41U];
    static u8 cells[40U * 40U];
    for (u32 c = 0U; c < 40U * 40U; ++c) {
        cells[c] = ((c / 40U + c % 40U) & 1U) != 0U ? NPH_PHYSICS_HEIGHTFIELD_FLIP : 0U;
    }
    NphPhysicsHeightfield *h = test_heightfield(ma, 41U, 41U, 0.25F, flat, cells);
    glide_all(nph_physics_geometry_heightfield(h), nph_dvec3(-5.0, 0.0, -5.0),
              nph_vec3_normalize(nph_vec3(0.956F, 0.0F, 0.292F)), 3.0F);
    nph_physics_heightfield_free(h, ma);
    test_alloc_end(&mta);
}

NTEST(world_heightfield, bodies_fall_through_holes)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    // A flat field with a 1 m pit: a box dropped over the pit falls through,
    // one dropped beside it rests, and a ball rolling over the pit's rim
    // drops in.
    static f32 flat[17U * 17U];
    static u8 cells[16U * 16U];
    for (u32 j = 6U; j < 10U; ++j) {
        for (u32 i = 6U; i < 10U; ++i) {
            cells[j * 16U + i] = NPH_PHYSICS_HEIGHTFIELD_HOLE;
        }
    }
    NphPhysicsHeightfield *h = test_heightfield(ma, 17U, 17U, 0.25F, flat, cells);
    Scene s;
    scene_begin(&s);
    add_field_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(-2.0, 0.0, -2.0), h);
    // The pit spans x and z in [-0.5, 0.5].
    const NphPhysicsBody over = add_box(s.w, nph_dvec3(0.0, 0.5, 0.0), nph_vec3_splat(0.2F));
    const NphPhysicsBody beside = add_box(s.w, nph_dvec3(1.2, 0.5, 0.0), nph_vec3_splat(0.2F));
    const NphPhysicsBody ball =
        add_body(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-1.5, 0.15, 0.0), nph_quat_identity(),
                 nph_physics_geometry_sphere(0.15F), 0.6F, 0.0F);
    nph_physics_body_set_linear_velocity(s.w, ball, nph_vec3(1.5F, 0.0F, 0.0F));
    nph_physics_body_set_angular_velocity(s.w, ball, nph_vec3(0.0F, 0.0F, -10.0F));
    step(s.w, NULL, 240U * 2U);
    NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, over).y < -1.0);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, beside).y, 0.2, 6e-3);
    NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, ball).y < -1.0);
    scene_end(&s);
    nph_physics_heightfield_free(h, ma);
    test_alloc_end(&mta);
}

NTEST(world_heightfield, a_pile_on_a_heightfield_terrain_keeps_its_course)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    NphPhysicsHeightfield *land = terrain_field(ma);
    const u32 bodies = 12U * TEST_SCALE;
    const u32 workers[4] = {0U, 1U, 4U, 8U};
    u64 reference = 0U;
    for (u32 run = 0U; run < 4U; ++run) {
        NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
        Scene s;
        scene_begin(&s);
        const NphPhysicsBody ground =
            add_field_body(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(-6.0, 0.0, -6.0), land);
        drop_mixed(s.w, bodies, 41U);
        for (u32 i = 0U; i < 600U; ++i) {
            step(s.w, js, 1U);
            if (run == 0U && i % 50U == 0U) {
                NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
                NTEST_EXPECT_EQ_UINT(under_terrain(s.w, 0.5), 0U);
            }
        }
        const u64 hash = nph_physics_world_hash(s.w);
        if (run == 0U) {
            reference = hash;
            const NphPhysicsStats st = nph_physics_world_stats(s.w);
            NTEST_LOG("%u bodies, %u pairs (%u touching, %u points), %u children", st.bodies,
                      st.contacts, st.touching_contacts, st.manifold_points, s.w->child_contacts);
        } else {
            NTEST_EXPECT_EQ_UINT(hash, reference);
        }
        nph_physics_body_destroy(s.w, ground);
        NTEST_EXPECT_EQ_UINT(s.w->child_contacts, 0U);
        NTEST_ASSERT_TRUE(nph_physics_world_validate(s.w));
        scene_end(&s);
        nph_job_system_destroy(js);
    }
    nph_physics_heightfield_free(land, ma);
    test_alloc_end(&mta);
}

NTEST(world_heightfield, heightfields_far_from_the_worlds_origin_behave_as_at_home)
{
    TestAlloc mta;
    NphAllocator *ma = test_alloc_begin(&mta);
    static f32 flat[25U * 25U];
    NphPhysicsHeightfield *h = test_heightfield(ma, 25U, 25U, 0.5F, flat, NULL);
    const NphPhysicsGeometry g = nph_physics_geometry_heightfield(h);
    const NphDVec3 at = nph_dvec3(-6.0, 0.0, -6.0);
    const NphDVec3 a = slide_on(g, at, nph_dvec3_zero());
    const NphDVec3 c = slide_on(g, at, nph_dvec3(1.0e7, 0.0, -3.0e6));
    NTEST_LOG("home (%.5f %.5f %.5f) far (%.5f %.5f %.5f)", a.x, a.y, a.z, c.x, c.y, c.z);
    NTEST_EXPECT_TRUE(a.x > 1.5);
    NTEST_EXPECT_NEAR_F64(c.x, a.x, 5e-4);
    NTEST_EXPECT_NEAR_F64(c.y, a.y, 5e-4);
    NTEST_EXPECT_NEAR_F64(c.z, a.z, 5e-4);
    nph_physics_heightfield_free(h, ma);
    test_alloc_end(&mta);
}
