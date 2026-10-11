# Core module (`engine/core`)

The foundation every other module is built on (CLAUDE.md §8.1). Core depends
only on the C standard library and compiler intrinsics; it includes no OS
header. This document grows with the module; each section names the headers
it covers.

Status: **complete for Phase 1** (base, errors, assertions, atomics,
allocators, hashing, containers, strings, logging, cvars, serialization).

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

## Hashing (`hash.h`)

- **`nph_hash64`**: a 64-bit non-cryptographic hash following the wyhash
  construction (Wang Yi): the mixing primitive is a 64×64→128 multiply
  whose halves are folded; 48 bytes per step in three independent lanes;
  inputs of 4–16 bytes are read with two overlapping loads from each end,
  1–3 bytes with first/middle/last, so there is no byte loop. Constants are
  our own (golden ratio, √2, √3, √5). Tests: pinned regression vectors,
  avalanche (every output bit flips 42–58 % of the time for every input bit,
  lengths 1–200), bucket distribution of sequential keys, alignment
  independence.
- **`nph_hash_mix_u64`**: the `moremur` finaliser (Evensen 2019), bijective,
  for integer keys; `nph_hash_combine`, `nph_hash_fold_u32`.
- **`nph_crc32c`**: Castagnoli CRC with the SSE4.2 instruction eight bytes
  at a time (the x86-64 baseline guarantees it); `nph_crc32c_portable` is
  the table-driven reference, cross-checked for every length 0–299 at every
  alignment. Standard vectors (RFC 3720) pass.
- **Stability**: both hashes are part of the asset format (cooked string
  IDs and checksums). Changing either is a format version change.

## Containers

Decisions and alternatives: [ADR-0006](adr/0006-containers.md).

| Container | Header | Shape | Complexity |
|---|---|---|---|
| dynamic array | `array.h` | `NPH_ARRAY(T)` struct + uppercase macros over an untyped `NphArrayRef` | push amortised O(1), doubling from 8 |
| hash map | `hashmap.h` | Swiss table: control bytes + AoS slots, SSE2 group probe | O(1) expected; 7/8 load |
| handle pool | `handle.h` | 32-bit index + generation, FIFO reuse | O(1) |
| SPSC ring | `ring.h` | Lamport queue with cached positions | O(1), lock-free |
| MPMC ring | `ring.h` | Vyukov bounded queue | O(1), lock-free |
| bitset | `bitset.h` | u64 words, zero tail invariant | O(words) |
| sparse set | `sparse_set.h` | sparse index + dense keys (+ values) | O(1) |

**Dynamic array.** `typedef NPH_ARRAY(T) Name;` declares `{T *data; u32
len, cap; NphAllocator *alloc;}`. Operations are macros (`NPH_ARRAY_PUSH`,
`_PUSH_SLOT`, `_PUSH_N`, `_INSERT`, `_POP`, `_LAST`, `_REMOVE_AT`,
`_REMOVE_SWAP`, `_RESERVE`, `_RESIZE`, `_SHRINK_TO_FIT`, `_FOREACH`) that
assign through `data[i]` for type safety and forward memory work to
functions on `NphArrayRef`. The `data` field is read and written through
`memcpy` in those functions, so no pointer type is punned. Growth uses
`NPH_REALLOC`, which TLSF grows in place when it can. Element alignment is
16, or 32/64 for power-of-two element sizes of 32/64 (SIMD register types),
since `_Alignof` cannot be applied to an expression in C17.

**Hash map.** See ADR-0006 for the layout. `nph_hashmap_desc_u64` gives
integer-keyed maps the mixer as hash; other keys use `nph_hash64` over the
key bytes and `memcmp` unless the descriptor supplies functions. Value
pointers are stable until the next mutating call. Iteration walks the
control bytes.

**Handle pool.** Issues `NphHandle {u32}` with a per-pool index width
(1–28 bits). Generations start at 1 and wrap to 1, so zero is never valid;
released indices queue in a FIFO and are reused only after
`min_free_before_reuse` other releases. The pool stores no payload.

**Rings.** Both copy fixed-size elements by value and never allocate after
init. SPSC keeps the producer's and consumer's positions on separate cache
lines and caches the remote position so a push or pop touches one shared
line only when the ring looks full or empty. MPMC cells carry sequence
numbers; positions are 32-bit and wrap safely (tested).

**Bitset and sparse set.** The bitset keeps tail bits zero so `count`,
`all`, and `equals` are whole-word operations; `find_next_set/clear` scan
with `ctz`. The sparse set grows its sparse array to the largest key
(rounded to a power of two) and keeps members packed in `dense` with an
optional parallel value array, swapping the last member into a removed
slot.

### Hashing and container benchmarks

`benchmarks/core/bench_hash.c`, Profile, Ryzen 7 5800X, MSVC 14.44,
2026-10-11, median of 5. Full JSON:
`benchmarks/results/core_hash-2026-10-11-win-msvc-profile-ryzen7-5800x.json`.

| Benchmark | ns/op | Note |
|---|---|---|
| `nph_hash64`, 8 / 16 / 32 / 64 B | 3.9 / 3.9 / 5.4 / 6.4 | short-input path |
| `nph_hash64`, 256 B / 4 KiB / 64 KiB | 12.4 / 133 / 2076 | 31.6 GB/s on long inputs |
| `nph_crc32c`, 8 / 64 B / 4 KiB / 64 KiB | 2.9 / 5.4 / 318 / 5072 | 12.9 GB/s: one dependent `crc32` chain |
| `nph_hash_mix_u64` | 2.5 | |
| hash map insert, 1 k / 100 k / 1 M u64 keys | 25.5 / 30.0 / 55.4 | includes rehashes from empty |
| hash map find hit, 1 k / 100 k / 1 M | 7.7 / 12.3 / 40.4 | L1 / L2 / DRAM resident |
| hash map find miss, 1 k / 100 k / 1 M | 5.1 / 8.7 / 8.8 | one group load, no key compare |
| hash map remove, 1 k / 100 k / 1 M | 8.0 / 12.3 / 40.7 | |
| array push u32 (100 k pushes, with growth) | 0.81 | inline fast path; 16.5 before it, when every push called the grow function |
| sparse set set + get, 4096 keys | 3.8 | per operation |
| bitset count + find, 1 M bits | 3147 | per call pair; 15 625 words |

Reading: the map's miss path is faster than its hit path because a miss
usually ends at the first group (an empty byte) without touching a slot; the
1 M-entry numbers are DRAM latency, as expected for a 16 MiB table. CRC32C
is latency-bound on the single `crc32` dependency chain; a three-stream
version with a combine step (Gopal et al., Intel 2011) would reach ~3× and
is worth adding when file checksums show up in profiles.

## Strings (`string.h`, `string_id.h`)

**Views.** `NphStr {const char *data; u64 len}` is the string type of every
engine API: non-owning, not necessarily NUL-terminated, UTF-8 bytes.
`NPH_STR("literal")` makes one at compile time; `NPH_STR_FMT`/`NPH_STR_ARG`
print one. Slicing, comparison (byte-wise, plus ASCII case-insensitive),
search, trimming, `nph_str_split_first` for separator loops, and strict
parsers for integers (overflow-checked), floats (whole-string `strtod`),
and booleans. Ownership is explicit: `nph_str_dup` returns an owned
NUL-terminated copy from an allocator.

**UTF-8.** `nph_utf8_decode` walks code points, yielding U+FFFD and
advancing one byte on an invalid sequence (overlong forms, surrogates,
truncation, bytes above 0xF7); `nph_utf8_encode`, `nph_utf8_is_valid`,
`nph_utf8_count`. No locale, no tables.

**Builder.** `NphStrBuilder` grows with `NPH_REALLOC` (doubling from 32
bytes) and always keeps a NUL after its contents, so `nph_sb_cstr` is free.
Appends for views, C strings, characters, integers (no printf), `%.17g`
floats, printf formats (two-pass `vsnprintf`), and code points.

**String IDs.** An `NphStrId` is the `nph_hash64` of the text (hash 0 is
remapped to 1 so zero means "none"). Equal strings give equal IDs with no
table involved, so IDs are computed anywhere and compared as integers, and
cooked assets store them. `NphStrTable` optionally interns text behind IDs
for diagnostics and text serialisation: text is copied into 64 KiB chunks,
the map is id → view, and interning the same text twice is a lookup. A
collision between different texts is detected on intern and reported as
`NPH_RESULT_CORRUPT` (asserted); at 2^-64 per pair it is a content bug to
fix, not a case to handle.

## Logging (`log.h`)

The split with the platform layer is [ADR-0005](adr/0005-atomics-in-core.md):
core owns rings, formatting, sinks, and `nph_log_flush`; the runtime's logger
thread (Phase 3) calls flush periodically.

- **Categories** are static objects (`NPH_LOG_CATEGORY_DEFINE(ident, "name",
  default_level)`) with an atomic minimum level; a global minimum applies on
  top. The `NPH_LOG_*` macros check both with relaxed loads before touching
  arguments. `TRACE` and `DEBUG` compile out of Shipping.
- **Records** are fixed 256-byte structs: UTC timestamp (C11
  `timespec_get`), category and file pointers (static strings), line, level,
  a 1-based log thread index, a per-thread sequence number, a count of
  records dropped before this one, and a 216-byte NUL-terminated message
  (longer messages end in `...`).
- **Rings.** Each thread owns an `NphSpscRing` of records, allocated from the
  logger's allocator on `nph_log_thread_attach` (or implicitly on the first
  write) and registered in a fixed slot table by compare-exchange. A write
  is a `vsnprintf` into a stack record and one ring push: no lock, no
  allocation, no I/O. A full ring drops the record and increments the
  thread's drop counter; the next record that fits carries the count, and
  the formatter prints "[N earlier records dropped]".
- **Flush** takes a single-flusher flag, then repeatedly picks the oldest
  head record across all rings (k-way merge by timestamp) and delivers it
  to every sink whose minimum level allows, bounded by `max_records`.
  Detached threads' rings are freed once drained.
- **Sinks** are `(fn, user, min_level)` triples (up to 8) behind a spin
  lock that is held only to copy the table. Built-in: console (stdout
  below WARN, stderr from WARN, optional ANSI colour, location always shown
  from WARN) and file (append, flushed on ERROR). The editor sink is a
  callback the editor registers.
- **Before init**, or if a thread cannot attach, a record is formatted and
  written to stderr synchronously, so early failures are never lost.

### String and logging benchmarks

`benchmarks/core/bench_log.c`, Profile, Ryzen 7 5800X, MSVC 14.44,
2026-10-11, median of 5
(`benchmarks/results/core_log-2026-10-11-win-msvc-profile-ryzen7-5800x.json`).

| Benchmark | ns/op | Note |
|---|---|---|
| `NPH_LOG_INFO` with two integer arguments, stored | 146 | `vsnprintf` into the record is the cost; the ring push is a few ns |
| `NPH_LOG_DEBUG` below the category level | 1.9 | two relaxed loads; arguments not evaluated |
| flush, per record delivered to one sink | 12.6 | k-way merge over 8 thread slots plus the sink call |
| builder append of a 6-byte view | 4.0 | |
| builder `append_u64` | 11.5 | no printf |
| builder `appendf("%u;")` | 77 | two-pass `vsnprintf` |
| `nph_str_find`, 6-byte needle in 64 bytes | 29 | `memchr`-free scan; a two-way or SIMD search is a later option |
| `nph_str_id` of 16 bytes | 5.3 | |

The 146 ns per stored record means a thread can log ~7 000 records per
millisecond; at 240 FPS the budget is one record per ~35 µs of frame time,
so logging in hot loops is still wrong (and the rings would drop), but a
few dozen records per frame cost nothing measurable. Formatting at the
call site (rather than deferring to the flusher) is deliberate: the record
is self-contained, arguments do not need to outlive the call, and the
flusher never touches caller memory.

## CVars (`cvar.h`)

A cvar is a static object defined next to the code that reads it
(`NPH_CVAR_BOOL/INT/FLOAT/STRING(ident, "name", default, [min, max],
description, flags)`) and registered explicitly with an `NphCvarRegistry`
owned by the engine context, so there is no global list (§2). Reads are
plain loads (`nph_cvar_get_int(&cv)`); writes go through the registry,
which type-checks, clamps numbers to their range, copies strings into
registry-owned storage, honours `READ_ONLY` and `CHEAT` (gated by the
registry's cheat switch), bumps a version counter the editor can poll, and
calls the cvar's change callback when the value actually changed. Names
are `[a-z0-9._]+` (asserted). Config text (`name = value`, `#` comments,
optional quotes) and command-line assignments apply through the registry;
values for names registered later are held pending and applied on
registration, and `nph_cvar_write_config` emits the `SAVED` set sorted by
name. Decisions: [ADR-0007](adr/0007-serialization-formats.md).

## Serialization (`serialize.h`, `json.h`)

**Binary.** `NphBinWriter` appends little-endian scalars, blobs, and
length-prefixed strings to a growable buffer and can back-patch; it writes
documents as `{magic, version}` + `{tag, size, payload}` sections + a
CRC-32C trailer. `NphBinReader` is a bounds-checked cursor over immutable
bytes: every read returns `NPH_RESULT_END_OF_STREAM` rather than reading
past the end, strings come back as views, headers are magic- and
range-checked, sections report `NPH_RESULT_CORRUPT` when their size
exceeds the data, and unknown sections are skipped by size. That is what
"versioning" means here: old readers survive new writers.

**JSON.** Strict RFC 8259 (escapes with surrogate pairs, no comments or
trailing commas) parsed by recursive descent into a tree whose nodes and
unescaped strings live in 16 KiB chunks owned by the `NphJsonDoc`; depth
is capped at 128 and numbers at 64 characters so hostile input cannot
exhaust the stack. Errors carry line, column, and a static message.
Numbers keep both the `f64` and, when lexically integral and in range, the
`i64`. Member access by key, element access by index, lenient getters with
fallbacks, strict `as_*` conversions, a builder API, a tree writer (compact
or two-space pretty, non-finite numbers as `null`), and a streaming writer
that validates nesting. The design choices are in
[ADR-0007](adr/0007-serialization-formats.md).

**Fuzzing.** `tests/fuzz/` holds libFuzzer targets for the JSON parser
(with a write-then-parse fixed-point check), the binary reader, UTF-8
decoding, and cvar config; they build with `-DNPH_BUILD_FUZZERS=ON` under
Clang and run nightly (`.github/workflows/fuzz.yml`) from seed corpora in
`tests/fuzz/corpus/`.

**Sample.** `samples/core_hello` ties the module together: console
logging, a debug-wrapped TLSF heap, cvars from the command line, a scene
written as pretty JSON and as a checksummed binary document and read back
from both, handles and interned names, and a leak check as the exit code.
CTest runs it as `sample.core_hello`.

### Serialization benchmarks

`benchmarks/core/bench_json.c`, Profile, Ryzen 7 5800X, MSVC 14.44,
2026-10-11, median of 5
(`benchmarks/results/core_json-2026-10-11-win-msvc-profile-ryzen7-5800x.json`).

| Benchmark | per op | Throughput | Note |
|---|---|---|---|
| JSON parse, 20 000 entities, 1.73 MB | 11.0 ms | 158 MB/s | 220 k nodes; `strtod` for 60 k floats is the largest single cost |
| JSON write, tree, compact | 13.2 ms | 131 MB/s | `%.17g` for 60 k floats dominates |
| JSON write, streaming | 18.1 ms | 96 MB/s | per-item nesting checks plus the same float formatting |
| binary write, 20 000 entities + CRC, 820 KB | 0.89 ms | 0.92 GB/s | |
| binary read + CRC verify | 0.27 ms | 3.0 GB/s | bounds-checked cursor; strings are views |
| cvar config apply, per `name = value` line | 121 ns | | trim, split, parse, set |

Reading: text is 12–40× more expensive than binary for the same data, which
is why scenes are cooked. Float formatting and parsing through the C
library are the JSON hot spots; a Ryu/Eisel-Lemire pair would roughly halve
both and is worth doing when editor save/load times show up in profiles.
The streaming writer's extra cost over the tree writer is the per-call
nesting validation and separator logic; it exists to avoid building a tree,
not to be faster.

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
double free), `test_hash.c` (regression vectors, avalanche, distribution,
CRC vectors and hardware/portable agreement), `test_array.c`,
`test_hashmap.c` (growth to 100 k, tombstone reclamation under churn,
randomised model check, byte keys, sets, allocation failure),
`test_handle.c` (staleness, reuse delay, generation wrap, randomised),
`test_ring.c` (order, capacity, position wrap-around for both rings),
`test_bitset.c`, `test_sparse_set.c` (randomised model check). Container
tests run under the debug allocator so a leaked allocation fails the test.
Multi-threaded tests of atomics, rings, and the debug wrapper's lock arrive
with platform threads in Phase 3.
