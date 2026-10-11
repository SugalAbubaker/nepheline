# Core module (`engine/core`)

The foundation every other module is built on (CLAUDE.md §8.1). Core depends
only on the C standard library and compiler intrinsics; it includes no OS
header. This document grows with the module; each section names the headers
it covers.

Status: **base, errors, assertions, atomics, and allocators are done**;
containers, strings, hashing, logging, cvars, and serialization follow in
the remaining Phase 1 pull requests.

## Base (`types.h`, `base.h`, `version.h`)

- Fixed-width scalars `u8…u64`, `i8…i64`, `f32`, `f64`, `b32`, `uptr`, `iptr`.
  Engine APIs express byte counts as `u64` and element counts as `u32`/`u64`;
  `size_t` appears only at C library boundaries (§5).
- Detection macros are always defined as 0 or 1 so `#if NPH_X` is safe under
  `-Wundef`: `NPH_COMPILER_{MSVC,GCC,CLANG}`, `NPH_MSVC_ABI` (clang-cl),
  `NPH_PLATFORM_{WINDOWS,LINUX}`, `NPH_ARCH_{X64,ARM64}`, `NPH_ASAN_ENABLED`,
  and the configuration flags from CMake (`NPH_CONFIG_*`,
  `NPH_ASSERTS_ENABLED`, `NPH_PROFILER_ENABLED`, `NPH_GPU_VALIDATION_ENABLED`).
- Portable attributes: `NPH_FORCE_INLINE`, `NPH_NO_INLINE`, `NPH_NORETURN`,
  `NPH_RESTRICT`, `NPH_ALIGNAS(n)`, `NPH_ALIGNOF(T)`, `NPH_MUST_USE`,
  `NPH_THREAD_LOCAL`, `NPH_PRINTF_LIKE`, `NPH_LIKELY`/`NPH_UNLIKELY`,
  `NPH_DEBUG_BREAK`, `NPH_UNREACHABLE_HINT`. Thread-safety annotations
  (`NPH_THREADSAFE`, `NPH_MAIN_THREAD_ONLY`, `NPH_RENDER_THREAD_ONLY`) are
  documentation that expands to nothing.
- Bit operations over intrinsics: popcount, clz/ctz, floor/ceil log2,
  power-of-two tests and rounding, rotates, byte swaps. Alignment helpers
  for integers and pointers. Typed min/max/clamp as inline functions so
  arguments are evaluated once.

## Errors (`result.h`)

`NphResult` is the return type of every fallible function; results come
back through out-parameters (§5). Values are grouped by what the caller can
do about them: `OUT_OF_MEMORY`, `INVALID_ARGUMENT`, `NOT_FOUND`,
`ALREADY_EXISTS`, `FULL`, `EMPTY`, `OVERFLOW`, `UNSUPPORTED`, `CORRUPT`,
`VERSION_MISMATCH`, `TYPE_MISMATCH`, `PARSE_ERROR`, `END_OF_STREAM`,
`IO_ERROR`, `UNKNOWN`. `nph_result_string` names them for logs;
`NPH_TRY(expr)` propagates failures. The one exception, raw allocation
returning NULL, is [ADR-0004](adr/0004-allocator-interface.md).

## Assertions (`assert.h`)

`NPH_ASSERT`, `NPH_ASSERT_MSG`, `NPH_VERIFY` (condition always evaluated),
`NPH_VERIFY_MSG`, and `NPH_UNREACHABLE`. With `NPH_ASSERTS_ENABLED` (Debug
and Development) a failure formats its message and calls the installed
handler with file, function, line, condition text, and message; the handler
returns `ABORT` (default: message on stderr, then a non-interactive
`abort()`), `BREAK` (debugger trap), or `CONTINUE` (tests). Without asserts
the condition of `NPH_ASSERT` is not evaluated at all and `NPH_UNREACHABLE`
becomes an optimiser hint. The handler is process-wide diagnostic state, in
the category §2 exempts from the no-globals rule.

Tests observe expected failures through `NPH_TEST_EXPECT_ASSERT(stmt)`
(`engine/core/tests/test_support.h`), which installs a counting handler. The
code under test must take its documented fallback (return NULL, ignore the
call) after a failed assertion so the same test passes in configurations
without asserts.

Known hazard: a variable used only inside an assertion is unreferenced in
Profile and Shipping builds and fails `-Werror`; mark it `NPH_UNUSED` after
the assertion. CI builds Shipping on two toolchains to catch this.

## Atomics (`atomic.h`)

C11-style atomics over compiler intrinsics with explicit memory orders:
`NphAtomicU32`, `NphAtomicU64`, `NphAtomicPtr` (struct-wrapped so plain
access is a type error); load, store, exchange, compare-exchange, fetch
add/sub/or/and; thread and signal fences; `nph_cpu_relax`. The MSVC
implementation is x86-64 only and mirrors the MSVC STL's mapping (acquire
and release are compiler barriers on TSO hardware; `seq_cst` stores use
`xchg`; RMW operations are full barriers). GCC/Clang use the `__atomic`
builtins. A test-and-test-and-set `NphSpinLock` exists for diagnostic code
only. Why atomics are in core rather than platform:
[ADR-0005](adr/0005-atomics-in-core.md).

## Allocators

The interface and its contract are fixed in
[ADR-0004](adr/0004-allocator-interface.md): a vtable `{alloc, realloc,
free}` plus a name, embedded as the first member of each allocator; sized
free; explicit alignment normalised to at least 16 and supported to 4096;
NULL on exhaustion; callsites forwarded for leak attribution; no silent
growth; ASan poisoning of memory the allocator owns but has not handed out.

| Allocator | Header | Alloc | Free | Use |
|---|---|---|---|---|
| system | `alloc.h` | malloc / `_aligned_malloc` | yes | backing blocks for the others; tools |
| arena | `arena.h` | bump, O(1) | top block only; bulk via markers/reset | per-task scratch, level loads |
| frame arena | `arena.h` | bump into the frame's arena | automatic after `frame_count` frames | per-frame data (§10.5) |
| pool | `pool.h` | free list then bump index, O(1) | O(1), intrusive list | fixed-size objects: entities, handles, nodes |
| stack | `stack_alloc.h` | bump + 16-byte header, O(1) | LIFO only, asserted | nested temporaries (parsers, traversals) |
| TLSF | `tlsf.h` | two-level segregated fit, O(1) | O(1), immediate coalescing | general-purpose heap |
| debug | `alloc_debug.h` | wraps any allocator | verifies guards, poisons | leak and overrun detection in Debug/Development |

### Arena and frame arena

The arena bumps a cursor through one block. `nph_arena_mark` /
`nph_arena_reset_to` give scoped bulk release; `peak` records the high-water
mark for sizing. Through the vtable, `free` reclaims only the most recent
allocation and `realloc` grows it in place, so an arena doubles as a cheap
builder allocator. The frame arena is `frame_count` arenas (2 to 4) used
round-robin: `nph_frame_arena_begin_frame` advances and resets the arena
that becomes current, so data allocated in frame *F* stays valid until
frame *F + frame_count* begins, long enough for a render thread or an
upload path to consume it (§10.5 "zero heap allocations per frame outside
frame arenas"). Lock-free sharing of a frame arena between jobs is a Phase 4
addition if measurements call for it; today each job gets its own.

Phase 3 adds an arena over reserved virtual memory that commits pages on
demand; it keeps the no-hidden-growth property (reservation is explicit and
free) while removing the need to guess sizes.

### Pool

Slots of one size and alignment from one block. Freed slots form an
intrusive singly-linked list; never-used slots come from a bump index, so
`init` does not touch the block and a mostly-unused pool costs no page
faults. `nph_pool_owns` rejects interior and foreign pointers, which the
free path asserts. The vtable path promises 16-byte alignment, so pools used
through `NphAllocator` are initialised with `elem_align >= 16`; the direct
API serves tighter pools (8-byte handles) without that overhead.

### Stack

LIFO allocation with a 16-byte header (`prev_offset`, `size`) before each
block, so popping restores the exact previous top, including alignment
padding, and out-of-order pops are detected. `realloc` of the top block
grows in place; of any other block it is refused (asserted, NULL) rather
than silently leaking the old block.

### TLSF

The engine's general-purpose allocator implements Masmano, Ripoll, Crespo
and Real, *"TLSF: a New Dynamic Memory Allocator for Real-Time Systems"*
(ECRTS 2004). Free blocks are indexed by a first level `fl = floor(log2
size)` and a second level that divides each power-of-two range into
`2^SL_LOG2 = 32` linear sub-ranges; sizes below 512 bytes map linearly in
16-byte steps into `fl = 0`. Two bitmaps (`fl_bitmap`, one `sl_bitmap` per
first level) make "find the smallest class ≥ request that is non-empty" two
`ctz` instructions. A search rounds the request up to the next class
boundary first (`mapping_search`), which guarantees any block in the found
class fits, so allocation never scans a list: it is O(1) in the strict
sense the paper proves. One departure: when that search fails, the list of
the request's *own* class is scanned for a first fit before returning NULL,
because the rounding otherwise refuses a request that exactly matches the
largest free block (asking for all of a fresh pool). Only the failure path
pays, and the scan is bounded by the free blocks in one class.

Block layout, chosen for 16-byte payload alignment: a 16-byte header
`{prev_phys, size | flags}` precedes every payload; sizes are multiples of
16; the minimum payload is 16 bytes (free-list links live there when the
block is free), so the minimum block span is 32 bytes and the maximum block
is 2^40 bytes. The classic trick of overlapping `prev_phys` with the
previous block's payload tail saves nothing at 16-byte alignment, so the
back link is always valid, which makes validation and coalescing simpler.
Flags: bit 0 "this block is free", bit 1 "the previous physical block is
free" (so `free` coalesces backwards without touching the neighbour's cache
line unless it will merge). A pool is one region ending in a zero-size
sentinel header; up to 16 pools may be added and removed (removal requires
the pool to be entirely free). Over-aligned requests allocate with enough
slack to split the leading gap off as a free block.

Validation (`nph_tlsf_validate`) walks every pool and every free list and
checks each invariant: header alignment, size multiples, back links, flag
consistency, no adjacent free blocks, membership of every free block in
exactly the list its size maps to, bitmap agreement, and the live counters.
The unit tests call it after every operation of randomised
allocate/free/realloc sequences whose payloads carry position-dependent
patterns, so any overlap or corruption is caught.

Thread-safety is deliberately not built in; the Phase 3 decision between a
locked shared heap and per-thread heaps is made with the job system's
workload in hand.

### Debug wrapper

`NphDebugAllocator` wraps any allocator. Each allocation gets a 96-byte
header (list links, inner block, callsite, size, alignment, sequence
number, magic) ending in a 16-byte guard, and a 16-byte trailing guard. New
memory is filled with `0xCD`, freed memory with `0xDD`; `free` verifies
magic, both guards, and the caller's size and alignment, then reports a
double free by the freed magic. `realloc` always moves so the old block is
poisoned. The live list (spin-locked) feeds `nph_debug_allocator_report`
(leaks with file and line) and `nph_debug_allocator_check` (guard sweep).

### ASan integration

Every allocator poisons the memory it owns and has not handed out and
unpoisons on allocation (`NPH_ASAN_POISON`/`NPH_ASAN_UNPOISON`, no-ops
without ASan). TLSF poisons free payloads past the list links, the pool
poisons free slots past the link word, arenas and stacks poison everything
above the cursor. This turns "read from a reset arena" and "use after pool
free" into immediate ASan reports in Debug builds. The one rule it imposes:
destroy an allocator before its block is reused for anything else, or the
poison outlives it (a stack buffer handed to an allocator and never
destroyed trips the next function to occupy that address).

### Allocator benchmarks

`benchmarks/core/bench_alloc.c`, run with `--min-time=0.5 --repeats=7` in
the Profile configuration on the development machine (Ryzen 7 5800X,
Windows 10, MSVC 14.44, 2026-10-11). Full JSON:
`benchmarks/results/core_alloc-2026-10-11-win-msvc-profile-ryzen7-5800x.json`.
Numbers are nanoseconds per operation, median of 7 runs.

| Workload | system (UCRT malloc) | TLSF | debug over TLSF | pool | arena | stack |
|---|---|---|---|---|---|---|
| alloc + free 64 B, through `NphAllocator` | 35.5 | 32.8 | 42.3 | 14.5 | – | – |
| alloc + free 64 B, direct API | 29.6 | 28.7 | – | 5.0 | 2.5 (bump) | 3.0 (push + pop) |
| batch: 4096 allocs of 16–1024 B, then free all (per op) | 33.6 | 15.9 | 30.3 | – | – | – |
| random mixed: 1024 live slots, 16–1024 B (per op) | 27.2 | 20.7 | 37.2 | – | – | – |
| realloc grow 16 B → 1 MiB by doubling (per step) | 6974 | 21.7 | – | – | – | – |

What the numbers say:

- **TLSF matches malloc on the tight loop and beats it on realistic
  traffic**: 2.1× on the batch workload and 1.3× on steady-state mixed
  traffic, with the O(1) bound the paper proves and immediate coalescing. The
  UCRT allocator is a reasonable baseline; the gap is in the workloads with
  many live blocks, where TLSF never searches.
- **In-place growth is the big win**: `realloc` into a free successor costs
  the same as an allocation, where the system allocator copies (320× here).
  Dynamic arrays growing in a TLSF heap benefit directly.
- **The specialised allocators earn their place**: pool 6×, stack 10×, and
  arena 12× cheaper than the general heap on the direct API, which is why
  per-frame and per-object data never go through TLSF (§2, §10.5).
- **The generic interface costs ~4 ns** per call (entry-point checks,
  alignment normalisation, vtable dispatch), visible by comparing the two
  64 B rows. Hot paths that own their allocator call it directly.
- **The debug wrapper costs 10–15 ns per op** (two fills, guard checks, the
  spin lock), affordable in Debug and Development.

## Testing

`engine/core/tests/`: `test_base.c` (bit tricks, alignment, min/max,
results), `test_assert.c` (handler protocol, message formatting, verify
semantics), `test_atomic.c` (single-threaded semantics of every operation
and order, spin lock), `test_alloc.c` (system allocator, typed macros,
realloc fallback via a failing allocator), `test_arena.c`, `test_pool.c`,
`test_stack_alloc.c`, `test_tlsf.c` (geometry, coalescing in every
direction, over-alignment, in-place realloc, pools, exhaustion, double free,
three randomised model checks with full validation), `test_alloc_debug.c`
(leaks with callsites, overrun/underrun, size/alignment mismatch, poison,
double free). Multi-threaded tests of atomics and the debug wrapper's lock
arrive with platform threads in Phase 3.
