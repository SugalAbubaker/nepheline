# SPDX-License-Identifier: Apache-2.0
# The platform module: OS abstraction (CLAUDE.md §8.3). Sources are split by
# operating system; the public headers are the same everywhere.

set(platform_sources
    src/async_io.c
    src/cpu.c
    src/crash_common.c
    src/file_common.c
    src/input.c
    src/locked_alloc.c
    src/log_thread.c
    src/path.c
    src/platform_common.c
    src/thread_common.c
    src/time_common.c
    src/window_glfw.c
)
if(WIN32)
    list(APPEND platform_sources
        src/async_io_iocp.c
        src/cpu_win32.c
        src/crash_win32.c
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
        src/crash_posix.c
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
# GLFW is private: only window_glfw.c includes its header (CLAUDE.md §1.8).
target_link_libraries(nph_platform PRIVATE glfw)

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
        tests/test_crash.c
        tests/test_dylib.c
        tests/test_fiber.c
        tests/test_file.c
        tests/test_input.c
        tests/test_locked_alloc.c
        tests/test_log_thread.c
        tests/test_path.c
        tests/test_platform.c
        tests/test_platform_support.h
        tests/test_realtime.c
        tests/test_sync.c
        tests/test_thread.c
        tests/test_time.c
        tests/test_vmem.c
        tests/test_window.c
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
    # White-box tests read the module's internal structures.
    target_include_directories(platform_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(platform_tests PRIVATE
        "NPH_TEST_DYLIB_PATH=\"$<TARGET_FILE:platform_test_dylib>\"")
    if(MSVC)
        target_compile_definitions(platform_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(NOT WIN32)
        # The crash tests' siginfo_t, sigaltstack, and SS_DISABLE are POSIX's,
        # which glibc declares under -std=c17 only when asked, as for the module.
        target_compile_definitions(platform_tests PRIVATE _GNU_SOURCE)
    endif()

    # The crash handler on real faults (docs/adr/0103-crash-handler.md):
    # crash_helper fails in a child process, and run_crash.cmake checks the
    # report it left, the minidump beside it (Windows, read back by
    # crash_dump_check), and the log line delivered after them. Where the
    # handler does not install (the sanitizers' builds) the tests are skipped.
    add_executable(crash_helper tests/crash_helper.c)
    target_link_libraries(crash_helper PRIVATE nph::options nph::platform)
    set_target_properties(crash_helper PROPERTIES FOLDER "tests")
    set(crash_dump_check "")
    if(WIN32)
        target_compile_definitions(crash_helper PRIVATE _CRT_SECURE_NO_WARNINGS)
        add_executable(crash_dump_check tests/crash_dump_check.c)
        target_link_libraries(crash_dump_check PRIVATE nph::options nph::core dbghelp)
        target_compile_definitions(crash_dump_check PRIVATE _CRT_SECURE_NO_WARNINGS)
        set_target_properties(crash_dump_check PROPERTIES FOLDER "tests")
        set(crash_dump_check "$<TARGET_FILE:crash_dump_check>")
    endif()

    # nph_crash_test(<mode> [REPORTS <n>] [LOG <substring>] [EXPECT <substring>...]
    #                [ON_WINDOWS <substring>...] [ON_LINUX <substring>...]
    #                [DUMP <substring>...])
    function(nph_crash_test mode)
        cmake_parse_arguments(PARSE_ARGV 1 ARG "" "REPORTS;LOG" "EXPECT;ON_WINDOWS;ON_LINUX;DUMP")
        set(expect ${ARG_EXPECT})
        set(dump_check "")
        set(dump_expect "")
        if(WIN32)
            list(APPEND expect ${ARG_ON_WINDOWS})
            if(ARG_DUMP)
                set(dump_check "${crash_dump_check}")
                set(dump_expect ${ARG_DUMP})
            endif()
        else()
            list(APPEND expect ${ARG_ON_LINUX})
        endif()
        if(NOT DEFINED ARG_REPORTS)
            set(ARG_REPORTS 1)
        endif()
        add_test(NAME platform.crash.${mode}
            COMMAND "${CMAKE_COMMAND}"
                "-DHELPER=$<TARGET_FILE:crash_helper>" "-DMODE=${mode}"
                "-DDIR=${CMAKE_CURRENT_BINARY_DIR}/crash_tests/$<CONFIG>/${mode}"
                "-DREPORTS=${ARG_REPORTS}" "-DEXPECT=${expect}" "-DLOG_EXPECT=${ARG_LOG}"
                "-DDUMP_CHECK=${dump_check}" "-DDUMP_EXPECT=${dump_expect}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_crash.cmake")
        set_tests_properties(platform.crash.${mode} PROPERTIES
            LABELS "unit" TIMEOUT 180 SKIP_REGULAR_EXPRESSION "skipped:")
    endfunction()

    set(crash_access_windows
        "fault:     EXCEPTION_ACCESS_VIOLATION (0xC0000005), write at 0x0000000000000010")
    set(crash_access_linux
        "fault:     SIGSEGV (11), SEGV_MAPERR: no mapping there at 0x0000000000000010")
    nph_crash_test(access
        EXPECT "note 0:    crash helper" "note 5:    access" "crash-main"
            "log:       the last records delivered" "the last line before the access"
        ON_WINDOWS "${crash_access_windows}" "module:    crash_helper.exe+0x" "dump:      crash_helper-"
        ON_LINUX "${crash_access_linux}" "registers: rip 0x" "maps:"
        DUMP "exception 0xC0000005" "exception thread listed 1" "executable data present"
        LOG "the last line before the access")
    nph_crash_test(abort
        ON_WINDOWS "fault:     abort (0x40000015)"
        ON_LINUX "fault:     SIGABRT (6), raised"
        DUMP "exception 0x40000015" "exception thread listed 1"
        LOG "the last line before the abort")
    nph_crash_test(assert
        EXPECT "assertion: 1 == 2 at " "on purpose: 42"
        ON_WINDOWS "fault:     abort (0x40000015)"
        ON_LINUX "fault:     SIGABRT (6)")
    nph_crash_test(divide
        ON_WINDOWS "fault:     EXCEPTION_INT_DIVIDE_BY_ZERO (0xC0000094)"
        ON_LINUX "fault:     SIGFPE (8), FPE_INTDIV")
    nph_crash_test(illegal
        ON_WINDOWS "fault:     EXCEPTION_ILLEGAL_INSTRUCTION (0xC000001D)"
        ON_LINUX "fault:     SIGILL (4)")
    nph_crash_test(overflow
        ON_WINDOWS "fault:     EXCEPTION_STACK_OVERFLOW (0xC00000FD)"
        ON_LINUX "fault:     SIGSEGV (11)"
        DUMP "exception 0xC00000FD" "exception thread listed 1")
    nph_crash_test(thread
        EXPECT "crash-worker"
        ON_WINDOWS "${crash_access_windows}"
        ON_LINUX "${crash_access_linux}")
    nph_crash_test(twice
        EXPECT "crash-pair-"
        ON_WINDOWS "${crash_access_windows}"
        ON_LINUX "${crash_access_linux}")
    nph_crash_test(fiber
        ON_WINDOWS "${crash_access_windows}"
        ON_LINUX "${crash_access_linux}")
    # A fiber's own stack overflowing: on Windows the system grows it into the
    # pages below its guard page and raises EXCEPTION_STACK_OVERFLOW
    # (fiber.c); on Linux the guard page's fault is reported from the
    # thread's alternate signal stack.
    nph_crash_test(fiber_overflow
        ON_WINDOWS "fault:     EXCEPTION_STACK_OVERFLOW (0xC00000FD)"
        ON_LINUX "fault:     SIGSEGV (11)"
        DUMP "exception 0xC00000FD" "exception thread listed 1")
    nph_crash_test(uninstalled REPORTS 0)
    if(WIN32)
        nph_crash_test(invalid_parameter
            ON_WINDOWS "fault:     invalid parameter (0xC0000417)"
            DUMP "exception 0xC0000417")
        nph_crash_test(purecall
            ON_WINDOWS "fault:     pure virtual call (0x40000015)")
    endif()
endif()
