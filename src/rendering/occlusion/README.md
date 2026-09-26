# `src/rendering/occlusion/` — layer 4

Ground-truth ambient occlusion on the device: the horizon search over the prepass depth and normals,
the shared denoiser's spatial cascade over its output, and the persistent target the forward pass
samples.

**Governed by**: `rendering-post-processing` — "Ambient occlusion" — and `denoising` for the
filter. Change: `openspec/changes/add-ambient-occlusion/`.

## The files

| File | What it holds |
|---|---|
| `gtao.h` | `GtaoSettings`, `GtaoView`, the two push-constant blocks, and `gtao_reference` — the horizon search on the host, expression for expression |
| `occlusion_pass.h` | `AmbientOcclusionPass`: the pipelines, the persistent target, and the stage the frame declares through `FrameStageDeclaration` |
| `shaders/gtao.slang` | `cyGtao`, the horizon search |
| `shaders/gtao_filter.slang` | `cyGtaoFilter`, one pass of `denoise::Denoiser`'s a-trous cascade |
| `shaders/regenerate.py` | recompiles both and rewrites `src/occlusion_spirv.h` and `src/occlusion_msl.h` |

## The frame

```
depth + normal ─► horizons ─► filter step 1 ─► step 2 ─► step 4 ─► target ─► opaque (ambient only)
   (prepass)        (raw)
```

`PostChainConfig::ambient_occlusion` is the setting. On, `ForwardFrame` derives the
`DepthNormal` prepass, and at its `FramePassKind::AmbientOcclusion` stage hands the declaration to
this module (`FrameSinks::ambient_occlusion`), which declares five compute passes: the barriers
between them are the graph's. Off, the stage is not declared and nothing here runs — and the frame
is byte-identical to one with no pass attached at all (`render.ambient_occlusion`, case (d)).

The target is imported rather than transient because the forward pass samples it through a slot of
its set 0 texture table, and a slot names a view that must outlive the graph. The caller puts
`target_view()` at a slot and writes `write_occlusion_control(slot, settings)` into
`FrameViewData::occlusion_control`;
`cy/frame.slang` multiplies the AMBIENT term by it and leaves the direct sum alone unless
`occlusion_control.y` asks for the non-physical option.

## Four decisions, and why

**GTAO, not SSAO.** SSAO counts samples inside a hemisphere, a ratio with no physical meaning that
darkens an open floor unless tuned against the depth precision. GTAO integrates the cosine-weighted
visible arc between two horizons in closed form: an open plane is exactly 1, and the specification
names it.

**Normals from the prepass, not from depth.** The `DepthNormal` prepass is derived from ambient
occlusion being on and fills the target anyway. A normal reconstructed from depth differences is
wrong at every silhouette and every contact — exactly where occlusion is decided.

**Self-normalised slices on a pixel lattice.** The slices are whole-pixel directions (eight over
half a turn, four a pixel by default, rotated by a 4x4 dither), so every tap is a pixel centre on the
slice's own screen line and reconstructs exactly in the slice plane; a snapped continuous rotation
put taps half a pixel off it and read an open floor at 0.71. The sum over slices is divided by the
same slices' unoccluded integral rather than by the slice count, so an unoccluded surface is exactly
1 rather than 1 on average, and a 0.03 horizon-cosine bias keeps the surface's own taps from reading
as occluders. `unit.rendering_occlusion` holds an open plane at 1 − 1e-5 for one, four and eight
slices.

**The filter is the denoiser's, transcribed, and measured against it.** `denoise::Denoiser` is a
module of host spans; `gtao_filter.slang` is its `filter_pixel` and `tap_weight` on the device,
driven by `default_config(SignalKind::AmbientOcclusion)` and the quality ladder's first position.
`render.ambient_occlusion` runs the device's raw term through `Denoiser::denoise` and compares the
two buffers: visibility within 4e-3 (measured 1.5e-3 worst, 2.7e-4 mean), bent-normal components
within 3e-2 (measured 1.4e-2), both set by the half-precision storage of the intermediates.

## What is not here

* **Temporal accumulation of the term.** The search's noise is a 4x4 pattern fixed in screen space,
  so a still view is still without history (case (e) measures zero change) and the 5x5 cascade base
  spans one period. A moving camera sees the pattern move with the screen, filtered but not
  accumulated; `temporal-rendering`'s reprojection is not fed to the denoiser for this signal.
* **Ray-traced ambient occlusion**, which the requirement makes selectable "where ray tracing is
  available". No device path traces rays in this tree.
* **Bent-normal specular occlusion in a frame.** The bent normal is produced (the target's rgb) and
  `unit.rendering_occlusion` checks it leans out of a corner, but neither `cy/frame.slang` nor the
  beauty shot has an indirect specular term for it to occlude. `specular_occlusion()` in
  `post/effects.h` is the arithmetic, tested on the host.
* **Half resolution.** `AmbientOcclusionSettings::resolution_scale` is not read; the term is full
  resolution, which the requirement allows.

## Backends

* **Vulkan** — built, run and tested on this host.
* **Metal** — the MSL is generated from the same Slang and committed; the depth binding is a
  `DepthTexture2D` so Metal sees `depth2d`, which is what a `D32Sfloat` prepass target is there.
  Compiled by Slang only: no Apple compiler has seen it here.
* **D3D12** — nothing is embedded, and `supported()` refuses it. Both entry points compile to DXIL
  with this tree's Slang (`-target dxil -profile cs_6_0`, checked on this host), so what is missing
  is the rest of the path: the DXIL embedded beside the SPIR-V and MSL and selected through
  `ShaderModuleBundle::dxil`, and the D3D12 backend's mapping of a compute `ParameterBlock` of four
  SRVs and one UAV onto its descriptor table, run on a Windows device. The frame's own fullscreen
  vertex stage not reaching D3D12 (`m11c:every-shader-reaches-every-target`) is a separate gap:
  this module has no vertex stage.
