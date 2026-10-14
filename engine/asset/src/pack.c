// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/asset.h>
#include <nepheline/asset/pack.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/hashmap.h>

#include <string.h>

#define PACK_COPY_CHUNK NPH_MIB(1)

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

static u32 pack_header_crc(const NphPackHeader *h)
{
    return nph_crc32c(h, NPH_PACK_HEADER_SIZE - sizeof(u32), 0U);
}

// Compares (hash, path bytes): the order of the entry table.
static i32 entry_order(u64 hash_a, NphStr path_a, u64 hash_b, NphStr path_b)
{
    if (hash_a != hash_b) {
        return hash_a < hash_b ? -1 : 1;
    }
    return nph_str_compare(path_a, path_b);
}

NphResult nph_pack_validate(const void *data, u64 size, NphPack *out)
{
    NPH_ASSERT(out != NULL);
    if (data == NULL || !nph_is_ptr_aligned(data, 8U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (size < NPH_PACK_HEADER_SIZE) {
        return NPH_RESULT_CORRUPT;
    }
    NphPackHeader h;
    memcpy(&h, data, sizeof h);
    if (h.magic != NPH_PACK_MAGIC || h.header_size != NPH_PACK_HEADER_SIZE ||
        pack_header_crc(&h) != h.header_crc) {
        return NPH_RESULT_CORRUPT;
    }
    if (h.format_version != NPH_PACK_FORMAT_VERSION) {
        return NPH_RESULT_VERSION_MISMATCH;
    }
    if (h.file_size != size || h.flags != 0U || h.entry_count > NPH_PACK_MAX_ENTRIES ||
        !nph_is_pow2_u32(h.entry_align) || h.entry_align < 16U || h.entry_align > NPH_MIB(1)) {
        return NPH_RESULT_CORRUPT;
    }
    const u64 n = h.entry_count;
    if (!nph_is_aligned_u64(h.entry_table_offset, 8U) || h.entry_table_offset > size ||
        n * sizeof(NphPackEntry) > size - h.entry_table_offset) {
        return NPH_RESULT_CORRUPT;
    }
    if (!nph_is_aligned_u64(h.guid_index_offset, 4U) || h.guid_index_offset > size ||
        n * sizeof(u32) > size - h.guid_index_offset) {
        return NPH_RESULT_CORRUPT;
    }
    if (h.string_table_offset > size || h.string_table_size > size - h.string_table_offset) {
        return NPH_RESULT_CORRUPT;
    }
    const u8 *base = (const u8 *)data;
    const NphPackEntry *entries = (const NphPackEntry *)(const void *)(base + h.entry_table_offset);
    const u32 *guid_index = (const u32 *)(const void *)(base + h.guid_index_offset);
    const char *strings = (const char *)(base + h.string_table_offset);
    if (n != 0U && (h.string_table_size == 0U || strings[h.string_table_size - 1U] != '\0')) {
        return NPH_RESULT_CORRUPT;
    }
    for (u64 i = 0; i < n; ++i) {
        const NphPackEntry *e = &entries[i];
        if (e->flags != 0U || e->reserved != 0U || nph_guid_is_null(e->guid)) {
            return NPH_RESULT_CORRUPT;
        }
        if (e->offset < NPH_PACK_HEADER_SIZE || e->offset > size || e->size > size - e->offset ||
            !nph_is_aligned_u64(e->offset, h.entry_align)) {
            return NPH_RESULT_CORRUPT;
        }
        if (e->path_len == 0U || e->path_len > NPH_PACK_MAX_PATH ||
            e->path_offset >= h.string_table_size ||
            e->path_len >= h.string_table_size - e->path_offset ||
            strings[e->path_offset + e->path_len] != '\0') {
            return NPH_RESULT_CORRUPT;
        }
        const NphStr path = nph_str(strings + e->path_offset, e->path_len);
        if (!nph_vfs_path_is_normalized(path) || e->path_hash != nph_str_hash(path)) {
            return NPH_RESULT_CORRUPT;
        }
        if (i != 0U) {
            const NphPackEntry *p = &entries[i - 1U];
            const NphStr prev = nph_str(strings + p->path_offset, p->path_len);
            if (entry_order(p->path_hash, prev, e->path_hash, path) >= 0) {
                return NPH_RESULT_CORRUPT; // unsorted or duplicate path
            }
        }
    }
    for (u64 i = 0; i < n; ++i) {
        if (guid_index[i] >= n) {
            return NPH_RESULT_CORRUPT;
        }
        if (i != 0U &&
            nph_guid_compare(entries[guid_index[i - 1U]].guid, entries[guid_index[i]].guid) >= 0) {
            return NPH_RESULT_CORRUPT; // unsorted or duplicate GUID
        }
    }
    out->header = h;
    out->entries = entries;
    out->guid_index = guid_index;
    out->strings = strings;
    return NPH_RESULT_OK;
}

NphResult nph_pack_open(NphAllocator *alloc, const char *path, NphPack *out)
{
    NPH_ASSERT(alloc != NULL && path != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    NphResult r = nph_file_open(path, NPH_FILE_READ | NPH_FILE_ASYNC | NPH_FILE_RANDOM, &out->file);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    u64 size = 0U;
    r = nph_file_size(&out->file, &size);
    if (r == NPH_RESULT_OK && size < NPH_PACK_HEADER_SIZE) {
        r = NPH_RESULT_CORRUPT;
    }
    if (r == NPH_RESULT_OK) {
        r = nph_file_map(&out->file, 0U, size, NPH_FILE_MAP_READ, &out->mapping);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_pack_validate(out->mapping.data, out->mapping.size, out);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_str_dup(alloc, nph_str_from_cstr(path), &out->path);
        out->path_len = strlen(path);
    }
    if (r != NPH_RESULT_OK) {
        if (r == NPH_RESULT_CORRUPT || r == NPH_RESULT_VERSION_MISMATCH) {
            NPH_LOG_ERROR(&NPH_LOG_ASSET, "pack %s: %s", path, nph_result_string(r));
        }
        nph_pack_close(out);
        return r;
    }
    NPH_LOG_DEBUG(&NPH_LOG_ASSET, "pack %s: %u entries, %llu bytes", path, out->header.entry_count,
                  (unsigned long long)size);
    return NPH_RESULT_OK;
}

void nph_pack_close(NphPack *pack)
{
    if (pack == NULL) {
        return;
    }
    nph_file_unmap(&pack->mapping);
    nph_file_close(&pack->file);
    if (pack->path != NULL) {
        nph_str_free(pack->alloc, pack->path, pack->path_len);
    }
    memset(pack, 0, sizeof *pack);
}

const NphPackEntry *nph_pack_find_path(const NphPack *pack, NphStr path)
{
    NPH_ASSERT(pack != NULL);
    const u64 hash = nph_str_hash(path);
    u32 lo = 0U;
    u32 hi = pack->header.entry_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2U;
        const NphPackEntry *e = &pack->entries[mid];
        const i32 c = entry_order(e->path_hash, nph_pack_entry_path(pack, e), hash, path);
        if (c == 0) {
            return e;
        }
        if (c < 0) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const NphPackEntry *nph_pack_find_guid(const NphPack *pack, NphGuid guid)
{
    NPH_ASSERT(pack != NULL);
    u32 lo = 0U;
    u32 hi = pack->header.entry_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2U;
        const NphPackEntry *e = &pack->entries[pack->guid_index[mid]];
        const i32 c = nph_guid_compare(e->guid, guid);
        if (c == 0) {
            return e;
        }
        if (c < 0) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

NphResult nph_pack_writer_begin(NphPackWriter *w, NphAllocator *alloc, const char *path,
                                u32 entry_align)
{
    NPH_ASSERT(w != NULL && alloc != NULL && path != NULL);
    memset(w, 0, sizeof *w);
    if (entry_align == 0U) {
        entry_align = NPH_PACK_DEFAULT_ENTRY_ALIGN;
    }
    if (!nph_is_pow2_u32(entry_align) || entry_align < 16U || entry_align > NPH_MIB(1)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    w->alloc = alloc;
    w->entry_align = entry_align;
    NPH_ARRAY_INIT(&w->entries, alloc);
    NPH_TRY(nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_TRUNCATE, &w->file));
    w->open = 1;
    w->cursor = NPH_PACK_HEADER_SIZE;
    return NPH_RESULT_OK;
}

static void writer_release(NphPackWriter *w)
{
    NPH_ARRAY_FOREACH (NphPackWriterEntry, e, &w->entries) {
        nph_str_free(w->alloc, e->path, e->entry.path_len);
    }
    NPH_ARRAY_FREE(&w->entries);
    nph_file_close(&w->file);
    w->open = 0;
}

void nph_pack_writer_abort(NphPackWriter *w)
{
    if (w == NULL || w->alloc == NULL) {
        return;
    }
    writer_release(w);
    memset(w, 0, sizeof *w);
}

// Checks the path and the identity against the entries so far and
// reserves the entry's place in the file. On success *out_entry is the
// new record and the file cursor sits at the entry's offset.
static NphResult writer_reserve(NphPackWriter *w, NphStr path, NphGuid guid, u32 type, u64 size,
                                NphPackWriterEntry **out_entry)
{
    if (!w->open) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (path.len == 0U || path.len > NPH_PACK_MAX_PATH || !nph_vfs_path_is_normalized(path) ||
        nph_guid_is_null(guid)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (w->entries.len >= NPH_PACK_MAX_ENTRIES) {
        return NPH_RESULT_FULL;
    }
    const u64 hash = nph_str_hash(path);
    NPH_ARRAY_FOREACH (NphPackWriterEntry, e, &w->entries) {
        if ((e->entry.path_hash == hash &&
             nph_str_equals(nph_str(e->path, e->entry.path_len), path)) ||
            nph_guid_equals(e->entry.guid, guid)) {
            return NPH_RESULT_ALREADY_EXISTS;
        }
    }
    NphPackWriterEntry rec;
    memset(&rec, 0, sizeof rec);
    rec.entry.guid = guid;
    rec.entry.path_hash = hash;
    rec.entry.offset = nph_align_up_u64(w->cursor, w->entry_align);
    rec.entry.size = size;
    rec.entry.path_len = (u32)path.len;
    rec.entry.type = type;
    NPH_TRY(nph_str_dup(w->alloc, path, &rec.path));
    NphPackWriterEntry *slot = NPH_ARRAY_PUSH_SLOT(&w->entries);
    if (slot == NULL) {
        nph_str_free(w->alloc, rec.path, path.len);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    *slot = rec;
    w->string_bytes += path.len + 1U;
    *out_entry = slot;
    return NPH_RESULT_OK;
}

NphResult nph_pack_writer_add(NphPackWriter *w, NphStr path, NphGuid guid, u32 type,
                              const void *data, u64 size)
{
    NPH_ASSERT(w != NULL);
    NPH_ASSERT(data != NULL || size == 0U);
    NphPackWriterEntry *e = NULL;
    NPH_TRY(writer_reserve(w, path, guid, type, size, &e));
    if (size != 0U) {
        const NphResult r = nph_file_write_at(&w->file, e->entry.offset, data, size, NULL);
        if (r != NPH_RESULT_OK) {
            return r;
        }
    }
    w->cursor = e->entry.offset + size;
    return NPH_RESULT_OK;
}

NphResult nph_pack_writer_add_file(NphPackWriter *w, NphStr path, NphGuid guid, u32 type,
                                   const char *os_path)
{
    NPH_ASSERT(w != NULL && os_path != NULL);
    NphFile src;
    NPH_TRY(nph_file_open(os_path, NPH_FILE_READ | NPH_FILE_SEQUENTIAL, &src));
    u64 size = 0U;
    NphResult r = nph_file_size(&src, &size);
    NphPackWriterEntry *e = NULL;
    if (r == NPH_RESULT_OK) {
        r = writer_reserve(w, path, guid, type, size, &e);
    }
    u8 *chunk = NULL;
    if (r == NPH_RESULT_OK && size != 0U) {
        chunk = (u8 *)NPH_ALLOC(w->alloc, PACK_COPY_CHUNK, 16U);
        if (chunk == NULL) {
            r = NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    u64 done = 0U;
    while (r == NPH_RESULT_OK && done < size) {
        const u64 want = nph_min_u64(PACK_COPY_CHUNK, size - done);
        u64 got = 0U;
        r = nph_file_read_at(&src, done, chunk, want, &got);
        if (r == NPH_RESULT_OK && got != want) {
            r = NPH_RESULT_IO_ERROR; // the file shrank while being copied
        }
        if (r == NPH_RESULT_OK) {
            r = nph_file_write_at(&w->file, e->entry.offset + done, chunk, got, NULL);
        }
        done += got;
    }
    if (chunk != NULL) {
        NPH_FREE(w->alloc, chunk, PACK_COPY_CHUNK, 16U);
    }
    nph_file_close(&src);
    if (r == NPH_RESULT_OK) {
        w->cursor = e->entry.offset + size;
    }
    return r;
}

// In-place heapsort over the entries by (hash, path): no allocation, and
// a total order so stability is moot.
static bool entry_less(const NphPackWriterEntry *a, const NphPackWriterEntry *b)
{
    return entry_order(a->entry.path_hash, nph_str(a->path, a->entry.path_len), b->entry.path_hash,
                       nph_str(b->path, b->entry.path_len)) < 0;
}

static void sift_down_entries(NphPackWriterEntry *v, u32 start, u32 end)
{
    u32 root = start;
    while (root * 2U + 1U < end) {
        u32 child = root * 2U + 1U;
        if (child + 1U < end && entry_less(&v[child], &v[child + 1U])) {
            ++child;
        }
        if (!entry_less(&v[root], &v[child])) {
            return;
        }
        const NphPackWriterEntry tmp = v[root];
        v[root] = v[child];
        v[child] = tmp;
        root = child;
    }
}

static void sort_entries(NphPackWriterEntry *v, u32 n)
{
    for (u32 i = n / 2U; i-- > 0U;) {
        sift_down_entries(v, i, n);
    }
    for (u32 end = n; end > 1U;) {
        --end;
        const NphPackWriterEntry tmp = v[0];
        v[0] = v[end];
        v[end] = tmp;
        sift_down_entries(v, 0U, end);
    }
}

static void sift_down_guids(u32 *idx, const NphPackWriterEntry *v, u32 start, u32 end)
{
    u32 root = start;
    while (root * 2U + 1U < end) {
        u32 child = root * 2U + 1U;
        if (child + 1U < end &&
            nph_guid_compare(v[idx[child]].entry.guid, v[idx[child + 1U]].entry.guid) < 0) {
            ++child;
        }
        if (nph_guid_compare(v[idx[root]].entry.guid, v[idx[child]].entry.guid) >= 0) {
            return;
        }
        const u32 tmp = idx[root];
        idx[root] = idx[child];
        idx[child] = tmp;
        root = child;
    }
}

static void sort_guid_index(u32 *idx, const NphPackWriterEntry *v, u32 n)
{
    for (u32 i = n / 2U; i-- > 0U;) {
        sift_down_guids(idx, v, i, n);
    }
    for (u32 end = n; end > 1U;) {
        --end;
        const u32 tmp = idx[0];
        idx[0] = idx[end];
        idx[end] = tmp;
        sift_down_guids(idx, v, 0U, end);
    }
}

NphResult nph_pack_writer_finish(NphPackWriter *w)
{
    NPH_ASSERT(w != NULL);
    if (!w->open) {
        return NPH_RESULT_INVALID_STATE;
    }
    const u32 n = w->entries.len;
    sort_entries(w->entries.data, n);

    // Assemble the trailer in memory: entries, GUID index, strings.
    const u64 table_offset = nph_align_up_u64(w->cursor, 8U);
    const u64 index_offset = table_offset + (u64)n * sizeof(NphPackEntry);
    const u64 strings_offset = index_offset + (u64)n * sizeof(u32);
    const u64 trailer_size = strings_offset + w->string_bytes - table_offset;
    const u64 file_size = nph_align_up_u64(strings_offset + w->string_bytes, 16U);
    u8 *trailer = (u8 *)NPH_ALLOC_ZEROED(w->alloc, nph_max_u64(trailer_size, 1U), 16U);
    u32 *index = (u32 *)(void *)(trailer + (index_offset - table_offset));
    if (trailer == NULL) {
        writer_release(w);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 string_cursor = 0U;
    for (u32 i = 0; i < n; ++i) {
        NphPackWriterEntry *e = &w->entries.data[i];
        e->entry.path_offset = string_cursor;
        memcpy(trailer + (strings_offset - table_offset) + string_cursor, e->path,
               e->entry.path_len + 1U);
        string_cursor += e->entry.path_len + 1U;
        memcpy(trailer + (u64)i * sizeof(NphPackEntry), &e->entry, sizeof e->entry);
        index[i] = i;
    }
    sort_guid_index(index, w->entries.data, n);

    NphPackHeader h;
    memset(&h, 0, sizeof h);
    h.magic = NPH_PACK_MAGIC;
    h.format_version = NPH_PACK_FORMAT_VERSION;
    h.header_size = NPH_PACK_HEADER_SIZE;
    h.entry_count = n;
    h.entry_align = w->entry_align;
    h.entry_table_offset = table_offset;
    h.guid_index_offset = index_offset;
    h.string_table_offset = strings_offset;
    h.string_table_size = w->string_bytes;
    h.file_size = file_size;
    h.header_crc = pack_header_crc(&h);

    NphResult r = NPH_RESULT_OK;
    if (trailer_size != 0U) {
        r = nph_file_write_at(&w->file, table_offset, trailer, trailer_size, NULL);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_file_truncate(&w->file, file_size);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_at(&w->file, 0U, &h, sizeof h, NULL);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_file_flush(&w->file);
    }
    NPH_FREE(w->alloc, trailer, nph_max_u64(trailer_size, 1U), 16U);
    writer_release(w);
    return r;
}
