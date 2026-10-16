# SPDX-License-Identifier: Apache-2.0
# The runtime module: the engine's lifecycle and frame loop, configuration,
# the console, simulation input, the ECS-to-renderer bridge, and game
# modules with hot reload (CLAUDE.md §8.12; docs/runtime.md;
# docs/adr/0023-runtime.md, docs/adr/0024-game-modules.md). The last
# module in the order: it depends on every other.

nph_add_module(runtime
    SOURCES
        src/clock.c
        src/components.c
        src/config.c
        src/console.c
        src/render_assets.c
        src/runtime_internal.h
        src/scene_import.c
        src/scene_render.c
        src/sim_input.c
    PUBLIC_DEPS nph::render nph::asset nph::scene nph::profile
)
if(MSVC)
    target_compile_definitions(nph_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(runtime
    SOURCES
        tests/test_clock.c
        tests/test_console.c
        tests/test_scene_render.c
        tests/test_sim_input.c
    DEPS
        nph::runtime
)
if(NPH_BUILD_TESTS)
    if(MSVC)
        target_compile_definitions(runtime_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    # The GPU tests share the gpu and render tests' helpers: the headless
    # device that skips without Vulkan, the validation gate, the leak
    # checker, and a small renderer.
    target_include_directories(runtime_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/gpu/tests"
        "${CMAKE_SOURCE_DIR}/engine/render/tests")
    nph_target_use_shaders(runtime_tests nph_shaders_render)
    if(NPH_GPU_REQUIRE_VALIDATION)
        target_compile_definitions(runtime_tests PRIVATE NPH_GPU_REQUIRE_VALIDATION=1)
    endif()
    # The scene renderer's tests draw the asset viewer's showcase scene,
    # cooked when they are built, as the samples' content is. nph_cook is
    # defined later (tools/cook); its name resolves when the build system
    # is generated.
    set(runtime_content "${CMAKE_SOURCE_DIR}/samples/asset_viewer/content")
    set(runtime_out "${CMAKE_CURRENT_BINARY_DIR}/test_content/$<CONFIG>")
    add_custom_command(
        OUTPUT "${runtime_out}/showcase.npk"
        COMMAND nph_cook "${runtime_content}/showcase.gltf" --root "${runtime_content}"
                -o "${runtime_out}/cooked" --pack "${runtime_out}/showcase.npk" --quiet
        DEPENDS nph_cook "${runtime_content}/showcase.gltf"
        COMMENT "Cooking the runtime tests' content"
        VERBATIM)
    add_custom_target(runtime_test_content DEPENDS "${runtime_out}/showcase.npk")
    set_target_properties(runtime_test_content PROPERTIES FOLDER "tests")
    add_dependencies(runtime_tests runtime_test_content)
    target_compile_definitions(runtime_tests PRIVATE
        "NPH_TEST_PACK=\"${runtime_out}/showcase.npk\"")
endif()
