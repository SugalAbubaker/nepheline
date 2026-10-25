// SPDX-License-Identifier: Apache-2.0
// Files, memory mapping, and the file system on Win32.
#include <nepheline/core/assert.h>
#include <nepheline/core/realtime.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/vmem.h>

#include "platform_internal.h"

#include <stdio.h>
#include <string.h>

// Largest single ReadFile/WriteFile: a DWORD, kept to 1 GiB chunks.
#define IO_CHUNK_MAX ((u64)1U << 30U)

static HANDLE file_handle(const NphFile *f)
{
    return (HANDLE)(uptr)f->native;
}

static bool is_async(const NphFile *f)
{
    return (f->mode & NPH_FILE_ASYNC) != 0U;
}

// Expected outcomes (a probe for a missing file) are logged at DEBUG; real
// failures at ERROR.
static NphResult file_fail(const char *what, const char *path, DWORD error)
{
    const NphResult r = nph_win32_result(error);
    char message[256];
    nph_win32_error_message(error, message, (u32)sizeof message);
    if (r == NPH_RESULT_NOT_FOUND || r == NPH_RESULT_ALREADY_EXISTS ||
        r == NPH_RESULT_ACCESS_DENIED || r == NPH_RESULT_BUSY) {
        NPH_LOG_DEBUG(&NPH_LOG_PLATFORM, "%s(%s): %s (%lu)", what, path != NULL ? path : "",
                      message, (unsigned long)error);
    } else {
        NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "%s(%s): %s (%lu)", what, path != NULL ? path : "",
                      message, (unsigned long)error);
    }
    return r;
}

static NphResult to_wide(const char *path, wchar_t *out)
{
    return nph_win32_utf8_to_wide(path, out, NPH_WIN32_PATH_MAX);
}

static u64 filetime_to_unix_ns(FILETIME ft)
{
    const u64 ticks = ((u64)ft.dwHighDateTime << 32U) | ft.dwLowDateTime;
    return ticks < 116444736000000000ULL ? 0U : (ticks - 116444736000000000ULL) * 100U;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

NphResult nph_file_open(const char *path, u32 mode, NphFile *out)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_open");
    NPH_ASSERT(path != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    const bool read = (mode & NPH_FILE_READ) != 0U;
    const bool write = (mode & NPH_FILE_WRITE) != 0U;
    if (!read && !write) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!write && (mode & (NPH_FILE_CREATE | NPH_FILE_TRUNCATE | NPH_FILE_APPEND |
                           NPH_FILE_EXCLUSIVE)) != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    wchar_t wide[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(path, wide));

    DWORD access = 0U;
    if (read) {
        access |= GENERIC_READ;
    }
    if (write) {
        access |= GENERIC_WRITE;
    }
    DWORD disposition;
    if ((mode & NPH_FILE_CREATE) != 0U) {
        if ((mode & NPH_FILE_EXCLUSIVE) != 0U) {
            disposition = CREATE_NEW;
        } else if ((mode & NPH_FILE_TRUNCATE) != 0U) {
            disposition = CREATE_ALWAYS;
        } else {
            disposition = OPEN_ALWAYS;
        }
    } else {
        disposition = (mode & NPH_FILE_TRUNCATE) != 0U ? TRUNCATE_EXISTING : OPEN_EXISTING;
    }
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if ((mode & NPH_FILE_ASYNC) != 0U) {
        flags |= FILE_FLAG_OVERLAPPED;
    }
    if ((mode & NPH_FILE_SEQUENTIAL) != 0U) {
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    }
    if ((mode & NPH_FILE_RANDOM) != 0U) {
        flags |= FILE_FLAG_RANDOM_ACCESS;
    }
    // Share everything, as POSIX does: the engine does not use mandatory
    // locks, and an editor holding a file open must not break a cook.
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE h = CreateFileW(wide, access, share, NULL, disposition, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return file_fail("CreateFileW", path, GetLastError());
    }
    out->native = (u64)(uptr)h;
    out->mode = mode;
    out->open = 1;
    return NPH_RESULT_OK;
}

void nph_file_close(NphFile *f)
{
    NPH_ASSERT(f != NULL);
    if (f->open) {
        CloseHandle(file_handle(f));
    }
    memset(f, 0, sizeof *f);
}

// One positional transfer of at most IO_CHUNK_MAX bytes. For overlapped
// handles the call completes through an event whose low bit is set, which
// keeps the completion off any I/O completion port the handle is bound to.
static NphResult transfer_at(NphFile *f, u64 offset, void *buf, u64 size, bool write, u64 *out_done)
{
    *out_done = 0U;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)(offset & 0xFFFFFFFFU);
    ov.OffsetHigh = (DWORD)(offset >> 32U);
    HANDLE event = NULL;
    if (is_async(f)) {
        event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (event == NULL) {
            return file_fail("CreateEventW", NULL, GetLastError());
        }
        ov.hEvent = (HANDLE)((uptr)event | 1U);
    }
    const DWORD chunk = (DWORD)(size > IO_CHUNK_MAX ? IO_CHUNK_MAX : size);
    DWORD done = 0U;
    BOOL ok = write ? WriteFile(file_handle(f), buf, chunk, &done, &ov)
                    : ReadFile(file_handle(f), buf, chunk, &done, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(file_handle(f), &ov, &done, TRUE);
        error = ok ? ERROR_SUCCESS : GetLastError();
    }
    if (event != NULL) {
        CloseHandle(event);
    }
    if (!ok && error != ERROR_HANDLE_EOF) {
        return file_fail(write ? "WriteFile" : "ReadFile", NULL, error);
    }
    *out_done = done;
    return NPH_RESULT_OK;
}

NphResult nph_file_read_at(NphFile *f, u64 offset, void *buf, u64 size, u64 *out_read)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_read_at");
    NPH_ASSERT(f != NULL && f->open && out_read != NULL && (buf != NULL || size == 0U));
    *out_read = 0U;
    while (*out_read < size) {
        u64 got = 0U;
        NPH_TRY(transfer_at(f, offset + *out_read, (u8 *)buf + *out_read, size - *out_read, false,
                            &got));
        if (got == 0U) {
            break; // end of file
        }
        *out_read += got;
    }
    return NPH_RESULT_OK;
}

NphResult nph_file_write_at(NphFile *f, u64 offset, const void *buf, u64 size, u64 *out_written)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_write_at");
    NPH_ASSERT(f != NULL && f->open && (buf != NULL || size == 0U));
    u64 written = 0U;
    NphResult r = NPH_RESULT_OK;
    while (written < size) {
        u64 done = 0U;
        // WriteFile takes a non-const buffer pointer; it never writes to it.
        r = transfer_at(f, offset + written, (u8 *)(uptr)buf + written, size - written, true,
                        &done);
        if (r != NPH_RESULT_OK) {
            break;
        }
        if (done == 0U) {
            r = NPH_RESULT_IO_ERROR;
            break;
        }
        written += done;
    }
    if (out_written != NULL) {
        *out_written = written;
    }
    return r;
}

NphResult nph_file_read(NphFile *f, void *buf, u64 size, u64 *out_read)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_read");
    NPH_ASSERT(f != NULL && f->open && out_read != NULL);
    if (is_async(f)) {
        NPH_TRY(nph_file_read_at(f, f->position, buf, size, out_read));
        f->position += *out_read;
        return NPH_RESULT_OK;
    }
    *out_read = 0U;
    while (*out_read < size) {
        const DWORD chunk =
            (DWORD)(size - *out_read > IO_CHUNK_MAX ? IO_CHUNK_MAX : size - *out_read);
        DWORD got = 0U;
        if (!ReadFile(file_handle(f), (u8 *)buf + *out_read, chunk, &got, NULL)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE) {
                break;
            }
            return file_fail("ReadFile", NULL, error);
        }
        if (got == 0U) {
            break;
        }
        *out_read += got;
    }
    return NPH_RESULT_OK;
}

NphResult nph_file_write(NphFile *f, const void *buf, u64 size, u64 *out_written)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_write");
    NPH_ASSERT(f != NULL && f->open);
    if (is_async(f)) {
        if ((f->mode & NPH_FILE_APPEND) != 0U) {
            NPH_TRY(nph_file_size(f, &f->position));
        }
        u64 written = 0U;
        const NphResult r = nph_file_write_at(f, f->position, buf, size, &written);
        f->position += written;
        if (out_written != NULL) {
            *out_written = written;
        }
        return r;
    }
    if ((f->mode & NPH_FILE_APPEND) != 0U) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(file_handle(f), zero, NULL, FILE_END)) {
            return file_fail("SetFilePointerEx(FILE_END)", NULL, GetLastError());
        }
    }
    u64 written = 0U;
    NphResult r = NPH_RESULT_OK;
    while (written < size) {
        const DWORD chunk = (DWORD)(size - written > IO_CHUNK_MAX ? IO_CHUNK_MAX : size - written);
        DWORD done = 0U;
        if (!WriteFile(file_handle(f), (const u8 *)buf + written, chunk, &done, NULL)) {
            r = file_fail("WriteFile", NULL, GetLastError());
            break;
        }
        if (done == 0U) {
            r = NPH_RESULT_IO_ERROR;
            break;
        }
        written += done;
    }
    if (out_written != NULL) {
        *out_written = written;
    }
    return r;
}

NphResult nph_file_seek(NphFile *f, i64 offset, NphFileSeek origin, u64 *out_position)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_seek");
    NPH_ASSERT(f != NULL && f->open);
    if (is_async(f)) {
        i64 base;
        switch (origin) {
        case NPH_FILE_SEEK_SET:
            base = 0;
            break;
        case NPH_FILE_SEEK_CURRENT:
            base = (i64)f->position;
            break;
        case NPH_FILE_SEEK_END: {
            u64 size = 0U;
            NPH_TRY(nph_file_size(f, &size));
            base = (i64)size;
            break;
        }
        default:
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        if (base + offset < 0) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        f->position = (u64)(base + offset);
        if (out_position != NULL) {
            *out_position = f->position;
        }
        return NPH_RESULT_OK;
    }
    DWORD method;
    switch (origin) {
    case NPH_FILE_SEEK_SET:
        method = FILE_BEGIN;
        break;
    case NPH_FILE_SEEK_CURRENT:
        method = FILE_CURRENT;
        break;
    case NPH_FILE_SEEK_END:
        method = FILE_END;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    LARGE_INTEGER distance;
    LARGE_INTEGER result;
    distance.QuadPart = offset;
    if (!SetFilePointerEx(file_handle(f), distance, &result, method)) {
        const DWORD error = GetLastError();
        return error == ERROR_NEGATIVE_SEEK ? NPH_RESULT_INVALID_ARGUMENT
                                            : file_fail("SetFilePointerEx", NULL, error);
    }
    if (out_position != NULL) {
        *out_position = (u64)result.QuadPart;
    }
    return NPH_RESULT_OK;
}

NphResult nph_file_tell(NphFile *f, u64 *out_position)
{
    NPH_ASSERT(out_position != NULL);
    return nph_file_seek(f, 0, NPH_FILE_SEEK_CURRENT, out_position);
}

NphResult nph_file_size(NphFile *f, u64 *out_size)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_size");
    NPH_ASSERT(f != NULL && f->open && out_size != NULL);
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file_handle(f), &size)) {
        *out_size = 0U;
        return file_fail("GetFileSizeEx", NULL, GetLastError());
    }
    *out_size = (u64)size.QuadPart;
    return NPH_RESULT_OK;
}

NphResult nph_file_truncate(NphFile *f, u64 size)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_truncate");
    NPH_ASSERT(f != NULL && f->open);
    // SetEndOfFile cuts at the file pointer; move it there and back.
    LARGE_INTEGER saved;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(file_handle(f), zero, &saved, FILE_CURRENT)) {
        return file_fail("SetFilePointerEx(FILE_CURRENT)", NULL, GetLastError());
    }
    LARGE_INTEGER end;
    end.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(file_handle(f), end, NULL, FILE_BEGIN)) {
        return file_fail("SetFilePointerEx(FILE_BEGIN)", NULL, GetLastError());
    }
    const BOOL ok = SetEndOfFile(file_handle(f));
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    (void)SetFilePointerEx(file_handle(f), saved, NULL, FILE_BEGIN);
    if (!ok) {
        return file_fail("SetEndOfFile", NULL, error);
    }
    if (is_async(f) && f->position > size) {
        f->position = size;
    }
    return NPH_RESULT_OK;
}

NphResult nph_file_flush(NphFile *f)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_flush");
    NPH_ASSERT(f != NULL && f->open);
    if (!FlushFileBuffers(file_handle(f))) {
        return file_fail("FlushFileBuffers", NULL, GetLastError());
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Memory mapping
// ---------------------------------------------------------------------------

NphResult nph_file_map(NphFile *f, u64 offset, u64 size, NphFileMapAccess access,
                       NphFileMapping *out)
{
    NPH_REALTIME_CHECK_HERE(NPH_REALTIME_VIOLATION_IO, "nph_file_map");
    NPH_ASSERT(f != NULL && f->open && out != NULL);
    memset(out, 0, sizeof *out);
    u64 file_size = 0U;
    NPH_TRY(nph_file_size(f, &file_size));
    if (size == 0U) {
        if (offset >= file_size) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        size = file_size - offset;
    }
    DWORD protect;
    DWORD view_access;
    switch (access) {
    case NPH_FILE_MAP_READ:
        protect = PAGE_READONLY;
        view_access = FILE_MAP_READ;
        break;
    case NPH_FILE_MAP_READ_WRITE:
        protect = PAGE_READWRITE;
        view_access = FILE_MAP_READ | FILE_MAP_WRITE;
        break;
    case NPH_FILE_MAP_COPY:
        protect = PAGE_WRITECOPY;
        view_access = FILE_MAP_COPY;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // A section covering the mapped end; the file may be extended by a
    // READ_WRITE mapping past its current size.
    const u64 end = offset + size;
    const HANDLE section = CreateFileMappingW(file_handle(f), NULL, protect, (DWORD)(end >> 32U),
                                              (DWORD)(end & 0xFFFFFFFFU), NULL);
    if (section == NULL) {
        return file_fail("CreateFileMappingW", NULL, GetLastError());
    }
    const u64 granularity = nph_vmem_allocation_granularity();
    const u64 view_offset = nph_align_down_u64(offset, granularity);
    const u64 delta = offset - view_offset;
    const u64 view_size = delta + size;
    void *view = MapViewOfFile(section, view_access, (DWORD)(view_offset >> 32U),
                               (DWORD)(view_offset & 0xFFFFFFFFU), (SIZE_T)view_size);
    if (view == NULL) {
        const DWORD error = GetLastError();
        CloseHandle(section);
        return file_fail("MapViewOfFile", NULL, error);
    }
    out->view = (u8 *)view;
    out->view_size = view_size;
    out->data = (u8 *)view + delta;
    out->size = size;
    out->section = section;
    return NPH_RESULT_OK;
}

void nph_file_unmap(NphFileMapping *m)
{
    NPH_ASSERT(m != NULL);
    if (m->view != NULL) {
        NPH_VERIFY(UnmapViewOfFile(m->view));
    }
    if (m->section != NULL) {
        CloseHandle((HANDLE)m->section);
    }
    memset(m, 0, sizeof *m);
}

NphResult nph_file_map_flush(NphFileMapping *m)
{
    NPH_ASSERT(m != NULL && m->view != NULL);
    if (!FlushViewOfFile(m->view, (SIZE_T)m->view_size)) {
        return file_fail("FlushViewOfFile", NULL, GetLastError());
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// File system
// ---------------------------------------------------------------------------

NphResult nph_fs_stat(const char *path, NphFileInfo *out)
{
    NPH_ASSERT(path != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(path, wide));
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        return file_fail("GetFileAttributesExW", path, GetLastError());
    }
    const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    out->is_directory = directory;
    out->is_regular = !directory && (data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) == 0U;
    out->size = directory ? 0U : (((u64)data.nFileSizeHigh << 32U) | data.nFileSizeLow);
    out->modified_ns = filetime_to_unix_ns(data.ftLastWriteTime);
    return NPH_RESULT_OK;
}

bool nph_fs_exists(const char *path)
{
    NPH_ASSERT(path != NULL);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    if (to_wide(path, wide) != NPH_RESULT_OK) {
        return false;
    }
    return GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

NphResult nph_fs_remove_file(const char *path)
{
    NPH_ASSERT(path != NULL);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(path, wide));
    if (!DeleteFileW(wide)) {
        return file_fail("DeleteFileW", path, GetLastError());
    }
    return NPH_RESULT_OK;
}

// A rename with POSIX semantics (Windows 10 1809 and later, on NTFS): the
// destination is replaced even while it is open or mapped elsewhere, as on
// Linux. Those handles and views keep the old file until they close, and
// opens by the name find the new one. Returns the error, 0 on success.
static DWORD rename_posix(const wchar_t *from, const wchar_t *to)
{
    // The information carries the destination's full path in place.
    union {
        FILE_RENAME_INFO info;
        u8 bytes[sizeof(FILE_RENAME_INFO) + NPH_WIN32_PATH_MAX * sizeof(wchar_t)];
    } rename;
    memset(&rename.info, 0, sizeof rename.info);
    const DWORD n = GetFullPathNameW(to, NPH_WIN32_PATH_MAX, rename.info.FileName, NULL);
    if (n == 0U || n >= NPH_WIN32_PATH_MAX) {
        return n == 0U ? GetLastError() : (DWORD)ERROR_FILENAME_EXCED_RANGE;
    }
    rename.info.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename.info.FileNameLength = n * (DWORD)sizeof(wchar_t);
    HANDLE h = CreateFileW(from, DELETE | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    const BOOL ok = SetFileInformationByHandle(
        h, FileRenameInfoEx, &rename.info, (DWORD)sizeof rename.info + rename.info.FileNameLength);
    const DWORD error = ok ? 0U : GetLastError();
    CloseHandle(h);
    return error;
}

// Moves `from` over `to` (both wide), with POSIX semantics where the
// system has them. `path` names `from` in messages.
static NphResult replace(const wchar_t *from, const wchar_t *to, const char *path)
{
    const DWORD error = rename_posix(from, to);
    if (error == 0U) {
        return NPH_RESULT_OK;
    }
    // Older systems, other file systems, and other volumes: the classic
    // move, which cannot replace a destination that is mapped, or open
    // without delete sharing.
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
        error != ERROR_INVALID_FUNCTION && error != ERROR_NOT_SAME_DEVICE) {
        return file_fail("SetFileInformationByHandle", path, error);
    }
    if (!MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return file_fail("MoveFileExW", path, GetLastError());
    }
    return NPH_RESULT_OK;
}

NphResult nph_fs_rename(const char *from, const char *to)
{
    NPH_ASSERT(from != NULL && to != NULL);
    wchar_t wfrom[NPH_WIN32_PATH_MAX];
    wchar_t wto[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(from, wfrom));
    NPH_TRY(to_wide(to, wto));
    return replace(wfrom, wto, from);
}

NphResult nph_fs_copy_file(const char *from, const char *to)
{
    NPH_ASSERT(from != NULL && to != NULL);
    NphFileInfo info;
    NPH_TRY(nph_fs_stat(from, &info));
    if (info.is_directory) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    char temp[NPH_WIN32_PATH_MAX];
    const int n = snprintf(temp, sizeof temp, "%s.part", to);
    if (n < 0 || (u64)n >= sizeof temp) {
        return NPH_RESULT_OVERFLOW;
    }
    // Two wide buffers: the source's is reused for the destination once the
    // bytes are copied.
    wchar_t wpath[NPH_WIN32_PATH_MAX];
    wchar_t wtemp[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(from, wpath));
    NPH_TRY(to_wide(temp, wtemp));
    if (!CopyFileExW(wpath, wtemp, NULL, NULL, NULL, 0U)) {
        return file_fail("CopyFileExW", from, GetLastError());
    }
    // CopyFileExW copies the attributes: a read-only source (a version
    // control checkout) would make a copy that the next copy cannot replace.
    const DWORD attributes = GetFileAttributesW(wtemp);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0U) {
        (void)SetFileAttributesW(wtemp, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    }
    NphResult r = to_wide(to, wpath);
    if (r == NPH_RESULT_OK) {
        r = replace(wtemp, wpath, temp);
    }
    if (r != NPH_RESULT_OK) {
        (void)DeleteFileW(wtemp);
    }
    return r;
}

NphResult nph_fs_create_directory(const char *path)
{
    NPH_ASSERT(path != NULL);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(path, wide));
    if (CreateDirectoryW(wide, NULL)) {
        return NPH_RESULT_OK;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            return NPH_RESULT_OK;
        }
        return NPH_RESULT_ALREADY_EXISTS; // a file is in the way
    }
    return file_fail("CreateDirectoryW", path, error);
}

NphResult nph_fs_remove_directory(const char *path)
{
    NPH_ASSERT(path != NULL);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(path, wide));
    if (!RemoveDirectoryW(wide)) {
        return file_fail("RemoveDirectoryW", path, GetLastError());
    }
    return NPH_RESULT_OK;
}

NphResult nph_fs_current_directory(char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    const DWORD n = GetCurrentDirectoryW((DWORD)NPH_ARRAY_COUNT(wide), wide);
    if (n == 0U || n >= NPH_ARRAY_COUNT(wide)) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return n == 0U ? file_fail("GetCurrentDirectoryW", NULL, GetLastError())
                       : NPH_RESULT_OVERFLOW;
    }
    return nph_win32_wide_to_utf8(wide, buf, cap, out_len);
}

NphResult nph_fs_temp_directory(char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    DWORD n = GetTempPathW((DWORD)NPH_ARRAY_COUNT(wide), wide);
    if (n == 0U || n >= NPH_ARRAY_COUNT(wide)) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return n == 0U ? file_fail("GetTempPathW", NULL, GetLastError()) : NPH_RESULT_OVERFLOW;
    }
    // GetTempPath appends a backslash; paths here never end in one.
    while (n > 1U && (wide[n - 1U] == L'\\' || wide[n - 1U] == L'/')) {
        wide[--n] = L'\0';
    }
    return nph_win32_wide_to_utf8(wide, buf, cap, out_len);
}

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------

static bool is_dot_entry(const wchar_t *name)
{
    return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

static void fill_entry(NphDirEntry *entry, const WIN32_FIND_DATAW *data)
{
    u64 len = 0U;
    // Truncation (OVERFLOW) leaves a terminated prefix, which is the
    // documented behaviour for over-long names.
    (void)nph_win32_wide_to_utf8(data->cFileName, entry->name, sizeof entry->name, &len);
    entry->is_directory = (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
}

NphResult nph_dir_open(const char *path, NphDirIterator *it)
{
    NPH_ASSERT(path != NULL && it != NULL);
    memset(it, 0, sizeof *it);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    NPH_TRY(to_wide(path, wide));
    const DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return file_fail("GetFileAttributesW", path, GetLastError());
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    size_t len = wcslen(wide);
    if (len + 3U > NPH_ARRAY_COUNT(wide)) {
        return NPH_RESULT_OVERFLOW;
    }
    if (len > 0U && wide[len - 1U] != L'\\' && wide[len - 1U] != L'/') {
        wide[len++] = L'\\';
    }
    wide[len++] = L'*';
    wide[len] = L'\0';
    WIN32_FIND_DATAW data;
    const HANDLE h = FindFirstFileExW(wide, FindExInfoBasic, &data, FindExSearchNameMatch, NULL,
                                      FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            it->open = 1; // an empty directory on some file systems
            it->handle = INVALID_HANDLE_VALUE;
            return NPH_RESULT_OK;
        }
        return file_fail("FindFirstFileExW", path, error);
    }
    it->handle = h;
    it->open = 1;
    if (!is_dot_entry(data.cFileName)) {
        fill_entry(&it->pending, &data);
        it->has_pending = 1;
    }
    return NPH_RESULT_OK;
}

bool nph_dir_next(NphDirIterator *it, NphDirEntry *out)
{
    NPH_ASSERT(it != NULL && it->open && out != NULL);
    if (it->has_pending) {
        *out = it->pending;
        it->has_pending = 0;
        return true;
    }
    const HANDLE h = (HANDLE)it->handle;
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    WIN32_FIND_DATAW data;
    while (FindNextFileW(h, &data)) {
        if (!is_dot_entry(data.cFileName)) {
            fill_entry(out, &data);
            return true;
        }
    }
    return false;
}

void nph_dir_close(NphDirIterator *it)
{
    NPH_ASSERT(it != NULL);
    if (it->open) {
        const HANDLE h = (HANDLE)it->handle;
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
        }
    }
    memset(it, 0, sizeof *it);
}
