// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: config text applied to a cvar registry, written back,
// and applied again, must never crash or leak.
#include <nepheline/core/cvar.h>

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static NPH_CVAR_BOOL(cv_flag, "a.flag", 1, "flag", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_INT(cv_count, "a.count", 3, -100, 100, "count", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_FLOAT(cv_ratio, "a.ratio", 0.5, 0.0, 1.0, "ratio", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_STRING(cv_name, "a.name", "x", "name", NPH_CVAR_FLAG_SAVED);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    NphAllocator *a = nph_allocator_system();
    NphCvarRegistry r;
    if (nph_cvar_registry_init(&r, a) != NPH_RESULT_OK) {
        abort();
    }
    NphCvar *all[] = {&cv_flag, &cv_count, &cv_ratio, &cv_name};
    for (u32 i = 0; i < 4U; ++i) {
        all[i]->registered = 0U;
        if (nph_cvar_register(&r, all[i]) != NPH_RESULT_OK) {
            abort();
        }
    }
    const NphStr text = nph_str((const char *)data, size);
    u32 line;
    NPH_IGNORE_RESULT(nph_cvar_apply_config(&r, text, &line));
    NphStrBuilder out;
    nph_sb_init(&out, a);
    if (nph_cvar_write_config(&r, &out) == NPH_RESULT_OK) {
        if (nph_cvar_apply_config(&r, nph_sb_str(&out), &line) != NPH_RESULT_OK) {
            abort(); // what we write must be readable
        }
    }
    nph_sb_free(&out);
    nph_cvar_registry_free(&r);
    return 0;
}
