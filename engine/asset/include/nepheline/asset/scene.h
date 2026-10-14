// SPDX-License-Identifier: Apache-2.0
#pragma once

// Cooked scenes (CLAUDE.md §8.8 "scenes"): a node hierarchy with mesh
// instances, cameras, and lights, as the cooker produces from a glTF file.
// The ECS scene format of §8.7 (Phase 8) is a different thing: it is what
// the editor saves; a cooked scene is content that a level references.
//
//     HEAD  NphSceneHeader
//     NODE  NphSceneNode[node]          parent/child links and local TRS
//     INST  NphSceneMeshInstance[inst]  a mesh at a node with its materials
//     MTRL  NphGuid[material]           material GUIDs referenced by instances
//     CAMS  NphSceneCamera[camera]
//     LGHT  NphSceneLight[light]
//     STRS  char[string]                NUL-terminated names
//
// Nodes are stored parent-before-child (a node's parent index is smaller
// than its own), so a single forward pass computes world transforms.
//
// Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/asset/loader.h>
#include <nepheline/math/affine.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_SCENE_TYPE_VERSION 1U

#define NPH_SCENE_TAG_HEADER    NPH_FOURCC('H', 'E', 'A', 'D')
#define NPH_SCENE_TAG_NODES     NPH_FOURCC('N', 'O', 'D', 'E')
#define NPH_SCENE_TAG_INSTANCES NPH_FOURCC('I', 'N', 'S', 'T')
#define NPH_SCENE_TAG_MATERIALS NPH_FOURCC('M', 'T', 'R', 'L')
#define NPH_SCENE_TAG_CAMERAS   NPH_FOURCC('C', 'A', 'M', 'S')
#define NPH_SCENE_TAG_LIGHTS    NPH_FOURCC('L', 'G', 'H', 'T')
#define NPH_SCENE_TAG_STRINGS   NPH_FOURCC('S', 'T', 'R', 'S')

#define NPH_SCENE_NO_NODE NPH_U32_MAX

typedef struct NphSceneHeader {
    u32 node_count;
    u32 instance_count;
    u32 material_count;
    u32 camera_count;
    u32 light_count;
    u32 string_bytes;
    u32 flags;
    u32 reserved[9];
} NphSceneHeader;

NPH_STATIC_ASSERT(sizeof(NphSceneHeader) == 64U, "the scene header is 64 bytes");

typedef struct NphSceneNode {
    NphVec3 translation;
    f32 reserved0;
    NphQuat rotation;
    NphVec3 scale;
    f32 reserved1;
    u32 parent;       // NPH_SCENE_NO_NODE for a root
    u32 name;         // offset into STRS
    u32 first_child;  // NPH_SCENE_NO_NODE when none
    u32 next_sibling; // NPH_SCENE_NO_NODE when last
} NphSceneNode;

NPH_STATIC_ASSERT(sizeof(NphSceneNode) == 64U, "a scene node is 64 bytes");

typedef struct NphSceneMeshInstance {
    NphGuid mesh;
    u32 node;
    u32 material_offset; // into MTRL: one GUID per material slot of the mesh
    u32 material_count;
    u32 flags;
} NphSceneMeshInstance;

NPH_STATIC_ASSERT(sizeof(NphSceneMeshInstance) == 32U, "a mesh instance is 32 bytes");

typedef enum NphSceneCameraType {
    NPH_SCENE_CAMERA_PERSPECTIVE = 0,
    NPH_SCENE_CAMERA_ORTHOGRAPHIC = 1,
} NphSceneCameraType;

typedef struct NphSceneCamera {
    u32 node;
    u32 type; // NphSceneCameraType
    f32 yfov; // radians (perspective)
    f32 aspect;
    f32 znear;
    f32 zfar; // 0: infinite
    f32 xmag; // orthographic half extents
    f32 ymag;
} NphSceneCamera;

NPH_STATIC_ASSERT(sizeof(NphSceneCamera) == 32U, "a camera is 32 bytes");

typedef enum NphSceneLightType {
    NPH_SCENE_LIGHT_DIRECTIONAL = 0,
    NPH_SCENE_LIGHT_POINT = 1,
    NPH_SCENE_LIGHT_SPOT = 2,
} NphSceneLightType;

typedef struct NphSceneLight {
    u32 node;
    u32 type; // NphSceneLightType
    NphVec3 color;
    f32 intensity; // lux (directional) or candela
    f32 range;     // 0: unlimited
    f32 inner_cone;
    f32 outer_cone;
    f32 reserved;
} NphSceneLight;

NPH_STATIC_ASSERT(sizeof(NphSceneLight) == 40U, "a light is 40 bytes");

typedef struct NphSceneView {
    const NphSceneHeader *header;
    const NphSceneNode *nodes;
    const NphSceneMeshInstance *instances;
    const NphGuid *materials;
    const NphSceneCamera *cameras;
    const NphSceneLight *lights;
    const char *strings;
} NphSceneView;

// Resolves and validates a READY scene asset: counts against blobs, node
// links in range and parents before children, instance and name offsets
// in range, names terminated.
NPH_MUST_USE NphResult nph_scene_view(const NphAsset *asset, NphSceneView *out);

// The name of node `index`.
NphStr nph_scene_node_name(const NphSceneView *view, u32 index);

// The local transform of a node.
NPH_INLINE NphAffine nph_scene_node_local(const NphSceneNode *node)
{
    return nph_affine_from_trs(node->translation, node->rotation, node->scale);
}

#ifdef __cplusplus
}
#endif
