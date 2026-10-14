// SPDX-License-Identifier: Apache-2.0
#pragma once

// Pack archives, `.npk` (CLAUDE.md §8.8 "Virtual file system: loose files
// in development, pack archives in shipping").
//
// A pack is a sequence of whole cooked files (`.nph`, format.h) with a
// table at the end that maps paths and GUIDs to byte ranges:
//
//     NphPackHeader          64 bytes at offset 0
//     entry data             each entry's bytes at its offset, aligned to
//                            the header's entry_align (4 KiB by default,
//                            so an entry can be mapped or read on its own
//                            with page-aligned I/O)
//     NphPackEntry[n]        sorted by path hash, then by path bytes
//     u32[n]                 entry indices sorted by GUID
//     string table           the paths, each NUL-terminated
//
// The reader maps the whole pack once (read-only; the OS pages it in on
// demand) and answers lookups by binary search without allocating. Paths
// are compared byte for byte after the hash matches, so a hash collision
// is harmless. The writer streams entries to the file as they are added
// and appends the tables at the end, so a pack of any size is built in
// constant memory plus its table.
//
// Thread-safety: lookups on an open pack are NPH_THREADSAFE (the pack is
// immutable); open and close are single-threaded. A writer belongs to one
// thread.

#include <nepheline/asset/guid.h>
#include <nepheline/core/alloc.h>
#include <nepheline/core/array.h>
#include <nepheline/core/string.h>
#include <nepheline/platform/file.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_PACK_MAGIC               NPH_FOURCC('N', 'P', 'A', 'K')
#define NPH_PACK_FORMAT_VERSION      1U
#define NPH_PACK_HEADER_SIZE         64U
#define NPH_PACK_DEFAULT_ENTRY_ALIGN 4096U
#define NPH_PACK_MAX_ENTRIES         (1U << 24U)
#define NPH_PACK_MAX_PATH            1024U

typedef struct NphPackHeader {
    u32 magic;          // NPH_PACK_MAGIC
    u16 format_version; // NPH_PACK_FORMAT_VERSION
    u16 header_size;    // NPH_PACK_HEADER_SIZE
    u32 entry_count;
    u32 entry_align;         // alignment of every entry's offset: a power of two >= 16
    u64 entry_table_offset;  // NphPackEntry[entry_count]
    u64 guid_index_offset;   // u32[entry_count], entry indices sorted by GUID
    u64 string_table_offset; // the paths
    u64 string_table_size;
    u64 file_size;
    u32 flags;      // reserved, zero
    u32 header_crc; // CRC-32C of the first 60 bytes
} NphPackHeader;

NPH_STATIC_ASSERT(sizeof(NphPackHeader) == NPH_PACK_HEADER_SIZE, "the pack header is 64 bytes");

typedef struct NphPackEntry {
    NphGuid guid;
    u64 path_hash;   // nph_hash64 of the path with the default seed
    u64 offset;      // of the entry's bytes from the start of the pack
    u64 size;        // bytes
    u32 path_offset; // into the string table
    u32 path_len;    // bytes, without the terminator
    u32 type;        // NphAssetType of the cooked file (informational)
    u32 flags;       // reserved, zero
    u64 reserved;    // zero
} NphPackEntry;

NPH_STATIC_ASSERT(sizeof(NphPackEntry) == 64U, "a pack entry is 64 bytes");

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

typedef struct NphPack {
    NphAllocator *alloc;
    NphFile file;
    NphFileMapping mapping;
    NphPackHeader header;
    const NphPackEntry *entries;
    const u32 *guid_index;
    const char *strings;
    char *path; // the pack file's path, for diagnostics (owned)
    u64 path_len;
} NphPack;

// Opens and maps the pack at `path`, validating its tables (every entry
// in range and aligned, every path terminated, the orderings sorted). The
// file is opened for asynchronous reads as well (NPH_FILE_ASYNC), so the
// VFS can hand out positional reads into it. NPH_RESULT_NOT_FOUND,
// NPH_RESULT_CORRUPT, NPH_RESULT_VERSION_MISMATCH, NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_pack_open(NphAllocator *alloc, const char *path, NphPack *out);

// Unmaps and closes. Safe on a zero-filled pack.
void nph_pack_close(NphPack *pack);

// Validates a pack image already in memory (8-byte aligned), filling the
// table pointers of *out (no file, no mapping). What nph_pack_open does
// after mapping; also the fuzz target's entry point.
NPH_MUST_USE NphResult nph_pack_validate(const void *data, u64 size, NphPack *out);

NPH_INLINE u32 nph_pack_entry_count(const NphPack *pack)
{
    return pack->header.entry_count;
}
NPH_INLINE const NphPackEntry *nph_pack_entry_at(const NphPack *pack, u32 index)
{
    return &pack->entries[index];
}

// The entry for `path` (exact bytes), or NULL.
const NphPackEntry *nph_pack_find_path(const NphPack *pack, NphStr path);

// The entry with `guid`, or NULL.
const NphPackEntry *nph_pack_find_guid(const NphPack *pack, NphGuid guid);

// The entry's bytes in the mapping.
NPH_INLINE const void *nph_pack_entry_data(const NphPack *pack, const NphPackEntry *entry)
{
    return (const u8 *)pack->mapping.data + entry->offset;
}

// The entry's path (a view into the string table).
NPH_INLINE NphStr nph_pack_entry_path(const NphPack *pack, const NphPackEntry *entry)
{
    return nph_str(pack->strings + entry->path_offset, entry->path_len);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

typedef struct NphPackWriterEntry {
    NphPackEntry entry;
    char *path; // owned copy
} NphPackWriterEntry;

typedef NPH_ARRAY(NphPackWriterEntry) NphPackWriterEntryArray;

typedef struct NphPackWriter {
    NphAllocator *alloc;
    NphFile file;
    NphPackWriterEntryArray entries;
    u64 cursor; // next free byte in the file
    u64 string_bytes;
    u32 entry_align;
    b32 open;
} NphPackWriter;

// Creates (or truncates) the pack file at `path`. `entry_align` is the
// alignment of every entry's offset (0: NPH_PACK_DEFAULT_ENTRY_ALIGN).
NPH_MUST_USE NphResult nph_pack_writer_begin(NphPackWriter *w, NphAllocator *alloc,
                                             const char *path, u32 entry_align);

// Appends `size` bytes as the entry for `path`. NPH_RESULT_ALREADY_EXISTS
// for a path or GUID already in the pack; NPH_RESULT_INVALID_ARGUMENT for
// an empty, too long, or non-normalised path (vfs.h) or a null GUID;
// NPH_RESULT_IO_ERROR from the file.
NPH_MUST_USE NphResult nph_pack_writer_add(NphPackWriter *w, NphStr path, NphGuid guid, u32 type,
                                           const void *data, u64 size);

// As nph_pack_writer_add, copying the contents of the file at `os_path`
// in chunks (the pack entry need not fit in memory).
NPH_MUST_USE NphResult nph_pack_writer_add_file(NphPackWriter *w, NphStr path, NphGuid guid,
                                                u32 type, const char *os_path);

// Writes the tables and the header and closes the file.
NPH_MUST_USE NphResult nph_pack_writer_finish(NphPackWriter *w);

// Closes the file without finishing (the file is left incomplete) and
// releases the writer. Safe after finish and on a zero-filled writer.
void nph_pack_writer_abort(NphPackWriter *w);

#ifdef __cplusplus
}
#endif
