# `src/rendering/pipeline/` — the layer above the frame

Layer 4. M8.c task 1b.1, and the wall M8.b's closing gate named.

**Governed by** `rendering-forward-clustered`, `shader-system`, `rhi-and-render-graph` and
`rendering-materials-and-shading`.

## Why it exists

M8.b's closing gate, verbatim:

> `FrameAssembly` hands each pass's record callback to its caller and the vertical slice supplies
> none, which is why the slice's picture is DRAWN rather than CAPTURED. A person could build all of
> M8.b and still not *see* it without writing their own renderer.

M8.b's frame is real — eight modules called in an order, thirteen stages declared into the render
graph, every barrier derived, compiled and executed on a device with validation on. And it recorded
nothing: `FrameSinks::passes` was empty in every caller in the tree, and `ForwardFrame` is explicit
that "a pass with no callback still declares its resources", which is a legitimate frame and a blank
one.

This module is the layer that was missing.

| Type | What it is |
|---|---|
| `FramePipelines` | the pipeline state objects, the three descriptor set layouts on the engine's own set convention, the pipeline layout and the sampler. Created once, never inside a frame. |
| `FrameBindings` | the ring of per-frame buffers and the descriptor sets that name them. |
| `FrameRecorder` | `sinks()` — the `FrameSinks` `FrameAssembly` has been asking for, with five real record callbacks in it. |

## The five callbacks, and what each one closes

| Stage | What it records |
|---|---|
| `Prepare` | the copy into the frame's OWN `lights` and `draw_instances` buffers. `ForwardFrame` calls that pass "the only pass that writes them"; before this, the graph derived a transfer barrier around a transfer that never happened |
| `DepthPrepass` | the opaque draws, **position stream only** — `render::kDepthPassStreams` made structural rather than documented — depth written, compared GreaterOrEqual |
| `Opaque` | the same draws with three streams, depth compared **Equal** and not written, shaded against the cluster's light list and the **GPU material table** |
| `Transparent` | the transparent layer in the sort's own order, alpha blended, depth tested and not written. Also where `PassExtension` consumers compose — `src/rendering/particles/` is the first |
| `PostProcess` | `cy/fullscreen.slang`'s **own** resolve entry points: exposure, tonemap, straight into the frame's output |

## The pictures

`docs/design/images/pipeline-frame-m8c-recorded.png` is this layer's frame, read back off an
RTX 5060. `docs/design/images/pipeline-frame-m8c-no-callbacks.png` is the identical frame — same
scene, same assembly, same graph, same device frame — with an empty `FrameSinks`, which is what the
engine did before this milestone. Both are written by `render.pipeline` on every run, from one code
path with one argument changed.

## Three things it refuses to own

**Geometry.** `GeometrySource` is the seam a caller fills: three stream buffers, an index buffer and
a per-draw lookup. The mesh table is the render server's and this module holds no copy of it, for
the same reason `FrameAssembly` holds none.

**Policy.** It creates the pipelines the `PipelineSetup` names, and the setup's formats, sample count
and feature flags are decisions `cy::rendering-arbiter` and the post chain already made.

**Shader compilation.** The SPIR-V is committed (`shaders/frame_spirv.h`) because `CY_SHADER_SLANG`
is off by default and off in Profile and Shipping, and `shader-system` requires a shipping build to
"contain compiled backend-native shader artefacts and no Slang compiler".
`src/rendering/shaders/cy/frame.slang` is the source and its header carries the invocations.

## What is measured and recorded rather than hidden

* **`rhi::Format` has no `Rgba16Snorm`**, so the normal stream is `Rgba16Sfloat` carrying the same
  octahedral pair as half floats rather than `render::PackedNormalTangent`'s 16-bit snorm form.
  `pack_normal_stream` is the bridge. Closing it means adding a format to `src/backends/rhi/`.
* **`SV_VertexID` costs a device feature.** Slang lowers it to `gl_VertexIndex - gl_BaseVertex`,
  which declares the SPIR-V `DrawParameters` capability, which a Vulkan device refuses without
  `shaderDrawParameters` — a feature `src/backends/rhi/vulkan/` does not request. `cy/frame.slang`
  and `cy/fullscreen.slang` use `SV_VulkanVertexID`; the alternative fix is in the backend.
* **Multisampled frames are refused, not mis-drawn.** `bind()` fails naming the mismatch rather than
  letting a pipeline created for one sample count meet an attachment with another.

## The suites

| Suite | Kind | What it proves |
|---|---|---|
| `integration.render_pipeline` | integration | the sinks carry five callbacks and an empty `FrameSinks` carries none; a frame WITH them records five passes and every draw, and the identical frame WITHOUT them records nothing while the assembly's own numbers are unchanged; the particle extension runs once; the ring turns over `frames_in_flight × 4 + 1` times and tears down with the device busy |
| `render.pipeline` | render | the same scene on Vulkan with validation and synchronisation validation on, the output copied back off the device, zero validation errors, and the three committed pictures written |

Both go red when `FrameRecorder::sinks()` stops attaching callbacks — which was run, not assumed.

## For the next caller — `samples/08-vertical-slice`, and anything else with a device

Task 1b.2 wants the slice's frame CAPTURED rather than drawn, and the slice's
`Presentation::update_frame` already assembles one. What it has to add is six calls, in this order,
inside the host's device frame:

```cpp
// Once, after FrameAssembly::initialize and attach_device.
PipelineSetup setup;
setup.color_format  = description.color_format;   // the assembly's own
setup.depth_format  = description.depth_format;
setup.output_format = <the swapchain's format>;
pipelines.initialize(device, setup);
bindings.initialize(device, pipelines,
                    BindingCapacity::for_grid(grid, max_draws, max_instances,
                                              material_capacity, max_lights));
recorder.initialize(pipelines, bindings);
recorder.set_geometry(<three stream buffers and a per-draw lookup>);

// Per frame, between Device::begin_frame() and Device::end_frame().
recorder.bind(assembly);
FrameSinks sinks = recorder.sinks();
sinks.surfaces = <the mesh table's surface query>;
assembly.assemble(index, view, sinks, graph, report);
bindings.upload(frame_slot, upload_for(assembly, report, projection * view, view,
                                       instance_rows, globals, material_offsets));
assembly.execute(executor, graph, report);
```

`recorder.bind()` refuses rather than mis-draws when the pipelines and the frame disagree about a
format or a sample count, so the mistake surfaces at the call rather than as a validation error a
hundred frames later. `src/rendering/pipeline/tests/frame_scene.cpp` is a complete worked example of
every one of those arguments, including the instance rows and the derived material offsets.
