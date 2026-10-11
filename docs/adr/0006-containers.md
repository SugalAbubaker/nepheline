# 0006. Core containers: representation and the hash map design

- **Date:** 2026-10-11
- **Status:** Accepted

## Context

CLAUDE.md §8.1 lists the core containers (dynamic array, open-addressing
hash map "Robin Hood or Swiss-table style, SIMD probe", generational handle
pool, SPSC and MPMC rings, bitset, sparse set) and §2 asks for
data-oriented, cache-aware structures with no per-object virtual dispatch
in hot loops. C has no templates, so the way a container is made generic
decides how every later module reads and performs.

## Decision

1. **Generic containers are untyped at the implementation level and typed
   at the call site.** The dynamic array is a macro-defined struct
   (`NPH_ARRAY(T)`) whose operations are uppercase macros that assign
   through `data[i]` (type-checked by the compiler) and forward memory work
   to functions taking an `NphArrayRef` view (element size, pointers to the
   fields). The hash map, sparse set, and rings take `key_size`/`val_size`/
   `elem_size` at init and move bytes with `memcpy`; the hash and equality
   functions are supplied at init (defaults: `nph_hash64` and `memcmp`).
   Function pointers are called once per lookup, never per probe step, so
   the dispatch cost is bounded and visible.
2. **Counts and capacities are `u32`.** Four billion elements per container
   is the practical limit for anything that lives in RAM on the reference
   machine; the halved header size matters more.
3. **Hash map: Swiss-table layout** (Kulukundis, CppCon 2017; Abseil
   `flat_hash_map`), not Robin Hood. One control byte per slot holds
   empty/deleted or 7 hash bits; a 16-byte SSE2 group compare finds
   candidates, so a probe touches one control line and almost always one
   slot. Robin Hood needs per-slot distance metadata, more writes on
   insert, and gains nothing on lookup once SIMD filtering exists. Load
   factor 7/8; tombstones on erase, reclaimed by an in-place rehash when the
   table is less than half full of live entries, doubling otherwise. Keys
   and values share a slot (array-of-structs) so a hit costs one line.
   The control array carries a 16-byte mirrored tail so group loads never
   branch on wrap-around.
4. **Handles are 32 bits with a per-pool split** between index and
   generation (1–28 index bits, at least 4 generation bits). Zero is never
   valid (generations start at 1), released indices wait in a FIFO and are
   reused only after a configurable number of other releases, and the pool
   issues indices only; storage is the caller's structure-of-arrays.
5. **Rings are bounded, fixed-element, and allocation-free after init.**
   SPSC: Lamport queue with cached remote positions on separate cache lines
   (FastForward, Giacomoni et al., PPoPP 2008). MPMC: Vyukov's bounded
   queue with per-cell sequence numbers. Both use the atomics of ADR-0005.
6. **Bitset keeps its tail bits zero** as an invariant so counting,
   comparison, and "all set" work on whole words. **Sparse set** follows
   Briggs & Torczon (1993): dense keys plus a parallel optional value
   array, which is the ECS component storage pattern.
7. **Hash functions are part of the asset format.** `nph_hash64` (a
   wyhash-construction hash with our own constants) and `nph_crc32c` are
   pinned by regression vectors; changing either changes the asset format
   version. The integer mixer (`moremur`) is used for integer keys.

## Alternatives considered

- **Robin Hood hashing** (listed in §8.1 as the alternative): simpler
  control structure, but lookups cost one key comparison per probe step
  and inserts shift runs; Swiss-table's byte filter does the same work with
  one SIMD compare per 16 slots.
- **Macro-generated typed containers** (`NPH_HASHMAP_DEFINE(name, K, V)`):
  fastest per call (no memcpy by runtime size), but every instantiation is a
  large macro, compile times and binary size grow with each key type, and
  the untyped version's dispatch is already cheap (measured: 7.7 ns per hit
  at 1 000 entries, 12 ns at 100 000).
- **Separate key and value arrays in the map (SoA)**: better for
  key-only scans, worse for the dominant hit path (two cache lines).
- **64-bit handles** (32 index + 32 generation): no wrap concern, but
  double the size in every component that stores a reference; §2 specifies
  32-bit handles.
- **Linked-node containers** (lists, trees): none are provided; §2's
  data-oriented rule prefers arrays, and every use so far is served by the
  array, map, and sparse set.

## Consequences

- Every container stores its allocator and takes it at init; nothing
  allocates from a global.
- Pointers into a container are invalidated by any call that can grow it;
  the headers say which calls those are.
- Over-aligned element types (above 16 bytes, except 32/64-byte SIMD types
  in arrays) are not supported by the containers; those types live in
  dedicated arrays.
- Concurrent use of the rings is verified by stress tests once platform
  threads exist (Phase 3); Phase 1 tests cover ordering, capacity, and
  counter wrap-around single-threaded.
