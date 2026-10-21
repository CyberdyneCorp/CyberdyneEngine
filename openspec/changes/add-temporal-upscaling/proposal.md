# Render the scene below its output extent and reconstruct it with a temporal upscaler

## Why

`rendering-post-processing` asks for "rendering the 3D scene at a reduced internal resolution and
reconstructing it at output resolution, with UI drawn at native resolution", a built-in temporal
upscaler behind one interface that vendor upscalers can plug into, and internal resolution as "a
budget allocation held by the renderer budget arbiter". The requirements map recorded it as
`exempt:m11e` with the finding "ONE OF FOUR SCENARIOS IS ANSWERED AND THERE IS NO UPSCALER": the
frame had one extent, every pass was recorded at it, and the only temporal stage was a
same-resolution resolve.

## What changes

- The frame has a RENDER extent and an OUTPUT extent. `ForwardFrame` takes
  `FrameDescription::output_width`/`output_height` (zero: the render extent) and a
  `temporal_stage` producer; every target from the prepass to the temporal stage is at the render
  extent, and the producer's colour, depth of field, motion blur, bloom, the post-process, the
  outlines, the interface and a frame-owned output at the output extent. The stages after the
  producer are handed depth and motion it resampled to the output extent.
  `FrameStageDeclaration` gains an `executed` hook; an `opaque_copy_stage` producer makes the
  pre-translucency copy a reactive mask is derived from.
- `FrameAssembly` takes `AssemblyDescription::render_width`/`render_height`,
  `produced_temporal_stage` and `reactive_mask`, `FrameSinks::temporal` and `opaque_copy`, and
  `set_render_extent()` — dynamic resolution. The temporal framework's view stays at the output
  extent; `TemporalView::render_width`/`render_height` make the jitter a fraction of a rendered
  pixel. `pipeline::FrameRecorder` records every scene pass, the jitter, the shaders' extent and the
  cluster grid at the render extent.
- A new module, `src/rendering/upscale/` (`cy::rendering-upscale`): the quality modes, render
  extents, the jitter phase count and texture LOD bias, dynamic resolution as the arbiter's
  allocation times the mode within bounds, the `Upscaler` interface and `UpscalerRegistry` (the
  vendor seam: FSR, DLSS, XeSS and MetalFX slots, no vendor types), and `TemporalUpscalePass` —
  reconstruction, sharpening, a depth and motion resample and a copy, with committed SPIR-V and MSL
  and a host reference written expression for expression.
- `rendering-graph`: a read in the same layout after a read that transitioned an image waits for
  that transition. It did not, and the upscaler's reconstruction and resample both sampling the
  prepass depth was the first frame to read an image from two stages after a read-made transition.
- Tests: `unit.rendering_upscale`, `integration.rendering_upscale`, `render.upscale`,
  `render.upscale_off`, and new cases in `unit.render_graph` and `unit.render_temporal`.
- `samples/12-beauty` and `samples/10-world` take `--upscale <mode>`, and measure the whole frame's
  GPU time and its error against native at 1440p and 4K.

## Scope

The upscaler runs where the frame's compute shaders run: SPIR-V and MSL are embedded, so a D3D12
device reports it unavailable and renders at the output extent. Vendor upscalers are seams, not
integrations. A world-space text surface is not in any scene the suites render; text drawn by the
interface is pixel-exact at the output extent. Dynamic resolution is wired through the assembly and
measured with the pipeline suites' scene; the beauty shot and the world run at fixed modes.
