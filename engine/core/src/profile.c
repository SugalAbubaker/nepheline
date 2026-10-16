// SPDX-License-Identifier: Apache-2.0
// CPU profiling, the recording half: per-thread SPSC rings of 32-byte
// events, drained by one collector. See profile.h and docs/profile.md.
#include <nepheline/core/assert.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/profile.h>
#include <nepheline/core/ring.h>

#include <stdio.h>
#include <string.h>

#if NPH_COMPILER_MSVC
#include <intrin.h>
#elif NPH_ARCH_X64
#include <x86intrin.h>
#endif

u64 nph_profile_ticks(void)
{
#if NPH_ARCH_X64
    return (u64)__rdtsc();
#elif NPH_COMPILER_MSVC
    return (u64)_ReadStatusReg(ARM64_CNTVCT);
#else
    u64 v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#endif
}

#if NPH_PROFILER_ENABLED

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct ProfileThread {
    NphSpscRing ring;
    NphAtomicU32 retired; // set by the owning thread on detach
    NphAtomicU64 dropped; // events lost to a full ring
    u32 id;
    NphSpinLock name_lock; // the owner renames; the collector copies
    char name[NPH_PROFILE_THREAD_NAME_MAX];
} ProfileThread;

// Process-wide profiling state (the exception CLAUDE.md §2 allows).
// `generation` changes with every init, so a thread whose ring belonged to
// an earlier initialisation never touches it again (its pointer is not
// dereferenced: the generation it was attached under is kept beside it).
static struct {
    bool initialized;
    NphAllocator *alloc;
    u32 ring_capacity;
    u32 max_threads;
    NphAtomicU32 enabled;
    NphAtomicU32 generation;
    NphAtomicU32 next_id;
    NphAtomicPtr threads[NPH_PROFILE_MAX_THREADS];
} g_profile = {false,
               NULL,
               0U,
               0U,
               NPH_ATOMIC_U32_INIT(0U),
               NPH_ATOMIC_U32_INIT(0U),
               NPH_ATOMIC_U32_INIT(0U),
               {{NULL}}};

static NPH_THREAD_LOCAL ProfileThread *t_profile_thread;
static NPH_THREAD_LOCAL u32 t_profile_generation;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult nph_profile_init(const NphProfileConfig *config)
{
    NPH_ASSERT(config != NULL && config->alloc != NULL);
    if (g_profile.initialized) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    const u32 capacity = config->ring_capacity != 0U ? config->ring_capacity : 16384U;
    const u32 threads = config->max_threads != 0U ? config->max_threads : 64U;
    if (!nph_is_pow2_u32(capacity) || capacity < 2U || threads > NPH_PROFILE_MAX_THREADS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    g_profile.alloc = config->alloc;
    g_profile.ring_capacity = capacity;
    g_profile.max_threads = threads;
    nph_atomic_store_u32(&g_profile.next_id, 0U, NPH_MEMORY_ORDER_RELAXED);
    for (u32 i = 0; i < NPH_PROFILE_MAX_THREADS; ++i) {
        nph_atomic_store_ptr(&g_profile.threads[i], NULL, NPH_MEMORY_ORDER_RELAXED);
    }
    // Generation 0 is what a thread that never attached holds.
    u32 generation = nph_atomic_load_u32(&g_profile.generation, NPH_MEMORY_ORDER_RELAXED) + 1U;
    if (generation == 0U) {
        generation = 1U;
    }
    nph_atomic_store_u32(&g_profile.generation, generation, NPH_MEMORY_ORDER_RELEASE);
    g_profile.initialized = true;
    nph_atomic_store_u32(&g_profile.enabled, 1U, NPH_MEMORY_ORDER_RELEASE);
    return NPH_RESULT_OK;
}

static void free_thread(ProfileThread *thread)
{
    nph_spsc_ring_free(&thread->ring);
    NPH_FREE_TYPE(g_profile.alloc, ProfileThread, thread);
}

void nph_profile_shutdown(void)
{
    if (!g_profile.initialized) {
        return;
    }
    nph_atomic_store_u32(&g_profile.enabled, 0U, NPH_MEMORY_ORDER_RELEASE);
    for (u32 i = 0; i < g_profile.max_threads; ++i) {
        ProfileThread *thread = (ProfileThread *)nph_atomic_exchange_ptr(
            &g_profile.threads[i], NULL, NPH_MEMORY_ORDER_ACQ_REL);
        if (thread != NULL) {
            free_thread(thread);
        }
    }
    t_profile_thread = NULL;
    t_profile_generation = 0U;
    g_profile.initialized = false;
    g_profile.max_threads = 0U;
}

bool nph_profile_is_initialized(void)
{
    return g_profile.initialized;
}

void nph_profile_set_enabled(bool enabled)
{
    if (g_profile.initialized) {
        nph_atomic_store_u32(&g_profile.enabled, enabled ? 1U : 0U, NPH_MEMORY_ORDER_RELEASE);
    }
}

bool nph_profile_is_enabled(void)
{
    return nph_atomic_load_u32(&g_profile.enabled, NPH_MEMORY_ORDER_RELAXED) != 0U;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

static void set_name(ProfileThread *thread, const char *name)
{
    nph_spin_lock_acquire(&thread->name_lock);
    if (name != NULL) {
        u32 n = 0;
        while (n + 1U < NPH_PROFILE_THREAD_NAME_MAX && name[n] != '\0') {
            thread->name[n] = name[n];
            ++n;
        }
        thread->name[n] = '\0';
    } else {
        (void)snprintf(thread->name, sizeof thread->name, "thread %u", thread->id);
    }
    nph_spin_lock_release(&thread->name_lock);
}

// The calling thread's ring for the current initialisation, or NULL.
static ProfileThread *current_thread(void)
{
    const u32 generation = nph_atomic_load_u32(&g_profile.generation, NPH_MEMORY_ORDER_ACQUIRE);
    return t_profile_generation == generation ? t_profile_thread : NULL;
}

static NphResult attach_thread(const char *name, ProfileThread **out)
{
    ProfileThread *existing = current_thread();
    if (existing != NULL) {
        if (name != NULL) {
            set_name(existing, name);
        }
        *out = existing;
        return NPH_RESULT_OK;
    }
    ProfileThread *thread = NPH_ALLOC_TYPE(g_profile.alloc, ProfileThread);
    if (thread == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const NphResult r = nph_spsc_ring_init(&thread->ring, g_profile.alloc,
                                           (u32)sizeof(NphProfileEvent), g_profile.ring_capacity);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(g_profile.alloc, ProfileThread, thread);
        return r;
    }
    thread->id = nph_atomic_fetch_add_u32(&g_profile.next_id, 1U, NPH_MEMORY_ORDER_RELAXED) + 1U;
    nph_spin_lock_init(&thread->name_lock);
    set_name(thread, name);
    // Claim a free registry slot. The release pairs with the collector's
    // acquire, publishing the initialised ring.
    for (u32 i = 0; i < g_profile.max_threads; ++i) {
        void *expected = NULL;
        if (nph_atomic_compare_exchange_ptr(&g_profile.threads[i], &expected, thread,
                                            NPH_MEMORY_ORDER_RELEASE, NPH_MEMORY_ORDER_RELAXED)) {
            t_profile_thread = thread;
            t_profile_generation =
                nph_atomic_load_u32(&g_profile.generation, NPH_MEMORY_ORDER_RELAXED);
            *out = thread;
            return NPH_RESULT_OK;
        }
    }
    free_thread(thread);
    return NPH_RESULT_FULL;
}

NphResult nph_profile_thread_attach(const char *name)
{
    if (!g_profile.initialized) {
        return NPH_RESULT_INVALID_STATE;
    }
    ProfileThread *thread;
    return attach_thread(name, &thread);
}

void nph_profile_thread_detach(void)
{
    ProfileThread *thread = current_thread();
    if (thread == NULL) {
        return;
    }
    // The collector frees the ring once it is empty; this thread must not
    // touch it again, so forget it first.
    t_profile_thread = NULL;
    t_profile_generation = 0U;
    nph_atomic_store_u32(&thread->retired, 1U, NPH_MEMORY_ORDER_RELEASE);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

// The recording functions are never inlined: a zone that spans a job's
// wait ends on whichever thread the fiber resumed on, and an inlined
// thread-local access could be computed once before the wait and reused
// after it (docs/platform.md, "Fibers").
static NPH_NO_INLINE void push_event(const NphProfileEvent *event)
{
    if (nph_atomic_load_u32(&g_profile.enabled, NPH_MEMORY_ORDER_RELAXED) == 0U) {
        return;
    }
    ProfileThread *thread = current_thread();
    if (thread == NULL && attach_thread(NULL, &thread) != NPH_RESULT_OK) {
        return;
    }
    if (!nph_spsc_ring_push(&thread->ring, event)) {
        nph_atomic_fetch_add_u64(&thread->dropped, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
}

static void record(u32 type, const char *name, u64 value)
{
    if (nph_atomic_load_u32(&g_profile.enabled, NPH_MEMORY_ORDER_RELAXED) == 0U) {
        return;
    }
    NphProfileEvent e;
    e.ticks = nph_profile_ticks();
    e.name = name;
    e.value = value;
    e.type = type;
    e.reserved = 0U;
    push_event(&e);
}

NPH_NO_INLINE void nph_profile_begin(const char *name)
{
    NPH_ASSERT(name != NULL);
    record(NPH_PROFILE_EVENT_BEGIN, name, 0U);
}

NPH_NO_INLINE void nph_profile_end(void)
{
    record(NPH_PROFILE_EVENT_END, NULL, 0U);
}

void nph_profile_frame(u64 index)
{
    record(NPH_PROFILE_EVENT_FRAME, NULL, index);
}

NPH_NO_INLINE void nph_profile_fiber(u32 fiber)
{
    record(NPH_PROFILE_EVENT_FIBER, NULL, fiber);
}

void nph_profile_counter(const char *name, f64 value)
{
    NPH_ASSERT(name != NULL);
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    record(NPH_PROFILE_EVENT_COUNTER, name, bits);
}

void nph_profile_record(const NphProfileEvent *event)
{
    NPH_ASSERT(event != NULL);
    push_event(event);
}

// ---------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------

u32 nph_profile_thread_slots(void)
{
    return g_profile.max_threads;
}

u32 nph_profile_drain(u32 slot, NphProfileEvent *out, u32 cap, NphProfileThreadInfo *info)
{
    NPH_ASSERT(info != NULL && (out != NULL || cap == 0U));
    memset(info, 0, sizeof *info);
    if (!g_profile.initialized || slot >= g_profile.max_threads) {
        return 0U;
    }
    ProfileThread *thread =
        (ProfileThread *)nph_atomic_load_ptr(&g_profile.threads[slot], NPH_MEMORY_ORDER_ACQUIRE);
    if (thread == NULL) {
        return 0U;
    }
    // Read the retirement before emptying the ring: everything the thread
    // pushed before it retired is then visible to the pops below.
    const bool retired = nph_atomic_load_u32(&thread->retired, NPH_MEMORY_ORDER_ACQUIRE) != 0U;
    info->id = thread->id;
    info->retired = retired ? 1 : 0;
    info->dropped = nph_atomic_load_u64(&thread->dropped, NPH_MEMORY_ORDER_RELAXED);
    nph_spin_lock_acquire(&thread->name_lock);
    memcpy(info->name, thread->name, sizeof info->name);
    nph_spin_lock_release(&thread->name_lock);
    u32 n = 0;
    while (n < cap && nph_spsc_ring_pop(&thread->ring, &out[n])) {
        ++n;
    }
    if (retired && nph_spsc_ring_count(&thread->ring) == 0U) {
        nph_atomic_store_ptr(&g_profile.threads[slot], NULL, NPH_MEMORY_ORDER_RELEASE);
        free_thread(thread);
    }
    return n;
}

#else // !NPH_PROFILER_ENABLED: the profiler is compiled out (Shipping).

NphResult nph_profile_init(const NphProfileConfig *config)
{
    NPH_UNUSED(config);
    return NPH_RESULT_UNSUPPORTED;
}

void nph_profile_shutdown(void)
{
}

bool nph_profile_is_initialized(void)
{
    return false;
}

void nph_profile_set_enabled(bool enabled)
{
    NPH_UNUSED(enabled);
}

bool nph_profile_is_enabled(void)
{
    return false;
}

NphResult nph_profile_thread_attach(const char *name)
{
    NPH_UNUSED(name);
    return NPH_RESULT_INVALID_STATE;
}

void nph_profile_thread_detach(void)
{
}

void nph_profile_begin(const char *name)
{
    NPH_UNUSED(name);
}

void nph_profile_end(void)
{
}

void nph_profile_frame(u64 index)
{
    NPH_UNUSED(index);
}

void nph_profile_fiber(u32 fiber)
{
    NPH_UNUSED(fiber);
}

void nph_profile_counter(const char *name, f64 value)
{
    NPH_UNUSED(name);
    NPH_UNUSED(value);
}

void nph_profile_record(const NphProfileEvent *event)
{
    NPH_UNUSED(event);
}

u32 nph_profile_thread_slots(void)
{
    return 0U;
}

u32 nph_profile_drain(u32 slot, NphProfileEvent *out, u32 cap, NphProfileThreadInfo *info)
{
    NPH_UNUSED(slot);
    NPH_UNUSED(out);
    NPH_UNUSED(cap);
    NPH_ASSERT(info != NULL);
    memset(info, 0, sizeof *info);
    return 0U;
}

#endif
