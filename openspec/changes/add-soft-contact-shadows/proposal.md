# Add soft and contact shadows to the frame

## Why

`virtual-shadows` asks for softness "driven by physical source shape — angular radius for
directional lights" and for contact refinement — "a short screen-space trace ... an addition to the
paged result, not a replacement for it" — and `rendering-lighting-and-shadows` asks for PCSS: "a
blocker search estimating average occluder depth, from which a penumbra radius scales the filter
kernel". The tree had the host arithmetic for a perspective penumbra (`pcss_penumbra_radius`) and a
`ShadowLever::Refinement` with nothing behind it; the frame's directional shadow was a fixed 3x3
filter, and both `virtual-shadows` rows were recorded as `exempt:m11e`.

## What changes

- `cy/shadow.slang` gains percentage-closer soft shadows: a rotated blocker search, a directional
  penumbra from similar triangles, and a variable filter, read through an `IShadowDepthSource` so
  the frame's bindless copy and a sample's depth texture share one function.
  `src/rendering/lighting/soft_shadows.h` derives the shape from the light's angular radius and the
  map's footprint, writes the frame words, and carries the filter's C++ twin.
- A new module, `src/rendering/contact_shadows/`: a screen-space trace toward the directional light
  through the prepass depth, lifted off the surface by one pixel's footprint along the prepass
  normal, into a persistent target, with committed SPIR-V and MSL and a host reference.
  `ShadowLever::Refinement`'s value scales its per-pixel reach.
- `ForwardFrame` gains a `ContactShadows` stage (`FrameFeatures::contact_shadows`), declared by its
  producer through `FrameStageDeclaration`, with an imported target the opaque pass reads.
  `FrameAssembly` passes it through (`AssemblyDescription::contact_shadows`,
  `AssemblyView::contact_shadows`, `FrameSinks::contact_shadows`).
- `cy/frame.slang` appends `softShadowControl` and `softShadowShape` to the frame block. With the
  PCSS flag the directional shadow is the soft filter; with the contact flag the sun's visibility is
  the darker of the map's and the trace's. Zero flags is the 3x3 filter, arithmetic for arithmetic.
  The frame's committed SPIR-V and MSL are regenerated.
- `samples/12-beauty` gains `--soft-shadows on|off` and `sceneFragmentSoft`; `just
  capture-soft-shadows` publishes the shot off and on and requires off to be M11.c's published
  pixels exactly.
- Tests: `integration.render_soft_shadows` (the penumbra measured on the host),
  `integration.rendering_contact_shadows` (the trace against the geometry on the host) and
  `render.soft_shadows` (the trace on the device against the host, and five frame cases).

## Scope

Kernels that account for virtual PAGE boundaries (the tree's frame has one map and no page table in
its lookup), the stochastic few-sample path through the shared denoiser, a traced contact ray
through `ray-tracing-infrastructure`, per-frame reseeding of the disc rotation for temporal
accumulation, soft shadows for local lights and a D3D12 shader package are not built; the
requirements map records each against its requirement.
