// SPDX-License-Identifier: Apache-2.0
// game_sandbox: a small game module (CLAUDE.md §8.12; docs/runtime.md
// "Game modules"), run by nph_player and the editor:
//
//     nph_player --game=<build>/bin/<Config>/game_sandbox<.dll|.so>
//                --pack=<build>/samples/asset_viewer/<Config>/showcase.npk
//
// When the world has nothing to draw, it brings in the asset viewer's
// cooked showcase scene and makes the torus spin. A camera flies with
// W/A/S/D and Q/E, looking around while the right mouse button is held.
// A grid marks the ground.
//
// Everything the game keeps lives in the world or in its state block, so a
// rebuild of this file is picked up by the running game (game.hot_reload):
// change SPIN_SPEED below, rebuild the target, and the torus turns at the
// new speed without a restart.
#include <nepheline/render/debug_draw.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/game.h>

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SANDBOX, "sandbox", NPH_LOG_LEVEL_INFO);

#define SPIN_SPEED   0.8F   // radians per second
#define FLY_SPEED    4.0F   // metres per second
#define LOOK_RADIANS 0.003F // per screen unit of mouse motion

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

#define SANDBOX_SPINNER_FIELDS(F)                                                                  \
    F(F32, speed, NPH_FIELD_NO_HINTS)                                                              \
    F(VEC3, axis, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(SandboxSpinner, SANDBOX_SPINNER_FIELDS);
NPH_COMPONENT_DECLARE(SandboxSpinner);
static const SandboxSpinner g_spinner_defaults = {SPIN_SPEED, {0.0F, 1.0F, 0.0F}};
NPH_COMPONENT_DEFINE(SandboxSpinner, SANDBOX_SPINNER_FIELDS, "sandbox.Spinner", 0U,
                     &g_spinner_defaults)

#define SANDBOX_FLY_FIELDS(F)                                                                      \
    F(F32, speed, NPH_FIELD_NO_HINTS)                                                              \
    F(F32, yaw, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_ANGLE))                                             \
    F(F32, pitch, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_ANGLE))
NPH_COMPONENT_STRUCT(SandboxFly, SANDBOX_FLY_FIELDS);
NPH_COMPONENT_DECLARE(SandboxFly);
static const SandboxFly g_fly_defaults = {FLY_SPEED, 0.0F, 0.0F};
NPH_COMPONENT_DEFINE(SandboxFly, SANDBOX_FLY_FIELDS, "sandbox.FlyCamera", 0U, &g_fly_defaults)

enum {
    COMPONENT_SPINNER,
    COMPONENT_FLY,
    COMPONENT_COUNT
};

// ---------------------------------------------------------------------------
// State (kept across hot reloads)
// ---------------------------------------------------------------------------

typedef struct SandboxState {
    NphActionId move_x; // A / D
    NphActionId move_y; // Q / E
    NphActionId move_z; // S / W
    NphActionId look;   // right mouse button held
    u64 plays;
} SandboxState;

static SandboxState *state(const NphGame *game)
{
    return (SandboxState *)nph_game_state(game);
}

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

static void spin(NphSystemContext *ctx)
{
    const f32 dt = (f32)ctx->dt;
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        const SandboxSpinner *s = NPH_CHUNK_COLUMN(&v, 0, SandboxSpinner);
        NphLocalTransform *t = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        for (u32 i = 0; i < v.count; ++i) {
            const NphVec3 axis = nph_vec3_normalize_or(s[i].axis, nph_vec3_unit_y());
            const NphQuat step = nph_quat_from_axis_angle(axis, s[i].speed * dt);
            t[i].rotation = nph_quat_normalize(nph_quat_mul(step, t[i].rotation));
        }
    }
}

static void fly(NphSystemContext *ctx)
{
    NphGame *game = (NphGame *)ctx->user;
    const SandboxState *st = state(game);
    const NphSimInput *in = nph_game_input(game);
    const f32 dt = (f32)ctx->dt;
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        SandboxFly *f = NPH_CHUNK_COLUMN(&v, 0, SandboxFly);
        NphLocalTransform *t = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        for (u32 i = 0; i < v.count; ++i) {
            if (nph_sim_input_down(in, st->look)) {
                f[i].yaw -= (f32)in->mouse_dx * LOOK_RADIANS;
                f[i].pitch =
                    nph_f32_clamp(f[i].pitch - (f32)in->mouse_dy * LOOK_RADIANS, -1.5F, 1.5F);
            }
            const NphQuat yaw = nph_quat_from_rotation_y(f[i].yaw);
            t[i].rotation = nph_quat_mul(yaw, nph_quat_from_rotation_x(f[i].pitch));
            // Cameras look down -Z.
            const NphVec3 forward = nph_quat_rotate(yaw, nph_vec3(0.0F, 0.0F, -1.0F));
            const NphVec3 right = nph_quat_rotate(yaw, nph_vec3(1.0F, 0.0F, 0.0F));
            NphVec3 move = nph_vec3_scale(right, nph_sim_input_value(in, st->move_x));
            move = nph_vec3_add(move, nph_vec3_scale(forward, nph_sim_input_value(in, st->move_z)));
            move = nph_vec3_add(
                move, nph_vec3_scale(nph_vec3_unit_y(), nph_sim_input_value(in, st->move_y)));
            const NphVec3 d = nph_vec3_scale(move, f[i].speed * dt);
            t[i].translation = nph_dvec3_add(t[i].translation, nph_dvec3_from_vec3(d));
        }
    }
}

static void draw_grid(NphSystemContext *ctx)
{
    NphGame *game = (NphGame *)ctx->user;
    NphRenderer *r = nph_engine_renderer(nph_game_engine(game));
    if (r != NULL) {
        nph_render_debug_grid(r, nph_vec3_zero(), 1.0F, 16U, NPH_RENDER_COLOR_GRAY, 0U);
    }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static NphResult bind_actions(NphGame *game)
{
    SandboxState *s = state(game);
    NphInput *input = nph_engine_input(nph_game_engine(game));
    NPH_TRY(nph_game_action(game, "sandbox.move_x", &s->move_x));
    NPH_TRY(nph_game_action(game, "sandbox.move_y", &s->move_y));
    NPH_TRY(nph_game_action(game, "sandbox.move_z", &s->move_z));
    NPH_TRY(nph_game_action(game, "sandbox.look", &s->look));
    // A reload binds again: the old bindings go first.
    const NphActionId all[4] = {s->move_x, s->move_y, s->move_z, s->look};
    for (u32 i = 0; i < 4U; ++i) {
        nph_input_unbind_all(input, all[i]);
    }
    NPH_TRY(nph_input_bind_key_axis(input, s->move_x, NPH_KEY_A, NPH_KEY_D));
    NPH_TRY(nph_input_bind_key_axis(input, s->move_y, NPH_KEY_Q, NPH_KEY_E));
    NPH_TRY(nph_input_bind_key_axis(input, s->move_z, NPH_KEY_S, NPH_KEY_W));
    return nph_input_bind_mouse_button(input, s->look, NPH_MOUSE_BUTTON_RIGHT, NPH_MOD_ANY);
}

static NphResult load(NphGame *game, const NphGameLoadInfo *info)
{
    NPH_LOG_INFO(&LOG_SANDBOX, "sandbox %s%s", info->reloaded ? "reloaded" : "loaded",
                 info->world_migrated ? " (components changed)" : "");
    return bind_actions(game);
}

static NphResult add_systems(NphGame *game, NphSchedule *schedule)
{
    const NphComponentId spinner = nph_game_component(game, COMPONENT_SPINNER);
    const NphComponentId fly_id = nph_game_component(game, COMPONENT_FLY);
    // Both move what the transform system composes this step.
    const char *const before[1] = {NPH_TRANSFORM_SYSTEM_NAME};
    const NphQueryTerm spin_terms[2] = {nph_term_read(spinner),
                                        nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "sandbox.spin";
    d.run = spin;
    d.user = game;
    d.terms = spin_terms;
    d.term_count = 2U;
    d.before = before;
    d.before_count = 1U;
    NPH_TRY(nph_schedule_add_system(schedule, &d, NULL));
    const NphQueryTerm fly_terms[2] = {nph_term_write(fly_id),
                                       nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    d.name = "sandbox.fly";
    d.run = fly;
    d.terms = fly_terms;
    NPH_TRY(nph_schedule_add_system(schedule, &d, NULL));
    memset(&d, 0, sizeof d);
    d.name = "sandbox.grid";
    d.run = draw_grid;
    d.user = game;
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

// The showcase and a camera, when the world brings none.
static NphResult play_begin(NphGame *game)
{
    NphEngine *engine = nph_game_engine(game);
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(engine);
    state(game)->plays += 1U;
    if (!world_draws_something(w, ids->mesh_renderer)) {
        NphSceneImportDesc d;
        memset(&d, 0, sizeof d);
        d.world = w;
        d.loader = nph_engine_loader(engine);
        d.scene = nph_guid_from_cstr("showcase.gltf#scene");
        d.root_transform = nph_local_transform_identity();
        d.name = "showcase";
        const NphResult r = nph_scene_import_cooked(&d, NULL, NULL);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&LOG_SANDBOX, "no showcase to play with (%s): mount its pack with --pack",
                         nph_result_string(r));
        }
        const NphEntity torus = find_named(w, "torus");
        if (!nph_entity_is_null(torus)) {
            NPH_TRY(nph_world_add(w, torus, nph_game_component(game, COMPONENT_SPINNER), NULL));
        }
    }
    bool has_camera = false;
    for (u32 i = 0; i < nph_world_entity_slot_count(w) && !has_camera; ++i) {
        has_camera = nph_world_has(w, nph_world_entity_at(w, i), ids->camera);
    }
    if (!has_camera) {
        const NphComponentId comps[5] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                         NPH_COMPONENT_ID_WORLD_TRANSFORM, ids->camera,
                                         nph_game_component(game, COMPONENT_FLY)};
        NphEntity cam;
        NPH_TRY(nph_world_spawn(w, comps, 5U, &cam));
        nph_text_set(&NPH_WORLD_GET_MUT(w, cam, NPH_COMPONENT_ID_NAME, NphName)->text,
                     NPH_STR("camera"));
        NPH_WORLD_GET_MUT(w, cam, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
            ->translation = nph_dvec3(0.0, 3.0, 8.0);
        SandboxFly *f =
            (SandboxFly *)nph_world_get_mut(w, cam, nph_game_component(game, COMPONENT_FLY));
        f->pitch = -0.36F; // at the stage
    }
    return NPH_RESULT_OK;
}

NPH_GAME_EXPORT const NphGameApi *nph_game_entry(void)
{
    static const NphComponentDesc *components[COMPONENT_COUNT];
    components[COMPONENT_SPINNER] = SandboxSpinner_component();
    components[COMPONENT_FLY] = SandboxFly_component();
    static NphGameApi api;
    api.abi_version = NPH_GAME_ABI_VERSION;
    api.state_version = 1U;
    api.state_size = sizeof(SandboxState);
    api.state_align = NPH_ALIGNOF(SandboxState);
    api.name = "sandbox";
    api.components = components;
    api.component_count = COMPONENT_COUNT;
    api.load = load;
    api.add_systems = add_systems;
    api.play_begin = play_begin;
    return &api;
}
