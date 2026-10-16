// SPDX-License-Identifier: Apache-2.0
#pragma once

// Synchronous file I/O, memory-mapped files, and the file system
// (CLAUDE.md §8.3 "File I/O: sync and async ... memory-mapped files").
// Asynchronous reads and writes are in async_io.h.
//
// Paths are UTF-8 with either separator; the Windows implementation
// converts to UTF-16. Offsets and sizes are 64-bit everywhere. An NphFile
// is a small value holding the OS handle; it is opened and closed in
// place, never allocated.
//
// Reads can be short: a sequential read returns fewer bytes than asked
// only at the end of the file (0 at the end); a positional read past the
// end likewise. Writes are complete on success: the implementation loops
// until every byte is written or an error occurs.
//
// Mixing sequential (nph_file_read/write, position-based) and positional
// (*_at) calls on one handle is allowed but the file position after a
// positional call is unspecified; streaming code uses one style.
//
// Errors map to NphResult: NOT_FOUND, ACCESS_DENIED, ALREADY_EXISTS, BUSY
// (sharing violation, non-empty directory), FULL (disk full), IO_ERROR
// for the rest; the OS message is logged on the platform category.
//
// Thread-safety: one NphFile may be used by one thread at a time, except
// that positional reads and writes (*_at) and async submissions are
// NPH_THREADSAFE on the same handle. The file system functions are
// NPH_THREADSAFE.

#include <nepheline/core/alloc.h>
#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

typedef enum NphFileMode {
    NPH_FILE_READ = 1U << 0U,
    NPH_FILE_WRITE = 1U << 1U,
    NPH_FILE_CREATE = 1U << 2U,     // create when missing (with WRITE)
    NPH_FILE_TRUNCATE = 1U << 3U,   // empty an existing file (with WRITE)
    NPH_FILE_EXCLUSIVE = 1U << 4U,  // with CREATE: ALREADY_EXISTS when present
    NPH_FILE_APPEND = 1U << 5U,     // every write goes to the end
    NPH_FILE_ASYNC = 1U << 6U,      // usable with async_io.h (overlapped on Windows)
    NPH_FILE_SEQUENTIAL = 1U << 7U, // access-pattern hint for the OS cache
    NPH_FILE_RANDOM = 1U << 8U,     // access-pattern hint for the OS cache
} NphFileMode;

typedef struct NphFile {
    u64 native;   // HANDLE or file descriptor
    u64 position; // sequential position, kept here for overlapped handles (Windows)
    u32 mode;     // NphFileMode bits it was opened with
    u32 state;    // implementation flags (async registration)
    b32 open;
} NphFile;

typedef enum NphFileSeek {
    NPH_FILE_SEEK_SET,     // from the start
    NPH_FILE_SEEK_CURRENT, // from the current position
    NPH_FILE_SEEK_END,     // from the end
} NphFileSeek;

// Opens `path` with `mode` (an or of NphFileMode; READ and/or WRITE
// required). Common combinations: READ; WRITE | CREATE | TRUNCATE (make a
// new file); READ | WRITE (edit in place). Files are shared for reading
// with other processes and never inherited by children.
NPH_MUST_USE NphResult nph_file_open(const char *path, u32 mode, NphFile *out);

// Closes the handle. Safe on a closed or zero-filled file.
void nph_file_close(NphFile *f);

NPH_INLINE bool nph_file_is_open(const NphFile *f)
{
    return f->open != 0;
}

// Reads up to `size` bytes at the current position into `buf`. *out_read is
// the count actually read: less than `size` only at the end of the file.
NPH_MUST_USE NphResult nph_file_read(NphFile *f, void *buf, u64 size, u64 *out_read);

// Writes all `size` bytes at the current position (the end, with APPEND).
// *out_written (may be NULL) is the count written; it equals `size` on
// success and is the count that made it on failure.
NPH_MUST_USE NphResult nph_file_write(NphFile *f, const void *buf, u64 size, u64 *out_written);

// Positional read and write at `offset`, independent of the file position
// (pread/pwrite; overlapped I/O with an offset on Windows).
NPH_MUST_USE NphResult nph_file_read_at(NphFile *f, u64 offset, void *buf, u64 size, u64 *out_read);
NPH_MUST_USE NphResult nph_file_write_at(NphFile *f, u64 offset, const void *buf, u64 size,
                                         u64 *out_written);

// Moves the file position; *out_position (may be NULL) receives the new
// absolute position. A result before the start is NPH_RESULT_INVALID_ARGUMENT.
NphResult nph_file_seek(NphFile *f, i64 offset, NphFileSeek origin, u64 *out_position);
NphResult nph_file_tell(NphFile *f, u64 *out_position);

NphResult nph_file_size(NphFile *f, u64 *out_size);

// Sets the file's length (extending with zeros or cutting). Needs WRITE.
// The file position afterwards is unspecified; seek before the next
// sequential read or write.
NphResult nph_file_truncate(NphFile *f, u64 size);

// Forces written data to the device (FlushFileBuffers / fsync).
NphResult nph_file_flush(NphFile *f);

// Reads a whole file into memory from `alloc` (size + 1 bytes, with a NUL
// after the data so text can be used as a C string). Free with
// nph_file_free_all. An empty file gives a 1-byte allocation.
NPH_MUST_USE NphResult nph_file_read_all(const char *path, NphAllocator *alloc, void **out_data,
                                         u64 *out_size);
void nph_file_free_all(NphAllocator *alloc, void *data, u64 size);

// Creates or truncates `path` and writes `size` bytes.
NPH_MUST_USE NphResult nph_file_write_all(const char *path, const void *data, u64 size);

// ---------------------------------------------------------------------------
// Memory-mapped files
// ---------------------------------------------------------------------------

typedef enum NphFileMapAccess {
    NPH_FILE_MAP_READ,       // read-only view; the file needs READ
    NPH_FILE_MAP_READ_WRITE, // writes go to the file; needs READ | WRITE
    NPH_FILE_MAP_COPY,       // private copy-on-write view; needs READ
} NphFileMapAccess;

typedef struct NphFileMapping {
    void *data; // the requested range
    u64 size;
    u8 *view; // the aligned OS view containing `data`
    u64 view_size;
    void *section; // file-mapping object (Windows)
} NphFileMapping;

// Maps [offset, offset + size) of the file (size 0: to the end). The view
// is page aligned internally; `offset` may be anything. Mapping an empty
// range is NPH_RESULT_INVALID_ARGUMENT.
NPH_MUST_USE NphResult nph_file_map(NphFile *f, u64 offset, u64 size, NphFileMapAccess access,
                                    NphFileMapping *out);

// Releases the view. Safe on a zero-filled mapping.
void nph_file_unmap(NphFileMapping *m);

// Writes a READ_WRITE view's dirty pages to the file.
NphResult nph_file_map_flush(NphFileMapping *m);

// ---------------------------------------------------------------------------
// File system
// ---------------------------------------------------------------------------

typedef struct NphFileInfo {
    u64 size;        // bytes (0 for directories)
    u64 modified_ns; // last write, UTC nanoseconds since the Unix epoch
    b32 is_directory;
    b32 is_regular; // a regular file (not a device, pipe, or link to a directory)
} NphFileInfo;

NphResult nph_fs_stat(const char *path, NphFileInfo *out);
bool nph_fs_exists(const char *path);
NphResult nph_fs_remove_file(const char *path);

// Moves `from` to `to`, replacing an existing file atomically where the
// OS allows (same volume). A destination open or mapped elsewhere is
// replaced too, as POSIX renames do: those handles and views keep the old
// file, and opens by the name find the new one. On Windows this takes
// POSIX semantics (Windows 10 1809 and later, NTFS); elsewhere on Windows
// a destination that is mapped, or open without delete sharing, is
// refused (NPH_RESULT_ACCESS_DENIED).
NphResult nph_fs_rename(const char *from, const char *to);

// Copies the regular file `from` to `to`, replacing `to` when it exists.
// The bytes go to a temporary file beside `to` (`to` + ".part"), which is
// then renamed over it (nph_fs_rename: a reader of the old `to` keeps it),
// so `to` is never seen half written; the copy is writable whatever the
// source's attributes. The OS copies the bytes
// (CopyFileExW; copy_file_range on Linux, which may share extents on
// file systems that can). NPH_RESULT_NOT_FOUND for a missing source or
// destination directory, NPH_RESULT_INVALID_ARGUMENT when `from` is a
// directory, NPH_RESULT_OVERFLOW for a path too long, and the OS's errors;
// on failure `to` is as it was and no temporary file is left.
NphResult nph_fs_copy_file(const char *from, const char *to);

// Creates one directory; OK when it already exists as a directory,
// NOT_FOUND when the parent does not exist.
NphResult nph_fs_create_directory(const char *path);

// Creates every missing directory on the path.
NphResult nph_fs_create_directories(const char *path);

// Removes an empty directory; BUSY when it is not empty.
NphResult nph_fs_remove_directory(const char *path);

// Removes a directory and everything under it.
NphResult nph_fs_remove_tree(const char *path);

NphResult nph_fs_current_directory(char *buf, u64 cap, u64 *out_len);
NphResult nph_fs_temp_directory(char *buf, u64 cap, u64 *out_len);

#define NPH_DIR_ENTRY_NAME_MAX 256U

typedef struct NphDirEntry {
    char name[NPH_DIR_ENTRY_NAME_MAX]; // UTF-8, truncated if longer
    b32 is_directory;
} NphDirEntry;

typedef struct NphDirIterator {
    void *handle; // search handle or DIR *
    NphDirEntry pending;
    b32 has_pending;
    b32 open;
} NphDirIterator;

// Starts listing `path`. NOT_FOUND when it does not exist, INVALID_ARGUMENT
// when it is not a directory.
NPH_MUST_USE NphResult nph_dir_open(const char *path, NphDirIterator *it);

// Fills *out with the next entry, skipping "." and "..". false at the end.
// Order is whatever the OS returns.
bool nph_dir_next(NphDirIterator *it, NphDirEntry *out);

void nph_dir_close(NphDirIterator *it);

#ifdef __cplusplus
}
#endif
