# SPDX-License-Identifier: Apache-2.0
# Sanitizer instrumentation for the Debug configuration (CLAUDE.md §6, §9).
# Applied through nph::options so every Nepheline object file and link step
# in a Debug build is instrumented consistently.

# MSVC's ASan runtime is a DLL next to cl.exe. Test executables launched by
# CTest outside a Visual Studio developer prompt cannot find it, so copy it next
# to the Debug binaries at configure time.
function(_nph_msvc_copy_asan_runtime)
    get_filename_component(bindir "${CMAKE_C_COMPILER}" DIRECTORY)
    file(GLOB dlls "${bindir}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT dlls)
        message(WARNING "NPH_ASAN is on but clang_rt.asan_dynamic-x86_64.dll was not found "
                        "next to ${CMAKE_C_COMPILER}; Debug executables may fail to start.")
        return()
    endif()
    get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi)
        set(dest "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(dest "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    else()
        return()
    endif()
    file(COPY ${dlls} DESTINATION "${dest}")
endfunction()

function(nph_apply_sanitizers target)
    set(dbg "$<CONFIG:Debug>")

    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        if(NPH_ASAN)
            # The compiler emits /INFERASANLIBS into the objects; no link flag needed.
            target_compile_options(${target} INTERFACE "$<${dbg}:/fsanitize=address>")
            _nph_msvc_copy_asan_runtime()
        endif()
        if(NPH_UBSAN OR NPH_TSAN)
            message(STATUS "UBSan/TSan are not available on MSVC; ignoring.")
        endif()
    elseif(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl: unverified, off by default (NphOptions.cmake).
        if(NPH_ASAN)
            message(WARNING "ASan with clang-cl is not yet verified in CI; enabling best-effort.")
            target_compile_options(${target} INTERFACE "$<${dbg}:-fsanitize=address>")
            target_link_options(${target} INTERFACE "$<${dbg}:-fsanitize=address>")
        endif()
        if(NPH_UBSAN OR NPH_TSAN)
            message(STATUS "UBSan/TSan are not supported with clang-cl; ignoring.")
        endif()
    else()
        set(san "")
        if(NPH_ASAN)
            list(APPEND san address)
        endif()
        if(NPH_UBSAN)
            list(APPEND san undefined)
        endif()
        if(NPH_TSAN)
            list(APPEND san thread)
        endif()
        if(san)
            list(JOIN san "," san)
            target_compile_options(${target} INTERFACE
                "$<${dbg}:-fsanitize=${san}>"
                "$<${dbg}:-fno-sanitize-recover=all>"   # UB is a test failure, not a log line
                "$<${dbg}:-fno-omit-frame-pointer>")
            target_link_options(${target} INTERFACE "$<${dbg}:-fsanitize=${san}>")
        endif()
    endif()
endfunction()
