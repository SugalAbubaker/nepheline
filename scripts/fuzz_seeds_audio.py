# SPDX-License-Identifier: Apache-2.0
"""Seed corpora of the audio fuzz targets (tests/fuzz/fuzz_audio_wav.c,
fuzz_audio_lossless.c, fuzz_audio_adpcm.c) and of the cooker's sounds
(fuzz_cook_sound.c), written by `python scripts/fuzz_seeds.py`.

The WAV seeds are files built here from the RIFF/WAVE layout (an
independent reading of the format the reader takes): every depth, the
extensible header, loops, chunks to skip, and the damage writers leave
behind. The codec targets read encoder input, not streams: channels,
depth, block size, and samples, then a truncation and damage records that
the target applies to a copy of the stream the encoder made (the layout is
in each target's header comment).
"""

import math
import os
import struct


def chunk(tag, body):
    out = tag.encode("ascii") + struct.pack("<I", len(body)) + body
    return out + (b"\x00" if len(body) & 1 else b"")


def riff(chunks, riff_size=None):
    body = b"WAVE" + b"".join(chunks)
    size = len(body) if riff_size is None else riff_size
    return b"RIFF" + struct.pack("<I", size) + body


def fmt(tag, channels, rate, bits):
    block = channels * (bits // 8)
    return chunk("fmt ", struct.pack("<HHIIHH", tag, channels, rate, rate * block, block, bits))


def fmt_extensible(channels, rate, bits, valid, subformat):
    block = channels * (bits // 8)
    guid = struct.pack("<H", subformat) + bytes(
        [0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])
    body = struct.pack("<HHIIHH", 0xFFFE, channels, rate, rate * block, block, bits)
    body += struct.pack("<HHI", 22, valid, (1 << channels) - 1) + guid
    return chunk("fmt ", body)


def tone(frames, channels, freq, amp, rate=48000):
    return [[amp * math.sin(2.0 * math.pi * freq * (1.0 + 0.25 * c) * i / rate)
             for c in range(channels)] for i in range(frames)]


def pcm(frames, bits):
    out = bytearray()
    full = (1 << (bits - 1)) - 1
    for frame in frames:
        for v in frame:
            q = max(-full - 1, min(full, int(round(v * full))))
            if bits == 8:
                out.append(q + 128)
            elif bits == 16:
                out += struct.pack("<h", q)
            elif bits == 24:
                out += struct.pack("<i", q)[:3]
            else:
                out += struct.pack("<i", q)
    return bytes(out)


def floats(frames, width):
    code = "<f" if width == 32 else "<d"
    return b"".join(struct.pack(code, v) for frame in frames for v in frame)


def smpl(start, end):
    body = struct.pack("<9I", 0, 0, 20833, 60, 0, 0, 0, 1, 0)
    body += struct.pack("<6I", 0, 0, start, end, 0, 0)
    return chunk("smpl", body)


def damage(truncation=0, records=()):
    out = struct.pack("<H", truncation)
    for offset, value in records:
        out += struct.pack("<HB", offset, value)
    return out


def samples16(values):
    return b"".join(struct.pack("<h", max(-32768, min(32767, int(v)))) for v in values)


def write_audio_seeds(write, corpus):
    wav = os.path.join(corpus, "audio_wav")
    t = tone(64, 2, 440.0, 0.5)
    write(os.path.join(wav, "pcm16_stereo"), riff([fmt(1, 2, 48000, 16), chunk("data", pcm(t, 16))]))
    write(os.path.join(wav, "pcm8_mono"),
          riff([fmt(1, 1, 22050, 8), chunk("data", pcm(tone(51, 1, 200.0, 0.8), 8))]))
    write(os.path.join(wav, "pcm24_extensible_loop"),
          riff([chunk("LIST", b"INFOISFT\x05\x00\x00\x00nph\x00\x00"),
                fmt_extensible(2, 44100, 24, 24, 1), chunk("data", pcm(t, 24)), smpl(4, 40)]))
    write(os.path.join(wav, "pcm32_six"),
          riff([fmt(1, 6, 96000, 32), chunk("data", pcm(tone(20, 6, 100.0, 0.9), 32))]))
    write(os.path.join(wav, "float32_fact"),
          riff([fmt(3, 1, 48000, 32), chunk("fact", struct.pack("<I", 32)),
                chunk("data", floats(tone(32, 1, 1000.0, 0.25), 32))]))
    write(os.path.join(wav, "float64_extensible"),
          riff([fmt_extensible(2, 48000, 64, 64, 3), chunk("data", floats(tone(16, 2, 50.0, 1.0), 64))]))
    # Damage writers leave: a data size past the end, a RIFF size of zero,
    # a trailing chunk cut short, a loop past the data.
    good = riff([fmt(1, 2, 48000, 16), chunk("data", pcm(t, 16))])
    write(os.path.join(wav, "data_past_end"), good[:-37])
    write(os.path.join(wav, "riff_size_zero"), riff([fmt(1, 2, 48000, 16), chunk("data", pcm(t, 16))], 0))
    write(os.path.join(wav, "trailing_cut"), good + b"junk\xff\xff\x00\x00ab")
    write(os.path.join(wav, "loop_outside"),
          riff([fmt(1, 1, 48000, 16), chunk("data", pcm(tone(10, 1, 300.0, 0.3), 16)), smpl(5, 900)]))
    write(os.path.join(wav, "adpcm_refused"), riff([fmt(2, 1, 48000, 16), chunk("data", b"\x00" * 8)]))

    # The cooker's sounds (fuzz_cook_sound.c): a settings byte (bits 0-1 the
    # codec: auto, pcm, lossless, adpcm; 2-3 the streaming: auto, always,
    # never; 4 exhaustive; 5-7 AUTO's threshold in tenths of a second),
    # then a WAV file.
    cook = os.path.join(corpus, "cook_sound")

    def settings(codec=0, stream=0, exhaustive=0, tenths=0):
        return bytes([codec | (stream << 2) | (exhaustive << 4) | (tenths << 5)])

    exact24 = [[(((i * 7919) % 16777216) - 8388608) / 8388608.0] for i in range(48)]
    write(os.path.join(cook, "auto_pcm16"),
          settings() + riff([fmt(1, 2, 48000, 16), chunk("data", pcm(t, 16))]))
    write(os.path.join(cook, "auto_streamed"),
          settings(tenths=1) + riff([fmt(1, 1, 8000, 16),
                                     chunk("data", pcm(tone(900, 1, 300.0, 0.5, 8000), 16))]))
    write(os.path.join(cook, "lossless24_streamed_loop"),
          settings(2, 1) + riff([fmt_extensible(2, 44100, 24, 24, 1), chunk("data", pcm(t, 24)),
                                 smpl(4, 40)]))
    write(os.path.join(cook, "adpcm8_mono"),
          settings(3, 2) + riff([fmt(1, 1, 22050, 8),
                                 chunk("data", pcm(tone(300, 1, 200.0, 0.8, 22050), 8))]))
    write(os.path.join(cook, "lossless_float_exact"),
          settings(2, 2, 1) + riff([fmt(3, 1, 48000, 32), chunk("data", floats(exact24, 32))]))
    write(os.path.join(cook, "lossless_float_kept"),
          settings(2) + riff([fmt(3, 1, 48000, 32),
                              chunk("data", floats(tone(32, 1, 1000.0, 0.25), 32))]))
    write(os.path.join(cook, "pcm_float64"),
          settings(1) + riff([fmt_extensible(2, 48000, 64, 64, 3),
                              chunk("data", floats(tone(16, 2, 50.0, 1.0), 64))]))
    write(os.path.join(cook, "lossless_s32_six"),
          settings(2) + riff([fmt(1, 6, 96000, 32), chunk("data", pcm(tone(20, 6, 100.0, 0.9), 32))]))

    # Lossless: u8 channels, u8 bits, u8 block, u16 frames, samples, damage.
    lossless = os.path.join(corpus, "audio_lossless")

    def ll_seed(channels, bits, block_byte, values, tail=b""):
        frames = len(values) // channels
        head = bytes([channels - 1, bits - 4, block_byte]) + struct.pack("<H", frames)
        return head + samples16(values) + tail

    sine = [12000 * math.sin(0.05 * i) for i in range(1200)]
    write(os.path.join(lossless, "mono16_sine"), ll_seed(1, 16, 30, sine))
    # The channel byte's top bit asks for the exhaustive order search.
    exhaustive = bytearray(ll_seed(1, 16, 30, sine[:500]))
    exhaustive[0] |= 0x80
    write(os.path.join(lossless, "mono16_sine_exhaustive"), bytes(exhaustive))
    stereo = []
    for i in range(800):
        left = 9000 * math.sin(0.031 * i) + 300 * math.sin(1.7 * i)
        stereo += [left, left * 0.9 + 50 * math.sin(0.4 * i)]
    write(os.path.join(lossless, "stereo16_correlated"), ll_seed(2, 16, 60, stereo))
    rng = 12345
    noise = []
    for _ in range(1000):
        rng = (rng * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        noise.append(((rng >> 33) & 0xFFFF) - 32768)
    write(os.path.join(lossless, "mono24_noise"), ll_seed(1, 24, 7, noise))
    write(os.path.join(lossless, "six8_square"),
          ll_seed(6, 8, 3, [(20000 if (i // 7) % 2 else -20000) for i in range(6 * 100)]))
    write(os.path.join(lossless, "stereo12_silence"), ll_seed(2, 12, 1, [0] * 400))
    write(os.path.join(lossless, "mono16_sine_damaged"),
          ll_seed(1, 16, 30, sine[:600], damage(0, [(12, 0x01), (40, 0x80), (300, 0x55)])))
    write(os.path.join(lossless, "stereo16_truncated"), ll_seed(2, 16, 60, stereo[:400], damage(200)))
    write(os.path.join(lossless, "stereo16_header_damage"),
          ll_seed(2, 16, 60, stereo[:400], damage(0, [(4, 0x10), (9, 0x01), (10, 0x02), (13, 0x40)])))

    # ADPCM: u8 channels, u16 frames, samples, damage.
    adpcm = os.path.join(corpus, "audio_adpcm")

    def ad_seed(channels, values, tail=b""):
        frames = len(values) // channels
        return bytes([channels - 1]) + struct.pack("<H", frames) + samples16(values) + tail

    write(os.path.join(adpcm, "mono_tone"), ad_seed(1, sine[:700]))
    write(os.path.join(adpcm, "stereo_mix"), ad_seed(2, stereo[:900]))
    write(os.path.join(adpcm, "mono_noise"), ad_seed(1, noise[:500]))
    write(os.path.join(adpcm, "three_short"), ad_seed(3, [100 * i for i in range(30)]))
    write(os.path.join(adpcm, "mono_damaged"),
          ad_seed(1, sine[:300], damage(0, [(2, 0xFF), (68, 0x0F), (70, 0xF0), (140, 0x33)])))
    write(os.path.join(adpcm, "stereo_truncated"), ad_seed(2, stereo[:600], damage(100)))
