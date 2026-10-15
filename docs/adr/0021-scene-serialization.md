# 0021. Scene files: canonical JSON for the editor, archetype snapshots for the runtime, prefab overrides

- **Date:** 2026-10-15
- **Status:** Accepted

## Context

CLAUDE.md §8.7 asks for scene serialization as "a text format for the
editor (diffable in git), cooked to binary for runtime", and for prefabs
with overrides. §8.8 requires every parser to be fuzzed and malformed input
never to crash. §10.5 wants runtime loads without a parsing step. The
component descriptors of [ADR-0020](0020-ecs.md) give every field's name,
kind, and offset, so both formats can be written generically.

## Decision

1. **The text format is JSON** through core's reader and writer
   (ADR-0007), in one canonical form, so that saving a loaded file writes
   the same bytes:
   - Entities are in hierarchy pre-order: roots in slot order, children in
     their list order. Re-creating them in file order reproduces both the
     slot order and the child order.
   - Ids are the file order. References are ids; references outside the
     saved set are written as null.
   - Components are sorted by name, so the file does not depend on
     registration order. Fields are in declaration order.
   - Floats are written in the shortest text that reads back exactly
     (core's `nph_sb_append_*_shortest`). The f32 form survives a reader
     that parses numbers as doubles. Non-finite values are written as
     "nan", "inf", and "-inf". Vectors stay on one line.
   - TRANSIENT components (WorldTransform) and fields are not written. The
     hierarchy is written as each entity's `"parent"`.
2. **Unknown components are errors and unknown fields are warnings.** A
   file that names a component the program does not register cannot be
   loaded faithfully. A field that no longer exists is dropped with a
   logged warning, so removing a field from code does not strand every
   scene; renaming one loses its value, and migrations are explicit.
3. **Prefab instances are saved as differences.** An instance entity is
   written as `"prefab": {name, index, root}` plus:
   - `"overrides"`: fields that differ from the prefab entity;
   - `"components"`: components the prefab entity lacks;
   - `"removed"`: components it has that the instance lacks.

   ENTITY fields that point inside the instance compare by their position
   in the instance, so an untouched instance writes nothing but its
   reference. Loading rebuilds the instance from the current prefab and
   applies the differences, so prefab edits reach every instance except
   where it overrode them. A save that cannot resolve the prefab writes the
   instance in full and marks it `"full": true`; it then loads exactly as
   written, without the prefab.
4. **Prefabs are flat.** A prefab is a world of its own with a copied
   registry. Nested prefabs (instances inside prefabs) are an editor
   feature for Phase 9.
5. **The binary format is a flat snapshot, grouped by archetype.** It has a
   header, a component table (name, layout hash, size), and one section per
   archetype group: the file ids, then each column contiguous and 16-byte
   aligned. It ends with string-table texts, hierarchy pairs, and a
   CRC-32C trailer. Column bytes are canonical (padding and text tails
   zero, transient fields at their defaults, references as id + 1), so
   equal worlds give equal files. Loading reserves every handle in file
   order, places each group with one batch, and copies runs of rows.
   There is no per-field parsing.
6. **Layout hashes guard the binary format.** A component's hash covers
   its name, size, alignment, and every field's name, kind, and offset. A
   file cooked with other definitions fails with
   NPH_RESULT_VERSION_MISMATCH and must be recooked from its text source.
7. **Both loaders are all or nothing and fuzzed.** Every count, index,
   and reference is validated. Any failure despawns what was created and
   reports where. Both formats accept exactly the same states (text fields
   and string tables must be canonical UTF-8, for example). The fuzz
   targets enforce this: an accepted file must reach a fixed point
   through both formats.

## Alternatives considered

- **Persistent per-entity GUIDs as file ids.** Ids would survive deletions
  in diffs, at the cost of 16 bytes per entity and a component every
  entity carries. File-order ids renumber later entities when one is
  deleted. Diffs show that as changed ids and references, but the
  structure of the change stays readable. This can be revisited with the
  editor (Phase 9) if review practice demands it.
- **A custom text syntax** (YAML-like, terser). The core JSON reader is
  already fuzzed, and JSON tooling is everywhere.
- **Per-component prefab overrides** (replace a whole component when any
  field differs). Simpler, but a prefab change to an untouched field of an
  overridden component would not propagate.
- **Memory-mapping the binary file as the world's chunks.** That would be
  "no parsing" in the strictest sense. But chunk layouts depend on the
  archetype (and on registration), and handles must be allocated. One
  batch spawn and contiguous column copies come within 2x of the copy
  bandwidth (36 ns per entity of 160 bytes).
- **Writing cooked scenes from nph_cook now.** The cooker cannot know a
  game's components until the game module is loadable (Phase 9's runtime
  and hot reload). Until then the conversion is a function call
  (`nph_scene_load_text` + `nph_scene_save_binary`), used by the sample
  and the tests.

## Consequences

- Scene files diff cleanly. Re-saving is a no-op, and vectors and
  overrides are one line each.
- Text saving costs about 3 µs per entity (two C library calls per
  fractional float dominate). Loading costs about 2.7 µs. That is fine
  for editor saves. A Ryu-class formatter would cut the save cost if
  editor scenes grow to hundreds of thousands of entities.
- Changing a component's layout invalidates cooked binaries, by design;
  text sources remain loadable through the unknown-field rule.
- Prefab nesting, cooking integration, and stable entity GUIDs are
  editor-phase work, recorded here so their absence is deliberate.
