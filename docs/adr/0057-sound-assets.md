# 0057. Sound assets and their cooking

- **Date:** 2026-10-25
- **Status:** Accepted (2026-10-25)

## Context

The audio module plays clips from memory and streams from storage
([ADR-0056](0056-audio-device-streaming.md)) in our own codecs
([ADR-0052](0052-audio-codecs.md): PCM, the lossless codec the user chose
for music, ambience, and dialogue, and ADPCM for short effects). It knows
neither files nor assets (it is beside the asset module, ADR-0051). The
engine needs sounds as cooked assets like meshes and textures: named by
GUID, loaded through the VFS from loose files or packs, memory-mapped with
no parsing step (§10.5), validated before use (§8.8: malformed input must
never crash), and made by the cooker from the formats sound designers
deliver. The cooker has to decide, per sound, whether it plays from memory
or streams, and in which codec, without losing a sample unless asked to.

## Decision

1. **A sound asset type** (`asset/sound.h`, `NPH_ASSET_TYPE_SOUND`, type
   version 1): `HEAD` (a 64-byte `NphSoundHeader`: codec, PCM format,
   channels, rate, frames, the loop, depth, the lossless block size and
   count, flags), `DATA` (the samples as the codec writes them), and, for
   the lossless codec, `BLKS` (each block's offset in `DATA`). Codecs and
   formats are pinned numbers the runtime maps to the audio module's enums,
   as texture formats are mapped to the GPU's. `NPH_SOUND_FLAG_STREAM`
   says the sound plays from storage.

2. **`DATA` is stored, never LZ-compressed.** The codecs are the
   compression; a second pass would gain little on coded samples and would
   make a streamed sound's bytes unreadable in place. The view refuses a
   compressed `DATA`.

3. **Streams read the file through the VFS, not the mapping.** Every
   loaded blob now carries its `offset` in the asset's file
   (`NphAssetBlob.offset`, `NphSoundView.offset`), and a stream source
   reads `DATA` there with `nph_vfs_open` and `nph_vfs_read_at` (as vfs.h
   foresaw: "streaming uses the handle"), on the streamer's thread. A read
   that fails then ends the stream in silence and is counted
   (`read_errors`); through the mapping, the same failure would fault in
   the middle of a copy. The loader's checksum pass (Debug and
   Development) still reads a streamed sound once when it loads, off the
   main thread; Shipping touches only the header and the table.

4. **The view checks the structure, the audio module the samples.** The
   asset module may not include the audio module (both are rank 6), so
   `nph_sound_view` checks what it can see: the header's ranges (channels,
   rate, frames, loop, depth per codec, flags, reserved fields), `DATA`'s
   exact size for PCM, and the lossless blocks tiling `DATA` from offset 0
   in order. The samples are checked where they are decoded: a clip checks
   every lossless block's CRC and an ADPCM stream's size and shifts when it
   is made, and a stream checks each block it reads. The view is fuzzed
   with the container (`fuzz_nph.c`, which touches every byte it vouches
   for).

5. **WAV is the source format**, read by the audio module's reader (any
   depth, plain or extensible, the sampler loop). A sound's loop is the
   file's first sampler loop, else the whole sound.

6. **The cooker's choice, per source** (`cook/sound.h`):
   - streaming: AUTO streams sounds of 8 seconds or more (music, ambience,
     long dialogue), ALWAYS and NEVER decide;
   - the codec: AUTO is PCM in memory (nothing to decode as it plays) and
     the lossless codec streamed (about half the bytes to read and to
     store); PCM and LOSSLESS ask for one; ADPCM is asked for, for short
     effects where memory counts more than the last bits, and is refused
     for a streamed sound (the streamer decodes PCM and lossless only).
   Every sample is kept unless ADPCM is asked for: the lossless codec takes
   8- to 24-bit integers at their depth, 32-bit integers whose low byte is
   always zero at 24 bits, and floats that are all 24-bit integers over
   2^23; anything else it cannot hold exactly stays PCM at its own depth,
   and the cooker logs it. 8-bit PCM becomes 16-bit (exactly); 64-bit
   floats become 32-bit ones (logged when that rounds a sample). Samples
   that are not numbers are refused.

7. **Per-source settings in a file beside the source**:
   `<name>.wav.import.json`, a JSON object of `codec`, `stream`,
   `stream_seconds`, and `exhaustive`, over the cooker's defaults (the
   `nph_cook` options `--sound-codec`, `--sound-stream`,
   `--stream-seconds`). Its bytes join the source's content hash, so
   editing it recooks; an unknown key or value fails the source rather
   than being ignored.

## Alternatives considered

- **LZ over `DATA`.** Lossless-coded audio has little redundancy left for
  a byte-level LZ (a few percent), and a compressed blob is decompressed
  whole at load: no streaming, and twice the memory for a sound in
  memory.
- **Streams reading the mapping.** Simpler (a `memcpy` from the mapped
  blob, the page faults taken on the streamer's thread), and it is how
  every other asset is read. But an I/O error becomes a fault, and reads
  happen a page at a time at the OS's pace. With the file handle the
  streamer reads whole blocks and survives a bad read.
- **A separate file for a streamed sound's samples.** Packs, GUIDs, and
  the database would have to know two files per asset; the offset of a
  blob in the one file does the same.
- **Choosing the codec by folder or file name** (everything under
  `music/` streams). Implicit and surprising; the length rule with
  per-source settings says the same thing explicitly.
- **Settings inside the WAV file** (a custom chunk). Sound tools would not
  keep it, and editing the settings would rewrite the samples' file.
- **Lossless by default in memory too.** Half the memory, but a voice
  then decodes on the audio thread (20.3 ns a stereo frame,
  `lossless_decode_stereo16`): affordable, but PCM costs nothing, and the
  user's choice gave PCM and ADPCM to short effects. A source can ask for
  it.
- **Quantising floats to 24 bits for the lossless codec by default.**
  Inaudible (below -144 dB), but not lossless; the cooker keeps them as
  32-bit PCM unless a later option opts in.

## Consequences

- Sounds are assets like the others: cooked by `nph_cook` and the editor's
  importer (any `.wav` it is given, and every `.wav` under a directory it
  cooks), packed, found by GUID, loaded and validated by the loader; the
  runtime (the next part of Phase 12) makes clips and streams of them.
- The streaming contract is tested end to end: a streamed sound cooked
  from a WAV file, loaded from loose files and packs, reads back through
  the VFS at its blob's offset as the bytes the view sees.
- The cooker's output is fuzzed for what it promises (`fuzz_cook_sound.c`):
  whatever WAV it takes, the asset it writes passes the view and the audio
  module's checks, a lossless sound decodes to the source's samples
  exactly, and a PCM one holds them.
- Settings are per source only: a folder of a thousand footsteps that
  should all be ADPCM needs a settings file each, or an `nph_cook` run of
  its own with `--sound-codec adpcm`. Folder-wide settings files are to
  come, with the editor's import settings.
- Only WAV is read. FLAC, Ogg, or AIFF sources would each need a reader
  in the cooker.
