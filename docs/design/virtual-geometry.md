# Virtual geometry, rendered

> Regenerate every image here with `just capture-virtual-geometry`. It renders
> `samples/07-fidelity`'s own scene through that sample's own frame, so there is one scene and one
> renderer to keep true rather than a second copy that drifts.

`virtual-geometry` reached **Working** at M7, and for two milestones the only committed picture of
it was a chart — because at M7 nothing in this engine could turn a frame into a file. M8.c built
that layer. These are the frames.

## The scene

One film-detail interior, cooked through the real builder with the crack-free check the cook is
required to run, put on the device through the cluster traversal and the visibility buffer, at
1280x720 on Vulkan with validation on.

| Figure | Value |
|---|---|
| Source triangles (every instance counted) | **4,478,208** |
| Distinct triangles cooked | 116,928 |
| Clusters / geometry pages | 1,915 / 31 |
| Covered pixels | 921,593 of 921,600 |
| Source triangles per covered pixel | **≈ 4.9** |

The last row is the point of the technique: there is more geometry in the set than there are pixels
to show it in, so the question is never "draw it all" but "which clusters, at which error".

![The frame](images/virtual-geometry-shaded.png)

*The shaded result, not a debug view. Fluted left wall, three displaced shells on the ceiling, a
corrugated back wall, a striped floor — all of it real geometry rather than a normal map, which is
what makes the triangle count above meaningful. The sun is shadowed through `virtual-shadows`' own
page cache: 132 pages allocated and rasterised for this frame, 535,027 shadow texels written, and
412,647 of the 921,593 covered pixels in shadow with 20,061 lit and no lookup falling back.*

**This picture is asserted, not merely published.** `render.virtual_geometry_shaded` renders the
same frame — the same scene, the same shot, through the same `shade_frame()` this capture calls —
at 320x180 and compares it against `tests/render/references/virtual_geometry_shaded.png` within
`tests/render/golden.h`'s derived tolerance. On this machine the frame is bit-identical across
processes, so the case asserts `differing == 0` as well as the tolerance. A second case starves
the shadow page cache to one physical slot and asserts the frame CHANGES, which is what stops a
reference that had quietly stopped sampling the shadow map from passing forever. Regenerating the
picture above with `just capture-virtual-geometry` does not regenerate that reference.

## The hierarchy choosing

A still frame cannot show level-of-detail selection; only the **same view at different error
budgets** can. `threshold_pixels` is the screen-space geometric error the hierarchy is allowed to
commit, and it is the only thing changed between these three.

| Threshold | Visible cluster instances | Covered pixels |
|---|---|---|
| 1 px | **5,247** | 921,593 |
| 4 px | **3,332** | 921,592 |
| 16 px | **1,878** | 921,592 |

| 1 px | 4 px | 16 px |
|---|---|---|
| ![1px](images/virtual-geometry-clusters-1px.png) | ![4px](images/virtual-geometry-clusters-4px.png) | ![16px](images/virtual-geometry-clusters-16px.png) |

One colour per cluster. Three things are visible at once, and each is a claim the specification
makes:

- **The patches grow.** The ceiling shells go from finely speckled to broad plates. Nobody authored
  a level of detail; the hierarchy descended less far because it was allowed more error.
- **Coverage does not move.** 921,592 pixels at 16 px error as at 1 px. Coarsening does not open
  holes between neighbouring clusters — that is the crack-free property, and it is what a naive
  per-cluster decimation fails.
- **Visible instances exceed cooked clusters.** 5,247 visible against 1,915 cooked is not a
  contradiction: a cooked cluster is *instanced*, drawn at many placements, and the traversal picks
  a level per placement. That is why 116,928 distinct triangles can present 4,478,208.

## Triangle density

![Triangles](images/virtual-geometry-triangles.png)

*One colour per triangle, at the 1 px threshold. Where this reads as noise rather than as facets,
the triangles have reached pixel size — which is the regime the whole cluster hierarchy exists to
make affordable.*

## These images are reproducible exactly

Three identical runs of `cy_fidelity_capture` at 1280x720, threshold 1 px, compared pixel for pixel:

| View | Differing pixels of 921,600 |
|---|---|
| shaded — the resolved world normal | **0** |
| clusters — one colour per cluster | **0** |
| triangles — one colour per triangle | **0** |

That took two fixes, and both were found by making these pictures rather than by a gate.

**First, depth and the payload were two unordered writes.** `vgVisRaster` settled depth with an
atomic and then stored `visbuffer[pixel]` separately, so a *farther* surface could take a pixel
simply by storing last: **110 to 150 pixels** moved between identical runs. They are one 64-bit
atomic minimum now — the depth key in the high half, the payload in the low half.

**Then an exact tie was still broken by append order.** Where two surfaces tie on the depth key
exactly, the 64-bit minimum is decided by the payload alone, and the payload carried the index of
the traversal's visible-cluster record — the slot an `InterlockedAdd` handed out, which permutes run
to run. That left **one pixel** moving in the shaded view and **two to five** in the triangle view,
and it repainted **883,921 to 886,414 pixels** of the cluster view, because the number a pixel held
named the same cluster differently on each run.

The payload's first word is now `instance * cluster_stride + cluster` — the traversal's own DAG mark
index, a pure function of the scene — and every pass after the raster derives the material from it
instead of indexing that list. An exact tie goes to the lower instance, and to the lower cluster
within it, on every run.

[`test_visbuffer.cpp`](../../src/rendering/virtual_geometry/tests/test_visbuffer.cpp) holds both:
one case renders a stack of overlapping instances six times and requires the resolve and the bin
counts to match, and one builds four *coincident pairs* of instances — every fragment tying exactly
with its twin's — and requires the visibility buffer itself to be bit-identical across six runs,
with the lower-identity twin winning every contested pixel. Against the old raster the second
reports 605 to 714 pixels won by the wrong twin and 1,825 to 2,001 visibility samples differing
between runs.

The cost is a bound rather than a cost in pixels: the payload gives the identity 24 bits, so a scene
needs `instance_count * cluster_stride` at or below 2^24 — four times tighter than the 2^26 the
traversal's own visit marks allow. `VisbufferPass::initialise` refuses above it by name.
