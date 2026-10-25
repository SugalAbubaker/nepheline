// SPDX-License-Identifier: Apache-2.0
#pragma once

// The mixer's state on both sides of its rings (mixer.h; docs/audio.md "The
// mixer"). The game thread's half (handles, mailboxes, mirrors of what it
// sent) and the audio thread's half (voices, buses, snapshots, contexts,
// scratch) live in one struct but are never touched from the other side,
// except through the rings, the mailboxes (written before a command is
// pushed, read after it is popped), and the published statistics (atomics).

#include <nepheline/audio/adpcm.h>
#include <nepheline/audio/filter.h>
#include <nepheline/audio/kernels.h>
#include <nepheline/audio/lossless.h>
#include <nepheline/audio/mixer.h>
#include <nepheline/audio/pan.h>
#include <nepheline/audio/resampler.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/handle.h>
#include <nepheline/core/ring.h>

#define MIX_BLOCK NPH_AUDIO_BLOCK_FRAMES
// Source frames a real voice keeps from one block to the next: the widest
// kernel's reach before a position, and its reach after (read ahead).
#define MIX_HISTORY (NPH_AUDIO_RESAMPLE_MAX_TAPS + 8U)
// The most source frames one block of one voice can need: the history, a
// block at the largest step, and the widest kernel's reach after.
#define MIX_WINDOW                                                                                 \
    (MIX_HISTORY + MIX_BLOCK * NPH_AUDIO_RESAMPLE_MAX_STEP + NPH_AUDIO_RESAMPLE_MAX_TAPS)
#define MIX_NONE NPH_U32_MAX

// How far past its last frame a voice that does not loop keeps going: the
// full-band kernel's half-width, so its ring-out is heard rather than cut.
#define MIX_TAIL_FRAMES (NPH_AUDIO_RESAMPLE_ZERO_CROSSINGS)

// Filter corners that mean no filter: a low-pass at or above MIX_FULL_BAND,
// a high-pass at or below MIX_HIGHPASS_OFF.
#define MIX_FULL_BAND    20000.0F
#define MIX_HIGHPASS_OFF 10.0F

// A clip, immutable once made (the game thread owns and frees it).
typedef struct MixClip {
    NphAudioCodec codec;
    NphAudioSampleFormat format;
    u32 channels;
    u32 sample_rate;
    u64 frames;
    u64 loop_start;
    u64 loop_end;
    const u8 *data;
    u64 size;
    u32 bytes_per_frame; // PCM
    NphLosslessStream lossless;
    NphAdpcmStream adpcm;
    f64 rate_ratio; // the clip's rate over the mix rate
    // Copies the clip owns (NULL when borrowed).
    u8 *owned_data;
    u32 *owned_offsets;
    u32 owned_offset_count;
} MixClip;

typedef enum MixCommandType {
    MIX_CMD_CLIP_ADD = 0,
    MIX_CMD_CLIP_REMOVE,
    MIX_CMD_VOICE_PLAY,
    MIX_CMD_VOICE_STOP,
    MIX_CMD_VOICE_SET,
    MIX_CMD_BUS_ADD,
    MIX_CMD_BUS_REMOVE,
    MIX_CMD_BUS_SET,
    MIX_CMD_BUS_SEND,
    MIX_CMD_BUS_DUCK,
    MIX_CMD_SNAPSHOT_ADD,
    MIX_CMD_SNAPSHOT_WEIGHT,
} MixCommandType;

typedef enum MixVoiceParam {
    MIX_VOICE_VOLUME = 0,
    MIX_VOICE_PITCH,
    MIX_VOICE_PAN,
    MIX_VOICE_PAUSED,
    MIX_VOICE_FILTERS,
    MIX_VOICE_SEND,
} MixVoiceParam;

typedef enum MixBusParam {
    MIX_BUS_VOLUME = 0,
    MIX_BUS_MUTE,
} MixBusParam;

typedef struct MixCommand {
    u32 type;
    u32 index;
    u32 generation;
    u32 param;
    f32 values[6];
    void *ptr;
} MixCommand;

typedef enum MixEventType {
    MIX_EVENT_VOICE_END = 0,
    MIX_EVENT_CLIP_RELEASED,
    MIX_EVENT_BUS_RELEASED,
} MixEventType;

typedef struct MixEvent {
    u32 type;
    u32 index;
    u32 generation;
    u32 reserved;
} MixEvent;

// What a play command carries, written by the game thread into the voice
// slot's mailbox before the command is pushed.
typedef struct MixVoiceInit {
    u32 clip;
    u32 bus;
    f32 volume;
    f32 pitch;
    f32 pan;
    u32 priority;
    b32 loop;
    u64 start_frame;
    u32 delay_frames;
    f32 fade_in;
    f32 lowpass_hz;
    f32 highpass_hz;
    u32 send_bus[NPH_AUDIO_MAX_SENDS];
    f32 send_gain[NPH_AUDIO_MAX_SENDS];
    u64 sequence; // play order: ties go to the older voice
} MixVoiceInit;

typedef enum MixVoiceState {
    MIX_VOICE_FREE = 0,
    MIX_VOICE_PLAYING,
} MixVoiceState;

// What only a real voice needs: the source frames it keeps between blocks,
// its decoder, and its filters' states. Pooled.
typedef struct MixContext {
    f32 history[NPH_AUDIO_MAX_CHANNELS][MIX_HISTORY];
    i64 history_first; // timeline frame of history[c][0]
    u32 history_len;
    NphLosslessDecoder decoder;
    u32 decoder_block; // the block the decoder is in, or MIX_NONE
    u64 decoder_frame; // the clip frame it produces next
    NphAudioFilterState lowpass[NPH_AUDIO_MAX_CHANNELS];
    NphAudioFilterState highpass[NPH_AUDIO_MAX_CHANNELS];
    u32 next_free;
} MixContext;

// A voice, on the audio thread.
typedef struct MixVoice {
    u32 state;
    u32 generation;
    u32 clip;
    u32 bus;
    u64 pos; // 32.32 frames of the voice's timeline (looping wraps it)
    f32 volume;
    f32 pitch;
    f32 pan;
    u32 priority;
    b32 loop;
    b32 paused;
    b32 stopping;
    u32 delay;      // frames before it starts
    f32 fade;       // gain of the fade at the end of the last block
    f32 fade_rate;  // per frame (+ in, - out)
    f32 lowpass_hz; // targets (0: none)
    f32 highpass_hz;
    f32 lowpass_now; // where the corners are, gliding to their targets a block at a time
    f32 highpass_now;
    NphAudioFilterCoeffs lowpass;
    NphAudioFilterCoeffs highpass;
    u32 send_bus[NPH_AUDIO_MAX_SENDS];
    f32 send_gain[NPH_AUDIO_MAX_SENDS];
    u64 sequence;
    u32 context;  // MIX_NONE when virtual
    b32 real;     // mixed this block
    b32 demoting; // mixed this block toward silence, then virtual
    b32 entering; // came back from virtual (or started mid-clip): fades in over its first block
    b32 fresh;    // not rendered yet: starts at full gain (its own attack), no ramp
    f32 gains[NPH_AUDIO_MAX_CHANNELS]
             [NPH_AUDIO_MAX_CHANNELS]; // source x output, at the block's end
    f32 sends_current[NPH_AUDIO_MAX_SENDS];
    f32 score; // audibility for the ranking
} MixVoice;

typedef struct MixBus {
    b32 active;
    u32 parent;
    u32 depth; // levels below the master
    f32 volume;
    b32 mute;
    f32 gain_current; // the last block's final gain
    u32 send_bus[NPH_AUDIO_MAX_SENDS];
    f32 send_gain[NPH_AUDIO_MAX_SENDS];
    f32 send_current[NPH_AUDIO_MAX_SENDS];
    // Ducking: lowered while `duck_trigger` is loud.
    u32 duck_trigger;
    f32 duck_depth_db;
    f32 duck_threshold; // linear RMS
    f32 duck_attack;    // per-block smoothing coefficients
    f32 duck_release;
    f32 duck_db; // the current reduction
    // Snapshots' low-pass: its corner (0: none), design, and state.
    f32 lowpass_hz;
    NphAudioFilterCoeffs lowpass;
    NphAudioFilterState lowpass_state[NPH_AUDIO_MAX_CHANNELS];
    f32 *buffer[NPH_AUDIO_MAX_CHANNELS];
    f32 peak;
    f32 rms;
    f32 snapshot_db;
    f32 snapshot_lowpass;
} MixBus;

typedef struct MixSnapshot {
    b32 active;
    u32 count;
    NphAudioSnapshotEntry entries[NPH_AUDIO_SNAPSHOT_MAX_ENTRIES];
    f32 weight;
    f32 target;
    f32 rate; // weight change per block
} MixSnapshot;

// The game thread's view of a bus: enough to validate what it sends.
typedef struct MixBusMirror {
    u32 generation; // 0: free
    u32 parent;
    u32 send_bus[NPH_AUDIO_MAX_SENDS];
    b32 releasing;
    char name[32];
} MixBusMirror;

typedef struct MixClipSlot {
    MixClip *clip;
    NphHandle handle;
    b32 releasing;
} MixClipSlot;

struct NphAudioMixer {
    NphAllocator *alloc;
    u32 rate;
    u32 channels;
    NphAudioLayout layout;
    u32 max_voices;
    u32 max_real;
    u32 max_clips;
    u32 max_buses;
    u32 max_snapshots;
    const NphAudioKernels *kernels;
    NphAudioResampler *resampler;
    NphAudioPanner panner;

    // --- The rings ---
    NphSpscRing commands; // game -> audio
    NphSpscRing events;   // audio -> game

    // --- The game thread's half ---
    NphHandlePool voice_handles;
    NphHandlePool clip_handles;
    MixVoiceInit *mailbox;    // per voice slot
    MixClipSlot *clip_slots;  // per clip slot
    u32 *voice_clip;          // per voice slot: its clip's slot (for stopping a clip's voices)
    MixBusMirror *bus_mirror; // per bus slot
    u32 bus_generation_next;
    MixSnapshot *snapshot_mailbox; // per snapshot slot
    u32 snapshot_count;
    u64 sequence;
    // Removals the command ring had no room for, retried by each update.
    u32 *pending_clip_removals;
    u32 pending_clip_removal_count;
    u32 *pending_bus_removals;
    u32 pending_bus_removal_count;

    // --- The audio thread's half ---
    MixVoice *voices;
    MixClip **audio_clips;
    u32 *clip_voice_count; // voices playing each clip
    b32 *clip_removing;
    MixBus *buses;
    u32 *bus_order; // deepest first, the master last
    u32 bus_order_count;
    b32 bus_order_dirty;
    MixSnapshot *snapshots;
    MixContext *contexts;
    u32 context_count;
    u32 context_free;                       // the free list's head
    u64 *ranking_keys;                      // scratch: (priority, score, slot) per candidate
    f32 *window[NPH_AUDIO_MAX_CHANNELS];    // a voice's source window
    f32 *voice_out[NPH_AUDIO_MAX_CHANNELS]; // a voice's resampled block
    f32 *filter_dry;                        // a block before a filter that switches on or off
    f32 *block_out;                         // the master block, interleaved
    f32 *bus_level;  // per bus: its gain times its ancestors' (for the ranking)
    f32 *bus_target; // per bus: this block's final gain
    u32 block_left;  // frames of block_out not yet handed out
    u64 blocks;
    u64 block_ns_max;
    u64 block_ns_total;
    u32 corrupt_reads;

    // --- Published by the audio thread (atomics) ---
    NphAtomicU64 stat_blocks;
    NphAtomicU32 stat_playing;
    NphAtomicU32 stat_real;
    NphAtomicU32 stat_virtual;
    NphAtomicU64 stat_ns_last;
    NphAtomicU64 stat_ns_max;
    NphAtomicU64 stat_ns_total;
    NphAtomicU32 stat_corrupt;
    NphAtomicU32 stat_dropped; // written by the game thread
    NphAtomicU32 *bus_peak;    // f32 bits per bus
    NphAtomicU32 *bus_rms;
};

// mixer_voice.c: one real voice's block, mixed into its bus and sends.
void mix_voice_render(NphAudioMixer *m, MixVoice *v, u32 offset, u32 frames);

// mixer_voice.c: a virtual voice's block (its position advanced).
void mix_voice_advance(NphAudioMixer *m, MixVoice *v, u32 frames);

// mixer_voice.c: the 32.32 step of a voice this block.
u64 mix_voice_step(const NphAudioMixer *m, const MixVoice *v);

// mixer_voice.c: the voice has played its clip out (and is not looping).
bool mix_voice_finished(const NphAudioMixer *m, const MixVoice *v);

// How a filter's block meets the last one: running on, switching on (the
// block crossfades from the signal without it to the signal through it),
// or switching off (the reverse).
typedef enum MixFilterChange {
    MIX_FILTER_STEADY = 0,
    MIX_FILTER_SWITCH_ON,
    MIX_FILTER_SWITCH_OFF,
} MixFilterChange;

// mixer_voice.c: filters `frames` samples of `x` in place (the voices' and
// the buses' filters) with `to`, gliding there from `from` unless it is
// NULL.
void mix_filter_block(NphAudioMixer *m, const NphAudioFilterCoeffs *from,
                      const NphAudioFilterCoeffs *to, NphAudioFilterState *state, f32 *x,
                      u32 frames, MixFilterChange change);

// mixer_voice.c: the voice's pan matrix (source x output gains, unit
// power per source channel) for a clip of `channels` channels.
void mix_voice_pan_matrix(const NphAudioMixer *m, const MixVoice *v, u32 channels,
                          f32 out[NPH_AUDIO_MAX_CHANNELS][NPH_AUDIO_MAX_CHANNELS]);
