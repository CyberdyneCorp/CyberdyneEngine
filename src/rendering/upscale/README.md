# `src/rendering/upscale/` — layer 4: temporal upscaling

`cy::rendering-upscale`. **Governed by** `rendering-post-processing` — "Temporal upscaling" — and by
`temporal-rendering` for the jitter and the history it keeps.

**Status: built and run on Vulkan, in the Development profile.** `unit.`, `integration.rendering_upscale`,
`render.upscale` and `render.upscale_off` pass with validation and synchronisation validation on,
every image case has been proven red by a mutation (the list is
`tools/roadmap/requirements-coverage.toml`'s "Temporal upscaling" note), and
`docs/design/images/upscale-*.png` are the beauty shot and the world in every mode. Metal: the MSL is
generated, embedded and compiles under `just build-shaders --strict`; it has not run on a device
from this change. D3D12: no DXIL is embedded, so `available()` is false there and a frame renders at
its output extent.

## What it does

The scene is drawn at a RENDER extent below the OUTPUT extent with the temporal framework's
sub-pixel jitter, and the frame's `FramePassKind::Temporal` stage — step 5 of the chain — rebuilds
the output from this frame's samples and a history kept at the output extent:

```
colour, depth, motion (render) ─┐
pre-translucency copy (render) ─┼─► reconstruct ─► sharpen ─► upscaled (output) ─► DoF, motion blur,
history, state (output) ────────┘        │                                         bloom, post, outlines, UI
                                         └─► history, state (output, next frame)
depth, motion (render) ─► resample ─► depth, motion (output) ─► the stages after it that read them
```

| Pass | Shader | What it computes |
|---|---|---|
| reconstruct | `shaders/upscale_reconstruct.slang` | the 3x3 render samples around the one nearest the pixel: a spatial kernel in render pixels, this frame's evidence in output pixels, the neighbourhood's moments in tonemapped YCoCg, the closest surface's depth and motion. The history is reprojected along that motion (Catmull-Rom), rejected where it saw another surface (disocclusion) or where this frame's samples fall outside what the history held at the same positions (a change of shading), rectified to the neighbourhood, and blended as `weight` samples with this frame's evidence |
| sharpen | `shaders/upscale_sharpen.slang` | robust contrast-adaptive sharpening of the output. Not fed back: the history is the unsharpened reconstruction |
| resample | `shaders/upscale_resample.slang` | the depth and motion of the sample nearest each output pixel, written at the output extent for depth of field, motion blur and the selection outlines. Declared only when one of them runs |
| copy | `shaders/upscale_copy.slang` | the colour before translucency, the frame's `OpaqueColorCopy` stage, from which the reactive mask is derived. Declared only with `FrameFeatures::reactive_mask` |

`include/cy/rendering/upscale/upscale.h` holds the modes, the extents, the jitter phase count, dynamic
resolution, the 128-byte constants and a host reference of the reconstruction, the sharpening and the
resample written expression for expression in the shaders' order. `upscale_pass.h` is the device half.
`upscaler.h` is the vendor seam.

## The modes

| Mode | Scale per axis | Pixels shaded |
|---|---|---|
| `NativeAntialiasing` | 1.00 | 100 % — the same reconstruction as temporal anti-aliasing |
| `Quality` | 0.67 | 45 % |
| `Balanced` | 0.58 | 34 % |
| `Performance` | 0.50 | 25 % |

`render_extent_for` rounds to the nearest pixel and never exceeds the output. `jitter_phase_count` is
`ceil(8 * ratio^2)` with a floor of eight: each output pixel needs as many phases as render pixels it
spreads over to see every sub-pixel position. `texture_lod_bias` is `log2(render / output)`, the
mip bias a material sampler should take so a texture is as sharp as at the output extent.

## Dynamic resolution

`dynamic_render_scale` is the budget arbiter's resolution allocation times the mode's scale, clamped
to `DynamicResolutionBounds` (half to full by default) and rounded to an 8-pixel granularity so a
small change of budget does not move the extent every frame. `assembly::FrameAssembly::set_render_extent`
applies it from the next frame. The temporal framework is not told: its view stays at the output
extent, so the history survives the move and `render.upscale`'s (f) shows the history-reset count
unchanged across it.

## What the reconstruction keeps and rejects

The history is RGBA16F — linear colour and the evidence it holds, in samples — and a RG32F state:
a lock and the depth of the surface the history saw. The rules, and why each exists:

- **Accumulation.** This frame's evidence is a narrow kernel in output pixels, unnormalised, so a
  pixel far from every sample adds little and its history is not diluted. The history is capped at
  16 samples when still and 6 when moving more than 4 output pixels a frame.
- **Disocclusion.** The history's depth at the reprojected position, nearest over 3x3, against this
  frame's nearest depth over 5x5 reprojected into the previous frame, with a 5 % relative tolerance.
  A pixel whose history saw a surface nearer than anything here now is a revealed background, and its
  history is dropped.
- **Change of shading.** The fraction of the 9 samples outside the range the history held at the
  same positions, with a margin of 0.02 plus 10 % of luma. A lamp switched on is new evidence, not
  noise to clip towards the old colour.
- **Rectification.** The history is clipped toward the neighbourhood's variance box: 2.5 standard
  deviations still, 1.0 moving, pulled by how much of the pixel this frame observed.
- **Locks.** A thin ridge — a one-pixel line the jitter only sometimes lands on — is locked for 64
  frames so rectification does not erase it between the frames that see it. A pixel that has just
  been rejected is unsettled for 4 frames and follows the current frame.
- **Reactive mask.** The maximum tonemapped difference between the final colour and the
  pre-translucency copy, times 4, lowers the history's weight: particles and translucent surfaces
  carry no motion of their own and would otherwise smear.
- **No motion vectors.** A frame without a velocity target (`samples/10-world`), and the sky in one
  with it, reprojects by the camera alone through `current_to_previous`.

## Where it sits in the frame

`FrameDescription::output_width`/`output_height` are the output extent and
`FrameDescription::temporal_stage` the producer. Everything before the temporal stage — the prepass,
ambient occlusion, contact shadows, the GI, the fog, shading, translucency — runs at the render
extent; depth of field, motion blur, bloom, the post-process, the outlines and the interface run at
the output extent after it. The interface is therefore pixel-exact: `render.upscale`'s (e) draws text
through `ui::UiRenderer` in every mode and finds the same bytes.

## The vendor seam

`Upscaler` is the interface — `family()`, `available(device)`, `declare(graph, inputs)`,
`executed()` — and `UpscalerRegistry::select` returns the requested family when a module for it was
added and is available on the device, and the built-in pass otherwise. `UpscalerFamily` has slots for FSR, DLSS, XeSS and MetalFX; no
vendor SDK is linked and no vendor type appears in a header. A vendor module implements `Upscaler`
over the same `ScreenSpaceStageInputs` the built-in pass is handed.

## The suites

| Suite | Kind | What it proves |
|---|---|---|
| `unit.rendering_upscale` | unit | the modes and their names, render extents, the jitter phase count, dynamic resolution's clamp and granularity, the constants and their refusals, the registry's choice and fallback, the stage declaration's hand-off |
| `integration.rendering_upscale` | integration | the host reference over synthetic 96x54 scenes (the thin line at 64x36), every case with a control: convergence to the supersampled scene per mode, sample placement, a thin line's stability and, separately, its coverage, no trail behind a moving object, a revealed background rebuilt from this frame, a change of shading reaching the picture in its frame, a reactive pixel following the current frame, exactness without history, sharpening, the resample |
| `render.upscale` | render | the pipeline suites' scene at 480x270 on Vulkan: (a) per-mode convergence to the native frame; (b) the device reconstruction against the host reference over read-back inputs; (c) a sliding box's trail; (d) a jumping box's revealed floor; (e) the interface byte for byte in every mode; (f) the arbiter's render extent moving with the history kept; (g) motion blur after the upscaler at the output extent |
| `render.upscale_off` | render | links no upscaler: the frame with TAA, moving and still, and with bloom, byte for byte the references drawn by the tree before this module |

## Measured

RTX 5060, Vulkan, whole-frame device time (the first scene pass to the output's read-back) as the
median of 60 held frames, and the error of the converged still against the native-AA still in 8-bit
sRGB. `samples/12-beauty` with its GI on; `samples/10-world` with water, cloud shadows and aerial
perspective.

| Scene | Output | Off (no AA) | Native AA | Quality | Balanced | Performance |
|---|---|---|---|---|---|---|
| beauty | 2560x1440 | 5.62 ms | 7.62 ms | 6.14 ms, 42.6 dB | 5.32 ms, 40.4 dB | 4.65 ms, 37.3 dB |
| beauty | 3840x2160 | 12.04 ms | 16.47 ms | 10.62 ms, 43.4 dB | 9.49 ms, 41.7 dB | 8.56 ms, 40.3 dB |
| world | 2560x1440 | 2.86 ms | 4.62 ms | 3.64 ms, 49.1 dB | 3.43 ms, 49.0 dB | 3.28 ms, 49.0 dB |
| world | 3840x2160 | 5.55 ms | 9.49 ms | 7.49 ms, 50.4 dB | 7.07 ms, 50.2 dB | 6.76 ms, 50.1 dB |

The no-AA frame differs from native AA by 38.1 to 44.0 dB, so Quality and Balanced are closer to
the anti-aliased frame than the aliased frame is everywhere, and Performance everywhere but the
beauty shot at 1440p. Native AA adds a motion prepass and the reconstruction at every output pixel:
in the world, which shades per vertex, the reconstruction is about 4 ms at 4K and dominates, so an
upscaled world frame costs more than the aliased one; in the beauty shot, where the GI scales with
the render extent, Performance is 17 % under the aliased frame at 1440p and 29 % at 4K, and 39 % and
48 % under native AA. The reconstruction reads around sixty texels a pixel; a render-extent dilation
pass, as FSR 2 has, is where its cost would come down.

`just capture-upscale` takes every number here, one run at a time on a quiet host — a run beside a
build measured up to 30 % slower — and writes `docs/design/images/upscale-beauty-{native,performance}.png`
(1440p) and the crop strips `upscale-{beauty,world}-crops-{1440,2160}.png`, the same region in every
mode at twice its size, nearest-neighbour.
