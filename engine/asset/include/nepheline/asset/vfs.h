// SPDX-License-Identifier: Apache-2.0
#pragma once

// The virtual file system (CLAUDE.md §8.8 "loose files in development,
// pack archives in shipping").
//
// A VFS is an ordered list of mounts. Each mount binds a prefix ("game/",
// "" for the root) to a directory on disk or to a pack (pack.h). A lookup
// walks the mounts from the most recently mounted to the first and takes
// the first that has the path, so a loose directory mounted after a pack
// overrides files in it, which is how patches and editor edits shadow
// shipped content.
//
// Paths are UTF-8 with '/' separators, relative to the root, without a
// leading slash, and normalised: no empty, "." or ".." components, no
// backslashes. nph_vfs_normalize_path turns user input into that form and
// refuses anything that would escape the root. Matching is exact bytes;
// packs are case-sensitive and the cooker writes paths as authored, so
// code that loads "Textures/A.nph" from a pack built from "textures/a.nph"
// fails on every platform rather than only on Linux.
//
// Reads come in three shapes: a copy (nph_vfs_read_all), a mapping that is
// used in place (nph_vfs_map: a memory-mapped loose file, or a pointer
// into the pack's mapping), and a file handle plus offset for positional
// and asynchronous reads (nph_vfs_open). The loader (loader.h) uses the
// mapping; streaming uses the handle.
//
// Thread-safety: mounting and unmounting are NPH_MAIN_THREAD_ONLY and
// must not overlap any lookup; every lookup and read is NPH_THREADSAFE.

#include <nepheline/asset/asset.h>
#include <nepheline/core/alloc.h>
#include <nepheline/core/string.h>
#include <nepheline/platform/file.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphVfs NphVfs;

#define NPH_VFS_MAX_MOUNTS 64U
#define NPH_VFS_MAX_PATH   1024U
#define NPH_VFS_NO_MOUNT   NPH_U32_MAX

typedef enum NphVfsMountType {
    NPH_VFS_MOUNT_DIRECTORY = 0,
    NPH_VFS_MOUNT_PACK,
} NphVfsMountType;

typedef struct NphVfsMountInfo {
    NphVfsMountType type;
    NphStr prefix;  // "", or ending in '/'
    NphStr backing; // the directory or the pack file
    u32 id;
} NphVfsMountInfo;

typedef struct NphVfsInfo {
    u64 size;
    u32 mount_id;
    NphVfsMountType source;
    b32 is_directory; // directories only exist in directory mounts
} NphVfsInfo;

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// True when `path` is in the canonical form described above.
bool nph_vfs_path_is_normalized(NphStr path);

// Writes the canonical form of `path` into `buf` (NUL terminated, at most
// `cap` bytes): backslashes become '/', repeated and leading separators
// collapse, "." components drop out, ".." removes the previous component.
// NPH_RESULT_INVALID_ARGUMENT when ".." would climb above the root or the
// result is empty; NPH_RESULT_OVERFLOW when it does not fit. *out (may be
// NULL) receives a view of the result in `buf`.
NphResult nph_vfs_normalize_path(NphStr path, char *buf, u64 cap, NphStr *out);

// ---------------------------------------------------------------------------
// Lifecycle and mounts
// ---------------------------------------------------------------------------

NPH_MUST_USE NphResult nph_vfs_create(NphAllocator *alloc, NphVfs **out);

// Unmounts everything (closing packs) and releases the VFS. Safe on NULL.
void nph_vfs_destroy(NphVfs *vfs);

// Mounts the directory `os_dir` at `prefix` (normalised; "" for the root).
// The directory need not exist yet. *out_id (may be NULL) receives the
// mount id for nph_vfs_unmount. NPH_RESULT_FULL past NPH_VFS_MAX_MOUNTS.
NPH_MUST_USE NphResult nph_vfs_mount_directory(NphVfs *vfs, NphStr prefix, const char *os_dir,
                                               u32 *out_id);

// Opens the pack at `pack_path` and mounts it at `prefix`. The VFS owns
// the pack. Errors from nph_pack_open.
NPH_MUST_USE NphResult nph_vfs_mount_pack(NphVfs *vfs, NphStr prefix, const char *pack_path,
                                          u32 *out_id);

// Removes a mount (closing its pack). NPH_RESULT_NOT_FOUND for an unknown
// id. Mappings and handles obtained through the mount must have been
// released.
NphResult nph_vfs_unmount(NphVfs *vfs, u32 id);

u32 nph_vfs_mount_count(const NphVfs *vfs);

// Mount `index` in mount order (0 is the oldest).
NphVfsMountInfo nph_vfs_mount_info(const NphVfs *vfs, u32 index);

// ---------------------------------------------------------------------------
// Lookup and reading
// ---------------------------------------------------------------------------

// Resolves `path` (normalised). NPH_RESULT_NOT_FOUND when no mount has it.
NphResult nph_vfs_stat(const NphVfs *vfs, NphStr path, NphVfsInfo *out);
bool nph_vfs_exists(const NphVfs *vfs, NphStr path);

// Reads the whole file into memory from `alloc`: size + 1 bytes with a NUL
// after the data (as nph_file_read_all). Free with nph_vfs_free_all.
NPH_MUST_USE NphResult nph_vfs_read_all(const NphVfs *vfs, NphStr path, NphAllocator *alloc,
                                        void **out_data, u64 *out_size);
void nph_vfs_free_all(NphAllocator *alloc, void *data, u64 size);

// A file's bytes, usable in place. For a loose file the VFS opens and maps
// it (the handle and the view live in the structure); for a pack entry
// `data` points into the pack's mapping and nothing is opened.
typedef struct NphVfsMapping {
    const void *data;
    u64 size;
    u32 mount_id;
    NphVfsMountType source;
    NphFile file;           // loose files only
    NphFileMapping mapping; // loose files only
} NphVfsMapping;

NPH_MUST_USE NphResult nph_vfs_map(const NphVfs *vfs, NphStr path, NphVfsMapping *out);
void nph_vfs_unmap(const NphVfs *vfs, NphVfsMapping *m);

// A handle for positional and asynchronous reads (platform/file.h,
// platform/async_io.h): `file` is the file to read, and the entry occupies
// [base_offset, base_offset + size) of it. For a loose file the VFS opens
// the file (with NPH_FILE_ASYNC and `extra_mode`) and owns the handle; for
// a pack entry `file` is the pack's own handle, shared by every entry.
typedef struct NphVfsFile {
    NphFile *file;
    u64 base_offset;
    u64 size;
    u32 mount_id;
    NphVfsMountType source;
    NphFile owned; // loose files only
} NphVfsFile;

NPH_MUST_USE NphResult nph_vfs_open(const NphVfs *vfs, NphStr path, u32 extra_mode,
                                    NphVfsFile *out);
void nph_vfs_close(const NphVfs *vfs, NphVfsFile *f);

// Reads [offset, offset + size) of the entry into `buf` (bounded to the
// entry; *out_read is the count, short at the end).
NPH_MUST_USE NphResult nph_vfs_read_at(NphVfsFile *f, u64 offset, void *buf, u64 size,
                                       u64 *out_read);

// Writes the OS path of `path` in the directory mount that would serve it
// (the newest directory mount whose prefix matches, whether or not the
// file exists yet), for tools that write loose files. NPH_RESULT_NOT_FOUND
// when no directory mount covers the path; NPH_RESULT_OVERFLOW when it
// does not fit.
NphResult nph_vfs_resolve_directory(const NphVfs *vfs, NphStr path, char *buf, u64 cap,
                                    u64 *out_len);

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

// Called once per file (not directory) whose path starts with the
// prefix given to nph_vfs_iterate; return false to stop early. Paths are
// views valid during the call only.
typedef bool (*NphVfsVisitFn)(NphStr path, const NphVfsInfo *info, void *user);

// Visits every file under `prefix` across all mounts, each path once, with
// the info of the mount that would serve it (shadowed copies are skipped).
// Directory mounts are walked recursively. Order is unspecified. Needs a
// scratch allocation (the set of visited paths) from the VFS allocator.
NPH_MUST_USE NphResult nph_vfs_iterate(const NphVfs *vfs, NphStr prefix, NphVfsVisitFn fn,
                                       void *user);

#ifdef __cplusplus
}
#endif
