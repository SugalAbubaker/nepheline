// SPDX-License-Identifier: Apache-2.0
#pragma once

// The asset loader (CLAUDE.md §8.8; §10.5 "Asset loads are memory-mapped
// with no parsing step").
//
// A loader turns a virtual path into an NphAsset: the file is mapped
// through the VFS and validated (format.h); blobs stored uncompressed are
// used in place in the mapping, and compressed blobs are decompressed into
// one allocation. No other work happens at load time; the typed views
// (mesh.h, texture.h, ...) are pointers into that memory.
//
// Assets are identified by handle and shared: loading a path that is
// already loaded (or loading) returns the same handle with one more
// reference, and the asset is released when the last reference goes. The
// loader therefore keeps one copy of every asset, which is what the GUID
// references between assets rely on.
//
// Loads are synchronous (nph_asset_load) or, with a job system,
// asynchronous (nph_asset_load_async): the mapping and validation run on an
// I/O thread, each compressed blob is decompressed by its own job on the
// workers, and the handle becomes READY (or FAILED) when the last job
// finishes; a counter the caller passed is signalled then. A synchronous
// load with a job system still decompresses the blobs in parallel, waiting
// on the calling thread.
//
// Costs (§1.4): the loader allocates its slot tables at creation; a load
// allocates the asset record, the path copy, and the decompression block
// from the loader's allocator, and maps the file through the VFS. Blob
// checksums are verified in Debug and Development by default
// (NphAssetLoaderDesc.verify) at a cost of one CRC pass per blob.
//
// Thread-safety: NPH_THREADSAFE. One mutex guards the slot tables for the
// duration of a lookup or a state change (which is why even the queries
// take a non-const loader); the data of a READY asset is immutable and
// read without locks. Create and destroy are NPH_MAIN_THREAD_ONLY.

#include <nepheline/asset/format.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/jobs/jobs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAssetLoader NphAssetLoader;

typedef struct NphAssetHandle {
    u32 value;
} NphAssetHandle;

#define NPH_ASSET_HANDLE_NULL ((NphAssetHandle){0U})

NPH_INLINE bool nph_asset_handle_is_null(NphAssetHandle h)
{
    return h.value == 0U;
}

typedef enum NphAssetState {
    NPH_ASSET_STATE_NONE = 0, // not a live handle
    NPH_ASSET_STATE_LOADING,
    NPH_ASSET_STATE_READY,
    NPH_ASSET_STATE_FAILED,
} NphAssetState;

// Resolves an identity to a virtual path when no mounted pack indexes it:
// writes the path into `buf` (NUL terminated, at most `cap` bytes) and its
// length into *out_len. NPH_RESULT_NOT_FOUND when it does not know the
// GUID; NPH_RESULT_OVERFLOW when the path does not fit.
typedef NphResult (*NphAssetResolveFn)(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len);

typedef enum NphAssetVerify {
    NPH_ASSET_VERIFY_DEFAULT = 0, // checksums in Debug and Development builds
    NPH_ASSET_VERIFY_ALWAYS,
    NPH_ASSET_VERIFY_NEVER,
} NphAssetVerify;

typedef struct NphAssetLoaderDesc {
    NphAllocator *alloc; // required
    NphVfs *vfs;         // required
    NphJobSystem *jobs;  // optional: enables nph_asset_load_async and parallel decompression
    u32 max_assets;      // live handles (0: 4096)
    NphAssetVerify verify;
    u64 parallel_threshold; // bytes of compressed output from which a sync load uses jobs (0: 256
                            // KiB)
    // Optional: identities that no mounted pack indexes, such as the loose
    // cooked files of a project in development (the editor answers from the
    // cooker's database). Consulted when nph_vfs_find_guid reports
    // NOT_FOUND, on the thread that loads by GUID: it must be thread-safe
    // when loads by GUID are made from several threads.
    NphAssetResolveFn resolve;
    void *resolve_user;
} NphAssetLoaderDesc;

// A blob of a loaded asset: `data` is the raw bytes (in the mapping for
// stored blobs, in the asset's decompression block otherwise), aligned as
// the entry asked.
typedef struct NphAssetBlob {
    u32 tag;
    u32 index;
    u32 codec;
    u32 alignment;
    const void *data;
    u64 size;
} NphAssetBlob;

typedef struct NphAsset {
    NphAssetHeader header;
    const NphAssetBlob *blobs;
    u32 blob_count;
    NphStr path; // the virtual path it was loaded from
} NphAsset;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NPH_MUST_USE NphResult nph_asset_loader_create(const NphAssetLoaderDesc *desc,
                                               NphAssetLoader **out);

// Waits for loads in flight, releases every asset still live (logged as
// leaks with their paths), and frees the loader. Safe on NULL.
void nph_asset_loader_destroy(NphAssetLoader *loader);

// Live handles (assets loading, ready, or failed and still referenced).
u32 nph_asset_loader_live(const NphAssetLoader *loader);

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

// Loads `path` (normalised, vfs.h) and returns a handle holding one
// reference. When the asset is already loaded the existing handle is
// returned at once; when it is loading, the call waits for it. Errors:
// NPH_RESULT_NOT_FOUND (no such file), NPH_RESULT_CORRUPT and
// NPH_RESULT_VERSION_MISMATCH (format.h), NPH_RESULT_FULL (max_assets),
// NPH_RESULT_INVALID_ARGUMENT (a path that is not normalised),
// NPH_RESULT_OUT_OF_MEMORY. A failed load leaves no handle behind.
NPH_MUST_USE NphResult nph_asset_load(NphAssetLoader *loader, NphStr path, NphAssetHandle *out);

// Starts loading `path` and returns a handle at once, in LOADING (or READY
// when already loaded). `counter` (may be NULL) is incremented by one now
// and signalled when the load finishes, whichever way; nph_job_wait on it
// is the usual way to wait for a batch. The handle holds one reference;
// a load that fails keeps the handle in FAILED until it is released, so
// nph_asset_error can report why. NPH_RESULT_UNSUPPORTED without a job
// system, otherwise the errors of nph_asset_load that are known up front
// (FULL, INVALID_ARGUMENT, OUT_OF_MEMORY).
NPH_MUST_USE NphResult nph_asset_load_async(NphAssetLoader *loader, NphStr path,
                                            NphJobCounter *counter, NphAssetHandle *out);

// As nph_asset_load and nph_asset_load_async for the asset with identity
// `guid`, located through nph_vfs_find_guid and then the loader's resolver
// (NphAssetLoaderDesc.resolve): how a scene's references to meshes,
// materials, and textures are loaded. NPH_RESULT_NOT_FOUND when neither
// knows the GUID; otherwise the errors of the path load.
NPH_MUST_USE NphResult nph_asset_load_guid(NphAssetLoader *loader, NphGuid guid,
                                           NphAssetHandle *out);
NPH_MUST_USE NphResult nph_asset_load_guid_async(NphAssetLoader *loader, NphGuid guid,
                                                 NphJobCounter *counter, NphAssetHandle *out);

// Blocks (on a worker: parks the fiber) until the handle is no longer
// LOADING. The result of the load: NPH_RESULT_OK when READY.
NphResult nph_asset_wait(NphAssetLoader *loader, NphAssetHandle handle);

NphAssetState nph_asset_state(NphAssetLoader *loader, NphAssetHandle handle);

// Why a FAILED load failed (NPH_RESULT_OK for any other state).
NphResult nph_asset_error(NphAssetLoader *loader, NphAssetHandle handle);

// The asset behind a READY handle, or NULL. The pointer stays valid while
// the caller holds a reference.
const NphAsset *nph_asset_get(NphAssetLoader *loader, NphAssetHandle handle);

// Adds a reference. Asserts a live handle.
void nph_asset_acquire(NphAssetLoader *loader, NphAssetHandle handle);

// Drops a reference; the last one unloads the asset (after its load
// finishes, if it is still in flight). Ignores a null or stale handle.
void nph_asset_release(NphAssetLoader *loader, NphAssetHandle handle);

// The handle of the loaded asset with `guid` (no new reference), or null.
NphAssetHandle nph_asset_find_guid(NphAssetLoader *loader, NphGuid guid);

// Forgets the asset behind `handle` as the one at its path and with its
// GUID, because its file was replaced (a cooker wrote it again,
// format.h nph_asset_writer_write_file). From now on a load of the path or
// the GUID reads the file again into a new asset, while holders of this
// one keep it, valid, until they release it; a load in flight finishes as
// usual. A null or stale handle is ignored. NPH_THREADSAFE.
void nph_asset_forget(NphAssetLoader *loader, NphAssetHandle handle);

// The blob with `tag` and `index`, or NULL.
const NphAssetBlob *nph_asset_blob(const NphAsset *asset, u32 tag, u32 index);

#ifdef __cplusplus
}
#endif
