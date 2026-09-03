## MODIFIED Requirements

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
| **M11** | Reach | Metal and D3D12 backends, a native platform backend proving the porting surface, mobile targets, XR prerequisites, packaging and patching complete, the documentation gate | The same project ships on every supported target from one command; every capability is Complete or explicitly deferred |

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

### Requirement: The backend and platform ladder
Graphics backends SHALL be delivered in this order, and the roadmap SHALL NOT reorder them without
a change:

| Order | Backend | Lands at | Role |
|---|---|---|---|
| 0 | Null | M3, with the first backend | Continuous integration without a GPU; the reference for what the RHI requires |
| 1 | Vulkan | M3 | The primary backend; the RHI's shape is Vulkan-shaped by decision |
| 2 | Metal | M11, seeded at M7 | Native Metal, not a translation layer |
| 3 | D3D12 | M11 | Last, because it adds no capability the first two do not exercise |

Target platforms SHALL be delivered in this order: **Linux, Windows and macOS** — each on x86-64 and
ARM64 — together from M0; the **porting surface** proven against a second platform-layer
implementation at M11; **iOS, Android, visionOS and Web** as planned targets whose requirements the
platform abstraction must not preclude. Consoles are out of scope, per
`core-platform-abstraction`.

The desktop platform layer SHALL be delivered as an SDL3-backed implementation of `Platform`,
`DisplayServer` and the input backend at M0. At least one **native** per-platform implementation of
those interfaces SHALL land at M11, for the same reason Metal is seeded at M7: an abstraction with
one implementation is a guess, and the cost of discovering that at 1.0 is a porting surface that
does not port.

A second backend SHALL be started only after the first has passed a milestone gate, so that the
abstraction is validated against a working implementation rather than against a guess.

#### Scenario: The null backend keeps continuous integration honest
- **WHEN** a render feature is added
- **THEN** it SHALL run on the null backend in continuous integration, or state why it cannot

#### Scenario: Metal is seeded before it is required
- **WHEN** M7 introduces features that could accidentally become Vulkan-specific
- **THEN** a Metal seed SHALL exist to expose the assumption while it is still cheap to remove

#### Scenario: The platform abstraction is validated by a second implementation
- **WHEN** a native platform backend is written at M11
- **THEN** it SHALL require no change in `src/core/`, `src/ecs/`, `src/servers/`, or `src/scene/`,
  and any change it does require SHALL be recorded as a defect in the abstraction
