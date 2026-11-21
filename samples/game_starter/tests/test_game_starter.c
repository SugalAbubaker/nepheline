// SPDX-License-Identifier: Apache-2.0
// The starter game (samples/game_starter; ADR-0108 decision 9) on a
// headless engine, driven by input fed as its window's would be (engine.h
// nph_engine_feed_event), one step a frame: the player stands at the
// level's PlayerStart (else at the origin, or as the level's own player,
// given what it lacks), its eye the camera at 0.94 of its height with the
// level's exposure, the level's cameras and listeners put out in the play
// copy alone; W walks it and Shift runs, the mouse turns the body and tilts
// the eye, Space jumps, E pushes a crate and swings a door on its hinge,
// walking shoves a crate, and a fall stands it back at the start. It asks
// for the mouse captured while it plays.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/math/scalar.h>
#include <nepheline/physics/body.h>
#include <nepheline/runtime/components.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/game.h>
#include <nepheline/scene/transform.h>

#include <ntest/ntest.h>

#include <string.h>

#ifndef NPH_STARTER_GAME
#error "NPH_STARTER_GAME names the starter game's module"
#endif

// The starter's defaults (game.c): walking and running speeds, the eye.
#define WALK       2.5F
#define RUN        6.0F
#define LOOK       0.0025F // radians a mouse unit
#define EYE_HEIGHT (0.94F * 1.8F)
#define STEP       (1.0F / 240.0F)
#define NO_BODY    NPH_U32_MAX

typedef struct StarterFixture {
    NphDebugAllocator da;
    NphEngine *engine;
    NphRuntimeComponents ids;
    NphComponentId player_id; // starter.Player
    f64 mouse_x;              // the cursor as the fed events left it
    f64 mouse_y;
} StarterFixture;

NTEST_SETUP(StarterFixture)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    static const char *const assignments[3] = {"rt.lockstep=true", "rt.pipelined=false",
                                               "game.hot_reload=false"};
    NphEngineConfig config;
    memset(&config, 0, sizeof config);
    config.game = NPH_STARTER_GAME;
    config.headless = 1;
    for (u32 i = 0; i < 3U; ++i) {
        config.assignments[i] = assignments[i];
    }
    config.assignment_count = 3U;
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.config = &config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "starter game tests";
    NTEST_ASSERT_EQ_INT(nph_engine_create(&d, &f->engine), NPH_RESULT_OK);
    f->ids = *nph_engine_components(f->engine);
    NphGame *game = nph_engine_game(f->engine);
    NTEST_ASSERT_NOT_NULL(game);
    f->player_id = nph_game_component(game, 0U);
    NTEST_ASSERT_NE_UINT(f->player_id, NPH_COMPONENT_NONE);
    f->mouse_x = 400.0;
    f->mouse_y = 300.0;
}

NTEST_TEARDOWN(StarterFixture)
{
    nph_engine_destroy(f->engine);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

static void frames(StarterFixture *f, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(f->engine), NPH_RESULT_OK);
    }
}

static void feed(StarterFixture *f, NphWindowEvent ev)
{
    NTEST_ASSERT_EQ_INT(nph_engine_feed_event(f->engine, &ev), NPH_RESULT_OK);
}

static void key(StarterFixture *f, NphKey k, bool down)
{
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_KEY;
    ev.u.key.key = (u32)k;
    ev.u.key.action = (u32)(down ? NPH_KEY_ACTION_PRESS : NPH_KEY_ACTION_RELEASE);
    feed(f, ev);
}

// A key pressed and let go within a frame: one press for the step.
static void tap(StarterFixture *f, NphKey k)
{
    key(f, k, true);
    key(f, k, false);
    frames(f, 1U);
}

// The cursor moved by (dx, dy), taken by the next frame.
static void mouse_by(StarterFixture *f, f64 dx, f64 dy)
{
    f->mouse_x += dx;
    f->mouse_y += dy;
    NphWindowEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (u32)NPH_WINDOW_EVENT_MOUSE_MOVE;
    ev.u.mouse_move.x = f->mouse_x;
    ev.u.mouse_move.y = f->mouse_y;
    feed(f, ev);
}

// ---------------------------------------------------------------------------
// The level
// ---------------------------------------------------------------------------

static NphWorld *level(StarterFixture *f)
{
    return nph_engine_edit_world(f->engine);
}

static NphLocalTransform *local_of(NphWorld *w, NphEntity e)
{
    return NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
}

// A box of `half` extents at `at`: static with NO_BODY, else a RigidBody of
// that type, of `mass` kg (dynamic).
static NphEntity box(StarterFixture *f, NphDVec3 at, NphVec3 half, u32 type, f32 mass)
{
    NphWorld *w = level(f);
    const NphComponentId comps[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.collider,
                                     f->ids.rigid_body};
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, type == NO_BODY ? 3U : 4U, &e), NPH_RESULT_OK);
    local_of(w, e)->translation = at;
    NphCollider *c = NPH_WORLD_GET_MUT(w, e, f->ids.collider, NphCollider);
    c->shape = (u32)NPH_COLLIDER_BOX;
    c->half_extents = half;
    if (type != NO_BODY) {
        NphRigidBody *rb = NPH_WORLD_GET_MUT(w, e, f->ids.rigid_body, NphRigidBody);
        rb->type = type;
        rb->mass = mass;
    }
    return e;
}

static void ground(StarterFixture *f)
{
    (void)box(f, nph_dvec3(0.0, -0.5, 0.0), nph_vec3(30.0F, 0.5F, 30.0F), NO_BODY, 0.0F);
}

// A PlayerStart at `at`, facing `yaw` (0: -Z).
static void player_start(StarterFixture *f, NphDVec3 at, f32 yaw)
{
    NphWorld *w = level(f);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.player_start};
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &e), NPH_RESULT_OK);
    local_of(w, e)->translation = at;
    local_of(w, e)->rotation = nph_quat_from_rotation_y(yaw);
}

static void play(StarterFixture *f)
{
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f->engine, true), NPH_RESULT_OK);
    frames(f, 1U);
}

// The player the starter stands in the play copy.
static NphEntity the_player(StarterFixture *f)
{
    NphWorld *w = nph_engine_world(f->engine);
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        if (nph_world_has(w, e, f->player_id)) {
            return e;
        }
    }
    return NPH_ENTITY_NULL;
}

// starter.Player's fields that the tests read (game.c): yaw, pitch, eye.
typedef struct PlayerView {
    f32 yaw;
    f32 pitch;
    NphEntity eye;
} PlayerView;

static PlayerView player_view(StarterFixture *f, NphEntity player)
{
    NphWorld *w = nph_engine_world(f->engine);
    const NphComponentDesc *d = nph_world_component_desc(w, f->player_id);
    const u8 *p = (const u8 *)nph_world_get(w, player, f->player_id);
    NTEST_ASSERT_NOT_NULL(p);
    PlayerView v;
    memcpy(&v.yaw, p + nph_component_find_field(d, NPH_STR("yaw"))->offset, sizeof v.yaw);
    memcpy(&v.pitch, p + nph_component_find_field(d, NPH_STR("pitch"))->offset, sizeof v.pitch);
    memcpy(&v.eye, p + nph_component_find_field(d, NPH_STR("eye"))->offset, sizeof v.eye);
    return v;
}

static NphDVec3 position(StarterFixture *f, NphEntity e)
{
    const NphLocalTransform *t = local_of(nph_engine_world(f->engine), e);
    NTEST_ASSERT_NOT_NULL(t);
    return t->translation;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

NTEST_F(StarterFixture, the_player_stands_at_the_playerstart_its_eye_the_camera)
{
    ground(f);
    player_start(f, nph_dvec3(2.0, 0.0, 3.0), 1.5707964F); // facing -X
    // The level's camera and listener, which play puts out in its copy.
    NphWorld *w = level(f);
    const NphComponentId cam_comps[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                         NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.camera,
                                         f->ids.audio_listener};
    NphEntity shot = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, cam_comps, 4U, &shot), NPH_RESULT_OK);
    NphCamera *c = NPH_WORLD_GET_MUT(w, shot, f->ids.camera, NphCamera);
    c->ev100 = 9.0F;
    c->tonemapper = (u32)NPH_RENDER_TONEMAPPER_ACES;
    NPH_WORLD_GET_MUT(w, shot, f->ids.audio_listener, NphAudioListener)->active = 1;
    NTEST_EXPECT_FALSE(nph_engine_mouse_capture_wanted(f->engine));
    play(f);
    const NphEntity player = the_player(f);
    NTEST_ASSERT_FALSE(nph_entity_is_null(player));
    NphWorld *pw = nph_engine_world(f->engine);
    NTEST_EXPECT_TRUE(nph_world_has(pw, player, f->ids.character_controller));
    const NphDVec3 at = position(f, player);
    NTEST_EXPECT_NEAR_F64(at.x, 2.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(at.z, 3.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(at.y, 0.0, 0.01);
    const PlayerView v = player_view(f, player);
    NTEST_EXPECT_NEAR_F32(v.yaw, 1.5707964F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(v.pitch, 0.0F, 0.0F);
    // The eye: a child at 0.94 of the height, the camera with the level's exposure.
    NTEST_ASSERT_TRUE(nph_world_is_alive(pw, v.eye));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(pw, v.eye), player));
    NTEST_EXPECT_NEAR_F64(local_of(pw, v.eye)->translation.y, (f64)EYE_HEIGHT, 1e-5);
    const NphCamera *eye = NPH_WORLD_GET(pw, v.eye, f->ids.camera, NphCamera);
    NTEST_ASSERT_NOT_NULL(eye);
    NTEST_EXPECT_EQ_INT(eye->active, 1);
    NTEST_EXPECT_NEAR_F32(eye->ev100, 9.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(eye->tonemapper, (u32)NPH_RENDER_TONEMAPPER_ACES);
    NTEST_EXPECT_NEAR_F32(eye->yfov, 1.2F, 0.0F);
    NTEST_EXPECT_EQ_INT(NPH_WORLD_GET(pw, v.eye, f->ids.audio_listener, NphAudioListener)->active,
                        1);
    NTEST_EXPECT_EQ_INT(NPH_WORLD_GET(pw, shot, f->ids.camera, NphCamera)->active, 0);
    NTEST_EXPECT_EQ_INT(NPH_WORLD_GET(pw, shot, f->ids.audio_listener, NphAudioListener)->active,
                        0);
    // It asks for the mouse while it plays.
    NTEST_EXPECT_TRUE(nph_engine_mouse_capture_wanted(f->engine));
    nph_engine_play_end(f->engine);
    NTEST_EXPECT_FALSE(nph_engine_mouse_capture_wanted(f->engine));
    // The level as it was: its camera on, no player.
    NTEST_EXPECT_EQ_INT(NPH_WORLD_GET(w, shot, f->ids.camera, NphCamera)->active, 1);
    NTEST_EXPECT_TRUE(nph_entity_is_null(the_player(f)));
}

NTEST_F(StarterFixture, w_walks_shift_runs_and_the_mouse_turns_the_body_and_tilts_the_eye)
{
    ground(f);
    player_start(f, nph_dvec3(0.0, 0.0, 0.0), 0.0F);
    play(f);
    const NphEntity player = the_player(f);
    NTEST_ASSERT_FALSE(nph_entity_is_null(player));
    mouse_by(f, 0.0, 0.0); // the cursor's first place: no motion yet
    frames(f, 10U);
    // A second of walking: 2.5 m along -Z.
    key(f, NPH_KEY_W, true);
    frames(f, 240U);
    NphDVec3 at = position(f, player);
    NTEST_EXPECT_NEAR_F64(at.x, 0.0, 1e-3);
    NTEST_EXPECT_NEAR_F64(at.z, -(f64)WALK, 0.03);
    // A second of running: 6 m more.
    key(f, NPH_KEY_LEFT_SHIFT, true);
    frames(f, 240U);
    const NphDVec3 ran = position(f, player);
    NTEST_EXPECT_NEAR_F64(ran.z - at.z, -(f64)RUN, 0.05);
    key(f, NPH_KEY_LEFT_SHIFT, false);
    key(f, NPH_KEY_W, false);
    frames(f, 2U);
    // The mouse: right turns the body right (yaw down), down tilts the eye down.
    mouse_by(f, 200.0, 100.0);
    frames(f, 1U);
    const PlayerView v = player_view(f, player);
    NTEST_EXPECT_NEAR_F32(v.yaw, -200.0F * LOOK, 1e-5F);
    NTEST_EXPECT_NEAR_F32(v.pitch, -100.0F * LOOK, 1e-5F);
    NphWorld *pw = nph_engine_world(f->engine);
    const NphQuat body = local_of(pw, player)->rotation;
    const NphQuat tilt = local_of(pw, v.eye)->rotation;
    const NphQuat want_body = nph_quat_from_rotation_y(v.yaw);
    const NphQuat want_tilt = nph_quat_from_rotation_x(v.pitch);
    NTEST_EXPECT_NEAR_F32(nph_f32_abs(nph_quat_dot(body, want_body)), 1.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(nph_f32_abs(nph_quat_dot(tilt, want_tilt)), 1.0F, 1e-6F);
    // W now walks where the body faces, level whatever the eye's tilt.
    at = position(f, player);
    key(f, NPH_KEY_W, true);
    frames(f, 240U);
    key(f, NPH_KEY_W, false);
    const NphDVec3 to = position(f, player);
    const NphVec3 facing = nph_quat_rotate(want_body, nph_vec3(0.0F, 0.0F, -1.0F));
    NTEST_EXPECT_NEAR_F64(to.x - at.x, (f64)(facing.x * WALK), 0.03);
    NTEST_EXPECT_NEAR_F64(to.z - at.z, (f64)(facing.z * WALK), 0.03);
    NTEST_EXPECT_NEAR_F64(to.y, 0.0, 0.01);
}

NTEST_F(StarterFixture, space_jumps_from_the_ground_and_lands)
{
    ground(f);
    player_start(f, nph_dvec3(0.0, 0.0, 0.0), 0.0F);
    play(f);
    const NphEntity player = the_player(f);
    frames(f, 10U);
    tap(f, NPH_KEY_SPACE);
    f64 top = 0.0;
    for (u32 i = 0U; i < 240U; ++i) {
        frames(f, 1U);
        top = nph_f64_max(top, position(f, player).y);
    }
    // 4 m/s up: v^2 / 2g, about 0.82 m.
    NTEST_EXPECT_NEAR_F64(top, 16.0 / (2.0 * 9.81), 0.03);
    NTEST_EXPECT_NEAR_F64(position(f, player).y, 0.0, 0.01);
    const NphCharacterController *cc = NPH_WORLD_GET(
        nph_engine_world(f->engine), player, f->ids.character_controller, NphCharacterController);
    NTEST_EXPECT_EQ_INT(cc->grounded, 1);
}

// The hinge's turn about +Y, from its door's rotation.
static f32 door_angle(StarterFixture *f, NphEntity door)
{
    const NphQuat q = local_of(nph_engine_world(f->engine), door)->rotation;
    return 2.0F * nph_f32_atan2(q.y, q.w);
}

NTEST_F(StarterFixture, e_pushes_what_the_eye_meets_from_where_it_meets_it)
{
    ground(f);
    player_start(f, nph_dvec3(0.0, 0.0, 0.0), 0.0F);
    // A light cupboard straight ahead, its front 1.25 m off: the eye's ray
    // meets it above its centre.
    const NphEntity cupboard = box(f, nph_dvec3(0.0, 1.0, -1.5), nph_vec3(0.5F, 1.0F, 0.25F),
                                   NPH_RIGID_BODY_DYNAMIC, 20.0F);
    play(f);
    frames(f, 10U);
    const f64 z0 = position(f, cupboard).z;
    tap(f, NPH_KEY_E);
    frames(f, 60U);
    // Away along the ray: 40 N s on 20 kg, 2 m/s, slowed by the floor.
    NTEST_EXPECT_TRUE(position(f, cupboard).z < z0 - 0.05);
    // A press at nothing pushes nothing: the cupboard has come to rest.
    mouse_by(f, 0.0, 0.0);
    frames(f, 240U);
    const NphDVec3 rest = position(f, cupboard);
    mouse_by(f, 1256.6, 0.0); // half a turn: facing +Z, the empty ground
    frames(f, 1U);
    tap(f, NPH_KEY_E);
    frames(f, 60U);
    const NphDVec3 still = position(f, cupboard);
    NTEST_EXPECT_NEAR_F64(still.x, rest.x, 1e-3);
    NTEST_EXPECT_NEAR_F64(still.z, rest.z, 1e-3);
}

NTEST_F(StarterFixture, e_swings_a_door_on_its_hinge_within_reach)
{
    ground(f);
    // A door from x = 3 to 4, hinged to the world at x = 4, a hair off the
    // floor; the player facing its free half from 3.5 m away.
    const NphEntity door = box(f, nph_dvec3(3.5, 1.05, -1.5), nph_vec3(0.5F, 1.0F, 0.025F),
                               NPH_RIGID_BODY_DYNAMIC, 30.0F);
    NphWorld *w = level(f);
    NTEST_ASSERT_EQ_INT(nph_world_add(w, door, f->ids.joint, NULL), NPH_RESULT_OK);
    NphJoint *j = NPH_WORLD_GET_MUT(w, door, f->ids.joint, NphJoint);
    j->kind = (u32)NPH_JOINT_HINGE;
    j->anchor = nph_vec3(0.5F, 0.0F, 0.0F);
    j->axis = nph_vec3(0.0F, 1.0F, 0.0F);
    player_start(f, nph_dvec3(3.25, 0.0, 2.0), 0.0F);
    play(f);
    frames(f, 10U);
    // Out of reach (3.5 m against 2.5): nothing.
    const f32 shut = door_angle(f, door);
    tap(f, NPH_KEY_E);
    frames(f, 60U);
    NTEST_EXPECT_NEAR_F32(door_angle(f, door), shut, 1e-4F);
    // A metre and a half nearer, it swings away about its hinge.
    key(f, NPH_KEY_W, true);
    frames(f, 144U);
    key(f, NPH_KEY_W, false);
    frames(f, 30U);
    NTEST_EXPECT_NEAR_F32(door_angle(f, door), shut, 1e-4F); // not walked into
    tap(f, NPH_KEY_E);
    frames(f, 120U);
    NTEST_EXPECT_TRUE(nph_f32_abs(door_angle(f, door) - shut) > 0.2F);
    // Still on its hinge: its centre half its width from the hinge's line.
    const NphDVec3 c = position(f, door);
    const f64 dx = c.x - 4.0;
    const f64 dz = c.z + 1.5;
    NTEST_EXPECT_NEAR_F64(nph_f64_sqrt(dx * dx + dz * dz), 0.5, 0.01);
}

NTEST_F(StarterFixture, walking_into_a_crate_shoves_it)
{
    ground(f);
    player_start(f, nph_dvec3(0.0, 0.0, 0.0), 0.0F);
    const NphEntity crate = box(f, nph_dvec3(0.0, 0.3, -1.5), nph_vec3(0.3F, 0.3F, 0.3F),
                                NPH_RIGID_BODY_DYNAMIC, 10.0F);
    play(f);
    frames(f, 10U);
    const f64 z0 = position(f, crate).z;
    key(f, NPH_KEY_W, true);
    frames(f, 480U);
    key(f, NPH_KEY_W, false);
    NTEST_EXPECT_TRUE(position(f, crate).z < z0 - 1.0);
}

NTEST_F(StarterFixture, a_fall_out_of_the_level_stands_it_back_at_the_start)
{
    player_start(f, nph_dvec3(5.0, 10.0, 0.0), 0.0F); // over nothing
    play(f);
    const NphEntity player = the_player(f);
    f64 lowest = 10.0;
    bool back = false;
    for (u32 i = 0U; i < 1200U && !back; ++i) {
        frames(f, 1U);
        const f64 y = position(f, player).y;
        back = lowest < -30.0 && y > 9.0;
        lowest = nph_f64_min(lowest, y);
    }
    NTEST_EXPECT_TRUE(back);
    NTEST_EXPECT_TRUE(lowest < -39.0);
    NTEST_EXPECT_NEAR_F64(position(f, player).x, 5.0, 1e-3);
}

NTEST_F(StarterFixture, a_level_without_a_playerstart_stands_it_at_the_origin)
{
    ground(f);
    play(f);
    const NphEntity player = the_player(f);
    NTEST_ASSERT_FALSE(nph_entity_is_null(player));
    const NphDVec3 at = position(f, player);
    NTEST_EXPECT_NEAR_F64(at.x, 0.0, 1e-6);
    NTEST_EXPECT_NEAR_F64(at.z, 0.0, 1e-6);
}

NTEST_F(StarterFixture, the_levels_own_player_is_driven_and_given_what_it_lacks)
{
    ground(f);
    player_start(f, nph_dvec3(9.0, 0.0, 9.0), 0.0F); // not used: the level has a player
    NphWorld *w = level(f);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->player_id};
    NphEntity own = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &own), NPH_RESULT_OK);
    local_of(w, own)->translation = nph_dvec3(1.0, 0.0, 1.0);
    play(f);
    NTEST_EXPECT_TRUE(nph_entity_equals(the_player(f), own));
    NphWorld *pw = nph_engine_world(f->engine);
    NTEST_EXPECT_TRUE(nph_world_has(pw, own, f->ids.character_controller));
    const PlayerView v = player_view(f, own);
    NTEST_EXPECT_TRUE(nph_world_is_alive(pw, v.eye));
    key(f, NPH_KEY_W, true);
    frames(f, 120U);
    NTEST_EXPECT_NEAR_F64(position(f, own).z, 1.0 - 0.5 * (f64)WALK, 0.03);
    // The level's entity has neither: they were the play copy's.
    NTEST_EXPECT_FALSE(nph_world_has(w, own, f->ids.character_controller));
}
