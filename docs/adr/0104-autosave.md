# 0104. Autosave: a session directory under a lock the system lets go of, and the work offered back

- **Date:** 2026-11-17
- **Status:** Proposed (Phase 15's first step, with ADR-0103, ADR-0102)

## Context

ADR-0102 puts autosave first in Phase 15, with the crash handler, so that the user's work is kept from the phase's
first day. Before it, the editor saved only when asked. A crash, in the editor's code, a game module's, or a
driver's, lost everything since the last save. ADR-0103 records that the crash handler does not save the scene: a
process that just failed cannot be trusted to serialize a world.

What autosave has to settle:

- **When.** Often enough to lose little, never in the middle of a change, and free of hitches. It runs at the
  frame's sync point, where the world is quiescent, so its cost is a frame's.
- **In what form.** The text scene format costs 2.9 µs an entity to write, and the binary format 0.10 µs
  (`benchmarks/results/scene-2026-10-15`): 29 and 1.0 ms for 10,000 entities, 294 and 10 ms for 100,000. An
  autosave is read back only by the build that wrote it, and never diffed.
- **How the next start knows the last one crashed.** A marker file that a clean exit removes cannot tell a crashed
  editor from one still running beside it (two on one project), and a process id can be reused.
- **Where.** Not in the project's sources: the asset browser lists them, and Cook Folder cooks the scenes there.

## Decision

1. **A session directory per running editor**, under `editor.autosave_dir`. The default is `autosave`, in the
   working directory, which is the project's, as `assets` and `cooked` are. It is named
   `<seconds since 1970>-<pid>`, with `-<n>` after it while a directory holds that name: an editor attached
   again in one process within the second would otherwise take over the last one's directory, a crashed
   one's work with it. It holds:
   - `lock`, a lock that lasts as long as the process (platform/file.h `nph_file_lock_*`, new). On Windows it is a
     file opened without sharing; on Linux, `flock`. The system lets go of either when the process ends, however
     it ends;
   - `session.json`: the scene's file, when the session began and last autosaved, and the source of a mesh being
     edited;
   - `scene.nscene`, the edited world as a binary scene, with the one before as `scene.previous.nscene`, in case
     the newer one holds what failed;
   - `mesh.nmesh`, the mesh being edited, while it has changes.

   Every file is written beside its place and renamed over it, so none is ever half written. A clean exit removes
   the directory.
2. **When.** Autosave runs at the sync point when the scene or the mesh being edited has changes the last autosave
   does not hold, `editor.autosave_interval` seconds (default 60, saved; 0: never by time) have passed since the
   last autosave, nothing is being dragged or typed into, and the world is not playing. It also runs before Play,
   whatever the interval, since a game's code runs from there. Saving, opening, or starting a new scene leaves
   nothing unsaved, so the scene's autosave goes then.
3. **What a start offers.** At attach, every other session directory whose lock can be taken belongs to an editor
   that did not end cleanly, since a running editor's is held. The newest such directory with work in it is
   offered in a "Recover Work" popup, which says how old the autosave is, what it holds, and whether the scene's
   file was saved after it:
   - **Recover** puts the mesh back in its source, cooked again, and opens the scene in place of the edited one,
     under the scene's own file and unsaved. It goes into this session's autosave at once.
   - **Discard** removes the directory.
   - **Later** keeps it for the next start.

   A crashed session's directory with nothing to recover is removed when found. Older ones wait for later starts.
4. **An exit nobody confirmed keeps its changes.** When the editor ends with unsaved changes that no "Unsaved
   Changes" answer let go (the console's `quit`, a failure), it autosaves them and keeps its directory, as a crash
   would. The next start offers them back.

## Alternatives considered

- **The text format** for the scene's autosave: diffable, and readable by every later build, but 29 times the cost
  at the sync point, 30 ms a frame at 10,000 entities. A recovery right after a crash runs the same build.
- **Writing on a thread** from a copy of the world: no frame pays for the write, but copying the world costs about
  what the binary write does. That is for the city-sized worlds of step 8, which will autosave a cell's scene file
  at a time.
- **Autosave when idle,** after a pause in the input: it hides a cost the binary format no longer has.
- **A journal of the history's steps,** replayed after the last save: finer than an interval, but the history holds
  handles and values in memory, and replaying needs the exact world it began from.
- **A marker file and the process id:** this cannot tell a second running editor's session from a dead one's, and
  a process id is reused. The lock can.
- **The scene saved from the crash handler:** see ADR-0103.

## Consequences

- At most `editor.autosave_interval` seconds of work are lost to a crash, and none to a crash in play.
- An autosave costs the binary format's write at the sync point: about a millisecond for a scene of 10,000 entities
  (the editor logs what each took). A world of hundreds of thousands of entities would pay about 10 ms a minute;
  step 8 splits such worlds into files.
- A recovery across builds whose binary scene versions differ fails to open, and the work stays where it is.
- `platform/file.h` gains `nph_file_lock_acquire` and `nph_file_lock_release`, which any tool can use to tell a live
  process's files from a dead one's. A crash test shows the lock free after the process that held it crashed.
