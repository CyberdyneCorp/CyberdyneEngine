# Temporal upscaling

How to render a frame below its output resolution and reconstruct it with the engine's temporal
upscaler, how dynamic resolution reaches it, where a vendor upscaler plugs in, and what to check
when a picture goes wrong. The contract is `rendering-post-processing`'s "Temporal upscaling" in
[`openspec/specs/`](../../openspec/specs/README.md); the module is
[`src/rendering/upscale/`](../../src/rendering/upscale/README.md).

## Contents

1. Turning it on in a frame
2. What runs at which extent
3. Dynamic resolution
4. A vendor upscaler
5. A program that draws its own geometry
6. Measuring and testing
7. Pitfalls
8. What is not built yet

## 1. Turning it on in a frame

Through `assembly::FrameAssembly`, which is how `samples/12-beauty` does it:

```cpp
using namespace cy::rendering;
const upscale::Extent2D output{2560, 1440};
const upscale::Extent2D render =
    upscale::render_extent_for(output, upscale::upscale_mode_scale(upscale::UpscaleMode::Quality));

assembly::AssemblyDescription description;
description.width = output.width;            // the OUTPUT: post chain, interface, temporal view
description.height = output.height;
description.render_width = render.width;     // every scene pass
description.render_height = render.height;
description.post.temporal_upscaling = true;
description.produced_temporal_stage = true;  // the upscaler owns the history
description.reactive_mask = true;            // with translucency or particles
description.temporal.jitter = upscale::jitter_config_for(output, render);
assembly.initialize(description);
assembly.attach_device(device);

upscale::TemporalUpscalePass pass;
upscale::UpscalePassDescription pass_description;
pass_description.output = output;
pass_description.depth_format = rhi::Format::D32Sfloat;
pass.create(allocator, device, pass_description);
pass.bind_temporal(assembly.temporal());     // declares its colour and state histories
```

Every frame, before `assemble`:

```cpp
upscale::UpscaleSettings settings;
settings.mode = upscale::UpscaleMode::Quality;
settings.exposure = std::exp2(exposure_stops);  // what the resolve multiplies by
pass.set_settings(settings);
sinks.temporal = upscale::stage_of(pass);
sinks.opaque_copy = pass.opaque_copy_stage();   // only with reactive_mask
```

`FrameAssembly::execute` tells the pass the frame ran, which is when it swaps its history. The
post-process reads `FrameResources::upscaled`; a caller that records its own resolve reads that
instead of `FrameResources::color`.

## 2. What runs at which extent

| Render extent | Output extent |
|---|---|
| depth and motion prepass, ambient occlusion, contact shadows, GI, volumetric fog, opaque, sky, translucency, the cluster grid, the decal grid | the reconstruction and its history, depth of field, motion blur, bloom, exposure, tonemap, grading, the selection outlines, the interface |

The stages after the upscaler that read depth or motion are handed `FrameResources::output_depth` and
`output_velocity`, which the upscaler's resample pass writes; they never see the render-extent
targets. The interface is therefore drawn at the output's own pixels and is byte-identical in every
mode.

## 3. Dynamic resolution

Internal resolution is the budget arbiter's allocation, not a controller of its own:

```cpp
const upscale::Extent2D extent = upscale::dynamic_render_extent(
    output, settings.mode, arbiter.resolution_scale(), upscale::DynamicResolutionBounds{});
if (!(extent == current)) {
    assembly.set_render_extent(extent.width, extent.height);  // from the next assemble
}
```

The scale is clamped to the bounds (half to full by default) and snapped to 8 pixels. The temporal
framework is not told — its view is the output — so the history survives the move.
`render.upscale`'s (f) runs exactly this loop with the frame over budget and then under it.

## 4. A vendor upscaler

Implement `upscale::Upscaler` — `family()`, `available(device)`, `declare(graph, inputs)` and
`executed()` — over the same `ScreenSpaceStageInputs` the built-in pass is handed, add it to an
`UpscalerRegistry` built over the built-in pass, and hand the frame `stage_of(registry.select(
UpscalerFamily::Fsr, &device))`. A family that was not added, or whose module says it cannot run on
this device, gets the built-in pass, with no change to content. No vendor type may appear in a public
header.

## 5. A program that draws its own geometry

`samples/10-world` has no velocity target and draws through its own pipeline. Two things change:

- **The jitter.** Its projection is not the frame's, so it applies `AssemblyReport::jitter` (in
  rendered pixels) as the opaque pass's viewport offset — `x = +jitter.x`, `y = -jitter.y`, because
  the viewport's y runs down the image. The offset is under half a pixel, so every pixel centre is
  still covered.
- **The motion.** Without motion vectors the upscaler reprojects by the camera alone. Objects that
  move under a still camera are rejected and rectified rather than followed.

## 6. Measuring and testing

- `just capture-upscale` times both samples in every mode at 1440p and 4K, one run at a time, and
  writes the published pictures and crop strips. Run it on a quiet host: beside a build it measured
  up to 30 % slower.
- `integration.rendering_upscale` is the host reference over synthetic scenes; `render.upscale` is
  the device; `render.upscale_off` pins the frame without an upscaler to references from before.
  `CY_UPSCALE_VERBOSE=1` prints every measured number.
- After changing a shader, `python3 src/rendering/upscale/shaders/regenerate.py --slangc
  build/dev/Development/bin/slangc`, and change `upscale.cpp`'s reference to match:
  `render.upscale`'s (b) compares them texel for texel.

## 7. Pitfalls

- **Bind the graphics pipeline before its sets.** The RHI binds sets at the bind point of the last
  pipeline bound; after the upscaler's compute passes, sets bound first land on the compute bind
  point and the draw reads whatever the graphics point last held. Both samples hit this.
- **Take the jitter out of motion vectors.** `history_uv = uv + motion` with the jitter removed; a
  prepass that leaves it in makes a still frame shimmer.
- **Do not record scene passes at the output extent.** Anything a stage before the upscaler samples
  must be at the render extent, including a decal or cluster grid built on the side.
- **Graph reads after a read-made transition.** Two reads of one image in different stages, the
  first of which transitioned it, need the second to wait for that transition. The graph derives
  this since this change; a hand-written pass outside the graph would not.

## 8. What is not built yet

- No vendor module: the seam exists, with FSR, DLSS, XeSS and MetalFX slots.
- No DXIL: a D3D12 device reports the pass unavailable and renders at the output extent.
- Metal compiles and has not run on a device.
- The reconstruction reads about sixty texels a pixel, about 4 ms at 4K on an RTX 5060; a
  render-extent dilation pass is the next step.
- A world-space text surface is not in any scene the suites draw.
