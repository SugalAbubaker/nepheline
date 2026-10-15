// SPDX-License-Identifier: Apache-2.0
#pragma once

// Component reflection without C++ (CLAUDE.md §8.7 "components are defined
// with X-macros that generate the struct, field metadata (name, type,
// offset, editor hints), and serialization"; docs/scene.md "Components").
//
// A component is a plain C struct whose fields are listed once, in an
// X-macro. The same list generates the struct and, in one source file, the
// descriptor the world registers: the component's name, size, alignment,
// default value, and one NphFieldDesc per field. Serialization (scene_io.h),
// the editor's inspector, state hashing, and entity-reference remapping all
// work from that descriptor, so adding a field is a one-line change that
// every consumer picks up.
//
//     // health.h
//     #define GAME_HEALTH_FIELDS(F)                                       \
//         F(F32, current, NPH_FIELD_NO_HINTS)                             \
//         F(F32, maximum, NPH_FIELD_RANGE(1.0F, 1000.0F))                 \
//         F(ENTITY, last_attacker, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))
//     NPH_COMPONENT_STRUCT(GameHealth, GAME_HEALTH_FIELDS);
//     NPH_COMPONENT_DECLARE(GameHealth);
//
//     // health.c
//     static const GameHealth g_health_defaults = {100.0F, 100.0F, {0U}};
//     NPH_COMPONENT_DEFINE(GameHealth, GAME_HEALTH_FIELDS, "Health", 0U, &g_health_defaults)
//
//     // at startup
//     NphComponentId health;
//     NPH_TRY(nph_world_register_component(world, GameHealth_component(), &health));
//
// Each F(KIND, name, hints) entry declares one field. KIND is one of the
// NphFieldKind names without the prefix (F32, VEC3, ENTITY, ...), which
// fixes both the C type (the NPH_FIELD_CTYPE_* macros) and how the field is
// serialized. `hints` is one of the NPH_FIELD_* hint macros below.
//
// Tags are components without data (a mask bit and nothing else):
// NPH_TAG_DECLARE / NPH_TAG_DEFINE.
//
// Components are plain data: they are copied with memcpy, moved between
// chunks, and compared field by field. Pointers are not a field kind on
// purpose; reference other entities with ENTITY fields (remapped on load
// and on instantiation) and assets by ID.
//
// Thread-safety: descriptors are immutable static data; NPH_THREADSAFE.

#include <nepheline/core/string.h>
#include <nepheline/core/string_id.h>
#include <nepheline/math/quat.h>
#include <nepheline/math/vec.h>
#include <nepheline/scene/entity.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Field kinds
// ---------------------------------------------------------------------------

// Inline UTF-8 text, NUL-terminated, at most NPH_TEXT_CAPACITY - 1 bytes.
// For names and short labels; longer text belongs in an asset.
#define NPH_TEXT_CAPACITY 64U

typedef struct NphText {
    char text[NPH_TEXT_CAPACITY];
} NphText;

typedef enum NphFieldKind {
    NPH_FIELD_BOOL = 0, // b32
    NPH_FIELD_U8,
    NPH_FIELD_U16,
    NPH_FIELD_U32,
    NPH_FIELD_U64,
    NPH_FIELD_I8,
    NPH_FIELD_I16,
    NPH_FIELD_I32,
    NPH_FIELD_I64,
    NPH_FIELD_F32,
    NPH_FIELD_F64,
    NPH_FIELD_VEC2,
    NPH_FIELD_VEC3,
    NPH_FIELD_VEC4,
    NPH_FIELD_QUAT,
    NPH_FIELD_DVEC3,  // 64-bit world positions (§8.2 "Large-world coordinates")
    NPH_FIELD_ENTITY, // remapped when entities are saved, loaded, or instantiated
    NPH_FIELD_STR_ID, // a 64-bit string ID, written as its text when the world knows it
    NPH_FIELD_TEXT,   // NphText
    NPH_FIELD_KIND_COUNT
} NphFieldKind;

// The C type behind each kind (used by NPH_COMPONENT_STRUCT).
#define NPH_FIELD_CTYPE_BOOL   b32
#define NPH_FIELD_CTYPE_U8     u8
#define NPH_FIELD_CTYPE_U16    u16
#define NPH_FIELD_CTYPE_U32    u32
#define NPH_FIELD_CTYPE_U64    u64
#define NPH_FIELD_CTYPE_I8     i8
#define NPH_FIELD_CTYPE_I16    i16
#define NPH_FIELD_CTYPE_I32    i32
#define NPH_FIELD_CTYPE_I64    i64
#define NPH_FIELD_CTYPE_F32    f32
#define NPH_FIELD_CTYPE_F64    f64
#define NPH_FIELD_CTYPE_VEC2   NphVec2
#define NPH_FIELD_CTYPE_VEC3   NphVec3
#define NPH_FIELD_CTYPE_VEC4   NphVec4
#define NPH_FIELD_CTYPE_QUAT   NphQuat
#define NPH_FIELD_CTYPE_DVEC3  NphDVec3
#define NPH_FIELD_CTYPE_ENTITY NphEntity
#define NPH_FIELD_CTYPE_STR_ID NphStrId
#define NPH_FIELD_CTYPE_TEXT   NphText

// Size and alignment of a kind's C type; 0 for an out-of-range kind.
u32 nph_field_kind_size(NphFieldKind kind);
u32 nph_field_kind_align(NphFieldKind kind);

// The kind's name as written in the X-macro ("F32", "VEC3", ...), or
// "INVALID".
const char *nph_field_kind_name(NphFieldKind kind);

// ---------------------------------------------------------------------------
// Field hints (for the editor and the serializer)
// ---------------------------------------------------------------------------

typedef enum NphFieldFlags {
    NPH_FIELD_FLAG_RANGE = 1U << 0U,     // `min` and `max` bound the editor's slider
    NPH_FIELD_FLAG_HIDDEN = 1U << 1U,    // not shown in the inspector
    NPH_FIELD_FLAG_READONLY = 1U << 2U,  // shown, not editable
    NPH_FIELD_FLAG_COLOR = 1U << 3U,     // VEC3/VEC4 edited as a linear color
    NPH_FIELD_FLAG_ANGLE = 1U << 4U,     // radians, shown in degrees
    NPH_FIELD_FLAG_TRANSIENT = 1U << 5U, // not saved; loads as the default value
} NphFieldFlags;

// A hint list is a parenthesised (flags, min, max) triple, expanded by the
// generator macros.
#define NPH_FIELD_HINTS(flags, lo, hi) ((u32)(flags), (f32)(lo), (f32)(hi))
#define NPH_FIELD_NO_HINTS             NPH_FIELD_HINTS(0U, 0.0F, 0.0F)
#define NPH_FIELD_RANGE(lo, hi)        NPH_FIELD_HINTS(NPH_FIELD_FLAG_RANGE, lo, hi)
#define NPH_FIELD_FLAGS(flags)         NPH_FIELD_HINTS(flags, 0.0F, 0.0F)

typedef struct NphFieldDesc {
    const char *name; // the field's C name, also its key in scene files
    u32 kind;         // NphFieldKind
    u32 offset;       // bytes from the start of the component
    u32 size;         // == nph_field_kind_size(kind)
    u32 flags;        // NphFieldFlags
    f32 min;          // with NPH_FIELD_FLAG_RANGE
    f32 max;
} NphFieldDesc;

// ---------------------------------------------------------------------------
// Component descriptors
// ---------------------------------------------------------------------------

typedef enum NphComponentFlags {
    // Never saved: derived state that is recomputed (WorldTransform).
    NPH_COMPONENT_FLAG_TRANSIENT = 1U << 0U,
    // Maintained by engine functions only (Hierarchy, through
    // nph_world_set_parent); systems read it and never write it.
    NPH_COMPONENT_FLAG_INTERNAL = 1U << 1U,
} NphComponentFlags;

// Largest component, in bytes. An archetype's chunk must hold at least a
// few entities, and a component this size already leaves room for only
// seven in a 16 KiB chunk.
#define NPH_ECS_MAX_COMPONENT_SIZE 2048U
// Largest component alignment; chunk columns start on cache lines.
#define NPH_ECS_MAX_COMPONENT_ALIGN 64U
#define NPH_ECS_MAX_FIELDS          64U
// Longest component or field name, excluding the terminator.
#define NPH_ECS_MAX_NAME_LEN 63U

typedef struct NphComponentDesc {
    const char *name;           // unique and stable: the component's key in scene files
    u32 size;                   // 0 for a tag
    u32 align;                  // power of two, at most NPH_ECS_MAX_COMPONENT_ALIGN
    const NphFieldDesc *fields; // NULL for a tag
    u32 field_count;
    u32 flags;            // NphComponentFlags
    const void *defaults; // `size` bytes copied into a new component; NULL means all zero
} NphComponentDesc;

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

// Declares the struct `T` from the X-macro `FIELDS`. Follow with a ';'.
#define NPH_COMPONENT_FIELD_DECL_(KIND, fname, hints) NPH_FIELD_CTYPE_##KIND fname;
#define NPH_COMPONENT_STRUCT(T, FIELDS)                                                            \
    typedef struct T {                                                                             \
        FIELDS(NPH_COMPONENT_FIELD_DECL_)                                                          \
    } T

// Declares `const NphComponentDesc *T_component(void)`. Follow with a ';'.
#define NPH_COMPONENT_DECLARE(T) const NphComponentDesc *T##_component(void)
#define NPH_TAG_DECLARE(T)       NPH_COMPONENT_DECLARE(T)

// The generator for one field's metadata. The descriptor function below
// names the component type NphComponentSelf in its own scope, so the
// X-macro entries need not repeat it. (`sizeof` does not evaluate its
// operand: the null pointer is never dereferenced.)
#define NPH_COMPONENT_HINTS_(flags, lo, hi) (flags), (lo), (hi)
#define NPH_COMPONENT_FIELD_META_(KIND, fname, hints)                                              \
    {#fname, (u32)NPH_FIELD_##KIND, (u32)offsetof(NphComponentSelf, fname),                        \
     (u32)sizeof(((NphComponentSelf *)0)->fname), NPH_COMPONENT_HINTS_ hints},

// Defines `T_component()` in exactly one source file. `name` is the
// component's stable name (a string literal), `flags` its
// NphComponentFlags, and `defaults` a pointer to a `const T` holding the
// default value, or NULL for all-zero. The descriptor and its field table
// are constant-initialised statics, so the function is free and
// NPH_THREADSAFE.
#define NPH_COMPONENT_DEFINE(T, FIELDS, name_, flags_, defaults_)                                  \
    const NphComponentDesc *T##_component(void)                                                    \
    {                                                                                              \
        typedef T NphComponentSelf;                                                                \
        static const NphFieldDesc fields_[] = {FIELDS(NPH_COMPONENT_FIELD_META_)};                 \
        static const NphComponentDesc desc_ = {                                                    \
            (name_),    (u32)sizeof(NphComponentSelf), NPH_ALIGNOF(NphComponentSelf),              \
            fields_,    (u32)NPH_ARRAY_COUNT(fields_), (u32)(flags_),                              \
            (defaults_)};                                                                          \
        return &desc_;                                                                             \
    }

// Defines a tag's descriptor: a component with no data.
#define NPH_TAG_DEFINE(T, name_, flags_)                                                           \
    const NphComponentDesc *T##_component(void)                                                    \
    {                                                                                              \
        static const NphComponentDesc desc_ = {(name_), 0U, 1U, NULL, 0U, (u32)(flags_), NULL};    \
        return &desc_;                                                                             \
    }

// ---------------------------------------------------------------------------
// Descriptor validation and field access
// ---------------------------------------------------------------------------

// Checks a descriptor against every rule the world relies on: a non-empty
// name of at most NPH_ECS_MAX_NAME_LEN bytes made of [A-Za-z0-9_.]; size
// and alignment in range (0 and no fields for a tag); every field of a
// valid kind, of that kind's size, aligned for it, inside the component,
// not overlapping another field, and uniquely named; at most
// NPH_ECS_MAX_FIELDS fields. NPH_RESULT_OK or NPH_RESULT_INVALID_ARGUMENT
// with a static reason in *out_reason (may be NULL).
// Thread-safety: NPH_THREADSAFE.
NPH_MUST_USE NphResult nph_component_desc_validate(const NphComponentDesc *desc,
                                                   const char **out_reason);

// The field named `name`, or NULL.
const NphFieldDesc *nph_component_find_field(const NphComponentDesc *desc, NphStr name);

// Writes the component's default value (desc->defaults or zeros) to `dst`.
void nph_component_init_default(const NphComponentDesc *desc, void *dst);

// True when every field of `a` and `b` holds the same value. Floats are
// compared by bit pattern, so NaNs with equal bits are equal and +0/-0
// differ: the question is "would saving one or the other write the same
// file", not numeric equality. Padding is ignored. TEXT fields compare up
// to their terminator.
bool nph_component_fields_equal(const NphComponentDesc *desc, const void *a, const void *b);

// Hash of the field values (padding excluded, TEXT up to its terminator),
// for state hashes and determinism tests.
u64 nph_component_hash_fields(const NphComponentDesc *desc, const void *value, u64 seed);

// A hash of the layout: the name, size, and every field's name, kind, and
// offset. Two builds agree on a component's binary representation exactly
// when their layout hashes match; the binary scene format stores it.
u64 nph_component_layout_hash(const NphComponentDesc *desc);

// ---------------------------------------------------------------------------
// NphText helpers
// ---------------------------------------------------------------------------

// Copies `s` into `t`, truncated at a UTF-8 character boundary to fit,
// and NUL-terminates. Returns false when the text was truncated.
bool nph_text_set(NphText *t, NphStr s);

// A view of the text up to its terminator (or all NPH_TEXT_CAPACITY bytes
// when there is none).
NphStr nph_text_str(const NphText *t);

#ifdef __cplusplus
}
#endif
