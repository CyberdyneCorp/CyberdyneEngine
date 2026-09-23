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

## And its sibling: strips — M11.c task 6.3

`StripRenderer` (`strip_renderer.h`, `cy/strip.slang`) draws what `vfx-system`'s `publish_ribbons`,
`publish_trails` and `publish_beams` derive: an ordered run of camera-relative vertices, each with a
half-width, a radiance, where along its strip it is, and which strip it belongs to. Until it existed
those three publications produced rows nothing drew — `src/vfx/README.md` recorded it by name.

It is `ParticleRenderer`'s arrangement exactly, and the two now share it rather than each carrying a
copy: `detail/transparent_draw.h` is the pipeline whose layout reuses the frame's set layouts 0 and 1
**and** its push-constant range, the ring per frame in flight at set 2, and the premultiplied,
depth-tested, not-depth-written blend. The two rules above that each cost a page of validation errors
to find are written once.

* **One draw for every strip in the frame.** Six vertices for each neighbouring pair of records,
  expanded from the vertex id; no vertex buffer, no index buffer, no per-strip call.
* **A strip ends where its identifier changes.** A pair whose two records belong to different strips
  collapses to a point in the vertex shader, so the ring is simply every strip back to back and a
  ribbon broken mid-chain by a kill stays broken in the picture. `StripReport::segments` counts the
  quads that join two vertices of ONE strip — the collapse is a number, not only a property of a
  picture.
* **It faces the camera.** The side a strip widens along is the cross product of its tangent with the
  line of sight, which in camera-relative space is `-position`. So `RibbonVertex::twist` has nothing
  to turn and is not read — named in `cy/vfx/renderers.h` beside the conversion, rather than dropped
  silently.
* **It does not know what a particle is.** `cy::vfx` links this module, not the other way round, so
  the record is the renderer's `StripVertex` and `cy::vfx::to_strip_vertices` is where a ribbon,
  trail or beam row becomes one.

**Where it is in a picture somebody looks at**: `docs/design/images/m11c-beauty-shot.png`. Every
ember carries a trail through the last third of a second of its flight, drawn by this renderer in
the frame's transparent stage before the motes, and `render.vfx` photographs the same trails
against a committed reference and measures what they add to the frame on their own.

* **A trail is light, not matter.** The fragment writes a premultiplied colour and an alpha of
  ZERO, so the shared blend adds and never takes away: a cooling mote's trail over a bright sky
  photographed as a dark streak when it occluded.

**The embedded modules are checked against the block they read.** `unit.particle_modules` reads the
`OpMemberName` and `Offset` decorations out of the SPIR-V this module hands the driver and compares
every member of `cy/frame.slang`'s per-view block with `FrameViewData`'s `offsetof`. It exists
because `particle_spirv.h`, compiled before the temporal work inserted four rows into that block,
read the sprite's billboard basis out of last frame's clip matrix for two days with every suite that
did not render the same frame twice green. Regenerate both headers whenever `cy/frame.slang`'s block
changes; this suite is what says so.

**Compiled, and on Metal not run.** `strip_msl.h` is `slangc -target metal`'s output for the same two
entry points, and its entry-point signature has the shape `particle_msl.h`'s has — a device buffer
at `buffer(0)` and the frame block at `buffer(1)` — but no Metal device on the machine it was
written on has drawn a strip.

## The picture

`docs/design/images/pipeline-particles-m8c.png` — 512 particles in one draw, composited into the
frame's own colour target, read back off the device. Engine output, not a diagram.
