# 0064. Culling by cells first: a world's instances read only where their cells are

- **Date:** 2026-10-29
- **Status:** Accepted (from ADR-0063 stage 4's measurements; built in stages: the runs and
  the camera's cell cull, measured below; the shadow map's next)

## Context

The production path culls a frame's instances one invocation per instance slot. The
camera's first phase reads every instance's record (80 bytes) wherever it is, and the
sun's shadow map culls each slot again for each map level with pages to draw
(docs/render.md "The production path", "Shadows"). A world partition's cell (ADR-0062)
decides inside that per-instance work: an instance whose cell is drawn as its proxy
leaves after reading its record and its cell's.

ADR-0063 stage 4 laid the landscape out as a world of the game's size (4 by 4.5 km,
591,189 instance slots, 4,608 cells) and measured what that costs:

| Pass | Landscape (384 cells) | 4 x 3 world |
|---|---|---|
| `vis cull instances 0`, still | 0.147 ms | 0.137 ms (47 MB of records read) |
| `vis cull instances 0`, turning | 0.086 ms | 0.134 ms |
| `vsm cull instances`, walking / sprinting / turning | 0.011 / 0.017 / 0.041 ms | 0.049 / 0.123 / 0.426 ms (p99 0.8) |

§10.4 gives culling and the depth pyramid 0.25 ms and the whole shadow map 0.40 ms, and
§2 holds that world size affects streaming, never the per-frame budget. Here the camera's
cull spends its time reading the records of instances far behind it or inside proxied
cells. The shadow map's cull grows with the instances times the levels a moving view
redraws, because a coarse map level covers kilometres. A world four times this one would
spend 0.5 ms and 1.7 ms on these two passes alone.

The cells already group the static world: the cooker writes a scene's instances cell by
cell (ADR-0062), and every instance of the landscape's scattered content names its cell.
Only the terrain's squares, instances that cannot be voxelized, and dynamic objects belong
to no cell.

## Decision

Cull the cells first, and read an instance's record only where its cell is in view and
drawn as its instances.

- **Member runs.** The renderer keeps each cell's members as a run of instance slots in
  one GPU buffer (`vis cell members`, a u32 a member), each run a range under the TLSF
  range allocator (core/range_alloc.h), as the voxel pool's meshes are. A cell's proxy
  instances (at most NPH_RENDER_CELL_MAX_LEVELS) get a run of their own beside it. The
  runs' places go in a record per cell slot (`NphVisCellRun`: the members' first slot
  entry and count, the proxies' first and count; 16 bytes).
  - Instances of no cell form one more run, the loose list: its slots dense, a slot's
    place kept for removal by swap.
  - A member made appends to its run, moving the run to a range twice the size where it
    is full. A member destroyed is swapped with the run's last.
  - Changed runs go up with the frame's uploads: a slot each, or the run where it moved.
    The cost is amortized O(1) a change. A scene's load, made cell by cell, fills each run
    once.
- **The camera: `vis cull cells`, then the instances of the cells it keeps.**
  - Phase 0 first runs an invocation per cell slot.
  - A cell whose box is outside the frustum adds nothing: its members' bounds lie in
    its box (the box is theirs by NphRenderCellDesc's contract, and the renderer grows
    a cell's box over a member made outside it), so none of their clusters is in view.
  - A cell in view adds its proxy's run where vis_cell_proxied holds, else its members'
    run and its counted proxy instance. That instance makes the cell's requests as
    today, after its own frustum and previous-pyramid tests, so the stream sees the
    same requests.
  - The runs' slots are written to a work list, a subgroup's together as the meshlet
    items are, and the loose list's slots go after them.
  - `vis cull instances 0` then runs an invocation per work item, indirect. The work is
    the same per instance (the frustum, last frame's pyramid, the deferral to phase 1,
    the levels' roots and the meshlets), and so is the answer: the cell's test, still
    made per instance, agrees with the cell pass's by the same arithmetic.
  - Phase 1 (the deferred instances) is unchanged. Occlusion stays per instance, so the
    two-phase test needs nothing new.
- **The shadow map: `vsm cull cells`, then the instances of the cells it keeps.**
  - The pass runs an invocation per cell slot and map level with pages to draw: the
    cell's box, as a sphere in light space, against the level's pages to draw and its
    far plane (vsm_touches_render, vsm_beyond).
  - A touching cell adds its proxy's run where vsm_cell_proxied holds for the level, else
    its members'. The loose list is added for every such level.
  - The items are (slot, level), and `vsm cull instances` runs an invocation per item,
    indirect, with its per-instance work unchanged. `vsm cell requests` keeps its own
    pass: it asks against the pages requested, drawn or kept, not only those drawn.
- **Statistics.** The instances a culled cell holds are counted as culled by their cell,
  and those of a proxied cell as proxied (by the runs' counts, in the cell pass). The
  frame statistics keep their meanings.

Stages:

1. The member runs and their upkeep (CPU and GPU), with tests.
2. The camera's cell pass and work list, measured.
3. The shadow map's, measured.

Each stage leaves the images byte for byte as they were.

## Alternatives considered

- **Lighter records per instance** (a 16-byte sphere stream read first). It halves the
  bytes and leaves the cost linear in the world.
- **A hierarchy over the instances or cells, walked on the GPU** (as the levels of
  detail are). It is logarithmic, but it needs a build and refits as content moves, and
  a walk's steps cost a frame's fixed overhead of dispatches and barriers. The cells are
  the grouping the world already has, and their pass costs microseconds at 4,608 cells.
  A coarser level of cells can come if worlds grow a hundredfold.
- **Culling the cells on the CPU** and uploading the work list. That is about 20 µs of the
  main thread for the camera, but the shadow map's pages to draw are known only on the
  GPU, and §2 keeps the CPU's draw work constant.
- **Implicit runs** (a cell's members as a range of consecutive slots, as a cell-by-cell
  load makes them). They break as soon as a slot is reused or a cell's content changes.

## Consequences

- The camera's cull then costs the cells' pass (4,608 boxes and records) and the
  instances of the cells in view and drawn as their instances, not the world: about
  0.03 ms at the world's scale where 0.14. The shadow map's, while turning, costs the
  cells times the levels redrawn plus the instances of the cells under their pages:
  about 0.05 ms where 0.43. To be measured at each stage.
- Memory: 4 bytes an instance (the runs), 16 a cell, and the work lists (a slot an
  instance for the camera; the shadow map's items within max_vsm_meshlets' share).
- Instances of no cell are still culled one by one, all of them every frame: the
  terrain and the dynamic objects. A game's dynamic objects are few beside its static
  world, and a terrain of squares could join the cells later.
- Scenes without cells (Bistro) put every instance in the loose list: the same work as
  today, plus one cell pass over no cells.

## Stages 1 and 2, the runs and the camera's cells (2026-10-29)

Built as decided (docs/render.md "Cell proxies", Cells first), with two changes the
measurements made:

- **The runs go up with the frame's uploads.** Made in the frame's vis pass at first,
  they missed the flush its uploads go out with and arrived a frame late: a cell
  destroyed was still listed the frame after (a test counting the cells drawn as proxies
  caught it).
- **The cell cull is a test and an expansion.** One pass, a lane a cell copying its runs
  in the subgroup's turn (the meshlet items' owner search), took 58 µs at 4,608 cells.
  Each subgroup copied about 4,000 entries in 130 dependent rounds, and the 144 subgroups
  could not hide the loads' latency. The test now lists the cells (each place in the work
  list reserved, a subgroup's together), and `vis expand cells` copies their runs, a
  workgroup a listed cell: 7.8 and 7.4 µs.
- Scenes without cells (Bistro, the synthetic ones) keep phase 0 over every slot, the
  same passes as before.

**Measured** (RTX 3060 Ti, native 4K, 16 times the lists, within 400 MB):

| Phase 0's culling | Before | Cells first |
|---|---|---|
| 4 x 3 world, still, walk, sprint | 137 µs (the instance cull alone) | 43 µs (test 7.8, args 2.1, expansion 7.4, instances 25.5) |
| 4 x 3 world, turning | 134 µs | 41 µs |
| landscape alone, forest views | 147 µs | 67 µs |
| landscape alone, turning | 86 µs | 85 µs |

In interleaved runs of the landscape alone (three of each, every slot and cells first,
one binary apart) the frame is the same (+0.015 ms on average, within the runs' drift of
0.6 ms), the rasters within their own spread. The image is byte for byte the same in
`test_vis_cells.c`'s field of cells from near, far, turned away and without levels of
detail, and with a member made 20 m off its cell's box, which the box grows over (without
the box sent again, that member is culled and the test fails).

Stage 3 remains: the shadow map's instance cull, 426 µs turning in the 4 x 3 world.
