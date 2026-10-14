// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/pack.h>
#include <nepheline/asset/vfs.h>

#include "test_asset_support.h"

NTEST(vfs, path_normalisation)
{
    char buf[128];
    NphStr out;
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR("a/b/c.nph"), buf, sizeof buf, &out),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "a/b/c.nph");
    NTEST_ASSERT_EQ_UINT(out.len, 9U);
    NTEST_ASSERT_EQ_INT(
        nph_vfs_normalize_path(NPH_STR("\\a\\\\b/./c/../d/"), buf, sizeof buf, &out),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "a/b/d");
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR("/x"), buf, sizeof buf, NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "x");
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR("a/../.."), buf, sizeof buf, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR("../a"), buf, sizeof buf, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR("./"), buf, sizeof buf, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR(""), buf, sizeof buf, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR("abcdefgh"), buf, 8U, NULL),
                        NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_EQ_INT(nph_vfs_normalize_path(NPH_STR("abcdefg"), buf, 8U, NULL), NPH_RESULT_OK);

    NTEST_ASSERT_TRUE(nph_vfs_path_is_normalized(NPH_STR("a/b/c.nph")));
    NTEST_ASSERT_TRUE(nph_vfs_path_is_normalized(NPH_STR("a")));
    NTEST_ASSERT_TRUE(nph_vfs_path_is_normalized(NPH_STR("a/.hidden")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("/a")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("a/")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("a//b")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("a\\b")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("a/./b")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("a/../b")));
    NTEST_ASSERT_FALSE(nph_vfs_path_is_normalized(NPH_STR("..")));
}

typedef struct VfsFixture {
    NphAllocator *alloc;
    TempDir dir;
    char loose[1200];     // a directory with loose files
    char override[1200];  // a second directory that shadows
    char pack_path[1200]; // a pack
    NphVfs *vfs;
} VfsFixture;

static void write_text_file(const char *dir, const char *rel, const char *text)
{
    char path[1400];
    NTEST_ASSERT_EQ_INT(
        nph_path_join(path, sizeof path, nph_str_from_cstr(dir), nph_str_from_cstr(rel), NULL),
        NPH_RESULT_OK);
    const NphStr d = nph_path_dirname(nph_str_from_cstr(path));
    char parent[1400];
    memcpy(parent, d.data, (size_t)d.len);
    parent[d.len] = '\0';
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(parent), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, text, strlen(text)), NPH_RESULT_OK);
}

NTEST_SETUP(VfsFixture)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "vfs");
    test_path_in(&f->dir, "loose", f->loose, sizeof f->loose);
    test_path_in(&f->dir, "override", f->override, sizeof f->override);
    test_path_in(&f->dir, "content.npk", f->pack_path, sizeof f->pack_path);
    write_text_file(f->loose, "a.txt", "loose a");
    write_text_file(f->loose, "sub/b.txt", "loose b");
    write_text_file(f->loose, "sub/deep/c.txt", "loose c");
    write_text_file(f->loose, "empty.txt", "");
    write_text_file(f->override, "sub/b.txt", "override b");
    write_text_file(f->override, "only.txt", "override only");

    NphPackWriter w;
    NTEST_ASSERT_EQ_INT(nph_pack_writer_begin(&w, f->alloc, f->pack_path, 16U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, NPH_STR("a.txt"), nph_guid_from_cstr("pa"),
                                            NPH_ASSET_TYPE_BLOB, "pack a", 6U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_add(&w, NPH_STR("p/q.txt"), nph_guid_from_cstr("pq"),
                                            NPH_ASSET_TYPE_BLOB, "pack q", 6U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_pack_writer_finish(&w), NPH_RESULT_OK);

    NTEST_ASSERT_EQ_INT(nph_vfs_create(f->alloc, &f->vfs), NPH_RESULT_OK);
}

NTEST_TEARDOWN(VfsFixture)
{
    nph_vfs_destroy(f->vfs);
    test_temp_dir_remove(&f->dir);
}

static void expect_text(VfsFixture *f, const char *path, const char *text)
{
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_vfs_read_all(f->vfs, nph_str_from_cstr(path), f->alloc, &data, &size),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, strlen(text));
    NTEST_ASSERT_EQ_STR((const char *)data, text);
    nph_vfs_free_all(f->alloc, data, size);
}

NTEST_F(VfsFixture, mounts_resolve_newest_first)
{
    u32 pack_id = 0U;
    u32 loose_id = 0U;
    u32 override_id = 0U;
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("game"), f->pack_path, &pack_id),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("game/"), f->loose, &loose_id),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_vfs_mount_directory(f->vfs, NPH_STR("/game\\"), f->override, &override_id),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_vfs_mount_count(f->vfs), 3U);
    const NphVfsMountInfo m0 = nph_vfs_mount_info(f->vfs, 0U);
    NTEST_ASSERT_EQ_INT(m0.type, NPH_VFS_MOUNT_PACK);
    NTEST_ASSERT_TRUE(nph_str_equals(m0.prefix, NPH_STR("game/")));
    NTEST_ASSERT_EQ_UINT(m0.id, pack_id);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_vfs_mount_info(f->vfs, 2U).prefix, NPH_STR("game/")));

    // The newest mount wins; the others serve what it lacks.
    expect_text(f, "game/sub/b.txt", "override b");
    expect_text(f, "game/only.txt", "override only");
    expect_text(f, "game/a.txt", "loose a");
    expect_text(f, "game/sub/deep/c.txt", "loose c");
    expect_text(f, "game/p/q.txt", "pack q");
    expect_text(f, "game/empty.txt", "");

    NphVfsInfo info;
    NTEST_ASSERT_EQ_INT(nph_vfs_stat(f->vfs, NPH_STR("game/a.txt"), &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.mount_id, loose_id);
    NTEST_ASSERT_EQ_INT(info.source, NPH_VFS_MOUNT_DIRECTORY);
    NTEST_ASSERT_EQ_UINT(info.size, 7U);
    NTEST_ASSERT_EQ_INT(nph_vfs_stat(f->vfs, NPH_STR("game/p/q.txt"), &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.mount_id, pack_id);
    NTEST_ASSERT_EQ_INT(info.source, NPH_VFS_MOUNT_PACK);
    NTEST_ASSERT_EQ_UINT(info.size, 6U);
    NTEST_ASSERT_EQ_INT(nph_vfs_stat(f->vfs, NPH_STR("game/sub"), &info), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(info.is_directory);
    NTEST_ASSERT_EQ_INT(nph_vfs_stat(f->vfs, NPH_STR("game/nope.txt"), &info),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_vfs_stat(f->vfs, NPH_STR("a.txt"), &info), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_vfs_stat(f->vfs, NPH_STR("game/../a.txt"), &info),
                        NPH_RESULT_NOT_FOUND); // not normalised
    NTEST_ASSERT_TRUE(nph_vfs_exists(f->vfs, NPH_STR("game/only.txt")));
    NTEST_ASSERT_FALSE(nph_vfs_exists(f->vfs, NPH_STR("game/other.txt")));

    // Reading a directory is refused.
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_vfs_read_all(f->vfs, NPH_STR("game/sub"), f->alloc, &data, &size),
                        NPH_RESULT_INVALID_ARGUMENT);

    // Unmounting the override reveals the loose file again.
    NTEST_ASSERT_EQ_INT(nph_vfs_unmount(f->vfs, override_id), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_unmount(f->vfs, override_id), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_UINT(nph_vfs_mount_count(f->vfs), 2U);
    expect_text(f, "game/sub/b.txt", "loose b");
    NTEST_ASSERT_FALSE(nph_vfs_exists(f->vfs, NPH_STR("game/only.txt")));
    NTEST_ASSERT_EQ_INT(nph_vfs_unmount(f->vfs, loose_id), NPH_RESULT_OK);
    expect_text(f, "game/a.txt", "pack a");
}

NTEST_F(VfsFixture, mapping_and_handles)
{
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, nph_str_empty(), f->pack_path, NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("loose"), f->loose, NULL),
                        NPH_RESULT_OK);

    NphVfsMapping m;
    NTEST_ASSERT_EQ_INT(nph_vfs_map(f->vfs, NPH_STR("loose/sub/b.txt"), &m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(m.source, NPH_VFS_MOUNT_DIRECTORY);
    NTEST_ASSERT_EQ_UINT(m.size, 7U);
    NTEST_ASSERT_EQ_MEM(m.data, "loose b", 7U);
    nph_vfs_unmap(f->vfs, &m);
    NTEST_ASSERT_EQ_INT(nph_vfs_map(f->vfs, NPH_STR("p/q.txt"), &m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(m.source, NPH_VFS_MOUNT_PACK);
    NTEST_ASSERT_EQ_MEM(m.data, "pack q", 6U);
    nph_vfs_unmap(f->vfs, &m);
    NTEST_ASSERT_EQ_INT(nph_vfs_map(f->vfs, NPH_STR("loose/empty.txt"), &m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.size, 0U);
    nph_vfs_unmap(f->vfs, &m);
    nph_vfs_unmap(f->vfs, &m); // idempotent
    NTEST_ASSERT_EQ_INT(nph_vfs_map(f->vfs, NPH_STR("loose/sub"), &m), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_vfs_map(f->vfs, NPH_STR("missing"), &m), NPH_RESULT_NOT_FOUND);

    NphVfsFile h;
    char buf[16];
    u64 got = 0U;
    NTEST_ASSERT_EQ_INT(nph_vfs_open(f->vfs, NPH_STR("p/q.txt"), 0U, &h), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(h.size, 6U);
    NTEST_ASSERT_EQ_INT(h.source, NPH_VFS_MOUNT_PACK);
    NTEST_ASSERT_EQ_INT(nph_vfs_read_at(&h, 5U, buf, sizeof buf, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 1U); // bounded to the entry
    NTEST_ASSERT_EQ_INT(buf[0], 'q');
    NTEST_ASSERT_EQ_INT(nph_vfs_read_at(&h, 6U, buf, sizeof buf, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_read_at(&h, 0U, buf, 4U, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 4U);
    NTEST_ASSERT_EQ_MEM(buf, "pack", 4U);
    nph_vfs_close(f->vfs, &h);
    NTEST_ASSERT_EQ_INT(nph_vfs_open(f->vfs, NPH_STR("loose/a.txt"), NPH_FILE_SEQUENTIAL, &h),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(h.source, NPH_VFS_MOUNT_DIRECTORY);
    NTEST_ASSERT_EQ_UINT(h.size, 7U);
    NTEST_ASSERT_EQ_INT(nph_vfs_read_at(&h, 6U, buf, sizeof buf, &got), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(got, 1U);
    NTEST_ASSERT_EQ_INT(buf[0], 'a');
    nph_vfs_close(f->vfs, &h);
    nph_vfs_close(f->vfs, &h);

    char os_path[1400];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_vfs_resolve_directory(f->vfs, NPH_STR("loose/new/file.nph"), os_path,
                                                  sizeof os_path, &len),
                        NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_ends_with(nph_str(os_path, len), NPH_STR("/new/file.nph")));
    NTEST_ASSERT_TRUE(nph_str_starts_with(nph_str(os_path, len), nph_str_from_cstr(f->loose)));
    NTEST_ASSERT_EQ_INT(
        nph_vfs_resolve_directory(f->vfs, NPH_STR("p/q.txt"), os_path, sizeof os_path, &len),
        NPH_RESULT_NOT_FOUND); // only a pack covers it
}

typedef struct Visited {
    char paths[16][64];
    u64 sizes[16];
    u32 count;
    u32 stop_after;
} Visited;

static bool visit(NphStr path, const NphVfsInfo *info, void *user)
{
    Visited *v = (Visited *)user;
    if (v->count < 16U) {
        memcpy(v->paths[v->count], path.data, (size_t)path.len);
        v->paths[v->count][path.len] = '\0';
        v->sizes[v->count] = info->size;
    }
    v->count += 1U;
    return v->stop_after == 0U || v->count < v->stop_after;
}

static bool visited_has(const Visited *v, const char *path, u64 size)
{
    for (u32 i = 0; i < v->count && i < 16U; ++i) {
        if (strcmp(v->paths[i], path) == 0) {
            return v->sizes[i] == size;
        }
    }
    return false;
}

NTEST_F(VfsFixture, iteration_visits_each_path_once)
{
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("game"), f->pack_path, NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("game"), f->loose, NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("game"), f->override, NULL),
                        NPH_RESULT_OK);
    Visited v;
    memset(&v, 0, sizeof v);
    NTEST_ASSERT_EQ_INT(nph_vfs_iterate(f->vfs, nph_str_empty(), visit, &v), NPH_RESULT_OK);
    // a.txt (loose over pack), sub/b.txt (override), sub/deep/c.txt, empty.txt,
    // only.txt, p/q.txt: six distinct files.
    NTEST_ASSERT_EQ_UINT(v.count, 6U);
    NTEST_ASSERT_TRUE(visited_has(&v, "game/a.txt", 7U));
    NTEST_ASSERT_TRUE(visited_has(&v, "game/sub/b.txt", 10U));
    NTEST_ASSERT_TRUE(visited_has(&v, "game/sub/deep/c.txt", 7U));
    NTEST_ASSERT_TRUE(visited_has(&v, "game/empty.txt", 0U));
    NTEST_ASSERT_TRUE(visited_has(&v, "game/only.txt", 13U));
    NTEST_ASSERT_TRUE(visited_has(&v, "game/p/q.txt", 6U));

    // A prefix restricts the walk.
    memset(&v, 0, sizeof v);
    NTEST_ASSERT_EQ_INT(nph_vfs_iterate(f->vfs, NPH_STR("game/sub/"), visit, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.count, 2U);
    NTEST_ASSERT_TRUE(visited_has(&v, "game/sub/b.txt", 10U));
    NTEST_ASSERT_TRUE(visited_has(&v, "game/sub/deep/c.txt", 7U));
    memset(&v, 0, sizeof v);
    NTEST_ASSERT_EQ_INT(nph_vfs_iterate(f->vfs, NPH_STR("game/p"), visit, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.count, 1U);
    memset(&v, 0, sizeof v);
    NTEST_ASSERT_EQ_INT(nph_vfs_iterate(f->vfs, NPH_STR("other/"), visit, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.count, 0U);
    // Early stop.
    memset(&v, 0, sizeof v);
    v.stop_after = 2U;
    NTEST_ASSERT_EQ_INT(nph_vfs_iterate(f->vfs, nph_str_empty(), visit, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.count, 2U);
}

NTEST_F(VfsFixture, mount_errors)
{
    char missing[1200];
    test_path_in(&f->dir, "missing.npk", missing, sizeof missing);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("x"), missing, NULL),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("../x"), f->loose, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(nph_vfs_mount_count(f->vfs), 0U);
    for (u32 i = 0; i < NPH_VFS_MAX_MOUNTS; ++i) {
        NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("m"), f->loose, NULL),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("m"), f->loose, NULL),
                        NPH_RESULT_FULL);
}
