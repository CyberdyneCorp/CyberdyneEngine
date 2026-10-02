# `src/ui/render/` — layer 4

CyberUI on the device: the flattened primitive stream drawn over the frame at its interface stage.

**Governed by**: `ui-system` — "Rendering", "GPU-driven rendering". Change:
`openspec/changes/add-runtime-ui-rendering/` (issue #91, stage 1). Behind `CY_UI` with the rest of
`src/ui/`.

## The files

| File | What it holds |
|---|---|
| `encode.h` | The device-free half: `GpuUiPrimitive` (the 64-byte row the shader reads), `scissor_for`, `build_draws` (one `UiDraw` per batch), and `shade_reference` / `draw_reference` — the shader transcribed for the suites |
| `ui_renderer.h` | `UiRenderer`: the pipeline, the atlas pages, the per-frame rows and indirect arguments, and the pass declared through `FrameStageDeclaration` |
| `shaders/ui.slang` | `cyUiVertex` and `cyUiFragment`, one shader with material-indexed behaviour |
| `shaders/regenerate.py` | recompiles it and rewrites `src/ui_spirv.h` and `src/ui_msl.h` |

## The frame

```
post-process ─► output ─► selection outlines ─► ui ─► present
                                                 ▲
         flatten() ─► rows + indirect arguments ─┘   (host-visible, one set per frame in flight)
```

`FrameSinks::ui = renderer.stage()` hands the `UiAndDebug` stage to this module, which declares one
pass that loads and stores the colour the chain ended in. Without a producer the stage is the single
pass it always was. With one attached and nothing to draw, the pass records nothing and the frame is
the frame from before the pass existed, byte for byte (`render.ui`, case (a)).

## What a caller does

```cpp
renderer.create(device, {width, height, output_format});
renderer.upload_atlas(1, rhi::Format::R8Unorm, extent, extent, text.atlas_pixels());  // outside a frame
// per frame:
ui::layout(store, scale_settings, viewport, &text, layout_report);
ui::flatten(store, viewport_rect, buffer, flatten_report, &text);
renderer.submit(buffer, ui::resolve_scale(scale_settings, viewport));
sinks.ui = renderer.stage();
```

## Five decisions, and why

**One draw per batch, indirect.** `flatten()` already breaks batches on material, atlas, clip and
transform, so a batch is exactly one pipeline state, one page and one scissor. Each is one
`draw_indexed_indirect` of an instanced quad; the arguments are written by `declare()` into the frame
slot's buffer, so a later GPU-side culling pass can write them instead without changing the record.

**A signed distance in pixels.** A pixel centre half a pixel inside the rounded box is fully covered,
half a pixel outside not at all, so a whole-pixel rectangle covers whole pixels and its colour lands
in the output byte for byte; a border is the box minus the box inset by its width; corners are
anti-aliased. The quad is grown by a pixel so a fractional edge is rasterised at all.

**Premultiplied, display-referred, discard where nothing is drawn.** Colours arrive premultiplied with
the element's opacity folded in and blend `One, OneMinusSourceAlpha`. On an sRGB target the shader
decodes first (`kUiOutputLinear`) so the bytes in the output are still the interface's, and a
translucent colour blends in linear light; `render.ui` case (g) draws on an `Rgba8Srgb` target to
hold both.

**Atlas pages are uploaded outside the frame.** `upload_atlas` runs a graph of its own and waits, as
grading's table upload does, and leaves the page in the sampled layout. Page 0 is one white texel,
so a shape samples white and the set always holds a texture.

**The host reference is the test oracle.** `draw_reference` rasterises the same rows the way the
device does; `render.ui` requires every pixel within one 8-bit step of it.

## Not built

Offscreen opacity groups, the HDR composite before the tone curve, custom UI materials, blur-behind,
world-space and surface-space documents, and transforms other than the identity (`ui-system` stage 2
and 3 of issue #91). Metal's MSL is generated and committed and not exercised on this host; D3D12 is
compiled and compared by `just build-shaders --strict`, and no DXIL is embedded.

## Testing

`unit.ui_render` — the rows, the scissor rule, one draw per batch, and the reference. `render.ui` —
seven cases on a Vulkan device, with validation on; see `tests/test_ui_device.cpp`'s header.

| No interface | The strategy HUD and the console |
|---|---|
| ![](../../../docs/design/images/rts-selection-after.png) | ![](../../../docs/design/images/rts-hud.png) |
