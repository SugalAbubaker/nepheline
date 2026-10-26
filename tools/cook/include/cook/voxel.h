// SPDX-License-Identifier: Apache-2.0
#pragma once

// The materials the cooker voxelizes aggregate geometry with (cook/mesh.h
// step 8; render/voxel.h; docs/render.md "Voxels"; ADR-0061): a mesh's
// material slots as the voxelizer's sampler reads them, from the glTF
// materials' factors and their decoded images.
//
// A material's images are kept as RGBA8 mip chains of at most
// COOK_VOXEL_IMAGE_MAX texels a side, box-filtered (the base colour's in
// linear light, stored sRGB-encoded): a voxel's look averages its cell's
// hits, so texture detail finer than the cell's rays resolve is averaged
// away whatever the source's resolution. A lookup filters trilinearly at
// the level the sampler's footprint asks for, through the triangle's
// texel density (its texture area over its surface area), as the GPU's
// texture LOD does from screen-space derivatives (Akenine-Möller et al.,
// "Texture Level of Detail Strategies for Real-Time Ray Tracing", Ray
// Tracing Gems, 2019, with the footprint as the cone's width).
//
// Thread-safety: images and materials are immutable once made; lookups and
// the sampler's functions are NPH_THREADSAFE.

#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/render/voxel.h>

#include <cook/image.h>
#include <cook/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_VOXEL_IMAGE_MAX    1024U
#define COOK_VOXEL_IMAGE_LEVELS 11U // 1024 texels to one

typedef struct CookVoxelImage {
    NphAllocator *alloc;
    u32 width; // level 0's
    u32 height;
    u32 levels;
    b32 srgb;   // the texels are sRGB-encoded (decoded by lookups)
    u8 *texels; // every level's RGBA8 rows, top row first, level 0 first
    u64 offset[COOK_VOXEL_IMAGE_LEVELS];
    u64 size;
    f32 decode[256]; // a byte's value in linear light (sRGB-decoded where `srgb`)
} CookVoxelImage;

// The mip chain of `image` (RGBA8, or RGBA32F taken as linear and clamped to
// [0, 1]), level 0 the image box-filtered down until neither side passes
// COOK_VOXEL_IMAGE_MAX; with `srgb` the texels are sRGB-encoded and filtered
// in linear light. NPH_RESULT_INVALID_ARGUMENT for an empty image.
NPH_MUST_USE NphResult cook_voxel_image_init(CookVoxelImage *out, NphAllocator *alloc,
                                             const CookImage *image, b32 srgb);
// Releases the texels. Safe on a zero-filled image.
void cook_voxel_image_free(CookVoxelImage *img);

// The image's RGBA (linear for an sRGB image) at `uv`, filtered trilinearly
// over `footprint` in texture coordinates (a share of the image across),
// with the wrap modes (NphMaterialWrap) on each axis.
NphVec4 cook_voxel_image_sample(const CookVoxelImage *img, NphVec2 uv, f32 footprint, u32 wrap_u,
                                u32 wrap_v);

// A texture slot of a voxel material.
typedef struct CookVoxelSlot {
    const CookVoxelImage *image; // NULL: the slot has none
    u32 wrap_u;                  // NphMaterialWrap
    u32 wrap_v;
} CookVoxelSlot;

typedef struct CookVoxelMaterial {
    u8 surface;         // NphRenderVoxelSurface
    NphVec4 base_color; // factor, linear RGBA
    f32 alpha_cutoff;
    f32 metallic;
    f32 roughness;
    f32 normal_scale;
    f32 occlusion_strength;
    CookVoxelSlot base_color_image;         // sRGB
    CookVoxelSlot metallic_roughness_image; // roughness in G, metallic in B
    CookVoxelSlot normal_image;             // tangent space, glTF's convention
    CookVoxelSlot occlusion_image;          // R
} CookVoxelMaterial;

// A material's factors and surface from its header, with no images (the
// caller sets the slots): blended, emissive, and unlit materials never
// voxelize (NPH_RENDER_VOXEL_SURFACE_NONE: a voxel is lit and opaque),
// masked ones cut out by alpha against their cutoff.
void cook_voxel_material_init(CookVoxelMaterial *out, const NphMaterialHeader *h);

// A mesh's materials, one per material slot of its submeshes.
typedef struct CookVoxelMaterials {
    const CookVoxelMaterial *slots;
    u32 slot_count;
} CookVoxelMaterials;

// The voxelizer's sampler over a quantised mesh (the positions and
// attributes the levels are built from, cook/mesh.h step 8): `surfaces`
// (one per submesh, the caller's) takes each submesh's surface, a slot past
// the materials' none.
typedef struct CookVoxelSampler {
    const CookMesh *mesh;
    const CookVoxelMaterials *materials;
    const NphVec3 *positions;
    const NphMeshAttrib *attribs;
} CookVoxelSampler;

void cook_voxel_sampler_init(CookVoxelSampler *s, const CookMesh *mesh,
                             const CookVoxelMaterials *materials, const NphVec3 *positions,
                             const NphMeshAttrib *attribs, u8 *surfaces,
                             NphRenderVoxelSampler *out);

#ifdef __cplusplus
}
#endif
