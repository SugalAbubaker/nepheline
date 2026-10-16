// SPDX-License-Identifier: Apache-2.0
#pragma once

// 128-bit identifiers (CLAUDE.md §8.8 "Asset database: 128-bit GUIDs").
//
// The type and its pure functions live in core so that every module can
// hold an asset reference: scene files store them in GUID fields
// (scene/component.h) without the scene module depending on the asset
// module, which sits beside it in the module order (CLAUDE.md §4). The
// generator seeded from the platform's clocks is asset/guid.h's.
//
// A GUID names an asset independently of its path, so references between
// cooked assets (a scene naming a mesh, a material naming a texture)
// survive renames and moves. Two ways to make one:
//   - nph_guid_from_str derives a GUID from text (a source path, a
//     sub-asset name) with two independent 64-bit hashes. The cooker uses
//     it so that cooking the same source twice yields the same identity,
//     which is what incremental cooking and diffable manifests need.
//   - nph_guid_generate draws a random one from a generator, seeded with
//     nph_guid_generator_seed or, from the clocks and the process id, with
//     nph_guid_generator_init (asset/guid.h), for assets created in the
//     editor.
// The all-zero GUID is "none" and is never produced by either.
//
// The text form is 32 lowercase hexadecimal digits, the high word first;
// parsing also accepts hyphens anywhere and upper case. Files store the
// two words little-endian (NphGuid is plain data).
//
// Thread-safety: the functions are pure and NPH_THREADSAFE, except that a
// generator belongs to one thread at a time.

#include <nepheline/core/hash.h>
#include <nepheline/core/string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphGuid {
    u64 lo;
    u64 hi;
} NphGuid;

#define NPH_GUID_NULL ((NphGuid){0U, 0U})

// Characters in the text form (without the terminator).
#define NPH_GUID_STRING_LEN 32U

NPH_STATIC_ASSERT(sizeof(NphGuid) == 16U, "a GUID is two 64-bit words");

NPH_INLINE bool nph_guid_is_null(NphGuid g)
{
    return g.lo == 0U && g.hi == 0U;
}
NPH_INLINE bool nph_guid_equals(NphGuid a, NphGuid b)
{
    return a.lo == b.lo && a.hi == b.hi;
}
// Total order (high word, then low word): <0, 0, >0.
NPH_INLINE i32 nph_guid_compare(NphGuid a, NphGuid b)
{
    if (a.hi != b.hi) {
        return a.hi < b.hi ? -1 : 1;
    }
    if (a.lo != b.lo) {
        return a.lo < b.lo ? -1 : 1;
    }
    return 0;
}
// A 64-bit hash for hash maps keyed by GUID.
NPH_INLINE u64 nph_guid_hash(NphGuid g)
{
    return nph_hash_mix_u64(g.lo ^ nph_rotl_u64(g.hi, 32U));
}

// The GUID of `s`: deterministic, never null. Equal strings give equal
// GUIDs on every platform (nph_hash64 is pinned, core/hash.h).
NphGuid nph_guid_from_str(NphStr s);
NphGuid nph_guid_from_cstr(const char *s);

// Writes the 32-character text form plus a NUL terminator into `out`.
void nph_guid_to_string(NphGuid g, char out[NPH_GUID_STRING_LEN + 1U]);

// Parses the text form: exactly 32 hexadecimal digits once hyphens are
// removed; case-insensitive. false (and *out untouched) otherwise.
bool nph_guid_parse(NphStr s, NphGuid *out);

// ---------------------------------------------------------------------------
// Random GUIDs
// ---------------------------------------------------------------------------

typedef struct NphGuidGenerator {
    u64 state;
    u64 inc;
} NphGuidGenerator;

// Seeds the generator deterministically: equal seeds give equal streams.
// asset/guid.h's nph_guid_generator_init seeds it from the clocks instead.
void nph_guid_generator_seed(NphGuidGenerator *g, u64 seed);

// The next random GUID. Never null.
NphGuid nph_guid_generate(NphGuidGenerator *g);

#ifdef __cplusplus
}
#endif
