# SPDX-License-Identifier: Apache-2.0
nph_add_benchmark(runtime
    SOURCES bench_runtime.c
    DEPS nph::runtime)

# The scene renderer's benchmarks draw the asset viewer's showcase, cooked
# here when the cooker is built (they are labelled not measured otherwise,
# and without a Vulkan device).
if(TARGET runtime_bench AND TARGET nph_cook)
    set(bench_content "${CMAKE_SOURCE_DIR}/samples/asset_viewer/content")
    set(bench_out "${CMAKE_CURRENT_BINARY_DIR}/content/$<CONFIG>")
    add_custom_command(
        OUTPUT "${bench_out}/showcase.npk"
        COMMAND nph_cook "${bench_content}/showcase.gltf" --root "${bench_content}"
                -o "${bench_out}/cooked" --pack "${bench_out}/showcase.npk" --quiet
        DEPENDS nph_cook "${bench_content}/showcase.gltf"
        COMMENT "Cooking the runtime benchmarks' content"
        VERBATIM)
    add_custom_target(runtime_bench_content DEPENDS "${bench_out}/showcase.npk")
    set_target_properties(runtime_bench_content PROPERTIES FOLDER "benchmarks")
    add_dependencies(runtime_bench runtime_bench_content)
    target_compile_definitions(runtime_bench PRIVATE
        "NPH_BENCH_SHOWCASE_PACK=\"${bench_out}/showcase.npk\"")
endif()
