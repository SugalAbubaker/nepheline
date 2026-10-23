# 0044. Physics: joints in the soft step

- **Date:** 2026-10-23
- **Status:** Accepted; amended 2026-10-23 with the slider, distance, and
  6-DOF joints (items 10 to 14). The solvers they ride on are ADR-0045's.

## Context

CLAUDE.md §8.9 asks for fixed, hinge, ball-socket, slider, distance, and
6-DOF joints with limits and motors, solved by the substepped soft-step
solver of ADR-0043, deterministic, within the 1.0 ms step. Phase 11 ends
when stacking, ragdoll, and determinism tests pass (§12): ragdolls are
chains of ball joints with cones and twist ranges, and hinges at the knees
and elbows. The §13 solver comparison (soft step against TGS and XPBD) is
to be made with benchmarks, and joints are where solvers differ most, so
they come before it.

This ADR covers the first three joints (ball, hinge, fixed), how joints
enter the solver, and their semantics in the world; the amendment (items
10 to 14) adds the slider, distance, and 6-DOF joints, which came with the
ragdoll's exit tests.

## Decision

1. **A joint is two frames.** Each body holds a frame (an anchor and three
   axes) in its own frame; the joint is satisfied when the two coincide,
   less the motion it frees. X is the hinge or twist axis. A helper makes
   frames meeting at a world point with X along a world axis for the
   bodies' current poses. This is PhysX's and Jolt's convention, and one
   description serves every joint type.
2. **Three types first**: ball (anchors together; an optional elliptical
   cone for B's X axis, an optional twist range, optional friction against
   any rotation), hinge (anchors together and X axes aligned; an optional
   angle range, motor, and spring), fixed (frames together). At least one
   body is dynamic; the other may be static, an anchor in the world.
3. **Joints are constraints in the graph.** A joint takes a colour free for
   its dynamic bodies from the same colours as contacts, and the colour's
   stages solve its joints and its contacts together; what fits no colour
   is solved serially before the overflow's contacts. They are prepared with
   the contacts and warm started, solved softly, and relaxed rigidly in
   every substep. Separate joint stages would double the synchronizations a
   substep; a separate joint solver would lose the soft step's coupling of
   joints and contacts within each substep.
4. **Rows in Box2D's order**: spring, motor, limits, then the locked axes,
   then the anchors last, so the most important constraint is the one left
   exact after each pass. The errors are the anchors' offset (three rows at
   once, by Cramer's rule on the 3x3 effective mass), the rotation vector
   from A's frame to B's (fixed: three rows), the cross product of the X
   axes (hinge: two rows about A's Y and Z, whose accumulated impulses are
   kept as scalars so the warm start never applies one about the hinge), the
   twist from the swing-twist decomposition (limits, motor, spring), and
   the angle between the X axes along the swing's own axis, held inside an
   ellipse over the swing vector (the angle times the axis: half angles
   about A's Y and Z).
5. **Limits are speculative; springs act once a substep.** An open limit
   only stops the approach within the substep, a closed one is soft while
   solving and rigid while relaxing, as contacts are. A spring is a force:
   it is solved in the solve pass only (solved again while relaxing, an
   undamped 2 Hz spring lost a third of its swing in five periods; once, 4 %
   a period, its implicit integration's own damping).
6. **Joint stiffness: 240 Hz, damping ratio 2.** A quarter of the substep
   rate (the cap) and twice the contacts' stiffness, Box2D's ratio. A soft
   joint gives g / ω² per body weight it carries over its effective mass:
   4.3 µm at 240 Hz.
7. **World semantics.** Joined bodies' shapes do not touch unless the joint
   says so: the pair search skips them, creating the joint destroys their
   contacts (reporting the touching ones' end), and destroying it queues the
   shapes for the next pair search. Joints link islands as touching contacts
   do. A moving kinematic body wakes what hangs from it; a motor driving
   towards a speed keeps its bodies awake (otherwise a slow start falls
   asleep before it gets going: item 14). Destroying a body destroys its joints
   and wakes the bodies at their other ends.
8. **Scalar and compiled once.** Joints are few beside contacts and differ
   by type, so their arithmetic is ordinary scalar code, the same bits on
   every ISA level. Static and kinematic ends are never written (the solver
   body of static ends is shared).
9. **Reactions are read back**: the force and torque the joint applied to
   B in the last substep, for breakable joints in game code.

The amendment:

10. **The 6-DOF joint is six axes**: translation along A's X, Y, and Z,
    the twist about X, and the swings about A's Y and Z, each locked,
    limited (a range), or free, each with an optional motor (a speed up to
    a force or torque) and spring (towards a value), PhysX's D6 and Jolt's
    six-DOF constraint. The values come from the frames: the translations
    are B's anchor less A's along A's axes; the twist and swings come from
    the swing-twist decomposition of B's frame in A's, the swing
    quaternion's angles about Y and Z limited each on its own (a pyramid,
    where the ball joint's cone is an ellipse). Rows follow item 4's order
    (springs, motors, limits, locks, then the anchors). Locked groups keep
    the block rows of the other joints: all three rotations as the fixed
    joint's, both swings as the hinge's two axes, all three translations
    as the anchors' 3x3 block, so a fully locked 6-DOF joint is a weld and
    the rows a joint does not need are never built.
11. **The slider is the 6-DOF joint** with only translation along X free
    or limited and its drives on that axis: one code path for both.
12. **The distance joint is one row** along the line between the anchors:
    rigid at its length (a rod), a range instead (a rope is 0 to its
    length, slack below it), or a spring towards the length, and a motor
    on the length's rate. The anchors are the bodies' frames' origins; the
    rotations are free.
13. **Every joint in every solver** (ADR-0045): the soft step's rows (soft
    while solving, rigid while relaxing), TGS's (the same rows, rigid with
    a Baumgarte bias), and XPBD's position-level forms (each row's error
    closed by its compliance: zero for locks and limits, the spring's for
    springs) with their motors and dampers on the derived velocities.
14. **Any joint's motor driving towards a speed keeps its bodies awake**,
    not only the hinge's: a slider motor at 0.02 m/s fell asleep after
    9 mm, below the 0.05 m/s sleep threshold.

## Alternatives considered

- **Eight joints per instruction**, as contacts are solved: joints of one
  type would batch, but a step's joints are of mixed types and few, and the
  scalar solve costs 1.1 µs a joint a step. Revisit when a scene's joints
  outnumber its contacts.
- **Rigid joints** (no softness, as PGS solvers do): the soft step's
  stability rests on the soft solve followed by the rigid relax; rigid rows
  alone would inject energy through position correction.
- **Jolt's swing-twist cone in quaternion space** (the ellipse over the
  swing quaternion's components): both are ellipses over the swing's
  direction, the same along the principal axes (where the tests hold 30°
  and 15° half angles to 0.07°), different only in how they pass between
  them. The swing-vector form needs no decomposition of the swing
  quaternion; either serves shoulders and hips.
- **Position-based joints (XPBD)**: part of the §13 comparison, measured on
  the same scenes before the solver is final.

## Consequences

- Measured (Profile, the tests): a pendulum's period matches the closed
  form to 0.000 % with its anchors within 5 µm; a 20-link chain hangs
  1.7 mm long; ten links carrying a body 100 times heavier stretch 9.7 mm; a
  chain released sideways whips at 28 m/s with its anchors at most 1.2 mm
  apart; cones and twists hold within 0.07°, hinge limits within 0.04°;
  motors, friction, and springs match their closed forms; a reaction
  carries the body's weight to 0.5 %.
- Heavy loads stretch soft joints in proportion to the mass ratio: a body
  hanging from many light links opens each joint by about 2 g / ω² per
  carried weight. A rope carrying a car stretches visibly; more substeps or
  a mass ratio closer to 1 reduce it. The §13 comparison measures this
  against TGS and XPBD.
- A slam into a limit opens the anchors for a few substeps (0.8 mm for a
  160 kg door at 6 rad/s): the limit's row and the anchors' converge over
  substeps, not within one.
- Joints are bit-identical across ISAs and worker counts (tested), and use
  the platform's `atan2f` for angles: deterministic on one platform (§8.9),
  not across C libraries.
- Measured for the amendment (Profile, the tests): a slider holds its
  axis to 4 µm under gravity and stops at its limit exactly (0.5000 m); a
  force-limited motor reaches the speed its force allows (0.15625 m/s); a
  rod holds its length to 0.05 mm through a swing and a rope never exceeds
  its length; springs ring at their frequency (0.5000 s at 2 Hz); a 6-DOF
  joint holds its twist and swings at 20.00°, 30.03°, and 15.07° against
  limits of 20, 30, and 15, and drives a wheel at its motor's 3.0000 rad/s.
- A ragdoll is ball joints at the spine, neck, shoulders, and hips and
  hinges at the elbows and knees, all with 2 N m of friction (the hinges'
  as a motor to standstill): a joint without friction lets a dangling limb
  swing for ever and keeps its heap awake. The exit tests drop, throw, pile,
  and crush ragdolls under every solver (docs/physics.md, ADR-0045).
