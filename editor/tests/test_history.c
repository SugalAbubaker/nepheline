// SPDX-License-Identifier: Apache-2.0
// The undo history and the edits on it (editor/src/history.h, edit.h), on a
// plain world: every change goes both ways, deleted and undone entities
// come back under their own handles, drags merge into one step, the states
// say when the document is clean, and the history lets go of what it can
// no longer bring back.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/math/scalar.h>
#include <nepheline/scene/component.h>

#include <ntest/ntest.h>

#include "edit.h"
#include "history.h"

#include <string.h>

// A component with a reference, and a tag.
#define TEST_LINK_FIELDS(F)                                                                        \
    F(ENTITY, target, NPH_FIELD_NO_HINTS)                                                          \
    F(F32, weight, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(TestLink, TEST_LINK_FIELDS);
NPH_COMPONENT_DECLARE(TestLink);
NPH_COMPONENT_DEFINE(TestLink, TEST_LINK_FIELDS, "test.Link", 0U, NULL)
NPH_TAG_DECLARE(TestMark);
NPH_TAG_DEFINE(TestMark, "test.Mark", 0U)

typedef struct HistoryFixture {
    NphDebugAllocator da;
    NphWorld *world;
    EditorHistory *h;
    NphComponentId link;
    NphComponentId mark;
} HistoryFixture;

static void open_history(HistoryFixture *f, u32 max_steps)
{
    if (f->h != NULL) {
        editor_history_destroy(f->h);
    }
    EditorHistoryDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.world = f->world;
    d.max_steps = max_steps;
    NTEST_ASSERT_EQ_INT(editor_history_create(&d, &f->h), NPH_RESULT_OK);
}

NTEST_SETUP(HistoryFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_debug_allocator_allocator(&f->da);
    wd.max_entities = 256U;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &f->world), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_register_component(f->world, TestLink_component(), &f->link),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_register_component(f->world, TestMark_component(), &f->mark),
                        NPH_RESULT_OK);
    f->h = NULL;
    open_history(f, 0U);
}

NTEST_TEARDOWN(HistoryFixture)
{
    editor_history_destroy(f->h);
    // The history lets go of every handle it kept suspended.
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(f->world), 0U);
    nph_world_destroy(f->world);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

static NphEntity create(HistoryFixture *f, const char *name, NphEntity parent, f64 x)
{
    NphLocalTransform t = nph_local_transform_identity();
    t.translation.x = x;
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(f->h, name, parent, &t, &e), NPH_RESULT_OK);
    return e;
}

static const char *name_of(HistoryFixture *f, NphEntity e)
{
    const NphName *n = NPH_WORLD_GET(f->world, e, NPH_COMPONENT_ID_NAME, NphName);
    return n != NULL ? n->text.text : "";
}

static f64 x_of(HistoryFixture *f, NphEntity e)
{
    return NPH_WORLD_GET(f->world, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
        ->translation.x;
}

static void undo(HistoryFixture *f)
{
    NTEST_ASSERT_EQ_INT(editor_history_undo(f->h), NPH_RESULT_OK);
}

static void redo(HistoryFixture *f)
{
    NTEST_ASSERT_EQ_INT(editor_history_redo(f->h), NPH_RESULT_OK);
}

// ---------------------------------------------------------------------------
// The history
// ---------------------------------------------------------------------------

NTEST_F(HistoryFixture, every_kind_of_change_goes_both_ways)
{
    const NphEntity e = create(f, "first", NPH_ENTITY_NULL, 1.0);
    NTEST_EXPECT_EQ_STR(editor_history_undo_label(f->h), "Create Entity");
    NTEST_EXPECT_TRUE(nph_world_has(f->world, e, NPH_COMPONENT_ID_WORLD_TRANSFORM));
    // set
    NTEST_ASSERT_EQ_INT(editor_edit_rename(f->h, e, "renamed"), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(name_of(f, e), "renamed");
    undo(f);
    NTEST_EXPECT_EQ_STR(name_of(f, e), "first");
    redo(f);
    NTEST_EXPECT_EQ_STR(name_of(f, e), "renamed");
    // add and remove, a value and a tag
    NTEST_ASSERT_EQ_INT(editor_edit_add_component(f->h, e, f->link), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_edit_add_component(f->h, e, f->mark), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(editor_history_undo_label(f->h), "Add test.Mark");
    TestLink link = {e, 2.5F};
    NTEST_ASSERT_EQ_INT(editor_history_begin(f->h, "Link"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_set(f->h, e, f->link, &link), NPH_RESULT_OK);
    editor_history_end(f->h);
    NTEST_ASSERT_EQ_INT(editor_edit_remove_component(f->h, e, f->link), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_world_has(f->world, e, f->link));
    undo(f); // the removal: the value comes back with the component
    const TestLink *back = NPH_WORLD_GET(f->world, e, f->link, TestLink);
    NTEST_ASSERT_NOT_NULL(back);
    NTEST_EXPECT_NEAR_F32(back->weight, 2.5F, 0.0F);
    NTEST_EXPECT_TRUE(nph_entity_equals(back->target, e));
    undo(f); // the set
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(f->world, e, f->link, TestLink)->weight, 0.0F, 0.0F);
    undo(f); // the tag
    NTEST_EXPECT_FALSE(nph_world_has(f->world, e, f->mark));
    undo(f); // the link
    NTEST_EXPECT_FALSE(nph_world_has(f->world, e, f->link));
    for (u32 i = 0; i < 4U; ++i) {
        redo(f);
    }
    NTEST_EXPECT_TRUE(nph_world_has(f->world, e, f->mark));
    NTEST_EXPECT_FALSE(nph_world_has(f->world, e, f->link)); // removed again
    NTEST_EXPECT_EQ_INT(editor_history_redo(f->h), NPH_RESULT_EMPTY);
}

NTEST_F(HistoryFixture, an_undone_spawn_comes_back_under_its_own_handle)
{
    const NphEntity e = create(f, "spawned", NPH_ENTITY_NULL, 3.0);
    undo(f);
    NTEST_EXPECT_FALSE(nph_world_is_alive(f->world, e));
    NTEST_EXPECT_TRUE(nph_world_is_suspended(f->world, e));
    redo(f);
    NTEST_ASSERT_TRUE(nph_world_is_alive(f->world, e));
    NTEST_EXPECT_EQ_STR(name_of(f, e), "spawned"); // the step's set replayed
    NTEST_EXPECT_NEAR_F64(x_of(f, e), 3.0, 0.0);
}

NTEST_F(HistoryFixture, a_deleted_subtree_comes_back_whole_with_references_intact)
{
    const NphEntity root = create(f, "root", NPH_ENTITY_NULL, 0.0);
    const NphEntity a = create(f, "a", root, 1.0);
    const NphEntity b = create(f, "b", root, 2.0);
    const NphEntity c = create(f, "c", root, 3.0);
    const NphEntity grandchild = create(f, "grandchild", b, 4.0);
    const NphEntity outside = create(f, "outside", NPH_ENTITY_NULL, 5.0);
    TestLink link = {b, 1.0F};
    NTEST_ASSERT_EQ_INT(editor_history_begin(f->h, "Link"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_set(f->h, outside, f->link, &link), NPH_RESULT_OK);
    editor_history_end(f->h);
    const u32 count = nph_world_entity_count(f->world);

    // Deleting the root and one of its children: the child goes once.
    const NphEntity doomed[2] = {b, root};
    NTEST_ASSERT_EQ_INT(editor_edit_delete(f->h, doomed, 2U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(editor_history_undo_label(f->h), "Delete 2 entities");
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(f->world), count - 5U);
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(f->world), 5U);
    // The reference still names b, which is gone for now.
    NTEST_EXPECT_TRUE(
        nph_entity_equals(NPH_WORLD_GET(f->world, outside, f->link, TestLink)->target, b));

    undo(f);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(f->world), count);
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(f->world), 0U);
    // The hierarchy, in its order, and the values.
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_first_child(f->world, root), a));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_next_sibling(f->world, a), b));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_next_sibling(f->world, b), c));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(f->world, grandchild), b));
    NTEST_EXPECT_EQ_STR(name_of(f, grandchild), "grandchild");
    NTEST_EXPECT_NEAR_F64(x_of(f, c), 3.0, 0.0);
    NTEST_EXPECT_TRUE(nph_world_has(f->world, grandchild, NPH_COMPONENT_ID_WORLD_TRANSFORM));
    // And the reference means the same entity again.
    NTEST_EXPECT_TRUE(
        nph_world_is_alive(f->world, NPH_WORLD_GET(f->world, outside, f->link, TestLink)->target));

    redo(f);
    NTEST_EXPECT_FALSE(nph_world_is_alive(f->world, grandchild));
    undo(f);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(f->world, grandchild), b));
}

NTEST_F(HistoryFixture, deleting_one_parent_alone_brings_its_children_back_under_it)
{
    const NphEntity parent = create(f, "parent", NPH_ENTITY_NULL, 0.0);
    const NphEntity x = create(f, "x", parent, 1.0);
    const NphEntity y = create(f, "y", parent, 2.0);
    NTEST_ASSERT_EQ_INT(editor_history_begin(f->h, "Delete one"), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_delete(f->h, parent), NPH_RESULT_OK);
    editor_history_end(f->h);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(f->world, x))); // roots now
    undo(f);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_first_child(f->world, parent), x));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_next_sibling(f->world, x), y));
}

NTEST_F(HistoryFixture, a_drag_is_one_step_and_the_states_track_saving)
{
    const NphEntity e = create(f, "dragged", NPH_ENTITY_NULL, 0.0);
    const u64 saved = editor_history_state(f->h);
    NTEST_EXPECT_NE_UINT(saved, 0U);
    NphLocalTransform t = nph_local_transform_identity();
    u64 previous = saved;
    for (u32 i = 1; i <= 10U; ++i) {
        t.translation.x = (f64)i;
        NTEST_ASSERT_EQ_INT(
            editor_history_set_merged(f->h, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &t, 77U, "Move"),
            NPH_RESULT_OK);
        NTEST_EXPECT_NE_UINT(editor_history_state(f->h), previous); // every frame is a change
        previous = editor_history_state(f->h);
    }
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(f->h), 2U);
    NTEST_EXPECT_NEAR_F64(x_of(f, e), 10.0, 0.0);
    // The same value again records nothing.
    NTEST_ASSERT_EQ_INT(
        editor_history_set_merged(f->h, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &t, 77U, "Move"),
        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(editor_history_state(f->h), previous);
    editor_history_end_merge(f->h);
    t.translation.x = 20.0;
    NTEST_ASSERT_EQ_INT(
        editor_history_set_merged(f->h, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &t, 77U, "Move"),
        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(f->h), 3U); // a new drag, a new step
    undo(f);
    NTEST_EXPECT_NEAR_F64(x_of(f, e), 10.0, 0.0);
    undo(f); // the whole first drag at once
    NTEST_EXPECT_NEAR_F64(x_of(f, e), 0.0, 0.0);
    // Back where it was saved: the state says so.
    NTEST_EXPECT_EQ_UINT(editor_history_state(f->h), saved);
    redo(f);
    NTEST_EXPECT_EQ_UINT(editor_history_state(f->h), previous);
    undo(f);
    undo(f);
    NTEST_EXPECT_EQ_UINT(editor_history_state(f->h), 0U); // before any step
}

NTEST_F(HistoryFixture, a_new_edit_lets_go_of_what_only_redo_could_bring_back)
{
    const NphEntity e = create(f, "temporary", NPH_ENTITY_NULL, 0.0);
    undo(f);
    NTEST_EXPECT_TRUE(nph_world_is_suspended(f->world, e));
    NTEST_EXPECT_TRUE(editor_history_can_redo(f->h));
    (void)create(f, "replacement", NPH_ENTITY_NULL, 0.0);
    NTEST_EXPECT_FALSE(editor_history_can_redo(f->h));
    NTEST_EXPECT_FALSE(nph_world_is_suspended(f->world, e));
    NTEST_EXPECT_FALSE(nph_world_is_alive(f->world, e));
}

NTEST_F(HistoryFixture, the_oldest_steps_go_and_take_their_deletions_with_them)
{
    open_history(f, 3U);
    const NphEntity e = create(f, "deleted early", NPH_ENTITY_NULL, 0.0);
    NTEST_ASSERT_EQ_INT(editor_edit_delete(f->h, &e, 1U), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_world_is_suspended(f->world, e));
    for (u32 i = 0; i < 3U; ++i) {
        (void)create(f, "later", NPH_ENTITY_NULL, (f64)i);
    }
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(f->h), 3U);
    NTEST_EXPECT_FALSE(nph_world_is_suspended(f->world, e)); // released with its step
    NTEST_EXPECT_FALSE(nph_world_is_alive(f->world, e));
    NTEST_EXPECT_GT_UINT(editor_history_bytes(f->h), 0U);
    editor_history_clear(f->h);
    NTEST_EXPECT_EQ_UINT(editor_history_bytes(f->h), 0U);
    NTEST_EXPECT_FALSE(editor_history_can_undo(f->h));
}

NTEST_F(HistoryFixture, an_aborted_step_leaves_nothing_behind)
{
    const NphEntity kept = create(f, "kept", NPH_ENTITY_NULL, 1.0);
    const u32 count = nph_world_entity_count(f->world);
    NTEST_ASSERT_EQ_INT(editor_history_begin(f->h, "Half done"), NPH_RESULT_OK);
    NphEntity spawned = NPH_ENTITY_NULL;
    const NphComponentId ids[1] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NTEST_ASSERT_EQ_INT(editor_history_spawn(f->h, ids, 1U, &spawned), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_delete(f->h, kept), NPH_RESULT_OK);
    editor_history_abort(f->h);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(f->world), count);
    NTEST_EXPECT_TRUE(nph_world_is_alive(f->world, kept));
    NTEST_EXPECT_FALSE(nph_world_is_alive(f->world, spawned));
    NTEST_EXPECT_FALSE(nph_world_is_suspended(f->world, spawned));
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(f->h), 1U); // the create only
}

NTEST_F(HistoryFixture, mistakes_are_refused_and_outside_changes_clear_the_history)
{
    const NphEntity e = create(f, "e", NPH_ENTITY_NULL, 0.0);
    NphName n;
    memset(&n, 0, sizeof n);
    // No step open; a step open twice; undo while recording.
    NTEST_EXPECT_EQ_INT(editor_history_set(f->h, e, NPH_COMPONENT_ID_NAME, &n),
                        NPH_RESULT_INVALID_STATE);
    NTEST_ASSERT_EQ_INT(editor_history_begin(f->h, "a"), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(editor_history_begin(f->h, "b"), NPH_RESULT_INVALID_STATE);
    NTEST_EXPECT_EQ_INT(editor_history_undo(f->h), NPH_RESULT_INVALID_STATE);
    NTEST_EXPECT_FALSE(editor_history_can_undo(f->h));
    // The internal Hierarchy, an unknown component, and dead entities.
    NphHierarchy hier;
    memset(&hier, 0, sizeof hier);
    NTEST_EXPECT_EQ_INT(editor_history_set(f->h, e, NPH_COMPONENT_ID_HIERARCHY, &hier),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_history_add(f->h, e, 250U, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_history_remove(f->h, e, f->link), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(editor_history_set(f->h, NPH_ENTITY_NULL, NPH_COMPONENT_ID_NAME, &n),
                        NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(editor_history_add(f->h, e, NPH_COMPONENT_ID_NAME, NULL),
                        NPH_RESULT_ALREADY_EXISTS);
    editor_history_end(f->h); // nothing recorded: dropped
    NTEST_EXPECT_EQ_UINT(editor_history_undo_count(f->h), 1U);
    // A cycle.
    const NphEntity child = create(f, "child", e, 0.0);
    NTEST_EXPECT_EQ_INT(editor_edit_reparent(f->h, e, child), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(editor_edit_reparent(f->h, e, e), NPH_RESULT_INVALID_ARGUMENT);
    // The world changed behind the history's back: undo fails and clears.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(f->world, child), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(editor_history_undo(f->h), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_FALSE(editor_history_can_undo(f->h));
    NTEST_EXPECT_FALSE(editor_history_can_redo(f->h));
}

// ---------------------------------------------------------------------------
// The edits
// ---------------------------------------------------------------------------

NTEST_F(HistoryFixture, duplicates_copy_subtrees_and_point_their_references_at_the_copies)
{
    const NphEntity parent = create(f, "parent", NPH_ENTITY_NULL, 0.0);
    const NphEntity original = create(f, "original", parent, 1.0);
    const NphEntity inner = create(f, "inner", original, 2.0);
    const NphEntity outside = create(f, "outside", NPH_ENTITY_NULL, 9.0);
    NTEST_ASSERT_EQ_INT(editor_history_begin(f->h, "Links"), NPH_RESULT_OK);
    const TestLink to_inner = {inner, 1.0F};
    const TestLink to_outside = {outside, 2.0F};
    NTEST_ASSERT_EQ_INT(editor_history_set(f->h, original, f->link, &to_inner), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_set(f->h, inner, f->link, &to_outside), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(editor_history_add(f->h, inner, f->mark, NULL), NPH_RESULT_OK);
    editor_history_end(f->h);

    // The child is under the original: it is copied once, with it.
    const NphEntity which[2] = {original, inner};
    NphEntity copies[2];
    NTEST_ASSERT_EQ_INT(editor_edit_duplicate(f->h, which, 2U, copies), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_world_is_alive(f->world, copies[0]));
    NTEST_EXPECT_TRUE(nph_entity_is_null(copies[1]));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(f->world, copies[0]), parent));
    NTEST_EXPECT_EQ_STR(name_of(f, copies[0]), "original");
    NTEST_EXPECT_NEAR_F64(x_of(f, copies[0]), 1.0, 0.0);
    const NphEntity inner_copy = nph_world_first_child(f->world, copies[0]);
    NTEST_ASSERT_FALSE(nph_entity_is_null(inner_copy));
    NTEST_EXPECT_FALSE(nph_entity_equals(inner_copy, inner));
    NTEST_EXPECT_EQ_STR(name_of(f, inner_copy), "inner");
    NTEST_EXPECT_TRUE(nph_world_has(f->world, inner_copy, f->mark));
    // Inside the subtree the reference follows the copy; outside it stays.
    NTEST_EXPECT_TRUE(nph_entity_equals(
        NPH_WORLD_GET(f->world, copies[0], f->link, TestLink)->target, inner_copy));
    NTEST_EXPECT_TRUE(
        nph_entity_equals(NPH_WORLD_GET(f->world, inner_copy, f->link, TestLink)->target, outside));
    // One step: undo takes both copies away.
    const u32 count = nph_world_entity_count(f->world);
    undo(f);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(f->world), count - 2U);
    redo(f);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_first_child(f->world, copies[0]), inner_copy));
}

NTEST_F(HistoryFixture, reparenting_keeps_the_entity_where_it_is_in_the_world)
{
    NphLocalTransform pt = nph_local_transform_identity();
    pt.translation = nph_dvec3(10.0, 0.0, -4.0);
    pt.rotation = nph_quat_from_axis_angle(nph_vec3(0.0F, 1.0F, 0.0F), 0.7F);
    pt.scale = nph_vec3(2.0F, 2.0F, 2.0F);
    NphEntity parent = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(f->h, "parent", NPH_ENTITY_NULL, &pt, &parent),
                        NPH_RESULT_OK);
    NphLocalTransform ct = nph_local_transform_identity();
    ct.translation = nph_dvec3(3.0, 1.0, 2.0);
    ct.rotation = nph_quat_from_axis_angle(nph_vec3(1.0F, 0.0F, 0.0F), -0.4F);
    NphEntity child = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(editor_edit_create(f->h, "child", NPH_ENTITY_NULL, &ct, &child),
                        NPH_RESULT_OK);
    const NphWorldTransform before = editor_world_transform_of(f->world, child);
    NTEST_ASSERT_EQ_INT(editor_edit_reparent(f->h, child, parent), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(f->world, child), parent));
    const NphWorldTransform after = editor_world_transform_of(f->world, child);
    NTEST_EXPECT_NEAR_F64(after.translation.x, before.translation.x, 1e-4);
    NTEST_EXPECT_NEAR_F64(after.translation.y, before.translation.y, 1e-4);
    NTEST_EXPECT_NEAR_F64(after.translation.z, before.translation.z, 1e-4);
    NTEST_EXPECT_NEAR_F32(after.x_axis.x, before.x_axis.x, 1e-4F);
    NTEST_EXPECT_NEAR_F32(after.y_axis.z, before.y_axis.z, 1e-4F);
    NTEST_EXPECT_NEAR_F32(after.z_axis.y, before.z_axis.y, 1e-4F);
    // The local scale undoes the parent's.
    NTEST_EXPECT_NEAR_F32(
        NPH_WORLD_GET(f->world, child, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)
            ->scale.x,
        0.5F, 1e-5F);
    undo(f);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(f->world, child)));
    NTEST_EXPECT_NEAR_F64(x_of(f, child), 3.0, 0.0);
    // Back to a root keeps it in place too.
    redo(f);
    NTEST_ASSERT_EQ_INT(editor_edit_reparent(f->h, child, NPH_ENTITY_NULL), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F64(x_of(f, child), before.translation.x, 1e-4);
}

NTEST_F(HistoryFixture, labels_and_world_transforms_for_the_panels)
{
    const NphEntity root = create(f, "root", NPH_ENTITY_NULL, 5.0);
    const NphEntity child = create(f, "", root, 1.0);
    char label[64];
    editor_entity_label(f->world, root, label, sizeof label);
    NTEST_EXPECT_EQ_STR(label, "root");
    editor_entity_label(f->world, child, label, sizeof label);
    NTEST_EXPECT_EQ_UINT(strncmp(label, "Entity ", 7U) == 0 ? 1U : 0U, 1U);
    NTEST_EXPECT_TRUE(editor_is_under(f->world, child, root));
    NTEST_EXPECT_TRUE(editor_is_under(f->world, root, root));
    NTEST_EXPECT_FALSE(editor_is_under(f->world, root, child));
    // The chain, before any transform update ran.
    const NphWorldTransform t = editor_world_transform_of(f->world, child);
    NTEST_EXPECT_NEAR_F64(t.translation.x, 6.0, 1e-9);
}
