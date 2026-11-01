# 0062. World partition: cells of static content and their far-field proxies (HLOD)

- **Date:** 2026-10-28
- **Status:** Accepted (built in stages; the first: the proxies, built by the cooker)

## Context

ADR-0060 draws each instance at its own cut of its own hierarchy, and ADR-0061 turns
the far cuts of aggregate geometry into voxels. The open landscape (`benchmarks/scenes/
landscape.json`, 1 by 1.5 km, 47,863 instances of ten Poly Haven assets) is what that
leaves. Its forest view, with every list sixteen times its default (the defaults
overflow and drop trees), took 64.7 ms on the RTX 3060 Ti after ADR-0061's amendments:
the voxels' raster 26.2 ms (1.6 million voxel clusters, 24 million bricks), the
triangles' software raster 24.9 ms (155 million triangles), the walk and the culls
most of the rest.

A model of the view's cuts (`build/scratch/claude/cut_cost.py`: every instance in the
frustum, no occlusion, its mesh's cut at a pixel's error from the cooker's cut log,
the finer power of two: an upper bound) shows where each cost lies:

| distance | instances | triangles | voxel clusters |
|---|---|---|---|
| 0 to 200 m | 870 | 272.6 million | 409,424 |
| 200 to 400 m | 2,990 | 61.2 million | 818,448 |
| 400 to 800 m | 10,997 | 27.4 million | 819,014 |
| 800 to 1,600 m | 9,409 | 9.7 million | 280,155 |

- **The far field is the voxels' half.** 82 % of the voxel clusters and 84 % of the
  instances in view lie beyond 200 m. A pine 1 km away (about 45 pixels tall) still
  draws about 40 clusters, its trunk's triangles and its canopy's voxels, inside and
  out: a cut cannot drop below a cluster an instance, and the walk visits every
  instance's hierarchy.
- **The near field is the triangles' half** (the needles within 200 m), which this
  decision does not address: the user chose the far field first (2026-10-28).
- **The game's world** (ADR-0059: a 4 by 4 km valley) at the landscape's density holds
  about 500,000 instances: per-instance cost does not scale to it.

§8.8 plans a world partition: streaming cells, with HLOD proxies for the far field.
Hierarchical levels of detail for static scenes go back to Erikson, Manocha and Baxter
("HLODs for Faster Display of Large Static and Dynamic Environments", I3D 2001); Unreal
Engine's world partition builds them per cell (instanced, merged, simplified, or
approximated meshes). Gobbetti and Marton's Far Voxels (SIGGRAPH 2005) is the closest
to what the far field needs: the inner nodes of an HLOD hierarchy are voxels about a
pixel across where the node is first drawn, sampled by rays from outside the node,
so that what is never seen from there is never stored ("environmental occlusion": 25
to 57 % of the voxels dropped in their models, Wagner, Raposo and Gattass, ISVC 2007).

## Decision

### Cells

- **The partition.** The cooker cuts a scene's static instances into square cells of
  64 m on the ground plane (x and z), each holding the instances whose bounding
  spheres' centres lie in it. The scene asset lists its instances cell by cell.
- **Why 64 m.** A cell is drawn as its proxy from several hundred metres (below), where
  its extent changes the distance by about a tenth; at the landscape's density it
  holds about 130 instances, a proxy's worth.
- **What stays out:** instances that move (a game's dynamic objects) and lights.

### Proxies

- **The representation.** A cell's proxy is a mesh of voxels alone, ADR-0061's bricks
  and voxels in clusters, in levels: the finest at cell size `c0`, each next level
  twice as coarse, to a single cluster. The levels form an octree over the cell, which
  is a hierarchy of groups as ADR-0060's (a node of the octree is the group that
  consumes its children's clusters and makes its own; its error is its cell size), so
  the renderer walks, culls, draws and shadows a proxy as any mesh with voxel levels.
- **The finest cell** is the largest power of two at most the error of a pixel at the
  distance `D` from which the proxy is drawn first: `c0 = 0.25 m` for `D = 560 m` at the
  reference views (4K, 0.9 rad vertical: 4.47e-4 m a metre a pixel).
- **Visibility.** A voxel is kept only where some ray from at least `D` away reaches it:
  rays from a sphere of directions around the cell, through the cell's own geometry
  and its neighbours' within that distance (which hide it from there) and the terrain.
  The sweep runs on an occupancy grid first, made from each instance's own cut at half
  the cell size, the voxels' coverage as transmittance along each ray; only the voxels
  it keeps are sampled. A forest seen from outside is its canopy's skin: what a proxy
  costs follows its visible surface, not its volume.
- **Sampling.** Each kept voxel is probed as ADR-0061 probes a group's cells (a jittered
  grid of rays along each axis: coverage, footprint, look) against the instances'
  triangles through one bounding volume hierarchy a mesh, reached through the
  instances' transforms. Which level of each mesh serves as the source (level 0, or the
  cut at a quarter of the cell) is measured in stage 1: level 0 is exact, a cut
  cheaper.
- **Errors.** A level's error is its cell. A proxy is drawn only where its finest
  level's error is within the view's (below), so it is never seen at an error the
  instances would not have had.

### Switching

- **The camera.** A cell is drawn as its proxy where both hold at the cell's nearest
  point: it is at least `D` away (no voxel dropped as unseen can be seen), and its
  proxy's finest error projected from there is within the view's error. Its instances
  are culled then, and its proxy drawn at its own cut; elsewhere the reverse. Each
  instance reads its cell's record in the instance cull and decides for itself (a
  cell's instances all decide alike), so there is no pass of its own.
- **The sun's shadow map.** Each level of the map draws a cell's proxy where its finest
  error is within the level's threshold (`vsm_lod_threshold`), the cell's instances
  elsewhere: the sun is infinitely far, and the proxy holds what is seen from every
  direction, the sun's among them.
- **Rays** (the radiance cache, the ray-traced shadows) keep tracing the instances'
  bottom levels.

### Formats and interfaces

- **The scene asset** gains a cell table (`CELL`: bounds, instance range, proxy mesh,
  `c0` and `D`), its instances sorted by cell.
- **The mesh asset** admits a mesh of voxels alone, whose level 0 is voxel clusters.
- **The renderer** gains cells (`nph_render_cell_create`: the proxy, bounds, `c0`, `D`),
  an instance's cell in its description, and the proxy's instance, which the cell owns.
  An instance's cell index rides in the upper 24 bits of its flags.

### Memory and streaming

A proxy's voxels are its own, where instances share their meshes. Its finest level
holds about `S / c0^2` voxels of `S` square metres of visible surface; drawn from `D`
out, the levels a view needs over a disc of cells from `D` to `R` take, at about 19
bytes a voxel and a forest's surface about 1.7 times its ground, roughly
`1.0 GB * ln(R / D)`: 0.9 GB for the landscape, 1.7 GB for the game's world. So stage 3
streams a proxy's levels by distance, the finest only within twice `D`, as the
geometry streaming streams clusters. Stage 1 measures what the estimate guesses.

### Stages

1. **The proxies.** The cooker's cells and proxy build (occupancy, visibility, sampling,
   octree levels), the formats, and tests: a proxy keeps what is seen from outside and
   drops what is not; its image from `D` matches its instances'.
2. **The switch.** The renderer's cells, the camera's and the shadow map's switch,
   `render_perf` and the landscape measured.
3. **Streaming:** cells' instances and proxies' levels by distance.

## Alternatives considered

- **Instances alone (ADR-0060/61 as they stand):** a cluster and a walk an instance at
  every distance; 500,000 instances in the game's world.
- **Unreal's instancing HLOD** (each instance's lowest level, instanced): our instances
  already are; the floor and the walk stay.
- **Merged, simplified or approximated triangle proxies** (Unreal's other HLOD layers,
  Simplygon): a remeshed surface closes a canopy's gaps and loses its coverage, which
  voxels measure (ADR-0061).
- **Impostors** (octahedral billboards): an alpha-tested look and no parallax, which our
  content rules avoid (§8.6), and no shadow-map depth.
- **A cell's voxels without visibility:** a forest cell's voxels grow with its volume
  (over a million at 0.25 m), most of them never seen.
- **Each mesh's far levels pruned to what is seen of the mesh alone:** cheaper far trees
  without memory of their own, and worth doing too, but the floor of a cluster and a
  walk an instance stays.

## Consequences

- The far field's cost follows the cells in view and their visible surface, not the
  instances in them; a cell's instances are culled where its proxy is drawn.
- The cooker's cost grows: every cell's proxy is sampled against its instances'
  geometry. Stage 1 measures it.
- Memory: proxies are unique, so their finer levels must stream (stage 3).
- The switch at `D` changes a cell's representation between two within a pixel of the
  geometry; temporal anti-aliasing smooths what remains.
- Open: the near field's needles (this ADR leaves them); cells' coarser groupings (2 by
  2) if the proxies' own levels prove too many far off; dynamic instances.

## Stage 1, measured (2026-10-28)

The builder is in (`render/cell_proxy.h`; docs/render.md "Cell proxies"), with
the decision's choices made as follows.

- **Sources.** Each level samples each instance's own cut at half the level's cell:
  its triangles, through its mesh's materials, and its voxels, which the voxelizer
  now takes as footprints (a ray taken into the voxel's mesh space and tested as the
  renderer tests it, the voxel's look and its lobe's second moment as the cell's).
  Level 0 or a cut at a quarter of the cell would cost more for no gain: sources four
  times finer measured no closer to the needles (below).
- **The octree.** A level's clusters hold blocks of 2 x 2 x 2 bricks; a group takes
  the clusters of a block twice that and makes the next level's one cluster there.
  A group's sphere holds what it makes as well as its makers' spheres: a coarser
  cell's footprints may reach past the finer ones'.
- **Visibility** is swept with rays, each stopping at the first footprint it meets,
  not with opacities: a wall's footprints close up, where a voxel's opacity along a
  slant (its footprint's shadow over its cell's) lets half the light through one
  layer of a wall. Each seen cell's neighbours are kept too (a margin against
  directions the sweep missed, which added half again to a double shell's voxels);
  whether it is worth its cost is measured on the landscape.
- **Slits.** One layer of a closed surface's voxels is not watertight: where its
  faces meet in a cell, the cell's footprint is fitted to their coverage, and may end
  a 32nd short of the cell's face. The renderer sees through such a slit as the sweep
  does, so what lies behind is kept; inside two such layers, nothing is.

Measured (`test_cell_proxy.c`, `test_voxel.c`): a bush inside a double shell is
left out entirely (403 voxels at level 0); inside an occluders' shell, the proxy is
empty; four turned trees of sparse needles give a finest level that stops more rays
than the trees' own cut at its cell: over 32 seeds of their needles, 4 to 7 % more on
average along the axes and 11 to 12 % along slants 10 degrees below the horizon
between them (a seed's direction from 2 % less to 26 % more), which is 5 to 6 % and
13 to 15 % more than the needles. The coverage tests' bounds are four standard
deviations of such a sweep's spread about its mean, per direction and for the mean
over the directions: one seed's numbers are one realization of the needles and of the
voxelizer's jittered rays (first measured on one seed, these bounds failed for others).

**Open: voxels made from voxels.** Re-voxelized where their grids agree, voxels stop
a few percent more rays than their sources (over 32 seeds of a needle cloud: 3 % on
average, 6 % along the axes; through a quarter turn, 2 %); in a turned grid (an
instance turned against the proxy's), each cell's footprint is fitted anew to the
union of several turned footprints, and the result stops more again (the trees above:
4 to 12 % more than their cut on average, the cut itself 0 to 3 % more than the
needles, ADR-0061's excess). Finer sources do not help (for one seed, a quarter of
the cell: 15 to 43 % over the needles, against 9 to 38 % at a half). The
cause is the fit: three axes' coverages leave a footprint's shadow along slants
free, and an ellipsoid fitted to a needle's axis coverages shadows more along its
own direction than the needle does. Stage 2 measures what it does to the image at
the switch, against the instances' cut there; if it shows, the remedy is the
voxelizer's (fitting each footprint to its coverage along the four diagonals too),
which would serve every voxel level.

## Stage 1, the formats and the cooker (2026-10-28)

- **The mesh asset** (version 4) admits a mesh of voxels alone: no vertices and no
  indices, every cluster a voxel cluster, level 0's too, one submesh of no indices
  over level 0's clusters. A record without vertices at level 0 is a voxel cluster's
  in such a mesh alone, so a mesh of triangles keeps its check.
- **The scene asset** (version 2) gains `CELL` (96 bytes a cell: its instances'
  bounds and range of `INST`, its proxy's mesh, material, origin, finest cell and
  distance), its instances written cell by cell, those of no cell after them.
- **The cooker** (`nph_cook --hlod`, `cook/cells.h`) partitions a glTF scene's
  instances by their spheres' centres into squares of 64 m; an instance whose mesh
  does not voxelize (a blended or emissive submesh) or whose sphere passes half a
  square (the terrain) is in no cell. The meshes' parts the proxies sample are kept
  as they cook: the clusters consumed above half the finest cell at their most
  stretched instance, with their vertices renumbered, every other cluster's record
  emptied. The proxies are built in rounds of 32 cells on the job system, each with
  the instances of the eight squares round it and of no cell (within its 3 x 3
  squares) as occluders, and written as meshes of voxels alone.

Measured on the landscape (`nph_cook --hlod`, eight workers): 47,862 of its 47,863
instances in 384 cells (the terrain in none), 35 to 209 a cell (median 123). The cook
takes 56 minutes, the cells 51 of them: 14.5 to 85 s a cell (median 60), about seven
at once. The proxies hold 107 million voxels in 5 to 7 levels, 7,300 to 414,000 a cell
(median 294,000), three quarters of them at the finest level (80.7 million: 5,200 to
312,000 a cell), about a tenth of what their instances' own cuts at 0.25 m draw (a
hundred pines at about 25,000 voxels each); written, 1.69 GB of mesh assets.
**The sweep left out almost nothing**: 20,131,122 of the 20,131,815 voxels swept at
the visibility level are seen from some direction. A pine's canopy at half a metre is
open (rays pass between its voxels), and the trees stand apart, so the proxies' saving
is the instances merged, not what they hide; the sweep stays for what does hide (the
ruins' interiors). **The proxies are twice the estimate** (above: 0.9 GB for the
landscape): their finest levels hold a canopy's volume seen through it, not a skin, so
stage 3's streaming of the finest levels by distance matters the more. A first run, on
fifteen workers, was stopped at 62 cells by the machine running short of memory (the
cooker at 4.5 GB and rising, 16 GB in all, other programs holding about half); with
the meshes' geometry, which only their submeshes are read of once they are cooked,
released before the cells are built, the cooker peaks at 5.0 GB while it loads the
scene and holds about 2.1 GB through the cells.

## Stage 2, the switch (2026-10-28)

The renderer's cells are in (`nph_render_cell_create`; docs/render.md "Cell proxies",
"The switch"), with the decision's choices made as follows.

- **A mesh of voxels alone** is a render mesh like any: no vertex or index buffers and
  no bottom level, every cluster a voxel cluster. The production path draws it; the
  reference renderer and the rays pass it by.
- **A cell** is its instances' box (a 64-bit centre), its proxy (a mesh of voxels alone,
  its materials, a rotation and translation), the proxy's finest error and its
  distance. The cell owns the proxy's instance; an instance names its cell in its
  description, and the cell's index, plus one, rides in the upper 24 bits of its
  record's flags, with a flag for the proxy. A table of the cells' records
  (`NphVisCell`, 48 bytes: the box's centre split as a translation is, its half
  extents, the error and the distance) is what the culls read.
- **The camera's switch** is the instance cull's first question, as decided: is the
  box's nearest point at least the distance away, and is the finest error fine from
  there (the levels' own test at that point, scale 1)? A cell's instances and its
  proxy ask it of the same record and view with the same arithmetic, so the proxy is
  drawn exactly where they are not, without a pass of its own. Views without levels of
  detail draw the instances.
- **The shadow map's** is the same question of each level: is the finest error within
  the level's threshold? Its answer does not change from frame to frame, so the pages
  drawn stay right.
- **The loading:** the cooked-scene bridge makes a scene's cells before its instances,
  which name them; `render_perf --no-cells` loads a scene without them, for comparing.
  The runtime's scene import makes no cells yet (its instances are drawn as
  themselves). The landscape's manifest does not ask the cooker for its cells yet, so
  the nightly measures it without them until stage 3 streams them: held whole (below),
  they leave the 16 GB runner 0.7 to 1.3 GB of free memory, and they add 50 minutes to
  its cook. (Amended 2026-10-31: the manifest asks for them now, and the nightly leaves
  the landscape out, ADR-0067.)

Measured (`test_vis_cells.c`): a grove of four trees of needles, triangles alone, and its
proxy at 0.125 m: near, the trees (8,904 triangles); from where the box is past the
proxy's distance and its finest error is fine, the proxy alone (110 voxel clusters, no
triangles, the four trees stood in for), covering 301 pixels where the trees alone cover
268 (on a target of 1,024 at a threshold of 4 pixels); the trees again in a view without
levels. A cell declared finer than every level of the shadow map is drawn into every
level as its proxy (113 voxel caster draws, 4 of triangles: the floor's and an
occluder's), one coarser than them all as its trees (110 triangle draws, no voxels).

Measured on the landscape (`render_perf --lists=16`, the lists the views need, its six
views at native 4K on the RTX 3060 Ti, with its cells and with `--no-cells`; a repeat of
the run without cells lands within 0.1 ms):

| View | Cells drawn as proxies | Instances they stand for | GPU, cells | Without | Change |
|---|---|---|---|---|---|
| forest | 138 | 17,118 | 61.21 ms | 64.17 ms | -2.96 ms (-4.6 %) |
| forest walk | 136 | 16,926 | 61.86 ms | 64.75 ms | -2.89 ms (-4.5 %) |
| forest sprint | 135 | 16,824 | 62.74 ms | 65.75 ms | -3.01 ms (-4.6 %) |
| forest turn | 138 | 17,118 | 63.55 ms | 64.73 ms | -1.19 ms (-1.8 %) |
| across | 117 | 13,586 | 64.04 ms | 64.40 ms | -0.36 ms (-0.6 %) |
| valley | 208 | 26,331 | 53.10 ms | 57.27 ms | -4.17 ms (-7.3 %) |

The savings are the voxel raster's (its first phase 2.2 ms in the forest views, 3.2 ms in
the valley: the proxies' bricks are 16 to 23 % fewer than the far trees' own cuts) and the
walk's (0.4 to 0.6 ms: 436,000 nodes against 571,000 in the forest). The instance cull
takes 0.5 ms more: it hands a proxy between its distance and about twice it its finest
level whole, as it does any mesh's level 0 where no coarser cut serves, so the work moves
out of the walk. The frame stays at 53 to 64 ms: the near and mid field's needles (the
software and voxel rasters' 44 ms) are not the cells' to save, and are next.

The image, against the run without cells: FLIP 0.014 to 0.029 over the frame (between
the two runs without cells, 0.0001 to 0.0035). The difference is the far ridges'
silhouettes against the sky, where the proxies are drawn (up to 0.6 at a pixel): their
trees in place, a little fuller, as the coverage bias above has them. The near field's
brightness is unchanged (to 0.03 %), its pixels 0.4 of 255 apart on average: the radiance
cache's fixed budget of rays falls otherwise once the far field changes, a different
realization of its noise.

Memory (`render_perf`'s GPU memory line, the valley's view run alone): the proxies are
2.06 GB decompressed (1.69 GB stored), and the device-local memory in use grows by about
as much, from 3.96 GB to 5.84 GB with the default lists (from 5.19 GB to 7.18 GB with
`--lists=16`): within the 7.83 GB the OS budgets the process and, with the default lists,
within Phase 13's 6.5 GB, though with 0.7 GB to spare. The host holds them twice over:
the cooked scene keeps every asset it loaded until it unloads, so the proxies stay
decompressed in the loader's heap and mapped where they were read, and the working set
grows by as much, from 3.6 GB to 7.2 GB (7.9 GB with `--lists=16`). The private memory
grows from 7.5 GB to 10.7 GB (from 8.6 GB to 12.7 GB), the machine's free memory falling
to 1.3 GB (0.7 GB) of its 16. Loading the proxies adds 1 to 3 s to the landscape's 230
to 274. Stage 3 streams them (the finest levels near, the coarser further), with no
copy on the host of what the GPU holds; it comes before the game's world, ten times the
landscape's area.

## References

- C. Erikson, D. Manocha, W. V. Baxter III. "HLODs for Faster Display of Large Static and
  Dynamic Environments." I3D 2001.
- E. Gobbetti, F. Marton. "Far Voxels: A Multiresolution Framework for Interactive
  Rendering of Huge Complex 3D Models on Commodity Graphics Platforms." ACM TOG 24(3),
  SIGGRAPH 2005.
- G. N. Wagner, A. Raposo, M. Gattass. "An Anti-aliasing Technique for Voxel-Based
  Massive Model Visualization Strategies." ISVC 2007.
- Epic Games. "World Partition: Hierarchical Level of Detail." Unreal Engine 5
  documentation.
