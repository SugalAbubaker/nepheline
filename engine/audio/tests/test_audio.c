// SPDX-License-Identifier: Apache-2.0
// The module's shared definitions (audio.h): formats, layouts, levels.

#include <nepheline/audio/audio.h>

#include "test_audio_support.h"

NTEST(audio, format_sizes_and_names)
{
    NTEST_ASSERT_EQ_UINT(nph_audio_format_bytes(NPH_AUDIO_FORMAT_S16), 2U);
    NTEST_ASSERT_EQ_UINT(nph_audio_format_bytes(NPH_AUDIO_FORMAT_S24), 3U);
    NTEST_ASSERT_EQ_UINT(nph_audio_format_bytes(NPH_AUDIO_FORMAT_S32), 4U);
    NTEST_ASSERT_EQ_UINT(nph_audio_format_bytes(NPH_AUDIO_FORMAT_F32), 4U);
    NTEST_ASSERT_EQ_UINT(nph_audio_format_bytes(NPH_AUDIO_FORMAT_UNKNOWN), 0U);
    NTEST_ASSERT_EQ_UINT(nph_audio_format_bytes(NPH_AUDIO_FORMAT_COUNT), 0U);
    NTEST_ASSERT_EQ_STR(nph_audio_format_name(NPH_AUDIO_FORMAT_S24), "s24");
    NTEST_ASSERT_EQ_STR(nph_audio_format_name(NPH_AUDIO_FORMAT_F32), "f32");
    NTEST_ASSERT_EQ_STR(nph_audio_format_name(NPH_AUDIO_FORMAT_COUNT), "unknown");
}

NTEST(audio, layouts_are_the_wave_orders)
{
    const u32 channels[NPH_AUDIO_LAYOUT_COUNT] = {1U, 2U, 4U, 6U, 8U};
    for (u32 l = 0U; l < (u32)NPH_AUDIO_LAYOUT_COUNT; ++l) {
        const NphAudioLayoutInfo *info = nph_audio_layout_info((NphAudioLayout)l);
        NTEST_ASSERT_NOT_NULL(info);
        NTEST_ASSERT_EQ_UINT(info->channels, channels[l]);
        NphAudioLayout back;
        NTEST_ASSERT_EQ_INT(nph_audio_layout_for_channels(channels[l], &back), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(back, l);
    }
    NTEST_ASSERT_NULL(nph_audio_layout_info(NPH_AUDIO_LAYOUT_COUNT));
    NphAudioLayout unused;
    NTEST_ASSERT_EQ_INT(nph_audio_layout_for_channels(3U, &unused), NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_EQ_INT(nph_audio_layout_for_channels(0U, &unused), NPH_RESULT_UNSUPPORTED);

    const NphAudioLayoutInfo *stereo = nph_audio_layout_info(NPH_AUDIO_LAYOUT_STEREO);
    NTEST_ASSERT_TRUE(stereo->azimuth[0] < 0.0F); // left is negative
    NTEST_ASSERT_TRUE(stereo->azimuth[1] > 0.0F);
    NTEST_ASSERT_EQ_UINT(stereo->lfe, NPH_AUDIO_NO_LFE);
    const NphAudioLayoutInfo *s51 = nph_audio_layout_info(NPH_AUDIO_LAYOUT_SURROUND_5_1);
    NTEST_ASSERT_EQ_UINT(s51->lfe, 3U);
    NTEST_ASSERT_NEAR_F32(s51->azimuth[4], -1.9198622F, 1e-6F); // -110 degrees
    const NphAudioLayoutInfo *s71 = nph_audio_layout_info(NPH_AUDIO_LAYOUT_SURROUND_7_1);
    NTEST_ASSERT_EQ_UINT(s71->lfe, 3U);
    NTEST_ASSERT_NEAR_F32(s71->azimuth[6], -1.5707964F, 1e-6F); // side left, -90 degrees
}

NTEST(audio, decibels)
{
    NTEST_ASSERT_NEAR_F32(nph_audio_db_to_gain(0.0F), 1.0F, 1e-7F);
    NTEST_ASSERT_NEAR_F32(nph_audio_db_to_gain(-6.0205999F), 0.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(nph_audio_db_to_gain(20.0F), 10.0F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(nph_audio_gain_to_db(0.1F), -20.0F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(nph_audio_gain_to_db(2.0F), 6.0206F, 1e-4F);
    // Silence, both ways, and NaN as silence.
    NTEST_ASSERT_TRUE(nph_audio_db_to_gain(NPH_AUDIO_SILENCE_DB) == 0.0F);
    NTEST_ASSERT_TRUE(nph_audio_db_to_gain(-1000.0F) == 0.0F);
    NTEST_ASSERT_TRUE(nph_audio_db_to_gain(NAN) == 0.0F);
    NTEST_ASSERT_TRUE(nph_audio_gain_to_db(0.0F) == NPH_AUDIO_SILENCE_DB);
    NTEST_ASSERT_TRUE(nph_audio_gain_to_db(-1.0F) == NPH_AUDIO_SILENCE_DB);
    NTEST_ASSERT_TRUE(nph_audio_gain_to_db(NAN) == NPH_AUDIO_SILENCE_DB);
    // Round trips over the range a mixer uses.
    for (i32 half_db = -240; half_db <= 48; ++half_db) {
        const f32 db = 0.5F * (f32)half_db;
        NTEST_ASSERT_NEAR_F32(nph_audio_gain_to_db(nph_audio_db_to_gain(db)), db, 2e-4F);
    }
}
