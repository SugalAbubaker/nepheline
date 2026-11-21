// SPDX-License-Identifier: Apache-2.0
// game_starter: the first-person starter game (ADR-0108 decision 9;
// docs/runtime.md "The starter game"). The editor plays it when the project
// names no game of its own, so any level can be walked; a game may start
// from it. nph_player runs it too:
//
//     nph_player --game=<build>/bin/<Config>/game_starter<.dll|.so> --scene=level.nscene
//
// Play stands a player in the level: at its first PlayerStart
// (runtime/components.h), else under the view the host set (the editor's
// camera: the player's eye where it was; engine.h nph_engine_host_view),
// else at the origin. A level may place
// its own instead: the first entity with a starter.Player, which is given a
// CharacterController and an eye when it has none.
//   - The player is a CharacterController (ADR-0108 decision 8) with an
//     eye: a child at 0.94 of its height (where a person's eyes are) with
//     the Camera and the AudioListener. The level's first active Camera
//     lends the eye its exposure and tonemapper, and is put out with the
//     level's other cameras and listeners (in the play copy).
//   - The mouse turns the body about +Y (its LocalTransform's rotation; the
//     controller keeps it upright) and tilts the eye; the game asks for the
//     mouse captured (game.h nph_game_capture_mouse).
//   - W A S D walk and Shift runs, along the ground the body faces; Space
//     jumps (the controller takes it when it stands).
//   - E pushes what the eye's ray meets within reach, from the point it
//     meets it: a crate slides, a door on a hinge swings. The push is an
//     impulse of `push`, but no more than gives the body `push_speed`, so a
//     can is not thrown across the room. What it would push is outlined.
//   - Props are pushed by walking into them (the controller's push_mass).
//   - A fall of FALL_DEPTH below where it started stands it there again.
//
// What the game keeps lives in the world or in its state block, so a
// rebuild of this file is picked up while it plays (game.hot_reload).
#include <nepheline/physics/body.h>
#include <nepheline/physics/query.h>
#include <nepheline/physics/shape.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/runtime/components.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/game.h>

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_STARTER, "starter", NPH_LOG_LEVEL_INFO);

#define EYE_FRACTION 0.94F   // of the controller's height: a person's eyes
#define EYE_YFOV     1.2F    // radians: 69 degrees up the screen, 103 across at 16:9
#define EYE_ZNEAR    0.05F   // metres: close enough to stand at a wall
#define PITCH_LIMIT  1.5533F // 89 degrees: never straight up or down
#define FALL_DEPTH   50.0    // metres below the start: stood there again

// What E would push, outlined over the scene (RGBA as the renderer's).
#define COLOR_TARGET NPH_RENDER_RGBA(255, 210, 90, 255)

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

// The player: how it walks, runs, jumps and looks, where it looks (`yaw`
// turns the body, `pitch` the eye), how far and how hard E pushes, and its
// eye (a child with the Camera).
#define STARTER_PLAYER_FIELDS(F)                                                                   \
    F(F32, walk_speed, NPH_FIELD_RANGE(0.0F, 20.0F))                                               \
    F(F32, run_speed, NPH_FIELD_RANGE(0.0F, 40.0F))                                                \
    F(F32, jump_speed, NPH_FIELD_RANGE(0.0F, 20.0F))                                               \
    F(F32, look_speed, NPH_FIELD_RANGE(0.0F, 0.05F))                                               \
    F(F32, yaw, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_ANGLE))                                             \
    F(F32, pitch, NPH_FIELD_HINTS(NPH_FIELD_FLAG_ANGLE | NPH_FIELD_FLAG_RANGE, -1.5533F, 1.5533F)) \
    F(F32, reach, NPH_FIELD_RANGE(0.0F, 20.0F))                                                    \
    F(F32, push, NPH_FIELD_RANGE(0.0F, 1000.0F))                                                   \
    F(F32, push_speed, NPH_FIELD_RANGE(0.0F, 20.0F))                                               \
    F(ENTITY, eye, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(StarterPlayer, STARTER_PLAYER_FIELDS);
NPH_COMPONENT_DECLARE(StarterPlayer);
// 2.5 m/s walking, 6 running; a jump of 4 m/s, about 0.8 m high; a radian
// for 400 mouse units; E reaching 2.5 m with 40 N s, at most 2.5 m/s (a 20 kg crate slides a
// third of a metre on a floor, a 30 kg door swings at 3 rad/s).
static const StarterPlayer g_player_defaults = {2.5F, 6.0F, 4.0F,  0.0025F, 0.0F,
                                                0.0F, 2.5F, 40.0F, 2.5F,    {0U}};
NPH_COMPONENT_DEFINE(StarterPlayer, STARTER_PLAYER_FIELDS, "starter.Player", 0U, &g_player_defaults)

enum {
    COMPONENT_PLAYER,
    COMPONENT_COUNT
};

// ---------------------------------------------------------------------------
// State (kept across hot reloads)
// ---------------------------------------------------------------------------

typedef struct StarterState {
    NphActionId move_x;   // A / D
    NphActionId move_z;   // S / W
    NphActionId run;      // Shift held
    NphActionId jump;     // Space
    NphActionId interact; // E
    NphEntity player;     // the one the game drives while playing
    NphDVec3 start;       // where it stood when play began: a fall stands it there again
    f32 start_yaw;
    u32 pushes; // E's that met a body to push, since play began
    u64 plays;
} StarterState;

static StarterState *state(const NphGame *game)
{
    return (StarterState *)nph_game_state(game);
}

static StarterPlayer *player_of(const NphGame *game, NphWorld *w, NphEntity e)
{
    return (StarterPlayer *)nph_world_get_mut(w, e, nph_game_component(game, COMPONENT_PLAYER));
}

// The eye's height above the feet: EYE_FRACTION of the controller's.
static f32 eye_height(const NphCharacterController *cc)
{
    const f32 h = cc != NULL && nph_f32_is_finite(cc->height) ? cc->height : 1.8F;
    return EYE_FRACTION * nph_f32_max(h, 0.0F);
}

// Where the eye looks: along -Z of the body's yaw and the eye's pitch.
static NphVec3 look_direction(f32 yaw, f32 pitch)
{
    const NphQuat aim =
        nph_quat_mul(nph_quat_from_rotation_y(yaw), nph_quat_from_rotation_x(pitch));
    return nph_quat_rotate(aim, nph_vec3(0.0F, 0.0F, -1.0F));
}

// The yaw and pitch of a direction (cameras look down -Z).
static void angles_of(NphVec3 forward, f32 *out_yaw, f32 *out_pitch)
{
    const NphVec3 f = nph_vec3_normalize_or(forward, nph_vec3(0.0F, 0.0F, -1.0F));
    *out_yaw = nph_f32_atan2(-f.x, -f.z);
    *out_pitch =
        nph_f32_clamp(nph_f32_asin(nph_f32_clamp(f.y, -1.0F, 1.0F)), -PITCH_LIMIT, PITCH_LIMIT);
}

// The query filter of the eye's ray: everything but the player's own body
// (its ghost capsule, which rays see).
static NphPhysicsQueryFilter eye_filter(const NphGame *game, NphEntity player)
{
    NphPhysicsQueryFilter filter = nph_physics_query_filter_default();
    filter.ignore = nph_game_physics_body(game, player);
    return filter;
}

// What the eye's ray from `eye` along `dir` meets within `reach`, when it is
// a body E pushes (a dynamic one).
static bool push_target(const NphPhysicsWorld *pw, const NphPhysicsQueryFilter *filter,
                        NphDVec3 eye, NphVec3 dir, f32 reach, NphPhysicsQueryHit *out)
{
    if (!(reach > 0.0F)) {
        return false;
    }
    return nph_physics_world_raycast_closest(pw, eye, nph_vec3_scale(dir, reach), filter, out) &&
           nph_physics_body_type(pw, out->body) == NPH_PHYSICS_BODY_DYNAMIC;
}

// ---------------------------------------------------------------------------
// The player's step
// ---------------------------------------------------------------------------

// Looks, pushes with E, falls back to the start, and asks the controller to
// walk, run, and jump: before the physics system, which moves the controller.
static void drive(NphSystemContext *ctx)
{
    NphGame *game = (NphGame *)ctx->user;
    StarterState *st = state(game);
    NphWorld *w = ctx->world;
    NphPhysicsWorld *pw = nph_game_physics(game);
    const NphSimInput *in = nph_game_input(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    if (pw == NULL || !nph_world_is_alive(w, st->player)) {
        return;
    }
    StarterPlayer *p = player_of(game, w, st->player);
    NphCharacterController *cc =
        NPH_WORLD_GET_MUT(w, st->player, ids->character_controller, NphCharacterController);
    NphLocalTransform *t =
        NPH_WORLD_GET_MUT(w, st->player, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    if (p == NULL || cc == NULL || t == NULL) {
        return;
    }
    // Look: the body turns, the eye tilts.
    p->yaw = nph_f32_wrap_angle(p->yaw - (f32)in->mouse_dx * p->look_speed);
    p->pitch =
        nph_f32_clamp(p->pitch - (f32)in->mouse_dy * p->look_speed, -PITCH_LIMIT, PITCH_LIMIT);
    const NphQuat yaw = nph_quat_from_rotation_y(p->yaw);
    t->rotation = yaw;
    const f32 eye_h = eye_height(cc);
    NphLocalTransform *eye =
        NPH_WORLD_GET_MUT(w, p->eye, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    if (eye != NULL) {
        eye->translation = nph_dvec3(0.0, (f64)eye_h, 0.0);
        eye->rotation = nph_quat_from_rotation_x(p->pitch);
    }
    // E: an impulse where the eye's ray meets a body, along the ray.
    if (nph_sim_input_pressed(in, st->interact)) {
        const NphDVec3 at = nph_dvec3_add_vec3(t->translation, nph_vec3(0.0F, eye_h, 0.0F));
        const NphVec3 dir = look_direction(p->yaw, p->pitch);
        const NphPhysicsQueryFilter filter = eye_filter(game, st->player);
        NphPhysicsQueryHit hit;
        if (push_target(pw, &filter, at, dir, p->reach, &hit)) {
            const f32 mass = nph_physics_body_mass_data(pw, hit.body).mass;
            const f32 impulse =
                nph_f32_min(nph_f32_max(p->push, 0.0F), mass * nph_f32_max(p->push_speed, 0.0F));
            nph_physics_body_set_awake(pw, hit.body, true);
            nph_physics_body_apply_linear_impulse(pw, hit.body, nph_vec3_scale(dir, impulse),
                                                  hit.point);
            st->pushes += 1U;
        }
    }
    // Off the bottom of the level: back where it started, drawn there at once
    // (the controller takes a written translation as a respawn, at rest).
    if (t->translation.y < st->start.y - FALL_DEPTH) {
        t->translation = st->start;
        p->yaw = st->start_yaw;
        t->rotation = nph_quat_from_rotation_y(p->yaw);
        nph_game_reset_interpolation(game, st->player);
        NPH_LOG_INFO(&LOG_STARTER, "fell out of the level: back at the start");
    }
    // Walk or run along the ground the body faces.
    const NphVec3 forward = nph_quat_rotate(yaw, nph_vec3(0.0F, 0.0F, -1.0F));
    const NphVec3 right = nph_quat_rotate(yaw, nph_vec3(1.0F, 0.0F, 0.0F));
    NphVec3 wish = nph_vec3_add(nph_vec3_scale(right, nph_sim_input_value(in, st->move_x)),
                                nph_vec3_scale(forward, nph_sim_input_value(in, st->move_z)));
    const f32 pace = nph_vec3_length(wish);
    if (pace > 1.0F) {
        wish = nph_vec3_scale(wish, 1.0F / pace); // no faster on a diagonal
    }
    const f32 speed = nph_sim_input_down(in, st->run) ? p->run_speed : p->walk_speed;
    cc->move = nph_vec3_scale(wish, nph_f32_max(speed, 0.0F));
    if (nph_sim_input_pressed(in, st->jump)) {
        cc->jump = nph_f32_max(p->jump_speed, 0.0F);
    }
}

// ---------------------------------------------------------------------------
// Play
// ---------------------------------------------------------------------------

typedef struct Spawn {
    NphDVec3 feet;
    f32 yaw;
    f32 pitch;
    const char *where; // for the log
} Spawn;

// Where the player stands: the first PlayerStart, else under the view the
// host set (the editor's camera: its eye where the camera was), else the
// origin.
static Spawn find_spawn(NphGame *game, f32 eye_h)
{
    NphEngine *engine = nph_game_engine(game);
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(engine);
    Spawn s;
    s.feet = nph_dvec3(0.0, 0.0, 0.0);
    s.yaw = 0.0F;
    s.pitch = 0.0F;
    s.where = "the origin";
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        const NphWorldTransform *wt =
            NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
        if (wt != NULL && nph_world_has(w, e, ids->player_start)) {
            s.feet = wt->translation;
            f32 unused = 0.0F;
            angles_of(nph_vec3_neg(wt->z_axis), &s.yaw, &unused);
            s.where = "the PlayerStart";
            return s;
        }
    }
    const NphRenderView *view = nph_engine_host_view(engine);
    if (view != NULL) {
        const NphQuat o = nph_quat_normalize_or_identity(view->camera.orientation);
        s.feet = nph_dvec3_add_vec3(view->camera.position, nph_vec3(0.0F, -eye_h, 0.0F));
        angles_of(nph_quat_rotate(o, nph_vec3(0.0F, 0.0F, -1.0F)), &s.yaw, &s.pitch);
        s.where = "the host's view";
    }
    return s;
}

static void name(NphWorld *w, NphEntity e, const char *text)
{
    NphName *n = NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_NAME, NphName);
    if (n != NULL) {
        NPH_IGNORE_RESULT(nph_text_set(&n->text, nph_str_from_cstr(text)));
    }
}

// The eye under `player`: the Camera (the level's first active one lending
// its exposure and tonemapper, every other one put out) and the
// AudioListener (the level's put out).
static NphResult make_eye(NphGame *game, NphEntity player, NphEntity *out)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    NphCamera lens;
    nph_component_init_default(NphCamera_component(), &lens);
    bool lent = false;
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        NphCamera *c = NPH_WORLD_GET_MUT(w, e, ids->camera, NphCamera);
        if (c != NULL && c->active != 0) {
            if (!lent) {
                lens.ev100 = c->ev100;
                lens.tonemapper = c->tonemapper;
                lent = true;
            }
            c->active = 0;
        }
        NphAudioListener *l = NPH_WORLD_GET_MUT(w, e, ids->audio_listener, NphAudioListener);
        if (l != NULL) {
            l->active = 0;
        }
    }
    lens.yfov = EYE_YFOV;
    lens.znear = EYE_ZNEAR;
    lens.active = 1;
    const NphComponentId comps[5] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->camera,
                                     ids->audio_listener};
    NPH_TRY(nph_world_spawn(w, comps, 5U, out));
    name(w, *out, "Eye");
    *NPH_WORLD_GET_MUT(w, *out, ids->camera, NphCamera) = lens;
    NPH_WORLD_GET_MUT(w, *out, ids->audio_listener, NphAudioListener)->active = 1;
    return nph_world_set_parent(w, *out, player);
}

// The level's own player (the first entity with a starter.Player), given a
// controller and an eye when it has none; else a new one where find_spawn
// says.
static NphResult stand_player(NphGame *game)
{
    StarterState *st = state(game);
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    const NphComponentId player_id = nph_game_component(game, COMPONENT_PLAYER);
    NphEntity player = NPH_ENTITY_NULL;
    for (u32 i = 0; i < nph_world_entity_slot_count(w) && nph_entity_is_null(player); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        if (nph_world_has(w, e, player_id) &&
            nph_world_has(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM)) {
            player = e;
        }
    }
    const char *where = "the level's player";
    if (nph_entity_is_null(player)) {
        NphCharacterController cc;
        nph_component_init_default(NphCharacterController_component(), &cc);
        const Spawn s = find_spawn(game, eye_height(&cc));
        const NphComponentId comps[5] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                         NPH_COMPONENT_ID_WORLD_TRANSFORM,
                                         ids->character_controller, player_id};
        NPH_TRY(nph_world_spawn(w, comps, 5U, &player));
        name(w, player, "Player");
        NphLocalTransform *t =
            NPH_WORLD_GET_MUT(w, player, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
        t->translation = s.feet;
        StarterPlayer *p = player_of(game, w, player);
        p->yaw = s.yaw;
        p->pitch = s.pitch;
        where = s.where;
    } else if (!nph_world_has(w, player, ids->character_controller)) {
        NPH_TRY(nph_world_add(w, player, ids->character_controller, NULL));
    }
    StarterPlayer *p = player_of(game, w, player);
    if (!nph_world_is_alive(w, p->eye)) {
        NphEntity eye = NPH_ENTITY_NULL;
        NPH_TRY(make_eye(game, player, &eye));
        player_of(game, w, player)->eye = eye; // spawning moved the player's chunk
    }
    p = player_of(game, w, player);
    NphLocalTransform *t =
        NPH_WORLD_GET_MUT(w, player, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    t->rotation = nph_quat_from_rotation_y(p->yaw);
    const NphCharacterController *cc =
        NPH_WORLD_GET(w, player, ids->character_controller, NphCharacterController);
    NphLocalTransform *eye =
        NPH_WORLD_GET_MUT(w, p->eye, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    if (eye != NULL) {
        eye->translation = nph_dvec3(0.0, (f64)eye_height(cc), 0.0);
        eye->rotation = nph_quat_from_rotation_x(p->pitch);
    }
    st->player = player;
    st->start = t->translation;
    st->start_yaw = p->yaw;
    st->pushes = 0U;
    NPH_LOG_INFO(&LOG_STARTER, "the player stands at %s (%.2f, %.2f, %.2f)", where,
                 t->translation.x, t->translation.y, t->translation.z);
    return NPH_RESULT_OK;
}

static NphResult play_begin(NphGame *game)
{
    state(game)->plays += 1U;
    NPH_TRY(stand_player(game));
    nph_game_capture_mouse(game, 1);
    return NPH_RESULT_OK;
}

static void play_end(NphGame *game)
{
    nph_game_capture_mouse(game, 0);
    state(game)->player = NPH_ENTITY_NULL;
}

// ---------------------------------------------------------------------------
// The frame: what E would push, outlined
// ---------------------------------------------------------------------------

typedef struct TargetLines {
    NphRenderer *renderer;
} TargetLines;

static void target_line(void *user, NphVec3 a, NphVec3 b, u32 rgba)
{
    const TargetLines *tl = (const TargetLines *)user;
    (void)nph_render_debug_line(tl->renderer, a, b, rgba, NPH_RENDER_DEBUG_OVERLAY);
}

static void frame(NphGame *game, f64 dt)
{
    (void)dt;
    NphEngine *engine = nph_game_engine(game);
    const StarterState *st = state(game);
    NphRenderer *r = nph_engine_renderer(engine);
    NphPhysicsWorld *pw = nph_game_physics(game);
    NphWorld *w = nph_game_world(game);
    if (r == NULL || pw == NULL || !nph_world_is_alive(w, st->player)) {
        return;
    }
    const StarterPlayer *p = player_of(game, w, st->player);
    const NphWorldTransform *eye =
        p != NULL ? NPH_WORLD_GET(w, p->eye, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform)
                  : NULL;
    if (eye == NULL) {
        return;
    }
    const NphPhysicsQueryFilter filter = eye_filter(game, st->player);
    const NphVec3 dir = nph_vec3_normalize_or(nph_vec3_neg(eye->z_axis), nph_vec3_zero());
    NphPhysicsQueryHit hit;
    if (!push_target(pw, &filter, eye->translation, dir, p->reach, &hit)) {
        return;
    }
    // The shape it meets, where its body is.
    const NphQuat body = nph_physics_body_rotation(pw, hit.body);
    const NphPhysicsPose local = nph_physics_shape_pose(pw, hit.shape);
    NphPhysicsPose pose;
    pose.position = nph_quat_rotate(body, local.position);
    pose.rotation = nph_quat_normalize(nph_quat_mul(body, local.rotation));
    const NphPhysicsGeometry g = nph_physics_shape_geometry(pw, hit.shape);
    TargetLines tl = {r};
    NphPhysicsDebugDraw d;
    memset(&d, 0, sizeof d);
    d.line = target_line;
    d.user = &tl;
    d.origin = nph_render_debug_origin(r);
    nph_physics_geometry_draw(&d, &g, nph_physics_body_position(pw, hit.body), pose, COLOR_TARGET);
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static NphResult bind_actions(NphGame *game)
{
    StarterState *s = state(game);
    NphInput *input = nph_engine_input(nph_game_engine(game));
    NPH_TRY(nph_game_action(game, "starter.move_x", &s->move_x));
    NPH_TRY(nph_game_action(game, "starter.move_z", &s->move_z));
    NPH_TRY(nph_game_action(game, "starter.run", &s->run));
    NPH_TRY(nph_game_action(game, "starter.jump", &s->jump));
    NPH_TRY(nph_game_action(game, "starter.interact", &s->interact));
    // A reload binds again: the old bindings go first.
    const NphActionId all[5] = {s->move_x, s->move_z, s->run, s->jump, s->interact};
    for (u32 i = 0; i < 5U; ++i) {
        nph_input_unbind_all(input, all[i]);
    }
    NPH_TRY(nph_input_bind_key_axis(input, s->move_x, NPH_KEY_A, NPH_KEY_D));
    NPH_TRY(nph_input_bind_key_axis(input, s->move_z, NPH_KEY_S, NPH_KEY_W));
    NPH_TRY(nph_input_bind_key(input, s->run, NPH_KEY_LEFT_SHIFT, NPH_MOD_ANY));
    NPH_TRY(nph_input_bind_key(input, s->jump, NPH_KEY_SPACE, NPH_MOD_ANY));
    return nph_input_bind_key(input, s->interact, NPH_KEY_E, NPH_MOD_ANY);
}

static NphResult load(NphGame *game, const NphGameLoadInfo *info)
{
    NPH_LOG_INFO(&LOG_STARTER, "starter %s%s", info->reloaded ? "reloaded" : "loaded",
                 info->world_migrated ? " (components changed)" : "");
    return bind_actions(game);
}

static NphResult add_systems(NphGame *game, NphSchedule *schedule)
{
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    // The player's components by lookup (one entity, its eye another's
    // LocalTransform); a body pushed by E: a write of RigidBody, so that no
    // other system changes bodies meanwhile (game.h nph_game_physics).
    const NphComponentId writes[4] = {nph_game_component(game, COMPONENT_PLAYER),
                                      ids->character_controller, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                      ids->rigid_body};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "starter.drive";
    d.run = drive;
    d.user = game;
    d.writes = writes;
    d.write_count = 4U;
    return nph_schedule_add_system(schedule, &d, NULL);
}

NPH_GAME_EXPORT const NphGameApi *nph_game_entry(void)
{
    static const NphComponentDesc *components[COMPONENT_COUNT];
    components[COMPONENT_PLAYER] = StarterPlayer_component();
    static NphGameApi api;
    api.abi_version = NPH_GAME_ABI_VERSION;
    api.state_version = 1U;
    api.state_size = sizeof(StarterState);
    api.state_align = NPH_ALIGNOF(StarterState);
    api.name = "starter";
    api.components = components;
    api.component_count = COMPONENT_COUNT;
    api.load = load;
    api.add_systems = add_systems;
    api.frame = frame;
    api.play_begin = play_begin;
    api.play_end = play_end;
    return &api;
}
