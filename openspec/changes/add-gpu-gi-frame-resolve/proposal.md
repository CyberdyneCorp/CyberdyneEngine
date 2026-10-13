# Light the frame's pixels with the GI on the device, and hold its cost to the budget

## Why

Issue #35 runs CyberGI on the GPU in five stages. Stages 1 to 3 put the distance field, the surface
cards and their lighting, the radiance cache and every tracing tier on the device — and composited
nothing: no frame the tree drew read anything the device GI wrote, and the frame's indirect light
was still the irradiance volume or a flat sky term. This change is stages 4 and 5: the per-pixel
resolve and its denoiser on the device, composited into the forward frame, and the device's cost in
the GI budget and the renderer's arbiter. It is also the first time the GI lights a real scene: the
beauty shot.

## What changes

- **The per-pixel resolve, on the host and on the device.** `gi/screen_resolve.h` is the oracle: a
  pixel takes the radiance cache traced to its own probes (`RadianceCache::sample_traced`), one
  cosine final-gather ray through the world tier and the far-field sky, combined under
  `exclusion_for()`, and the rough specular; `screen_history` reprojects through
  `temporal-rendering`; `ScreenGrid` and `screen_upsample` resolve a half-resolution grid and
  upsample it joint-bilaterally. `gi_gpu::GpuScreenResolve` transcribes all of it and the shared
  denoiser's accumulate, variance and a-trous passes, and publishes two targets.
- **The composite.** `GpuScreenResolve` is the producer of the forward frame's
  `FramePassKind::ScreenSpaceGi` stage (`FrameDescription::screen_space_gi_stage`, appended), and
  `cy/frame.slang` samples its two targets where the irradiance volume's ambient sits, under the
  double-counting rule (`FrameViewData::gi_control`, appended). The words default to none: a frame
  that attaches nothing is the frame it was.
- **The budget (stage 5).** `gi_gpu::GpuGiBudget` puts the resolve's measured GPU time into
  `GiBudget`, `apply_levers` turns `TracingResolution` and `DenoiserQuality` into the resolve's grid,
  ray density and denoiser quality, and `submit` / `apply` link the budget to `BudgetArbiter` on the
  rung of its declared GI ladder the levers add up to, relaxing only when granted.
- **A GI scene from meshes.** `gi/mesh_cards.h`: cards pooled per world cell and facing, and a signed
  distance grid from a mesh's triangles.
- **The beauty shot lit by it.** `samples/12-beauty --gi on` builds its GI scene from the shot,
  converges the caches (converged mode, as the specification asks of a capture), and shades through
  `sceneFragmentGi`. `just capture-beauty-gi` photographs it beside the published still and path
  traces 400 of its pixels against the exact triangles.
- **Defects found and fixed, each with a regression red on the old code**: a brick's last voxel was
  flat (a 10 cm wall vanished from one side); the device probe mirror never uploaded a probe the
  host gathered again; the final-gather ray was weighted by what it met, pulling the estimate toward
  the sky; and the beauty shot's shader programs omitted the motion-blur prepass, because the entry
  list was kept by hand.

## Scope

Stages 4 and 5 of issue #35. Not in this change: `samples/10-world` (its terrain is not in a GI
scene and it shades per vertex with no prepass); the GI's card shading and probe gather in the
beauty shot run on the host; the editor and Swift expose none of the dynamic GI; DXR and Metal
acceleration structures.
