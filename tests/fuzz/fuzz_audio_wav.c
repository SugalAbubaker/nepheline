// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the WAV reader (engine/audio/wav.h; CLAUDE.md §8.8
// "every parser is fuzzed; malformed input must never crash"): the input is
// a file. What the reader accepts must lie within the input, every sample
// of it must read, and it must survive a round trip through the writer.
#include <nepheline/audio/wav.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    NphWavInfo info;
    if (nph_wav_parse(data, size, &info) != NPH_RESULT_OK) {
        return 0;
    }
    const u8 *base = (const u8 *)data;
    const u8 *samples = (const u8 *)info.data;
    if (info.channels == 0U || info.channels > NPH_AUDIO_MAX_CHANNELS || samples < base ||
        (u64)(samples - base) + info.data_size > size ||
        info.data_size != info.frames * info.bytes_per_frame) {
        abort(); // what was accepted must describe the input
    }
    if (info.has_loop && !(info.loop_start < info.loop_end && info.loop_end <= info.frames)) {
        abort();
    }
    // Read every sample, both ways, and write the first frames back out.
    const u64 frames = info.frames < 4096U ? info.frames : 4096U;
    f32 *planes[NPH_AUDIO_MAX_CHANNELS];
    for (u32 c = 0U; c < info.channels; ++c) {
        planes[c] = (f32 *)NPH_ALLOC(nph_allocator_system(), (frames + 1U) * sizeof(f32), 16U);
        if (planes[c] == NULL) {
            abort();
        }
    }
    for (u64 i = 0U; i < info.frames; ++i) {
        for (u32 c = 0U; c < info.channels; ++c) {
            const f64 v = nph_wav_sample_float(&info, i, c);
            (void)nph_wav_sample_int(&info, i, c);
            if (i < frames) {
                planes[c][i] = (f32)v;
            }
        }
    }
    const u64 out_size = nph_wav_size(NPH_WAV_FLOAT32, info.channels, frames);
    u8 *out = (u8 *)NPH_ALLOC(nph_allocator_system(), out_size, 16U);
    if (out == NULL) {
        abort();
    }
    const f32 *const *src = (const f32 *const *)planes;
    if (nph_wav_write(NPH_WAV_FLOAT32, info.channels, info.sample_rate, src, frames, out) !=
        NPH_RESULT_OK) {
        abort(); // anything the reader takes, the writer writes
    }
    NphWavInfo back;
    if (nph_wav_parse(out, out_size, &back) != NPH_RESULT_OK || back.frames != frames ||
        back.channels != info.channels || back.sample_rate != info.sample_rate) {
        abort();
    }
    NPH_FREE(nph_allocator_system(), out, out_size, 16U);
    for (u32 c = 0U; c < info.channels; ++c) {
        NPH_FREE(nph_allocator_system(), planes[c], (frames + 1U) * sizeof(f32), 16U);
    }
    return 0;
}
