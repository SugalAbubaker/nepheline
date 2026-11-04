# 0074. Per-pixel reservoirs for the many lights' far fields

- **Date:** 2026-11-02
- **Status:** Accepted

## Context

CLAUDE.md §8.6 stage 10 names ReSTIR (Bitterli et al., "Spatiotemporal Reservoir Resampling for Real-Time Ray Tracing with
Dynamic Direct Lighting", SIGGRAPH 2020) for many-light sampling. ADR-0073 gave the many lights a place in the engine: near
fields per pixel, far fields through the radiance cache. The user's decision on it (2026-11-01): find a way to fit
per-pixel ReSTIR on the reference hardware, not only in a high-end tier.

The cache alone leaves something out. It gathers irradiance, so a far light has no highlight: the specular takes what the
cell gathered as arriving evenly from everywhere, which washes out a floor seen at a grazing angle (the interior's floor
drew 7 % too bright overall, and lost the contrast of its dark inlays) and gives metals a flat, blotchy look.

Published costs were not encouraging: ReSTIR at 3.6 to 3.95 ms at 1080p on an RX 7900 XT (Boksansky & Meister, JCGT 2025),
MegaLights at 5.51 ms at 1080p on a PS5 (Narkowicz & Costa, SIGGRAPH 2025). But a shadow ray a pixel at 4K measures only
0.88 to 3.31 ms here (ADR-0073), a pixel's light rarely changes, and for what does not move, visibility persists.

## Decision

1. **A reservoir a pixel, 8 bytes, carried from frame to frame by reprojection:** its light (18 bits), how many candidates
   it has weighed (8), which way its surface faces (6), its weight W and its depth (half floats). Tiled by 8 x 8, as the
   resolve's workgroups are. Two buffers, the last frame's and this one's. W times what the light delivers at the pixel is the
   mean of the candidates' weights: an estimate of all the lights' light that this one light stands for.
2. **The resolve only reads, shades and carries forward.** It reprojects the pixel, takes the reservoir there if its depth
   agrees within 1.5 %, evaluates that one light, and writes the reservoir to this frame's buffer.
3. **What a reservoir learns it learns on its turn, in a pass of its own (`many_turns.comp`),** the resolve's decode and the
   point's terms over a few tiles a frame, with the ray inline. A tile takes a turn once in 32 frames, or once in 8 while
   its reservoirs have weighed fewer than 16. Turns cycle: candidates, neighbours, candidates, and the reservoir's own light
   looked for again.
4. **A candidate's weight counts only if the candidate is seen.** A turn weighs 4 candidates from the tile lists, or 2
   neighbours' reservoirs, and one chosen by weight stands for all with one ray (resampled importance sampling; Talbot et
   al., EGSR 2005). Not seen, they count as candidates of no weight. A turn that weighs little next to the reservoir's mean
   plays Russian roulette for its ray and, surviving, weighs that much more: the same on average, and a specular lobe's many
   small candidates cost no ray.
5. **Candidates come from a tile's list:** 8 lights a 16-pixel tile, each from the tree at a random point of the tile,
   replaced one a frame (`many_tiles.comp`), and a pixel takes its candidates from a list up to 16 pixels away, so that which
   list does not show the tiles. A pixel's own walk down the tree reads lights nobody else reads: 1.41 ms.
6. **By default they take the far fields' highlights** (`NPH_RENDER_RESTIR_SPECULAR`, `NphRenderView.many_restir`): the lobe's
   single scattering. The diffuse light, and what scatters more than once, stay the cache's, which has no noise.
   `NPH_RENDER_RESTIR_ALL` takes all of it, at more noise (the direct light; the cache keeps their bounces, ADR-0079);
   `NPH_RENDER_RESTIR_OFF` none. A device without ray queries draws as
   with OFF.
7. **They start again** when the set of lights is rebuilt (their indices are the old set's), when the mode changes, and at a
   new size. A surface uncovered starts again by its depth; a reservoir whose own light is no longer seen, by its turn.
8. **The interior's near range is 0.25 m,** from ADR-0073's first guess of 0.5 m. See "What it does".

Every one of these is by measurement against a truth the tests compute by ray casting (`test_many_restir.c`).

## What it does

Measured on the reference machine, native 4K, Profile, 240 frames, the editor minimized (`docs/benchmarks.md`).
The interior's views, frame in ms, mean with the 99th percentile in brackets:

| View | The cache alone, near range 0.5 m (ADR-0073) | The cache alone, 0.25 m | Reservoirs, 0.25 m |
|---|---|---|---|
| `floor` | 8.15 | 6.64 (6.71) | 7.36 (7.43) |
| `floor_walk` | 8.15 | 6.56 (6.64) | 7.34 (7.44) |
| `floor_turn` | 7.10 | 6.08 (6.55) | 6.89 (7.40) |
| `gallery` | 8.53 | 6.77 (6.84) | 7.47 (7.53) |
| `among` | 8.84 | 6.63 (6.69) | 7.38 (7.47) |
| `upward` | 7.45 | 5.82 (5.85) | 6.40 (6.44) |

So reservoirs cost 0.58 to 0.81 ms, and every view is faster with them than it was without them before the near range changed,
by 0.2 to 1.5 ms. The 144 FPS gate (6.94 ms) holds for the cache alone and is still 0.5 to 0.6 ms away with reservoirs.
ADR-0075 onward will have to find it.

On the `floor` view, of the 0.72 ms: the resolve 0.40 (2.32 to 2.72), `many turns` 0.28, `many tiles` 0.03. The turns trace 186,000 to
306,000 rays a frame, about one in 35 pixels. On the 10,000 lights of the synthetic field the frame goes from 3.00 to 3.32 ms.

**Bytes.** 133 MB of device memory for the two reservoir buffers (66 MB each at 4K) and 2 MB for the tile lists. A frame's traffic: the
resolve reads one buffer and writes the other, 133 MB. `many turns` declares both, but touches about one tile in 8 to test
reservoirs of few, and about one in 32 to write: about 10 MB. `render_perf`'s declared bytes count the whole buffers.

**Against the truth,** the mean of many frames against a ray per light per pixel (`test_many_restir.c`, 128 x 128, a wall hiding
half of 64 lights):

| Case | Drew, of the truth | A pixel within, of its own |
|---|---|---|
| All the light, a grey ground | 99.9 % | 28.7 % |
| Highlights, a metal of roughness 0.5 | 99.8 % | 31.7 % |
| Highlights, a metal of roughness 0.9 | 99.9 % | 13.0 % |
| Highlights, with the cache's diffuse | 99.9 % | 3.3 % |
| A moving camera, the depth changing 2 % a frame | 96.3 % | |

The first version of this design drew 137 % in the first row. On the interior's floor, against a run of many frames of the longest
memory and many candidates, the frame's mean is within 1 %, and under 0.5 % of pixels are off by over a factor of 2.

**The near range.** A cell's list holds every light whose sphere touches the cell, and the cell is twice the near range across. At
the atrium's 2.5 lights a cubic metre, at 0.5 m that is 20 lights a pixel near candles walks to find the one in range, with three
dependent loads each; and a lane with a long list keeps its subgroup waiting. Its cost is the cube of the range: at 0.5 m the near
fields took 2.05 ms of the frame, at 0.25 m 0.3 ms. Crops side by side show no difference on walls and arches, and a small change on a
large candle beside the camera. At 0.125 m the frame gains another 0.2 ms for more change on the candles, so it stays at 0.25 m.

**The tests' own strength.** Each of 17 deliberate faults in the estimators fails a test (light uncounted, weights not scaled, the
scattering counted twice, the depth test gone, reprojection gone, the clears gone, and so on).

## How the estimator went wrong

Written down because each of these looked right in pictures, and each showed only against a truth.

- **Light through walls.** The first version looked for a candidate only if it would be chosen, reasoning that visibility
  matters only for what is shaded. But a candidate not chosen still added its weight to the mean, seen or not. With half the
  lights hidden the mean settles near the unshadowed sum, and the one seen light receives all of it: 137 % of the truth on a
  wall scene, while the interior, whose candles are mostly in view, read within 0.3 % of its reference. Hence decision 4.
- **Neighbours that found no light.** Reuse at first skipped neighbours holding no light. They are real samples of zero:
  leaving them out averages the lucky ones. It read 118 to 131 % with four neighbours, and looked like nothing but brightness.
- **A candidate equal to the reservoir's light** was at first skipped uncounted, which darkens a pixel that few lights see. It
  counts, and needs no ray.
- **Counted twice.** The direct lights' specular carries an energy term for what scatters more than once, and the cache's
  indirect light adds that scattering too. A reservoir taking the highlight with its energy term was 40 % too bright on a
  rough metal. It takes the single scattering; the two add up to the energy term. (`spec_base + spec_grazing` is that term,
  so it needs nothing kept live.)
- **Half floats overflow.** W can pass 65,504: stored unguarded it became infinity, and neighbours spread it (+158 %).
- **A light flush with a wall** is seen or not by a rounding: the test's truth and the rays disagreed about one light in 64,
  which read as +2 % that no change of estimator moved. Testing each light alone found it.

## Alternatives

Frames on the interior's `floor` at 0.25 m unless it says otherwise; the error is a pixel's, against a long run.

- **The turns inside the resolve, with their rays in a pass over a list** (the design until today): 7.77 ms against 7.49 at the same
  rate. The resolve's registers spill (0 to 4,352 bytes of shared memory), and it needs a 16 MB list, an atomic a candidate, and a
  reservoir written twice, as if seen and as if not.
- **Rays in the resolve itself:** the ray-query variant takes 80 registers against 64: +0.94 ms before a single ray.
- **Turns by the pixel rather than the tile:** +0.25 ms at the same rate, and they stop following the rate at all: a lane at work costs
  its subgroup's every lane the work's time, so one pixel in 16 costs nearly what one in 4 does.
- **A turn every 16 frames:** +0.21 ms, for the same error at rest. Lights that come into view would arrive in half the time.
- **A reservoir of 40:** a pixel off by 74 % on the field of spheres, against 58 % at 255.
- **4 or 8 neighbours a turn:** 53 to 55 % on that field against 56 % at 2, and darker (95.6, 95.1 % of the truth against 97.4 %), for 0.03
  to 0.06 ms. Neighbours help little on a curved metal, whose highlight changes fast from pixel to pixel.
- **Each pixel its own walk down the tree for its candidates:** +1.41 ms for the reads of lights no one else reads. The lists make
  a tile's reads the same few.
- **The textbook's visibility reuse** (candidates unseen, the reservoir emptied if its choice turns out hidden): the same mean, but
  all of a reservoir's memory lost on a failed ray: heavy-tailed grain. A candidate that is not seen here costs only its own weight.
- **Testing every candidate** without roulette: 1,267,000 rays against 905,000 at the rates then, on the same image.
- **A denoiser, or a filter over the weights:** not built. What the temporal and spatial reuse leaves is under what the eye sees on
  the interior; on metals it may be needed, but that is another pass for a frame that has no room.
- **Reservoirs for all the far light by default:** visibly grainy on the interior's columns where the cache is smooth, at no saving.

## Consequences

- **Memory is the price of little noise.** Noise falls with the root of a reservoir's count, and a light that comes into
  view takes that many candidates to count in full: where an occluder has moved away, or a door opened, the far highlights
  of what it hid arrive over many seconds (255 candidates, 4 every 64 frames: about 17 s at 240 FPS). The cache's diffuse
  light takes about a second. A light that goes out of view shows sooner: a reservoir whose own light is gone starts again
  within 128 frames, though the others keep its share of the mean as long as the lag above.
- **A pixel's colour is one light's at a time,** its weight is the mean's. Lights of one colour do not show this (the
  interior's candles). Among many colours it is a grain of hue that anti-aliasing cannot average, since a reservoir changes
  its light seldom. A city at night will want the cell's colour mixed in where no one light dominates; the first game's
  scenes will say.
- **Grain on metal among lights nearby.** The field of metal spheres (`many_lights_10k_production`) keeps a visible grain:
  a highlight's value changes fast from pixel to pixel on a curved surface, so a neighbour's reservoir transfers poorly.
  It is brighter by the truth than what it replaces, and noisier. Reflections' rays may change the balance.
- **A surface just uncovered** has no far highlight for up to 8 frames, and a speckle until its reservoirs fill. A 90-degree-a-
  second turn shows it over a band at the leading edge of the frame.
- **Neighbours darken a little where they do not agree** on what they see: a reused reservoir counts lights the pixel sees and
  its neighbour does not as nothing. 2.6 % on the field of spheres, under 1 % on the interior.
- **Patents.** NVIDIA holds patents in this area: US12299801B2 (grid-based light sampling), US11948246B2 and US12646247
  (spatiotemporal resampling with decoupled shading and reuse). This record is not legal advice: they need review before
  anything ships.
