# SPDX-License-Identifier: Apache-2.0
# The platform module: OS abstraction (CLAUDE.md §8.3). Sources are split by
# operating system; the public headers are the same everywhere.

set(platform_sources
    src/async_io.c
    src/cpu.c
    src/file_common.c
    src/locked_alloc.c
    src/log_thread.c
    src/path.c
    src/platform_common.c
    src/thread_common.c
    src/time_common.c
)
if(WIN32)
    list(APPEND platform_sources
        src/async_io_iocp.c
        src/cpu_win32.c
        src/dylib_win32.c
        src/file_win32.c
        src/platform_win32.c
        src/thread_win32.c
        src/time_win32.c
        src/vmem_win32.c
    )
else()
    list(APPEND platform_sources
        src/async_io_uring.c
        src/cpu_linux.c
        src/dylib_posix.c
        src/file_posix.c
        src/platform_posix.c
        src/thread_posix.c
        src/time_posix.c
        src/vmem_posix.c
    )
endif()

# Fibers: the assembly context switch by default, the Win32 fiber API on
# request (docs/adr/0010-fibers.md). The assembly lives in its own target so
# the C warning and sanitizer flags of nph::options never reach the
# assembler.
if(WIN32 AND NPH_FIBERS_WIN32)
    list(APPEND platform_sources src/fiber_win32.c)
else()
    list(APPEND platform_sources src/fiber.c)
    if(WIN32)
        enable_language(ASM_MASM)
        add_library(nph_platform_fiber_asm STATIC src/fiber_win64.asm)
    else()
        enable_language(ASM)
        add_library(nph_platform_fiber_asm STATIC src/fiber_sysv_x64.S)
    endif()
    set_target_properties(nph_platform_fiber_asm PROPERTIES FOLDER "engine")
endif()

nph_add_module(platform
    SOURCES ${platform_sources}
    PUBLIC_DEPS nph::math
)
if(TARGET nph_platform_fiber_asm)
    target_link_libraries(nph_platform PRIVATE nph_platform_fiber_asm)
endif()

if(WIN32)
    # fopen and friends are portable C; the "secure" CRT variants are not.
    target_compile_definitions(nph_platform PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # pthread_setaffinity_np, pthread_setname_np, sem_clockwait, MADV_HUGEPAGE.
    target_compile_definitions(nph_platform PRIVATE _GNU_SOURCE)
    find_package(Threads REQUIRED)
    target_link_libraries(nph_platform PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(platform
    SOURCES
        tests/test_async_io.c
        tests/test_core_concurrent.c
        tests/test_cpu.c
        tests/test_dylib.c
        tests/test_fiber.c
        tests/test_file.c
        tests/test_locked_alloc.c
        tests/test_log_thread.c
        tests/test_path.c
        tests/test_platform.c
        tests/test_platform_support.h
        tests/test_sync.c
        tests/test_thread.c
        tests/test_time.c
        tests/test_vmem.c
    DEPS
        nph::platform
)

if(NPH_BUILD_TESTS)
    # A shared library for the dylib tests to load. Built with the engine's
    # options so a Debug build is instrumented the same way as the test
    # executable that loads it.
    add_library(platform_test_dylib SHARED tests/dylib_fixture.c)
    target_link_libraries(platform_test_dylib PRIVATE nph::options nph::core)
    set_target_properties(platform_test_dylib PROPERTIES FOLDER "tests")
    add_dependencies(platform_tests platform_test_dylib)
    target_compile_definitions(platform_tests PRIVATE
        "NPH_TEST_DYLIB_PATH=\"$<TARGET_FILE:platform_test_dylib>\"")
    if(MSVC)
        target_compile_definitions(platform_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
