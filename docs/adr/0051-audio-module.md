# 0051. Audio: module boundaries, real-time scopes, the signal format, and the resampler

- **Date:** 2026-10-24
- **Status:** Accepted

## Context

CLAUDE.md §8.10 asks for a mixer on its own audio thread that the game thread
drives through a lock-free SPSC queue, with "nothing on the audio thread
allocates or locks"; voices, buses, virtual voices, ducking, and snapshots;
our own WAV/PCM and ADPCM decoding, streaming for music, and a compressed codec
chosen by ADR; distance attenuation, equal-power panning, Doppler, and
occlusion by physics rays; biquads, delay, a compressor, and reverberation
after Schroeder (1962) and Jot & Chaigne (1991), with SIMD mixing; and tests
that render mixes offline and compare them with reference buffers. miniaudio
may open the output device and nothing more (§3). Phase 12 ends when the
offline mix tests pass and the audio thread is verified not to allocate (§12).

This ADR records the decisions of the first part: where the module sits, how
the real-time rule is enforced, the signal format every later part uses, and
the resampler, the costliest kernel and the one whose quality every voice
inherits. The codecs, the mixer, the effects and spatialisation, the device and
streaming, and the engine's integration have their own ADRs.

Constraints from the rest of the plan:

- The module order (§4) puts `audio` at the rank of `asset`, `scene`,
  `physics`, and `profile` (scripts/check_layering.py): it may use `core`,
  `math`, `platform`, and `jobs`, never one of its peers.
- No hidden costs (§1.4): no allocation, I/O, or locking the caller cannot see.
- Scalar and SIMD math are bit-identical and FMA contraction is off (ADR-0008
  §6), so results do not depend on the CPU's ISA level.
- Quality is never traded (the user's decision of 2026-10-20): a design that
  is audibly worse to save time is not an option, and measurements, not
  claims, show that one is not.

## Decision

1. **Audio knows nothing of assets, entities, or physics.** The runtime
   connects them: it turns cooked sounds into the mixer's clips, gives streams
   a file and the range of the data in it, moves voices with their entities,
   and casts the occlusion rays, setting each voice's occlusion. The module is
   then testable offline, without a device, a world, or a file system.

2. **The real-time rule is checked, not trusted.** Core gains real-time scopes
   (`core/realtime.h`): a thread enters one around its deadline-bound work, and
   the places where hidden costs live report a call made inside it: the
   allocation entry points (`alloc.h`), the spin lock, and the platform's
   blocking calls (mutexes, read-write locks, condition variables, semaphore
   waits, joins, one-time initialisation, sleeps and yields, file and
   asynchronous I/O). Non-blocking variants (try-locks, try-waits, posts) are
   allowed. A violation fails an assertion in Debug and Development, or is
   recorded by a hook a test installs; without asserts the scope and the checks
   compile to nothing. The audio thread's render runs in a scope, and the
   mixer's tests also hook the C library's allocator in the sanitizer builds,
   which see every malloc, so code that bypasses the engine's allocators is
   caught too.

3. **The signal format.** Samples are 32-bit floats, planar inside the module
   (a contiguous array per channel) and interleaved only at the edges. The
   mixer works in blocks of `NPH_AUDIO_BLOCK_FRAMES` = 256 frames (5.3 ms at
   48 kHz), and every gain changes as a linear ramp across a block, so no
   change clicks. Layouts are mono, stereo, quad, 5.1, and 7.1 in the WAVE
   channel order, with ITU-R BS.775 and Dolby 7.1 speaker angles.
   Directions are azimuths in the listener's frame of the engine's convention
   (ADR-0008): 0 ahead, positive to the right.

4. **The block kernels are dispatched per ISA level**, on math's pattern
   (ADR-0008 §4, §9): one source compiled at the SSE4.2 baseline and again for
   AVX2, plus a scalar reference for the tests. Both SIMD tables do every
   element's arithmetic in the same order with eight lanes (`NphF32x8`) and no
   FMA, so they agree to the bit; the tests assert it.

5. **The resampler is a bandlimited interpolator** (Smith & Gossett, ICASSP
   1984): the source convolved with a windowed sinc centred on each output's
   position.
   - The kernel: a Kaiser window, beta 10.06, over 24 zero crossings each
     side, 48 taps. Measured (engine/audio/tests/test_resampler.c), the
     passband is flat to within 0.0001 dB up to 0.4 of the output rate
     (19.2 kHz at 48 kHz), a tone comes through with everything else at least
     113 dB below it, and tones the output cannot represent are suppressed by
     at least 115 dB.
   - The table: per fraction of a frame, 16 segments, each tap a cubic in the
     position within its segment, fitted through Chebyshev nodes. Its error
     against the exact kernel is below 1e-7 (the coefficients' own f32
     rounding), and a 48-tap bank is 12 KiB, so it stays in L1. The kernel
     sums the source against each power's row and applies the cubic once per
     output (four dot products, then Horner's rule), which keeps the Horner
     arithmetic out of the tap loop.
   - Playing fast: 25 banks with cutoffs 2^(-k/12) from 1 down to 1/4, each
     stretched and rescaled (up to 192 taps, 670 KiB in all, computed at
     creation in about 10 ms). A step uses the widest bank that cannot alias:
     its cutoff is at most a semitone below the ideal, so the passband ends
     between 0.415 and 0.44 of the mix rate (19.9 to 21.1 kHz at 48 kHz).
     Steps are capped at 4: two octaves up for a source at the mix rate.
   - Positions and steps are 32.32 fixed point, so a voice's position
     accumulates exactly over any length.
   - With AVX2 the kernel costs 8.5 ns per output frame for one channel and
     14.9 ns for two (Profile, Ryzen 7 5800X); a voice that plays at its
     source's rate and pitch is not resampled at all.

6. **Filters are the cookbook's biquads in state-variable form.** The Audio EQ
   Cookbook's sections (Bristow-Johnson) are realised as the
   topology-preserving state-variable filter (Zavalishin 2012; Simper 2013):
   the same transfer functions (the tests check every type against the
   cookbook's formulas, within 0.0001 dB), but stable and free of zipper noise
   when the cutoff moves every block, and precise at low corners. A one-pole
   low-pass (impulse-invariant) smooths and darkens.

7. **Panning keeps power constant.** Stereo pans by the sine of the azimuth
   with the equal-power law (behind folds to in front); quad, 5.1, and 7.1 use
   pairwise VBAP (Pulkki, JAES 1997) over the ring of full-range speakers; a
   source's spread is the sum of 16 virtual sources over its arc (Pulkki,
   WASPAA 1999), normalised to unit power.

8. **Delay lines** are power-of-two rings read before they are written, so a
   feedback loop of a block or more runs a block at a time; shorter loops run
   per sample; fractional reads use the Catmull-Rom spline.

## Alternatives considered

- **Linear or cubic (Hermite) interpolation for the resampler**, as many game
  mixers ship: a few operations per sample, but images and aliases only 30 to
  60 dB down, audible on tonal material pitched up. Rejected on quality.
- **A polyphase table with linear interpolation between phases** (Smith's
  original layout): about 512 phases for the same accuracy, 200 KiB a bank,
  read at random phases: cache misses where the cubic segments stay in L1.
- **Interpolating between two banks** (as OpenAL Soft's bsinc does): an exact
  cutoff at twice the cost of every pitched-up voice. Choosing the bank below
  costs at most a semitone of cutoff, all of it above 19.9 kHz.
- **Resampling every source to the mix rate when cooking.** Not bit-exact
  against the source, which the lossless codec (ADR-0052) exists to keep, and
  the mixer still needs a runtime resampler for pitch and Doppler. It may come
  back as an opt-in cooking setting.
- **Direct-form biquads** (DF1, transposed DF2): the textbook realisation, but
  in f32 a 20 Hz corner at 48 kHz loses its accuracy and coefficient changes
  under modulation produce transients.
- **An interleaved internal format:** every filter and mix kernel would
  shuffle or stride; planar data is what the SIMD kernels and per-channel
  filters want.
- **Verifying the real-time rule by review alone, or by malloc hooks alone:**
  review misses what changes later; malloc hooks see allocations but not
  locks, waits, or I/O, and exist only in sanitizer builds.

## Consequences

- Every later part of Phase 12 inherits a checked real-time rule, the block
  format, and a resampler measured far beyond audibility.
- Core's allocation entry points and the platform's blocking calls carry one
  thread-local check each in Debug and Development; nothing in Profile or
  Shipping.
- A pitched-up voice costs more as its pitch rises (80 taps at 1.5, 192 at 4);
  steps beyond 4 are clamped.
- The resampler's 670 KiB of tables are computed when an audio system is
  created (about 10 ms).
