# SPDX-License-Identifier: Apache-2.0
# Shader sources (CLAUDE.md §4 "shaders/: GLSL sources + shared headers").
# shaders/shared/ holds headers included by both GLSL and C (nph_shared.h)
# and the bindless declarations every shader uses (nph_bindless.glsl).

# Shaders the gpu tests render with.
nph_add_shaders(nph_shaders_test
    SOURCES
        test/test_fill.comp
        test/test_specialize.comp
        test/test_ray_query.comp
        test/test_image.comp
        test/test_triangle.vert
        test/test_clip.vert
        test/test_color.frag
        test/test_sample.frag
        test/test_fullscreen.vert
        test/test_texel_fetch.comp
        test/test_mesh.mesh
        test/test_mesh_color.mesh
        test/test_prim_color.frag
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
# renderer, image-based lighting, tonemapping, debug lines, and the
# GPU-driven visibility-buffer renderer (vis_*).
nph_add_shaders(nph_shaders_render
    SOURCES
        render/forward.vert
        render/forward_opaque.frag
        render/forward_mask.frag
        render/forward_blend.frag
        render/forward_opaque_rt.frag
        render/forward_mask_rt.frag
        render/forward_blend_rt.frag
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
        render/vis_scatter.comp
        render/vis_cull_instances.comp
        render/vis_cull_meshlets.comp
        render/vis_args.comp
        render/vis_raster_opaque.mesh
        render/vis_raster_masked.mesh
        render/vis_raster_opaque.vert
        render/vis_raster_masked.vert
        render/vis_raster_mesh_opaque.frag
        render/vis_raster_mesh_masked.frag
        render/vis_raster_vert_opaque.frag
        render/vis_raster_vert_masked.frag
        render/vis_hzb.comp
        render/vis_resolve.comp
        render/vis_resolve_rt.comp
        render/rt_instances.comp
        render/vsm_pages.comp
        render/vsm_cull.comp
        render/vsm_raster.mesh
        render/vsm_raster.vert
        render/vsm_clear.vert
        render/vsm_request.frag
        render/vis_sw_raster.comp
        render/vis_lights.comp
        render/vis_light_bins.comp
        render/vis_light_tiles.comp
        render/vis_depth.frag
        render/vis_debug_line.frag
        # The culling math for the render tests (render/reference.h twins).
        render/vis_math_test.comp
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

# The editor's UI (editor/src/ui_renderer.c): Dear ImGui's draw data.
nph_add_shaders(nph_shaders_editor
    SOURCES
        editor/ui.vert
        editor/ui.frag
)
