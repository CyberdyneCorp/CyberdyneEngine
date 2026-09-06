## MODIFIED Requirements

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
