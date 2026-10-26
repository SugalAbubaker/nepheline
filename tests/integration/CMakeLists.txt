# SPDX-License-Identifier: Apache-2.0
# Integration tests (CLAUDE.md §4 "tests/: integration tests", §9): tests
# that span modules and tools, which no single module's tests may depend
# on. They share the gpu tests' device helpers (the headless device that
# skips without Vulkan, the validation gate, the leak checker) and the
# cooker tests' scene generator.

nph_add_test(integration
    SOURCES
        test_bc_gpu.c
        test_cook_load.c
    DEPS
        nph::cook
        nph::gpu
        nph::asset
)
if(NPH_BUILD_TESTS)
    target_include_directories(integration_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/gpu/tests"
        "${CMAKE_SOURCE_DIR}/tools/cook/tests")
    nph_target_use_shaders(integration_tests nph_shaders_test)
    if(MSVC)
        target_compile_definitions(integration_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(NPH_GPU_REQUIRE_VALIDATION)
        target_compile_definitions(integration_tests PRIVATE NPH_GPU_REQUIRE_VALIDATION=1)
    endif()
    set_tests_properties(integration PROPERTIES LABELS "integration")
endif()

# The open-landscape reference scene's layout (scripts/landscape.py): its
# self-test lays a small scene out over fake assets and checks it (the
# merged document's indices, every instance on the ground and off the
# trail, the same layout twice, the sun), with Python's standard library.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(NPH_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME scripts.landscape
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/scripts/landscape.py" --self-test)
    set_tests_properties(scripts.landscape PROPERTIES LABELS "integration")
endif()
