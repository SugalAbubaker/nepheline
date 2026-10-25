# 0054. Audio effects: the equaliser, dynamics, echo, and the reverberator

- **Date:** 2026-10-25
- **Status:** Accepted

## Context

CLAUDE.md §8.10 asks for "DSP: biquad filters, delay, compressor, and
reverb (Schroeder 1962; feedback delay network after Jot & Chaigne 1991).
SIMD mixing." The mixer (ADR-0053) has buses, and a game's buses carry
insert effects:
- an equaliser to shape a category;
- a compressor to hold dialogue or music together;
- a limiter so the master never clips;
- echoes and rooms on send buses.

The forces are the mixer's:
- **Nothing on the audio thread allocates or waits.**
- **Every change is smooth.** That includes an effect coming in or going
  out, which the mixer's first draft got wrong (below).
- **Quality is never traded** (the user's decision of 2026-10-20). The
  reverberator must decay at the times it is set to, with a dense tail and
  no metallic ringing. The limiter must hold the true peak, not only the
  samples.

## Decision

1. **Each effect is a plain DSP object, then a bus insert.**
   - `eq.h`, `dynamics.h`, `echo.h`, and `reverb.h` each process planar
     blocks in place. They allocate only in their `init`, with the
     caller's allocator, and are tested alone.
   - The mixer hosts up to four per bus (`NPH_AUDIO_BUS_EFFECTS`), run in
     order before the bus's snapshot low-pass, gain, meters, and sends.
   - The game thread builds an effect with all its memory and hands over a
     pointer. It frees the effect at an update, after the audio thread's
     release event, as it does clips.
   - Parameters travel one at a time as commands (a type's enum, or
     `NPH_AUDIO_EQ_PARAM(band, field)` for the equaliser). The audio thread
     applies them to its own copy, so no mailbox is ever rewritten under
     it.
   - A destroyed effect keeps its place in its bus's count until the
     release arrives. Otherwise a destroy followed by a create could, for
     one block, put five effects on the audio side of a full bus.

2. **Effects come in by their input, and go out by their output.**
   - An effect made, or brought back from bypass, runs on its input faded
     in along a smoothstep while the bus's signal without it fades out:
     (1 − s)·x + E(s·x). Its delays and tails are then made from a signal
     that starts at silence.
   - An effect going out (bypassed or destroyed) crossfades its output to
     the signal without it. A bypassed effect is reset, so it comes back
     without the tail it had.

3. **The equaliser**: six bands of filter.h's sections (any type) in
   series.
   - A band's frequency, Q, or gain glides across a block, sample by
     sample (`nph_audio_filter_process_glide`).
   - A band switched on or off crossfades with a smoothstep.
   - A band changed to another type crossfades from its old section's
     output to its new one's.

4. **The compressor** is Giannoulis, Massberg & Reiss's feed-forward
   design (JAES 60(6), 2012):
   - stereo-linked peak level in dB;
   - a soft-knee static curve;
   - their smooth branching peak detector on the gain reduction, one time
     constant for attack and one for release;
   - makeup gain ramped per block;
   - no logarithm for samples below the knee.

5. **The limiter** is lookahead and true-peak.
   - Its detector interpolates each channel at the quarter positions
     between samples, as ITU-R BS.1770-4 measures true peak, with a 48-tap
     Kaiser (β 6) windowed sinc, 12 taps a phase, designed at init. A
     sample's peak counts the points on both sides of it.
   - The required gain (ceiling over peak) recovers exponentially at the
     release rate, is held at its minimum over the lookahead window (a
     monotonic deque), and is averaged over the window (a running sum, made
     exact again each block). By the time a peak, delayed by the lookahead,
     comes out, the gain has fallen exactly to what it needs, along a
     linear ramp.
   - A final clamp to the ceiling guards against rounding.
   - The latency is the lookahead plus the interpolator's half-width
     (77 frames at 1.5 ms and 48 kHz), so the limiter belongs on the
     master, made before anything plays.

6. **The echo**: a delay line per channel with feedback through one-pole
   damping and high-pass filters, and ping-pong for stereo.
   - A new delay time is reached by crossfading, over 50 ms along a
     smoothstep, from a tap at the old delay to one at the new, rather than
     by sliding the tap and sweeping the pitch.
   - Delays are computed in double precision: 10 ms at 48 kHz is 480
     frames, not 479.99998.

7. **The reverberator** follows the structure of Valimaki et al.'s survey
   ("Fifty Years of Artificial Reverberation", IEEE TASLP 2012):
   - **The input**: pre-delay, then eight early-reflection taps (Moorer,
     CMJ 1979) alternating sides, then four Schroeder all-passes in series
     with Dattorro's lengths and gains (JAES 1997).
   - **The tail**: a feedback delay network of 16 lines of mutually prime
     lengths, 30 to 84 ms at size 1, mixed by the orthonormal 16 × 16
     Hadamard matrix (a fast Walsh–Hadamard transform: 64 additions a
     frame), so the network itself loses nothing.
   - **The decay**: each line passes through a first-order shelf whose DC
     and Nyquist gains lose exactly 60 dB in the low and the high decay
     time (Jot, ICASSP 1992). It is g_low times a bilinear one-pole
     low-pass plus g_high times its exactly complementary high-pass,
     crossing over at a corner.
   - **The modulation**: four lines wander slowly (under 1 Hz, at most
     0.6 ms, read per sample by Catmull-Rom), so the modes do not ring.
   - **The outputs**: each output channel reads the lines through its own
     Hadamard row, so the outputs are mutually uncorrelated, and a width
     blends them towards their mean.
   - **Changes**: the decay times and the crossover glide (50 ms); a new
     size or pre-delay crossfades the new taps in across a block.
   - The network runs a block at a time, in sub-blocks no longer than its
     shortest line.

8. **Verified**:
   - The reverberator's decay, by Schroeder's backward integration (JASA
     1965) of band-limited impulse responses: 1.523 s at a setting of 1.5,
     0.601 s at 0.6, 2.995 s at 3.0, all within 2%.
   - Its echo density (Abel & Huang, AES 2006): 0.88 to 1.04 from 100 ms
     on.
   - The correlation of its stereo tails: 0.04 at width 1, and 1.000 at
     width 0.
   - The limiter: every sample at or under the ceiling for noise 12 dB
     hot, and the true peak of a 12 kHz tone at 45° from +1.70 dBTP to
     exactly −1.000.
   - Every change the effects make, measured as a second difference on a
     steady tone: the tone's own.
   - Effects in the mixer, in order, equal to the same effects alone, to
     the bit.

## Alternatives considered

- **Schroeder–Moorer combs and all-passes** (as in Freeverb). These are
  cheaper, but their tails are metallic and sparse, and their decay is not
  set by frequency. The FDN with absorptive shelves is the design §8.10
  names, and it measures right.
- **A Householder feedback matrix** (O(N), as a Hadamard is O(N log N)).
  For 16 lines it is nearly the identity (2/N off the diagonal), mixes
  slowly, and rings. The Hadamard's full mixing costs 64 additions a frame.
- **Convolution reverberation** gives real rooms from measured responses,
  but at a cost per voice-second that grows with the response's length,
  and with no parametric control. It may come later for content that wants
  it.
- **BS.1770-4's own interpolation coefficients.** They could not be
  checked against the standard here. Our own 48-tap design measures within
  0.13 dB of the true peak at 12 kHz, and the limiter holds its ceiling as
  its own detector measures.
- **Crossfading an effect's output as it comes in.** This was the first
  draft. An effect with an empty delay line starts its delayed copies with
  a step: for a 1 ms echo, half the signal 48 frames into the fade, where
  a smoothstep has only reached 0.1, so it clicks. Fading the input in is
  click-free for any linear effect, and smooth for the compressor.
- **Full parameter structs through mailboxes.** These would race when
  parameters change every frame, which per-field commands avoid.

## Consequences

- Effects cost, per stereo frame (Profile, Ryzen 7 5800X):

  | Effect | Per stereo frame | Per 256-frame block |
  |---|---|---|
  | six-band equaliser | 40 ns | 10 µs |
  | compressor | 16 ns | |
  | true-peak limiter | 33 ns | |
  | echo | 14 ns | |
  | reverberator | 89 ns | 23 µs, 0.4% of a core |

- An effect's latency is its own: only the limiter has one, and it
  belongs on the master.
- In the block in which a limiter comes in, the signal without it is still
  heard: make it before anything plays.
- The compressor has no side-chain input yet (ducking covers the common
  case). A side-chain from another bus's previous block would follow
  ducking's design.
