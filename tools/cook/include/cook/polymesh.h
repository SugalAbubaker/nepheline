// SPDX-License-Identifier: Apache-2.0
#pragma once

// Editable meshes (CLAUDE.md §8.13 "Model creation: primitives (cube,
// sphere, plane, cylinder) and basic mesh editing (vertex/face transform,
// extrude), saved as cooked meshes"; docs/cook.md "Editable meshes";
// docs/adr/0027-editable-meshes.md): a polygon mesh the editor creates and
// edits, kept as a `.nmesh` source that the cooker cooks like any other.
//
// A CookPolyMesh is positions and faces. A face is a loop of corners,
// counter-clockwise seen from its front, each corner a position and a UV
// of its own, so faces meet with seams where their UVs differ. Faces have
// three corners or more and may be concave or not quite planar. They are
// triangulated by ear clipping in the plane facing their Newell normal
// (Eberly, "Triangulation by Ear Clipping", 2002; Newell's method from
// Sutherland, Sproull & Schumacker, "A Characterization of Ten
// Hidden-Surface Algorithms", 1974), with a fan for convex faces. Normals
// follow a smoothing angle: a corner's normal averages the normals of the
// faces around its position that are within the angle of its own face's,
// weighted by their angles at that position. 0 gives flat faces.
//
// The `.nmesh` file is JSON (core/json.h), written in one canonical form so
// it diffs cleanly in git:
//
//     {
//       "nmesh": 1,
//       "smoothing": 60,
//       "positions": [[x, y, z], ...],
//       "faces": [{"v": [0, 1, 2, 3], "uv": [[u, v], ...], "m": 0}, ...]
//     }
//
// "uv" (one per corner) and "m" (the material slot, 0 to
// COOK_POLYMESH_MATERIALS - 1) are optional. Reading checks every value:
// the version, finite numbers within COOK_MESH_MAX_COORDINATE, indices in
// range, corner counts, and the totals below. Malformed input gives
// NPH_RESULT_PARSE_ERROR (NPH_RESULT_VERSION_MISMATCH for another
// version), never a crash: the reader is a fuzz target (tests/fuzz).
//
// Thread-safety: a mesh belongs to one thread.

#include <nepheline/core/array.h>
#include <nepheline/core/string.h>
#include <nepheline/math/vec.h>

#include <cook/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOK_POLYMESH_VERSION          1U
#define COOK_POLYMESH_MAX_POSITIONS    (1U << 20U)
#define COOK_POLYMESH_MAX_FACES        (1U << 20U)
#define COOK_POLYMESH_MAX_CORNERS      (1U << 22U)
#define COOK_POLYMESH_MAX_FACE_CORNERS 256U  // ear clipping is cubic in a face's corners
#define COOK_POLYMESH_MATERIALS        8U    // a MeshRenderer's material slots
#define COOK_POLYMESH_SMOOTHING        60.0F // degrees: the primitives' default

typedef struct CookPolyFace {
    u32 first;    // its first corner in the mesh's corner arrays
    u32 count;    // corners, 3 .. COOK_POLYMESH_MAX_FACE_CORNERS
    u32 material; // slot, < COOK_POLYMESH_MATERIALS
} CookPolyFace;

typedef NPH_ARRAY(NphVec3) CookVec3Array;
typedef NPH_ARRAY(NphVec2) CookVec2Array;
typedef NPH_ARRAY(CookPolyFace) CookPolyFaceArray;

typedef struct CookPolyMesh {
    NphAllocator *alloc;
    CookVec3Array positions;
    CookPolyFaceArray faces;
    NphArrayU32 corners; // per corner: its position
    CookVec2Array uvs;   // per corner
    f32 smoothing;       // degrees, 0..180
} CookPolyMesh;

// Empty, with the default smoothing. No allocation until something is added.
void cook_polymesh_init(CookPolyMesh *m, NphAllocator *alloc);
void cook_polymesh_free(CookPolyMesh *m);
// Drops the positions and faces, keeping the storage.
void cook_polymesh_clear(CookPolyMesh *m);
// A copy of `src` in `out` (initialised here, with `alloc`).
NPH_MUST_USE NphResult cook_polymesh_copy(const CookPolyMesh *src, NphAllocator *alloc,
                                          CookPolyMesh *out);

NPH_MUST_USE NphResult cook_polymesh_add_position(CookPolyMesh *m, NphVec3 position,
                                                  u32 *out_index);
// A face of `count` corners on `positions` (indices into the mesh), with
// `uvs` per corner (NULL: zeros) and a material slot.
// NPH_RESULT_INVALID_ARGUMENT for a count out of range, an index out of
// range, a material past the slots, or too many corners in all.
NPH_MUST_USE NphResult cook_polymesh_add_face(CookPolyMesh *m, const u32 *positions,
                                              const NphVec2 *uvs, u32 count, u32 material,
                                              u32 *out_face);

// Checks what the reader checks. NPH_RESULT_CORRUPT on the first problem.
NPH_MUST_USE NphResult cook_polymesh_validate(const CookPolyMesh *m);

// ---------------------------------------------------------------------------
// The `.nmesh` file
// ---------------------------------------------------------------------------

// Reads a document from `text` into `out` (initialised here).
NPH_MUST_USE NphResult cook_polymesh_read(NphAllocator *alloc, NphStr text, CookPolyMesh *out);
// As cook_polymesh_read from the file at `path`; the file's errors too.
NPH_MUST_USE NphResult cook_polymesh_load(NphAllocator *alloc, const char *path, CookPolyMesh *out);
// Appends the document to `out`, numbers in their shortest exact form.
NPH_MUST_USE NphResult cook_polymesh_write(const CookPolyMesh *m, NphStrBuilder *out);
// Writes the document to `path`, replacing it whole (`path` + ".part"
// renamed over it).
NPH_MUST_USE NphResult cook_polymesh_save(const CookPolyMesh *m, const char *path);

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// The face's Newell normal, unit length, or zero for a degenerate face.
NphVec3 cook_polymesh_face_normal(const CookPolyMesh *m, u32 face);
// The mean of its corners' positions.
NphVec3 cook_polymesh_face_center(const CookPolyMesh *m, u32 face);
// Its area: half the length of its Newell vector (exact for planar faces).
f32 cook_polymesh_face_area(const CookPolyMesh *m, u32 face);

// Triangulates face `face`: writes (count - 2) * 3 corner offsets within the
// face (0 .. count - 1) to `out`, counter-clockwise as the face. Returns the
// triangle count. A face ear clipping cannot cut (degenerate, or crossing
// itself) falls back to a fan.
u32 cook_polymesh_triangulate_face(const CookPolyMesh *m, u32 face, u32 *out);

// The cooker's intermediate form (mesh.h): every face triangulated, with
// normals by the smoothing angle and the corners' UVs, a submesh per
// material slot used, in slot order. `out` is initialised here; its name is
// `name`.
NPH_MUST_USE NphResult cook_polymesh_to_mesh(const CookPolyMesh *m, const char *name,
                                             CookMesh *out);

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

// Extrudes the faces `faces` (indices, each once) as one region: their
// positions are duplicated and moved `distance` along the mean normal of
// the region's faces around each, the faces move onto the new positions,
// and a quad joins every boundary edge of the region (an edge no other
// face of the region shares) to its new copy, facing out of it. Returns
// the number of side faces added in `out_sides` (may be NULL); the side
// faces come after every existing face. NPH_RESULT_INVALID_ARGUMENT for a
// face out of range; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult cook_polymesh_extrude(CookPolyMesh *m, const u32 *faces, u32 count,
                                             f32 distance, u32 *out_sides);

// ---------------------------------------------------------------------------
// Primitives: centred on the origin, +Y up, faces outwards, UVs in [0, 1]
// with v growing down the image (glTF's convention). Each clears `m` first
// and sets the default smoothing.
// ---------------------------------------------------------------------------

// Six quads on eight positions; each face maps the whole texture.
NPH_MUST_USE NphResult cook_polymesh_cube(CookPolyMesh *m, f32 size);
// A square in the XZ plane facing +Y, `subdivisions` (>= 1) quads a side.
NPH_MUST_USE NphResult cook_polymesh_plane(CookPolyMesh *m, f32 size, u32 subdivisions);
// `segments` (>= 3) side quads around Y, and a cap polygon at each end.
NPH_MUST_USE NphResult cook_polymesh_cylinder(CookPolyMesh *m, f32 radius, f32 height,
                                              u32 segments);
// A UV sphere: `segments` (>= 3) around Y and `rings` (>= 2) from pole to
// pole; triangles at the poles and quads between.
NPH_MUST_USE NphResult cook_polymesh_sphere(CookPolyMesh *m, f32 radius, u32 segments, u32 rings);

#ifdef __cplusplus
}
#endif
