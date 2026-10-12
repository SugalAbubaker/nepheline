// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/path.h>

#include <ntest/ntest.h>

#include <string.h>

#define S(lit) NPH_STR(lit)

static bool eq(NphStr a, const char *b)
{
    return nph_str_equals(a, nph_str_from_cstr(b));
}

NTEST(path, basename)
{
    NTEST_ASSERT_TRUE(eq(nph_path_basename(S("a/b/c.txt")), "c.txt"));
    NTEST_ASSERT_TRUE(eq(nph_path_basename(S("a\\b\\c.txt")), "c.txt"));
    NTEST_ASSERT_TRUE(eq(nph_path_basename(S("c.txt")), "c.txt"));
    NTEST_ASSERT_TRUE(eq(nph_path_basename(S("a/b/")), ""));
    NTEST_ASSERT_TRUE(eq(nph_path_basename(S("")), ""));
}

NTEST(path, dirname)
{
    NTEST_ASSERT_TRUE(eq(nph_path_dirname(S("a/b/c.txt")), "a/b"));
    NTEST_ASSERT_TRUE(eq(nph_path_dirname(S("c.txt")), ""));
    NTEST_ASSERT_TRUE(eq(nph_path_dirname(S("/c")), "/"));
    NTEST_ASSERT_TRUE(eq(nph_path_dirname(S("C:\\x")), "C:\\"));
    NTEST_ASSERT_TRUE(eq(nph_path_dirname(S("C:/x/y")), "C:/x"));
    NTEST_ASSERT_TRUE(eq(nph_path_dirname(S("a/b/")), "a/b"));
}

NTEST(path, extension_and_stem)
{
    NTEST_ASSERT_TRUE(eq(nph_path_extension(S("a/b.tar.gz")), ".gz"));
    NTEST_ASSERT_TRUE(eq(nph_path_stem(S("a/b.tar.gz")), "b.tar"));
    NTEST_ASSERT_TRUE(eq(nph_path_extension(S("a.b/c")), ""));
    NTEST_ASSERT_TRUE(eq(nph_path_stem(S("a.b/c")), "c"));
    NTEST_ASSERT_TRUE(eq(nph_path_extension(S(".hidden")), ""));
    NTEST_ASSERT_TRUE(eq(nph_path_stem(S(".hidden")), ".hidden"));
    NTEST_ASSERT_TRUE(eq(nph_path_extension(S("mesh.nph")), ".nph"));
    NTEST_ASSERT_TRUE(eq(nph_path_stem(S("mesh.nph")), "mesh"));
}

NTEST(path, is_absolute)
{
    NTEST_ASSERT_TRUE(nph_path_is_absolute(S("/usr")));
    NTEST_ASSERT_TRUE(nph_path_is_absolute(S("C:\\Users")));
    NTEST_ASSERT_TRUE(nph_path_is_absolute(S("c:/users")));
    NTEST_ASSERT_TRUE(nph_path_is_absolute(S("\\\\server\\share")));
    NTEST_ASSERT_FALSE(nph_path_is_absolute(S("usr")));
    NTEST_ASSERT_FALSE(nph_path_is_absolute(S("C:x"))); // drive-relative
    NTEST_ASSERT_FALSE(nph_path_is_absolute(S("")));
}

NTEST(path, join)
{
    char buf[64];
    u64 len = 0U;
    NTEST_ASSERT_EQ_INT(nph_path_join(buf, sizeof buf, S("a"), S("b"), &len), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "a/b");
    NTEST_ASSERT_EQ_UINT(len, 3U);
    NTEST_ASSERT_EQ_INT(nph_path_join(buf, sizeof buf, S("a/"), S("b"), NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "a/b");
    NTEST_ASSERT_EQ_INT(nph_path_join(buf, sizeof buf, S("a\\"), S("b"), NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "a\\b");
    NTEST_ASSERT_EQ_INT(nph_path_join(buf, sizeof buf, S(""), S("b"), NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "b");
    NTEST_ASSERT_EQ_INT(nph_path_join(buf, sizeof buf, S("a"), S(""), NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "a/");
    NTEST_ASSERT_EQ_INT(nph_path_join(buf, sizeof buf, S("a"), S("/abs"), NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(buf, "/abs");
    char tiny[4];
    NTEST_ASSERT_EQ_INT(nph_path_join(tiny, sizeof tiny, S("abc"), S("def"), &len),
                        NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_EQ_UINT(len, 7U);
    NTEST_ASSERT_EQ_STR(tiny, "");
}

NTEST(path, normalize_separators)
{
    char p[] = "a\\b\\c/d";
    nph_path_normalize_separators(p);
    NTEST_ASSERT_EQ_STR(p, "a/b/c/d");
}
