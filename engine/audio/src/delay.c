// SPDX-License-Identifier: Apache-2.0
// The delay line (delay.h).

#include <nepheline/audio/delay.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

#define DELAY_MAX (1U << 28U)

NphResult nph_audio_delay_init(NphAudioDelay *delay, NphAllocator *alloc, u32 max_delay)
{
    NPH_ASSERT(delay != NULL && alloc != NULL);
    memset(delay, 0, sizeof *delay);
    if (max_delay == 0U || max_delay > DELAY_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // Four samples beyond the longest delay: the Hermite points around it.
    const u32 capacity = nph_next_pow2_u32(max_delay + 4U);
    f32 *buffer = (f32 *)NPH_ALLOC_ZEROED(alloc, (u64)capacity * sizeof(f32), 64U);
    if (buffer == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    delay->buffer = buffer;
    delay->capacity = capacity;
    delay->mask = capacity - 1U;
    delay->max_delay = max_delay;
    delay->alloc = alloc;
    return NPH_RESULT_OK;
}

void nph_audio_delay_free(NphAudioDelay *delay)
{
    if (delay->buffer != NULL) {
        NPH_FREE(delay->alloc, delay->buffer, (u64)delay->capacity * sizeof(f32), 64U);
    }
    memset(delay, 0, sizeof *delay);
}

void nph_audio_delay_clear(NphAudioDelay *delay)
{
    if (delay->buffer != NULL) {
        memset(delay->buffer, 0, (size_t)delay->capacity * sizeof(f32));
    }
}

void nph_audio_delay_write(NphAudioDelay *delay, const f32 *in, u32 count)
{
    NPH_ASSERT(count <= delay->capacity);
    const u32 start = delay->write & delay->mask;
    const u32 first = nph_min_u32(count, delay->capacity - start);
    memcpy(delay->buffer + start, in, (size_t)first * sizeof(f32));
    memcpy(delay->buffer, in + first, (size_t)(count - first) * sizeof(f32));
    delay->write += count;
}

void nph_audio_delay_read(const NphAudioDelay *delay, u32 d, f32 *out, u32 count)
{
    NPH_ASSERT_MSG(count <= d && d <= delay->max_delay, "delay %u outside [%u, %u]", d, count,
                   delay->max_delay);
    const u32 start = (delay->write - d) & delay->mask;
    const u32 first = nph_min_u32(count, delay->capacity - start);
    memcpy(out, delay->buffer + start, (size_t)first * sizeof(f32));
    memcpy(out + first, delay->buffer, (size_t)(count - first) * sizeof(f32));
}

void nph_audio_delay_read_frac(const NphAudioDelay *delay, f32 d, f32 *out, u32 count)
{
    const f32 lo = (f32)count + 2.0F;
    const f32 hi = (f32)delay->max_delay;
    if (!(d >= lo)) {
        d = lo;
    }
    if (d > hi) {
        d = hi;
    }
    // Output i reads the input at time write + i - d: split d into a whole
    // part and the fraction past the sample before it.
    const f32 whole = ceilf(d);
    const f32 mu = whole - d; // 0 .. 1: how far past x[n] the read lands
    const u32 back = (u32)whole;
    const u32 mask = delay->mask;
    const f32 *buf = delay->buffer;
    for (u32 i = 0U; i < count; ++i) {
        const u32 n = delay->write + i - back; // x[n] is at or before the read time
        const f32 xm1 = buf[(n - 1U) & mask];
        const f32 x0 = buf[n & mask];
        const f32 x1 = buf[(n + 1U) & mask];
        const f32 x2 = buf[(n + 2U) & mask];
        // Catmull-Rom: tangents (x1 - xm1) / 2 and (x2 - x0) / 2.
        const f32 c1 = 0.5F * (x1 - xm1);
        const f32 c2 = xm1 - 2.5F * x0 + 2.0F * x1 - 0.5F * x2;
        const f32 c3 = 0.5F * (x2 - xm1) + 1.5F * (x0 - x1);
        out[i] = x0 + mu * (c1 + mu * (c2 + mu * c3));
    }
}
