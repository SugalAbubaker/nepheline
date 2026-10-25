// SPDX-License-Identifier: Apache-2.0
// Golden mixes (CLAUDE.md §8.10 "Tests: offline rendering of mixes,
// compared against reference buffers within tolerance"; Phase 12's exit
// "offline mix tests pass"; docs/audio.md "Golden mixes"). Each scene is a
// script of commands at given blocks, rendered offline, written to
// <build>/audio_golden/<Config>/<name>.wav, and compared with its approved
// baseline in tests/audio/baselines (Git LFS): the largest difference and
// the RMS of the difference must stay below a hundred-thousandth of full
// scale, which absorbs the last-bit differences of the C libraries' sin,
// exp, and pow between compilers while catching any change a listener
// could hear, and most they could not.
//
// A scene without a baseline fails and says how to approve one; a baseline
// that is still a Git LFS pointer (a checkout without LFS: the Windows CI
// jobs) skips.

#include <nepheline/audio/adpcm.h>
#include <nepheline/audio/lossless.h>
#include <nepheline/audio/mixer.h>
#include <nepheline/audio/wav.h>
#include <nepheline/platform/file.h>

#include <ntest/ntest.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define GOLDEN_RATE   48000U
#define GOLDEN_BLOCKS 280U   // 1.49 s
#define GOLDEN_FRAMES 71680U // GOLDEN_BLOCKS blocks
NPH_STATIC_ASSERT(GOLDEN_FRAMES == GOLDEN_BLOCKS * NPH_AUDIO_BLOCK_FRAMES,
                  "a whole number of blocks");
#define GOLDEN_PI 3.14159265358979323846

// The tolerances, against full scale.
#define GOLDEN_MAX_ERROR 2e-5
#define GOLDEN_RMS_ERROR 2e-6

typedef struct GoldenScene {
    NphAudioMixer *m;
    NphAudioClip clips[8];
    NphAudioBus buses[4];
    NphAudioVoice voices[32];
    NphAudioSnapshot snapshots[2];
    u64 rng;
} GoldenScene;

typedef void (*GoldenScript)(GoldenScene *s, u32 block);

static u64 golden_rng(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL); // splitmix64
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

static NphAudioMixer *golden_mixer(u32 max_real)
{
    NphAudioMixerDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.sample_rate = GOLDEN_RATE;
    d.channels = 2U;
    d.max_voices = 64U;
    d.max_real_voices = max_real;
    d.max_clips = 16U;
    d.max_buses = 8U;
    NphAudioMixer *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&d, &m), NPH_RESULT_OK);
    return m;
}

// A float clip from `fn(i, channel)` for `frames` frames at `rate`.
static NphAudioClip golden_clip(NphAudioMixer *m, u32 channels, u32 rate, u32 frames,
                                f64 (*fn)(u32 i, u32 c, u32 rate))
{
    f32 *x = (f32 *)NPH_ALLOC(nph_allocator_system(), (u64)frames * channels * sizeof(f32), 16U);
    NTEST_ASSERT_NOT_NULL(x);
    for (u32 i = 0U; i < frames; ++i) {
        for (u32 c = 0U; c < channels; ++c) {
            x[(u64)i * channels + c] = (f32)fn(i, c, rate);
        }
    }
    NphAudioClipDesc d;
    memset(&d, 0, sizeof d);
    d.codec = NPH_AUDIO_CODEC_PCM;
    d.format = NPH_AUDIO_FORMAT_F32;
    d.channels = channels;
    d.sample_rate = rate;
    d.frames = frames;
    d.data = x;
    d.size = (u64)frames * channels * sizeof(f32);
    NphAudioClip clip;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(m, &d, &clip), NPH_RESULT_OK);
    NPH_FREE(nph_allocator_system(), x, (u64)frames * channels * sizeof(f32), 16U);
    return clip;
}

static f64 sig_220(u32 i, u32 c, u32 rate)
{
    NPH_UNUSED(c);
    return 0.5 * sin(2.0 * GOLDEN_PI * 220.0 * (f64)i / (f64)rate);
}

static f64 sig_330(u32 i, u32 c, u32 rate)
{
    NPH_UNUSED(c);
    return 0.4 * sin(2.0 * GOLDEN_PI * 330.0 * (f64)i / (f64)rate);
}

static f64 sig_pluck(u32 i, u32 c, u32 rate)
{
    // A decaying harmonic note: the shape of a plucked string.
    NPH_UNUSED(c);
    const f64 t = (f64)i / (f64)rate;
    f64 v = 0.0;
    for (u32 h = 1U; h <= 6U; ++h) {
        v += exp(-3.0 * (f64)h * t) / (f64)h * sin(2.0 * GOLDEN_PI * 440.0 * (f64)h * t);
    }
    return 0.5 * v;
}

static f64 sig_chord(u32 i, u32 c, u32 rate)
{
    // A stereo chord near C4-E4-G4 with a slow beat between the channels.
    // Every frequency is a multiple of 5/3 Hz, so the chord repeats every
    // 0.6 s and a loop of that length (the codecs scene's) wraps without a
    // seam.
    const f64 t = (f64)i / (f64)rate;
    const f64 step = 5.0 / 3.0;
    const f64 detune = c == 0U ? 0.0 : step;
    return 0.2 * (sin(2.0 * GOLDEN_PI * (157.0 * step + detune) * t) +
                  sin(2.0 * GOLDEN_PI * (198.0 * step + detune) * t) +
                  sin(2.0 * GOLDEN_PI * (235.0 * step + detune) * t));
}

static f64 sig_voice(u32 i, u32 c, u32 rate)
{
    // Dialogue's stand-in: a buzz under formant-like peaks, in syllables.
    NPH_UNUSED(c);
    const f64 t = (f64)i / (f64)rate;
    const f64 envelope = 0.5 - 0.5 * cos(2.0 * GOLDEN_PI * 3.0 * t);
    f64 v = 0.0;
    for (u32 h = 1U; h <= 20U; ++h) {
        const f64 f = 140.0 * (f64)h;
        const f64 formant =
            exp(-pow((f - 700.0) / 300.0, 2.0)) + 0.6 * exp(-pow((f - 1200.0) / 400.0, 2.0));
        v += formant * sin(2.0 * GOLDEN_PI * f * t);
    }
    return 0.08 * envelope * v; // peaks near 0.47
}

// --- Scene: tones, pans, a sweep, a delayed start, a fade ---------------

static void script_tones(GoldenScene *s, u32 block)
{
    if (block == 0U) {
        s->clips[0] = golden_clip(s->m, 1U, GOLDEN_RATE, 96000U, sig_220);
        s->clips[1] = golden_clip(s->m, 1U, GOLDEN_RATE, 96000U, sig_330);
        s->clips[2] = golden_clip(s->m, 1U, GOLDEN_RATE, 48000U, sig_pluck);
        NphAudioPlayDesc d = nph_audio_play_desc_default(s->clips[0]);
        d.pan = -0.8F;
        d.volume = 0.5F;
        NTEST_ASSERT_EQ_INT(nph_audio_voice_play(s->m, &d, &s->voices[0]), NPH_RESULT_OK);
        d = nph_audio_play_desc_default(s->clips[1]);
        d.pan = -1.0F;
        d.volume = 0.6F;
        NTEST_ASSERT_EQ_INT(nph_audio_voice_play(s->m, &d, &s->voices[1]), NPH_RESULT_OK);
        d = nph_audio_play_desc_default(s->clips[2]);
        d.volume = 0.35F;
        d.delay_frames = 12345U;
        NTEST_ASSERT_EQ_INT(nph_audio_voice_play(s->m, &d, &s->voices[2]), NPH_RESULT_OK);
    }
    // The second tone sweeps left to right over the scene.
    nph_audio_voice_set_pan(s->m, s->voices[1], -1.0F + 2.0F * (f32)block / (f32)GOLDEN_BLOCKS);
    // The first swells and fades out.
    if (block < 100U) {
        nph_audio_voice_set_volume(s->m, s->voices[0], 0.5F + 0.003F * (f32)block);
    }
    if (block == 180U) {
        nph_audio_voice_stop(s->m, s->voices[0], 0.3F);
    }
}

// --- Scene: every codec, clip rates, pitches, a loop ---------------------

static void script_codecs(GoldenScene *s, u32 block)
{
    if (block == 0U) {
        // The chord at 44.1 kHz as PCM, lossless, and ADPCM.
        enum {
            FRAMES = 44100
        };
        i16 *pcm = (i16 *)NPH_ALLOC(nph_allocator_system(), (u64)FRAMES * 2U * sizeof(i16), 16U);
        i32 *ints = (i32 *)NPH_ALLOC(nph_allocator_system(), (u64)FRAMES * 2U * sizeof(i32), 16U);
        NTEST_ASSERT_NOT_NULL(pcm);
        NTEST_ASSERT_NOT_NULL(ints);
        for (u32 i = 0U; i < (u32)FRAMES; ++i) {
            for (u32 c = 0U; c < 2U; ++c) {
                const f64 v = sig_chord(i, c, 44100U);
                pcm[(u64)i * 2U + c] = (i16)floor(v * 32767.0 + 0.5);
                ints[(u64)i * 2U + c] = pcm[(u64)i * 2U + c];
            }
        }
        NphAudioClipDesc d;
        memset(&d, 0, sizeof d);
        d.codec = NPH_AUDIO_CODEC_PCM;
        d.format = NPH_AUDIO_FORMAT_S16;
        d.channels = 2U;
        d.sample_rate = 44100U;
        d.frames = FRAMES;
        d.data = pcm;
        d.size = (u64)FRAMES * 2U * sizeof(i16);
        d.loop_start = 4410U;
        d.loop_end = 30870U;
        NTEST_ASSERT_EQ_INT(nph_audio_clip_create(s->m, &d, &s->clips[0]), NPH_RESULT_OK);
        NphLosslessEncoded enc;
        NTEST_ASSERT_EQ_INT(
            nph_lossless_encode(nph_allocator_system(), ints, 2U, 16U, FRAMES, NULL, &enc),
            NPH_RESULT_OK);
        NphAudioClipDesc l = d;
        l.codec = NPH_AUDIO_CODEC_LOSSLESS;
        l.data = enc.stream.data;
        l.size = enc.stream.size;
        l.bits = 16U;
        l.block_frames = enc.stream.block_frames;
        l.block_offsets = enc.stream.block_offsets;
        l.block_count = enc.stream.block_count;
        NTEST_ASSERT_EQ_INT(nph_audio_clip_create(s->m, &l, &s->clips[1]), NPH_RESULT_OK);
        nph_lossless_encoded_free(&enc);
        NphAdpcmEncoded aenc;
        NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), pcm, 2U, FRAMES, &aenc),
                            NPH_RESULT_OK);
        NphAudioClipDesc a = d;
        a.codec = NPH_AUDIO_CODEC_ADPCM;
        a.data = aenc.stream.data;
        a.size = aenc.stream.size;
        NTEST_ASSERT_EQ_INT(nph_audio_clip_create(s->m, &a, &s->clips[2]), NPH_RESULT_OK);
        nph_adpcm_encoded_free(&aenc);
        NPH_FREE(nph_allocator_system(), pcm, (u64)FRAMES * 2U * sizeof(i16), 16U);
        NPH_FREE(nph_allocator_system(), ints, (u64)FRAMES * 2U * sizeof(i32), 16U);
        const f32 pitches[3] = {1.0F, 0.9F, 1.25F};
        for (u32 k = 0U; k < 3U; ++k) {
            NphAudioPlayDesc p = nph_audio_play_desc_default(s->clips[k]);
            p.pitch = pitches[k];
            p.volume = 0.4F;
            p.pan = -0.6F + 0.6F * (f32)k;
            p.loop = 1;
            p.start_frame = (u64)k * 1000U;
            NTEST_ASSERT_EQ_INT(nph_audio_voice_play(s->m, &p, &s->voices[k]), NPH_RESULT_OK);
        }
    }
    // The lossless voice's pitch glides down an octave and back. Arithmetic
    // only: a pitch from the C library's powf or sinf could differ in its
    // last bit between compilers, and a pitch's error accumulates as phase.
    const f32 phase = (f32)block / (f32)GOLDEN_BLOCKS;
    const f32 tri = phase < 0.5F ? 2.0F * phase : 2.0F - 2.0F * phase;
    nph_audio_voice_set_pitch(s->m, s->voices[1], 0.9F * (1.0F - 0.5F * tri));
    if (block == 140U) {
        nph_audio_voice_set_filters(s->m, s->voices[2], 1500.0F, 200.0F);
    }
}

// --- Scene: buses, sends, ducking, snapshots, mute ------------------------

static void script_buses(GoldenScene *s, u32 block)
{
    if (block == 0U) {
        s->clips[0] = golden_clip(s->m, 2U, GOLDEN_RATE, 96000U, sig_chord);
        s->clips[1] = golden_clip(s->m, 1U, GOLDEN_RATE, 48000U, sig_voice);
        s->clips[2] = golden_clip(s->m, 1U, GOLDEN_RATE, 48000U, sig_pluck);
        NphAudioBusDesc bd = nph_audio_bus_desc_default();
        bd.name = "music";
        NTEST_ASSERT_EQ_INT(nph_audio_bus_create(s->m, &bd, &s->buses[0]), NPH_RESULT_OK);
        bd.name = "dialogue";
        NTEST_ASSERT_EQ_INT(nph_audio_bus_create(s->m, &bd, &s->buses[1]), NPH_RESULT_OK);
        bd.name = "effects";
        NTEST_ASSERT_EQ_INT(nph_audio_bus_create(s->m, &bd, &s->buses[2]), NPH_RESULT_OK);
        bd.name = "aux";
        bd.volume = 0.5F;
        NTEST_ASSERT_EQ_INT(nph_audio_bus_create(s->m, &bd, &s->buses[3]), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_audio_bus_set_send(s->m, s->buses[2], 0U, s->buses[3], 0.7F),
                            NPH_RESULT_OK);
        nph_audio_bus_set_duck(s->m, s->buses[0], s->buses[1], -10.0F, -35.0F, 0.03F, 0.25F);
        const NphAudioSnapshotEntry e[2] = {{s->buses[0], -4.0F, 900.0F},
                                            {s->buses[2], -9.0F, 0.0F}};
        NTEST_ASSERT_EQ_INT(nph_audio_snapshot_create(s->m, e, 2U, &s->snapshots[0]),
                            NPH_RESULT_OK);
        NphAudioPlayDesc d = nph_audio_play_desc_default(s->clips[0]);
        d.bus = s->buses[0];
        d.loop = 1;
        d.volume = 0.6F;
        NTEST_ASSERT_EQ_INT(nph_audio_voice_play(s->m, &d, &s->voices[0]), NPH_RESULT_OK);
    }
    if (block == 40U) {
        NphAudioPlayDesc d = nph_audio_play_desc_default(s->clips[1]);
        d.bus = s->buses[1];
        NTEST_ASSERT_EQ_INT(nph_audio_voice_play(s->m, &d, &s->voices[1]), NPH_RESULT_OK);
    }
    if (block % 37U == 5U) {
        NphAudioPlayDesc d = nph_audio_play_desc_default(s->clips[2]);
        d.bus = s->buses[2];
        d.volume = 0.3F;
        d.pan = (f32)(block % 3U) - 1.0F;
        d.pitch = 0.75F + 0.125F * (f32)(block % 5U);
        NphAudioVoice v;
        NTEST_ASSERT_EQ_INT(nph_audio_voice_play(s->m, &d, &v), NPH_RESULT_OK);
    }
    if (block == 150U) {
        nph_audio_snapshot_set_weight(s->m, s->snapshots[0], 1.0F, 0.2F);
    }
    if (block == 230U) {
        nph_audio_snapshot_set_weight(s->m, s->snapshots[0], 0.0F, 0.1F);
    }
    if (block == 200U) {
        nph_audio_bus_set_mute(s->m, s->buses[3], true);
    }
}

// --- Scene: more voices than real ones -----------------------------------

static void script_virtual(GoldenScene *s, u32 block)
{
    if (block == 0U) {
        s->clips[0] = golden_clip(s->m, 1U, GOLDEN_RATE, 48000U, sig_pluck);
        s->clips[1] = golden_clip(s->m, 1U, 32000U, 64000U, sig_220);
    }
    if (block % 9U == 0U && block < 240U) {
        const u32 slot = (block / 9U) % 32U;
        NphAudioPlayDesc d = nph_audio_play_desc_default(s->clips[slot % 2U]);
        d.volume = 0.05F + 0.04F * (f32)(golden_rng(&s->rng) % 10U);
        d.priority = (u8)(golden_rng(&s->rng) % 4U * 60U);
        d.pitch = 0.5F + 0.0625F * (f32)(golden_rng(&s->rng) % 24U);
        d.pan = -1.0F + 0.25F * (f32)(golden_rng(&s->rng) % 9U);
        d.loop = (slot % 3U) == 0U;
        NphAudioVoice v;
        if (nph_audio_voice_play(s->m, &d, &v) == NPH_RESULT_OK) {
            s->voices[slot] = v;
        }
    }
    if (block % 23U == 11U) {
        nph_audio_voice_stop(s->m, s->voices[golden_rng(&s->rng) % 32U], 0.05F);
    }
}

// --- The comparison --------------------------------------------------------

typedef enum GoldenOutcome {
    GOLDEN_COMPARED = 0,
    GOLDEN_NO_BASELINE = 1,
    GOLDEN_LFS_POINTER = 2,
    GOLDEN_BAD_BASELINE = 3,
} GoldenOutcome;

typedef struct GoldenComparison {
    GoldenOutcome outcome;
    f64 worst;    // the largest difference
    u32 worst_at; // its frame
    f64 rms;      // the RMS of the difference
} GoldenComparison;

static void golden_path(char *out, u64 cap, const char *dir, const char *name)
{
    (void)snprintf(out, (size_t)cap, "%s/%s.wav", dir, name);
}

// Renders the scene: two planes, left then right, in one allocation of
// 2 * GOLDEN_FRAMES samples.
static f32 *golden_render(GoldenScript script, u32 max_real)
{
    GoldenScene s;
    memset(&s, 0, sizeof s);
    s.rng = 2026U;
    s.m = golden_mixer(max_real);
    const u64 bytes = (u64)GOLDEN_FRAMES * 2U * sizeof(f32);
    f32 *mix = (f32 *)NPH_ALLOC(nph_allocator_system(), bytes, 16U);
    f32 *planes = (f32 *)NPH_ALLOC(nph_allocator_system(), bytes, 16U);
    NTEST_ASSERT_NOT_NULL(mix);
    NTEST_ASSERT_NOT_NULL(planes);
    for (u32 b = 0U; b < GOLDEN_BLOCKS; ++b) {
        script(&s, b);
        nph_audio_mixer_update(s.m);
        nph_audio_mixer_render(s.m, mix + (u64)b * NPH_AUDIO_BLOCK_FRAMES * 2U,
                               NPH_AUDIO_BLOCK_FRAMES);
    }
    nph_audio_mixer_destroy(s.m);
    for (u32 i = 0U; i < GOLDEN_FRAMES; ++i) {
        planes[i] = mix[(u64)i * 2U];
        planes[GOLDEN_FRAMES + i] = mix[(u64)i * 2U + 1U];
    }
    NPH_FREE(nph_allocator_system(), mix, bytes, 16U);
    return planes;
}

// Writes the render to the output directory, for review and approval.
static void golden_write(const char *name, const f32 *planes)
{
    const f32 *channels[2] = {planes, planes + GOLDEN_FRAMES};
    const u64 size = nph_wav_size(NPH_WAV_FLOAT32, 2U, GOLDEN_FRAMES);
    u8 *file = (u8 *)NPH_ALLOC(nph_allocator_system(), size, 16U);
    NTEST_ASSERT_NOT_NULL(file);
    NTEST_ASSERT_EQ_INT(
        nph_wav_write(NPH_WAV_FLOAT32, 2U, GOLDEN_RATE, channels, GOLDEN_FRAMES, file),
        NPH_RESULT_OK);
    char path[1024];
    golden_path(path, sizeof path, NPH_AUDIO_GOLDEN_OUTPUT_DIR, name);
    const NphResult dir = nph_fs_create_directories(NPH_AUDIO_GOLDEN_OUTPUT_DIR);
    const NphResult wrote = dir == NPH_RESULT_OK ? nph_file_write_all(path, file, size) : dir;
    NPH_FREE(nph_allocator_system(), file, size, 16U);
    NTEST_ASSERT_EQ_INT(wrote, NPH_RESULT_OK);
}

static GoldenComparison golden_compare(const char *name, const f32 *planes)
{
    GoldenComparison r;
    memset(&r, 0, sizeof r);
    char path[1024];
    golden_path(path, sizeof path, NPH_AUDIO_GOLDEN_BASELINE_DIR, name);
    void *base = NULL;
    u64 base_size = 0U;
    if (nph_file_read_all(path, nph_allocator_system(), &base, &base_size) != NPH_RESULT_OK) {
        r.outcome = GOLDEN_NO_BASELINE;
        return r;
    }
    static const char lfs[] = "version https://git-lfs";
    NphWavInfo info;
    if (base_size >= sizeof lfs - 1U && memcmp(base, lfs, sizeof lfs - 1U) == 0) {
        r.outcome = GOLDEN_LFS_POINTER;
    } else if (nph_wav_parse(base, base_size, &info) != NPH_RESULT_OK ||
               info.encoding != NPH_WAV_FLOAT32 || info.channels != 2U ||
               info.sample_rate != GOLDEN_RATE || info.frames != GOLDEN_FRAMES) {
        r.outcome = GOLDEN_BAD_BASELINE;
    } else {
        f64 sum = 0.0;
        for (u32 c = 0U; c < 2U; ++c) {
            for (u32 i = 0U; i < GOLDEN_FRAMES; ++i) {
                const f64 e = fabs((f64)planes[(u64)c * GOLDEN_FRAMES + i] -
                                   nph_wav_sample_float(&info, i, c));
                sum += e * e;
                if (e > r.worst) {
                    r.worst = e;
                    r.worst_at = i;
                }
            }
        }
        r.rms = sqrt(sum / (2.0 * (f64)GOLDEN_FRAMES));
    }
    nph_file_free_all(nph_allocator_system(), base, base_size);
    return r;
}

static void golden_run(const char *name, GoldenScript script, u32 max_real)
{
    f32 *planes = golden_render(script, max_real);
    golden_write(name, planes);
    const GoldenComparison r = golden_compare(name, planes);
    // A scene must not clip: the output's clamp would hide its gains.
    f32 peak = 0.0F;
    for (u64 i = 0U; i < (u64)GOLDEN_FRAMES * 2U; ++i) {
        peak = fabsf(planes[i]) > peak ? fabsf(planes[i]) : peak;
    }
    NPH_FREE(nph_allocator_system(), planes, (u64)GOLDEN_FRAMES * 2U * sizeof(f32), 16U);
    NTEST_ASSERT_TRUE(peak < 0.95F);
    if (r.outcome == GOLDEN_LFS_POINTER) {
        NTEST_SKIP("%s: the baseline is a Git LFS pointer (checked out without LFS)", name);
    }
    if (r.outcome == GOLDEN_NO_BASELINE) {
        NTEST_FAIL("%s: no baseline in %s; listen to %s/%s.wav, then approve it with "
                   "python scripts/audio_golden_approve.py --from %s %s",
                   name, NPH_AUDIO_GOLDEN_BASELINE_DIR, NPH_AUDIO_GOLDEN_OUTPUT_DIR, name,
                   NPH_AUDIO_GOLDEN_OUTPUT_DIR, name);
    }
    if (r.outcome == GOLDEN_BAD_BASELINE) {
        NTEST_FAIL("%s: the baseline is not a 2-channel 48 kHz float WAV of %u frames", name,
                   GOLDEN_FRAMES);
    }
    NTEST_LOG("%s: largest difference %.3g at frame %u, RMS of the difference %.3g", name, r.worst,
              r.worst_at, r.rms);
    NTEST_ASSERT_TRUE(r.worst <= GOLDEN_MAX_ERROR);
    NTEST_ASSERT_TRUE(r.rms <= GOLDEN_RMS_ERROR);
}

NTEST(audio_golden, tones_and_pans)
{
    golden_run("tones_and_pans", script_tones, 16U);
}

NTEST(audio_golden, codecs_and_rates)
{
    golden_run("codecs_and_rates", script_codecs, 16U);
}

NTEST(audio_golden, buses_ducking_snapshots)
{
    golden_run("buses_ducking_snapshots", script_buses, 16U);
}

NTEST(audio_golden, virtual_voices)
{
    golden_run("virtual_voices", script_virtual, 4U);
}
