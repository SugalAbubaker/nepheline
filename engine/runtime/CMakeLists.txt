# SPDX-License-Identifier: Apache-2.0
# The runtime module: the engine's lifecycle and frame loop, configuration,
# the console, simulation input, the ECS-to-renderer bridge, and game
# modules with hot reload (CLAUDE.md §8.12; docs/runtime.md;
# docs/adr/0023-runtime.md, docs/adr/0024-game-modules.md). The last
# module in the order: it depends on every other.

nph_add_module(runtime
    SOURCES
        src/clock.c
        src/config.c
        src/console.c
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
        tests/test_sim_input.c
    DEPS
        nph::runtime
)
if(NPH_BUILD_TESTS AND MSVC)
    target_compile_definitions(runtime_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
