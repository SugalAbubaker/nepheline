// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/pack.h>
#include <nepheline/core/hash.h>

#include "test_asset_support.h"

typedef struct PackFixture {
    NphAllocator *alloc;
    TempDir dir;
    char pack_path[1200];
} PackFixture;

NTEST_SETUP(PackFixture)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "pack");
    test_path_in(&f->dir, "content.npk", f->pack_path, sizeof f->pack_path);
}

NTEST_TEARDOWN(PackFixture)
{
    test_temp_dir_remove(&f->dir);
}

#define ENTRY_COUNT 40U

// Writes a pack of ENTRY_COUNT entries "dir<i%4>/file<i>.nph" with
// pattern seed i and sizes that vary, plus one from a file on disk.
static void write_pack(PackFixture *f, u32 entry_align)
{
    NphPackWriter w;
    NTEST_ASSERT_EQ_INT(nph_pack_writer_begin(&w, f->alloc, f->pack_path, entry_align),
                        NPH_RESULT_OK);
    u8 *buf = (u8 *)NPH_ALLOC(f->alloc, NPH_KIB(64), 16U);
    NTEST_ASSERT_NOT_NULL(buf);
    for (u32 i = 0; i < ENTRY_COUNT; ++i) {
        char name[64];
        snprintf(name, sizeof name, "dir%u/file%u.nph", i % 4U, i);
        const u64 size = ((u64)i * 977U) % NPH_KIB(64);
        test_fill_pattern(buf, size, i);
        NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, nph_str_from_cstr(name),
                                                nph_guid_from_cstr(name), NPH_ASSET_TYPE_BLOB, buf,
                                                size),
                            NPH_RESULT_OK);
    }
    // From a file: 200 KiB so the copy loop takes more than one chunk
    // only when the chunk is small; here it checks the plain path.
    char on_disk[1200];
    test_path_in(&f->dir, "big.bin", on_disk, sizeof on_disk);
    const u64 big = NPH_KIB(200);
    u8 *big_buf = (u8 *)NPH_ALLOC(f->alloc, big, 16U);
    NTEST_ASSERT_NOT_NULL(big_buf);
    test_fill_pattern(big_buf, big, 1000U);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(on_disk, big_buf, big), NPH_RESULT_OK);
    NPH_FREE(f->alloc, big_buf, big, 16U);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add_file(&w, NPH_STR("big/one.nph"),
                                                 nph_guid_from_cstr("big"), NPH_ASSET_TYPE_BLOB,
                                                 on_disk),
                        NPH_RESULT_OK);
    // Duplicates are refused: same path, then same GUID under another path.
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, NPH_STR("dir0/file0.nph"),
                                            nph_guid_from_cstr("other"), NPH_ASSET_TYPE_BLOB, buf,
                                            1U),
                        NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, NPH_STR("dir0/twin.nph"),
                                            nph_guid_from_cstr("dir0/file0.nph"),
                                            NPH_ASSET_TYPE_BLOB, buf, 1U),
                        NPH_RESULT_ALREADY_EXISTS);
    // Bad paths and GUIDs.
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, NPH_STR("/abs.nph"), nph_guid_from_cstr("a"),
                                            NPH_ASSET_TYPE_BLOB, buf, 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, NPH_STR("a/../b.nph"), nph_guid_from_cstr("a"),
                                            NPH_ASSET_TYPE_BLOB, buf, 1U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(
        nph_pack_writer_add(&w, NPH_STR("null.nph"), NPH_GUID_NULL, NPH_ASSET_TYPE_BLOB, buf, 1U),
        NPH_RESULT_INVALID_ARGUMENT);
    NPH_FREE(f->alloc, buf, NPH_KIB(64), 16U);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_finish(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_finish(&w), NPH_RESULT_INVALID_STATE);
    nph_pack_writer_abort(&w); // safe after finish
}

static void check_pack(PackFixture *f, u32 entry_align)
{
    NphPack pack;
    NTEST_ASSERT_EQ_INT(nph_pack_open(f->alloc, f->pack_path, &pack), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_pack_entry_count(&pack), ENTRY_COUNT + 1U);
    NTEST_ASSERT_EQ_UINT(pack.header.entry_align, entry_align);
    for (u32 i = 0; i < ENTRY_COUNT; ++i) {
        char name[64];
        snprintf(name, sizeof name, "dir%u/file%u.nph", i % 4U, i);
        const NphPackEntry *e = nph_pack_find_path(&pack, nph_str_from_cstr(name));
        NTEST_ASSERT_NOT_NULL(e);
        NTEST_ASSERT_EQ_UINT(e->size, ((u64)i * 977U) % NPH_KIB(64));
        NTEST_ASSERT_EQ_UINT(e->offset % entry_align, 0U);
        NTEST_ASSERT_EQ_UINT(e->type, NPH_ASSET_TYPE_BLOB);
        NTEST_ASSERT_TRUE(nph_str_equals(nph_pack_entry_path(&pack, e), nph_str_from_cstr(name)));
        NTEST_ASSERT_TRUE(test_check_pattern(nph_pack_entry_data(&pack, e), e->size, i));
        const NphPackEntry *by_guid = nph_pack_find_guid(&pack, nph_guid_from_cstr(name));
        NTEST_ASSERT_EQ_PTR(by_guid, e);
    }
    const NphPackEntry *big = nph_pack_find_path(&pack, NPH_STR("big/one.nph"));
    NTEST_ASSERT_NOT_NULL(big);
    NTEST_ASSERT_EQ_UINT(big->size, NPH_KIB(200));
    NTEST_ASSERT_TRUE(test_check_pattern(nph_pack_entry_data(&pack, big), big->size, 1000U));
    NTEST_ASSERT_NULL(nph_pack_find_path(&pack, NPH_STR("dir0/file1.nph")));
    NTEST_ASSERT_NULL(nph_pack_find_path(&pack, NPH_STR("DIR0/file0.nph"))); // case-sensitive
    NTEST_ASSERT_NULL(nph_pack_find_guid(&pack, nph_guid_from_cstr("nope")));
    // The entry table is sorted by hash, then path; the GUID index by GUID.
    for (u32 i = 1; i < nph_pack_entry_count(&pack); ++i) {
        const NphPackEntry *a = nph_pack_entry_at(&pack, i - 1U);
        const NphPackEntry *b = nph_pack_entry_at(&pack, i);
        NTEST_ASSERT_LE_UINT(a->path_hash, b->path_hash);
        NTEST_ASSERT_LT_INT(nph_guid_compare(pack.entries[pack.guid_index[i - 1U]].guid,
                                             pack.entries[pack.guid_index[i]].guid),
                            0);
    }
    nph_pack_close(&pack);
    nph_pack_close(&pack); // idempotent
}

NTEST_F(PackFixture, write_then_read_with_the_default_alignment)
{
    write_pack(f, 0U);
    check_pack(f, NPH_PACK_DEFAULT_ENTRY_ALIGN);
}

NTEST_F(PackFixture, write_then_read_with_a_small_alignment)
{
    write_pack(f, 16U);
    check_pack(f, 16U);
}

NTEST_F(PackFixture, empty_pack)
{
    NphPackWriter w;
    NTEST_ASSERT_EQ_INT(nph_pack_writer_begin(&w, f->alloc, f->pack_path, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_finish(&w), NPH_RESULT_OK);
    NphPack pack;
    NTEST_ASSERT_EQ_INT(nph_pack_open(f->alloc, f->pack_path, &pack), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_pack_entry_count(&pack), 0U);
    NTEST_ASSERT_NULL(nph_pack_find_path(&pack, NPH_STR("x")));
    NTEST_ASSERT_NULL(nph_pack_find_guid(&pack, nph_guid_from_cstr("x")));
    nph_pack_close(&pack);
}

NTEST_F(PackFixture, missing_and_corrupt_packs_are_reported)
{
    NphPack pack;
    char missing[1200];
    test_path_in(&f->dir, "missing.npk", missing, sizeof missing);
    NTEST_ASSERT_EQ_INT(nph_pack_open(f->alloc, missing, &pack), NPH_RESULT_NOT_FOUND);

    write_pack(f, 16U);
    void *image = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(f->pack_path, f->alloc, &image, &size), NPH_RESULT_OK);
    u8 *bytes = (u8 *)image;
    NphPack mem;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(mem.header.entry_count, ENTRY_COUNT + 1U);

    // Flip the magic, a header field (checksum), truncate.
    bytes[0] ^= 1U;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_CORRUPT);
    bytes[0] ^= 1U;
    bytes[8] ^= 1U;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_CORRUPT);
    bytes[8] ^= 1U;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size - 16U, &mem), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, 10U, &mem), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(nph_pack_validate(bytes + 4U, size - 4U, &mem),
                        NPH_RESULT_INVALID_ARGUMENT);
    // A future version.
    NphPackHeader h;
    memcpy(&h, bytes, sizeof h);
    h.format_version = 2U;
    h.header_crc = nph_crc32c(&h, 60U, 0U);
    memcpy(bytes, &h, sizeof h);
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_VERSION_MISMATCH);
    h.format_version = 1U;
    h.header_crc = nph_crc32c(&h, 60U, 0U);
    memcpy(bytes, &h, sizeof h);
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_OK);

    // Corrupt an entry: a wrong path hash, a bad offset, a swapped order.
    NphPackEntry *entries = (NphPackEntry *)(bytes + h.entry_table_offset);
    const u64 saved_hash = entries[3].path_hash;
    entries[3].path_hash ^= 1U;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_CORRUPT);
    entries[3].path_hash = saved_hash;
    const u64 saved_size = entries[3].size;
    entries[3].size = size;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_CORRUPT);
    entries[3].size = saved_size;
    const NphPackEntry tmp = entries[3];
    entries[3] = entries[4];
    entries[4] = tmp;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_CORRUPT);
    entries[4] = entries[3];
    entries[3] = tmp;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_OK);
    // A GUID index entry out of range.
    u32 *index = (u32 *)(bytes + h.guid_index_offset);
    const u32 saved_index = index[0];
    index[0] = 0xFFFFFFFFU;
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_CORRUPT);
    index[0] = saved_index;
    // A string table that no longer ends in NUL.
    bytes[h.string_table_offset + h.string_table_size - 1U] = 'x';
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_CORRUPT);
    bytes[h.string_table_offset + h.string_table_size - 1U] = '\0';
    NTEST_ASSERT_EQ_INT(nph_pack_validate(image, size, &mem), NPH_RESULT_OK);
    nph_file_free_all(f->alloc, image, size);
}

NTEST_F(PackFixture, abort_leaves_an_unreadable_file_and_begin_rejects_bad_alignment)
{
    NphPackWriter w;
    NTEST_ASSERT_EQ_INT(nph_pack_writer_begin(&w, f->alloc, f->pack_path, 24U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_begin(&w, f->alloc, f->pack_path, 0U), NPH_RESULT_OK);
    u8 data[16] = {0};
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, NPH_STR("a.nph"), nph_guid_from_cstr("a"),
                                            NPH_ASSET_TYPE_BLOB, data, sizeof data),
                        NPH_RESULT_OK);
    nph_pack_writer_abort(&w);
    nph_pack_writer_abort(&w);
    NphPack pack;
    NTEST_ASSERT_EQ_INT(nph_pack_open(f->alloc, f->pack_path, &pack), NPH_RESULT_CORRUPT);
    NphPackWriter zero;
    memset(&zero, 0, sizeof zero);
    nph_pack_writer_abort(&zero);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_finish(&zero), NPH_RESULT_INVALID_STATE);
}
