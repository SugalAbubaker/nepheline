// SPDX-License-Identifier: Apache-2.0
#pragma once

// Material sources (docs/cook.md "Materials"; docs/adr/0105-materials-made-in-the-editor.md): a
// material the editor makes and edits, kept as a `.nmat` source that the cooker cooks into a
// material asset (asset/material.h), and each image it names into a texture of the usage its
// slot needs (cook_material_slot_usage).
//
// The `.nmat` file is JSON (core/json.h), written in one canonical form so it diffs cleanly in
// git:
//
//     {
//       "nmat": 1,
//       "base_color": [0.8, 0.8, 0.8, 1],
//       "metallic": 0,
//       "roughness": 0.5,
//       "emissive": [0, 0, 0],
//       "emissive_strength": 1,
//       "normal_scale": 1,
//       "occlusion_strength": 1,
//       "alpha": "opaque",
//       "alpha_cutoff": 0.5,
//       "double_sided": false,
//       "unlit": false,
//       "tiling": [1, 1],
//       "rotation": 0,
//       "offset": [0, 0],
//       "textures": {
//         "base_color": {"image": "textures/brick.png", "wrap": ["repeat", "repeat"],
//                        "filter": "linear"}
//       }
//     }
//
// Every key but "nmat" may be left out, taking the value above (cook_material_source_default);
// the writer writes every one. Colours are linear, as glTF's factors are. The texture slots are
// glTF's: "base_color", "metallic_roughness" (roughness in G, metallic in B), "normal",
// "occlusion" (R), and "emissive"; a slot left out is unused. A slot names an image by its
// source key (cook_material_image_key_valid); its "wrap" is "repeat", "clamp" or "mirror" per
// axis, and its "filter" "linear" or "nearest". Tiling, rotation (degrees) and offset move every
// texture coordinate as KHR_texture_transform does (cook_material_source_uv).
//
// Reading checks every value: the version, the keys (an unknown or repeated one is refused), the
// ranges below, and the images' keys. Malformed input gives NPH_RESULT_PARSE_ERROR
// (NPH_RESULT_VERSION_MISMATCH for another version), never a crash: the reader is a fuzz target
// (tests/fuzz).
//
// Thread-safety: pure; a source belongs to one thread.

#include <nepheline/asset/material.h>
#include <nepheline/core/string.h>

#include <cook/texture.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_MATERIAL_SOURCE_VERSION 1U
#define COOK_MATERIAL_IMAGE_MAX      512U // bytes of an image's key, its NUL included
#define COOK_MATERIAL_MAX_EMISSIVE   1.0e6F
#define COOK_MATERIAL_MAX_NORMAL     100.0F
#define COOK_MATERIAL_MIN_TILING     1.0e-4F // in size: a tiling is never zero
#define COOK_MATERIAL_MAX_TILING     1.0e4F
#define COOK_MATERIAL_MAX_ROTATION   3600.0F // degrees, either way
#define COOK_MATERIAL_MAX_OFFSET     1.0e4F

typedef struct CookMaterialTexture {
    char image[COOK_MATERIAL_IMAGE_MAX]; // its source key; empty when the slot is unused
    u8 wrap_u;                           // NphMaterialWrap
    u8 wrap_v;
    u8 nearest; // 1: nearest filtering
    u8 reserved;
} CookMaterialTexture;

typedef struct CookMaterialSource {
    NphVec4 base_color;     // linear, each 0..1
    f32 metallic;           // 0..1
    f32 roughness;          // 0..1
    NphVec3 emissive;       // linear, each 0..1
    f32 emissive_strength;  // 0..COOK_MATERIAL_MAX_EMISSIVE
    f32 normal_scale;       // -COOK_MATERIAL_MAX_NORMAL..COOK_MATERIAL_MAX_NORMAL
    f32 occlusion_strength; // 0..1
    u32 alpha;              // NphMaterialAlphaMode
    f32 alpha_cutoff;       // 0..1
    b32 double_sided;       // 0 or 1
    b32 unlit;              // 0 or 1
    NphVec2 tiling;         // each COOK_MATERIAL_MIN_TILING..COOK_MATERIAL_MAX_TILING in size
    f32 rotation;           // degrees
    NphVec2 offset;
    CookMaterialTexture textures[NPH_MATERIAL_TEXTURE_COUNT];
} CookMaterialSource;

// A new material: light grey (linear 0.8), not metallic, roughness 0.5, opaque, single-sided,
// untiled, no textures (each slot repeating and filtered linearly once it has one).
void cook_material_source_default(CookMaterialSource *out);

// True for a source key the cooker can cook as an image: relative, '/' between its parts, no
// empty, "." or ".." part, no '\\', ':' or control character, shorter than
// COOK_MATERIAL_IMAGE_MAX, with an image's extension (cooker.h COOKER_SOURCE_IMAGE).
bool cook_material_image_key_valid(NphStr key);

// Checks what the reader checks. NPH_RESULT_CORRUPT on the first problem.
NPH_MUST_USE NphResult cook_material_source_validate(const CookMaterialSource *m);

// True when the two hold the same material.
bool cook_material_source_equal(const CookMaterialSource *a, const CookMaterialSource *b);

// The usage a slot's image is cooked with: colour for base colour and emissive, linear for
// metallic-roughness, normal for normal, mask for occlusion (as the glTF importer's).
CookTextureUsage cook_material_slot_usage(u32 slot);

// The transform of its texture coordinates, composed as KHR_texture_transform's: scale, then
// rotation, then offset (the glTF importer's composition). The identity for an untiled material.
NphMaterialUvTransform cook_material_source_uv(const CookMaterialSource *m);

// The cooked header for it: its factors, alpha, flags and slots, with `textures[slot]` as the
// texture of each slot that names an image (ignored for the others). The alpha key is 0: no
// mesh's triangles for the rays depend on the material.
void cook_material_source_header(const CookMaterialSource *m,
                                 const NphGuid textures[NPH_MATERIAL_TEXTURE_COUNT],
                                 NphMaterialHeader *out);

// ---------------------------------------------------------------------------
// The `.nmat` file
// ---------------------------------------------------------------------------

// Reads a document from `text` into `out`. `alloc` holds the parse only.
NPH_MUST_USE NphResult cook_material_source_read(NphAllocator *alloc, NphStr text,
                                                 CookMaterialSource *out);
// As cook_material_source_read from the file at `path`; the file's errors too.
NPH_MUST_USE NphResult cook_material_source_load(NphAllocator *alloc, const char *path,
                                                 CookMaterialSource *out);
// Appends the document to `out`, numbers in their shortest exact form.
// NPH_RESULT_CORRUPT for a source that does not validate.
NPH_MUST_USE NphResult cook_material_source_write(const CookMaterialSource *m, NphStrBuilder *out);
// Writes the document to `path`, replacing it whole (`path` + ".part" renamed over it).
NPH_MUST_USE NphResult cook_material_source_save(const CookMaterialSource *m, NphAllocator *alloc,
                                                 const char *path);

#ifdef __cplusplus
}
#endif
