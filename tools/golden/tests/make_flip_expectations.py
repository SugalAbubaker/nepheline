# SPDX-License-Identifier: Apache-2.0
"""Computes the expected values of test_flip.c with NVIDIA's own FLIP
implementation (the `flip-evaluator` package, BSD-3-Clause, version 1.7),
so our LDR-FLIP is checked against the authors' code, not against itself:

    python -m venv venv && venv/bin/pip install flip-evaluator==1.7 numpy
    venv/bin/python tools/golden/tests/make_flip_expectations.py

It prints the C table test_flip.c holds. The image pairs are generated
here and in test_flip.c by the same formulas (flip_pair in both files).
flip-evaluator is a development tool for this script only; nothing in the
build or the tests uses it.
"""
import numpy as np
import flip_evaluator as flip

W, H = 64, 48


def pattern(x, y):
    return ((x * 7 + y * 13) & 255, (x * x + y) & 255, ((x ^ y) * 4) & 255)


def flip_pair(case):
    """Returns (reference, test) as H x W x 3 uint8 arrays."""
    ref = np.zeros((H, W, 3), np.uint8)
    test = np.zeros((H, W, 3), np.uint8)
    for y in range(H):
        for x in range(W):
            if case == 0:  # identical patterns
                ref[y, x] = test[y, x] = pattern(x, y)
            elif case == 1:  # uniform greys
                ref[y, x] = (128, 128, 128)
                test[y, x] = (140, 140, 140)
            elif case == 2:  # white against black
                ref[y, x] = (255, 255, 255)
                test[y, x] = (0, 0, 0)
            elif case == 3:  # a red offset
                p = pattern(x, y)
                ref[y, x] = p
                test[y, x] = (min(255, p[0] + 40), p[1], p[2])
            elif case == 4:  # a checkerboard shifted by one texel
                c = 230 if ((x // 8) + (y // 8)) & 1 else 25
                d = 230 if (((x + 1) // 8) + (y // 8)) & 1 else 25
                ref[y, x] = (c, c, c)
                test[y, x] = (d, d, d)
            else:  # a hue change: reddish against greenish
                ref[y, x] = (200, 80, 60)
                test[y, x] = (80, 200, 60)
    return ref, test


def main():
    rows = []
    for case in range(6):
        ref, test = flip_pair(case)
        m, _, _ = flip.evaluate(ref.astype(np.float32) / 255.0, test.astype(np.float32) / 255.0,
                                "LDR", applyMagma=False, parameters={"ppd": 67.0})
        m = np.asarray(m, np.float64).reshape(H, W)
        rows.append(f"    {{{m.mean():.6f}, {m[24, 32]:.6f}, {m[5, 3]:.6f}, {m.max():.6f}}},"
                    f" // case {case}")
    print("// {mean, error at (32, 24), error at (3, 5), max}, from flip-evaluator 1.7")
    print("static const f64 g_expected[6][4] = {")
    print("\n".join(rows))
    print("};")


main()
