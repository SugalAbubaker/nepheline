# SPDX-License-Identifier: Apache-2.0
nph_add_module(core
    SOURCES
        src/version.c
)

nph_add_test(core
    SOURCES
        tests/test_version.c
    DEPS
        nph::core
)
