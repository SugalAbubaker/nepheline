# Asset module (`engine/asset`)

The cooked asset runtime (CLAUDE.md §8.8): the `.nph` container, pack
archives (`.npk`), the virtual file system, the LZ codec, GUIDs, the
loader, and the typed views of meshes, textures, materials, scenes, and
sounds. Decisions: [ADR-0016](adr/0016-asset-format.md),
[ADR-0057](adr/0057-sound-assets.md) (sounds). The offline cooker that
produces these files lives in `tools/cook` ([cook.md](cook.md),
[ADR-0017](adr/0017-cooker.md)).

Depends on `jobs` (asynchronous loads and parallel decompression) and
through it on `platform` (files, memory mapping, the mutex of the loader),
`math` (bounds in the typed headers), and `core`. It does not depend on
`gpu`, nor on `audio`: texture formats, sound codecs, and the like are
pinned numbers that the renderer and the audio runtime map.

| Header | Contents |
|---|---|
| `asset.h` | the log category, asset types, four-character codes |
| `guid.h` | 128-bit identifiers: derived from text, random, text form |
| `lz.h` | the LZ codec: two compression levels, one bounds-checked decoder |
| `format.h` | the `.nph` container: validation, blob lookup, the writer |
| `pack.h` | pack archives: reader (mapped, binary search) and streaming writer |
| `vfs.h` | mounts, path normalisation, reads as copies, mappings, or handles |
| `loader.h` | shared handles, synchronous and asynchronous loads, blob access |
| `mesh.h`, `texture.h`, `material.h`, `scene.h`, `sound.h` | the typed headers and the views that validate them |

## The container (`format.h`)

```
NphAssetHeader        64 bytes: magic "NPHA", format version, type, type
                      version, GUID, content hash, blob count, file size,
                      CRC-32C of the first 60 bytes
NphAssetBlobEntry[n]  48 bytes each: tag, index, codec, alignment, offset,
                      stored size, raw size, CRC-32C of the stored bytes
blob data             at each entry's offset, aligned as the entry says
```

A blob is a byte range named by a four-character tag and an index (a mip
level, a stream number); it is stored as is or LZ-compressed. Alignment is
16 bytes by default and up to a page, and holds both in the file and in
memory: a stored blob mapped from a page-aligned file (or sitting at a
4 KiB-aligned pack entry) is used in place, which is what "no parsing step"
means in practice. The typed headers are stored blobs; bulk data (vertex
streams, mips) is compressed.

`nph_asset_file_validate` is the one function that reads an untrusted
image. It checks the magic, header size, header checksum, format version,
file size, the table bounds, and for every entry: a known codec, a valid
alignment, an offset at that alignment inside the data area, a stored size
within the file, a raw size under 4 GiB, raw equal to stored for stored
blobs, stored within the LZ bound for compressed ones, and no duplicate
tag/index pair. After it, everything is offset arithmetic;
`nph_asset_file_verify_blob` checks a blob's CRC and
`nph_asset_file_unpack_blob` decodes it.

The writer (`NphAssetWriter`) collects blobs, compressing each as it is
added (HIGH level by default; the cooker runs offline), lays them out at
their alignments, and writes the image to memory or to a file. A file is
replaced whole: the image goes to `<path>.part`, which is renamed over the
file. A reader never sees half an asset, and a loader that mapped the old
file keeps its bytes. Rewriting it in place would change the bytes under
the loader, and on Windows it fails while the file is mapped.

## The LZ codec (`lz.h`)

The byte stream is the LZ4 block format (Collet, "LZ4 Block Format
Description", 2011): a sequence of tokens, each a run of literals followed
by a back-reference of at least four bytes at a 16-bit offset; lengths
past 15 extend by bytes of 255; the final sequence carries only literals.
The compressor and decoder are the engine's own. Why that format: a
decoder is a copy loop with no entropy stage, which is what keeps
decompression at memory speed on the loading path (§10.5), and the public
layout can be checked against other implementations.

- **FAST**: one hash-table candidate per position (16 K entries of 4-byte
  hashes), greedy, extended backwards into the pending literals; the probe
  step grows after 64 misses so incompressible input costs little. For
  tools that run often.
- **HIGH**: every position goes into hash chains covering the 64 KiB
  window; up to 64 candidates are compared for the longest match, and a
  match is deferred when the next position starts a longer one (lazy
  matching). The cooker's default.
- **The decoder** checks every read against the input and every write
  against the output and handles overlapping matches (offsets below 8,
  which repeat a short pattern) by copying one period at a time until
  eight bytes of pattern exist, then eight bytes at a time. A stream that
  is malformed, truncated, or decodes to a size other than the one asked
  for yields `NPH_RESULT_CORRUPT`. It is fuzzed (`tests/fuzz/fuzz_lz.c`).

The compressor takes its tables from the caller (`nph_lz_workspace_size`:
64 KiB for FAST, 512 KiB for HIGH); nothing in the codec allocates.

## GUIDs (`guid.h`)

`NphGuid` is two 64-bit words. `nph_guid_from_str` hashes text with two
fixed seeds, so the cooker derives an asset's identity from its source path
(and sub-asset name) and cooks reproducibly; the seeds are part of the
format and pinned by a test. `nph_guid_generate` draws random identifiers
from a generator seeded by the wall clock, the cycle counter, the process
id, and a serial, for assets the editor creates. The text form is 32 hex
digits, high word first; parsing accepts hyphens and upper case.

## Packs (`pack.h`)

```
NphPackHeader       64 bytes: magic "NPAK", version, entry count, entry
                    alignment, table/index/string offsets, file size, CRC
entry data          whole .nph files, each at an aligned offset (4 KiB by
                    default, so an entry can be read or mapped on its own)
NphPackEntry[n]     64 bytes each: GUID, path hash, offset, size, path
                    offset and length, type; sorted by (hash, path bytes)
u32[n]              entry indices sorted by GUID
string table        the paths, NUL-terminated
```

The reader maps the whole file read-only (the OS pages it on demand),
validates every table (ranges, alignment, terminated paths, the sort
orders, no duplicates), and answers `nph_pack_find_path` and
`nph_pack_find_guid` by binary search without allocating. The writer
streams entries to the file as they are added (`nph_pack_writer_add_file`
copies a file in 1 MiB chunks) and appends the tables at
`nph_pack_writer_finish`, sorting with an in-place heapsort so a pack of
any size needs memory only for its table. Duplicate paths and GUIDs, paths
that are not canonical, and null GUIDs are refused at add time.

## The VFS (`vfs.h`)

A VFS is an ordered list of up to 64 mounts; each binds a prefix (`""` or
`a/b/`) to a directory or a pack. Lookups walk the mounts newest first, so
a directory mounted after a pack overrides files in it (editor edits and
patches shadow shipped content). Paths are canonical, produced by
`nph_vfs_normalize_path` (separators to `/`, repeated and leading
separators collapsed, `.` dropped, `..` resolved and refused above the
root), and matched as exact bytes on every platform.

Three ways to read: `nph_vfs_read_all` copies; `nph_vfs_map` gives a
`NphVfsMapping` used in place (a memory-mapped loose file whose handle and
view live in the structure, or a pointer into the pack's mapping with
nothing opened); `nph_vfs_open` gives a file handle and the entry's offset
for positional and asynchronous reads (the pack's shared handle, opened
with `NPH_FILE_ASYNC`, or a loose file the VFS opens). `nph_vfs_iterate`
visits every file under a prefix across mounts, each path once, with the
info of the mount that would serve it; `nph_vfs_resolve_directory` gives
tools the OS path a loose file would be written to.
`nph_vfs_find_guid` resolves an identity to a path: it asks the pack
mounts, newest first, each a binary search of its GUID index, and
returns the entry's path under the mount's prefix (directory mounts have
no identity index; a loose file is found by path).

Cost to know about: when a directory mount shadows a pack, a lookup that
ends in the pack first pays a failed `stat` on the directory (about 30 µs
on Windows). Shipping builds mount packs only; the editor's loads are
deduplicated by the loader, so the cost is paid once per asset.

## The loader (`loader.h`)

`nph_asset_load(loader, path, &handle)` maps the file through the VFS,
validates it, points stored blobs into the mapping, decompresses the rest
into one allocation (aligned as the largest blob asks), and returns a
handle holding one reference. A path already loaded returns the same
handle with one more reference; one still loading is waited for. Handles
are generational (core/handle.h); `nph_asset_get` returns the `NphAsset`
(header, blobs with raw pointers and sizes, path) for a READY handle and
NULL for anything else. Each blob also carries its `offset` in the file:
a stored blob read there through the VFS (`nph_vfs_open`,
`nph_vfs_read_at`) is the same bytes, which is how a sound streams from
the file rather than from the mapping. `nph_asset_release` drops a
reference; the last one unloads. `nph_asset_load_guid` and `nph_asset_load_guid_async` load
by identity, resolving the GUID to a path through `nph_vfs_find_guid`:
that is how a scene's references to meshes and materials, and a
material's to textures, are followed (the cooker derives every GUID from
the source path and sub-asset, [cook.md](cook.md)). Identities that no
mounted pack indexes, such as the loose cooked files of a project in
development, go to the loader's optional resolver
(`NphAssetLoaderDesc.resolve`; the editor answers from the cooker's
database). `nph_asset_find_guid` finds an asset already loaded by
identity, without a new reference. The `NphGuid` type itself lives in
core (`core/guid.h`), so that scene files can hold asset references;
`asset/guid.h` adds the generator seeded from the clocks.

With a job system, `nph_asset_load_async` returns a LOADING handle at
once, runs the mapping and validation on an I/O thread, spawns one job per
blob that needs work (a decompression, or a checksum pass over a stored
blob) on the workers at LOW priority, and the last job to finish publishes
READY or FAILED and signals the counter the caller passed. A synchronous
load with a job system decompresses in parallel the same way when the
work is at least `parallel_threshold` bytes (256 KiB by default), waiting
on the calling thread. A second asynchronous request for an asset in
flight shares the handle and gets its counter signalled by a job that
waits on the first load.

**Replaced files.** A path already loaded returns the asset already
there, even after its file was replaced. `nph_asset_forget` takes an
asset out of the path and GUID indices, so the next load of either reads
the new file into a new asset. Holders of the old one keep it, valid,
until they release it, and a load of it still in flight lands without
taking the GUID back. This is how the editor reloads what it re-cooks
([editor.md](editor.md)).

The pipeline holds its own reference while in flight: a release during a
load unloads when the load completes, and `nph_asset_loader_destroy`
waits for every pipeline before it reports the assets still live as leaks
(with their paths) and frees them. Blob checksums are verified by default
in Debug and Development (`NphAssetLoaderDesc.verify` makes the policy
explicit); a stored blob that fails its CRC, or a compressed one that
does not decode, fails the load with `NPH_RESULT_CORRUPT` and the slot is
released (synchronous) or left FAILED with the error until released
(asynchronous).

Thread-safety: one mutex guards the slot tables for the duration of a
lookup or a state change; the data of a READY asset is immutable and read
without locks. Costs (§1.4): the slot tables at creation; per load the
record, the path copy, the decompression block, and the mapping.

## Typed views

Each asset type defines blobs and a header (all little-endian plain data,
sizes fixed by static assertions) and a view function that resolves them
from a READY asset and checks every count against its blob and every
index and range against the array it addresses, so a view that resolves
can be handed to the renderer without further checks:

- **Mesh** (`mesh.h`, type version 5; earlier versions without voxels still read): `HEAD`
  (counts, bounds, sphere, position quantisation), `POSN` (16-bit unorm
  positions across the bounds, 8 bytes each, separate from the other
  attributes so the visibility-buffer pass of §8.6.4 reads positions
  only), `ATTR` (normal as octahedral snorm16, tangent as snorm8 with the
  handedness in w, half float UV, RGBA8 colour: 16 bytes), `INDX` (u32
  triangle list), `SUBM` (index and meshlet ranges per material slot,
  bounds), `MLET` (meshlets of at most 64 vertices and 124 triangles),
  `MLBD` (sphere and normal cone per meshlet, after Kapoulkine's
  meshoptimizer), `MLVX` (meshlet vertex indices), `MLTR` (meshlet-local
  triangle corners, 3 bytes each). From version 2, a mesh with levels of
  detail (docs/render.md "Levels of detail") adds `LODS` (the levels'
  counts), `LODC` (each cluster's maker group, consumer group, and level),
  `LODG` (the groups), `LODM` (each group's clusters), and `LODN` (the
  hierarchy, node 0 the root); its meshlet blobs then hold every level's
  clusters, level 0's first (the submeshes' meshlet ranges name level
  0's). The records are the renderer's (render/cluster_lod.h), byte for
  byte; the view checks every index they hold and leaves the tree and its
  bounds to the renderer, which checks them when the mesh is created.
  From version 3, `LODS` also counts bricks and voxels, and a mesh whose
  coarser levels hold voxels (docs/render.md "Voxels", ADR-0061) adds
  them; from version 5 compact (render/voxel.h "The compact form",
  ADR-0063), the renderer's records byte for byte: `VOXC` (a voxel
  cluster's record: its voxels' first word, its cells' edge as a power of
  two's exponent, the occlusion and metallic its voxels share, a flag when
  they hold their own; 8 bytes), `VOXB` (bricks: 4 x 4 x 4 cells of their
  cluster's edge, the corner in whole bricks, the first voxel's rank in its
  cluster, a 64-bit occupancy mask; 16 bytes) and `VOXA` (the voxels' words,
  one voxel per set bit in bit order: the footprint's box and shape; the
  normals' lobe, its axis at 12 bits a component or a capsule's at 8 with
  the capsule's code; the albedo and roughness; and in a wide cluster the
  occlusion and metallic: three words, or four). `LODS` counts the records,
  bricks and words. A voxel cluster is a cluster past level 0 whose meshlet
  record has no vertices: its `triangle_count` (1 to 32) bricks from
  `vertex_offset`, its record `VOXC[triangle_offset]`. The view checks each
  such cluster's record (in range, its exponent a normal float's, no flag
  but the wide one, a wide record's shared fields zero), its bricks (masks
  not empty, their first ranks following one another from 0, at most 1,024
  voxels, their words in range), and each voxel's footprint (a box at least
  one step across on each axis, a known shape, a capsule's code at most 56)
  and reserved bits. Versions 3 and 4 kept the builders' records (bricks of
  32 bytes, voxels of 16); such a mesh no longer reads
  (`NPH_RESULT_VERSION_MISMATCH`: cook it again). From version 4, a mesh
  may be voxels alone (a world partition cell's proxy, docs/render.md "Cell
  proxies", ADR-0062): no vertices and no indices (`POSN`, `ATTR`, `INDX`,
  `MLVX` and `MLTR` absent), every cluster a voxel cluster, level 0's too, and
  one submesh of no indices over level 0's clusters. A record without
  vertices at level 0 is a voxel cluster's in such a mesh alone: in a mesh
  with vertices or indices, or before version 4, it is a meshlet's.
- **Texture** (`texture.h`): `HEAD` (extent, mips, layers, format, kind,
  flags) and one `MIP ` blob per level holding every layer, exactly the
  bytes the format and extent imply (whole 4x4 blocks for BC formats).
  Formats: R8, RG8, RGBA8 (UNORM and sRGB), RGBA16F, BC4, BC5, BC6H, BC7
  (UNORM and sRGB).
- **Material** (`material.h`): one `HEAD` with glTF's metallic-roughness
  parameters, alpha mode and cutoff, flags, and five texture references
  (GUID, UV set, wrap modes, filter, scale).
- **Scene** (`scene.h`, type version 2; version 1 still reads): `HEAD`,
  `NODE` (parent-before-child hierarchy with local TRS, child and sibling
  links, names), `INST` (a mesh GUID at a node with a range of material
  GUIDs in `MTRL`), `CAMS`, `LGHT`, and `STRS` (NUL-terminated names).
  From version 2, a scene the cooker partitioned (`nph_cook --hlod`,
  docs/cook.md "Cells") adds `CELL`: per cell of the world partition its
  instances' bounds, their range of `INST` (a cell's instances follow one
  another, the cells' in turn, the instances in no cell after them), and
  its proxy: a mesh GUID (none where it is empty), the material its one
  submesh names, the origin of its space, its finest cell and the
  distance it stands for its instances from (96 bytes; ADR-0062). The
  view checks the ranges (in range, in order, apart), the bounds and
  origins (finite), a proxy's cell (a positive normal power of two) and
  distance (positive, finite), and the reserved words (zero). A version-1
  scene has no cells, whatever its header's reserved word holds.
- **Sound** (`sound.h`): `HEAD` (codec, PCM format, channels, rate,
  frames, the loop, depth, the lossless block size and count, flags:
  `NPH_SOUND_FLAG_STREAM` for one played from storage), `DATA` (the
  samples: PCM, our ADPCM stream, or our lossless codec's blocks; always
  stored, so a stream reads it in the file at the view's `offset`), and
  `BLKS` (a u32 offset in `DATA` per lossless block). The view checks the
  header's ranges, PCM's exact size, and that the lossless blocks tile
  `DATA` in order; the samples themselves are checked by the audio module
  where they are decoded (a clip's every block when it is made, a stream's
  as it reads it), since the asset module does not depend on it
  ([ADR-0057](adr/0057-sound-assets.md)).

## Testing

`asset_tests` (50 cases): GUID derivation (pinned), text round trips and
rejections, generator uniqueness; the codec on empty, tiny, zero,
random, text, periodic (offsets 1–9), window-edge, and structured inputs
at both levels, every size up to 300 bytes, hand-built streams from the
format description, and a dozen malformed streams; the container's
writer against the validator with every corruption the validator must
catch, and a file it replaces while a mapping keeps the old one; packs
written and read back with both alignments, including
duplicates refused, every corruption, and the empty pack; the VFS's path
rules, mount precedence and unmounting, mappings and handles, iteration
with shadowing, identities resolved through pack indexes; the loader
synchronous and asynchronous, loads by GUID through a pack, shared handles,
release during a load, parallel decompression, checksum policies,
failure paths, leak reporting, and forgotten assets (read again while
their holders keep them, also during a load); every typed view on valid and corrupted
data, sounds of each codec among them; a stored blob read back through
the VFS at its offset, from a directory and from a pack. The fuzz seeds,
written by `scripts/fuzz_seeds.py` from an
independent Python description of the formats (with a port of
`nph_hash64` in `scripts/nph_hash64.py`), must validate with the C
readers, so a drift between the description and the code fails the unit
tests.

Fuzz targets (`tests/fuzz`): `lz` (decode arbitrary bytes into several
output sizes, then compress-decompress the input at both levels and
compare), `nph` (validate, checksum, unpack, and resolve every typed
view, touching every byte a sound's view vouches for), `pack` (validate, then look every entry up by path and GUID and
touch its bytes). Besides the nightly libFuzzer runs, each builds as a
CTest smoke test in every configuration (`fuzz_smoke.lz` and so on) that
replays the corpus and a fixed set of mutations under the sanitizers
([cook.md](cook.md), "Fuzzing").

## Benchmarks

`benchmarks/asset/bench_asset.c`, run with `--min-time=0.5 --repeats=7`
in the Profile configuration on the development machine (Ryzen 7 5800X,
Windows 10, MSVC 14.44, 2026-10-15). Full JSON:
`benchmarks/results/asset-2026-10-15-win-msvc-profile-ryzen7-5800x.json`.
The codec rows are nanoseconds per KiB (1000 ns/KiB is about 1 GB/s).

| Benchmark | ns/op | Reading |
|---|---|---|
| lz_compress_fast_text (256 KiB) | 1038 | 1.0 GB/s, 40.9 % of input |
| lz_compress_high_text | 18577 | 55 MB/s, 19.7 % of input |
| lz_decompress_text | 551 | 1.9 GB/s of output |
| lz_compress_fast_vertices | 234 | 4.4 GB/s, 8.1 % of input (HIGH: 6.4 % at 3242 ns/KiB) |
| lz_decompress_vertices | 105 | 9.8 GB/s |
| lz_decompress_zeros | 37 | 27.6 GB/s (one long match) |
| crc32c_blob_256k | 79 | 12.9 GB/s |
| pack_find_path_4k | 102 | hash + binary search over 4096 entries |
| pack_find_guid_4k | 46 | binary search over the GUID index |
| vfs_stat_pack | 80 | one pack mount |
| vfs_stat_shadowed_pack | 21488 | a directory mount consulted first (a failed stat) |
| vfs_normalize_path | 54 | 33 bytes with two separators to fix |
| asset_load_cached | 54 | lookup, reference, release of a loaded asset |
| asset_load_small | 330 | map, validate, release a 512-byte stored asset from a pack |

**Short-offset match copies.** A match whose offset is below 64 bytes is
first grown by doubling copies (each a whole number of the pattern's
periods) until the source lies 64 bytes back; the rest copies 16 bytes a
step. Copying straight from `offset` back made every step load what one of
the last steps stored, a store-to-load forwarding chain: on some hosted CI
CPUs `lz_decompress_zeros` ran at 210–226 ns/KiB against 47–67 on others
(2026-10-15 nightly). Measured here, same session, before → after:

| Variant | text | vertices | zeros |
|---|---|---|---|
| 8-byte steps from `offset` back (before) | 539 | 153 | 46 |
| 8-byte steps, 32/64/128 bytes back | 542–551 | 107–139 | 55 |
| 16-byte steps, 32 bytes back | 548 | 129 | 66 |
| **16-byte steps, 64 bytes back (chosen)** | 551 | 105 | 37 |
| 16-byte steps, 128 bytes back | 616 | 141 | 23 |

The 5800X forwards the 8-byte chain cheaply, so the old copy was already
fast here; a source two 16-byte stores back is slower than that; four is
enough; 128 bytes back doubles more often than short matches repay.

What the numbers say: decompression runs at memory-copy speed, which is
what lets streaming decompress on the job system without a budget line of
its own; the HIGH level trades an 18x slower compressor for a stream half
the size of FAST's on text, the right trade for an offline cooker whose
blobs are compressed in parallel jobs; a pack lookup is a hundred
nanoseconds and a cached load fifty, so resolving a scene's references
costs nothing noticeable; and an uncached load of a pack entry is tens of
microseconds, dominated by the loader's bookkeeping and the mapping
arithmetic rather than I/O, because the pack is already mapped.
