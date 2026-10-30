# 0065. Virtual texturing: sparse images, a page's residency tested where it is sampled

- **Date:** 2026-10-29
- **Status:** Accepted (stage 0, the measurements below, decided between sparse images and a
  page table of our own; built in stages from stage 1)

## Context

§8.8 asks for virtual texturing: "textures are split into BC-compressed pages. Residency
is driven by GPU feedback (which pages and mips were actually sampled), so the working set
fits in 8 GB of VRAM", and Phase 13 lists it. Today every texture is resident whole:
`nph_render_texture_create` uploads every level of every layer, and a texture stays until
destroyed. Summed from the cooked packs:

| Scene | Textures | On the GPU | Top levels | By format |
|---|---|---|---|---|
| Bistro | 573 (68 at 4096², 270 at 2048²) | 3.20 GiB | 2.35 GiB | BC7 sRGB 1.24, BC7 1.22, BC5 0.67 GiB |
| Landscape | 51 (all 2048²) | 272 MiB | 204 MiB | BC7 sRGB, BC7, BC5: 91 MiB each |

That is what a scene holds, not what a view samples. The first game (ADR-0059) adds its
ruins, its village and its terrain's materials, and full residency grows with that content,
where §2 lets the world's size touch only streaming. The landscape's 4 x 3 world holds
6.04 GB of the 6.5 GB working set (ADR-0063 stage 4) with only its 272 MiB of textures.

The resolve samples a material's textures (up to five: base colour, normal, metallic and
roughness, occlusion, emission) with the analytic gradients it computes (`textureGrad`,
trilinear and 8x anisotropic). In Bistro's street they are 209 of the resolve's 2,291
instructions a pixel and 0.32 ms, spent waiting on memory (docs/render.md "Where the time
goes"). The forward reference, the alpha tests of the raster passes and the radiance
cache's ray hits sample the same textures. The user's rule stands (2026-10-20): never
trade quality. A resident texture must sample as the whole texture does.

Two ways to page a texture:

- **A page table of our own** (Barrett 2008; van Waveren 2009, 2012; Unreal's and Unity's
  streaming virtual textures): pages with border texels copied into a physical atlas, an
  indirection lookup per sample. It works on any device. The borders must hold the
  filter's whole footprint, though, and an anisotropic trilinear footprint at the finer
  level reaches up to N + 1 texels from its centre (N probes over up to 2N texels, plus
  bilinear's texel): 9 at 8x. Block-aligned borders that hold it exactly are 16 texels
  with a second level in each page, 1.58 to 1.95 times the payload's memory. Narrower
  borders (Unity's 8 limit anisotropy to 8, and still cut the outer probes as the level
  fraction nears 1) or a manual blend of two single-level samples differ from the whole
  texture's filter.
- **Sparse images** (Vulkan sparse residency, the hardware's own page table): no borders,
  no indirection, filtering exactly the whole texture's. But binding goes through the
  driver (`vkQueueBindSparse`), and NVIDIA's has been reported slow: tens to hundreds of
  microseconds a page, growing with the pages bound, on Windows and Linux drivers 535 to
  575, under a lock shared across processes, acknowledged and not fixed (NVIDIA developer
  forums, 2023 to 2025).

### Stage 0, measured (2026-10-29)

A standalone benchmark (`build/scratch/claude/sparse_bench`, to become a tracked GPU
benchmark in stage 1) on the RTX 3060 Ti (driver 610.74, Windows 10 19045): four sparse
BC7 images of 16384² (15 levels), pages of 256 x 256 texels (64 KiB, the standard block
shape), the mip tail from level 7 (64 KiB). Binding on the transfer family's queue (family
1, sparse binding), a fence a call:

| Pages a call | The call | Until done | A page |
|---|---|---|---|
| 1 | 20 µs | 108 µs | 113 µs |
| 16 | 22 µs | 233 µs | 15 µs |
| 64 | 28 µs | 473 µs | 7.4 µs |
| 256 | 55 µs | 1.5 ms | 5.8 µs |

Unbinding costs 8 to 42 µs a page until done, in batches of 16 to 256 (alternating between
two rates from one range of 2,048 pages to the next). Neither cost grew with the pages bound
(to 16,384, 1 GiB). About one call in 2,000 took 2.1 ms. The render thread's submits showed
the same 2.1 ms stall with no binding at all.

Beside rendering: another thread submitted 0.36 ms of compute work at a time to the graphics
queue, as fast as each completed (about 2,400 a second). Meanwhile the main thread moved
pages at 240 Hz, each move one page unbound and another bound to its memory:

| Moves a frame (binds a call) | Render GPU time p99 | Render submit p99 | The bind call p99 | Until done (median) |
|---|---|---|---|---|
| none | 410 µs (clocks rising) | 20 µs | | |
| 8 (16) | 358 µs | 19 µs | 127 µs | 348 µs |
| 32 (64) | 358 µs | 20 µs | 1.0 ms | 791 µs |
| 128 (256) | 359 µs | **2.66 ms** (max 4.2) | 3.8 ms | 2.3 ms |

The GPU's rendering never slowed. A call of 256 binds holds a lock in the driver that
stalls the render thread's `vkQueueSubmit`. Calls of 64 binds or fewer did not.

Sampling a sparse image, every page bound, against an ordinary image with the same texels
(4K, one or four BC7 textures a pixel with the same coordinates, trilinear, 8x anisotropic):

| View | Sparse | Ordinary | |
|---|---|---|---|
| wall, a texel a pixel, 4 textures | 275.9 µs | 275.6 µs | +0.1% |
| wall, a texel a pixel, 1 texture | 158.4 µs | 158.4 µs | 0.0% |
| wall, 4 pixels a texel, 4 textures | 236.9 µs | 236.8 µs | 0.0% |
| wall, 4 texels a pixel, 4 textures | 288.0 µs | 287.0 µs | +0.3% |
| floor to a horizon, 4 textures | 1,752.9 µs | 1,737.9 µs | +0.9% |
| floor, 4 times the texels, 4 textures | 1,701.4 µs | 1,701.4 µs | 0.0% |

The RTX also offers `shaderResourceResidency`, `shaderResourceMinLod`,
`residencyNonResidentStrict` and the standard 2D block shapes. lavapipe (Mesa 25.2.8 on
Windows) offers no sparse binding or residency at all.

## Decision

1. **Sparse images, the hardware's page table.** A texture whose top level spans more than
   one page is created sparse-resident, with the standard block shapes: 256 x 256 texels
   for 16-byte blocks (BC5, BC6H, BC7), 512 x 256 for 8-byte blocks (BC4). Its mip tail
   (the levels smaller than a page in either dimension: the first at level 7 of a 16384²
   BC7 image, 4 of a 2048²) is bound and uploaded at creation and never leaves, so every
   texture always has a level to fall back to. Smaller textures and devices without
   `sparseResidencyImage2D` and the standard shapes keep the images they have today,
   resident whole, and the same shaders. (Stage 1's review dropped `shaderResourceMinLod`
   from the requirement: see there.)
2. **A page is tested where it is sampled.** *Replaced in stage 2: a paged texture's
   levels are resident whole, and its samples clamp to the first with the sampler's
   minimum level of detail ("Stage 2").* One GPU buffer holds a bit per page of every
   level of every sparse texture: set only once the page is bound and its texels uploaded,
   cleared before it is unbound. Every shader that samples a material texture (the
   resolve, the forward reference, the raster passes' alpha tests, the radiance cache's
   hits) does so through one function:
   - It computes the level the sampler will take from the gradients, as the sampler does:
     the derivatives' lengths in texels, the anisotropic ratio clamped to the sampler's
     maximum. It allows a margin for the hardware's own arithmetic, so the levels tested
     always include the two the hardware blends.
   - At each such level, the footprint's box: the centre ± half the gradients' extent,
     ± one texel for bilinear, wrapped or clamped as the sampler addresses. Then the pages
     the box touches: one or two a side.
   - While a level's pages are not all resident, it moves a level coarser (to the tail at
     worst). It samples with that level as its minimum level of detail (`MinLod`).
   - Where every page tested is resident, it clamps nothing: the sample is the whole
     texture's, byte for byte.
   - A material's textures of one extent and block size share the footprint's
     arithmetic. Each tests only its own bits.
3. **No frame reads a page while its binding changes**, as Vulkan requires ("other queues
   do not access ranges of memory concurrently with a binding change"):
   - A page is bound, then its texels are copied (the copy waits on the bind's timeline
     value). Its bit is set in the upload that follows the copy's completion.
   - Leaving, its bit is cleared in frame N's upload, and its unbinding waits until frame
     N's work and every frame before it are done. A shader never touches a page whose bit
     is clear, so no frame reads it.
   - A coarser page goes resident before any finer page under it, and leaves after them,
     so the levels tested always end at a resident one.
4. **Feedback where the pixels sample.** *In stage 2, one pixel of each 8 x 8 tile a
   frame, the opaque pixels in a pass of their own.* Each frame, one pixel in 16 (a
   pattern that covers every pixel in 16 frames) records the pages its footprint touches
   at the level the hardware wants, unclamped. It writes them as `atomicOr` into a request bitmap laid out
   like the residency bits, one atomic a subgroup per distinct page. The bitmap is read
   back `NPH_GPU_FRAMES_IN_FLIGHT` frames late, as the cells' level requests are
   (ADR-0063). Bistro's pages are about 52,000 bits, 6.5 KiB a frame.
5. **A stream with a budget, in the runtime**, like the proxies' (`NphProxyStream`,
   ADR-0063). *With stage 2's whole levels, it loads a texture's levels whole, down to
   the finest its pixels ask for, and evicts them finest first.*
   - It holds each page's last request and loads what is asked and missing, coarser levels
     first (a finer page waits for its parent).
   - Over its budget, it evicts the least recently requested pages, never one with a
     resident child.
   - Pages come from the memory-mapped pack, 64 KiB contiguous each. The renderer
     allocates the page's memory from a pool of 64 MiB blocks, sized by the budget, and
     copies through the staging ring.
   - It moves at most 32 pages a frame: one call of at most 64 binds, from the thread that
     submits the frame, right after its submit, where stage 0 measured no stall
     (480 MiB/s at 240 Hz).
6. **Cooked page by page.** The texture type's version 2 stores each level of at least a
   page as its pages in raster order, each page's blocks row by row, so a page is one read
   and one copy. Partial pages at the right and bottom edges hold only their texels. The
   tail's levels are stored as today. The cooker lays out the blocks it already encoded,
   with no new encoding and no change to a texel. A device that pages nothing copies the
   same layout into a whole image, page by page.
7. **Checked in development builds.** *Dropped in stage 2 ("Stage 2").* The development
   and debug builds sample with `OpImageSparseSampleExplicitLod` and count samples that
   read a page not resident (`residencyNonResidentStrict` makes such reads zeros). The
   tests require the count to be zero. Shipping samples without the residency code.

Stages:

1. **gpu**: sparse images (creation, page and tail requirements), a page pool, binding and
   unbinding on the sparse queue with timeline values, tests on devices with sparse
   residency (skipped without), and stage 0's benchmark tracked in `benchmarks/gpu`.
2. **render**: sparse textures with their tails, the residency and request bits, the
   sampling function in every shader that samples a material, the feedback, page install
   and evict with the deferred unbind. Tests:
   - with every requested page resident, images byte for byte those of whole textures;
   - a missing page samples its level's coarser neighbour, not zeros;
   - the residency counter at zero and validation clean.
   Measured: the resolve's added instructions and time, the feedback's bytes.
3. **asset, cook, runtime**: texture version 2 (laid out from the cooked blocks), the
   stream and its budget, and `render_perf` with a texture budget. Measured on Bistro and
   the landscape: textures' VRAM against today's, frames' p99 walking and turning, the
   time to settle, and images after settling against whole textures.

### Stage 1, the gpu module's sparse images (2026-10-29)

Built as decided (docs/gpu.md "Sparse images"):
- `NPH_GPU_IMAGE_FLAG_SPARSE` images, kept in GENERAL and shared by the
  families, and page pools of 64 KiB pages in 64 MiB blocks.
- `nph_gpu_sparse_bind` on the transfer queue, each binding waiting for the
  one before on the sparse timeline.
- Upload flushes that wait for the latest binding, and freed pages returned
  with the frame.

`gpu_bench` now tracks what stage 0 measured, with budgets on the RTX runner:

| Benchmark | Median |
|---|---|
| `texture_sample_4k_rgba8_sparse` | 0.280 to 0.283 ms (ordinary: 0.280 to 0.282) |
| `sparse_bind_64_pages` | 0.33 to 0.37 ms |
| `sparse_unbind_64_pages` | 0.47 to 0.49 ms |

The binding benchmarks' budgets are twice their medians, as they time the
driver. In `test_sparse.c`, pages of three levels and the tail are bound in
one call and hold what is copied into them, and pages never bound read
zeros, under validation and synchronisation validation with no message.

Revised in review: the device's sparse residency now needs only
`sparseResidencyImage2D`, the standard block shapes and a queue that binds.
The shaders' features (`shaderResourceMinLod`, `shaderResourceResidency`)
are for the stage that samples with them to ask for. Unbound pages need not
read as zeros: nothing reads a page not bound, and `sparse_residency_strict`
reports it for the tests that check them. lavapipe on Linux (Mesa 25.2)
offers sparse residency without `residencyNonResidentStrict`, so requiring
it had left the sparse code untested in CI (the gpu module's coverage at
79.85% against its 80%).

### Stage 2, the renderer's paged textures: whole levels (2026-10-29)

Built (docs/render.md "Virtual textures"):
- **`paging.c`:** installs and evictions from any thread, one binding a frame
  after its upload flush, the copies in the frame after, the unbinding
  `NPH_GPU_FRAMES_IN_FLIGHT` frames after a page leaves; requests cleared
  and read back.
- **The levels resident from creation** are those from the first under a
  page in either dimension: the device's mip tail, and on a device that
  keeps none (lavapipe on Linux, whose tail starts past the last level),
  those levels' own pages, a page each.
- **`vis page requests`** and the forward pass: the feedback.
- **`cooked_scene`'s `paged_textures`** and `render_perf --paged-textures`:
  every page of every texture installed before a view is measured.

Point 2 as decided, a page tested where it is sampled, cost the resolve too
much, and was replaced: **a paged texture's levels are resident whole**, and
its samples clamp to its first resident level with the sampler's minimum
level of detail (each material sampler made again for levels 1 to 8).
Every shader samples it as it samples any texture, so the clamp costs no
register and no instruction, and every level from it is resident whole, so
no sample reads a page that is not, whatever its footprint, anisotropy or
addressing. Requests come from one pixel of each 8 x 8 tile a frame, in a
pass of their own for the production path's opaque pixels.

What was measured first, Bistro's street with every one of its 51,605 pages
resident (3,382 MB; RTX 3060 Ti, native 4K; its resolve 2.77 ms with the
textures whole):

| Sampling | Resolve | |
|---|---|---|
| each footprint's pages tested against residency bits, the gradients scaled (point 2) | 3.49 ms | 72 registers and 160 to 176 bytes of local memory where the variant had 80 and none; the test compiled out, 2.86 |
| the tests hoisted before the shading, slot by slot, scales packed | 3.50 ms | |
| one test in a loop over the slots | 3.72 ms | |
| a map of each 32 x 32 texels' finest resident level (kept on the CPU), a lookup a texture, MinLod | 3.50 ms | requests in the resolve: 0.45 ms of it |
| the same without the requests | 3.01 ms | |
| one lookup a pixel before the surface's rebuild | 3.03 ms (hedge 2.62 against 2.42) | no spill: a dependent load more in a pass bound by its chains of them |
| the same with the requests beside it, never run | 3.26 to 3.30 ms | |
| each texture's first whole level as MinLod, from the material | 2.82 to 2.93 ms | the operand alone 0.09 ms, the masked index 0.05 |
| **the level as the sampler's minimum level of detail** | **2.82 to 2.88 ms** | the samples of sparse images about 0.04 ms, the request pass beside it the rest |

With the requests in their own pass (one pixel of each 8 x 8 tile a frame,
0.051 to 0.058 ms), whole levels cost the frame +0.09 to +0.15 ms in the
street, +0.01 to +0.04 at the hedge, +0.13 on the terrace. In the request
pass, the atomics on words many pixels share were most of its time (0.11
to 0.18 ms with every pixel of one tile in 16 asking).

What whole levels give up, from the pages Bistro's views asked for (of its
3,346 MB of paged levels):

| View | Pages asked for | Whole levels from the finest asked |
|---|---|---|
| street | 376 MB | 864 MB |
| terrace | 350 MB | 1,032 MB |
| hedge | 185 MB | 450 MB |
| street, walking | 434 MB | 914 MB |
| street, turning | 603 MB | 1,391 MB |

So whole levels keep 2.3 to 3 times what pages would, and 59 to 87% less
than every texture whole. The per-page map (a lookup a pixel, about +0.26
ms) stays the alternative for textures too large to keep a level whole,
taken up if a scene's measured working set needs it.

Point 7, the development builds' residency codes, is dropped: with whole
levels a read of a page not resident would be a missed level, which the
tests' byte-for-byte images catch. Nothing samples with the MinLod operand
or residency codes, as stage 1's review had made the device's requirement.

## Alternatives considered

- **A page table of our own** (above): rejected for the filter it cannot reproduce at
  borders that fit (or the memory of borders that do), and for a dependent lookup and
  address arithmetic per sample in a resolve bound by issue and latency. Stage 0 measured
  sparse images' sampling at the cost of ordinary images and their binding at microseconds
  a page on this driver, which removed the reasons to prefer it. It remains the way to
  page textures on devices without sparse residency if one ever has to.
- **A region's minimum level in a map** (D3D12 sampler feedback streaming's MinMip map,
  one lookup a sample): a region's clamp changes all its pixels at once, so evicting a
  page coarsens its neighbours' pixels too. A footprint near a region's edge reads the next
  region's pages, so pages being bound or unbound beside resident ones would be read during
  the change unless a ring of pages around everything requested stayed resident (a page is
  256 texels wide). The per-pixel test reads one or two words more and needs neither.
- **The residency code alone** (sample, and resample coarser where the code says a texel
  was not resident): reads pages while they are being bound or unbound (point 3), and
  every pixel at a missing level pays a second sample.
- **Streaming whole levels** (each texture recreated at its finest needed level): a level
  is the unit (a 4096² BC7 top level is 16 MiB where a corner is seen), and each change
  recreates an image and its descriptor.
- **Lower-resolution textures** to fit a budget: a trade of quality, declined.

## Consequences

- Textures' VRAM becomes the pages views sample, the tails, and the pool's slack, bounded
  by the budget, instead of every level of every texture loaded.
- Every material sample takes the residency test: in the resolve, the footprint once per
  material and a word or two per texture. Stage 2 measures it against the 209
  instructions the textures take now.
- Binding is a driver path with a history of slowness elsewhere. The benchmark runs
  nightly on the RTX runner so a driver regression shows, and the stream's 32 moves a
  frame stay where stage 0 found no stall.
- Pixels whose pages are not resident yet are drawn from a coarser level. They are never
  wrong, and they sharpen as their pages arrive, 2 to 3 frames after the request plus the
  load.
- lavapipe on Windows has no sparse residency: the sparse paths are tested on the RTX
  (locally and on the runner), and in CI if Linux's lavapipe offers them. The CI log of
  the device's optional features will say.
- Textures cooked before version 2 stay loadable, resident whole. The scenes are re-laid
  out, not re-encoded.

## References

- S. Barrett. "Sparse Virtual Textures." GDC 2008.
- J. M. P. van Waveren. "id Tech 5 Challenges: From Texture Virtualization to Massive
  Parallelization." SIGGRAPH 2009, Beyond Programmable Shading.
- J. M. P. van Waveren. "Software Virtual Textures." id Software, 2012.
- J. Obert, J. M. P. van Waveren, G. Sellers. "Virtual Texturing in Software and
  Hardware." SIGGRAPH 2012 course.
- M. Mittring. "Advanced Virtual Texture Topics." SIGGRAPH 2008, Advances in Real-Time
  Rendering in 3D Graphics and Games.
- Microsoft. "Coming to DirectX 12: Sampler Feedback." DirectX developer blog, 2019; and
  the Sampler Feedback Streaming sample.
- The Khronos Group. Vulkan 1.3 specification, "Sparse Resources" (the concurrency rule,
  the standard block shapes, the mip tail).
- Unity. "Streaming Virtual Texturing: requirements and compatibility" (pages of 128 texels
  with 8-texel borders, anisotropy limited to 8).
- NVIDIA developer forums. "Sparse texture binding is painfully slow" (2023 to 2025).
