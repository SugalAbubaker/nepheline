// SPDX-License-Identifier: Apache-2.0
#pragma once

// The scene renderer: the bridge from an ECS world to the renderer
// (docs/runtime.md "From the world to the renderer").
//
// Once per frame, at the frame's sync point, nph_scene_renderer_sync makes
// the renderer show what the world holds:
//   - every entity with a MeshRenderer and a WorldTransform gets a render
//     instance of its mesh with its materials, once those have loaded;
//   - every entity with a Light and a WorldTransform gets a render light at
//     its position, pointing along its -Z axis;
//   - the first Environment sets the sky (a world without one gets the
//     component's defaults, a clear day), and the first active Camera is
//     the view (nph_scene_renderer_view).
// Only what changed is touched: entities in chunks whose MeshRenderer,
// Light, or WorldTransform columns changed since the previous sync (the
// world's change ticks, scene/query.h), and the despawns and removals the
// world's event stream reports. A static world costs one version check per
// chunk.
//
// Assets are cached by GUID and shared: meshes, materials, and the
// textures materials name are loaded asynchronously through the asset
// loader, turned into render resources when they are ready, and destroyed
// when the last entity using them goes. An entity waits, without an
// instance, until its mesh and materials are ready; a mesh that fails to
// load leaves the entity undrawn (counted, logged once per asset), and a
// material that fails draws with the default material. A MeshRenderer
// that names no mesh draws nothing and is not a failure. A frame never
// waits for a load (§10.5 "Streaming must never hitch a frame") unless
// the caller asks (nph_scene_renderer_wait).
//
// Requirements on the world: the runtime's components are registered
// (components.h) and the world has an event stream (NphWorldDesc.
// event_capacity). The sync reads the events recorded since the world's
// last clear and does not clear them: the world's owner clears them once
// every reader has seen them (the engine does, at the end of the frame's
// sync point). When the stream overflowed, the sync re-validates every
// entity it mirrors.
//
// Costs (§1.4): create allocates the asset cache and an entity table the
// size of the world's capacity; render resources, asset loads, and the
// pending list allocate as they grow.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY, like the renderer. The sync reads
// the world while nothing else changes it (the frame's sync point).

#include <nepheline/asset/loader.h>
#include <nepheline/render/renderer.h>
#include <nepheline/runtime/components.h>
#include <nepheline/scene/transform.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphSceneRenderer NphSceneRenderer;

typedef struct NphSceneRendererDesc {
    NphAllocator *alloc;    // required
    NphRenderer *renderer;  // required
    NphAssetLoader *loader; // required, with a job system (loads are asynchronous)
    NphWorld *world;        // required: the world to mirror
    u32 max_assets;         // meshes, materials, and textures cached at once (0: 4096)
    f32 emissive_scale;     // cd/m^2 per unit of glTF emissive (0: 1)
} NphSceneRendererDesc;

typedef struct NphSceneRenderStats {
    u32 entities;       // entities mirrored (with a MeshRenderer or a Light)
    u32 instances;      // render instances alive
    u32 pending;        // entities waiting for their assets
    u32 failed;         // entities whose mesh failed to load
    u32 lights;         // render lights alive
    u32 assets_loading; // cached assets not ready yet
    u32 assets_ready;
    u32 assets_failed;
    u32 created;   // instances created by the last sync
    u32 destroyed; // instances destroyed by the last sync
    u32 moved;     // instances whose transform the last sync wrote
} NphSceneRenderStats;

// NPH_RESULT_INVALID_ARGUMENT for a missing field, a world without the
// runtime's components or without an event stream, or a loader without a
// job system; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_scene_renderer_create(const NphSceneRendererDesc *desc,
                                                 NphSceneRenderer **out);

// Destroys every instance, light, and render resource it created and
// releases its assets (after waiting for the loads in flight). Safe on
// NULL.
void nph_scene_renderer_destroy(NphSceneRenderer *sr);

// Mirrors another world from now on (play-in-editor switches between the
// edited world and its play copy): every instance and light of the old
// world is destroyed, and the next sync builds the new world's. The old
// world's assets are held until that sync has taken the new world's, so
// the assets both use are neither unloaded nor reloaded. The same
// requirements apply as at creation; on failure the scene renderer
// mirrors no world and must be given one before its next sync.
NPH_MUST_USE NphResult nph_scene_renderer_set_world(NphSceneRenderer *sr, NphWorld *world);

NphWorld *nph_scene_renderer_world(const NphSceneRenderer *sr);

// Brings the renderer up to date with the world (see above). Returns the
// first failure to create a render resource for lack of capacity
// (NPH_RESULT_FULL from the renderer's pools) or memory; content errors
// (a mesh that does not load, a degenerate transform) are logged and
// counted, not returned. `out` may be NULL.
NPH_MUST_USE NphResult nph_scene_renderer_sync(NphSceneRenderer *sr, NphSceneRenderStats *out);

// Blocks until every asset in flight has loaded or failed (tests, loading
// screens). The next sync then creates whatever was waiting.
void nph_scene_renderer_wait(NphSceneRenderer *sr);

// Fills `view` from the world's first active Camera (by entity slot) as of
// the last sync: its position, orientation, field of view, near plane,
// exposure, and tonemapper; other fields keep their values. False (and
// `view` untouched) when the world has no active camera.
bool nph_scene_renderer_view(const NphSceneRenderer *sr, NphRenderView *view);

// The loaded mesh asset an entity draws, for picking and bounds (READY
// assets only; valid until the entity's mesh changes or it goes), or NULL.
const NphAsset *nph_scene_renderer_mesh_asset(const NphSceneRenderer *sr, NphEntity entity);

// The render instance of an entity, or a null handle while it has none.
NphRenderInstance nph_scene_renderer_instance(const NphSceneRenderer *sr, NphEntity entity);

// ---------------------------------------------------------------------------
// Importing cooked scenes
// ---------------------------------------------------------------------------

typedef struct NphSceneImportDesc {
    NphWorld *world;                  // required, with the runtime's components
    NphAssetLoader *loader;           // required
    NphGuid scene;                    // the cooked scene asset (asset/scene.h)
    NphEntity parent;                 // the new root's parent (null: a root)
    NphLocalTransform root_transform; // the new root's local transform
    const char *name;                 // the new root's Name (NULL: "scene")
    NphAllocator *scratch;            // temporary tables (NULL: the system allocator)
} NphSceneImportDesc;

// Spawns a cooked scene into the world: one root entity named `name`, and
// under it one entity per node of the scene with its Name and
// LocalTransform in the scene's hierarchy, a MeshRenderer for each mesh
// instance (the mesh and its materials by GUID), a Light for each light,
// and a Camera for each perspective camera (inactive: the importer does
// not take the view). A node holding more than one instance, light, or
// camera gets one child entity per extra one. Loads the scene asset
// synchronously (it blocks) and releases it before returning; the meshes
// and materials load when a scene renderer draws them. `out_root` and
// `out_count` (entities created, the root included) may be NULL.
// NPH_RESULT_NOT_FOUND for an unknown scene, the loader's errors, the
// world's; all or nothing.
NPH_MUST_USE NphResult nph_scene_import_cooked(const NphSceneImportDesc *desc, NphEntity *out_root,
                                               u32 *out_count);

#ifdef __cplusplus
}
#endif
