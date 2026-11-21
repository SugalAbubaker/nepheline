// SPDX-License-Identifier: Apache-2.0
// Characters in the engine (components.h CharacterController; docs/runtime.md
// "Characters"; ADR-0108): a CharacterController stands on the floor and walks
// where the game says, falls, jumps and lands; climbs the steps and slopes its
// component allows; rides what it stands on, turning platforms and lifts too,
// and keeps a platform's velocity when it jumps; pushes as its mass; is seen by
// triggers and rays, by its filter, through its body; goes where the game puts
// it and is made again when its component changes; is its entity's body, and
// goes with its component; moves in the world under a parent; and repeats.
// Headless engines in lockstep: one step a frame, 240 a second.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/query.h>
#include <nepheline/physics/shape.h>
#include <nepheline/runtime/engine.h>

#include <ntest/ntest.h>

#include <string.h>

#define NO_RIGID_BODY NPH_U32_MAX
#define STEP          (1.0 / 240.0)
#define GRAVITY       9.81

typedef struct CharFixture {
    NphDebugAllocator da;
    NphEngineConfig config;
    NphEngine *engine;
    NphRuntimeComponents ids;
} CharFixture;

static void start(CharFixture *f)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->config.headless = 1;
    f->config.assignments[0] = "rt.lockstep=true";
    f->config.assignment_count = 1U;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.config = &f->config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "physics character tests";
    NTEST_ASSERT_EQ_INT(nph_engine_create(&d, &f->engine), NPH_RESULT_OK);
    f->ids = *nph_engine_components(f->engine);
}

static void finish(CharFixture *f)
{
    nph_engine_destroy(f->engine);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

static void play(CharFixture *f)
{
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f->engine, false), NPH_RESULT_OK);
}

// Runs `n` frames (steps, in lockstep) and waits for the last one's.
static void frames(CharFixture *f, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(f->engine), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_wait(f->engine), NPH_RESULT_OK);
}

static NphWorld *world_of(CharFixture *f)
{
    return nph_engine_world(f->engine);
}

static NphLocalTransform *local(CharFixture *f, NphEntity e)
{
    return NPH_WORLD_GET_MUT(world_of(f), e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
}

static NphDVec3 at(CharFixture *f, NphEntity e)
{
    return local(f, e)->translation;
}

static NphCharacterController *character(CharFixture *f, NphEntity e)
{
    return NPH_WORLD_GET_MUT(world_of(f), e, f->ids.character_controller, NphCharacterController);
}

static NphRigidBody *rigid(CharFixture *f, NphEntity e)
{
    return NPH_WORLD_GET_MUT(world_of(f), e, f->ids.rigid_body, NphRigidBody);
}

// An entity at `where`, turned by `rotation`, with a box Collider of half extents `half` and a
// RigidBody of `type` (NO_RIGID_BODY: none, a static body).
static NphEntity spawn_box_turned(CharFixture *f, NphDVec3 where, NphQuat rotation, NphVec3 half,
                                  u32 type)
{
    NphWorld *w = world_of(f);
    NphComponentId comps[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM, NPH_COMPONENT_ID_WORLD_TRANSFORM,
                               f->ids.collider};
    u32 n = 3U;
    if (type != NO_RIGID_BODY) {
        comps[n++] = f->ids.rigid_body;
    }
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, n, &e), NPH_RESULT_OK);
    local(f, e)->translation = where;
    local(f, e)->rotation = rotation;
    NPH_WORLD_GET_MUT(w, e, f->ids.collider, NphCollider)->half_extents = half;
    if (type != NO_RIGID_BODY) {
        rigid(f, e)->type = type;
    }
    return e;
}

static NphEntity spawn_box(CharFixture *f, NphDVec3 where, NphVec3 half, u32 type)
{
    return spawn_box_turned(f, where, nph_quat_identity(), half, type);
}

// A static floor 40 m across, its top at y = 0.
static NphEntity spawn_floor(CharFixture *f)
{
    return spawn_box(f, nph_dvec3(0.0, -0.5, 0.0), nph_vec3(20.0F, 0.5F, 20.0F), NO_RIGID_BODY);
}

// A character of the component's defaults (1.8 m tall, 0.3 m round), its feet at `where`.
static NphEntity spawn_character(CharFixture *f, NphDVec3 where)
{
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.character_controller};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(world_of(f), comps, 3U, &e), NPH_RESULT_OK);
    local(f, e)->translation = where;
    return e;
}

static bool quat_bits_equal(NphQuat a, NphQuat b)
{
    return nph_f32_to_bits(a.x) == nph_f32_to_bits(b.x) &&
           nph_f32_to_bits(a.y) == nph_f32_to_bits(b.y) &&
           nph_f32_to_bits(a.z) == nph_f32_to_bits(b.z) &&
           nph_f32_to_bits(a.w) == nph_f32_to_bits(b.w);
}

static bool dvec3_bits_equal(NphDVec3 a, NphDVec3 b)
{
    return nph_f64_to_bits(a.x) == nph_f64_to_bits(b.x) &&
           nph_f64_to_bits(a.y) == nph_f64_to_bits(b.y) &&
           nph_f64_to_bits(a.z) == nph_f64_to_bits(b.z);
}

NTEST(Characters, a_character_stands_on_the_floor_and_walks_where_the_game_says)
{
    static CharFixture fx;
    start(&fx);
    const NphEntity floor = spawn_floor(&fx);
    const NphEntity hero = spawn_character(&fx, nph_dvec3(0.0, 0.0, 0.0));
    const NphQuat facing = nph_quat_from_rotation_y(0.5F);
    local(&fx, hero)->rotation = facing;
    play(&fx);
    // Its body is there at once, for the game's play_begin: kinematic, named by its entity, a
    // ghost capsule 1.8 m tall standing a skin above its feet.
    NphPhysicsWorld *pw = nph_engine_physics(fx.engine);
    const NphPhysicsBody body = nph_engine_physics_body(fx.engine, hero);
    NTEST_ASSERT_FALSE(nph_physics_body_is_null(body));
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, body), (int)NPH_PHYSICS_BODY_KINEMATIC);
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(fx.engine, body).value, hero.value);
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_UINT(nph_physics_body_shapes(pw, body, &shape, 1U), 1U);
    const NphPhysicsGeometry g = nph_physics_shape_geometry(pw, shape);
    NTEST_EXPECT_EQ_INT((int)g.type, (int)NPH_PHYSICS_SHAPE_CAPSULE);
    NTEST_EXPECT_NEAR_F32(g.capsule.radius, 0.3F, 0.0F);
    NTEST_EXPECT_NEAR_F32(g.capsule.half_height, 0.6F, 1e-6F);
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body).y, 0.905, 1e-6);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).bodies, 2U);

    frames(&fx, 10U);
    NTEST_EXPECT_TRUE(character(&fx, hero)->grounded != 0);
    NTEST_EXPECT_EQ_UINT(character(&fx, hero)->ground.value, floor.value);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 0.0, 1e-4);
    NTEST_EXPECT_NEAR_F32(nph_vec3_length(character(&fx, hero)->velocity), 0.0F, 1e-4F);

    // A metre a second along +X, its upward part ignored: a metre in a second.
    character(&fx, hero)->move = nph_vec3(1.0F, 3.0F, 0.0F);
    frames(&fx, 240U);
    const NphCharacterController *c = character(&fx, hero);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x, 1.0, 0.005);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 0.0, 1e-4);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).z, 0.0, 1e-6);
    NTEST_EXPECT_NEAR_F32(c->velocity.x, 1.0F, 1e-3F);
    NTEST_EXPECT_NEAR_F32(c->velocity.y, 0.0F, 1e-3F);
    NTEST_EXPECT_TRUE(c->grounded != 0);
    // Its rotation is the game's, and its body went with it.
    NTEST_EXPECT_TRUE(quat_bits_equal(local(&fx, hero)->rotation, facing));
    NTEST_EXPECT_NEAR_F64(nph_physics_body_position(pw, body).x, at(&fx, hero).x, 1e-6);
    // The world transform composed from where it went.
    const NphWorldTransform *wt =
        NPH_WORLD_GET(world_of(&fx), hero, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    NTEST_EXPECT_NEAR_F64(wt->translation.x, at(&fx, hero).x, 1e-9);

    // It stops at once.
    character(&fx, hero)->move = nph_vec3_zero();
    frames(&fx, 1U);
    const f64 x = at(&fx, hero).x;
    frames(&fx, 60U);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x, x, 1e-9);
    NTEST_EXPECT_NEAR_F32(nph_vec3_length(character(&fx, hero)->velocity), 0.0F, 1e-6F);

    nph_engine_play_end(fx.engine);
    NTEST_EXPECT_TRUE(nph_physics_body_is_null(nph_engine_physics_body(fx.engine, hero)));
    finish(&fx);
}

NTEST(Characters, a_character_falls_jumps_and_lands)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    const NphEntity hero = spawn_character(&fx, nph_dvec3(0.0, 2.0, 0.0));
    play(&fx);
    // A quarter of a second of fall, the velocity first at every step: g dt^2 n (n + 1) / 2.
    frames(&fx, 60U);
    const f64 fallen = GRAVITY * STEP * STEP * 60.0 * 61.0 * 0.5;
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 2.0 - fallen, 1e-3);
    NTEST_EXPECT_NEAR_F32(character(&fx, hero)->velocity.y, (f32)(-GRAVITY * 0.25), 0.01F);
    NTEST_EXPECT_FALSE(character(&fx, hero)->grounded != 0);
    NTEST_EXPECT_TRUE(nph_entity_is_null(character(&fx, hero)->ground));
    frames(&fx, 240U);
    NTEST_EXPECT_TRUE(character(&fx, hero)->grounded != 0);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 0.0, 1e-3);

    // A jump of 5 m/s: taken at once, and cleared.
    character(&fx, hero)->jump = 5.0F;
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F32(character(&fx, hero)->jump, 0.0F, 0.0F);
    NTEST_EXPECT_FALSE(character(&fx, hero)->grounded != 0);
    NTEST_EXPECT_NEAR_F32(character(&fx, hero)->velocity.y, 5.0F, 1e-3F);
    // One asked for in the air is not taken (and cleared too).
    character(&fx, hero)->jump = 5.0F;
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F32(character(&fx, hero)->jump, 0.0F, 0.0F);
    NTEST_EXPECT_TRUE(character(&fx, hero)->velocity.y < 5.0F);
    // Up about v^2 / 2g, and down again after about 2v / g.
    f64 peak = 0.0;
    u32 flight = 2U;
    while (character(&fx, hero)->grounded == 0 && flight < 960U) {
        frames(&fx, 1U);
        peak = nph_f64_max(peak, at(&fx, hero).y);
        flight += 1U;
    }
    NTEST_LOG("a 5 m/s jump peaked at %.4f m and landed after %u steps", peak, flight);
    NTEST_EXPECT_NEAR_F64(peak, 25.0 / (2.0 * GRAVITY), 0.02);
    NTEST_EXPECT_NEAR_F64((f64)flight * STEP, 10.0 / GRAVITY, 0.02);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 0.0, 1e-3);

    // At half gravity, twice as high and twice as long.
    character(&fx, hero)->gravity_scale = 0.5F;
    character(&fx, hero)->jump = 5.0F;
    frames(&fx, 1U);
    peak = 0.0;
    flight = 1U;
    while (character(&fx, hero)->grounded == 0 && flight < 960U) {
        frames(&fx, 1U);
        peak = nph_f64_max(peak, at(&fx, hero).y);
        flight += 1U;
    }
    NTEST_LOG("at half gravity it peaked at %.4f m and landed after %u steps", peak, flight);
    NTEST_EXPECT_NEAR_F64(peak, 25.0 / GRAVITY, 0.03);
    NTEST_EXPECT_NEAR_F64((f64)flight * STEP, 20.0 / GRAVITY, 0.03);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

NTEST(Characters, a_character_climbs_the_steps_its_component_allows)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    // A step 0.4 m high, its face at x = 1, across both characters' ways.
    spawn_box(&fx, nph_dvec3(1.5, 0.2, 0.0), nph_vec3(0.5F, 0.2F, 3.0F), NO_RIGID_BODY);
    const NphEntity low = spawn_character(&fx, nph_dvec3(0.0, 0.0, -1.5));
    const NphEntity high = spawn_character(&fx, nph_dvec3(0.0, 0.0, 1.5));
    character(&fx, high)->step_height = 0.5F;
    play(&fx);
    character(&fx, low)->move = nph_vec3(1.0F, 0.0F, 0.0F);
    character(&fx, high)->move = nph_vec3(1.0F, 0.0F, 0.0F);
    frames(&fx, 480U);
    // Steps of 0.3 m stop at its face, a skin off; of 0.5 m climb it.
    NTEST_LOG("the low stepper at x %.4f y %.4f, the high one at x %.4f y %.4f", at(&fx, low).x,
              at(&fx, low).y, at(&fx, high).x, at(&fx, high).y);
    NTEST_EXPECT_NEAR_F64(at(&fx, low).x, 1.0 - 0.3 - 0.005, 0.002);
    NTEST_EXPECT_NEAR_F64(at(&fx, low).y, 0.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(at(&fx, high).y, 0.4, 1e-3);
    NTEST_EXPECT_TRUE(at(&fx, high).x > 1.5);
    NTEST_EXPECT_TRUE(character(&fx, high)->grounded != 0);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

NTEST(Characters, a_character_walks_up_the_slopes_its_component_allows)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    // A ramp of 35 degrees rising along +X from x = 2, across both characters' ways.
    const f32 slope = 0.61086524F;
    const NphQuat tilt = nph_quat_from_axis_angle(nph_vec3_unit_z(), slope);
    const NphDVec3 center =
        nph_dvec3(2.0 + 0.25 * (f64)nph_f32_sin(slope), -0.25 * (f64)nph_f32_cos(slope), 0.0);
    spawn_box_turned(&fx, center, tilt, nph_vec3(3.0F, 0.25F, 3.0F), NO_RIGID_BODY);
    const NphEntity walker = spawn_character(&fx, nph_dvec3(0.0, 0.0, -1.5));
    const NphEntity timid = spawn_character(&fx, nph_dvec3(0.0, 0.0, 1.5));
    character(&fx, timid)->max_slope = 0.52359878F; // 30 degrees
    play(&fx);
    character(&fx, walker)->move = nph_vec3(1.0F, 0.0F, 0.0F);
    character(&fx, timid)->move = nph_vec3(1.0F, 0.0F, 0.0F);
    frames(&fx, 720U);
    NTEST_LOG("the walker at x %.4f y %.4f, the timid one at x %.4f y %.4f", at(&fx, walker).x,
              at(&fx, walker).y, at(&fx, timid).x, at(&fx, timid).y);
    NTEST_EXPECT_TRUE(at(&fx, walker).y > 0.5);
    NTEST_EXPECT_TRUE(character(&fx, walker)->grounded != 0);
    NTEST_EXPECT_TRUE(at(&fx, timid).y < 0.05);
    NTEST_EXPECT_TRUE(at(&fx, timid).x < 2.0);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// The pose of a body's frame.
static NphDVec3 body_at(CharFixture *f, NphEntity e)
{
    return nph_physics_body_position(nph_engine_physics(f->engine),
                                     nph_engine_physics_body(f->engine, e));
}

NTEST(Characters, what_a_character_stands_on_carries_it)
{
    static CharFixture fx;
    start(&fx);
    // A kinematic platform 4 m across, its top at y = 1, and the character on it.
    const NphEntity platform = spawn_box(&fx, nph_dvec3(0.0, 0.9, 0.0), nph_vec3(2.0F, 0.1F, 2.0F),
                                         (u32)NPH_RIGID_BODY_KINEMATIC);
    const NphEntity hero = spawn_character(&fx, nph_dvec3(0.5, 1.0, 0.0));
    play(&fx);
    frames(&fx, 5U);
    NTEST_ASSERT_TRUE(character(&fx, hero)->grounded != 0);
    NTEST_EXPECT_EQ_UINT(character(&fx, hero)->ground.value, platform.value);

    // Along +X at 1 m/s for a second: it keeps its place on the platform.
    rigid(&fx, platform)->linear_velocity = nph_vec3(1.0F, 0.0F, 0.0F);
    frames(&fx, 240U);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x - body_at(&fx, platform).x, 0.5, 0.01);
    NTEST_EXPECT_NEAR_F32(character(&fx, hero)->velocity.x, 1.0F, 0.01F);
    NTEST_EXPECT_TRUE(character(&fx, hero)->grounded != 0);

    // Up and down at 2 m/s, a lift: on its feet at every step, a skin over the deck.
    u32 aloft = 0U;
    f64 off = 0.0;
    for (u32 leg = 0U; leg < 2U; ++leg) {
        rigid(&fx, platform)->linear_velocity = nph_vec3(0.0F, leg == 0U ? 2.0F : -2.0F, 0.0F);
        for (u32 i = 0U; i < 120U; ++i) {
            frames(&fx, 1U);
            aloft += character(&fx, hero)->grounded != 0 ? 0U : 1U;
            off = nph_f64_max(off, nph_f64_abs(at(&fx, hero).y - (body_at(&fx, platform).y + 0.1)));
        }
    }
    NTEST_LOG("on the lift: %u steps off its feet, at most %.4f m off the deck", aloft, off);
    NTEST_EXPECT_EQ_UINT(aloft, 0U);
    NTEST_EXPECT_TRUE(off < 0.02);

    // Turning at 1 rad/s about the platform's centre: a quarter turn in pi/2 s takes the
    // character from +X of the centre to -Z of it.
    rigid(&fx, platform)->linear_velocity = nph_vec3_zero();
    frames(&fx, 1U);
    const NphDVec3 middle = body_at(&fx, platform);
    rigid(&fx, platform)->angular_velocity = nph_vec3(0.0F, 1.0F, 0.0F);
    frames(&fx, 377U); // pi / 2 s
    rigid(&fx, platform)->angular_velocity = nph_vec3_zero();
    frames(&fx, 1U);
    const NphDVec3 h = at(&fx, hero);
    NTEST_LOG("a quarter turn took the character to (%.4f, %.4f) of the centre", h.x - middle.x,
              h.z - middle.z);
    NTEST_EXPECT_NEAR_F64(h.x - middle.x, 0.0, 0.03);
    NTEST_EXPECT_NEAR_F64(h.z - middle.z, -0.5, 0.03);

    // Jumping off it moving at 2 m/s along +Z, it keeps the platform's velocity in the air
    // and lands about where it was on it.
    rigid(&fx, platform)->linear_velocity = nph_vec3(0.0F, 0.0F, 2.0F);
    frames(&fx, 10U);
    const f64 before = at(&fx, hero).z - body_at(&fx, platform).z;
    character(&fx, hero)->jump = 3.0F;
    frames(&fx, 1U);
    NTEST_EXPECT_FALSE(character(&fx, hero)->grounded != 0);
    u32 flight = 0U;
    while (character(&fx, hero)->grounded == 0 && flight < 480U) {
        frames(&fx, 1U);
        flight += 1U;
    }
    const f64 after = at(&fx, hero).z - body_at(&fx, platform).z;
    NTEST_LOG("a jump of %u steps from the moving platform: %.4f m along it before, %.4f after",
              flight, before, after);
    NTEST_EXPECT_GT_UINT(flight, 100U);
    NTEST_EXPECT_NEAR_F64(after, before, 0.03);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

NTEST(Characters, a_character_pushes_what_it_walks_into_as_its_mass)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    // Two crates of 20 kg, a character walking into each at 2 m/s: one pushing as 80 kg, one
    // as nothing.
    NphEntity crates[2];
    NphEntity heroes[2];
    for (u32 i = 0U; i < 2U; ++i) {
        const f64 z = i == 0U ? -2.0 : 2.0;
        crates[i] =
            spawn_box(&fx, nph_dvec3(1.0, 0.25, z), nph_vec3_splat(0.25F), NPH_RIGID_BODY_DYNAMIC);
        NPH_WORLD_GET_MUT(world_of(&fx), crates[i], fx.ids.collider, NphCollider)->density = 160.0F;
        heroes[i] = spawn_character(&fx, nph_dvec3(0.0, 0.0, z));
    }
    character(&fx, heroes[1])->push_mass = 0.0F;
    play(&fx);
    frames(&fx, 5U);
    for (u32 i = 0U; i < 2U; ++i) {
        character(&fx, heroes[i])->move = nph_vec3(2.0F, 0.0F, 0.0F);
    }
    frames(&fx, 240U);
    NTEST_LOG("pushing: crate at x %.4f, character at %.4f; not: crate at %.4f, character at %.4f",
              at(&fx, crates[0]).x, at(&fx, heroes[0]).x, at(&fx, crates[1]).x,
              at(&fx, heroes[1]).x);
    // Pushed along at about its pace.
    NTEST_EXPECT_TRUE(at(&fx, crates[0]).x > 2.2);
    NTEST_EXPECT_TRUE(at(&fx, heroes[0]).x > 1.6);
    // Not pushed: the crate stays, and the character stops a skin off it.
    NTEST_EXPECT_NEAR_F64(at(&fx, crates[1]).x, 1.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(at(&fx, heroes[1]).x, 1.0 - 0.25 - 0.3 - 0.005, 0.002);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

static bool has_event(const NphTriggerEvent *events, u32 count, NphEntity trigger, NphEntity who)
{
    for (u32 i = 0U; i < count; ++i) {
        if (events[i].trigger.value == trigger.value && events[i].visitor.value == who.value &&
            events[i].body.value == who.value) {
            return true;
        }
    }
    return false;
}

NTEST(Characters, triggers_and_rays_see_a_character_by_its_filter)
{
    static CharFixture fx;
    start(&fx);
    const NphEntity floor = spawn_floor(&fx);
    // Two triggers on its way: one seeing every category, one only the default category.
    const NphEntity gate =
        spawn_box(&fx, nph_dvec3(2.0, 1.0, 0.0), nph_vec3(0.25F, 1.0F, 1.0F), NO_RIGID_BODY);
    const NphEntity blind =
        spawn_box(&fx, nph_dvec3(4.0, 1.0, 0.0), nph_vec3(0.25F, 1.0F, 1.0F), NO_RIGID_BODY);
    NPH_WORLD_GET_MUT(world_of(&fx), gate, fx.ids.collider, NphCollider)->trigger = 1;
    NphCollider *b = NPH_WORLD_GET_MUT(world_of(&fx), blind, fx.ids.collider, NphCollider);
    b->trigger = 1;
    b->mask = 1U;
    const NphEntity hero = spawn_character(&fx, nph_dvec3(0.0, 0.0, 0.0));
    character(&fx, hero)->category = 2U;
    play(&fx);
    frames(&fx, 5U);
    character(&fx, hero)->move = nph_vec3(2.0F, 0.0F, 0.0F);
    u32 entered = 0U;
    u32 left = 0U;
    u32 blind_events = 0U;
    for (u32 i = 0U; i < 480U; ++i) {
        frames(&fx, 1U);
        const NphTriggerEvents t = nph_engine_physics_triggers(fx.engine);
        entered += has_event(t.entered, t.entered_count, gate, hero) ? 1U : 0U;
        left += has_event(t.left, t.left_count, gate, hero) ? 1U : 0U;
        blind_events += has_event(t.entered, t.entered_count, blind, hero) ? 1U : 0U;
        blind_events += has_event(t.left, t.left_count, blind, hero) ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(entered, 1U);
    NTEST_EXPECT_EQ_UINT(left, 1U);
    NTEST_EXPECT_EQ_UINT(blind_events, 0U);

    // A ray down onto it meets its capsule's top, a skin and 1.8 m over its feet; one that
    // ignores its body, or whose mask leaves out its category, meets the floor.
    character(&fx, hero)->move = nph_vec3_zero();
    frames(&fx, 1U);
    const NphPhysicsWorld *pw = nph_engine_physics(fx.engine);
    const NphDVec3 feet = at(&fx, hero);
    const NphDVec3 above = nph_dvec3(feet.x, 5.0, feet.z);
    const NphVec3 down = nph_vec3(0.0F, -10.0F, 0.0F);
    NphPhysicsQueryFilter filter = nph_physics_query_filter_default();
    NphPhysicsQueryHit hit;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(pw, above, down, &filter, &hit));
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(fx.engine, hit.body).value, hero.value);
    NTEST_EXPECT_NEAR_F64(hit.point.y, feet.y + 1.805, 1e-4);
    filter.ignore = nph_engine_physics_body(fx.engine, hero);
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(pw, above, down, &filter, &hit));
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(fx.engine, hit.body).value, floor.value);
    filter = nph_physics_query_filter_default();
    filter.mask = 1U;
    NTEST_ASSERT_TRUE(nph_physics_world_raycast_closest(pw, above, down, &filter, &hit));
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_entity(fx.engine, hit.body).value, floor.value);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

NTEST(Characters, a_character_goes_where_the_game_puts_it_and_is_made_again_when_changed)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    const NphEntity hero = spawn_character(&fx, nph_dvec3(0.0, 0.0, 0.0));
    play(&fx);
    character(&fx, hero)->move = nph_vec3(1.0F, 0.0F, 0.0F);
    frames(&fx, 24U);
    // Put 3 m up elsewhere: there at the next step, falling from rest.
    local(&fx, hero)->translation = nph_dvec3(5.0, 3.0, 0.0);
    character(&fx, hero)->move = nph_vec3_zero();
    frames(&fx, 1U);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x, 5.0, 1e-9);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 3.0 - GRAVITY * STEP * STEP, 1e-6);
    NTEST_EXPECT_NEAR_F32(character(&fx, hero)->velocity.y, (f32)(-GRAVITY * STEP), 1e-4F);
    frames(&fx, 480U);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 0.0, 1e-3);
    NTEST_EXPECT_TRUE(character(&fx, hero)->grounded != 0);

    // A thinner, taller capsule: made again where it stands.
    character(&fx, hero)->radius = 0.2F;
    character(&fx, hero)->height = 2.0F;
    frames(&fx, 1U);
    NphPhysicsWorld *pw = nph_engine_physics(fx.engine);
    NphPhysicsShape shape;
    NTEST_ASSERT_EQ_UINT(
        nph_physics_body_shapes(pw, nph_engine_physics_body(fx.engine, hero), &shape, 1U), 1U);
    const NphPhysicsGeometry g = nph_physics_shape_geometry(pw, shape);
    NTEST_EXPECT_NEAR_F32(g.capsule.radius, 0.2F, 0.0F);
    NTEST_EXPECT_NEAR_F32(g.capsule.half_height, 0.8F, 1e-6F);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x, 5.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).y, 0.0, 1e-3);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).bodies, 2U);

    // No capsule (no radius): no body, and it does not move until it has one again.
    character(&fx, hero)->radius = 0.0F;
    character(&fx, hero)->move = nph_vec3(1.0F, 0.0F, 0.0F);
    frames(&fx, 60U);
    NTEST_EXPECT_TRUE(nph_physics_body_is_null(nph_engine_physics_body(fx.engine, hero)));
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x, 5.0, 1e-6);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).bodies, 1U);
    character(&fx, hero)->radius = 0.3F;
    frames(&fx, 60U);
    NTEST_EXPECT_FALSE(nph_physics_body_is_null(nph_engine_physics_body(fx.engine, hero)));
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x, 5.25, 0.005);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

NTEST(Characters, a_character_is_its_entitys_body_and_goes_with_its_component)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    // A dynamic box that is a character too: the character is its body.
    const NphEntity hero =
        spawn_box(&fx, nph_dvec3(0.0, 0.0, 0.0), nph_vec3_splat(0.5F), NPH_RIGID_BODY_DYNAMIC);
    NTEST_ASSERT_EQ_INT(nph_world_add(world_of(&fx), hero, fx.ids.character_controller, NULL),
                        NPH_RESULT_OK);
    play(&fx);
    NphPhysicsWorld *pw = nph_engine_physics(fx.engine);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).bodies, 2U);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, nph_engine_physics_body(fx.engine, hero)),
                        (int)NPH_PHYSICS_BODY_KINEMATIC);
    frames(&fx, 10U);
    NTEST_EXPECT_TRUE(character(&fx, hero)->grounded != 0);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).shapes, 2U); // the floor and the capsule

    // Without its CharacterController, its RigidBody and Collider make its body again.
    NTEST_ASSERT_EQ_INT(nph_world_remove(world_of(&fx), hero, fx.ids.character_controller),
                        NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).bodies, 2U);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, nph_engine_physics_body(fx.engine, hero)),
                        (int)NPH_PHYSICS_BODY_DYNAMIC);
    // With one again, the character.
    NTEST_ASSERT_EQ_INT(nph_world_add(world_of(&fx), hero, fx.ids.character_controller, NULL),
                        NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).bodies, 2U);
    NTEST_EXPECT_EQ_INT((int)nph_physics_body_type(pw, nph_engine_physics_body(fx.engine, hero)),
                        (int)NPH_PHYSICS_BODY_KINEMATIC);
    // Despawned, its body goes.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(world_of(&fx), hero), NPH_RESULT_OK);
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(nph_physics_world_stats(pw).bodies, 1U);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

NTEST(Characters, a_character_under_a_parent_moves_in_the_world)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    const NphComponentId holder_comps[2] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                            NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NphEntity holder;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(world_of(&fx), holder_comps, 2U, &holder), NPH_RESULT_OK);
    local(&fx, holder)->translation = nph_dvec3(10.0, 0.0, 0.0);
    const NphEntity hero = spawn_character(&fx, nph_dvec3(0.0, 0.0, 0.0));
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(world_of(&fx), hero, holder), NPH_RESULT_OK);
    frames(&fx, 1U); // the world transforms composed before play
    play(&fx);
    const NphPhysicsWorld *pw = nph_engine_physics(fx.engine);
    NTEST_EXPECT_NEAR_F64(body_at(&fx, hero).x, 10.0, 1e-9);
    character(&fx, hero)->move = nph_vec3(1.0F, 0.0F, 0.0F);
    frames(&fx, 240U);
    character(&fx, hero)->move = nph_vec3_zero();
    frames(&fx, 1U);
    const f64 x = nph_physics_body_position(pw, nph_engine_physics_body(fx.engine, hero)).x;
    NTEST_EXPECT_NEAR_F64(x, 11.0, 0.01);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).x, x - 10.0, 1e-6);
    // The parent moving does not carry it: its translation is written against the parent.
    local(&fx, holder)->translation = nph_dvec3(10.0, 0.0, 5.0);
    frames(&fx, 2U);
    const NphWorldTransform *wt =
        NPH_WORLD_GET(world_of(&fx), hero, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    NTEST_EXPECT_NEAR_F64(wt->translation.x, x, 1e-6);
    NTEST_EXPECT_NEAR_F64(wt->translation.z, 0.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(at(&fx, hero).z, -5.0, 1e-6);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// A course: a character walks into a crate, pushes it into a step, and climbs the step; a
// second walks into the first. Where both and the crate end.
static void course(NphDVec3 *out)
{
    static CharFixture fx;
    start(&fx);
    spawn_floor(&fx);
    spawn_box(&fx, nph_dvec3(4.5, 0.15, 0.0), nph_vec3(1.5F, 0.15F, 3.0F), NO_RIGID_BODY);
    const NphEntity crate =
        spawn_box(&fx, nph_dvec3(1.0, 0.25, 0.3), nph_vec3_splat(0.25F), NPH_RIGID_BODY_DYNAMIC);
    const NphEntity first = spawn_character(&fx, nph_dvec3(0.0, 0.0, 0.0));
    const NphEntity second = spawn_character(&fx, nph_dvec3(-1.5, 0.0, 0.1));
    play(&fx);
    character(&fx, first)->move = nph_vec3(1.5F, 0.0F, 0.0F);
    character(&fx, second)->move = nph_vec3(2.0F, 0.0F, 0.0F);
    frames(&fx, 720U);
    out[0] = at(&fx, first);
    out[1] = at(&fx, second);
    out[2] = at(&fx, crate);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

NTEST(Characters, characters_repeat_bit_for_bit)
{
    NphDVec3 a[3];
    NphDVec3 b[3];
    course(a);
    course(b);
    NTEST_LOG("first at (%.4f, %.4f, %.4f), second at (%.4f, %.4f, %.4f), crate at (%.4f, %.4f, "
              "%.4f)",
              a[0].x, a[0].y, a[0].z, a[1].x, a[1].y, a[1].z, a[2].x, a[2].y, a[2].z);
    for (u32 i = 0U; i < 3U; ++i) {
        NTEST_EXPECT_TRUE(dvec3_bits_equal(a[i], b[i]));
    }
    // The first climbed the step; the second is held behind the first.
    NTEST_EXPECT_NEAR_F64(a[0].y, 0.3, 1e-3);
    NTEST_EXPECT_TRUE(a[1].x < a[0].x);
}
