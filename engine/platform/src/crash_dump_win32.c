// SPDX-License-Identifier: Apache-2.0
// One attempt at a minidump, as the crash handler (crash_win32.c) and its dumper
// (dumper/main.c) both make it (docs/adr/0106-crash-dumps-from-a-separate-process.md).
#include "crash_dump_win32.h"

#include <string.h>
#include <wchar.h>

// The global data (MiniDumpWithDataSegs) is kept for the modules beside the
// executable only: the driver's alone are tens of megabytes (the player's
// dump, with NVIDIA's, came to 40 MB) and say nothing the engine's do not.
// NOLINTNEXTLINE(misc-misplaced-const): MINIDUMP_CALLBACK_ROUTINE's own signature
BOOL CALLBACK crash_win32_dump_callback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                        PMINIDUMP_CALLBACK_OUTPUT output)
{
    const CrashDumpScope *scope = (const CrashDumpScope *)param;
    if (input == NULL || output == NULL) {
        return FALSE;
    }
    switch (input->CallbackType) {
    case ModuleCallback: {
        const wchar_t *path = input->Module.FullPath;
        const bool full = scope != NULL && scope->full;
        const bool ours = scope != NULL && scope->exe_dir != NULL && scope->exe_dir_len != 0U &&
                          path != NULL &&
                          _wcsnicmp(path, scope->exe_dir, (size_t)scope->exe_dir_len) == 0 &&
                          wcschr(path + scope->exe_dir_len, L'\\') == NULL;
        if (!ours && !full) {
            output->ModuleWriteFlags &= ~(ULONG)ModuleWriteDataSeg;
        }
        return TRUE;
    }
    case IncludeModuleCallback:
    case IncludeThreadCallback:
    case ThreadCallback:
    case ThreadExCallback:
        return TRUE;
    case ReadMemoryFailureCallback:
        // Memory that cannot be read when the dump gets to it (freed by a
        // thread still running, or never readable) is left out. Otherwise
        // the one failed read fails the whole dump (MINIDUMP_CALLBACK_TYPE).
        output->Status = S_OK;
        return TRUE;
    default:
        return FALSE;
    }
}

MINIDUMP_TYPE crash_win32_dump_type(bool full)
{
    // The flags combined, as dbghelp takes them, are no one enumerator.
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    return full ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo |
                                  MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                  MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData)
                : (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                  MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData |
                                  MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                  MiniDumpWithProcessThreadData);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
}

DWORD crash_win32_dbghelp_load(HMODULE *out_module, CrashMiniDumpWriteDumpFn *out_write)
{
    *out_module = NULL;
    *out_write = NULL;
    HMODULE module = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == NULL) {
        return GetLastError();
    }
    union {
        FARPROC proc;
        CrashMiniDumpWriteDumpFn fn;
    } u;
    u.proc = GetProcAddress(module, "MiniDumpWriteDump");
    if (u.proc == NULL) {
        const DWORD error = GetLastError();
        FreeLibrary(module);
        return error != ERROR_SUCCESS ? error : ERROR_PROC_NOT_FOUND;
    }
    *out_module = module;
    *out_write = u.fn;
    return ERROR_SUCCESS;
}

DWORD crash_win32_dump_attempt(CrashMiniDumpWriteDumpFn write_dump, HANDLE process, DWORD pid,
                               HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION *mei,
                               const CrashDumpScope *scope, CrashDumpStage stage,
                               const char *comment)
{
    LARGE_INTEGER start;
    start.QuadPart = 0;
    if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
        return GetLastError();
    }
    if (stage == CRASH_DUMP_STAGE_FAIL) {
        return ERROR_INVALID_USER_BUFFER; // what dbghelp's failures were (ADR-0103)
    }
    if (stage == CRASH_DUMP_STAGE_STAND_IN) {
        static const char signature[4] = {'M', 'D', 'M', 'P'};
        DWORD written = 0U;
        return WriteFile(file, signature, (DWORD)sizeof signature, &written, NULL) &&
                       written == (DWORD)sizeof signature
                   ? ERROR_SUCCESS
                   : GetLastError();
    }
    if (write_dump == NULL) {
        return ERROR_PROC_NOT_FOUND;
    }
    // dbghelp only reads the comment, and the callback only reads the scope; both are handed on
    // through dbghelp's non-const pointers.
    union {
        const void *read_only;
        void *writable;
    } comment_u, scope_u;
    comment_u.read_only = comment;
    scope_u.read_only = scope;
    MINIDUMP_USER_STREAM stream;
    stream.Type = CommentStreamA;
    stream.BufferSize = (ULONG)strlen(comment) + 1U; // with its terminator
    stream.Buffer = comment_u.writable;
    MINIDUMP_USER_STREAM_INFORMATION streams;
    streams.UserStreamCount = 1U;
    streams.UserStreamArray = &stream;
    MINIDUMP_CALLBACK_INFORMATION callback;
    callback.CallbackRoutine = crash_win32_dump_callback;
    callback.CallbackParam = scope_u.writable;
    if (!write_dump(process, pid, file, type, mei, &streams, &callback)) {
        const DWORD error = GetLastError();
        return error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
    }
    return ERROR_SUCCESS;
}
