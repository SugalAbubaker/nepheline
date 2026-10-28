# The asset cooker (`tools/cook`)

The offline cooker of CLAUDE.md §8.8: it turns source content (glTF 2.0
scenes, images, editable meshes, WAV sounds) into the cooked runtime files
of the asset module
([asset.md](asset.md)) and packs them. It is a library, `nph::cook`, used
by the tests, the benchmarks, and the build of the samples, and a
command-line tool, `nph_cook`. Decisions:
[ADR-0017](adr/0017-cooker.md).

The editor imports through the same sources built over the engine's shared
library, `nph::cook_shared` (only with `NPH_BUILD_EDITOR`): the editor runs
one engine, and linking the static modules the cooker uses beside the
shared library would give it two logs and two job systems
([ADR-0026](adr/0026-editor-import.md)).

The cooker is the only code that includes the two decoding libraries
CLAUDE.md §3 allows offline, cgltf and stb_image (`scripts/check_layering.py`
enforces it), and it never ships: a game links the asset runtime, whose
files need no parsing step, and reads packs. Everything else is the
engine's own: the mesh pipeline, the block encoders, mip generation, the
asset database, and the identity scheme.

| Header | Contents |
|---|---|
| `cook.h` | the log category, `COOK_VERSION` and `COOK_MESH_VERSION`, memory blocks for the third-party libraries |
| `image.h` | source images: decoding, sRGB transfer, exact-area mips, normal renormalisation, 4x4 block fetch |
| `bc.h` | the BC4, BC5, BC6H, and BC7 encoders and decoders |
| `texture.h` | an image to a texture asset: usage, mip chain, encoding |
| `mesh.h` | the mesh pipeline: cleaning, normals, welding, tangents, reordering, meshlets, quantisation, levels of detail |
| `gltf.h` | glTF 2.0 import into the cooker's intermediate form |
| `sound.h` | a WAV file to a sound asset: streaming, the codec, per-source settings |
| `database.h` | the asset database, `cook.db.json` |
| `cooker.h` | the cook of a source tree: identity, incremental decisions, outputs, packs |

## Using it

```
nph_cook <source>... -o <out_dir> [--root <dir>] [--pack <file.npk>]
         [--force] [--lz fast|high] [--usage color|linear|normal|mask|hdr]
         [--uncompressed] [--no-mips] [--max-extent N]
         [--no-optimize] [--no-lods] [--voxels] [--hlod] [--hlod-cell M]
         [--hlod-distance M] [--threads N] [--quiet] [--verbose]
nph_cook --pack-only -o <out_dir> --pack <file.npk>
```

A source is a `.gltf`, a `.glb`, an image (`.png`, `.jpg`, `.tga`, `.bmp`,
`.hdr`), an editable mesh (`.nmesh`), or a directory, whose scenes and
editable meshes are cooked in sorted order. `--root` is the source root that identities are relative to
(default: the current directory); a source outside it is refused. The
output directory receives the cooked files and the database; `--pack`
then writes every asset the database knows into one archive. The exit
status is 0 when every source cooked or was current, 1 when one failed,
and 2 for a usage error. Block encoding and the meshes' levels of detail
run on the job system (`--threads`, default cores - 1), to the same bytes
as on one thread. Each mesh's log line is followed by where its time went:
the cleaning, attributes and welding, the reordering, the meshlets, the
levels, and the blobs' compression. With `--verbose` each level of each mesh
gets a debug line too: its clusters (voxel ones apart), triangles, voxels
and their cells, and the errors of the groups that made it, which show
where a mesh's levels stop shrinking or switch to voxels. Then each cut at a
power-of-two error (`nph_render_lod_cuts`) gets one: the clusters,
triangles, bricks and voxels a view drawing the mesh at that error draws,
which show how far off it stays costly.

The asset_viewer sample cooks its content this way as part of the build
(`samples/CMakeLists.txt`): a custom command runs the freshly built
`nph_cook` on `samples/asset_viewer/content/showcase.gltf` and the sample
mounts the pack.

## Identity, outputs, and incremental cooks

**Identity.** A source is named by its path relative to the root, with
`/` separators: the *source key* (`models/crate.gltf`). Each asset it
produces has a *sub-asset* name, and its GUID is `nph_guid_from_cstr` of
`"<key>#<sub>"`:

| Asset | Sub-asset |
|---|---|
| a glTF's scene | `scene` |
| mesh *i* | `mesh/i` |
| material *i* | `material/i`; the default material of primitives without one: `material/default` |
| image *i* encoded for a usage | `image/i/<usage>` (`color`, `linear`, `normal`, `mask`, `hdr`) |
| a standalone image | `<usage>` |

The identity is therefore independent of the working directory, the
machine, and the order of cooking, and a reference from a scene to its
meshes or from a material to its textures is the same GUID on every run.
A game finds the scene of `levels/a.gltf` as
`nph_guid_from_cstr("levels/a.gltf#scene")` and everything else through
the GUIDs stored in it (`nph_asset_load_guid`, [asset.md](asset.md)). One
image used with two usages (a texture that is both a base colour and an
emissive map, say) is encoded once per usage.

Keys come from canonical paths: `cooker_canonical_path` makes a path
absolute, with `/` separators and no `.` or `..` components, and
`cooker_relative_path` takes the part below a root (names compare without
case on Windows). `cooker_source_kind` says what the cooker makes of a
file by its extension. The editor uses all three to decide what lies
inside a project's sources and what to cook.

**Outputs** mirror the source tree:
`<key with its last '.' made '_'>/<kind>_<index>_<name>.nph`, for example
`models/crate_gltf/mesh_0_Crate.nph`, sanitised to `[A-Za-z0-9._-]`. If
sanitising ever makes two assets collide, the later one gets a GUID
suffix (`ALREADY_EXISTS` is never silent). The same path is the asset's
virtual path in a pack.

**The database** (`cook.db.json` in the output directory) has one entry
per cooked asset: source key, sub-asset, GUID, type, output path, content
hash, settings hash, and the GUIDs it references. It is pretty-printed
and sorted by output path so it diffs cleanly; it is saved atomically
(written to `.tmp`, then renamed) and loaded all or nothing, so an
interrupted cook can never leave a half-written database behind.

**An asset is current**, and skipped, when its entry's content hash and
settings hash match and its output file exists. The content hash covers
what the asset is made of: for a glTF scene, the document and every
buffer and image; for a texture, its image's bytes alone (editing a
glTF's materials or nodes re-encodes no texture). The settings hash
covers the cooker's settings for that type, the asset format and type
versions, and `COOK_VERSION`, which is bumped whenever the cooker's
output for an unchanged source changes in a way the hash does not
otherwise see, so a cooker improvement recooks everything on the next run.
Meshes add `COOK_MESH_VERSION`, bumped for a change to the mesh pipeline
alone (the meshlet and level builders): it recooks the meshes and leaves
the textures, whose encoding is most of a cook (Bistro's meshes recook in
about a minute; its textures take some 23). When a source is cooked again, assets it no
longer produces (a deleted mesh) leave the database and their files are
removed. `--force` cooks everything.

## glTF import (`gltf.h`)

cgltf parses and validates the document; the importer reads every
accessor through `cgltf_accessor_unpack_floats`, so every component type
the specification allows works, normalised integers and
KHR_mesh_quantization included, as do sparse accessors. All memory goes
through the caller's allocator, all file access through the engine's
files, and with no base directory (a document in memory) external
buffers and images are refused rather than resolved against the working
directory.

| Imported | Notes |
|---|---|
| triangle primitives | lists, strips, and fans (triangulated); one submesh per primitive |
| POSITION, NORMAL, TANGENT, TEXCOORD_0, COLOR_0 | any allowed component type; non-finite values and positions beyond ±1e9 are refused, but for tangents: a primitive with a non-finite tangent (exporters write NaN where texture coordinates are degenerate) gets its tangents made, with a warning |
| missing normals | flat normals, as glTF 2.0 §3.7.2.1 requires; the source tangents are then ignored |
| missing tangents | MikkTSpace tangents when the primitive has UVs (per mesh, or per primitive in a mesh where only some have them) |
| metallic-roughness materials | five textures with their samplers, alpha mode and cutoff, double-sidedness |
| KHR_materials_pbrSpecularGlossiness | converted to metallic-roughness (below); takes precedence where a material has both |
| KHR_materials_unlit, KHR_materials_emissive_strength | |
| MSFT_texture_dds | a texture's DDS source replaces its `source`, which is then a fallback the importer does not read |
| KHR_texture_transform | baked into each primitive's TEXCOORD_0 from its material's transform (offset, rotation, scale, composed as the extension does); authored tangents turn with the texture's u axis, and a mirroring transform flips their handedness. Where a material's textures differ, its base colour's is baked, with a warning (a baked transform is one per material); a `texCoord` override is warned about and TEXCOORD_0 used |
| node TRS or matrices | parent-before-child order, iterative traversal; a node reached twice is warned about and kept once |
| cameras, KHR_lights_punctual | instanced at every node that uses them |
| the default scene | else the first scene, else every root node |

Of the images, only those the materials use are read: an image no slot
takes (the fallback of a DDS source, or one nothing references) may be
missing. Skins, morph targets, animations, points and lines, TEXCOORD_1
and above, and other extensions are skipped with a warning when the
document merely uses them; a document that *requires*
an extension the importer does not implement is refused with
`NPH_RESULT_UNSUPPORTED`, because skipping it would change what the
asset looks like.

**Specular-glossiness materials** (`specgloss.h`) become metallic-roughness
by the conversion the extension's specification publishes (its
"convert-between-workflows" example, after Babylon.js), in linear light:
the metallic share solved from the perceived brightnesses of the diffuse
and specular colours against a dielectric's 0.04, the base colour blended
between the colour the diffuse implies and the one the specular implies by
the metallic share squared, roughness one minus glossiness, and the
diffuse alpha the opacity. It is exact for dielectrics and metals, and a
grey material made by the inverse model comes back to within 2·10⁻⁴. A
material with factors alone is converted on import; one with textures
gets a base colour (BC7 sRGB) and a metallic-roughness (BC7 linear)
texture made texel by texel from its diffuse and specular-glossiness
images and its factors, which the textures then carry (the material's
factors are one). The textures take the larger extent of the two images,
the other sampled bilinearly; materials that share the images and the
factors share the textures.

`cook_gltf_files` lists what a copy of a document needs beside it: the
files its buffers and images name, percent-decoded and normalised,
relative to its directory, each once (an image that only serves as the
fallback of DDS sources is left out). It reads only the document. A URI
with a scheme, an absolute path, or one above the document's directory is
refused, since a copy could not keep it beside the document. The editor's
import copies a dropped `.gltf` with these files.

## Editable meshes (`polymesh.h`)

The editor's own meshes (CLAUDE.md §8.13 "primitives ... and basic mesh
editing ..., saved as cooked meshes";
[ADR-0027](adr/0027-editable-meshes.md)) are `.nmesh` sources: polygon
meshes the cooker cooks like any other source, into one mesh asset with
sub-asset `mesh`.

A `CookPolyMesh` is positions and faces. A face is a loop of corners,
counter-clockwise seen from its front. Each corner has a position and a
UV of its own, so faces meet with seams where their UVs differ, and each
face has a material slot.
- **Triangulation.** Each face is cut in the plane facing its Newell
  normal (Sutherland, Sproull & Schumacker 1974): a fan when the face is
  convex, otherwise ear clipping (Eberly, "Triangulation by Ear
  Clipping", 2002). A face that clipping cannot cut, because it is
  degenerate or crosses itself, falls back to a fan.
- **Normals.** They follow the mesh's smoothing angle. A corner averages
  the normals of the faces around its position that lie within the angle
  of its own face, weighted by their angles at that position. 0 gives
  flat faces; the primitives use 60 degrees, so a cube is flat and a
  sphere is smooth.
- **The mesh pipeline.** The triangles go through it with their normals
  and UVs (tangents by MikkTSpace), one submesh per material slot used.

The file is JSON in one canonical form (numbers in their shortest exact
text, -0 written as 0), so it diffs cleanly:

```json
{
  "nmesh": 1,
  "smoothing": 60,
  "positions": [[x, y, z], ...],
  "faces": [{"v": [0, 1, 2, 3], "uv": [[u, v], ...], "m": 0}, ...]
}
```

"uv" and "m" are optional. The reader checks every value:
- the version;
- finite coordinates within the mesh pipeline's limit;
- indices in range, and 3 to 256 corners a face;
- material slots below 8 (a MeshRenderer's);
- the totals.

It is a fuzz target.

The primitives (cube, plane, cylinder, sphere) are closed where they
should be, face outwards, and map UVs in [0, 1]. `cook_polymesh_extrude`
extrudes a set of faces as one region:
- the region's positions are copied and moved along the mean normal of
  the region's faces around each;
- the faces move onto the copies;
- a quad joins each boundary edge (one no other face of the region
  shares) to its copy.

A closed mesh stays closed.

## The mesh pipeline (`mesh.h`)

`cook_mesh_process` runs, in order:

1. **Validation and cleaning**: indices in range, counts in triangles,
   degenerate triangles (a repeated index or zero area) removed.
2. **Flat normals** when the source has none: each triangle gets its own
   corners with the face normal; the weld below merges the identical ones
   again.
3. **Welding**: vertices whose attributes are bitwise equal (after
   clearing attributes the mesh does not claim, and with -0 equal to +0)
   are merged, and vertices no triangle references are dropped; the
   survivors keep their order.
4. **Tangents** by the default rules of MikkTSpace (Mikkelsen, "Simulation
   of Wrinkled Surfaces Revisited", 2008), following the structure of the
   reference `mikktspace.c`: vertices identified by position, normal, and
   UV; triangles grouped around each vertex across shared edges with the
   same UV orientation, degenerate-UV triangles joining any group; each
   group's tangent the angle-weighted sum of its triangles' dP/du
   projected into the normal's plane; a vertex with two groups (a mirrored
   seam) split.
5. **Reordering** per submesh: vertex cache optimisation (Forsyth,
   "Linear-Speed Vertex Cache Optimisation", 2006, with precomputed score
   tables), overdraw reduction by sorting runs of triangles by their
   outward facing (the cluster pass of Sander, Nehab & Barczak, "Fast
   Triangle Reordering for Vertex Locality and Reduced Overdraw",
   SIGGRAPH 2007, as simplified in meshoptimizer), and vertex fetch
   optimisation (vertices in order of first use). `--no-optimize` skips
   this step.
6. **Meshlets** of at most 64 vertices and 124 triangles (§8.6.3), grown
   one triangle at a time over adjacency, after meshoptimizer's
   clusterizer (Kapoulkine): topology decides first (a triangle adding no
   vertex, then one that would be left dangling, then fewest new
   vertices), then a spatial score (distance from the meshlet's centre
   relative to the expected meshlet radius, scaled by how far the
   triangle's normal leans from the meshlet's, `meshlet_cone_weight`,
   0.25 by default); when nothing touches the meshlet, the nearest unused
   triangle is found with a k-d tree over centroids that tracks how many
   triangles each subtree still holds. The builder lives in the render
   module (`render/meshlet.h`), which also meshletises meshes made at run
   time; the cooker's `cook_mesh_build_meshlets` wraps it and gives the
   same bytes as before the move.
7. **Quantisation and writing**: 16-bit unorm positions over the mesh
   bounds, octahedral snorm16 normals, snorm8 tangents with the handedness
   in w, half-float UVs, RGBA8 colours ([asset.md](asset.md), "Typed
   views"). Every bound, of the mesh, its submeshes, and its meshlets, is
   computed from the *dequantised* positions, which are the ones the GPU
   culls and rasterises.
8. **Levels of detail** where the mesh has more than one meshlet
   (`--no-lods` skips them): the renderer's builder (`render/cluster_lod.h`;
   docs/render.md "Levels of detail") over the dequantised positions and
   the quantised attributes, so that vertices at one position whose
   records differ are a seam the levels keep. Every level's clusters are
   written in place of level 0's (level 0's first and unchanged), with the
   levels' blobs. A mesh whose levels stop at once (one meshlet, or nothing
   to simplify) is written without them. Each level's groups are built as
   jobs (docs/render.md "The build at once"). Bistro's 1.75 million
   triangles in 551 meshes take about a minute; most of its meshes get 3 to
   8 levels, and flat-shaded ones, whose every vertex is a seam corner, stop
   early. The open landscape (`benchmarks/scenes/landscape.json`, with voxels)
   cooks in 6 minutes on fifteen workers, against 20 on one thread: its pine's
   largest mesh (7.0 million triangles) in 58 s, its levels 33 of them, against
   309.
   The runtime's loaders hand the cooked meshlets and levels to the
   renderer as they are.

   With `--voxels`, a glTF mesh's aggregate groups become voxels where
   their collapses stall (docs/render.md "Voxels",
   ADR-0061), their look sampled from the mesh's materials
   (`cook/voxel.h`): the base colour (factor, vertex colour, and texture),
   metallic-roughness, occlusion, and normal textures at each ray's hit,
   through the triangle's interpolated attributes; a masked material's
   alpha against its cutoff decides each hit; blended, emissive, and unlit
   materials never voxelize. Each image a voxelized mesh needs is decoded
   once per scene into an RGBA8 mip chain of at most 1,024 texels a side,
   box-filtered (the base colour in linear light), and a hit reads it
   trilinearly at the level of the ray spacing's footprint in texture space
   (the triangle's texel density, as Akenine-Möller et al.'s ray cones do).
   Voxels stay off by default until the voxel raster draws the reference
   scenes' foliage faster than their stalled triangles (docs/render.md
   "Voxels"); editable meshes cook without them. A flat-shaded aggregate stays
   triangles: each of its faces has vertices of its own, so a meshlet fills
   at about 28 triangles and its groups stay under the 256 a group needs.

Every step is deterministic: the same input gives the same bytes.

### Cells

With `--hlod` (which implies `--voxels`), a glTF scene's static instances
are partitioned into cells of the ground plane, and each cell gets a
proxy that stands for its instances from afar: a mesh of voxels alone
(`cook/cells.h`; docs/render.md "Cell proxies"; ADR-0062).

- **The partition** (`cook_cells_partition`): squares of `--hlod-cell`
  metres (64 by default) on x and z, an instance in the square of its
  bounding sphere's centre, where every submesh of its mesh voxelizes
  (opaque or masked) and its sphere's radius is at most half an edge; the
  others (the terrain, blended or emissive meshes) are in no cell. The
  scene's `CELL` table lists each cell's instances as a range of `INST`,
  the cells in order of (z, x), the instances in no cell after them
  (docs/asset.md).
- **The proxies** (`cook_cell_proxy`, `nph_render_cell_proxy_build`): each
  in its square's space (origin at its lowest corner, on the ground
  plane), its finest cell the largest power of two at most a pixel at
  `--hlod-distance` metres (560 by default: 0.25 m in the reference views,
  4.47e-4 m a metre a pixel), sampled from each instance's cut at half a
  level's cell, with the instances of the eight squares round it and those
  of no cell that reach its 3 x 3 squares as occluders. The meshes' parts
  the proxies sample are kept as they cook (`cook_mesh_process_proxy`: the
  clusters consumed above the finest cut, with their vertices renumbered;
  every other cluster's record emptied) with their voxel materials, until
  the proxies are built; the cells are built in rounds of 32 at once on
  the job system, each logged with its instances, levels, voxels, what
  its sweep saw, and its time. A proxy is a mesh asset of voxels alone
  (`cell/<x>_<z>`, version 4) written by `cook_proxy_write`; an empty one
  writes nothing.
- **Incremental cooks.** The scene's settings hash folds in the cells'
  settings and the meshes'; the proxies are current while the scene is
  and each one's database entry and file are (a cook rebuilds them all
  otherwise, recooking in memory the meshes it needs that are current).
  Cooking a scene without `--hlod` drops its proxies.

### Tangent handedness

glTF's texture coordinates grow downwards while its normal textures have
+Y up, so the bitangent `cross(normal, tangent.xyz) * tangent.w` must
point along *decreasing* v. The cooker therefore stores w = +1 where a
triangle's UVs wind clockwise in (u, v): MikkTSpace's sign negated
(equivalently, MikkTSpace run on 1 - v). Tangent directions are the same
either way, and authored glTF tangents, which follow the same rule, are
kept as they are. A renderer reconstructs the bitangent as
`cross(n, t.xyz) * t.w` and samples normal maps without flipping green.

### The meshlet normal cone

Each meshlet stores a bounding sphere and a normal cone: an apex, a
snorm8 axis, and a snorm8 cutoff, for the backface test of §8.6.3 —
cull when `dot(normalize(apex - camera), axis) >= cutoff`. The test must
never cull a triangle that faces the camera, so the stored values are
derived for the stored, quantised axis `a` (whose length |a| is close to
but not exactly 1) rather than for the ideal one:

- Let α be the cone's half-angle around the axis's direction â: every
  triangle normal n satisfies dot(â, n) >= cos α. The cooker measures
  `mindp = min dot(a, n) = |a| cos α` directly against the quantised
  axis.
- The apex is placed on the axis behind the sphere's centre, far enough
  back (`center - a t`, t >= dot(center - p, n) / dot(a, n) for every
  triangle with a vertex p) that it lies behind the plane of every
  triangle: dot(p - apex, n) >= 0.
- For a camera at e, dot(p - e, n) = dot(p - apex, n) + dot(apex - e, n).
  The first term is non-negative by the apex's placement; the second is
  non-negative for every n in the cone when the direction v =
  normalize(apex - e) lies within 90° - α of â, that is when
  dot(v, â) >= sin α. Then every triangle is back-facing (or edge-on).
- The GPU computes dot(v, a) = |a| dot(v, â), so the cutoff to store is
  |a| sin α = sqrt(|a|² - mindp²), rounded *up* to the next snorm8 step
  (a larger cutoff culls less, never more).
- When the normals spread over about a hemisphere (mindp <= 0.1 |a|, a
  half-angle past about 84°) or the cutoff would round to 1, the cone
  cannot cull anything useful: the axis is stored as zero with cutoff
  127, which the test never passes.

The tests check the guarantee directly: for camera positions sampled
around every meshlet, of the test meshes and of the dequantised positions
of a written asset, whenever the stored cone culls, every triangle of the
meshlet faces away from the camera.

## Textures (`image.h`, `texture.h`, `bc.h`)

stb_image decodes PNG, JPEG, TGA, BMP, and Radiance HDR; DDS the cooker
reads itself (`dds.c`: the container as Direct3D documents it). Of a DDS
file it reads the top level of a 2D texture in BC1 to BC5 or BC7 (UNORM
or SRGB alike: the stored values are read as stored, and the material slot
decides their meaning, as for a PNG), BC6H unsigned (mode 11), or 8-bit
RGBA, BGRA, and BGRX, named by the DX10 header's DXGI format or by the
legacy FourCC codes and RGB masks; cube maps, arrays, volumes, and other
formats are refused as unsupported, and the levels after the first are
not read (the cooker makes its own mips). The decoded texels go through
the same pipeline as any image's. Everything after decoding is the
engine's own and deterministic. The usage decides the pipeline and the
runtime format:

| Usage | Pipeline | Format |
|---|---|---|
| `color` (base colour, emissive) | sRGB decode, mips in linear light, sRGB encode | BC7 sRGB |
| `linear` (metallic-roughness, and occlusion packed in the same image) | mips as stored | BC7 UNORM |
| `normal` | mips of vectors, renormalised per level | BC5 (X, Y; the shader rebuilds Z) |
| `mask` (occlusion on its own) | the red channel | BC4 |
| `hdr` (Radiance `.hdr`) | float mips | BC6H unsigned |

`--uncompressed` writes RGBA8 (sRGB or UNORM), RG8, R8, or RGBA16F
instead. The glTF importer assigns usages from the material slots; an
occlusion map that is the same image as the metallic-roughness map shares
its BC7 texture (glTF's ORM packing).

**Mips** are exact area averages: each destination texel is the mean of
the source region it covers, with fractional weights where an odd size
splits a texel (a 5-wide row becomes 2 texels each covering two and a
half), so every level keeps the image's mean and nothing drifts by half a
texel. The sRGB transfer functions are the exact piecewise ones
(IEC 61966-2-1); alpha is never transfer-coded.

**Encoders.** BC4 and BC5 search both endpoint modes with least-squares
refinement. BC7 evaluates modes 1, 3, and 6 for opaque blocks and 4, 5,
6, and 7 for blocks with alpha (0 and 2 on request through
`CookBc7Settings.mode_mask`): for the partitioned modes it ranks all 64
partitions with a cheap estimate (the residual of each subset around its
principal axis, from exact integer moments), fully encodes the best
`max_partitions` (8) with PCA endpoints, every p-bit combination, and
`refine_passes` (2) least-squares passes, and keeps the best; modes 4
and 5 try every rotation and (mode 4) both index selectors. BC6H encodes
unsigned half floats in mode 11 (one subset, 10-bit endpoints). The
decoders implement every BC7 mode and BC6H mode 11; they are the
reference the encoders measure their error against, and they match a real
GPU (below).

**Quality**, from the tests (`tools/cook/tests/test_bc.c`) and the
benchmark images:

- BC7 against a bound computed in the test (the best any partition can
  do when each subset is fitted by its principal line, with no
  quantisation): a 2-D colour gradient reaches the bound (27.9 dB), random
  noise stays within 0.3 dB of it, a 1-D ramp encodes at 51.7 dB, and the
  photograph-like benchmark image at 41.19 dB (RGB) and 40.30 dB (RGBA).
- BC5 normal map: 42.06 dB over X and Y; BC4 mask: 48.73 dB; BC6H over
  2⁻⁵..2⁵: 2.7 % mean relative error (mode 11 only; see Limitations).

**Against the GPU.** `tests/integration/test_bc_gpu.c` uploads blocks to
the GPU and reads back what its sampler decodes, through
`shaders/test/test_texel_fetch.comp`: random BC7 blocks of every mode,
partition, rotation, selector, and p-bit, the reserved mode, and the
encoder's own output decode exactly as our decoder does, as do BC6H mode
11 blocks. BC4 and BC5 are compared with a tolerance, because GPUs do not
all honour the specification's interpolation fractions: the RTX 3060 Ti
decodes the six-value palette of (0, 255) as 47.6, 95.3, 159.7, 207.4
where the fractions give 51, 102, 153, 204 (at most 13 % of a palette
step, the same for every block; endpoints and the fixed 0 and 255 are
exact). The tolerance (1.5 levels plus a fifth of the block's palette
step) still catches any bitstream error, which moves texels by whole
steps.

## Sounds (`sound.h`)

A WAV file (`.wav`: integer PCM of 8 to 32 bits or floats of 32 or 64,
plain or extensible, read by the audio module's reader) becomes one sound
asset, sub-asset `sound` (`<key>/sound.nph`), with the file's first sampler
loop or else the whole sound as its loop. `cooker_cook_file` takes a WAV
file, and `cooker_cook_directory` cooks every one it finds
([ADR-0057](adr/0057-sound-assets.md)). Two choices are made per sound
(`CookSoundSettings`):

- **Streaming.** AUTO streams sounds of `stream_seconds` (8) or more:
  music, ambience, long dialogue; shorter ones play from memory. ALWAYS and
  NEVER decide outright. A sound too long for memory (2^31 frames) always
  streams under AUTO.
- **The codec.** AUTO keeps every sample, whichever way the sound plays:
  PCM in memory, where nothing has to be decoded as it plays, and the
  lossless codec when it streams, about half the bytes to read and store.
  PCM and LOSSLESS ask for one; ADPCM, a quarter of 16-bit PCM and lossy
  (54 to 88 dB on tones), is for short effects whose memory counts more
  than their last bits, and plays from memory only.

Samples are kept exactly. The lossless codec takes integers of 8 to 24
bits at their depth, 32-bit integers at 24 bits when their low byte is
always zero, and floats at 24 bits when every one is a 24-bit integer over
2^23; anything else it cannot hold exactly stays PCM at its own depth,
logged. 8-bit PCM is stored as 16-bit (exactly); 64-bit floats become
32-bit (logged when that rounds a sample). ADPCM codes 16-bit samples, so
deeper ones are rounded to 16 bits first, well below its own noise.
Samples that are not numbers are refused: in a mix they would reach every
bus their voice feeds.

**Settings per source.** A JSON object beside the source, named as the
source with `.import.json` appended, overrides the cooker's defaults for
it:

```json
{"codec": "adpcm", "stream": "never"}
```

| Key | Values |
|---|---|
| `codec` | `"auto"`, `"pcm"`, `"lossless"`, `"adpcm"` |
| `stream` | `"auto"`, `"always"`, `"never"` |
| `stream_seconds` | a number above 0 |
| `exhaustive` | `true`: the lossless encoder tries every prediction order (a slower cook, a smaller file) |

Its bytes join the source's content hash, so editing it recooks the
sound; an unknown key or value fails the source rather than being ignored.
The defaults come from `nph_cook`'s `--sound-codec`, `--sound-stream`, and
`--stream-seconds` (the settings file's own words).

## Fuzzing

The cooker's inputs are hostile until proven otherwise (§8.8 "malformed
input must never crash"):

- `tests/fuzz/fuzz_gltf.c`: a `.gltf` or `.glb` with embedded data
  through the importer and, when it imports, every mesh (up to 65536
  vertices in all) through the whole pipeline; the written asset must
  pass the runtime's validation and `nph_mesh_view`, so whatever the
  cooker writes from hostile input is something the engine can load.
- `tests/fuzz/fuzz_nmesh.c`: editable meshes. One that reads must
  validate, write back to text that reads as the same mesh, stay valid
  when its faces (up to 64) are extruded, and cook, before and after, into
  a mesh the runtime can view (the check `fuzz_mesh_check.h` shares with
  the glTF target).
- `tests/fuzz/fuzz_cook_sound.c`: a settings byte, then a WAV file.
  Whatever the reader takes (up to 2^17 samples) is cooked under those
  settings, and the asset written must pass the runtime's validation, the
  sound view, and the audio module's checks of its codec data; a lossless
  sound must decode to the source's samples exactly, and a PCM one hold
  them.
- `tests/fuzz/fuzz_image.c`: PNG, JPEG, TGA, BMP, and HDR through
  `cook_image_info` (header only; at most a million texels are decoded)
  and the decoder, then images up to 64 x 64 through the texture cook with
  a usage chosen by the input, the result checked with `nph_texture_view`.
- Seeds come from `scripts/fuzz_seeds_cook.py` (called by
  `scripts/fuzz_seeds.py`), written from the format descriptions rather
  than by the libraries under test: PNGs (RGBA, grey, palette, 16-bit),
  TGA and RLE TGA, BMP, flat and RLE HDR, a minimal baseline JPEG, glTF
  documents (a triangle, a textured mesh, strips with a sparse accessor, a
  GLB), and editable meshes (a triangle, cubes, concave faces, a pyramid,
  degenerate faces). Unit tests decode every seed, so a seed that stops
  importing fails the normal build.

Every fuzz target, the asset module's included, runs in two forms
(`nph_add_fuzzer`, cmake/NphModule.cmake): the libFuzzer executable with
`-DNPH_BUILD_FUZZERS=ON` under Clang, which the nightly fuzz workflow runs
with coverage guidance, and a smoke test in every test build,
`fuzz_smoke.<name>` (label `fuzz`), which replays the seed corpus and a
fixed set of mutations of each seed (bit flips, boundary values,
truncations, removed and duplicated ranges, splices) under each
toolchain's sanitizers on every push (`tests/fuzz/fuzz_smoke_main.c`).

With `NPH_BUILD_FUZZERS`, all first-party code and the cgltf/stb unit
are compiled with `-fsanitize=fuzzer-no-link`, so libFuzzer is guided by
the coverage of the parsers themselves; before Phase 6 only the harnesses
were instrumented, which left the nightly runs close to blind. The first
instrumented runs (two minutes per target) found four problems the blind
ones never had, each now fixed with a test and its input kept in the
corpus as `regression_*`:

- JSON: `-1.5e-3333...` underflows to -0.0, which was written as `-0`
  and re-parsed as the integer 0, so writing was not a fixed point (core).
- SPIR-V: a struct naming itself among its members made the
  depth-limited type sizing exponential (seven members, depth 32); sizes
  are now memoised and such a struct is rejected (gpu).
- glTF: an index accessor whose `count` key was garbled became an empty
  index list, and the mesh writer offset into the null array (cook).
- Packs and `.nph` files: the harnesses' page-aligned copies of
  few-hundred-byte inputs let ASan's quarantine, which counts the bytes
  asked for, hold gigabytes until libFuzzer's RSS limit stopped the run;
  the copies are now whole pages (harness only).

Replaying the new regression seeds with longer local mutation runs
(`<name>_fuzz_smoke <corpus> 200000 --save-last=FILE`, which keeps the
input that crashed) found two more in the cooker, both a Debug assertion
on a zero vector: a source normal of (0, 0, 0), and one of (0, 1.8e19, 0)
whose squared length overflows, so normalising it gave zero. The math
module's `normalize_or` functions now rescale such vectors and return the
fallback for non-finite ones, the tangent code normalises every normal it
projects onto, and mesh validation refuses position coordinates beyond
`COOK_MESH_MAX_COORDINATE` (1e9), below which every edge, cross product,
and quantisation step stays finite.

The 2026-10-15 nightly run found a glTF buffer with a `byteLength` of
5.6e41 and a data URI: cgltf saturates the length to about 2^63 and
allocates it before decoding a character, which ASan aborts on (and a
smaller lie would have cost real memory). `gltf.c` now refuses any
embedded buffer longer than its base64 payload can decode, six bits per
character, before cgltf loads buffers (`regression_huge_buffer`).

Two problems the fuzzing work turned up are handled in `image.c`:
stb_image computes `NULL + 0` and calls `memcpy(NULL, p, 0)` on a PNG
whose first IDAT chunk is empty (undefined behaviour that UBSan reports),
so such files are refused before stb_image sees them; and its failure
messages can carry bytes of the input (an unknown chunk's type), so they
are logged with everything outside printable ASCII replaced.

## Testing

- `cook_tests` (95 cases): the block codecs against hand-built blocks
  from the format descriptions (BC1's palettes among them) and against
  the line-fit bound; DDS files built byte by byte in every format read,
  against the block decoders, and the malformed and unsupported refused;
  image decoding of every seed, sRGB round trips, exact-area mips (including
  odd sizes), normal renormalisation, edge replication; the mesh pipeline
  step by step (flat normals, the weld's order and attribute rules,
  MikkTSpace handedness, mirrored seams, and angle weighting, the miss
  ratio of known orders and after optimisation, meshlet limits, coverage,
  and compactness, the cone guarantee above, bounds that hold for the
  dequantised positions); glTF import of embedded and external data,
  strips and fans, per-primitive attributes, the tangent rules, sparse
  and quantised accessors, cameras and lights per node, deep hierarchies
  and repeated roots, required and used extensions, texture transforms
  baked into the coordinates and the tangents, non-finite values
  (tangents made again),
  and malformed documents; DDS sources replacing missing fallbacks; the
  specular-glossiness conversion (dielectrics, metals, grey round trips,
  textures texel by texel and of other extents) and its materials
  imported and cooked; the files a document names (decoded,
  normalised, once each, a GLB's chunk excluded, and the URIs a copy could
  not keep refused); the database's round trips, atomic saves, and
  refusals; editable meshes (closed primitives with their volumes,
  triangulation of convex, concave, star-shaped, and degenerate faces
  into their own area, normals by the smoothing angle, submeshes by
  material, the file read back and refused where it must be, extrusion
  that keeps a mesh closed, copies); the cooker's canonical and relative
  paths, identities, outputs, collisions, incremental decisions,
  stale-asset removal, directories, packs, and editable meshes cooked
  with the assets written reported; sounds: the plan by length and
  settings, every WAV encoding as PCM holding its samples and as lossless
  decoding to them to the bit, ADPCM from deeper samples, the file's loop,
  settings files parsed, refused, and recooking their source, refusals
  (samples that are not numbers, a sound too long for memory that may not
  stream), cooked sounds loaded through the VFS and a streamed one's
  samples read back at its offset; the command-line tool end to end, its
  sound options too.
- `integration_tests` (label `integration`): the GPU BC check above, and
  `test_cook_load.c`, the Phase 6 exit criterion end to end: a generated
  scene with external buffers and images, a data-URI normal map, a mesh
  without normals, a camera, and a light is cooked and packed with the job
  system encoding; a VFS mounts the pack and a loader with a job system
  loads the scene by GUID, then its meshes, materials, and textures by
  the GUIDs it finds, all asynchronously; every view resolves and every
  mip and stream is uploaded to the GPU under the validation gate.
- `sample.asset_viewer`: the sample cooks at build time, loads, renders
  120 frames, and writes the last one to `capture.bmp` in the build tree.

## Benchmarks

`benchmarks/cook/bench_cook.c`, run with the default `--min-time=0.2
--repeats=5` in the Profile configuration on the development machine
(Ryzen 7 5800X, Windows 10, MSVC 14.44, 2026-10-14). Full JSON:
`benchmarks/results/cook-2026-10-14-win-msvc-profile-ryzen7-5800x.json`.
Budgets: `benchmarks/budgets.txt`. The inputs are generated: a 128 x 128
photograph-like image (smooth shading, hard-edged discs, noise), a normal
map, a mask, an HDR image, a 256 x 256-quad height-field grid (131072
triangles), and an 8-mesh, 7.1 MiB GLB.

| Benchmark | ns/op | Per | Reading |
|---|---|---|---|
| bc7_encode_opaque | 108017 | 4x4 block | 0.15 Mpixel/s per core, 41.19 dB |
| bc7_encode_alpha | 113170 | block | 40.30 dB RGBA |
| bc7_encode_opaque_p2_r1 | 38349 | block | 2 partitions, 1 pass: 41.14 dB at 2.8x the speed |
| bc7_encode_opaque_p1 | 37698 | block | 1 partition: 41.10 dB |
| bc7_encode_opaque_mode6 | 10655 | block | mode 6 only: 38.73 dB |
| bc7_encode_opaque_mode1 | 51064 | block | mode 1 only: 41.13 dB |
| bc7_decode | 163 | block | |
| bc5_encode_normals | 1549 | block | 10 Mpixel/s, 42.06 dB |
| bc4_encode_mask | 588 | block | 27 Mpixel/s, 48.73 dB |
| bc6h_encode_hdr | 5403 | block | 3 Mpixel/s, 2.7 % mean relative error |
| texture_cook_256 | 9035 | base texel | linearise, mips, BC7, LZ HIGH on one thread |
| texture_cook_256_jobs | 1263 | base texel | 15 workers: 7.2x |
| mesh_weld | 327 | triangle | 393216 corners to 66049 vertices |
| mesh_normals_smooth | 28 | triangle | |
| mesh_tangents | 422 | triangle | MikkTSpace, no splits on a grid |
| mesh_optimize | 254 | triangle | ACMR 3.00 to 0.76 (32-entry FIFO) |
| mesh_meshlets | 552 | triangle | 1368 meshlets, 95.8 triangles each |
| mesh_process | 2104 | triangle | the whole pipeline, LZ HIGH: 2.1 s per million triangles |
| gltf_import_glb | 2534 | KiB | 400 MB/s |

What the numbers say:

- **BC7 dominates every texture cook.** The default setting spends
  108 µs per block: a 1024² colour texture with its mips takes about 9.5 s
  on one thread (1.3 s on the job system here). That is the price of fully encoding
  eight partitions per mode with every p-bit combination; two partitions
  and one refinement pass cost 0.05 dB on the benchmark image at 2.8x the
  speed. The ranking of the partitions was rebuilt during Phase 6 on exact
  integer moments with four power iterations, and the per-assignment
  palette is now computed once: together 147 → 108 µs with output
  identical at the default setting (the benchmark hashes the encoded
  blocks to show it). What remains is the subset fitting itself (about
  78 % of the time, measured with cycle counters); the next steps are
  choosing p-bits per endpoint instead of trying every combination,
  and SIMD over texels, both of which change the output and so need the
  quality tests' judgement. Meanwhile `CookBc7Settings` exposes the
  trade.
- **The texture cook parallelises 7.2x on 15 workers** at 256²; the
  serial parts are linearisation, the mip filter, and LZ compression of
  each level.
- **Meshes are cheap**: a million-triangle mesh goes through the whole
  pipeline in about two seconds on one thread, with the vertex cache at
  0.76 misses per triangle (0.5 is the limit for a regular grid) and
  meshlets 77 % full on average.

## Limitations (future work)

- BC6H uses mode 11 only (one subset): smooth HDR content is fine, sharp
  HDR edges lose precision that the partitioned modes would keep. Signed
  BC6H is not produced.
- No KTX2, Basis, or WebP input; no texture arrays or cube maps from the
  cooker (the runtime format has layers).
- Sounds: WAV only (no FLAC, Ogg, or AIFF); settings per source only, not
  per folder; floats that are not 24-bit integers stay 32-bit PCM, with no
  option yet to quantise them for the lossless codec.
- glTF: no skins, morph targets, or animations (Phase 9+ needs them); no
  TEXCOORD_1, no texture transform per texture (a material's one is
  baked), and no PBR extension materials but specular-glossiness
  (KHR_materials_transmission and the others are skipped with a
  warning).
- DDS sources are decoded and encoded again even where their blocks
  already are what the runtime would store (a BC7 colour texture), which
  costs the BC7 encoder's time.
- The cooker is single-threaded outside block encoding: sources and
  meshes cook one after another.
- No LOD generation yet: the cluster DAG of §8.6.9 is Phase 13.
