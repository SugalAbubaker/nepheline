# Audio (`engine/audio`)

The audio module (CLAUDE.md §8.10) mixes the game's sound: voices of clips and
streams, buses with effects, spatialisation around a listener, our own codecs,
and the output device, the one place miniaudio is used. It depends on
`platform` and through it on `math` and `core`. It does not know assets,
entities, or physics: the runtime gives it clips and streams, moves its voices,
and casts its occlusion rays. Decisions:
[ADR-0051](adr/0051-audio-module.md) (module boundaries, real-time scopes, the
signal format, the resampler), [ADR-0052](adr/0052-audio-codecs.md) (the
codecs, and the user's choice of a lossless one),
[ADR-0053](adr/0053-audio-mixer.md) (the mixer and its golden mixes),
[ADR-0054](adr/0054-audio-effects.md) (the effects),
[ADR-0055](adr/0055-audio-spatialisation.md) (3D), and
[ADR-0056](adr/0056-audio-device-streaming.md) (the device and streaming).

Phase 12 builds it in parts. This document describes what exists:

| Part | Status |
|---|---|
| Real-time scopes (core, platform); the signal format; the block kernels per SIMD level; the resampler; filters; delay lines; panning | done |
| Codecs: PCM, our lossless codec, our ADPCM; WAV files | done |
| The mixer: voices, clips, buses, virtual voices, ducking, snapshots, offline rendering, golden mixes | done |
| Effects and 3D: EQ, compressor, limiter, echo, reverberation; attenuation, Doppler, air absorption, occlusion, obstruction | done |
| The device and streaming | done |
| Cooked sounds: the sound asset and the cooker's WAV import ([ADR-0057](adr/0057-sound-assets.md), [cook.md](cook.md) "Sounds") | done |
| The engine: AudioSource and AudioListener, the bridge, one-shots, occlusion rays, the editor ([ADR-0058](adr/0058-audio-in-the-engine.md), [runtime.md](runtime.md) "Audio") | done |
| The sample, and the Phase 12 wrap-up | planned |

| Header | Contents |
|---|---|
| `audio.h` | the log category, limits, sample formats, channel layouts, decibels |
| `kernels.h` | the block kernels: mixing with ramps, meters, format loads and stores, resampling; per SIMD level |
| `resampler.h` | the bandlimited interpolator's kernel banks, steps, and spans |
| `filter.h` | the cookbook's biquads in state-variable form; the one-pole low-pass |
| `delay.h` | the delay line: whole and fractional reads, per block or per sample |
| `pan.h` | equal-power stereo panning, pairwise VBAP, spread |
| `wav.h` | WAV files: the reader the cooker uses, and a writer |
| `lossless.h` | the lossless codec: streams, validation, the encoder, the incremental decoder |
| `adpcm.h` | our ADPCM: streams, validation, random-access decoding, the encoder |
| `mixer.h` | the mixer: clips, voices (3D too), buses, effects, ducking, snapshots, rendering, statistics |
| `eq.h` | the six-band equaliser |
| `dynamics.h` | the compressor and the lookahead true-peak limiter |
| `echo.h` | the echo |
| `reverb.h` | the reverberator: a 16-line feedback delay network |
| `spatial.h` | attenuation laws, Doppler, air absorption, directions, spread |
| `device.h` | the output device (miniaudio): the system's or the null one, statistics |
| `stream.h` | the streamer, and streams' descriptions and sources |

## Conventions

- **Signals** are 32-bit floats, nominally in [-1, 1]. Inside the module they
  are planar: one contiguous array per channel. Interleaved samples exist only
  at the edges, in sources and in the device's buffers.
- **Blocks.** The mixer renders `NPH_AUDIO_BLOCK_FRAMES` = 256 frames at a time
  (5.3 ms at 48 kHz). A gain that changes does so as a linear ramp across one
  block: element i of a block of n gets g0 + i (g1 - g0) / n, so the next block,
  starting at g1, continues without a step, and no change clicks.
- **Layouts** are mono, stereo, quad, 5.1, and 7.1, with channels in the WAVE
  order (front left, front right, centre, LFE, back left, back right, side left,
  side right, as the layout has them). Speakers stand at the ITU-R BS.775 angles
  (fronts at 30 degrees, 5.1's surrounds at 110) and Dolby's 7.1 ones (sides at
  90, backs at 150); quad is a square. Each channel names its speaker
  (`NphAudioSpeaker`). 5.1's surrounds are the side pair, as in Microsoft's
  `KSAUDIO_SPEAKER_5POINT1_SURROUND`, so they play on 7.1's sides.
- **Directions** are azimuths in the listener's frame of the engine's
  convention (ADR-0008: right-handed, Y up, looking along -Z): 0 straight
  ahead, +pi/2 to the right, -pi/2 to the left, +-pi behind.
- **Levels** convert with `nph_audio_db_to_gain` and `nph_audio_gain_to_db`;
  -144 dB (the floor of 24-bit audio) and below is silence.

## Real-time scopes

The audio thread has a few milliseconds per block and no second chance: a
block that is late is a click. So it must not allocate, lock, wait, sleep, or
do file I/O, any of which can stall it for longer than its deadline (§8.10).
That rule is checked, not trusted (`core/realtime.h`; docs/core.md "Real-time
scopes"): a thread enters a scope around its deadline-bound work, and the
allocation entry points, the spin lock, and the platform's blocking calls
report any call made inside one. With asserts on (Debug, Development) a
violation fails an assertion, or is counted by a hook that a test installs to
prove a path clean; without asserts, scopes and checks compile to nothing.

Every kernel and DSP routine in this module is real-time safe: none
allocates, locks, or keeps hidden state. The things that allocate (a
resampler's banks, a delay line's ring, a stream's ring) do it once, at
creation, on the caller's thread.

## Kernels

`kernels.h` holds the block math on the pattern of the math module's
(ADR-0008 §4, §9): one source (`src/kernels_impl.h`) compiled at the SSE4.2
baseline and again for AVX2, chosen once by `nph_simd_detect_level`, and a
scalar reference that the tests compare against. Both SIMD levels do every
element's arithmetic in the same order on eight lanes (`NphF32x8`, one AVX
register or two SSE ones) with no FMA, and reduce sums the same way, so they
agree to the bit (the tests assert it); the scalar reference sums in another
order and agrees within rounding.

| Kernel | Does |
|---|---|
| `mix`, `mix_ramp` | dst += src times a gain, or a gain ramp |
| `scale_ramp` | dst *= a gain ramp, in place |
| `peak`, `sum_squares` | the meters' reductions (NaNs never win the peak) |
| `load_s16`, `load_s24`, `load_s32`, `load_f32` | one channel of interleaved samples to planar floats in [-1, 1) |
| `store_f32`, `store_s16` | planar channels to interleaved samples, clamped, NaNs as 0; 16-bit rounds to nearest |
| `resample` | the bandlimited interpolator, any number of channels |

## Resampling

A voice plays its source at the mix rate and its pitch, so the mixer reads
the source at fractional positions. Every output sample is the source
convolved with a windowed sinc centred on the output's position: bandlimited
interpolation (Smith & Gossett, "A Flexible Sampling-Rate Conversion Method",
ICASSP 1984; Smith, "Digital Audio Resampling Home Page", CCRMA).

**The kernel** is a sinc under a Kaiser window (Kaiser 1974) of beta 10.06 (a
100 dB design: 0.1102 (A - 8.7)), 24 zero crossings each side: 48 taps. Its
response is -6 dB at the source's Nyquist frequency.

**The table.** The kernel is needed at every fraction of a frame. It is
tabulated as 16 segments per frame, each tap a cubic in the position within
its segment, fitted through the segment's four Chebyshev nodes (the inverse
Vandermonde matrix is computed once). The fit is 125 dB below the signal and
the table's error against the exact kernel stays below 1e-7, the f32
coefficients' own rounding; a 48-tap bank is 12 KiB and stays in the L1 cache.
The kernel never evaluates the cubic per tap: it sums the source against the
segment's four rows (one per power of the position) and applies the cubic
once, to the four sums.

**Playing fast.** A step above one frame (pitch up, or a source rate above the
mix rate) needs the cutoff lowered to the output's Nyquist frequency, or what
lies above it folds back as aliases. There are 25 banks of cutoff 2^(-k/12),
from 1 down to 1/4, each kernel stretched by the inverse of its cutoff (up to
192 taps) and scaled back to unit gain; 670 KiB in all, computed when the
resampler is created (about 10 ms). A step uses the bank with the largest
cutoff at or below 1 / step. That cutoff is at most a semitone below the ideal
one, so the passband ends between 0.415 and 0.44 of the mix rate (19.9 to
21.1 kHz at 48 kHz) and nothing aliases below 0.46 of it. Steps are capped at
4.

**Positions** are 32.32 fixed point in source frames: the step is added
exactly, block after block, so a voice never drifts. A position's top four
fraction bits pick the segment and the rest are the cubic's argument.

Measured (`engine/audio/tests/test_resampler.c`, on tones, at steps from 0.5
to 4):

| Measure | Result |
|---|---|
| Passband ripple, to 0.4 of the output rate | within 0.0001 dB |
| Everything but the tone (images, aliases, rounding) | at least 113 dB down |
| Tones above the output's Nyquist frequency | suppressed by at least 115 dB |
| DC gain, any step and phase | 1 within 3e-6 |
| Whole frames at step 1 | the samples themselves, within 2e-6 |

## Filters

The second-order filters are the Audio EQ Cookbook's sections
(Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients",
2005): low-pass, high-pass, band-pass (0 dB at the centre), notch, all-pass,
peaking, and the two shelves, all the bilinear transform of the analog
section with the corner prewarped. They are realised as the
topology-preserving state-variable filter (Zavalishin, "The Art of VA Filter
Design", 2012; Simper, "Linear Trapezoidal Integrated SVF", Cytomic, 2013):
the same transfer functions, but the states are the integrators' and the
coefficients smooth functions of the corner. The filter stays stable however
its parameters move (a voice's occlusion), and it keeps its precision at low
corners, where a direct form's poles crowd against the unit circle.

A corner that steps once a block still kinks the output at each step, as
the phase response jumps: a second difference of 0.003 on a 200 Hz tone at
-6 dBFS, whose own is 0.00034. So a filter in motion glides.
`nph_audio_filter_process_glide` moves from one design to the next sample by
sample: the prewarped cutoff by a constant ratio, the damping and the output
mix linearly. It costs a division a sample.

`nph_audio_filter_magnitude` evaluates a section's transfer function exactly.
The tests compare every type with the cookbook's own direct-form formulas
(within 0.0001 dB from 20 Hz to 20 kHz), the processing with the response on
tones (within 0.01 dB), a 20 Hz high-pass at 48 kHz in f32 (-3.01 dB at its
corner, within 0.02 dB), and a resonant low-pass whose corner and Q jump every
64 samples on full-scale noise (bounded, no NaN). They also check glides of
four kinds against a double-precision reference (within 9e-6), from a
steady block into a glide and on into a steady block, and the same jumps
taken as glides (bounded).

The one-pole low-pass, y += a (x - y) with a = 1 - exp(-2 pi f / rate), smooths
and darkens (air absorption).

## Delay lines

A delay line is a power-of-two ring of past samples. Time counts samples
written; after t of them, a delay of d reads the sample written at t - d (1 is
the newest). A block is read before it is written, so a read at a delay of at
least the block's length never sees the block itself, which is what lets a
feedback loop of a block or more run a block at a time (the tests check a
comb filter run per block against one run per sample, to the bit). Shorter
loops, the diffusers' all-passes, run per sample with `tap` and `push`.
Fractional delays use the four-point Catmull-Rom spline: exact at whole
delays, exact on quadratics, smooth under modulation.

## Panning

Every panner keeps a source's total power constant (its squared gains sum to
one) wherever it is, so a sound circling the listener neither swells nor
dips.

- **Stereo** pans by the source's lateral position, the sine of its azimuth,
  with the equal-power (sine-cosine) law: centred is -3 dB in each channel. A
  source behind folds to its mirror image in front, as it must on two
  channels.
- **Quad, 5.1, and 7.1** use pairwise vector base amplitude panning (Pulkki,
  "Virtual Sound Source Positioning Using Vector Base Amplitude Panning",
  JAES 45(6), 1997) over the ring of full-range speakers: the two speakers
  that bracket the direction, with the gains that sum their direction vectors
  to it, normalised to unit power. A source at a speaker comes from that
  speaker alone; the LFE channel gets nothing.
- **Spread** (an angular width) is the sum of 16 virtual sources over the arc
  (Pulkki, "Uniform Spreading of Amplitude Panned Virtual Sources", WASPAA
  1999), normalised to unit power: a wide source covers more speakers, and one
  spread all around comes from everywhere.

## Codecs

Every clip is stored in one of three ways (ADR-0052):

- **PCM**, as authored: signed 16, 24, or 32 bits, or 32-bit float.
- **The lossless codec**, the default for everything: bit-exact at about
  half the size.
- **Our ADPCM**, lossy at 4.3 bits a sample, only where a clip's import
  settings ask for it (effects whose memory matters more than their last
  decibels).

The user chose lossless over a lossy codec on 2026-10-24 (§13). Every
format's reader is fuzzed (`tests/fuzz/fuzz_audio_*.c`, seeds from
`scripts/fuzz_seeds_audio.py`): the codec targets encode the input, check
the round trip, and then damage the stream, which validation must refuse
or the decoder survive.

### WAV

`wav.h` reads RIFF/WAVE images in memory without copying:
- integer PCM of 8, 16, 24, or 32 bits and IEEE floats of 32 or 64 bits,
  plain or `WAVE_FORMAT_EXTENSIBLE`;
- the first loop of the sampler chunk (`smpl`, whose end is inclusive in
  the file and exclusive here);
- every other chunk is skipped, odd sizes padded.

Every size is checked against the image. A data chunk that claims more than
the file holds is cut to whole frames (a common writer bug), and a loop
outside the data is dropped. The writer produces 16-bit, 24-bit, and float
files from planar floats (integers at x (2^(b-1) - 1), so +1 does not
clip).

### The lossless codec

The design is FLAC's (Coalson; RFC 9639): each channel of a block is
predicted, and what the prediction misses is entropy coded.
- **Predictors.** A fixed polynomial of order 0 to 4, or linear prediction
  of order 1 to 32 from Levinson-Durbin on the Tukey(0.5)-windowed
  autocorrelation (Makhoul 1975), quantised at 15 bits (fewer for short
  blocks) with the rounding error carried forward.
- **Stereo pairs** are coded as left/right, left/side, side/right, or
  mid/side.
- **The residual** uses partitioned Rice codes (Rice 1979), escaping to raw
  samples where that is smaller.
- **Wasted bits**: low bits that are zero throughout a block are shifted
  out.

The bitstream is our own, laid out for the mixer:

| Choice | Why |
|---|---|
| Blocks of 4096 frames (up to 65535), found through an offset table | a voice starts, resumes, or loops anywhere, decoding at most a block's lead-in |
| A block lists where each channel's subframe starts | a voice decodes all its channels in lockstep, 256 frames a mixer block, with a 464-byte state per channel instead of a decoded block per voice |
| Bits least significant first, refilled a little-endian word at a time | one load refills the reader; a Rice code's unary part is a count of trailing zeros |
| A CRC-32C per block, checked off the audio thread | a corrupt clip is refused when it is made, or when a stream reads it |
| A bounded decoder | reads checked against the subframe's end, samples against the depth, partitions against the block: garbage yields `NPH_RESULT_CORRUPT`, never a fault |

Block layout (little-endian): a CRC-32C of the rest, the block's size, its
frame count (u16), the stereo mode and channel count (u8 each), then each
channel's subframe offset (u32), then the subframes. A subframe is a kind
byte and a wasted-bits byte, then its bit stream: the constant, the verbatim
samples, or the warm-up samples, the coefficients (linear prediction:
precision and shift, then each), and the residual's partitions (order;
then per partition a Rice parameter, 31 escaping to a 5-bit raw width).

The encoder codes each candidate channel (for stereo: left, right, side,
mid) with its cheapest subframe and keeps the cheapest pairing: an estimate
from fixed predictors, as libFLAC uses, picked mid/side for uncorrelated
noise and paid the side channel's extra bit (102.4% of PCM, against 100.2%
coded exactly). Linear prediction tries the four orders whose error
promises the fewest bits; `exhaustive` tries all.

Measured on synthetic material (`audio_tests --filter=audio_lossless.*
--verbose`, 16-bit stereo):

| Signal | Size against PCM |
|---|---|
| A 440 Hz tone at -6 dB | 14.9% |
| A chirp | 12.3% |
| Impulses | 5.4% |
| Low bits unused (an 8-bit signal in 16) | 6.6% |
| A square wave | 43.9% |
| Harmonics with a -40 dB noise floor | 63.2% |
| Full-scale white noise | 100.2% |

Recorded music typically lands near half of PCM.

### ADPCM

Our ADPCM is block-adaptive, in the line of Microsoft ADPCM and the
GameCube's DSP-ADPCM rather than IMA's per-sample step adaptation:
- **The block.** 128 frames of one channel in 69 bytes (4.3 bits a sample,
  3.7:1 against 16-bit): the two samples before it (so it decodes on its
  own), a byte naming one of the clip's 16 predictors and a shift, and 128
  four-bit codes.
- **The decoder.** A sample is p + code * 2^shift, clamped to 16 bits, with
  p = (c1 s[n-1] + c2 s[n-2] + 1024) >> 11. It decodes any range directly,
  and reads silence past the end or where a stream is shorter than it says.
- **The predictors.** The encoder fits them to the clip: Microsoft's seven
  standard pairs, plus nine from weighted k-means over the blocks' own
  least-squares predictors, held inside the stability triangle.
- **The search.** Per block, the encoder tries every predictor and shift in
  closed loop, quantising against its own reconstruction as the decoder will
  see it, and per sample keeps the code with the least error after
  clamping.

Measured: 87.8 dB signal-to-noise on a 110 Hz tone, 66.0 at 1 kHz, 53.3 at
6 kHz, 74.7 dB on a quiet 440 Hz tone (-40 dB); 23.8 dB on full-scale white
noise, where there is nothing to predict.

## The mixer

`mixer.h` (ADR-0053) is the module's centre: clips, voices, buses, and
snapshots, rendered a block at a time.

**Two threads.** The game thread owns the mixer. It makes clips, buses, and
snapshots, plays and steers voices, and calls `nph_audio_mixer_update` once
a frame. The audio thread calls `nph_audio_mixer_render`; so does a test,
offline. The two talk only through:
- a command ring (game to audio), applied at the start of the next block;
- an event ring (audio to game): a voice ended, a clip or a bus may be freed;
- per-slot mailboxes for payloads too big for a command;
- relaxed atomics for the statistics and the bus meters.

Everything the audio thread touches is allocated by
`nph_audio_mixer_create`:
- the voice slots;
- a context per real voice, plus a quarter more for the voices fading out
  as others fade in, holding the source history, a lossless decoder, and
  filter states;
- the bus buffers and the scratch blocks.

A clip or a bus is freed by an update only after the audio thread's event
says it is done with it. The event ring holds every slot's event at once,
so it never refuses one. The command ring can fill: a play then fails with
`NPH_RESULT_FULL`, and every refused command is counted in the statistics.

**Clips** are PCM (16, 24, or 32-bit integers, or floats), lossless, or
ADPCM, at any rate from 8 to 192 kHz, with a loop region. A clip is checked
when it is made: every lossless block's CRC, an ADPCM stream's size and
shifts. The audio thread therefore never meets a malformed one, and a
decode that still fails (memory gone bad) plays silence and is counted. A
clip copies its data, or borrows it with `borrow`.

**Voices.**
- A voice plays a clip on a bus at a volume, pitch, and pan, from any frame
  (`start_frame`), after any delay (`delay_frames`, sample-accurate), once
  or looping, with up to two sends.
- Its position is 32.32 fixed point. It is resampled at the clip's rate
  times the pitch, or copied at the mix rate and pitch 1.
- A start at frame 0 keeps its attack. Any other start fades in over a
  block, and a stop fades over at least a block.
- A one-shot plays out its clip, then the resampler's 24-frame ring-out.

**Filters on voices** (the designers', and occlusion's: 3D below). A
low-pass and a high-pass, each of which:
- moves its corner a third of the way in octaves to its target each block;
- glides there sample by sample (`nph_audio_filter_process_glide`), so a
  sweep has no steps;
- is off when fully open (20 kHz, 10 Hz), and its state starts over;
- crossfades, over the block in which it switches on or off, between the
  voice through it and the voice without it, along a smoothstep, so the
  filter's start from silence is not heard. A linear crossfade let 1/256
  of the step through; the smoothstep lets through a hundred-thousandth.

**Multichannel clips** play speaker to speaker. A channel whose speaker
the output has plays there alone, so matching layouts map channel to
channel, and a stereo clip keeps its width everywhere. A channel the output
lacks plays as a speaker at its own angle:
- on stereo it goes to its side at constant power: the front stage's
  +-30 degrees are the pair's ends, sides and backs fold onto them, and the
  centre splits at -3 dB;
- on more channels it is panned between the two speakers that bracket it
  (VBAP).

The LFE goes to the LFE or nowhere. A mono clip's pan is its place; a
multichannel clip's pan is a balance, lowering the other side.

**Virtual voices.** Each block every playing voice is ranked by:
1. its priority (0 to 255);
2. then its score: its volume, times its fade while fading out, times its
   bus's gain and all its ancestors'.

A real voice's score counts double, a 6 dB hysteresis against two near
equals trading places every block. The first `max_real_voices` are mixed.
The rest are virtual: they advance through their clips and fades in
silence, holding no context, and come back exactly where they would be.
- A promotion fades in over a block, and so does a demotion out.
- A promoted lossless voice decodes its block's lead-in, at most 4095
  frames.
- Ties fall to the lower slot, so the ranking is deterministic. It is an
  in-place heapsort of 64-bit keys.

**Buses** form a tree under the master (`NPH_AUDIO_MASTER_BUS`), mixed
children first in an order Kahn's algorithm keeps. Sends count as edges,
and the game thread refuses a send or parent that would close a cycle. Each
bus has:
- a volume and a mute, which ramp;
- two sends, each ramping from the last block's bus gain times the send's
  last gain to the new pair;
- the snapshots' low-pass, gliding and crossfading as a voice's filters do;
- peak and RMS meters after its gain, readable from any thread
  (`nph_audio_bus_meter`).

**Ducking** lowers a bus by up to `depth_db` while a trigger bus's RMS over
the previous block is above a threshold, with one-pole attack and release
per block. Reading the previous block makes the result independent of the
order of the buses.

**Snapshots** are sets of (bus, volume in dB, low-pass corner). A
snapshot's weight moves linearly to a target over a time. The weighted
decibels of all active snapshots add per bus, and their corners glide from
fully open in log frequency with the weight, the lowest winning.

**The output.** The master is clamped to [-1, 1] as it is interleaved (a
limiter on the master keeps it from getting there: Effects below). While it renders, the mixer sets flush-to-zero and
denormals-are-zero and restores the caller's MXCSR afterwards: a decaying
tail in denormals runs a hundred times slower on x86. `render` takes any
frame count, rendering whole blocks and keeping the rest of the last one.
The output is a function of the commands and the blocks they arrive in,
whatever the sizes of the render calls.

**Real time, verified.** Rendering runs inside a real-time scope.
`test_mixer_realtime.c` plays a busy scene with a hook that counts any
allocation or blocking call, and the count must be zero. The scene has
every codec, starts, stops, promotions and demotions, filters, sends,
delays, ducking, and snapshots. On the Linux sanitizer builds, the
sanitizer allocator's own hooks also count every `malloc` and `free` the
audio thread makes. A threaded run drives the rings from a game thread and
an audio thread at once, under ThreadSanitizer in CI.

## Effects

A bus carries up to four insert effects (ADR-0054), run in order on its
block before its snapshot low-pass, gain, meters, and sends. Each is a
plain DSP object with its own header, tested alone, and hosted by the
mixer (`nph_audio_effect_*`).

**In the mixer.**
- The game thread builds an effect with all its memory and hands over a
  pointer; an update frees it after the audio thread's release event, as
  it does clips.
- Parameters travel one at a time as commands: a type's enum, or
  `NPH_AUDIO_EQ_PARAM(band, field)` for the equaliser. The audio thread
  applies them to its own copy, so nothing it reads is rewritten under it.
- An effect comes in, when made or brought back from bypass, by its input:
  (1 - s) x + E(s x) along a smoothstep, so what its delays and tails make
  starts from silence too. Crossfading its output instead clicked: a 1 ms
  echo's first copy starts as a step 48 frames into the fade.
- An effect goes out, bypassed or destroyed, by crossfading its output to
  the signal without it. A bypassed effect starts over: it comes back
  without its old tail.
- A destroyed effect keeps its place in its bus's count until it is
  released, so a full bus never holds five, even for a block.
- `nph_audio_effect_meter` reads a compressor's or limiter's deepest gain
  reduction.

**The equaliser** (`eq.h`): six bands of any filter type, in series. Each
band's changes glide sample by sample; a band switched on or off
crossfades; a band changed to another type crossfades from its old
section's output to its new one's.

**The compressor** (`dynamics.h`) is Giannoulis, Massberg & Reiss's
feed-forward design (JAES 60(6), 2012):
- the stereo-linked peak level in dB;
- a static curve with a soft knee;
- their smooth branching peak detector on the gain reduction, with an
  attack and a release time constant;
- makeup gain, ramped per block.

**The limiter** (`dynamics.h`) holds the true peak, not just the samples.
Its detector interpolates each channel at the quarter positions between
samples, as ITU-R BS.1770-4 measures true peak, with our own 48-tap
Kaiser-windowed sinc. Each sample's required gain:
1. recovers exponentially at the release rate;
2. is held at its minimum over the lookahead window (a monotonic deque);
3. is averaged over the window.

So when a peak leaves the lookahead delay, the gain has fallen exactly to
what that peak needs, along a ramp. Its latency is the lookahead plus 6
frames, which is why it belongs on the master, made before anything plays.

**The echo** (`echo.h`): per channel, a delay line whose output feeds back
through a damping low-pass and a rumble high-pass, with ping-pong for
stereo. A new delay time crossfades between taps over 50 ms instead of
sweeping the pitch.

**The reverberator** (`reverb.h`; Valimaki et al., IEEE TASLP 2012, for
the lineage):
- **The input:** a pre-delay; eight early reflections (Moorer 1979),
  alternating sides; four Schroeder all-passes for diffusion (Dattorro's
  lengths and gains).
- **The tail:** a 16-line feedback delay network (Jot & Chaigne 1991) of
  mutually prime lengths, mixed by the orthonormal Hadamard matrix (a fast
  Walsh-Hadamard transform).
- **The decay:** per line, a first-order shelf whose gains at DC and
  Nyquist lose exactly 60 dB in the low and the high decay time (Jot 1992).
- **The modulation:** four lines wander slowly, so modes do not ring.
- **The outputs:** each output channel reads the lines through its own
  Hadamard row, so the outputs are uncorrelated; a width blends them.
- **Changes:** decay times glide; a new size or pre-delay crossfades in
  over a block.

Measured: decay times by Schroeder's backward integration within 2% of
their settings (1.523 s for 1.5, 0.601 s for 0.6, 2.995 s for 3.0); echo
density 0.88 to 1.04 from 100 ms on; stereo tails correlated at 0.04.

## 3D

A spatial voice (`NphAudioPlayDesc.spatial`, `spatial.h`, ADR-0055) is
placed by its position and velocity in the listener's frame (+x right, +y
up, -z ahead). The runtime computes them each frame from the engine's
64-bit world positions, so precision holds anywhere. Each block, before
the ranking, the mixer works out from its place:

- **Distance:** an inverse law (OpenAL's "inverse distance clamped",
  physical at a factor of 1), a linear fade, a designer's curve in
  decibels, or none. The default is inverse from 1 m to 50 m. Sends take
  the distance's attenuation only in part (`send_distance`, 0.5), as a
  room's reverberation stays while the direct sound falls.
- **Direction:** the azimuth, through pan.h, with a spread from the
  source's size: the angle it subtends, pi at its surface, widening to all
  around at its centre. A multichannel clip is its own layout turned to
  face the voice, a point far away, opening to its own angles from a
  spread of 60 degrees.
- **Doppler:** c / (c + v_r), with v_r the rate at which the distance
  grows, applied to the resampler's step and clamped to [0.5, 2].
- **The air:** ISO 9613-1 at 20 C and 50% humidity. A one-pole low-pass,
  whose loss grows as the square of the frequency like the air's, has its
  coefficient solved exactly so the digital filter loses 0.0766 dB a metre
  at 8 kHz.
- **Occlusion** (through a wall): the direct sound and the sends lose up
  to 24 dB and their highs down to 700 Hz.
- **Obstruction** (the direct path alone blocked): the direct sound loses
  up to 12 dB and its highs down to 1 kHz on a filter of its own; the
  sends take the voice before it and keep the room.

A voice starts as occluded and obstructed as its play description says
(`occlusion`, `obstruction`), its filters already there in its first
block: a sound that begins behind a wall is dark from its first frame,
and the runtime casts a voice's rays before it plays (runtime.md
"Audio").

Measured: a 1 kHz source receding at a tenth of the speed of sound sounds
at 909.07 Hz (909.09 expected), and approaching at 1111.08 Hz (1111.11).
The air takes 7.66 dB from 8 kHz over 100 m and 0.09 dB from 500 Hz.
Obstruction takes 12.01 dB from the direct path and nothing from the send;
occlusion takes 24.02 dB from both, and a voice played occluded is
24.07 dB down in its first block. A source circling the listener,
re-placed and re-occluded every block, bends the signal less than a still
tone does.

## The device

`device.h` (ADR-0056) plays a mixer on the output device, the one use of
miniaudio (0.11.25, a pinned submodule). Its implementation is a translation
unit of its own, compiled without our warnings, and `device.c` is the only
file that includes it. Decoding, the resource manager, the node graph, and
the engine are compiled out, and so is every backend but WASAPI, PulseAudio
(PipeWire through it), ALSA, and the null device.

- **A callback is one render:** `nph_audio_mixer_render` of the frames the
  system asks for, into its buffer, at the mixer's rate and layout.
  - The channel map comes from the speaker roles.
  - miniaudio neither pre-silences, clips, nor regroups the frames: the
    mixer writes them all, clamped, in any count.
  - On WASAPI the thread joins MMCSS's "Pro Audio" class.
- **The format:** `nph_audio_device_query` reports the default output's own
  rate and channels (as one of the mixer's layouts).
  - Make the mixer at that format and nothing converts at the edge.
  - The device's stats report the hardware's format beside the mix's. A
    difference means the system (WASAPI's resampler) or miniaudio (linear,
    behind an eighth-order low-pass) converts, and the device logs a
    warning when it opens that way.
  - When the default output changes, WASAPI's stream follows it.
- **The null device** asks for periods in real time without hardware, for
  CI and headless runs.
- **Memory:** miniaudio's comes from the device's allocator when it opens,
  each block with a 16-byte header for its size. The callback allocates
  nothing.

## Streaming

`stream.h` (ADR-0056) plays long sounds (music, ambience, dialogue) from
storage. A streamer is a thread that keeps each open stream's ring of
decoded frames ahead of its voice:

- **Reading.** The streamer reads the stream's bytes through its source,
  `read(user, offset, dst, size)`, which the runtime implements over loose
  files or packs.
  - It decodes PCM, or our lossless codec block by block through the cooked
    block offsets, into the ring.
  - It unrolls loops itself, so the voice sees one endless run of frames.
  - The streamer checks each lossless block it reads, its header and CRC
    (`nph_lossless_check_block`), as a clip's blocks are checked when it
    is made: a flipped bit in a file would otherwise play as wrong
    samples, since the decoder alone cannot tell.
  - A read that fails, or a block that fails its checks, ends the stream:
    it plays on as silence, and the streamer counts the one
    (`read_errors`) or the other (`corrupt_blocks`).
- **The ring** holds planar 32-bit frames, a power of two long (one second
  by default: 512 KB for stereo at 48 kHz), between two atomic indices.
  - The streamer writes `produced` and the audio thread `consumed`.
  - Each side publishes its own index with release and reads the other's
    with acquire.
  - The audio thread copies what has arrived and never waits. Frames that
    have not arrived play as silence and count as starvation
    (`stream_starved` in the mixer's stats).
  - The read index moves on regardless, so the stream keeps time. The
    streamer, finding the voice ahead of what it decoded, seeks there.
- **Virtual voices** keep their streams' read indices moving as their time
  passes, so a voice coming back finds its frames there: being virtual saves
  the mixing, not the streaming.
- **A stream plays as a clip.** `nph_audio_clip_create_stream` allocates the
  ring and fills it on the calling thread, so the cost is seen where it is
  paid and a voice played at once starts on sound.
  - The clip plays once, from where the stream was opened (`start_frame`).
    Playing it again is `NPH_RESULT_INVALID_STATE`: make another clip,
    which is also how a stream seeks.
  - `nph_audio_clip_stream_ahead` reads how far the ring is ahead of the
    voice, for a health readout, and `nph_audio_clip_stream_ready` whether
    it holds what a voice needs (at most half the ring) or has nothing more
    to give (its end decoded, or a failure): what a deferred stream's voice
    waits for before it starts, and what rendering faster than real time
    waits for before each render, so its voices never outrun their
    streams.
  - Destroying the clip closes the stream once the audio thread lets go,
    and the streamer frees it on its own thread. The streamer's allocator
    must therefore be thread-safe.
- **Positions.** A stream's voice never wraps its 32.32 position, since the
  stream unrolls the loop. It moves the position back by 2^30 frames, and
  its base on by as much, each time it reaches 2^31, so a looping stream
  plays as long as the game does. Clips in memory hold at most 2^31 frames
  (`NPH_AUDIO_MAX_CLIP_FRAMES`, 12.4 hours at 48 kHz).

A stream plays the same frames its clip would play from memory, to the bit,
looping or not, through the resampler (`test_stream.c`).

## Golden mixes

`tests/audio` renders scripted scenes offline: 1.49 s of stereo at 48 kHz,
commands at given blocks. It compares each with its approved baseline in
`tests/audio/baselines` (Git LFS, 32-bit float WAV):

| Scene | What it holds |
|---|---|
| `tones_and_pans` | two tones, one swelling and fading out, one sweeping left to right; a pluck after a sample-accurate delay |
| `codecs_and_rates` | one 44.1 kHz chord as PCM, lossless, and ADPCM, three pitches, loops that wrap; a pitch glide; filters switched on |
| `buses_ducking_snapshots` | music, dialogue, and effects buses; an effects send; the music ducked under the dialogue; a snapshot darkening and lowering two buses, in and out; a mute |
| `virtual_voices` | 27 voices of two clips at random volumes, pitches, priorities, and pans, some looping, through four real voices, starting and stopping |
| `effects` | music through an equaliser and a compressor, plucks through a ping-pong echo and a room's reverberator, a limiter on the master; a band moved, the echo bypassed, the room shortened, the compressor destroyed |
| `spatial` | a buzz passing from left to right with Doppler and the air; plucks behind a wall that falls away; a chord behind a pillar, obstructed, sending to a room, then round it |

The largest difference from the baseline must be at most 2e-5 of full scale
and the RMS of the difference at most 2e-6. Those limits absorb the
differences, in the last bit, of the C libraries' `sinf`, `powf`, and `tan`
between MSVC, GCC, and Clang, while catching any change a listener could
hear. To stay comparable across compilers, the scripts keep library
functions out of quantities that accumulate: a pitch from `powf` would turn
its last bit into a phase error that grows. A scene that clips fails, since
the output's clamp would hide its gains.

Every CI job compares, the Windows ones included: they fetch the baselines
from LFS. The three MSVC configurations match the baselines bit for bit. A
checkout without LFS sees pointer files, and the tests skip. Renders go to
`<build>/audio_golden/<Config>/`, and CI uploads them when a job fails. A
change that alters a mix fails the test until its renders are approved:
listen to the new renders, look at the difference against the old ones,
then run

    python scripts/audio_golden_approve.py --from <build>/audio_golden/<Config> NAME...

The approval script reports each baseline as new, changed (with its
largest difference), or unchanged.

Reviewing the first renders found two defects:
- the stereo clips' crosstalk, as a 2.2 dB beat between the music's
  detuned channels;
- the filters' switch-on click, a second difference of 0.19 in the codecs
  scene.

Both have unit tests now
(`multichannel_clips_map_speaker_to_speaker`,
`filters_switch_on_and_off_without_a_click`).

## Performance

`audio_bench` (benchmarks/audio), Profile, Ryzen 7 5800X
(`benchmarks/results/audio-2026-10-24-*.json`; budgets in
`benchmarks/budgets.txt`):

| Benchmark | Scalar | SSE4.2 | AVX2 |
|---|---|---|---|
| Mixing with a gain ramp, per sample | 0.27 ns | 0.16 ns | 0.08 ns |
| Resampling one channel (48 taps), per output frame | 31.9 ns | 16.7 ns | 8.5 ns |
| Resampling two channels, per output frame | 62.0 ns | 32.5 ns | 14.9 ns |

| Benchmark | Cost |
|---|---|
| Resampling two channels at a step of 1.5 (80 taps) / 4 (192 taps) | 21.5 / 37.7 ns a frame |
| Peak / sum of squares of a block | 0.04 / 0.06 ns a sample |
| Loading interleaved 16-bit / storing planar to interleaved | 0.29 / 0.18 ns a frame |
| The state-variable low-pass | 3.3 ns a sample |
| A filter's design | 35 ns |
| A Catmull-Rom delay read | 1.8 ns a sample |
| VBAP gains for 7.1 / with spread | 60 / 442 ns |
| Creating the resampler (every bank) | 10.3 ms |
| Lossless decode, 16 or 24-bit stereo, 256 frames at a time | 20.6 ns a frame (0.1% of a core per stream) |
| Lossless encode, 16-bit stereo (the cooker) | 933 ns a frame: 22x real time |
| ADPCM decode / encode, stereo | 4.4 / 340 ns a frame |

A stereo voice resampled from 44.1 kHz to 48 kHz costs 0.7 ms of one core per
second of sound (0.07% of a core); a voice at its source's own rate and pitch
skips the resampler.

The mixer, per 256-frame block of stereo output, with voices looping and
settled (`mixer_block_*`; `benchmarks/results/audio-2026-10-25-*.json`):

| Scene | Per block | Per voice |
|---|---|---|
| 64 voices at the mix rate and pitch 1 (copied) | 11.9 µs | 0.19 µs |
| 64 voices from 44.1 kHz at pitches 0.8 to 1.25, a quarter low-passed | 177 µs | 2.8 µs |
| A game's load: 128 real of 512 (lossless loops, ADPCM effects, 44.1 kHz PCM, filters, sends, three buses, a duck, a snapshot) | 440 µs, 8.3% of a core | 3.4 µs |
| 16 real of 512 (the ranking and the virtual voices) | 59 µs | |

The resampler is two thirds of a resampled voice. A filter in glide costs
3.5 ns a sample against 3.3 steady (`filter_lowpass_glide`). 64 moving 3D
voices (`mixer_block_64_spatial`: Doppler, the air, occlusion, obstruction,
a spread for a quarter) take 230 µs, against 177 µs for 64 plain resampled
ones.

The effects, per stereo frame (`effect_*`):

| Effect | Per frame | Per block |
|---|---|---|
| Six-band equaliser | 40 ns | 10 µs |
| Compressor | 16 ns | 4.2 µs |
| True-peak limiter | 33 ns | 8.4 µs |
| Echo | 14 ns | 3.6 µs |
| Reverberator | 89 ns | 23 µs (0.4% of a core) |

On SSE4.2 the resampler runs channel by channel: two channels' eight
accumulators are sixteen SSE registers, and spilling them made a stereo frame
40.9 ns where two single channels take 32.5. AVX2 keeps the pairs.

Streams (`benchmarks/results/audio-2026-10-25c-*.json`): eight stereo loops
at the mix rate cost the audio thread 44.6 µs a block as lossless clips in
memory (`mixer_block_8_lossless_loops`), and 2.94 µs as streams
(`mixer_block_8_streamed_loops`), whose voices copy from rings that the
streamer has decoded. That is 15 times less. The streamer pays what the
audio thread no longer does, the codec's 20.3 ns a frame. The rings in that
benchmark are long, 32 MB in all, so the voices read them from memory, as a
game's voices do a second after the streamer wrote them.

## Testing

`audio_tests` (engine/audio/tests): the WAV reader and writer (every depth,
the extensible header, loops, refusals, truncations); the lossless codec
(bit-exact round trips over eleven signals at 8, 16, 20, and 24 bits, one to
eight channels, block sizes from 1 to 65535; the stereo modes; incremental
decoding in uneven pieces and skips; determinism; validation of damage; a
decoder fed garbage); ADPCM (quality on tones, noise, and silence; any range
against the whole; channels apart; garbage); the formats, layouts, and
levels; every
kernel at every level against the scalar reference and SSE4.2 against AVX2 to
the bit, with tails, NaNs, clamping, and format extremes; the resampler's
banks, its table against the exact kernel, and its quality on tones; every
filter type against the cookbook, the processing against the response, a low
corner, modulation; the delay line's reads, wrap, and a feedback loop per
block against per sample; and the panners' power, speaker hits, continuity,
and spread. The mixer's tests (`test_mixer.c`) cover:
- gains, pans, and the equal-power law to the bit;
- pitch and clip rates, sample-accurate delays, and starts keeping their
  attack;
- loops, stops, and fades;
- multichannel clips speaker to speaker, on every layout pair that matters;
- filters switching and sweeping without a click;
- buses, sends, and mutes;
- virtual voices resuming in place, and priority over loudness;
- ducking, and snapshots in and out;
- every codec through the mixer;
- clips destroyed while playing, and refusals;
- a full command ring;
- rendering in any pieces to the same bits.

The effects' own tests check the equaliser's bands against their responses
in series and every kind of change for clicks; the compressor's curve,
levels, and time constants; the limiter's ceiling on samples and between
them, its transparency, latency, and ramps; the echo's repeats, ping-pong,
damping, and time changes; and the reverberator's decay times, echo
density, decorrelation, stability, and changes. `test_mixer_effects.c` and
`test_mixer_spatial.c` check the effects and 3D in the mixer, and
`test_spatial.c` the functions against their laws and ISO 9613-1.
`test_mixer_realtime.c` is the real-time proof above, now with every effect,
a third of its voices in 3D, and two streams made again and again, and
`tests/audio` the golden mixes. `test_device.c` renders a mixer on the null
device in real time inside the real-time rules, and plays silence on the
system's device where the machine has one. `test_stream.c` covers:
- streams against the same clips in memory, PCM and lossless, looping and
  not, to the bit;
- starvation as silence that keeps time, and the streamer's seek;
- a virtual voice coming back on sound, to the bit, with nothing starved;
- a looping stream's voice across the rebasing of its position;
- a source failing mid-stream, and a corrupt block, each counted;
- closing while it plays, playing once, and refusals. The real-time scopes have their own tests in core
(`test_realtime.c`: allocations, the spin lock, nesting, the assertion) and
platform (`test_realtime.c`: every blocking call, per thread).
