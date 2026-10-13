# SPDX-License-Identifier: Apache-2.0
# GLSL -> SPIR-V at build time with glslc from the Vulkan SDK (CLAUDE.md §3,
# §6 "Shaders are compiled through CMake custom commands. Dependency tracking
# covers #includes"). glslc writes a Make-style depfile (-MD -MF) that Ninja
# reads, so editing a header in shaders/shared/ rebuilds every shader that
# includes it.
#
# Every shader is compiled once into ${NPH_SHADER_OUTPUT_DIR} (the binary is
# independent of the build configuration) and loaded at run time from there;
# executables learn the directory through the NPH_SHADER_DIR definition that
# nph_target_use_shaders adds. The editor's hot reload (Phase 9) runs glslc
# on the same sources with the same flags.

find_program(NPH_GLSLC
    NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "glslc, the GLSL to SPIR-V compiler from the Vulkan SDK (or the glslc package)")
if(NOT NPH_GLSLC)
    message(FATAL_ERROR
        "glslc was not found. Install the Vulkan SDK (https://vulkan.lunarg.com/) and set "
        "VULKAN_SDK, or on Debian/Ubuntu: apt install glslc. See docs/build.md.")
endif()

set(NPH_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${NPH_SHADER_OUTPUT_DIR}")

# The flags every shader is compiled with. Vulkan 1.3 (SPIR-V 1.6), optimised,
# with debug names kept so validation messages and GPU debuggers can name
# variables, and warnings as errors like the C code.
set(NPH_GLSLC_FLAGS
    --target-env=vulkan1.3
    -O
    -g
    -Werror
    "-I${CMAKE_SOURCE_DIR}/shaders/shared")

# nph_add_shaders(<target> SOURCES <file.vert|.frag|.comp|...>...)
#
# Compiles each source to ${NPH_SHADER_OUTPUT_DIR}/<basename>.spv and creates
# a custom target <target> that depends on all of them. The stage comes from
# the extension, as glslc expects (.vert .frag .comp .geom .tesc .tese .mesh
# .task .rgen ...).
function(nph_add_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "nph_add_shaders(${target}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "nph_add_shaders(${target}): SOURCES is required")
    endif()
    set(outputs)
    foreach(src IN LISTS ARG_SOURCES)
        get_filename_component(abs "${src}" ABSOLUTE)
        get_filename_component(name "${src}" NAME)
        set(spv "${NPH_SHADER_OUTPUT_DIR}/${name}.spv")
        set(dep "${NPH_SHADER_OUTPUT_DIR}/${name}.d")
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${NPH_GLSLC}" ${NPH_GLSLC_FLAGS} -MD -MF "${dep}" -o "${spv}" "${abs}"
            DEPENDS "${abs}"
            DEPFILE "${dep}"
            COMMENT "glslc ${name}"
            VERBATIM)
        list(APPEND outputs "${spv}")
    endforeach()
    add_custom_target(${target} DEPENDS ${outputs} SOURCES ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES FOLDER "shaders")
endfunction()

# nph_target_use_shaders(<target> <shader_target>...)
#
# Makes <target> depend on the shader targets and defines NPH_SHADER_DIR for
# it: the directory the .spv files are loaded from at run time.
function(nph_target_use_shaders target)
    add_dependencies(${target} ${ARGN})
    target_compile_definitions(${target} PRIVATE "NPH_SHADER_DIR=\"${NPH_SHADER_OUTPUT_DIR}\"")
    # The C/GLSL shared headers (nph_shared.h).
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/shaders/shared")
endfunction()
