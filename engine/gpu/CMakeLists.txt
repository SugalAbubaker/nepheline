# SPDX-License-Identifier: Apache-2.0
# The gpu module: the Vulkan RHI (CLAUDE.md §8.5; docs/gpu.md;
# docs/adr/0014-gpu-module.md). Depends on jobs (parallel command
# recording) and, through it, on platform (window surfaces, files, threads).

nph_add_module(gpu
    SOURCES
        src/bindless.c
        src/commands.c
        src/device.c
        src/frame.c
        src/pipeline.c
        src/render_graph.c
        src/resources.c
        src/shader.c
        src/spirv_reflect.c
        src/swapchain.c
        src/upload.c
        src/vk_util.c
        src/gpu_alloc.h
        src/gpu_internal.h
    PUBLIC_DEPS nph::jobs
)

# volk and the Vulkan headers are PUBLIC only because vk.h, the escape hatch
# for the editor backend, tests, and benchmarks, includes volk. No other
# engine module may include a Vulkan header (scripts/check_layering.py).
target_link_libraries(nph_gpu PUBLIC volk)
if(MSVC)
    target_compile_definitions(nph_gpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The Vulkan Memory Allocator implementation: one C++ translation unit with
# the library's own warning policy, never the engine's (CLAUDE.md §3).
enable_language(CXX)
add_library(nph_gpu_vma STATIC src/gpu_vma.cpp)
target_compile_features(nph_gpu_vma PRIVATE cxx_std_17)
target_link_libraries(nph_gpu_vma PRIVATE GPUOpen::VulkanMemoryAllocator Vulkan::Headers)
target_include_directories(nph_gpu_vma PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/engine/core/include")
if(MSVC)
    target_compile_options(nph_gpu_vma PRIVATE /W0)
else()
    target_compile_options(nph_gpu_vma PRIVATE -w)
endif()
set_target_properties(nph_gpu_vma PROPERTIES FOLDER "engine")
target_link_libraries(nph_gpu PRIVATE nph_gpu_vma)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(gpu
    SOURCES
        tests/test_commands.c
        tests/test_device.c
        tests/test_formats.c
        tests/test_gpu_support.h
        tests/test_pipeline.c
        tests/test_render_graph.c
        tests/test_resources.c
        tests/test_spirv.c
        tests/test_swapchain.c
        tests/test_upload.c
    DEPS
        nph::gpu
)
if(NPH_BUILD_TESTS)
    nph_target_use_shaders(gpu_tests nph_shaders_test)
    if(MSVC)
        target_compile_definitions(gpu_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
