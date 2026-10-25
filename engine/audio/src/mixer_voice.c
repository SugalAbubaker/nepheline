// SPDX-License-Identifier: Apache-2.0
// One voice's block (mixer.h; docs/audio.md "Voices"): its clip read into a
// window of source frames (PCM, ADPCM, or lossless, looping or not), the
// window resampled to the mix rate and the voice's pitch, filtered, panned,
// and mixed into its bus and sends. Audio thread only.

#include <nepheline/core/assert.h>

#include "mixer_internal.h"

#include <math.h>
#include <string.h>

#define MV_HALF_MAX (NPH_AUDIO_RESAMPLE_MAX_TAPS / 2U - 1U) // frames the widest kernel reaches back

// A third of the way from `now` to `target`, in log frequency; there once
// within a hundredth of an octave.
static f32 mv_glide(f32 now, f32 target)
{
    const f32 ratio = target / now;
    if (ratio > 0.99309F && ratio < 1.00695F) {
        return target;
    }
    return now * powf(ratio, 1.0F / 3.0F);
}

void mix_filter_block(NphAudioMixer *m, const NphAudioFilterCoeffs *from,
                      const NphAudioFilterCoeffs *to, NphAudioFilterState *state, f32 *x,
                      u32 frames, MixFilterChange change)
{
    if (change != MIX_FILTER_STEADY) {
        memcpy(m->filter_dry, x, (size_t)frames * sizeof(f32));
    }
    if (from != NULL) {
        nph_audio_filter_process_glide(from, to, state, x, x, frames);
    } else {
        nph_audio_filter_process(to, state, x, x, frames);
    }
    if (change == MIX_FILTER_STEADY) {
        return;
    }
    // A filter switching on starts from silence, and one switching off
    // leaves its last state behind: either way its output and the signal
    // without it differ for a moment, so the block crossfades between the
    // two. The crossfade is a smoothstep, t^2 (3 - 2t), whose share starts
    // at about 3 t^2: the filter's first samples, where the difference is,
    // come in at a hundred-thousandth, where a linear ramp's 1/256 would
    // leave a tick. It ends at exactly the filtered signal (or the dry one).
    const f32 *dry = m->filter_dry;
    const f32 step = 1.0F / (f32)frames;
    const bool on = change == MIX_FILTER_SWITCH_ON;
    for (u32 i = 0U; i < frames; ++i) {
        const f32 t = (f32)(i + 1U) * step;
        const f32 s = t * t * (3.0F - 2.0F * t);
        const f32 wet = on ? s : 1.0F - s;
        x[i] = dry[i] + (x[i] - dry[i]) * wet;
    }
}

u64 mix_voice_step(const NphAudioMixer *m, const MixVoice *v)
{
    const MixClip *c = m->audio_clips[v->clip];
    return nph_audio_resample_step(c->rate_ratio * (f64)v->pitch);
}

// The clip frame behind timeline frame `t`, or -1 for silence: before the
// clip, or after it for a voice that does not loop. Loops repeat
// [loop_start, loop_end).
static i64 mv_clip_frame(const MixClip *c, const MixVoice *v, i64 t)
{
    if (t < 0) {
        return -1;
    }
    if (v->loop) {
        const i64 end = (i64)c->loop_end;
        if (t >= end) {
            const i64 start = (i64)c->loop_start;
            return start + (t - start) % (end - start);
        }
        return t;
    }
    return t < (i64)c->frames ? t : -1;
}

static void mv_zero(f32 *const *dst, u32 channels, u32 at, u32 count)
{
    for (u32 ch = 0U; ch < channels; ++ch) {
        memset(dst[ch] + at, 0, (size_t)count * sizeof(f32));
    }
}

// Positions the lossless decoder at clip frame `frame`.
static bool mv_seek(NphAudioMixer *m, const MixClip *c, MixContext *ctx, u64 frame)
{
    const NphLosslessStream *s = &c->lossless;
    const u32 block = (u32)(frame / s->block_frames);
    if (ctx->decoder_block == block && ctx->decoder_frame == frame &&
        nph_lossless_decoder_remaining(&ctx->decoder) > 0U) {
        return true;
    }
    const u64 start = s->block_offsets[block];
    const u64 end = block + 1U < s->block_count ? s->block_offsets[block + 1U] : s->size;
    ctx->decoder_block = MIX_NONE;
    if (nph_lossless_decoder_begin(&ctx->decoder, s->data + start, end - start, s->channels,
                                   s->bits) != NPH_RESULT_OK) {
        m->corrupt_reads += 1U;
        return false;
    }
    const u64 block_first = (u64)block * s->block_frames;
    if (nph_lossless_decoder_skip(&ctx->decoder, (u32)(frame - block_first)) != NPH_RESULT_OK) {
        m->corrupt_reads += 1U;
        return false;
    }
    ctx->decoder_block = block;
    ctx->decoder_frame = frame;
    return true;
}

// Reads clip frames [frame, frame + count) (all within the clip) into
// dst[ch] + at.
static void mv_read_clip(NphAudioMixer *m, const MixClip *c, MixContext *ctx, u64 frame, u32 count,
                         f32 *const *dst, u32 at)
{
    switch (c->codec) {
    case NPH_AUDIO_CODEC_PCM: {
        const u32 bytes = nph_audio_format_bytes(c->format);
        const u8 *base = c->data + frame * c->bytes_per_frame;
        for (u32 ch = 0U; ch < c->channels; ++ch) {
            const void *src = base + (u64)ch * bytes;
            switch (c->format) {
            case NPH_AUDIO_FORMAT_S16:
                m->kernels->load_s16(src, c->channels, dst[ch] + at, count);
                break;
            case NPH_AUDIO_FORMAT_S24:
                m->kernels->load_s24(src, c->channels, dst[ch] + at, count);
                break;
            case NPH_AUDIO_FORMAT_S32:
                m->kernels->load_s32(src, c->channels, dst[ch] + at, count);
                break;
            case NPH_AUDIO_FORMAT_F32:
                m->kernels->load_f32(src, c->channels, dst[ch] + at, count);
                break;
            case NPH_AUDIO_FORMAT_UNKNOWN:
            case NPH_AUDIO_FORMAT_COUNT:
                memset(dst[ch] + at, 0, (size_t)count * sizeof(f32));
                break;
            }
        }
        return;
    }
    case NPH_AUDIO_CODEC_ADPCM: {
        f32 *planes[NPH_AUDIO_MAX_CHANNELS];
        for (u32 ch = 0U; ch < c->channels; ++ch) {
            planes[ch] = dst[ch] + at;
        }
        nph_adpcm_decode(&c->adpcm, frame, count, planes);
        return;
    }
    case NPH_AUDIO_CODEC_LOSSLESS: {
        // Sequential: seek when the decoder is not already there, then read
        // across blocks.
        i32 scratch[NPH_AUDIO_MAX_CHANNELS * MIX_BLOCK];
        u32 done = 0U;
        while (done < count) {
            const u64 f = frame + done;
            if (!mv_seek(m, c, ctx, f)) {
                mv_zero(dst, c->channels, at + done, count - done);
                return;
            }
            const u32 n = nph_min_u32(nph_min_u32(count - done, MIX_BLOCK),
                                      nph_lossless_decoder_remaining(&ctx->decoder));
            f32 *planes[NPH_AUDIO_MAX_CHANNELS];
            for (u32 ch = 0U; ch < c->channels; ++ch) {
                planes[ch] = dst[ch] + at + done;
            }
            if (nph_lossless_decoder_read(&ctx->decoder, planes, n, scratch) != NPH_RESULT_OK) {
                // The clip was checked when it was made, so this is memory
                // gone bad: silence for the rest, and counted.
                m->corrupt_reads += 1U;
                ctx->decoder_block = MIX_NONE;
                mv_zero(dst, c->channels, at + done, count - done);
                return;
            }
            done += n;
            ctx->decoder_frame = f + n;
            if (nph_lossless_decoder_remaining(&ctx->decoder) == 0U) {
                // The next read begins the next block.
                ctx->decoder_block = MIX_NONE;
            }
        }
        return;
    }
    case NPH_AUDIO_CODEC_COUNT:
        break;
    }
    mv_zero(dst, c->channels, at, count);
}

// Reads timeline frames [first, first + count) into dst[ch] + at, mapping
// each run of them to the clip (silence, a contiguous range, a loop's wrap).
static void mv_read_timeline(NphAudioMixer *m, const MixClip *c, const MixVoice *v, MixContext *ctx,
                             i64 first, u32 count, f32 *const *dst, u32 at)
{
    u32 done = 0U;
    while (done < count) {
        const i64 t = first + (i64)done;
        const i64 f = mv_clip_frame(c, v, t);
        u32 run = count - done;
        if (f < 0) {
            // Silence until the clip starts (t < 0) or for ever after its end.
            if (t < 0) {
                run = (u32)nph_min_i64((i64)run, -t);
            }
            mv_zero(dst, c->channels, at + done, run);
        } else {
            // Contiguous in the clip up to the loop's end or the clip's.
            const u64 limit = v->loop ? c->loop_end : c->frames;
            run = (u32)nph_min_u64(run, limit - (u64)f);
            mv_read_clip(m, c, ctx, (u64)f, run, dst, at + done);
        }
        done += run;
    }
}

void mix_voice_pan_matrix(const NphAudioMixer *m, const MixVoice *v, u32 channels,
                          f32 out[NPH_AUDIO_MAX_CHANNELS][NPH_AUDIO_MAX_CHANNELS])
{
    memset(out, 0, sizeof(f32) * NPH_AUDIO_MAX_CHANNELS * NPH_AUDIO_MAX_CHANNELS);
    const NphAudioLayoutInfo *out_info = nph_audio_layout_info(m->layout);
    const f32 pan = v->pan < -1.0F ? -1.0F : (v->pan > 1.0F ? 1.0F : v->pan);
    if (channels == 1U) {
        // A point in front: on two channels the pan is the sine law's
        // position itself; on more, the front stage (+-30 degrees).
        const f32 azimuth = m->channels == 2U ? asinf(pan) : pan * 0.5235988F;
        nph_audio_pan(&m->panner, azimuth, 0.0F, out[0]);
        return;
    }
    // Several channels: speaker feeds in the clip's layout (the first
    // `channels` of 7.1's when the count has no layout). A channel whose
    // speaker the output has plays there alone, so matching layouts map
    // channel to channel and a stereo clip keeps its width on every layout.
    // One the output lacks is a virtual speaker at its own angle: on two
    // channels it goes to its side (the front stage's +-30 degrees are the
    // pair's ends, so sides and backs fold onto them and the centre splits
    // at -3 dB, as in the ITU-R BS.775 down-mix but at constant power); on
    // more, it is panned between the speakers that bracket it. The LFE goes
    // to the LFE, or nowhere. The pan becomes a balance.
    NphAudioLayout layout;
    const NphAudioLayoutInfo *in_info =
        nph_audio_layout_for_channels(channels, &layout) == NPH_RESULT_OK
            ? nph_audio_layout_info(layout)
            : nph_audio_layout_info(NPH_AUDIO_LAYOUT_SURROUND_7_1);
    for (u32 c = 0U; c < channels && c < NPH_AUDIO_MAX_CHANNELS; ++c) {
        if (c == in_info->lfe) {
            if (out_info->lfe != NPH_AUDIO_NO_LFE) {
                out[c][out_info->lfe] = 1.0F;
            }
            continue;
        }
        const f32 azimuth = in_info->azimuth[c];
        u32 same = MIX_NONE;
        for (u32 o = 0U; o < out_info->channels; ++o) {
            if (out_info->speaker[o] == in_info->speaker[c]) {
                same = o;
            }
        }
        if (same != MIX_NONE) {
            out[c][same] = 1.0F;
        } else if (m->channels == 2U) {
            // sin(30 degrees) is 1/2; the stereo law clamps past the ends.
            nph_audio_pan_stereo(2.0F * sinf(azimuth), &out[c][0], &out[c][1]);
        } else {
            nph_audio_pan(&m->panner, azimuth, 0.0F, out[c]);
        }
        f32 balance = 1.0F;
        if (azimuth < -1e-3F && pan > 0.0F) {
            balance = 1.0F - pan;
        } else if (azimuth > 1e-3F && pan < 0.0F) {
            balance = 1.0F + pan;
        }
        for (u32 o = 0U; o < m->channels; ++o) {
            out[c][o] *= balance;
        }
    }
}

bool mix_voice_finished(const NphAudioMixer *m, const MixVoice *v)
{
    if (v->loop) {
        return false;
    }
    const MixClip *c = m->audio_clips[v->clip];
    return (v->pos >> 32U) >= c->frames + MIX_TAIL_FRAMES;
}

// Wraps a looping voice's position back into its loop.
static void mv_wrap(const MixClip *c, MixVoice *v, MixContext *ctx)
{
    if (!v->loop) {
        return;
    }
    const u64 end = c->loop_end;
    const u64 len = end - c->loop_start;
    const u64 frame = v->pos >> 32U;
    if (frame < end) {
        return;
    }
    const u64 laps = (frame - c->loop_start) / len;
    const u64 back = laps * len;
    v->pos -= back << 32U;
    if (ctx != NULL) {
        ctx->history_first -= (i64)back;
    }
}

void mix_voice_advance(NphAudioMixer *m, MixVoice *v, u32 frames)
{
    const MixClip *c = m->audio_clips[v->clip];
    v->pos += (u64)frames * mix_voice_step(m, v);
    mv_wrap(c, v, NULL);
}

void mix_voice_render(NphAudioMixer *m, MixVoice *v, u32 offset, u32 frames)
{
    const MixClip *c = m->audio_clips[v->clip];
    MixContext *ctx = &m->contexts[v->context];
    const u32 channels = c->channels;
    const u64 step = mix_voice_step(m, v);
    const NphAudioResampleBank *bank = nph_audio_resampler_bank(m->resampler, step);
    const bool copy = step == NPH_AUDIO_FRAME_ONE && (u32)v->pos == 0U;

    // The window: from the widest kernel's reach before the first position
    // to this kernel's reach after the last.
    const i64 base = (i64)(v->pos >> 32U);
    const i64 first = base - (i64)MV_HALF_MAX;
    i64 last_pos = 0;
    i64 span_first = 0;
    nph_audio_resample_span(v->pos, step, frames, bank->taps, &span_first, &last_pos);
    const i64 last = last_pos;
    const u32 len = (u32)(last - first + 1);
    NPH_ASSERT(len <= MIX_WINDOW);
    u32 have = 0U;
    if (ctx->history_len > 0U && ctx->history_first <= first) {
        const i64 hist_last = ctx->history_first + (i64)ctx->history_len - 1;
        if (hist_last >= first) {
            have = (u32)nph_min_i64(hist_last - first + 1, (i64)len);
            const u32 from = (u32)(first - ctx->history_first);
            for (u32 ch = 0U; ch < channels; ++ch) {
                memcpy(m->window[ch], ctx->history[ch] + from, (size_t)have * sizeof(f32));
            }
        }
    }
    if (have < len) {
        mv_read_timeline(m, c, v, ctx, first + (i64)have, len - have, m->window, have);
    }

    // Resample (or copy, at the mix rate exactly on a whole frame).
    const u32 at = (u32)(base - first);
    if (copy) {
        for (u32 ch = 0U; ch < channels; ++ch) {
            memcpy(m->voice_out[ch], m->window[ch] + at, (size_t)frames * sizeof(f32));
        }
    } else {
        const f32 *src[NPH_AUDIO_MAX_CHANNELS];
        for (u32 ch = 0U; ch < channels; ++ch) {
            src[ch] = m->window[ch] + at;
        }
        m->kernels->resample(bank, src, channels, v->pos & 0xFFFFFFFFULL, step, m->voice_out,
                             frames);
    }

    // Keep what the next block needs: from the widest kernel's reach before
    // the next position to the last frame read.
    v->pos += (u64)frames * step;
    const i64 keep_first = (i64)(v->pos >> 32U) - (i64)MV_HALF_MAX;
    const i64 keep_from = keep_first > first ? keep_first : first;
    const u32 keep = (u32)(last - keep_from + 1);
    NPH_ASSERT(keep <= MIX_HISTORY);
    for (u32 ch = 0U; ch < channels; ++ch) {
        memcpy(ctx->history[ch], m->window[ch] + (keep_from - first), (size_t)keep * sizeof(f32));
    }
    ctx->history_first = keep_from;
    ctx->history_len = keep;
    mv_wrap(c, v, ctx);

    // Filters, their corners moving a third of the way (in octaves) to
    // their targets each block, and gliding there sample by sample, so a
    // sweep is smooth. Off is fully open, where the filter drops out and its
    // state starts over; the blocks a filter switches on and off in
    // crossfade (mix_filter_block). A voice's first block has no last block
    // to meet: its filters start from the silence before it.
    const bool lowpass_was = v->lowpass_now < MIX_FULL_BAND;
    const bool highpass_was = v->highpass_now > MIX_HIGHPASS_OFF;
    const f32 lowpass_before = v->lowpass_now;
    const f32 highpass_before = v->highpass_now;
    const NphAudioFilterCoeffs lowpass_from = v->lowpass;
    const NphAudioFilterCoeffs highpass_from = v->highpass;
    const f32 lp_target = v->lowpass_hz > 0.0F ? v->lowpass_hz : MIX_FULL_BAND;
    const f32 hp_target = v->highpass_hz > 0.0F ? v->highpass_hz : MIX_HIGHPASS_OFF;
    v->lowpass_now = mv_glide(v->lowpass_now, lp_target);
    v->highpass_now = mv_glide(v->highpass_now, hp_target);
    const bool lowpass = v->lowpass_now < MIX_FULL_BAND;
    const bool highpass = v->highpass_now > MIX_HIGHPASS_OFF;
    const MixFilterChange lp_change =
        lowpass == lowpass_was || v->fresh
            ? MIX_FILTER_STEADY
            : (lowpass ? MIX_FILTER_SWITCH_ON : MIX_FILTER_SWITCH_OFF);
    const MixFilterChange hp_change =
        highpass == highpass_was || v->fresh
            ? MIX_FILTER_STEADY
            : (highpass ? MIX_FILTER_SWITCH_ON : MIX_FILTER_SWITCH_OFF);
    const bool run_lowpass = lowpass || lp_change != MIX_FILTER_STEADY;
    const bool run_highpass = highpass || hp_change != MIX_FILTER_STEADY;
    if (run_lowpass) {
        v->lowpass = nph_audio_filter_design(NPH_AUDIO_FILTER_LOWPASS, v->lowpass_now, 0.7071068F,
                                             0.0F, (f32)m->rate);
    }
    if (run_highpass) {
        v->highpass = nph_audio_filter_design(NPH_AUDIO_FILTER_HIGHPASS, v->highpass_now,
                                              0.7071068F, 0.0F, (f32)m->rate);
    }
    // Glide where the corner moved and the filter ran last block too
    // (switching off included: it glides out to fully open).
    const bool lp_glides = lowpass_was && !v->fresh && v->lowpass_now != lowpass_before;
    const bool hp_glides = highpass_was && !v->fresh && v->highpass_now != highpass_before;
    for (u32 ch = 0U; ch < channels; ++ch) {
        if (run_lowpass) {
            mix_filter_block(m, lp_glides ? &lowpass_from : NULL, &v->lowpass, &ctx->lowpass[ch],
                             m->voice_out[ch], frames, lp_change);
        }
        if (run_highpass) {
            mix_filter_block(m, hp_glides ? &highpass_from : NULL, &v->highpass, &ctx->highpass[ch],
                             m->voice_out[ch], frames, hp_change);
        }
    }
    if (!lowpass) {
        memset(ctx->lowpass, 0, sizeof ctx->lowpass);
    }
    if (!highpass) {
        memset(ctx->highpass, 0, sizeof ctx->highpass);
    }

    // Gains: from where the last block left them to this block's, which
    // carry the volume, the fade at the block's end, and the pan.
    f32 matrix[NPH_AUDIO_MAX_CHANNELS][NPH_AUDIO_MAX_CHANNELS];
    mix_voice_pan_matrix(m, v, channels, matrix);
    f32 fade_end = v->fade + v->fade_rate * (f32)frames;
    fade_end = fade_end < 0.0F ? 0.0F : (fade_end > 1.0F ? 1.0F : fade_end);
    // A demoted voice ramps to silence this block, but its fade runs on: it
    // comes back where the fade would be.
    const f32 level = v->demoting ? 0.0F : v->volume * fade_end;
    MixBus *bus = &m->buses[v->bus];
    // Sends take the voice as panned, scaled by their own gains, and ramp
    // with it: from the last block's gain times the send's last gain to this
    // block's times its new one.
    MixBus *send_target[NPH_AUDIO_MAX_SENDS];
    f32 s0[NPH_AUDIO_MAX_SENDS];
    f32 s1[NPH_AUDIO_MAX_SENDS];
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        send_target[s] = v->send_bus[s] != MIX_NONE ? &m->buses[v->send_bus[s]] : NULL;
        s1[s] = v->send_gain[s];
        s0[s] = v->fresh ? s1[s] : (v->entering ? 0.0F : v->sends_current[s]);
        if (s0[s] == 0.0F && s1[s] == 0.0F) {
            send_target[s] = NULL;
        }
    }
    for (u32 ch = 0U; ch < channels; ++ch) {
        for (u32 o = 0U; o < m->channels; ++o) {
            const f32 g1 = matrix[ch][o] * level;
            const f32 g0 = v->fresh ? g1 : (v->entering ? 0.0F : v->gains[ch][o]);
            if (g0 != 0.0F || g1 != 0.0F) {
                m->kernels->mix_ramp(bus->buffer[o] + offset, m->voice_out[ch], frames, g0, g1);
                for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
                    if (send_target[s] != NULL) {
                        m->kernels->mix_ramp(send_target[s]->buffer[o] + offset, m->voice_out[ch],
                                             frames, g0 * s0[s], g1 * s1[s]);
                    }
                }
            }
            v->gains[ch][o] = g1;
        }
    }
    for (u32 s = 0U; s < NPH_AUDIO_MAX_SENDS; ++s) {
        v->sends_current[s] = s1[s];
    }
    v->fade = fade_end;
    v->entering = 0;
    v->fresh = 0;
}
