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
