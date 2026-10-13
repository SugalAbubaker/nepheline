# SPDX-License-Identifier: Apache-2.0
# GPU microbenchmarks (CLAUDE.md §12 Phase 5; docs/benchmarks.md "GPU
# benchmarks"). Not on nbench: GPU time comes from timestamp queries, so
# the executable has its own harness with the same JSON schema and exit
# codes. The CTest smoke run skips (exit 77) without a Vulkan device; the
# self-hosted RTX 3060 Ti runner runs it for real (.github/workflows/gpu-perf.yml).

add_executable(gpu_bench bench_gpu.c)
target_link_libraries(gpu_bench PRIVATE nph::options nph::gpu)
nph_target_use_shaders(gpu_bench nph_shaders_bench)
set_target_properties(gpu_bench PROPERTIES FOLDER "benchmarks")
if(MSVC)
    target_compile_definitions(gpu_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(NPH_BUILD_TESTS)
    add_test(NAME bench.gpu COMMAND gpu_bench --repeats=1)
    set_tests_properties(bench.gpu PROPERTIES TIMEOUT 600 LABELS "benchmark" SKIP_RETURN_CODE 77)
endif()
