# SPDX-License-Identifier: Apache-2.0
# The profile module: the collecting half of CPU profiling (the recording
# half is core/profile.h), the Chrome trace export, and tracked allocators
# (CLAUDE.md §8.11; docs/profile.md; docs/adr/0022-profiling.md). Depends
# on platform (the time-stamp counter's rate, files) and through it on
# core.

nph_add_module(profile
    SOURCES
        src/chrome_trace.c
        src/memory.c
        src/profile_internal.h
        src/profiler.c
    PUBLIC_DEPS nph::platform
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(profile
    SOURCES
        tests/test_chrome_trace.c
        tests/test_memory.c
        tests/test_profile_support.h
        tests/test_profiler.c
        tests/test_profiler_jobs.c
    DEPS
        nph::profile
        nph::jobs
)
if(NPH_BUILD_TESTS)
    # White-box tests read the collector's internal state.
    target_include_directories(profile_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_definitions(profile_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
