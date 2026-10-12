// SPDX-License-Identifier: Apache-2.0
// Process helpers and error mapping for Linux.
#include <nepheline/core/assert.h>
#include <nepheline/platform/platform.h>

#include "platform_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

NphResult nph_posix_result(int error)
{
    switch (error) {
    case ENOENT:
    case ENOTDIR:
    case ENXIO:
    case ESRCH:
        return NPH_RESULT_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:
        return NPH_RESULT_ACCESS_DENIED;
    case EBUSY:
    case ETXTBSY:
    case ENOTEMPTY:
    case EAGAIN:
        return NPH_RESULT_BUSY;
    case EEXIST:
        return NPH_RESULT_ALREADY_EXISTS;
    case ENOMEM:
        return NPH_RESULT_OUT_OF_MEMORY;
    case ENOSPC:
    case EDQUOT:
    case EMFILE:
    case ENFILE:
        return NPH_RESULT_FULL;
    case EINVAL:
    case EBADF:
    case EISDIR:
    case EFAULT:
        return NPH_RESULT_INVALID_ARGUMENT;
    case ENAMETOOLONG:
    case ERANGE:
    case EOVERFLOW:
    case E2BIG:
    case EFBIG:
        return NPH_RESULT_OVERFLOW;
    case ENOSYS:
    case EOPNOTSUPP:
        return NPH_RESULT_UNSUPPORTED;
    default:
        return NPH_RESULT_IO_ERROR;
    }
}

NphResult nph_posix_fail(const char *what, int error)
{
    char message[256];
    // strerror_r's GNU variant may return a static string instead of
    // filling the buffer; the result is what must be printed.
    const char *text = strerror_r(error, message, sizeof message);
    NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "%s: %s (%d)", what, text, error);
    return nph_posix_result(error);
}

NphResult nph_platform_executable_path(char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    char path[4096];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1U);
    if (n < 0) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return nph_posix_fail("readlink(/proc/self/exe)", errno);
    }
    const u64 len = (u64)n;
    if (out_len != NULL) {
        *out_len = len;
    }
    if (len + 1U > cap) {
        buf[0] = '\0';
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(buf, path, len);
    buf[len] = '\0';
    return NPH_RESULT_OK;
}

NphResult nph_platform_env_get(const char *name, char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(name != NULL && buf != NULL && cap > 0U);
    const char *value = getenv(name);
    if (value == NULL) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return NPH_RESULT_NOT_FOUND;
    }
    const u64 len = strlen(value);
    if (out_len != NULL) {
        *out_len = len;
    }
    if (len + 1U > cap) {
        buf[0] = '\0';
        return NPH_RESULT_OVERFLOW;
    }
    memcpy(buf, value, len + 1U);
    return NPH_RESULT_OK;
}

u64 nph_platform_process_id(void)
{
    return (u64)getpid();
}

bool nph_platform_console_enable_vt(void)
{
    return isatty(STDOUT_FILENO) != 0 || isatty(STDERR_FILENO) != 0;
}

bool nph_platform_debugger_present(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL) {
        return false;
    }
    char line[256];
    bool traced = false;
    while (fgets(line, (int)sizeof line, f) != NULL) {
        if (strncmp(line, "TracerPid:", 10U) == 0) {
            traced = strtol(line + 10, NULL, 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
}
