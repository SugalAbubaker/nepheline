// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the scene fuzz targets: a world with the scene tests'
// components (the names the seed corpora use; scripts/fuzz_seeds_scene.py)
// and a "crate" prefab for instances to name.

#include <nepheline/scene/prefab.h>
#include <nepheline/scene/scene_io.h>
#include <nepheline/scene/transform.h>

#include "test_scene_components.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Small enough that a hostile file runs out of entities quickly, large
// enough for the seeds.
#define FUZZ_SCENE_MAX_ENTITIES 4096U

// Aborts (a fuzzer finding) when `cond` is false, saying which.
#define FUZZ_REQUIRE(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: fuzz invariant failed: %s\n", __FILE__, __LINE__, #cond);      \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static inline NphWorld *fuzz_scene_world(void)
{
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.max_entities = FUZZ_SCENE_MAX_ENTITIES;
    NphWorld *w = NULL;
    FUZZ_REQUIRE(nph_world_create(&desc, &w) == NPH_RESULT_OK);
    const NphComponentDesc *descs[7] = {TestPosition_component(), TestVelocity_component(),
                                        TestHealth_component(),   TestAllKinds_component(),
                                        TestTagA_component(),     TestTagB_component(),
                                        TestBig_component()};
    for (u32 i = 0; i < 7U; ++i) {
        NphComponentId id;
        FUZZ_REQUIRE(nph_world_register_component(w, descs[i], &id) == NPH_RESULT_OK);
    }
    return w;
}

// The prefab of the seeds: a root with a position, a health that points
// at its lid, and a local transform; a lid child with a tag.
static inline NphPrefab *fuzz_scene_crate(const NphWorld *registry)
{
    NphPrefab *p = NULL;
    FUZZ_REQUIRE(nph_prefab_create(NPH_STR("crate"), registry, nph_allocator_system(), 16U, &p) ==
                 NPH_RESULT_OK);
    NphWorld *pw = nph_prefab_world(p);
    const NphComponentId position = nph_world_find_component(pw, NPH_STR("test.Position"));
    const NphComponentId health = nph_world_find_component(pw, NPH_STR("test.Health"));
    const NphComponentId tag = nph_world_find_component(pw, NPH_STR("test.TagA"));
    const NphComponentId rc[3] = {position, health, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    const NphComponentId lc[2] = {tag, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NphEntity root;
    NphEntity lid;
    FUZZ_REQUIRE(nph_world_spawn(pw, rc, 3U, &root) == NPH_RESULT_OK);
    FUZZ_REQUIRE(nph_world_spawn(pw, lc, 2U, &lid) == NPH_RESULT_OK);
    const TestHealth h = {100.0F, 100.0F, lid};
    FUZZ_REQUIRE(nph_world_set(pw, root, health, &h) == NPH_RESULT_OK);
    FUZZ_REQUIRE(nph_world_set_parent(pw, lid, root) == NPH_RESULT_OK);
    return p;
}

static inline const NphPrefab *fuzz_scene_resolve(void *user, NphStrId id)
{
    const NphPrefab *p = (const NphPrefab *)user;
    return nph_str_id_equals(nph_prefab_id(p), id) ? p : NULL;
}
