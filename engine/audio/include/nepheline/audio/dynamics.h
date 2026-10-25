// SPDX-License-Identifier: Apache-2.0
#pragma once

// Dynamics (CLAUDE.md §8.10 "DSP: ... compressor"; docs/audio.md
// "Effects"): a compressor and a lookahead true-peak limiter.
//
// The compressor is the feed-forward design of Giannoulis, Massberg &
// Reiss ("Digital Dynamic Range Compressor Design - A Tutorial and
// Analysis", JAES 60(6), 2012):
//   - the level is the largest magnitude of the channels' samples
//     (stereo-linked, so the image does not shift), in decibels;
//   - a static curve with a soft knee gives the gain reduction;
//   - the reduction is smoothed in the log domain by their smooth
//     branching peak detector: one time constant while it rises (attack),
//     another while it falls (release);
//   - makeup gain follows, ramped across a block when it changes.
//
// The limiter keeps every sample, and the peaks between samples, at or
// below a ceiling:
//   - its detector estimates the true peak by interpolating each channel
//     at the quarter positions between samples (a 4x oversampling, as
//     ITU-R BS.1770-4 Annex 2 measures true peak, with our own 48-tap
//     Kaiser-windowed sinc);
//   - each sample's required gain (ceiling over its peak, at most 1)
//     recovers exponentially at the release rate, is held at its minimum
//     over the lookahead window, and is averaged over the window, so the
//     gain has fallen to exactly what a peak needs by the time the peak,
//     delayed by the lookahead, comes out: no overshoot, and the gain moves
//     in ramps, not steps.
//
// The limiter delays its signal by nph_audio_limiter_latency frames.
//
// Memory: nph_audio_limiter_init allocates its lines; nothing else
// allocates or locks. Processing is real-time safe.
//
// Thread-safety: a compressor or limiter belongs to one thread at a time.

#include <nepheline/audio/audio.h>
#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// The compressor
// ---------------------------------------------------------------------------

typedef struct NphAudioCompressorParams {
    f32 threshold_db; // where compression starts (the knee's centre)
    f32 ratio;        // >= 1; 1 does nothing, 20 and above is limiting
    f32 knee_db;      // the soft knee's width (0: hard)
    f32 attack_ms;    // the reduction's time constant while it rises
    f32 release_ms;   // and while it falls
    f32 makeup_db;    // gain after compression
} NphAudioCompressorParams;

// Threshold -18 dB, ratio 4, knee 6 dB, attack 10 ms, release 100 ms, no
// makeup.
NphAudioCompressorParams nph_audio_compressor_params_default(void);

typedef struct NphAudioCompressor {
    NphAudioCompressorParams params;
    f32 rate;
    u32 channels;
    f32 attack_coeff; // exp(-1 / (tau rate))
    f32 release_coeff;
    f32 reduction_db; // the smoothed reduction (>= 0)
    f32 makeup;       // the last block's makeup gain (linear)
    f32 deepest_db;   // the largest reduction of the last block (for meters)
} NphAudioCompressor;

void nph_audio_compressor_init(NphAudioCompressor *c, u32 channels, f32 rate,
                               const NphAudioCompressorParams *params);

// New parameters, heard from the next block (the curve at once, through
// the smoothing; the makeup as a ramp).
void nph_audio_compressor_set(NphAudioCompressor *c, const NphAudioCompressorParams *params);

// The static curve's gain reduction for a level of `level_db` (>= 0).
f32 nph_audio_compressor_curve(const NphAudioCompressorParams *params, f32 level_db);

void nph_audio_compressor_reset(NphAudioCompressor *c);

// Compresses `frames` frames of the compressor's channels in place.
void nph_audio_compressor_process(NphAudioCompressor *c, f32 *const *planes, u32 frames);

// ---------------------------------------------------------------------------
// The limiter
// ---------------------------------------------------------------------------

#define NPH_AUDIO_LIMITER_MAX_LOOKAHEAD_MS 20.0F

typedef struct NphAudioLimiterParams {
    f32 ceiling_db;   // the true peak never exceeds this (<= 0)
    f32 release_ms;   // the time constant of the gain's recovery
    f32 lookahead_ms; // fixed at init: 0.5 .. NPH_AUDIO_LIMITER_MAX_LOOKAHEAD_MS
} NphAudioLimiterParams;

// Ceiling -1 dB, release 80 ms, lookahead 1.5 ms.
NphAudioLimiterParams nph_audio_limiter_params_default(void);

// The interpolator's taps per quarter-frame phase.
#define NPH_AUDIO_LIMITER_TAPS 12U

typedef struct NphAudioLimiter {
    NphAudioLimiterParams params;
    f32 rate;
    u32 channels;
    u32 lookahead; // frames (L)
    u32 latency;   // L - 1 + the interpolator's half-width
    f32 ceiling;   // linear
    f32 release_coeff;
    f32 taps[3][NPH_AUDIO_LIMITER_TAPS]; // phases 1/4, 2/4, 3/4
    // Per channel: the input history the interpolator reads, and the delay
    // the signal waits in.
    f32 *history;                        // channels * NPH_AUDIO_LIMITER_TAPS
    f32 segment[NPH_AUDIO_MAX_CHANNELS]; // each channel's last segment peak
    f32 *delay;                          // channels * delay_size
    u32 delay_size;                      // a power of two > latency
    u32 delay_write;
    // The gain path: the recovering gain, its window minimum (a monotonic
    // deque of (value, time) over L frames), and its running sum.
    f32 recover;
    f32 *deque_value;
    u32 *deque_time;
    u32 deque_size; // a power of two > L
    u32 deque_head; // oldest
    u32 deque_tail; // one past the newest
    f32 *window;    // the last L window minima, for the sum
    u32 window_pos;
    f64 window_sum;
    u32 time;
    f32 lowest_gain; // the smallest gain of the last block (for meters)
    NphAllocator *alloc;
} NphAudioLimiter;

// A limiter of `channels` channels at `rate`. NPH_RESULT_INVALID_ARGUMENT
// for parameters out of range; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_limiter_init(NphAudioLimiter *l, NphAllocator *alloc, u32 channels,
                                              f32 rate, const NphAudioLimiterParams *params);

void nph_audio_limiter_free(NphAudioLimiter *l);

// A new ceiling or release (the lookahead stays as it was made).
void nph_audio_limiter_set(NphAudioLimiter *l, const NphAudioLimiterParams *params);

void nph_audio_limiter_reset(NphAudioLimiter *l);

// The frames by which the limiter delays its signal.
u32 nph_audio_limiter_latency(const NphAudioLimiter *l);

// Limits `frames` frames of the limiter's channels in place.
void nph_audio_limiter_process(NphAudioLimiter *l, f32 *const *planes, u32 frames);

// The true peak of a channel at the quarter positions too, measured with
// the limiter's interpolator over `count` samples (from silence before
// them). For tests and meters.
f32 nph_audio_true_peak(const NphAudioLimiter *l, const f32 *x, u32 count);

#ifdef __cplusplus
}
#endif
