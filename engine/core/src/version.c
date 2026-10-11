// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/version.h>

uint32_t nph_version(void)
{
    return NPH_VERSION;
}

const char *nph_version_string(void)
{
    return NPH_VERSION_STRING;
}
