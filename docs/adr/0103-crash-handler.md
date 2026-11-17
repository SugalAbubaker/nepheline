# 0103. The crash handler: a report and a minidump from the failing process, fibers included

- **Date:** 2026-11-17
- **Status:** Proposed (Phase 15's first step, ADR-0102; CLAUDE.md §8.11 amended; amends
  [ADR-0010](0010-fibers.md)'s stacks on Windows)

## Context

CLAUDE.md §8.11 asks for a "crash handler with minidumps (Windows) and backtraces (Linux)", and ADR-0102 puts it
first in Phase 15, with autosave (ADR-0104), so that the user's work and its failures are kept from the phase's
first day. Before it, a crash left nothing behind. The log reached the console every few milliseconds, and the
last of it never. Windows wrote no dump unless its error reporting was told to in the registry, machine-wide.

A handler runs in a process that has just failed. Whatever the failure broke, the handler must not need:
- the heap, which may be corrupt or locked;
- a lock the failing thread holds;
- on Linux, anything but async-signal-safe calls inside the signal handler;
- the failing thread's own stack, when that stack overflowed.

The engine runs most of its work as jobs on fibers (ADR-0013). The fibers' stacks come from `fiber.c`, and the
switch is our own assembly (ADR-0010).

**What the first build showed on Windows.** An access violation on a fiber left no report. Windows dispatches an
exception by unwinding the stack it happened on, looking for a handler, and it calls the unhandled-exception filter
only from the thread's own start frame. A fiber's stack ends at the assembly trampoline, which has no unwind data,
so the dispatch never got there, and the process ended unreported.

A fiber's own stack overflow was not reported either. Its guard page was a page of no access, so the processor's
fault had no room left on the stack to be delivered from.

## Decision

1. **The handler is platform's** (`platform/crash.h`; docs/platform.md "The crash handler"). §8.11 lists it under
   `profile`, but it is an OS facility: signals, structured exceptions, dbghelp, alternate stacks. And only platform
   includes OS headers (platform_internal.h). Install and uninstall are process-wide, as the OS's hooks are: the
   §2 exception that the log's sinks have.
2. **What a crash leaves**, in the directory given at install:
   - **A text report**, `<app>-<yyyymmdd>-<hhmmss>-<pid>.txt`, holding:
     - the program, its version and build, the UTC time, the process, and the thread, by the name the engine gave it;
     - the fault and its details: a write, `SEGV_MAPERR`, and the address;
     - the instruction;
     - the failed assertion, when one ended it (install wraps the assert handler, and calls it on);
     - the notes, `nph_crash_set_note`;
     - the stack: on Windows as module + offset, unwound with the modules' unwind data; on Linux a `backtrace()`
       and the memory map, for `addr2line`;
     - the log's last 64 records (`nph_crash_log_sink`).
   - **On Windows, a minidump** beside it (`MiniDumpWriteDump`). By default it holds the threads, their stacks, the
     memory those stacks point at, the modules, handles, and the memory map. It keeps global data for the modules
     beside the executable only, which are the engine's and the game's: with every module's, the player's dump
     came to 40 MB, nearly all of it NVIDIA's driver, and without the driver's it is 1.7 MB. `crash.full_dump=1`
     writes the whole process instead.
   - **A line on standard error** naming the report.

   The process then ends as the fault would have ended it. On Windows that is the exception's code as the exit
   code, with no error-reporting dialog. On Linux it is the signal's default action, raised again: its exit
   status, and a core file where the system writes them.
3. **How, on each system:**

   | | Windows | Linux |
   |---|---|---|
   | Hooks | the unhandled-exception filter; the C runtime's SIGABRT (`abort()`, so failed asserts), invalid-parameter, and pure-call handlers | `sigaction` on SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS |
   | Who writes | a thread made at install: the failing thread hands it the exception and waits | the signal handler, on the thread's alternate signal stack (64 KiB with a guard page), with async-signal-safe calls; `backtrace()` is warmed at install |
   | Stack overflows | `EXCEPTION_STACK_OVERFLOW` (the writing thread has a stack of its own) | the guard page's SIGSEGV, on the alternate stack |

   A second fault while one is reported waits for the process to end. A fault while reporting ends it at once.
   Under a debugger, Windows calls no filter, and the fibers' frame (below) and the C runtime's hooks stop in the
   debugger first.
4. **Fibers on Windows.**
   - **A frame at the base of every fiber's stack.** A `__try` in `nph_fiber_entry` (and in the Win32-fiber
     backend's) stands in for the thread's start: what no handler on the fiber took reaches its filter, which
     reports as the unhandled-exception filter does. It is table-based, so it costs nothing until an exception
     comes.
   - **Fiber stacks laid out like a thread's.** Below the committed stack, which is committed whole as before,
     there is now a guard page of the system's kind and reserved pages to 64 KiB. Because the switch keeps the
     thread information block's stack bounds per fiber (ADR-0010), the system treats a fiber's stack as the
     thread's: an overflow grows into the reserve, and then raises `EXCEPTION_STACK_OVERFLOW` with the system's
     guarantee left to report from. Linux keeps one no-access guard page, because there the alternate stack
     reports the fault.
5. **The log, last and under a watchdog.** Once the report and its dump are written:
   - the log's pending records are delivered (`nph_log_drain`) and the C library's streams flushed, so the
     console and the log files get the last lines (a file sink buffers what is below an error);
   - then the report ends with the ring's records.

   The sinks take locks the failure may hold. So on Windows the failing thread waits only 5 s for this step
   before it ends the process, and on Linux `alarm(5)` ends it. What was written before stays.
6. **Not under the sanitizers.** ASan and TSan own the same signals and report more; install returns
   `NPH_RESULT_UNSUPPORTED` there (the Debug builds), and the hosts run without a handler.
7. **The hosts install it before the engine,** so that a failure while the engine comes up is reported too:
   - `nph_engine_crash_handler_install` reads `crash.dir=<path>` (default `crashes`, in the working directory,
     the editor's project) and `crash.full_dump=1` from the command line, and adds the log's sink. The editor and
     the player call it.
   - The engine keeps three notes current: the GPU and its driver, the scene last loaded or saved, and the game
     module. The editor sets the project.
   - The console's `crash [access|abort|assert]` ends the process on purpose, to see all of it work. It is not in
     Shipping builds.
8. **Tested on real faults.** `crash_helper` fails in a child process, and `run_crash.cmake` checks:
   - that the process ended by the fault;
   - the report's text;
   - on Windows, the minidump read back with dbghelp (`crash_dump_check`): its exception, its thread, the
     executable's data;
   - the log line delivered after it.

   The modes are access violation, abort, a failed assertion, division by zero, an illegal instruction, a stack
   overflow, a fault on a thread, two threads at once, a fault on a fiber, a fiber's own stack overflow, the
   handler uninstalled, and on Windows the C runtime's invalid parameter and pure call. The in-process tests
   (`test_crash.c`) cover the text, the time arithmetic (every day of four centuries), the notes, the log's ring,
   install's checks, the assert hook, and a whole report written without a crash.

## Alternatives considered

- **A watchdog process** that writes the dump from outside (as Crashpad does): it survives a corrupt heap or a held
  loader lock, which an in-process handler only survives by luck and timeouts. It is a second process to start,
  find, and keep alive. That is right for a public release, where crashes come from machines we never see. For
  in-house use the in-process handler, with its timeouts, is enough. It is not built.
- **Crashpad or Breakpad:** third-party code, which needs an ADR of its own (CLAUDE.md §3). In-process, they have
  the same limits, and out of process the same cost as above.
- **Windows Error Reporting's LocalDumps:** a machine-wide registry setting, with no report text, no notes, no log,
  and nothing on Linux.
- **The dump written from the failing thread:** a thread whose stack overflowed has none left to write it with.
- **A vectored exception handler for the fibers:** it sees every exception first, those that a handler on the stack
  would take too (a driver's own `__try`), and would report them as crashes.
- **The global data of every module in the dump:** 40 MB a crash, for the driver's data, which no one reads.
- **Saving the user's scene from the handler:** serializing a world that just failed takes the heap and whatever
  it holds. Autosave keeps the work instead (ADR-0104).

## Consequences

- A crash in the editor or the player leaves a report and, on Windows, a dump, which Visual Studio opens with the
  build's PDBs. The report alone usually says which module and function: the offsets match the PDB's or
  `addr2line`'s.
- A fiber on Windows takes 60 KiB more address space and one more committed page, and an overflow of a fiber's
  stack is reported instead of ending the process silently. The job system's stack check (`stack_remaining`)
  reads the same lowest usable address as before.
- `nph_assert_default_handler` is public in core, so that a handler wrapping it can call it on.
- Still unreported: on Windows a fail-fast (a /GS cookie check, heap corruption the heap detects), and on both
  systems a kill from outside or a hang. A hang is no crash; a watchdog for frames that stop is for later.
- Under the sanitizers, a fault is the sanitizer's report on standard error, not ours.
