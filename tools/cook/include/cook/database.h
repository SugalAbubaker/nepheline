// SPDX-License-Identifier: Apache-2.0
#pragma once

// The asset database (CLAUDE.md §8.8 "Asset database: 128-bit GUIDs, a
// dependency graph, content hashes for incremental cooking"): a JSON
// manifest, `cook.db.json`, kept in the output directory. One entry per
// cooked asset records where it came from (source file, relative to the
// cook's source root, and sub-asset name), its GUID, type, output path,
// the hash of the source content and of the settings and cooker version
// it was cooked with, and the GUIDs it references. A source whose hashes
// match and whose output exists is skipped on the next run; the pack step
// reads the entries to know what to pack and how to name it.
//
// The file is pretty-printed JSON sorted by output path, so it diffs
// cleanly in git (§8.8).
//
// Lookups by (source, sub), by GUID, and by output path are hash-map
// lookups; setting an entry is O(1) amortised, removing one O(n).
//
// Thread-safety: a database belongs to one thread.

#include <nepheline/asset/guid.h>
#include <nepheline/core/array.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/string.h>

#include <cook/cook.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_DB_FILE_NAME "cook.db.json"
#define COOK_DB_VERSION   1U

typedef struct CookDbEntry {
    char *source; // source path, relative to the cook root, '/' separators (owned)
    char *sub;    // sub-asset name within the source ("" for the whole file; owned)
    char *output; // output path relative to the output directory (owned)
    NphGuid guid;
    u32 type; // NphAssetType
    u64 content_hash;
    u64 settings_hash;
    NphGuid *deps; // GUIDs this asset references (owned)
    u32 dep_count;
} CookDbEntry;

typedef NPH_ARRAY(CookDbEntry) CookDbEntryArray;

typedef struct CookDb {
    NphAllocator *alloc;
    CookDbEntryArray entries;
    NphHashMap by_key;    // hash of (source, sub) -> entry index
    NphHashMap by_guid;   // NphGuid -> entry index
    NphHashMap by_output; // hash of the output path -> entry index
    b32 dirty;
} CookDb;

NPH_MUST_USE NphResult cook_db_init(CookDb *db, NphAllocator *alloc);
void cook_db_free(CookDb *db);

// Loads `path`; a missing file leaves the database empty and is not an
// error. NPH_RESULT_PARSE_ERROR or NPH_RESULT_VERSION_MISMATCH for a bad
// file (the database is then empty).
NPH_MUST_USE NphResult cook_db_load(CookDb *db, const char *path);

// Writes the database when it changed since the load (or always with
// `force`), through a temporary file renamed over the old one so a crash
// never leaves half a database.
NPH_MUST_USE NphResult cook_db_save(CookDb *db, const char *path, bool force);

// The entry for a source and sub-asset, for a GUID, or for an output
// path; NULL when there is none. The pointers stay valid until the next
// change to the database.
const CookDbEntry *cook_db_find(const CookDb *db, NphStr source, NphStr sub);
const CookDbEntry *cook_db_find_guid(const CookDb *db, NphGuid guid);
const CookDbEntry *cook_db_find_output(const CookDb *db, NphStr output);

// Adds or replaces the entry for (source, sub); an entry that held the
// same GUID under another key (a renamed sub-asset) is replaced too.
// Strings and deps are copied. NPH_RESULT_ALREADY_EXISTS when another
// asset already owns `output`.
NPH_MUST_USE NphResult cook_db_set(CookDb *db, NphStr source, NphStr sub, NphStr output,
                                   NphGuid guid, u32 type, u64 content_hash, u64 settings_hash,
                                   const NphGuid *deps, u32 dep_count);

// Removes the entry for (source, sub); false when there was none.
bool cook_db_remove(CookDb *db, NphStr source, NphStr sub);

// Removes every entry of a source. Returns the number removed.
u32 cook_db_remove_source(CookDb *db, NphStr source);

// True when the entry was cooked from the same content with the same
// settings (the caller checks that the output file still exists).
bool cook_db_entry_is_current(const CookDbEntry *entry, u64 content_hash, u64 settings_hash);

NPH_INLINE u32 cook_db_count(const CookDb *db)
{
    return db->entries.len;
}
NPH_INLINE const CookDbEntry *cook_db_entry(const CookDb *db, u32 index)
{
    return &db->entries.data[index];
}

#ifdef __cplusplus
}
#endif
