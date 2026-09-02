# rendering-architecture Specification

## Purpose

Defines the rendering server layer: the handle-based `RenderServer` API, how ECS data becomes a
render frame, the view/scene/instance model, the snapshot boundary between simulation and
rendering, and the frame structure that the concrete pipelines fill in.

## Requirements

### Requirement: Handle-based render server
`RenderServer` SHALL own all renderable state and expose it through generational handles, with
no knowledge of entities, nodes, or scripts.

Object families: textures, samplers, meshes, materials, shaders, skeletons, particle systems,
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
- **Instance** — a placement of a renderable (mesh, particle system, decal, light, probe) into a
  scene, with a transform, bounds, layer mask, LOD parameters, visibility, and per-instance data.

#### Scenario: Multiple views of one scene
- **WHEN** a main camera, a shadow-casting light, and a reflection probe all render the same
  scene
- **THEN** each SHALL cull and render independently against the shared instance set

#### Scenario: Split screen
- **WHEN** two views target different rects of one render target
- **THEN** both SHALL be rendered in the same frame with independent culling

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

The engine SHALL ship: **Forward+** (clustered, the default desktop pipeline), **Mobile**
(reduced feature set, tile-friendly), and **Null**. Projects SHALL be able to supply their own.

#### Scenario: Pipeline selection
- **WHEN** the configuration selects a pipeline and the device supports it
- **THEN** it SHALL be used; otherwise the engine SHALL fall back with a diagnostic

#### Scenario: Custom pipeline
- **WHEN** a project registers a custom pipeline
- **THEN** it SHALL receive the same snapshot and graph builder as the built-in ones, with no
  engine modification

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
