// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/cvar.h>
#include <nepheline/core/log.h>
#include <nepheline/core/string_id.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool name_is_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (const char *p = name; *p != '\0'; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || nph_ascii_is_digit(c) || c == '.' || c == '_')) {
            return false;
        }
    }
    return true;
}

static u64 name_key(NphStr name)
{
    return nph_str_id(name).value;
}

// Owned string helpers: the registry copies every string value it stores.
static void release_string(NphCvarRegistry *r, NphCvar *cvar)
{
    if (cvar->value_string != NULL && cvar->value_string != cvar->default_string) {
        nph_str_free(r->alloc, (char *)(uptr)cvar->value_string, (u64)strlen(cvar->value_string));
    }
    cvar->value_string = cvar->default_string;
}

static void notify(NphCvarRegistry *r, NphCvar *cvar)
{
    cvar->change_count += 1U;
    r->version += 1U;
    if (cvar->on_change != NULL) {
        cvar->on_change(cvar, cvar->on_change_user);
    }
}

NphResult nph_cvar_registry_init(NphCvarRegistry *r, NphAllocator *alloc)
{
    NPH_ASSERT(r != NULL && alloc != NULL);
    memset(r, 0, sizeof *r);
    r->alloc = alloc;
    const NphHashMapDesc by_name = nph_hashmap_desc_u64(sizeof(NphCvar *));
    NPH_TRY(nph_hashmap_init(&r->by_name, alloc, &by_name));
    const NphHashMapDesc pending = nph_hashmap_desc_u64(sizeof(char *));
    NPH_TRY(nph_hashmap_init(&r->pending, alloc, &pending));
    NPH_ARRAY_INIT(&r->cvars, alloc);
    return NPH_RESULT_OK;
}

void nph_cvar_registry_free(NphCvarRegistry *r)
{
    if (r == NULL || r->alloc == NULL) {
        return;
    }
    NPH_ARRAY_FOREACH (void *, entry, &r->cvars) {
        NphCvar *cvar = (NphCvar *)*entry;
        if (cvar->type == NPH_CVAR_TYPE_STRING) {
            release_string(r, cvar);
        }
        cvar->registered = 0U;
    }
    NphHashMapIter it = nph_hashmap_iter(&r->pending);
    const void *key;
    void *val;
    while (nph_hashmap_next(&it, &key, &val)) {
        char *text = *(char **)val;
        nph_str_free(r->alloc, text, (u64)strlen(text));
    }
    nph_hashmap_free(&r->pending);
    nph_hashmap_free(&r->by_name);
    NPH_ARRAY_FREE(&r->cvars);
    memset(r, 0, sizeof *r);
}

static bool default_in_range(const NphCvar *cvar)
{
    switch (cvar->type) {
    case NPH_CVAR_TYPE_INT:
        return cvar->min_int <= cvar->max_int && cvar->default_int >= cvar->min_int &&
               cvar->default_int <= cvar->max_int;
    case NPH_CVAR_TYPE_FLOAT:
        return cvar->min_float <= cvar->max_float && cvar->default_float >= cvar->min_float &&
               cvar->default_float <= cvar->max_float;
    case NPH_CVAR_TYPE_BOOL:
    case NPH_CVAR_TYPE_STRING:
        return true;
    }
    return false;
}

NphResult nph_cvar_register(NphCvarRegistry *r, NphCvar *cvar)
{
    NPH_ASSERT(r != NULL && r->alloc != NULL && cvar != NULL);
    NPH_ASSERT_MSG(name_is_valid(cvar->name), "cvar name \"%s\" is not [a-z0-9._]+",
                   cvar->name != NULL ? cvar->name : "(null)");
    if (!name_is_valid(cvar->name) || !default_in_range(cvar) || cvar->registered != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 key = name_key(nph_str_from_cstr(cvar->name));
    if (nph_hashmap_contains_u64(&r->by_name, key)) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    NPH_TRY(NPH_ARRAY_PUSH(&r->cvars, (void *)cvar));
    const NphResult inserted = nph_hashmap_insert_u64(&r->by_name, key, &cvar);
    if (inserted != NPH_RESULT_OK) {
        r->cvars.len -= 1U;
        return inserted;
    }
    cvar->registered = 1U;
    // Start from the defaults, then apply a pending config value if any.
    cvar->value_bool = cvar->default_bool;
    cvar->value_int = cvar->default_int;
    cvar->value_float = cvar->default_float;
    cvar->value_string = cvar->default_string;
    char **pending = (char **)nph_hashmap_find_u64(&r->pending, key);
    if (pending != NULL) {
        char *text = *pending;
        (void)nph_hashmap_remove_u64(&r->pending, key);
        const NphResult applied = nph_cvar_force_from_string(r, cvar, nph_str_from_cstr(text));
        if (applied != NPH_RESULT_OK) {
            // A config value that does not fit the cvar is a content error,
            // not a registration failure: report it and keep the default.
            NPH_LOG_WARN(&NPH_LOG_CORE, "cvar %s: config value \"%s\" rejected (%s)", cvar->name,
                         text, nph_result_string(applied));
        }
        nph_str_free(r->alloc, text, (u64)strlen(text));
    }
    return NPH_RESULT_OK;
}

NphCvar *nph_cvar_find(const NphCvarRegistry *r, NphStr name)
{
    NPH_ASSERT(r != NULL);
    NphCvar **found = (NphCvar **)nph_hashmap_find_u64(&r->by_name, name_key(name));
    return found != NULL ? *found : NULL;
}

static NphResult check_writable(const NphCvarRegistry *r, const NphCvar *cvar, bool force)
{
    if (force) {
        return NPH_RESULT_OK;
    }
    if ((cvar->flags & NPH_CVAR_FLAG_READ_ONLY) != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if ((cvar->flags & NPH_CVAR_FLAG_CHEAT) != 0U && !r->cheats_enabled) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return NPH_RESULT_OK;
}

static NphResult set_bool(NphCvarRegistry *r, NphCvar *cvar, b32 value, bool force)
{
    if (cvar->type != NPH_CVAR_TYPE_BOOL) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    NPH_TRY(check_writable(r, cvar, force));
    const b32 normalized = value != 0;
    if (cvar->value_bool != normalized) {
        cvar->value_bool = normalized;
        notify(r, cvar);
    }
    return NPH_RESULT_OK;
}

static NphResult set_int(NphCvarRegistry *r, NphCvar *cvar, i64 value, bool force)
{
    if (cvar->type != NPH_CVAR_TYPE_INT) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    NPH_TRY(check_writable(r, cvar, force));
    const i64 clamped =
        value < cvar->min_int ? cvar->min_int : (value > cvar->max_int ? cvar->max_int : value);
    if (cvar->value_int != clamped) {
        cvar->value_int = clamped;
        notify(r, cvar);
    }
    return NPH_RESULT_OK;
}

static NphResult set_float(NphCvarRegistry *r, NphCvar *cvar, f64 value, bool force)
{
    if (cvar->type != NPH_CVAR_TYPE_FLOAT) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    NPH_TRY(check_writable(r, cvar, force));
    if (!(value == value)) {
        return NPH_RESULT_INVALID_ARGUMENT; // NaN
    }
    const f64 clamped = value < cvar->min_float
                            ? cvar->min_float
                            : (value > cvar->max_float ? cvar->max_float : value);
    if (cvar->value_float != clamped) {
        cvar->value_float = clamped;
        notify(r, cvar);
    }
    return NPH_RESULT_OK;
}

static NphResult set_string(NphCvarRegistry *r, NphCvar *cvar, NphStr value, bool force)
{
    if (cvar->type != NPH_CVAR_TYPE_STRING) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    NPH_TRY(check_writable(r, cvar, force));
    if (nph_str_equals(nph_str_from_cstr(cvar->value_string), value)) {
        return NPH_RESULT_OK;
    }
    char *copy;
    NPH_TRY(nph_str_dup(r->alloc, value, &copy));
    release_string(r, cvar);
    cvar->value_string = copy;
    notify(r, cvar);
    return NPH_RESULT_OK;
}

NphResult nph_cvar_set_bool(NphCvarRegistry *r, NphCvar *cvar, b32 value)
{
    NPH_ASSERT(r != NULL && cvar != NULL && cvar->registered != 0U);
    return set_bool(r, cvar, value, false);
}

NphResult nph_cvar_set_int(NphCvarRegistry *r, NphCvar *cvar, i64 value)
{
    NPH_ASSERT(r != NULL && cvar != NULL && cvar->registered != 0U);
    return set_int(r, cvar, value, false);
}

NphResult nph_cvar_set_float(NphCvarRegistry *r, NphCvar *cvar, f64 value)
{
    NPH_ASSERT(r != NULL && cvar != NULL && cvar->registered != 0U);
    return set_float(r, cvar, value, false);
}

NphResult nph_cvar_set_string(NphCvarRegistry *r, NphCvar *cvar, NphStr value)
{
    NPH_ASSERT(r != NULL && cvar != NULL && cvar->registered != 0U);
    return set_string(r, cvar, value, false);
}

static NphResult set_from_string(NphCvarRegistry *r, NphCvar *cvar, NphStr text, bool force)
{
    text = nph_str_trim(text);
    switch (cvar->type) {
    case NPH_CVAR_TYPE_BOOL: {
        bool b;
        if (!nph_str_parse_bool(text, &b)) {
            return NPH_RESULT_PARSE_ERROR;
        }
        return set_bool(r, cvar, b, force);
    }
    case NPH_CVAR_TYPE_INT: {
        i64 i;
        if (!nph_str_parse_i64(text, &i)) {
            return NPH_RESULT_PARSE_ERROR;
        }
        return set_int(r, cvar, i, force);
    }
    case NPH_CVAR_TYPE_FLOAT: {
        f64 f;
        if (!nph_str_parse_f64(text, &f)) {
            return NPH_RESULT_PARSE_ERROR;
        }
        return set_float(r, cvar, f, force);
    }
    case NPH_CVAR_TYPE_STRING:
        return set_string(r, cvar, text, force);
    }
    return NPH_RESULT_TYPE_MISMATCH;
}

NphResult nph_cvar_set_from_string(NphCvarRegistry *r, NphCvar *cvar, NphStr text)
{
    NPH_ASSERT(r != NULL && cvar != NULL && cvar->registered != 0U);
    return set_from_string(r, cvar, text, false);
}

NphResult nph_cvar_force_from_string(NphCvarRegistry *r, NphCvar *cvar, NphStr text)
{
    NPH_ASSERT(r != NULL && cvar != NULL && cvar->registered != 0U);
    return set_from_string(r, cvar, text, true);
}

NphResult nph_cvar_reset(NphCvarRegistry *r, NphCvar *cvar)
{
    NPH_ASSERT(r != NULL && cvar != NULL && cvar->registered != 0U);
    switch (cvar->type) {
    case NPH_CVAR_TYPE_BOOL:
        return set_bool(r, cvar, cvar->default_bool, true);
    case NPH_CVAR_TYPE_INT:
        return set_int(r, cvar, cvar->default_int, true);
    case NPH_CVAR_TYPE_FLOAT:
        return set_float(r, cvar, cvar->default_float, true);
    case NPH_CVAR_TYPE_STRING:
        return set_string(r, cvar, nph_str_from_cstr(cvar->default_string), true);
    }
    return NPH_RESULT_TYPE_MISMATCH;
}

u64 nph_cvar_format(const NphCvar *cvar, char *out, u64 cap)
{
    NPH_ASSERT(cvar != NULL && out != NULL);
    int n = 0;
    switch (cvar->type) {
    case NPH_CVAR_TYPE_BOOL:
        n = snprintf(out, (size_t)cap, "%s", cvar->value_bool ? "true" : "false");
        break;
    case NPH_CVAR_TYPE_INT:
        n = snprintf(out, (size_t)cap, "%lld", (long long)cvar->value_int);
        break;
    case NPH_CVAR_TYPE_FLOAT:
        n = snprintf(out, (size_t)cap, "%.17g", cvar->value_float);
        break;
    case NPH_CVAR_TYPE_STRING:
        n = snprintf(out, (size_t)cap, "%s", cvar->value_string != NULL ? cvar->value_string : "");
        break;
    }
    return n < 0 ? 0U : (u64)n;
}

// Stores a value for a name that is not registered yet.
static NphResult store_pending(NphCvarRegistry *r, NphStr name, NphStr value)
{
    const u64 key = name_key(name);
    char *copy;
    NPH_TRY(nph_str_dup(r->alloc, value, &copy));
    char **slot = (char **)nph_hashmap_find_u64(&r->pending, key);
    if (slot != NULL) {
        nph_str_free(r->alloc, *slot, (u64)strlen(*slot));
        *slot = copy;
        return NPH_RESULT_OK;
    }
    const NphResult inserted = nph_hashmap_insert_u64(&r->pending, key, &copy);
    if (inserted != NPH_RESULT_OK) {
        nph_str_free(r->alloc, copy, value.len);
    }
    return inserted;
}

// Strips one pair of surrounding double quotes.
static NphStr unquote(NphStr value)
{
    if (value.len >= 2U && value.data[0] == '"' && value.data[value.len - 1U] == '"') {
        return nph_str_slice(value, 1U, value.len - 1U);
    }
    return value;
}

static NphResult apply_pair(NphCvarRegistry *r, NphStr name, NphStr value)
{
    name = nph_str_trim(name);
    value = unquote(nph_str_trim(value));
    if (name.len == 0U) {
        return NPH_RESULT_PARSE_ERROR;
    }
    NphCvar *cvar = nph_cvar_find(r, name);
    if (cvar == NULL) {
        return store_pending(r, name, value);
    }
    return set_from_string(r, cvar, value, true);
}

NphResult nph_cvar_apply_config(NphCvarRegistry *r, NphStr text, u32 *out_error_line)
{
    NPH_ASSERT(r != NULL);
    NphResult result = NPH_RESULT_OK;
    u32 line_number = 0U;
    NphStr rest = text;
    NphStr line;
    while (nph_str_split_first(rest, '\n', &line, &rest)) {
        ++line_number;
        const u64 hash = nph_str_find_char(line, '#');
        if (hash != NPH_STR_NPOS) {
            line = nph_str_slice(line, 0U, hash);
        }
        line = nph_str_trim(line);
        if (line.len == 0U) {
            continue;
        }
        const u64 eq = nph_str_find_char(line, '=');
        if (eq == NPH_STR_NPOS) {
            if (result == NPH_RESULT_OK) {
                result = NPH_RESULT_PARSE_ERROR;
                if (out_error_line != NULL) {
                    *out_error_line = line_number;
                }
            }
            continue;
        }
        const NphResult applied =
            apply_pair(r, nph_str_slice(line, 0U, eq), nph_str_slice(line, eq + 1U, line.len));
        if (applied != NPH_RESULT_OK && result == NPH_RESULT_OK) {
            result = applied;
            if (out_error_line != NULL) {
                *out_error_line = line_number;
            }
        }
    }
    return result;
}

NphResult nph_cvar_apply_assignment(NphCvarRegistry *r, NphStr assignment)
{
    NPH_ASSERT(r != NULL);
    const u64 eq = nph_str_find_char(assignment, '=');
    if (eq == NPH_STR_NPOS) {
        return NPH_RESULT_PARSE_ERROR;
    }
    return apply_pair(r, nph_str_slice(assignment, 0U, eq),
                      nph_str_slice(assignment, eq + 1U, assignment.len));
}

static int compare_cvar_names(const void *a, const void *b)
{
    const NphCvar *x = *(const NphCvar *const *)a;
    const NphCvar *y = *(const NphCvar *const *)b;
    return strcmp(x->name, y->name);
}

NphResult nph_cvar_write_config(const NphCvarRegistry *r, NphStrBuilder *out)
{
    NPH_ASSERT(r != NULL && out != NULL);
    const u32 count = r->cvars.len;
    if (count == 0U) {
        return NPH_RESULT_OK;
    }
    NphCvar **sorted = NPH_ALLOC_ARRAY(out->alloc, NphCvar *, count);
    if (sorted == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < count; ++i) {
        sorted[i] = (NphCvar *)r->cvars.data[i];
    }
    qsort((void *)sorted, count, sizeof sorted[0], compare_cvar_names);
    NphResult result = NPH_RESULT_OK;
    for (u32 i = 0; i < count && result == NPH_RESULT_OK; ++i) {
        const NphCvar *cvar = sorted[i];
        if ((cvar->flags & NPH_CVAR_FLAG_SAVED) == 0U) {
            continue;
        }
        char value[512];
        nph_cvar_format(cvar, value, sizeof value);
        if (cvar->type == NPH_CVAR_TYPE_STRING) {
            result = nph_sb_appendf(out, "%s = \"%s\"\n", cvar->name, value);
        } else {
            result = nph_sb_appendf(out, "%s = %s\n", cvar->name, value);
        }
    }
    NPH_FREE_ARRAY(out->alloc, NphCvar *, sorted, count);
    return result;
}
