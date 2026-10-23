# 0043. Physics: the world, the soft-step solver, and its threads

- **Date:** 2026-10-22
- **Status:** Accepted (the solver is provisional until the comparison of
  CLAUDE.md §13, which this ADR does not settle)

## Context

ADR-0042 built the collision layer. CLAUDE.md §8.9 asks next for a
substepped soft-step solver (Macklin et al., "Small Steps in Physics
Simulation", SCA 2019; Catto, "Solver2D", 2024), compared against TGS and
XPBD by an ADR with benchmarks before the final choice (§13); warm
starting, graph colouring for a parallel solve, island detection and
sleeping; a fixed 240 Hz step within 1.0 ms of job-system time; and results
that are deterministic run to run, which the tests check by state hashes.

The rest of the plan constrains it:

- Bodies are 64-bit in the world (ADR-0042 §2); everything the solver
  touches should be relative and 32-bit.
- No allocation per frame in steady state (§10.5), none hidden (§1.4).
- Per-frame work runs on the job system (§2), whose results must not
  depend on its worker count (ADR-0020 §8 holds the ECS to the same).
- Scalar and SIMD code agree to the bit, with FMA contraction off
  (ADR-0008 §6).
- Physics stays independent of the ECS and the renderer (ADR-0042 §1).

## Decision

1. **The world follows Box2D v3's architecture in 3D.** Bodies and shapes
   in pools behind 32-bit generational handles (20 bits of slot, 12 of
   generation); contacts in a pool, found by shape pair through a hash map,
   in intrusive per-body lists; three broadphase trees (static, kinematic,
   dynamic) and a move buffer of proxies that grew; awake lists of bodies,
   contacts, and islands, so a step's cost follows what is awake. It is
   the published, measured design of a modern solver (Catto's "Solver2D"
   and his talks on simulation islands and dynamic trees), and it maps to
   3D without change of structure. The code is our own.
2. **The step is pairs, collide, solve, finalize, islands**, each pass
   either parallel over records that write only their own data or serial
   in a fixed order (the awake lists, the move buffer). New pairs are found
   in two passes (count, then fill) so each query owns a slice of the
   output. Contacts that began or stopped touching take effect serially
   after the parallel narrow phase. Determinism across worker counts
   follows from the structure, not from locks.
3. **The solver is the soft step.** Four substeps per 240 Hz step (960 Hz):
   integrate velocities, warm start, solve with soft contacts (a spring of
   the world's stiffness and damping ratio, its push capped at 3 m/s),
   integrate positions, relax rigidly without bias; restitution after the
   substeps. The current separation inside the step comes from the bodies'
   displacements applied to anchors fixed at the step's start, so no
   collision runs inside the step. Speculative points stop the approach
   exactly; friction is a coupled two-dimensional row clamped to the
   friction circle. This is §8.9's choice; whether it stays, against TGS
   and XPBD, is the §13 decision, taken with benchmarks once joints exist
   (they are where the solvers differ most).
4. **Contacts default to 120 Hz, an eighth of the substep rate.** A soft
   contact holds a load at a depth of g / ω² per body weight carried per
   unit of effective mass. Box2D's 30 Hz is an eighth of its 240 Hz
   substep rate; keeping the ratio at our 960 Hz gives the same arithmetic
   per substep with sixteen times less give: 1.7 mm between a box and one
   100 times heavier on it (27 mm at 30 Hz), 1.9 mm at the top of a 20-row
   pyramid (26 mm). Contacts with static and kinematic bodies are twice as
   stiff, damping is a ratio of 10, and the stiffness is capped at a
   quarter of the substep rate.
5. **The gyroscopic term is implicit, per substep, in body space** (Catto,
   "Numerical Methods", GDC 2015), on by default. A box spun about its
   intermediate axis tumbles and keeps 98.5 % of its energy over six
   seconds. The same Newton iteration in the world frame with the step's
   starting inertia avoids two rotations per substep but kept 84 %: the
   inertia must turn with the body. Bodies with isotropic inertia (spheres,
   cubes) skip the term, which is exactly zero for them.
6. **Constraints are coloured persistently.** A contact keeps its colour
   while it is in the graph; colours keep bitsets of their dynamic bodies;
   24 colours, the first 20 open to dynamic pairs, colour 0 reserved for
   them (contacts with a static body need only their dynamic body free);
   what fits nowhere is solved serially in an overflow before the colours.
7. **The kernels run eight contacts per instruction and agree to the bit
   across ISAs.** Solver bodies are one cache line of four 16-byte rows
   (velocity, angular velocity, displacement, rotation), gathered eight at
   a time by 4x4 transposes. Contacts are grouped eight to a record by
   colour, with points in structure-of-arrays rows carrying the effective
   masses and the angular terms I^-1 (r x axis), so applying an impulse
   needs no inertia (Jolt's formulation). Preparation, warm start, solve,
   relax, and restitution are kernels compiled from one source at SSE4.2
   (pairs of SSE registers) and AVX2 (one register) with the same
   arithmetic in the same order; the best is chosen at creation.
8. **The stages run in lockstep on spinning helpers, with claimed blocks
   and a thread count from a cost model.** The step's solver work is one
   unrolled list of stages (about 60 for four colours, 200 for fifteen).
   Each participating thread starts at its own share of a stage's blocks
   and claims blocks by atomic exchange, forward while free, then
   backward, so a thread keeps the same blocks, and their data in its
   cache, from substep to substep; the calling thread then takes what is
   left, so an unstarted or preempted helper holds back only its claimed
   block; the thread that finishes a stage's last block publishes the
   next. Helpers spin between stages, since a stage lasts microseconds and
   a wake costs more. The step takes as many threads as its work pays for:
   `1 + work / (stages x 450 ns)`, the work estimated as 600 ns per point
   row and 290 ns per body (measured; docs/physics.md "Threads").
9. **Islands are persistent and split lazily** (Catto, "Simulation
   Islands", 2023): a contact that begins touching between dynamic bodies
   links their islands as a union merged at the end of the step (the
   larger absorbs the smaller); one that stops touching counts a possible
   split; an island is split into its components only when it is about to
   fall asleep, one island per step. Sleeping islands cost nothing per
   step.
10. **Events are arrays, not callbacks.** Begin, end, hit, and move events
    are valid until the next step; nothing calls user code during a step,
    so nothing can change the world while it is in flight. Move events are
    how the runtime mirrors bodies into the scene.

## Alternatives considered

- **PGS with Baumgarte or split impulses** (Bullet, early PhysX): a rigid
  solver is stiffer than a soft one under load, but needs many iterations
  for tall stacks and mass ratios, and position correction by Baumgarte
  injects energy. TGS (PhysX 4+) and XPBD (Müller et al. 2020) are the real
  competitors; they are measured against the soft step for the §13
  decision.
- **One substep per step** (and more solver iterations): the soft step's
  quality comes from small substeps, and at 240 Hz one substep is Box2D's
  rate; four keep stacks and joint chains stiffer, at a cost measured in
  the benchmarks. The substep count stays configurable.
- **Islands as the unit of parallelism** (Jolt): solving islands on
  separate threads is simple and cache-friendly, but one big island (a pile,
  a ragdoll heap) runs on one thread. Colouring parallelises inside an
  island.
- **One job per stage, or a shared block counter.** A job wake per stage
  costs more than the stage; one counter shared by every thread is a
  contended cache line, and hands each thread different blocks every
  substep. Claimed blocks with per-thread starting shares cut both.
- **All the job system's workers, always.** Measured: past the point the
  work pays for, each extra spinning thread makes a stage slower (a
  210-box pyramid solves in 0.099 ms on 6 threads and 0.193 ms on 16), and
  spinning threads take workers from the rest of the frame.
- **The gyroscopic term once per step, or in the world frame.** Both are
  cheaper and both lose energy (the world-frame form kept 84 % against
  98.5 % in the intermediate-axis test). Most engines leave the term off
  by default (PhysX, Jolt); a game's tumbling props are where it shows.
- **Box2D's 30 Hz contacts.** Its numerics at our substep rate are four
  times softer than Box2D's own; see decision 4.

## Consequences

- Measured on the reference machine (Ryzen 7 5800X, Profile,
  `benchmarks/physics`), per 240 Hz step: a 20-row pyramid (210 boxes)
  0.369 ms on one thread and 0.140 ms on the job system; sixteen pyramids
  (3360 boxes) 0.99 ms; a 1000-shape pile in a bin 3.4 ms and 0.66 ms;
  10 000 sleeping boxes 2.8 µs. The six-core reference CPU class of §3
  holds a few thousand resting bodies, or a thousand piled, inside the
  1.0 ms budget.
- The narrow phase is now the larger cost on one thread (2.1 ms of the
  pile's 3.4 ms): every awake contact runs its pair function every step.
  Persistent manifolds for resting contacts (refreshing the separations of
  points whose bodies have barely moved relative to each other, as
  PhysX's PCM does) are the next optimisation.
- With every logical core spinning, one preempted thread stalls the rest;
  the cost model keeps small scenes on few threads, and a desktop with
  other programs running sees the variance at full width.
- Soft contacts give under load, a millimetre or two at the defaults for
  ordinary stacks and mass ratios; a game that wants stiffer contacts can
  raise `contact_hertz` up to a quarter of the substep rate.
- Joints, the solver comparison, and the remaining shapes build on these
  structures: joints join the graph and the islands as contacts do.
