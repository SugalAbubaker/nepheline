# SPDX-License-Identifier: Apache-2.0
# The scene module: the archetype ECS, queries, command buffers, the system
# scheduler, transforms and the hierarchy, prefabs, and scene serialization
# (CLAUDE.md §8.7; docs/scene.md; docs/adr/0020-ecs.md). Depends on jobs
# (the scheduler) and through it on platform, math, and core.

nph_add_module(scene
    SOURCES
        src/command.c
        src/component.c
        src/prefab.c
        src/query.c
        src/schedule.c
        src/scene_binary.c
        src/scene_internal.h
        src/scene_io.c
        src/scene_io_internal.h
        src/scene_text.c
        src/transform.c
        src/world.c
    PUBLIC_DEPS nph::jobs
)
if(MSVC)
    target_compile_definitions(nph_scene PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(scene
    SOURCES
        tests/test_command.c
        tests/test_component.c
        tests/test_components.c
        tests/test_prefab.c
        tests/test_query.c
        tests/test_scene_copy.c
        tests/test_scene_io.c
        tests/test_scene_support.h
        tests/test_schedule.c
        tests/test_suspend.c
        tests/test_transform.c
        tests/test_world.c
    DEPS
        nph::scene
)
if(NPH_BUILD_TESTS)
    # White-box tests read the module's internal structures.
    target_include_directories(scene_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # The fuzz seed corpora (scripts/fuzz_seeds_scene.py) are an independent
    # description of the scene formats; the tests load them.
    target_compile_definitions(scene_tests PRIVATE
        "NPH_FUZZ_CORPUS_DIR=\"${CMAKE_SOURCE_DIR}/tests/fuzz/corpus\"")
    if(MSVC)
        target_compile_definitions(scene_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
