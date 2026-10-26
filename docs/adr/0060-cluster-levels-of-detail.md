# 0060. Virtualized geometry: a hierarchy of cluster levels, walked two levels a step

- **Date:** 2026-10-26
- **Status:** Accepted (the shadow map's pages and the shadow rays draw level 0 until
  their own levels arrive later in Phase 13; the cooker stores the levels in the next
  step of the phase)

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
or when a level takes off less than a tenth of the triangles. The build is
deterministic.

**The hierarchy.** Each level's groups, in Morton order, lie under a tree of nodes
of eight children; the levels' trees and the root group lie under node 0. A child
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
a mesh with levels to the walk as its root; the walk's steps are dispatches one
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

**What draws level 0 for now:** the reference renderer, the sun's virtual shadow
map's pages, and the shadow rays' and radiance cache's bottom levels. Each gets the
levels in its own step of the phase.

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
