# SPDX-License-Identifier: Apache-2.0
# Reads NPH_VERSION_{MAJOR,MINOR,PATCH} from version.h so the header stays the
# single source of truth for the project version.

function(nph_read_version_header header out_var)
    if(NOT EXISTS "${header}")
        message(FATAL_ERROR "Version header not found: ${header}")
    endif()
    foreach(part MAJOR MINOR PATCH)
        file(STRINGS "${header}" line REGEX "^#define NPH_VERSION_${part} +[0-9]+")
        if(NOT line MATCHES "#define NPH_VERSION_${part} +([0-9]+)")
            message(FATAL_ERROR "NPH_VERSION_${part} not found in ${header}")
        endif()
        set(v_${part} "${CMAKE_MATCH_1}")
    endforeach()
    set(${out_var} "${v_MAJOR}.${v_MINOR}.${v_PATCH}" PARENT_SCOPE)
endfunction()
