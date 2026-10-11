# SPDX-License-Identifier: Apache-2.0
# Helpers that define engine modules and their tests with one call each, so
# every module gets identical settings (CLAUDE.md §4, §6).

# nph_add_module(<name> SOURCES <src>... [PUBLIC_DEPS <target>...] [PRIVATE_DEPS <target>...])
#
# Creates the static library nph_<name> with alias nph::<name>, exposing
# include/ as its public include directory. Headers under include/ and src/
# are attached for IDE navigation only.
function(nph_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "nph_add_module(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "nph_add_module(${name}): SOURCES is required")
    endif()

    set(target nph_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(nph::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${target}
        PUBLIC nph::options ${ARG_PUBLIC_DEPS}
        PRIVATE ${ARG_PRIVATE_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "engine")

    file(GLOB_RECURSE headers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
    target_sources(${target} PRIVATE ${headers})
endfunction()

# nph_add_test(<name> SOURCES <src>... [DEPS <target>...])
#
# Builds the executable <name>_tests on the ntest framework and registers it
# with CTest as test "<name>". Each run writes a JUnit report to
# <build>/test-results/<Config>/<name>.xml for CI.
function(nph_add_test name)
    if(NOT NPH_BUILD_TESTS)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "nph_add_test(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(target ${name}_tests)
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE nph::options ntest::main ${ARG_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "tests")

    add_test(NAME ${name}
             COMMAND ${target} "--junit=${CMAKE_BINARY_DIR}/test-results/$<CONFIG>/${name}.xml")
    set_tests_properties(${name} PROPERTIES TIMEOUT 600 LABELS "unit")
endfunction()

# nph_add_sample(<name> SOURCES <src>... [DEPS <target>...] [ARGS <arg>...])
#
# Builds the executable <name> and registers "sample.<name>" with CTest
# (label "sample") so every sample keeps running.
function(nph_add_sample name)
    if(NOT NPH_BUILD_SAMPLES)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS;ARGS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "nph_add_sample(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE nph::options ${ARG_DEPS})
    set_target_properties(${name} PROPERTIES FOLDER "samples")
    if(MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    if(NPH_BUILD_TESTS)
        add_test(NAME sample.${name} COMMAND ${name} ${ARG_ARGS})
        set_tests_properties(sample.${name} PROPERTIES TIMEOUT 120 LABELS "sample")
    endif()
endfunction()

# nph_add_fuzzer(<name> SOURCES <src>... [DEPS <target>...])
#
# Builds a libFuzzer target (Clang only, NPH_BUILD_FUZZERS=ON) as
# <name>_fuzz. The entry point is LLVMFuzzerTestOneInput. With any other
# compiler the function does nothing.
function(nph_add_fuzzer name)
    if(NOT NPH_BUILD_FUZZERS)
        return()
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(STATUS "nph_add_fuzzer(${name}): libFuzzer needs Clang; skipping")
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "nph_add_fuzzer(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(target ${name}_fuzz)
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE nph::options ${ARG_DEPS})
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${target} PROPERTIES FOLDER "fuzz")
endfunction()

# nph_add_benchmark(<name> SOURCES <src>... [DEPS <target>...])
#
# Builds the executable <name>_bench on the nbench harness and registers a
# short smoke run with CTest as "bench.<name>" (label "benchmark") so a
# benchmark that no longer builds or runs fails the normal test matrix. The
# nightly workflow runs the executables at full length with --json.
function(nph_add_benchmark name)
    if(NOT NPH_BUILD_BENCHMARKS)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "nph_add_benchmark(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(target ${name}_bench)
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE nph::options nbench::main ${ARG_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "benchmarks")

    if(NPH_BUILD_TESTS)
        add_test(NAME bench.${name} COMMAND ${target} --min-time=0.005 --repeats=1)
        set_tests_properties(bench.${name} PROPERTIES TIMEOUT 600 LABELS "benchmark")
    endif()
endfunction()
