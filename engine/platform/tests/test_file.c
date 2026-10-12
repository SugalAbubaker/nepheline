// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <stdio.h>
#include <string.h>

// Every test works in its own directory under the temp directory and
// removes it afterwards.
typedef struct TempDir {
    char dir[1024];
} TempDir;

static u32 g_temp_counter;

NTEST_SETUP(TempDir)
{
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    snprintf(f->dir, sizeof f->dir, "%s/nph_platform_test_%llu_%u", tmp,
             (unsigned long long)nph_platform_process_id(), g_temp_counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
}

NTEST_TEARDOWN(TempDir)
{
    NTEST_ASSERT_EQ_INT(nph_fs_remove_tree(f->dir), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_fs_exists(f->dir));
}

static const char *path_in(const TempDir *f, const char *name, char *buf, u64 cap)
{
    NTEST_ASSERT_EQ_INT(
        nph_path_join(buf, cap, nph_str_from_cstr(f->dir), nph_str_from_cstr(name), NULL),
        NPH_RESULT_OK);
    return buf;
}

static void fill(u8 *p, u64 n, u64 seed)
{
    for (u64 i = 0; i < n; ++i) {
        p[i] = (u8)((seed * 131U + i * 7U) & 0xFFU);
    }
}

static bool check(const u8 *p, u64 n, u64 seed)
{
    for (u64 i = 0; i < n; ++i) {
        if (p[i] != (u8)((seed * 131U + i * 7U) & 0xFFU)) {
            return false;
        }
    }
    return true;
}

#define BIG (NPH_MIB(1) + 12345U)
static u8 g_write_buf[BIG];
static u8 g_read_buf[BIG];

NTEST_F(TempDir, write_then_read_whole_file)
{
    char path[1024];
    path_in(f, "data.bin", path, sizeof path);
    fill(g_write_buf, BIG, 1U);

    NphFile file;
    NTEST_ASSERT_EQ_INT(
        nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_TRUNCATE, &file),
        NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_file_is_open(&file));
    u64 written = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_write(&file, g_write_buf, BIG, &written), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(written, BIG);
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_size(&file, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, BIG);
    NTEST_ASSERT_EQ_INT(nph_file_flush(&file), NPH_RESULT_OK);
    nph_file_close(&file);
    NTEST_ASSERT_FALSE(nph_file_is_open(&file));
    nph_file_close(&file); // idempotent

    NphFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_fs_stat(path, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.size, BIG);
    NTEST_ASSERT_TRUE(info.is_regular);
    NTEST_ASSERT_FALSE(info.is_directory);
    const u64 now = nph_time_wall_ns();
    NTEST_ASSERT_LT_UINT(now - info.modified_ns, 60U * NPH_TIME_NS_PER_S);

    NTEST_ASSERT_EQ_INT(nph_file_open(path, NPH_FILE_READ | NPH_FILE_SEQUENTIAL, &file),
                        NPH_RESULT_OK);
    memset(g_read_buf, 0, sizeof g_read_buf);
    u64 total = 0U;
    for (;;) {
        // Never ask for more than the buffer holds: the OS validates the
        // whole range before reading, even when the file ends sooner.
        const u64 room = BIG - total;
        const u64 ask = room < 100000U ? room : 100000U;
        u8 tail[16];
        u64 got = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read(&file, ask > 0U ? g_read_buf + total : tail,
                                          ask > 0U ? ask : sizeof tail, &got),
                            NPH_RESULT_OK);
        if (got == 0U) {
            break; // the end: a read into `tail` returned nothing
        }
        total += got;
    }
    NTEST_ASSERT_EQ_UINT(total, BIG);
    NTEST_ASSERT_TRUE(check(g_read_buf, BIG, 1U));
    u64 pos = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_tell(&file, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pos, BIG);
    nph_file_close(&file);
}

NTEST_F(TempDir, open_modes)
{
    char path[1024];
    path_in(f, "modes.txt", path, sizeof path);
    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(path, NPH_FILE_READ, &file), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_FALSE(nph_file_is_open(&file));
    NTEST_ASSERT_EQ_INT(nph_file_open(path, 0U, &file), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_file_open(path, NPH_FILE_READ | NPH_FILE_CREATE, &file),
                        NPH_RESULT_INVALID_ARGUMENT); // CREATE needs WRITE

    // Create exclusively, then fail to create exclusively again.
    NTEST_ASSERT_EQ_INT(
        nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_EXCLUSIVE, &file),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write(&file, "hello", 5U, NULL), NPH_RESULT_OK);
    nph_file_close(&file);
    NTEST_ASSERT_EQ_INT(
        nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_EXCLUSIVE, &file),
        NPH_RESULT_ALREADY_EXISTS);

    // CREATE without TRUNCATE keeps the contents; APPEND adds to the end.
    NTEST_ASSERT_EQ_INT(
        nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_APPEND, &file),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write(&file, " world", 6U, NULL), NPH_RESULT_OK);
    nph_file_close(&file);
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_allocator_system(), &data, &size),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, 11U);
    NTEST_ASSERT_EQ_STR((const char *)data, "hello world"); // NUL-terminated
    nph_file_free_all(nph_allocator_system(), data, size);

    // TRUNCATE empties it.
    NTEST_ASSERT_EQ_INT(nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_TRUNCATE, &file),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_size(&file, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, 0U);
    nph_file_close(&file);

    // Reading a directory as a file fails cleanly.
    NTEST_ASSERT_NE_INT(nph_file_open(f->dir, NPH_FILE_READ | NPH_FILE_WRITE, &file),
                        NPH_RESULT_OK);
}

NTEST_F(TempDir, positional_io_and_short_reads)
{
    char path[1024];
    path_in(f, "pos.bin", path, sizeof path);
    NphFile file;
    NTEST_ASSERT_EQ_INT(
        nph_file_open(path, NPH_FILE_READ | NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_RANDOM,
                      &file),
        NPH_RESULT_OK);
    u8 block[4096];
    // Write blocks out of order; the file grows to cover the highest one.
    const u64 order[] = {3U, 0U, 2U, 1U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(order); ++i) {
        fill(block, sizeof block, order[i]);
        NTEST_ASSERT_EQ_INT(
            nph_file_write_at(&file, order[i] * sizeof block, block, sizeof block, NULL),
            NPH_RESULT_OK);
    }
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_size(&file, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, 4U * sizeof block);
    for (u64 b = 0; b < 4U; ++b) {
        u64 got = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, b * sizeof block, block, sizeof block, &got),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(got, sizeof block);
        NTEST_ASSERT_TRUE(check(block, sizeof block, b));
    }
    // Crossing the end gives a short read; starting past it gives none.
    u64 got = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, size - 100U, block, sizeof block, &got),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 100U);
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, size + 1000U, block, sizeof block, &got),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 0U);
    // Unaligned offsets and sizes.
    fill(block, 13U, 77U);
    NTEST_ASSERT_EQ_INT(nph_file_write_at(&file, 1001U, block, 13U, NULL), NPH_RESULT_OK);
    memset(block, 0, sizeof block);
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, 1001U, block, 13U, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 13U);
    NTEST_ASSERT_TRUE(check(block, 13U, 77U));
    nph_file_close(&file);
}

NTEST_F(TempDir, seek_tell_truncate)
{
    char path[1024];
    path_in(f, "seek.bin", path, sizeof path);
    NphFile file;
    NTEST_ASSERT_EQ_INT(
        nph_file_open(path, NPH_FILE_READ | NPH_FILE_WRITE | NPH_FILE_CREATE, &file),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write(&file, "0123456789", 10U, NULL), NPH_RESULT_OK);
    u64 pos = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_seek(&file, 2, NPH_FILE_SEEK_SET, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pos, 2U);
    char c[4] = {0};
    u64 got = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read(&file, c, 3U, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 3U);
    NTEST_ASSERT_EQ_STR(c, "234");
    NTEST_ASSERT_EQ_INT(nph_file_seek(&file, -2, NPH_FILE_SEEK_CURRENT, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pos, 3U);
    NTEST_ASSERT_EQ_INT(nph_file_seek(&file, -1, NPH_FILE_SEEK_END, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pos, 9U);
    NTEST_ASSERT_EQ_INT(nph_file_read(&file, c, 3U, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 1U);
    NTEST_ASSERT_EQ_INT(c[0], '9');
    NTEST_ASSERT_EQ_INT(nph_file_seek(&file, -100, NPH_FILE_SEEK_SET, &pos),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_file_tell(&file, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pos, 10U);

    NTEST_ASSERT_EQ_INT(nph_file_truncate(&file, 4U), NPH_RESULT_OK);
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_size(&file, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, 4U);
    NTEST_ASSERT_EQ_INT(nph_file_truncate(&file, 8U), NPH_RESULT_OK); // extends with zeros
    u8 tail[4];
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, 4U, tail, 4U, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 4U);
    NTEST_ASSERT_EQ_UINT(tail[0] | tail[1] | tail[2] | tail[3], 0U);
    nph_file_close(&file);
}

NTEST_F(TempDir, async_mode_files_support_sequential_io)
{
    // The ASYNC flag changes the Windows handle type; the sequential API
    // must still behave the same.
    char path[1024];
    path_in(f, "async.bin", path, sizeof path);
    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(path,
                                      NPH_FILE_READ | NPH_FILE_WRITE | NPH_FILE_CREATE |
                                          NPH_FILE_TRUNCATE | NPH_FILE_ASYNC,
                                      &file),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write(&file, "abcdef", 6U, NULL), NPH_RESULT_OK);
    u64 pos = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_tell(&file, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pos, 6U);
    NTEST_ASSERT_EQ_INT(nph_file_seek(&file, 2, NPH_FILE_SEEK_SET, NULL), NPH_RESULT_OK);
    char c[8] = {0};
    u64 got = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read(&file, c, sizeof c, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 4U);
    NTEST_ASSERT_EQ_STR(c, "cdef");
    NTEST_ASSERT_EQ_INT(nph_file_read(&file, c, sizeof c, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 0U); // at the end
    NTEST_ASSERT_EQ_INT(nph_file_seek(&file, 0, NPH_FILE_SEEK_END, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(pos, 6U);
    NTEST_ASSERT_EQ_INT(nph_file_truncate(&file, 3U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_tell(&file, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_LE_UINT(pos, 3U);
    nph_file_close(&file);
}

NTEST_F(TempDir, memory_mapping)
{
    char path[1024];
    path_in(f, "map.bin", path, sizeof path);
    const u64 size = 300000U; // not page aligned
    fill(g_write_buf, size, 9U);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, g_write_buf, size), NPH_RESULT_OK);

    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(path, NPH_FILE_READ | NPH_FILE_WRITE, &file), NPH_RESULT_OK);

    // Whole-file read-only view.
    NphFileMapping map;
    NTEST_ASSERT_EQ_INT(nph_file_map(&file, 0U, 0U, NPH_FILE_MAP_READ, &map), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(map.size, size);
    NTEST_ASSERT_TRUE(check((const u8 *)map.data, size, 9U));
    nph_file_unmap(&map);
    NTEST_ASSERT_NULL(map.data);
    nph_file_unmap(&map); // idempotent

    // An unaligned sub-range.
    const u64 offset = 70001U;
    const u64 len = 1234U;
    NTEST_ASSERT_EQ_INT(nph_file_map(&file, offset, len, NPH_FILE_MAP_READ, &map), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(map.size, len);
    NTEST_ASSERT_EQ_MEM(map.data, g_write_buf + offset, len);
    nph_file_unmap(&map);

    // Copy-on-write: changes stay private.
    NTEST_ASSERT_EQ_INT(nph_file_map(&file, 0U, 0U, NPH_FILE_MAP_COPY, &map), NPH_RESULT_OK);
    ((u8 *)map.data)[0] = (u8)~g_write_buf[0];
    ((u8 *)map.data)[size - 1U] = (u8)~g_write_buf[size - 1U];
    nph_file_unmap(&map);
    u8 first = 0U;
    u64 got = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, 0U, &first, 1U, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(first, g_write_buf[0]);

    // Shared read-write: changes reach the file.
    NTEST_ASSERT_EQ_INT(nph_file_map(&file, 4096U, 8192U, NPH_FILE_MAP_READ_WRITE, &map),
                        NPH_RESULT_OK);
    memset(map.data, 0x5A, (size_t)map.size);
    NTEST_ASSERT_EQ_INT(nph_file_map_flush(&map), NPH_RESULT_OK);
    nph_file_unmap(&map);
    u8 probe[8192];
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, 4096U, probe, sizeof probe, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, sizeof probe);
    NTEST_ASSERT_EQ_UINT(probe[0], 0x5AU);
    NTEST_ASSERT_EQ_UINT(probe[8191], 0x5AU);
    NTEST_ASSERT_EQ_INT(nph_file_read_at(&file, 4095U, probe, 1U, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(probe[0], g_write_buf[4095]); // the byte before was untouched

    // Nothing to map.
    NTEST_ASSERT_EQ_INT(nph_file_map(&file, size, 0U, NPH_FILE_MAP_READ, &map),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_file_close(&file);
}

NTEST_F(TempDir, directories_and_listing)
{
    char nested[1024];
    char file_a[1024];
    char file_b[1024];
    path_in(f, "one/two/three", nested, sizeof nested);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(nested), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(nested), NPH_RESULT_OK); // idempotent
    NphFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_fs_stat(nested, &info), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(info.is_directory);
    NTEST_ASSERT_FALSE(info.is_regular);

    char one[1024];
    path_in(f, "one", one, sizeof one);
    NTEST_ASSERT_EQ_INT(
        nph_path_join(file_a, sizeof file_a, nph_str_from_cstr(one), NPH_STR("a.txt"), NULL),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_path_join(file_b, sizeof file_b, nph_str_from_cstr(one), NPH_STR("b.txt"), NULL),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(file_a, "A", 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(file_b, "BB", 2U), NPH_RESULT_OK);

    // A missing parent is NOT_FOUND; a file in the way is ALREADY_EXISTS.
    char bad[1024];
    path_in(f, "missing/child", bad, sizeof bad);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directory(bad), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directory(file_a), NPH_RESULT_ALREADY_EXISTS);

    // Listing "one": two files and one directory, in any order.
    NphDirIterator it;
    NTEST_ASSERT_EQ_INT(nph_dir_open(one, &it), NPH_RESULT_OK);
    u32 files = 0U;
    u32 dirs = 0U;
    NphDirEntry entry;
    while (nph_dir_next(&it, &entry)) {
        if (entry.is_directory) {
            NTEST_ASSERT_EQ_STR(entry.name, "two");
            ++dirs;
        } else {
            NTEST_ASSERT_TRUE(strcmp(entry.name, "a.txt") == 0 || strcmp(entry.name, "b.txt") == 0);
            ++files;
        }
    }
    NTEST_ASSERT_FALSE(nph_dir_next(&it, &entry)); // stays at the end
    nph_dir_close(&it);
    nph_dir_close(&it); // idempotent
    NTEST_ASSERT_EQ_UINT(files, 2U);
    NTEST_ASSERT_EQ_UINT(dirs, 1U);

    // An empty directory lists nothing; a file is not a directory.
    NTEST_ASSERT_EQ_INT(nph_dir_open(nested, &it), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_dir_next(&it, &entry));
    nph_dir_close(&it);
    NTEST_ASSERT_EQ_INT(nph_dir_open(file_a, &it), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_dir_open(bad, &it), NPH_RESULT_NOT_FOUND);

    // Rename replaces; remove of a non-empty directory is BUSY.
    NTEST_ASSERT_EQ_INT(nph_fs_rename(file_b, file_a), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_fs_exists(file_b));
    NTEST_ASSERT_EQ_INT(nph_fs_stat(file_a, &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.size, 2U);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_directory(one), NPH_RESULT_BUSY);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_file(file_a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_file(file_a), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_tree(one), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_fs_exists(one));
    NTEST_ASSERT_EQ_INT(nph_fs_stat(one, &info), NPH_RESULT_NOT_FOUND);
}

NTEST_F(TempDir, read_all_and_write_all)
{
    char path[1024];
    path_in(f, "all.bin", path, sizeof path);
    NphAllocator *a = nph_allocator_system();
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, a, &data, &size), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_NULL(data);

    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, NULL, 0U), NPH_RESULT_OK); // empty file
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, a, &data, &size), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(data);
    NTEST_ASSERT_EQ_UINT(size, 0U);
    NTEST_ASSERT_EQ_UINT(((const u8 *)data)[0], 0U);
    nph_file_free_all(a, data, size);

    fill(g_write_buf, BIG, 3U);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, g_write_buf, BIG), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, a, &data, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, BIG);
    NTEST_ASSERT_TRUE(check((const u8 *)data, BIG, 3U));
    NTEST_ASSERT_EQ_UINT(((const u8 *)data)[BIG], 0U);
    nph_file_free_all(a, data, size);
    nph_file_free_all(a, NULL, 0U); // harmless
}

NTEST(fs, current_and_temp_directories_exist)
{
    char buf[1024];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_fs_current_directory(buf, sizeof buf, &len), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(len, strlen(buf));
    NTEST_ASSERT_GT_UINT(len, 0U);
    NTEST_ASSERT_TRUE(nph_fs_exists(buf));
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(buf, sizeof buf, &len), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(len, strlen(buf));
    NTEST_ASSERT_TRUE(nph_fs_exists(buf));
    NTEST_ASSERT_FALSE(nph_path_is_separator(buf[len - 1U]));
    char tiny[2];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tiny, sizeof tiny, &len), NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_GT_UINT(len, 1U);
}
