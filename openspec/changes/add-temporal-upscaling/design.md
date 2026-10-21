# Design

## Two extents, one frame

`FrameDescription::width`/`height` stay the extent every pass from the prepass to the temporal stage
records at, so every caller that never heard of upscaling declares the frame it always did.
`output_width`/`output_height` are appended and default to zero, which means "the render extent".
`ForwardFrame` keeps a `stage_width`/`stage_height` and a `stage_depth`/`stage_velocity` in its build
state: the temporal producer moves them to the output extent and to the depth and motion it
resampled, and every stage declared after it — depth of field, motion blur, bloom, the post-process,
the outlines, the interface — is handed those. The frame refuses an output extent without a
producer, a producer without a temporal stage or over multisampled targets, and an output smaller
than the scene.

The temporal stage is a `FrameStageDeclaration` like every other produced stage, with an `executed`
hook appended: a history is swapped only for a frame whose fence passed, and only the code that
executes the frame knows that, so `FrameAssembly::execute` calls it.

## The temporal framework stays at the output extent

Jitter is a fraction of a RENDERED pixel, so `TemporalView` gains `render_width`/`render_height` and
the jittered projection divides by them. The view, the histories and the invalidation causes stay at
the output extent: changing the render extent — dynamic resolution — is not a resolution change the
framework sees, which is why the history survives it. The phase count grows with the square of the
upscale ratio (`jitter_phase_count`), because each output pixel needs as many phases as render pixels
it spreads over.

## The reconstruction

One compute pass per output pixel over the 3x3 render samples around the nearest one: a spatial
kernel in render pixels for the colour this frame alone would give, and an evidence kernel in output
pixels — unnormalised — for how much of the pixel this frame actually observed. The history is
weighted in samples (alpha), so accumulation is a running mean capped at 16 samples still and 6
moving; evidence below one keeps a distant sample from diluting a converged pixel.

Motion is the closest surface's over the 3x3 (so an edge's background is reprojected with the edge),
or the camera's alone when the frame has no velocity target or the closest sample is the sky. The
history is fetched with a 16-tap Catmull-Rom so a moving picture does not soften every frame.

Rejection is three tests, each answering a different failure:

1. **Disocclusion** compares depth. The history keeps the depth of the surface it saw (state.y, at the
   output extent). If that surface — the nearest over a 3x3 of history state — is nearer than
   anything over a 5x5 of this frame's reprojected depth by more than 5 %, the history saw an
   occluder that has gone. The 5x5 on this side and 3x3 on the other is deliberate: an edge moving
   less than a pixel must not reject itself.
2. **Shading change** compares colour at the same positions. The fraction of the 9 samples outside
   the 2x2 history texels' range (with a margin) is the part of the pixel the history cannot explain.
   A quarter may be unexplained — a pixel's own noise and edges — and past that the history's trust
   falls, to none at 60 %. A variance clip alone would pull a
   switched-on lamp in over many frames.
3. **Reactive** pixels — what translucency changed against the pre-translucency copy — keep less
   history, because a particle carries no motion of its own.

What is kept is rectified toward the neighbourhood's variance box in tonemapped YCoCg, by how much
of the pixel this frame observed. Locks protect thin ridges for 64 frames and mark a just-rejected
pixel unsettled for 4, in state.x.

The host reference in `upscale.cpp` is written expression for expression in the shader's order, and
`render.upscale`'s (b) holds the device to it texel for texel over inputs read back from a frame
with the camera moving, particles and the reactive mask.

## Sharpening, resampling, the copy

RCAS on the reconstruction, never fed back. The resample writes the depth and motion of the sample
nearest each output pixel for the stages after it that read them, as a graphics pass with
`SV_Depth`, because a depth format is not a storage format. The opaque copy is a compute dispatch
the frame declares through `opaque_copy_stage`, so the reactive mask needs no transfer usage on the
scene colour.

## The graph fix

Reconstruction and resample both sample the prepass depth. The first read transitions it to a
sampled layout; the second, in another stage, needed no transition and so had no barrier — and was
not ordered after the first read's transition either. `compile.cpp` now records the stage that made
a read transition, and a later read in the same layout waits for it. `unit.render_graph`'s case was
red on the old derivation.

## What is not here

No vendor SDK: `Upscaler` and `UpscalerRegistry` are the seam, with FSR, DLSS, XeSS and MetalFX slots
and no vendor type in a header. No DXIL: a D3D12 device reports the pass unavailable. The cost — about
sixty texel reads a pixel — is above FSR 2's on the same hardware; a render-extent dilation pass that
computes the closest depth, motion and lock inputs once per render pixel is the next step.
