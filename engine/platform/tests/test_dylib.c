// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/dylib.h>

#include "test_platform_support.h"

#include <string.h>

#ifndef NPH_TEST_DYLIB_PATH
#error "NPH_TEST_DYLIB_PATH must point at the platform_test_dylib shared library"
#endif

typedef i32 (*AddFn)(i32, i32);
typedef const char *(*NameFn)(void);

static void test_placeholder_fn(void)
{
}

NTEST(dylib, open_call_and_close)
{
    NphDylib lib;
    NTEST_ASSERT_EQ_INT(nph_dylib_open(NPH_TEST_DYLIB_PATH, &lib), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_dylib_is_open(&lib));

    NphDylibFn fn = NULL;
    NTEST_ASSERT_EQ_INT(nph_dylib_function(&lib, "nph_test_dylib_add", &fn), NPH_RESULT_OK);
    if (fn == NULL) {
        NTEST_FAIL("nph_test_dylib_add resolved to NULL");
        return;
    }
    NTEST_ASSERT_EQ_INT(((AddFn)fn)(40, 2), 42);

    NTEST_ASSERT_EQ_INT(nph_dylib_function(&lib, "nph_test_dylib_name", &fn), NPH_RESULT_OK);
    if (fn == NULL) {
        NTEST_FAIL("nph_test_dylib_name resolved to NULL");
        return;
    }
    NTEST_ASSERT_EQ_STR(((NameFn)fn)(), "platform_test_dylib");

    void *data = NULL;
    NTEST_ASSERT_EQ_INT(nph_dylib_data(&lib, "nph_test_dylib_value", &data), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(*(const u32 *)data, 0xC0FFEEU);

    // Addresses inside the library's image, and outside it.
    NTEST_ASSERT_TRUE(nph_dylib_contains(&lib, data));
    NTEST_ASSERT_TRUE(nph_dylib_contains(&lib, ((NameFn)fn)())); // a string literal of the library
    static const u32 local = 1U;
    NTEST_ASSERT_FALSE(nph_dylib_contains(&lib, &local));
    NTEST_ASSERT_FALSE(nph_dylib_contains(&lib, NULL));

    nph_dylib_close(&lib);
    NTEST_ASSERT_FALSE(nph_dylib_is_open(&lib));
    NTEST_ASSERT_FALSE(nph_dylib_contains(&lib, &local));
    nph_dylib_close(&lib); // idempotent
}

NTEST(dylib, missing_symbol_is_not_found)
{
    NphDylib lib;
    NTEST_ASSERT_EQ_INT(nph_dylib_open(NPH_TEST_DYLIB_PATH, &lib), NPH_RESULT_OK);
    NphDylibFn fn = test_placeholder_fn;
    NTEST_ASSERT_EQ_INT(nph_dylib_function(&lib, "nph_test_dylib_absent", &fn),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_TRUE(fn == NULL);
    void *data = &lib;
    NTEST_ASSERT_EQ_INT(nph_dylib_data(&lib, "nph_test_dylib_absent", &data), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_NULL(data);
    nph_dylib_close(&lib);
}

NTEST(dylib, missing_file_is_not_found)
{
    NphDylib lib;
    lib.handle = &lib; // must be cleared by a failed open
    NTEST_ASSERT_EQ_INT(nph_dylib_open("./no_such_library_anywhere.xyz", &lib),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_FALSE(nph_dylib_is_open(&lib));
}

NTEST(dylib, open_twice_shares_the_module)
{
    NphDylib a;
    NphDylib b;
    NTEST_ASSERT_EQ_INT(nph_dylib_open(NPH_TEST_DYLIB_PATH, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_dylib_open(NPH_TEST_DYLIB_PATH, &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_PTR(a.handle, b.handle); // the loader reference-counts
    void *da = NULL;
    void *db = NULL;
    NTEST_ASSERT_EQ_INT(nph_dylib_data(&a, "nph_test_dylib_value", &da), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_dylib_data(&b, "nph_test_dylib_value", &db), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_PTR(da, db);
    nph_dylib_close(&a);
    // b still works after a's reference is dropped.
    NphDylibFn fn = NULL;
    NTEST_ASSERT_EQ_INT(nph_dylib_function(&b, "nph_test_dylib_add", &fn), NPH_RESULT_OK);
    if (fn == NULL) {
        NTEST_FAIL("nph_test_dylib_add resolved to NULL");
        return;
    }
    NTEST_ASSERT_EQ_INT(((AddFn)fn)(1, 2), 3);
    nph_dylib_close(&b);
}

NTEST(dylib, extension)
{
#if NPH_PLATFORM_WINDOWS
    NTEST_ASSERT_EQ_STR(nph_dylib_extension(), ".dll");
#else
    NTEST_ASSERT_EQ_STR(nph_dylib_extension(), ".so");
#endif
    const char *path = NPH_TEST_DYLIB_PATH;
    const u64 len = strlen(path);
    const u64 ext_len = strlen(nph_dylib_extension());
    NTEST_ASSERT_GT_UINT(len, ext_len);
    NTEST_ASSERT_EQ_STR(path + len - ext_len, nph_dylib_extension());
}
