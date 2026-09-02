## MODIFIED Requirements

### Requirement: Scene, view, and instance model
The renderer SHALL model:

- **Scene** — a renderable world: a spatial index of instances, a light set, GI state, and an
  environment. Multiple scenes MAY exist simultaneously.
- **View** — a camera into a scene: transform, projection, viewport rect, render target,
  layer mask, quality settings, importance, a history identity, and post-process configuration. A
  view MAY have multiple **sub-views** for stereo or cubemap rendering.
- **Instance** — a placement of a renderable (mesh, VFX effect, decal, light, probe) into a
  scene, with a transform, bounds, layer mask, LOD parameters, visibility, and per-instance data.

Views SHALL be **produced from evaluated cameras** (see `camera-system`), which supply pose,
projection semantics, viewport, importance, and history identity. One evaluated camera MAY produce
several views. The renderer SHALL construct backend-specific projection matrices from the camera's
semantic projection, and temporal jitter SHALL be applied by the temporal framework rather than by
the camera.

Views SHALL be **first class and plural**. The main camera is one view among many: shadow views,
reflection probe captures, scene captures, editor viewports, minimaps, thumbnails, and each XR eye
are all views, rendered through the same path.

Views SHALL be organisable into **view families** that share work. A family SHALL declare what its
members share — the prepared GPU scene, culling results, shadow maps, acceleration structures,
history resources — so shared work is performed once rather than per view.

Each view SHALL carry its own budget allocation and importance, so a secondary view cannot consume
the frame budget of the primary one.

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

#### Scenario: The camera supplies the view
- **WHEN** a player camera is evaluated
- **THEN** it SHALL produce one or more views carrying its pose, projection semantics, importance
  and history identity, and the renderer SHALL not reach into camera rig state
