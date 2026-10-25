// SPDX-License-Identifier: Apache-2.0
#pragma once

// The mixer (CLAUDE.md §8.10 "Voices, buses or submixes, a priority-based
// virtual voice system, ducking, and snapshots"; docs/audio.md "The
// mixer"; docs/adr/0053-audio-mixer.md).
//
// Two threads share a mixer:
//   - the game thread, which owns it: it creates clips, buses, and
//     snapshots, plays and steers voices, and calls nph_audio_mixer_update
//     once a frame to hear back from the audio thread;
//   - the audio thread, which calls nph_audio_mixer_render for every buffer
//     the device asks for (or a test, offline, for every block it wants).
// They talk through two lock-free single-producer rings (core/ring.h): the
// game thread's commands, applied at the start of the next block, and the
// audio thread's events (a voice ended, a clip's memory may go). Nothing
// is shared otherwise: each side keeps its own copy of what it needs.
//
// Real-time rules (ADR-0051): rendering allocates nothing, takes no lock,
// waits on nothing, and does no I/O; it runs inside a real-time scope
// (core/realtime.h) that reports any call that would. Every buffer the
// audio thread uses is allocated by nph_audio_mixer_create; clips and
// buses the game thread makes are handed over by pointer and released by
// the game thread once the audio thread says it is done with them.
//
// Voices. A voice plays a clip on a bus, at a volume, pitch, and pan (or a
// position, for 3D voices: spatial.h), from any frame, after any delay
// (sample-accurate), once or looping. Every change of gain ramps over a
// block, and starts, stops, and changes of state fade, so nothing clicks.
// There are more voice slots than voices mixed at once: each block the
// voices are ranked by priority, then by how loud they will be, and the
// first max_real_voices are mixed; the rest are virtual, advancing through
// their clips in silence, ready to come back at the right place.
//
// Buses form a tree under the master. Voices mix into their bus, buses
// into their parent, children first; each bus has insert effects, a
// low-pass for snapshots, a volume, a mute, and sends. Ducking lowers a
// bus while another sounds; snapshots fade sets of bus changes in and out.
//
// Thread-safety: everything is NPH_MAIN_THREAD_ONLY (the game thread) but
// nph_audio_mixer_render (the audio thread, one at a time) and
// nph_audio_mixer_stats (any thread). The meters are the game thread's:
// the audio thread publishes their values through atomics, but checking
// the handle reads the game thread's tables.

#include <nepheline/audio/audio.h>
#include <nepheline/audio/dynamics.h>
#include <nepheline/audio/echo.h>
#include <nepheline/audio/eq.h>
#include <nepheline/audio/reverb.h>
#include <nepheline/audio/spatial.h>
#include <nepheline/audio/stream.h>
#include <nepheline/core/alloc.h>
#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAudioMixer NphAudioMixer;

typedef struct NphAudioClip {
    u32 value;
} NphAudioClip;

typedef struct NphAudioVoice {
    u32 value;
} NphAudioVoice;

typedef struct NphAudioBus {
    u32 value; // 0 is the master
} NphAudioBus;

typedef struct NphAudioSnapshot {
    u32 value;
} NphAudioSnapshot;

#define NPH_AUDIO_MASTER_BUS ((NphAudioBus){0U})

// Sends a voice may have (to buses with reverberators, typically).
#define NPH_AUDIO_MAX_SENDS 2U

// ---------------------------------------------------------------------------
// The mixer
// ---------------------------------------------------------------------------

typedef struct NphAudioMixerDesc {
    NphAllocator *alloc;  // required
    u32 sample_rate;      // the mix rate (0: 48000)
    u32 channels;         // the output layout's channel count: 1, 2, 4, 6, or 8 (0: 2)
    u32 max_voices;       // voice slots, real and virtual (0: 512)
    u32 max_real_voices;  // voices mixed at once (0: 128; at most max_voices)
    u32 max_clips;        // (0: 4096)
    u32 max_buses;        // the master included (0: 32)
    u32 max_snapshots;    // (0: 32)
    u32 command_capacity; // commands in flight to the audio thread (0: 4096; a power of two)
    u32 max_effects;      // effects on all buses (0: 64; at most 4096)
    f32 speed_of_sound;   // for Doppler, m/s (0: NPH_AUDIO_SPEED_OF_SOUND)
} NphAudioMixerDesc;

// Creates a mixer: every buffer and table the audio thread will use, and
// the resampler's banks (resampler.h). NPH_RESULT_INVALID_ARGUMENT for a
// missing allocator or a value out of range; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_mixer_create(const NphAudioMixerDesc *desc, NphAudioMixer **out);

// Releases everything, clips and buses included. The audio thread must have
// stopped rendering. Safe on NULL.
void nph_audio_mixer_destroy(NphAudioMixer *mixer);

u32 nph_audio_mixer_sample_rate(const NphAudioMixer *mixer);
u32 nph_audio_mixer_channels(const NphAudioMixer *mixer);

// Hears back from the audio thread: releases the handles of voices that
// ended and the memory of clips and buses it no longer uses. Call once a
// frame (more is fine). Cheap when nothing happened.
void nph_audio_mixer_update(NphAudioMixer *mixer);

// The audio thread: writes `frames` interleaved frames of the output
// layout to `out`, in [-1, 1]. Any frame count: the mixer renders whole
// blocks and keeps what is left of the last one for the next call. Applies
// the commands sent before the call at the start of the first block it
// renders. NPH_RENDER_THREAD_ONLY, in the sense of the audio thread.
void nph_audio_mixer_render(NphAudioMixer *mixer, f32 *out, u32 frames);

typedef struct NphAudioMixerStats {
    u64 blocks;           // blocks rendered
    u32 voices_playing;   // started and not ended, real or virtual
    u32 voices_real;      // mixed in the last block
    u32 voices_virtual;   // advanced in silence in the last block
    u32 commands_dropped; // commands refused because the ring was full
    u64 block_ns_last;    // the last block's render time
    u64 block_ns_max;     // the slowest block's
    u64 block_ns_total;   // all blocks'
    u32 corrupt_reads;    // blocks of compressed clips that failed to decode (silence instead)
    u64 stream_starved;   // frames streams had not delivered in time (silence instead)
} NphAudioMixerStats;

// The audio thread's statistics, as of its last block. NPH_THREADSAFE.
void nph_audio_mixer_stats(const NphAudioMixer *mixer, NphAudioMixerStats *out);

// ---------------------------------------------------------------------------
// Clips
// ---------------------------------------------------------------------------

// The most frames a clip in memory holds (12.4 hours at 48 kHz): a voice's
// position is 32.32 fixed point. Longer sounds stream (a stream's voice
// moves its position on as it goes).
#define NPH_AUDIO_MAX_CLIP_FRAMES (1ULL << 31U)

typedef struct NphAudioClipDesc {
    NphAudioCodec codec;
    NphAudioSampleFormat format; // PCM: the samples' format
    u32 channels;                // 1 .. NPH_AUDIO_MAX_CHANNELS
    u32 sample_rate;             // NPH_AUDIO_MIN_RATE .. NPH_AUDIO_MAX_RATE
    u64 frames;                  // 1 .. NPH_AUDIO_MAX_CLIP_FRAMES
    const void *data;            // the samples, the lossless stream's blocks, or the ADPCM stream
    u64 size;                    // bytes of data
    // Lossless only: the stream's depth, block size, and block offsets
    // (lossless.h NphLosslessStream).
    u32 bits;
    u32 block_frames;
    const u32 *block_offsets;
    u32 block_count;
    // The loop a looping voice repeats: [loop_start, loop_end) in frames
    // (loop_end 0: the whole clip).
    u64 loop_start;
    u64 loop_end;
    // false (the default): the clip copies data and offsets, so the caller
    // may free them at once. true: the clip uses them where they are, and
    // they must stay valid and resident (not paged out: the audio thread
    // must never fault on them) until the clip's release completes
    // (nph_audio_clip_destroy, then nph_audio_mixer_update).
    b32 borrow;
} NphAudioClipDesc;

// Makes a clip from `desc`, checking the data (a lossless stream's every
// block CRC; an ADPCM stream's size and shifts), on this thread, so the
// audio thread never meets a malformed clip. NPH_RESULT_INVALID_ARGUMENT
// for impossible parameters, NPH_RESULT_CORRUPT for data that fails its
// checks, NPH_RESULT_FULL past max_clips, NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_clip_create(NphAudioMixer *mixer, const NphAudioClipDesc *desc,
                                             NphAudioClip *out);

// Stops every voice playing the clip (with a short fade) and releases it:
// its memory goes at an update after the audio thread lets go. The handle
// is stale at once. Ignores a stale handle.
void nph_audio_clip_destroy(NphAudioMixer *mixer, NphAudioClip clip);

bool nph_audio_clip_is_valid(const NphAudioMixer *mixer, NphAudioClip clip);

// The clip's length in seconds (0 for a stale handle).
f64 nph_audio_clip_duration(const NphAudioMixer *mixer, NphAudioClip clip);

// A clip whose frames `streamer` reads from storage as it plays (stream.h).
// It plays once: one voice plays it from where the stream was opened, and
// another play (while that voice plays or after it ends) is
// NPH_RESULT_INVALID_STATE; to play the stream again, make another clip
// (which reads and fills its ring here, where the cost is seen). The voice
// neither loops it (desc->loop does) nor starts it part-way
// (desc->start_frame does). Destroying the clip closes the stream once the
// audio thread lets go. NPH_RESULT_INVALID_ARGUMENT for a malformed
// description (ADPCM too: it is for clips in memory); NPH_RESULT_FULL past
// max_clips or the streamer's max_streams; NPH_RESULT_IO_ERROR when the
// first fill cannot read, NPH_RESULT_CORRUPT when it meets a corrupt block;
// NPH_RESULT_OUT_OF_MEMORY. Its duration is infinite when it loops.
NPH_MUST_USE NphResult nph_audio_clip_create_stream(NphAudioMixer *mixer,
                                                    NphAudioStreamer *streamer,
                                                    const NphAudioStreamDesc *desc,
                                                    NphAudioClip *out);

// The frames a stream's clip holds decoded ahead of its voice: the ring's
// fill, the streamer's lead (0 for a stale handle, a clip in memory, or a
// stream its voice has outrun). A health readout: the audio thread and the
// streamer move it as it is read.
u64 nph_audio_clip_stream_ahead(const NphAudioMixer *mixer, NphAudioClip clip);

// ---------------------------------------------------------------------------
// Buses
// ---------------------------------------------------------------------------

typedef struct NphAudioBusDesc {
    NphAudioBus parent; // NPH_AUDIO_MASTER_BUS by default
    f32 volume;         // linear gain (the desc from nph_audio_bus_desc_default has 1)
    const char *name;   // optional, copied (for the editor), up to 31 bytes
} NphAudioBusDesc;

NphAudioBusDesc nph_audio_bus_desc_default(void);

// Makes a bus under `desc->parent`. NPH_RESULT_INVALID_ARGUMENT for a stale
// parent; NPH_RESULT_FULL past max_buses.
NPH_MUST_USE NphResult nph_audio_bus_create(NphAudioMixer *mixer, const NphAudioBusDesc *desc,
                                            NphAudioBus *out);

// Removes a bus (not the master): its voices and child buses move to its
// parent. Ignores a stale handle or the master.
void nph_audio_bus_destroy(NphAudioMixer *mixer, NphAudioBus bus);

bool nph_audio_bus_is_valid(const NphAudioMixer *mixer, NphAudioBus bus);

// The bus's base volume (linear, ramped over a block) and mute (faded over
// a block).
void nph_audio_bus_set_volume(NphAudioMixer *mixer, NphAudioBus bus, f32 volume);
void nph_audio_bus_set_mute(NphAudioMixer *mixer, NphAudioBus bus, bool mute);

// Sends the bus's output, after its volume, at `gain` to bus `target` as
// well as to its parent (slot 0 .. NPH_AUDIO_MAX_SENDS - 1; gain 0 removes
// it). A send may not make a cycle (NPH_RESULT_INVALID_ARGUMENT).
NPH_MUST_USE NphResult nph_audio_bus_set_send(NphAudioMixer *mixer, NphAudioBus bus, u32 slot,
                                              NphAudioBus target, f32 gain);

// Ducking: while bus `trigger` is louder than `threshold_db` (its RMS over
// a block), bus `target` is lowered by `depth_db` (negative), reaching it
// over `attack` seconds and recovering over `release`. One trigger per
// target; depth_db 0 removes it.
void nph_audio_bus_set_duck(NphAudioMixer *mixer, NphAudioBus target, NphAudioBus trigger,
                            f32 depth_db, f32 threshold_db, f32 attack, f32 release);

// The bus's level after its volume in the last block: peak and RMS, as
// linear gains (published by the audio thread; read on the game thread).
void nph_audio_bus_meter(const NphAudioMixer *mixer, NphAudioBus bus, f32 *out_peak, f32 *out_rms);

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

// What a snapshot does to a bus at full weight: a volume change in dB, and
// a low-pass corner in Hz (0: none). A pause menu: music -6 dB, effects
// -18 dB through 800 Hz.
typedef struct NphAudioSnapshotEntry {
    NphAudioBus bus;
    f32 volume_db;
    f32 lowpass_hz;
} NphAudioSnapshotEntry;

#define NPH_AUDIO_SNAPSHOT_MAX_ENTRIES 16U

// Makes a snapshot of up to NPH_AUDIO_SNAPSHOT_MAX_ENTRIES entries, at
// weight 0. NPH_RESULT_INVALID_ARGUMENT for a stale bus or too many entries;
// NPH_RESULT_FULL past max_snapshots.
NPH_MUST_USE NphResult nph_audio_snapshot_create(NphAudioMixer *mixer,
                                                 const NphAudioSnapshotEntry *entries, u32 count,
                                                 NphAudioSnapshot *out);

// Fades the snapshot's weight to `weight` (0 off .. 1 fully on) over
// `seconds`. Active snapshots combine: their volume changes add (in dB,
// each times its weight), and the lowest corner wins.
void nph_audio_snapshot_set_weight(NphAudioMixer *mixer, NphAudioSnapshot snapshot, f32 weight,
                                   f32 seconds);

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------

// A bus's insert effects run in order on its signal, before its snapshot
// low-pass, gain, meters, and sends (docs/audio.md "Effects"):
//   - the equaliser (eq.h), the compressor and the limiter (dynamics.h),
//     the echo (echo.h), and the reverberator (reverb.h), each as its own
//     header describes;
//   - an effect is made on the game thread with all its memory, handed to
//     the audio thread, and freed by an update after the audio thread lets
//     go, as clips are;
//   - its parameters change one at a time, as commands, so nothing the
//     audio thread reads is rewritten under it; each effect smooths its
//     own changes;
//   - an effect coming in (made, or brought back from bypass) fades its
//     input in across a block while the bus's signal without it fades out,
//     so what its delays and tails make starts from silence; one going out
//     (bypassed or destroyed) crossfades its output to the signal without
//     it. A bypassed effect's state starts over: it comes back without the
//     tail it had.
// A limiter delays its bus by its latency (dynamics.h), so it belongs on
// the master, where everything waits alike, made before anything plays (in
// the block it comes in, the signal without it is still heard).

typedef struct NphAudioEffect {
    u32 value;
} NphAudioEffect;

typedef enum NphAudioEffectType {
    NPH_AUDIO_EFFECT_EQ = 0,
    NPH_AUDIO_EFFECT_COMPRESSOR = 1,
    NPH_AUDIO_EFFECT_LIMITER = 2,
    NPH_AUDIO_EFFECT_ECHO = 3,
    NPH_AUDIO_EFFECT_REVERB = 4,
    NPH_AUDIO_EFFECT_COUNT = 5
} NphAudioEffectType;

// Effects a bus may carry.
#define NPH_AUDIO_BUS_EFFECTS 4U

typedef struct NphAudioEffectDesc {
    NphAudioEffectType type;
    // The settings of its type (only that one is read).
    NphAudioEqBand eq[NPH_AUDIO_EQ_BANDS];
    NphAudioCompressorParams compressor;
    NphAudioLimiterParams limiter;
    NphAudioEchoParams echo;
    f32 echo_max_delay_ms; // the longest delay the echo will be set to (0: 2000)
    NphAudioReverbParams reverb;
} NphAudioEffectDesc;

// An effect of `type` with its default settings (the equaliser's bands
// off).
NphAudioEffectDesc nph_audio_effect_desc_default(NphAudioEffectType type);

// Appends an effect to the bus's chain. NPH_RESULT_INVALID_ARGUMENT for a
// stale bus or impossible settings; NPH_RESULT_FULL past max_effects or
// NPH_AUDIO_BUS_EFFECTS on the bus (a destroyed effect keeps its place
// until an update after the audio thread lets it go);
// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_effect_create(NphAudioMixer *mixer, NphAudioBus bus,
                                               const NphAudioEffectDesc *desc, NphAudioEffect *out);

// Fades the effect out of its chain and releases it (its memory goes at an
// update after the audio thread lets go). The handle is stale at once.
// Ignores a stale handle. Destroying a bus destroys its effects.
void nph_audio_effect_destroy(NphAudioMixer *mixer, NphAudioEffect effect);

bool nph_audio_effect_is_valid(const NphAudioMixer *mixer, NphAudioEffect effect);

// The parameters, by type: an equaliser's are NPH_AUDIO_EQ_PARAM(band,
// field); the others' are their enums below. Values are those of the
// type's parameter struct (an equaliser band's type as its
// NphAudioFilterType, a flag as 0 or 1).
typedef enum NphAudioEqField {
    NPH_AUDIO_EQ_TYPE = 0,
    NPH_AUDIO_EQ_FREQ = 1,
    NPH_AUDIO_EQ_Q = 2,
    NPH_AUDIO_EQ_GAIN = 3,
    NPH_AUDIO_EQ_ENABLED = 4,
    NPH_AUDIO_EQ_FIELDS = 5
} NphAudioEqField;

#define NPH_AUDIO_EQ_PARAM(band, field) ((u32)(band) * (u32)NPH_AUDIO_EQ_FIELDS + (u32)(field))

typedef enum NphAudioCompressorParam {
    NPH_AUDIO_COMPRESSOR_THRESHOLD = 0,
    NPH_AUDIO_COMPRESSOR_RATIO = 1,
    NPH_AUDIO_COMPRESSOR_KNEE = 2,
    NPH_AUDIO_COMPRESSOR_ATTACK = 3,
    NPH_AUDIO_COMPRESSOR_RELEASE = 4,
    NPH_AUDIO_COMPRESSOR_MAKEUP = 5
} NphAudioCompressorParam;

typedef enum NphAudioLimiterParam {
    NPH_AUDIO_LIMITER_CEILING = 0,
    NPH_AUDIO_LIMITER_RELEASE = 1
} NphAudioLimiterParam;

typedef enum NphAudioEchoParam {
    NPH_AUDIO_ECHO_DELAY = 0,
    NPH_AUDIO_ECHO_FEEDBACK = 1,
    NPH_AUDIO_ECHO_DAMPING = 2,
    NPH_AUDIO_ECHO_HIGHPASS = 3,
    NPH_AUDIO_ECHO_WET = 4,
    NPH_AUDIO_ECHO_DRY = 5,
    NPH_AUDIO_ECHO_PING_PONG = 6
} NphAudioEchoParam;

typedef enum NphAudioReverbParam {
    NPH_AUDIO_REVERB_SIZE = 0,
    NPH_AUDIO_REVERB_DECAY = 1,
    NPH_AUDIO_REVERB_DAMPING = 2,
    NPH_AUDIO_REVERB_CROSSOVER = 3,
    NPH_AUDIO_REVERB_PREDELAY = 4,
    NPH_AUDIO_REVERB_DIFFUSION = 5,
    NPH_AUDIO_REVERB_MODULATION = 6,
    NPH_AUDIO_REVERB_EARLY = 7,
    NPH_AUDIO_REVERB_LATE = 8,
    NPH_AUDIO_REVERB_WIDTH = 9,
    NPH_AUDIO_REVERB_DRY = 10
} NphAudioReverbParam;

// Sets one parameter, heard from the next block. Ignores a stale handle
// and a parameter the type does not have.
void nph_audio_effect_set_param(NphAudioMixer *mixer, NphAudioEffect effect, u32 param, f32 value);

// Bypasses the effect (crossfading across a block) or brings it back.
void nph_audio_effect_set_bypass(NphAudioMixer *mixer, NphAudioEffect effect, bool bypass);

// A compressor's or limiter's deepest gain reduction in its last block, in
// dB (>= 0); 0 for the other types and stale handles.
f32 nph_audio_effect_meter(const NphAudioMixer *mixer, NphAudioEffect effect);

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

typedef struct NphAudioPlayDesc {
    NphAudioClip clip;
    NphAudioBus bus;  // NPH_AUDIO_MASTER_BUS by default
    f32 volume;       // linear gain
    f32 pitch;        // playback rate (1: as recorded), with the clip's own rate taken care of
    f32 pan;          // -1 left .. 1 right (a mono clip's place; a multichannel clip's balance)
    u8 priority;      // 0 .. 255: higher keeps its real voice
    b32 loop;         // repeat the clip's loop
    u64 start_frame;  // where in the clip to start
    u32 delay_frames; // frames of the mix to wait before starting (sample-accurate)
    f32 fade_in;      // seconds (starting mid-clip always fades over a block at least)
    f32 lowpass_hz;   // a low-pass on the voice (0: none)
    f32 highpass_hz;  // a high-pass on the voice (0: none)
    NphAudioBus sends[NPH_AUDIO_MAX_SENDS]; // extra buses fed by the voice (reverb)
    f32 send_gains[NPH_AUDIO_MAX_SENDS];    // their gains (0: unused)
    // 3D (spatial.h). A spatial voice is placed by its position instead of
    // its pan: attenuated by distance, spread when near, pitched by Doppler,
    // darkened by air, occlusion, and obstruction. The sends take the
    // distance's attenuation only in part (send_distance), as a room's
    // reverberation stays while the direct sound falls.
    b32 spatial;
    NphVec3 position; // in the listener's frame: m, +x right, +y up, -z ahead
    NphVec3 velocity; // relative to the listener, in its frame (m/s)
    NphAudioAttenuation attenuation;
    f32 size;           // the source's radius (m): its spread when near (0: a point)
    f32 doppler;        // 0: none, 1: physical (the default)
    f32 air_absorption; // 0: none, 1: ISO 9613-1 at 20 C and 50% humidity (the default)
    f32 send_distance;  // 0 .. 1: the share of the distance attenuation the sends take (0.5)
} NphAudioPlayDesc;

// A desc with volume and pitch 1, centred, priority 128, on the master;
// not spatial, but ready to be: the default attenuation (spatial.h),
// physical Doppler and air absorption, and sends taking half the distance's
// attenuation.
NphAudioPlayDesc nph_audio_play_desc_default(NphAudioClip clip);

// Starts a voice. NPH_RESULT_INVALID_ARGUMENT for a stale clip or bus, or a
// start past the clip's end (or anywhere but 0 on a stream's clip);
// NPH_RESULT_INVALID_STATE for a stream's clip played before;
// NPH_RESULT_FULL when every voice slot is taken or the command ring is
// (nothing is played then).
NPH_MUST_USE NphResult nph_audio_voice_play(NphAudioMixer *mixer, const NphAudioPlayDesc *desc,
                                            NphAudioVoice *out);

// Stops the voice, fading over `fade` seconds (at least a block). Ignores a
// stale handle. The handle stays valid until the voice has faded and the
// audio thread says so (an update after it has).
void nph_audio_voice_stop(NphAudioMixer *mixer, NphAudioVoice voice, f32 fade);

// True from nph_audio_voice_play until an update hears that it ended.
bool nph_audio_voice_is_playing(const NphAudioMixer *mixer, NphAudioVoice voice);

void nph_audio_voice_set_volume(NphAudioMixer *mixer, NphAudioVoice voice, f32 volume);
void nph_audio_voice_set_pitch(NphAudioMixer *mixer, NphAudioVoice voice, f32 pitch);
void nph_audio_voice_set_pan(NphAudioMixer *mixer, NphAudioVoice voice, f32 pan);
void nph_audio_voice_set_paused(NphAudioMixer *mixer, NphAudioVoice voice, bool paused);
void nph_audio_voice_set_filters(NphAudioMixer *mixer, NphAudioVoice voice, f32 lowpass_hz,
                                 f32 highpass_hz);
void nph_audio_voice_set_send(NphAudioMixer *mixer, NphAudioVoice voice, u32 slot, f32 gain);

// A spatial voice's place: its position and velocity in the listener's
// frame (the runtime's job each frame, from the world's 64-bit positions).
// Ignored for a voice that is not spatial.
void nph_audio_voice_set_position(NphAudioMixer *mixer, NphAudioVoice voice, NphVec3 position,
                                  NphVec3 velocity);

// A spatial voice's occlusion and obstruction, 0 .. 1 each (the runtime's
// rays decide them):
//   - occlusion, the sound through a wall: the direct sound and the sends
//     lose up to 24 dB, and their highs from 20 kHz down to 700 Hz;
//   - obstruction, the direct path alone blocked (round a corner, behind a
//     pillar): the direct sound loses up to 12 dB and its highs down to
//     1 kHz, while the sends, the room's reflections, keep theirs.
void nph_audio_voice_set_occlusion(NphAudioMixer *mixer, NphAudioVoice voice, f32 occlusion,
                                   f32 obstruction);

#ifdef __cplusplus
}
#endif
