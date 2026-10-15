# SPDX-License-Identifier: Apache-2.0
"""Writes pbr_scene.gltf, the Phase 7 renderer's source content.

    python samples/pbr_viewer/content/make_pbr_scene.py

It exercises every material feature the forward reference renderer
implements (CLAUDE.md §8.6 stage 2), through the cooker, so the
golden-image tests (tests/golden) and the pbr_viewer sample cover the
whole path from glTF to pixels:

- tiles: a base colour, a tangent-space normal map, and an
  occlusion-roughness-metallic image (occlusion packed with
  metallic-roughness, as glTF allows), on a box, a sphere, and a mirrored
  copy of the box (negative scale: the tangent frame flips);
- an emissive panel (an emissive texture and KHR_materials_emissive_strength;
  the renderer reads emissive radiance in cd/m^2);
- a perforated grate: alpha MASK, double-sided;
- a glass pane: alpha BLEND, smooth;
- an unlit marker (KHR_materials_unlit);
- a row of gold spheres from smooth to rough, without textures;
- a ground with a separate occlusion map (encoded on its own, BC4);
- a low sun, a warm point light, and a cold spot light
  (KHR_lights_punctual, in lux and candela).

The textures are procedural and the output deterministic; rerun the script
after changing it and commit both. The build cooks pbr_scene.gltf with
nph_cook (samples/CMakeLists.txt).
"""

import base64
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 128  # texture extent


# ---------------------------------------------------------------------------
# PNG (RFC 2083): 8-bit RGBA, filter 0 on every row.
# ---------------------------------------------------------------------------

def png_chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def png_rgba(width, height, pixel):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(bytes(max(0, min(255, int(round(c * 255.0)))) for c in pixel(x, y)))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + png_chunk(b"IEND", b""))


def data_uri(png):
    return "data:image/png;base64," + base64.b64encode(png).decode("ascii")


# ---------------------------------------------------------------------------
# Procedural textures (values in [0, 1]; colour images are sRGB-encoded)
# ---------------------------------------------------------------------------

TILES = 4  # tiles across a texture
GROUT = 0.06  # grout width, in tiles


def hash01(i, j):
    """A deterministic value in [0, 1) per tile."""
    h = (i * 73856093) ^ (j * 19349663) ^ 0x5BD1E995
    h = (h * 2654435761) & 0xFFFFFFFF
    return (h >> 8) / float(1 << 24)


def tile_height(u, v):
    """1 on a tile's face, 0 in the grout, with a rounded bevel between."""
    fu = (u * TILES) % 1.0
    fv = (v * TILES) % 1.0
    d = min(fu, 1.0 - fu, fv, 1.0 - fv)  # distance to the tile's edge, in tiles
    if d <= GROUT * 0.5:
        return 0.0
    t = min(1.0, (d - GROUT * 0.5) / 0.08)
    return math.sin(t * math.pi * 0.5)


def tile_index(u, v):
    return int(u * TILES) % TILES, int(v * TILES) % TILES


def tile_albedo(x, y):
    u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
    h = tile_height(u, v)
    i, j = tile_index(u, v)
    k = 0.85 + 0.15 * hash01(i, j)
    face = (0.86 * k, 0.80 * k, 0.70 * k)  # warm ceramic
    grout = (0.22, 0.21, 0.20)
    return tuple(grout[c] + (face[c] - grout[c]) * h for c in range(3)) + (1.0,)


def tile_normal(x, y):
    """Tangent-space normals from the height field's slopes: +X towards
    growing u, +Y towards decreasing v (glTF), +Z out of the surface."""
    e = 1.0 / SIZE
    u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
    dhdu = (tile_height(u + e, v) - tile_height(u - e, v)) / (2.0 * e)
    dhdv = (tile_height(u, v + e) - tile_height(u, v - e)) / (2.0 * e)
    strength = 0.02  # bevel depth, in UV units
    n = (-dhdu * strength, dhdv * strength, 1.0)
    length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
    return tuple(0.5 + 0.5 * c / length for c in n) + (1.0,)


def tile_orm(x, y):
    """R: ambient occlusion (dark in the grout), G: roughness (glossy
    glaze, rough grout), B: metallic (none)."""
    u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
    h = tile_height(u, v)
    i, j = tile_index(u, v)
    rough = 0.22 + 0.12 * hash01(j, i)
    return (0.35 + 0.65 * h, 0.95 + (rough - 0.95) * h, 0.0, 1.0)


def emissive_pattern(x, y):
    """Concentric rings fading towards the edges, cyan and amber."""
    u, v = (x + 0.5) / SIZE - 0.5, (y + 0.5) / SIZE - 0.5
    r = math.sqrt(u * u + v * v)
    ring = 0.5 + 0.5 * math.cos(r * 2.0 * math.pi * 6.0)
    fade = max(0.0, 1.0 - 2.0 * r)
    a = ring * fade
    return (0.15 + 0.85 * a, 0.55 * a + 0.25 * (1.0 - a), 0.9 * (1.0 - a) * fade, 1.0)


def grate(x, y):
    """A perforated metal sheet: round holes (alpha 0) on a square grid."""
    u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
    cu = (u * 8.0) % 1.0 - 0.5
    cv = (v * 8.0) % 1.0 - 0.5
    hole = math.sqrt(cu * cu + cv * cv) < 0.32
    return (0.55, 0.57, 0.6, 0.0 if hole else 1.0)


def ground_albedo(x, y):
    u, v = (x + 0.5) / 64.0, (y + 0.5) / 64.0
    c = 0.40 if ((int(u * 8) + int(v * 8)) & 1) else 0.34
    return (c, c * 1.02, c * 1.05, 1.0)


def ground_occlusion(x, y):
    """Soft dark rings, so the occlusion slot (alone: BC4) is visible."""
    u, v = (x + 0.5) / 64.0 - 0.5, (y + 0.5) / 64.0 - 0.5
    r = math.sqrt(u * u + v * v)
    a = 0.75 + 0.25 * math.cos(r * 2.0 * math.pi * 5.0)
    return (a, a, a, 1.0)


# ---------------------------------------------------------------------------
# Geometry: (positions, normals, uvs, triangles), counter-clockwise outside
# ---------------------------------------------------------------------------

def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def grid_quads(rows, cols, base=0):
    """Two counter-clockwise triangles per quad of a grid whose u grows to
    the right and v downwards seen from outside."""
    tris = []
    stride = cols + 1
    for j in range(rows):
        for i in range(cols):
            a = base + j * stride + i
            b = a + 1
            c = a + stride
            d = c + 1
            tris += [(a, c, d), (a, d, b)]
    return tris


def face(centre, right, up, hr, hu, n=1, uv_scale=1.0):
    normal = cross(right, up)
    positions, normals, uvs = [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            s, t = i / n, j / n
            p = tuple(centre[c] + right[c] * hr * (2.0 * s - 1.0) + up[c] * hu * (1.0 - 2.0 * t)
                      for c in range(3))
            positions.append(p)
            normals.append(normal)
            uvs.append((s * uv_scale, t * uv_scale))
    return positions, normals, uvs, grid_quads(n, n)


def merge(parts):
    positions, normals, uvs, tris = [], [], [], []
    for p, nrm, uv, t in parts:
        base = len(positions)
        positions += p
        normals += nrm
        uvs += uv
        tris += [(a + base, b + base, c + base) for a, b, c in t]
    return positions, normals, uvs, tris


def box(sx, sy, sz):
    hx, hy, hz = sx * 0.5, sy * 0.5, sz * 0.5
    return merge([
        face((hx, 0, 0), (0, 0, -1), (0, 1, 0), hz, hy),
        face((-hx, 0, 0), (0, 0, 1), (0, 1, 0), hz, hy),
        face((0, hy, 0), (1, 0, 0), (0, 0, -1), hx, hz),
        face((0, -hy, 0), (1, 0, 0), (0, 0, 1), hx, hz),
        face((0, 0, hz), (1, 0, 0), (0, 1, 0), hx, hy),
        face((0, 0, -hz), (-1, 0, 0), (0, 1, 0), hx, hy),
    ])


def sphere(radius, segments=48, rings=24):
    positions, normals, uvs = [], [], []
    for j in range(rings + 1):
        theta = math.pi * j / rings
        for i in range(segments + 1):
            phi = 2.0 * math.pi * i / segments
            n = (math.sin(theta) * math.cos(phi), math.cos(theta), -math.sin(theta) * math.sin(phi))
            positions.append(tuple(radius * c for c in n))
            normals.append(n)
            uvs.append((2.0 * i / segments, 1.0 * j / rings))
    tris = []
    stride = segments + 1
    for j in range(rings):
        for i in range(segments):
            a = j * stride + i
            b = a + 1
            c = a + stride
            d = c + 1
            if j + 1 < rings:
                tris.append((a, c, d))
            if j > 0:
                tris.append((a, d, b))
    return positions, normals, uvs, tris


def check_winding(mesh, name):
    positions, normals, _, tris = mesh
    for t in tris:
        p0, p1, p2 = (positions[i] for i in t)
        g = cross(sub(p1, p0), sub(p2, p0))
        n = tuple(normals[t[0]][c] + normals[t[1]][c] + normals[t[2]][c] for c in range(3))
        assert dot(g, n) > 0.0, f"{name}: a triangle winds clockwise"


# ---------------------------------------------------------------------------
# The document
# ---------------------------------------------------------------------------

class Buffer:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def add(self, fmt, rows, component, kind, with_bounds=False, target=None):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        for r in rows:
            self.data.extend(struct.pack("<" + fmt, *r))
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(self.data) - offset}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        accessor = {"bufferView": len(self.views) - 1, "componentType": component,
                    "count": len(rows), "type": kind}
        if with_bounds:
            accessor["min"] = [round(min(r[c] for r in rows), 6) for c in range(len(rows[0]))]
            accessor["max"] = [round(max(r[c] for r in rows), 6) for c in range(len(rows[0]))]
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def quat_axis_angle(axis, degrees):
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return [round(axis[0] * s, 6), round(axis[1] * s, 6), round(axis[2] * s, 6),
            round(math.cos(half), 6)]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [round(c, 6) for c in (aw * bx + ax * bw + ay * bz - az * by,
                                  aw * by - ax * bz + ay * bw + az * bx,
                                  aw * bz + ax * by - ay * bx + az * bw,
                                  aw * bw - ax * bx - ay * by - az * bz)]


def main():
    FLOAT, U16 = 5126, 5123
    ARRAY, ELEMENTS = 34962, 34963
    buf = Buffer()

    def add_mesh(mesh, name):
        check_winding(mesh, name)
        positions, normals, uvs, tris = mesh
        assert len(positions) < 65536
        return {
            "POSITION": buf.add("3f", positions, FLOAT, "VEC3", with_bounds=True, target=ARRAY),
            "NORMAL": buf.add("3f", normals, FLOAT, "VEC3", target=ARRAY),
            "TEXCOORD_0": buf.add("2f", uvs, FLOAT, "VEC2", target=ARRAY),
        }, buf.add("H", [(i,) for t in tris for i in t], U16, "SCALAR", target=ELEMENTS)

    geo = {
        "tile_box": add_mesh(box(1.2, 1.2, 1.2), "tile box"),
        "tile_sphere": add_mesh(sphere(0.7), "tile sphere"),
        "panel": add_mesh(box(1.4, 1.0, 0.12), "panel"),
        "grate": add_mesh(face((0, 0, 0), (1, 0, 0), (0, 1, 0), 0.8, 0.8), "grate"),
        "glass": add_mesh(face((0, 0, 0), (1, 0, 0), (0, 1, 0), 0.6, 0.8), "glass"),
        "marker": add_mesh(box(0.3, 0.3, 0.3), "marker"),
        "gold": add_mesh(sphere(0.35, 32, 16), "gold sphere"),
        "ground": add_mesh(face((0, 0, 0), (1, 0, 0), (0, 0, -1), 8.0, 8.0, n=8, uv_scale=4.0),
                           "ground"),
    }

    images = [
        ("tile albedo", png_rgba(SIZE, SIZE, tile_albedo)),
        ("tile normal", png_rgba(SIZE, SIZE, tile_normal)),
        ("tile orm", png_rgba(SIZE, SIZE, tile_orm)),
        ("emissive rings", png_rgba(SIZE, SIZE, emissive_pattern)),
        ("grate", png_rgba(SIZE, SIZE, grate)),
        ("ground albedo", png_rgba(64, 64, ground_albedo)),
        ("ground occlusion", png_rgba(64, 64, ground_occlusion)),
    ]
    # texture i samples image i with sampler 0 (trilinear, repeat).
    materials = [
        {"name": "tiles",
         "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                  "metallicRoughnessTexture": {"index": 2},
                                  "metallicFactor": 1.0, "roughnessFactor": 1.0},
         "normalTexture": {"index": 1, "scale": 1.0},
         "occlusionTexture": {"index": 2, "strength": 1.0}},
        {"name": "emissive panel",
         "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.05, 0.05, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.4},
         "emissiveFactor": [1.0, 1.0, 1.0], "emissiveTexture": {"index": 3},
         "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 1200.0}}},
        {"name": "grate",
         "pbrMetallicRoughness": {"baseColorTexture": {"index": 4}, "metallicFactor": 1.0,
                                  "roughnessFactor": 0.45},
         "alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True},
        {"name": "glass",
         "pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.85, 0.9, 0.3],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.05},
         "alphaMode": "BLEND", "doubleSided": True},
        {"name": "marker",
         "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.25, 0.1, 1.0]},
         "extensions": {"KHR_materials_unlit": {}}},
        {"name": "ground",
         "pbrMetallicRoughness": {"baseColorTexture": {"index": 5}, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.85},
         "occlusionTexture": {"index": 6, "strength": 0.8}},
    ]
    gold_first = len(materials)
    for k in range(5):
        materials.append({"name": f"gold {k}",
                          "pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.766, 0.336, 1.0],
                                                   "metallicFactor": 1.0,
                                                   "roughnessFactor": round(0.1 + 0.2 * k, 3)}})

    meshes = []
    mesh_index = {}

    def mesh(name, geo_name, material):
        attributes, indices = geo[geo_name]
        mesh_index[name] = len(meshes)
        meshes.append({"name": name, "primitives": [
            {"attributes": attributes, "indices": indices, "material": material}]})

    mesh("tile box", "tile_box", 0)
    mesh("tile sphere", "tile_sphere", 0)
    mesh("panel", "panel", 1)
    mesh("grate", "grate", 2)
    mesh("glass", "glass", 3)
    mesh("marker", "marker", 4)
    mesh("ground", "ground", 5)
    for k in range(5):
        mesh(f"gold {k}", "gold", gold_first + k)  # shares the sphere's accessors

    nodes = [
        {"name": "ground", "mesh": mesh_index["ground"]},
        {"name": "tile box", "mesh": mesh_index["tile box"], "translation": [-2.4, 0.6, -0.2],
         "rotation": quat_axis_angle((0, 1, 0), 25.0)},
        {"name": "tile box (mirrored)", "mesh": mesh_index["tile box"],
         "translation": [-2.4, 0.6, 1.9], "rotation": quat_axis_angle((0, 1, 0), -25.0),
         "scale": [-1.0, 1.0, 1.0]},
        {"name": "tile sphere", "mesh": mesh_index["tile sphere"], "translation": [-0.7, 0.7, 0.5]},
        {"name": "panel", "mesh": mesh_index["panel"], "translation": [0.9, 0.75, -1.3],
         "rotation": quat_axis_angle((0, 1, 0), -15.0)},
        {"name": "grate", "mesh": mesh_index["grate"], "translation": [2.5, 0.85, 0.2],
         "rotation": quat_axis_angle((0, 1, 0), -40.0)},
        {"name": "glass", "mesh": mesh_index["glass"], "translation": [0.9, 0.8, 1.7],
         "rotation": quat_axis_angle((0, 1, 0), 10.0)},
        {"name": "marker", "mesh": mesh_index["marker"], "translation": [0.0, 0.15, 2.9]},
    ]
    for k in range(5):
        nodes.append({"name": f"gold {k}", "mesh": mesh_index[f"gold {k}"],
                      "translation": [round(-2.0 + 1.0 * k, 6), 0.35, -2.9]})
    # glTF lights shine along their node's -Z.
    # The sun 28 degrees above the horizon, behind and to the left of a
    # camera looking down -Z: it travels along about (0.45, -0.47, -0.76).
    sun_rot = quat_mul(quat_axis_angle((0, 1, 0), -30.0), quat_axis_angle((1, 0, 0), -28.0))
    spot_rot = quat_axis_angle((1, 0, 0), -90.0)  # straight down
    light_nodes = [
        {"name": "sun", "rotation": sun_rot, "extensions": {"KHR_lights_punctual": {"light": 0}}},
        {"name": "lamp", "translation": [0.3, 1.1, 0.6],
         "extensions": {"KHR_lights_punctual": {"light": 1}}},
        {"name": "spot", "translation": [-2.4, 3.2, -0.2], "rotation": spot_rot,
         "extensions": {"KHR_lights_punctual": {"light": 2}}},
    ]
    first_light = len(nodes)
    nodes += light_nodes

    doc = {
        "asset": {"version": "2.0",
                  "generator": "Nepheline samples/pbr_viewer/content/make_pbr_scene.py"},
        "extensionsUsed": ["KHR_lights_punctual", "KHR_materials_emissive_strength",
                           "KHR_materials_unlit"],
        "extensions": {"KHR_lights_punctual": {"lights": [
            {"name": "sun", "type": "directional", "color": [1.0, 0.82, 0.62], "intensity": 20000.0},
            {"name": "lamp", "type": "point", "color": [1.0, 0.62, 0.32], "intensity": 1500.0,
             "range": 8.0},
            {"name": "spot", "type": "spot", "color": [0.55, 0.75, 1.0], "intensity": 25000.0,
             "spot": {"innerConeAngle": 0.25, "outerConeAngle": 0.42}},
        ]}},
        "scene": 0,
        "scenes": [{"name": "pbr", "nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "textures": [{"source": i, "sampler": 0} for i in range(len(images))],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"name": name, "uri": data_uri(png)} for name, png in images],
        "accessors": buf.accessors,
        "bufferViews": buf.views,
        "buffers": [{"byteLength": len(buf.data),
                     "uri": "data:application/octet-stream;base64," +
                            base64.b64encode(bytes(buf.data)).decode("ascii")}],
    }
    assert first_light == 13
    path = os.path.join(HERE, "pbr_scene.gltf")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"{path}: {os.path.getsize(path)} bytes, {len(meshes)} meshes, {len(nodes)} nodes, "
          f"{len(images)} images")


if __name__ == "__main__":
    main()
