# 0058. Audio in the engine: components, the bridge, one-shots, and occlusion

- **Date:** 2026-10-25
- **Status:** Accepted (2026-10-25)

## Context

The audio module mixes, spatialises, decodes, and streams
([ADR-0051](0051-audio-module.md) to [ADR-0056](0056-audio-device-streaming.md)),
and sounds are cooked assets ([ADR-0057](0057-sound-assets.md)); but it knows
no entities, files, or physics (it is beside them in the module order). Games
are written against the ECS: their state is components, and the editor,
scene files, play copies, and hot reload work on components
([ADR-0050](0050-physics-in-the-engine.md) made the same step for physics).
CLAUDE.md §8.10 asks for 3D sound with "occlusion via physics raycasts", and
§10.5 for nothing that hitches a frame: no allocation in steady state, no
I/O on the main thread. The mixer is the main thread's (its commands go
through one single-producer ring), while a game's systems run on the job
system.

## Decision

1. **The engine owns the output** (`audio_bridge.c`, `runtime/audio.h`): the
   mixer, the streamer, and the device, made when the engine starts by
   `audio.device`: `auto` (the system's device with a window, none
   without), `system` (falling back to `null` without a device), `null` (a
   real-time thread without hardware: CI), `offline` (each frame renders
   its own duration on the engine's thread, a step's in lockstep: tests
   and capture; before each render it waits for every playing stream to
   hold half its ring, since it runs faster than real time and would
   otherwise starve its streams by the machine's speed), and `off`. The mix runs at the device's own rate and
   channels, so nothing converts at the edge (ADR-0056).

2. **Two components.** `AudioSource`: a sound by GUID, a bus, volume,
   pitch, loop, `autoplay`, `playing`, 3D or not, the fall-off (inverse,
   linear, a four-point curve in dB, none), size, Doppler, occlusion, a
   reverberation send, priority. `AudioListener`: where 3D sound is heard,
   the first active one; else the host's view (the editor's camera), else
   the first active Camera.

3. **`playing` is the game's switch and the engine's report.** The game
   writes it to start and stop a source; the engine writes it true when
   `autoplay` starts one (when play begins, or when it appears while
   playing) and false when its sound ends or cannot play. It is transient:
   scene files do not keep it.

4. **Changes found by comparison at the sync point**, as the scene renderer
   finds them: a fresh world tick, the chunks whose AudioSource columns were
   written since the last pass, each entity compared with its voice's
   record; the event stream says what went. Volume, pitch, the reverb
   send, and occlusion change a playing voice at once; a new sound plays
   again; what a mixer voice fixes at its start (looping, bus, 3D, fall-off,
   size, Doppler, priority) applies when it next plays.

5. **Placing voices every frame, in 64 bits.** After the view is chosen,
   each 3D voice's entity's world position less the listener's is taken in
   64 bits and rounded to 32 in the listener's axes (§8.2: precision
   anywhere in the world), with the velocity of its motion since the last
   frame, smoothed over 50 ms, for Doppler; a teleport
   (`nph_engine_reset_interpolation`) has none. The bridge keeps the frame's
   time as the audio's: a step's when it renders offline in lockstep.

6. **Occlusion by rays, budgeted.** A source that asks samples its centre
   and four points around it (at its size or 30 cm, its own body ignored)
   through the physics world, as Steam Audio's volumetric occlusion samples
   a sphere round a source. Our probes from the centre drop the points it
   cannot see, which lie in or behind what the source rests against and
   are not part of it; a sight line from the listener to each of the rest
   finds what hides it, every line ending 5 cm short, so the surface a
   source rests on (an impact's) does not. The share hidden is its
   occlusion, followed over 80 ms so walls fade rather than click; but a
   voice's first rays are cast as it starts, and it starts as hidden as
   they say (the mixer's play description takes an occlusion now), since a
   sound's attack is its loudest part. `audio.occlusion_rays` (72 a frame, up to nine a source)
   bounds the cost: the voices starting in a frame first, then round the
   rest; one that starts past the budget starts open and is cast first
   thing the next frame.

7. **One-shots from any thread.** `nph_engine_play_sound` (and
   `nph_game_play_sound`) queues a sound played once, without an entity of
   its own, into a bounded MPMC ring (256 a frame), drained at the sync
   point. It plays in 3D at a position or following an entity. One whose
   sound takes more than a quarter second to load is dropped: late is
   worse than not at all, so `nph_engine_preload_sound` loads ahead (from
   `play_begin`).

8. **No I/O on the main thread for streams.** A sound in memory becomes a
   clip, a copy (the audio thread must never fault on a mapping), and its
   asset is released. A streamed sound keeps its file open (checked against
   the mapped asset's header), and each of its voices gets a stream whose
   ring the streamer fills (`defer_fill`, new in stream.h): the voice starts
   once the ring holds a quarter second, or half a smaller ring
   (`nph_audio_clip_stream_ready`, new in mixer.h), or when two seconds
   pass, starting dry and counted.

9. **Lifetimes by play.** The world's sounds load when play begins (waited
   for: what plays by itself starts on time); others when first named.
   Everything stays loaded until play ends, which stops every voice, waits
   for the streams to close (a second at most), and releases the sounds.

10. **Buses.** Five under the master (effects, music, dialogue, ambience,
    interface), each with a saved volume cvar, and a reverberation bus every
    voice sends to (`audio.reverb`). The game reaches the rest of the mixer
    through `nph_engine_audio_mixer` and `nph_engine_audio_bus`.

## Alternatives considered

- **Audio as a system on the schedule.** It would run on a worker beside
  other systems, but the mixer takes commands from one thread; a lock or a
  per-thread ring per system would follow. At the sync point the world is
  quiescent and the main thread owns the mixer.
- **Voice handles in components.** As with physics bodies, handles do not
  survive scene files, play copies, or migrations; the game controls a
  source through its fields and reaches the mixer for the rest.
- **Restarting a voice when its fall-off changes.** Audible: a designer
  tuning a distance in the inspector would hear the sound start again. The
  change waits for the next play; a mixer command for it is the step to
  take if live tuning is needed.
- **Interpolated positions** (where the scene renderer draws). Sound one
  step late (4.2 ms) is inaudible; the latest positions are exact and free.
- **One ray to the source.** Binary: a source crossing behind a pillar's
  edge snaps. Five points give fractions, smoothed.
- **Points without the probes.** A source resting on a floor or against a
  wall, as most impacts do, has points inside it, and its centre's sight
  line ends on its surface: measured, a source on a floor in plain view was
  heard 20.8 dB down (the test the probes made pass).
- **Smoothing a new voice in from open.** A 50 ms knock behind a wall came
  through 5.6 dB down rather than 24.6: its attack, the part that matters,
  was bright.
- **Streams filled on the main thread** (the stream's default). A second
  of decoding (about 1 ms for lossless stereo) and its reads at the sync
  point, against §10.5's 1 ms.
- **Unloading sounds during play.** Needs to know when every stream that
  reads a sound has closed (the streamer frees them on its own time); it
  comes with world partition, which also says when a region's sounds go.

## Consequences

- A game plays sound by data (an AudioSource in a scene) or by a call from
  any system (a one-shot); the editor edits sources in the inspector and
  shows the mixer, the device, the streams, and the buses' meters and
  volumes in the statistics panel.
- Costs on the Ryzen 7 5800X (Profile;
  `benchmarks/results/runtime-2026-10-25-win-msvc-profile-ryzen7-5800x.json`;
  docs/runtime.md "Performance"): an idle frame with audio about 0.2 µs
  more than one without; 256 moving 3D sources about 70 ns each a frame;
  the occlusion budget's 72 rays 12.8 µs a frame among 10,000 static
  boxes.
- Tests listen: headless engines render offline in lockstep, so they hear
  what the bridge did (direction, distance, walls, volumes, starts and
  ends) frame for frame.
- Still to come: unloading during play, live fall-off changes, obstruction
  from rays (a game sets it through the mixer), propagation round corners,
  more than one listener, HRTF and ambisonics (§8.10 "later").
