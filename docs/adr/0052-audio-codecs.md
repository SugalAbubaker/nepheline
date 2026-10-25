# 0052. Audio codecs: PCM, our lossless codec, and our ADPCM

- **Date:** 2026-10-24
- **Status:** Accepted

## Context

CLAUDE.md §8.10 asks for "WAV/PCM and an ADPCM variant, written by us",
streaming for music, and "the compressed codec (e.g. Opus/Vorbis, ours or
wrapped) is chosen via an ADR". §13 lists that choice among the user's
decisions: "Compressed audio codec: write our own or wrap one (Phase 12)".

The forces:

- **Quality is never traded** (the user's decision of 2026-10-20). A lossy
  codec at any rate is a trade, however small.
- **The mixer decodes on the audio thread**, a block of 256 frames per voice
  at a time, for every voice that plays a compressed clip, with no
  allocation and bounded work (ADR-0051). Decode cost is per voice, and a
  game plays dozens of voices.
- **Clips start anywhere**: virtual voices resume mid-clip, streams seek,
  loops wrap. A codec's blocks must decode on their own.
- **Every parser is fuzzed** (§8.8): a damaged or hostile clip must never
  fault the mixer.
- **Third-party code stays behind our interfaces** (§1.8), and anything new
  needs an ADR; miniaudio's own decoders are excluded (§3: device output only).

On 2026-10-24 the user was asked which codec to use, with four options:

1. our own Opus decoder (CELT mode), checked against the official test
   vectors, with libopus encoding in the cooker only;
2. our own lossy game codec in the manner of RAD's Bink Audio;
3. wrapping libopus in the runtime and the cooker;
4. a lossless codec of our own (FLAC-class: linear prediction and Rice codes)
   for music, ambience, and dialogue, beside ADPCM for effects. Bit-exact,
   very cheap to decode, about six times the size of Opus at 128 kbps (an
   hour of stereo music in 350 to 400 MB rather than about 58 MB).

**The user chose the fourth: "Lossless, our own".**

## Decision

1. **Three codecs, all ours.** Every clip is stored in one of them:
   - **PCM** as authored: signed 16, 24, or 32 bits, or 32-bit float, in the
     WAVE channel order. Unsigned 8-bit and 64-bit float sources are
     converted, the 8-bit ones exactly.
   - **Lossless** (`lossless.h`): the default for everything.
   - **ADPCM** (`adpcm.h`): lossy, chosen per clip only by an import setting,
     for effects whose memory matters more than their last decibels of
     signal-to-noise.

   A float source is coded losslessly as 24-bit integers when every sample is
   exactly representable so; otherwise it stays PCM float, so even float
   sources play back bit-exact. Which codec a cooked sound uses is the
   cooker's decision (docs/cook.md, Phase 12 part F).

2. **The lossless codec is FLAC's design with our own bitstream.** The
   design is FLAC's (Coalson; RFC 9639, 2024):
   - each channel of a block is predicted by a fixed polynomial (orders 0 to
     4) or by linear prediction of order 1 to 32, Levinson-Durbin on the
     Tukey(0.5)-windowed autocorrelation (Makhoul 1975);
   - stereo pairs are decorrelated as left/right, left/side, side/right, or
     mid/side;
   - the residual is coded with partitioned Rice codes (Rice 1979), escaping
     to raw samples where those are smaller;
   - low bits that are zero throughout a block are shifted out.

   The bitstream is laid out for a game's mixer instead of for a file:
   - Blocks (4096 frames by default, up to 65535) decode on their own and are
     found through an offset table. A voice starts, resumes, or loops anywhere,
     at the cost of decoding at most one block's lead-in.
   - **A block lists where each channel's data starts**, so a voice decodes
     all its channels in lockstep, 256 frames per mixer block. Its state is
     the decoder's (464 bytes a channel), not a decoded block (32 KiB for a
     stereo block of 4096 frames). With FLAC's layout, where a channel's start
     is known only after the previous one is decoded, a voice must decode its
     whole block at once: a spike in one mixer block per 4096 frames, and a
     buffer per voice.
   - Bits are read least significant first from little-endian words: one
     load refills the reader, and a Rice code's unary part is a count of
     trailing zeros.
   - Every block carries a CRC-32C, checked off the audio thread (when a clip
     is made, or when a stream reads the block). The decoder is bounded on any
     input anyway: every read is checked against its subframe's end, every
     sample against its depth's range, every partition against the block. A
     corrupt block yields `NPH_RESULT_CORRUPT`, never a fault. This is fuzzed
     (`tests/fuzz/fuzz_audio_lossless.c`).

   Samples are signed integers of 4 to 24 bits, at up to eight channels.

3. **The encoder searches exactly where it matters.**
   - Each candidate channel (for stereo: left, right, side, mid) is coded
     with its cheapest subframe, and the cheapest of the four pairings wins.
     An estimate from fixed predictors' residuals, as libFLAC uses, chose
     mid/side for uncorrelated noise and lost the side channel's extra bit:
     102.4% of PCM, against 100.2% coded exactly.
   - Linear prediction tries the four orders whose prediction error promises
     the fewest bits (exhaustive mode tries every order).
   - Coefficients are quantised at 15 bits, fewer for short blocks, with the
     rounding error carried forward.
   - The residual's partition order (0 to 8) and each partition's Rice
     parameter come from exact bit counts.

   It runs about 22 times faster than real time for 48 kHz stereo (Profile,
   Ryzen 7 5800X), in the cooker.

4. **Our ADPCM uses per-block prediction.** Each block holds 128 frames of
   one channel in 69 bytes (4.3 bits a sample, 3.7:1 against 16-bit):
   - the two samples before it, so it decodes on its own;
   - one of the clip's 16 second-order predictors (Q11) and a shift;
   - 128 four-bit codes.

   This is block-adaptive ADPCM in the line of Microsoft ADPCM and the
   GameCube's DSP-ADPCM, not IMA ADPCM's per-sample step adaptation, which is
   what keeps tonal sounds clean. The encoder fits the clip's predictors:
   Microsoft's seven standard pairs, plus nine from weighted k-means over the
   blocks' own least-squares predictors, held inside the stability triangle.
   Per block it tries every predictor and shift in closed loop, quantising
   against its own reconstruction as the decoder will see it, and per sample
   picks the code with the least error after clamping. Measured: 54 to 88 dB
   signal-to-noise on tones, 24 dB on full-scale white noise (nothing to
   predict).

5. **WAV is the cooker's source format and our test and tool output.**
   - The reader handles 8, 16, 24, and 32-bit PCM and 32 and 64-bit floats,
     plain or `WAVE_FORMAT_EXTENSIBLE`, plus the sampler chunk's first loop.
   - It bounds every size by the image and cuts an over-long data chunk to
     whole frames, a common writer bug.
   - The writer produces 16-bit, 24-bit, and float files.
   - Both are fuzzed.

## Alternatives considered

- **The other three options the user was given** (above). Each is lossy:
  - Opus has the best quality per bit of the royalty-free codecs.
  - A Bink-style codec has the cheapest lossy decode.
  - Wrapping libopus is the fastest to build.

  The user's decision ranks bit-exactness above size.
- **FLAC's own bitstream** (MSB-first, sync codes, CRC-8 and CRC-16, frame
  numbers): compatible with existing tools, but its channels can only decode
  in sequence (see above), and nothing in a cooked asset needs to be read by
  another program.
- **Higher-ratio lossless codecs** (MPEG-4 ALS's RLS and LMS cascades,
  WavPack, Monkey's Audio, OptimFROG): 2 to 5% smaller with adaptive
  prediction and context-modelled entropy coding, at 2 to 10 times the decode
  cost per sample. That is the wrong trade for dozens of voices on one audio
  thread. It may return as an option for music streams, measured.
- **IMA ADPCM** (one step size adapted per sample): simpler, but noisier on
  tonal sounds than per-block predictors. **Microsoft ADPCM**'s fixed seven
  predictors: ours keeps those and learns nine more per clip.

## Consequences

- Music, ambience, and dialogue play back bit-exact.
  - Synthetic material measured here: a clean tone 15% of PCM, a chirp 12%,
    the benchmark's mix with a -50 dB noise floor about 63%, white noise
    100.2%.
  - Recorded music typically lands near half of PCM; real material will be
    measured as it arrives.
  - Sizes are a multiple of a lossy codec's: an hour of stereo 48 kHz music
    is roughly 350 to 400 MB.
- Decoding costs 10.3 ns a sample (20.6 ns a stereo frame, 0.1% of a core
  per stream), lossless at 16 or 24 bits alike; ADPCM 2.2 ns a sample.
- A voice that plays a lossless clip carries a decoder of 3.7 KB (eight
  channels' worth); the mixer pools them.
- Cooking an hour of music takes about three minutes.
- Every clip format is fuzzed, and damaged data degrades to silence and an
  error, never a fault.
