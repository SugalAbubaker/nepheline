// SPDX-License-Identifier: Apache-2.0
#pragma once

// Streaming (CLAUDE.md §8.10 "Streaming for music"; docs/audio.md
// "Streaming"; docs/adr/0056-audio-device-streaming.md): long sounds
// (music, ambience, dialogue) read from storage as they play instead of
// held in memory.
//
// A streamer is a thread that keeps every open stream's ring of decoded
// frames ahead of its voice:
//   - it reads the stream's bytes through the stream's source (the
//     runtime's files or packs: the audio module knows neither), decodes
//     them (PCM or our lossless codec, as clips), and writes planar f32
//     frames into the ring;
//   - it loops a looping stream itself, so the voice sees one endless run
//     of frames;
//   - the voice, on the audio thread, takes frames from the ring by two
//     atomic indices and never waits: frames that have not arrived play as
//     silence, counted as starvation (the mixer's stream_starved), and the
//     read index moves on regardless, so the streamer seeks past them and
//     the stream keeps time;
//   - a voice gone virtual keeps moving the read index as its time passes,
//     so the streamer keeps the ring ahead of it and it comes back on
//     sound: being virtual saves the mixing, not the streaming.
//
// A stream plays as a clip: nph_audio_clip_create_stream (mixer.h) opens it
// on a streamer and returns the clip a voice plays, once. Making it fills
// the ring on the calling thread, so a voice played at once does not start
// dry; or, with defer_fill, leaves the filling to the streamer, so the
// caller does no I/O and no decoding (a game's main thread), and its voice
// starts once nph_audio_clip_stream_ahead shows the ring holds what it
// needs. Destroying the clip closes the stream when the audio thread lets go;
// the streamer frees it on its own thread, with its allocator, which must
// therefore be thread-safe.
//
// Thread-safety: create and destroy are NPH_MAIN_THREAD_ONLY; a source's
// read callback is called on the streamer's thread, and on the thread
// making the clip while it fills the ring (not with defer_fill).

#include <nepheline/audio/audio.h>
#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAudioStreamer NphAudioStreamer;

typedef struct NphAudioStreamerDesc {
    NphAllocator *alloc; // required, and thread-safe (the streamer frees closed streams)
    u32 max_streams;     // open at once (0: 16)
    u32 period_ms;       // how often the streamer looks at its streams (0: 10)
} NphAudioStreamerDesc;

// Starts a streamer's thread. NPH_RESULT_INVALID_ARGUMENT for a missing
// allocator; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_streamer_create(const NphAudioStreamerDesc *desc,
                                                 NphAudioStreamer **out);

// Stops the thread and frees every stream still open. Destroy the mixer
// that plays its streams (or their clips, updated away) first. Safe on NULL.
void nph_audio_streamer_destroy(NphAudioStreamer *streamer);

// Where a stream's bytes come from.
typedef struct NphAudioStreamSource {
    // Reads `size` bytes at `offset` of the stream's data into `dst`;
    // anything but NPH_RESULT_OK ends the stream (it plays on as silence).
    // Called on the streamer's thread; it may block.
    NphResult (*read)(void *user, u64 offset, void *dst, u64 size);
    void *user;
} NphAudioStreamSource;

typedef struct NphAudioStreamDesc {
    NphAudioStreamSource source;
    NphAudioCodec codec;         // PCM or LOSSLESS
    NphAudioSampleFormat format; // PCM: the samples' format
    u32 channels;
    u32 sample_rate;
    u64 frames; // below 2^62
    u64 size;   // bytes of data at the source
    // Lossless: the depth, block size, and block offsets (copied).
    u32 bits;
    u32 block_frames;
    const u32 *block_offsets;
    u32 block_count;
    // The loop (loop_end 0: the whole stream), and whether it repeats.
    u64 loop_start;
    u64 loop_end;
    b32 loop;
    u64 start_frame;    // where it starts
    f32 buffer_seconds; // the ring's length (0: 1 s)
    // The streamer fills the ring, from its thread, rather than the call
    // that makes the clip: a voice played before the ring holds frames
    // starts dry (silence, counted as starvation), and a source that fails
    // or a corrupt block ends the stream there rather than refusing the
    // clip.
    b32 defer_fill;
} NphAudioStreamDesc;

// A stream whose source fails a read, or whose lossless block fails its
// checks (lossless.h nph_lossless_check_block: its header and CRC), ends
// there: its voice plays on in silence.
typedef struct NphAudioStreamerStats {
    u32 streams;        // open
    u64 frames_decoded; // written to rings
    u64 bytes_read;
    u64 seeks;          // re-reads after a voice outran its stream
    u32 read_errors;    // streams ended by a failed read
    u32 corrupt_blocks; // streams ended by a corrupt block
} NphAudioStreamerStats;

// NPH_THREADSAFE (atomics).
void nph_audio_streamer_stats(const NphAudioStreamer *streamer, NphAudioStreamerStats *out);

#ifdef __cplusplus
}
#endif
