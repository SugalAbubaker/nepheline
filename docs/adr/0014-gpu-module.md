# 0014. GPU module: vendored Vulkan headers, volk and VMA, one bindless layout, and a Vulkan-shaped RHI

- **Date:** 2026-10-13
- **Status:** Accepted

## Context

CLAUDE.md §8.5 fixes the shape of the RHI: Vulkan 1.3 only, a thin
abstraction that removes boilerplate and enforces correctness, VMA behind
`nph_gpu_alloc_*`, one global bindless descriptor set with buffers reached
through device addresses, per-thread command pools per frame in flight, a
render graph that derives barriers and aliases transients, a persisted
pipeline cache, SPIR-V reflection by our own parser, and headless tests on
lavapipe with a zero-validation-message gate. §1.8 forbids every module
but the owner from including a third-party header; §3 lists volk and VMA
as the allowed plumbing and says the shaders are compiled at build time
with glslc from the Vulkan SDK.

Questions the plan leaves open: where the Vulkan headers come from, how a
C engine compiles VMA's C++ implementation, how far "thin" goes (do
modules to the right see Vulkan types?), how the render graph records in
parallel, and how resources cross queue families.

## Decision

1. **Vulkan-Headers is vendored as a pinned submodule** (`third_party/
   vulkan_headers`, tag `vulkan-sdk-1.4.321.0`), alongside volk (same tag)
   and VMA (`v3.4.0`). The engine compiles against the vendored headers on
   every machine; the installed SDK supplies only glslc (build time), the
   validation layers (Debug and Development), and the loader (run time,
   found by volk). The instance asks for API 1.3 and the device must offer
   1.3; 1.4 headers compile fine for that. CI installs the matching SDK on
   Windows and `glslc` plus `mesa-vulkan-drivers` and
   `vulkan-validationlayers` on Ubuntu 24.04.

2. **VMA is one C++ translation unit behind a C interface.**
   `engine/gpu/src/gpu_vma.cpp` includes `vk_mem_alloc.h`, defines
   `VMA_IMPLEMENTATION`, and implements `nph_gpu_alloc_*`
   (`src/gpu_alloc.h`); nothing else includes the VMA header. The TU is
   built with its own warning policy (`/W0`, `-w`), without the engine's
   sanitizer flags, and the C++ configuration flags mirror the C ones
   (`cmake/NphConfigurations.cmake`). VMA's assertions go through the
   engine's assert handler. `NPH_STATIC_ASSERT` became C++-safe for the
   core headers that TU includes.

3. **The public API is Vulkan-shaped but exposes no Vulkan type.** Enums
   mirror Vulkan's concepts one to one (stages, access, layouts, formats,
   load and store operations), handles are the engine's 32-bit
   generational handles, and the one escape hatch, `nepheline/gpu/vk.h`,
   may be included only by the gpu module's own tests, the GPU
   benchmarks, and the editor's UI backend. `scripts/check_layering.py`
   enforces this and the §4 module order in CI. A later `render` module
   therefore never sees `vulkan.h`, which is what §1.8 asks, without
   pretending to be cross-API.

4. **One global pipeline layout.** A single descriptor set holds sampled
   images (binding 0), storage images (one slot per mip, binding 1), and
   samplers (binding 2), all update-after-bind and partially bound, sized
   from the device limits (65536 / 16384 / 1024 by default), plus 128
   bytes of push constants for every stage. Buffers are reached only
   through device addresses. Reflection verifies that a shader fits this
   layout and rejects anything else; there are no per-material descriptor
   sets, no descriptor pools per frame, and binding a pipeline binds
   everything.

5. **Deferred destruction keyed to frames in flight.** Destroying a
   resource invalidates its handle at once and queues the Vulkan objects
   and bindless slot on the current frame slot; they are released when
   that frame completes on every queue, or at `nph_gpu_device_wait_idle`.
   Two frames in flight keep the CPU one frame ahead (§10.6).

6. **Timeline semaphores only, one per queue plus the user's.** Every
   submit signals its queue's timeline; frame completion is a wait on the
   recorded values; cross-queue dependencies in the render graph and the
   upload path are timeline waits. The only binary semaphores are the
   swapchain's acquire (per frame slot) and present (per image, per
   swapchain generation) semaphores, handled by naming the swapchain in
   the submit.

7. **Sharing policy.** Buffers are shared across queue families
   (`VK_SHARING_MODE_CONCURRENT`): no compression is at stake and it
   removes every buffer ownership transfer. Images that are uploaded into
   and never rendered to are shared as well, so uploads from the transfer
   queue need no release/acquire and may replace a subset of a mip's
   layers. Attachments stay exclusive to keep framebuffer compression
   (§10.3.5); the render graph generates release/acquire pairs when they
   cross families, and the upload path does the same for an exclusive
   image. Layouts are tracked per mip; a mip's layers stay in one layout.

8. **The render graph records one primary command buffer per pass**, with
   that pass's barriers in front, after compiling barriers for the whole
   frame. Passes are then independent and, given a job system, are
   recorded in parallel as one job each and submitted in order. No
   secondary command buffers are needed for this; they remain available
   for a later multi-threaded pass. Transient memory requirements are
   taken with `vkGetDeviceImageMemoryRequirements` (core 1.3) before any
   object exists, transients are packed into a few blocks by lifetime,
   and a transient whose description, block, and offset repeat is reused
   from a cache, so a steady-state frame allocates nothing.

9. **Cross-queue barriers name no source stages.** Work on another queue
   is ordered by a semaphore wait, which also makes its writes available,
   so the barrier on the consumer uses `NONE` as its source scope; a source
   stage of the producer's family would not even be valid on the
   consumer's queue (`VUID-vkCmdPipelineBarrier2-srcStageMask-09675`,
   found by the mesh sample on the first run with async compute).

10. **Validation messages are the gate.** The validation layer with
    synchronisation validation is on in Debug and Development; messages
    are counted by severity, errors are printed in full to stderr, the
    test fixture requires zero errors, and the samples exit non-zero on
    any. Loader and layer status messages are traced, not counted.

## Alternatives considered

- **Use the installed SDK's headers** (`find_package(Vulkan)`). Different
  machines and CI images would compile against different header versions;
  Ubuntu 24.04 ships 1.3.275, the development machine 1.4.321. A pinned
  submodule makes the build reproducible and the SDK optional for
  compiling.
- **A cross-API RHI** (D3D12 later). Not a goal (CLAUDE.md §8.5); the
  abstraction would cost the exact control the §10 budgets need.
- **Expose Vulkan types in the public API** for thinness. Would make every
  renderer file depend on `vulkan.h` and defeat §1.8; the enum mirrors
  cost one switch each and nothing at run time.
- **Per-material descriptor sets** or push descriptors. The bindless set
  is what §8.5 and the GPU-driven pipeline of §8.6 require; descriptor
  management per draw is the cost the design avoids.
- **Fences for frame completion.** Timeline semaphores express the same
  thing with one object per queue, support waiting for several queues at
  once, and are what the transfer-queue uploads need anyway.
- **Secondary command buffers for parallel recording.** One primary per
  pass is simpler and exactly as parallel for pass-granular work; the
  render graph's pass count (tens) matches the worker count.
- **Exclusive sharing for every image** with ownership transfers in the
  upload path. Partial-layer uploads would then need a release on the
  owning queue before the transfer, which the upload API cannot record;
  sharing textures removes the transfer at no cost on the reference
  hardware.
- **Our own GPU allocator now.** VMA is allowed by §3 and candidate for
  later replacement; the C interface it sits behind is the seam.

## Consequences

- Every machine compiles against Vulkan 1.4.321 headers; raising the pin
  is one submodule update for headers and volk together.
- Windows CI installs the LunarG SDK (cached per version) for glslc and
  skips the GPU tests for lack of a driver; Linux CI runs them on
  lavapipe under Xvfb, which also exercises the swapchain through X11.
- A shader that declares its own descriptor set does not load; the
  reflection error says why.
- Cross-queue use of an *exclusive* image imported into the render graph
  must begin on the queue that owns it (the graph cannot release on a
  queue it has no pass on); shared textures have no such rule.
- The self-hosted RTX 3060 Ti runner (ADR-0002) is registered in this
  phase; its workflow gains push and pull-request triggers once the GPU
  benchmarks exist.
