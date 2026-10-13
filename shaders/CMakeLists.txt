# SPDX-License-Identifier: Apache-2.0
# Shader sources (CLAUDE.md §4 "shaders/: GLSL sources + shared headers").
# shaders/shared/ holds headers included by both GLSL and C (nph_shared.h)
# and the bindless declarations every shader uses (nph_bindless.glsl).

# Shaders the gpu tests render with.
nph_add_shaders(nph_shaders_test
    SOURCES
        test/test_fill.comp
        test/test_image.comp
        test/test_triangle.vert
        test/test_color.frag
        test/test_sample.frag
        test/test_fullscreen.vert
)

# Shaders of the Phase 5 samples.
nph_add_shaders(nph_shaders_samples
    SOURCES
        samples/triangle.vert
        samples/triangle.frag
        samples/mesh.vert
        samples/mesh.frag
        samples/mesh_anim.comp
)

# Shaders of the GPU benchmarks (benchmarks/gpu).
nph_add_shaders(nph_shaders_bench
    SOURCES
        bench/bench_fill.comp
        bench/bench_read.comp
        bench/bench_fma.comp
        bench/bench_random.comp
        bench/bench_fullscreen.vert
        bench/bench_color.frag
        bench/bench_sample.frag
        bench/bench_visbuffer.frag
)
