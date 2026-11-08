# SPDX-License-Identifier: Apache-2.0
# nbench.cli.*: black-box tests of nbench's budgets. Each runs nbench_selfcases
# (benchmarks of a known relative cost) against a budget file of its own and
# checks the exit code and output through ntest's run_check.cmake.

add_executable(nbench_selfcases nbench_selfcases.c)
target_link_libraries(nbench_selfcases PRIVATE nph::options nbench::main)
set_target_properties(nbench_selfcases PROPERTIES FOLDER "tests")

# nbench_cli_test(<name> EXIT <code> [BUDGETS <file>] [ARGS ...]
#                 [EXPECT <substring>...] [EXPECT_NOT <substring>...])
function(nbench_cli_test name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "EXIT;BUDGETS" "ARGS;EXPECT;EXPECT_NOT")
    set(args --min-time=0.005 --repeats=3 ${ARG_ARGS})
    if(ARG_BUDGETS)
        list(APPEND args "--budgets=${CMAKE_CURRENT_SOURCE_DIR}/budgets/${ARG_BUDGETS}")
    endif()
    add_test(NAME nbench.cli.${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:nbench_selfcases>" "-DARGS=${args}"
            "-DEXPECT_EXIT=${ARG_EXIT}" "-DEXPECT=${ARG_EXPECT}" "-DEXPECT_NOT=${ARG_EXPECT_NOT}"
            -P "${PROJECT_SOURCE_DIR}/tools/ntest/tests/run_check.cmake")
    set_tests_properties(nbench.cli.${name} PROPERTIES LABELS "unit" TIMEOUT 120)
endfunction()

# Absolute budgets, as before.
nbench_cli_test(absolute_within EXIT 0 BUDGETS absolute_within.txt ARGS --filter=self_*
    EXPECT "budgets: all 2 benchmarks within budget"
    EXPECT_NOT "BUDGET EXCEEDED")
nbench_cli_test(absolute_exceeded EXIT 4 BUDGETS absolute_exceeded.txt ARGS --filter=self_*
    EXPECT "BUDGET EXCEEDED  self_work_1" "budgets: FAILED"
    EXPECT_NOT "BUDGET EXCEEDED  self_work_8")

# Relative budgets: self_work_8 measures about 8x self_work_1.
nbench_cli_test(relative_within EXIT 0 BUDGETS relative_within.txt ARGS --filter=self_*
    EXPECT "budgets: all 2 benchmarks within budget"
    EXPECT_NOT "BUDGET EXCEEDED")
nbench_cli_test(relative_exceeded EXIT 4 BUDGETS relative_exceeded.txt ARGS --filter=self_*
    EXPECT "BUDGET EXCEEDED  self_work_8" "> 2.00x self_work_1 (" "budgets: FAILED")
# Every budget of a benchmark counts, not just its first line.
nbench_cli_test(every_line_counts EXIT 4 BUDGETS absolute_then_relative.txt ARGS --filter=self_*
    EXPECT "BUDGET EXCEEDED  self_work_8" "> 2.00x self_work_1 (")
# A base filtered out of the run skips the budget and says so; a base the
# executable does not have is an error.
nbench_cli_test(relative_base_not_run EXIT 0 BUDGETS relative_exceeded.txt
    ARGS --filter=self_work_8
    EXPECT "self_work_8's 2x self_work_1 skipped: self_work_1 did not run"
    EXPECT_NOT "BUDGET EXCEEDED")
nbench_cli_test(relative_unknown_base EXIT 4 BUDGETS relative_unknown_base.txt
    ARGS --filter=self_*
    EXPECT "unknown base 'no_such_benchmark' for self_work_8")

# Malformed lines.
nbench_cli_test(malformed_suffix EXIT 4 BUDGETS malformed_suffix.txt ARGS --filter=self_*
    EXPECT "malformed_suffix.txt:2: expected `name max_ns` or `name <k>x base`")
nbench_cli_test(malformed_no_base EXIT 4 BUDGETS malformed_no_base.txt ARGS --filter=self_*
    EXPECT "malformed_no_base.txt:1: expected `name <k>x base`")
nbench_cli_test(malformed_self EXIT 4 BUDGETS malformed_self.txt ARGS --filter=self_*
    EXPECT "malformed_self.txt:1: a budget relative to itself")

# The machine's unit runs in every nbench executable, and can carry budgets.
# It runs first and again after the rest, and the faster counts; alone, once.
nbench_cli_test(locked_add EXIT 0 BUDGETS locked_add.txt ARGS --filter=nbench.*
    EXPECT "nbench.locked_add" "budgets: all 1 benchmarks within budget"
    EXPECT_NOT "(again)")
nbench_cli_test(locked_add_twice EXIT 0 BUDGETS locked_add.txt
    EXPECT "nbench.locked_add (again)" "budgets: all 3 benchmarks within budget")
nbench_cli_test(locked_add_listed EXIT 0 ARGS --list
    EXPECT "nbench.locked_add" "self_work_1" "self_work_8")
