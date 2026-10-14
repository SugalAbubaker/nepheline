# SPDX-License-Identifier: Apache-2.0
# The asset module: the `.nph` container, pack archives, the virtual file
# system, the LZ codec, and the loader (CLAUDE.md §8.8; docs/asset.md;
# docs/adr/0016-asset-format.md). Depends on jobs (asynchronous loads,
# parallel decompression) and through it on platform, math, and core.

nph_add_module(asset
    SOURCES
        src/asset.c
        src/format.c
        src/guid.c
        src/lz.c
        src/pack.c
        src/vfs.c
    PUBLIC_DEPS nph::jobs
)
if(MSVC)
    target_compile_definitions(nph_asset PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(asset
    SOURCES
        tests/test_asset_support.h
        tests/test_format.c
        tests/test_guid.c
        tests/test_lz.c
        tests/test_pack.c
        tests/test_vfs.c
    DEPS
        nph::asset
)
if(NPH_BUILD_TESTS)
    if(MSVC)
        target_compile_definitions(asset_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()
