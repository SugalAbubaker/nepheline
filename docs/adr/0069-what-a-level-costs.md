# 0069. Levels of detail: what a collapse costs, what a level may spend, and corners that collapse

- **Date:** 2026-10-31
- **Status:** Accepted

## Context

[ADR-0068](0068-city-reference-scene.md) measured the city at 1%-lows of 6.59 to 10.49 ms
against [ADR-0067](0067-the-game-is-a-city.md)'s 6.94, with raster the largest thing the
rooftop could lose (4.94 ms, 22.9 million triangles). A tile holds 8.64 million triangles at
level 0, and levels of detail were doing less than they should. Two things were plain:

- **Corners never moved.** A vertex where more than two seams or open edges meet could not
  collapse, and those are most of a flat-shaded building's vertices (69% of
  `Building_Generic`'s), so such meshes stalled within a level or two: a tile's coarsest level
  held 1.14 million triangles.
- **Levels were spent unevenly.** A tile's triangles at each error (below) showed
  `Japanese_Maple` holding half of everything, at 15,000 of its 18,867 triangles out to 64 cm.
  Its 13,245 triangles of bark simplify cheaply, but its groups also hold its big leaf cards. A
  group must halve, so it took a card's collapse at a metre of error, and that error is the
  whole group's: its bark stayed at the finest level for a kilometre.

## How it was measured

Frame times alone misled at every step here, so three instruments were built, and each caught
what the others could not:

1. **A tile's triangles at each error,** from a verbose cook of a geometry-only copy of the
   document (each mesh once, no images): exact, free of noise, three minutes a variant.
2. **Frame times,** quiet and in turn over rounds.
3. **Pictures against level 0,** the same view drawn with `lod_error` 0, in the lit and normal
   views: the share of pixels off by more than 32 of 255, and crops looked at by eye. Moving
   views do not compare; still ones do, to the bit.

## Decision

1. **Corners collapse by adding vertices,** where the input gives texture coordinates. A corner
   collapses along any edge; a surface the edge's triangles do not reach has no vertex at the
   neighbour, so it takes one added there, with its own attributes (its normal, and so the hard
   edge) and its texture coordinates carried along its largest triangle's plane. The asset holds
   them after the mesh's own, and the clusters name them as the mesh's vertices continuing.
2. **A collapse costs the borders it moves,** adding up. A surface's quadrics do not see a border
   between two surfaces that look different move where they lie in one plane (a window in a
   wall: a corner that goes turns its rectangle into a triangle), and the measured distances do
   not either, since a point's nearest surface may be the other one. So each seam and open edge
   of the class that goes counts how far that class stood from where the edge now runs, and those
   steps add up per class, since a corner can walk along its edge in steps that each cost little.
3. **A collapse costs what the surface then shows.** Positions alone take wrinkled cloth for a
   flat sheet. With the input's normals, a collapse also costs the change between the vertex's own
   normal and the normal its nearest staying triangle interpolates where it stood, times that
   triangle's longest edge and a quarter (after Hoppe, "New Quadric Metric for Simplifying Meshes
   with Appearance Attributes", IEEE Visualization 1999, whose attributes count in the error too).
   A normal that varies evenly comes back from the interpolation, so a true curve pays little
   (a torus's error at level 2: 0.0071 without normals, 0.0079 with, and the same triangles) and a
   wrinkle its full size.
4. **A level has an error of its own.** Its collapses go no further than twice the error of the
   median collapse its groups could make as they stand, and no less far than the level before's.
   A group this stops keeps more than half its triangles, and a later level, whose error is
   larger, goes on with them. The costs are read group by group, at once where there are jobs.
5. **A mesh that may voxelize keeps the simplifier as it was.** A group's collapses stalling, and
   their error against the voxels', is what makes it voxels (ADR-0061), so none of this applies to
   it. The landscape's pack is the same to the byte. It is measured against the cluster lists'
   cap in every view, so nothing can be judged there until its raster is taken up (ADR-0067), and
   this is to be taken up with it.

## What it does

Quiet, the median of three rounds, before to after, with the same binary.

**The city:**

| View | Mean (ms) | 1%-low (ms) | Triangles |
|---|---|---|---|
| street | 6.48 -> 6.43 | 6.59 -> 6.48 | 1.61M -> 1.43M (-11%) |
| walk | 6.55 -> 6.49 | 6.72 -> 6.66 | 1.61M -> 1.41M (-12%) |
| sprint | 6.70 -> 6.64 | 6.91 -> 6.85 | 0.83M -> 0.78M (-6%) |
| turn | 6.56 -> 6.45 | 7.39 -> 7.22 | 0.86M -> 0.75M (-13%) |
| crossing | 6.63 -> 6.51 | 6.76 -> 6.66 | 3.70M -> 3.02M (-18%) |
| park | 7.75 -> 7.66 | 7.85 -> 7.74 | 4.03M -> 3.93M (-3%) |
| rooftop | 10.40 -> 10.15 | 10.49 -> 10.24 | 22.89M -> 20.03M (-13%) |

**Bistro,** whose frame moves less than a round's spread (0.06 to 0.10 ms) in every view, and
whose images come far nearer level 0, the share of pixels off by more than 32 of 255:

| View | Off, lit | Off, normals | Mean (ms) | Triangles |
|---|---|---|---|---|
| street | 0.25% -> 0.04% | 0.31% -> 0.12% | 6.86 -> 6.92 | 1.18M -> 1.10M (-7%) |
| terrace | 0.44% -> 0.05% | 0.50% -> 0.10% | 7.44 -> 7.45 | 0.75M -> 0.71M (-6%) |
| hedge | 0.08% -> 0.07% | 0.09% -> 0.07% | 7.09 -> 7.13 | 0.40M -> 0.38M (-4%) |

The old levels were themselves wrong on the terrace: big orange and white patches on its awnings,
which their dear collapses had made and nothing measured. They are gone.

**Cost.** GPU memory is unchanged (3.15 GB, 4.46 GB) and so is each pass's declared DRAM traffic;
the packs grow by 0.35%: the city by 2.4 MB, Bistro by 4.9 MB. Vertices added: 83 thousand over the
city's meshes. The rooftop's masked raster rises 0.41 ms, because cards stay until their size
allows them to go where collapses used to take them away; its software raster falls 0.42 ms, and
its culling 0.19 ms. Meshes cook in 34 s (the city) and 32 s (Bistro) on fifteen workers.

## Alternatives

Each measured on the way, and the order matters: each of these looked like the answer at the time.

- **Corners alone** lowered a tile's coarsest level from 1.14M to 905k triangles and moved no frame:
  the rooftop went from 10.47 to 10.55 ms, and its pictures showed torn facades with zero recorded
  error. A floor is not a frame: the rooftop never reaches it.
- **Corners with the step's border cost** mended that picture and cost the frame (rooftop 11.03 ms):
  an honest error keeps finer levels drawn longer, and groups forced to halve spent dear collapses.
- **The level's error without normals** gave the best frames of all (rooftop 9.96 ms at 1%-low, -21%
  triangles) and a clean city, which had no cloth to show otherwise. Bistro's pictures did: an image
  error of 1.06% on the street against the old levels' 0.25%. The old levels' high errors had hidden
  this by keeping fine levels drawn. It is not offered: it trades quality.
- **A level's error at other factors,** a tile's triangles at each error in thousands (base is before
  all of this; the others had corners and borders, and not yet shading):

  | Error (cm) | base | factor 1.41 | **2** | 2.83 | 4 |
  |---|---|---|---|---|---|
  | 4 | 4210 | 2786 | **2997** | 3246 | 3399 |
  | 8 | 2874 | 2135 | **2207** | 2277 | 2593 |
  | 32 | 1852 | 1338 | **1307** | 1305 | 1319 |
  | 64 | 1760 | 1244 | **1176** | 1179 | 1182 |
  | 128 | 1363 | 1190 | **1143** | 1120 | 1130 |

  Below 2 it costs the far field; above, the near.
- **The level's error without corners:** at 32 cm 1435k against 1307k, at 64 cm 1354k against 1176k,
  and a floor of 1170k against 917k. Worth having both.
- **Comparing the two ends' normals,** the edge's length times their difference, instead of what the
  surface shows. It charged true curves as much as wrinkles, and left wedges where the ends agreed
  and a third vertex did not. To match the old levels' pictures it needed a weight that gave up the
  rooftop's gain (weight 4: 10.65 ms at 1%-low).
- **Other weights of what the surface shows,** the same shares against level 0 with the street's
  triangles, and the rooftop's 1%-low:

  | Weight | Street, lit | Terrace, lit | Hedge, lit | Street triangles | Rooftop |
  |---|---|---|---|---|---|
  | none | 1.06% | 1.55% | 0.63% | 997k | 9.96 ms |
  | 1/16 | 0.09% | 0.07% | 0.11% | 1028k | |
  | 1/8 | 0.09% | 0.09% | 0.12% | 1063k | 10.00 ms |
  | **1/4** | **0.04%** | **0.05%** | **0.07%** | **1095k** | **10.24 ms** |
  | 1/2 | 0.06% | 0.05% | 0.11% | 1166k | |
  | 1 | 0.05% | 0.05% | 0.10% | 1209k | |
  | before | 0.25% | 0.44% | 0.08% | 1187k | 10.49 ms |

  A quarter is the least that beats the old levels in every view. An eighth would save another 0.24 ms
  on the rooftop and falls behind on the hedge, so that is left unspent.
- **Thinning small pieces** (Cook, Halstead, Planck & Ryu, "Stochastic Simplification of Aggregate
  Detail", SIGGRAPH 2007): after collapses stall, an evenly spread share of a group's smallest pieces
  goes, and those of their size grow to keep their area. It lowers the floor by a further 28%, and pays
  only past 64 cm of error, over a kilometre at a pixel: at 4 to 8 cm it drew 19 to 29% more triangles,
  and it added thirteen times the vertices. It may yet suit the landscape's needles, whose list limit
  decides what that scene draws, and is to be measured there.

## Consequences

- This does not reach the gate. The city's street, walk, sprint and crossing are inside 6.94 ms (6.48
  to 6.85), as they were; its turn (7.22), park (7.74), and rooftop (10.24) are not, nor is any of Bistro's
  views but its walk's mean (1%-lows 6.98 to 7.70). The next levers are as ADR-0067 lists them: masked
  raster, now 2.60 ms on the rooftop, and the hierarchy's walk (ADR-0066).
- The reference budgets are re-baselined (`reference_scenes-2026-10-31`). The rooftop's masked raster
  budget rises by 0.41 ms, and its frame falls by 0.36 ms.
- What the error does not yet see: texture coordinates' change across a collapse where they are not
  affine. Bistro's base colour differs little (0.08 to 0.11% of pixels), so nothing is added for it.
- A level's error reads each group's costs before its colours run, one more pass of each group's
  first candidates, which is parallel and costs a few seconds in a cook.
