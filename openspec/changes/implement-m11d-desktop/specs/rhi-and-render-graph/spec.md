## ADDED Requirements

### Requirement: The interface is backend-portable vocabulary
The RHI is an **engine-owned** interface, and a member whose shape only one graphics API can answer
is that API spelled into the engine. The Metal seed measured eight such places
(`src/backends/rhi-metal/`, `metal_gaps()`), three where Metal has no equivalent at all and two with
no workaround, and it exists precisely so they are found while they are cheap.

The interface SHALL therefore express, in engine terms rather than in one backend's terms:

- **Transient memory pooling** — a backend-opaque **memory pool class** that the render graph only
  compares for equality, rather than a bitmask of memory types. The property the graph must preserve
  is that one pool is provably legal for every transient in a frame; the Vulkan spelling of that
  property is not the property.
- **Resource state** — barriers SHALL be expressed in the access masks the engine already carries,
  and any image layout SHALL be derived **inside** the backend that needs one.
- **Queue ownership** — a capability answering whether the backend requires ownership transfer at
  all, rather than a queue-family index and an "ignored" sentinel at every call site.
- **Pipeline caches** — an opaque, backend-defined token or path, rather than a memory blob.
- **Shader modules** — a native form beside SPIR-V, with the backend declaring which form it accepts.
- **Formats** — a per-format support query, so the **engine** chooses a substitute for an unsupported
  format rather than each backend inventing one silently.

Every one of these SHALL be selected through the capability model. The renderer SHALL branch on
capabilities and never on backend identity, and that rule SHALL hold for these members as for any
other.

An interface change of this kind SHALL land on **every backend already in the tree** before a new
backend is written against it, because the cost of the change is the number of call sites it moves
and that number only grows.

#### Scenario: One transient pool is still proved legal for every transient
- **WHEN** the render graph plans a frame's transient resources
- **THEN** it SHALL establish that a single pool serves all of them by comparing opaque pool classes
  for equality, and a backend that cannot enumerate memory types SHALL still be able to answer

#### Scenario: An unsupported format is substituted by the engine
- **WHEN** a requested format is unsupported on the active backend
- **THEN** the per-format query SHALL report it, the engine SHALL select the declared substitute, and
  the choice SHALL be reported — rather than the backend silently choosing one

#### Scenario: No member requires a backend identity test
- **WHEN** the renderer needs behaviour that differs between backends
- **THEN** it SHALL query a capability, and a test on backend identity above the backend directories
  SHALL be a defect

### Requirement: Secondary command buffers are recorded within their pass
Parallel recording SHALL state, as a **precondition of the interface**, that a render pass instance
is begun before any secondary command buffer belonging to it is recorded.

The interface today permits a secondary to be recorded before its pass instance exists, which no
Metal equivalent can express: `MTLParallelRenderCommandEncoder` creates sub-encoders only inside a
live encoder. This is one of the two gaps with no workaround, so it is a contract change rather than
a backend concern.

The precondition SHALL be **enforced where every test can see it**, not documented only: a backend
SHALL refuse a secondary recorded outside its pass instance with a diagnostic naming the pass and the
buffer.

#### Scenario: A secondary recorded before its pass is refused
- **WHEN** a secondary command buffer is recorded before its render pass instance is begun
- **THEN** the call SHALL fail with a diagnostic naming the pass and the command buffer, rather than
  succeeding on one backend and being unrepresentable on another

#### Scenario: Parallel recording still fans out
- **WHEN** a pass records its draws across several threads
- **THEN** the pass SHALL be begun first and each thread's secondary SHALL be recorded within it,
  with the same parallelism the interface already provides

### Requirement: A delivered backend is one that draws
A backend SHALL NOT be recorded as delivered because it compiles. The claim is that it produces the
engine's frame, and the evidence is the golden-image set rendered through it and compared against the
committed references within tolerance.

Where the evidence cannot be gathered — no runner with a device for that backend's platform — the
claim SHALL be reported as **not evaluated**, with the reason, through the milestone ledger's own
mechanism. A criterion that passes on a machine unable to judge it is the defect that mechanism
exists to prevent, and *not evaluated* SHALL NOT be read as a pass.

Where a device is present, the result SHALL **name the device class** — hardware, virtualised, or a
software adapter such as WARP. A software adapter is a legitimate D3D12 device and is not a GPU; a
result that does not say which answered is not evidence about hardware.

Backend comparison SHALL be against **one** reference set rather than a per-backend one. A reference
committed per backend makes each backend its own truth and no divergence can ever be detected.

#### Scenario: A backend compiles and draws nothing
- **WHEN** a backend builds and registers but has never produced a compared frame
- **THEN** it SHALL NOT be recorded as delivered, and the milestone record SHALL state what is
  missing

#### Scenario: No runner can present a device
- **WHEN** no available machine can create a device for a backend's platform
- **THEN** the image criterion SHALL report not evaluated with its reason, and the backend's claim
  SHALL be limited to what compiling and validation demonstrate

#### Scenario: A software adapter answers
- **WHEN** the device that produced a frame is a software or virtualised adapter
- **THEN** the result SHALL name it as such, and a parity claim against a reference photographed on
  hardware SHALL be reported as a measured difference rather than as a pass

## MODIFIED Requirements

### Requirement: Memory management
The RHI SHALL manage GPU memory through a suballocating allocator with pools per memory type,
dedicated allocations for large resources, defragmentation for transient pools, and budget
tracking against device-reported limits.

Transient pooling SHALL be requested through a **backend-opaque memory pool class** rather than a
bitmask of memory types, so that the render graph's proof that one pool serves every transient in a
frame is expressible on a backend that does not enumerate memory types.

GPU memory SHALL be reported into the engine's **memory domain and budget tree** (see
`core-memory-and-containers`) as the `GPU` domain with sub-domains for persistent, streaming,
upload and readback, and transient graph memory — so that GPU and CPU memory are visible in one
model rather than two unrelated reports. **The backend that allocates device memory is the module
that reports it**: a budgeted domain nothing writes into is a budget nobody can exceed and a report
that is silently empty.

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

#### Scenario: The GPU domain has a producer
- **WHEN** a backend allocates device memory for a resource or for the graph's transient pool
- **THEN** the allocation SHALL be attributed to the `GPU` domain's sub-domain, so a report of that
  domain reflects what the device actually holds

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

**Parity is the claim a second or third backend makes**, and it SHALL mean: the same render graph,
the same capability queries, the same golden scenes compared against the same references, and no
member of the interface that exists only for one backend's sake. A backend that requires the engine
above `src/backends/` to know its identity has not reached parity, whatever it can draw.

Each backend SHALL be independently configurable, and the engine SHALL build and pass its suites with
each backend option **off** as well as on.

#### Scenario: Feature requires a newer version
- **WHEN** a capability requires an extension beyond the baseline
- **THEN** it SHALL be optional and capability-gated, with the baseline path still correct

#### Scenario: A backend that is not built is refused by name
- **WHEN** a backend is requested in a build where its option is off
- **THEN** registration SHALL fail naming the option, rather than registering a factory that fails at
  the first call

#### Scenario: Parity is measured, not asserted
- **WHEN** a second backend is claimed at parity with the primary
- **THEN** the golden scenes SHALL be rendered through it and compared against the same references,
  and any divergence SHALL identify both backends
