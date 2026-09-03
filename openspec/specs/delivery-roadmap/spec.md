# delivery-roadmap Specification

## Purpose

Defines the **order in which the specified capabilities are implemented**, the maturity each reaches
at each step, and the executable criterion that closes each step.

Ordering is a design decision here, not a scheduling detail. Several commitments in these
specifications — computed barriers, stable field identity, one command stream into the simulation,
transactions as the only persistent write path — are properties of every line of code written after
them rather than features of a subsystem. Established at the right moment they cost almost nothing;
established late, everything built in the meantime has to be revisited. This capability pins each
such invariant to the milestone at which it must land.

It carries **no dates**. A date is a compound estimate that decays from the moment it is written,
and once written it becomes the thing people track. What does not decay is *after what* — and that
is the question that changes decisions.

The two failure modes it exists to prevent are the horizontal build, where a dozen subsystems are
each nearly finished and nothing runs, and drift, where the implementation quietly settles a
question the specifications had already decided. Vertical slices with executable exit criteria
address the first; a status record checked by a recipe against the specification set addresses the
second.

The narrative view of the ladder lives in `docs/ROADMAP.md`, with the capability matrix, dependency
graphs, and risk register beneath it. Those are views; this specification is authoritative.

## Requirements

### Requirement: The roadmap is a sequencing contract, not a schedule
The project SHALL maintain a **delivery roadmap** that states the order in which the specified
capabilities are implemented, the maturity each reaches at each step, and the criterion that closes
each step.

The roadmap SHALL be expressed in **milestones ordered by dependency and risk**, and SHALL NOT
carry calendar dates, durations, or team-size assumptions. Dates are estimates that decay; ordering
is a design consequence that does not.

Where two capabilities have no dependency between them, the roadmap SHALL NOT imply an order, and
they MAY be implemented concurrently or in either order.

#### Scenario: The roadmap is read for order, not for a date
- **WHEN** a contributor asks when a capability will be implemented
- **THEN** the roadmap SHALL answer with the milestone and the prerequisites that gate it, and
  SHALL NOT state a date

#### Scenario: Slippage does not invalidate the roadmap
- **WHEN** a milestone takes longer than anyone expected
- **THEN** no roadmap artefact SHALL become incorrect, because none of them asserted a duration

### Requirement: Milestones are vertical slices
Every milestone SHALL end in a **runnable artefact** that exercises the capabilities it claims,
end to end, through the same entry points a user would use.

A milestone SHALL NOT be defined as a set of finished subsystems with no assembly. Horizontal
milestones hide integration cost until the point where it is most expensive to pay.

Each milestone SHALL name its runnable artefact — a sample project, a headless run, a rendered
frame, a captured trace — and that artefact SHALL be committed to the repository and exercised by
continuous integration.

#### Scenario: A milestone closes on a demonstration
- **WHEN** a milestone is proposed as complete
- **THEN** its named artefact SHALL run from a single recipe, and the run SHALL be the evidence

#### Scenario: A horizontal milestone is rejected
- **WHEN** a proposed milestone consists only of subsystem work with no assembled result
- **THEN** it SHALL be flagged against this requirement and restructured around a slice

### Requirement: Capability maturity tiers
Each capability's progress SHALL be recorded against exactly one of four tiers:

| Tier | Meaning |
|---|---|
| **—** | Not started. No implementation exists. |
| **Seed** | The interfaces, the data model, and the invariants exist. Enough is implemented that dependent capabilities can be built against it. Behaviour may be minimal, single-threaded, unoptimised, or restricted to one backend. |
| **Working** | The requirements a real project depends on are satisfied. The capability is usable in the sample projects, is covered by tests, and its diagnostics exist. Optional and advanced requirements may be outstanding. |
| **Complete** | Every requirement in the capability's spec is satisfied, every scenario has a corresponding test or documented reason, and the capability's gates are in continuous integration. |

A capability SHALL reach **Seed** in the milestone at which its first dependent needs it, not in the
milestone at which it is interesting.

Most capabilities SHALL span several milestones. A roadmap entry that moves a large capability from
**—** to **Complete** in one milestone SHALL be treated as an unanalysed estimate.

#### Scenario: A dependent is unblocked by a seed
- **WHEN** the renderer needs a job system
- **THEN** the job system SHALL be at Seed before the renderer's work begins, and SHALL NOT be
  required to be Complete

#### Scenario: A tier claim is checked against the spec
- **WHEN** a capability is claimed Complete
- **THEN** each requirement in its spec SHALL map to a test, a gate, or a recorded exemption

### Requirement: The milestone ladder
The roadmap SHALL consist of the following milestones, in this order. Each names the capabilities
it advances and the artefact that closes it.

| # | Milestone | Theme | Closing artefact |
|---|---|---|---|
| **M0** | Ground | Toolchain, workflow, continuous integration, the empty application | `just doctor && just build && just test` green on three desktop platforms; a window opens and closes; a trace file is produced |
| **M1** | Substrate | `core-*`: types and reflection, memory, math, jobs, assets and the virtual filesystem | A headless host loads a package, runs a parallel job graph, and reports its memory budget tree; the identity manifest gate is live |
| **M2** | World | ECS, node façade, serialization and prefabs, the main loop, determinism seeds | A headless run loads a scene, ticks at a fixed rate, prints a hierarchical state hash, and reproduces it exactly on a second run |
| **M3** | First light | RHI and render graph on Vulkan, the shader system, the render server, clustered forward, standard material, lights and shadows, culling | A lit, textured, shadowed scene renders, guarded by a golden-image test; the null backend runs the same frame in continuous integration |
| **M4** | Playable | The C ABI, the Swift overlay, input, camera, physics, audio playback, the command stream | A character controller sample written entirely in Swift: move, jump, collide, hear it; the ABI gate is live |
| **M5** | Authorable | The Rust editor, documents and transactions, viewport and gizmos, asset import, live editing, projects and plugins | Open a project, import a glTF asset, edit a scene with gizmos, undo, save, press play; killing the runtime does not kill the editor |
| **M6** | Scale | The build graph and derived data cache, world partition and streaming, residency, virtual texturing, save and persistence | Walk continuously across a multi-kilometre streamed world, save, quit, reload, and arrive in the same state; a cooked package ships and a patch applies |
| **M7** | Fidelity | The material compiler, virtual geometry, virtual shadows, temporal rendering, post-processing, global illumination, denoising, ray tracing, the budget arbiter | A film-detail scene holds its frame budget while the arbiter reallocates under load; indirect light and reflections converge; degradation is coarser, never missing |
| **M8** | Game systems | Gameplay framework, abilities, visual scripting, sequencing, animation, AI, navigation, VFX, UI, text, 2D, full audio, inference | A playable vertical-slice game exercising every gameplay-facing capability at Working |
| **M9** | Integrity | Determinism profiles and the validator, replay, rollback, networking and replication, dedicated server | A four-player session with rollback reconciliation; a recorded replay reproduces bit-exactly; an injected divergence is narrowed to a field on an entity |
| **M10** | Worlds | Environment fields, terrain, foliage, water, weather and wind, atmosphere and clouds, procedural generation | An open-world environment demo: procedurally populated terrain, dynamic weather driving foliage, water and wetness through the shared field substrate |
| **M11** | Reach | Metal and D3D12 backends, the console porting surface, mobile, XR prerequisites, packaging and patching complete, the documentation gate | The same project ships on every supported target from one command; every capability is Complete or explicitly deferred |

**M11** SHALL be the **1.0** gate. Before it, no compatibility promise SHALL be made beyond the ABI
versioning rules that apply from **M4**.

Milestones **M0** through **M2** SHALL be treated as a single unbroken sequence: none of them
produces user-visible value on its own, and stopping between them leaves nothing usable.

#### Scenario: A capability is located on the ladder
- **WHEN** a contributor asks where navigation belongs
- **THEN** the roadmap SHALL place it at M8, gated on ECS (M2), world partition (M6), and the AI
  system's locomotion needs

#### Scenario: The ladder is followed, not skipped
- **WHEN** work is proposed on a capability whose milestone has not been reached
- **THEN** it SHALL be either a recorded spike under the risk requirement, or a re-sequencing
  change, and SHALL NOT be an ad-hoc exception

### Requirement: Milestone exit criteria are executable
A milestone's exit criteria SHALL be **executable checks**, not judgements. Each criterion SHALL be
one of: a recipe that exits zero, a test that passes, a benchmark within a stated threshold, or a
committed artefact that is regenerated and compared.

A criterion phrased as an assessment — "the renderer is solid", "the editor feels good" — SHALL be
replaced by the check that would settle it, or removed.

Every milestone SHALL be closable by one recipe that runs its full criteria set.

#### Scenario: A milestone is verified without argument
- **WHEN** a milestone's closing recipe is run on a clean checkout
- **THEN** it SHALL pass or fail, and the result SHALL be the decision

#### Scenario: A subjective criterion is rewritten
- **WHEN** a proposed criterion cannot be evaluated by a machine
- **THEN** it SHALL be restated as a check, or moved to the milestone's notes as non-binding

### Requirement: Milestone gates do not regress
Once a milestone's criteria are green, its checks SHALL join the permanent continuous integration
set and SHALL remain green for every subsequent change.

A later milestone SHALL NOT be permitted to break an earlier milestone's gate. Where a change
genuinely supersedes an earlier criterion — a sample replaced, a format changed — the supersession
SHALL be recorded in a change, and the replacement check SHALL land in the same change.

#### Scenario: A later milestone breaks an earlier sample
- **WHEN** work in M7 breaks the M4 character controller sample
- **THEN** the merge SHALL be blocked until the sample runs again or its replacement lands

#### Scenario: A gate is retired deliberately
- **WHEN** a milestone criterion is superseded
- **THEN** an OpenSpec change SHALL record why, and the new check SHALL be in the same change

### Requirement: Retrofit-hostile invariants land at seed tier
Certain commitments in the specifications cannot be added later without invalidating everything
built on top of them. Each SHALL be implemented at the **Seed** tier of its capability, in the
milestone stated, even though the surrounding capability is far from Complete:

| Invariant | Owning capability | Lands at | Why it cannot wait |
|---|---|---|---|
| Stable type and field identity, the committed manifest, and its gate | `core-type-system` | M1 | Every serialized artefact produced after this point encodes identity; retrofitting invalidates all of them |
| Architectural layering enforcement over the project graph | `project-and-plugins` | M1 | A layering violation is cheap to prevent and unbounded to unwind |
| Access-declaration-driven scheduling and structural-change deferral | `core-jobs-and-concurrency`, `ecs-core` | M1–M2 | Systems written without declarations cannot be parallelised afterwards without rewriting them |
| The fixed-tick commit boundary, seeded random streams, and state-hash hooks | `simulation-and-determinism` | M2 | Determinism is a property of every line of simulation code; the validator in M9 can only find violations, not prevent them |
| Cook-time flattening of hierarchy into archetype-native blocks | `serialization-and-prefabs` | M2 | The runtime data layout is the reason for the ECS; a runtime prefab graph would become load-bearing |
| Barriers, aliasing, and scheduling computed by the render graph | `rhi-and-render-graph` | M3 | Hand-written barriers spread across every pass; removing them later is a renderer rewrite |
| Camera-relative rendering and the coordinate, depth, and unit conventions | `core-math`, `rendering-architecture` | M3 | Precision assumptions propagate into every shader and every transform path |
| Versioned, append-only ABI with the compatibility gate | `native-abi` | M4 | The first published symbol starts the compatibility obligation |
| One validated command stream as the only input to the simulation | `gameplay-framework` | M4 | Replay, rollback, and lockstep in M9 are this mechanism read differently; a second input path defeats all three |
| Transactions as the only path for persistent mutation | `editor-documents-and-transactions` | M5 | Undo, autosave, recovery, merge, and live editing are all consequences; a direct write path silently breaks each |
| Residency, activation, simulation rate, and detail as four separate decisions | `residency`, `world-partition-and-streaming` | M6 | Collapsed once, they are collapsed in every consumer that follows |
| The determinism firewall around presentation-only subsystems | `vfx-system`, `ml-inference`, `weather-and-wind` | At each subsystem's Seed | A single gameplay read from a non-deterministic subsystem is undetectable until a desync months later |
| Graphs compile to programs and are never interpreted | `visual-scripting` and every graph consumer | At each consumer's Seed | An interpreter becomes the compatibility surface, and per-entity virtual dispatch is what the ECS exists to avoid |
| Privacy classification on every diagnostic field | `diagnostics-profiling-and-crash` | M0 | Unclassified fields accumulate faster than they can be audited |

A change that implements a capability at Seed SHALL demonstrate the invariants in this table that
apply to it, or record why they do not.

#### Scenario: A seed is reviewed for its invariants
- **WHEN** the ECS reaches Seed without structural-change deferral
- **THEN** the change SHALL be flagged against this requirement, because every system written
  afterwards would assume immediate structural mutation

#### Scenario: An invariant is not deferred for convenience
- **WHEN** a proposal defers the identity manifest to a later milestone to move faster
- **THEN** it SHALL be rejected, and the cost of the deferral SHALL be stated in the rejection

### Requirement: Dependency order is derived from the specifications
A capability SHALL NOT be scheduled to reach **Working** before every capability it depends on has
reached **Seed**, and SHALL NOT reach **Complete** before its dependencies have reached **Working**.

Dependencies SHALL be taken from the specifications themselves — the interfaces a capability names,
the contracts it consumes — and not asserted independently in the roadmap. Where the roadmap and a
specification disagree about a dependency, the specification SHALL be authoritative and the roadmap
SHALL be corrected.

A dependency cycle between capabilities SHALL be resolved by splitting one of them into a Seed that
carries only the contract, and a later tier that carries the implementation.

#### Scenario: A cycle is broken by a contract seed
- **WHEN** world partition needs terrain payloads and terrain needs cell streaming
- **THEN** the cell payload contract SHALL land at world partition's Seed in M6, and terrain SHALL
  implement it as a producer in M10

#### Scenario: The roadmap defers to the spec
- **WHEN** the roadmap places a capability before a prerequisite its spec names
- **THEN** the roadmap SHALL be corrected rather than the specification relaxed

### Requirement: The backend and platform ladder
Graphics backends SHALL be delivered in this order, and the roadmap SHALL NOT reorder them without
a change:

| Order | Backend | Lands at | Role |
|---|---|---|---|
| 0 | Null | M3, with the first backend | Continuous integration without a GPU; the reference for what the RHI requires |
| 1 | Vulkan | M3 | The primary backend; the RHI's shape is Vulkan-shaped by decision |
| 2 | Metal | M11, seeded at M7 | Native Metal, not a translation layer |
| 3 | D3D12 | M11 | Last, because it adds no capability the first two do not exercise |

Target platforms SHALL be delivered in this order: the three desktop platforms together from M0;
the console **porting surface** — the platform abstraction proving it has no desktop assumptions —
from M11; mobile from M11; XR deferred with its prerequisites held open from M3.

A second backend SHALL be started only after the first has passed a milestone gate, so that the
abstraction is validated against a working implementation rather than against a guess.

#### Scenario: The null backend keeps continuous integration honest
- **WHEN** a render feature is added
- **THEN** it SHALL run on the null backend in continuous integration, or state why it cannot

#### Scenario: Metal is seeded before it is required
- **WHEN** M7 introduces features that could accidentally become Vulkan-specific
- **THEN** a Metal seed SHALL exist to expose the assumption while it is still cheap to remove

### Requirement: Third-party integration is staged
Each integrated dependency SHALL enter the project at the milestone that first needs it, behind its
engine-owned interface, with a stub or null implementation of that interface retained:

| Dependency | Enters at | Behind |
|---|---|---|
| Slang | M3 | The shader system |
| Jolt | M4 | `PhysicsServer` |
| miniaudio | M4 | The audio driver layer |
| glTF, meshoptimizer | M5 | The importer framework |
| HarfBuzz, ICU, FreeType | M5 | `TextServer` |
| ufbx, xatlas | M6 | The importer framework and lightmap packing |
| Recast | M8 | The navigation interface |
| Steam Audio | M8 | The spatial acoustics backend |
| Optional middleware | Deferred | The same interfaces, as alternative backends |

No dependency SHALL be introduced before its engine-owned interface exists. Integrating first and
abstracting later produces an interface shaped by the library rather than by the engine.

#### Scenario: An interface precedes its library
- **WHEN** physics work begins in M4
- **THEN** `PhysicsServer` SHALL be defined and exercised by a trivial implementation before Jolt
  is linked

#### Scenario: Replaceability is demonstrated, not asserted
- **WHEN** a dependency is claimed replaceable
- **THEN** the retained stub SHALL build and run, proving the interface does not leak the library

### Requirement: Deferred scope has re-entry points
Scope that the specifications defer SHALL be recorded in the roadmap with the milestone at which it
would be reconsidered, the seams that must remain open until then, and the check that proves those
seams are still open.

Deferred scope SHALL NOT be silently dropped, and SHALL NOT be silently started. Bringing deferred
scope forward SHALL be an OpenSpec change.

At minimum the following SHALL be recorded as deferred with re-entry points: XR, fluid simulation,
hair and fibre rendering, offline simulation import, voxel rendering, authority migration,
distributed build execution, cloud save backends, and optional audio middleware.

#### Scenario: A deferred seam is protected
- **WHEN** a render change would break multi-view rendering
- **THEN** the XR prerequisite check SHALL fail, and the change SHALL either preserve the seam or
  propose closing it explicitly

#### Scenario: Deferred work is not accidental scope
- **WHEN** a contributor begins implementing a deferred capability
- **THEN** a change SHALL first move it onto the ladder, with its dependencies analysed

### Requirement: Risk is scheduled, not discovered
Each milestone SHALL name the work within it that carries the most uncertainty, and that work SHALL
be attempted **first** within the milestone, as a time-boxed spike whose only deliverable is a
decision.

A spike SHALL be permitted to violate the ladder — to build a throwaway prototype of a later
capability — provided the prototype is not merged into the engine and the decision it produces is
recorded.

Where a spike's outcome would change the roadmap, the roadmap change SHALL be proposed before the
rest of the milestone proceeds.

#### Scenario: The hard part goes first
- **WHEN** M7 begins
- **THEN** the material compiler's intermediate representation and the budget arbiter's control
  loop SHALL be spiked before the surrounding work is scheduled

#### Scenario: A spike changes the plan
- **WHEN** a spike shows a milestone's approach is unworkable
- **THEN** a roadmap change SHALL land before the milestone continues, rather than the milestone
  quietly absorbing the discovery

### Requirement: Implementation status is recorded in one place
The project SHALL maintain exactly one authoritative record of per-capability implementation
status: the capability, its tier, the milestone that last advanced it, and the change that did so.

A change that implements or advances a capability SHALL update that record in the same change. A
change SHALL NOT claim a tier the record does not support.

Status SHALL be reported by a recipe rather than read by hand, and the recipe SHALL fail when the
record references a capability that does not exist, or omits a capability that does.

#### Scenario: Status cannot drift from the specifications
- **WHEN** a capability is added or renamed
- **THEN** the status recipe SHALL fail until the record is updated

#### Scenario: A tier claim is traceable
- **WHEN** a capability is listed as Working
- **THEN** the record SHALL name the change that advanced it

### Requirement: The roadmap changes through OpenSpec
Re-sequencing the ladder, moving a capability between milestones, changing a milestone's exit
criteria, and bringing deferred scope forward SHALL each be an OpenSpec change against this
capability, and SHALL state what was learned that made the previous ordering wrong.

The narrative roadmap in the repository's documentation SHALL be a **view** of this specification.
Where the two disagree, this specification SHALL be authoritative, and the documentation SHALL be
corrected in the same change.

#### Scenario: A reorder carries its reason
- **WHEN** a capability moves from M8 to M6
- **THEN** the change SHALL state the dependency or risk that was discovered, not merely the new
  position

#### Scenario: Documentation is kept in step
- **WHEN** the ladder changes
- **THEN** the documentation view SHALL be regenerated or edited in the same change

### Requirement: Forbidden roadmap patterns
The following SHALL NOT appear, and each SHALL be checkable:

- A milestone with a calendar date, a duration, or an assumed team size
- A milestone that finishes subsystems without assembling them into a runnable artefact
- An exit criterion that cannot be evaluated by running something
- A capability scheduled to reach Working before a prerequisite reaches Seed
- A retrofit-hostile invariant deferred past the milestone named for it
- A third-party library introduced before its engine-owned interface exists
- A second graphics backend started before the first passes a milestone gate
- Deferred scope removed from the roadmap without a change, or implemented without one
- A status claim in a document that the status record does not support
- A milestone gate disabled rather than fixed or explicitly superseded

#### Scenario: A proposal is checked
- **WHEN** a change proposes implementing a capability whose prerequisites are not seeded
- **THEN** it SHALL be flagged against this requirement, and either the prerequisite SHALL be
  seeded first or the roadmap SHALL be re-sequenced deliberately
