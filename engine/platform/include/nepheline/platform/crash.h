// SPDX-License-Identifier: Apache-2.0
#pragma once

// The crash handler (CLAUDE.md §8.11 "Crash handler with minidumps (Windows)
// and backtraces (Linux)"; docs/adr/0103-crash-handler.md).
//
// nph_crash_install hooks the faults that end a process: access violations
// and the other processor exceptions, abort() (where a failed NPH_ASSERT
// ends), and on Windows the C runtime's invalid-parameter and pure-call
// failures. When one happens, it writes into the directory it was given:
//
// - `<app>-<yyyymmdd>-<hhmmss>-<pid>.txt`, the report: what failed and
//   where, the thread, the process's notes (nph_crash_set_note), the failed
//   assertion if one did, and the stack. On Windows the stack is its return
//   addresses as module + offset; on Linux a backtrace, with the process's
//   memory map for addr2line.
// - On Windows, `<same>.dmp`: a minidump, which Visual Studio or WinDbg opens
//   with the build's PDBs.
//
// The process then ends as the fault ends it: on Windows with the
// exception's code as its exit code and no error-reporting dialog, on Linux
// by the signal's default action (its exit status, and a core file where
// the system writes them). One report is written a process: a fault on a
// second thread while the first is reported waits for the process to end.
//
// A failing process may have broken what a report would use, so writing it
// takes no locks the process shares and allocates nothing. On Windows a
// thread made at install writes it, so a thread whose stack overflowed is
// reported too. On Linux the signal handler writes it with
// async-signal-safe calls, on an alternate signal stack that the installing
// thread and every thread nph_thread_create makes have
// (nph_crash_thread_attach for threads made elsewhere). After the report,
// and its dump, are written, the log's pending records are delivered
// (nph_log_drain), best effort: a watchdog ends the process if that hangs.
// Then the report ends with the log's last records, which
// nph_crash_log_sink keeps where a host adds it.
//
// Faults on fibers are reported as on threads, their own stack overflows
// included (fiber.c lays a fiber's stack out for it). Not reported: what ends
// a process without an exception reaching it: on Windows a fail-fast (/GS
// cookie checks, heap corruption that the heap detects), and on both systems
// a kill from outside.
//
// The OS hooks belong to the whole process, so there is one handler a
// process: state outside an NphEngine, as logging's sinks are (CLAUDE.md
// §2). It does not install under AddressSanitizer or ThreadSanitizer, which
// own the same signals and report more (NPH_RESULT_UNSUPPORTED).
//
// Thread-safety: install, uninstall, and nph_crash_thread_attach/detach are
// documented per function. A note may be set from any thread, but one being
// written while a report reads it can come out torn.

#include <nepheline/core/log.h>
#include <nepheline/core/result.h>
#include <nepheline/core/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_CRASH_NOTE_COUNT 8U   // notes a report carries
#define NPH_CRASH_NOTE_MAX   256U // bytes of a note, its terminator included
#define NPH_CRASH_DIR_MAX    768U // bytes of the directory's path, its terminator included
#define NPH_CRASH_APP_MAX    64U  // bytes of the files' prefix, its terminator included
#define NPH_CRASH_LOG_LINES  64U  // the log's last records a report ends with

// Bytes of the alternate signal stack each thread gets on Linux.
#define NPH_CRASH_ALT_STACK_SIZE 65536U // 64 KiB

typedef struct NphCrashDesc {
    // Where reports go; created, with its parents, at install. Required.
    const char *dir;
    // The files' prefix: letters, digits, '_', '-', and '.'. NULL: "nepheline".
    const char *app_name;
    // Windows: the dump holds the whole process's memory, instead of the
    // threads, their stacks, the loaded modules, and the memory the stacks
    // point at. Large: the editor's is gigabytes.
    b32 full_dump;
    // Do not deliver the log's pending records after the report.
    b32 no_log_drain;
} NphCrashDesc;

// Installs the handler. NPH_RESULT_ALREADY_EXISTS when one is installed,
// NPH_RESULT_INVALID_ARGUMENT for a missing or over-long directory or a
// prefix with other characters, NPH_RESULT_UNSUPPORTED under the sanitizers,
// and the file system's and the OS's errors (the directory not made, the
// hooks not set). On Linux the calling thread gets its alternate signal
// stack. It wraps the installed assert handler (core/assert.h), so that a
// failed assertion's text is in the report, and calls it on.
// Thread-safety: NPH_MAIN_THREAD_ONLY, before other threads that assert.
NPH_MUST_USE NphResult nph_crash_install(const NphCrashDesc *desc);

// Removes the handler: the hooks it set are given back, and the assert
// handler it wrapped is installed again. Nothing when none is installed.
// Thread-safety: NPH_MAIN_THREAD_ONLY, when no other thread can fault.
void nph_crash_uninstall(void);

// True between nph_crash_install and nph_crash_uninstall.
// Thread-safety: NPH_THREADSAFE.
bool nph_crash_is_installed(void);

// Sets note `slot` (below NPH_CRASH_NOTE_COUNT) to `text`, truncated to
// NPH_CRASH_NOTE_MAX - 1 bytes; NULL or "" clears it. Notes are kept whether
// a handler is installed or not, so a host may set them first. What the
// engine puts in them: its version and build, the scene being edited.
// Thread-safety: NPH_THREADSAFE (see the module's comment).
void nph_crash_set_note(u32 slot, const char *text);

// A log sink (core/log.h) that keeps the last NPH_CRASH_LOG_LINES records
// delivered to it, for a report to end with; `user` is unused. A host adds
// it with nph_log_add_sink(nph_crash_log_sink, NULL, <level>). It keeps
// records whether a handler is installed or not, in static memory.
// Thread-safety: as for every sink, called by the thread that flushes.
void nph_crash_log_sink(const NphLogRecord *record, void *user);

// Gives the calling thread what reporting its own faults needs: on Linux an
// alternate signal stack of NPH_CRASH_ALT_STACK_SIZE bytes, nothing on
// Windows. Threads nph_thread_create makes while a handler is installed
// attach themselves; a thread made elsewhere may call it. Idempotent; OK
// when no handler is installed (nothing is done). A thread that could not
// attach (the mapping refused) still has its faults reported, but not a
// stack overflow of its own.
// Thread-safety: for the calling thread.
NphResult nph_crash_thread_attach(void);

// Releases what nph_crash_thread_attach gave the calling thread. Call before
// the thread ends (nph_thread_create's threads do).
// Thread-safety: for the calling thread.
void nph_crash_thread_detach(void);

#ifdef __cplusplus
}
#endif
