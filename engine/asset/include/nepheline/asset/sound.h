// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cooked sounds (CLAUDE.md §8.8 "Materials, scenes, audio clips"; §8.10;
// docs/asset.md "Sounds"; docs/adr/0057-sound-assets.md).
//
// A sound asset (NPH_ASSET_TYPE_SOUND) holds a header blob, the encoded
// samples, and, for the lossless codec, where each block starts:
//
//     HEAD      NphSoundHeader
//     DATA      the samples: interleaved PCM, our ADPCM stream, or our
//               lossless stream's blocks (audio/adpcm.h, audio/lossless.h)
//     BLKS      a u32 per block, its offset in DATA (lossless only)
//
// DATA is stored, never LZ-compressed: the codecs are the compression, and
// a sound that streams is read from the file where DATA lies, a block at a
// time (NphSoundView.offset). Codecs and sample formats are pinned numbers
// that the runtime maps to the audio module's; they never depend on them.
//
// The view checks the structure: the header's ranges, DATA's size for PCM,
// and that the lossless blocks tile DATA in order. The samples are checked
// where they are decoded, off the audio thread: a clip checks every
// lossless block's CRC and an ADPCM stream's size and shifts when it is
// made, and a stream checks each block as it reads it.
//
// Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/asset/loader.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_SOUND_TYPE_VERSION 1U
#define NPH_SOUND_TAG_HEADER   NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_SOUND_TAG_DATA     NPH_FOURCC('D', 'A', 'T', 'A')
#define NPH_SOUND_TAG_BLOCKS   NPH_FOURCC('B', 'L', 'K', 'S')
#define NPH_SOUND_MAX_CHANNELS 8U
#define NPH_SOUND_MIN_RATE     8000U
#define NPH_SOUND_MAX_RATE     192000U
// Frames a sound played from memory may have (audio/mixer.h
// NPH_AUDIO_MAX_CLIP_FRAMES: 12.4 hours at 48 kHz), and one that streams.
#define NPH_SOUND_MAX_FRAMES        (1ULL << 31U)
#define NPH_SOUND_MAX_STREAM_FRAMES (1ULL << 40U)

// Values are part of the file format and never renumbered.
typedef enum NphSoundCodec {
    NPH_SOUND_CODEC_PCM = 1,      // interleaved samples in `format`
    NPH_SOUND_CODEC_ADPCM = 2,    // audio/adpcm.h, from 16-bit samples; played from memory only
    NPH_SOUND_CODEC_LOSSLESS = 3, // audio/lossless.h, of `bits`-bit samples
} NphSoundCodec;

// PCM's sample formats (NONE for the other codecs).
typedef enum NphSoundFormat {
    NPH_SOUND_FORMAT_NONE = 0,
    NPH_SOUND_FORMAT_S16 = 1, // signed 16-bit, little-endian
    NPH_SOUND_FORMAT_S24 = 2, // signed 24-bit, packed in three bytes, little-endian
    NPH_SOUND_FORMAT_S32 = 3, // signed 32-bit, little-endian
    NPH_SOUND_FORMAT_F32 = 4, // IEEE-754 binary32, little-endian
} NphSoundFormat;

typedef enum NphSoundFlags {
    // Played from storage through a stream (audio/stream.h) rather than
    // from memory: what the cooker chose for long sounds.
    NPH_SOUND_FLAG_STREAM = 1U << 0U,
} NphSoundFlags;

typedef struct NphSoundHeader {
    u32 codec;       // NphSoundCodec
    u32 format;      // NphSoundFormat: PCM's, else NONE
    u32 channels;    // 1 .. NPH_SOUND_MAX_CHANNELS, in the WAVE order (audio/audio.h)
    u32 sample_rate; // NPH_SOUND_MIN_RATE .. NPH_SOUND_MAX_RATE
    u64 frames;      // 1 .. NPH_SOUND_MAX_FRAMES (NPH_SOUND_MAX_STREAM_FRAMES streamed)
    // The loop a looping voice repeats, [loop_start, loop_end) in frames:
    // the source's, or the whole sound (0 and `frames`).
    u64 loop_start;
    u64 loop_end;
    u32 bits;         // the samples' depth: the format's for PCM, 16 for ADPCM, 4 .. 24 lossless
    u32 block_frames; // lossless: frames per block, else 0
    u32 block_count;  // lossless: blocks (BLKS's entries), else 0
    u32 flags;        // NphSoundFlags
    u32 reserved[2];
} NphSoundHeader;

NPH_STATIC_ASSERT(sizeof(NphSoundHeader) == 64U, "the sound header is 64 bytes");

typedef struct NphSoundView {
    const NphSoundHeader *header;
    const u8 *data; // DATA's bytes
    u64 size;
    u64 offset;               // where DATA begins in the asset's file (a stream reads it there)
    const u32 *block_offsets; // BLKS (lossless), else NULL
} NphSoundView;

// Bytes of one PCM sample of `format` (0 for NONE and unknown values).
u32 nph_sound_format_bytes(u32 format);

// Resolves and validates a READY sound asset (see above).
// NPH_RESULT_TYPE_MISMATCH and NPH_RESULT_VERSION_MISMATCH for another type
// or schema; NPH_RESULT_CORRUPT for anything out of range.
NPH_MUST_USE NphResult nph_sound_view(const NphAsset *asset, NphSoundView *out);

#ifdef __cplusplus
}
#endif
