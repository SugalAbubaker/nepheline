# SPDX-License-Identifier: Apache-2.0
"""Generate the seed corpora of the asset module's fuzz targets.

    python scripts/fuzz_seeds.py   # writes tests/fuzz/corpus/{lz,nph,pack,image,gltf}/

The files are built here, in Python, from the format descriptions in
engine/asset/include/nepheline/asset/{lz,format,pack}.h rather than by the
engine's own writers, so they double as an independent reading of the byte
layout: if the C code and this script disagree, nph_asset_file_validate
and nph_pack_validate reject these seeds and the fuzz run reports it at
once (the fuzz targets trap on a seed that fails validation only through
their coverage; the unit test `asset_tests --filter=format.*` is the
authoritative check of the layout).

Seed files carry no extension: `.nph` and `.npk` are Git LFS patterns
(.gitattributes), and the fuzz workflow checks out without LFS.
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "tests", "fuzz", "corpus")


def crc32c(data, crc=0):
    """CRC-32C (Castagnoli), bitwise reference: matches nph_crc32c."""
    crc ^= 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def wymix(a, b):
    r = (a * b) & ((1 << 128) - 1)
    return (r & ((1 << 64) - 1)) ^ (r >> 64)


def fourcc(text):
    return struct.unpack("<I", text.encode("ascii"))[0]


def lz_literals(data):
    """An LZ block holding `data` as literals only (always valid)."""
    out = bytearray()
    n = len(data)
    if n >= 15:
        out.append(0xF0)
        rem = n - 15
        while rem >= 255:
            out.append(255)
            rem -= 255
        out.append(rem)
    else:
        out.append(n << 4)
    out += data
    return bytes(out)


def lz_repeat(byte, count):
    """`count` copies of `byte`: one literal and one long match, then the
    mandatory literal-only final sequence."""
    assert count >= 2
    match = count - 1
    out = bytearray([0x1F if match - 4 >= 15 else ((match - 4) & 0xF)])
    out.append(byte)
    out += struct.pack("<H", 1)
    if match - 4 >= 15:
        rem = match - 4 - 15
        while rem >= 255:
            out.append(255)
            rem -= 255
        out.append(rem)
    out.append(0x00)  # final token: zero literals
    return bytes(out)


def guid_from_str(text):
    """Any non-null GUID will do for a seed; the engine's hash is not
    replicated here (it is pinned by the unit tests instead)."""
    lo = (sum(text.encode()) * 0x9E3779B97F4A7C15 + 1) & ((1 << 64) - 1)
    hi = (lo * 0xBF58476D1CE4E5B9 + 7) & ((1 << 64) - 1)
    return struct.pack("<QQ", lo or 1, hi)


def nph_file(asset_type, type_version, blobs):
    """A .nph image. `blobs` is a list of (tag, index, codec, align, raw, stored)."""
    header_size = 64
    entry_size = 48
    table = bytearray()
    offset = header_size + entry_size * len(blobs)
    layout = []
    for tag, index, codec, align, raw, stored in blobs:
        offset = (offset + align - 1) // align * align
        layout.append((offset, stored))
        table += struct.pack("<IIIIQQQII", fourcc(tag), index, codec, align, offset,
                             len(stored), len(raw), crc32c(stored), 0)
        offset += len(stored)
    file_size = (offset + 15) // 16 * 16
    body = bytearray(file_size)
    body[header_size:header_size + len(table)] = table
    for (off, stored) in layout:
        body[off:off + len(stored)] = stored
    head = struct.pack("<IHHII", fourcc("NPHA"), 1, header_size, asset_type, type_version)
    head += guid_from_str("seed")
    head += struct.pack("<QIIQI", 0xC0FFEE, len(blobs), header_size, file_size, 0)
    head += struct.pack("<I", crc32c(head))
    assert len(head) == 64
    body[0:64] = head
    return bytes(body)


def mesh_blobs():
    """A triangle: the smallest valid mesh (format: mesh.h)."""
    header = struct.pack("<8I", 3, 3, 1, 1, 3, 1, 2, 0)
    header += struct.pack("<4f", 0.0, 0.0, 0.0, 0.0)  # position_offset + pad
    header += struct.pack("<4f", 1.0, 1.0, 1.0, 0.0)  # position_scale + pad
    header += struct.pack("<6f", 0.0, 0.0, 0.0, 1.0, 1.0, 0.0)  # bounds
    header += struct.pack("<2f", 0.0, 0.0)
    header += struct.pack("<4f", 0.5, 0.5, 0.0, 0.71)  # sphere
    assert len(header) == 112
    positions = struct.pack("<4H", 0, 0, 0, 0) + struct.pack("<4H", 65535, 0, 0, 0) + \
        struct.pack("<4H", 0, 65535, 0, 0)
    attribs = struct.pack("<4I", 0x7FFF7FFF, 0x7F00007F, 0, 0xFFFFFFFF) * 3
    indices = struct.pack("<3I", 0, 1, 2)
    submesh = struct.pack("<8I", 0, 3, 0, 1, 0, 0, 0, 0) + struct.pack("<8f", 0, 0, 0, 1, 1, 0, 0, 0)
    meshlet = struct.pack("<IIHHI", 0, 0, 3, 1, 0)
    bounds = struct.pack("<4f", 0.5, 0.5, 0.0, 0.71) + struct.pack("<4b", 0, 0, 127, 127) + \
        struct.pack("<3f", 0, 0, 0)
    mlv = struct.pack("<3I", 0, 1, 2)
    mlt = bytes([0, 1, 2])
    return [
        ("HEAD", 0, 0, 16, header, header),
        ("POSN", 0, 0, 16, positions, positions),
        ("ATTR", 0, 0, 16, attribs, attribs),
        ("INDX", 0, 1, 16, indices, lz_literals(indices)),
        ("SUBM", 0, 0, 16, submesh, submesh),
        ("MLET", 0, 0, 16, meshlet, meshlet),
        ("MLBD", 0, 0, 16, bounds, bounds),
        ("MLVX", 0, 0, 16, mlv, mlv),
        ("MLTR", 0, 0, 16, mlt, mlt),
    ]


def texture_blobs():
    """A 4x2 RGBA8 texture with two mips, the base mip LZ-compressed."""
    header = struct.pack("<16I", 4, 2, 2, 1, 3, 0, 0, *([0] * 9))
    mip0 = bytes(range(32))
    mip1 = bytes(range(8))
    return [
        ("HEAD", 0, 0, 16, header, header),
        ("MIP ", 0, 1, 16, mip0, lz_literals(mip0)),
        ("MIP ", 1, 0, 16, mip1, mip1),
    ]


def material_blobs():
    header = struct.pack("<4f", 1, 1, 1, 1) + struct.pack("<3f", 0, 0, 0) + struct.pack("<f", 1)
    header += struct.pack("<3f", 1, 1, 0.5) + struct.pack("<2I", 0, 0) + struct.pack("<3I", 0, 0, 0)
    for _ in range(5):
        header += bytes(16) + struct.pack("<I4BfI", 0, 0, 0, 0, 0, 1.0, 0)
    assert len(header) == 224
    return [("HEAD", 0, 0, 16, header, header)]


def scene_blobs():
    strings = b"root\0child\0"
    header = struct.pack("<16I", 2, 1, 1, 1, 1, len(strings), 0, *([0] * 9))
    no_node = 0xFFFFFFFF
    node = struct.pack("<4f", 0, 0, 0, 0) + struct.pack("<4f", 0, 0, 0, 1) + \
        struct.pack("<4f", 1, 1, 1, 0)
    nodes = node + struct.pack("<4I", no_node, 0, 1, no_node) + \
        node + struct.pack("<4I", 0, 5, no_node, no_node)
    instance = guid_from_str("mesh") + struct.pack("<4I", 1, 0, 1, 0)
    materials = guid_from_str("material")
    camera = struct.pack("<2I6f", 0, 0, 1.0, 1.5, 0.1, 0.0, 0.0, 0.0)
    light = struct.pack("<2I", 1, 1) + struct.pack("<3f", 1, 1, 1) +         struct.pack("<5f", 10, 0, 0, 0, 0)
    assert len(light) == 40
    return [
        ("HEAD", 0, 0, 16, header, header),
        ("NODE", 0, 0, 16, nodes, nodes),
        ("INST", 0, 0, 16, instance, instance),
        ("MTRL", 0, 0, 16, materials, materials),
        ("CAMS", 0, 0, 16, camera, camera),
        ("LGHT", 0, 0, 16, light, light),
        ("STRS", 0, 0, 16, strings, strings),
    ]


def pack_file(entries, entry_align=16):
    """A .npk image. `entries` is a list of (path, guid_bytes, type, data)."""
    header_size = 64
    body = bytearray(header_size)
    placed = []
    for path, guid, asset_type, data in entries:
        offset = (len(body) + entry_align - 1) // entry_align * entry_align
        body += bytes(offset - len(body))
        body += data
        placed.append((path, guid, asset_type, offset, len(data)))
    # The engine sorts by nph_hash64 of the path; a seed only needs a
    # consistent order, so a single entry (or one with hashes in order)
    # keeps the script free of the hash. With one entry nothing is sorted.
    assert len(placed) <= 1
    table_offset = (len(body) + 7) // 8 * 8
    body += bytes(table_offset - len(body))
    strings = bytearray()
    table = bytearray()
    for path, guid, asset_type, offset, size in placed:
        path_hash = nph_hash64(path.encode())
        table += guid + struct.pack("<QQQIIIIQ", path_hash, offset, size, len(strings),
                                    len(path), asset_type, 0, 0)
        strings += path.encode() + b"\0"
    index_offset = table_offset + len(table)
    index = b"".join(struct.pack("<I", i) for i in range(len(placed)))
    strings_offset = index_offset + len(index)
    body += table + index + strings
    file_size = (len(body) + 15) // 16 * 16
    body += bytes(file_size - len(body))
    head = struct.pack("<IHHIIQQQQQI", fourcc("NPAK"), 1, header_size, len(placed), entry_align,
                       table_offset, index_offset, strings_offset, len(strings), file_size, 0)
    head += struct.pack("<I", crc32c(head))
    assert len(head) == 64
    body[0:64] = head
    return bytes(body)


# --- nph_hash64, the engine's wyhash-style construction (core/hash.c) ----
# Only needed for pack entries' path hashes; kept exactly in step with the
# C implementation and checked by `asset_tests` through the pack seeds the
# fuzz workflow feeds back.

def _read(data, offset, n):
    return int.from_bytes(data[offset:offset + n], "little")


def nph_hash64(data, seed=0x4E65706865):
    import importlib.util
    spec = os.path.join(ROOT, "scripts", "nph_hash64.py")
    if os.path.exists(spec):
        module_spec = importlib.util.spec_from_file_location("nph_hash64", spec)
        module = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(module)
        return module.nph_hash64(data, seed)
    raise SystemExit("scripts/nph_hash64.py is missing")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print(f"{os.path.relpath(path, ROOT)}: {len(data)} bytes")


def main():
    text = b"nepheline mesh texture vertex index the buffer frame pixel cache " * 8
    write(os.path.join(CORPUS, "lz", "literals"), bytes([0x10]) + lz_literals(text[:200]))
    write(os.path.join(CORPUS, "lz", "repeat"), bytes([0x40]) + lz_repeat(0x41, 300))
    write(os.path.join(CORPUS, "nph", "blob"),
          nph_file(fourcc("BLOB"), 1, [("DATA", 0, 1, 16, text, lz_literals(text)),
                                        ("DATA", 1, 0, 64, b"stored", b"stored")]))
    write(os.path.join(CORPUS, "nph", "mesh"), nph_file(fourcc("MESH"), 1, mesh_blobs()))
    write(os.path.join(CORPUS, "nph", "texture"), nph_file(fourcc("TEX "), 1, texture_blobs()))
    write(os.path.join(CORPUS, "nph", "material"), nph_file(fourcc("MATL"), 1, material_blobs()))
    write(os.path.join(CORPUS, "nph", "scene"), nph_file(fourcc("SCEN"), 1, scene_blobs()))
    blob = nph_file(fourcc("BLOB"), 1, [("DATA", 0, 0, 16, b"pack", b"pack")])
    write(os.path.join(CORPUS, "pack", "one_entry"),
          pack_file([("assets/one.nph", guid_from_str("one"), fourcc("BLOB"), blob)]))
    write(os.path.join(CORPUS, "pack", "empty"), pack_file([]))
    # The cooker's inputs: images and glTF documents (scripts/fuzz_seeds_cook.py).
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import fuzz_seeds_cook
    fuzz_seeds_cook.write_cook_seeds(write, CORPUS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
