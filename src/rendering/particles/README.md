# `src/rendering/particles/` — the pipeline layer's first consumer

Layer 4. M8.c task 1b.4.

**Governed by** `vfx-system`, the compositing half of it.

## Why it exists

Task 1b.4: *"A particle renderer is the first consumer, and it proves the layer by USING it rather
than by a test written beside it."*

So this module writes no frame, declares no pass, and creates no descriptor set layout for anything
the frame already has. It:

* **reuses `FramePipelines`' set layouts 0 and 1 verbatim** — the handles, not copies of the
  declarations — so its pipeline layout is *compatible* with the frame's and the sets
  `FrameRecorder` bound stay bound;
* **reads the same per-view block the opaque pass reads**, at the same set and binding, through
  `import cy.frame` — so a particle and a mesh cannot disagree about the projection;
* **attaches through `PassExtension`**, the seam `FrameRecorder` published, and draws inside the
  frame's own transparent stage.

If any of those three seams were wrong this module would not draw. That is the test, and two of them
were wrong when it was first written:

* the recorder bound its descriptor sets only inside `draw_layer`, so a transparent stage with no
  transparent meshes left set 1 unbound — seventeen validation errors on a frame whose transparent
  layer happened to be empty;
* the particle pipeline layout declared no push-constant range, and Vulkan's pipeline-layout
  compatibility rule (14.2.2) requires **identical push constant ranges** as well as identical set
  layouts before a previously bound set survives a bind with another layout.

Neither is visible in a unit test of either module. Both are visible the moment one uses the other.

## What it is and is not

**It is the compositing half of `vfx-system` and nothing else.** It takes a span of
`ParticleInstance` — a camera-relative position, a size and a linear colour — and issues ONE draw:
six vertices a particle expanded from the vertex id, no vertex buffer, no index buffer, no
per-particle call. `src/vfx/` owns emitters, the graph compiler, the attribute layout and the
scheduler; `ParticleInstance` is the seam between the two.

**It has no atlas and no soft depth fade.** Both are texture work and both are `vfx-system`'s data
interfaces; the sprite is a smooth radial falloff computed in the fragment shader. Recorded here
rather than hidden, because a particle renderer that quietly drew squares would look like a defect
in the simulation.

**A particle's colour is a radiance, not a colour.** The frame's colour target is scene-referred and
the tonemap is downstream, so an emissive particle is in the same physical range as a light — a
colour of 1 is invisible at the exposure a 22 000 lux sun is viewed through. That is a property of a
physically-based frame rather than a fudge factor, and `render.pipeline`'s scene says so where it
sets the numbers.

## The picture

`docs/design/images/pipeline-particles-m8c.png` — 512 particles in one draw, composited into the
frame's own colour target, read back off the device. Engine output, not a diagram.
