# 0050. Physics in the engine: components, the bridge, and drawing between steps

- **Date:** 2026-10-24
- **Status:** Accepted (2026-10-24)

## Context

The physics module knows nothing of the ECS (ADR-0042): it has bodies,
shapes, and a step. Games are written against the ECS (ADR-0020, ADR-0024):
their state is components, their logic systems, and the editor, scene
files, play-in-editor copies, and hot reload all work on components.
CLAUDE.md §8.9 asks for a fixed 240 Hz step "with interpolation when it
differs from the frame rate", and §8.13 for physics debug toggles in the
editor. So the engine has to:

- give entities bodies from data the editor edits and scene files keep;
- let a game move bodies through those components and read their motion
  back from them, step after step, at a cost bounded by what changes and
  what moves (a settled level of thousands of bodies costs nothing; a pile
  of a thousand moving ones must not flood anything);
- make bodies when play begins and drop them when it ends, leaving the
  edited world alone, and keep them through a hot reload;
- handle hierarchies: a crate on a moving ship, a platform under a rig;
- draw the world between its last two states when frames and steps differ,
  without the motion stuttering or the camera lagging behind its target;
- leave the physics world to the game for what components do not say
  (queries, characters, joints, events).

## Decision

1. **Two runtime components** (`runtime/components.h`), registered in
   every world after the renderer's. `RigidBody`: the type (static,
   kinematic, dynamic), a mass (0: the Collider's), velocities, damping,
   gravity scale, locks, `bullet`, `can_sleep`. `Collider`: a sphere, a
   capsule along Y, or a box, at a centre and rotation in the entity's
   frame and scaled by its world scale, with a density, friction,
   restitution, and a filter (category, mask, group). A Collider alone is a
   static body, a RigidBody alone a point of 1 kg.

2. **A bridge with one system** (`runtime/src/physics_bridge.c`). The
   physics world exists while the world plays, created with the `phys.*`
   cvars. Its system, `nph.physics`, steps it once per simulation step
   after the game's systems (they move bodies) and before the transform
   system (it composes where they went). It runs alone on the schedule's
   thread (`NPH_SYSTEM_MAIN_THREAD`), so nothing runs beside the step and
   the game's systems in the other stages may use the physics world:
   queries, characters, joints, bodies of their own.

3. **Changes found by comparison, bounded by change ticks.** Each record
   keeps the components its body was made from or last wrote back. A step
   looks only at chunks whose RigidBody, Collider, or LocalTransform columns
   were written since its previous run (the ECS change filters; its own
   writes carry its own tick, so it never sees them) and applies only what
   differs from the record. The world's event stream, read by a cursor up
   to the frame's sync point, says what went (despawns, removed
   components) and what changed parent; a stream that overflowed makes the
   next step check every record.

4. **Moving bodies through components.** A new LocalTransform teleports a
   dynamic or static body (and resets its interpolation) and moves a
   kinematic one there over the step: velocities that arrive at the step's
   end, so it pushes what it meets, zeroed the step after unless it is
   moved again. RigidBody fields apply in place; a new type remakes the
   body; a new Collider remakes the shape (and the mass over it).

5. **Writing back through chunk views.** The step's move events name the
   bodies that moved, with their poses and velocities. Each one's chunk and
   row come from the world (`nph_world_entity_chunk`), written through a
   query view that stamps the chunk's columns once: no per-entity events
   (a pile of a thousand boxes would overflow the stream every step). A
   root's LocalTransform is its body's pose; a child's is written relative
   to its parent wherever the parent is, so a parent's motion does not
   carry a dynamic body, while static and kinematic bodies follow their
   parents.

6. **Bodies before the first step.** Play begins with a transform update
   and a body for every entity, then the game's `play_begin`, then another
   transform update and a refresh: what `play_begin` made or moved has its
   body when the first step's systems run, placed where it was put
   (kinematic bodies too: before the first step nothing has moved).

7. **Drawing between steps in the scene renderer.** After each batch of
   steps the engine captures what the simulation moved: where each instance,
   light, and the view's camera was at the previous capture and where it is
   now, decomposed into a 64-bit translation, a rotation, and the stretch
   left (scale and shear), so that a turn blends as a turn (Shoemake and
   Duff, "Matrix Animation and Polar Decomposition", 1992, with the rotation
   from Gram-Schmidt rather than the polar factor: exact at both ends,
   smooth between). Every sync until the next capture draws them `fraction`
   of the way: the translation by linear interpolation in 64 bits, the
   rotation by normalised linear interpolation (Blow, "Understanding Slerp,
   Then Not Using It", 2004), the stretch linearly. The fraction is
   1 − (1 − α) / N, where α is the
   clock's leftover fraction of a step and N the batch's steps: the state
   one step behind the time the clock has reached, so what is drawn moves
   at the pace of real time (Fiedler, "Fix Your Timestep!", 2004). It is 1,
   everything as it is, while paused, for single steps, in lockstep, and
   with `sim.interpolate` off. A discontinuous move resets an entity's
   interpolation (`nph_engine_reset_interpolation`, thread-safe, also
   called by the bridge for teleports); a reset camera, or another camera,
   is a camera cut.

8. **Debug drawing** through the renderer's debug lines (`phys.debug_draw`:
   shapes, boxes, contacts, centres of mass, sleep, joints), and in the
   editor every Collider's outline, the selection's over the scene.

## Alternatives considered

- **Physics as an ordinary system beside the game's.** The step spreads
  over the job system itself; systems running beside it could not touch
  the physics world, and every query would need a lock. Running alone costs
  one stage boundary, which measured free (an empty frame: 3.09 µs with the
  system returning at once, 3.04 µs without it).
- **Body handles in components, the game calling the physics API.**
  Handles do not survive scene files, play copies, or migrations, and the
  editor could not show or edit bodies. The game reaches a body when it
  needs one (`nph_game_physics_body`).
- **SET events from `nph_world_get_mut` to find changes and to write
  back.** One event per body per step: a moving pile overflows any stream
  and turns every step into a full pass. Change filters and comparisons
  cost what changed; chunk views cost a stamp per chunk.
- **Extrapolating ahead of the last state** instead of drawing one step
  behind it: no added delay, but it predicts through contacts and
  rebounds. The delay is one step (4.2 ms at 240 Hz), inside §10.6's three
  frames.
- **Blending world matrices column by column:** a turning body shrinks
  mid-blend by the cosine of half its turn. Decomposing keeps it rigid.
- **Interpolating on the GPU:** every consumer of instance transforms
  (culling, the virtual shadow map's page invalidation, the ray-tracing
  instances, motion vectors) would blend. The scene renderer's CPU cost
  is measured below; GPU-side blending is the step to take when it binds.

## Consequences

- Costs (Profile, Ryzen 7 5800X;
  `benchmarks/results/runtime-2026-10-24-win-msvc-profile-ryzen7-5800x.json`):
  a settled scene of 1,000 bodies adds 0.4 µs to a frame; 1,000 moving
  bodies cost 120.6 µs a frame against 68.1 µs for their step alone, of
  which the write-back is 18 µs (53 µs before the move events carried
  velocities and roots skipped hierarchy lookups); an empty world's frame
  went from 3.04 to 4.05 µs (the empty step and the bridge's checks).
- The scene renderer's sync is serial on the main thread: 79 ns per moving
  instance, 262 ns drawn between steps. A world with 10,000 moving
  instances spends 0.79 ms (2.6 ms interpolated) of §10.5's 1 ms
  main-thread budget there. Parallel capture and blending, or the GPU-side
  blending above, come before games move that many.
- Colliders are spheres, capsules, and boxes. Convex hulls, triangle
  meshes, and heightfields need cooked collision assets; compounds through
  the hierarchy (children's Colliders on their parent's body), joint
  components, and sensors are to come. Until then a game builds them
  through the physics API.
- The character controller is not a component: a game keeps its
  characters in its state block and moves them in its own systems
  (`samples/game_physics`, whose headless smoke run walks a course
  through stairs, a drop, a jump, and a moving platform).
- The move events carry velocities (24 more bytes a moving body), which
  the bridge reads instead of two handle lookups.
