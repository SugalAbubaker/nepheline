// SPDX-License-Identifier: Apache-2.0
// A shared library the dylib tests load: one function and one data export.
#include <nepheline/core/base.h>

NPH_EXPORT i32 nph_test_dylib_add(i32 a, i32 b);
NPH_EXPORT const char *nph_test_dylib_name(void);

NPH_EXPORT u32 nph_test_dylib_value = 0xC0FFEEU;

i32 nph_test_dylib_add(i32 a, i32 b)
{
    return a + b;
}

const char *nph_test_dylib_name(void)
{
    return "platform_test_dylib";
}
