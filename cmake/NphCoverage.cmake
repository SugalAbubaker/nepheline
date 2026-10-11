# SPDX-License-Identifier: Apache-2.0
# Source-based code coverage with Clang and llvm-cov (CLAUDE.md §9: >= 90 %
# line coverage for core, math, and jobs; >= 80 % elsewhere). Applied through
# nph::options when NPH_COVERAGE is ON, in every configuration, so a Debug
# build without sanitizers gives the cleanest numbers:
#
#   cmake --preset linux-clang-tidy -DCMAKE_BUILD_TYPE=Debug -DNPH_COVERAGE=ON \
#         -DNPH_ASAN=OFF -DNPH_UBSAN=OFF
#   LLVM_PROFILE_FILE=prof/%p-%m.profraw ctest --test-dir build/linux-clang-tidy
#   llvm-profdata merge -sparse prof/*.profraw -o coverage.profdata
#   llvm-cov report bin/core_tests -instr-profile=coverage.profdata
#
# The CI "coverage" job runs exactly that (.github/workflows/ci.yml).

function(nph_apply_coverage target)
    if(NOT NPH_COVERAGE)
        return()
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(WARNING "NPH_COVERAGE needs Clang with llvm-cov; ignoring for ${CMAKE_C_COMPILER_ID}.")
        return()
    endif()
    target_compile_options(${target} INTERFACE -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(${target} INTERFACE -fprofile-instr-generate)
endfunction()
