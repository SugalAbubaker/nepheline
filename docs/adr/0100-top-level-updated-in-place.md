# 0100. The top level updated in place where only what its records hold changes

- **Date:** 2026-11-16
- **Status:** Accepted (the user's go-ahead of 2026-11-16, "Fix the stutter, then measure", of the
  offer to fix the top level's rebuild each time the camera travels 16 m before Phase 14's exit
  measurements; built and measured; accepted the same day after the user tested Bistro and the
  city in the viewer: "commit it and open a PR")

## Context

The rays trace one top-level acceleration structure over the instances (rt.c). Its space is the
world less an anchor near the camera, so 32-bit coordinates hold anywhere: ray origins are offset
by 256 units in the last place of their coordinates in that space (rt_common.glsl), at most half
a millimetre while the camera stays within 16 m of the anchor (`RT_ANCHOR_RADIUS`). When the
camera goes further, the anchor moves to it, and every record's translation with it.

The production path (ADR-0029's GPU scene) rebuilt the structure whole each time: when instances
it holds changed, after a frame that traced nothing, the frame after compacted bottom levels were
swapped in, when a material's ray key changed (ADR-0098), and on each move of the anchor.
ADR-0063 measured such a build at 2.85 ms over a world of the game's size (574,356 instances
traced), once every 16 m, every 2.7 s at a sprint, and left what rays need of the far field to
Phase 14's ray-traced effects; ADR-0066's amendment carried it, and nothing since built it.

**Measured 2026-11-16** (c03bb30, the RTX 3060 Ti, `render_perf` in Profile, medians of Bistro's
turn's first 60 frames and of the city's 240, with the window busy; a variant that rebuilds the
structure every frame to price one rebuild):

| View | Instances | `rt instances` | `rt accel` | Frame |
|---|---|---|---|---|
| Bistro's street turn | 2,909 | 0.000 | 0.194 | +0.13 |
| The city's rooftop | 246,784 | 0.154 | 1.223 | +1.36 |

A camera moving through the still city rebuilds nothing else: a sprint of 932 frames (6 m/s, the
city's `street_sprint` run ten times its frames) logged each rebuild's cause, and found 18, all in
its first 0.3 s, while the cells' instances streamed in (16 for 15,007 or 943 instances changed,
one for the first frame, one owed to a compaction), and none while it moved. In a still scene the
anchor's move is the stutter: 1.38 ms in one frame every 16 m on the city.

Vulkan offers an update in place for a top level built with
`VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR`: it keeps the tree a build made and refits
its boxes to what the records hold now. Its rules (the specification's "Acceleration Structure
Update Rules"): the same records, count and flags as the build, and no record turning active or
inactive ("Any transition between the active and inactive states requires a full acceleration
structure rebuild"); what the records hold, transforms and the bottom levels they name included,
may change. An update keeps the tree's shape, so it traces as fast as a build where the records
moved together, and slower the further they moved apart. The anchor's move is a translation of
every record: the refit tree is the build's, translated.

The update needs the records in the same places as at the build. `rt_instances.comp` appended the
active records with one atomic a subgroup, so their order changed from frame to frame with the
order the subgroups ran in; an update over such records is valid (the active records still come
first) but refits a tree whose leaves hold other instances than the build put there, which traces
far slower. Records at their slots (inactive ones between active ones) would hold still, but
lavapipe (Mesa 25.2) drops an active record behind an inactive one (rt.c), so the active records
stay first.

## Decision

1. **The GPU module updates top levels** (`gpu/accel.h`): `NPH_GPU_ACCEL_ALLOW_UPDATE`, its
   scratch size (`NphGpuAccelSizes.update_scratch`), and `nph_gpu_cmd_update_tlas`, which records
   the update in place under Vulkan's rules, stated in its contract.
2. **The records in their slots' order.** `rt_instances.comp` writes them in three dispatches: the
   live instances of each block of 64 slots counted; one workgroup turning the counts into each
   block's first record and their total; each block's live records written from its first in slot
   order (a scan in shared memory, which depends on no grouping of the invocations into
   subgroups), and every record past the total inactive. The same instances give the same records
   in the same places, frame after frame.
3. **The production path builds the top level whole only where a record may turn active or
   inactive**: when instances it holds changed (shown, of a mesh with triangles, before or after),
   after a frame that traced nothing, after the reference drew one, or when the record count
   changed. Where only what the records hold changes (the anchor's move, a compaction's swap, a
   material's ray key), it updates the structure in place. The structure is built with
   `PREFER_FAST_BUILD` and `ALLOW_UPDATE`; the reference path builds every frame as before.
4. **The frame statistics say which** (`NphRenderFrameStats.shadow_rebuild` for a build whole,
   `shadow_update` for an update), and the editor's statistics panel shows it.
5. **The rays' structures are made last** (renderer.c), after every buffer the frame's passes
   read. The top level built to allow updates is larger, and made before those buffers it moved
   every one of them elsewhere in device memory, which slowed passes that never touch it
   (Measured, "Where the structure lives"); made last, a size of theirs moves nothing else.

Moved instances still have the structure built whole: an update after instances move apart from
where they were built would trace slower and slower, and choosing when to build again is a
question for moving content, which the reference scenes do not have (the game's NPCs will).

## Measured

The RTX 3060 Ti, `render_perf` in Profile, the medians of Bistro's street turn's first 60 frames
and of the city's rooftop's 240, with the window busy (ms). A variant had the production path
take new records every frame, which a still scene takes as an update in place (what each move of
the anchor costs); another had it build the structure whole every frame:

| | Bistro: `rt instances` | `rt accel` | City: `rt instances` | `rt accel` | City's frame |
|---|---|---|---|---|---|
| Built every frame, before (c03bb30) | 0.000 | 0.194 | 0.154 | 1.223 | +1.36 |
| Built every frame, after | 0.000 | 0.195 | 0.138 | 1.214 | +1.38 |
| Updated every frame, after | 0.000 | 0.029 | 0.138 | 0.312 | +0.48 |

- **The frame that moves the anchor** costs the city 0.45 ms where it cost 1.38, and Bistro 0.03
  where it cost 0.19. A world of the game's size (574,356 instances traced, ADR-0063) pays about
  in proportion: estimated, not measured, an update of about 0.7 ms with its records where a build
  took 2.85.
- **The records in slot order cost no more** than the append: 0.138 ms on the city against 0.154,
  three dispatches against two (the append first wrote every record inactive). Blocks of 1,024
  slots, 16 rounds of 64 a workgroup, first cost Bistro's few thousand slots 0.062 ms, their rounds
  run one after another; blocks of one round run side by side.
- **Where the structure lives.** The renderer made the rays' structures before the frame's other
  buffers (post, depth of field, the visibility buffer, TAA, motion, occlusion, the shadow map,
  the radiance cache). The structure built to allow updates is larger, so the first build of this
  change moved all of those to other places in device memory, and with no change of their own the
  radiance cache's filter ran 4 % slower on Bistro's turn (0.392 to 0.394 ms in three runs against
  0.379) and 7 % on the synthetic scenes (27 of them against main's run; this pull request's GPU
  job failed their budgets by 0.001 to 0.003 ms), the blended surfaces 3 to 6 %, Bistro's frame
  0.04 to 0.07 ms. Variants told the two apart: the structure built without the flag, in a buffer
  of its old size, was as fast as before (0.376); the same tree in a buffer of the new size was as
  slow (0.390). Made last, the structures move nothing: the filter 0.377 ms on Bistro, the
  synthetic scenes' 1.000 of main's (the blended surfaces' 0.993), every renderer budget held
  (CI's command, the window idle), and the steady frames within noise of before (Bistro 7.03 ms
  against 7.10, the city 7.79 against 7.80; `reflect rays` 1.43 against 1.45 and 1.55 against
  1.55).
- **Memory:** the structure built to allow updates is larger, the scratch a little smaller:
  Bistro's (119,041 slots) 20.65 to 22.24 MB, its scratch 9.80 to 9.33; the city's (291,584)
  50.58 to 54.47 MB, its scratch 24.01 to 22.84. The blocks' counts: 4 bytes a 64 slots (18 KB on
  the city).

## Alternatives considered

- **A wider anchor radius, or none:** the ray offsets grow with the coordinates' magnitude in
  the top level's space (256 ulp: half a millimetre within 16 m, two within 64), which moves the
  picture's contact shadows and self-shadowing (ADR-0094's slack). Rejected by ADR-0041's rule.
- **The next anchor's structure built ahead of the move**, in frames with time to spare, or on
  the async compute queue: the same build, moved; the frames at the gate have no time to spare,
  and work on this card's compute family slows the graphics queue's passes (ADR-0099).
- **Records at their slots, inactive ones between:** no compaction at all, but lavapipe loses
  active records behind inactive ones; a path for lavapipe alone would leave CI's GPU tests
  testing another one.
- **What the rays meet of the far field as proxies** (ADR-0063's fewer instances): a smaller
  structure to build, but rays then meet proxies where they met the instances, which changes the
  image. Left to the game, with the user.
- **An update for moved instances too:** the tree degrades as they move apart, and when to build
  it again needs moving content to measure; built whole, as before.

## Consequences

- A camera travelling through a still scene costs an update every 16 m instead of a build: on
  the city 0.45 ms in that frame where it cost 1.38. It is not gone: the update still visits
  every record, and a world of the game's size pays in proportion (about 0.7 ms, estimated).
- Every write of the records runs three dispatches where it ran two, at no more cost.
- The structure is built with `ALLOW_UPDATE`: 1.6 to 3.9 MB more of it (a little less scratch);
  the rays trace it as fast.
- The rays' structures are made after the frame's buffers, so their sizes, which follow the
  device, the build's flags and the instances' capacity, no longer move those buffers. The
  passes' times depend on where their buffers lie: a change of 1.6 MB made two of them 4 to 7 %
  slower here, and a change elsewhere in what the renderer makes, and in what order, can do it
  again (the budgets' 5 % catches it).
- A frame after compacted bottom levels were swapped in, and one where a material's ray key
  changed, update instead of building (`Shadows.bottom_levels_are_compacted_and_cast_the_same`).
- Moving content (the game's) still builds the structure whole every frame it moves; ADR-0063's
  far-field question for the rays stays open.

## Tests

- `GpuFixture.a_top_level_updated_in_place_traces_its_new_records` (test_accel.c): a top level
  built over three records (one inactive), updated in place from records that move two
  triangles and rename one: rays find them where each set of records puts them, and nothing where
  the inactive record lies.
- `Shadows.the_top_level_is_rebuilt_only_when_it_must_be`: the camera 24 m on updates the
  structure (and the shadows are right from there), and so does a move back with an instance
  hidden between others' records (the floor as unshadowed as a drawing without the occluder);
  instance changes, a frame without rays and the reference still build it whole.
- `ShadowRecords.the_top_levels_records_are_the_shown_instances_in_slot_order`: 3,002 instances
  over 47 blocks of slots, every seventh hidden: the records read back are the shown ones in slot
  order and inactive after them, and the same after the update. A control that places each
  record by a global atomic, as the append did across workgroups, fails it (2,469 and then all
  2,573 of the active records out of place).
- All of it on the RTX 3060 Ti in Development with validation and synchronization validation
  (the 141 CTest entries), on lavapipe (Mesa 25.2.8: the GPU module's tests, the shadow tests and
  the cells' top-level test), and the changed tests in Debug with the address sanitizer.
