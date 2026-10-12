// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/platform/dylib.h>

#include "platform_internal.h"

#include <dlfcn.h>
#include <unistd.h>

NphResult nph_dylib_open(const char *path, NphDylib *out)
{
    NPH_ASSERT(path != NULL && out != NULL);
    out->handle = NULL;
    if (access(path, F_OK) != 0) {
        return NPH_RESULT_NOT_FOUND;
    }
    // RTLD_LOCAL keeps a game module's symbols out of the global namespace
    // so two versions can coexist during a hot reload.
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        const char *message = dlerror();
        NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "dlopen(%s): %s", path,
                      message != NULL ? message : "unknown error");
        return NPH_RESULT_IO_ERROR;
    }
    out->handle = handle;
    return NPH_RESULT_OK;
}

void nph_dylib_close(NphDylib *lib)
{
    NPH_ASSERT(lib != NULL);
    if (lib->handle != NULL) {
        NPH_VERIFY(dlclose(lib->handle) == 0);
        lib->handle = NULL;
    }
}

// dlsym returns an object pointer for every symbol; a union converts it to
// a function pointer without the cast C forbids (POSIX guarantees the
// representations agree on platforms that have dlsym).
NphResult nph_dylib_function(const NphDylib *lib, const char *name, NphDylibFn *out)
{
    NPH_ASSERT(lib != NULL && name != NULL && out != NULL);
    NPH_ASSERT(lib->handle != NULL);
    *out = NULL;
    union {
        void *data;
        NphDylibFn fn;
    } u;
    u.data = dlsym(lib->handle, name);
    if (u.data == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    *out = u.fn;
    return NPH_RESULT_OK;
}

NphResult nph_dylib_data(const NphDylib *lib, const char *name, void **out)
{
    NPH_ASSERT(lib != NULL && name != NULL && out != NULL);
    NPH_ASSERT(lib->handle != NULL);
    *out = dlsym(lib->handle, name);
    return *out != NULL ? NPH_RESULT_OK : NPH_RESULT_NOT_FOUND;
}

const char *nph_dylib_extension(void)
{
    return ".so";
}
