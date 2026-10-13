// SPDX-License-Identifier: Apache-2.0
// A minimal SPIR-V reflection parser (CLAUDE.md §8.5 "our own minimal
// parser"): entry point and stage, push constant block size, workgroup
// size, the bindless bindings a shader uses, fragment outputs, and the
// checks that reject shaders outside the engine's layout. Two passes over
// the instruction stream (decorations precede the types they decorate in
// SPIR-V's logical layout, so types are collected first), every word
// bounds-checked. Fuzzed by tests/fuzz/fuzz_spirv.c.
//
// Opcode and enum values from the SPIR-V 1.6 specification (Khronos).
#include "gpu_internal.h"

#define SPV_MAGIC 0x07230203U

enum {
    OP_EXTENSION = 10,
    OP_MEMORY_MODEL = 14,
    OP_ENTRY_POINT = 15,
    OP_EXECUTION_MODE = 16,
    OP_CAPABILITY = 17,
    OP_TYPE_VOID = 19,
    OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_MATRIX = 24,
    OP_TYPE_IMAGE = 25,
    OP_TYPE_SAMPLER = 26,
    OP_TYPE_SAMPLED_IMAGE = 27,
    OP_TYPE_ARRAY = 28,
    OP_TYPE_RUNTIME_ARRAY = 29,
    OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32,
    OP_TYPE_FORWARD_POINTER = 39,
    OP_CONSTANT_TRUE = 41,
    OP_CONSTANT = 43,
    OP_VARIABLE = 59,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    OP_FUNCTION = 54,
    OP_EXECUTION_MODE_ID = 331,
};

enum {
    DECO_BUILTIN = 11,
    DECO_ARRAY_STRIDE = 6,
    DECO_MATRIX_STRIDE = 7,
    DECO_LOCATION = 30,
    DECO_BINDING = 33,
    DECO_DESCRIPTOR_SET = 34,
    DECO_OFFSET = 35,
};

enum {
    STORAGE_UNIFORM_CONSTANT = 0,
    STORAGE_INPUT = 1,
    STORAGE_UNIFORM = 2,
    STORAGE_OUTPUT = 3,
    STORAGE_PUSH_CONSTANT = 9,
    STORAGE_STORAGE_BUFFER = 12,
    STORAGE_PHYSICAL_STORAGE_BUFFER = 5349,
};

enum {
    MODEL_VERTEX = 0,
    MODEL_FRAGMENT = 4,
    MODEL_GL_COMPUTE = 5,
    MODEL_TASK_EXT = 5364,
    MODEL_MESH_EXT = 5365,
};

enum {
    MODE_LOCAL_SIZE = 17,
    MODE_LOCAL_SIZE_ID = 38,
};

enum {
    CAP_PHYSICAL_STORAGE_BUFFER_ADDRESSES = 5347,
};

typedef enum IdKind {
    ID_NONE = 0,
    ID_TYPE_BOOL,
    ID_TYPE_INT,
    ID_TYPE_FLOAT,
    ID_TYPE_VECTOR,
    ID_TYPE_MATRIX,
    ID_TYPE_IMAGE,
    ID_TYPE_SAMPLER,
    ID_TYPE_SAMPLED_IMAGE,
    ID_TYPE_ARRAY,
    ID_TYPE_RUNTIME_ARRAY,
    ID_TYPE_STRUCT,
    ID_TYPE_POINTER,
    ID_TYPE_FORWARD_POINTER,
    ID_TYPE_OTHER,
    ID_CONSTANT,
    ID_VARIABLE,
} IdKind;

typedef struct Id {
    u8 kind;
    u8 has_set;
    u8 has_binding;
    u8 has_location;
    u8 builtin;
    u8 sampled; // images: 1 sampled, 2 storage
    u32 a;      // width / component or element type / pointee / variable type / constant value
    u32 b;      // count / length constant id / storage class
    u32 first_member;
    u32 member_count;
    u32 set;
    u32 binding;
    u32 location;
    u32 array_stride;
} Id;

typedef struct Member {
    u32 type;
    u32 offset;
    u32 matrix_stride;
    u8 has_offset;
} Member;

typedef struct Parser {
    const u32 *words;
    u64 count;
    u32 bound;
    Id *ids;
    Member *members;
    u32 member_count;
    u32 member_cap;
    NphGpuShaderInfo *info;
    u32 entry_id;
    u32 model;
    b32 have_entry;
    u32 local_size_ids[3];
    b32 have_local_size_ids;
    const char *reject;
} Parser;

#define MAX_BOUND   (1U << 20U)
#define MAX_MEMBERS (1U << 20U)
#define MAX_DEPTH   32U

static bool id_ok(const Parser *p, u32 id)
{
    return id != 0U && id < p->bound;
}

static NphResult prescan(Parser *p)
{
    // Validates the instruction framing and counts struct members.
    u64 pos = 5U;
    u64 members = 0U;
    while (pos < p->count) {
        const u32 w = p->words[pos];
        const u32 n = w >> 16U;
        const u32 op = w & 0xFFFFU;
        if (n == 0U || pos + n > p->count) {
            return NPH_RESULT_CORRUPT;
        }
        if (op == OP_TYPE_STRUCT) {
            if (n < 2U) {
                return NPH_RESULT_CORRUPT;
            }
            members += n - 2U;
        }
        pos += n;
    }
    if (members > MAX_MEMBERS) {
        return NPH_RESULT_CORRUPT;
    }
    p->member_cap = (u32)members;
    return NPH_RESULT_OK;
}

// Reads a literal string starting at word `pos`; returns the number of
// words it occupies or 0 when unterminated.
static u32 read_string(const Parser *p, u64 pos, u64 end, char *out, u32 cap)
{
    u32 len = 0U;
    for (u64 i = pos; i < end; ++i) {
        const u32 w = p->words[i];
        for (u32 b = 0; b < 4U; ++b) {
            const char c = (char)((w >> (8U * b)) & 0xFFU);
            if (c == '\0') {
                if (out != NULL) {
                    out[len < cap ? len : cap - 1U] = '\0';
                }
                return (u32)(i - pos + 1U);
            }
            if (out != NULL && len < cap - 1U) {
                out[len] = c;
            }
            ++len;
        }
    }
    return 0U;
}

static NphResult pass_types(Parser *p)
{
    u64 pos = 5U;
    while (pos < p->count) {
        const u32 w = p->words[pos];
        const u32 n = w >> 16U;
        const u32 op = w & 0xFFFFU;
        const u32 *ops = p->words + pos + 1U;
        const u32 argc = n - 1U;
        switch (op) {
        case OP_CAPABILITY:
            if (argc < 1U) {
                return NPH_RESULT_CORRUPT;
            }
            ++p->info->capability_count;
            if (ops[0] == CAP_PHYSICAL_STORAGE_BUFFER_ADDRESSES) {
                p->info->uses_device_address = 1;
            }
            break;
        case OP_ENTRY_POINT: {
            if (argc < 3U || !id_ok(p, ops[1])) {
                return NPH_RESULT_CORRUPT;
            }
            if (!p->have_entry) {
                p->model = ops[0];
                p->entry_id = ops[1];
                const u32 str_words = read_string(p, pos + 3U, pos + n, p->info->entry_point,
                                                  NPH_GPU_ENTRY_POINT_MAX);
                if (str_words == 0U) {
                    return NPH_RESULT_CORRUPT;
                }
                p->have_entry = 1;
            }
            break;
        }
        case OP_EXECUTION_MODE:
            if (argc < 2U) {
                return NPH_RESULT_CORRUPT;
            }
            if (ops[1] == MODE_LOCAL_SIZE) {
                if (argc < 5U) {
                    return NPH_RESULT_CORRUPT;
                }
                p->info->workgroup_size[0] = ops[2];
                p->info->workgroup_size[1] = ops[3];
                p->info->workgroup_size[2] = ops[4];
            }
            break;
        case OP_EXECUTION_MODE_ID:
            if (argc < 2U) {
                return NPH_RESULT_CORRUPT;
            }
            if (ops[1] == MODE_LOCAL_SIZE_ID) {
                if (argc < 5U) {
                    return NPH_RESULT_CORRUPT;
                }
                p->local_size_ids[0] = ops[2];
                p->local_size_ids[1] = ops[3];
                p->local_size_ids[2] = ops[4];
                p->have_local_size_ids = 1;
            }
            break;
        case OP_TYPE_VOID:
        case OP_TYPE_BOOL:
        case OP_TYPE_INT:
        case OP_TYPE_FLOAT:
        case OP_TYPE_VECTOR:
        case OP_TYPE_MATRIX:
        case OP_TYPE_IMAGE:
        case OP_TYPE_SAMPLER:
        case OP_TYPE_SAMPLED_IMAGE:
        case OP_TYPE_ARRAY:
        case OP_TYPE_RUNTIME_ARRAY:
        case OP_TYPE_STRUCT:
        case OP_TYPE_POINTER: {
            if (argc < 1U || !id_ok(p, ops[0])) {
                return NPH_RESULT_CORRUPT;
            }
            Id *id = &p->ids[ops[0]];
            if (id->kind != ID_NONE && id->kind != ID_TYPE_FORWARD_POINTER) {
                return NPH_RESULT_CORRUPT; // redefined id
            }
            switch (op) {
            case OP_TYPE_BOOL:
                id->kind = ID_TYPE_BOOL;
                break;
            case OP_TYPE_INT:
            case OP_TYPE_FLOAT:
                if (argc < 2U) {
                    return NPH_RESULT_CORRUPT;
                }
                id->kind = op == OP_TYPE_INT ? ID_TYPE_INT : ID_TYPE_FLOAT;
                id->a = ops[1];
                break;
            case OP_TYPE_VECTOR:
            case OP_TYPE_MATRIX:
                if (argc < 3U || !id_ok(p, ops[1])) {
                    return NPH_RESULT_CORRUPT;
                }
                id->kind = op == OP_TYPE_VECTOR ? ID_TYPE_VECTOR : ID_TYPE_MATRIX;
                id->a = ops[1];
                id->b = ops[2];
                break;
            case OP_TYPE_IMAGE:
                if (argc < 8U) {
                    return NPH_RESULT_CORRUPT;
                }
                id->kind = ID_TYPE_IMAGE;
                id->sampled = (u8)(ops[6] & 0xFFU);
                break;
            case OP_TYPE_SAMPLER:
                id->kind = ID_TYPE_SAMPLER;
                break;
            case OP_TYPE_SAMPLED_IMAGE:
                id->kind = ID_TYPE_SAMPLED_IMAGE;
                break;
            case OP_TYPE_ARRAY:
                if (argc < 3U || !id_ok(p, ops[1]) || !id_ok(p, ops[2])) {
                    return NPH_RESULT_CORRUPT;
                }
                id->kind = ID_TYPE_ARRAY;
                id->a = ops[1];
                id->b = ops[2];
                break;
            case OP_TYPE_RUNTIME_ARRAY:
                if (argc < 2U || !id_ok(p, ops[1])) {
                    return NPH_RESULT_CORRUPT;
                }
                id->kind = ID_TYPE_RUNTIME_ARRAY;
                id->a = ops[1];
                break;
            case OP_TYPE_STRUCT:
                id->kind = ID_TYPE_STRUCT;
                id->first_member = p->member_count;
                id->member_count = argc - 1U;
                if (p->member_count + id->member_count > p->member_cap) {
                    return NPH_RESULT_CORRUPT;
                }
                for (u32 i = 0; i < id->member_count; ++i) {
                    if (!id_ok(p, ops[1U + i])) {
                        return NPH_RESULT_CORRUPT;
                    }
                    Member *m = &p->members[p->member_count++];
                    m->type = ops[1U + i];
                    m->offset = 0U;
                    m->matrix_stride = 0U;
                    m->has_offset = 0U;
                }
                break;
            case OP_TYPE_POINTER:
                if (argc < 3U || !id_ok(p, ops[2])) {
                    return NPH_RESULT_CORRUPT;
                }
                id->kind = ID_TYPE_POINTER;
                id->b = ops[1];
                id->a = ops[2];
                break;
            default:
                id->kind = ID_TYPE_OTHER;
                break;
            }
            break;
        }
        case OP_TYPE_FORWARD_POINTER:
            if (argc < 2U || !id_ok(p, ops[0])) {
                return NPH_RESULT_CORRUPT;
            }
            if (p->ids[ops[0]].kind == ID_NONE) {
                p->ids[ops[0]].kind = ID_TYPE_FORWARD_POINTER;
                p->ids[ops[0]].b = ops[1];
            }
            break;
        case OP_CONSTANT: {
            if (argc < 3U || !id_ok(p, ops[1])) {
                return NPH_RESULT_CORRUPT;
            }
            Id *id = &p->ids[ops[1]];
            if (id->kind != ID_NONE) {
                return NPH_RESULT_CORRUPT;
            }
            id->kind = ID_CONSTANT;
            id->a = ops[2]; // low word
            id->b = ops[0]; // type
            break;
        }
        case OP_VARIABLE: {
            if (argc < 3U || !id_ok(p, ops[0]) || !id_ok(p, ops[1])) {
                return NPH_RESULT_CORRUPT;
            }
            Id *id = &p->ids[ops[1]];
            if (id->kind != ID_NONE) {
                return NPH_RESULT_CORRUPT;
            }
            id->kind = ID_VARIABLE;
            id->a = ops[0]; // pointer type
            id->b = ops[2]; // storage class
            break;
        }
        case OP_FUNCTION:
            // Everything of interest precedes the first function.
            return NPH_RESULT_OK;
        default:
            break;
        }
        pos += n;
    }
    return NPH_RESULT_OK;
}

static NphResult pass_decorations(Parser *p)
{
    u64 pos = 5U;
    while (pos < p->count) {
        const u32 w = p->words[pos];
        const u32 n = w >> 16U;
        const u32 op = w & 0xFFFFU;
        const u32 *ops = p->words + pos + 1U;
        const u32 argc = n - 1U;
        if (op == OP_DECORATE) {
            if (argc < 2U || !id_ok(p, ops[0])) {
                return NPH_RESULT_CORRUPT;
            }
            Id *id = &p->ids[ops[0]];
            switch (ops[1]) {
            case DECO_DESCRIPTOR_SET:
                if (argc < 3U) {
                    return NPH_RESULT_CORRUPT;
                }
                id->has_set = 1U;
                id->set = ops[2];
                break;
            case DECO_BINDING:
                if (argc < 3U) {
                    return NPH_RESULT_CORRUPT;
                }
                id->has_binding = 1U;
                id->binding = ops[2];
                break;
            case DECO_LOCATION:
                if (argc < 3U) {
                    return NPH_RESULT_CORRUPT;
                }
                id->has_location = 1U;
                id->location = ops[2];
                break;
            case DECO_BUILTIN:
                id->builtin = 1U;
                break;
            case DECO_ARRAY_STRIDE:
                if (argc < 3U) {
                    return NPH_RESULT_CORRUPT;
                }
                id->array_stride = ops[2];
                break;
            default:
                break;
            }
        } else if (op == OP_MEMBER_DECORATE) {
            if (argc < 3U || !id_ok(p, ops[0])) {
                return NPH_RESULT_CORRUPT;
            }
            const Id *st = &p->ids[ops[0]];
            if (st->kind == ID_TYPE_STRUCT && ops[1] < st->member_count) {
                Member *m = &p->members[st->first_member + ops[1]];
                if (ops[2] == DECO_OFFSET) {
                    if (argc < 4U) {
                        return NPH_RESULT_CORRUPT;
                    }
                    m->offset = ops[3];
                    m->has_offset = 1U;
                } else if (ops[2] == DECO_MATRIX_STRIDE) {
                    if (argc < 4U) {
                        return NPH_RESULT_CORRUPT;
                    }
                    m->matrix_stride = ops[3];
                } else if (ops[2] == DECO_BUILTIN) {
                    // Built-in blocks (gl_PerVertex) carry no layout.
                }
            }
        } else if (op == OP_FUNCTION) {
            break;
        }
        pos += n;
    }
    return NPH_RESULT_OK;
}

static u64 type_size(const Parser *p, u32 type, u32 matrix_stride, u32 depth);

static u64 constant_value(const Parser *p, u32 id)
{
    if (!id_ok(p, id) || p->ids[id].kind != ID_CONSTANT) {
        return 0U;
    }
    return p->ids[id].a;
}

static u64 type_size(const Parser *p, u32 type, u32 matrix_stride, u32 depth)
{
    if (!id_ok(p, type) || depth > MAX_DEPTH) {
        return 0U;
    }
    const Id *t = &p->ids[type];
    switch ((IdKind)t->kind) {
    case ID_TYPE_BOOL:
        return 4U;
    case ID_TYPE_INT:
    case ID_TYPE_FLOAT:
        return t->a / 8U;
    case ID_TYPE_VECTOR:
        return (u64)t->b * type_size(p, t->a, 0U, depth + 1U);
    case ID_TYPE_MATRIX:
        if (matrix_stride != 0U) {
            return (u64)t->b * matrix_stride;
        }
        return (u64)t->b * type_size(p, t->a, 0U, depth + 1U);
    case ID_TYPE_ARRAY: {
        const u64 length = constant_value(p, t->b);
        const u64 stride =
            t->array_stride != 0U ? t->array_stride : type_size(p, t->a, 0U, depth + 1U);
        return length * stride;
    }
    case ID_TYPE_STRUCT: {
        u64 size = 0U;
        for (u32 i = 0; i < t->member_count; ++i) {
            const Member *m = &p->members[t->first_member + i];
            const u64 end = (u64)m->offset + type_size(p, m->type, m->matrix_stride, depth + 1U);
            if (end > size) {
                size = end;
            }
        }
        return size;
    }
    case ID_TYPE_POINTER:
    case ID_TYPE_FORWARD_POINTER:
        return 8U; // a buffer reference
    default:
        return 0U;
    }
}

// Strips arrays (and runtime arrays) from a descriptor type.
static const Id *element_type(const Parser *p, u32 type, u32 depth)
{
    if (!id_ok(p, type) || depth > MAX_DEPTH) {
        return NULL;
    }
    const Id *t = &p->ids[type];
    if (t->kind == ID_TYPE_ARRAY || t->kind == ID_TYPE_RUNTIME_ARRAY) {
        return element_type(p, t->a, depth + 1U);
    }
    return t;
}

static NphResult analyse(Parser *p)
{
    NphGpuShaderInfo *info = p->info;
    if (!p->have_entry) {
        p->reject = "no entry point";
        return NPH_RESULT_CORRUPT;
    }
    switch (p->model) {
    case MODEL_VERTEX:
        info->stage = NPH_GPU_SHADER_VERTEX;
        break;
    case MODEL_FRAGMENT:
        info->stage = NPH_GPU_SHADER_FRAGMENT;
        break;
    case MODEL_GL_COMPUTE:
        info->stage = NPH_GPU_SHADER_COMPUTE;
        break;
    case MODEL_TASK_EXT:
        info->stage = NPH_GPU_SHADER_TASK;
        break;
    case MODEL_MESH_EXT:
        info->stage = NPH_GPU_SHADER_MESH;
        break;
    default:
        p->reject = "execution model is not vertex, fragment, compute, task, or mesh";
        return NPH_RESULT_UNSUPPORTED;
    }
    if (p->have_local_size_ids) {
        for (u32 i = 0; i < 3U; ++i) {
            info->workgroup_size[i] = (u32)constant_value(p, p->local_size_ids[i]);
        }
    }
    for (u32 id = 1; id < p->bound; ++id) {
        const Id *v = &p->ids[id];
        if (v->kind != ID_VARIABLE) {
            continue;
        }
        const Id *ptr = id_ok(p, v->a) ? &p->ids[v->a] : NULL;
        const u32 pointee = ptr != NULL && ptr->kind == ID_TYPE_POINTER ? ptr->a : 0U;
        switch (v->b) {
        case STORAGE_PUSH_CONSTANT: {
            const u64 size = type_size(p, pointee, 0U, 0U);
            if (size > NPH_GPU_PUSH_CONSTANT_BYTES) {
                p->reject = "push constant block larger than NPH_GPU_PUSH_CONSTANT_BYTES";
                return NPH_RESULT_UNSUPPORTED;
            }
            if (size > info->push_constant_bytes) {
                info->push_constant_bytes = (u32)size;
            }
            break;
        }
        case STORAGE_UNIFORM_CONSTANT: {
            const Id *elem = element_type(p, pointee, 0U);
            if (elem == NULL) {
                p->reject = "descriptor variable with an unknown type";
                return NPH_RESULT_CORRUPT;
            }
            if (!v->has_set || !v->has_binding) {
                p->reject = "descriptor without set and binding";
                return NPH_RESULT_UNSUPPORTED;
            }
            if (v->set != 0U) {
                p->reject = "descriptor outside set 0 (only the bindless set exists)";
                return NPH_RESULT_UNSUPPORTED;
            }
            if (v->binding == 0U && elem->kind == ID_TYPE_IMAGE && elem->sampled == 1U) {
                info->bindless_uses |= (u32)NPH_GPU_BINDLESS_SAMPLED_IMAGES;
            } else if (v->binding == 1U && elem->kind == ID_TYPE_IMAGE && elem->sampled == 2U) {
                info->bindless_uses |= (u32)NPH_GPU_BINDLESS_STORAGE_IMAGES;
            } else if (v->binding == 2U && elem->kind == ID_TYPE_SAMPLER) {
                info->bindless_uses |= (u32)NPH_GPU_BINDLESS_SAMPLERS;
            } else {
                p->reject = "descriptor does not match the bindless layout (binding 0: sampled "
                            "images, 1: storage images, 2: samplers; no combined samplers)";
                return NPH_RESULT_UNSUPPORTED;
            }
            break;
        }
        case STORAGE_UNIFORM:
        case STORAGE_STORAGE_BUFFER:
            if (v->has_set || v->has_binding) {
                p->reject = "uniform or storage buffer descriptor (use buffer references)";
                return NPH_RESULT_UNSUPPORTED;
            }
            break;
        case STORAGE_INPUT:
            if (info->stage == NPH_GPU_SHADER_VERTEX && v->has_location && !v->builtin) {
                p->reject = "vertex input attribute (vertex data is pulled from buffer addresses)";
                return NPH_RESULT_UNSUPPORTED;
            }
            break;
        case STORAGE_OUTPUT:
            if (info->stage == NPH_GPU_SHADER_FRAGMENT && v->has_location && !v->builtin &&
                v->location < 32U) {
                info->color_output_mask |= 1U << v->location;
            }
            break;
        default:
            break;
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_gpu_spirv_reflect(const void *spirv, u64 size, NphAllocator *scratch,
                                NphGpuShaderInfo *out)
{
    NPH_ASSERT(out != NULL && scratch != NULL);
    memset(out, 0, sizeof *out);
    if (spirv == NULL || size < 5U * sizeof(u32) || (size % 4U) != 0U ||
        !nph_is_ptr_aligned(spirv, 4U)) {
        return NPH_RESULT_CORRUPT;
    }
    Parser p;
    memset(&p, 0, sizeof p);
    p.words = (const u32 *)spirv;
    p.count = size / 4U;
    p.info = out;
    if (p.words[0] != SPV_MAGIC) {
        return NPH_RESULT_CORRUPT;
    }
    out->spirv_version_major = (p.words[1] >> 16U) & 0xFFU;
    out->spirv_version_minor = (p.words[1] >> 8U) & 0xFFU;
    p.bound = p.words[3];
    if (p.bound == 0U || p.bound > MAX_BOUND) {
        return NPH_RESULT_CORRUPT;
    }
    NphResult r = prescan(&p);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    p.ids = NPH_ALLOC_ARRAY_ZEROED(scratch, Id, p.bound);
    if (p.ids == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (p.member_cap != 0U) {
        p.members = NPH_ALLOC_ARRAY_ZEROED(scratch, Member, p.member_cap);
        if (p.members == NULL) {
            NPH_FREE_ARRAY(scratch, Id, p.ids, p.bound);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    r = pass_types(&p);
    if (r == NPH_RESULT_OK) {
        r = pass_decorations(&p);
    }
    if (r == NPH_RESULT_OK) {
        r = analyse(&p);
    }
    if (r == NPH_RESULT_UNSUPPORTED && p.reject != NULL) {
        GPU_LOG_ERROR("shader '%s' rejected: %s", out->entry_point, p.reject);
    }
    if (p.members != NULL) {
        NPH_FREE_ARRAY(scratch, Member, p.members, p.member_cap);
    }
    NPH_FREE_ARRAY(scratch, Id, p.ids, p.bound);
    return r;
}
