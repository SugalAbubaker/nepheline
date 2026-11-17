// SPDX-License-Identifier: Apache-2.0
// The crash handler on Windows (docs/adr/0103-crash-handler.md).
//
// The unhandled-exception filter and the C runtime's hooks (abort through
// SIGABRT, invalid parameters, pure calls) hand the failure to a thread made
// at install, and wait. That thread writes the report and the minidump: it
// has a stack of its own, so a thread whose stack overflowed is reported
// too, and the dump shows the failing thread as it stopped. The failing
// thread then ends the process itself, with the exception's code, so that
// no other handler and no error-reporting dialog runs.
#include <nepheline/core/log.h>
#include <nepheline/platform/crash.h>
#include <nepheline/platform/thread.h>

#include "crash_internal.h"

#include <dbghelp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The codes a report gives the failures that are not exceptions: those the
// C runtime's own reporting gives them.
#define CRASH_STATUS_FATAL_APP_EXIT        0x40000015UL // abort(), a pure call
#define CRASH_STATUS_INVALID_CRT_PARAMETER 0xC0000417UL

// How long a failing thread waits for the report and the dump (a full dump
// of a large process takes long), then for the log to drain, before it ends
// the process regardless.
#define CRASH_WRITE_TIMEOUT_MS      120000U
#define CRASH_FULL_WRITE_TIMEOUT_MS 900000U
#define CRASH_DRAIN_TIMEOUT_MS      5000U

#define CRASH_THREAD_STACK ((SIZE_T)256U * 1024U)

typedef BOOL(WINAPI *CrashMiniDumpWriteDumpFn)(HANDLE process, DWORD pid, HANDLE file,
                                               MINIDUMP_TYPE type,
                                               PMINIDUMP_EXCEPTION_INFORMATION exception,
                                               PMINIDUMP_USER_STREAM_INFORMATION user,
                                               PMINIDUMP_CALLBACK_INFORMATION callback);

static HMODULE g_dbghelp;
static CrashMiniDumpWriteDumpFn g_write_dump;
static HANDLE g_thread; // the thread that writes
static DWORD g_thread_id;
static HANDLE g_request; // set by a failing thread
static HANDLE g_written; // set when the report and the dump are written
static HANDLE g_drained; // set when the log has drained too
static volatile LONG g_quit;
static volatile LONG g_crashing; // 0, or the id of the thread being reported

// The failure a failing thread hands over.
static EXCEPTION_POINTERS *g_req_pointers;
static DWORD g_req_thread;
static const char *g_req_what;
static char g_req_thread_name[NPH_THREAD_NAME_MAX];

// The executable's directory, with its separator: the modules there (the
// engine's, the game's) keep their global data in a small dump.
static wchar_t g_exe_dir[NPH_WIN32_PATH_MAX];
static u32 g_exe_dir_len;

// What install replaced, given back at uninstall.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static _invalid_parameter_handler g_prev_invalid;
static _purecall_handler g_prev_purecall;
static _crt_signal_t g_prev_sigabrt;
static unsigned int g_prev_abort_behavior;
static UINT g_prev_error_mode;

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

bool crash_os_write(i64 file, const void *data, u64 size)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr): a CrashOut keeps the HANDLE as an integer
    HANDLE h = (HANDLE)(iptr)file;
    const u8 *p = (const u8 *)data;
    while (size != 0U) {
        const DWORD chunk = size > 0x40000000U ? 0x40000000U : (DWORD)size;
        DWORD written = 0U;
        if (!WriteFile(h, p, chunk, &written, NULL) || written == 0U) {
            return false;
        }
        p += written;
        size -= written;
    }
    return true;
}

i64 crash_os_stderr(void)
{
    return (i64)(iptr)GetStdHandle(STD_ERROR_HANDLE);
}

typedef struct CodeName {
    DWORD code;
    const char *name;
} CodeName;

static const CodeName k_codes[] = {
    {EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION"},
    {EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"},
    {EXCEPTION_BREAKPOINT, "EXCEPTION_BREAKPOINT"},
    {EXCEPTION_DATATYPE_MISALIGNMENT, "EXCEPTION_DATATYPE_MISALIGNMENT"},
    {EXCEPTION_FLT_DENORMAL_OPERAND, "EXCEPTION_FLT_DENORMAL_OPERAND"},
    {EXCEPTION_FLT_DIVIDE_BY_ZERO, "EXCEPTION_FLT_DIVIDE_BY_ZERO"},
    {EXCEPTION_FLT_INEXACT_RESULT, "EXCEPTION_FLT_INEXACT_RESULT"},
    {EXCEPTION_FLT_INVALID_OPERATION, "EXCEPTION_FLT_INVALID_OPERATION"},
    {EXCEPTION_FLT_OVERFLOW, "EXCEPTION_FLT_OVERFLOW"},
    {EXCEPTION_FLT_STACK_CHECK, "EXCEPTION_FLT_STACK_CHECK"},
    {EXCEPTION_FLT_UNDERFLOW, "EXCEPTION_FLT_UNDERFLOW"},
    {EXCEPTION_GUARD_PAGE, "EXCEPTION_GUARD_PAGE"},
    {EXCEPTION_ILLEGAL_INSTRUCTION, "EXCEPTION_ILLEGAL_INSTRUCTION"},
    {EXCEPTION_IN_PAGE_ERROR, "EXCEPTION_IN_PAGE_ERROR"},
    {EXCEPTION_INT_DIVIDE_BY_ZERO, "EXCEPTION_INT_DIVIDE_BY_ZERO"},
    {EXCEPTION_INT_OVERFLOW, "EXCEPTION_INT_OVERFLOW"},
    {EXCEPTION_INVALID_DISPOSITION, "EXCEPTION_INVALID_DISPOSITION"},
    {EXCEPTION_INVALID_HANDLE, "EXCEPTION_INVALID_HANDLE"},
    {EXCEPTION_NONCONTINUABLE_EXCEPTION, "EXCEPTION_NONCONTINUABLE_EXCEPTION"},
    {EXCEPTION_PRIV_INSTRUCTION, "EXCEPTION_PRIV_INSTRUCTION"},
    {EXCEPTION_SINGLE_STEP, "EXCEPTION_SINGLE_STEP"},
    {EXCEPTION_STACK_OVERFLOW, "EXCEPTION_STACK_OVERFLOW"},
    {0xC0000374UL, "STATUS_HEAP_CORRUPTION"},
    {0xC0000409UL, "STATUS_STACK_BUFFER_OVERRUN"},
    {0xE06D7363UL, "a C++ exception"},
    {CRASH_STATUS_FATAL_APP_EXIT, "abort"},
    {CRASH_STATUS_INVALID_CRT_PARAMETER, "invalid parameter"},
};

static const char *code_name(DWORD code)
{
    for (u32 i = 0U; i < (u32)NPH_ARRAY_COUNT(k_codes); ++i) {
        if (k_codes[i].code == code) {
            return k_codes[i].name;
        }
    }
    return "an exception";
}

// "<module>+0x<offset>" for an address in a loaded module, else "?".
static void out_module(CrashOut *o, u64 address)
{
    HMODULE module = NULL;
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): FROM_ADDRESS takes the address as the name
    if (!GetModuleHandleExW(flags, (LPCWSTR)(uptr)address, &module) || module == NULL) {
        crash_out_str(o, "?");
        return;
    }
    wchar_t wide[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, wide, (DWORD)NPH_ARRAY_COUNT(wide));
    const wchar_t *base = wide;
    if (n != 0U && n < NPH_ARRAY_COUNT(wide)) {
        for (DWORD i = 0U; i < n; ++i) {
            if (wide[i] == L'\\' || wide[i] == L'/') {
                base = wide + i + 1U;
            }
        }
    } else {
        wide[0] = L'\0';
    }
    char name[MAX_PATH * 3U];
    u64 len = 0U;
    if (nph_win32_wide_to_utf8(base, name, sizeof name, &len) != NPH_RESULT_OK || len == 0U) {
        crash_out_str(o, "?");
    } else {
        crash_out_str(o, name);
    }
    crash_out_str(o, "+");
    crash_out_hex(o, address - (u64)(uptr)module, 1U);
}

#if NPH_ARCH_X64

// The return addresses of the stack `start` describes, by the unwind data
// of the modules' functions (RtlVirtualUnwind). A frame that cannot be read
// ends the walk: the failing thread's stack may be what failed.
static u32 walk_stack(const CONTEXT *start, u64 *frames, u32 cap)
{
    CONTEXT ctx = *start;
    u32 n = 0U;
#if NPH_COMPILER_CLANG
// Structured exception handling is Windows's, which clang-cl accepts as an extension.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif
    __try {
        while (n < cap && ctx.Rip != 0U) {
            const DWORD64 pc = ctx.Rip;
            frames[n++] = pc;
            DWORD64 image_base = 0U;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &image_base, NULL);
            if (fn == NULL) {
                // A leaf function: its return address is where the stack points.
                // NOLINTNEXTLINE(performance-no-int-to-ptr): the stack pointer, read
                ctx.Rip = *(const DWORD64 *)(uptr)ctx.Rsp;
                ctx.Rsp += 8U;
            } else {
                PVOID handler_data = NULL;
                DWORD64 establisher = 0U;
                (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, fn, &ctx, &handler_data,
                                       &establisher, NULL);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The walk ends at the frame that could not be read.
    }
#if NPH_COMPILER_CLANG
#pragma clang diagnostic pop
#endif
    return n;
}

#endif

// The global data (MiniDumpWithDataSegs) is kept for the modules beside the
// executable only: the driver's alone are tens of megabytes (the player's
// dump, with NVIDIA's, came to 40 MB) and say nothing the engine's do not.
// NOLINTNEXTLINE(misc-misplaced-const): MINIDUMP_CALLBACK_ROUTINE's own signature
BOOL CALLBACK crash_win32_dump_callback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                        PMINIDUMP_CALLBACK_OUTPUT output)
{
    NPH_UNUSED(param);
    if (input == NULL || output == NULL) {
        return FALSE;
    }
    switch (input->CallbackType) {
    case ModuleCallback: {
        const wchar_t *path = input->Module.FullPath;
        const bool ours = path != NULL && g_exe_dir_len != 0U &&
                          _wcsnicmp(path, g_exe_dir, (size_t)g_exe_dir_len) == 0 &&
                          wcschr(path + g_exe_dir_len, L'\\') == NULL;
        if (!ours && !g_crash.full_dump) {
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

// Opens `<stem><ext>` for writing, creating it; INVALID_HANDLE_VALUE when it
// cannot be.
static HANDLE create_file(const char *stem, const char *ext, char *path, u32 cap)
{
    const u64 stem_len = (u64)strlen(stem);
    const u64 ext_len = (u64)strlen(ext);
    if (stem_len + ext_len + 1U > cap) {
        return INVALID_HANDLE_VALUE;
    }
    memcpy(path, stem, (size_t)stem_len);
    memcpy(path + stem_len, ext, (size_t)ext_len + 1U);
    wchar_t wide[NPH_WIN32_PATH_MAX];
    if (nph_win32_utf8_to_wide(path, wide, (u32)NPH_ARRAY_COUNT(wide)) != NPH_RESULT_OK) {
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

void crash_win32_write(EXCEPTION_POINTERS *pointers, u32 thread_id, const char *thread_name,
                       const char *what, void (*between)(void), char *out_path, u32 cap)
{
    const EXCEPTION_RECORD *rec = pointers->ExceptionRecord;
    const CONTEXT *ctx = pointers->ContextRecord;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const u64 ticks = ((u64)ft.dwHighDateTime << 32U) | (u64)ft.dwLowDateTime;
    const i64 unix_seconds = (i64)(ticks / 10000000U) - 11644473600LL; // 1601 to 1970
    const u64 pid = (u64)GetCurrentProcessId();

    // The report's name: the first that no report of this process took yet.
    char stem[CRASH_PATH_MAX];
    char path[CRASH_PATH_MAX];
    HANDLE file = INVALID_HANDLE_VALUE;
    for (u32 n = 0U; n < 16U && file == INVALID_HANDLE_VALUE; ++n) {
        if (crash_stem(stem, sizeof stem, unix_seconds, pid, n) == 0U) {
            break;
        }
        file = create_file(stem, ".txt", path, sizeof path);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }
    if (out_path != NULL && cap != 0U) {
        out_path[0] = '\0';
    }
    if (file == INVALID_HANDLE_VALUE) {
        crash_write_stderr_line(what != NULL ? what : code_name(rec->ExceptionCode),
                                "not written: the directory cannot be written");
        if (between != NULL) {
            between();
        }
        return;
    }

    CrashFault f;
    memset(&f, 0, sizeof f);
    f.what = what != NULL ? what : code_name(rec->ExceptionCode);
    f.code = (u64)rec->ExceptionCode;
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2U) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        f.detail = kind == 0U ? "read" : kind == 1U ? "write" : kind == 8U ? "execute" : "access";
        f.address = (u64)rec->ExceptionInformation[1];
        f.has_address = 1;
    }
#if NPH_ARCH_X64
    f.pc = ctx->Rip;
#else
    f.pc = (u64)(uptr)rec->ExceptionAddress;
#endif
    f.thread_id = thread_id;
    f.thread_name = thread_name;
    f.unix_seconds = unix_seconds;
    f.pid = pid;

    CrashOut o;
    crash_out_init(&o, (i64)(iptr)file);
    crash_write_header(&o, &f);
    crash_out_flush(&o);

    // The dump first: it says the most, and what follows reads the modules'
    // lists, which a failure in the loader may have left locked.
    char dump_path[CRASH_PATH_MAX];
    HANDLE dump = create_file(stem, ".dmp", dump_path, sizeof dump_path);
    DWORD dump_error = ERROR_SUCCESS;
    if (dump == INVALID_HANDLE_VALUE) {
        dump_error = GetLastError();
    } else {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = thread_id;
        mei.ExceptionPointers = pointers;
        mei.ClientPointers = FALSE;
        // The flags combined, as dbghelp takes them, are no one enumerator.
        // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
        const MINIDUMP_TYPE type =
            g_crash.full_dump
                ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo |
                                  MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                  MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData)
                : (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                  MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData |
                                  MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                  MiniDumpWithProcessThreadData);
        // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
        MINIDUMP_CALLBACK_INFORMATION callback;
        callback.CallbackRoutine = crash_win32_dump_callback;
        callback.CallbackParam = NULL;
        if (g_write_dump == NULL || !g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                                                  type, &mei, NULL, &callback)) {
            dump_error = g_write_dump == NULL ? ERROR_PROC_NOT_FOUND : GetLastError();
        }
        CloseHandle(dump);
    }

    crash_out_str(&o, "module:    ");
    out_module(&o, f.pc);
    crash_out_str(&o, "\nstack:     return addresses, innermost first\n");
#if NPH_ARCH_X64
    u64 frames[CRASH_FRAMES_MAX];
    const u32 count = walk_stack(ctx, frames, CRASH_FRAMES_MAX);
    for (u32 i = 0U; i < count; ++i) {
        crash_out_str(&o, "  #");
        crash_out_dec(&o, i);
        crash_out_str(&o, i < 10U ? "  " : " ");
        crash_out_hex(&o, frames[i], 16U);
        crash_out_str(&o, "  ");
        out_module(&o, frames[i]);
        crash_out_str(&o, "\n");
    }
#endif
    crash_out_str(&o, "dump:      ");
    if (dump_error == ERROR_SUCCESS) {
        const char *name = dump_path;
        for (const char *p = dump_path; *p != '\0'; ++p) {
            if (*p == '/' || *p == '\\') {
                name = p + 1;
            }
        }
        crash_out_str(&o, name);
    } else {
        crash_out_str(&o, "not written (error ");
        crash_out_dec(&o, dump_error);
        crash_out_str(&o, ")");
    }
    crash_out_str(&o, "\n");
    crash_out_flush(&o);
    (void)FlushFileBuffers(file);
    crash_write_stderr_line(f.what, path);
    if (out_path != NULL && cap != 0U) {
        const u64 n = (u64)strlen(path);
        if (n < cap) {
            memcpy(out_path, path, (size_t)n + 1U);
        }
    }

    // What follows may never finish (the log's sinks take locks the failure
    // may hold): the report and the dump are on disk before it.
    if (between != NULL) {
        between();
    }
    crash_write_log_tail(&o);
    crash_out_flush(&o);
    (void)FlushFileBuffers(file);
    CloseHandle(file);
}

// ---------------------------------------------------------------------------
// The handler's thread, and the failing threads' side
// ---------------------------------------------------------------------------

// Between the report and its log tail: the failing thread may stop waiting
// for the report, and only waits a little longer for the log.
static void handler_between(void)
{
    (void)SetEvent(g_written);
    crash_drain_log();
}

static DWORD WINAPI handler_main(LPVOID unused)
{
    NPH_UNUSED(unused);
    for (;;) {
        (void)WaitForSingleObject(g_request, INFINITE);
        if (InterlockedCompareExchange(&g_quit, 0, 0) != 0) {
            return 0U;
        }
        crash_win32_write(g_req_pointers, g_req_thread, g_req_thread_name, g_req_what,
                          handler_between, NULL, 0U);
        (void)SetEvent(g_written);
        (void)SetEvent(g_drained);
    }
}

// The C runtime calls the SIGABRT handler below from abort() itself, on the
// thread that aborts, not asynchronously: the async-signal-safety the signal
// handler check asks for does not apply, and what it runs is what the
// unhandled-exception filter runs too.
// NOLINTBEGIN(bugprone-signal-handler,cert-sig30-c,cert-msc54-cpp)

NPH_NORETURN static void end_process(DWORD code)
{
    (void)TerminateProcess(GetCurrentProcess(), code);
    for (;;) {
        Sleep(INFINITE);
    }
}

// Hands `pointers` to the handler's thread and waits for the report. A
// second failure (another thread's, or this one's while it is reported)
// does not report again.
static void report_and_wait(EXCEPTION_POINTERS *pointers, const char *what)
{
    const DWORD self = GetCurrentThreadId();
    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    if (self == g_thread_id) {
        end_process(code); // the writing itself failed
    }
    const LONG prev = InterlockedCompareExchange(&g_crashing, (LONG)self, 0);
    if (prev != 0) {
        if ((DWORD)prev == self) {
            end_process(code); // failed again while being reported
        }
        for (;;) {
            Sleep(INFINITE); // the process ends once the first is reported
        }
    }
    g_req_pointers = pointers;
    g_req_thread = self;
    g_req_what = what;
    const char *name = nph_thread_current_name_copy();
    u32 i = 0U;
    for (; i + 1U < sizeof g_req_thread_name && name[i] != '\0'; ++i) {
        g_req_thread_name[i] = name[i];
    }
    g_req_thread_name[i] = '\0';
    (void)SetEvent(g_request);
    (void)WaitForSingleObject(g_written, g_crash.full_dump ? CRASH_FULL_WRITE_TIMEOUT_MS
                                                           : CRASH_WRITE_TIMEOUT_MS);
    (void)WaitForSingleObject(g_drained, CRASH_DRAIN_TIMEOUT_MS);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *pointers)
{
    report_and_wait(pointers, NULL);
    end_process(pointers->ExceptionRecord->ExceptionCode);
}

LONG crash_win32_fiber_filter(EXCEPTION_POINTERS *pointers)
{
    // Windows calls no unhandled-exception filter under a debugger either.
    if (!nph_crash_is_installed() || IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return crash_filter(pointers);
}

// A failure that is not an exception: reported with the context here, under
// the code the C runtime would give it.
static void report_here(DWORD code, const char *what)
{
    // Under a debugger, stop where it failed first, as an unreported
    // exception would.
    if (IsDebuggerPresent()) {
        DebugBreak();
    }
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = code;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if NPH_ARCH_X64
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the record holds the address as a pointer
    rec.ExceptionAddress = (PVOID)(uptr)ctx.Rip;
#endif
    EXCEPTION_POINTERS pointers = {&rec, &ctx};
    report_and_wait(&pointers, what);
}

static void __cdecl crash_sigabrt(int sig)
{
    NPH_UNUSED(sig);
    report_here(CRASH_STATUS_FATAL_APP_EXIT, "abort");
    _exit(3); // what abort() ends with
}

// NOLINTEND(bugprone-signal-handler,cert-sig30-c,cert-msc54-cpp)

static void __cdecl crash_invalid_parameter(const wchar_t *expression, const wchar_t *function,
                                            const wchar_t *file, unsigned int line,
                                            uintptr_t reserved)
{
    NPH_UNUSED(expression);
    NPH_UNUSED(function);
    NPH_UNUSED(file);
    NPH_UNUSED(line);
    NPH_UNUSED(reserved);
    report_here(CRASH_STATUS_INVALID_CRT_PARAMETER, "invalid parameter");
    end_process(CRASH_STATUS_INVALID_CRT_PARAMETER);
}

static void __cdecl crash_purecall(void)
{
    report_here(CRASH_STATUS_FATAL_APP_EXIT, "pure virtual call");
    _exit(3);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

static void close_handles(void)
{
    if (g_request != NULL) {
        CloseHandle(g_request);
    }
    if (g_written != NULL) {
        CloseHandle(g_written);
    }
    if (g_drained != NULL) {
        CloseHandle(g_drained);
    }
    if (g_dbghelp != NULL) {
        FreeLibrary(g_dbghelp);
    }
    g_request = NULL;
    g_written = NULL;
    g_drained = NULL;
    g_dbghelp = NULL;
    g_write_dump = NULL;
}

NphResult crash_os_install(void)
{
    // Loaded now, from the system's directory: loading a library while the
    // process fails could take the loader's lock a failing thread holds.
    g_dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_dbghelp == NULL) {
        return nph_win32_fail("LoadLibraryExW(dbghelp.dll)", GetLastError());
    }
    union {
        FARPROC proc;
        CrashMiniDumpWriteDumpFn fn;
    } u;
    u.proc = GetProcAddress(g_dbghelp, "MiniDumpWriteDump");
    g_write_dump = u.fn;
    if (g_write_dump == NULL) {
        const DWORD error = GetLastError();
        close_handles();
        return nph_win32_fail("GetProcAddress(MiniDumpWriteDump)", error);
    }
    // The executable's directory, for the dump's callback.
    const DWORD exe_len = GetModuleFileNameW(NULL, g_exe_dir, (DWORD)NPH_ARRAY_COUNT(g_exe_dir));
    g_exe_dir_len = 0U;
    if (exe_len != 0U && exe_len < NPH_ARRAY_COUNT(g_exe_dir)) {
        for (DWORD i = 0U; i < exe_len; ++i) {
            if (g_exe_dir[i] == L'\\' || g_exe_dir[i] == L'/') {
                g_exe_dir_len = (u32)i + 1U;
            }
        }
    }
    g_request = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_written = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_drained = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_request == NULL || g_written == NULL || g_drained == NULL) {
        const DWORD error = GetLastError();
        close_handles();
        return nph_win32_fail("CreateEventW", error);
    }
    InterlockedExchange(&g_quit, 0);
    InterlockedExchange(&g_crashing, 0);
    g_thread = CreateThread(NULL, CRASH_THREAD_STACK, handler_main, NULL,
                            STACK_SIZE_PARAM_IS_A_RESERVATION, &g_thread_id);
    if (g_thread == NULL) {
        const DWORD error = GetLastError();
        close_handles();
        return nph_win32_fail("CreateThread", error);
    }
    (void)SetThreadDescription(g_thread, L"nph-crash");

    g_prev_filter = SetUnhandledExceptionFilter(crash_filter);
    g_prev_invalid = _set_invalid_parameter_handler(crash_invalid_parameter);
    g_prev_purecall = _set_purecall_handler(crash_purecall);
    g_prev_sigabrt = signal(SIGABRT, crash_sigabrt);
    // abort() raises SIGABRT, which reports; without the reporting bits it
    // shows no dialog and calls no error reporting if a hook returns.
    g_prev_abort_behavior = _set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // No error-reporting dialog for what ends the process unreported (a
    // fail-fast): it would hang an unattended run.
    g_prev_error_mode = GetErrorMode();
    (void)SetErrorMode(g_prev_error_mode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    return NPH_RESULT_OK;
}

void crash_os_uninstall(void)
{
    (void)SetUnhandledExceptionFilter(g_prev_filter);
    (void)_set_invalid_parameter_handler(g_prev_invalid);
    (void)_set_purecall_handler(g_prev_purecall);
    (void)signal(SIGABRT, g_prev_sigabrt == SIG_ERR ? SIG_DFL : g_prev_sigabrt);
    (void)_set_abort_behavior(g_prev_abort_behavior, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)SetErrorMode(g_prev_error_mode);

    InterlockedExchange(&g_quit, 1);
    (void)SetEvent(g_request);
    (void)WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_thread_id = 0U;
    close_handles();
}

NphResult crash_os_thread_attach(void)
{
    return NPH_RESULT_OK; // the handler's thread reports every thread
}

void crash_os_thread_detach(void)
{
}
