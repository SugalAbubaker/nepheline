# 0056. Audio output device and streaming

- **Date:** 2026-10-25
- **Status:** Accepted

## Context

CLAUDE.md §8.10 says: "miniaudio is used only to open the output device and
receive buffer callbacks". The audio thread runs our mixer, and "nothing on
the audio thread allocates or locks". It also asks for "streaming for
music".

The user chose our own lossless codec for music, ambience, and dialogue
(ADR-0052). A three-minute stereo song is about 15 MB that way, and a game
has hours of such sound, so long sounds must play from storage, not memory.

The audio module (rank 6) cannot see the asset module's virtual file system
or its packs (§4). The runtime (Phase 12 part F) connects them.

## Decision

1. **miniaudio 0.11.25, for the device alone.**
   - It is a pinned submodule (`third_party/miniaudio`).
   - Its implementation is one translation unit (`miniaudio_impl.c`, its
     own static library, warnings off). `device.c` is the only engine file
     that includes it.
   - Its configuration is on its interface target: no decoding, encoding,
     generation, resource manager, node graph, or engine, and only the
     WASAPI, PulseAudio, ALSA, and null backends.
   - Its memory comes from the device's allocator. miniaudio frees without a
     size, so each block carries its size in a 16-byte header.

2. **A callback is one render.**
   - Each callback calls `nph_audio_mixer_render` for the frames the system
     asks for, straight into its buffer: 32-bit float, interleaved, at the
     mixer's rate and layout. The mixer brings its own real-time scope and
     flush-to-zero.
   - The channel map is explicit, from each channel's speaker role.
   - miniaudio does not pre-silence, clip, or regroup into fixed sizes. The
     mixer writes every frame, clamps, and renders any count.
   - On WASAPI the device's thread joins MMCSS's "Pro Audio" class.
   - The null backend asks for periods in real time without hardware: CI and
     headless runs.

3. **The mixer is made at the hardware's format.**
   - `nph_audio_device_query` reports the default output's own rate and
     channel count (as one of the mixer's layouts). The runtime makes the
     mixer at that format, so nothing converts at the edge.
   - The device's stats report the hardware's format beside the mix's. When
     the hardware forces a conversion (or a device change brings one), the
     caller sees it and can make the mixer again. Meanwhile WASAPI converts
     with its own resampler, or miniaudio with its linear one behind its
     steepest (eighth-order) low-pass.
   - When the default output changes, WASAPI's stream follows it.

4. **Streams decode on a thread of their own into rings of float frames.**
   - A streamer (`NphAudioStreamer`) is a thread outside the job system.
     Its reads may block, and §8.4 keeps blocking I/O off the fiber pool.
   - Each stream has a ring of planar 32-bit frames, a power of two long
     (one second by default), with two atomic indices:
     - `produced`, written by the streamer;
     - `consumed`, written by the audio thread.
     Each side publishes its own index with release and reads the other's
     with acquire.
   - The streamer reads the stream's bytes through its source, a callback
     the runtime implements over loose files or packs. It decodes PCM or our
     lossless codec, block by block through the cooked block offsets.
     ADPCM, for short effects in memory, is refused.
   - The streamer unrolls loops itself, so the voice sees one endless run of
     frames.

5. **The audio thread never waits.**
   - It copies what has arrived.
   - Frames that have not arrived play as silence and count as starvation
     (`stream_starved`).
   - The read index moves on regardless, so the stream keeps time. The
     streamer, finding the voice ahead of what it decoded, seeks there.
   - A virtual voice keeps moving its stream's read index as its time
     passes. The streamer therefore keeps the ring ahead of it, and the voice
     comes back on sound with nothing starved: being virtual saves the
     mixing, not the streaming.

6. **Costs stay where the caller sees them.**
   - Making a stream's clip (`nph_audio_clip_create_stream`) allocates the
     ring and fills it on the calling thread, so a voice played at once
     starts on sound.
   - A stream's clip plays once, from where the stream was opened. Another
     play is `NPH_RESULT_INVALID_STATE`, and to play the stream again the
     game makes another clip, which also seeks (`start_frame`).
   - Destroying the clip closes the stream once the audio thread has let go
     (the clip's release event). The streamer then frees it on its own
     thread, with its allocator, which must therefore be thread-safe.

7. **Positions do not run out.**
   - A voice's position is 32.32 fixed point. Clips in memory are capped at
     2^31 frames (`NPH_AUDIO_MAX_CLIP_FRAMES`, 12.4 hours at 48 kHz).
   - A stream's voice never wraps, since the stream unrolls the loop. When
     its position reaches 2^31 frames, the position moves back by 2^30 and
     the voice's `stream_base` moves on by as much, so a looping stream
     plays for as long as the game runs.

## Alternatives considered

- **Rings of compressed bytes, decoded on the audio thread.** A ring would
  be a quarter of the size. But the audio thread would pay for the decode:
  eight lossless loops take 44.6 µs a block decoded from memory, against
  2.94 µs copied from rings. Its cost would also spike at the codec's block
  boundaries.
- **Streaming on the job system.** Its workers would block in reads. §8.4
  keeps I/O on dedicated threads.
- **Virtual voices that freeze their streams**, seeking when they come
  back. This was the first design. It saves the streaming while a voice is
  virtual, but every return began with silence of up to a streamer period
  plus a read, counted as starvation, which then no longer measured the
  storage alone.
- **Restarting a played stream inside `play`.** The streamer would have to
  decode again from a new origin while the voice waited. That means hidden
  I/O in `play`, or a gap of silence. Making another clip shows the cost.
- **miniaudio's decoders, resampler, or node graph.** §8.10 rules them out,
  and they are compiled out.
- **Our own device layer over WASAPI, PulseAudio, and ALSA.** §3 gives the
  device to miniaudio. It also handles stream routing, formats, and Linux's
  many sound servers.

## Consequences

- **Measured** (Profile, Ryzen 7 5800X):
  - Eight streamed stereo loops cost the audio thread 2.94 µs a block,
    against 44.6 µs for the same loops as lossless clips in memory.
  - The streamer's cost is the codec's decode: 20.3 ns a lossless frame,
    0.1% of a core per stream.
  - The null device delivered 75 callbacks of 256 frames in 400 ms. WASAPI
    granted 480-frame periods at the headset's 48 kHz, with nothing
    converted.
- **Memory:** a second of ring is 512 KB for a stereo stream at 48 kHz.
- **The runtime (part F) must:**
  - query the device, and make the mixer, the device, and a streamer;
  - implement stream sources over the virtual file system;
  - watch the device's stats for a conversion after a device change.
- **Limits:** a stream's voice neither starts part-way nor plays twice. A
  seek is another clip made at `start_frame`.
