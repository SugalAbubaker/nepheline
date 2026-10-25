// SPDX-License-Identifier: Apache-2.0
// game_audio: the audio sample (CLAUDE.md §12 Phase 12; docs/runtime.md
// "Audio"), a game module run by nph_player and the editor:
//
//     nph_player --game=<build>/bin/<Config>/game_audio<.dll|.so>
//                --pack=<build>/samples/game_physics/<Config>/physics.npk
//                --pack=<build>/samples/sounds/<Config>/sounds.npk
//
// When the world has nothing to draw, it builds a yard on the physics
// sample's ground and fills it with sound (samples/sounds/make_sounds.c
// makes the sounds; the physics sample's content draws the yard):
//   - music on the music bus and wind on the ambience bus, both streamed
//     and everywhere (not in 3D);
//   - a drone, a ball humming in 3D as it circles in front of the camera,
//     which is the listener: heard from the side it is on, louder near,
//     its pitch bent as it comes and goes (Doppler), and darker and quieter
//     while a stone wall stands between (occlusion);
//   - chimes played once round the yard, some behind the wall, by a system
//     on the job system (one-shots are played from any thread), and knocks
//     that follow the drone (cooked as ADPCM).
//
// With yard.check set (the headless smoke run, which mixes offline in
// lockstep: audio.device=offline), the game listens to its own mix every
// frame and says at play_end whether it ran clean: sound from the first
// frame of play to the last, the drone heard from either side, the chimes
// and knocks played, and nothing starved, dropped, refused, or failed. With
// yard.capture set to a path, it also writes what it heard there as a WAV
// file to listen to (the smoke run writes the build's
// samples/game_audio/yard.wav).
#include <nepheline/audio/wav.h>
#include <nepheline/math/quat.h>
#include <nepheline/math/random.h>
#include <nepheline/platform/file.h>
#include <nepheline/runtime/audio.h>
#include <nepheline/runtime/components.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/game.h>
#include <nepheline/runtime/scene_render.h>

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_YARD, "yard", NPH_LOG_LEVEL_INFO);

// The physics sample's content (samples/game_physics/content): the cube and
// sphere meshes, and the ball and stone materials.
#define MESH_CUBE      "physics.gltf#mesh/0"
#define MESH_SPHERE    "physics.gltf#mesh/1"
#define MATERIAL_BALL  "physics.gltf#material/3"
#define MATERIAL_STONE "physics.gltf#material/6"

// The sounds, by the GUIDs nph_cook gives them (docs/cook.md "Sounds").
#define SOUND_MUSIC "music.wav#sound"
#define SOUND_WIND  "wind.wav#sound"
#define SOUND_HUM   "hum.wav#sound"
#define SOUND_CHIME "chime.wav#sound"
#define SOUND_KNOCK "knock.wav#sound"

// The camera looks down -Z from above the yard's near edge; the drone's
// orbit passes 1.7 m in front of it, 6 m off to either side (35 degrees
// out), and 8.5 m away behind the wall.
#define CAMERA_HEIGHT  2.0
#define CAMERA_Z       8.0
#define DRONE_RADIUS   0.25F // the ball's
#define ORBIT_CENTRE_Z 3.0
#define ORBIT_RADIUS   3.5F
#define ORBIT_PERIOD   6.0F // seconds a lap: about 3.7 m/s, a Doppler shift of 1%
#define WALL_Z         0.8
#define CHIME_SECONDS  1.25 // between chimes
#define KNOCK_SECONDS  2.0  // between knocks
// How far to a side the drone must be for the check to count the mix's
// lean as its.
#define SIDE_X 1.5

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

// Circles its entity round `centre` at `radius`, once every `period`
// seconds, `phase` of a lap in at step 0.
#define YARD_ORBIT_FIELDS(F)                                                                       \
    F(DVEC3, centre, NPH_FIELD_NO_HINTS)                                                           \
    F(F32, radius, NPH_FIELD_RANGE(0.0F, 100.0F))                                                  \
    F(F32, period, NPH_FIELD_RANGE(0.1F, 600.0F))                                                  \
    F(F32, phase, NPH_FIELD_RANGE(0.0F, 1.0F))
NPH_COMPONENT_STRUCT(YardOrbit, YARD_ORBIT_FIELDS);
NPH_COMPONENT_DECLARE(YardOrbit);
static const YardOrbit g_orbit_defaults = {{0.0, 0.0, 0.0}, ORBIT_RADIUS, ORBIT_PERIOD, 0.0F};
NPH_COMPONENT_DEFINE(YardOrbit, YARD_ORBIT_FIELDS, "yard.Orbit", 0U, &g_orbit_defaults)

enum {
    COMPONENT_ORBIT,
    COMPONENT_COUNT
};

// ---------------------------------------------------------------------------
// State (kept across hot reloads)
// ---------------------------------------------------------------------------

static NPH_CVAR_BOOL(g_check, "yard.check", false,
                     "listen to the mix and say at the end of play whether it ran clean (the "
                     "headless smoke run; read when play begins)",
                     NPH_CVAR_FLAG_NONE);

static NPH_CVAR_STRING(g_capture, "yard.capture", "",
                       "a WAV file to write the offline mix to at the end of play (read when play "
                       "begins; empty: none)",
                       NPH_CVAR_FLAG_NONE);

typedef struct YardState {
    NphEntity drone; // the knocks follow it
    b32 check;       // yard.check when play began
    u32 channels;    // the mix's
    // The one-shots, counted by the system that plays them.
    u32 chimes;
    u32 knocks;
    u32 refused;
    // What the check heard (the main thread's, at the sync point).
    u64 heard_frames;
    u32 heard_steps; // frames of play the mix had something in, from the first
    u32 silent_steps;
    b32 sounding; // the first sound has come
    // The mix's lean ((R - L) / (R + L) of energy) summed over the frames
    // the drone was off to the right, and to the left.
    f64 lean_right;
    f64 lean_left;
    u32 right_steps;
    u32 left_steps;
    // The capture (yard.capture): the mix heard, interleaved.
    char capture_path[512];
    f32 *capture;
    u64 capture_samples;
    u64 capture_cap;
    u64 plays;
} YardState;

static YardState *state(const NphGame *game)
{
    return (YardState *)nph_game_state(game);
}

static NphGuid guid(const char *name)
{
    return nph_guid_from_cstr(name);
}

// ---------------------------------------------------------------------------
// Making the yard
// ---------------------------------------------------------------------------

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

// An entity named `name` with a transform and `extra` components.
static NphResult spawn_named(NphWorld *w, const char *name, const NphComponentId *extra,
                             u32 extra_count, NphEntity *out)
{
    NphComponentId comps[8] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                               NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NPH_ASSERT(extra_count <= 5U);
    memcpy(comps + 3, extra, (size_t)extra_count * sizeof *extra);
    NPH_TRY(nph_world_spawn(w, comps, 3U + extra_count, out));
    nph_text_set(&NPH_WORLD_GET_MUT(w, *out, NPH_COMPONENT_ID_NAME, NphName)->text,
                 nph_str_from_cstr(name));
    return NPH_RESULT_OK;
}

// A sound everywhere, looping by itself from the start of play.
static NphResult spawn_ambient(NphGame *game, const char *name, const char *sound, u32 bus,
                               f32 volume)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    NphEntity e;
    NPH_TRY(spawn_named(w, name, &ids->audio_source, 1U, &e));
    NphAudioSource *src = NPH_WORLD_GET_MUT(w, e, ids->audio_source, NphAudioSource);
    src->sound = guid(sound);
    src->bus = bus;
    src->volume = volume;
    src->loop = 1;
    src->autoplay = 1;
    src->spatial = 0;
    return NPH_RESULT_OK;
}

// The camera, which is also the listener, looking at the yard from above
// its near edge.
static NphResult spawn_listener(NphGame *game)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    const NphComponentId comps[2] = {ids->camera, ids->audio_listener};
    NphEntity e;
    NPH_TRY(spawn_named(w, "listener", comps, 2U, &e));
    NphLocalTransform *t =
        NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    t->translation = nph_dvec3(0.0, CAMERA_HEIGHT, CAMERA_Z);
    t->rotation = nph_quat_from_rotation_x(-0.1F);
    NPH_WORLD_GET_MUT(w, e, ids->camera, NphCamera)->active = 1;
    NPH_WORLD_GET_MUT(w, e, ids->audio_listener, NphAudioListener)->active = 1;
    return NPH_RESULT_OK;
}

// A static block of stone at `at`, `size` across: drawn, and a Collider,
// which the drone's occlusion rays meet.
static NphResult spawn_stone(NphGame *game, const char *name, NphDVec3 at, NphVec3 size)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    const NphComponentId comps[2] = {ids->mesh_renderer, ids->collider};
    NphEntity e;
    NPH_TRY(spawn_named(w, name, comps, 2U, &e));
    NphLocalTransform *t =
        NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    t->translation = at;
    t->scale = size; // the Collider's unit cube scales with it
    NphMeshRenderer *m = NPH_WORLD_GET_MUT(w, e, ids->mesh_renderer, NphMeshRenderer);
    m->mesh = guid(MESH_CUBE);
    nph_mesh_renderer_set_material(m, 0U, guid(MATERIAL_STONE));
    return NPH_RESULT_OK;
}

// The drone: a humming ball circling the yard's middle.
static NphResult spawn_drone(NphGame *game)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    const NphComponentId comps[3] = {ids->mesh_renderer, ids->audio_source,
                                     nph_game_component(game, COMPONENT_ORBIT)};
    NphEntity e;
    NPH_TRY(spawn_named(w, "drone", comps, 3U, &e));
    YardOrbit *orbit =
        (YardOrbit *)nph_world_get_mut(w, e, nph_game_component(game, COMPONENT_ORBIT));
    orbit->centre = nph_dvec3(0.0, 1.2, ORBIT_CENTRE_Z);
    NphLocalTransform *t =
        NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    t->translation = nph_dvec3(0.0, 1.2, ORBIT_CENTRE_Z + (f64)ORBIT_RADIUS); // the orbit's start
    t->scale = nph_vec3_splat(2.0F * DRONE_RADIUS);
    NphMeshRenderer *m = NPH_WORLD_GET_MUT(w, e, ids->mesh_renderer, NphMeshRenderer);
    m->mesh = guid(MESH_SPHERE);
    nph_mesh_renderer_set_material(m, 0U, guid(MATERIAL_BALL));
    NphAudioSource *src = NPH_WORLD_GET_MUT(w, e, ids->audio_source, NphAudioSource);
    src->sound = guid(SOUND_HUM);
    src->bus = (u32)NPH_AUDIO_SOURCE_BUS_EFFECTS;
    src->volume = 1.2F;
    src->loop = 1;
    src->autoplay = 1;
    src->spatial = 1;
    src->min_distance = 1.5F;
    src->max_distance = 40.0F;
    src->size = DRONE_RADIUS;
    src->occlusion = 1;
    src->reverb = 0.3F;
    src->priority = 200U;
    state(game)->drone = e;
    return NPH_RESULT_OK;
}

// The physics sample's ground and sun (a static floor for the rays too),
// the wall, the drone, the music, the wind, and the listener.
static NphResult build_yard(NphGame *game)
{
    NphEngine *engine = nph_game_engine(game);
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(engine);
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = w;
    d.loader = nph_engine_loader(engine);
    d.scene = guid("physics.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    d.name = "yard";
    const NphResult r = nph_scene_import_cooked(&d, NULL, NULL);
    if (r != NPH_RESULT_OK) {
        // Heard but not seen: the sounds do not need the yard drawn.
        NPH_LOG_WARN(&LOG_YARD, "no ground to draw (%s): mount the physics sample's pack",
                     nph_result_string(r));
    } else {
        const NphEntity ground = find_named(w, "ground");
        if (!nph_entity_is_null(ground)) {
            NPH_TRY(nph_world_add(w, ground, ids->collider, NULL));
            NphCollider *c = NPH_WORLD_GET_MUT(w, ground, ids->collider, NphCollider);
            c->half_extents = nph_vec3(20.0F, 0.5F, 20.0F);
            c->center = nph_vec3(0.0F, -0.5F, 0.0F);
        }
    }
    // Narrower than the orbit, which goes round its ends.
    NPH_TRY(spawn_stone(game, "wall", nph_dvec3(0.0, 1.5, WALL_Z), nph_vec3(4.0F, 3.0F, 0.3F)));
    NPH_TRY(spawn_drone(game));
    NPH_TRY(spawn_ambient(game, "music", SOUND_MUSIC, (u32)NPH_AUDIO_SOURCE_BUS_MUSIC, 0.25F));
    NPH_TRY(spawn_ambient(game, "wind", SOUND_WIND, (u32)NPH_AUDIO_SOURCE_BUS_AMBIENCE, 0.2F));
    return spawn_listener(game);
}

// ---------------------------------------------------------------------------
// Systems
// ---------------------------------------------------------------------------

// Orbits move their entities' LocalTransforms to where they are at the end
// of this step.
static void orbit(NphSystemContext *ctx)
{
    const NphGame *game = (const NphGame *)ctx->user;
    const f64 tau = 6.283185307179586;
    const f64 t = (f64)(nph_game_step(game) + 1U) * nph_game_step_seconds(game);
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        const YardOrbit *o = NPH_CHUNK_COLUMN(&v, 0, YardOrbit);
        NphLocalTransform *lt = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        for (u32 i = 0; i < v.count; ++i) {
            const f64 laps = t / nph_f64_max((f64)o[i].period, 0.1) + (f64)o[i].phase;
            const f64 a = tau * (laps - nph_f64_floor(laps));
            const f64 r = (f64)o[i].radius;
            lt[i].translation =
                nph_dvec3_add(o[i].centre, nph_dvec3(r * nph_f64_sin(a), 0.0, r * nph_f64_cos(a)));
        }
    }
}

// Every so many steps, `seconds` of them.
static bool every(const NphGame *game, f64 seconds)
{
    const u64 steps = (u64)nph_f64_max(1.0, seconds / nph_game_step_seconds(game) + 0.5);
    const u64 step = nph_game_step(game);
    return step > 0U && step % steps == 0U;
}

// The one-shots, played from this system's job: a chime somewhere round the
// yard (a different place and note each time, from the step), and a knock
// on the drone.
static void sounds(NphSystemContext *ctx)
{
    NphGame *game = (NphGame *)ctx->user;
    YardState *st = state(game);
    if (every(game, CHIME_SECONDS)) {
        NphPcg32 rng;
        nph_pcg32_seed(&rng, nph_game_step(game), 7U);
        static const f32 notes[5] = {1.0F, 1.125F, 1.25F, 1.5F, 1.6667F}; // a pentatonic scale
        const f64 a = 6.283185307179586 * nph_pcg32_next_f64(&rng);
        const f64 r = 3.0 + 6.0 * nph_pcg32_next_f64(&rng);
        NphSoundPlayDesc d = nph_sound_play_desc_default();
        d.position = nph_dvec3(r * nph_f64_sin(a), 0.5 + 2.0 * nph_pcg32_next_f64(&rng),
                               r * nph_f64_cos(a) - 1.0);
        d.pitch = notes[nph_pcg32_next_u32(&rng) % 5U];
        d.volume = 0.6F;
        d.reverb = 0.4F;
        if (nph_game_play_sound(game, guid(SOUND_CHIME), &d) == NPH_RESULT_OK) {
            st->chimes += 1U;
        } else {
            st->refused += 1U;
        }
    }
    if (every(game, KNOCK_SECONDS) && nph_world_is_alive(ctx->world, st->drone)) {
        NphSoundPlayDesc d = nph_sound_play_desc_default();
        d.follow = st->drone;
        d.volume = 0.8F;
        if (nph_game_play_sound(game, guid(SOUND_KNOCK), &d) == NPH_RESULT_OK) {
            st->knocks += 1U;
        } else {
            st->refused += 1U;
        }
    }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static NphResult load(NphGame *game, const NphGameLoadInfo *info)
{
    NPH_LOG_INFO(&LOG_YARD, "yard %s%s", info->reloaded ? "reloaded" : "loaded",
                 info->world_migrated ? " (components changed)" : "");
    NPH_TRY(nph_game_register_cvar(game, &g_check));
    return nph_game_register_cvar(game, &g_capture);
}

static NphResult add_systems(NphGame *game, NphSchedule *schedule)
{
    const NphQueryTerm orbit_terms[2] = {nph_term_read(nph_game_component(game, COMPONENT_ORBIT)),
                                         nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "yard.orbit";
    d.run = orbit;
    d.user = game;
    d.terms = orbit_terms;
    d.term_count = 2U;
    NPH_TRY(nph_schedule_add_system(schedule, &d, NULL));
    // It reads nothing of the world but whether the drone lives.
    memset(&d, 0, sizeof d);
    d.name = "yard.sounds";
    d.run = sounds;
    d.user = game;
    return nph_schedule_add_system(schedule, &d, NULL);
}

// Keeps `count` samples of the mix for the capture, the buffer doubling as
// it fills (on the main thread, at the sync point: a sample's tool, not a
// game's frame).
static void capture(YardState *st, const f32 *samples, u64 count)
{
    if (st->capture_samples + count > st->capture_cap) {
        u64 cap = nph_max_u64(st->capture_cap, 1U << 20U);
        while (cap < st->capture_samples + count) {
            cap *= 2U;
        }
        f32 *grown = (f32 *)NPH_REALLOC(nph_allocator_system(), st->capture,
                                        st->capture_cap * sizeof(f32), cap * sizeof(f32), 16U);
        if (grown == NULL) {
            NPH_LOG_WARN(&LOG_YARD, "the capture stops: out of memory");
            st->capture_path[0] = '\0';
            return;
        }
        st->capture = grown;
        st->capture_cap = cap;
    }
    memcpy(st->capture + st->capture_samples, samples, (size_t)count * sizeof(f32));
    st->capture_samples += count;
}

// Writes the capture as a 32-bit float WAV file, and lets it go.
static void write_capture(YardState *st, u32 rate)
{
    const u32 channels = st->channels;
    const u64 frames = channels > 0U ? st->capture_samples / channels : 0U;
    if (st->capture_path[0] != '\0' && frames > 0U && channels <= 8U) {
        f32 *planes[8] = {NULL};
        bool ok = true;
        for (u32 c = 0; c < channels && ok; ++c) {
            planes[c] = NPH_ALLOC_ARRAY(nph_allocator_system(), f32, frames);
            ok = planes[c] != NULL;
            for (u64 i = 0; ok && i < frames; ++i) {
                planes[c][i] = st->capture[i * channels + c];
            }
        }
        const u64 size = nph_wav_size(NPH_WAV_FLOAT32, channels, frames);
        void *bytes = ok ? NPH_ALLOC(nph_allocator_system(), size, 16U) : NULL;
        const f32 *const *planar = (const f32 *const *)planes;
        NphResult r = bytes != NULL
                          ? nph_wav_write(NPH_WAV_FLOAT32, channels, rate, planar, frames, bytes)
                          : NPH_RESULT_OUT_OF_MEMORY;
        if (r == NPH_RESULT_OK) {
            r = nph_file_write_all(st->capture_path, bytes, size);
        }
        if (r == NPH_RESULT_OK) {
            NPH_LOG_INFO(&LOG_YARD, "the mix written to %s (%.1f s)", st->capture_path,
                         (f64)frames / (f64)rate);
        } else {
            NPH_LOG_WARN(&LOG_YARD, "the mix not written to %s: %s", st->capture_path,
                         nph_result_string(r));
        }
        if (bytes != NULL) {
            NPH_FREE(nph_allocator_system(), bytes, size, 16U);
        }
        for (u32 c = 0; c < channels; ++c) {
            if (planes[c] != NULL) {
                NPH_FREE_ARRAY(nph_allocator_system(), f32, planes[c], frames);
            }
        }
    }
    if (st->capture != NULL) {
        NPH_FREE(nph_allocator_system(), st->capture, st->capture_cap * sizeof(f32), 16U);
    }
    st->capture = NULL;
    st->capture_samples = 0U;
    st->capture_cap = 0U;
}

// The check's ears, and the capture's: the mix the last frame rendered
// (offline), each frame once.
static void frame(NphGame *game, f64 dt)
{
    (void)dt;
    YardState *st = state(game);
    if (!st->check && st->capture_path[0] == '\0') {
        return;
    }
    u32 frames = 0U;
    const f32 *out = nph_engine_audio_output(nph_game_engine(game), &frames);
    if (out == NULL || frames == 0U || st->channels < 2U) {
        return;
    }
    if (st->capture_path[0] != '\0') {
        capture(st, out, (u64)frames * st->channels);
    }
    // The front pair: the first two channels.
    f64 left = 0.0;
    f64 right = 0.0;
    for (u32 i = 0; i < frames; ++i) {
        const f64 l = (f64)out[(u64)i * st->channels];
        const f64 r = (f64)out[(u64)i * st->channels + 1U];
        left += l * l;
        right += r * r;
    }
    st->heard_frames += frames;
    if (!(left + right > 0.0)) {
        st->silent_steps += st->sounding ? 1U : 0U;
        return;
    }
    st->sounding = 1;
    st->heard_steps += 1U;
    // Which side the drone is on, the listener at x = 0 facing -Z: the
    // chimes and the arpeggio lean the mix this way and that too, but only
    // the drone stays on a side long enough to lean the average.
    const NphWorldTransform *wt = NPH_WORLD_GET(
        nph_game_world(game), st->drone, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    if (wt == NULL) {
        return;
    }
    const f64 lean = (right - left) / (right + left); // -1 all left, 1 all right
    if (wt->translation.x > SIDE_X) {
        st->lean_right += lean;
        st->right_steps += 1U;
    } else if (wt->translation.x < -SIDE_X) {
        st->lean_left += lean;
        st->left_steps += 1U;
    }
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

static NphResult play_begin(NphGame *game)
{
    NphWorld *w = nph_game_world(game);
    const NphRuntimeComponents *ids = nph_engine_components(nph_game_engine(game));
    YardState *st = state(game);
    const u64 plays = st->plays + 1U;
    memset(st, 0, sizeof *st);
    st->plays = plays;
    st->drone = find_named(w, "drone");
    st->check = nph_cvar_get_bool(&g_check);
    NphEngineAudioStats a;
    nph_engine_audio_stats(nph_game_engine(game), &a);
    st->channels = a.channels;
    const NphStr path = nph_cvar_get_string(&g_capture);
    if (path.len > 0U && path.len < sizeof st->capture_path) {
        memcpy(st->capture_path, path.data, (size_t)path.len);
        st->capture_path[path.len] = '\0';
    }
    if (!world_draws_something(w, ids->mesh_renderer)) {
        NPH_TRY(build_yard(game));
    }
    // The one-shots' sounds, loaded before the first (a one-shot whose
    // sound is late is dropped: runtime.md "Audio").
    const char *const oneshots[2] = {SOUND_CHIME, SOUND_KNOCK};
    for (u32 i = 0; i < 2U; ++i) {
        const NphResult r = nph_game_preload_sound(game, guid(oneshots[i]));
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&LOG_YARD, "%s: %s (mount the samples' sounds pack)", oneshots[i],
                         nph_result_string(r));
        }
    }
    return NPH_RESULT_OK;
}

static void play_end(NphGame *game)
{
    YardState *st = state(game);
    NphEngine *engine = nph_game_engine(game);
    NphEngineAudioStats a;
    nph_engine_audio_stats(engine, &a);
    write_capture(st, a.sample_rate);
    NPH_LOG_INFO(&LOG_YARD,
                 "played %llu steps: %u chimes, %u knocks; %u sounds (%u streamed, %u failed), %u "
                 "voices; streams decoded %llu frames, starved %llu",
                 (unsigned long long)nph_game_step(game), st->chimes, st->knocks, a.sounds,
                 a.sounds_streamed, a.sounds_failed, a.voices,
                 (unsigned long long)a.streamer.frames_decoded,
                 (unsigned long long)a.mixer.stream_starved);
    if (!st->check) {
        return;
    }
    // The verdict. The drone circles in front of the listener: on average
    // its hum leans the mix towards whichever side it is on, by more than a
    // fifth of the energy, whatever the beds and the one-shots do.
    const f64 seconds = (f64)st->heard_frames / nph_f64_max((f64)a.sample_rate, 1.0);
    const u64 laps = nph_game_step(game) / (u64)((f64)ORBIT_PERIOD / nph_game_step_seconds(game));
    const f64 right = st->right_steps > 0U ? st->lean_right / (f64)st->right_steps : 0.0;
    const f64 left = st->left_steps > 0U ? st->lean_left / (f64)st->left_steps : 0.0;
    const bool clean = a.mode == NPH_ENGINE_AUDIO_OFFLINE && st->sounding &&
                       st->silent_steps == 0U && laps >= 1U && right > 0.2 && left < -0.2 &&
                       st->chimes > 0U && st->knocks > 0U && st->refused == 0U &&
                       a.oneshots_dropped == 0U && a.sounds_failed == 0U &&
                       a.sounds_streamed == 2U && a.mixer.stream_starved == 0U &&
                       a.mixer.commands_dropped == 0U && a.mixer.corrupt_reads == 0U &&
                       a.streamer.read_errors == 0U && a.streamer.corrupt_blocks == 0U;
    if (clean) {
        NPH_LOG_INFO(&LOG_YARD,
                     "check: the mix ran clean (%.1f s heard with no silence; over %llu laps the "
                     "drone leaning it %+.2f on the right and %+.2f on the left; %u chimes and %u "
                     "knocks played; music and wind streamed)",
                     seconds, (unsigned long long)laps, right, left, st->chimes, st->knocks);
    } else {
        NPH_LOG_ERROR(&LOG_YARD,
                      "check failed: mode %u, %.1f s heard, %u silent steps, leaning %+.2f on "
                      "the right and %+.2f on the left over %llu laps, %u chimes, %u knocks, %u "
                      "refused, %u dropped, %u failed, %u streamed, starved %llu, commands dropped "
                      "%u, corrupt reads %u, read errors %u, corrupt blocks %u",
                      (u32)a.mode, seconds, st->silent_steps, right, left, (unsigned long long)laps,
                      st->chimes, st->knocks, st->refused, a.oneshots_dropped, a.sounds_failed,
                      a.sounds_streamed, (unsigned long long)a.mixer.stream_starved,
                      a.mixer.commands_dropped, a.mixer.corrupt_reads, a.streamer.read_errors,
                      a.streamer.corrupt_blocks);
    }
}

NPH_GAME_EXPORT const NphGameApi *nph_game_entry(void)
{
    static const NphComponentDesc *components[COMPONENT_COUNT];
    components[COMPONENT_ORBIT] = YardOrbit_component();
    static NphGameApi api;
    api.abi_version = NPH_GAME_ABI_VERSION;
    api.state_version = 1U;
    api.state_size = sizeof(YardState);
    api.state_align = NPH_ALIGNOF(YardState);
    api.name = "yard";
    api.components = components;
    api.component_count = COMPONENT_COUNT;
    api.load = load;
    api.add_systems = add_systems;
    api.frame = frame;
    api.play_begin = play_begin;
    api.play_end = play_end;
    return &api;
}
