// SPDX-License-Identifier: Apache-2.0
// Definitions of the test components declared in test_scene_components.h.

#include "test_scene_components.h"

NPH_COMPONENT_DEFINE(TestPosition, TEST_POSITION_FIELDS, "test.Position", 0U, NULL)

static const TestVelocity g_velocity_default = {{0.0F, 1.0F, 0.0F}};
NPH_COMPONENT_DEFINE(TestVelocity, TEST_VELOCITY_FIELDS, "test.Velocity", 0U, &g_velocity_default)

static const TestHealth g_health_default = {100.0F, 100.0F, {0U}};
NPH_COMPONENT_DEFINE(TestHealth, TEST_HEALTH_FIELDS, "test.Health", 0U, &g_health_default)

NPH_COMPONENT_DEFINE(TestAllKinds, TEST_ALL_KINDS_FIELDS, "test.AllKinds", 0U, NULL)

NPH_TAG_DEFINE(TestTagA, "test.TagA", 0U)
NPH_TAG_DEFINE(TestTagB, "test.TagB", 0U)

NPH_COMPONENT_DEFINE(TestBig, TEST_BIG_FIELDS, "test.Big", 0U, NULL)
