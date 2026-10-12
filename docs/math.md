# Math module (`engine/math`)

Vectors, quaternions, matrices, affine transforms, geometry, SIMD and wide
types, and random sequences (CLAUDE.md §8.2). Math depends on `core` only.
The decisions that shape every header are in
[ADR-0008](adr/0008-math-conventions.md); this document describes what the
module contains and how it is tested. It grows with the module.

Status: **complete for Phase 2.** Scalar layer (`scalar.h`, `vec.h`,
`quat.h`, `mat.h`, `affine.h`), geometry (`geometry.h`), packing (`pack.h`),
random (`random.h`), the SIMD register layer (`simd.h`), wide SoA types
(`wide.h`), and the ISA-dispatched batch kernels (`kernels.h`).

## Conventions in one paragraph

Right-handed, Y up, column vectors, column-major matrices (`m.c[col]`),
`nph_*_mul(a, b)` applies `b` first. View space looks down −Z. Projections
produce Vulkan clip space (Y down, Z in [0, 1]) with **reversed depth** and
an **infinite far plane** by default; the Y flip lives only in the projection
matrix. Quaternions are `{x, y, z, w}` with the Hamilton product. Angles are
radians. Storage types are plain `f32` structs with natural alignment and no
padding, so they can be embedded in files and shared with GLSL unchanged.
Fallible operations (`inverse`, `decompose`) return `bool` and leave the out
parameter untouched on failure; preconditions that would produce NaN
(normalising a zero vector) are asserted in Debug and Development.

## Scalar layer

### `scalar.h`

Constants (`NPH_PI`, `NPH_TAU`, `NPH_DEG_TO_RAD`, `NPH_F32_EPSILON`, …) and
inline wrappers over `<math.h>` with the engine's naming: `nph_f32_min/max/
clamp/saturate/lerp/inverse_lerp/remap/smoothstep`, rounding (`floor`,
`ceil`, `round` half-away, `round_even`, `trunc`, `fract`, `mod`, `wrap`,
`wrap_angle`), `sqrt`/`rsqrt` (correctly rounded, never the approximate
instruction), transcendental functions with `asin`/`acos` clamped to [−1, 1],
classification, bit access, and the comparisons the tests are built on:
`nph_f32_ulp_distance` (Dawson's integer-mapped distance; +0 and −0 are
0 apart), `nph_f32_approx_eq(a, b, abs_tol, rel_tol)`, `nph_f32_near`. A
small `f64` subset serves world positions, and the integer helpers cover
what C's `/` and `%` get wrong for negatives (`nph_i32_div_floor`,
`nph_i32_mod_floor`) plus `div_ceil`.

`nph_f32_lerp` is written as `(1 − t)·a + t·b` so that `t = 0` and `t = 1`
return `a` and `b` exactly; animation and interpolation code relies on that.

### `vec.h`

`NphVec2/3/4` with the full component-wise algebra, dot, cross (right-handed,
`x × y = z`), lengths and distances, `normalize` (asserted non-zero) with
`_or_zero` / `_or(fallback)` variants, lerp, min/max/clamp/abs, rounding,
component queries (`hadd`, `min/max_component`, `max_index`), `project`,
`reject`, `reflect`, `refract` (GLSL semantics, zero on total internal
reflection), `angle`, `perpendicular` (Hughes & Möller 1999), and
`orthonormal_basis` (Duff et al., JCGT 2017: branch-free, right-handed,
`cross(t, b) = n`). `NphIVec2/3/4` and `NphUVec2/3/4` carry the arithmetic
texture sizes and tile coordinates need, with float conversions.

`NphDVec3` holds 64-bit world positions. `nph_dvec3_relative(p, origin)`
subtracts in `f64` and rounds once, which is how positions reach the GPU:
100 km from the origin an `f32` resolves ~8 mm, but relative to a camera
one metre away it resolves ~60 nm (`test_vec.c` checks exactly that).

### `quat.h`

`NphQuat` with the Hamilton product (`mul(a, b)` applies `b` first), conjugate
and inverse, normalisation, axis-angle both ways (`atan2`-based for
precision at small angles), the single-axis constructors, Euler angles in
the XYZ (glTF/Blender) order with a gimbal-lock-aware inverse,
`nph_quat_rotate` as two cross products, the rotated basis vectors
(`axis_x/y/z`), the shortest-arc `from_to` (Melax, Game Programming Gems 1)
including the antiparallel case, `angle` and `angle_between`, `nlerp`,
`slerp` (Shoemake 1985; shortest path, nlerp fallback below 0.5°),
swing-twist decomposition (Dobrowolski 2015), and first-order integration of
an angular velocity for the physics substeps.

### `mat.h`

`NphMat3` and `NphMat4`, column-major. Constructors from columns, rows,
diagonals, quaternions, axis-angle, TRS; element and row access; products
(`mul_vec3`, `mul_vec4`, `transform_point`, `transform_point_perspective`,
`transform_vector`); transpose, determinant, inverse (3×3 by adjugate; 4×4
by Lengyel's six-cross-product method, *Foundations of Game Engine
Development* Vol. 1 §1.7.5) and the cheaper `inverse_affine`; `nph_quat_from_mat3`
(Shepperd/Day largest-diagonal branch selection); Gram–Schmidt
`orthonormalize` that preserves handedness; TRS decomposition that reports a
reflection as a negative x scale.

View and projection: `look_to`/`look_at` build the right-handed view matrix;
`perspective_infinite` (the default), `perspective`, and `orthographic`
build Vulkan clip space with reversed depth. For the infinite perspective,
depth is simply `near / distance`: 1 at the near plane, ½ at twice the
near distance, 0 at infinity (Reed, "Depth Precision Visualized", 2015).

### `affine.h`

`NphAffine` is the top three rows of a column-vector 4×4, stored as three
`NphVec4` rows (48 bytes, ADR-0008 §5), so it packs into GPU instance
buffers without padding and transforms a point with three dot products.
It offers the same constructors, products, transforms, inverse (general and
`inverse_rigid`), and TRS decomposition as `NphMat4`, and converts to and
from it losslessly.

## Geometry (`geometry.h`)

Scalar reference implementations of the bounding volumes and tests the
culling and physics code is built on (Ericson, *Real-Time Collision
Detection*). The wide SoA kernels implement the same frustum tests for four
or eight volumes at a time and are checked against these.

- **`NphAABB`** (`min`, `max`; 24 bytes). `nph_aabb_empty` is the inverted
  box (+inf, −inf) so `union`/`expand` need no first-point special case;
  `is_empty` detects it and the result of intersecting disjoint boxes.
  Centre/extents/size/area/volume, the eight corners (bit *i* of the index
  selects `max` on axis *i*), union, expand, grow, intersection, translate,
  closed-interval containment and overlap, closest point and squared
  distance, `from_points`, `bounding_sphere`, and `nph_aabb_transform`
  (Arvo, Graphics Gems 1990: transformed centre plus `|L|·extents`).
- **`NphSphere`** (16 bytes). Containment and overlap with points, spheres,
  and boxes (closest-point test), the minimal enclosing sphere of two
  spheres, Ritter's two-pass approximate bounding sphere of a point set
  (Graphics Gems 1990), and transform by a rotation-and-scale affine.
- **`NphPlane`** (`normal`, `d`; `n·p + d = 0`; 16 bytes, one SIMD register).
  Construction from point-and-normal, three points (right-hand rule), or a
  `vec4`; normalisation; signed distance; projection; side with epsilon;
  flip; and transform by the inverse transpose of an affine's linear part.
- **`NphRay`** (`origin + t·dir`, `t ≥ 0`). Slab test against boxes with an
  explicit branch for axis-parallel directions (avoiding the `0·∞` NaN of
  the pure form), nearest intersection with spheres (exit point from
  inside), planes, and triangles (Möller–Trumbore 1997, optional back-face
  culling, barycentrics out). Closest point on a segment and on a triangle
  (Ericson §5.1.5, Voronoi regions).
- **`NphFrustum`**: five or six inward-facing normalised planes extracted
  from a clip matrix by combining its rows (Gribb & Hartmann 2001) for
  Vulkan's `0 ≤ z ≤ w` and reversed depth; the far plane of an infinite
  projection is degenerate and dropped, and unused slots hold a plane every
  point satisfies so fixed-count loops work. Point containment, and
  box/sphere classification into OUTSIDE / INTERSECTS / INSIDE with the
  p-vertex/n-vertex test (Assarsson & Möller 2000). OUTSIDE and INSIDE are
  exact; INTERSECTS may be reported for a box that is actually outside near
  a frustum edge, which is the accepted trade-off for culling.

## Packing (`pack.h`)

Compact encodings that follow the Vulkan conversion rules, so what the CPU
writes is what the GPU reads back.

- **binary16**: `nph_f32_to_f16` rounds to nearest even with correct
  subnormals, overflow to infinity, and quiet NaN; `nph_f16_to_f32` is
  exact. Array and 2/4-wide packed forms. Tested exhaustively (every half
  round-trips) and against an independent binary-search oracle over a
  million random bit patterns.
- **unorm/snorm 8/16**: Vulkan §3.9 rounding; both most-negative snorm
  codes decode to −1. Packed RGBA8, snorm8×4, unorm16×2, snorm16×2 with
  the first component in the low bits.
- **Octahedral unit vectors** (Meyer et al. 2010; Cigolle et al., JCGT
  2014): `nph_oct_encode/decode` to `[−1, 1]²`, plus snorm16×2 (32-bit,
  < 0.006° error) and snorm8×2 (16-bit, < 0.7°) quantised forms. The axes
  round-trip exactly.
- **R11G11B10F** (`VK_FORMAT_B10G11R11_UFLOAT_PACK32`) via binary16 with a
  second round-to-nearest-even to 6 or 5 mantissa bits; negatives clamp to
  0, infinity and NaN survive, overflow saturates to infinity.
- **RGB9E5** (`VK_FORMAT_E5B9G9R9_UFLOAT_PACK32`): exactly the
  `EXT_texture_shared_exponent` algorithm, including the mantissa-overflow
  exponent bump and the 65408 clamp.

## Random (`random.h`)

Every generator is a pure function of explicit state or index: the same
seed gives the same stream on every platform (§2 determinism).

- **PCG32** (O'Neill 2014; XSH-RR on a 64-bit LCG): seeding identical to
  `pcg32_srandom_r` (verified against the reference outputs for seed 42 /
  stream 54), `u32`/`u64`, unbiased bounded integers (Lemire 2019, nearly
  divisionless), `f32` in [0, 1) from 24 bits (exactly representable, never
  1), `f64` from 53 bits, and O(log n) `advance` (Brown 1994) so jobs can
  carve disjoint sub-streams from one seed.
- **Low-discrepancy**: base-2 radical inverse by bit reversal, general-base
  radical inverse (Halton), the (2, 3) Halton pair, and Roberts's R1/R2
  additive recurrences (golden ratio, plastic constant) for per-pixel
  jitter.
- **Sample warps**: Shirley–Chiu concentric disk, uniform sphere and
  hemisphere, cosine-weighted hemisphere (Malley), uniform triangle
  barycentrics, with the matching pdfs.
- **Blue noise**: `nph_blue_noise_generate` implements Ulichney's
  void-and-cluster method (SPIE 1993) with a toroidal Gaussian, producing
  a rank permutation for dither and sample textures. O(N²) time and 14
  bytes of scratch per texel from the caller's allocator; a cooker-side
  tool. Tests check the permutation, determinism per seed, and that the
  early ranks are spaced near the ideal `sqrt(N/k)` where white noise is
  not.

## SIMD register layer (`simd.h`)

`NphF32x4` is one SSE register. Following DirectXMath, the one register type
serves vectors, points, quaternions, and planes; the operation name says
which semantics apply (`nph_f32x4_dot3`, `nph_f32x4_quat_mul`,
`nph_f32x4_plane_distance`). Values enter and leave through explicit
`nph_f32x4_load_*` / `nph_f32x4_store_*` functions that take the storage
types and use unaligned loads, so storage types keep natural alignment.

- Lane construction and access, shuffles (`NPH_F32X4_SHUFFLE`), arithmetic,
  min/max, sqrt, rounding (`floor`, `ceil`, `round_even`, `trunc`),
  comparisons producing all-ones masks, `select`, `movemask`, integer
  conversion.
- Vector semantics: `dot3`/`dot4` (splatted and scalar), `cross3`,
  lengths, `normalize3`/`normalize4`; quaternion product, conjugate, and
  rotation; plane distance.
- `NphSimdMat4` (four column registers): product, vector and point
  transforms, transpose, and a general inverse by Zhang's 2×2 block method
  ("Fast 4x4 Matrix Inverse with SSE SIMD, Explained", 2016), which runs on
  our column-major matrices without a transpose because inverting the
  transpose yields the columns of the inverse.
- `NphSimdAffine`: the storage type keeps rows for the GPU, but in registers
  the linear part is held as three columns (w = 0) plus the translation
  column (w = 1). A point transform is then three multiply-adds with no
  horizontal sums, and load/store transpose once.

Every operation uses correctly rounded instructions and performs the same
arithmetic in the same order as its scalar twin, so `test_simd.c` asserts
**bit-identical** results for add, sub, mul, div, min, max, sqrt, rounding,
dot, cross, normalize, quaternion product and rotation, matrix product and
transforms, and affine product and transforms. The approximate `rsqrt`/`rcp`
instructions are exposed only as `*_fast`, differ between CPU vendors, and
are tested to a tolerance (ADR-0008 §6).

## Wide SoA types (`wide.h`)

`NphVec3x4` and `NphVec3x8` hold four or eight independent vectors with the
x, y, and z components in separate registers. `NphF32x8` is one AVX
register in translation units compiled with AVX (`NPH_WIDE_AVX`) and a
32-byte-aligned pair of SSE registers elsewhere, so the same inline code
compiles to either ISA. Operations: SoA load/store, AoS↔SoA transposes
(five shuffles for four `NphVec3`), arithmetic, dot, cross, normalize,
lerp, select, and affine point/vector transforms that match
`nph_affine_transform_*` bit for bit. `nph_frustum_cull_aabb_x4/x8` and
`nph_frustum_cull_sphere_x4/x8` classify four or eight volumes per call
with the formula `n·c + |n|·e + d ≥ 0` per plane, in a fixed operation
order, over all six plane slots (an infinite frustum carries an always-true
sixth plane, so there is no branch on the count).

## Batch kernels and ISA dispatch (`kernels.h`)

`engine/math/src/kernels_impl.h` holds the kernel bodies once; they are
compiled into `kernels_sse4.c` at the baseline and into `kernels_avx2.c`
with `/arch:AVX2` or `-mavx2 -mfma -mf16c` (`nph_set_avx2_sources` in
`cmake/NphArch.cmake`). `kernels.c` holds the scalar reference kernels and
the dispatch. `nph_simd_detect_level` reads CPUID and XGETBV through
compiler intrinsics (SSE4.2, AVX, AVX2, FMA, F16C, and the OS's AVX state
support) and `nph_math_kernels(level)` returns an immutable table:

| Kernel | Does |
|---|---|
| `frustum_cull_aabbs` / `frustum_cull_spheres` | one visibility bit per volume into `u64` words |
| `transform_points` / `transform_vectors` | `NphAffine` applied to an array, in place allowed |
| `f32_to_f16` | binary16 conversion; F16C on AVX2 |

Kernels take and return plain memory, so no wide value crosses between
units compiled at different ISA levels, and every level produces
bit-identical outputs for the same inputs, which `test_kernels.c` checks
for counts of 0, 1, 7, 8, 9, 63, 64, 65, 100, 1000, and 1031 (full and
partial groups, padding bits cleared). Boxes are gathered with the AoS
transposes plus one shuffle per component to separate min from max;
spheres (16 bytes each) with a 4×4 register transpose. The engine picks the
table once at start-up; dispatch never happens per element.

## Benchmarks

`benchmarks/math/bench_math.c`, Profile, Ryzen 7 5800X, MSVC 14.44,
2026-10-12, median of 5
(`benchmarks/results/math-2026-10-12-win-msvc-profile-ryzen7-5800x.json`).
Batch entries are per element over 65 536 elements.

| Benchmark | scalar | SSE4 register / kernel | AVX2 kernel |
|---|---|---|---|
| `mat4_mul` | 29.4 ns | 5.2 ns (5.6×) | |
| `mat4_inverse` | 25.8 ns | 18.6 ns (1.4×) | |
| `quat_mul`, dependent chain | 4.9 ns | 5.2 ns (parity) | |
| `affine_point`, dependent chain | 4.8 ns | 5.0 ns (parity) | |
| `transform_points` | 2.03 ns | 0.80 ns (2.5×) | 0.61 ns (3.3×) |
| `cull_aabbs` | 11.5 ns | 3.3 ns (3.5×) | 2.2 ns (5.1×) |
| `cull_spheres` | 8.3 ns | 1.37 ns (6.0×) | 1.14 ns (7.3×) |
| `f32_to_f16` | 4.4 ns | 4.4 ns (scalar) | 0.06 ns (F16C, 76×) |
| `pcg32_next_u32` | 1.9 ns | | |
| `pcg32_bounded` | 3.5 ns | | |
| `halton_2d` | 23 ns | | |
| `r2` | 4.9 ns | | |
| `oct_encode_snorm16` | 49 ns | | |
| `blue_noise_32x32` | 1.34 ms | | |

Reading: the SIMD paths beat scalar wherever there is parallel work (Phase 2
exit criterion). A single quaternion product or point transform in a
dependent chain is latency-bound and the scalar code already compiles to
SSE, so the register form is at parity there; the gain for per-object work
comes from batching into the SoA kernels. `cull_spheres` gains more than
`cull_aabbs` because a sphere is exactly one register and transposes
cheaply. The first benchmark run had the AVX2 cull no faster than SSE4 and
the register affine slower than scalar; the shuffle gathers and the column
register layout fixed both, and the bit-exactness tests were unchanged by
either. `oct_encode_snorm16` is dominated by `roundf` and the normalise; the
cooker will batch it.

## Testing

`engine/math/tests/` uses `test_math_support.h`: a splitmix64 generator
(independent of the engine's own PRNG, which is itself under test) for
uniform scalars, vectors, unit vectors (Marsaglia), unit quaternions
(Shoemake), well-conditioned TRS and general matrices, plus assertion macros
that print both operands. Every property test runs 1 000–2 000 random cases
from a fixed seed. 113 tests in all. Tolerances are stated per property: exact (0 ULP) where
the arithmetic is identical, 1–2 ULP for normalisation, and absolute
tolerances of 1e-4 to 1e-5 for products of several operations.

Properties covered so far: cross-product identities (Lagrange), projection
plus rejection reconstructs the input, reflection preserves length, bases
are right-handed and orthonormal; quaternion algebra (associativity, norm,
inverse), rotation agrees with the matrix form, `mul` composes in the
documented order, axis-angle and Euler round trips, `from_to` lands on the
target with the minimal angle, slerp has constant angular speed and takes
the shorter arc, swing·twist reproduces the input with the twist about the
axis; matrix inverses satisfy `M⁻¹M ≈ I` for general and TRS matrices and
refuse singular input without touching the output, determinants multiply,
`quat_from_mat3` exercises all four branches, TRS compose/decompose round
trips including reflections; the view matrix sends the eye to the origin
and the target onto −Z; the projections put the near plane at depth 1, the
far plane (or infinity) at 0, decrease monotonically, and flip Y; the affine
type agrees with `NphMat4` on every operation.

SIMD and kernels: `test_simd.c` compares every register operation with its
scalar twin bit for bit (tolerances only for the fast approximations and
the differently ordered inverse); `test_wide.c` does the same for the
eight-lane fallback and both SoA types, including the AoS transposes and
the wide frustum tests against the scalar kernel; `test_kernels.c` runs the
SSE4 and (when the CPU has it) AVX2 tables against the scalar table on
partial and full groups, checks padding bits and in-place transforms, and
confirms the scalar kernel agrees with `geometry.h` away from plane
boundaries. GitHub's hosted runners have AVX2, so the AVX2 unit is tested
in CI.
