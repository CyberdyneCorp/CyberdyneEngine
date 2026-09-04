# `samples/03-first-light` — the M3 milestone artefact

One program that builds a lit, textured, shadowed scene, orbits a camera around it, and renders it
through the render graph. It is the first thing in this repository that draws anything, and it is
deliberately what a contributor reads to find out how a frame is built. Tasks 6.1 and 6.2.

```
just run-sample first-light                          sixty frames, and a report
just run-sample first-light --frames 1               one frame
just run-sample first-light --capture /tmp/a.ppm     write the last frame as an image
just run-sample first-light --origin 1000000         the same scene a million units out
just run-sample first-light --no-shadows             the control: the sun casts nothing
just run-sample first-light --no-aliasing            what the frame's targets cost unaliased
just run-sample first-light --backend null           the frame with no device at all
just run-sample first-light --help
```

A run says what it did and exits zero. On a build with `-D CY_RENDERER_VULKAN=ON`:

```
03-first-light: device   backend=vulkan asked=vulkan
03-first-light: scene    objects=8 vertices=28 indices=42 origin=0 shadows=yes
03-first-light: frame    passes=4 culled=0 submits=1 barriers=6 batches=4 transfers=0
03-first-light: draws    draws=16 triangles=172 viewport=192x108
03-first-light: memory   transients=196608 B unaliased=196608 B aliasing=on
03-first-light: plan     hash=f31bdc099ded9851 frames=60
03-first-light: loop     ticks=240 alpha=0.000 events=0 dropped=0
03-first-light: exit 0 (clean)
```

Sixty frames take about a third of a second. On a default build — `CY_RENDERER_VULKAN` is off — the
same command reports `backend=null` and the same eight lines with the same pass, submit and barrier
counts. That is not a degraded mode; see "The null backend is not a fallback" below.

## Four files, one job each

| File | Owns |
|---|---|
| `main.cpp` | The host: the options, the platform, the display server, the runtime, the loop, the report. |
| `scene.cpp` | The content: the geometry, the objects, the sun, the camera path and the one texture. No device, no handles, pure arithmetic. |
| `renderer.cpp` | The device side: the pipelines, the persistent resources, and the frame declared into the render graph. |
| `shaders/first_light.slang` | The shading: a depth-only vertex stage for the shadow pass, and a lit, textured, shadow-sampling pair for the forward pass. |

`scene.cpp` and `renderer.cpp` are compiled into a library, `cy::sample-first-light`, and
`main.cpp` is the host around it. That is not tidiness: `tests/render/` links the library, so
**the golden images are photographs of this program** and the null-backend suite compiles this
program's frame. A test scene written separately would drift from this one inside a milestone, and
the references would then be photographs of something nobody ships.

## The frame is four declarations and no barriers

That is the whole of the M3 invariant, and it is what a reader should take away:

```cpp
graph.add_pass("albedo upload", QueueKind::Graphics)     // first frame only
     .write(albedo, Access::TransferWrite)
     .record(&record_upload, &state);
graph.add_pass("sun shadow", QueueKind::Graphics)
     .write(shadow, Access::DepthStencilAttachmentWrite)
     .record(&record_shadow, &state);
graph.add_pass("forward opaque", QueueKind::Graphics)
     .read(shadow, Access::FragmentSampledRead)
     .read(albedo, Access::FragmentSampledRead)
     .write(color, Access::ColorAttachmentWrite)
     .write(depth, Access::DepthStencilAttachmentWrite)
     .record(&record_forward, &state);
graph.add_pass("colour readback", QueueKind::Graphics)
     .read(color, Access::TransferRead)
     .write(readback, Access::TransferWrite)
     .record(&record_readback, &state);
graph.add_pass("host read", QueueKind::Graphics).read(readback, Access::HostRead).side_effect();
```

Six barriers in four batches come out of those lines, and this file writes none of them. It could
not: a record callback is handed a `cy::rhi::CommandBuffer`, and a command buffer has draws,
dispatches, copies and debug labels on it and no synchronisation primitive at all. The transitions
that fall out are the albedo texture's `UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY`, the shadow
map's into and out of its attachment layout, the colour target's into `TRANSFER_SRC`, and the
transfer-to-host at the end — every one of them derived from a `read` or a `write` above.

**Even the texture upload goes through the graph.** It would have been shorter to record a one-off
command buffer at start-up with two barriers around a copy, and that is exactly the shape the
invariant exists to forbid. The upload is a pass; the graph derives its two transitions; and on the
second frame the pass is **not declared at all**, which is the same mechanism
`rendering-forward-clustered` uses for a disabled feature — an absent pass rather than a branch.
`render.null_frame` asserts both halves: five passes on the first frame and four afterwards.

## Camera-relative rendering, with the first draw

design.md §3 requires it to land with the first draw rather than when precision breaks, and this
sample is where you can see the mechanism end to end:

* `Object::world_position` and `Camera::position` are `f64`. `cy::Vec3` is `f32` and would not hold
  the value at a million units.
* The subtraction happens **once**, on the CPU, in `Renderer::render` — one difference per object,
  narrowed to `f32` afterwards, and small because the difference is small.
* Nothing below that line sees a world coordinate. The push block carries an object-to-**camera
  relative** matrix, the uniform block carries a **camera-relative**-to-clip view projection, and
  the shader's vertex input is named `positionRelativeToCamera` because there is nowhere to put a
  world one.
* The sun's shadow projection is camera-relative too, fitted to the scene's bounding sphere
  expressed in that space — so the light's own position is never a world coordinate either.

`--origin 1000000` moves everything a million units out. The image is **byte-identical** to the
image at the origin, which is `render.golden`'s second case: it compares against the same reference
file rather than against a second one, because the claim is not "it still looks right".

At a million units an `f32` position has a spacing of about 0.06 metres, and the smallest feature in
this scene is smaller than that. A renderer that narrowed before subtracting would not be slightly
wrong here; it would be visibly wrong.

## Reversed-Z, including in the shadow map

The depth buffer is `[0, 1]`, cleared to **0**, compared **GreaterOrEqual**. The shadow map is a
depth buffer like any other and obeys the same convention, so the occluder nearest the light holds
the **largest** value and the comparison sampler compares `GreaterOrEqual` too. The consequence a
reader should keep: the shadow bias is **added** to the reference, because that is the direction
that moves a fragment towards the light — under the other convention it is the direction that causes
acne rather than removing it.

`render.conventions` is what makes those numbers facts rather than comments: it samples the depth
buffer back off the device and asserts near → 1 and far → 0.

## The null backend is not a fallback

`just run-sample first-light` on a default build runs the whole frame against `cy::rhi-null`. The
declarations are the same, the derivation is the same, the pass and barrier counts are the same, and
the only thing that differs is that no draw executes. There is no `#if` anywhere in `scene.cpp` or
`renderer.cpp` and neither of them links a backend — `cy::sample-first-light` depends on `cy::rhi`,
the interface, and the host chooses.

That is design.md §1's argument made checkable, and it is why `render.null_frame` and
`render.xr_prerequisites` run on every pull request on machines that have never had a GPU.

## Why there is no window

The sample renders offscreen and writes a PPM with `--capture`. It does not present.

`DisplayServer::create_surface(window, {api = GraphicsApi::Vulkan, api_instance = ...})` exists,
works, and is task 2.3.2's deliverable — `platform/desktop-sdl3/` implements it and the RHI takes a
`native_surface` in `SwapchainDescription`. **What is missing is the middle:** `cy::rhi::Device`
exposes no way to obtain the API instance a surface must be created against. `native_handle()`
returns the `VkDevice`, and the `VkInstance` stays inside `src/backends/rhi/vulkan/`. So a host can
create a window and a device, and cannot join them.

Closing it is a small, deliberate addition to the RHI — an accessor that answers "what object would
a surface be created against on this backend", in engine-owned terms, so that no Vulkan type crosses
the layer. It belongs to whoever owns `src/backends/rhi/`, and it is recorded here and in
`tools/roadmap/milestones/m3.toml`'s notes rather than worked around, because a sample that reached
into a backend's private header to get an instance would be the thing the layer rule exists to stop.

A windowed sample is the natural first consumer of that accessor, and it is one file's change here
when it lands: swap the headless display server for the SDL3 one, create a surface, create a
swapchain, import the acquired image into the graph, and declare a `Present` pass — which the access
table already has an intent for.

## What is thinner than it looks

**One frame at a time.** `render()` waits for the device before it returns. Frames in flight,
per-frame descriptor pools and a ring of uniform buffers are all in the RHI and all exercised by the
graph's own suites; using them here would teach two things at once and the second one would be
bookkeeping. This sample is not a throughput demonstration and does not pretend to be.

**It does not use `ForwardFrame`.** `cy::rendering::ForwardFrame` declares the thirteen-stage frame
`rendering-forward-clustered` specifies, and this sample declares four passes directly. The reason
is the shadow map: `FrameDescription` has no seam for a pass-specific extra read, so the opaque
stage cannot be told that it samples a resource the frame does not know about — and an undeclared
read is exactly the thing the graph must not have. Declaring the passes here is also the more
readable teaching example. `unit.render_forward` covers the thirteen-stage frame; **a shadow-map
resource in `FrameDescription`, or a way to add a use to a declared pass, is the change that would
let a sample use both.**

**The sun is a direction, a colour and an ambient floor.** `cy::rendering::lighting` has physical
units, a shadow atlas and cascades; this sample uses none of them, because a lit scene needs one
light and the milestone's lighting work has its own suite. The tone curve is a `pow(x, 1/2.2)` in
the fragment shader rather than the post-process stage `rendering-forward-clustered` specifies, and
the shader says so at the line.

**Nothing is culled and nothing is sorted.** Eight objects, drawn in declaration order, twice each.
`cy::rendering::culling` and `render::sort_draws` exist and are tested; a sample with eight boxes
would exercise neither.

**The renderer is not wired into `Runtime::tick()`.** The host calls `runtime.tick()` and then
renders, in that order, which is `engine-architecture`'s loop — but `RuntimeConfig` has no renderer
field and the runtime's render step is still the empty seam M2 left. Closing it is a four-line
`runtime::Server` adapter around `cy::render::RenderServer`, at layer 5, which this sample does not
own. The `loop` line's `alpha=0.000` is the same fixed-step zero `samples/02-headless-sim` reported
and predicted would stay zero until something asks for a variable rate.

**The interpolation alpha is still zero, and the shader still ignores it.** Interpolating a
transform between two ticks is the renderer's job at the moment there is a variable-rate frame to
interpolate for. There is not one here.

**Windows and macOS are unverified.** The sample adds no platform-conditional code, and it has only
been run on Linux.

**Governed by**: `delivery-roadmap` (milestone artefacts), `rhi-and-render-graph`,
`rendering-architecture`, `rendering-forward-clustered`, `core-math` (reversed-Z, camera-relative,
the axis conventions), `shader-system` (the descriptor-set convention).
