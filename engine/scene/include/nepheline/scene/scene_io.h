// SPDX-License-Identifier: Apache-2.0
#pragma once

// Scene serialization (CLAUDE.md §8.7 "Scene serialization: a text format
// for the editor (diffable in git), cooked to binary for runtime";
// docs/scene.md "Scene files"; docs/adr/0021-scene-serialization.md).
//
// Both formats are driven by the component descriptors (component.h): a
// component is written field by field from its X-macro, so a new field or
// component needs no serialization code.
//
// The text format is JSON (core/json.h), one object per entity:
//
//     {
//       "nepheline_scene": 1,
//       "entities": [
//         {"id": 0, "components": {"LocalTransform": {...}, "Name": {"text": "root"}}},
//         {"id": 1, "parent": 0, "components": {...}},
//         {"id": 2, "prefab": {"name": "crate", "index": 0, "root": 2},
//          "overrides": {"LocalTransform": {"translation": [4, 0, 0]}},
//          "components": {"Health": {...}}, "removed": ["Light"]}
//       ]
//     }
//
// It is written to diff well: entities in hierarchy pre-order (roots in
// slot order, children in their order), ids numbered in that order,
// components sorted by name, fields in declaration order, floats in the
// shortest text that reads back exactly, pretty-printed one value per
// line. Saving a loaded file writes the same bytes. ENTITY fields hold
// ids; references to entities outside the saved set are written as null.
// TRANSIENT components and fields are not written (they load as their
// defaults; WorldTransform is recomputed by the transform system), and the
// hierarchy is written as each entity's "parent".
//
// Prefab instances (prefab.h) are written as the prefab entity they came
// from plus their differences: changed fields under "overrides",
// components the prefab entity lacks under "components", and components
// it has that the instance lacks under "removed". Loading rebuilds the
// instance from the current prefab and applies the differences.
//
// The binary format is what the runtime loads: a flat snapshot (prefab
// instances are stored expanded), grouped by archetype with each
// component's column stored contiguously, so loading is one batch spawn
// and one copy per column. A component table with layout hashes guards
// against loading data cooked with other component definitions
// (NPH_RESULT_VERSION_MISMATCH), and a CRC-32C trailer against corruption.
// Every count and index is validated: both readers are fuzzed (§8.8
// "every parser is fuzzed").
//
// Loading is all or nothing: on failure, every entity created so far is
// despawned and the result's `error` says what was wrong and where.
//
// Thread-safety: saving reads the world (concurrent reads are fine);
// loading changes it (one thread, outside schedule stages).

#include <nepheline/core/serialize.h>
#include <nepheline/core/string.h>
#include <nepheline/scene/prefab.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_SCENE_TEXT_VERSION   1U
#define NPH_SCENE_BINARY_VERSION 1U
// 'N' 'S' 'C' 'B' in file order.
#define NPH_SCENE_BINARY_MAGIC 0x4243534EU

#define NPH_SCENE_ERROR_MAX 192U

typedef struct NphSceneSaveDesc {
    // The entities to save (duplicates and dead entities are ignored);
    // NULL saves every entity of the world.
    const NphEntity *entities;
    u32 entity_count;
    // Text: finds prefabs to write instances as differences. Without it
    // (or for a prefab it does not find), instances are written in full,
    // still marked as instances.
    NphPrefabResolveFn resolve;
    void *resolve_user;
    // Text: one line per entity instead of one per value.
    b32 compact;
    // Scratch memory for tables (NULL: the system allocator).
    NphAllocator *scratch;
} NphSceneSaveDesc;

typedef struct NphSceneLoadDesc {
    // Text: finds the prefabs that instances name. Required when the file
    // has instances.
    NphPrefabResolveFn resolve;
    void *resolve_user;
    // Scratch memory for the parsed document and tables (NULL: the
    // system allocator).
    NphAllocator *scratch;
    // Receives the created entities in file order (up to out_capacity).
    NphEntity *out_entities;
    u32 out_capacity;
} NphSceneLoadDesc;

typedef struct NphSceneResult {
    u32 entity_count;                // entities written or created
    char error[NPH_SCENE_ERROR_MAX]; // "" on success
} NphSceneResult;

// Appends the text form of the world (or of desc->entities) to `out`.
// `desc` and `result` may be NULL. NPH_RESULT_OUT_OF_MEMORY on failure.
NPH_MUST_USE NphResult nph_scene_save_text(const NphWorld *world, const NphSceneSaveDesc *desc,
                                           NphStrBuilder *out, NphSceneResult *result);

// Creates the entities of a text scene in `world` (in addition to those it
// has). NPH_RESULT_PARSE_ERROR for malformed JSON or a malformed scene,
// NPH_RESULT_VERSION_MISMATCH for another format version,
// NPH_RESULT_NOT_FOUND for an unknown component, entity id, or prefab,
// NPH_RESULT_TYPE_MISMATCH or NPH_RESULT_OVERFLOW for a value that does not
// fit its field, NPH_RESULT_FULL or NPH_RESULT_OUT_OF_MEMORY when the world
// runs out. `desc` and `result` may be NULL.
NPH_MUST_USE NphResult nph_scene_load_text(NphWorld *world, NphStr text,
                                           const NphSceneLoadDesc *desc, NphSceneResult *result);

// Appends the binary form to `out` (resolve and compact are ignored).
NPH_MUST_USE NphResult nph_scene_save_binary(const NphWorld *world, const NphSceneSaveDesc *desc,
                                             NphBinWriter *out, NphSceneResult *result);

// Creates the entities of a binary scene in `world`. NPH_RESULT_CORRUPT for
// a damaged or malformed file, NPH_RESULT_VERSION_MISMATCH for another
// format version or a component whose layout differs from the world's,
// NPH_RESULT_NOT_FOUND for a component the world does not register,
// NPH_RESULT_FULL or NPH_RESULT_OUT_OF_MEMORY when the world runs out.
NPH_MUST_USE NphResult nph_scene_load_binary(NphWorld *world, const void *data, u64 size,
                                             const NphSceneLoadDesc *desc, NphSceneResult *result);

#ifdef __cplusplus
}
#endif
