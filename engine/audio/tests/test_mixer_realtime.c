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

// A scene that uses everything a block can do: every codec, loops, pitch,
// filters, sends, delays, buses, ducking, snapshots, and more voices than
// real ones, so voices go virtual and come back all the time.
typedef struct RtScene {
    NphAudioMixer *m;
    NphAudioClip clips[4];
    NphAudioBus music;
    NphAudioBus effects;
    NphAudioBus reverb;
    NphAudioSnapshot pause;
    NphAudioVoice voices[48];
    u64 rng;
} RtScene;

static void rt_scene_build(RtScene *s)
{
    memset(s, 0, sizeof *s);
    s->rng = 99U;
    s->m = tm_mixer(2U, 64U, 12U);
    enum {
        FRAMES = 20000
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
    nph_lossless_encoded_free(&enc);
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
    NPH_FREE(nph_allocator_system(), pcm, (u64)FRAMES * 2U * sizeof(i16), 16U);
    NPH_FREE(nph_allocator_system(), ints, (u64)FRAMES * 2U * sizeof(i32), 16U);

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
    switch (step % 6U) {
    case 0U:
        nph_audio_voice_set_pitch(s->m, s->voices[i], 0.5F + 0.1F * (f32)(step % 30U));
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
        break;
    case 4U:
        nph_audio_snapshot_set_weight(s->m, s->pause, (step % 12U) < 6U ? 1.0F : 0.0F, 0.1F);
        break;
    default:
        nph_audio_voice_set_paused(s->m, s->voices[i], (step % 4U) == 1U);
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
    NTEST_LOG("%llu blocks; last block %u real, %u virtual; slowest block %.1f us",
              (unsigned long long)st.blocks, st.voices_real, st.voices_virtual,
              (f64)st.block_ns_max / 1000.0);
    NTEST_ASSERT_GT_UINT(st.blocks, 700U);
    NTEST_ASSERT_EQ_UINT(st.corrupt_reads, 0U);
    test_buffer_free(out, 2U * 1500U);
    nph_audio_mixer_destroy(s.m);
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
    RtThreads t;
    t.m = s.m;
    nph_atomic_store_u32(&t.stop, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u64(&t.frames, 0U, NPH_MEMORY_ORDER_RELAXED);
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
        }
        rt_scene_steer(&s, step);
        if (step % 97U == 13U) {
            // A clip made and removed under the voices that play it.
            const NphAudioClip c = tm_tone(s.m, 880.0, 8000U, TM_RATE, 0.3F, NULL);
            NphAudioPlayDesc d = nph_audio_play_desc_default(c);
            NphAudioVoice v;
            // Every slot may be busy: then nothing plays, which is the test too.
            NPH_IGNORE_RESULT(nph_audio_voice_play(s.m, &d, &v));
            nph_audio_clip_destroy(s.m, c);
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
    NTEST_ASSERT_GT_UINT(nph_atomic_load_u64(&t.frames, NPH_MEMORY_ORDER_RELAXED), 4800U);
    for (u32 i = 0U; i < 48U; ++i) {
        NTEST_ASSERT_FALSE(nph_audio_voice_is_playing(s.m, s.voices[i]));
    }
    nph_audio_mixer_destroy(s.m);
#if NPH_ASSERTS_ENABLED
    nph_realtime_set_hook(previous, previous_user, NULL, NULL);
#endif
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&g_rt_violations, NPH_MEMORY_ORDER_RELAXED), 0U);
}
