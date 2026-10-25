// SPDX-License-Identifier: Apache-2.0
// The audio thread's rules, verified (CLAUDE.md §12, Phase 12's exit: "no
// audio-thread allocations (verified)"; §8.10: "nothing on the audio thread
// allocates or locks"). A busy scene is rendered while two things watch:
//   - the real-time scope (core/realtime.h) the render runs in, which
//     reports every allocation through the engine's allocators, every lock,
//     wait, sleep, and file or asynchronous I/O call;
//   - in the sanitizer builds on Linux, the sanitizer allocator's own hooks,
//     which see every malloc and free in the process, the C library's and
//     the OS's included, and count those made inside a scope.
// And the game thread and an audio thread run together, which the
// ThreadSanitizer job checks for races.

#include <nepheline/core/realtime.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "test_mixer_support.h"

#if (NPH_ASAN_ENABLED || NPH_TSAN_ENABLED) && NPH_PLATFORM_LINUX && !NPH_COMPILER_MSVC
#if NPH_COMPILER_CLANG
#include <sanitizer/allocator_interface.h>
#else
// GCC's sanitizer runtime has the hooks (they are sanitizer_common's) but
// GCC does not install compiler-rt's header for them.
int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void *, size_t),
                                              void (*free_hook)(const volatile void *));
#endif
#define TEST_MALLOC_HOOKS 1
#else
#define TEST_MALLOC_HOOKS 0
#endif

static NphAtomicU64 g_rt_violations;
static NphAtomicU64 g_rt_mallocs;

#if NPH_ASSERTS_ENABLED
static void rt_count(NphRealtimeViolation kind, const char *what, NphCallsite site, void *user)
{
    NPH_UNUSED(kind);
    NPH_UNUSED(what);
    NPH_UNUSED(site);
    NPH_UNUSED(user);
    (void)nph_atomic_fetch_add_u64(&g_rt_violations, 1U, NPH_MEMORY_ORDER_RELAXED);
}
#endif

#if TEST_MALLOC_HOOKS
static void rt_malloc_hook(const volatile void *ptr, size_t size)
{
    NPH_UNUSED(ptr);
    NPH_UNUSED(size);
    if (nph_realtime_active()) {
        (void)nph_atomic_fetch_add_u64(&g_rt_mallocs, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
}

static void rt_free_hook(const volatile void *ptr)
{
    NPH_UNUSED(ptr);
    if (nph_realtime_active()) {
        (void)nph_atomic_fetch_add_u64(&g_rt_mallocs, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
}

static void rt_install_malloc_hooks(void)
{
    // The sanitizer keeps hooks for the process's lifetime: install once.
    static bool installed = false;
    if (!installed) {
        installed = __sanitizer_install_malloc_and_free_hooks(rt_malloc_hook, rt_free_hook) != 0;
    }
}
#endif

// A stream's bytes, from memory as a file's would be.
typedef struct RtSource {
    const u8 *data;
    u64 size;
} RtSource;

static NphResult rt_read(void *user, u64 offset, void *dst, u64 size)
{
    const RtSource *s = (const RtSource *)user;
    if (offset > s->size || size > s->size - offset) {
        return NPH_RESULT_IO_ERROR;
    }
    memcpy(dst, s->data + offset, (size_t)size);
    return NPH_RESULT_OK;
}

// A scene that uses everything a block can do: every codec, streams, loops,
// pitch, filters, sends, delays, buses, ducking, snapshots, and more voices
// than real ones, so voices go virtual and come back all the time.
typedef struct RtScene {
    NphAudioMixer *m;
    NphAudioClip clips[4];
    NphAudioBus music;
    NphAudioBus effects;
    NphAudioBus reverb;
    NphAudioSnapshot pause;
    NphAudioEffect fx[5]; // equaliser, compressor, echo, reverberator, limiter
    NphAudioVoice voices[48];
    // Two streams, PCM looping and lossless in 3D, made again now and then
    // (a stream's clip plays once).
    NphAudioStreamer *streamer;
    i16 *stream_pcm;
    NphLosslessEncoded stream_enc;
    RtSource sources[2];
    NphAudioStreamDesc stream_descs[2];
    NphAudioClip streams[2];
    NphAudioVoice stream_voices[2];
    u64 rng;
    bool threaded; // an audio thread renders it as the game thread steers
} RtScene;

enum {
    RT_FRAMES = 20000
};

static void rt_scene_build(RtScene *s)
{
    memset(s, 0, sizeof *s);
    s->rng = 99U;
    s->m = tm_mixer(2U, 64U, 12U);
    enum {
        FRAMES = RT_FRAMES
    };
    i16 *pcm = (i16 *)NPH_ALLOC(nph_allocator_system(), (u64)FRAMES * 2U * sizeof(i16), 16U);
    i32 *ints = (i32 *)NPH_ALLOC(nph_allocator_system(), (u64)FRAMES * 2U * sizeof(i32), 16U);
    NTEST_ASSERT_NOT_NULL(pcm);
    NTEST_ASSERT_NOT_NULL(ints);
    for (u32 i = 0U; i < (u32)FRAMES; ++i) {
        const f64 t = (f64)i / 44100.0;
        const f64 v = 0.3 * sin(2.0 * TEST_PI * 330.0 * t) + 0.1 * sin(2.0 * TEST_PI * 2700.0 * t);
        pcm[(u64)i * 2U] = (i16)(v * 32767.0);
        pcm[(u64)i * 2U + 1U] = (i16)(-v * 30000.0);
        ints[(u64)i * 2U] = pcm[(u64)i * 2U];
        ints[(u64)i * 2U + 1U] = pcm[(u64)i * 2U + 1U];
    }
    NphAudioClipDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.codec = NPH_AUDIO_CODEC_PCM;
    cd.format = NPH_AUDIO_FORMAT_S16;
    cd.channels = 2U;
    cd.sample_rate = 44100U;
    cd.frames = FRAMES;
    cd.data = pcm;
    cd.size = (u64)FRAMES * 2U * sizeof(i16);
    cd.loop_start = 1000U;
    cd.loop_end = 15000U;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(s->m, &cd, &s->clips[0]), NPH_RESULT_OK);
    const NphLosslessParams lp = {1024U, 0U, 0};
    NphLosslessEncoded enc;
    NTEST_ASSERT_EQ_INT(
        nph_lossless_encode(nph_allocator_system(), ints, 2U, 16U, FRAMES, &lp, &enc),
        NPH_RESULT_OK);
    NphAudioClipDesc ld = cd;
    ld.codec = NPH_AUDIO_CODEC_LOSSLESS;
    ld.data = enc.stream.data;
    ld.size = enc.stream.size;
    ld.bits = 16U;
    ld.block_frames = enc.stream.block_frames;
    ld.block_offsets = enc.stream.block_offsets;
    ld.block_count = enc.stream.block_count;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(s->m, &ld, &s->clips[1]), NPH_RESULT_OK);
    NphAdpcmEncoded aenc;
    NTEST_ASSERT_EQ_INT(nph_adpcm_encode(nph_allocator_system(), pcm, 2U, FRAMES, &aenc),
                        NPH_RESULT_OK);
    NphAudioClipDesc ad = cd;
    ad.codec = NPH_AUDIO_CODEC_ADPCM;
    ad.data = aenc.stream.data;
    ad.size = aenc.stream.size;
    NTEST_ASSERT_EQ_INT(nph_audio_clip_create(s->m, &ad, &s->clips[2]), NPH_RESULT_OK);
    nph_adpcm_encoded_free(&aenc);
    s->clips[3] = tm_tone(s->m, 523.25, 30000U, 32000U, 0.4F, NULL);
    NPH_FREE(nph_allocator_system(), ints, (u64)FRAMES * 2U * sizeof(i32), 16U);

    // The streams read the same material as it plays: it stays.
    NphAudioStreamerDesc sdesc;
    memset(&sdesc, 0, sizeof sdesc);
    sdesc.alloc = nph_allocator_system();
    sdesc.period_ms = 2U;
    NTEST_ASSERT_EQ_INT(nph_audio_streamer_create(&sdesc, &s->streamer), NPH_RESULT_OK);
    s->stream_pcm = pcm;
    s->stream_enc = enc;
    s->sources[0] = (RtSource){(const u8 *)pcm, cd.size};
    s->sources[1] = (RtSource){enc.stream.data, enc.stream.size};
    for (u32 k = 0U; k < 2U; ++k) {
        NphAudioStreamDesc *sd = &s->stream_descs[k];
        memset(sd, 0, sizeof *sd);
        sd->source = (NphAudioStreamSource){rt_read, &s->sources[k]};
        sd->codec = k == 0U ? NPH_AUDIO_CODEC_PCM : NPH_AUDIO_CODEC_LOSSLESS;
        sd->format = NPH_AUDIO_FORMAT_S16;
        sd->channels = 2U;
        sd->sample_rate = 44100U;
        sd->frames = FRAMES;
        sd->size = s->sources[k].size;
        sd->bits = 16U;
        sd->block_frames = enc.stream.block_frames;
        sd->block_offsets = enc.stream.block_offsets;
        sd->block_count = enc.stream.block_count;
        sd->loop_start = 1000U;
        sd->loop_end = 15000U;
        sd->loop = k == 0U;
        sd->buffer_seconds = 0.2F;
    }

    NphAudioBusDesc bd = nph_audio_bus_desc_default();
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(s->m, &bd, &s->music), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(s->m, &bd, &s->effects), NPH_RESULT_OK);
    bd.volume = 0.6F;
    NTEST_ASSERT_EQ_INT(nph_audio_bus_create(s->m, &bd, &s->reverb), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_bus_set_send(s->m, s->effects, 0U, s->reverb, 0.3F),
                        NPH_RESULT_OK);
    nph_audio_bus_set_duck(s->m, s->music, s->effects, -9.0F, -30.0F, 0.02F, 0.3F);
    const NphAudioSnapshotEntry entries[2] = {{s->music, -6.0F, 1200.0F},
                                              {s->effects, -12.0F, 0.0F}};
    NTEST_ASSERT_EQ_INT(nph_audio_snapshot_create(s->m, entries, 2U, &s->pause), NPH_RESULT_OK);
    // Every effect: an equaliser and a compressor on the music, an echo on
    // the effects, a reverberator on the send bus, a limiter on the master.
    NphAudioEffectDesc fx = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_EQ);
    fx.eq[0] = (NphAudioEqBand){NPH_AUDIO_FILTER_LOW_SHELF, 150.0F, 0.7071F, 3.0F, 1};
    fx.eq[1] = (NphAudioEqBand){NPH_AUDIO_FILTER_PEAK, 2500.0F, 1.0F, -4.0F, 1};
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(s->m, s->music, &fx, &s->fx[0]), NPH_RESULT_OK);
    fx = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_COMPRESSOR);
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(s->m, s->music, &fx, &s->fx[1]), NPH_RESULT_OK);
    fx = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_ECHO);
    fx.echo.ping_pong = 1;
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(s->m, s->effects, &fx, &s->fx[2]), NPH_RESULT_OK);
    fx = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_REVERB);
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(s->m, s->reverb, &fx, &s->fx[3]), NPH_RESULT_OK);
    fx = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_LIMITER);
    NTEST_ASSERT_EQ_INT(nph_audio_effect_create(s->m, NPH_AUDIO_MASTER_BUS, &fx, &s->fx[4]),
                        NPH_RESULT_OK);
}

static void rt_scene_destroy(RtScene *s)
{
    nph_audio_mixer_destroy(s->m); // first: it closes its streams
    nph_audio_streamer_destroy(s->streamer);
    nph_lossless_encoded_free(&s->stream_enc);
    NPH_FREE(nph_allocator_system(), s->stream_pcm, (u64)RT_FRAMES * 2U * sizeof(i16), 16U);
}

// Makes stream k's clip (again: the last one goes) and plays it.
static void rt_scene_stream(RtScene *s, u32 k)
{
    nph_audio_clip_destroy(s->m, s->streams[k]);
    s->streams[k].value = 0U;
    // The streamer frees closed streams on its own time: its slots may all
    // be taken a moment, and then this one waits for the next turn.
    if (nph_audio_clip_create_stream(s->m, s->streamer, &s->stream_descs[k], &s->streams[k]) !=
        NPH_RESULT_OK) {
        return;
    }
    NphAudioPlayDesc d = nph_audio_play_desc_default(s->streams[k]);
    d.bus = s->music;
    d.volume = 0.3F;
    d.pitch = k == 0U ? 1.0F : 0.83F;
    d.priority = (u8)(test_rng_next(&s->rng) % 256U);
    if (k == 1U) {
        d.spatial = 1;
        d.position = (NphVec3){5.0F, 0.0F, -3.0F};
    }
    NphAudioVoice v;
    if (nph_audio_voice_play(s->m, &d, &v) == NPH_RESULT_OK) {
        s->stream_voices[k] = v;
    }
}

static void rt_scene_play(RtScene *s, u32 i)
{
    NphAudioPlayDesc d = nph_audio_play_desc_default(s->clips[i % 4U]);
    d.bus = (i % 3U) == 0U ? s->music : s->effects;
    d.volume = 0.05F + 0.02F * (f32)(test_rng_next(&s->rng) % 20U);
    d.pitch = 0.5F + 0.1F * (f32)(test_rng_next(&s->rng) % 25U);
    d.pan = test_rng_signed(&s->rng);
    d.priority = (u8)(test_rng_next(&s->rng) % 256U);
    d.loop = (i % 2U) == 0U;
    d.start_frame = test_rng_next(&s->rng) % 10000U;
    d.delay_frames = (u32)(test_rng_next(&s->rng) % 2000U);
    d.lowpass_hz = (i % 5U) == 0U ? 1500.0F : 0.0F;
    d.highpass_hz = (i % 7U) == 0U ? 300.0F : 0.0F;
    d.sends[0] = s->reverb;
    d.send_gains[0] = (i % 4U) == 1U ? 0.4F : 0.0F;
    d.fade_in = (i % 6U) == 0U ? 0.05F : 0.0F;
    // A third are in 3D: placed, moving, near and far.
    if ((i % 3U) == 2U) {
        d.spatial = 1;
        d.position =
            (NphVec3){test_rng_signed(&s->rng) * 20.0F, 0.0F, test_rng_signed(&s->rng) * 20.0F};
        d.velocity = (NphVec3){test_rng_signed(&s->rng) * 10.0F, 0.0F, 0.0F};
        d.size = (i % 5U) == 0U ? 2.0F : 0.0F;
    }
    NphAudioVoice v;
    if (nph_audio_voice_play(s->m, &d, &v) == NPH_RESULT_OK) {
        s->voices[i] = v;
    }
}

// Game-thread work between renders: steer, stop, restart, snapshots.
static void rt_scene_steer(RtScene *s, u32 step)
{
    nph_audio_mixer_update(s->m);
    const u32 i = (u32)(test_rng_next(&s->rng) % 48U);
    if (step % 40U == 7U) {
        rt_scene_stream(s, (step / 40U) % 2U);
    }
    switch (step % 6U) {
    case 0U:
        nph_audio_voice_set_pitch(s->m, s->voices[i], 0.5F + 0.1F * (f32)(step % 30U));
        nph_audio_voice_set_pitch(s->m, s->stream_voices[step % 2U],
                                  0.7F + 0.1F * (f32)(step % 7U));
        break;
    case 1U:
        nph_audio_voice_stop(s->m, s->voices[i], 0.02F);
        break;
    case 2U:
        if (!nph_audio_voice_is_playing(s->m, s->voices[i])) {
            rt_scene_play(s, i);
        }
        break;
    case 3U:
        nph_audio_voice_set_filters(s->m, s->voices[i], (step % 2U) != 0U ? 800.0F : 0.0F, 0.0F);
        // And the 3D voices move, behind walls and round corners.
        nph_audio_voice_set_position(
            s->m, s->voices[(i + 1U) % 48U],
            (NphVec3){test_rng_signed(&s->rng) * 30.0F, 1.0F, test_rng_signed(&s->rng) * 30.0F},
            (NphVec3){test_rng_signed(&s->rng) * 20.0F, 0.0F, test_rng_signed(&s->rng) * 20.0F});
        nph_audio_voice_set_occlusion(s->m, s->voices[(i + 2U) % 48U], (f32)(step % 3U) * 0.5F,
                                      (f32)(step % 2U));
        break;
    case 4U:
        nph_audio_snapshot_set_weight(s->m, s->pause, (step % 12U) < 6U ? 1.0F : 0.0F, 0.1F);
        // The effects move too: the reverberator's size and decay, the
        // equaliser's bell, the echo's time, bypasses.
        nph_audio_effect_set_param(s->m, s->fx[3], NPH_AUDIO_REVERB_SIZE,
                                   0.5F + 0.1F * (f32)(step % 10U));
        nph_audio_effect_set_param(s->m, s->fx[3], NPH_AUDIO_REVERB_DECAY,
                                   0.5F + 0.2F * (f32)(step % 7U));
        nph_audio_effect_set_param(s->m, s->fx[0], NPH_AUDIO_EQ_PARAM(1, NPH_AUDIO_EQ_FREQ),
                                   1000.0F + 300.0F * (f32)(step % 9U));
        nph_audio_effect_set_param(s->m, s->fx[2], NPH_AUDIO_ECHO_DELAY,
                                   200.0F + 30.0F * (f32)(step % 5U));
        nph_audio_effect_set_bypass(s->m, s->fx[1], (step % 18U) < 6U);
        break;
    default:
        nph_audio_voice_set_paused(s->m, s->voices[i], (step % 4U) == 1U);
        nph_audio_voice_set_paused(s->m, s->stream_voices[(step / 6U) % 2U], (step % 4U) == 1U);
        // Now and then an effect goes and another comes in its place.
        if (step % 30U == 5U) {
            nph_audio_effect_destroy(s->m, s->fx[2]);
            const NphAudioEffectDesc echo = nph_audio_effect_desc_default(NPH_AUDIO_EFFECT_ECHO);
            const NphResult r = nph_audio_effect_create(s->m, s->effects, &echo, &s->fx[2]);
            // A destroyed effect keeps its place on its bus until the audio
            // thread lets it go, and a bus holds four. An audio thread that
            // lags (a slow machine, a sanitizer) may still hold the last
            // echoes: the bus is full, and the echo comes back next time.
            // Offline, each render lets them go, so the echo must fit. Not
            // fatal: leaving the test would leave an audio thread behind.
            NTEST_EXPECT_TRUE(r == NPH_RESULT_OK || (s->threaded && r == NPH_RESULT_FULL));
        }
        break;
    }
}

NTEST(audio_mixer_realtime, rendering_allocates_and_locks_nothing)
{
#if NPH_ASSERTS_ENABLED
    NphRealtimeHook previous = NULL;
    void *previous_user = NULL;
    nph_realtime_set_hook(rt_count, NULL, &previous, &previous_user);
#endif
#if TEST_MALLOC_HOOKS
    rt_install_malloc_hooks();
#endif
    nph_atomic_store_u64(&g_rt_violations, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u64(&g_rt_mallocs, 0U, NPH_MEMORY_ORDER_RELAXED);
    RtScene s;
    rt_scene_build(&s);
    for (u32 i = 0U; i < 48U; ++i) {
        rt_scene_play(&s, i);
    }
    rt_scene_stream(&s, 0U);
    rt_scene_stream(&s, 1U);
    // Four seconds in uneven pieces, the game thread working between them.
    f32 *out = test_buffer(2U * 1500U);
    u32 rendered = 0U;
    for (u32 step = 0U; rendered < 4U * 48000U; ++step) {
        const u32 n = 1U + (u32)(test_rng_next(&s.rng) % 1500U);
        nph_audio_mixer_render(s.m, out, n);
        rendered += n;
        rt_scene_steer(&s, step);
    }
    NphAudioMixerStats st;
    nph_audio_mixer_stats(s.m, &st);
    NphAudioStreamerStats ss;
    nph_audio_streamer_stats(s.streamer, &ss);
    NTEST_LOG("%llu blocks; last block %u real, %u virtual; slowest block %.1f us; streams "
              "decoded %llu frames (%llu starved, rendering ahead of real time)",
              (unsigned long long)st.blocks, st.voices_real, st.voices_virtual,
              (f64)st.block_ns_max / 1000.0, (unsigned long long)ss.frames_decoded,
              (unsigned long long)st.stream_starved);
    // Not fatal while the streams are open: their sources are on this frame.
    NTEST_EXPECT_GT_UINT(st.blocks, 700U);
    NTEST_EXPECT_EQ_UINT(st.corrupt_reads, 0U);
    NTEST_EXPECT_EQ_UINT(ss.read_errors, 0U);
    NTEST_EXPECT_EQ_UINT(ss.corrupt_blocks, 0U);
    NTEST_EXPECT_GT_UINT(ss.frames_decoded, 0U);
    test_buffer_free(out, 2U * 1500U);
    rt_scene_destroy(&s);
#if NPH_ASSERTS_ENABLED
    nph_realtime_set_hook(previous, previous_user, NULL, NULL);
#endif
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&g_rt_violations, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&g_rt_mallocs, NPH_MEMORY_ORDER_RELAXED), 0U);
#if !NPH_ASSERTS_ENABLED && !TEST_MALLOC_HOOKS
    NTEST_LOG("no checks in this configuration: the scene only had to render");
#endif
}

#if NPH_ASSERTS_ENABLED
NTEST(audio_mixer_realtime, the_scope_does_see_an_allocation)
{
    // The check above is only worth something if a violation would show.
    NphRealtimeHook previous = NULL;
    void *previous_user = NULL;
    nph_realtime_set_hook(rt_count, NULL, &previous, &previous_user);
    nph_atomic_store_u64(&g_rt_violations, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_realtime_enter();
    void *p = NPH_ALLOC(nph_allocator_system(), 64U, 16U);
    nph_realtime_exit();
    NPH_FREE(nph_allocator_system(), p, 64U, 16U);
    nph_realtime_set_hook(previous, previous_user, NULL, NULL);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&g_rt_violations, NPH_MEMORY_ORDER_RELAXED), 1U);
}
#endif

// An audio thread rendering as a device would, while the game thread plays
// and steers voices, makes and removes clips and buses, and updates.
typedef struct RtThreads {
    NphAudioMixer *m;
    NphAtomicU32 stop;
    NphAtomicU64 frames;
} RtThreads;

static void rt_audio_thread(void *user)
{
    RtThreads *t = (RtThreads *)user;
    f32 out[2U * 480U];
    while (nph_atomic_load_u32(&t->stop, NPH_MEMORY_ORDER_ACQUIRE) == 0U) {
        nph_audio_mixer_render(t->m, out, 480U);
        (void)nph_atomic_fetch_add_u64(&t->frames, 480U, NPH_MEMORY_ORDER_RELAXED);
        nph_thread_yield();
    }
}

NTEST(audio_mixer_realtime, the_game_and_audio_threads_run_together)
{
#if NPH_ASSERTS_ENABLED
    NphRealtimeHook previous = NULL;
    void *previous_user = NULL;
    nph_realtime_set_hook(rt_count, NULL, &previous, &previous_user);
#endif
    nph_atomic_store_u64(&g_rt_violations, 0U, NPH_MEMORY_ORDER_RELAXED);
    RtScene s;
    rt_scene_build(&s);
    s.threaded = true;
    RtThreads t;
    t.m = s.m;
    nph_atomic_store_u32(&t.stop, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u64(&t.frames, 0U, NPH_MEMORY_ORDER_RELAXED);
    // The clip the loop makes and removes (the mixer copies its samples).
    f32 *tone = test_buffer(8000U);
    test_sine(tone, 8000U, 880.0, (f64)TM_RATE, 0.0, 0.3);
    NphAudioClipDesc tone_desc;
    memset(&tone_desc, 0, sizeof tone_desc);
    tone_desc.codec = NPH_AUDIO_CODEC_PCM;
    tone_desc.format = NPH_AUDIO_FORMAT_F32;
    tone_desc.channels = 1U;
    tone_desc.sample_rate = TM_RATE;
    tone_desc.frames = 8000U;
    tone_desc.data = tone;
    tone_desc.size = 8000U * sizeof(f32);
    NphThread audio;
    const NphThreadDesc desc = {rt_audio_thread, &t, "test-audio", 0U};
    NTEST_ASSERT_EQ_INT(nph_thread_create(&audio, &desc), NPH_RESULT_OK);
    const u64 start = nph_time_now_ns();
    u32 step = 0U;
    while (nph_time_now_ns() - start < nph_time_ms_to_ns(600U)) {
        if (step % 50U == 0U) {
            for (u32 i = 0U; i < 48U; ++i) {
                if (!nph_audio_voice_is_playing(s.m, s.voices[i])) {
                    rt_scene_play(&s, i);
                }
            }
            for (u32 k = 0U; k < 2U; ++k) {
                if (!nph_audio_voice_is_playing(s.m, s.stream_voices[k])) {
                    rt_scene_stream(&s, k);
                }
            }
        }
        rt_scene_steer(&s, step);
        if (step % 97U == 13U) {
            // A clip made and removed under the voices that play it. A
            // destroyed clip's slot is free again once the audio thread lets
            // go, and the streams' clips come and go too, so behind a lagging
            // audio thread every slot may be taken a moment. Not fatal: this
            // test must stop its threads before it ends.
            NphAudioClip c;
            const NphResult r = nph_audio_clip_create(s.m, &tone_desc, &c);
            NTEST_EXPECT_TRUE(r == NPH_RESULT_OK || r == NPH_RESULT_FULL);
            if (r == NPH_RESULT_OK) {
                NphAudioPlayDesc d = nph_audio_play_desc_default(c);
                NphAudioVoice v;
                // Every voice may be busy: then nothing plays, which is the
                // test too.
                NPH_IGNORE_RESULT(nph_audio_voice_play(s.m, &d, &v));
                nph_audio_clip_destroy(s.m, c);
            }
            NphAudioBusDesc bd = nph_audio_bus_desc_default();
            NphAudioBus b;
            if (nph_audio_bus_create(s.m, &bd, &b) == NPH_RESULT_OK) {
                nph_audio_bus_destroy(s.m, b);
            }
        }
        ++step;
        nph_thread_sleep_ns(nph_time_us_to_ns(500U));
    }
    // Stop everything and wait for the audio thread to say it is done.
    for (u32 i = 0U; i < 48U; ++i) {
        nph_audio_voice_stop(s.m, s.voices[i], 0.0F);
    }
    for (u32 k = 0U; k < 2U; ++k) {
        nph_audio_voice_stop(s.m, s.stream_voices[k], 0.0F);
    }
    for (u32 tries = 0U; tries < 2000U; ++tries) {
        nph_audio_mixer_update(s.m);
        NphAudioMixerStats st;
        nph_audio_mixer_stats(s.m, &st);
        bool any = false;
        for (u32 i = 0U; i < 48U; ++i) {
            any = any || nph_audio_voice_is_playing(s.m, s.voices[i]);
        }
        if (!any && st.voices_playing == 0U) {
            break;
        }
        nph_thread_sleep_ns(nph_time_ms_to_ns(1U));
    }
    nph_atomic_store_u32(&t.stop, 1U, NPH_MEMORY_ORDER_RELEASE);
    nph_thread_join(&audio);
    nph_audio_mixer_update(s.m);
    NTEST_LOG("%u game steps; %llu frames rendered", step,
              (unsigned long long)nph_atomic_load_u64(&t.frames, NPH_MEMORY_ORDER_RELAXED));
    // Not fatal while the streams are open: their sources are on this frame.
    NTEST_EXPECT_GT_UINT(nph_atomic_load_u64(&t.frames, NPH_MEMORY_ORDER_RELAXED), 4800U);
    for (u32 i = 0U; i < 48U; ++i) {
        NTEST_EXPECT_FALSE(nph_audio_voice_is_playing(s.m, s.voices[i]));
    }
    rt_scene_destroy(&s);
    test_buffer_free(tone, 8000U);
#if NPH_ASSERTS_ENABLED
    nph_realtime_set_hook(previous, previous_user, NULL, NULL);
#endif
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&g_rt_violations, NPH_MEMORY_ORDER_RELAXED), 0U);
}
