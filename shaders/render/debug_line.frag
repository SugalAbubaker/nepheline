// SPDX-License-Identifier: Apache-2.0
// Debug lines: the colour of the line (debug_line.vert).
#version 460

layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = in_color;
}
