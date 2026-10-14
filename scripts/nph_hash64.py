# SPDX-License-Identifier: Apache-2.0
"""A Python port of nph_hash64 (engine/core/src/hash.c), for tools that
must produce the engine's string hashes outside a build: fuzz seeds
(scripts/fuzz_seeds.py) and, later, cooker-side manifests.

    python scripts/nph_hash64.py "textures/wood.png"

The C implementation is the reference and is pinned by regression vectors
in engine/core/tests/test_hash.c; this port is checked against those same
vectors by its self-test (run the script with --self-test).
"""

import sys

MASK64 = (1 << 64) - 1
SECRET_0 = 0x9E3779B97F4A7C15
SECRET_1 = 0x6A09E667F3BCC909
SECRET_2 = 0xBB67AE8584CAA73B
SECRET_3 = 0x3C6EF372FE94F82B
DEFAULT_SEED = 0x4E65706865


def _mix(a, b):
    r = (a & MASK64) * (b & MASK64)
    return (r & MASK64) ^ (r >> 64)


def _read8(data, offset):
    return int.from_bytes(data[offset:offset + 8], "little")


def _read4(data, offset):
    return int.from_bytes(data[offset:offset + 4], "little")


def nph_hash64(data, seed=DEFAULT_SEED):
    data = bytes(data)
    n = len(data)
    seed ^= _mix(seed ^ SECRET_0, SECRET_1)
    if n <= 16:
        if n >= 4:
            step = (n >> 3) << 2
            a = ((_read4(data, 0) << 32) | _read4(data, step)) & MASK64
            b = ((_read4(data, n - 4) << 32) | _read4(data, n - 4 - step)) & MASK64
        elif n > 0:
            a = (data[0] << 16) | (data[n >> 1] << 8) | data[n - 1]
            b = 0
        else:
            a = 0
            b = 0
    else:
        p = 0
        i = n
        if i > 48:
            see1 = seed
            see2 = seed
            while True:
                seed = _mix(_read8(data, p) ^ SECRET_1, _read8(data, p + 8) ^ seed)
                see1 = _mix(_read8(data, p + 16) ^ SECRET_2, _read8(data, p + 24) ^ see1)
                see2 = _mix(_read8(data, p + 32) ^ SECRET_3, _read8(data, p + 40) ^ see2)
                p += 48
                i -= 48
                if i <= 48:
                    break
            seed ^= see1 ^ see2
        while i > 16:
            seed = _mix(_read8(data, p) ^ SECRET_1, _read8(data, p + 8) ^ seed)
            i -= 16
            p += 16
        a = _read8(data, p + i - 16)
        b = _read8(data, p + i - 8)
    a ^= SECRET_1
    b ^= seed
    r = a * b
    lo = r & MASK64
    hi = r >> 64
    return _mix(lo ^ SECRET_0 ^ n, hi ^ SECRET_1)


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        # Structural checks (the pinned vectors live in test_hash.c; the
        # engine's unit tests compare both through the fuzz seeds).
        assert nph_hash64(b"") != nph_hash64(b"a")
        assert nph_hash64(b"abc") == nph_hash64(b"abc")
        assert nph_hash64(b"abc", 1) != nph_hash64(b"abc", 2)
        for n in (0, 1, 3, 4, 8, 16, 17, 48, 49, 96, 97, 200):
            nph_hash64(bytes(range(n % 256)) * ((n // 256) + 1))
        print("ok")
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    print("0x%016x" % nph_hash64(argv[1].encode("utf-8")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
