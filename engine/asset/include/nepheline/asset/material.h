// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cooked materials (CLAUDE.md §8.8 "Materials"): the metallic-roughness
// model of glTF 2.0 (and of the §8.6 reference renderer), as one header
// blob (HEAD, NphMaterialHeader). Textures are referenced by GUID; the
// loader resolves them through nph_asset_find_guid after the referenced
// texture assets are loaded (the scene lists them).
//
// Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/asset/loader.h>
#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

// 2: the alpha test's key (NphMaterialHeader.alpha_key). Version 1 still reads: it has none.
#define NPH_MATERIAL_TYPE_VERSION 2U
#define NPH_MATERIAL_TAG_HEADER   NPH_FOURCC('H', 'E', 'A', 'D')

typedef enum NphMaterialAlphaMode {
    NPH_MATERIAL_ALPHA_OPAQUE = 0,
    NPH_MATERIAL_ALPHA_MASK = 1,
    NPH_MATERIAL_ALPHA_BLEND = 2,
} NphMaterialAlphaMode;

typedef enum NphMaterialFlags {
    NPH_MATERIAL_FLAG_DOUBLE_SIDED = 1U << 0U,
    NPH_MATERIAL_FLAG_UNLIT = 1U << 1U,
    // Glass: a blended material (NPH_MATERIAL_ALPHA_BLEND) whose base colour's alpha is the
    // share of the light behind it that its body stops, and whose colour is already scaled by
    // it; what it reflects is not (render/renderer.h NphRenderMaterialDesc.transmissive).
    NPH_MATERIAL_FLAG_TRANSMISSIVE = 1U << 2U,
} NphMaterialFlags;

// The texture slots, in NphMaterialHeader.textures order.
typedef enum NphMaterialTextureSlot {
    NPH_MATERIAL_TEXTURE_BASE_COLOR = 0,
    NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS, // glTF: roughness in G, metallic in B
    NPH_MATERIAL_TEXTURE_NORMAL,
    NPH_MATERIAL_TEXTURE_OCCLUSION, // R
    NPH_MATERIAL_TEXTURE_EMISSIVE,
    NPH_MATERIAL_TEXTURE_COUNT
} NphMaterialTextureSlot;

typedef enum NphMaterialWrap {
    NPH_MATERIAL_WRAP_REPEAT = 0,
    NPH_MATERIAL_WRAP_CLAMP = 1,
    NPH_MATERIAL_WRAP_MIRROR = 2,
} NphMaterialWrap;

typedef struct NphMaterialTextureRef {
    NphGuid texture; // null when the slot is unused
    u32 texcoord;    // which UV set (0 in this version)
    u8 wrap_u;       // NphMaterialWrap
    u8 wrap_v;
    u8 nearest; // 1: nearest filtering
    u8 reserved;
    f32 scale; // normal scale, occlusion strength; 1 otherwise
    u32 reserved2;
} NphMaterialTextureRef;

NPH_STATIC_ASSERT(sizeof(NphMaterialTextureRef) == 32U, "a texture reference is 32 bytes");

typedef struct NphMaterialHeader {
    NphVec4 base_color_factor;
    NphVec3 emissive_factor;
    f32 emissive_strength;
    f32 metallic_factor;
    f32 roughness_factor;
    f32 alpha_cutoff;
    u32 alpha_mode; // NphMaterialAlphaMode
    u32 flags;      // NphMaterialFlags
    u32 reserved;   // zero
    // Version 2: what an alpha-tested material's test reads, as one number: the cooker's hash of
    // its base colour texture's alpha at the finest level (as cooked), that texture's wrap modes
    // and filter, the base colour factor's alpha and the cutoff; 0 where the cooker made none
    // (not alpha tested, no base colour texture, or triangles trimmed for the rays switched
    // off). A mesh's triangles for the rays alone hold for a material of the same key
    // (asset/mesh.h NphMeshRayRange.alpha_key; ADR-0096).
    u64 alpha_key;
    NphMaterialTextureRef textures[NPH_MATERIAL_TEXTURE_COUNT];
} NphMaterialHeader;

NPH_STATIC_ASSERT(sizeof(NphMaterialHeader) == 224U, "the material header is 224 bytes");

typedef struct NphMaterialView {
    const NphMaterialHeader *header;
} NphMaterialView;

NPH_MUST_USE NphResult nph_material_view(const NphAsset *asset, NphMaterialView *out);

// A header with glTF's defaults: white, non-metallic, fully rough, opaque,
// no textures.
void nph_material_header_default(NphMaterialHeader *out);

#ifdef __cplusplus
}
#endif
