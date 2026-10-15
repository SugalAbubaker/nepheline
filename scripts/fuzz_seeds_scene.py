# SPDX-License-Identifier: Apache-2.0
"""Seed corpora of the scene fuzz targets (tests/fuzz/fuzz_scene_text.c and
tests/fuzz/fuzz_scene_bin.c), written by `python scripts/fuzz_seeds.py`.

The text seeds are scene files written by hand; the binary seeds are built
here from the format description in engine/scene/src/scene_binary.c, with
the component layout hashes computed by a port of
nph_component_layout_hash (engine/scene/src/component.c) over
nph_hash64 (scripts/nph_hash64.py). They double as an independent reading
of both formats: `scene_tests --filter=scene_io.fuzz_seeds*` loads every
seed into a world with the same components, and a seed it refuses fails
the unit tests.
"""

import json
import struct

from nph_hash64 import DEFAULT_SEED, nph_hash64

MASK64 = (1 << 64) - 1

# NphFieldKind (engine/scene/include/nepheline/scene/component.h).
KIND = {"BOOL": 0, "U8": 1, "U16": 2, "U32": 3, "U64": 4, "I8": 5, "I16": 6, "I32": 7,
        "I64": 8, "F32": 9, "F64": 10, "VEC2": 11, "VEC3": 12, "VEC4": 13, "QUAT": 14,
        "DVEC3": 15, "ENTITY": 16, "STR_ID": 17, "TEXT": 18}


def mix_u64(x):
    """nph_hash_mix_u64 (moremur)."""
    x ^= x >> 27
    x = (x * 0x3C79AC492BA7B653) & MASK64
    x ^= x >> 33
    x = (x * 0x1C69B3F74AC4AE35) & MASK64
    x ^= x >> 27
    return x


def combine(a, b):
    """nph_hash_combine."""
    return mix_u64(a ^ ((b + 0x9E3779B97F4A7C15 + ((a << 6) & MASK64) + (a >> 2)) & MASK64))


def str_id(text):
    """nph_str_id: the hash, with 0 remapped to 1."""
    h = nph_hash64(text.encode("utf-8"), DEFAULT_SEED)
    return h if h != 0 else 1


def layout_hash(name, size, align, fields):
    h = nph_hash64(name.encode("ascii"), DEFAULT_SEED)
    h = combine(h, (size << 32) | align)
    for field_name, kind, offset in fields:
        h = combine(h, nph_hash64(field_name.encode("ascii"), DEFAULT_SEED))
        h = combine(h, (KIND[kind] << 32) | offset)
    return h


# The components the seeds use: built-ins and the scene tests' components
# (engine/scene/tests/test_scene_components.h), with their C layouts.
COMPONENTS = {
    "Name": (64, 1, [("text", "TEXT", 0)]),
    "LocalTransform": (56, 8, [("translation", "DVEC3", 0), ("rotation", "QUAT", 24),
                               ("scale", "VEC3", 40)]),
    "PrefabInstance": (16, 8, [("prefab", "STR_ID", 0), ("index", "U32", 8),
                               ("root", "ENTITY", 12)]),
    "test.Position": (12, 4, [("value", "VEC3", 0)]),
    "test.Health": (12, 4, [("current", "F32", 0), ("maximum", "F32", 4),
                            ("attacker", "ENTITY", 8)]),
    "test.TagA": (0, 1, []),
}

MAGIC = 0x4243534E  # 'NSCB'
VERSION = 1


def tag(text):
    return struct.unpack("<I", text.encode("ascii"))[0]


def crc32c(data, crc=0):
    crc ^= 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def bin_str(text):
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


class Writer:
    """Tracks the absolute offset: columns align to 16 in the file."""

    def __init__(self):
        self.data = bytearray(struct.pack("<II", MAGIC, VERSION))

    def section(self, name, payload_fn):
        self.data += struct.pack("<II", tag(name), 0)
        start = len(self.data)
        payload_fn(self)
        struct.pack_into("<I", self.data, start - 4, len(self.data) - start)

    def align(self, n):
        while len(self.data) % n:
            self.data.append(0)

    def finish(self):
        return bytes(self.data) + struct.pack("<I", crc32c(self.data))


def local_transform(t=(0.0, 0.0, 0.0), r=(0.0, 0.0, 0.0, 1.0), s=(1.0, 1.0, 1.0)):
    return struct.pack("<3d4f3f", *t, *r, *s) + b"\0" * 4


def name(text):
    data = text.encode("utf-8")
    return data + b"\0" * (64 - len(data))


def binary_scene(table, groups, hier, strings=()):
    """`table`: component names; `groups`: (hierarchy flag, [table indices],
    [file ids], {table index: [per-entity bytes]}); `hier`: (child, parent)."""
    n = sum(len(g[2]) for g in groups)
    w = Writer()
    w.section("HEAD", lambda o: o.data.extend(struct.pack("<II", n, 0)))

    def comp(o):
        o.data += struct.pack("<I", len(table))
        for comp_name in table:
            size, align, fields = COMPONENTS[comp_name]
            o.data += bin_str(comp_name) + struct.pack("<QI", layout_hash(comp_name, size, align, fields), size)

    w.section("COMP", comp)
    for flags, comps, ids, columns in groups:
        def arch(o, flags=flags, comps=comps, ids=ids, columns=columns):
            o.data += struct.pack("<II", flags, len(comps))
            o.data += struct.pack("<%dI" % len(comps), *comps)
            o.data += struct.pack("<I", len(ids)) + struct.pack("<%dI" % len(ids), *ids)
            for index in comps:
                if COMPONENTS[table[index]][0] == 0:
                    continue
                o.align(16)
                for value in columns[index]:
                    o.data += value

        w.section("ARCH", arch)

    def strs(o):
        o.data += struct.pack("<I", len(strings))
        for s in strings:
            o.data += bin_str(s)

    w.section("STRS", strs)

    def hier_fn(o):
        o.data += struct.pack("<I", len(hier))
        for child, parent in hier:
            o.data += struct.pack("<II", child, parent)

    w.section("HIER", hier_fn)
    return w.finish()


def binary_seeds():
    table = ["Name", "LocalTransform", "test.Position", "test.Health", "test.TagA",
             "PrefabInstance"]
    basic = binary_scene(
        table,
        [(1, [0, 1], [0, 1], {0: [name("root"), name("child é")],
                              1: [local_transform((1.0, 2.0, 1.0e7)),
                                  local_transform(s=(2.0, 2.0, 2.0))]}),
         (0, [2, 3, 4], [2, 3], {2: [struct.pack("<3f", 1.0, 2.0, 3.0),
                                     struct.pack("<3f", -1.0, 0.5, 0.25)],
                                 3: [struct.pack("<ffI", 10.0, 20.0, 1),
                                     struct.pack("<ffI", 5.0, 5.0, 0)]})],
        [(1, 0)])
    instance = binary_scene(
        table,
        [(1, [1, 5], [0, 1], {1: [local_transform(), local_transform((0.0, 1.0, 0.0))],
                              5: [struct.pack("<QII", str_id("crate"), 0, 1),
                                  struct.pack("<QII", str_id("crate"), 1, 1)]})],
        [(1, 0)], strings=["crate"])
    empty = binary_scene([], [], [])
    return {"basic": basic, "instance": instance, "empty": empty}


def text_seeds():
    all_kinds = {
        "flag": True, "u8v": 255, "u16v": 65535, "u32v": 4000000000,
        "u64v": "18446744073709551615", "i8v": -128, "i16v": -32768, "i32v": -2147483648,
        "i64v": -9223372036854775808, "f32v": 0.1, "f64v": -0.0, "v2": ["nan", "inf"],
        "v3": ["-inf", 1e-30, 3.4028235e38], "v4": [1, 2, 3, 4], "q": [0, 0, 0, 1],
        "d3": [0.3333333333333333, 1e300, -5e-324], "target": 1,
        "sid": "assets/crate.mesh", "label": "café \"quoted\"",
    }
    scenes = {
        "basic": {"nepheline_scene": 1, "entities": [
            {"id": 0, "components": {
                "Name": {"text": "root"},
                "LocalTransform": {"translation": [0, 0, 10000000], "rotation": [0, 0, 0, 1],
                                   "scale": [1, 1, 1]}}},
            {"id": 1, "parent": 0, "components": {
                "LocalTransform": {"translation": [1, 2, 3], "rotation": [0, 0.70710677, 0, 0.70710677],
                                   "scale": [2, 2, 2]},
                "test.Health": {"current": 5, "maximum": 10, "attacker": 0}}},
            {"id": 2, "parent": 1, "components": {"test.TagA": {}, "test.Position": {"value": [1, 2, 3]}}},
        ]},
        "all_kinds": {"nepheline_scene": 1, "entities": [
            {"id": 0, "components": {"test.AllKinds": all_kinds}},
            {"id": 1, "components": {"test.AllKinds": dict(all_kinds, target=None,
                                                           sid={"hash": "0123456789abcdef"})}},
        ]},
        "prefab": {"nepheline_scene": 1, "entities": [
            {"id": 0, "prefab": {"name": "crate", "index": 0, "root": 0}},
            {"id": 1, "parent": 0, "prefab": {"name": "crate", "index": 1, "root": 0}},
            {"id": 2, "prefab": {"name": "crate", "index": 0, "root": 2},
             "overrides": {"test.Position": {"value": [9, 2, 3]}},
             "components": {"test.Velocity": {"value": [0, 1, 0]}}},
            {"id": 3, "parent": 2, "prefab": {"name": "crate", "index": 1, "root": 2},
             "removed": ["test.TagA"]},
            {"id": 4, "prefab": {"name": "crate", "index": 0, "root": 4, "full": True},
             "components": {"test.Position": {"value": [0, 0, 0]}}},
        ]},
        "forward_refs": {"nepheline_scene": 1, "entities": [
            {"id": 10, "components": {"test.Health": {"current": 1, "maximum": 2, "attacker": 30}}},
            {"id": 30, "parent": 20, "components": {"test.Health": {"attacker": 10}}},
            {"id": 20, "components": {"Name": {"text": "middle"}}},
        ]},
    }
    out = {k: json.dumps(v, indent=2).encode("utf-8") for k, v in scenes.items()}
    out["empty"] = b'{"nepheline_scene": 1, "entities": []}'
    return out


def write_scene_seeds(write, corpus):
    import os
    for seed_name, data in text_seeds().items():
        write(os.path.join(corpus, "scene_text", seed_name), data)
    for seed_name, data in binary_seeds().items():
        write(os.path.join(corpus, "scene_bin", seed_name), data)
