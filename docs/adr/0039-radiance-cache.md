# 0039. A hashed world-space radiance cache, kept by a fixed ray budget

- **Date:** 2026-10-19
- **Status:** Accepted (with the cost against §10.4's indirect-light line
  and a patent to review before release: see Consequences)

## Context

CLAUDE.md §8.6 stage 7 asks for "a world-space radiance cache updated
incrementally within a fixed per-frame ray budget", sampled per pixel at
full resolution, with "the final GI technique chosen via an ADR after
research and benchmarks" (§13). Until now the diffuse indirect light was
the environment's irradiance (its SH9 projection) everywhere: the sky lit
the inside of a box as brightly as an open field, nothing occluded it but
the screen-space ambient occlusion within half a metre (ADR-0038), and no
light bounced.

The candidates studied:

- **Probe grids** (Majercik et al., "Dynamic Diffuse Global Illumination
  with Ray-Traced Irradiance Fields", JCGT 2019): irradiance and visibility
  octahedra in a regular grid, rays from every probe each frame. Leaks
  through walls thinner than the grid's spacing unless probes are placed
  or relocated per scene; cascades for range.
- **Surface caches** (Wright et al., "Lumen: Real-time Global Illumination
  in Unreal Engine 5", SIGGRAPH 2022 course): mesh cards captured and lit
  in their own atlas, traced through signed distance fields. A large
  system with an authoring step per mesh.
- **Screen-space probes plus a world cache** (Boissé et al., "GI-1.0: A
  Fast and Scalable Two-Level Radiance Caching Scheme for Real-Time
  Global Illumination", AMD 2023): a hash grid in the world behind
  screen probes.
- **A hashed cache of path-space vertices** (Binder, Fricke & Keller,
  "Fast Path Space Filtering by Jittered Spatial Hashing", SIGGRAPH 2018
  talks, and "Massively Parallel Path Space Filtering", MCQMC 2020;
  Gautron, "Real-Time Ray-Traced Ambient Occlusion of Complex Scenes using
  Spatial Hashing", SIGGRAPH 2020 talks; NVIDIA's SHaRC, its product
  form): cells of the world hashed into a fixed table, sized by their
  distance from the camera, filled by whatever rays the frame can afford,
  no placement, no authoring.

The user chose the hash grid (2026-10-19) after being told that SHaRC's
code is under NVIDIA's RTX SDK licence, which forbids its use in open
source, and that NVIDIA holds a patent on hashed path-space filtering
(see Consequences). During the research SHaRC's repository (its
documentation and its shader headers) was downloaded and summarised to
compare designs. This implementation is our own code, written from the
papers above, and copies none of SHaRC's; whether that exposure matters
belongs to the legal review below.

The production path has a visibility buffer (each pixel's triangle), a
top-level acceleration structure where the device has ray queries
(ADR-0032), the ambient occlusion of the near field (ADR-0038), and the
temporal resolve that averages per-pixel jitter (ADR-0034). §10.4 gives
indirect light 0.60 ms, of which the ambient occlusion takes 0.38.
Measurements are on the RTX 3060 Ti, Profile build, at 3840 × 2160 in
`render_perf`'s field; images are `test_gi.c`'s 128 × 128 views against
ground truths the test computes itself by casting 576 stratified rays a
pixel at the scene's boxes.

## Decision

1. **API.** `NphRenderView.gi` asks for global illumination: on the
   production path, where the device has ray queries, the diffuse indirect
   light takes its irradiance from the cache instead of from the
   environment's SH alone. It is made for lit views and the indirect and
   irradiance debug views (frame statistic `gi`); the reference renderer
   has none. `NphRendererDesc.gi_cells` (2^18 slots, a power of two of at
   least 16; 36 bytes each: 9 MB) and `gi_rays` (65,536 a frame) size it,
   and `disable_gi` leaves it out. `NPH_RENDER_VIEW_IRRADIANCE` shows the
   irradiance the diffuse light takes, over π. The runtime's `r.gi` (on)
   sets it.
2. **Cells** (`gi_common.glsl`). A point's level is floor(log2 d) + 8
   for its distance d from the camera (32 levels), and its cell a cube of
   2^(level − 8) / 48 metres: 1/96 to 1/48 of the distance, 23 to 47
   pixels across at 4K with a 0.9 rad field of view, so the cells a frame
   sees number about the same however large the world (Binder et al.'s
   level of detail by distance). A cell holds the surfaces in it whose
   geometric normals face one octant (their components' signs), so the
   two sides of a wall are cells apart. The point is taken half a cell
   out along the normal before it is quantised, so that a surface on a
   cell's face (a floor at y = 0, which the cells' powers of two align
   with) does not fall either side of it as rounding goes. Each level's
   camera cell is worked out on the CPU in double precision and the
   shaders add the camera-relative point to it, so cells stay put in the
   world at any distance from the origin (CLAUDE.md §8.2).
3. **The table.** A key packs the cell's coordinates (their low 17 bits),
   the level, and the octant into 64 bits; MurmurHash3's 64-bit
   finaliser (Appleby) mixes it into a bucket of 16 slots and a 32-bit
   fingerprint (never 0, a free slot's mark), so 32-bit atomics claim
   slots on every device. A lookup reads the bucket's 16 fingerprints
   (64 bytes); a claim scans for its own and else takes the first free
   slot by compare-and-swap in order, so that invocations claiming the
   same new cell meet in one slot. A full bucket drops the sample.
4. **The update** (`gi_update.comp`), within `gi_rays`: one invocation
   for each block of stride × stride pixels (the smallest square that keeps
   the samples within the budget: 12 at 4K, 57,600 rays) takes a random
   pixel of it, rebuilds the surface point exactly as the resolve does
   (the triangle's perspective-correct barycentrics), claims its cell, and
   traces one cosine-distributed ray (Malley's method), whose radiance
   times π estimates the irradiance. A miss takes the environment (a
   level of its cube 16 texels across a face, which changes the estimate
   little and its noise a lot). A hit takes its emission plus its albedo
   over π times the irradiance there: the sun's (the live directional
   light in the lowest slot, with a shadow ray where it casts shadows)
   and the cache's own at the hit's cell, the environment's where the
   cache has none, so each frame adds a bounce. Textures at a hit are
   read at the level the hit's cell resolves (the cell as a ray cone's
   footprint: Akenine-Möller et al., "Texture Level of Detail Strategies
   for Real-Time Ray Tracing", *Ray Tracing Gems*, 2019). A ray that hits
   within 0.95 of the ambient occlusion's radius counts nothing: that range
   is the occlusion's, so nothing is occluded twice.
5. **Accumulation.** Samples are pre-exposed, clamped to 4,096, and added
   in fixed point (2,048 units each) by 32-bit atomics; `gi_resolve.comp`
   then visits every slot: a cell with samples blends their mean into its
   irradiance (kept in lux, whatever the exposure does), weighted by their
   count against its history, at most 128 samples, so older light fades
   as new arrives; a cell without samples for 240 frames gives its slot
   back.
6. **The lookup.** The production path's resolve is specialised on the
   cache (`k_gi`, as on the sun's virtual shadow map), so frames without
   it pay nothing for it. It looks the pixel's cell up as soon as it has
   the pixel's point, before the triangle's attributes, and the shading
   takes the cell's irradiance where the environment's would be, the
   environment's making up for what a cell does not trust yet (fewer than
   16 samples). Under temporal anti-aliasing the lookup is at a random
   point of the cell in the surface's plane, so the cells' edges blend
   over frames in the temporal resolve (Binder et al.'s jittered hashing).
   The irradiance debug view is a branch of its own in the resolve's
   `main()`.

## Measurements

**The images** (`test_gi.c`; NVIDIA and lavapipe alike):

| Case | Result |
|---|---|
| an open ground under a uniform sky: the cache against the environment's irradiance | within 0 levels (of 255) |
| a black box on a black ground under a uniform sky: where the truth is occluded (3,913 pixels), the error as a share of the occlusion | 0.103 (bias +0.003) |
| a white wall in the sun over a black ground, no sky: the light it reflects onto the ground (6,594 pixels) | error 0.146 of it (bias −0.026) |
| the cache's keys after a frame against their CPU twins, every pixel's cell | 6,526 of 6,526 |

The update first rebuilt its points from depth through the inverse of the
reverse-Z infinite projection: off by up to centimetres, they keyed half
the cells differently from the resolve and sat below their surfaces,
which then occluded themselves (the open ground came out up to 46 levels
of 255 dark).
Rebuilt from the triangle as the resolve does, the two agree to the bit.

**The cost** at 4K (`render_perf-2026-10-19d`; ms; single runs vary by a
few hundredths; the rows without a `_no_gi` twin compare with
`render_perf-2026-10-19c`, run before the cache existed):

| Scene | Without | With | Added | Of which the resolve's lookup, the update, the slots' resolve |
|---|---|---|---|---|
| the field under the sky (`field_4k_production` against `field_4k_no_gi`) | 3.33 | 3.60 | **+0.27** | 0.24, 0.032, 0.011 |
| the field with the sun (`shadows_4k_production` against `shadows_4k_no_gi`) | 3.68 | 3.88 | **+0.20** | 0.15, 0.035, 0.011 |
| the field at night among 1,024 lamps (`lights_1k_production`) | 3.25 | 3.60 | **+0.36** | 0.30, 0.031, 0.011 |
| the same among 4,000 lamps (`lights_4k_production`) | 4.92 | 5.33 | **+0.41** | 0.36, 0.031, 0.011 |
| the sun, a sphere moving every frame (`shadows_moving_4k_production`) | 3.71 | 4.24 | **+0.54** | 0.15, 0.034, 0.011, and the top level's rebuild with its records, 0.36 |

The lamps' resolve walks its light grid with the cache's variant holding
more registers, and a frame in which an instance moves rebuilds the top
level wherever rays are traced: the sun's virtual shadow map traces none,
so before the cache that frame rebuilt nothing.

**What shaped the lookup** (the field's resolve, single runs of 480
frames; the resolve without the cache takes 0.99 ms):

| Step | The resolve |
|---|---|
| one shader, the lookup behind a branch on the view | 1.41 with it, 1.25 without (48 bytes of spills) |
| specialised on the cache; the irradiance debug view a case of the shading's debug views | 1.24 with it, 1.05 without |
| that debug view a branch of its own in `main()` | **1.23** with it, **0.99** without |
| the lookup in the shading where it takes the environment's irradiance | 1.31 |
| the lookup after the triangle's attributes | 1.33 (16 bytes of spills) |
| the cell carried in shared memory instead of registers | 1.23 |
| 32-bit keys hashed by pcg3d (Jarzynski & Olano 2020), a reciprocal instead of a division | 1.24 |

The lookup's parts, by removing them in turn: the random point in the
cell 0.05 ms (its arithmetic is cheap; pixels of a warp then read
different cells), the key and its hash about 0.07, the bucket's and the
cell's reads about 0.06. The resolve runs at its register limit (64);
holding even one value from `main()` into the shading measured 0.13 ms.

## Alternatives considered

- **Probe grids (DDGI):** leaks at thin walls without per-scene probe
  placement, and a grid that scales with the world, against §2's "cost
  scales with pixels".
- **A surface cache (Lumen-like):** an authoring step per mesh and a
  second scene representation; far more machinery for the same diffuse
  term.
- **Screen-space probes in front of the hash grid (GI-1.0's first
  level):** better detail at contacts, a second cache's cost; the
  ambient occlusion covers the near field instead.
- **The lookup in the shading, or the cell carried another way:** measured
  above.
- **Points from depth in the update:** measured above.
- **SDF or software tracing on devices without ray queries** (§8.6 stage
  7 names it): not built; those devices keep the environment's light and
  the ambient occlusion.

## Consequences

- **The cost is reported:** global illumination adds 0.20 to 0.27 ms to a
  4K frame lit by the sky or the sun, and 0.36 to 0.41 ms at night among
  many lamps. With the ambient occlusion's 0.38 ms the sky-lit field's
  indirect light takes 0.65 ms against §10.4's 0.60 ms line, 0.05 ms past
  it (the sun's field 0.58 ms; the night scenes have no occlusion). A
  frame in which anything moves also rebuilds the top level, 0.36 ms over
  the field's slots: refitting it instead, which ADR-0032 set aside while
  a still scene's shadows built nothing at all, is the next lever now
  that the cache's rays keep it in use every frame. `r.gi 0` turns the cache off, and frames without it pay
  nothing.
- **The patent, for the user's legal review before any release:** US
  10,614,613 B2, "Reducing noise during rendering by performing parallel
  path space filtering utilizing hashing" (NVIDIA; Binder, Fricke &
  Keller; filed 2018-07-25, priority 2017-07-28, granted 2020-04-07,
  anticipated expiry 2038-07-25). Its claim 1 selects a vertex of a light
  transport path, quantises its features, hashes them, resolves
  collisions in a hash table, accumulates the path's contribution there
  with a counter, and averages. The cache does those steps for each
  update ray's surface point. The user chose to proceed (2026-10-19); this
  must be reviewed before a release, together with SHaRC's licence given
  that its repository was read during the research (Context), and the
  cache can be left out of a renderer (`disable_gi`) if it has to be.
- `render_perf`'s production scenes take the runtime's default (on), and
  `field_4k_no_gi` and `shadows_4k_no_gi` show what it costs; every scene
  is re-baselined (`render_perf-2026-10-19d`).
- Point and spot lights do not bounce yet (only the sun, the environment,
  and emission); blended surfaces and the reference renderer take the
  environment's irradiance; normal maps do not modulate the cache's
  irradiance (a cell's is one value); specular indirect light is still the
  environment's (stage 10's reflections).
- A changed scene shows in the cache over its history window: about 128
  samples a cell, a few dozen frames at 4K.
- The resolve has four more pipelines (each existing one with the cache):
  about 0.35 s more to create a renderer on lavapipe.
- New: `NphRenderView.gi`, `NphRendererDesc.disable_gi`, `gi_cells`, and
  `gi_rays`, `NphRenderFrameStats.gi`, `NPH_RENDER_VIEW_IRRADIANCE`, the
  `r.gi` cvar, and the passes `gi clear`, `gi update`, and `gi resolve`.
  The meshes' GPU records gain their index buffer's address and each
  submesh's first index, which the update's hits decode triangles from.
- Open: SDF or software tracing without ray queries; local lights'
  bounce; the update on the async compute queue; the patent review.
