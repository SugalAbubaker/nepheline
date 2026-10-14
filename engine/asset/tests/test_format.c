// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/format.h>
#include <nepheline/core/hash.h>

#include "test_asset_support.h"

#define TAG_DATA NPH_FOURCC('D', 'A', 'T', 'A')
#define TAG_HEAD NPH_FOURCC('H', 'E', 'A', 'D')

typedef struct FormatFixture {
    NphAllocator *alloc;
    void *image;
    u64 size;
} FormatFixture;

NTEST_SETUP(FormatFixture)
{
    f->alloc = nph_allocator_system();
}

NTEST_TEARDOWN(FormatFixture)
{
    if (f->image != NULL) {
        test_free_asset_image(f->alloc, f->image, f->size);
    }
}

// A file with a stored header blob, a compressed data blob, an empty
// blob, and a 4 KiB-aligned stored blob.
static void build_mixed(FormatFixture *f)
{
    NphAssetWriter w;
    const NphGuid guid = nph_guid_from_cstr("test/mixed");
    nph_asset_writer_init(&w, f->alloc, NPH_ASSET_TYPE_BLOB, 3U, guid, 0x1234U);
    u8 head[48];
    test_fill_pattern(head, sizeof head, 1U);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_HEAD, 0U, head, sizeof head, NPH_ASSET_CODEC_NONE, 0U),
        NPH_RESULT_OK);
    u8 *text = (u8 *)NPH_ALLOC(f->alloc, NPH_KIB(64), 16U);
    NTEST_ASSERT_NOT_NULL(text);
    test_fill_text(text, NPH_KIB(64), 2U);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 0U, text, NPH_KIB(64), NPH_ASSET_CODEC_LZ, 0U),
        NPH_RESULT_OK);
    NPH_FREE(f->alloc, text, NPH_KIB(64), 16U);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 1U, NULL, 0U, NPH_ASSET_CODEC_NONE, 0U),
        NPH_RESULT_OK);
    u8 page[100];
    test_fill_pattern(page, sizeof page, 9U);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 2U, page, sizeof page, NPH_ASSET_CODEC_NONE, 4096U),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(w.blobs.len, 4U);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_finish_alloc(&w, f->alloc, &f->image, &f->size),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->size, nph_asset_writer_size(&w));
    nph_asset_writer_free(&w);
}

NTEST_F(FormatFixture, writer_output_validates_and_blobs_read_back)
{
    build_mixed(f);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.header.magic, NPH_ASSET_MAGIC);
    NTEST_ASSERT_EQ_UINT(info.header.type, NPH_ASSET_TYPE_BLOB);
    NTEST_ASSERT_EQ_UINT(info.header.type_version, 3U);
    NTEST_ASSERT_EQ_UINT(info.header.content_hash, 0x1234U);
    NTEST_ASSERT_TRUE(nph_guid_equals(info.header.guid, nph_guid_from_cstr("test/mixed")));
    NTEST_ASSERT_EQ_UINT(info.blob_count, 4U);
    NTEST_ASSERT_EQ_UINT(info.header.file_size, f->size);
    NTEST_ASSERT_EQ_UINT(f->size % 16U, 0U);

    const NphAssetBlobEntry *head = nph_asset_file_find(&info, TAG_HEAD, 0U);
    NTEST_ASSERT_NOT_NULL(head);
    NTEST_ASSERT_EQ_UINT(head->codec, NPH_ASSET_CODEC_NONE);
    NTEST_ASSERT_EQ_UINT(head->raw_size, 48U);
    NTEST_ASSERT_EQ_UINT(head->stored_size, 48U);
    NTEST_ASSERT_EQ_UINT(head->alignment, 16U);
    NTEST_ASSERT_EQ_UINT(head->offset % 16U, 0U);
    NTEST_ASSERT_TRUE(test_check_pattern(nph_asset_file_blob(f->image, head), 48U, 1U));
    NTEST_ASSERT_TRUE(nph_asset_file_verify_blob(f->image, head));

    const NphAssetBlobEntry *data = nph_asset_file_find(&info, TAG_DATA, 0U);
    NTEST_ASSERT_NOT_NULL(data);
    NTEST_ASSERT_EQ_UINT(data->codec, NPH_ASSET_CODEC_LZ);
    NTEST_ASSERT_EQ_UINT(data->raw_size, NPH_KIB(64));
    NTEST_ASSERT_LT_UINT(data->stored_size, NPH_KIB(32));
    NTEST_ASSERT_TRUE(nph_asset_file_verify_blob(f->image, data));
    u8 *back = (u8 *)NPH_ALLOC(f->alloc, NPH_KIB(64), 16U);
    u8 *expect = (u8 *)NPH_ALLOC(f->alloc, NPH_KIB(64), 16U);
    NTEST_ASSERT_NOT_NULL(back);
    NTEST_ASSERT_NOT_NULL(expect);
    test_fill_text(expect, NPH_KIB(64), 2U);
    NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(f->image, data, back), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_MEM(back, expect, NPH_KIB(64));
    NPH_FREE(f->alloc, back, NPH_KIB(64), 16U);
    NPH_FREE(f->alloc, expect, NPH_KIB(64), 16U);

    const NphAssetBlobEntry *empty = nph_asset_file_find(&info, TAG_DATA, 1U);
    NTEST_ASSERT_NOT_NULL(empty);
    NTEST_ASSERT_EQ_UINT(empty->raw_size, 0U);
    NTEST_ASSERT_EQ_UINT(empty->stored_size, 0U);
    NTEST_ASSERT_TRUE(nph_asset_file_verify_blob(f->image, empty));
    NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(f->image, empty, NULL), NPH_RESULT_OK);

    const NphAssetBlobEntry *page = nph_asset_file_find(&info, TAG_DATA, 2U);
    NTEST_ASSERT_NOT_NULL(page);
    NTEST_ASSERT_EQ_UINT(page->alignment, 4096U);
    NTEST_ASSERT_EQ_UINT(page->offset % 4096U, 0U);
    u8 copy[100];
    NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(f->image, page, copy), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(test_check_pattern(copy, sizeof copy, 9U));

    NTEST_ASSERT_NULL(nph_asset_file_find(&info, TAG_DATA, 3U));
    NTEST_ASSERT_NULL(nph_asset_file_find(&info, NPH_FOURCC('N', 'O', 'P', 'E'), 0U));
}

NTEST_F(FormatFixture, writer_rejects_bad_blobs)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, f->alloc, NPH_ASSET_TYPE_BLOB, 1U, nph_guid_from_cstr("x"), 0U);
    u8 data[8] = {0};
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 0U, data, sizeof data, NPH_ASSET_CODEC_NONE, 0U),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 0U, data, sizeof data, NPH_ASSET_CODEC_NONE, 0U),
        NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, 0U, 1U, data, sizeof data, NPH_ASSET_CODEC_NONE, 0U),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 1U, data, sizeof data, NPH_ASSET_CODEC_NONE, 24U),
        NPH_RESULT_INVALID_ARGUMENT); // not a power of two
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 1U, data, sizeof data, NPH_ASSET_CODEC_NONE, 8192U),
        NPH_RESULT_INVALID_ARGUMENT); // too large
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 1U, data, sizeof data, (NphAssetCodec)7, 0U),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(w.blobs.len, 1U);
    nph_asset_writer_free(&w);
    nph_asset_writer_free(&w); // idempotent
}

// Corrupts one byte and expects the validation (or the blob check) to notice.
NTEST_F(FormatFixture, corruption_is_detected)
{
    build_mixed(f);
    u8 *bytes = (u8 *)f->image;
    NphAssetFileInfo info;

    // The magic.
    bytes[0] ^= 1U;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_CORRUPT);
    bytes[0] ^= 1U;
    // The header checksum covers the type.
    bytes[8] ^= 1U;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_CORRUPT);
    bytes[8] ^= 1U;
    // A future format version.
    NphAssetHeader h;
    memcpy(&h, bytes, sizeof h);
    h.format_version = 99U;
    h.header_crc = nph_crc32c(&h, 60U, 0U);
    memcpy(bytes, &h, sizeof h);
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info),
                        NPH_RESULT_VERSION_MISMATCH);
    h.format_version = NPH_ASSET_FORMAT_VERSION;
    h.header_crc = nph_crc32c(&h, 60U, 0U);
    memcpy(bytes, &h, sizeof h);
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_OK);

    // Truncation.
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size - 1U, &info), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, 32U, &info), NPH_RESULT_CORRUPT);
    // Misaligned data.
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(bytes + 4U, f->size - 4U, &info),
                        NPH_RESULT_INVALID_ARGUMENT);

    // A blob entry pointing past the end (patch the first entry's size).
    NphAssetBlobEntry *table = (NphAssetBlobEntry *)(void *)(bytes + NPH_ASSET_HEADER_SIZE);
    const u64 saved = table[0].stored_size;
    table[0].stored_size = f->size;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_CORRUPT);
    table[0].stored_size = saved;
    // Misaligned blob offset.
    table[0].offset += 1U;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_CORRUPT);
    table[0].offset -= 1U;
    // A duplicate tag/index pair.
    const u32 saved_index = table[2].index;
    table[2].index = table[3].index;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_CORRUPT);
    table[2].index = saved_index;
    // A stored blob whose raw size differs from its stored size.
    table[0].raw_size += 1U;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_CORRUPT);
    table[0].raw_size -= 1U;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_OK);

    // A flipped data byte fails the blob's checksum, not the validation,
    // and a flipped byte in a compressed blob also fails to decode or
    // decodes to the wrong bytes (the checksum is what guarantees it).
    const NphAssetBlobEntry *head = nph_asset_file_find(&info, TAG_HEAD, 0U);
    bytes[head->offset + 5U] ^= 0x80U;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(f->image, f->size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_asset_file_verify_blob(f->image, head));
    bytes[head->offset + 5U] ^= 0x80U;
    NTEST_ASSERT_TRUE(nph_asset_file_verify_blob(f->image, head));
    const NphAssetBlobEntry *data = nph_asset_file_find(&info, TAG_DATA, 0U);
    bytes[data->offset + data->stored_size / 2U] ^= 0xFFU;
    NTEST_ASSERT_FALSE(nph_asset_file_verify_blob(f->image, data));
}

NTEST_F(FormatFixture, file_round_trip_through_the_file_system)
{
    TempDir dir;
    test_temp_dir_make(&dir, "format");
    char path[1200];
    test_path_in(&dir, "thing.nph", path, sizeof path);

    NphAssetWriter w;
    nph_asset_writer_init(&w, f->alloc, NPH_ASSET_TYPE_BLOB, 1U, nph_guid_from_cstr("thing"), 7U);
    nph_asset_writer_set_lz_level(&w, NPH_LZ_LEVEL_FAST);
    u8 data[5000];
    test_fill_text(data, sizeof data, 11U);
    NTEST_ASSERT_EQ_INT(
        nph_asset_writer_add_blob(&w, TAG_DATA, 0U, data, sizeof data, NPH_ASSET_CODEC_LZ, 64U),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_write_file(&w, path), NPH_RESULT_OK);
    nph_asset_writer_free(&w);

    void *image = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, f->alloc, &image, &size), NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(image, size, &info), NPH_RESULT_OK);
    const NphAssetBlobEntry *e = nph_asset_file_find(&info, TAG_DATA, 0U);
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_ASSERT_EQ_UINT(e->alignment, 64U);
    NTEST_ASSERT_EQ_UINT(e->offset % 64U, 0U);
    u8 back[5000];
    NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(image, e, back), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_MEM(back, data, sizeof data);
    nph_file_free_all(f->alloc, image, size);
    test_temp_dir_remove(&dir);
}

NTEST(format, fourcc_and_type_names)
{
    NTEST_ASSERT_EQ_UINT(NPH_FOURCC('N', 'P', 'H', 'A'), 0x4148504EU);
    char text[5];
    nph_fourcc_to_string(NPH_ASSET_TYPE_MESH, text);
    NTEST_ASSERT_EQ_STR(text, "MESH");
    nph_fourcc_to_string(0x00FF0041U, text);
    NTEST_ASSERT_EQ_STR(text, "A..."); // non-printable bytes become dots
    NTEST_ASSERT_EQ_STR(nph_asset_type_name(NPH_ASSET_TYPE_TEXTURE), "texture");
    NTEST_ASSERT_EQ_STR(nph_asset_type_name(NPH_ASSET_TYPE_MATERIAL), "material");
    NTEST_ASSERT_EQ_STR(nph_asset_type_name(NPH_ASSET_TYPE_SCENE), "scene");
    NTEST_ASSERT_EQ_STR(nph_asset_type_name(NPH_ASSET_TYPE_BLOB), "blob");
    NTEST_ASSERT_EQ_STR(nph_asset_type_name(123U), "unknown");
}
