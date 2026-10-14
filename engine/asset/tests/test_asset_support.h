// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the asset tests: a temporary directory per test,
// deterministic test data, and builders for small cooked files.

#include <nepheline/asset/format.h>
#include <nepheline/core/assert.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>

#include <ntest/ntest.h>

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Expected assertions (as in the core tests)
// ---------------------------------------------------------------------------

typedef struct TestAssertRecorder {
    u32 count;
} TestAssertRecorder;

static inline NphAssertAction test_assert_record(const NphAssertInfo *info, void *user)
{
    NPH_UNUSED(info);
    ((TestAssertRecorder *)user)->count += 1U;
    return NPH_ASSERT_ACTION_CONTINUE;
}

#if NPH_ASSERTS_ENABLED
#define NPH_TEST_EXPECT_ASSERT(stmt)                                                               \
    do {                                                                                           \
        TestAssertRecorder rec_ = {0U};                                                            \
        NphAssertHandler prev_ = NULL;                                                             \
        void *prev_user_ = NULL;                                                                   \
        nph_assert_set_handler(test_assert_record, &rec_, &prev_, &prev_user_);                    \
        stmt;                                                                                      \
        nph_assert_set_handler(prev_, prev_user_, NULL, NULL);                                     \
        NTEST_ASSERT_GE_UINT(rec_.count, 1U);                                                      \
    } while (0)
#else
#define NPH_TEST_EXPECT_ASSERT(stmt)                                                               \
    do {                                                                                           \
        stmt;                                                                                      \
    } while (0)
#endif

// ---------------------------------------------------------------------------
// Temporary directories
// ---------------------------------------------------------------------------

typedef struct TempDir {
    char dir[1024];
} TempDir;

static inline void test_temp_dir_make(TempDir *t, const char *tag)
{
    static u32 counter = 0U;
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    snprintf(t->dir, sizeof t->dir, "%s/nph_asset_%s_%llu_%u", tmp, tag,
             (unsigned long long)nph_platform_process_id(), counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(t->dir), NPH_RESULT_OK);
}

static inline void test_temp_dir_remove(TempDir *t)
{
    NTEST_ASSERT_EQ_INT(nph_fs_remove_tree(t->dir), NPH_RESULT_OK);
}

// "<dir>/<name>" into buf.
static inline const char *test_path_in(const TempDir *t, const char *name, char *buf, u64 cap)
{
    NTEST_ASSERT_EQ_INT(
        nph_path_join(buf, cap, nph_str_from_cstr(t->dir), nph_str_from_cstr(name), NULL),
        NPH_RESULT_OK);
    return buf;
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

// Deterministic PRNG (splitmix64).
static inline u64 test_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Fills `n` bytes with a pattern derived from `seed`, and checks it.
static inline void test_fill_pattern(void *p, u64 n, u64 seed)
{
    u8 *b = (u8 *)p;
    for (u64 i = 0; i < n; ++i) {
        b[i] = (u8)((seed * 31U + i) & 0xFFU);
    }
}

static inline bool test_check_pattern(const void *p, u64 n, u64 seed)
{
    const u8 *b = (const u8 *)p;
    for (u64 i = 0; i < n; ++i) {
        if (b[i] != (u8)((seed * 31U + i) & 0xFFU)) {
            return false;
        }
    }
    return true;
}

// Text-like data: words from a small vocabulary, so it compresses well
// but not trivially.
static inline void test_fill_text(u8 *p, u64 n, u64 seed)
{
    static const char *words[] = {"nepheline ", "mesh ",   "texture ", "vertex ", "index ",
                                  "the ",       "buffer ", "frame ",   "pixel ",  "cache "};
    u64 pos = 0U;
    while (pos < n) {
        const char *w = words[test_rng_next(&seed) % NPH_ARRAY_COUNT(words)];
        const u64 len = strlen(w);
        for (u64 i = 0; i < len && pos < n; ++i) {
            p[pos++] = (u8)w[i];
        }
    }
}

// ---------------------------------------------------------------------------
// Cooked files
// ---------------------------------------------------------------------------

// Builds a BLOB asset with `blob_count` blobs: tag 'DATA', index i, size
// `blob_size`, pattern seed i, alternating stored and compressed.
static inline void test_make_blob_asset(NphAllocator *alloc, NphGuid guid, u32 blob_count,
                                        u64 blob_size, NphAssetCodec codec, void **out_data,
                                        u64 *out_size)
{
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_BLOB, 1U, guid, 0xC0FFEEU);
    u8 *buf = (u8 *)NPH_ALLOC(alloc, nph_max_u64(blob_size, 1U), 16U);
    NTEST_ASSERT_NOT_NULL(buf);
    for (u32 i = 0; i < blob_count; ++i) {
        test_fill_pattern(buf, blob_size, i);
        NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_FOURCC('D', 'A', 'T', 'A'), i, buf,
                                                      blob_size, codec, 0U),
                            NPH_RESULT_OK);
    }
    NPH_FREE(alloc, buf, nph_max_u64(blob_size, 1U), 16U);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_finish_alloc(&w, alloc, out_data, out_size),
                        NPH_RESULT_OK);
    nph_asset_writer_free(&w);
}

static inline void test_free_asset_image(NphAllocator *alloc, void *data, u64 size)
{
    NPH_FREE(alloc, data, size, NPH_ASSET_BLOB_MAX_ALIGN);
}
