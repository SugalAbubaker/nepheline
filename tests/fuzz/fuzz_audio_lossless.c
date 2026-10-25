// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the lossless codec (engine/audio/lossless.h;
// CLAUDE.md §8.8 "every parser is fuzzed; malformed input must never
// crash"). The input is samples for the encoder, whose stream must
// validate and decode back to them exactly, then damage to a copy of the
// stream: validation must catch it or the decoder must survive it, never
// reading past the bytes it is given and never producing a sample that
// does not fit its depth's range (plus a side channel's undoing).
//
// Input (little-endian; fuzz_audio_common.h):
//   u8  channels       1 + byte % 8
//   u8  bits           4 + byte % 21 (4 .. 24)
//   u8  block          the block's frames: 1 + byte * 17
//   u16 frames         at most 6000, and at most the samples given
//   u16 sample ...     each scaled to the depth
//   u16 truncation, then (u16 offset, u8 xor) damage records
#include <nepheline/audio/lossless.h>

#include "fuzz_audio_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Every block of `stream` through the decoder, in pieces; true when every
// block decoded (the samples into `out`, interleaved, when it is not NULL).
static bool decode_blocks(const NphLosslessStream *stream, i32 *out)
{
    NphLosslessDecoder dec;
    i32 buffer[NPH_AUDIO_MAX_CHANNELS][97];
    i32 *planes[NPH_AUDIO_MAX_CHANNELS];
    for (u32 c = 0U; c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        planes[c] = buffer[c];
    }
    const i32 limit = 3 << stream->bits; // left and right after undoing a side
    bool ok = true;
    u64 frame = 0U;
    for (u32 b = 0U; b < stream->block_count; ++b) {
        const u64 start = stream->block_offsets[b];
        const u64 end = b + 1U < stream->block_count ? stream->block_offsets[b + 1U] : stream->size;
        if (start >= end || end > stream->size) {
            return false;
        }
        if (nph_lossless_decoder_begin(&dec, stream->data + start, end - start, stream->channels,
                                       stream->bits) != NPH_RESULT_OK) {
            ok = false;
            continue;
        }
        while (nph_lossless_decoder_remaining(&dec) > 0U) {
            const u32 n = nph_min_u32(97U, nph_lossless_decoder_remaining(&dec));
            if (nph_lossless_decoder_read_int(&dec, planes, n) != NPH_RESULT_OK) {
                ok = false;
                break;
            }
            for (u32 i = 0U; i < n; ++i) {
                for (u32 c = 0U; c < stream->channels; ++c) {
                    if (planes[c][i] < -limit || planes[c][i] > limit) {
                        abort();
                    }
                    if (out != NULL && frame + i < stream->frames) {
                        out[(frame + i) * stream->channels + c] = planes[c][i];
                    }
                }
            }
            frame += n;
        }
    }
    return ok;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FuzzAudioReader r = {data, size};
    u8 channel_byte;
    u8 bits_byte;
    u8 block_byte;
    u16 frames_wanted;
    if (!fuzz_audio_u8(&r, &channel_byte) || !fuzz_audio_u8(&r, &bits_byte) ||
        !fuzz_audio_u8(&r, &block_byte) || !fuzz_audio_u16(&r, &frames_wanted)) {
        return 0;
    }
    const u32 channels = 1U + channel_byte % 8U;
    const u32 bits = 4U + bits_byte % 21U;
    const u32 block = 1U + (u32)block_byte * 17U;
    const u64 available = r.n / ((size_t)channels * 2U);
    const u64 frames = nph_min_u64(nph_min_u64(frames_wanted, 6000U), available);
    const u64 count = frames * channels;
    i32 *samples = (i32 *)NPH_ALLOC(nph_allocator_system(), (count + 1U) * sizeof(i32), 16U);
    i32 *back = (i32 *)NPH_ALLOC(nph_allocator_system(), (count + 1U) * sizeof(i32), 16U);
    if (samples == NULL || back == NULL) {
        abort();
    }
    for (u64 i = 0U; i < count; ++i) {
        u16 v = 0U;
        (void)fuzz_audio_u16(&r, &v);
        // The 16 bits as the top of a `bits`-bit sample, the low bits repeating them.
        const i32 top = (i32)(i16)v;
        samples[i] = bits <= 16U
                         ? top >> (16U - bits)
                         : (i32)((u32)top << (bits - 16U)) | (i32)(v & ((1U << (bits - 16U)) - 1U));
    }
    const NphLosslessParams params = {block, 0U, (channel_byte & 0x80U) != 0U ? 1 : 0};
    NphLosslessEncoded enc;
    if (nph_lossless_encode(nph_allocator_system(), samples, channels, bits, frames, &params,
                            &enc) != NPH_RESULT_OK) {
        abort(); // every in-range input encodes
    }
    if (nph_lossless_validate(&enc.stream) != NPH_RESULT_OK || !decode_blocks(&enc.stream, back)) {
        abort();
    }
    for (u64 i = 0U; i < count; ++i) {
        if (back[i] != samples[i]) {
            abort(); // lossless means exactly
        }
    }
    // Damage: validation must refuse it (the CRCs), and the decoder,
    // given it anyway, must survive.
    u64 damaged_size = 0U;
    u8 *damaged = fuzz_audio_damage(&r, enc.stream.data, enc.stream.size, &damaged_size);
    if (damaged != NULL) {
        NphLosslessStream copy = enc.stream;
        copy.data = damaged;
        copy.size = damaged_size;
        NPH_IGNORE_RESULT(nph_lossless_validate(&copy)); // refused or not, decoding must survive
        (void)decode_blocks(&copy, NULL);
        fuzz_audio_free(damaged, damaged_size);
    }
    nph_lossless_encoded_free(&enc);
    NPH_FREE(nph_allocator_system(), samples, (count + 1U) * sizeof(i32), 16U);
    NPH_FREE(nph_allocator_system(), back, (count + 1U) * sizeof(i32), 16U);
    return 0;
}
