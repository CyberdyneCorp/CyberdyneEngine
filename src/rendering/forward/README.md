# `src/rendering/forward/` — layer 4

The clustered forward frame: the cluster grid and its light assignment, the depth prepass, the sorted
draw list, the pass order, and the pipeline diagnostics.

**Governed by**: `rendering-forward-clustered`, at **Working** for M3. Tasks 4.3.1 through 4.3.4.

## The files

| File | What it holds |
|---|---|
| `cluster.h` | the grid, its exponential slicing constants, and the CPU reference assignment |
| `draw_list.h` | the sort key's inputs, a radix sort, and the per-draw `GpuDrawInstance` record |
| `frame.h` | the thirteen stages, declared into the render graph with their reads and writes |
| `diagnostics.h` | cluster occupancy, per-pass draws and time, and whether the sort grouped what it promised to |

## This module declares; it does not record

Every pass is declared with its reads and its writes and a record callback the **caller** supplies.
`ForwardFrame` knows the frame's structure — what depends on what — and the caller knows how to draw.
Neither can write a barrier, because there is nowhere in either interface for one.

The consequence is what makes the pass order testable: a pass with no callback still declares its
resources, and the graph still derives every barrier around it. So a frame is built, compiled and
asserted on with no device, no shaders and no draws, which is exactly what `unit.forward_frame` does.

## Two things that are derived rather than set

**The prepass mode** is a function of the feature set (`select_prepass_mode`), so a velocity target
cannot exist with nothing writing it, and a normal buffer cannot be read by an effect nobody enabled.

**A disabled feature is a pass that was never declared**, so its target was never created. "Their
targets unallocated" is observable — `FrameResources` holds `kInvalidResource` — rather than asserted.

## One stage that is not the specification's: virtual geometry

M11.c task 4.3 added `FramePassKind::VirtualGeometry`, declared only when
`FrameFeatures::virtual_geometry` is on. It sits after the depth prepass (and would sit after its MSAA
resolve, except that a frame with virtual geometry refuses MSAA) and before every stage that reads
depth, because it WRITES the frame's own depth: a mesh the prepass drew and a cluster occlude one
another through one buffer. It also writes an `R32Uint` visibility target, `FrameResources::visibility`.

This module still records nothing for it. `vg::ForwardVisibility` in
`src/rendering/virtual_geometry/` supplies the callback, which is why that module links this one and
not the other way round. Its callback draws indirectly and pulls vertices out of storage buffers, so
`FramePassCallback` grew a general `reads` list beside `vertex_reads`: each entry is a resource and a
READING intent, and the graph derives the dependency from it. `unit.render_forward` asserts the
stage's place, its targets, a declared read producing a dependency, and the MSAA refusal.

## Why the cluster assignment exists twice

The specification requires it to run as a compute pass, and `frame.h` declares one. The C++ version in
`cluster.cpp` is the null backend's answer, the reference the compute pass is checked against, and the
one place the specification's numbers live — the 60° spot threshold, the bounded per-cluster count,
deterministic nearest-kept dropping, the camera-inside-a-light case. The cost is that two
implementations of one algorithm can drift; the mitigation is that this one is the reference.

## Why the sort exists twice

`render::sort_draws` (layer 2) is a comparison sort over the total order `(key, stable_id, surface)`
and is obviously correct. `radix_sort_draws` here is linear. `unit.forward_frame` sorts one list both
ways and asserts the results are byte-identical, which is a stronger statement than either alone.
