# 0049. Physics: the character controller

- **Date:** 2026-10-24
- **Status:** Accepted (2026-10-24)

## Context

CLAUDE.md §8.9 asks for a character controller: kinematic, with step and
slope handling. A game moves its characters by their own rules (input,
animation, jumps), not by forces, and expects them to slide along walls,
stop in corners, walk up slopes they can and not up those they cannot,
climb steps, keep to the ground going down stairs and slopes, ride moving
platforms, push loose objects, and do all of it smoothly at 240 Hz and at
any distance from the origin. Engines answer this in two ways: a body
driven by velocities (PhysX's and Jolt's CharacterVirtual's predecessors,
Bullet's), whose contacts the solver resolves; or a shape moved by queries
alone (PhysX's CCT, Jolt's CharacterVirtual, Box2D v3.1's mover, Unreal's
character movement), which the world does not know.

## Decision

1. **A virtual character moved by queries alone** (`character.h`). The
   game owns its state (64-bit position, rotation, the ground found) and
   calls `nph_physics_character_move` with a displacement a frame. A move
   reads the world and nothing else, so it allocates nothing, takes no
   lock, and any number run at once on any threads between steps.
   Dynamic bodies feel a character only through
   `nph_physics_character_push`, which the game calls with a move's hits:
   at each hit, along the surface's normal, the impulse that brings the two
   to one speed there (their reduced mass), the character not slowed.
2. **The slide is Quake's** (PM_SlideMove) **in Box2D v3.1's mover's
   frame**: cast along what is left of the motion, stop at the first
   surface, keep the motion's part along the surfaces met (one, along it;
   two, along their crease; three, none), up to four times; then push out
   of anything nearer than the skin, the contacts' half-spaces solved
   together as b2SolvePlanes does (sweeps over each plane's push, clamped
   at zero, so a push a later plane makes too large is taken back). A plane
   within 10 degrees of one met before is that one again.
3. **Every cast keeps a skin** (5 mm by default): the shape is cast rounded
   by it (a margin on the world's shape casts, ADR-0047), so each stops
   exactly a skin off what it meets along the true closest direction,
   whatever the shapes. Resting, sliding, snapping, and stepping all keep
   the skin by the same rule, and a walk too slow to close the skin in a
   frame still meets the step it presses against.
4. **Walking is Unreal's.** On walkable ground and not moving up, only the
   displacement's part across the up axis is the walk; it goes along the
   ground's plane from the start, and ground rising ahead raises it over
   every ground plane met, its pace across kept. A slope steeper than
   `max_slope` is a wall, its normal laid level. Afterwards the character
   snaps down onto ground within `snap_distance`.
5. **Steps up, forward, and down** (Unreal's StepUp, Jolt's WalkStairs):
   from where a wall stopped the walk, up by the step height as far as a
   ceiling lets it, forward by what is left of the walk, and down onto
   ground to stand on, kept only when that ground is a step: no lower than
   where the wall was met (beside a wall the way down finds the same
   floor), no higher than the step height above the feet, and no higher
   than the rise (else a low ceiling would leave the character perched on
   the step's edge).
6. **Standing on the surface's own normal** too (Jolt's mSurfaceNormal,
   Unreal's ImpactNormal): ground is walkable when the contact's normal is
   no steeper than the slope limit, or the surface's normal at the contact
   is (`nph_physics_world_surface_normal`, a new query: the face a point is
   on, and where faces meet the one nearest the way asked). A capsule's
   round bottom meets a 0.25 m step's edge 79 degrees from up although the
   step's top is level: the character perches on the edge while it climbs
   it or walks off it.
7. **The ground** (walkable, too steep, none; its normal, point, body, and
   that body's velocity at the point) is the snap's when it snapped, else a
   short cast's. A moving platform's velocity is the game's to add to the
   next displacement, so the game decides how much of it a character keeps
   (Box2D's mover leaves the same to the game).

## Alternatives considered

- **A body driven by velocities**: the solver resolves its contacts with
  everything else's, so it pushes and is pushed for free, but it bounces
  off steps, slides down slopes it stands on unless friction is tuned per
  surface, and its response to the game's input is the solver's, a step
  late. The virtual character's push helper gives the pushing.
- **Box2D v3.1's pogo stick** (a ray or shape cast below a capsule held up
  by a spring): steps up to the pogo's length climb without a step-up, but
  the capsule floats above the ground by that length, its feet are a
  spring's, and a game that places a character's feet on the ground has to
  undo it.
- **Casting the bare shape and stepping a skin back along each hit's
  normal** (built first): a skin kept straight up from a slope is less
  than a skin off it, and the push out along the normal moved a character
  standing on a 30 degree slope 0.3 mm down it every frame; on an edge the
  back-off's height was a linear guess, a centimetre off; and a walk slower
  than a skin a frame (1.2 m/s at 240 Hz) never reached a step's riser.
- **Judging walkability by the contact's normal alone** (built first): a
  character walking down stairs was off the ground at every step's edge,
  and none climbed a 0.25 m step.
- **Clipping the walk against each ground plane in turn** (Quake's rule
  for every plane): over a bump in a mesh, four triangles around its peak
  each stopped a cast at fraction 0, and the four slides were spent where
  the character stood.
- **A push out at the start of a move only** (built first, with one at the
  end for corners): the end's is the one that leaves the skin, and it
  covers what moved into the character since its last move too.

## Consequences

- A move costs about two shape casts and a contact query: 2.1 µs on a flat
  floor and 13.1 µs on a rippled heightfield among a thousand rocks
  (Profile, the reference machine). The casts' cost, GJK's, is the lever.
- Characters do not collide with each other: a game that wants them to
  gives each a body (kinematic, moved to the character) for the others to
  meet, or steers them apart.
- Building it found two flaws below it, now fixed: a round shape over a
  gentle convex ridge in a mesh had no contact from either triangle, and
  beyond such a ridge a cast leaving it met the faces' planes (ADR-0046,
  "As built: gentle ridges").
