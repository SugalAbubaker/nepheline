# SPDX-License-Identifier: Apache-2.0
nph_add_benchmark(core_alloc
    SOURCES bench_alloc.c
    DEPS nph::core)

nph_add_benchmark(core_hash
    SOURCES bench_hash.c
    DEPS nph::core)
