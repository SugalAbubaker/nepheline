# 0047. Physics: queries

- **Date:** 2026-10-23
- **Status:** Accepted (2026-10-23)

## Context

CLAUDE.md §8.9 lists queries (raycast, shape cast, overlap) and builds the
character controller and continuous collision on them. Gameplay asks a
world what a bullet meets, what a sweep of a character's capsule meets,
and what lies inside an explosion; the controller asks every substep where
its capsule may go and which planes hold it; continuous collision asks
when a fast body first meets the static world. The answers must be:

1. **exact enough to stand on.** A capsule moved to a cast's hit and cast
   again must neither sink in nor stop short; a box landing flat must get
   the face's normal, not one tilted by rounding.
2. **free of hidden costs** (§1.4): no allocation, no lock, on any thread
   while the world is not stepping, many at once.
3. **as precise far out as near.** Bodies are 64-bit (ADR-0042); a query
   10 000 km from the origin must answer as one at it.
4. **deterministic**: the same world and query give the same calls in the
   same order.

## Decision

1. **Two levels.** Against one geometry (`collide.h`): `nph_physics_raycast`
   (any type), `nph_physics_shape_cast` (a convex shape moved without
   turning, against any type), `nph_physics_overlap`. Against a world
   (`query.h`): closest-hit and callback raycasts and shape casts, overlaps
   of a convex shape and of a box, and the contact manifolds of a convex
   shape placed in the world (what the controller pushes out of and
   slides along). The world's queries walk the broadphase trees and run
   the geometry's tests on what they find.
2. **Rays are exact and meet convex shapes from outside.** Spheres and
   capsules by closest approach and then Pythagoras (no cancellation for a
   far origin; Ericson §5.3), boxes by the slab test, hulls by clipping
   the segment with their planes (Haines 1991); meshes and heightfields by
   their watertight raycasts (ADR-0046). A ray that starts inside a convex
   shape does not hit it (Box2D's and Unity's rule, Godot's default); one
   that starts on its surface and goes in hits at 0.
3. **Shape casts are conservative advancement on GJK distances** (Mirtich
   1996, as Box2D v3.1's b2ShapeCast): each step moves the shape as far as
   the plane between the closest points allows, so it never passes the
   contact, until the two are within 0.1 mm; a last step along the
   contact's normal closes what is left (exactly across a face). GJK runs
   in a frame at the moving shape's place, where its coordinates are the
   shapes' own size however long the cast.
4. **The hit's normal is exact where it can be.** For a round shape (a
   sphere's or capsule's core is apart at contact) it is GJK's. Polytopes'
   cores touch at contact, where GJK's normal is noise; theirs is read off
   the closest features of GJK's last simplex (a face against a point, or
   two edges: the separating axis Box2D's separation functions take from
   the same simplex), or GJK's own from a millimetre back, whichever
   separates the two farther (the axis test's measure). Triangles take the
   contact rules' normal (one side, active edges, ADR-0046 §5), so a shape
   crossing a flat seam is not stopped by the next triangle's edge.
5. **Touching shapes stop a cast only if it goes into them.** A pair that
   touches or overlaps at the start (or on arrival) is hit, at fraction 0,
   only if the motion's component into the contact's normal is more than
   1e-5 of its length (a motion projected onto the contact is off it by
   rounding): a body resting on a floor casts along it freely, and stops at
   once when cast into it. Its normal then is the narrow phase's
   (`nph_physics_collide`), defined at any depth.
6. **Filters are Box2D's query filters, plus kinds and a body.** A shape is
   seen when its category meets the query's mask and its mask the query's
   category (so a shape can keep queries off it), on a kind of body the
   query wants, and not on the body it ignores (a character's own). The
   kinds pick the trees to walk.
7. **64-bit origins.** Each shape is tested in a frame centred on the
   query's origin: convex shapes by their pose relative to it, meshes and
   heightfields with the origin placed in their frame in 64 bits from the
   bodies' 64-bit origins (as their contacts are). The trees' 32-bit boxes
   are walked with a ray or box grown by the 32-bit stand-in's rounding
   (`nph_aabb_tree_box_cast`: a box swept along a ray, the ray against each
   node grown by the box), so the walk stays conservative.
8. **One hit per shape, in the trees' order.** A shape's answer is its
   own: every test sees the whole motion and a limit (the nearest so far),
   never a motion shortened to it, so the trees' order cannot change it.
   Callbacks return the fraction to clip to (Box2D's contract); the closest
   queries keep the first of equals and end at a hit at fraction 0.

## Alternatives considered

- **GJK ray casting** (van den Bergen 2004, Jolt's CastShape): one support
  point per iteration rather than a GJK solve per step, but Box2D's source
  noted it failing on boxes and converging slowly with a radius before its
  shape cast moved to conservative advancement. Conservative advancement
  on our signed-volumes GJK was robust from the first test.
- **Box2D's target separation** (stopping a slop short of contact, or a
  slop into it for round shapes): fine for continuous collision, wrong for a
  query that a character stands on, and inconsistent between shape types.
- **The narrow phase's normal for every hit**: exact, but its axis test
  leans to faces by a bias (for steady contacts), which tilts normals at
  edges by degrees, and for 32-vertex hulls it is two thirds of the cost
  (15.9 µs a cast against 5.3).
- **Solid convex shapes for rays** (PhysX's, Jolt's default: a hit at 0
  from inside): safer for a bullet fired from inside a wall, but every
  picking ray from inside a room's shape returns the room. An overlap query
  answers "inside what"; the choice can become a flag if games ask.
- **Analytic sweeps** (PhysX: sphere and capsule against each primitive):
  several times faster for those pairs; measured against GJK's cost first
  (below), and added per pair if the controller's budget needs them.

## Consequences

- The controller (and continuous collision) build on one cast that stops
  exactly at contact, never past it, and on contacts as the world makes
  them.
- Costs (Profile, the reference machine): a ray against one shape 27 to
  107 ns, against 10 000 static shapes 1.6 µs; a convex shape cast 0.88 µs
  (sphere and box) to 5.3 µs (two 32-vertex hulls), against the rippled
  mesh or heightfield about 6 µs, among 10 000 shapes 1.05 µs; a
  character's capsule cast a step along a rocky heightfield 2.3 µs, its
  contacts there 1.7 µs. A cast's cost is GJK's (about 3.6 calls a cast, 205 ns each
  cold for a sphere and a box): faster GJK is the next optimisation, for
  queries and contacts both.
- Thread safety follows from reading: queries may run on every worker at
  once between steps (tested, results equal to the bit).
