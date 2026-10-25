// SPDX-License-Identifier: Apache-2.0
#pragma once

// WAV files (CLAUDE.md §8.10 "WAV/PCM ... written by us"; docs/audio.md
// "WAV"): the cooker's source format for sounds, and what tests and tools
// write renders to.
//
// The reader takes a whole file in memory and describes it without copying:
// the format, the channels, the rate, the frame count, a pointer to the
// interleaved samples inside the image, and the loop the file's sampler
// chunk names. It reads the RIFF/WAVE container (Microsoft, "Multimedia
// Programming Interface and Data Specifications 1.0", 1991) with integer
// PCM of 8, 16, 24, or 32 bits and IEEE floats of 32 or 64 bits, plain or
// WAVE_FORMAT_EXTENSIBLE, and skips every other chunk. It is the only
// entry point for untrusted bytes here: every size is checked against the
// image, and malformed input yields NPH_RESULT_PARSE_ERROR or
// NPH_RESULT_UNSUPPORTED, never a fault (fuzzed: tests/fuzz).
//
// The writer produces 16-bit, 24-bit, or float files from planar floats.
//
// Thread-safety: pure functions; NPH_THREADSAFE.

#include <nepheline/audio/audio.h>
#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

// How a WAV file stores its samples. Unsigned 8-bit and 64-bit floats have
// no NphAudioSampleFormat: the cooker converts them (exactly, for 8-bit).
typedef enum NphWavEncoding {
    NPH_WAV_PCM_U8 = 0,
    NPH_WAV_PCM_S16 = 1,
    NPH_WAV_PCM_S24 = 2,
    NPH_WAV_PCM_S32 = 3,
    NPH_WAV_FLOAT32 = 4,
    NPH_WAV_FLOAT64 = 5,
} NphWavEncoding;

typedef struct NphWavInfo {
    NphWavEncoding encoding;
    u32 channels;        // 1 .. NPH_AUDIO_MAX_CHANNELS
    u32 sample_rate;     // NPH_AUDIO_MIN_RATE .. NPH_AUDIO_MAX_RATE
    u32 bytes_per_frame; // channels * the sample's bytes
    u64 frames;
    const void *data; // the interleaved samples, inside the image
    u64 data_size;    // frames * bytes_per_frame
    // The first loop of the sampler chunk ('smpl'), as frames: [loop_start,
    // loop_end) with loop_end exclusive. has_loop is false without one.
    b32 has_loop;
    u64 loop_start;
    u64 loop_end;
} NphWavInfo;

// Describes the WAV file image [data, data + size). NPH_RESULT_PARSE_ERROR
// for a malformed container (sizes beyond the image, no 'fmt ' or 'data',
// a format chunk too short); NPH_RESULT_UNSUPPORTED for a valid file in a
// format the reader does not take (compressed, more than eight channels, a
// rate out of range, bit depths other than those above). A data chunk that
// claims more bytes than the file holds is truncated to whole frames (a
// common writer bug), not refused.
NPH_MUST_USE NphResult nph_wav_parse(const void *data, u64 size, NphWavInfo *out);

// Bytes nph_wav_write needs for `frames` frames of `channels` channels in
// `encoding` (16-bit, 24-bit, or 32-bit float; 0 for anything else).
u64 nph_wav_size(NphWavEncoding encoding, u32 channels, u64 frames);

// Writes a WAV image of `frames` frames from planar channels (each sample
// clamped to [-1, 1], rounded to nearest for the integer encodings) into
// `out` (nph_wav_size bytes). NPH_RESULT_INVALID_ARGUMENT for an encoding
// the writer does not produce, a channel count of 0 or above
// NPH_AUDIO_MAX_CHANNELS, a rate out of range, or a file past 4 GiB.
NPH_MUST_USE NphResult nph_wav_write(NphWavEncoding encoding, u32 channels, u32 sample_rate,
                                     const f32 *const *planar, u64 frames, void *out);

// Reads one sample of channel `channel` of frame `frame` as an exact
// signed integer at the file's own depth (U8 is centred: x - 128) when the
// encoding is integer PCM; floats return 0. For the cooker and tests.
i32 nph_wav_sample_int(const NphWavInfo *info, u64 frame, u32 channel);

// Reads one sample as a float in [-1, 1) (integers: x / 2^(bits-1)).
f64 nph_wav_sample_float(const NphWavInfo *info, u64 frame, u32 channel);

#ifdef __cplusplus
}
#endif
