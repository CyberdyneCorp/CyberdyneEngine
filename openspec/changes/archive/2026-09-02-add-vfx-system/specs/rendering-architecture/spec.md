## ADDED Requirements

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

All producers SHALL use the same representation, so downstream culling, LOD, sorting, and drawing
require no knowledge of an instance's origin.

Instance publication SHALL be possible entirely GPU-side, without CPU round trips, for producers
whose data already lives on the GPU.

#### Scenario: One representation, many producers
- **WHEN** mesh particles, instanced meshes, and ordinary entities are all visible
- **THEN** they SHALL occupy the same GPU scene representation and be culled and drawn by the same
  passes

#### Scenario: GPU-side publication
- **WHEN** a producer's instance data is computed on the GPU
- **THEN** it SHALL publish into the GPU scene from a compute shader, with no readback and no CPU
  submission per instance

#### Scenario: Producer removed
- **WHEN** an effect or entity is destroyed
- **THEN** its instances SHALL be removed from the GPU scene without requiring a full rebuild

## MODIFIED Requirements

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

#### Scenario: Multiple views of one scene
- **WHEN** a main camera, a shadow-casting light, and a reflection probe all render the same
  scene
- **THEN** each SHALL cull and render independently against the shared instance set

#### Scenario: Split screen
- **WHEN** two views target different rects of one render target
- **THEN** both SHALL be rendered in the same frame with independent culling
