// SPDX-License-Identifier: Apache-2.0
#pragma once

// The output device (CLAUDE.md §8.10 "miniaudio is used only to open the
// output device and receive buffer callbacks"; docs/audio.md "The device";
// docs/adr/0056-audio-device-streaming.md).
//
// A device renders a mixer from the system's audio thread: each callback
// is one nph_audio_mixer_render of the frames the system asks for, into
// its buffer, in the mixer's layout and rate. Make the mixer at the format
// nph_audio_device_query reports and the mix reaches the hardware
// unconverted. A format the hardware lacks is converted at the edge, by
// the system (WASAPI's resampler) or by miniaudio's (linear, its steepest
// low-pass): the stats show the hardware's format, so the caller can see a
// conversion and make the mixer again at the hardware's.
//
// When the default output changes (headphones unplugged), WASAPI's device
// follows it, converting if the new one's format differs; when none is
// left, the callbacks stop.
//
// Backends: the platform's (WASAPI on Windows, its thread in MMCSS's "Pro
// Audio" class; PulseAudio, PipeWire through it, or ALSA on Linux) or the
// null device, a thread that asks for periods in real time with no
// hardware: headless runs and CI.
//
// miniaudio is used for nothing else: no decoding, mixing, or resampling of
// ours passes through it (the mixer renders at the device's rate). Its
// memory comes from the device's allocator, when it is opened; the
// callback allocates nothing.
//
// Thread-safety: open, close, start, and stop are NPH_MAIN_THREAD_ONLY;
// nph_audio_device_stats is NPH_THREADSAFE. The mixer's other side (its
// commands) stays the game thread's.

#include <nepheline/audio/mixer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAudioDevice NphAudioDevice;

typedef enum NphAudioBackend {
    NPH_AUDIO_BACKEND_SYSTEM = 0, // the platform's own
    NPH_AUDIO_BACKEND_NULL = 1,   // no hardware: periods asked for in real time
    NPH_AUDIO_BACKEND_COUNT = 2
} NphAudioBackend;

typedef struct NphAudioDeviceInfo {
    u32 sample_rate; // the default output device's own rate
    u32 channels;    // and channel count, as one of the mixer's layouts' (1, 2, 4, 6, 8)
    char name[128];  // its name, UTF-8
} NphAudioDeviceInfo;

// The default output device's own format on `backend`, so the mixer can be
// made at it. NPH_RESULT_UNSUPPORTED when the backend has no device (no
// sound hardware, no server).
NPH_MUST_USE NphResult nph_audio_device_query(NphAllocator *alloc, NphAudioBackend backend,
                                              NphAudioDeviceInfo *out);

typedef struct NphAudioDeviceDesc {
    NphAllocator *alloc;  // required: the device's memory and miniaudio's
    NphAudioMixer *mixer; // required: rendered at its own rate and layout
    NphAudioBackend backend;
    u32 period_frames; // the device's period (0: 256, a mixer block)
} NphAudioDeviceDesc;

// Opens the default output device for the mixer, stopped.
// NPH_RESULT_INVALID_ARGUMENT for a missing allocator or mixer;
// NPH_RESULT_UNSUPPORTED when the backend has no device;
// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_device_open(const NphAudioDeviceDesc *desc, NphAudioDevice **out);

// Stops the device if it runs, and releases it. Safe on NULL.
void nph_audio_device_close(NphAudioDevice *device);

// Starts the callbacks. NPH_RESULT_UNSUPPORTED if the system refuses.
NPH_MUST_USE NphResult nph_audio_device_start(NphAudioDevice *device);

// Stops them; when this returns, no callback runs.
void nph_audio_device_stop(NphAudioDevice *device);

typedef struct NphAudioDeviceStats {
    NphAudioBackend backend;
    u32 sample_rate; // as opened: the mixer's
    u32 channels;
    u32 device_sample_rate; // the hardware's: differs from sample_rate when the edge converts
    u32 device_channels;
    u32 period_frames; // as the system granted it
    u64 callbacks;     // since it was opened
    u64 frames;        // rendered
    u64 longest_ns;    // the slowest callback
} NphAudioDeviceStats;

void nph_audio_device_stats(const NphAudioDevice *device, NphAudioDeviceStats *out);

#ifdef __cplusplus
}
#endif
