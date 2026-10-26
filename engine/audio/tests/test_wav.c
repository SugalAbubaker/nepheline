// SPDX-License-Identifier: Apache-2.0
// WAV files (wav.h): writing and reading back, the formats the reader
// takes from hand-made files, the files it refuses, loops, and truncation.

#include <nepheline/audio/wav.h>

#include "test_audio_support.h"

// A builder for hand-made files.
typedef struct WavBytes {
    u8 data[4096];
    u32 size;
} WavBytes;

static void wb_bytes(WavBytes *w, const void *p, u32 n)
{
    NTEST_ASSERT_LE_UINT(w->size + n, sizeof w->data);
    memcpy(w->data + w->size, p, n);
    w->size += n;
}

static void wb_u16(WavBytes *w, u32 v)
{
    const u8 b[2] = {(u8)(v & 0xFFU), (u8)(v >> 8U)};
    wb_bytes(w, b, 2U);
}

static void wb_u32(WavBytes *w, u32 v)
{
    const u8 b[4] = {(u8)(v & 0xFFU), (u8)((v >> 8U) & 0xFFU), (u8)((v >> 16U) & 0xFFU),
                     (u8)(v >> 24U)};
    wb_bytes(w, b, 4U);
}

static void wb_tag(WavBytes *w, const char *tag)
{
    wb_bytes(w, tag, 4U);
}

static void wb_riff(WavBytes *w)
{
    w->size = 0U;
    wb_tag(w, "RIFF");
    wb_u32(w, 0U); // patched by wb_finish
    wb_tag(w, "WAVE");
}

static void wb_fmt(WavBytes *w, u32 tag, u32 channels, u32 rate, u32 bits)
{
    wb_tag(w, "fmt ");
    wb_u32(w, 16U);
    wb_u16(w, tag);
    wb_u16(w, channels);
    wb_u32(w, rate);
    wb_u32(w, rate * channels * (bits / 8U));
    wb_u16(w, channels * (bits / 8U));
    wb_u16(w, bits);
}

static void wb_finish(WavBytes *w)
{
    const u32 riff = w->size - 8U;
    w->data[4] = (u8)(riff & 0xFFU);
    w->data[5] = (u8)((riff >> 8U) & 0xFFU);
    w->data[6] = (u8)((riff >> 16U) & 0xFFU);
    w->data[7] = (u8)(riff >> 24U);
}

NTEST(audio_wav, written_files_read_back)
{
    enum {
        FRAMES = 333
    };
    const NphWavEncoding encodings[3] = {NPH_WAV_PCM_S16, NPH_WAV_PCM_S24, NPH_WAV_FLOAT32};
    const u32 channel_counts[3] = {1U, 2U, 6U};
    f32 planes[6][FRAMES];
    const f32 *ptrs[6];
    for (u32 c = 0U; c < 6U; ++c) {
        test_noise(planes[c], FRAMES, 40U + c, 0.9F);
        ptrs[c] = planes[c];
    }
    planes[0][7] = 1.0F;
    planes[0][8] = -1.0F;
    planes[0][9] = 3.0F; // clamped on the way out
    for (u32 e = 0U; e < 3U; ++e) {
        for (u32 ci = 0U; ci < 3U; ++ci) {
            const u32 ch = channel_counts[ci];
            const u64 size = nph_wav_size(encodings[e], ch, FRAMES);
            NTEST_ASSERT_GT_UINT(size, 44U);
            u8 *file = (u8 *)NPH_ALLOC(nph_allocator_system(), size, 16U);
            NTEST_ASSERT_NOT_NULL(file);
            NTEST_ASSERT_EQ_INT(nph_wav_write(encodings[e], ch, 44100U, ptrs, FRAMES, file),
                                NPH_RESULT_OK);
            NphWavInfo info;
            NTEST_ASSERT_EQ_INT(nph_wav_parse(file, size, &info), NPH_RESULT_OK);
            NTEST_ASSERT_EQ_INT(info.encoding, encodings[e]);
            NTEST_ASSERT_EQ_UINT(info.channels, ch);
            NTEST_ASSERT_EQ_UINT(info.sample_rate, 44100U);
            NTEST_ASSERT_EQ_UINT(info.frames, FRAMES);
            NTEST_ASSERT_FALSE(info.has_loop);
            const f64 step = encodings[e] == NPH_WAV_PCM_S16   ? 1.0 / 32767.0
                             : encodings[e] == NPH_WAV_PCM_S24 ? 1.0 / 8388607.0
                                                               : 0.0;
            for (u32 i = 0U; i < (u32)FRAMES; ++i) {
                for (u32 c = 0U; c < ch; ++c) {
                    f64 want = (f64)planes[c][i];
                    want = want > 1.0 ? 1.0 : (want < -1.0 ? -1.0 : want);
                    const f64 got = nph_wav_sample_float(&info, i, c);
                    // Integers are written at x * (2^(b-1) - 1), so +1 does
                    // not clip, and read as n / 2^(b-1): within a rounding
                    // step plus that scale's |x| / 2^(b-1).
                    NTEST_ASSERT_NEAR_F64(got, want, step * 2.01 + 1e-12);
                }
            }
            NPH_FREE(nph_allocator_system(), file, size, 16U);
        }
    }
    // What the writer refuses.
    u8 unused[64];
    NTEST_ASSERT_EQ_INT(nph_wav_write(NPH_WAV_PCM_U8, 1U, 44100U, ptrs, 1U, unused),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_wav_write(NPH_WAV_PCM_S16, 0U, 44100U, ptrs, 1U, unused),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_wav_write(NPH_WAV_PCM_S16, 1U, 1000U, ptrs, 1U, unused),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(nph_wav_size(NPH_WAV_FLOAT64, 1U, 1U), 0U);
}

NTEST(audio_wav, the_reader_takes_every_pcm_and_float_depth)
{
    // U8, S32, and F64, which the writer does not produce.
    WavBytes w;
    wb_riff(&w);
    wb_fmt(&w, 1U, 1U, 22050U, 8U);
    wb_tag(&w, "data");
    wb_u32(&w, 3U);
    const u8 u8s[4] = {0U, 128U, 255U, 0U}; // and a pad byte
    wb_bytes(&w, u8s, 4U);
    wb_finish(&w);
    NphWavInfo info;
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.encoding, NPH_WAV_PCM_U8);
    NTEST_ASSERT_EQ_UINT(info.frames, 3U);
    NTEST_ASSERT_EQ_INT(nph_wav_sample_int(&info, 0U, 0U), -128);
    NTEST_ASSERT_EQ_INT(nph_wav_sample_int(&info, 1U, 0U), 0);
    NTEST_ASSERT_EQ_INT(nph_wav_sample_int(&info, 2U, 0U), 127);

    wb_riff(&w);
    wb_fmt(&w, 1U, 2U, 96000U, 32U);
    wb_tag(&w, "data");
    wb_u32(&w, 8U);
    wb_u32(&w, 0x80000000U);
    wb_u32(&w, 0x7FFFFFFFU);
    wb_finish(&w);
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.encoding, NPH_WAV_PCM_S32);
    NTEST_ASSERT_EQ_INT(nph_wav_sample_int(&info, 0U, 0U), NPH_I32_MIN);
    NTEST_ASSERT_EQ_INT(nph_wav_sample_int(&info, 0U, 1U), NPH_I32_MAX);

    wb_riff(&w);
    wb_fmt(&w, 3U, 1U, 48000U, 64U);
    wb_tag(&w, "data");
    wb_u32(&w, 8U);
    const f64 value = -0.375;
    wb_bytes(&w, &value, 8U);
    wb_finish(&w);
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.encoding, NPH_WAV_FLOAT64);
    NTEST_ASSERT_NEAR_F64(nph_wav_sample_float(&info, 0U, 0U), -0.375, 0.0);
    NTEST_ASSERT_EQ_INT(nph_wav_sample_int(&info, 0U, 0U), 0); // floats are not integers
}

NTEST(audio_wav, extensible_files_unknown_chunks_and_loops)
{
    WavBytes w;
    wb_riff(&w);
    // A LIST chunk of odd size first: padded, skipped.
    wb_tag(&w, "LIST");
    wb_u32(&w, 5U);
    wb_bytes(&w, "INFOx\0", 6U);
    // WAVE_FORMAT_EXTENSIBLE, 24-bit PCM, stereo.
    wb_tag(&w, "fmt ");
    wb_u32(&w, 40U);
    wb_u16(&w, 0xFFFEU);
    wb_u16(&w, 2U);
    wb_u32(&w, 48000U);
    wb_u32(&w, 48000U * 6U);
    wb_u16(&w, 6U);
    wb_u16(&w, 24U);
    wb_u16(&w, 22U);
    wb_u16(&w, 24U);
    wb_u32(&w, 3U);
    const u8 pcm_guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                             0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    wb_bytes(&w, pcm_guid, 16U);
    wb_tag(&w, "data");
    wb_u32(&w, 6U * 4U);
    for (u32 i = 0U; i < 8U; ++i) {
        const u8 s[3] = {(u8)i, 0U, (u8)(i == 1U ? 0x80U : 0x00U)};
        wb_bytes(&w, s, 3U);
    }
    // A sampler chunk with one loop over frames 1 .. 2 (end inclusive).
    wb_tag(&w, "smpl");
    wb_u32(&w, 36U + 24U);
    for (u32 i = 0U; i < 7U; ++i) {
        wb_u32(&w, 0U);
    }
    wb_u32(&w, 1U); // loops
    wb_u32(&w, 0U);
    wb_u32(&w, 0U); // cue id
    wb_u32(&w, 0U); // type
    wb_u32(&w, 1U); // start
    wb_u32(&w, 2U); // end
    wb_u32(&w, 0U);
    wb_u32(&w, 0U);
    wb_finish(&w);
    NphWavInfo info;
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.encoding, NPH_WAV_PCM_S24);
    NTEST_ASSERT_EQ_UINT(info.channels, 2U);
    NTEST_ASSERT_EQ_UINT(info.frames, 4U);
    NTEST_ASSERT_EQ_INT(nph_wav_sample_int(&info, 0U, 1U), -8388607); // 0x800001
    NTEST_ASSERT_TRUE(info.has_loop);
    NTEST_ASSERT_EQ_UINT(info.loop_start, 1U);
    NTEST_ASSERT_EQ_UINT(info.loop_end, 3U);
    // A loop past the data is dropped, not trusted.
    w.data[w.size - 12U] = 9U; // end = 9
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(info.has_loop);
}

NTEST(audio_wav, what_the_reader_refuses)
{
    WavBytes w;
    NphWavInfo info;
    // Not RIFF; too short.
    NTEST_ASSERT_EQ_INT(nph_wav_parse("RIFX\0\0\0\0WAVE", 12U, &info), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(nph_wav_parse("RIFF", 4U, &info), NPH_RESULT_PARSE_ERROR);
    // Data before the format.
    wb_riff(&w);
    wb_tag(&w, "data");
    wb_u32(&w, 2U);
    wb_u16(&w, 0U);
    wb_fmt(&w, 1U, 1U, 44100U, 16U);
    wb_finish(&w);
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_PARSE_ERROR);
    // No data at all.
    wb_riff(&w);
    wb_fmt(&w, 1U, 1U, 44100U, 16U);
    wb_finish(&w);
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_PARSE_ERROR);
    // Compressed (Microsoft ADPCM), nine channels, 4 kHz, 12-bit: unsupported.
    const u32 bad[4][3] = {{2U, 1U, 16U}, {1U, 9U, 16U}, {1U, 1U, 16U}, {1U, 1U, 12U}};
    const u32 rates[4] = {44100U, 44100U, 4000U, 44100U};
    for (u32 i = 0U; i < 4U; ++i) {
        wb_riff(&w);
        wb_fmt(&w, bad[i][0], bad[i][1], rates[i], bad[i][2]);
        wb_tag(&w, "data");
        wb_u32(&w, 0U);
        wb_finish(&w);
        NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_UNSUPPORTED);
    }
    // A block alignment that disagrees with the format.
    wb_riff(&w);
    wb_fmt(&w, 1U, 2U, 44100U, 16U);
    w.data[w.size - 4U] = 3U; // block_align 3
    wb_tag(&w, "data");
    wb_u32(&w, 0U);
    wb_finish(&w);
    NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_PARSE_ERROR);
    // A second format after the data (the fuzzer's: 8-bit data read as
    // 32-bit frames ran past its end), before it, and a second data chunk.
    const u8 samples[6] = {0x80U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U};
    for (u32 i = 0U; i < 3U; ++i) {
        wb_riff(&w);
        wb_fmt(&w, 1U, 1U, 22050U, 8U);
        if (i == 1U) {
            wb_fmt(&w, 3U, 6U, 48000U, 32U);
        }
        wb_tag(&w, "data");
        wb_u32(&w, 6U);
        wb_bytes(&w, samples, 6U);
        if (i == 0U) {
            wb_fmt(&w, 3U, 6U, 48000U, 32U);
        } else if (i == 2U) {
            wb_tag(&w, "data");
            wb_u32(&w, 2U);
            wb_bytes(&w, samples, 2U);
        }
        wb_finish(&w);
        NTEST_ASSERT_EQ_INT(nph_wav_parse(w.data, w.size, &info), NPH_RESULT_PARSE_ERROR);
    }
}

NTEST(audio_wav, truncation_never_faults)
{
    // A data chunk claiming more than the file has is cut to whole frames;
    // every shorter prefix of a file either parses or is refused.
    f32 tone[200];
    test_sine(tone, 200U, 440.0, 44100.0, 0.0, 0.5);
    const f32 *ptrs[2] = {tone, tone};
    const u64 size = nph_wav_size(NPH_WAV_PCM_S16, 2U, 200U);
    u8 file[1024];
    NTEST_ASSERT_LE_UINT(size, sizeof file);
    NTEST_ASSERT_EQ_INT(nph_wav_write(NPH_WAV_PCM_S16, 2U, 44100U, ptrs, 200U, file),
                        NPH_RESULT_OK);
    NphWavInfo info;
    NTEST_ASSERT_EQ_INT(nph_wav_parse(file, size - 10U, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.frames, 197U); // 790 bytes of 800: 197 whole frames
    for (u64 n = 0U; n < size; ++n) {
        const NphResult r = nph_wav_parse(file, n, &info);
        NTEST_ASSERT_TRUE(r == NPH_RESULT_OK || r == NPH_RESULT_PARSE_ERROR);
        if (r == NPH_RESULT_OK) {
            NTEST_ASSERT_LE_UINT(info.data_size + 44U, n);
        }
    }
}
