// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/format.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/platform/file.h>

#include <string.h>

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

static u32 header_crc(const NphAssetHeader *h)
{
    return nph_crc32c(h, NPH_ASSET_HEADER_SIZE - sizeof(u32), 0U);
}

static bool codec_is_known(u32 codec)
{
    return codec == NPH_ASSET_CODEC_NONE || codec == NPH_ASSET_CODEC_LZ;
}

static bool alignment_is_valid(u32 alignment)
{
    return nph_is_pow2_u32(alignment) && alignment >= NPH_ASSET_BLOB_DEFAULT_ALIGN &&
           alignment <= NPH_ASSET_BLOB_MAX_ALIGN;
}

NphResult nph_asset_file_validate(const void *data, u64 size, NphAssetFileInfo *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    if (data == NULL || !nph_is_ptr_aligned(data, 8U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (size < NPH_ASSET_HEADER_SIZE) {
        return NPH_RESULT_CORRUPT;
    }
    NphAssetHeader h;
    memcpy(&h, data, sizeof h);
    if (h.magic != NPH_ASSET_MAGIC) {
        return NPH_RESULT_CORRUPT;
    }
    if (h.header_size != NPH_ASSET_HEADER_SIZE || header_crc(&h) != h.header_crc) {
        return NPH_RESULT_CORRUPT;
    }
    if (h.format_version != NPH_ASSET_FORMAT_VERSION) {
        return NPH_RESULT_VERSION_MISMATCH;
    }
    if (h.file_size != size || h.blob_table_offset != NPH_ASSET_HEADER_SIZE ||
        h.blob_count > NPH_ASSET_MAX_BLOBS || h.flags != 0U) {
        return NPH_RESULT_CORRUPT;
    }
    const u64 table_bytes = (u64)h.blob_count * sizeof(NphAssetBlobEntry);
    const u64 data_start = NPH_ASSET_HEADER_SIZE + table_bytes;
    if (data_start > size) {
        return NPH_RESULT_CORRUPT;
    }
    const NphAssetBlobEntry *blobs =
        (const NphAssetBlobEntry *)((const u8 *)data + NPH_ASSET_HEADER_SIZE);
    for (u32 i = 0; i < h.blob_count; ++i) {
        const NphAssetBlobEntry *b = &blobs[i];
        if (b->tag == 0U || !codec_is_known(b->codec) || !alignment_is_valid(b->alignment) ||
            b->reserved != 0U) {
            return NPH_RESULT_CORRUPT;
        }
        if (b->offset < data_start || b->offset > size || b->stored_size > size - b->offset ||
            !nph_is_aligned_u64(b->offset, b->alignment)) {
            return NPH_RESULT_CORRUPT;
        }
        if (b->raw_size > NPH_ASSET_BLOB_MAX_SIZE) {
            return NPH_RESULT_CORRUPT;
        }
        if (b->codec == NPH_ASSET_CODEC_NONE && b->raw_size != b->stored_size) {
            return NPH_RESULT_CORRUPT;
        }
        if (b->codec == NPH_ASSET_CODEC_LZ &&
            (b->stored_size == 0U || b->stored_size > nph_lz_compress_bound(b->raw_size))) {
            return NPH_RESULT_CORRUPT;
        }
        for (u32 j = 0; j < i; ++j) {
            if (blobs[j].tag == b->tag && blobs[j].index == b->index) {
                return NPH_RESULT_CORRUPT;
            }
        }
    }
    out->header = h;
    out->blobs = blobs;
    out->blob_count = h.blob_count;
    return NPH_RESULT_OK;
}

const NphAssetBlobEntry *nph_asset_file_find(const NphAssetFileInfo *info, u32 tag, u32 index)
{
    NPH_ASSERT(info != NULL);
    for (u32 i = 0; i < info->blob_count; ++i) {
        if (info->blobs[i].tag == tag && info->blobs[i].index == index) {
            return &info->blobs[i];
        }
    }
    return NULL;
}

bool nph_asset_file_verify_blob(const void *data, const NphAssetBlobEntry *entry)
{
    NPH_ASSERT(data != NULL && entry != NULL);
    return nph_crc32c(nph_asset_file_blob(data, entry), entry->stored_size, 0U) == entry->crc;
}

NphResult nph_asset_file_unpack_blob(const void *data, const NphAssetBlobEntry *entry, void *out)
{
    NPH_ASSERT(data != NULL && entry != NULL);
    NPH_ASSERT(out != NULL || entry->raw_size == 0U);
    const void *stored = nph_asset_file_blob(data, entry);
    if (entry->codec == NPH_ASSET_CODEC_NONE) {
        if (entry->raw_size != 0U) {
            memcpy(out, stored, (size_t)entry->raw_size);
        }
        return NPH_RESULT_OK;
    }
    if (entry->codec == NPH_ASSET_CODEC_LZ) {
        return nph_lz_decompress(stored, entry->stored_size, out, entry->raw_size);
    }
    return NPH_RESULT_CORRUPT;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void nph_asset_writer_init(NphAssetWriter *w, NphAllocator *alloc, u32 type, u32 type_version,
                           NphGuid guid, u64 content_hash)
{
    NPH_ASSERT(w != NULL && alloc != NULL);
    memset(w, 0, sizeof *w);
    w->alloc = alloc;
    w->header.magic = NPH_ASSET_MAGIC;
    w->header.format_version = NPH_ASSET_FORMAT_VERSION;
    w->header.header_size = NPH_ASSET_HEADER_SIZE;
    w->header.type = type;
    w->header.type_version = type_version;
    w->header.guid = guid;
    w->header.content_hash = content_hash;
    w->header.blob_table_offset = NPH_ASSET_HEADER_SIZE;
    w->lz_level = NPH_LZ_LEVEL_HIGH;
    NPH_ARRAY_INIT(&w->blobs, alloc);
}

void nph_asset_writer_free(NphAssetWriter *w)
{
    if (w == NULL || w->alloc == NULL) {
        return;
    }
    NPH_ARRAY_FOREACH (NphAssetWriterBlob, b, &w->blobs) {
        if (b->data != NULL) {
            NPH_FREE(w->alloc, b->data, b->capacity, 16U);
        }
    }
    NPH_ARRAY_FREE(&w->blobs);
    if (w->lz_workspace != NULL) {
        NPH_FREE(w->alloc, w->lz_workspace, w->lz_workspace_size, 16U);
        w->lz_workspace = NULL;
    }
    w->alloc = NULL;
}

void nph_asset_writer_set_lz_level(NphAssetWriter *w, NphLzLevel level)
{
    NPH_ASSERT(w != NULL);
    w->lz_level = level;
}

static NphResult writer_workspace(NphAssetWriter *w, NphLzLevel level)
{
    const u64 need = nph_lz_workspace_size(level);
    if (w->lz_workspace != NULL && w->lz_workspace_size >= need) {
        return NPH_RESULT_OK;
    }
    if (w->lz_workspace != NULL) {
        NPH_FREE(w->alloc, w->lz_workspace, w->lz_workspace_size, 16U);
        w->lz_workspace = NULL;
    }
    w->lz_workspace = NPH_ALLOC(w->alloc, need, 16U);
    if (w->lz_workspace == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    w->lz_workspace_size = need;
    return NPH_RESULT_OK;
}

NphResult nph_asset_writer_add_blob(NphAssetWriter *w, u32 tag, u32 index, const void *data,
                                    u64 size, NphAssetCodec codec, u32 alignment)
{
    NPH_ASSERT(w != NULL && w->alloc != NULL);
    NPH_ASSERT(data != NULL || size == 0U);
    if (alignment == 0U) {
        alignment = NPH_ASSET_BLOB_DEFAULT_ALIGN;
    }
    if (tag == 0U || !alignment_is_valid(alignment) || !codec_is_known((u32)codec) ||
        size > NPH_ASSET_BLOB_MAX_SIZE) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (w->blobs.len >= NPH_ASSET_MAX_BLOBS) {
        return NPH_RESULT_FULL;
    }
    NPH_ARRAY_FOREACH (NphAssetWriterBlob, b, &w->blobs) {
        if (b->entry.tag == tag && b->entry.index == index) {
            return NPH_RESULT_ALREADY_EXISTS;
        }
    }

    NphAssetWriterBlob blob;
    memset(&blob, 0, sizeof blob);
    blob.entry.tag = tag;
    blob.entry.index = index;
    blob.entry.codec = (u32)codec;
    blob.entry.alignment = alignment;
    blob.entry.raw_size = size;

    if (codec == NPH_ASSET_CODEC_LZ) {
        NPH_TRY(writer_workspace(w, w->lz_level));
        const u64 bound = nph_lz_compress_bound(size);
        if (bound == 0U) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        u8 *packed = (u8 *)NPH_ALLOC(w->alloc, bound, 16U);
        if (packed == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        u64 packed_size = 0U;
        const NphResult r =
            nph_lz_compress(data, size, packed, bound, w->lz_level, w->lz_workspace, &packed_size);
        if (r != NPH_RESULT_OK) {
            NPH_FREE(w->alloc, packed, bound, 16U);
            return r;
        }
        // Keep exactly the compressed bytes (a shrinking realloc); an
        // allocator that cannot shrink leaves the block as it is, and the
        // record remembers the block's true size for the free.
        u8 *fitted = (u8 *)NPH_REALLOC(w->alloc, packed, bound, packed_size, 16U);
        blob.data = fitted != NULL ? fitted : packed;
        blob.capacity = fitted != NULL ? packed_size : bound;
        blob.entry.stored_size = packed_size;
    } else {
        blob.entry.stored_size = size;
        blob.capacity = size;
        if (size != 0U) {
            blob.data = (u8 *)NPH_ALLOC(w->alloc, size, 16U);
            if (blob.data == NULL) {
                return NPH_RESULT_OUT_OF_MEMORY;
            }
            memcpy(blob.data, data, (size_t)size);
        }
    }
    blob.entry.crc = nph_crc32c(blob.data, blob.entry.stored_size, 0U);

    const NphResult pr = NPH_ARRAY_PUSH(&w->blobs, blob);
    if (pr != NPH_RESULT_OK) {
        if (blob.data != NULL) {
            NPH_FREE(w->alloc, blob.data, blob.capacity, 16U);
        }
        return pr;
    }
    return NPH_RESULT_OK;
}

// Assigns every blob its offset and returns the file size.
static u64 writer_layout(NphAssetWriter *w)
{
    u64 offset = NPH_ASSET_HEADER_SIZE + (u64)w->blobs.len * sizeof(NphAssetBlobEntry);
    NPH_ARRAY_FOREACH (NphAssetWriterBlob, b, &w->blobs) {
        offset = nph_align_up_u64(offset, b->entry.alignment);
        b->entry.offset = offset;
        offset += b->entry.stored_size;
    }
    return nph_align_up_u64(offset, NPH_ASSET_BLOB_DEFAULT_ALIGN);
}

u64 nph_asset_writer_size(const NphAssetWriter *w)
{
    NPH_ASSERT(w != NULL);
    // The same walk as writer_layout, without recording the offsets.
    u64 offset = NPH_ASSET_HEADER_SIZE + (u64)w->blobs.len * sizeof(NphAssetBlobEntry);
    NPH_ARRAY_FOREACH (NphAssetWriterBlob, b, &w->blobs) {
        offset = nph_align_up_u64(offset, b->entry.alignment) + b->entry.stored_size;
    }
    return nph_align_up_u64(offset, NPH_ASSET_BLOB_DEFAULT_ALIGN);
}

void nph_asset_writer_finish(NphAssetWriter *w, void *out)
{
    NPH_ASSERT(w != NULL && out != NULL);
    const u64 size = writer_layout(w);
    u8 *p = (u8 *)out;
    memset(p, 0, (size_t)size);
    w->header.blob_count = w->blobs.len;
    w->header.file_size = size;
    w->header.header_crc = header_crc(&w->header);
    memcpy(p, &w->header, sizeof w->header);
    u8 *table = p + NPH_ASSET_HEADER_SIZE;
    NPH_ARRAY_FOREACH (NphAssetWriterBlob, b, &w->blobs) {
        memcpy(table, &b->entry, sizeof b->entry);
        table += sizeof b->entry;
        if (b->entry.stored_size != 0U) {
            memcpy(p + b->entry.offset, b->data, (size_t)b->entry.stored_size);
        }
    }
}

NphResult nph_asset_writer_finish_alloc(NphAssetWriter *w, NphAllocator *alloc, void **out_data,
                                        u64 *out_size)
{
    NPH_ASSERT(w != NULL && alloc != NULL && out_data != NULL && out_size != NULL);
    const u64 size = writer_layout(w);
    void *data = NPH_ALLOC(alloc, size, NPH_ASSET_BLOB_MAX_ALIGN);
    if (data == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    nph_asset_writer_finish(w, data);
    *out_data = data;
    *out_size = size;
    return NPH_RESULT_OK;
}

NphResult nph_asset_writer_write_file(NphAssetWriter *w, const char *path)
{
    NPH_ASSERT(w != NULL && path != NULL);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_asset_writer_finish_alloc(w, w->alloc, &data, &size));
    const NphResult r = nph_file_write_all(path, data, size);
    NPH_FREE(w->alloc, data, size, NPH_ASSET_BLOB_MAX_ALIGN);
    return r;
}
