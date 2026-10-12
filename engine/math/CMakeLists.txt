# SPDX-License-Identifier: Apache-2.0
nph_add_module(math
    SOURCES
        src/affine.c
        src/mat.c
        src/quat.c
    PUBLIC_DEPS
        nph::core
)

nph_add_test(math
    SOURCES
        tests/test_affine.c
        tests/test_mat.c
        tests/test_math_support.h
        tests/test_quat.c
        tests/test_scalar.c
        tests/test_vec.c
    DEPS
        nph::math
)
