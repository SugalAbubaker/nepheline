# SPDX-License-Identifier: Apache-2.0
# The job system (CLAUDE.md §8.4): fiber-based workers with work-stealing
# deques, counters, priorities, and I/O threads. Depends on platform for
# threads, fibers, and CPU topology.

nph_add_module(jobs
    SOURCES
        src/job_deque.c
        src/jobs.c
    PUBLIC_DEPS nph::platform
)

if(WIN32)
    target_compile_definitions(nph_jobs PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(jobs
    SOURCES
        tests/test_deque.c
        tests/test_counter.c
        tests/test_jobs.c
        tests/test_jobs_stress.c
        tests/test_jobs_support.h
    DEPS
        nph::jobs
)
