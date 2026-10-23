# SPDX-License-Identifier: Apache-2.0
"""Seed corpora of the physics fuzz targets (tests/fuzz/fuzz_physics_hull.c,
fuzz_physics_mesh.c, and fuzz_physics_heightfield.c), written by
`python scripts/fuzz_seeds.py`.

The targets read builder input, not blocks: a point cloud, a triangle
soup, or a grid of heights, with coordinates in 1/256 m, then a truncation and damage records that
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

    field = os.path.join(corpus, "physics_heightfield")

    def field_seed(flags, columns, rows, heights, cells=None, tail=b""):
        # The target reads 2 + byte % 31 samples a side.
        out = bytes([flags, columns - 2, rows - 2])
        out += b"".join(coord(y) for y in heights)
        if cells is not None:
            out += bytes(cells)
        return out + tail

    write(os.path.join(field, "flat_4"), field_seed(0x11, 4, 4, [0.0] * 16))
    waves = [0.3 * math.sin(0.9 * i) * math.cos(0.7 * j) for j in range(9) for i in range(9)]
    write(os.path.join(field, "waves_9"), field_seed(0x00, 9, 9, waves))
    steps = [0.25 * ((i * 7 + j * 3) % 5) for j in range(12) for i in range(10)]
    write(os.path.join(field, "steps_10x12_spaced"), field_seed(0x23, 10, 12, steps))
    holes = [1 if (c % 5) == 2 else 0 for c in range(8 * 8)]
    flips = [2 if (c % 3) == 0 else 0 for c in range(8 * 8)]
    mixed = [h | f for h, f in zip(holes, flips)]
    write(os.path.join(field, "holes_and_flips_9"), field_seed(0x08, 9, 9, waves, mixed))
    write(os.path.join(field, "ridge_30deg_9"),
          field_seed(0x04, 9, 9, [1.0 - 0.25 * abs(j - 4) for j in range(9) for i in range(9)]))
    big = [0.2 * math.sin(0.4 * i + 0.3 * j) for j in range(32) for i in range(32)]
    write(os.path.join(field, "big_32"), field_seed(0x00, 32, 32, big))
    # Damage: the header, the heights, the blocks at the end, truncations.
    write(os.path.join(field, "waves_damaged_header"),
          field_seed(0x00, 9, 9, waves, None, damage(0, [(4, 0x01), (16, 0x80), (24, 0x10)])))
    write(os.path.join(field, "waves_damaged_heights"),
          field_seed(0x00, 9, 9, waves, None, damage(0, [(90, 0xFF), (131, 0x7F), (200, 0x01)])))
    write(os.path.join(field, "waves_damaged_cells"),
          field_seed(0x08, 9, 9, waves, mixed, damage(0, [(420, 0x03), (430, 0xFC), (440, 0x01)])))
    write(os.path.join(field, "waves_truncated"), field_seed(0x00, 9, 9, waves, None, damage(100)))
