// SPDX-License-Identifier: Apache-2.0
// SPIR-V reflection on the compiled test shaders, on hand-assembled
// modules, and on malformed input; shader modules and hot reload.
#include <nepheline/platform/file.h>

#include "test_gpu_support.h"

#include <nph_shared.h>

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

static NphResult reflect_file(const char *file, NphGpuShaderInfo *info)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.spv", NPH_SHADER_DIR, file);
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_allocator_system(), &data, &size),
                        NPH_RESULT_OK);
    const NphResult r = nph_gpu_spirv_reflect(data, size, nph_allocator_system(), info);
    nph_file_free_all(nph_allocator_system(), data, size);
    return r;
}

NTEST(gpu_spirv, reflects_the_compute_shader)
{
    NphGpuShaderInfo info;
    NTEST_ASSERT_EQ_INT(reflect_file("test_fill.comp", &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.stage, NPH_GPU_SHADER_COMPUTE);
    NTEST_ASSERT_EQ_STR(info.entry_point, "main");
    NTEST_ASSERT_EQ_UINT(info.workgroup_size[0], 64U);
    NTEST_ASSERT_EQ_UINT(info.workgroup_size[1], 1U);
    NTEST_ASSERT_EQ_UINT(info.workgroup_size[2], 1U);
    NTEST_ASSERT_EQ_UINT(info.push_constant_bytes, (u32)sizeof(NphTestPush));
    NTEST_ASSERT_TRUE(info.uses_device_address);
    NTEST_ASSERT_GE_UINT(info.capability_count, 2U);
    NTEST_ASSERT_EQ_UINT(info.spirv_version_major, 1U);
    NTEST_ASSERT_GE_UINT(info.spirv_version_minor, 3U);
    NTEST_ASSERT_EQ_UINT(info.color_output_mask, 0U);
}

NTEST(gpu_spirv, reflects_the_graphics_shaders)
{
    NphGpuShaderInfo info;
    NTEST_ASSERT_EQ_INT(reflect_file("test_triangle.vert", &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.stage, NPH_GPU_SHADER_VERTEX);
    NTEST_ASSERT_EQ_UINT(info.push_constant_bytes, 0U);
    NTEST_ASSERT_EQ_UINT(info.bindless_uses, 0U);
    NTEST_ASSERT_EQ_INT(reflect_file("test_sample.frag", &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.stage, NPH_GPU_SHADER_FRAGMENT);
    NTEST_ASSERT_EQ_UINT(info.bindless_uses,
                         (u32)NPH_GPU_BINDLESS_SAMPLED_IMAGES | (u32)NPH_GPU_BINDLESS_SAMPLERS);
    NTEST_ASSERT_EQ_UINT(info.color_output_mask, 1U);
    NTEST_ASSERT_EQ_UINT(info.push_constant_bytes, (u32)sizeof(NphTestPush));
    NTEST_ASSERT_EQ_INT(reflect_file("test_image.comp", &info), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(info.bindless_uses, (u32)NPH_GPU_BINDLESS_STORAGE_IMAGES);
    NTEST_ASSERT_EQ_UINT(info.workgroup_size[0], 8U);
    NTEST_ASSERT_EQ_UINT(info.workgroup_size[1], 8U);
}

// ---------------------------------------------------------------------------
// Hand-assembled modules
// ---------------------------------------------------------------------------

typedef struct Asm {
    u32 words[256];
    u32 count;
} Asm;

static void asm_begin(Asm *a, u32 bound)
{
    a->count = 0U;
    a->words[a->count++] = 0x07230203U;
    a->words[a->count++] = 0x00010600U; // 1.6
    a->words[a->count++] = 0U;
    a->words[a->count++] = bound;
    a->words[a->count++] = 0U;
}

static void asm_op(Asm *a, u32 op, const u32 *operands, u32 n)
{
    a->words[a->count++] = ((n + 1U) << 16U) | op;
    for (u32 i = 0; i < n; ++i) {
        a->words[a->count++] = operands[i];
    }
}

#define OP(a, op, ...)                                                                             \
    do {                                                                                           \
        const u32 ops_[] = {__VA_ARGS__};                                                          \
        asm_op((a), (op), ops_, (u32)NPH_ARRAY_COUNT(ops_));                                       \
    } while (0)
#define OP0(a, op) asm_op((a), (op), NULL, 0U)

// "main" as literal string words.
#define MAIN_STR 0x6E69616DU, 0U

// A vertex shader with one input attribute at location 0: rejected.
static void asm_vertex_with_input(Asm *a)
{
    asm_begin(a, 12U);
    OP(a, 17, 1U);                   // OpCapability Shader
    OP(a, 14, 0U, 1U);               // OpMemoryModel Logical GLSL450
    OP(a, 15, 0U, 4U, MAIN_STR, 8U); // OpEntryPoint Vertex %4 "main" %8
    OP(a, 71, 8U, 30U, 0U);          // OpDecorate %8 Location 0
    OP(a, 19, 2U);                   // %2 = OpTypeVoid
    OP(a, 33, 3U, 2U);               // %3 = OpTypeFunction %2
    OP(a, 22, 6U, 32U);              // %6 = OpTypeFloat 32
    OP(a, 32, 7U, 1U, 6U);           // %7 = OpTypePointer Input %6
    OP(a, 59, 7U, 8U, 1U);           // %8 = OpVariable %7 Input
    OP(a, 54, 2U, 4U, 0U, 3U);       // %4 = OpFunction %2 None %3
    OP(a, 248, 5U);                  // %5 = OpLabel
    OP0(a, 253);                     // OpReturn
    OP0(a, 56);                      // OpFunctionEnd
}

// A compute shader with a push constant block of {vec4; mat4} = 80 bytes.
static void asm_compute_push(Asm *a)
{
    asm_begin(a, 20U);
    OP(a, 17, 1U); // Shader
    OP(a, 14, 0U, 1U);
    OP(a, 15, 5U, 4U, MAIN_STR);    // GLCompute
    OP(a, 16, 4U, 17U, 4U, 2U, 1U); // LocalSize 4 2 1
    OP(a, 72, 10U, 0U, 35U, 0U);    // member 0 Offset 0
    OP(a, 72, 10U, 1U, 35U, 16U);   // member 1 Offset 16
    OP(a, 72, 10U, 1U, 7U, 16U);    // member 1 MatrixStride 16
    OP(a, 19, 2U);                  // void
    OP(a, 33, 3U, 2U);
    OP(a, 22, 6U, 32U);      // float
    OP(a, 23, 7U, 6U, 4U);   // vec4
    OP(a, 24, 8U, 7U, 4U);   // mat4
    OP(a, 30, 10U, 7U, 8U);  // struct {vec4, mat4}
    OP(a, 32, 11U, 9U, 10U); // ptr PushConstant
    OP(a, 59, 11U, 12U, 9U); // variable PushConstant
    OP(a, 54, 2U, 4U, 0U, 3U);
    OP(a, 248, 5U);
    OP0(a, 253);
    OP0(a, 56);
}

// A fragment shader with a combined sampler at set 0 binding 0: rejected.
static void asm_fragment_combined_sampler(Asm *a, u32 set, u32 binding)
{
    asm_begin(a, 20U);
    OP(a, 17, 1U);
    OP(a, 14, 0U, 1U);
    OP(a, 15, 4U, 4U, MAIN_STR);  // Fragment
    OP(a, 16, 4U, 7U);            // OriginUpperLeft
    OP(a, 71, 12U, 34U, set);     // DescriptorSet
    OP(a, 71, 12U, 33U, binding); // Binding
    OP(a, 19, 2U);
    OP(a, 33, 3U, 2U);
    OP(a, 22, 6U, 32U);
    OP(a, 25, 9U, 6U, 1U, 0U, 0U, 0U, 1U, 0U); // image 2D sampled=1
    OP(a, 27, 10U, 9U);                        // sampled image
    OP(a, 32, 11U, 0U, 10U);                   // ptr UniformConstant
    OP(a, 59, 11U, 12U, 0U);                   // variable UniformConstant
    OP(a, 54, 2U, 4U, 0U, 3U);
    OP(a, 248, 5U);
    OP0(a, 253);
    OP0(a, 56);
}

// A fragment shader using a runtime array of sampled images at binding 0
// and a storage buffer with set/binding: the buffer is rejected.
static void asm_fragment_storage_buffer(Asm *a)
{
    asm_begin(a, 20U);
    OP(a, 17, 1U);
    OP(a, 14, 0U, 1U);
    OP(a, 15, 4U, 4U, MAIN_STR);
    OP(a, 16, 4U, 7U);
    OP(a, 71, 12U, 34U, 0U);
    OP(a, 71, 12U, 33U, 3U);
    OP(a, 19, 2U);
    OP(a, 33, 3U, 2U);
    OP(a, 21, 6U, 32U, 0U);   // uint
    OP(a, 29, 8U, 6U);        // runtime array of uint
    OP(a, 30, 10U, 8U);       // struct
    OP(a, 32, 11U, 12U, 10U); // ptr StorageBuffer
    OP(a, 59, 11U, 12U, 12U); // variable StorageBuffer
    OP(a, 54, 2U, 4U, 0U, 3U);
    OP(a, 248, 5U);
    OP0(a, 253);
    OP0(a, 56);
}

NTEST(gpu_spirv, rejects_layouts_outside_the_bindless_model)
{
    Asm a;
    NphGpuShaderInfo info;
    asm_vertex_with_input(&a);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, nph_allocator_system(), &info),
                        NPH_RESULT_UNSUPPORTED);
    asm_fragment_combined_sampler(&a, 0U, 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, nph_allocator_system(), &info),
                        NPH_RESULT_UNSUPPORTED);
    asm_fragment_combined_sampler(&a, 1U, 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, nph_allocator_system(), &info),
                        NPH_RESULT_UNSUPPORTED);
    asm_fragment_storage_buffer(&a);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, nph_allocator_system(), &info),
                        NPH_RESULT_UNSUPPORTED);
}

NTEST(gpu_spirv, computes_push_constant_block_sizes)
{
    Asm a;
    NphGpuShaderInfo info;
    asm_compute_push(&a);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, nph_allocator_system(), &info),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(info.stage, NPH_GPU_SHADER_COMPUTE);
    NTEST_ASSERT_EQ_UINT(info.push_constant_bytes, 80U);
    NTEST_ASSERT_EQ_UINT(info.workgroup_size[0], 4U);
    NTEST_ASSERT_EQ_UINT(info.workgroup_size[1], 2U);
    // A block over 128 bytes is rejected: move member 1 to offset 120.
    asm_compute_push(&a);
    for (u32 i = 5; i + 4U < a.count; ++i) {
        if (a.words[i] == ((5U << 16U) | 72U) && a.words[i + 2U] == 1U && a.words[i + 3U] == 35U) {
            a.words[i + 4U] = 120U;
        }
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, nph_allocator_system(), &info),
                        NPH_RESULT_UNSUPPORTED);
}

NTEST(gpu_spirv, rejects_malformed_modules)
{
    NphGpuShaderInfo info;
    NphAllocator *alloc = nph_allocator_system();
    u32 words[64];
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(NULL, 0U, alloc, &info), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(words, 7U, alloc, &info), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(words, 16U, alloc, &info), NPH_RESULT_CORRUPT);
    Asm a;
    asm_compute_push(&a);
    a.words[0] = 0x12345678U; // bad magic
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    asm_compute_push(&a);
    a.words[3] = 0U; // bound 0
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    asm_compute_push(&a);
    a.words[3] = 1U << 30U; // absurd bound
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    asm_compute_push(&a);
    a.words[a.count - 1U] = (99U << 16U) | 56U; // last instruction claims 99 words
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    asm_compute_push(&a);
    a.words[5] = 17U; // word count 0
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    // Header only: no entry point.
    asm_begin(&a, 4U);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    // An entry point with an unterminated name.
    asm_begin(&a, 8U);
    OP(&a, 15, 5U, 4U, 0x6E69616DU);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    // An unknown execution model.
    asm_begin(&a, 8U);
    OP(&a, 15, 3U, 4U, MAIN_STR); // Geometry
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_UNSUPPORTED);
    // Ids out of bound.
    asm_begin(&a, 8U);
    OP(&a, 15, 5U, 4U, MAIN_STR);
    OP(&a, 19, 200U);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
    // Redefined id.
    asm_begin(&a, 8U);
    OP(&a, 15, 5U, 4U, MAIN_STR);
    OP(&a, 19, 2U);
    OP(&a, 19, 2U);
    NTEST_ASSERT_EQ_INT(nph_gpu_spirv_reflect(a.words, a.count * 4U, alloc, &info),
                        NPH_RESULT_CORRUPT);
}

// ---------------------------------------------------------------------------
// Shader modules
// ---------------------------------------------------------------------------

NTEST_F(GpuFixture, shader_modules_create_reload_and_destroy)
{
    REQUIRE_DEVICE(f);
    NphGpuShader frag = test_load_shader(f->dev, "test_color.frag");
    NTEST_ASSERT_TRUE(nph_gpu_shader_is_valid(f->dev, frag));
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_info(f->dev, frag)->stage, NPH_GPU_SHADER_FRAGMENT);
    NTEST_ASSERT_EQ_UINT(nph_gpu_shader_generation(f->dev, frag), 1U);
    NTEST_ASSERT_NE_PTR(nph_gpu_vk_shader_module(f->dev, frag), VK_NULL_HANDLE);
    // Reload with another fragment shader: fine. With a vertex shader: not.
    u64 size = 0U;
    void *sample = test_read_spirv(f->dev, "test_sample.frag", &size);
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_reload(f->dev, frag, sample, size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_shader_generation(f->dev, frag), 2U);
    NTEST_ASSERT_NE_UINT(nph_gpu_shader_info(f->dev, frag)->bindless_uses, 0U);
    test_free_spirv(f->dev, sample, size);
    void *vert = test_read_spirv(f->dev, "test_triangle.vert", &size);
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_reload(f->dev, frag, vert, size),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_reload(f->dev, frag, vert, 3U), NPH_RESULT_CORRUPT);
    test_free_spirv(f->dev, vert, size);
    NTEST_ASSERT_EQ_UINT(nph_gpu_shader_generation(f->dev, frag), 2U);
    nph_gpu_shader_destroy(f->dev, frag);
    nph_gpu_shader_destroy(f->dev, NPH_GPU_SHADER_NULL);
    NTEST_ASSERT_FALSE(nph_gpu_shader_is_valid(f->dev, frag));
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).shaders, 0U);
    NphGpuShader missing = NPH_GPU_SHADER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_load(f->dev, "no/such/file.spv", NULL, &missing),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_TRUE(nph_gpu_shader_is_null(missing));
    const u32 garbage[8] = {0U};
    NTEST_ASSERT_EQ_INT(nph_gpu_shader_create(f->dev, garbage, sizeof garbage, "garbage", &missing),
                        NPH_RESULT_CORRUPT);
}
