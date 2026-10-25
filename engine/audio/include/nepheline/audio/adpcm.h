// SPDX-License-Identifier: Apache-2.0
#pragma once

// Our ADPCM (CLAUDE.md §8.10 "an ADPCM variant, written by us";
// docs/audio.md "ADPCM"; docs/adr/0052-audio-codecs.md): a lossy 4.3 bits a
// sample for effects that ask for it, decoded at almost no cost. Music,
// ambience, and dialogue use the lossless codec (lossless.h); effects use
// it or PCM unless their import settings choose this.
//
// Each block holds 128 frames of one channel in 69 bytes: the two samples
// before the block (so it decodes on its own, anywhere), a byte naming one
// of the clip's 16 second-order predictors and a shift, and 128 four-bit
// codes. A sample is the prediction plus the code scaled by the shift:
//
//     p = (c1 s[n-1] + c2 s[n-2] + 1024) >> 11       (c1, c2 in Q11)
//     s[n] = clamp16(p + (code << shift))            (code in [-8, 7])
//
// as in the block-adaptive ADPCM of the GameCube's DSP and of Microsoft
// ADPCM (Microsoft, "Multimedia Standards Update", 1994): predictors per
// block rather than one step size adapted per sample (IMA, 1992), which is
// what keeps tonal sounds clean. The encoder fits the clip's 16 predictor
// pairs to it (least squares per block, then weighted k-means, seeded with
// Microsoft's seven standard pairs) and, per block, tries every predictor
// and shift in closed loop (quantising against its own reconstruction, as
// the decoder will see it), choosing per sample the code with the least
// error after clamping.
//
// Stream layout: the predictor table (16 pairs of little-endian i16, 64
// bytes), then the blocks, frame-block by frame-block, each channel's
// block in turn. The last frame-block may be partial; its unused codes are
// zero.
//
// Thread-safety: pure functions; NPH_THREADSAFE. Decoding allocates
// nothing and is safe on the audio thread.

#include <nepheline/audio/audio.h>
#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_ADPCM_BLOCK_FRAMES 128U
#define NPH_ADPCM_BLOCK_BYTES  69U
#define NPH_ADPCM_PREDICTORS   16U
#define NPH_ADPCM_TABLE_BYTES  64U
#define NPH_ADPCM_MAX_SHIFT    12U

typedef struct NphAdpcmStream {
    u32 channels;
    u64 frames;
    const u8 *data; // the table, then the blocks
    u64 size;
} NphAdpcmStream;

// Bytes of a stream of `frames` frames of `channels` channels.
u64 nph_adpcm_size(u32 channels, u64 frames);

// Checks the size against the frames and every block's shift.
// NPH_RESULT_INVALID_ARGUMENT for a channel count out of range;
// NPH_RESULT_CORRUPT otherwise.
NPH_MUST_USE NphResult nph_adpcm_validate(const NphAdpcmStream *stream);

// Decodes frames [first, first + count) of every channel into out[c]
// (floats in [-1, 1)), from any position: blocks decode on their own.
// Frames past the stream's end read as silence. Bounded on any input (a
// corrupt shift is clamped), so it never faults.
void nph_adpcm_decode(const NphAdpcmStream *stream, u64 first, u32 count, f32 *const *out);

// As nph_adpcm_decode into 16-bit integers, interleaved (count * channels).
void nph_adpcm_decode_int(const NphAdpcmStream *stream, u64 first, u32 count, i16 *out);

typedef struct NphAdpcmEncoded {
    NphAdpcmStream stream;
    u8 *data;
    NphAllocator *alloc;
} NphAdpcmEncoded;

// Encodes `frames` frames of `channels` channels (1 .. 8) of interleaved
// 16-bit samples. NPH_RESULT_INVALID_ARGUMENT for bad parameters;
// NPH_RESULT_OUT_OF_MEMORY. Free with nph_adpcm_encoded_free.
NPH_MUST_USE NphResult nph_adpcm_encode(NphAllocator *alloc, const i16 *samples, u32 channels,
                                        u64 frames, NphAdpcmEncoded *out);

void nph_adpcm_encoded_free(NphAdpcmEncoded *encoded);

#ifdef __cplusplus
}
#endif
