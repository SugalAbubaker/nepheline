#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Lays out the big-city reference scene (benchmarks/scenes/city.json): NVIDIA's
Emerald Square, converted from the FBX it ships as to one glTF document the
cooker takes.

    python scripts/city.py MANIFEST DIR
    python scripts/city.py --self-test

DIR/<name>/ holds the manifest's archive as `scripts/scenes.py fetch` extracted
it. The script writes DIR/<name>/<document> and a buffer beside it: every placed
mesh a node, instanced as the file instances it; every material the textures it
names, referenced where they are through MSFT_texture_dds; and the sun.

The scene's four blocks, Kwow, Sheleg, ToyHotel and Park, make a square of 153.5
m that tiles (render_perf's --tiles and the manifest's `tile_pitch`). What stands
outside that square is left out, as it closes the file's city off: the end caps'
buildings and their lights, the road's outer strips, and the trees along them.

The file is pinned by md5, so this reads what it holds and refuses anything else:
FBX 7300, triangles, normals per corner, one set of UVs by index, materials per
polygon or for the whole mesh. Everything follows from the manifest and the file:
the same inputs write the same bytes.

Why not Blender: its exporter would rewrite the DDS textures as PNG, and map the
channel-packed Specular texture by its own guess. Its glTF is not the file's.
"""
import argparse
import array
import json
import math
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# Binary FBX: the node tree (the layout is undocumented but stable; Blender's
# io_scene_fbx and Kaydara's own SDK agree on it)
# ---------------------------------------------------------------------------

FBX_MAGIC = b"Kaydara FBX Binary  \x00\x1a\x00"
FBX_VERSION = 7300  # the pinned file's; 7500 and later widen the offsets to 64 bits
ARRAY_CODES = {b"f": "f", b"d": "d", b"l": "q", b"i": "i", b"b": "b", b"c": "B"}
SCALAR_CODES = {b"Y": "<h", b"C": "<?", b"I": "<i", b"F": "<f", b"D": "<d", b"L": "<q"}


class Node:
    """An FBX node: its name, its properties, and the nodes under it."""

    __slots__ = ("name", "props", "kids")

    def __init__(self, name, props, kids):
        self.name, self.props, self.kids = name, props, kids

    def find(self, name):
        for k in self.kids:
            if k.name == name:
                return k
        return None

    def all(self, name):
        return [k for k in self.kids if k.name == name]

    def value(self, name):
        """The first property of the node named `name`, which must be there."""
        k = self.find(name)
        if k is None:
            sys.exit(f"{self.name}: no {name}")
        return k.props[0]


def read_property(data, at):
    code = data[at:at + 1]
    at += 1
    if code in SCALAR_CODES:
        fmt = SCALAR_CODES[code]
        return struct.unpack_from(fmt, data, at)[0], at + struct.calcsize(fmt)
    if code in ARRAY_CODES:
        count, encoding, size = struct.unpack_from("<III", data, at)
        at += 12
        raw = data[at:at + size]
        if encoding == 1:
            raw = zlib.decompress(raw)
        values = array.array(ARRAY_CODES[code])
        values.frombytes(raw)
        if sys.byteorder != "little":
            values.byteswap()
        if len(values) != count:
            sys.exit(f"an FBX array holds {len(values)} values, not {count}")
        return values, at + size
    if code in (b"S", b"R"):
        size = struct.unpack_from("<I", data, at)[0]
        at += 4
        raw = data[at:at + size]
        return (raw.decode("utf-8") if code == b"S" else raw), at + size
    sys.exit(f"an FBX property of type {code!r}")


def read_node(data, at):
    """The node at `at` and the offset after it; None at a list's end."""
    end, count, _size, name_size = struct.unpack_from("<IIIB", data, at)
    at += 13
    if end == 0:
        return None, at
    name = data[at:at + name_size].decode("ascii")
    at += name_size
    props = []
    for _ in range(count):
        value, at = read_property(data, at)
        props.append(value)
    kids = []
    while at < end:
        kid, at = read_node(data, at)
        if kid is None:
            break
        kids.append(kid)
    return Node(name, props, kids), end


def parse_fbx(data):
    if data[:len(FBX_MAGIC)] != FBX_MAGIC:
        sys.exit("not a binary FBX")
    version = struct.unpack_from("<I", data, len(FBX_MAGIC))[0]
    if version != FBX_VERSION:
        sys.exit(f"FBX {version}: this reads the pinned file's {FBX_VERSION}")
    at = len(FBX_MAGIC) + 4
    top = []
    while at < len(data):
        kid, at = read_node(data, at)
        if kid is None:
            break
        top.append(kid)
    return Node("", [], top)


def properties70(node):
    """A node's Properties70, name to values: the values start at the fifth field."""
    out = {}
    block = node.find("Properties70")
    if block is not None:
        for p in block.all("P"):
            out[p.props[0]] = list(p.props[4:])
    return out


def object_name(raw):
    """FBX writes a name as `name\\0\\1Class`."""
    return raw.split("\x00")[0]


# ---------------------------------------------------------------------------
# Transforms: 4 x 4 lists of rows, column vectors
# ---------------------------------------------------------------------------

IDENTITY = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def translation(v):
    return [[1.0, 0.0, 0.0, v[0]], [0.0, 1.0, 0.0, v[1]], [0.0, 0.0, 1.0, v[2]], [0.0, 0.0, 0.0, 1.0]]


def scaling(v):
    return [[v[0], 0.0, 0.0, 0.0], [0.0, v[1], 0.0, 0.0], [0.0, 0.0, v[2], 0.0], [0.0, 0.0, 0.0, 1.0]]


def rotation_xyz(degrees):
    """FBX's default order: about x first, then y, then z."""
    x, y, z = (math.radians(d) for d in degrees)
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    rx = [[1.0, 0.0, 0.0, 0.0], [0.0, cx, -sx, 0.0], [0.0, sx, cx, 0.0], [0.0, 0.0, 0.0, 1.0]]
    ry = [[cy, 0.0, sy, 0.0], [0.0, 1.0, 0.0, 0.0], [-sy, 0.0, cy, 0.0], [0.0, 0.0, 0.0, 1.0]]
    rz = [[cz, -sz, 0.0, 0.0], [sz, cz, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
    return matmul(rz, matmul(ry, rx))


def transposed_rotation(m):
    return [[m[j][i] for j in range(3)] + [0.0] for i in range(3)] + [[0.0, 0.0, 0.0, 1.0]]


def negated(v):
    return [-v[0], -v[1], -v[2]]


def local_matrix(props):
    """FBX's T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1
    (Autodesk's FbxNode documentation). The file has no geometric transforms and no
    rotation order but the default; `read_models` refuses any that appear."""

    def get(name, default):
        return props.get(name, default)

    pivot = get("RotationPivot", [0.0, 0.0, 0.0])
    scale_pivot = get("ScalingPivot", [0.0, 0.0, 0.0])
    m = translation(get("Lcl Translation", [0.0, 0.0, 0.0]))
    m = matmul(m, translation(get("RotationOffset", [0.0, 0.0, 0.0])))
    m = matmul(m, translation(pivot))
    m = matmul(m, rotation_xyz(get("PreRotation", [0.0, 0.0, 0.0])))
    m = matmul(m, rotation_xyz(get("Lcl Rotation", [0.0, 0.0, 0.0])))
    m = matmul(m, transposed_rotation(rotation_xyz(get("PostRotation", [0.0, 0.0, 0.0]))))
    m = matmul(m, translation(negated(pivot)))
    m = matmul(m, translation(get("ScalingOffset", [0.0, 0.0, 0.0])))
    m = matmul(m, translation(scale_pivot))
    m = matmul(m, scaling(get("Lcl Scaling", [1.0, 1.0, 1.0])))
    m = matmul(m, translation(negated(scale_pivot)))
    return m


def column_major(m):
    return [m[r][c] for c in range(4) for r in range(4)]


# ---------------------------------------------------------------------------
# The scene in the file
# ---------------------------------------------------------------------------


class Model:
    __slots__ = ("id", "name", "kind", "parent", "props", "geometry", "materials", "world")

    def __init__(self, node):
        self.id = node.props[0]
        self.name = object_name(node.props[1])
        self.kind = node.props[2]
        self.parent = None
        self.props = properties70(node)
        self.geometry = None
        self.materials = []
        self.world = None


def read_models(root):
    """The file's models, each with its world matrix, its geometry, and its materials in
    the order their connections list them, which LayerElementMaterial's indices name."""
    objects = root.find("Objects")
    models = {n.props[0]: Model(n) for n in objects.all("Model")}
    geometries = {n.props[0]: n for n in objects.all("Geometry")}
    materials = {n.props[0]: n for n in objects.all("Material")}
    for c in root.find("Connections").all("C"):
        if c.props[0] != "OO":
            continue
        child, parent = c.props[1], c.props[2]
        owner = models.get(parent)
        if owner is None:
            continue
        if child in models:
            models[child].parent = owner
        elif child in geometries:
            if owner.geometry is not None:
                sys.exit(f"{owner.name}: two geometries")
            owner.geometry = geometries[child]
        elif child in materials:
            owner.materials.append(child)
    for m in models.values():
        for name in ("GeometricTranslation", "GeometricRotation", "GeometricScaling", "RotationOrder"):
            if name in m.props:
                sys.exit(f"{m.name}: {name} is not supported")
        w = local_matrix(m.props)
        p = m.parent
        while p is not None:
            w = matmul(local_matrix(p.props), w)
            p = p.parent
        m.world = w
    return models


class Buffer:
    """The document's binary buffer: little-endian floats, one view per attribute."""

    def __init__(self, path):
        self.file = open(path, "wb")
        self.size = 0
        self.views = []
        self.accessors = []

    def add(self, values, kind, bounds=None):
        raw = values.tobytes()
        self.file.write(raw)
        self.views.append({"buffer": 0, "byteOffset": self.size, "byteLength": len(raw)})
        self.size += len(raw)
        count = len(values) // {"VEC2": 2, "VEC3": 3}[kind]
        accessor = {"bufferView": len(self.views) - 1, "componentType": 5126, "count": count, "type": kind}
        if bounds is not None:
            accessor["min"], accessor["max"] = bounds
        self.accessors.append(accessor)
        return len(self.accessors) - 1

    def close(self):
        self.file.close()


def layer(geometry, name, mapping, reference):
    """The geometry's first layer element `name` (UV set 0), checked to map as the file does."""
    for k in geometry.all(name):
        if k.props[0] != 0:
            continue
        got = (k.value("MappingInformationType"), k.value("ReferenceInformationType"))
        if got[0] not in mapping or got[1] != reference:
            sys.exit(f"{name}: {got} is not supported")
        return k, got[0]
    sys.exit(f"no {name} on a geometry")


def write_geometry(geometry, buf):
    """The accessors of each material slot the geometry's polygons use. FBX's UVs grow upwards,
    a glTF's downwards. Its vertices are the corners, not welded: the cooker welds."""
    positions = geometry.value("Vertices")
    polygon = geometry.value("PolygonVertexIndex")
    if len(polygon) % 3 != 0 or any(polygon[i] >= 0 or polygon[i - 1] < 0 or polygon[i - 2] < 0
                                    for i in range(2, len(polygon), 3)):
        sys.exit("a geometry has polygons that are not triangles")
    normal_layer, _ = layer(geometry, "LayerElementNormal", ("ByPolygonVertex",), "Direct")
    normals = normal_layer.value("Normals")
    uv_layer, _ = layer(geometry, "LayerElementUV", ("ByPolygonVertex",), "IndexToDirect")
    uvs = uv_layer.value("UV")
    uv_index = uv_layer.value("UVIndex")
    material_layer, mapping = layer(geometry, "LayerElementMaterial", ("AllSame", "ByPolygon"), "IndexToDirect")
    slots = material_layer.value("Materials")
    triangles = len(polygon) // 3
    if len(normals) != 3 * len(polygon) or len(uv_index) != len(polygon):
        sys.exit("a geometry's layers do not cover its corners")
    if mapping == "ByPolygon" and len(slots) != triangles:
        sys.exit("a geometry's materials do not cover its polygons")
    by_slot = {}
    for t in range(triangles):
        by_slot.setdefault(slots[0] if mapping == "AllSame" else slots[t], []).append(t)
    out = {}
    for slot in sorted(by_slot):
        p = array.array("f")
        n = array.array("f")
        uv = array.array("f")
        for t in by_slot[slot]:
            for corner in (3 * t, 3 * t + 1, 3 * t + 2):
                v = polygon[corner]
                v = 3 * (v if v >= 0 else ~v)
                p.append(positions[v])
                p.append(positions[v + 1])
                p.append(positions[v + 2])
                c = 3 * corner
                n.append(normals[c])
                n.append(normals[c + 1])
                n.append(normals[c + 2])
                u = 2 * uv_index[corner]
                uv.append(uvs[u])
                uv.append(1.0 - uvs[u + 1])
        # The bounds of the single-precision values the buffer holds, as glTF asks.
        bounds = ([min(p[k::3]) for k in range(3)], [max(p[k::3]) for k in range(3)])
        out[slot] = {
            "POSITION": buf.add(p, "VEC3", bounds),
            "NORMAL": buf.add(n, "VEC3"),
            "TEXCOORD_0": buf.add(uv, "VEC2"),
        }
    return out


# ---------------------------------------------------------------------------
# Textures: which base colours' alpha is a mask
# ---------------------------------------------------------------------------


def base_color_is_masked(path):
    """Whether a DDS's alpha cuts its surface out: BC3 blocks that cross the 0.5 cutoff. A base
    colour without any is opaque whatever its alpha says: the small constant textures of the
    file's lamps, panes and paint hold alpha 0 that no surface of theirs uses as a cutout."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"DDS " or data[84:88] != b"DXT5":
        return False
    height, width = struct.unpack_from("<II", data, 12)
    blocks = ((width + 3) // 4) * ((height + 3) // 4)
    crossing = 0
    for b in range(blocks):
        a0, a1 = data[128 + 16 * b], data[129 + 16 * b]
        if min(a0, a1) < 128 < max(a0, a1):
            crossing += 1
    return crossing > 0


# ---------------------------------------------------------------------------
# The document
# ---------------------------------------------------------------------------


def outside_tile(model, half):
    """Whether the centre of a model's bounds stands outside the tile's square."""
    positions = model.geometry.value("Vertices")
    w = model.world
    xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
    mid = ((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0, (min(zs) + max(zs)) / 2.0)
    x = w[0][0] * mid[0] + w[0][1] * mid[1] + w[0][2] * mid[2] + w[0][3]
    z = w[2][0] * mid[0] + w[2][1] * mid[1] + w[2][2] * mid[2] + w[2][3]
    return abs(x) > half or abs(z) > half


def texture_files(directory):
    """The directory's files by lower-case name: the file's paths are Windows paths."""
    return {name.lower(): name for name in sorted(os.listdir(directory))}


class Materials:
    """The document's textures and materials: the file repeats a material under numbered names
    (Painted_Metal2, Painted_Metal4), which name the same textures and become one. A name ending
    .DoubleSided is two-sided, the flag the file's Falcor materials carry there."""

    SLOTS = ("DiffuseColor", "NormalMap", "SpecularColor", "EmissiveColor")

    def __init__(self, root, texture_dir, relative_dir):
        self.dir = texture_dir
        self.relative = relative_dir
        self.files = texture_files(texture_dir)
        self.images = []
        self.textures = []
        self.materials = []
        self.by_file = {}
        self.by_textures = {}
        self.mask = {}
        objects = root.find("Objects")
        texture_nodes = {n.props[0]: n for n in objects.all("Texture")}
        self.names = {n.props[0]: object_name(n.props[1]) for n in objects.all("Material")}
        self.slots = {n.props[0]: {} for n in objects.all("Material")}
        for c in root.find("Connections").all("C"):
            if c.props[0] == "OP" and c.props[1] in texture_nodes and c.props[2] in self.slots:
                name = texture_nodes[c.props[1]].value("RelativeFilename").replace("\\", "/").rsplit("/", 1)[-1]
                self.slots[c.props[2]][c.props[3]] = name

    def texture(self, name):
        found = self.files.get(name.lower())
        if found is None:
            sys.exit(f"{name}: no such texture in {self.dir}")
        if found not in self.by_file:
            self.images.append({"uri": f"{self.relative}/{found}", "mimeType": "image/vnd-ms.dds"})
            self.textures.append({"extensions": {"MSFT_texture_dds": {"source": len(self.images) - 1}}})
            self.by_file[found] = len(self.textures) - 1
        return self.by_file[found]

    def material(self, fbx_id):
        """The document's material for the file's. Specular's green and blue are roughness and
        metalness, as glTF's metallic-roughness reads them. Its red, occlusion by the file's README,
        is zero in all 115 of them, which glTF's occlusion would read as no ambient light at all, so
        no material takes it."""
        slots = self.slots[fbx_id]
        key = tuple(slots.get(s, "").lower() for s in self.SLOTS)
        if key in self.by_textures:
            return self.by_textures[key]
        base = slots.get("DiffuseColor")
        spec = slots.get("SpecularColor")
        normal = slots.get("NormalMap")
        if base is None or spec is None or normal is None:
            sys.exit(f"{self.names[fbx_id]}: a texture is missing: {slots}")
        file_name = object_name(self.names[fbx_id])
        name = file_name.split(".")[0].rstrip("0123456789_")
        m = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": self.texture(base)},
                "metallicRoughnessTexture": {"index": self.texture(spec)},
            },
            "normalTexture": {"index": self.texture(normal)},
        }
        if "EmissiveColor" in slots:
            m["emissiveTexture"] = {"index": self.texture(slots["EmissiveColor"])}
            m["emissiveFactor"] = [1.0, 1.0, 1.0]
        if base_color_is_masked(os.path.join(self.dir, self.files[base.lower()])):
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.5
        if file_name.endswith(".DoubleSided"):
            m["doubleSided"] = True
        self.materials.append(m)
        self.by_textures[key] = len(self.materials) - 1
        return self.by_textures[key]


def sun_rotation(model):
    """The quaternion that turns glTF's light direction -Z to the file's sun: its rotation alone,
    the post-rotation being what makes FBX's -Y axis that same direction."""
    r = rotation_xyz(model.props.get("Lcl Rotation", [0.0, 0.0, 0.0]))
    trace = r[0][0] + r[1][1] + r[2][2]
    if trace > 0.0:
        s = 2.0 * math.sqrt(1.0 + trace)
        return [(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, s / 4.0]
    i = max(range(3), key=lambda k: r[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = 2.0 * math.sqrt(1.0 + r[i][i] - r[j][j] - r[k][k])
    q = [0.0, 0.0, 0.0, (r[k][j] - r[j][k]) / s]
    q[i] = s / 4.0
    q[j] = (r[j][i] + r[i][j]) / s
    q[k] = (r[k][i] + r[i][k]) / s
    return q


def convert(data, layout, texture_dir, relative_dir, document_path):
    """Writes the document and its buffer; returns its counts."""
    root = parse_fbx(data)
    models = read_models(root)
    materials = Materials(root, texture_dir, relative_dir)
    half = layout["tile"] / 2.0
    buffer_name = os.path.splitext(os.path.basename(document_path))[0] + ".bin"
    buf = Buffer(os.path.join(os.path.dirname(document_path), buffer_name))
    primitives = {}
    meshes = []
    mesh_of = {}
    nodes = []
    dropped = 0
    triangles = 0
    for model in models.values():
        if model.geometry is None:
            continue
        group = model.parent.name if model.parent is not None else ""
        if group in layout["leave_out"] or outside_tile(model, half):
            dropped += 1
            continue
        gid = model.geometry.props[0]
        if gid not in primitives:
            primitives[gid] = write_geometry(model.geometry, buf)
        slots = primitives[gid]
        table = []
        for slot in sorted(slots):
            if slot >= len(model.materials):
                sys.exit(f"{model.name}: no material {slot}")
            table.append((slot, materials.material(model.materials[slot])))
        key = (gid, tuple(table))
        if key not in mesh_of:
            meshes.append({
                "name": model.name.rstrip("0123456789_"),
                "primitives": [{"attributes": slots[slot], "material": mat} for slot, mat in table],
            })
            mesh_of[key] = len(meshes) - 1
        for slot in slots:
            triangles += buf.accessors[slots[slot]["POSITION"]]["count"] // 3
        nodes.append({"name": model.name, "mesh": mesh_of[key], "matrix": column_major(model.world)})
    buf.close()
    sun = next(m for m in models.values() if m.kind == "Light" and m.name == layout["sun"]["model"])
    light_node = next(n for n in root.find("Objects").all("NodeAttribute") if n.props[2] == "Light")
    color = properties70(light_node)["Color"]
    nodes.append({"name": "sun", "rotation": sun_rotation(sun), "extensions": {"KHR_lights_punctual": {"light": 0}}})
    document = {
        "asset": {"version": "2.0", "generator": "scripts/city.py"},
        "extensionsUsed": ["KHR_lights_punctual", "MSFT_texture_dds"],
        "extensionsRequired": ["MSFT_texture_dds"],
        "extensions": {"KHR_lights_punctual": {"lights": [
            {"name": "sun", "type": "directional", "color": color, "intensity": layout["sun"]["intensity"]},
        ]}},
        "buffers": [{"uri": buffer_name, "byteLength": buf.size}],
        "bufferViews": buf.views,
        "accessors": buf.accessors,
        "images": materials.images,
        "textures": materials.textures,
        "materials": materials.materials,
        "meshes": meshes,
        "nodes": nodes,
        "scenes": [{"name": "scene", "nodes": list(range(len(nodes)))}],
        "scene": 0,
    }
    part = document_path + ".part"
    with open(part, "w", encoding="utf-8", newline="\n") as f:
        json.dump(document, f, separators=(",", ":"), sort_keys=True)
    os.replace(part, document_path)
    return {
        "nodes": len(nodes) - 1, "left_out": dropped, "meshes": len(meshes), "materials": len(materials.materials),
        "textures": len(materials.textures), "triangles": triangles, "unique": sum(
            buf.accessors[p["POSITION"]]["count"] // 3 for g in primitives.values() for p in g.values()),
        "buffer_mb": round(buf.size / 1e6, 1),
    }


def lay_out(manifest, root):
    layout = manifest["layout"]
    directory = os.path.join(root, manifest["name"])
    source = os.path.join(directory, layout["fbx"])
    if not os.path.isfile(source):
        sys.exit(f"{source} is missing (run `scripts/scenes.py fetch` first)")
    with open(source, "rb") as f:
        data = f.read()
    return convert(data, layout, os.path.join(directory, layout["textures"]), layout["textures"],
                   os.path.join(directory, manifest["source"]["document"]))


# ---------------------------------------------------------------------------
# Self-test: an FBX written byte by byte, as the file has it
# ---------------------------------------------------------------------------


def _prop(v):
    if isinstance(v, bool):
        return b"C" + struct.pack("<?", v)
    if isinstance(v, int):
        return b"L" + struct.pack("<q", v)
    if isinstance(v, float):
        return b"D" + struct.pack("<d", v)
    if isinstance(v, str):
        raw = v.encode("utf-8")
        return b"S" + struct.pack("<I", len(raw)) + raw
    if isinstance(v, array.array):
        code = {"d": b"d", "i": b"i"}[v.typecode]
        raw = zlib.compress(v.tobytes())
        return code + struct.pack("<III", len(v), 1, len(raw)) + raw
    raise TypeError(type(v))


def _node(at, name, props=(), kids=()):
    body = b"".join(_prop(p) for p in props)
    raw_name = name.encode("ascii")
    head = 13 + len(raw_name) + len(body)
    out = b""
    here = at + head
    for k in kids:
        raw = _node(here, *k)
        out += raw
        here += len(raw)
    if kids:
        out += b"\x00" * 13
        here += 13
    return struct.pack("<IIIB", here, len(props), len(body), len(raw_name)) + raw_name + body + out


def _fbx(top):
    out = FBX_MAGIC + struct.pack("<I", FBX_VERSION)
    for t in top:
        out += _node(len(out), *t)
    return out + b"\x00" * 13


def _p(name, *values):
    return ("P", (name, name, "", "A") + values)


def _fake_fbx():
    tri = array.array("d", [0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 2.0, 0.0, 2.0])
    poly = array.array("i", [0, 1, ~2, 1, 3, ~2])
    normals = array.array("d", [0.0, 1.0, 0.0] * 6)
    uv = array.array("d", [0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0])
    uv_index = array.array("i", [0, 1, 2, 1, 3, 2])
    geometry = (
        "Geometry", (10, "\x00\x01Geometry", "Mesh"), (
            ("Vertices", (tri,)),
            ("PolygonVertexIndex", (poly,)),
            ("LayerElementNormal", (0,), (
                ("MappingInformationType", ("ByPolygonVertex",)),
                ("ReferenceInformationType", ("Direct",)),
                ("Normals", (normals,)),
            )),
            ("LayerElementUV", (0,), (
                ("MappingInformationType", ("ByPolygonVertex",)),
                ("ReferenceInformationType", ("IndexToDirect",)),
                ("UV", (uv,)),
                ("UVIndex", (uv_index,)),
            )),
            ("LayerElementMaterial", (0,), (
                ("MappingInformationType", ("ByPolygon",)),
                ("ReferenceInformationType", ("IndexToDirect",)),
                ("Materials", (array.array("i", [0, 1]),)),
            )),
        ))

    def model(i, name, kind, props):
        return ("Model", (i, name + "\x00\x01Model", kind), (("Properties70", (), props),))

    objects = [
        geometry,
        model(20, "Block", "Null", (_p("Lcl Scaling", 0.01, 0.01, 0.01),)),
        model(21, "Inside1", "Mesh", (_p("Lcl Translation", 100.0, 0.0, 0.0), _p("Lcl Rotation", 0.0, 90.0, 0.0),
                                      _p("Lcl Scaling", 100.0, 100.0, 100.0))),
        model(22, "Outside", "Mesh", (_p("Lcl Translation", 900.0, 0.0, 0.0), _p("Lcl Scaling", 100.0, 100.0, 100.0))),
        model(23, "Caps", "Null", (_p("Lcl Scaling", 0.01, 0.01, 0.01),)),
        model(24, "Cap", "Mesh", (_p("Lcl Scaling", 100.0, 100.0, 100.0),)),
        model(25, "Day", "Light", (_p("Lcl Rotation", 90.0, 0.0, 0.0), _p("PostRotation", -90.0, 0.0, 0.0))),
        ("NodeAttribute", (26, "\x00\x01NodeAttribute", "Light"), (("Properties70", (), (_p("Color", 1.0, 0.5, 0.25),)),)),
        ("Material", (30, "Red2\x00\x01Material", ""), ()),
        ("Material", (31, "Red4\x00\x01Material", ""), ()),
        ("Material", (32, "Leaf.DoubleSided\x00\x01Material", ""), ()),
        ("Texture", (40, "b\x00\x01Texture", ""), (("RelativeFilename", ("..\\Textures\\Red_BaseColor.dds",)),)),
        ("Texture", (41, "n\x00\x01Texture", ""), (("RelativeFilename", ("Textures/red_normal.dds",)),)),
        ("Texture", (42, "s\x00\x01Texture", ""), (("RelativeFilename", ("Red_Specular.dds",)),)),
        ("Texture", (43, "l\x00\x01Texture", ""), (("RelativeFilename", ("Leaf_BaseColor.dds",)),)),
    ]
    oo = [(21, 20), (22, 20), (24, 23), (10, 21), (10, 22), (10, 24), (30, 21), (32, 21), (31, 22), (32, 22), (30, 24),
          (32, 24), (26, 25)]
    op = [(40, 30, "DiffuseColor"), (41, 30, "NormalMap"), (42, 30, "SpecularColor"), (40, 31, "DiffuseColor"),
          (41, 31, "NormalMap"), (42, 31, "SpecularColor"), (43, 32, "DiffuseColor"), (41, 32, "NormalMap"),
          (42, 32, "SpecularColor")]
    connections = [("C", ("OO", a, b)) for a, b in oo] + [("C", ("OP", a, b, s)) for a, b, s in op]
    return _fbx([("Objects", (), objects), ("Connections", (), connections)])


def _write_dds(path, fourcc, alpha=(255, 255)):
    head = bytearray(128)
    head[:4] = b"DDS "
    struct.pack_into("<II", head, 12, 4, 4)
    head[84:88] = fourcc
    with open(path, "wb") as f:
        f.write(bytes(head) + bytes(alpha) + bytes(14))


def self_test():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        textures = os.path.join(d, "Textures")
        os.mkdir(textures)
        _write_dds(os.path.join(textures, "Red_BaseColor.dds"), b"DXT1")
        _write_dds(os.path.join(textures, "Red_Normal.dds"), b"ATI2")
        _write_dds(os.path.join(textures, "Red_Specular.dds"), b"DXT1")
        _write_dds(os.path.join(textures, "Leaf_BaseColor.dds"), b"DXT5", (0, 255))
        layout = {"tile": 10.0, "leave_out": ["Caps"], "sun": {"model": "Day", "intensity": 30000.0}}
        path = os.path.join(d, "out.gltf")
        counts = convert(_fake_fbx(), layout, textures, "Textures", path)
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        with open(os.path.join(d, "out.bin"), "rb") as f:
            blob = f.read()
        first = (doc, blob)
        # One mesh node inside the tile; one outside it and one in the left-out group dropped.
        assert counts["nodes"] == 1 and counts["left_out"] == 2, counts
        node = doc["nodes"][0]
        assert node["name"] == "Inside1" and node["mesh"] == 0
        # World: the block's 0.01 over the model's 100 is unit scale, the translation 1 m, a quarter turn.
        m = node["matrix"]
        assert abs(m[12] - 1.0) < 1e-9 and abs(m[13]) < 1e-9 and abs(m[14]) < 1e-9, m
        assert abs(m[0]) < 1e-9 and abs(m[2] + 1.0) < 1e-9 and abs(m[8] - 1.0) < 1e-9, m
        # Two material slots, two primitives, three corners each, not welded, UVs flipped.
        mesh = doc["meshes"][0]
        assert len(mesh["primitives"]) == 2
        a = doc["accessors"][mesh["primitives"][0]["attributes"]["POSITION"]]
        assert a["count"] == 3 and a["min"] == [0.0, 0.0, 0.0] and a["max"] == [2.0, 0.0, 2.0], a
        t = doc["accessors"][mesh["primitives"][0]["attributes"]["TEXCOORD_0"]]
        view = doc["bufferViews"][t["bufferView"]]
        uv = struct.unpack_from("<6f", blob, view["byteOffset"])
        assert uv == (0.0, 1.0, 1.0, 1.0, 0.0, 0.0), uv
        # Materials: opaque with Specular as metallic-roughness alone; the leaf masked.
        red, leaf = doc["materials"]
        assert red["name"] == "Red" and "alphaMode" not in red
        assert "metallicRoughnessTexture" in red["pbrMetallicRoughness"] and "occlusionTexture" not in red
        assert leaf["name"] == "Leaf" and leaf["alphaMode"] == "MASK" and leaf["doubleSided"] is True
        assert "doubleSided" not in red
        # Paths by the directory's own case, through the DDS extension.
        uris = [i["uri"] for i in doc["images"]]
        assert "Textures/Red_Normal.dds" in uris and all(u.startswith("Textures/") for u in uris), uris
        assert all("MSFT_texture_dds" in t["extensions"] for t in doc["textures"])
        # The sun: its rotation alone, so glTF's -Z becomes the file's direction: here x +90 sends -Z to +Y.
        sun = doc["nodes"][-1]
        q = sun["rotation"]
        assert sun["name"] == "sun" and abs(q[0] - math.sqrt(0.5)) < 1e-9 and abs(q[3] - math.sqrt(0.5)) < 1e-9, q
        assert doc["extensions"]["KHR_lights_punctual"]["lights"][0]["color"] == [1.0, 0.5, 0.25]
        # A second run writes the same bytes.
        convert(_fake_fbx(), layout, textures, "Textures", path)
        with open(path, encoding="utf-8") as f:
            assert json.load(f) == first[0]
        with open(os.path.join(d, "out.bin"), "rb") as f:
            assert f.read() == first[1]
    # Refusals: another version, not an FBX.
    for bad in (b"not an fbx", FBX_MAGIC + struct.pack("<I", 7500)):
        try:
            parse_fbx(bad)
        except SystemExit:
            continue
        raise AssertionError("refused nothing")
    # A rotation_xyz of 90 about y sends +x to -z, as the right-handed glTF frame has it.
    r = rotation_xyz([0.0, 90.0, 0.0])
    assert abs(r[2][0] + 1.0) < 1e-12
    print("city.py self-test: ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("manifest", nargs="?")
    parser.add_argument("dir", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.manifest or not args.dir:
        parser.error("MANIFEST and DIR are needed")
    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    counts = lay_out(manifest, args.dir)
    print(f"{manifest['name']}: {counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
