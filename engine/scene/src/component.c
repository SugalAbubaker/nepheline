// SPDX-License-Identifier: Apache-2.0
// Component descriptors: validation, defaults, field-wise comparison and
// hashing, layout hashes, entity remapping (component.h).

#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>

#include "scene_internal.h"

#include <string.h>

typedef struct KindInfo {
    const char *name;
    u32 size;
    u32 align;
} KindInfo;

static const KindInfo g_kinds[NPH_FIELD_KIND_COUNT] = {
    {"BOOL", sizeof(b32), NPH_ALIGNOF(b32)},
    {"U8", sizeof(u8), NPH_ALIGNOF(u8)},
    {"U16", sizeof(u16), NPH_ALIGNOF(u16)},
    {"U32", sizeof(u32), NPH_ALIGNOF(u32)},
    {"U64", sizeof(u64), NPH_ALIGNOF(u64)},
    {"I8", sizeof(i8), NPH_ALIGNOF(i8)},
    {"I16", sizeof(i16), NPH_ALIGNOF(i16)},
    {"I32", sizeof(i32), NPH_ALIGNOF(i32)},
    {"I64", sizeof(i64), NPH_ALIGNOF(i64)},
    {"F32", sizeof(f32), NPH_ALIGNOF(f32)},
    {"F64", sizeof(f64), NPH_ALIGNOF(f64)},
    {"VEC2", sizeof(NphVec2), NPH_ALIGNOF(NphVec2)},
    {"VEC3", sizeof(NphVec3), NPH_ALIGNOF(NphVec3)},
    {"VEC4", sizeof(NphVec4), NPH_ALIGNOF(NphVec4)},
    {"QUAT", sizeof(NphQuat), NPH_ALIGNOF(NphQuat)},
    {"DVEC3", sizeof(NphDVec3), NPH_ALIGNOF(NphDVec3)},
    {"ENTITY", sizeof(NphEntity), NPH_ALIGNOF(NphEntity)},
    {"STR_ID", sizeof(NphStrId), NPH_ALIGNOF(NphStrId)},
    {"TEXT", sizeof(NphText), NPH_ALIGNOF(NphText)},
};

u32 nph_field_kind_size(NphFieldKind kind)
{
    return (u32)kind < NPH_FIELD_KIND_COUNT ? g_kinds[kind].size : 0U;
}

u32 nph_field_kind_align(NphFieldKind kind)
{
    return (u32)kind < NPH_FIELD_KIND_COUNT ? g_kinds[kind].align : 0U;
}

const char *nph_field_kind_name(NphFieldKind kind)
{
    return (u32)kind < NPH_FIELD_KIND_COUNT ? g_kinds[kind].name : "INVALID";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

static bool name_is_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    u32 len = 0U;
    for (const char *p = name; *p != '\0'; ++p) {
        const char c = *p;
        const bool ok = nph_ascii_is_alpha(c) || nph_ascii_is_digit(c) || c == '_' || c == '.';
        if (!ok || ++len > NPH_ECS_MAX_NAME_LEN) {
            return false;
        }
    }
    return true;
}

static NphResult invalid(const char **out_reason, const char *reason)
{
    if (out_reason != NULL) {
        *out_reason = reason;
    }
    return NPH_RESULT_INVALID_ARGUMENT;
}

NphResult nph_component_desc_validate(const NphComponentDesc *desc, const char **out_reason)
{
    if (out_reason != NULL) {
        *out_reason = "";
    }
    if (desc == NULL) {
        return invalid(out_reason, "no descriptor");
    }
    if (!name_is_valid(desc->name)) {
        return invalid(out_reason, "the name must be 1-63 characters of [A-Za-z0-9_.]");
    }
    if (desc->size == 0U) {
        if (desc->field_count != 0U) {
            return invalid(out_reason, "a tag (size 0) has no fields");
        }
        return NPH_RESULT_OK;
    }
    if (desc->size > NPH_ECS_MAX_COMPONENT_SIZE) {
        return invalid(out_reason, "the component is larger than NPH_ECS_MAX_COMPONENT_SIZE");
    }
    if (desc->align == 0U || !nph_is_pow2_u32(desc->align) ||
        desc->align > NPH_ECS_MAX_COMPONENT_ALIGN) {
        return invalid(out_reason, "the alignment must be a power of two up to 64");
    }
    if (desc->size % desc->align != 0U) {
        return invalid(out_reason, "the size must be a multiple of the alignment");
    }
    if (desc->field_count > NPH_ECS_MAX_FIELDS) {
        return invalid(out_reason, "more than NPH_ECS_MAX_FIELDS fields");
    }
    if (desc->field_count > 0U && desc->fields == NULL) {
        return invalid(out_reason, "field_count is set but fields is NULL");
    }
    for (u32 i = 0; i < desc->field_count; ++i) {
        const NphFieldDesc *f = &desc->fields[i];
        if (!name_is_valid(f->name)) {
            return invalid(out_reason, "a field name must be 1-63 characters of [A-Za-z0-9_.]");
        }
        if (f->kind >= NPH_FIELD_KIND_COUNT) {
            return invalid(out_reason, "a field has an invalid kind");
        }
        const u32 kind_size = g_kinds[f->kind].size;
        const u32 kind_align = g_kinds[f->kind].align;
        if (f->size != kind_size) {
            return invalid(out_reason, "a field's size does not match its kind");
        }
        if (f->offset % kind_align != 0U) {
            return invalid(out_reason, "a field is misaligned for its kind");
        }
        if ((u64)f->offset + f->size > desc->size) {
            return invalid(out_reason, "a field extends past the end of the component");
        }
        for (u32 j = 0; j < i; ++j) {
            const NphFieldDesc *g = &desc->fields[j];
            if (strcmp(f->name, g->name) == 0) {
                return invalid(out_reason, "two fields have the same name");
            }
            const bool disjoint =
                f->offset + f->size <= g->offset || g->offset + g->size <= f->offset;
            if (!disjoint) {
                return invalid(out_reason, "two fields overlap");
            }
        }
        if ((f->flags & NPH_FIELD_FLAG_RANGE) != 0U && !(f->min <= f->max)) {
            return invalid(out_reason, "a field's range has min > max");
        }
    }
    return NPH_RESULT_OK;
}

const NphFieldDesc *nph_component_find_field(const NphComponentDesc *desc, NphStr name)
{
    NPH_ASSERT(desc != NULL);
    for (u32 i = 0; i < desc->field_count; ++i) {
        if (nph_str_equals(nph_str_from_cstr(desc->fields[i].name), name)) {
            return &desc->fields[i];
        }
    }
    return NULL;
}

void nph_component_init_default(const NphComponentDesc *desc, void *dst)
{
    NPH_ASSERT(desc != NULL);
    if (desc->size == 0U) {
        return;
    }
    if (desc->defaults != NULL) {
        memcpy(dst, desc->defaults, desc->size);
    } else {
        memset(dst, 0, desc->size);
    }
}

// ---------------------------------------------------------------------------
// Field-wise comparison and hashing
// ---------------------------------------------------------------------------

static u32 text_len(const u8 *p)
{
    u32 n = 0U;
    while (n < NPH_TEXT_CAPACITY && p[n] != 0U) {
        ++n;
    }
    return n;
}

bool nph_component_fields_equal(const NphComponentDesc *desc, const void *a, const void *b)
{
    NPH_ASSERT(desc != NULL);
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    for (u32 i = 0; i < desc->field_count; ++i) {
        const NphFieldDesc *f = &desc->fields[i];
        const u8 *fa = pa + f->offset;
        const u8 *fb = pb + f->offset;
        if (f->kind == NPH_FIELD_TEXT) {
            const u32 la = text_len(fa);
            if (la != text_len(fb) || memcmp(fa, fb, la) != 0) {
                return false;
            }
        } else if (memcmp(fa, fb, f->size) != 0) {
            return false;
        }
    }
    return true;
}

u64 nph_component_hash_fields(const NphComponentDesc *desc, const void *value, u64 seed)
{
    NPH_ASSERT(desc != NULL);
    const u8 *p = (const u8 *)value;
    u64 h = seed;
    for (u32 i = 0; i < desc->field_count; ++i) {
        const NphFieldDesc *f = &desc->fields[i];
        const u8 *fp = p + f->offset;
        const u32 len = f->kind == NPH_FIELD_TEXT ? text_len(fp) : f->size;
        h = nph_hash_combine(h, nph_hash64(fp, len, NPH_HASH_DEFAULT_SEED));
    }
    return h;
}

u64 nph_component_layout_hash(const NphComponentDesc *desc)
{
    NPH_ASSERT(desc != NULL);
    u64 h = nph_hash64_cstr(desc->name, NPH_HASH_DEFAULT_SEED);
    h = nph_hash_combine(h, ((u64)desc->size << 32U) | desc->align);
    for (u32 i = 0; i < desc->field_count; ++i) {
        const NphFieldDesc *f = &desc->fields[i];
        h = nph_hash_combine(h, nph_hash64_cstr(f->name, NPH_HASH_DEFAULT_SEED));
        h = nph_hash_combine(h, ((u64)f->kind << 32U) | f->offset);
    }
    return h;
}

void nph_component_remap_entities(const NphComponentInfo *info, void *value, NphEntityMapFn fn,
                                  void *user)
{
    u8 *p = (u8 *)value;
    for (u32 i = 0; i < info->entity_field_count; ++i) {
        NphEntity e;
        memcpy(&e, p + info->entity_field_offsets[i], sizeof e);
        e = fn(user, e);
        memcpy(p + info->entity_field_offsets[i], &e, sizeof e);
    }
}

// ---------------------------------------------------------------------------
// NphText
// ---------------------------------------------------------------------------

bool nph_text_set(NphText *t, NphStr s)
{
    NPH_ASSERT(t != NULL);
    u64 n = s.len;
    bool fits = true;
    if (n > NPH_TEXT_CAPACITY - 1U) {
        fits = false;
        n = NPH_TEXT_CAPACITY - 1U;
        // Back up to the start of a UTF-8 sequence: continuation bytes are
        // 10xxxxxx, so the cut never splits a character.
        while (n > 0U && ((u8)s.data[n] & 0xC0U) == 0x80U) {
            --n;
        }
    }
    memset(t->text, 0, sizeof t->text);
    if (n > 0U) {
        memcpy(t->text, s.data, (size_t)n);
    }
    return fits;
}

NphStr nph_text_str(const NphText *t)
{
    NPH_ASSERT(t != NULL);
    return nph_str(t->text, text_len((const u8 *)t->text));
}
