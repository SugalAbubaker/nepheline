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

// 2: the alpha test's key (NphMaterialHeader.alpha_key). 3: the texture coordinates'
// transform (UVTR, NphMaterialUvTransform; ADR-0105). Versions 1 and 2 still read: they have no
// transform, and version 1 no key.
#define NPH_MATERIAL_TYPE_VERSION 3U
#define NPH_MATERIAL_TAG_HEADER   NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_MATERIAL_TAG_UV       NPH_FOURCC('U', 'V', 'T', 'R')

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

// Version 3, optional (UVTR): the transform every texture coordinate of the material goes through
// before any of its textures is sampled, u' = m00 u + m01 v + m02 and v' = m10 u + m11 v + m12.
// The cooker composes it as KHR_texture_transform does: scale, then rotation, then offset
// (cook/material.h). Every value is finite and the linear part is not singular. A material
// without it, and every version 1 and 2 material, has the identity.
typedef struct NphMaterialUvTransform {
    f32 m00, m01, m02;
    f32 m10, m11, m12;
} NphMaterialUvTransform;

NPH_STATIC_ASSERT(sizeof(NphMaterialUvTransform) == 24U, "a UV transform is 24 bytes");

typedef struct NphMaterialView {
    const NphMaterialHeader *header;
    const NphMaterialUvTransform *uv; // NULL: the identity
} NphMaterialView;

NPH_MUST_USE NphResult nph_material_view(const NphAsset *asset, NphMaterialView *out);

// The identity transform.
NphMaterialUvTransform nph_material_uv_identity(void);
// The view's transform: its UVTR blob, or the identity.
NphMaterialUvTransform nph_material_view_uv(const NphMaterialView *view);
// True when `t` is the identity, exactly.
bool nph_material_uv_is_identity(const NphMaterialUvTransform *t);

// A header with glTF's defaults: white, fully metallic and fully rough
// (glTF's metallic and roughness factors default to 1), opaque, no
// textures.
void nph_material_header_default(NphMaterialHeader *out);

#ifdef __cplusplus
}
#endif
