# 0108. Physics in levels: collision assets, compounds, triggers, joints, physics materials, a character, a first-person game

- **Date:** 2026-11-20
- **Status:** Proposed (Phase 15's fourth step, ADR-0102 §1.4)

## Context

ADR-0102 puts physics in levels fourth in Phase 15: "colliders of cooked meshes and hulls, compounds through the
hierarchy, and joints, triggers and physics materials as components; a character controller component; a first-person
starter game (walk, run, jump, open doors, push props) that plays a level in the editor". The phase ends with the test
district walked in first person: colliding, climbing stairs, pushing a prop, opening a door.

What there is today:

- **The physics module has the shapes, joints and character, not sensors.**
  - It has spheres, capsules, boxes, convex hulls, triangle meshes and heightfields, and several shapes on a body,
    each at a pose (ADR-0042, ADR-0046).
  - It has joints: ball, hinge, fixed, slider, distance and six-axis, with limits, motors and springs (ADR-0044).
  - It reports contact begins, ends and hits (ADR-0043), answers queries (ADR-0047), and moves a kinematic character
    controller with steps, slopes and pushing (ADR-0049).
  - Nothing reports what overlaps a volume without colliding with it, so a trigger cannot be made.
  - Friction and restitution are a shape's, combined one way only: the geometric mean and the larger.
- **The runtime mirrors two components while the world plays (ADR-0050):** `RigidBody`, and `Collider` (a sphere, a
  capsule or a box). Each entity is one body with one shape. A `Collider` under a parent's `RigidBody` is a separate
  static body that follows the parent, not a part of the parent's body.
- **Collision assets exist but are not read.** Step three cooks them (`COLL`: hulls and triangle meshes) and names each
  node's in the cooked scene (`COLS`; ADR-0107), but nothing in the runtime reads them. Physics geometry has no scale
  (ADR-0042), so an entity at another scale needs scaled copies.
- **A game writes its own character.** `samples/game_physics` keeps a capsule in its state, moves it by
  `nph_physics_character_move`, and looks at it through a third-person camera. There is no character component, no
  first-person camera, and no body by which rays and volumes could see the player.
- **The inspector edits every field generically.** Enumerations and bit masks are numbers, and an entity reference has
  no picker.

## Decision

1. **Sensors and ghosts in the physics module.**
   - A shape description's `sensor` makes a convex shape (sphere, capsule, box or hull) that touches nothing. It gets
     no contacts and no mass, and queries skip it unless their filter asks for sensors.
   - After each step's solve, each sensor finds the shapes it overlaps, by the world's own overlap query: the tree,
     each candidate's tight box, then the exact test.
   - A visitor is a shape that:
     - is not a sensor;
     - belongs to a dynamic or kinematic body other than the sensor's (a static shape never moves into a volume);
     - has a category that meets the sensor's mask;
     - has its `sensor_events` on, the default.
   - The visitor's own mask is not asked.
   - A shape description's `ghost` makes a shape that touches nothing and has no mass, but that queries and sensors
     see by its filter, as any shape's: a character's body (decision 8). A shape is not both.
   - Each sensor keeps its visitors sorted, and the step reports sensor begin and end events (the two shapes and their
     user data) in sensor order, then visitor order: deterministic.
   - A destroyed visitor drops out of the next step's sets, which report its end with the user data the last step's
     set kept. A destroyed sensor's visitors end at the next step.
   - A step that fails reports no sensor events and keeps the sets of the last that succeeded, so the next that
     succeeds reports every change since.
   - Box2D 3.1 finds sensor overlaps the same way, at the end of the step rather than as contacts (Catto, 2025).
   - The sensors run on the job system, each writing its own room of one buffer, so the sets come out the same
     whatever the threads do.
   - **Measured** (Ryzen 7 5800X, Profile): 64 sensors tiling the 1000-shape pile's bin, every shape inside one (1973
     visitors), cost 0.18 ms a step on the job system and 1.5 ms on one thread, 0.76 µs a visitor. A level's
     triggers hold a few shapes each.
2. **Combine modes for friction and restitution.**
   - Each shape has a mode for each: the default, average, minimum, multiply or maximum.
   - Of two shapes' modes the higher one wins, PhysX's rule (its order: average, minimum, multiply, maximum), so ice
     (minimum) is slippery against every shape of the default and average modes.
   - Two defaults combine as today: friction by the geometric mean, restitution by the larger. Pairs of default shapes
     take that arithmetic after one test, so the contacts cost what they did (measured within the noise).
3. **Compounds through the hierarchy.** The bridge's record of one entity, one body and one shape splits into bodies
   and shapes.
   - A body belongs to an entity with a `RigidBody`, or to a `Collider` entity with no `RigidBody` above it (a static
     body, as today).
   - A `Collider` on an entity under a `RigidBody`, with no closer one between them, is a shape of that body. It sits
     at the pose its entity has relative to the body's entity, scaled by its own world scale.
   - Moving such a child relative to its body remakes the child's shapes and the body's mass.
   - A `RigidBody` under another stays a body of its own, as today; joints link the two (decision 6).
   - **Built:**
     - A part's frame is composed from the LocalTransforms between it and its body, under the body's world scale.
       These are exact inputs, so a moving body's parts read the same every step and are never made again.
     - Each body's parts are a list through their entity slots, so a body made again for a new type takes its parts'
       shapes in the same step.
     - A part with entities between it and its body (whose changes the bridge does not watch) is checked every step
       by a hash of what its frame is composed from.
     - A Collider whose RigidBody comes or goes above it is made again as what it now is.
   - **Measured** (Ryzen 7 5800X, Profile):
     - 1000 spinning bodies of two parts cost a frame 0.25 ms, where the same children as static bodies following
       their parents cost 1.64 ms.
     - With an entity between each part and its body, 0.41 ms.
     - A full pass over 10k grouped Colliders costs 1 % more, for the question of a body above each.
4. **Colliders of collision assets.**
   - `Collider`'s `shape` gains `ASSET`: every shape of the collision asset `collision` (a GUID), at the collider's
     `center` and `rotation`.
   - Assets load through the engine's loader. At play begin they load synchronously, as the audio bridge's sounds do;
     during play asynchronously, and the body takes its shapes when the asset is ready.
   - Each block is checked once by the physics module's validators.
     - At a world scale of one, the block is used in place from the asset.
     - At another scale, the bridge makes a scaled copy, shared by every collider of the same asset, shape and scale:
       a hull is rebuilt from its scaled points, and a mesh from its scaled vertices, wound again where the scale
       mirrors.
   - A mesh shape on a dynamic body is refused with a warning: meshes collide only with what moves.
   - **Cooked scenes bring their collision.** `nph_scene_import_cooked` gives each node that `COLS` names a `Collider`
     of its asset and no `RigidBody`, so the shape is static. A designer makes a prop move by adding a `RigidBody`.
   - **Built:**
     - A copy bakes the entity's scale after the Collider's turn into its points. That is exact for hulls and meshes,
       mirroring included (the primitives keep their per-axis sizes). A copy is keyed by the asset, the shape, and
       the scale and turn bits, and counted, so a scale that keeps changing frees each copy with its last shape.
     - Scales are signed through the bridge's poses: Z is negative where a frame mirrors. Primitives' centres and
       compounds' parts follow a mirrored entity too.
     - A body waiting for its asset sleeps, and wakes with its shapes. Tested: 35 frames of a background load, the
       crate unmoved, then falling.
     - **Measured:** the steady frames are unchanged (1000 resting bodies ×1.007, 1000 spinning ×0.996). A full pass
       over 10k grouped Colliders, which a change of parent starts, costs 6 % more than before this decision and
       decision 3: 3.33 ms against 3.14.
5. **Triggers.**
   - `Collider`'s `trigger` makes its shapes sensors.
   - After each step the runtime reports the entities that entered and left each trigger (the trigger's entity and the
     visitor's), for games and for the editor's play.
   - **Built:** `nph_game_physics_triggers`, beside `nph_engine_physics_triggers`.
     - Each event names the trigger, the visitor's Collider entity, and the visitor's body's entity (its compound's
       for a part).
     - Shapes are counted per pair over the step, so a Collider enters with its first shape and leaves with its last.
       A trigger made again around what it holds reports nothing.
     - Only the bridge's own shapes count; the game's own sensors and bodies report through the physics world.
6. **Joints as components.** A `Joint` on an entity links its body (its own `RigidBody`'s, or that of its nearest body
   above it) to the body of `other`, an entity reference; none means the world.
   - **Kinds:** ball, hinge, fixed, slider or distance.
   - **Fields:**
     - an anchor and an axis in the entity's frame;
     - limits (lower and upper: radians for a hinge, metres for a slider or a distance);
     - a motor (a speed and the most torque or force);
     - a spring (a frequency, a damping and a target);
     - whether the linked bodies collide;
     - a breaking force. Past it the joint breaks and stays broken (`broken`, read only); 0 never breaks.
   - The joint is made once both bodies exist, at the frames its anchor and axis give in the world then, and made again
     when the component changes.
   - A door is a dynamic body hinged to the world: its limits are its swing, and its spring what closes it.
   - **Built:**
     - **Frames are kept.** A change of settings (limits, motor, spring), or a body made again for the same entity,
       makes the joint again from the frames it had. A hinge's angle and a rope's length therefore keep their zero.
       Only a change of structure (kind, `other`, an anchor, the axis, collision), or joining again after breaking,
       takes the frames anew from the world's poses. This refines "made again when the component changes": made
       again, but from its old frames unless its structure changed. Settings are not set in place, because the
       physics setters assert on values out of range, and an inspector's half-typed limit must not stop play.
     - A distance joint's far end is `other_anchor`, in `other`'s frame or the world's. A ball's limit is a cone of
       `upper` radians about its axis.
     - Breaking compares the joint's reaction force over the last step with `break_force`. A broken joint stays
       unmade until the game writes `broken` false. `nph_game_physics_joint` gives the physics joint, for a door's
       angle.
     - **The spring's frequency and damping are its row's own:** those of a body turning about its centre. A door,
       turning about its edge, swings at half of each: measured at 0.24 Hz with a 17 % overshoot, asked for 0.5 Hz
       critically damped. Box2D's revolute spring behaves the same way.
     - **Measured** (Profile): 1000 hinged doors cost a frame 0.36 ms, against 0.12 ms for 1000 free bodies spinning.
       Of the bridge's upkeep, checking every joint's breaking force costs 55 µs. Joints whose bodies have not changed
       since they were settled cost nothing (a body generation; looking their bodies up again cost 14 µs).
7. **Physics materials.**
   - A physics material is an asset: a `.npmat` source, cooked to `PMAT` version 1. It holds friction, restitution,
     their combine modes, and density.
   - A `Collider`'s `physics_material` (a GUID) takes these over the collider's own fields; without one, the collider's
     fields hold.
   - Physics materials are made and edited in the editor (Assets, New Physics Material), and loaded as collision assets
     are.
   - **Built:**
     - `PMAT` holds one 64-byte header: friction (0 to 100), restitution (0 to 1), density (0 to 10^6 kg/m^3), and
       the two modes, numbered as the physics module's. The source names the modes `default`, `average`, `minimum`,
       `multiply` and `maximum`.
     - The shapes wait for a material that loads, as for a collision asset. One that is missing or damaged leaves the
       Collider its own fields, logged, where a collision asset that cannot be read makes no shapes: a level with a
       material missing still plays.
     - **A material changes in place while the world plays:** every shape naming it takes the new friction,
       restitution and modes from the next step, and a new density weighs its body again
       (`nph_physics_shape_set_density`, new). Nothing is made again, so contacts keep their warm starts. Nothing
       wakes either, as with the physics module's surface setters: what rests stays at rest until something touches
       it. The values come from `nph_engine_physics_material_set`, or from the asset loaded again
       (`nph_engine_physics_reload`) for a host that cooks while the world plays.
     - **The editor's physics material panel** (Assets, New Physics Material; Edit from the browser or from a
       Collider's field, which takes a `.npmat` dropped on it) writes and cooks each settled change, with a history
       of its own as the material panel's. It sets what it shows into play directly, as it is dragged, rather than
       through its cooks: the editor holds cooks' results until play ends (scenes join the edited world outside
       play), so a cook does not reach play. The panel sets its source again when play begins.
     - **Measured** (Profile, against #215): the frames of moving and hinged bodies, and the full pass over 10,000
       Colliders, are within the noise (x0.995 to x1.011). The frame of 1,000 sleeping bodies costs 0.14 µs more
       (4.56 to 4.70 µs, x1.03): the Collider's 16 more bytes put the bodies in 21 chunks of 16 KiB where they took
       20. The bridge's own work adds one test a step.
8. **A character controller component.**
   - **Shape and limits:** a `CharacterController` is a capsule (a radius and a height), with the highest step it
     climbs, the steepest slope it walks, how far it snaps down to the ground, its filter, and the mass it pushes with.
   - **Each step**, before the physics step, the runtime moves it:
     - The game sets `move`, a velocity along the ground in world axes, and `jump`, an upward speed taken when the
       character stands.
     - The world's gravity times `gravity_scale` pulls it while it falls.
     - A moving body it stands on carries it.
     - What it walks into is pushed (`nph_physics_character_push`).
     - Its `velocity`, whether it stands, and what it stands on are written back for the game, read only.
   - **Its body:** it has a kinematic body whose capsule is a ghost (decision 1). It touches nothing, but triggers
     and rays see it by its filter. A trigger therefore knows the player came in, and an enemy's ray finds the
     player.
   - **Built:**
     - The capsule stands on the entity's origin, its feet, a skin (5 mm) over what it stands on, and never turns:
       the entity's rotation stays the game's, and its scale does not size it. `jump` is cleared at every step,
       taken or not. A game that writes the translation puts the character there at rest (a respawn).
     - **What carries it** is followed by where the point it stood on went over the last step, that body's motion
       turns included: a lift up or down at 2 m/s keeps it on its feet at every step, 8 mm behind the deck, and a
       quarter turn of a platform takes it a quarter round. Off the ground it keeps the velocity it left with (a
       platform's: a jump from one moving at 2 m/s lands where it took off on it), gravity times `gravity_scale`
       pulls it, and what it runs into takes its part of that velocity (a ceiling ends a jump). The carry's rounding
       is kept level: a rise of a micrometre stopped the physics module's move from walking, so the character
       climbed no step and kept to no ground.
     - **Its body moves over the step** with the velocity that takes it where the character went, as a kinematic
       platform's does. A teleport moved its proxy in the tree and looked for its pairs again at every step: 0.8
       µs a character, where the motion costs 0.3. Each move therefore sees the other characters where they were
       when the step began, whatever their order.
     - The entity's RigidBody and Collider make no body beside it; a CharacterController coming or going makes the
       entity's body again as what it now is.
     - **Measured** (Profile): 64 characters walking a floor cost a frame 221 µs, 3.5 µs a character: 2.6 µs the
       physics module's move (2.1 µs in its own bench's world), 0.3 µs the body's motion, 0.3 µs the moves' queries
       passing over the character's own body, the rest the pass and the write-back. The frames of resting,
       spinning, compound and hinged bodies, and the full pass, are within the noise of #218's (x0.97 to x1.014);
       a world without characters skips the pass.
9. **A first-person starter game**, `samples/game_starter`.
   - **The player:** a `CharacterController` with a camera at eye height.
     - **Controls:** mouse look (yaw on the body, pitch on the camera), walk (W A S D), run (Shift), jump (Space), and
       interact (E).
     - **Interact:** a ray from the eye pushes what it meets from the point it meets it. A door swings.
     - **Props** are pushed by walking into them.
   - **Playing a level:** the editor plays the project's game, or this one where the project names none. The player
     starts at a `PlayerStart` entity, or where the editor's camera stands.
10. **The editor.**
    - **Fields:**
      - enumerations show their names in a list (a field hint, `NPH_FIELD_ENUM`), and bit masks show as boxes
        (`NPH_FIELD_FLAGS`);
      - an entity field takes an entity dragged from the outliner;
      - a GUID field takes an asset of its kind dragged from the browser.
    - **Drawing:** the viewport outlines hulls and meshes, draws triggers in a colour of their own, and draws joints'
      anchors and axes.

The step lands as about ten pull requests, stacked (ADR-0102; the user's batching of 2026-11-19):
1. sensors, with this record;
2. combine modes;
3. compounds;
4. collision assets in the runtime;
5. triggers;
6. joints;
7. physics materials;
8. the character controller;
9. the editor's fields and drawing;
10. the starter game.

## Alternatives considered

- **Triggers by overlap queries in the runtime:** the bridge would query each trigger's shape each step, with no
  physics change. But the character, which is not a body, would need its own test, and events would come in the
  bridge's order rather than the step's. Sensors in the step reuse the tree, and the character's body serves rays too.
- **Triggers as pairs of the broadphase (PhysX's trigger pairs; Box2D 3.0's sensor contacts):** each overlapping pair
  kept and tested every step, as contacts are. It reuses the pairs' bookkeeping, but puts sensors in the contact
  pipeline (made and destroyed with the pairs, kept out of the solver's graph and the islands), which Box2D 3.1 took
  them out of for a pass at the end of the step.
- **The character's body as a shape of mask 0:** it would touch nothing, but the world's queries are two-way (Box2D's
  filter: the shape's mask must meet the query's category too), so no ray would see it either. A ghost is seen by its
  filter, as any shape.
- **Two queries a sensor, to count and then to fill** (as the broadphase's pairs are found): measured 0.33 ms a step
  against 0.18 ms for one query into each sensor's own room.
- **The character as a dynamic capsule:** every contact pushes it, it slides down slopes, and it needs friction tricks
  to stand still. A first-person controller is kinematic, as the physics module's is (ADR-0049).
- **Physics materials as fields alone:** they stay as each collider's default. But a level of hundreds of surfaces
  wants ice made once and changed once.
- **Compounds as a component (a list of shapes on one entity):** the hierarchy already places the parts and the editor
  moves them; a list would duplicate those transforms.

## Consequences

- **New formats:**
  - `Collider`'s new fields (`collision`, `trigger`, `physics_material`) and the new components. Text scenes still read
    by field name. Binary scenes' layout hashes change, so their cooked copies cook again.
  - The physics material source and its asset, `PMAT` version 1.
- **New physics API:** sensors and ghosts with their events, `sensor_events`, a query filter's `sensors`, and combine
  modes. All are deterministic: sets are kept sorted, and modes are compared as integers.
- **CPU costs**, to be measured by the physics and bridge benchmarks:
  - a sensor: a tree query and its exact tests a step, on the job system (measured above);
  - a character: its moves (at most four slides) and its body's move, a step;
  - scaled copies of collision blocks: at load.
- **Play needs no game:** the editor walks any level with the starter game.
