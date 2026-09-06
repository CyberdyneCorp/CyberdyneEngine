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
| **M5.5** | Operable | The editor opened: a window, docked panels, a viewport showing the engine's own image, gizmo manipulation by hand — and the agent interface at Working, so an agent can compose a scene, write a gameplay script, reload it and look at the result | A person selects an object and drags a gizmo; an agent composes a scene from an empty project, writes and reloads a script, and captures the viewport to confirm it |
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

**M5.5 is an insertion, and it is recorded as one.** M5's row claimed `editor-ui-ux` at Working
while closing on a scripted session, which cannot exercise docking, workspaces, the command palette
or keyboard-first operation through the entry points a user would use — the very property this
capability's vertical-slice requirement demands. The implementation satisfied the row exactly; the
row was wrong. Inserting rather than renumbering keeps every existing reference to M6 through M11
valid, and the fractional number is deliberate: it says a milestone was added rather than pretending
the ladder was always twelve entries.

#### Scenario: A milestone claiming an interactive capability closes on an interactive artefact
- **WHEN** a milestone advances a capability about what a user sees or operates
- **THEN** its closing artefact SHALL exercise that capability through the entry points a user
  would use, and a scripted harness driving the model beneath it SHALL NOT satisfy the criterion

#### Scenario: A capability is located on the ladder
- **WHEN** a contributor asks where navigation belongs
- **THEN** the roadmap SHALL place it at M8, gated on ECS (M2), world partition (M6), and the AI
  system's locomotion needs

#### Scenario: The ladder is followed, not skipped
- **WHEN** work is proposed on a capability whose milestone has not been reached
- **THEN** it SHALL be either a recorded spike under the risk requirement, or a re-sequencing
  change, and SHALL NOT be an ad-hoc exception
