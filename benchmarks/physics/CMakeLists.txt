# SPDX-License-Identifier: Apache-2.0
# The physics module (CLAUDE.md §8.9; docs/physics.md "Performance"): the
# collision layer per call, and (from the world on) the simulation step.
nph_add_benchmark(physics
    SOURCES bench_physics.c
    DEPS nph::physics)
