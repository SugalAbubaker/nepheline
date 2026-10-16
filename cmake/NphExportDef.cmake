# SPDX-License-Identifier: Apache-2.0
# Writes the module-definition file of nepheline_shared from the engine's
# static libraries (script mode; cmake/NphGameModule.cmake runs it):
#
#   cmake -DDUMPBIN=<dumpbin.exe> -DOUTPUT=<file.def> "-DLIBRARIES=<a.lib|b.lib>"
#         -P cmake/NphExportDef.cmake
#
# CMake's WINDOWS_EXPORT_ALL_SYMBOLS lists exports from the object files'
# symbol tables, which MSVC's link-time code generation objects (/GL, the
# Shipping configuration) do not have in COFF form. A library's archive
# index lists its public symbols either way, so the exports come from
# there: the engine's API (nph_*), the component descriptor functions
# (Nph*_component), and the engine's log categories (NPH_LOG_*, exported
# as DATA: importers declare them NPH_ENGINE_DATA). Nothing else is
# exported: not the C runtime's inline functions an object may carry, nor
# a module's internal helpers.

foreach(var DUMPBIN OUTPUT LIBRARIES)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "NphExportDef.cmake: ${var} is required")
    endif()
endforeach()

string(REPLACE "|" ";" LIBRARIES "${LIBRARIES}")
set(exports "")
foreach(lib IN LISTS LIBRARIES)
    execute_process(
        COMMAND "${DUMPBIN}" /NOLOGO /LINKERMEMBER:1 "${lib}"
        OUTPUT_VARIABLE listing
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "NphExportDef.cmake: ${DUMPBIN} failed on ${lib} (${status})")
    endif()
    string(REGEX MATCHALL "\n +[0-9A-F]+ +(nph_[A-Za-z0-9_]+|Nph[A-Za-z0-9]+_component|NPH_LOG_[A-Z0-9_]+)"
           found "${listing}")
    foreach(line IN LISTS found)
        string(REGEX REPLACE "^\n +[0-9A-F]+ +" "" symbol "${line}")
        list(APPEND exports "${symbol}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES exports)
list(SORT exports)
list(LENGTH exports count)
if(count EQUAL 0)
    message(FATAL_ERROR "NphExportDef.cmake: no engine symbols found in ${LIBRARIES}")
endif()

set(text "EXPORTS\n")
foreach(symbol IN LISTS exports)
    if(symbol MATCHES "^NPH_LOG_")
        string(APPEND text "    ${symbol} DATA\n")
    else()
        string(APPEND text "    ${symbol}\n")
    endif()
endforeach()
# Rewritten only when it changes, so an unchanged export list relinks
# nothing.
file(WRITE "${OUTPUT}.new" "${text}")
file(COPY_FILE "${OUTPUT}.new" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.new")
