# `src/servers/render/culling/` — layer 2

GPU-driven culling's records, the hierarchical depth buffer, and the CPU reference that says what a
compute dispatch must compute.

**Governed by**: `rendering-culling-and-lod`, reaching **Working** at M6. Task 8.5.

## What is here, and what is in `src/rendering/culling/`

This capability lives in two places, at two layers, and the split is not arbitrary:

| | `src/rendering/culling/` (layer 4) | here (layer 2) |
|---|---|---|
| Landed at | M3, at Seed | M6, at Working |
| Holds | a `SpatialIndex` of `DynamicBvh` trees, a `jobs::JobSystem`, per-view CPU visible lists | the layouts a compute dispatch reads and writes |
| Reads | the scene it is given | the GPU scene the render server publishes |
| Produces | `CullResults` for a frame being assembled above it | indirect draw arguments and counters |

A GPU cull's input record is `render::GpuInstance`, which is at layer 2 for the reasons
`gpu_scene.h` gives. A module that consumed it from layer 4 and published a layout the backends then
had to read would be the wrong way up.

## The files

| File | What it holds |
|---|---|
| `gpu_cull.h` | `GpuCullView`, `GpuMeshLod`, `GpuVisibilityRange`, the indirect argument and payload records, `GpuCullCounters`, and `cpu_reference_cull()` |
| `hzb.h` | the depth pyramid's shape and its test, the visibility hysteresis, and the two-pass scheme's state |

## Five things worth knowing before changing anything here

**The reference is the test, not the fallback.** `cpu_reference_cull` runs the whole algorithm
headless, so the compute shader that lands with the pass is checked against a reference by comparing
buffers rather than by looking at a picture. It also happens to be the CPU path the specification
requires "for cases needing CPU visibility results".

**Reversed Z means the furthest depth is the smallest.** The pyramid keeps the furthest depth of each
footprint, and an instance is occluded when its nearest depth is smaller than that. Getting the
inequality backwards produces a renderer that culls what is visible and draws what is not, which
reads as a broken pyramid rather than as a flipped comparison.

**False occlusion is a correctness bug.** `Hzb::invalidate()` for a camera cut, `VisibilityHistory`
for reprojection error, and `TwoPassCull` for disocclusion. Every case where the answer is not known
— an invalid pyramid, a sphere straddling the near plane, a rectangle off the buffer — reads as
"draw it".

**A shadow view carries two frustums.** `planes` is the shadow projection's and `camera_planes` the
camera's, because the requirement asks two different questions: is the caster inside the light's
volume, and can it cast into the frustum the shadows will be seen in. One frustum cannot answer both.

**A visibility parent is stored plus one.** A zeroed record must mean "no parent", and storing the
slot directly would make every instance that declared no hierarchy claim slot 0 as its parent — which
is a real instance.

## What is deliberately absent

Cluster-granular culling for virtual geometry, which is `virtual-geometry` at M7. This cull already
routes a `kInstanceVirtualGeometry` instance into a separate output list rather than emitting an
indexed draw for it, so the traversal that lands then consumes a list that is already produced.
