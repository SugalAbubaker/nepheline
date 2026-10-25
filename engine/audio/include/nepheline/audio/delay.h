// SPDX-License-Identifier: Apache-2.0
#pragma once

// A delay line (CLAUDE.md §8.10 "DSP: ... delay"; docs/audio.md "Delay
// lines"): the building block of the delay effect, the reverberators'
// combs, all-passes, and feedback network, and the lookahead limiter.
//
// A power-of-two ring of past samples. Time is counted in samples written:
// after t samples, a delay of d reads the sample written at time t - d
// (d = 1 is the newest). Blocks are read before they are written, so a
// block read at a delay of at least its length never sees its own samples:
// that is what lets a feedback loop of any delay of one block or more run
// a block at a time. Shorter loops (the diffusers' all-passes) run sample
// by sample with tap and push.
//
// Fractional delays interpolate with the four-point, third-order Hermite
// (Catmull-Rom) spline, exact at whole delays and smooth under modulation.
//
// Memory (§1.4): nph_audio_delay_init allocates the ring; nothing after
// allocates or locks.
//
// Thread-safety: a delay line belongs to one thread at a time.

#include <nepheline/audio/audio.h>
#include <nepheline/core/alloc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAudioDelay {
    f32 *buffer;
    u32 capacity; // a power of two, > max_delay + 3
    u32 mask;
    u32 max_delay; // the longest delay that reads stay within
    u32 write;     // where the next sample goes
    NphAllocator *alloc;
} NphAudioDelay;

// A line for delays up to `max_delay` samples (>= 1), cleared to silence.
// NPH_RESULT_INVALID_ARGUMENT for 0 or more than 2^28; OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_audio_delay_init(NphAudioDelay *delay, NphAllocator *alloc,
                                            u32 max_delay);

// Releases the ring. Safe on a zero-filled line.
void nph_audio_delay_free(NphAudioDelay *delay);

// Back to silence.
void nph_audio_delay_clear(NphAudioDelay *delay);

// Appends `count` samples.
void nph_audio_delay_write(NphAudioDelay *delay, const f32 *in, u32 count);

// out[i] = the sample `d - i` samples before the next one to be written,
// that is, the input delayed by `d`, for the block about to be written.
// Requires count <= d <= max_delay (asserted); `out` may not alias the
// ring.
void nph_audio_delay_read(const NphAudioDelay *delay, u32 d, f32 *out, u32 count);

// As nph_audio_delay_read for a fractional delay `d` (clamped to
// [count + 2, max_delay], so all four Hermite points exist and none is in
// the block being written).
void nph_audio_delay_read_frac(const NphAudioDelay *delay, f32 d, f32 *out, u32 count);

// The sample written `d` samples ago (1 .. max_delay).
NPH_INLINE f32 nph_audio_delay_tap(const NphAudioDelay *delay, u32 d)
{
    return delay->buffer[(delay->write - d) & delay->mask];
}

// Appends one sample.
NPH_INLINE void nph_audio_delay_push(NphAudioDelay *delay, f32 x)
{
    delay->buffer[delay->write & delay->mask] = x;
    delay->write += 1U;
}

#ifdef __cplusplus
}
#endif
