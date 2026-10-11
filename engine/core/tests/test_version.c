// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/version.h>

#include <ntest/ntest.h>

#include <ctype.h>
#include <string.h>

NTEST(version, library_matches_header)
{
    // A mismatch here means the test binary was linked against a stale nph_core.
    NTEST_ASSERT_EQ_UINT(nph_version(), NPH_VERSION);
    NTEST_ASSERT_EQ_STR(nph_version_string(), NPH_VERSION_STRING);
}

NTEST(version, packed_fields_round_trip)
{
    const uint32_t v = NPH_MAKE_VERSION(1U, 2U, 3U);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_MAJOR(v), 1U);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_MINOR(v), 2U);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_PATCH(v), 3U);

    // Field extremes must not bleed into their neighbours.
    const uint32_t max = NPH_MAKE_VERSION(1023U, 1023U, 4095U);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_MAJOR(max), 1023U);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_MINOR(max), 1023U);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_PATCH(max), 4095U);
}

NTEST(version, packed_versions_order_numerically)
{
    NTEST_ASSERT_TRUE(NPH_MAKE_VERSION(0U, 9U, 9U) < NPH_MAKE_VERSION(1U, 0U, 0U));
    NTEST_ASSERT_TRUE(NPH_MAKE_VERSION(1U, 0U, 99U) < NPH_MAKE_VERSION(1U, 1U, 0U));
    NTEST_ASSERT_TRUE(NPH_MAKE_VERSION(1U, 1U, 0U) < NPH_MAKE_VERSION(1U, 1U, 1U));
}

NTEST(version, string_is_dotted_decimal)
{
    const char *s = nph_version_string();
    NTEST_ASSERT_NOT_NULL(s);
    NTEST_ASSERT_TRUE(strlen(s) >= 5U); // shortest possible: "0.0.0"

    int dots = 0;
    for (const char *p = s; *p != '\0'; ++p) {
        if (*p == '.') {
            ++dots;
        } else {
            NTEST_ASSERT_TRUE(isdigit((unsigned char)*p));
        }
    }
    NTEST_ASSERT_EQ_INT(dots, 2);
    NTEST_ASSERT_NE_INT(s[0], '.');
    NTEST_ASSERT_NE_INT(s[strlen(s) - 1U], '.');
}

NTEST(version, string_agrees_with_packed)
{
    // Parse "a.b.c" and check it equals the packed fields.
    const char *s = nph_version_string();
    unsigned int major = 0U;
    unsigned int minor = 0U;
    unsigned int patch = 0U;
    unsigned int *field = &major;
    for (const char *p = s; *p != '\0'; ++p) {
        if (*p == '.') {
            field = (field == &major) ? &minor : &patch;
        } else {
            *field = *field * 10U + (unsigned int)(*p - '0');
        }
    }
    const uint32_t v = nph_version();
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_MAJOR(v), major);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_MINOR(v), minor);
    NTEST_ASSERT_EQ_UINT(NPH_VERSION_GET_PATCH(v), patch);
}
