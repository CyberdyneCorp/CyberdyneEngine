# `src/core/math/` — the mathematics layer

Layer 0, target `cy::core-math`. Section 3.1 of M1. Governed by `core-math`.

Everything that draws, moves, collides or is placed in the world is expressed in these types, so the
part of this module that matters most is not any single routine — it is the set of conventions every
other subsystem silently assumes. Those live in `tests/test_conventions.cpp` as executable
assertions rather than as prose, which is design.md §4's requirement and the reason this module was
written before the renderer rather than alongside it.

| Header | What it is |
|---|---|
| `scalar.h` | Constants, angle conversion, clamping and interpolation, and the fixed-step accumulator. The only two functions in the engine that may say "degrees". |
| `vec.h` | `Vec2`/`Vec3`/`Vec4` and their integer forms, plus `kAxisForward` and friends — the engine's basis as data. **`Vec3` is 12 bytes and is never padded.** |
| `quat.h` | `Quat` — the runtime rotation. Euler is an authoring boundary, order YXZ. |
| `matrix.h` | `Mat3`/`Mat4`, column-major with column vectors. `A * B` applies B first. |
| `transform.h` | `Transform` (decomposed TRS, canonical) and `Transform2D`. Matrices are derived from these, never the reverse. |
| `projection.h` | Reversed-Z projections, `look_at`, and `DepthConvention` — the depth rules as values a pipeline reads rather than as prose a pipeline author remembers. |
| `shapes.h` | `Rect`, `IRect`, `Aabb`, `Obb`, `Plane`, `Ray`, `Sphere`, `Frustum` with its sign masks. |
| `color.h` | Linear RGBA, and the sRGB boundary in the two named places it belongs. |
| `simd.h` | The scalar reference and the SIMD backends behind it, plus which ones this build contains. |
| `batch.h` | The array-wide operations: transform points, transform directions, cull AABBs. |
| `bvh.h` | `DynamicBvh` (incremental, fat AABBs) and `Bvh<T>` (static, binned SAH). |
| `spatial.h` | `SpatialHash` and `Octree`. |
| `geometry.h` | Ray casts, closest points, hulls, triangulation, plane clipping, atlas packing, mesh preparation. |
| `curve.h` | `Curve`, `Curve2D`, `Curve3D` with baked arc length, and `Gradient`. |
| `easing.h` | The one shared easing table: eleven families, four modes each. |
| `random.h` | `Random` — PCG32, explicit seeds and streams, and a state that can be recorded and restored. |
| `math.h` | The umbrella, and the conventions in one table. |

## The conventions, and why they are tests

| | |
|---|---|
| Handedness | Right-handed. `cross(kAxisX, kAxisY) == kAxisZ`. |
| Up / forward | +Y up; local **−Z forward**. A camera looks down its own −Z. |
| Rotation | Counter-clockwise about an axis seen from its positive end. Euler order YXZ, authoring only. |
| 2D screen | Origin top-left, +Y downward. |
| Matrices | Column-major storage, column vectors, `M * v`. Translation is the **fourth column**. `A * B` applies B first. |
| Depth | `[0, 1]`, **reversed**: near → 1, far → 0. Cleared to 0. Opaque compares `GreaterEqual`, shadow samplers `Greater`. |
| Units | Metres, seconds, kilograms, radians. |
| Precision | `f32` at runtime; `f64` only for the simulation clock, accumulated time and interchange. |

Each row has at least one test asserting its *numeric consequence*, not its wording: that
`perspective_reversed_z` maps the near plane to 1.0 and the far plane to 0.0; that
`look_at(origin, −Z)` is exactly the identity matrix; that `Mat4::from_translation({1,2,3}).at(0,3)`
is 1 while `at(3,0)` is 0; that `move * turn` and `turn * move` place a point in two visibly
different spots and which is which.

The reversed-Z half has a second, quieter consequence that is also asserted:
`Frustum::from_view_projection` extracts the **near** plane from the `z ≤ w` clip bound and the
**far** plane from `z ≥ 0`, which is the opposite assignment to a conventional-depth engine. Getting
it backwards produces a frustum that culls nothing near and everything far — a failure that looks
like a rendering bug and is a convention bug.

## SIMD: one reference, and everything measured against it

design.md §5. There is exactly one scalar reference implementation (`simd::reference`), it is
compiled into every build on every platform, and every SIMD path is compared against it in
`tests/test_simd.cpp`.

**The stated tolerance is zero.** Every operation in the abstraction is a single IEEE 754 operation
or a fixed sequence of them, so the comparison is `memcmp`, not an epsilon. Three things make that
possible and each would silently break it:

* **No fused multiply-add, in either path.** `madd` is a multiply and an add, rounding twice. An FMA
  rounds once and gives a different answer; a future FMA backend is welcome and must declare itself
  as a separate backend with a stated tolerance.
* **Floating-point contraction is disabled for this module** (`-ffp-contract=off`, see
  `CMakeLists.txt`). Without it the compiler may fuse in one path and not the other, and the failure
  would arrive on a compiler upgrade rather than on a code change.
* **`reference::min` and `max` use the hardware's selection rule** — the first operand when it
  compares less, the second otherwise — rather than the more natural "first on a tie". That makes
  the two agree bit-for-bit even on `+0.0` against `-0.0` and on NaN. This cost one test failure to
  discover, which is what the comparison exists for.

Selection is at **build time**. There is no CPUID dispatch: a build targets an instruction set and
`simd::active_backend()` reports which. `-DCY_MATH_FORCE_SCALAR` builds the reference alone, which
is how the scalar path is exercised on a machine that has SIMD, and the suite has been run that way.

## What is not here, and is therefore not claimed

* **AVX2.** `core-math` names it. `simd::backend_compiled(Backend::Avx2)` answers **false** rather
  than reporting the 128-bit path under a 256-bit name. An 8-wide path needs an eight-lane type; the
  batch functions are written over a backend tag so that adding one is a new `Ops` struct and a new
  instantiation rather than a rewrite.
* **3D convex hull, Delaunay triangulation, polygon boolean operations, polygon offsetting.** All
  four are named by `core-math` and none is implemented. The four that *are* here — 2D monotone-chain
  hull, ear-clipping triangulation, plane-set clipping, skyline atlas packing — are the ones this
  milestone's own consumers need. Each of the missing four is a substantial algorithm that deserves
  its own tests.
* **ARM/NEON is unverified.** The NEON backend is written and is structured identically to the SSE
  one so the diff between them is reviewable, but there is no ARM64 host in this milestone's
  environment and it has never been compiled or run. NEON's `FMIN`/`FMAX` use a different tie rule
  for signed zeros and NaN than x86, so the bit-identity guarantee above is expected to hold on
  ordinary values and to need a stated tolerance on those two. The first ARM CI run settles it.
  Windows and macOS are likewise unverified; the module adds no platform-conditional code beyond the
  contraction flag, and that one is explained where it is set.

## Seams other modules will cross

* **`core-memory-and-containers` (section 2).** `Curve`, `Curve3D`, `Gradient`, `SpatialHash`,
  `Octree` and both BVHs hold `std::vector` / `std::unordered_map`. When the allocator-aware
  containers of task 2.4 land, these are declarations to change and the interfaces do not move.
  Nothing on a query path allocates: the BVH traversals use a fixed stack and `SpatialHash`
  de-duplicates with a per-query stamp rather than a set, precisely so a query can run inside a job.
* **`core-type-system` (section 1.3).** `Var` already declares boundary shapes named `VarVec3`,
  `VarMat4`, `VarTransform` in `src/core/values/include/cy/core/values/payload.h`, deliberately
  spelled so they cannot be mistaken for these types. `var_payload_cast` carries a trivially
  copyable, same-sized math type across in one `memcpy`. Now that `cy::Vec3` and friends exist, a
  round trip is one call each way and no allocation, and nothing in either module has to change.
* **Reflection (section 1.1).** None of these types is annotated yet. They are the obvious first
  candidates for a reflected corpus larger than `reflect/`'s own two demo structs, and they are
  suitable: they are aggregates of scalars, standard-layout, and their headers are lean — which the
  generator's spike specifically asks reflected headers to be.
* **M3's renderer.** `DepthConvention` and `DepthCompareOp` are the pipeline's source of truth for
  clear value and compare op. The Vulkan viewport Y flip is deliberately *not* baked into the
  projection matrices: it is a viewport concern and belongs to the backend.
* **M9's determinism work.** The scalar reference and `RandomState` are what make a cross-platform
  comparison expressible at all. `Random::draws()` is compiled into every configuration, so a
  divergence has an answer to "which generator, and after how many draws".

## Testing

Four suites, declared from `tests/CMakeLists.txt`:

| Suite | Kind | What it covers |
|---|---|---|
| `unit.math` | unit | The conventions, the types, the geometry utilities, the curves and the easing table. |
| `integration.math_simd` | integration | Every primitive on every compiled backend against the reference, the batch operations bit-for-bit, and the ten-thousand-point bulk transform `core-math` names. |
| `integration.math_spatial` | integration | Both BVHs, the spatial hash and the octree, every query compared against brute force **in the direction that matters**: the accelerated answer must be a superset of the exact one. |
| `integration.math_random` | integration | Reproducibility, stream independence, snapshot and restore, and the distributions. |

The split is about cost, not subject: a unit test has a millisecond, and a suite that sweeps
thousands of values or measures a distribution does not fit in one.

Run in all four profiles — a convention test that only holds in `dev` is not a convention — under
both GCC 13 and Clang 18, with `CY_MATH_FORCE_SCALAR` as a fifth configuration, and under
ASan + UBSan.
