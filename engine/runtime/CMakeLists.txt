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
        src/engine.c
        src/engine_internal.h
        src/game_host.c
        src/physics_bridge.c
        src/render_assets.c
        src/runtime_internal.h
        src/scene_import.c
        src/scene_render.c
        src/sim_input.c
    PUBLIC_DEPS nph::render nph::asset nph::scene nph::physics nph::profile
)
if(MSVC)
    target_compile_definitions(nph_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# The renderer's compiled shaders, where the engine finds them by default.
nph_target_use_shaders(nph_runtime nph_shaders_render)

# nph_player: runs a game module (player/main.c, docs/runtime.md "The
# player"). It hosts game modules, so it runs the engine as a shared library.
add_executable(nph_player player/main.c)
target_link_libraries(nph_player PRIVATE nph::shared)
set_target_properties(nph_player PROPERTIES FOLDER "tools")
if(MSVC)
    target_compile_definitions(nph_player PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(runtime
    SOURCES
        tests/test_clock.c
        tests/test_console.c
        tests/test_engine.c
        tests/test_physics_bridge.c
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

# The game module tests run against the engine as a shared library, as the
# player and the editor do, loading three builds of one test game.
if(NPH_BUILD_TESTS)
    foreach(v 1 2 3)
        nph_add_game_module(test_game_v${v} SOURCES tests/games/test_game.c)
        target_compile_definitions(test_game_v${v} PRIVATE TEST_GAME_VERSION=${v})
        set_target_properties(test_game_v${v} PROPERTIES FOLDER "tests")
    endforeach()
endif()
nph_add_test(runtime_game
    SOURCES
        tests/test_game_modules.c
        tests/games/test_game.h
    DEPS
        nph::shared
)
if(NPH_BUILD_TESTS)
    add_dependencies(runtime_game_tests test_game_v1 test_game_v2 test_game_v3)
    target_compile_definitions(runtime_game_tests PRIVATE
        "TEST_GAME_V1=\"$<TARGET_FILE:test_game_v1>\""
        "TEST_GAME_V2=\"$<TARGET_FILE:test_game_v2>\""
        "TEST_GAME_V3=\"$<TARGET_FILE:test_game_v3>\"")
endif()
