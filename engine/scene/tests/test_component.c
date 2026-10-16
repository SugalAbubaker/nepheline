// SPDX-License-Identifier: Apache-2.0
// Component reflection: the X-macro generators, descriptor validation,
// field-wise comparison and hashing, layout hashes, NphText.

#include "test_scene_support.h"

#include <stddef.h>

NTEST(component, field_kinds_have_their_c_sizes)
{
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_BOOL), 4U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_U8), 1U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_I16), 2U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_U64), 8U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_VEC3), 12U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_QUAT), 16U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_DVEC3), 24U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_ENTITY), 4U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_STR_ID), 8U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_TEXT), NPH_TEXT_CAPACITY);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_GUID), 16U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_align(NPH_FIELD_GUID), 8U);
    NTEST_EXPECT_EQ_STR(nph_field_kind_name(NPH_FIELD_GUID), "GUID");
    NTEST_EXPECT_EQ_UINT(nph_field_kind_align(NPH_FIELD_DVEC3), 8U);
    NTEST_EXPECT_EQ_UINT(nph_field_kind_size(NPH_FIELD_KIND_COUNT), 0U);
    NTEST_EXPECT_EQ_STR(nph_field_kind_name(NPH_FIELD_VEC4), "VEC4");
    NTEST_EXPECT_EQ_STR(nph_field_kind_name(NPH_FIELD_KIND_COUNT), "INVALID");
}

NTEST(component, generator_matches_the_struct)
{
    const NphComponentDesc *d = TestHealth_component();
    NTEST_ASSERT_NOT_NULL(d);
    NTEST_EXPECT_EQ_STR(d->name, "test.Health");
    NTEST_EXPECT_EQ_UINT(d->size, sizeof(TestHealth));
    NTEST_EXPECT_EQ_UINT(d->align, NPH_ALIGNOF(TestHealth));
    NTEST_ASSERT_EQ_UINT(d->field_count, 3U);
    NTEST_EXPECT_EQ_STR(d->fields[0].name, "current");
    NTEST_EXPECT_EQ_UINT(d->fields[0].kind, NPH_FIELD_F32);
    NTEST_EXPECT_EQ_UINT(d->fields[0].offset, offsetof(TestHealth, current));
    NTEST_EXPECT_EQ_STR(d->fields[1].name, "maximum");
    NTEST_EXPECT_EQ_UINT(d->fields[1].flags, NPH_FIELD_FLAG_RANGE);
    NTEST_EXPECT_NEAR_F32(d->fields[1].min, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(d->fields[1].max, 1000.0F, 0.0F);
    NTEST_EXPECT_EQ_STR(d->fields[2].name, "attacker");
    NTEST_EXPECT_EQ_UINT(d->fields[2].kind, NPH_FIELD_ENTITY);
    NTEST_EXPECT_EQ_UINT(d->fields[2].offset, offsetof(TestHealth, attacker));
    NTEST_EXPECT_EQ_UINT(d->fields[2].size, sizeof(NphEntity));
    NTEST_EXPECT_EQ_UINT(d->fields[2].flags, NPH_FIELD_FLAG_READONLY);
    NTEST_ASSERT_NOT_NULL(d->defaults);
    const TestHealth *def = (const TestHealth *)d->defaults;
    NTEST_EXPECT_NEAR_F32(def->current, 100.0F, 0.0F);
    // The same descriptor object on every call.
    NTEST_EXPECT_EQ_PTR(TestHealth_component(), d);
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(d, NULL), NPH_RESULT_OK);
}

NTEST(component, every_kind_validates)
{
    const NphComponentDesc *d = TestAllKinds_component();
    // One field of every kind, plus a transient float.
    NTEST_ASSERT_EQ_UINT(d->field_count, (u32)NPH_FIELD_KIND_COUNT + 1U);
    u32 seen = 0U;
    for (u32 i = 0; i < d->field_count; ++i) {
        NTEST_EXPECT_EQ_UINT(d->fields[i].size,
                             nph_field_kind_size((NphFieldKind)d->fields[i].kind));
        seen |= 1U << d->fields[i].kind;
    }
    NTEST_EXPECT_EQ_UINT(seen, (1U << (u32)NPH_FIELD_KIND_COUNT) - 1U);
    NTEST_EXPECT_EQ_UINT(d->fields[d->field_count - 1U].flags, NPH_FIELD_FLAG_TRANSIENT);
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(d, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(TestTagA_component(), NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(TestTagA_component()->size, 0U);
}

NTEST(component, find_field_and_defaults)
{
    const NphComponentDesc *d = TestHealth_component();
    const NphFieldDesc *f = nph_component_find_field(d, NPH_STR("maximum"));
    NTEST_ASSERT_NOT_NULL(f);
    NTEST_EXPECT_EQ_UINT(f->offset, offsetof(TestHealth, maximum));
    NTEST_EXPECT_NULL(nph_component_find_field(d, NPH_STR("missing")));

    TestHealth h;
    memset(&h, 0xAB, sizeof h);
    nph_component_init_default(d, &h);
    NTEST_EXPECT_NEAR_F32(h.maximum, 100.0F, 0.0F);
    NTEST_EXPECT_TRUE(nph_entity_is_null(h.attacker));

    TestPosition p;
    memset(&p, 0xAB, sizeof p);
    nph_component_init_default(TestPosition_component(), &p);
    NTEST_EXPECT_NEAR_F32(p.value.x, 0.0F, 0.0F);
    nph_component_init_default(TestTagA_component(), NULL); // a tag has no data
}

static const NphFieldDesc g_one_f32[] = {{"x", NPH_FIELD_F32, 0U, 4U, 0U, 0.0F, 0.0F}};

static NphComponentDesc base_desc(void)
{
    const NphComponentDesc d = {"test.Valid", 4U, 4U, g_one_f32, 1U, 0U, NULL};
    return d;
}

NTEST(component, validation_rejects_malformed_descriptors)
{
    const char *reason = NULL;
    NphComponentDesc d = base_desc();
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(reason, "");
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(NULL, &reason), NPH_RESULT_INVALID_ARGUMENT);

    d.name = "";
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d.name = "bad name";
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d.name = "a234567890123456789012345678901234567890123456789012345678901234"; // 64
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);

    d = base_desc();
    d.size = NPH_ECS_MAX_COMPONENT_SIZE + 4U;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d = base_desc();
    d.align = 3U;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d.align = 128U;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d = base_desc();
    d.size = 6U;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d = base_desc();
    d.fields = NULL;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d = base_desc();
    d.field_count = NPH_ECS_MAX_FIELDS + 1U;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);

    // A tag has no fields.
    d = base_desc();
    d.size = 0U;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_INVALID_ARGUMENT);
    d.field_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&d, &reason), NPH_RESULT_OK);

    NphFieldDesc f[2] = {{"x", NPH_FIELD_F32, 0U, 4U, 0U, 0.0F, 0.0F},
                         {"y", NPH_FIELD_F32, 4U, 4U, 0U, 0.0F, 0.0F}};
    NphComponentDesc two = {"test.Two", 8U, 4U, f, 2U, 0U, NULL};
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_OK);
    f[1].offset = 2U; // misaligned
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
    f[1].offset = 0U; // overlaps x
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
    f[1].offset = 8U; // past the end
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
    f[1].offset = 4U;
    f[1].name = "x"; // duplicate name
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
    f[1].name = "y";
    f[1].size = 8U; // wrong size for the kind
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
    f[1].size = 4U;
    f[1].kind = NPH_FIELD_KIND_COUNT;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
    f[1].kind = NPH_FIELD_F32;
    f[1].flags = NPH_FIELD_FLAG_RANGE;
    f[1].min = 2.0F;
    f[1].max = 1.0F;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
    f[1].max = 3.0F;
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_OK);
    f[1].name = "";
    NTEST_EXPECT_EQ_INT(nph_component_desc_validate(&two, &reason), NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(component, fields_equal_and_hash_ignore_padding_and_text_tails)
{
    const NphComponentDesc *d = TestAllKinds_component();
    TestAllKinds a;
    TestAllKinds b;
    memset(&a, 0x11, sizeof a);
    memset(&b, 0x22, sizeof b); // different padding
    nph_component_init_default(d, &a);
    nph_component_init_default(d, &b);
    // Make the padding differ again after the defaults zeroed it.
    u8 *pa = (u8 *)&a;
    u8 *pb = (u8 *)&b;
    for (u32 i = 0; i < sizeof a; ++i) {
        bool covered = false;
        for (u32 f = 0; f < d->field_count; ++f) {
            covered = covered ||
                      (i >= d->fields[f].offset && i < d->fields[f].offset + d->fields[f].size);
        }
        if (!covered) {
            pa[i] = 0x5AU;
            pb[i] = 0xA5U;
        }
    }
    NTEST_EXPECT_TRUE(nph_text_set(&a.label, NPH_STR("hello")));
    NTEST_EXPECT_TRUE(nph_text_set(&b.label, NPH_STR("hello")));
    b.label.text[10] = 'z'; // after the terminator
    NTEST_EXPECT_TRUE(nph_component_fields_equal(d, &a, &b));
    NTEST_EXPECT_EQ_UINT(nph_component_hash_fields(d, &a, 7U),
                         nph_component_hash_fields(d, &b, 7U));

    b.f32v = -0.0F; // +0 and -0 differ: they are written differently
    NTEST_EXPECT_FALSE(nph_component_fields_equal(d, &a, &b));
    NTEST_EXPECT_NE_UINT(nph_component_hash_fields(d, &a, 7U),
                         nph_component_hash_fields(d, &b, 7U));
    b.f32v = 0.0F;
    NTEST_EXPECT_TRUE(nph_text_set(&b.label, NPH_STR("hellO")));
    NTEST_EXPECT_FALSE(nph_component_fields_equal(d, &a, &b));
    NTEST_EXPECT_TRUE(nph_text_set(&b.label, NPH_STR("hello!")));
    NTEST_EXPECT_FALSE(nph_component_fields_equal(d, &a, &b));
    NTEST_EXPECT_NE_UINT(nph_component_hash_fields(d, &a, 7U),
                         nph_component_hash_fields(d, &a, 8U));
}

NTEST(component, layout_hash_tracks_the_layout_only)
{
    NphComponentDesc d = base_desc();
    const u64 h = nph_component_layout_hash(&d);
    NTEST_EXPECT_EQ_UINT(nph_component_layout_hash(&d), h);
    NphFieldDesc renamed[1] = {{"y", NPH_FIELD_F32, 0U, 4U, 0U, 0.0F, 0.0F}};
    d.fields = renamed;
    NTEST_EXPECT_NE_UINT(nph_component_layout_hash(&d), h);
    NphFieldDesc retyped[1] = {{"x", NPH_FIELD_I32, 0U, 4U, 0U, 0.0F, 0.0F}};
    d.fields = retyped;
    NTEST_EXPECT_NE_UINT(nph_component_layout_hash(&d), h);
    // Editor hints are not part of the layout.
    NphFieldDesc hinted[1] = {{"x", NPH_FIELD_F32, 0U, 4U, NPH_FIELD_FLAG_COLOR, 0.0F, 0.0F}};
    d.fields = hinted;
    NTEST_EXPECT_EQ_UINT(nph_component_layout_hash(&d), h);
    d.name = "test.Other";
    NTEST_EXPECT_NE_UINT(nph_component_layout_hash(&d), h);
}

NTEST(component, text_truncates_on_a_character_boundary)
{
    NphText t;
    NTEST_EXPECT_TRUE(nph_text_set(&t, NPH_STR("")));
    NTEST_EXPECT_EQ_UINT(nph_text_str(&t).len, 0U);
    NTEST_EXPECT_TRUE(nph_text_set(&t, NPH_STR("name")));
    NTEST_EXPECT_TRUE(nph_str_equals(nph_text_str(&t), NPH_STR("name")));

    // 62 ASCII bytes and a 3-byte euro sign: 65 bytes do not fit 63, and
    // the cut must not split the euro sign.
    char buf[80];
    memset(buf, 'a', sizeof buf);
    buf[62] = '\xE2';
    buf[63] = '\x82';
    buf[64] = '\xAC';
    NTEST_EXPECT_FALSE(nph_text_set(&t, nph_str(buf, 65U)));
    NTEST_EXPECT_EQ_UINT(nph_text_str(&t).len, 62U);
    NTEST_EXPECT_TRUE(nph_utf8_is_valid(nph_text_str(&t)));

    memset(buf, 'b', 70);
    NTEST_EXPECT_FALSE(nph_text_set(&t, nph_str(buf, 70U)));
    NTEST_EXPECT_EQ_UINT(nph_text_str(&t).len, NPH_TEXT_CAPACITY - 1U);

    // No terminator: the view stops at the capacity.
    memset(t.text, 'c', sizeof t.text);
    NTEST_EXPECT_EQ_UINT(nph_text_str(&t).len, NPH_TEXT_CAPACITY);
}
