# 0004. Allocator interface

- **Date:** 2026-10-11
- **Status:** Accepted

## Context

CLAUDE.md §1.4 and §2 require that every allocation be explicit: no
allocation happens unless the caller can see it, every subsystem receives its
allocator, and all memory goes through Nepheline allocators. §5 says that
fallible functions return `NphResult` with out-parameters and never return a
silently-null pointer. §8.1 lists the allocators to build (arena, frame
arena, pool, stack, TLSF, debug wrappers) behind one `NphAllocator` vtable.

Several details of that vtable shape the entire codebase, so they are fixed
here before any container or subsystem is written on top of it.

## Decision

1. **Raw allocation returns a pointer; NULL means "cannot satisfy".** The
   vtable's `alloc` and `realloc` return `void *`, and the entry points
   (`NPH_ALLOC`, `nph_alloc_at`, …) pass that through. This is the one
   documented exception to the §5 rule: an allocator has exactly one failure
   mode, NULL is its universal encoding in C, the result is not *silent*
   (it is the contract, and the debug paths assert on misuse), and the
   alternative, `NphResult alloc(…, void **out)`, forces a `(void **)&typed`
   cast at every typed call site that is undefined behaviour under strict
   aliasing. Everything built on allocators (containers, strings, readers)
   returns `NphResult` and maps NULL to `NPH_RESULT_OUT_OF_MEMORY`.
2. **Sized free.** `free(ptr, size, align)` and
   `realloc(ptr, old_size, new_size, align)` receive the size and alignment
   of the original request. Arenas, pools, and stacks therefore need no
   per-allocation header; the debug wrapper verifies the caller's claim; TLSF
   checks it against its own header when asserts are on.
3. **Explicit alignment on every call,** normalised to at least 16 bytes
   (`NPH_ALLOC_DEFAULT_ALIGN`, enough for every scalar and SSE type) and
   supported up to 4096 (`NPH_ALLOC_MAX_ALIGN`) by every allocator.
4. **Zero-size allocations are an error** (asserted, NULL returned), so
   allocators never have to invent a unique pointer for nothing.
5. **The callsite travels with the request.** `NphCallsite {file, line}` is
   captured by the `NPH_ALLOC*` macros and forwarded through the vtable so
   the debug wrapper can attribute leaks without stack-walking (which needs
   OS APIs that core must not use). Allocators that do not track it ignore
   it; the cost is two argument registers.
6. **Allocators embed `NphAllocator` as their first member** and hand out
   `&self->allocator`. The vtable functions cast `self` back. There is no
   separate user-data pointer and no heap-allocated allocator object.
7. **Allocators own their memory explicitly.** Each has `init` over a
   caller-owned block (nothing is allocated) and `init_from(backing, size)`
   that takes the block from another allocator and returns it in `destroy`.
   No allocator grows silently: an exhausted arena, pool, stack, or TLSF
   heap returns NULL. Growth is a visible call (`nph_tlsf_add_pool`).
8. **AddressSanitizer integration is built in.** Allocators poison the
   memory they own but have not handed out (`NPH_ASAN_POISON`), so in Debug
   builds a use of reset arena memory or a freed pool slot is reported at
   the faulting instruction. Consequence: memory given to `init` must be
   released with `destroy` before it is reused for anything else.
9. **Diagnostics are a wrapper, not a mode.** Guard bytes, leak tracking
   with callsites, and use-after-free poisoning live in `NphDebugAllocator`,
   which wraps any allocator. The production allocators carry no debug
   branches beyond asserts. The wrapper's live list is protected by a spin
   lock, acceptable for a diagnostic tool and documented as such.
10. **Thread-safety is per allocator and documented on the type.** The
    system allocator is thread-safe; arena, frame arena, pool, stack, and
    TLSF are single-threaded by design. Sharing a TLSF heap between threads
    (locked or per-thread heaps) is decided in Phase 3 with measurements.

## Alternatives considered

- **`NphResult`-returning allocation** — consistent with §5 but verbose at
  every call and UB-adjacent at typed call sites (see 1).
- **Unsized free (`free(ptr)`)** — the C convention; requires headers in
  every allocator and loses the free verification the debug wrapper gives.
- **Implicit alignment (always 16)** — simpler, but SIMD-wide types, cache
  line padding, and GPU staging buffers need more; passing it is cheaper
  than a second API.
- **Growable arenas (chained blocks)** — convenient, but a hidden
  allocation inside what callers treat as a bump pointer violates §1.4.
  Phase 3 adds an arena over reserved virtual memory that commits on demand,
  which has no hidden heap cost.
- **Stack-walked callsites in the debug wrapper** — more informative, but
  needs `CaptureStackBackTrace`/`backtrace` and symbolisation, which belong
  to `platform` and `profile`; the callsite macro costs nothing and is
  available in core.
- **A global default allocator** — forbidden by §2; `nph_allocator_system()`
  exists only to obtain the backing blocks of the engine's own allocators
  and for tools.

## Consequences

- Every function that allocates takes an `NphAllocator *`; containers store
  the allocator they were created with.
- Callers must keep the size and alignment of every allocation (containers
  naturally do; ad-hoc code uses the typed `NPH_ALLOC_TYPE`/`NPH_FREE_TYPE`
  macros so the numbers are derived, not remembered).
- Tests that build an allocator over a stack buffer must destroy it before
  returning, or ASan reports the next function to occupy that stack range.
- The raw-pointer rule is confined to the allocator layer; the §5 rule
  applies unchanged to everything above it.
