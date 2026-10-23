# 0046. Physics: triangle meshes and heightfields

- **Date:** 2026-10-23
- **Status:** Accepted (2026-10-23); meshes, heightfields, and compounds built

## Context

CLAUDE.md §8.9 lists triangle meshes, heightfields, and compounds among the
shapes. Levels and terrain are made of them: in a game most dynamic bodies
rest on the world's static geometry most of the time, so contacts against
meshes and heightfields are the common case, not the exception, and must
run on the solver's eight-lane path at full speed.

Three problems are particular to them:

1. **Many normals per pair.** A box lying across a crease between two
   triangles, or in a corner of three, needs a contact normal per surface.
   The world holds one manifold (a normal and up to four points) per shape
   pair.
2. **Internal edges.** A body sliding across the shared edge of two
   coplanar triangles can hit that edge (a normal pointing sideways) and
   bump: the "ghost collision" every mesh engine must suppress.
3. **Size.** A level mesh has millions of triangles, a terrain millions of
   samples; neither may cost per step more than the triangles under the
   bodies that touch it.

## Decision

1. **Two shape types, static and kinematic only.** `NPH_PHYSICS_SHAPE_MESH`
   holds a shared, immutable `NphPhysicsMesh`; `NPH_PHYSICS_SHAPE_HEIGHTFIELD`
   a shared `NphPhysicsHeightfield`. Neither has mass, so a dynamic body may
   not carry them (shape creation refuses it). They collide with spheres,
   capsules, boxes, and hulls, not with each other (both ends would be
   static or kinematic). Kinematic carriers make moving platforms.
2. **Blocks like hulls.** Each is one block of memory, its arrays located by
   byte offsets, with no pointers: copyable, cookable, and usable from a
   memory-mapped file (§8.8), validated by a function that the fuzzers run
   (malformed input never crashes: §8.8 "Robustness").
3. **The mesh: indexed triangles under a four-wide BVH.** Vertices (f32, in
   the mesh's frame), triangles (three u32 indices, in leaf order), each
   triangle's original index (for the game's materials and hits), and
   per-triangle active-edge flags. The tree is built by binned SAH
   (Wald, "On Fast Construction of SAH-based Bounding Volume Hierarchies",
   2007) into a binary tree with leaves of at most four triangles, then
   collapsed into four-wide nodes whose child boxes are stored as
   structure-of-arrays, so one node is tested against a box or a ray in a
   few SIMD instructions (Wald, Benthin, and Boulos, "Getting Rid of
   Packets", 2008; Dammertz et al., "Shallow Bounding Volume Hierarchies",
   2008). The builder welds coincident vertices (a glTF mesh duplicates them
   at hard edges) and drops degenerate triangles.
4. **The heightfield: a grid of samples.** Heights (f32) at regular spacing
   in X and Z, a cell split into two triangles along one diagonal, a hole
   bit per cell, and the minimum and maximum height of each block of 8 by 8
   cells, so a box over the terrain finds its cells by arithmetic and a ray
   skips empty blocks.
5. **Active edges** (Jolt's, after Bullet's internal-edge utility): the
   mesh builder marks an edge active when it borders one triangle (an open
   edge), more than two, or two whose dihedral angle is convex by more than
   5 degrees; flat and concave edges are inactive. A contact whose normal
   is not the triangle's own, at a point on inactive edges or their
   vertices only, takes the triangle's normal: across a flat seam the
   neighbour's face carries the body, and nothing pushes sideways. The
   heightfield computes the same flags for its cells.
6. **One-sided by default.** A triangle collides from its front (its
   vertices counter-clockwise seen from there, right-handed); a body behind
   it is not pushed through to the front. A mesh may be built two-sided
   (open sheets: fences, cloth-thin walls). Heightfields are one-sided, up.
7. **Convex against a triangle**: spheres and capsules by closest points
   (Ericson, "Real-Time Collision Detection", §5.1.5 and §5.1.9, a capsule
   giving two points when it lies along the face); boxes and hulls by the
   separating axis test against the triangle as a flat polyhedron (its
   normal, the hull's faces, and the edge pairs whose Gauss-map arcs cross:
   a triangle's edge spans the half circle from its normal to the reverse,
   whose plane is the edge's direction, which replaces the degenerate cross
   product of the two face normals in Gregorius's test), then clipping the
   incident face against the reference face. Point ids mix the triangle's
   original index with the feature, so warm starting survives the step.
8. **Manifolds by normal.** The triangles under a body give one manifold
   each; manifolds whose normals agree within 5 degrees merge into one
   (Jolt's manifold reduction), the merged points reduced to four
   (Gregorius 2015). A pair keeps at most four manifolds, the ones with the
   deepest points.
9. **Child contacts.** A mesh or heightfield pair's contact (the parent,
   found by the broadphase as any pair is) owns three child contacts,
   created with it, for its second to fourth manifolds. Children are
   ordinary contacts to the graph, the islands, and the solver: they take
   their own colours (a pair's manifolds share a body, so they never share a
   colour), so the solver, its kernels, and their speed are unchanged. The
   narrow phase skips them; the parent's update writes every manifold, and
   the serial pass after it syncs the children's touching state and graph
   membership. Events stay per pair: the parent reports beginning and
   ending to touch (any manifold touching), and one hit for the pair.
10. **Queries.** A mesh and a heightfield answer box overlaps (the triangles
    under a box) and raycasts (the closest hit: the watertight test of Woop,
    Benthin, and Wald, JCGT 2013, so a ray never slips through an edge two
    triangles share), with the triangle's original index, the point, and
    the normal. The world's queries (raycasts, shape casts, overlaps) build
    on them.
11. **Compounds are bodies with several shapes**, as in PhysX and Box2D: each
    shape is placed by its pose on the body, the body's mass is the sum of
    its shapes', and each shape has its own broadphase proxy and contacts. A
    compound shape type with its own tree (Jolt's, whose bodies hold one
    shape) is not needed while a body's shapes are counted in tens.

## As built: meshes (2026-10-23)

The mesh half is in the engine (`mesh.h`, `collide_mesh.c`, the world's
child contacts; docs/physics.md "Meshes"); heightfields (§4) and the
world's queries (§10's last sentence) follow. Building it refined the plan:

- **Triangle contacts keep every clipped point** (up to sixteen), and each
  merged manifold is reduced once: a box's face clipped to a triangle has
  up to seven points, and reducing each triangle's contact to four before
  merging lost corners of the whole contact.
- **A corrected normal remakes the contact** (§5). A contact that takes
  the face's normal is made again against the face along it, as Jolt's
  manifold between two faces is: the shape's supporting feature clipped to
  the triangle. A shape not over the face gets nothing from that triangle
  (its neighbour carries it), and a remade contact deeper than the axis
  test's by more than a slop is dropped (a box straddling a platform's
  plane beside its open edge would be lifted onto it). Spheres and
  capsules place their points under the core along the final normal, so
  no point beside a rolling ball brakes it, and a sphere keeps one point
  per manifold.
- **Merging (§8) refined.** A contact joins the cluster whose first
  member's normal agrees within 5 degrees; the cluster's normal is its
  members' average, each weighted by its depth. The plan's deepest member's
  normal flipped between neighbouring triangles' from step to step on
  curved ground and rocked boxes that never slept. Copies of a point from
  triangles either side of a seam (within 0.1 mm) are one point, and a
  cluster of 32 points is compacted to its deepest and the extremes of its
  touching points along eight directions and of its speculative points
  along four.
- **Touching points first**, in every manifold's reduction: points that
  touch (within a tenth of a slop) are kept before speculative ones, and
  four or more touching are the only candidates. Trading a supporting point
  for one merely near the surface let boxes on a curved mesh rock between
  two pairs of supports.
- **Islands split by Box2D's rule.** An island that lost constraints is
  split once any of its bodies is ready to sleep (the island whose body has
  been still the longest, one a step). Waiting for every body, as before,
  let one sliding off the edge of a terrain keep the pile it had left
  awake: 38 of 40 boxes dropped on a terrain now sleep.
- **Warm starting by place.** A point whose id changed takes the impulses
  of an unclaimed old point within 1 cm on both bodies (Jolt's matching by
  position, here behind the ids). The reduction's tie rule above made ids
  flip between near-coincident clipped corners from step to step: a
  settling column of ten boxes lost a twentieth of its warm starts and
  drifted 2 to 5 mm aside (0.04 mm before the rule; 0.09 mm with the
  fallback). In a mesh pair the old point's manifold must also face within
  5 degrees of the new one's: a crease's two manifolds share a corner, and
  on a terrain, impulses passed between manifolds 5 to 15 degrees apart
  kept boxes creeping for seconds (36 of 40 asleep after 8 s, against 38).
  A convex pair's one manifold needs no angle: the same limit there refused
  tumbling limbs' contacts and sent a chaotic ragdoll pile three times
  deeper into the floor at its worst. Carrying every warm start changed the
  solver comparison's stacks (ADR-0045, re-measured).
- **Child contacts as planned (§9)**, kept out of the pair map, the bodies'
  contact lists, and the awake list: the parent's update writes every
  record and carries impulses across all four. A pair reports at most one
  hit a step, at the fastest-approaching point of any manifold.
- **Validation and fuzzing** (the plan's consequence below, now done for
  hulls and meshes). `nph_physics_hull_validate` takes the block's size,
  as the mesh's does, instead of trusting the header's, and both bound
  what the module's arrays and arithmetic assume: counts (a hull's faces
  at most 2V − 4), coordinates within `NPH_PHYSICS_MAX_COORDINATE`
  (100 km, also enforced by the builders and on primitive sizes), finite
  header numbers (a NaN tolerance or vertex passes every comparison), a
  mesh's empty slots exactly the builder's inverted box (the overlap query
  would take another for a child, and read node 0xFFFFFFFF), and a tree no
  deeper than the queries' stacks in any order of visit. The fuzz targets
  `physics_hull` and `physics_mesh` build from the input, use the block,
  then damage a copy; they also found Quickhull leaving a sliver's merged
  face reflex beyond its tolerance, which the builder now builds again at
  four, then sixteen times the tolerance.
- **Known: rolling contacts lose support within a step.** A rolling
  shape's contact anchors are material points, which rise off the surface
  as the shape turns (r (ω h)² / 2 over the step), so the later substeps
  find the contact apart, and a rolling ball's stored normal impulse is
  zero with an end-of-step vertical velocity of −g h / 2 (Box2D v3 shares
  this). At 240 Hz it is harmless (balls and capsules roll across seams
  within 1 mm of height); re-anchoring rolling contacts each substep is a
  follow-up for fast rollers.

## As built: heightfields and compounds (2026-10-23)

- **The grid (§4) as planned**, with each cell's diagonal chosen by the
  builder's input (flipped cells), and the active edges by the mesh
  builder's rule, in 64 bits: the two builders mark the same edges on the
  same triangles (tested).
- **No tree**: a box's cells by arithmetic, a ray's by walking blocks and
  then cells (Amanatides and Woo 1987). Where the ray leaves a cell through
  a corner, to rounding, both neighbours across it are tested: the walk
  steps the ray where the watertight test shears it, and at a corner on a
  block's side the walk skipped the cell the test found the hit in.
- **One narrow phase for both surfaces**: `collide_mesh.c` takes any
  source of triangles, so a heightfield collides as the same triangles
  built as a mesh (tested). A capsule's merged manifold keeps its deepest
  point and its ends along its axis (four points in a line came over a
  heightfield's uniform diagonals, which the reduction keeps whole).
- **Compounds (§11) as planned**, now tested: mass summed from posed
  shapes, a table standing on four legs on a box ground and on a
  heightfield, a compound's own shapes never meeting.
- **Fuzzed** like the other blocks (`physics_heightfield`).

## Alternatives considered

- **One manifold per pair, the best normal**: a box in a crease gets one
  face's normal and sinks into, or jitters on, the other.
- **A normal per contact point** (every point its own normal and tangents):
  every contact in the world would carry three times the data to the
  solver, to serve the minority of points that disagree.
- **A contact per triangle** (Bullet's per-triangle manifolds): a box on a
  finely tessellated floor would carry dozens of contacts that say the same
  thing.
- **Two-sided by default**: a body pushed through a wall would be pushed on
  through it, out of the level.
- **Quantized nodes and vertices** (Jolt's half-float tree): half the
  memory, measured later against the f32 tree's speed.
- **A heightfield as a mesh**: twice the memory and none of the grid's
  arithmetic.

## Consequences

- Meshes and heightfields cost per step what the triangles under each body
  cost, found in a BVH or by grid arithmetic; contacts against them are
  ordinary eight-lane contacts.
- A pair against a mesh holds up to four contacts (sixteen points); a pair
  of convex shapes stays one.
- Mesh and heightfield blobs are validated before use from a file, and
  fuzzed, with the hull's validation (done for hulls and meshes: above).
