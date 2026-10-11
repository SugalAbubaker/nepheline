# SPDX-License-Identifier: Apache-2.0
# Runs a test executable and checks its exit code, output, and JUnit report.
#
#   cmake -DEXE=<path> [-DARGS=<list>] -DEXPECT_EXIT=<n>
#         [-DEXPECT=<substring list>] [-DEXPECT_NOT=<substring list>]
#         [-DJUNIT=<file> -DEXPECT_JUNIT=<substring list>] -P run_check.cmake

if(NOT DEFINED EXE OR NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "run_check.cmake: EXE and EXPECT_EXIT are required")
endif()

if(DEFINED JUNIT AND NOT JUNIT STREQUAL "")
    file(REMOVE "${JUNIT}")
    list(APPEND ARGS "--junit=${JUNIT}")
endif()

execute_process(
    COMMAND "${EXE}" ${ARGS}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
set(all "${out}${err}")

if(NOT rc EQUAL EXPECT_EXIT)
    message(FATAL_ERROR "exit code was '${rc}', expected ${EXPECT_EXIT}\n--- output ---\n${all}")
endif()

foreach(needle IN LISTS EXPECT)
    string(FIND "${all}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "output lacks '${needle}'\n--- output ---\n${all}")
    endif()
endforeach()

foreach(needle IN LISTS EXPECT_NOT)
    string(FIND "${all}" "${needle}" pos)
    if(NOT pos EQUAL -1)
        message(FATAL_ERROR "output must not contain '${needle}'\n--- output ---\n${all}")
    endif()
endforeach()

if(DEFINED JUNIT AND NOT JUNIT STREQUAL "")
    if(NOT EXISTS "${JUNIT}")
        message(FATAL_ERROR "JUnit report was not written: ${JUNIT}\n--- output ---\n${all}")
    endif()
    file(READ "${JUNIT}" xml)
    foreach(needle IN LISTS EXPECT_JUNIT)
        string(FIND "${xml}" "${needle}" pos)
        if(pos EQUAL -1)
            message(FATAL_ERROR "JUnit report lacks '${needle}'\n--- report ---\n${xml}")
        endif()
    endforeach()
endif()

message(STATUS "ok: exit ${rc}, ${ARGS}")
