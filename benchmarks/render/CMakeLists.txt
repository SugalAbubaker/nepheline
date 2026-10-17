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
add_executable(render_perf render_perf.c)
target_link_libraries(render_perf PRIVATE nph::options nph::render)
nph_target_use_shaders(render_perf nph_shaders_render)
set_target_properties(render_perf PROPERTIES FOLDER "benchmarks")
if(MSVC)
    target_compile_definitions(render_perf PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(NPH_BUILD_TESTS)
    add_test(NAME bench.render_perf COMMAND render_perf --filter=grid_1k_* --frames=4)
    set_tests_properties(bench.render_perf PROPERTIES
        TIMEOUT 600 LABELS "benchmark" SKIP_RETURN_CODE 77)
endif()
