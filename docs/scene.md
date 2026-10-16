# Scene module (`engine/scene`)

The scene module is the engine's entity-component system (CLAUDE.md §8.7):
archetype storage, queries with change detection, command buffers, a
scheduler that runs systems in parallel on the job system, 64-bit
transforms and the hierarchy, prefabs, and scene files. Decisions:
[ADR-0020](adr/0020-ecs.md) (the ECS) and
[ADR-0021](adr/0021-scene-serialization.md) (scene files and prefabs).
It depends on `jobs` and, through it, on `platform`, `math`, and `core`.

| Header | Contents |
|---|---|
| `entity.h` | `NphEntity` (32-bit generational handle), `NphComponentId` |
| `component.h` | field kinds, editor hints, `NphComponentDesc`, the X-macro generators, validation, field-wise comparison and hashing, layout hashes, `NphText` |
| `world.h` | the world: registry, entities, per-entity component access, change ticks, the event stream, stats, state hash |
| `query.h` | queries, chunk iteration, change filters, chunk collection for parallel work |
| `command.h` | command buffers: deferred spawns, despawns, adds, sets, removes, reparenting |
| `schedule.h` | systems, the schedule, the system context |
| `transform.h` | Name, LocalTransform, WorldTransform, Hierarchy; transform math; the hierarchy API; the transform system |
| `prefab.h` | PrefabInstance; prefabs and instantiation |
| `scene_io.h` | text and binary scene files |

## Using it

```c
// A component: the X-macro lists the fields once.
#define GAME_VELOCITY_FIELDS(F) F(VEC3, value, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(GameVelocity, GAME_VELOCITY_FIELDS);
NPH_COMPONENT_DECLARE(GameVelocity);
NPH_COMPONENT_DEFINE(GameVelocity, GAME_VELOCITY_FIELDS, "Velocity", 0U, NULL) // in one .c

NphWorldDesc wd = {0};
wd.alloc = nph_allocator_system();
wd.max_entities = 1U << 20U;
NphWorld *world;
NPH_TRY(nph_world_create(&wd, &world));
NphComponentId velocity;
NPH_TRY(nph_world_register_component(world, GameVelocity_component(), &velocity));

const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                 NPH_COMPONENT_ID_WORLD_TRANSFORM, velocity};
NPH_TRY(nph_world_spawn_batch(world, comps, 3U, 100000U, NULL));

// A system: called per chunk, on every worker.
static void integrate(NphSystemContext *ctx, const NphChunkView *v) {
    NphLocalTransform *t = NPH_CHUNK_COLUMN(v, 0, NphLocalTransform);
    const GameVelocity *vel = NPH_CHUNK_COLUMN(v, 1, GameVelocity);
    for (u32 i = 0; i < v->count; ++i) {
        t[i].translation = nph_dvec3_add_vec3(t[i].translation,
                                              nph_vec3_scale(vel[i].value, (f32)ctx->dt));
    }
}

NphScheduleDesc sd = {world, jobs, nph_allocator_system(), 0U};
NphSchedule *schedule;
NPH_TRY(nph_schedule_create(&sd, &schedule));
const NphQueryTerm terms[2] = {nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM),
                               nph_term_read(velocity)};
NphSystemDesc move = {0};
move.name = "game.move";
move.run_chunk = integrate;
move.terms = terms;
move.term_count = 2U;
NPH_TRY(nph_schedule_add_system(schedule, &move, NULL));
NphTransformSystem *transforms;
NPH_TRY(nph_transform_system_create(world, nph_allocator_system(), &transforms));
const NphSystemDesc tf = nph_transform_system_desc(transforms); // runs after game.move:
NPH_TRY(nph_schedule_add_system(schedule, &tf, NULL));           // it reads what move writes

for (;;) {
    NPH_TRY(nph_schedule_run(schedule, 1.0 / 240.0)); // one step, every core
}
```

`samples/scene_demo` builds an orrery this way and renders it with the
reference renderer (below).

## Components and reflection

A component is a plain C struct whose fields are listed once in an
X-macro. `NPH_COMPONENT_STRUCT` generates the struct.
`NPH_COMPONENT_DEFINE` generates `T_component()`, which returns a
constant-initialised descriptor:

- the component's stable name (its key in scene files);
- its size and alignment, and a default value;
- per field: the name, the kind, the offset, the size, and the editor
  hints (range, hidden, read-only, color, angle, transient).

The descriptor function's statics are constant-initialised, so the
function costs nothing and needs no registration order. A world registers
it with `nph_world_register_component`, which validates it and returns
the id.

Field kinds are a closed set chosen for serialization: BOOL (b32), the
integer types, F32, F64, VEC2–VEC4, QUAT, DVEC3 (64-bit positions),
ENTITY (remapped when saved, loaded, or instantiated), STR_ID (a 64-bit
string ID, written as its text when the world's string table knows it),
and TEXT (64 bytes of inline UTF-8). There are no pointers: components are
copied with `memcpy`, moved between chunks, compared field by field
(`nph_component_fields_equal`), and hashed field by field
(`nph_component_hash_fields`; padding never matters).

Tags are components without data (`NPH_TAG_DEFINE`): a bit in the
archetype mask, no column.

Two flags change how the engine treats a component:

- **TRANSIENT** components are derived state, never saved. WorldTransform
  is one; fields can be transient too.
- **INTERNAL** components are maintained by engine functions only.
  Hierarchy is changed through `nph_world_set_parent`; `nph_world_set`,
  `nph_world_get_mut`, write queries in systems, and command buffers
  refuse it.

## Storage

Every distinct component set is an **archetype**, and an archetype stores
its entities in **16 KiB chunks**:

```
+--------+-----------------+---------------------+--------+--------+-----
| header | versions[ncol]  | NphEntity[capacity] | col 0  | col 1  | ...
| 24 B   | u32 per column  | (cache-line aligned)| aligned to 64 bytes each
+--------+-----------------+---------------------+--------+--------+-----
```

The capacity is the largest row count whose layout fits, computed when
the archetype is created. For example, about 110 entities of
LocalTransform, WorldTransform, and two vec3s; 4,000 tag-only entities.
An archetype that cannot hold one row is refused (NPH_RESULT_INVALID_ARGUMENT).

- **Dense chunks.** Removing an entity (despawn, or a move to another
  archetype) fills its row with the archetype's last entity. Every chunk
  but the last is therefore full, iteration never meets a hole, and the
  moved entity's record is updated.
- **Entities** are handles from core's generational pool (ADR-0006). The
  index width comes from the world's capacity, at least 10 bits; the rest
  of the 32 bits are the generation. A per-slot record holds the chunk and
  row. Released slots are reused FIFO after 1,024 other releases, so a
  stale handle meets a recycled slot only after the generation wraps many
  times. The entity table is allocated whole at creation.
- **Archetype edges.** Adding or removing component c from archetype A
  looks up a cache of (A, c, add) → B before computing the mask, so
  repeated moves cost a hash lookup.
- **Chunk memory** comes from 1 MiB slabs (64 chunks), allocated from the
  world's allocator when the free list runs dry and kept until
  `nph_world_trim`. A free chunk's body is poisoned for ASan. `max_chunks`
  caps the total; spawning beyond it fails with OUT_OF_MEMORY and changes
  nothing.

Structural changes (spawn, despawn, add, remove, reparent) are made on
one thread, outside schedule stages. Each one bumps the world's epoch,
and a query iteration that sees a different epoch asserts. Batch spawns
(`nph_world_spawn_batch`) resolve the archetype once, reserve every chunk
first (all or nothing), fill defaults by doubling copies, and write the
records in one pass.

## Queries and change detection

A query is up to 16 terms. REQUIRED terms must be present, EXCLUDED terms
absent, and OPTIONAL terms give a NULL column where absent. Each term
declares READ, WRITE, or NONE (a filter). A query caches its matching
archetypes. The world tests each new archetype against every live query,
with room reserved first, so a failure leaves nothing half-registered.

Iteration walks matched archetypes and their chunks, producing
`NphChunkView`s: the entity handles, the count, and the column per term.
`nph_query_collect_chunks` and `nph_query_chunk_view` split an iteration
across jobs.

**Change detection** is per chunk column:

- Every chunk keeps a version per column: the tick at which the column was
  last written through a WRITE term (stamped when the chunk is produced),
  `nph_world_set`/`get_mut`, a spawn or move into the chunk, or a command.
- A term with `changed` set makes the iteration skip chunks whose flagged
  columns are no newer than its `since` tick. An archetype that has none of
  the flagged components passes.
- Every system run gets a fresh tick (`nph_world_advance_tick`), assigned
  in schedule order before the stage starts. A system's `last_tick` is its
  previous run's tick. Direct writes between runs are stamped one tick
  ahead, so the next run of every system sees them exactly once.
- Ticks are 32-bit and compared with wrap-around
  (`nph_world_tick_newer`). The schedule clamps chunk versions and system
  ticks older than 2^30 ticks every 2^29 ticks
  (`nph_world_clamp_ticks`), so the comparison never flips. Code that
  keeps its own `since` tick across very long runs clamps it the same way.

## Command buffers

Systems run while the structure is frozen, so structural changes from a
system go to a command buffer:

- `nph_commands_spawn` returns a pending handle (generation zero), valid
  in that buffer. Later commands may target it, and ENTITY fields of
  values recorded in the buffer may refer to it.
- Applying first creates every spawned entity, in recording order, each
  directly in the archetype of the adds and sets that follow its spawn.
  It then applies everything else in order, resolving pending handles
  wherever they were recorded, inside component values too.
- Commands on entities that are no longer alive, and removes of absent
  components, are skipped and counted. A reparenting that would form a
  cycle is skipped with a warning.
- Blocks come from the buffer's allocator. Failure is sticky: recording
  stops, and apply reports the error without touching the world.

## Systems and the schedule

A system is a function plus its access. It either iterates its query
itself (`run`) or is called once per matching chunk (`run_chunk`), and
then the schedule splits the chunks across jobs. Extra `reads` and
`writes` declare components a system reaches by entity lookup (the
transform system reads parents' world transforms that way).

Building the schedule:

1. **Order.** The `after`/`before` constraints are sorted topologically
   (Kahn's algorithm), with registration order breaking ties. A cycle or an
   unknown name fails the build, and `nph_schedule_error` says which.
2. **Stages.** A MAIN_THREAD or EXCLUSIVE system is a stage of its own and
   runs on the thread that calls `nph_schedule_run`. EXCLUSIVE systems may
   change the structure directly; everything else is frozen while it runs.
3. **Dependencies.** Within a stage, a system depends on every earlier
   system it conflicts with (one writes what the other reads or writes),
   and on every earlier system it is explicitly ordered after.

Running a stage launches the systems without dependencies as jobs. Each
system, when it finishes, decrements its dependents' counters and launches
those that reach zero. A system starts the moment its last dependency
finishes, not when a phase does. When the job pool is full, the system
runs inline.

At the end of each stage, the command buffers the stage recorded are
applied:

- `nph_system_commands` gives each `run` system, and each piece of a
  `run_chunk` system, its own buffer.
- The buffers are sorted by system position, then by the first chunk of
  the piece, and applied in that order. The pieces of
  `nph_job_parallel_for` depend only on the chunk count and the batch, so
  a serial schedule, one worker, and sixteen produce the same world bit
  for bit (`commands_are_deterministic_across_worker_counts`).

Scratch memory (chunk lists, command buffers) comes from one arena per
worker plus one for the calling thread, reset at the start of every run.
A buffer's blocks come from whichever worker's arena is current when it
grows, so a fiber that migrates between workers stays safe. Exhausting an
arena fails the run with OUT_OF_MEMORY. Failures reported by systems
(`nph_system_report`), scratch exhaustion, and apply failures are kept
(the first one) and returned. The other systems still run, so the world
stays consistent.

## Transforms

Positions are 64-bit on the CPU and camera-relative 32-bit on the GPU
(§8.2):

| Component | Layout | Notes |
|---|---|---|
| LocalTransform | DVEC3 translation, QUAT rotation, VEC3 scale (56 B) | relative to the parent |
| WorldTransform | DVEC3 translation, 3 × VEC3 axes (64 B, one cache line) | transient, recomputed |
| Hierarchy | parent, first/last child, next/previous sibling, depth (24 B) | internal |

The composition `nph_world_transform_compose` multiplies the linear parts
in f32. It computes the translation in f64: the parent's translation plus
the parent's linear part times the local translation. A child 1 mm from a
parent 30,000 km away keeps the millimetre to within the f64 resolution
there, about 4 nm. `nph_world_transform_relative` gives the GPU's
camera-relative affine.

**The hierarchy** is intrusive. `nph_world_set_parent` appends the child
to its parent's list. It adds Hierarchy to either entity when needed and
renumbers the subtree's depths. It refuses cycles, and depths beyond
`NPH_HIERARCHY_MAX_DEPTH` (1024). Despawning an entity detaches it and
turns its children into roots; `nph_world_despawn_recursive` removes a
whole subtree.

**The transform system** (`nph_transform_system_desc` in a schedule, or
`nph_transform_system_update`) runs in two phases:

1. **Roots.** Entities without a parent, in chunks whose LocalTransform or
   Hierarchy changed since the previous run, are recomputed per chunk in
   parallel. Each recomputed entity is stamped with the run's id in a
   per-slot array.
2. **Children.** The phase is skipped when no root was recomputed and no
   hierarchy chunk changed. Otherwise children are bucketed by depth with
   a two-pass parallel counting sort over blocks of 16 chunks:
   per-block histograms, a serial prefix, and a parallel fill, which is
   deterministic. Each depth level then runs as a parallel-for. A child is
   recomputed when its own chunk changed or its parent was stamped in this
   run, so moving one root recomputes its subtree and nothing else.

A parent without a transform (a grouping entity) counts as the identity.

## Prefabs

A prefab (`prefab.h`) is a world of its own, created with a copy of the
target registry, holding template entities in slot order. Instance index 0
is the first entity.

`nph_prefab_instantiate` spawns one entity per prefab entity, directly in
its final archetype, plus PrefabInstance. It copies the values and
redirects ENTITY fields that point inside the prefab to the new entities
(others become null). It rebuilds the hierarchy with children in the
prefab's order and records the prefab's interned name, the entity's index,
and the instance root. Instantiation is all or nothing. A prefab may be
instantiated into different worlds from different threads.

Saving writes instances as differences from their prefab; loading rebuilds
them from the current prefab. See below.

## Scene files

### Text

```json
{
  "nepheline_scene": 1,
  "entities": [
    {
      "id": 0,
      "components": {
        "LocalTransform": {
          "translation": [0, 0, 10000000],
          "rotation": [0, 0, 0, 1],
          "scale": [1, 1, 1]
        },
        "Name": {
          "text": "root"
        }
      }
    },
    {
      "id": 1,
      "parent": 0,
      "prefab": {
        "name": "crate",
        "index": 0,
        "root": 1
      },
      "overrides": {
        "test.Position": {
          "value": [9, 2, 3]
        }
      },
      "removed": ["test.TagA"]
    }
  ]
}
```

The text form is canonical, so a re-save writes the same bytes:

- **Order.** Entities are in hierarchy pre-order (roots in slot order,
  children in their order). Ids are numbered in that order; loading in file
  order recreates the slot order and the child order. Components are
  sorted by name and fields are in declaration order.
- **Floats.** Each float is written in the shortest text that reads back
  exactly. The f32 form survives readers that parse doubles. Non-finite
  values are written as "nan", "inf", and "-inf". Vectors stay on one
  line.
- **References and omissions.** References are ids, and references outside
  the saved set are null. TRANSIENT components and fields are not written.
  U64 values above 2^63 are written as decimal strings. STR_ID fields are
  written as their text, or `{"hash": "..."}` when the world does not know
  it.
- **Prefab instances.** An instance entity carries `"prefab"` and three
  sections: `"overrides"` (changed fields), `"components"` (added
  components), and `"removed"` (removed components). ENTITY fields that
  point inside the instance compare by position, so an untouched instance
  writes only its reference. Without the prefab, an instance is written
  in full with `"full": true`; it loads as written, without the prefab.
- **Unknown names.** An unknown component is an error. An unknown field is
  ignored with a warning.

### Binary

```
header  magic 'NSCB', version 1
HEAD    entity count, flags
COMP    per component: name, layout hash, size
ARCH*   flags (hierarchy), component indices, entity count, file ids,
        then each column contiguous, 16-byte aligned
STRS    the texts of the string IDs in the data
HIER    (child, parent) pairs in file order
CRC-32C trailer
```

Column bytes are canonical: zero padding, zero text tails, transient
fields at their defaults, and references as id + 1. Equal worlds give
equal files. A component whose layout hash differs from the world's fails
with VERSION_MISMATCH, so stale cooked data is never misread. Loading
reserves every handle in file order (the slots follow the file), places
each group with one batch, and copies runs of rows.

Both loaders validate everything and are all or nothing: any failure
despawns what was created, and `NphSceneResult::error` says what was
wrong and where. Both accept exactly the same states; the fuzz targets
require an accepted file to reach a fixed point through both formats.

## Profiling

A schedule run is the profiler zone `scene.schedule`; each system run on
the calling thread (main-thread and exclusive systems, or a schedule
without a job system) is a zone of its own name, systems launched as jobs
are named by their job, and each chunk piece of a `run_chunk` system is a
zone of its system's name ([profile.md](profile.md)). System names belong
to the schedule: code that destroys a schedule while the profiler runs
calls `nph_profiler_forget_names` first.

## Threading model

| What | Rule |
|---|---|
| Structural changes, queries' creation, registration | one thread, outside schedule stages (asserted) |
| `nph_world_get`, `is_alive`, query iteration | concurrent, while the structure is frozen |
| `nph_world_set` on a present component, `get_mut` | not structural; the caller owns the data race question (declare access) |
| Systems | in parallel when their access does not conflict; the structure is frozen |
| Command buffers | one recorder at a time; applied at stage ends on the calling thread |
| Prefabs | read-only once built; instantiation changes only the target world |
| Chunk versions | relaxed atomics (several jobs may stamp one column with one tick) |

## Costs (§1.4)

- The world allocates its entity table (24 bytes per entity of capacity)
  at creation.
- Chunks come from 1 MiB slabs on demand, kept until trimmed.
- Registering components, creating archetypes and queries, and adding
  systems allocate bookkeeping.
- A schedule allocates its scratch arenas (1 MiB each by default) at
  creation. A run allocates nothing from the heap.
- The transform system allocates a stamp per entity of capacity at
  creation, and grows its bucket arrays when a hierarchy outgrows them.

## Performance

`benchmarks/scene/bench_scene.c`, Profile, Ryzen 7 5800X (16 threads, 15
workers), MSVC 14.44, 2026-10-15, median of 7 runs. Full JSON:
`benchmarks/results/scene-2026-10-15-win-msvc-profile-ryzen7-5800x.json`.
Worlds of 1,048,576 entities with LocalTransform, WorldTransform, and two
vec3 components.

| Benchmark | ns per entity | 1M entities | Notes |
|---|---|---|---|
| spawn_batch_1M | 18.2 | 19 ms | defaults by doubling copies, one handle per row |
| spawn_single_1M | 43.5 | 46 ms | |
| despawn_1M | 26.0 | 27 ms | swap-removal keeps chunks dense |
| add_remove_tag_1M | 126 | 132 ms | two archetype moves (edge cache hits) |
| get_random_1M | 49.0 | | a cache miss on the record and one on the chunk |
| query_read_1M | 1.73 | 1.8 ms | one thread, a dependent float sum |
| query_write_1M | 3.46 | 3.6 ms | one thread, position += velocity × dt |
| schedule_write_1M | 0.43 | 0.45 ms | the same as a `run_chunk` system on 15 workers (8.0x) |
| schedule_64_systems | 160 per system | | 64 empty systems in 4 conflict chains: the executor's cost |
| commands_spawn_1M | 131 | 137 ms | three commands per entity, recorded and applied |
| transform_roots_1M | 8.50 | 8.9 ms | every root moved; bound by memory (56 B read, 64 B written per entity) |
| transform_static_1M | 0.07 | 73 µs | nothing moved: one version check per chunk |
| transform_tree_1M | 13.3 | 14 ms | a forest of depth 4, fan-out 8, every root moved |
| save_binary_1M | 101 | 106 ms | canonical bytes per value |
| load_binary_1M | 36.3 | 38 ms | batch placement and contiguous copies |
| save_text_64K | 2,941 | | dominated by the C library's float printing |
| load_text_64K | 2,659 | | JSON parse, `strtod`, field lookups |

The sample's frame (12,022 entities, 12,014 of them in a three-level
hierarchy, every one moving) runs its orbit, spin, and transform systems
in about 0.15 ms of wall time on the same machine.

Text saving costs about 1 µs per fractional float in MSVC's C library
(`core_json_bench f32_shortest`). Integers take 16 ns through the engine's
own printer. A Ryu-class formatter (Adams, "Ryū: fast float-to-string
conversion", PLDI 2018) would remove most of the text save time if editor
scenes outgrow it.

## Testing

`engine/scene/tests/` (70 tests):

- Component generators, descriptor validation, field comparison and
  hashing, layout hashes, and text truncation at UTF-8 boundaries.
- The world: registry limits, every component operation, stale handles,
  dense chunks, trimming, all-or-nothing batch spawns, oversized
  archetypes, events, hashes, and copied registries. A world in a small
  arena runs out at every step of its growth in turn, and every failure
  must leave it consistent. A randomized comparison runs thousands of
  random operations against a reference model.
- Queries: validation, matching of later archetypes, optional columns,
  change filters and stamps, tick clamping (white-box), and the assert on
  a structural change mid-iteration.
- Command buffers: pending references (forward ones too), coalesced
  spawns, skipped commands, sticky failures, and world failures.
- The schedule: constraints, cycles, unknown names, stages, conflict-derived
  dependencies, parallel results equal to serial ones, conflicting
  systems that never overlap, worker-count-independent command order,
  change ticks across runs, disabled systems, misuse, failures, and
  scratch exhaustion.
- Transforms: the math against 4x4 references, large-world precision,
  links and depths, cycles, the depth limit, orphaning on despawn,
  recursive despawn, and the system against a recursive reference. Static
  content is skipped, a single change is localized, and parallel results
  equal serial ones.
- Scene files: byte-identical round trips, the exact text form, subsets,
  every field kind and its accepted spellings, 25 malformed files with
  their errors and rollback, binary round trips, every single-byte
  corruption, version and layout mismatches, and capacity.
- Prefabs: instantiation, internal references, instance differences,
  prefab changes reaching instances, full instances, and errors.

**Fuzzing.** The targets are `tests/fuzz/fuzz_scene_text.c` and
`fuzz_scene_bin.c` (the binary target repairs the CRC so mutations reach
the parser). Their seeds are written independently of the engine by
`scripts/fuzz_seeds_scene.py`, including a Python port of the layout hash,
and the unit tests load every seed. Before merging, the smoke drivers ran
750,005 text and 450,003 binary inputs locally without a finding. The
earlier, shallower runs had found three text/binary inconsistencies,
which are fixed.

**ThreadSanitizer.** CI runs the scene tests under TSan with the job
system's (`tsan (jobs, scene)`).

## Limitations and future work

- Prefabs are flat; nesting is editor work (Phase 9). Text ids are file
  order, so deleting an entity renumbers the ids after it
  ([ADR-0021](adr/0021-scene-serialization.md)).
- Cooking scenes inside `nph_cook` waits for the runtime's game modules
  (Phase 9), which own the game's components.
- Change detection is per chunk column (about 100 entities). Finer
  tracking can be added per system if a profile asks for it.
- `nph_world_set` and `get_mut` from systems are not checked against
  declared access; a debug-build access checker would catch undeclared
  writes.

## References

- Acton, "Data-Oriented Design and C++", CppCon 2014; Fabian,
  *Data-Oriented Design*, 2018.
- Martin, "Entity Systems are the future of MMOG development", 2007.
- Mertens, "Building an ECS" series (flecs), 2019–2020: archetypes,
  chunked storage, and the archetype graph.
- Unity Technologies, Entities package documentation: archetypes, 16 KiB
  chunks, chunk-level change versions.
- The Bevy engine's ECS scheduler (2020–2024): access-derived parallelism,
  per-system change ticks, and tick clamping.
- Ford, "Overwatch Gameplay Architecture and Netcode", GDC 2017:
  deterministic, system-ordered ECS simulation.
- Gyrling, "Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015
  (the job system the schedule runs on).
- Steele & White, "How to Print Floating-Point Numbers Accurately", PLDI
  1990; Adams, "Ryū", PLDI 2018 (shortest round-trip floats).
