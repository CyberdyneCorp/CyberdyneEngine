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
| `geometry.h` | Ray casts, closest points, hulls (2D and 3D), triangulation (ear clipping and Delaunay), polygon booleans and offsetting, plane clipping, atlas packing, mesh preparation. |
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

## The four geometry algorithms that have degenerate cases

`core-math`'s "Geometry utilities" names a long list, and most of it is a formula with no failure
mode: a ray against a box, the closest point on a triangle, twice a polygon's signed area. Four are
not, and they live one algorithm to a source file — `src/hull3d.cpp`, `src/delaunay.cpp`,
`src/polygon_boolean.cpp`, `src/polygon_offset.cpp` — because each carries a robustness argument
that a reader should be able to find without searching a nine-hundred-line file.

| | Algorithm | What it refuses, and why that is the interesting half |
|---|---|---|
| `convex_hull_3d` | Incremental face stitching (quickhull's construction without its recursion) | A cloud with no volume — coincident, collinear or coplanar within the tolerance. A flat set has a 2D hull in a plane, not a zero-thickness 3D shell that every consumer would then special-case. |
| `triangulate_delaunay` | Bowyer–Watson | Duplicate points (no Delaunay answer exists, only an arbitrary one — weld first) and a collinear set (no triangulation at all, which is not the same as an empty one). |
| `polygon_boolean` | Greiner–Hormann | A boundary that touches without crossing, and a difference that would be an annulus. Both are `Unsupported` with a message naming what was found. |
| `offset_polygon` | Per-corner miter with a limit and a bevel fallback | Clockwise input, because the sign of the offset must mean one thing; and a repeated vertex, which leaves an edge with no direction. |

Two decisions run through all four:

* **The predicates are computed in `f64` from `f32` inputs.** Every one of them turns on the sign of
  a difference of products of nearly equal coordinates — a signed distance to a face, an incircle
  determinant, a crossing parameter — and at engine scale the `f32` answer is dominated by rounding
  error exactly where the answer matters. The failure is never "slightly wrong": a face wrongly
  judged visible opens a hole the horizon walk then stitches across the hull's interior. The
  interfaces stay `f32` because `core-math` fixes runtime precision there; only the arithmetic
  between them widens.
* **Iteration order is defined.** Where a hash set would have been the obvious container for the
  horizon edges or a cavity boundary, these use a sorted vector and a binary search: the same cost
  at these sizes, and an output order that does not depend on which standard library built it. A
  cook step whose output hash differs between two machines is the divergence
  `simulation-and-determinism` exists to prevent.

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

## AVX2 is compiled and is not used, and that is a measurement

`core-math` names SSE4.2, AVX2, NEON and a portable fallback. All four are here. `Avx2Ops` is a real
256-bit backend — eight lanes, the full primitive set, no fused multiply-add so that it stays
bit-identical — and `tests/test_simd.cpp` checks it lane for lane against the reference in any build
that targets AVX2 (`cmake -B build/x -DCMAKE_CXX_FLAGS=-mavx2`, which is how it was verified here).

**No batch function calls it.** An eight-lane array transform was written, tested bit-identical at
every length from 0 to 16, and measured against the four-wide loop in the same `-mavx2` build:

| | four-wide | eight-wide |
|---|---|---|
| `transform_points` | 0.572 ns/point | 0.544 ns/point |
| `transform_directions` | 0.518 ns/point | 0.557 ns/point |

Medians of five paired runs of 100 000 points, GCC 13 at `-O2`, best-of-25 within each run. Faster
at one, slower at the other, both by less than the spread between runs — a wash, and a wash does not
pay for a second loop shape with its own tail case in the module's most alias-sensitive code.

The reason is worth keeping, because it will be true of the next such loop: the transform moves 24
bytes per point and does nine multiply-adds on them, so it is store-bound long before it is
arithmetic-bound. And the trick a wide loop needs — one `vpermps` to spread a point's x across four
lanes — competes for the shuffle port, while the four-wide loop's `vbroadcastss` reads straight from
memory on a load port the loop is not saturating. Twice the width, the same throughput.

So `simd::backend_compiled(Backend::Avx2)` and `simd::active_backend()` answer different questions
and, in an AVX2 build, give different answers: the binary contains the 256-bit backend, and the
batch loops run the 128-bit one. The first arithmetic-bound consumer — skinning, or culling over
bounds already stored as component arrays rather than as `Aabb`s — is where that should be
re-measured, in either direction, rather than assumed.

**A CI gap this leaves, which is not this module's to close:** no configuration under
`.github/workflows/` targets AVX2, so the 256-bit backend is compiled by nobody on the way in and
will bit-rot the way NEON would. One job configuring with `-DCMAKE_CXX_FLAGS=-mavx2` and running the
math suites is enough, and it belongs with whoever owns the workflow files.

## What is not here, and is therefore not claimed

* **Constrained Delaunay, and Delaunay in 3D.** `triangulate_delaunay` triangulates a point set in
  the plane, and there is no way to require that a particular edge survives — which a navigation
  mesh built from a floor plan would want. That is a materially larger algorithm and nothing in this
  milestone needs it.
* **Polygon booleans through a degeneracy, or with holes.** `polygon_boolean` refuses two cases by
  name rather than guessing: a boundary that touches without crossing (a shared vertex, a vertex
  lying on an edge, two collinear overlapping edges), and a difference whose result is an annulus,
  which this interface cannot express. Both come back as `ErrorCode::Unsupported` with a message
  naming what was found. Greiner–Hormann is exact in general position and undefined in those cases;
  the published extensions that handle them are a large amount of case analysis, and a boolean that
  silently drops a contour is the most expensive kind of bug in this file — the caller cannot tell
  it from a correct answer.
* **Offset polygons are not de-looped.** Shrink past the radius of the narrowest neck and the offset
  ring crosses itself. Every vertex returned is individually correct; the ring is not simple.
  Removing the loops is a boolean union of the offset segments' swept regions, an algorithm an order
  of magnitude larger, and its answer is a *set* of contours rather than one.
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
| `unit.math` | unit | The conventions, the types, the geometry utilities, the polygon operations, the curves and the easing table. |
| `integration.math_simd` | integration | Every primitive on every compiled backend against the reference, the batch operations bit-for-bit, and the ten-thousand-point bulk transform `core-math` names. |
| `integration.math_spatial` | integration | Both BVHs, the spatial hash and the octree, every query compared against brute force **in the direction that matters**: the accelerated answer must be a superset of the exact one. |
| `integration.math_random` | integration | Reproducibility, stream independence, snapshot and restore, and the distributions. |
| `integration.math_geometry` | integration | The 3D hull, the Delaunay triangulation, the booleans and the offset against their **defining properties** over hundreds of random shapes: every point behind every hull face, no point inside any circumcircle, the triangles tiling the hull exactly, area(A ∪ B) + area(A ∩ B) = area(A) + area(B), and every offset edge exactly its distance from the edge it came from. |

The split is about cost, not subject: a unit test has a millisecond, and a suite that sweeps
thousands of values or measures a distribution does not fit in one.

Run in all four profiles — a convention test that only holds in `dev` is not a convention — under
both GCC 13 and Clang 18, with `CY_MATH_FORCE_SCALAR` as a fifth configuration, `-mavx2` as a sixth,
and under ASan and UBSan.
