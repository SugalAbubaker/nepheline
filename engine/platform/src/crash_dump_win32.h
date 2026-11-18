// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the crash handler (crash_win32.c) and its dumper (dumper/main.c, nph_crash_dumper.exe)
// share on Windows (docs/adr/0106-crash-dumps-from-a-separate-process.md): how one attempt at a
// minidump goes, and the block of memory through which the handler asks for a dump and the
// dumper answers. Both compile crash_dump_win32.c. The dumper links no engine library, so nothing
// here calls into one, and the header includes core's types alone.

#include <nepheline/core/types.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows 10, as platform_internal.h asks, for the dumper, which includes this header first.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
// dbghelp.h needs windows.h's types first; sorted, it would come before it.
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

// The dumper's file, beside the executable that installs the handler: every executable the build
// makes shares bin/<config>, and a game ships the dumper beside its own.
#define CRASH_DUMPER_NAME L"nph_crash_dumper.exe"

#define CRASH_DUMPER_MAGIC   0x4448504EU // "NPHD"
#define CRASH_DUMPER_VERSION 1U
// UTF-16 units of the executable's directory the block holds: platform_internal.h's
// NPH_WIN32_PATH_MAX, which crash_win32.c checks it against.
#define CRASH_DUMPER_DIR_MAX 4096U

// The dumper's exit codes, besides 0 (its process ended, or the handler let it go).
#define CRASH_DUMPER_EXIT_USAGE   2 // the command line does not name four handles
#define CRASH_DUMPER_EXIT_BLOCK   3 // the mapping is not the handler's block
#define CRASH_DUMPER_EXIT_DBGHELP 4 // dbghelp or its MiniDumpWriteDump did not load

// What the dumps' comment streams say of their writer.
#define CRASH_DUMP_BY_DUMPER  "written by nph_crash_dumper"
#define CRASH_DUMP_BY_PROCESS "written by the failing process"

// How one attempt at a dump goes: dbghelp's, or, for tests (crash_win32_fail_dumps), a failure
// as dbghelp's sometimes are, or a stand-in that writes a minidump's signature alone.
typedef enum CrashDumpStage {
    CRASH_DUMP_STAGE_DBGHELP = 0,
    CRASH_DUMP_STAGE_FAIL = 1,
    CRASH_DUMP_STAGE_STAND_IN = 2,
} CrashDumpStage;

// The block both processes map: a mapping of the page file that the handler makes at install and
// the dumper inherits. The handler writes a request and then sets the request event; the dumper
// writes the answer and then sets the done event. The events order the writes across the two
// processes, so the fields need nothing more.
typedef struct CrashDumperShared {
    u32 magic;   // CRASH_DUMPER_MAGIC
    u32 version; // CRASH_DUMPER_VERSION
    // Written at install.
    u32 full_dump;   // the whole process's memory (NphCrashDesc.full_dump)
    u32 exe_dir_len; // UTF-16 units, the separator included; 0 when unknown
    wchar_t exe_dir[CRASH_DUMPER_DIR_MAX];
    // A request.
    u32 quit;      // 1: the handler is uninstalled, and the dumper ends
    u32 thread_id; // the failing thread
    u32 type;      // the MINIDUMP_TYPE
    u32 stage;     // a CrashDumpStage
    u64 pointers;  // its EXCEPTION_POINTERS: an address in the handler's process, 0 for none
    u64 file;      // the dump's file: a handle the handler duplicated into the dumper
    // The answer.
    u32 error;    // ERROR_SUCCESS, or why the attempt failed
    u32 answered; // requests answered: 0 when the dumper says it is ready
} CrashDumperShared;

// What a dump keeps (the callback's parameter): the global data of the modules beside the
// executable only, unless the dump is of the whole process.
typedef struct CrashDumpScope {
    const wchar_t *exe_dir; // with its separator; NULL when unknown
    u32 exe_dir_len;
    b32 full;
} CrashDumpScope;

typedef BOOL(WINAPI *CrashMiniDumpWriteDumpFn)(HANDLE process, DWORD pid, HANDLE file,
                                               MINIDUMP_TYPE type,
                                               PMINIDUMP_EXCEPTION_INFORMATION exception,
                                               PMINIDUMP_USER_STREAM_INFORMATION user,
                                               PMINIDUMP_CALLBACK_INFORMATION callback);

// The dump's callback (MINIDUMP_CALLBACK_ROUTINE; `param` is a CrashDumpScope, or NULL for one
// that knows no executable): the global data of the modules beside the executable only, all of
// it in a full dump, and memory that cannot be read when the dump gets to it left out, which
// would otherwise fail the whole dump. Tests call it.
// NOLINTNEXTLINE(misc-misplaced-const): MINIDUMP_CALLBACK_ROUTINE's own signature
BOOL CALLBACK crash_win32_dump_callback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                        PMINIDUMP_CALLBACK_OUTPUT output);

// The dump's type: the threads, their stacks, the memory they point at, the modules' data (as the
// scope keeps it), handles, and the memory map; with `full`, the whole process's memory instead.
MINIDUMP_TYPE crash_win32_dump_type(bool full);

// Loads dbghelp from the system's directory, never the executable's, and finds its
// MiniDumpWriteDump: ERROR_SUCCESS, or why not. Loaded ahead of any failure, since loading a
// library while a process fails could take the loader's lock a failing thread holds.
DWORD crash_win32_dbghelp_load(HMODULE *out_module, CrashMiniDumpWriteDumpFn *out_write);

// One attempt at a dump of `process` (`pid`) into `file`, from the file's start and cut to what it
// writes, as `stage` says: ERROR_SUCCESS, or why not. `mei` may be NULL (no exception). `comment`
// goes into the dump's comment stream. Allocates nothing.
DWORD crash_win32_dump_attempt(CrashMiniDumpWriteDumpFn write_dump, HANDLE process, DWORD pid,
                               HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION *mei,
                               const CrashDumpScope *scope, CrashDumpStage stage,
                               const char *comment);
