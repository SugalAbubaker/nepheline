// SPDX-License-Identifier: Apache-2.0
// Audio in the engine (components.h "Audio"; docs/runtime.md "Audio"):
// AudioSources play their sounds while the world plays, by themselves or
// as the game says; spatial ones sound from where they are around the
// listener (an AudioListener, else the camera), behind walls darker and
// quieter, but not hidden by the floor they rest on; one-shots play from any thread; streamed sounds prime and play;
// sources that go stop; and what cannot load gives up. Headless engines in
// lockstep (a step's worth of sound a frame), mixing offline, with sound
// assets written for the test and found through the engine's resolver.
#include <nepheline/asset/format.h>
#include <nepheline/asset/sound.h>
#include <nepheline/audio/lossless.h>
#include <nepheline/core/alloc_debug.h>
#include <nepheline/math/quat.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>
#include <nepheline/runtime/engine.h>

#include <ntest/ntest.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TA_RATE 48000U
#define TA_PI   3.14159265358979323846

typedef struct TaSound {
    NphGuid guid;
    char path[64]; // virtual
} TaSound;

typedef struct AudioFixture {
    NphDebugAllocator da;
    NphEngineConfig config;
    NphEngine *engine;
    NphRuntimeComponents ids;
    char dir[1024]; // the content mounted at the VFS root
    TaSound sounds[8];
    u32 sound_count;
    // The mix, every frame's appended (interleaved stereo).
    f32 *heard;
    u64 heard_frames;
    u64 heard_cap;
} AudioFixture;

static NphResult ta_resolve(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len)
{
    const AudioFixture *f = (const AudioFixture *)user;
    for (u32 i = 0U; i < f->sound_count; ++i) {
        if (nph_guid_equals(f->sounds[i].guid, guid)) {
            const u64 n = strlen(f->sounds[i].path);
            if (n + 1U > cap) {
                return NPH_RESULT_OVERFLOW;
            }
            memcpy(buf, f->sounds[i].path, (size_t)n + 1U);
            *out_len = n;
            return NPH_RESULT_OK;
        }
    }
    return NPH_RESULT_NOT_FOUND;
}

// A sound asset named `name` in the fixture's content: `frames` frames of a
// mono 440 Hz tone at the mix's rate, at `amp`, 16-bit PCM in memory, or
// our lossless codec streamed.
static NphGuid ta_sound(AudioFixture *f, const char *name, u64 frames, f32 amp, bool stream)
{
    NphAllocator *a = nph_allocator_system();
    i16 *pcm = (i16 *)NPH_ALLOC(a, frames * sizeof(i16), 16U);
    i32 *ints = (i32 *)NPH_ALLOC(a, frames * sizeof(i32), 16U);
    NTEST_ASSERT_NOT_NULL(pcm);
    NTEST_ASSERT_NOT_NULL(ints);
    for (u64 i = 0U; i < frames; ++i) {
        pcm[i] = (i16)(32767.0 * (f64)amp * sin(2.0 * TA_PI * 440.0 * (f64)i / (f64)TA_RATE));
        ints[i] = pcm[i];
    }
    const NphGuid guid = nph_guid_from_cstr(name);
    NphAssetWriter w;
    nph_asset_writer_init(&w, a, NPH_ASSET_TYPE_SOUND, NPH_SOUND_TYPE_VERSION, guid, 7U);
    NphSoundHeader h;
    memset(&h, 0, sizeof h);
    h.channels = 1U;
    h.sample_rate = TA_RATE;
    h.frames = frames;
    h.loop_end = frames;
    h.bits = 16U;
    if (!stream) {
        h.codec = NPH_SOUND_CODEC_PCM;
        h.format = NPH_SOUND_FORMAT_S16;
        NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_SOUND_TAG_HEADER, 0U, &h, sizeof h,
                                                      NPH_ASSET_CODEC_NONE, 0U),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_SOUND_TAG_DATA, 0U, pcm,
                                                      frames * sizeof(i16), NPH_ASSET_CODEC_NONE,
                                                      0U),
                            NPH_RESULT_OK);
    } else {
        NphLosslessEncoded enc;
        NTEST_ASSERT_EQ_INT(nph_lossless_encode(a, ints, 1U, 16U, frames, NULL, &enc),
                            NPH_RESULT_OK);
        h.codec = NPH_SOUND_CODEC_LOSSLESS;
        h.block_frames = enc.stream.block_frames;
        h.block_count = enc.stream.block_count;
        h.flags = NPH_SOUND_FLAG_STREAM;
        NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_SOUND_TAG_HEADER, 0U, &h, sizeof h,
                                                      NPH_ASSET_CODEC_NONE, 0U),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_SOUND_TAG_DATA, 0U, enc.stream.data,
                                                      enc.stream.size, NPH_ASSET_CODEC_NONE, 0U),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_SOUND_TAG_BLOCKS, 0U,
                                                      enc.stream.block_offsets,
                                                      (u64)enc.stream.block_count * sizeof(u32),
                                                      NPH_ASSET_CODEC_NONE, 0U),
                            NPH_RESULT_OK);
        nph_lossless_encoded_free(&enc);
    }
    NTEST_ASSERT_LT_UINT(f->sound_count, NPH_ARRAY_COUNT(f->sounds));
    TaSound *s = &f->sounds[f->sound_count++];
    s->guid = guid;
    (void)snprintf(s->path, sizeof s->path, "%s.nph", name);
    char full[1200];
    (void)snprintf(full, sizeof full, "%s/%s", f->dir, s->path);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_write_file(&w, full), NPH_RESULT_OK);
    nph_asset_writer_free(&w);
    NPH_FREE(a, pcm, frames * sizeof(i16), 16U);
    NPH_FREE(a, ints, frames * sizeof(i32), 16U);
    return guid;
}

static void start_with(AudioFixture *f, const char *device)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    static u32 counter = 0U;
    (void)snprintf(f->dir, sizeof f->dir, "%s/nph_audio_bridge_%llu_%u", tmp,
                   (unsigned long long)nph_platform_process_id(), counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
    static char assignment[64];
    (void)snprintf(assignment, sizeof assignment, "audio.device=%s", device);
    f->config.headless = 1;
    f->config.assignments[0] = "rt.lockstep=true";
    f->config.assignments[1] = assignment;
    f->config.assignments[2] = "audio.sample_rate=48000";
    f->config.assignments[3] = "audio.channels=2";
    f->config.assignment_count = 4U;
    f->config.contents[0] = f->dir;
    f->config.content_count = 1U;
}

static void create(AudioFixture *f)
{
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.config = &f->config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "audio bridge tests";
    d.resolve = ta_resolve;
    d.resolve_user = f;
    NTEST_ASSERT_EQ_INT(nph_engine_create(&d, &f->engine), NPH_RESULT_OK);
    f->ids = *nph_engine_components(f->engine);
}

static void finish(AudioFixture *f)
{
    nph_engine_destroy(f->engine);
    if (f->heard != NULL) {
        NPH_FREE(nph_allocator_system(), f->heard, f->heard_cap * 2U * sizeof(f32), 16U);
    }
    NTEST_EXPECT_EQ_INT(nph_fs_remove_tree(f->dir), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// Runs `n` frames, keeping what each rendered.
static void frames(AudioFixture *f, u32 n)
{
    for (u32 i = 0U; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(f->engine), NPH_RESULT_OK);
        u32 got = 0U;
        const f32 *out = nph_engine_audio_output(f->engine, &got);
        if (out == NULL || got == 0U) {
            continue;
        }
        if (f->heard_frames + got > f->heard_cap) {
            const u64 cap = nph_max_u64(2U * f->heard_cap, f->heard_frames + got + 48000U);
            f32 *grown = (f32 *)NPH_ALLOC(nph_allocator_system(), cap * 2U * sizeof(f32), 16U);
            NTEST_ASSERT_NOT_NULL(grown);
            if (f->heard != NULL) {
                memcpy(grown, f->heard, (size_t)(f->heard_frames * 2U * sizeof(f32)));
                NPH_FREE(nph_allocator_system(), f->heard, f->heard_cap * 2U * sizeof(f32), 16U);
            }
            f->heard = grown;
            f->heard_cap = cap;
        }
        memcpy(f->heard + f->heard_frames * 2U, out, (size_t)got * 2U * sizeof(f32));
        f->heard_frames += got;
    }
    NTEST_ASSERT_EQ_INT(nph_engine_wait(f->engine), NPH_RESULT_OK);
}

// The energy of channel `ch` over the last `count` frames heard.
static f64 energy(const AudioFixture *f, u32 ch, u64 count)
{
    count = nph_min_u64(count, f->heard_frames);
    f64 e = 0.0;
    for (u64 i = f->heard_frames - count; i < f->heard_frames; ++i) {
        const f64 x = (f64)f->heard[i * 2U + ch];
        e += x * x;
    }
    return count > 0U ? e / (f64)count : 0.0;
}

static f64 loudness(const AudioFixture *f, u64 count)
{
    return energy(f, 0U, count) + energy(f, 1U, count);
}

// An entity at `at` with an AudioSource of `sound`.
static NphEntity spawn_source(AudioFixture *f, NphGuid sound, NphDVec3 at, bool spatial,
                              bool autoplay, bool loop)
{
    NphWorld *w = nph_engine_world(f->engine);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.audio_source};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &e), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation = at;
    NphAudioSource *src = NPH_WORLD_GET_MUT(w, e, f->ids.audio_source, NphAudioSource);
    src->sound = sound;
    src->spatial = spatial;
    src->autoplay = autoplay;
    src->loop = loop;
    src->occlusion = 0;
    src->reverb = 0.0F; // the dry sound alone: what the tests measure
    return e;
}

static NphAudioSource *source(AudioFixture *f, NphEntity e)
{
    return NPH_WORLD_GET_MUT(nph_engine_world(f->engine), e, f->ids.audio_source, NphAudioSource);
}

static const NphAudioSource *source_now(AudioFixture *f, NphEntity e)
{
    return NPH_WORLD_GET(nph_engine_world(f->engine), e, f->ids.audio_source, NphAudioSource);
}

static NphEngineAudioStats stats_of(AudioFixture *f)
{
    NphEngineAudioStats st;
    nph_engine_audio_stats(f->engine, &st);
    return st;
}

// A static box centred at `at` (a Collider without a RigidBody).
static NphEntity spawn_box(AudioFixture *f, NphDVec3 at, NphVec3 half_extents)
{
    NphWorld *w = nph_engine_world(f->engine);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.collider};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &e), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation = at;
    NphCollider *c = NPH_WORLD_GET_MUT(w, e, f->ids.collider, NphCollider);
    c->shape = NPH_COLLIDER_BOX;
    c->half_extents = half_extents;
    return e;
}

// A listener at the origin, looking down -Z.
static NphEntity spawn_listener(AudioFixture *f, NphQuat rotation)
{
    NphWorld *w = nph_engine_world(f->engine);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.audio_listener};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &e), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->rotation =
        rotation;
    return e;
}

// ---------------------------------------------------------------------------

NTEST(audio_bridge, a_source_plays_by_itself_and_says_when_it_ends)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "tone", 4800U, 0.5F, false); // 0.1 s
    create(&f);
    const NphEntity e = spawn_source(&f, tone, nph_dvec3_zero(), false, true, false);
    NTEST_EXPECT_FALSE(source_now(&f, e)->playing);
    frames(&f, 4U); // not playing yet: silence
    NTEST_EXPECT_TRUE(loudness(&f, 800U) == 0.0);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 2U);
    NTEST_EXPECT_TRUE(source_now(&f, e)->playing != 0);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 1U);
    frames(&f, 10U);
    // A centred mono tone at 0.5: about 0.25 / 2 of energy in each channel
    // after equal-power panning (0.707 each).
    const f64 l = energy(&f, 0U, 1600U);
    const f64 r = energy(&f, 1U, 1600U);
    NTEST_LOG("left %.4f right %.4f", l, r);
    NTEST_EXPECT_TRUE(l > 0.04 && r > 0.04);
    NTEST_EXPECT_TRUE(fabs(l - r) < 1e-6);
    // 0.1 s is 24 steps: over by 40, the source said so and is silent.
    frames(&f, 30U);
    NTEST_EXPECT_FALSE(source_now(&f, e)->playing);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 0U);
    NTEST_EXPECT_TRUE(loudness(&f, 1600U) == 0.0);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, the_game_starts_and_stops_a_source)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "loop", 9600U, 0.5F, false);
    create(&f);
    const NphEntity e = spawn_source(&f, tone, nph_dvec3_zero(), false, false, true);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 6U);
    NTEST_EXPECT_TRUE(loudness(&f, 800U) == 0.0); // no autoplay: silence
    source(&f, e)->playing = 1;
    frames(&f, 6U);
    NTEST_EXPECT_TRUE(loudness(&f, 800U) > 0.05);
    // Looping: still playing well past its length.
    frames(&f, 100U);
    NTEST_EXPECT_TRUE(source_now(&f, e)->playing != 0);
    NTEST_EXPECT_TRUE(loudness(&f, 800U) > 0.05);
    // Quieter as its volume falls, at once.
    const f64 before = loudness(&f, 800U);
    source(&f, e)->volume = 0.25F;
    frames(&f, 6U);
    const f64 after = loudness(&f, 800U);
    NTEST_LOG("volume 1: %.4f, 0.25: %.4f", before, after);
    NTEST_EXPECT_TRUE(fabs(after / before - 0.0625) < 0.005);
    // Stopped: it fades, then silence; the voice is gone.
    source(&f, e)->playing = 0;
    frames(&f, 20U);
    NTEST_EXPECT_TRUE(loudness(&f, 800U) == 0.0);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 0U);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, a_spatial_source_sounds_from_where_it_is)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "far", 9600U, 0.5F, false);
    create(&f);
    // A listener at the origin looking down -Z: +X is its right.
    (void)spawn_listener(&f, nph_quat_identity());
    const NphEntity e = spawn_source(&f, tone, nph_dvec3(5.0, 0.0, 0.0), true, true, true);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 20U);
    f64 l = energy(&f, 0U, 1600U);
    f64 r = energy(&f, 1U, 1600U);
    NTEST_LOG("at +x: left %.5f right %.5f", l, r);
    NTEST_EXPECT_TRUE(r > 10.0 * l);
    // Moved to its left: the other way round.
    NPH_WORLD_GET_MUT(nph_engine_world(f.engine), e, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                      NphLocalTransform)
        ->translation = nph_dvec3(-5.0, 0.0, 0.0);
    nph_engine_reset_interpolation(f.engine, e); // a jump, not a motion: no Doppler
    frames(&f, 20U);
    l = energy(&f, 0U, 1600U);
    r = energy(&f, 1U, 1600U);
    NTEST_LOG("at -x: left %.5f right %.5f", l, r);
    NTEST_EXPECT_TRUE(l > 10.0 * r);
    // Farther is quieter (inverse distance: 5 m against 20 m).
    const f64 near = l + r;
    NPH_WORLD_GET_MUT(nph_engine_world(f.engine), e, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                      NphLocalTransform)
        ->translation = nph_dvec3(-20.0, 0.0, 0.0);
    nph_engine_reset_interpolation(f.engine, e);
    frames(&f, 20U);
    const f64 far = loudness(&f, 1600U);
    NTEST_LOG("5 m: %.5f, 20 m: %.5f", near, far);
    NTEST_EXPECT_TRUE(far < near / 8.0);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, without_a_listener_the_camera_hears)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "cam", 9600U, 0.5F, false);
    create(&f);
    // A camera turned to look down +Z: world +X is its left.
    NphWorld *w = nph_engine_world(f.engine);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f.ids.camera};
    NphEntity cam;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &cam), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, cam, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->rotation =
        nph_quat_from_axis_angle(nph_vec3_unit_y(), (f32)TA_PI);
    NPH_WORLD_GET_MUT(w, cam, f.ids.camera, NphCamera)->active = 1;
    (void)spawn_source(&f, tone, nph_dvec3(5.0, 0.0, 0.0), true, true, true);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 20U);
    const f64 l = energy(&f, 0U, 1600U);
    const f64 r = energy(&f, 1U, 1600U);
    NTEST_LOG("left %.5f right %.5f", l, r);
    NTEST_EXPECT_TRUE(l > 10.0 * r);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, a_wall_between_darkens_and_quietens)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "wall", 9600U, 0.5F, false);
    create(&f);
    (void)spawn_listener(&f, nph_quat_identity());
    const NphEntity e = spawn_source(&f, tone, nph_dvec3(0.0, 0.0, -10.0), true, true, true);
    source(&f, e)->occlusion = 1;
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 60U);
    const f64 open = loudness(&f, 1600U);
    // A wall halfway, wider than every ray's offset: all five sight lines
    // blocked (the four probes find nothing around the source).
    (void)spawn_box(&f, nph_dvec3(0.0, 0.0, -5.0), nph_vec3(4.0F, 4.0F, 0.2F));
    frames(&f, 120U); // the smoothing (80 ms) settles in half a second
    const f64 shut = loudness(&f, 1600U);
    const NphEngineAudioStats st = stats_of(&f);
    NTEST_LOG("open %.5f, behind the wall %.5f (%u rays a frame)", open, shut, st.occlusion_rays);
    NTEST_EXPECT_TRUE(st.occlusion_rays == 9U);
    NTEST_EXPECT_TRUE(shut < open / 10.0); // occlusion takes up to 24 dB and the highs
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, a_sound_that_starts_behind_a_wall_starts_hidden)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid blip = ta_sound(&f, "knock", 2400U, 0.5F, false); // 50 ms
    create(&f);
    (void)spawn_listener(&f, nph_quat_identity());
    (void)spawn_box(&f, nph_dvec3(0.0, 0.0, -5.0), nph_vec3(4.0F, 4.0F, 0.2F));
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_preload_sound(f.engine, blip), NPH_RESULT_OK);
    frames(&f, 2U);
    // Behind the wall, a knock shorter than the occlusion's smoothing: all
    // of it, its attack too, as hidden as the wall makes it, against the
    // same knock heard through no wall (its occlusion off).
    NphSoundPlayDesc d = nph_sound_play_desc_default();
    d.position = nph_dvec3(0.0, 0.0, -10.0);
    d.reverb = 0.0F;
    d.occlusion = 0;
    NTEST_ASSERT_EQ_INT(nph_engine_play_sound(f.engine, blip, &d), NPH_RESULT_OK);
    frames(&f, 20U);
    const f64 open = loudness(&f, 4000U);
    d.occlusion = 1;
    NTEST_ASSERT_EQ_INT(nph_engine_play_sound(f.engine, blip, &d), NPH_RESULT_OK);
    frames(&f, 20U);
    const f64 hidden = loudness(&f, 4000U);
    NTEST_LOG("a knock through no wall %.6f, behind the wall %.6f (%.1f dB)", open, hidden,
              10.0 * log10(hidden / open));
    NTEST_EXPECT_TRUE(open > 1e-4);
    NTEST_EXPECT_TRUE(hidden < open / 100.0); // occlusion takes 24 dB and the highs
    // Three at once with a budget of one source's rays a frame: the first is
    // cast as it starts, the other two first thing the next frame (a frame
    // of them open, 4 ms of their 50), so all three are hidden but for that.
    NphCvar *rays = nph_cvar_find(nph_engine_cvars(f.engine), NPH_STR("audio.occlusion_rays"));
    NTEST_ASSERT_NOT_NULL(rays);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_int(nph_engine_cvars(f.engine), rays, 9), NPH_RESULT_OK);
    d.occlusion = 0;
    for (u32 i = 0U; i < 3U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_play_sound(f.engine, blip, &d), NPH_RESULT_OK);
    }
    frames(&f, 20U);
    const f64 open3 = loudness(&f, 4000U);
    d.occlusion = 1;
    for (u32 i = 0U; i < 3U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_play_sound(f.engine, blip, &d), NPH_RESULT_OK);
    }
    u32 most = 0U;
    for (u32 i = 0U; i < 20U; ++i) {
        frames(&f, 1U);
        most = nph_max_u32(most, stats_of(&f).occlusion_rays);
    }
    const f64 hidden3 = loudness(&f, 4000U);
    NTEST_LOG("three knocks through no wall %.6f, behind it %.6f (%.1f dB), at most %u rays a frame",
              open3, hidden3, 10.0 * log10(hidden3 / open3), most);
    NTEST_EXPECT_TRUE(most <= 9U);
    NTEST_EXPECT_TRUE(hidden3 < open3 / 10.0);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, the_floor_a_source_rests_on_does_not_hide_it)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "floor", 9600U, 0.5F, false);
    create(&f);
    // A listener at head height over a floor whose top is at y = 0, and a
    // source resting on it 10 m ahead, as an impact's point lies on a
    // surface: of the points around it, the one below lies in the floor.
    const NphEntity l = spawn_listener(&f, nph_quat_identity());
    NPH_WORLD_GET_MUT(nph_engine_world(f.engine), l, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                      NphLocalTransform)
        ->translation = nph_dvec3(0.0, 1.7, 0.0);
    (void)spawn_box(&f, nph_dvec3(0.0, -0.5, 0.0), nph_vec3(30.0F, 0.5F, 30.0F));
    const NphEntity e = spawn_source(&f, tone, nph_dvec3(0.0, 0.0, -10.0), true, true, true);
    source(&f, e)->occlusion = 1;
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 120U);
    const f64 rays = (f64)stats_of(&f).occlusion_rays;
    const f64 heard = loudness(&f, 1600U);
    // The same source without occlusion: what it sounds like unhidden.
    source(&f, e)->occlusion = 0;
    frames(&f, 20U);
    const f64 open = loudness(&f, 1600U);
    NTEST_LOG("occluded %.5f, open %.5f (%.0f rays a frame)", heard, open, rays);
    // The probe into the floor finds it, and those to the points beside the
    // source, on the floor's surface, may: four probes and two to four sight
    // lines.
    NTEST_EXPECT_TRUE(rays >= 6.0 && rays <= 8.0);
    NTEST_EXPECT_TRUE(fabs(heard / open - 1.0) < 0.01);
    // A wall between them still hides it.
    source(&f, e)->occlusion = 1;
    (void)spawn_box(&f, nph_dvec3(0.0, 2.0, -5.0), nph_vec3(4.0F, 2.0F, 0.2F));
    frames(&f, 120U);
    NTEST_EXPECT_TRUE(loudness(&f, 1600U) < open / 10.0);
    nph_engine_play_end(f.engine);
    finish(&f);
}

typedef struct TaPlayer {
    NphEngine *engine;
    NphGuid sound;
    NphResult result;
} TaPlayer;

static void ta_play_from_thread(void *user)
{
    TaPlayer *p = (TaPlayer *)user;
    NphSoundPlayDesc d = nph_sound_play_desc_default();
    d.spatial = 0;
    d.reverb = 0.0F;
    p->result = nph_engine_play_sound(p->engine, p->sound, &d);
}

NTEST(audio_bridge, one_shots_play_from_any_thread)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid blip = ta_sound(&f, "blip", 2400U, 0.5F, false);
    create(&f);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    // Loaded before its first one-shot, as a game's play_begin would.
    NTEST_ASSERT_EQ_INT(nph_engine_preload_sound(f.engine, blip), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_engine_preload_sound(f.engine, nph_guid_from_cstr("absent")),
                        NPH_RESULT_NOT_FOUND);
    frames(&f, 2U);
    // From another thread (as a system would), then heard at the next
    // frame.
    TaPlayer p = {f.engine, blip, NPH_RESULT_UNKNOWN};
    NphThread t;
    const NphThreadDesc td = {ta_play_from_thread, &p, "test-play", 0U};
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &td), NPH_RESULT_OK);
    nph_thread_join(&t);
    NTEST_EXPECT_EQ_INT(p.result, NPH_RESULT_OK);
    frames(&f, 3U);
    NTEST_EXPECT_TRUE(loudness(&f, 400U) > 0.05);
    frames(&f, 30U); // 50 ms: over
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 0U);
    // A full queue refuses, and counts.
    NphSoundPlayDesc d = nph_sound_play_desc_default();
    d.spatial = 0;
    u32 refused = 0U;
    for (u32 i = 0U; i < 300U; ++i) {
        refused += nph_engine_play_sound(f.engine, blip, &d) == NPH_RESULT_FULL ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(refused, 300U - 256U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).oneshots_dropped, 300U - 256U);
    frames(&f, 2U);
    NTEST_EXPECT_TRUE(stats_of(&f).voices > 0U); // the mixer's slots took what they could
    // A sound nobody can find gives up.
    NTEST_EXPECT_EQ_INT(nph_engine_play_sound(f.engine, nph_guid_from_cstr("nowhere"), &d),
                        NPH_RESULT_OK);
    frames(&f, 40U);
    const NphEngineAudioStats st = stats_of(&f);
    NTEST_EXPECT_EQ_UINT(st.sounds_failed, 2U); // "absent" and "nowhere"
    NTEST_EXPECT_EQ_UINT(st.voices, 0U);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, a_streamed_sound_primes_and_plays)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid song = ta_sound(&f, "song", 3ULL * TA_RATE, 0.5F, true);
    create(&f);
    (void)spawn_source(&f, song, nph_dvec3_zero(), false, true, false);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    // The first frame makes its stream; the streamer primes the ring on its
    // thread, and the voice starts once it holds a quarter second (offline
    // frames outrun real time, so give it time).
    frames(&f, 1U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 1U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices_waiting, 1U);
    u32 waited = 0U;
    while (stats_of(&f).voices_waiting > 0U && waited < 2000U) {
        frames(&f, 1U);
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
        ++waited;
    }
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices_waiting, 0U);
    // Paced as the device would be, it plays without starving.
    for (u32 i = 0U; i < 60U; ++i) {
        frames(&f, 1U);
        nph_thread_sleep_ns(nph_time_us_to_ns(4200U));
    }
    const NphEngineAudioStats st = stats_of(&f);
    NTEST_LOG("primed after %u frames; streams %u, decoded %llu frames, starved %llu", waited,
              st.streamer.streams, (unsigned long long)st.streamer.frames_decoded,
              (unsigned long long)st.mixer.stream_starved);
    NTEST_EXPECT_EQ_UINT(st.sounds_streamed, 1U);
    NTEST_EXPECT_EQ_UINT(st.streamer.streams, 1U);
    NTEST_EXPECT_EQ_UINT(st.streamer.read_errors, 0U);
    NTEST_EXPECT_EQ_UINT(st.streamer.corrupt_blocks, 0U);
    NTEST_EXPECT_TRUE(loudness(&f, 1600U) > 0.05);
    // Play's end closes the stream and releases the sound.
    nph_engine_play_end(f.engine);
    const NphEngineAudioStats after = stats_of(&f);
    NTEST_EXPECT_EQ_UINT(after.streamer.streams, 0U);
    NTEST_EXPECT_EQ_UINT(after.sounds, 0U);
    finish(&f);
}

NTEST(audio_bridge, what_goes_stops)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "hum", 9600U, 0.5F, false);
    create(&f);
    const NphEntity a = spawn_source(&f, tone, nph_dvec3_zero(), false, true, true);
    const NphEntity b = spawn_source(&f, tone, nph_dvec3_zero(), false, true, true);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 4U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 2U);
    // A despawned entity's voice and a removed AudioSource's stop.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(nph_engine_world(f.engine), a), NPH_RESULT_OK);
    frames(&f, 2U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 1U);
    NTEST_ASSERT_EQ_INT(nph_world_remove(nph_engine_world(f.engine), b, f.ids.audio_source),
                        NPH_RESULT_OK);
    frames(&f, 20U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 0U);
    NTEST_EXPECT_TRUE(loudness(&f, 800U) == 0.0);
    // Added back while playing, with autoplay: it plays by itself again.
    NphAudioSource src = *(const NphAudioSource *)NphAudioSource_component()->defaults;
    src.sound = tone;
    src.autoplay = 1;
    src.spatial = 0;
    NTEST_ASSERT_EQ_INT(nph_world_set(nph_engine_world(f.engine), b, f.ids.audio_source, &src),
                        NPH_RESULT_OK);
    frames(&f, 4U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 1U);
    nph_engine_play_end(f.engine);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 0U);
    // Played again (as the editor does), it starts over.
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, true), NPH_RESULT_OK);
    frames(&f, 4U);
    NTEST_EXPECT_EQ_UINT(stats_of(&f).voices, 1U);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, the_volume_cvars_and_a_sound_that_cannot_load)
{
    AudioFixture f;
    start_with(&f, "offline");
    const NphGuid tone = ta_sound(&f, "level", 9600U, 0.5F, false);
    create(&f);
    (void)spawn_source(&f, tone, nph_dvec3_zero(), false, true, true);
    const NphEntity lost =
        spawn_source(&f, nph_guid_from_cstr("missing"), nph_dvec3_zero(), false, true, true);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    frames(&f, 10U);
    const f64 full = loudness(&f, 800U);
    NTEST_EXPECT_TRUE(full > 0.05);
    NTEST_EXPECT_FALSE(source_now(&f, lost)->playing); // it gave up and said so
    NTEST_EXPECT_EQ_UINT(stats_of(&f).sounds_failed, 1U);
    NphCvar *v = nph_cvar_find(nph_engine_cvars(f.engine), NPH_STR("audio.volume_effects"));
    NTEST_ASSERT_NOT_NULL(v);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_float(nph_engine_cvars(f.engine), v, 0.5), NPH_RESULT_OK);
    frames(&f, 10U);
    NTEST_EXPECT_TRUE(fabs(loudness(&f, 800U) / full - 0.25) < 0.01);
    NphCvar *m = nph_cvar_find(nph_engine_cvars(f.engine), NPH_STR("audio.volume"));
    NTEST_ASSERT_NOT_NULL(m);
    NTEST_ASSERT_EQ_INT(nph_cvar_set_float(nph_engine_cvars(f.engine), m, 0.0), NPH_RESULT_OK);
    frames(&f, 10U);
    NTEST_EXPECT_TRUE(loudness(&f, 800U) == 0.0);
    nph_engine_play_end(f.engine);
    finish(&f);
}

NTEST(audio_bridge, with_audio_off_nothing_plays)
{
    AudioFixture f;
    start_with(&f, "off");
    const NphGuid tone = ta_sound(&f, "quiet", 4800U, 0.5F, false);
    create(&f);
    NTEST_EXPECT_NULL(nph_engine_audio_mixer(f.engine));
    (void)spawn_source(&f, tone, nph_dvec3_zero(), false, true, false);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(f.engine, false), NPH_RESULT_OK);
    const NphSoundPlayDesc d = nph_sound_play_desc_default();
    NTEST_EXPECT_EQ_INT(nph_engine_play_sound(f.engine, tone, &d), NPH_RESULT_OK);
    frames(&f, 10U);
    const NphEngineAudioStats st = stats_of(&f);
    NTEST_EXPECT_EQ_INT(st.mode, NPH_ENGINE_AUDIO_OFF);
    NTEST_EXPECT_EQ_UINT(st.voices, 0U);
    NTEST_EXPECT_EQ_UINT(f.heard_frames, 0U);
    nph_engine_play_end(f.engine);
    finish(&f);
}
