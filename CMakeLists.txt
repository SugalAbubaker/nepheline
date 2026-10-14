# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The version number is owned by the C header so that code, CMake, and the
# packaged artifacts can never disagree. CMake reads it rather than generating it.
include(NphVersion)
nph_read_version_header("${CMAKE_CURRENT_SOURCE_DIR}/engine/core/include/nepheline/core/version.h"
                        NPH_VERSION)

# Replace CMake's default configuration set with ours before any language is
# enabled (see docs/build.md and docs/adr/0003-build-configurations.md).
include(NphConfigurations)
nph_declare_configurations()

project(Nepheline
    VERSION ${NPH_VERSION}
    DESCRIPTION "A production-grade, GPU-driven 3D game engine in C17 and Vulkan 1.3"
    HOMEPAGE_URL "https://github.com/SugalAbubaker/nepheline"
    LANGUAGES C CXX)
# C++ exists for exactly one engine translation unit, the VMA implementation
# in engine/gpu (CLAUDE.md §3), and for the editor's cimgui later. It is
# enabled here rather than in engine/gpu because CMake propagates a
# language's linker preference only within the directory scope that enabled
# it: enabling it in engine/gpu left samples and benchmarks linking with the
# C driver and without the C++ runtime on Linux.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset <name>")
endif()

include(NphOptions)
nph_apply_configuration_flags()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Multi-config generators append /<Config> to these automatically.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(NphWarnings)
include(NphSanitizers)
include(NphArch)
include(NphCoverage)
include(NphModule)
include(NphShaders)

# nph::options carries every compiler setting shared by all Nepheline code:
# the C standard, warnings-as-errors, per-configuration defines, sanitizers.
# Every engine module, tool, test, and sample links it.
add_library(nph_options INTERFACE)
add_library(nph::options ALIAS nph_options)
target_compile_features(nph_options INTERFACE c_std_17)
# libm is part of the C runtime everywhere except Windows, where the CRT has it.
target_link_libraries(nph_options INTERFACE $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
nph_apply_warnings(nph_options)
nph_apply_configuration_definitions(nph_options)
nph_apply_sanitizers(nph_options)
nph_apply_arch_flags(nph_options)
nph_apply_coverage(nph_options)

if(NPH_BUILD_TESTS)
    enable_testing()
    # JUnit reports from ntest (per configuration) and from `ctest --output-junit`.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-results")
    add_subdirectory(tools/ntest)
endif()

# Vendored libraries (pinned submodules), before the modules that wrap them.
add_subdirectory(third_party)

# Engine modules, in dependency order (CLAUDE.md §4).
add_subdirectory(engine/core)
add_subdirectory(engine/math)
add_subdirectory(engine/platform)
add_subdirectory(engine/jobs)
add_subdirectory(engine/gpu)
add_subdirectory(engine/asset)

# Shader sources shared by the engine, its tests, and the samples.
add_subdirectory(shaders)

# The aggregate target a game or tool links to get the whole engine.
add_library(nepheline INTERFACE)
target_link_libraries(nepheline INTERFACE nph::core nph::math nph::platform nph::jobs nph::gpu
                                          nph::asset)

if(NPH_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(NPH_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
if(NPH_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

include(NphFormat)
nph_add_format_targets()

message(STATUS "Nepheline ${PROJECT_VERSION}: compiler=${CMAKE_C_COMPILER_ID} "
               "frontend=${CMAKE_C_COMPILER_FRONTEND_VARIANT} generator=${CMAKE_GENERATOR} "
               "asan=${NPH_ASAN} ubsan=${NPH_UBSAN} tsan=${NPH_TSAN} tests=${NPH_BUILD_TESTS} "
               "benchmarks=${NPH_BUILD_BENCHMARKS} samples=${NPH_BUILD_SAMPLES} "
               "fuzzers=${NPH_BUILD_FUZZERS}")
