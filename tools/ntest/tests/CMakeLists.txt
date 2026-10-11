# SPDX-License-Identifier: Apache-2.0
# ntest's own tests.
#   ntest           unit tests of the assertion macros, fixtures, and helpers
#   ntest.cli.*     black-box tests: run ntest_failcases (an executable whose
#                   tests deliberately fail) and check exit code, output, and
#                   the JUnit report through run_check.cmake

nph_add_test(ntest SOURCES test_ntest.c)
target_include_directories(ntest_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(ntest_failcases test_ntest_failing.c)
target_link_libraries(ntest_failcases PRIVATE nph::options ntest::main)
set_target_properties(ntest_failcases PROPERTIES FOLDER "tests")

# ntest_cli_test(<name> EXIT <code> [ARGS ...] [EXPECT <substring>...]
#                [EXPECT_NOT <substring>...] [JUNIT EXPECT_JUNIT <substring>...])
function(ntest_cli_test name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "JUNIT" "EXIT" "ARGS;EXPECT;EXPECT_NOT;EXPECT_JUNIT")
    set(script "${CMAKE_CURRENT_SOURCE_DIR}/run_check.cmake")
    set(exe "$<TARGET_FILE:ntest_failcases>")
    if(ARG_JUNIT)
        add_test(NAME ntest.cli.${name}
            COMMAND "${CMAKE_COMMAND}"
                "-DEXE=${exe}" "-DARGS=${ARG_ARGS}" "-DEXPECT_EXIT=${ARG_EXIT}"
                "-DEXPECT=${ARG_EXPECT}" "-DEXPECT_NOT=${ARG_EXPECT_NOT}"
                "-DJUNIT=${CMAKE_BINARY_DIR}/test-results/$<CONFIG>/ntest-cli/${name}.xml"
                "-DEXPECT_JUNIT=${ARG_EXPECT_JUNIT}"
                -P "${script}")
    else()
        add_test(NAME ntest.cli.${name}
            COMMAND "${CMAKE_COMMAND}"
                "-DEXE=${exe}" "-DARGS=${ARG_ARGS}" "-DEXPECT_EXIT=${ARG_EXIT}"
                "-DEXPECT=${ARG_EXPECT}" "-DEXPECT_NOT=${ARG_EXPECT_NOT}"
                -P "${script}")
    endif()
    set_tests_properties(ntest.cli.${name} PROPERTIES LABELS "unit" TIMEOUT 120)
endfunction()

# The failing executable has 11 tests: 2 pass, 1 skips, 8 fail (see test_ntest_failing.c).
ntest_cli_test(failing_run EXIT 1
    EXPECT
        "[  FAILED  ] fail.eq_int"
        "lhs: 1 = 1"
        "rhs: 2 = 2"
        "expected strings equal"
        "differ at index 3"
        "first difference at byte 2"
        "ULPs"
        "continues after expect"
        "explicit failure 7"
        "[  SKIPPED ] fail.skipped: not today"
        "[       OK ] fail.teardown_observed"
        "[  PASSED  ] 2 tests"
        "[  SKIPPED ] 1 test"
        "[  FAILED  ] 8 tests"
    EXPECT_NOT
        "unreachable after fatal assert")

ntest_cli_test(filter_positive EXIT 0 ARGS --filter=fail.passes
    EXPECT "[  PASSED  ] 1 test"
    EXPECT_NOT "FAILED  ] fail")

ntest_cli_test(filter_negative EXIT 1 ARGS --filter=-fail.*
    EXPECT "TeardownFixture.teardown_runs_after_fatal"
    EXPECT_NOT "fail.eq_int")

ntest_cli_test(filter_suite_only EXIT 1 ARGS --filter=TeardownFixture
    EXPECT "[  FAILED  ] 1 test"
    EXPECT_NOT "fail.passes")

ntest_cli_test(filter_no_match EXIT 3 ARGS --filter=nothing.*)

ntest_cli_test(list EXIT 0 ARGS --list
    EXPECT "fail.eq_int" "TeardownFixture.teardown_runs_after_fatal"
    EXPECT_NOT "[ RUN")

ntest_cli_test(bad_option EXIT 2 ARGS --bogus)

ntest_cli_test(shuffle EXIT 1 ARGS --shuffle=7
    EXPECT "shuffle seed: 7")

ntest_cli_test(repeat EXIT 0 ARGS --filter=fail.passes --repeat=3
    EXPECT "[  PASSED  ] 3 tests")

ntest_cli_test(fail_fast EXIT 1 ARGS --fail-fast
    EXPECT "[  FAILED  ] 1 test"
    EXPECT_NOT "fail.eq_str")

ntest_cli_test(quiet EXIT 0 ARGS --quiet --filter=fail.passes
    EXPECT "[  PASSED  ] 1 test"
    EXPECT_NOT "[ RUN")

ntest_cli_test(junit EXIT 1 JUNIT
    EXPECT_JUNIT
        "<testsuites name=\"ntest_failcases\" tests=\"11\" failures=\"8\" skipped=\"1\""
        "<testsuite name=\"fail\""
        "<testcase classname=\"fail\" name=\"eq_int\""
        "<failure message=\""
        "<skipped message=\"not today\"/>"
        "<system-out>continues after expect"
        "&lt;html&gt;")
