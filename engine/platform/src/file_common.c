// SPDX-License-Identifier: Apache-2.0
// The portable part of file.h: whole-file helpers and the recursive
// directory operations, built on the per-OS primitives.
#include <nepheline/core/assert.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>

#include "platform_internal.h"

#include <string.h>

#define PATH_BUFFER    4096U
#define TREE_DEPTH_MAX 64U

NphResult nph_file_read_all(const char *path, NphAllocator *alloc, void **out_data, u64 *out_size)
{
    NPH_ASSERT(path != NULL && alloc != NULL && out_data != NULL && out_size != NULL);
    *out_data = NULL;
    *out_size = 0U;
    NphFile f;
    NPH_TRY(nph_file_open(path, NPH_FILE_READ | NPH_FILE_SEQUENTIAL, &f));
    u64 size = 0U;
    NphResult r = nph_file_size(&f, &size);
    if (r != NPH_RESULT_OK) {
        nph_file_close(&f);
        return r;
    }
    u8 *data = (u8 *)NPH_ALLOC(alloc, size + 1U, NPH_ALLOC_DEFAULT_ALIGN);
    if (data == NULL) {
        nph_file_close(&f);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u64 total = 0U;
    while (total < size) {
        u64 got = 0U;
        r = nph_file_read(&f, data + total, size - total, &got);
        if (r != NPH_RESULT_OK) {
            break;
        }
        if (got == 0U) {
            // The file shrank while being read; report rather than hand out
            // a block whose recorded size would not match its allocation.
            r = NPH_RESULT_IO_ERROR;
            break;
        }
        total += got;
    }
    nph_file_close(&f);
    if (r != NPH_RESULT_OK) {
        NPH_FREE(alloc, data, size + 1U, NPH_ALLOC_DEFAULT_ALIGN);
        return r;
    }
    data[size] = 0U;
    *out_data = data;
    *out_size = size;
    return NPH_RESULT_OK;
}

void nph_file_free_all(NphAllocator *alloc, void *data, u64 size)
{
    NPH_ASSERT(alloc != NULL);
    if (data != NULL) {
        NPH_FREE(alloc, data, size + 1U, NPH_ALLOC_DEFAULT_ALIGN);
    }
}

NphResult nph_file_write_all(const char *path, const void *data, u64 size)
{
    NPH_ASSERT(path != NULL && (data != NULL || size == 0U));
    NphFile f;
    NPH_TRY(nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_TRUNCATE, &f));
    NphResult r = NPH_RESULT_OK;
    if (size != 0U) {
        r = nph_file_write(&f, data, size, NULL);
    }
    nph_file_close(&f);
    return r;
}

NphResult nph_fs_create_directories(const char *path)
{
    NPH_ASSERT(path != NULL);
    char buf[PATH_BUFFER];
    const u64 len = strlen(path);
    if (len + 1U > sizeof buf) {
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(buf, path, len + 1U);
    // Skip the root so "/" or "C:\" is never created.
    u64 start = 0U;
    if (len >= 2U && buf[1] == ':') {
        start = 2U;
    }
    while (start < len && nph_path_is_separator(buf[start])) {
        ++start;
    }
    for (u64 i = start; i < len; ++i) {
        if (!nph_path_is_separator(buf[i])) {
            continue;
        }
        if (i > start) {
            buf[i] = '\0';
            NPH_TRY(nph_fs_create_directory(buf));
            buf[i] = path[i];
        }
    }
    if (len > start && !nph_path_is_separator(buf[len - 1U])) {
        return nph_fs_create_directory(buf);
    }
    return NPH_RESULT_OK;
}

static NphResult remove_tree_recursive(const char *path, u32 depth)
{
    if (depth >= TREE_DEPTH_MAX) {
        return NPH_RESULT_OVERFLOW;
    }
    NphDirIterator it;
    NPH_TRY(nph_dir_open(path, &it));
    NphResult r = NPH_RESULT_OK;
    NphDirEntry entry;
    while (r == NPH_RESULT_OK && nph_dir_next(&it, &entry)) {
        char child[PATH_BUFFER];
        r = nph_path_join(child, sizeof child, nph_str_from_cstr(path),
                          nph_str_from_cstr(entry.name), NULL);
        if (r != NPH_RESULT_OK) {
            break;
        }
        r = entry.is_directory ? remove_tree_recursive(child, depth + 1U)
                               : nph_fs_remove_file(child);
    }
    nph_dir_close(&it);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    return nph_fs_remove_directory(path);
}

NphResult nph_fs_remove_tree(const char *path)
{
    NPH_ASSERT(path != NULL);
    return remove_tree_recursive(path, 0U);
}
