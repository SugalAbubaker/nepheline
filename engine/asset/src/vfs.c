// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/pack.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/platform/path.h>

#include <string.h>

// Room for a backing directory plus a virtual path.
#define VFS_OS_PATH_MAX (NPH_VFS_MAX_PATH * 2U)
// Directory walks are bounded so a pathological tree (or a symlink loop)
// cannot exhaust the stack.
#define VFS_MAX_WALK_DEPTH 32U

typedef struct Mount {
    NphVfsMountType type;
    u32 id;
    char *prefix; // "" or "a/b/" (owned)
    u64 prefix_len;
    char *backing; // the directory (no trailing separator) or the pack path (owned)
    u64 backing_len;
    NphPack pack; // NPH_VFS_MOUNT_PACK
} Mount;

struct NphVfs {
    NphAllocator *alloc;
    Mount mounts[NPH_VFS_MAX_MOUNTS]; // in mount order
    u32 mount_count;
    u32 next_id;
};

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool nph_vfs_path_is_normalized(NphStr path)
{
    if (path.len == 0U || path.len > NPH_VFS_MAX_PATH) {
        return false;
    }
    if (path.data[0] == '/' || path.data[path.len - 1U] == '/') {
        return false;
    }
    u64 start = 0U;
    for (u64 i = 0; i <= path.len; ++i) {
        const char c = i < path.len ? path.data[i] : '/';
        if (c == '\\' || c == '\0') {
            return false;
        }
        if (c == '/') {
            const u64 n = i - start;
            if (n == 0U || (n == 1U && path.data[start] == '.') ||
                (n == 2U && path.data[start] == '.' && path.data[start + 1U] == '.')) {
                return false;
            }
            start = i + 1U;
        }
    }
    return true;
}

NphResult nph_vfs_normalize_path(NphStr path, char *buf, u64 cap, NphStr *out)
{
    NPH_ASSERT(buf != NULL && cap > 0U);
    u64 len = 0U;
    u64 start = 0U;
    for (u64 i = 0; i <= path.len; ++i) {
        const char c = i < path.len ? path.data[i] : '/';
        if (c == '\0') {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        if (!nph_path_is_separator(c)) {
            continue;
        }
        const NphStr comp = nph_str(path.data + start, i - start);
        start = i + 1U;
        if (comp.len == 0U || nph_str_equals(comp, NPH_STR("."))) {
            continue;
        }
        if (nph_str_equals(comp, NPH_STR(".."))) {
            if (len == 0U) {
                return NPH_RESULT_INVALID_ARGUMENT; // would climb above the root
            }
            // Drop the last component and its separator.
            while (len > 0U && buf[len - 1U] != '/') {
                --len;
            }
            if (len > 0U) {
                --len;
            }
            continue;
        }
        const u64 need = len + (len != 0U ? 1U : 0U) + comp.len;
        if (need + 1U > cap || need > NPH_VFS_MAX_PATH) {
            return NPH_RESULT_OVERFLOW;
        }
        if (len != 0U) {
            buf[len++] = '/';
        }
        memcpy(buf + len, comp.data, (size_t)comp.len);
        len += comp.len;
    }
    buf[len] = '\0';
    if (len == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (out != NULL) {
        *out = nph_str(buf, len);
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle and mounts
// ---------------------------------------------------------------------------

NphResult nph_vfs_create(NphAllocator *alloc, NphVfs **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    NphVfs *vfs = NPH_ALLOC_TYPE(alloc, NphVfs);
    if (vfs == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    vfs->alloc = alloc;
    vfs->next_id = 1U;
    *out = vfs;
    return NPH_RESULT_OK;
}

static void mount_release(NphVfs *vfs, Mount *m)
{
    if (m->type == NPH_VFS_MOUNT_PACK) {
        nph_pack_close(&m->pack);
    }
    if (m->prefix != NULL) {
        nph_str_free(vfs->alloc, m->prefix, m->prefix_len);
    }
    if (m->backing != NULL) {
        nph_str_free(vfs->alloc, m->backing, m->backing_len);
    }
    memset(m, 0, sizeof *m);
}

void nph_vfs_destroy(NphVfs *vfs)
{
    if (vfs == NULL) {
        return;
    }
    for (u32 i = 0; i < vfs->mount_count; ++i) {
        mount_release(vfs, &vfs->mounts[i]);
    }
    NphAllocator *alloc = vfs->alloc;
    NPH_FREE_TYPE(alloc, NphVfs, vfs);
}

// Stores the prefix in canonical form ("" or "a/b/").
static NphResult mount_set_prefix(NphVfs *vfs, Mount *m, NphStr prefix)
{
    char buf[NPH_VFS_MAX_PATH + 2U];
    NphStr norm = nph_str_empty();
    if (prefix.len != 0U) {
        const NphResult r = nph_vfs_normalize_path(prefix, buf, sizeof buf - 1U, &norm);
        if (r == NPH_RESULT_OK) {
            buf[norm.len] = '/';
            buf[norm.len + 1U] = '\0';
            norm.len += 1U;
        } else if (r != NPH_RESULT_INVALID_ARGUMENT ||
                   !nph_str_equals(nph_str_trim(prefix), NPH_STR("/"))) {
            return r;
        }
    }
    m->prefix_len = norm.len;
    return nph_str_dup(vfs->alloc, norm, &m->prefix);
}

static NphResult mount_add(NphVfs *vfs, NphVfsMountType type, NphStr prefix, const char *backing,
                           u32 *out_id)
{
    NPH_ASSERT(vfs != NULL && backing != NULL);
    if (vfs->mount_count >= NPH_VFS_MAX_MOUNTS) {
        return NPH_RESULT_FULL;
    }
    Mount *m = &vfs->mounts[vfs->mount_count];
    memset(m, 0, sizeof *m);
    m->type = type;
    NphResult r = mount_set_prefix(vfs, m, prefix);
    if (r == NPH_RESULT_OK) {
        NphStr b = nph_str_from_cstr(backing);
        // A directory keeps no trailing separator so joins are uniform.
        while (type == NPH_VFS_MOUNT_DIRECTORY && b.len > 1U &&
               nph_path_is_separator(b.data[b.len - 1U])) {
            b.len -= 1U;
        }
        m->backing_len = b.len;
        r = nph_str_dup(vfs->alloc, b, &m->backing);
    }
    if (r == NPH_RESULT_OK && type == NPH_VFS_MOUNT_PACK) {
        r = nph_pack_open(vfs->alloc, backing, &m->pack);
    }
    if (r != NPH_RESULT_OK) {
        mount_release(vfs, m);
        return r;
    }
    m->id = vfs->next_id++;
    vfs->mount_count += 1U;
    if (out_id != NULL) {
        *out_id = m->id;
    }
    NPH_LOG_DEBUG(&NPH_LOG_ASSET, "mounted %s '%s' at '%s' (id %u)",
                  type == NPH_VFS_MOUNT_PACK ? "pack" : "directory", backing, m->prefix, m->id);
    return NPH_RESULT_OK;
}

NphResult nph_vfs_mount_directory(NphVfs *vfs, NphStr prefix, const char *os_dir, u32 *out_id)
{
    return mount_add(vfs, NPH_VFS_MOUNT_DIRECTORY, prefix, os_dir, out_id);
}

NphResult nph_vfs_mount_pack(NphVfs *vfs, NphStr prefix, const char *pack_path, u32 *out_id)
{
    return mount_add(vfs, NPH_VFS_MOUNT_PACK, prefix, pack_path, out_id);
}

NphResult nph_vfs_unmount(NphVfs *vfs, u32 id)
{
    NPH_ASSERT(vfs != NULL);
    for (u32 i = 0; i < vfs->mount_count; ++i) {
        if (vfs->mounts[i].id == id) {
            mount_release(vfs, &vfs->mounts[i]);
            for (u32 j = i + 1U; j < vfs->mount_count; ++j) {
                vfs->mounts[j - 1U] = vfs->mounts[j];
            }
            vfs->mount_count -= 1U;
            memset(&vfs->mounts[vfs->mount_count], 0, sizeof(Mount));
            return NPH_RESULT_OK;
        }
    }
    return NPH_RESULT_NOT_FOUND;
}

u32 nph_vfs_mount_count(const NphVfs *vfs)
{
    NPH_ASSERT(vfs != NULL);
    return vfs->mount_count;
}

NphVfsMountInfo nph_vfs_mount_info(const NphVfs *vfs, u32 index)
{
    NPH_ASSERT(vfs != NULL && index < vfs->mount_count);
    const Mount *m = &vfs->mounts[index];
    NphVfsMountInfo info;
    info.type = m->type;
    info.prefix = nph_str(m->prefix, m->prefix_len);
    info.backing = nph_str(m->backing, m->backing_len);
    info.id = m->id;
    return info;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

// The part of `path` under the mount's prefix, or false.
static bool mount_relative(const Mount *m, NphStr path, NphStr *out_rel)
{
    if (!nph_str_starts_with(path, nph_str(m->prefix, m->prefix_len))) {
        return false;
    }
    *out_rel = nph_str_slice(path, m->prefix_len, path.len);
    return out_rel->len != 0U;
}

static NphResult mount_os_path(const Mount *m, NphStr rel, char *buf, u64 cap, u64 *out_len)
{
    return nph_path_join(buf, cap, nph_str(m->backing, m->backing_len), rel, out_len);
}

// Finds the newest mount holding `path`. For directory mounts the file
// system is consulted; *out_info is filled when not NULL.
static const Mount *resolve(const NphVfs *vfs, NphStr path, NphVfsInfo *out_info,
                            const NphPackEntry **out_entry, char *os_path, u64 os_cap)
{
    if (!nph_vfs_path_is_normalized(path)) {
        return NULL;
    }
    for (u32 i = vfs->mount_count; i-- > 0U;) {
        const Mount *m = &vfs->mounts[i];
        NphStr rel;
        if (!mount_relative(m, path, &rel)) {
            continue;
        }
        if (m->type == NPH_VFS_MOUNT_PACK) {
            const NphPackEntry *e = nph_pack_find_path(&m->pack, rel);
            if (e == NULL) {
                continue;
            }
            if (out_info != NULL) {
                out_info->size = e->size;
                out_info->mount_id = m->id;
                out_info->source = NPH_VFS_MOUNT_PACK;
                out_info->is_directory = 0;
            }
            if (out_entry != NULL) {
                *out_entry = e;
            }
            return m;
        }
        if (mount_os_path(m, rel, os_path, os_cap, NULL) != NPH_RESULT_OK) {
            continue;
        }
        NphFileInfo fi;
        if (nph_fs_stat(os_path, &fi) != NPH_RESULT_OK) {
            continue;
        }
        if (out_info != NULL) {
            out_info->size = fi.size;
            out_info->mount_id = m->id;
            out_info->source = NPH_VFS_MOUNT_DIRECTORY;
            out_info->is_directory = fi.is_directory;
        }
        return m;
    }
    return NULL;
}

NphResult nph_vfs_stat(const NphVfs *vfs, NphStr path, NphVfsInfo *out)
{
    NPH_ASSERT(vfs != NULL && out != NULL);
    char os_path[VFS_OS_PATH_MAX];
    memset(out, 0, sizeof *out);
    return resolve(vfs, path, out, NULL, os_path, sizeof os_path) != NULL ? NPH_RESULT_OK
                                                                          : NPH_RESULT_NOT_FOUND;
}

bool nph_vfs_exists(const NphVfs *vfs, NphStr path)
{
    char os_path[VFS_OS_PATH_MAX];
    return resolve(vfs, path, NULL, NULL, os_path, sizeof os_path) != NULL;
}

NphResult nph_vfs_read_all(const NphVfs *vfs, NphStr path, NphAllocator *alloc, void **out_data,
                           u64 *out_size)
{
    NPH_ASSERT(vfs != NULL && alloc != NULL && out_data != NULL && out_size != NULL);
    char os_path[VFS_OS_PATH_MAX];
    NphVfsInfo info;
    const NphPackEntry *entry = NULL;
    const Mount *m = resolve(vfs, path, &info, &entry, os_path, sizeof os_path);
    if (m == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (m->type == NPH_VFS_MOUNT_DIRECTORY) {
        if (info.is_directory) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        return nph_file_read_all(os_path, alloc, out_data, out_size);
    }
    u8 *copy = (u8 *)NPH_ALLOC(alloc, entry->size + 1U, 16U);
    if (copy == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (entry->size != 0U) {
        memcpy(copy, nph_pack_entry_data(&m->pack, entry), (size_t)entry->size);
    }
    copy[entry->size] = 0U;
    *out_data = copy;
    *out_size = entry->size;
    return NPH_RESULT_OK;
}

void nph_vfs_free_all(NphAllocator *alloc, void *data, u64 size)
{
    nph_file_free_all(alloc, data, size);
}

NphResult nph_vfs_map(const NphVfs *vfs, NphStr path, NphVfsMapping *out)
{
    NPH_ASSERT(vfs != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    char os_path[VFS_OS_PATH_MAX];
    NphVfsInfo info;
    const NphPackEntry *entry = NULL;
    const Mount *m = resolve(vfs, path, &info, &entry, os_path, sizeof os_path);
    if (m == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    out->mount_id = m->id;
    out->source = m->type;
    if (m->type == NPH_VFS_MOUNT_PACK) {
        out->data = nph_pack_entry_data(&m->pack, entry);
        out->size = entry->size;
        return NPH_RESULT_OK;
    }
    if (info.is_directory) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(nph_file_open(os_path, NPH_FILE_READ | NPH_FILE_RANDOM, &out->file));
    u64 size = 0U;
    NphResult r = nph_file_size(&out->file, &size);
    if (r == NPH_RESULT_OK && size != 0U) {
        r = nph_file_map(&out->file, 0U, size, NPH_FILE_MAP_READ, &out->mapping);
    }
    if (r != NPH_RESULT_OK) {
        nph_file_close(&out->file);
        return r;
    }
    out->data = size != 0U ? out->mapping.data : NULL;
    out->size = size;
    return NPH_RESULT_OK;
}

void nph_vfs_unmap(const NphVfs *vfs, NphVfsMapping *m)
{
    NPH_UNUSED(vfs);
    if (m == NULL) {
        return;
    }
    if (m->source == NPH_VFS_MOUNT_DIRECTORY) {
        nph_file_unmap(&m->mapping);
        nph_file_close(&m->file);
    }
    memset(m, 0, sizeof *m);
}

NphResult nph_vfs_open(const NphVfs *vfs, NphStr path, u32 extra_mode, NphVfsFile *out)
{
    NPH_ASSERT(vfs != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    char os_path[VFS_OS_PATH_MAX];
    NphVfsInfo info;
    const NphPackEntry *entry = NULL;
    const Mount *m = resolve(vfs, path, &info, &entry, os_path, sizeof os_path);
    if (m == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    out->mount_id = m->id;
    out->source = m->type;
    if (m->type == NPH_VFS_MOUNT_PACK) {
        // The pack's handle is shared: positional reads on it are safe
        // from any thread (platform/file.h).
        out->file = (NphFile *)&m->pack.file;
        out->base_offset = entry->offset;
        out->size = entry->size;
        return NPH_RESULT_OK;
    }
    if (info.is_directory) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(nph_file_open(os_path, NPH_FILE_READ | NPH_FILE_ASYNC | extra_mode, &out->owned));
    const NphResult r = nph_file_size(&out->owned, &out->size);
    if (r != NPH_RESULT_OK) {
        nph_file_close(&out->owned);
        return r;
    }
    out->file = &out->owned;
    out->base_offset = 0U;
    return NPH_RESULT_OK;
}

void nph_vfs_close(const NphVfs *vfs, NphVfsFile *f)
{
    NPH_UNUSED(vfs);
    if (f == NULL) {
        return;
    }
    if (f->source == NPH_VFS_MOUNT_DIRECTORY) {
        nph_file_close(&f->owned);
    }
    memset(f, 0, sizeof *f);
}

NphResult nph_vfs_read_at(NphVfsFile *f, u64 offset, void *buf, u64 size, u64 *out_read)
{
    NPH_ASSERT(f != NULL && f->file != NULL && out_read != NULL);
    if (offset >= f->size) {
        *out_read = 0U;
        return NPH_RESULT_OK;
    }
    const u64 n = nph_min_u64(size, f->size - offset);
    return nph_file_read_at(f->file, f->base_offset + offset, buf, n, out_read);
}

NphResult nph_vfs_resolve_directory(const NphVfs *vfs, NphStr path, char *buf, u64 cap,
                                    u64 *out_len)
{
    NPH_ASSERT(vfs != NULL && buf != NULL);
    if (!nph_vfs_path_is_normalized(path)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = vfs->mount_count; i-- > 0U;) {
        const Mount *m = &vfs->mounts[i];
        NphStr rel;
        if (m->type != NPH_VFS_MOUNT_DIRECTORY || !mount_relative(m, path, &rel)) {
            continue;
        }
        return mount_os_path(m, rel, buf, cap, out_len);
    }
    return NPH_RESULT_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

typedef struct Walk {
    const NphVfs *vfs;
    const Mount *mount;
    NphStr prefix; // the caller's filter
    NphVfsVisitFn fn;
    void *user;
    NphHashMap visited; // set of path hashes
    char virtual_path[NPH_VFS_MAX_PATH + 1U];
    char os_path[VFS_OS_PATH_MAX];
    bool stopped;
    NphResult error;
} Walk;

// Reports one file unless an earlier (newer) mount already served it.
static NphResult walk_visit(Walk *w, NphStr path, u64 size)
{
    if (!nph_str_starts_with(path, w->prefix)) {
        return NPH_RESULT_OK;
    }
    const u64 key = nph_str_hash(path);
    if (nph_hashmap_contains_u64(&w->visited, key)) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(nph_hashmap_insert_u64(&w->visited, key, NULL));
    NphVfsInfo info;
    info.size = size;
    info.mount_id = w->mount->id;
    info.source = w->mount->type;
    info.is_directory = 0;
    if (!w->fn(path, &info, w->user)) {
        w->stopped = true;
    }
    return NPH_RESULT_OK;
}

// Recursively lists the directory at os_path (virtual_path is its virtual
// counterpart, "" or ending in '/'); both buffers are extended in place.
static void walk_directory(Walk *w, u64 os_len, u64 virtual_len, u32 depth)
{
    if (depth > VFS_MAX_WALK_DEPTH) {
        return;
    }
    NphDirIterator it;
    if (nph_dir_open(w->os_path, &it) != NPH_RESULT_OK) {
        return;
    }
    NphDirEntry e;
    while (!w->stopped && w->error == NPH_RESULT_OK && nph_dir_next(&it, &e)) {
        const u64 name_len = strlen(e.name);
        if (os_len + 1U + name_len + 1U > sizeof w->os_path ||
            virtual_len + name_len + 1U > sizeof w->virtual_path) {
            continue;
        }
        w->os_path[os_len] = '/';
        memcpy(w->os_path + os_len + 1U, e.name, name_len + 1U);
        memcpy(w->virtual_path + virtual_len, e.name, name_len + 1U);
        const u64 child_os_len = os_len + 1U + name_len;
        const u64 child_virtual_len = virtual_len + name_len;
        if (e.is_directory) {
            w->virtual_path[child_virtual_len] = '/';
            w->virtual_path[child_virtual_len + 1U] = '\0';
            walk_directory(w, child_os_len, child_virtual_len + 1U, depth + 1U);
        } else {
            NphFileInfo fi;
            if (nph_fs_stat(w->os_path, &fi) == NPH_RESULT_OK && fi.is_regular) {
                w->error = walk_visit(w, nph_str(w->virtual_path, child_virtual_len), fi.size);
            }
        }
        w->os_path[os_len] = '\0';
        w->virtual_path[virtual_len] = '\0';
    }
    nph_dir_close(&it);
}

NphResult nph_vfs_iterate(const NphVfs *vfs, NphStr prefix, NphVfsVisitFn fn, void *user)
{
    NPH_ASSERT(vfs != NULL && fn != NULL);
    Walk *w = NPH_ALLOC_TYPE(vfs->alloc, Walk);
    if (w == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    w->vfs = vfs;
    w->prefix = prefix;
    w->fn = fn;
    w->user = user;
    w->error = NPH_RESULT_OK;
    const NphHashMapDesc set_desc = nph_hashmap_desc_u64(0U);
    NphResult r = nph_hashmap_init(&w->visited, vfs->alloc, &set_desc);
    for (u32 i = vfs->mount_count; i-- > 0U && r == NPH_RESULT_OK && !w->stopped;) {
        const Mount *m = &vfs->mounts[i];
        w->mount = m;
        const NphStr mount_prefix = nph_str(m->prefix, m->prefix_len);
        if (m->type == NPH_VFS_MOUNT_PACK) {
            const u32 n = nph_pack_entry_count(&m->pack);
            for (u32 j = 0; j < n && r == NPH_RESULT_OK && !w->stopped; ++j) {
                const NphPackEntry *e = nph_pack_entry_at(&m->pack, j);
                const NphStr rel = nph_pack_entry_path(&m->pack, e);
                if (m->prefix_len + rel.len > NPH_VFS_MAX_PATH) {
                    continue;
                }
                memcpy(w->virtual_path, m->prefix, (size_t)m->prefix_len);
                memcpy(w->virtual_path + m->prefix_len, rel.data, (size_t)rel.len);
                r = walk_visit(w, nph_str(w->virtual_path, m->prefix_len + rel.len), e->size);
            }
            continue;
        }
        // Start the walk as deep as the filter allows: at the directory
        // part of the filter's remainder below the mount prefix.
        NphStr start_dir = nph_str_empty();
        if (nph_str_starts_with(prefix, mount_prefix)) {
            const NphStr rel = nph_str_slice(prefix, m->prefix_len, prefix.len);
            const u64 slash = nph_str_rfind_char(rel, '/');
            if (slash != NPH_STR_NPOS) {
                start_dir = nph_str_prefix(rel, slash); // without the slash
            }
        } else if (!nph_str_starts_with(mount_prefix, prefix)) {
            continue; // the filter and the mount cannot overlap
        }
        u64 os_len = 0U;
        if (mount_os_path(m, start_dir, w->os_path, sizeof w->os_path, &os_len) != NPH_RESULT_OK) {
            continue;
        }
        if (os_len > 0U && nph_path_is_separator(w->os_path[os_len - 1U])) {
            w->os_path[--os_len] = '\0'; // a root mount joined with ""
        }
        const u64 virtual_len = m->prefix_len + start_dir.len + (start_dir.len != 0U ? 1U : 0U);
        if (virtual_len >= sizeof w->virtual_path) {
            continue;
        }
        memcpy(w->virtual_path, m->prefix, (size_t)m->prefix_len);
        if (start_dir.len != 0U) {
            memcpy(w->virtual_path + m->prefix_len, start_dir.data, (size_t)start_dir.len);
            w->virtual_path[m->prefix_len + start_dir.len] = '/';
        }
        w->virtual_path[virtual_len] = '\0';
        walk_directory(w, os_len, virtual_len, 0U);
        r = w->error;
    }
    nph_hashmap_free(&w->visited);
    NPH_FREE_TYPE(vfs->alloc, Walk, w);
    return r;
}
