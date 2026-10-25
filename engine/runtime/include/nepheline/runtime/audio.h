// SPDX-License-Identifier: Apache-2.0
#pragma once

// The engine's sound (CLAUDE.md §8.10; docs/runtime.md "Audio";
// docs/adr/0058-audio-in-the-engine.md): how the engine plays (the
// audio.device cvar), the sounds a game plays once without an entity of
// their own, and the statistics the editor shows. The world's own sounds
// are its AudioSource components (components.h "Audio").
//
// Thread-safety: as each function in engine.h and game.h says;
// nph_sound_play_desc_default is pure.

#include <nepheline/audio/device.h>
#include <nepheline/audio/mixer.h>
#include <nepheline/audio/stream.h>
#include <nepheline/core/guid.h>
#include <nepheline/runtime/components.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphEngineAudioMode {
    NPH_ENGINE_AUDIO_OFF = 0,     // no mixer: nothing plays (servers, and tests that do not listen)
    NPH_ENGINE_AUDIO_SYSTEM = 1,  // the platform's output device, at its own format
    NPH_ENGINE_AUDIO_NULL = 2,    // the null device: real time without hardware (CI)
    NPH_ENGINE_AUDIO_OFFLINE = 3, // rendered by each frame on the engine's thread (tests, capture)
} NphEngineAudioMode;

// A sound played once (nph_engine_play_sound): on a bus, at a volume and
// pitch, and in 3D at `position` or following `follow`'s world position.
typedef struct NphSoundPlayDesc {
    f32 volume;        // linear (1)
    f32 pitch;         // a playback rate (1)
    u32 bus;           // NphAudioSourceBus (effects)
    u32 priority;      // 0 .. 255 (128)
    b32 spatial;       // in 3D (true)
    NphDVec3 position; // in the world, when it follows no entity
    NphEntity follow;  // an entity whose world position it sounds from (null: none)
    f32 min_distance;  // full level within (1 m)
    f32 max_distance;  // falling no further beyond (50 m)
    f32 reverb;        // the send to the room (0.25)
    b32 occlusion;     // darkened by what lies between it and the listener (true)
} NphSoundPlayDesc;

// The defaults above.
NphSoundPlayDesc nph_sound_play_desc_default(void);

typedef struct NphEngineAudioStats {
    NphEngineAudioMode mode;
    u32 sample_rate; // the mix's (0 without audio)
    u32 channels;
    NphAudioDeviceStats device; // zeros without a device (OFF, OFFLINE)
    NphAudioMixerStats mixer;
    NphAudioStreamerStats streamer;
    u32 sounds;          // known since play began
    u32 sounds_loading;  // of which still loading
    u32 sounds_failed;   // that cannot be played (logged)
    u32 sounds_streamed; // played from storage
    u32 voices;          // playing, or waiting for their sound or their stream
    u32 voices_waiting;
    u32 oneshots_dropped; // refused because the queue was full, since the engine started
    u32 occlusion_rays;   // cast by the last frame (audio.occlusion_rays)
} NphEngineAudioStats;

#ifdef __cplusplus
}
#endif
