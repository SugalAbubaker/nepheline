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

    # Floating-point policy (docs/adr/0008-math-conventions.md §6): IEEE-754
    # semantics, no fast-math, and no contraction of a*b+c into FMA. Results
    # are then identical across translation units compiled at different ISA
    # levels, which the SIMD kernels and the determinism tests rely on.
    # GCC already defaults to -ffp-contract=off in ISO C mode; Clang does not.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            target_compile_options(${target} INTERFACE /clang:-ffp-contract=off)
        else()
            target_compile_options(${target} INTERFACE -ffp-contract=off)
        endif()
    elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        # The default, stated explicitly: /fp:precise does not contract.
        target_compile_options(${target} INTERFACE /fp:precise)
    endif()
endfunction()

# nph_set_avx2_sources(<src>...)
#
# Compiles the given sources of the current directory with AVX2, FMA, and
# F16C enabled. Used for the second copy of the math batch kernels
# (engine/math/src/kernels_avx2.c), which is selected at run time after
# CPUID (docs/adr/0008-math-conventions.md 4, 9). Contraction stays off, so
# the flags widen the registers without changing any rounding.
function(nph_set_avx2_sources)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
        return()
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-mf16c")
    endif()
endfunction()
