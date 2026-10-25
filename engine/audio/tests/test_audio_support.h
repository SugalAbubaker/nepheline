// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the audio tests: signals, their measurement, buffers,
// and a deterministic random source.

#include <nepheline/core/alloc.h>
#include <nepheline/math/kernels.h>

#include <ntest/ntest.h>

#include <math.h>
#include <string.h>

#define TEST_PI 3.14159265358979323846

// A buffer of `count` floats (zeroed, 64-byte aligned) from the system
// allocator, and its release.
static inline f32 *test_buffer(u32 count)
{
    f32 *p = (f32 *)NPH_ALLOC_ZEROED(nph_allocator_system(), (u64)count * sizeof(f32), 64U);
    NTEST_ASSERT_NOT_NULL(p);
    return p;
}

static inline void test_buffer_free(f32 *p, u32 count)
{
    NPH_FREE(nph_allocator_system(), p, (u64)count * sizeof(f32), 64U);
}

// x[i] = amp sin(2 pi freq (i + start) / rate), computed in double precision.
static inline void test_sine(f32 *x, u32 count, f64 freq, f64 rate, f64 start, f64 amp)
{
    for (u32 i = 0U; i < count; ++i) {
        x[i] = (f32)(amp * sin(2.0 * TEST_PI * freq * ((f64)i + start) / rate));
    }
}

// The amplitude of the component of x at `freq`, by least-squares
// projection on a sine and a cosine (accurate when the signal is that one
// tone and the span holds many periods).
static inline f64 test_tone_amplitude(const f32 *x, u32 count, f64 freq, f64 rate)
{
    f64 ss = 0.0;
    f64 cc = 0.0;
    f64 sc = 0.0;
    f64 xs = 0.0;
    f64 xc = 0.0;
    for (u32 i = 0U; i < count; ++i) {
        const f64 w = 2.0 * TEST_PI * freq * (f64)i / rate;
        const f64 s = sin(w);
        const f64 c = cos(w);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += (f64)x[i] * s;
        xc += (f64)x[i] * c;
    }
    // Solve [ss sc; sc cc] [a b]^T = [xs xc]^T.
    const f64 det = ss * cc - sc * sc;
    const f64 a = (xs * cc - xc * sc) / det;
    const f64 b = (xc * ss - xs * sc) / det;
    return sqrt(a * a + b * b);
}

static inline f64 test_rms(const f32 *x, u32 count)
{
    f64 sum = 0.0;
    for (u32 i = 0U; i < count; ++i) {
        sum += (f64)x[i] * (f64)x[i];
    }
    return count > 0U ? sqrt(sum / (f64)count) : 0.0;
}

static inline f64 test_db(f64 ratio)
{
    return 20.0 * log10(ratio > 1e-30 ? ratio : 1e-30);
}

// splitmix64 (Steele, Lea & Flood, OOPSLA 2014).
static inline u64 test_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

// Uniform in [-1, 1).
static inline f32 test_rng_signed(u64 *state)
{
    return (f32)((f64)(test_rng_next(state) >> 11U) * (2.0 / 9007199254740992.0) - 1.0);
}

static inline void test_noise(f32 *x, u32 count, u64 seed, f32 amp)
{
    u64 s = seed;
    for (u32 i = 0U; i < count; ++i) {
        x[i] = amp * test_rng_signed(&s);
    }
}

// The SIMD levels this CPU can run, scalar first: the kernel tests compare
// every one against the others.
static inline u32 test_simd_levels(NphSimdLevel out[NPH_SIMD_LEVEL_COUNT])
{
    u32 n = 0U;
    const NphSimdLevel best = nph_simd_detect_level();
    for (u32 l = 0U; l < (u32)NPH_SIMD_LEVEL_COUNT; ++l) {
        if (l <= (u32)best) {
            out[n++] = (NphSimdLevel)l;
        }
    }
    return n;
}

// Bit-exact float comparison of two arrays; reports the first difference.
static inline bool test_same_bits(const f32 *a, const f32 *b, u32 count)
{
    for (u32 i = 0U; i < count; ++i) {
        u32 x;
        u32 y;
        memcpy(&x, &a[i], sizeof x);
        memcpy(&y, &b[i], sizeof y);
        if (x != y) {
            NTEST_LOG("first difference at %u: %.9g vs %.9g", i, (f64)a[i], (f64)b[i]);
            return false;
        }
    }
    return true;
}
