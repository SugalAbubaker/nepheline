# SPDX-License-Identifier: Apache-2.0
nph_add_module(core
    SOURCES
        src/alloc.c
        src/alloc_debug.c
        src/arena.c
        src/array.c
        src/assert.c
        src/bitset.c
        src/cvar.c
        src/handle.c
        src/hash.c
        src/hashmap.c
        src/json.c
        src/log.c
        src/pool.c
        src/result.c
        src/ring.c
        src/serialize.c
        src/sparse_set.c
        src/stack_alloc.c
        src/string.c
        src/string_id.c
        src/tlsf.c
        src/version.c
)

if(MSVC)
    # fopen/timespec_get are portable C; the "secure" CRT variants are not.
    target_compile_definitions(nph_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

nph_add_test(core
    SOURCES
        tests/test_alloc.c
        tests/test_alloc_debug.c
        tests/test_arena.c
        tests/test_array.c
        tests/test_assert.c
        tests/test_atomic.c
        tests/test_base.c
        tests/test_bitset.c
        tests/test_cvar.c
        tests/test_handle.c
        tests/test_hash.c
        tests/test_hashmap.c
        tests/test_json.c
        tests/test_log.c
        tests/test_pool.c
        tests/test_ring.c
        tests/test_serialize.c
        tests/test_sparse_set.c
        tests/test_stack_alloc.c
        tests/test_string.c
        tests/test_support.h
        tests/test_tlsf.c
        tests/test_version.c
    DEPS
        nph::core
)
if(NPH_BUILD_TESTS AND MSVC)
    target_compile_definitions(core_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
