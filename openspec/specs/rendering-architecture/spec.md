# rendering-architecture Specification

## Purpose

Defines the rendering server layer: the handle-based `RenderServer` API, how ECS data becomes a
render frame, the view/scene/instance model, the snapshot boundary between simulation and
rendering, and the frame structure that the concrete pipelines fill in.

## Requirements

### Requirement: Handle-based render server
`RenderServer` SHALL own all renderable state and expose it through generational handles, with
no knowledge of entities, nodes, or scripts.

Object families: textures, samplers, meshes, materials, shaders, skeletons, VFX effect instances,
lights, reflection probes, decals, GI volumes, lightmaps, occluders, cameras, views, scenes,
instances, canvases, environments, and post-process settings.

#### Scenario: Renderer is testable in isolation
- **WHEN** a test drives `RenderServer` directly with handles
- **THEN** it SHALL produce a frame without an ECS world or scene tree existing

#### Scenario: Dependency invalidation
- **WHEN** a material's shader is destroyed
- **THEN** dependent cached pipelines and descriptor sets SHALL be invalidated through a
  dependency-tracking mechanism, not left dangling

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

### Requirement: Simulation-to-render snapshot
Rendering SHALL consume an immutable **render snapshot** published at a defined point each frame,
not live ECS storage.

The snapshot SHALL contain: visible instance data (transforms, bounds, material and mesh handles,
per-instance parameters), light state, camera state, and environment state — extracted by an
`Extract` system running at the end of the frame stage.

Extraction SHALL be incremental: only instances whose relevant components changed since the last
extraction SHALL be re-extracted, using ECS change detection.

#### Scenario: Render reads a consistent world
- **WHEN** the render thread builds a frame
- **THEN** it SHALL see a coherent snapshot even while simulation advances concurrently

#### Scenario: Incremental extraction
- **WHEN** 100 000 static instances exist and 50 move
- **THEN** only the 50 changed instances SHALL be re-extracted

#### Scenario: Interpolated transforms
- **WHEN** rendering falls between simulation ticks
- **THEN** extraction SHALL write interpolated transforms using the frame's interpolation alpha

### Requirement: Frame structure
A rendered frame SHALL be composed as:

1. **Extract** — publish the render snapshot
2. **Prepare** — update GPU resources from the snapshot: instance buffers, light buffers,
   material data, skinning, particle simulation
3. **Cull** — per view, produce visible instance lists and shadow caster lists
4. **Build graph** — declare passes, resources, and dependencies for every view
5. **Compile graph** — cull, schedule, alias, synchronise
6. **Execute** — record and submit, potentially in parallel
7. **Present** — swap chain presentation and end-of-frame callbacks

#### Scenario: Views share prepared data
- **WHEN** several views render the same scene in one frame
- **THEN** instance and material buffers SHALL be prepared once and shared across views

### Requirement: Pipelines are pluggable
The concrete rendering pipeline SHALL be a replaceable component implementing a
`RenderPipeline` interface that receives the snapshot and view list and populates the render
graph.

The engine SHALL ship: **Forward+** (clustered, the default desktop pipeline), **Visibility
Buffer** (deferred material evaluation, the pipeline in which virtual geometry realises its full
benefit — see `virtual-geometry`), **Mobile** (reduced feature set, tile-friendly), and **Null**.
Projects SHALL be able to supply their own.

Pipelines SHALL differ in their strengths, and the documentation SHALL state them rather than
presenting one as strictly better: Forward+ handles transparency, MSAA, and varied shading models
directly; the visibility buffer handles very high geometric density and many materials, at the cost
of a more constrained transparency and MSAA story.

Content SHALL work under any shipped pipeline; a pipeline choice SHALL affect performance
characteristics and available features, not whether a scene renders.

#### Scenario: Pipeline selection
- **WHEN** the configuration selects a pipeline and the device supports it
- **THEN** it SHALL be used; otherwise the engine SHALL fall back with a diagnostic

#### Scenario: Custom pipeline
- **WHEN** a project registers a custom pipeline
- **THEN** it SHALL receive the same snapshot and graph builder as the built-in ones, with no
  engine modification

#### Scenario: Content is portable across pipelines
- **WHEN** a scene authored under Forward+ is rendered under the visibility buffer pipeline
- **THEN** it SHALL render correctly, with documented differences in transparency and
  anti-aliasing behaviour rather than missing content

### Requirement: Render passes are extensible
Projects and modules SHALL be able to insert custom passes at defined **extension points**:
before opaque, after opaque, after sky, before transparent, after transparent, before
post-process, after post-process, after UI.

A custom pass SHALL declare its resource usage so the graph can schedule and synchronise it, and
SHALL be able to request optional resources (depth, normals, motion vectors), which the pipeline
then guarantees to produce.

#### Scenario: Custom pass requests depth
- **WHEN** a custom post-process pass declares it needs the depth buffer
- **THEN** the pipeline SHALL ensure depth is produced and available, and the graph SHALL
  synchronise access

#### Scenario: Unused optional resource is not produced
- **WHEN** no pass requests motion vectors
- **THEN** the motion vector pass SHALL be culled and its target not allocated

### Requirement: Environment and post-process configuration
An **environment** SHALL describe: background (colour, sky, or captured), ambient lighting
source and intensity, fog (linear, exponential, height-based, volumetric), and the sky model.

A **post-process configuration** SHALL describe: exposure and auto-exposure, tonemapping,
bloom, depth of field, motion blur, colour grading, vignette, chromatic aberration, film grain,
and anti-aliasing selection.

Both SHALL be per view, with a scene-level default.

#### Scenario: Per-view override
- **WHEN** a security-camera view sets its own post-process configuration
- **THEN** it SHALL render with that configuration while the main view is unaffected

### Requirement: Render targets and formats
The renderer SHALL render HDR scene colour in a floating-point format (`RGBA16F` by default,
`R11G11B10F` where alpha is unneeded and precision permits), with tonemapping to the output
format at the end of the chain.

Output SHALL support SDR (sRGB) and HDR (Rec.2020 PQ / scRGB) presentation where the display and
platform support it.

#### Scenario: HDR output
- **WHEN** the display supports HDR and it is enabled
- **THEN** tonemapping SHALL target the display's luminance range rather than mapping to SDR
  white

#### Scenario: Precision fallback
- **WHEN** a device does not support the preferred HDR format
- **THEN** the renderer SHALL select the best supported alternative and record the choice in
  diagnostics

### Requirement: Debug visualisation
The renderer SHALL provide debug view modes selectable per view: albedo, normals, roughness,
metallic, ambient occlusion, world position, depth, overdraw, wireframe, shading complexity,
light complexity, cluster occupancy, LOD level, mip level, motion vectors, GI contribution,
shadow cascades, and bounding volumes.

A **debug draw** API SHALL allow any system or script to submit lines, spheres, boxes, capsules,
frustums, and text, batched and drawn after the main scene.

#### Scenario: Debug draw from a system
- **WHEN** a gameplay system submits a debug sphere during simulation
- **THEN** it SHALL be double buffered and drawn in the following frame with no allocation per
  primitive

#### Scenario: Debug modes cost nothing in shipping
- **WHEN** the engine is built for shipping
- **THEN** debug view modes and debug draw SHALL be compiled out

### Requirement: Render statistics
The renderer SHALL accumulate per view and per frame: visible instance count, draw call count,
triangle count, pass count, GPU time per pass (via timestamps), CPU time per stage, and memory
usage by resource category.

#### Scenario: Per-pass GPU timing
- **WHEN** timestamp queries are available
- **THEN** each render graph pass SHALL report its GPU duration, attributable by pass name

### Requirement: Deterministic submission order
Draw submission order SHALL be determined by explicit sort keys, not by ECS iteration order or
thread timing, so a frame is reproducible.

#### Scenario: Same scene, same commands
- **WHEN** the same snapshot is rendered twice
- **THEN** the recorded command stream SHALL be identical
### Requirement: GPU scene
The renderer SHALL maintain a **GPU scene**: the authoritative GPU-side representation of
renderable instances, from which GPU-driven culling, LOD selection, and indirect drawing are
performed.

The GPU scene SHALL hold per instance at minimum: a transform and its previous-frame value,
bounds, a mesh reference, a material reference, an LOD chain reference, a layer mask, and instance
flags.

Instances SHALL be publishable into the GPU scene from multiple producers:

- the **extract** stage, from ECS entities with renderable components
- **instanced mesh** components, from their transform buffers
- the **VFX system**, from mesh particles (see `vfx-system`)
- the **UI system**, from world-space and surface-space UI documents (see `ui-system`)

All producers SHALL use the same representation, so downstream culling, LOD, sorting, and drawing
require no knowledge of an instance's origin.

Instance publication SHALL be possible entirely GPU-side, without CPU round trips, for producers
whose data already lives on the GPU.

#### Scenario: One representation, many producers
- **WHEN** mesh particles, instanced meshes, world-space UI, and ordinary entities are all visible
- **THEN** they SHALL occupy the same GPU scene representation and be culled and drawn by the same
  passes

#### Scenario: GPU-side publication
- **WHEN** a producer's instance data is computed on the GPU
- **THEN** it SHALL publish into the GPU scene from a compute shader, with no readback and no CPU
  submission per instance

#### Scenario: Producer removed
- **WHEN** an effect, entity, or UI document is destroyed
- **THEN** its instances SHALL be removed from the GPU scene without requiring a full rebuild
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
