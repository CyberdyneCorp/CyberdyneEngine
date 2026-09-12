# `src/vfx/` — CyberVFX

Layer 4. M8.c section 2. **Governed by** `vfx-system`.

Behind `CY_VFX`, which the specification requires the system to be removable by: *"The system SHALL
be removable at build time via `CY_VFX` without affecting the rest of the renderer."*
`-D CY_VFX=OFF` excludes this directory and its five suites; nothing else changes, and
`src/rendering/particles/` — the sprite compositing this module publishes into — is deliberately
**not** behind it.

## Two targets, and the split is a requirement

| Target | What it is | What it may name |
|---|---|---|
| `cy::vfx-compiler` | the asset model, the VFX IR, the derived attribute layout, the data-interface registry, the Slang emitter, the GPU binding contract | `cy::core`, `cy::graph`. **No device, no world, no renderer** — a compiler that needed one cannot run in a cook |
| `cy::vfx` | the simulation world, the shared pool, the global scheduler, the budget controller, the event channels, the bounded readback, the CPU kernel executor, the publications | the above, plus `cy::ecs` (the determinism firewall) and `cy::rendering-particles` (the seam). **No device and no render graph** — that absence is what keeps `integration.vfx` headless |
| `cy::vfx-gpu` | the compute dispatch: the pipelines, the device-resident particle buffers, the indirect arguments, the async queue and the sort | all of the above, plus `cy::rhi`, `cy::rendering-graph` and — where the build has one — `cy::shader-slang` |

*"The runtime SHALL contain no graph compiler"* and *"no graph interpreter"* are the reason for the
first split. A promise in a comment is not that; two targets are. Nothing reachable from `cy::vfx`
can name a `cy::graph::Graph`.

The third target exists for the reason `src/rendering/gpu_culling/` and `src/rendering/skinning/`
each record for themselves: a compute pass needs a device, the module that holds the algorithm may
not name one, and a dispatch that can only be checked by looking at a picture is a dispatch checked
by nobody. Nothing in `cy::vfx` or `cy::vfx-compiler` links back.

## The IR is VFX's own, and here is the sentence that decides it

M8.b's spike measured that one graph IR cannot serve seven consumers. For VFX the blocker is the
second of the five properties `cy/graph/expr.h` lists about the shared expression core:

> identity is content, so two reads of one mutable cell **are** one value — there is no Store

**A particle kernel writes attributes.** `position` at the end of an update step is not the
`position` at the start, and a hash-consed pure-expression DAG cannot tell them apart, because
telling them apart is exactly the property it gives up to get content identity.

So a kernel is *an ordered list of attribute writes, each of whose values is a pure expression*. The
ordered list is `include/cy/vfx/ir.h`. The values lower onto `cy::graph`'s shared core unchanged,
which is what `design.md` §1 says a particle's per-attribute expression should do.

Everything that follows from the core comes free and is required: typed and SSA-formed, constant
folding, dead-code elimination, common-subexpression elimination. The two the core **cannot** make
are statements about the write list, so they are here:

* **attribute liveness** — written by some stage, read by none (including the renderer): not
  allocated, and its store is not emitted;
* **kernel fusion** — Initialise and Update share one dispatch over the newly spawned particles, so
  the initial values never round-trip through memory.

`CompileReport` counts all four separately. Every claim in the suites is a comparison between two
cooks of one asset with one switch changed.

## What is measured rather than asserted

| Claim | Where the number is | The control |
|---|---|---|
| unused attributes cost nothing | `EmitterReport::bytes_per_particle` | cook again with `attribute_liveness` off |
| a cook-time parameter is folded | `folded_parameters`, and `cyVfxParams.gravity` absent from the Slang | cook again with `fold_parameters` off |
| fusion reduces the dispatch count | `dispatches_before_fusion` / `after` | cook again with `fuse_stages` off |
| 400 instances are few dispatches | `StepReport::dispatches_unmerged` / `merged` | — |
| a decorative effect cannot starve a critical one | `PoolReport::shortfall_particles`, and `Critical`'s reservation still acquirable | — |
| cost is bounded by configuration | per-particle CPU time at 256 and at 1024 | a quadratic step lands at 4× and fails |
| the budget controller does something visible | `vfx-budget-full.png` vs `vfx-budget-degraded.png` | delete the levers and the two pictures become identical |
| an authored graph raises a bounded event | `ChannelReport::raised` / `delivered` / `dropped` | — |

### The adversarial pass, run

Five mutations, each applied to the built tree and each turning a specific check red. Every one was
run; none is a prediction.

| Mutation | What went red |
|---|---|
| `eliminate_dead` never called | five checks in `integration.vfx_compiler`, including the two that read the generated Slang |
| the fusion substitution disabled | `integration.vfx`'s first-frame velocity, which is the fusion as a number |
| the `WriteScope` removed from `ReadbackQueue::deliver` | the firewall-origin check |
| the pool's reservation subtraction removed | four checks: the decorative request is no longer reduced and `Critical` loses its reservation |
| `levers_for` ignored in `simulate_instance` | the device pair goes to **0 texels differ**, and `integration.vfx`'s population comparison goes flat |

The last one is why there are two checks for it: the first version of that mutation left every
device assertion about counts green and only the *picture* changed. A machine with no GPU has to
catch it, so `integration.vfx` compares the populations too.

### M10's own adversarial pass, run

Thirteen mutations over the GPU dispatch and the renderer kinds, each applied to the built tree and
each run. Twelve turned a check red on the first attempt; the thirteenth did not, and what it found
is in the row below it.

| Mutation | What went red |
|---|---|
| `cyVfxArgs[0]` written as a constant `1` instead of the live group count | the CPU/GPU value comparison: 8.2e-01 against a 1e-05 tolerance |
| the sort's write-back to `cyVfxIndices` removed | 181 of 368 pairs out of order |
| the compaction's free-list order perturbed | the population, the liveness array and the values — three checks |
| `gpu_array_base_words` returns 0 for every attribute | six checks across three cases, including `color` at a full 255 steps |
| `queue_` forced to `Graphics` | the async case, on a machine whose driver exposes the queue |
| `cyVfxKill`'s population decrement removed | the population comparison at 96 sub-steps — and **not** at 16, which is why it runs 96 |
| the cap kill removed from `vfx_compact` | **nothing, on the first attempt.** A population *growing* into a cap stops at it anyway, because the free list never offers a slot above it. The case that catches it fills the block and *then* reduces the cap, which is the direction `vfx-system` actually means; with it, the mutation kills 11 instead of 383 and the check goes red |
| the ribbon's chain-break test removed | five checks, including the one that asserts slot 6 and slot 8 are not consecutive in one strip |
| `count_cap_scale` dropped from the light budget | three checks |
| the light budget keeps the first N it walks | the ranking check — which is why the case publishes twice from two camera positions |
| `history.sample`'s absence no longer suppresses | every row of the spawn frame |
| the renderer dropped from the cook key | a ribbon and a trail over one graph share a key |
| liveness no longer keeps a `Decal`'s `velocity` | the projection axis — see below |

The last row is a defect this pass found rather than a mutation of a deliberate choice. `Decal`'s
publication derives its projection axis from `velocity`; attribute liveness had elided it, because
until the emitter declared its renderer the compiler's live-output list could not depend on which
renderer would read it. `renderer_inputs` now branches on the declared kind, which is what makes
`Emitter::set_renderer` load-bearing in the **compiler** and not only in the publication.

## The generated Slang compiles, and that was run rather than asserted

`integration.vfx_compiler` writes the assembled translation unit to `vfx-kernel.slang` in its
working directory every run. Handed to the pinned Slang compiler:

```
$ slangc -target spirv -entry cyVfxKernel -stage compute vfx-kernel.slang -o out.spv
$ echo $?
0
```

11 364 bytes in, 14 256 bytes of SPIR-V out, no diagnostics — including the attribute accessors at
the precisions the layout chose (`f16tof32`, `cyVfxPackUnorm8`), the folded `gravity`, the elided
`scratch` store's absence, and the event append bounded by the channel's own declared maximum. That is `material-compiler`'s recorded
M7 gap — *"the bundle carries the IR, the generated source, the cost report and the cook key, and
nothing invokes the compiler"* — closed for VFX, and it is closed by the unit importing **nothing**:
a generated program that needed a nested authored import would be blocked on this milestone's
finding C, which is a defect in `src/backends/shader/slang/`'s file system and below this layer.

**M8.c left this saying "nothing in the tree compiles it automatically", and that is now false.**
`VfxGpuPass::create` hands the unit `assemble_dispatch_unit` produces to `cy::shader`'s Slang front
end with `SourceOrigin::Generated` and the generator named `vfx-graph`, so a diagnostic can tell
"the shader is broken" from "the generator emitted broken Slang". `render.vfx_gpu` runs it every
time it creates a pass.

The `slangc` invocation above is still run by hand, and `integration.vfx_compiler` still writes the
three units it compares — `vfx-kernel.slang` (the probe), `vfx-dispatch.slang` (what a frame
dispatches) and `vfx-sampler.slang` (an effect that samples a data interface) — so that all three
can be put in front of a compiler without a device.

**One thing that claim was quietly false about until M10**: a data-interface sampler was emitted as a
*declaration with no body*, so "self-contained translation unit" held only for an effect that
sampled nothing — and the plume samples nothing, so every suite agreed. The body is now the same
answer the CPU executor gives an unbound interface, zero, and
`integration.vfx_compiler` has a case with an effect that samples.

## The pictures

Engine output, read back off a Vulkan device with validation on, written by `render.vfx`:

* `docs/design/images/vfx-simulation.png` — the effect mid-flight, simulated by
  `SimulationWorld`, published through `publish_sprites`, drawn in one draw.
* `docs/design/images/vfx-budget-full.png` and `docs/design/images/vfx-budget-degraded.png` —
  the same frame, the same camera, the same seed; the one difference is what the budget controller
  was told VFX cost.

## Using it, in the order the pieces expect

```cpp
NodeRegistry registry(allocator);
DataInterfaceRegistry interfaces(allocator);
register_vfx_nodes(registry);            // the node library the stage graphs are authored from
register_builtin_interfaces(interfaces); // scene depth, the SDF, the shared wind field, ...

VfxSystemAsset asset = /* emitters, parameters, channels, importance, scalability policy */;
asset.resolve(registry);                 // CyberGraph's load-time step — see below

DiagnosticSink sink(allocator);
CompileReport report(allocator);
auto system = compile_system(asset, registry, interfaces, CompileOptions{}, sink, report);

SimulationWorld world(allocator);
world.initialize(WorldDescription{});
world.play(*system, EffectSpawn{});      // declares the system's event channels on the router
world.step(1.0F / 60.0F, step_report);

Array<particles::ParticleInstance> records(allocator);
publish_sprites(world, camera_position, ring_capacity, records, publish_report);
// ... renderer.upload(frame_slot, records.span()); recorder.add_extension(renderer.extension());
```

### And on the GPU, where a host has a device

```cpp
VfxGpuPass pass;                                 // one per emitter: one kernel, one layout
pass.create(allocator, device, *system, GpuPassDescription{});
// once, in a frame of its own: the reset is a dispatch, because the block is device-local
pass.declare_reset(graph);

// per sub-step
GpuStepInputs inputs;
inputs.dt = substep;
inputs.levers = controller.levers_for(system->importance(), system->scalability());
inputs.parameters = instance_parameter_words;   // the same packing the CPU path reads
pass.step(inputs);                              // takes NO population — there is nowhere to put one
pass.declare(graph);                            // spawn, compact, initialise*, update*, keys*, sort
                                                //   * indirect, from counts the device maintains
```

### And the renderer kinds beyond `Sprite`

```cpp
PublicationHistory history(allocator);           // the caller's, not the world's: two cameras, two
history.resize(block_capacity, 4);               //   histories, and that is correct

RendererDecl decl;                               // what the emitter declared, plus the controller's
decl.kind = RendererKind::Ribbon;                //   feature level where a host overrides it
Array<RibbonVertex> strips(allocator);
RenderPublishReport report;
publish_ribbons(world, decl, camera_position, capacity, history, strips, report);
```

**`asset.resolve(registry)` is not optional and `compile_system` cannot do it for you** — the asset
is const there, and resolution is a load-time mutation of the authored graph. Skipping it produces
a page of *"this node's type is not registered"*, which blames the author for the caller's omission;
it was the first thing this module's own suite got wrong.

## The determinism firewall

VFX does not enforce it and must not. `ReadbackQueue::deliver` opens
`cy::ecs::WriteScope(WriteOrigin::Vfx, "vfx.collision-readback")` around the caller's sink, so any
ECS write the sink reaches — directly or four frames deep in gameplay code — arrives at the firewall
labelled as VFX's, and the ECS refuses it. *VFX declares what it is; it does not decide what it is
allowed to do.* A subsystem that asked for permission and then wrote anyway would be a subsystem
whose check is decorative.

## The GPU path, which exists since M10 — `cy::vfx-gpu`

M8.c left this section saying *"what does not exist in this tree is the compute dispatch that would
run a compiled kernel"*. `src/vfx/gpu/` is that dispatch. `device_dispatch_available()` answers
**true**, and `decide_path` on a capable device returns `ExecutionPath::Gpu` with no reason attached.

| | |
|---|---|
| `include/cy/vfx/gpu_layout.h` | the binding contract — nine bindings, one push block, the counter table, the GPU block's word arithmetic. In `cy::vfx-compiler`, because the *generator* must emit against it and a compiler may not name a device |
| `gpu/shaders/vfx_support.slang` | the three dispatches that are the same for every effect: the reset, the compaction, the sort. Checked-in SPIR-V, like `gpu_culling`'s and `skinning`'s |
| `gpu/include/.../gpu_pass.h` | `VfxGpuPass`: the device, the pipelines, the buffers, and the graph declaration |

**The simulation kernel is not embedded and cannot be**: it is generated per effect by
`assemble_dispatch_unit`, and `VfxGpuPass::create` compiles it through `cy::shader`'s Slang front
end. A build with `CY_SHADER_SLANG` off must be handed the module through
`GpuPassDescription::kernel_spirv`, and `create()` fails naming the missing front end when it is
given neither. **Nothing in this tree cooks that module yet** — the cook step is
`asset-import-pipeline`'s — so a Profile or Shipping build can run this pass only once something
cooks for it, and the error says so rather than a document.

### The four sentences it is accountable for, and where each one is a number

| `vfx-system` | Where it is | The control |
|---|---|---|
| "particle state SHALL live in GPU buffers and remain there" | every buffer is `MemoryUse::DeviceLocal` and there is no host pointer to one; `read_back_particles()` is off unless the suite asks | — |
| "indirect dispatch driven by live particle counts **maintained on the GPU**" | `GpuStepReport::indirect_dispatches`, and `VfxGpuPass::step` takes no population | set `cyVfxArgs[0]` to a constant: the value comparison against the CPU executor goes to 8.2e-01 |
| "where the device exposes an async compute queue, VFX simulation SHALL be schedulable on it" | `VfxGpuPass::queue()`, gated on `Device::has_queue` | force `Graphics`: the async case goes red on a machine that has the queue |
| "distance sorting SHALL be performed on the GPU ... budgeted and reducible" | `GpuStepReport::sort_passes` — 45 for a 512-particle block, 0 when `BudgetLevers::sorted` is false | drop the sort's write-back: 181 of 368 pairs go out of order |

### What `render.vfx_gpu` compares, and why it is not a picture

Every case runs one cooked effect **twice** — once through `SimulationWorld`'s CPU executor and once
through `VfxGpuPass` — and compares the liveness array slot for slot and the attribute values
particle by particle over 96 sub-steps, long enough that particles expire and their slots are reused.

The liveness comparison is **exact**, which is why `vfx_compact` is a single workgroup with a
shared-memory prefix scan rather than an atomic append: an atomic append produces whatever order the
hardware scheduled, and a live list in that order cannot be compared against the CPU's ascending
one. Measured worst relative difference on the reference machine: **2.4e-07** over `position`,
`velocity` and `age`; `color`, which the compiler put at `Unorm8`, agrees to **zero** steps of 1/255.

### Two defects this comparison found, which reading the code did not

* **The GPU skipped one update per particle.** The CPU marks a fused-initialised particle `2` so the
  same pass does not advance it twice; the GPU's compaction runs *before* the spawn, so the
  exclusion is structural and the marker made it an exclusion twice. Worst relative difference was
  9.7e-02 before the fix and 2.4e-07 after.
* **`reported_live` over-counted by a sub-step's kills**, because the compaction sets it before the
  update runs. `cyVfxKill` now decrements it. Invisible at 16 sub-steps — nothing had died yet —
  which is why the case runs 96.

`SimulationPath::CpuRequired` is **not** a fallback and is not counted as one. `vfx-system`: *"The
CPU path SHALL NOT be presented as merely a degraded GPU path, because its use cases differ."*

**A `SimulationWorld` is still the CPU path and must be.** It holds no device — that is what keeps
`integration.vfx` headless — so every emitter it steps reports
`FallbackReason::NoDeviceInThisWorld`, which replaced `DeviceDispatchUnimplemented` when that stopped
being true. A host that wants the GPU path drives a `VfxGpuPass` per emitter from its own frame.

## Deliberate limits, recorded rather than hidden

* **A kernel writes at most `kMaxKernelWrites` (16) attributes**, because the shared core's root
  table is the domain's and a domain is a static table. Exceeding it is `OutOfRange` naming the
  emitter, never a truncation.
* **The six renderer kinds beyond `Sprite` and `Mesh` publish rows; nothing composites them.**
  `renderers.h` derives the geometry and the instances each kind needs from particle state —
  `publish_ribbons`, `publish_trails`, `publish_beams`, `publish_decals`, `publish_lights`,
  `publish_volumes` — because that derivation is the simulation's and a renderer would be wrong to
  invent it. What is still absent is the compositing: no pass draws a ribbon strip, projects a decal
  or feeds a `LightInstance` to clustered assignment. The half M8.c called "compositing work in the
  renderer's layer" is still exactly that, and the half it used as the reason not to start is done.
* **Collision is not implemented.** The data interfaces it would read — `scene_sdf`,
  `scene_depth`, `physics_query` — are declared, versioned, cost-classed and cook-gated, and a graph
  can sample them; the RESPONSE (bounce, restitution, friction, sliding) does not exist. `kill` and
  `raise an event` do, so an authored graph can already express the third and fourth of the four
  responses `vfx-system` lists, against a predicate of its own rather than against a real contact.
* **A sample of an unbound data interface reads zero on the CPU path.** The cook-time gate is what
  stops an effect *depending* on an interface it cannot have; a bound-but-empty one is not an error
  and produces zero, on both paths, at the same place in each.
* **A GPU event chain is still one level deep**, and the `Event` stage has no dispatch of its own.
  `cyVfxRaise_*` appends into the shared ring and `kGpuCountEventsBase + channel` counts the
  overflow, so a raise is a GPU-side append with both of its declared bounds enforced — but nothing
  feeds a channel's contents back in as a dispatch, so "a bullet impact spawns sparks, and a spark
  collision spawns dust" is one link short.
* **The editor's VFX graph editor is not built.** `vfx-system`'s authoring requirement is
  `editor-*`'s surface over this module's `CompileReport`, `AttributeLayout` and `GeneratedSource`,
  all three of which are public for exactly that reason.
* **The GPU sort is bounded at `kGpuSortCapacity` (2048) particles a block**, because it is one
  workgroup over group-shared memory — one render-graph pass rather than the 66 a global bitonic
  sort would need, and the RHI deliberately exposes no barrier outside the graph's executor. A block
  larger than that is REFUSED the sort and says so in `GpuStepReport::sort_refused`, rather than
  being sorted partially.
* **An event chain is one level deep.** A raise carries `depth = 0`, so `EventRouter`'s chain-depth
  bound is exercised by `unit.vfx_values` and never reached by a compiled kernel: a graph cannot yet
  consume an event and raise another. The `Event` stage compiles, and what is missing is the
  scheduler feeding a channel's contents into it as a dispatch.
* **A curve is the identity over [0, 1]** on both paths until a cooked curve resource is bound. Both
  spellings — the CPU executor's and the generated Slang's — say so at the same place, because two
  spellings of one function is how a fallback and a GPU path come to disagree.
