// SPDX-License-Identifier: Apache-2.0
// The module's log category, sample formats, layouts, and levels (audio.h).

#include <nepheline/audio/audio.h>

#include <math.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_AUDIO, "audio", NPH_LOG_LEVEL_INFO);

u32 nph_audio_format_bytes(NphAudioSampleFormat format)
{
    switch (format) {
    case NPH_AUDIO_FORMAT_S16:
        return 2U;
    case NPH_AUDIO_FORMAT_S24:
        return 3U;
    case NPH_AUDIO_FORMAT_S32:
    case NPH_AUDIO_FORMAT_F32:
        return 4U;
    case NPH_AUDIO_FORMAT_UNKNOWN:
    case NPH_AUDIO_FORMAT_COUNT:
        break;
    }
    return 0U;
}

const char *nph_audio_format_name(NphAudioSampleFormat format)
{
    switch (format) {
    case NPH_AUDIO_FORMAT_S16:
        return "s16";
    case NPH_AUDIO_FORMAT_S24:
        return "s24";
    case NPH_AUDIO_FORMAT_S32:
        return "s32";
    case NPH_AUDIO_FORMAT_F32:
        return "f32";
    case NPH_AUDIO_FORMAT_UNKNOWN:
    case NPH_AUDIO_FORMAT_COUNT:
        break;
    }
    return "unknown";
}

#define AUDIO_DEG(d) ((f32)((d) * 3.14159265358979323846 / 180.0))

// Speaker angles: ITU-R BS.775-3 for stereo and 5.1 (fronts at 30 degrees,
// surrounds at 110), the Dolby 7.1 arrangement (sides at 90, backs at 150),
// and the square for quad.
static const NphAudioLayoutInfo g_audio_layouts[NPH_AUDIO_LAYOUT_COUNT] = {
    {"mono", 1U, NPH_AUDIO_NO_LFE, {0.0F}},
    {"stereo", 2U, NPH_AUDIO_NO_LFE, {AUDIO_DEG(-30.0), AUDIO_DEG(30.0)}},
    {"quad",
     4U,
     NPH_AUDIO_NO_LFE,
     {AUDIO_DEG(-45.0), AUDIO_DEG(45.0), AUDIO_DEG(-135.0), AUDIO_DEG(135.0)}},
    {"5.1",
     6U,
     3U,
     {AUDIO_DEG(-30.0), AUDIO_DEG(30.0), 0.0F, 0.0F, AUDIO_DEG(-110.0), AUDIO_DEG(110.0)}},
    {"7.1",
     8U,
     3U,
     {AUDIO_DEG(-30.0), AUDIO_DEG(30.0), 0.0F, 0.0F, AUDIO_DEG(-150.0), AUDIO_DEG(150.0),
      AUDIO_DEG(-90.0), AUDIO_DEG(90.0)}},
};

const NphAudioLayoutInfo *nph_audio_layout_info(NphAudioLayout layout)
{
    if ((u32)layout >= (u32)NPH_AUDIO_LAYOUT_COUNT) {
        return NULL;
    }
    return &g_audio_layouts[layout];
}

NphResult nph_audio_layout_for_channels(u32 channels, NphAudioLayout *out)
{
    for (u32 i = 0U; i < (u32)NPH_AUDIO_LAYOUT_COUNT; ++i) {
        if (g_audio_layouts[i].channels == channels) {
            *out = (NphAudioLayout)i;
            return NPH_RESULT_OK;
        }
    }
    return NPH_RESULT_UNSUPPORTED;
}

f32 nph_audio_db_to_gain(f32 db)
{
    if (!(db > NPH_AUDIO_SILENCE_DB)) {
        return 0.0F;
    }
    return powf(10.0F, db / 20.0F);
}

f32 nph_audio_gain_to_db(f32 gain)
{
    // 10^(-144/20): the gain of the silence level.
    if (!(gain > 6.3095734e-8F)) {
        return NPH_AUDIO_SILENCE_DB;
    }
    return 20.0F * log10f(gain);
}
