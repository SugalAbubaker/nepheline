# 0053. The audio mixer: threads, voices, virtual voices, buses, and golden mixes

- **Date:** 2026-10-25
- **Status:** Accepted

## Context

CLAUDE.md §8.10 asks for an audio thread that runs our mixer, with "voices,
buses or submixes, a priority-based virtual voice system, ducking, and
snapshots". The game thread sends commands through a lock-free SPSC queue,
and nothing on the audio thread allocates or locks. Phase 12's exit (§12)
is "offline mix tests pass" and "no audio-thread allocations (verified)".
§9 asks for offline renders compared with reference buffers within a
tolerance.

The forces:

- **A block that is late is a click.** At 48 kHz a 256-frame block lasts
  5.3 ms, and the device gives the mixer a fraction of that. A lock, an
  allocation, or a page fault on the audio thread can cost more than the
  whole block. ADR-0051's real-time scopes check this rule.
- **A game plays more sounds than it can mix**: hundreds of emitters, of
  which a few dozen are audible. The quiet and unimportant ones must cost
  almost nothing, yet come back in the right place when they matter.
- **Every change must be smooth.** A gain step, a filter switched on, a
  voice cut off, or a stereo image folded wrongly is audible.
- **Quality is never traded** (the user's decision of 2026-10-20): voices
  are resampled with ADR-0051's bandlimited interpolator, not linearly.
- **Tests must hear the mix.** The output must be a deterministic function
  of the commands, so that a render can be compared with a baseline.

## Decision

1. **Two threads and two rings.** The game thread owns the mixer. It
   creates clips, buses, and snapshots, plays and steers voices, and calls
   `nph_audio_mixer_update` once a frame. The audio thread calls
   `nph_audio_mixer_render`. The two share:
   - a command ring (game to audio). Commands apply at the start of the next
     block, and `delay_frames` makes a start sample-accurate;
   - an event ring (audio to game): a voice ended, a clip's or a bus's memory
     may go;
   - per-slot mailboxes for payloads larger than a command (a voice's play
     description, a snapshot's entries). These are written before the
     command is pushed and read after it is popped, so the ring's
     release/acquire orders them;
   - statistics and bus meters, as relaxed atomics.

   Nothing else is shared. Each side keeps its own view: the game thread
   mirrors what it sent (handles, bus parents and sends) to validate calls;
   the audio thread keeps the voices, buses, and snapshots.

2. **The audio thread allocates nothing, ever.** `nph_audio_mixer_create`
   allocates everything it will use:
   - voice slots;
   - per-voice contexts (source history, a lossless decoder, filter states)
     for the real voices plus a quarter more, because a voice being demoted
     still fades in its context while its replacement starts in another;
   - bus buffers, ranking keys, and scratch blocks.

   Clips and buses are made on the game thread and handed over by pointer.
   They are freed only after the audio thread's event says it no longer
   uses them. The event ring has room for every slot's event at once, so a
   push can never fail. The command ring can fill: a play then returns
   `NPH_RESULT_FULL`, and every refused command is counted. Rendering runs
   inside a real-time scope, and the tests prove a busy scene clean of
   allocations and locks (point 9).

3. **Voices.** A voice plays a clip, PCM, lossless, or ADPCM (ADR-0052), at
   a volume, a pitch, and a pan, once or looping, from any frame and after
   any delay:
   - **Position.** The position is 32.32 fixed point in clip frames, so a
     voice never drifts. The resampler (ADR-0051) reads it at the clip's
     rate times the pitch. A voice at the mix rate and pitch 1 is copied.
   - **Starts, stops, and changes.** A start at frame 0 keeps its attack. A
     start anywhere else fades in over a block. A stop fades over at least
     a block. Every gain change ramps linearly across a block (ADR-0051).
   - **Filters.** Each voice has a low-pass and a high-pass (§8.10
     occlusion, and the designers' own). A corner moves a third of the way
     (in octaves) to its target each block and glides there sample by
     sample (`nph_audio_filter_process_glide`). A filter is off when fully
     open. The block in which one switches on or off crossfades between the
     signal with it and without it along a smoothstep, so the filter's
     start from silence is not heard.
   - **Ends.** A one-shot plays its clip and then the resampler's ring-out
     (24 frames). A loop repeats its region.

4. **Multichannel clips play speaker to speaker.** A clip channel whose
   speaker the output also has plays on that speaker alone. The layouts
   name their speakers, with 5.1's surrounds as the side pair. So matching
   layouts map channel to channel, and a stereo clip keeps its width on
   every layout. A channel whose speaker the output lacks becomes a virtual
   speaker at its own angle:
   - on stereo it goes to its side at constant power: the front stage's
     ±30° are the pair's ends, sides and backs fold onto them, and the
     centre splits at −3 dB;
   - on more channels it is panned (VBAP) between the speakers that bracket
     it.

   The LFE goes to the LFE or nowhere. A multichannel voice's pan is a
   balance.

5. **Virtual voices by rank.** Each block, every playing voice is ranked by
   its priority, then by its score: its volume, times its fade while fading
   out, times its bus's gain with all its ancestors'. Real voices' scores
   count double, a 6 dB hysteresis so near-equals do not trade places every
   block. The first `max_real_voices` are mixed. The rest are virtual:
   - they advance their position and fade in silence, holding no context;
   - when promoted they take a context and fade in over a block, exactly
     where they would be;
   - a demoted voice fades out over a block and then gives its context back,
     but its own fade keeps running.

   The sort is an in-place heapsort of 64-bit keys (priority, the score's
   bits, slot): no allocation, no recursion, ties broken by slot, so the
   ranking is deterministic.

6. **Buses.** Buses form a tree under the master, mixed children first, in
   an order kept by Kahn's algorithm. The order counts sends as edges, and
   the game thread refuses a send or a parent that would make a cycle. A
   bus has:
   - a volume and a mute (both ramping);
   - two sends, which ramp from the last block's gain times the send's last
     gain to the new pair, so neither change steps;
   - the snapshots' low-pass, gliding and crossfading as a voice's filters
     do;
   - meters: peak and RMS after its gain, published every block.

7. **Ducking and snapshots.**
   - **Ducking** lowers a target bus by up to `depth_db` while its trigger
     bus's RMS over the previous block exceeds a threshold. The reduction
     follows with one-pole attack and release per block. Reading the
     previous block makes the result independent of the bus order.
   - **A snapshot** is a set of (bus, volume in dB, low-pass corner). Its
     weight moves linearly to a target over a given time. Active snapshots
     add their weighted decibels per bus. Their corners glide in log
     frequency from fully open with the weight, and the lowest wins.

8. **The output.** The master is clamped to [−1, 1] when it is
   interleaved, since a limiter comes with the effects (Phase 12 part D).
   Rendering sets flush-to-zero and denormals-are-zero for its duration and
   restores the caller's MXCSR: a filter's or a reverberator's decaying tail
   in denormals runs a hundred times slower on x86. `render` takes any frame
   count, rendering whole blocks and keeping the rest of the last one.

9. **Verification.**
   - **Real time:** `test_mixer_realtime.c` renders a busy scene (voices
     starting, stopping, promoted and demoted, every codec, buses, sends,
     ducking, snapshots) inside a real-time scope with a hook that counts
     any allocation or blocking call. It must count zero. On the Linux
     sanitizer builds, the sanitizer allocator's own hooks also count every
     `malloc` the audio thread makes. A threaded run has a game thread and
     an audio thread driving the rings for 1.8M frames, under TSan in CI.
   - **Determinism:** rendering the same script in pieces of any size gives
     the same bits.
   - **Golden mixes** (`tests/audio`): scripted scenes of 1.49 s are
     rendered offline and compared with approved baselines
     (`tests/audio/baselines`, Git LFS). The largest difference must be at
     most 2e-5 of full scale and the RMS of the difference at most 2e-6.
     That absorbs the C libraries' last-bit differences in `sinf`, `powf`,
     and `tan` between compilers. The scenes avoid feeding a library
     function's result into a quantity that accumulates (a pitch, whose
     error grows as phase), and no scene clips. Every CI job compares,
     Windows included (the baselines are fetched from LFS there too).
     Locally the three MSVC configurations match the baselines bit for bit.

## Alternatives considered

- **A lock around shared state** (the obvious way). Rejected by §8.10 and
  by priority inversion: the game thread holding the lock for a frame's
  work starves the device.
- **Triple-buffered state** instead of commands: the audio thread would read
  the newest copy of every voice. That costs memory traffic proportional to
  all voices every block, and it loses ordering: a play followed by a stop
  in one frame, or a sample-accurate start.
- **Linear or cubic interpolation for voices**, about four times cheaper
  than the 48-tap kernel. Rejected for quality (ADR-0051). The kernel is
  most of a resampled voice's cost (Consequences), and a voice at its
  clip's own rate and pitch skips it.
- **Each multichannel channel as a virtual speaker at its layout angle**
  (the first implementation). On stereo the panner places a source by the
  sine of its azimuth, so a stereo clip's left channel (−30°) landed at pan
  −0.5: 0.92 left and 0.38 right, narrowing every stereo clip. The golden
  buses scene caught it as a 2.2 dB beat between the detuned channels.
- **ITU-R BS.775's down-mix coefficients** (surrounds at −3 dB into their
  side). The mixer keeps constant power per channel, like its panning, so
  a sound panned behind is as loud on stereo as in front.
- **Block-rate filter coefficients.** Each block's step jumped the phase
  response and kinked the output: a second difference of 0.003 on a
  200 Hz tone at −6 dBFS whose own is 0.00034. Per-sample glides bring it
  back to the tone's own.
- **A filter switched on from silence, or crossfaded linearly.** A low-pass
  starting from zero state on a playing signal drops the output to about a
  third for a sample or two: a click (0.19 in the codecs scene). A linear
  crossfade still let 1/256 of that through (a bend 2.5 times the tone's
  own). The smoothstep lets through a hundred-thousandth.
- **Exact (bit-identical) golden comparisons.** These would hold within one
  compiler but break between MSVC, GCC, and Clang over a `sinf` ulp, with
  no audible meaning.

## Consequences

- **Capacities are fixed when a mixer is made:** voices, real voices,
  clips, buses, snapshots, and commands in flight. Asking for more returns
  `NPH_RESULT_FULL`.
- **Controls apply a block late at most**, on top of the device's buffer:
  5.3 ms at 48 kHz.
- **Promoting a lossless voice decodes its block's lead-in** on the audio
  thread: up to 4095 frames, about 80 µs for stereo. This is bounded. The
  real-time test's busy scene, whose 12 real voices out of 64 trade places
  across every codec, logs its slowest block: 135 µs in Development, against
  the block's 5.3 ms.
- **The MXCSR handling is x86's.** ARM64 (§3, later) needs FPCR's FZ bit.
- **Effects and 3D** (part D) plug into the buses and voices. Their
  smoothing must meet the same tests: no step, no click, and golden
  scenes of their own.
- **Measured** (`audio_bench`, Profile, Ryzen 7 5800X), per 256-frame
  stereo block:
  - 64 voices on the copy path: 11.9 µs;
  - 64 resampled voices: 177 µs, about 2.8 µs a voice, two thirds of it
    the resampler;
  - a game's load of 128 real voices out of 512: 440 µs, 8.3% of one
    core;
  - 16 real voices out of 512: 59 µs.

  A filter in glide costs 3.5 ns a sample against 3.3 ns steady.
