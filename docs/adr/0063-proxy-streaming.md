# 0063. Streaming: the cells' proxies, level by level, as the culls ask for them

- **Date:** 2026-10-28
- **Status:** Accepted (ADR-0062's stage 3; the user's decisions of 2026-10-28: streaming
  next, and first a cheaper voxel encoding that keeps the far field's detail, else a
  finer minimum; see "The encoding and the minimum")

## Context

Phase 13's exit asks for an open world that streams without hitches and stays under
6.5 GB of VRAM (CLAUDE.md §12, §8.8). Nothing streams yet: a scene is loaded whole
before its first frame (the landscape in 230 to 274 s), and what the GPU holds the host
holds again (ADR-0062 "Stage 2": the cooked scene keeps every asset it loaded, its
compressed blobs decompressed in the loader's heap and the rest mapped).

What grows with a world's area is its cells' proxies. A shared mesh is resident once
however many instances draw it: the landscape's 31 meshes (1.34 GB decompressed, the
pines 0.9 GB of it) and their bottom levels (811 MB) are bounded by the assets'
variety, which the content budgets bound (§8.6), and the terrain is 19 MB for its
1.5 km². A proxy is its cell's alone: 5.4 MB a cell on the landscape (2.06 GB for 384
cells, 1.45 GB of it level 0), 20.5 GB for a world of the game's size.

A model of what views need (`build/scratch/claude/proxy_residency.py`): every proxy's
clusters, bricks, voxels and level errors read from the cooked landscape; for each
cell past its distance `D`, the finest level its cut can use at a pixel's error from
the camera's distance to its box (in plan, elevation ignored) and every level from
there to the root; for each cell nearer than `D`, its root alone; the shadow map taken
to need what the camera does (a texel a pixel at the receivers); and a 4 km world of
3,844 cells tiled at random with the landscape's proxies.

| Where | Proxies needed | Of which records |
|---|---|---|
| the landscape, from its six views, all around | 0.68 to 1.05 GB (whole: 2.06) | 0.14 GB |
| ... in the views' field of view only | 0.19 to 0.94 GB | 0.14 GB |
| a 4 km world, from its centre, all around | 8.35 GB | 1.38 GB |
| ... the same, with a 20 % margin for prefetch | 9.93 GB | 1.38 GB |
| ... in the field of view only (81 degrees) | 3.23 to 3.26 GB | 1.38 GB |
| ... from a point a quarter of the way in, field of view only | 2.27 to 3.65 GB | 1.38 GB |

(Records: the clusters', groups' and hierarchy's records, held whole in the model.)

Two things follow. On the landscape, a proxy's levels held by distance halve what the
proxies take. In a world of the game's size, what lies around the camera is more than
the card holds: each doubling of distance holds about as many voxels as the one
before (a level has a quarter of the voxels of the next finer: 1.45, 0.36, 0.085,
0.020 GB on the landscape, over four times the area), and a 4 km world spans three
such rings past `D`. What the views see fits: about a fifth of the ring, and then the
records (1.38 of the 3.26 GB from the centre) must stream with their levels.

Nanite streams what its views ask for (Karis et al., "Nanite: A Deep Dive", SIGGRAPH
2021): "the GPU requests data where it finds the quality is insufficient and the CPU
asynchronously fulfills those requests"; its culling hierarchy stays resident, so a
traversal can ask for every level it needs at once; the primary view and the shadow
views (at a lower priority) both ask, even for what is resident, to keep its priority;
the CPU evicts what is asked for least to make room; and what is resident must always
be "a valid cut of the full DAG". Its unit is a page of cluster groups (a group is
never drawn before all its parts are loaded), and its first page, the top of the DAG,
is always resident, "so we always have something to render".

What there is to build on: a mesh's hierarchy is a tree per level under the root node
(render/cluster_lod.h), so a level's nodes, groups and clusters are already a unit; a
proxy's every cluster is a voxel cluster, its bricks and voxels following one another
level by level; the culls know each instance's distance; and proxies have no bottom
levels (rays trace the instances').

## Decision

**A proxy's level is the unit of streaming**, asked for by the culls and fulfilled by a
manager on the CPU, within a pool of fixed size.

- **The unit.** A level of a streamed mesh: its tree's nodes, its groups and their
  cluster lists, its clusters' records (meshlet, bounds, level), its bricks and its
  voxels, as one block. The cooker writes each level as its own blob (by index), so it
  is read and decompressed alone. A mesh's header, its level table (each level's size,
  the range of its errors, its blob), its root node and its coarsest level stay
  resident: there is always something to draw.
- **Which meshes:** proxies (meshes of voxels alone, ADR-0062). Shared meshes stay
  whole: their size is the assets' variety, which the content budgets hold, and their
  triangles feed the rays' bottom levels.
- **Addressing.** A streamed mesh's level table on the GPU gives each level's block (its
  place in the pool, or none) and the first index of each of its arrays; the shaders
  that read a mesh's records by index (the walk, the culls, the rasters, the resolve)
  find a level's block through it. The lists between the passes carry the cluster's
  level, so the lookup is a read, not a search.
- **Requests.** The camera's instance cull (both phases) and the shadow map's, for each
  streamed instance they draw, compute the finest level its cut can use from its
  nearest distance (at a margin nearer, for prefetch) and the level table's errors, and
  lower the mesh's request (an atomic minimum, with the frame's number). The CPU reads
  the requests back two frames late.
- **Residency.** The manager holds each streamed mesh's levels as a run from some level
  to the root, loads in the order of how far a request lies below what is resident and
  of distance, and, the pool full, evicts the finest levels of the meshes asked for
  least recently. Out of view, every cell past its distance holds its levels from one
  coarser than its cut would use there, which the manager works out from the camera
  on the CPU, so that what a quick turn brings into view is at most one level coarser
  (voxels of two pixels) until its own level arrives ("The encoding and the minimum").
- **Loading without a host copy.** An I/O thread reads a level's stored bytes from the
  pack (`nph_vfs_read_at`), a job decompresses them into the upload ring, and uploads
  are bounded per frame in bytes. A level is installed at a frame's start (its table
  entry written, the mesh's resident level lowered); an eviction raises the resident
  level first and frees the block once the frames in flight are done. The asset loader
  loads a streamed mesh without its levels' blobs, and the cooked-scene bridge and the
  runtime release what the renderer has uploaded.
- **The cut.** The walk treats a mesh's finest resident level as its level 0: those
  clusters are drawn wherever their consumers are too coarse, whatever their makers'
  error. A DAG truncated at a level is a DAG, so the cut stays valid (no cracks),
  coarser where data has not arrived.
- **Statistics:** resident bytes, requests, loads in flight, bytes uploaded a frame,
  evictions; `render_perf` prints them, and a moving and turning camera's frame times
  show whether streaming hitches.

## Alternatives considered

- **Whole residency (today):** 20.5 GB of proxies in a 4 km world.
- **Levels by distance alone, decided on the CPU:** no readback, deterministic; but all
  around the camera a 4 km world needs 8.35 GB, the shadow map's casters out of view
  are guessed, and what a ridge hides is loaded.
- **Nanite's pages:** groups packed into fixed-size pages by place and level, records
  patched on install: finer than a level (a cell half behind a ridge loads half) and
  without fragmentation, at the cost of the patching. The level table leaves the door
  open (a level's block can become pages) if measurements show the waste.
- **Sparse buffers (Vulkan sparse residency):** the arrays keep their indices and the
  shaders their addressing while the driver maps 64 KB pages; but each level's range
  in each array rounds out to pages (about a megabyte a resident cell, a gigabyte at
  a thousand),
  binding goes through `vkQueueBindSparse`, and lavapipe's support would gate CI.
- **Coarser proxies** (`D` at 1,120 m, `c0` 0.5 m): a ring fewer, but the instances
  drawn out to 1,120 m, where stage 2 measured the proxies saving 0.4 to 4.2 ms a
  frame.
- **Smaller voxels in memory** (16 bytes now): complementary, measured after streaming.

## Consequences

- VRAM is the pool's budget plus the shared content, whatever the world's size.
- Loading a scene loads its shared meshes and its proxies' roots; the rest arrives as
  the views ask for it.
- A quick turn shows the far field a level coarser until its own level arrives: in a
  4 km world about 1 GB for a new view (the compact encoding), a fraction of a second
  of reading and decompressing. Holding every cell at its own level all round, which
  would avoid even that, takes 5.36 GB with the compact encoding: more than the card
  holds beside the rest of the scene.
- Every shader that reads a streamed mesh's records goes through the level table.
- The requests arrive two frames late: the prefetch margin decides how often an
  approach outruns them.

## The encoding and the minimum (2026-10-28)

Asked first for a cheaper encoding that keeps the far field's detail, and failing that
a finer minimum than the 2 m floor proposed above, we measured what a voxel's 16 bytes
hold (`build/scratch/claude/voxel_stats.py`, `voxel_rare.py`: 3.3 million voxels of 12
proxies, 1.6 million of the shared meshes' voxel levels):

- **Occlusion and metallic** are the same in every voxel cluster, proxies and trees
  alike (255 and 0: the landscape's materials have neither map): 16 bits a voxel that
  a cluster's record can hold once.
- **The capsule's byte** is used by 2.8 % of the voxels, but they lie in 24 % of the
  proxies' bricks (35 % of their voxels), so a wider record for those bricks would give
  most of the saving back, and their roughness varies within a cluster (one roughness in
  36 % of them), so it cannot lend its byte.
- **The footprint's box** uses its 30 bits (21 bits of entropy; at 16ths of a cell 97 %
  of the voxels would change). The albedo (11 bits of entropy over 24; at 6 bits a
  channel, linear error up to 0.011), the roughness and the concentration (about 6 bits
  of entropy each over 8) would lose detail at fewer bits: they keep theirs.
- **The axis** needs fewer bits than its 32: at 12:12 octahedral the largest error is
  0.06 degrees, at 8:8 0.95. The capsules' lobes are broad (median 30 degrees, 90 % wider
  than 15; a few sheet-sharp), so a capsule's axis at 8:8 lies within the spread GGX
  gives a leaf's roughness.
- **Bricks** sit on the mesh's grid of their cell size (origins are whole bricks), so
  three 16-bit coordinates hold an origin exactly, and a cluster's bricks share one cell.

So the compact encoding: a voxel of 12 bytes (the box and shape; the axis at 12:12 and
the concentration, or for a capsule the axis at 8:8, the concentration and the capsule's
byte; the albedo and roughness at their present precision), occlusion and metallic in
the cluster's record (a cluster whose voxels differ keeps 16-byte voxels), and a brick of
16 bytes (its coordinates, its mask, its first voxel within its cluster). It is lossless
but for the axis's rounding, which an image comparison against the present encoding on
the landscape's views must show at the run-to-run noise. It takes the proxies from
16 + 32/16.5 bytes a voxel to about 12 + 1, 0.72 times the memory (the landscape's 2.06
GB to 1.48), and the voxel rasters' reads with it.

It does not make the transient go away. Residency for the 4 km world, records streamed
with their levels (`build/scratch/claude/proxy_policies.py`, the mean of eight headings):

| Out of view, a cell past its distance holds | Now, centre | Now, a quarter in | Compact, centre | Compact, a quarter in |
|---|---|---|---|---|
| its own level, as in view (no transient) | 7.49 GB | 5.89 GB | 5.36 GB | 4.22 GB |
| **one level coarser (2-pixel voxels at worst)** | 3.35 GB | 2.63 GB | **2.40 GB** | 1.88 GB |
| its levels from 1 m voxels | 2.84 GB | 2.40 GB | 2.03 GB | 1.72 GB |
| its levels from 2 m voxels (proposed above) | 2.22 GB | 1.78 GB | 1.59 GB | 1.27 GB |
| its root | 2.02 GB | 1.58 GB | 1.45 GB | 1.13 GB |

Beside the landscape's 4 GB of the rest (shared meshes, bottom levels, textures, lists
and targets), about 2.5 GB is left for the proxies under 6.5 GB. Every cell at its own
level all round does not fit even compact; one level coarser out of view fits compact
(2.40 GB at the worst place, 2.42 at its worst heading) and not now (3.35). So we take
the compact encoding and the finest minimum it affords: a turn shows the far field at
most a level coarser, briefly. The world the model tiles is the landscape's forest
everywhere, denser than the game's valley, ruins and meadows.

## Stages

1. **The compact voxels:** the encoding above in the renderer's and the cooker's
   formats (the mesh asset's version 5), the shaders and their C twins, the builders;
   the landscape recooked, its views compared with the present encoding (images, frame
   times, memory).
2. **The formats and the addressing:** levels as blobs, the level table, the shaders'
   lookups, partial loads; everything still resident, images unchanged.
3. **Streaming:** the requests, the manager, the pool, loading and eviction, the cut's
   clamp, the one-coarser residency out of view; tests (a level withheld is drawn
   coarser without holes; installs and evictions while frames are in flight).
4. **The open world:** `render_perf` tiles the landscape into a world of the game's size
   (each tile's proxies its own), measured for VRAM, for frame times while walking,
   sprinting and turning, and for load time.

## Stage 1, the compact voxels, measured (2026-10-28)

Built as decided (render/voxel.h "The compact form", docs/render.md "Voxels"): the
builders keep their records; `nph_render_voxels_pack` makes a mesh's compact ones, the
mesh asset's version 5 stores them, and the shaders' loaders hand the footprint tests
and the resolve the records they read before, bit for bit, but for the axis.

The landscape recooked (55 minutes, as before; the same 107,072,903 voxels, cell by
cell): its proxies hold 1.54 GB decompressed where they held 2.06 (0.75 times; the model
above, 0.72, had taken the clusters' records smaller too, which this stage leaves as they
are) and 1.40 GB stored of 1.69 (the voxels' words compress less than their 16 bytes
did); the pack is 2.59 GB of 2.88. The GPU's device-local memory in use: 6.73 GB of
7.18 at 16 times the lists, 5.30 of 5.84 at the default.

The image: in the views' deterministic normal debug view, 99.56 % of the pixels are the
same bit for bit, 0.44 % within 4 levels of 255 (the axis's rounding), and 119 isolated
pixels of the far field (0.0014 %) differ more, where the two-sided lobe's side facing
the viewer flips at a grazing view, as the 16-bit axis flips at others. The lit views
differ from the previous build by FLIP 0.013 to 0.029, the radiance cache's noise
realization: 0.4 to 0.6 of 255 apart on average in every band of the image, the near
field's triangles, which the encoding does not touch, alike.

Frame times (16 times the lists, native 4K, the views' passes against the previous
build's in a run of each after a first pair in the other order; the previous build's own
runs agree within 0.1 ms):

| Pass | Change |
|---|---|
| voxel raster, first phase | +0.07 to +0.18 ms (0.3 to 0.9 %) |
| voxel raster, second phase | +0.01 to +0.02 ms |
| voxel resolve | +0.012 to +0.022 ms (3 to 4 %) |
| the shadow map's voxel splat | unchanged |

On the synthetic foliage scenes, in interleaved rounds, every voxel pass is within its
noise (the voxel raster -0.008 to +0.001 ms). A first version of the loaders spilled the
voxel resolve's registers to shared memory (18 % slower); the searches now take a
cluster's run of words and its stride, two values where its record was four.

**Open:** the first view after loading, at 16 times the lists, runs the radiance cache's
update at 2.3 to 2.5 ms a frame for its whole measurement (1.1 ms in the valley's, when
it comes first) where the previous build's ran at 0.30; the same view measured later
takes 0.30, as does the first at the default lists. `render_perf --memory-trace` reads
7.68 GB of the 7.83 GB budget at the first frame, while the bottom levels build, and 6.73
GB after. The update's rays are the bottom levels' main readers; the likeliest reading is
the driver placing what was made near the budget in slower memory until it moves it back
(which `VK_EXT_memory_budget` does not show), not the encoding (the data are the same,
and later the same). Stage 3's streaming lowers the peak; the transient is watched for.

## References

- B. Karis, R. Stubbe, G. Wihlidal. "Nanite: A Deep Dive." SIGGRAPH 2021, Advances in
  Real-Time Rendering in Games.
- J. M. P. van Waveren. "Software Virtual Textures." id Software, 2012 (feedback-driven
  residency).
