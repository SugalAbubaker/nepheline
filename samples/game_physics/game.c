// SPDX-License-Identifier: Apache-2.0
// game_physics: the physics sample (CLAUDE.md §12 Phase 11; docs/runtime.md
// "Physics"), a game module run by nph_player and the editor:
//
//     nph_player --game=<build>/bin/<Config>/game_physics<.dll|.so>
//                --pack=<build>/samples/game_physics/<Config>/physics.npk
//
// When the world has nothing to draw, it builds a playground on the cooked
// ground (content/make_physics_content.py): a pyramid of crates, balls
// dropped from above, a staircase up to a landing, two ramps (one gentle
// enough to walk up, one too steep), and a platform that a kinematic body
// swings to and fro with two crates riding it. When the world has no camera
// of its own, a player walks it as a character (physics/character.h):
// W/A/S/D walk, Space jumps, the right mouse button held looks around, and
// the left one throws a ball (the newest 64 stay). It climbs the stairs and
// the gentle ramp, is stopped by the steep one, rides the platform, and
// shoves the crates it walks into. Its camera follows it on a boom that a
// sphere cast keeps out of the level. With playground.autopilot set it
// walks a course by itself, which the headless smoke run checks.
//
// The bodies are the engine's (runtime/components.h): an entity with a
// RigidBody or a Collider has one while the world plays, stepped after the
// game's systems and drawn between steps when the frame rate is not the
// step rate (sim.interpolate). The game moves them through their
// components only: the platform by its LocalTransform (a kinematic body
// goes there over each step, carrying what rides it), a ball by its
// RigidBody's velocity when it is thrown.
//
// What the game keeps lives in the world or in its state block, so a
// rebuild of this file is picked up while it runs (game.hot_reload).
#include <nepheline/physics/body.h>
#include <nepheline/physics/character.h>
#include <nepheline/runtime/components.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/game.h>
#include <nepheline/runtime/scene_render.h>

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_PLAYGROUND, "playground", NPH_LOG_LEVEL_INFO);

#define PLAYER_RADIUS      0.3F   // the capsule mesh's (content)
#define PLAYER_HALF_HEIGHT 0.6F   // half its core
#define WALK_SPEED         5.0F   // m/s
#define JUMP_SPEED         5.0F   // m/s up: about 1.3 m high
#define PLAYER_MASS        80.0F  // kg: what it shoves crates with
#define LOOK_RADIANS       0.003F // per screen unit of mouse motion
#define EYE_HEIGHT         0.6F   // the camera's pivot above the capsule's centre
#define BOOM_LENGTH        4.5F   // metres from the pivot back to the camera
#define BOOM_RADIUS        0.2F   // the room kept around the camera
#define THROW_SPEED        14.0F  // m/s
#define BALL_RADIUS        0.2F
#define BALL_DENSITY       2000.0F // kg/m^3: 67 kg, enough to knock a crate over
#define MAX_BALLS          64U
#define CRATE_DENSITY      150.0F // kg/m^3: a crate of 1 m weighs 150 kg
#define CRATE_SPACING      1.02   // metres between crates' centres in a row
#define FALL_LIMIT         (-30.0) // below this the player starts over

// The cooked content (content/make_physics_content.py).
enum {
    MESH_CUBE,    // half extents 0.5
    MESH_SPHERE,  // radius 0.5
    MESH_CAPSULE, // radius 0.3, core 1.2 along Y
    MESH_GROUND,  // the 40 x 40 m slab
    MESH_COUNT
};
enum {
    MATERIAL_GROUND,
    MATERIAL_CRATE_WARM,
    MATERIAL_CRATE_COOL,
    MATERIAL_BALL,
    MATERIAL_PLAYER,
    MATERIAL_PLATFORM,
    MATERIAL_STONE,
    MATERIAL_COUNT
};
static const char *const g_meshes[MESH_COUNT] = {"physics.gltf#mesh/0", "physics.gltf#mesh/1",
                                                 "physics.gltf#mesh/2", "physics.gltf#mesh/3"};
static const char *const g_materials[MATERIAL_COUNT] = {
    "physics.gltf#material/0", "physics.gltf#material/1", "physics.gltf#material/2",
    "physics.gltf#material/3", "physics.gltf#material/4", "physics.gltf#material/5",
    "physics.gltf#material/6"};

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

// The player: how it walks and jumps, how heavy it shoves, where it looks,
// and its camera (a child entity).
#define PLAYGROUND_PLAYER_FIELDS(F)                                                                \
    F(F32, speed, NPH_FIELD_RANGE(0.0F, 20.0F))                                                    \
    F(F32, jump_speed, NPH_FIELD_RANGE(0.0F, 20.0F))                                               \
    F(F32, mass, NPH_FIELD_RANGE(1.0F, 1000.0F))                                                   \
    F(F32, yaw, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_ANGLE))                                             \
    F(F32, pitch, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_ANGLE))                                           \
    F(ENTITY, camera, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(PlaygroundPlayer, PLAYGROUND_PLAYER_FIELDS);
NPH_COMPONENT_DECLARE(PlaygroundPlayer);
static const PlaygroundPlayer g_player_defaults = {WALK_SPEED, JUMP_SPEED, PLAYER_MASS,
                                                   0.0F,       -0.3F,      {0U}};
NPH_COMPONENT_DEFINE(PlaygroundPlayer, PLAYGROUND_PLAYER_FIELDS, "playground.Player", 0U,
                     &g_player_defaults)

// Moves its entity to and fro about `origin`, as far as `reach` either way,
// there and back every `period` seconds, `phase` of a period in at step 0.
#define PLAYGROUND_SWING_FIELDS(F)                                                                 \
    F(DVEC3, origin, NPH_FIELD_NO_HINTS)                                                           \
    F(VEC3, reach, NPH_FIELD_NO_HINTS)                                                             \
    F(F32, period, NPH_FIELD_RANGE(0.1F, 60.0F))                                                   \
    F(F32, phase, NPH_FIELD_RANGE(0.0F, 1.0F))
NPH_COMPONENT_STRUCT(PlaygroundSwing, PLAYGROUND_SWING_FIELDS);
NPH_COMPONENT_DECLARE(PlaygroundSwing);
static const PlaygroundSwing g_swing_defaults = {{0.0, 0.0, 0.0}, {1.0F, 0.0F, 0.0F}, 4.0F, 0.0F};
NPH_COMPONENT_DEFINE(PlaygroundSwing, PLAYGROUND_SWING_FIELDS, "playground.Swing", 0U,
                     &g_swing_defaults)

enum {
    COMPONENT_PLAYER,
    COMPONENT_SWING,
    COMPONENT_COUNT
};

// ---------------------------------------------------------------------------
// The autopilot
// ---------------------------------------------------------------------------

static NPH_CVAR_BOOL(g_autopilot, "playground.autopilot", false,
                     "the player walks the playground's course by itself (read when play begins)",
                     NPH_CVAR_FLAG_NONE);

// The autopilot's course, leg by leg from the player's start, each within
// LEG_STEPS: up the stairs, off the landing, a jump, a ride on the
// platform, and more balls thrown than are kept. It takes the character up
// steps, down a drop, into the air, and onto moving ground, has a kinematic
// body carry it, and makes and unmakes bodies while the world plays; at
// play_end it says whether it got round (the sample's headless smoke run
// fails when it did not: CMakeLists.txt).
enum {
    LEG_TO_STAIRS,   // to the foot of the stairs
    LEG_UP_STAIRS,   // up them, onto the landing 1.2 m up
    LEG_OFF_LANDING, // off its far edge, down to the ground
    LEG_JUMP,        // a jump where it lands: a metre up, and down again
    LEG_TO_PLATFORM, // onto the swinging platform, riding it RIDE_STEPS
    LEG_THROW,       // PILOT_THROWS balls, more than are kept
    LEG_DONE
};
#define LEG_STEPS    2400U // 10 s at 240 Hz
#define RIDE_STEPS   480U  // 2 s
#define PILOT_THROWS (MAX_BALLS + 6U)

typedef struct Autopilot {
    u32 leg;
    u32 leg_steps;  // on this leg so far
    u32 ride_steps; // in a row on the platform
    u32 throws;
    b32 failed; // ran out of steps on `leg`
    f64 top;    // the highest the player's centre was on this leg
} Autopilot;

// ---------------------------------------------------------------------------
// State (kept across hot reloads)
// ---------------------------------------------------------------------------

typedef struct PlaygroundState {
    NphActionId move_x;     // A / D
    NphActionId move_z;     // S / W
    NphActionId jump;       // Space
    NphActionId look;       // right mouse button held
    NphActionId throw_ball; // left mouse button
    // The player the game drives (the first one found while playing) and
    // its character, which only the game knows: the physics world has no
    // body for it (physics/character.h).
    NphEntity player;
    NphPhysicsCharacter character;
    NphDVec3 start; // where it began: it starts over there after a fall
    f32 up_speed;   // m/s along +Y: a jump's, then gravity's
    u32 ball_count;
    u32 ball_next; // the oldest ball, once all are out
    NphEntity balls[MAX_BALLS];
    NphEntity platform; // the playground's, which the autopilot rides
    b32 autopilot;      // playground.autopilot when play began
    Autopilot pilot;
    u64 plays;
} PlaygroundState;

static PlaygroundState *state(const NphGame *game)
{
    return (PlaygroundState *)nph_game_state(game);
}

// The player's shape and how it walks: the character's defaults (a 45
// degree slope, steps of 0.3 m, a 5 mm skin) around the capsule mesh.
static NphPhysicsCharacterDesc character_desc(void)
{
    return nph_physics_character_desc_default(
        nph_physics_geometry_capsule(PLAYER_HALF_HEIGHT, PLAYER_RADIUS));
}

// ---------------------------------------------------------------------------
// Making things
// ---------------------------------------------------------------------------

#define NO_RIGID_BODY NPH_U32_MAX

// A root entity drawn with one of the content's meshes at `t`, with a
// Collider (as it comes: a box the size of the cube mesh) and, unless
// `rigid` is NO_RIGID_BODY, a RigidBody of that NphRigidBodyType. A Collider
// alone is a static body. The caller adjusts both.
static NphResult spawn_piece(NphGame *game, const char *name, u32 mesh, u32 material,
                             const NphLocalTransform *t, u32 rigid, NphEntity *out)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    const NphComponentId comps[6] = {NPH_COMPONENT_ID_NAME,
                                     NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM,
                                     ids->mesh_renderer,
                                     ids->collider,
                                     ids->rigid_body};
    NPH_TRY(nph_world_spawn(w, comps, rigid == NO_RIGID_BODY ? 5U : 6U, out));
    nph_text_set(&NPH_WORLD_GET_MUT(w, *out, NPH_COMPONENT_ID_NAME, NphName)->text,
                 nph_str_from_cstr(name));
    *NPH_WORLD_GET_MUT(w, *out, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform) = *t;
    NphMeshRenderer *m = NPH_WORLD_GET_MUT(w, *out, ids->mesh_renderer, NphMeshRenderer);
    m->mesh = nph_guid_from_cstr(g_meshes[mesh]);
    nph_mesh_renderer_set_material(m, 0U, nph_guid_from_cstr(g_materials[material]));
    if (rigid != NO_RIGID_BODY) {
        NPH_WORLD_GET_MUT(w, *out, ids->rigid_body, NphRigidBody)->type = rigid;
    }
    return NPH_RESULT_OK;
}

static NphCollider *collider_of(NphGame *game, NphEntity e)
{
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    return NPH_WORLD_GET_MUT(nph_game_world(game), e, ids->collider, NphCollider);
}

static NphRigidBody *rigid_body_of(NphGame *game, NphEntity e)
{
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    return NPH_WORLD_GET_MUT(nph_game_world(game), e, ids->rigid_body, NphRigidBody);
}

// A crate of 1 m, centred at `at`.
static NphResult spawn_crate(NphGame *game, NphDVec3 at, bool warm, NphEntity *out)
{
    NphLocalTransform t = nph_local_transform_identity();
    t.translation = at;
    NPH_TRY(spawn_piece(game, "crate", MESH_CUBE, warm ? MATERIAL_CRATE_WARM : MATERIAL_CRATE_COOL,
                        &t, NPH_RIGID_BODY_DYNAMIC, out));
    collider_of(game, *out)->density = CRATE_DENSITY;
    return NPH_RESULT_OK;
}

// A ball of `radius`: the sphere mesh scaled, and its Collider with it.
static NphResult spawn_ball(NphGame *game, NphDVec3 at, f32 radius, f32 restitution,
                            NphEntity *out)
{
    NphLocalTransform t = nph_local_transform_identity();
    t.translation = at;
    t.scale = nph_vec3_splat(2.0F * radius);
    NPH_TRY(spawn_piece(game, "ball", MESH_SPHERE, MATERIAL_BALL, &t, NPH_RIGID_BODY_DYNAMIC, out));
    NphCollider *c = collider_of(game, *out);
    c->shape = NPH_COLLIDER_SPHERE;
    c->radius = 0.5F;
    c->density = BALL_DENSITY;
    c->friction = 0.5F;
    c->restitution = restitution;
    // Rolling stops in a few seconds on the flat (rigid balls on a rigid
    // floor would roll for ever).
    rigid_body_of(game, *out)->angular_damping = 1.0F;
    return NPH_RESULT_OK;
}

// A static block of stone: the cube mesh at `at`, turned and scaled.
static NphResult spawn_stone(NphGame *game, const char *name, NphDVec3 at, NphQuat rotation,
                             NphVec3 scale)
{
    NphLocalTransform t = nph_local_transform_identity();
    t.translation = at;
    t.rotation = rotation;
    t.scale = scale;
    NphEntity e;
    NPH_TRY(spawn_piece(game, name, MESH_CUBE, MATERIAL_STONE, &t, NO_RIGID_BODY, &e));
    collider_of(game, e)->friction = 0.8F;
    return NPH_RESULT_OK;
}

// A pyramid of crates, `base` of them along X on the bottom row and one
// fewer on each row above, standing on the ground at `at`.
static NphResult build_pyramid(NphGame *game, NphDVec3 at, u32 base)
{
    u32 n = 0U;
    for (u32 row = 0; row < base; ++row) {
        const u32 count = base - row;
        for (u32 i = 0; i < count; ++i) {
            const f64 x = at.x + ((f64)i - 0.5 * (f64)(count - 1U)) * CRATE_SPACING;
            NphEntity e;
            NPH_TRY(spawn_crate(game, nph_dvec3(x, at.y + 0.5 + (f64)row, at.z), (n++ & 1U) == 0U,
                                &e));
        }
    }
    return NPH_RESULT_OK;
}

// Steps `rise` high and `tread` deep up to a landing, climbing towards -Z
// from `at` (the foot of the first step's front), `width` wide.
static NphResult build_stairs(NphGame *game, NphDVec3 at, u32 steps, f32 rise, f32 tread,
                              f32 width, f32 landing)
{
    for (u32 k = 0; k < steps; ++k) {
        const f32 h = rise * (f32)(k + 1U);
        NPH_TRY(spawn_stone(game, "step",
                            nph_dvec3(at.x, at.y + 0.5 * (f64)h, at.z - (f64)tread * (k + 0.5)),
                            nph_quat_identity(), nph_vec3(width, h, tread)));
    }
    const f32 h = rise * (f32)steps;
    const f64 front = at.z - (f64)tread * (f64)steps;
    return spawn_stone(game, "landing",
                       nph_dvec3(at.x, at.y + 0.5 * (f64)h, front - 0.5 * (f64)landing),
                       nph_quat_identity(), nph_vec3(width, h, landing));
}

// A slab `length` long and `width` wide rising at `degrees` towards -Z, the
// low edge of its top on the ground at `at`.
static NphResult build_ramp(NphGame *game, NphDVec3 at, f32 degrees, f32 length, f32 width)
{
    const f32 a = degrees * NPH_DEG_TO_RAD;
    const f32 thick = 0.3F;
    // The top's low edge is the centre plus the turned (0, thick/2, length/2).
    const f32 up = 0.5F * length * nph_f32_sin(a) - 0.5F * thick * nph_f32_cos(a);
    const f32 back = 0.5F * length * nph_f32_cos(a) + 0.5F * thick * nph_f32_sin(a);
    return spawn_stone(game, "ramp", nph_dvec3(at.x, at.y + (f64)up, at.z - (f64)back),
                       nph_quat_from_rotation_x(a), nph_vec3(width, thick, length));
}

// A platform on the ground at `at`, swung along `reach` by a kinematic body,
// with two crates riding it.
static NphResult build_platform(NphGame *game, NphDVec3 at, NphVec3 reach, f32 period)
{
    const f32 thick = 0.2F; // under the character's step height: it steps on
    NphLocalTransform t = nph_local_transform_identity();
    t.translation = nph_dvec3(at.x, at.y + 0.5 * (f64)thick, at.z);
    t.scale = nph_vec3(3.0F, thick, 3.0F);
    NphEntity e;
    NPH_TRY(spawn_piece(game, "platform", MESH_CUBE, MATERIAL_PLATFORM, &t,
                        NPH_RIGID_BODY_KINEMATIC, &e));
    state(game)->platform = e;
    PlaygroundSwing s = g_swing_defaults;
    s.origin = t.translation;
    s.reach = reach;
    s.period = period;
    NPH_TRY(nph_world_add(nph_game_world(game), e, nph_game_component(game, COMPONENT_SWING), &s));
    for (u32 k = 0; k < 2U; ++k) {
        const f64 x = at.x + (k == 0U ? -0.6 : 0.6);
        NphEntity crate;
        NPH_TRY(spawn_crate(game, nph_dvec3(x, at.y + (f64)thick + 0.5, at.z), k == 0U, &crate));
    }
    return NPH_RESULT_OK;
}

static NphEntity find_named(NphWorld *w, const char *name)
{
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        const NphName *n = NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_NAME, NphName);
        if (n != NULL && nph_str_equals(nph_text_str(&n->text), nph_str_from_cstr(name))) {
            return e;
        }
    }
    return NPH_ENTITY_NULL;
}

// The cooked ground and sun, and everything on them.
static NphResult build_playground(NphGame *game)
{
    NphEngine *engine = nph_game_engine(game);
    NphWorld *w = nph_game_world(game);
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = w;
    d.loader = nph_engine_loader(engine);
    d.scene = nph_guid_from_cstr("physics.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    d.name = "playground";
    const NphResult r = nph_scene_import_cooked(&d, NULL, NULL);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_PLAYGROUND, "no playground to play in (%s): mount its pack with --pack",
                     nph_result_string(r));
        return NPH_RESULT_OK;
    }
    // The ground: a static body the size of its slab, whose top is y = 0.
    const NphEntity ground = find_named(w, "ground");
    if (!nph_entity_is_null(ground)) {
        NPH_TRY(nph_world_add(w, ground, nph_engine_components(engine)->collider, NULL));
        NphCollider *c = collider_of(game, ground);
        c->half_extents = nph_vec3(20.0F, 0.5F, 20.0F);
        c->center = nph_vec3(0.0F, -0.5F, 0.0F);
        c->friction = 0.8F;
    }
    // The sun casts the bodies' shadows (an imported light casts none).
    NphLight *sun = NPH_WORLD_GET_MUT(w, find_named(w, "sun"), nph_engine_components(engine)->light,
                                      NphLight);
    if (sun != NULL) {
        sun->shadows = 1;
    }
    NPH_TRY(build_pyramid(game, nph_dvec3(-6.0, 0.0, -4.0), 5U));
    // Balls dropped from above onto one another, bouncing.
    for (u32 k = 0; k < 4U; ++k) {
        NphEntity e;
        NPH_TRY(spawn_ball(game, nph_dvec3(-1.5 + 0.3 * k, 2.0 + 1.5 * k, 2.5 + 0.2 * k), 0.35F,
                           0.6F, &e));
    }
    NPH_TRY(build_stairs(game, nph_dvec3(6.0, 0.0, 6.0), 6U, 0.2F, 0.35F, 2.4F, 3.0F));
    NPH_TRY(build_ramp(game, nph_dvec3(-4.0, 0.0, 7.0), 20.0F, 6.0F, 2.0F)); // walkable
    NPH_TRY(build_ramp(game, nph_dvec3(-7.5, 0.0, 7.0), 55.0F, 4.0F, 2.0F)); // too steep
    return build_platform(game, nph_dvec3(0.0, 0.0, -10.0), nph_vec3(5.0F, 0.0F, 0.0F), 8.0F);
}

// The player's camera on its boom behind and above the player's head
// (`yaw` turns the player; the camera, its child, pitches): pulled in where
// the boom would go into the level, by a sphere cast against static and
// kinematic bodies (crates and balls only pass by).
static void place_camera(NphWorld *w, const NphPhysicsWorld *pw, NphDVec3 at,
                         const PlaygroundPlayer *p)
{
    NphLocalTransform *cam =
        NPH_WORLD_GET_MUT(w, p->camera, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    if (cam == NULL) {
        return;
    }
    const NphQuat pitch = nph_quat_from_rotation_x(p->pitch);
    const NphVec3 pivot = nph_vec3(0.0F, EYE_HEIGHT, 0.0F);
    NphVec3 boom = nph_quat_rotate(pitch, nph_vec3(0.0F, 0.0F, BOOM_LENGTH));
    if (pw != NULL) {
        NphPhysicsQueryFilter filter = nph_physics_query_filter_default();
        filter.body_types = NPH_PHYSICS_QUERY_STATIC | NPH_PHYSICS_QUERY_KINEMATIC;
        const NphPhysicsGeometry probe = nph_physics_geometry_sphere(BOOM_RADIUS);
        const NphVec3 along = nph_quat_rotate(nph_quat_from_rotation_y(p->yaw), boom);
        NphPhysicsQueryHit hit;
        if (nph_physics_world_cast_shape_closest(pw, &probe, nph_dvec3_add_vec3(at, pivot),
                                                 nph_quat_identity(), along, &filter, &hit)) {
            boom = nph_vec3_scale(boom, hit.fraction);
        }
    }
    cam->translation = nph_dvec3_from_vec3(nph_vec3_add(pivot, boom));
    cam->rotation = pitch;
}

// A player standing at `at`, with its camera.
static NphResult spawn_player(NphGame *game, NphDVec3 at)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    const NphComponentId player_id = nph_game_component(game, COMPONENT_PLAYER);
    const NphComponentId comps[5] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->mesh_renderer,
                                     player_id};
    NphEntity player;
    NPH_TRY(nph_world_spawn(w, comps, 5U, &player));
    const NphComponentId camera_comps[4] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                            NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->camera};
    NphEntity camera;
    NPH_TRY(nph_world_spawn(w, camera_comps, 4U, &camera));
    NPH_TRY(nph_world_set_parent(w, camera, player));
    nph_text_set(&NPH_WORLD_GET_MUT(w, player, NPH_COMPONENT_ID_NAME, NphName)->text,
                 NPH_STR("player"));
    nph_text_set(&NPH_WORLD_GET_MUT(w, camera, NPH_COMPONENT_ID_NAME, NphName)->text,
                 NPH_STR("camera"));
    NPH_WORLD_GET_MUT(w, player, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
        ->translation = at;
    NphMeshRenderer *m = NPH_WORLD_GET_MUT(w, player, ids->mesh_renderer, NphMeshRenderer);
    m->mesh = nph_guid_from_cstr(g_meshes[MESH_CAPSULE]);
    nph_mesh_renderer_set_material(m, 0U, nph_guid_from_cstr(g_materials[MATERIAL_PLAYER]));
    PlaygroundPlayer *p = (PlaygroundPlayer *)nph_world_get_mut(w, player, player_id);
    p->camera = camera;
    place_camera(w, NULL, at, p); // the boom unchecked until the first step
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

// Swings move their entities' LocalTransforms to where they are at the end
// of this step; a kinematic body goes there over the step.
static void swing(NphSystemContext *ctx)
{
    const NphGame *game = (const NphGame *)ctx->user;
    const f64 tau = 6.283185307179586;
    const f64 t = (f64)(nph_game_step(game) + 1U) * nph_game_step_seconds(game);
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        const PlaygroundSwing *s = NPH_CHUNK_COLUMN(&v, 0, PlaygroundSwing);
        NphLocalTransform *lt = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        for (u32 i = 0; i < v.count; ++i) {
            const f64 turns = t / nph_f64_max((f64)s[i].period, 0.1) + (f64)s[i].phase;
            const f64 k = nph_f64_sin(tau * (turns - nph_f64_floor(turns)));
            lt[i].translation =
                nph_dvec3_add(s[i].origin, nph_dvec3_scale(nph_dvec3_from_vec3(s[i].reach), k));
        }
    }
}

// What the player is told to do in a step: by the keys and the mouse, or by
// the autopilot.
typedef struct PlayerInput {
    f32 right;   // -1 .. 1
    f32 forward; // -1 .. 1
    f32 turn;    // radians onto the yaw
    f32 tilt;    // radians onto the pitch
    bool jump;
    bool throw_ball;
} PlayerInput;

static PlayerInput read_input(const PlaygroundState *st, const NphSimInput *in)
{
    PlayerInput pi;
    memset(&pi, 0, sizeof pi);
    pi.right = nph_sim_input_value(in, st->move_x);
    pi.forward = nph_sim_input_value(in, st->move_z);
    if (nph_sim_input_down(in, st->look)) {
        pi.turn = -(f32)in->mouse_dx * LOOK_RADIANS;
        pi.tilt = -(f32)in->mouse_dy * LOOK_RADIANS;
    }
    pi.jump = nph_sim_input_pressed(in, st->jump);
    pi.throw_ball = nph_sim_input_pressed(in, st->throw_ball);
    return pi;
}

// Faces (x, z) and walks there; true once `within` of it.
static bool steer(const NphPhysicsCharacter *c, const PlaygroundPlayer *p, f64 x, f64 z, f64 within,
                  PlayerInput *out)
{
    const f64 dx = x - c->position.x;
    const f64 dz = z - c->position.z;
    if (dx * dx + dz * dz < within * within) {
        return true;
    }
    // Forward is (-sin yaw, 0, -cos yaw).
    out->turn = nph_f32_wrap_angle((f32)nph_f64_atan2(-dx, -dz) - p->yaw);
    out->forward = 1.0F;
    return false;
}

// The autopilot's input for this step, and its progress round the course.
static PlayerInput pilot_input(PlaygroundState *st, const NphWorld *w, const NphPhysicsWorld *pw,
                               const PlaygroundPlayer *p)
{
    Autopilot *a = &st->pilot;
    const NphPhysicsCharacter *c = &st->character;
    PlayerInput pi;
    memset(&pi, 0, sizeof pi);
    if (a->leg == LEG_DONE || a->failed) {
        return pi; // idle
    }
    a->top = nph_f64_max(a->top, c->position.y);
    const bool standing = c->ground == NPH_PHYSICS_CHARACTER_WALKABLE;
    bool done = false;
    switch (a->leg) {
    case LEG_TO_STAIRS:
        done = steer(c, p, 6.0, 7.5, 0.3, &pi);
        break;
    case LEG_UP_STAIRS:
        done = steer(c, p, 6.0, 2.4, 0.3, &pi) && standing && c->position.y > 2.0;
        break;
    case LEG_OFF_LANDING:
        done = steer(c, p, 6.0, -1.5, 0.3, &pi) && standing && c->position.y < 1.0;
        break;
    case LEG_JUMP:
        pi.jump = a->leg_steps == 0U;
        done = a->leg_steps > 0U && standing && a->top > c->position.y + 1.0;
        break;
    case LEG_TO_PLATFORM: {
        // The strip along its near edge, clear of the crates it carries.
        const NphLocalTransform *t =
            NPH_WORLD_GET(w, st->platform, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
        if (t != NULL) {
            (void)steer(c, p, t->translation.x, t->translation.z + 1.0, 0.2, &pi);
        }
        const bool riding = standing && !nph_physics_body_is_null(c->ground_body) &&
                            nph_physics_body_type(pw, c->ground_body) == NPH_PHYSICS_BODY_KINEMATIC;
        a->ride_steps = riding ? a->ride_steps + 1U : 0U;
        done = a->ride_steps >= RIDE_STEPS;
        break;
    }
    case LEG_THROW:
        pi.throw_ball = (a->leg_steps & 1U) == 0U;
        a->throws += pi.throw_ball ? 1U : 0U;
        done = a->throws >= PILOT_THROWS;
        break;
    default:
        break;
    }
    a->leg_steps += 1U;
    if (done) {
        NPH_LOG_INFO(&LOG_PLAYGROUND, "autopilot: leg %u in %u steps, at (%.2f, %.2f, %.2f)", a->leg,
                     a->leg_steps, c->position.x, c->position.y, c->position.z);
        a->leg += 1U;
        a->leg_steps = 0U;
        a->top = c->position.y;
    } else if (a->leg_steps > LEG_STEPS) {
        a->failed = 1; // stopped where it was
    }
    return pi;
}

// One step of the player: it looks, walks or falls as its character, shoves
// what it walked into, and puts its camera where it can see.
static void move_player(NphGame *game, NphWorld *w, NphPhysicsWorld *pw, const PlayerInput *cmd,
                        f32 dt, PlaygroundPlayer *p, NphLocalTransform *t)
{
    PlaygroundState *st = state(game);
    NphPhysicsCharacter *c = &st->character;
    const NphPhysicsCharacterDesc desc = character_desc();
    p->yaw = nph_f32_wrap_angle(p->yaw + cmd->turn);
    p->pitch = nph_f32_clamp(p->pitch + cmd->tilt, -1.2F, 0.5F);
    // Along the ground the camera faces (cameras look down -Z).
    const NphQuat yaw = nph_quat_from_rotation_y(p->yaw);
    const NphVec3 forward = nph_quat_rotate(yaw, nph_vec3(0.0F, 0.0F, -1.0F));
    const NphVec3 right = nph_quat_rotate(yaw, nph_vec3(1.0F, 0.0F, 0.0F));
    NphVec3 walk = nph_vec3_add(nph_vec3_scale(right, cmd->right),
                                nph_vec3_scale(forward, cmd->forward));
    const f32 pace = nph_vec3_length(walk);
    if (pace > 1.0F) {
        walk = nph_vec3_scale(walk, 1.0F / pace); // no faster on a diagonal
    }
    NphVec3 velocity = nph_vec3_scale(walk, p->speed);
    if (c->ground == NPH_PHYSICS_CHARACTER_WALKABLE) {
        // On its feet: it rides what it stands on, and may jump.
        velocity = nph_vec3_add(velocity, c->ground_velocity);
        st->up_speed = cmd->jump ? p->jump_speed : 0.0F;
    } else {
        st->up_speed += nph_physics_world_gravity(pw).y * dt; // falling, or sliding down
    }
    velocity.y += st->up_speed;
    NphPhysicsCharacterHit hits[8];
    u32 hit_count = 0U;
    if (nph_physics_character_move(pw, &desc, c, nph_vec3_scale(velocity, dt), hits, 8U,
                                   &hit_count) == NPH_RESULT_OK) {
        nph_physics_character_push(pw, hits, hit_count, p->mass, velocity);
        for (u32 i = 0; i < hit_count; ++i) {
            if (hits[i].normal.y < -0.5F && st->up_speed > 0.0F) {
                st->up_speed = 0.0F; // a head against a ceiling
            }
        }
    }
    if (c->position.y < FALL_LIMIT) {
        // Off the edge of the world: back to the start, drawn there at once.
        NPH_IGNORE_RESULT(
            nph_physics_character_init(pw, &desc, st->start, nph_quat_identity(), c));
        st->up_speed = 0.0F;
        nph_game_reset_interpolation(game, st->player);
    }
    t->translation = c->position;
    t->rotation = yaw;
    place_camera(w, pw, c->position, p);
}

// A ball out of the player's chest along its camera's view; the oldest
// goes when MAX_BALLS are out.
static NphResult throw_ball(NphGame *game, NphWorld *w, const PlaygroundPlayer *p)
{
    PlaygroundState *st = state(game);
    const NphQuat aim =
        nph_quat_mul(nph_quat_from_rotation_y(p->yaw), nph_quat_from_rotation_x(p->pitch));
    const NphVec3 forward = nph_quat_rotate(aim, nph_vec3(0.0F, 0.0F, -1.0F));
    const NphVec3 out = nph_vec3_add(nph_vec3(0.0F, 0.4F, 0.0F),
                                     nph_vec3_scale(forward, PLAYER_RADIUS + BALL_RADIUS + 0.1F));
    const u32 slot = st->ball_next;
    if (st->ball_count == MAX_BALLS && nph_world_is_alive(w, st->balls[slot])) {
        NPH_TRY(nph_world_despawn(w, st->balls[slot]));
    }
    NphEntity ball;
    NPH_TRY(spawn_ball(game, nph_dvec3_add_vec3(st->character.position, out), BALL_RADIUS, 0.3F,
                       &ball));
    rigid_body_of(game, ball)->linear_velocity = nph_vec3_scale(forward, THROW_SPEED);
    st->balls[slot] = ball;
    st->ball_next = (slot + 1U) % MAX_BALLS;
    st->ball_count += st->ball_count < MAX_BALLS ? 1U : 0U;
    return NPH_RESULT_OK;
}

static void drive(NphSystemContext *ctx)
{
    NphGame *game = (NphGame *)ctx->user;
    PlaygroundState *st = state(game);
    NphPhysicsWorld *pw = nph_game_physics(game);
    if (pw == NULL) {
        return;
    }
    const NphSimInput *in = nph_game_input(game);
    PlaygroundPlayer thrower = g_player_defaults;
    bool throwing = false;
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        PlaygroundPlayer *p = NPH_CHUNK_COLUMN(&v, 0, PlaygroundPlayer);
        NphLocalTransform *t = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        for (u32 i = 0; i < v.count; ++i) {
            if (!nph_entity_equals(v.entities[i], st->player)) {
                if (nph_world_is_alive(ctx->world, st->player)) {
                    continue; // the game drives one player
                }
                // The first one found: its character starts where it is.
                const NphPhysicsCharacterDesc desc = character_desc();
                if (nph_physics_character_init(pw, &desc, t[i].translation, nph_quat_identity(),
                                               &st->character) != NPH_RESULT_OK) {
                    continue;
                }
                st->player = v.entities[i];
                st->start = t[i].translation;
                st->up_speed = 0.0F;
            }
            const PlayerInput cmd =
                st->autopilot ? pilot_input(st, ctx->world, pw, &p[i]) : read_input(st, in);
            move_player(game, ctx->world, pw, &cmd, (f32)ctx->dt, &p[i], &t[i]);
            if (cmd.throw_ball) {
                thrower = p[i];
                throwing = true;
            }
        }
    }
    // After the iteration: spawning moves chunks.
    if (throwing) {
        const NphResult r = throw_ball(game, ctx->world, &thrower);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&LOG_PLAYGROUND, "no ball thrown: %s", nph_result_string(r));
        }
    }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static NphResult bind_actions(NphGame *game)
{
    PlaygroundState *s = state(game);
    NphInput *input = nph_engine_input(nph_game_engine(game));
    NPH_TRY(nph_game_action(game, "playground.move_x", &s->move_x));
    NPH_TRY(nph_game_action(game, "playground.move_z", &s->move_z));
    NPH_TRY(nph_game_action(game, "playground.jump", &s->jump));
    NPH_TRY(nph_game_action(game, "playground.look", &s->look));
    NPH_TRY(nph_game_action(game, "playground.throw", &s->throw_ball));
    // A reload binds again: the old bindings go first.
    const NphActionId all[5] = {s->move_x, s->move_z, s->jump, s->look, s->throw_ball};
    for (u32 i = 0; i < 5U; ++i) {
        nph_input_unbind_all(input, all[i]);
    }
    NPH_TRY(nph_input_bind_key_axis(input, s->move_x, NPH_KEY_A, NPH_KEY_D));
    NPH_TRY(nph_input_bind_key_axis(input, s->move_z, NPH_KEY_S, NPH_KEY_W));
    NPH_TRY(nph_input_bind_key(input, s->jump, NPH_KEY_SPACE, NPH_MOD_ANY));
    NPH_TRY(nph_input_bind_mouse_button(input, s->look, NPH_MOUSE_BUTTON_RIGHT, NPH_MOD_ANY));
    return nph_input_bind_mouse_button(input, s->throw_ball, NPH_MOUSE_BUTTON_LEFT, NPH_MOD_ANY);
}

static NphResult load(NphGame *game, const NphGameLoadInfo *info)
{
    NPH_LOG_INFO(&LOG_PLAYGROUND, "playground %s%s", info->reloaded ? "reloaded" : "loaded",
                 info->world_migrated ? " (components changed)" : "");
    NPH_TRY(nph_game_register_cvar(game, &g_autopilot));
    return bind_actions(game);
}

static NphResult add_systems(NphGame *game, NphSchedule *schedule)
{
    const NphComponentId swing_id = nph_game_component(game, COMPONENT_SWING);
    const NphComponentId player_id = nph_game_component(game, COMPONENT_PLAYER);
    // Both move what this step's physics starts from.
    const char *const before[1] = {NPH_PHYSICS_SYSTEM_NAME};
    const NphQueryTerm swing_terms[2] = {nph_term_read(swing_id),
                                         nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "playground.swing";
    d.run = swing;
    d.user = game;
    d.terms = swing_terms;
    d.term_count = 2U;
    d.before = before;
    d.before_count = 1U;
    NPH_TRY(nph_schedule_add_system(schedule, &d, NULL));
    // The player's character queries the physics world and pushes its
    // bodies, and its throws spawn balls: it runs alone, on the schedule's
    // thread, free to change the world's structure (EXCLUSIVE).
    const NphQueryTerm player_terms[2] = {nph_term_write(player_id),
                                          nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    d.name = "playground.player";
    d.run = drive;
    d.terms = player_terms;
    d.flags = (u32)NPH_SYSTEM_EXCLUSIVE;
    return nph_schedule_add_system(schedule, &d, NULL);
}

static bool world_draws_something(NphWorld *w, NphComponentId mesh_renderer)
{
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        if (nph_world_has(w, nph_world_entity_at(w, i), mesh_renderer)) {
            return true;
        }
    }
    return false;
}

// The playground, when the world brings nothing to draw, and a player,
// when it brings neither a player nor a camera.
static NphResult play_begin(NphGame *game)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    PlaygroundState *st = state(game);
    st->plays += 1U;
    st->player = NPH_ENTITY_NULL; // found at the first step
    st->ball_count = 0U;
    st->ball_next = 0U;
    st->platform = NPH_ENTITY_NULL;
    st->autopilot = nph_cvar_get_bool(&g_autopilot);
    memset(&st->pilot, 0, sizeof st->pilot);
    if (!world_draws_something(w, ids->mesh_renderer)) {
        NPH_TRY(build_playground(game));
    }
    const NphComponentId player_id = nph_game_component(game, COMPONENT_PLAYER);
    bool watched = false;
    for (u32 i = 0; i < nph_world_entity_slot_count(w) && !watched; ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        watched = nph_world_has(w, e, ids->camera) || nph_world_has(w, e, player_id);
    }
    if (!watched) {
        NPH_TRY(spawn_player(
            game, nph_dvec3(0.0, (f64)(PLAYER_HALF_HEIGHT + PLAYER_RADIUS) + 0.01, 8.0)));
    }
    return NPH_RESULT_OK;
}

static void play_end(NphGame *game)
{
    const PlaygroundState *st = state(game);
    const NphPhysicsStats *s = nph_engine_physics_stats(nph_game_engine(game));
    NPH_LOG_INFO(&LOG_PLAYGROUND, "played %llu steps: %u bodies, %u awake, %u balls out",
                 (unsigned long long)nph_game_step(game), s->bodies, s->awake_bodies,
                 st->ball_count);
    if (!st->autopilot) {
        return;
    }
    // The verdict: round the course, and the balls kept are the newest.
    const Autopilot *a = &st->pilot;
    const NphWorld *w = nph_game_world(game);
    u32 kept = 0U;
    for (u32 i = 0; i < st->ball_count; ++i) {
        kept += nph_world_is_alive(w, st->balls[i]) ? 1U : 0U;
    }
    const NphDVec3 at = st->character.position;
    if (a->leg == LEG_DONE && !a->failed && kept == MAX_BALLS) {
        NPH_LOG_INFO(&LOG_PLAYGROUND,
                     "autopilot: the course is done (up the stairs, off the landing, a jump, a "
                     "ride on the platform; %u balls kept of %u thrown)",
                     kept, a->throws);
    } else {
        NPH_LOG_ERROR(&LOG_PLAYGROUND,
                      "autopilot failed: %s leg %u at (%.2f, %.2f, %.2f), %u balls kept of %u",
                      a->failed ? "out of time on" : "still on", a->leg, at.x, at.y, at.z, kept,
                      a->throws);
    }
}

NPH_GAME_EXPORT const NphGameApi *nph_game_entry(void)
{
    static const NphComponentDesc *components[COMPONENT_COUNT];
    components[COMPONENT_PLAYER] = PlaygroundPlayer_component();
    components[COMPONENT_SWING] = PlaygroundSwing_component();
    static NphGameApi api;
    api.abi_version = NPH_GAME_ABI_VERSION;
    api.state_version = 1U;
    api.state_size = sizeof(PlaygroundState);
    api.state_align = NPH_ALIGNOF(PlaygroundState);
    api.name = "playground";
    api.components = components;
    api.component_count = COMPONENT_COUNT;
    api.load = load;
    api.add_systems = add_systems;
    api.play_begin = play_begin;
    api.play_end = play_end;
    return &api;
}
