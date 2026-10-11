// SPDX-License-Identifier: Apache-2.0
// Allocator benchmarks (CLAUDE.md §12 Phase 1 exit criterion: "Allocator
// benchmarks recorded"). Results are discussed in docs/core.md.
//
// Workloads:
//   alloc_free_64B      allocate and immediately free one 64-byte block
//   batch_4k            allocate 4096 blocks of 16..1024 bytes, free them all
//   random_mixed        steady state: 1024 live slots, random alloc/free of
//                       16..1024 bytes (the shape of a game's heap traffic)
//   realloc_grow        grow one block by doubling from 16 B to 1 MiB
//   bump_64B            arena / stack: 1024 allocations then a reset
// Each is run against the system allocator (malloc), TLSF, and where the
// workload fits, the pool, arena, stack, and the debug wrapper over TLSF.

#include <nepheline/core/alloc.h>
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/arena.h>
#include <nepheline/core/pool.h>
#include <nepheline/core/stack_alloc.h>
#include <nepheline/core/tlsf.h>

#include <nbench/nbench.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_SIZE   NPH_MIB(64)
#define BATCH_COUNT 4096U
#define LIVE_SLOTS  1024U
#define MIXED_OPS   8192U
#define MIN_SIZE    16U
#define MAX_SIZE    1024U

// Deterministic sequences, generated once so the RNG is not measured.
static u64 g_sizes[MIXED_OPS];
static u32 g_slots[MIXED_OPS];
static bool g_sequences_ready;

static u64 splitmix(u64 *s)
{
    u64 z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void prepare_sequences(void)
{
    if (g_sequences_ready) {
        return;
    }
    u64 s = 0x5EEDULL;
    for (u32 i = 0; i < MIXED_OPS; ++i) {
        g_sizes[i] = MIN_SIZE + splitmix(&s) % (MAX_SIZE - MIN_SIZE + 1U);
        g_slots[i] = (u32)(splitmix(&s) % LIVE_SLOTS);
    }
    g_sequences_ready = true;
}

// ---------------------------------------------------------------------------
// Heaps under test
// ---------------------------------------------------------------------------

typedef struct Heaps {
    NphTlsf tlsf;
    NphDebugAllocator debug;
    NphPool pool;
    NphArena arena;
    NphStackAlloc stack;
} Heaps;

static Heaps *heaps_create(void)
{
    Heaps *h = NPH_ALLOC_TYPE(nph_allocator_system(), Heaps);
    if (h == NULL ||
        nph_tlsf_init_from(&h->tlsf, nph_allocator_system(), HEAP_SIZE) != NPH_RESULT_OK ||
        nph_pool_init_from(&h->pool, nph_allocator_system(), 64U, 16U, LIVE_SLOTS * 2U) !=
            NPH_RESULT_OK ||
        nph_arena_init_from(&h->arena, nph_allocator_system(), NPH_MIB(8)) != NPH_RESULT_OK ||
        nph_stack_alloc_init_from(&h->stack, nph_allocator_system(), NPH_MIB(8)) != NPH_RESULT_OK) {
        abort();
    }
    nph_debug_allocator_init(&h->debug, nph_tlsf_allocator(&h->tlsf));
    return h;
}

static void heaps_destroy(Heaps *h)
{
    nph_debug_allocator_destroy(&h->debug, NULL, NULL);
    nph_stack_alloc_destroy(&h->stack);
    nph_arena_destroy(&h->arena);
    nph_pool_destroy(&h->pool);
    nph_tlsf_destroy(&h->tlsf);
    NPH_FREE_TYPE(nph_allocator_system(), Heaps, h);
}

// ---------------------------------------------------------------------------
// Workloads over the generic interface
// ---------------------------------------------------------------------------

static void workload_alloc_free_64(NbenchState *state, NphAllocator *a)
{
    while (nbench_keep_running(state)) {
        void *p = NPH_ALLOC(a, 64U, 16U);
        nbench_do_not_optimize(p);
        NPH_FREE(a, p, 64U, 16U);
    }
}

static void workload_batch(NbenchState *state, NphAllocator *a)
{
    static void *ptrs[BATCH_COUNT];
    prepare_sequences();
    nbench_set_items_per_iteration(state, (u64)BATCH_COUNT * 2U);
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < BATCH_COUNT; ++i) {
            ptrs[i] = NPH_ALLOC(a, g_sizes[i], 16U);
        }
        nbench_clobber_memory();
        for (u32 i = 0; i < BATCH_COUNT; ++i) {
            NPH_FREE(a, ptrs[i], g_sizes[i], 16U);
        }
    }
}

static void workload_random_mixed(NbenchState *state, NphAllocator *a)
{
    static void *live[LIVE_SLOTS];
    static u64 live_size[LIVE_SLOTS];
    prepare_sequences();
    memset((void *)live, 0, sizeof live);
    nbench_set_items_per_iteration(state, MIXED_OPS);
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < MIXED_OPS; ++i) {
            const u32 slot = g_slots[i];
            if (live[slot] != NULL) {
                NPH_FREE(a, live[slot], live_size[slot], 16U);
                live[slot] = NULL;
            } else {
                live_size[slot] = g_sizes[i];
                live[slot] = NPH_ALLOC(a, g_sizes[i], 16U);
            }
        }
    }
    for (u32 i = 0; i < LIVE_SLOTS; ++i) {
        if (live[i] != NULL) {
            NPH_FREE(a, live[i], live_size[i], 16U);
            live[i] = NULL;
        }
    }
}

static void workload_realloc_grow(NbenchState *state, NphAllocator *a)
{
    nbench_set_items_per_iteration(state, 17U);
    while (nbench_keep_running(state)) {
        u64 size = 16U;
        void *p = NPH_ALLOC(a, size, 16U);
        for (u32 step = 0; step < 16U; ++step) {
            p = NPH_REALLOC(a, p, size, size * 2U, 16U);
            size *= 2U;
        }
        nbench_do_not_optimize(p);
        NPH_FREE(a, p, size, 16U);
    }
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

#define DEFINE_ALLOCATOR_BENCH(workload, suffix, expr)                                             \
    NBENCH(workload##_##suffix)                                                                    \
    {                                                                                              \
        Heaps *h = heaps_create();                                                                 \
        NPH_UNUSED(h);                                                                             \
        nbench_set_label(state, #suffix);                                                          \
        workload_##workload(state, (expr));                                                        \
        heaps_destroy(h);                                                                          \
    }

DEFINE_ALLOCATOR_BENCH(alloc_free_64, system, nph_allocator_system())
DEFINE_ALLOCATOR_BENCH(alloc_free_64, tlsf, nph_tlsf_allocator(&h->tlsf))
DEFINE_ALLOCATOR_BENCH(alloc_free_64, pool, nph_pool_allocator(&h->pool))
DEFINE_ALLOCATOR_BENCH(alloc_free_64, debug_tlsf, nph_debug_allocator_allocator(&h->debug))

DEFINE_ALLOCATOR_BENCH(batch, system, nph_allocator_system())
DEFINE_ALLOCATOR_BENCH(batch, tlsf, nph_tlsf_allocator(&h->tlsf))
DEFINE_ALLOCATOR_BENCH(batch, debug_tlsf, nph_debug_allocator_allocator(&h->debug))

DEFINE_ALLOCATOR_BENCH(random_mixed, system, nph_allocator_system())
DEFINE_ALLOCATOR_BENCH(random_mixed, tlsf, nph_tlsf_allocator(&h->tlsf))
DEFINE_ALLOCATOR_BENCH(random_mixed, debug_tlsf, nph_debug_allocator_allocator(&h->debug))

DEFINE_ALLOCATOR_BENCH(realloc_grow, system, nph_allocator_system())
DEFINE_ALLOCATOR_BENCH(realloc_grow, tlsf, nph_tlsf_allocator(&h->tlsf))

NBENCH(bump_64_arena)
{
    Heaps *h = heaps_create();
    nbench_set_label(state, "arena");
    nbench_set_items_per_iteration(state, 1024U);
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < 1024U; ++i) {
            nbench_do_not_optimize(nph_arena_alloc(&h->arena, 64U, 16U));
        }
        nph_arena_reset(&h->arena);
    }
    heaps_destroy(h);
}

NBENCH(bump_64_stack)
{
    Heaps *h = heaps_create();
    nbench_set_label(state, "stack");
    nbench_set_items_per_iteration(state, 1024U);
    static void *ptrs[1024];
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < 1024U; ++i) {
            ptrs[i] = nph_stack_alloc_push(&h->stack, 64U, 16U);
        }
        nbench_clobber_memory();
        for (u32 i = 1024U; i > 0U; --i) {
            nph_stack_alloc_pop(&h->stack, ptrs[i - 1U]);
        }
    }
    heaps_destroy(h);
}

NBENCH(pool_direct_alloc_free_64)
{
    Heaps *h = heaps_create();
    nbench_set_label(state, "pool (direct API)");
    while (nbench_keep_running(state)) {
        void *p = nph_pool_alloc(&h->pool);
        nbench_do_not_optimize(p);
        nph_pool_free(&h->pool, p);
    }
    heaps_destroy(h);
}

NBENCH(tlsf_direct_alloc_free_64)
{
    Heaps *h = heaps_create();
    nbench_set_label(state, "tlsf (direct API)");
    while (nbench_keep_running(state)) {
        void *p = nph_tlsf_alloc(&h->tlsf, 64U, 16U);
        nbench_do_not_optimize(p);
        nph_tlsf_free(&h->tlsf, p);
    }
    heaps_destroy(h);
}

NBENCH(malloc_direct_alloc_free_64)
{
    nbench_set_label(state, "malloc (direct API)");
    while (nbench_keep_running(state)) {
        void *p = malloc(64U);
        nbench_do_not_optimize(p);
        free(p);
    }
}
