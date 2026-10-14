// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/asset.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_ASSET, "asset", NPH_LOG_LEVEL_INFO);

const char *nph_asset_type_name(u32 type)
{
    switch (type) {
    case NPH_ASSET_TYPE_MESH:
        return "mesh";
    case NPH_ASSET_TYPE_TEXTURE:
        return "texture";
    case NPH_ASSET_TYPE_MATERIAL:
        return "material";
    case NPH_ASSET_TYPE_SCENE:
        return "scene";
    case NPH_ASSET_TYPE_BLOB:
        return "blob";
    default:
        return "unknown";
    }
}

void nph_fourcc_to_string(u32 code, char out[5])
{
    for (u32 i = 0; i < 4U; ++i) {
        const u8 c = (u8)(code >> (8U * i));
        out[i] = (char)((c >= 0x20U && c < 0x7FU) ? c : (u8)'.');
    }
    out[4] = '\0';
}
