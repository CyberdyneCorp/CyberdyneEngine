# Add ambient occlusion to the frame

## Why

`rendering-post-processing` asks for ground-truth ambient occlusion computed from depth and normals,
filtered by the shared denoiser as a visibility term, and applied to indirect light only. The tree
had the arithmetic for applying a visibility term (`apply_ambient_occlusion`, `specular_occlusion`)
and an `AmbientOcclusionSettings` nothing read, but no device pass: `FrameFeatures::ambient_occlusion`
declared a stage that recorded nothing and the requirement was recorded as `exempt:m11e`.

## What changes

- A new module, `src/rendering/occlusion/`: a GTAO horizon search and one compute pass per level
  of `denoise::Denoiser`'s a-trous cascade, with committed SPIR-V and MSL and a host reference of
  the search.
- `ForwardFrame` gains `FrameStageDeclaration`, so a stage whose producer needs several passes
  declares them at the stage's position in the order, and can take an imported target for the
  ambient occlusion stage. `FrameAssembly` passes both through (`FrameSinks::ambient_occlusion`,
  `AssemblyView::ambient_occlusion`).
- `cy/frame.slang` multiplies the ambient term by the visibility it reads through a set 0 texture
  slot named by `FrameViewData::occlusion_control`; the direct sum is untouched unless the
  non-physical option is on. The frame's committed SPIR-V and MSL are regenerated for the six
  frame entry points; the fullscreen resolve and temporal entries are unchanged.
- `samples/12-beauty` gains `--ambient-occlusion on|off`: on, it records the frame's depth and
  normal prepass and shades the sky term through the term. `just capture-ambient-occlusion`
  publishes the frame off and on and requires off to be M11.c's published pixels exactly.
- Tests: `unit.rendering_occlusion` (the host reference) and `render.ambient_occlusion` (the
  device against the reference and against the denoiser, and five frame cases).

## Scope

Temporal accumulation of the term, ray-traced ambient occlusion, bent-normal specular occlusion in
a frame (no indirect specular term exists to apply it to), half-resolution computation and a D3D12
shader package are not built; the requirements map records each against the requirement.
