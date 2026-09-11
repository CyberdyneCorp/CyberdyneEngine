# `src/vfx/` — CyberVFX

Layer 4. M8.c section 2. **Governed by** `vfx-system`.

Behind `CY_VFX`, which the specification requires the system to be removable by: *"The system SHALL
be removable at build time via `CY_VFX` without affecting the rest of the renderer."*
`-D CY_VFX=OFF` excludes this directory and its four suites; nothing else changes, and
`src/rendering/particles/` — the sprite compositing this module publishes into — is deliberately
**not** behind it.

## Two targets, and the split is a requirement

| Target | What it is | What it may name |
|---|---|---|
| `cy::vfx-compiler` | the asset model, the VFX IR, the derived attribute layout, the data-interface registry, the Slang emitter | `cy::core`, `cy::graph`. **No device, no world, no renderer** — a compiler that needed one cannot run in a cook |
| `cy::vfx` | the simulation world, the shared pool, the global scheduler, the budget controller, the event channels, the bounded readback, the CPU kernel executor, the publication | the above, plus `cy::ecs` (the determinism firewall) and `cy::rendering-particles` (the seam) |

*"The runtime SHALL contain no graph compiler"* and *"no graph interpreter"* are the reason. A
promise in a comment is not that; two targets are. Nothing reachable from `cy::vfx` can name a
`cy::graph::Graph`.

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

**What is NOT yet done: nothing in the tree compiles it automatically.** The invocation above is by
hand. A `smoke.vfx_slang` suite driving `cy::shader`'s front end is the next step and is not in this
milestone.

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

## What is honest about the GPU path

`vfx-system` makes GPU compute the default and this module's decision agrees: `decide_path` returns
`ExecutionPath::Gpu` for every emitter that can have it. **What does not exist in this tree is the
compute dispatch that would run a compiled kernel**, so `device_dispatch_available()` answers false
and every GPU-preferred emitter falls back to the CPU with
`FallbackReason::DeviceDispatchUnimplemented` — a value in `StepReport::cpu_fallbacks`, reported
every frame rather than once in a document. The kernels are compiled and their Slang is generated
and self-contained; `device_dispatch_available()` is one line and everything above it already reads
it.

`SimulationPath::CpuRequired` is **not** a fallback and is not counted as one. `vfx-system`: *"The
CPU path SHALL NOT be presented as merely a degraded GPU path, because its use cases differ."*

## Deliberate limits, recorded rather than hidden

* **A kernel writes at most `kMaxKernelWrites` (16) attributes**, because the shared core's root
  table is the domain's and a domain is a static table. Exceeding it is `OutOfRange` naming the
  emitter, never a truncation.
* **`Ribbon`, `Beam`, `Trail`, `Decal`, `Light` and `Volume` renderers are not implemented.**
  `Sprite` is `src/rendering/particles/` and `Mesh` is `publish_mesh_instances`; the other six are
  compositing work in the renderer's layer, and this module has no way to fake them.
* **Collision is not implemented.** The data interfaces it would read — `scene_sdf`,
  `scene_depth`, `physics_query` — are declared, versioned, cost-classed and cook-gated, and a graph
  can sample them; the RESPONSE (bounce, restitution, friction, sliding) does not exist. `kill` and
  `raise an event` do, so an authored graph can already express the third and fourth of the four
  responses `vfx-system` lists, against a predicate of its own rather than against a real contact.
* **A sample of an unbound data interface reads zero on the CPU path.** The cook-time gate is what
  stops an effect *depending* on an interface it cannot have; a bound-but-empty one is not an error
  and produces zero, on both paths, at the same place in each.
* **Async compute is not scheduled.** `vfx-system` requires simulation to be schedulable on an async
  queue where the device exposes one; with no device dispatch at all there is nothing to schedule,
  and `DeviceCapability::async_compute` is declared and read by nothing.
* **The editor's VFX graph editor is not built.** `vfx-system`'s authoring requirement is
  `editor-*`'s surface over this module's `CompileReport`, `AttributeLayout` and `GeneratedSource`,
  all three of which are public for exactly that reason.
* **A GPU sort is not implemented**, so `BudgetLevers::sorted` is a lever nothing yet reads.
* **An event chain is one level deep.** A raise carries `depth = 0`, so `EventRouter`'s chain-depth
  bound is exercised by `unit.vfx_values` and never reached by a compiled kernel: a graph cannot yet
  consume an event and raise another. The `Event` stage compiles, and what is missing is the
  scheduler feeding a channel's contents into it as a dispatch.
* **A curve is the identity over [0, 1]** on both paths until a cooked curve resource is bound. Both
  spellings — the CPU executor's and the generated Slang's — say so at the same place, because two
  spellings of one function is how a fallback and a GPU path come to disagree.
