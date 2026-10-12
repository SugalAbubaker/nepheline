// SPDX-License-Identifier: Apache-2.0
// Files, memory mapping, and the file system on POSIX (Linux).
#include <nepheline/core/assert.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/vmem.h>

#include "platform_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Largest single read/write: the kernel caps at 2 GiB minus a page anyway.
#define IO_CHUNK_MAX ((u64)1U << 30U)

static int file_fd(const NphFile *f)
{
    return (int)f->native;
}

static NphResult file_fail(const char *what, const char *path, int error)
{
    const NphResult r = nph_posix_result(error);
    char message[256];
    const char *text = strerror_r(error, message, sizeof message);
    if (r == NPH_RESULT_NOT_FOUND || r == NPH_RESULT_ALREADY_EXISTS ||
        r == NPH_RESULT_ACCESS_DENIED || r == NPH_RESULT_BUSY) {
        NPH_LOG_DEBUG(&NPH_LOG_PLATFORM, "%s(%s): %s (%d)", what, path != NULL ? path : "", text,
                      error);
    } else {
        NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "%s(%s): %s (%d)", what, path != NULL ? path : "", text,
                      error);
    }
    return r;
}

static u64 timespec_to_ns(struct timespec ts)
{
    return (u64)ts.tv_sec * NPH_TIME_NS_PER_S + (u64)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

NphResult nph_file_open(const char *path, u32 mode, NphFile *out)
{
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
    int flags = O_CLOEXEC;
    flags |= read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY);
    if ((mode & NPH_FILE_CREATE) != 0U) {
        flags |= O_CREAT;
    }
    if ((mode & NPH_FILE_EXCLUSIVE) != 0U) {
        flags |= O_EXCL;
    }
    if ((mode & NPH_FILE_TRUNCATE) != 0U) {
        flags |= O_TRUNC;
    }
    if ((mode & NPH_FILE_APPEND) != 0U) {
        flags |= O_APPEND;
    }
    const int fd = open(path, flags, 0644);
    if (fd < 0) {
        return file_fail("open", path, errno);
    }
    if ((mode & NPH_FILE_SEQUENTIAL) != 0U) {
        (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    } else if ((mode & NPH_FILE_RANDOM) != 0U) {
        (void)posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    }
    out->native = (u64)fd;
    out->mode = mode;
    out->open = 1;
    return NPH_RESULT_OK;
}

void nph_file_close(NphFile *f)
{
    NPH_ASSERT(f != NULL);
    if (f->open) {
        (void)close(file_fd(f));
    }
    memset(f, 0, sizeof *f);
}

NphResult nph_file_read(NphFile *f, void *buf, u64 size, u64 *out_read)
{
    NPH_ASSERT(f != NULL && f->open && out_read != NULL && (buf != NULL || size == 0U));
    *out_read = 0U;
    while (*out_read < size) {
        const u64 chunk = size - *out_read > IO_CHUNK_MAX ? IO_CHUNK_MAX : size - *out_read;
        const ssize_t got = read(file_fd(f), (u8 *)buf + *out_read, (size_t)chunk);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return file_fail("read", NULL, errno);
        }
        if (got == 0) {
            break;
        }
        *out_read += (u64)got;
    }
    return NPH_RESULT_OK;
}

NphResult nph_file_write(NphFile *f, const void *buf, u64 size, u64 *out_written)
{
    NPH_ASSERT(f != NULL && f->open && (buf != NULL || size == 0U));
    u64 written = 0U;
    NphResult r = NPH_RESULT_OK;
    while (written < size) {
        const u64 chunk = size - written > IO_CHUNK_MAX ? IO_CHUNK_MAX : size - written;
        const ssize_t done = write(file_fd(f), (const u8 *)buf + written, (size_t)chunk);
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            r = file_fail("write", NULL, errno);
            break;
        }
        if (done == 0) {
            r = NPH_RESULT_IO_ERROR;
            break;
        }
        written += (u64)done;
    }
    if (out_written != NULL) {
        *out_written = written;
    }
    return r;
}

NphResult nph_file_read_at(NphFile *f, u64 offset, void *buf, u64 size, u64 *out_read)
{
    NPH_ASSERT(f != NULL && f->open && out_read != NULL && (buf != NULL || size == 0U));
    *out_read = 0U;
    while (*out_read < size) {
        const u64 chunk = size - *out_read > IO_CHUNK_MAX ? IO_CHUNK_MAX : size - *out_read;
        const ssize_t got =
            pread(file_fd(f), (u8 *)buf + *out_read, (size_t)chunk, (off_t)(offset + *out_read));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return file_fail("pread", NULL, errno);
        }
        if (got == 0) {
            break;
        }
        *out_read += (u64)got;
    }
    return NPH_RESULT_OK;
}

NphResult nph_file_write_at(NphFile *f, u64 offset, const void *buf, u64 size, u64 *out_written)
{
    NPH_ASSERT(f != NULL && f->open && (buf != NULL || size == 0U));
    u64 written = 0U;
    NphResult r = NPH_RESULT_OK;
    while (written < size) {
        const u64 chunk = size - written > IO_CHUNK_MAX ? IO_CHUNK_MAX : size - written;
        const ssize_t done =
            pwrite(file_fd(f), (const u8 *)buf + written, (size_t)chunk, (off_t)(offset + written));
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            r = file_fail("pwrite", NULL, errno);
            break;
        }
        if (done == 0) {
            r = NPH_RESULT_IO_ERROR;
            break;
        }
        written += (u64)done;
    }
    if (out_written != NULL) {
        *out_written = written;
    }
    return r;
}

NphResult nph_file_seek(NphFile *f, i64 offset, NphFileSeek origin, u64 *out_position)
{
    NPH_ASSERT(f != NULL && f->open);
    int whence;
    switch (origin) {
    case NPH_FILE_SEEK_SET:
        whence = SEEK_SET;
        break;
    case NPH_FILE_SEEK_CURRENT:
        whence = SEEK_CUR;
        break;
    case NPH_FILE_SEEK_END:
        whence = SEEK_END;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const off_t result = lseek(file_fd(f), (off_t)offset, whence);
    if (result < 0) {
        return errno == EINVAL ? NPH_RESULT_INVALID_ARGUMENT : file_fail("lseek", NULL, errno);
    }
    if (out_position != NULL) {
        *out_position = (u64)result;
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
    NPH_ASSERT(f != NULL && f->open && out_size != NULL);
    struct stat st;
    if (fstat(file_fd(f), &st) != 0) {
        *out_size = 0U;
        return file_fail("fstat", NULL, errno);
    }
    *out_size = (u64)st.st_size;
    return NPH_RESULT_OK;
}

NphResult nph_file_truncate(NphFile *f, u64 size)
{
    NPH_ASSERT(f != NULL && f->open);
    if (ftruncate(file_fd(f), (off_t)size) != 0) {
        return file_fail("ftruncate", NULL, errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_file_flush(NphFile *f)
{
    NPH_ASSERT(f != NULL && f->open);
    if (fsync(file_fd(f)) != 0) {
        return file_fail("fsync", NULL, errno);
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Memory mapping
// ---------------------------------------------------------------------------

NphResult nph_file_map(NphFile *f, u64 offset, u64 size, NphFileMapAccess access,
                       NphFileMapping *out)
{
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
    int prot;
    int flags;
    switch (access) {
    case NPH_FILE_MAP_READ:
        prot = PROT_READ;
        flags = MAP_SHARED;
        break;
    case NPH_FILE_MAP_READ_WRITE:
        prot = PROT_READ | PROT_WRITE;
        flags = MAP_SHARED;
        break;
    case NPH_FILE_MAP_COPY:
        prot = PROT_READ | PROT_WRITE;
        flags = MAP_PRIVATE;
        break;
    default:
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 page = nph_vmem_page_size();
    const u64 view_offset = nph_align_down_u64(offset, page);
    const u64 delta = offset - view_offset;
    const u64 view_size = delta + size;
    void *view = mmap(NULL, (size_t)view_size, prot, flags, file_fd(f), (off_t)view_offset);
    if (view == MAP_FAILED) {
        return file_fail("mmap", NULL, errno);
    }
    out->view = (u8 *)view;
    out->view_size = view_size;
    out->data = (u8 *)view + delta;
    out->size = size;
    return NPH_RESULT_OK;
}

void nph_file_unmap(NphFileMapping *m)
{
    NPH_ASSERT(m != NULL);
    if (m->view != NULL) {
        NPH_VERIFY(munmap(m->view, (size_t)m->view_size) == 0);
    }
    memset(m, 0, sizeof *m);
}

NphResult nph_file_map_flush(NphFileMapping *m)
{
    NPH_ASSERT(m != NULL && m->view != NULL);
    if (msync(m->view, (size_t)m->view_size, MS_SYNC) != 0) {
        return file_fail("msync", NULL, errno);
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
    struct stat st;
    if (stat(path, &st) != 0) {
        return file_fail("stat", path, errno);
    }
    out->is_directory = S_ISDIR(st.st_mode);
    out->is_regular = S_ISREG(st.st_mode);
    out->size = out->is_directory ? 0U : (u64)st.st_size;
    out->modified_ns = timespec_to_ns(st.st_mtim);
    return NPH_RESULT_OK;
}

bool nph_fs_exists(const char *path)
{
    NPH_ASSERT(path != NULL);
    return access(path, F_OK) == 0;
}

NphResult nph_fs_remove_file(const char *path)
{
    NPH_ASSERT(path != NULL);
    if (unlink(path) != 0) {
        return file_fail("unlink", path, errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_fs_rename(const char *from, const char *to)
{
    NPH_ASSERT(from != NULL && to != NULL);
    if (rename(from, to) != 0) {
        return file_fail("rename", from, errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_fs_create_directory(const char *path)
{
    NPH_ASSERT(path != NULL);
    if (mkdir(path, 0755) == 0) {
        return NPH_RESULT_OK;
    }
    const int error = errno;
    if (error == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return NPH_RESULT_OK;
        }
        return NPH_RESULT_ALREADY_EXISTS; // a file is in the way
    }
    return file_fail("mkdir", path, error);
}

NphResult nph_fs_remove_directory(const char *path)
{
    NPH_ASSERT(path != NULL);
    if (rmdir(path) != 0) {
        return file_fail("rmdir", path, errno);
    }
    return NPH_RESULT_OK;
}

NphResult nph_fs_current_directory(char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    char tmp[4096];
    if (getcwd(tmp, sizeof tmp) == NULL) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return errno == ERANGE ? NPH_RESULT_OVERFLOW : file_fail("getcwd", NULL, errno);
    }
    const u64 len = strlen(tmp);
    if (out_len != NULL) {
        *out_len = len;
    }
    if (len + 1U > cap) {
        buf[0] = '\0';
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(buf, tmp, len + 1U);
    return NPH_RESULT_OK;
}

NphResult nph_fs_temp_directory(char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    u64 len = strlen(dir);
    while (len > 1U && dir[len - 1U] == '/') {
        --len;
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    if (len + 1U > cap) {
        buf[0] = '\0';
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(buf, dir, len);
    buf[len] = '\0';
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------

NphResult nph_dir_open(const char *path, NphDirIterator *it)
{
    NPH_ASSERT(path != NULL && it != NULL);
    memset(it, 0, sizeof *it);
    struct stat st;
    if (stat(path, &st) != 0) {
        return file_fail("stat", path, errno);
    }
    if (!S_ISDIR(st.st_mode)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return file_fail("opendir", path, errno);
    }
    it->handle = dir;
    it->open = 1;
    return NPH_RESULT_OK;
}

bool nph_dir_next(NphDirIterator *it, NphDirEntry *out)
{
    NPH_ASSERT(it != NULL && it->open && out != NULL);
    DIR *dir = (DIR *)it->handle;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            return false;
        }
        const char *name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        u64 len = strlen(name);
        if (len >= sizeof out->name) {
            len = sizeof out->name - 1U;
        }
        memcpy(out->name, name, len);
        out->name[len] = '\0';
        if (entry->d_type == DT_DIR) {
            out->is_directory = 1;
        } else if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK) {
            out->is_directory = 0;
        } else {
            // File systems that do not report types, and symbolic links:
            // ask relative to the directory, following links.
            struct stat st;
            const int dir_fd = dirfd(dir);
            out->is_directory =
                dir_fd >= 0 && fstatat(dir_fd, name, &st, 0) == 0 && S_ISDIR(st.st_mode);
        }
        return true;
    }
}

void nph_dir_close(NphDirIterator *it)
{
    NPH_ASSERT(it != NULL);
    if (it->open) {
        closedir((DIR *)it->handle);
    }
    memset(it, 0, sizeof *it);
}
