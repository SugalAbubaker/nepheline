# 0017. The asset cooker: derived identities, a JSON database, our own mesh and texture pipelines

- **Date:** 2026-10-14
- **Status:** Accepted

## Context

CLAUDE.md §8.8 asks for an offline cooker, `nph_cook`, that converts
source assets into the runtime format of ADR-0016: meshes from glTF (via
cgltf) with "vertex quantization, vertex cache and overdraw optimization,
meshletization, LOD DAG generation, bounds. All written by us", textures
(via stb_image) with "mip generation in linear space, our own
BC7/BC5/BC4/BC6H encoders, normal-map handling", materials, and scenes;
an asset database with "128-bit GUIDs, a dependency graph, content hashes
for incremental cooking"; and every parser fuzzed (§8.8, §9). §3 confines
cgltf and stb_image to `tools/cook`, offline, never in the shipped
runtime. §1.3 demands zero warnings everywhere, §1.4 no hidden allocation,
§2 deterministic results.

The cooker runs on artists' and build machines, again and again: it has
to be reproducible (the same source gives the same bytes and the same
identities on every machine), incremental (a one-texture change must not
recook a level), and safe on hostile input (content comes from anywhere).
Its outputs feed the GPU-driven renderer of Phases 7 and 10, so mesh
bounds and meshlet cones must be correct for the data the GPU actually
reads. Phase 6 covers meshes, textures, materials, and scenes; the LOD
DAG is Phase 13 (§8.6.9).

## Decision

1. **A library and a thin command-line tool, with the third-party code
   isolated.** `nph::cook` holds everything; `nph_cook` parses arguments.
   cgltf and stb_image are compiled in one translation unit each, with
   their own warning policy and without the engine's sanitizer flags, like
   the VMA unit of ADR-0014; the rest of the cooker includes their headers
   as system headers, and `scripts/check_layering.py` refuses them
   anywhere else. cgltf allocates through the caller's allocator (sized
   blocks with a header, since it frees by pointer alone); stb_image's
   hooks have no context argument, so its blocks come from the system
   allocator and live only inside one decode call.

2. **Identities are derived, not stored.** A source is named by its path
   relative to a source root (its *key*); every asset it produces has a
   sub-asset name (`scene`, `mesh/0`, `material/1`, `image/2/color`) and
   its GUID is `nph_guid_from_cstr("<key>#<sub>")`. References between
   assets are these GUIDs, so a cook is reproducible from any machine and
   working directory with no sidecar files. Outputs mirror the source tree
   (`<key>_<ext>/<kind>_<index>_<name>.nph`), sanitised, with a GUID
   suffix if sanitising ever makes two collide; the same path is the
   asset's virtual path in a pack.

3. **The asset database is a JSON manifest kept with the outputs.**
   `cook.db.json` has one entry per asset: key, sub-asset, GUID, type,
   output, content hash, settings hash, and the GUIDs it references, sorted
   by output and pretty-printed so it diffs in git. It is saved atomically
   (temporary file and rename) and loaded all or nothing. An asset is
   skipped when both hashes match and its output exists. The settings hash
   includes the format and type versions and `COOK_VERSION`, which is
   bumped whenever the cooker's output for an unchanged source changes;
   a texture's content hash is its image's bytes alone. Assets a source no
   longer produces are removed with their files.

4. **The mesh pipeline is the engine's own, in a fixed order:**
   validation and degenerate removal, flat normals where glTF requires
   them, an order-preserving weld, MikkTSpace tangents (by its default
   rules, with glTF's handedness: w = +1 for clockwise UVs, so the
   bitangent points along decreasing v), Forsyth vertex-cache order with
   Sander et al.'s overdraw clustering and fetch order, meshoptimizer-style
   adjacency meshlets (64 vertices, 124 triangles), and quantisation.
   Every bound is computed from the *dequantised* positions, and meshlet
   normal cones are made conservative for the *quantised* axis (the
   cutoff is |a| sin α for the stored axis a, rounded up; docs/cook.md
   derives it). Every step is deterministic.

5. **Textures follow their usage, with our own encoders.** The usage
   (colour, linear, normal, mask, HDR) picks the pipeline and the format
   (BC7 sRGB, BC7, BC5, BC4, BC6H). Mips are exact area averages in linear
   light; normal maps are filtered as vectors and renormalised. The BC7
   encoder ranks all partitions with a cheap integer-moment estimate,
   fully encodes the best few with PCA endpoints, every p-bit combination,
   and least-squares refinement, and keeps the best; its quality is tested
   against a line-fit bound computed in the test. BC6H uses mode 11. Block
   rows encode in parallel on the job system with identical output.

6. **glTF import refuses what it cannot represent and warns about what it
   skips.** An extension the document *requires* and the importer does
   not implement fails the import (`NPH_RESULT_UNSUPPORTED`): skipping it
   would change the asset silently. Features that are only *used* (skins,
   animation, a second UV set, texture transforms) are skipped with a
   warning. A document loaded from memory without a base directory cannot
   reach files.

7. **The decoders are checked against real hardware, and the inputs are
   fuzzed in every build.** An integration test decodes BC blocks on the
   GPU and compares them with the cooker's decoders (exact for BC7 and
   BC6H; within a stated tolerance for BC4/BC5, whose interpolation GPUs
   implement differently). Every fuzz target also builds as a corpus
   replay with deterministic mutations, registered with CTest, so each
   toolchain's sanitizers see the cooker's inputs on every push, not only
   in the nightly libFuzzer runs.

## Alternatives considered

- **Third-party mesh and texture processing (meshoptimizer, the
  reference mikktspace.c, ISPC texcomp, bc7enc, DirectXTex).** Faster to
  adopt and, for BC7, much faster to run. §8.8 and §3 ask for our own
  implementations; they are written from the papers and the format
  specifications, the third-party code serving only as a reference for
  behaviour (the meshlet scoring and MikkTSpace's grouping rules follow
  their originals closely, and say so).
- **Random GUIDs stored in sidecar files (`.meta`, as some engines do).**
  Renaming a source would keep its identity, but every source would carry
  a second file that must be committed, merged, and kept in sync, and a
  lost sidecar silently breaks references. Derived identities need
  nothing stored; the cost is that renaming or moving a source changes its
  GUIDs (below).
- **A binary database.** Smaller and faster to load, but undiffable;
  thousands of entries load in milliseconds as JSON, so the readability
  is free.
- **Cooking on demand in the runtime.** It would put the parsers in the
  shipped executable, against §3 and §10.5 ("no parsing step").
- **Quantising after computing bounds from the source positions.** The
  common approach; but the GPU culls the dequantised positions, which can
  lie outside source-space bounds by half a quantisation step, and a
  meshlet cone computed for the unquantised axis can cull a visible
  triangle. Computing both from what the GPU reads costs nothing.
- **A faster BC7 default (fewer partitions, one pass).** 2.8x faster for
  0.05 dB on the benchmark image. The default favours quality because the
  cooker is incremental and parallel; the settings expose the trade, and
  the encoder's speed is a recorded follow-up.

## Consequences

- Renaming or moving a source gives its assets new GUIDs, and references
  to them from other sources go stale until those are recooked; the
  editor (Phase 9) will handle renames through its asset database
  (redirects), not the cooker.
- The cooker's output is a function of the source, the settings, and
  `COOK_VERSION`; any change that alters output for an unchanged source
  must bump the version (it was bumped to 2 in Phase 6 for the BC7 mode
  4/5 layout fix, flat normals, glTF tangent handedness, the meshlet
  builder, dequantised bounds, and exact-area mips).
- BC7 encoding dominates cook time (108 µs per block at the default
  setting, docs/cook.md); per-endpoint p-bit selection and SIMD are the
  next steps, judged by the same quality tests.
- BC6H mode 11 loses precision at sharp HDR edges; the partitioned modes
  are future work, as are the LOD DAG (Phase 13), skins and animation
  import (Phase 9+), and cooking several sources in parallel.
- Samples and tests cook their content at build or test time with the
  cooker itself, so the cooker is exercised by every CI configuration.
