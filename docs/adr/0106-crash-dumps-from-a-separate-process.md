# 0106. Crash dumps from a separate process: the dumper beside the executable

- **Date:** 2026-11-18
- **Status:** Proposed (the user's decision of 2026-11-18, to write the dumps from a separate process; amends
  [ADR-0103](0103-crash-handler.md)'s decisions 2, 3 and 8, and its first alternative)

## Context

ADR-0103 wrote the minidump inside the failing process. A thread made at install called `MiniDumpWriteDump` on its
own process, and it listed a watchdog process that writes the dump from outside as the design for a public release.

On some of GitHub's Windows runners, every dump a process wrote of itself failed with `ERROR_INVALID_USER_BUFFER`
(0x800706F8), however often it was retried, the reduced dump included. The stack-overflow modes then left no report
at all.
- It happened in about 3% of some 160 Windows jobs over two days, in four Azure regions.
- The runners were alike: Windows Server 2022 build 20348, the windows-2022 image 20261112.366.7, dbghelp
  10.0.20348.5622, and user shadow stacks off everywhere (the machine lines #190 added). That ruled out shadow
  stacks (CET).
- It never happened on the development machine (Windows 10 19045, dbghelp 10.0.19041). Nor did it with threads
  starting and ending around the dump, with dumps at a process's start, or with an 8 MB hard cap on the working set
  of a process with a 512 MB data segment.
- It belongs to a process, not to a runner: in one job the platform tests' process failed every dump, and the
  helper processes after it on the same runner wrote theirs.

Microsoft's documentation of `MiniDumpWriteDump` advises calling it "from a separate process if at all possible,
rather than from within the target process being dumped". In process, dbghelp copies the memory of the process it
runs in while that process runs around it, its own thread's stack included. From outside, it reads the failing
process's memory into buffers of its own (`ReadProcessMemory`), and nothing in the failing process can fail it.

The user decided on 2026-11-18 to write the dumps from a separate process.

## Decision

1. **A dumper process, `nph_crash_dumper.exe`** (`engine/platform/dumper/main.c`), found beside the executable.
   - Every executable the build makes is in `bin/<config>`, and whatever links the platform module builds the
     dumper, so it is beside every one of them.
   - It links no engine library. It shares `crash_dump_win32.c` with the handler: one attempt at a dump, and the
     dump's callback, which keeps the global data of the modules beside the executable only (ADR-0103).
   - It is about 13 KB, and it allocates nothing once it is ready.
2. **Started at install** (`nph_crash_install`, Windows):
   - the handler maps a block of shared memory (`CrashDumperShared`) and makes two events;
   - it duplicates a handle to its own process, with what dbghelp needs: query, read, duplicate handles, and
     synchronize;
   - it starts the dumper in the executable's directory. The dumper inherits those four handles alone
     (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), and gets no console and no standard handles, so a test runner reading the
     process's pipes does not wait for the dumper too;
   - it waits up to 10 s for the dumper to say it is ready (dbghelp loaded, from the system's directory).

   Install's log line says which process writes the dumps.
3. **A dump is asked for, not written.**
   - The handler's thread writes the report as before. For each attempt at the dump it hands the dumper the file
     (duplicated into it), the failing thread's id, and the address of the exception's pointers. Then it waits for
     the answer, or for the dumper's end.
   - dbghelp, in the dumper, reads the exception from the failing process (`ClientPointers`), and the process's
     memory through the handle.
   - The attempts stay as ADR-0103 made them: once more a tenth of a second later, then reduced. They cost nothing
     when the first one succeeds.
   - The dump's comment stream says which process wrote it: `written by nph_crash_dumper`, or `written by the
     failing process`.
4. **The dumper ends with its process.** It waits on the process's handle as well as on the requests, so it ends
   when the process ends, crashed or not. Uninstall asks it to end, and after 5 s makes it.
5. **Without the dumper, the failing process writes the dump**, as before, and the report's dump line ends with
   `, by the failing process (<why>)`. That happens when the dumper:
   - is not beside the executable;
   - does not start, or is not ready in 10 s;
   - ended before the fault;
   - ends while it writes;
   - does not answer in 100 s (840 s for a full dump). It is then made to end first, so that it writes no more into
     the file.

   Install's log line warns.
6. **Linux is unchanged**: it writes no minidump, and the signal handler writes the report.

## Alternatives considered

- **Mend the dump in process.** The failure could not be made to happen anywhere but on some of CI's runners, and
  dbghelp's own documentation advises against dumping a process from inside it.
- **The dumper inside each executable**, the executable started again with an argument, as Crashpad's handler can
  be. Every host's `main`, every test executable's included, would have to look for the argument first. A file of
  its own needs nothing of them, and is what Crashpad and Breakpad's out-of-process mode ship.
- **Crashpad or Breakpad:** third-party code (CLAUDE.md §3), and an upload pipeline that in-house use does not need.
- **Windows Error Reporting's LocalDumps:** a machine-wide registry setting, with no report text, notes, or log.
- **Named pipes or named objects** to find the dumper: a name can be taken first by another process. Inherited
  handles, passed by their values, need no names.
- **Suspending the whole process for the dump**, as Crashpad does. dbghelp suspends each thread to read its
  context, and the failing thread already waits. Not built.
- **No dumper, no handler:** install failing without the dumper would cost a host its reports along with its dumps.

## Consequences

- A process that installs the handler runs a second process while it lives. On the reference machine (Profile):
  - install and uninstall take 5.7 ms with the dumper, against 0.9 ms with it moved aside
    (`crash_install_uninstall` in platform_bench), once a process and never on a frame's path;
  - the waiting dumper holds 0.75 MB of private memory, in a 3.1 MB working set that is mostly the system's and
    dbghelp's shared pages.
- A dump no longer depends on what the failure left of the failing process, since dbghelp does not run in it. The
  report is still written there, by the handler's thread, as ADR-0103 decided.
- A game ships `nph_crash_dumper.exe` beside its executable (packaging, after Phase 16). Without it, a game's dumps
  are written in process, and the reports say so.
- Bitdefender quarantined the platform tests' executable, as a generic machine-learning detection
  (`Gen:Variant.Application.Tedy`), the first time it ran with this code; the build directory needs an exclusion on
  the development machine. An unsigned program that starts a process to read its memory may be flagged on players'
  machines too: the public release's signed builds are the answer there (after Phase 16).
- The tests (docs/platform.md "Testing"):
  - every crash mode's dump is read back on Windows, and must be the dumper's;
  - the in-process tests cover the dumper beside the executable, its end at uninstall and with its process, and
    the failing process writing the dump without it and after it ended;
  - the retries go through the dumper, as the tests make them fail.
