// SPDX-License-Identifier: Apache-2.0
// The output device (device.h): the null device renders the mixer from its
// own thread, in real time and inside the real-time rules (a violation
// would fail an assertion on that thread); the system's device, where the
// machine has one, opens, plays silence, and closes.

#include <nepheline/audio/device.h>
#include <nepheline/core/realtime.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "test_mixer_support.h"

// Runs the device for `ms` milliseconds of wall time, updating the mixer
// as a game thread would.
static void td_run(NphAudioDevice *dev, NphAudioMixer *m, u32 ms)
{
    NTEST_ASSERT_EQ_INT(nph_audio_device_start(dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_audio_device_start(dev), NPH_RESULT_OK); // started already: fine
    const u64 t0 = nph_time_now_ns();
    while (nph_time_now_ns() - t0 < nph_time_ms_to_ns(ms)) {
        nph_audio_mixer_update(m);
        nph_thread_sleep_ns(nph_time_ms_to_ns(5U));
    }
    nph_audio_device_stop(dev);
    nph_audio_device_stop(dev); // stopped already: fine
}

NTEST(audio_device, the_null_device_renders_the_mixer_in_real_time)
{
    NphAudioDeviceInfo info;
    NTEST_ASSERT_EQ_INT(
        nph_audio_device_query(nph_allocator_system(), NPH_AUDIO_BACKEND_NULL, &info),
        NPH_RESULT_OK);
    NTEST_LOG("null device \"%s\": %u Hz, %u channels", info.name, info.sample_rate, info.channels);
    NphAudioMixer *m = tm_mixer(2U, 8U, 8U);
    const NphAudioClip clip = tm_tone(m, 440.0, 96000U, TM_RATE, 0.3F, NULL);
    NphAudioPlayDesc d = nph_audio_play_desc_default(clip);
    d.loop = 1;
    (void)tm_play(m, &d);
    NphAudioDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.mixer = m;
    dd.backend = NPH_AUDIO_BACKEND_NULL;
    NphAudioDevice *dev = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_device_open(&dd, &dev), NPH_RESULT_OK);
    const u64 violations = nph_realtime_violations();
    td_run(dev, m, 400U);
    NphAudioDeviceStats s;
    nph_audio_device_stats(dev, &s);
    NphAudioMixerStats ms;
    nph_audio_mixer_stats(m, &ms);
    NTEST_LOG("%llu callbacks, %llu frames (period %u), slowest %.1f us; %llu blocks mixed",
              (unsigned long long)s.callbacks, (unsigned long long)s.frames, s.period_frames,
              (f64)s.longest_ns / 1000.0, (unsigned long long)ms.blocks);
    NTEST_ASSERT_EQ_INT(s.backend, NPH_AUDIO_BACKEND_NULL);
    NTEST_ASSERT_EQ_UINT(s.sample_rate, TM_RATE);
    NTEST_ASSERT_EQ_UINT(s.channels, 2U);
    // No hardware, so nothing converts at the edge.
    NTEST_EXPECT_EQ_UINT(s.device_sample_rate, TM_RATE);
    NTEST_EXPECT_EQ_UINT(s.device_channels, 2U);
    NTEST_ASSERT_TRUE(s.callbacks > 0U);
    // The null device keeps time: most of 400 ms of frames, and no more.
    NTEST_ASSERT_TRUE(s.frames > (u64)TM_RATE / 5U);
    NTEST_ASSERT_TRUE(s.frames < (u64)TM_RATE);
    NTEST_ASSERT_TRUE(ms.blocks * NPH_AUDIO_BLOCK_FRAMES >= s.frames);
    NTEST_ASSERT_EQ_UINT(nph_realtime_violations(), violations);
    nph_audio_device_close(dev);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_device, the_system_device_plays_silence_where_there_is_one)
{
    NphAudioDeviceInfo info;
    const NphResult q =
        nph_audio_device_query(nph_allocator_system(), NPH_AUDIO_BACKEND_SYSTEM, &info);
    if (q == NPH_RESULT_UNSUPPORTED) {
        NTEST_SKIP("no audio output device on this machine");
    }
    NTEST_ASSERT_EQ_INT(q, NPH_RESULT_OK);
    NTEST_LOG("system device \"%s\": %u Hz, %u channels", info.name, info.sample_rate,
              info.channels);
    // The mixer at the device's own format, with nothing playing.
    NphAudioMixerDesc md;
    memset(&md, 0, sizeof md);
    md.alloc = nph_allocator_system();
    md.sample_rate = info.sample_rate;
    md.channels = info.channels;
    md.max_voices = 4U;
    md.max_real_voices = 4U;
    NphAudioMixer *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_mixer_create(&md, &m), NPH_RESULT_OK);
    NphAudioDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.mixer = m;
    dd.backend = NPH_AUDIO_BACKEND_SYSTEM;
    NphAudioDevice *dev = NULL;
    const NphResult r = nph_audio_device_open(&dd, &dev);
    if (r == NPH_RESULT_UNSUPPORTED) {
        nph_audio_mixer_destroy(m);
        NTEST_SKIP("the audio output device would not open");
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    td_run(dev, m, 150U);
    NphAudioDeviceStats s;
    nph_audio_device_stats(dev, &s);
    NTEST_LOG("%llu callbacks of %u frames; the hardware at %u Hz, %u channels",
              (unsigned long long)s.callbacks, s.period_frames, s.device_sample_rate,
              s.device_channels);
    NTEST_EXPECT_TRUE(s.callbacks > 0U);
    NTEST_EXPECT_EQ_UINT(s.sample_rate, info.sample_rate);
    // Made at the device's own format, the mix reaches it unconverted.
    NTEST_EXPECT_EQ_UINT(s.device_sample_rate, info.sample_rate);
    NTEST_EXPECT_EQ_UINT(s.device_channels, info.channels);
    nph_audio_device_close(dev);
    nph_audio_mixer_destroy(m);
}

NTEST(audio_device, refusals)
{
    NphAudioDeviceInfo info;
    NTEST_ASSERT_EQ_INT(nph_audio_device_query(NULL, NPH_AUDIO_BACKEND_NULL, &info),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(
        nph_audio_device_query(nph_allocator_system(), NPH_AUDIO_BACKEND_COUNT, &info),
        NPH_RESULT_INVALID_ARGUMENT);
    NphAudioDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    NphAudioDevice *dev = NULL;
    NTEST_ASSERT_EQ_INT(nph_audio_device_open(&dd, &dev), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(dev);
    nph_audio_device_close(NULL);
}
