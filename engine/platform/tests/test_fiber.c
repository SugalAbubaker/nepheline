// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/fiber.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <math.h>
#include <string.h>

#if NPH_COMPILER_MSVC
#include <intrin.h>
#else
#include <xmmintrin.h>
#endif
#if NPH_ASAN_ENABLED
#include <sanitizer/asan_interface.h>
#endif

// The MXCSR rounding-control field (bits 13-14): what the fiber switch
// preserves per fiber. The x87 control word is deliberately not part of the
// contract (ADR-0010 §8), so the test reads MXCSR directly rather than
// through fegetround, which glibc answers from the x87 word.
#define MXCSR_RC_MASK 0x6000U
#define MXCSR_RC_UP   0x4000U
#define MXCSR_RC_DOWN 0x2000U

static u32 get_rounding(void)
{
    return (u32)_mm_getcsr() & MXCSR_RC_MASK;
}

static void set_rounding(u32 rc)
{
    _mm_setcsr(((u32)_mm_getcsr() & ~MXCSR_RC_MASK) | rc);
}

// ---------------------------------------------------------------------------
// One fiber: start, run, switch back, resume, switch back again.
// ---------------------------------------------------------------------------

typedef struct Simple {
    NphFiber main;
    NphFiber fiber;
    u32 step;
    NphFiber *seen_current;
    void *seen_user;
} Simple;

static void simple_fn(void *user)
{
    Simple *s = (Simple *)user;
    s->seen_user = user;
    s->seen_current = nph_fiber_current();
    s->step = 1U;
    nph_fiber_switch(&s->fiber, &s->main);
    s->step = 2U;
    nph_fiber_switch(&s->fiber, &s->main);
    s->step = 99U; // never reached: the test destroys the fiber while suspended
    nph_fiber_switch(&s->fiber, &s->main);
}

NTEST(fiber, create_switch_resume_destroy)
{
    Simple s;
    memset(&s, 0, sizeof s);
    NTEST_ASSERT_NULL(nph_fiber_current());
    nph_fiber_init_thread(&s.main);
    NTEST_ASSERT_EQ_PTR(nph_fiber_current(), &s.main);
    NTEST_ASSERT_TRUE(s.main.is_thread);

    NphFiberDesc desc;
    desc.fn = simple_fn;
    desc.user = &s;
    desc.stack_size = 0U;
    desc.name = "simple";
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&s.fiber, &desc), NPH_RESULT_OK);
    NTEST_ASSERT_GE_UINT(s.fiber.stack_size, NPH_FIBER_DEFAULT_STACK_SIZE);
    NTEST_ASSERT_EQ_UINT(s.step, 0U); // not started until switched to

    nph_fiber_switch(&s.main, &s.fiber);
    NTEST_ASSERT_EQ_UINT(s.step, 1U);
    NTEST_ASSERT_EQ_PTR(s.seen_user, &s);
    NTEST_ASSERT_EQ_PTR(s.seen_current, &s.fiber);
    NTEST_ASSERT_EQ_PTR(nph_fiber_current(), &s.main);

    nph_fiber_switch(&s.main, &s.fiber);
    NTEST_ASSERT_EQ_UINT(s.step, 2U);

    u8 *const stack_lo = s.fiber.stack_lo;
    const u64 stack_size = s.fiber.stack_size;
    nph_fiber_destroy(&s.fiber);
    NTEST_ASSERT_NULL(s.fiber.context);
#if NPH_ASAN_ENABLED
    // The frames suspended on the stack had poisoned redzones; destroying
    // the fiber must clear them, or whatever next reuses the addresses is
    // reported falsely.
    if (stack_lo != NULL) {
        NTEST_ASSERT_NULL(__asan_region_is_poisoned(stack_lo, (size_t)stack_size));
    }
#else
    NPH_UNUSED(stack_lo);
    NPH_UNUSED(stack_size);
#endif
    nph_fiber_destroy(&s.fiber); // idempotent
    nph_fiber_release_thread(&s.main);
    NTEST_ASSERT_NULL(nph_fiber_current());
}

NTEST(fiber, null_fn_is_rejected)
{
    NphFiber f;
    NphFiberDesc desc;
    desc.fn = NULL;
    desc.user = NULL;
    desc.stack_size = 0U;
    desc.name = NULL;
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&f, &desc), NPH_RESULT_INVALID_ARGUMENT);
    nph_fiber_destroy(&f); // safe on the zero-filled result
    NTEST_LOG("backend: %s", nph_fiber_backend_name());
}

// ---------------------------------------------------------------------------
// Ping-pong: two fibers and the thread, many switches.
// ---------------------------------------------------------------------------

typedef struct PingPong {
    NphFiber main;
    NphFiber a;
    NphFiber b;
    u32 rounds;
    u64 count;
    u64 order_hash; // records who ran when
} PingPong;

static void ping_fn(void *user)
{
    PingPong *p = (PingPong *)user;
    for (;;) {
        p->count += 1U;
        p->order_hash = p->order_hash * 31U + 1U;
        nph_fiber_switch(&p->a, &p->b);
    }
}

static void pong_fn(void *user)
{
    PingPong *p = (PingPong *)user;
    for (;;) {
        p->count += 1U;
        p->order_hash = p->order_hash * 31U + 2U;
        nph_fiber_switch(&p->b, &p->main);
    }
}

NTEST(fiber, ping_pong_between_fibers_and_thread)
{
    PingPong p;
    memset(&p, 0, sizeof p);
    nph_fiber_init_thread(&p.main);
    NphFiberDesc desc;
    desc.fn = ping_fn;
    desc.user = &p;
    desc.stack_size = NPH_KIB(32);
    desc.name = "ping";
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&p.a, &desc), NPH_RESULT_OK);
    desc.fn = pong_fn;
    desc.name = "pong";
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&p.b, &desc), NPH_RESULT_OK);

    p.rounds = 20000U * TEST_SCALE;
    u64 expected_hash = 0U;
    const NphStopwatch sw = nph_stopwatch_start();
    for (u32 i = 0; i < p.rounds; ++i) {
        nph_fiber_switch(&p.main, &p.a); // main -> a -> b -> main
        expected_hash = (expected_hash * 31U + 1U) * 31U + 2U;
    }
    const u64 elapsed = nph_stopwatch_elapsed_ns(&sw);
    NTEST_LOG("%.1f ns per switch (3 per round)", (f64)elapsed / ((f64)p.rounds * 3.0));
    NTEST_ASSERT_EQ_UINT(p.count, (u64)p.rounds * 2U);
    NTEST_ASSERT_EQ_UINT(p.order_hash, expected_hash);
    NTEST_ASSERT_EQ_PTR(nph_fiber_current(), &p.main);

    nph_fiber_destroy(&p.a);
    nph_fiber_destroy(&p.b);
    nph_fiber_release_thread(&p.main);
}

// ---------------------------------------------------------------------------
// Register and floating-point state survive a switch.
// ---------------------------------------------------------------------------

typedef struct FpState {
    NphFiber main;
    NphFiber noisy;
    NphFiber worker;
    f64 sum;
    u32 rounding_seen; // what the worker observed after resuming
    u32 rounds;
} FpState;

// Churns every register the ABI lets a callee clobber and sets a different
// rounding mode, then hands control back.
static void noisy_fn(void *user)
{
    FpState *s = (FpState *)user;
    volatile f64 junk[16];
    for (;;) {
        for (u32 i = 0; i < 16U; ++i) {
            junk[i] = sqrt((f64)i * 7.0 + 1.0) * 1e9;
        }
        set_rounding(MXCSR_RC_DOWN);
        nph_fiber_switch(&s->noisy, &s->worker);
    }
}

static void worker_fn(void *user)
{
    FpState *s = (FpState *)user;
    set_rounding(MXCSR_RC_UP);
    // The compiler may keep `acc` in a callee-saved xmm register on Windows
    // across the switch; the switch must preserve it.
    f64 acc = 0.0;
    for (u32 i = 0; i < s->rounds; ++i) {
        acc += sqrt((f64)i);
        nph_fiber_switch(&s->worker, &s->noisy);
        acc += 1.0;
    }
    s->sum = acc;
    s->rounding_seen = get_rounding();
    set_rounding(0U); // back to nearest
    nph_fiber_switch(&s->worker, &s->main);
}

NTEST(fiber, callee_saved_and_floating_point_state_survive_switches)
{
    FpState s;
    memset(&s, 0, sizeof s);
    s.rounds = 1000U;
    nph_fiber_init_thread(&s.main);
    NphFiberDesc desc;
    desc.fn = noisy_fn;
    desc.user = &s;
    desc.stack_size = 0U;
    desc.name = "noisy";
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&s.noisy, &desc), NPH_RESULT_OK);
    desc.fn = worker_fn;
    desc.name = "worker";
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&s.worker, &desc), NPH_RESULT_OK);

    const u32 rounding_before = get_rounding();
    nph_fiber_switch(&s.main, &s.worker);

    f64 expected = 0.0;
    set_rounding(MXCSR_RC_UP);
    for (u32 i = 0; i < s.rounds; ++i) {
        expected += sqrt((f64)i);
        expected += 1.0;
    }
    set_rounding(rounding_before);
    NTEST_ASSERT_EQ_UINT(s.rounding_seen, MXCSR_RC_UP);    // MXCSR travels with the fiber
    NTEST_ASSERT_EQ_UINT(get_rounding(), rounding_before); // and the thread's is untouched
    NTEST_ASSERT_NEAR_F64(s.sum, expected, 1e-6);

    nph_fiber_destroy(&s.noisy);
    nph_fiber_destroy(&s.worker);
    nph_fiber_release_thread(&s.main);
}

// ---------------------------------------------------------------------------
// Deep stack use on a fiber: recursion and large frames (exercise the
// stack probe on Windows).
// ---------------------------------------------------------------------------

typedef struct Deep {
    NphFiber main;
    NphFiber fiber;
    u64 checksum;
} Deep;

static u64 recurse(u32 depth, u64 acc)
{
    volatile u8 frame[512];
    frame[0] = (u8)depth;
    frame[511] = (u8)(depth * 3U);
    acc = acc * 1315423911ULL + frame[0] + frame[511];
    if (depth == 0U) {
        return acc;
    }
    return recurse(depth - 1U, acc);
}

static void deep_fn(void *user)
{
    Deep *d = (Deep *)user;
    // A 20 KiB frame first (several pages at once), then 100 x 512 B frames.
    volatile u8 big[20U * 1024U];
    big[0] = 1U;
    big[sizeof big - 1U] = 2U;
    d->checksum = recurse(100U, big[0] + big[sizeof big - 1U]);
    nph_fiber_switch(&d->fiber, &d->main);
}

NTEST(fiber, deep_stack_use_within_the_stack_size)
{
    Deep d;
    memset(&d, 0, sizeof d);
    nph_fiber_init_thread(&d.main);
    NphFiberDesc desc;
    desc.fn = deep_fn;
    desc.user = &d;
    desc.stack_size = NPH_KIB(256);
    desc.name = "deep";
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&d.fiber, &desc), NPH_RESULT_OK);
    nph_fiber_switch(&d.main, &d.fiber);
    NTEST_ASSERT_EQ_UINT(d.checksum, recurse(100U, 3U));
    nph_fiber_destroy(&d.fiber);
    nph_fiber_release_thread(&d.main);
}

// ---------------------------------------------------------------------------
// Many fibers, round robin.
// ---------------------------------------------------------------------------

#define MANY 256U

typedef struct Many {
    NphFiber main;
    NphFiber fibers[MANY];
    u32 visits[MANY];
    u32 index[MANY];
} Many;

static Many g_many; // too large for a test's stack

static void many_fn(void *user)
{
    const u32 i = *(const u32 *)user;
    for (;;) {
        g_many.visits[i] += 1U;
        nph_fiber_switch(&g_many.fibers[i], &g_many.main);
    }
}

NTEST(fiber, hundreds_of_fibers_round_robin)
{
    memset(&g_many, 0, sizeof g_many);
    nph_fiber_init_thread(&g_many.main);
    for (u32 i = 0; i < MANY; ++i) {
        g_many.index[i] = i;
        NphFiberDesc desc;
        desc.fn = many_fn;
        desc.user = &g_many.index[i];
        desc.stack_size = NPH_FIBER_MIN_STACK_SIZE;
        desc.name = "many";
        NTEST_ASSERT_EQ_INT(nph_fiber_create(&g_many.fibers[i], &desc), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(g_many.fibers[i].stack_size, NPH_FIBER_MIN_STACK_SIZE);
    }
    for (u32 round = 0; round < 10U; ++round) {
        for (u32 i = 0; i < MANY; ++i) {
            nph_fiber_switch(&g_many.main, &g_many.fibers[i]);
        }
    }
    for (u32 i = 0; i < MANY; ++i) {
        NTEST_ASSERT_EQ_UINT(g_many.visits[i], 10U);
        nph_fiber_destroy(&g_many.fibers[i]);
    }
    nph_fiber_release_thread(&g_many.main);
}

// ---------------------------------------------------------------------------
// A fiber migrates between threads: started on the main thread, continued
// on a worker, finished on another worker. This is what the job system
// relies on.
// ---------------------------------------------------------------------------

typedef struct Migrate {
    NphFiber fiber;
    NphFiber *host; // the thread fiber of whoever runs it now
    u64 thread_ids[3];
    u32 step;
} Migrate;

static void migrating_fn(void *user)
{
    Migrate *m = (Migrate *)user;
    for (u32 i = 0; i < 3U; ++i) {
        m->thread_ids[i] = nph_thread_current_id();
        m->step = i + 1U;
        nph_fiber_switch(&m->fiber, m->host);
    }
}

static void host_worker(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Migrate *m = (Migrate *)shared;
    NphFiber thread_fiber;
    nph_fiber_init_thread(&thread_fiber);
    m->host = &thread_fiber;
    nph_fiber_switch(&thread_fiber, &m->fiber);
    m->host = NULL;
    nph_fiber_release_thread(&thread_fiber);
}

NTEST(fiber, migrates_between_threads)
{
    Migrate m;
    memset(&m, 0, sizeof m);
    NphFiber main;
    nph_fiber_init_thread(&main);
    NphFiberDesc desc;
    desc.fn = migrating_fn;
    desc.user = &m;
    desc.stack_size = 0U;
    desc.name = "migrant";
    NTEST_ASSERT_EQ_INT(nph_fiber_create(&m.fiber, &desc), NPH_RESULT_OK);

    m.host = &main;
    nph_fiber_switch(&main, &m.fiber);
    NTEST_ASSERT_EQ_UINT(m.step, 1U);
    NTEST_ASSERT_EQ_UINT(m.thread_ids[0], nph_thread_current_id());

    TestWorker w;
    test_threads_run(&w, 1U, host_worker, &m);
    NTEST_ASSERT_EQ_UINT(m.step, 2U);
    NTEST_ASSERT_NE_UINT(m.thread_ids[1], m.thread_ids[0]);

    test_threads_run(&w, 1U, host_worker, &m);
    NTEST_ASSERT_EQ_UINT(m.step, 3U);
    NTEST_ASSERT_NE_UINT(m.thread_ids[2], m.thread_ids[0]);
    // The second worker is a new thread; its id may or may not be reused.
    NTEST_ASSERT_EQ_PTR(nph_fiber_current(), &main);

    nph_fiber_destroy(&m.fiber);
    nph_fiber_release_thread(&main);
}

// ---------------------------------------------------------------------------
// Fibers on several threads at once, each thread with its own set.
// ---------------------------------------------------------------------------

typedef struct PerThread {
    u32 rounds;
    NphAtomicU32 failures;
} PerThread;

typedef struct Local {
    NphFiber thread_fiber;
    NphFiber fibers[4];
    u32 counts[4];
    u32 which;
} Local;

static void local_fn(void *user)
{
    Local *l = (Local *)user;
    const u32 i = l->which; // captured at the first run
    for (;;) {
        l->counts[i] += 1U;
        nph_fiber_switch(&l->fibers[i], &l->thread_fiber);
    }
}

static void fiber_thread_worker(u32 index, void *shared)
{
    NPH_UNUSED(index);
    PerThread *p = (PerThread *)shared;
    Local l;
    memset(&l, 0, sizeof l);
    nph_fiber_init_thread(&l.thread_fiber);
    for (u32 i = 0; i < 4U; ++i) {
        NphFiberDesc desc;
        desc.fn = local_fn;
        desc.user = &l;
        desc.stack_size = NPH_KIB(32);
        desc.name = "local";
        if (nph_fiber_create(&l.fibers[i], &desc) != NPH_RESULT_OK) {
            nph_atomic_fetch_add_u32(&p->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
            return;
        }
        l.which = i;
        nph_fiber_switch(&l.thread_fiber, &l.fibers[i]); // first run captures `which`
    }
    for (u32 r = 0; r < p->rounds; ++r) {
        for (u32 i = 0; i < 4U; ++i) {
            nph_fiber_switch(&l.thread_fiber, &l.fibers[i]);
        }
    }
    for (u32 i = 0; i < 4U; ++i) {
        if (l.counts[i] != p->rounds + 1U) {
            nph_atomic_fetch_add_u32(&p->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
        nph_fiber_destroy(&l.fibers[i]);
    }
    nph_fiber_release_thread(&l.thread_fiber);
}

NTEST(fiber, independent_fiber_sets_on_many_threads)
{
    PerThread p;
    p.rounds = 2000U * TEST_SCALE;
    nph_atomic_store_u32(&p.failures, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker workers[8];
    test_threads_run(workers, 8U, fiber_thread_worker, &p);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.failures, NPH_MEMORY_ORDER_RELAXED), 0U);
}
