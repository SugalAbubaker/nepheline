# SPDX-License-Identifier: Apache-2.0
# The engine as one shared library, and game modules built against it
# (CLAUDE.md §8.12 "Game code is built as a shared library loaded by the
# runtime and hot-reloaded in development"; docs/adr/0024-game-modules.md).
#
# A game module calls engine functions, and the host that loads it (the
# player, the editor, the game module tests) must run the same engine: one
# log, one profiler, one window system, one job system. So both link
# nepheline_shared, which is built from the engine modules' object files
# (the static libraries every other target links are untouched). On
# Windows the engine's API is exported through a module-definition file
# built from the libraries (MSVC) or WINDOWS_EXPORT_ALL_SYMBOLS (clang-cl);
# data needs NPH_ENGINE_DATA on the importing side (core/base.h), which
# NPH_ENGINE_SHARED_IMPORT, defined for every target that links it, turns
# on. Third-party code linked into the library stays private to it.

set(NPH_ENGINE_MODULES core math platform jobs gpu render asset scene physics audio profile
                       runtime)

# nph_add_engine_shared_library()
#
# Defines nepheline_shared (alias nph::shared). Called once, after every
# engine module.
function(nph_add_engine_shared_library)
    set(objects)
    foreach(m IN LISTS NPH_ENGINE_MODULES)
        list(APPEND objects $<TARGET_OBJECTS:nph_${m}>)
    endforeach()
    add_library(nepheline_shared SHARED ${objects})
    add_library(nph::shared ALIAS nepheline_shared)
    set_target_properties(nepheline_shared PROPERTIES
        # VMA's translation unit is C++: link with the C++ runtime.
        LINKER_LANGUAGE CXX
        FOLDER "engine")
    if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # MSVC: the exports come from the static libraries' archive indexes
        # (cmake/NphExportDef.cmake), since the Shipping configuration's
        # /GL objects have no symbol table WINDOWS_EXPORT_ALL_SYMBOLS can
        # read. A .def source is passed to the linker as /DEF.
        get_filename_component(linker_dir "${CMAKE_LINKER}" DIRECTORY)
        find_program(NPH_DUMPBIN dumpbin HINTS "${linker_dir}" REQUIRED)
        set(libraries)
        set(library_targets)
        foreach(m IN LISTS NPH_ENGINE_MODULES)
            list(APPEND libraries "$<TARGET_FILE:nph_${m}>")
            list(APPEND library_targets nph_${m})
        endforeach()
        # One argument: the paths joined with '|' (a ';' list would be split).
        list(JOIN libraries "|" library_paths)
        set(def "${CMAKE_CURRENT_BINARY_DIR}/nepheline_shared_$<CONFIG>.def")
        add_custom_command(
            OUTPUT "${def}"
            COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${NPH_DUMPBIN}" "-DOUTPUT=${def}"
                    "-DLIBRARIES=${library_paths}" -P "${CMAKE_SOURCE_DIR}/cmake/NphExportDef.cmake"
            DEPENDS ${library_targets} "${CMAKE_SOURCE_DIR}/cmake/NphExportDef.cmake"
            COMMENT "Listing the engine's exports"
            VERBATIM)
        target_sources(nepheline_shared PRIVATE "${def}")
    else()
        # clang-cl (COFF objects in every configuration it builds); elsewhere
        # the property does nothing and default visibility exports.
        set_target_properties(nepheline_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
    foreach(m IN LISTS NPH_ENGINE_MODULES)
        target_include_directories(nepheline_shared PUBLIC "${CMAKE_SOURCE_DIR}/engine/${m}/include")
    endforeach()
    target_link_libraries(nepheline_shared
        PUBLIC nph::options
        PRIVATE glfw volk nph_gpu_vma)
    if(TARGET nph_platform_fiber_asm)
        target_link_libraries(nepheline_shared PRIVATE nph_platform_fiber_asm)
    endif()
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(nepheline_shared PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
        # Keep the vendored libraries' symbols out of the library's exports.
        target_link_options(nepheline_shared PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    target_compile_definitions(nepheline_shared INTERFACE NPH_ENGINE_SHARED_IMPORT=1)
    # The runtime finds the renderer's shaders where this build compiles them.
    add_dependencies(nepheline_shared nph_shaders_render)
endfunction()

# nph_add_game_module(<name> SOURCES <src>... [DEPS <target>...])
#
# Builds the game module <name><ext> (no "lib" prefix) against the shared
# engine. Only the entry point (NPH_GAME_EXPORT) is exported.
function(nph_add_game_module name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;DEPS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "nph_add_game_module(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    add_library(${name} MODULE ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE nph::shared ${ARG_DEPS})
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        C_VISIBILITY_PRESET hidden
        FOLDER "games")
    if(MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()
