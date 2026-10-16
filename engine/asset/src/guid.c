// SPDX-License-Identifier: Apache-2.0
// The generator seeded from the platform's clocks. See asset/guid.h.
#include <nepheline/asset/guid.h>
#include <nepheline/core/atomic.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

void nph_guid_generator_init(NphGuidGenerator *g)
{
    static NphAtomicU64 counter = NPH_ATOMIC_U64_INIT(0U);
    const u64 serial = nph_atomic_fetch_add_u64(&counter, 1U, NPH_MEMORY_ORDER_RELAXED);
    u64 seed = nph_time_wall_ns();
    seed = nph_hash_combine(seed, nph_time_cycles());
    seed = nph_hash_combine(seed, nph_platform_process_id());
    seed = nph_hash_combine(seed, serial);
    seed = nph_hash_combine(seed, (u64)(uptr)g);
    nph_guid_generator_seed(g, seed);
}
