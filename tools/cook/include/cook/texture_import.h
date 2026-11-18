// SPDX-License-Identifier: Apache-2.0
#pragma once

// Import settings for images (docs/cook.md "Texture import settings";
// docs/adr/0105-materials-made-in-the-editor.md): a JSON file beside an image,
// `<image>.import.json`, that the cooker reads when it cooks the image alone
// (cooker_cook_image) or for a material (cook/material.h). Its bytes join the
// image's content hash, so a change cooks the image again.
//
//     {"texture": 1, "usage": "normal", "max_extent": 2048, "uncompressed": false,
//      "mips": true, "normal_green": "down"}
//
// Every key but "texture" may be left out; the cooker's setting then holds.
// "usage" is the image's data kind when it is cooked alone ("color", "linear",
// "normal", "mask", "hdr"; texture.h CookTextureUsage); in a material the
// slot says it. "max_extent" caps the largest side (0: no cap, over the
// cooker's). "normal_green" is a normal map's green convention: "up" (OpenGL's
// and glTF's, the renderer's) or "down" (DirectX's), whose green the cooker
// flips. The writer writes what is set, in this order; "normal_green" only
// when it is "down".
//
// Reading is strict: another version gives NPH_RESULT_VERSION_MISMATCH, an
// unknown or repeated key or a value out of range NPH_RESULT_PARSE_ERROR,
// never a crash: the reader is a fuzz target (tests/fuzz).
//
// Thread-safety: pure.

#include <nepheline/core/string.h>

#include <cook/texture.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_TEXTURE_IMPORT_VERSION    1U
#define COOK_TEXTURE_IMPORT_MAX_EXTENT 16384U

typedef struct CookTextureImport {
    b32 has_usage;
    CookTextureUsage usage;
    b32 has_max_extent;
    u32 max_extent; // 0..COOK_TEXTURE_IMPORT_MAX_EXTENT; 0: no cap
    b32 has_uncompressed;
    b32 uncompressed;
    b32 has_mips;
    b32 mips;
    b32 green_down; // a normal map in DirectX's convention: its green flipped when cooked
} CookTextureImport;

// Nothing set: the cooker's settings, green up.
void cook_texture_import_default(CookTextureImport *out);

// True when the two say the same.
bool cook_texture_import_equal(const CookTextureImport *a, const CookTextureImport *b);

// `base` with the settings over it (the usage too, when `alone` and it is set).
CookTextureSettings cook_texture_import_apply(const CookTextureImport *s,
                                              const CookTextureSettings *base, bool alone);

// Reads settings from `text` into `out`. `alloc` holds the parse only.
NPH_MUST_USE NphResult cook_texture_import_read(NphAllocator *alloc, NphStr text,
                                                CookTextureImport *out);
// Appends the settings to `out`. NPH_RESULT_CORRUPT for settings out of range.
NPH_MUST_USE NphResult cook_texture_import_write(const CookTextureImport *s, NphStrBuilder *out);
// Writes the settings to `path`, replacing it whole (`path` + ".part" renamed
// over it).
NPH_MUST_USE NphResult cook_texture_import_save(const CookTextureImport *s, NphAllocator *alloc,
                                                const char *path);

#ifdef __cplusplus
}
#endif
