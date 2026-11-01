# 0070. Levels of detail: a flat cull of the levels an instance's distance may take

- **Date:** 2026-10-31
- **Status:** Accepted

## Context

[ADR-0066](0066-phase-13-exit.md) made a cheaper walk down the level hierarchies Phase 14's first
task, for Bistro's 0.04 to 0.08 ms. [ADR-0067](0067-the-game-is-a-city.md) made the city a gate, where
the walk costs more: 0.13 ms in a street, 0.39 ms on the rooftop. Its first idea, an instance needing
level 0 whole skipping the walk, was already in the code (ADR-0060); the rest had to come from its
second, fewer dependent steps.

A timer on each step showed where the time goes:

| View | Nodes | Steps a phase | Phase 0, by step (µs) | Phase 1, by step (µs) |
|---|---|---|---|---|
| Bistro street | 1,434 | 3 | 11.4, 11.3, 9.7 | 10.4, 2.7, 2.6 |
| Bistro hedge | 372 | 3 | 10.8, 10.6, 9.7 | 2.7, 2.7, 2.6 |
| City street | 12,434 | 4 | 18.0, 21.8, 20.4, 26.3 | 12.9, 11.9, 12.0, 9.1 |
| City rooftop | 65,210 | 4 | 78.6, 207.6, 31.3, 31.3 | 13.1, 12.5, 2.5, 2.7 |

A step with no work takes 2.7 µs; one with any takes at least 10 µs, whatever it holds. That is the
dispatch, not the lanes: one workgroup of 64 lanes rather than 512 took it from 11.4 to 10.6 µs. Past
that, the cost is about 5 ns a node visited (64 lanes an item, about 0.4 ns a lane).

Meanwhile a cluster's test costs 0.34 ns, and a street's instances mostly hold what cannot be seen:
of 400,000 clusters tested on a city street, 284,000 are behind something, and each is tested twice,
in both phases, because the walk's phase 0 does not test occlusion.

And the levels have a shape a tree does not use. Across all 612 meshes of the city and Bistro every
level's clusters are a run, and a level's largest error and smallest maker error only rise from fine
to coarse. So an instance's distance alone rules out most levels, with no node visited, and what is
left is short: the city's most used mesh has 889 clusters in 8 levels, and past 105 m is 13 of them.

## Decision

1. **A table of a mesh's levels.** Sixteen bytes a level, made at creation from the groups, which lie
   level by level: the largest group error (never less than a finer level's), the smallest error of
   the groups that made its clusters, and the run of its groups. With it, a radius about the mesh's
   sphere's centre that holds every group's sphere.
2. **An instance's distance picks a run of groups** (`vis_lod_span`), in the instance cull: from the
   coarsest level down, past those whose every maker is surely too coarse even from that sphere's far
   side, then on while a level's largest error may be too coarse from its near side. These are the
   walk's two prunes, written for one sphere and many errors and as loose, so a level left out holds
   no group the exact test takes.
3. **One flat cull a phase** (`vis cull groups`, `vis_cull_groups.comp`): a lane a group, with the
   walk's tests of a group, whose record now carries its makers' smallest error where it held its
   level, which no shader read. No tree, no steps, nothing to wait for. Its decision is
   `vis_lod_group_too_coarse`, so the meshlet cull still decides alike, bit for bit.
4. **Occlusion on the group.** Phase 0 tests the previous frame's depth from the previous camera, and
   a group hidden there waits, whole, for phase 1, which tests this frame's (Karis et al., "Nanite: A
   Deep Dive", SIGGRAPH 2021, does so for its nodes). A group's sphere holds every triangle of its
   clusters, so a hidden group's clusters are hidden. A hidden group costs a lane a phase where its
   clusters cost a lane each in both.
5. **Short runs as items, long ones as chunks.** A dispatch ends with its slowest subgroup, and a near
   instance of a big mesh may hold thousands of groups. A run of up to a workgroup's goes as (instance,
   group) items, the subgroup's together, as meshlets do. A longer one goes as chunks, one record and
   one workgroup per 64 groups, so it runs in parallel and takes few writes.
6. **Level 0 whole keeps its own order,** as level 0's meshlets handed straight to the meshlet cull,
   up to 4,096 of them (past that, the writes would hold its subgroup too long, and it takes the flat
   cull). Clusters a group at a time raster slower: Bistro's `vis raster 0` by 0.01 to 0.04 ms.
7. **The walk stays where the flat cull does not fit:** a level apart (ADR-0063), whose makers the
   table cannot hold; a mesh with voxel levels (below); groups out of their levels' order; and a run
   of more than `max_lod_flat_groups` (2^14). The frame takes steps only for meshes that may walk, so
   the city and Bistro now take none. A step with nothing to do now runs nothing: phase 0's used to
   run one workgroup each.
8. **A stale queue is fixed,** in the camera's walk and the shadow map's. The queues are three in turn,
   and each step emptied the one its predecessor read, which a step with no workgroups could not do. A
   walk of four steps whose second had nothing to take then read the first's queue again, and drew its
   clusters twice. A step now zeroes its own arguments as it starts, so that a queue left full is
   never taken again. Two new tests fail without this.

## What it does

Quiet, the median of three rounds, interleaved, against the commit before.

**The city's frame:**

| View | Mean (ms) | 1%-low (ms) |
|---|---|---|
| street | 6.45 -> 6.27 | 6.49 -> 6.34 |
| walk | 6.51 -> 6.35 | 6.68 -> 6.51 |
| sprint | 6.65 -> 6.50 | 6.90 -> 6.71 |
| turn | 6.47 -> 6.29 | 7.30 -> 7.08 |
| crossing | 6.55 -> 6.37 | 6.71 -> 6.50 |
| park | 7.68 -> 7.50 | 7.78 -> 7.56 |
| rooftop | 10.16 -> 9.81 | 10.24 -> 9.90 |

**Its culling, street and rooftop (µs):**

| Pass | Street | Rooftop |
|---|---|---|
| `vis cull nodes` (both phases) | 135 -> 0 | 386 -> 0 |
| `vis cull groups` (both phases) | 0 -> 39 | 0 -> 109 |
| `vis cull meshlets` (both phases) | 147 -> 84 | 566 -> 470 |
| `vis cull instances` (both phases) | 89 -> 95 | 85 -> 96 |

**Bistro's** cost is mostly each dispatch's floor, so it gains less: means 0.01 to 0.03 ms faster, its
1%-lows from street 7.01 -> 6.99 and hedge 7.22 -> 7.19 to turn 7.68 -> 7.70, all inside a round's spread.

**Against Phase 13's start** (ADR-0066's test: 8274d5f with its cook, interleaved, four rounds):

| View | Start, mean (1%-low) | Now, mean (1%-low) | Triangles |
|---|---|---|---|
| street | 6.89 (6.96) | 6.90 (6.96) | 1.41M -> 1.09M |
| walk | 6.94 (7.04) | 6.96 (7.06) | 1.39M -> 1.06M |
| turn | 7.13 (7.64) | 7.14 (7.68) | 204k -> 185k |
| terrace | 7.42 (7.50) | 7.44 (7.52) | 878k -> 706k |
| hedge | 7.07 (7.15) | 7.09 (7.16) | 443k -> 380k |

ADR-0066 recorded +0.04 to +0.08 ms. This is +0.01 to +0.02 ms, inside each round's spread of 0.09 to
0.13 ms and drawing 9 to 24% fewer triangles, so its target is met to what can be measured, and not
strictly: what remains is `vis cull groups`' two floors and the shadow map's empty steps.

**The landscape** (lists at 16 times, its levels' voxels on the walk, cooked without its cells) draws the
same to within a few clusters of 5.5 million, no slower: means -0.05 to -0.18 ms in one round, its 1%-lows
within 0.22 ms, and 6.39 GB as before.

**The images** of every still view, in the normals and instances debug views, are the same to the bit
as before, in both scenes. Lit images differ only as two runs of the same build do (the warm-up's TAA
and GI).

**Cost.** Declared traffic falls: the city's frame 6.76 GB -> 6.20 GB, Bistro's 3.85 GB -> 3.67 GB, the
walk's steps gone for two passes that read and write less. GPU memory is the same to the 0.01 GB
reported: the flat cull's lists add 4 MB at the default capacity and 17 MB at the city's, and a level
is 16 bytes. `lod_4k_production`'s walk of 0.069 ms is now 0.020 ms.

## Alternatives

Each measured on the way.

- **One workgroup an item, 64 lanes rather than 512:** my first guess at the step's floor, and wrong
  (11.4 -> 10.6 µs). It is the dispatch that costs.
- **Items for every run, through the subgroup's shuffles:** a street's `vis cull instances` rose by 45 µs,
  a near instance of the city's largest mesh writing 11,557 groups from one subgroup, while the rest of
  the dispatch waited on it.
- **Each lane writing its own run:** the same subgroup, worse: 650 µs, a store at a time rather than a
  subgroup's together.
- **Level 0 whole through the flat cull too,** which would drop the shortcut and test its groups for
  occlusion: 9 µs better in the instance cull and 8 to 40 µs worse in the raster on Bistro, for the
  order clusters reach it in.
- **The flat cull for voxel meshes:** the landscape wants over 4 million groups a frame. Their
  simplifier, kept as it was by ADR-0069, leaves too wide a range of errors in a level for a distance to
  pick few. There the tree's spatial pruning earns its keep, and it is to be looked at again with their
  raster (ADR-0067). This also showed the danger: overflowing a list of groups drops geometry.
- **A persistent walk in one dispatch** stays rejected: ADR-0060's reason holds, and the flat cull needs
  no workgroup to wait on another.
- **Fewer levels a step, or more:** three levels a step (512 lanes an item) leaves the floors, and only
  shaves one step of four.

## Consequences

- This does not reach the gate alone. The city's turn (7.08), park (7.56) and rooftop (9.90) are still
  over 6.94 ms, as are all of Bistro's 1%-lows (6.99 to 7.70). The largest lever left in the culling is
  the floors: about 10 µs each for six dispatches a frame, even where they have little to do. Masked
  raster comes next (ADR-0067).
- ADR-0066's second target, the landscape's walk cut, is not met: its voxel meshes keep the walk, 1.77 ms
  at Phase 13's end and 1.76 ms now in the forest view, with its cells streamed as ADR-0066 measured it.
- A list of groups that fills drops work, as the other lists do. `max_lod_groups` (2^18) scales with a
  manifest's `lists`; the rooftop tests 430,000 groups of its 2^20. `render_perf` and `scene_viewer` hold
  it at four times its default, since what would want more walks.
- The walk's code stays in use for voxel meshes and levels apart, and the shadow map's walk is unchanged
  but for the stale queue. It could take a flat cull of its own, its threshold being one number a level:
  8 to 44 µs are there, less than the floors it would add.
- The reference budgets are re-baselined (`reference_scenes-2026-10-31b`).
