// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/json.h>
#include <nepheline/platform/file.h>

#include <cook/database.h>
#include <stdio.h>
#include <string.h>

typedef NPH_ARRAY(NphGuid) GuidArray;

NphResult cook_db_init(CookDb *db, NphAllocator *alloc)
{
    NPH_ASSERT(db != NULL && alloc != NULL);
    memset(db, 0, sizeof *db);
    db->alloc = alloc;
    NPH_ARRAY_INIT(&db->entries, alloc);
    const NphHashMapDesc u64_to_index = nph_hashmap_desc_u64(sizeof(u32));
    NphHashMapDesc guid_to_index;
    memset(&guid_to_index, 0, sizeof guid_to_index);
    guid_to_index.key_size = sizeof(NphGuid);
    guid_to_index.val_size = sizeof(u32);
    NphResult r = nph_hashmap_init(&db->by_key, alloc, &u64_to_index);
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&db->by_guid, alloc, &guid_to_index);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&db->by_output, alloc, &u64_to_index);
    }
    if (r != NPH_RESULT_OK) {
        nph_hashmap_free(&db->by_key);
        nph_hashmap_free(&db->by_guid);
        nph_hashmap_free(&db->by_output);
    }
    return r;
}

static void entry_free(NphAllocator *alloc, CookDbEntry *e)
{
    if (e->source != NULL) {
        nph_str_free(alloc, e->source, strlen(e->source));
    }
    if (e->sub != NULL) {
        nph_str_free(alloc, e->sub, strlen(e->sub));
    }
    if (e->output != NULL) {
        nph_str_free(alloc, e->output, strlen(e->output));
    }
    if (e->deps != NULL) {
        NPH_FREE_ARRAY(alloc, NphGuid, e->deps, e->dep_count);
    }
    memset(e, 0, sizeof *e);
}

void cook_db_free(CookDb *db)
{
    if (db == NULL || db->alloc == NULL) {
        return;
    }
    NPH_ARRAY_FOREACH (CookDbEntry, e, &db->entries) {
        entry_free(db->alloc, e);
    }
    NPH_ARRAY_FREE(&db->entries);
    nph_hashmap_free(&db->by_key);
    nph_hashmap_free(&db->by_guid);
    nph_hashmap_free(&db->by_output);
    db->alloc = NULL;
}

// ---------------------------------------------------------------------------
// The indices
// ---------------------------------------------------------------------------

static u64 key_hash(NphStr source, NphStr sub)
{
    return nph_hash_combine(nph_str_hash(source), nph_str_hash(sub));
}

static NphResult index_entry(CookDb *db, u32 i)
{
    const CookDbEntry *e = &db->entries.data[i];
    NPH_TRY(nph_hashmap_set_u64(
        &db->by_key, key_hash(nph_str_from_cstr(e->source), nph_str_from_cstr(e->sub)), &i));
    NPH_TRY(nph_hashmap_set(&db->by_guid, &e->guid, &i));
    return nph_hashmap_set_u64(&db->by_output, nph_str_hash(nph_str_from_cstr(e->output)), &i);
}

static void unindex_entry(CookDb *db, const CookDbEntry *e)
{
    NPH_IGNORE_RESULT(nph_hashmap_remove_u64(
        &db->by_key, key_hash(nph_str_from_cstr(e->source), nph_str_from_cstr(e->sub))));
    NPH_IGNORE_RESULT(nph_hashmap_remove(&db->by_guid, &e->guid));
    NPH_IGNORE_RESULT(
        nph_hashmap_remove_u64(&db->by_output, nph_str_hash(nph_str_from_cstr(e->output))));
}

static NphResult reindex(CookDb *db)
{
    nph_hashmap_clear(&db->by_key);
    nph_hashmap_clear(&db->by_guid);
    nph_hashmap_clear(&db->by_output);
    for (u32 i = 0; i < db->entries.len; ++i) {
        NPH_TRY(index_entry(db, i));
    }
    return NPH_RESULT_OK;
}

const CookDbEntry *cook_db_find(const CookDb *db, NphStr source, NphStr sub)
{
    NPH_ASSERT(db != NULL);
    const u32 *index = (const u32 *)nph_hashmap_find_u64(&db->by_key, key_hash(source, sub));
    if (index == NULL) {
        return NULL;
    }
    const CookDbEntry *e = &db->entries.data[*index];
    if (!nph_str_equals(nph_str_from_cstr(e->source), source) ||
        !nph_str_equals(nph_str_from_cstr(e->sub), sub)) {
        return NULL; // a 64-bit hash collision: not this key
    }
    return e;
}

const CookDbEntry *cook_db_find_guid(const CookDb *db, NphGuid guid)
{
    NPH_ASSERT(db != NULL);
    const u32 *index = (const u32 *)nph_hashmap_find(&db->by_guid, &guid);
    return index != NULL ? &db->entries.data[*index] : NULL;
}

const CookDbEntry *cook_db_find_output(const CookDb *db, NphStr output)
{
    NPH_ASSERT(db != NULL);
    const u32 *index = (const u32 *)nph_hashmap_find_u64(&db->by_output, nph_str_hash(output));
    if (index == NULL) {
        return NULL;
    }
    const CookDbEntry *e = &db->entries.data[*index];
    return nph_str_equals(nph_str_from_cstr(e->output), output) ? e : NULL;
}

// ---------------------------------------------------------------------------
// Changes
// ---------------------------------------------------------------------------

static void remove_at(CookDb *db, u32 index)
{
    entry_free(db->alloc, &db->entries.data[index]);
    NPH_ARRAY_REMOVE_AT(&db->entries, index);
}

NphResult cook_db_set(CookDb *db, NphStr source, NphStr sub, NphStr output, NphGuid guid, u32 type,
                      u64 content_hash, u64 settings_hash, const NphGuid *deps, u32 dep_count)
{
    NPH_ASSERT(db != NULL);
    NPH_ASSERT(deps != NULL || dep_count == 0U);
    const CookDbEntry *by_key = cook_db_find(db, source, sub);
    const CookDbEntry *by_guid = cook_db_find_guid(db, guid);
    const CookDbEntry *by_output = cook_db_find_output(db, output);
    if (by_output != NULL && by_output != by_key && by_output != by_guid) {
        return NPH_RESULT_ALREADY_EXISTS; // another asset is written there
    }
    if (by_key != NULL && by_guid != NULL && by_key != by_guid) {
        // The GUID moves to this key from another entry, which would keep
        // a duplicate: that entry goes (a rare path, so the O(n) removal
        // and reindex are fine).
        remove_at(db, (u32)(by_guid - db->entries.data));
        NPH_TRY(reindex(db));
        by_key = cook_db_find(db, source, sub);
        by_guid = NULL;
    }
    CookDbEntry e;
    memset(&e, 0, sizeof e);
    NphResult r = nph_str_dup(db->alloc, source, &e.source);
    if (r == NPH_RESULT_OK) {
        r = nph_str_dup(db->alloc, sub, &e.sub);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_str_dup(db->alloc, output, &e.output);
    }
    if (r == NPH_RESULT_OK && dep_count != 0U) {
        e.deps = NPH_ALLOC_ARRAY(db->alloc, NphGuid, dep_count);
        if (e.deps == NULL) {
            r = NPH_RESULT_OUT_OF_MEMORY;
        } else {
            memcpy(e.deps, deps, (size_t)dep_count * sizeof(NphGuid));
            e.dep_count = dep_count;
        }
    }
    if (r != NPH_RESULT_OK) {
        entry_free(db->alloc, &e);
        return r;
    }
    e.guid = guid;
    e.type = type;
    e.content_hash = content_hash;
    e.settings_hash = settings_hash;
    const CookDbEntry *existing = by_key != NULL ? by_key : by_guid;
    u32 index;
    if (existing != NULL) {
        index = (u32)(existing - db->entries.data);
        CookDbEntry *slot = &db->entries.data[index];
        unindex_entry(db, slot);
        entry_free(db->alloc, slot);
        *slot = e;
    } else {
        index = db->entries.len;
        r = NPH_ARRAY_PUSH(&db->entries, e);
        if (r != NPH_RESULT_OK) {
            entry_free(db->alloc, &e);
            return r;
        }
    }
    db->dirty = 1;
    return index_entry(db, index);
}

bool cook_db_remove(CookDb *db, NphStr source, NphStr sub)
{
    NPH_ASSERT(db != NULL);
    const CookDbEntry *e = cook_db_find(db, source, sub);
    if (e == NULL) {
        return false;
    }
    remove_at(db, (u32)(e - db->entries.data));
    db->dirty = 1;
    NPH_IGNORE_RESULT(reindex(db)); // only shrinks the maps' contents
    return true;
}

u32 cook_db_remove_source(CookDb *db, NphStr source)
{
    NPH_ASSERT(db != NULL);
    u32 removed = 0U;
    u32 write = 0U;
    for (u32 i = 0; i < db->entries.len; ++i) {
        CookDbEntry *e = &db->entries.data[i];
        if (nph_str_equals(nph_str_from_cstr(e->source), source)) {
            entry_free(db->alloc, e);
            ++removed;
            continue;
        }
        db->entries.data[write++] = *e;
    }
    db->entries.len = write;
    if (removed != 0U) {
        db->dirty = 1;
        NPH_IGNORE_RESULT(reindex(db));
    }
    return removed;
}

bool cook_db_entry_is_current(const CookDbEntry *entry, u64 content_hash, u64 settings_hash)
{
    return entry != NULL && entry->content_hash == content_hash &&
           entry->settings_hash == settings_hash;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

static const char *hex64(u64 v, char out[17])
{
    static const char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < 16U; ++i) {
        out[i] = digits[(v >> (60U - 4U * i)) & 0xFU];
    }
    out[16] = '\0';
    return out;
}

static bool parse_hex64(NphStr s, u64 *out)
{
    if (s.len != 16U) {
        return false;
    }
    u64 v = 0U;
    for (u64 i = 0; i < 16U; ++i) {
        const char c = s.data[i];
        u32 d;
        if (c >= '0' && c <= '9') {
            d = (u32)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (u32)(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            d = (u32)(c - 'A') + 10U;
        } else {
            return false;
        }
        v = (v << 4U) | d;
    }
    *out = v;
    return true;
}

// Sorts the entries by output path (insertion sort: nearly sorted after a
// load, so close to linear; a fresh database of thousands is still cheap
// next to the cooking that produced it).
static void sort_entries(CookDb *db)
{
    for (u32 i = 1; i < db->entries.len; ++i) {
        const CookDbEntry key = db->entries.data[i];
        u32 j = i;
        while (j > 0U && strcmp(key.output, db->entries.data[j - 1U].output) < 0) {
            db->entries.data[j] = db->entries.data[j - 1U];
            --j;
        }
        db->entries.data[j] = key;
    }
}

static NphResult write_entry(NphJsonWriter *w, const CookDbEntry *e)
{
    char hex[17];
    char guid_text[NPH_GUID_STRING_LEN + 1U];
    char fourcc[5];
    nph_fourcc_to_string(e->type, fourcc);
    nph_guid_to_string(e->guid, guid_text);
    NPH_TRY(nph_json_writer_begin_object(w));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("output")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(e->output)));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("source")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(e->source)));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("sub")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(e->sub)));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("guid")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(guid_text)));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("type")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(fourcc)));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("content_hash")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(hex64(e->content_hash, hex))));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("settings_hash")));
    NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(hex64(e->settings_hash, hex))));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("deps")));
    NPH_TRY(nph_json_writer_begin_array(w));
    for (u32 d = 0; d < e->dep_count; ++d) {
        nph_guid_to_string(e->deps[d], guid_text);
        NPH_TRY(nph_json_writer_str(w, nph_str_from_cstr(guid_text)));
    }
    NPH_TRY(nph_json_writer_end_array(w));
    return nph_json_writer_end_object(w);
}

NphResult cook_db_save(CookDb *db, const char *path, bool force)
{
    NPH_ASSERT(db != NULL && path != NULL);
    if (!db->dirty && !force) {
        return NPH_RESULT_OK;
    }
    sort_entries(db);
    NPH_TRY(reindex(db));
    NphStrBuilder sb;
    nph_sb_init(&sb, db->alloc);
    NphJsonWriter w;
    nph_json_writer_init(&w, &sb, true);
    NphResult r = nph_json_writer_begin_object(&w);
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("version"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_i64(&w, (i64)COOK_DB_VERSION);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_key(&w, NPH_STR("assets"));
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_begin_array(&w);
    }
    for (u32 i = 0; i < db->entries.len && r == NPH_RESULT_OK; ++i) {
        r = write_entry(&w, &db->entries.data[i]);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_end_array(&w);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_json_writer_end_object(&w);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_sb_append_char(&sb, '\n');
    }
    // Written beside and renamed over: an interrupted save leaves the old
    // database, never half of a new one.
    char tmp[1100];
    if (r == NPH_RESULT_OK && (u64)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) {
        r = NPH_RESULT_OVERFLOW;
    }
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(tmp, sb.data, sb.len);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_fs_rename(tmp, path);
    }
    nph_sb_free(&sb);
    if (r == NPH_RESULT_OK) {
        db->dirty = 0;
    }
    return r;
}

// One entry of the "assets" array; `scratch` holds its dependencies.
static NphResult load_entry(CookDb *db, const NphJsonValue *item, GuidArray *scratch)
{
    if (item->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const NphStr output = nph_json_get_str(item, NPH_STR("output"), nph_str_empty());
    const NphStr source = nph_json_get_str(item, NPH_STR("source"), nph_str_empty());
    const NphStr sub = nph_json_get_str(item, NPH_STR("sub"), nph_str_empty());
    const NphStr guid_text = nph_json_get_str(item, NPH_STR("guid"), nph_str_empty());
    const NphStr type_text = nph_json_get_str(item, NPH_STR("type"), nph_str_empty());
    NphGuid guid;
    u64 content_hash;
    u64 settings_hash;
    if (output.len == 0U || source.len == 0U || !nph_guid_parse(guid_text, &guid) ||
        type_text.len != 4U ||
        !parse_hex64(nph_json_get_str(item, NPH_STR("content_hash"), nph_str_empty()),
                     &content_hash) ||
        !parse_hex64(nph_json_get_str(item, NPH_STR("settings_hash"), nph_str_empty()),
                     &settings_hash)) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const u32 type =
        NPH_FOURCC(type_text.data[0], type_text.data[1], type_text.data[2], type_text.data[3]);
    NPH_ARRAY_CLEAR(scratch);
    const NphJsonValue *dep_list = nph_json_object_get(item, NPH_STR("deps"));
    if (dep_list != NULL) {
        if (dep_list->type != NPH_JSON_ARRAY) {
            return NPH_RESULT_PARSE_ERROR;
        }
        NPH_JSON_FOREACH(d, dep_list)
        {
            NphStr text;
            NphGuid g;
            if (nph_json_as_str(d, &text) != NPH_RESULT_OK || !nph_guid_parse(text, &g)) {
                return NPH_RESULT_PARSE_ERROR;
            }
            NPH_TRY(NPH_ARRAY_PUSH(scratch, g));
        }
    }
    const NphResult r = cook_db_set(db, source, sub, output, guid, type, content_hash,
                                    settings_hash, scratch->data, scratch->len);
    return r == NPH_RESULT_ALREADY_EXISTS ? NPH_RESULT_PARSE_ERROR : r; // two assets, one output
}

NphResult cook_db_load(CookDb *db, const char *path)
{
    NPH_ASSERT(db != NULL && path != NULL);
    void *text = NULL;
    u64 size = 0U;
    const NphResult rr = nph_file_read_all(path, db->alloc, &text, &size);
    if (rr == NPH_RESULT_NOT_FOUND) {
        return NPH_RESULT_OK;
    }
    if (rr != NPH_RESULT_OK) {
        return rr;
    }
    NphJsonDoc doc;
    NphResult r = nph_json_parse(nph_str((const char *)text, size), db->alloc, &doc);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s:%u:%u: %s", path, doc.error_line, doc.error_column,
                      doc.error_message != NULL ? doc.error_message : "invalid JSON");
        nph_json_doc_free(&doc);
        nph_file_free_all(db->alloc, text, size);
        return NPH_RESULT_PARSE_ERROR;
    }
    const NphJsonValue *root = doc.root;
    if (root == NULL || root->type != NPH_JSON_OBJECT) {
        r = NPH_RESULT_PARSE_ERROR;
    } else if (nph_json_get_i64(root, NPH_STR("version"), 0) != (i64)COOK_DB_VERSION) {
        r = NPH_RESULT_VERSION_MISMATCH;
    }
    const NphJsonValue *assets =
        r == NPH_RESULT_OK ? nph_json_object_get(root, NPH_STR("assets")) : NULL;
    if (r == NPH_RESULT_OK && (assets == NULL || assets->type != NPH_JSON_ARRAY)) {
        r = NPH_RESULT_PARSE_ERROR;
    }
    if (r == NPH_RESULT_OK) {
        GuidArray scratch;
        NPH_ARRAY_INIT(&scratch, db->alloc);
        NPH_JSON_FOREACH(item, assets)
        {
            r = load_entry(db, item, &scratch);
            if (r != NPH_RESULT_OK) {
                break;
            }
        }
        NPH_ARRAY_FREE(&scratch);
    }
    nph_json_doc_free(&doc);
    nph_file_free_all(db->alloc, text, size);
    if (r != NPH_RESULT_OK) {
        // All or nothing: a bad file leaves an empty database.
        NPH_ARRAY_FOREACH (CookDbEntry, e, &db->entries) {
            entry_free(db->alloc, e);
        }
        db->entries.len = 0U;
        NPH_IGNORE_RESULT(reindex(db));
        return r;
    }
    db->dirty = 0;
    return NPH_RESULT_OK;
}
