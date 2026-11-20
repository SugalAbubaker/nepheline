// SPDX-License-Identifier: Apache-2.0
// The crash handler on Windows (docs/adr/0103-crash-handler.md,
// docs/adr/0106-crash-dumps-from-a-separate-process.md).
//
// The unhandled-exception filter and the C runtime's hooks (abort through
// SIGABRT, invalid parameters, pure calls) hand the failure to a thread made
// at install, and wait. That thread writes the report: it has a stack of its
// own, so a thread whose stack overflowed is reported too. The minidump is
// written from outside the process, by the dumper the handler started at
// install (nph_crash_dumper.exe, dumper/main.c), which the thread asks and
// waits for; the dump shows the failing thread as it stopped. Without the
// dumper, the thread writes the dump itself, as it did before ADR-0106. The
// failing thread then ends the process itself, with the exception's code, so
// that no other handler and no error-reporting dialog runs.
#include <nepheline/core/base.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/crash.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/vmem.h>

#include "crash_dump_win32.h"
#include "crash_internal.h"

#include <intrin.h> // __cpuid, __cpuidex: the system line's processor
#include <psapi.h>  // K32GetProcessMemoryInfo, in kernel32: the memory line
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

// How long install waits for the dumper to say it is ready (a process's start, which a virus
// scanner may slow), a dump waits for it (inside the failing thread's wait above, with time
// left for the rest of the report), and uninstall waits for it to end.
#define CRASH_DUMPER_START_TIMEOUT_MS 10000U
#define CRASH_DUMPER_TIMEOUT_MS       100000U
#define CRASH_DUMPER_FULL_TIMEOUT_MS  840000U
#define CRASH_DUMPER_STOP_TIMEOUT_MS  5000U

NPH_STATIC_ASSERT(CRASH_DUMPER_DIR_MAX == NPH_WIN32_PATH_MAX,
                  "the dumper's block holds the executable's directory as the handler does");

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

// The dumper (ADR-0106): its process, the block the two share, and the events a request and its
// answer set. While `g_dumper_ok` holds, dumps are asked of it; else this process writes them,
// and `g_in_process_why` says why, for the report and the install's log line.
static const wchar_t *g_dumper_name; // tests: crash_win32_set_dumper; NULL for CRASH_DUMPER_NAME
static HANDLE g_dumper;
static DWORD g_dumper_pid;
static HANDLE g_dumper_request;
static HANDLE g_dumper_done;
static CrashDumperShared *g_shared;
// CONTEXT_XSTATE's own bit (the flag holds the architecture's too), and the flags a context
// with extended state is laid out with: x64's; none elsewhere.
#if NPH_ARCH_X64
#define CRASH_XSTATE_BIT     ((DWORD)CONTEXT_XSTATE & ~(DWORD)CONTEXT_AMD64)
#define CRASH_CONTEXT_XSTATE ((DWORD)CONTEXT_XSTATE)
#else
#define CRASH_XSTATE_BIT     0U
#define CRASH_CONTEXT_XSTATE 0U
#endif
// The exception's context as InitializeContext lays one out on this processor (ADR-0106's
// amendment): CONTEXT_ALL, with CONTEXT_XSTATE where the system enables extended state, and the
// bytes the layout takes; 0 bytes while unknown, and the exception is then handed on as the
// failure left it. Found at install, or by the first fiber's guarantee (crash_internal.h).
static DWORD g_context_flags;
static DWORD g_context_bytes;
static LONG g_context_found;
// The attempts this process makes itself copy the exception here (the dumper's copy is in the
// block): the handler's thread makes them one at a time.
static EXCEPTION_RECORD g_local_record;
static u8 g_local_context[CRASH_DUMPER_CONTEXT_MAX];
static b32 g_dumper_ok;
static char g_in_process_why[192];

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

// A word of the stack, read in a call: clang-cl's __try covers the calls in it, not a load that
// faults inline (it has no asynchronous exceptions without -fasync-exceptions), and a fault in a
// stack that failed must end the walk, not the report. The call goes through a pointer the
// optimizer cannot see through, so no compiler drops it or its cover.
static NPH_NO_INLINE DWORD64 read_stack_word(DWORD64 at)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the stack pointer, read
    return *(const volatile DWORD64 *)(uptr)at;
}
static DWORD64 (*const volatile g_read_stack_word)(DWORD64) = read_stack_word;

// The return addresses of the stack `start` describes, by the unwind data
// of the modules' functions (RtlVirtualUnwind). A frame that cannot be read
// ends the walk: the failing thread's stack may be what failed.
u32 crash_win32_walk_stack(const CONTEXT *start, u64 *frames, u32 cap)
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
                ctx.Rip = g_read_stack_word(ctx.Rsp);
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

// Appends `s` to the text in `buf` (`cap` bytes) at `*at`, within its size; with no lock and no
// allocation, so a failing process may build a line with it.
static void text_add(char *buf, u32 cap, u32 *at, const char *s)
{
    while (*s != '\0' && *at + 1U < cap) {
        buf[(*at)++] = *s++;
    }
    buf[*at] = '\0';
}

static void text_add_dec(char *buf, u32 cap, u32 *at, u32 v)
{
    char digits[12];
    u32 n = 0U;
    do {
        digits[n++] = (char)('0' + (char)(v % 10U));
        v /= 10U;
    } while (v != 0U && n < (u32)sizeof digits);
    char out[13];
    for (u32 i = 0U; i < n; ++i) {
        out[i] = digits[n - 1U - i];
    }
    out[n] = '\0';
    text_add(buf, cap, at, out);
}

// What a report says of its machine (the "system:" line, ADR-0103): the system's version, the
// processor and whether it has shadow stacks (CET), dbghelp's version, and whether the process
// runs with user shadow stacks. Made at install, so the handler takes no lock for it. On some of
// CI's runners every dump a process wrote of itself failed (ERROR_INVALID_USER_BUFFER) where on
// others none did: this is what tells the runners apart.
static char g_system[256];

static void system_add(u32 *at, const char *s)
{
    text_add(g_system, (u32)sizeof g_system, at, s);
}

static void system_add_dec(u32 *at, u32 v)
{
    text_add_dec(g_system, (u32)sizeof g_system, at, v);
}

// The file version of the loaded module `m`, from its version resource's fixed part (the
// signature 0xFEEF04BD, then the structure's version, then the file's two words): false where it
// has none.
static bool module_version(HMODULE m, u32 out[4])
{
    // RT_VERSION is 16 and VS_VERSION_INFO 1 (winver.h).
    HRSRC res = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));
    if (res == NULL) {
        return false;
    }
    HGLOBAL data = LoadResource(m, res);
    const u8 *p = data != NULL ? (const u8 *)LockResource(data) : NULL;
    const DWORD size = SizeofResource(m, res);
    if (p == NULL || size < 16U) {
        return false;
    }
    for (DWORD i = 0U; i + 16U <= size; i += 4U) {
        u32 words[4];
        memcpy(words, p + i, sizeof words);
        if (words[0] == 0xFEEF04BDU) {
            out[0] = words[2] >> 16U;
            out[1] = words[2] & 0xFFFFU;
            out[2] = words[3] >> 16U;
            out[3] = words[3] & 0xFFFFU;
            return true;
        }
    }
    return false;
}

static void describe_system(void)
{
    u32 at = 0U;
    g_system[0] = '\0';
    // The version as the kernel has it (GetVersionEx answers what the manifest asks for).
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    union {
        FARPROC proc;
        RtlGetVersionFn fn;
    } u;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    u.proc = ntdll != NULL ? GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW os;
    memset(&os, 0, sizeof os);
    os.dwOSVersionInfoSize = (DWORD)sizeof os;
    system_add(&at, "Windows ");
    if (u.proc != NULL && u.fn(&os) == 0) {
        system_add_dec(&at, os.dwMajorVersion);
        system_add(&at, ".");
        system_add_dec(&at, os.dwMinorVersion);
        system_add(&at, ".");
        system_add_dec(&at, os.dwBuildNumber);
    } else {
        system_add(&at, "(version unknown)");
    }
    NphCpuInfo cpu;
    nph_cpu_detect(&cpu);
    system_add(&at, "; ");
    system_add(&at, cpu.brand[0] != '\0' ? cpu.brand : "(processor unknown)");
    // CPUID leaf 7, ECX bit 7: the processor's shadow stacks (CET_SS).
    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 0);
    bool cet = false;
    if (regs[0] >= 7) {
        __cpuidex(regs, 7, 0);
        cet = ((u32)regs[2] & (1U << 7U)) != 0U;
    }
    system_add(&at, cet ? ", shadow stacks" : ", no shadow stacks");
    system_add(&at, "; dbghelp ");
    u32 v[4];
    if (g_dbghelp != NULL && module_version(g_dbghelp, v)) {
        for (u32 i = 0U; i < 4U; ++i) {
            if (i != 0U) {
                system_add(&at, ".");
            }
            system_add_dec(&at, v[i]);
        }
    } else {
        system_add(&at, "(version unknown)");
    }
    // Whether this process runs with user shadow stacks (Windows 11 and Server 2022 on, where the
    // processor has them and the executable allows them).
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY ss;
    memset(&ss, 0, sizeof ss);
    system_add(&at, "; user shadow stacks ");
    if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &ss,
                                   sizeof ss)) {
        system_add(&at, ss.EnableUserShadowStack == 0U       ? "off"
                        : ss.EnableUserShadowStackStrictMode ? "on (strict)"
                                                             : "on");
    } else {
        system_add(&at, "(unknown, error ");
        system_add_dec(&at, (u32)GetLastError());
        system_add(&at, ")");
    }
    // The bytes an exception's context takes with the processor's extended state (ADR-0106's
    // amendment): about 12 KiB where it holds AMX's tiles, where dumps once failed.
    system_add(&at, "; extended context ");
    if (g_context_bytes != 0U) {
        system_add_dec(&at, (u32)g_context_bytes);
        system_add(&at, (g_context_flags & CRASH_XSTATE_BIT) != 0U
                            ? " bytes"
                            : " bytes, without extended state");
    } else {
        system_add(&at, "(layout unknown)");
    }
}

// The exception context's layout on this processor (g_context_flags, g_context_bytes): the
// extended state where the system enables any, and the bytes InitializeContext asks for it;
// without the state where its layout is refused. Once: what it finds never changes.
static void find_context_layout(void)
{
    if (InterlockedCompareExchange(&g_context_found, 0, 0) != 0) {
        return;
    }
    DWORD flags = 0U;
    DWORD bytes = 0U;
    const DWORD tries[2] = {(DWORD)CONTEXT_ALL | CRASH_CONTEXT_XSTATE, (DWORD)CONTEXT_ALL};
    const bool xstate = CRASH_CONTEXT_XSTATE != 0U && GetEnabledXStateFeatures() != 0U;
    for (u32 i = xstate ? 0U : 1U; i < 2U && bytes == 0U; ++i) {
        DWORD length = 0U;
        if (!InitializeContext(NULL, tries[i], NULL, &length) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER && length != 0U &&
            length <= CRASH_DUMPER_CONTEXT_MAX) {
            flags = tries[i];
            bytes = length;
        }
    }
    g_context_flags = flags;
    g_context_bytes = bytes;
    InterlockedExchange(&g_context_found, 1);
}

bool crash_win32_exception_copy(void *area, u32 cap, const EXCEPTION_POINTERS *source,
                                EXCEPTION_RECORD *record, EXCEPTION_POINTERS *out)
{
    if (g_context_bytes == 0U || cap < g_context_bytes || source == NULL ||
        source->ExceptionRecord == NULL || source->ContextRecord == NULL) {
        return false;
    }
    DWORD length = (DWORD)cap;
    PCONTEXT ctx = NULL;
    if (!InitializeContext(area, g_context_flags, &ctx, &length) || ctx == NULL) {
        return false;
    }
    // The parts both hold (InitializeContext may drop flags the system lacks), and the copy's
    // flags say which: a flag may be taken from a context's, never added.
    DWORD flags = source->ContextRecord->ContextFlags & ctx->ContextFlags;
    if (!CopyContext(ctx, flags, source->ContextRecord)) {
        // Its base alone, the layout's extended part left as the layout made it.
        memcpy(ctx, source->ContextRecord, sizeof(CONTEXT));
        flags &= ~CRASH_XSTATE_BIT;
    }
    ctx->ContextFlags = flags;
    *record = *source->ExceptionRecord;
    record->ExceptionRecord = NULL; // the chain is the failing process's
    if (record->NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS) {
        record->NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS;
    }
    out->ExceptionRecord = record;
    out->ContextRecord = ctx;
    return true;
}

u32 crash_win32_context_bytes(void)
{
    find_context_layout();
    return (u32)g_context_bytes;
}

u32 crash_win32_stack_guarantee(void)
{
    find_context_layout();
    // A layout not known stands in at the largest the block takes.
    const u64 frame =
        g_context_bytes != 0U ? (u64)g_context_bytes : (u64)(CRASH_DUMPER_CONTEXT_MAX);
    return (u32)nph_align_up_u64(frame + (u64)(CRASH_OVERFLOW_HANDLER_BYTES), nph_vmem_page_size());
}

void crash_win32_guarantee_stack(void)
{
    ULONG bytes = (ULONG)crash_win32_stack_guarantee();
    // It only grows; one more than the stack reserves is refused, and the stack keeps its own.
    (void)SetThreadStackGuarantee(&bytes);
}

// The report's "memory:" line: the machine's physical memory and commit charge, free of their
// totals, and the process's working set and private commit, as the fault found them (ADR-0103):
// on some of CI's runners every dump fails, perhaps for want of them. Kernel queries alone.
static void out_memory(CrashOut *o)
{
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof ms);
    ms.dwLength = (DWORD)sizeof ms;
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = (DWORD)sizeof pmc;
    const u64 mb = (u64)1024U * 1024U;
    crash_out_str(o, "memory:    ");
    if (GlobalMemoryStatusEx(&ms)) {
        crash_out_dec(o, ms.ullAvailPhys / mb);
        crash_out_str(o, " of ");
        crash_out_dec(o, ms.ullTotalPhys / mb);
        crash_out_str(o, " MB physical free, ");
        crash_out_dec(o, ms.ullAvailPageFile / mb);
        crash_out_str(o, " of ");
        crash_out_dec(o, ms.ullTotalPageFile / mb);
        crash_out_str(o, " MB commit free");
    } else {
        crash_out_str(o, "(the machine's unknown)");
    }
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, (DWORD)sizeof pmc)) {
        crash_out_str(o, "; the process ");
        crash_out_dec(o, (u64)pmc.WorkingSetSize / mb);
        crash_out_str(o, " MB working set, ");
        crash_out_dec(o, (u64)pmc.PagefileUsage / mb);
        crash_out_str(o, " MB committed");
    }
    crash_out_str(o, "\n");
}

const char *crash_os_system(void)
{
    return g_system;
}

// The dumps tests make fail, and whether the attempts after them are stand-ins
// (crash_win32_fail_dumps).
static volatile LONG g_dump_failures;
static volatile LONG g_dump_stand_in;

void crash_win32_fail_dumps(u32 count, bool stand_in)
{
    (void)InterlockedExchange(&g_dump_failures, (LONG)count);
    (void)InterlockedExchange(&g_dump_stand_in, stand_in ? 1 : 0);
}

// Whether the tests decide the dumps' attempts (crash_win32_fail_dumps).
static bool dumps_staged(void)
{
    return InterlockedCompareExchange(&g_dump_failures, 0, 0) > 0 ||
           InterlockedCompareExchange(&g_dump_stand_in, 0, 0) != 0;
}

// How the next attempt at a dump goes: as the tests decide (crash_win32_fail_dumps), else
// dbghelp's.
static CrashDumpStage next_stage(void)
{
    if (InterlockedCompareExchange(&g_dump_failures, 0, 0) > 0) {
        (void)InterlockedDecrement(&g_dump_failures);
        return CRASH_DUMP_STAGE_FAIL;
    }
    return InterlockedCompareExchange(&g_dump_stand_in, 0, 0) != 0 ? CRASH_DUMP_STAGE_STAND_IN
                                                                   : CRASH_DUMP_STAGE_DBGHELP;
}

// The dumper can no longer be asked: `what`, `code`, and `after` say why, in the reports from
// now on.
static void dumper_lost(const char *what, DWORD code, const char *after)
{
    u32 at = 0U;
    g_in_process_why[0] = '\0';
    text_add(g_in_process_why, (u32)sizeof g_in_process_why, &at, what);
    text_add_dec(g_in_process_why, (u32)sizeof g_in_process_why, &at, (u32)code);
    text_add(g_in_process_why, (u32)sizeof g_in_process_why, &at, after);
    g_dumper_ok = 0;
}

// One attempt at the dump, asked of the dumper: what it answered, or, with `*lost`, nothing,
// since the dumper could not be asked or did not answer (and then never is asked again).
static DWORD dumper_attempt(HANDLE file, MINIDUMP_TYPE type,
                            const MINIDUMP_EXCEPTION_INFORMATION *mei, CrashDumpStage stage,
                            bool *lost)
{
    *lost = true;
    DWORD code = 0U;
    if (WaitForSingleObject(g_dumper, 0U) != WAIT_TIMEOUT) {
        (void)GetExitCodeProcess(g_dumper, &code);
        dumper_lost("the dumper had ended, exit ", code, "");
        return ERROR_SUCCESS;
    }
    // The file, as a handle of the dumper's own: the two share the file's position, which the
    // attempt sets to its start.
    HANDLE remote = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), file, g_dumper, &remote, 0U, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        dumper_lost("the file could not be handed to the dumper, error ", GetLastError(), "");
        return ERROR_SUCCESS;
    }
    g_shared->thread_id = mei->ThreadId;
    g_shared->type = (u32)type;
    g_shared->stage = (u32)stage;
    g_shared->pointers = (u64)(uptr)mei->ExceptionPointers;
    g_shared->file = (u64)(uptr)remote;
    // The exception, copied into the block for the dumper to read as its own (ADR-0106's
    // amendment); the address above stays for when it cannot be.
    EXCEPTION_POINTERS copied;
    g_shared->copied = 0U;
    if (mei->ExceptionPointers != NULL &&
        crash_win32_exception_copy((u8 *)g_shared + CRASH_DUMPER_CONTEXT_AT,
                                   CRASH_DUMPER_CONTEXT_MAX, mei->ExceptionPointers,
                                   &g_shared->record, &copied)) {
        g_shared->context_offset = (u32)((uptr)copied.ContextRecord - (uptr)g_shared);
        g_shared->context_length = (u32)g_context_bytes;
        g_shared->copied = 1U;
    }
    (void)SetEvent(g_dumper_request);
    const HANDLE waits[2] = {g_dumper_done, g_dumper};
    const DWORD w = WaitForMultipleObjects(2U, waits, FALSE,
                                           g_crash.full_dump ? CRASH_DUMPER_FULL_TIMEOUT_MS
                                                             : CRASH_DUMPER_TIMEOUT_MS);
    if (w == WAIT_OBJECT_0) {
        *lost = false;
        return (DWORD)g_shared->error;
    }
    if (w == WAIT_OBJECT_0 + 1U) {
        (void)GetExitCodeProcess(g_dumper, &code);
        dumper_lost("the dumper ended as it wrote, exit ", code, "");
        return ERROR_SUCCESS;
    }
    // No answer in time: ended, so that it writes no more into the file, which this process
    // writes instead.
    (void)TerminateProcess(g_dumper, ERROR_TIMEOUT);
    (void)WaitForSingleObject(g_dumper, CRASH_DUMPER_STOP_TIMEOUT_MS);
    dumper_lost(
        "the dumper did not answer in ",
        (g_crash.full_dump ? CRASH_DUMPER_FULL_TIMEOUT_MS : CRASH_DUMPER_TIMEOUT_MS) / 1000U, " s");
    return ERROR_SUCCESS;
}

// One attempt at the dump into `file`, from its start: ERROR_SUCCESS, or why not. The dumper
// makes it while it can be asked; else this process does, and `*in_process` says so.
static DWORD attempt(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION *mei,
                     bool *in_process)
{
    const CrashDumpStage stage = next_stage();
    if (g_dumper_ok) {
        bool lost = false;
        const DWORD error = dumper_attempt(file, type, mei, stage, &lost);
        if (!lost) {
            *in_process = false;
            return error;
        }
    }
    *in_process = true;
    const CrashDumpScope scope = {g_exe_dir, g_exe_dir_len, g_crash.full_dump};
    // A copy of the exception here too, as the dumper reads it (ADR-0106's amendment).
    MINIDUMP_EXCEPTION_INFORMATION local = *mei;
    EXCEPTION_POINTERS copied;
    if (mei->ExceptionPointers != NULL &&
        crash_win32_exception_copy(g_local_context, (u32)sizeof g_local_context,
                                   mei->ExceptionPointers, &g_local_record, &copied)) {
        local.ExceptionPointers = &copied;
    }
    return crash_win32_dump_attempt(g_write_dump, GetCurrentProcess(), GetCurrentProcessId(), file,
                                    type, &local, &scope, stage, CRASH_DUMP_BY_PROCESS);
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
    crash_out_str(&o, "system:    ");
    crash_out_str(&o, g_system);
    crash_out_str(&o, "\n");
    out_memory(&o);
    crash_out_flush(&o);

    // The dump first: it says the most, and what follows reads the modules'
    // lists, which a failure in the loader may have left locked.
    char dump_path[CRASH_PATH_MAX];
    HANDLE dump = create_file(stem, ".dmp", dump_path, sizeof dump_path);
    DWORD dump_error = ERROR_SUCCESS;
    DWORD first_error = ERROR_SUCCESS; // the first attempt's, when it failed
    bool reduced = false;
    bool in_process = false; // the last attempt was this process's, not the dumper's
    if (dump == INVALID_HANDLE_VALUE) {
        dump_error = GetLastError();
    } else {
        // The exception as this process sees it; the dumper reads the same pointers from here.
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = thread_id;
        mei.ExceptionPointers = pointers;
        mei.ClientPointers = FALSE;
        const MINIDUMP_TYPE type = crash_win32_dump_type(g_crash.full_dump);
        // A dump that fails is written once more as it was a tenth of a second later, then
        // reduced to the threads, their stacks, and the modules with their data a quarter second
        // after that. dbghelp failed so when processes dumped themselves
        // (ERROR_INVALID_USER_BUFFER, in spells: on one of CI's runners every attempt for a quarter
        // second failed), which the dumper is for (ADR-0103, ADR-0106); the attempts stay for
        // whatever fails it.
        const bool staged = dumps_staged();
        dump_error = attempt(dump, type, &mei, &in_process);
        first_error = dump_error;
        if (dump_error != ERROR_SUCCESS) {
            Sleep(staged ? 0U : 100U);
            dump_error = attempt(dump, type, &mei, &in_process);
        }
        if (dump_error != ERROR_SUCCESS) {
            reduced = true;
            Sleep(staged ? 0U : 250U);
            dump_error = attempt(dump, MiniDumpWithDataSegs, &mei, &in_process);
        }
        CloseHandle(dump);
    }

    crash_out_str(&o, "module:    ");
    out_module(&o, f.pc);
    crash_out_str(&o, "\nstack:     return addresses, innermost first\n");
#if NPH_ARCH_X64
    u64 frames[CRASH_FRAMES_MAX];
    const u32 count = crash_win32_walk_stack(ctx, frames, CRASH_FRAMES_MAX);
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
        if (first_error != ERROR_SUCCESS) {
            crash_out_str(&o, reduced ? " (reduced to threads, stacks and modules after error "
                                      : " (written again after error ");
            crash_out_dec(&o, first_error);
            crash_out_str(&o, ")");
        }
    } else {
        crash_out_str(&o, "not written (error ");
        crash_out_dec(&o, dump_error);
        crash_out_str(&o, ")");
    }
    if (in_process) {
        crash_out_str(&o, ", by the failing process (");
        crash_out_str(&o, g_in_process_why[0] != '\0' ? g_in_process_why : "no dumper");
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
// The dumper
// ---------------------------------------------------------------------------

void crash_win32_set_dumper(const wchar_t *name)
{
    g_dumper_name = name;
}

u32 crash_win32_dumper_pid(void)
{
    return g_dumper_ok ? (u32)g_dumper_pid : 0U;
}

// The dumper's name, for the install's log line and the reasons.
static char g_dumper_name_utf8[96];

// Appends `s` to the wide text in `buf` (`cap` units) at `*at`, within its size.
static void wide_add(wchar_t *buf, u32 cap, u32 *at, const wchar_t *s)
{
    while (*s != L'\0' && *at + 1U < cap) {
        buf[(*at)++] = *s++;
    }
    buf[*at] = L'\0';
}

// Appends `v` in hexadecimal digits, as the dumper reads a handle's value.
static void wide_add_hex(wchar_t *buf, u32 cap, u32 *at, u64 v)
{
    wchar_t digits[17];
    u32 n = 0U;
    do {
        const u32 d = (u32)(v & 0xFU);
        digits[n++] = (wchar_t)(d < 10U ? L'0' + d : L'A' + (d - 10U));
        v >>= 4U;
    } while (v != 0U && n < 16U);
    wchar_t out[17];
    for (u32 i = 0U; i < n; ++i) {
        out[i] = digits[n - 1U - i];
    }
    out[n] = L'\0';
    wide_add(buf, cap, at, out);
}

// Ends the dumper, asked to and else made to, and gives back the block and the events. The
// reason dumps are written in process is left as it was.
static void dumper_stop(void)
{
    if (g_dumper != NULL) {
        if (g_shared != NULL) {
            g_shared->quit = 1U;
        }
        if (g_dumper_request != NULL) {
            (void)SetEvent(g_dumper_request);
        }
        if (WaitForSingleObject(g_dumper, CRASH_DUMPER_STOP_TIMEOUT_MS) != WAIT_OBJECT_0) {
            (void)TerminateProcess(g_dumper, 0U);
            (void)WaitForSingleObject(g_dumper, CRASH_DUMPER_STOP_TIMEOUT_MS);
        }
        CloseHandle(g_dumper);
    }
    if (g_dumper_request != NULL) {
        CloseHandle(g_dumper_request);
    }
    if (g_dumper_done != NULL) {
        CloseHandle(g_dumper_done);
    }
    if (g_shared != NULL) {
        (void)UnmapViewOfFile(g_shared);
    }
    g_dumper = NULL;
    g_dumper_pid = 0U;
    g_dumper_request = NULL;
    g_dumper_done = NULL;
    g_shared = NULL;
    g_dumper_ok = 0;
}

// Starts the dumper beside the executable, handing it the block and the events, and waits for
// it to say it is ready: true then. Else false, with g_in_process_why saying why, and the dumps
// are written in process.
static bool dumper_start(void)
{
    g_dumper_ok = 0;
    g_in_process_why[0] = '\0';
    const wchar_t *name = g_dumper_name != NULL ? g_dumper_name : CRASH_DUMPER_NAME;
    if (nph_win32_wide_to_utf8(name, g_dumper_name_utf8, sizeof g_dumper_name_utf8, NULL) !=
        NPH_RESULT_OK) {
        memcpy(g_dumper_name_utf8, "the dumper", sizeof "the dumper");
    }
    u32 at = 0U;
    wchar_t path[NPH_WIN32_PATH_MAX];
    const u64 name_len = (u64)wcslen(name);
    if (g_exe_dir_len == 0U || (u64)g_exe_dir_len + name_len + 1U > NPH_ARRAY_COUNT(path)) {
        text_add(g_in_process_why, (u32)sizeof g_in_process_why, &at,
                 "the executable's directory is unknown");
        return false;
    }
    memcpy(path, g_exe_dir, (size_t)g_exe_dir_len * sizeof(wchar_t));
    memcpy(path + g_exe_dir_len, name, ((size_t)name_len + 1U) * sizeof(wchar_t));
    // Its working directory: the executable's (g_exe_dir holds the executable's whole path).
    wchar_t dir[NPH_WIN32_PATH_MAX];
    memcpy(dir, g_exe_dir, (size_t)g_exe_dir_len * sizeof(wchar_t));
    dir[g_exe_dir_len] = L'\0';
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        text_add(g_in_process_why, (u32)sizeof g_in_process_why, &at, g_dumper_name_utf8);
        text_add(g_in_process_why, (u32)sizeof g_in_process_why, &at,
                 " is not beside the executable");
        return false;
    }

    // The block, the events, and a handle to this process with what dbghelp needs of it: all
    // inheritable, for the dumper to inherit, and only it (below).
    SECURITY_ATTRIBUTES inherit;
    memset(&inherit, 0, sizeof inherit);
    inherit.nLength = (DWORD)sizeof inherit;
    inherit.bInheritHandle = TRUE;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0U,
                                        CRASH_DUMPER_BLOCK_BYTES, NULL);
    if (mapping != NULL) {
        g_shared = (CrashDumperShared *)MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0U,
                                                      0U, CRASH_DUMPER_BLOCK_BYTES);
    }
    g_dumper_request = CreateEventW(&inherit, FALSE, FALSE, NULL);
    g_dumper_done = CreateEventW(&inherit, FALSE, FALSE, NULL);
    HANDLE self = NULL;
    const DWORD rights =
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE;
    if (mapping == NULL || g_shared == NULL || g_dumper_request == NULL || g_dumper_done == NULL ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                         rights, TRUE, 0U)) {
        const DWORD error = GetLastError();
        if (mapping != NULL) {
            CloseHandle(mapping);
        }
        dumper_stop();
        dumper_lost("the dumper's handles were not made, error ", error, "");
        return false;
    }
    memset(g_shared, 0, sizeof *g_shared);
    g_shared->magic = CRASH_DUMPER_MAGIC;
    g_shared->version = CRASH_DUMPER_VERSION;
    g_shared->full_dump = g_crash.full_dump ? 1U : 0U;
    g_shared->exe_dir_len = g_exe_dir_len;
    memcpy(g_shared->exe_dir, g_exe_dir, (size_t)g_exe_dir_len * sizeof(wchar_t));

    // Its command line: its path, and the four handles' values.
    HANDLE inherited[4] = {self, mapping, g_dumper_request, g_dumper_done};
    wchar_t cmd[NPH_WIN32_PATH_MAX + 96U];
    u32 cmd_at = 0U;
    wide_add(cmd, (u32)NPH_ARRAY_COUNT(cmd), &cmd_at, L"\"");
    wide_add(cmd, (u32)NPH_ARRAY_COUNT(cmd), &cmd_at, path);
    wide_add(cmd, (u32)NPH_ARRAY_COUNT(cmd), &cmd_at, L"\"");
    for (u32 i = 0U; i < (u32)NPH_ARRAY_COUNT(inherited); ++i) {
        wide_add(cmd, (u32)NPH_ARRAY_COUNT(cmd), &cmd_at, L" ");
        wide_add_hex(cmd, (u32)NPH_ARRAY_COUNT(cmd), &cmd_at, (u64)(uptr)inherited[i]);
    }

    // It inherits these four handles alone (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): not this
    // process's standard handles or other inheritable ones, such as a test runner's pipes, whose
    // reader would otherwise wait for the dumper's end as well. No console, no busy cursor.
    u64 list_storage[16]; // the list's opaque storage: 48 bytes for one attribute on x64
    SIZE_T list_size = 0U;
    (void)InitializeProcThreadAttributeList(NULL, 1U, 0U, &list_size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = (LPPROC_THREAD_ATTRIBUTE_LIST)list_storage;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    DWORD error = ERROR_INSUFFICIENT_BUFFER;
    bool started = false;
    if (list_size <= sizeof list_storage &&
        InitializeProcThreadAttributeList(list, 1U, 0U, &list_size)) {
        if (UpdateProcThreadAttribute(list, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, (PVOID)inherited,
                                      sizeof inherited, NULL, NULL)) {
            STARTUPINFOEXW si;
            memset(&si, 0, sizeof si);
            si.StartupInfo.cb = (DWORD)sizeof si;
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
            si.lpAttributeList = list;
            started = CreateProcessW(path, cmd, NULL, NULL, TRUE,
                                     EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS, NULL, dir,
                                     &si.StartupInfo, &pi) != FALSE;
        }
        error = started ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(list);
    }
    // What this process starts later inherits none of them: the dumper has its own.
    (void)SetHandleInformation(g_dumper_request, HANDLE_FLAG_INHERIT, 0U);
    (void)SetHandleInformation(g_dumper_done, HANDLE_FLAG_INHERIT, 0U);
    CloseHandle(self);
    CloseHandle(mapping); // the view keeps the block
    if (!started) {
        dumper_stop();
        dumper_lost("the dumper did not start, error ", error, "");
        return false;
    }
    CloseHandle(pi.hThread);
    g_dumper = pi.hProcess;
    g_dumper_pid = pi.dwProcessId;

    const HANDLE waits[2] = {g_dumper_done, g_dumper};
    const DWORD w = WaitForMultipleObjects(2U, waits, FALSE, CRASH_DUMPER_START_TIMEOUT_MS);
    if (w == WAIT_OBJECT_0) {
        g_dumper_ok = 1;
        return true;
    }
    DWORD code = 0U;
    if (w == WAIT_OBJECT_0 + 1U) {
        (void)GetExitCodeProcess(g_dumper, &code);
        dumper_stop();
        dumper_lost("the dumper ended as it started, exit ", code, "");
    } else {
        dumper_stop(); // which ends it
        dumper_lost("the dumper was not ready in ", CRASH_DUMPER_START_TIMEOUT_MS / 1000U, " s");
    }
    return false;
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
    // dbghelp for the dumps this process writes itself, when there is no dumper, and for the
    // system line's version: loaded now, from the system's directory.
    const DWORD loaded = crash_win32_dbghelp_load(&g_dbghelp, &g_write_dump);
    if (loaded != ERROR_SUCCESS) {
        return nph_win32_fail("loading dbghelp.dll's MiniDumpWriteDump", loaded);
    }
    // The exception context's layout, for the copies and the system line, and room on the
    // installing thread's stack to report an overflow from (ADR-0106's amendment).
    find_context_layout();
    crash_win32_guarantee_stack();
    describe_system();
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

    // The dumps' writer, ready before the hooks are set (ADR-0106). Without it the handler still
    // installs, and this process writes them.
    if (dumper_start()) {
        NPH_LOG_INFO(&NPH_LOG_PLATFORM, "crash handler: dumps are written by %s, process %lu",
                     g_dumper_name_utf8, (unsigned long)g_dumper_pid);
    } else {
        NPH_LOG_WARN(&NPH_LOG_PLATFORM,
                     "crash handler: dumps are written by the failing process: %s",
                     g_in_process_why);
    }

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
    dumper_stop();
    close_handles();
}

NphResult crash_os_thread_attach(void)
{
    return NPH_RESULT_OK; // the handler's thread reports every thread
}

void crash_os_thread_detach(void)
{
}
