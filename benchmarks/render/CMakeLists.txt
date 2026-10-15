# SPDX-License-Identifier: Apache-2.0
# The render module's CPU-side work and the golden toolkit's comparisons.
nph_add_benchmark(render
    SOURCES bench_render.c
    DEPS nph::render nph::golden)
