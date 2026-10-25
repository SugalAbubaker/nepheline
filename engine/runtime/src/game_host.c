// SPDX-License-Identifier: Apache-2.0
// The game host: loading game modules, swapping in newer builds, and the
// services game code calls. See game.h, docs/runtime.md "Game modules",
// and docs/adr/0024-game-modules.md.
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/profile.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

#include "engine_internal.h"

#include <stdio.h>
#include <string.h>

// A build is swapped in once its file has stopped changing for this long
// (a linker writes the file in several steps).
#define SETTLE_NS       (300ULL * NPH_TIME_NS_PER_MS)
#define STATE_MAX_ALIGN 4096U
#define STATE_MAX_SIZE  (1ULL << 30U)

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

static NphResult source_info(const char *path, u64 *out_modified, u64 *out_size)
{
    NphFileInfo info;
    NPH_TRY(nph_fs_stat(path, &info));
    if (!info.is_regular) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    *out_modified = info.modified_ns;
    *out_size = info.size;
    return NPH_RESULT_OK;
}

// <temp>/nepheline-<pid>, created on first use.
static NphResult copies_dir(char *out, u64 cap)
{
    char temp[GAME_PATH_MAX];
    u64 len = 0U;
    NPH_TRY(nph_fs_temp_directory(temp, sizeof temp, &len));
    char leaf[64];
    const int n = snprintf(leaf, sizeof leaf, "nepheline-%llu",
                           (unsigned long long)nph_platform_process_id());
    NPH_ASSERT(n > 0 && (u64)n < sizeof leaf);
    NPH_TRY(nph_path_join(out, cap, nph_str(temp, len), nph_str(leaf, (u64)n), NULL));
    return nph_fs_create_directories(out);
}

// Copies the build at `source` to a fresh temporary name, so the build can
// overwrite the original while the copy is loaded (Windows locks a loaded
// image's file). `out_hash` receives the content's hash.
static NphResult copy_build(NphEngine *e, const char *source, char *out, u64 cap, u64 *out_hash)
{
    NphGame *g = &e->game;
    char dir[GAME_PATH_MAX];
    NPH_TRY(copies_dir(dir, sizeof dir));
    const NphStr stem = nph_path_stem(nph_str_from_cstr(source));
    char name[160];
    const int n = snprintf(name, sizeof name, "%.*s-%u%s", (int)nph_min_u64(stem.len, 96U),
                           stem.data, g->copies, nph_dylib_extension());
    if (n <= 0 || (u64)n >= sizeof name) {
        return NPH_RESULT_OVERFLOW;
    }
    NPH_TRY(nph_path_join(out, cap, nph_str_from_cstr(dir), nph_str(name, (u64)n), NULL));
    NphAllocator *scratch = engine_alloc(e, ENGINE_ALLOC_GAME);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(source, scratch, &data, &size));
    *out_hash = nph_hash64(data, size, 0U);
    const NphResult r = nph_file_write_all(out, data, size);
    nph_file_free_all(scratch, data, size);
    if (r == NPH_RESULT_OK) {
        g->copies += 1U;
    }
    return r;
}

// The hash of a file's content, or 0 when it cannot be read (a build being
// written: the next check sees it).
static u64 content_hash(NphEngine *e, const char *path)
{
    NphAllocator *scratch = engine_alloc(e, ENGINE_ALLOC_GAME);
    void *data = NULL;
    u64 size = 0U;
    if (nph_file_read_all(path, scratch, &data, &size) != NPH_RESULT_OK) {
        return 0U;
    }
    const u64 h = nph_hash64(data, size, 0U);
    nph_file_free_all(scratch, data, size);
    return h;
}

// ---------------------------------------------------------------------------
// Component descriptor copies
// ---------------------------------------------------------------------------

// Defaults are copied at the strictest alignment a component may have
// (checked when the build is validated).
#define COPY_DEFAULTS_ALIGN 16U

static void free_component(NphAllocator *alloc, GameComponentCopy *c)
{
    if (c->fields != NULL) {
        NPH_FREE_ARRAY(alloc, NphFieldDesc, c->fields, c->desc.field_count);
    }
    if (c->text != NULL) {
        NPH_FREE_ARRAY(alloc, char, c->text, c->text_bytes);
    }
    if (c->defaults != NULL) {
        NPH_FREE(alloc, c->defaults, c->desc.size, COPY_DEFAULTS_ALIGN);
    }
    memset(c, 0, sizeof *c);
}

// The descriptor, its field array, its names, and its defaults, in memory
// of the engine's, so the world's descriptor never points into the module.
static NphResult copy_component(NphAllocator *alloc, const NphComponentDesc *src,
                                GameComponentCopy *out)
{
    memset(out, 0, sizeof *out);
    if (src->align > COPY_DEFAULTS_ALIGN) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    out->desc = *src;
    out->desc.fields = NULL;
    out->desc.defaults = NULL;
    out->text_bytes = strlen(src->name) + 1U;
    for (u32 i = 0; i < src->field_count; ++i) {
        out->text_bytes += strlen(src->fields[i].name) + 1U;
    }
    out->text = NPH_ALLOC_ARRAY(alloc, char, out->text_bytes);
    if (src->field_count > 0U) {
        out->fields = NPH_ALLOC_ARRAY(alloc, NphFieldDesc, src->field_count);
    }
    if (src->defaults != NULL) {
        out->defaults = (u8 *)NPH_ALLOC(alloc, src->size, COPY_DEFAULTS_ALIGN);
    }
    if (out->text == NULL || (src->field_count > 0U && out->fields == NULL) ||
        (src->defaults != NULL && out->defaults == NULL)) {
        free_component(alloc, out);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    char *text = out->text;
    const u64 name_len = strlen(src->name) + 1U;
    memcpy(text, src->name, (size_t)name_len);
    out->desc.name = text;
    text += name_len;
    for (u32 i = 0; i < src->field_count; ++i) {
        out->fields[i] = src->fields[i];
        const u64 len = strlen(src->fields[i].name) + 1U;
        memcpy(text, src->fields[i].name, (size_t)len);
        out->fields[i].name = text;
        text += len;
    }
    out->desc.fields = out->fields;
    if (src->defaults != NULL) { // out->defaults was allocated for it above
        memcpy(out->defaults, src->defaults, src->size);
        out->desc.defaults = out->defaults;
    }
    out->layout_hash = nph_component_layout_hash(&out->desc);
    return NPH_RESULT_OK;
}

static void free_set(NphAllocator *alloc, GameComponentSet *set)
{
    for (u32 i = 0; i < set->count; ++i) {
        free_component(alloc, &set->items[i]);
    }
    if (set->items != NULL) {
        NPH_FREE_ARRAY(alloc, GameComponentCopy, set->items, set->count);
    }
    memset(set, 0, sizeof *set);
}

static const GameComponentCopy *set_find(const GameComponentSet *set, const char *name)
{
    for (u32 i = 0; i < set->count; ++i) {
        if (strcmp(set->items[i].desc.name, name) == 0) {
            return &set->items[i];
        }
    }
    return NULL;
}

static NphResult copy_set(NphAllocator *alloc, const NphGameApi *api, GameComponentSet *out)
{
    memset(out, 0, sizeof *out);
    if (api->component_count == 0U) {
        return NPH_RESULT_OK;
    }
    out->items = NPH_ALLOC_ARRAY_ZEROED(alloc, GameComponentCopy, api->component_count);
    if (out->items == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    out->count = api->component_count;
    for (u32 i = 0; i < api->component_count; ++i) {
        const NphResult r = copy_component(alloc, api->components[i], &out->items[i]);
        if (r != NPH_RESULT_OK) {
            free_set(alloc, out);
            return r;
        }
    }
    return NPH_RESULT_OK;
}

// True when the world registered with `old_set` can keep running the new
// build: every old component is still defined with the same layout. (New
// components are registered next to them.)
static bool layouts_compatible(const GameComponentSet *old_set, const GameComponentSet *new_set)
{
    for (u32 i = 0; i < old_set->count; ++i) {
        const GameComponentCopy *n = set_find(new_set, old_set->items[i].desc.name);
        if (n == NULL || n->layout_hash != old_set->items[i].layout_hash) {
            return false;
        }
    }
    return true;
}

// Takes what a new build changed without changing the layout (flags, field
// hints, defaults) into the copy the world points at.
static void refresh_component(GameComponentCopy *dst, const GameComponentCopy *src)
{
    NPH_ASSERT(dst->layout_hash == src->layout_hash && dst->desc.size == src->desc.size &&
               dst->desc.field_count == src->desc.field_count);
    dst->desc.flags = src->desc.flags;
    for (u32 i = 0; i < src->desc.field_count; ++i) {
        dst->fields[i].flags = src->fields[i].flags;
        dst->fields[i].min = src->fields[i].min;
        dst->fields[i].max = src->fields[i].max;
    }
    if (dst->defaults != NULL && src->defaults != NULL) {
        memcpy(dst->defaults, src->defaults, dst->desc.size);
    }
}

// ---------------------------------------------------------------------------
// Validating a build
// ---------------------------------------------------------------------------

static bool is_pow2(u64 v)
{
    return v != 0U && (v & (v - 1U)) == 0U;
}

static NphResult check_api(NphEngine *e, const NphGameApi *api)
{
    if (api == NULL) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the game module's entry returned no API");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (api->abi_version != NPH_GAME_ABI_VERSION) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the game module was built for game ABI %u, not %u",
                      api->abi_version, NPH_GAME_ABI_VERSION);
        return NPH_RESULT_VERSION_MISMATCH;
    }
    if (api->name == NULL || api->name[0] == '\0') {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the game module has no name");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 align = api->state_align != 0U ? api->state_align : 16U;
    if (!is_pow2(align) || align > STATE_MAX_ALIGN || api->state_size > STATE_MAX_SIZE) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "game %s: state of %llu bytes aligned to %llu", api->name,
                      (unsigned long long)api->state_size, (unsigned long long)align);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (api->component_count > 0U && api->components == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const GameComponentSet *current = &e->game.components;
    for (u32 i = 0; i < api->component_count; ++i) {
        const NphComponentDesc *d = api->components[i];
        const char *reason = NULL;
        if (d == NULL || nph_component_desc_validate(d, &reason) != NPH_RESULT_OK ||
            d->align > COPY_DEFAULTS_ALIGN) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "game %s: component %u is invalid: %s", api->name, i,
                          reason != NULL ? reason : "alignment above 16");
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        for (u32 j = 0; j < i; ++j) {
            if (strcmp(api->components[j]->name, d->name) == 0) {
                NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "game %s: component %s is listed twice", api->name,
                              d->name);
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
        // A name the engine's components use is not the game's to define.
        if (nph_world_find_component(e->edit_world, nph_str_from_cstr(d->name)) !=
                NPH_COMPONENT_NONE &&
            set_find(current, d->name) == NULL) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "game %s: component %s is the engine's", api->name,
                          d->name);
            return NPH_RESULT_ALREADY_EXISTS;
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// What the module registered
// ---------------------------------------------------------------------------

NphResult nph_game_register_cvar(NphGame *game, NphCvar *cvar)
{
    NPH_ASSERT(game != NULL && cvar != NULL);
    NphEngine *e = game->engine;
    NPH_TRY(NPH_ARRAY_RESERVE(&game->cvars, game->cvars.len + 1U));
    NPH_TRY(nph_cvar_register(&e->cvars, cvar));
    NPH_VERIFY(NPH_ARRAY_PUSH(&game->cvars, (void *)cvar) == NPH_RESULT_OK);
    return NPH_RESULT_OK;
}

NphResult nph_game_register_command(NphGame *game, const char *name, const char *help,
                                    NphConsoleCommandFn fn, void *user)
{
    NPH_ASSERT(game != NULL && name != NULL);
    if (game->command_count == GAME_MAX_COMMANDS || strlen(name) >= sizeof game->commands[0]) {
        return NPH_RESULT_FULL;
    }
    NPH_TRY(nph_console_register(game->engine->console, name, help, fn, user));
    memcpy(game->commands[game->command_count++], name, strlen(name) + 1U);
    return NPH_RESULT_OK;
}

static void unregister_all(NphGame *g)
{
    NphEngine *e = g->engine;
    for (u32 i = 0; i < g->cvars.len; ++i) {
        NPH_IGNORE_RESULT(nph_cvar_unregister(&e->cvars, (NphCvar *)g->cvars.data[i]));
    }
    NPH_ARRAY_CLEAR(&g->cvars);
    for (u32 i = 0; i < g->command_count; ++i) {
        NPH_IGNORE_RESULT(nph_console_unregister(e->console, g->commands[i]));
    }
    g->command_count = 0U;
}

// ---------------------------------------------------------------------------
// Unloading images safely
// ---------------------------------------------------------------------------

// Whether anything the engine still holds points into `lib`'s image: a
// registered component descriptor, a cvar, or a console command's name.
// The engine copies or removes all of these before unloading; a hit means
// a game kept something registered behind the engine's back, and the image
// then stays loaded rather than leave a dangling pointer.
static const char *referenced_by(NphEngine *e, const NphDylib *lib)
{
    NphWorld *worlds[2] = {e->edit_world, e->play_world};
    for (u32 w = 0; w < 2U; ++w) {
        if (worlds[w] == NULL) {
            continue;
        }
        for (u32 id = 0; id < nph_world_component_count(worlds[w]); ++id) {
            const NphComponentDesc *d = nph_world_component_desc(worlds[w], id);
            if (d != NULL &&
                (nph_dylib_contains(lib, d) || nph_dylib_contains(lib, d->name) ||
                 nph_dylib_contains(lib, d->fields) || nph_dylib_contains(lib, d->defaults))) {
                return "a component descriptor";
            }
        }
    }
    for (u32 i = 0; i < nph_cvar_registry_count(&e->cvars); ++i) {
        const NphCvar *c = nph_cvar_registry_at(&e->cvars, i);
        if (nph_dylib_contains(lib, c) || nph_dylib_contains(lib, c->name)) {
            return "a cvar";
        }
    }
    for (u32 i = 0; i < nph_console_command_count(e->console); ++i) {
        if (nph_dylib_contains(lib, nph_console_command_name(e->console, i))) {
            return "a console command";
        }
    }
    return NULL;
}

static void unload_image(NphEngine *e, NphDylib *lib, const char *path)
{
    nph_dylib_close(lib);
    if (path[0] != '\0' && nph_fs_remove_file(path) != NPH_RESULT_OK) {
        NPH_LOG_DEBUG(&NPH_LOG_RUNTIME, "cannot remove %s", path);
    }
    NPH_UNUSED(e);
}

// Unloads an image that was replaced, or keeps it when something still
// points into it. Log records and profiler events name things with
// pointers into the image (categories, call sites, zone names): both are
// drained first.
static void retire_image(NphEngine *e, NphDylib lib, const char *path)
{
    if (!nph_dylib_is_open(&lib)) {
        return;
    }
    (void)nph_log_drain();
    if (e->profiler != NULL) {
        nph_profiler_forget_names(e->profiler);
    }
    const char *what = referenced_by(e, &lib);
    if (what == NULL) {
        unload_image(e, &lib, path);
        return;
    }
    NphGame *g = &e->game;
    NPH_LOG_WARN(&NPH_LOG_RUNTIME, "%s still points into the replaced build %s; kept loaded", what,
                 path);
    if (g->retired_count == GAME_MAX_RETIRED) {
        // No room to remember it: leak the handle (the image stays mapped
        // for the process's lifetime) rather than unload it.
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "too many replaced builds kept loaded");
        return;
    }
    GameImage *img = &g->retired[g->retired_count++];
    img->lib = lib;
    (void)snprintf(img->path, sizeof img->path, "%s", path);
}

// ---------------------------------------------------------------------------
// The state block
// ---------------------------------------------------------------------------

// Keeps the block when the new build's state has the same version, size,
// and alignment; otherwise replaces it with a zeroed one.
static NphResult prepare_state(NphEngine *e, const NphGameApi *api, bool *out_reset)
{
    NphGame *g = &e->game;
    NphAllocator *alloc = engine_alloc(e, ENGINE_ALLOC_GAME);
    const u64 align = api->state_align != 0U ? api->state_align : 16U;
    if (g->state != NULL && g->state_size == api->state_size && g->state_align == align &&
        g->state_version == api->state_version) {
        *out_reset = false;
        return NPH_RESULT_OK;
    }
    void *state = NULL;
    if (api->state_size > 0U) {
        state = NPH_ALLOC_ZEROED(alloc, api->state_size, (u32)align);
        if (state == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    if (g->state != NULL) {
        NPH_FREE(alloc, g->state, g->state_size, (u32)g->state_align);
    }
    g->state = state;
    g->state_size = api->state_size;
    g->state_align = align;
    g->state_version = api->state_version;
    *out_reset = true;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Loading and swapping
// ---------------------------------------------------------------------------

static void update_ids(NphEngine *e)
{
    NphGame *g = &e->game;
    for (u32 i = 0; i < NPH_ECS_MAX_COMPONENTS; ++i) {
        g->ids[i] = NPH_COMPONENT_NONE;
    }
    if (g->api == NULL) {
        return;
    }
    for (u32 i = 0; i < g->api->component_count; ++i) {
        g->ids[i] =
            nph_world_find_component(e->edit_world, nph_str_from_cstr(g->api->components[i]->name));
    }
}

// Registers the components of `set` that `world` lacks (a build that added
// components), and points the ones it has at the refreshed copies.
static NphResult register_new_components(NphWorld *world, const GameComponentSet *set)
{
    for (u32 i = 0; i < set->count; ++i) {
        NphComponentId id;
        NPH_TRY(nph_world_register_component(world, &set->items[i].desc, &id));
    }
    return NPH_RESULT_OK;
}

// Merges a compatible new set into the current one: components the world
// knows keep their copies (refreshed), new ones are adopted. `incoming` is
// emptied (what it held is moved or freed).
static NphResult merge_sets(NphAllocator *alloc, GameComponentSet *current,
                            GameComponentSet *incoming)
{
    u32 added = 0U;
    for (u32 i = 0; i < incoming->count; ++i) {
        added += set_find(current, incoming->items[i].desc.name) == NULL ? 1U : 0U;
    }
    GameComponentCopy *items = current->items;
    if (added > 0U) {
        items = NPH_ALLOC_ARRAY_ZEROED(alloc, GameComponentCopy, current->count + added);
        if (items == NULL) {
            free_set(alloc, incoming);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        if (current->count > 0U) {
            memcpy(items, current->items, sizeof(GameComponentCopy) * current->count);
        }
    }
    u32 n = current->count;
    for (u32 i = 0; i < incoming->count; ++i) {
        GameComponentCopy *have = NULL;
        for (u32 k = 0; k < current->count; ++k) {
            if (strcmp(items[k].desc.name, incoming->items[i].desc.name) == 0) {
                have = &items[k];
            }
        }
        if (have != NULL) {
            refresh_component(have, &incoming->items[i]);
            free_component(alloc, &incoming->items[i]);
        } else {
            items[n++] = incoming->items[i]; // moved
            memset(&incoming->items[i], 0, sizeof incoming->items[i]);
        }
    }
    if (added > 0U && current->items != NULL) {
        NPH_FREE_ARRAY(alloc, GameComponentCopy, current->items, current->count);
    }
    current->items = items;
    current->count = n;
    free_set(alloc, incoming);
    return NPH_RESULT_OK;
}

// Opens the build at `source` (through a temporary copy) and checks it.
static NphResult open_build(NphEngine *e, const char *source, NphDylib *out_lib, char *out_copy,
                            const NphGameApi **out_api, u64 *out_hash)
{
    NPH_TRY(copy_build(e, source, out_copy, GAME_PATH_MAX, out_hash));
    NphResult r = nph_dylib_open(out_copy, out_lib);
    NphDylibFn fn = NULL;
    if (r == NPH_RESULT_OK) {
        r = nph_dylib_function(out_lib, NPH_GAME_ENTRY_NAME, &fn);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s exports no %s", source, NPH_GAME_ENTRY_NAME);
        }
    }
    if (r == NPH_RESULT_OK) {
        const NphGameEntryFn entry = (NphGameEntryFn)fn;
        *out_api = entry();
        r = check_api(e, *out_api);
    }
    if (r != NPH_RESULT_OK) {
        unload_image(e, out_lib, out_copy);
        out_copy[0] = '\0';
    }
    return r;
}

// The swap (game.h, steps 1 to 4). `source` is the build's file;
// `reloading` when it replaces a build of the same file.
static NphResult swap_build(NphEngine *e, const char *source, bool reloading)
{
    NphGame *g = &e->game;
    NphAllocator *alloc = engine_alloc(e, ENGINE_ALLOC_GAME);
    NPH_PROFILE_BEGIN("runtime.game_swap");
    NphDylib lib;
    memset(&lib, 0, sizeof lib);
    char copy[GAME_PATH_MAX];
    copy[0] = '\0';
    const NphGameApi *api = NULL;
    u64 hash = 0U;
    NphResult r = open_build(e, source, &lib, copy, &api, &hash);
    GameComponentSet incoming;
    memset(&incoming, 0, sizeof incoming);
    if (r == NPH_RESULT_OK) {
        r = copy_set(alloc, api, &incoming);
    }
    // A build whose layouts differ needs the world rebuilt; built before
    // anything changes, so a failure leaves the running build in place.
    const bool migrate = r == NPH_RESULT_OK && !layouts_compatible(&g->components, &incoming);
    NphWorld *migrated = NULL;
    if (migrate) {
        if (e->play_world != NULL) {
            NPH_LOG_INFO(&NPH_LOG_RUNTIME, "the game's components changed: stopping play");
            nph_engine_play_end(e);
        }
        r = engine_world_copy(e, e->edit_world, &incoming, &migrated);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME,
                          "%s: the world cannot move to the new components (%s); keeping the "
                          "running build",
                          source, nph_result_string(r));
        }
    }
    if (r != NPH_RESULT_OK) {
        free_set(alloc, &incoming);
        unload_image(e, &lib, copy);
        NPH_PROFILE_END();
        return r;
    }

    // From here on the new build replaces the old one. A reload keeps the
    // world playing through the swap: the new build joins the play without
    // a play_begin.
    const NphGameApi *old_api = g->api;
    if (old_api != NULL) {
        if (!reloading && g->playing) {
            game_play_end(e);
        }
        if (old_api->unload != NULL) {
            old_api->unload(g, reloading ? 1 : 0);
        }
    }
    unregister_all(g);
    g->api = NULL; // no game systems until the new build is in
    NphDylib old_lib = g->lib;
    char old_copy[GAME_PATH_MAX];
    memcpy(old_copy, g->loaded, sizeof old_copy);

    NphGameLoadInfo info;
    memset(&info, 0, sizeof info);
    info.reloaded = reloading ? 1 : 0;
    if (migrate) {
        // The new world holds the incoming copies; the old world, and with
        // it the old copies, go.
        NphWorld *previous = NULL;
        r = engine_world_activate(e, migrated, true, &previous);
        NPH_ASSERT(r == NPH_RESULT_OK); // rebinding an empty-or-copied world cannot fail on lookups
        nph_world_destroy(previous);
        free_set(alloc, &g->components);
        g->components = incoming;
        info.world_migrated = 1;
    } else {
        r = merge_sets(alloc, &g->components, &incoming);
        if (r == NPH_RESULT_OK) {
            r = register_new_components(e->edit_world, &g->components);
        }
        if (r == NPH_RESULT_OK && e->play_world != NULL) {
            r = register_new_components(e->play_world, &g->components);
        }
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "%s: registering its components failed (%s)", source,
                          nph_result_string(r));
        }
    }
    g->api = api;
    g->lib = lib;
    memcpy(g->loaded, copy, sizeof g->loaded);
    if (!reloading) {
        (void)snprintf(g->source, sizeof g->source, "%s", source);
    }
    NPH_IGNORE_RESULT(source_info(g->source, &g->source_modified_ns, &g->source_size));
    g->source_hash = hash;
    g->changed_at_ns = 0U;
    update_ids(e);
    bool reset = false;
    NphResult sr = prepare_state(e, api, &reset);
    info.state_reset = reset ? 1 : 0;
    if (r == NPH_RESULT_OK) {
        r = sr;
    }
    if (r == NPH_RESULT_OK && api->load != NULL) {
        r = api->load(g, &info);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "game %s: load failed (%s)", api->name,
                          nph_result_string(r));
        }
    }
    // The schedule takes the new build's systems (none when its load failed:
    // the next build gets another chance).
    if (r != NPH_RESULT_OK) {
        g->api = NULL;
    }
    const NphResult br = engine_schedule_rebuild(e);
    r = r != NPH_RESULT_OK ? r : br;
    // A build loaded into a world that is playing joins the play (a reload
    // already has: the world played through the swap).
    if (r == NPH_RESULT_OK && e->playing && !g->playing) {
        r = game_play_begin(e);
    }
    if (reloading) {
        g->reloads += 1U;
    }
    retire_image(e, old_lib, old_copy);
    NPH_LOG_INFO(&NPH_LOG_RUNTIME, "game %s %s%s", api->name, reloading ? "reloaded" : "loaded",
                 migrate ? " (world moved to new components)" : "");
    NPH_PROFILE_END();
    return r;
}

NphResult game_load(NphEngine *e, const char *path)
{
    NPH_ASSERT(e != NULL && path != NULL);
    NphGame *g = &e->game;
    const bool reloading = g->api != NULL && strcmp(g->source, path) == 0;
    if (!reloading && g->api != NULL) {
        game_unload(e);
    }
    return swap_build(e, path, reloading);
}

void game_unload(NphEngine *e)
{
    NphGame *g = &e->game;
    if (g->api == NULL) {
        if (nph_dylib_is_open(&g->lib)) {
            NphDylib lib = g->lib;
            memset(&g->lib, 0, sizeof g->lib);
            retire_image(e, lib, g->loaded);
            g->loaded[0] = '\0';
        }
        return;
    }
    game_play_end(e);
    if (g->api->unload != NULL) {
        g->api->unload(g, 0);
    }
    unregister_all(g);
    g->api = NULL;
    update_ids(e);
    NPH_IGNORE_RESULT(engine_schedule_rebuild(e));
    NphDylib lib = g->lib;
    memset(&g->lib, 0, sizeof g->lib);
    retire_image(e, lib, g->loaded);
    g->loaded[0] = '\0';
    g->source[0] = '\0';
}

NphResult game_poll_reload(NphEngine *e, bool force, bool *out_reloaded)
{
    NphGame *g = &e->game;
    *out_reloaded = false;
    if (g->source[0] == '\0') {
        return NPH_RESULT_OK;
    }
    const u64 now = nph_time_now_ns();
    if (!force && now < g->next_poll_ns) {
        return NPH_RESULT_OK;
    }
    g->next_poll_ns = now + nph_time_ms_to_ns((u64)nph_cvar_get_int(&e->cv.reload_poll_ms));
    u64 modified = 0U;
    u64 size = 0U;
    if (source_info(g->source, &modified, &size) != NPH_RESULT_OK) {
        return NPH_RESULT_OK; // being rewritten: look again later
    }
    if (modified == g->source_modified_ns && size == g->source_size) {
        g->changed_at_ns = 0U;
        // A forced check also compares the content: a build written within
        // the file system's time resolution, at the same size, looks
        // unchanged otherwise.
        if (!force || content_hash(e, g->source) == g->source_hash) {
            return NPH_RESULT_OK;
        }
    }
    // Changed: wait until it has stopped changing (forced checks too, so
    // a half-written file is never loaded).
    if (g->changed_at_ns == 0U || modified != g->source_modified_ns) {
        g->changed_at_ns = now;
        g->source_modified_ns = modified;
        g->source_size = size;
        if (!force) {
            return NPH_RESULT_OK;
        }
    }
    if (!force && now - g->changed_at_ns < SETTLE_NS) {
        return NPH_RESULT_OK;
    }
    const NphResult r = swap_build(e, g->source, g->api != NULL);
    if (r != NPH_RESULT_OK) {
        // Keep the running build; try again when the file changes again.
        NPH_IGNORE_RESULT(source_info(g->source, &g->source_modified_ns, &g->source_size));
        g->changed_at_ns = 0U;
        return r;
    }
    *out_reloaded = true;
    return NPH_RESULT_OK;
}

NphResult game_add_systems(NphEngine *e, NphSchedule *schedule)
{
    NphGame *g = &e->game;
    if (g->api == NULL || g->api->add_systems == NULL) {
        return NPH_RESULT_OK;
    }
    const NphResult r = g->api->add_systems(g, schedule);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "game %s: add_systems failed (%s)", g->api->name,
                      nph_result_string(r));
    }
    return r;
}

void game_frame(NphEngine *e, f64 dt)
{
    NphGame *g = &e->game;
    if (g->api != NULL && g->api->frame != NULL) {
        g->api->frame(g, dt);
    }
}

NphResult game_play_begin(NphEngine *e)
{
    NphGame *g = &e->game;
    g->step = 0U;
    if (g->api == NULL || g->playing) {
        return NPH_RESULT_OK;
    }
    if (g->api->play_begin != NULL) {
        NPH_TRY(g->api->play_begin(g));
    }
    g->playing = 1;
    return NPH_RESULT_OK;
}

void game_play_end(NphEngine *e)
{
    NphGame *g = &e->game;
    if (g->api == NULL || !g->playing) {
        g->playing = 0;
        return;
    }
    if (g->api->play_end != NULL) {
        g->api->play_end(g);
    }
    g->playing = 0;
}

void game_free(NphEngine *e)
{
    NphGame *g = &e->game;
    NphAllocator *alloc = engine_alloc(e, ENGINE_ALLOC_GAME);
    // The worlds are gone: nothing refers to the copies or the images now.
    (void)nph_log_drain();
    if (e->profiler != NULL) {
        nph_profiler_forget_names(e->profiler);
    }
    if (nph_dylib_is_open(&g->lib)) {
        unload_image(e, &g->lib, g->loaded);
    }
    for (u32 i = 0; i < g->retired_count; ++i) {
        unload_image(e, &g->retired[i].lib, g->retired[i].path);
    }
    g->retired_count = 0U;
    free_set(alloc, &g->components);
    if (g->state != NULL) {
        NPH_FREE(alloc, g->state, g->state_size, (u32)g->state_align);
        g->state = NULL;
    }
    NPH_ARRAY_FREE(&g->cvars);
    char dir[GAME_PATH_MAX];
    if (g->copies > 0U && copies_dir(dir, sizeof dir) == NPH_RESULT_OK) {
        NPH_IGNORE_RESULT(nph_fs_remove_directory(dir)); // only when empty
    }
}

// ---------------------------------------------------------------------------
// Services for game code
// ---------------------------------------------------------------------------

NphEngine *nph_game_engine(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return game->engine;
}

NphWorld *nph_game_world(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return game->engine->world;
}

void *nph_game_state(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return game->state;
}

NphComponentId nph_game_component(const NphGame *game, u32 index)
{
    NPH_ASSERT(game != NULL);
    if (game->api == NULL || index >= game->api->component_count) {
        return NPH_COMPONENT_NONE;
    }
    return game->ids[index];
}

const NphSimInput *nph_game_input(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return game->input != NULL ? game->input : &game->idle_input;
}

f64 nph_game_step_seconds(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return nph_fixed_clock_step_seconds(&game->engine->clock);
}

u64 nph_game_step(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return game->step;
}

NphPhysicsWorld *nph_game_physics(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return nph_engine_physics(game->engine);
}

NphPhysicsBody nph_game_physics_body(const NphGame *game, NphEntity entity)
{
    NPH_ASSERT(game != NULL);
    return nph_engine_physics_body(game->engine, entity);
}

NphEntity nph_game_physics_entity(const NphGame *game, NphPhysicsBody body)
{
    NPH_ASSERT(game != NULL);
    return nph_engine_physics_entity(game->engine, body);
}

NphResult nph_game_play_sound(NphGame *game, NphGuid sound, const NphSoundPlayDesc *desc)
{
    NPH_ASSERT(game != NULL);
    return nph_engine_play_sound(game->engine, sound, desc);
}

NphResult nph_game_preload_sound(NphGame *game, NphGuid sound)
{
    NPH_ASSERT(game != NULL);
    return nph_engine_preload_sound(game->engine, sound);
}

NphAudioMixer *nph_game_audio_mixer(const NphGame *game)
{
    NPH_ASSERT(game != NULL);
    return nph_engine_audio_mixer(game->engine);
}

void nph_game_reset_interpolation(NphGame *game, NphEntity entity)
{
    NPH_ASSERT(game != NULL);
    engine_reset_interpolation(game->engine, entity);
}

NphResult nph_game_action(NphGame *game, const char *name, NphActionId *out)
{
    NPH_ASSERT(game != NULL && name != NULL && out != NULL);
    NphInput *input = game->engine->input;
    if (nph_input_action_find(input, name, out) == NPH_RESULT_OK) {
        return NPH_RESULT_OK;
    }
    if (nph_input_action_count(input) >= NPH_SIM_INPUT_MAX_ACTIONS) {
        return NPH_RESULT_FULL;
    }
    return nph_input_action_register(input, name, out);
}
