# Physics (`engine/physics`)

The physics module (CLAUDE.md §8.9) simulates rigid bodies: collision
geometry, the broadphase and narrow phase, the solver, joints, continuous
collision, queries, and the character controller, stepped at a fixed
240 Hz on the job system. It depends on `jobs` and through it on
`platform`, `math`, and `core`; it does not know the ECS (the runtime
bridges entities to bodies) or the renderer (debug drawing goes out through
a callback). Decisions: [ADR-0042](adr/0042-physics-collision.md) (the
collision layer), [ADR-0043](adr/0043-physics-world.md) (the world and the
solver), and [ADR-0044](adr/0044-physics-joints.md) (joints).

Phase 11 builds it in parts. This document describes what exists:

| Part | Status |
|---|---|
| Geometry, convex hulls, the narrow phase, the AABB tree | done |
| The world: bodies, shapes, contacts, the soft-step solver, threads, islands, sleep, events | done |
| Joints: ball (cone, twist, friction), hinge (limits, motor, spring), fixed | done |
| The solver comparison (§13), slider, distance, and 6-DOF joints, the ragdoll, meshes, heightfields, compounds, queries, continuous collision, the character controller, the runtime bridge | to come |

| Header | Contents |
|---|---|
| `types.h` | units, tolerances, rigid poses, mass properties |
| `geometry.h` | the shape types, bounds, mass, support points, GJK proxies |
| `hull.h` | the convex hull block and the Quickhull builder |
| `collide.h` | contact manifolds, GJK distance, EPA penetration, segment distance, manifold reduction |
| `aabb_tree.h` | the dynamic AABB tree |
| `world.h` | the world: creation, the step, events, statistics, the state hash, debug drawing |
| `body.h` | rigid bodies: types, poses, velocities, forces, mass, locks, sleep |
| `shape.h` | shapes on bodies: geometry, density, friction, restitution, filters, events |
| `joint.h` | joints between bodies: ball, hinge, fixed; frames, limits, motors, springs, reactions |

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

## The world

`NphPhysicsWorld` (`world.h`) holds bodies and shapes, addressed by 32-bit
generational handles (20 bits of slot, 12 of generation), so a handle to a
destroyed object is detected rather than reused. The world owns every
array it works with and grows them from its allocator only between the
step's parallel stages, keeping their capacity; a scene in steady state
steps without allocating.

A step (`nph_physics_world_step`, 1/240 s in the engine) runs five passes:

1. **Pairs.** Shapes whose broadphase box grew last step (the move buffer)
   query the three trees, static, kinematic, and dynamic, in parallel:
   once to count each query's new pairs and once to fill them, so every
   query writes its own slice of the pair buffer. The new pairs become
   contacts in the move buffer's order.
2. **Collide.** Every contact with an awake body runs the narrow phase, in
   parallel, in a frame centred on its first body's centre of mass (so the
   manifold's points are that body's anchors). Points that keep their
   feature ids keep last step's impulses, for warm starting; the tangent
   impulse is kept as a world vector, which survives a turn of the tangent
   basis. Then, serially and in the awake list's order, the contacts that
   began or stopped touching, or whose fattened boxes parted, take effect:
   events, waking, island links, graph membership.
3. **Solve** (below): velocities, positions, and the contact impulses.
4. **Finalize.** Positions in 64 bits, shape boxes, sleep timers; boxes
   that left their fattened proxies grow in the trees, in the awake list's
   order, and join the move buffer; move events.
5. **Islands.** Pending merges, then, when sleep is on, islands that have
   been still for `time_to_sleep` fall asleep.

**Determinism.** Each parallel piece writes only its own records, and each
serial pass runs in a fixed order (the awake lists, the move buffer), so
the job system's worker count, the solver's thread count, and the SIMD
level change nothing: the tests compare state hashes
(`nph_physics_world_hash`) across all three.

## Bodies and shapes

A body (`body.h`) is static, kinematic, or dynamic. Static bodies never
move unless teleported; kinematic bodies move by their velocity alone,
push dynamic bodies, and are never pushed back; dynamic bodies move under
gravity, forces, and contacts. A body's mass, centre of mass, and inertia
come from its shapes' densities (summed with the parallel axis theorem)
unless set explicitly. Locks remove world axes of translation or rotation
from a dynamic body: a 2.5D game locks Z and the rotations about X and Y,
a character every rotation. Damping divides velocities by `1 + h c` each
substep, the Padé fit of `exp(-c h)` that never reverses a velocity.
Velocities are clamped to `max_linear_speed`, and angular velocities to an
eighth of a turn per substep (Box2D's limit, 7200 rpm at 960 Hz), beyond
which the rotation integrator degrades.

A shape (`shape.h`) is a geometry placed in its body, with a density, a
friction coefficient, a restitution, a filter, and event switches. Two
shapes meet when their filters allow: shapes of the same positive group
always do, of the same negative group never do, and otherwise each's
category must be in the other's mask (Box2D's rule). Shapes of one body
never meet, and neither do two bodies that are not dynamic. A pair's friction is the geometric mean of the two
coefficients and its restitution the larger, so a frictionless surface
makes every contact on it frictionless and a bouncy ball bounces on any
floor.

## Contacts

A contact exists while two shapes' fattened boxes overlap and their
filters allow it, and is touching while its manifold has points. Contacts
live in a pool with a free list, are found from a shape pair through a hash
map, and sit in both bodies' contact lists (intrusive and doubly linked:
two `prev`/`next` pairs per contact, keyed by contact and side).

Membership follows the bodies:

| List | Holds |
|---|---|
| the awake contacts | contacts with an awake body: the narrow phase runs on them |
| the constraint graph | touching awake contacts whose bodies are awake, static, or kinematic: the solver runs on them |
| an island | touching contacts between two dynamic bodies |

`contact_sync` restores the first two after any change to a contact or
its bodies; island links are made when a contact begins touching and cut
when it stops.

The **constraint graph** colours contacts so that no two contacts of one
colour share a dynamic body, which lets a colour's contacts be solved in
parallel without locks (Catto, "Solver2D", 2024; Box2D v3's graph). Each
colour keeps a bitset of its dynamic bodies; a contact takes the first
colour free for both of its dynamic bodies. Contacts with a static or
kinematic body only need their one dynamic body free, so they start at
colour 1 and leave colour 0 to dynamic pairs, the harder to place. There
are 24 colours, the first 20 open to dynamic pairs; a contact that fits
none goes to the overflow, solved serially before the colours. A dense pile
of mixed shapes uses about 15.

## The solver

The solver is the **soft step** (Catto, "Solver2D", 2024, after Macklin et
al., "Small Steps in Physics Simulation", SCA 2019): each step is split into
substeps (four by default, so 960 Hz at the engine's 240 Hz), and each
substep

1. integrates velocities (gravity, forces, damping, the gyroscopic term);
2. **warm starts**: applies each contact point's accumulated impulses;
3. **solves** each contact as a soft constraint, a spring of the world's
   stiffness and damping (120 Hz and a ratio of 10 by default, capped at a
   quarter of the substep rate) that pushes overlaps out no faster than
   3 m/s;
4. integrates positions;
5. **relaxes**: solves the same rows rigidly, without the springs' bias,
   removing the velocity the springs added, so bodies come to rest instead
   of bouncing off the push.

After the substeps, **restitution** gives contacts that approached faster
than the threshold (1 m/s) and carried load the pair's bounce, and the
impulses are stored for the next step's warm start. Contacts against static
and kinematic bodies are twice as stiff, since they take the whole
correction.

**Stiffness.** A soft contact holds a load at a depth: in steady state
the solve pass's spring must supply what the warm start carries, which
works out to a penetration of g / ω² for each body weight the contact
carries per unit of its effective mass (ω = 2π times the stiffness),
independent of the substep and the damping. Box2D runs its contacts at
30 Hz with 240 Hz of substeps, an eighth of the rate; at our 960 Hz the
same ratio is 120 Hz, the same arithmetic per substep with sixteen times
less give. A box carrying a box 100 times heavier sinks 0.44 mm into the
ground and the heavy box 1.7 mm more into it (at 30 Hz: 7 and 27 mm), and
the top of a 20-row pyramid stands 1.9 mm low (26 mm).

Within a substep, the current separation of a point comes from the bodies'
displacements and rotations since the step began, applied to the anchors
fixed at the step's start: no collision runs inside the step, and the
anchors are relative to the centres of mass, so this is exact to 32 bits
anywhere in the world. Positions accumulate as 32-bit displacements and
reach the 64-bit body only in the finalize pass.

**Speculative points** (separation above zero, within the speculative
distance) only stop the approach: the solver removes exactly the velocity
that would close the gap within the substep (Catto, "Speculative
Contacts", 2013), so fast bodies stop at the surface instead of tunnelling,
without continuous collision for most cases.

**Friction** follows the normal rows, as a coupled two-dimensional impulse
on the contact's two tangents, held in the friction circle (|impulse| ≤ μ
times the normal impulse): one coupled row instead of two independent
clamps, which would make a box sliding diagonally feel the friction of a
square. Inside the circle the impulse stops the slip (sticking). Past it,
kinetic friction must oppose the slip itself (Coulomb's law, the maximal
dissipation); scaling the stopping impulse down to the circle would keep
its direction, which rotation's coupling in the effective mass turns away
from the slip at each corner: a box sliding down a slope lost 6 % of its
friction that way, and spun. The impulse on the circle's edge takes a
direction between the two, weighted by r = limit / |stopping impulse|:
hard sliding (r near 0) opposes the slip exactly, and barely past the
circle (r near 1) it keeps the stopping impulse's direction, continuous
with sticking, where the slip's own direction is noise (taken alone, it
made a 20-row pyramid creep and stay awake). A box now slides at
g (sin θ − μ cos θ) to the third decimal, for 3 to 5 % more solver time.

**The gyroscopic term** is integrated implicitly in body space (Catto,
"Numerical Methods", GDC 2015): one Newton iteration on
I ω₂ + h ω₂ × I ω₂ = I ω₁ per substep, which conserves the magnitude of
angular momentum and never gains energy, where the explicit torque ω × I ω
would. A box spun about its intermediate axis flips over and over, as it
should (the Dzhanibekov effect), keeping 98.5 % of its energy over six
seconds. The iteration needs the body's current rotation: the same
iteration in the world frame with the step's starting inertia was cheaper
and kept only 84 %. Bodies whose inertia is a multiple of the identity
(spheres, cubes) skip it: their term is exactly zero.

**Data layout.** A solver body is one cache line: velocity, angular
velocity, displacement, and rotation this step, each a 16-byte row. The
contacts are grouped eight to a `PhysContactLanes` record by colour (one
per record in the overflow), with their points in structure-of-arrays
rows: anchors, effective masses, impulses, and the angular terms I⁻¹ (r ×
axis) for both bodies and all three axes, so applying an impulse needs no
inertia. The kernels gather eight bodies' rows by 4×4 transposes, compute
eight contacts per instruction, and scatter the velocities of the lanes
that own a dynamic body back. The preparation of the rows (the angular
terms, the effective masses, the approach speed for restitution, the
warm-start impulses on this step's tangents) is a kernel too.

The kernels are compiled twice, at the SSE4.2 baseline (each eight-lane
value a pair of SSE registers) and with AVX2 (one AVX register), from one
source, `solver_impl.h`. Both do the same arithmetic in the same order, and
contraction to FMA is off (ADR-0008 §6), so they agree to the bit; the
machine's best is chosen at world creation.

## Joints

A joint (`joint.h`) holds a frame on each of two bodies, given in the
body's frame: an anchor and three axes, of which X is the hinge or twist
axis. The joint is satisfied when the frames coincide, less the motion it
allows. `nph_physics_joint_frames_at` makes frames that meet at a world
point with X along a world axis for the bodies' current poses, so a joint
made from them starts satisfied.

| Type | Holds | Frees | Options |
|---|---|---|---|
| ball | the anchors together | rotation | a cone for B's X axis (elliptical: half angles about A's Y and Z), a twist range about it, friction against any rotation |
| hinge | the anchors together, the X axes aligned | rotation about X | an angle range, a motor (a speed up to a torque), a spring (towards an angle) |
| fixed | the frames together | nothing | |

At least one body must be dynamic; either may be static, a fixed point in
the world. Joined bodies' shapes do not touch unless the joint says so
(`collide_connected`): the pair search skips pairs of bodies joined that
way, creating such a joint destroys the bodies' contacts, and destroying it
queues the shapes so the next step finds their pairs again. Joints link
islands as touching contacts do, so joined bodies sleep and wake together;
a moving kinematic body wakes what hangs from it, and a hinge's motor
driving towards a speed keeps its bodies awake. Destroying a body destroys
its joints. `nph_physics_joint_reaction` gives the force and torque the
joint applied to B in the last substep, what a breakable joint compares
with its strength.

**Solving.** Joints are constraints in the graph like contacts: a joint
takes a colour free for its dynamic bodies, the colour's stages solve its
joints and its contacts together, and joints that fit no colour are solved
serially before the overflow's contacts. Each step prepares them with the
contacts (anchors from the centres of mass and the frames' rotations in
the world, fixed for the step; the inverse masses and inertias), and each
substep warm starts them, solves them softly, and relaxes them rigidly, as
the soft step does contacts. Inside the step, the bodies' displacements and
rotations since its start move the fixed anchors and frames, so a joint's
error needs no 64-bit arithmetic.

The rows, in the order a joint solves them (the most important last, so it
is left exact):

| Row | Error | Rows |
|---|---|---|
| hinge spring | the angle less its target | 1, about X, soft at the spring's frequency, in the solve pass only |
| hinge motor | the relative speed less the target | 1, its impulse within the torque times the substep |
| hinge limits, ball twist limits | the angle past either end | 1 each, one-sided |
| ball cone | the angle between the X axes past an ellipse over the swing vector (angle times axis), along the swing's axis | 1, one-sided |
| ball friction | the relative angular velocity | 3, the impulse within a ball of the torque times the substep |
| hinge axes | the cross product of the X axes | 2, about A's Y and Z |
| fixed rotation | the rotation vector from A's frame to B's (twice the error quaternion's vector part) | 3 |
| anchors | B's anchor less A's | 3 |

Limits are speculative like contacts: open, they only stop the approach
within the substep; closed, they are soft while solving and rigid while
relaxing. The soft rows use the world's joint stiffness, 240 Hz with a
damping ratio of 2 by default (a quarter of the substep rate, twice the
contacts', Box2D's ratio). A spring solved again in the relax pass would
double its implicit damping, so the spring acts once a substep, like the
force it is.

**Accuracy** (the tests, Profile):

- A pendulum on a ball joint swings at its closed-form period
  (2π √((⅖r² + L²) / gL) with the large-amplitude term) to 0.000 %, its
  anchors within 5 µm, with no loss of amplitude in four periods.
- A soft joint gives g / ω² (4.3 µm at 240 Hz) per body weight it carries
  over its effective mass: a hanging chain of 20 links is 1.7 mm long in
  all, and 10 links carrying a body 100 times heavier than a link stretch
  9.7 mm. A chain released sideways whips down at 28 m/s with its anchors
  at most 1.2 mm apart.
- Cones and twist ranges hold within 0.07° of their limits; a door slammed
  into its hinge's limits at 6 rad/s stops within 0.04°, its axis within
  0.0003°, and its anchors open by 0.8 mm for a few substeps before closing.
- A motor reaches its speed exactly, or accelerates at its torque over the
  inertia; ball friction slows a spin at its torque to 0.01 %; an undamped
  spring rings at its frequency, losing about 4 % a period to its implicit
  integration (a damping ratio near hω / 2); a welded 2 m, 80 kg cantilever
  sags 0.04 mm at its tip.
- The reaction holds the body's weight to 0.5 %.

**Cost.** A joint is scalar code, about 1.1 µs a step on one thread
(preparation and three passes in each of four substeps): sixteen chains of
25 links (400 joints) step in 0.57 ms on one thread and 0.16 ms on the job
system.

## Choosing a solver

The world's `solver` setting selects one of three solvers, all on the same
collision, graph, threads, and kernel layout (the comparison of CLAUDE.md
§13: [ADR-0045](adr/0045-physics-solver-comparison.md) has the scenes and
the measurements):

| Solver | Per substep | Choose it for | Avoid it for |
|---|---|---|---|
| soft step (default) | soft solve, then a rigid relax | everything: stacks, piles, ragdolls, exact friction and energy | very heavy loads on joints at 4 substeps (raise the substeps) |
| TGS (PhysX 4) | rigid rows with a Baumgarte bias on the step's error; one velocity pass after the substeps | stacks and piles where cost matters most (9 to 16 % cheaper) | heavy loads on joints: a 100:1 chain stretches 161 mm and can come apart |
| XPBD (Müller et al. 2020) | predicted positions corrected by rigid constraints, velocities derived, then a velocity pass | stiff machinery and rigs (near rigid at 16 substeps) | stacks (a 20-row pyramid collapses), sliding friction (5 % weak), long oscillations (10 % of a pendulum's swing lost in 10 periods) |

The soft step is the only one that stacks, keeps energy, gets friction
exactly, and settles ragdoll heaps; its joints' give falls by four for each
doubling of the substeps, since the default stiffnesses are fractions of
the substep rate (contacts an eighth, joints a quarter): the substep count
is the quality setting.

## Threads

The solver's work is one list of stages, unrolled for the step: prepare
bodies, prepare contacts, then per substep integrate velocities, warm
start, solve, integrate positions, relax (each contact stage once per
colour, in order), then restitution, store, and finalize; about 60 stages
for a pyramid in four colours, 200 for a pile in fifteen. A stage is split
into blocks, at least 32 bodies or four contact records (32 contacts) and
at most four per thread.

The threads go through the stages in lockstep. Each starts at its own
share of a stage's blocks and claims blocks one at a time with an atomic
exchange, forward while they are free, then backward. A thread therefore
keeps the same blocks substep after substep, and their records stay in its
cache. The calling thread then claims whatever is left, so a helper the job
system has not started yet, or one the operating system preempted, holds
back only the block it has; whichever thread finishes a stage's last block
publishes the next. Helpers spin between stages rather than park: a stage
lasts a few microseconds and a wake costs more (Box2D v3's solver, on our
job system). A thread that has spun through about ten stages' worth of
pauses without a new stage also yields: some participant is likely
preempted and holding a block, and needs a core.

**How many threads.** Every thread that joins a stage costs a few hundred
nanoseconds of synchronization whether or not it finds work, so the solver
takes as many threads as the step's work pays for. On one thread a step
costs about 600 ns per point row, 290 ns per body, and 1200 ns per joint; a
thread pays for itself with about 450 ns of work in each stage it joins:

    threads = min(1 + work / (stages × 450 ns), workers + 1, logical cores)

The ratio of work to synchronization is what matters, and machines scale
both alike. The thread count changes no result.

Measured on the reference machine (Ryzen 7 5800X, 8 cores and 16 threads;
Profile), the solver's time per step by thread count:

| Scene | 1 | 3 | 6 | 8 | 12 | 16 | chosen |
|---|---|---|---|---|---|---|---|
| pyramid, 210 boxes, 4 colours | 0.183 | 0.107 | 0.099 | 0.102 | 0.108 | 0.193 | 7: 0.099 |
| pile, 1000 mixed shapes, 15 colours | 1.22 | 0.654 | 0.466 | 0.429 | 0.386 | 0.534 | 10: 0.395 |
| 16 pyramids, 3360 boxes | 3.02 | 1.45 | 0.875 | 0.723 | 0.579 | 0.565 | 16: 0.533 |

(milliseconds, the mean of 400 steps.) With every logical core spinning,
one preempted thread stalls everyone, so the mean at 16 threads is erratic
on a desktop with other programs running; its best steps match 12 threads.

## Islands and sleep

Islands are persistent (Catto, "Simulation Islands", 2023). A contact that
begins touching between two dynamic bodies links their islands, recorded
as a union (the larger absorbs the smaller, so a merge moves the fewest
records) and carried out at the end of the step; a contact that stops
touching only counts a possible split. An island whose bodies have all
been below their sleep thresholds for `time_to_sleep` (half a second by
default) is a candidate: if contacts left it, it is first split into its
connected components (one island per step, depth first in list order),
which are reconsidered next step; otherwise it falls asleep. Finding
components is spent only where it buys sleep, and an island that has grown
too big while awake costs nothing until then.

A sleeping island's bodies leave the awake list, its contacts leave the
awake list and the graph, and the island costs nothing per step: ten
thousand sleeping boxes step in 2.8 µs. A body that moves by a call
(velocity, force, impulse, teleport), a contact that begins touching it,
or a moving kinematic body touching it wakes its island.

## Events and debug drawing

A step reports what it did as arrays valid until the next step
(`nph_physics_world_events`): contacts that began and ended touching (for
shapes with `contact_events`), hits (a contact that began at a speed above
the hit threshold, with the point and the speed, for sounds and effects;
for shapes with `hit_events`), and moves (every awake body's new pose, how
the runtime mirrors physics into the scene). Nothing calls back into user
code during a step, so nothing can change the world while it is in
flight.

`nph_physics_world_draw` draws shapes, broadphase boxes, contact points and
normals, and centres of mass through a line callback, in 32-bit
coordinates relative to the renderer's debug origin.

## Performance

Profile build, Ryzen 7 5800X, `benchmarks/physics` (each collision row
cycles through 64 prepared configurations; world rows are per step at
240 Hz, after the scene has settled, with sleep off except where it is the
point):

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
| `world_pyramid_20_st` (210 boxes, one thread) | 387 000 |
| `world_pyramid_20_mt` (the job system) | 147 000 |
| `world_pyramids_16_mt` (3360 boxes) | 905 000 |
| `world_pile_1000_st` (1000 mixed shapes in a bin) | 3 390 000 |
| `world_pile_1000_mt` | 682 000 |
| `world_sleeping_10k` (10k asleep, one ball awake) | 2 800 |
| `world_chains_st` (16 chains of 25 links, 400 ball joints, swinging) | 574 000 |
| `world_chains_mt` | 160 000 |

Hull against hull is dominated by the edge-pair pass, about 8000 Gauss-map
tests for two 32-vertex hulls; the planned optimisation buckets B's arcs on
the Gauss map so each arc of A meets only its neighbours, measured with the
world's scenes. Until then, hulls of 16 to 32 vertices suit props.

In the world, the narrow phase is now the larger cost on one thread: of the
pile's 3.4 ms, 2.1 ms is collision (every awake contact, touching or not,
runs its pair function every step) and 1.2 ms the solver. Resting contacts
whose bodies have barely moved relative to each other can keep their
manifold, refreshing only the separations (the persistent manifolds of
PhysX's PCM); that is the next optimisation. On the job system the 1000
pile steps in 0.68 ms and 3360 resting boxes in 0.91 ms, against the 1.0 ms
budget of §8.9 on the six cores of the reference CPU class.

## Testing

`physics_tests` (87 tests):

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
- The world: creation and bad descriptions; free fall against the
  integrator's closed form; a dropped sphere settling and sleeping; a box
  resting without drift; a stack of ten and a pyramid of 20 rows standing
  (10 in Debug) within millimetres, and asleep; a box carrying one 100 times
  heavier;
  friction holding below its angle and sliding above; restitution
  bouncing to its height; momentum conserved through collisions; sleepers
  waking when touched or pushed; begin, end, hit, and move events; filters
  and groups; destroying a support; kinematic bodies pushing and carrying;
  the gyroscopic term flipping the intermediate axis without gaining
  energy; the world's invariants (`nph_physics_world_validate`) through a
  pile with teleports and new bodies, and with sleep off; a stack
  10 000 km from the origin behaving as at the origin; the SSE4 and AVX2 kernels
  agreeing to the bit; and the same state hash with no job system, 1, 4,
  and 8 workers, and 2, 5, and 9 forced solver threads.
- Joints: descriptions and their errors; frames that start satisfied; a
  pendulum's period against its closed form; a chain hanging, whipping
  down from the side, and carrying a body 100 times heavier than a link;
  ball cones (elliptical) and twists pushed into their limits about each
  axis; ball friction against a constant torque; a door slammed into both
  of its hinge's limits; a motor at its speed and at its torque limit; a
  spring's frequency and its implicit damping; a welded cantilever's sag;
  joined bodies touching only when asked, and touching again once
  unjoined; destruction (of joints and of bodies) waking and dropping what
  they held; chains sleeping and waking as one; a kinematic crane carrying
  a sleeping load; the reaction against the weight; and the same state
  hash across worker and solver thread counts with chains, a motorized
  door, a weld, and boxes falling on them.
