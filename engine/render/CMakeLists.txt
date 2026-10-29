# SPDX-License-Identifier: Apache-2.0
# The render module: the forward PBR reference renderer, image-based
# lighting, tonemapping, debug draw, the GPU profiler, and procedural
# primitives (CLAUDE.md §8.6 stages 1-2; docs/render.md;
# docs/adr/0018-reference-renderer.md). Depends on gpu and, through it, on
# jobs, platform, math, and core.

nph_add_module(render
    SOURCES
        src/ao.c
        src/cell_proxy.c
        src/cluster_level.c
        src/cluster_lod.c
        src/cluster_lod_internal.h
        src/cluster_simplify.c
        src/cluster_voxel.c
        src/debug_draw.c
        src/dof.c
        src/frame.c
        src/gi.c
        src/gpu_profiler.c
        src/ibl.c
        src/meshlet.c
        src/motion_blur.c
        src/post.c
        src/primitives.c
        src/reference.c
        src/renderer.c
        src/render_internal.h
        src/rt.c
        src/taa.c
        src/vis.c
        src/vis_scene.c
        src/voxel.c
        src/vsm.c
    PUBLIC_DEPS nph::gpu
)
# The GPU records are declared once for C and GLSL (shaders/shared/
# nph_render.h); the shaders are built with the module so a renderer
# always finds its .spv files next to the build.
target_include_directories(nph_render PRIVATE "${CMAKE_SOURCE_DIR}/shaders/shared")
add_dependencies(nph_render nph_shaders_render)
if(MSVC)
    target_compile_definitions(nph_render PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(render
    SOURCES
        tests/test_ao.c
        tests/test_boxes.h
        tests/test_cell_proxy.c
        tests/test_cluster_lod.c
        tests/test_dof.c
        tests/test_frame.c
        tests/test_gi.c
        tests/test_gpu_profiler.c
        tests/test_levels_apart.h
        tests/test_motion_blur.c
        tests/test_post.c
        tests/test_primitives.c
        tests/test_reference.c
        tests/test_render_support.h
        tests/test_renderer.c
        tests/test_shadows.c
        tests/test_taa.c
        tests/test_vis.c
        tests/test_vis_cells.c
        tests/test_vis_lod.c
        tests/test_vis_voxel.c
        tests/test_voxel.c
        tests/test_vsm.c
    DEPS
        nph::render
    # On lavapipe in Debug with sanitizers every test pays seconds of device
    # and renderer setup, and a hosted runner can run 1.9 times slower than
    # usual: eight CTest entries keep each under the timeout there. (Six put
    # two of the four voxel GPU tests, 90 to 200 s each on such a runner, on
    # each of two entries, which passed 600 s; the four are 1, 5, 6 and 7
    # tests apart, so 5 to 7 entries pair them.)
    SHARDS 8
)
if(NPH_BUILD_TESTS)
    # White-box tests read the renderer's internal images; the gpu tests'
    # headless device fixture and readback helpers are shared.
    target_include_directories(render_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/engine/gpu/tests")
    nph_target_use_shaders(render_tests nph_shaders_render)
    if(MSVC)
        target_compile_definitions(render_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(NPH_GPU_REQUIRE_VALIDATION)
        target_compile_definitions(render_tests PRIVATE NPH_GPU_REQUIRE_VALIDATION=1)
    endif()
endif()
