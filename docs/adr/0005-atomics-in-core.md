# 0005. Atomics live in core; the logger thread lives in platform

- **Date:** 2026-10-11
- **Status:** Accepted

## Context

CLAUDE.md §4 fixes the module order `core → math → platform → …` and says
`core` depends only on the C standard library and compiler intrinsics. §8.3
places `nph_atomic.h` in `platform`. Yet §8.1 asks `core` for lock-free
containers (SPSC and Vyukov MPMC rings) and for logging with "a lock-free
per-thread ring buffer flushed by a logger thread". Those need atomics
with explicit memory orders, and a logger *thread* needs the OS.

MSVC does not ship `<stdatomic.h>` for C, so a portable atomics layer is
required in any case.

## Decision

1. **`nepheline/core/atomic.h` is part of core.** It is implemented purely
   with compiler intrinsics (`_Interlocked*`, `__iso_volatile_*`,
   `_ReadWriteBarrier`, `_mm_mfence` on MSVC; `__atomic_*` builtins on
   GCC/Clang), which §4 explicitly allows core to use. It provides u32, u64,
   and pointer atomics with C11 memory orders, fences, `nph_cpu_relax`, and
   a spin lock reserved for diagnostic code. `platform` builds threads,
   mutexes, condition variables, and semaphores on top of it; §8.3's listing
   of atomics under platform is superseded by this record.
2. **Atomic values are wrapped in structs** (`NphAtomicU32 { u32 value; }`)
   so that a plain read or write of the payload is a visible mistake, and
   the only way to touch it is through the ordered operations.
3. **MSVC implementation targets x86-64 only for now.** On x86's TSO model
   loads are acquire and stores are release in hardware, so those orders
   compile to a compiler barrier; `seq_cst` stores use a locked exchange;
   every read-modify-write is a full barrier. This mirrors the MSVC STL.
   ARM64 (which needs `_acq`/`_rel` intrinsics and `dmb`) is added when the
   platform is, behind the same API.
4. **Logging is split across the boundary.** Core implements the per-thread
   rings, the message format, the sinks, and `nph_log_flush()`, which drains
   every registered ring into the sinks on the calling thread. The thread
   that calls it periodically ("the logger thread") is created by `platform`
   in Phase 3 and owned by the runtime. Until then, and in tools, flush is
   called synchronously at well-defined points. Thread-local storage uses
   the C11 `_Thread_local` keyword (`__declspec(thread)` on MSVC), which is
   a language feature, not an OS call.

## Alternatives considered

- **Keep atomics in platform and move the lock-free containers and logging
  there** — breaks §8.1 (they are core facilities that math and everything
  else want) and would make `core` unable to log.
- **Use `<stdatomic.h>` where available and a shim on MSVC** — MSVC's C
  support (`/experimental:c11atomics`) is still experimental and would add a
  second code path to validate; one implementation over intrinsics is
  smaller and is what the MSVC STL itself does.
- **A single global MPSC log queue instead of per-thread rings** — simpler
  registry, but every log call then contends on one cache line; per-thread
  SPSC rings make a log call a few stores with no shared writes.

## Consequences

- `core` gains a dependency on compiler intrinsics for atomics, within the
  letter of §4. No OS header is included by core.
- Multi-threaded tests of the atomics, rings, and logger arrive with the
  thread API in Phase 3; Phase 1 tests cover single-threaded semantics and
  the memory-order mapping by inspection.
- The spin lock in core is a documented hazard: it exists for the debug
  allocator and must not appear on a hot path (§1.4).
