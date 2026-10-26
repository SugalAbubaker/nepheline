# 0060. Virtualized geometry: a hierarchy of cluster levels, walked two levels a step

- **Date:** 2026-10-26
- **Status:** Accepted (the shadow rays and the radiance cache draw level 0 until their
  own levels arrive later in Phase 13); amended 2026-10-26: the sun's shadow map draws
  cuts of its own (below)

## Context

CLAUDE.md §8.6 stage 9 bounds the frame's geometry by its pixels: a hierarchy of
cluster levels of detail aimed at about one triangle per two to four pixels at 4K,
so that the visible triangles stay near 4 million or fewer whatever the world holds
(§10.3, strategy 2). Phase 13 builds it, with the streaming and the world partition
that depend on it. The first game is a forested valley seen at 1.7 m with modelled
vegetation to the horizon (ADR-0059): its ground, rocks, trunks, and ruins are dense
meshes seen from a metre to kilometres away, which is the case the hierarchy is for.

Before it, the production path drew every mesh at its one level: the instance cull
handed every meshlet of a visible instance to the meshlet cull, and a mesh's cost
followed its triangle count whatever its distance.

The constraints:

- **Watertight wherever the levels switch.** A part of a mesh drawn at one level
  next to a part at another must share its boundary exactly, in both rasters.
- **No holes or doubles from the GPU's arithmetic.** Two shaders that decide the
  same question with different rounding draw a part at two levels or at none.
- **Vulkan's guarantees only.** The walk must not depend on workgroups of one
  dispatch running concurrently, which Vulkan does not promise (lavapipe, which CI
  runs, executes them on a thread pool in any order).
- **Ours** (CLAUDE.md §3): the simplifier, the partition, and the walk are written
  here; no third-party geometry library.

## Decision

**The levels** (`render/cluster_lod.h`, `cluster_lod.c`, `cluster_simplify.c`). A
mesh's meshlets are level 0. Each coarser level is made from the one before, after
Batched Multi Triangulation (Cignoni et al. 2005) as Nanite does it (Karis et al.
2021):

1. The level's clusters are grouped by eight: grown greedily from seeds in Morton
   order, each step taking the cluster that shares the most vertices with the group;
   a cluster sharing none joins the nearest group with room within two radii.
2. The vertices a group shares with another are locked.
3. The group is simplified to half its triangles by half-edge collapses ranked by
   quadric error (Garland & Heckbert 1997): every vertex of every level is a vertex
   of the mesh, so no attribute is interpolated. Vertex kinds after meshoptimizer's
   simplifier (manifold, border, seam, complex, locked) say where each may move; the
   link condition keeps the topology; a collapse may not turn a triangle over, fold
   it to a line, or face it away from the level-0 normals merged into its corners.
4. The result is split into clusters again by the meshlet builder.

A group's **error** is the largest of its collapses' quadric error, two measured
distances (every level-0 point to the level's result through the class it merged
into, and the result's triangles' centroids and edge midpoints back to level 0),
and the errors of the groups that made its clusters; its sphere holds theirs. The
quadric error alone came out 2.5 to 3.3 times under the measured distance on curved
surfaces, which is why both directions are measured. The levels stop at one cluster,
or when a level takes off less than a tenth of the triangles. The coarsest level's
clusters are grouped as any level's, into root groups whose error is the largest
float. The build is deterministic.

**The hierarchy.** Each level's groups, and the root groups, in Morton order, lie
under trees of nodes of eight children, the trees under node 0. A child
records its sphere, the largest group error below it, and the smallest error of the
groups that made the clusters below it (0 where level 0 is). Every sphere is the
union of its parts rounded up in double precision, so it holds them exactly.

**The cut.** A cluster is drawn exactly when the group that made it projects an
error within the view's threshold and the group that consumes it does not
(`NphRenderView.lod_error`, in pixels, 1 by default). One function,
`vis_lod_group_too_coarse`, decides for a group: the hierarchy's walk calls it for
the groups it takes, and the meshlet cull for the group that made each cluster. It
is `precise` throughout (SPIR-V NoContraction), takes no square root (both sides
squared, the instance's scale factored out), and reads only the instance record and
the group's numbers, which validation requires the hierarchy to copy exactly: the
two shaders decide alike for every group, bit for bit. The walk's prunes are bounds,
widened past any rounding between them and the exact test (a part in 2^12 of the
error and radius, 2^-16 of the magnitudes the centre's rounding is relative to): a
node is entered if any group below may be too coarse, and skipped if every maker
below is surely too coarse even from its sphere's far side.

**The walk** (`vis_cull_nodes.comp`). The instance cull hands a visible instance of
a mesh with levels to the walk as its root, unless even the finest group that
consumes level 0 is surely too coarse from the far side of the instance's sphere,
when the cut is level 0 whole and it hands on level 0's meshlets directly; the
walk's steps are dispatches one
after another, each taking two node levels: an (instance, node) work item takes 64
lanes, one per child and grandchild. A lane that takes a group appends the group's
clusters to the phase's meshlet list; one that takes a node at the second level
queues it for the next step. The queues are three in turn: a step reads one, fills
the next and raises that step's indirect arguments itself with an atomic maximum,
and empties the third for the step after the next, so no pass runs between steps.
The frame takes as many steps as the deepest live hierarchy needs (half its node
levels, rounded up), in each phase. Phase 0 tests the frustum; phase 1, which walks
the instances phase 0 deferred, tests this frame's depth pyramid too. A threshold of
0 turns the walk off and every mesh draws level 0's meshlets as before.

**What draws level 0 for now:** the reference renderer, and the shadow rays' and
radiance cache's bottom levels; the sun's virtual shadow map draws cuts of its own
(the amendment below). Each gets the levels in its own step of the phase.

**The mesh interface.** `NphRenderMeshDesc` takes the levels as
`nph_render_cluster_lods_build` makes them: every level's clusters as its meshlets
(level 0's first, `lod_level0_meshlets` of them), each cluster's place, the groups,
their clusters, and the nodes. Creation checks every index the GPU reads, that the
nodes form a tree at most 32 levels deep, that leaves copy their groups' numbers
exactly, and that the nodes' bounds hold their children's; that the levels cut
cleanly is the builder's contract, tested on its own.

## Alternatives considered

- **One node level a step with an arguments pass between steps** (the first
  version): 0.192 ms in `lod_4k_production` (render_perf, the RTX 3060 Ti), against
  0.067 ms now. Empty dispatches put a floor of 2.7 µs under each step and 2 µs under
  each arguments pass; the rest was work done by one thread per node in a chain of
  dependent loads. A lane per child halved it (0.097 ms), the makers' bound took 79%
  of the clusters the meshlet cull rejected out of the lists (94,000 to 20,000; 0.088
  ms), and two levels a step halved the steps (0.067 ms).
- **One persistent dispatch walking a work queue to the end** (Nanite's culling):
  one dispatch a phase instead of one per two levels, but its workgroups wait for
  work other workgroups produce, and Vulkan does not guarantee that a waiting
  workgroup's producer is running: on an implementation that runs them in turn, a
  wait never ends and the device is lost. Rejected while the stepped walk costs
  0.07 ms.
- **One root group for the coarsest level** (the first version): a mesh whose
  levels stop early (Bistro has flat-shaded and foliage meshes of hundreds of
  coarse clusters) put them all in one group, which one lane handed on cluster by
  cluster and the walk could neither cull nor skip in part: 0.185 ms in Bistro's
  street view, 0.035 ms with root groups.
- **One tree over every level's groups:** shallower, but a node would hold groups
  of many levels, so its largest error would come from the coarse ones and its
  smallest maker error from the fine ones, and neither prune would cut much.
- **No hierarchy, a cull of every group of every visible instance:** one dispatch,
  but its cost follows the groups of the visible instances rather than the cut: a
  distant mesh drawn at its coarsest level would test every group it has.
- **Discrete levels per mesh** (a chain of whole-mesh LODs): a large mesh (a
  terrain tile, a cliff) would draw one level for parts a metre and a kilometre
  away, and the switch pops whole meshes.
- **meshoptimizer's simplifier:** a third-party dependency (§3), and the levels need
  what it does not do as a library: borders locked per group, quadrics carried from
  level 0 across levels, and measured errors.
- **Geomorphing between levels:** positions that change every frame would redraw
  the shadow map's cached pages and give still surfaces motion; with a one-pixel
  threshold a switch is under a pixel.

## Consequences

- Rendering cost follows the pixels: in `lod_4k_production` (a terrain of 131,072
  triangles and 1,024 rocks of 65,024 each, 67 million in all) the frame draws 1.15
  million triangles in 18,300 clusters and takes 3.70 ms, against 4.30 million and
  3.99 ms at level 0; culling and raster take 0.70 ms against 0.99. The walk visits
  4,700 nodes in three steps a phase (0.040 + 0.029 ms).
- Bistro, cooked with its levels (551 meshes, 1.75 million triangles; most get 3 to
  8 levels, flat-shaded ones stop early), against itself at level 0, interleaved:
  street +0.03, walk -0.02, turn 0.00, terrace 0.00, hedge -0.02 ms, within the
  runs' spread (0.05 to 0.1 ms). Its geometry is near, where level 0 is the cut;
  the levels' gains are for the landscape, whose far field they bound.
- The tests: the clusters drawn are the CPU's cut, cluster for cluster, for six
  views that mix levels (`the_clusters_drawn_are_the_cpus_cut`); cuts that mix
  levels leave no hole on a terrain sheet and a torus, on both raster paths and both
  rasters (a planted mismatch between the two tests shows thousands of holes there);
  a threshold of 0 draws level 0 byte for byte; an instance revealed in phase 1 draws
  its whole cut; a full queue is reported; broken levels are refused. They pass on
  lavapipe, in Debug with sanitizers, and with CI's glslang 14.
- The build takes about 11 µs a triangle on one thread (3.0 s for 262,144): fine
  for the cooker, which builds each mesh once; the groups of a level are independent
  and will run as jobs when the cooker builds big content.
- The error is geometric only: an attribute seam or material border keeps its
  shape, but normals and texture coordinates inside a coarse level are not measured,
  so shading may differ from level 0 within the geometric bound.
- `max_lod_nodes` (2^18 work items a step) bounds the walk; work past it is dropped
  and reported in `vis_overflow`, which leaves parts of meshes undrawn.
- The cooker stores the levels next. They add about as many triangles again as
  level 0 has (each level halves the one before), in 1.3 to 2 times as many
  clusters (a coarser level's clusters are less full: 592 in all for the test
  torus's 258), with the groups and nodes. Then geometry streaming pages the
  clusters by level, and the shadow map and the bottom levels take coarse cuts of
  their own.

## Amendment (2026-10-26): the sun's shadow map draws cuts of its own

**Context.** The sun's virtual shadow map (ADR-0033) drew every caster at level 0: a
page of a far level, whose texels span metres, rasterised a distant mesh's every
triangle, and the first game's forest seen across its valley would put millions of
triangles into texels that resolve none of them. The camera's cut does not serve the
map: it follows the camera's distance where a page's texels are all one size, it
changes as the camera moves where the map keeps its pages, and a caster out of view
has none.

**Decision.** A level's texels all have the level's size, so a caster drawn into it
needs one error everywhere: the cut at a uniform threshold, which the hierarchy holds
without a walk. A mesh carries its cuts at the powers of two of its own units
(`nph_render_lod_cuts`, made when the mesh is created; at most 32, from the largest
power of two below its smallest error to the first past its largest short of the root
groups'): cut T holds the clusters whose maker's error is at most T and whose
consumer's is more, closed as every cut of the hierarchy is; each cluster's cuts are
a run from its errors' exponents, so making them follows the clusters and the
entries, and a table that would pass 2^24 entries loses its finest cuts first.
`vsm cull instances` takes, for each instance and level, the largest threshold within
half a texel of the level at the instance's scale, from the float's exponent, and
hands on that cut's entries in the table, or level 0's meshlets where the finest
threshold is too coarse; `vsm cull meshlets` reads each entry's cluster. Half a texel
lies within the receiver's offset along its normal (1.5 texels, ADR-0033) with a
texel to spare. The cut follows the instance's scale and the level alone, so cached
pages stay right while the camera moves; an instance's change redraws its pages as
before, and turning the view's levels on or off redraws every page.

**Alternatives considered.**

- **The camera's cut** (the walk's clusters): wrong for the pages of other levels and
  for casters out of view, and it changes with every camera move, which would make
  every cached page stale.
- **The cut's clusters read in the instance cull** (the first version): the
  expansion that hands an instance's meshlets on read each one's entry, a load the
  subgroup waited on every round, and shuffled two more values a round: Bistro's
  street turning paid 14 µs more in `vsm cull instances` (0.030 to 0.044 ms) for no
  gain there. Handing on the entries, read in the meshlet cull among many loads in
  flight, took it back (0.031 ms), and the full redraw's instance cull fell from 0.036
  to 0.026 ms.
- **A walk of the hierarchy per instance and level:** it would find the same uniform
  cut, at the cost of the walk's steps in every frame that draws pages; the table
  replaces it with two reads.
- **Thresholds closer than powers of two:** cuts nearer the error allowed (the power of
  two below it can keep a level more than needed, up to about twice the triangles),
  for a table twice as long or more and a logarithm in the lookup.
- **A coarser error than half a texel:** fewer triangles still, but the cut would
  move shadow edges past the receiver's offset, which self-shadowing would show.

**Consequences.**

- With the sun moving every frame (`lod_shadows_sun_4k_production`, render_perf: the
  terrain and the 1,024 rocks of `lod_4k_production`, all 750 pages drawn again each
  frame), the caster draws fall from 326,586 to 58,039 and the map's raster from 8.08
  to 1.54 ms; the frame from 11.95 to 5.37 ms. A sun that moves every frame still
  redraws every page (ADR-0033's rule), 1.5 ms here against the 0.40 ms shadows line
  of §10.4: a day-night cycle needs its pages kept across small turns of the sun,
  which this amendment does not decide.
- Where pages stay drawn (the camera walking, in that scene and in Bistro's street,
  cooked with levels), nothing measurable changes: the culls within a microsecond and
  the frames within the runs' spread, interleaved (-0.011 and +0.008 ms in Bistro).
- The tables hold about 1.2 entries a cluster (367 u32 for the test torus's 297
  clusters), made when the mesh is created.
- `test_vsm.c` (`a_caster_with_levels_shadows_as_its_level_0`): a torus with levels
  out of view casts its level-0 shadow away from the edges (no pixel of 61,000
  compared differs, 3,261 of them in the shadow; 25 differ along the edges), from 80
  (meshlet, page) draws against 162, and turning the levels on redraws every page;
  with three of the cut's clusters dropped, 15 pixels inside differ.
