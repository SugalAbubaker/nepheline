// SPDX-License-Identifier: Apache-2.0
#pragma once

// Sound cooking (CLAUDE.md §8.8 "audio clips", §8.10; docs/cook.md
// "Sounds"; docs/adr/0057-sound-assets.md): a WAV file becomes a sound
// asset (engine/asset/sound.h), its samples in one of our codecs, played
// from memory or streamed from storage.
//
// The choices (CookSoundSettings):
//   - Streaming. AUTO streams sounds of `stream_seconds` or longer (music,
//     ambience, long dialogue) and keeps shorter ones in memory; ALWAYS and
//     NEVER decide outright.
//   - The codec. AUTO keeps every sample, whichever way the sound plays:
//     PCM in memory (nothing to decode as it plays), our lossless codec
//     streamed (half the bytes or so to read). PCM and LOSSLESS ask for
//     one. ADPCM, a quarter of 16-bit PCM and lossy, is for short effects
//     where memory counts more than the last bits; it plays from memory
//     only.
//   - The loop: the file's first sampler loop ('smpl'), else the whole
//     sound.
//
// Samples are kept exactly. Integers of 8 to 24 bits go to the lossless
// codec at their depth, and 32-bit ones at 24 when their low byte is
// always zero; floats go at 24 bits only when every sample is exactly such
// an integer over 2^23. Anything else the lossless codec cannot hold
// exactly stays PCM, at its own depth, and the log says so. 8-bit PCM
// becomes 16-bit (exactly), 64-bit floats 32-bit ones (rounded, logged
// when that changed a sample). ADPCM codes 16-bit samples: deeper ones are
// rounded to 16 bits first, far below its own noise.
//
// Per-source settings: a JSON object in a file beside the source, named as
// the source with ".import.json" appended (boom.wav.import.json),
// overrides the cooker's defaults for that source:
//
//     {"codec": "adpcm", "stream": "never"}
//
// Keys: "codec" ("auto", "pcm", "lossless", "adpcm"), "stream" ("auto",
// "always", "never"), "stream_seconds" (above 0), "exhaustive" (true or
// false). An unknown key or value fails the source's cook; the file's
// bytes count in the source's content hash, so editing it recooks.
//
// Thread-safety: NPH_THREADSAFE for distinct arguments.

#include <nepheline/asset/format.h>
#include <nepheline/asset/sound.h>
#include <nepheline/audio/wav.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CookSoundCodec {
    COOK_SOUND_CODEC_AUTO = 0,
    COOK_SOUND_CODEC_PCM = 1,
    COOK_SOUND_CODEC_LOSSLESS = 2,
    COOK_SOUND_CODEC_ADPCM = 3,
} CookSoundCodec;

typedef enum CookSoundStream {
    COOK_SOUND_STREAM_AUTO = 0,
    COOK_SOUND_STREAM_ALWAYS = 1,
    COOK_SOUND_STREAM_NEVER = 2,
} CookSoundStream;

// AUTO's threshold when the settings leave it 0.
#define COOK_SOUND_STREAM_SECONDS 8.0F

// Zero-filled settings are the defaults.
typedef struct CookSoundSettings {
    CookSoundCodec codec;
    CookSoundStream stream;
    f32 stream_seconds; // AUTO streams sounds at least this long (0: COOK_SOUND_STREAM_SECONDS)
    b32 exhaustive;     // lossless: every prediction order (a slower cook, a smaller file)
} CookSoundSettings;

// Defaults: AUTO codec, AUTO streaming from COOK_SOUND_STREAM_SECONDS, the
// lossless encoder's usual search.
void cook_sound_settings_default(CookSoundSettings *out);

// Applies per-source settings (the JSON above) over `inout`; on failure
// `inout` is unchanged. NPH_RESULT_PARSE_ERROR for malformed JSON or
// anything but an object; NPH_RESULT_INVALID_ARGUMENT for an unknown key
// or value.
NPH_MUST_USE NphResult cook_sound_settings_parse(NphAllocator *alloc, const void *json, u64 size,
                                                 CookSoundSettings *inout);

// What cook_sound makes of a source: whether it streams, its codec, its
// PCM format (NPH_SOUND_FORMAT_NONE for the other codecs), and its depth.
typedef struct CookSoundPlan {
    b32 stream;
    NphSoundCodec codec;
    NphSoundFormat format;
    u32 bits;
} CookSoundPlan;

// The plan for `wav` under `settings`. NPH_RESULT_INVALID_ARGUMENT for what
// the settings cannot take (ADPCM streamed) or a sound too long to play
// from memory (NPH_SOUND_MAX_FRAMES) that may not stream.
NPH_MUST_USE NphResult cook_sound_plan(const NphWavInfo *wav, const CookSoundSettings *settings,
                                       CookSoundPlan *out);

// Cooks the parsed WAV `wav` into `writer` (initialised by the caller with
// the asset's type, GUID, and content hash): HEAD, DATA, and, for the
// lossless codec, BLKS. cook_sound_plan's errors; the encoders' errors
// (NPH_RESULT_OVERFLOW for a lossless stream past 4 GiB);
// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult cook_sound(NphAllocator *alloc, const NphWavInfo *wav,
                                  const CookSoundSettings *settings, NphAssetWriter *writer);

#ifdef __cplusplus
}
#endif
