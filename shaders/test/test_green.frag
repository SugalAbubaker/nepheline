// SPDX-License-Identifier: Apache-2.0
// Opaque green, whatever was drawn: the gpu tests' draws that check where
// geometry lands and nothing else (no push constants, no inputs).
#version 460

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(0.0, 1.0, 0.0, 1.0);
}
