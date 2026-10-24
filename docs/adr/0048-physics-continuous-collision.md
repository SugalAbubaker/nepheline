# 0048. Physics: continuous collision, and impacts

- **Date:** 2026-10-24
- **Status:** Accepted (2026-10-24)

## Context

CLAUDE.md §8.9 asks for continuous collision: speculative contacts plus
conservative advancement for fast bodies. The world steps at 240 Hz with
speculative contacts (ADR-0043), which stop a body at a surface its
manifold reaches; beyond that reach a body can pass through a thin wall
between two steps' collisions, or sink into a block too deep to come back
out the way it went in. Gameplay throws things that fast: bullets, debris,
a character's dash.

Building it, a drop sweep (boxes 0.2 × 0.2 × 0.3 m dropped flat from 0.5
to 10 m onto a floor, every solver) found that boxes did not land flat even
when nothing tunnelled. The soft step tilted them up to 18° and bounced
them at 1 m/s with no restitution; TGS laid them on their sides from a
metre and upside down from 5 m; XPBD laid them on their sides from 10 m.
Five mechanisms, each measured by switching it alone:

1. The speculative distance was a fixed 2 cm. A box at 10 m/s, 4.7 cm
   above the floor at one collision, was 1.9 cm inside it at the next: the
   contacts pushed it out at an angle.
2. Gauss-Seidel loads a manifold's points one at a time. The first takes
   nearly all of an impact, and the torque left over tips the box.
3. Restitution, one pass over the points, the same.
4. Warm starts carried an impact's impulse into the next substep. Four
   points share a load in many ways, and one way of sharing moves nothing;
   rigid rows (TGS) cannot take that share back, and when a point's impulse
   reaches zero the share left over becomes motion: the box resting where it
   landed was launched and turned.
5. XPBD corrects the points' positions one at a time (the same tilt), and
   its friction, within μ times each point's uneven multiplier, spun a box
   about its vertical.

## Decision

1. **Continuous collision as Box2D v3's** (Catto, "Continuous Collision",
   GDC 2013). A dynamic body is fast when it moved farther in the step than
   half the inner radius of its thinnest shape (its centre's motion plus its
   turn's angle times its reach; Box2D's safety factor), or farther than
   speculative contacts reach (the fattened boxes' margin less a slop). In
   the finalize pass its shapes are swept over the step, the centre along a
   line and the rotation by slerp, against the static shapes they may meet,
   and the body is set back to the first time any comes within a slop of
   one; the next step's speculative contacts stop it. Its velocity is kept.
2. **Times of impact by conservative advancement** on GJK distances with a
   bound on rotation (Mirtich 1996; Bullet's btContinuousConvexCollision),
   in a frame at the moving shape (the casts' precision, ADR-0047).
3. **Three rules against stopping what does not tunnel.** A shape already
   within reach of a target at the start is swept as its core, a ball at
   its centroid a quarter of its inner radius wide (Box2D's fallback). A
   triangle is swept against only when the shape's centroid comes towards
   its plane by more than a slop (Box2D's rule for chain segments). An
   impact the speculative contacts already stopped is not set back: along
   the impact's axis the gap is linear in the fraction for a body that does
   not turn, and dips below the smaller of its ends by at most the arc's bow
   (r θ²/8) for one that does, so a shape within a slop of the target at
   both ends with that taken off never went through it. The axis is the
   closest features' exact normal or GJK's, whichever separates the two
   farther (`nph_physics_contact_axis`, shared with the casts).
4. **Bullets** also sweep against kinematic and dynamic bodies at their
   final poses, in a stage after the finalize pass (not against each
   other).
5. **Speculative contacts reach as far as a pair can close**: 2 cm, or the
   two bodies' relative motion and each one's spin times its reach over the
   step and a slop, up to the fattened boxes' 10 cm margin. Slow pairs keep
   2 cm exactly (a root-free test first).
6. **Impacts are swept more.** A manifold with a point approaching faster
   than 1 m/s at the step's start gets seven more sweeps of its normal rows
   in each pass, and of its restitution; XPBD's get them of their position
   corrections and of their velocity pass, friction included. Other
   manifolds are left exactly as they were.
7. **Restitution counts loaded points still a little apart** (Box2D's
   rule): the relax pass slowed them before they touched.
8. **Under TGS, a hit's warm start is scaled** until it pushes none of its
   manifold's points apart, the share found by applying it to copies of the
   bodies (impulses add linearly). The soft step warm-starts all of it.

## Alternatives considered

- **Time-of-impact substepping** (Box2D v2.4, PhysX's CCD): advancing the
  whole island to the first impact and solving again there. Exact for
  chains of impacts, but serial, several solves a step for a busy scene,
  and the time-of-impact island breaks the fixed-cost step (§10.5). Box2D v3
  left it for the scheme above.
- **Speculative contacts alone, without a sweep** (Jolt's default motion
  quality): no tunnelling cost at all, but no protection past the
  manifold's reach, which must stay near the fattened margin so the
  broadphase finds the pair in time.
- **A fixed speculative distance** (Box2D v3: four slops): bodies faster
  than it reaches sink in before their contacts see them (mechanism 1).
- **Restitution only at points touching at the step's end**: tried first
  against mechanism 3; a ball of restitution 1 came back from 2 m to
  0.5 m, since the relax pass had slowed it before it touched. The sweeps
  fixed mechanism 3 instead.
- **Warm-start rules for impacts**, each measured on the drop sweep, a
  216 kg crate dropped onto a lying ragdoll, and a heap of 16 ragdolls
  (soft step; the crate's sink and the heap's worst joint-limit overshoot):

  | Rule | Landings (worst tilt) | Crate, heap |
  |---|---|---|
  | warm-start everything (Box2D) | soft 0.8°, TGS upside down | 1.3 mm, 3.0° |
  | nothing for points that approached fast, at the step's end only | TGS 15° from 1 m, upside down from 5 m | |
  | the smaller of the last two substeps' impulses | soft 0.4°, TGS 43° from 10 m | |
  | the warm start from before the hit, while the lane is hit | flat | 7.7 mm, 10.2° |
  | the same until the hit is over | soft 0.7°, TGS 0.6° | 3.7 mm, 4.9° |
  | scaled to push no point apart, every solver | soft 0.1°, TGS 0.2° | 4.8 mm, 8.7° |
  | scaled against static and kinematic bodies only | soft 0.1° | joints 14 mm open, 10.5° |
  | scaled under TGS, all of it under the soft step (chosen) | soft 0.2°, TGS flat | 1.2 mm, 3.3° |

  A heavy body landing on a light one is stopped over several substeps,
  which the warm starts converge: any rule that drops them on a hit
  starves that, and only rigid rows need it.
- **Removing the share that moves nothing** from the warm starts (projecting
  each four-point manifold's impulses onto those that move the bodies): the
  root of mechanism 4, but a null vector per lane and a clamp, for each of
  the normal and friction rows. The scaled warm start handles the case that
  matters (a hit) without it.
- **Judging an impact in the middle of a pass** (the point's speed when the
  pass reaches it): contacts in a heap of jointed ragdolls passed 1 m/s
  every step, the joints' pull, which cost the heap 12 %.
- **A reach without the spin, or trimmed to each point's own approach**:
  measured against the thrown ragdoll's energy (below), neither changed
  it; the pair's bound stands.
- **A separate stage for the sweeps**: one more barrier a step, 3 % of a
  small world's step; plain fast bodies are swept in the finalize pass,
  which leaves the static shapes alone, and only bullets wait for a stage.

## Consequences

- Boxes dropped flat from 0.5 to 10 m land within 0.2° under the soft
  step, flat under TGS (1.4 cm/s of rebound) and XPBD, and come to rest
  flat (tested: `solvers.boxes_dropped_flat_land_flat`). The crate on the
  ragdoll sinks 1.2 mm and the heap's joints overshoot their limits by 3.3°
  at worst (they were 2.2 mm and 3.2°).
- The reach costs one scene: a ragdoll thrown cartwheeling at the ground
  under the soft step gains 3.5 % of its energy at worst where it gained
  1.3 % (its bound is 4 %); dropped, it gains 4.3 % where it gained 5.4 %,
  and under TGS, thrown and dropped, 2.7 % and 23 % where it gained 14 %
  and 56 %.
- Fast bodies never pass a wall 2 cm thick at 300 m/s, a blade spinning a
  tenth of a turn a step never cuts through one, and bodies sliding at
  60 m/s across a mesh's seams are never stopped (tested).
- Costs (Profile, the reference machine, against the same build without
  any of this, side by side): the resting scenes 0 to 3 % (two lane masks
  per contact group, a root-free test per body and per contact: the
  pyramid 0.2 %, the pile 2.1 %, the heap of ragdolls 2.4 %); chains whose
  lower links swing fast enough to be swept 2.7 % (65 of 400 links a step),
  under TGS 4.6 % (303). A gallery of 200 balls and boxes at 50 to 200 m/s,
  a third of them bullets, all swept every step, steps in 323 µs on one
  thread and 190 µs on the job system.
- XPBD at 16 substeps keeps more of a pile in its bin (805 of 1000 shapes,
  against about 500: they used to be thrown out), so the pile costs more
  to step, 2.3 ms against 1.5.
- Open: a body's rotation is swept as a slerp, which a body spinning many
  turns a step aliases (as Box2D's); chains of impacts within a step (a
  body set back from one wall into another's reach) wait for the next step.
