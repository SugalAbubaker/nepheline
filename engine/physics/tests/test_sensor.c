// SPDX-License-Identifier: Apache-2.0
// Sensors and ghosts (shape.h `sensor` and `ghost`; docs/physics.md "Sensors and ghosts";
// ADR-0108): what a sensor sees and reports, that both touch nothing and weigh nothing, what
// queries see of them, the ends of destroyed shapes, and the order of the events.
#include <nepheline/physics/character.h>
#include <nepheline/physics/query.h>

#include "test_world_support.h"

// A shape on a new body of `type` at `p`.
static NphPhysicsShape add_shape(NphPhysicsWorld *w, NphPhysicsBodyType type, NphDVec3 p,
                                 NphPhysicsShapeDesc sd, NphPhysicsBody *out_body)
{
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.type = type;
    bd.position = p;
    NphPhysicsBody b;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(w, &bd, &b), NPH_RESULT_OK);
    NphPhysicsShape s;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(w, b, &sd, &s), NPH_RESULT_OK);
    if (out_body != NULL) {
        *out_body = b;
    }
    return s;
}

static NphPhysicsShapeDesc sensor_desc(NphPhysicsGeometry g, u64 user_data)
{
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    sd.sensor = 1;
    sd.user_data = user_data;
    return sd;
}

static NphPhysicsShapeDesc solid_desc(NphPhysicsGeometry g, u64 user_data)
{
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    sd.user_data = user_data;
    return sd;
}

static void scene_without_gravity(Scene *s)
{
    NphPhysicsWorldDesc d = nph_physics_world_desc_default();
    d.gravity = nph_vec3_zero();
    scene_begin_with(s, d);
}

// A sphere falls through a sensor slab: it begins overlapping, then ends, once each, the events
// carrying both shapes' user data; it passes through, for the sensor touches nothing.
NTEST(sensor, reports_what_enters_and_leaves)
{
    Scene s;
    scene_begin(&s);
    const NphPhysicsShape sensor =
        add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(0.0, 0.0, 0.0),
                  sensor_desc(nph_physics_geometry_box(nph_vec3(2.0F, 0.5F, 2.0F)), 7U), NULL);
    NphPhysicsBody ball_body;
    const NphPhysicsShape ball =
        add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 3.0, 0.0),
                  solid_desc(nph_physics_geometry_sphere(0.25F), 42U), &ball_body);
    u32 begins = 0U;
    u32 ends = 0U;
    u32 begin_step = 0U;
    u32 end_step = 0U;
    for (u32 i = 1U; i <= 480U; ++i) {
        step(s.w, NULL, 1U);
        const NphPhysicsEvents e = nph_physics_world_events(s.w);
        for (u32 k = 0U; k < e.sensor_begin_count; ++k) {
            NTEST_EXPECT_EQ_UINT(e.sensor_begin[k].sensor.value, sensor.value);
            NTEST_EXPECT_EQ_UINT(e.sensor_begin[k].visitor.value, ball.value);
            NTEST_EXPECT_EQ_UINT(e.sensor_begin[k].sensor_user_data, 7U);
            NTEST_EXPECT_EQ_UINT(e.sensor_begin[k].visitor_user_data, 42U);
            begins += 1U;
            begin_step = i;
        }
        for (u32 k = 0U; k < e.sensor_end_count; ++k) {
            NTEST_EXPECT_EQ_UINT(e.sensor_end[k].visitor.value, ball.value);
            NTEST_EXPECT_EQ_UINT(e.sensor_end[k].visitor_user_data, 42U);
            ends += 1U;
            end_step = i;
        }
    }
    NTEST_EXPECT_EQ_UINT(begins, 1U);
    NTEST_EXPECT_EQ_UINT(ends, 1U);
    NTEST_EXPECT_TRUE(begin_step < end_step);
    NTEST_EXPECT_TRUE(nph_physics_body_position(s.w, ball_body).y < -2.0);
    const NphPhysicsStats st = nph_physics_world_stats(s.w);
    NTEST_EXPECT_EQ_UINT(st.sensors, 1U);
    NTEST_EXPECT_EQ_UINT(st.sensor_visitors, 0U);
    scene_end(&s);
}

// What a sensor sees: moving shapes (dynamic and kinematic) whose category meets its mask, their
// own masks not asked; never static shapes, shapes of other categories, or other sensors.
NTEST(sensor, sees_moving_shapes_its_mask_meets_one_way)
{
    Scene s;
    scene_without_gravity(&s);
    NphPhysicsShapeDesc sd = sensor_desc(nph_physics_geometry_box(nph_vec3_one()), 1U);
    sd.filter.mask = 2U; // sees category 2 alone
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(), sd, NULL);
    const NphPhysicsGeometry small = nph_physics_geometry_sphere(0.2F);
    NphPhysicsShapeDesc seen = solid_desc(small, 2U);
    seen.filter.category = 2U;
    seen.filter.mask = 0U; // touches nothing itself: a sensor still sees it
    const NphPhysicsShape dynamic_seen =
        add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.3, 0.0, 0.0), seen, NULL);
    seen.user_data = 3U;
    const NphPhysicsShape kinematic_seen =
        add_shape(s.w, NPH_PHYSICS_BODY_KINEMATIC, nph_dvec3(-0.3, 0.0, 0.0), seen, NULL);
    seen.user_data = 4U;
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(0.0, 0.3, 0.0), seen, NULL); // static
    NphPhysicsShapeDesc other = solid_desc(small, 5U);
    other.filter.category = 4U; // not in the sensor's mask
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, -0.3, 0.0), other, NULL);
    NphPhysicsShapeDesc another = sensor_desc(small, 6U);
    another.filter.category = 2U; // a sensor: never a visitor
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 0.0, 0.3), another, NULL);
    step(s.w, NULL, 1U);
    const NphPhysicsEvents e = nph_physics_world_events(s.w);
    NTEST_ASSERT_EQ_UINT(e.sensor_begin_count, 2U);
    // In the visitors' handle order.
    const u32 lo =
        dynamic_seen.value < kinematic_seen.value ? dynamic_seen.value : kinematic_seen.value;
    const u32 hi =
        dynamic_seen.value < kinematic_seen.value ? kinematic_seen.value : dynamic_seen.value;
    NTEST_EXPECT_EQ_UINT(e.sensor_begin[0].visitor.value, lo);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin[1].visitor.value, hi);
    // Nothing changed: nothing is reported again.
    step(s.w, NULL, 1U);
    const NphPhysicsEvents e2 = nph_physics_world_events(s.w);
    NTEST_EXPECT_EQ_UINT(e2.sensor_begin_count, 0U);
    NTEST_EXPECT_EQ_UINT(e2.sensor_end_count, 0U);
    scene_end(&s);
}

// A ghost touches nothing (a ball falls through it to the ground), but rays and sensors see it.
NTEST(sensor, ghosts_touch_nothing_but_queries_and_sensors_see_them)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    NphPhysicsShapeDesc gd = solid_desc(nph_physics_geometry_box(nph_vec3(1.0F, 0.5F, 1.0F)), 9U);
    gd.ghost = 1;
    NphPhysicsBody ghost_body;
    const NphPhysicsShape ghost =
        add_shape(s.w, NPH_PHYSICS_BODY_KINEMATIC, nph_dvec3(0.0, 0.5, 0.0), gd, &ghost_body);
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(0.0, 0.5, 0.0),
              sensor_desc(nph_physics_geometry_box(nph_vec3(0.2F, 0.2F, 0.2F)), 1U), NULL);
    NphPhysicsBody ball;
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 3.0, 0.0),
              solid_desc(nph_physics_geometry_sphere(0.25F), 2U), &ball);
    step(s.w, NULL, 1U);
    const NphPhysicsEvents e = nph_physics_world_events(s.w);
    NTEST_ASSERT_EQ_UINT(e.sensor_begin_count, 1U);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin[0].visitor.value, ghost.value);
    step(s.w, NULL, 600U);
    // On the ground, not on the ghost's top at 1 m.
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(s.w, ball).y, 0.25, 0.02);
    NphPhysicsQueryHit hit;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(s.w, nph_dvec3(2.0, 0.5, 0.0),
                                                        nph_vec3(-4.0F, 0.0F, 0.0F), NULL, &hit));
    NTEST_EXPECT_EQ_UINT(hit.body.value, ghost_body.value);
    scene_end(&s);
}

// Neither a sensor nor a ghost adds mass to its body.
NTEST(sensor, sensors_and_ghosts_have_no_mass)
{
    Scene s;
    scene_without_gravity(&s);
    NphPhysicsBody b;
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3_zero(),
              solid_desc(nph_physics_geometry_sphere(0.5F), 0U), &b);
    const f32 mass = nph_physics_body_mass_data(s.w, b).mass;
    NTEST_ASSERT_TRUE(mass > 0.0F);
    NphPhysicsShape extra;
    NphPhysicsShapeDesc sensor = sensor_desc(nph_physics_geometry_box(nph_vec3_one()), 0U);
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, b, &sensor, &extra), NPH_RESULT_OK);
    NphPhysicsShapeDesc ghost = solid_desc(nph_physics_geometry_box(nph_vec3_one()), 0U);
    ghost.ghost = 1;
    NTEST_ASSERT_EQ_INT(nph_physics_shape_create(s.w, b, &ghost, &extra), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(s.w, b).mass, mass, 0.0F);
    scene_end(&s);
}

static bool count_overlap(void *user, NphPhysicsShape shape, NphPhysicsBody body)
{
    NPH_UNUSED(shape);
    NPH_UNUSED(body);
    *(u32 *)user += 1U;
    return true;
}

// Queries skip sensors unless their filter asks for them.
NTEST(sensor, queries_skip_sensors_unless_asked)
{
    Scene s;
    scene_without_gravity(&s);
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(),
              sensor_desc(nph_physics_geometry_box(nph_vec3_one()), 0U), NULL);
    NphPhysicsQueryHit hit;
    NTEST_EXPECT_FALSE(nph_physics_world_raycast_closest(s.w, nph_dvec3(-3.0, 0.0, 0.0),
                                                         nph_vec3(6.0F, 0.0F, 0.0F), NULL, &hit));
    NphPhysicsQueryFilter f = nph_physics_query_filter_default();
    NTEST_EXPECT_EQ_INT(f.sensors, 0);
    f.sensors = 1;
    NTEST_EXPECT_TRUE(nph_physics_world_raycast_closest(s.w, nph_dvec3(-3.0, 0.0, 0.0),
                                                        nph_vec3(6.0F, 0.0F, 0.0F), &f, &hit));
    const NphPhysicsGeometry probe = nph_physics_geometry_sphere(0.1F);
    u32 n = 0U;
    nph_physics_world_overlap_shape(s.w, &probe, nph_dvec3_zero(), nph_quat_identity(), NULL,
                                    count_overlap, &n);
    NTEST_EXPECT_EQ_UINT(n, 0U);
    nph_physics_world_overlap_shape(s.w, &probe, nph_dvec3_zero(), nph_quat_identity(), &f,
                                    count_overlap, &n);
    NTEST_EXPECT_EQ_UINT(n, 1U);
    scene_end(&s);
}

// A visitor destroyed inside a sensor, by its shape or by its body, ends its overlap at the next
// step with the user data it had, though a new shape took its slot in between; a destroyed sensor
// ends its visitors' overlaps.
NTEST(sensor, destroyed_shapes_end_their_overlaps_with_their_user_data)
{
    Scene s;
    scene_without_gravity(&s);
    NphPhysicsBody sensor_body;
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(),
              sensor_desc(nph_physics_geometry_box(nph_vec3_one()), 5U), &sensor_body);
    const NphPhysicsGeometry small = nph_physics_geometry_sphere(0.2F);
    const NphPhysicsShape a = add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.3, 0.0, 0.0),
                                        solid_desc(small, 11U), NULL);
    NphPhysicsBody b_body;
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-0.3, 0.0, 0.0), solid_desc(small, 22U),
              &b_body);
    step(s.w, NULL, 1U);
    NTEST_ASSERT_EQ_UINT(nph_physics_world_events(s.w).sensor_begin_count, 2U);
    nph_physics_shape_destroy(s.w, a);
    nph_physics_body_destroy(s.w, b_body);
    // A new shape inside, in a freed slot with a new handle.
    const NphPhysicsShape c = add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.0, 0.3, 0.0),
                                        solid_desc(small, 44U), NULL);
    NTEST_EXPECT_NE_UINT(c.value, a.value);
    step(s.w, NULL, 1U);
    NphPhysicsEvents e = nph_physics_world_events(s.w);
    NTEST_ASSERT_EQ_UINT(e.sensor_begin_count, 1U);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin[0].visitor.value, c.value);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin[0].visitor_user_data, 44U);
    NTEST_ASSERT_EQ_UINT(e.sensor_end_count, 2U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end[0].visitor_user_data, 11U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end[1].visitor_user_data, 22U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end[0].sensor_user_data, 5U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end[1].sensor_user_data, 5U);
    // Visitors still inside when the sensor goes: their ends come with the sensor's user data.
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3_zero(), solid_desc(small, 33U), NULL);
    step(s.w, NULL, 1U);
    NTEST_ASSERT_EQ_UINT(nph_physics_world_events(s.w).sensor_begin_count, 1U);
    nph_physics_body_destroy(s.w, sensor_body);
    step(s.w, NULL, 1U);
    e = nph_physics_world_events(s.w);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin_count, 0U);
    NTEST_ASSERT_EQ_UINT(e.sensor_end_count, 2U);
    u64 seen = 0U;
    for (u32 k = 0U; k < 2U; ++k) {
        NTEST_EXPECT_EQ_UINT(e.sensor_end[k].sensor_user_data, 5U);
        seen += e.sensor_end[k].visitor_user_data;
    }
    NTEST_EXPECT_EQ_UINT(seen, 33U + 44U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).sensors, 0U);
    step(s.w, NULL, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_events(s.w).sensor_end_count, 0U);
    scene_end(&s);
}

// A shape without `sensor_events` goes unseen; the rest of its kind are seen.
NTEST(sensor, shapes_without_sensor_events_go_unseen)
{
    Scene s;
    scene_without_gravity(&s);
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(),
              sensor_desc(nph_physics_geometry_box(nph_vec3_one()), 1U), NULL);
    const NphPhysicsGeometry small = nph_physics_geometry_sphere(0.2F);
    NphPhysicsShapeDesc quiet = solid_desc(small, 2U);
    NTEST_EXPECT_EQ_INT(quiet.sensor_events, 1);
    quiet.sensor_events = 0;
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-0.3, 0.0, 0.0), quiet, NULL);
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(0.3, 0.0, 0.0), solid_desc(small, 3U), NULL);
    step(s.w, NULL, 1U);
    const NphPhysicsEvents e = nph_physics_world_events(s.w);
    NTEST_ASSERT_EQ_UINT(e.sensor_begin_count, 1U);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin[0].visitor_user_data, 3U);
    scene_end(&s);
}

// A sensor holding many shapes, past its first room (so its pass runs again) and past the sort's
// insertion threshold, reports each once, in handle order, as it does those that leave.
NTEST(sensor, a_full_sensor_reports_its_shapes_in_handle_order)
{
    Scene s;
    scene_without_gravity(&s);
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(),
              sensor_desc(nph_physics_geometry_box(nph_vec3(3.0F, 3.0F, 3.0F)), 1U), NULL);
    NphPhysicsShape shapes[100];
    for (u32 i = 0U; i < 100U; ++i) {
        // Placed out of creation order, so the tree gives them out of handle order.
        const u32 at = (i * 37U) % 100U;
        const u32 row = (at / 5U) % 5U;
        const u32 layer = at / 25U;
        const NphDVec3 p = nph_dvec3((f64)(at % 5U) - 2.0, (f64)row - 2.0, (f64)layer - 1.5);
        shapes[i] = add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, p,
                              solid_desc(nph_physics_geometry_sphere(0.2F), 100U + i), NULL);
    }
    step(s.w, NULL, 1U);
    NphPhysicsEvents e = nph_physics_world_events(s.w);
    NTEST_ASSERT_EQ_UINT(e.sensor_begin_count, 100U);
    for (u32 k = 1U; k < 100U; ++k) {
        NTEST_EXPECT_LT_UINT(e.sensor_begin[k - 1U].visitor.value, e.sensor_begin[k].visitor.value);
    }
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).sensor_visitors, 100U);
    step(s.w, NULL, 1U);
    e = nph_physics_world_events(s.w);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin_count, 0U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end_count, 0U);
    for (u32 i = 0U; i < 100U; i += 2U) {
        nph_physics_shape_destroy(s.w, shapes[i]);
    }
    step(s.w, NULL, 1U);
    e = nph_physics_world_events(s.w);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin_count, 0U);
    NTEST_ASSERT_EQ_UINT(e.sensor_end_count, 50U);
    for (u32 k = 0U; k < 50U; ++k) {
        const u32 j = 2U * k;
        NTEST_EXPECT_EQ_UINT(e.sensor_end[k].visitor.value, shapes[j].value);
        NTEST_EXPECT_EQ_UINT(e.sensor_end[k].visitor_user_data, 100U + j);
    }
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(s.w).sensor_visitors, 50U);
    scene_end(&s);
}

// An allocator that serves `budget` allocations from `inner`, then fails.
typedef struct FailingAllocator {
    NphAllocator allocator;
    NphAllocator *inner;
    u32 budget;
} FailingAllocator;

static void *failing_alloc(NphAllocator *self, u64 size, u32 align, NphCallsite site)
{
    FailingAllocator *f = (FailingAllocator *)self;
    if (f->budget == 0U) {
        return NULL;
    }
    f->budget -= 1U;
    return nph_alloc_at(f->inner, size, align, site);
}

static void *failing_realloc(NphAllocator *self, void *ptr, u64 old_size, u64 new_size, u32 align,
                             NphCallsite site)
{
    FailingAllocator *f = (FailingAllocator *)self;
    if (f->budget == 0U) {
        return NULL;
    }
    f->budget -= 1U;
    return nph_realloc_at(f->inner, ptr, old_size, new_size, align, site);
}

static void failing_free(NphAllocator *self, void *ptr, u64 size, u32 align, NphCallsite site)
{
    nph_free_at(((FailingAllocator *)self)->inner, ptr, size, align, site);
}

static const NphAllocatorVtable g_failing_vtable = {failing_alloc, failing_realloc, failing_free};

// The overlaps that end and begin in the failing step of the test below: one visitor leaves the
// first sensor and forty come in, and the second sensor, holding one, is destroyed before it.
static void expect_the_changes(const NphPhysicsWorld *w)
{
    const NphPhysicsEvents e = nph_physics_world_events(w);
    NTEST_ASSERT_EQ_UINT(e.sensor_end_count, 2U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end[0].sensor_user_data, 2U); // the destroyed sensor's first
    NTEST_EXPECT_EQ_UINT(e.sensor_end[0].visitor_user_data, 20U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end[1].sensor_user_data, 1U);
    NTEST_EXPECT_EQ_UINT(e.sensor_end[1].visitor_user_data, 10U);
    NTEST_EXPECT_EQ_UINT(e.sensor_begin_count, 40U);
}

// A step that fails, at any allocation it makes, reports no sensor events and keeps the sets: the
// next step that succeeds reports every change since the last that did.
NTEST(sensor, a_failed_step_keeps_the_sets_for_the_next)
{
    bool succeeded = false;
    u32 failures = 0U;
    for (u32 budget = 0U; budget < 64U && !succeeded; ++budget) {
        TestAlloc ta;
        FailingAllocator fa;
        fa.allocator.vtable = &g_failing_vtable;
        fa.allocator.name = "failing";
        fa.inner = test_alloc_begin(&ta);
        fa.budget = NPH_U32_MAX;
        NphPhysicsWorldDesc d = nph_physics_world_desc_default();
        d.gravity = nph_vec3_zero();
        d.alloc = &fa.allocator;
        NphPhysicsWorld *w = NULL;
        NTEST_ASSERT_EQ_INT(nph_physics_world_create(&d, &w), NPH_RESULT_OK);
        add_shape(w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(),
                  sensor_desc(nph_physics_geometry_box(nph_vec3(2.0F, 2.0F, 2.0F)), 1U), NULL);
        NphPhysicsBody second;
        add_shape(w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(10.0, 0.0, 0.0),
                  sensor_desc(nph_physics_geometry_box(nph_vec3_one()), 2U), &second);
        const NphPhysicsGeometry small = nph_physics_geometry_sphere(0.1F);
        NphPhysicsBody leaving;
        add_shape(w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3_zero(), solid_desc(small, 10U), &leaving);
        add_shape(w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(10.0, 0.0, 0.0), solid_desc(small, 20U),
                  NULL);
        NphPhysicsBody coming[40];
        for (u32 i = 0U; i < 40U; ++i) {
            add_shape(w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-20.0 - (f64)i, 0.0, 0.0),
                      solid_desc(small, 100U + i), &coming[i]);
        }
        NTEST_ASSERT_EQ_INT(nph_physics_world_step(w, DT, NULL), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(nph_physics_world_events(w).sensor_begin_count, 2U);
        // The changes: one leaves, forty come in (apart, touching nothing), a sensor goes.
        nph_physics_body_set_transform(w, leaving, nph_dvec3(-5.0, 0.0, 0.0), nph_quat_identity());
        for (u32 i = 0U; i < 40U; ++i) {
            const u32 row = (i / 4U) % 5U;
            const u32 layer = i / 20U;
            const NphDVec3 p =
                nph_dvec3((f64)(i % 4U) * 0.5 - 0.75, (f64)row * 0.5 - 1.0, (f64)layer - 0.5);
            nph_physics_body_set_transform(w, coming[i], p, nph_quat_identity());
        }
        nph_physics_body_destroy(w, second);
        fa.budget = budget;
        const NphResult r = nph_physics_world_step(w, DT, NULL);
        fa.budget = NPH_U32_MAX;
        if (r == NPH_RESULT_OK) {
            succeeded = true;
            expect_the_changes(w);
        } else {
            failures += 1U;
            NTEST_EXPECT_EQ_INT(r, NPH_RESULT_OUT_OF_MEMORY);
            const NphPhysicsEvents e = nph_physics_world_events(w);
            NTEST_EXPECT_EQ_UINT(e.sensor_begin_count, 0U);
            NTEST_EXPECT_EQ_UINT(e.sensor_end_count, 0U);
            NTEST_EXPECT_TRUE(nph_physics_world_validate(w));
            NTEST_ASSERT_EQ_INT(nph_physics_world_step(w, DT, NULL), NPH_RESULT_OK);
            expect_the_changes(w);
        }
        NTEST_ASSERT_EQ_INT(nph_physics_world_step(w, DT, NULL), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(nph_physics_world_events(w).sensor_begin_count, 0U);
        NTEST_EXPECT_EQ_UINT(nph_physics_world_events(w).sensor_end_count, 0U);
        NTEST_EXPECT_TRUE(nph_physics_world_validate(w));
        nph_physics_world_destroy(w);
        test_alloc_end(&ta);
    }
    NTEST_EXPECT_TRUE(succeeded);
    // The pending end's room, the sets' growth, and the begins' room at least.
    NTEST_EXPECT_GT_UINT(failures, 2U);
    NTEST_LOG("the step failed at %u allocations before one sufficed", failures);
}

typedef struct LineColors {
    u32 sensor;
    u32 ghost;
    u32 other;
} LineColors;

static void count_line(void *user, NphVec3 a, NphVec3 b, u32 rgba)
{
    NPH_UNUSED(a);
    NPH_UNUSED(b);
    LineColors *c = (LineColors *)user;
    if (rgba == NPH_PHYSICS_DRAW_SENSOR_RGBA) {
        c->sensor += 1U;
    } else if (rgba == NPH_PHYSICS_DRAW_GHOST_RGBA) {
        c->ghost += 1U;
    } else {
        c->other += 1U;
    }
}

// Debug drawing shows sensors and ghosts in their own colours, whatever their bodies.
NTEST(sensor, debug_drawing_colours_sensors_and_ghosts)
{
    Scene s;
    scene_without_gravity(&s);
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3_zero(),
              sensor_desc(nph_physics_geometry_box(nph_vec3_one()), 0U), NULL);
    NphPhysicsShapeDesc gd = solid_desc(nph_physics_geometry_box(nph_vec3_one()), 0U);
    gd.ghost = 1;
    add_shape(s.w, NPH_PHYSICS_BODY_KINEMATIC, nph_dvec3(5.0, 0.0, 0.0), gd, NULL);
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(-5.0, 0.0, 0.0),
              solid_desc(nph_physics_geometry_box(nph_vec3_one()), 0U), NULL);
    LineColors c = {0U, 0U, 0U};
    NphPhysicsDebugDraw draw;
    draw.line = count_line;
    draw.user = &c;
    draw.origin = nph_dvec3_zero();
    draw.flags = NPH_PHYSICS_DRAW_SHAPES;
    nph_physics_world_draw(s.w, &draw);
    NTEST_EXPECT_EQ_UINT(c.sensor, 12U); // a box's edges
    NTEST_EXPECT_EQ_UINT(c.ghost, 12U);
    NTEST_EXPECT_EQ_UINT(c.other, 12U);
    scene_end(&s);
}

// A sensor must be convex, and cannot be a ghost too; a sensor may ride a dynamic body.
NTEST(sensor, sensors_are_convex_and_not_ghosts)
{
    Scene s;
    scene_without_gravity(&s);
    static const NphVec3 v[4] = {
        {-1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 1.0F}};
    static const u32 idx[6] = {0U, 2U, 1U, 0U, 3U, 2U};
    NphPhysicsMesh *mesh = test_mesh(s.alloc, v, 4U, idx, 2U, false);
    NphPhysicsBody b = add_anchor(s.w, nph_dvec3_zero());
    NphPhysicsShapeDesc sd = sensor_desc(nph_physics_geometry_mesh(mesh), 0U);
    NphPhysicsShape out;
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(s.w, b, &sd, &out), NPH_RESULT_INVALID_ARGUMENT);
    sd = sensor_desc(nph_physics_geometry_sphere(1.0F), 0U);
    sd.ghost = 1;
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(s.w, b, &sd, &out), NPH_RESULT_INVALID_ARGUMENT);
    NphPhysicsBody rider;
    add_shape(s.w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3_zero(),
              solid_desc(nph_physics_geometry_sphere(0.5F), 0U), &rider);
    sd.ghost = 0;
    NTEST_EXPECT_EQ_INT(nph_physics_shape_create(s.w, rider, &sd, &out), NPH_RESULT_OK);
    nph_physics_mesh_free(mesh, s.alloc); // no shape holds it: its sensor was refused
    scene_end(&s);
}

// A character controller walks through a sensor: its moves do not meet sensors.
NTEST(sensor, characters_walk_through_sensors)
{
    Scene s;
    scene_begin(&s);
    add_ground(s.w, 0.0);
    add_shape(s.w, NPH_PHYSICS_BODY_STATIC, nph_dvec3(2.0, 1.0, 0.0),
              sensor_desc(nph_physics_geometry_box(nph_vec3(0.5F, 1.0F, 2.0F)), 0U), NULL);
    const NphPhysicsCharacterDesc desc =
        nph_physics_character_desc_default(nph_physics_geometry_capsule(0.6F, 0.3F));
    NphPhysicsCharacter c;
    NTEST_ASSERT_EQ_INT(
        nph_physics_character_init(s.w, &desc, nph_dvec3(0.0, 0.92, 0.0), nph_quat_identity(), &c),
        NPH_RESULT_OK);
    for (u32 i = 0U; i < 240U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_physics_character_move(
                                s.w, &desc, &c, nph_vec3(4.0F * DT, 0.0F, 0.0F), NULL, 0U, NULL),
                            NPH_RESULT_OK);
    }
    NTEST_EXPECT_NEAR_F64(c.position.x, 4.0, 0.05);
    scene_end(&s);
}

typedef struct SensorLog {
    u64 words[4096];
    u32 count;
} SensorLog;

static void log_events(SensorLog *record, const NphPhysicsWorld *w)
{
    const NphPhysicsEvents e = nph_physics_world_events(w);
    for (u32 k = 0U; k < e.sensor_begin_count && record->count + 2U <= 4096U; ++k) {
        record->words[record->count++] =
            ((u64)e.sensor_begin[k].sensor.value << 32U) | e.sensor_begin[k].visitor.value;
        record->words[record->count++] = e.sensor_begin[k].visitor_user_data;
    }
    for (u32 k = 0U; k < e.sensor_end_count && record->count + 2U <= 4096U; ++k) {
        record->words[record->count++] = ((u64)e.sensor_end[k].sensor.value << 32U) |
                                         e.sensor_end[k].visitor.value | (1ULL << 63U);
        record->words[record->count++] = e.sensor_end[k].visitor_user_data;
    }
}

static void rain_through_sensors(Scene *s, NphJobSystem *js, SensorLog *record)
{
    // Sixteen sensors, so the job system's pass splits them (sensor.c: from eight).
    for (u32 i = 0U; i < 16U; ++i) {
        const u32 row = i / 4U;
        add_shape(s->w, NPH_PHYSICS_BODY_STATIC,
                  nph_dvec3((f64)(i % 4U) * 2.0 - 3.0, 0.0, (f64)row * 0.5 - 0.75),
                  sensor_desc(nph_physics_geometry_box(nph_vec3(0.9F, 0.3F, 0.2F)), 100U + i),
                  NULL);
    }
    for (u32 i = 0U; i < 40U; ++i) {
        const u32 row = i / 8U;
        const f64 x = (f64)(i % 8U) * 0.9 - 3.4;
        const f64 y = 2.0 + (f64)row * 0.7;
        add_shape(s->w, NPH_PHYSICS_BODY_DYNAMIC, nph_dvec3(x, y, (f64)(i % 3U) * 0.3 - 0.3),
                  solid_desc(nph_physics_geometry_sphere(0.2F), i), NULL);
    }
    record->count = 0U;
    for (u32 i = 0U; i < 480U; ++i) {
        step(s->w, js, 1U);
        log_events(record, s->w);
    }
}

// Worlds made and stepped the same way report the same events in the same order, on one thread
// and on a job system that splits the sensors between its workers.
NTEST(sensor, events_are_deterministic)
{
    static SensorLog logs[3];
    static const u32 workers[3] = {0U, 0U, 4U};
    for (u32 run = 0U; run < 3U; ++run) {
        NphJobSystem *js = workers[run] == 0U ? NULL : make_jobs(workers[run]);
        Scene s;
        scene_begin(&s);
        rain_through_sensors(&s, js, &logs[run]);
        scene_end(&s);
        if (js != NULL) {
            nph_job_system_destroy(js);
        }
    }
    NTEST_ASSERT_TRUE(logs[0].count > 40U);
    for (u32 run = 1U; run < 3U; ++run) {
        NTEST_ASSERT_EQ_UINT(logs[run].count, logs[0].count);
        NTEST_EXPECT_EQ_MEM(logs[run].words, logs[0].words, (u64)logs[0].count * sizeof(u64));
    }
}
