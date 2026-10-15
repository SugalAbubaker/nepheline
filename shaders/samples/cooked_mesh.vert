// SPDX-License-Identifier: Apache-2.0
// asset_viewer: draws a cooked mesh straight from its streams
// (engine/asset mesh.h): 16-bit unorm positions across the mesh bounds,
// octahedral snorm16 normals, half-float texture coordinates. Nothing is
// converted on the CPU; the record of the draw says where the streams are.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

// The streams are arrays of 8- and 16-byte records (NphMeshPosition,
// NphMeshAttrib), which scalar layout reads as they are.
NPH_BUFFER_REF(CookedDraws) { NphCookedDraw d[]; };
NPH_BUFFER_REF(CookedPositions) { uvec2 p[]; };
NPH_BUFFER_REF(CookedAttribs) { uvec4 a[]; };

layout(push_constant, scalar) uniform Push { NphCookedPush pc; };

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;

// The inverse of math/pack.c nph_oct_encode (Cigolle et al., "A Survey of
// Efficient Representations for Independent Unit Vectors", JCGT 2014).
vec3 oct_decode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = vec2(x, y);
    }
    return normalize(n);
}

void main()
{
    const NphCookedDraw draw = CookedDraws(pc.draws_address).d[pc.draw_index];
    const uvec2 q = CookedPositions(draw.positions_address).p[gl_VertexIndex];
    const vec3 unorm = vec3(q.x & 0xFFFFu, q.x >> 16u, q.y & 0xFFFFu) / 65535.0;
    const vec3 position = draw.position_offset.xyz + unorm * draw.position_scale.xyz;
    const uvec4 attr = CookedAttribs(draw.attribs_address).a[gl_VertexIndex];
    gl_Position = pc.view_proj * (draw.model * vec4(position, 1.0));
    // The sample's nodes have uniform scale, so the model matrix's upper
    // 3x3 carries normals (a general renderer uses its inverse transpose).
    out_normal = normalize(mat3(draw.model) * oct_decode(unpackSnorm2x16(attr.x)));
    out_uv = unpackHalf2x16(attr.z);
}
