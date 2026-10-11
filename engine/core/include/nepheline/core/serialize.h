// SPDX-License-Identifier: Apache-2.0
#pragma once

// Binary serialization with versioning (CLAUDE.md §8.1 "a binary
// writer/reader with versioning").
//
// The writer appends little-endian fixed-width scalars, byte blobs, and
// length-prefixed strings to a growable buffer and can back-patch earlier
// positions. The reader is a cursor over an immutable byte range: every
// read reports NPH_RESULT_END_OF_STREAM instead of running past the end,
// which is what makes the format safe to fuzz and to load from untrusted
// files.
//
// Document layout convention:
//
//     header   { u32 magic; u32 version; }           nph_bin_write_header
//     section* { u32 tag; u32 size; u8 payload[size]; } nph_bin_write_section_*
//     trailer  { u32 crc32c of everything before }  nph_bin_write_checksum
//
// Readers that know a tag parse its payload; unknown or newer sections
// are skipped by size, which is how old readers survive new writers and
// what "versioning" means here beyond the header's version range check.
//
// Thread-safety: a writer or reader belongs to one thread.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/core/string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

typedef struct NphBinWriter {
    u8 *data;
    u64 len;
    u64 cap;
    NphAllocator *alloc;
} NphBinWriter;

void nph_bin_writer_init(NphBinWriter *w, NphAllocator *alloc);
void nph_bin_writer_free(NphBinWriter *w);
NPH_MUST_USE NphResult nph_bin_writer_reserve(NphBinWriter *w, u64 cap);
void nph_bin_writer_clear(NphBinWriter *w);

NPH_INLINE const u8 *nph_bin_writer_data(const NphBinWriter *w)
{
    return w->data;
}
NPH_INLINE u64 nph_bin_writer_len(const NphBinWriter *w)
{
    return w->len;
}

NPH_MUST_USE NphResult nph_bin_write_u8(NphBinWriter *w, u8 v);
NPH_MUST_USE NphResult nph_bin_write_u16(NphBinWriter *w, u16 v);
NPH_MUST_USE NphResult nph_bin_write_u32(NphBinWriter *w, u32 v);
NPH_MUST_USE NphResult nph_bin_write_u64(NphBinWriter *w, u64 v);
NPH_MUST_USE NphResult nph_bin_write_i8(NphBinWriter *w, i8 v);
NPH_MUST_USE NphResult nph_bin_write_i16(NphBinWriter *w, i16 v);
NPH_MUST_USE NphResult nph_bin_write_i32(NphBinWriter *w, i32 v);
NPH_MUST_USE NphResult nph_bin_write_i64(NphBinWriter *w, i64 v);
NPH_MUST_USE NphResult nph_bin_write_f32(NphBinWriter *w, f32 v);
NPH_MUST_USE NphResult nph_bin_write_f64(NphBinWriter *w, f64 v);
NPH_MUST_USE NphResult nph_bin_write_bool(NphBinWriter *w, bool v); // one byte
NPH_MUST_USE NphResult nph_bin_write_bytes(NphBinWriter *w, const void *data, u64 len);
// u32 length followed by the bytes (no terminator).
NPH_MUST_USE NphResult nph_bin_write_str(NphBinWriter *w, NphStr s);
// Zero bytes until the length is a multiple of `align` (power of two).
NPH_MUST_USE NphResult nph_bin_write_align(NphBinWriter *w, u32 align);

// Overwrites a u32 written earlier at `offset` (for sizes and offsets).
void nph_bin_patch_u32(NphBinWriter *w, u64 offset, u32 v);
void nph_bin_patch_u64(NphBinWriter *w, u64 offset, u64 v);

// Header, sections, and checksum per the layout above.
NPH_MUST_USE NphResult nph_bin_write_header(NphBinWriter *w, u32 magic, u32 version);

typedef struct NphBinSection {
    u64 size_offset; // where the u32 size lives
} NphBinSection;

NPH_MUST_USE NphResult nph_bin_write_section_begin(NphBinWriter *w, u32 tag, NphBinSection *out);
void nph_bin_write_section_end(NphBinWriter *w, const NphBinSection *section);

// Appends the CRC-32C of everything written so far.
NPH_MUST_USE NphResult nph_bin_write_checksum(NphBinWriter *w);

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

typedef struct NphBinReader {
    const u8 *data;
    u64 len;
    u64 pos;
} NphBinReader;

NPH_INLINE NphBinReader nph_bin_reader(const void *data, u64 len)
{
    const NphBinReader r = {(const u8 *)data, len, 0U};
    return r;
}

NPH_INLINE u64 nph_bin_reader_remaining(const NphBinReader *r)
{
    return r->len - r->pos;
}
NPH_INLINE bool nph_bin_reader_at_end(const NphBinReader *r)
{
    return r->pos >= r->len;
}

// Every read leaves the cursor unchanged on NPH_RESULT_END_OF_STREAM.
NPH_MUST_USE NphResult nph_bin_read_u8(NphBinReader *r, u8 *out);
NPH_MUST_USE NphResult nph_bin_read_u16(NphBinReader *r, u16 *out);
NPH_MUST_USE NphResult nph_bin_read_u32(NphBinReader *r, u32 *out);
NPH_MUST_USE NphResult nph_bin_read_u64(NphBinReader *r, u64 *out);
NPH_MUST_USE NphResult nph_bin_read_i8(NphBinReader *r, i8 *out);
NPH_MUST_USE NphResult nph_bin_read_i16(NphBinReader *r, i16 *out);
NPH_MUST_USE NphResult nph_bin_read_i32(NphBinReader *r, i32 *out);
NPH_MUST_USE NphResult nph_bin_read_i64(NphBinReader *r, i64 *out);
NPH_MUST_USE NphResult nph_bin_read_f32(NphBinReader *r, f32 *out);
NPH_MUST_USE NphResult nph_bin_read_f64(NphBinReader *r, f64 *out);
NPH_MUST_USE NphResult nph_bin_read_bool(NphBinReader *r, bool *out);
NPH_MUST_USE NphResult nph_bin_read_bytes(NphBinReader *r, void *out, u64 len);
// A view into the reader's data (no copy); valid as long as the data is.
NPH_MUST_USE NphResult nph_bin_read_str(NphBinReader *r, NphStr *out);
NPH_MUST_USE NphResult nph_bin_read_skip(NphBinReader *r, u64 len);
NPH_MUST_USE NphResult nph_bin_read_align(NphBinReader *r, u32 align);

// Checks the magic (NPH_RESULT_CORRUPT) and that min <= version <= max
// (NPH_RESULT_VERSION_MISMATCH); the version is returned either way when
// the header could be read.
NPH_MUST_USE NphResult nph_bin_read_header(NphBinReader *r, u32 magic, u32 min_version,
                                           u32 max_version, u32 *out_version);

// Reads a section head. The payload follows; nph_bin_read_skip(size)
// skips it. NPH_RESULT_CORRUPT if the size exceeds the remaining data.
NPH_MUST_USE NphResult nph_bin_read_section(NphBinReader *r, u32 *out_tag, u32 *out_size);

// Verifies a trailing CRC-32C over [data, data + len - 4). NPH_RESULT_CORRUPT
// on mismatch or when len < 4. On success *out_payload_len is len - 4.
NPH_MUST_USE NphResult nph_bin_verify_checksum(const void *data, u64 len, u64 *out_payload_len);

#ifdef __cplusplus
}
#endif
