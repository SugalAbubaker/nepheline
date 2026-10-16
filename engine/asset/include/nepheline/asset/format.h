// SPDX-License-Identifier: Apache-2.0
#pragma once

// The cooked asset container, `.nph` (CLAUDE.md §8.8 "Runtime format:
// versioned header; data is offset-based so it can be memory-mapped and
// used without a parsing step; LZ4-class compression").
//
// Layout (every scalar little-endian):
//
//     NphAssetHeader        64 bytes at offset 0
//     NphAssetBlobEntry[n]  the blob table, right after the header
//     blob data             each blob at its entry's offset, aligned as the
//                           entry says (so an uncompressed blob mapped from
//                           a page-aligned file is usable in place)
//
// A blob is a byte range tagged with a four-character code and an index
// (mip level, stream number). Blobs are either stored or LZ-compressed
// (lz.h); each carries a CRC-32C of its stored bytes. The typed headers of
// meshes, textures, materials, and scenes (mesh.h, texture.h, material.h,
// scene.h) are blobs like any other, stored uncompressed so the loader
// points at them in the mapping.
//
// nph_asset_file_validate is the only entry point that reads an untrusted
// file: it checks the header's magic, version, and checksum, and that every
// blob lies within the file at its stated alignment, so everything after it
// can trust the table. It is fuzzed (tests/fuzz/fuzz_nph.c).
//
// The writer builds a file in memory from blobs it compresses as they are
// added; the cooker (tools/cook) and the editor use it.
//
// Thread-safety: validation and lookup are pure and NPH_THREADSAFE; a
// writer belongs to one thread.

#include <nepheline/asset/asset.h>
#include <nepheline/asset/lz.h>
#include <nepheline/core/alloc.h>
#include <nepheline/core/array.h>
#include <nepheline/core/guid.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_ASSET_MAGIC          NPH_FOURCC('N', 'P', 'H', 'A')
#define NPH_ASSET_FORMAT_VERSION 1U
#define NPH_ASSET_HEADER_SIZE    64U
#define NPH_ASSET_MAX_BLOBS      4096U

// Alignment of blob data by default, and the largest a blob may ask for.
#define NPH_ASSET_BLOB_DEFAULT_ALIGN 16U
#define NPH_ASSET_BLOB_MAX_ALIGN     4096U

// Largest raw size of one blob (bounds the loader's allocations against a
// hostile table).
#define NPH_ASSET_BLOB_MAX_SIZE NPH_GIB(4)

typedef enum NphAssetCodec {
    NPH_ASSET_CODEC_NONE = 0, // stored
    NPH_ASSET_CODEC_LZ = 1,   // lz.h
} NphAssetCodec;

typedef struct NphAssetHeader {
    u32 magic;             // NPH_ASSET_MAGIC
    u16 format_version;    // NPH_ASSET_FORMAT_VERSION
    u16 header_size;       // NPH_ASSET_HEADER_SIZE
    u32 type;              // NphAssetType
    u32 type_version;      // schema version of the typed header blob
    NphGuid guid;          // the asset's identity
    u64 content_hash;      // what it was cooked from (source bytes and settings)
    u32 blob_count;        // entries in the table
    u32 blob_table_offset; // NPH_ASSET_HEADER_SIZE
    u64 file_size;         // bytes in the whole file
    u32 flags;             // reserved, zero
    u32 header_crc;        // CRC-32C of the first 60 bytes
} NphAssetHeader;

NPH_STATIC_ASSERT(sizeof(NphAssetHeader) == NPH_ASSET_HEADER_SIZE, "the header is 64 bytes");

typedef struct NphAssetBlobEntry {
    u32 tag;         // four-character code
    u32 index;       // distinguishes blobs with the same tag (mip, stream)
    u32 codec;       // NphAssetCodec
    u32 alignment;   // of the data in the file and in memory: a power of two, 16..4096
    u64 offset;      // of the stored bytes from the start of the file
    u64 stored_size; // bytes in the file
    u64 raw_size;    // bytes after decompression (== stored_size when NONE)
    u32 crc;         // CRC-32C of the stored bytes
    u32 reserved;    // zero
} NphAssetBlobEntry;

NPH_STATIC_ASSERT(sizeof(NphAssetBlobEntry) == 48U, "a blob entry is 48 bytes");

// What validation learned about a file image: the header (a copy) and the
// table (a pointer into the image, valid while it is).
typedef struct NphAssetFileInfo {
    NphAssetHeader header;
    const NphAssetBlobEntry *blobs;
    u32 blob_count;
} NphAssetFileInfo;

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Validates the file image [data, data + size), which must be 8-byte
// aligned (the table is used in place). NPH_RESULT_CORRUPT for a bad
// magic, header size, checksum, table, or blob range; NPH_RESULT_VERSION_MISMATCH
// for a format version this reader does not know; NPH_RESULT_INVALID_ARGUMENT
// for misaligned data.
NPH_MUST_USE NphResult nph_asset_file_validate(const void *data, u64 size, NphAssetFileInfo *out);

// The entry with `tag` and `index`, or NULL.
const NphAssetBlobEntry *nph_asset_file_find(const NphAssetFileInfo *info, u32 tag, u32 index);

// The stored bytes of `entry` in the image that `info` was validated from.
NPH_INLINE const void *nph_asset_file_blob(const void *data, const NphAssetBlobEntry *entry)
{
    return (const u8 *)data + entry->offset;
}

// True when the stored bytes match the entry's checksum.
bool nph_asset_file_verify_blob(const void *data, const NphAssetBlobEntry *entry);

// Decodes `entry` into `out` (raw_size bytes): a copy for stored blobs, a
// decompression for compressed ones. NPH_RESULT_CORRUPT when the stream
// does not decode to raw_size bytes.
NPH_MUST_USE NphResult nph_asset_file_unpack_blob(const void *data, const NphAssetBlobEntry *entry,
                                                  void *out);

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

typedef struct NphAssetWriterBlob {
    NphAssetBlobEntry entry;
    u8 *data;     // the stored bytes, owned by the writer
    u64 capacity; // bytes allocated for `data` (at least stored_size)
} NphAssetWriterBlob;

typedef NPH_ARRAY(NphAssetWriterBlob) NphAssetWriterBlobArray;

typedef struct NphAssetWriter {
    NphAllocator *alloc;
    NphAssetHeader header;
    NphAssetWriterBlobArray blobs;
    void *lz_workspace; // allocated on the first compressed blob
    u64 lz_workspace_size;
    NphLzLevel lz_level;
} NphAssetWriter;

// Starts a file of `type` with the given typed-header version and identity.
void nph_asset_writer_init(NphAssetWriter *w, NphAllocator *alloc, u32 type, u32 type_version,
                           NphGuid guid, u64 content_hash);

// Releases every blob added so far. Safe on a zero-filled writer.
void nph_asset_writer_free(NphAssetWriter *w);

// The compression level for blobs added with NPH_ASSET_CODEC_LZ (default
// HIGH: the writer runs offline).
void nph_asset_writer_set_lz_level(NphAssetWriter *w, NphLzLevel level);

// Adds a blob (copied; compressed now when `codec` is LZ). `alignment` is
// 0 for the default. NPH_RESULT_ALREADY_EXISTS for a duplicate tag and
// index, NPH_RESULT_INVALID_ARGUMENT for a bad alignment, codec, or size,
// NPH_RESULT_FULL past NPH_ASSET_MAX_BLOBS, NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_asset_writer_add_blob(NphAssetWriter *w, u32 tag, u32 index,
                                                 const void *data, u64 size, NphAssetCodec codec,
                                                 u32 alignment);

// Bytes the finished file will occupy.
u64 nph_asset_writer_size(const NphAssetWriter *w);

// Writes the whole file into `out` (nph_asset_writer_size bytes). The
// writer may keep being used (adding blobs) afterwards.
void nph_asset_writer_finish(NphAssetWriter *w, void *out);

// Allocates the file image from `alloc` and finishes into it. Free with
// NPH_FREE(alloc, data, size, NPH_ASSET_BLOB_MAX_ALIGN).
NPH_MUST_USE NphResult nph_asset_writer_finish_alloc(NphAssetWriter *w, NphAllocator *alloc,
                                                     void **out_data, u64 *out_size);

// Finishes into a file at `path` (created or truncated).
NPH_MUST_USE NphResult nph_asset_writer_write_file(NphAssetWriter *w, const char *path);

#ifdef __cplusplus
}
#endif
