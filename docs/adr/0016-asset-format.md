# 0016. Asset runtime: the `.nph` container, pack archives, the VFS, the LZ codec, and the loader

- **Date:** 2026-10-14
- **Status:** Accepted

## Context

CLAUDE.md §8.8 asks for a cooked runtime format with a versioned header
whose data "is offset-based so it can be memory-mapped and used without a
parsing step", compressed with "LZ4-class compression we implement
ourselves"; an asset database keyed by 128-bit GUIDs; a virtual file
system with loose files in development and pack archives in shipping; and
streaming built on async I/O feeding job-based decompression. §10.5 makes
memory-mapped loads a frame-budget requirement ("asset loads are
memory-mapped with no parsing step; streaming must never hitch a frame"),
§1.4 forbids hidden allocation and locking, and §8.8 demands that every
parser be fuzzed and never crash on malformed input.

Phase 6 is "Assets v1": the container, the codec, the loader, the VFS, and
the cooker (ADR-0017). Streaming with residency budgets and the GPU
feedback of virtual texturing are Phase 13; what is decided here must not
have to be undone then.

## Decision

1. **One container for every asset type: a header, a blob table, and
   aligned blobs.** A `.nph` file is a 64-byte header (magic, format
   version, asset type as a four-character code, type schema version,
   GUID, content hash, blob count, file size, CRC-32C of the header), a
   table of 48-byte entries (tag, index, codec, alignment, offset, stored
   and raw size, CRC-32C of the stored bytes), and the blobs at their
   offsets, each aligned as its entry says (16 bytes by default, up to a
   page). Type-specific data (the mesh, texture, material, and scene
   headers and arrays) are blobs like any other, stored uncompressed and
   used in place from the mapping; only bulk data is compressed. Readers
   validate the whole table once (`nph_asset_file_validate`), after which
   every pointer is derived by offset arithmetic and no byte is parsed.
   The typed views (`nph_mesh_view` and friends) check counts against blob
   sizes and every index and range against the array it addresses, so a
   file that passes them cannot send the renderer out of bounds.

2. **The LZ codec is our implementation of the LZ4 block format.** The
   byte stream is exactly LZ4's (Collet, 2011): tokens of literal and
   match lengths, 16-bit offsets within a 64 KiB window, a minimum match
   of four, a literal-only final sequence. The format is the right design
   for a streaming decoder (a copy loop with no entropy stage, which is
   why it reaches memory speed) and, being public, it is verifiable
   against any other implementation. The compressor has two levels: FAST
   (a 16 K-entry hash table, greedy, with the probe-skipping of the
   reference implementation) and HIGH (hash chains over the whole window,
   up to 256 candidates, one step of lazy matching). The decoder is
   bounds-checked on every read and write, so a corrupt stream yields
   `NPH_RESULT_CORRUPT` and never a fault; it is fuzzed. The compressor
   takes its work tables from the caller (§1.4).

3. **GUIDs are 128-bit, derived or random.** `nph_guid_from_str` hashes
   text with two fixed seeds (pinned, like `nph_hash64` itself, because
   cooked references store the result); the cooker derives every asset's
   GUID from its source path and sub-asset name so cooking is
   reproducible. `nph_guid_generate` draws random identifiers for assets
   the editor creates. The all-zero GUID means "none".

4. **Packs are concatenated `.nph` files with a sorted table at the end.**
   Entries are aligned to 4 KiB by default so any entry can be read with
   page-aligned I/O on its own; the table is sorted by path hash (then by
   path bytes, so a hash collision is harmless) with a second index
   sorted by GUID, and a string table holds the paths. The reader maps the
   whole pack read-only and answers lookups by binary search; the writer
   streams entries to disk as they are added and appends the tables, so a
   pack of any size is built in constant memory plus its table. The pack
   file is opened for asynchronous reads too, so the VFS can hand the
   streamer a handle and an offset.

5. **The VFS is an ordered list of mounts, newest first.** A mount binds
   a prefix to a directory or a pack; a lookup takes the first mount
   (most recently mounted) that has the path. Paths are canonical
   (forward slashes, no leading slash, no empty, `.`, or `..`
   components), produced by `nph_vfs_normalize_path`, which refuses to
   climb above the root; matching is exact bytes on every platform so
   case bugs appear on Windows as on Linux. Three read shapes: a copy, a
   mapping used in place (a memory-mapped loose file or a pointer into
   the pack's mapping), and a file handle plus offset for positional and
   asynchronous reads.

6. **The loader shares assets by handle and decompresses on the job
   system.** A load maps the file through the VFS, validates it, points
   stored blobs into the mapping, and decompresses the rest into one
   allocation; an asset loaded twice is one record with a reference
   count; handles are generational (core/handle.h) so a stale one is
   detected. With a job system, `nph_asset_load_async` runs the mapping
   and validation on an I/O thread and one job per compressed blob on
   the workers, and signals the caller's counter when the last finishes;
   a synchronous load with a job system still decompresses in parallel.
   The pipeline holds its own reference while in flight, so a release
   during a load unloads when the load completes, and the loader's
   destructor waits for every pipeline. Blob checksums are verified in
   Debug and Development by default; the policy is explicit per loader.

7. **Formats are pinned by numbers, not by other modules' enums.** The
   texture format and kind, the material alpha mode, the scene light and
   camera types are integers fixed in the asset headers; the renderer
   maps them to GPU formats. The asset module therefore depends on jobs
   (and through it platform, math, core) but not on gpu.

## Alternatives considered

- **A general serialization format (the binary documents of ADR-0007)
  for assets.** The document format is a stream of sections read front
  to back; assets need random access to blobs of arbitrary size at
  arbitrary alignment from a mapping, with per-blob compression and
  checksums. The container shares the document's principles (little-endian
  fixed-width fields, a version-checked header, CRC-32C) but not its
  layout.
- **A custom byte format for the LZ stream.** Nothing in the engine's
  needs is better served by a format of our own; the LZ4 block format is
  already the minimal design, and keeping its exact byte layout means the
  stream can be checked with other tools.
- **Zstandard-class compression (entropy coding).** Better ratios, but
  decompression at a fraction of the speed and an order of magnitude more
  code; the ratio difference on vertex data and BC-compressed textures
  (already dense) is small. Revisit with measurements in Phase 13 if disk
  bandwidth, not CPU, turns out to be the streaming limit.
- **Per-file compression of whole assets.** Simpler, but the loader would
  have to decompress a texture's every mip to read its header, and the
  streaming of §8.8 needs the small mips without the large ones. Per-blob
  compression lets the typed headers stay uncompressed and in place.
- **Reference-counted shared ownership through pointers instead of
  handles.** Pointers cannot detect use after release; the engine's rule
  (§2 "handles over pointers") applies to assets like everything else.
- **Case-insensitive path matching in the VFS.** It would hide
  Windows-only content bugs until the Linux build; exact matching makes
  them fail everywhere, and the cooker writes paths as authored.

## Consequences

- The cooker (ADR-0017) produces exactly these files and packs them; the
  renderer's mesh and texture uploads read the typed views directly.
- Any change to a typed header is a new type version; any change to the
  container or pack layout is a new format version. Readers reject
  versions they do not know (`NPH_RESULT_VERSION_MISMATCH`) rather than
  guess.
- Phase 13's streamer uses `nph_vfs_open` (a handle and an offset) with
  the asynchronous I/O of ADR-0011 and the per-mip blobs; the loader's
  pipeline shape (I/O stage, per-blob jobs, a counter) is what it extends.
- The fuzz corpus for the readers is generated by an independent Python
  description of the formats (`scripts/fuzz_seeds.py`), which the unit
  tests validate with the C readers so the two can never drift apart.
- `.nph` and `.npk` are Git LFS patterns; checked-in test data for these
  formats therefore carries no extension.
