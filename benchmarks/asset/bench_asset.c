// SPDX-License-Identifier: Apache-2.0
// Asset module benchmarks (docs/asset.md).
//
//   lz_compress_fast_text      compress 256 KiB of text at the FAST level (ns per KiB)
//   lz_compress_high_text      the HIGH level on the same input
//   lz_decompress_text         decode the HIGH stream (ns per KiB of output)
//   lz_compress_fast_vertices  structured binary data (a vertex-buffer-like stream)
//   lz_compress_high_vertices
//   lz_decompress_vertices
//   lz_decompress_zeros        the best case: one long match
//   crc32c_blob_256k           the checksum pass of a 256 KiB blob (ns per KiB)
//   pack_find_path_4k          look up a path in a 4096-entry pack
//   pack_find_guid_4k          look up a GUID in the same pack
//   vfs_stat_pack              resolve a path in a VFS with one pack mount
//   vfs_stat_shadowed_pack     the same with a directory mounted over the pack: the
//                              directory is consulted first (one failed stat)
//   vfs_normalize_path         normalise a 40-byte path
//   asset_load_cached          nph_asset_load of an asset already loaded (a lookup + a reference)
//   asset_load_small           load and release a small stored asset from a pack
//
// The compression numbers are reported per KiB so they read as throughput:
// 1 ns/KiB is about 1 TB/s, 1000 ns/KiB about 1 GB/s.

#include <nepheline/asset/loader.h>
#include <nepheline/asset/lz.h>
#include <nepheline/asset/pack.h>
#include <nepheline/core/hash.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>

#include <nbench/nbench.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_SIZE    NPH_KIB(256)
#define PACK_ENTRIES 4096U

static u8 g_text[TEXT_SIZE];
static u8 g_vertices[TEXT_SIZE];
static u8 g_zeros[TEXT_SIZE];
static u8 g_packed[TEXT_SIZE + TEXT_SIZE / 255U + 16U];
static u8 g_out[TEXT_SIZE];
static bool g_data_ready;

static void prepare_data(void)
{
    if (g_data_ready) {
        return;
    }
    static const char *words[] = {"nepheline ", "mesh ",   "texture ", "vertex ", "index ",
                                  "the ",       "buffer ", "frame ",   "pixel ",  "cache "};
    u64 s = 7U;
    u64 pos = 0U;
    while (pos < TEXT_SIZE) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const char *w = words[(s >> 59U) % NPH_ARRAY_COUNT(words)];
        for (const char *c = w; *c != '\0' && pos < TEXT_SIZE; ++c) {
            g_text[pos++] = (u8)*c;
        }
    }
    for (u64 i = 0; i < TEXT_SIZE / 2U; ++i) {
        const u16 v = (u16)(1000U + (i % 8U) * 37U + (i / 64U));
        g_vertices[i * 2U] = (u8)(v & 0xFFU);
        g_vertices[i * 2U + 1U] = (u8)(v >> 8U);
    }
    memset(g_zeros, 0, sizeof g_zeros);
    g_data_ready = true;
}

static void *workspace(NphLzLevel level)
{
    static void *fast;
    static void *high;
    void **slot = level == NPH_LZ_LEVEL_HIGH ? &high : &fast;
    if (*slot == NULL) {
        *slot = NPH_ALLOC(nph_allocator_system(), nph_lz_workspace_size(level), 16U);
    }
    return *slot;
}

static u64 compress_into(const u8 *src, NphLzLevel level)
{
    u64 size = 0U;
    if (nph_lz_compress(src, TEXT_SIZE, g_packed, sizeof g_packed, level, workspace(level),
                        &size) != NPH_RESULT_OK) {
        abort();
    }
    return size;
}

static void bench_compress(NbenchState *state, const u8 *src, NphLzLevel level, char *label,
                           u64 label_cap)
{
    prepare_data();
    nbench_set_items_per_iteration(state, TEXT_SIZE / 1024U);
    u64 size = 0U;
    while (nbench_keep_running(state)) {
        size = compress_into(src, level);
    }
    snprintf(label, (size_t)label_cap, "%.1f%% of input", 100.0 * (f64)size / (f64)TEXT_SIZE);
    nbench_set_label(state, label);
    nbench_do_not_optimize_u64(size);
}

static void bench_decompress(NbenchState *state, const u8 *src)
{
    prepare_data();
    const u64 size = compress_into(src, NPH_LZ_LEVEL_HIGH);
    nbench_set_items_per_iteration(state, TEXT_SIZE / 1024U);
    while (nbench_keep_running(state)) {
        if (nph_lz_decompress(g_packed, size, g_out, TEXT_SIZE) != NPH_RESULT_OK) {
            abort();
        }
    }
    nbench_do_not_optimize(g_out);
}

// Labels are static per benchmark: nbench keeps the pointer until the
// report is written.
NBENCH(lz_compress_fast_text)
{
    static char label[64];
    bench_compress(state, g_text, NPH_LZ_LEVEL_FAST, label, sizeof label);
}
NBENCH(lz_compress_high_text)
{
    static char label[64];
    bench_compress(state, g_text, NPH_LZ_LEVEL_HIGH, label, sizeof label);
}
NBENCH(lz_decompress_text)
{
    bench_decompress(state, g_text);
}
NBENCH(lz_compress_fast_vertices)
{
    static char label[64];
    bench_compress(state, g_vertices, NPH_LZ_LEVEL_FAST, label, sizeof label);
}
NBENCH(lz_compress_high_vertices)
{
    static char label[64];
    bench_compress(state, g_vertices, NPH_LZ_LEVEL_HIGH, label, sizeof label);
}
NBENCH(lz_decompress_vertices)
{
    bench_decompress(state, g_vertices);
}
NBENCH(lz_decompress_zeros)
{
    bench_decompress(state, g_zeros);
}

NBENCH(crc32c_blob_256k)
{
    prepare_data();
    nbench_set_items_per_iteration(state, TEXT_SIZE / 1024U);
    u32 acc = 0U;
    while (nbench_keep_running(state)) {
        acc = nph_crc32c(g_text, TEXT_SIZE, acc);
    }
    nbench_do_not_optimize_u64(acc);
}

// ---------------------------------------------------------------------------
// Packs, the VFS, and the loader share one pack written to the temp
// directory on first use and removed at exit.
// ---------------------------------------------------------------------------

typedef struct Fixture {
    NphAllocator *alloc;
    char dir[1024];
    char pack_path[1100];
    NphPack pack;
    NphVfs *vfs;      // the pack under a directory mount
    NphVfs *vfs_pack; // the pack alone
    NphAssetLoader *loader;
    bool ready;
} Fixture;

static Fixture g_fx;

static void fixture_cleanup(void)
{
    nph_asset_loader_destroy(g_fx.loader);
    nph_vfs_destroy(g_fx.vfs_pack);
    nph_vfs_destroy(g_fx.vfs);
    nph_pack_close(&g_fx.pack);
    if (g_fx.dir[0] != '\0') {
        (void)nph_fs_remove_tree(g_fx.dir);
    }
}

static void fixture_prepare(void)
{
    if (g_fx.ready) {
        return;
    }
    g_fx.alloc = nph_allocator_system();
    // A failed stat on the directory mount is reported at DEBUG; before
    // nph_log_init that would go straight to stderr for every iteration.
    nph_log_set_global_level(NPH_LOG_LEVEL_INFO);
    char tmp[800];
    if (nph_fs_temp_directory(tmp, sizeof tmp, NULL) != NPH_RESULT_OK) {
        abort();
    }
    snprintf(g_fx.dir, sizeof g_fx.dir, "%s/nph_asset_bench_%llu", tmp,
             (unsigned long long)nph_platform_process_id());
    if (nph_fs_create_directories(g_fx.dir) != NPH_RESULT_OK) {
        abort();
    }
    atexit(fixture_cleanup);
    snprintf(g_fx.pack_path, sizeof g_fx.pack_path, "%s/bench.npk", g_fx.dir);

    // One small stored asset, repeated under 4096 paths.
    NphAssetWriter aw;
    nph_asset_writer_init(&aw, g_fx.alloc, NPH_ASSET_TYPE_BLOB, 1U, nph_guid_from_cstr("x"), 0U);
    u8 payload[512];
    memset(payload, 0x5A, sizeof payload);
    if (nph_asset_writer_add_blob(&aw, NPH_FOURCC('D', 'A', 'T', 'A'), 0U, payload, sizeof payload,
                                  NPH_ASSET_CODEC_NONE, 0U) != NPH_RESULT_OK) {
        abort();
    }
    NphPackWriter pw;
    if (nph_pack_writer_begin(&pw, g_fx.alloc, g_fx.pack_path, 256U) != NPH_RESULT_OK) {
        abort();
    }
    for (u32 i = 0; i < PACK_ENTRIES; ++i) {
        char name[64];
        snprintf(name, sizeof name, "assets/group%u/item%u.nph", i % 16U, i);
        aw.header.guid = nph_guid_from_cstr(name);
        void *image = NULL;
        u64 size = 0U;
        if (nph_asset_writer_finish_alloc(&aw, g_fx.alloc, &image, &size) != NPH_RESULT_OK) {
            abort();
        }
        if (nph_pack_writer_add(&pw, nph_str_from_cstr(name), aw.header.guid, NPH_ASSET_TYPE_BLOB,
                                image, size) != NPH_RESULT_OK) {
            abort();
        }
        NPH_FREE(g_fx.alloc, image, size, NPH_ASSET_BLOB_MAX_ALIGN);
    }
    nph_asset_writer_free(&aw);
    if (nph_pack_writer_finish(&pw) != NPH_RESULT_OK) {
        abort();
    }
    if (nph_pack_open(g_fx.alloc, g_fx.pack_path, &g_fx.pack) != NPH_RESULT_OK) {
        abort();
    }
    if (nph_vfs_create(g_fx.alloc, &g_fx.vfs) != NPH_RESULT_OK ||
        nph_vfs_mount_pack(g_fx.vfs, NPH_STR("game"), g_fx.pack_path, NULL) != NPH_RESULT_OK ||
        nph_vfs_mount_directory(g_fx.vfs, NPH_STR("game"), g_fx.dir, NULL) != NPH_RESULT_OK) {
        abort();
    }
    if (nph_vfs_create(g_fx.alloc, &g_fx.vfs_pack) != NPH_RESULT_OK ||
        nph_vfs_mount_pack(g_fx.vfs_pack, NPH_STR("game"), g_fx.pack_path, NULL) != NPH_RESULT_OK) {
        abort();
    }
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = g_fx.alloc;
    ld.vfs = g_fx.vfs_pack;
    ld.verify = NPH_ASSET_VERIFY_NEVER;
    if (nph_asset_loader_create(&ld, &g_fx.loader) != NPH_RESULT_OK) {
        abort();
    }
    g_fx.ready = true;
}

NBENCH(pack_find_path_4k)
{
    fixture_prepare();
    u32 i = 0U;
    const NphPackEntry *e = NULL;
    while (nbench_keep_running(state)) {
        char name[64];
        nbench_pause_timing(state);
        snprintf(name, sizeof name, "assets/group%u/item%u.nph", i % 16U, i);
        i = (i + 613U) % PACK_ENTRIES;
        nbench_resume_timing(state);
        e = nph_pack_find_path(&g_fx.pack, nph_str_from_cstr(name));
    }
    nbench_do_not_optimize(e);
}

NBENCH(pack_find_guid_4k)
{
    fixture_prepare();
    u32 i = 0U;
    const NphPackEntry *e = NULL;
    while (nbench_keep_running(state)) {
        e = nph_pack_find_guid(&g_fx.pack, nph_pack_entry_at(&g_fx.pack, i)->guid);
        i = (i + 613U) % PACK_ENTRIES;
    }
    nbench_do_not_optimize(e);
}

NBENCH(vfs_stat_pack)
{
    fixture_prepare();
    NphVfsInfo info;
    while (nbench_keep_running(state)) {
        if (nph_vfs_stat(g_fx.vfs_pack, NPH_STR("game/assets/group3/item19.nph"), &info) !=
            NPH_RESULT_OK) {
            abort();
        }
    }
    nbench_do_not_optimize(&info);
}

NBENCH(vfs_stat_shadowed_pack)
{
    fixture_prepare();
    // The directory mount is searched first (a stat that misses), then the pack.
    NphVfsInfo info;
    while (nbench_keep_running(state)) {
        if (nph_vfs_stat(g_fx.vfs, NPH_STR("game/assets/group3/item19.nph"), &info) !=
            NPH_RESULT_OK) {
            abort();
        }
    }
    nbench_do_not_optimize(&info);
}

NBENCH(vfs_normalize_path)
{
    char buf[256];
    NphStr out;
    while (nbench_keep_running(state)) {
        if (nph_vfs_normalize_path(NPH_STR("game\\assets//group3/./item19.nph"), buf, sizeof buf,
                                   &out) != NPH_RESULT_OK) {
            abort();
        }
    }
    nbench_do_not_optimize(buf);
}

NBENCH(asset_load_cached)
{
    fixture_prepare();
    NphAssetHandle keep;
    if (nph_asset_load(g_fx.loader, NPH_STR("game/assets/group5/item5.nph"), &keep) !=
        NPH_RESULT_OK) {
        abort();
    }
    while (nbench_keep_running(state)) {
        NphAssetHandle h;
        if (nph_asset_load(g_fx.loader, NPH_STR("game/assets/group5/item5.nph"), &h) !=
            NPH_RESULT_OK) {
            abort();
        }
        nph_asset_release(g_fx.loader, h);
    }
    nph_asset_release(g_fx.loader, keep);
}

NBENCH(asset_load_small)
{
    fixture_prepare();
    while (nbench_keep_running(state)) {
        NphAssetHandle h;
        if (nph_asset_load(g_fx.loader, NPH_STR("game/assets/group7/item7.nph"), &h) !=
            NPH_RESULT_OK) {
            abort();
        }
        nph_asset_release(g_fx.loader, h);
    }
}
