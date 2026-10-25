// SPDX-License-Identifier: Apache-2.0
// The output device (device.h): miniaudio's context and playback device,
// their memory from our allocator, and a callback that renders the mixer.

#include <nepheline/audio/device.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/atomic.h>
#include <nepheline/platform/time.h>

#include <miniaudio.h>
#include <string.h>

struct NphAudioDevice {
    NphAllocator *alloc;
    NphAudioMixer *mixer;
    NphAudioBackend backend;
    ma_allocation_callbacks callbacks;
    ma_context context;
    ma_device device;
    b32 context_ready;
    b32 device_ready;
    b32 started;
    NphAtomicU64 stat_callbacks;
    NphAtomicU64 stat_frames;
    NphAtomicU64 stat_longest;
};

// ---------------------------------------------------------------------------
// miniaudio's memory
// ---------------------------------------------------------------------------

// miniaudio frees without a size, which our allocators need: each block
// carries its size in a 16-byte header.
#define DEV_HEADER 16U

static void *dev_malloc(size_t size, void *user)
{
    NphAllocator *a = (NphAllocator *)user;
    u8 *p = (u8 *)NPH_ALLOC(a, (u64)size + DEV_HEADER, 16U);
    if (p == NULL) {
        return NULL;
    }
    const u64 n = (u64)size;
    memcpy(p, &n, sizeof n);
    return p + DEV_HEADER;
}

static void dev_free(void *ptr, void *user)
{
    if (ptr == NULL) {
        return;
    }
    u8 *p = (u8 *)ptr - DEV_HEADER;
    u64 n;
    memcpy(&n, p, sizeof n);
    NPH_FREE((NphAllocator *)user, p, n + DEV_HEADER, 16U);
}

static void *dev_realloc(void *ptr, size_t size, void *user)
{
    if (ptr == NULL) {
        return dev_malloc(size, user);
    }
    if (size == 0U) {
        dev_free(ptr, user);
        return NULL;
    }
    void *fresh = dev_malloc(size, user);
    if (fresh == NULL) {
        return NULL;
    }
    u64 old;
    memcpy(&old, (u8 *)ptr - DEV_HEADER, sizeof old);
    memcpy(fresh, ptr, (size_t)(old < (u64)size ? old : (u64)size));
    dev_free(ptr, user);
    return fresh;
}

// ---------------------------------------------------------------------------
// Contexts
// ---------------------------------------------------------------------------

static NphResult dev_context(NphAllocator *alloc, NphAudioBackend backend,
                             ma_allocation_callbacks *callbacks, ma_context *out)
{
    callbacks->pUserData = alloc;
    callbacks->onMalloc = dev_malloc;
    callbacks->onRealloc = dev_realloc;
    callbacks->onFree = dev_free;
    ma_context_config cfg = ma_context_config_init();
    cfg.allocationCallbacks = *callbacks;
#if defined(_WIN32)
    static const ma_backend system_backends[] = {ma_backend_wasapi};
#else
    static const ma_backend system_backends[] = {ma_backend_pulseaudio, ma_backend_alsa};
#endif
    static const ma_backend null_backends[] = {ma_backend_null};
    const bool null = backend == NPH_AUDIO_BACKEND_NULL;
    const ma_result r = ma_context_init(
        null ? null_backends : system_backends,
        null ? 1U : (ma_uint32)(sizeof system_backends / sizeof system_backends[0]), &cfg, out);
    return r == MA_SUCCESS ? NPH_RESULT_OK : NPH_RESULT_UNSUPPORTED;
}

// The mixer's layouts' channel counts: the nearest at or below (mono's
// below stereo's above).
static u32 dev_layout_channels(u32 channels)
{
    if (channels <= 1U) {
        return 1U;
    }
    if (channels < 4U) {
        return 2U;
    }
    if (channels < 6U) {
        return 4U;
    }
    return channels < 8U ? 6U : 8U;
}

NphResult nph_audio_device_query(NphAllocator *alloc, NphAudioBackend backend,
                                 NphAudioDeviceInfo *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    if (alloc == NULL || (u32)backend >= (u32)NPH_AUDIO_BACKEND_COUNT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    ma_allocation_callbacks callbacks;
    ma_context *context = (ma_context *)dev_malloc(sizeof(ma_context), alloc);
    if (context == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphResult r = dev_context(alloc, backend, &callbacks, context);
    if (r != NPH_RESULT_OK) {
        dev_free(context, alloc);
        return r;
    }
    ma_device_info info;
    if (ma_context_get_device_info(context, ma_device_type_playback, NULL, &info) != MA_SUCCESS) {
        r = NPH_RESULT_UNSUPPORTED;
    } else {
        // The device's own format: its first native one (a rate of 0, or
        // no native format at all, means any: the mix's default).
        u32 rate = 0U;
        u32 channels = 0U;
        if (info.nativeDataFormatCount > 0U) {
            rate = info.nativeDataFormats[0].sampleRate;
            channels = info.nativeDataFormats[0].channels;
        }
        out->sample_rate = rate >= NPH_AUDIO_MIN_RATE && rate <= NPH_AUDIO_MAX_RATE ? rate : 48000U;
        out->channels = dev_layout_channels(channels > 0U ? channels : 2U);
        const size_t n = strlen(info.name);
        const size_t keep = n < sizeof out->name - 1U ? n : sizeof out->name - 1U;
        memcpy(out->name, info.name, keep);
        out->name[keep] = '\0';
    }
    ma_context_uninit(context);
    dev_free(context, alloc);
    return r;
}

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

// The system's audio thread: one render of the frames it asks for.
static void dev_callback(ma_device *ma, void *output, const void *input, ma_uint32 frames)
{
    NPH_UNUSED(input);
    NphAudioDevice *d = (NphAudioDevice *)ma->pUserData;
    const u64 t0 = nph_time_now_ns();
    nph_audio_mixer_render(d->mixer, (f32 *)output, (u32)frames);
    const u64 ns = nph_time_now_ns() - t0;
    (void)nph_atomic_fetch_add_u64(&d->stat_callbacks, 1U, NPH_MEMORY_ORDER_RELAXED);
    (void)nph_atomic_fetch_add_u64(&d->stat_frames, frames, NPH_MEMORY_ORDER_RELAXED);
    if (ns > nph_atomic_load_u64(&d->stat_longest, NPH_MEMORY_ORDER_RELAXED)) {
        nph_atomic_store_u64(&d->stat_longest, ns, NPH_MEMORY_ORDER_RELAXED);
    }
}

static ma_channel dev_channel(NphAudioSpeaker speaker, u32 channels)
{
    if (channels == 1U) {
        return MA_CHANNEL_MONO;
    }
    switch (speaker) {
    case NPH_AUDIO_SPEAKER_FRONT_LEFT:
        return MA_CHANNEL_FRONT_LEFT;
    case NPH_AUDIO_SPEAKER_FRONT_RIGHT:
        return MA_CHANNEL_FRONT_RIGHT;
    case NPH_AUDIO_SPEAKER_FRONT_CENTER:
        return MA_CHANNEL_FRONT_CENTER;
    case NPH_AUDIO_SPEAKER_LOW_FREQUENCY:
        return MA_CHANNEL_LFE;
    case NPH_AUDIO_SPEAKER_BACK_LEFT:
        return MA_CHANNEL_BACK_LEFT;
    case NPH_AUDIO_SPEAKER_BACK_RIGHT:
        return MA_CHANNEL_BACK_RIGHT;
    case NPH_AUDIO_SPEAKER_SIDE_LEFT:
        return MA_CHANNEL_SIDE_LEFT;
    case NPH_AUDIO_SPEAKER_SIDE_RIGHT:
        return MA_CHANNEL_SIDE_RIGHT;
    case NPH_AUDIO_SPEAKER_COUNT:
        break;
    }
    return MA_CHANNEL_NONE;
}

NphResult nph_audio_device_open(const NphAudioDeviceDesc *desc, NphAudioDevice **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL || desc->mixer == NULL ||
        (u32)desc->backend >= (u32)NPH_AUDIO_BACKEND_COUNT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAudioDevice *d = NPH_ALLOC_TYPE(desc->alloc, NphAudioDevice);
    if (d == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(d, 0, sizeof *d);
    d->alloc = desc->alloc;
    d->mixer = desc->mixer;
    d->backend = desc->backend;
    NphResult r = dev_context(desc->alloc, desc->backend, &d->callbacks, &d->context);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(desc->alloc, NphAudioDevice, d);
        return r;
    }
    d->context_ready = 1;
    const u32 channels = nph_audio_mixer_channels(desc->mixer);
    NphAudioLayout layout;
    NPH_VERIFY(nph_audio_layout_for_channels(channels, &layout) == NPH_RESULT_OK);
    const NphAudioLayoutInfo *info = nph_audio_layout_info(layout);
    ma_channel map[NPH_AUDIO_MAX_CHANNELS];
    for (u32 c = 0U; c < channels; ++c) {
        map[c] = dev_channel(info->speaker[c], channels);
    }
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = channels;
    cfg.playback.pChannelMap = map;
    cfg.sampleRate = nph_audio_mixer_sample_rate(desc->mixer);
    cfg.periodSizeInFrames = desc->period_frames != 0U ? desc->period_frames : 256U;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.dataCallback = dev_callback;
    cfg.pUserData = d;
    // A conversion at the edge, if the hardware forces one: miniaudio's
    // best (WASAPI converts with its own unless asked not to).
    cfg.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    // The scheduler's class for audio on short deadlines.
    cfg.wasapi.usage = ma_wasapi_usage_pro_audio;
    // The mixer writes every frame it is given, clamped: nothing for
    // miniaudio to clear, clip, or regroup into fixed sizes.
    cfg.noPreSilencedOutputBuffer = MA_TRUE;
    cfg.noClip = MA_TRUE;
    cfg.noFixedSizedCallback = MA_TRUE;
    if (ma_device_init(&d->context, &cfg, &d->device) != MA_SUCCESS) {
        nph_audio_device_close(d);
        return NPH_RESULT_UNSUPPORTED;
    }
    d->device_ready = 1;
    const u32 rate = d->device.playback.internalSampleRate;
    const u32 hw_channels = d->device.playback.internalChannels;
    NPH_LOG_INFO(&NPH_LOG_AUDIO, "device \"%s\" (%s): %u Hz, %u channels, %u-frame periods",
                 d->device.playback.name,
                 desc->backend == NPH_AUDIO_BACKEND_NULL ? "null" : "system", rate, hw_channels,
                 d->device.playback.internalPeriodSizeInFrames);
    if (rate != cfg.sampleRate || hw_channels != channels) {
        NPH_LOG_WARN(&NPH_LOG_AUDIO,
                     "the mix (%u Hz, %u channels) is converted to the device's at the edge: "
                     "make the mixer at the device's format",
                     cfg.sampleRate, channels);
    }
    *out = d;
    return NPH_RESULT_OK;
}

void nph_audio_device_close(NphAudioDevice *device)
{
    if (device == NULL) {
        return;
    }
    NphAudioDevice *d = device;
    if (d->device_ready) {
        nph_audio_device_stop(d);
        ma_device_uninit(&d->device);
    }
    if (d->context_ready) {
        ma_context_uninit(&d->context);
    }
    NPH_FREE_TYPE(d->alloc, NphAudioDevice, d);
}

NphResult nph_audio_device_start(NphAudioDevice *device)
{
    NPH_ASSERT(device != NULL);
    if (device->started) {
        return NPH_RESULT_OK;
    }
    if (ma_device_start(&device->device) != MA_SUCCESS) {
        return NPH_RESULT_UNSUPPORTED;
    }
    device->started = 1;
    return NPH_RESULT_OK;
}

void nph_audio_device_stop(NphAudioDevice *device)
{
    NPH_ASSERT(device != NULL);
    if (!device->started) {
        return;
    }
    (void)ma_device_stop(&device->device);
    device->started = 0;
}

void nph_audio_device_stats(const NphAudioDevice *device, NphAudioDeviceStats *out)
{
    NPH_ASSERT(device != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->backend = device->backend;
    out->sample_rate = device->device.sampleRate;
    out->channels = device->device.playback.channels;
    out->device_sample_rate = device->device.playback.internalSampleRate;
    out->device_channels = device->device.playback.internalChannels;
    out->period_frames = device->device.playback.internalPeriodSizeInFrames;
    out->callbacks = nph_atomic_load_u64(&device->stat_callbacks, NPH_MEMORY_ORDER_RELAXED);
    out->frames = nph_atomic_load_u64(&device->stat_frames, NPH_MEMORY_ORDER_RELAXED);
    out->longest_ns = nph_atomic_load_u64(&device->stat_longest, NPH_MEMORY_ORDER_RELAXED);
}
