// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the crash handler's sources (crash_common.c and the OS halves,
// crash_win32.c and crash_posix.c) and by the platform tests. A report is
// written with what is here only: fixed buffers, no locks the process
// shares, no allocation, so that it can be written in a failing process (on
// Linux, inside a signal handler).

#include <nepheline/core/atomic.h>
#include <nepheline/platform/crash.h>

#include "platform_internal.h"

// The longest path a report or a dump takes: the directory, the prefix, and
// "-<yyyymmdd>-<hhmmss>-<pid>-<n>.txt".
#define CRASH_PATH_MAX   (NPH_CRASH_DIR_MAX + NPH_CRASH_APP_MAX + 64U)
#define CRASH_ASSERT_MAX 1024U // bytes of the failed assertion's text
#define CRASH_FRAMES_MAX 64U   // return addresses a report lists

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

// A buffered writer into an open file, a file descriptor on Linux and a
// HANDLE on Windows. A failed write is remembered and later output dropped.
typedef struct CrashOut {
    i64 file;
    u32 len;
    b32 failed;
    char buf[2048];
} CrashOut;

void crash_out_init(CrashOut *o, i64 file);
void crash_out_bytes(CrashOut *o, const char *s, u64 n);
void crash_out_str(CrashOut *o, const char *s);
void crash_out_dec(CrashOut *o, u64 v);
// "0x" and the hexadecimal digits of `v`, zero-padded to at least `digits`.
void crash_out_hex(CrashOut *o, u64 v, u32 digits);
void crash_out_flush(CrashOut *o);

// The OS half's: writes all `size` bytes to `file`. False when it could not.
bool crash_os_write(i64 file, const void *data, u64 size);

// The OS half's: the process's standard error, as a CrashOut file.
i64 crash_os_stderr(void);

// ---------------------------------------------------------------------------
// Time and names
// ---------------------------------------------------------------------------

typedef struct CrashTime {
    u32 year;
    u32 month; // 1 to 12
    u32 day;   // 1 to 31
    u32 hour;
    u32 minute;
    u32 second;
} CrashTime;

// The UTC calendar time of `seconds` since 1970-01-01 (Hinnant, "chrono-
// Compatible Low-Level Date Algorithms", civil_from_days). Seconds before
// 1970 give 1970-01-01 00:00:00.
CrashTime crash_time_from_unix(i64 seconds);

// Writes "<dir><app>-<yyyymmdd>-<hhmmss>-<pid>" to `out`, then "-<n>" when
// `n` is not 0, terminated. Returns its length, or 0 when it would not fit
// in `cap` bytes.
u32 crash_stem(char *out, u32 cap, i64 unix_seconds, u64 pid, u32 n);

// ---------------------------------------------------------------------------
// The handler's state
// ---------------------------------------------------------------------------

typedef struct CrashState {
    NphAtomicU32 installed;
    b32 full_dump;
    b32 drain_log;
    char dir[NPH_CRASH_DIR_MAX]; // ending in a separator
    char app[NPH_CRASH_APP_MAX];
    char notes[NPH_CRASH_NOTE_COUNT][NPH_CRASH_NOTE_MAX];
    char assertion[CRASH_ASSERT_MAX]; // the last failed assertion; "" before one
} CrashState;

extern CrashState g_crash;

// What a report says about the fault. The OS half fills it in.
typedef struct CrashFault {
    const char *what;   // "EXCEPTION_ACCESS_VIOLATION", "SIGSEGV", "abort"
    const char *detail; // more about it ("write", "SEGV_MAPERR"); NULL for none
    u64 code;           // the exception's code, or the signal's number
    u64 address;        // the address it was about, when `has_address`
    b32 has_address;
    u64 pc;        // the instruction it happened at
    u64 thread_id; // the OS's id of the thread
    const char *thread_name;
    i64 unix_seconds;
    u64 pid;
} CrashFault;

// Writes the start of a report: the title, the program, the time, the
// process, the thread, the fault, the failed assertion, and the notes.
void crash_write_header(CrashOut *o, const CrashFault *f);

// Writes the line every report ends with to the process's standard error:
// "<app>: <what>; report <path>".
void crash_write_stderr_line(const char *what, const char *report_path);

// Writes the log's last records nph_crash_log_sink kept, oldest first, after
// a "log:" line; nothing when it kept none.
void crash_write_log_tail(CrashOut *o);

// Delivers the log's pending records (nph_log_drain) and flushes the C
// library's streams, when the handler drains the log: a file sink leaves
// what is below an error buffered. Not async-signal-safe: callers guard it
// with a watchdog.
void crash_drain_log(void);

// ---------------------------------------------------------------------------
// The OS halves
// ---------------------------------------------------------------------------

// After the common checks: set the OS's hooks. Called with g_crash filled.
NphResult crash_os_install(void);
// Give the hooks back.
void crash_os_uninstall(void);
NphResult crash_os_thread_attach(void);
void crash_os_thread_detach(void);

#if NPH_PLATFORM_WINDOWS

#include <dbghelp.h> // after windows.h, which platform_internal.h brings

// Writes the report and the dump for `pointers` (a fault on thread
// `thread_id`; `what` names one that is not an exception, NULL otherwise),
// and the report's path to `out_path` when it is not NULL. Once the report
// and the dump are written it calls `between` (when not NULL), and then ends
// the report with the log's last records: the handler's thread tells the
// failing thread there that the report is written, and drains the log. The
// handler's thread calls it; tests call it with a captured context.
void crash_win32_write(EXCEPTION_POINTERS *pointers, u32 thread_id, const char *thread_name,
                       const char *what, void (*between)(void), char *out_path, u32 cap);

// The filter of the frame at the base of every fiber's stack (fiber.c,
// fiber_win32.c): what reaches it no handler took, so it is reported and the
// process ended, as the unhandled-exception filter does for a thread. Hands
// it on (EXCEPTION_CONTINUE_SEARCH) when no handler is installed or a
// debugger is attached, which then sees it as unhandled.
LONG crash_win32_fiber_filter(EXCEPTION_POINTERS *pointers);

// Tests: the next `count` attempts at a dump fail as dbghelp's sometimes do
// (ERROR_INVALID_USER_BUFFER), without writing; 0 ends it. With `stand_in`,
// the attempts after them write a stand-in (a minidump's signature alone)
// instead of calling dbghelp, so a test of the retries does not depend on
// how dbghelp's own attempts go on the machine (on CI's runners they fail
// in spells: ADR-0103). The retries' pauses are skipped while either holds.
void crash_win32_fail_dumps(u32 count, bool stand_in);

// The dump's callback (MINIDUMP_CALLBACK_ROUTINE): the global data of the
// modules beside the executable only (all of it in a full dump), and memory
// that cannot be read when the dump gets to it left out, which would
// otherwise fail the whole dump. Tests call it.
// NOLINTNEXTLINE(misc-misplaced-const): MINIDUMP_CALLBACK_ROUTINE's own signature
BOOL CALLBACK crash_win32_dump_callback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                        PMINIDUMP_CALLBACK_OUTPUT output);

#else

// Writes the report for signal `sig` (`info` and the handler's `ucontext`),
// and the report's path to `out_path` when it is not NULL. With `drain`, it
// drains the log under a watchdog once the report's fault and stack are
// written, before the log's last records end it. The signal handler calls
// it; tests call it with a context of their own.
void crash_posix_write(int sig, const void *info, const void *ucontext, bool drain, char *out_path,
                       u32 cap);

#endif

// The name the calling thread was given by nph_thread_set_current_name, ""
// when none (thread_win32.c; read where it is safe to).
const char *nph_thread_current_name_copy(void);
