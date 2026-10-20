# SPDX-License-Identifier: Apache-2.0
# The render module's CPU-side work and the golden toolkit's comparisons.
nph_add_benchmark(render
    SOURCES bench_render.c
    DEPS nph::render nph::golden)

# The renderer on a device: its CPU and GPU cost per scene (CLAUDE.md §10.7;
# docs/benchmarks.md "Renderer runs"). Its own harness, like gpu_bench: GPU
# times come from the renderer's GPU profiler. The CTest smoke run renders
# the smallest scenes and skips (77) without a Vulkan device; the
# self-hosted RTX 3060 Ti runner runs every scene (.github/workflows/gpu-perf.yml).
#
# The reference scenes (benchmarks/scenes/*.json, cooked by
# scripts/scenes.py) load through the asset loader and samples/common's
# asset-to-renderer bridge, as the golden tests do.
add_executable(render_perf render_perf.c
    "${CMAKE_SOURCE_DIR}/samples/common/cooked_scene.c"
    "${CMAKE_SOURCE_DIR}/samples/common/cooked_scene.h"
    "${CMAKE_SOURCE_DIR}/samples/common/capture.c"
    "${CMAKE_SOURCE_DIR}/samples/common/capture.h")
target_link_libraries(render_perf PRIVATE nph::options nph::render nph::asset nph::jobs)
target_include_directories(render_perf PRIVATE "${CMAKE_SOURCE_DIR}/samples/common")
target_compile_definitions(render_perf PRIVATE
    "NPH_SCENE_MANIFEST_DIR=\"${CMAKE_SOURCE_DIR}/benchmarks/scenes\"")
nph_target_use_shaders(render_perf nph_shaders_render)
set_target_properties(render_perf PROPERTIES FOLDER "benchmarks")
if(MSVC)
    target_compile_definitions(render_perf PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(NPH_BUILD_TESTS)
    add_test(NAME bench.render_perf COMMAND render_perf --filter=grid_1k_* --frames=4)
    set_tests_properties(bench.render_perf PROPERTIES
        TIMEOUT 600 LABELS "benchmark" SKIP_RETURN_CODE 77)
    # The reference-scene path, on pbr_viewer's content cooked here (CI has
    # none of the third-party scenes): test_scenes/pbr.json, rendered small.
    if(TARGET nph_cook)
        set(perf_content "${CMAKE_SOURCE_DIR}/samples/pbr_viewer/content")
        set(perf_scenes "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/scenes")
        add_custom_command(
            OUTPUT "${perf_scenes}/pbr_scene.npk"
            COMMAND nph_cook "${perf_content}/pbr_scene.gltf" --root "${perf_content}"
                    -o "${perf_scenes}/cooked" --pack "${perf_scenes}/pbr_scene.npk" --quiet
            DEPENDS nph_cook "${perf_content}/pbr_scene.gltf"
            COMMENT "Cooking render_perf's test scene"
            VERBATIM)
        add_custom_target(render_perf_content DEPENDS "${perf_scenes}/pbr_scene.npk")
        set_target_properties(render_perf_content PROPERTIES FOLDER "benchmarks")
        add_dependencies(render_perf render_perf_content)
        add_test(NAME bench.render_perf_reference
            COMMAND render_perf "--manifests=${CMAKE_CURRENT_SOURCE_DIR}/test_scenes"
                    "--scenes=${perf_scenes}" --filter=pbr_* --frames=4 --ref-size=320x180)
        set_tests_properties(bench.render_perf_reference PROPERTIES
            TIMEOUT 600 LABELS "benchmark" SKIP_RETURN_CODE 77)
    endif()
endif()
