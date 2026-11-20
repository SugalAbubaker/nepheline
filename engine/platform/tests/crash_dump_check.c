// SPDX-License-Identifier: Apache-2.0
// Reads a minidump the crash handler wrote, with dbghelp, and prints what
// run_crash.cmake checks: the exception's code, thread, and the instruction
// its context holds, that the thread
// is among the dump's threads, how many threads and modules it holds, and
// its comment, which says what wrote it (the dumper or the failing process,
// docs/adr/0106-crash-dumps-from-a-separate-process.md).
//
//     crash_dump_check <file.dmp>
//
// Exit 0 when the dump reads, 1 when it does not, 2 for a usage error.
#include <nepheline/core/base.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// dbghelp.h needs windows.h's types first; sorted, it would come before it.
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: crash_dump_check <file.dmp>\n");
        return 2;
    }
    HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "crash_dump_check: cannot open %s\n", argv[1]);
        return 1;
    }
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0U, 0U, NULL);
    void *base = mapping != NULL ? MapViewOfFile(mapping, FILE_MAP_READ, 0U, 0U, 0U) : NULL;
    int rc = 1;
    if (base == NULL) {
        fprintf(stderr, "crash_dump_check: cannot map %s\n", argv[1]);
    } else {
        PMINIDUMP_DIRECTORY dir = NULL;
        void *stream = NULL;
        ULONG size = 0U;
        if (!MiniDumpReadDumpStream(base, ExceptionStream, &dir, &stream, &size) ||
            size < sizeof(MINIDUMP_EXCEPTION_STREAM)) {
            fprintf(stderr, "crash_dump_check: no exception stream\n");
        } else {
            const MINIDUMP_EXCEPTION_STREAM *ex = (const MINIDUMP_EXCEPTION_STREAM *)stream;
            printf("exception 0x%08lX thread %lu\n",
                   (unsigned long)ex->ExceptionRecord.ExceptionCode, (unsigned long)ex->ThreadId);
#if defined(_M_X64)
            // The context the dump holds for it: the copy the handler made (ADR-0106's
            // amendment), whose instruction must be the report's.
            if (ex->ThreadContext.DataSize >= sizeof(CONTEXT)) {
                const CONTEXT *ctx = (const CONTEXT *)((const char *)base + ex->ThreadContext.Rva);
                printf("context rip 0x%016llX\n", (unsigned long long)ctx->Rip);
            }
#endif
            rc = 0;
            void *comment = NULL;
            ULONG comment_size = 0U;
            if (MiniDumpReadDumpStream(base, CommentStreamA, &dir, &comment, &comment_size) &&
                comment_size != 0U) {
                // Its terminator is in the stream.
                printf("comment: %.*s\n", (int)(comment_size - 1U), (const char *)comment);
            }
            if (MiniDumpReadDumpStream(base, ThreadListStream, &dir, &stream, &size)) {
                const MINIDUMP_THREAD_LIST *threads = (const MINIDUMP_THREAD_LIST *)stream;
                int listed = 0;
                for (ULONG i = 0U; i < threads->NumberOfThreads; ++i) {
                    if (threads->Threads[i].ThreadId == ex->ThreadId) {
                        listed = 1;
                    }
                }
                printf("threads %lu\nexception thread listed %d\n",
                       (unsigned long)threads->NumberOfThreads, listed);
            }
            if (MiniDumpReadDumpStream(base, ModuleListStream, &dir, &stream, &size)) {
                const MINIDUMP_MODULE_LIST *modules = (const MINIDUMP_MODULE_LIST *)stream;
                printf("modules %lu\n", (unsigned long)modules->NumberOfModules);
                // The executable's global data: the dump keeps memory inside
                // its image (the handler keeps the data segments of the
                // modules beside it, and drops the others').
                void *mem = NULL;
                ULONG mem_size = 0U;
                if (modules->NumberOfModules != 0U &&
                    MiniDumpReadDumpStream(base, MemoryListStream, &dir, &mem, &mem_size)) {
                    const MINIDUMP_MODULE *exe = &modules->Modules[0];
                    const ULONG64 lo = exe->BaseOfImage;
                    const ULONG64 hi = lo + exe->SizeOfImage;
                    const MINIDUMP_MEMORY_LIST *list = (const MINIDUMP_MEMORY_LIST *)mem;
                    ULONG64 inside = 0U;
                    for (ULONG i = 0U; i < list->NumberOfMemoryRanges; ++i) {
                        const ULONG64 start = list->MemoryRanges[i].StartOfMemoryRange;
                        const ULONG64 end = start + list->MemoryRanges[i].Memory.DataSize;
                        if (start < hi && end > lo) {
                            inside += (end < hi ? end : hi) - (start > lo ? start : lo);
                        }
                    }
                    printf("executable data %s (%llu bytes)\n", inside != 0U ? "present" : "absent",
                           (unsigned long long)inside);
                }
            }
        }
        UnmapViewOfFile(base);
    }
    if (mapping != NULL) {
        CloseHandle(mapping);
    }
    CloseHandle(file);
    return rc;
}
