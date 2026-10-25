// SPDX-License-Identifier: Apache-2.0
// Audio benchmarks (CLAUDE.md §1.5, §8.10; docs/audio.md "Performance").
//
// The block kernels, per sample (or per output frame), at each SIMD level
// for the hot ones, so the vector paths are seen to beat the scalar
// reference:
//
//   mix_ramp_<level>                  one 256-sample block mixed in with a gain ramp
//   resample_mono_<level>             one channel through the 48-tap kernel, a 44.1 kHz
//                                     source at a 48 kHz mix (per output frame)
//   resample_stereo_<level>           the same, two channels sharing the coefficients
//   resample_stereo_pitch_1_5         a step of 1.5: the 80-tap bank (per output frame)
//   resample_stereo_pitch_4           a step of 4: the 192-tap bank
//   peak_256, sum_squares_256         the meters' reductions, per sample
//   load_s16_stereo, store_f32_stereo the edges: one channel of interleaved 16-bit
//                                     in, two planar channels out interleaved (per frame)
//   filter_lowpass                    the state-variable biquad, per sample
//   filter_design                     one section's coefficients (per call)
//   delay_read_frac                   a Catmull-Rom read, per sample
//   pan_vbap_7_1, pan_vbap_7_1_spread one voice's gains (per call)
//   resampler_create                  every bank computed (per call)
//
// The codecs, per frame, on 10 s of a synthesised stereo mix (harmonics
// under envelopes, a melody, a noise floor at -50 dB):
//
//   lossless_encode_stereo16          the cooker's encode, default search
//   lossless_decode_stereo16 / _24    the mixer's decode: 256 frames at a time
//   adpcm_encode_stereo               the closed-loop block search
//   adpcm_decode_stereo               random-access decode, 256 frames at a time
//
// The mixer, per 256-frame block of stereo output, voices looping and
// settled, pans and volumes spread:
//
//   mixer_block_64_copy               64 mono float voices at the mix rate and pitch 1:
//                                     the copy path, panned, ramped, mixed
//   mixer_block_64_resampled          64 mono 16-bit voices recorded at 44.1 kHz, pitches
//                                     0.8 to 1.25, a quarter low-passed
//   mixer_block_128_of_512_game       a game's load: 128 real of 512, stereo lossless loops,
//                                     ADPCM effects mono and stereo, 44.1 kHz PCM, filters,
//                                     sends, three buses with a duck and a snapshot
//   mixer_block_16_of_512_virtual     16 real of 512: the ranking and the virtual voices
//
// And filter_lowpass_glide: the state-variable section moving its corner
// sample by sample (per sample).
//
// The effects, per stereo frame, on the mix: effect_eq_6_bands (six bells),
// effect_compressor, effect_limiter (true peak, 1.5 ms lookahead),
// effect_echo, effect_reverb (the default room); and
// mixer_block_64_spatial, per block: 64 moving 3D voices with Doppler, the
// air, occlusion, and obstruction.
//
// Streams, per block: mixer_block_8_lossless_loops, eight stereo lossless
// loops the audio thread decodes from memory, against
// mixer_block_8_streamed_loops, the same eight as streams the streamer
// decodes, the audio thread copying from their rings.

#include <nepheline/audio/adpcm.h>
#include <nepheline/audio/delay.h>
#include <nepheline/audio/filter.h>
#include <nepheline/audio/kernels.h>
#include <nepheline/audio/lossless.h>
#include <nepheline/audio/mixer.h>
#include <nepheline/audio/pan.h>
#include <nepheline/audio/resampler.h>
#include <nepheline/core/alloc.h>
#include <nepheline/platform/thread.h>

#include <nbench/nbench.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BLOCK  NPH_AUDIO_BLOCK_FRAMES
#define FRAMES 4096U
#define SOURCE (4U * FRAMES + 2U * NPH_AUDIO_RESAMPLE_MAX_TAPS)

static f32 g_src[2][SOURCE];
static f32 g_dst[2][FRAMES];
static i16 g_interleaved[2U * FRAMES];
static f32 g_out_interleaved[2U * FRAMES];
static bool g_ready;

static void prepare(void)
{
    if (g_ready) {
        return;
    }
    u64 s = 12345U;
    for (u32 c = 0U; c < 2U; ++c) {
        for (u32 i = 0U; i < SOURCE; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            g_src[c][i] = (f32)((f64)(s >> 11U) / 9007199254740992.0 * 2.0 - 1.0) * 0.5F;
        }
    }
    for (u32 i = 0U; i < 2U * FRAMES; ++i) {
        g_interleaved[i] = (i16)((i * 7919U) & 0x7FFFU);
    }
    g_ready = true;
}

static const NphAudioKernels *kernels_for(NbenchState *state, NphSimdLevel wanted)
{
    const NphSimdLevel have = nph_simd_detect_level();
    if (wanted > have) {
        nbench_set_label(state,
                         wanted == NPH_SIMD_LEVEL_AVX2 ? "avx2 unavailable: ran sse4" : "fallback");
        return nph_audio_kernels(have);
    }
    nbench_set_label(state, nph_simd_level_name(wanted));
    return nph_audio_kernels(wanted);
}

// The resampler is made per benchmark (and released), so the process ends
// with nothing allocated (LeakSanitizer runs the smoke tests).
static void run_resample(NbenchState *state, const NphAudioKernels *k, u32 channels, f64 ratio)
{
    NphAudioResampler *rs = NULL;
    if (nph_audio_resampler_create(nph_allocator_system(), &rs) != NPH_RESULT_OK) {
        return;
    }
    const u64 step = nph_audio_resample_step(ratio);
    const NphAudioResampleBank *bank = nph_audio_resampler_bank(rs, step);
    const f32 *src[2] = {g_src[0] + NPH_AUDIO_RESAMPLE_MAX_TAPS,
                         g_src[1] + NPH_AUDIO_RESAMPLE_MAX_TAPS};
    f32 *dst[2] = {g_dst[0], g_dst[1]};
    nbench_set_items_per_iteration(state, FRAMES);
    while (nbench_keep_running(state)) {
        k->resample(bank, src, channels, 0x1234567ULL, step, dst, FRAMES);
        nbench_do_not_optimize(g_dst);
    }
    nph_audio_resampler_destroy(rs);
}

#define DEFINE_LEVEL_BENCHES(suffix, level)                                                        \
    NBENCH(mix_ramp_##suffix)                                                                      \
    {                                                                                              \
        prepare();                                                                                 \
        const NphAudioKernels *k = kernels_for(state, level);                                      \
        nbench_set_items_per_iteration(state, BLOCK);                                              \
        while (nbench_keep_running(state)) {                                                       \
            k->mix_ramp(g_dst[0], g_src[0], BLOCK, 0.25F, 0.75F);                                  \
            nbench_do_not_optimize(g_dst[0]);                                                      \
        }                                                                                          \
    }                                                                                              \
    NBENCH(resample_mono_##suffix)                                                                 \
    {                                                                                              \
        prepare();                                                                                 \
        run_resample(state, kernels_for(state, level), 1U, 44100.0 / 48000.0);                     \
    }                                                                                              \
    NBENCH(resample_stereo_##suffix)                                                               \
    {                                                                                              \
        prepare();                                                                                 \
        run_resample(state, kernels_for(state, level), 2U, 44100.0 / 48000.0);                     \
    }

DEFINE_LEVEL_BENCHES(scalar, NPH_SIMD_LEVEL_SCALAR)
DEFINE_LEVEL_BENCHES(sse4, NPH_SIMD_LEVEL_SSE4)
DEFINE_LEVEL_BENCHES(avx2, NPH_SIMD_LEVEL_AVX2)

NBENCH(resample_stereo_pitch_1_5)
{
    prepare();
    run_resample(state, nph_audio_kernels_best(), 2U, 1.5);
}

NBENCH(resample_stereo_pitch_4)
{
    prepare();
    run_resample(state, nph_audio_kernels_best(), 2U, 4.0);
}

NBENCH(peak_256)
{
    prepare();
    const NphAudioKernels *k = nph_audio_kernels_best();
    nbench_set_items_per_iteration(state, BLOCK);
    while (nbench_keep_running(state)) {
        const f32 p = k->peak(g_src[0], BLOCK);
        nbench_do_not_optimize(&p);
    }
}

NBENCH(sum_squares_256)
{
    prepare();
    const NphAudioKernels *k = nph_audio_kernels_best();
    nbench_set_items_per_iteration(state, BLOCK);
    while (nbench_keep_running(state)) {
        const f32 e = k->sum_squares(g_src[0], BLOCK);
        nbench_do_not_optimize(&e);
    }
}

NBENCH(load_s16_stereo)
{
    prepare();
    const NphAudioKernels *k = nph_audio_kernels_best();
    nbench_set_items_per_iteration(state, FRAMES);
    while (nbench_keep_running(state)) {
        k->load_s16(g_interleaved, 2U, g_dst[0], FRAMES);
        nbench_do_not_optimize(g_dst[0]);
    }
}

NBENCH(store_f32_stereo)
{
    prepare();
    const NphAudioKernels *k = nph_audio_kernels_best();
    const f32 *planes[2] = {g_src[0], g_src[1]};
    nbench_set_items_per_iteration(state, FRAMES);
    while (nbench_keep_running(state)) {
        k->store_f32(planes, 2U, g_out_interleaved, FRAMES);
        nbench_do_not_optimize(g_out_interleaved);
    }
}

NBENCH(filter_lowpass)
{
    prepare();
    const NphAudioFilterCoeffs c =
        nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, 2000.0F, 0.7071F, 0.0F, 48000.0F);
    NphAudioFilterState s;
    nph_audio_filter_reset(&s);
    nbench_set_items_per_iteration(state, BLOCK);
    while (nbench_keep_running(state)) {
        nph_audio_filter_process(&c, &s, g_src[0], g_dst[0], BLOCK);
        nbench_do_not_optimize(g_dst[0]);
    }
}

NBENCH(filter_lowpass_glide)
{
    prepare();
    const NphAudioFilterCoeffs lo =
        nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, 500.0F, 0.7071F, 0.0F, 48000.0F);
    const NphAudioFilterCoeffs hi =
        nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, 8000.0F, 0.7071F, 0.0F, 48000.0F);
    NphAudioFilterState s;
    nph_audio_filter_reset(&s);
    bool up = true;
    nbench_set_items_per_iteration(state, BLOCK);
    while (nbench_keep_running(state)) {
        nph_audio_filter_process_glide(up ? &lo : &hi, up ? &hi : &lo, &s, g_src[0], g_dst[0],
                                       BLOCK);
        nbench_do_not_optimize(g_dst[0]);
        up = !up;
    }
}

NBENCH(filter_design)
{
    f32 corner = 100.0F;
    while (nbench_keep_running(state)) {
        const NphAudioFilterCoeffs c =
            nph_audio_filter_design(NPH_AUDIO_FILTER_PEAK, corner, 1.0F, 6.0F, 48000.0F);
        nbench_do_not_optimize(&c);
        corner = corner > 15000.0F ? 100.0F : corner * 1.01F;
    }
}

NBENCH(delay_read_frac)
{
    prepare();
    NphAudioDelay d;
    if (nph_audio_delay_init(&d, nph_allocator_system(), 48000U) != NPH_RESULT_OK) {
        return;
    }
    nph_audio_delay_write(&d, g_src[0], FRAMES);
    nbench_set_items_per_iteration(state, BLOCK);
    f32 delay = 1000.25F;
    while (nbench_keep_running(state)) {
        nph_audio_delay_read_frac(&d, delay, g_dst[0], BLOCK);
        nbench_do_not_optimize(g_dst[0]);
        delay = delay > 3000.0F ? 1000.25F : delay + 1.5F;
    }
    nph_audio_delay_free(&d);
}

static void run_pan(NbenchState *state, f32 spread)
{
    NphAudioPanner p;
    if (nph_audio_panner_init(&p, NPH_AUDIO_LAYOUT_SURROUND_7_1) != NPH_RESULT_OK) {
        return;
    }
    f32 gains[NPH_AUDIO_MAX_CHANNELS];
    f32 azimuth = 0.0F;
    while (nbench_keep_running(state)) {
        nph_audio_pan(&p, azimuth, spread, gains);
        nbench_do_not_optimize(gains);
        azimuth += 0.0137F;
    }
}

NBENCH(pan_vbap_7_1)
{
    run_pan(state, 0.0F);
}

NBENCH(pan_vbap_7_1_spread)
{
    run_pan(state, 1.5F);
}

NBENCH(resampler_create)
{
    while (nbench_keep_running(state)) {
        NphAudioResampler *rs = NULL;
        if (nph_audio_resampler_create(nph_allocator_system(), &rs) != NPH_RESULT_OK) {
            return;
        }
        nbench_do_not_optimize(rs);
        nph_audio_resampler_destroy(rs);
    }
}

// ---------------------------------------------------------------------------
// Codecs
// ---------------------------------------------------------------------------

#define MIX_FRAMES 480000U // ten seconds at 48 kHz

static i32 g_mix16[2U * MIX_FRAMES];
static i32 g_mix24[2U * MIX_FRAMES];
static i16 g_mix_s16[2U * MIX_FRAMES];
static bool g_mix_ready;

// A stand-in for music: decaying harmonic notes, a melody that moves between
// the channels, and a noise floor, so neither predictor has it easy.
static void prepare_mix(void)
{
    if (g_mix_ready) {
        return;
    }
    u64 s = 777U;
    for (u32 i = 0U; i < MIX_FRAMES; ++i) {
        const f64 t = (f64)i / 48000.0;
        const f64 note = 110.0 * pow(2.0, (f64)((i / 12000U) % 12U) / 12.0);
        const f64 env = exp(-4.0 * fmod(t, 0.25));
        f64 l = 0.0;
        for (u32 h = 1U; h <= 8U; ++h) {
            l += env * 0.1 / (f64)h * sin(2.0 * 3.14159265358979 * note * h * t);
        }
        const f64 melody = 0.15 * sin(2.0 * 3.14159265358979 * 2.0 * note * t + sin(5.0 * t));
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const f64 noise = ((f64)(s >> 11U) / 9007199254740992.0 - 0.5) * 0.006;
        const f64 pan = 0.5 + 0.4 * sin(0.7 * t);
        const f64 left = l + melody * (1.0 - pan) + noise;
        const f64 right = 0.8 * l + melody * pan - noise * 0.5;
        g_mix16[(u64)i * 2U] = (i32)floor(left * 32767.0 + 0.5);
        g_mix16[(u64)i * 2U + 1U] = (i32)floor(right * 32767.0 + 0.5);
        g_mix24[(u64)i * 2U] = (i32)floor(left * 8388607.0 + 0.5);
        g_mix24[(u64)i * 2U + 1U] = (i32)floor(right * 8388607.0 + 0.5);
        g_mix_s16[(u64)i * 2U] = (i16)g_mix16[(u64)i * 2U];
        g_mix_s16[(u64)i * 2U + 1U] = (i16)g_mix16[(u64)i * 2U + 1U];
    }
    g_mix_ready = true;
}

NBENCH(lossless_encode_stereo16)
{
    prepare_mix();
    // Two seconds a run: the encoder is offline and slow per frame.
    const u64 frames = 96000U;
    nbench_set_items_per_iteration(state, frames);
    u64 bytes = 0U;
    while (nbench_keep_running(state)) {
        NphLosslessEncoded enc;
        if (nph_lossless_encode(nph_allocator_system(), g_mix16, 2U, 16U, frames, NULL, &enc) !=
            NPH_RESULT_OK) {
            return;
        }
        bytes = enc.stream.size;
        nph_lossless_encoded_free(&enc);
    }
    nbench_do_not_optimize_u64(bytes);
}

static void run_lossless_decode(NbenchState *state, const i32 *mix, u32 bits)
{
    prepare_mix();
    NphLosslessEncoded enc;
    if (nph_lossless_encode(nph_allocator_system(), mix, 2U, bits, MIX_FRAMES, NULL, &enc) !=
        NPH_RESULT_OK) {
        return;
    }
    f32 *planes[2] = {g_dst[0], g_dst[1]};
    i32 scratch[2U * BLOCK];
    nbench_set_items_per_iteration(state, MIX_FRAMES);
    while (nbench_keep_running(state)) {
        NphLosslessDecoder dec;
        for (u32 b = 0U; b < enc.stream.block_count; ++b) {
            const u64 start = enc.stream.block_offsets[b];
            const u64 end = b + 1U < enc.stream.block_count ? enc.stream.block_offsets[b + 1U]
                                                            : enc.stream.size;
            if (nph_lossless_decoder_begin(&dec, enc.stream.data + start, end - start, 2U, bits) !=
                NPH_RESULT_OK) {
                return;
            }
            while (nph_lossless_decoder_remaining(&dec) > 0U) {
                const u32 n = nph_min_u32(BLOCK, nph_lossless_decoder_remaining(&dec));
                if (nph_lossless_decoder_read(&dec, planes, n, scratch) != NPH_RESULT_OK) {
                    return;
                }
            }
        }
        nbench_do_not_optimize(g_dst);
    }
    nph_lossless_encoded_free(&enc);
}

NBENCH(lossless_decode_stereo16)
{
    run_lossless_decode(state, g_mix16, 16U);
}

NBENCH(lossless_decode_stereo24)
{
    run_lossless_decode(state, g_mix24, 24U);
}

NBENCH(adpcm_encode_stereo)
{
    prepare_mix();
    const u64 frames = 48000U;
    nbench_set_items_per_iteration(state, frames);
    while (nbench_keep_running(state)) {
        NphAdpcmEncoded enc;
        if (nph_adpcm_encode(nph_allocator_system(), g_mix_s16, 2U, frames, &enc) !=
            NPH_RESULT_OK) {
            return;
        }
        nbench_do_not_optimize(enc.data);
        nph_adpcm_encoded_free(&enc);
    }
}

NBENCH(adpcm_decode_stereo)
{
    prepare_mix();
    NphAdpcmEncoded enc;
    if (nph_adpcm_encode(nph_allocator_system(), g_mix_s16, 2U, MIX_FRAMES, &enc) !=
        NPH_RESULT_OK) {
        return;
    }
    f32 *planes[2] = {g_dst[0], g_dst[1]};
    nbench_set_items_per_iteration(state, MIX_FRAMES);
    while (nbench_keep_running(state)) {
        for (u64 f = 0U; f < MIX_FRAMES; f += BLOCK) {
            nph_adpcm_decode(&enc.stream, f, BLOCK, planes);
        }
        nbench_do_not_optimize(g_dst);
    }
    nph_adpcm_encoded_free(&enc);
}

// ---------------------------------------------------------------------------
// The mixer
// ---------------------------------------------------------------------------

// Clips for the mixer's scenes: the mix's first second as stereo lossless,
// stereo and mono ADPCM, mono 16-bit PCM (played as a 44.1 kHz recording),
// and mono float PCM at the mix rate. The samples are made once; the
// encodings each run, and freed with its mixer.
#define BENCH_CLIP_FRAMES 48000U

static i16 g_pcm_mono[BENCH_CLIP_FRAMES];
static f32 g_float_mono[BENCH_CLIP_FRAMES];

typedef struct BenchClips {
    NphLosslessEncoded lossless;
    NphAdpcmEncoded adpcm_stereo;
    NphAdpcmEncoded adpcm_mono;
} BenchClips;

static void bench_clips_free(BenchClips *c)
{
    if (c->lossless.alloc != NULL) {
        nph_lossless_encoded_free(&c->lossless);
    }
    if (c->adpcm_stereo.alloc != NULL) {
        nph_adpcm_encoded_free(&c->adpcm_stereo);
    }
    if (c->adpcm_mono.alloc != NULL) {
        nph_adpcm_encoded_free(&c->adpcm_mono);
    }
}

static bool bench_clips_make(BenchClips *c)
{
    memset(c, 0, sizeof *c);
    prepare_mix();
    for (u32 i = 0U; i < BENCH_CLIP_FRAMES; ++i) {
        g_pcm_mono[i] = g_mix_s16[(u64)i * 2U];
        g_float_mono[i] = (f32)g_mix_s16[(u64)i * 2U] / 32768.0F;
    }
    const bool ok = nph_lossless_encode(nph_allocator_system(), g_mix16, 2U, 16U, BENCH_CLIP_FRAMES,
                                        NULL, &c->lossless) == NPH_RESULT_OK &&
                    nph_adpcm_encode(nph_allocator_system(), g_mix_s16, 2U, BENCH_CLIP_FRAMES,
                                     &c->adpcm_stereo) == NPH_RESULT_OK &&
                    nph_adpcm_encode(nph_allocator_system(), g_pcm_mono, 1U, BENCH_CLIP_FRAMES,
                                     &c->adpcm_mono) == NPH_RESULT_OK;
    if (!ok) {
        bench_clips_free(c);
    }
    return ok;
}

typedef enum BenchClipKind {
    BENCH_CLIP_FLOAT48 = 0, // mono float at 48 kHz: the copy path
    BENCH_CLIP_PCM44,       // mono 16-bit at 44.1 kHz: the resampler
    BENCH_CLIP_LOSSLESS,    // stereo lossless at 48 kHz
    BENCH_CLIP_ADPCM_STEREO,
    BENCH_CLIP_ADPCM_MONO,
    BENCH_CLIP_COUNT
} BenchClipKind;

static NphResult bench_clip(NphAudioMixer *m, const BenchClips *c, BenchClipKind kind,
                            NphAudioClip *out)
{
    const u32 frames = BENCH_CLIP_FRAMES;
    NphAudioClipDesc d;
    memset(&d, 0, sizeof d);
    d.sample_rate = 48000U;
    d.borrow = true;
    switch (kind) {
    case BENCH_CLIP_FLOAT48:
        d.codec = NPH_AUDIO_CODEC_PCM;
        d.format = NPH_AUDIO_FORMAT_F32;
        d.channels = 1U;
        d.frames = frames;
        d.data = g_float_mono;
        d.size = sizeof g_float_mono;
        break;
    case BENCH_CLIP_PCM44:
        d.codec = NPH_AUDIO_CODEC_PCM;
        d.format = NPH_AUDIO_FORMAT_S16;
        d.channels = 1U;
        d.sample_rate = 44100U;
        d.frames = frames;
        d.data = g_pcm_mono;
        d.size = sizeof g_pcm_mono;
        break;
    case BENCH_CLIP_LOSSLESS:
        d.codec = NPH_AUDIO_CODEC_LOSSLESS;
        d.channels = 2U;
        d.frames = c->lossless.stream.frames;
        d.data = c->lossless.stream.data;
        d.size = c->lossless.stream.size;
        d.bits = 16U;
        d.block_frames = c->lossless.stream.block_frames;
        d.block_offsets = c->lossless.stream.block_offsets;
        d.block_count = c->lossless.stream.block_count;
        break;
    case BENCH_CLIP_ADPCM_STEREO:
    case BENCH_CLIP_ADPCM_MONO: {
        const NphAdpcmStream *s =
            kind == BENCH_CLIP_ADPCM_STEREO ? &c->adpcm_stereo.stream : &c->adpcm_mono.stream;
        d.codec = NPH_AUDIO_CODEC_ADPCM;
        d.channels = s->channels;
        d.frames = s->frames;
        d.data = s->data;
        d.size = s->size;
        break;
    }
    case BENCH_CLIP_COUNT:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return nph_audio_clip_create(m, &d, out);
}

// A scene: `voices` looping voices, `real` of them mixed, on clips of the
// kinds in `mix` (cycled), with pitches spread from 0.8 to 1.25 (or 1 for
// the copy path), every fourth with a low-pass, every eighth sending to a
// bus. Renders until every voice has settled, then measures one block.
typedef struct BenchScene {
    u32 voices;
    u32 real;
    const BenchClipKind *mix;
    u32 mix_count;
    bool pitched;
    bool buses; // music, effects, and a send bus, a snapshot, and a duck
} BenchScene;

// The scene's mixer, clips, buses, and voices; false (all released) if
// anything fails.
static bool bench_scene_start(const BenchScene *scene, const BenchClips *c, NphAudioMixer **out)
{
    NphAudioMixerDesc md;
    memset(&md, 0, sizeof md);
    md.alloc = nph_allocator_system();
    md.max_voices = scene->voices;
    md.max_real_voices = scene->real;
    md.max_clips = 16U;
    NphAudioMixer *m = NULL;
    if (nph_audio_mixer_create(&md, &m) != NPH_RESULT_OK) {
        return false;
    }
    NphAudioClip clips[BENCH_CLIP_COUNT];
    for (u32 k = 0U; k < (u32)BENCH_CLIP_COUNT; ++k) {
        if (bench_clip(m, c, (BenchClipKind)k, &clips[k]) != NPH_RESULT_OK) {
            nph_audio_mixer_destroy(m);
            return false;
        }
    }
    NphAudioBus music = NPH_AUDIO_MASTER_BUS;
    NphAudioBus effects = NPH_AUDIO_MASTER_BUS;
    NphAudioBus send = NPH_AUDIO_MASTER_BUS;
    if (scene->buses) {
        const NphAudioBusDesc bd = nph_audio_bus_desc_default();
        if (nph_audio_bus_create(m, &bd, &music) != NPH_RESULT_OK ||
            nph_audio_bus_create(m, &bd, &effects) != NPH_RESULT_OK ||
            nph_audio_bus_create(m, &bd, &send) != NPH_RESULT_OK) {
            nph_audio_mixer_destroy(m);
            return false;
        }
        nph_audio_bus_set_duck(m, music, effects, -6.0F, -30.0F, 0.05F, 0.3F);
        const NphAudioSnapshotEntry e[1] = {{music, -3.0F, 4000.0F}};
        NphAudioSnapshot snapshot;
        if (nph_audio_snapshot_create(m, e, 1U, &snapshot) != NPH_RESULT_OK) {
            nph_audio_mixer_destroy(m);
            return false;
        }
        nph_audio_snapshot_set_weight(m, snapshot, 0.5F, 0.0F);
    }
    u64 rng = 99U;
    for (u32 i = 0U; i < scene->voices; ++i) {
        const BenchClipKind kind = scene->mix[i % scene->mix_count];
        NphAudioPlayDesc d = nph_audio_play_desc_default(clips[kind]);
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        const f32 u = (f32)(rng >> 40U) / 16777216.0F; // [0, 1)
        d.loop = true;
        d.volume = 0.05F + 0.5F * u;
        d.pan = 2.0F * u - 1.0F;
        d.pitch = scene->pitched ? 0.8F * powf(1.5625F, u) : 1.0F;
        d.start_frame = (u64)(u * 20000.0F);
        d.priority = (u8)(i % 4U * 40U);
        if (scene->pitched && i % 4U == 1U) {
            d.lowpass_hz = 2000.0F + 6000.0F * u;
        }
        if (scene->buses) {
            d.bus = kind == BENCH_CLIP_LOSSLESS ? music : effects;
            if (i % 8U == 3U) {
                d.sends[0] = send;
                d.send_gains[0] = 0.3F;
            }
        }
        NphAudioVoice v;
        if (nph_audio_voice_play(m, &d, &v) != NPH_RESULT_OK) {
            nph_audio_mixer_destroy(m);
            return false;
        }
    }
    *out = m;
    return true;
}

// `label` is the benchmark's own buffer: nbench keeps the pointer, not a
// copy.
static void run_mixer(NbenchState *state, const BenchScene *scene, char *label, u32 label_size)
{
    BenchClips clips;
    NphAudioMixer *m = NULL;
    if (!bench_clips_make(&clips)) {
        nbench_set_label(state, "the clips failed to encode");
        return;
    }
    if (!bench_scene_start(scene, &clips, &m)) {
        nbench_set_label(state, "the scene failed to start");
        bench_clips_free(&clips);
        return;
    }
    // Settle: every start's fade done, the ranking stable.
    for (u32 b = 0U; b < 8U; ++b) {
        nph_audio_mixer_render(m, g_out_interleaved, BLOCK);
    }
    nbench_set_items_per_iteration(state, 1U);
    while (nbench_keep_running(state)) {
        nph_audio_mixer_render(m, g_out_interleaved, BLOCK);
        nbench_do_not_optimize(g_out_interleaved);
    }
    NphAudioMixerStats stats;
    nph_audio_mixer_stats(m, &stats);
    (void)snprintf(label, label_size, "%u real, %u virtual", stats.voices_real,
                   stats.voices_virtual);
    nbench_set_label(state, label);
    nph_audio_mixer_destroy(m);
    bench_clips_free(&clips);
}

NBENCH(mixer_block_64_copy)
{
    static const BenchClipKind mix[1] = {BENCH_CLIP_FLOAT48};
    const BenchScene scene = {64U, 64U, mix, 1U, false, false};
    static char label[48];
    run_mixer(state, &scene, label, (u32)sizeof label);
}

NBENCH(mixer_block_64_resampled)
{
    static const BenchClipKind mix[1] = {BENCH_CLIP_PCM44};
    const BenchScene scene = {64U, 64U, mix, 1U, true, false};
    static char label[48];
    run_mixer(state, &scene, label, (u32)sizeof label);
}

NBENCH(mixer_block_128_of_512_game)
{
    // A game's load: a few stereo lossless loops (music and ambience), the
    // rest ADPCM effects, mono and stereo, a quarter of them low-passed, an
    // eighth sending, on three buses with a duck and a snapshot.
    static const BenchClipKind mix[8] = {
        BENCH_CLIP_LOSSLESS,   BENCH_CLIP_ADPCM_MONO,   BENCH_CLIP_ADPCM_MONO,
        BENCH_CLIP_ADPCM_MONO, BENCH_CLIP_ADPCM_STEREO, BENCH_CLIP_ADPCM_MONO,
        BENCH_CLIP_PCM44,      BENCH_CLIP_ADPCM_MONO,
    };
    const BenchScene scene = {512U, 128U, mix, 8U, true, true};
    static char label[48];
    run_mixer(state, &scene, label, (u32)sizeof label);
}

NBENCH(mixer_block_16_of_512_virtual)
{
    // Mostly virtual: the ranking's sort and the virtual voices' advance.
    static const BenchClipKind mix[2] = {BENCH_CLIP_ADPCM_MONO, BENCH_CLIP_PCM44};
    const BenchScene scene = {512U, 16U, mix, 2U, true, false};
    static char label[48];
    run_mixer(state, &scene, label, (u32)sizeof label);
}

// ---------------------------------------------------------------------------
// Effects and 3D
// ---------------------------------------------------------------------------

// Two channels of noise-like material for the effects (the mix's first
// block, then the next, round and round).
static void bench_stereo_block(u32 n, f32 *l, f32 *r)
{
    prepare_mix();
    const u32 at = (n * BLOCK) % (MIX_FRAMES - BLOCK);
    for (u32 i = 0U; i < BLOCK; ++i) {
        l[i] = (f32)g_mix16[(u64)(at + i) * 2U] / 32768.0F;
        r[i] = (f32)g_mix16[(u64)(at + i) * 2U + 1U] / 32768.0F;
    }
}

typedef enum BenchEffect {
    BENCH_EQ = 0,
    BENCH_COMPRESSOR,
    BENCH_LIMITER,
    BENCH_ECHO,
    BENCH_REVERB,
} BenchEffect;

static void run_effect(NbenchState *state, BenchEffect which)
{
    static f32 l[BLOCK];
    static f32 r[BLOCK];
    f32 *planes[2] = {l, r};
    f32 scratch[BLOCK];
    NphAudioEq eq;
    NphAudioCompressor comp;
    NphAudioLimiter lim;
    NphAudioEcho echo;
    NphAudioReverb reverb;
    const NphAudioLimiterParams lp = nph_audio_limiter_params_default();
    const NphAudioEchoParams ep = nph_audio_echo_params_default();
    const NphAudioReverbParams rp = nph_audio_reverb_params_default();
    const NphAudioCompressorParams cp = nph_audio_compressor_params_default();
    switch (which) {
    case BENCH_EQ:
        nph_audio_eq_init(&eq, 2U, 48000.0F);
        for (u32 b = 0U; b < NPH_AUDIO_EQ_BANDS; ++b) {
            const NphAudioEqBand band = {NPH_AUDIO_FILTER_PEAK,
                                         100.0F * (f32)(b + 1U) * (f32)(b + 1U), 1.0F, 3.0F, 1};
            nph_audio_eq_set_band(&eq, b, &band);
        }
        break;
    case BENCH_COMPRESSOR:
        nph_audio_compressor_init(&comp, 2U, 48000.0F, &cp);
        break;
    case BENCH_LIMITER:
        if (nph_audio_limiter_init(&lim, nph_allocator_system(), 2U, 48000.0F, &lp) !=
            NPH_RESULT_OK) {
            return;
        }
        break;
    case BENCH_ECHO:
        if (nph_audio_echo_init(&echo, nph_allocator_system(), 2U, 48000.0F, 1000.0F, &ep) !=
            NPH_RESULT_OK) {
            return;
        }
        break;
    case BENCH_REVERB:
        if (nph_audio_reverb_init(&reverb, nph_allocator_system(), 2U, 48000.0F, &rp) !=
            NPH_RESULT_OK) {
            return;
        }
        break;
    }
    u32 n = 0U;
    nbench_set_items_per_iteration(state, BLOCK);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        bench_stereo_block(n++, l, r);
        nbench_resume_timing(state);
        switch (which) {
        case BENCH_EQ:
            nph_audio_eq_process(&eq, planes, BLOCK, scratch);
            break;
        case BENCH_COMPRESSOR:
            nph_audio_compressor_process(&comp, planes, BLOCK);
            break;
        case BENCH_LIMITER:
            nph_audio_limiter_process(&lim, planes, BLOCK);
            break;
        case BENCH_ECHO:
            nph_audio_echo_process(&echo, planes, BLOCK);
            break;
        case BENCH_REVERB:
            nph_audio_reverb_process(&reverb, planes, BLOCK);
            break;
        }
        nbench_do_not_optimize(l);
    }
    if (which == BENCH_LIMITER) {
        nph_audio_limiter_free(&lim);
    } else if (which == BENCH_ECHO) {
        nph_audio_echo_free(&echo);
    } else if (which == BENCH_REVERB) {
        nph_audio_reverb_free(&reverb);
    }
}

NBENCH(effect_eq_6_bands)
{
    run_effect(state, BENCH_EQ);
}

NBENCH(effect_compressor)
{
    run_effect(state, BENCH_COMPRESSOR);
}

NBENCH(effect_limiter)
{
    run_effect(state, BENCH_LIMITER);
}

NBENCH(effect_echo)
{
    run_effect(state, BENCH_ECHO);
}

NBENCH(effect_reverb)
{
    run_effect(state, BENCH_REVERB);
}

NBENCH(mixer_block_64_spatial)
{
    // 64 mono 3D voices from 44.1 kHz, scattered over 40 m, moving (so
    // Doppler resamples every one), the air on, a third occluded and a
    // third obstructed: what 3D adds to a block.
    BenchClips clips;
    if (!bench_clips_make(&clips)) {
        nbench_set_label(state, "the clips failed to encode");
        return;
    }
    NphAudioMixerDesc md;
    memset(&md, 0, sizeof md);
    md.alloc = nph_allocator_system();
    md.max_voices = 64U;
    md.max_real_voices = 64U;
    NphAudioMixer *m = NULL;
    NphAudioClip clip;
    if (nph_audio_mixer_create(&md, &m) != NPH_RESULT_OK) {
        bench_clips_free(&clips);
        return;
    }
    if (bench_clip(m, &clips, BENCH_CLIP_PCM44, &clip) != NPH_RESULT_OK) {
        nph_audio_mixer_destroy(m);
        bench_clips_free(&clips);
        return;
    }
    u64 rng = 7U;
    NphAudioVoice voices[64];
    for (u32 i = 0U; i < 64U; ++i) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        const f32 u = (f32)(rng >> 40U) / 16777216.0F;
        NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
        d.spatial = 1;
        d.loop = 1;
        d.volume = 0.2F;
        d.position = (NphVec3){40.0F * u - 20.0F, 0.0F, 20.0F - 40.0F * (1.0F - u)};
        d.velocity = (NphVec3){8.0F, 0.0F, -3.0F};
        d.size = (i % 4U) == 0U ? 1.5F : 0.0F;
        if (nph_audio_voice_play(m, &d, &voices[i]) != NPH_RESULT_OK) {
            nph_audio_mixer_destroy(m);
            bench_clips_free(&clips);
            return;
        }
        nph_audio_voice_set_occlusion(m, voices[i], (i % 3U) == 0U ? 0.6F : 0.0F,
                                      (i % 3U) == 1U ? 0.8F : 0.0F);
    }
    for (u32 b = 0U; b < 16U; ++b) {
        nph_audio_mixer_render(m, g_out_interleaved, BLOCK);
    }
    nbench_set_items_per_iteration(state, 1U);
    while (nbench_keep_running(state)) {
        nph_audio_mixer_render(m, g_out_interleaved, BLOCK);
        nbench_do_not_optimize(g_out_interleaved);
    }
    nph_audio_mixer_destroy(m);
    bench_clips_free(&clips);
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------

// Eight stereo loops at the mix rate and pitch 1, as music and ambience
// play: clips in memory the audio thread decodes (lossless), or streams the
// streamer decodes into rings the audio thread copies from. What a stream
// costs the audio thread does not depend on its codec (its ring holds float
// frames), so these streams are float PCM, which the streamer keeps up
// with; what one costs the streamer is its codec's decode
// (lossless_decode_stereo16, per frame).
#define BENCH_LOOPS 8U

static f32 g_stream_f32[2U * BENCH_CLIP_FRAMES];

static NphResult bench_stream_read(void *user, u64 offset, void *dst, u64 size)
{
    NPH_UNUSED(user);
    if (offset > sizeof g_stream_f32 || size > sizeof g_stream_f32 - offset) {
        return NPH_RESULT_IO_ERROR;
    }
    memcpy(dst, (const u8 *)g_stream_f32 + offset, (size_t)size);
    return NPH_RESULT_OK;
}

// True when every stream holds two blocks ahead of its voice.
static bool bench_streams_ready(const NphAudioMixer *m, const NphAudioClip *loops)
{
    for (u32 i = 0U; i < BENCH_LOOPS; ++i) {
        if (nph_audio_clip_stream_ahead(m, loops[i]) < 2ULL * BLOCK) {
            return false;
        }
    }
    return true;
}

// The eight loops' voices, as clips or streams; false (all released) if
// anything fails.
static bool bench_loops_start(const BenchClips *c, bool streamed, NphAudioMixer **out_mixer,
                              NphAudioStreamer **out_streamer, NphAudioClip *loops)
{
    for (u32 i = 0U; i < 2U * BENCH_CLIP_FRAMES; ++i) {
        g_stream_f32[i] = (f32)g_mix16[i] / 32768.0F;
    }
    NphAudioMixerDesc md;
    memset(&md, 0, sizeof md);
    md.alloc = nph_allocator_system();
    md.max_voices = BENCH_LOOPS;
    md.max_real_voices = BENCH_LOOPS;
    md.max_clips = BENCH_LOOPS;
    NphAudioStreamerDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.alloc = nph_allocator_system();
    sd.max_streams = BENCH_LOOPS;
    sd.period_ms = 1U;
    NphAudioMixer *m = NULL;
    NphAudioStreamer *streamer = NULL;
    bool ok = nph_audio_mixer_create(&md, &m) == NPH_RESULT_OK &&
              (!streamed || nph_audio_streamer_create(&sd, &streamer) == NPH_RESULT_OK);
    if (ok && !streamed) {
        ok = bench_clip(m, c, BENCH_CLIP_LOSSLESS, &loops[0]) == NPH_RESULT_OK;
    }
    for (u32 i = 0U; ok && i < BENCH_LOOPS; ++i) {
        // Spread through the loop, panned across the stage.
        const u64 start = (u64)i * 4000U;
        if (streamed) {
            NphAudioStreamDesc d;
            memset(&d, 0, sizeof d);
            d.source = (NphAudioStreamSource){bench_stream_read, NULL};
            d.codec = NPH_AUDIO_CODEC_PCM;
            d.format = NPH_AUDIO_FORMAT_F32;
            d.channels = 2U;
            d.sample_rate = 48000U;
            d.frames = BENCH_CLIP_FRAMES;
            d.size = sizeof g_stream_f32;
            d.loop = 1;
            d.start_frame = start;
            // Long rings, so the offline loop seldom waits for the streamer
            // (it wakes on the system timer); 32 MB of them, so the voices
            // read from memory, as a game's do a second after the streamer
            // wrote the frames.
            d.buffer_seconds = 8.0F;
            ok = nph_audio_clip_create_stream(m, streamer, &d, &loops[i]) == NPH_RESULT_OK;
        }
        NphAudioPlayDesc p = nph_audio_play_desc_default(streamed ? loops[i] : loops[0]);
        p.volume = 0.1F;
        p.pan = 2.0F * (f32)i / (f32)(BENCH_LOOPS - 1U) - 1.0F;
        if (!streamed) {
            p.loop = 1;
            p.start_frame = start;
        }
        NphAudioVoice v;
        ok = ok && nph_audio_voice_play(m, &p, &v) == NPH_RESULT_OK;
    }
    if (!ok) {
        nph_audio_mixer_destroy(m); // first: it closes its streams
        nph_audio_streamer_destroy(streamer);
        return false;
    }
    *out_mixer = m;
    *out_streamer = streamer;
    return true;
}

static void run_loops(NbenchState *state, bool streamed, char *label, u32 label_size)
{
    BenchClips clips;
    if (!bench_clips_make(&clips)) {
        nbench_set_label(state, "the clips failed to encode");
        return;
    }
    NphAudioMixer *m = NULL;
    NphAudioStreamer *streamer = NULL;
    NphAudioClip loops[BENCH_LOOPS];
    memset(loops, 0, sizeof loops);
    if (!bench_loops_start(&clips, streamed, &m, &streamer, loops)) {
        nbench_set_label(state, "the scene failed to start");
        bench_clips_free(&clips);
        return;
    }
    for (u32 b = 0U; b < 8U; ++b) {
        nph_audio_mixer_render(m, g_out_interleaved, BLOCK);
    }
    nbench_set_items_per_iteration(state, 1U);
    bool stalled = false;
    while (nbench_keep_running(state)) {
        // Offline, the mixer outruns any streamer: the streamer is waited
        // for, untimed (the check itself, a few tens of ns, is timed).
        if (streamed && !stalled && !bench_streams_ready(m, loops)) {
            nbench_pause_timing(state);
            const u64 t0 = nbench_now_ns();
            while (!bench_streams_ready(m, loops)) {
                if (nbench_now_ns() - t0 > 2000000000ULL) {
                    stalled = true;
                    break;
                }
                nph_thread_yield();
            }
            nbench_resume_timing(state);
        }
        nph_audio_mixer_render(m, g_out_interleaved, BLOCK);
        nbench_do_not_optimize(g_out_interleaved);
    }
    NphAudioMixerStats stats;
    nph_audio_mixer_stats(m, &stats);
    if (stalled) {
        nbench_set_label(state, "the streamer stalled");
    } else if (streamed) {
        (void)snprintf(label, label_size, "%u real, %llu frames starved", stats.voices_real,
                       (unsigned long long)stats.stream_starved);
        nbench_set_label(state, label);
    } else {
        (void)snprintf(label, label_size, "%u real", stats.voices_real);
        nbench_set_label(state, label);
    }
    nph_audio_mixer_destroy(m);
    nph_audio_streamer_destroy(streamer);
    bench_clips_free(&clips);
}

NBENCH(mixer_block_8_lossless_loops)
{
    static char label[48];
    run_loops(state, false, label, (u32)sizeof label);
}

NBENCH(mixer_block_8_streamed_loops)
{
    static char label[48];
    run_loops(state, true, label, (u32)sizeof label);
}
