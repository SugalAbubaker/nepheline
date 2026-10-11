// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/serialize.h>

#include <string.h>

#define BIN_MIN_CAP 64U

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

void nph_bin_writer_init(NphBinWriter *w, NphAllocator *alloc)
{
    NPH_ASSERT(w != NULL && alloc != NULL);
    memset(w, 0, sizeof *w);
    w->alloc = alloc;
}

void nph_bin_writer_free(NphBinWriter *w)
{
    if (w == NULL || w->alloc == NULL) {
        return;
    }
    if (w->data != NULL) {
        NPH_FREE(w->alloc, w->data, w->cap, 16U);
    }
    w->data = NULL;
    w->len = 0U;
    w->cap = 0U;
}

NphResult nph_bin_writer_reserve(NphBinWriter *w, u64 cap)
{
    NPH_ASSERT(w != NULL && w->alloc != NULL);
    if (cap <= w->cap) {
        return NPH_RESULT_OK;
    }
    u64 new_cap = nph_max_u64(w->cap * 2U, BIN_MIN_CAP);
    if (new_cap < cap) {
        new_cap = cap;
    }
    u8 *data = (u8 *)NPH_REALLOC(w->alloc, w->data, w->cap, new_cap, 16U);
    if (data == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    w->data = data;
    w->cap = new_cap;
    return NPH_RESULT_OK;
}

void nph_bin_writer_clear(NphBinWriter *w)
{
    w->len = 0U;
}

NphResult nph_bin_write_bytes(NphBinWriter *w, const void *data, u64 len)
{
    NPH_ASSERT(w != NULL);
    if (len == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_ASSERT(data != NULL);
    if (len > NPH_U64_MAX - w->len) {
        return NPH_RESULT_OVERFLOW;
    }
    NPH_TRY(nph_bin_writer_reserve(w, w->len + len));
    memcpy(w->data + w->len, data, (size_t)len);
    w->len += len;
    return NPH_RESULT_OK;
}

// Scalars are stored little-endian regardless of the host.
static NphResult write_le(NphBinWriter *w, u64 v, u32 size)
{
    u8 bytes[8];
    for (u32 i = 0; i < size; ++i) {
        bytes[i] = (u8)(v >> (8U * i));
    }
    return nph_bin_write_bytes(w, bytes, size);
}

NphResult nph_bin_write_u8(NphBinWriter *w, u8 v)
{
    return write_le(w, v, 1U);
}
NphResult nph_bin_write_u16(NphBinWriter *w, u16 v)
{
    return write_le(w, v, 2U);
}
NphResult nph_bin_write_u32(NphBinWriter *w, u32 v)
{
    return write_le(w, v, 4U);
}
NphResult nph_bin_write_u64(NphBinWriter *w, u64 v)
{
    return write_le(w, v, 8U);
}
NphResult nph_bin_write_i8(NphBinWriter *w, i8 v)
{
    return write_le(w, (u8)v, 1U);
}
NphResult nph_bin_write_i16(NphBinWriter *w, i16 v)
{
    return write_le(w, (u16)v, 2U);
}
NphResult nph_bin_write_i32(NphBinWriter *w, i32 v)
{
    return write_le(w, (u32)v, 4U);
}
NphResult nph_bin_write_i64(NphBinWriter *w, i64 v)
{
    return write_le(w, (u64)v, 8U);
}
NphResult nph_bin_write_f32(NphBinWriter *w, f32 v)
{
    u32 bits;
    memcpy(&bits, &v, sizeof bits);
    return write_le(w, bits, 4U);
}
NphResult nph_bin_write_f64(NphBinWriter *w, f64 v)
{
    u64 bits;
    memcpy(&bits, &v, sizeof bits);
    return write_le(w, bits, 8U);
}
NphResult nph_bin_write_bool(NphBinWriter *w, bool v)
{
    return write_le(w, v ? 1U : 0U, 1U);
}

NphResult nph_bin_write_str(NphBinWriter *w, NphStr s)
{
    if (s.len > NPH_U32_MAX) {
        return NPH_RESULT_OVERFLOW;
    }
    NPH_TRY(nph_bin_write_u32(w, (u32)s.len));
    return nph_bin_write_bytes(w, s.data, s.len);
}

NphResult nph_bin_write_align(NphBinWriter *w, u32 align)
{
    NPH_ASSERT(nph_is_pow2_u32(align));
    const u64 aligned = nph_align_up_u64(w->len, align);
    while (w->len < aligned) {
        NPH_TRY(nph_bin_write_u8(w, 0U));
    }
    return NPH_RESULT_OK;
}

void nph_bin_patch_u32(NphBinWriter *w, u64 offset, u32 v)
{
    NPH_ASSERT(offset + 4U <= w->len);
    for (u32 i = 0; i < 4U; ++i) {
        w->data[offset + i] = (u8)(v >> (8U * i));
    }
}

void nph_bin_patch_u64(NphBinWriter *w, u64 offset, u64 v)
{
    NPH_ASSERT(offset + 8U <= w->len);
    for (u32 i = 0; i < 8U; ++i) {
        w->data[offset + i] = (u8)(v >> (8U * i));
    }
}

NphResult nph_bin_write_header(NphBinWriter *w, u32 magic, u32 version)
{
    NPH_TRY(nph_bin_write_u32(w, magic));
    return nph_bin_write_u32(w, version);
}

NphResult nph_bin_write_section_begin(NphBinWriter *w, u32 tag, NphBinSection *out)
{
    NPH_ASSERT(out != NULL);
    NPH_TRY(nph_bin_write_u32(w, tag));
    out->size_offset = w->len;
    return nph_bin_write_u32(w, 0U);
}

void nph_bin_write_section_end(NphBinWriter *w, const NphBinSection *section)
{
    NPH_ASSERT(section != NULL && section->size_offset + 4U <= w->len);
    const u64 size = w->len - (section->size_offset + 4U);
    NPH_ASSERT_MSG(size <= NPH_U32_MAX, "section larger than 4 GiB");
    nph_bin_patch_u32(w, section->size_offset, (u32)size);
}

NphResult nph_bin_write_checksum(NphBinWriter *w)
{
    const u32 crc = nph_crc32c(w->data, w->len, 0U);
    return nph_bin_write_u32(w, crc);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

static NphResult read_le(NphBinReader *r, u32 size, u64 *out)
{
    if (nph_bin_reader_remaining(r) < size) {
        return NPH_RESULT_END_OF_STREAM;
    }
    u64 v = 0U;
    for (u32 i = 0; i < size; ++i) {
        v |= (u64)r->data[r->pos + i] << (8U * i);
    }
    r->pos += size;
    *out = v;
    return NPH_RESULT_OK;
}

NphResult nph_bin_read_u8(NphBinReader *r, u8 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 1U, &v));
    *out = (u8)v;
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_u16(NphBinReader *r, u16 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 2U, &v));
    *out = (u16)v;
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_u32(NphBinReader *r, u32 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 4U, &v));
    *out = (u32)v;
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_u64(NphBinReader *r, u64 *out)
{
    return read_le(r, 8U, out);
}
NphResult nph_bin_read_i8(NphBinReader *r, i8 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 1U, &v));
    *out = (i8)(u8)v;
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_i16(NphBinReader *r, i16 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 2U, &v));
    *out = (i16)(u16)v;
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_i32(NphBinReader *r, i32 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 4U, &v));
    *out = (i32)(u32)v;
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_i64(NphBinReader *r, i64 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 8U, &v));
    *out = (i64)v;
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_f32(NphBinReader *r, f32 *out)
{
    u64 v;
    NPH_TRY(read_le(r, 4U, &v));
    const u32 bits = (u32)v;
    memcpy(out, &bits, sizeof bits);
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_f64(NphBinReader *r, f64 *out)
{
    u64 bits;
    NPH_TRY(read_le(r, 8U, &bits));
    memcpy(out, &bits, sizeof bits);
    return NPH_RESULT_OK;
}
NphResult nph_bin_read_bool(NphBinReader *r, bool *out)
{
    u64 v;
    NPH_TRY(read_le(r, 1U, &v));
    *out = v != 0U;
    return NPH_RESULT_OK;
}

NphResult nph_bin_read_bytes(NphBinReader *r, void *out, u64 len)
{
    if (nph_bin_reader_remaining(r) < len) {
        return NPH_RESULT_END_OF_STREAM;
    }
    if (len > 0U) {
        NPH_ASSERT(out != NULL);
        memcpy(out, r->data + r->pos, (size_t)len);
    }
    r->pos += len;
    return NPH_RESULT_OK;
}

NphResult nph_bin_read_str(NphBinReader *r, NphStr *out)
{
    const u64 start = r->pos;
    u32 len;
    NPH_TRY(nph_bin_read_u32(r, &len));
    if (nph_bin_reader_remaining(r) < len) {
        r->pos = start;
        return NPH_RESULT_END_OF_STREAM;
    }
    *out = nph_str((const char *)r->data + r->pos, len);
    r->pos += len;
    return NPH_RESULT_OK;
}

NphResult nph_bin_read_skip(NphBinReader *r, u64 len)
{
    if (nph_bin_reader_remaining(r) < len) {
        return NPH_RESULT_END_OF_STREAM;
    }
    r->pos += len;
    return NPH_RESULT_OK;
}

NphResult nph_bin_read_align(NphBinReader *r, u32 align)
{
    NPH_ASSERT(nph_is_pow2_u32(align));
    const u64 aligned = nph_align_up_u64(r->pos, align);
    if (aligned > r->len) {
        return NPH_RESULT_END_OF_STREAM;
    }
    r->pos = aligned;
    return NPH_RESULT_OK;
}

NphResult nph_bin_read_header(NphBinReader *r, u32 magic, u32 min_version, u32 max_version,
                              u32 *out_version)
{
    const u64 start = r->pos;
    u32 found_magic;
    u32 version;
    NPH_TRY(nph_bin_read_u32(r, &found_magic));
    const NphResult v = nph_bin_read_u32(r, &version);
    if (v != NPH_RESULT_OK) {
        r->pos = start;
        return v;
    }
    if (out_version != NULL) {
        *out_version = version;
    }
    if (found_magic != magic) {
        return NPH_RESULT_CORRUPT;
    }
    if (version < min_version || version > max_version) {
        return NPH_RESULT_VERSION_MISMATCH;
    }
    return NPH_RESULT_OK;
}

NphResult nph_bin_read_section(NphBinReader *r, u32 *out_tag, u32 *out_size)
{
    const u64 start = r->pos;
    u32 tag;
    u32 size;
    NPH_TRY(nph_bin_read_u32(r, &tag));
    const NphResult s = nph_bin_read_u32(r, &size);
    if (s != NPH_RESULT_OK) {
        r->pos = start;
        return s;
    }
    if (size > nph_bin_reader_remaining(r)) {
        r->pos = start;
        return NPH_RESULT_CORRUPT;
    }
    *out_tag = tag;
    *out_size = size;
    return NPH_RESULT_OK;
}

NphResult nph_bin_verify_checksum(const void *data, u64 len, u64 *out_payload_len)
{
    if (len < 4U) {
        return NPH_RESULT_CORRUPT;
    }
    const u64 payload = len - 4U;
    const u8 *bytes = (const u8 *)data;
    u32 stored = 0U;
    for (u32 i = 0; i < 4U; ++i) {
        stored |= (u32)bytes[payload + i] << (8U * i);
    }
    if (nph_crc32c(bytes, payload, 0U) != stored) {
        return NPH_RESULT_CORRUPT;
    }
    if (out_payload_len != NULL) {
        *out_payload_len = payload;
    }
    return NPH_RESULT_OK;
}
