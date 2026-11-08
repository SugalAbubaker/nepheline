// SPDX-License-Identifier: Apache-2.0
// Benchmarks of a known relative cost, for the black-box tests of nbench's
// budgets (CMakeLists.txt here): self_work_8 does eight times the work of
// self_work_1, so on any machine it measures about eight times as long, and a
// budget relative to self_work_1 has a known answer.

#include <nbench/nbench.h>

#include <stdint.h>

// Read once: an opaque multiplier keeps the optimiser from composing the
// chain's steps into fewer.
static volatile uint64_t g_multiplier = 6364136223846793005ULL;

// `rounds` steps of a linear congruential chain, each waiting on the last.
static uint64_t chain(uint64_t x, uint64_t a, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        x = x * a + 1442695040888963407ULL;
    }
    return x;
}

NBENCH(self_work_1)
{
    const uint64_t a = g_multiplier;
    uint64_t x = 1U;
    while (nbench_keep_running(state)) {
        x = chain(x, a, 64);
        nbench_do_not_optimize_u64(x);
    }
}

NBENCH(self_work_8)
{
    const uint64_t a = g_multiplier;
    uint64_t x = 1U;
    while (nbench_keep_running(state)) {
        x = chain(x, a, 512);
        nbench_do_not_optimize_u64(x);
    }
}
