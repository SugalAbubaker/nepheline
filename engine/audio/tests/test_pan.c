// SPDX-License-Identifier: Apache-2.0
// Panning (pan.h): constant power everywhere, speakers hit exactly, at
// most two speakers per point source, continuity around the circle, and
// spread.

#include <nepheline/audio/pan.h>

#include "test_audio_support.h"

static f32 deg(f32 d)
{
    return d * 3.14159265F / 180.0F;
}

static f32 power(const f32 *g, u32 n)
{
    f32 p = 0.0F;
    for (u32 c = 0U; c < n; ++c) {
        p += g[c] * g[c];
    }
    return p;
}

NTEST(audio_pan, the_stereo_law_is_equal_power)
{
    f32 l = 0.0F;
    f32 r = 0.0F;
    nph_audio_pan_stereo(0.0F, &l, &r);
    NTEST_ASSERT_NEAR_F32(l, 0.70710678F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(r, 0.70710678F, 1e-6F);
    nph_audio_pan_stereo(-1.0F, &l, &r);
    NTEST_ASSERT_NEAR_F32(l, 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(r, 0.0F, 1e-6F);
    nph_audio_pan_stereo(7.0F, &l, &r); // clamped
    NTEST_ASSERT_NEAR_F32(r, 1.0F, 1e-6F);
    nph_audio_pan_stereo(NAN, &l, &r);
    NTEST_ASSERT_NEAR_F32(l, 1.0F, 1e-6F);
    for (i32 i = -100; i <= 100; ++i) {
        nph_audio_pan_stereo((f32)i / 100.0F, &l, &r);
        NTEST_ASSERT_NEAR_F32(l * l + r * r, 1.0F, 2e-6F);
    }
}

NTEST(audio_pan, stereo_places_by_the_lateral_position)
{
    NphAudioPanner p;
    NTEST_ASSERT_EQ_INT(nph_audio_panner_init(&p, NPH_AUDIO_LAYOUT_STEREO), NPH_RESULT_OK);
    f32 g[NPH_AUDIO_MAX_CHANNELS];
    nph_audio_pan(&p, 0.0F, 0.0F, g);
    NTEST_ASSERT_NEAR_F32(g[0], g[1], 1e-6F);
    nph_audio_pan(&p, deg(90.0F), 0.0F, g);
    NTEST_ASSERT_NEAR_F32(g[0], 0.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(g[1], 1.0F, 1e-6F);
    nph_audio_pan(&p, deg(-90.0F), 0.0F, g);
    NTEST_ASSERT_NEAR_F32(g[0], 1.0F, 1e-6F);
    // Behind folds onto in front.
    f32 front[NPH_AUDIO_MAX_CHANNELS];
    nph_audio_pan(&p, deg(30.0F), 0.0F, front);
    nph_audio_pan(&p, deg(150.0F), 0.0F, g);
    NTEST_ASSERT_NEAR_F32(g[0], front[0], 1e-5F);
    NTEST_ASSERT_NEAR_F32(g[1], front[1], 1e-5F);
    // A full spread is centred.
    nph_audio_pan(&p, deg(80.0F), 6.2831853F, g);
    NTEST_ASSERT_NEAR_F32(g[0], g[1], 1e-5F);
}

NTEST(audio_pan, mono_takes_everything)
{
    NphAudioPanner p;
    NTEST_ASSERT_EQ_INT(nph_audio_panner_init(&p, NPH_AUDIO_LAYOUT_MONO), NPH_RESULT_OK);
    f32 g[NPH_AUDIO_MAX_CHANNELS];
    nph_audio_pan(&p, deg(123.0F), 0.5F, g);
    NTEST_ASSERT_NEAR_F32(g[0], 1.0F, 1e-6F);
    NTEST_ASSERT_EQ_INT(nph_audio_panner_init(&p, NPH_AUDIO_LAYOUT_COUNT),
                        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(audio_pan, surround_vbap_hits_speakers_and_keeps_power)
{
    const NphAudioLayout layouts[3] = {NPH_AUDIO_LAYOUT_QUAD, NPH_AUDIO_LAYOUT_SURROUND_5_1,
                                       NPH_AUDIO_LAYOUT_SURROUND_7_1};
    for (u32 li = 0U; li < 3U; ++li) {
        NphAudioPanner p;
        NTEST_ASSERT_EQ_INT(nph_audio_panner_init(&p, layouts[li]), NPH_RESULT_OK);
        const NphAudioLayoutInfo *info = nph_audio_layout_info(layouts[li]);
        f32 g[NPH_AUDIO_MAX_CHANNELS];
        // At a speaker's own azimuth, that speaker alone.
        for (u32 c = 0U; c < info->channels; ++c) {
            if (c == info->lfe) {
                continue;
            }
            nph_audio_pan(&p, info->azimuth[c], 0.0F, g);
            for (u32 o = 0U; o < info->channels; ++o) {
                NTEST_ASSERT_NEAR_F32(g[o], o == c ? 1.0F : 0.0F, 1e-5F);
            }
        }
        // Everywhere: unit power, at most two speakers, never the LFE, and
        // no jump between neighbouring directions.
        f32 previous[NPH_AUDIO_MAX_CHANNELS];
        nph_audio_pan(&p, deg(-180.0F), 0.0F, previous);
        for (i32 d = -179; d <= 180; ++d) {
            nph_audio_pan(&p, deg((f32)d), 0.0F, g);
            NTEST_ASSERT_NEAR_F32(power(g, info->channels), 1.0F, 1e-5F);
            u32 active = 0U;
            for (u32 o = 0U; o < info->channels; ++o) {
                NTEST_ASSERT_TRUE(g[o] >= 0.0F);
                active += g[o] > 1e-6F ? 1U : 0U;
                NTEST_ASSERT_TRUE(fabsf(g[o] - previous[o]) < 0.1F);
                previous[o] = g[o];
            }
            NTEST_ASSERT_LE_UINT(active, 2U);
            if (info->lfe != NPH_AUDIO_NO_LFE) {
                NTEST_ASSERT_TRUE(g[info->lfe] == 0.0F);
            }
        }
        // Any azimuth is wrapped: 370 degrees is 10.
        f32 wrapped[NPH_AUDIO_MAX_CHANNELS];
        nph_audio_pan(&p, deg(10.0F), 0.0F, g);
        nph_audio_pan(&p, deg(370.0F), 0.0F, wrapped);
        for (u32 o = 0U; o < info->channels; ++o) {
            NTEST_ASSERT_NEAR_F32(g[o], wrapped[o], 1e-5F);
        }
    }
}

NTEST(audio_pan, spread_widens_and_keeps_power)
{
    NphAudioPanner p;
    NTEST_ASSERT_EQ_INT(nph_audio_panner_init(&p, NPH_AUDIO_LAYOUT_SURROUND_7_1), NPH_RESULT_OK);
    f32 point[NPH_AUDIO_MAX_CHANNELS];
    f32 narrow[NPH_AUDIO_MAX_CHANNELS];
    f32 wide[NPH_AUDIO_MAX_CHANNELS];
    f32 all[NPH_AUDIO_MAX_CHANNELS];
    nph_audio_pan(&p, deg(15.0F), 0.0F, point);
    nph_audio_pan(&p, deg(15.0F), deg(2.0F), narrow);
    nph_audio_pan(&p, deg(15.0F), deg(120.0F), wide);
    nph_audio_pan(&p, deg(15.0F), 6.2831853F, all);
    u32 wide_active = 0U;
    for (u32 c = 0U; c < 8U; ++c) {
        NTEST_ASSERT_NEAR_F32(narrow[c], point[c], 0.02F); // a sliver is still a point
        wide_active += wide[c] > 1e-3F ? 1U : 0U;
    }
    NTEST_ASSERT_GE_UINT(wide_active, 3U);
    NTEST_ASSERT_NEAR_F32(power(narrow, 8U), 1.0F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(power(wide, 8U), 1.0F, 1e-5F);
    NTEST_ASSERT_NEAR_F32(power(all, 8U), 1.0F, 1e-5F);
    // All around: every full-range speaker sounds, the LFE does not.
    for (u32 c = 0U; c < 8U; ++c) {
        if (c == 3U) {
            NTEST_ASSERT_TRUE(all[c] == 0.0F);
        } else {
            NTEST_ASSERT_TRUE(all[c] > 0.15F);
        }
    }
}
