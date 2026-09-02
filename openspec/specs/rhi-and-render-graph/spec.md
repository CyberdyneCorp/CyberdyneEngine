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

Uploads SHALL go through a ring staging buffer; devices with host-visible device-local memory
(unified memory, resizable BAR) SHALL be able to write directly.

#### Scenario: Budget exceeded
- **WHEN** GPU memory allocation approaches the device budget
- **THEN** the engine SHALL report it, trigger streaming eviction, and fail the allocation
  gracefully rather than crashing

#### Scenario: Unified memory
- **WHEN** the device exposes host-visible device-local memory
- **THEN** per-frame instance data SHALL be written directly, skipping the staging copy

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

The graph SHALL be able to dump its structure — passes, resources, lifetimes, barriers, aliasing
decisions — as text or a Graphviz diagram.

Where the backend supports it, breadcrumb markers SHALL be written per pass so a device-lost
error identifies the last executing pass.

#### Scenario: GPU crash diagnosis
- **WHEN** the device is lost
- **THEN** the engine SHALL report the last breadcrumb reached, naming the render graph pass

#### Scenario: Validation error fails loudly
- **WHEN** a validation layer reports an error in a development build
- **THEN** the engine SHALL log it with the pass name and, by configuration, break into the
  debugger

### Requirement: Null backend
A **null** RHI backend SHALL implement the full interface without a GPU, satisfying resource
creation and command recording as no-ops while preserving handle semantics and validation.

#### Scenario: Headless CI
- **WHEN** rendering tests run in CI without a GPU
- **THEN** the null backend SHALL allow render graph construction, culling, and scheduling to be
  tested without device access
