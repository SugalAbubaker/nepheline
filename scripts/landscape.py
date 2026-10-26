#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Lays out the open-landscape reference scene (benchmarks/scenes/landscape.json):
a valley's terrain and Poly Haven's CC0 pine-forest assets scattered over it,
as one glTF document the cooker takes.

    python scripts/landscape.py MANIFEST DIR [--views]

DIR/<name>/assets/<asset>/ holds each asset as `scripts/scenes.py fetch`
fetched it (its glTF, its buffer, its textures). The script writes
DIR/<name>/<document> and DIR/<name>/terrain.bin beside them: the document
takes every asset's meshes, materials, and textures (their buffers and images
named where they are, nothing copied), the terrain, and one node per placed
instance. With --views it prints the manifest's views instead: eyes at 1.7 m
over the terrain at the trail's points the manifest names.

Everything follows from the manifest and its seed: the same manifest lays out
the same scene, so its cooked pack and its measurements are reproducible.
"""
import argparse
import json
import math
import os
import struct
import sys

# ---------------------------------------------------------------------------
# Deterministic randomness and noise
# ---------------------------------------------------------------------------


class Pcg32:
    """PCG32 (O'Neill 2014), as core/random.h has it: the layout's every draw."""

    def __init__(self, seed, stream=0x5851F42D4C957F2D):
        self.state = 0
        self.inc = ((stream << 1) | 1) & 0xFFFFFFFFFFFFFFFF
        self.next_u32()
        self.state = (self.state + seed) & 0xFFFFFFFFFFFFFFFF
        self.next_u32()

    def next_u32(self):
        old = self.state
        self.state = (old * 6364136223846793005 + self.inc) & 0xFFFFFFFFFFFFFFFF
        xorshifted = (((old >> 18) ^ old) >> 27) & 0xFFFFFFFF
        rot = old >> 59
        return ((xorshifted >> rot) | (xorshifted << ((-rot) & 31))) & 0xFFFFFFFF

    def uniform(self, lo=0.0, hi=1.0):
        return lo + (hi - lo) * (self.next_u32() / 4294967296.0)


def hash2(ix, iz, seed):
    """A lattice point's value in [0, 1): an integer hash (Wang 1997)."""
    h = (ix * 0x8DA6B343 + iz * 0xD8163841 + seed * 0xCB1AB31F) & 0xFFFFFFFF
    h = ((h ^ 61) ^ (h >> 16)) & 0xFFFFFFFF
    h = (h * 9) & 0xFFFFFFFF
    h ^= h >> 4
    h = (h * 0x27D4EB2D) & 0xFFFFFFFF
    h ^= h >> 15
    return h / 4294967296.0


def value_noise(x, z, seed):
    """Smooth value noise on the unit lattice, in [0, 1)."""
    ix, iz = math.floor(x), math.floor(z)
    fx, fz = x - ix, z - iz
    sx, sz = fx * fx * (3.0 - 2.0 * fx), fz * fz * (3.0 - 2.0 * fz)
    a = hash2(ix, iz, seed)
    b = hash2(ix + 1, iz, seed)
    c = hash2(ix, iz + 1, seed)
    d = hash2(ix + 1, iz + 1, seed)
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz


def fbm(x, z, seed, octaves):
    """Fractional Brownian motion of value noise, about [-0.5, 0.5]."""
    total, amp, freq, norm = 0.0, 1.0, 1.0, 0.0
    for o in range(octaves):
        total += amp * (value_noise(x * freq, z * freq, seed + o * 101) - 0.5)
        norm += amp
        amp *= 0.5
        freq *= 2.0
    return total / norm


# ---------------------------------------------------------------------------
# The terrain
# ---------------------------------------------------------------------------


class Terrain:
    """A valley along z: its floor `floor_width` wide, its sides rising as a
    parabola to `side_height` at the edges, rolling hills of `hills` metres
    over it (fBm on a `hill_scale` lattice), and the floor sloping down the
    valley by `fall` metres over its length."""

    def __init__(self, t, seed):
        self.width, self.length = t["size"]
        self.step = t["step"]
        self.floor_width = t["floor_width"]
        self.side_height = t["side_height"]
        self.hills = t["hills"]
        self.hill_scale = t["hill_scale"]
        self.fall = t["fall"]
        self.seed = seed

    def height(self, x, z):
        half = self.width * 0.5
        floor = self.floor_width * 0.5
        across = max(abs(x) - floor, 0.0) / max(half - floor, 1e-6)
        sides = self.side_height * across * across
        rolling = self.hills * fbm(x / self.hill_scale, z / self.hill_scale, self.seed, 5)
        # The trail's floor is gentler: the hills fade within the floor.
        rolling *= 0.35 + 0.65 * min(across * 2.0, 1.0)
        return sides + rolling - self.fall * (z / self.length + 0.5)

    def normal(self, x, z):
        e = self.step * 0.5
        dx = (self.height(x + e, z) - self.height(x - e, z)) / (2.0 * e)
        dz = (self.height(x, z + e) - self.height(x, z - e)) / (2.0 * e)
        n = (-dx, 1.0, -dz)
        k = 1.0 / math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
        return (n[0] * k, n[1] * k, n[2] * k)

    def mesh(self, uv_tile):
        """The grid: positions, normals, texture coordinates (a tile every
        `uv_tile` metres), and indices, packed little-endian."""
        nx = int(round(self.width / self.step)) + 1
        nz = int(round(self.length / self.step)) + 1
        x0, z0 = -self.width * 0.5, -self.length * 0.5
        pos, nrm, uv = bytearray(), bytearray(), bytearray()
        lo = [math.inf] * 3
        hi = [-math.inf] * 3
        for j in range(nz):
            z = z0 + j * self.step
            for i in range(nx):
                x = x0 + i * self.step
                y = self.height(x, z)
                p = (x, y, z)
                for k in range(3):
                    lo[k] = min(lo[k], p[k])
                    hi[k] = max(hi[k], p[k])
                pos += struct.pack("<3f", *p)
                nrm += struct.pack("<3f", *self.normal(x, z))
                uv += struct.pack("<2f", x / uv_tile, z / uv_tile)
        idx = bytearray()
        for j in range(nz - 1):
            for i in range(nx - 1):
                a = j * nx + i
                b, c, d = a + 1, a + nx, a + nx + 1
                # Counter-clockwise seen from above (+y).
                idx += struct.pack("<6I", a, c, d, a, d, b)
        return nx * nz, (nx - 1) * (nz - 1) * 2, pos, nrm, uv, idx, lo, hi


# ---------------------------------------------------------------------------
# Scattering
# ---------------------------------------------------------------------------


def poisson(rng, width, length, radius, accept, tries=24):
    """Bridson's Poisson-disk sampling ("Fast Poisson Disk Sampling in
    Arbitrary Dimensions", SIGGRAPH 2007 sketches) over the rectangle
    [-width/2, width/2] x [-length/2, length/2]: points at least `radius`
    apart, each one kept where accept(x, z) says."""
    cell = radius / math.sqrt(2.0)
    gw, gl = int(math.ceil(width / cell)), int(math.ceil(length / cell))
    grid = [-1] * (gw * gl)
    points, active = [], []

    def fits(x, z):
        gx, gz = int((x + width * 0.5) / cell), int((z + length * 0.5) / cell)
        for jz in range(max(gz - 2, 0), min(gz + 3, gl)):
            for jx in range(max(gx - 2, 0), min(gx + 3, gw)):
                k = grid[jz * gw + jx]
                if k >= 0:
                    px, pz = points[k]
                    if (px - x) ** 2 + (pz - z) ** 2 < radius * radius:
                        return False
        return True

    def add(x, z):
        gx, gz = int((x + width * 0.5) / cell), int((z + length * 0.5) / cell)
        grid[gz * gw + gx] = len(points)
        points.append((x, z))
        active.append(len(points) - 1)

    add(rng.uniform(-width * 0.5, width * 0.5), rng.uniform(-length * 0.5, length * 0.5))
    while active:
        k = active[int(rng.uniform(0.0, len(active)))] if len(active) > 1 else active[0]
        px, pz = points[k]
        placed = False
        for _ in range(tries):
            ang = rng.uniform(0.0, 2.0 * math.pi)
            dist = radius * (1.0 + rng.uniform())
            x, z = px + dist * math.cos(ang), pz + dist * math.sin(ang)
            if abs(x) < width * 0.5 and abs(z) < length * 0.5 and fits(x, z):
                add(x, z)
                placed = True
                break
        if not placed:
            active.remove(k)
    return [p for p in points if accept(*p)]


def trail_x(z, trail):
    """The trail's centre across the valley at z: a gentle meander."""
    return trail["amplitude"] * math.sin(2.0 * math.pi * z / trail["wavelength"])


# ---------------------------------------------------------------------------
# Merging the assets' documents
# ---------------------------------------------------------------------------


class Merged:
    """One glTF document assembled from many: each asset's arrays appended
    with its indices shifted, its buffers' and images' URIs made relative to
    the merged document."""

    def __init__(self):
        self.doc = {
            "asset": {"version": "2.0", "generator": "nepheline scripts/landscape.py"},
            "extensionsUsed": [],
            "extensionsRequired": [],
            "buffers": [],
            "bufferViews": [],
            "accessors": [],
            "samplers": [],
            "images": [],
            "textures": [],
            "materials": [],
            "meshes": [],
            "nodes": [],
            "scenes": [{"name": "landscape", "nodes": []}],
            "scene": 0,
        }
        self.meshes = {}  # (asset, node name) -> (merged mesh index, the node's own scale)

    def add_asset(self, asset, gltf_path, rel_dir):
        with open(gltf_path, encoding="utf-8") as f:
            g = json.load(f)
        d = self.doc
        base = {k: len(d[k]) for k in ("buffers", "bufferViews", "accessors", "samplers",
                                       "images", "textures", "materials", "meshes")}
        for b in g.get("buffers", []):
            b = dict(b)
            b["uri"] = rel_dir + "/" + b["uri"]
            d["buffers"].append(b)
        for v in g.get("bufferViews", []):
            v = dict(v)
            v["buffer"] += base["buffers"]
            d["bufferViews"].append(v)
        for a in g.get("accessors", []):
            a = json.loads(json.dumps(a))
            if "bufferView" in a:
                a["bufferView"] += base["bufferViews"]
            sp = a.get("sparse")
            if sp:
                sp["indices"]["bufferView"] += base["bufferViews"]
                sp["values"]["bufferView"] += base["bufferViews"]
            d["accessors"].append(a)
        d["samplers"].extend(g.get("samplers", []))
        for im in g.get("images", []):
            im = dict(im)
            if "uri" in im:
                im["uri"] = rel_dir + "/" + im["uri"]
            if "bufferView" in im:
                im["bufferView"] += base["bufferViews"]
            d["images"].append(im)
        for t in g.get("textures", []):
            t = dict(t)
            if "source" in t:
                t["source"] += base["images"]
            if "sampler" in t:
                t["sampler"] += base["samplers"]
            d["textures"].append(t)

        def shift_textures(node):
            if isinstance(node, dict):
                for k, v in node.items():
                    if k.endswith("Texture") and isinstance(v, dict) and "index" in v:
                        v["index"] += base["textures"]
                    shift_textures(v)
            elif isinstance(node, list):
                for v in node:
                    shift_textures(v)

        for m in g.get("materials", []):
            m = json.loads(json.dumps(m))
            shift_textures(m)
            d["materials"].append(m)
        for i, m in enumerate(g.get("meshes", [])):
            m = json.loads(json.dumps(m))
            for p in m["primitives"]:
                p["attributes"] = {k: v + base["accessors"] for k, v in p["attributes"].items()}
                if "indices" in p:
                    p["indices"] += base["accessors"]
                if "material" in p:
                    p["material"] += base["materials"]
            m["name"] = f"{asset}/{m.get('name', i)}"
            d["meshes"].append(m)
        # Meshes by the names of the asset's nodes that hold them, with the
        # nodes' uniform scale (an instance scales it in turn); their places
        # in the asset's own scene are not the layout's.
        for n in g.get("nodes", []):
            if "mesh" in n:
                scale = n.get("scale", [1.0, 1.0, 1.0])[0]
                self.meshes[(asset, n.get("name", ""))] = (base["meshes"] + n["mesh"], scale)
        for e in g.get("extensionsUsed", []):
            if e not in d["extensionsUsed"]:
                d["extensionsUsed"].append(e)
        for e in g.get("extensionsRequired", []):
            if e not in d["extensionsRequired"]:
                d["extensionsRequired"].append(e)

    def add_sun(self, sun):
        """The scene's sun (KHR_lights_punctual): a directional light whose
        node's -z travels from the sky at `elevation` degrees, from
        `azimuth` degrees (0 the valley's +z, turning to +x)."""
        el, az = math.radians(sun["elevation"]), math.radians(sun["azimuth"])
        d = (-math.cos(el) * math.sin(az), -math.sin(el), -math.cos(el) * math.cos(az))
        # The rotation taking -z to d: about cross(-z, d) by the angle between.
        ax = (d[1], -d[0], 0.0)
        n = math.sqrt(ax[0] ** 2 + ax[1] ** 2)
        ang = math.acos(max(-1.0, min(1.0, -d[2])))
        q = [ax[0] / n * math.sin(ang * 0.5), ax[1] / n * math.sin(ang * 0.5), 0.0,
             math.cos(ang * 0.5)] if n > 1e-9 else [0.0, 0.0, 0.0, 1.0]
        ext = self.doc.setdefault("extensions", {}).setdefault("KHR_lights_punctual",
                                                              {"lights": []})
        ext["lights"].append({"name": "sun", "type": "directional",
                              "color": sun.get("color", [1.0, 1.0, 1.0]),
                              "intensity": sun["intensity"]})
        if "KHR_lights_punctual" not in self.doc["extensionsUsed"]:
            self.doc["extensionsUsed"].append("KHR_lights_punctual")
        self.doc["scenes"][0]["nodes"].append(len(self.doc["nodes"]))
        self.doc["nodes"].append({"name": "sun", "rotation": [round(c, 6) for c in q],
                                  "extensions": {"KHR_lights_punctual": {
                                      "light": len(ext["lights"]) - 1}}})

    def add_node(self, name, mesh, translation, yaw=0.0, scale=1.0):
        node = {"name": name, "mesh": mesh, "translation": [round(c, 4) for c in translation]}
        if yaw != 0.0:
            node["rotation"] = [0.0, round(math.sin(yaw * 0.5), 6), 0.0,
                                round(math.cos(yaw * 0.5), 6)]
        if scale != 1.0:
            node["scale"] = [round(scale, 4)] * 3
        self.doc["scenes"][0]["nodes"].append(len(self.doc["nodes"]))
        self.doc["nodes"].append(node)


# ---------------------------------------------------------------------------
# The layout
# ---------------------------------------------------------------------------


def ground_material(m, layout):
    """The terrain's material: a Poly Haven texture set fetched with the
    assets (its colour, OpenGL-convention normal, and AO/roughness/metal)."""
    t = layout["terrain"]["texture"]
    rel = "assets/" + t["asset"] + "/textures/"
    d = m.doc
    first = len(d["images"])
    for suffix in ("diff", "nor_gl", "arm"):
        d["images"].append({"uri": f"{rel}{t['asset']}_{suffix}_{t['resolution']}.jpg"})
    sampler = len(d["samplers"])
    d["samplers"].append({"wrapS": 10497, "wrapT": 10497})
    tex = len(d["textures"])
    for i in range(3):
        d["textures"].append({"source": first + i, "sampler": sampler})
    d["materials"].append({
        "name": "terrain",
        "pbrMetallicRoughness": {"baseColorTexture": {"index": tex},
                                 "metallicRoughnessTexture": {"index": tex + 2}},
        "normalTexture": {"index": tex + 1},
        "occlusionTexture": {"index": tex + 2},
    })
    return len(d["materials"]) - 1


def add_terrain(m, terrain, layout, out_dir):
    material = ground_material(m, layout)
    count, tris, pos, nrm, uv, idx, lo, hi = terrain.mesh(layout["terrain"]["texture"]["tile"])
    with open(os.path.join(out_dir, "terrain.bin"), "wb") as f:
        for blob in (pos, nrm, uv, idx):
            f.write(blob)
    d = m.doc
    buffer = len(d["buffers"])
    d["buffers"].append({"uri": "terrain.bin", "byteLength": len(pos) + len(nrm) + len(uv) + len(idx)})
    views = []
    at = 0
    for blob in (pos, nrm, uv, idx):
        views.append(len(d["bufferViews"]))
        d["bufferViews"].append({"buffer": buffer, "byteOffset": at, "byteLength": len(blob)})
        at += len(blob)
    acc = len(d["accessors"])
    d["accessors"] += [
        {"bufferView": views[0], "componentType": 5126, "count": count, "type": "VEC3",
         "min": lo, "max": hi},
        {"bufferView": views[1], "componentType": 5126, "count": count, "type": "VEC3"},
        {"bufferView": views[2], "componentType": 5126, "count": count, "type": "VEC2"},
        {"bufferView": views[3], "componentType": 5125, "count": tris * 3, "type": "SCALAR"},
    ]
    d["meshes"].append({"name": "terrain", "primitives": [{
        "attributes": {"POSITION": acc, "NORMAL": acc + 1, "TEXCOORD_0": acc + 2},
        "indices": acc + 3, "material": material}]})
    m.add_node("terrain", len(d["meshes"]) - 1, (0.0, 0.0, 0.0))
    return tris


def scatter(m, terrain, layout, rng):
    """Every species' instances: Poisson-disk points over the terrain, kept
    off the trail and outside the clearings, each a random variant of the
    species, turned and scaled at random, sunk a little into the ground."""
    trail = layout["trail"]
    counts = {}
    for sp in layout["scatter"]:
        clear = sp.get("trail_clearance", 0.0)
        mask = sp.get("mask")

        def accept(x, z, clear=clear, mask=mask):
            if abs(x - trail_x(z, trail)) < clear:
                return False
            if mask is not None:
                v = value_noise(x / mask["scale"], z / mask["scale"], layout["seed"] + 7)
                if v < mask["below"]:
                    return False
            return True

        w, l = terrain.width - 2.0 * terrain.step, terrain.length - 2.0 * terrain.step
        points = poisson(rng, w, l, sp["spacing"], accept)
        meshes = [m.meshes[(sp["asset"], node)] for node in sp["nodes"]]
        for i, (x, z) in enumerate(points):
            mesh, own = meshes[int(rng.uniform(0.0, len(meshes)))]
            yaw = rng.uniform(0.0, 2.0 * math.pi)
            s = rng.uniform(sp["scale"][0], sp["scale"][1])
            y = terrain.height(x, z) - sp.get("sink", 0.0) * s
            m.add_node(f"{sp['asset']}_{i}", mesh, (x, y, z), yaw, s * own)
        counts[sp["asset"]] = len(points)
    return counts


def views(manifest, terrain):
    """The manifest's views with their eyes and targets placed: each named by
    a point along the trail (z, and an offset across), the eye at `eye`
    metres over the ground, looking `look` metres ahead along the trail or
    at a yaw from it."""
    layout = manifest["layout"]
    trail = layout["trail"]
    out = []
    for v in layout["views"]:
        z = v["z"]
        x = trail_x(z, trail) + v.get("across", 0.0)
        eye = (x, terrain.height(x, z) + v.get("eye", 1.7), z)
        yaw = math.radians(v.get("yaw", 0.0))
        ahead = v.get("look", 30.0)
        dz = math.cos(yaw) * ahead * v.get("direction", 1.0)
        dx = math.sin(yaw) * ahead
        tx, tz = x + dx, z + dz
        target = (tx, terrain.height(tx, tz) + v.get("eye", 1.7) + v.get("look_up", 0.0), tz)
        row = {"name": v["name"], "eye": [round(c, 3) for c in eye],
               "target": [round(c, 3) for c in target]}
        for k in ("walk", "turn", "yfov", "near"):
            if k in v:
                row[k] = v[k]
        out.append(row)
    return out


def lay_out(manifest, root):
    """Writes the manifest's document and terrain under root/<name>/ and
    returns (the document's path, terrain triangles, instances per asset)."""
    layout = manifest["layout"]
    terrain = Terrain(layout["terrain"], layout["seed"])
    out_dir = os.path.join(root, manifest["name"])
    m = Merged()
    for a in manifest["source"]["polyhaven"]["models"]:
        rel = "assets/" + a["asset"]
        path = os.path.join(out_dir, rel, a["files"][0]["path"])
        if not os.path.isfile(path):
            sys.exit(f"{path} is missing (run `scenes.py fetch` first)")
        m.add_asset(a["asset"], path, rel)
    tris = add_terrain(m, terrain, layout, out_dir)
    m.add_sun(layout["sun"])
    rng = Pcg32(layout["seed"])
    counts = scatter(m, terrain, layout, rng)
    doc = os.path.join(out_dir, manifest["source"]["document"])
    with open(doc, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m.doc, f, separators=(",", ":"))
    return doc, tris, counts


# ---------------------------------------------------------------------------
# Self-test (CTest scripts.landscape)
# ---------------------------------------------------------------------------


def _fake_asset(path, meshes, scale):
    """A glTF of `meshes` single-triangle meshes over one buffer and one
    texture, each held by a node (the first with uniform `scale`)."""
    doc = {
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "data.bin", "byteLength": 48}],
        "bufferViews": [{"buffer": 0, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 12}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                       "min": [0, 0, 0], "max": [1, 1, 0]},
                      {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}],
        "images": [{"uri": "textures/t.jpg"}],
        "samplers": [{}],
        "textures": [{"source": 0, "sampler": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
                       "normalTexture": {"index": 0}}],
        "meshes": [{"name": f"m{k}", "primitives": [{"attributes": {"POSITION": 0},
                                                     "indices": 1, "material": 0}]}
                   for k in range(meshes)],
        "nodes": [dict({"name": f"n{k}", "mesh": k}, **({"scale": [scale] * 3} if k == 0 else {}))
                  for k in range(meshes)],
        "extensionsUsed": ["KHR_texture_transform"],
    }
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)


def self_test():
    import shutil
    import tempfile

    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    root = tempfile.mkdtemp(prefix="landscape_test_")
    try:
        manifest = {
            "name": "t",
            "source": {"document": "t.gltf", "polyhaven": {"models": [
                {"asset": "a", "files": [{"path": "a.gltf"}]},
                {"asset": "b", "files": [{"path": "b.gltf"}]}]}},
            "layout": {
                "seed": 7,
                "terrain": {"size": [120.0, 160.0], "step": 4.0, "floor_width": 20.0,
                            "side_height": 30.0, "hills": 3.0, "hill_scale": 20.0, "fall": 5.0,
                            "texture": {"asset": "g", "resolution": "2k", "tile": 4.0}},
                "trail": {"amplitude": 5.0, "wavelength": 80.0},
                "sun": {"elevation": 40.0, "azimuth": 30.0, "intensity": 1000.0},
                "scatter": [
                    {"asset": "a", "nodes": ["n0"], "spacing": 6.0, "scale": [0.5, 1.5],
                     "sink": 0.2, "trail_clearance": 4.0, "mask": {"scale": 30.0, "below": 0.3}},
                    {"asset": "b", "nodes": ["n0", "n1"], "spacing": 9.0, "scale": [1.0, 2.0],
                     "trail_clearance": 2.0}],
                "views": [{"name": "v", "z": 10.0, "look": 20.0}],
            },
        }
        _fake_asset(os.path.join(root, "t", "assets", "a", "a.gltf"), 1, 0.5)
        _fake_asset(os.path.join(root, "t", "assets", "b", "b.gltf"), 2, 2.0)
        doc_path, tris, counts = lay_out(manifest, root)
        first = open(doc_path, "rb").read()
        lay_out(manifest, root)
        check(open(doc_path, "rb").read() == first, "the same manifest lays out the same scene")
        check(tris == 30 * 40 * 2, f"terrain triangles {tris}")
        d = json.loads(first)
        # The second asset's arrays follow the first's, shifted.
        check(d["buffers"][1]["uri"] == "assets/b/data.bin", "buffer URIs made relative")
        check(d["bufferViews"][2]["buffer"] == 1, "buffer views shifted")
        check(d["accessors"][2]["bufferView"] == 2, "accessors shifted")
        check(d["images"][1]["uri"] == "assets/b/textures/t.jpg", "image URIs made relative")
        check(d["textures"][1] == {"source": 1, "sampler": 1}, "textures shifted")
        mb = d["materials"][1]
        check(mb["pbrMetallicRoughness"]["baseColorTexture"]["index"] == 1 and
              mb["normalTexture"]["index"] == 1, "material textures shifted")
        prim = d["meshes"][2]["primitives"][0]
        check(prim == {"attributes": {"POSITION": 2}, "indices": 3, "material": 1},
              f"mesh primitives shifted: {prim}")
        check(d["meshes"][2]["name"] == "b/m1", "mesh names keep their asset")
        check("KHR_texture_transform" in d["extensionsUsed"], "extensions carried")
        # Every instance on the ground (less its sink), off the trail, scaled
        # by its node's own scale in turn.
        terrain = Terrain(manifest["layout"]["terrain"], 7)
        trail = manifest["layout"]["trail"]
        rules = {"a": manifest["layout"]["scatter"][0], "b": manifest["layout"]["scatter"][1]}
        seen = {"a": 0, "b": 0}
        for n in d["nodes"]:
            asset = n.get("name", "").split("_")[0]
            if asset not in rules:
                continue
            seen[asset] += 1
            sp = rules[asset]
            x, y, z = n["translation"]
            own = 0.5 if asset == "a" else (2.0 if d["meshes"][n["mesh"]]["name"].endswith("m0")
                                            else 1.0)
            s = n.get("scale", [1.0])[0] / own
            check(sp["scale"][0] - 1e-3 <= s <= sp["scale"][1] + 1e-3, f"{n['name']} scale {s}")
            check(abs(y - (terrain.height(x, z) - sp.get("sink", 0.0) * s)) < 0.01,
                  f"{n['name']} on the ground")
            check(abs(x - trail_x(z, trail)) >= sp["trail_clearance"] - 1e-6,
                  f"{n['name']} off the trail")
        check(seen == counts and seen["a"] > 10 and seen["b"] > 10, f"instances {seen} {counts}")
        # The sun's node turns -z to the light's travel: down, away from
        # its azimuth.
        sun = next(n for n in d["nodes"] if n.get("name") == "sun")
        qx, qy, qz, qw = sun["rotation"]
        # -z rotated by q: the third column of the rotation, negated.
        dz = (-(2 * (qx * qz + qy * qw)), -(2 * (qy * qz - qx * qw)),
              -(1 - 2 * (qx * qx + qy * qy)))
        el, az = math.radians(40.0), math.radians(30.0)
        want = (-math.cos(el) * math.sin(az), -math.sin(el), -math.cos(el) * math.cos(az))
        check(all(abs(dz[k] - want[k]) < 1e-5 for k in range(3)), f"sun travels {dz}, not {want}")
        v = views(manifest, terrain)[0]
        check(abs(v["eye"][1] - (terrain.height(v["eye"][0], 10.0) + 1.7)) < 0.01,
              "a view's eye 1.7 m over the ground")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    for f in failures:
        print("FAILED:", f)
    print("landscape self-test:", "ok" if not failures else f"{len(failures)} failures")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("manifest", nargs="?")
    ap.add_argument("dir", nargs="?", help="where the scenes live (scenes.py --dir)")
    ap.add_argument("--views", action="store_true", help="print the views, write nothing")
    ap.add_argument("--self-test", action="store_true", help="lay out a small scene, check it")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.manifest is None or args.dir is None:
        ap.error("a manifest and a directory, or --self-test")
    with open(args.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    if args.views:
        terrain = Terrain(manifest["layout"]["terrain"], manifest["layout"]["seed"])
        print(json.dumps(views(manifest, terrain), indent=2))
        return 0
    doc, tris, counts = lay_out(manifest, args.dir)
    print(f"{manifest['name']}: {doc}: terrain {tris} triangles; "
          + ", ".join(f"{k} {v}" for k, v in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
