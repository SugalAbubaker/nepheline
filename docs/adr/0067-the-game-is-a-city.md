# 0067. The game is a city: its reference scenes, and 144 FPS until it is built

- **Date:** 2026-10-30
- **Status:** Accepted (the user's decisions of 2026-10-30; supersedes
  [ADR-0059](0059-structure-checkpoint.md)'s decisions 3 and 4 and
  [ADR-0041](0041-frame-target-gates.md)'s decision 6; CLAUDE.md §10.1, §10.4,
  §10.7, §12 and §13 amended); amended 2026-10-31: the landscape leaves the
  nightly (below)

## Context

ADR-0059 recorded the first game as a forested mountain valley, picked from
options that held no first-person city. The landscape and the voxel far field
(ADR-0061 to ADR-0063) were built for it. Having walked through the landscape
in `scene_viewer`, the user stated on 2026-10-30 the game they will build: a
first-person shooter with elements of roleplay, like GTA V, in a big city with
many streets and buildings, large indoor facilities with many lights, and many
interactible NPCs. It has no large forests. They build it after Phase 15, and
optimize the engine throughout its development against the requirements that
come up. Until then they want the reference scenes at 144 FPS at native 4K,
and the game brought to 240 FPS as it is built.

**The forest's cost is not a city's.** The landscape's frame is 52.9 to 64.2 ms
and its 1%-low 53.2 to 68.5 ms, of which the raster takes 42 to 52 ms (ADR-0066).
A street's cost is per-pixel instead. Bistro's street against §10.4
(`reference_scenes-2026-10-30`, RTX 3060 Ti, native 4K, ms):

| §10.4 line | Budget | Street |
|---|---|---|
| Culling + depth pyramid | 0.25 | 0.47 |
| Visibility buffer raster (HW + SW) | 0.65 | 0.75 |
| Material + shading + direct light | 1.20 | 2.90 |
| Shadows (cached virtual pages) | 0.40 | 0.09 |
| Indirect light (radiance cache, occlusion) | 0.60 | 1.52 |
| TAA + post + tonemap | 0.40 | 1.16 |
| **Frame** | **4.17** | **6.89** |

**Bistro against 144 FPS** (6.94 ms, by §10.1's measure, the 1%-low):

| View | Mean (ms) | 1%-low (ms) | 1%-low FPS | Over 6.94 ms |
|---|---|---|---|---|
| `street` | 6.89 | 6.93 | 144 | inside |
| `street_walk` | 6.97 | 7.08 | 141 | 0.14 |
| `hedge` | 7.10 | 7.18 | 139 | 0.24 |
| `terrace` | 7.47 | 7.55 | 132 | 0.61 |
| `street_turn` | 7.17 | 7.69 | 130 | 0.75 |

**A scene that is a city.** Bistro is one street. NVIDIA's Emerald Square (Open
Research Content Archive, v4.1; Nicholas Hull, Kate Anderson, Nir Benty, 2017)
is four city blocks built for graphics research and
[designed to tile into a city of any size](https://developer.nvidia.com/blog/professional-quality-3d-asset-library-for-research-launched-by-nvidia-amazon-lumberyard-and-speedtree/).
The download holds 1,030 placed meshes of 159, 10,046,405 triangles of which
2,695,054 are unique, on a 76.8 m block pitch, 230 m square with the end caps
that close its edge, a tower of 108 m among buildings of 16 to 47 m; a park of
grass, hedges and trees; day and dusk versions, the dusk lit by emissive
surfaces; a camera path at 1.5 m above the street; and 351 DDS textures for a
metal-rough GGX material. Its licence is CC BY-NC-SA 3.0, non-commercial and
share-alike.

**What ADR-0041 argued.** It rejected gating the frame only on the game in
Phase 15, because "no phase after 15 absorbs a miss", and because structure is
cheapest to change before the later phases build on it. The user's plan
answers the first: the game's development comes after Phase 15 and absorbs it.
The second was answered at the checkpoint, where the frame kept its passes
(ADR-0059, decision 1).

## Decision

1. **The first game** (§13) is a first-person shooter with roleplay elements in
   a big city, as above, built after Phase 15. ADR-0059's forest valley no
   longer stands. Whether it has vehicles is open: they were not named, so
   nothing here assumes them, and driving would add vehicle physics and
   streaming at car speed against the 6 m/s the views test today.
2. **The reference scenes against the game:**

   | Scene | Stands for | Gates |
   |---|---|---|
   | Bistro | A street at the eye's height | Yes |
   | The city: Emerald Square, tiled | Many blocks, streets and buildings; the skyline; streaming | Yes |
   | The interior: Intel's Sponza and its 10,000 candles | Indoor facilities' many lights | Yes |
   | The landscape | Nothing in the game: a stress test of geometry | No |

   The landscape stays measured (nightly, until the amendment below), is still
   optimized in Phase 14 (the user's words: it "definitely needs to be
   optimized"), and is held by the
   guard rail, no slower at a phase's end than at its start. It does not hold up
   an exit. Sponza's interior tests the lighting of a facility, not its size:
   the one search for a free large facility found none, and the game's own
   content can add one.
3. **Emerald Square is used only to test the engine** (the user's decision on
   its licence). Like every reference scene (ADR-0040), it stays outside the
   repository and its artifacts, pinned by a manifest, and is never shipped or
   redistributed. The city's size is set when it is first measured, at least 2
   by 2 km, and its tile is the four blocks' 153.6 m square without the end caps.
4. **The gates.** §10.1's definition of native stands, and so does ADR-0041's
   rule that no quality is given up to meet a number: no upscaling, no dynamic
   resolution, no frame generation, no half-resolution passes.
   - **Phase 14's exit**, the renderer's last phase: Bistro, the city and the
     interior each hold 144 FPS, 1%-low ≤ 6.94 ms at native 4K on the RTX 3060
     Ti, with the phase's features on, each §10.4 line reported against its
     budget.
   - **Phase 15's exit:** the same three scenes hold it in shipping builds.
   - **The first game's development, after Phase 15:** the game holds §10.1's
     target, 1%-low ≤ 4.17 ms (240 FPS) at native 4K on the reference hardware,
     inside the §10.4 and §10.5 budgets. This is ADR-0041's gate, moved.
5. **The target and the table stand.** 240 FPS at native 4K remains the engine's
   target and §10.4 its plan. No line is raised, and renderer pull requests
   still state their cost in milliseconds and DRAM bytes. ADR-0041's guard rail
   stands, extended to Phase 15.
6. **Phase 14 starts with the city.** The order: ADR-0066's walk, which wins back
   what Phase 13 cost Bistro; the city scene and its measurements; the world
   partition's cells and streams in the engine's runtime, which only the samples
   have today; the landscape's raster; then the interior, the many-light
   sampling, the local lights' and contact shadows, reflections and tiers, each
   inside the gate.
7. **No new phase for characters.** Skeletal animation and compute skinning (the
   cooker drops skins and animations today, and no phase has built them),
   navigation, NPC AI and crowds are built as the game needs them, after Phase
   15, each as tested engine work with its own ADR, with a crowd reference scene
   when there are characters to draw.

## Alternatives considered

- **Keep the 240 FPS gate at Phase 14's exit**, the plan until now, with Bistro
  2.76 to 3.52 ms from it and two lines ADR-0059 could not meet at full quality.
  The user chose to reach it on the game, against the game's own requirements.
- **144 FPS as an average.** §10.1 measures the 1%-low so that stutter counts. By
  the average, Bistro's turn is 0.23 ms from 6.94 ms; by the 1%-low, 0.75 ms.
- **Gate the landscape at 144 FPS too.** It would need its 1%-low cut 8 to 10
  times, by a research problem (needles that do not simplify, ADR-0060) the game
  does not have. Reported and optimized, not gated.
- **A characters and crowds phase before hardening**, with a crowd scene gating
  (recommended to the user). Not chosen: the focus now is the world, and the game
  starts after Phase 15.
- **Bistro tiled as the city.** No licence question and no new content, but one
  street that was not made to tile, and no grid of streets.
- **A procedural city.** ADR-0040's reason stands: no authored materials,
  textures, or depth complexity.

## Consequences

- Bistro's distance to its gate falls from 2.76 to 3.52 ms to at most 0.75 ms,
  but Phase 14's features add to the frame and must fit in it: the 144 FPS gate
  is not met by today's frame, it is met with reflections, many-light sampling
  and the local lights' shadows on.
- Reaching 240 FPS stays unproven. The lines ADR-0059 could not meet, TAA + post
  + tonemap and the indirect light, and the shading's, are carried to the game's
  development, with a frame that by then holds more features.
- The gates hold no characters. A crowd's skinning has no §10.4 line, and moving
  characters will redraw shadow pages that today's static scenes keep cached (the
  street's shadows take 0.09 ms because nothing moves). 144 FPS in these scenes
  does not predict the game's frame rate with its NPCs.
- The voxel far field was built for trees and is unproven on buildings, whose
  flat walls and straight skylines are not what voxels hold best. It stays for
  the landscape; the city measures it, and if it does not hold, a triangle-based
  proxy gets its own ADR. Levels of detail, cells, streaming and virtual
  texturing carry over, and buildings should simplify far better than needles.
- ADR-0066's decision 2 stands as Phase 14's first task, measured on Bistro and
  the landscape as it says. Its consequence, every reference scene inside its
  §10.4 lines at Phase 14's end, is replaced by the gates above.
- Emerald Square is FBX and the cooker reads glTF, so its manifest needs a
  conversion, which its own pull request records.

## Amendment (2026-10-31): the landscape leaves the nightly

**Context.** The nightly ran the landscape as its manifest then cooked it, without
its cells, at the default lists, where every view overflows and drops work: 15.2 to
16.6 ms on a5872ff, whose forest with its cells, its streams and its lists takes
61.7 ms. It had no budget rows, since a budget on dropped work holds nothing, so it
could fail only by crashing. Since Phase 13 landed, the one fault it found was the
nightly's own: the device lost when the city's lists sized its renderer (90c106d).

**Decision** (the user's). The nightly leaves it out, and fetches, cooks and measures
every other manifest's scene. The guard rail above stands, measured by hand at a
phase's start and end as ADR-0066's table was (docs/benchmarks.md has the commands).
With no nightly cook to pay for them, the manifest now asks for what that frame
needs: its cells (`cook.hlod`) and its lists (`lists`: 16), so that a run that drops
work fails.

**Alternatives considered.**

- *The full frame in the nightly*, which ADR-0062 expected once the cells streamed:
  about 50 minutes more whenever the cooker's output changes, and some 4 minutes a
  night, for a scene that does not gate.
- *The run as it was, for a smoke test:* it would say that the landscape still cooks
  without its cells, loads, and draws what its lists hold, and go on printing numbers
  that are not its frame.

**Consequences.**

- Between a phase's start and end nothing automatic says that the landscape got
  slower or stopped cooking or loading. What it stresses keeps its tests and synthetic
  scenes on every renderer pull request (`test_voxel.c`, `test_vis_voxel.c`,
  `test_vis_cells.c`, `test_proxy_stream.c`, `foliage_*` and `lod_*`).
- The commands checked on a5872ff with the cells of an earlier cook, the forest view,
  the editor not minimized: 61.65 ms mean, 62.88 ms at the 99th percentile, 6.24 GB,
  no list overflowed (ADR-0066: 61.84, 62.33, 6.23 GB).
- The nightly is about a minute shorter, and about 6 minutes more so when the cooker's
  output changes.
