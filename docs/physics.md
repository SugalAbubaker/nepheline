# Physics (`engine/physics`)

The physics module (CLAUDE.md §8.9) simulates rigid bodies: collision
geometry, the broadphase and narrow phase, the solver, joints, continuous
collision, queries, and the character controller, stepped at a fixed
240 Hz on the job system. It depends on `jobs` and through it on
`platform`, `math`, and `core`; it does not know the ECS (the runtime
bridges entities to bodies) or the renderer (debug drawing goes out through
a callback). Decisions: [ADR-0042](adr/0042-physics-collision.md).

Phase 11 builds it in parts. This document describes what exists:

| Part | Status |
|---|---|
| Geometry, convex hulls, the narrow phase, the AABB tree | done (this page) |
| The world: bodies, contacts, the soft-step solver, islands, sleep | next |
| Joints, the solver comparison (§13), meshes, heightfields, compounds, queries, continuous collision, the character controller, the runtime bridge | to come |

| Header | Contents |
|---|---|
| `types.h` | units, tolerances, rigid poses, mass properties |
| `geometry.h` | the shape types, bounds, mass, support points, GJK proxies |
| `hull.h` | the convex hull block and the Quickhull builder |
| `collide.h` | contact manifolds, GJK distance, EPA penetration, segment distance, manifold reduction |
| `aabb_tree.h` | the dynamic AABB tree |

## Conventions

SI units: metres, kilograms, seconds, radians. Y is up (ADR-0008), so
gravity points along -Y.

**Precision.** A body's position in the world is 64-bit (`NphDVec3`), as
the scene's transforms are (CLAUDE.md §8.2). Everything else is 32-bit and
relative: a shape's pose in its body, collision in a frame centred on one
of the two bodies, the solver's anchors relative to the centres of mass.
Contacts only exist between nearby shapes, so the relative numbers are a
few metres at most and keep full precision anywhere in the world.

**Tolerances** (`types.h`): contacts may keep `NPH_PHYSICS_LINEAR_SLOP`
(5 mm) of penetration, which keeps resting contacts from flickering;
contact points are reported up to `NPH_PHYSICS_SPECULATIVE_DISTANCE` (four
slops) apart, so the solver can stop an approaching body before it
penetrates (speculative contacts, Catto 2013); broadphase boxes are
fattened by `NPH_PHYSICS_AABB_MARGIN` (10 cm).

**Poses** (`NphPhysicsPose`) are a unit quaternion and a translation,
32-bit. Mass properties (`NphPhysicsMassData`) are a mass, a centre of
mass, and the inertia tensor about it, in one frame; `nph_physics_mass_transform`
moves them to a parent frame and `nph_physics_mass_add` sums them with the
parallel axis theorem.

## Geometry

Every geometry is canonical in its own frame and placed by a pose:

| Type | Shape |
|---|---|
| sphere | a ball centred on the origin |
| capsule | the segment from (0, -h, 0) to (0, +h, 0) swept by a ball; Y is the long axis, as for a standing character |
| box | axis-aligned, centred, with half extents |
| hull | a shared convex polyhedron (below) |

`nph_physics_geometry_aabb` gives the tight box of a placed geometry (for
a box, |R| times the half extents: Arvo, Graphics Gems 1990),
`nph_physics_geometry_mass` the mass properties at a density (closed forms
for the primitives; the hull's are precomputed by its builder), and
`nph_physics_geometry_support` the farthest point along a direction. A
**proxy** (`NphPhysicsProxy`) describes a convex shape to GJK as the hull of
a few points rounded by a radius: a sphere is one point, a capsule two, a
box its corners, a hull its vertices.

## Convex hulls

A hull (`NphPhysicsHull`) is one block of memory: a 128-byte header and its
arrays, which the header locates by byte offsets.

| Array | Element |
|---|---|
| vertices | `NphVec3` |
| planes | `NphPlane` per face: outward unit normal, n.p + d = 0 on the face |
| faces | the index of one half-edge per face |
| edges | `NphPhysicsHullEdge`: next, twin (always the index with its low bit flipped), origin vertex, face |

The header adds the bounds, the centroid and volume, the inertia about the
centroid at density 1, the inner radius (the smallest centroid-to-face
distance) and outer radius, and the build tolerance. The block holds no
pointer, so it can be copied, written into a cooked file, and used straight
from a memory-mapped one. Faces are convex polygons, not triangles: a box
has six faces and twelve edges, which is what the separating axis test and
contact clipping want. `nph_physics_hull_validate` checks every invariant
(twins, closed loops, planar convex faces, every vertex behind every plane,
Euler's formula, offsets inside the block), for tests and for blobs read
from files.

**The builder** (`nph_physics_hull_build`) is Quickhull (Barber, Dobkin, and
Huhdanpaa, ACM TOMS 1996), run in double precision on the 32-bit points:

1. The initial tetrahedron from the extreme points: the widest pair along
   an axis, the point farthest from their line, the point farthest from
   their plane. A cloud without volume within the tolerance is an error.
2. Each point goes to the conflict list of the face it is farthest above.
3. Repeatedly, the farthest conflict point of any face becomes the eye:
   the faces that see it are deleted (a depth-first walk that collects the
   horizon edges in order, on an explicit stack), a triangle fans from each
   horizon edge to the eye, and the new faces merge with neighbours that
   are non-convex within the tolerance: first with respect to the larger
   face of each pair, then with respect to either (John Lloyd's
   QuickHull3D rules). Merging collapses vertices that end up between two
   edges of the same pair of faces, and slivers get their normal from their
   longest edge. The deleted faces' points go to the new faces they are
   above, or are dropped as inside.
4. The live faces become the block; the mass integrals are Eberly's
   ("Polyhedral Mass Properties (Revisited)") over each face's fan.

Choosing the farthest point globally adds volume fastest, so the vertex
budget (`max_vertices`, 64 by default, at most 256) keeps the hull of the
most significant points: an inner approximation that loses the least. The
default tolerance is `3 * FLT_EPSILON * (max|x| + max|y| + max|z|)`, the
round-off bound of a plane distance at the cloud's scale. Build from points
near the origin: the hull's frame is the points' frame, and coordinates far
from it cost collision precision. The build is deterministic: the same
points give the same bytes.

`nph_physics_hull_make_box` makes a box's hull directly, with its exact
topology.

## Collision

`nph_physics_collide` gives the contact manifold of two placed geometries:
a unit normal from the first shape to the second and up to four points,
each midway between the surfaces, with its separation along the normal
(negative where they overlap) and an id hashed from the features that made
it. Pairs farther apart than the speculative distance get no points.
Arguments in either order work; reversing them reverses the normal.

| Pair | Method |
|---|---|
| sphere, sphere | centres |
| capsule, sphere | nearest point of the core segment |
| capsule, capsule | segment closest points (Ericson §5.1.9); nearly parallel cores that overlap along their length get two points, at the ends of the overlap |
| box, sphere | the clamp (Ericson §5.1.3); inside, out through the nearest face |
| box, box | the fifteen axes of the oriented-box test (Gottschalk et al., SIGGRAPH 1996), then the contact as for hulls |
| hull or box, sphere | face planes; outside every plane's reach, GJK point to hull |
| hull or box, capsule | face planes and the edges whose Gauss-map arcs cross the segment's great circle; faces clip the segment (two points), edges give one |
| hull, hull or box | Gregorius's separating axis test (GDC 2013) |

**The separating axis test** tries every face normal of both polyhedra and
the cross products of the edge pairs whose Gauss-map arcs intersect, the
faces of the Minkowski difference; together they contain the axis of least
penetration (or greatest separation) of two convex polyhedra, so the test
is exact. For an edge pair forming a Minkowski face the separation is one
dot product, so the cost is in finding the pairs: B's edges are prepared in
A's frame as structure-of-arrays data, and the arc test runs on four of
them per SSE instruction with the scalar path's arithmetic, so both decide
identically. Nearly parallel edges (sine below 0.005) are skipped: their
cross product is no reliable axis, and the face normals cover them.

**Choosing the contact.** The first shape's face wins unless the second's
separates by a tenth of a slop more, and faces win over edge pairs unless
the edge pair separates by half a slop more: a tie must not flip between
steps. A face contact clips the incident face (the other polyhedron's face
most anti-parallel to the reference normal) against the side planes of the
reference face (Sutherland and Hodgman 1974), keeps the points within the
speculative distance, and reduces them to four (Gregorius, "Robust Contact
Creation for Physics Simulations", GDC 2015): the deepest, the farthest
from it, the one that makes the largest triangle with both, and the one
farthest outside that triangle. An edge pair gives one point, from the
closest points of the two edges.

**Apart, GJK decides.** When the shapes are separated by more than a slop,
the axis test only bounds the gap from below (two corners approaching each
other are farther apart than any face or edge axis says), and a speculative
contact with too small a separation would stop a body short of the
surface. GJK measures the gap exactly; when its closest features are not
the axis test's face (the normals differ by more than about 6 degrees), or
the axis is an edge pair, the contact is GJK's closest-point pair.

**Temporal coherence.** A pair's `NphPhysicsCollideCache` remembers the
axis that separated it; while that axis still separates the pair by more
than the speculative distance, the test ends after it (57 ns for two boxes
against a full test's 300).

**Deep penetration.** When one shape is far inside the other (most of its
size), clipping can remove the deepest corner where it lies beside the
reference face, and the manifold reports less depth than the axis. The
solver resolves such overlaps over a few steps as they become shallow;
shallow contacts carry the exact depth (tested against EPA).

### GJK and EPA

`nph_physics_distance` is GJK (Gilbert, Johnson, and Keerthi 1988) between
two proxies, with or without their radii. Each simplex's closest point to
the origin comes from the signed-volumes subalgorithm of Montanari,
Petrinic, and Barbieri ("Improving the GJK Algorithm for Faster and More
Reliable Distance Queries Between Convex Objects", ACM TOG 2017): the
barycentric weights are ratios of signed lengths, areas, and volumes
measured in the coordinate axis or plane where the projection is largest,
which avoids dividing by vanishing determinants, the classic failure of
Johnson's subalgorithm in single precision. The loop ends on a repeated
support pair, on van den Bergen's relative progress test, on a lack of
progress, or when a tetrahedron contains the origin. A
`NphPhysicsSimplexCache` restarts from the previous simplex: four times
fewer iterations on coherent queries (204 ns against 849 ns for 32-vertex
hulls).

`nph_physics_penetration` adds EPA (van den Bergen, GDC 2001) when the
cores overlap: GJK's final simplex is grown to a tetrahedron with volume
(touching shapes can end GJK on a point, segment, or triangle through the
origin), then the face nearest the origin is repeatedly replaced by the
faces from the support point along its normal to its horizon, until the
support adds less than 10 µm or 0.01 %. The result is the normal and depth
of the smallest separating translation and the deepest points of each
shape. When only the rounding overlaps, GJK's closest points answer
directly.

## Broadphase

`NphAabbTree` is the dynamic bounding volume hierarchy of Catto's "Dynamic
Bounding Volume Hierarchies" (GDC 2019):

- **Insertion** pairs the new leaf with the sibling that adds the least
  surface area to the tree: a greedy descent that keeps the cost the
  ancestors would inherit, takes a child's growth as a lower bound, and
  stops when no subtree can beat the best found (Bittner, Hapala, and
  Havran, I3D 2012).
- **Refits** walk up from every change and consider rotating a child with a
  grandchild on the other side when that shrinks the area: Catto's
  rotations, which keep incremental trees close to rebuilt ones.
- **Enlargement** grows a proxy's box in place, grows its ancestors, and
  marks the path: a small motion costs O(depth) and no restructuring.
- **Rebuilds** are top-down binned SAH (Wald, "On fast Construction of
  SAH-based Bounding Volume Hierarchies", RT 2007): sixteen bins on the
  longest centroid axis, on an explicit task stack. A partial rebuild
  places only the marked region, keeping the untouched subtrees whole.
- **Queries** walk a fixed 1024-entry stack: boxes, and rays that visit the
  nearer child first and clip as hits come in. Each node carries the union
  of its leaves' category bits, so a filtered query skips subtrees with
  nothing it wants.

The same edits build the same tree, bit for bit.

## Performance

Profile build, Ryzen 7 5800X, `benchmarks/physics` (each collision row
cycles through 64 prepared configurations):

| Benchmark | ns/op |
|---|---|
| `collide_spheres` | 24 |
| `collide_capsules` | 50 |
| `collide_box_sphere` | 28 |
| `collide_box_box_random` (overlapping, random poses) | 287 |
| `collide_box_box_resting` (four points from an octagon) | 450 |
| `collide_box_box_cached_apart` | 57 |
| `collide_hull_hull_32v` | 9 100 |
| `collide_hull_capsule_32v` | 720 |
| `gjk_hull_hull_32v` / `_cached` | 849 / 204 |
| `epa_box_hull_32v` | 3 370 |
| `tree_insert_10k` (insert and remove) | 455 |
| `tree_move_10k` | 1 690 |
| `tree_query_10k` (2 m box) | 71 |
| `tree_raycast_10k` (50 m, closest hit) | 851 |
| `tree_rebuild_10k` (per proxy) | 212 |
| `hull_build_1000_64v` | 311 000 |

Hull against hull is dominated by the edge-pair pass, about 8000 Gauss-map
tests for two 32-vertex hulls; the planned optimisation buckets B's arcs on
the Gauss map so each arc of A meets only its neighbours, measured with the
world's scenes. Until then, hulls of 16 to 32 vertices suit props.

## Testing

`physics_tests` (47 tests):

- Hulls on known solids (box, tetrahedron and its inertia, cylinder with
  merged caps), random clouds (every point inside, every vertex an input
  point), budgets (monotonic volume), lattices at four scales and noisy
  spheres at three (the classic degenerate inputs), degenerate and invalid
  input, far-from-origin coordinates, determinism, and validation of seven
  kinds of corrupted blob.
- Geometry: mass properties against a numeric integration and closed
  forms, the parallel axis sum and rotation, tight bounds, proxies against
  support points.
- GJK against closed forms and brute force, warm starts, rounded cores;
  EPA against the separating axis test on random overlapping pairs (each
  method's depth is the separating translation along its own normal, and the
  axis test is within its face preference of EPA's minimum).
- Manifolds for every pair type on known configurations, the box view
  against the box hull (same topology, same results), argument order,
  geometric soundness on random hull pairs (each point's surface points lie
  on or in both shapes) and on 100-vertex hulls (the scalar path), id
  persistence under small motion, the cache, and the reduction.
- The tree against brute force through random edits, moves, enlargements,
  category changes, and rebuilds; raycasts (every hit and the closest);
  partial rebuilds; determinism.
