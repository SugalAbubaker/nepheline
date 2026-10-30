# 0068. The city reference scene: Emerald Square, converted and tiled

- **Date:** 2026-10-30
- **Status:** Accepted

## Context

[ADR-0067](0067-the-game-is-a-city.md) makes a tiled NVIDIA Emerald Square the
reference scene for the game's big city. The download is a 617 MB archive:
`EmeraldSquare_Day.fbx` and `EmeraldSquare_Dusk.fbx` (FBX 7300, binary, Y up, in
metres), 351 DDS textures, two HDR environment maps, and Falcor scene files. The
cooker reads glTF. What the file holds:

- 1,030 placed meshes of 159, all triangles, 10,046,405 in all and 2,695,054
  unique, with normals per corner, two UV sets, and materials per polygon or for
  the whole mesh.
- Five groups: four blocks (`Kwow`, `Sheleg`, `ToyHotel`, `Park`) and `EndCap`,
  which closes the city's edge with a row of buildings. The park alone holds 6.6
  million triangles, 1.6 million of them one mesh of grass.
- 220 materials naming 115 sets of textures: BaseColor (alpha is opacity),
  Specular (by the README, occlusion, roughness, metalness), Normal (DirectX), and
  for six of them Emissive.
- A sun, a faint fill light, and a camera path of 64 seconds at 1 to 3 m.

## Decision

1. **`scripts/city.py` converts it**, with Python's standard library, and is the
   manifest's generator. `scripts/scenes.py fetch` learns a third kind of source,
   an archive pinned by URL and md5, and extracts it once. The same file and
   manifest write the same bytes. It reads binary FBX 7300 and refuses what the
   pinned file does not hold (`scripts.city` writes an FBX byte by byte and checks
   the document). Instancing is kept as the file has it: 151 meshes (a geometry
   per set of materials) and 883 nodes, with the textures referenced where they
   are through `MSFT_texture_dds`.
2. **What it makes of the materials, each checked against the data:**
   - **No occlusion.** The Specular textures' red channel is zero in all 115, so
     glTF's occlusion would read no ambient light at all: the first cook showed
     every shadow pure black. Their green and blue are glTF's roughness and
     metalness as they stand.
   - **A base colour's alpha is a mask where BC3 blocks cross 0.5.** That is 17
     materials, the foliage's. The rest are opaque: the small constant textures of
     the file's lamps, panes and paint hold an alpha of 0 that no surface uses as a
     cutout, and `Hedge_Base` holds it across 99% of its blocks.
   - **Two-sided where the name says so:** the file's Falcor materials carry
     `.DoubleSided`. The 220 materials become 110, the numbered copies naming the
     same textures.
   - **DirectX's normal maps** go through `nph_cook --normal-y-down`, which negates
     the tangents' w and changes no texel.
3. **The tile.** The four blocks are a square of 153.5 m that repeats. The `EndCap`
   group and whatever else is centred outside the square are left out: the road's
   outer strips, which would lie on the next tile's road, and the trees along them.
   That is 883 of 1,030 placed meshes, 8.64 million triangles. The city's edge is
   therefore open, a street cut down its middle.
4. **The world is 16 by 16 tiles, 2.46 km square:** 226,048 instances and 2.2
   billion triangles before levels of detail, on ADR-0067's "at least 2 by 2 km".
   The manifest's `tiles` lays it out at load, sharing meshes, materials and
   textures, so a cook takes 10 minutes, or 28 seconds when only meshes change. Its
   capacities are in the manifest too, since render_perf's per-tile defaults would
   ask for 12.5 million instances.
5. **The light is the landscape's:** the file's sun for its direction and colour
   (48 degrees up) at 30,000 lux, the default sky, and an EV100 of 13. The faint
   fill light and the environment maps are left out, as Falcor's own conventions.
6. **Levels of detail alone: no voxels and no cells.** Measured below. Its far
   field is its meshes' triangle levels. The cooker's cell proxies **lose the far
   towers**, which is a loss of quality and is declined.
7. **The manifest's `lists` says what its views need (4), and overflowing them
   fails the run.** The rooftop at the defaults dropped work and read 7.99 ms
   against its true 10.3 ms. A number that flatters a gate is worse than none. The
   landscape says nothing and keeps its behaviour (ADR-0066).
8. **Views at the game's eye, 1.7 m:** a street, still, walking, sprinting and
   turning; a crossing's long sightline; the park's foliage. And one from 112 m over
   the rooftops, the most a person can see of the city at once.

## Measurements

The RTX 3060 Ti at native 4K, Profile, 240 frames, the editor's windows minimized
(`reference_scenes-2026-10-30c`). 144 FPS is a 1%-low of 6.94 ms.

| View | Mean (ms) | 1%-low (ms) | 1%-low FPS | Over 6.94 ms | Triangles |
|---|---|---|---|---|---|
| `street` | 6.54 | 6.64 | 151 | inside | 1.6 M |
| `street_walk` | 6.56 | 6.76 | 148 | inside | 1.6 M |
| `crossing` | 6.63 | 6.76 | 148 | inside | 3.7 M |
| `street_sprint` | 6.71 | 6.95 | 144 | 0.01 | 1.4 M |
| `street_turn` | 6.55 | 7.49 | 133 | 0.55 | 0.7 M |
| `park` | 7.77 | 7.89 | 127 | 0.95 | 4.0 M |
| `rooftop` | 10.43 | 10.55 | 95 | 3.61 | 22.9 M |

Against §10.4's lines, the street: culling 0.79 ms (budget 0.25), raster 1.00
(0.65), shading 2.02 (1.20), shadows 0.08 (0.40), indirect light 1.50 (0.60), TAA
and post 1.15 (0.40). It holds 2.94 GB of device-local memory with every texture
whole, and 4.1 GB in `scene_viewer`, whose lists are four times larger.

**The cooker's options** (the means in ms, lists 2 unless the rooftop overflowed;
the runs' spread is about 0.05 ms):

| Cook | `street` | `crossing` | `park` | `rooftop` | Rooftop's triangles | Device-local |
|---|---|---|---|---|---|---|
| Levels of detail | 6.41 | 6.58 | 7.68 | 10.35 | 22.9 M | 2.94 GB |
| With voxel levels | 6.50 | 6.62 | 7.74 | 10.98 | 16.4 M | 3.11 GB |
| With cells of 76.75 m and proxies | 6.60 | 6.52 | 7.82 | 9.90 | 9.4 M | 3.11 GB |

Voxel levels make the rooftop slower: its voxel raster takes 1.80 ms for what 6.5
million triangles took less. Proxies win 0.45 ms there and nothing in the street,
for a cook three minutes longer, and past 560 m most of the towers are gone from
the picture, the bases standing. A tall, thin building is what a voxel proxy holds
worst, as ADR-0067 feared. Nothing here needs cells for culling either: the 226,048
instances' cull takes 0.06 ms.

**The rooftop's time** is the far foliage. `vis raster 0 masked` takes 2.16 ms
(0.52 in the street), the software raster 1.26 (0.08), `vis raster 0` 1.10 (0.33),
the walk 0.45 (0.11). A tile's trees and hedges are cards of leaves, which do not
simplify: the forest's problem (ADR-0060) on a smaller scale.

## Alternatives considered

- **Blender.** Its exporter rewrites DDS as PNG and maps the channel-packed Specular
  by its own guess, and a measurement would rest on a tool's version outside the
  pipeline. Community exports exist for the same reasons of convenience.
- **FBX in the cooker.** A parser in the tool that ships, for one scene's format, and
  an inflate to go with it, where the cooker's input is glTF by design (§8.8).
- **A cooked layout of the world**, blocks shuffled and turned so that it does not
  repeat, end caps round it. Better to walk through, and each cell's proxies would
  be its own. The levels and the instances cost the same, so the numbers above would
  hardly move; it stays open for when the picture matters more than the measurement.
- **The dusk version**, lit by emissive surfaces. It is the same geometry, and it is
  for the many-light sampling to take up.

## Consequences

- `reference_budgets.txt` holds the city's lines; the nightly fetches and cooks it
  like the others, with no new dependency on the runner.
- Phase 14 knows its distance on the city: the streets are at the gate, the turn
  and the park within a millisecond, and the rooftop 3.61 ms away, by the far
  foliage, which the landscape's raster work takes on with it.
- The voxel far field has no use in the city as it stands. It stays for the
  landscape. A far field for buildings, if the rooftop needs one, is triangles first.
- The world repeats, one tower to a tile, and its edge is open: a test scene, not a
  place to spend time.
- The gate's frame holds no moving objects, no characters, and few lights: the sun,
  and emissive surfaces that light nothing yet.
