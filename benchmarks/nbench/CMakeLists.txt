# SPDX-License-Identifier: Apache-2.0
# nbench: the Nepheline benchmark harness (docs/benchmarks.md). Depends only
# on the C library and the OS.

add_library(nbench STATIC
    src/nbench.c
    include/nbench/nbench.h
)
add_library(nbench::nbench ALIAS nbench)
target_include_directories(nbench PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(nbench PUBLIC nph::options)
if(MSVC)
    target_compile_definitions(nbench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(nbench PROPERTIES FOLDER "benchmarks")

add_library(nbench_main STATIC src/nbench_main.c)
add_library(nbench::main ALIAS nbench_main)
target_link_libraries(nbench_main PUBLIC nbench::nbench)
set_target_properties(nbench_main PROPERTIES FOLDER "benchmarks")
