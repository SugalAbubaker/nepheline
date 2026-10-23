# SPDX-License-Identifier: Apache-2.0
"""Seed corpora of the physics fuzz targets (tests/fuzz/fuzz_physics_hull.c
and tests/fuzz/fuzz_physics_mesh.c), written by `python scripts/fuzz_seeds.py`.

The targets read builder input, not blocks: a point cloud or a triangle
soup with coordinates in 1/256 m, then a truncation and damage records that
the target applies to a copy of the block the builder made (the input
layout is in each target's header comment). So the seeds are shapes worth
starting from (a cube, a tetrahedron, a ball's cloud, a grid, a crease, a
closed box, a two-sided sheet, inputs the builders refuse), each also with
damage that the validators must catch.
"""

import math
import os
import struct


def coord(v):
    """A coordinate in metres as the targets read it: i16, 1/256 m."""
    q = int(round(v * 256.0))
    return struct.pack("<h", max(-32768, min(32767, q)))


def points(pts):
    return b"".join(coord(x) + coord(y) + coord(z) for x, y, z in pts)


def damage(truncation=0, records=()):
    """The tail both targets read: a u16 truncation, then (u16 offset, u8 xor)."""
    out = struct.pack("<H", truncation)
    for offset, value in records:
        out += struct.pack("<HB", offset, value)
    return out


def hull_seed(budget, pts, tail=b""):
    return bytes([budget, len(pts)]) + points(pts) + tail


def mesh_seed(flags, verts, tris, tail=b""):
    out = bytes([flags, len(verts), len(tris)]) + points(verts)
    for a, b, c in tris:
        out += bytes([a, b, c])
    return out + tail


def cube(h=0.5, at=(0.0, 0.0, 0.0)):
    return [(at[0] + (h if i & 1 else -h), at[1] + (h if i & 2 else -h),
             at[2] + (h if i & 4 else -h)) for i in range(8)]


def ball(n, r):
    """Points spread over a ball by a golden-angle spiral (deterministic)."""
    out = []
    for i in range(n):
        y = 1.0 - 2.0 * (i + 0.5) / n
        s = math.sqrt(max(0.0, 1.0 - y * y))
        a = i * 2.399963229728653
        out.append((r * s * math.cos(a), r * y, r * s * math.sin(a)))
    return out


def grid(n, cell, height=None):
    verts = []
    for i in range(n + 1):
        for j in range(n + 1):
            x = (i - n / 2.0) * cell
            z = (j - n / 2.0) * cell
            verts.append((x, height(x, z) if height else 0.0, z))
    tris = []
    for i in range(n):
        for j in range(n):
            a = i * (n + 1) + j
            b, c, d = a + 1, a + n + 1, a + n + 2
            # Counter-clockwise seen from above (+Y): a, b, c then b, d, c.
            tris += [(a, b, c), (b, d, c)]
    return verts, tris


def write_physics_seeds(write, corpus):
    hull = os.path.join(corpus, "physics_hull")
    write(os.path.join(hull, "cube"), hull_seed(0, cube()))
    write(os.path.join(hull, "tetrahedron"),
          hull_seed(0, [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)]))
    write(os.path.join(hull, "ball_budget_16"), hull_seed(12, ball(60, 0.75)))
    write(os.path.join(hull, "ball_255"), hull_seed(0, ball(255, 2.0)))
    write(os.path.join(hull, "far_cube"), hull_seed(0, cube(0.25, (100.0, -90.0, 120.0))))
    write(os.path.join(hull, "flat"), hull_seed(0, [(x * 0.5, 0.0, z * 0.5)
                                                    for x in range(4) for z in range(4)]))
    write(os.path.join(hull, "repeated"), hull_seed(0, cube() + cube() + [(0.0, 0.0, 0.0)] * 4))
    # The cube's block damaged: header counts and offsets, a vertex, the
    # edges near the end, and truncations.
    write(os.path.join(hull, "cube_damaged_header"),
          hull_seed(0, cube(), damage(0, [(4, 0x10), (16, 0x01), (40, 0x80)])))
    write(os.path.join(hull, "cube_damaged_vertex"),
          hull_seed(0, cube(), damage(0, [(131, 0x7F), (140, 0xFF)])))
    write(os.path.join(hull, "cube_damaged_edges"),
          hull_seed(0, cube(), damage(0, [(460, 0x03), (470, 0x40), (500, 0x01)])))
    write(os.path.join(hull, "cube_truncated"), hull_seed(0, cube(), damage(100)))
    write(os.path.join(hull, "cube_short_edges"), hull_seed(0, cube(), damage(300)))

    mesh = os.path.join(corpus, "physics_mesh")
    quad_v = [(-1.0, 0.0, -1.0), (-1.0, 0.0, 1.0), (1.0, 0.0, -1.0), (1.0, 0.0, 1.0)]
    quad_t = [(0, 1, 2), (2, 1, 3)]
    write(os.path.join(mesh, "quad"), mesh_seed(0, quad_v, quad_t))
    gv, gt = grid(4, 0.5)
    write(os.path.join(mesh, "grid_4"), mesh_seed(0, gv, gt))
    hv, ht = grid(8, 0.25, lambda x, z: 0.2 * math.sin(2.0 * x) * math.cos(1.5 * z))
    write(os.path.join(mesh, "terrain_8"), mesh_seed(4, hv, ht))
    valley_v = [(-2.0, 2.0, -2.0), (-2.0, 2.0, 2.0), (0.0, 0.0, -2.0), (0.0, 0.0, 2.0),
                (2.0, 2.0, -2.0), (2.0, 2.0, 2.0)]
    valley_t = [(0, 1, 2), (2, 1, 3), (2, 3, 4), (4, 3, 5)]
    write(os.path.join(mesh, "valley"), mesh_seed(0, valley_v, valley_t))
    box_v = cube()
    # Outward, counter-clockwise faces of the cube (corner i: +x bit 0,
    # +y bit 1, +z bit 2).
    box_t = [(0, 4, 6), (0, 6, 2), (1, 3, 7), (1, 7, 5), (0, 1, 5), (0, 5, 4),
             (2, 6, 7), (2, 7, 3), (0, 2, 3), (0, 3, 1), (4, 5, 7), (4, 7, 6)]
    write(os.path.join(mesh, "closed_box"), mesh_seed(0, box_v, box_t))
    write(os.path.join(mesh, "two_sided_sheet"), mesh_seed(1, quad_v, quad_t))
    write(os.path.join(mesh, "split_vertices"),
          mesh_seed(2, quad_v + quad_v, [(0, 1, 2), (6, 5, 7)]))
    write(os.path.join(mesh, "degenerate"),
          mesh_seed(0, quad_v, [(0, 0, 1), (0, 1, 2), (1, 2, 2), (0, 1, 3)]))
    write(os.path.join(mesh, "index_out_of_range"), mesh_seed(0, quad_v, [(0, 1, 9)]))
    # The grid's block damaged: header, the root node's boxes and children,
    # vertices, triangles, and truncations.
    write(os.path.join(mesh, "grid_damaged_header"),
          mesh_seed(0, gv, gt, damage(0, [(8, 0x01), (16, 0x20), (36, 0x02)])))
    write(os.path.join(mesh, "grid_damaged_root"),
          mesh_seed(0, gv, gt, damage(0, [(64, 0x80), (70, 0x7F), (160, 0x01), (165, 0xFF)])))
    write(os.path.join(mesh, "grid_damaged_body"),
          mesh_seed(0, gv, gt, damage(0, [(400, 0x40), (700, 0x01), (900, 0x10)])))
    write(os.path.join(mesh, "grid_truncated"), mesh_seed(0, gv, gt, damage(200)))
