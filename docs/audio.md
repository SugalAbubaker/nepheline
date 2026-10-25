# Audio (`engine/audio`)

The audio module (CLAUDE.md §8.10) mixes the game's sound: voices of clips and
streams, buses with effects, spatialisation around a listener, our own codecs,
and the output device, the one place miniaudio is used. It depends on
`platform` and through it on `math` and `core`. It does not know assets,
entities, or physics: the runtime gives it clips and streams, moves its voices,
and casts its occlusion rays. Decisions:
[ADR-0051](adr/0051-audio-module.md) (module boundaries, real-time scopes, the
signal format, the resampler).

Phase 12 builds it in parts. This document describes what exists:

| Part | Status |
|---|---|
| Real-time scopes (core, platform); the signal format; the block kernels per SIMD level; the resampler; filters; delay lines; panning | done |
| Codecs: PCM, our ADPCM, our lossless codec; WAV | planned |
| The mixer: voices, clips, buses, virtual voices, ducking, snapshots, offline rendering | planned |
| Effects and 3D: EQ, compressor, limiter, delay, reverberation; attenuation, Doppler, air absorption, occlusion | planned |
| The device and streaming | planned |
| The engine: components, the bridge, cooked sounds, the editor, the sample | planned |

| Header | Contents |
|---|---|
| `audio.h` | the log category, limits, sample formats, channel layouts, decibels |
| `kernels.h` | the block kernels: mixing with ramps, meters, format loads and stores, resampling; per SIMD level |
| `resampler.h` | the bandlimited interpolator's kernel banks, steps, and spans |
| `filter.h` | the cookbook's biquads in state-variable form; the one-pole low-pass |
| `delay.h` | the delay line: whole and fractional reads, per block or per sample |
| `pan.h` | equal-power stereo panning, pairwise VBAP, spread |

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
  90, backs at 150); quad is a square.
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
resampler's banks, a delay line's ring) do it once, at creation, on the
caller's thread.

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
coefficients smooth functions of the corner, so the filter stays stable and
quiet when its parameters change every block (a voice's occlusion), and it
keeps its precision at low corners, where a direct form's poles crowd
against the unit circle.

`nph_audio_filter_magnitude` evaluates a section's transfer function exactly.
The tests compare every type with the cookbook's own direct-form formulas
(within 0.0001 dB from 20 Hz to 20 kHz), the processing with the response on
tones (within 0.01 dB), a 20 Hz high-pass at 48 kHz in f32 (-3.01 dB at its
corner, within 0.02 dB), and a resonant low-pass whose corner and Q jump every
64 samples on full-scale noise (bounded, no NaN).

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

A stereo voice resampled from 44.1 kHz to 48 kHz costs 0.7 ms of one core per
second of sound (0.07% of a core); a voice at its source's own rate and pitch
skips the resampler.

On SSE4.2 the resampler runs channel by channel: two channels' eight
accumulators are sixteen SSE registers, and spilling them made a stereo frame
40.9 ns where two single channels take 32.5. AVX2 keeps the pairs.

## Testing

`audio_tests` (engine/audio/tests): the formats, layouts, and levels; every
kernel at every level against the scalar reference and SSE4.2 against AVX2 to
the bit, with tails, NaNs, clamping, and format extremes; the resampler's
banks, its table against the exact kernel, and its quality on tones; every
filter type against the cookbook, the processing against the response, a low
corner, modulation; the delay line's reads, wrap, and a feedback loop per
block against per sample; and the panners' power, speaker hits, continuity,
and spread. The real-time scopes have their own tests in core
(`test_realtime.c`: allocations, the spin lock, nesting, the assertion) and
platform (`test_realtime.c`: every blocking call, per thread).
