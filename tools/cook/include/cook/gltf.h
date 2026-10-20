// SPDX-License-Identifier: Apache-2.0
#pragma once

// glTF 2.0 import (CLAUDE.md §8.8 "Meshes (glTF via cgltf)"): a .gltf or
// .glb file becomes a CookGltfScene, the cooker's intermediate form:
// meshes (CookMesh, one per glTF mesh, a submesh per primitive), materials
// (an NphMaterialHeader with image references in place of texture GUIDs),
// images (bytes still to be decoded), a node hierarchy in
// parent-before-child order, cameras, and lights (KHR_lights_punctual).
//
// What is imported:
// - triangle primitives (lists, strips, and fans are triangulated) with
//   POSITION, NORMAL, TANGENT, TEXCOORD_0, COLOR_0, in any component type
//   the specification allows (KHR_mesh_quantization included) and from
//   sparse accessors;
// - per primitive, the rules of glTF 2.0 §3.7.2.1: without NORMAL, flat
//   normals are generated and TANGENT is ignored; a mesh whose primitives
//   with texture coordinates all lack tangents gets MikkTSpace tangents
//   from the mesh pipeline, and in a mesh where only some have them the
//   others get theirs at import;
// - the metallic-roughness material with its five textures and samplers,
//   alpha mode and cutoff, double-sidedness, KHR_materials_unlit and
//   KHR_materials_emissive_strength; KHR_materials_pbrSpecularGlossiness,
//   converted to metallic-roughness (cook/specgloss.h); textures' DDS
//   sources (MSFT_texture_dds), whose fallback images are then not read;
// - of the images, those the materials use (an image no slot takes is not
//   read: it may be missing);
// - node TRS or matrices, cameras (a camera or light used by several nodes
//   is instanced at each), and the default scene (or the first scene, or
//   every root node when the document declares none).
// Skins, morph targets, animations, points and lines, TEXCOORD_1 and
// above, KHR_texture_transform, and the other extensions are skipped with
// a warning when the document merely uses them; a document that
// *requires* an extension the importer does not implement is refused
// with NPH_RESULT_UNSUPPORTED, because skipping it would change the asset.
//
// cgltf does the parsing and validation; memory goes through the caller's
// allocator and file access through the engine's files. Malformed input
// yields NPH_RESULT_PARSE_ERROR (cgltf validates the structure; the
// importer checks every index and value it uses), never a crash: the
// importer is a fuzz target (tests/fuzz/fuzz_gltf.c).
//
// Thread-safety: a scene belongs to one thread.

#include <nepheline/asset/material.h>
#include <nepheline/asset/scene.h>

#include <cook/mesh.h>
#include <cook/specgloss.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_GLTF_NAME_MAX 128U
#define COOK_GLTF_URI_MAX  1024U

typedef struct CookGltfImage {
    char name[COOK_GLTF_NAME_MAX];
    const u8 *data; // the encoded bytes (in a buffer, or owned by the scene)
    u64 size;
    u64 hash;                    // of the bytes: all a texture cooked from the image depends on
    char uri[COOK_GLTF_URI_MAX]; // the file the bytes came from, "" when embedded
} CookGltfImage;

typedef struct CookGltfMaterial {
    char name[COOK_GLTF_NAME_MAX];
    NphMaterialHeader header;              // texture GUIDs left null
    i32 image[NPH_MATERIAL_TEXTURE_COUNT]; // image index per slot, -1 when unused
    // KHR_materials_pbrSpecularGlossiness with textures: the base colour and
    // metallic-roughness textures are made, when the textures are cooked,
    // from the diffuse and specular-glossiness images (`specgloss_image`,
    // -1 for one the material lacks) and the factors (cook/specgloss.h),
    // which the textures then carry (the header's factors are one); `image`
    // leaves those two slots -1. A material with the factors alone is
    // converted on import and has `specgloss` false.
    b32 specgloss;
    i32 specgloss_image[2]; // diffuse, specular-glossiness
    CookSpecGloss specgloss_factors;
} CookGltfMaterial;

typedef struct CookGltfMesh {
    CookMesh mesh;  // submesh material slots index `materials`
    i32 *materials; // glTF material index per slot, -1 for the default material
    u32 material_count;
    u32 material_capacity; // the allocation (one slot per primitive at most)
} CookGltfMesh;

typedef struct CookGltfNode {
    char name[COOK_GLTF_NAME_MAX];
    u32 parent; // NPH_SCENE_NO_NODE for a root
    NphVec3 translation;
    NphQuat rotation;
    NphVec3 scale;
    i32 mesh;   // index into meshes, -1 none
    i32 camera; // index into cameras, -1 none
    i32 light;  // index into lights, -1 none
} CookGltfNode;

// A block the scene owns: decoded data URIs and image files read from
// disk, released with the scene.
typedef struct CookGltfOwned {
    void *data;
    u64 size;
    b32 from_file; // nph_file_read_all's block (size + 1 bytes), else cgltf's
} CookGltfOwned;

typedef struct CookGltfScene {
    NphAllocator *alloc;
    CookGltfMesh *meshes;
    u32 mesh_count;
    CookGltfMaterial *materials;
    u32 material_count;
    CookGltfImage *images;
    u32 image_count;
    CookGltfNode *nodes; // parent-before-child
    u32 node_count;
    NphSceneCamera *cameras; // node fields hold the cooked node index
    u32 camera_count;
    NphSceneLight *lights;
    u32 light_count;
    u64 content_hash;       // the document's bytes, every buffer's, and every image's
    u32 warnings;           // skipped features, logged as they are met
    void *cgltf;            // the parsed document (keeps the buffers alive)
    CookGltfOwned document; // the document's bytes: a GLB's binary chunk is used in place
    CookGltfOwned *owned;   // one per image at most
    u32 owned_count;
    u32 capacity_meshes; // allocation sizes, for the release
    u32 capacity_materials;
    u32 capacity_images;
    u32 capacity_nodes;
} CookGltfScene;

// Parses `path` (.gltf with external or embedded buffers, or .glb) and
// loads its buffers and images. NPH_RESULT_NOT_FOUND for a missing file,
// NPH_RESULT_PARSE_ERROR for an invalid document, NPH_RESULT_IO_ERROR
// for an unreadable buffer or image file, NPH_RESULT_UNSUPPORTED for a
// required extension the importer does not implement.
NPH_MUST_USE NphResult cook_gltf_load(NphAllocator *alloc, const char *path, CookGltfScene *out);

// As cook_gltf_load from memory; `base_dir` resolves relative URIs
// (may be NULL: only embedded data then).
NPH_MUST_USE NphResult cook_gltf_load_memory(NphAllocator *alloc, const void *data, u64 size,
                                             const char *base_dir, CookGltfScene *out);

// Releases the scene. Safe on a zero-filled scene and twice.
void cook_gltf_free(CookGltfScene *scene);

// Receives one file a document names (cook_gltf_files).
typedef void (*CookGltfFileFn)(void *user, const char *relative_path);

// The files a document at `path` names besides itself, which a copy of the
// document needs beside it: the URIs of its buffers and images that are
// files (data: URIs and a GLB's binary chunk name none), percent-decoded
// and normalised (asset/vfs.h nph_vfs_normalize_path), relative to the
// document's directory with '/' separators. `fn` receives each once,
// buffers first, in the document's order. Only the document is read; the
// files it names need not exist. NPH_RESULT_UNSUPPORTED for a URI with a
// scheme ("https://"), an absolute path, or one that climbs out of the
// document's directory, which a copy could not keep beside it;
// NPH_RESULT_NOT_FOUND for a missing document; NPH_RESULT_PARSE_ERROR for
// one that does not parse.
NPH_MUST_USE NphResult cook_gltf_files(NphAllocator *alloc, const char *path, CookGltfFileFn fn,
                                       void *user);

#ifdef __cplusplus
}
#endif
