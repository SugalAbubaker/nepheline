// SPDX-License-Identifier: Apache-2.0
#include <nepheline/math/affine.h>

NphAffine nph_affine_from_trs(NphVec3 translation, NphQuat rotation, NphVec3 scale)
{
    const NphMat3 l = nph_mat3_from_cols(nph_vec3_scale(nph_quat_axis_x(rotation), scale.x),
                                         nph_vec3_scale(nph_quat_axis_y(rotation), scale.y),
                                         nph_vec3_scale(nph_quat_axis_z(rotation), scale.z));
    return nph_affine_from_mat3_translation(l, translation);
}

bool nph_affine_decompose_trs(NphAffine a, NphVec3 *out_translation, NphQuat *out_rotation,
                              NphVec3 *out_scale)
{
    return nph_mat4_decompose_trs(nph_affine_to_mat4(a), out_translation, out_rotation, out_scale);
}

bool nph_affine_inverse(NphAffine a, NphAffine *out)
{
    NphMat3 inv_l;
    if (!nph_mat3_inverse(nph_affine_linear(a), &inv_l)) {
        return false;
    }
    const NphVec3 t = nph_vec3_neg(nph_mat3_mul_vec3(inv_l, nph_affine_translation(a)));
    *out = nph_affine_from_mat3_translation(inv_l, t);
    return true;
}
