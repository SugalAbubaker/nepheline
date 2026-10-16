# 0022. Profiling: zones recorded in core, collected per fiber by the profile module

- **Date:** 2026-10-16
- **Status:** Accepted

## Context

CLAUDE.md §8.11 asks for CPU instrumentation zones recorded per thread
into ring buffers, memory tracking per allocator and tag, an in-editor
timeline viewer, and a Chrome trace export. The roadmap gives the profile
module no phase of its own; the editor's profiler panel (§8.13) and the
runtime's CPU budgets (§10.5: main thread at most 1 ms, physics at most
1 ms of job time, render submission at most 0.3 ms) need it, so Phase 9
builds it (decided with the user on 2026-10-16).

Forces:

- **Layering.** The module order (§4) is `core → math → platform → jobs →
  gpu → render`, then `asset`, `scene`, `physics`, `audio`, `profile`,
  then `runtime`. A module includes only modules to its left, so a
  `profile` module cannot be included by `jobs`, `gpu`, `render`, or
  `scene`, which hold most of the per-frame CPU work.
- **Fibers.** Jobs run on fibers that park on counters and resume on other
  worker threads (ADR-0013). A zone that spans a wait begins on one thread
  and ends on another. Per-thread zone stacks would nest unrelated jobs
  inside each other and pop the wrong zone.
- **No hidden costs** (§1.4) and **no global mutable state** outside the
  engine context, except logging and profiling sinks (§2).
- **C17**: no destructors to close a scope.
- Recording must be cheap enough to leave on in Development and Profile
  builds; Shipping compiles it out (ADR-0003).

## Decision

1. **Recording lives in core** (`core/profile.h`), beside logging and on
   the same pattern: process-wide state (the exception §2 allows), a
   single-producer ring per thread created at attach or at the thread's
   first event, retirement on detach, and a drain for one collector. The
   collecting half is the `engine/profile` module (`profiler.h`,
   `memory.h`), which only the runtime, the editor, and games use.
2. **Events, not zones, are recorded**: 32 bytes each (time-stamp counter,
   static name pointer, 64-bit value, type: BEGIN, END, FRAME, FIBER,
   COUNTER). The time-stamp counter (RDTSC, about 7 ns) rather than the
   monotonic clock (about 24 ns) is the clock; the collector converts with
   the rate platform/cpu.h reports or measures. The recording functions
   are not inlined, so a zone that spans a fiber switch never reuses a
   thread-local address computed before it.
3. **Explicit BEGIN/END pairs**, no `NPH_PROFILE_SCOPE`. GCC's and Clang's
   `cleanup` attribute could end a scope automatically, but MSVC has no
   equivalent, and a block macro built from a `for` loop silently skips
   its end on `return` or `break`. The collector repairs unbalanced streams
   rather than trusting them, and counts each repair.
4. **Zone stacks per fiber.** The job system reports every fiber switch
   (`nph_profile_fiber` after each `nph_fiber_switch`). The collector
   keeps a stack per fiber plus one per thread for code outside fibers;
   when a fiber leaves a thread, the pieces its open zones ran there are
   emitted as segments, and they continue where it resumes. Segments are
   pieces of zones that one thread ran without interruption, so a timeline
   lane is always truthful (after Tracy's fiber support).
5. **Merging by timestamp with a settle window.** All threads' events are
   merged with a heap keyed by timestamp. Events younger than 2 ms wait
   for the next collection, so an event stamped but not yet pushed when
   the rings were drained is still processed in order. This relies on the
   invariant, synchronised TSC of every supported CPU (§3).
6. **Frames cut zones.** A FRAME event closes the frame in progress and
   cuts every running zone there, so each segment lies inside one frame
   and a frame's segments are contiguous in a fixed segment ring. The
   history keeps the newest frames whose segments are still in the ring.
7. **Names are interned on first sight** (pointer cache, then content
   hash), so the history survives freed names. Code about to free names
   (a game module unloading, a schedule being destroyed) calls
   `nph_profiler_forget_names`, which processes every pending event and
   then forgets the pointer cache.
8. **Memory tracking is a counting allocator wrapper** with atomic
   counters (bytes and allocations alive, peaks, totals), registered with
   the profiler for the panel; it never knows or changes what it wraps.
9. **Chrome Trace Event Format** for export: complete events per segment,
   frame instants, named thread lanes, counter events.
10. **What is instrumented now:** every job (by its declared name), fiber
    switches, worker and I/O thread lanes; the ECS schedule, each system
    run on the calling thread, and each chunk piece; render-graph compile,
    execute, and per-pass recording; the renderer's frame. The runtime adds
    its frame phases.

## Alternatives considered

- **The whole profiler in `engine/profile`, with hooks registered into
  lower modules.** A table of function pointers in core that the profile
  module fills is the same global state with an indirection per event and
  a second header to keep in sync. Rejected.
- **Per-thread zone stacks only.** Breaks as soon as a job waits:
  measured jobs nest inside one another and ENDs close the wrong zones.
- **Integrating Tracy.** A mature profiler with the same fiber model, but
  a third-party dependency with a C++ client, and §3 keeps the profiler
  among the things the engine writes itself. Rejected; its design informs
  this one.
- **The monotonic clock (QPC, `CLOCK_MONOTONIC`).** Three times the cost
  of the counter per event, for no accuracy the invariant TSC lacks.
- **Copying zone names into events** (24 more bytes per event and a copy
  per zone) instead of interning at collection. Rejected: interning costs
  a hash lookup per event on the collector's side only.
- **Sorting each collection's events** instead of merging per-thread
  streams. Each thread's events are already in order; a heap merge is
  O(n log threads) where a sort is O(n log n).

## Consequences

- Every module can be instrumented, at 17 ns per zone when recording and
  2.3 ns when paused or uninitialised; Shipping pays nothing.
- Timelines show what each thread really ran, including jobs that migrate;
  the jobs test checks that segments of one depth never overlap on a lane.
- Instrumented code must pair BEGIN and END on every path; mistakes are
  repaired and counted, not silent.
- Zone names must outlive their collection; module reloads and schedule
  rebuilds call `nph_profiler_forget_names` first (the runtime does).
- The collector costs about 15 ns per event (0.18 ms for 12 000 events), on
  the main thread, in profiling builds.
- GPU timings stay a separate table (`NphRenderGpuProfiler`) until
  calibrated timestamps put them on the CPU timeline. The crash handler of
  §8.11 remains Phase 15 work.
