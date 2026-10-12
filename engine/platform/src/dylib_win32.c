// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/platform/dylib.h>

#include "platform_internal.h"

NphResult nph_dylib_open(const char *path, NphDylib *out)
{
    NPH_ASSERT(path != NULL && out != NULL);
    out->handle = NULL;
    wchar_t wide[NPH_WIN32_PATH_MAX];
    const NphResult conv = nph_win32_utf8_to_wide(path, wide, (u32)NPH_ARRAY_COUNT(wide));
    if (conv != NPH_RESULT_OK) {
        return conv;
    }
    // Distinguish "no such file" from "cannot load": LoadLibrary reports
    // ERROR_MOD_NOT_FOUND for a missing dependency as well.
    if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) {
        return NPH_RESULT_NOT_FOUND;
    }
    const HMODULE module = LoadLibraryW(wide);
    if (module == NULL) {
        char message[256];
        const DWORD error = GetLastError();
        nph_win32_error_message(error, message, (u32)sizeof message);
        NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "LoadLibrary(%s): %s (%lu)", path, message,
                      (unsigned long)error);
        return NPH_RESULT_IO_ERROR;
    }
    out->handle = module;
    return NPH_RESULT_OK;
}

void nph_dylib_close(NphDylib *lib)
{
    NPH_ASSERT(lib != NULL);
    if (lib->handle != NULL) {
        NPH_VERIFY(FreeLibrary((HMODULE)lib->handle));
        lib->handle = NULL;
    }
}

NphResult nph_dylib_function(const NphDylib *lib, const char *name, NphDylibFn *out)
{
    NPH_ASSERT(lib != NULL && name != NULL && out != NULL);
    NPH_ASSERT(lib->handle != NULL);
    *out = NULL;
    const FARPROC proc = GetProcAddress((HMODULE)lib->handle, name);
    if (proc == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    *out = (NphDylibFn)proc;
    return NPH_RESULT_OK;
}

NphResult nph_dylib_data(const NphDylib *lib, const char *name, void **out)
{
    NPH_ASSERT(lib != NULL && name != NULL && out != NULL);
    NPH_ASSERT(lib->handle != NULL);
    *out = NULL;
    // GetProcAddress returns a function pointer type even for data exports;
    // a union converts it without a function/data pointer cast.
    union {
        FARPROC fn;
        void *data;
    } u;
    u.fn = GetProcAddress((HMODULE)lib->handle, name);
    if (u.fn == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    *out = u.data;
    return NPH_RESULT_OK;
}

const char *nph_dylib_extension(void)
{
    return ".dll";
}
