# SPDX-License-Identifier: Apache-2.0
# Cache options that control what gets built and how it is instrumented.

option(NPH_BUILD_TESTS "Build the unit tests and the ntest framework" ON)
option(NPH_BUILD_BENCHMARKS "Build the benchmarks and the nbench harness" ON)
option(NPH_BUILD_SAMPLES "Build the sample applications" ON)
option(NPH_BUILD_FUZZERS "Build the libFuzzer targets (Clang only; links -fsanitize=fuzzer)" OFF)
option(NPH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (CLAUDE.md §1.3)" ON)

# Sanitizers apply to the Debug configuration only (CLAUDE.md §6).
# clang-cl's AddressSanitizer runtime conflicts with the MSVC CRT's copy of the
# same library when link.exe resolves it, so it is off by default there until
# verified in CI (see docs/build.md, "Known gaps").
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(_nph_asan_default OFF)
else()
    set(_nph_asan_default ON)
endif()
option(NPH_ASAN "Enable AddressSanitizer in the Debug configuration" ${_nph_asan_default})
option(NPH_UBSAN "Enable UndefinedBehaviorSanitizer in the Debug configuration (GCC/Clang)" ON)
option(NPH_TSAN "Enable ThreadSanitizer in the Debug configuration (GCC/Clang; excludes ASan)" OFF)
unset(_nph_asan_default)

if(NPH_ASAN AND NPH_TSAN)
    message(FATAL_ERROR "NPH_ASAN and NPH_TSAN are mutually exclusive; enable one of them.")
endif()
