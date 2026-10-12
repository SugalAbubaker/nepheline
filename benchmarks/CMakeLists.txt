# SPDX-License-Identifier: Apache-2.0
# Performance benchmarks (CLAUDE.md §1.5, §9). Each executable is built on
# nbench and registered with CTest under the "benchmark" label as a short
# smoke run, so a benchmark that stops compiling or crashes is caught by the
# normal test matrix. The nightly workflow runs them for real and archives
# the JSON (docs/benchmarks.md).

add_subdirectory(nbench)

add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(platform)
add_subdirectory(jobs)
