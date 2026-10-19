# SPDX-License-Identifier: Apache-2.0
# The render module: the forward PBR reference renderer, image-based
# lighting, tonemapping, debug draw, the GPU profiler, and procedural
# primitives (CLAUDE.md §8.6 stages 1-2; docs/render.md;
# docs/adr/0018-reference-renderer.md). Depends on gpu and, through it, on
# jobs, platform, math, and core.

nph_add_module(render
    SOURCES
        src/debug_draw.c
        src/dof.c
        src/frame.c
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
        tests/test_dof.c
        tests/test_frame.c
        tests/test_gpu_profiler.c
        tests/test_motion_blur.c
        tests/test_post.c
        tests/test_primitives.c
        tests/test_reference.c
        tests/test_render_support.h
        tests/test_renderer.c
        tests/test_shadows.c
        tests/test_taa.c
        tests/test_vis.c
        tests/test_vsm.c
    DEPS
        nph::render
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
