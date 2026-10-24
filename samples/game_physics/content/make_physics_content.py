# SPDX-License-Identifier: Apache-2.0
"""Writes physics.gltf, the game_physics sample's source content.

    python samples/game_physics/content/make_physics_content.py

The meshes the sample's bodies are drawn with, each the shape of its
Collider at scale 1, and the materials that tell them apart:

- mesh 0, a cube of half extents 0.5 (boxes, the platform, the stairs:
  scaled by their entities, as their Colliders are);
- mesh 1, a sphere of radius 0.5 (balls);
- mesh 2, a capsule of radius 0.3 around a core 1.2 long (the character);
- mesh 3, the ground: a 40 x 40 m slab whose top face is y = 0.

The scene holds the ground and a directional sun; the game makes the rest.
The document is self-contained (a data URI buffer) and deterministic; rerun
the script after changing it and commit both. The build cooks physics.gltf
with nph_cook (samples/CMakeLists.txt).
"""

import base64
import json
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def box(half, centre=(0.0, 0.0, 0.0), uv_scale=1.0):
    """Four vertices a face with its normal, so edges stay sharp."""
    positions, normals, uvs, triangles = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    for n, u, v in faces:
        base = len(positions)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = tuple(centre[c] + half[c] * (n[c] + su * u[c] + sv * v[c]) for c in range(3))
            positions.append(p)
            normals.append(tuple(float(x) for x in n))
            du = sum(abs(u[c]) * half[c] for c in range(3))
            dv = sum(abs(v[c]) * half[c] for c in range(3))
            uvs.append((uv_scale * (su + 1) * du, uv_scale * (sv + 1) * dv))
        triangles += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return positions, normals, uvs, triangles


def capsule(radius, half_height, segments=24, rings=8):
    """A capsule along Y (a sphere for half_height 0): two hemispheres of
    `rings` rings each and the cylinder between them."""
    positions, normals, uvs = [], [], []
    lats = []
    for k in range(rings + 1):  # the lower hemisphere, pole to equator
        lats.append((-0.5 * math.pi + 0.5 * math.pi * k / rings, -half_height))
    for k in range(rings + 1):  # the upper, equator to pole
        lats.append((0.5 * math.pi * k / rings, half_height))
    for j, (phi, centre) in enumerate(lats):
        for i in range(segments + 1):
            theta = 2.0 * math.pi * i / segments
            n = (math.cos(phi) * math.cos(theta), math.sin(phi), math.cos(phi) * math.sin(theta))
            positions.append((radius * n[0], centre + radius * n[1], radius * n[2]))
            normals.append(n)
            uvs.append((i / segments, j / (len(lats) - 1)))
    triangles = []
    row = segments + 1
    for j in range(len(lats) - 1):
        for i in range(segments):
            a = j * row + i
            b = a + 1
            c = a + row
            d = c + 1
            triangles += [(a, c, b), (b, c, d)]
    # The poles' rings collapse to points: drop the triangles with no area.
    keep = []
    for t in triangles:
        p0, p1, p2 = (positions[i] for i in t)
        if dot(cross(sub(p1, p0), sub(p2, p0)), cross(sub(p1, p0), sub(p2, p0))) > 1e-18:
            keep.append(t)
    return positions, normals, uvs, keep


def check_winding(positions, triangles, outward):
    """Every face's counter-clockwise normal agrees with `outward`."""
    for t in triangles:
        p0, p1, p2 = (positions[i] for i in t)
        n = cross(sub(p1, p0), sub(p2, p0))
        centre = tuple((p0[c] + p1[c] + p2[c]) / 3.0 for c in range(3))
        assert dot(n, outward(centre, t)) > 0.0, "a triangle winds clockwise"


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
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half)]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz]


def main():
    FLOAT, U16 = 5126, 5123
    ARRAY, ELEMENTS = 34962, 34963
    buf = Buffer()
    meshes = []

    def add_mesh(name, geometry, outward, material):
        p, n, uv, tri = geometry
        check_winding(p, tri, outward)
        pos = buf.add("3f", p, FLOAT, "VEC3", with_bounds=True, target=ARRAY)
        nrm = buf.add("3f", n, FLOAT, "VEC3", target=ARRAY)
        tex = buf.add("2f", uv, FLOAT, "VEC2", target=ARRAY)
        idx = buf.add("H", [(i,) for t in tri for i in t], U16, "SCALAR", target=ELEMENTS)
        meshes.append({"name": name, "primitives": [{
            "attributes": {"POSITION": pos, "NORMAL": nrm, "TEXCOORD_0": tex},
            "indices": idx, "material": material}]})
        return len(p), len(tri)

    def from_normals(normals):
        return lambda c, t: normals[t[0]]

    counts = []
    cube = box((0.5, 0.5, 0.5))
    counts.append(add_mesh("cube", cube, from_normals(cube[1]), 1))
    sphere = capsule(0.5, 0.0, segments=24, rings=8)
    counts.append(add_mesh("sphere", sphere, lambda c, t: c, 3))
    body = capsule(0.3, 0.6, segments=20, rings=6)

    def capsule_out(c, t):
        core = (0.0, max(-0.6, min(0.6, c[1])), 0.0)
        return sub(c, core)
    counts.append(add_mesh("capsule", body, capsule_out, 4))
    ground = box((20.0, 0.5, 20.0), centre=(0.0, -0.5, 0.0), uv_scale=0.25)
    counts.append(add_mesh("ground", ground, from_normals(ground[1]), 0))

    def material(name, rgb, roughness):
        return {"name": name, "pbrMetallicRoughness": {
            "baseColorFactor": [rgb[0], rgb[1], rgb[2], 1.0], "metallicFactor": 0.0,
            "roughnessFactor": roughness}}

    # A clear day's sun, 50 degrees up: 100 000 lux, for the sunny-day
    # exposure a Camera has by default (EV100 15) under the default sky.
    sun = quat_mul(quat_axis_angle((0.0, 1.0, 0.0), 30.0), quat_axis_angle((1.0, 0.0, 0.0), -50.0))
    doc = {
        "asset": {"version": "2.0",
                  "generator": "Nepheline samples/game_physics/content/make_physics_content.py"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": [
            {"name": "sun", "type": "directional", "color": [1.0, 0.96, 0.9],
             "intensity": 100000.0}]}},
        "scene": 0,
        "scenes": [{"name": "physics", "nodes": [0, 1]}],
        "nodes": [
            {"name": "ground", "mesh": 3},
            {"name": "sun", "rotation": [round(c, 6) for c in sun],
             "extensions": {"KHR_lights_punctual": {"light": 0}}},
        ],
        "meshes": meshes,
        "materials": [
            material("ground", (0.30, 0.33, 0.36), 0.9),
            material("box_warm", (0.85, 0.55, 0.22), 0.6),
            material("box_cool", (0.25, 0.42, 0.75), 0.6),
            material("ball", (0.78, 0.18, 0.16), 0.35),
            material("character", (0.25, 0.70, 0.35), 0.5),
            material("platform", (0.55, 0.30, 0.70), 0.5),
            material("stone", (0.62, 0.60, 0.55), 0.8),
        ],
        "accessors": buf.accessors,
        "bufferViews": buf.views,
        "buffers": [{"byteLength": len(buf.data),
                     "uri": "data:application/octet-stream;base64," +
                            base64.b64encode(bytes(buf.data)).decode("ascii")}],
    }
    path = os.path.join(HERE, "physics.gltf")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"{path}: {os.path.getsize(path)} bytes; meshes (vertices, triangles): {counts}")


if __name__ == "__main__":
    main()
