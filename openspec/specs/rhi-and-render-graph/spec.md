# rhi-and-render-graph Specification

## Purpose

Defines the graphics abstraction: an explicit, Vulkan-shaped **RHI** (Rendering Hardware
Interface) and the **render graph** layered on it that computes barriers, transitions, and
resource lifetimes automatically.

Backends ship in order: **Vulkan** first, **Metal** second, **D3D12** later. The RHI is shaped
around Vulkan because it is the most explicit of the three; mapping down to Metal is
straightforward, mapping up from a less explicit API would not be.

(Influence: Godot's `RenderingDevice` + `RenderingDeviceGraph`; Unreal's RDG.)

## Requirements

### Requirement: Explicit RHI
The RHI SHALL expose: buffers, textures with views and subresource ranges, samplers,
framebuffers and render passes with subpasses, shader modules, pipeline layouts, descriptor
sets, graphics/compute pipelines, command buffers, queues, fences, semaphores, timestamp
queries, and debug labels.

Resources SHALL be created from descriptor structs using designated initializers, and addressed
by generational handles rather than pointers.

Hard limits SHALL be defined and asserted: maximum descriptor sets bound simultaneously (**8**),
maximum push-constant size (**128 bytes**), maximum vertex attributes (**16**), maximum colour
attachments (**8**).

#### Scenario: Descriptor exceeds a limit
- **WHEN** a pipeline requests more descriptor sets than the limit
- **THEN** creation SHALL fail with a diagnostic naming the limit, at creation time rather than
  at draw time

#### Scenario: Handle-based resources
- **WHEN** a texture is destroyed and its slot reused
- **THEN** a stale texture handle SHALL fail validation rather than aliasing the new texture

### Requirement: No manual barriers in user-facing code
The RHI's public recording API SHALL NOT expose barriers, image layout transitions, or queue
ownership transfers. These SHALL be computed by the render graph.

Backends MAY expose an escape hatch for backend-specific work, documented as unsafe and excluded
from portability guarantees.

#### Scenario: Renderer never writes a barrier
- **WHEN** a render pass reads a texture a previous compute pass wrote
- **THEN** the required barrier SHALL be inserted by the graph, with no barrier code in the
  renderer

### Requirement: Render graph
The render graph SHALL be rebuilt each frame from a declarative description of passes and their
resource usage.

A pass SHALL declare: its kind (raster, compute, copy, present), the resources it reads and
writes with access types, its attachments with load and store operations, and an execution
callback that records commands.

The graph SHALL:
1. **Build** — collect passes and resource declarations
2. **Cull** — remove passes whose outputs are never consumed and which have no side effect flag
3. **Schedule** — topologically order passes and compute a level per pass for potential overlap
4. **Alias** — assign transient resources to a memory pool, reusing memory whose lifetimes do not
   overlap
5. **Synchronise** — insert barriers, layout transitions, and semaphores from usage transitions
6. **Execute** — record commands, optionally on multiple threads

**Aliasing creates dependencies the resource graph cannot see, and they SHALL be added before
submits are cut.** The pass that first uses a transient SHALL be ordered after the last use of every
transient whose memory it reuses; across queues that ordering can only be a semaphore, so the alias
edges SHALL exist before the schedule is cut into submissions. The schedule step therefore runs
again after aliasing: `cull → schedule → lifetimes → place → add alias edges → re-schedule → derive`.

**Every cross-queue transition of an exclusive resource SHALL carry a queue-family ownership
transfer** — a release recorded at the end of the producing submission and an acquire in the
consuming pass — ordered by the semaphore between the two submissions. A pipeline barrier alone
SHALL NOT be treated as synchronising two command streams.

Both of the above SHALL be **structurally guaranteed by the derivation**, and SHALL NOT be left to
be discovered by a validation layer or by a rendered frame. See the "Validation and debugging"
requirement for the measurements that make this obligatory rather than advisory.

#### Scenario: Unused pass is culled
- **WHEN** a debug visualisation pass writes a texture nothing samples and is not marked
  side-effecting
- **THEN** the graph SHALL remove it, and the renderer SHALL not need to branch on the debug flag

#### Scenario: Transient memory is aliased
- **WHEN** two intermediate render targets have non-overlapping lifetimes
- **THEN** they SHALL share memory, reducing peak GPU memory

#### Scenario: Write-after-read is synchronised
- **WHEN** a compute pass writes a texture a previous raster pass sampled
- **THEN** the graph SHALL insert the barrier with correct source and destination stage and
  access masks

#### Scenario: Attachment store is elided
- **WHEN** a render target's contents are not read after the pass
- **THEN** its store operation SHALL be set to `DontCare`, which matters greatly on tiled GPUs

#### Scenario: Async compute
- **WHEN** a compute pass has no dependency on the graphics work running alongside it and the
  device exposes an async compute queue
- **THEN** the graph MAY schedule it on that queue with semaphore synchronisation

#### Scenario: Aliased transients on two queues are ordered
- **WHEN** two independent pass chains on different queues use transients the aliaser placed on the
  same bytes
- **THEN** the graph SHALL emit an alias edge between them, and that edge SHALL become a semaphore
  because the two chains are in different submissions — the plan SHALL be asserted to contain it,
  rather than the frame being inspected for corruption

#### Scenario: Cross-queue read acquires ownership
- **WHEN** a pass on the graphics queue reads an exclusive resource a pass on the compute queue
  wrote
- **THEN** the derived plan SHALL contain a release in the producing submission and a matching
  acquire in the consuming pass, with the semaphore that orders them — and a plan carrying only a
  plain barrier SHALL fail the graph's own tests

### Requirement: Parallel command recording
The graph SHALL support recording independent passes, and large draw lists within a pass, on
multiple job workers into secondary command buffers, joined before submission.

Recording SHALL be deterministic: the same frame description SHALL produce the same command
stream regardless of thread scheduling.

#### Scenario: Large pass is split
- **WHEN** a pass contains more draws than a configurable threshold
- **THEN** it SHALL be split into ranges recorded in parallel and executed in order

### Requirement: Resource lifetime and frames in flight
The RHI SHALL support `frames_in_flight` (default 2) concurrent GPU frames, with per-frame
descriptor pools, command pools, staging buffers, and transient memory.

Destroying a resource SHALL defer the actual release until the GPU has finished all frames that
could reference it.

#### Scenario: Resource destroyed while in use
- **WHEN** a texture is destroyed during frame N
- **THEN** its memory SHALL be released only after frame N's fence has signalled

#### Scenario: Frame pacing
- **WHEN** the CPU is `frames_in_flight` frames ahead
- **THEN** it SHALL wait on the oldest frame's fence before reusing that frame's pools

### Requirement: Shader modules and pipelines
Shaders SHALL be consumed as **SPIR-V**, reflected at load to extract descriptor bindings,
push-constant ranges, vertex inputs, specialization constants, and compute workgroup size.

Backends that do not consume SPIR-V natively SHALL translate it offline where possible (Metal:
SPIRV-Cross to MSL, cooked at build time) and cache the result.

Pipelines SHALL be cached by a hash of their full state, and the engine SHALL persist a pipeline
cache across runs.

#### Scenario: Pipeline cache warm start
- **WHEN** the game starts with a valid on-disk pipeline cache
- **THEN** pipeline creation SHALL be near-instant and no first-use compilation hitch SHALL occur

#### Scenario: Cache invalidated by driver update
- **WHEN** the GPU driver version changes
- **THEN** the cache key SHALL differ and pipelines SHALL be recompiled and re-cached

#### Scenario: Specialization over permutation
- **WHEN** a shader feature can be expressed as a specialization constant
- **THEN** it SHALL be, rather than compiling a separate preprocessor permutation

### Requirement: Descriptor management
**Bindless SHALL be the default resource model.** Textures, samplers, and buffers SHALL be
addressed by index into global descriptor arrays, and shaders SHALL reach resources through the
GPU scene and the GPU material table rather than through per-draw binding.

This is architectural, not an optimisation: draw workloads generated on the GPU from the GPU
scene have no CPU in the loop to bind a descriptor set per draw. GPU-driven rendering requires
bindless.

The RHI SHALL support classic descriptor sets as a **compatibility path** for devices lacking the
required capabilities. The compatibility path's limitations SHALL be documented: it cannot execute
fully GPU-generated draw workloads, and therefore constrains virtual geometry and GPU-driven
culling to a CPU-submitted approximation with reduced instance and cluster counts.

Where a backend's bindless model differs from the engine's, the RHI SHALL emulate the engine's
model rather than exposing the difference upward.

#### Scenario: Bindless material access
- **WHEN** shading reaches a material
- **THEN** it SHALL index the global descriptor arrays through the material table, so a material
  change requires no descriptor rebinding

#### Scenario: Fallback path
- **WHEN** the device lacks the required bindless capabilities
- **THEN** the engine SHALL use the compatibility path with per-material descriptor sets, the
  renderer's structure SHALL be unchanged, and the reduced GPU-driven capability SHALL be reported
  rather than silently degrading

#### Scenario: Backend differences do not leak
- **WHEN** a backend expresses bindless differently from the engine's model
- **THEN** the RHI SHALL emulate the engine's model, and renderer code SHALL be unaware of the
  difference

### Requirement: Memory management
The RHI SHALL manage GPU memory through a suballocating allocator with pools per memory type,
dedicated allocations for large resources, defragmentation for transient pools, and budget
tracking against device-reported limits.

GPU memory SHALL be reported into the engine's **memory domain and budget tree** (see
`core-memory-and-containers`) as the `GPU` domain with sub-domains for persistent, streaming,
upload and readback, and transient graph memory — so that GPU and CPU memory are visible in one
model rather than two unrelated reports.

GPU memory pressure SHALL raise the engine's **pressure level** so that streaming and residency
systems respond through the same mechanism they use for CPU memory, rather than each polling the
device budget.

Resource destruction SHALL use the engine's **retirement and epoch** mechanism rather than a
GPU-specific deferral scheme.

Uploads SHALL go through a ring staging buffer; devices with host-visible device-local memory
(unified memory, resizable BAR) SHALL be able to write directly.

#### Scenario: Budget exceeded
- **WHEN** GPU memory allocation approaches the device budget
- **THEN** the engine SHALL raise pressure, trigger streaming eviction through the shared
  mechanism, and fail the allocation gracefully rather than crashing

#### Scenario: Unified memory
- **WHEN** the device exposes host-visible device-local memory
- **THEN** per-frame instance data SHALL be written directly, skipping the staging copy

#### Scenario: One memory report
- **WHEN** memory is reported
- **THEN** GPU memory SHALL appear in the same domain and budget model as CPU memory

### Requirement: Backend capability model
Every optional capability SHALL be queryable: compute, geometry and tessellation shaders,
async compute, bindless, mesh shaders, ray tracing, variable-rate shading, multiview, 64-bit
atomics, subgroup operations and their size, sparse resources, timestamp queries, and per-format
support.

The renderer SHALL branch on capabilities, never on backend identity.

#### Scenario: Capability, not backend
- **WHEN** a feature needs subgroup ballot
- **THEN** the renderer SHALL check the subgroup capability, not whether the backend is Vulkan

#### Scenario: Missing capability degrades
- **WHEN** variable-rate shading is unavailable
- **THEN** the VRS pass SHALL be skipped and rendering SHALL proceed at full rate

### Requirement: Backend roadmap
| Backend | Status | Platforms |
|---|---|---|
| Vulkan 1.3 | Primary; first to ship | Linux, Windows, Android |
| Metal 3 | Second | macOS, iOS, visionOS |
| D3D12 | Later | Windows |

MoltenVK SHALL NOT be the long-term Apple strategy; a native Metal backend SHALL be built so
tile memory, memoryless attachments, and MetalFX are usable directly.

Vulkan 1.3 SHALL be the minimum, permitting dynamic rendering, synchronisation2, and timeline
semaphores rather than maintaining fallbacks for older versions.

#### Scenario: Feature requires a newer version
- **WHEN** a capability requires an extension beyond the baseline
- **THEN** it SHALL be optional and capability-gated, with the baseline path still correct

### Requirement: Validation and debugging
Development builds SHALL enable backend validation layers, name every resource for debugging
tools, emit debug labels per render graph pass, and support RenderDoc, PIX, and Xcode GPU capture.

Backend **synchronisation validation** is a separate switch from the validation layers and SHALL be
enabled explicitly where the backend offers it. With the layers on and synchronisation validation
off, none of the synchronisation controls below fire, and a suite that never enabled it would report
a clean run over a frame full of hazards.

The graph SHALL be able to dump its structure — passes, resources, lifetimes, barriers, aliasing
decisions — as text or a Graphviz diagram.

**WHAT VALIDATION DOES NOT POLICE — measured, not assumed.** M3's spike ran negative controls on an
RTX 5060 under Vulkan 1.4 with `SYNCHRONIZATION_VALIDATION` on across two queue families. Two of
them **did not fire**:

| Defect deliberately introduced | What validation reported | What the frame looked like |
|---|---|---|
| A cross-queue hazard emitted as a plain pipeline barrier with **no queue-family ownership transfer** | zero errors | correct pixels |
| An **alias barrier removed**, so a transient was used over another transient's live bytes | zero errors | correct pixels |

The control that proves the harness had teeth is the one that **did** fire: dropping the timeline
wait between two submissions produced `SYNC-HAZARD-WRITE-RACING-WRITE`. So the silence above is a
property of the layers, not of the harness.

Consequently:

- Queue ownership transfers and memory aliasing SHALL be **structurally guaranteed by the render
  graph's derivation**, and SHALL NOT be treated as checkable by a validation layer or by comparing
  a rendered frame against a reference.
- The tests covering them SHALL assert on the **contents of the derived plan** — the barriers,
  the release and acquire halves, the semaphores and the alias edges — rather than on whether a
  frame rendered or matched an image. A frame that renders correctly is not evidence about either
  property.
- A capability built on this graph SHALL NOT assume validation covers them. This is written here
  because the milestones that add virtual geometry, global illumination and further async-compute
  work build directly on this graph, and each would otherwise inherit an assumption the device has
  already been measured to violate.
- Where a control of this kind is added, its **positive control SHALL be kept with it**, so that a
  control which has silently stopped firing is distinguishable from a defect that is absent.

**Breadcrumb markers SHALL be written per pass** where the backend supports it, using the breadcrumb
mechanism in `diagnostics-profiling-and-crash`, so that they survive into a crash artefact when the
trace tail does not.

On **device loss or a graphics fault**, the engine SHALL contribute to the crash artefact: the render
graph as built for the frame, the last submitted and last completed passes, pipeline and shader
identities, resource identities and states, barrier history where available, queue state, the frame
identity, and any driver-reported reason. A driver message alone SHALL NOT be considered the
diagnosis.

GPU pass timings, queue occupancy, barriers, and transient memory SHALL be emitted into the shared
trace, so that GPU behaviour correlates with task, memory, and streaming activity on one timeline.

#### Scenario: GPU crash diagnosis
- **WHEN** the device is lost
- **THEN** the engine SHALL report the last breadcrumb reached, naming the render graph pass, and
  contribute the surrounding graph and pipeline state to the crash artefact

#### Scenario: Validation error fails loudly
- **WHEN** a validation layer reports an error in a development build
- **THEN** the engine SHALL log it with the pass name and, by configuration, break into the
  debugger

#### Scenario: GPU and CPU on one timeline
- **WHEN** a frame is investigated
- **THEN** GPU pass events and CPU task events SHALL appear on the same timeline

#### Scenario: A clean validation run is not evidence of correct ownership
- **WHEN** a cross-queue hazard is emitted as a plain barrier with no ownership transfer
- **THEN** validation SHALL be expected to report nothing and the frame to be correct, and the
  defect SHALL be caught instead by an assertion on the derived plan

#### Scenario: A clean validation run is not evidence of correct aliasing
- **WHEN** an alias barrier is removed and a transient is used over another transient's live bytes
- **THEN** validation SHALL be expected to report nothing, and the defect SHALL be caught instead by
  an assertion on the plan's alias edges and transient placements

#### Scenario: The synchronisation harness proves it fires
- **WHEN** the timeline wait between two dependent submissions is removed
- **THEN** synchronisation validation SHALL report a write-after-write hazard, which is the positive
  control that keeps a silent negative control from being read as a pass

### Requirement: Null backend
A **null** RHI backend SHALL implement the full interface without a GPU, satisfying resource
creation and command recording as no-ops while preserving handle semantics and validation.

#### Scenario: Headless CI
- **WHEN** rendering tests run in CI without a GPU
- **THEN** the null backend SHALL allow render graph construction, culling, and scheduling to be
  tested without device access
