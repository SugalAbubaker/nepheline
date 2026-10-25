// SPDX-License-Identifier: Apache-2.0
// Equal-power stereo panning and pairwise VBAP (pan.h).

#include <nepheline/audio/pan.h>
#include <nepheline/core/assert.h>

#include <math.h>
#include <string.h>

#define PAN_PI     3.14159265358979323846F
#define PAN_TWO_PI 6.28318530717958647692F

void nph_audio_pan_stereo(f32 pan, f32 *out_left, f32 *out_right)
{
    if (!(pan >= -1.0F)) {
        pan = -1.0F; // NaN too
    }
    if (pan > 1.0F) {
        pan = 1.0F;
    }
    const f32 angle = (pan + 1.0F) * (0.25F * PAN_PI);
    *out_left = cosf(angle);
    *out_right = sinf(angle);
}

NphResult nph_audio_panner_init(NphAudioPanner *panner, NphAudioLayout layout)
{
    NPH_ASSERT(panner != NULL);
    memset(panner, 0, sizeof *panner);
    const NphAudioLayoutInfo *info = nph_audio_layout_info(layout);
    if (info == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    panner->layout = layout;
    panner->channels = info->channels;
    if (layout == NPH_AUDIO_LAYOUT_MONO || layout == NPH_AUDIO_LAYOUT_STEREO) {
        return NPH_RESULT_OK;
    }
    // The full-range speakers in order of azimuth (insertion sort: at most
    // seven of them).
    u32 n = 0U;
    for (u32 c = 0U; c < info->channels; ++c) {
        if (c == info->lfe) {
            continue;
        }
        u32 at = n;
        while (at > 0U && panner->ring_azimuth[at - 1U] > info->azimuth[c]) {
            panner->ring_azimuth[at] = panner->ring_azimuth[at - 1U];
            panner->ring_channel[at] = panner->ring_channel[at - 1U];
            --at;
        }
        panner->ring_azimuth[at] = info->azimuth[c];
        panner->ring_channel[at] = c;
        ++n;
    }
    panner->ring_count = n;
    // Pair k is speakers k and k + 1 (the last pair wraps to the first).
    // Its matrix has the speakers' unit vectors (x right, y ahead) as rows;
    // a source's gains are its direction times the inverse.
    for (u32 k = 0U; k < n; ++k) {
        const f32 t1 = panner->ring_azimuth[k];
        const f32 t2 = panner->ring_azimuth[(k + 1U) % n];
        const f32 a = sinf(t1);
        const f32 b = cosf(t1);
        const f32 c = sinf(t2);
        const f32 d = cosf(t2);
        const f32 det = a * d - b * c;
        NPH_ASSERT(fabsf(det) > 1e-6F);
        panner->ring_inverse[k][0] = d / det;
        panner->ring_inverse[k][1] = -b / det;
        panner->ring_inverse[k][2] = -c / det;
        panner->ring_inverse[k][3] = a / det;
    }
    return NPH_RESULT_OK;
}

// Adds one point source's unit-power gains to `acc`.
static void pan_point(const NphAudioPanner *panner, f32 azimuth, f32 *acc)
{
    switch (panner->layout) {
    case NPH_AUDIO_LAYOUT_MONO:
        acc[0] += 1.0F;
        return;
    case NPH_AUDIO_LAYOUT_STEREO: {
        f32 l;
        f32 r;
        nph_audio_pan_stereo(sinf(azimuth), &l, &r);
        acc[0] += l;
        acc[1] += r;
        return;
    }
    case NPH_AUDIO_LAYOUT_QUAD:
    case NPH_AUDIO_LAYOUT_SURROUND_5_1:
    case NPH_AUDIO_LAYOUT_SURROUND_7_1:
    case NPH_AUDIO_LAYOUT_COUNT:
        break;
    }
    if (panner->ring_count == 0U) {
        return; // a zero-filled panner: no speakers to place it among
    }
    // The pair whose smaller gain is largest: both gains are non-negative
    // exactly for the pair that brackets the direction.
    const f32 px = sinf(azimuth);
    const f32 py = cosf(azimuth);
    u32 best = 0U;
    f32 best_g1 = 0.0F;
    f32 best_g2 = 0.0F;
    f32 best_min = -1e30F;
    for (u32 k = 0U; k < panner->ring_count; ++k) {
        const f32 *m = panner->ring_inverse[k];
        const f32 g1 = px * m[0] + py * m[2];
        const f32 g2 = px * m[1] + py * m[3];
        const f32 lo = g1 < g2 ? g1 : g2;
        if (lo > best_min) {
            best_min = lo;
            best = k;
            best_g1 = g1;
            best_g2 = g2;
        }
    }
    best_g1 = best_g1 > 0.0F ? best_g1 : 0.0F;
    best_g2 = best_g2 > 0.0F ? best_g2 : 0.0F;
    const f32 norm = sqrtf(best_g1 * best_g1 + best_g2 * best_g2);
    if (norm > 0.0F) {
        acc[panner->ring_channel[best]] += best_g1 / norm;
        acc[panner->ring_channel[(best + 1U) % panner->ring_count]] += best_g2 / norm;
    }
}

void nph_audio_pan(const NphAudioPanner *panner, f32 azimuth, f32 spread,
                   f32 gains[NPH_AUDIO_MAX_CHANNELS])
{
    NPH_ASSERT(panner != NULL && gains != NULL);
    f32 acc[NPH_AUDIO_MAX_CHANNELS] = {0.0F};
    if (!isfinite(azimuth)) {
        azimuth = 0.0F;
    }
    azimuth = remainderf(azimuth, PAN_TWO_PI);
    if (!(spread > 0.0F)) {
        pan_point(panner, azimuth, acc);
    } else {
        const f32 width = spread < PAN_TWO_PI ? spread : PAN_TWO_PI;
        for (u32 m = 0U; m < NPH_AUDIO_PAN_SPREAD_SOURCES; ++m) {
            const f32 t = ((f32)m + 0.5F) / (f32)NPH_AUDIO_PAN_SPREAD_SOURCES - 0.5F;
            pan_point(panner, azimuth + t * width, acc);
        }
    }
    f32 power = 0.0F;
    for (u32 c = 0U; c < panner->channels; ++c) {
        power += acc[c] * acc[c];
    }
    const f32 scale = power > 0.0F ? 1.0F / sqrtf(power) : 0.0F;
    for (u32 c = 0U; c < panner->channels; ++c) {
        gains[c] = acc[c] * scale;
    }
}
