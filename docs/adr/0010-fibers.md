# 0010. Fibers: assembly context switch, guard-paged stacks, and the Win32 fallback

- **Date:** 2026-10-12
- **Status:** Accepted

## Context

CLAUDE.md §8.3 specifies "hand-written assembly context switch for the
Windows x64 and System V x64 ABIs, with guard-paged stacks. Win32 Fibers
as a fallback", and §8.4 builds the job system on fibers (Gyrling,
"Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015): a job that
waits on a counter parks its fiber and the worker thread runs another job,
so no OS thread ever blocks and no kernel transition happens on the wait
path. The switch cost therefore sits on the hot path thousands of times
per frame.

The engine's Debug builds run under AddressSanitizer and the job system's
CI under ThreadSanitizer (§9), and both sanitizers track stacks: a stack
switch they are not told about produces false reports or corrupts their
bookkeeping.

## Decision

1. **The switch is a stack swap.** `nph_fiber_switch_asm(void **from_sp,
   void *to_sp)` pushes what the ABI makes a callee responsible for onto
   the current stack, stores the stack pointer through `from_sp`, loads
   `to_sp`, pops the same, and returns. Windows x64 saves rbx, rbp, rdi,
   rsi, r12–r15, xmm6–xmm15, MXCSR, the x87 control word, and the three
   stack bounds in the thread information block (`StackBase`,
   `StackLimit`, `DeallocationStack`), which the stack probe (`__chkstk`)
   and exception dispatch read. System V saves rbx, rbp, r12–r15, MXCSR,
   and the x87 control word; its vector registers are all caller-saved.
   Windows assembles with MASM (`enable_language(ASM_MASM)`), Linux with
   the GNU assembler from a `.S` file (`.note.GNU-stack` marks the stack
   non-executable; `.cfi_undefined rip` ends unwinding at the trampoline).
2. **A fresh fiber's first switch "returns" into a trampoline.** fiber.c
   lays out on the new stack exactly the frame the switch restores, with
   r12 holding the `NphFiber *` and the return slot holding the
   trampoline, which calls `nph_fiber_entry(fiber)`. A zero word above
   it terminates stack walks. A fiber function never returns; doing so is
   a fatal error (logged, then `abort`).
3. **Stacks come from vmem.h with a guard page.** `nph_fiber_create`
   reserves `size + page` bytes and commits all but the lowest page, which
   stays inaccessible, so an overflow faults at once. Sizes round up to
   pages with a 16 KiB minimum and a 64 KiB default; the job system picks
   its own. Nothing else is allocated; the `NphFiber` lives where the
   caller puts it.
4. **Fibers migrate between threads.** The switch has no thread affinity:
   a fiber suspended on one thread resumes on whichever thread switches to
   it, which is what lets the job system's workers pick up parked jobs.
   Thread-local storage is the hazard: the compiler computes a
   thread-local's address once per function and may keep it across calls,
   so code after a switch may read the wrong thread's variable. fiber.c
   confines every thread-local access to non-inlined functions
   (`nph_fiber_switch` itself, `finish_switch`, `nph_fiber_current`): the
   Shipping build's LTO otherwise inlined the switch into a fiber function
   and its writes landed in the wrong thread's slot. The header tells
   callers never to cache a thread-local address across `nph_fiber_switch`.
5. **Sanitizer annotations.** With ASan, each switch brackets the assembly
   with `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`
   so the fake-stack and stack-bounds bookkeeping follows the fiber; the
   thread's own stack bounds are learned from the first switch away from
   it. With TSan, fibers are created, destroyed, and switched through the
   `__tsan_*_fiber` API. The MSVC AddressSanitizer runtime exports the same
   fiber functions as LLVM's (it is the same runtime), verified by the
   Debug build.
6. **The Win32 fiber API is a build-time fallback**
   (`-DNPH_FIBERS_WIN32=ON`): `ConvertThreadToFiberEx`, `CreateFiberEx`
   with `FIBER_FLAG_FLOAT_SWITCH`, `SwitchToFiber`. Same API and semantics,
   same tests; CI builds one Windows configuration with it so it cannot
   rot. It exists for diagnosing a problem in the assembly path on a new
   toolchain or OS build, not for shipping: it is slower (the kernel API
   saves more state) and cannot carry the sanitizer annotations.
7. **The assembly lives in its own CMake target** without `nph::options`,
   so the C warning and sanitizer flags never reach the assembler.
8. **MXCSR is switched; the x87 control word is not.** Both ABIs list both
   as callee-saved, and the first version saved both. Measured on the
   reference CPU (Ryzen 7 5800X, Profile), a round trip through two
   fibers cost 38 ns per switch with `fnstcw`/`fldcw`, 11 ns without them,
   and 10 ns without `stmxcsr`/`ldmxcsr` as well: `FLDCW` is a serializing
   microcoded instruction that costs more than everything else in the
   switch combined, while MXCSR costs about a nanosecond. x64 compilers
   never emit x87 code (`long double` aside, which the engine does not
   use), and ADR-0008 fixes every floating-point mode at its default, so
   the x87 word is invariant for the life of the process. MXCSR stays per
   fiber because it is cheap and because SSE rounding and denormal modes
   are what a DSP or physics job could conceivably change and expect to
   keep. The tests check MXCSR directly rather than through `fegetround`,
   which glibc answers from the x87 word.

## Alternatives considered

- **`ucontext` (`makecontext`/`swapcontext`) on Linux.** Rejected: each
  switch makes a `sigprocmask` system call, hundreds of times the cost of
  the register swap.
- **Win32 fibers as the primary Windows backend.** Rejected: slower (24 ns
  against 11 ns per switch, since the OS routine does switch the x87
  word), no sanitizer support, and a second code path to keep equivalent
  to the Linux one.
- **Unwind information (`.pdata`) for the Windows switch function.** Not
  emitted: the function changes rsp mid-body in a way the unwind codes
  cannot describe, and no exception is ever raised inside it. Stack walks
  from inside a fiber stop at the trampoline's zero return address, which
  is the intended end of the walk.
- **Saving the full XSAVE state (AVX upper halves, AVX-512).** Not needed:
  both ABIs make every vector register above xmm (and on System V, all of
  them) caller-saved, so the compiler has already spilled what matters
  before calling the switch.

## Consequences

- Phase 4 builds the scheduler on `nph_fiber_init_thread`,
  `nph_fiber_create`, and `nph_fiber_switch`, pins workers with
  `nph_thread_set_affinity`, and parks them with `NphSemaphore`.
- The job system's TSan job (§9) exercises the TSan annotations; the
  Debug matrix exercises the ASan ones on both platforms.
- ARM64 adds a third assembly file (AAPCS64: x19–x28, d8–d15, sp, lr)
  when that platform arrives; the C side does not change.
- `docs/platform.md` records the measured switch cost per backend.
