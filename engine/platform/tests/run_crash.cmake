# SPDX-License-Identifier: Apache-2.0
# Runs crash_helper in one mode and checks what the crash handler left
# (engine/platform/tests/crash_helper.c, docs/adr/0103-crash-handler.md):
#
#   cmake -DHELPER=<exe> -DMODE=<mode> -DDIR=<dir> [-DREPORTS=<n>]
#         [-DEXPECT=<substrings>] [-DLOG_EXPECT=<substring>]
#         [-DDUMP_CHECK=<exe> -DDUMP_EXPECT=<substrings>]
#         [-DAFTER_MODE=<mode> -DAFTER_EXPECT=<substring>] -P run_crash.cmake
#
# It passes when the helper ended by its failure (not 0, nor the 90 to 93
# it returns when it could not fail as asked), DIR holds REPORTS reports (1 by
# default) that each contain every EXPECT, the helper's log holds
# LOG_EXPECT (delivered after the report), and, with DUMP_CHECK, the
# minidump beside each report reads back with every DUMP_EXPECT. With
# AFTER_MODE, the helper then runs again in that mode on the same directory
# and must exit 0 saying AFTER_EXPECT (what the failed process left, seen
# from a live one). Where the helper prints "skipped:" (the sanitizers'
# builds) it prints that too, for ctest's SKIP_REGULAR_EXPRESSION.

if(NOT DEFINED HELPER OR NOT DEFINED MODE OR NOT DEFINED DIR)
    message(FATAL_ERROR "run_crash.cmake: HELPER, MODE, and DIR are required")
endif()
if(NOT DEFINED REPORTS)
    set(REPORTS 1)
endif()

file(REMOVE_RECURSE "${DIR}")
file(MAKE_DIRECTORY "${DIR}")
execute_process(
    COMMAND "${HELPER}" "${MODE}" "${DIR}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
set(all "${out}${err}")

if(all MATCHES "skipped:")
    message(STATUS "skipped: ${MODE}: the crash handler does not install in this build")
    return()
endif()
if("${rc}" STREQUAL "0" OR "${rc}" MATCHES "^9[0-3]$")
    message(FATAL_ERROR "${MODE}: the helper exited with '${rc}', not by its failure\n"
                        "--- output ---\n${all}")
endif()

file(GLOB reports "${DIR}/*.txt")
list(LENGTH reports count)
# What each report says of its machine and of its dump, kept beside the mode's directory after it
# is cleared (<mode>.lines.txt): CI's Windows jobs print them, so the runners whose dbghelp fails
# every dump of a process can be told from the others (ADR-0103).
get_filename_component(lines_dir "${DIR}" DIRECTORY)
set(summary "${MODE} (exit '${rc}', ${count} report(s)):")
foreach(report IN LISTS reports)
    file(READ "${report}" text)
    foreach(key IN ITEMS "system:" "memory:" "dump:")
        string(FIND "${text}" "\n${key}" at)
        if(NOT at EQUAL -1)
            math(EXPR at "${at} + 1")
            string(SUBSTRING "${text}" ${at} -1 rest)
            string(FIND "${rest}" "\n" end)
            string(SUBSTRING "${rest}" 0 ${end} line)
            string(APPEND summary " ${line}")
        endif()
    endforeach()
endforeach()
if(count EQUAL 0)
    # No report: the helper's own words, whose install line names the machine.
    string(REGEX MATCH "system: [^\n]*; reports go to" said "${all}")
    string(APPEND summary " (from its log) ${said}")
endif()
file(WRITE "${lines_dir}/${MODE}.lines.txt" "${summary}\n")
if(NOT count EQUAL REPORTS)
    message(FATAL_ERROR "${MODE}: ${count} reports where ${REPORTS} were expected: ${reports}\n"
                        "--- output (exit '${rc}') ---\n${all}")
endif()

foreach(report IN LISTS reports)
    file(READ "${report}" text)
    foreach(needle IN LISTS EXPECT)
        string(FIND "${text}" "${needle}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR "${MODE}: the report lacks '${needle}'\n--- ${report} ---\n${text}")
        endif()
    endforeach()
    if(DEFINED DUMP_CHECK AND NOT DUMP_CHECK STREQUAL "")
        string(REGEX REPLACE "\\.txt$" ".dmp" dump "${report}")
        if(NOT EXISTS "${dump}")
            message(FATAL_ERROR "${MODE}: no minidump beside the report\n--- ${report} ---\n${text}")
        endif()
        # The report says whether MiniDumpWriteDump wrote it ("dump: <name>")
        # or why not ("not written (error <n>)").
        get_filename_component(dump_name "${dump}" NAME)
        string(FIND "${text}" "dump:      ${dump_name}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR "${MODE}: the report does not name its minidump (exit '${rc}')\n"
                                "--- ${report} ---\n${text}")
        endif()
        execute_process(
            COMMAND "${DUMP_CHECK}" "${dump}"
            RESULT_VARIABLE dump_rc
            OUTPUT_VARIABLE dump_out
            ERROR_VARIABLE dump_err)
        if(NOT dump_rc EQUAL 0)
            file(SIZE "${dump}" dump_size)
            message(FATAL_ERROR "${MODE}: the minidump does not read: '${dump_rc}' (${dump_size} "
                                "bytes, the helper's exit '${rc}')\n${dump_out}${dump_err}"
                                "--- ${report} ---\n${text}")
        endif()
        foreach(needle IN LISTS DUMP_EXPECT)
            string(FIND "${dump_out}" "${needle}" pos)
            if(pos EQUAL -1)
                message(FATAL_ERROR "${MODE}: the minidump says\n${dump_out}\nwithout '${needle}'")
            endif()
        endforeach()
        # The dump's exception context is the one the report names: the handler copies it for
        # dbghelp (ADR-0106's amendment), and its instruction is the report's "at:".
        string(REGEX MATCH "at:        0x([0-9A-F]+)" at_match "${text}")
        set(report_pc "${CMAKE_MATCH_1}")
        string(REGEX MATCH "context rip 0x([0-9A-F]+)" rip_match "${dump_out}")
        set(dump_pc "${CMAKE_MATCH_1}")
        if(report_pc STREQUAL "" OR NOT report_pc STREQUAL dump_pc)
            message(FATAL_ERROR "${MODE}: the minidump's exception context is at 0x${dump_pc}, the "
                                "report's at 0x${report_pc}\n${dump_out}--- ${report} ---\n${text}")
        endif()
    endif()
endforeach()

if(REPORTS GREATER 0)
    # The line the handler writes to standard error names the report.
    string(FIND "${all}" "crash report" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "${MODE}: nothing on standard error names the report\n--- output ---\n${all}")
    endif()
endif()

if(DEFINED LOG_EXPECT AND NOT LOG_EXPECT STREQUAL "")
    if(NOT EXISTS "${DIR}/helper.log")
        message(FATAL_ERROR "${MODE}: the helper's log was not written")
    endif()
    file(READ "${DIR}/helper.log" log)
    string(FIND "${log}" "${LOG_EXPECT}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "${MODE}: the log lacks '${LOG_EXPECT}' (not delivered after the report)\n"
                            "--- helper.log ---\n${log}")
    endif()
endif()

if(DEFINED AFTER_MODE AND NOT AFTER_MODE STREQUAL "")
    execute_process(
        COMMAND "${HELPER}" "${AFTER_MODE}" "${DIR}"
        RESULT_VARIABLE after_rc
        OUTPUT_VARIABLE after_out
        ERROR_VARIABLE after_err)
    if(NOT after_rc EQUAL 0)
        message(FATAL_ERROR "${MODE}: then ${AFTER_MODE} exited with '${after_rc}'\n${after_out}${after_err}")
    endif()
    string(FIND "${after_out}" "${AFTER_EXPECT}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "${MODE}: then ${AFTER_MODE} did not say '${AFTER_EXPECT}'\n${after_out}${after_err}")
    endif()
endif()

file(REMOVE_RECURSE "${DIR}")
message(STATUS "ok: ${MODE}, exit '${rc}', ${count} report(s)")
