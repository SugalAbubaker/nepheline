# 0008. Math module: conventions, storage layouts, and SIMD layering

- **Date:** 2026-10-12
- **Status:** Accepted

## Context

CLAUDE.md §8.2 asks for a math module with scalar storage types, SIMD
register types, SoA "wide" types, explicit load/store between the layers,
right-handed Y-up column-major conventions, reverse-Z infinite-far
projection in Vulkan clip space, 64-bit world coordinates, and property
tests with ULP tolerances. §2 asks for deterministic simulation and §3 fixes
the CPU baseline at SSE4.2 with AVX2 paths selected at runtime.

Every later module reads and writes these types, shares some of them with
GLSL through `shaders/shared/`, serialises them into cooked assets, and
runs the culling and physics kernels built on the wide types in the frame
budget. The following details therefore have to be fixed before any of
that code exists, and they are not derivable from the headers alone.

## Decision

1. **Coordinate and algebra conventions.** Right-handed, Y up. Vectors are
   column vectors, matrices are **column-major** (`NphMat4.c[col]` is a
   column), and a transform is applied as `M · v`. `nph_*_mul(a, b)` is the
   product `a · b`, so it applies `b` first and `a` second; the same holds
   for quaternions and affine transforms. View space looks down **−Z**.
   Angles are radians.
2. **Projection.** The projection matrices produce Vulkan clip space
   directly: X right, **Y down**, Z in [0, 1]. Depth is **reversed** (the
   near plane maps to 1) and the default perspective has an **infinite far
   plane** (Reed, "Depth Precision Visualized", 2015; Upchurch & Desbrun,
   "Tightening the Precision of Perspective Rendering", 2012). Because the
   far plane is at infinity, a frustum extracted from such a matrix has
   **five** planes; `NphFrustum` carries a plane count. The Y flip lives in
   the projection matrix and nowhere else, so view and world space stay
   right-handed Y-up and viewport transforms are never needed.
3. **Quaternions** are `{x, y, z, w}` with the scalar last, matching glTF,
   GLSL, and the common C convention, and use the Hamilton product. Unit
   quaternions represent rotations; `nph_quat_rotate(q, v)` computes
   `q v q*`. Functions that take a rotation assume a unit quaternion and do
   not renormalise.
4. **Three layers, one set of semantics.**
   - *Scalar storage types* (`NphVec2/3/4`, `NphQuat`, `NphMat3/4`,
     `NphAffine`, `NphAABB`, …) are plain structs of `f32` with natural
     4-byte alignment and no padding, so they can be embedded in any struct
     or file format and shared with GLSL. Every operation exists on them;
     these implementations are the **reference** that the SIMD paths are
     tested and benchmarked against.
   - *SIMD register types* (`NphF32x4` over `__m128`, `NphSimdMat4`,
     `NphSimdAffine`) carry the hot-path implementations. They enter and
     leave through explicit `nph_simd_load_*` / `nph_simd_store_*`
     functions that use unaligned loads, so storage types never need
     over-alignment. Following DirectXMath, one register type serves
     vectors, points, quaternions, and planes; the operation name says
     which semantics apply (`nph_f32x4_dot3`, `nph_f32x4_quat_mul`).
   - *Wide SoA types* (`NphVec3x4`, `NphVec3x8`) hold four or eight
     independent vectors for culling and physics batches. `NphF32x8` is
     `__m256` inside translation units compiled with AVX2 and a pair of
     `__m128` elsewhere, with identical size and alignment. The batch
     kernels are compiled twice (an SSE4.2 unit and an AVX2 unit) from one
     source and exposed through `NphMathKernels`, a table selected once by
     `NphSimdLevel`; dispatch never happens per call, and wide values never
     cross between units compiled at different ISA levels.
5. **`NphAffine` is stored as three rows of `NphVec4`** (48 bytes): the top
   three rows of the column-vector 4×4 matrix. Mathematically it is the
   same column-vector transform as `NphMat4`; only the memory order differs.
   Three rows pack into three GPU `vec4`s with no padding, which the
   column-major 4×3 layout cannot do under std140/std430, and a point
   transform is three dot products. Every other matrix is column-major.
6. **Floating-point determinism policy.** All Nepheline code compiles with
   IEEE-754 semantics: no fast-math, and **no contraction** of `a*b + c`
   into FMA (`-ffp-contract=off` on GCC/Clang, `/fp:precise` on MSVC, set
   in `cmake/NphArch.cmake`). The SIMD layer uses only correctly rounded
   instructions (add, sub, mul, div, sqrt, min, max). The approximate
   `rsqrt`/`rcp` instructions differ between CPU vendors and are exposed
   only under a `_fast` suffix for code that has shown, by measurement,
   that it needs them and can tolerate the error; they are banned from
   simulation. Consequence: for the same sequence of operations the
   scalar, SSE, and AVX2 paths produce **bit-identical** results, which the
   tests assert, and §8.9's same-platform determinism holds for math.
7. **Failure reporting.** Math functions return values. The few that can
   fail on mathematical grounds (`inverse` of a singular matrix,
   `decompose` of a degenerate transform) return `bool` and leave the out
   parameter untouched; they do not return `NphResult`, which is for
   operational failures (memory, I/O, parsing). Precondition violations
   such as normalising a zero vector are `NPH_ASSERT`ed in Debug and
   Development and documented as undefined otherwise; `_or_zero` and
   `_or` variants exist for callers that want a defined fallback.
8. **Large-world coordinates.** World positions on the CPU are `NphDVec3`
   (`f64`). The GPU receives only camera-relative `f32` positions produced
   by `nph_dvec3_relative`, so single precision is spent on the distance
   from the camera rather than from the origin.
9. **SIMD level detection lives in math.** `nph_math_detect_simd_level`
   reads CPUID and XGETBV through compiler intrinsics, which §4 permits for
   modules at the left of the dependency chain, and returns the highest
   level the CPU *and* the OS support. Platform's broader CPU detection
   (Phase 3) builds on it rather than duplicating it.

## Alternatives considered

- **A single SIMD type set with 16-byte-aligned `NphVec3`** (DirectXMath,
  many C++ engines). Rejected: padded vectors cost 33 % more memory and
  bandwidth in every vertex, bounds, and transform array (§10 "bandwidth
  is the budget"), and over-aligned structs do not match GLSL layouts or
  file formats.
- **Row-major matrices.** Rejected: GLSL and SPIR-V default to
  column-major, and shared C/GLSL structs must agree without transposes.
- **Column-major 4×3 `NphAffine`.** Rejected for the std140/std430 padding
  described above; the rows-of-`vec4` layout is what instance buffers use.
- **`NphResult` from math functions.** Rejected: the failure is a property
  of the input, not of the system, and `if (!nph_mat4_inverse(&m, &inv))`
  reads correctly at every call site.
- **Allowing FMA contraction** for speed. Rejected for now: it makes the
  SSE and AVX2 kernels disagree in the last bit and makes results depend on
  the compiler's expression grouping. A future ADR may introduce explicit
  FMA in kernels that opt out of determinism, backed by measurements.
- **Our own `sin`/`cos`/`exp`** for cross-platform bit-exactness. Deferred:
  §8.9 guarantees determinism on the same platform only, which the C
  library provides. Lockstep networking across platforms would reopen
  this.

## Consequences

- Shader-side structs in `shaders/shared/` mirror the storage layouts
  directly: `vec3`/`vec4`/`mat4` column-major, affine transforms as three
  `vec4` rows.
- Every SIMD operation has a scalar twin, and `engine/math/tests/` checks
  them against each other bit-for-bit for the operations that are the same
  arithmetic, and within an ULP bound where the order differs.
- Code that needs the fast approximations must say so in its name, and
  reviewers can grep for `_fast`.
- Benchmarks in `benchmarks/math/` compare scalar, SSE, and AVX2 variants of
  the same workload; the Phase 2 exit criterion is that the SIMD paths win.
