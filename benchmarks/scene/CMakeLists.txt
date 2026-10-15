# SPDX-License-Identifier: Apache-2.0
# The scene module (CLAUDE.md §8.7; docs/scene.md "Performance"): the
# million-entity benchmarks of the Phase 8 exit criterion.
nph_add_benchmark(scene
    SOURCES bench_scene.c
    DEPS nph::scene)
