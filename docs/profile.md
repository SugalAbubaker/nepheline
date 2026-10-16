# Profiling (`core/profile.h`, `engine/profile`)

CPU instrumentation zones, recorded per thread into ring buffers and
collected into a frame-by-frame history (CLAUDE.md §8.11), plus memory
tracking per allocator and tag. The editor's profiler panel (Phase 9) shows
the history as a timeline and as statistics, and the history exports to the
Chrome Trace Event Format for chrome://tracing and Perfetto. Decisions:
[ADR-0022](adr/0022-profiling.md). GPU time per render-graph pass is the
render module's `NphRenderGpuProfiler` ([render.md](render.md)).

The work is split in two:

| Half | Where | What |
|---|---|---|
| Recording | `engine/core`, `profile.h` | `NPH_PROFILE_*` macros, per-thread rings of 32-byte events, the drain |
| Collecting | `engine/profile`, `profiler.h`, `memory.h` | merging, zone rebuilding per fiber, frames, statistics, Chrome trace export, tracked allocators |

Recording has to live in core: the module order (CLAUDE.md §4) puts
`profile` to the right of `jobs`, `gpu`, `render`, and `scene`, the code
most worth measuring, and a module cannot include anything to its right.
It follows the logger's pattern (also core state that is global by the
exception CLAUDE.md §2 allows for logging and profiling sinks).

## Recording

```c
NPH_PROFILE_FRAME(frame_index);       // the frame loop, as each frame starts
NPH_PROFILE_BEGIN("scene.transforms");
...
NPH_PROFILE_END();
NPH_PROFILE_COUNTER("render.draws", stats.draws);
```

- **Events.** Each call appends one event to the calling thread's
  single-producer ring: the CPU's time-stamp counter (RDTSC on x86-64,
  CNTVCT on ARM64), a static name, a 64-bit value, and a type (BEGIN, END,
  FRAME, FIBER, COUNTER). No lock, no allocation, no I/O. A zone costs
  17 ns (two events); a paused or uninitialised profiler costs 2.3 ns per
  zone.
- **Scopes.** C has no destructors, so a zone is an explicit BEGIN/END
  pair; every return path in between must end it. GCC's and Clang's
  `cleanup` attribute could close a scope automatically, but MSVC has no
  equivalent, so the plan's `NPH_PROFILE_SCOPE` is not offered (ADR-0022).
  The collector repairs unbalanced streams instead of trusting them.
- **Threads.** A thread's ring is created by `nph_profile_thread_attach`
  (which also names its lane in the timeline) or by its first event, which
  then allocates. `nph_profile_thread_detach` retires it; the collector
  frees it once drained. Job workers attach as `job-worker-N` and I/O
  threads as `job-io-N` when the job system starts.
- **Names** must stay valid until the event has been collected (normally
  the end of the frame). The collector interns every name it sees, so the
  history never points into memory that is freed later. Code that frees
  zone names (a game module being reloaded, a schedule being destroyed)
  calls `nph_profiler_forget_names` first: it processes every pending
  event, then forgets the pointers.
- **Builds.** In Shipping (`NPH_PROFILER_ENABLED == 0`) the macros expand
  to nothing and the functions record nothing. Elsewhere nothing records
  until `nph_profile_init`, and `nph_profile_set_enabled` pauses recording
  at run time.
- **Overflow.** A full ring drops the new event and counts it. The
  collector charges the loss to the frame in progress and closes the
  thread's open zones at the next frame boundary, since their ENDs may be
  among the lost events.

## Collecting

`nph_profiler_collect` runs once per frame on the main thread:

1. **Drain** every ring into a pending queue per lane (one lane per
   thread, in the order threads are first seen).
2. **Merge** the lanes by timestamp with a binary heap. Events newer than
   the settle window (2 ms by default) wait for the next collection, so an
   event that a thread stamped but had not pushed yet when the rings were
   drained is still processed in order. The invariant TSC that every
   supported CPU has (CLAUDE.md §3) keeps the counters of all cores in step.
3. **Rebuild** zones into *segments*: a segment is a piece of a zone that
   one thread ran without interruption (24 bytes: start, end, name, lane,
   depth, flags).

### Fibers

A job runs on a fiber and may park on a counter and resume on another
worker (docs/jobs.md). Zones are therefore kept on a stack *per fiber*, plus
one stack per thread for code outside fibers, and the job system reports
every switch (`NPH_PROFILE_FIBER` in `after_switch`). When a fiber leaves a
thread, the pieces its open zones ran there are emitted (flag `CONTINUES`);
where it resumes, they continue (flag `CONTINUED`):

```
worker 0  |-- job a ---|                      a parks on a counter
worker 1                    |- job a -|       a resumes on worker 1 and ends
```

Every segment is work one thread really did, so segments of one depth on
one lane never overlap (`test_profiler_jobs.c` checks it against the real
job system with migrating jobs).

### Frames

A FRAME event closes the frame in progress. Zones running at that moment
are cut there and continue in the next frame, so every segment lies inside
exactly one frame and a frame's segments are contiguous in the segment
ring. The history keeps `history_frames` complete frames (240 by default,
one second at the §10 target) and drops frames whose segments the ring has
overwritten.

### Repairs

Instrumented code is trusted to pair its calls, and streams are checked
anyway. Each repair is counted in `NphProfilerStats.anomalies`:

- an END with no open zone is dropped;
- a BEGIN past the depth limit (64) is dropped together with its END;
- a fiber reported on a second lane before its first lane reported leaving
  it (an ordering anomaly, or a lost event) is suspended where it was;
- a lane that lost events has its open zones closed at the next frame;
- names past the name table (1024) are unnamed ("?").

### Statistics and queries

Per zone name, a window of per-frame totals (all threads together, time
inside the zone) gives the last value, the mean per frame (frames without
the zone count as zero), the maximum, the frames it ran in, and calls per
frame (pieces that begin a zone, not continuations). Running sums are
recomputed once per trip around the window to bound floating-point drift.
Counters keep each frame's last sample (mean, minimum, maximum over the
frames that sampled them). Frames, their segments, frame times for a
graph, lanes, and names are all queryable.

### Chrome trace export

`nph_profiler_write_chrome_trace` writes the newest frames in the Trace
Event Format (Google, 2016): one complete event (`"ph":"X"`) per segment, a
global instant (`"ph":"i"`) per frame start, the lanes as named and sorted
threads, and the counters as counter events (`"ph":"C"`). Times are
microseconds from the first frame's start, written with integer formatting
so a second of frames exports in milliseconds.
`nph_profiler_save_chrome_trace` writes it to a file.

## Memory tracking (`memory.h`)

`NphTrackedAllocator` wraps another allocator and counts what passes
through it: bytes and allocations alive, their peaks, and the totals. The
runtime gives each subsystem its own tracked allocator over the shared
heap, and registers them with the profiler (`nph_profiler_track_memory`)
for the editor's memory panel. The counters are atomics, so the wrapper is
as thread-safe as the allocator it wraps; it adds 13 ns to a 64-byte
allocation and free (49 ns against 36 ns for the system allocator alone).

## What is instrumented

| Where | Zones |
|---|---|
| `jobs` | every job, named by its declaration (`NphJobDecl.name`; `parallel_for` pieces); fiber switches; worker and I/O thread lanes |
| `scene` | `scene.schedule` around a schedule run; each system run on the calling thread by its name (systems run as jobs are named by their job); each chunk piece of a `run_chunk` system |
| `gpu` | `gpu.rg.compile`, `gpu.rg.execute`, and each pass's command recording by pass name |
| `render` | `render.frame` |

The runtime (Phase 9) adds the frame loop's phases.

## Threading model

| What | Rule |
|---|---|
| Recording (`NPH_PROFILE_*`) | any thread; each writes its own ring |
| `nph_profile_init`, `shutdown`, the drain | one thread (the collector's) |
| `NphProfiler` | one thread: collect and query on the main thread |
| Tracked allocators | as thread-safe as their backing allocator; statistics readable anywhere |

## Costs (§1.4)

- A thread ring is `ring_capacity` × 32 bytes (16384 events, 512 KiB, by
  default), allocated at attach or at the first event.
- The collector allocates its tables at creation: with the defaults, about
  6 MiB of segments, 1 MiB of fiber stacks, and 3.7 MiB of statistics
  windows. A collection allocates only when a lane's pending queue grows or
  a lane or name appears.

## Performance

`benchmarks/profile/bench_profile.c`, Profile, Ryzen 7 5800X, MSVC 14.44,
2026-10-16, median of 7
(`benchmarks/results/profile-2026-10-16-win-msvc-profile-ryzen7-5800x.json`).

| Benchmark | ns/op | Note |
|---|---|---|
| `profile_zone_enabled` | 17.0 | a BEGIN/END pair into the ring |
| `profile_zone_paused` | 2.3 | recording paused at run time |
| `profile_zone_uninitialised` | 2.3 | before `nph_profile_init` |
| `profile_collect_per_event` | 15.0 | drain, merge, rebuild, statistics; 12 290 events per frame take 0.18 ms |
| `tracked_alloc_free_64` | 48.8 | a 64-byte allocation and free through a tracked allocator |
| `system_alloc_free_64` | 35.7 | the same, untracked |

## Testing

- `engine/core/tests/test_profile.c` (11 tests): the recording half on one
  thread: event order and contents, the drop count, capacity of a drain,
  naming and renaming, retirement and slot reuse, pausing, a full
  registry, re-initialisation with a thread that recorded before, and the
  compiled-out configuration.
- `engine/profile/tests/` (27 tests): nesting, frame cuts, fiber migration
  staged on two threads with exact timestamps, a thread stack suspended
  under a fiber, every repair, counters, sliding statistics, segment-ring
  eviction, interning across freed names, reset, the settle window, the
  lane limit, creation under allocation failure at every step, real threads
  merging into consistent lanes, the real job system with jobs that park
  and migrate, the Chrome trace parsed back as JSON, and tracked
  allocators under concurrency.
- CI runs the profile tests under ThreadSanitizer with the jobs and scene
  tests.

## Limitations and future work

- GPU work is not on the CPU timeline: pass times come two frames late
  without a common clock. `VK_EXT_calibrated_timestamps` (detected by the
  gpu module) would align them.
- No source locations per zone and no sampling profiler; the zones say
  where time goes at the granularity the code was instrumented.
- The crash handler and minidumps of §8.11 are Phase 15 work.
- ARM64's counter rate is read from the platform's measurement; the ARM64
  path is untested until ARM64 builds exist.

## References

- Google, "Trace Event Format", 2016 (the export).
- Taudul, *Tracy Profiler* manual, 2024: per-fiber zone stacks and fiber
  switch events, the model followed here.
- Gyrling, "Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015:
  why jobs migrate between threads.
- Intel, *Software Developer's Manual*, vol. 3B §18.17: the invariant TSC.
