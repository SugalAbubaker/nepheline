# 0072. The world partition in the engine's runtime

- **Date:** 2026-11-01
- **Status:** Accepted

## Context

[ADR-0067](0067-the-game-is-a-city.md) decision 6 puts the world partition's cells and streams in the
engine's runtime early in Phase 14. Everything that makes the open world work ([ADR-0062](0062-world-partition-hlod.md),
[ADR-0063](0063-proxy-streaming.md), [ADR-0065](0065-virtual-texturing.md)) was proven in `samples/common/cooked_scene.c`,
the harness `render_perf` and `scene_viewer` share. A game runs on something else: `nph_scene_import_cooked` makes
entities, and the scene renderer mirrors them. There, a scene past the renderer's default capacities failed on its first
frame (the landscape's 47,862 instances against 16,384), the importer dropped a scene's cells, and no stream ran.

## Decision

1. **A cell is a component.** `WorldCell` holds its id, its box and its proxy, as `NphSceneCell` does, relative to its
   entity. `MeshRenderer.cell` names a cell by id (0: none). It is reflected data like the rest, so the editor shows it,
   scene files keep it, and a play copy of the world has it. The id is a number rather than an entity, since a scene file
   would otherwise need its references remapped.
2. **The importer makes them:** a child of the root per cell, with a new id, past the world's highest. So a scene imported
   again, a further tile of a world, has cells of its own. `no_cells` declines them, and `instances_only` declines the
   lights and cameras of the tiles past the first (a world has one sun). A root that turns or scales its scene gets no
   cells, as an axis-aligned box cannot follow it; that is logged.
3. **The scene renderer mirrors them,** as it does instances and lights, in `scene_cells.c`:
   - An entity's WorldCell becomes a render cell once its proxy's head, its coarsest level and its material have loaded.
     The mesh instances that name it wait, as they wait for their meshes: an instance's cell is fixed when it is made.
   - A cell without a proxy, or whose proxy fails, makes no render cell, and its instances draw as themselves.
   - A cell that goes is marked, and released once its instances have dropped their handles, a pass over the records per
     sync that has any. They are made anew, so a despawned cell leaves its instances drawn as themselves rather than gone.
   - A change to a cell's place or proxy is a cell going and a cell coming.
4. **One function runs the streams:** `nph_scene_renderer_stream`, between `nph_gpu_frame_begin` and `nph_render_frame`,
   for the view the frame draws. The engine calls it. The proxy stream comes with the first cell that streams; the texture
   stream with a budget, on a device that pages, and the asset cache then makes its textures paged and gives them to it.
5. **`nph_proxy_stream_remove`.** Cells could only join a stream. They now leave it in O(1), by the index `add` returned, so
   worlds can come and go. A slot freed is reused.
6. **Capacities and budgets are latched cvars:** `r.max_instances`, `r.max_meshes`, `r.max_materials`, `r.max_textures`,
   `r.max_lights`, `r.max_cells`, `r.max_assets`, `r.deferred_destroys`, `r.lists`, `r.voxel_pool_mb`, `r.proxy_budget_mb`, `r.texture_budget_mb`.
   Zero is the renderer's default, so nothing changes for a game that sets none. `nph_renderer_desc_scale_lists` is what
   the harnesses did by hand, now in one place.
7. **`game_sandbox` takes `sandbox.tiles` and `sandbox.tile_pitch`,** and its header gives the commands for the city and
   the landscape, in `nph_player` as a game runs them.
8. **A mesh of many materials keeps them.** `MeshRenderer` holds eight slots, which nearly every mesh does not fill, and on
   a large world the component is paid for per entity. The slots past them go in a component of their own, `MeshMaterials`,
   of 24 more, which only such a mesh carries, and the importer adds it. 32 slots cover the reference scenes; past them the
   importer warns and the rest draw with the default material. The player now logs a world's stats as it ends, since a
   run with no error says little about what it drew.

## What it does

Two tests hold it. On the grove the integration tests cook, the scene renderer's cells and streams draw exactly the image
of the harness's own (0 pixels of 65,536 differ), and hold every level the views ask for.
`cells_go_with_their_entities_and_leave_their_instances` takes them through a cell despawned, changed, its root despawned,
imported again, a world swapped, a turned root, and a proxy that does not exist.

Through `nph_player`, as a game runs them (1920 by 1080, Profile, 6,000 frames, the commands in `game_sandbox`'s header),
each world ends with nothing failed or waiting and no validation message:

| World | Instances | Cells | Held at the end | Last frame | Peak private memory |
|---|---|---|---|---|---|
| The city, 16 by 16 tiles | 226,048 | none | | 2.88 ms | 7.05 GB |
| The landscape | 47,863 | 384 | 1,459 proxy levels, 715 MB; 75 texture levels, 61.5 MB | 23.17 ms | 7.76 GB |

The frame times are of the view the scene's own camera gives, in a smaller window than the reference runs': they say that
the path works, and `render_perf` stays what measures the frame. These runs found what the harness had kept from us: the
city has a mesh of 13 materials and the interior two of 28, and all but the first eight drew with the default material
(decision 8).

**Cost.** A world with no WorldCell pays for an empty query's iteration and a hash lookup per instance made. `render_perf`
does not run this path, so its reference scenes do not move. The stream's update is as ADR-0063 measured it. `EntityRecord`
grows by 16 bytes, per entity that draws.

## Alternatives

- **A cell as an entity reference in `MeshRenderer`,** the natural ECS form: scene files would need their references
  remapped on load and on copy, and nothing else in the components needs that yet.
- **Parenting instances to their cell.** A cell's instances keep the scene's hierarchy, which a cell cuts across.
- **A world-partition call beside the world,** which registers cells with the scene renderer directly (as the harness
  does): shorter, and neither the editor nor scene files nor play-in-editor would see them.
- **Sizing the renderer from the scene.** It is created before any scene loads, and a game streams worlds it has not seen.
  A game knows its worlds' sizes; cvars say them.
- **Moving `render_perf` and `scene_viewer` onto this path.** The harness measures the renderer alone, and its numbers
  should not move with the ECS. They stay apart; the test compares the two.

## Consequences

- A game can load the landscape and the city through `nph_player` (`samples/game_sandbox/game.c` has the commands).
- A world's meshes and materials still load whole, as ADR-0063 decided for shared meshes. Sounds still unload only when
  play ends. Unloading by region stays open.
- The texture stream's install-frame cost (ADR-0066 decision 4) now reaches the main thread of a game: it is left for the
  work on it, measured by `prof` in the frame's `runtime.sync` and the streams' stats.
- Instances whose cell comes later than them are not moved into it: a game makes the cell first, as the importer does.
