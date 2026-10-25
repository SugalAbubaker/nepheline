// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for our ADPCM (engine/audio/adpcm.h; CLAUDE.md §8.8
// "every parser is fuzzed; malformed input must never crash"). The input is
// samples for the encoder, whose stream must validate and decode the same
// from any position, then damage to a copy of the stream, which the decoder
// must survive at any position and any size.
//
// Input (little-endian; fuzz_audio_common.h):
//   u8  channels       1 + byte % 8
//   u16 frames         at most 4000, and at most the samples given
//   u16 sample ...
//   u16 truncation, then (u16 offset, u8 xor) damage records
#include <nepheline/audio/adpcm.h>

#include "fuzz_audio_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FuzzAudioReader r = {data, size};
    u8 channel_byte;
    u16 frames_wanted;
    if (!fuzz_audio_u8(&r, &channel_byte) || !fuzz_audio_u16(&r, &frames_wanted)) {
        return 0;
    }
    const u32 channels = 1U + channel_byte % 8U;
    const u64 frames = nph_min_u64(nph_min_u64(frames_wanted, 4000U), r.n / (2U * (u64)channels));
    const u64 count = frames * channels;
    i16 *samples = (i16 *)NPH_ALLOC(nph_allocator_system(), (count + 1U) * sizeof(i16), 16U);
    i16 *whole = (i16 *)NPH_ALLOC(nph_allocator_system(), (count + 1U) * sizeof(i16), 16U);
    if (samples == NULL || whole == NULL) {
        abort();
    }
    for (u64 i = 0U; i < count; ++i) {
        u16 v = 0U;
        (void)fuzz_audio_u16(&r, &v);
        samples[i] = (i16)v;
    }
    NphAdpcmEncoded enc;
    if (nph_adpcm_encode(nph_allocator_system(), samples, channels, frames, &enc) !=
            NPH_RESULT_OK ||
        nph_adpcm_validate(&enc.stream) != NPH_RESULT_OK) {
        abort();
    }
    nph_adpcm_decode_int(&enc.stream, 0U, (u32)frames, whole);
    // Any range decodes as the whole does there.
    i16 part[NPH_AUDIO_MAX_CHANNELS * 300U];
    const u64 first = frames > 0U ? (u64)data[0] * 13U % (frames + 1U) : 0U;
    const u32 n = (u32)nph_min_u64(300U, frames + 5U - first); // past the end too
    nph_adpcm_decode_int(&enc.stream, first, n, part);
    for (u32 i = 0U; i < n; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            i16 want = 0;
            if (first + i < frames) {
                want = whole[(first + i) * channels + c];
            }
            if (part[(u64)i * channels + c] != want) {
                abort();
            }
        }
    }
    u64 damaged_size = 0U;
    u8 *damaged = fuzz_audio_damage(&r, enc.stream.data, enc.stream.size, &damaged_size);
    if (damaged != NULL) {
        NphAdpcmStream copy = enc.stream;
        copy.data = damaged;
        copy.size = damaged_size;
        (void)nph_adpcm_validate(&copy);
        nph_adpcm_decode_int(&copy, first, n, part);
        fuzz_audio_free(damaged, damaged_size);
    }
    nph_adpcm_encoded_free(&enc);
    NPH_FREE(nph_allocator_system(), samples, (count + 1U) * sizeof(i16), 16U);
    NPH_FREE(nph_allocator_system(), whole, (count + 1U) * sizeof(i16), 16U);
    return 0;
}
