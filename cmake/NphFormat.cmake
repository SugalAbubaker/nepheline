# SPDX-License-Identifier: Apache-2.0
# Optional convenience targets around scripts/format.py:
#   nph-format        rewrite every tracked .c/.h file with clang-format
#   nph-format-check  fail if any tracked file is not formatted (what CI runs)

function(nph_add_format_targets)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "Python3 not found; nph-format targets unavailable.")
        return()
    endif()
    set(script "${CMAKE_SOURCE_DIR}/scripts/format.py")
    add_custom_target(nph-format
        COMMAND "${Python3_EXECUTABLE}" "${script}" --fix
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "clang-format: rewriting tracked sources"
        VERBATIM)
    add_custom_target(nph-format-check
        COMMAND "${Python3_EXECUTABLE}" "${script}" --check
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "clang-format: checking tracked sources"
        VERBATIM)
    set_target_properties(nph-format nph-format-check PROPERTIES FOLDER "tools")
endfunction()
