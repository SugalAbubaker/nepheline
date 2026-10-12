// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/platform.h>

#include "test_platform_support.h"

#include <string.h>

NTEST(platform, executable_path_names_this_test_binary)
{
    char path[1024];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_platform_executable_path(path, sizeof path, &len), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(len, strlen(path));
    NTEST_ASSERT_GT_UINT(len, 0U);
    NTEST_ASSERT_NOT_NULL(strstr(path, "platform_tests"));
#if NPH_PLATFORM_WINDOWS
    NTEST_ASSERT_TRUE(len > 2U && path[1] == ':'); // absolute: drive letter
#else
    NTEST_ASSERT_EQ_INT(path[0], '/');
#endif
    // out_len is optional.
    NTEST_ASSERT_EQ_INT(nph_platform_executable_path(path, sizeof path, NULL), NPH_RESULT_OK);
}

NTEST(platform, executable_path_overflow_reports_length)
{
    char small[8];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_platform_executable_path(small, sizeof small, &len),
                        NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_GT_UINT(len, sizeof small);
    NTEST_ASSERT_EQ_INT(small[0], '\0');
}

NTEST(platform, executable_dir_is_a_prefix_without_the_file_name)
{
    char path[1024];
    char dir[1024];
    u64 dir_len = 0U;
    NTEST_ASSERT_EQ_INT(nph_platform_executable_path(path, sizeof path, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_platform_executable_dir(dir, sizeof dir, &dir_len), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(dir_len, strlen(dir));
    NTEST_ASSERT_EQ_MEM(path, dir, dir_len);
    NTEST_ASSERT_TRUE(path[dir_len] == '/' || path[dir_len] == '\\');
    NTEST_ASSERT_NULL(strstr(dir, "platform_tests"));
    NTEST_ASSERT_TRUE(dir[dir_len - 1U] != '/' && dir[dir_len - 1U] != '\\');
}

NTEST(platform, env_get)
{
#if NPH_PLATFORM_WINDOWS
    const char *present = "SystemRoot";
#else
    const char *present = "HOME";
#endif
    char value[512];
    u64 len = 0U;
    const NphResult r = nph_platform_env_get(present, value, sizeof value, &len);
    if (r == NPH_RESULT_NOT_FOUND) {
        NTEST_SKIP("%s is not set in this environment", present);
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(len, strlen(value));
    NTEST_ASSERT_GT_UINT(len, 0U);

    NTEST_ASSERT_EQ_INT(
        nph_platform_env_get("NPH_SURELY_UNSET_VARIABLE_42", value, sizeof value, &len),
        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(value[0], '\0');

    char tiny[2];
    NTEST_ASSERT_EQ_INT(nph_platform_env_get(present, tiny, sizeof tiny, &len),
                        NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_GT_UINT(len, 1U);
}

NTEST(platform, process_and_os)
{
    NTEST_ASSERT_GT_UINT(nph_platform_process_id(), 0U);
#if NPH_PLATFORM_WINDOWS
    NTEST_ASSERT_EQ_STR(nph_platform_os_name(), "windows");
#else
    NTEST_ASSERT_EQ_STR(nph_platform_os_name(), "linux");
#endif
    // Whatever the answers, these must not crash or hang.
    const bool vt = nph_platform_console_enable_vt();
    const bool dbg = nph_platform_debugger_present();
    NTEST_LOG("console vt %d, debugger %d", (int)vt, (int)dbg);
}

NTEST(platform, log_category_exists)
{
    NTEST_ASSERT_EQ_STR(NPH_LOG_PLATFORM.name, "platform");
}
