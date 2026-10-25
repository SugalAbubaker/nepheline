// SPDX-License-Identifier: Apache-2.0
#pragma once

// The audio module (CLAUDE.md §8.10; docs/audio.md;
// docs/adr/0051-audio-module.md): the mixer and its voices, buses, effects,
// and spatialisation; the codecs (PCM, our ADPCM, our lossless codec);
// streaming; and the output device, the one place miniaudio is used.
//
// This header holds what every other audio header shares: the log
// category, the limits, the sample formats, the channel layouts, and the
// decibel helpers.
//
// Conventions (ADR-0051):
//   - Signals are 32-bit float, nominally in [-1, 1], planar inside the
//     mixer (one contiguous array per channel), interleaved only at the
//     edges (sources, the device).
//   - The mixer works in blocks of NPH_AUDIO_BLOCK_FRAMES frames; parameter
//     changes ramp linearly across a block, so none clicks.
//   - Directions are in the listener's frame of the engine's convention
//     (ADR-0008: right-handed, Y up, looking along -Z): an azimuth is 0
//     straight ahead, +pi/2 to the right, -pi/2 to the left, and +-pi
//     behind.
//
// The module depends on platform (threads, files, asynchronous I/O, CPU
// features) and through it on math and core; never on asset, scene, or
// physics: the runtime connects cooked sounds, entities, and occlusion
// rays to it.
//
// Thread-safety is stated per header; everything here is pure and
// NPH_THREADSAFE.

#include <nepheline/core/log.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_AUDIO);

// The most channels a signal may have anywhere in the module (7.1).
#define NPH_AUDIO_MAX_CHANNELS 8U

// Frames the mixer renders per block: 5.3 ms at 48 kHz. A multiple of
// eight, so the SIMD kernels run whole vectors.
#define NPH_AUDIO_BLOCK_FRAMES 256U

// The sample rates the module accepts, for sources and for the mix.
#define NPH_AUDIO_MIN_RATE 8000U
#define NPH_AUDIO_MAX_RATE 192000U

// ---------------------------------------------------------------------------
// Sample formats
// ---------------------------------------------------------------------------

// How interleaved samples are stored in a source or a device buffer. The
// values are part of file formats (the cooked sound header) and are never
// renumbered.
typedef enum NphAudioSampleFormat {
    NPH_AUDIO_FORMAT_UNKNOWN = 0,
    NPH_AUDIO_FORMAT_S16 = 1, // signed 16-bit, little-endian
    NPH_AUDIO_FORMAT_S24 = 2, // signed 24-bit, packed in three bytes, little-endian
    NPH_AUDIO_FORMAT_S32 = 3, // signed 32-bit, little-endian
    NPH_AUDIO_FORMAT_F32 = 4, // IEEE-754 binary32, little-endian
    NPH_AUDIO_FORMAT_COUNT = 5
} NphAudioSampleFormat;

// Bytes per sample of `format` (0 for UNKNOWN or an invalid value).
u32 nph_audio_format_bytes(NphAudioSampleFormat format);

// A static name ("s16", "s24", "s32", "f32"; "unknown" otherwise).
const char *nph_audio_format_name(NphAudioSampleFormat format);

// ---------------------------------------------------------------------------
// Channel layouts
// ---------------------------------------------------------------------------

// The speaker layouts the mixer renders. Channels are in the WAVE order
// (Microsoft, WAVEFORMATEXTENSIBLE): front left, front right, front centre,
// low frequency, back left, back right, side left, side right, as the
// layout has them.
typedef enum NphAudioLayout {
    NPH_AUDIO_LAYOUT_MONO = 0,         // C
    NPH_AUDIO_LAYOUT_STEREO = 1,       // L R
    NPH_AUDIO_LAYOUT_QUAD = 2,         // L R Lb Rb
    NPH_AUDIO_LAYOUT_SURROUND_5_1 = 3, // L R C LFE Ls Rs (ITU-R BS.775: surrounds at +-110 deg)
    NPH_AUDIO_LAYOUT_SURROUND_7_1 = 4, // L R C LFE Lb Rb Ls Rs (sides +-90, backs +-150 deg)
    NPH_AUDIO_LAYOUT_COUNT = 5
} NphAudioLayout;

// The speakers, as WAVEFORMATEXTENSIBLE names them. 5.1's surrounds (at
// +-110 degrees) are the side pair, as in Microsoft's
// KSAUDIO_SPEAKER_5POINT1_SURROUND and in the up-mix of 5.1 to 7.1 that
// feeds them to 7.1's sides; quad's and 7.1's back pairs are the backs.
typedef enum NphAudioSpeaker {
    NPH_AUDIO_SPEAKER_FRONT_LEFT = 0,
    NPH_AUDIO_SPEAKER_FRONT_RIGHT = 1,
    NPH_AUDIO_SPEAKER_FRONT_CENTER = 2,
    NPH_AUDIO_SPEAKER_LOW_FREQUENCY = 3,
    NPH_AUDIO_SPEAKER_BACK_LEFT = 4,
    NPH_AUDIO_SPEAKER_BACK_RIGHT = 5,
    NPH_AUDIO_SPEAKER_SIDE_LEFT = 6,
    NPH_AUDIO_SPEAKER_SIDE_RIGHT = 7,
    NPH_AUDIO_SPEAKER_COUNT = 8
} NphAudioSpeaker;

typedef struct NphAudioLayoutInfo {
    const char *name; // "mono", "stereo", "quad", "5.1", "7.1"
    u32 channels;
    u32 lfe; // index of the low-frequency channel, or NPH_AUDIO_NO_LFE
    // Each channel's speaker (mono's one channel is the front centre).
    NphAudioSpeaker speaker[NPH_AUDIO_MAX_CHANNELS];
    // Each channel's azimuth in radians (0 ahead, positive to the right);
    // the LFE channel's is 0 and means nothing: it is not a direction.
    f32 azimuth[NPH_AUDIO_MAX_CHANNELS];
} NphAudioLayoutInfo;

#define NPH_AUDIO_NO_LFE NPH_U32_MAX

// The description of `layout`, or NULL for an invalid value.
const NphAudioLayoutInfo *nph_audio_layout_info(NphAudioLayout layout);

// The layout with `channels` channels (1, 2, 4, 6, or 8).
// NPH_RESULT_UNSUPPORTED for any other count.
NphResult nph_audio_layout_for_channels(u32 channels, NphAudioLayout *out);

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

// The quietest level the module distinguishes from silence: gains at or
// below it are silence (and nph_audio_gain_to_db returns it for them).
#define NPH_AUDIO_SILENCE_DB (-144.0F)

// 10^(db / 20); NPH_AUDIO_SILENCE_DB and below give 0.
f32 nph_audio_db_to_gain(f32 db);

// 20 log10(gain); NPH_AUDIO_SILENCE_DB for gains at or below its level.
f32 nph_audio_gain_to_db(f32 gain);

#ifdef __cplusplus
}
#endif
