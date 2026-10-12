// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/platform.h>

#include "platform_internal.h"

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_PLATFORM, "platform", NPH_LOG_LEVEL_TRACE);

const char *nph_platform_os_name(void)
{
#if NPH_PLATFORM_WINDOWS
    return "windows";
#else
    return "linux";
#endif
}

NphResult nph_platform_executable_dir(char *buf, u64 cap, u64 *out_len)
{
    u64 len = 0U;
    const NphResult r = nph_platform_executable_path(buf, cap, &len);
    if (r != NPH_RESULT_OK) {
        if (out_len != NULL) {
            *out_len = len;
        }
        return r;
    }
    u64 end = len;
    while (end > 0U && buf[end - 1U] != '/' && buf[end - 1U] != '\\') {
        --end;
    }
    // Keep a root separator ("/" or "C:\") rather than returning "".
    if (end > 1U && buf[end - 2U] != ':') {
        --end;
    }
    buf[end] = '\0';
    if (out_len != NULL) {
        *out_len = end;
    }
    return NPH_RESULT_OK;
}
