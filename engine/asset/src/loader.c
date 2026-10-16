// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/loader.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/handle.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/platform/thread.h>

#include <string.h>

#define LOADER_DEFAULT_MAX_ASSETS 4096U
#define LOADER_DEFAULT_PARALLEL   NPH_KIB(256)
#define LOADER_REUSE_DELAY        64U

// One unit of blob work: a decompression, or a checksum pass over a
// stored blob. Each becomes a job in an asynchronous or parallel load.
typedef struct Work {
    struct NphAssetLoader *loader;
    struct Slot *slot;
    u32 blob;
} Work;

typedef struct Slot {
    NphAtomicU32 state; // NphAssetState
    NphAtomicU32 refs;  // callers' references plus one held by a load in flight
    NphAtomicU32 error; // NphResult of the first failure
    NphHandle handle;
    char *path;
    u64 path_len;
    u64 path_hash;
    NphGuid guid; // in by_guid once READY
    NphAsset asset;
    NphAssetBlob *blobs;
    u32 blob_count;
    NphVfsMapping mapping;
    u8 *heap; // raw bytes of compressed blobs
    u64 heap_size;
    u32 heap_align;
    // The load in flight.
    NphJobCounter done;          // 1 while loading (job system only)
    NphJobCounter *user_counter; // signalled on finish (async loads)
    NphAtomicU32 remaining;      // work items outstanding
    Work *work;
    NphJobDecl *decls;
    u32 work_count;
    u64 work_bytes; // raw bytes the work produces or checks
} Slot;

struct NphAssetLoader {
    NphAllocator *alloc;
    NphVfs *vfs;
    NphJobSystem *jobs;
    NphMutex lock;
    NphCondVar changed; // a load finished (for waiters without a job system)
    NphHandlePool handles;
    Slot *slots;
    u32 max_assets;
    NphHashMap by_path; // u64 path hash -> u32 slot index
    NphHashMap by_guid; // NphGuid -> u32 slot index
    bool verify;
    u64 parallel_threshold;
    NphAssetResolveFn resolve;
    void *resolve_user;
    u32 in_flight; // loads whose pipeline may still touch the loader (under lock)
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult nph_asset_loader_create(const NphAssetLoaderDesc *desc, NphAssetLoader **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    if (desc->alloc == NULL || desc->vfs == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAllocator *alloc = desc->alloc;
    NphAssetLoader *l = NPH_ALLOC_TYPE(alloc, NphAssetLoader);
    if (l == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    l->alloc = alloc;
    l->vfs = desc->vfs;
    l->jobs = desc->jobs;
    l->max_assets = desc->max_assets != 0U ? desc->max_assets : LOADER_DEFAULT_MAX_ASSETS;
    l->parallel_threshold =
        desc->parallel_threshold != 0U ? desc->parallel_threshold : LOADER_DEFAULT_PARALLEL;
    l->resolve = desc->resolve;
    l->resolve_user = desc->resolve_user;
#if NPH_CONFIG_DEBUG || NPH_CONFIG_DEVELOPMENT
    const bool verify_default = true;
#else
    const bool verify_default = false;
#endif
    l->verify = desc->verify == NPH_ASSET_VERIFY_ALWAYS  ? true
                : desc->verify == NPH_ASSET_VERIFY_NEVER ? false
                                                         : verify_default;
    nph_mutex_init(&l->lock);
    nph_condvar_init(&l->changed);

    u32 index_bits = nph_log2_ceil_u32(l->max_assets);
    index_bits =
        nph_clamp_u32(index_bits, NPH_HANDLE_POOL_MIN_INDEX_BITS, NPH_HANDLE_POOL_MAX_INDEX_BITS);
    NphResult r =
        nph_handle_pool_init(&l->handles, alloc, l->max_assets, index_bits, LOADER_REUSE_DELAY);
    if (r == NPH_RESULT_OK) {
        l->slots = NPH_ALLOC_ARRAY_ZEROED(alloc, Slot, l->max_assets);
        if (l->slots == NULL) {
            r = NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    if (r == NPH_RESULT_OK) {
        const NphHashMapDesc pd = nph_hashmap_desc_u64(sizeof(u32));
        r = nph_hashmap_init(&l->by_path, alloc, &pd);
    }
    if (r == NPH_RESULT_OK) {
        NphHashMapDesc gd;
        memset(&gd, 0, sizeof gd);
        gd.key_size = sizeof(NphGuid);
        gd.val_size = sizeof(u32);
        r = nph_hashmap_init(&l->by_guid, alloc, &gd);
    }
    if (r != NPH_RESULT_OK) {
        nph_asset_loader_destroy(l);
        return r;
    }
    *out = l;
    return NPH_RESULT_OK;
}

static Slot *slot_of(const NphAssetLoader *l, NphAssetHandle h)
{
    const NphHandle nh = {h.value};
    if (!nph_handle_pool_is_valid(&l->handles, nh)) {
        return NULL;
    }
    return &l->slots[nph_handle_pool_index(&l->handles, nh)];
}

static NphAssetHandle handle_of(const Slot *s)
{
    const NphAssetHandle h = {s->handle.value};
    return h;
}

// Releases everything a slot owns and returns it to the pool. Under lock.
static void slot_free(NphAssetLoader *l, Slot *s)
{
    nph_hashmap_remove_u64(&l->by_path, s->path_hash);
    if (!nph_guid_is_null(s->guid)) {
        u32 *index = (u32 *)nph_hashmap_find(&l->by_guid, &s->guid);
        if (index != NULL && *index == (u32)(s - l->slots)) {
            nph_hashmap_remove(&l->by_guid, &s->guid);
        }
    }
    nph_vfs_unmap(l->vfs, &s->mapping);
    if (s->heap != NULL) {
        NPH_FREE(l->alloc, s->heap, s->heap_size, s->heap_align);
    }
    if (s->blobs != NULL) {
        NPH_FREE_ARRAY(l->alloc, NphAssetBlob, s->blobs, s->blob_count);
    }
    if (s->work != NULL) {
        NPH_FREE_ARRAY(l->alloc, Work, s->work, s->work_count);
        NPH_FREE_ARRAY(l->alloc, NphJobDecl, s->decls, s->work_count);
    }
    if (s->path != NULL) {
        nph_str_free(l->alloc, s->path, s->path_len);
    }
    nph_handle_pool_release(&l->handles, s->handle);
    memset(s, 0, sizeof *s);
}

void nph_asset_loader_destroy(NphAssetLoader *l)
{
    if (l == NULL) {
        return;
    }
    if (l->slots != NULL) {
        // Let every load in flight finish before touching anything.
        nph_mutex_lock(&l->lock);
        while (l->in_flight != 0U) {
            nph_condvar_wait(&l->changed, &l->lock);
        }
        for (u32 i = 0; i < l->max_assets; ++i) {
            Slot *s = &l->slots[i];
            if (nph_handle_is_null(s->handle)) {
                continue;
            }
            NPH_LOG_WARN(&NPH_LOG_ASSET, "asset leaked at loader destroy: %s (%u references)",
                         s->path, nph_atomic_load_u32(&s->refs, NPH_MEMORY_ORDER_RELAXED));
            slot_free(l, s);
        }
        nph_mutex_unlock(&l->lock);
        NPH_FREE_ARRAY(l->alloc, Slot, l->slots, l->max_assets);
    }
    nph_hashmap_free(&l->by_path);
    nph_hashmap_free(&l->by_guid);
    nph_handle_pool_free(&l->handles);
    nph_condvar_destroy(&l->changed);
    nph_mutex_destroy(&l->lock);
    NPH_FREE_TYPE(l->alloc, NphAssetLoader, l);
}

u32 nph_asset_loader_live(const NphAssetLoader *l)
{
    NPH_ASSERT(l != NULL);
    return nph_handle_pool_live(&l->handles);
}

// ---------------------------------------------------------------------------
// The load pipeline
// ---------------------------------------------------------------------------

static void slot_set_error(Slot *s, NphResult r)
{
    u32 expected = (u32)NPH_RESULT_OK;
    (void)nph_atomic_compare_exchange_u32(&s->error, &expected, (u32)r, NPH_MEMORY_ORDER_RELAXED,
                                          NPH_MEMORY_ORDER_RELAXED);
}

// Maps the file, validates it, and lays out the blobs and the work list.
// Runs once per load, on the loading thread (an I/O thread for
// asynchronous loads).
static NphResult load_prepare(NphAssetLoader *l, Slot *s)
{
    NPH_TRY(nph_vfs_map(l->vfs, nph_str(s->path, s->path_len), &s->mapping));
    NphAssetFileInfo info;
    NPH_TRY(nph_asset_file_validate(s->mapping.data, s->mapping.size, &info));
    s->asset.header = info.header;
    s->blob_count = info.blob_count;
    if (info.blob_count != 0U) {
        s->blobs = NPH_ALLOC_ARRAY_ZEROED(l->alloc, NphAssetBlob, info.blob_count);
        if (s->blobs == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    // Heap layout for compressed blobs, and the work list.
    u64 heap = 0U;
    u32 heap_align = NPH_ASSET_BLOB_DEFAULT_ALIGN;
    u32 work_count = 0U;
    u64 work_bytes = 0U;
    for (u32 i = 0; i < info.blob_count; ++i) {
        const NphAssetBlobEntry *e = &info.blobs[i];
        NphAssetBlob *b = &s->blobs[i];
        b->tag = e->tag;
        b->index = e->index;
        b->codec = e->codec;
        b->alignment = e->alignment;
        b->size = e->raw_size;
        if (e->codec == NPH_ASSET_CODEC_NONE) {
            b->data = nph_asset_file_blob(s->mapping.data, e);
            if (l->verify) {
                ++work_count;
                work_bytes += e->stored_size;
            }
        } else {
            heap = nph_align_up_u64(heap, e->alignment) + e->raw_size;
            heap_align = nph_max_u32(heap_align, e->alignment);
            ++work_count;
            work_bytes += e->raw_size;
        }
    }
    if (heap != 0U) {
        s->heap = (u8 *)NPH_ALLOC(l->alloc, heap, heap_align);
        if (s->heap == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        s->heap_size = heap;
        s->heap_align = heap_align;
        // The same walk as above assigns each compressed blob its place.
        u64 offset = 0U;
        for (u32 i = 0; i < info.blob_count; ++i) {
            if (s->blobs[i].codec != NPH_ASSET_CODEC_NONE) {
                offset = nph_align_up_u64(offset, s->blobs[i].alignment);
                s->blobs[i].data = s->heap + offset;
                offset += s->blobs[i].size;
            }
        }
    }
    if (work_count != 0U) {
        s->work = NPH_ALLOC_ARRAY_ZEROED(l->alloc, Work, work_count);
        s->decls = NPH_ALLOC_ARRAY_ZEROED(l->alloc, NphJobDecl, work_count);
        if (s->work == NULL || s->decls == NULL) {
            if (s->work != NULL) {
                NPH_FREE_ARRAY(l->alloc, Work, s->work, work_count);
                s->work = NULL;
            }
            if (s->decls != NULL) {
                NPH_FREE_ARRAY(l->alloc, NphJobDecl, s->decls, work_count);
                s->decls = NULL;
            }
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        s->work_count = work_count;
        u32 w = 0U;
        for (u32 i = 0; i < info.blob_count; ++i) {
            if (s->blobs[i].codec != NPH_ASSET_CODEC_NONE || l->verify) {
                s->work[w].loader = l;
                s->work[w].slot = s;
                s->work[w].blob = i;
                ++w;
            }
        }
    }
    s->work_bytes = work_bytes;
    s->asset.blobs = s->blobs;
    s->asset.blob_count = s->blob_count;
    s->asset.path = nph_str(s->path, s->path_len);
    return NPH_RESULT_OK;
}

// One work item: verify and decode a blob.
static NphResult work_run(const Work *w)
{
    const Slot *s = w->slot;
    // The table was validated by load_prepare; re-deriving it is cheaper
    // than storing a pointer per blob and keeps the work item tiny.
    const NphAssetBlobEntry *entries =
        (const NphAssetBlobEntry *)((const u8 *)s->mapping.data + NPH_ASSET_HEADER_SIZE);
    const NphAssetBlobEntry *e = &entries[w->blob];
    if (w->loader->verify && !nph_asset_file_verify_blob(s->mapping.data, e)) {
        char tag[5];
        nph_fourcc_to_string(e->tag, tag);
        NPH_LOG_ERROR(&NPH_LOG_ASSET, "%s: blob %s[%u] failed its checksum", s->path, tag,
                      e->index);
        return NPH_RESULT_CORRUPT;
    }
    if (e->codec == NPH_ASSET_CODEC_NONE) {
        return NPH_RESULT_OK;
    }
    // The blob's raw bytes live in the slot's heap (load_prepare placed
    // them there); the public view holds a const pointer to the same place.
    u8 *dst = s->heap + nph_ptr_diff(s->heap, s->blobs[w->blob].data);
    const NphResult r = nph_asset_file_unpack_blob(s->mapping.data, e, dst);
    if (r != NPH_RESULT_OK) {
        char tag[5];
        nph_fourcc_to_string(e->tag, tag);
        NPH_LOG_ERROR(&NPH_LOG_ASSET, "%s: blob %s[%u] does not decompress: %s", s->path, tag,
                      e->index, nph_result_string(r));
    }
    return r;
}

// Completes a load: publishes the state, wakes waiters, and drops the
// pipeline's reference. The last thing a pipeline does.
static void load_finish(NphAssetLoader *l, Slot *s)
{
    const NphResult r = (NphResult)nph_atomic_load_u32(&s->error, NPH_MEMORY_ORDER_RELAXED);
    nph_mutex_lock(&l->lock);
    if (r == NPH_RESULT_OK) {
        s->guid = s->asset.header.guid;
        const u32 index = (u32)(s - l->slots);
        if (!nph_guid_is_null(s->guid) && !nph_hashmap_contains(&l->by_guid, &s->guid)) {
            NPH_IGNORE_RESULT(nph_hashmap_insert(&l->by_guid, &s->guid, &index));
        }
        nph_atomic_store_u32(&s->state, (u32)NPH_ASSET_STATE_READY, NPH_MEMORY_ORDER_RELEASE);
    } else {
        // Keep the path (for nph_asset_error's callers) and release the rest.
        nph_vfs_unmap(l->vfs, &s->mapping);
        if (s->heap != NULL) {
            NPH_FREE(l->alloc, s->heap, s->heap_size, s->heap_align);
            s->heap = NULL;
        }
        if (s->blobs != NULL) {
            NPH_FREE_ARRAY(l->alloc, NphAssetBlob, s->blobs, s->blob_count);
            s->blobs = NULL;
        }
        s->asset.blobs = NULL;
        s->asset.blob_count = 0U;
        nph_atomic_store_u32(&s->state, (u32)NPH_ASSET_STATE_FAILED, NPH_MEMORY_ORDER_RELEASE);
    }
    if (s->work != NULL) {
        NPH_FREE_ARRAY(l->alloc, Work, s->work, s->work_count);
        NPH_FREE_ARRAY(l->alloc, NphJobDecl, s->decls, s->work_count);
        s->work = NULL;
        s->decls = NULL;
        s->work_count = 0U;
    }
    NphJobCounter *user_counter = s->user_counter;
    s->user_counter = NULL;
    // Waiters: the job counter (parked fibers, blocked threads) and the
    // condition variable (threads without a job system).
    if (l->jobs != NULL) {
        nph_job_counter_signal(l->jobs, &s->done);
    }
    if (user_counter != NULL) {
        nph_job_counter_signal(l->jobs, user_counter);
    }
    // The pipeline's own reference.
    if (nph_atomic_fetch_sub_u32(&s->refs, 1U, NPH_MEMORY_ORDER_ACQ_REL) == 1U) {
        slot_free(l, s);
    }
    l->in_flight -= 1U;
    nph_condvar_broadcast(&l->changed);
    nph_mutex_unlock(&l->lock);
}

static void work_job(void *user)
{
    Work *w = (Work *)user;
    const NphResult r = work_run(w);
    if (r != NPH_RESULT_OK) {
        slot_set_error(w->slot, r);
    }
    if (nph_atomic_fetch_sub_u32(&w->slot->remaining, 1U, NPH_MEMORY_ORDER_ACQ_REL) == 1U) {
        load_finish(w->loader, w->slot);
    }
}

static void run_work_inline(Slot *s)
{
    for (u32 i = 0; i < s->work_count; ++i) {
        const NphResult r = work_run(&s->work[i]);
        if (r != NPH_RESULT_OK) {
            slot_set_error(s, r);
        }
    }
}

// ---------------------------------------------------------------------------
// Slot allocation and lookup (under lock)
// ---------------------------------------------------------------------------

// Finds the slot holding `path`, or NULL.
static Slot *find_path(NphAssetLoader *l, NphStr path, u64 hash)
{
    const u32 *index = (const u32 *)nph_hashmap_find_u64(&l->by_path, hash);
    if (index == NULL) {
        return NULL;
    }
    Slot *s = &l->slots[*index];
    if (s->path_len != path.len || memcmp(s->path, path.data, (size_t)path.len) != 0) {
        return NULL; // a 64-bit collision between different paths
    }
    return s;
}

// Creates a LOADING slot for `path` holding the caller's reference and
// the pipeline's.
static NphResult slot_new(NphAssetLoader *l, NphStr path, u64 hash, Slot **out)
{
    if (nph_hashmap_find_u64(&l->by_path, hash) != NULL) {
        return NPH_RESULT_ALREADY_EXISTS; // the collision case of find_path
    }
    NphHandle h;
    NPH_TRY(nph_handle_pool_alloc(&l->handles, &h));
    Slot *s = &l->slots[nph_handle_pool_index(&l->handles, h)];
    memset(s, 0, sizeof *s);
    s->handle = h;
    NphResult r = nph_str_dup(l->alloc, path, &s->path);
    if (r == NPH_RESULT_OK) {
        s->path_len = path.len;
        s->path_hash = hash;
        const u32 index = (u32)(s - l->slots);
        r = nph_hashmap_insert_u64(&l->by_path, hash, &index);
    }
    if (r != NPH_RESULT_OK) {
        if (s->path != NULL) {
            nph_str_free(l->alloc, s->path, path.len);
        }
        nph_handle_pool_release(&l->handles, h);
        memset(s, 0, sizeof *s);
        return r;
    }
    nph_atomic_store_u32(&s->state, (u32)NPH_ASSET_STATE_LOADING, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&s->refs, 2U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&s->error, (u32)NPH_RESULT_OK, NPH_MEMORY_ORDER_RELAXED);
    nph_job_counter_init(&s->done);
    if (l->jobs != NULL) {
        nph_job_counter_add(&s->done, 1U);
    }
    l->in_flight += 1U;
    *out = s;
    return NPH_RESULT_OK;
}

// Waits until `s` leaves LOADING. The caller holds a reference.
static void slot_wait(NphAssetLoader *l, Slot *s)
{
    if (l->jobs != NULL) {
        nph_job_wait(l->jobs, &s->done);
        return;
    }
    nph_mutex_lock(&l->lock);
    while (nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE) ==
           (u32)NPH_ASSET_STATE_LOADING) {
        nph_condvar_wait(&l->changed, &l->lock);
    }
    nph_mutex_unlock(&l->lock);
}

static NphResult slot_result(const Slot *s)
{
    const u32 state = nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE);
    if (state == (u32)NPH_ASSET_STATE_READY) {
        return NPH_RESULT_OK;
    }
    const NphResult r = (NphResult)nph_atomic_load_u32(&s->error, NPH_MEMORY_ORDER_RELAXED);
    return r != NPH_RESULT_OK ? r : NPH_RESULT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Synchronous loads
// ---------------------------------------------------------------------------

// Runs the whole pipeline on the calling thread, decompressing in
// parallel on the job system when the work is large enough.
static void load_sync_pipeline(NphAssetLoader *l, Slot *s)
{
    const NphResult r = load_prepare(l, s);
    if (r != NPH_RESULT_OK) {
        slot_set_error(s, r);
    } else if (s->work_count != 0U) {
        bool parallel = false;
        if (l->jobs != NULL && s->work_count >= 2U && s->work_bytes >= l->parallel_threshold) {
            for (u32 i = 0; i < s->work_count; ++i) {
                s->decls[i].fn = work_job;
                s->decls[i].user = &s->work[i];
                s->decls[i].name = "asset blob";
            }
            // The counter counts the jobs; `remaining` is not used on this
            // path, so the jobs' own finish never fires: set it above the
            // job count.
            nph_atomic_store_u32(&s->remaining, s->work_count + 1U, NPH_MEMORY_ORDER_RELAXED);
            NphJobCounter counter = NPH_JOB_COUNTER_INIT;
            if (nph_job_run(l->jobs, s->decls, s->work_count, NPH_JOB_PRIORITY_NORMAL, &counter) ==
                NPH_RESULT_OK) {
                nph_job_wait(l->jobs, &counter);
                parallel = true;
            }
        }
        if (!parallel) {
            run_work_inline(s);
        }
    }
    load_finish(l, s);
}

NphResult nph_asset_load(NphAssetLoader *l, NphStr path, NphAssetHandle *out)
{
    NPH_ASSERT(l != NULL && out != NULL);
    *out = NPH_ASSET_HANDLE_NULL;
    if (!nph_vfs_path_is_normalized(path)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 hash = nph_str_hash(path);
    nph_mutex_lock(&l->lock);
    Slot *s = find_path(l, path, hash);
    bool owner = false;
    NphResult r = NPH_RESULT_OK;
    if (s != NULL) {
        nph_atomic_fetch_add_u32(&s->refs, 1U, NPH_MEMORY_ORDER_RELAXED);
    } else {
        r = slot_new(l, path, hash, &s);
        owner = true;
    }
    nph_mutex_unlock(&l->lock);
    if (r != NPH_RESULT_OK) {
        return r;
    }
    if (owner) {
        load_sync_pipeline(l, s);
    } else if (nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE) ==
               (u32)NPH_ASSET_STATE_LOADING) {
        slot_wait(l, s);
    }
    r = slot_result(s);
    if (r != NPH_RESULT_OK) {
        nph_asset_release(l, handle_of(s));
        return r;
    }
    *out = handle_of(s);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Asynchronous loads
// ---------------------------------------------------------------------------

typedef struct IoStage {
    NphAssetLoader *loader;
    Slot *slot;
} IoStage;

static void io_stage_job(void *user)
{
    IoStage *stage = (IoStage *)user;
    NphAssetLoader *l = stage->loader;
    Slot *s = stage->slot;
    NPH_FREE_TYPE(l->alloc, IoStage, stage);

    const NphResult r = load_prepare(l, s);
    if (r != NPH_RESULT_OK) {
        slot_set_error(s, r);
        load_finish(l, s);
        return;
    }
    if (s->work_count == 0U) {
        load_finish(l, s);
        return;
    }
    for (u32 i = 0; i < s->work_count; ++i) {
        s->decls[i].fn = work_job;
        s->decls[i].user = &s->work[i];
        s->decls[i].name = "asset blob";
    }
    nph_atomic_store_u32(&s->remaining, s->work_count, NPH_MEMORY_ORDER_RELEASE);
    if (nph_job_run(l->jobs, s->decls, s->work_count, NPH_JOB_PRIORITY_LOW, NULL) !=
        NPH_RESULT_OK) {
        // The job pool is full: do the work here rather than fail the load.
        run_work_inline(s);
        load_finish(l, s);
    }
}

// A job that waits for a load already in flight on behalf of a second
// asynchronous request, so that request's counter is signalled too.
static void wait_stage_job(void *user)
{
    IoStage *stage = (IoStage *)user;
    nph_job_wait(stage->loader->jobs, &stage->slot->done);
    NPH_FREE_TYPE(stage->loader->alloc, IoStage, stage);
}

// A GUID load is a path load once the VFS has named the path: the slot
// tables deduplicate by path, so an asset already loaded (or loading) by
// either route is shared, and there is no separate GUID-keyed state to
// keep consistent.
// The virtual path of `guid`: from a pack's index, else from the resolver.
static NphResult guid_path(NphAssetLoader *l, NphGuid guid, char *path, u64 cap, u64 *len)
{
    const NphResult r = nph_vfs_find_guid(l->vfs, guid, path, cap, len);
    if (r == NPH_RESULT_NOT_FOUND && l->resolve != NULL) {
        return l->resolve(l->resolve_user, guid, path, cap, len);
    }
    return r;
}

NphResult nph_asset_load_guid(NphAssetLoader *l, NphGuid guid, NphAssetHandle *out)
{
    NPH_ASSERT(l != NULL && out != NULL);
    *out = NPH_ASSET_HANDLE_NULL;
    char path[NPH_VFS_MAX_PATH + 1U];
    u64 len = 0U;
    NPH_TRY(guid_path(l, guid, path, sizeof path, &len));
    return nph_asset_load(l, nph_str(path, len), out);
}

NphResult nph_asset_load_guid_async(NphAssetLoader *l, NphGuid guid, NphJobCounter *counter,
                                    NphAssetHandle *out)
{
    NPH_ASSERT(l != NULL && out != NULL);
    *out = NPH_ASSET_HANDLE_NULL;
    char path[NPH_VFS_MAX_PATH + 1U];
    u64 len = 0U;
    NPH_TRY(guid_path(l, guid, path, sizeof path, &len));
    return nph_asset_load_async(l, nph_str(path, len), counter, out);
}

NphResult nph_asset_load_async(NphAssetLoader *l, NphStr path, NphJobCounter *counter,
                               NphAssetHandle *out)
{
    NPH_ASSERT(l != NULL && out != NULL);
    *out = NPH_ASSET_HANDLE_NULL;
    if (l->jobs == NULL) {
        return NPH_RESULT_UNSUPPORTED;
    }
    if (!nph_vfs_path_is_normalized(path)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    IoStage *stage = NPH_ALLOC_TYPE(l->alloc, IoStage);
    if (stage == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    stage->loader = l;

    const u64 hash = nph_str_hash(path);
    nph_mutex_lock(&l->lock);
    Slot *s = find_path(l, path, hash);
    NphResult r = NPH_RESULT_OK;
    if (s != NULL) {
        nph_atomic_fetch_add_u32(&s->refs, 1U, NPH_MEMORY_ORDER_RELAXED);
        nph_mutex_unlock(&l->lock);
        stage->slot = s;
        if (counter != NULL && nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE) ==
                                   (u32)NPH_ASSET_STATE_LOADING) {
            // Someone else's load is in flight: a job waits on it for us.
            const NphJobDecl decl = {wait_stage_job, stage, "asset wait"};
            r = nph_job_run(l->jobs, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, counter);
            if (r != NPH_RESULT_OK) {
                nph_asset_release(l, handle_of(s));
                NPH_FREE_TYPE(l->alloc, IoStage, stage);
                return r;
            }
        } else {
            NPH_FREE_TYPE(l->alloc, IoStage, stage);
        }
        *out = handle_of(s);
        return NPH_RESULT_OK;
    }
    r = slot_new(l, path, hash, &s);
    if (r == NPH_RESULT_OK) {
        s->user_counter = counter;
        if (counter != NULL) {
            nph_job_counter_add(counter, 1U);
        }
        stage->slot = s;
        const NphJobDecl decl = {io_stage_job, stage, "asset load"};
        r = nph_job_run_io(l->jobs, &decl, 1U, NULL);
        if (r != NPH_RESULT_OK) {
            // Undo: nobody else can see the slot yet except through the
            // path map, which the lock still guards.
            if (counter != NULL) {
                nph_job_counter_signal(l->jobs, counter);
            }
            nph_job_counter_signal(l->jobs, &s->done);
            l->in_flight -= 1U;
            slot_free(l, s);
        }
    }
    nph_mutex_unlock(&l->lock);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(l->alloc, IoStage, stage);
        return r;
    }
    *out = handle_of(s);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

NphResult nph_asset_wait(NphAssetLoader *l, NphAssetHandle handle)
{
    NPH_ASSERT(l != NULL);
    nph_mutex_lock(&l->lock);
    Slot *s = slot_of(l, handle);
    nph_mutex_unlock(&l->lock);
    if (s == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE) == (u32)NPH_ASSET_STATE_LOADING) {
        slot_wait(l, s);
    }
    return slot_result(s);
}

NphAssetState nph_asset_state(NphAssetLoader *l, NphAssetHandle handle)
{
    NPH_ASSERT(l != NULL);
    nph_mutex_lock(&l->lock);
    const Slot *s = slot_of(l, handle);
    const u32 state = s != NULL ? nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE)
                                : (u32)NPH_ASSET_STATE_NONE;
    nph_mutex_unlock(&l->lock);
    return (NphAssetState)state;
}

NphResult nph_asset_error(NphAssetLoader *l, NphAssetHandle handle)
{
    NPH_ASSERT(l != NULL);
    nph_mutex_lock(&l->lock);
    const Slot *s = slot_of(l, handle);
    NphResult r = NPH_RESULT_OK;
    if (s != NULL &&
        nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE) == (u32)NPH_ASSET_STATE_FAILED) {
        r = slot_result(s);
    }
    nph_mutex_unlock(&l->lock);
    return r;
}

const NphAsset *nph_asset_get(NphAssetLoader *l, NphAssetHandle handle)
{
    NPH_ASSERT(l != NULL);
    nph_mutex_lock(&l->lock);
    const Slot *s = slot_of(l, handle);
    const NphAsset *asset = NULL;
    if (s != NULL &&
        nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE) == (u32)NPH_ASSET_STATE_READY) {
        asset = &s->asset;
    }
    nph_mutex_unlock(&l->lock);
    return asset;
}

void nph_asset_acquire(NphAssetLoader *l, NphAssetHandle handle)
{
    NPH_ASSERT(l != NULL);
    nph_mutex_lock(&l->lock);
    Slot *s = slot_of(l, handle);
    NPH_ASSERT_MSG(s != NULL, "nph_asset_acquire on a stale or null handle");
    if (s != NULL) {
        nph_atomic_fetch_add_u32(&s->refs, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    nph_mutex_unlock(&l->lock);
}

void nph_asset_release(NphAssetLoader *l, NphAssetHandle handle)
{
    NPH_ASSERT(l != NULL);
    nph_mutex_lock(&l->lock);
    Slot *s = slot_of(l, handle);
    if (s != NULL) {
        if (nph_atomic_fetch_sub_u32(&s->refs, 1U, NPH_MEMORY_ORDER_ACQ_REL) == 1U) {
            NPH_ASSERT(nph_atomic_load_u32(&s->state, NPH_MEMORY_ORDER_ACQUIRE) !=
                       (u32)NPH_ASSET_STATE_LOADING);
            slot_free(l, s);
        }
    }
    nph_mutex_unlock(&l->lock);
}

NphAssetHandle nph_asset_find_guid(NphAssetLoader *l, NphGuid guid)
{
    NPH_ASSERT(l != NULL);
    NphAssetHandle h = NPH_ASSET_HANDLE_NULL;
    nph_mutex_lock(&l->lock);
    const u32 *index = (const u32 *)nph_hashmap_find(&l->by_guid, &guid);
    if (index != NULL) {
        h = handle_of(&l->slots[*index]);
    }
    nph_mutex_unlock(&l->lock);
    return h;
}

const NphAssetBlob *nph_asset_blob(const NphAsset *asset, u32 tag, u32 index)
{
    NPH_ASSERT(asset != NULL);
    for (u32 i = 0; i < asset->blob_count; ++i) {
        if (asset->blobs[i].tag == tag && asset->blobs[i].index == index) {
            return &asset->blobs[i];
        }
    }
    return NULL;
}
