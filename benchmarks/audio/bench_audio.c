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

#include <nepheline/audio/delay.h>
#include <nepheline/audio/filter.h>
#include <nepheline/audio/kernels.h>
#include <nepheline/audio/pan.h>
#include <nepheline/audio/resampler.h>
#include <nepheline/core/alloc.h>

#include <nbench/nbench.h>

#include <math.h>
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
