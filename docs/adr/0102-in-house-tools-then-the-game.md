# 0102. Phase 15 builds the tools to make the game's world in-house; Phase 16 is the game

- **Date:** 2026-11-17
- **Status:** Accepted (the user's decisions of 2026-11-17, below, and their acceptance the same day; amends
  [ADR-0067](0067-the-game-is-a-city.md)'s decisions 4, 5 and 7, [ADR-0082](0082-gate-at-1440p.md) for Phase 15,
  [ADR-0099](0099-two-views-over-the-gate.md)'s decision 4 and [ADR-0101](0101-phase-14-exit.md)'s decision 3;
  CLAUDE.md §8.13, §10.1, §10.7, §12 and §13 amended)

## Context

CLAUDE.md §12 gave Phase 15 to production hardening: shipping builds, packaging, crash reporting, soak tests, docs
completion and the editor's modeling tools v2. It was to end when Bistro, the city and the interior hold 1%-low ≤ 6.94 ms
at native 1440p in shipping builds, with the release candidate `v1.0.0`. The first game came after it (ADR-0067).

Before Phase 15 started, on 2026-11-17, the user said what they need from it instead:

> For phase 15 I'm not going to be ready to ship the engine. What I want from phase 15 is to have all the tools I need
> to start to build the environment of my game. So I need a editor that I can use to make models/textures/objects or
> bring these assets in from other software like blender. I will also need the physics to work and to add audio to the
> game. Basically I need it to be ready for my personal in-house production and we don't need to worry about a public
> release right now. Once I start building the game phase 16 will be the final optimization to get the game to 240 fps
> 4k native and adding features such as character models, animations, NPC AI, etc.

**What the editor and its pipeline do today, against that** (docs/editor.md, cook.md and runtime.md;
`runtime/components.h`):

| To build the world | Today | Missing |
|---|---|---|
| Bring assets from Blender | A `.gltf` or `.glb` dropped on the editor is copied, cooked and placed: its meshes, metallic-roughness materials (specular-glossiness converted, unlit, emissive strength), their textures, punctual lights, cameras and node hierarchy (ADR-0026). An edited file dropped again is drawn anew wherever it is used (ADR-0027) | Collision from the models; `KHR_materials_transmission` and the other material extensions (skipped with a warning); `TEXCOORD_1`; a re-import when the source changes on disk; import settings per asset (a WAV has its `.import.json`, a glTF none) |
| Make models | Primitives (cube, sphere, plane, cylinder); vertices and faces moved, turned, scaled and extruded, with a history of its own (ADR-0027). The `.nmesh` source holds a material slot per face and a UV per corner | Edges; delete, merge, split; bevel, inset, loop cut, mirror; materials per face from the editor; UVs; the preview draws only the default material |
| Make materials and textures | Materials come only inside glTF documents, assigned by GUID (`MeshRenderer`, `MeshMaterials`) | A material the editor makes and edits; textures made or painted in it |
| Make and place objects | Entities and components through the outliner and the reflection-driven inspector; duplicate, reparent, gizmos with snapping, undo for every edit. `scene/prefab.h` has prefabs with overrides | Prefabs in the editor; placement tools; thumbnails in the asset browser |
| Build a city-sized world | Cells cooked into scenes and streamed by the runtime (ADR-0062, ADR-0072); the reference city is laid out by `game_sandbox`'s commands; the editor edits one scene | Cells, scene files and proxies made in the editor, which streams the world as the game does; terrain (the renderer has none), roads, scatter |
| Physics in levels | `RigidBody` and `Collider` (sphere, capsule, box) in the inspector; play-in-editor simulates a copy (ADR-0050) | Mesh, hull and compound colliders as components (the physics API has the shapes; runtime.md: they need cooked collision assets); joints, triggers and physics materials as components; a character controller component (today a game writes its own, as `samples/game_physics` does) |
| Audio in levels | WAV sounds, `AudioSource` and `AudioListener`, buses with their volumes; play loads and plays them (runtime.md "Audio") | Hearing a sound in the asset browser; each source's range in the viewport; ambience and reverb zones; music |
| Daily use | Undo and redo; a prompt before unsaved changes are lost | Autosave and recovery after a crash; the crash handler of §8.11 (none in the tree); soak tests (none); a guide to the editor's workflows |

The user chose the scope where it could go several ways (2026-11-17):
- modeling at **level-design grade**, with detailed assets from Blender (a full modeler in the editor, and today's tools,
  were the other options);
- for textures, **a material editor, procedural textures and texture painting** (decals were offered, not chosen);
- for the world, **all four tools offered**: city-sized worlds in the editor, terrain, roads and splines, scatter
  painting.

They did not object to the rest of the plan offered with those questions: shipping and a release after Phase 16, the
gate in shipping builds giving way to Phase 16's target, Phase 14's leftovers going to Phase 16, and the crash handler,
soak tests and docs kept for in-house use.

## Decision

1. **Phase 15 builds the in-house production tools:** what the user needs to build the first game's world themselves,
   in the editor and from Blender, for their own production and not a public release. In this order, each as tested
   engine, cooker or editor work with its design doc, and with its own ADR where a technique is chosen:
   1. **Daily use, first:** autosave, and recovery of the work after a crash; the crash handler of §8.11, minidumps on
      Windows and backtraces on Linux, written locally. They protect the work from the phase's first day.
   2. **Materials:** a material source that the cooker cooks and the editor makes and edits (textures, factors, tiling,
      alpha, double-sidedness), a browser of materials with previews, and a material dropped on an entity or a face;
      import settings for textures (colour, normal, linear data).
   3. **The Blender round trip:** cooked collision assets, triangle meshes for the static world and convex hulls for
      props, chosen per asset or by a naming convention; glass from `KHR_materials_transmission` and the other material
      extensions the renderer can draw; `TEXCOORD_1`; a re-import when a source changes on disk; import settings per
      asset; a guide to exporting from Blender.
   4. **Physics in levels:** colliders of cooked meshes and hulls, compounds through the hierarchy, and joints, triggers
      and physics materials as components; a character controller component; a first-person starter game (walk, run,
      jump, open doors, push props) that plays a level in the editor.
   5. **Objects:** prefabs in the editor, made from a selection, placed, edited once for every instance and overridden
      per instance; placement tools (snapping to surfaces and vertices, aligning to the surface, duplicating along a line
      or a grid); thumbnails in the asset browser.
   6. **Modeling tools v2 (§8.13), at level-design grade:** edges; delete, merge, split; bevel, inset, loop cut, mirror;
      materials per face; UV projection that tiles textures at real-world scale; grid and vertex snapping.
   7. **Procedural textures:** tileable textures (brick, concrete, asphalt, noise) generated in the editor and cooked like
      images.
   8. **City-sized worlds:** cells and several scene files made in the editor; the editor streaming the world as the game
      does; the cells' proxies cooked from the editor.
   9. **Terrain:** a heightfield ground sculpted and painted in the editor, drawn by a renderer system of its own, with
      its collider (the physics module's heightfield), streamed with the cells.
   10. **Roads and splines:** roads, sidewalks, curbs, rails and fences along curves, made into meshes and colliders.
   11. **Scatter painting:** trees, bushes, rocks and litter painted over surfaces as instances. Plants keep §8.6's rule
       for our own content: modeled, not alpha-tested.
   12. **Texture painting:** colour, roughness and grime painted on models in the viewport.
   13. **Audio in levels:** sounds heard in the asset browser; each source's range in the viewport; ambience and reverb
       zones; music.
   14. **Soak tests and the guide, last:** hours of editing and play with no memory growth; the editor's guide, a page
       for each workflow.
2. **Phase 15's exit:**
   - **A test district the user builds** with the editor and Blender, using every tool above: terrain sculpted and
     painted; roads and sidewalks drawn along splines; a building imported from Blender with its materials and
     collision, and one modeled in the editor; materials made in the editor, a procedural one and a painted surface
     among them; props as prefabs and plants scattered; street and interior lights; ambient, positional and zoned sound.
     It is walked in first person in play (colliding, climbing stairs, pushing a prop, opening a door), saved, closed and
     opened again unchanged, streamed as cells, and the user signs it off.
   - Tests drive each workflow. A soak run of hours of editing and play shows no memory growth; a crash forced in a test
     leaves its dump, and the autosave gives the work back.
   - No reference scene ends the phase slower than ADR-0101 recorded (§10.7's guard rail), and each §10.4 line is
     reported at native 4K.
   - The editor's guide is written. The phase's tag is `v0.10.0`.
3. **Phase 16 is the first game** (ADR-0067). The game is built on the engine, and what it needs is built as it needs
   it, each as tested engine work with its own ADR: character models, skeletal animation and compute skinning (§8.6),
   navigation, NPC AI and crowds, and a crowd reference scene. The engine is optimized throughout against the game until
   it holds 1%-low ≤ 4.17 ms (240 FPS) at native 4K on the reference hardware, inside the §10.4 and §10.5 budgets. That
   work takes up what Phase 14 left:
   - the §10.4 lines ADR-0101 reported over budget;
   - the two views ADR-0099 recorded over the gate;
   - the landscape's memory (ADR-0101's decision 3, which gave it to Phase 15);
   - the six items ADR-0101 carried forward.
4. **Shipping and a release wait for the user's word, after Phase 16** (§13). That covers:
   - shipping builds measured as a target;
   - packaging with `LICENSE` and `NOTICE` (ADR-0001);
   - the game linked into the player statically (ADR-0024);
   - crash reports beyond the local dump;
   - documentation for other users;
   - `v1.0.0`.

   Meanwhile the Shipping configuration keeps building and passing its tests in CI.
5. **The gates.** Phase 15 has no frame gate beyond the guard rail: no reference scene ends it slower than when it began,
   unless an ADR shows where the cost is won back (§10.7). The gate in shipping builds at Phase 15's exit (ADR-0067's
   decision 4, ADR-0082, ADR-0099's decision 4) no longer stands; Phase 16 holds §10.1's target on the game. The nightly
   and its budgets are unchanged.

## Alternatives considered

- **Phase 15 as planned, production hardening and `v1.0.0`:** a public release is not the user's goal now, and
  hardening gives none of the tools above.
- **A full modeler in the editor** (UV unwrapping and editing, booleans, modifiers, sculpting), to replace Blender for
  most work: several times the size of the phase. **Today's modeling tools**, with nearly all modeling in Blender: no
  blockouts in the editor. The user chose level-design grade.
- **Decals** (road markings, graffiti, posters, dirt): offered, not chosen; the renderer has none.
- **The 144 FPS gate kept at Phase 15's exit, in Development builds:** a gate on tools work that changes no pass the
  reference scenes draw. The guard rail keeps them from regressing, and Phase 16 gates the game.
- **Characters before the world** (ADR-0067's alternative of a characters phase): the user builds the world first, and
  the characters with the game.

## Consequences

- Phase 15 is larger than the phase it replaces, and most of it is editor, cooker and runtime work. Terrain is the one
  new renderer system. The reference scenes draw none, so the guard rail measures what it costs a frame without it.
- Texture painting needs a layout of its own on what it paints, while level-design materials tile by projection. Its
  ADR chooses the method (a second UV set the cooker lays out, or paint per vertex), using an asset's own `TEXCOORD_1`
  from Blender where it has one.
- The editor's new formats (materials, terrain, splines, scatter, the world's scene files) are versioned and fuzzed like
  every parser (§8.8), and every edit with them is undoable (§8.13).
- The engine reaches the game without a shipping build measured against a gate. Phase 16 measures the game the way the
  gates have been measured, in Profile builds (ADR-0099: the per-pass timestamps add 0.05 to 0.06 ms a frame).
- The packaging ADR-0001 asked of Phase 15, and the static link ADR-0024 left to it, move with the shipping builds;
  docs/runtime.md says so. The crash handler stays in Phase 15 (docs/profile.md, ADR-0022).
- Emerald Square stays a test of the engine only (ADR-0067's decision 3); the city the user builds is their own content.
- Versions: `v0.10.0` at Phase 15's end; `v1.0.0` waits for a release.
