# SPDX-License-Identifier: Apache-2.0
# CPU baseline flags (CLAUDE.md §3): x86-64 with SSE4.2 (which implies POPCNT).
# AVX2 paths are compiled in dedicated translation units and selected at
# runtime, never enabled globally, so a baseline machine still runs the engine.

function(nph_apply_arch_flags target)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
        return()
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            # clang-cl forwards GCC-style flags through /clang:.
            target_compile_options(${target} INTERFACE /clang:-msse4.2 /clang:-mpopcnt)
        else()
            target_compile_options(${target} INTERFACE -msse4.2 -mpopcnt)
        endif()
    endif()
    # MSVC has no /arch level between SSE2 and AVX. The SSE4.2 and POPCNT
    # intrinsics are always available and emit the instructions directly, so
    # no flag is needed; auto-vectorisation simply stays at SSE2.
endfunction()
