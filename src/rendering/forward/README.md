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
asserted on with no device, no shaders and no draws, which is exactly what `unit.render_forward` does.

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

The refusal is stated to the graph too (M11.d task 5.1): the stage declares
`PassBuilder::single_sample(reason)` with the same words `build()` refuses a multisampled frame with,
so a pass author who asks the declared stage for a sample count is refused by the graph with that
reason rather than handed a multisampled twin nothing could resolve.

## MSAA: the graph's model, and what this frame still declares itself

Since M11.d the render graph owns MSAA: a pass declares `multisample(n)` before its attachments and
`resolve(target)` where its colour is final, and the graph creates the multisampled twin, redirects
the pass's attachment uses to it, inserts the resolve pass and refuses a frame whose resolve is
missing or misplaced (`src/rendering/graph/src/multisample.cpp`). **This frame does not use it yet**:
`FrameFeatures::msaa_samples` still declares its own `colour (msaa)` and `depth (msaa)` targets and a
`resolve` stage whose recording is the caller's, and `pipeline::FrameRecorder` still refuses a
multisampled frame. Moving the frame onto the graph's model is a change to `frame.cpp`'s shading and
post-chain declarations, and it is deliberately not made while other work edits those declarations;
depth resolve also has no RHI resolve mode yet, and the graph refuses a depth `resolve()` by name.

## A stage whose producer declares its own passes: ambient occlusion

`FrameStageDeclaration` is the seam. A screen-space stage that needs more than one pass — ambient
occlusion is a horizon search and a three-pass filter cascade, and a barrier between two dispatches
can only come from the graph — is handed to its producer at the stage's own position in the order,
with the depth, the normal target and the stage's target (`ScreenSpaceStageInputs`). The producer
declares its passes and returns the first; a producer that refuses fails the build rather than
leaving the stage out. `FrameDescription::ambient_occlusion_target` lets the producer own the
target, because the forward pass samples it through a texture-table slot. Without a producer the
stage is the one pass it always was. `src/rendering/occlusion/` is the producer.

## A second produced stage: contact shadows

`FramePassKind::ContactShadows` is one of stage 4's screen-space passes, after ambient occlusion and
before the opaque pass that reads it. It is `virtual-shadows`' "Contact and traced refinement" and
not one of the specification's thirteen, and it is declared the way ambient occlusion is: the frame
hands `ScreenSpaceStageInputs` to `FrameDescription::contact_shadows_stage` and reads the imported
`contact_shadows_target` in the opaque pass. Unlike ambient occlusion there is no single-pass
fallback — `build()` refuses `FrameFeatures::contact_shadows` without a producer and a target,
because the opaque pass would sample an image nothing wrote. The feature derives the `DepthNormal`
prepass, whose normal lifts the trace off its surface. `src/rendering/contact_shadows/` is the
producer.

## A third, after the tone curve: selection outlines

`FramePassKind::SelectionOutlines` sits after the post-process and before the interface: an outline
is display-referred, so it is drawn where the colour a game asks for is the colour in the output,
and the interface is drawn over it. The frame hands `ScreenSpaceStageInputs` to
`FrameDescription::selection_outlines_stage` with `target` set to the colour the chain ended in — the
output, when post-processing tonemapped into it — and `draw_instances` set to the frame's draw
records, because the producer draws the marked objects again and must declare that read. Its last
pass reads and writes the target, so the interface loads the outlined colour. `build()` refuses the
feature without a producer, and without the depth prepass: a marked surface is found hidden by
comparing it with the prepass depth. `src/rendering/selection/` is the producer.

## A fourth, in the post chain: depth of field

`FramePassKind::DepthOfField` is step 7 of `rendering-post-processing`'s chain: after the temporal
resolve, so it blurs a converged image, and before bloom, so a defocused highlight blooms as the
disc it became. It is the first produced stage that writes a NEW colour rather than a term another
pass samples: the frame creates `FrameResources::depth_of_field` (full resolution, the scene
colour's format, storage-writable) and hands it to `FrameDescription::depth_of_field_stage` as
`target`, with the colour the chain has reached as `ScreenSpaceStageInputs::source` — appended, as
`draw_instances` was. Every later stage, bloom and the post-process included, reads the target.
`build()` refuses the feature without a producer, and on a multisampled frame without the prepass,
whose depth resolve is the only single-sample depth such a frame has; a single-sample frame without
the prepass reads the depth its opaque pass wrote. `src/rendering/depth_of_field/` is the producer.

## Why the cluster assignment exists twice

The specification requires it to run as a compute pass, and `frame.h` declares one. The C++ version in
`cluster.cpp` is the null backend's answer, the reference the compute pass is checked against, and the
one place the specification's numbers live — the 60° spot threshold, the bounded per-cluster count,
deterministic nearest-kept dropping, the camera-inside-a-light case. The cost is that two
implementations of one algorithm can drift; the mitigation is that this one is the reference.

**Tile row 0 is the top of the image.** `cy/cluster.slang`'s `clusterCoordOf` takes a fragment's
tile from its pixel row, and after the Vulkan viewport's Y flip pixel row 0 is view-space up, so
`cluster_bounds` maps row 0 to normalised +Y. It used to map it to -Y, which mirrored every list
against the lookup. `render.pipeline` and `render.golden` pass under either convention; a floor
decal read the lists of the sky above it and drew nothing, which is how it was found. `unit.render_forward`'s "tile row 0 is the top of the image" pins it.

## Why the sort exists twice

`render::sort_draws` (layer 2) is a comparison sort over the total order `(key, stable_id, surface)`
and is obviously correct. `radix_sort_draws` here is linear. `unit.render_forward` sorts one list both
ways and asserts the results are byte-identical, which is a stronger statement than either alone.
