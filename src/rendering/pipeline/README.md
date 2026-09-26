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
| `FrameRecorder` | `sinks()` — the `FrameSinks` `FrameAssembly` has been asking for, with six real record callbacks in it, and a seventh for bloom when a `BloomRenderer` is attached. |
| `BloomRenderer` | bloom's four pipelines over the frame's full-screen vertex stage, its pass set (set 2), and the recording of every step `forward/bloom_chain.h` declares. `shaders/embed_bloom.py` regenerates its committed SPIR-V and MSL. |

## The six callbacks, and what each one closes

| Stage | What it records |
|---|---|
| `Prepare` | the copy into the frame's OWN `lights` and `draw_instances` buffers. `ForwardFrame` calls that pass "the only pass that writes them"; before this, the graph derived a transfer barrier around a transfer that never happened |
| `DepthPrepass` | the opaque draws, with normal and derived camera motion when the temporal path requests them; depth is written and compared GreaterOrEqual |
| `Opaque` | the same draws with three streams, depth compared **Equal** and not written, shaded against the cluster's light list and the **GPU material table** |
| `Transparent` | the transparent layer in the sort's own order, alpha blended, depth tested and not written. Also where `PassExtension` consumers compose — `src/rendering/particles/` is the first |
| `Temporal` | reprojects the previous completed RGBA16F history, clamps it to the current 3x3 neighbourhood, and writes the other retained history image before tone mapping |
| `Bloom` | with `set_bloom`: `cy/bloom.slang`'s prefilter, downsamples, upsamples and composite, one graph pass each; `bind()` refuses a frame whose chain has bloom and no renderer |
| `PostProcess` | `cy/fullscreen.slang`'s **own** resolve entry points: exposure, tonemap, straight into the frame's output — reading `FrameResources::post_source`, which is the bloomed colour when bloom ran |

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

## Set 0 carries the globals block and the material texture table — M11.c task 3.7

The frame's fragment shader samples `cyMaterialTextures[]` out of the engine's global texture table,
and until this task it could not. The pieces were all built and none of them met:
`cy/material.slang` declared the array at **(set 0, binding 1)** and its sampler at **(set 0,
binding 2)**, `rhi::Device::global_texture_table()` filled that set, and `MaterialTextureTable` made
cooked pixels resident in it — while this module's set 0 carried `cy/globals.slang`'s block at
binding 0 and nothing else. **A pipeline binds one set per index**, so a program that wants both
needs a set 0 that has both. `FramePipelines::create_layouts` now builds that set, and
`FrameBindings::write_sets` writes both halves of it every frame, because the set is allocated every
frame.

**The slot index stays the device's.** `FrameBindings::set_material_textures` writes each resident
view at `array_index = slot`, and the slot is what `Device::bind_texture_globally` handed out — this
module allocates none of its own. So a material's slot word means the same thing to the frame's set,
to the device's own table, and to any other consumer of either; the two sets differ in which
descriptors are written, never in what a number means.

**The macOS viewport's answer was read first and could not be lifted.** `samples/03-first-light`
solved the same problem two days earlier by giving its compiled-material path a SECOND pipeline
layout — the device's table at set 0, the material's parameters at set 1, and its own globals moved
to set 2 — which works because a sample owns its shaders and can renumber them. The frame cannot:
`cy/globals.slang` fixes the globals block at (set 0, binding 0) for every shader in the engine, and
sets 1 and 2 are the view and the pass. Moving the frame's globals would move them for
`cy/fullscreen.slang` and everything else compiled against the standard library, so the set that
carries both is the only arrangement that leaves the convention intact.

**A frame that says nothing samples nothing.** `FrameViewData::material_textures` defaults to
`kNoMaterialTexture`, so a caller written before the field existed uploads the frame it always
uploaded and photographs the picture it always photographed. `apply_standard_defaults` now writes
the same sentinel into every texture slot of a material, which the comment there had claimed since
M3 while the block actually held zero — and zero is a perfectly good slot of the global table.

**Metal now captures the frame.** The view resources in `cy/frame.slang` are one `ParameterBlock`,
matching the Metal RHI's argument buffer for set 1. Slang compacts the MSL entry-point indices when
other sets are unused; `shaders/embed_msl.py` checks and remaps the view to buffer 1 and the draw
push constant to buffer 3. The captured geometry is committed as
`docs/design/images/pipeline-frame-metal-recorded.png`. Metal still shades material constants: its
bounded material texture argument buffer remains separate work.

## The ambient occlusion term — `FrameViewData::occlusion_control`

Appended to the view block, defaulted to "none", so every caller that predates it uploads the frame
it always uploaded. A caller that runs `occlusion::AmbientOcclusionPass` names its target at a slot
of set 0's texture table (`FrameBindings::set_material_textures`) and writes the slot into `.x`;
`cy/frame.slang`'s forward fragment then multiplies the AMBIENT term by the visibility and leaves the
direct sum alone unless `.y` asks for the non-physical option. The six `frame.slang` entry points
were regenerated for the longer block; the fullscreen resolve and temporal entries are unchanged.
`render.ambient_occlusion` renders this module's scene with the stage on.

## The irradiance volume — `FrameViewData::probe_volume_*`

Three words appended after `occlusion_control` (the block is 480 bytes), defaulted to "none". A
caller that runs `light_probes::ProbeVolumeTexture` names the texture at a slot of set 0's texture
table and calls `light_probes::write_probe_volume`, which writes the slot and grid size, the volume's
origin RELATIVE TO THE CAMERA and its spacing, and the coefficient scale, normal offset and
visibility slack. `cy/frame.slang`'s forward fragment then takes the ambient radiance from
`probeVolumeAmbient` in place of the flat sky term, and ambient occlusion multiplies it as before.
The six `frame.slang` entries were regenerated for the longer block; the fullscreen resolve and
temporal entries are unchanged. `render.light_probes` renders this module's scene with it.

## What is measured and recorded rather than hidden

* **`rhi::Format` has no `Rgba16Snorm`**, so the normal stream is `Rgba16Sfloat` carrying the same
  octahedral pair as half floats rather than `render::PackedNormalTangent`'s 16-bit snorm form.
  `pack_normal_stream` is the bridge. Closing it means adding a format to `src/backends/rhi/`.
* **`SV_VertexID` costs a device feature, and the device carries it.** Slang lowers it to
  `gl_VertexIndex - gl_BaseVertex`, which declares the SPIR-V `DrawParameters` capability, so
  `src/backends/rhi/vulkan/` creates its device with `shaderDrawParameters` and refuses one without
  it. Until M11.d `cy/fullscreen.slang` took `SV_VulkanVertexID` instead, which needs no feature and
  which DXC rejects (`m11c:every-shader-reaches-every-target`). The subtraction is of zero because
  every draw of the resolve and the temporal resolve is `draw(3, 1, 0, 0)` — the one base on which
  SPIR-V and Metal agree about the index — and `integration.render_pipeline`'s *every procedural
  draw starts at vertex and instance zero* holds every procedural draw of the frame to it.
* **Multisampled frames are refused, not mis-drawn.** `bind()` fails naming the mismatch rather than
  letting a pipeline created for one sample count meet an attachment with another.

## The suites

| Suite | Kind | What it proves |
|---|---|---|
| `integration.render_pipeline` | integration | the sinks carry six callbacks, including temporal resolve; a frame with them records every draw while an empty `FrameSinks` records nothing; the history and upload rings turn over beyond the frames-in-flight count; with bloom in the chain every step is recorded and the post-process reads the bloomed colour, and a chain with bloom and no renderer is refused |
| `render.pipeline` / `render.pipeline_metal` | render | Vulkan and Metal compare captured pixels, including a shaded frame against the same frame with no record callbacks, and measure temporal accumulation and determinism. Mixed backend builds register both suites; a machine without a requested backend is reported as skipped by CTest. |
| `render.bloom` | render | bloom on Vulkan, measured on the linear HDR the composite writes: a bright block's halo falls off and is centred on it; intensity zero and a scene below the knee return the input bit for bit; the frame never gains energy and the halo gives most of the threshold's energy back; the Karis average suppresses a single-texel firefly and holds it stable under a one-texel shift; anamorphic and lens dirt; the assembled `FrameScene` with bloom at zero is texel-identical to the frame without it; and with bloom off it matches `tests/references/frame_scene_before_bloom.png`, captured before bloom existed |
| `render.forward_material_texture` | render | the same scene rendered three times on a Vulkan device — every texture slot unbound, a pattern bound as every material's base colour, and **that texture replaced by its declared average** — and the differences between the three pictures. Vulkan only, because the case needs a device with a global bindless table |

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
