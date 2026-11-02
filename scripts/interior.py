#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Lays out the many-light interior reference scene (benchmarks/scenes/interior.json):
Intel's Sponza with its 10,000 candles, two glTF documents joined into the one the
cooker takes.

    python scripts/interior.py MANIFEST DIR
    python scripts/interior.py --self-test

DIR/<name>/ holds the manifest's two archives as `scripts/scenes.py fetch` extracted
them. The script writes DIR/<name>/<document> beside them, naming their buffers and
textures where they are: the palace's nodes, and under them the candles' mesh and the
10,000 nodes of their one point light.

The candles' mesh holds all 10,000 of them, 19.5 million triangles, one run of vertices
and of indices after another. With the manifest's `candle_chunk_metres` it is written as
one mesh per cube of that size (each candle in the cube its first vertex lies in), the
same triangles in the same places in a buffer of this script's: a mesh that size takes
the cooker more memory than the machines it runs on may have, and all of a scene's
candles at once are of no use to the culling either.

The palace's own lights all have an intensity of 0 in the pinned file (they are its
3ds Max scene's, without their units), and are left out; the candles light it. The
flame's material is the only emissive one and has no strength of its own: it takes
the manifest's `flame_luminance`, what a 1 cd flame about 1 by 3 cm is.

Everything follows from the manifest and the files, and the same inputs write the
same bytes. The files are pinned by md5, so this refuses anything else they might hold.
"""
import argparse
import array
import json
import os
import sys
import tempfile


def fail(message):
    sys.exit(f"interior.py: {message}")


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def strip_lights(doc):
    """The palace's lights go, each refused unless its intensity is 0."""
    lights = doc.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
    for light in lights:
        if light.get("intensity", 1.0) != 0.0:
            fail(f"a palace light with an intensity: {light}")
    for node in doc["nodes"]:
        ext = node.get("extensions")
        if ext is not None:
            ext.pop("KHR_lights_punctual", None)
            if not ext:
                del node["extensions"]
    doc.get("extensions", {}).pop("KHR_lights_punctual", None)
    if not doc.get("extensions"):
        doc.pop("extensions", None)
    doc["extensionsUsed"] = [e for e in doc.get("extensionsUsed", []) if e != "KHR_lights_punctual"]


def prefix_uris(doc, folder):
    """The document's files, named from where the joined document lies."""
    for kind in ("buffers", "images"):
        for item in doc.get(kind, []):
            uri = item.get("uri")
            if uri is None or uri.startswith("data:"):
                fail(f"a {kind} entry without a file: {item}")
            item["uri"] = f"{folder}/{uri}"


def append(base, extra, key):
    """`extra`'s array `key` after `base`'s; its first index there."""
    first = len(base.setdefault(key, []))
    base[key].extend(extra.get(key, []))
    return first


def accessor_view(doc, buffer, accessor_index, typecode, per_element):
    """An accessor's values as an array of `typecode`, read from `buffer` (bytes)."""
    accessor = doc["accessors"][accessor_index]
    view = doc["bufferViews"][accessor["bufferView"]]
    if "byteStride" in view or accessor.get("byteOffset", 0) != 0:
        fail(f"an accessor of the candles that is not packed: {accessor}")
    begin = view.get("byteOffset", 0)
    values = array.array(typecode)
    values.frombytes(buffer[begin:begin + accessor["count"] * per_element * values.itemsize])
    if sys.byteorder != "little":
        values.byteswap()
    return values


def split_candles(doc, buffer, chunk_metres, candle_count, out_uri):
    """Bytes of a new buffer, and `doc` rewritten to one mesh per cube of `chunk_metres`.

    Each primitive holds a run of indices per candle, all of a length (the vertices'
    runs are not: the file's exporter merged what it could). A run's vertices move with
    it, from the lowest it names to the highest.
    """
    if len(doc.get("meshes", [])) != 1:
        fail(f"{len(doc.get('meshes', []))} candle meshes, the pinned file has one")
    streams = []
    for prim in doc["meshes"][0]["primitives"]:
        attrs = prim["attributes"]
        indices = doc["accessors"][prim["indices"]]["count"]
        if indices % candle_count != 0:
            fail(f"a primitive of {indices} indices is not {candle_count} candles alike")
        streams.append({
            "material": prim["material"],
            "indices": indices // candle_count,
            "position": accessor_view(doc, buffer, attrs["POSITION"], "f", 3),
            "normal": accessor_view(doc, buffer, attrs["NORMAL"], "f", 3),
            "uv": accessor_view(doc, buffer, attrs["TEXCOORD_0"], "f", 2),
            "index": accessor_view(doc, buffer, prim["indices"], "I", 1),
        })

    # Each run's cube, by its first index's vertex: the cubes hold (cube, stream) runs.
    low = [min(min(st["position"][a::3]) for st in streams) for a in range(3)]
    chunks = {}
    for which, st in enumerate(streams):
        n = st["indices"]
        position = st["position"]
        for k in range(candle_count):
            at = st["index"][k * n] * 3
            cube = tuple(int((position[at + a] - low[a]) / chunk_metres) for a in range(3))
            chunks.setdefault(cube, {}).setdefault(which, []).append(k)

    out = bytearray()
    views = []
    accessors = []

    def add(values, component, kind, target_min=None, target_max=None):
        while len(out) % 4 != 0:
            out.append(0)
        begin = len(out)
        out.extend(values.tobytes())
        views.append({"buffer": 0, "byteLength": len(out) - begin, "byteOffset": begin})
        accessor = {"bufferView": len(views) - 1, "componentType": component,
                    "count": len(values) // {"VEC3": 3, "VEC2": 2, "SCALAR": 1}[kind], "type": kind}
        if target_min is not None:
            accessor["min"] = target_min
            accessor["max"] = target_max
        accessors.append(accessor)
        return len(accessors) - 1

    meshes = []
    for cube in sorted(chunks):
        primitives = []
        for which in sorted(chunks[cube]):
            st = streams[which]
            n = st["indices"]
            pos = array.array("f")
            nor = array.array("f")
            uvs = array.array("f")
            idx = array.array("I")
            placed = 0
            for k in chunks[cube][which]:
                run = st["index"][k * n:(k + 1) * n]
                first = min(run)
                last = max(run)
                pos.extend(st["position"][first * 3:(last + 1) * 3])
                nor.extend(st["normal"][first * 3:(last + 1) * 3])
                uvs.extend(st["uv"][first * 2:(last + 1) * 2])
                idx.extend(array.array("I", (i - first + placed for i in run)))
                placed += last + 1 - first
            lo = [min(pos[a::3]) for a in range(3)]
            hi = [max(pos[a::3]) for a in range(3)]
            primitives.append({
                "attributes": {"POSITION": add(pos, 5126, "VEC3", lo, hi),
                               "NORMAL": add(nor, 5126, "VEC3"),
                               "TEXCOORD_0": add(uvs, 5126, "VEC2")},
                "indices": add(idx, 5125, "SCALAR"),
                "material": st["material"],
            })
        name = f"{doc['meshes'][0].get('name', 'candles')}_{cube[0]}_{cube[1]}_{cube[2]}"
        meshes.append({"name": name, "primitives": primitives})

    # The node that held the mesh holds them, its transform theirs.
    holder = next(n for n in doc["nodes"] if "mesh" in n)
    del holder["mesh"]
    children = holder.setdefault("children", [])
    for i, mesh in enumerate(meshes):
        children.append(len(doc["nodes"]))
        doc["nodes"].append({"name": mesh["name"], "mesh": i})
    doc["meshes"] = meshes
    doc["accessors"] = accessors
    doc["bufferViews"] = views
    doc["buffers"] = [{"uri": out_uri, "byteLength": len(out)}]
    return bytes(out)


def decals_out_of_the_rays(doc):
    """A mesh wholly of blended materials blocks no ray: the palace's dirt, sheets a hair in
    front of its walls, would otherwise put the walls behind them in shadow."""
    materials = doc.get("materials", [])
    for mesh in doc.get("meshes", []):
        prims = mesh["primitives"]
        if prims and all(materials[p["material"]].get("alphaMode") == "BLEND" for p in prims
                         if "material" in p) and all("material" in p for p in prims):
            mesh["extras"] = {"no_rays": True}


def join(palace, candles, palace_dir, candles_dir, flame_luminance):
    """The candles' document into the palace's, whose indices it shifts. Both are changed."""
    strip_lights(palace)
    decals_out_of_the_rays(palace)
    prefix_uris(palace, palace_dir)
    prefix_uris(candles, candles_dir)
    first_buffer = append(palace, candles, "buffers")
    for view in candles.get("bufferViews", []):
        view["buffer"] += first_buffer
    first_view = append(palace, candles, "bufferViews")
    for accessor in candles.get("accessors", []):
        accessor["bufferView"] += first_view
    first_accessor = append(palace, candles, "accessors")
    flames = 0
    for material in candles.get("materials", []):
        if any(v > 0.0 for v in material.get("emissiveFactor", [0.0, 0.0, 0.0])):
            material.setdefault("extensions", {})["KHR_materials_emissive_strength"] = {
                "emissiveStrength": flame_luminance
            }
            flames += 1
    if flames != 1:
        fail(f"{flames} emissive candle materials, the pinned file has one")
    first_material = append(palace, candles, "materials")
    for mesh in candles.get("meshes", []):
        # Rays could not afford 19.5 million triangles of candles, and need not see
        # them: the many lights carry the flames' light (docs/cook.md "Rays").
        mesh["extras"] = {"no_rays": True}
        for prim in mesh["primitives"]:
            prim["attributes"] = {k: v + first_accessor for k, v in prim["attributes"].items()}
            prim["indices"] += first_accessor
            prim["material"] += first_material
    first_mesh = append(palace, candles, "meshes")
    first_node = len(palace["nodes"])
    lights = candles["extensions"]["KHR_lights_punctual"]["lights"]
    for node in candles["nodes"]:
        if "mesh" in node:
            node["mesh"] += first_mesh
        if "children" in node:
            node["children"] = [c + first_node for c in node["children"]]
    palace["nodes"].extend(candles["nodes"])
    palace.setdefault("extensions", {})["KHR_lights_punctual"] = {"lights": lights}
    used = palace.setdefault("extensionsUsed", [])
    for ext in ("KHR_lights_punctual", "KHR_materials_emissive_strength"):
        if ext not in used:
            used.append(ext)
    roots = candles["scenes"][candles.get("scene", 0)]["nodes"]
    palace["scenes"][palace.get("scene", 0)]["nodes"] += [r + first_node for r in roots]
    palace["asset"]["generator"] = "scripts/interior.py"
    return palace


def main_layout(manifest, root):
    m = load(manifest)
    layout = m["layout"]
    path = os.path.join(root, m["name"])
    palace_doc = load(os.path.join(path, layout["palace"]))
    candles_doc = load(os.path.join(path, layout["candles"]))
    chunk_metres = layout.get("candle_chunk_metres")
    if chunk_metres is not None:
        candles_dir = os.path.join(path, os.path.dirname(layout["candles"]))
        uri = candles_doc["buffers"][0]["uri"]
        with open(os.path.join(candles_dir, uri), "rb") as f:
            buffer = f.read()
        count = sum(1 for n in candles_doc["nodes"] if "KHR_lights_punctual" in n.get("extensions", {}))
        split = split_candles(candles_doc, buffer, chunk_metres, count, "candles_split.bin")
        with open(os.path.join(candles_dir, "candles_split.bin"), "wb") as f:
            f.write(split)
        del buffer
    joined = join(palace_doc, candles_doc, os.path.dirname(layout["palace"]),
                  os.path.dirname(layout["candles"]), layout["flame_luminance"])
    out = os.path.join(path, m["source"]["document"])
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(joined, f, separators=(",", ":"), sort_keys=True)
    nodes = joined["nodes"]
    lit = sum(1 for n in nodes if "KHR_lights_punctual" in n.get("extensions", {}))
    print(f"{m['name']}: {out}: {len(nodes)} nodes, {lit} lights, {len(joined['meshes'])} meshes")
    return 0


def split_self_test():
    """Four candles, two with a third vertex, in two cubes: the same triangles in the same places."""
    positions = array.array("f")
    index = array.array("I")
    corners = []
    base = 0
    for k, (x, extra) in enumerate(((0.5, 1), (3.5, 0), (0.6, 0), (3.6, 1))):
        vertices = [(x, 0.0), (x, 1.0)] + ([(x, 2.0)] if extra else [])
        for vx, vy in vertices:
            positions.extend([vx, vy, 0.0])
        # A triangle's worth of indices a candle, naming its last vertex.
        run = [base, base + 1, base + len(vertices) - 1]
        index.extend(run)
        corners.extend((round(positions[i * 3], 3), positions[i * 3 + 1]) for i in run)
        base += len(vertices)
    count = base
    normals = array.array("f", [0.0, 0.0, 1.0] * count)
    uvs = array.array("f", [0.25, 0.75] * count)
    buffer = positions.tobytes() + normals.tobytes() + uvs.tobytes() + index.tobytes()
    lengths = [len(positions.tobytes()), len(normals.tobytes()), len(uvs.tobytes()),
               len(index.tobytes())]
    offsets = [0, lengths[0], lengths[0] + lengths[1], lengths[0] + lengths[1] + lengths[2]]

    def document():
        return {
            "nodes": [{"name": "holder", "mesh": 0, "translation": [1.0, 2.0, 3.0]}],
            "meshes": [{"name": "c", "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3,
                 "material": 7}]}],
            "accessors": [{"bufferView": 0, "count": count}, {"bufferView": 1, "count": count},
                          {"bufferView": 2, "count": count}, {"bufferView": 3, "count": 12}],
            "bufferViews": [{"buffer": 0, "byteOffset": o, "byteLength": n}
                            for o, n in zip(offsets, lengths)],
            "buffers": [{"uri": "c.bin"}],
        }

    doc = document()
    out = split_candles(doc, buffer, 2.0, 4, "split.bin")
    assert len(doc["meshes"]) == 2
    assert doc["buffers"] == [{"uri": "split.bin", "byteLength": len(out)}]
    holder = doc["nodes"][0]
    assert "mesh" not in holder and holder["translation"] == [1.0, 2.0, 3.0]
    assert holder["children"] == [1, 2]
    assert [n["mesh"] for n in doc["nodes"][1:]] == [0, 1]
    seen = []
    for mesh in doc["meshes"]:
        prim = mesh["primitives"][0]
        assert prim["material"] == 7
        pos = accessor_view(doc, out, prim["attributes"]["POSITION"], "f", 3)
        idx = accessor_view(doc, out, prim["indices"], "I", 1)
        assert len(idx) == 6, "each cube has two candles"
        assert max(idx) < len(pos) // 3, "indices start again in each mesh"
        accessor = doc["accessors"][prim["attributes"]["POSITION"]]
        assert accessor["min"][1] == 0.0
        for i in idx:
            seen.append((round(pos[i * 3], 3), pos[i * 3 + 1]))
    assert sorted(seen) == sorted(corners), "the same triangles in the same places"
    # What is not a run per candle is refused.
    try:
        split_candles(document(), buffer, 2.0, 5, "split.bin")
    except SystemExit as e:
        assert "not 5 candles alike" in str(e)
    else:
        raise AssertionError("twelve indices split among five candles")


def self_test():
    """Two small documents joined: every index shifted, the lights' left as they should be."""
    palace = {
        "asset": {"version": "2.0"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": [{"type": "point", "intensity": 0.0}]}},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "a", "mesh": 0, "children": [1]},
                  {"name": "b", "extensions": {"KHR_lights_punctual": {"light": 0}}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
        "accessors": [{"bufferView": 0}, {"bufferView": 1}],
        "bufferViews": [{"buffer": 0}, {"buffer": 0}],
        "buffers": [{"uri": "p.bin"}],
        "materials": [{"name": "stone"}],
        "images": [{"uri": "textures/x.png"}],
    }
    candles = {
        "asset": {"version": "2.0"},
        "extensionsUsed": ["KHR_lights_punctual"],
        "extensions": {"KHR_lights_punctual": {"lights": [{"type": "point", "intensity": 1}]}},
        "scene": 0,
        "scenes": [{"nodes": [1, 2]}],
        "nodes": [{"extensions": {"KHR_lights_punctual": {"light": 0}}},
                  {"children": [0]},
                  {"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 1}]}],
        "accessors": [{"bufferView": 0}, {"bufferView": 1}],
        "bufferViews": [{"buffer": 0}, {"buffer": 0}],
        "buffers": [{"uri": "c.bin"}],
        "materials": [{"name": "wax"}, {"name": "flame", "emissiveFactor": [1, 1, 0]}],
    }
    j = join(palace, candles, "main", "cand", 3000.0)
    assert j["buffers"][0]["uri"] == "main/p.bin" and j["buffers"][1]["uri"] == "cand/c.bin"
    assert j["images"][0]["uri"] == "main/textures/x.png"
    assert j["bufferViews"][2]["buffer"] == 1 and j["accessors"][2]["bufferView"] == 2
    prim = j["meshes"][1]["primitives"][0]
    assert j["meshes"][1]["extras"] == {"no_rays": True}, "the candles' mesh is out of the rays"
    assert "extras" not in j["meshes"][0], "the palace's meshes are in them"
    blended = {
        "materials": [{"alphaMode": "BLEND"}, {}],
        "meshes": [{"primitives": [{"material": 0}, {"material": 0}]},
                   {"primitives": [{"material": 0}, {"material": 1}]},
                   {"primitives": [{}]}],
    }
    decals_out_of_the_rays(blended)
    assert blended["meshes"][0]["extras"] == {"no_rays": True}, "a mesh wholly blended is out"
    assert "extras" not in blended["meshes"][1], "a mesh partly blended is in"
    assert "extras" not in blended["meshes"][2], "a mesh of no material is in"
    assert prim["attributes"]["POSITION"] == 2 and prim["indices"] == 3 and prim["material"] == 2
    assert j["nodes"][1].get("extensions") is None, "the palace's light is gone"
    assert j["nodes"][2]["extensions"]["KHR_lights_punctual"]["light"] == 0
    assert j["nodes"][3]["children"] == [2] and j["nodes"][4]["mesh"] == 1
    assert j["scenes"][0]["nodes"] == [0, 3, 4]
    assert j["extensions"]["KHR_lights_punctual"]["lights"][0]["intensity"] == 1
    strength = j["materials"][2]["extensions"]["KHR_materials_emissive_strength"]
    assert strength["emissiveStrength"] == 3000.0
    split_self_test()
    # Refuses what the pinned file does not hold, and writes the same bytes twice.
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "s", "main"))
        os.makedirs(os.path.join(d, "s", "cand"))
        small_palace = {"asset": {}, "scenes": [{"nodes": []}], "nodes": []}
        small_candles = {"asset": {}, "scenes": [{"nodes": []}], "nodes": [],
                         "extensions": {"KHR_lights_punctual": {"lights": []}},
                         "materials": [{"emissiveFactor": [1, 1, 0]}]}
        for name, doc in (("main/p.gltf", small_palace), ("cand/c.gltf", small_candles)):
            with open(os.path.join(d, "s", name), "w", encoding="utf-8") as f:
                json.dump(doc, f)
        manifest = os.path.join(d, "m.json")
        with open(manifest, "w", encoding="utf-8") as f:
            json.dump({"name": "s", "source": {"document": "o.gltf"},
                       "layout": {"palace": "main/p.gltf", "candles": "cand/c.gltf",
                                  "flame_luminance": 1.0}}, f)
        main_layout(manifest, d)
        with open(os.path.join(d, "s", "o.gltf"), "rb") as f:
            first = f.read()
        main_layout(manifest, d)
        with open(os.path.join(d, "s", "o.gltf"), "rb") as f:
            assert f.read() == first
    print("interior.py self-test: ok")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", nargs="?")
    parser.add_argument("root", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.manifest is None or args.root is None:
        parser.error("MANIFEST and DIR, or --self-test")
    return main_layout(args.manifest, args.root)


if __name__ == "__main__":
    sys.exit(main())
