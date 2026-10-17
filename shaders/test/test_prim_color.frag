// SPDX-License-Identifier: Apache-2.0
// Writes the per-primitive colour test_mesh_color.mesh hands it.
#version 460
#extension GL_EXT_mesh_shader : require

layout(location = 0) perprimitiveEXT flat in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = in_color;
}
