// SPDX-License-Identifier: Apache-2.0
// core_hello: exercises the core module end to end (CLAUDE.md §9 "Sample
// app demonstrating the feature"). Run with cvar assignments as arguments:
//
//     core_hello r.vsync=false game.player_name=Ada
//
// It logs through the console sink, allocates from a TLSF heap wrapped in
// the debug allocator, registers cvars and applies the command line, builds
// a small scene as JSON and as a checksummed binary document, reads both
// back, and exits non-zero if anything leaked.

#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/array.h>
#include <nepheline/core/cvar.h>
#include <nepheline/core/handle.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/json.h>
#include <nepheline/core/log.h>
#include <nepheline/core/serialize.h>
#include <nepheline/core/string_id.h>
#include <nepheline/core/tlsf.h>
#include <nepheline/core/version.h>

#include <stdio.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "sample", NPH_LOG_LEVEL_TRACE);

static NPH_CVAR_BOOL(cv_vsync, "r.vsync", 1, "Vertical sync", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_INT(cv_entities, "sample.entities", 8, 1, 1000, "Entities to create",
                    NPH_CVAR_FLAG_NONE);
static NPH_CVAR_STRING(cv_player, "game.player_name", "Player", "Player name", NPH_CVAR_FLAG_SAVED);

#define SCENE_MAGIC      0x4E534345U // "NSCE"
#define SCENE_VERSION    1U
#define SECTION_ENTITIES 1U

typedef struct Entity {
    NphHandle handle;
    NphStrId name;
    f32 x, y, z;
} Entity;

typedef NPH_ARRAY(Entity) EntityArray;

static void on_leak(const NphDebugAllocInfo *info, void *user)
{
    NPH_UNUSED(user);
    NPH_LOG_ERROR(&LOG_SAMPLE, "leak: %llu bytes at %s:%u", (unsigned long long)info->size,
                  info->site.file, info->site.line);
}

static NphResult build_scene(NphAllocator *a, const NphCvarRegistry *cvars, EntityArray *entities,
                             NphHandlePool *handles, NphStrTable *names)
{
    NPH_UNUSED(cvars);
    const i64 count = nph_cvar_get_int(&cv_entities);
    for (i64 i = 0; i < count; ++i) {
        Entity *e = NPH_ARRAY_PUSH_SLOT(entities);
        if (e == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        NPH_TRY(nph_handle_pool_alloc(handles, &e->handle));
        char name[32];
        snprintf(name, sizeof name, "entity_%lld", (long long)i);
        NPH_TRY(nph_str_table_intern(names, nph_str_from_cstr(name), &e->name));
        e->x = (f32)i;
        e->y = (f32)i * 0.5F;
        e->z = 0.0F;
    }
    NPH_UNUSED(a);
    return NPH_RESULT_OK;
}

static NphResult scene_to_json(const EntityArray *entities, const NphStrTable *names,
                               NphStrBuilder *out)
{
    NphJsonWriter w;
    nph_json_writer_init(&w, out, true);
    NPH_TRY(nph_json_writer_begin_object(&w));
    NPH_TRY(nph_json_writer_key(&w, NPH_STR("player")));
    NPH_TRY(nph_json_writer_str(&w, nph_cvar_get_string(&cv_player)));
    NPH_TRY(nph_json_writer_key(&w, NPH_STR("entities")));
    NPH_TRY(nph_json_writer_begin_array(&w));
    NPH_ARRAY_FOREACH (const Entity, e, entities) {
        NPH_TRY(nph_json_writer_begin_object(&w));
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("name")));
        NPH_TRY(nph_json_writer_str(&w, nph_str_table_lookup(names, e->name)));
        NPH_TRY(nph_json_writer_key(&w, NPH_STR("position")));
        NPH_TRY(nph_json_writer_begin_array(&w));
        NPH_TRY(nph_json_writer_f64(&w, (f64)e->x));
        NPH_TRY(nph_json_writer_f64(&w, (f64)e->y));
        NPH_TRY(nph_json_writer_f64(&w, (f64)e->z));
        NPH_TRY(nph_json_writer_end_array(&w));
        NPH_TRY(nph_json_writer_end_object(&w));
    }
    NPH_TRY(nph_json_writer_end_array(&w));
    return nph_json_writer_end_object(&w);
}

static NphResult scene_to_binary(const EntityArray *entities, const NphStrTable *names,
                                 NphBinWriter *w)
{
    NPH_TRY(nph_bin_write_header(w, SCENE_MAGIC, SCENE_VERSION));
    NphBinSection section;
    NPH_TRY(nph_bin_write_section_begin(w, SECTION_ENTITIES, &section));
    NPH_TRY(nph_bin_write_u32(w, entities->len));
    NPH_ARRAY_FOREACH (const Entity, e, entities) {
        NPH_TRY(nph_bin_write_str(w, nph_str_table_lookup(names, e->name)));
        NPH_TRY(nph_bin_write_f32(w, e->x));
        NPH_TRY(nph_bin_write_f32(w, e->y));
        NPH_TRY(nph_bin_write_f32(w, e->z));
    }
    nph_bin_write_section_end(w, &section);
    return nph_bin_write_checksum(w);
}

static NphResult read_binary_back(const NphBinWriter *w, u32 *out_entities)
{
    u64 payload_len;
    NPH_TRY(nph_bin_verify_checksum(nph_bin_writer_data(w), nph_bin_writer_len(w), &payload_len));
    NphBinReader r = nph_bin_reader(nph_bin_writer_data(w), payload_len);
    u32 version;
    NPH_TRY(nph_bin_read_header(&r, SCENE_MAGIC, SCENE_VERSION, SCENE_VERSION, &version));
    *out_entities = 0U;
    while (!nph_bin_reader_at_end(&r)) {
        u32 tag;
        u32 size;
        NPH_TRY(nph_bin_read_section(&r, &tag, &size));
        if (tag != SECTION_ENTITIES) {
            NPH_TRY(nph_bin_read_skip(&r, size));
            continue;
        }
        u32 count;
        NPH_TRY(nph_bin_read_u32(&r, &count));
        for (u32 i = 0; i < count; ++i) {
            NphStr name;
            f32 xyz[3];
            NPH_TRY(nph_bin_read_str(&r, &name));
            for (u32 k = 0; k < 3U; ++k) {
                NPH_TRY(nph_bin_read_f32(&r, &xyz[k]));
            }
            NPH_LOG_TRACE(&LOG_SAMPLE, "  %.*s at (%g, %g, %g)", NPH_STR_ARG(name), (f64)xyz[0],
                          (f64)xyz[1], (f64)xyz[2]);
        }
        *out_entities += count;
    }
    return NPH_RESULT_OK;
}

static int run(int argc, char **argv, NphAllocator *a)
{
    NphCvarRegistry cvars;
    if (nph_cvar_registry_init(&cvars, a) != NPH_RESULT_OK ||
        nph_cvar_register(&cvars, &cv_vsync) != NPH_RESULT_OK ||
        nph_cvar_register(&cvars, &cv_entities) != NPH_RESULT_OK ||
        nph_cvar_register(&cvars, &cv_player) != NPH_RESULT_OK) {
        NPH_LOG_FATAL(&LOG_SAMPLE, "cvar registration failed");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        const NphResult r = nph_cvar_apply_assignment(&cvars, nph_str_from_cstr(argv[i]));
        if (r != NPH_RESULT_OK) {
            NPH_LOG_WARN(&LOG_SAMPLE, "ignoring argument \"%s\": %s", argv[i],
                         nph_result_string(r));
        }
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "r.vsync=%s sample.entities=%lld game.player_name=\"%s\"",
                 nph_cvar_get_bool(&cv_vsync) ? "true" : "false",
                 (long long)nph_cvar_get_int(&cv_entities), cv_player.value_string);

    int status = 0;
    EntityArray entities;
    NPH_ARRAY_INIT(&entities, a);
    NphHandlePool handles;
    NphStrTable names;
    NphStrBuilder json;
    NphBinWriter binary;
    nph_sb_init(&json, a);
    nph_bin_writer_init(&binary, a);
    if (nph_handle_pool_init(&handles, a, 1024U, 20U, 16U) != NPH_RESULT_OK ||
        nph_str_table_init(&names, a, 0U) != NPH_RESULT_OK ||
        build_scene(a, &cvars, &entities, &handles, &names) != NPH_RESULT_OK ||
        scene_to_json(&entities, &names, &json) != NPH_RESULT_OK ||
        scene_to_binary(&entities, &names, &binary) != NPH_RESULT_OK) {
        NPH_LOG_FATAL(&LOG_SAMPLE, "building the scene failed");
        status = 1;
    } else {
        NPH_LOG_INFO(&LOG_SAMPLE, "%u entities, %u handles live, %u names interned", entities.len,
                     nph_handle_pool_live(&handles), nph_str_table_count(&names));
        NPH_LOG_INFO(&LOG_SAMPLE, "JSON (%llu bytes):\n%s", (unsigned long long)json.len,
                     nph_sb_cstr(&json));
        NphJsonDoc doc;
        if (nph_json_parse(nph_sb_str(&json), a, &doc) != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "JSON parse failed at %u:%u: %s", doc.error_line,
                          doc.error_column, doc.error_message);
            status = 1;
        } else {
            const NphJsonValue *list = nph_json_object_get(doc.root, NPH_STR("entities"));
            NPH_LOG_INFO(&LOG_SAMPLE, "JSON read back: player \"%.*s\", %u entities",
                         NPH_STR_ARG(nph_json_get_str(doc.root, NPH_STR("player"), NPH_STR("?"))),
                         list != NULL ? list->count : 0U);
        }
        nph_json_doc_free(&doc);
        u32 read_entities = 0U;
        const NphResult r = read_binary_back(&binary, &read_entities);
        NPH_LOG_INFO(&LOG_SAMPLE, "binary document: %llu bytes, checksum %s, %u entities read",
                     (unsigned long long)nph_bin_writer_len(&binary), nph_result_string(r),
                     read_entities);
        if (r != NPH_RESULT_OK || read_entities != entities.len) {
            status = 1;
        }
    }
    NPH_ARRAY_FOREACH (const Entity, e, &entities) {
        (void)nph_handle_pool_release(&handles, e->handle);
    }
    nph_bin_writer_free(&binary);
    nph_sb_free(&json);
    nph_str_table_free(&names);
    nph_handle_pool_free(&handles);
    NPH_ARRAY_FREE(&entities);
    nph_cvar_registry_free(&cvars);
    return status;
}

int main(int argc, char **argv)
{
    NphAllocator *sys = nph_allocator_system();
    const NphLogConfig log_config = {sys, 256U, 4U};
    if (nph_log_init(&log_config) != NPH_RESULT_OK) {
        fputs("log init failed\n", stderr);
        return 1;
    }
    NphLogConsoleSink console = {false, false};
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_TRACE) != NPH_RESULT_OK) {
        return 1;
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "Nepheline core %s", nph_version_string());

    NphTlsf heap;
    if (nph_tlsf_init_from(&heap, sys, NPH_MIB(4)) != NPH_RESULT_OK) {
        NPH_LOG_FATAL(&LOG_SAMPLE, "heap init failed");
        nph_log_shutdown();
        return 1;
    }
    NphDebugAllocator debug;
    nph_debug_allocator_init(&debug, nph_tlsf_allocator(&heap));

    int status = run(argc, argv, nph_debug_allocator_allocator(&debug));

    NphDebugAllocStats stats;
    nph_debug_allocator_get_stats(&debug, &stats);
    NPH_LOG_INFO(&LOG_SAMPLE, "allocations: %llu made, peak %llu bytes live",
                 (unsigned long long)stats.total_allocations,
                 (unsigned long long)stats.peak_live_bytes);
    const u32 leaks = nph_debug_allocator_destroy(&debug, on_leak, NULL);
    if (leaks != 0U) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "%u allocations leaked", leaks);
        status = 1;
    }
    NphTlsfStats heap_stats;
    nph_tlsf_get_stats(&heap, &heap_stats);
    NPH_LOG_INFO(&LOG_SAMPLE, "heap: %u live blocks, %llu bytes peak", heap_stats.live_count,
                 (unsigned long long)heap_stats.peak_live_bytes);
    nph_tlsf_destroy(&heap);
    NPH_LOG_INFO(&LOG_SAMPLE, status == 0 ? "done" : "failed");
    nph_log_shutdown();
    return status;
}
