# SPDX-License-Identifier: Apache-2.0
nph_add_module(math
    SOURCES
        src/affine.c
        src/geometry.c
        src/mat.c
        src/pack.c
        src/quat.c
        src/random.c
    PUBLIC_DEPS
        nph::core
)

nph_add_test(math
    SOURCES
        tests/test_affine.c
        tests/test_geometry.c
        tests/test_mat.c
        tests/test_math_support.h
        tests/test_pack.c
        tests/test_quat.c
        tests/test_random.c
        tests/test_scalar.c
        tests/test_vec.c
    DEPS
        nph::math
)
