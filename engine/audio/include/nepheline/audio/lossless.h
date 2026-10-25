// SPDX-License-Identifier: Apache-2.0
#pragma once

// The lossless codec (CLAUDE.md §8.10 "the compressed codec ... chosen via
// an ADR"; docs/audio.md "The lossless codec"; docs/adr/0052-audio-codecs.md):
// music, ambience, and dialogue stored bit-exact at about half their size.
//
// The design is FLAC's (Coalson, "FLAC - Free Lossless Audio Codec",
// 2000-2007; RFC 9639, 2024): each channel of a block is predicted by a
// fixed polynomial or a linear predictor fitted to it (Levinson-Durbin on
// the windowed autocorrelation; Makhoul, "Linear Prediction: A Tutorial
// Review", Proc. IEEE 63(4), 1975), stereo pairs are decorrelated (left,
// right, side, mid), and what the predictor misses is coded with
// partitioned Rice codes (Rice, "Some practical universal noiseless coding
// techniques", JPL 79-22, 1979). The bitstream is our own, laid out for a
// game's mixer rather than for a file:
//   - Blocks (4096 frames by default) decode on their own and are found
//     through an offset table, so a voice can start or jump anywhere.
//   - A block lists where each channel's data starts, so all channels
//     decode in lockstep, a mixer block at a time, without a buffer of the
//     whole block per voice: a voice's decoder state is a few hundred bytes.
//   - Bits are read least-significant first from little-endian words, so the
//     decoder refills with one load and finds a Rice code's unary part with
//     a count of trailing zeros.
//   - Every block carries a CRC-32C, checked off the audio thread when a
//     clip is made or a stream reads the block; the decoder itself is
//     bounded on any input, so a corrupt block it is given anyway yields
//     NPH_RESULT_CORRUPT, never a fault (fuzzed: tests/fuzz).
//
// Samples are signed integers of 4 to 24 bits. (Floats exactly
// representable in 24 bits are encoded as such; other floats stay PCM: the
// cooker's choice, docs/cook.md.)
//
// Block layout (all little-endian):
//
//     u32 crc        CRC-32C of the block's bytes after this field
//     u32 size       the block's size in bytes, this header included
//     u16 frames     frames in the block (1 .. 65535)
//     u8  stereo     NphLosslessStereo (0 for one channel or more than two)
//     u8  channels
//     u32 offset[channels]   each channel's subframe, from the block's start
//     subframes      byte-aligned, in channel order
//
// Subframe: u8 kind (0 constant, 1 verbatim, 2 + order for a fixed predictor
// of order 0 .. 4, 31 + order for linear prediction of order 1 .. 32), u8
// wasted (low bits that are zero in every sample, shifted out), then a bit
// stream: the constant or the verbatim samples, or the warm-up samples, the
// coefficients (linear prediction: precision - 1 in 4 bits, shift in 5
// bits, then each coefficient), and the residual (partition order in 4
// bits; per partition a Rice parameter in 5 bits, 31 escaping to raw
// samples of a 5-bit width, then the zigzag-mapped values as Rice codes).
//
// Memory (§1.4): the encoder allocates its output from the caller's
// allocator, and its scratch; the decoder allocates nothing and keeps its
// state in the caller's NphLosslessDecoder.
//
// Thread-safety: the encoder and the validation are pure (NPH_THREADSAFE);
// a decoder belongs to one thread at a time.

#include <nepheline/audio/audio.h>
#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_LOSSLESS_MAX_ORDER         32U
#define NPH_LOSSLESS_DEFAULT_BLOCK     4096U
#define NPH_LOSSLESS_MAX_BLOCK         65535U
#define NPH_LOSSLESS_MIN_BITS          4U
#define NPH_LOSSLESS_MAX_BITS          24U
#define NPH_LOSSLESS_BLOCK_HEADER_SIZE 12U

// How a stereo block's two subframes hold left and right.
typedef enum NphLosslessStereo {
    NPH_LOSSLESS_STEREO_INDEPENDENT = 0, // left, right
    NPH_LOSSLESS_STEREO_LEFT_SIDE = 1,   // left, left - right
    NPH_LOSSLESS_STEREO_SIDE_RIGHT = 2,  // left - right, right
    NPH_LOSSLESS_STEREO_MID_SIDE = 3,    // (left + right) >> 1, left - right
    NPH_LOSSLESS_STEREO_COUNT = 4
} NphLosslessStereo;

// ---------------------------------------------------------------------------
// The encoded stream
// ---------------------------------------------------------------------------

// A stream: its blocks back to back in `data`, block i at block_offsets[i]
// (and ending where the next begins, or at `size`). Every block but the
// last holds block_frames frames.
typedef struct NphLosslessStream {
    u32 channels;
    u32 bits;
    u32 block_frames;
    u64 frames;
    const u8 *data;
    u64 size;
    const u32 *block_offsets;
    u32 block_count;
} NphLosslessStream;

// The blocks a stream of `frames` frames has at `block_frames` per block.
NPH_INLINE u32 nph_lossless_block_count(u64 frames, u32 block_frames)
{
    return block_frames == 0U ? 0U : (u32)((frames + block_frames - 1U) / block_frames);
}

// Checks a stream's description and every block's header and CRC: what a
// clip is checked with when it is made, off the audio thread.
// NPH_RESULT_INVALID_ARGUMENT for impossible parameters (channels, bits,
// block size, a block count that does not match the frames);
// NPH_RESULT_CORRUPT for offsets outside the data or out of order, or a
// block whose header or CRC is wrong.
NPH_MUST_USE NphResult nph_lossless_validate(const NphLosslessStream *stream);

// Checks one block: its size against `size`, its frames, its channels, its
// subframe offsets, and its CRC (the streamer checks each block it reads).
NPH_MUST_USE NphResult nph_lossless_check_block(const u8 *block, u64 size, u32 channels,
                                                u32 expected_frames);

// ---------------------------------------------------------------------------
// Encoding (offline: the cooker, tools, tests)
// ---------------------------------------------------------------------------

typedef struct NphLosslessParams {
    u32 block_frames; // 0: NPH_LOSSLESS_DEFAULT_BLOCK
    u32 max_order;    // highest linear-prediction order tried (0: 32)
    b32 exhaustive;   // try every prediction order (else the four its error estimate ranks best)
} NphLosslessParams;

// An encoder's output, owned by `alloc`.
typedef struct NphLosslessEncoded {
    NphLosslessStream stream;
    u8 *data;
    u32 *block_offsets;
    u64 data_capacity;
    NphAllocator *alloc;
} NphLosslessEncoded;

// Encodes `frames` frames of `channels` channels (1 .. 8) of `bits`-bit
// signed samples (4 .. 24), interleaved in `samples` (each in range for its
// bits). NPH_RESULT_INVALID_ARGUMENT for parameters out of range or a
// sample that does not fit; NPH_RESULT_OVERFLOW for a stream past 4 GiB;
// NPH_RESULT_OUT_OF_MEMORY. Free the result with nph_lossless_encoded_free.
NPH_MUST_USE NphResult nph_lossless_encode(NphAllocator *alloc, const i32 *samples, u32 channels,
                                           u32 bits, u64 frames, const NphLosslessParams *params,
                                           NphLosslessEncoded *out);

void nph_lossless_encoded_free(NphLosslessEncoded *encoded);

// ---------------------------------------------------------------------------
// Decoding (the audio thread: no allocation, no locks, bounded work)
// ---------------------------------------------------------------------------

// The decoding state of one channel within a block.
typedef struct NphLosslessChannelState {
    const u8 *cursor; // the next unread bytes of the subframe
    const u8 *end;    // the subframe's end
    u64 bits;         // bits loaded and not yet used, least significant first
    u32 bit_count;
    u32 kind;
    u32 order;
    u32 wasted;
    u32 sample_bits; // bits of a stored sample: the depth, less wasted, plus one for a side
    u32 shift;       // linear prediction's quantisation shift
    u32 produced;    // samples of the block produced so far
    u32 partition_left;
    u32 partition_index;
    u32 partition_order;
    u32 rice;     // the current partition's parameter, or NPH_U32_MAX for raw
    u32 raw_bits; // the escape's width
    i32 constant;
    i32 coeffs[NPH_LOSSLESS_MAX_ORDER];
    // The newest NPH_LOSSLESS_MAX_ORDER samples, written twice (at i and
    // i + 32) so the predictor reads them as one contiguous window.
    i32 history[2U * NPH_LOSSLESS_MAX_ORDER];
    u32 history_at;
} NphLosslessChannelState;

typedef struct NphLosslessDecoder {
    u32 channels;
    u32 bits;
    u32 frames;   // in the current block
    u32 produced; // frames of it produced
    u32 stereo;   // NphLosslessStereo
    b32 failed;   // the block was corrupt: no more frames come from it
    NphLosslessChannelState ch[NPH_AUDIO_MAX_CHANNELS];
} NphLosslessDecoder;

// Starts decoding a block of `size` bytes. Reads the header and each
// subframe's set-up (kind, warm-up samples, coefficients); the CRC is not
// checked here (see above). NPH_RESULT_CORRUPT for anything malformed (the
// decoder then produces nothing until the next begin);
// NPH_RESULT_INVALID_ARGUMENT for a channel count or depth out of range.
NPH_MUST_USE NphResult nph_lossless_decoder_begin(NphLosslessDecoder *dec, const u8 *block,
                                                  u64 size, u32 channels, u32 bits);

// Frames of the current block not yet produced.
NPH_INLINE u32 nph_lossless_decoder_remaining(const NphLosslessDecoder *dec)
{
    return dec->failed ? 0U : dec->frames - dec->produced;
}

// Decodes the next `count` frames (at most the remaining) of every channel
// into out[c][0 .. count) as integers at the stream's depth. NPH_RESULT_OK,
// or NPH_RESULT_CORRUPT when the data is malformed (what was produced
// before the fault is valid; the rest of the block is lost).
NPH_MUST_USE NphResult nph_lossless_decoder_read_int(NphLosslessDecoder *dec, i32 *const *out,
                                                     u32 count);

// As nph_lossless_decoder_read_int, converted to floats in [-1, 1):
// x / 2^(bits - 1). `scratch` holds count * channels integers.
NPH_MUST_USE NphResult nph_lossless_decoder_read(NphLosslessDecoder *dec, f32 *const *out,
                                                 u32 count, i32 *scratch);

// Decodes and discards `count` frames (a seek within a block).
NPH_MUST_USE NphResult nph_lossless_decoder_skip(NphLosslessDecoder *dec, u32 count);

// Decodes a whole stream into interleaved integers (count: stream->frames *
// channels). For tools and tests. The stream is validated first.
NPH_MUST_USE NphResult nph_lossless_decode_all(const NphLosslessStream *stream, i32 *out);

#ifdef __cplusplus
}
#endif
