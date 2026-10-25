// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the cooker's sounds (tools/cook/sound.h; CLAUDE.md
// §8.8 "every parser is fuzzed", §9 "the cooker inputs"): a settings byte,
// then a WAV file. Whatever the WAV reader takes is cooked under those
// settings, and whatever the cooker writes must pass the runtime's
// validation and sound view and the audio module's checks of its codec
// data; a lossless sound must decode to the source's samples exactly, and
// a PCM one hold them.
//
// Input: u8 settings (bits 0-1 the codec, 2-3 the streaming, 4 exhaustive,
// 5-7 AUTO's threshold in tenths of a second, 0 for the default), then the
// WAV image. Seeds from scripts/fuzz_seeds_audio.py.
#include <nepheline/asset/format.h>
#include <nepheline/asset/sound.h>
#include <nepheline/audio/adpcm.h>
#include <nepheline/audio/lossless.h>

#include <cook/sound.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Samples a source may have, so the lossless encoder stays quick.
#define FUZZ_MAX_SAMPLES (1U << 17U)

// The integer the lossless codec holds for sample i (cook/sound.h).
static i32 lossless_sample(const NphWavInfo *wav, u64 i)
{
    const u64 frame = i / wav->channels;
    const u32 ch = (u32)(i % wav->channels);
    if (wav->encoding == NPH_WAV_FLOAT32 || wav->encoding == NPH_WAV_FLOAT64) {
        return (i32)(nph_wav_sample_float(wav, frame, ch) * 8388608.0);
    }
    if (wav->encoding == NPH_WAV_PCM_S32) {
        return nph_wav_sample_int(wav, frame, ch) / 256;
    }
    return nph_wav_sample_int(wav, frame, ch);
}

// PCM must hold the samples: the file's bytes, or 8-bit ones at 16 bits
// and 64-bit floats at 32.
static bool pcm_holds(const NphWavInfo *wav, const NphSoundView *v)
{
    const u64 n = wav->frames * wav->channels;
    for (u64 i = 0U; i < n; ++i) {
        const u64 frame = i / wav->channels;
        const u32 ch = (u32)(i % wav->channels);
        if (wav->encoding == NPH_WAV_PCM_U8) {
            i16 x;
            memcpy(&x, v->data + i * 2U, sizeof x);
            if (x != (i16)(nph_wav_sample_int(wav, frame, ch) * 256)) {
                return false;
            }
        } else if (wav->encoding == NPH_WAV_FLOAT64) {
            f32 x;
            memcpy(&x, v->data + i * 4U, sizeof x);
            if (x != (f32)nph_wav_sample_float(wav, frame, ch)) {
                return false;
            }
        }
    }
    if (wav->encoding != NPH_WAV_PCM_U8 && wav->encoding != NPH_WAV_FLOAT64) {
        return v->size == wav->data_size && memcmp(v->data, wav->data, (size_t)v->size) == 0;
    }
    return true;
}

static void check(NphAllocator *alloc, const NphWavInfo *wav, void *image, u64 size)
{
    NphAssetFileInfo info;
    if (nph_asset_file_validate(image, size, &info) != NPH_RESULT_OK) {
        abort(); // the cooker wrote a file its own reader refuses
    }
    NphAssetBlob blobs[3];
    if (info.blob_count > 3U) {
        abort();
    }
    for (u32 i = 0U; i < info.blob_count; ++i) {
        const NphAssetBlobEntry *e = &info.blobs[i];
        if (e->codec != NPH_ASSET_CODEC_NONE) {
            abort(); // a sound's blobs are stored
        }
        blobs[i].tag = e->tag;
        blobs[i].index = e->index;
        blobs[i].codec = e->codec;
        blobs[i].alignment = e->alignment;
        blobs[i].size = e->raw_size;
        blobs[i].offset = e->offset;
        blobs[i].data = nph_asset_file_blob(image, e);
    }
    NphAsset asset;
    memset(&asset, 0, sizeof asset);
    asset.header = info.header;
    asset.blobs = blobs;
    asset.blob_count = info.blob_count;
    asset.path = NPH_STR("fuzz");
    NphSoundView v;
    if (nph_sound_view(&asset, &v) != NPH_RESULT_OK || v.header->frames != wav->frames ||
        v.header->channels != wav->channels || v.header->sample_rate != wav->sample_rate) {
        abort();
    }
    const u64 n = wav->frames * wav->channels;
    switch ((NphSoundCodec)v.header->codec) {
    case NPH_SOUND_CODEC_PCM:
        if (!pcm_holds(wav, &v)) {
            abort();
        }
        break;
    case NPH_SOUND_CODEC_ADPCM: {
        NphAdpcmStream s;
        memset(&s, 0, sizeof s);
        s.channels = wav->channels;
        s.frames = wav->frames;
        s.data = v.data;
        s.size = v.size;
        if (nph_adpcm_validate(&s) != NPH_RESULT_OK) {
            abort();
        }
        break;
    }
    case NPH_SOUND_CODEC_LOSSLESS: {
        NphLosslessStream s;
        memset(&s, 0, sizeof s);
        s.channels = wav->channels;
        s.bits = v.header->bits;
        s.block_frames = v.header->block_frames;
        s.frames = wav->frames;
        s.data = v.data;
        s.size = v.size;
        s.block_offsets = v.block_offsets;
        s.block_count = v.header->block_count;
        i32 *x = (i32 *)NPH_ALLOC(alloc, n * sizeof(i32), 16U);
        if (x == NULL) {
            return;
        }
        if (nph_lossless_validate(&s) != NPH_RESULT_OK ||
            nph_lossless_decode_all(&s, x) != NPH_RESULT_OK) {
            abort();
        }
        for (u64 i = 0U; i < n; ++i) {
            if (x[i] != lossless_sample(wav, i)) {
                abort(); // lossless, and not to the bit
            }
        }
        NPH_FREE(alloc, x, n * sizeof(i32), 16U);
        break;
    }
    default:
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2U || size > (1U << 20U)) {
        return 0;
    }
    NphAllocator *alloc = nph_allocator_system();
    // An aligned copy, as a file read into memory is.
    const u64 n = size - 1U;
    u8 *copy = (u8 *)NPH_ALLOC(alloc, n, 16U);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, data + 1, n);
    NphWavInfo wav;
    if (nph_wav_parse(copy, n, &wav) == NPH_RESULT_OK &&
        wav.frames * wav.channels <= FUZZ_MAX_SAMPLES) {
        CookSoundSettings s;
        cook_sound_settings_default(&s);
        s.codec = (CookSoundCodec)(data[0] & 3U);
        s.stream = (CookSoundStream)((data[0] >> 2U) & 3U);
        if ((u32)s.stream > (u32)COOK_SOUND_STREAM_NEVER) {
            s.stream = COOK_SOUND_STREAM_AUTO;
        }
        s.exhaustive = (b32)((data[0] >> 4U) & 1U);
        const u32 tenths = (u32)data[0] >> 5U;
        s.stream_seconds = tenths != 0U ? (f32)tenths * 0.1F : 0.0F;
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_SOUND, NPH_SOUND_TYPE_VERSION,
                              nph_guid_from_cstr("fuzz"), 0U);
        if (cook_sound(alloc, &wav, &s, &w) == NPH_RESULT_OK) {
            void *image = NULL;
            u64 image_size = 0U;
            if (nph_asset_writer_finish_alloc(&w, alloc, &image, &image_size) == NPH_RESULT_OK) {
                check(alloc, &wav, image, image_size);
                NPH_FREE(alloc, image, image_size, NPH_ASSET_BLOB_MAX_ALIGN);
            }
        }
        nph_asset_writer_free(&w);
    }
    NPH_FREE(alloc, copy, n, 16U);
    return 0;
}
