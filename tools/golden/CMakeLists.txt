# SPDX-License-Identifier: Apache-2.0
# The golden-image toolkit (CLAUDE.md §9 "GPU golden-image tests ... compare
# with NVIDIA FLIP against approved baselines"; docs/golden.md): nph::golden,
# a library with our own PNG codec and LDR-FLIP, and nph_flip, the
# command-line comparison the approval workflow uses. It depends on
# platform for file access and nothing else of the engine.

add_library(nph_golden STATIC
    src/flip.c
    src/inflate.c
    src/png.c
)
add_library(nph::golden ALIAS nph_golden)
target_include_directories(nph_golden PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(nph_golden PUBLIC nph::options nph::platform)
if(MSVC)
    target_compile_definitions(nph_golden PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(nph_golden PROPERTIES FOLDER "tools")
file(GLOB golden_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/golden/*.h")
target_sources(nph_golden PRIVATE ${golden_headers})

add_executable(nph_flip src/main.c)
target_link_libraries(nph_flip PRIVATE nph::options nph::golden)
if(MSVC)
    target_compile_definitions(nph_flip PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(nph_flip PROPERTIES FOLDER "tools")

nph_add_test(golden_tools
    SOURCES
        tests/test_flip.c
        tests/test_png.c
    DEPS
        nph::golden
)
if(NPH_BUILD_TESTS AND MSVC)
    target_compile_definitions(golden_tools_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(NPH_BUILD_TESTS)
    target_compile_definitions(golden_tools_tests PRIVATE
        "NPH_TEST_TEMP_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\""
        "NPH_FUZZ_CORPUS_DIR=\"${CMAKE_SOURCE_DIR}/tests/fuzz/corpus\"")
endif()
