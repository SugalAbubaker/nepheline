// SPDX-License-Identifier: Apache-2.0
// make_sounds: the samples' sounds, synthesized when the samples are built
// (the repository holds no recordings), written as WAV files for nph_cook
// (docs/cook.md "Sounds"):
//
//     make_sounds <out_dir>
//
//   music.wav   16 s, stereo, 24-bit: a slow progression (A, F#m, D, E) as
//               a pad, a bass, and an arpeggio, every note inside its bar,
//               so the loop meets itself in silence. 8 s or more: cooked to
//               stream, lossless.
//   wind.wav    12 s, stereo, 16-bit: noise through low-passes that wander,
//               in gusts, its end crossfaded into its start so it loops.
//               Streamed.
//   hum.wav     2 s, mono, 16-bit: a machine's hum, 110 Hz and a fifth
//               above, beating once, whole cycles of each so it loops. In
//               memory.
//   chime.wav   1.5 s, mono, 16-bit: a bell by frequency modulation
//               (Chowning, "The Synthesis of Complex Audio Spectra by Means
//               of Frequency Modulation", JAES 1973). In memory.
//   knock.wav   0.2 s, mono, 16-bit: a knock on wood, cooked as ADPCM (the
//               knock.wav.import.json written beside it).
//   thud.wav    0.4 s, mono, 16-bit: a crate landing (game_physics).
//   bonk.wav    0.3 s, mono, 16-bit: a ball bouncing (game_physics).
//
// The synthesis is deterministic (PCG32 noise, fixed seeds); each sound is
// scaled to its own peak. Exits 0, or 1 with a message.
#include <nepheline/audio/wav.h>
#include <nepheline/core/alloc.h>
#include <nepheline/math/random.h>
#include <nepheline/platform/file.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000U
#define TAU  6.283185307179586

typedef struct Sound {
    const char *name;
    u32 channels;
    u64 frames;
    NphWavEncoding encoding;
    f32 *planes[2];
} Sound;

static void fail(const char *what, const char *detail)
{
    fprintf(stderr, "make_sounds: %s%s%s\n", what, detail != NULL ? ": " : "",
            detail != NULL ? detail : "");
    exit(1);
}

static Sound sound_new(const char *name, u32 channels, f64 seconds, NphWavEncoding encoding)
{
    Sound s;
    memset(&s, 0, sizeof s);
    s.name = name;
    s.channels = channels;
    s.frames = (u64)(seconds * (f64)RATE);
    s.encoding = encoding;
    for (u32 c = 0; c < channels; ++c) {
        s.planes[c] = NPH_ALLOC_ARRAY_ZEROED(nph_allocator_system(), f32, s.frames);
        if (s.planes[c] == NULL) {
            fail("out of memory for", name);
        }
    }
    return s;
}

static void sound_free(Sound *s)
{
    for (u32 c = 0; c < s->channels; ++c) {
        NPH_FREE_ARRAY(nph_allocator_system(), f32, s->planes[c], s->frames);
    }
}

// Scales the sound so its largest sample's magnitude is `peak`.
static void normalise(Sound *s, f32 peak)
{
    f32 most = 0.0F;
    for (u32 c = 0; c < s->channels; ++c) {
        for (u64 i = 0; i < s->frames; ++i) {
            most = fmaxf(most, fabsf(s->planes[c][i]));
        }
    }
    if (!(most > 0.0F)) {
        fail("a silent sound", s->name);
    }
    const f32 k = peak / most;
    for (u32 c = 0; c < s->channels; ++c) {
        for (u64 i = 0; i < s->frames; ++i) {
            s->planes[c][i] *= k;
        }
    }
}

static void write_file(const char *dir, const char *name, const void *data, u64 size)
{
    char path[1100];
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    if (nph_file_write_all(path, data, size) != NPH_RESULT_OK) {
        fail("cannot write", path);
    }
}

static void write_sound(const char *dir, Sound *s)
{
    const u64 size = nph_wav_size(s->encoding, s->channels, s->frames);
    void *bytes = NPH_ALLOC(nph_allocator_system(), size, 16U);
    if (bytes == NULL) {
        fail("out of memory for", s->name);
    }
    const f32 *const planes[2] = {s->planes[0], s->planes[1]};
    if (nph_wav_write(s->encoding, s->channels, RATE, planes, s->frames, bytes) != NPH_RESULT_OK) {
        fail("cannot encode", s->name);
    }
    write_file(dir, s->name, bytes, size);
    NPH_FREE(nph_allocator_system(), bytes, size, 16U);
    sound_free(s);
}

// 0 before `at`, 1 from `at + len`, a raised cosine between.
static f64 ramp(f64 t, f64 at, f64 len)
{
    if (t <= at) {
        return 0.0;
    }
    if (t >= at + len) {
        return 1.0;
    }
    return 0.5 - 0.5 * cos(TAU * 0.5 * (t - at) / len);
}

// A note's level: rising over `attack` from `start`, falling over `release`
// to silence at `end`.
static f64 envelope(f64 t, f64 start, f64 end, f64 attack, f64 release)
{
    if (t < start || t >= end) {
        return 0.0;
    }
    return ramp(t, start, attack) * (1.0 - ramp(t, end - release, release));
}

static f64 midi_hz(i32 note)
{
    return 440.0 * pow(2.0, (f64)(note - 69) / 12.0);
}

// A one-pole low-pass's coefficient for `hz`.
static f64 lowpass_coeff(f64 hz)
{
    return 1.0 - exp(-TAU * hz / (f64)RATE);
}

// ---------------------------------------------------------------------------
// The sounds
// ---------------------------------------------------------------------------

#define BAR_SECONDS 4.0

static void make_music(const char *dir)
{
    Sound s = sound_new("music.wav", 2U, 4.0 * BAR_SECONDS, NPH_WAV_PCM_S24);
    // Each bar's bass and triad (MIDI notes).
    static const i32 bass[4] = {45, 42, 38, 40};
    static const i32 triad[4][3] = {{57, 61, 64}, {54, 57, 61}, {50, 54, 57}, {52, 56, 59}};
    static const u32 arp[8] = {0U, 1U, 2U, 1U, 0U, 1U, 2U, 1U};
    for (u64 i = 0; i < s.frames; ++i) {
        const f64 t = (f64)i / (f64)RATE;
        const u32 bar = (u32)(t / BAR_SECONDS);
        const f64 start = (f64)bar * BAR_SECONDS;
        f64 left = 0.0;
        f64 right = 0.0;
        // The pad: four partials a note, a little flat on the left and sharp
        // on the right (their slow beating widens it).
        const f64 pad = envelope(t, start, start + BAR_SECONDS - 0.05, 0.8, 1.0);
        for (u32 n = 0; n < 3U; ++n) {
            const f64 f = midi_hz(triad[bar][n]);
            for (u32 k = 1; k <= 4U; ++k) {
                const f64 a = 0.25 * pad / (f64)(k * k);
                left += a * sin(TAU * f * (f64)k * 0.999 * t);
                right += a * sin(TAU * f * (f64)k * 1.001 * t);
            }
        }
        // The bass, in the middle.
        const f64 fb = midi_hz(bass[bar]);
        const f64 low = 0.5 * envelope(t, start, start + BAR_SECONDS - 0.1, 0.05, 0.6) *
                        (sin(TAU * fb * t) + 0.3 * sin(TAU * 2.0 * fb * t));
        left += low;
        right += low;
        // The arpeggio: eight plucks a bar an octave up, each over within
        // 0.45 s, left and right by turns.
        const u32 step = (u32)((t - start) / 0.5);
        const f64 at = start + 0.5 * (f64)step;
        const f64 u = t - at;
        if (u < 0.45) {
            const f64 f = midi_hz(triad[bar][arp[step % 8U]] + 12);
            const f64 level =
                0.3 * exp(-u / 0.18) * ramp(u, 0.0, 0.005) * (1.0 - ramp(u, 0.41, 0.04));
            const f64 pluck = level * (sin(TAU * f * u) + 0.4 * sin(TAU * 2.0 * f * u));
            left += pluck * ((step & 1U) == 0U ? 1.0 : 0.7);
            right += pluck * ((step & 1U) == 0U ? 0.7 : 1.0);
        }
        s.planes[0][i] = (f32)left;
        s.planes[1][i] = (f32)right;
    }
    normalise(&s, 0.5F);
    write_sound(dir, &s);
}

#define WIND_SECONDS 12.0
#define WIND_FADE    1.0

static void make_wind(const char *dir)
{
    // Rendered a fade longer than its loop, the extra crossfaded into the
    // start: the loop's end runs on into its beginning.
    Sound raw = sound_new("wind.wav", 2U, WIND_SECONDS + WIND_FADE, NPH_WAV_PCM_S16);
    for (u32 c = 0; c < 2U; ++c) {
        NphPcg32 rng;
        nph_pcg32_seed(&rng, 0x77696E64ULL, c);
        f64 y1 = 0.0;
        f64 y2 = 0.0;
        const f64 phase = (f64)c * 1.7;
        for (u64 i = 0; i < raw.frames; ++i) {
            const f64 t = (f64)i / (f64)RATE;
            const f64 hz = 350.0 + 250.0 * sin(TAU * 0.11 * t + phase) +
                           150.0 * sin(TAU * 0.037 * t + 2.0 * phase);
            const f64 a = lowpass_coeff(hz);
            const f64 x = 2.0 * nph_pcg32_next_f64(&rng) - 1.0;
            y1 += a * (x - y1);
            y2 += a * (y1 - y2);
            const f64 gust = 0.6 + 0.4 * (0.5 + 0.5 * sin(TAU * 0.07 * t + phase));
            raw.planes[c][i] = (f32)(gust * y2);
        }
    }
    Sound s = sound_new("wind.wav", 2U, WIND_SECONDS, NPH_WAV_PCM_S16);
    const u64 fade = (u64)(WIND_FADE * (f64)RATE);
    for (u32 c = 0; c < 2U; ++c) {
        for (u64 i = 0; i < s.frames; ++i) {
            f64 y = (f64)raw.planes[c][i];
            if (i < fade) {
                // Equal power: the two halves are uncorrelated noise.
                const f64 w = 0.25 * TAU * (f64)i / (f64)fade;
                y = y * sin(w) + (f64)raw.planes[c][s.frames + i] * cos(w);
            }
            s.planes[c][i] = (f32)y;
        }
    }
    sound_free(&raw);
    normalise(&s, 0.4F);
    write_sound(dir, &s);
}

static void make_hum(const char *dir)
{
    // 110 Hz (220 cycles in 2 s), 165 Hz (330), and a beat at 0.5 Hz (1):
    // whole cycles of each, so the loop is seamless.
    Sound s = sound_new("hum.wav", 1U, 2.0, NPH_WAV_PCM_S16);
    for (u64 i = 0; i < s.frames; ++i) {
        const f64 t = (f64)i / (f64)RATE;
        f64 y = 0.0;
        for (u32 k = 1; k <= 10U; ++k) {
            y += sin(TAU * 110.0 * (f64)k * t + 0.3 * (f64)k) / (f64)k;
        }
        for (u32 k = 1; k <= 4U; ++k) {
            y += 0.3 * sin(TAU * 165.0 * (f64)k * t) / (f64)k;
        }
        s.planes[0][i] = (f32)(y * (1.0 + 0.25 * sin(TAU * 0.5 * t)));
    }
    normalise(&s, 0.5F);
    write_sound(dir, &s);
}

static void make_chime(const char *dir)
{
    // A carrier at 880 Hz modulated at 1.4 times it, the index falling
    // faster than the level: bright, then pure (Chowning's bell).
    Sound s = sound_new("chime.wav", 1U, 1.5, NPH_WAV_PCM_S16);
    const f64 end = (f64)s.frames / (f64)RATE;
    for (u64 i = 0; i < s.frames; ++i) {
        const f64 t = (f64)i / (f64)RATE;
        const f64 index = 3.5 * exp(-t / 0.4);
        const f64 level = exp(-t / 0.6) * ramp(t, 0.0, 0.002) * (1.0 - ramp(t, end - 0.06, 0.06));
        s.planes[0][i] = (f32)(level * sin(TAU * 880.0 * t + index * sin(TAU * 1232.0 * t)));
    }
    normalise(&s, 0.6F);
    write_sound(dir, &s);
}

// A short noise burst through a low-pass at `hz`, falling over `tau`.
static f64 burst(NphPcg32 *rng, f64 *state, f64 hz, f64 t, f64 tau)
{
    const f64 x = 2.0 * nph_pcg32_next_f64(rng) - 1.0;
    *state += lowpass_coeff(hz) * (x - *state);
    return *state * exp(-t / tau);
}

static void make_knock(const char *dir)
{
    Sound s = sound_new("knock.wav", 1U, 0.2, NPH_WAV_PCM_S16);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0x6B6E6F63ULL, 1U);
    f64 lp = 0.0;
    const f64 end = (f64)s.frames / (f64)RATE;
    for (u64 i = 0; i < s.frames; ++i) {
        const f64 t = (f64)i / (f64)RATE;
        const f64 body =
            sin(TAU * 190.0 * t) * exp(-t / 0.035) + 0.5 * sin(TAU * 420.0 * t) * exp(-t / 0.02);
        const f64 click = 2.0 * burst(&rng, &lp, 3000.0, t, 0.003);
        const f64 level = ramp(t, 0.0, 0.0005) * (1.0 - ramp(t, end - 0.02, 0.02));
        s.planes[0][i] = (f32)(level * (body + click));
    }
    normalise(&s, 0.7F);
    write_sound(dir, &s);
    // Short and percussive: ADPCM's quarter of the bytes costs it nothing
    // audible (docs/cook.md "Sounds").
    static const char settings[] = "{\"codec\": \"adpcm\"}\n";
    write_file(dir, "knock.wav.import.json", settings, sizeof settings - 1U);
}

static void make_thud(const char *dir)
{
    // A low thump falling from 110 Hz to 60, with a muffled crack.
    Sound s = sound_new("thud.wav", 1U, 0.4, NPH_WAV_PCM_S16);
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0x74687564ULL, 1U);
    f64 lp = 0.0;
    f64 phase = 0.0;
    const f64 end = (f64)s.frames / (f64)RATE;
    for (u64 i = 0; i < s.frames; ++i) {
        const f64 t = (f64)i / (f64)RATE;
        phase += TAU * (60.0 + 50.0 * exp(-t / 0.04)) / (f64)RATE;
        const f64 body = sin(phase) * exp(-t / 0.09);
        const f64 crack = burst(&rng, &lp, 800.0, t, 0.015);
        const f64 level = ramp(t, 0.0, 0.001) * (1.0 - ramp(t, end - 0.04, 0.04));
        s.planes[0][i] = (f32)(level * (body + 2.0 * crack));
    }
    normalise(&s, 0.8F);
    write_sound(dir, &s);
}

static void make_bonk(const char *dir)
{
    // A hollow note dropping a little as it rings.
    Sound s = sound_new("bonk.wav", 1U, 0.3, NPH_WAV_PCM_S16);
    f64 phase = 0.0;
    const f64 end = (f64)s.frames / (f64)RATE;
    for (u64 i = 0; i < s.frames; ++i) {
        const f64 t = (f64)i / (f64)RATE;
        phase += TAU * (330.0 + 80.0 * exp(-t / 0.02)) / (f64)RATE;
        const f64 tone = sin(phase) * exp(-t / 0.07) + 0.25 * sin(2.0 * phase) * exp(-t / 0.03);
        const f64 level = ramp(t, 0.0, 0.001) * (1.0 - ramp(t, end - 0.03, 0.03));
        s.planes[0][i] = (f32)(level * tone);
    }
    normalise(&s, 0.7F);
    write_sound(dir, &s);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: make_sounds <out_dir>\n");
        return 1;
    }
    const char *dir = argv[1];
    if (nph_fs_create_directories(dir) != NPH_RESULT_OK) {
        fail("cannot make", dir);
    }
    make_music(dir);
    make_wind(dir);
    make_hum(dir);
    make_chime(dir);
    make_knock(dir);
    make_thud(dir);
    make_bonk(dir);
    return 0;
}
