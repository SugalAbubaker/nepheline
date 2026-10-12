# SPDX-License-Identifier: Apache-2.0
# Warning policy (CLAUDE.md §5): everything on, warnings are errors.
#   MSVC       /W4 /WX /permissive-
#   GCC/Clang  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
# plus a small set of additional checks that catch real C bugs.

function(nph_apply_warnings target)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} INTERFACE
            /W4
            /permissive-
            /utf-8
            /Zc:preprocessor      # standards-conforming preprocessor (correct __VA_ARGS__)
            /diagnostics:caret
            # CRT and SDK headers come from the INCLUDE environment (vcvars); our own
            # headers arrive via /I and stay fully checked.
            /external:env:INCLUDE /external:W0
            /w44062               # enum value not handled in switch without default
            /w44242 /w44254       # implicit narrowing conversions (close to -Wconversion)
            # C4324 "structure was padded due to alignment specifier" fires for
            # every cache-line-aligned field; that padding is the point.
            /wd4324
            # C4702 "unreachable code" is raised during link-time code generation
            # after whole-program constant propagation (e.g. a `return` following a
            # conditional longjmp whose condition is constant at every call site).
            # It reports optimizer facts, not source bugs, so it is off only where
            # LTO runs. The other configurations keep it. C4723 "potential divide
            # by 0" is the same phenomenon: a call site with a constant argument
            # (a ray perpendicular to a plane) is inlined and the divisor becomes
            # a known zero on a path the preceding guard already returns from.
            $<$<CONFIG:Shipping>:/wd4702>
            $<$<CONFIG:Shipping>:/wd4723>
        )
        if(NPH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} INTERFACE /WX)
        endif()
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            # clang-cl maps -Wall to -Weverything; /W4 is its -Wall -Wextra.
            target_compile_options(${target} INTERFACE /W4)
        else()
            target_compile_options(${target} INTERFACE -Wall -Wextra)
        endif()
        target_compile_options(${target} INTERFACE
            -Wpedantic
            -Wconversion -Wshadow
            -Wdouble-promotion    # accidental float->double promotion is a perf bug
            -Wstrict-prototypes -Wmissing-prototypes
            -Wundef               # #if on an undefined macro
            -Wvla
            -Wcast-qual
            -Wformat=2
            -Wimplicit-fallthrough
        )
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} INTERFACE -Wlogical-op -Wduplicated-cond)
        endif()
        if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            target_compile_options(${target} INTERFACE /utf-8)
        endif()
        if(NPH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} INTERFACE -Werror)
        endif()
    else()
        message(WARNING "Unknown C compiler '${CMAKE_C_COMPILER_ID}'; no warning flags applied.")
    endif()
endfunction()
