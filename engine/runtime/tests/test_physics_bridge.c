// SPDX-License-Identifier: Apache-2.0
// Physics in the engine (components.h "Physics"; docs/runtime.md
// "Physics"): RigidBody and Collider components become bodies when play
// begins, bodies move their entities, the game moves bodies through the
// components, components coming and going make and unmake bodies, bodies
// name their entities, children follow or not, the world's events
// overflowing is recovered, and runs repeat. Headless engines in lockstep: one step a frame.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/query.h>
#include <nepheline/physics/shape.h>
#include <nepheline/runtime/engine.h>

#include <ntest/ntest.h>

#include <string.h>

#define NO_RIGID_BODY NPH_U32_MAX

typedef struct PhysFixture {
    NphDebugAllocator da;
    NphEngineConfig config;
    NphEngine *engine;
    NphWorld *world;
    NphRuntimeComponents ids;
} PhysFixture;

static void start_with(PhysFixture *f, const char *const *assignments, u32 count)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->config.headless = 1;
    NTEST_ASSERT_LE_UINT(count, NPH_ENGINE_CONFIG_MAX_ASSIGNMENTS);
    for (u32 i = 0; i < count; ++i) {
        f->config.assignments[i] = assignments[i];
    }
    f->config.assignment_count = count;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.config = &f->config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "physics bridge tests";
    NTEST_ASSERT_EQ_INT(nph_engine_create(&d, &f->engine), NPH_RESULT_OK);
    f->world = nph_engine_edit_world(f->engine);
    f->ids = *nph_engine_components(f->engine);
}

static void start(PhysFixture *f)
{
    static const char *const lockstep[1] = {"rt.lockstep=true"};
    start_with(f, lockstep, 1U);
}

static void finish(PhysFixture *f)
{
    nph_engine_destroy(f->engine);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// Runs `n` frames (steps, in lockstep) and waits for the last one's.
static void frames(PhysFixture *f, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(f->engine), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_wait(f->engine), NPH_RESULT_OK);
}

static NphCollider box_collider(NphVec3 half)
{
    NphCollider c = *(const NphCollider *)NphCollider_component()->defaults;
    c.shape = NPH_COLLIDER_BOX;
    c.half_extents = half;
    return c;
}

static NphCollider sphere_collider(f32 radius)
{
    NphCollider c = *(const NphCollider *)NphCollider_component()->defaults;
    c.shape = NPH_COLLIDER_SPHERE;
    c.radius = radius;
    return c;
}

// An entity at `at` with a RigidBody of `type` (NO_RIGID_BODY: none) and
// `collider` (NULL: none), in the active world.
static NphEntity spawn(PhysFixture *f, NphDVec3 at, u32 type, const NphCollider *collider)
{
    NphWorld *w = nph_engine_world(f->engine);
    NphComponentId comps[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM, NPH_COMPONENT_ID_WORLD_TRANSFORM};
    u32 n = 2U;
    if (type != NO_RIGID_BODY) {
        comps[n++] = f->ids.rigid_body;
    }
    if (collider != NULL) {
        comps[n++] = f->ids.collider;
    }
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, n, &e), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation = at;
    if (type != NO_RIGID_BODY) {
        NPH_WORLD_GET_MUT(w, e, f->ids.rigid_body, NphRigidBody)->type = type;
    }
    if (collider != NULL) {
        *NPH_WORLD_GET_MUT(w, e, f->ids.collider, NphCollider) = *collider;
    }
    return e;
}

static NphEntity spawn_floor(PhysFixture *f)
{
    const NphCollider c = box_collider(nph_vec3(5.0F, 0.5F, 5.0F));
    return spawn(f, nph_dvec3(0.0, -0.5, 0.0), NO_RIGID_BODY, &c); // its top at y = 0
}

static NphLocalTransform *local(PhysFixture *f, NphEntity e)
{
    return NPH_WORLD_GET_MUT(nph_engine_world(f->engine), e, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                             NphLocalTransform);
}

static const NphLocalTransform *local_of(PhysFixture *f, NphEntity e)
{
    return NPH_WORLD_GET(nph_engine_world(f->engine), e, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                         NphLocalTransform);
}

static NphRigidBody *rigid(PhysFixture *f, NphEntity e)
{
    return NPH_WORLD_GET_MUT(nph_engine_world(f->engine), e, f->ids.rigid_body, NphRigidBody);
}

static NphPhysicsBody body_of(PhysFixture *f, NphEntity e)
{
    return nph_engine_physics_body(f->engine, e);
}

static u32 body_count(PhysFixture *f)
{
    return nph_physics_world_stats(nph_engine_physics(f->engine)).bodies;
}

NTEST(Physics, a_box_falls_onto_the_floor_and_rests)
{
    static PhysFixture fx;
    start(&fx);
    NphEngine *e = fx.engine;
    const NphEntity floor = spawn_floor(&fx);
    const NphCollider cube = box_collider(nph_vec3(0.5F, 0.5F, 0.5F));
    const NphEntity box = spawn(&fx, nph_dvec3(0.0, 2.0, 0.0), NPH_RIGID_BODY_DYNAMIC, &cube);
    // Not playing: no physics world, no bodies.
    frames(&fx, 2U);
    NTEST_EXPECT_NULL(nph_engine_physics(e));
    NTEST_EXPECT_TRUE(nph_physics_body_is_null(body_of(&fx, box)));

    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, false), NPH_RESULT_OK);
    // The bodies are there at once, for the game's play_begin.
    NTEST_ASSERT_NOT_NULL(nph_engine_physics(e));
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 2U);
    NphPhysicsWorld *pw = nph_engine_physics(e);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, body_of(&fx, floor)),
                        (int)NPH_PHYSICS_BODY_STATIC);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, body_of(&fx, box)),
                        (int)NPH_PHYSICS_BODY_DYNAMIC);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body_of(&fx, box)).y, 2.0, 0.0);
    // A cube of water.
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(pw, body_of(&fx, box)).mass, 1000.0F, 0.01F);

    // Falling: the entity follows its body, and its RigidBody its speed.
    frames(&fx, 60U);
    const f64 y_quarter = local_of(&fx, box)->translation.y;
    NTEST_EXPECT_TRUE(y_quarter < 1.8 && y_quarter > 1.6); // 1/2 g t^2 = 0.31 m at 0.25 s
    NTEST_EXPECT_NEAR_F32(rigid(&fx, box)->linear_velocity.y, -9.81F * 0.25F, 0.02F);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, box)->translation.y,
                          nph_physics_body_position(pw, body_of(&fx, box)).y, 0.0);
    // At rest on the floor within two seconds, and asleep.
    frames(&fx, 420U);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, box)->translation.y, 0.5, 0.005);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, box)->translation.x, 0.0, 1e-4);
    NTEST_EXPECT_TRUE(nph_vec3_length(rigid(&fx, box)->linear_velocity) < 0.01F);
    NTEST_EXPECT_FALSE(nph_physics_body_is_awake(pw, body_of(&fx, box)));
    // The world transform composed from where the body went.
    const NphWorldTransform *wt = NPH_WORLD_GET(
        nph_engine_world(e), box, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    NTEST_EXPECT_NEAR_F64(wt->translation.y, 0.5, 0.005);
    // The floor never moved.
    NTEST_EXPECT_NEAR_F64(local_of(&fx, floor)->translation.y, -0.5, 0.0);

    nph_engine_play_end(e);
    NTEST_EXPECT_NULL(nph_engine_physics(e));
    NTEST_EXPECT_TRUE(nph_physics_body_is_null(body_of(&fx, box)));
    finish(&fx);
}

NTEST(Physics, a_body_names_its_entity)
{
    // While playing, each body names the entity whose components made it,
    // so what a query hits or an event reports says what it was; a body of
    // the game's own names none, whatever its user data says, and neither
    // does one that went.
    static PhysFixture fx;
    start(&fx);
    NphEngine *e = fx.engine;
    const NphEntity floor = spawn_floor(&fx);
    const NphCollider sphere = sphere_collider(0.5F);
    const NphEntity ball = spawn(&fx, nph_dvec3(0.0, 3.0, 0.0), NPH_RIGID_BODY_DYNAMIC, &sphere);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_engine_physics_entity(e, NPH_PHYSICS_BODY_NULL)));
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, false), NPH_RESULT_OK);
    NphPhysicsWorld *pw = nph_engine_physics(e);
    const NphPhysicsBody floor_body = body_of(&fx, floor);
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(e, floor_body).value, floor.value);
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(e, body_of(&fx, ball)).value, ball.value);
    // A ray down meets the ball, and past it the floor, through its shape.
    const NphDVec3 above = nph_dvec3(0.0, 10.0, 0.0);
    const NphVec3 down = nph_vec3(0.0F, -20.0F, 0.0F);
    NphPhysicsQueryHit hit;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(pw, above, down, NULL, &hit));
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(e, hit.body).value, ball.value);
    NphPhysicsQueryFilter past = nph_physics_query_filter_default();
    past.ignore = hit.body;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(pw, above, down, &past, &hit));
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(e, nph_physics_shape_body(pw, hit.shape)).value,
                         floor.value);
    // A body of the game's own that claims the ball's entity: none.
    NphPhysicsBodyDesc d = nph_physics_body_desc_default();
    d.user_data = ball.value;
    NphPhysicsBody own;
    NTEST_ASSERT_EQ_INT(nph_physics_body_create(pw, &d, &own), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_engine_physics_entity(e, own)));
    nph_physics_body_destroy(pw, own);
    // The ball gone, its body names nothing; and nothing does after play.
    const NphPhysicsBody ball_body = body_of(&fx, ball);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(nph_engine_world(e), ball), NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_engine_physics_entity(e, ball_body)));
    nph_engine_play_end(e);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_engine_physics_entity(e, floor_body)));
    finish(&fx);
}

NTEST(Physics, play_in_the_editor_leaves_the_edited_world_alone)
{
    static PhysFixture fx;
    start(&fx);
    NphEngine *e = fx.engine;
    (void)spawn_floor(&fx);
    const NphCollider ball = sphere_collider(0.25F);
    const NphEntity b = spawn(&fx, nph_dvec3(0.0, 3.0, 0.0), NPH_RIGID_BODY_DYNAMIC, &ball);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, true), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_engine_world(e) != nph_engine_edit_world(e));
    frames(&fx, 360U);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, b)->translation.y, 0.25, 0.005); // the copy's
    nph_engine_play_end(e);
    // The edited world, untouched, plays again from the start.
    NTEST_EXPECT_NEAR_F64(local_of(&fx, b)->translation.y, 3.0, 0.0);
    NTEST_EXPECT_NEAR_F32(rigid(&fx, b)->linear_velocity.y, 0.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, true), NPH_RESULT_OK);
    frames(&fx, 30U);
    NTEST_EXPECT_TRUE(local_of(&fx, b)->translation.y < 3.0);
    nph_engine_play_end(e);
    finish(&fx);
}

NTEST(Physics, the_game_moves_bodies_through_their_components)
{
    static PhysFixture fx;
    start(&fx);
    NphEngine *e = fx.engine;
    (void)spawn_floor(&fx);
    const NphCollider cube = box_collider(nph_vec3(0.5F, 0.5F, 0.5F));
    const NphEntity box = spawn(&fx, nph_dvec3(0.0, 0.5, 0.0), NPH_RIGID_BODY_DYNAMIC, &cube);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, false), NPH_RESULT_OK);
    frames(&fx, 240U);
    NphPhysicsWorld *pw = nph_engine_physics(e);

    // A velocity: it slides along X, friction stopping it (mu g = 5.9 m/s^2:
    // 0.34 m from 2 m/s).
    rigid(&fx, box)->linear_velocity = nph_vec3(2.0F, 0.0F, 0.0F);
    frames(&fx, 1U);
    NTEST_EXPECT_TRUE(nph_physics_body_linear_velocity(pw, body_of(&fx, box)).x > 1.9F);
    frames(&fx, 239U);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, box)->translation.x, 0.34, 0.03);
    NTEST_EXPECT_TRUE(nph_vec3_length(rigid(&fx, box)->linear_velocity) < 0.01F);

    // A new LocalTransform teleports it, its speed kept, and it falls from
    // there (no floor under it).
    local(&fx, box)->translation = nph_dvec3(10.0, 3.0, 0.0);
    rigid(&fx, box)->linear_velocity = nph_vec3(0.0F, 1.0F, 0.0F);
    frames(&fx, 1U);
    const NphDVec3 p = nph_physics_body_position(pw, body_of(&fx, box));
    NTEST_EXPECT_NEAR_F64(p.x, 10.0, 0.0);
    NTEST_EXPECT_NEAR_F64(p.y, 3.0 + (1.0 - 9.81 / 480.0) / 240.0, 1e-4); // risen a little
    frames(&fx, 60U);
    NTEST_EXPECT_TRUE(local_of(&fx, box)->translation.y < 3.0);

    // A kinematic platform the game moves at 1 m/s carries a box riding on
    // it, then stops when the game stops moving it.
    const NphCollider slab = box_collider(nph_vec3(1.0F, 0.1F, 1.0F));
    const NphEntity platform =
        spawn(&fx, nph_dvec3(-3.0, 1.0, 0.0), NPH_RIGID_BODY_KINEMATIC, &slab);
    const NphCollider small = box_collider(nph_vec3(0.2F, 0.2F, 0.2F));
    const NphEntity rider = spawn(&fx, nph_dvec3(-3.0, 1.3, 0.0), NPH_RIGID_BODY_DYNAMIC, &small);
    frames(&fx, 120U); // the rider settles
    const f64 rider_x = local_of(&fx, rider)->translation.x;
    f64 rider_half_way = 0.0;
    for (u32 i = 1; i <= 240U; ++i) {
        local(&fx, platform)->translation.x = -3.0 + (f64)i / 240.0;
        frames(&fx, 1U);
        rider_half_way = i == 120U ? local_of(&fx, rider)->translation.x : rider_half_way;
    }
    NTEST_EXPECT_NEAR_F64(local_of(&fx, platform)->translation.x, -2.0, 1e-6);
    NTEST_EXPECT_NEAR_F32(rigid(&fx, platform)->linear_velocity.x, 1.0F, 1e-3F);
    // Friction (mu g = 5.9 m/s^2) brings the rider to the platform's speed
    // in 0.17 s, 8.5 cm behind; from there on it rides without slipping.
    NTEST_EXPECT_NEAR_F64(local_of(&fx, rider)->translation.x - rider_x, 1.0 - 0.085, 0.02);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, rider)->translation.x - rider_half_way, 0.5, 0.002);
    frames(&fx, 10U);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, platform)->translation.x, -2.0, 1e-5);
    NTEST_EXPECT_NEAR_F32(rigid(&fx, platform)->linear_velocity.x, 0.0F, 0.0F);
    // Or by its velocity: it keeps moving until told otherwise.
    rigid(&fx, platform)->linear_velocity = nph_vec3(0.0F, 0.5F, 0.0F);
    frames(&fx, 240U);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, platform)->translation.y, 1.5, 1e-3);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, rider)->translation.y, 1.8, 0.01);

    // Other fields: no gravity, and it floats where it is.
    rigid(&fx, rider)->gravity_scale = 0.0F;
    rigid(&fx, platform)->linear_velocity = nph_vec3_zero();
    frames(&fx, 2U);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(pw, body_of(&fx, rider)).mass, 64.0F, 0.01F);
    rigid(&fx, rider)->mass = 5.0F;
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(pw, body_of(&fx, rider)).mass, 5.0F, 0.0F);
    nph_engine_play_end(e);
    finish(&fx);
}

NTEST(Physics, components_coming_and_going_make_and_unmake_bodies)
{
    static PhysFixture fx;
    start(&fx);
    NphEngine *e = fx.engine;
    (void)spawn_floor(&fx);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, false), NPH_RESULT_OK);
    NphPhysicsWorld *pw = nph_engine_physics(e);
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 1U);

    // A Collider alone: a static body.
    const NphCollider ball = sphere_collider(0.5F);
    const NphEntity a = spawn(&fx, nph_dvec3(2.0, 3.0, 0.0), NO_RIGID_BODY, &ball);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 2U);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, body_of(&fx, a)),
                        (int)NPH_PHYSICS_BODY_STATIC);
    frames(&fx, 30U);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, a)->translation.y, 3.0, 0.0);

    // With a RigidBody it falls.
    NTEST_ASSERT_EQ_INT(nph_world_add(nph_engine_world(e), a, fx.ids.rigid_body, NULL),
                        NPH_RESULT_OK);
    frames(&fx, 30U);
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 2U);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, body_of(&fx, a)),
                        (int)NPH_PHYSICS_BODY_DYNAMIC);
    NTEST_EXPECT_TRUE(local_of(&fx, a)->translation.y < 3.0);
    const f32 ball_mass = 1000.0F * 4.0F / 3.0F * 3.14159265F * 0.125F;
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(pw, body_of(&fx, a)).mass, ball_mass, 0.1F);

    // A new Collider: its shape and mass made again.
    NphCollider *c = NPH_WORLD_GET_MUT(nph_engine_world(e), a, fx.ids.collider, NphCollider);
    c->radius = 0.25F;
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(pw, body_of(&fx, a)).mass, ball_mass / 8.0F,
                          0.02F);
    // The entity's scale scales its Collider.
    local(&fx, a)->scale = nph_vec3(2.0F, 2.0F, 2.0F);
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(pw, body_of(&fx, a)).mass, ball_mass, 0.1F);
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_UINT(nph_physics_body_shapes(pw, body_of(&fx, a), &shape, 1U), 1U);
    NTEST_EXPECT_NEAR_F32(nph_physics_shape_geometry(pw, shape).sphere.radius, 0.5F, 0.0F);

    // A type of its own: made again as a static body, where it is.
    const f64 y = local_of(&fx, a)->translation.y;
    rigid(&fx, a)->type = NPH_RIGID_BODY_STATIC;
    frames(&fx, 20U);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, body_of(&fx, a)),
                        (int)NPH_PHYSICS_BODY_STATIC);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, a)->translation.y, y, 0.0);

    // Without its Collider: a body without a shape (a dynamic one falls as a
    // point of 1 kg).
    rigid(&fx, a)->type = NPH_RIGID_BODY_DYNAMIC;
    NTEST_ASSERT_EQ_INT(nph_world_remove(nph_engine_world(e), a, fx.ids.collider), NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_body_shapes(pw, body_of(&fx, a), &shape, 1U), 0U);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(pw, body_of(&fx, a)).mass, 1.0F, 0.0F);

    // Without its RigidBody either, and despawned: no body.
    NTEST_ASSERT_EQ_INT(nph_world_remove(nph_engine_world(e), a, fx.ids.rigid_body), NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_TRUE(nph_physics_body_is_null(body_of(&fx, a)));
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 1U);
    const NphEntity b = spawn(&fx, nph_dvec3(0.0, 1.0, 0.0), NPH_RIGID_BODY_DYNAMIC, &ball);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 2U);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(nph_engine_world(e), b), NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 1U);
    NTEST_EXPECT_TRUE(nph_physics_body_is_null(body_of(&fx, b)));
    // A Collider that is no shape (a sphere of radius 0): a body without one.
    NphCollider none = sphere_collider(0.0F);
    const NphEntity d = spawn(&fx, nph_dvec3(0.0, 1.0, 0.0), NPH_RIGID_BODY_DYNAMIC, &none);
    frames(&fx, 1U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_null(body_of(&fx, d)));
    NTEST_EXPECT_EQ_UINT(nph_physics_body_shapes(pw, body_of(&fx, d), &shape, 1U), 0U);
    nph_engine_play_end(e);
    finish(&fx);
}

NTEST(Physics, children_follow_their_parents_unless_dynamic)
{
    static PhysFixture fx;
    start(&fx);
    NphEngine *e = fx.engine;
    NphWorld *w = nph_engine_world(e);
    // A kinematic carrier with a static collider and a dynamic ball under
    // it.
    const NphEntity carrier = spawn(&fx, nph_dvec3(0.0, 5.0, 0.0), NPH_RIGID_BODY_KINEMATIC, NULL);
    const NphCollider cube = box_collider(nph_vec3(0.5F, 0.5F, 0.5F));
    const NphEntity block = spawn(&fx, nph_dvec3(0.0, 1.0, 0.0), NO_RIGID_BODY, &cube);
    const NphCollider ball = sphere_collider(0.2F);
    const NphEntity loose = spawn(&fx, nph_dvec3(3.0, 0.0, 0.0), NPH_RIGID_BODY_DYNAMIC, &ball);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, block, carrier), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, loose, carrier), NPH_RESULT_OK);
    rigid(&fx, loose)->gravity_scale = 0.0F;
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, false), NPH_RESULT_OK);
    NphPhysicsWorld *pw = nph_engine_physics(e);
    // Bodies start where the entities are in the world.
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body_of(&fx, block)).y, 6.0, 1e-9);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body_of(&fx, loose)).x, 3.0, 1e-9);
    // The carrier moves over 10 frames (and its world transform with it).
    for (u32 i = 1; i <= 10U; ++i) {
        local(&fx, carrier)->translation.x = 0.1 * (f64)i;
        frames(&fx, 1U);
    }
    frames(&fx, 2U);
    // The static block followed it; the loose ball stayed where it was in the
    // world, its LocalTransform relative to the carrier's new place.
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body_of(&fx, block)).x, 1.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body_of(&fx, block)).y, 6.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body_of(&fx, loose)).x, 3.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(local_of(&fx, loose)->translation.x, 2.0, 1e-5);
    const NphWorldTransform *lw =
        NPH_WORLD_GET(w, loose, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    NTEST_EXPECT_NEAR_F64(lw->translation.x, 3.0, 1e-5);
    nph_engine_play_end(e);
    finish(&fx);
}

NTEST(Physics, a_flood_of_world_events_is_recovered)
{
    static PhysFixture fx;
    static const char *const settings[2] = {"rt.lockstep=true", "rt.world_events=64"};
    start_with(&fx, settings, 2U);
    NphEngine *e = fx.engine;
    (void)spawn_floor(&fx);
    const NphCollider cube = box_collider(nph_vec3(0.2F, 0.2F, 0.2F));
    NphEntity first[20];
    for (u32 i = 0; i < 20U; ++i) {
        first[i] =
            spawn(&fx, nph_dvec3((f64)i * 0.5 - 5.0, 0.2, 0.0), NPH_RIGID_BODY_DYNAMIC, &cube);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, false), NPH_RESULT_OK);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 21U);
    // More changes than the stream holds: a hundred spawns and ten despawns
    // between two frames.
    for (u32 i = 0; i < 100U; ++i) {
        const u32 column = i % 10U;
        const u32 layer = i / 10U;
        (void)spawn(&fx, nph_dvec3((f64)column - 5.0, 2.0 + (f64)layer, 2.0),
                    NPH_RIGID_BODY_DYNAMIC, &cube);
    }
    for (u32 i = 0; i < 10U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_despawn(nph_engine_world(e), first[i]), NPH_RESULT_OK);
    }
    u32 count = 0U;
    u32 dropped = 0U;
    (void)nph_world_events(nph_engine_world(e), &count, &dropped);
    NTEST_EXPECT_GT_UINT(dropped, 0U);
    frames(&fx, 2U);
    NTEST_EXPECT_EQ_UINT(body_count(&fx), 111U);
    for (u32 i = 0; i < 20U; ++i) {
        NTEST_EXPECT_EQ_INT(nph_physics_body_is_null(body_of(&fx, first[i])) ? 1 : 0,
                            i < 10U ? 1 : 0);
    }
    nph_engine_play_end(e);
    finish(&fx);
}

// A pile of boxes and balls, stepped for 1.5 s: the world's hash at the end.
static u64 pile_hash(const char *const *settings, u32 count, u32 solver)
{
    static PhysFixture fx;
    start_with(&fx, settings, count);
    NphEngine *e = fx.engine;
    NphCvar *cv = nph_cvar_find(nph_engine_cvars(e), NPH_STR("phys.solver"));
    NTEST_ASSERT_NOT_NULL(cv);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(nph_engine_cvars(e), cv, (i64)solver), NPH_RESULT_OK);
    (void)spawn_floor(&fx);
    const NphCollider cube = box_collider(nph_vec3(0.25F, 0.25F, 0.25F));
    const NphCollider ball = sphere_collider(0.2F);
    for (u32 i = 0; i < 24U; ++i) {
        const NphDVec3 at =
            nph_dvec3(0.13 * (f64)(i % 3U), 0.3 + 0.55 * (f64)i, 0.07 * (f64)(i % 5U));
        (void)spawn(&fx, at, NPH_RIGID_BODY_DYNAMIC, i % 4U == 3U ? &ball : &cube);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(e, true), NPH_RESULT_OK);
    frames(&fx, 360U);
    const u64 h = nph_world_hash(nph_engine_world(e));
    NTEST_EXPECT_GT_UINT(nph_physics_world_stats(nph_engine_physics(e)).touching_contacts, 10U);
    nph_engine_play_end(e);
    finish(&fx);
    return h;
}

NTEST(Physics, runs_repeat_whatever_the_threads)
{
    static const char *const pipelined[1] = {"rt.lockstep=true"};
    static const char *const sequential[3] = {"rt.lockstep=true", "rt.pipelined=false",
                                              "rt.workers=1"};
    for (u32 solver = 0; solver < 3U; ++solver) {
        const u64 a = pile_hash(pipelined, 1U, solver);
        const u64 b = pile_hash(pipelined, 1U, solver);
        const u64 c = pile_hash(sequential, 3U, solver);
        NTEST_EXPECT_EQ_UINT(a, b);
        NTEST_EXPECT_EQ_UINT(a, c);
    }
}
