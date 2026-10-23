# 0042. Physics: module boundaries, 64-bit bodies, and the collision layer

- **Date:** 2026-10-22
- **Status:** Accepted

## Context

CLAUDE.md §8.9 asks for sphere, capsule, box, convex hull, triangle mesh,
heightfield, and compound shapes; a dynamic AABB tree broadphase with fat
boxes, incremental refit, and SAH rebuilds; a narrow phase of GJK with EPA,
the separating axis test for boxes and hulls (Gregorius, GDC 2013), and
contact manifold reduction and persistence (Gregorius, GDC 2015); then a
substepped soft-step solver, joints, continuous collision, queries, and a
character controller, all at a fixed 240 Hz within 1.0 ms of job time per
step, deterministic run to run. Phase 11 ends when stacking, ragdoll, and
determinism tests pass and benchmarks are recorded (§12).

This ADR records the decisions of the first part, the collision layer:
what a shape is, how precision is kept in a 64-bit world, and how pairs
collide. The world and the solver have their own ADRs.

Constraints from the rest of the plan:

- The module order (§4) puts `physics` at the rank of `scene` and
  `asset`: it may use `core`, `math`, `platform`, `jobs` (and, in
  principle, `gpu` and `render`), never `scene`.
- World positions are 64-bit on the CPU (§8.2); the scene's transforms
  already are (ADR-0020).
- No hidden allocation (§1.4), none per frame in steady state (§10.5).
- Determinism for identical inputs (§2), for any worker count, which the
  scheduler already guarantees for the ECS (ADR-0020 §8).
- Scalar and SIMD math are bit-identical and FMA contraction is off
  (ADR-0008 §6).

## Decision

1. **Physics knows nothing of the ECS or the renderer.** The runtime owns
   the bridge (components, the step in the frame, the transform sync), so
   physics is testable headless and usable without a world of entities.
   Debug drawing goes out through a callback interface the runtime
   connects to the render debug-draw API, rather than a dependency on
   `render` that would pull Vulkan into the physics tests.
2. **Bodies are 64-bit; everything else is relative 32-bit.** A body's
   position is an `NphDVec3`. Shapes are placed in their body with a
   32-bit pose. Collision runs on 32-bit poses in a frame centred on one
   of the two bodies, so coordinates are at most a few metres and keep
   full precision at any distance from the origin; contacts are expressed
   relative to the centres of mass, which is what the solver uses.
   Broadphase boxes are 32-bit, computed relative to the body and rounded
   outward when converted, so they stay conservative where 32-bit
   resolution coarsens (an ulp is 1 mm at 8 km, 6 cm at 1000 km).
3. **Geometry is canonical, placed by a pose.** A sphere is centred on its
   origin, a capsule's core runs along Y (the axis of a standing character
   in a Y-up world), a box is axis-aligned, and a hull is in the frame of
   the points it was built from. Collision functions take two geometries
   and two poses in one frame and return the manifold in that frame.
4. **Hulls are immutable, shareable, position-independent blobs.** One
   allocation holds a 128-byte header and the arrays, located by byte
   offsets: vertices, outward unit planes, a half-edge mesh with twins at
   `2k, 2k+1`, each face's first half-edge, plus bounds, centroid, volume,
   inertia at unit density, inner and outer radii, and the build
   tolerance. No pointer is stored, so a cooked hull can be used straight
   from a memory-mapped file (§8.8). The caller owns the hull and keeps it
   alive while shapes use it.
5. **The builder is Quickhull with face merging.** Barber, Dobkin, and
   Huhdanpaa's algorithm, in double precision on the 32-bit input, with
   the two-pass merge rules of Lloyd's QuickHull3D (non-convex with
   respect to the larger face, then with respect to either) so coplanar
   triangles become one polygon and no face is thinner than the
   tolerance, `3 * FLT_EPSILON * (max|x| + max|y| + max|z|)` by default.
   Points are added farthest first, so a vertex budget (64 by default, at
   most 256) keeps the most significant points. Degenerate clouds are an
   error, not a flat hull. The same points give the same bytes.
6. **Pairs get the cheapest exact method.** Spheres and capsules use closed
   forms on points and segments, with two points for parallel capsules.
   Box and sphere use the clamp. Box and box use the fifteen axes of the
   oriented-box test (Gottschalk et al., SIGGRAPH 1996), whose projection
   radii are closed forms. Hulls (and boxes against hulls) use Gregorius's
   separating axis test: every face normal, and the edge pairs whose
   Gauss-map arcs intersect, which yields the exact separation or
   penetration of two convex polyhedra; the edge-pair search runs four
   arcs per SSE instruction on structure-of-arrays data, bit-identical to
   the scalar loop. Capsules against hulls test the segment's Gauss map (a
   great circle) against the hull's arcs.
7. **Face contacts clip; edge contacts are a point; separated pairs ask
   GJK.** A face contact clips the incident face (the most anti-parallel
   one) against the side planes of the reference face and keeps the points
   within the speculative distance, reduced to four (deepest, farthest,
   largest triangle, most added area). Points sit midway between the
   surfaces. The first shape's face is preferred unless the second's
   separates by a tenth of a slop more, and faces are preferred over edge
   pairs unless the edge pair separates by half a slop more, so contacts do
   not flip between steps. When the polyhedra are apart, the axis test
   only bounds the gap from below (corners approach each other farther
   apart than any axis says), so GJK measures it: if its features disagree
   with the axis test's face, or the axis is an edge pair, the contact is
   GJK's single closest-point pair.
8. **GJK uses signed volumes; EPA is for penetration queries.** GJK's
   closest point per simplex comes from the signed-volumes subalgorithm of
   Montanari et al. (ACM TOG 2017), which measures barycentric coordinates
   in the coordinate plane of largest projection rather than dividing by
   determinants that vanish (the classic single-precision failure of
   Johnson's algorithm). Shapes are proxies, points rounded by a radius,
   and a simplex cache warm-starts coherent queries. EPA (van den Bergen,
   GDC 2001) grows GJK's final tetrahedron to the face of the Minkowski
   difference nearest the origin, for `nph_physics_penetration` (shape
   casts that start inside, general convex fallbacks).
9. **Manifolds are four points with stable ids.** One unit normal from the
   first shape to the second, up to four points with separations (negative
   overlapping) and 32-bit ids hashed from the features that made them, so
   the solver can carry each point's impulses to the next step. Points
   farther apart than the speculative distance (four slops by default) are
   not reported. A per-pair cache (8 bytes) remembers the separating axis
   so a pair that is still apart is rejected after one axis.
10. **The broadphase tree follows Catto (GDC 2019).** Insertion picks the
    sibling of least added surface area by a greedy descent bounded by the
    inherited cost (Bittner et al., I3D 2012); refits rotate a child with a
    grandchild when that shrinks the area; small motions enlarge a proxy in
    place and mark the path; a rebuild, full or of the marked subtrees only,
    is top-down binned SAH (Wald 2007) on an explicit stack. Nodes carry the
    union of their leaves' category bits for filtered queries. The same edits
    build the same tree.

## Alternatives considered

- **GJK/EPA for every convex pair, with a convex radius** (Jolt, Havok):
  shapes are shrunk by a few centimetres and rounded, so resting contacts
  have separated cores and GJK, warm-started, finds them in a few hundred
  nanoseconds. It is the fastest known approach for complex hulls, but it
  rounds every corner and edge by the radius, and §8.9 specifies the
  separating axis test. The axis test is exact for sharp polyhedra; its cost
  for large hulls is the open item below.
- **Johnson's distance subalgorithm.** It is what most GJK code uses, and
  what fails in single precision on nearly degenerate simplices.
- **f32 positions with origin rebasing.** That would couple every query and
  every body to a moving origin and lose precision near the edges of a
  region; §8.2 chose 64-bit positions and the scene already uses them.
- **Shapes with built-in placement** (Box2D's polygons store their own
  vertices in body space): fewer parameters, but every pair function would
  handle offsets, and hulls could not be shared between placements.
- **Sweep-and-prune or grids for the broadphase.** Sweep-and-prune
  degrades with clustered motion along its axes and large worlds; grids
  need a cell size that fits every object. A dynamic tree adapts to both,
  and is what §8.9 specifies.

## Consequences

- Measured on the reference machine (Profile, `benchmarks/physics`):
  spheres 24 ns, capsules 50 ns, box and sphere 28 ns, box and box 287 ns in
  random overlap and 450 ns resting (four points from an octagon), a
  separated pair rejected by its cached axis 57 ns, GJK between 32-vertex
  hulls 849 ns cold and 204 ns warm, EPA 3.4 us, a 10k-proxy tree query
  71 ns and a 50 m ray 851 ns, Quickhull of 1000 points 311 us.
- **Hull against hull is the open cost**: 9.1 us for two 32-vertex hulls in
  contact, dominated by the brute-force pass over all edge pairs (about
  8000 Gauss-map tests). Bucketing B's arcs on the Gauss map so each arc of
  A meets only its neighbours is the planned optimisation, measured with the
  world's scenes; until then, keep hulls small (16 to 32 vertices for
  props) and prefer boxes and capsules where they fit.
- Deep penetrations (most of a shape's size) can clip away the deepest
  corner when it lies beside the reference face: the manifold then reports
  less depth than the axis does, and the solver resolves it over a few steps
  as the contact becomes shallow. Shallow contacts, the normal case, carry
  the exact depth (tested against EPA).
- The cache lives with the pair; the world keeps one per contact.
