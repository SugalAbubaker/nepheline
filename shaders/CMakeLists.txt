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
        test/test_texel_fetch.comp
)

# Shaders of the Phase 5 samples.
nph_add_shaders(nph_shaders_samples
    SOURCES
        samples/triangle.vert
        samples/triangle.frag
        samples/mesh.vert
        samples/mesh.frag
        samples/mesh_anim.comp
        samples/cooked_mesh.vert
        samples/cooked_mesh.frag
)

# The renderer (engine/render; docs/render.md): the forward PBR reference
# renderer, image-based lighting, tonemapping, and debug lines.
nph_add_shaders(nph_shaders_render
    SOURCES
        render/forward.vert
        render/forward_opaque.frag
        render/forward_mask.frag
        render/forward_blend.frag
        render/fullscreen.vert
        render/sky.frag
        render/tonemap.frag
        render/debug_line.vert
        render/debug_line.frag
        render/ibl_sky.comp
        render/ibl_equirect.comp
        render/ibl_cube.comp
        render/ibl_downsample.comp
        render/ibl_prefilter.comp
        render/ibl_sh.comp
        render/brdf_lut.comp
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
