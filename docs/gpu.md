# GPU module (`engine/gpu`)

The Vulkan RHI (CLAUDE.md §8.5): a thin, Vulkan-specific layer that
removes boilerplate and enforces correctness. It is not cross-API. Decisions:
[ADR-0014](adr/0014-gpu-module.md). Depends on `jobs` (parallel command
recording) and, through it, on `platform` (window surfaces, files,
threads), `math`, and `core`.

| Header | Contents |
|---|---|
| `types.h` | handles, formats, resource descriptions, the barrier vocabulary (stages, access, layouts), attachments and clears |
| `device.h` | device creation and selection, features and limits, the validation gate and leak checker, frames in flight, timelines, submission |
| `resources.h` | buffers (with device addresses), images (with bindless slots), samplers |
| `commands.h` | per-thread command buffers, barriers, dynamic rendering, draws, dispatches, transfers, labels, timestamps |
| `swapchain.h` | the swapchain for a window: acquire, present, resize, vsync |
| `upload.h` | the staging ring and transfer-queue uploads |
| `shader.h` | SPIR-V reflection and shader modules with hot reload |
| `pipeline.h` | graphics and compute pipelines, the pipeline cache file |
| `render_graph.h` | passes, declared resource uses, culling, barriers, aliasing, async compute, parallel recording, timings |
| `vk.h` | the escape hatch exposing Vulkan handles (editor backend, gpu tests, GPU benchmarks only) |

Third-party code behind the module (CLAUDE.md §3): the Vulkan headers,
volk (function loading), and the Vulkan Memory Allocator (one C++
translation unit, `src/gpu_vma.cpp`, behind the C interface
`src/gpu_alloc.h`). No other engine module includes a Vulkan header;
`scripts/check_layering.py` enforces it in CI.

## Baseline and device selection

The device requires Vulkan 1.3 with dynamic rendering, synchronization2,
timeline semaphores, buffer device address, descriptor indexing with
update-after-bind and partially bound descriptors, draw indirect count,
host query reset, scalar block layout, format-less storage images, and 128
bytes of push constants. Optional capabilities are detected and enabled
when present and reported in `NphGpuFeatures`: mesh shaders, ray queries,
64-bit image atomics (the software rasteriser of §8.6.4), min/max
reduction samplers (Hi-Z), 16-bit floats and 8-bit integers, depth clamp,
wireframe, BC compression, memory budgets, calibrated timestamps.

Physical devices are scored (discrete > integrated > virtual > CPU, then
device-local memory, then optional features) and every candidate is logged
with its score or the reason it was rejected. `device_index_plus_one`
forces a device. A machine without a loader or a conforming device gets
`NPH_RESULT_UNSUPPORTED`, which the tests and samples turn into a skip;
lavapipe (Mesa's software driver) is a conforming device and runs the whole
suite in CI.

Three queues: GRAPHICS (also compute, transfer, present), COMPUTE (a
dedicated family when the hardware has one, otherwise a second queue of the
graphics family, otherwise the graphics queue itself) and TRANSFER (a
transfer-only family when present, otherwise the graphics queue). The
queue table may alias; `NphGpuFeatures.async_compute` and
`dedicated_transfer` say whether the queues are distinct.

## Validation gate

Debug and Development builds enable the Khronos validation layer with
synchronisation validation (`VK_EXT_validation_features`) and route every
message to the `gpu` log category; errors are also written in full to
stderr, because validation messages run to a kilobyte and the log record
keeps 216 bytes. `nph_gpu_device_validation_stats` counts errors, warnings,
and performance warnings; the test fixture asserts `errors == 0` on
teardown and the GPU samples exit with status 1 when any error was
reported (CLAUDE.md §1.3, §8.5). The loader's and the layer's own status
chatter is traced and never counted. Every object the module creates gets a
debug name (`VK_EXT_debug_utils`), so a message names the resource.

## Resources and bindless access

Handles are 32-bit generational values from `core/handle.h`; a stale handle
asserts in development builds and returns the "null" answer in shipping
builds. Every buffer has a device address. Every sampled image has one slot
in the global descriptor set (binding 0, the whole image; depth images
expose their depth aspect), every storage image one slot per mip (binding
1), every sampler one slot (binding 2). Shaders receive indices and
addresses through 128 bytes of push constants and never own a descriptor
set; `shaders/shared/nph_bindless.glsl` declares the arrays and
`shaders/shared/nph_shared.h` the structs shared between C and GLSL.

Destruction is deferred: the handle dies at once, the Vulkan objects and
the bindless slot are released when the frame being recorded completes (or
at `nph_gpu_device_wait_idle`), so a slot never changes under a shader in
flight. At device destruction every live handle is reported as a leak with
its name and then released.

Sharing: textures (transfer destinations that are never attachments) are
shared across queue families so uploads need no ownership transfer and may
touch a subset of layers; attachments are exclusive, which keeps the
hardware's framebuffer compression (CLAUDE.md §10.3.5), and move between
families with release/acquire pairs that the render graph and the upload
path generate. Buffers are always shared. Layouts are tracked per mip.

## Frames, commands, submission

`nph_gpu_frame_begin` / `nph_gpu_frame_end` bound the CPU to
`NPH_GPU_FRAMES_IN_FLIGHT` (2) frames ahead of the GPU (§10.6). Beginning a
frame waits on the queue timelines for the frame that last used the slot,
then recycles that slot's command pools, staging memory, timestamp
results, and deferred destructions. Command pools are per thread, per
frame slot, per queue family: any thread, including a job worker, begins
its own command buffers and nothing locks. A submit signals its queue's
timeline with the next value and may wait on and signal any timeline;
swapchain binary semaphores are handled by naming the swapchain in the
submit. Submissions outside frames (tests, tools) are followed by a
timeline wait or `nph_gpu_device_wait_idle`, which also resets the pools.

Timestamps: `nph_gpu_cmd_write_timestamp` writes into the frame slot's
query pool; results are read when the slot is recycled two frames later
(`nph_gpu_frame_timestamps`), which is how the render graph reports
per-pass GPU time.

## Uploads

`nph_gpu_upload_buffer` / `nph_gpu_upload_image` copy into a persistently
mapped staging ring (64 MiB by default; larger uploads get a dedicated
buffer that dies with the frame) and record the copy into the frame's
transfer command buffer. `nph_gpu_upload_flush` submits it on the
transfer queue, waiting for the previous frame's graphics work so an
upload into a resource that frame read cannot race it, then submits a
small graphics-queue command buffer that waits for the transfer and
acquires any exclusive images; every later graphics submit is ordered
after it (the semaphore wait's second scope includes later submissions).
Uploaded mips end in SHADER_READ_ONLY (sampled images) or GENERAL. Ring
space is released when the frame that allocated it completes; outside a
frame a flush is synchronous.

## Shaders and pipelines

`nph_gpu_spirv_reflect` is the engine's own SPIR-V parser: two passes over
the instruction stream with every word bounds-checked, fuzzed by
`tests/fuzz/fuzz_spirv.c`. It reports the stage and entry point, the push
constant block size (computed from Offset, ArrayStride, and MatrixStride
decorations), the workgroup size (including `LocalSizeId`), the bindless
bindings used, and the fragment outputs, and rejects anything outside the
layout: descriptors beyond set 0 bindings 0 to 2, combined samplers,
uniform or storage buffer descriptors (buffers are reached through
addresses), vertex input attributes (vertex data is pulled), and push
constant blocks over 128 bytes.

Every pipeline uses the one global layout, so a graphics description is
the stages, the fixed-function state the engine uses, and the attachment
formats; viewport and scissor are dynamic. The pipeline cache is persisted
to the path in the device description, in a file with the engine's own
header (magic, version, size, CRC32C) around the driver's blob whose
`VkPipelineCacheHeaderVersionOne` is checked against the device before the
driver sees it; writes go through a temporary file and a rename.

Hot reload: `nph_gpu_shader_reload` swaps the module behind a handle,
`nph_gpu_pipeline_reload` rebuilds a pipeline from its stored description;
the old objects retire with the frame and the handles never change.
`samples/gpu_mesh` polls the compiled `.spv` files and reloads them.

## Render graph

The graph follows O'Donnell, "FrameGraph: Extensible Rendering
Architecture in Frostbite" (GDC 2017). Per frame the renderer declares
transient resources, imports persistent ones (with their tracked layouts
as the starting state), adds passes with their reads, writes, and
attachments, optionally names a swapchain image to present, then compiles
and executes:

1. **Culling** by reference counting: a pass survives if it writes a
   resource that a surviving pass reads, writes an imported resource, or
   is flagged `NPH_GPU_RG_PASS_SIDE_EFFECTS`.
2. **Scheduling**: declaration order; compute passes flagged
   `ASYNC_COMPUTE` go to the compute queue when it is distinct. Consecutive
   passes on one queue form a batch (one submit); a batch waits on the
   batch that last touched any of its resources on another queue, on
   everything an outside queue was given before the frame for imported
   resources entering from that queue, and, for compute batches, on the
   frame's uploads.
3. **Transient placement**: memory requirements come from
   `vkGetDeviceImageMemoryRequirements` without creating objects; images
   and buffers are packed, size-descending, into blocks by first-fit over
   lifetimes, so resources whose lifetimes do not overlap share memory.
   Blocks are kept across frames; a transient whose description, block,
   and offset repeat is reused from a cache, so a steady-state frame
   creates nothing.
4. **Barriers** are derived from the declared uses with synchronization2
   stage and access masks that cover only what the pass does, per mip for
   images: layout changes and write hazards get a barrier, read-after-read
   accumulates stages, the first use of a transient starts from UNDEFINED
   and orders after whatever it aliases, cross-queue sources name no
   stages (the semaphore orders them), and exclusive images crossing
   families get a release on the owner's last pass and an acquire on the
   consumer. The final transition to PRESENT and the swapchain semaphores
   attach to the batches that first and last touch the presented image.
5. **Recording**: each pass records into its own primary command buffer
   with its barriers in front, so with a job system the passes record in
   parallel, one job per pass, and are submitted in order. Timestamps
   bracket every pass; `nph_gpu_rg_timings` reports the frame that
   completed two frames ago.

Imported resources get their final state written back at the end of
execute, so the next frame, or explicit commands, continue from the truth.

## Swapchain

A swapchain is created for a window (the platform module makes the
surface) and exposes its images as ordinary handles. Acquire recreates the
swapchain on resize, out-of-date, or a vsync change and returns
`NPH_RESULT_EMPTY` for a minimised window; present semaphores belong to
one swapchain generation and retire with it. Vsync on is FIFO; off prefers
MAILBOX, then IMMEDIATE.

## Samples

- `samples/gpu_triangle`: window, device, swapchain, one pipeline, a
  one-pass graph (§8.6 stage 1).
- `samples/gpu_mesh`: the Phase 5 exit sample: textured spinning cubes
  with vertex pulling, a mip chain generated on the GPU, a reverse-Z
  transient depth buffer, per-instance transforms from an async compute
  pass, and shader hot reload. Exits with 1 on any validation error.

Both exit with 77 (CTest "skipped") on a machine without a window system
or a Vulkan device.

## Testing

`engine/gpu/tests` (68 tests) run on any conforming device and skip
without one: device selection and information, the validation and leak
gates, timelines and submission, frames and timestamps, deferred
destruction, buffers, images of every kind, samplers, bindless slot
bounds, fills, clears, copies, blits, mip generation, per-thread command
pools from eight threads, uploads through the ring and past it, image
uploads across queues with layer subsets, SPIR-V reflection on the
compiled shaders and on hand-assembled modules (including malformed
ones), compute through buffer addresses, offscreen triangle rendering
with pixel checks, bindless sampling and storage images, pipeline
validation, hot reload, the pipeline cache file (including a corrupt
one), render-graph culling, attachments, aliasing, async compute,
parallel recording on the job system, timings, limits, and, on a window,
swapchain present, resize, vsync, and presentation through the graph. CI
runs them on lavapipe under Xvfb; the self-hosted runner runs them on the
RTX 3060 Ti.
