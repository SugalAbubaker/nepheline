# 0061. Aggregate geometry's far field: voxel levels in the cluster hierarchy

- **Date:** 2026-10-26
- **Status:** Accepted (the direction is the user's decision of 2026-10-26; the design
  below is built in the stages listed at the end, each measured as it lands)

## Context

ADR-0060's levels simplify a mesh by edge collapses that keep its topology. That suits
surfaces (ground, rocks, trunks, ruins), but not **aggregate geometry**: foliage, twigs,
and debris made of many small disconnected pieces. A collapse cannot remove a piece,
so the simplifier stalls once each piece is down to a few triangles. The open-landscape
reference scene (Poly Haven's pine forest in our layout, Phase 13) shows what that
costs:

- The pine sapling (2.66 million triangles) stops at 95,716 triangles in 1,539
  clusters; the fir sapling (157,000) at 4,503. The rocks reach 58 to 488.
- At ground level the frame took 10.8 to 16 ms. The visible-cluster, meshlet, and
  shadow-meshlet lists overflowed (so terrain and near trees went missing); with
  four times the capacity it still overflowed at 2 million clusters and 134 million
  triangles, in 35 ms.

The first game is a forested valley with modelled vegetation to the horizon
(ADR-0059), so this is the case §8.6 stage 9's "cost bounded by pixels" must hold
for. The user chose the direction on 2026-10-26: distant foliage becomes voxels, as
Unreal Engine 5.7's Nanite Foliage does, rather than pruning pieces and preserving
area (Cook et al. 2007; Nanite's "preserve area"), which was the alternative put to
them.

What is known of Nanite's voxels (Epic, "Nanite Foliage", UE 5.7 documentation,
Experimental; Unreal Fest Stockholm 2025):

- The builder voxelizes clusters into at most 128 bricks of 4 × 4 × 4 voxels, and
  switches a part of the hierarchy to voxels where that gives a lower error than
  triangles.
- Each voxel stores a distribution of normals rather than one normal. The G-buffer
  pass picks a normal from it at random per pixel; Epic says integrating the
  distribution in the shading, as roughness is, would remove that noise.
- Voxel clusters are rasterised by a separate shader, front to back in depth
  buckets. Flat surfaces stay triangles.

The constraints here:

- **The visibility buffer.** 64 bits per pixel: the depth, then the visible
  cluster's index (25 bits) and a primitive within it (7 bits) (ADR-0029). A voxel
  must be drawn into it and shaded from it like a triangle.
- **The cut.** Voxels must take part in the same cut as triangles, decided by the same
  `precise` test, so that no part of a mesh is drawn twice or not at all (ADR-0060).
- **No noise.** Shading must not depend on per-frame random choices that temporal
  anti-aliasing has to average away: the far field is where it would shimmer, and
  TAA's neighbourhood clamp keeps part of such noise.
- **The resolve is at its register limit** (docs/render.md "Shading"). A new path
  must not add registers to the triangles' shading, or Bistro slows down (the §10.7
  ratchet).
- **Ours** (CLAUDE.md §3): voxelizer, representation, raster, and shading are written
  here.

## Decision

### The representation

A **voxel cluster** is a cluster of the hierarchy whose primitives are bricks. It
holds at most 32 bricks, all of one cell size, and at most 1,024 voxels. A **brick** is
4 × 4 × 4 cells of one size: its lowest corner and cell size (mesh space), a 64-bit
occupancy mask (bit x + 4y + 16z), and the index of its first voxel, its other voxels
following in bit order. That is 32 bytes. A cluster's bricks' voxels follow one
another in the bricks' order, so the cluster's voxel k is its first brick's first
voxel plus k. Bricks lie on a grid aligned to four cells of their size from the
mesh's origin, so neighbouring clusters' bricks of one size never overlap.

A **voxel** (16 bytes) is what the cell's geometry looks like from outside, at the
cell's scale:

- **Its footprint**, a box inside the cell (corners in 32nds of the cell), the
  ellipsoid inscribed in it, or a capsule between two of its opposite corners. A ray
  through the cell meets the voxel exactly where it meets the footprint.
- **Its albedo** (sRGB, 8 bits a channel) and the material's ambient occlusion.
- **Its roughness and metallic.**
- **Its distribution of normals:** an axis (octahedral, 16 bits a component) and a
  concentration κ. These describe a two-sided lobe, a von Mises-Fisher distribution
  about the axis and its opposite. Foliage is two-sided, and a voxel is seen from
  every side.

The footprint does the job that partial opacity would otherwise do. To build it:

1. Each axis gets its **coverage**: the fraction of 16 × 16 jittered rays along that
   axis through the cell that hit the cell's geometry. This is the union of the
   pieces' projections, so geometry hidden behind other geometry in the cell is not
   counted twice (Heitz & Neyret, "Representing Appearance and Pre-filtering Subpixel
   Data in Sparse Voxel Octrees", HPG 2012).
2. The footprint lies within the bounds of the rays' hits, widened by half a ray's
   spacing.
3. A **surface's** footprint is those bounds: a sheet crossing the cell keeps its
   whole extent, so a surface stays watertight from cell to cell. This avoids the
   holes Nanite's voxelised trunks show. A surface is a cell whose normals' second
   moment is 0.9 or more along one axis and whose bounds' shadow along the axis of
   most coverage is within a quarter of that coverage: a sheet across the cell fills
   it, a thin sliver across it at a slant does not.
4. A **needle's** (the hits strung along one line, their spread's largest eigenvalue
   0.9 of the whole) is a capsule along that line between the hits' farthest points
   on it, of the radius of their spread across it, held to the cell. Its projected
   area follows the needle's as the view turns, down to its section end-on, which
   no shape aligned with the grid can do. The radius comes from the hits' points,
   which lie on the needle's surface, and not from the coverage: a needle a
   hundredth of the cell's face across meets two or three of an axis's 256 rays,
   so its coverage is known only to about 40 %. Its code is logarithmic,
   2^(c/8 − 8) of the cell from 1/256 to 1/2, 9 % a step.
5. An **aggregate's** is smaller, sized so that its projected area along each axis
   is that axis's coverage: the ellipsoid inscribed in a box where an ellipsoid can
   reach the coverages, else the box. A box's projected area along a direction d is
   c_x|d_x| + c_y|d_y| + c_z|d_z| of the cell's face (c the coverages), up to √3 times
   an isotropic cloud's along a diagonal of the grid. The ellipsoid's is
   √((c_x d_x)² + (c_y d_y)² + (c_z d_z)²), the projected area of an SGGX
   distribution whose axes are the grid's (Heitz, Dupuy, Crassin & Dachsbacher, "The
   SGGX Microflake Distribution", SIGGRAPH 2015): the same from every direction for
   an isotropic cloud, a cosine for a disc.
6. An aggregate's sits at a place within the bounds hashed from the cell. Footprints in their
   cells' middles would line up along each axis, so that a ray down a column of
   cells would meet all of them or none, where the pieces they stand for are
   independent from cell to cell; Cook et al. place their stand-ins at random for the
   same reason ("Stochastic Simplification of Aggregate Detail", SIGGRAPH 2007).
   Geometry gathered in one corner keeps its place, since its bounds are small.

Rays through a canopy of such footprints stop about as often as through the needles:
Cook et al.'s area preservation, in a deterministic form, with no per-frame
randomness.

The voxel's other attributes come from the same rays:

- Albedo, roughness, metallic, and occlusion are averaged over the first and last
  hit of every other ray each way, from all six directions: what is seen from
  outside the cell, not what is hidden inside it.
- The normal statistics come from those hits too. Each hit is weighted by
  1 / (|nₓ| + |n_y| + |n_z|), which cancels the bias that axis-aligned views give
  toward axis-facing normals, so the estimate follows the pieces' area. The axis
  is the dominant eigenvector of the second moment E[n nᵀ]. κ is the concentration
  whose E[(n·μ)²] equals the dominant eigenvalue (1/3 for an isotropic cloud, 1 for
  a flat sheet).

### The build (`render/cluster_lod.h`, `cluster_voxel.c`)

Voxels are made inside ADR-0060's level-by-level build.

- **Which groups may.** Aggregate geometry alone: a group of at least 256
  triangles whose pieces (triangles connected through shared positions) average at
  most 32 triangles. A surface that stalls on its borders or seams (an open sheet's
  coarsest groups, a flat-shaded mesh) is one piece or a few, and stays triangles; a
  smaller group costs little as it is.
- **When a group becomes voxels.** A group is simplified as before. It becomes voxels
  instead when either of these holds:
  - the simplifier stalls, keeping more than three quarters of its triangles where
    it aimed for half;
  - its quadric error, which is a lower bound of its final error, exceeds the error of
    the voxels it would become (their cell size, below).

  A group that holds a voxel cluster always becomes voxels again: once voxels, a part
  of the mesh never returns to triangles.
- **The cell size and the error.** Cell sizes are powers of two in mesh units, so
  cells of one size align across the whole mesh. The voxels' error is the cell size;
  the group's error is the largest of that and its makers' errors, so errors still
  never fall going up.
  - *Where triangles first become voxels*, the cell is the smallest power of two at
    least twice the inputs' error whose occupied cells number at most four times the
    group's triangles. Halving the primitives there, as a triangle level does, would
    jump the error: a stalled group's error is that of its finest pieces (about a
    millimetre for the pines' needles), while cells few enough to halve its triangles
    are centimetres across. Every distance between the two would then draw the
    stalled triangles, the pines' 600,000 a tree out to 160 m. With at most four
    times the count, the pines' first cells come out near 8 mm and voxels take over
    from about 15 m (at 4K and 90°).
  - *Above that*, a voxel group's cell is at least twice its input voxels', and its
    occupied cells fit half its input primitives, or as few as they can be for a
    group of a few voxels across the mesh's origin: the grid of every cell size
    divides each axis at zero, so such a group lies in two cells along each axis it
    crosses however large they grow (a tree's trunk stands there).
  - A level of voxel groups always coarsens, so the rule that stops the levels when
    they stop shrinking does not stop a level in which triangles became voxels (which
    happens once a group at most).
- **The source triangles.** A group voxelizes the triangles its clusters hold.
  Triangles first become voxels at the level where their group stalls. A voxel
  cluster keeps the triangles it was made from for the next level's build, each
  triangle in exactly one cluster (the one holding the cell of its centroid). Every
  coarser level therefore voxelizes those same triangles again at its own cell size,
  rather than averaging finer voxels, which would mix up the correlation the
  footprints measure (Heitz & Neyret 2012).
- **Materials.** The builder takes a material sampler from its caller, which gives
  base colour, alpha, normal (with the normal map), roughness, metallic, and
  occlusion at a point of a triangle. The cooker samples the decoded textures with a
  footprint matching the ray spacing. A masked material's rays pass where its alpha
  is below the cutoff, so alpha-tested foliage voxelizes too. Some groups never
  become voxels:
  - groups with an emissive or blended material;
  - groups of a mesh built without a sampler (meshes made at run time).
- **Splitting.** The bricks are split at their voxels' median along the longest axis
  of their span until each part fits a voxel cluster. A voxel cluster's bounds hold
  its footprints, and it has no normal cone.
- **Stopping.** The levels go on until one cluster is left: voxel levels always shrink.

### The cut and the cull

The hierarchy, its walk, and `vis_lod_group_too_coarse` are unchanged: a voxel cluster
has its maker and consumer like any cluster. The shadow map's uniform cuts
(`nph_render_lod_cuts`) need nothing new either.

In the record, a meshlet with no vertices is a voxel cluster:

- `vertex_offset` holds its first brick;
- `triangle_count` holds its brick count;
- `submesh` holds the submesh of most of its hits.

The meshlet cull hands voxel clusters to a raster list of their own,
`NPH_VIS_RASTER_VOXEL`, and flags them `NPH_VIS_CLUSTER_VOXELS`.

### The raster (`vis_voxel_raster.comp`)

The raster uses one workgroup per voxel cluster and splats its voxels, an invocation
per voxel.

1. **Rays.** A pixel's ray runs from the camera through its point on the near plane.
   The projection is reverse-Z with an infinite far plane, so a hit at parameter t
   along that ray has depth exactly 1/t.
2. **Voxels to pixels.** Each voxel projects its footprint's box (a capsule's widened
   by its radius, within its cell) to the pixel centres it may cover. The box's
   corners lie on its brick's grid of 32nds of a cell, so its centre and half-extents
   reach clip space as steps along that grid, which are the same for the whole cluster
   since its bricks share a cell size. The projection lies within
   `(r_x + |x_c / w_c| r_w) / (w_c − r_w)` of the centre's in x, and likewise in y,
   where r_x and r_w are the half-extents' reach in clip x and w; it is widened by a
   64th of a pixel against rounding. A voxel wholly behind the near plane covers
   nothing.
3. **Pairs.** A subgroup pools its voxels' (voxel, pixel) pairs, a prefix sum and a
   binary search spreading them over its lanes, so the rays' tests fill the lanes
   whichever voxels cover pixels. At the first voxel levels a cell is about a pixel
   and a footprint a fraction of its cell, so about one voxel in five covers a pixel's
   centre.
4. **The test.** Each pair's ray goes into mesh space through the inverse of the
   instance's linear part and is tested against that voxel alone: the ray's span
   through the voxel's cell, from the cell's own planes, then the footprint over
   that span. Cells are powers of two and their corners small multiples of them, so
   those planes are exact, and the test depends on nothing but the ray and the
   voxel.
5. **Output.** A hit is written with the atomic maximum, as triangles are, so the
   nearest voxel wins. The 7-bit primitive field names the brick.

The test is one `precise` function. The resolve and the radiance cache's update find
the pixel's voxel again in the brick the visibility buffer names: they test the cell
holding the ray's point at the stored depth, then the two cells along each axis whose
centres lie nearest that point (which hold it whatever its rounding), and stop at the
first whose hit gives the stored depth again, bit for bit. They get the box face the
ray entered as the geometric normal. That normal offsets the shadow lookups and keys
the radiance cache, as a triangle's normal does.

### The shading (`vis_voxel_resolve.comp`)

The triangle resolve's first step reads the cluster record it reads anyway; at a voxel
cluster's pixel it writes nothing. The first invocation of a subgroup that saw one sets
its tile's bit, and the one that set it appends the tile to a list (a subgroup lies in
one tile, so the resolve gains no barrier), and a separate voxel resolve shades those
tiles through an indirect dispatch. The two paths compile separately, so the
triangles' registers do not change.

A voxel is shaded with the surface model of `surface.glsl` (lights, light grid, VSM,
radiance cache, ambient occlusion), with the distribution of normals integrated
analytically and no random choice:

- **The normal** is the lobe's axis, turned toward the viewer.
- **Diffuse light from a light l:** the cosine at l, weighted by how much of each
  normal the viewer sees, averaged over the lobe:
  ∫ D(n) (n·v)⁺ (n·l)⁺ dn / ∫ D(n) (n·v)⁺ dn.
  The viewer's clamped cosine is taken as a spherical Gaussian of sharpness 2.8,
  whose product with the vMF lobe is a lobe again (Wang et al., "All-Frequency
  Rendering of Dynamic, Spatially-Varying Reflectance", SIGGRAPH Asia 2009). That
  visible lobe's mean clamped cosine toward the light has Stephen Hill's fitted
  closed form (in Pettineo, "SG Series Part 3: Diffuse Lighting From an SG Light
  Source", 2016), which is the cosine itself for a lobe of one normal: a flat sheet's
  voxel shades as its triangles do, so the switch between them does not change its
  brightness. Per light it is a dot product and a few multiplies; the lobe's terms
  are the pixel's. Against quadrature of the definition, its error averages 0.7 % of
  a facing light's response, is at most 5 % for a viewer within 60° of the lobe's
  axis, and 8.3 % at a grazing 84°, where the Gaussian does not cut the lobe at its
  horizon as the cosine does.
- **Specular:** GGX with the lobe's variance added to roughness (Toksvig,
  "Mipmapping Normal Maps", 2005; Kaplanyan et al., "Filtering Distributions of
  Normals for Shading Antialiasing", HPG 2016).
- **Diffuse from the environment's spherical harmonics:** convolved with the lobe,
  which scales band 1 by A(κ) = coth κ − 1/κ and band 2 by 1 − 3A(κ)/κ (Han et al.,
  "Frequency Domain Normal Map Filtering", SIGGRAPH 2007; Ramamoorthi & Hanrahan
  2001).

Textures are not sampled: a voxel's attributes are already filtered at its scale.

### Shadows

**The sun's virtual shadow map.** Voxel clusters cast through a raster of their own,
one draw per (cluster, page) as triangles have:

- The mesh shader, or the vertex shader on the indirect path, emits one quad per brick
  over the brick's bounds in the page.
- The fragment shader tests the light's ray against the brick's voxels with the same
  test, then either writes the nearest hit's depth or discards. The quad lies at the
  brick's nearest depth, so the written depth only moves away from the light and the
  depth layout keeps early rejection.

**Rays.** The resolve's shadow rays and the radiance cache's rays still trace level 0's
triangles (ADR-0060's bottom levels). A ray from a voxel starts one cell diagonal off
its face, so it does not hit the triangles the voxel stands for. Bottom levels made of
voxel bricks (procedural boxes, with the traversal as their intersection) are the last
stage.

### What does not change

- Motion vectors, the depth pyramid, ambient occlusion, light tiles, and
  depth-of-field read depth only, and a voxel has one.
- Per-instance material overrides do not reach voxels: a voxel's appearance is baked
  from the mesh's own materials.

### Accuracy

Measured in stage 3 (`test_vis_voxel.c`, on the RTX 3060 Ti and lavapipe alike) on a
foliage mesh of 1,500 six-sided smooth-shaded needles, sixteen instances turned every
way, against level 0 at the same distances:

- **The renderer is exact to its model.** The raster draws every pixel the CPU's rays
  through the same clusters meet, and the voxel resolve's lit pixels are its C twin's
  within 0.1 % on average.
- **Coverage.** The first voxel levels cover 3.7 to 7.1 % more pixels than level 0,
  the coarsest 4.9 % fewer. The footprints are sized to the needles' coverage along
  the voxels' own axes; seen obliquely, their projections (the capsules' length, the
  ellipsoids' and boxes' shadows) drift from the needles'.
- **Light.** The first voxel levels reflect 7 to 10 % more light a pixel. The visible
  lobe weighs a normal by the viewer's cosine, as microfacets do; a needle of a few
  smooth-shaded facets shows its normals spread flatter than that (its visible
  normals' mean cosine to the viewer is 0.55 to 0.61 against the lobe's 2/3), and
  the lobe's axis fits a needle's ring of normals poorly. Coarser cells, which hold
  many needles, are nearly isotropic and agree within a few per cent.

- **Cost.** `render_perf`'s `foliage_4k_production` (1,024 shrubs of 4,000 sparse
  needles, 82 million triangles at level 0, at 4K from head height) draws about
  145,000 voxel clusters of 3.2 million bricks and 24 million voxels a frame: 7.6 ms
  on the RTX 3060 Ti, its voxel raster 3.4 ms and the voxel resolve 0.069 ms, against
  5.6 ms for the same shrubs' stalled triangle levels (`foliage_4k_triangles`) and 6.6
  ms at level 0. A voxel costs the raster about what a triangle does (some 140 ps
  against 146), but at the first voxel levels a cell of a pixel holds about one
  needle's length of a cell, the voxels number about twice the triangles of the
  stalled levels, and four in five of them cover no pixel's centre. The splat is bound
  by the latency of its dependent loads, not by memory or registers ("Alternatives
  considered"). Splatting took the raster from 5.5 ms to 3.4 ms; exact pixel
  rectangles and the instance's ray hoisted out of the pixel loop had taken it from
  10.6 ms to 5.5 ms before.
- **The first voxel level's budget.** At four times the triangles they replace (the
  build's rule), the first voxel levels cover 3.7 to 7.1 % more pixels than level 0 at
  24 and 40 m and reflect 9.6 to 10 % more light a pixel. At one times, the frame is
  5.8 ms (the stalled triangles' 5.6), but the voxels start later, and the simplified
  triangle levels drawn in their place cover 11.6 to 12.5 % fewer pixels than level 0
  there and reflect 9 % less light a pixel. On light a pixel the two err by about as
  much, either way; on coverage the voxels are the closer. The triangles' error is the
  simplifier thinning needles below a pixel, which shows as a canopy that thins and
  pops, where the voxels' excess of light is the lobe's limit, which the follow-ups
  below address. The budget stays at four times.
- **Frames without voxels.** Until the renderer holds a mesh with voxel levels, the
  meshlet cull and the resolve are specialised without the voxels' routing and test
  (`k_voxels`), and the voxel passes and their tile list are left out: `render_perf`'s
  4K field, LOD and shadow scenes and Bistro's street measure within 0.004 ms of their
  frames before, the resolve unchanged. Compiled in, the resolve's test alone, never
  taken, cost it 0.5 to 1.7 % on those scenes in every form measured: in the cluster
  branch (where its early exit also moved the production variant's spills from 1,792
  to 2,304 bytes), placed before anything of the triangles' path (1.7 %, behind a
  dependent load of the cluster's flags), as a bare exit there (1.1 %), and on a bit
  of the visibility key with no load (0.5 to 0.7 %).

The tests hold the coverage to 8 % and the light to 12 % as they stand. The next work,
before stage 5 measures the landscape and before the cooker makes voxels by default:

- the raster's cost: each voxel's cell tested against the pixel centres before its
  record is read (a voxel whose cell covers no centre needs no record), bricks
  tested against the depth pyramid, and a shorter chain of dependent loads in the
  prologue; on denser foliage like the landscape's pines, where a cell aggregates
  many needles;
- the accuracy: a visibility sharpness per voxel in place of the constant 2.8, fitted
  in the build to the diffuse responses its hits give from each axis (a byte from the
  normal's encoding), and footprints sized against oblique views as well as the axes;

each measured against level 0 on these needles and on the landscape's own pines.

### Stages

1. The voxelizer and the voxel levels in the builder, the C twin of the traversal,
   and their tests. Done (PR #112).
2. The mesh format (version 3: `VOXB` bricks, `VOXA` voxels), the loader's checks,
   the cooker's sampler, and fuzzing. Done (PR #113).
3. The renderer: the voxel raster list, raster, and resolve, the radiance cache's
   surface point, statistics, and the debug views. Done (PR #114; the splat raster,
   PR #115). Until stage 4 the shadow map's uniform cuts end below the first that
   holds a voxel cluster. The cooker's
   voxels stay opt-in (`--voxels`): on sparse foliage the raster's first voxel
   levels still cost more than the stalled triangles they replace ("Accuracy"), and
   they become the default once it draws the reference scenes' foliage faster.
4. The shadow map's voxel casters, on both raster paths.
5. The landscape cooked with voxels, measured against every §10.4 line.
6. Voxel bottom levels for rays.

## Alternatives considered

- **Pruning pieces with area preservation** (Cook et al. 2007; Nanite's "preserve
  area"). Triangles throughout and no new raster, but it thins and pops a canopy, and
  far trees still cost a triangle per remaining piece. This was put to the user, who
  declined it.
- **Binary voxels, a full cell or none** (the simplest form of Nanite's bricks).
  Coverage is quantised to whole cells, so a sparse cell is either opaque or gone.
  Sparse foliage turns solid or vanishes, depending on the threshold.
- **Random opacity or normals per pixel and frame** (stochastic transparency;
  Nanite's G-buffer normals). Unbiased, but noisy where the far field is still.
  Epic's own documentation names the noise as the thing to remove.
- **A participating medium, ray-marched** (Loubet & Neyret, "Hybrid Mesh-Volume LoDs
  for All-Scale Pre-filtering of Complex 3D Assets", Eurographics 2017, with SGGX
  microflakes, Heitz et al. 2015). The most faithful for sub-pixel geometry, but it
  composites many samples per pixel, which an opaque visibility buffer cannot hold,
  and its cost follows depth complexity rather than pixels. The footprints and the
  lobe keep what of it an opaque buffer can: coverage that preserves self-occlusion,
  and a normal distribution in place of a normal.
- **An SGGX matrix per voxel** instead of an axial lobe. SGGX holds fibre-like
  distributions (a twig's needles), which a lobe represents only by their dominant
  direction with a wide spread. But SGGX has no closed-form diffuse integral; it is
  sampled. The lobe is the deterministic choice. A fibre-like cell gets κ near 2
  (eigenvalue near 1/2), which shades close to an isotropic cloud.
- **A separate sparse voxel octree or voxel DAG for the far field** (Laine & Karras,
  "Efficient Sparse Voxel Octrees", I3D 2010; Crassin et al., GigaVoxels, 2009). A
  second hierarchy and walk beside ADR-0060's, with its own transitions. Bricks in
  the cluster hierarchy reuse the cut, the culling, the streaming unit, and the
  shadow map's cuts, and switch where the cut does.
- **Billboards and impostors.** Flat, view-dependent, and alpha-tested; excluded by
  §8.6 for our content.
- **Each pixel of a brick's rectangle walking the brick's cells** (stage 3's first
  raster, with Amanatides & Woo's traversal, "A Fast Voxel Traversal Algorithm for Ray
  Tracing", Eurographics 1987). On `foliage_4k_production` its 14.9 million rays took
  59.7 million steps and 15.2 million footprint tests for 1.78 million hits, since a
  brick's rectangle is mostly pixels that no footprint covers: 5.5 ms against the
  splat's 3.4. A cell's span along a ray also came from the walk's running sums, so
  only a pass that walked the same way met a voxel at the same t.
- **The resolve's search over all eight cells nearest the depth's point**, without
  the point's own cell first and the stop at the stored depth: the voxel resolve took
  0.104 ms against 0.068.
- **A faster splat by registers or by queueing**: the workgroup's constants kept in
  shared memory, which took the raster from 72 registers to 64, and each subgroup's
  pairs queued in a ring and tested 32 at a time. Neither measured faster per cluster
  drawn (clusters drawn vary by a few per cent from run to run with the occlusion).
  With every voxel's record read from the same 64, always cached (an ablation, its
  pixels wrong), it was no faster either: the splat waits on its loads' latency, not
  on memory.
- **One resolve for both primitive kinds.** One pass and no tile list, but the voxel
  shading's live values would join the triangles' at their register limit.
- **Box footprints for aggregates too** (the first version). A box sized to the axes'
  coverages over-covers a sparse isotropic cloud by up to √3 along a diagonal of the
  grid, so a canopy would look denser from some directions than others.
- **Footprints in their cells' middles** (the first version): a cloud of 2,000
  needles stopped 0.030 of a grid of rays along an axis where the needles stopped
  0.073, since a column of cells' footprints lined up. Hashed places: 0.071.
- **Coverage matching for surfaces too** (the first version): a sheet that crossed
  only part of a cell got a box narrower than the sheet, and 84 of 4,000 rays went
  through a sheet across sixteen cells. Surfaces keep their bounds: none.
- **Aligned normals alone for a surface:** a cell crossed by a thin sliver of one
  needle at a slant has them too, and its bounds' box covered several times what the
  sliver does: a foliage mesh's cut of voxels at 300 m covered 16 % more than level 0
  (12,544 rays). With the shadow's test as well: 9 %.
- **Ellipsoids for needles** (before the capsule): a single straight piece's
  projected area falls to its section along the piece, which no axis-aligned shape's
  does, so needle cells over-covered from most directions: that cut's remaining 9 %.
- **A capsule along the diagonal of the hits' widened bounds, its radius from the
  coverage** (the capsule's first version): the bounds and their widening made it
  about 10 % longer than the needle, its clipped ends added about as much, and the
  coverage-fitted radius was off by up to 40 %, so a needle's coverage came out 10
  to 24 % off from some directions. Between the hits' extremes on the line, with the
  hits' spread for its radius: within 11 %, and the cut covers what level 0 covers
  (1,778 rays of 9,216 against 1,777).
- **Voxels wherever the collapses stall** (the first version): an open wavy sheet's
  coarsest groups stall on its locked border, and 317 bricks stood in for a surface
  its triangles drew well. Groups of few pieces stay triangles.
- **Wang et al.'s Gaussian for the light's cosine too** (the first version): its
  amplitude of 1.17 makes a flat sheet's voxel 17 % brighter facing a light than its
  triangles, which would show at every switch. Hill's fit is the cosine there.
- **A viewer's Gaussian whose sharpness follows the view** (the slope of the cosine at
  the lobe's axis, or blends of it with a constant): the best lowered the worst case
  from 0.083 to 0.059 by raising the error of other lobes; 1 / (n·v) itself raised
  it to 0.37. The constant stays: the alternatives moved the error between lobes
  rather than removing it.

## Consequences

- A far-field tree costs about a voxel per pixel it covers, whatever its pieces: the
  landscape's lists and triangles stop growing with distance. The numbers come in
  stage 5.
- The cut switches to voxels where a cell is about a pixel (the view's threshold, one
  pixel by default). A cluster's switch is a pop below a pixel, as between triangle
  levels. Its coverage and mean colour are preserved by construction, and the tests
  measure them against level 0.
- A voxel's appearance is a filtered stand-in. Two limits are named and measured
  rather than assumed:
  - shadowing inside a cell is the shadow map's (texels of about a cell there, as for
    triangles at that distance);
  - a fibre-like cell shades as a wide lobe.
- Memory: 16 bytes a voxel and 32 a brick. The first voxel level holds up to four
  times the stalled triangles it stands for, and the coarser levels about a third
  again (the pines: an estimated 35 MB a tree species, beside their triangles).
- The build voxelizes each stalled triangle once per voxel level, with 3 × 256 rays a
  cell: 0.89 s for 2,000 closed needles (16,000 triangles) on one thread, about 55 µs
  a triangle against 11 for the triangle levels alone, most of it in the cells' rays.
  The groups are independent and will run as jobs for big content (the pines).
- Validation: a voxel cluster's bricks (of one cell size), masks, and voxel ranges
  (following one another in the bricks' order) are checked when a mesh is created and
  when it is loaded, and the loader's checks are fuzzed.
- Tests (stage 1, `test_voxel.c`):
  - a ray's first voxel against a search of every footprint in double precision (a
    capsule's by bisection of the distance from its segment): 25,600 rays at 400
    random bricks of every shape agree on every hit;
  - the shading terms against Monte Carlo integrals of their definitions;
  - the voxelizer: a sheet across cells and a slanted one stop every ray through
    them; a cloud of 2,000 needles keeps its coverage within 9 % from the axes, the
    diagonals and a random direction; a needle across a cell becomes a capsule within
    11 % of its coverage from six directions, and a dot end-on; a two-sheet cell's
    look and a masked surface's footprint; the same triangles give the same bytes;
  - the levels: stalled foliage becomes voxels up to a root of one voxel cluster,
    with valid bricks, masks and bounds and errors that never fall; a wavy sheet
    makes none; a broken sampler is refused; cuts at 300 m and 1.2 km are all voxels
    and cover what level 0 covers, within the rays' sampling.

- Tests (stage 3, `test_vis_voxel.c`, on the RTX 3060 Ti and lavapipe): the shaders'
  voxel test against the C one (16,384 rays, every hit, voxel and face alike), and the
  resolve's search from each hit's depth, which meets the hit again exactly; the
  raster against the CPU's rays through the clusters drawn, pixel for pixel; the voxel
  resolve's albedo and its lit pixels against its C twin; the voxel levels' coverage
  and light against level 0 ("Accuracy"); clusters whose bricks differ in cell size or
  whose voxels leave the bricks' order refused.

  To come with their stages: the shadow casters against level 0's shadow.
