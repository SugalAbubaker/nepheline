// SPDX-License-Identifier: Apache-2.0
#include "test_cook_support.h"

#include <cook/gltf.h>

// ---------------------------------------------------------------------------
// Hand-written documents: a JSON text with one "%s" where the buffer's
// data URI goes, loaded from memory (embedded data only).
// ---------------------------------------------------------------------------

// Imports `json` with its one "%s" replaced by a data URI of `buffer`
// (substituted rather than formatted, so the documents need no escaping).
static NphResult load_doc(const char *json, const void *buffer, u32 buffer_len, CookGltfScene *out)
{
    NphAllocator *alloc = nph_allocator_system();
    char *uri = NPH_ALLOC_ARRAY(alloc, char, (u32)test_base64_size(buffer_len) + 64U);
    NTEST_ASSERT_NOT_NULL(uri);
    static const char prefix[] = "data:application/octet-stream;base64,";
    memcpy(uri, prefix, sizeof prefix);
    test_base64((const u8 *)buffer, buffer_len, uri + sizeof prefix - 1U);
    const char *hole = strstr(json, "%s");
    NTEST_ASSERT_NOT_NULL(hole);
    NphStrBuilder sb;
    nph_sb_init(&sb, alloc);
    NTEST_ASSERT_EQ_INT(nph_sb_append(&sb, nph_str(json, (u64)(hole - json))), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, uri), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, hole + 2), NPH_RESULT_OK);
    const NphResult r = cook_gltf_load_memory(alloc, sb.data, sb.len, NULL, out);
    nph_sb_free(&sb);
    NPH_FREE_ARRAY(alloc, char, uri, (u32)test_base64_size(buffer_len) + 64U);
    return r;
}

static void check_scene(const CookGltfScene *s)
{
    NTEST_ASSERT_EQ_UINT(s->mesh_count, 2U);
    NTEST_ASSERT_EQ_UINT(s->material_count, 2U);
    NTEST_ASSERT_EQ_UINT(s->image_count, 2U);
    NTEST_ASSERT_EQ_UINT(s->node_count, 5U);
    NTEST_ASSERT_EQ_UINT(s->camera_count, 1U);
    NTEST_ASSERT_EQ_UINT(s->light_count, 1U);
    NTEST_ASSERT_TRUE(s->content_hash != 0U);

    // Meshes.
    const CookGltfMesh *cube = &s->meshes[0];
    NTEST_ASSERT_EQ_STR(cube->mesh.name, "Cube");
    NTEST_ASSERT_EQ_UINT(cube->mesh.vertices.len, 24U);
    NTEST_ASSERT_EQ_UINT(cube->mesh.indices.len, 36U);
    NTEST_ASSERT_EQ_UINT(cube->mesh.submeshes.len, 1U);
    NTEST_ASSERT_EQ_UINT(cube->material_count, 1U);
    NTEST_ASSERT_EQ_INT(cube->materials[0], 0);
    NTEST_ASSERT_TRUE((cube->mesh.flags & (u32)COOK_MESH_HAS_NORMALS) != 0U);
    NTEST_ASSERT_TRUE((cube->mesh.flags & (u32)COOK_MESH_HAS_UV0) != 0U);
    NTEST_ASSERT_FALSE((cube->mesh.flags & (u32)COOK_MESH_HAS_TANGENTS) != 0U);
    NTEST_ASSERT_FALSE((cube->mesh.flags & (u32)COOK_MESH_HAS_COLORS) != 0U);
    NTEST_ASSERT_TRUE(nph_vec3_near(cube->mesh.vertices.data[0].normal, nph_vec3(0, 0, 1), 0.0F));
    NTEST_ASSERT_EQ_UINT(cube->mesh.indices.data[35], 23U);
    // The quad has no normals: glTF demands flat ones, so its two
    // triangles own their corners (the mesh pipeline welds them later).
    const CookGltfMesh *quad = &s->meshes[1];
    NTEST_ASSERT_EQ_STR(quad->mesh.name, "Quad");
    NTEST_ASSERT_EQ_UINT(quad->mesh.vertices.len, 6U);
    NTEST_ASSERT_TRUE((quad->mesh.flags & (u32)COOK_MESH_HAS_COLORS) != 0U);
    NTEST_ASSERT_TRUE((quad->mesh.flags & (u32)COOK_MESH_HAS_NORMALS) != 0U);
    for (u32 i = 0; i < 6U; ++i) {
        NTEST_ASSERT_TRUE(
            nph_vec3_near(quad->mesh.vertices.data[i].normal, nph_vec3(0, 1, 0), 1e-6F));
    }
    // Corner 2 of the first triangle (0, 2, 1) is vertex 1: green.
    NTEST_ASSERT_TRUE(nph_vec4_near(quad->mesh.vertices.data[2].color, nph_vec4(0, 1, 0, 1), 0.0F));
    NTEST_ASSERT_EQ_INT(quad->materials[0], 1);

    // Materials.
    const CookGltfMaterial *checker = &s->materials[0];
    NTEST_ASSERT_EQ_STR(checker->name, "Checker");
    NTEST_ASSERT_EQ_INT(checker->image[NPH_MATERIAL_TEXTURE_BASE_COLOR], 0);
    NTEST_ASSERT_EQ_INT(checker->image[NPH_MATERIAL_TEXTURE_NORMAL], 1);
    NTEST_ASSERT_EQ_INT(checker->image[NPH_MATERIAL_TEXTURE_EMISSIVE], -1);
    NTEST_ASSERT_NEAR_F32(checker->header.metallic_factor, 0.2F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(checker->header.roughness_factor, 0.7F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(checker->header.textures[NPH_MATERIAL_TEXTURE_NORMAL].scale, 0.5F, 1e-6F);
    NTEST_ASSERT_EQ_UINT(checker->header.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].wrap_u,
                         NPH_MATERIAL_WRAP_CLAMP);
    NTEST_ASSERT_EQ_UINT(checker->header.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].wrap_v,
                         NPH_MATERIAL_WRAP_MIRROR);
    NTEST_ASSERT_EQ_UINT(checker->header.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].nearest, 1U);
    NTEST_ASSERT_TRUE((checker->header.flags & (u32)NPH_MATERIAL_FLAG_DOUBLE_SIDED) != 0U);
    NTEST_ASSERT_EQ_UINT(checker->header.alpha_mode, NPH_MATERIAL_ALPHA_OPAQUE);
    const CookGltfMaterial *plain = &s->materials[1];
    NTEST_ASSERT_TRUE(
        nph_vec4_near(plain->header.base_color_factor, nph_vec4(0.2F, 0.4F, 0.8F, 1.0F), 1e-6F));
    NTEST_ASSERT_EQ_UINT(plain->header.alpha_mode, NPH_MATERIAL_ALPHA_MASK);
    NTEST_ASSERT_NEAR_F32(plain->header.alpha_cutoff, 0.4F, 1e-6F);
    NTEST_ASSERT_EQ_INT(plain->image[NPH_MATERIAL_TEXTURE_BASE_COLOR], -1);

    // Images decode, and each has its own hash.
    for (u32 i = 0; i < 2U; ++i) {
        CookImage img;
        NTEST_ASSERT_EQ_INT(cook_image_decode(s->alloc, s->images[i].data, s->images[i].size, &img),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(img.width, i == 0U ? 8U : 4U);
        cook_image_free(&img);
        NTEST_ASSERT_EQ_UINT(s->images[i].hash, nph_hash64(s->images[i].data, s->images[i].size,
                                                           NPH_HASH_DEFAULT_SEED));
    }
    NTEST_ASSERT_EQ_STR(s->images[0].name, "checker");

    // Nodes in parent-before-child order with transforms.
    NTEST_ASSERT_EQ_STR(s->nodes[0].name, "root");
    NTEST_ASSERT_EQ_UINT(s->nodes[0].parent, NPH_SCENE_NO_NODE);
    NTEST_ASSERT_TRUE(nph_vec3_near(s->nodes[0].translation, nph_vec3(0, 1, 0), 0.0F));
    NTEST_ASSERT_EQ_STR(s->nodes[1].name, "cube");
    NTEST_ASSERT_EQ_UINT(s->nodes[1].parent, 0U);
    NTEST_ASSERT_EQ_INT(s->nodes[1].mesh, 0);
    NTEST_ASSERT_NEAR_F32(s->nodes[1].rotation.y, 0.7071068F, 1e-6F);
    NTEST_ASSERT_EQ_STR(s->nodes[2].name, "quad");
    NTEST_ASSERT_EQ_INT(s->nodes[2].mesh, 1);
    NTEST_ASSERT_TRUE(nph_vec3_near(s->nodes[2].scale, nph_vec3(2, 2, 2), 0.0F));
    NTEST_ASSERT_EQ_STR(s->nodes[3].name, "camera");
    NTEST_ASSERT_EQ_INT(s->nodes[3].camera, 0);
    NTEST_ASSERT_TRUE(
        nph_vec3_near(s->nodes[3].translation, nph_vec3(0, 2, 5), 1e-6F)); // from the matrix
    NTEST_ASSERT_EQ_STR(s->nodes[4].name, "light");
    NTEST_ASSERT_EQ_INT(s->nodes[4].light, 0);
    NTEST_ASSERT_EQ_UINT(s->cameras[0].node, 3U);
    NTEST_ASSERT_NEAR_F32(s->cameras[0].yfov, 0.8F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(s->cameras[0].aspect, 1.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(s->cameras[0].zfar, 0.0F, 0.0F); // infinite
    NTEST_ASSERT_EQ_UINT(s->lights[0].node, 4U);
    NTEST_ASSERT_EQ_UINT(s->lights[0].type, NPH_SCENE_LIGHT_POINT);
    NTEST_ASSERT_NEAR_F32(s->lights[0].intensity, 50.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(s->lights[0].range, 10.0F, 0.0F);
}

NTEST(gltf, loads_an_embedded_scene)
{
    NphAllocator *alloc = nph_allocator_system();
    TempDir dir;
    test_temp_dir_make(&dir, "gltf");
    test_write_gltf(&dir, "embedded", false);
    char path[1200];
    test_path_in(&dir, "embedded.gltf", path, sizeof path);
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(cook_gltf_load(alloc, path, &scene), NPH_RESULT_OK);
    check_scene(&scene);
    NTEST_ASSERT_EQ_UINT(scene.warnings, 0U);
    cook_gltf_free(&scene);
    cook_gltf_free(&scene);
    test_temp_dir_remove(&dir);
}

NTEST(gltf, loads_external_buffers_and_images)
{
    NphAllocator *alloc = nph_allocator_system();
    TempDir dir;
    test_temp_dir_make(&dir, "gltf_ext");
    test_write_gltf(&dir, "ext", true);
    char path[1200];
    test_path_in(&dir, "ext.gltf", path, sizeof path);
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(cook_gltf_load(alloc, path, &scene), NPH_RESULT_OK);
    check_scene(&scene);
    NTEST_ASSERT_TRUE(
        nph_str_ends_with(nph_str_from_cstr(scene.images[0].uri), NPH_STR("ext_base.tga")));
    NTEST_ASSERT_EQ_STR(scene.images[1].uri, ""); // the data URI one
    const u64 hash = scene.content_hash;
    cook_gltf_free(&scene);
    // From memory, with the base directory for the external files.
    void *text = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, alloc, &text, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, text, size, dir.dir, &scene), NPH_RESULT_OK);
    check_scene(&scene);
    NTEST_ASSERT_EQ_UINT(scene.content_hash, hash);
    cook_gltf_free(&scene);
    // The content hash follows the external buffer's bytes.
    char bin[1200];
    test_path_in(&dir, "ext.bin", bin, sizeof bin);
    void *bytes = NULL;
    u64 bin_size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(bin, alloc, &bytes, &bin_size), NPH_RESULT_OK);
    ((u8 *)bytes)[0] ^= 0x01U; // the lowest mantissa bit of the cube's first position
    NTEST_ASSERT_EQ_INT(nph_file_write_all(bin, bytes, bin_size), NPH_RESULT_OK);
    nph_file_free_all(alloc, bytes, bin_size);
    NTEST_ASSERT_EQ_INT(cook_gltf_load(alloc, path, &scene), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(scene.content_hash != hash);
    cook_gltf_free(&scene);
    // Without a directory, external files cannot be found.
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, text, size, NULL, &scene),
                        NPH_RESULT_PARSE_ERROR);
    // A missing image file is an error.
    char image_path[1200];
    test_path_in(&dir, "ext_base.tga", image_path, sizeof image_path);
    NTEST_ASSERT_EQ_INT(nph_fs_remove_file(image_path), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, text, size, dir.dir, &scene),
                        NPH_RESULT_IO_ERROR);
    nph_file_free_all(alloc, text, size);
    test_temp_dir_remove(&dir);
}

// ---------------------------------------------------------------------------
// The files a document names
// ---------------------------------------------------------------------------

typedef struct FileNames {
    char names[8][256];
    u32 count;
} FileNames;

static void collect_file(void *user, const char *relative_path)
{
    FileNames *f = (FileNames *)user;
    NTEST_ASSERT_LT_UINT(f->count, 8U);
    snprintf(f->names[f->count++], sizeof f->names[0], "%s", relative_path);
}

static NphResult files_of_text(const TempDir *dir, const char *name, const char *json,
                               FileNames *out)
{
    char path[1200];
    test_path_in(dir, name, path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, json, strlen(json)), NPH_RESULT_OK);
    memset(out, 0, sizeof *out);
    return cook_gltf_files(nph_allocator_system(), path, collect_file, out);
}

NTEST(gltf, lists_the_files_a_document_names)
{
    NphAllocator *alloc = nph_allocator_system();
    TempDir dir;
    test_temp_dir_make(&dir, "gltf_files");
    char path[1200];
    FileNames f;
    // The generated scenes: external buffer and base colour, the normal map
    // a data URI; then everything embedded.
    test_write_gltf(&dir, "ext", true);
    memset(&f, 0, sizeof f);
    NTEST_ASSERT_EQ_INT(
        cook_gltf_files(alloc, test_path_in(&dir, "ext.gltf", path, sizeof path), collect_file, &f),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f.count, 2U);
    NTEST_EXPECT_EQ_STR(f.names[0], "ext.bin");
    NTEST_EXPECT_EQ_STR(f.names[1], "ext_base.tga");
    test_write_gltf(&dir, "emb", false);
    memset(&f, 0, sizeof f);
    NTEST_ASSERT_EQ_INT(
        cook_gltf_files(alloc, test_path_in(&dir, "emb.gltf", path, sizeof path), collect_file, &f),
        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(f.count, 0U);

    // Decoded, normalised, and each once; the files need not exist.
    NTEST_ASSERT_EQ_INT(
        files_of_text(&dir, "uris.gltf",
                      "{\"asset\":{\"version\":\"2.0\"},"
                      "\"buffers\":[{\"byteLength\":4,\"uri\":\"a%20b.bin\"}],"
                      "\"images\":[{\"uri\":\"./tex/../tex/c.png\"},{\"uri\":\"tex//c.png\"},"
                      "{\"uri\":\"tex/d%C3%A9.png\"}]}",
                      &f),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f.count, 3U);
    NTEST_EXPECT_EQ_STR(f.names[0], "a b.bin");
    NTEST_EXPECT_EQ_STR(f.names[1], "tex/c.png");
    NTEST_EXPECT_EQ_STR(f.names[2], "tex/d\xC3\xA9.png");

    // What a copy beside the document could not keep.
    static const char *const refused[] = {"../up.png", "a/../../up.png", "/abs.png", "C:/abs.png",
                                          "https://example.com/x.png"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(refused); ++i) {
        char json[512];
        snprintf(json, sizeof json,
                 "{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"uri\":\"ok.png\"},"
                 "{\"uri\":\"%s\"}]}",
                 refused[i]);
        NTEST_EXPECT_EQ_INT(files_of_text(&dir, "refused.gltf", json, &f), NPH_RESULT_UNSUPPORTED);
    }

    // A GLB: its binary chunk is no file, its external image is.
    static const char glb_json[] = "{\"asset\":{\"version\":\"2.0\"},"
                                   "\"buffers\":[{\"byteLength\":4}],"
                                   "\"images\":[{\"uri\":\"skin.png\"}]}";
    // The chunk is padded with spaces to a multiple of 4 bytes.
    const u32 json_len = ((u32)(sizeof glb_json - 1U) + 3U) & ~3U;
    u8 glb[512];
    const u32 total = 12U + 8U + json_len + 8U + 4U;
    const u32 header[3] = {0x46546C67U, 2U, total};    // "glTF", version, length
    const u32 json_chunk[2] = {json_len, 0x4E4F534AU}; // "JSON"
    const u32 bin_chunk[2] = {4U, 0x004E4942U};        // "BIN\0"
    memcpy(glb, header, sizeof header);
    memcpy(glb + 12, json_chunk, sizeof json_chunk);
    memset(glb + 20, ' ', json_len);
    memcpy(glb + 20, glb_json, sizeof glb_json - 1U);
    memcpy(glb + 20U + json_len, bin_chunk, sizeof bin_chunk);
    memset(glb + 28U + json_len, 0, 4U);
    test_path_in(&dir, "skin.glb", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, glb, total), NPH_RESULT_OK);
    memset(&f, 0, sizeof f);
    NTEST_ASSERT_EQ_INT(cook_gltf_files(alloc, path, collect_file, &f), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f.count, 1U);
    NTEST_EXPECT_EQ_STR(f.names[0], "skin.png");

    // A missing or unreadable document.
    NTEST_EXPECT_EQ_INT(cook_gltf_files(alloc, test_path_in(&dir, "none.gltf", path, sizeof path),
                                        collect_file, &f),
                        NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(files_of_text(&dir, "bad.gltf", "{ not json", &f), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_INT(files_of_text(&dir, "empty.gltf", "", &f), NPH_RESULT_PARSE_ERROR);
    test_temp_dir_remove(&dir);
}

NTEST(gltf, rejects_bad_input)
{
    NphAllocator *alloc = nph_allocator_system();
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(cook_gltf_load(alloc, "C:/nonexistent/nowhere.gltf", &scene),
                        NPH_RESULT_NOT_FOUND);
    const char junk[] = "{\"asset\": {\"version\": \"2.0\"}, \"meshes\": [{\"primitives\": "
                        "[{\"attributes\": {\"POSITION\": 7}}]}]}";
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, junk, sizeof junk - 1U, NULL, &scene),
                        NPH_RESULT_PARSE_ERROR);
    const char not_json[] = "hello";
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, not_json, 5U, NULL, &scene),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, NULL, 0U, NULL, &scene),
                        NPH_RESULT_INVALID_ARGUMENT);
    // An empty but valid document.
    const char empty[] = "{\"asset\": {\"version\": \"2.0\"}}";
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, empty, sizeof empty - 1U, NULL, &scene),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(scene.mesh_count, 0U);
    NTEST_ASSERT_EQ_UINT(scene.node_count, 0U);
    cook_gltf_free(&scene);
    // Malformed image data URIs: too short to decode, padding alone,
    // characters outside the alphabet (in an image a material uses: only
    // those are read).
    static const char *const bad_uris[] = {
        "data:image/png;base64,=",    "data:image/png;base64,==",   "data:image/png;base64,a",
        "data:image/png;base64,a===", "data:image/png;base64,!!!!", "data:image/png,AAAA"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bad_uris); ++i) {
        char doc[384];
        snprintf(doc, sizeof doc,
                 "{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"uri\":\"%s\"}],"
                 "\"textures\":[{\"source\":0}],"
                 "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]}",
                 bad_uris[i]);
        NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, doc, strlen(doc), NULL, &scene),
                            NPH_RESULT_PARSE_ERROR);
    }
    // A buffer longer than its data URI can decode is refused before
    // anything is allocated for it (the fuzzer's 5.6e41-byte buffer
    // saturated to ~2^63 inside cgltf). "AAAA" decodes to exactly 3 bytes.
    static const struct {
        const char *length;
        NphResult expected;
    } lengths[] = {
        {"3", NPH_RESULT_OK},
        {"4", NPH_RESULT_PARSE_ERROR},
        {"9223372036854775807", NPH_RESULT_PARSE_ERROR},
        {"555555555555555555555555555555555555555544", NPH_RESULT_PARSE_ERROR},
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(lengths); ++i) {
        char doc[256];
        snprintf(doc, sizeof doc,
                 "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":%s,"
                 "\"uri\":\"data:application/octet-stream;base64,AAAA\"}]}",
                 lengths[i].length);
        NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, doc, strlen(doc), NULL, &scene),
                            lengths[i].expected);
        if (lengths[i].expected == NPH_RESULT_OK) {
            cook_gltf_free(&scene);
        }
    }
}

NTEST(gltf, triangulates_strips_and_fans)
{
    // Five vertices on a plane as a strip (3 triangles) and a fan (3 triangles).
    const f32 pos[5 * 3] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 1, 0};
    const char *doc =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"name\":\"s\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"mode\":5},"
        "{\"attributes\":{\"POSITION\":0},\"mode\":6},{\"attributes\":{\"POSITION\":0},\"mode\":0}]"
        "}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":5,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[2,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":60}],"
        "\"buffers\":[{\"byteLength\":60,\"uri\":\"%s\"}]}";
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(load_doc(doc, pos, sizeof pos, &scene), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(scene.mesh_count, 1U);
    const CookMesh *m = &scene.meshes[0].mesh;
    NTEST_ASSERT_EQ_UINT(m->submeshes.len, 2U); // the point primitive was skipped
    NTEST_ASSERT_EQ_UINT(m->indices.len, 18U);
    NTEST_ASSERT_EQ_UINT(scene.warnings, 1U);
    // Strip: (0,1,2), (2,1,3), (2,3,4); fan: (0,1,2), (0,2,3), (0,3,4).
    // Without normals every corner is its own vertex: compare positions.
#define CORNER(i) (m->vertices.data[m->indices.data[(i)]].position)
    NTEST_ASSERT_TRUE(nph_vec3_near(CORNER(3), nph_vec3(0, 1, 0), 0.0F)); // vertex 2
    NTEST_ASSERT_TRUE(nph_vec3_near(CORNER(4), nph_vec3(1, 0, 0), 0.0F)); // vertex 1
    NTEST_ASSERT_TRUE(nph_vec3_near(CORNER(5), nph_vec3(1, 1, 0), 0.0F)); // vertex 3
    NTEST_ASSERT_TRUE(nph_vec3_near(CORNER(9), nph_vec3(0, 0, 0), 0.0F)); // the fan's centre
    NTEST_ASSERT_TRUE(nph_vec3_near(CORNER(13), nph_vec3(0, 1, 0), 0.0F));
#undef CORNER
    // The strip's triangles all face +Z: its odd triangle kept the winding
    // (the third is degenerate, its corners on y = 1, and falls back to +Z).
    for (u32 i = 0; i < 9U; ++i) {
        NTEST_ASSERT_TRUE(
            nph_vec3_near(m->vertices.data[m->indices.data[i]].normal, nph_vec3(0, 0, 1), 1e-6F));
    }
    NTEST_ASSERT_EQ_UINT(scene.meshes[0].material_count, 1U);
    NTEST_ASSERT_EQ_INT(scene.meshes[0].materials[0], -1);
    cook_gltf_free(&scene);
}

NTEST(gltf, primitives_keep_their_own_attributes)
{
    // Two primitives of one mesh: the first with UVs, colours, and
    // normals; the second with positions only. Nothing the first has may
    // be lost because the second lacks it.
    struct {
        f32 pos[6 * 3];
        f32 nrm[3 * 3];
        f32 uv[3 * 2];
        f32 col[3 * 4];
    } buf = {{0, 0, 0, 1, 0, 0, 0, 1, 0, 5, 0, 0, 6, 0, 0, 5, 1, 0},
             {0, 0, 1, 0, 0, 1, 0, 0, 1},
             {0, 1, 1, 1, 0, 0},
             {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1}};
    const char *doc =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":2,\"TEXCOORD_0\":3,\"COLOR_0\":4}},"
        "{\"attributes\":{\"POSITION\":1}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],"
        "\"max\":[1,1,0]},"
        "{\"bufferView\":0,\"byteOffset\":36,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[5,0,0],\"max\":[6,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":72},{\"buffer\":0,\"byteOffset\":72,"
        "\"byteLength\":36},{\"buffer\":0,\"byteOffset\":108,\"byteLength\":24},{\"buffer\":0,"
        "\"byteOffset\":132,\"byteLength\":48}],"
        "\"buffers\":[{\"byteLength\":180,\"uri\":\"%s\"}]}";
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(load_doc(doc, &buf, sizeof buf, &scene), NPH_RESULT_OK);
    const CookMesh *m = &scene.meshes[0].mesh;
    NTEST_ASSERT_EQ_UINT(m->submeshes.len, 2U);
    NTEST_ASSERT_TRUE((m->flags & (u32)COOK_MESH_HAS_UV0) != 0U);
    NTEST_ASSERT_TRUE((m->flags & (u32)COOK_MESH_HAS_COLORS) != 0U);
    NTEST_ASSERT_TRUE((m->flags & (u32)COOK_MESH_HAS_NORMALS) != 0U);
    // The first primitive's attributes survived.
    NTEST_ASSERT_TRUE(nph_vec2_near(m->vertices.data[0].uv0, nph_vec2(0, 1), 0.0F));
    NTEST_ASSERT_TRUE(nph_vec4_near(m->vertices.data[1].color, nph_vec4(0, 1, 0, 1), 0.0F));
    // The second got defaults: flat normals, no UV, white.
    const u32 first2 = m->indices.data[m->submeshes.data[1].index_offset];
    NTEST_ASSERT_TRUE(nph_vec3_near(m->vertices.data[first2].normal, nph_vec3(0, 0, 1), 1e-6F));
    NTEST_ASSERT_TRUE(nph_vec2_near(m->vertices.data[first2].uv0, nph_vec2(0, 0), 0.0F));
    NTEST_ASSERT_TRUE(nph_vec4_near(m->vertices.data[first2].color, nph_vec4_one(), 0.0F));
    cook_gltf_free(&scene);
}

NTEST(gltf, tangents_follow_the_normals_rules)
{
    // A triangle with TANGENT but no NORMAL: the tangents MUST be ignored
    // (glTF 2.0 §3.7.2.1), so the mesh carries none and the pipeline will
    // generate them against the flat normals.
    struct {
        f32 pos[9];
        f32 nrm[9];
        f32 tan[12];
        f32 uv[6];
    } buf = {{0, 0, 0, 1, 0, 0, 0, 1, 0},
             {0, 0, 1, 0, 0, 1, 0, 0, 1},
             {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
             {0, 1, 1, 1, 0, 0}};
    const char *no_normals =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TANGENT\":2,"
        "\"TEXCOORD_0\":3}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],"
        "\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,"
        "\"byteLength\":36},{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48},{\"buffer\":0,"
        "\"byteOffset\":120,\"byteLength\":24}],"
        "\"buffers\":[{\"byteLength\":144,\"uri\":\"%s\"}]}";
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(load_doc(no_normals, &buf, sizeof buf, &scene), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE((scene.meshes[0].mesh.flags & (u32)COOK_MESH_HAS_TANGENTS) != 0U);
    cook_gltf_free(&scene);

    // Two primitives, one with authored tangents and one with UVs but
    // none: the second makes its own at import, the first keeps its.
    const char *mixed =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TANGENT\":2,\"TEXCOORD_0\":3}},"
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":3}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],"
        "\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,"
        "\"byteLength\":36},{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48},{\"buffer\":0,"
        "\"byteOffset\":120,\"byteLength\":24}],"
        "\"buffers\":[{\"byteLength\":144,\"uri\":\"%s\"}]}";
    NTEST_ASSERT_EQ_INT(load_doc(mixed, &buf, sizeof buf, &scene), NPH_RESULT_OK);
    const CookMesh *m = &scene.meshes[0].mesh;
    NTEST_ASSERT_TRUE((m->flags & (u32)COOK_MESH_HAS_TANGENTS) != 0U);
    // Authored: (0, 1, 0, +1).
    NTEST_ASSERT_TRUE(nph_vec4_near(m->vertices.data[0].tangent, nph_vec4(0, 1, 0, 1), 0.0F));
    // Generated for the second (u along +X, v down the image: w = +1).
    const u32 v2 = m->indices.data[m->submeshes.data[1].index_offset];
    NTEST_ASSERT_TRUE(nph_vec4_near(m->vertices.data[v2].tangent, nph_vec4(1, 0, 0, 1), 1e-5F));
    cook_gltf_free(&scene);
}

NTEST(gltf, sparse_and_quantised_accessors)
{
    // Positions as normalised i16 (KHR_mesh_quantization) with a sparse
    // substitution that moves vertex 2 from (0, 1, 0) to (0, 0.5, 0).
    struct {
        i16 pos[3 * 4]; // three VEC3 elements padded to 8 bytes each
        u8 sparse_index[4];
        i16 sparse_value[4];
    } buf;
    memset(&buf, 0, sizeof buf);
    buf.pos[4] = 32767; // vertex 1 x = 1
    buf.pos[9] = 32767; // vertex 2 y = 1
    buf.sparse_index[0] = 2U;
    buf.sparse_value[1] = 16384; // y = 16384 / 32767 = 0.50002
    const char *doc =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_mesh_quantization\"],"
        "\"extensionsRequired\":[\"KHR_mesh_quantization\"],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5122,\"normalized\":true,\"count\":3,"
        "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0],\"sparse\":{\"count\":1,\"indices\":"
        "{\"bufferView\":1,\"componentType\":5121},\"values\":{\"bufferView\":2}}}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":24,\"byteStride\":8},{\"buffer\":0,"
        "\"byteOffset\":24,\"byteLength\":1},{\"buffer\":0,\"byteOffset\":28,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":36,\"uri\":\"%s\"}]}";
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(load_doc(doc, &buf, sizeof buf, &scene), NPH_RESULT_OK);
    const CookMesh *m = &scene.meshes[0].mesh;
    NTEST_ASSERT_EQ_UINT(m->indices.len, 3U);
    NTEST_ASSERT_TRUE(
        nph_vec3_near(m->vertices.data[m->indices.data[1]].position, nph_vec3(1, 0, 0), 1e-4F));
    NTEST_ASSERT_TRUE(
        nph_vec3_near(m->vertices.data[m->indices.data[2]].position, nph_vec3(0, 0.5F, 0), 1e-4F));
    NTEST_ASSERT_EQ_UINT(scene.warnings, 0U);
    cook_gltf_free(&scene);
}

NTEST(gltf, cameras_and_lights_are_instanced_per_node)
{
    // One light and one camera definition, each used by two nodes; a
    // spot light's cones come through.
    const char doc[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_lights_punctual\"],"
        "\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":\"spot\",\"intensity\":3,"
        "\"spot\":{\"innerConeAngle\":0.2,\"outerConeAngle\":0.5}}]}},"
        "\"cameras\":[{\"type\":\"orthographic\",\"orthographic\":{\"xmag\":2,\"ymag\":1,"
        "\"znear\":0.5,\"zfar\":40}}],"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
        "\"nodes\":[{\"camera\":0,\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}}},"
        "{\"camera\":0,\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}},\"translation\":"
        "[1,2,3]}]}";
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(
        cook_gltf_load_memory(nph_allocator_system(), doc, sizeof doc - 1U, NULL, &scene),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(scene.node_count, 2U);
    NTEST_ASSERT_EQ_UINT(scene.camera_count, 2U);
    NTEST_ASSERT_EQ_UINT(scene.light_count, 2U);
    NTEST_ASSERT_EQ_UINT(scene.cameras[1].node, 1U);
    NTEST_ASSERT_EQ_UINT(scene.cameras[1].type, NPH_SCENE_CAMERA_ORTHOGRAPHIC);
    NTEST_ASSERT_NEAR_F32(scene.cameras[1].xmag, 2.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(scene.cameras[1].zfar, 40.0F, 0.0F);
    NTEST_ASSERT_EQ_UINT(scene.lights[1].node, 1U);
    NTEST_ASSERT_EQ_UINT(scene.lights[1].type, NPH_SCENE_LIGHT_SPOT);
    NTEST_ASSERT_NEAR_F32(scene.lights[1].inner_cone, 0.2F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(scene.lights[1].outer_cone, 0.5F, 1e-6F);
    NTEST_ASSERT_EQ_INT(scene.nodes[1].camera, 1);
    NTEST_ASSERT_EQ_INT(scene.nodes[1].light, 1);
    cook_gltf_free(&scene);
}

NTEST(gltf, hierarchies_of_any_depth_and_repeated_roots)
{
    // 300 nodes, each the only child of the one before: deeper than any
    // fixed recursion limit. The scene lists the root twice.
    NphAllocator *alloc = nph_allocator_system();
    NphStrBuilder sb;
    nph_sb_init(&sb, alloc);
    NTEST_ASSERT_EQ_INT(
        nph_sb_append_cstr(&sb, "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{"
                                "\"nodes\":[0,0]}],\"nodes\":["),
        NPH_RESULT_OK);
    for (u32 i = 0; i < 300U; ++i) {
        if (i + 1U < 300U) {
            NTEST_ASSERT_EQ_INT(
                nph_sb_appendf(&sb, "%s{\"children\":[%u]}", i == 0U ? "" : ",", i + 1U),
                NPH_RESULT_OK);
        } else {
            NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, ",{\"name\":\"leaf\"}"), NPH_RESULT_OK);
        }
    }
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, "]}"), NPH_RESULT_OK);
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, sb.data, sb.len, NULL, &scene), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(scene.node_count, 300U);
    NTEST_ASSERT_EQ_STR(scene.nodes[299].name, "leaf");
    NTEST_ASSERT_EQ_UINT(scene.nodes[299].parent, 298U);
    NTEST_ASSERT_EQ_UINT(scene.warnings, 1U); // the repeated root
    cook_gltf_free(&scene);
    nph_sb_free(&sb);
}

// A 4 x 4 RGBA8 DDS file (the DX10 header, R8G8B8A8_UNORM), texel i
// holding (i, 2 i, 3 i, 255).
static u64 tiny_dds(u8 *out)
{
    memset(out, 0, 4U + 124U + 20U);
    const u32 words[] = {0x20534444U, 124U, 0x1007U, 4U, 4U};
    memcpy(out, words, sizeof words);
    const u32 pf[] = {32U, 0x4U, 0x30315844U}; // size, FOURCC, "DX10"
    memcpy(out + 4U + 72U, pf, sizeof pf);
    const u32 dx10[] = {28U, 3U, 0U, 1U, 0U}; // R8G8B8A8_UNORM, TEXTURE2D, one element
    memcpy(out + 4U + 124U, dx10, sizeof dx10);
    u8 *texels = out + 4U + 124U + 20U;
    for (u32 i = 0; i < 16U; ++i) {
        texels[(u64)i * 4U] = (u8)i;
        texels[i * 4U + 1U] = (u8)(2U * i);
        texels[i * 4U + 2U] = (u8)(3U * i);
        texels[i * 4U + 3U] = 255U;
    }
    return 4U + 124U + 20U + 64U;
}

// MSFT_texture_dds: a texture's DDS source replaces its own source, which
// then serves clients that read no DDS and may be missing; a document that
// names no image as the DDS source is refused.
NTEST(gltf, dds_sources_replace_their_fallbacks)
{
    NphAllocator *alloc = nph_allocator_system();
    TempDir dir;
    test_temp_dir_make(&dir, "gltf_dds");
    u8 dds[4 + 124 + 20 + 64];
    const u64 dds_size = tiny_dds(dds);
    char path[1200];
    test_path_in(&dir, "tex.dds", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, dds, dds_size), NPH_RESULT_OK);
    static const char doc[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"MSFT_texture_dds\"],"
        "\"images\":[{\"uri\":\"missing.png\"},{\"uri\":\"tex.dds\"}],"
        "\"textures\":[{\"source\":0,\"extensions\":{\"MSFT_texture_dds\":{\"source\":1}}}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]}";
    test_path_in(&dir, "doc.gltf", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, doc, sizeof doc - 1U), NPH_RESULT_OK);
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(cook_gltf_load(alloc, path, &scene), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(scene.warnings, 0U); // the extension is implemented
    NTEST_ASSERT_EQ_UINT(scene.image_count, 2U);
    NTEST_EXPECT_EQ_INT(scene.materials[0].image[NPH_MATERIAL_TEXTURE_BASE_COLOR], 1);
    NTEST_EXPECT_TRUE(scene.images[0].data == NULL); // the fallback, never read
    NTEST_ASSERT_EQ_UINT(scene.images[1].size, dds_size);
    NTEST_EXPECT_EQ_MEM(scene.images[1].data, dds, dds_size);
    cook_gltf_free(&scene);
    // A copy needs the DDS file alone.
    FileNames f;
    memset(&f, 0, sizeof f);
    NTEST_ASSERT_EQ_INT(cook_gltf_files(alloc, path, collect_file, &f), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f.count, 1U);
    NTEST_EXPECT_EQ_STR(f.names[0], "tex.dds");

    // A source that is no image of the document.
    static const char bad[] =
        "{\"asset\":{\"version\":\"2.0\"},\"images\":[{\"uri\":\"missing.png\"}],"
        "\"textures\":[{\"source\":0,\"extensions\":{\"MSFT_texture_dds\":{\"source\":3}}}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]}";
    NTEST_EXPECT_EQ_INT(cook_gltf_load_memory(alloc, bad, sizeof bad - 1U, dir.dir, &scene),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_INT(files_of_text(&dir, "bad.gltf", bad, &f), NPH_RESULT_PARSE_ERROR);
    test_temp_dir_remove(&dir);
}

// KHR_materials_pbrSpecularGlossiness on import: a material with a
// texture keeps its images and factors for the cook (cooker.c makes the
// textures), the slot without an image sampling as the other does; one
// with factors alone is converted at once; the core model is ignored where
// both are given.
NTEST(gltf, specular_glossiness_materials)
{
    NphAllocator *alloc = nph_allocator_system();
    u8 rgba[4 * 4 * 4];
    memset(rgba, 200, sizeof rgba);
    u8 tga[18 + 64];
    const u64 tga_size = test_write_tga(rgba, 4U, 4U, tga);
    char b64[256];
    test_base64(tga, tga_size, b64);
    char doc[2048];
    snprintf(doc, sizeof doc,
             "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":["
             "\"KHR_materials_pbrSpecularGlossiness\"],"
             "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33648}],"
             "\"textures\":[{\"source\":0,\"sampler\":0}],"
             "\"images\":[{\"uri\":\"data:image/tga;base64,%s\"}],"
             "\"materials\":[{\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{"
             "\"specularGlossinessTexture\":{\"index\":0},\"diffuseFactor\":[0.5,0.5,0.5,0.75]}}},"
             "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0,1,0,1],\"metallicFactor\":1},"
             "\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{"
             "\"diffuseFactor\":[0.2,0.4,0.6,1],\"specularFactor\":[0.04,0.04,0.04],"
             "\"glossinessFactor\":0.9}}}]}",
             b64);
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, doc, strlen(doc), NULL, &scene),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(scene.warnings, 0U);
    NTEST_ASSERT_EQ_UINT(scene.material_count, 2U);
    const CookGltfMaterial *t = &scene.materials[0];
    NTEST_EXPECT_TRUE(t->specgloss);
    NTEST_EXPECT_EQ_INT(t->specgloss_image[0], -1);
    NTEST_EXPECT_EQ_INT(t->specgloss_image[1], 0);
    NTEST_EXPECT_EQ_INT(t->image[NPH_MATERIAL_TEXTURE_BASE_COLOR], -1);
    NTEST_EXPECT_EQ_INT(t->image[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS], -1);
    NTEST_EXPECT_NEAR_F32(t->specgloss_factors.diffuse.w, 0.75F, 0.0F);
    NTEST_EXPECT_NEAR_F32(t->header.base_color_factor.x, 1.0F, 0.0F);
    // Both slots sample as the specular-glossiness texture does (clamp,
    // mirror), the base colour's missing image notwithstanding.
    for (u32 slot = NPH_MATERIAL_TEXTURE_BASE_COLOR;
         slot <= NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS; ++slot) {
        NTEST_EXPECT_EQ_UINT(t->header.textures[slot].wrap_u, (u32)NPH_MATERIAL_WRAP_CLAMP);
        NTEST_EXPECT_EQ_UINT(t->header.textures[slot].wrap_v, (u32)NPH_MATERIAL_WRAP_MIRROR);
    }
    NTEST_EXPECT_TRUE(scene.images[0].data != NULL); // used by the conversion
    const CookGltfMaterial *f = &scene.materials[1];
    NTEST_EXPECT_FALSE(f->specgloss);
    NTEST_EXPECT_NEAR_F32(f->header.base_color_factor.x, 0.2F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(f->header.base_color_factor.y, 0.4F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(f->header.metallic_factor, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(f->header.roughness_factor, 0.1F, 1e-6F);
    cook_gltf_free(&scene);
}

NTEST(gltf, extensions_required_and_merely_used)
{
    NphAllocator *alloc = nph_allocator_system();
    CookGltfScene scene;
    // Draco compression changes what the buffers mean: required, refused.
    const char draco[] = "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":["
                         "\"KHR_draco_mesh_compression\"],\"extensionsRequired\":["
                         "\"KHR_draco_mesh_compression\"]}";
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, draco, sizeof draco - 1U, NULL, &scene),
                        NPH_RESULT_UNSUPPORTED);
    // Only used: the asset is valid without it; skipped with a warning.
    const char used[] = "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":["
                        "\"KHR_materials_clearcoat\",\"KHR_materials_unlit\"]}";
    NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, used, sizeof used - 1U, NULL, &scene),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(scene.warnings, 1U); // clearcoat; unlit is implemented
    cook_gltf_free(&scene);
}

NTEST(gltf, non_finite_values_are_refused)
{
    f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const u32 nan_bits = 0x7FC00000U;
    memcpy(&pos[4], &nan_bits, sizeof nan_bits);
    const char *doc =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36,\"uri\":\"%s\"}]}";
    CookGltfScene scene;
    NTEST_ASSERT_EQ_INT(load_doc(doc, pos, sizeof pos, &scene), NPH_RESULT_PARSE_ERROR);
    // A non-finite node transform falls back to identity with a warning.
    const char node[] = "{\"asset\":{\"version\":\"2.0\"},\"nodes\":[{\"translation\":[1e39,0,0]}"
                        "]}";
    NTEST_ASSERT_EQ_INT(
        cook_gltf_load_memory(nph_allocator_system(), node, sizeof node - 1U, NULL, &scene),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(scene.node_count, 1U);
    NTEST_ASSERT_TRUE(nph_vec3_near(scene.nodes[0].translation, nph_vec3_zero(), 0.0F));
    NTEST_ASSERT_EQ_UINT(scene.warnings, 1U);
    cook_gltf_free(&scene);
}

#ifdef NPH_FUZZ_CORPUS_DIR
// The glTF fuzz seeds (scripts/fuzz_seeds_cook.py) must import.
NTEST(gltf, the_fuzz_corpus_imports)
{
    NphAllocator *alloc = nph_allocator_system();
    static const struct {
        const char *name;
        u32 meshes;
        u32 materials;
        u32 images;
        u32 nodes;
    } seeds[] = {
        {"triangle", 1U, 0U, 0U, 1U},      {"textured", 1U, 1U, 1U, 4U},
        {"strips_sparse", 2U, 0U, 0U, 2U}, {"triangle_glb", 1U, 0U, 0U, 1U},
        {"specgloss_dds", 1U, 1U, 2U, 1U},
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(seeds); ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/gltf/%s", NPH_FUZZ_CORPUS_DIR, seeds[i].name);
        void *bytes = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read_all(path, alloc, &bytes, &size), NPH_RESULT_OK);
        CookGltfScene scene;
        NTEST_ASSERT_EQ_INT(cook_gltf_load_memory(alloc, bytes, size, NULL, &scene), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(scene.mesh_count, seeds[i].meshes);
        NTEST_ASSERT_EQ_UINT(scene.material_count, seeds[i].materials);
        NTEST_ASSERT_EQ_UINT(scene.image_count, seeds[i].images);
        NTEST_ASSERT_EQ_UINT(scene.node_count, seeds[i].nodes);
        NTEST_ASSERT_EQ_UINT(scene.warnings, 0U);
        for (u32 m = 0; m < scene.mesh_count; ++m) {
            NTEST_ASSERT_GT_UINT(scene.meshes[m].mesh.indices.len, 0U);
        }
        if (strcmp(seeds[i].name, "textured") == 0) {
            NTEST_ASSERT_EQ_UINT(scene.camera_count, 1U);
            NTEST_ASSERT_EQ_UINT(scene.light_count, 1U);
            NTEST_ASSERT_NEAR_F32(scene.materials[0].header.emissive_strength, 2.0F, 0.0F);
        }
        if (strcmp(seeds[i].name, "specgloss_dds") == 0) {
            // The DDS source read, its missing fallback not.
            NTEST_ASSERT_TRUE(scene.materials[0].specgloss);
            NTEST_ASSERT_EQ_INT(scene.materials[0].specgloss_image[0], 1);
            NTEST_ASSERT_TRUE(scene.images[0].data == NULL);
            NTEST_ASSERT_TRUE(scene.images[1].data != NULL);
        }
        cook_gltf_free(&scene);
        nph_file_free_all(alloc, bytes, size);
    }
}
#endif
