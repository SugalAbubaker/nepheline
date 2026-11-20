# Exporting from Blender

This guide covers how to export from Blender so the cooker reads what you made
([ADR-0107](adr/0107-the-blender-round-trip.md)). It starts with the exporter's settings, then
what Blender's objects and materials become in the engine:
- collision;
- glass;
- what a surface reflects;
- a second UV map;
- the rays;
- how exporting again updates the editor.

The names are those of the glTF 2.0 exporter that comes with Blender 4.5 (File > Export >
glTF 2.0 (.glb/.gltf)). Its [manual](https://docs.blender.org/manual/en/4.5/addons/import_export/scene_gltf2.html)
covers the rest of what it writes.

The cook's warnings say what it did not draw and why. After an import, read them in the
editor's Console ([editor.md](editor.md) "Console").

## The export settings

| Setting | Set it to | Why |
|---|---|---|
| Format | glTF Binary (`.glb`) | One file holds the meshes, materials and images, so an export is one file for the editor to take. glTF Separate (`.gltf` with its `.bin` and images) works too: the editor copies and watches the files a `.gltf` names. |
| Include > Custom Properties | on | Collision and the rays are custom properties (below). Without this they are not exported. |
| Include > Cameras, Punctual Lights | as the scene needs | Each is placed where its object is. |
| Include > Limit to | as you like, keeping the collision proxies | A proxy hidden in the viewport is left out under Visible Objects ("Collision" below). |
| Transform > +Y Up | on (the default) | The engine is Y-up, as glTF is. |
| Data > Mesh > Apply Modifiers | on | The exporter writes modifiers only applied. |
| Data > Mesh > UVs, Normals | on | A mesh without normals gets flat ones. |
| Data > Mesh > Tangents | either | The cooker makes MikkTSpace tangents for a mesh without them, as Blender does. |
| Data > Material > Materials | Export | |
| Data > Material > Images | PNG or JPEG | The cooker reads no WebP, and Blender writes WebP without a PNG or JPEG fallback. |
| Data > Lighting > Lighting Mode | Standard | The engine reads lights in glTF's units: candela for point and spot lights, lux for the sun ([render.md](render.md) "Units and conventions"). |
| Data > Compression | off | Compressed meshes can be read only through the Draco extension, which the cooker does not implement, so the file is refused. |
| Data > Shape Keys, Skinning; Animation | off | They are skipped with a warning. Characters come with the first game (Phase 16, ADR-0102). |
| Data > Scene Graph > GPU Instances | off | The cooker does not read `EXT_mesh_gpu_instancing`, so the copies it holds would be lost. |

Tick **Remember Export Settings** so the `.blend` file keeps these settings.

**A material's sides.** Blender leaves a material's Backface Culling off unless it is ticked
(Material Properties > Settings), and the exporter then writes the material double-sided. It is
drawn from both sides, and its mesh collides from both. Tick Backface Culling for closed meshes,
whose back faces are never seen.

A file that requires an extension the cooker does not implement is refused whole, rather than
drawn wrong. One that merely uses such an extension is cooked without it, with a warning
([cook.md](cook.md) "glTF import").

## Units and orientation

Keep the scene's Unit Scale at 1 (Scene Properties > Units). A Blender unit is then a metre, as
it is in glTF and in the engine. With +Y Up, Blender's Z becomes the engine's Y. What faces
Blender's front view (-Y) faces +Z, glTF's front.

## Collision

What an object collides as is cooked with the scene ([cook.md](cook.md) "Collision"). The
runtime makes colliders of it in Phase 15's fourth step (ADR-0102). An object takes the first of
these that applies:

1. **Its proxies.** These are objects named `UCX_` followed by the object's name: `UCX_Crate`
   for `Crate`. Each proxy is one convex hull of its points, so a concave object takes several:
   `UCX_Crate_00`, `UCX_Crate_01`, and so on. Objects named `UBX_`, `USP_` and `UCP_` are taken
   as hulls too.
   - A Blender duplicate's suffix is allowed. `UCX_Crate_01.001` looks for `Crate_01.001`, then
     `Crate_01`, then `Crate`. The prefixes are matched case-sensitively.
   - A proxy can be anywhere in the hierarchy: its points are moved into its object's frame.
   - A hull keeps at most the scene's `hull_vertices` points: 32 unless its settings say
     otherwise, and up to 256.
   - Proxies are not drawn, but they must be exported. Under Include > Visible Objects, a proxy
     hidden in the viewport is left out. Show proxies as wires instead (Object Properties >
     Viewport Display > Display As: Wire).
   - A proxy that finds no object is drawn as it is, with a warning, so nothing vanishes unseen.
2. **The object's custom property `collision`** (Object Properties > Custom Properties, a
   String): `mesh`, `hull` or `none`.
   - `mesh` collides as the drawn triangles. It is for the static world (walls, floors, stairs)
     and for kinematic bodies, not for things that fall. A mesh with a double-sided material
     (Backface Culling off, above) collides from both sides, and otherwise from the triangles'
     fronts.
   - `hull` collides as one convex hull of the mesh: for props.
   - `none` turns collision off for the object, whatever the scene's settings say.
3. **The scene's import settings.** Their `collision` is `none` unless set. In the editor,
   right-click the scene in the Assets panel and choose Import Settings... ([editor.md](editor.md)
   "Import settings"). Setting `mesh` there makes every mesh of a level solid. Proxies and
   objects' properties then make the exceptions.

A flat object asked to collide as a hull, a plane say, collides as its triangles from both
sides, with a warning. Collision shapes carry no scale: the runtime scales them by the object's.

## Glass

A material is glass when its Principled BSDF's **Transmission > Weight** is 0.5 or more and its
**Alpha** is 1 ([cook.md](cook.md) "Glass"). A weight of 1 is a clear pane.

- **Keep Alpha at 1.** Below 1, Blender exports the material as blended. The cooker then keeps it
  as a surface you see through by its coverage, such as a decal or a curtain, not as glass.
- **Its colour tints what is behind it.** That colour is the Base Color, times its texture and
  the vertex colour. White is clear, and amber lamp glass or a green bottle take their colour.
- **Deeper where thicker.** Plug a Volume Absorption node into the Material Output's Volume
  socket, and give the glass a thickness with a glTF Material Output node's Thickness. The tint
  then deepens by Beer's law: the node's Color, raised to the thickness over the distance its
  Density sets (1 / Density). The glTF Material Output node is under Add > Output once Shader
  Editor Add-ons is ticked in the glTF add-on's preferences. A thickness texture is not drawn.
- **IOR** sets what the glass reflects head-on. At 1.5, Blender's default, that is
  ((1.5 - 1) / (1.5 + 1))², or 4 %.
- **What is behind is seen through, not bent.** There is no refraction, and it stays sharp at any
  roughness. Roughness blurs what the glass reflects.
- **The sun and the rays pass through glass.** A mesh that is all glass is left out of the rays
  ([cook.md](cook.md) "Rays").

## What other surfaces reflect

A dielectric reflects 4 % head-on unless its material says otherwise ([cook.md](cook.md) "glTF
import").
- **To make a surface reflect less,** lower the Principled BSDF's **Specular IOR Level** below
  0.5. The exporter writes twice the level as `KHR_materials_specular`'s factor, which scales what
  the surface reflects: at 0.25 it reflects half as much, at 0 nothing.
- **Above 0.5, or with a Specular Tint other than white,** the exporter writes the specular colour
  instead. The engine does not draw that colour, and the cook warns about it.
- **The IOR alone changes nothing on an opaque material.** The exporter writes it there only with
  the specular extension.
- **Metals** reflect their Base Color, as before.
- **Far away, every surface reflects 4 %.** Where the scene is drawn as voxels, the far field
  keeps that value ([ADR-0107](adr/0107-the-blender-round-trip.md)).

## A second UV map

Blender exports a mesh's UV maps in the order of its UV Maps list (Object Data Properties > UV
Maps), as glTF's `TEXCOORD_0`, `TEXCOORD_1`, and so on. The engine reads the first two
([cook.md](cook.md) "glTF import").

- **Keep the map the base textures use first.** Give a second map to what needs its own layout:
  baked ambient occlusion, a decal sheet, a detail texture.
- **Read the second map through a UV Map node.** An Image Texture reads it when a UV Map node
  naming it feeds its Vector input, directly or through a Mapping node. An Image Texture without
  a UV Map node reads the map the list marks for rendering (the camera icon), so keep that mark
  on the first.
- **Keep normal maps on the first map.** Tangents are the first map's. A normal map on the second
  is read in the first's frame, with a warning.
- **A third map is skipped,** with a warning, and a texture that reads it reads the first. A mesh
  none of whose textures read the second map stores none.
- **One transform per map.** A Mapping node exports as `KHR_texture_transform` (location,
  rotation about Z, scale), which the cooker bakes into the mesh's coordinates. A material keeps
  one transform per map, so its textures on one map should share it.
- **Editor materials read the first map.** That is true of materials made in the editor
  (`.nmat`).

## Out of the rays

Give a mesh a custom property `no_rays`, and the rays pass it by while it is still drawn
([cook.md](cook.md) "Rays"). It is a Boolean, ticked, under Object Data Properties > Custom
Properties: the mesh data's, not the object's, so every object using the mesh takes it. It is
for many small things that rays could not afford and would learn little from, such as the
interior's candles.

## Exporting again

Import a file once by dropping it on the editor's window. The editor copies it into the project,
cooks it, and remembers where it came from ([editor.md](editor.md) "Importing").

- **Export again to the same file.** Within a second or two the editor copies it in, cooks it,
  and draws anew what is placed ([editor.md](editor.md) "Re-import on change"). An export made
  while the editor was closed is taken when it opens. A file exported straight into the project's
  sources is taken the same way.
- **A collection exporter** (Collection Properties > Exporters, Blender 4.2 and later) keeps the
  file and the settings with the collection, so exporting again is one click.
- **What updates:** the meshes, materials, textures and collision of what is placed.
- **What does not:** the entities. An object added in Blender is not added where the scene is
  already placed, and one removed stays there. Place the scene again to have them.
- **Meshes are numbered in the file's order.** Adding or removing objects can renumber the meshes
  after them, and what is placed then draws the mesh that now has its number. After such a
  change, place the scene again.
- **To stop the watch,** set the editor's `editor.watch` cvar to 0. It holds the milliseconds
  between looks, 1000 by default, and a change takes effect with the next project.

## What is not read

- Armatures, shape keys and animation are skipped with a warning. Characters and their animation
  come with the first game (Phase 16).
- Clearcoat, sheen, iridescence, anisotropy, dispersion, diffuse transmission and the specular
  colour are warned about and not drawn.
- A node setup the exporter cannot write, such as a procedural texture, never reaches the file.
  Bake it to an image first.
- Points and loose edges are skipped.
