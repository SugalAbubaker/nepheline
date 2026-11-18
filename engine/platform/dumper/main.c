// SPDX-License-Identifier: Apache-2.0
// nph_crash_dumper: writes a failing process's minidumps from outside it
// (docs/adr/0106-crash-dumps-from-a-separate-process.md).
//
//     nph_crash_dumper <process> <block> <request> <done>
//
// The crash handler (crash_win32.c) starts it when it installs, from beside the executable, and
// hands it four inherited handles by their values in hexadecimal: the process to dump, the
// mapping of the block the two share (CrashDumperShared), and the request and done events. It
// loads dbghelp, says it is ready, and waits. It answers a request with one attempt at a dump of
// the process into the file the request hands it. The process's end, or the handler's
// uninstall, ends it.
//
// From outside, dbghelp reads the failing process's memory into buffers of its own
// (ReadProcessMemory), whatever the failure left of that process. From inside, on some of CI's
// runners, every dump a process wrote of itself failed (ADR-0103, ADR-0106). Once ready, the
// dumper allocates nothing, and it shows no dialog. Its exit codes are in crash_dump_win32.h.
#include "crash_dump_win32.h"

#include <stdlib.h>

// A handle's value from its hexadecimal digits; NULL for anything else.
static HANDLE parse_handle(const char *s)
{
    char *end = NULL;
    const unsigned long long v = strtoull(s, &end, 16);
    if (end == s || *end != '\0' || v == 0ULL) {
        return NULL;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr): a handle's value, as the handler wrote it
    return (HANDLE)(uptr)v;
}

static CrashDumpStage stage_of(u32 v)
{
    return v == (u32)CRASH_DUMP_STAGE_FAIL       ? CRASH_DUMP_STAGE_FAIL
           : v == (u32)CRASH_DUMP_STAGE_STAND_IN ? CRASH_DUMP_STAGE_STAND_IN
                                                 : CRASH_DUMP_STAGE_DBGHELP;
}

int main(int argc, char **argv)
{
    // A failure of its own ends it quietly: a dialog would hold an unattended run.
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 5) {
        return CRASH_DUMPER_EXIT_USAGE;
    }
    HANDLE process = parse_handle(argv[1]);
    HANDLE mapping = parse_handle(argv[2]);
    HANDLE request = parse_handle(argv[3]);
    HANDLE done = parse_handle(argv[4]);
    if (process == NULL || mapping == NULL || request == NULL || done == NULL) {
        return CRASH_DUMPER_EXIT_USAGE;
    }
    CrashDumperShared *shared = (CrashDumperShared *)MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0U, 0U, sizeof(CrashDumperShared));
    if (shared == NULL || shared->magic != CRASH_DUMPER_MAGIC ||
        shared->version != CRASH_DUMPER_VERSION || shared->exe_dir_len >= CRASH_DUMPER_DIR_MAX) {
        return CRASH_DUMPER_EXIT_BLOCK;
    }
    HMODULE dbghelp = NULL;
    CrashMiniDumpWriteDumpFn write_dump = NULL;
    if (crash_win32_dbghelp_load(&dbghelp, &write_dump) != ERROR_SUCCESS) {
        return CRASH_DUMPER_EXIT_DBGHELP;
    }
    const DWORD pid = GetProcessId(process);
    const CrashDumpScope scope = {shared->exe_dir, shared->exe_dir_len, shared->full_dump != 0U};
    shared->answered = 0U;
    (void)SetEvent(done); // ready

    // The process first: once it has ended, a request of its is no longer answered.
    const HANDLE waits[2] = {process, request};
    for (;;) {
        const DWORD w = WaitForMultipleObjects(2U, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 1U || shared->quit != 0U) {
            break;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr): the handle the handler duplicated in
        HANDLE file = (HANDLE)(uptr)shared->file;
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = shared->thread_id;
        // NOLINTNEXTLINE(performance-no-int-to-ptr): an address in the dumped process...
        mei.ExceptionPointers = (PEXCEPTION_POINTERS)(uptr)shared->pointers;
        mei.ClientPointers = TRUE; // ...which dbghelp reads there
        const MINIDUMP_TYPE type = (MINIDUMP_TYPE)shared->type; // flags, as dbghelp takes them
        shared->error = crash_win32_dump_attempt(write_dump, process, pid, file, type,
                                                 shared->pointers != 0U ? &mei : NULL, &scope,
                                                 stage_of(shared->stage), CRASH_DUMP_BY_DUMPER);
        (void)CloseHandle(file);
        shared->answered += 1U;
        (void)SetEvent(done);
    }
    FreeLibrary(dbghelp);
    (void)UnmapViewOfFile(shared);
    return 0;
}
