# 0023. The runtime: one engine context and a pipelined fixed-step frame

- **Date:** 2026-10-16
- **Status:** Accepted

## Context

Phase 9 (CLAUDE.md §12) needs an engine loop that the player and the
editor both host. The plan fixes several properties of it:

- §2: no global mutable state outside an explicit `NphEngine` context;
  deterministic simulation with a fixed timestep; every per-frame piece of
  work on the job system.
- §8.12: init in module order, shutdown in reverse; the loop is input,
  simulation (fixed step), animation, render submission; configuration from
  the command line, config files, and cvars.
- §10.5 and §10.6: the simulation of frame N+1 overlaps the GPU rendering
  of frame N; the CPU runs at most one frame ahead; a fixed 240 Hz step for
  the §10 target; steady-state frames allocate nothing.
- The editor (Phase 9 too) edits the world between frames, plays a copy of
  it, and draws its UI over the scene.

## Decision

1. **One context.** `NphEngine` owns every subsystem: cvars and the
   console, profiling, the job system, the VFS and the asset loader, input,
   the window, the device and swapchain (or an offscreen target), the
   render graph, the renderer, the scene renderer, the worlds, the
   schedule, the clock, and the game module. Its cvars are members, copied
   from static templates, so two engines can coexist (tests do). Logging
   and core profiling stay process-wide (§2's exception); the engine starts
   core profiling only if the host has not.
2. **Three modes.** WINDOWED, OFFSCREEN (a device without a window:
   tests can run the whole engine, renderer included, on lavapipe), and
   HEADLESS (simulation only).
3. **The frame has one sync point.** Everything that touches the world
   from outside the simulation happens there, with the previous frame's
   simulation finished: swapping a game build, the game's `frame`
   callback, the host's `sync` hook (the editor's edits), the transform
   update, the scene renderer's sync, clearing the world's events. Then
   the next simulation starts as a job, and this thread renders what was
   mirrored. Rendering and the host's `render` hook never read the world.
   `rt.pipelined` off runs the simulation at the sync point instead.
4. **Fixed steps with captured input.** The fixed-step clock (Fiedler,
   "Fix Your Timestep!", 2004) decides the steps per frame, capped by
   `sim.max_steps` with the excess dropped and counted. Each step is one
   schedule run, and gets an immutable `NphSimInput` built on the main
   thread before the job starts (sim_input.h): edges accumulate until a
   step consumes them, so no input is lost or doubled whatever the ratio of
   frames to steps. `rt.lockstep` forces one step per frame.
5. **The world is mirrored, not shared.** The scene renderer turns world
   state into render instances and lights at the sync point, touching only
   what the change ticks and the event stream say changed; assets load
   asynchronously and never block a frame.
6. **Play copies.** Play-in-editor copies the edited world with the same
   entity handles (scene.md "Copies that keep their handles") and
   simulates the copy; stopping discards it. The edited world is never
   simulated in the editor.
7. **Settings are cvars**, in rising precedence: defaults, the user
   config (written back at exit, SAVED cvars only), `--config` files,
   command-line assignments.

## Alternatives considered

- **A render thread** owning the GPU, with the main thread simulating. It
  costs a thread that mostly waits, a hand-off of every renderer call, and
  two sync points instead of one. The job-based simulation gets the same
  overlap, and recording already runs on jobs (the render graph's
  parallel recording).
- **Double-buffered world state** (render reads N while simulation writes
  N+1). It doubles the memory of every mirrored component. The scene
  renderer's mirror is the double buffer, but only for what the renderer
  needs and only for what changed.
- **Variable timestep.** It is simpler and smooth at any frame rate, but
  not deterministic, and physics (Phase 11) needs fixed steps.
- **Simulating the edited world in the editor** and restoring a
  snapshot afterwards. A crash or a bug during play would lose edits, and
  the editor would need to undo every simulated change. A copy keeps the
  edited world untouched by construction.
- **A global cvar list** of static cvars. Simpler to declare, but global
  mutable state, and two engines in one process would fight over it.

## Consequences

- The editor reads and edits the world in its sync hook and draws its UI
  in its render hook from data it captured then. Hooks that ignore this
  race the simulation; TSan runs the pipelined engine (the runtime game
  tests) to catch it.
- A host that wants the world between frames calls `nph_engine_wait`.
- Rendering shows the latest simulated state without interpolation; when
  the simulation rate differs from the frame rate, motion can judder.
  Interpolation comes with physics (Phase 11, §8.9).
- Steady-state frames do not allocate. Reloads, migrations, play copies,
  and scene files do, and they happen at the sync point, outside the
  frame's budget.
