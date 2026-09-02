## ADDED Requirements

### Requirement: Renderer profiles
The engine SHALL define named **renderer profiles** — at minimum `Mobile`, `Standard`, `HighEnd`,
and `Cinematic` — each a configuration over one renderer, selecting: the pipeline, the enabled
feature set, quality tiers, budget allocations, and capability requirements.

A profile SHALL NOT be a separate renderer implementation. Where a difference cannot be expressed
as configuration, it belongs in the pipeline, which is already a replaceable component.

Content SHALL render under every profile it targets, differing in fidelity and performance, not in
whether it appears. A profile requiring content changes SHALL be treated as a defect in the
profile.

Projects SHALL be able to define additional profiles.

#### Scenario: One scene, four profiles
- **WHEN** a scene is rendered under each shipped profile
- **THEN** it SHALL render in all four, with documented differences in fidelity and cost

#### Scenario: Profile requires an unavailable capability
- **WHEN** a profile requires a capability the device lacks
- **THEN** the engine SHALL fall back to a profile the device supports, with a diagnostic naming
  the missing capability

#### Scenario: Profile is configuration, not code
- **WHEN** a project defines a custom profile
- **THEN** it SHALL do so by configuration, without engine modification

### Requirement: Renderer budget arbiter
The renderer SHALL contain exactly one **budget arbiter**, which measures frame cost and
distributes a frame budget as **allocations** to subsystems: geometry, shadows, global
illumination, reflections, material evaluation, VFX, post-processing, and resolution scale.

Subsystem controllers SHALL hold their own allocation using their own levers. They SHALL report
their measured cost to the arbiter, and SHALL NOT independently measure total frame time or infer
global load.

The arbiter SHALL adjust allocations on a **longer time constant** than the subsystem controllers
adjust within them, so a subsystem is never tracking a moving target.

Each subsystem SHALL declare a **reserved minimum**. A subsystem at its minimum SHALL report so,
and the arbiter SHALL reallocate from subsystems with headroom rather than continuing to reduce
one that has none.

A **pinned mode** SHALL disable the arbiter and every subsystem controller together, for
cinematics, capture, benchmarking, and deterministic tests. Partial pinning SHALL NOT be
possible.

The arbiter SHALL report, per frame: each allocation, each subsystem's measured cost, which
subsystems are at their minimum, and every adjustment made with its cause.

#### Scenario: Controllers do not fight
- **WHEN** heavy geometry pushes the frame over budget
- **THEN** the arbiter SHALL reduce the geometry allocation, and the VFX and post-processing
  controllers SHALL NOT independently reduce quality for a cost they did not incur

#### Scenario: Resolution and quality do not oscillate
- **WHEN** dynamic resolution reduces internal resolution, lowering every subsystem's measured
  cost
- **THEN** allocations SHALL be re-evaluated by the arbiter alone, on its own time constant,
  rather than every controller reacting to the apparent headroom

#### Scenario: A subsystem with nothing left to give
- **WHEN** virtual geometry has reached its minimum quality and the frame is still over budget
- **THEN** it SHALL report that it is at its minimum and the arbiter SHALL take the shortfall from
  a subsystem with headroom

#### Scenario: Pinned mode is total
- **WHEN** pinned mode is enabled for a capture
- **THEN** the arbiter and every subsystem controller SHALL stop adapting, and budget overruns
  SHALL be reported rather than corrected

#### Scenario: Adjustments are attributable
- **WHEN** quality drops during play
- **THEN** the report SHALL state which allocation changed, by how much, and what measurement
  caused it

### Requirement: Render features
Renderer functionality beyond the pipeline's core SHALL be organised as **render features**:
modules that declare their dependencies and contribute passes to the render graph for a view.

A render feature SHALL declare: the resources it requires, the resources it produces, its
extension point, its capability requirements, and its budget category.

Engine features — ambient occlusion, screen-space reflections, global illumination, volumetrics,
decals, debug visualisation — SHALL be implemented as render features using the same interface
available to projects and plugins.

Features SHALL be enableable per profile and per view, and a disabled feature SHALL contribute no
passes and allocate no resources.

#### Scenario: The engine uses its own extension interface
- **WHEN** an engine feature is implemented
- **THEN** it SHALL use the public render feature interface, so limitations are found internally

#### Scenario: Disabled feature costs nothing
- **WHEN** a feature is disabled for a view
- **THEN** it SHALL contribute no graph passes and its transient resources SHALL not be allocated

#### Scenario: Dependency is satisfied or the feature is skipped
- **WHEN** a feature requires motion vectors and no producer exists
- **THEN** the pipeline SHALL either produce them or skip the feature with a diagnostic, never
  execute it against missing data

### Requirement: Render pipeline configuration
A project's rendering configuration SHALL be an **asset**, not scattered settings: it SHALL name
the profile, the pipeline, the enabled render features and their quality levels, budget
allocations, and platform overrides.

The configuration SHALL be versioned and diffable, and changing it SHALL NOT require code.

The engine SHALL validate a configuration and report combinations that are unsupported, mutually
exclusive, or exceed the declared frame budget in the sum of their declared costs.

#### Scenario: Configuration is reviewable
- **WHEN** a rendering change is proposed
- **THEN** it SHALL appear as a diff of the configuration asset

#### Scenario: Over-subscribed budget is caught early
- **WHEN** the sum of enabled features' declared costs exceeds the frame budget
- **THEN** validation SHALL report it at configuration time, not at runtime

#### Scenario: Platform override
- **WHEN** a platform needs a reduced feature set
- **THEN** it SHALL be expressed as an override in the same asset

## MODIFIED Requirements

### Requirement: Scene, view, and instance model
The renderer SHALL model:

- **Scene** — a renderable world: a spatial index of instances, a light set, GI state, and an
  environment. Multiple scenes MAY exist simultaneously.
- **View** — a camera into a scene: transform, projection, viewport rect, render target,
  layer mask, quality settings, and post-process configuration. A view MAY have multiple
  **sub-views** for stereo or cubemap rendering.
- **Instance** — a placement of a renderable (mesh, VFX effect, decal, light, probe) into a
  scene, with a transform, bounds, layer mask, LOD parameters, visibility, and per-instance data.

Views SHALL be **first class and plural**. The main camera is one view among many: shadow views,
reflection probe captures, scene captures, editor viewports, minimaps, thumbnails, and each XR eye
are all views, rendered through the same path.

Views SHALL be organisable into **view families** that share work. A family SHALL declare what its
members share — the prepared GPU scene, culling results, shadow maps, acceleration structures,
history resources — so shared work is performed once rather than per view.

Each view SHALL carry its own budget allocation, so a secondary view cannot consume the frame
budget of the primary one.

#### Scenario: Multiple views of one scene
- **WHEN** a main camera, a shadow-casting light, and a reflection probe all render the same
  scene
- **THEN** each SHALL cull and render independently against the shared instance set

#### Scenario: Split screen
- **WHEN** two views target different rects of one render target
- **THEN** both SHALL be rendered in the same frame with independent culling

#### Scenario: Family shares prepared work
- **WHEN** two stereo eye views belong to one family
- **THEN** the GPU scene, shadow maps, and acceleration structures SHALL be prepared once and
  consumed by both

#### Scenario: Secondary view is budgeted
- **WHEN** a scene capture renders alongside the main view
- **THEN** it SHALL draw from its own allocation, and SHALL degrade before the main view does
