// SPDX-License-Identifier: Apache-2.0
// The audio bridge: the engine's sound. It owns the mixer, the streamer,
// and the output device (the audio.* cvars), loads the sounds the world's
// AudioSources and the game's one-shots name, and plays them while the
// world plays, placing spatial voices around the listener every frame. See
// components.h "Audio", docs/runtime.md "Audio", and
// docs/adr/0058-audio-in-the-engine.md.
//
// Finding what the game changed: at the frame's sync point, like a system
// run (as the scene renderer does), the bridge takes a fresh world tick and
// looks at the chunks whose AudioSource columns were written since its last
// pass, comparing each entity with its voice's record; the world's event
// stream says what went. Placing voices: every frame, each spatial voice's
// entity's world transform in the listener's frame (subtracted in 64 bits,
// then rounded to 32), with its velocity from its motion since the last
// frame (none across a teleport, nph_engine_reset_interpolation).
//
// Sounds load when first named (asynchronously; play_begin waits for those
// the world names) and stay until play ends. A sound played from memory
// becomes a clip, a copy (the audio thread must never fault on a mapping),
// and its asset is released at once. A streamed sound keeps a handle on its
// file, which each of its voices' streams reads at DATA's offset on the
// streamer's thread; the streamer primes the ring (defer_fill), so the main
// thread does no I/O, and the voice starts once the ring holds a quarter
// second.
#include <nepheline/asset/sound.h>
#include <nepheline/audio/device.h>
#include <nepheline/audio/mixer.h>
#include <nepheline/audio/stream.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/profile.h>
#include <nepheline/core/ring.h>
#include <nepheline/math/quat.h>
#include <nepheline/physics/query.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>
#include <nepheline/scene/query.h>
#include <nepheline/scene/transform.h>

#include "engine_internal.h"

#include <math.h>
#include <string.h>

#define NO_VOICE NPH_U32_MAX
// One-shots queued at once (nph_engine_play_sound).
#define ONESHOT_CAPACITY 256U
// How long a one-shot waits for its sound to load before it is dropped,
// and a stream for its ring to fill before its voice starts anyway.
#define ONESHOT_WAIT_NS (250ULL * 1000000ULL)
#define PRIME_WAIT_NS   (2000ULL * 1000000ULL)
// A spatial source's occlusion samples its centre and four points around
// it, as Steam Audio's volumetric occlusion samples a sphere round a
// source: a probe from the centre to each of the four (a point the source
// cannot see lies in or behind what it rests against, and is not part of
// it), and a sight line from the listener to each point that is. At most
// nine rays a source.
#define OCCLUSION_POINTS 5U
#define OCCLUSION_RAYS   9U
// Sight lines (and the probes, from the centre's) end this far, or a quarter
// of their length, short of their point: the surface a source rests on, as
// an impact's does, does not hide it.
#define OCCLUSION_END_MARGIN 0.05F
// Seconds the occlusion and a source's velocity take to follow (exponential
// smoothing): a wall that comes and goes must not click, nor a turn jump
// the pitch.
#define OCCLUSION_SMOOTHING 0.08
#define VELOCITY_SMOOTHING  0.05

typedef enum SoundState {
    SOUND_LOADING = 0,
    SOUND_READY,
    SOUND_FAILED,
} SoundState;

// What a streamed sound's streams read: its file from DATA's offset. On
// the heap, so the pointer the streamer holds stays valid while the sound
// table grows; freed at play end, once no stream is open.
typedef struct SoundFile {
    NphVfsFile file;
    u64 offset;
} SoundFile;

typedef struct SoundEntry {
    NphGuid guid;
    u32 state;            // SoundState
    NphAssetHandle asset; // while loading
    NphSoundHeader header;
    NphAudioCodec codec;
    NphAudioSampleFormat format;
    NphAudioClip clip;  // played from memory
    SoundFile *file;    // streamed
    u64 data_size;      // DATA's bytes
    u32 *block_offsets; // streamed lossless: its blocks (each stream copies them again)
} SoundEntry;

typedef enum VoiceState {
    VOICE_WAITING = 0, // for its sound to load, or for a stream slot
    VOICE_PRIMING,     // for its stream's ring to fill
    VOICE_PLAYING,
} VoiceState;

typedef struct VoiceRecord {
    NphEntity entity; // the AudioSource's, or the entity a one-shot follows (null: none)
    b32 oneshot;      // played by nph_engine_play_sound
    u32 sound;        // its entry
    u32 state;        // VoiceState
    NphAudioVoice voice;
    NphAudioClip stream_clip; // a streamed sound's stream for this voice
    NphAudioSource source;    // as last applied (a one-shot's made from its description)
    b32 spatial;              // its voice started in 3D (a mixer voice keeps that)
    NphDVec3 position;        // in the world, at the last frame
    NphVec3 velocity;         // in the world, smoothed
    b32 placed;               // `position` is known: velocities from the next frame
    f32 occlusion;            // as applied, 0 .. 1
    f32 occlusion_target;     // what the last rays found
    b32 occlusion_known;      // its rays were cast since it started (else the next frame's first)
    u64 since_ns;             // waiting or priming since
} VoiceRecord;

typedef struct OneShot {
    NphGuid sound;
    NphSoundPlayDesc desc;
} OneShot;

struct AudioBridge {
    NphEngine *engine;
    NphAllocator *alloc;
    NphEngineAudioMode mode;
    u32 rate;
    u32 channels;
    // The output.
    NphAudioMixer *mixer;
    NphAudioStreamer *streamer;
    NphAudioDevice *device;
    NphAudioBus buses[NPH_AUDIO_SOURCE_BUS_COUNT];
    NphAudioBus reverb_bus;
    NphAudioEffect reverb;
    f32 volumes[NPH_AUDIO_SOURCE_BUS_COUNT + 1U]; // as applied; the last is the master's
    // Offline rendering: what the last frame rendered, interleaved.
    f32 *offline;
    u32 offline_cap; // frames
    u32 offline_frames;
    f64 offline_carry; // the fraction of a sample frame a frame left over
    // The world.
    NphWorld *world;
    NphQuery *source_query;   // AudioSource: changes
    NphQuery *listener_query; // AudioListener, WorldTransform
    NphQuery *camera_query;   // Camera, WorldTransform
    u32 last_tick;
    u32 events_dropped;
    b32 full_pass;
    // Sounds, by GUID.
    SoundEntry *sounds;
    u32 sound_count;
    u32 sound_cap;
    NphHashMap sound_index; // NphGuid -> u32
    // Voices: entity voices are found by their entity's slot.
    VoiceRecord *voices;
    u32 voice_count;
    u32 voice_cap;
    u32 *slot_voice; // per entity slot: its voice, or NO_VOICE
    u32 *slot_seen;  // per entity slot: the handle last seen there (autoplay once)
    u32 slot_cap;
    NphMpmcRing oneshots;
    NphAtomicU32 oneshots_dropped;
    // The listener.
    NphDVec3 listener_position;
    NphVec3 listener_axes[3]; // right, up, back (-Z ahead), unit
    NphVec3 listener_velocity;
    b32 listener_placed;
    u32 occlusion_cursor;
    u32 occlusion_rays; // cast by the last frame
    b32 playing;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static f32 finite_clamp(f32 v, f32 lo, f32 hi, f32 fallback)
{
    if (!nph_f32_is_finite(v)) {
        return fallback;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

static f64 dvec3_dot(NphDVec3 a, NphVec3 b)
{
    return a.x * (f64)b.x + a.y * (f64)b.y + a.z * (f64)b.z;
}

static f32 vec3_dot(NphVec3 a, NphVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// A world offset in the listener's frame: +x right, +y up, -z ahead.
static NphVec3 to_listener(const AudioBridge *b, NphDVec3 d)
{
    return nph_vec3((f32)dvec3_dot(d, b->listener_axes[0]), (f32)dvec3_dot(d, b->listener_axes[1]),
                    (f32)dvec3_dot(d, b->listener_axes[2]));
}

static NphVec3 vec_to_listener(const AudioBridge *b, NphVec3 v)
{
    return nph_vec3(vec3_dot(v, b->listener_axes[0]), vec3_dot(v, b->listener_axes[1]),
                    vec3_dot(v, b->listener_axes[2]));
}

// A teleport this frame (nph_engine_reset_interpolation): no velocity from
// the jump.
static bool was_reset(const AudioBridge *b, NphEntity e)
{
    const NphEngine *engine = b->engine;
    if (engine->frame_resets > ENGINE_MAX_RESETS) {
        return true; // too many to keep: every entity is taken as reset
    }
    for (u32 i = 0U; i < engine->frame_resets; ++i) {
        if (engine->resets[i] == e.value) {
            return true;
        }
    }
    return false;
}

static void log_sound(NphGuid guid, const char *what, NphResult r)
{
    char text[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(guid, text);
    NPH_LOG_WARN(&NPH_LOG_RUNTIME, "sound %s: %s (%s)", text, what, nph_result_string(r));
}

// ---------------------------------------------------------------------------
// The output
// ---------------------------------------------------------------------------

static NphEngineAudioMode mode_of(const NphEngine *e)
{
    const NphStr d = nph_cvar_get_string(&e->cv.audio_device);
    if (nph_str_equals(d, NPH_STR("off"))) {
        return NPH_ENGINE_AUDIO_OFF;
    }
    if (nph_str_equals(d, NPH_STR("system"))) {
        return NPH_ENGINE_AUDIO_SYSTEM;
    }
    if (nph_str_equals(d, NPH_STR("null"))) {
        return NPH_ENGINE_AUDIO_NULL;
    }
    if (nph_str_equals(d, NPH_STR("offline"))) {
        return NPH_ENGINE_AUDIO_OFFLINE;
    }
    if (!nph_str_equals(d, NPH_STR("auto"))) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                     "audio.device \"%.*s\" is none of auto, off, system, null, offline: no audio",
                     (int)d.len, d.data);
        return NPH_ENGINE_AUDIO_OFF;
    }
    // The speakers with a window; nothing without one (tests and servers ask
    // for "offline" or "null").
    return e->mode == NPH_ENGINE_MODE_WINDOWED ? NPH_ENGINE_AUDIO_SYSTEM : NPH_ENGINE_AUDIO_OFF;
}

static NphResult open_device(AudioBridge *b, NphAudioBackend backend)
{
    NphAudioDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = b->alloc;
    dd.mixer = b->mixer;
    dd.backend = backend;
    NphResult r = nph_audio_device_open(&dd, &b->device);
    if (r == NPH_RESULT_OK) {
        r = nph_audio_device_start(b->device);
        if (r != NPH_RESULT_OK) {
            nph_audio_device_close(b->device);
            b->device = NULL;
        }
    }
    return r;
}

static NphResult start_output(AudioBridge *b)
{
    NphEngine *e = b->engine;
    u32 rate = (u32)nph_cvar_get_int(&e->cv.audio_rate);
    u32 channels = (u32)nph_cvar_get_int(&e->cv.audio_channels);
    if (b->mode == NPH_ENGINE_AUDIO_SYSTEM) {
        // The mix at the hardware's own format: nothing converts at the edge.
        NphAudioDeviceInfo info;
        const NphResult q = nph_audio_device_query(b->alloc, NPH_AUDIO_BACKEND_SYSTEM, &info);
        if (q != NPH_RESULT_OK) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME, "no audio device (%s): sound plays to the null device",
                         nph_result_string(q));
            b->mode = NPH_ENGINE_AUDIO_NULL;
        } else {
            rate = rate != 0U ? rate : info.sample_rate;
            channels = channels != 0U ? channels : info.channels;
        }
    }
    b->rate = rate != 0U ? rate : 48000U;
    b->channels = channels != 0U ? channels : 2U;
    NphAudioMixerDesc md;
    memset(&md, 0, sizeof md);
    md.alloc = b->alloc;
    md.sample_rate = b->rate;
    md.channels = b->channels;
    md.max_voices = (u32)nph_cvar_get_int(&e->cv.audio_voices);
    md.max_real_voices =
        nph_min_u32(md.max_voices, (u32)nph_cvar_get_int(&e->cv.audio_real_voices));
    NPH_TRY(nph_audio_mixer_create(&md, &b->mixer));
    NphAudioStreamerDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.alloc = b->alloc; // thread-safe, as the engine's allocators are
    sd.max_streams = (u32)nph_cvar_get_int(&e->cv.audio_streams);
    NPH_TRY(nph_audio_streamer_create(&sd, &b->streamer));
    static const char *const names[NPH_AUDIO_SOURCE_BUS_COUNT] = {"effects", "music", "dialogue",
                                                                  "ambience", "ui"};
    for (u32 i = 0U; i < NPH_AUDIO_SOURCE_BUS_COUNT; ++i) {
        NphAudioBusDesc bd = nph_audio_bus_desc_default();
        bd.name = names[i];
        NPH_TRY(nph_audio_bus_create(b->mixer, &bd, &b->buses[i]));
    }
    for (u32 i = 0U; i <= NPH_AUDIO_SOURCE_BUS_COUNT; ++i) {
        b->volumes[i] = 1.0F;
    }
    NphAudioBusDesc rd = nph_audio_bus_desc_default();
    rd.name = "reverb";
    NPH_TRY(nph_audio_bus_create(b->mixer, &rd, &b->reverb_bus));
    const NphAudioEffectDesc reverb = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_REVERB);
    NPH_TRY(nph_audio_effect_create(b->mixer, b->reverb_bus, &reverb, &b->reverb));
    if (b->mode == NPH_ENGINE_AUDIO_SYSTEM) {
        const NphResult r = open_device(b, NPH_AUDIO_BACKEND_SYSTEM);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                         "the audio device did not open (%s): sound plays to the null device",
                         nph_result_string(r));
            b->mode = NPH_ENGINE_AUDIO_NULL;
        }
    }
    if (b->mode == NPH_ENGINE_AUDIO_NULL) {
        NPH_TRY(open_device(b, NPH_AUDIO_BACKEND_NULL));
    }
    if (b->mode == NPH_ENGINE_AUDIO_OFFLINE) {
        b->offline_cap = b->rate; // a second; a longer frame renders in pieces
        b->offline = NPH_ALLOC_ARRAY_ZEROED(b->alloc, f32, (u64)b->offline_cap * b->channels);
        if (b->offline == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    NPH_LOG_INFO(&NPH_LOG_RUNTIME, "audio: %s, %u Hz, %u channels",
                 b->mode == NPH_ENGINE_AUDIO_SYSTEM ? "the system's device"
                 : b->mode == NPH_ENGINE_AUDIO_NULL ? "the null device"
                                                    : "rendered offline",
                 b->rate, b->channels);
    return NPH_RESULT_OK;
}

static void stop_output(AudioBridge *b)
{
    // The device first: once it is closed, no callback renders the mixer.
    nph_audio_device_close(b->device);
    b->device = NULL;
    nph_audio_mixer_destroy(b->mixer); // its stream clips close their streams
    b->mixer = NULL;
    nph_audio_streamer_destroy(b->streamer);
    b->streamer = NULL;
    if (b->offline != NULL) {
        NPH_FREE_ARRAY(b->alloc, f32, b->offline, (u64)b->offline_cap * b->channels);
        b->offline = NULL;
    }
}

// The bus volumes from their cvars, ramped by the mixer where they changed.
static void apply_volumes(AudioBridge *b)
{
    const EngineCvars *cv = &b->engine->cv;
    const NphCvar *const vars[NPH_AUDIO_SOURCE_BUS_COUNT + 1U] = {
        &cv->audio_volume_effects,  &cv->audio_volume_music, &cv->audio_volume_dialogue,
        &cv->audio_volume_ambience, &cv->audio_volume_ui,    &cv->audio_volume};
    for (u32 i = 0U; i <= NPH_AUDIO_SOURCE_BUS_COUNT; ++i) {
        const f32 v = finite_clamp((f32)nph_cvar_get_float(vars[i]), 0.0F, 4.0F, 1.0F);
        if (nph_f32_to_bits(v) == nph_f32_to_bits(b->volumes[i])) {
            continue;
        }
        b->volumes[i] = v;
        nph_audio_bus_set_volume(
            b->mixer, i < NPH_AUDIO_SOURCE_BUS_COUNT ? b->buses[i] : NPH_AUDIO_MASTER_BUS, v);
    }
}

// The seconds a frame's sound spans: its own duration, or, rendered
// offline in lockstep, a step's (so the motion, the smoothing, and the mix
// keep the same time as the simulation).
static f64 frame_seconds(const AudioBridge *b, f64 dt)
{
    const NphEngine *e = b->engine;
    if (b->mode == NPH_ENGINE_AUDIO_OFFLINE && nph_cvar_get_bool(&e->cv.lockstep)) {
        return nph_fixed_clock_step_seconds(&e->clock);
    }
    return dt > 0.0 && dt < 1.0 ? dt : 0.0;
}

// Offline: the frame's worth of the mix, rendered on this thread as the
// device's would be.
static void render_offline(AudioBridge *b, f64 seconds)
{
    const f64 want = seconds * (f64)b->rate + b->offline_carry;
    const f64 whole = floor(want);
    b->offline_carry = want - whole;
    u64 frames = (u64)whole;
    // A frame longer than the buffer renders its excess first (time goes
    // on), so the buffer ends with the frame's last second.
    while (frames > b->offline_cap) {
        const u32 n = (u32)nph_min_u64(frames - b->offline_cap, b->offline_cap);
        nph_audio_mixer_render(b->mixer, b->offline, n);
        frames -= n;
    }
    if (frames > 0U) {
        nph_audio_mixer_render(b->mixer, b->offline, (u32)frames);
    }
    b->offline_frames = (u32)frames;
}

// ---------------------------------------------------------------------------
// Sounds
// ---------------------------------------------------------------------------

// The audio module's numbers for a sound's pinned ones (asset/sound.h).
static NphResult map_codec(const NphSoundHeader *h, NphAudioCodec *codec,
                           NphAudioSampleFormat *format)
{
    *format = NPH_AUDIO_FORMAT_UNKNOWN;
    switch (h->codec) {
    case NPH_SOUND_CODEC_PCM:
        *codec = NPH_AUDIO_CODEC_PCM;
        switch (h->format) {
        case NPH_SOUND_FORMAT_S16:
            *format = NPH_AUDIO_FORMAT_S16;
            return NPH_RESULT_OK;
        case NPH_SOUND_FORMAT_S24:
            *format = NPH_AUDIO_FORMAT_S24;
            return NPH_RESULT_OK;
        case NPH_SOUND_FORMAT_S32:
            *format = NPH_AUDIO_FORMAT_S32;
            return NPH_RESULT_OK;
        case NPH_SOUND_FORMAT_F32:
            *format = NPH_AUDIO_FORMAT_F32;
            return NPH_RESULT_OK;
        default:
            return NPH_RESULT_UNSUPPORTED;
        }
    case NPH_SOUND_CODEC_ADPCM:
        *codec = NPH_AUDIO_CODEC_ADPCM;
        return NPH_RESULT_OK;
    case NPH_SOUND_CODEC_LOSSLESS:
        *codec = NPH_AUDIO_CODEC_LOSSLESS;
        return NPH_RESULT_OK;
    default:
        return NPH_RESULT_UNSUPPORTED;
    }
}

// A streamed sound's bytes, on the streamer's thread.
static NphResult stream_read(void *user, u64 offset, void *dst, u64 size)
{
    SoundFile *f = (SoundFile *)user;
    u64 got = 0U;
    const NphResult r = nph_vfs_read_at(&f->file, f->offset + offset, dst, size, &got);
    return r == NPH_RESULT_OK && got == size ? NPH_RESULT_OK : NPH_RESULT_IO_ERROR;
}

static void free_file(AudioBridge *b, SoundEntry *s)
{
    if (s->file != NULL) {
        nph_vfs_close(nph_engine_vfs(b->engine), &s->file->file);
        NPH_FREE_TYPE(b->alloc, SoundFile, s->file);
        s->file = NULL;
    }
    if (s->block_offsets != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, s->block_offsets, s->header.block_count);
        s->block_offsets = NULL;
    }
}

// A loaded sound made playable: a clip in memory, or its file opened for
// its streams, checked to be the file the asset was mapped from.
static NphResult make_sound(AudioBridge *b, SoundEntry *s, const NphAsset *asset)
{
    NphSoundView v;
    NPH_TRY(nph_sound_view(asset, &v));
    s->header = *v.header;
    s->data_size = v.size;
    NPH_TRY(map_codec(v.header, &s->codec, &s->format));
    if ((v.header->flags & NPH_SOUND_FLAG_STREAM) == 0U) {
        NphAudioClipDesc cd;
        memset(&cd, 0, sizeof cd);
        cd.codec = s->codec;
        cd.format = s->format;
        cd.channels = v.header->channels;
        cd.sample_rate = v.header->sample_rate;
        cd.frames = v.header->frames;
        cd.data = v.data;
        cd.size = v.size;
        cd.bits = v.header->bits;
        cd.block_frames = v.header->block_frames;
        cd.block_offsets = v.block_offsets;
        cd.block_count = v.header->block_count;
        cd.loop_start = v.header->loop_start;
        cd.loop_end = v.header->loop_end;
        // Copied: the asset's mapping may page out, and the audio thread must
        // never fault on it.
        return nph_audio_clip_create(b->mixer, &cd, &s->clip);
    }
    SoundFile *f = NPH_ALLOC_TYPE(b->alloc, SoundFile);
    if (f == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(f, 0, sizeof *f);
    NphResult r = nph_vfs_open(nph_engine_vfs(b->engine), asset->path, 0U, &f->file);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(b->alloc, SoundFile, f);
        return r;
    }
    s->file = f;
    f->offset = v.offset;
    // The file the VFS opens is the one the loader mapped: the same header,
    // GUID, content hash, and size (a mount shadowing it since, or a cook
    // writing it again, would not be).
    NphAssetHeader on_disk;
    u64 got = 0U;
    r = nph_vfs_read_at(&f->file, 0U, &on_disk, sizeof on_disk, &got);
    if (r == NPH_RESULT_OK &&
        (got != sizeof on_disk || memcmp(&on_disk, &asset->header, sizeof on_disk) != 0)) {
        r = NPH_RESULT_CORRUPT;
    }
    if (r == NPH_RESULT_OK && v.block_offsets != NULL) {
        s->block_offsets = NPH_ALLOC_ARRAY(b->alloc, u32, v.header->block_count);
        if (s->block_offsets == NULL) {
            r = NPH_RESULT_OUT_OF_MEMORY;
        } else {
            memcpy(s->block_offsets, v.block_offsets, (u64)v.header->block_count * sizeof(u32));
        }
    }
    if (r != NPH_RESULT_OK) {
        free_file(b, s);
    }
    return r;
}

// A sound loading: ready or failed when its asset is, the asset released
// either way (a clip copies it; a stream reads the file).
static void poll_sound(AudioBridge *b, u32 index)
{
    SoundEntry *s = &b->sounds[index];
    if (s->state != SOUND_LOADING) {
        return;
    }
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    const NphAssetState st = nph_asset_state(loader, s->asset);
    if (st == NPH_ASSET_STATE_LOADING) {
        return;
    }
    NphResult r = nph_asset_error(loader, s->asset);
    if (st == NPH_ASSET_STATE_READY) {
        r = make_sound(b, s, nph_asset_get(loader, s->asset));
    } else if (r == NPH_RESULT_OK) {
        r = NPH_RESULT_NOT_FOUND;
    }
    nph_asset_release(loader, s->asset);
    s->asset = NPH_ASSET_HANDLE_NULL;
    s->state = r == NPH_RESULT_OK ? SOUND_READY : SOUND_FAILED;
    if (r != NPH_RESULT_OK) {
        log_sound(s->guid, "cannot be played", r);
    }
}

static NphResult grow_sounds(AudioBridge *b)
{
    if (b->sound_count < b->sound_cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = nph_max_u32(32U, 2U * b->sound_cap);
    SoundEntry *grown = NPH_ALLOC_ARRAY(b->alloc, SoundEntry, cap);
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (b->sounds != NULL) {
        memcpy(grown, b->sounds, (u64)b->sound_count * sizeof *grown);
        NPH_FREE_ARRAY(b->alloc, SoundEntry, b->sounds, b->sound_cap);
    }
    b->sounds = grown;
    b->sound_cap = cap;
    return NPH_RESULT_OK;
}

// The entry of `guid`, loading it when it is new (`wait`: until it is
// loaded). NO_VOICE for the null GUID or when the table cannot grow.
static u32 sound_of(AudioBridge *b, NphGuid guid, bool wait)
{
    if (nph_guid_is_null(guid)) {
        return NO_VOICE;
    }
    const u32 *known = NPH_HASHMAP_FIND(&b->sound_index, u32, &guid);
    if (known != NULL) {
        return *known;
    }
    if (grow_sounds(b) != NPH_RESULT_OK) {
        return NO_VOICE;
    }
    const u32 index = b->sound_count;
    if (nph_hashmap_insert(&b->sound_index, &guid, &index) != NPH_RESULT_OK) {
        return NO_VOICE;
    }
    SoundEntry *s = &b->sounds[index];
    memset(s, 0, sizeof *s);
    s->guid = guid;
    b->sound_count += 1U;
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    const NphResult r = wait ? nph_asset_load_guid(loader, guid, &s->asset)
                             : nph_asset_load_guid_async(loader, guid, NULL, &s->asset);
    if (r != NPH_RESULT_OK) {
        s->state = SOUND_FAILED;
        log_sound(guid, "cannot be loaded", r);
        return index;
    }
    s->state = SOUND_LOADING;
    poll_sound(b, index); // a load that has finished (synchronous, or loaded before)
    return index;
}

// After play ends: every sound's clip released (at a later update), its
// file closed, its table emptied. The streams that read the files are
// closed by then (end_play waits).
static void release_sounds(AudioBridge *b)
{
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    for (u32 i = 0U; i < b->sound_count; ++i) {
        SoundEntry *s = &b->sounds[i];
        if (!nph_asset_handle_is_null(s->asset)) {
            nph_asset_release(loader, s->asset);
        }
        if (b->mixer != NULL) {
            nph_audio_clip_destroy(b->mixer, s->clip); // a null clip is ignored
        }
        free_file(b, s);
    }
    b->sound_count = 0U;
    nph_hashmap_clear(&b->sound_index);
}

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

static NphResult grow_voices(AudioBridge *b)
{
    if (b->voice_count < b->voice_cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = nph_max_u32(64U, 2U * b->voice_cap);
    VoiceRecord *grown = NPH_ALLOC_ARRAY(b->alloc, VoiceRecord, cap);
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (b->voices != NULL) {
        memcpy(grown, b->voices, (u64)b->voice_count * sizeof *grown);
        NPH_FREE_ARRAY(b->alloc, VoiceRecord, b->voices, b->voice_cap);
    }
    b->voices = grown;
    b->voice_cap = cap;
    return NPH_RESULT_OK;
}

// The voice of an entity's AudioSource, or NO_VOICE.
static u32 voice_of(const AudioBridge *b, NphEntity e)
{
    if (b->slot_voice == NULL || nph_entity_is_null(e)) {
        return NO_VOICE;
    }
    const u32 slot = nph_world_handle_slot(b->world, e);
    if (slot >= b->slot_cap) {
        return NO_VOICE;
    }
    const u32 index = b->slot_voice[slot];
    return index != NO_VOICE && !b->voices[index].oneshot &&
                   b->voices[index].entity.value == e.value
               ? index
               : NO_VOICE;
}

// The fall-off of a source (spatial.h), its curve made to rise.
static NphAudioAttenuation attenuation_of(const NphAudioSource *src)
{
    NphAudioAttenuation a = nph_audio_attenuation_default();
    a.rolloff = src->rolloff < (u32)NPH_AUDIO_ROLLOFF_COUNT ? (NphAudioRolloff)src->rolloff
                                                            : NPH_AUDIO_ROLLOFF_INVERSE;
    a.min_distance = finite_clamp(src->min_distance, 0.01F, 100000.0F, 1.0F);
    a.max_distance = finite_clamp(src->max_distance, a.min_distance, 100000.0F, 50.0F);
    a.factor = finite_clamp(src->rolloff_factor, 0.0F, 10.0F, 1.0F);
    const f32 d[4] = {src->curve_distances.x, src->curve_distances.y, src->curve_distances.z,
                      src->curve_distances.w};
    const f32 g[4] = {src->curve_gains.x, src->curve_gains.y, src->curve_gains.z,
                      src->curve_gains.w};
    a.point_count = 4U;
    f32 previous = 0.0F;
    for (u32 i = 0U; i < 4U; ++i) {
        previous = finite_clamp(d[i], previous, 100000.0F, previous);
        a.points[i][0] = previous;
        a.points[i][1] = finite_clamp(g[i], -120.0F, 24.0F, 0.0F);
    }
    return a;
}

// Where a voice's source is in the world now: its entity's world position,
// or, for a one-shot that follows none, where it was played.
static NphDVec3 source_position(const AudioBridge *b, const VoiceRecord *v)
{
    if (!nph_entity_is_null(v->entity)) {
        const NphWorldTransform *wt =
            NPH_WORLD_GET(b->world, v->entity, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
        if (wt != NULL) {
            return wt->translation;
        }
    }
    return v->position;
}

static NphAudioPlayDesc play_desc(const AudioBridge *b, const VoiceRecord *v, NphAudioClip clip)
{
    const NphAudioSource *src = &v->source;
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.bus = b->buses[src->bus < NPH_AUDIO_SOURCE_BUS_COUNT ? src->bus : 0U];
    d.volume = finite_clamp(src->volume, 0.0F, 4.0F, 1.0F);
    d.pitch = finite_clamp(src->pitch, 0.125F, 8.0F, 1.0F);
    d.priority = (u8)nph_min_u32(src->priority, 255U);
    d.loop = src->loop; // a stream's clip loops itself (its description)
    if (nph_cvar_get_bool(&b->engine->cv.audio_reverb)) {
        d.sends[0] = b->reverb_bus;
        d.send_gains[0] = finite_clamp(src->reverb, 0.0F, 1.0F, 0.0F);
    }
    d.spatial = src->spatial;
    if (src->spatial) {
        d.position = to_listener(b, nph_dvec3_sub(source_position(b, v), b->listener_position));
        d.velocity = nph_vec3_zero();
        d.attenuation = attenuation_of(src);
        d.size = finite_clamp(src->size, 0.0F, 100.0F, 0.0F);
        d.doppler = finite_clamp(src->doppler, 0.0F, 4.0F, 1.0F);
    }
    return d;
}

static void write_playing(AudioBridge *b, NphEntity e, bool playing)
{
    if (nph_entity_is_null(e) || !nph_world_is_alive(b->world, e)) {
        return;
    }
    const NphRuntimeComponents *ids = &b->engine->ids;
    const NphAudioSource *now = NPH_WORLD_GET(b->world, e, ids->audio_source, NphAudioSource);
    if (now == NULL || (now->playing != 0) == playing) {
        return;
    }
    NphAudioSource *src = NPH_WORLD_GET_MUT(b->world, e, ids->audio_source, NphAudioSource);
    src->playing = playing ? 1 : 0;
}

// Takes record `index` out (the last moves into its place).
static void remove_voice(AudioBridge *b, u32 index)
{
    NPH_ASSERT(index < b->voice_count);
    VoiceRecord *v = &b->voices[index];
    if (!v->oneshot && !nph_entity_is_null(v->entity)) {
        const u32 slot = nph_world_handle_slot(b->world, v->entity);
        if (slot < b->slot_cap && b->slot_voice[slot] == index) {
            b->slot_voice[slot] = NO_VOICE;
        }
    }
    const u32 last = b->voice_count - 1U;
    if (index != last) {
        b->voices[index] = b->voices[last];
        const VoiceRecord *moved = &b->voices[index];
        if (!moved->oneshot && !nph_entity_is_null(moved->entity)) {
            const u32 slot = nph_world_handle_slot(b->world, moved->entity);
            if (slot < b->slot_cap && b->slot_voice[slot] == last) {
                b->slot_voice[slot] = index;
            }
        }
    }
    b->voice_count = last;
}

// Stops a voice (fading over `fade` seconds) and drops its record: its
// stream closes when the audio thread lets go.
static void stop_voice(AudioBridge *b, u32 index, f32 fade)
{
    VoiceRecord *v = &b->voices[index];
    if (v->state == VOICE_PLAYING) {
        nph_audio_voice_stop(b->mixer, v->voice, fade);
    }
    nph_audio_clip_destroy(b->mixer, v->stream_clip); // a null clip is ignored
    remove_voice(b, index);
}

// A voice that will not play (its sound failed, the mixer is full): its
// source says so.
static void give_up(AudioBridge *b, u32 index)
{
    if (!b->voices[index].oneshot) {
        write_playing(b, b->voices[index].entity, false);
    }
    stop_voice(b, index, 0.0F);
}

// A new record for an entity's AudioSource or a one-shot, waiting for its
// sound. NO_VOICE when the table cannot grow.
static u32 add_voice(AudioBridge *b, NphEntity e, bool oneshot, const NphAudioSource *src)
{
    if (grow_voices(b) != NPH_RESULT_OK) {
        return NO_VOICE;
    }
    const u32 index = b->voice_count;
    VoiceRecord *v = &b->voices[index];
    memset(v, 0, sizeof *v);
    v->entity = e;
    v->oneshot = oneshot;
    v->source = *src;
    v->sound = sound_of(b, src->sound, false);
    v->state = VOICE_WAITING;
    v->since_ns = nph_time_now_ns();
    b->voice_count += 1U;
    if (!oneshot && !nph_entity_is_null(e)) {
        const u32 slot = nph_world_handle_slot(b->world, e);
        if (slot < b->slot_cap) {
            b->slot_voice[slot] = index;
        }
    }
    return index;
}

// ---------------------------------------------------------------------------
// Occlusion
// ---------------------------------------------------------------------------

// The point `line` (from the listener) reaches, stopped short of its end by
// OCCLUSION_END_MARGIN.
static NphVec3 short_of_end(NphVec3 line)
{
    const f32 length = nph_vec3_length(line);
    if (!(length > 0.0F)) {
        return line;
    }
    const f32 margin = nph_f32_min(OCCLUSION_END_MARGIN, 0.25F * length);
    return nph_vec3_scale(line, (length - margin) / length);
}

// The share of a spatial source at `at` hidden from the listener: of its
// centre and the four points around it (at its size or 30 cm) that the
// centre sees, those whose sight lines something blocks. Up to
// OCCLUSION_RAYS rays, counted in the frame's.
static f32 cast_occlusion(AudioBridge *b, const NphPhysicsWorld *world, const VoiceRecord *v,
                          NphDVec3 at)
{
    const NphVec3 to = nph_dvec3_to_vec3(nph_dvec3_sub(at, b->listener_position));
    const f32 length = nph_vec3_length(to);
    if (!(length > 0.01F)) {
        return 0.0F;
    }
    const NphVec3 dir = nph_vec3_scale(to, 1.0F / length);
    // Two directions across the sight line.
    const NphVec3 helper = fabsf(dir.y) < 0.9F ? nph_vec3_unit_y() : nph_vec3_unit_x();
    const NphVec3 across = nph_vec3_normalize_or(nph_vec3_cross(dir, helper), nph_vec3_unit_x());
    const NphVec3 other = nph_vec3_cross(dir, across);
    const f32 r = nph_f32_max(finite_clamp(v->source.size, 0.0F, 100.0F, 0.0F), 0.3F);
    const NphVec3 offsets[OCCLUSION_POINTS] = {nph_vec3_zero(), nph_vec3_scale(across, r),
                                               nph_vec3_scale(across, -r), nph_vec3_scale(other, r),
                                               nph_vec3_scale(other, -r)};
    NphPhysicsQueryFilter filter = nph_physics_query_filter_default();
    if (!nph_entity_is_null(v->entity)) {
        filter.ignore = nph_engine_physics_body(b->engine, v->entity); // its own body
    }
    // The probes start where the centre's sight line ends, so a source on a
    // surface probes from just off it.
    const NphVec3 centre = short_of_end(to);
    const NphDVec3 probe_origin = nph_dvec3_add_vec3(b->listener_position, centre);
    u32 seen = 0U;
    u32 blocked = 0U;
    for (u32 k = 0U; k < OCCLUSION_POINTS; ++k) {
        NphPhysicsQueryHit hit;
        if (k > 0U) {
            const NphVec3 probe = nph_vec3_add(nph_vec3_sub(to, centre), offsets[k]);
            b->occlusion_rays += 1U;
            if (nph_physics_world_raycast_closest(world, probe_origin, probe, &filter, &hit)) {
                continue; // not part of the source
            }
        }
        seen += 1U;
        b->occlusion_rays += 1U;
        if (nph_physics_world_raycast_closest(world, b->listener_position,
                                              short_of_end(nph_vec3_add(to, offsets[k])), &filter,
                                              &hit)) {
            blocked += 1U;
        }
    }
    return (f32)blocked / (f32)seen;
}

// The frame's occlusion budget (audio.occlusion_rays) still holds a source's
// rays.
static bool rays_left(const AudioBridge *b)
{
    return b->occlusion_rays + OCCLUSION_RAYS <=
           (u32)nph_cvar_get_int(&b->engine->cv.audio_occlusion_rays);
}

// A playing spatial voice whose source asks for occlusion.
static bool occludes(const VoiceRecord *v)
{
    return v->state == VOICE_PLAYING && v->spatial && v->source.occlusion != 0;
}

// A voice about to start: when it is spatial and asks for occlusion, its
// rays are cast at once, ahead of the frame's round, and it starts as
// hidden as it is (`d`'s occlusion): a sound that begins behind a wall must
// not be bright through its first 80 ms, its attack. Past the frame's
// budget it starts open, and the next frame casts it first.
static void start_occlusion(AudioBridge *b, VoiceRecord *v, NphAudioPlayDesc *d)
{
    v->occlusion = 0.0F;
    v->occlusion_target = 0.0F;
    v->occlusion_known = !d->spatial || v->source.occlusion == 0; // nothing to find
    const NphPhysicsWorld *world = nph_engine_physics(b->engine);
    if (v->occlusion_known || world == NULL || !rays_left(b)) {
        return;
    }
    const f32 o = cast_occlusion(b, world, v, source_position(b, v));
    v->occlusion = o;
    v->occlusion_target = o;
    v->occlusion_known = 1;
    d->occlusion = o;
}

// A waiting voice, once its sound has loaded: playing at once from memory,
// or its stream made, for the streamer to fill (defer_fill).
static void start_voice(AudioBridge *b, u32 index)
{
    VoiceRecord *v = &b->voices[index];
    if (v->sound == NO_VOICE) {
        give_up(b, index);
        return;
    }
    poll_sound(b, v->sound);
    const SoundEntry *s = &b->sounds[v->sound];
    if (s->state == SOUND_LOADING) {
        if (v->oneshot && nph_time_now_ns() - v->since_ns > ONESHOT_WAIT_NS) {
            give_up(b, index); // late is worse than not at all for a one-shot
        }
        return;
    }
    if (s->state == SOUND_FAILED) {
        give_up(b, index);
        return;
    }
    if (s->file == NULL) {
        NphAudioPlayDesc d = play_desc(b, v, s->clip);
        start_occlusion(b, v, &d);
        if (nph_audio_voice_play(b->mixer, &d, &v->voice) != NPH_RESULT_OK) {
            give_up(b, index); // every voice slot taken, or the command ring full
            return;
        }
        v->state = VOICE_PLAYING;
        v->spatial = d.spatial;
        return;
    }
    NphAudioStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.source = (NphAudioStreamSource){stream_read, s->file};
    sd.codec = s->codec;
    sd.format = s->format;
    sd.channels = s->header.channels;
    sd.sample_rate = s->header.sample_rate;
    sd.frames = s->header.frames;
    sd.size = s->data_size;
    sd.bits = s->header.bits;
    sd.block_frames = s->header.block_frames;
    sd.block_offsets = s->block_offsets;
    sd.block_count = s->header.block_count;
    sd.loop_start = s->header.loop_start;
    sd.loop_end = s->header.loop_end;
    sd.loop = v->source.loop;
    sd.buffer_seconds = (f32)nph_cvar_get_float(&b->engine->cv.audio_stream_seconds);
    sd.defer_fill = 1;
    const NphResult r = nph_audio_clip_create_stream(b->mixer, b->streamer, &sd, &v->stream_clip);
    if (r == NPH_RESULT_FULL) {
        return; // every stream slot taken: the next frame tries again
    }
    if (r != NPH_RESULT_OK) {
        log_sound(s->guid, "cannot stream", r);
        give_up(b, index);
        return;
    }
    v->state = VOICE_PRIMING;
    v->since_ns = nph_time_now_ns();
}

// A priming voice starts once its ring holds a quarter second (or all of a
// shorter sound), or when the streamer has taken too long (it starts dry,
// counted as starvation).
static void prime_voice(AudioBridge *b, u32 index)
{
    VoiceRecord *v = &b->voices[index];
    const SoundEntry *s = &b->sounds[v->sound];
    const u64 need = nph_min_u64(s->header.sample_rate / 4U, s->header.frames);
    if (nph_audio_clip_stream_ahead(b->mixer, v->stream_clip) < need &&
        nph_time_now_ns() - v->since_ns < PRIME_WAIT_NS) {
        return;
    }
    NphAudioPlayDesc d = play_desc(b, v, v->stream_clip);
    start_occlusion(b, v, &d);
    if (nph_audio_voice_play(b->mixer, &d, &v->voice) != NPH_RESULT_OK) {
        give_up(b, index);
        return;
    }
    v->state = VOICE_PLAYING;
    v->spatial = d.spatial;
}

// The fields of a voice's AudioSource that changed: volume, pitch, the
// reverberation's send, and occlusion apply to it at once; a new sound plays
// again; the rest (looping, bus, spatial, fall-off, size, Doppler,
// priority), which a mixer voice keeps from its start, apply when it next
// plays.
static void apply_source(AudioBridge *b, u32 index, const NphAudioSource *now)
{
    VoiceRecord *v = &b->voices[index];
    const NphAudioSource was = v->source;
    if (!nph_guid_equals(now->sound, was.sound)) {
        const NphEntity e = v->entity;
        stop_voice(b, index, 0.05F);
        (void)add_voice(b, e, false, now);
        return;
    }
    v->source = *now;
    if (v->state != VOICE_PLAYING) {
        return; // its voice starts with them
    }
    if (nph_f32_to_bits(now->volume) != nph_f32_to_bits(was.volume)) {
        nph_audio_voice_set_volume(b->mixer, v->voice, finite_clamp(now->volume, 0.0F, 4.0F, 1.0F));
    }
    if (nph_f32_to_bits(now->pitch) != nph_f32_to_bits(was.pitch)) {
        nph_audio_voice_set_pitch(b->mixer, v->voice, finite_clamp(now->pitch, 0.125F, 8.0F, 1.0F));
    }
    if (nph_f32_to_bits(now->reverb) != nph_f32_to_bits(was.reverb) &&
        nph_cvar_get_bool(&b->engine->cv.audio_reverb)) {
        nph_audio_voice_set_send(b->mixer, v->voice, 0U,
                                 finite_clamp(now->reverb, 0.0F, 1.0F, 0.0F));
    }
    if (now->occlusion == 0 && was.occlusion != 0 && v->spatial) {
        v->occlusion = 0.0F;
        v->occlusion_target = 0.0F;
        nph_audio_voice_set_occlusion(b->mixer, v->voice, 0.0F, 0.0F);
    }
}

// ---------------------------------------------------------------------------
// The world
// ---------------------------------------------------------------------------

static void destroy_queries(AudioBridge *b)
{
    nph_query_destroy(b->source_query);
    nph_query_destroy(b->listener_query);
    nph_query_destroy(b->camera_query);
    b->source_query = NULL;
    b->listener_query = NULL;
    b->camera_query = NULL;
}

static NphResult make_queries(AudioBridge *b)
{
    const NphRuntimeComponents *ids = &b->engine->ids;
    const NphQueryTerm source_terms[1] = {nph_term_changed(nph_term_read(ids->audio_source))};
    const NphQueryTerm listener_terms[2] = {nph_term_read(ids->audio_listener),
                                            nph_term_read(NPH_COMPONENT_ID_WORLD_TRANSFORM)};
    const NphQueryTerm camera_terms[2] = {nph_term_read(ids->camera),
                                          nph_term_read(NPH_COMPONENT_ID_WORLD_TRANSFORM)};
    const NphQueryDesc sq = {source_terms, 1U};
    const NphQueryDesc lq = {listener_terms, 2U};
    const NphQueryDesc cq = {camera_terms, 2U};
    NphResult r = nph_query_create(b->world, &sq, &b->source_query);
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &lq, &b->listener_query);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &cq, &b->camera_query);
    }
    if (r != NPH_RESULT_OK) {
        destroy_queries(b);
    }
    return r;
}

// The slot tables cover the world's capacity.
static NphResult size_slots(AudioBridge *b)
{
    const u32 cap = nph_world_entity_capacity(b->world);
    if (cap <= b->slot_cap) {
        return NPH_RESULT_OK;
    }
    u32 *voice = NPH_ALLOC_ARRAY(b->alloc, u32, cap);
    u32 *seen = NPH_ALLOC_ARRAY(b->alloc, u32, cap);
    if (voice == NULL || seen == NULL) {
        if (voice != NULL) {
            NPH_FREE_ARRAY(b->alloc, u32, voice, cap);
        }
        if (seen != NULL) {
            NPH_FREE_ARRAY(b->alloc, u32, seen, cap);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0U; i < cap; ++i) {
        voice[i] = i < b->slot_cap ? b->slot_voice[i] : NO_VOICE;
        seen[i] = i < b->slot_cap ? b->slot_seen[i] : 0U;
    }
    if (b->slot_voice != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_voice, b->slot_cap);
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_seen, b->slot_cap);
    }
    b->slot_voice = voice;
    b->slot_seen = seen;
    b->slot_cap = cap;
    return NPH_RESULT_OK;
}

// The voices of entities that went, or lost their AudioSource, stopped; and
// every voice checked when the stream dropped events.
static void read_events(AudioBridge *b)
{
    u32 count = 0U;
    u32 dropped = 0U;
    const NphWorldEvent *events = nph_world_events(b->world, &count, &dropped);
    const NphComponentId source = b->engine->ids.audio_source;
    for (u32 i = 0U; i < count; ++i) {
        const NphWorldEvent *ev = &events[i];
        const bool gone = ev->type == NPH_WORLD_EVENT_DESPAWNED ||
                          (ev->type == NPH_WORLD_EVENT_REMOVED && ev->component == source);
        if (!gone) {
            continue;
        }
        const u32 index = voice_of(b, ev->entity);
        if (index != NO_VOICE) {
            stop_voice(b, index, 0.05F);
        }
        // Seen again (a component added back) is a new source: it may play
        // by itself again.
        const u32 slot = nph_world_handle_slot(b->world, ev->entity);
        if (slot < b->slot_cap && b->slot_seen[slot] == ev->entity.value) {
            b->slot_seen[slot] = 0U;
        }
    }
    if (dropped == b->events_dropped) {
        return;
    }
    b->events_dropped = dropped;
    for (u32 i = b->voice_count; i-- > 0U;) {
        const VoiceRecord *v = &b->voices[i];
        if (!v->oneshot && (!nph_world_is_alive(b->world, v->entity) ||
                            !nph_world_has(b->world, v->entity, source))) {
            stop_voice(b, i, 0.05F);
        }
    }
    b->full_pass = 1;
}

// An AudioSource of a changed chunk: its voice started, stopped, or brought
// up to date; one seen for the first time plays by itself with `autoplay`.
static void visit(AudioBridge *b, NphEntity e, const NphAudioSource *src, NphArrayU32 *autoplay)
{
    const u32 slot = nph_world_handle_slot(b->world, e);
    if (slot >= b->slot_cap) {
        return;
    }
    const bool fresh = b->slot_seen[slot] != e.value;
    b->slot_seen[slot] = e.value;
    const u32 index = voice_of(b, e);
    if (index != NO_VOICE) {
        if (src->playing == 0) {
            stop_voice(b, index, 0.05F);
        } else {
            apply_source(b, index, src);
        }
        return;
    }
    if (src->playing != 0) {
        (void)add_voice(b, e, false, src);
    } else if (fresh && src->autoplay != 0) {
        // Written after the walk: the chunk is being read.
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(autoplay, e.value));
    }
}

static void change_pass(AudioBridge *b, u32 since, u32 now)
{
    NphArrayU32 autoplay;
    NPH_ARRAY_INIT(&autoplay, b->alloc);
    const bool full = b->full_pass != 0;
    b->full_pass = 0;
    NphQueryIter it;
    NphChunkView v;
    if (full) {
        nph_query_iter(b->source_query, &it);
    } else {
        nph_query_iter_since(b->source_query, since, now, &it);
    }
    while (nph_query_next(&it, &v)) {
        const NphAudioSource *src = NPH_CHUNK_COLUMN(&v, 0, NphAudioSource);
        for (u32 i = 0U; i < v.count; ++i) {
            visit(b, v.entities[i], &src[i], &autoplay);
        }
    }
    for (u32 i = 0U; i < autoplay.len; ++i) {
        const NphEntity e = {autoplay.data[i]};
        const NphAudioSource *src =
            NPH_WORLD_GET(b->world, e, b->engine->ids.audio_source, NphAudioSource);
        if (src != NULL && add_voice(b, e, false, src) != NO_VOICE) {
            write_playing(b, e, true);
            b->voices[b->voice_count - 1U].source.playing = 1;
        }
    }
    NPH_ARRAY_FREE(&autoplay);
}

// The listener: the first active AudioListener, else the host's view (the
// editor's camera), else the first active Camera, else the origin.
static void find_listener(AudioBridge *b, f64 dt)
{
    NphDVec3 position = nph_dvec3_zero();
    NphVec3 axes[3] = {nph_vec3_unit_x(), nph_vec3_unit_y(), nph_vec3_unit_z()};
    bool found = false;
    NphQueryIter it;
    NphChunkView v;
    nph_query_iter(b->listener_query, &it);
    while (!found && nph_query_next(&it, &v)) {
        const NphAudioListener *l = NPH_CHUNK_COLUMN(&v, 0, NphAudioListener);
        const NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 1, NphWorldTransform);
        for (u32 i = 0U; i < v.count && !found; ++i) {
            if (l[i].active != 0) {
                position = wt[i].translation;
                axes[0] = wt[i].x_axis;
                axes[1] = wt[i].y_axis;
                axes[2] = wt[i].z_axis;
                found = true;
            }
        }
    }
    const NphEngine *e = b->engine;
    if (!found && e->has_view_override) {
        const NphRenderCamera *c = &e->view_override.camera;
        position = c->position;
        axes[0] = nph_quat_rotate(c->orientation, nph_vec3_unit_x());
        axes[1] = nph_quat_rotate(c->orientation, nph_vec3_unit_y());
        axes[2] = nph_quat_rotate(c->orientation, nph_vec3_unit_z());
        found = true;
    }
    if (!found) {
        nph_query_iter(b->camera_query, &it);
        while (!found && nph_query_next(&it, &v)) {
            const NphCamera *c = NPH_CHUNK_COLUMN(&v, 0, NphCamera);
            const NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 1, NphWorldTransform);
            for (u32 i = 0U; i < v.count && !found; ++i) {
                if (c[i].active != 0) {
                    position = wt[i].translation;
                    axes[0] = wt[i].x_axis;
                    axes[1] = wt[i].y_axis;
                    axes[2] = wt[i].z_axis;
                    found = true;
                }
            }
        }
    }
    // Unit axes: the listener is not scaled.
    b->listener_axes[0] = nph_vec3_normalize_or(axes[0], nph_vec3_unit_x());
    b->listener_axes[1] = nph_vec3_normalize_or(axes[1], nph_vec3_unit_y());
    b->listener_axes[2] = nph_vec3_normalize_or(axes[2], nph_vec3_unit_z());
    NphVec3 velocity = nph_vec3_zero();
    if (b->listener_placed && dt > 0.0) {
        velocity = nph_vec3_scale(nph_dvec3_to_vec3(nph_dvec3_sub(position, b->listener_position)),
                                  (f32)(1.0 / dt));
    }
    const f32 k = (f32)(1.0 - exp(-dt / VELOCITY_SMOOTHING));
    b->listener_velocity = nph_vec3_add(
        b->listener_velocity, nph_vec3_scale(nph_vec3_sub(velocity, b->listener_velocity), k));
    b->listener_position = position;
    b->listener_placed = 1;
}

// Every spatial voice where its source is, in the listener's frame, moving
// as it moved since the last frame (Doppler).
static void place_voices(AudioBridge *b, f64 dt)
{
    const f32 k = (f32)(1.0 - exp(-dt / VELOCITY_SMOOTHING));
    for (u32 i = 0U; i < b->voice_count; ++i) {
        VoiceRecord *v = &b->voices[i];
        if (v->state != VOICE_PLAYING || !v->spatial) {
            continue;
        }
        const NphDVec3 p = source_position(b, v);
        NphVec3 velocity = nph_vec3_zero();
        if (v->placed && dt > 0.0 && !(!nph_entity_is_null(v->entity) && was_reset(b, v->entity))) {
            velocity =
                nph_vec3_scale(nph_dvec3_to_vec3(nph_dvec3_sub(p, v->position)), (f32)(1.0 / dt));
        }
        v->velocity =
            nph_vec3_add(v->velocity, nph_vec3_scale(nph_vec3_sub(velocity, v->velocity), k));
        v->position = p;
        v->placed = 1;
        nph_audio_voice_set_position(
            b->mixer, v->voice, to_listener(b, nph_dvec3_sub(p, b->listener_position)),
            vec_to_listener(b, nph_vec3_sub(v->velocity, b->listener_velocity)));
    }
}

// What lies between the listener and each spatial source that asks
// (cast_occlusion), within the frame's budget of audio.occlusion_rays: the
// voices that started past an earlier frame's budget first, taken as they
// are; then round the rest, the next frame starting where this one stopped.
// Every frame each such voice's occlusion moves towards what its last rays
// found.
static void occlude(AudioBridge *b, f64 dt)
{
    const NphPhysicsWorld *world = nph_engine_physics(b->engine);
    const u32 n = b->voice_count;
    for (u32 i = 0U; world != NULL && i < n && rays_left(b); ++i) {
        VoiceRecord *v = &b->voices[i];
        if (occludes(v) && !v->occlusion_known) {
            v->occlusion = cast_occlusion(b, world, v, v->position);
            v->occlusion_target = v->occlusion;
            v->occlusion_known = 1;
            nph_audio_voice_set_occlusion(b->mixer, v->voice, v->occlusion, 0.0F);
        }
    }
    u32 step = 0U;
    for (; world != NULL && step < n && rays_left(b); ++step) {
        VoiceRecord *v = &b->voices[(b->occlusion_cursor + step) % n];
        if (occludes(v)) {
            v->occlusion_target = cast_occlusion(b, world, v, v->position);
        }
    }
    b->occlusion_cursor = n > 0U ? (b->occlusion_cursor + step) % n : 0U;
    const f32 k = (f32)(1.0 - exp(-dt / OCCLUSION_SMOOTHING));
    for (u32 i = 0U; i < n; ++i) {
        VoiceRecord *v = &b->voices[i];
        if (!occludes(v)) {
            continue;
        }
        const f32 next = v->occlusion + (v->occlusion_target - v->occlusion) * k;
        if (fabsf(next - v->occlusion) > 1e-4F || (next == 0.0F) != (v->occlusion == 0.0F)) {
            v->occlusion = next;
            nph_audio_voice_set_occlusion(b->mixer, v->voice, next, 0.0F);
        }
    }
}

// The one-shots queued since the last frame, as voices of their own.
static void take_oneshots(AudioBridge *b)
{
    OneShot o;
    while (nph_mpmc_ring_pop(&b->oneshots, &o)) {
        NphAudioSource src = *(const NphAudioSource *)NphAudioSource_component()->defaults;
        src.sound = o.sound;
        src.bus = o.desc.bus;
        src.volume = o.desc.volume;
        src.pitch = o.desc.pitch;
        src.playing = 1;
        src.spatial = o.desc.spatial;
        src.min_distance = o.desc.min_distance;
        src.max_distance = o.desc.max_distance;
        src.reverb = o.desc.reverb;
        src.occlusion = o.desc.occlusion;
        src.priority = o.desc.priority;
        const u32 index = add_voice(b, o.desc.follow, true, &src);
        if (index != NO_VOICE) {
            b->voices[index].position = o.desc.position;
        }
    }
}

// Waiting and priming voices started when they can; ended voices dropped,
// their sources told.
static void advance_voices(AudioBridge *b)
{
    for (u32 i = b->voice_count; i-- > 0U;) {
        VoiceRecord *v = &b->voices[i];
        if (v->state == VOICE_WAITING) {
            start_voice(b, i);
        } else if (v->state == VOICE_PRIMING) {
            prime_voice(b, i);
        } else if (!nph_audio_voice_is_playing(b->mixer, v->voice)) {
            give_up(b, i); // it ended: its source no longer plays
        }
    }
}

// ---------------------------------------------------------------------------
// The bridge
// ---------------------------------------------------------------------------

NphResult audio_bridge_create(NphEngine *e, AudioBridge **out)
{
    NPH_ASSERT(e != NULL && out != NULL);
    *out = NULL;
    NphAllocator *alloc = engine_alloc(e, ENGINE_ALLOC_AUDIO);
    AudioBridge *b = NPH_ALLOC_TYPE(alloc, AudioBridge);
    if (b == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(b, 0, sizeof *b);
    b->engine = e;
    b->alloc = alloc;
    b->mode = mode_of(e);
    NphHashMapDesc hd;
    memset(&hd, 0, sizeof hd);
    hd.key_size = (u32)sizeof(NphGuid);
    hd.val_size = (u32)sizeof(u32);
    NphResult r = nph_hashmap_init(&b->sound_index, alloc, &hd);
    if (r == NPH_RESULT_OK) {
        r = nph_mpmc_ring_init(&b->oneshots, alloc, (u32)sizeof(OneShot), ONESHOT_CAPACITY);
    }
    if (r == NPH_RESULT_OK && b->mode != NPH_ENGINE_AUDIO_OFF) {
        r = start_output(b);
    }
    if (r != NPH_RESULT_OK) {
        audio_bridge_destroy(b);
        return r;
    }
    *out = b;
    return NPH_RESULT_OK;
}

void audio_bridge_destroy(AudioBridge *b)
{
    if (b == NULL) {
        return;
    }
    audio_bridge_end(b);
    destroy_queries(b);
    stop_output(b);
    if (b->voices != NULL) {
        NPH_FREE_ARRAY(b->alloc, VoiceRecord, b->voices, b->voice_cap);
    }
    if (b->sounds != NULL) {
        NPH_FREE_ARRAY(b->alloc, SoundEntry, b->sounds, b->sound_cap);
    }
    if (b->slot_voice != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_voice, b->slot_cap);
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_seen, b->slot_cap);
    }
    nph_hashmap_free(&b->sound_index);
    nph_mpmc_ring_free(&b->oneshots);
    NPH_FREE_TYPE(b->alloc, AudioBridge, b);
}

NphResult audio_bridge_set_world(AudioBridge *b, NphWorld *world)
{
    NPH_ASSERT(b != NULL);
    destroy_queries(b);
    const bool same_slots = b->world != NULL && world != NULL;
    b->world = world;
    if (world == NULL || b->mixer == NULL) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(make_queries(b));
    if (b->slot_cap != 0U && !same_slots) {
        for (u32 i = 0U; i < b->slot_cap; ++i) {
            b->slot_seen[i] = 0U;
        }
    }
    NPH_TRY(size_slots(b));
    // A world rebuilt with the same handles (a reload's migration, the play
    // copy) keeps its voices; the next pass compares every source with them.
    for (u32 i = b->voice_count; i-- > 0U;) {
        const VoiceRecord *v = &b->voices[i];
        if (!v->oneshot && (!nph_world_is_alive(world, v->entity) ||
                            !nph_world_has(world, v->entity, b->engine->ids.audio_source))) {
            stop_voice(b, i, 0.05F);
        }
    }
    b->last_tick = nph_world_tick(world);
    b->full_pass = 1;
    return NPH_RESULT_OK;
}

NphResult audio_bridge_begin(AudioBridge *b)
{
    NPH_ASSERT(b != NULL);
    if (b->mixer == NULL || b->world == NULL) {
        return NPH_RESULT_OK;
    }
    b->playing = 1;
    b->listener_placed = 0;
    NPH_TRY(size_slots(b));
    // The world's sounds loaded before the first frame, so what plays by
    // itself starts on time.
    NphQueryIter it;
    NphChunkView v;
    nph_query_iter(b->source_query, &it);
    while (nph_query_next(&it, &v)) {
        const NphAudioSource *src = NPH_CHUNK_COLUMN(&v, 0, NphAudioSource);
        for (u32 i = 0U; i < v.count; ++i) {
            (void)sound_of(b, src[i].sound, true);
        }
    }
    b->full_pass = 1;
    return NPH_RESULT_OK;
}

void audio_bridge_end(AudioBridge *b)
{
    NPH_ASSERT(b != NULL);
    if (!b->playing) {
        return;
    }
    b->playing = 0;
    for (u32 i = b->voice_count; i-- > 0U;) {
        stop_voice(b, i, 0.0F);
    }
    // Hand every stream's clip back before the sounds' files close: the
    // audio thread lets go of the clips, the updates release them, and the
    // streamer frees their streams (a second at most; offline, the bridge
    // renders the blocks itself).
    const u64 t0 = nph_time_now_ns();
    for (;;) {
        nph_audio_mixer_update(b->mixer);
        NphAudioStreamerStats ss;
        nph_audio_streamer_stats(b->streamer, &ss);
        if (ss.streams == 0U) {
            break;
        }
        if (nph_time_now_ns() - t0 > 1000ULL * 1000000ULL) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME, "audio: %u streams still open at the end of play",
                         ss.streams);
            break;
        }
        if (b->mode == NPH_ENGINE_AUDIO_OFFLINE) {
            nph_audio_mixer_render(b->mixer, b->offline, NPH_AUDIO_BLOCK_FRAMES);
        } else {
            nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
        }
    }
    release_sounds(b);
    // What the next play sees is new again.
    for (u32 i = 0U; i < b->slot_cap; ++i) {
        b->slot_seen[i] = 0U;
    }
    OneShot drop;
    while (nph_mpmc_ring_pop(&b->oneshots, &drop)) {
    }
}

void audio_bridge_sync(AudioBridge *b)
{
    NPH_ASSERT(b != NULL);
    if (b->mixer == NULL || b->world == NULL) {
        return;
    }
    // The engine clears the world's events (and their drop count) after
    // this: read now.
    if (b->playing) {
        read_events(b);
        // Like a system run: a fresh tick; changes stamped after the previous
        // pass are this pass's (scene/world.h "Change ticks").
        nph_world_clamp_ticks(b->world, &b->last_tick, 1U);
        const u32 now = nph_world_advance_tick(b->world);
        change_pass(b, b->last_tick, now);
        b->last_tick = now;
    }
    b->events_dropped = 0U;
}

void audio_bridge_frame(AudioBridge *b, f64 dt)
{
    NPH_ASSERT(b != NULL);
    if (b->mixer == NULL) {
        return;
    }
    NPH_PROFILE_BEGIN("runtime.audio");
    const f64 seconds = frame_seconds(b, dt);
    apply_volumes(b);
    b->occlusion_rays = 0U; // the frame's, the voices starting in it first
    if (b->playing && b->world != NULL) {
        take_oneshots(b);
        find_listener(b, seconds);
        advance_voices(b);
        place_voices(b, seconds);
        occlude(b, seconds);
    }
    nph_audio_mixer_update(b->mixer);
    if (b->mode == NPH_ENGINE_AUDIO_OFFLINE) {
        render_offline(b, seconds);
    }
    NPH_PROFILE_END();
}

NphResult audio_bridge_play(AudioBridge *b, NphGuid sound, const NphSoundPlayDesc *desc)
{
    NPH_ASSERT(b != NULL && desc != NULL);
    if (b->mixer == NULL) {
        return NPH_RESULT_OK; // no audio: nothing to hear
    }
    OneShot o;
    memset(&o, 0, sizeof o);
    o.sound = sound;
    o.desc = *desc;
    if (!nph_mpmc_ring_push(&b->oneshots, &o)) {
        (void)nph_atomic_fetch_add_u32(&b->oneshots_dropped, 1U, NPH_MEMORY_ORDER_RELAXED);
        return NPH_RESULT_FULL;
    }
    return NPH_RESULT_OK;
}

NphResult audio_bridge_preload(AudioBridge *b, NphGuid sound)
{
    NPH_ASSERT(b != NULL);
    if (b->mixer == NULL) {
        return NPH_RESULT_OK;
    }
    const u32 index = sound_of(b, sound, true);
    if (index == NO_VOICE) {
        return nph_guid_is_null(sound) ? NPH_RESULT_INVALID_ARGUMENT : NPH_RESULT_OUT_OF_MEMORY;
    }
    poll_sound(b, index);
    return b->sounds[index].state == SOUND_READY ? NPH_RESULT_OK : NPH_RESULT_NOT_FOUND;
}

NphAudioMixer *audio_bridge_mixer(const AudioBridge *b)
{
    return b->mixer;
}

NphAudioBus audio_bridge_bus(const AudioBridge *b, u32 bus)
{
    return b->mixer != NULL && bus < NPH_AUDIO_SOURCE_BUS_COUNT ? b->buses[bus]
                                                                : NPH_AUDIO_MASTER_BUS;
}

const f32 *audio_bridge_output(const AudioBridge *b, u32 *out_frames)
{
    *out_frames = b->offline != NULL ? b->offline_frames : 0U;
    return b->offline;
}

void audio_bridge_stats(const AudioBridge *b, NphEngineAudioStats *out)
{
    memset(out, 0, sizeof *out);
    out->mode = b->mode;
    if (b->mixer == NULL) {
        return;
    }
    out->sample_rate = b->rate;
    out->channels = b->channels;
    if (b->device != NULL) {
        nph_audio_device_stats(b->device, &out->device);
    }
    nph_audio_mixer_stats(b->mixer, &out->mixer);
    nph_audio_streamer_stats(b->streamer, &out->streamer);
    out->sounds = b->sound_count;
    for (u32 i = 0U; i < b->sound_count; ++i) {
        out->sounds_loading += b->sounds[i].state == SOUND_LOADING ? 1U : 0U;
        out->sounds_failed += b->sounds[i].state == SOUND_FAILED ? 1U : 0U;
        out->sounds_streamed += b->sounds[i].file != NULL ? 1U : 0U;
    }
    out->voices = b->voice_count;
    for (u32 i = 0U; i < b->voice_count; ++i) {
        out->voices_waiting += b->voices[i].state != VOICE_PLAYING ? 1U : 0U;
    }
    out->oneshots_dropped = nph_atomic_load_u32(&b->oneshots_dropped, NPH_MEMORY_ORDER_RELAXED);
    out->occlusion_rays = b->occlusion_rays;
}

NphSoundPlayDesc nph_sound_play_desc_default(void)
{
    NphSoundPlayDesc d;
    memset(&d, 0, sizeof d);
    d.volume = 1.0F;
    d.pitch = 1.0F;
    d.bus = (u32)NPH_AUDIO_SOURCE_BUS_EFFECTS;
    d.priority = 128U;
    d.spatial = 1;
    d.min_distance = 1.0F;
    d.max_distance = 50.0F;
    d.reverb = 0.25F;
    d.occlusion = 1;
    return d;
}
