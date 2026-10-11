// SPDX-License-Identifier: Apache-2.0
// Serialization benchmarks (docs/core.md).
//
//   json_parse_scene     parse a ~1.2 MB document of 20 000 entity objects
//   json_write_scene     write the same tree compactly
//   json_write_streaming emit the same text with the streaming writer
//   bin_write_scene      write the entities as a binary document with checksum
//   bin_read_scene       verify the checksum and read every entity back
//   cvar_apply_config    apply a 4-line config to a registry
// Throughput in bytes per op is derivable from the document sizes logged
// in the labels.

#include <nepheline/core/cvar.h>
#include <nepheline/core/json.h>
#include <nepheline/core/serialize.h>

#include <nbench/nbench.h>

#include <stdio.h>
#include <stdlib.h>

#define ENTITY_COUNT 20000U

static NphStrBuilder g_scene_json;
static bool g_scene_ready;

static void write_scene_streaming(NphStrBuilder *out)
{
    NphJsonWriter w;
    nph_json_writer_init(&w, out, false);
    if (nph_json_writer_begin_array(&w) != NPH_RESULT_OK) {
        abort();
    }
    for (u32 i = 0; i < ENTITY_COUNT; ++i) {
        if (nph_json_writer_begin_object(&w) != NPH_RESULT_OK ||
            nph_json_writer_key(&w, NPH_STR("id")) != NPH_RESULT_OK ||
            nph_json_writer_i64(&w, i) != NPH_RESULT_OK ||
            nph_json_writer_key(&w, NPH_STR("name")) != NPH_RESULT_OK ||
            nph_json_writer_str(&w, NPH_STR("entity \"quoted\" name")) != NPH_RESULT_OK ||
            nph_json_writer_key(&w, NPH_STR("position")) != NPH_RESULT_OK ||
            nph_json_writer_begin_array(&w) != NPH_RESULT_OK ||
            nph_json_writer_f64(&w, (f64)i * 0.25) != NPH_RESULT_OK ||
            nph_json_writer_f64(&w, 1.5) != NPH_RESULT_OK ||
            nph_json_writer_f64(&w, -2.0) != NPH_RESULT_OK ||
            nph_json_writer_end_array(&w) != NPH_RESULT_OK ||
            nph_json_writer_key(&w, NPH_STR("visible")) != NPH_RESULT_OK ||
            nph_json_writer_bool(&w, (i & 1U) != 0U) != NPH_RESULT_OK ||
            nph_json_writer_end_object(&w) != NPH_RESULT_OK) {
            abort();
        }
    }
    if (nph_json_writer_end_array(&w) != NPH_RESULT_OK) {
        abort();
    }
}

static void prepare_scene(void)
{
    if (g_scene_ready) {
        return;
    }
    nph_sb_init(&g_scene_json, nph_allocator_system());
    write_scene_streaming(&g_scene_json);
    g_scene_ready = true;
}

NBENCH(json_parse_scene)
{
    prepare_scene();
    static char label[64];
    snprintf(label, sizeof label, "%llu bytes", (unsigned long long)g_scene_json.len);
    nbench_set_label(state, label);
    while (nbench_keep_running(state)) {
        NphJsonDoc doc;
        if (nph_json_parse(nph_sb_str(&g_scene_json), nph_allocator_system(), &doc) !=
            NPH_RESULT_OK) {
            abort();
        }
        nbench_do_not_optimize(doc.root);
        nph_json_doc_free(&doc);
    }
}

NBENCH(json_write_scene)
{
    prepare_scene();
    NphJsonDoc doc;
    if (nph_json_parse(nph_sb_str(&g_scene_json), nph_allocator_system(), &doc) != NPH_RESULT_OK) {
        abort();
    }
    NphStrBuilder out;
    nph_sb_init(&out, nph_allocator_system());
    while (nbench_keep_running(state)) {
        nph_sb_clear(&out);
        if (nph_json_write(doc.root, &out, false) != NPH_RESULT_OK) {
            abort();
        }
    }
    nbench_do_not_optimize(out.data);
    nph_sb_free(&out);
    nph_json_doc_free(&doc);
}

NBENCH(json_write_streaming)
{
    NphStrBuilder out;
    nph_sb_init(&out, nph_allocator_system());
    while (nbench_keep_running(state)) {
        nph_sb_clear(&out);
        write_scene_streaming(&out);
    }
    nbench_do_not_optimize(out.data);
    nph_sb_free(&out);
}

static void write_scene_binary(NphBinWriter *w)
{
    nph_bin_writer_clear(w);
    NphBinSection section;
    if (nph_bin_write_header(w, 0x4E534345U, 1U) != NPH_RESULT_OK ||
        nph_bin_write_section_begin(w, 1U, &section) != NPH_RESULT_OK) {
        abort();
    }
    for (u32 i = 0; i < ENTITY_COUNT; ++i) {
        if (nph_bin_write_u32(w, i) != NPH_RESULT_OK ||
            nph_bin_write_str(w, NPH_STR("entity \"quoted\" name")) != NPH_RESULT_OK ||
            nph_bin_write_f32(w, (f32)i * 0.25F) != NPH_RESULT_OK ||
            nph_bin_write_f32(w, 1.5F) != NPH_RESULT_OK ||
            nph_bin_write_f32(w, -2.0F) != NPH_RESULT_OK ||
            nph_bin_write_bool(w, (i & 1U) != 0U) != NPH_RESULT_OK) {
            abort();
        }
    }
    nph_bin_write_section_end(w, &section);
    if (nph_bin_write_checksum(w) != NPH_RESULT_OK) {
        abort();
    }
}

NBENCH(bin_write_scene)
{
    NphBinWriter w;
    nph_bin_writer_init(&w, nph_allocator_system());
    while (nbench_keep_running(state)) {
        write_scene_binary(&w);
    }
    static char label[64];
    snprintf(label, sizeof label, "%llu bytes", (unsigned long long)w.len);
    nbench_set_label(state, label);
    nbench_do_not_optimize(w.data);
    nph_bin_writer_free(&w);
}

NBENCH(bin_read_scene)
{
    NphBinWriter w;
    nph_bin_writer_init(&w, nph_allocator_system());
    write_scene_binary(&w);
    u64 acc = 0U;
    while (nbench_keep_running(state)) {
        u64 payload;
        if (nph_bin_verify_checksum(w.data, w.len, &payload) != NPH_RESULT_OK) {
            abort();
        }
        NphBinReader r = nph_bin_reader(w.data, payload);
        u32 version;
        u32 tag;
        u32 size;
        if (nph_bin_read_header(&r, 0x4E534345U, 1U, 1U, &version) != NPH_RESULT_OK ||
            nph_bin_read_section(&r, &tag, &size) != NPH_RESULT_OK) {
            abort();
        }
        for (u32 i = 0; i < ENTITY_COUNT; ++i) {
            u32 id;
            NphStr name;
            f32 xyz[3];
            bool visible;
            if (nph_bin_read_u32(&r, &id) != NPH_RESULT_OK ||
                nph_bin_read_str(&r, &name) != NPH_RESULT_OK ||
                nph_bin_read_f32(&r, &xyz[0]) != NPH_RESULT_OK ||
                nph_bin_read_f32(&r, &xyz[1]) != NPH_RESULT_OK ||
                nph_bin_read_f32(&r, &xyz[2]) != NPH_RESULT_OK ||
                nph_bin_read_bool(&r, &visible) != NPH_RESULT_OK) {
                abort();
            }
            acc += id + name.len + (visible ? 1U : 0U);
        }
    }
    nbench_do_not_optimize_u64(acc);
    nph_bin_writer_free(&w);
}

static NPH_CVAR_BOOL(cv_a, "bench.a", 1, "a", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_INT(cv_b, "bench.b", 1, 0, 100, "b", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_FLOAT(cv_c, "bench.c", 1.0, 0.0, 10.0, "c", NPH_CVAR_FLAG_SAVED);
static NPH_CVAR_STRING(cv_d, "bench.d", "d", "d", NPH_CVAR_FLAG_SAVED);

NBENCH(cvar_apply_config)
{
    NphCvarRegistry r;
    if (nph_cvar_registry_init(&r, nph_allocator_system()) != NPH_RESULT_OK ||
        nph_cvar_register(&r, &cv_a) != NPH_RESULT_OK ||
        nph_cvar_register(&r, &cv_b) != NPH_RESULT_OK ||
        nph_cvar_register(&r, &cv_c) != NPH_RESULT_OK ||
        nph_cvar_register(&r, &cv_d) != NPH_RESULT_OK) {
        abort();
    }
    const NphStr config =
        NPH_STR("bench.a = false\nbench.b = 42\nbench.c = 2.5\nbench.d = \"text\"\n");
    nbench_set_items_per_iteration(state, 4U);
    while (nbench_keep_running(state)) {
        if (nph_cvar_apply_config(&r, config, NULL) != NPH_RESULT_OK) {
            abort();
        }
        if (nph_cvar_reset(&r, &cv_d) != NPH_RESULT_OK) {
            abort();
        }
    }
    nph_cvar_registry_free(&r);
}
