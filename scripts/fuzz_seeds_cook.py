# SPDX-License-Identifier: Apache-2.0
"""Seed corpora of the cooker's fuzz targets (tests/fuzz/fuzz_gltf.c and
tests/fuzz/fuzz_image.c), written by `python scripts/fuzz_seeds.py`.

Every file is built here from the published format descriptions (the PNG,
JPEG, TGA, BMP, Radiance HDR, and DDS layouts; glTF 2.0 and its binary
container)
rather than by any library, so the seeds double as an independent check:
`cook_tests --filter=*corpus*` decodes and imports each one with the
cooker, and a seed it refuses fails the unit tests.

Files carry no extension: image and .glb extensions are Git LFS patterns
(.gitattributes), and the fuzz workflow checks out without LFS.
"""

import base64
import json
import struct
import zlib

# ---------------------------------------------------------------------------
# Images
# ---------------------------------------------------------------------------


def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def png(width, height, color_type, rows, bit_depth=8, extra=b""):
    """A PNG (ISO/IEC 15948): `rows` are unfiltered scanlines; each gets
    filter type 0 here except odd rows, which use Sub (1) so the decoder's
    filters are exercised."""
    raw = bytearray()
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type] * (bit_depth // 8)
    for y, row in enumerate(rows):
        if y % 2 == 0:
            raw.append(0)
            raw += row
        else:
            raw.append(1)  # Sub: each byte minus the one bpp to its left
            raw += bytes((row[i] - (row[i - bpp] if i >= bpp else 0)) & 0xFF
                         for i in range(len(row)))
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr) + extra +
            _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _png_chunk(b"IEND", b""))


def gradient_rgba(width, height):
    return [bytes(v for x in range(width)
                  for v in ((x * 40) & 0xFF, (y * 50) & 0xFF, ((x + y) * 20) & 0xFF,
                            255 - x * 10))
            for y in range(height)]


def tga_uncompressed(width, height, rgba):
    head = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, width, height, 32, 0x28)
    body = bytearray()
    for row in rgba:
        for x in range(width):
            r, g, b, a = row[x * 4:x * 4 + 4]
            body += bytes((b, g, r, a))
    return head + bytes(body)


def tga_rle(width, height):
    """A run-length encoded 24-bit TGA (image type 10), bottom-up rows:
    each row is a run of one colour then a raw packet."""
    head = struct.pack("<BBBHHBHHHHBB", 0, 0, 10, 0, 0, 0, 0, 0, width, height, 24, 0x00)
    body = bytearray()
    half = width // 2
    for y in range(height):
        body.append(0x80 | (half - 1))  # a run of `half` texels
        body += bytes((10 * y, 200, 30))
        body.append(width - half - 1)  # a raw packet of the rest
        for x in range(width - half):
            body += bytes((x * 30, y * 30, 100))
    return head + bytes(body)


def bmp24(width, height):
    """A BITMAPINFOHEADER bitmap, 24 bits, bottom-up, rows padded to 4 bytes."""
    row_bytes = (width * 3 + 3) // 4 * 4
    pixels = bytearray()
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes((x * 50 & 0xFF, y * 50 & 0xFF, 128))  # B, G, R
        row += bytes(row_bytes - len(row))
        pixels += row
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    file_head = struct.pack("<2sIHHI", b"BM", 14 + len(info) + len(pixels), 0, 0, 14 + len(info))
    return file_head + info + bytes(pixels)


def _rgbe(r, g, b):
    import math
    v = max(r, g, b)
    if v < 1e-32:
        return bytes(4)
    m, e = math.frexp(v)
    scale = m * 256.0 / v
    return bytes((int(r * scale), int(g * scale), int(b * scale), e + 128))


def hdr_flat(width, height):
    head = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (height, width)
    body = b"".join(_rgbe(0.5 + x, 4.0, 20.0 - y) for y in range(height) for x in range(width))
    return head + body


def hdr_rle(width, height):
    """New-style run-length scanlines (width 8..32767): a 2 2 hi lo header,
    then each of the four components as runs (count > 128) or literals."""
    head = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (height, width)
    body = bytearray()
    for y in range(height):
        texels = [_rgbe(0.25 * (x + 1), 1.5, 3.0 + y) for x in range(width)]
        body += bytes((2, 2, width >> 8, width & 0xFF))
        for c in range(4):
            values = [t[c] for t in texels]
            if len(set(values)) == 1:
                body += bytes((128 + width, values[0]))  # one run
            else:
                body += bytes((width,)) + bytes(values)  # literals
    return head + bytes(body)


def jpeg_gray_flat(value):
    """A baseline JPEG (ITU-T T.81): one 8x8 grey block of `value`, with a
    one-code DC table (category 8) and a one-code AC table (end of block).
    The DC coefficient of a flat block of v - 128 is 8 (v - 128); with a
    quantiser of 1 and v = 144 it is 128, category 8."""
    def segment(marker, payload):
        return struct.pack(">BBH", 0xFF, marker, len(payload) + 2) + payload
    dc = 8 * (value - 128)
    assert 128 <= dc <= 255  # category 8, written as its 8 bits
    dqt = segment(0xDB, bytes([0x00]) + bytes([1] * 64))
    sof = segment(0xC0, struct.pack(">BHHB", 8, 8, 8, 1) + bytes([1, 0x11, 0]))
    one_code = bytes([1] + [0] * 15)  # one code of length 1: "0"
    dht_dc = segment(0xC4, bytes([0x00]) + one_code + bytes([8]))
    dht_ac = segment(0xC4, bytes([0x10]) + one_code + bytes([0x00]))
    sos = segment(0xDA, bytes([1, 1, 0x00, 0, 63, 0]))
    # "0" (category 8) + the value's 8 bits + "0" (EOB), padded with ones.
    bits = "0" + format(dc, "08b") + "0"
    bits += "1" * (-len(bits) % 8)
    scan = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
    assert 0xFF not in scan  # no byte stuffing needed
    return b"\xff\xd8" + dqt + sof + dht_dc + dht_ac + sos + scan + b"\xff\xd9"


def _dds(width, height, data, fourcc=b"", dxgi=None, pf_flags=0x4, bits=0, masks=(0, 0, 0, 0),
         mips=1):
    """A DDS file (Microsoft, "DDS", "DDS_HEADER", "DDS_PIXELFORMAT",
    "DDS_HEADER_DXT10"): the magic, the 124-byte header (its pixel format
    at offset 72 after eleven reserved words, the caps at 104), the DX10
    header when `dxgi` is given, then `data`, the top level and any after."""
    flags = 0x1007 | (0x20000 if mips > 1 else 0)  # caps, height, width, pixel format
    if dxgi is not None:
        fourcc = b"DX10"
        pf_flags = 0x4
    pixel_format = struct.pack("<II4sI4I", 32, pf_flags, fourcc.ljust(4, b"\x00"), bits, *masks)
    header = struct.pack("<7I", 124, flags, height, width, 0, 0, mips) + bytes(44)
    header += pixel_format + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    assert len(header) == 124
    out = b"DDS " + header
    if dxgi is not None:
        out += struct.pack("<5I", dxgi, 3, 0, 1, 0)  # TEXTURE2D, one element
    return out + data


def _bytes(count, seed):
    """Deterministic filler (a linear congruential sequence)."""
    out = bytearray()
    x = seed
    for _ in range(count):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


def _blocks(count, size, mode_byte=None, seed=1):
    """`count` blocks of `size` bytes; with `mode_byte`, each block's first
    byte names a BC7 or BC6H mode (the rest of any such block is valid)."""
    out = bytearray()
    for i in range(count):
        b = bytearray(_bytes(size, seed + i))
        if mode_byte is not None:
            b[0] = mode_byte | (b[0] & 0x80 if mode_byte == 0x40 else 0)
        out += b
    return bytes(out)


def dds_seeds():
    """DDS sources (tools/cook/src/dds.c), small and in each family read:
    BC7 by DX10 header with a second level after the top, legacy DXT1 with
    partial blocks, DXT5, ATI2, BC6H mode 11, and 32-bit BGRA by masks."""
    return {
        # 8 x 4: two blocks, then the 4 x 2 level (one block).
        "dds_bc7": _dds(8, 4, _blocks(3, 16, 0x40), dxgi=99, mips=2),
        # 5 x 3: two blocks across, one down.
        "dds_dxt1": _dds(5, 3, _blocks(2, 8), fourcc=b"DXT1"),
        "dds_dxt5": _dds(4, 4, _blocks(1, 16, seed=7), fourcc=b"DXT5"),
        "dds_ati2": _dds(4, 4, _blocks(1, 16, seed=9), fourcc=b"ATI2"),
        "dds_bc6h": _dds(4, 4, _blocks(1, 16, 0x03, seed=11), dxgi=95),
        "dds_bgra": _dds(3, 2, _bytes(3 * 2 * 4, 13), pf_flags=0x41, bits=32,
                         masks=(0xFF0000, 0xFF00, 0xFF, 0xFF000000)),
    }


def image_seeds():
    rgba = gradient_rgba(4, 4)
    gray_rows = [bytes((x * 60 + y * 10) & 0xFF for x in range(5)) for y in range(3)]
    palette_rows = [bytes((x + y) % 4 for x in range(4)) for y in range(4)]
    plte = _png_chunk(b"PLTE", bytes((0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255)))
    trns = _png_chunk(b"tRNS", bytes((255, 128, 64, 0)))
    rgb16_rows = [b"".join(struct.pack(">HHH", x * 9000, y * 9000, 30000) for x in range(3))
                  for y in range(2)]
    return {
        "png_rgba": png(4, 4, 6, rgba),
        "png_gray": png(5, 3, 0, gray_rows),
        "png_palette": png(4, 4, 3, palette_rows, extra=plte + trns),
        "png_rgb16": png(3, 2, 2, rgb16_rows, bit_depth=16),
        "tga": tga_uncompressed(4, 4, rgba),
        "tga_rle": tga_rle(6, 3),
        "bmp": bmp24(5, 3),
        "hdr": hdr_flat(4, 2),
        "hdr_rle": hdr_rle(8, 2),
        "jpeg": jpeg_gray_flat(144),
        **dds_seeds(),
    }


# ---------------------------------------------------------------------------
# glTF documents
# ---------------------------------------------------------------------------


def _data_uri(data, mime="application/octet-stream"):
    return "data:%s;base64,%s" % (mime, base64.b64encode(data).decode("ascii"))


def _dump(doc):
    return json.dumps(doc, separators=(",", ":")).encode("utf-8")


def gltf_triangle():
    """Positions and indices: the least a mesh can be."""
    pos = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    idx = struct.pack("<3H", 0, 1, 2) + b"\0\0"
    buf = pos + idx
    return {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
        ],
        "bufferViews": [{"buffer": 0, "byteLength": 36},
                        {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
        "buffers": [{"byteLength": len(buf), "uri": _data_uri(buf)}],
    }, buf


def gltf_textured():
    """A quad with normals, UVs, colours, and a material with an embedded
    PNG, a sampler, a camera, a point light, and a node hierarchy using
    both TRS and a matrix."""
    pos = struct.pack("<12f", -1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1)
    nrm = struct.pack("<12f", *([0, 1, 0] * 4))
    uv = struct.pack("<8f", 0, 0, 1, 0, 1, 1, 0, 1)
    col = struct.pack("<16B", *([255, 128, 0, 255] * 4))
    idx = struct.pack("<6H", 0, 2, 1, 0, 3, 2)
    buf = pos + nrm + uv + col + idx
    offsets = [0, 48, 96, 128, 144]
    image = png(4, 4, 6, gradient_rgba(4, 4))
    doc = {
        "asset": {"version": "2.0", "generator": "nepheline fuzz seeds"},
        "extensionsUsed": ["KHR_lights_punctual", "KHR_materials_emissive_strength"],
        "extensions": {"KHR_lights_punctual": {"lights": [
            {"type": "point", "color": [1, 0.8, 0.6], "intensity": 20, "range": 5}]}},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": "root", "children": [1, 2, 3], "translation": [0, 1, 0]},
            {"name": "quad", "mesh": 0, "rotation": [0, 0.7071068, 0, 0.7071068],
             "scale": [2, 2, 2]},
            {"name": "camera", "camera": 0,
             "matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 2, 5, 1]},
            {"name": "lamp", "extensions": {"KHR_lights_punctual": {"light": 0}}},
        ],
        "cameras": [{"type": "perspective",
                     "perspective": {"yfov": 0.8, "znear": 0.1, "zfar": 100}}],
        "meshes": [{"name": "Quad", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "COLOR_0": 3},
            "indices": 4, "material": 0}]}],
        "materials": [{
            "name": "Painted",
            "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                     "metallicFactor": 0.1, "roughnessFactor": 0.6},
            "emissiveFactor": [0.1, 0.1, 0.1],
            "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 2}},
            "alphaMode": "MASK", "alphaCutoff": 0.3, "doubleSided": True}],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9728, "wrapS": 33071, "wrapT": 33648}],
        "images": [{"name": "gradient", "uri": _data_uri(image, "image/png")}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [-1, 0, -1], "max": [1, 0, 1]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5121, "normalized": True, "count": 4,
             "type": "VEC4"},
            {"bufferView": 4, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": offsets[0], "byteLength": 48},
            {"buffer": 0, "byteOffset": offsets[1], "byteLength": 48},
            {"buffer": 0, "byteOffset": offsets[2], "byteLength": 32},
            {"buffer": 0, "byteOffset": offsets[3], "byteLength": 16},
            {"buffer": 0, "byteOffset": offsets[4], "byteLength": 12},
        ],
        "buffers": [{"byteLength": len(buf), "uri": _data_uri(buf)}],
    }
    return doc


def gltf_strips_and_sparse():
    """A strip and a fan over one accessor, and a quantised (normalised
    i16) position accessor with a sparse substitution."""
    pos = struct.pack("<15f", 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 1, 0)
    q = struct.pack("<12h", 0, 0, 0, 0, 32767, 0, 0, 0, 0, 32767, 0, 0)
    sparse_idx = bytes([2, 0, 0, 0])
    sparse_val = struct.pack("<3h", 0, 16384, 0) + b"\0\0"
    buf = pos + q + sparse_idx + sparse_val
    return {
        "asset": {"version": "2.0"},
        "extensionsUsed": ["KHR_mesh_quantization"],
        "extensionsRequired": ["KHR_mesh_quantization"],
        "meshes": [
            {"primitives": [{"attributes": {"POSITION": 0}, "mode": 5},
                            {"attributes": {"POSITION": 0}, "mode": 6}]},
            {"primitives": [{"attributes": {"POSITION": 1}}]},
        ],
        "nodes": [{"mesh": 0}, {"mesh": 1, "translation": [3, 0, 0]}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 5, "type": "VEC3",
             "min": [0, 0, 0], "max": [2, 1, 0]},
            {"bufferView": 1, "componentType": 5122, "normalized": True, "count": 3,
             "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0],
             "sparse": {"count": 1, "indices": {"bufferView": 2, "componentType": 5121},
                        "values": {"bufferView": 3}}},
        ],
        "bufferViews": [
            {"buffer": 0, "byteLength": 60},
            {"buffer": 0, "byteOffset": 60, "byteLength": 24, "byteStride": 8},
            {"buffer": 0, "byteOffset": 84, "byteLength": 1},
            {"buffer": 0, "byteOffset": 88, "byteLength": 6},
        ],
        "buffers": [{"byteLength": len(buf), "uri": _data_uri(buf)}],
    }


def glb(doc, binary):
    """The binary container (glTF 2.0 §4.4): a 12-byte header, a JSON chunk
    padded with spaces, a BIN chunk padded with zeros."""
    doc = dict(doc)
    doc["buffers"] = [{"byteLength": len(binary)}]  # buffer 0 is the BIN chunk
    text = _dump(doc)
    text += b" " * (-len(text) % 4)
    binary += b"\0" * (-len(binary) % 4)
    json_chunk = struct.pack("<II", len(text), 0x4E4F534A) + text
    bin_chunk = struct.pack("<II", len(binary), 0x004E4942) + binary
    total = 12 + len(json_chunk) + len(bin_chunk)
    return struct.pack("<III", 0x46546C67, 2, total) + json_chunk + bin_chunk


def gltf_specgloss_dds():
    """A quad whose material is specular-glossiness
    (KHR_materials_pbrSpecularGlossiness) and whose diffuse texture has a
    DDS source (MSFT_texture_dds, a BC7 file embedded) beside a fallback
    PNG that does not exist: the importer reads the DDS alone."""
    pos = struct.pack("<12f", -1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1)
    uv = struct.pack("<8f", 0, 0, 1, 0, 1, 1, 0, 1)
    idx = struct.pack("<6H", 0, 2, 1, 0, 3, 2)
    buf = pos + uv + idx
    dds = _dds(4, 4, _blocks(1, 16, 0x40, seed=17), dxgi=98)
    return {
        "asset": {"version": "2.0", "generator": "nepheline fuzz seeds"},
        "extensionsUsed": ["KHR_materials_pbrSpecularGlossiness", "MSFT_texture_dds"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                                    "indices": 2, "material": 0}]}],
        "materials": [{"extensions": {"KHR_materials_pbrSpecularGlossiness": {
            "diffuseTexture": {"index": 0}, "specularFactor": [0.5, 0.4, 0.3],
            "glossinessFactor": 0.6}}}],
        "textures": [{"source": 0, "extensions": {"MSFT_texture_dds": {"source": 1}}}],
        "images": [{"uri": "missing.png"}, {"uri": _data_uri(dds, "image/vnd-ms.dds")}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [-1, 0, -1], "max": [1, 0, 1]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48},
                        {"buffer": 0, "byteOffset": 48, "byteLength": 32},
                        {"buffer": 0, "byteOffset": 80, "byteLength": 12}],
        "buffers": [{"byteLength": len(buf), "uri": _data_uri(buf)}],
    }


def gltf_seeds():
    triangle, triangle_buf = gltf_triangle()
    return {
        "triangle": _dump(triangle),
        "textured": _dump(gltf_textured()),
        "strips_sparse": _dump(gltf_strips_and_sparse()),
        "triangle_glb": glb(triangle, triangle_buf),
        "specgloss_dds": _dump(gltf_specgloss_dds()),
    }


def nmesh_seeds():
    """Editable meshes (tools/cook/polymesh.h): the shapes the editor makes,
    faces the triangulation must handle, and degenerate cases."""
    import json

    def doc(positions, faces, smoothing=None):
        d = {"nmesh": 1, "positions": positions, "faces": faces}
        if smoothing is not None:
            d["smoothing"] = smoothing
        return json.dumps(d, separators=(",", ":")).encode("utf-8")

    quad_uv = [[0, 1], [1, 1], [1, 0], [0, 0]]
    cube_positions = [[(-0.5 if i & 1 == 0 else 0.5), (-0.5 if i & 2 == 0 else 0.5),
                       (-0.5 if i & 4 == 0 else 0.5)] for i in range(8)]
    cube_faces = [[4, 5, 7, 6], [1, 0, 2, 3], [5, 1, 3, 7], [0, 4, 6, 2], [6, 7, 3, 2],
                  [0, 1, 5, 4]]
    seeds = {}
    seeds["triangle"] = doc([[0, 0, 0], [1, 0, 0], [0, 1, 0]], [{"v": [0, 1, 2]}])
    seeds["cube"] = doc(cube_positions, [{"v": f, "uv": quad_uv} for f in cube_faces], 60)
    seeds["cube_materials_flat"] = doc(
        cube_positions, [{"v": f, "uv": quad_uv, "m": i % 3} for i, f in enumerate(cube_faces)],
        0)
    # An L and a comb, in the XZ plane facing +Y: reflex corners.
    ell = [[0, 0], [0, 2], [2, 2], [2, 1], [1, 1], [1, 0]]
    comb = [[0, 0], [0, 3], [5, 3], [5, 0], [4, 0], [4, 2], [3, 2], [3, 0], [2, 0], [2, 2],
            [1, 2], [1, 0]]
    positions = [[x, 0, z] for x, z in ell] + [[x + 10, 0, z] for x, z in comb]
    seeds["concave"] = doc(positions, [{"v": list(range(6))},
                                       {"v": list(range(6, 18))}])
    seeds["pyramid"] = doc([[-1, 0, -1], [1, 0, -1], [1, 0, 1], [-1, 0, 1], [0, 1.5, 0]],
                           [{"v": [0, 1, 2, 3]}, {"v": [3, 2, 4]}, {"v": [2, 1, 4]},
                            {"v": [1, 0, 4]}, {"v": [0, 3, 4]}], 30)
    seeds["regression_degenerate"] = doc(
        [[0, 0, 0], [1, 0, 0], [2, 0, 0], [3, 0, 0], [0, 1, 0]],
        [{"v": [0, 1, 2, 3]}, {"v": [0, 0, 4]}, {"v": [0, 1, 4, 1]}])
    return seeds


def write_cook_seeds(write, corpus):
    import os
    for name, data in image_seeds().items():
        write(os.path.join(corpus, "image", name), data)
    for name, data in gltf_seeds().items():
        write(os.path.join(corpus, "gltf", name), data)
    for name, data in nmesh_seeds().items():
        write(os.path.join(corpus, "nmesh", name), data)
