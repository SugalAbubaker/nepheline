// SPDX-License-Identifier: Apache-2.0
#pragma once

// Loads a cooked scene (engine/asset scene.h) and everything it references
// into a renderer (engine/render renderer.h): the bridge between the asset
// module and the render module, which the module dependency rule keeps
// apart (CLAUDE.md §4: render sits left of asset). The samples and the
// golden-image tests share it; the runtime module (Phase 9) takes it over.
//
// What it does, in the order a game streams a level:
//   1. loads the scene by GUID through the asset loader, then the meshes
//      and materials it names, then the textures those materials name,
//      each batch in flight together on the job system;
//   2. creates a render texture per texture asset (block-compressed
//      levels uploaded as they are), a render mesh per mesh asset (the
//      cooked streams handed over unchanged), a render material per
//      material asset (the scene's material table names one per instance
//      slot, so instances share them: Bistro's 2,909 slots name 254), an
//      instance per mesh instance at its node's world transform, and a
//      light per KHR_lights_punctual light; where the scene has the world
//      partition's cells (asset/scene.h version 2), a render cell per cell
//      with a proxy, first (its proxy a mesh asset of voxels alone, its
//      material the first instance's first; ADR-0062), which the cell's
//      instances name.
//
// Units: glTF lights are already lux and candela; emissive is
// emissiveFactor x KHR_materials_emissive_strength x `emissive_scale`,
// read as cd/m^2.
//
// Costs: allocates its handle tables from `alloc`; the loader and the
// renderer allocate as their documentation says. NPH_MAIN_THREAD_ONLY.

#include <nepheline/asset/loader.h>
#include <nepheline/core/guid.h>
#include <nepheline/render/renderer.h>

typedef struct CookedSceneDesc {
    NphAllocator *alloc;    // required
    NphRenderer *renderer;  // required
    NphAssetLoader *loader; // required, with a job system
    NphJobSystem *jobs;     // required: the loader's
    NphGuid scene;          // the cooked scene's GUID
    NphDVec3 origin;        // added to every node position (large-world tests)
    f32 emissive_scale;     // cd/m^2 per unit of glTF emissive (0: 1)
    // Without the world partition's cells (asset/scene.h version 2): every
    // instance drawn as itself, no proxy loaded (an A/B of the cells).
    b32 no_cells;
} CookedSceneDesc;

typedef struct CookedScene {
    NphAllocator *alloc;
    NphRenderer *renderer;
    NphAssetLoader *loader;
    NphAssetHandle *assets; // every asset loaded, released by unload
    NphGuid *guids;
    u32 asset_count;
    u32 asset_cap;
    NphRenderTexture *textures;         // parallel to `assets` (null for non-textures)
    NphRenderMesh *meshes;              // parallel to `assets`
    NphRenderMaterial *asset_materials; // parallel to `assets`: the materials, owned
    u32 render_material_count;          // how many of those were created
    NphRenderMaterial *materials;       // per entry of the scene's material table
    u32 material_count;                 // (instance slots), sharing asset_materials
    u32 material_cap;
    NphRenderInstance *instances;
    u32 instance_count;
    u32 instance_cap;
    // The world partition's cells, per cell of the scene (null where the cell
    // has no proxy, or without them), made before the instances that name
    // them (ADR-0062).
    NphRenderCell *cells;
    u32 cell_count; // made so far
    u32 cell_cap;
    u32 proxy_count; // of them, with a proxy
    NphRenderLight *lights;
    NphRenderLightDesc *light_descs; // as created (for gizmos and tests)
    u32 light_count;
    u32 light_cap;
    NphAABB bounds; // world bounds of the instances, relative to `origin`
    // The scene's first camera, if any (else has_camera is 0).
    b32 has_camera;
    NphRenderCamera camera;
} CookedScene;

// Loads the scene and creates its render resources. On failure everything
// created so far is released and the error returned (logged).
NPH_MUST_USE NphResult cooked_scene_load(const CookedSceneDesc *desc, CookedScene *out);

// Destroys the render resources and releases the assets. Safe on a zeroed
// scene.
void cooked_scene_unload(CookedScene *scene);
