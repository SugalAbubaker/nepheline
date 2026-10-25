// SPDX-License-Identifier: Apache-2.0
#pragma once

// A stream between its streamer and the mixer (stream.h): the ring the
// streamer's thread fills and the audio thread drains, and what each side
// keeps of its own.

#include <nepheline/audio/lossless.h>
#include <nepheline/audio/stream.h>
#include <nepheline/core/atomic.h>

// The frames the streamer decodes at a time.
#define MIX_STREAM_CHUNK 1024U

typedef struct MixStream {
    // Set when opened, then read-only.
    NphAllocator *alloc;
    NphAudioStreamSource source;
    NphAudioCodec codec;
    NphAudioSampleFormat format;
    u32 channels;
    u32 sample_rate;
    u32 bytes_per_frame; // PCM
    u32 bits;            // lossless
    u32 block_frames;
    u32 block_count;
    u32 *block_offsets; // the stream's own copy
    u64 frames;
    u64 size;
    u64 loop_start;
    u64 loop_end;
    b32 loop;
    u64 start_frame;
    u64 total; // the frames the voice sees: endless (MIX_STREAM_ENDLESS) when looping
    // The ring: planar, `capacity` frames (a power of two) per channel.
    f32 *ring[NPH_AUDIO_MAX_CHANNELS];
    u32 capacity;
    // Absolute stream frames: written so far (the streamer's), and taken
    // so far (the voice's). Each side reads the other's with acquire and
    // publishes its own with release.
    NphAtomicU64 produced;
    NphAtomicU64 consumed;
    // The streamer's: its decoder, its place, its scratch.
    NphLosslessDecoder decoder;
    u64 decoder_frame; // the clip frame the decoder gives next (MIX_STREAM_NOWHERE: none)
    u8 *scratch;       // a block's bytes (lossless) or a chunk's (PCM)
    u64 scratch_size;
    f32 *chunk[NPH_AUDIO_MAX_CHANNELS]; // MIX_STREAM_CHUNK decoded frames
    i32 *ints;                          // the lossless decoder's (channels * MIX_STREAM_CHUNK)
    NphAtomicU32 failed; // 0, or why no more frames come (NPH_RESULT_IO_ERROR, _CORRUPT)
} MixStream;

#define MIX_STREAM_ENDLESS (NPH_U64_MAX / 4U)
#define MIX_STREAM_NOWHERE NPH_U64_MAX

// Opens a stream on `streamer` (the game thread): validates `desc`, makes
// the ring and the decoder, fills the ring's first half here, and hands
// the stream to the streamer's thread.
NphResult mix_stream_open(NphAudioStreamer *streamer, const NphAudioStreamDesc *desc,
                          MixStream **out);

// Closes it (the game thread, once the audio thread has let go): the
// streamer's thread frees it.
void mix_stream_close(NphAudioStreamer *streamer, MixStream *stream);

// The audio thread: stream frames [frame, frame + count) into dst[c] + at,
// silence where they have not arrived or are already gone. Returns the
// frames that had not arrived (starvation).
u32 mix_stream_read(MixStream *stream, u64 frame, u32 count, f32 *const *dst, u32 at);

// The audio thread: the voice reads from stream frame `frame` on (it is
// virtual, and its time moves on), so the frames before it are dropped and
// the streamer keeps the ring ahead of it. Never moves the read index back.
void mix_stream_skip(MixStream *stream, u64 frame);
