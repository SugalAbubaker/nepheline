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
// transform (UVTR, NphMaterialUvTransform; ADR-0105). 4: a slot may read the second texture
// coordinate set (NphMaterialTextureRef.texcoord 1; ADR-0107). 5: its optics (OPTC,
// NphMaterialOptics; ADR-0107). Versions 1 to 4 still read: they read the first set alone
// before 4, have F0 0.04 and no tint, no transform before 3, and version 1 no key.
#define NPH_MATERIAL_TYPE_VERSION 5U
#define NPH_MATERIAL_TAG_HEADER   NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_MATERIAL_TAG_UV       NPH_FOURCC('U', 'V', 'T', 'R')
#define NPH_MATERIAL_TAG_OPTICS   NPH_FOURCC('O', 'P', 'T', 'C')

// The reflectance head on of a material's dielectric part where nothing says otherwise: glTF's,
// an index of refraction of 1.5.
#define NPH_MATERIAL_F0_DEFAULT 0.04F

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
    // it (but for NPH_MATERIAL_FLAG_TRANSMISSION_TEXELS); what it reflects is not
    // (render/renderer.h NphRenderMaterialDesc.transmissive).
    NPH_MATERIAL_FLAG_TRANSMISSIVE = 1U << 2U,
    // Glass whose transmission varies per texel, from version 5 (KHR_materials_transmission's
    // texture; ADR-0107): its base colour texture's alpha holds each texel's share, and its
    // colour is not scaled by it ahead: the renderer scales it texel by texel
    // (NphRenderMaterialDesc.transmission_texels). Glass's alone.
    NPH_MATERIAL_FLAG_TRANSMISSION_TEXELS = 1U << 3U,
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
    // The texture coordinates it reads: 0, or from version 4 1, the second set (a mesh's UV1,
    // asset/mesh.h; one without reads its first). The transform (UVTR) applies to either.
    u32 texcoord;
    u8 wrap_u; // NphMaterialWrap
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

// Version 5, optional (OPTC; ADR-0107): what the material's surface reflects head on, and what
// it lets through when it is glass (NPH_MATERIAL_FLAG_TRANSMISSIVE). A material without it, and
// every material before version 5, has F0 NPH_MATERIAL_F0_DEFAULT and no tint.
typedef struct NphMaterialOptics {
    // The dielectric part's reflectance head on (F0), [0, 1]: KHR_materials_ior's
    // ((ior - 1) / (ior + 1))^2 times KHR_materials_specular's factor.
    f32 f0;
    // Glass's tint, per channel, before the vertex colour and the base colour texture tint it as
    // they do its base colour: its base colour factor as authored (the header's is scaled by the
    // share of the light the glass keeps: docs/cook.md "Glass") times KHR_materials_volume's
    // attenuation. Non-negative and finite; (0, 0, 0): untinted, and on any material not glass.
    NphVec3 transmission;
} NphMaterialOptics;

NPH_STATIC_ASSERT(sizeof(NphMaterialOptics) == 16U, "the optics are 16 bytes");

typedef struct NphMaterialView {
    const NphMaterialHeader *header;
    const NphMaterialUvTransform *uv; // NULL: the identity
    const NphMaterialOptics *optics;  // NULL: NPH_MATERIAL_F0_DEFAULT, untinted
} NphMaterialView;

NPH_MUST_USE NphResult nph_material_view(const NphAsset *asset, NphMaterialView *out);

// The identity transform.
NphMaterialUvTransform nph_material_uv_identity(void);
// The view's transform: its UVTR blob, or the identity.
NphMaterialUvTransform nph_material_view_uv(const NphMaterialView *view);

// The optics of a material without OPTC: F0 NPH_MATERIAL_F0_DEFAULT, untinted.
NphMaterialOptics nph_material_optics_default(void);
// The view's optics: its OPTC blob, or the default.
NphMaterialOptics nph_material_view_optics(const NphMaterialView *view);
// Whether `o` is the default (the cooker then writes no OPTC).
bool nph_material_optics_is_default(const NphMaterialOptics *o);
// True when `t` is the identity, exactly.
bool nph_material_uv_is_identity(const NphMaterialUvTransform *t);

// A header with glTF's defaults: white, fully metallic and fully rough
// (glTF's metallic and roughness factors default to 1), opaque, no
// textures.
void nph_material_header_default(NphMaterialHeader *out);

#ifdef __cplusplus
}
#endif
