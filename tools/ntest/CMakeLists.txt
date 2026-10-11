# SPDX-License-Identifier: Apache-2.0
# ntest: the Nepheline test framework (docs/ntest.md). Depends only on the C
# standard library and the OS, never on engine modules, since it tests them.

add_library(ntest STATIC
    src/ntest.c
    src/ntest_internal.h
    include/ntest/ntest.h
)
add_library(ntest::ntest ALIAS ntest)
target_include_directories(ntest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ntest PUBLIC nph::options)
if(MSVC)
    # fopen/getenv/gmtime are portable C; the "secure" variants are not.
    target_compile_definitions(ntest PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(ntest PROPERTIES FOLDER "tools")

# ntest::main supplies main(); link it from test executables.
add_library(ntest_main STATIC src/ntest_main.c)
add_library(ntest::main ALIAS ntest_main)
target_link_libraries(ntest_main PUBLIC ntest::ntest)
set_target_properties(ntest_main PROPERTIES FOLDER "tools")

add_subdirectory(tests)
