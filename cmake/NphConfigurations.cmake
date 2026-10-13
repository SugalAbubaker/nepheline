# SPDX-License-Identifier: Apache-2.0
# The four Nepheline build configurations (CLAUDE.md §6):
#   Debug        no optimization, all asserts, validation layers, sanitizers
#   Development  optimized, asserts, validation, profiler
#   Profile      optimized, profiler, no asserts
#   Shipping     LTO, no asserts, no validation, profiler compiled out
#
# nph_declare_configurations() must run before project(); the flag functions
# run after it, once the compiler is known.

set(NPH_CONFIGURATIONS Debug Development Profile Shipping)

macro(nph_declare_configurations)
    get_property(_nph_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_nph_multi OR CMAKE_GENERATOR MATCHES "Multi-Config|Visual Studio|Xcode")
        set(CMAKE_CONFIGURATION_TYPES "${NPH_CONFIGURATIONS}" CACHE STRING
            "Nepheline build configurations" FORCE)
    else()
        if(NOT CMAKE_BUILD_TYPE)
            set(CMAKE_BUILD_TYPE Development CACHE STRING "Build configuration" FORCE)
        endif()
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${NPH_CONFIGURATIONS})
        if(NOT CMAKE_BUILD_TYPE IN_LIST NPH_CONFIGURATIONS)
            message(FATAL_ERROR "CMAKE_BUILD_TYPE must be one of: ${NPH_CONFIGURATIONS} "
                                "(got '${CMAKE_BUILD_TYPE}')")
        endif()
    endif()
    unset(_nph_multi)
endmacro()

# Sets the per-configuration compiler and linker flags for the whole tree.
# These are plain variables (not cache) so they take precedence over whatever
# CMake's compiler module seeded for Debug.
macro(nph_apply_configuration_flags)
    if(MSVC)
        # /Zi is added by CMAKE_MSVC_DEBUG_INFORMATION_FORMAT (policy CMP0141).
        set(CMAKE_C_FLAGS_DEBUG       "/Od /Ob0")
        set(CMAKE_C_FLAGS_DEVELOPMENT "/O2 /Ob2 /Oi /Gy /GF")
        set(CMAKE_C_FLAGS_PROFILE     "/O2 /Ob2 /Oi /Gy /GF /DNDEBUG")
        set(CMAKE_C_FLAGS_SHIPPING    "/O2 /Ob2 /Oi /Gy /GF /DNDEBUG")
        # /RTC1 run-time checks are incompatible with /fsanitize=address, and
        # clang-cl does not implement them at all.
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND NOT NPH_ASAN)
            string(APPEND CMAKE_C_FLAGS_DEBUG " /RTC1")
        endif()
        # Incremental linking is incompatible with ASan and buys nothing with Ninja.
        set(CMAKE_EXE_LINKER_FLAGS_DEBUG       "/DEBUG /INCREMENTAL:NO")
        set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF")
        set(CMAKE_EXE_LINKER_FLAGS_PROFILE     "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF")
        set(CMAKE_EXE_LINKER_FLAGS_SHIPPING    "/INCREMENTAL:NO /OPT:REF /OPT:ICF")
        foreach(cfg ${NPH_CONFIGURATIONS})
            string(TOUPPER "${cfg}" CFG)
            set(CMAKE_SHARED_LINKER_FLAGS_${CFG} "${CMAKE_EXE_LINKER_FLAGS_${CFG}}")
            set(CMAKE_MODULE_LINKER_FLAGS_${CFG} "${CMAKE_EXE_LINKER_FLAGS_${CFG}}")
            set(CMAKE_STATIC_LINKER_FLAGS_${CFG} "")
        endforeach()
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
            "$<$<CONFIG:Debug,Development,Profile>:ProgramDatabase>")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        # The one C++ translation unit in the runtime (the VMA implementation,
        # engine/gpu/src/gpu_vma.cpp) follows the same optimisation levels.
        set(CMAKE_CXX_FLAGS_DEBUG       "/Od /Ob0 /EHsc")
        set(CMAKE_CXX_FLAGS_DEVELOPMENT "/O2 /Ob2 /Oi /Gy /GF /EHsc")
        set(CMAKE_CXX_FLAGS_PROFILE     "/O2 /Ob2 /Oi /Gy /GF /EHsc /DNDEBUG")
        set(CMAKE_CXX_FLAGS_SHIPPING    "/O2 /Ob2 /Oi /Gy /GF /EHsc /DNDEBUG")
    else()
        set(CMAKE_C_FLAGS_DEBUG       "-O0 -g3 -fno-omit-frame-pointer")
        set(CMAKE_C_FLAGS_DEVELOPMENT "-O2 -g -fno-omit-frame-pointer")
        set(CMAKE_C_FLAGS_PROFILE     "-O2 -g -fno-omit-frame-pointer -DNDEBUG")
        set(CMAKE_C_FLAGS_SHIPPING    "-O3 -DNDEBUG")
        set(CMAKE_CXX_FLAGS_DEBUG       "-O0 -g3 -fno-omit-frame-pointer")
        set(CMAKE_CXX_FLAGS_DEVELOPMENT "-O2 -g -fno-omit-frame-pointer")
        set(CMAKE_CXX_FLAGS_PROFILE     "-O2 -g -fno-omit-frame-pointer -DNDEBUG")
        set(CMAKE_CXX_FLAGS_SHIPPING    "-O3 -DNDEBUG")
        foreach(cfg ${NPH_CONFIGURATIONS})
            string(TOUPPER "${cfg}" CFG)
            set(CMAKE_EXE_LINKER_FLAGS_${CFG} "")
            set(CMAKE_SHARED_LINKER_FLAGS_${CFG} "")
            set(CMAKE_MODULE_LINKER_FLAGS_${CFG} "")
            set(CMAKE_STATIC_LINKER_FLAGS_${CFG} "")
        endforeach()
    endif()

    # Shipping uses link-time optimization (/GL+/LTCG on MSVC, -flto on GCC/Clang).
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _nph_ipo OUTPUT _nph_ipo_msg LANGUAGES C)
    if(_nph_ipo)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_SHIPPING ON)
    else()
        message(WARNING "LTO is not supported by this toolchain; Shipping builds without it:\n${_nph_ipo_msg}")
    endif()
    unset(_nph_ipo)
    unset(_nph_ipo_msg)

    # Imported third-party targets (later phases) only know Debug/Release.
    set(CMAKE_MAP_IMPORTED_CONFIG_DEVELOPMENT Release RelWithDebInfo "")
    set(CMAKE_MAP_IMPORTED_CONFIG_PROFILE     Release RelWithDebInfo "")
    set(CMAKE_MAP_IMPORTED_CONFIG_SHIPPING    Release MinSizeRel "")
endmacro()

# Per-configuration preprocessor definitions seen by all Nepheline code.
function(nph_apply_configuration_definitions target)
    target_compile_definitions(${target} INTERFACE
        $<$<CONFIG:Debug>:NPH_CONFIG_DEBUG=1>
        $<$<CONFIG:Development>:NPH_CONFIG_DEVELOPMENT=1>
        $<$<CONFIG:Profile>:NPH_CONFIG_PROFILE=1>
        $<$<CONFIG:Shipping>:NPH_CONFIG_SHIPPING=1>
        $<$<CONFIG:Debug,Development>:NPH_ASSERTS_ENABLED=1>
        $<$<CONFIG:Debug,Development>:NPH_GPU_VALIDATION_ENABLED=1>
        $<$<CONFIG:Debug,Development,Profile>:NPH_PROFILER_ENABLED=1>
    )
endfunction()
