// SPDX-License-Identifier: Apache-2.0
// Process helpers, error mapping, and string conversions for Windows.
#include <nepheline/core/assert.h>
#include <nepheline/platform/platform.h>

#include "platform_internal.h"

#include <stdio.h>
#include <string.h>

NphResult nph_win32_result(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_MOD_NOT_FOUND:
    case ERROR_PROC_NOT_FOUND:
    case ERROR_ENVVAR_NOT_FOUND:
    case ERROR_NO_MORE_FILES:
        return NPH_RESULT_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
        return NPH_RESULT_ACCESS_DENIED;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:
    case ERROR_DIR_NOT_EMPTY:
        return NPH_RESULT_BUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return NPH_RESULT_ALREADY_EXISTS;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
    case ERROR_NO_SYSTEM_RESOURCES:
        return NPH_RESULT_OUT_OF_MEMORY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_TOO_MANY_OPEN_FILES:
        return NPH_RESULT_FULL;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
    case ERROR_BAD_ARGUMENTS:
    case ERROR_NEGATIVE_SEEK:
    case ERROR_INVALID_ADDRESS:
    case ERROR_INVALID_NAME:
    case ERROR_DIRECTORY:
        return NPH_RESULT_INVALID_ARGUMENT;
    case ERROR_INSUFFICIENT_BUFFER:
    case ERROR_MORE_DATA:
    case ERROR_BUFFER_OVERFLOW:
    case ERROR_FILENAME_EXCED_RANGE:
        return NPH_RESULT_OVERFLOW;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
    case ERROR_INVALID_FUNCTION:
        return NPH_RESULT_UNSUPPORTED;
    case ERROR_HANDLE_EOF:
        return NPH_RESULT_END_OF_STREAM;
    case ERROR_NO_UNICODE_TRANSLATION:
        return NPH_RESULT_PARSE_ERROR;
    default:
        return NPH_RESULT_IO_ERROR;
    }
}

u32 nph_win32_error_message(DWORD error, char *buf, u32 cap)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    wchar_t wide[512];
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                   error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), wide,
                                   (DWORD)NPH_ARRAY_COUNT(wide), NULL);
    if (n == 0U) {
        const int written = snprintf(buf, cap, "error %lu", (unsigned long)error);
        return written < 0 ? 0U : (u32)written;
    }
    // Trim the trailing "\r\n." FormatMessage appends.
    DWORD len = n;
    while (len > 0U &&
           (wide[len - 1U] == L'\r' || wide[len - 1U] == L'\n' || wide[len - 1U] == L' ')) {
        --len;
    }
    if (len > 0U && wide[len - 1U] == L'.') {
        --len;
    }
    wide[len] = L'\0';
    u64 out_len = 0U;
    if (nph_win32_wide_to_utf8(wide, buf, cap, &out_len) != NPH_RESULT_OK) {
        // Truncated: the conversion left a prefix; make sure it is terminated.
        buf[cap - 1U] = '\0';
        return (u32)strlen(buf);
    }
    return (u32)out_len;
}

NphResult nph_win32_fail(const char *what, DWORD error)
{
    char message[256];
    nph_win32_error_message(error, message, (u32)sizeof message);
    NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "%s: %s (%lu)", what, message, (unsigned long)error);
    return nph_win32_result(error);
}

NphResult nph_win32_utf8_to_wide(const char *utf8, wchar_t *out, u32 cap)
{
    NPH_ASSERT(utf8 != NULL && out != NULL && cap > 0U);
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, (int)cap);
    if (n > 0) {
        return NPH_RESULT_OK;
    }
    out[0] = L'\0';
    const DWORD error = GetLastError();
    if (error == ERROR_INSUFFICIENT_BUFFER) {
        return NPH_RESULT_OVERFLOW;
    }
    return NPH_RESULT_INVALID_ARGUMENT;
}

NphResult nph_win32_wide_to_utf8(const wchar_t *wide, char *out, u64 cap, u64 *out_len)
{
    NPH_ASSERT(wide != NULL && out != NULL && cap > 0U);
    const int cap_int = cap > (u64)INT32_MAX ? INT32_MAX : (int)cap;
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, cap_int, NULL, NULL);
    if (n > 0) {
        if (out_len != NULL) {
            *out_len = (u64)n - 1U; // n counts the terminator
        }
        return NPH_RESULT_OK;
    }
    const DWORD error = GetLastError();
    out[0] = '\0';
    if (error == ERROR_INSUFFICIENT_BUFFER) {
        n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
        if (out_len != NULL) {
            *out_len = n > 0 ? (u64)n - 1U : 0U;
        }
        return NPH_RESULT_OVERFLOW;
    }
    if (out_len != NULL) {
        *out_len = 0U;
    }
    return NPH_RESULT_INVALID_ARGUMENT;
}

NphResult nph_platform_executable_path(char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    const DWORD n = GetModuleFileNameW(NULL, wide, (DWORD)NPH_ARRAY_COUNT(wide));
    if (n == 0U || n >= NPH_ARRAY_COUNT(wide)) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        return n == 0U ? nph_win32_fail("GetModuleFileNameW", GetLastError()) : NPH_RESULT_OVERFLOW;
    }
    return nph_win32_wide_to_utf8(wide, buf, cap, out_len);
}

NphResult nph_platform_env_get(const char *name, char *buf, u64 cap, u64 *out_len)
{
    NPH_ASSERT(name != NULL && buf != NULL && cap > 0U);
    wchar_t wname[256];
    NphResult r = nph_win32_utf8_to_wide(name, wname, (u32)NPH_ARRAY_COUNT(wname));
    if (r != NPH_RESULT_OK) {
        buf[0] = '\0';
        return r;
    }
    wchar_t wvalue[NPH_WIN32_PATH_MAX];
    const DWORD n = GetEnvironmentVariableW(wname, wvalue, (DWORD)NPH_ARRAY_COUNT(wvalue));
    if (n == 0U) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = 0U;
        }
        const DWORD error = GetLastError();
        // An empty value also returns 0, with no error set.
        return error == ERROR_SUCCESS ? NPH_RESULT_OK : nph_win32_result(error);
    }
    if (n >= NPH_ARRAY_COUNT(wvalue)) {
        buf[0] = '\0';
        if (out_len != NULL) {
            *out_len = n;
        }
        return NPH_RESULT_OVERFLOW;
    }
    return nph_win32_wide_to_utf8(wvalue, buf, cap, out_len);
}

u64 nph_platform_process_id(void)
{
    return (u64)GetCurrentProcessId();
}

static bool enable_vt_on(DWORD std_handle)
{
    const HANDLE h = GetStdHandle(std_handle);
    if (h == INVALID_HANDLE_VALUE || h == NULL) {
        return false;
    }
    DWORD mode = 0U;
    if (!GetConsoleMode(h, &mode)) {
        return false; // redirected to a file or pipe
    }
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0U) {
        return true;
    }
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

bool nph_platform_console_enable_vt(void)
{
    const bool out = enable_vt_on(STD_OUTPUT_HANDLE);
    const bool err = enable_vt_on(STD_ERROR_HANDLE);
    return out || err;
}

bool nph_platform_debugger_present(void)
{
    return IsDebuggerPresent() != 0;
}
