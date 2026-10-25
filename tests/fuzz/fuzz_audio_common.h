// SPDX-License-Identifier: Apache-2.0
// What the audio fuzz targets share (fuzz_audio_lossless.c,
// fuzz_audio_adpcm.c): a little-endian reader over the input, and the
// damage the input asks for applied to a copy of an encoded stream.
#pragma once

#include <nepheline/core/alloc.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct FuzzAudioReader {
    const uint8_t *p;
    size_t n;
} FuzzAudioReader;

static inline bool fuzz_audio_u8(FuzzAudioReader *r, u8 *out)
{
    if (r->n < 1U) {
        return false;
    }
    *out = r->p[0];
    r->p += 1U;
    r->n -= 1U;
    return true;
}

static inline bool fuzz_audio_u16(FuzzAudioReader *r, u16 *out)
{
    if (r->n < 2U) {
        return false;
    }
    *out = (u16)(r->p[0] | (r->p[1] << 8U));
    r->p += 2U;
    r->n -= 2U;
    return true;
}

// A copy of `data` damaged as the rest of the input says: a u16 truncation
// (0 keeps the size), then (u16 offset, u8 xor) records. The copy is exactly
// as long as its size, so a read past it is a sanitizer's error. NULL when
// the input has no damage left to describe.
static inline u8 *fuzz_audio_damage(FuzzAudioReader *r, const u8 *data, u64 size, u64 *out_size)
{
    u16 truncation;
    if (size == 0U || !fuzz_audio_u16(r, &truncation)) {
        return NULL;
    }
    const u64 n = truncation != 0U && truncation < size ? truncation : size;
    u8 *copy = (u8 *)NPH_ALLOC(nph_allocator_system(), n, 16U);
    if (copy == NULL) {
        abort();
    }
    memcpy(copy, data, (size_t)n);
    u16 offset;
    u8 value;
    while (fuzz_audio_u16(r, &offset) && fuzz_audio_u8(r, &value)) {
        copy[offset % n] ^= value;
    }
    *out_size = n;
    return copy;
}

static inline void fuzz_audio_free(u8 *copy, u64 size)
{
    NPH_FREE(nph_allocator_system(), copy, size, 16U);
}
