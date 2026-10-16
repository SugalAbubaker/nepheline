// SPDX-License-Identifier: Apache-2.0
// The editor's selection (editor/src/selection.h).
#include <ntest/ntest.h>

#include "selection.h"

#include <string.h>

static EditorSelection g_sel; // ~4 KB: kept off the stack

static NphEntity entity(u32 value)
{
    NphEntity e;
    e.value = value;
    return e;
}

NTEST(EditorSelection, clicks_set_add_and_toggle_with_a_primary)
{
    EditorSelection *s = &g_sel;
    editor_selection_clear(s);
    NTEST_EXPECT_EQ_UINT(s->count, 0U);
    NTEST_EXPECT_TRUE(nph_entity_is_null(s->primary));
    editor_selection_set(s, entity(7U));
    NTEST_EXPECT_EQ_UINT(s->count, 1U);
    NTEST_EXPECT_EQ_UINT(s->primary.value, 7U);
    editor_selection_add(s, entity(9U));
    editor_selection_add(s, entity(9U)); // once
    NTEST_EXPECT_EQ_UINT(s->count, 2U);
    NTEST_EXPECT_EQ_UINT(s->primary.value, 9U);
    NTEST_EXPECT_TRUE(editor_selection_contains(s, entity(7U)));
    // Toggling the primary off: the primary moves to the last one left.
    editor_selection_toggle(s, entity(9U));
    NTEST_EXPECT_EQ_UINT(s->count, 1U);
    NTEST_EXPECT_EQ_UINT(s->primary.value, 7U);
    editor_selection_toggle(s, entity(11U));
    NTEST_EXPECT_EQ_UINT(s->primary.value, 11U);
    // Null selects nothing.
    editor_selection_set(s, NPH_ENTITY_NULL);
    NTEST_EXPECT_EQ_UINT(s->count, 0U);
    NTEST_EXPECT_TRUE(nph_entity_is_null(s->primary));
}

NTEST(EditorSelection, it_holds_a_bounded_number_and_drops_what_is_gone)
{
    EditorSelection *s = &g_sel;
    editor_selection_clear(s);
    for (u32 i = 1; i <= EDITOR_SELECTION_MAX + 10U; ++i) {
        editor_selection_add(s, entity(i));
    }
    NTEST_EXPECT_EQ_UINT(s->count, EDITOR_SELECTION_MAX);

    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_allocator_system();
    wd.max_entities = 16U;
    NphWorld *w = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &w), NPH_RESULT_OK);
    NphEntity e[3];
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, NULL, 0U, 3U, e), NPH_RESULT_OK);
    editor_selection_set(s, e[0]);
    editor_selection_add(s, e[1]);
    editor_selection_add(s, e[2]);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e[2]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, e[0]), NPH_RESULT_OK);
    editor_selection_prune(s, w);
    NTEST_EXPECT_EQ_UINT(s->count, 1U);
    NTEST_EXPECT_TRUE(nph_entity_equals(s->primary, e[1]));
    nph_world_release(w, e[0]);
    nph_world_destroy(w);
}
