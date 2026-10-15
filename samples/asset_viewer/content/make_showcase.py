# SPDX-License-Identifier: Apache-2.0
"""Writes showcase.gltf, the asset_viewer sample's source content.

    python samples/asset_viewer/content/make_showcase.py

The document is self-contained: its buffer and its one image are data
URIs, so the source stays a single text file in git. It holds what the
cooker has to handle in a real scene:

- a torus with authored normals and texture coordinates, rotated under a
  parent node, with a textured material (a PNG checkerboard);
- a ground plane without normals, so the cooker generates flat ones (the
  glTF rule), with an untextured material;
- a KHR_lights_punctual directional light, which the sample lights with.

The output is deterministic; rerun the script after changing it and commit
both. The build cooks showcase.gltf with nph_cook (samples/CMakeLists.txt).
"""

import base64
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


# ---------------------------------------------------------------------------
# PNG (RFC 2083): an 8-bit RGBA image, filter 0 on every row.
# ---------------------------------------------------------------------------

def png_chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def png_rgba(width, height, pixel):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + png_chunk(b"IEND", b""))


def checker(x, y):
    """A two-colour checkerboard with a thin dark grid and a soft vertical
    gradient, so both the block encoder and the mip chain have work."""
    size = 64
    cell = ((x // 16) + (y // 16)) & 1
    shade = 0.85 + 0.15 * (y / (size - 1))
    base = (232, 176, 72) if cell else (52, 92, 168)
    if x % 16 == 0 or y % 16 == 0:
        base = (24, 24, 30)
    return bytes([min(255, int(c * shade + 0.5)) for c in base] + [255])


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def torus(major=1.0, minor=0.35, segments=32, sides=16):
    """Vertices (position, normal, uv) and counter-clockwise triangles."""
    positions, normals, uvs = [], [], []
    for j in range(sides + 1):
        v = 2.0 * math.pi * j / sides
        for i in range(segments + 1):
            u = 2.0 * math.pi * i / segments
            ring = major + minor * math.cos(v)
            positions.append((ring * math.cos(u), minor * math.sin(v), ring * math.sin(u)))
            normals.append((math.cos(v) * math.cos(u), math.sin(v), math.cos(v) * math.sin(u)))
            uvs.append((4.0 * i / segments, 1.0 * j / sides))
    triangles = []
    row = segments + 1
    for j in range(sides):
        for i in range(segments):
            a = j * row + i
            b = a + 1
            c = a + row
            d = c + 1
            triangles += [(a, c, b), (b, c, d)]
    return positions, normals, uvs, triangles


def ground(half=4.0, quads=4):
    positions, uvs = [], []
    for k in range(quads + 1):
        for i in range(quads + 1):
            x = -half + 2.0 * half * i / quads
            z = -half + 2.0 * half * k / quads
            positions.append((x, 0.0, z))
            uvs.append((x / 2.0, z / 2.0))
    triangles = []
    row = quads + 1
    for k in range(quads):
        for i in range(quads):
            a = k * row + i
            b = a + 1
            c = a + row
            d = c + 1
            triangles += [(a, c, b), (b, c, d)]
    return positions, uvs, triangles


def check_winding(positions, triangles, outward):
    """Every face's counter-clockwise normal agrees with `outward`."""
    for t in triangles:
        p0, p1, p2 = (positions[i] for i in t)
        n = cross(sub(p1, p0), sub(p2, p0))
        centre = tuple((p0[c] + p1[c] + p2[c]) / 3.0 for c in range(3))
        assert dot(n, outward(centre, t)) > 0.0, "a triangle winds clockwise"


def quat_axis_angle(axis, degrees):
    half = math.radians(degrees) / 2.0
    s = math.sin(half)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half)]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


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


def main():
    FLOAT, U16 = 5126, 5123
    ARRAY, ELEMENTS = 34962, 34963
    buf = Buffer()

    tp, tn, tuv, ttri = torus()
    check_winding(tp, ttri, lambda c, t: tn[t[0]])
    t_pos = buf.add("3f", tp, FLOAT, "VEC3", with_bounds=True, target=ARRAY)
    t_nrm = buf.add("3f", tn, FLOAT, "VEC3", target=ARRAY)
    t_uv = buf.add("2f", tuv, FLOAT, "VEC2", target=ARRAY)
    t_idx = buf.add("H", [(i,) for t in ttri for i in t], U16, "SCALAR", target=ELEMENTS)

    gp, guv, gtri = ground()
    check_winding(gp, gtri, lambda c, t: (0.0, 1.0, 0.0))
    g_pos = buf.add("3f", gp, FLOAT, "VEC3", with_bounds=True, target=ARRAY)
    g_uv = buf.add("2f", guv, FLOAT, "VEC2", target=ARRAY)
    g_idx = buf.add("H", [(i,) for t in gtri for i in t], U16, "SCALAR", target=ELEMENTS)

    png = png_rgba(64, 64, checker)
    sun = quat_mul(quat_axis_angle((0.0, 1.0, 0.0), 35.0), quat_axis_angle((1.0, 0.0, 0.0), -55.0))
    doc = {
        "asset": {"version": "2.0",
                  "generator": "Nepheline samples/asset_viewer/content/make_showcase.py"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": [
            {"name": "sun", "type": "directional", "color": [1.0, 0.96, 0.9], "intensity": 3.0}]}},
        "scene": 0,
        "scenes": [{"name": "showcase", "nodes": [0, 3]}],
        "nodes": [
            {"name": "stage", "children": [1, 2]},
            {"name": "torus", "mesh": 0, "translation": [0.0, 1.1, 0.0],
             "rotation": [round(c, 6) for c in quat_axis_angle((1.0, 0.0, 0.0), 30.0)]},
            {"name": "ground", "mesh": 1},
            {"name": "sun", "rotation": [round(c, 6) for c in sun],
             "extensions": {"KHR_lights_punctual": {"light": 0}}},
        ],
        "meshes": [
            {"name": "torus", "primitives": [{
                "attributes": {"POSITION": t_pos, "NORMAL": t_nrm, "TEXCOORD_0": t_uv},
                "indices": t_idx, "material": 0}]},
            {"name": "ground", "primitives": [{
                "attributes": {"POSITION": g_pos, "TEXCOORD_0": g_uv},
                "indices": g_idx, "material": 1}]},
        ],
        "materials": [
            {"name": "checker", "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.6}},
            {"name": "ground", "pbrMetallicRoughness": {
                "baseColorFactor": [0.32, 0.36, 0.42, 1.0], "metallicFactor": 0.0,
                "roughnessFactor": 0.9}},
        ],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"name": "checker",
                    "uri": "data:image/png;base64," + base64.b64encode(png).decode("ascii")}],
        "accessors": buf.accessors,
        "bufferViews": buf.views,
        "buffers": [{"byteLength": len(buf.data),
                     "uri": "data:application/octet-stream;base64," +
                            base64.b64encode(bytes(buf.data)).decode("ascii")}],
    }
    path = os.path.join(HERE, "showcase.gltf")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"{path}: {os.path.getsize(path)} bytes, {len(tp)} + {len(gp)} vertices, "
          f"{len(ttri)} + {len(gtri)} triangles, a {len(png)}-byte PNG")


if __name__ == "__main__":
    main()
