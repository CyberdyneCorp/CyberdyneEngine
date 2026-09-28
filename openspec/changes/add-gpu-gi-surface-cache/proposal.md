# Put the GI scene and the surface cache on the device

## Why

Issue #35 asks for CyberGI to run on the GPU as part of the frame, in five stages that each replace
a host subsystem behind its existing seam and keep the host version as the test oracle. Every tier
of `src/rendering/gi/` is host arithmetic today: the distance field, the surface cards and the card
lighting exist only in host memory, so nothing on the device can trace the field or read a card.
This change is the first slice — stages 1 and 2 — and it is the foundation the radiance cache,
the tracers and the per-pixel resolve (stages 3 to 5) are built on.

## What changes

- **Stage 1, the GI scene on the device.** A new module `cy::rendering-gi-gpu`
  (`src/rendering/gi_gpu/`) uploads the distance field's clipmaps — a toroidal page table per level
  and the brick pool, slot for slot — and the surface cards, INCREMENTALLY: the field keeps a change
  journal (`DistanceField::last_changes()`, the bricks each `scroll_to` solved, which are the bricks
  the scene's invalidation causes reached plus the band a scroll exposed) and each surface page a
  `revision` bumped by allocation, release and invalidation. A still frame uploads nothing. A
  compute entry point sphere traces the uploaded field.
- **Stage 2, the surface cache's card lighting in compute.** Direct light from every light, shadowed
  through a directional light's shadow map (`gi::ShadowMap`) and through the uploaded field for every
  other light; the sky term for gather rays that escape; and last frame's card radiance for rays that
  hit, which is the multi-bounce feedback. The update is the host's own budgeted, prioritised
  selection — `SurfaceCache::select`, factored out of `SurfaceCache::service` unchanged.
- **Behind a seam.** `gi::SurfaceShadingBackend` in `surface_cache.h`: the cache keeps every
  decision (allocation, invalidation, selection, lookup) and hands a backend the arithmetic.
  `IlluminationSystem::set_surface_shading` installs the device one; null is the host shading.
  `cy::rendering-gi` names no device.
- **The host oracle.** `gi/card_lighting.h`: `ShadowMap`, `ShadowMapOccluder`, `CardGrid` (the
  lookup structure both sides index), `CardSnapshot` (a `RadianceLookup` over last frame's radiance)
  and `CardGather` (the `IndirectSource` the device gather transcribes).
- **Tests.** `render.gi_gpu` (Vulkan): the device trace against `DistanceField::sphere_trace`, a
  moved object re-uploading only its bricks, the device card radiance against the host surface cache
  in a closed room and under a shadow-mapped sun, and the budget. `integration.render_gi_pipeline`
  gains the headless half: the journal, the snapshot against the cache's own lookup, the shadow map
  against the field, and the seam in the composed system.

## Scope

Stages 1 and 2 only. NOTHING IS COMPOSITED INTO THE FRAME: no frame the tree draws reads anything
the new module writes, and `cy/frame.slang` and the forward frame are untouched. Not in this change:
the radiance cache and tracing tiers on the device (stage 3), the per-pixel resolve, the denoiser
transcription and the composite at `probeVolumeAmbient` (stage 4), and the GPU cost in `GiBudget`
(stage 5). The shadow map is a depth array the host provides, not the frame's shadow cascade, and
every buffer is host-visible and written in place — right for a suite that drains each frame, and
something stage 4 replaces with per-frame staging before a live frame uses it.
