# 0045. Physics: the solver comparison (soft step, TGS, XPBD)

- **Date:** 2026-10-23
- **Status:** Accepted (2026-10-23, the user's choice under CLAUDE.md §13:
  the soft step by default, TGS and XPBD kept as options)

## Context

CLAUDE.md §8.9 specifies a substepped soft-step solver "compared against
TGS/XPBD via an ADR with benchmarks", and §13 leaves the final choice open
until Phase 11. ADR-0043 built the soft step (Catto, "Solver2D", 2024) and
ADR-0044 put joints in it. This ADR measures the alternatives on the same
footing:

- **TGS** (temporal Gauss-Seidel, PhysX 4): per substep, rigid rows with a
  Baumgarte bias (a fifth of the error per substep, a 5 mm slop) on the
  error since the step began; one velocity pass without bias after the
  substeps.
- **XPBD** (Müller, Macklin, Chentanez, Jeschke, and Kim, "Detailed Rigid
  Body Simulation with Extended Position Based Dynamics", SCA 2020): per
  substep, the bodies predicted, positions corrected by rigid constraints
  (contacts' penetration, static friction on the slip, joints' anchors,
  axes, and limits; springs compliant), velocities derived from the
  positions, then a velocity pass for dynamic friction, restitution,
  motors, and dampers.

All three run on the same collision, the same constraint graph and its
colours, the same threads and stages, and the same eight-lane kernel
layout (TGS reuses the soft step's kernels; XPBD has its own position and
velocity kernels and position-level joints). The world's `solver` setting
selects one; the default is the soft step.

## Measurements

Profile build, Ryzen 7 5800X; scenes in `engine/physics/tests/test_solvers.c`
(run with `--verbose`), costs from `benchmarks/physics`. "Soft 8" is the
soft step at 8 substeps, whose default stiffnesses scale with the substep
rate (contacts an eighth of it, joints a quarter); "XPBD 16" is XPBD at 16,
since it converges with substeps.

### Quality

| Scene | Soft 4 | Soft 8 | TGS 4 | XPBD 4 | XPBD 16 |
|---|---|---|---|---|---|
| 20-row pyramid (210 boxes): top sinks / drifts aside | 1.9 / 0.35 mm, asleep in 0.53 s | 0.5 / 0.07 mm, asleep 0.50 s | 2.7 / 1.7 mm, asleep 0.55 s | collapses (2.4 m) | collapses (0.69 m) |
| a column of 10: top sinks / drifts | 1.7 / 0.04 mm | 0.5 / 0.08 mm | 2.9 / 2.9 mm | 0.6 / 1.0 mm | 0.04 / 0.4 mm |
| a box under one 100 times heavier: heavy box sinks | 2.2 mm | 0.54 mm | 1.5 mm | 1.5 mm | 0.08 mm |
| 10 links carrying a body 100 times a link: stretch | 9.7 mm | 2.2 mm | 161 mm (8 substeps: breaks) | 11.3 mm | 0.8 mm |
| 20 links whipping down from the side: worst joint gap | 1.2 mm | 0.33 mm | 9.7 mm | 2.5 mm | 0.14 mm |
| 20° pendulum, 10 periods: period error, amplitude kept | 0.000 %, 100 % | 0.000 %, 100 % | 0.000 %, 100 % | −0.083 %, 90 % | −0.022 %, 97 % |
| door slammed into its hinge limits at 6 rad/s: overshoot, anchor gap | 0.04°, 0.92 mm | 0.04°, 0.21 mm | 0.26°, 0.05 mm | 0.40°, 0.01 mm | 0.08°, 0.00 mm |
| four ragdolls dropped in a heap: worst joint gap, settled | 20 mm, asleep in 3.5 s | 4.3 mm, asleep 3.5 s | 24 mm, asleep 3.5 s | 33 mm, asleep 3.9 s | 3.9 mm, asleep 3.2 s |
| ball, restitution 0.5 from 1 m: bounce (0.25 m) | 0.247 | 0.248 | 0.247 | 0.248 | 0.250 |
| box at μ 0.6 on 20°: creeps in 1.4 s | 0 | 0 | 0 | 2.1 mm | 0.5 mm |
| box at μ 0.6 on 40°: slides at (1.797 m/s²) | 1.799 | 1.798 | 1.801 | 1.895 | 1.891 |

The ragdolls' elbows and knees carry 2 N m of friction, as their ball
joints do. Measured first without it, a dangling forearm swung for ever
and kept the heap awake wherever one ended up hanging: under TGS and XPBD
here, and under the soft step itself on GCC's rounding, which sends the
heap elsewhere (a heap is chaotic). The knees, which first bent forward,
bend back.

### Cost (µs a 240 Hz step)

| Scene | Soft 4 | Soft 8 | TGS 4 | XPBD 4 | XPBD 16 |
|---|---|---|---|---|---|
| the 20-row pyramid, one thread | 385 | 522 | 349 | 484 | 888 |
| 1000 mixed shapes piled in a bin, the job system | 673 | 1127 | 591 | 622 | 1418 |
| 16 swinging chains of 25 links (400 joints), one thread | 573 | 1037 | 483 | 442 | 1444 |

(Collision is common to all: 0.19 ms of the pyramid's step.)

## Reading

- **The soft step is the only candidate that does everything well.** It
  stacks (a 20-row pyramid within 2 mm, asleep in half a second), keeps
  energy (a pendulum's period and amplitude exact), gets friction exactly
  (to three decimals of Coulomb's law), and settles ragdoll heaps to
  sleep. Its weakness is compliance: a soft joint gives about 2 g / ω² per
  carried body weight, so heavy loads on light joints stretch (9.7 mm for
  10 links under 100 link weights) and slammed limits open the anchors
  briefly (0.9 mm). Doubling the substeps, at 1.4 to 1.8 times the cost,
  quarters that give (2.2 mm, 0.2 mm), because the default stiffnesses now
  follow the substep rate.
- **TGS is the cheapest (9 to 16 % less) and stacks nearly as well**, but
  its Baumgarte-biased joints do not converge under load: the heavy chain
  stretches 161 mm at 4 substeps and comes apart at 8. It would need
  per-joint tuning to be safe.
- **XPBD has the stiffest joints** (at 16 substeps nearly rigid: 0.8 mm
  under the heavy load, no gap at the door) **but cannot stack**: the
  20-row pyramid collapses at 4 substeps and still at 16, as Catto found in
  2D ("Solver2D"). It loses energy (10 % of a pendulum's swing in 10
  periods), its friction is 5 % weak with creep on a slope that should
  hold, and its cost at the substeps its joints need is 2.1 to 2.5 times
  the soft step's.
- What XPBD does better (joint stiffness) the soft step reaches by more
  substeps, keeping its stacking, energy, and friction; what TGS does
  cheaper it pays for in joints.

## Decision

The soft step is the default solver, at 4 substeps, with its stiffnesses
relative to the substep rate (the measurements make the substep count the
one quality setting). **TGS and XPBD stay in the engine as supported
options**, selected per world (`NphPhysicsWorldDesc::solver`):

| Solver | Choose it for | Avoid it for |
|---|---|---|
| soft step (default) | everything: stacks, piles, ragdolls, vehicles, exact friction and energy | joints that must stay rigid under very heavy loads at 4 substeps (raise the substeps) |
| TGS | worlds of stacked and piled bodies where the step's cost matters most (9 to 16 % cheaper) | heavy loads on joints (chains, cranes, ragdolls in piles) |
| XPBD | stiff machinery and rigs (joints near rigid at 16 substeps) | stacks and piles, sliding friction, long-lived oscillations |

The recommendation was the soft step alone (removing TGS and XPBD, since
no scene showed another winning outright, at the cost of three solvers to
maintain); the user chose to keep the options.

## Alternatives

- **The soft step alone**, TGS and XPBD removed: one solver to maintain and
  test, the recommendation.
- **TGS as the default**: the cheapest, but joints under load are not
  robust.
- **XPBD as the default**: the stiffest joints, but stacking fails.

## Consequences

- Every solver feature exists in all three: the next joints (slider,
  distance, 6-DOF) and the ragdoll come with soft-step, TGS (the soft
  step's rows, hard), and XPBD (position-level) forms.
- All three are tested as the soft step is: the comparison scenes for
  soundness, and the ISA kernels' agreement to the bit and the results'
  independence of the worker count for each; their benchmark rows carry
  budgets.
- XPBD's stacking and TGS's loaded joints are known limits, documented
  with the guidance above, not defects to work around in a scene.
- The default stiffnesses are now fractions of the substep rate (contacts
  an eighth, joints a quarter); explicit values in hertz still override
  them.
- The comparison found and fixed a friction error common to all three
  solvers (the friction circle's impulse kept the stopping impulse's
  direction; kinetic friction now opposes the slip: ADR-0043, the
  commit "kinetic friction opposes the slip").
