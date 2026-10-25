// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/loader.h>
#include <nepheline/asset/pack.h>
#include <nepheline/platform/cpu.h>

#include "test_asset_support.h"

#define TAG_DATA NPH_FOURCC('D', 'A', 'T', 'A')

typedef struct LoaderFixture {
    NphAllocator *alloc;
    TempDir dir;
    char loose[1200];
    char pack_path[1200];
    NphVfs *vfs;
    NphJobSystem *jobs;
    NphAssetLoader *loader;
} LoaderFixture;

// Writes "<loose>/<name>" as a BLOB asset with `blob_count` blobs of
// `blob_size` bytes (pattern seed = blob index) and the given codec.
static void write_asset(LoaderFixture *f, const char *name, u32 blob_count, u64 blob_size,
                        NphAssetCodec codec)
{
    void *image = NULL;
    u64 size = 0U;
    test_make_blob_asset(f->alloc, nph_guid_from_cstr(name), blob_count, blob_size, codec, &image,
                         &size);
    char path[1400];
    NTEST_ASSERT_EQ_INT(nph_path_join(path, sizeof path, nph_str_from_cstr(f->loose),
                                      nph_str_from_cstr(name), NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, image, size), NPH_RESULT_OK);
    test_free_asset_image(f->alloc, image, size);
}

NTEST_SETUP(LoaderFixture)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "loader");
    test_path_in(&f->dir, "loose", f->loose, sizeof f->loose);
    test_path_in(&f->dir, "content.npk", f->pack_path, sizeof f->pack_path);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->loose), NPH_RESULT_OK);
    write_asset(f, "stored.nph", 3U, 1000U, NPH_ASSET_CODEC_NONE);
    write_asset(f, "packed.nph", 3U, NPH_KIB(40), NPH_ASSET_CODEC_LZ);
    write_asset(f, "empty.nph", 0U, 0U, NPH_ASSET_CODEC_NONE);
    write_asset(f, "big.nph", 8U, NPH_KIB(128), NPH_ASSET_CODEC_LZ);
    NTEST_ASSERT_EQ_INT(nph_vfs_create(f->alloc, &f->vfs), NPH_RESULT_OK);
}

NTEST_TEARDOWN(LoaderFixture)
{
    nph_asset_loader_destroy(f->loader);
    nph_job_system_destroy(f->jobs);
    nph_vfs_destroy(f->vfs);
    test_temp_dir_remove(&f->dir);
}

// A resolver over a fixed table of (name, path) identities.
typedef struct TestResolver {
    const char *const *names;
    const char *const *paths;
    u32 count;
    u32 calls;
} TestResolver;

static NphResult test_resolve(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len)
{
    TestResolver *r = (TestResolver *)user;
    r->calls += 1U;
    for (u32 i = 0; i < r->count; ++i) {
        if (nph_guid_equals(guid, nph_guid_from_cstr(r->names[i]))) {
            const u64 len = strlen(r->paths[i]);
            if (len + 1U > cap) {
                return NPH_RESULT_OVERFLOW;
            }
            memcpy(buf, r->paths[i], len + 1U);
            *out_len = len;
            return NPH_RESULT_OK;
        }
    }
    return NPH_RESULT_NOT_FOUND;
}

static TestResolver *g_resolver; // set by tests that want one, read by make_loader

static void make_loader(LoaderFixture *f, bool with_jobs, NphAssetVerify verify, u32 max_assets)
{
    if (with_jobs) {
        NphJobSystemDesc jd;
        memset(&jd, 0, sizeof jd);
        jd.alloc = f->alloc;
        jd.worker_count = nph_clamp_u32(nph_cpu_logical_count(), 2U, 4U);
        NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &f->jobs), NPH_RESULT_OK);
    }
    NphAssetLoaderDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.vfs = f->vfs;
    desc.jobs = f->jobs;
    desc.verify = verify;
    desc.max_assets = max_assets;
    desc.parallel_threshold = 1U; // every multi-blob sync load uses jobs when they exist
    if (g_resolver != NULL) {
        desc.resolve = test_resolve;
        desc.resolve_user = g_resolver;
    }
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&desc, &f->loader), NPH_RESULT_OK);
}

static void check_blobs(const NphAsset *a, u32 blob_count, u64 blob_size, NphAssetCodec codec)
{
    NTEST_ASSERT_EQ_UINT(a->blob_count, blob_count);
    for (u32 i = 0; i < blob_count; ++i) {
        const NphAssetBlob *b = nph_asset_blob(a, TAG_DATA, i);
        NTEST_ASSERT_NOT_NULL(b);
        NTEST_ASSERT_EQ_UINT(b->size, blob_size);
        NTEST_ASSERT_EQ_UINT(b->codec, (u32)codec);
        NTEST_ASSERT_TRUE(nph_is_ptr_aligned(b->data, b->alignment));
        NTEST_ASSERT_TRUE(test_check_pattern(b->data, blob_size, i));
    }
    NTEST_ASSERT_NULL(nph_asset_blob(a, TAG_DATA, blob_count));
}

// A stored blob read again through the VFS at its offset is its bytes: how
// a sound streams (sound.h), from loose files and packs alike.
static void check_offsets(LoaderFixture *f, NphStr path, const NphAsset *a)
{
    NphVfsFile file;
    NTEST_ASSERT_EQ_INT(nph_vfs_open(f->vfs, path, 0U, &file), NPH_RESULT_OK);
    for (u32 i = 0; i < a->blob_count; ++i) {
        const NphAssetBlob *b = &a->blobs[i];
        if (b->codec != NPH_ASSET_CODEC_NONE) {
            continue;
        }
        u8 bytes[1000];
        NTEST_ASSERT_LE_UINT(b->size, sizeof bytes);
        u64 got = 0U;
        NTEST_EXPECT_EQ_INT(nph_vfs_read_at(&file, b->offset, bytes, b->size, &got), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(got, b->size);
        NTEST_EXPECT_TRUE(memcmp(bytes, b->data, (size_t)b->size) == 0);
    }
    nph_vfs_close(f->vfs, &file);
}

NTEST_F(LoaderFixture, sync_loads_from_a_directory)
{
    make_loader(f, false, NPH_ASSET_VERIFY_DEFAULT, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("game"), f->loose, NULL),
                        NPH_RESULT_OK);
    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("game/stored.nph"), &h), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_asset_handle_is_null(h));
    NTEST_ASSERT_EQ_INT(nph_asset_state(f->loader, h), NPH_ASSET_STATE_READY);
    NTEST_ASSERT_EQ_INT(nph_asset_error(f->loader, h), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, h), NPH_RESULT_OK);
    const NphAsset *a = nph_asset_get(f->loader, h);
    NTEST_ASSERT_NOT_NULL(a);
    NTEST_ASSERT_EQ_UINT(a->header.type, NPH_ASSET_TYPE_BLOB);
    NTEST_ASSERT_TRUE(nph_guid_equals(a->header.guid, nph_guid_from_cstr("stored.nph")));
    NTEST_ASSERT_TRUE(nph_str_equals(a->path, NPH_STR("game/stored.nph")));
    check_blobs(a, 3U, 1000U, NPH_ASSET_CODEC_NONE);
    check_offsets(f, a->path, a);
    NTEST_ASSERT_EQ_UINT(nph_asset_find_guid(f->loader, nph_guid_from_cstr("stored.nph")).value,
                         h.value);
    NTEST_ASSERT_TRUE(
        nph_asset_handle_is_null(nph_asset_find_guid(f->loader, nph_guid_from_cstr("other"))));

    // The same path again is the same handle with another reference.
    NphAssetHandle again;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("game/stored.nph"), &again),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(again.value, h.value);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 1U);
    nph_asset_acquire(f->loader, h);
    nph_asset_release(f->loader, h);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 1U);
    NTEST_ASSERT_NOT_NULL(nph_asset_get(f->loader, h));
    nph_asset_release(f->loader, again);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
    NTEST_ASSERT_NULL(nph_asset_get(f->loader, h)); // stale
    NTEST_ASSERT_EQ_INT(nph_asset_state(f->loader, h), NPH_ASSET_STATE_NONE);
    NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, h), NPH_RESULT_NOT_FOUND);
    nph_asset_release(f->loader, h); // ignored
    nph_asset_release(f->loader, NPH_ASSET_HANDLE_NULL);

    // Compressed blobs decompress; an asset with no blobs is fine.
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("game/packed.nph"), &h), NPH_RESULT_OK);
    check_blobs(nph_asset_get(f->loader, h), 3U, NPH_KIB(40), NPH_ASSET_CODEC_LZ);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("game/empty.nph"), &h), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_asset_get(f->loader, h)->blob_count, 0U);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, sync_loads_from_a_pack)
{
    make_loader(f, false, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NphPackWriter w;
    NTEST_ASSERT_EQ_INT(nph_pack_writer_begin(&w, f->alloc, f->pack_path, 0U), NPH_RESULT_OK);
    const char *names[] = {"stored.nph", "packed.nph", "empty.nph"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(names); ++i) {
        char path[1400];
        NTEST_ASSERT_EQ_INT(nph_path_join(path, sizeof path, nph_str_from_cstr(f->loose),
                                          nph_str_from_cstr(names[i]), NULL),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_pack_writer_add_file(&w, nph_str_from_cstr(names[i]),
                                                     nph_guid_from_cstr(names[i]),
                                                     NPH_ASSET_TYPE_BLOB, path),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_pack_writer_finish(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("pak"), f->pack_path, NULL),
                        NPH_RESULT_OK);

    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("pak/stored.nph"), &h), NPH_RESULT_OK);
    check_blobs(nph_asset_get(f->loader, h), 3U, 1000U, NPH_ASSET_CODEC_NONE);
    check_offsets(f, NPH_STR("pak/stored.nph"), nph_asset_get(f->loader, h));
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("pak/packed.nph"), &h), NPH_RESULT_OK);
    check_blobs(nph_asset_get(f->loader, h), 3U, NPH_KIB(40), NPH_ASSET_CODEC_LZ);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("pak/empty.nph"), &h), NPH_RESULT_OK);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, load_failures_leave_nothing_behind)
{
    make_loader(f, false, NPH_ASSET_VERIFY_ALWAYS, 2U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    NphAssetHandle h = {123U};
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("missing.nph"), &h),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_TRUE(nph_asset_handle_is_null(h));
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("../stored.nph"), &h),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);

    // A file that is not an asset.
    char junk[1400];
    NTEST_ASSERT_EQ_INT(
        nph_path_join(junk, sizeof junk, nph_str_from_cstr(f->loose), NPH_STR("junk.nph"), NULL),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(junk, "not an asset at all, sorry", 26U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("junk.nph"), &h), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);

    // A stored blob with a flipped byte fails its checksum.
    char stored[1400];
    NTEST_ASSERT_EQ_INT(nph_path_join(stored, sizeof stored, nph_str_from_cstr(f->loose),
                                      NPH_STR("stored.nph"), NULL),
                        NPH_RESULT_OK);
    void *image = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(stored, f->alloc, &image, &size), NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(image, size, &info), NPH_RESULT_OK);
    const NphAssetBlobEntry *e = nph_asset_file_find(&info, TAG_DATA, 1U);
    ((u8 *)image)[e->offset + 10U] ^= 0x40U;
    char damaged[1400];
    NTEST_ASSERT_EQ_INT(nph_path_join(damaged, sizeof damaged, nph_str_from_cstr(f->loose),
                                      NPH_STR("damaged.nph"), NULL),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(damaged, image, size), NPH_RESULT_OK);
    nph_file_free_all(f->alloc, image, size);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("damaged.nph"), &h), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);

    // max_assets is enforced.
    NphAssetHandle a;
    NphAssetHandle b;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("stored.nph"), &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("packed.nph"), &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("empty.nph"), &h), NPH_RESULT_FULL);
    nph_asset_release(f->loader, a);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("empty.nph"), &h), NPH_RESULT_OK);
    nph_asset_release(f->loader, b);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, verify_never_skips_the_checksums)
{
    make_loader(f, false, NPH_ASSET_VERIFY_NEVER, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    char stored[1400];
    NTEST_ASSERT_EQ_INT(nph_path_join(stored, sizeof stored, nph_str_from_cstr(f->loose),
                                      NPH_STR("stored.nph"), NULL),
                        NPH_RESULT_OK);
    void *image = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(stored, f->alloc, &image, &size), NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(image, size, &info), NPH_RESULT_OK);
    const NphAssetBlobEntry *e = nph_asset_file_find(&info, TAG_DATA, 0U);
    ((u8 *)image)[e->offset] ^= 0x01U;
    NTEST_ASSERT_EQ_INT(nph_file_write_all(stored, image, size), NPH_RESULT_OK);
    nph_file_free_all(f->alloc, image, size);
    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("stored.nph"), &h), NPH_RESULT_OK);
    const NphAssetBlob *b = nph_asset_blob(nph_asset_get(f->loader, h), TAG_DATA, 0U);
    NTEST_ASSERT_FALSE(test_check_pattern(b->data, b->size, 0U)); // the damage went through
    nph_asset_release(f->loader, h);
}

NTEST_F(LoaderFixture, sync_loads_decompress_in_parallel_with_a_job_system)
{
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("big.nph"), &h), NPH_RESULT_OK);
    check_blobs(nph_asset_get(f->loader, h), 8U, NPH_KIB(128), NPH_ASSET_CODEC_LZ);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("stored.nph"), &h), NPH_RESULT_OK);
    check_blobs(nph_asset_get(f->loader, h), 3U, 1000U, NPH_ASSET_CODEC_NONE);
    nph_asset_release(f->loader, h);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, async_loads_signal_a_counter)
{
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    const char *names[] = {"stored.nph", "packed.nph", "empty.nph", "big.nph"};
    NphAssetHandle handles[4];
    NphJobCounter counter = NPH_JOB_COUNTER_INIT;
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_EQ_INT(
            nph_asset_load_async(f->loader, nph_str_from_cstr(names[i]), &counter, &handles[i]),
            NPH_RESULT_OK);
        NTEST_ASSERT_FALSE(nph_asset_handle_is_null(handles[i]));
    }
    // A second request for a file in flight shares the handle and gets
    // its own counter signalled.
    NphJobCounter second = NPH_JOB_COUNTER_INIT;
    NphAssetHandle dup;
    NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("big.nph"), &second, &dup),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(dup.value, handles[3].value);
    nph_job_wait(f->jobs, &counter);
    nph_job_wait(f->jobs, &second);
    for (u32 i = 0; i < 4U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_asset_state(f->loader, handles[i]), NPH_ASSET_STATE_READY);
        NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, handles[i]), NPH_RESULT_OK);
    }
    check_blobs(nph_asset_get(f->loader, handles[0]), 3U, 1000U, NPH_ASSET_CODEC_NONE);
    check_blobs(nph_asset_get(f->loader, handles[1]), 3U, NPH_KIB(40), NPH_ASSET_CODEC_LZ);
    check_blobs(nph_asset_get(f->loader, handles[2]), 0U, 0U, NPH_ASSET_CODEC_NONE);
    check_blobs(nph_asset_get(f->loader, handles[3]), 8U, NPH_KIB(128), NPH_ASSET_CODEC_LZ);
    // A sync load of a READY asset returns the same handle.
    NphAssetHandle again;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("big.nph"), &again), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(again.value, handles[3].value);
    nph_asset_release(f->loader, again);
    nph_asset_release(f->loader, dup);
    for (u32 i = 0; i < 4U; ++i) {
        nph_asset_release(f->loader, handles[i]);
    }
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);

    // A failed asynchronous load stays FAILED, with its error, until released.
    NphAssetHandle bad;
    NphJobCounter c2 = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("missing.nph"), &c2, &bad),
                        NPH_RESULT_OK);
    nph_job_wait(f->jobs, &c2);
    NTEST_ASSERT_EQ_INT(nph_asset_state(f->loader, bad), NPH_ASSET_STATE_FAILED);
    NTEST_ASSERT_EQ_INT(nph_asset_error(f->loader, bad), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, bad), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_NULL(nph_asset_get(f->loader, bad));
    // A sync load of the failed path reports the failure and does not
    // keep a second handle.
    NphAssetHandle h2;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("missing.nph"), &h2),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 1U);
    nph_asset_release(f->loader, bad);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, releasing_during_a_load_unloads_when_it_finishes)
{
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    for (u32 round = 0; round < 20U; ++round) {
        NphJobCounter counter = NPH_JOB_COUNTER_INIT;
        NphAssetHandle h;
        NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("big.nph"), &counter, &h),
                            NPH_RESULT_OK);
        nph_asset_release(f->loader, h);
        nph_job_wait(f->jobs, &counter);
    }
    // Everything the pipelines held is gone once they finished.
    NphJobCounter drain = NPH_JOB_COUNTER_INIT;
    NphAssetHandle last;
    NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("stored.nph"), &drain, &last),
                        NPH_RESULT_OK);
    nph_job_wait(f->jobs, &drain);
    NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, last), NPH_RESULT_OK);
    nph_asset_release(f->loader, last);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

// Replaces "<loose>/<name>" whole, as a cooker does: written beside it and
// renamed over it, so a loader that mapped it keeps the old file.
static void replace_asset(LoaderFixture *f, const char *name, u32 blob_count, u64 blob_size)
{
    void *image = NULL;
    u64 size = 0U;
    test_make_blob_asset(f->alloc, nph_guid_from_cstr(name), blob_count, blob_size,
                         NPH_ASSET_CODEC_NONE, &image, &size);
    char path[1400];
    char part[1500];
    NTEST_ASSERT_EQ_INT(nph_path_join(path, sizeof path, nph_str_from_cstr(f->loose),
                                      nph_str_from_cstr(name), NULL),
                        NPH_RESULT_OK);
    (void)snprintf(part, sizeof part, "%s.part", path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(part, image, size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_fs_rename(part, path), NPH_RESULT_OK);
    test_free_asset_image(f->alloc, image, size);
}

NTEST_F(LoaderFixture, a_forgotten_asset_is_read_again_and_its_holders_keep_it)
{
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    const NphGuid guid = nph_guid_from_cstr("replaced.nph");
    replace_asset(f, "replaced.nph", 2U, 500U);
    NphAssetHandle old_h;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("replaced.nph"), &old_h), NPH_RESULT_OK);
    const NphAsset *old_asset = nph_asset_get(f->loader, old_h);
    NTEST_ASSERT_NOT_NULL(old_asset);
    check_blobs(old_asset, 2U, 500U, NPH_ASSET_CODEC_NONE);

    // The file is replaced; until forgotten, a load is the asset already there.
    replace_asset(f, "replaced.nph", 3U, 700U);
    NphAssetHandle again;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("replaced.nph"), &again), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(again.value, old_h.value);
    nph_asset_release(f->loader, again);

    // Forgotten: the path loads the new file into a new asset, which takes
    // the GUID, and the old one stays whole for its holder.
    nph_asset_forget(f->loader, old_h);
    nph_asset_forget(f->loader, old_h); // twice is once
    NTEST_EXPECT_EQ_UINT(nph_asset_find_guid(f->loader, guid).value, 0U);
    NphAssetHandle new_h;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("replaced.nph"), &new_h), NPH_RESULT_OK);
    NTEST_EXPECT_NE_UINT(new_h.value, old_h.value);
    check_blobs(nph_asset_get(f->loader, new_h), 3U, 700U, NPH_ASSET_CODEC_NONE);
    check_blobs(old_asset, 2U, 500U, NPH_ASSET_CODEC_NONE);
    NTEST_EXPECT_EQ_UINT(nph_asset_find_guid(f->loader, guid).value, new_h.value);
    NTEST_EXPECT_EQ_UINT(nph_asset_loader_live(f->loader), 2U);
    // Releasing the old one leaves the new one where it is.
    nph_asset_release(f->loader, old_h);
    NTEST_EXPECT_EQ_UINT(nph_asset_find_guid(f->loader, guid).value, new_h.value);
    NphAssetHandle same;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("replaced.nph"), &same), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(same.value, new_h.value);
    nph_asset_release(f->loader, same);
    nph_asset_release(f->loader, new_h);

    // Forgotten while its load is in flight: the load lands without taking
    // the GUID back from the asset loaded after it.
    for (u32 round = 0; round < 8U; ++round) {
        NphJobCounter first = NPH_JOB_COUNTER_INIT;
        NphJobCounter second = NPH_JOB_COUNTER_INIT;
        NphAssetHandle a;
        NphAssetHandle b;
        NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("big.nph"), &first, &a),
                            NPH_RESULT_OK);
        nph_asset_forget(f->loader, a);
        NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("big.nph"), &second, &b),
                            NPH_RESULT_OK);
        NTEST_EXPECT_NE_UINT(a.value, b.value);
        nph_job_wait(f->jobs, &first);
        nph_job_wait(f->jobs, &second);
        NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, a), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, b), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(nph_asset_find_guid(f->loader, nph_guid_from_cstr("big.nph")).value,
                             b.value);
        nph_asset_release(f->loader, a);
        nph_asset_release(f->loader, b);
    }
    nph_asset_forget(f->loader, NPH_ASSET_HANDLE_NULL); // ignored
    NTEST_EXPECT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, many_concurrent_async_loads)
{
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    // Forty distinct files, each requested several times while in flight.
    for (u32 i = 0; i < 40U; ++i) {
        char name[32];
        snprintf(name, sizeof name, "many%u.nph", i);
        write_asset(f, name, 2U + i % 3U, NPH_KIB(8) + (u64)i * 100U,
                    i % 2U == 0U ? NPH_ASSET_CODEC_LZ : NPH_ASSET_CODEC_NONE);
    }
    NphJobCounter counter = NPH_JOB_COUNTER_INIT;
    NphAssetHandle handles[40][3];
    for (u32 rep = 0; rep < 3U; ++rep) {
        for (u32 i = 0; i < 40U; ++i) {
            char name[32];
            snprintf(name, sizeof name, "many%u.nph", i);
            NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, nph_str_from_cstr(name), &counter,
                                                     &handles[i][rep]),
                                NPH_RESULT_OK);
        }
    }
    nph_job_wait(f->jobs, &counter);
    for (u32 i = 0; i < 40U; ++i) {
        NTEST_ASSERT_EQ_UINT(handles[i][1].value, handles[i][0].value);
        NTEST_ASSERT_EQ_UINT(handles[i][2].value, handles[i][0].value);
        NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, handles[i][0]), NPH_RESULT_OK);
        check_blobs(nph_asset_get(f->loader, handles[i][0]), 2U + i % 3U,
                    NPH_KIB(8) + (u64)i * 100U,
                    i % 2U == 0U ? NPH_ASSET_CODEC_LZ : NPH_ASSET_CODEC_NONE);
        for (u32 rep = 0; rep < 3U; ++rep) {
            nph_asset_release(f->loader, handles[i][rep]);
        }
    }
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, destroy_waits_for_loads_and_reports_leaks)
{
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, nph_str_empty(), f->loose, NULL),
                        NPH_RESULT_OK);
    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("big.nph"), NULL, &h),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("stored.nph"), &h), NPH_RESULT_OK);
    // Neither is released: destroy must wait for the first and log both.
    nph_asset_loader_destroy(f->loader);
    f->loader = NULL;
    NphAssetLoaderDesc bad;
    memset(&bad, 0, sizeof bad);
    NphAssetLoader *none = NULL;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&bad, &none), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(none);
    nph_asset_loader_destroy(NULL);
}

NTEST_F(LoaderFixture, async_needs_a_job_system)
{
    make_loader(f, false, NPH_ASSET_VERIFY_DEFAULT, 0U);
    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load_async(f->loader, NPH_STR("stored.nph"), NULL, &h),
                        NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_TRUE(nph_asset_handle_is_null(h));
}

NTEST_F(LoaderFixture, loads_by_guid_through_the_pack_index)
{
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    NphPackWriter w;
    NTEST_ASSERT_EQ_INT(nph_pack_writer_begin(&w, f->alloc, f->pack_path, 0U), NPH_RESULT_OK);
    const char *names[] = {"stored.nph", "packed.nph"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(names); ++i) {
        char path[1400];
        NTEST_ASSERT_EQ_INT(nph_path_join(path, sizeof path, nph_str_from_cstr(f->loose),
                                          nph_str_from_cstr(names[i]), NULL),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_pack_writer_add_file(&w, nph_str_from_cstr(names[i]),
                                                     nph_guid_from_cstr(names[i]),
                                                     NPH_ASSET_TYPE_BLOB, path),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_pack_writer_finish(&w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, NPH_STR("pak"), f->pack_path, NULL),
                        NPH_RESULT_OK);
    // Synchronous: the GUID names the path, and a later load by path
    // shares the same record.
    NphAssetHandle by_guid;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid(f->loader, nph_guid_from_cstr("stored.nph"), &by_guid),
                        NPH_RESULT_OK);
    const NphAsset *a = nph_asset_get(f->loader, by_guid);
    NTEST_ASSERT_NOT_NULL(a);
    NTEST_ASSERT_TRUE(nph_str_equals(a->path, NPH_STR("pak/stored.nph")));
    check_blobs(a, 3U, 1000U, NPH_ASSET_CODEC_NONE);
    NphAssetHandle by_path;
    NTEST_ASSERT_EQ_INT(nph_asset_load(f->loader, NPH_STR("pak/stored.nph"), &by_path),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(by_path.value, by_guid.value);
    NTEST_ASSERT_EQ_UINT(nph_asset_find_guid(f->loader, nph_guid_from_cstr("stored.nph")).value,
                         by_guid.value);
    // Asynchronous.
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NphAssetHandle async;
    NTEST_ASSERT_EQ_INT(
        nph_asset_load_guid_async(f->loader, nph_guid_from_cstr("packed.nph"), &done, &async),
        NPH_RESULT_OK);
    nph_job_wait(f->jobs, &done);
    NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, async), NPH_RESULT_OK);
    check_blobs(nph_asset_get(f->loader, async), 3U, NPH_KIB(40), NPH_ASSET_CODEC_LZ);
    // An identity no mounted pack knows.
    NphAssetHandle none;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid(f->loader, nph_guid_from_cstr("missing"), &none),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_TRUE(nph_asset_handle_is_null(none));
    NTEST_ASSERT_EQ_INT(
        nph_asset_load_guid_async(f->loader, nph_guid_from_cstr("missing"), &done, &none),
        NPH_RESULT_NOT_FOUND);
    nph_asset_release(f->loader, by_guid);
    nph_asset_release(f->loader, by_path);
    nph_asset_release(f->loader, async);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}

NTEST_F(LoaderFixture, loads_by_guid_through_the_resolver)
{
    // Loose files have no identity index: the resolver names them.
    static const char *const names[] = {"textures/stored", "textures/packed"};
    static const char *const paths[] = {"game/stored.nph", "game/packed.nph"};
    TestResolver resolver = {names, paths, 2U, 0U};
    g_resolver = &resolver;
    make_loader(f, true, NPH_ASSET_VERIFY_ALWAYS, 0U);
    g_resolver = NULL;
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_directory(f->vfs, NPH_STR("game"), f->loose, NULL),
                        NPH_RESULT_OK);
    NphAssetHandle a;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid(f->loader, nph_guid_from_cstr("textures/stored"), &a),
                        NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(
        nph_str_equals(nph_asset_get(f->loader, a)->path, NPH_STR("game/stored.nph")));
    check_blobs(nph_asset_get(f->loader, a), 3U, 1000U, NPH_ASSET_CODEC_NONE);
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NphAssetHandle b;
    NTEST_ASSERT_EQ_INT(
        nph_asset_load_guid_async(f->loader, nph_guid_from_cstr("textures/packed"), &done, &b),
        NPH_RESULT_OK);
    nph_job_wait(f->jobs, &done);
    NTEST_ASSERT_EQ_INT(nph_asset_wait(f->loader, b), NPH_RESULT_OK);
    // Unknown to both the packs and the resolver.
    NphAssetHandle none;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid(f->loader, nph_guid_from_cstr("nowhere"), &none),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_UINT(resolver.calls, 3U);
    nph_asset_release(f->loader, a);
    nph_asset_release(f->loader, b);
    NTEST_ASSERT_EQ_UINT(nph_asset_loader_live(f->loader), 0U);
}
